---
type: Concept
title: Frontend, Type System & Semantic Analysis
description: Dual type representation (ZeroCCType and TypeInfo), struct/union layout, function pointer declarations, and semantic checking rules.
resource: frontend/types-and-semantics
tags: [frontend, parser, types, semantic, function-pointers, c11]
timestamp: 2026-10-22T15:24:02Z
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
   - **Scope Symbol Table (`src/zero_cc/semantic/scope.c`)**: Uses a dynamically resizing separate-chaining hash table with power-of-two bucket count (starts at 64 buckets, doubles when load factor exceeds 75%). Symbols and duplicated names are allocated in the compilation permanent arena (`perm_arena`), while bucket pointers are freed on `scope_leave()`. This completely replaced the fixed-capacity TPHT (Two-Phase Hash Table) in the semantic analyzer, removing the fixed ~3,000 / ~8,000 symbols-per-scope cap (M5) and eliminating TPHT collision insertion failures.
3. **Canonical Type Bridge & AST Accessors (`src/zero_cc/jit/type_info.h`)**:
   - `zero_cc_resolve_base_type(c, type_tok, type_name)`: Single source of truth for resolving base declaration, parameter, and return types (primitives, structs, unions, enums, and typedefs).
   - `zero_cc_type_to_typeinfo(perm, t)` and `typeinfo_to_zero_cc_type(ti)`: Bidirectional conversion between `ZeroCCType` and `TypeInfo *`, preserving pointers, function pointers (`is_func_ptr`), multi-dimensional arrays/VLAs, structs/unions, enums, and qualifiers (`const`, `volatile`, `restrict`, `_Atomic`). The parser constructs `ZeroCCType` once per declarator and derives `TypeInfo *` via `zero_cc_type_to_typeinfo()` rather than building parallel type trees.
   - `ast_node_set_typeinfo(c, node, ti)` and `ast_node_get_typeinfo(c, node)`: Canonical accessors for attaching and querying semantic `TypeInfo *` on `ZeroCCASTNode.aux.extra` across parser (`ast_expr.h`), semantic analyzer (`visitor.c`), and IR lowering (`ast_lowering_expr.c`).

## SemContext: Single Canonical Definition

`SemContext` (the semantic-pass state: arenas, `diags`, current scope, label table) is defined **only** in `src/zero_cc/semantic/visitor.h`. `decl.h` and `expr.h` used to each carry a stale 8-field copy behind a shared `SEM_CONTEXT_DEFINED` first-include-wins guard, so any TU that included `decl.h`/`expr.h` before `visitor.h` (the JIT pipeline headers `compile.h`/`compile_unit.h`) compiled a **smaller** struct than `visitor.c` wrote into — a cross-TU ABI mismatch whose overflow was dormant until a stack-layout change moved a live variable into the spilled bytes (found by the Sprint 42 multi-error-recovery work; ASan stack-buffer-overflow in `sem_context_init`). The other two headers now `#include "zero_cc/semantic/visitor.h"`. **Invariant:** never redeclare `SemContext` (or any cross-TU pipeline struct) in a second header — one definition, included everywhere.

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
  - `case NODE_CALL:` inspects the callee's `TypeInfo *fn_t` (supporting both direct `type_is_function(fn_t)` and indirect `type_is_pointer(fn_t)` to function) and annotates the `NODE_CALL` AST node with `res` via `expr_node_set_type(ctx, node, res)`. It additionally enforces the C99 6.7.5.3p6 argument-count constraint **only when the callee type is a prototype** (`fn_t->details.function.is_prototype`); variadic prototypes require at least the named parameters, and non-prototype (empty-paren / implicit) function types are unconstrained. Unprototyped/enum conversions remain accepted.

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
- **Multi-Segment Member Chains & Pointer Dereference Stores (C5)**:
  - Multi-segment member chains (`chain_depth >= 2` such as `o.p->v`, `a.b.c`, `p->a->b`, `(*pp)->w`) resolve via `lower_member_chain_addr` in `src/zero_cc/ir/ast_lowering_expr.c`. Instead of a flat stack-offset sum that cannot dereference intermediate pointers, lowering recursively computes the pointer/struct base and traverses member offsets and pointer dereferences.
  - Assignment through pointers (`*ptr = val` in `NODE_UNOP`) and member stores inspect the target `TypeInfo` size and emit width-specific stores (`store_byte`, `store_16`, `store_32`, `store_64`, `vstoress`, `vstoresd`) to avoid clobbering adjacent struct fields.
  - Compound assignment on member expressions (`o.p->f += 0.5f`) preserves and loads float LHS values via `vloadss`/`vloadsd`, converts integer RHS if necessary, and dispatches float arithmetic opcodes before storing.
- `#pragma pack(push, N)` and `#pragma pack(pop)` adjust field alignment and total struct padding (`src/zero_cc/core/pragma_state.c`).
- Local variables allocated via `zero_cc_add_local()` (`src/zero_cc/types/symbol_mgmt.h`) align `c->current_stack_frame` to `max(type.align, 8)` and increment by `alloc_sz = max(type.size, 8)`.
  - **Important Rule**: Do **not** round `alloc_sz` up to a multiple of 8 for all structs in `zero_cc_add_local`, as compound literal and designated initializer offset math in `test/jit/designated_init_test.c` relies on exact `sym->stack_offset` placement. Instead, use size-aware tail chunk loads/stores (`32`/`16`/`8`-bit) when copying structs whose size is not a multiple of 8.

## Multi-Dimensional Arrays & Matrix VLAs (Phase 0 & Phase 1 Supported)

Multi-dimensional fixed arrays (e.g. `int a[2][3]`, `b[2][2][2]`) and matrix-shaped VLAs (e.g. `int a[n][M]` where dimension 0 is runtime-sized and inner dimensions are compile-time constants) are fully supported across AOT and JIT:
- **Parser** (`src/zero_cc/parser/ast_stmt.h`): variable declarations parse all consecutive `[ ... ]` dimensions (up to `MAX_ARRAY_DIMS = 16`), constructing a nested chain of `ZeroCCType` and `TypeInfo` (`array[d0] of array[d1] of … of T`). For multi-dimensional VLAs, dimension 0 may be runtime-evaluated while inner dimensions must be compile-time constants. A mandatory `;` check diagnoses any trailing syntax tokens.
- **`NODE_INDEX` lowering & pointer decay** (`src/zero_cc/ir/ast_lowering_expr.c`): when an indexing expression indexes an array whose element type is itself an array (e.g. `a[i]` in `a[i][j]`) or a struct/union (`p[i][j]` in `p[i][j].x`), `NODE_INDEX` decays to the element's base address (`addr_temp`) without emitting intermediate load instructions (`LOAD_32`, `VLOADSS`, etc.). Chained indexing multiplies outer dimensions by the full row stride (`dim1 * ... * dimK * sizeof(elem)`).
- **Member assignment on index lvalues** (`src/zero_cc/ir/ast_lowering_expr.c`): `NODE_ASSIGN` with `NODE_MEMBER` LHS supports non-identifier base expressions (`p[i][j].x = val`), storing directly through the evaluated address pointer rather than assuming a stack-relative offset.
- **Auto-Vectorization Invariant** (`src/zero_cc/opt/opt_vectorize.c`): the loop vectorizer restricts analysis to innermost loops (loops with no inner backedges) to avoid misclassifying reduction sums or multi-loop matrix kernels as parallel SIMD loops without horizontal reduction.

## Array Parameters, N-D Initializers & Declarator Completeness (Sprint 40)

- **Array parameter adjustment** (C99 6.7.6.3p7, `src/zero_cc/jit/compile.h`): a parameter declared `T x[...]` has its OUTERMOST array dimension adjusted to a pointer; remaining dimensions form the pointed-to array type — `int f(int x[])` → `int *x`, `int f(int m[][4])` → `int (*)[4]` (a `TYPE_PTR` whose base is `TYPE_ARRAY(int, 4)`). Pre-Sprint 40 the `[...]` suffixes were skipped and the parameter kept its element type, so the pointer argument was sign-extended as a 32-bit int (`movslq`) and truncated at runtime (wrong values or segfaults on `m[i][j]` through the parameter). Only the first dimension token may become the pointer: wrapping each dimension in a pointer (`int**` for `int m[][4]`) is the documented first-attempt bug.
- **Flattened brace-initializer placement** (`src/zero_cc/ir/ast_lowering_expr.c`): the parser's `parse_brace_init_list_ast` splices nested brace lists into a single leaf-level chain (`int m[2][4] = {{1,2,3,4},{5,6,7,8}}` becomes 8 int nodes). The compound-literal lowering therefore places each positional element by **C-layout leaf index**: `object_count_scalar_leaves` counts the scalar leaves of one object (arrays recurse `array_len × leaves(base)`; structs sum member leaves, mirroring `struct_resolve_scalar_leaf`'s array-member counting), and `object_resolve_scalar_leaf` maps leaf index → (offset, size) for 1-D, N-D, and struct members (mixed widths included). Pre-Sprint 40 the element k went to `k * base_size` — correct only for 1-D arrays of scalars (`int m[2][4]` stored element 1 at byte 16; `struct P v[2]` stored each int 16 bytes apart). Designated `[idx] =` maps element idx to the element's first leaf; unresolvable layouts fall back to the legacy `base_size` stride. The whole object is zero-initialized first, so unfilled slots stay 0 (C semantics). 2-level array designators (`[0][2] =`) are not parsed (single-level only).
- **Multi-declarator arrays** (`src/zero_cc/parser/ast_stmt.h`): `float a[n], b[n];` — every declarator in a declaration parses its own `[...]` dimensions and VLA info (pre-Sprint 40 only the first declarator's dimensions were parsed, so later declarators were mis-typed). Function parameter lists follow the array-parameter rule above.
- **Multiple VLAs per function** (`src/zero_cc/ir/ast_lowering_stmt.c`): the `Only one VLA allowed per function` restriction is gone — each VLA increments `vla_count`; the remaining rules (function scope, not inside loops) are unchanged.

## FP Conditional Expressions (Sprint 40)

`cond ? fa : fb` with float/double arms is correct at every -O level (JIT + AOT):
- The result temp is **defined by `OP_FMOV`** when either arm is FP — a temp's register class is derived from its defining opcode (`live_range.h`: FP op → XMM), so a GPR `MOV_REG` into an XMM-class temp is a class conflict. Pre-Sprint 40 the ternary always emitted `MOV_REG` and marked the result non-float: the caller read an uninitialized GPR and a trailing `(int)` cast lowered to `OP_SEXT32` instead of `OP_CVTTSD2SI`.
- **Mixed int/FP arms** follow the C usual arithmetic conversion: the int arm is `cvtsi2sd`'d before the `OP_FMOV`, in both directions (`c ? 7 : 2.5` and `c ? 1.5 : 0`).
- **Emission-order constraint**: the then-arm move is emitted before the else arm is lowered, so the else arm's floatness is predicted by `ast_expr_is_fp` (conservative AST probe: float literals, FP casts via `ast_node_get_typeinfo`, FP-typed idents/calls, FP binops — comparisons and `!` yield int; unknown shapes return 0, keeping the int path rather than over-converting). The final `last_expr_is_float` uses `then_fp || else_fp || else_may_be_fp`.
- The branchless pass (`opt_branchless.c`) still rejects FP arms (GPR guard) - unaffected.

## Hex Literal Classification (Sprint 40)

`lexer/scan.h` (`zero_cc_scan_number`): a hex literal (`0x…`) is classified as a floating constant **only when the consumed prefix contains `.` or a `p`/`P` exponent** (C99 6.4.4.2 - `0x1.8p1`, `0x1.Fp0`). In a hex literal `e`/`E` are ordinary hex DIGITS, not exponent markers. The pre-Sprint 40 scan treated `E` as an exponent, so `0x123456789ABCDEFLL` (and any hex literal containing an `E`/`e` digit) was lexed as a hex float: one `L` was consumed as a long-double suffix, the second was left dangling (parse error), or a double bit-pattern was stored in an int (the root cause of the Sprint 39 "64-bit literals > 32 bits miscompile" finding in ternary arms and XOR operands). Decimal literals keep the `.`/`e`/`E` float test (`1e2` is still a double); binary literals (`0b…`) are always integers. Suffix parsing (`U`/`L`/`LL`) is unchanged.

## Goto, User Labels & Switch (Implemented)

- **`goto label;` and `label: stmt`** (`src/zero_cc/parser/ast_stmt.h`): the statement parser handles `TK_GOTO` (`NODE_GOTO`, name in `s.str`) and a statement-leading `IDENT` immediately followed by `TK_COLON` (`NODE_LABEL`, name in `s.str`, `left` = the labeled statement). Lowering (`lower_stmt_goto`/`lower_stmt_label` in `src/zero_cc/ir/ast_lowering_stmt.c`) maps user names to IR labels through `label_patcher_find_or_create_user_label` + `get_or_create_user_ir_label` (names persist in `c->label_patcher.user_labels`; labels are created lazily on first goto/label, supporting forward references).
- **`switch` dispatch** (`lower_stmt_switch`, `src/zero_cc/ir/ast_lowering_stmt.c`): evaluates the controlling expression, pre-scans the body block's top-level statement chain for `NODE_CASE`/`NODE_DEFAULT`, emits a `CMP`/`COND_SET`/`JNE` dispatch chain per case (jump-if-**nonzero** — see the conditional-branch note in [isa-backends.md](./isa-backends.md)), stores each case's IR label in `node->aux.alt`, and registers the switch exit as the current break target via `label_patcher_enter_loop(..., switch_exit, 0)` so `break` works. A non-matching value skips the body entirely (unconditional jump past the body, per C semantics); fallthrough between case labels is natural code order. `NODE_CASE`/`NODE_DEFAULT` lowering emits their stored label (when set by a switch) and lowers the attached statement.
- **Known limitation**: `case` constants are lowered as runtime expressions (no compile-time deduplication); deeply nested `continue` inside a bare switch (no loop) is a no-op, matching prior behavior.

## C23 Features & Standard Conformance

- **`typeof` and `typeof_unqual`** (C23 6.7.2.5, GNU `__typeof__` / `__typeof`):
  - Supported everywhere type specifiers appear: local and global declarations, function return types, function parameters, struct/union members, typedefs, casts (`(typeof(T))val`), and `sizeof(typeof(...))`.
  - Deduced at parse-time via `zero_cc_parse_type_name_at_pos()` or `zero_cc_type_of_equality()` in `src/zero_cc/parser/const_eval.h`.
  - Supports both type names (`typeof(int)`) and expressions (`typeof(a + b)`, `typeof(p.x)`, `typeof(*ptr)`, `typeof(fn(x))`).
  - `typeof_unqual` strips `const`, `volatile`, `restrict`, and `_Atomic` qualifiers from the evaluated type.
- **Empty `{}` Initializers** (C23 6.7.10):
  - Standard zero-initialization syntax (`= {}`) supported across all storage classes: automatic locals (scalars, structs, arrays), static locals (`zero_cc_static_local_patch_init` in `ast_stmt.h`), and global variables (`compile_unit.h`).
- **`__STDC_NO_COMPLEX__ 1` Conformance**:
  - Predefined object macro `__STDC_NO_COMPLEX__ 1` in `src/zero_cc/core/preprocessor.h` formally signaling non-support for complex number types in accordance with ISO C11/C23 standards.

## Preprocessor Include-Path Semantics (Sprint 42)

Quoted `#include` resolution follows C99 6.10.2 and failures are no longer silent:

- **Main-file directory search (C99 6.10.2p2)**: `src/main.c` passes the command-line file path into the preprocessor via `zero_cc_preprocess_ext2(source, inc_dirs, defines, main_file, err_out, err_cap)` (new entry point in `src/zero_cc/core/preprocessor.h`). The main file's directory is pushed as the first entry of the `include_file_dirs` search stack, so `#include "sub/x.h"` in the top-level file resolves relative to that file's directory — not the process CWD. Nested includes keep the existing nearest-file-directory-first behavior (stack scanned from top). `__FILE__` in the main file now expands to the main file's path (previously empty).
- **Missing include is a hard error**: when `zero_cc_cpp_resolve_include` finds no candidate, the first failure is recorded in `ZeroCCPreprocessor.include_error` (gcc-style `<file>:<line>: fatal error: <name>: No such file or directory`) and `zero_cc_preprocess_ext2` returns NULL with the diagnostic in `err_out`. The CLI (`preprocess_with_options` in `src/main.c`) prints it and exits 1. Previously a missing include was silently skipped, producing confusing downstream "undeclared identifier" errors.
- **Legacy contract preserved**: `zero_cc_preprocess` / `zero_cc_preprocess_ext` delegate to `ext2` with `main_file = NULL` / `err_out = NULL`, reproducing the old behavior exactly (CWD-relative fallback, no error channel). Regression test: `test/semantic/include_path_test.c` (main-file-dir resolution, missing-include diagnostic, `__FILE__`, legacy `-I` dirs).

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

## Block-Scope `static` Locals (Sprint 37)

Block-scope `static` locals (C99 6.9.2) are implemented as **data-section objects with a per-call stack mirror**:
- At parse time (`src/zero_cc/parser/ast_stmt.h`), each `static` local is registered in the global-var table under a mangled name `__sl_<seq>` (`c->static_local_seq`) at an 8-byte-aligned `data_section` offset; the local symbol keeps a normal frame slot as a per-call mirror (`ZeroCCSym.is_static_local`, `.static_data_offset`, `.static_global_name`).
- Initializers must be constant literals (int/float/char, optional sign); the bytes are patched into `data_section` at parse time and the initializer is deliberately **not** attached to the AST, so it never re-runs per call. Complex initializers and multi-declarator `static` lists are rejected with diagnostics; `static` + VLA is an error.
- Lowering (`src/zero_cc/ir/ast_lowering.c`) emits `data → slot` chunked 8-byte copies in the prologue (before the body) and `slot → data` in the shared epilogue block (every return jumps to the epilogue label, natural fallthrough reaches it). All existing frame-relative access sites are unchanged.
- **Return-value protection**: the epilogue mirror-out runs *after* the return value is materialized in `VREG_0`, and its allocator temps may be mapped onto the return register. Therefore, when a function has ≥1 static local, an extra 8-byte frame slot (`lowering.static_local_rt_off`, reserved before the frame size is computed) saves/restores `VREG_0` around the mirror-out. FP results live in XMM registers, which the integer-only mirror-out cannot touch.
- A function containing a static local always declares a local, so the inliner's local-decl filter already excludes it — no duplicated data objects across inlined copies are possible.
- Regression guard: `test/inline_audit/10_static_local.c` (counter must read 100/101/102 across three calls). This test is **standalone**, so `opt_sweep.py` compiles it directly with zerocc at `-O0..-O3` in JIT and AOT; it failed at every `-O` level before Sprint 37 (statics were lowered as plain stack locals and re-initialized every call — the `TK_STATIC` specifier was silently discarded by the parser).

## Related Concepts
- Return to [Root Index](./index.md)
- See [Compilation Pipeline & Two-Pass Architecture](./architecture-pipeline.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)

