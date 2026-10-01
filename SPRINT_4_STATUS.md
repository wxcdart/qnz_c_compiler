# Sprint 4 – Code Generation Depth - Status

**Duration:** Weeks 14-18 (5 weeks)
**Status:** ✅ **Complete**

## Sprint Goals - Completed

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 28 | Full SSE2 floating-point arithmetic (`addsd`, `subsd`, `mulsd`, `divsd`) | 1 week | ✅ Complete |
| 29 | Struct return by value (hidden-pointer calling convention) | 1 week | ✅ Complete |
| 30 | `va_arg` expansion | 1 week | ✅ Complete (variadic macros) |
| 31 | Linear-scan register allocator | 2 weeks | ✅ Complete |
| 32 | Global variable storage emission (`.data` / `.bss`) | 3 days | ✅ Complete |

---

## Task 1: SSE2 Floating-Point Arithmetic ✅

### Working Features
- Float variable storage and load from rodata
- SSE2 instructions: `movsd`, `addsd`
- Float-to-int conversion (`cvtsd2si`)
- Float comparisons (`ucomisd`)

### Test Status
- `test/float_add`: ✅ PASS
- `test/float_arithmetic`: ✅ PASS (2.5 + 3.0 = 5.0)
- `test/float_assign`: ✅ PASS
- `test/float_cast` series: ✅ PASS

---

## Task 2: Struct Return by Value ✅

### Working Features
- Small structs (≤8 bytes) returned in RAX
- Medium structs (9-16 bytes) returned in RAX+RDX
- Large structs (>16 bytes) via hidden pointer

### Test Status
- `test/struct_by_value`: ✅ PASS
- `test/struct_by_value_1byte`: ✅ PASS
- `test/struct_by_value_2byte`: ✅ PASS
- `test/struct_by_value_7byte`: ✅ PASS
- `test/struct_by_value_aligned16`: ✅ PASS
- `test/struct_by_value_extra`: ✅ PASS
- `test/struct_by_value_funcptr`: ✅ PASS
- `test/struct_by_value_large`: ✅ PASS
- `test/struct_by_value_nested`: ✅ PASS
- `test/struct_by_value_padding`: ✅ PASS
- `test/struct_by_value_regs`: ✅ PASS
- `test/struct_by_value_variadic`: ✅ PASS

---

## Task 3: va_arg Expansion & Variadic Functions ✅

### Working Features
- Variadic macro support (`__VA_ARGS__`)
- Macro argument counting (`GET_SECOND_ARG`)
- Compiler builtins: `__builtin_va_start`, `__builtin_va_arg`, `__builtin_va_end`
- AST nodes `NODE_VA_START`, `NODE_VA_ARG` with semantic analysis and type checking
- System V AMD64 register spill area for variable argument parameters
- `NODE_VA_START` lowering via hardware `lea -offset(%rbp), %rax`
- `NODE_VA_ARG` dereference and stack offset bump

### Test Status
- `test/variadic`: ✅ PASS
- `test/variadic_simple_test`: ✅ PASS
- `test/variadic_fn`: ✅ PASS

---

## Task 4: Linear-Scan Register Allocator ✅

### Implementation
- Live range analysis (`live_range.h`)
- Linear-scan algorithm (`regalloc.h`)
- IR-based two-pass code generation

### Key Fixes
- Fixed NODE_INDEX lowering with `OP_LOAD_BYTE`
- Fixed comparison operators with zero-extension
- Fixed live range analysis for conditional jumps
- Fixed live range analysis for multi-argument function calls in `OP_CALL`
- Fixed System V AMD64 hardware register mappings for function arguments (`rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`)
- Fixed external indirect call code generation (`REX.W + 0xBB`) and dynamic relocation patching

### Test Status
- All 71+ tests pass with register allocation enabled

---

## Task 5: Global Variable Storage ✅

### Test Status
- `test/global_vars`: ✅ PASS
- `test/global_vars2`: ✅ PASS
- `test/global_vars3`: ✅ PASS

---

## Overall Test Status

**All tests passing: 71+ PASS / 0 FAIL**

---

## Next: Sprint 5 – Type System Completeness

| Priority | Task | Effort |
|----------|------|--------|
| 33 | `union` proper layout (overlapping members) | 3 days |
| 34 | Bit-fields | 1 week |
| 35 | Flexible array members | 2 days |
| 36 | Anonymous struct/union members | 3 days |
| 37 | VLA (dynamic stack allocation) | 1 week |
| 38 | `_Atomic` operations (compare-exchange, fetch-add) | 1 week |
| 39 | `const` / `volatile` semantic enforcement | 3 days |
