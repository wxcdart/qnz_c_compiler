# `-S` Flag Limitation with JIT Test Files

## Issue Summary

The `-S` flag (assembly output) **does not work** with files in `test/jit/*.c`, but **does work** with regular C files.

### Example

```bash
# ❌ Does NOT work - produces empty output
./build/bin/zerocc -S -o /tmp/test.s test/jit/vla_test.c
# Result: /tmp/test.s is empty (0 bytes)

# ✅ Works correctly - produces assembly
./build/bin/zerocc -S -o /tmp/test.s /tmp/vla_standalone.c
# Result: /tmp/test.s has 250+ lines of assembly
```

---

## Root Cause

### JIT Test Files Are Test Harness Code

Files in `test/jit/*.c` are **not regular C programs**. They are test harness code that:

1. **Include compiler internal headers**:
   ```c
   #include "zero_cc.h"  // ← Compiler internal API
   ```

2. **Use compiler internal types**:
   ```c
   ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
   ```

3. **Call compiler internal functions**:
   ```c
   char *prep = zero_cc_preprocess(src, NULL);
   zero_cc_tokenize_c99(prep, comp);
   zero_cc_compile_c99(comp);
   int ret = zero_cc_execute_jit(comp, NULL);
   ```

4. **Are meant to be RUN as tests, not compiled as standalone programs**

### Regular C Files Work Fine

Regular C files (even complex ones with VLA, loops, function calls) work perfectly:

```c
// /tmp/complex.c - Regular C code ✅
#include <stdio.h>

int add(int a, int b) {
    return a + b;
}

int vla_test(int n) {
    int arr[n];
    int sum = 0;
    for (int i = 0; i < n; i++) {
        arr[i] = i * 2;
        sum += arr[i];
        sum = add(sum, i);
    }
    return sum;
}

int main(void) {
    return vla_test(5);
}
```

```bash
./build/bin/zerocc -S -o /tmp/complex.s /tmp/complex.c
# Result: ✅ 250+ lines of assembly
```

---

## Comparison

| Aspect | JIT Test Files | Regular C Files |
|--------|---------------|-----------------|
| Location | `test/jit/*.c` | Any `.c` file |
| Purpose | Test harness | Standalone program |
| Includes | `zero_cc.h` (internal) | `<stdio.h>`, etc. |
| Uses compiler APIs | ✅ Yes | ❌ No |
| Works with `-S` | ❌ No | ✅ Yes |
| Works with `make test` | ✅ Yes | N/A |

---

## Why This Design?

### JIT Test Files

```c
// test/jit/vla_test.c
#include "zero_cc.h"

static int run_snippet(const char *src) {
    // This function COMPILES a C string at runtime using the JIT
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    zero_cc_compile_c99(comp);
    int ret = zero_cc_execute_jit(comp, NULL);  // ← JIT execution
    // ... cleanup ...
    return ret;
}

int main(void) {
    const char *source = "int main() { return 42; }";
    return run_snippet(source);
}
```

**Purpose**: Test the **JIT compilation path** by:
1. Parsing C source code as a string
2. Compiling it at runtime using JIT
3. Executing the generated machine code
4. Verifying the result

**Cannot use `-S` because**: The C source is embedded as a string, not in the file itself!

### Regular C Files

```c
// /tmp/vla_standalone.c
int vla_test(int n) {
    int arr[n];
    // ... code ...
}

int main(void) {
    return vla_test(5);
}
```

**Purpose**: Standalone C program that can be:
- Compiled with `-S` to generate assembly
- Compiled to object files
- Linked and executed

---

## Impact on Testing

### What We Can Test with `-S`

✅ **Regular C files**:
```bash
./build/bin/zerocc -S -o /tmp/test.s /tmp/vla_standalone.c
python3 tools/stack_alignment_checker.py --asm-file /tmp/test.s
```

✅ **Generated test cases**:
```bash
python3 tools/prologue_regression_suite.py --run-all
```

### What We Cannot Test with `-S`

❌ **JIT test harness files**:
```bash
./build/bin/zerocc -S -o /tmp/test.s test/jit/vla_test.c
# Result: Empty file (not useful)
```

---

## Workarounds

### Option 1: Extract Test Logic (Recommended)

Create standalone C files that test the same functionality:

```bash
# Original JIT test (can't use -S)
test/jit/vla_test.c

# Extracted standalone version (can use -S)
/tmp/vla_standalone.c
```

**Example**: `prologue_regression_suite.py` does exactly this:
- Generates standalone C test cases
- Compiles them with `-S`
- Validates the assembly output

### Option 2: Use `make test` for JIT Tests

JIT tests are meant to be run, not compiled:

```bash
make test  # Runs all JIT tests ✅
```

### Option 3: Use Assembly Output from Successful Compilation

If you need to inspect assembly for a JIT test, create a standalone version:

```c
// test/jit/vla_test.c (JIT harness)
// ↓ Extract the actual test code ↓
// /tmp/vla_standalone.c (standalone)
```

---

## Best Practices

### For Assembly Testing

1. **Use standalone C files**:
   ```bash
   ./build/bin/zerocc -S -o test.s standalone.c
   ```

2. **Use regression suite**:
   ```bash
   python3 tools/prologue_regression_suite.py --run-all
   ```

3. **Use stack alignment checker**:
   ```bash
   python3 tools/stack_alignment_checker.py --asm-file test.s
   ```

### For JIT Testing

1. **Use `make test`**:
   ```bash
   make test  # Runs all JIT tests
   ```

2. **Don't try to compile with `-S`**:
   ```bash
   # ❌ Don't do this
   ./build/bin/zerocc -S -o test.s test/jit/vla_test.c
   ```

### For New Tests

When adding new tests:

1. **If testing JIT**: Add to `test/jit/*.c`
2. **If testing assembly**: Create standalone C file + add to regression suite

---

## Summary

| Question | Answer |
|----------|--------|
| Is `-S` broken? | ❌ No, it works correctly |
| Why do JIT tests fail? | They're test harness code, not regular C |
| Can I test assembly output? | ✅ Yes, use standalone C files |
| How do I test prologue/epilogue? | Use `prologue_regression_suite.py` |
| Should I modify JIT tests? | No, use standalone versions for assembly testing |

---

## Related Documentation

- [Prologue Debugging Tools](PROLOGUE_DEBUGGING_TOOLS.md)
- [Prologue Refactor Readiness](../PROLOGUE_REFACTOR_READINESS.md)
- [Assembly Emitter Testing](ASM_EMITTER_TESTING.md)

---

## Changelog

| Date | Change |
|------|--------|
| 2024-10-08 | Documented `-S` flag limitation with JIT test files |
