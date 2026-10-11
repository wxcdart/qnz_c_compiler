# Struct Tools for ZeroCC - Phase 2

This directory contains Python tools for testing and verifying struct argument passing and return value handling according to the System V AMD64 ABI specification.

## Tools Overview

### 1. struct_abi_verifier.py
Verifies that struct argument passing and return values follow the System V AMD64 ABI specification.

**Usage:**
```bash
# Verify a test file
python3 tools/struct_abi_verifier.py --test test/jit/struct_byval_test.c

# Run built-in test cases
python3 tools/struct_abi_verifier.py

# Comprehensive mode
python3 tools/struct_abi_verifier.py --mode comprehensive
```

**What it checks:**
- 1-chunk structs (1-8 bytes): passed in RDI, returned in RAX
- 2-chunk structs (9-16 bytes): passed in RDI:RSI, returned in RAX:RDX
- 3+ chunk structs (>16 bytes): passed/returned via hidden pointer

### 2. struct_return_verifier.py
Specialized tool for verifying struct return value handling.

**Usage:**
```bash
# Verify return values
python3 tools/struct_return_verifier.py --verify-all

# Generate and run test cases
python3 tools/struct_return_verifier.py --generate-tests

# Verbose output
python3 tools/struct_return_verifier.py --verbose
```

**What it checks:**
- Return register usage (RAX for 1-chunk, RAX:RDX for 2-chunk)
- Hidden pointer usage for large structs
- Return value setup instructions

### 3. struct_test_generator.py
Generates comprehensive test cases for struct handling.

**Usage:**
```bash
# Generate comprehensive tests
python3 tools/struct_test_generator.py --output test_structs_full.c

# Generate argument passing tests only
python3 tools/struct_test_generator.py --mode args --output test_struct_args.c

# Generate return value tests only
python3 tools/struct_test_generator.py --mode returns --output test_struct_returns.c

# Generate all test files
python3 tools/struct_test_generator.py --mode all
```

**Test modes:**
- `args`: Struct argument passing tests
- `returns`: Struct return value tests
- `comprehensive`: All struct sizes (1-4 chunks)
- `mixed`: Mixed argument types (struct + int + struct)
- `nested`: Nested struct tests
- `all`: Generate all test files

### 4. struct_asm_analyzer.py
Analyzes generated assembly to verify correct struct handling.

**Usage:**
```bash
# Analyze assembly file
python3 tools/struct_asm_analyzer.py --asm file.s

# Compile and analyze source
python3 tools/struct_asm_analyzer.py --source test.c

# Full analysis with built-in tests
python3 tools/struct_asm_analyzer.py --full-analysis

# Verbose output
python3 tools/struct_asm_analyzer.py --verbose
```

**What it analyzes:**
- Struct definitions and sizes
- Function argument register usage
- Return value register usage
- Hidden pointer detection
- Struct member access patterns

## System V AMD64 ABI Struct Rules

### Argument Passing
| Size | Chunks | Passing Method |
|------|--------|----------------|
| 1-8 bytes | 1 | Register (RDI) |
| 9-16 bytes | 2 | Registers (RDI:RSI) |
| >16 bytes | 3+ | Hidden pointer (first arg) |

### Return Values
| Size | Chunks | Return Method |
|------|--------|---------------|
| 1-8 bytes | 1 | RAX |
| 9-16 bytes | 2 | RAX:RDX |
| >16 bytes | 3+ | Hidden pointer (first arg) |

## Test Case Examples

### 1-Chunk Struct (4-8 bytes)
```c
typedef struct { int x; int y; } Pair8;

Pair8 make_pair8(int x, int y) {
    return (Pair8){x, y};  // Returns in RAX
}

int take_pair8(Pair8 p) {
    return p.x + p.y;  // Passed in RDI
}
```

### 2-Chunk Struct (9-16 bytes)
```c
typedef struct { int a; int b; int c; } Triple12;

Triple12 make_triple12(int a, int b, int c) {
    return (Triple12){a, b, c};  // Returns in RAX:RDX
}

int take_triple12(Triple12 t) {
    return t.a + t.b + t.c;  // Passed in RDI:RSI
}
```

### 3-Chunk Struct (>16 bytes)
```c
typedef struct { int a; int b; int c; int d; int e; } Pent20;

Pent20 make_pent20(int a, int b, int c, int d, int e) {
    return (Pent20){a, b, c, d, e};  // Returns via hidden pointer
}

int take_pent20(Pent20 p) {
    return p.a + p.b + p.c + p.d + p.e;  // Passed via hidden pointer
}
```

## Workflow for Phase 2

1. **Generate Tests**: Use `struct_test_generator.py` to create test cases
   ```bash
   python3 tools/struct_test_generator.py --mode comprehensive --output test/jit/struct_comprehensive.c
   ```

2. **Compile and Verify**: Use `struct_abi_verifier.py` to check ABI compliance
   ```bash
   python3 tools/struct_abi_verifier.py --test test/jit/struct_comprehensive.c
   ```

3. **Analyze Assembly**: Use `struct_asm_analyzer.py` to inspect generated code
   ```bash
   ./build/bin/zerocc -S test/jit/struct_comprehensive.c -o - | \
     python3 tools/struct_asm_analyzer.py --asm -
   ```

4. **Verify Returns**: Use `struct_return_verifier.py` for return value checks
   ```bash
   python3 tools/struct_return_verifier.py --verify-all
   ```

## Integration with Test Suite

Generated tests can be added to `test/jit/`:
```bash
python3 tools/struct_test_generator.py --mode comprehensive -o test/jit/struct_comprehensive.c
make test  # Run all tests including new struct tests
```

## Expected Output

### struct_abi_verifier.py
```
======================================================================
STRUCT ABI VERIFICATION RESULTS
======================================================================

[PASS] make_pair8() -> Pair8
  ✓ 1-chunk struct return: rax register

[PASS] take_pair8(Pair8)
  ✓ 1-chunk struct arg: rdi register

======================================================================
SUMMARY: 2 passed, 0 failed
======================================================================
```

### struct_asm_analyzer.py
```
======================================================================
STRUCT ASSEMBLY ANALYSIS
======================================================================

[Struct Definitions]
  Pair8: 8 bytes (1 chunk(s)) -> registers
  Triple12: 12 bytes (2 chunk(s)) -> registers

[Function Calls]

  make_pair8:
    Return register: rax

  take_pair8:
    Arg registers: rdi
    Return register: rax

======================================================================
```

## Troubleshooting

### Issue: "Compilation failed"
- Ensure `build/bin/zerocc` exists and is executable
- Check that the source file compiles without errors

### Issue: "Could not find function in assembly"
- Verify the function is not optimized away
- Try compiling with `-O0` to preserve function

### Issue: "Expected struct in RDI, but RDI not used"
- Check if struct is being passed via hidden pointer
- Verify struct size is correct (1-8 bytes for RDI)

## Dependencies

- Python 3.6+
- ZeroCC compiler (`build/bin/zerocc`)
- Standard Unix tools (subprocess, tempfile, re)

## Future Enhancements

- [ ] Add AArch64 and RISC-V ABI support
- [ ] Support for floating-point struct members
- [ ] Support for nested struct analysis
- [ ] Automated test generation for edge cases
- [ ] Integration with CI/CD pipeline
