# ZeroCC Fragility Analysis

Date: 2026-07-09 · Method: static pattern scan + boundary bisecting with 20+ generated probes, each cross-checked against host `gcc` (JIT `--run` and AOT `-o` modes).

Legend: **CRIT** = silently wrong code or compiler crash on valid C · **HIGH** = compiler crash/hang on valid C · **MED** = silent limit with no diagnostic · **LOW** = documented constraint.

---

## CRITICAL

### C1. 32-bit `int` overflow does not wrap (64-bit residue leaks into `int` values)

`int a=2000000000; int b=a+a; return b==-294967296?0:1;` → gcc 0, zerocc 1.

Integer arithmetic is performed in 64-bit registers and 32-bit `int` results are never truncated. Reloading a local `int` from its stack slot also lacks sign-extension (unsigned reloads zero-extend, which is why the unsigned variant passes). Every `int` computation whose true value exceeds the 31-bit range is silently wrong: comparisons, shifts, stores, return values.

- Probe: `p12a` (fails), `p12b` (unsigned, passes), `p12c` (in-range, passes).
- Fix direction: truncate to 32 bits when storing/reloading 32-bit signed int locals (sign-extend on load), or emit 32-bit ALU ops for `int`-typed values.

### C2. Temp-id exhaustion at 255 → silent miscompilation — **C2a applied, C2b pending**

`Instr` temp fields are `uint8_t` with `0xFF` = "no temp". `next_temp_id` is an unbounded `int` that truncates on store: temp id 255 aliases the 0xFF sentinel (LRA skips it, `regalloc_get_reg_for_temp` returns the default register), and 256+ wrap into 0–15, colliding with the real VREG_0–15 (return/arg registers).

- **Refined diagnosis (Sprint 23)**: this is *silent miscompilation*, not a compiler crash. The compiler finishes (even `-S` succeeds) and the **generated program** segfaults (AOT) or infinite-loops (JIT). A bare `-dump-ir` also "hangs" because `compile_jit` always calls `zero_cc_execute_jit` — it runs the code even without `--run`.
- Boundary: 116 nested calls = 240 temps (ids 16–255, first id to hit the sentinel) → OK; 117 → broken.
- The LRA is **not** the problem: nested-call results are only live across one outer call (~2–4 temps simultaneously live), so its 2-register/no-spill allocation is correct.
- **C2a (applied, `ast_lowering.c`)**: after `lower_node`, if the function used ≥240 temps, a clean fatal diagnostic: `function 'main' uses 241 temporaries; max is 239 per function (8-bit temp ids, 0xFF = no-temp sentinel). Split the function into smaller helper functions ...`. Verified: 116 OK / 117 clean error (rc=1, no binary) in both modes; temp ids never reset per statement, so splitting *expressions* does not help — splitting into *helper functions* does (verified: 120 nested calls via a 4-call helper passes all modes).
- **C2b (pending, the real fix)**: widen `Instr.temp_dest/src1/src2` to `uint16_t` (packed struct 19→25 B, in-memory only), sentinel `0xFF`→`0xFFFF` via a `TEMP_NONE` macro. Blast radius measured: 152 sentinel sites across 16 files (516 field refs, most read-only); LRA/arena are already `realloc`/`sizeof(Instr)`-based. Raises the bound to 65535 temps/function.
- Consequence while C2b is pending: the C3 (>128 cases) and C4 (~231 locals) boundaries are now *masked* — 129 cases ≈ 325 temps and 231 locals ≈ 234+ temps both trip the C2a guard first. Re-measure after C2b.

### C3. Switch with >128 cases silently drops the rest

`ast_lowering_stmt.c:493`: `while (s != AST_NULL && n_cases < 128)` — cases 129+ never get labels, no diagnostic.

- 300-case switch: JIT **hangs** on out-of-range value; AOT binary **segfaults** (rc=139). gcc: both fine.
- **Refined (Sprint 23)**: the 119/120 "switch" boundary measured earlier was C2 in disguise (~2.5 temps per case dispatch; 100 cases already reach temp 255). With C2a's guard, ~95+ cases already produce a clean temp-id error, so the true >128-case cap cannot be exercised until C2b lands. Re-measure after C2b.
- Fix direction: emit all cases (dispatch is O(n) CMP/JNE already) or a diagnostic.

### C4. ~231 locals in one function → compiler hang/crash

Bisect: 230 `int` locals → OK; 232 → **hang** (rc=124), at 400+ also segfault (rc=139, ASLR-dependent). Correlates with **symbol count**, not frame size (250 `char` locals hang too). `ZERO_CC_MAX_LOCALS` is 8192, so a hidden smaller structure saturates first (scope lookup table or O(n²) pass suspected). No diagnostic. Root cause not yet pinned.

- **Refined (Sprint 23)**: each `int l_i=0;` declaration also consumes a temp, so 231+ locals now trip the C2a temp guard *first* (clean error instead of hang). The underlying C4 mechanism is still unmeasured; re-bisect after C2b with a temp-cheap probe.

### C5. Member chains through pointer members fail to resolve

`b.p->v` (local struct) and `b->p->v` (param) both → `error: member reference base is not a structure or union`, even for the minimal 2-segment chain. Multi-level `->`/`.` chains (lists, trees) are unusable; single-segment access works.

---

## HIGH

### H1. ~200 nested expression depth → parser stack overflow (segfault/hang at compile time)

**Refined (Sprint 23)**: the original ~115 boundary was C2 in disguise (temp exhaustion at ~118-deep `+0` chains). The *real* H1 is a **parser stack overflow**: recursive-descent expression parsing uses ~18 stack frames per nesting level, so the default 8 MB main-thread stack exhausts near depth 200 (backtrace: 3600+ frames of `parse_primary_ast` → `parse_assignment_ast` cycles; non-deterministic segfault/hang depending where the overflow lands). Verified: with `ulimit -s 65536`, depth-200 parses fine and reaches the C2a temp guard. Pre-existing; independent of C2. gcc handles 120 (and far more) trivially.

### H2. Diagnostics do not stop compilation

Errors such as `undefined local variable`, `failed to insert symbol`, `use of undeclared identifier` are printed, but compilation **continues and produces a binary** (often with wrong semantics, rc=0). Broken builds pass silently.

---

## MEDIUM (silent limits, no diagnostics)

| # | Limit | Behavior past limit | Probe |
|---|-------|--------------------|-------|
| M1 | identifiers ≤ **127** chars | name truncated → **collision**, silently wrong code (error diagnostics fire on the split token) | `id.c`: 127 OK, 128 fails |
| M2 | globals ≤ **256** | 257th silently dropped (`add_global_var` returns -1, ignored) | `p3` 260 globals "passes" (only checked first 13) |
| M3 | struct members ≤ **64** | 65th silently dropped (bounds-checked array, no diagnostic) | `p4` |
| M4 | IR dump shows `imm` (int16) not `imm64` | large immediates displayed truncated (e.g. `2000000000` shown as `-27648`); **emission is correct** (uses `imm64`) — misleading for debugging only | `p11` IR dump |

---

## LOW (documented / accepted)

- **WSL2 GPF**: any 128-bit SIMD read from a `PROT_READ|PROT_EXEC` page faults on kernel 6.18.54 WSL2 (see `docs/okf/scalar-float-x86.md`). 32/64-bit reads fine.
- **`-S` path** lacks `.data` section emission (AOT `-o` fine).
- Computed-size VLA, flexible-array `offsetof` (C99 audit leftovers).
- `>>` lowers to logical shift (implementation-defined for signed).

---

## Robustness positives (verified during this analysis)

- Global integer constants up to `INT_MAX` are exact (JIT + AOT).
- Struct-member width narrowing (1/2/4-byte) works for local and global bases.
- `members[64]` is bounds-checked — no OOB write from oversized structs.
- Long string literals (600B), 5000-element VLA, 1000-deep runtime recursion, `long long` INT64 boundaries, char/short sign boundaries — all correct.
- Reloc/label tables have generous caps (32K); LRA arrays grow dynamically.
- `make test` 140/140 and the C99 probe battery (30/30) still pass.

## Suggested fix order

1. **C1** int32 wrap/extension — largest semantic gap, affects real-world code (hashes, coordinates, bit math).
2. **C2** ~~temp-id bound check + diagnostic (crash → error)~~ — **C2a done (Sprint 23)**; remaining: C2b 16-bit temp-id widening.
3. **C3** switch case cap (remove or diagnose).
4. **C4** root-cause the ~231-local hang.
5. **C5** multi-segment member chain type resolution.
6. **H2** make diagnostics fatal by default.
7. **M1** error (not truncate) on identifiers >127.
8. **H1** raise parser stack or iterate the expression recursion.
