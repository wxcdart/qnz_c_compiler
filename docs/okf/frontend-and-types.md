---
type: Concept
title: Frontend, Type System & Semantic Analysis
description: Dual type representation (ZeroCCType and TypeInfo), struct/union layout, function pointer declarations, and semantic checking rules.
resource: frontend/types-and-semantics
tags: [frontend, parser, types, semantic, function-pointers, c11]
timestamp: 2026-10-17T23:46:19Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Frontend, Type System & Semantic Analysis

ZeroCC synchronizes its parser/lowering type representation (`ZeroCCType`) and semantic analyzer type representation (`TypeInfo`) through a single canonical bridge in `src/zero_cc/jit/type_info.h`:

1. **`ZeroCCType` (`src/zero_cc/types/type_system.h`)**:
   - Used by the parser, symbol tables (`ZeroCCSym`, `ZeroCCFunction`), and AST-to-IR lowering.
   - Tracks `tag` (`TYPE_INT32`, `TYPE_PTR`, `TYPE_STRUCT`, `TYPE_UNION`, `TYPE_ARRAY`, etc.), `size`, `align`, `base` pointer, `members` array, qualifiers (`is_const`, `is_volatile`, `is_restrict`, `is_atomic`), and `is_func_ptr`.
2. **`TypeInfo` (`src/zero_cc/semantic/type.h`, bridged via `src/zero_cc/jit/type_info.h`)**:
   - Used by the semantic analysis pass (`src/zero_cc/semantic/`) to validate expressions and annotate `ZeroCCASTNode`.
3. **Canonical Type Bridge & AST Accessors (`src/zero_cc/jit/type_info.h`)**:
   - `zero_cc_resolve_base_type(c, type_tok, type_name)`: Single source of truth for resolving base declaration, parameter, and return types (primitives, structs, unions, enums, and typedefs).
   - `zero_cc_type_to_typeinfo(perm, t)` and `typeinfo_to_zero_cc_type(ti)`: Bidirectional conversion between `ZeroCCType` and `TypeInfo *`, preserving pointers, function pointers (`is_func_ptr`), multi-dimensional arrays/VLAs, structs/unions, enums, and qualifiers (`const`, `volatile`, `restrict`, `_Atomic`). The parser constructs `ZeroCCType` once per declarator and derives `TypeInfo *` via `zero_cc_type_to_typeinfo()` rather than building parallel type trees.
   - `ast_node_set_typeinfo(c, node, ti)` and `ast_node_get_typeinfo(c, node)`: Canonical accessors for attaching and querying semantic `TypeInfo *` on `ZeroCCASTNode.aux.extra` across parser (`ast_expr.h`), semantic analyzer (`visitor.c`), and IR lowering (`ast_lowering_expr.c`).

## Signedness & Narrow-Int Correctness (C1)

All C int types carry an explicit signedness tag end-to-end, and every narrow-int value is re-extended to 64-bit double-word at every value boundary:

- **Tags**: `ZeroCCType.tag` uses `TYPE_UINT8/16/32/64` (not the signed variants) for `unsigned char/short/int/long`; `TypeInfo` uses `TYPE_MOD_UNSIGNED` on `TYPE_KIND_CHAR/SHORT/INT/LONG`. Both directions of the bridge preserve it: `zero_cc_type_to_typeinfo` maps `TYPE_UINT8→type_uchar` etc., and `typeinfo_to_zero_cc_type` maps back via the `TYPE_MOD_UNSIGNED` modifier.
- **Parser signedness consumption**: a leading `signed`/`unsigned` token is consumed before the base specifier in every declaration path — locals/declarations (`ast_stmt.h`, `decl_is_unsigned`), function parameters (**two** parser paths: `ast_stmt.h` and the legacy `jit/compile.h`), function return types (`jit/compile.h`), struct members (`definitions.h`, all six tag sites use `is_unsigned ? TYPE_UINTn : TYPE_INTn`), file-scope globals (`compile_unit.h` via `zero_cc_parse_type_name`), and cast/initializer type names (`zero_cc_parse_type_name_at_pos` — `unsigned char` must consume the `char` token). `zero_cc_resolve_base_type` always returns the **signed** variant; callers apply the `UINTn` remap.
- **Lowering width extension**: `ir/ast_lowering_expr.c` marks integer results with `ir_mark_last_width`/`zero_cc_operand_int_width` (wider operand wins; equal width → unsigned wins; comparisons unmarked). Extension is emitted in-place as `OP_SEXT8/16/32`/`OP_ZEXT8/16/32` at: local/global loads, struct-member loads (replacing the old AND/xor/sub narrowing), call arguments (per callee `param_types[i]`), scalar returns, int↔int and float→int casts, and `NODE_BINOP`/`NODE_UNOP(~)` results. Signed right shifts of signed left operands lower to `OP_SAR`, not logical `SHR`.
- **`int` wraparound**: 32-bit `int` arithmetic truncates to 32 bits before further use (`SEXT32`/`ZEXT32` on the result), so `int a=2000000000; a+a` wraps to `-294967296` like gcc.

## Function Pointer Representation (`is_func_ptr`)

- When parsing function pointer typedefs (`typedef RetType (*Alias)(params...);` in `src/zero_cc/parser/definitions.h`) or inline local function pointer declarations (`RetType (*fn)(params...) = init;` in `src/zero_cc/parser/ast_stmt.h`):
  - The symbol's `ZeroCCType` has `tag = TYPE_PTR`, `size = 8`, `align = 8`, `is_func_ptr = true`, and `base` pointing to a heap/arena-allocated `ZeroCCType` representing the **return type** (`RetType`).
- Function and prototype `NODE_DECL` nodes attach a proper `TYPE_KIND_FUNCTION` `TypeInfo *` via `decl_node_set_type(ctx, func_node, fn_ti)`.
- In `zero_cc_type_to_typeinfo()` (`src/zero_cc/jit/type_info.h`):
  - If `zt.tag == TYPE_PTR && zt.is_func_ptr`, it constructs `TYPE_KIND_POINTER` → `TYPE_KIND_FUNCTION` whose `.details.function.ret` preserves the return `TypeInfo` (including `TYPE_STRUCT` with its full byte size).
- In `sem_analyze_node()` (`src/zero_cc/semantic/visitor.c`):
  - `case NODE_CALL:` inspects the callee's `TypeInfo *fn_t` (supporting both direct `type_is_function(fn_t)` and indirect `type_is_pointer(fn_t)` to function) and annotates the `NODE_CALL` AST node with `res` via `expr_node_set_type(ctx, node, res)` without rejecting unprototyped/enum conversions.

## Struct & Union Layout, Nested Initializers & Declarator Rules

- Structs and unions store field metadata in `type.members[i]` (`name`, `offset`, `size`, `bit_width`, `bit_offset`, `is_flexible`).
- **Floating-Point Member Tags**: `zero_cc_parse_struct_body` (`src/zero_cc/parser/definitions.h`) assigns `TYPE_FLOAT` for `TK_FLOAT_KW` and `TYPE_DOUBLE` for `TK_DOUBLE_KW` members (all six tag-assignment sites — union, bitfield, regular, and comma-continuation declarators). Do **not** collapse them to `TYPE_INT32`/`TYPE_INT64`: member floatness detection (global/local struct member loads/stores in `ast_lowering_expr.c`, file-scope aggregate init in `compile_unit.h`) relies on these tags. Sizes/alignment still come from the token-based `base_size`/`base_align` tables, so layout is unaffected.
- **`NODE_MEMBER` Payload Gotcha**: `ZeroCCASTNode` is a 32-byte node whose 8-byte payload union aliases `s.{str,len}` and `aux.{op,alt,step,extra}`. The parser stores the member name in `s.str`, but semantic analysis later writes `aux.alt` (member offset) into the **same bytes** on `NODE_MEMBER`/`NODE_ARROW` nodes. Therefore lowering code must read member identity from `aux.alt` (offset) or the semantic `TypeInfo` — never from `s.str` on member nodes after semantic analysis (garbage pointer → SIGSEGV).
- **Fixed-Size Array Members & Nested Brace Initializers**:
  - Struct members with `[N]` array dimensions (`elem_count > 1`) set `m->tag = TYPE_ARRAY` with `m->base` pointing to the element `ZeroCCType` (`src/zero_cc/parser/definitions.h`), which `zero_cc_type_to_typeinfo` converts into `TYPE_KIND_ARRAY` (`src/zero_cc/jit/type_info.h`).
  - `NODE_MEMBER` in `src/zero_cc/ir/ast_lowering_expr.c` emits `ir_emit_lea` (address decay) when accessing an array member (`b.arr[i]`).
  - `parse_brace_init_list_ast` (`src/zero_cc/parser/ast_expr.h`) recursively flattens nested `{ ... }` initializer lists, and `struct_resolve_scalar_leaf` (`src/zero_cc/ir/ast_lowering_expr.c`) maps positional scalar initializers to the `idx`-th scalar leaf across nested struct and array members.
- **Multi-Level Pointers, Opaque Struct Typedefs & Multi-Declarator `extern`**:
  - Function parameters, local declarations, comma-separated declarators, and return types count arbitrary pointer indirection depth (`ptr_depth` for `int **mat`, `void ***p`, etc.) in `src/zero_cc/jit/compile.h` and `src/zero_cc/parser/ast_stmt.h`.
  - `zero_cc_stmt_typedef_struct` (`src/zero_cc/parser/definitions.h`) supports opaque forward struct typedefs (`typedef struct FILE FILE;`) and copies the alias name into `st.struct_name` for anonymous `typedef struct { ... } Alias;`.
  - File-scope `extern` declarations (`src/zero_cc/jit/compile_unit.h`) support comma-separated declarators (`extern FILE *stdin, *stdout, *stderr;`) and register `is_external` symbols as `SYMBOL_VAR` in `global_scope_root`.
  - `__builtin_va_arg(ap, Type)` (`src/zero_cc/parser/ast_expr.h`, `src/zero_cc/semantic/expr.c`) parses arbitrary type names (including typedefs and structs) via `zero_cc_parse_type_name(c)`.
- `#pragma pack(push, N)` and `#pragma pack(pop)` adjust field alignment and total struct padding (`src/zero_cc/core/pragma_state.c`).
- Local variables allocated via `zero_cc_add_local()` (`src/zero_cc/types/symbol_mgmt.h`) align `c->current_stack_frame` to `max(type.align, 8)` and increment by `alloc_sz = max(type.size, 8)`.
  - **Important Rule**: Do **not** round `alloc_sz` up to a multiple of 8 for all structs in `zero_cc_add_local`, as compound literal and designated initializer offset math in `test/jit/designated_init_test.c` relies on exact `sym->stack_offset` placement. Instead, use size-aware tail chunk loads/stores (`32`/`16`/`8`-bit) when copying structs whose size is not a multiple of 8.

## Multi-Dimensional Arrays & Matrix VLAs (Phase 0 & Phase 1 Supported)

Multi-dimensional fixed arrays (e.g. `int a[2][3]`, `b[2][2][2]`) and matrix-shaped VLAs (e.g. `int a[n][M]` where dimension 0 is runtime-sized and inner dimensions are compile-time constants) are fully supported across AOT and JIT:
- **Parser** (`src/zero_cc/parser/ast_stmt.h`): variable declarations parse all consecutive `[ ... ]` dimensions (up to `MAX_ARRAY_DIMS = 16`), constructing a nested chain of `ZeroCCType` and `TypeInfo` (`array[d0] of array[d1] of … of T`). For multi-dimensional VLAs, dimension 0 may be runtime-evaluated while inner dimensions must be compile-time constants. A mandatory `;` check diagnoses any trailing syntax tokens.
- **`NODE_INDEX` lowering & pointer decay** (`src/zero_cc/ir/ast_lowering_expr.c`): when an indexing expression indexes an array whose element type is itself an array (e.g. `a[i]` in `a[i][j]`) or a struct/union (`p[i][j]` in `p[i][j].x`), `NODE_INDEX` decays to the element's base address (`addr_temp`) without emitting intermediate load instructions (`LOAD_32`, `VLOADSS`, etc.). Chained indexing multiplies outer dimensions by the full row stride (`dim1 * ... * dimK * sizeof(elem)`).
- **Member assignment on index lvalues** (`src/zero_cc/ir/ast_lowering_expr.c`): `NODE_ASSIGN` with `NODE_MEMBER` LHS supports non-identifier base expressions (`p[i][j].x = val`), storing directly through the evaluated address pointer rather than assuming a stack-relative offset.
- **Auto-Vectorization Invariant** (`src/zero_cc/opt/opt_vectorize.c`): the loop vectorizer restricts analysis to innermost loops (loops with no inner backedges) to avoid misclassifying reduction sums or multi-loop matrix kernels as parallel SIMD loops without horizontal reduction.

## Goto, User Labels & Switch (Implemented)

- **`goto label;` and `label: stmt`** (`src/zero_cc/parser/ast_stmt.h`): the statement parser handles `TK_GOTO` (`NODE_GOTO`, name in `s.str`) and a statement-leading `IDENT` immediately followed by `TK_COLON` (`NODE_LABEL`, name in `s.str`, `left` = the labeled statement). Lowering (`lower_stmt_goto`/`lower_stmt_label` in `src/zero_cc/ir/ast_lowering_stmt.c`) maps user names to IR labels through `label_patcher_find_or_create_user_label` + `get_or_create_user_ir_label` (names persist in `c->label_patcher.user_labels`; labels are created lazily on first goto/label, supporting forward references).
- **`switch` dispatch** (`lower_stmt_switch`, `src/zero_cc/ir/ast_lowering_stmt.c`): evaluates the controlling expression, pre-scans the body block's top-level statement chain for `NODE_CASE`/`NODE_DEFAULT`, emits a `CMP`/`COND_SET`/`JNE` dispatch chain per case (jump-if-**nonzero** — see the conditional-branch note in [isa-backends.md](./isa-backends.md)), stores each case's IR label in `node->aux.alt`, and registers the switch exit as the current break target via `label_patcher_enter_loop(..., switch_exit, 0)` so `break` works. A non-matching value skips the body entirely (unconditional jump past the body, per C semantics); fallthrough between case labels is natural code order. `NODE_CASE`/`NODE_DEFAULT` lowering emits their stored label (when set by a switch) and lowers the attached statement.
- **Known limitation**: `case` constants are lowered as runtime expressions (no compile-time deduplication); deeply nested `continue` inside a bare switch (no loop) is a no-op, matching prior behavior.

## C11 Core Keywords, Alignment, Atomics & Threading

ZeroCC supports C11 core language features and standard concurrency primitives verified by `tools/c11/test_c11.py`:
- **Alignment (`_Alignas`, `_Alignof`)**:
  - `_Alignof(type)` evaluates compile-time natural alignment in `src/zero_cc/parser/ast_expr.h`.
  - `_Alignas(N)` and `_Alignas(type)` apply explicit alignment overrides to struct fields (`src/zero_cc/parser/definitions.h`) and local stack variables (`src/zero_cc/parser/ast_stmt.h`, `src/zero_cc/types/symbol_mgmt.h`).
- **Anonymous Structs & Unions**:
  - Unnamed nested `struct { ... };` and `union { ... };` members inside enclosing structs/unions flatten their inner fields into the parent's member table with accumulated byte offsets (`src/zero_cc/parser/definitions.h`).
- **Generic Selection (`_Generic`) & Static Assertions (`_Static_assert`)**:
  - `_Generic(controlling-expr, type: expr, default: expr)` resolves the matching association branch at compile time based on the controlling expression's static type (`src/zero_cc/parser/ast_expr.h`).
  - `_Static_assert(const-expr, "msg")` validates compile-time constant invariants at both file and block scope.
- **C11 Atomics & `<threads.h>`**:
  - `_Atomic` qualifiers and `__sync_*` / `__atomic_*` builtins lower to hardware atomic IR opcodes (`OP_ATOMIC_XADD`, `OP_ATOMIC_CMPXCHG`, `OP_ATOMIC_XCHG`, `OP_MFENCE`).
  - `include/threads.h` maps C11 thread (`thrd_t`, `thrd_create`, `thrd_join`) and mutex (`mtx_t`, `mtx_init`, `mtx_lock`, `mtx_unlock`, `mtx_destroy`) primitives to POSIX threads (`-lpthread`).
  - Global variables (`src/zero_cc/types/global_vars.h`, instantiated in `src/zero_cc/ir/ir.c`) emit `ELF_STT_OBJECT` symbols in `.data` and support address-of (`&g`), scalar value loads, assignments, and increments (`g++`).

## Related Concepts
- Return to [Root Index](./index.md)
- See [Compilation Pipeline & Two-Pass Architecture](./architecture-pipeline.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)

