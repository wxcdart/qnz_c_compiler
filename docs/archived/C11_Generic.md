# C11 _Generic Extension Plan

## Current State
* `_Generic` is parsed in `zero_cc_parse_primary`
* Controlling expression type is obtained via the type‑only parser (`zero_cc_type_of_equality`)
* Association matching is tag-based for scalar types
* Default branch works
* Pointer-type inference via symbol table works for `generic_ptr_jit`

## Goals
Full C11 generic selection with minimal self-hostable changes.

## Phases

### Phase 1 – Proper controlling expression typing
* Save token position before parsing controlling expression
* Use `zero_cc_type_of_equality` family to obtain `ZeroCCType` of controlling expression without codegen
* Strip qualifiers `const`/`volatile`/`_Atomic` for comparison
* Handle array-to-pointer decay for controlling expression
* Implementation sketch:
  ```c
  size_t ctrl_pos = c->token_pos;
  ZeroCCType ctrl_ty = zero_cc_type_of_equality(c, &ctrl_pos);
  ctrl_ty = zero_cc_type_strip_qualifiers(ctrl_ty);
  if (ctrl_ty.tag == TYPE_ARRAY) ctrl_ty = zero_cc_type_ptr(&ctrl_ty);
  ```
* Add helper `zero_cc_type_strip_qualifiers` – clears `is_const`, `is_volatile`, `is_atomic`

### Phase 1.5 – Type canonicalisation
* Normalise integer types: `signed int`, `int` -> `TYPE_INT`
* Normalise `long long` -> `TYPE_INT64`
* Normalise pointer types: store base type pointer
* For typedefs, resolve to underlying type for comparison
* For struct/enum/union, compare `tag` + `name` hash

### Phase 2 – Association type parsing
* Parse each association type using `zero_cc_parse_type_name` which already handles:
  * `int`, `char`, `long`, `long long`, `short`, `float`, `double`, `void`
  * typedef names via `zero_cc_find_typedef`
  * pointer stars `T*`, `T**`
  * `_Atomic(T)`, `_Atomic T`
  * struct/union/enum tag names via symbol table
* Build a canonical `ZeroCCType` for each association
* Store association list in temporary struct for matching
* Parser state machine:
  1. Consume association type tokens until `:`
  2. Call `zero_cc_parse_type_name` with saved position
  3. Canonicalise result
  4. Advance to expression start
* Store up to 16 associations in stack array `struct { ZeroCCType ty; size_t expr_pos; }`

### Phase 3 – Selection rules
Implement C11 6.5.1.1 selection algorithm:
1. Exact match after qualifier stripping
   * Compare `tag`, `ptr_depth`, `base name` for structs/enums
2. If no exact match, consider compatible types with promotion rules:
   * integer promotions: `char`/`short` -> `int`, `unsigned char` -> `unsigned int` etc.
   * usual arithmetic conversions
3. For `void*` vs other pointers – allow `void*` association to match any object pointer
4. Struct/union/enum: match by `tag` and `name` equality; ignore qualifiers
5. Default association if present

Matching function sketch:
```c
static inline int zero_cc_type_match(ZeroCCType a, ZeroCCType b){
    a = zero_cc_type_strip_qualifiers(a);
    b = zero_cc_type_strip_qualifiers(b);
    if (a.tag == b.tag && a.tag != TYPE_PTR) return a.tag == b.tag && strcmp(a.name, b.name) == 0;
    if (a.tag == TYPE_PTR && b.tag == TYPE_PTR){
        /* void* wildcard */
        if (a.base.tag == TYPE_VOID || b.base.tag == TYPE_VOID) return 1;
        return zero_cc_type_match(a.base, b.base);
    }
    /* integer promotion */
    if (zero_cc_type_is_integral(a.tag) && zero_cc_type_is_integral(b.tag)){
        return zero_cc_type_promote_rank(a) == zero_cc_type_promote_rank(b);
    }
    return 0;
}
```

### Phase 4 – Code generation
* Once matching association is identified, parse and emit code for the associated expression only
* Skip non-matching branches without codegen to avoid side effects
* Preserve source location for error messages
* Implementation steps:
  1. After selection, reset `c->token_pos` to start of matching expression
  2. Call `zero_cc_parse_expr(c)`
  3. Advance to next comma or `)`
  4. For non-matching branches, fast-forward using `zero_cc_skip_expr`
* `zero_cc_skip_expr` uses the type-of parser to consume tokens without codegen

### Phase 4.5 – Edge cases
* Controlling expression is `_Generic` itself – nested handling
* Association types with qualifiers: `const int`, `volatile int`
* Array type associations – decay to pointer for controlling expression only
* `_Atomic` types – strip atomic flag for matching, preserve for expression typing

### Phase 5 – Tests
* `test/generic_struct.c` – struct tag selection
* `test/generic_struct_jit.c` – JIT variant
* `test/generic_ptr_decay.c` – array decays to pointer
* `test/generic_ptr_decay_jit.c` – JIT variant
* `test/generic_qual.c` – const qualified matching
* `test/generic_qual_jit.c` – JIT variant
* `test/generic_complex.c` – mixed scalar/pointer/struct with default
* `test/generic_complex_jit.c` – JIT variant
* `test/generic_nested.c` – nested `_Generic`
* Regression tests for existing `test/generic.c` and `test/generic_ptr_jit.c`

Expected outputs:
```c
_Generic(x, int: 1, float: 2, default: 0)
```
```c
_Generic(s, struct S: 10, default: 0)
```
```c
_Generic(arr, int*: 3, default: 0) // arr decays
```

## Non-Goals for Self-Hosted MVP
* Full type compatibility graph for all C11 types
* Generic selection with expression side-effects – still compile-time only

## Implementation Notes
* Keep changes in `expressions.h` `zero_cc_parse_primary`
* Reuse existing `zero_cc_type_of_*` functions
* No new allocations – use stack buffers
* Keep header size <=900 lines; split if needed later

## Success Criteria
* `make test` passes
* All generic tests compile with GCC and JIT
* No macro cheating
* Self-hostable
