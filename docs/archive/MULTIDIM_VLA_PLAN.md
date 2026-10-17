# Multi-Dimensional VLA Plan (Pragmatic, Matrix-Math Focus)

Status: **Planned (revised 2026-10-16 after root-cause review)**
Primary use case: matrix math kernels such as `float A[n][M]`, `double B[n][M][K]`.

---

## 0. Executive Summary

The original plan assumed 2-D array *indexing* already worked and that only the
VLA declaration/size path needed the dimension product. **That premise is false.**

A minimal 2-D fixed-array program (`int a[2][3]`, fill, `return a[1][2] - a[0][0]`)
**segfaults in both JIT and AOT**, and the parser **silently drops every dimension
after the first** with no diagnostic. Multi-dim VLA therefore cannot be built until
multi-dimensional arrays in general work.

The work splits into two phases:

- **Phase 0** — Multi-dim arrays in general (parser + `NODE_INDEX` lowering). This is
  a prerequisite bug fix, not VLA-specific.
- **Phase 1** — Pragmatic multi-dim VLA on top of Phase 0 (single runtime outer
  dimension, constant inner dimensions).

---

## 1. Root-Cause Findings (Evidence)

### 1.1 Repro

```c
/* /tmp/t2d_fixed.c — segfaults (JIT and AOT) */
int main(void) {
    int a[2][3];
    a[0][0] = 1; a[0][1] = 2; a[0][2] = 3;
    a[1][0] = 4; a[1][1] = 5; a[1][2] = 6;
    return a[1][2] - a[0][0];
}
```

`./build/bin/zerocc --run /tmp/t2d_fixed.c` → SIGSEGV.
`./build/bin/zerocc /tmp/t2d_fixed.c -o bin && ./bin` → SIGSEGV (same bug, AOT).

### 1.2 Parser drops all but the first dimension

`src/zero_cc/parser/ast_stmt.h` (~line 800) consumes **exactly one** `[ ... ]` pair:

```c
if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
    c->token_pos++; // consume '['
    is_array = true;
    ... /* one dimension only */
}
```

For `int a[2][3]` the declaration is parsed as `int a[2]`; the leftover `[3]` is not
consumed (the `TK_SEMI` check at the end of the declaration fails) and falls through
to the "General Expression Statement" path, where it is **silently swallowed — no
diagnostic is emitted** (compile exits 0).

### 1.3 `NODE_INDEX` never decays to pointer

`src/zero_cc/parser/ast_lowering_expr.c` (`case NODE_INDEX`) always computes
`addr = base + index * elem_size` and then **loads the element value**. When the base
is a multi-dim array the element type is itself an array, so the correct semantics is
to return the *address* (pointer decay), but the lowering loads the 4/8-byte value
and uses that loaded garbage as the base pointer of the outer index.

Generated assembly for `a[0][0] = 1` (x86-64 AOT):

```asm
lea    -56(%rbp),%rbx      ; rbx = &a
add    %rdx,%rbx           ; inner index: addr = &a + 0*4
mov    (%rbx),%ebx         ; ← inner index LOADS the 4-byte "element" (uninit = 0)
add    %rdx,%rbx           ; outer index: base = loaded garbage (0)
mov    %eax,(%rbx)         ; store to address 0 → SIGSEGV
```

The 32-bit load into the same register is not itself a codegen bug (register
allocation legally aliases the dead address register); the IR is simply *semantically
wrong*: a value load where a pointer was required.

### 1.4 Consequences

- 2-D/3-D fixed arrays: silent miscompile (truncated to 1-D), wrong strides
  (both indices scaled by `sizeof(elem)` instead of the row stride), out-of-bounds
  stores that "work" only thanks to frame slack, and segfaults when the garbage
  base happens to be 0.
- The current 1-D hybrid VLA path (`lower_stmt_decl`) is unaffected, but its
  `count * elem_size` formula is only correct for 1-D.

---

## 2. Scope & Invariants (Phase 1)

| Item | Rule |
|------|------|
| Supported form | `T D0[expr0][D1][D2]…[Dk]` with **exactly one** runtime dimension (`expr0`, the outer one); all inner dimensions compile-time constant. |
| Out of scope | More than one runtime dimension (`int a[n][m]`) — needs runtime stride descriptors and a relaxed hybrid rule; deferred. |
| Memory model | Single contiguous block, row-major: `total_bytes = runtime_dim × Π(const inner dims) × sizeof(T)`, 16-byte aligned, one `OP_VLA_ALLOC`. |
| Hybrid rules | Unchanged: one runtime VLA per function, function scope only (`block_depth == 1`), not inside loops (`loop_depth == 0`). |
| Indexing | Row-major via the type chain: first index scales by row size (inner array), innermost scales by `sizeof(T)`. No IR changes beyond Phase 0. |

---

## 3. Phase 0 — Multi-Dimensional Arrays (Prerequisite)

### 3.1 Parser: consume all bracket dimensions

File: `src/zero_cc/parser/ast_stmt.h` (variable declaration path).

1. Replace the single-bracket block with a loop:

   ```c
   while (next token == TK_LBRACKET) {
       consume '[';
       parse dimension expr (or empty);
       consume ']';
       dim_exprs[dim_count++] = expr;     /* fixed: dim_node->int_val if NODE_INT >= 0 */
       if (!constant) has_runtime_dim = true;
   }
   ```

2. Build the **nested** type, innermost first:

   ```
   cur = elem_type (e.g. int)
   for dim in reverse(dim_exprs):
       cur = array(cur, dim_count_value_or_0)   /* 0 + is_vla for runtime dims */
   ```

   Use the existing `zero_cc_type_array(&elem, n)` / `type_array_of(..., n)` per
   level so both `ZeroCCType` and the semantic `TypeInfo` chain are nested.
   Set `is_vla` on the outer node if any dimension is runtime.

3. **Fail loudly**: if a trailing `[` remains after a complete declarator (the old
   silent-swallow path), emit a diagnostic instead of dropping tokens.

### 3.2 Type system / semantics: verify recursive size

- `type_size` / `type_align` on a `TypeInfo` whose kind is `TYPE_KIND_ARRAY` must be
  `count * type_size(elem)` (verify in `src/zero_cc/semantic/type.h`).
- `zero_cc_add_local` slot size must be the **full nested** size
  (`int a[2][3]` → 24-byte slot, not 8).
- Semantic analysis (`semantic/visitor.c`, `NODE_INDEX`) must annotate
  `node->aux.extra` with the *element* `TypeInfo` — for `int a[2][3]`, the element
  of `a` is `array[3] of int` (size 12), not `int`.

### 3.3 `NODE_INDEX` lowering: pointer decay for array elements

File: `src/zero_cc/parser/ast_lowering_expr.c`, `case NODE_INDEX`.

After computing `addr_temp = base + index * elem_size`:

```c
if (elem_ti && type_is_array(elem_ti)) {
    /* a[i] where a is T[d0][d1][...]: yields a pointer to the inner array.
     * Return the address; do NOT load. */
    c->last_expr_is_float = 0;
    return addr_temp;
}
/* existing scalar load/store path (LOAD_BYTE/16/32/DEREF/VLOADSS/VLOADSD) */
```

This is the actual segfault fix and makes chained indexing compose:
the outer `NODE_INDEX` receives the inner row pointer as `array_temp`.

### 3.4 Phase 0 verification (must pass before Phase 1)

| Test | Expectation |
|------|-------------|
| 2-D fixed `int a[2][3]` fill/read (the §1.1 repro) | Returns 5 (JIT + AOT). |
| 3-D fixed `int b[2][2][2]` with distinct values | Row-major offsets verified. |
| 2-D float `float f[2][3]` (VLOADSS path) | Correct values, no NaN/garbage. |
| Struct-element `struct P p[2][3]` (size ≠ 1/2/4/8) | Offset = `i*3*sizeof(P) + j*sizeof(P)`. |
| Existing 1-D tests (`test/jit/vla_test.c`, fixed-array tests) | No regression. |
| `zerocc -S` of the repro | Inner index emits **no load** (address arithmetic only). |

---

## 4. Phase 1 — Pragmatic Multi-Dim VLA

### 4.1 Declaration lowering

File: `src/zero_cc/parser/ast_lowering_stmt.c` (`lower_stmt_decl`, VLA branch).

With the Phase 0 nested type, the existing formula works if `elem_size` comes from
the **inner** array type:

```
count_temp  = lower_expr(runtime outer dim)
elem_size   = sym->type.base->size      /* = Π(const inner dims) × sizeof(T) */
size        = count_temp * elem_size    /* full flattened size */
aligned     = (size + 15) & ~15
ir_emit_vla_alloc(vla_ptr, aligned)
ir_emit_store(vla_ptr, fp, -sym->stack_offset)
```

Explicit guards to add:

- Exactly one runtime dimension (outer); more than one → diagnostic
  ("multi-dim VLA with more than one runtime dimension is not supported").
- Runtime dimension must be the **outer** (first) dimension; a runtime inner
  dimension would make the row stride non-constant → diagnostic for now.
- 64-bit intermediate arithmetic for the size product; diagnostic on overflow or
  unreasonable size.
- Keep `has_runtime_vla` / `block_depth` / `loop_depth` hybrid checks unchanged.

### 4.2 Indexing

No change needed — Phase 0 decay + `type_size(elem_ti)` scaling give
`addr = base + i*(Π inner dims × sizeof T) + j*sizeof T` automatically.

### 4.3 Phase 1 verification

| Check | Tool / command |
|-------|----------------|
| Single `OP_VLA_ALLOC`, correct `imm64` | `python3 tools/vla/ir_diff.py --dump-ir ./build/bin/zerocc <test>` |
| Stack balance | `python3 tools/vla/vla_stack_track.py --asm test.s --verify` |
| Hybrid rules | `python3 tools/vla/verify_opt.py --asm test.s --verify-hybrid` |
| Patterns/ABI | `python3 tools/vla/check_asm.py test.s --check-vla` |
| VLA suite + new cases | `python3 tools/vla/test_vla.py --all --compiler ./build/bin/zerocc` (add `multi_dim_vla_2d`, `multi_dim_vla_3d`) |
| Matrix kernel | `float matmul(int n, float A[n][M], float B[n][M], float C[n][M])` → compare vs scalar reference in test |
| Direct compiler | `./build/bin/zerocc <test>.c -o /tmp/b && /tmp/b` **and** `./build/bin/zerocc --run <test>.c` |

---

## 5. Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| Parser change breaks existing 1-D array/VLA parsing | Loop consumes zero-or-more brackets; single-bracket cases take the exact same path as today. Full `make test` after each phase. |
| Silent-swallow removal surfaces previously "working" (miscompiled) code | That is the point — emit a diagnostic. Update any in-tree test that accidentally relied on truncated multi-dim declarations. |
| `type_size` not recursive on nested arrays | Phase 0 §3.2 adds a unit test before wiring the parser. |
| Decay change alters codegen for compound literals / `a[i].f` (member after index) | `NODE_MEMBER`'s general branch already handles a pointer base (`lower_expr` of base + `OP_DEREF`); cover with a `struct` 2-D test in Phase 0 §3.4. |
| Size overflow in VLA allocation | 64-bit intermediates + size sanity diagnostic. |
| Future need for `int a[n][m]` (two runtime dims) | Deferred; requires storing dims as temps, runtime stride, and relaxed hybrid rules. Documented as out of scope. |

---

## 6. Timeline Estimate

| Phase | Work | Estimate |
|-------|------|----------|
| 0 | Parser multi-bracket + nested types + decay lowering + diagnostics | 2–3 days |
| 0 | Type/semantic size recursion verification + regression tests | 1 day |
| 1 | VLA declaration guards + product size + test cases | 1 day |
| 1 | Verification (JIT/AOT, `tools/vla/*`, matmul kernel) + OKF update | 1 day |
| — | Buffer for parser regressions | 1 day |
| **Total** | | **~6 working days** |

---

## 7. Related Documents

- `docs/okf/frontend-and-types.md` — dual type representation; **Known Defect: 2-D
  arrays** section records the Phase 0 bugs.
- `docs/VLA_BUG_ANALYSIS.md`, `docs/VLA_ARRAY_ACCESS_FIX.md`,
  `docs/MEM2REG_VLA_SAFE_IMPLEMENTATION.md` — prior 1-D VLA history.
- `docs/ZEROCC_Development_Plan.md` — lists "Multi-dim VLA: Only 1D VLAs supported"
  as a known limitation (remains true until Phase 1 lands).
- `tools/vla/README.md` — hybrid VLA rules and verification workflow.
