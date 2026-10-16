---
type: Concept
title: Frontend, Type System & Semantic Analysis
description: Dual type representation (ZeroCCType and TypeInfo), struct/union layout, function pointer declarations, and semantic checking rules.
resource: frontend/types-and-semantics
tags: [frontend, parser, types, semantic, function-pointers, c11]
timestamp: 2026-10-15T22:55:00Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Frontend, Type System & Semantic Analysis

ZeroCC uses two complementary type representations bridged during compilation:

1. **`ZeroCCType` (`src/zero_cc/types/type_system.h`)**:
   - Used by the parser, symbol tables (`ZeroCCSym`, `ZeroCCFunction`), and AST-to-IR lowering.
   - Tracks `tag` (`TYPE_INT32`, `TYPE_PTR`, `TYPE_STRUCT`, `TYPE_UNION`, `TYPE_ARRAY`, etc.), `size`, `align`, `base` pointer, `members` array, qualifiers (`is_const`, `is_volatile`, `is_restrict`, `is_atomic`), and `is_func_ptr`.
2. **`TypeInfo` (`src/zero_cc/semantic/type.h`, bridged via `src/zero_cc/jit/type_info.h`)**:
   - Used by the semantic analysis pass (`src/zero_cc/semantic/`) to validate expressions and annotate `ZeroCCASTNode` via `expr_node_set_type`.

## Function Pointer Representation (`is_func_ptr`)

- When parsing function pointer typedefs (`typedef RetType (*Alias)(params...);` in `src/zero_cc/parser/definitions.h`) or inline local function pointer declarations (`RetType (*fn)(params...) = init;` in `src/zero_cc/parser/ast_stmt.h`):
  - The symbol's `ZeroCCType` has `tag = TYPE_PTR`, `size = 8`, `align = 8`, `is_func_ptr = true`, and `base` pointing to a heap/arena-allocated `ZeroCCType` representing the **return type** (`RetType`).
- In `zero_cc_type_to_typeinfo()` (`src/zero_cc/jit/type_info.h`):
  - If `zt.tag == TYPE_PTR && zt.is_func_ptr`, it constructs `TYPE_KIND_POINTER` → `TYPE_KIND_FUNCTION` whose `.details.function.ret` preserves the return `TypeInfo` (including `TYPE_STRUCT` with its full byte size).
- In `sem_analyze_node()` (`src/zero_cc/semantic/visitor.c`):
  - `case NODE_CALL:` inspects the callee's `TypeInfo *fn_t` (supporting both direct `type_is_function(fn_t)` and indirect `type_is_pointer(fn_t)` to function) and annotates the `NODE_CALL` AST node with `res` via `expr_node_set_type(ctx, node, res)` without rejecting unprototyped/enum conversions.

## Struct & Union Layout Rules

- Structs and unions store field metadata in `type.members[i]` (`name`, `offset`, `size`, `bit_width`, `bit_offset`, `is_flexible`).
- `#pragma pack(push, N)` and `#pragma pack(pop)` adjust field alignment and total struct padding (`src/zero_cc/core/pragma_state.c`).
- Local variables allocated via `zero_cc_add_local()` (`src/zero_cc/types/symbol_mgmt.h`) align `c->current_stack_frame` to `max(type.align, 8)` and increment by `alloc_sz = max(type.size, 8)`.
  - **Important Rule**: Do **not** round `alloc_sz` up to a multiple of 8 for all structs in `zero_cc_add_local`, as compound literal and designated initializer offset math in `test/jit/designated_init_test.c` relies on exact `sym->stack_offset` placement. Instead, use size-aware tail chunk loads/stores (`32`/`16`/`8`-bit) when copying structs whose size is not a multiple of 8.

## Related Concepts
- Return to [Root Index](./index.md)
- See [Compilation Pipeline & Two-Pass Architecture](./architecture-pipeline.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
