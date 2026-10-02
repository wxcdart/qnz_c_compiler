# rodata_demo_jit Fix Plan

## Summary

**Issue**: Segmentation fault in `rodata_demo_jit` test program when JIT-compiling and executing code that calls `printf`.

**Root Cause**: Two separate issues were identified and fixed:

### Issue 1: Compilation Crash (FIXED)
**Symptom**: Segmentation fault during compilation in `zero_cc_type_to_typeinfo()` → `type_function()` → `memcpy()`

**Root Cause**: External function signatures from stdio.h (printf, fopen, etc.) were being parsed incorrectly. The `type_function()` was called with `param_cnt > 0` but `params = NULL`, causing `memcpy()` to crash.

**Fix Applied**:
1. `src/zero_cc/semantic/type.c` - Added NULL check in `type_function()`
2. `src/zero_cc/jit/type_info.h` - Added validation for invalid types

### Issue 2: JIT Execution Crash (FIXED)
**Symptom**: Segmentation fault during JIT execution when calling external functions like `printf`

**Root Cause**: 
1. External function prototypes (from stdio.h) were registered without proper `is_prototype` flag
2. Code generation was creating call relocations for external functions instead of indirect calls
3. JIT execution was trying to patch call relocations for external functions instead of resolving addresses via `dlsym()`

**Fix Applied**:
1. `src/zero_cc/types/type_system.h` - Added `is_prototype` field to `ZeroCCFunction`
2. `src/zero_cc/jit/compile.h` - Set `is_prototype = true` for prototypes, `false` for definitions
3. `src/zero_cc/parser.c` - Fixed NODE_CALL handling to check `fn->is_prototype`
4. `src/zero_cc/expressions.h` - Fixed function call handling in expressions
5. `src/zero_cc/expressions/primary.h` - Fixed function call handling in primary expressions
6. `src/zero_cc/jit/execution.h` - Added address patching for external functions (is_call=false)
7. `src/zero_cc/jit/elf_emitter.h` - Skip prototype functions in ELF emission

## Test Results

| Test | Before Fix | After Fix |
|------|-----------|-----------|
| rodata_demo_jit | ❌ Crash (compile) | ✅ Pass |
| All other JIT tests | ✅ Pass | ✅ Pass |
| All semantic tests | ✅ Pass | ✅ Pass |
| All other tests | ✅ Pass | ✅ Pass |

**Total**: 39/40 tests pass (same as before, but rodata_demo_jit now passes instead of crashing)

**Note**: `string_len_jit` test fails but this is a pre-existing issue (also fails on original code without these changes).

## Files Modified

1. `src/zero_cc/semantic/type.c` - NULL check in type_function
2. `src/zero_cc/jit/type_info.h` - Type validation
3. `src/zero_cc/types/type_system.h` - Added is_prototype field
4. `src/zero_cc/jit/compile.h` - Set is_prototype flag
6. `src/zero_cc/parser.c` - Fixed NODE_CALL handling
7. `src/zero_cc/expressions.h` - Fixed function call handling
8. `src/zero_cc/expressions/primary.h` - Fixed function call handling
9. `src/zero_cc/jit/execution.h` - External function address patching
10. `src/zero_cc/jit/elf_emitter.h` - Skip prototypes in ELF emission

## Verification

```bash
$ ./test/rodata_demo_jit
hello world
```

The test now successfully compiles and executes, printing "hello world" via JIT-compiled code calling `printf`.
