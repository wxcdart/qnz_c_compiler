# Sprint 3 – Code Generation Depth

**Duration:** Weeks 9-13 (5 weeks)
**Status:** 🚀 Starting

## Overview

Sprint 3 focuses on deepening the code generation capabilities, particularly for floating-point operations, struct handling, variadic functions, and register allocation.

---

## Sprint Goals

| Priority | Task | Effort | Status |
|----------|------|--------|--------|
| 1 | Full SSE2 floating-point arithmetic (`addsd`, `subsd`, `mulsd`, `divsd`) | 1 week | ⏸️ Deferred - Requires XMM register management infrastructure |
| 2 | Struct return by value (hidden-pointer calling convention) | 1 week | 📋 Planned |
| 3 | `va_arg` expansion | 1 week | 📋 Planned |
| 4 | Linear-scan register allocator | 2 weeks | 🚀 In Progress |
| 5 | Global variable storage emission (`.data` / `.bss`) | 3 days | ✅ Complete |

---

## Task 1: SSE2 Floating-Point Arithmetic (Deferred)

### Current State
- `NODE_FLOAT` lowering exists and emits `movsd` from rodata correctly
- Binary operations (`+`, `-`, `*`, `/`) only handle integers
- Float literals work but float variables and operations do not

### Challenges
Full SSE2 floating-point arithmetic requires:
1. XMM register management during code generation
2. Type-aware code generation (tracking float vs int operands)
3. Conversions between integer and float registers
4. Float argument passing in function calls (System V ABI uses XMM0-XMM7)
5. Float return value handling (result in XMM0)

### Deferred Until
This task requires a more comprehensive redesign of the code generation infrastructure. It will be addressed after the register allocator (Task 4) is implemented, as proper register allocation is essential for efficient XMM register usage.

### Implementation Plan

1. **Type tracking during lowering**
   - Add type information to AST nodes during semantic analysis
   - Track whether expressions produce float or integer results

2. **Float binary operations**
   - Detect when operands are floats (via type info or NODE_FLOAT)
   - Emit SSE2 instructions:
     - `addsd xmm0, xmm1` for float addition
     - `subsd xmm0, xmm1` for float subtraction
     - `mulsd xmm0, xmm1` for float multiplication
     - `divsd xmm0, xmm1` for float division

3. **Float comparisons**
   - `ucomisd` for comparisons
   - Set flags appropriately for `<`, `<=`, `>`, `>=`, `==`, `!=`

4. **Float-to-int and int-to-float casts**
   - `cvtsd2si` for float to int
   - `cvtsi2sd` for int to float

### Test Cases
```c
// Basic float arithmetic
double add(double a, double b) { return a + b; }
double sub(double a, double b) { return a - b; }
double mul(double a, double b) { return a * b; }
double div(double a, double b) { return a / b; }

// Mixed type arithmetic
double mixed(int i, double d) { return i + d; }

// Float comparisons
int float_eq(double a, double b) { return a == b; }
int float_lt(double a, double b) { return a < b; }
```

---

## Task 2: Struct Return by Value

### Current State
- Structs are parsed and can be passed as arguments
- No proper struct return handling (small structs in RAX/rdx, large via hidden pointer)

### Implementation Plan

1. **Small struct return (≤16 bytes)**
   - Return in RAX (and RDX for >8 bytes)
   - Pack fields appropriately

2. **Large struct return (>16 bytes)**
   - Apply hidden-pointer convention: caller allocates space, passes pointer as hidden first arg
   - callee writes to that pointer

3. **Struct argument passing**
   - Small structs: pass in general-purpose registers
   - Large structs: pass by hidden pointer

### Test Cases
```c
// Small struct return
struct Point { int x, y; };
struct Point make_point(int x, int y) {
    struct Point p = {x, y};
    return p;
}

// Large struct return
struct Large {
    char data[32];
};
struct Large make_large(void);
```

---

## Task 3: va_arg Expansion

### Current State
- Variadic functions can be parsed
- `__VA_ARGS__` macro support exists
- No `va_arg` expansion or variadic calling convention

### Implementation Plan

1. **Variadic function calling convention**
   - Count register arguments (for `al` in variadic calls)
   - Set up stack spill area for variadic args

2. **va_list implementation**
   - Define va_list structure matching System V ABI
   - Implement va_start macro/expansion

3. **va_arg expansion**
   - Parse `va_arg(ap, type)`
   - Generate code to:
     - Read argument from stack/register area
     - Advance pointer by type size (with alignment)
     - Return argument value

### Test Cases
```c
#include <stdarg.h>

int sum(int count, ...) {
    va_list ap;
    va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += va_arg(ap, int);
    }
    va_end(ap);
    return total;
}
```

---

## Task 4: Linear-Scan Register Allocator

### Current State
- Naive stack-spill approach
- All temporaries go to stack
- No register reuse optimization

### Implementation Plan

1. **Liveness analysis**
   - Compute live intervals for each temporary
   - Track which values are live at each instruction

2. **Linear-scan algorithm**
   - Sort intervals by start position
   - Allocate registers greedily
   - Spill to stack when no registers available

3. **Register classes**
   - General-purpose: RAX, RCX, RDX, RBX, RSI, RDI, R8-R15
   - XMM for floats: XMM0-XMM7 (argument registers)

### Benefits
- Reduces stack traffic
- Improves performance significantly
- Required for efficient float operations (XMM registers)

---

## Task 5: Global Variable Storage

### Current State
- `data_section` buffer exists
- Globals are parsed but not emitted to data section

### Implementation Plan

1. **Global variable emission**
   - Write initialized globals to `.data` section
   - Write uninitialized globals to `.bss` section

2. **Relocation handling**
   - Generate relocations for global references
   - Support RIP-relative addressing

3. **Alignment**
   - Respect `_Alignas` for globals
   - Proper section alignment

### Test Cases
```c
int global_int = 42;
double global_double = 3.14;
char global_str[] = "hello";
static int static_var = 100;
extern int external_var;
```

---

## Acceptance Criteria

1. **Float arithmetic tests pass**
   - All basic arithmetic operations work correctly
   - Mixed int/float operations work
   - Float comparisons work

2. **Struct return tests pass**
   - Small structs returned correctly
   - Large structs use hidden pointer

3. **Variadic tests pass**
   - `printf`-style functions work
   - `va_arg` retrieves correct values

4. **Register allocator integrated**
   - Code generation uses register allocator
   - No regressions in existing tests

5. **Global variables work**
   - Initialized globals have correct values
   - Uninitialized globals zero-initialized
   - External symbol resolution works

---

## Dependencies

- Task 1 (Float) can start immediately
- Task 2 (Struct return) benefits from Task 4 (Register allocator)
- Task 3 (va_arg) requires Task 2 (calling convention)
- Task 5 (Globals) can be done independently

---

## Notes

- Float operations require careful handling of XMM registers
- The register allocator is the most complex task
- Consider incremental approach: implement float ops first without full register allocator
- Use SSE2 for double, consider SSSE3/SSE4 for optimizations later

---

## Sprint 3 Progress Summary

### Completed Tasks

**Task 5: Global Variable Storage Emission** ✅

Implemented support for global variable storage and access:

1. **Data section inclusion in JIT memory**
   - Updated `execution.h` to allocate memory for data_section
   - Data section is now copied to JIT memory after text and rodata sections

2. **Global variable access code generation**
   - Updated `NODE_IDENT` lowering in `ast_lowering.h` to handle global variables
   - Uses RIP-relative addressing to load global variable values
   - Relocation records track global variable references for patching

3. **Relocation patching for data references**
   - Updated `execution.h` to patch RIP-relative offsets for global variable access
   - Global variables are correctly resolved at JIT execution time

4. **Type system update**
   - Added `data_offset` field to `ZeroCCReloc` structure for data reference tracking

**Test Results:**
- `test/global_vars`: ✅ PASS (simple global variable read)
- `test/global_vars2`: ✅ PASS (global variable with initializer)
- `test/global_vars3`: ✅ PASS (multiple global variables)

### Deferred Tasks

**Task 1: SSE2 Floating-Point Arithmetic** ⏸️

Full SSE2 floating-point arithmetic was deferred because it requires:
- XMM register management during code generation
- Type-aware code generation (tracking float vs int operands)
- Conversions between integer and float registers
- Float argument passing in function calls (System V ABI uses XMM0-XMM7)
- Float return value handling (result in XMM0)

This task will be addressed after the register allocator (Task 4) is implemented.

### Test Status

**Overall:** ✅ **71 PASS / 0 FAIL**

**New Tests Added (Task 5 - Global Variables):**
- ✅ `test/global_vars` - Global variable read
- ✅ `test/global_vars2` - Global variable with initializer  
- ✅ `test/global_vars3` - Multiple global variables

**Previously Failing Tests Now Fixed:** ✅
- `test/jit_test` - Basic function call JIT
- `test/ternary_jit` - Ternary operator
- `test/string_literal_jit` - String literals
- `test/do_while_jit` - Do-while loops
- `test/compound_literal_jit` - Compound literals
- `test/tls_jit` - Thread-local storage
- `test/abi_sum_jit` - ABI parameter passing
- `test/atomic_jit` - Atomic operations
- `test/generic_ptr_jit` - Generic pointers
- `test/string_expr_jit`, `test/string_expr2_jit`, `test/string_len_jit`, `test/string_simple_jit` - String expressions
- `test/ast_lowering` - AST lowering tests
- `test/predefined_macros` - Predefined macros
- `test/exe_test` - ELF executable generation

**Root Cause:** The JIT tests were failing because `parser.o` had compilation errors in the lowering code (incorrect field access `node->s.float_val` and missing includes for `global_vars.h`). The stale object file was being used, so the lowering code never executed. Fixes:
1. Fixed `node->s.float_val` → `node->float_val` (direct union member)
2. Added `#include "../types/global_vars.h"` to `ast_lowering.h`
3. Removed duplicate epilogue code (now emitted only in `zero_cc_lower_ast`)
4. Fixed Makefile `clean` target to remove `.o` files
5. Fixed Makefile parser dependencies to include all header files

**All Tests Passing:** ✅
- Basic compilation
- Statement tests
- Expression tests
- Type system tests
- Struct by value tests
- Global variable tests
- Goto/label tests
- JIT execution tests
- ELF generation tests
- And more...

### Next Steps

1. Complete Task 4: Integrate register allocator with code generation
   - [x] Create register allocator data structures
   - [x] Implement linear-scan algorithm
   - [x] Add temporary variable tracking
   - [x] Infrastructure for code generation using registers
   - [ ] Generate code using allocated registers (replace stack-based temps)
   - [ ] Handle spill code generation
2. Once register allocator is complete, revisit Task 1: SSE2 floating-point arithmetic
3. Implement Task 2: Struct return by value
4. Implement Task 3: va_arg expansion

---

## Task 4: Linear-Scan Register Allocator (In Progress)

### Current State

**Infrastructure Complete:** ✅
- Created `src/zero_cc/parser/regalloc.h` with linear-scan algorithm
- Added `RegAllocator` and `LiveInterval` data structures
- Implemented register allocation for GPR and XMM registers
- Added temporary variable tracking in lowering code
- Integrated register allocator initialization in compilation flow

**Integration Status:** 🚧
- Register allocator runs after lowering (placeholder)
- Temp variables are tracked but not yet used for code generation
- Next step: Generate code using allocated registers instead of stack

### Implementation Details

**Register Classes:**
- GPR: RAX, RCX, RDX, RBX, RSI, RDI, R8-R15 (14 registers)
- XMM: XMM0-XMM7 (8 registers for SSE2 floats)

**Algorithm:**
1. Track temporary variables during lowering
2. Compute live intervals (start/end positions)
3. Sort intervals by start position
4. Allocate registers greedily
5. Spill to stack when no registers available

### Files Modified

- `src/zero_cc/parser/regalloc.h` - New register allocator implementation
- `src/zero_cc/types/symbols.h` - Added `regalloc` and `next_temp_id` to compiler state
- `src/zero_cc/jit/compile_unit.h` - Initialize register allocator
- `src/zero_cc/jit/compile.h` - Reset regalloc per function
- `src/zero_cc/parser/ast_lowering.h` - Add temp tracking and run allocation

### Remaining Work

1. **Code Generation Integration:**
   - Modify lowering to use allocated registers
   - Generate load/store for spilled temporaries
   - Update instruction emission to use register indices

2. **Liveness Analysis:**
   - More precise live interval computation
   - Track actual use-def chains

3. **Spill Code Generation:**
   - Generate stack slots for spilled temps
   - Insert load/store instructions at correct points

4. **Testing:**
   - Verify register allocation correctness
   - Performance comparison with stack-only approach
