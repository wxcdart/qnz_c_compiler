# VLA Debugging Tools

A comprehensive toolset for debugging and verifying VLA (Variable Length Array) implementation in ZeroCC.

## Tools Overview

### 1. `ir_diff.py` - IR Analysis Tool

Compares IR before and after optimizations to identify VLA patterns.

**Usage:**
```bash
# Analyze IR file
python3 ir_diff.py --before ir_before.txt --after ir_after.txt

# Dump IR from compiler
python3 ir_diff.py --dump-ir ../build/bin/zerocc test.c

# Generate report
python3 ir_diff.py --before ir.txt --output report.txt
```

**Features:**
- Parse IR into structured format
- Track VLA allocations (OP_VLA_ALLOC)
- Identify single-scope vs multiple VLA
- Compare IR before/after optimizations
- Generate compliance report

### 2. `vla_stack_track.py` - Stack Tracker

Tracks VLA stack allocations and deallocations to detect stack imbalance.

**Usage:**
```bash
# Analyze assembly
python3 vla_stack_track.py --asm test.s

# Verify compliance
python3 vla_stack_track.py --asm test.s --verify

# Generate report
python3 vla_stack_track.py --asm test.s --output stack_report.txt
```

**Features:**
- Parse assembly for stack operations
- Track VLA allocation/deallocation
- Detect stack imbalance
- Identify nested VLA violations
- Generate compliance report

### 3. `verify_opt.py` - Optimization Verifier

Verifies VLA optimizations are correct using CFG analysis.

**Usage:**
```bash
# Verify assembly
python3 verify_opt.py --asm test.s

# Verify with compiler
python3 verify_opt.py --test test.c --compile ../build/bin/zerocc

# Check hybrid rules
python3 verify_opt.py --asm test.s --verify-hybrid
```

**Features:**
- Build CFG from assembly
- Verify VLA cleanup on all paths
- Check stack balance at returns
- Verify hybrid VLA rules
- Generate detailed report

### 4. `check_asm.py` - Assembly Pattern Checker

Checks generated assembly for VLA patterns and ABI compliance.

**Usage:**
```bash
# Check VLA patterns
python3 check_asm.py test.s --check-vla

# Check ABI compliance
python3 check_asm.py test.s --check-abi

# Compare two assemblies
python3 check_asm.py --diff before.s after.s
```

**Features:**
- Parse assembly for VLA patterns
- Check prologue/epilogue
- Verify callee-saved registers
- Check stack alignment
- Compare assembly changes

### 5. `test_vla.py` - Test Suite

Comprehensive test suite for VLA implementation.

**Usage:**
```bash
# Run all tests
python3 test_vla.py --all --compiler ../build/bin/zerocc

# Run specific test
python3 test_vla.py --test single_vla_1d --compiler ../build/bin/zerocc

# Generate report
python3 test_vla.py --all --output test_report.txt
```

**Test Cases:**
- Single-scope runtime VLA
- 2D VLA
- VLA with early return
- Multiple VLA (should fail in hybrid mode)
- Comptime VLA
- VLA in loop (should fail)
- VLA in nested block (should fail)

## Hybrid VLA Rules

The tools verify compliance with these hybrid VLA rules:

### ✅ Supported

1. **Single-scope runtime VLA**
   ```c
   void foo(int n) {
       int arr[n];  // One VLA per function
       arr[0] = 42;
   }
   ```

2. **Comptime VLA (regular arrays)**
   ```c
   void foo(void) {
       enum { N = 10 };
       int arr1[N];  // Regular stack array
       int arr2[20]; // Multiple OK
   }
   ```

### ❌ Rejected

1. **Multiple runtime VLA**
   ```c
   void foo(int n, int m) {
       int arr1[n];  // First VLA - OK
       int arr2[m];  // Second VLA - ERROR
   }
   ```

2. **VLA in loops**
   ```c
   void foo(int n) {
       for (int i = 0; i < 10; i++) {
           int arr[n];  // ERROR
       }
   }
   ```

3. **VLA in nested blocks**
   ```c
   void foo(int n) {
       if (n > 0) {
           int arr[n];  // ERROR
       }
   }
   ```

## Workflow

### Development Workflow

1. **Write VLA code**
   ```c
   void process(int n) {
       int arr[n];
       // ... use arr
   }
   ```

2. **Compile and dump IR**
   ```bash
   python3 ir_diff.py --dump-ir ../build/bin/zerocc test.c -o /tmp/ir.txt
   ```

3. **Analyze IR**
   ```bash
   python3 ir_diff.py --before /tmp/ir.txt --output ir_report.txt
   ```

4. **Generate assembly**
   ```bash
   ../build/bin/zerocc -S test.c -o test.s
   ```

5. **Check assembly**
   ```bash
   python3 check_asm.py test.s --check-vla
   python3 vla_stack_track.py --asm test.s --verify
   python3 verify_opt.py --asm test.s --verify-hybrid
   ```

6. **Run test suite**
   ```bash
   python3 test_vla.py --all --compiler ../build/bin/zerocc
   ```

### CI/CD Integration

```bash
#!/bin/bash
# Run all VLA checks

# Compile test
../build/bin/zerocc -S test.c -o test.s

# Check patterns
python3 check_asm.py test.s --check-vla || exit 1

# Track stack
python3 vla_stack_track.py --asm test.s --verify || exit 1

# Verify optimizations
python3 verify_opt.py --asm test.s --verify-hybrid || exit 1

# Run test suite
python3 test_vla.py --all --compiler ../build/bin/zerocc || exit 1

echo "All VLA checks passed!"
```

## Output Examples

### IR Diff Report
```
============================================================
VLA IR Analysis Report
============================================================

Total functions: 5
Functions with VLA: 2
Total VLA allocations: 2

VLA Pattern Analysis:
  - Single-scope VLA: 2
  - Multiple VLA per function: 0
  - Nested VLA: 0
  - VLA in loops: 0

Hybrid VLA Compliance:
  Status: ✅ PASS
============================================================
```

### Stack Track Report
```
======================================================================
VLA Stack Trace Analysis Report
======================================================================

Total functions analyzed: 5
Functions with VLA: 2
  - Runtime VLA: 2
  - Comptime VLA: 0

Issues Found:
  - Stack unbalanced: 0
  - Nested VLA: 0

Hybrid VLA Compliance:
  Status: ✅ PASS
======================================================================
```

### Test Suite Report
```
======================================================================
VLA Test Suite Report
======================================================================

Total tests: 8
Passed: 6
Failed: 2

By VLA Type:
  runtime: 4/4
  comptime: 1/1
  hybrid: 1/3

Failed Tests:
  ❌ multiple_vla
  ❌ vla_in_loop
  ❌ vla_in_block

======================================================================
```

## Troubleshooting

### Common Issues

1. **"VLA allocated but not cleaned up"**
   - Check that function has proper epilogue
   - Verify `mov rsp, [rbp-x]` before `leave; ret`

2. **"Stack unbalanced"**
   - Check prologue/epilogue symmetry
   - Verify all `sub rsp` have matching `add rsp`

3. **"Multiple VLA per function"**
   - This is expected in hybrid mode
   - Refactor to use single VLA or comptime arrays

### Debug Mode

Enable verbose output:
```bash
python3 check_asm.py test.s -v
python3 vla_stack_track.py --asm test.s -v
```

## Future Enhancements

- [ ] Add LLVM IR support
- [ ] Support for VLA with struct members
- [ ] Performance benchmarking
- [ ] Integration with test framework
- [ ] Visual CFG generation

## License

Part of ZeroCC compiler project.
