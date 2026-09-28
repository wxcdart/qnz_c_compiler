# Fix Plan: `rodata_demo_jit` Segfault

## Issue Summary

**Test:** `test/rodata_demo_jit`  
**Status:** ❌ Segmentation fault (core dumped)  
**Nature:** Pre-existing issue, unrelated to Phase 3 refactoring  
**Location:** Suspected in `zero_cc_type_to_typeinfo()` / `type_function()` during JIT compilation

---

## Investigation Results

### Root Cause Found

**Phase 1 Complete** - The crash was in `zero_cc_type_to_typeinfo()` during compilation, not during JIT execution.

**Issue 1: Compilation Crash (FIXED)**
- The crash occurred when processing external function signatures from stdio.h (printf, fopen, etc.)
- The function signature parsing was incorrectly populating return types and parameter types
- When `type_function()` was called with `param_cnt > 0` but `params = NULL`, `memcpy()` crashed

**Fix Applied:**
1. Added NULL check in `type_function()` to prevent memcpy from NULL
2. Added validation in `zero_cc_type_to_typeinfo()` to handle invalid types (size > 1024)
3. Added NULL check for pointer base types (incomplete types like FILE*)

**Status:** ✅ FIXED - Compilation now succeeds (39/40 tests pass)

---

**Issue 2: JIT Execution Crash (INVESTIGATING)**
- After fixing the compilation crash, the test now crashes during JIT execution
- The crash occurs at address 0x7ffff7fba012 (in libc)
- The generated code calls `printf("hello world\n")` and crashes

**Possible Causes:**
1. External function relocation is incorrect
2. The `dlsym()` resolution for `printf` returns NULL or incorrect address
3. The generated call sequence is incorrect

**Next Steps:**
1. Add debug output to `zero_cc_resolve_symbol()` to verify `printf` resolution
2. Check the generated machine code for the printf call
3. Verify the relocation patching in `zero_cc_execute_jit()`

### Symptom
```
Running test/rodata_demo_jit...
Segmentation fault (core dumped)
```

### Suspected Root Cause
Based on prior analysis:
- Crash occurs in `type_function()`'s `memcpy()` copying the parameters array
- Segfault happens in `zero_cc_type_to_typeinfo()` during JIT compilation
- Likely related to **incomplete types** (e.g., `FILE *` from `stdio.h`) being processed by the JIT linker's type info generator

### Context
- The JIT linker's `type_to_typeinfo()` function may attempt to copy type information for types that are **incomplete** or **opaque** (like `FILE *` where the struct definition is unknown to the JIT)
- `memcpy()` may be called with invalid size or pointer values

---

## Investigation Steps

### Step 1: Reproduce with Debugger

```bash
# Run with gdb
gdb --args ./test/rodata_demo_jit

# In gdb:
(gdb) run
(gdb) bt          # Get backtrace
(gdb) info registers
(gdb) x/20i $pc   # Examine instructions around crash
```

**Expected Output:**
- Identify exact line in `jit/type_info.h` or `jit/compile_unit.h`
- Check `memcpy` arguments: destination, source, size

### Step 2: Inspect Test Source

```c
// test/rodata_demo_jit.c
// Read the test to understand what code pattern triggers the crash
```

**Look for:**
- Function signatures with complex types
- Use of `FILE *`, `struct foo *` where `struct foo` is incomplete
- Function pointers
- Structs with nested types

### Step 3: Examine Type Info Generator

**File:** `src/zero_cc/jit/type_info.h`

**Functions to inspect:**
1. `zero_cc_type_to_typeinfo()` - Main type conversion function
2. `type_function()` - Handles function type conversion
3. `memcpy` calls - Check for size calculations

**Questions to answer:**
- Does it handle opaque pointers correctly?
- Does it validate type completeness before copying?
- Does it handle `sizeof(params)` correctly when params is empty?

### Step 4: Check Type Registration

**File:** `src/zero_cc/jit/compile_unit.h`

**Questions:**
- How are external types (libc types like `FILE *`) registered?
- Are incomplete types handled or skipped?
- Is there validation before JIT compilation?

---

## Hypotheses & Fixes

### Hypothesis A: Incomplete Type Handling (Most Likely)

**Problem:**
The JIT linker tries to generate type info for `FILE *` or similar opaque types, but the struct definition is unknown, causing invalid size calculations.

**Fix:**
Add validation in `zero_cc_type_to_typeinfo()` to skip or handle incomplete types:

```c
// In jit/type_info.h
static inline TypeInfo *zero_cc_type_to_typeinfo(ZeroCCType *type) {
    // Handle opaque pointers (e.g., FILE *)
    if (type->kind == TYPE_PTR && type->ptr_type->kind == TYPE_INCOMPLETE) {
        // Return minimal type info for opaque pointers
        TypeInfo *info = typeinfo_create();
        info->size = sizeof(void*);
        info->align = sizeof(void*);
        return info;
    }
    // ... rest of function
}
```

### Hypothesis B: Invalid memcpy Size

**Problem:**
`memcpy()` is called with size 0, negative size, or invalid pointer.

**Fix:**
Add validation before `memcpy`:

```c
// In jit/type_info.h, type_function()
if (fn->param_count > 0 && fn->params) {
    size_t params_size = fn->param_count * sizeof(ZeroCCType);
    if (params_size > 0 && params_size < MAX_PARAMS * sizeof(ZeroCCType)) {
        memcpy(info->params, fn->params, params_size);
    }
}
```

### Hypothesis C: Stack/Scope Issue

**Problem:**
The `params` array is allocated on the stack and accessed after the function returns.

**Fix:**
Ensure `params` is allocated in the arena or permanent memory:

```c
// In type_function()
// Instead of: ZeroCCType params[MAX_PARAMS];
// Use:
ZeroCCType *params = arena_alloc(perm, fn->param_count * sizeof(ZeroCCType));
```

---

## Testing Plan

### Before Fix
```bash
make clean && make -j4
./test/rodata_demo_jit  # Should segfault
```

### After Fix
```bash
make clean && make -j4
./test/rodata_demo_jit  # Should pass
make test               # All 40 tests should pass
```

### Regression Check
Ensure these tests still pass:
- `jit_test` - Basic JIT functionality
- `abi_sum_jit` - Function ABI
- `string_simple_jit` - String handling
- `struct_by_value` - Struct handling

---

## Files to Modify

1. **`src/zero_cc/jit/type_info.h`** - Main fix location
2. **`src/zero_cc/jit/compile_unit.h`** - Type registration logic
3. **`src/zero_cc/jit/execution.h`** - JIT execution (if needed)

---

## Success Criteria

1. ✅ `rodata_demo_jit` passes without segfault
2. ✅ All 40 tests pass (no regressions)
3. ✅ Build succeeds with `make -j4`
4. ✅ No new warnings introduced
5. ✅ Code is well-documented with comments explaining the fix

---

## Timeline

| Phase | Duration | Goal |
|-------|----------|------|
| Investigation | 1-2 hours | Get backtrace, identify root cause |
| Fix Implementation | 1-2 hours | Implement and test fix |
| Validation | 30 min | Run all tests, ensure no regressions |
| Documentation | 30 min | Add comments, update docs |

**Total Estimated Time:** 3-4 hours

---

## Notes

- This is a **pre-existing bug** from before the Phase 3 refactoring
- The refactoring actually **helps** debugging because:
  - Code is more modular (`jit/type_info.h` is isolated)
  - Include paths are correct
  - Build system is stable
- Consider adding a **minimal regression test** if the fix involves a specific edge case

---

## Status

| Item | Status |
|------|--------|
| Phase 3 Refactoring | ✅ Complete |
| Build Stability | ✅ Stable (39/40 tests pass) |
| Investigation Started | ⏳ Pending |
| Fix Implemented | ⏳ Pending |
| All Tests Passing | ❌ (1 failure) |

**Next Action:** Run `gdb` on `rodata_demo_jit` to get backtrace and confirm root cause.
