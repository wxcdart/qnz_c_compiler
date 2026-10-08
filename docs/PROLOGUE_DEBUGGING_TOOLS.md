# Prologue/Epilogue Debugging Tools

This directory contains Python tools for debugging and verifying prologue/epilogue code generation.

## Overview

These tools help identify and prevent prologue/epilogue bugs like the stack alignment issue we discovered:

- **Prologue Validator**: Validates frame size alignment and prologue/epilogue structure
- **Frame Layout Analyzer**: Analyzes frame size calculation code for issues
- **Stack Alignment Checker**: Verifies RSP alignment at all call sites
- **Regression Test Suite**: Tests prologue/epilogue for various scenarios

## Tools

### 1. Prologue Validator (`prologue_validator.py`)

Validates that prologue/epilogue code generation is correct.

**Features**:
- Checks frame size is 16-byte aligned
- Validates prologue sequence (push rbp, mov rbp, rsp, sub rsp)
- Validates epilogue sequence (leave, ret)
- Detects missing or incorrect instructions

**Usage**:
```bash
# Validate assembly file
python3 tools/prologue_validator.py --asm-file test.s

# Compile C file and validate
python3 tools/prologue_validator.py --test test.c

# Check all test files
python3 tools/prologue_validator.py --test test/jit/vla_test.c --verbose

# Generate test cases
python3 tools/prologue_validator.py --generate-tests /tmp/prologue_tests
```

**Example Output**:
```
Validating assembly file: test.s

Validation Results: 2/2 passed

[PASS] main
[PASS] main
```

---

### 2. Frame Layout Analyzer (`frame_layout_analyzer.py`)

Analyzes frame size calculation code for issues.

**Features**:
- Detects conditional frame size calculations (JIT vs Assembly)
- Identifies 8-byte vs 16-byte alignment issues
- Finds patch operations (frame size overwrites)
- Detects magic numbers without documentation

**Usage**:
```bash
# Analyze specific file
python3 tools/frame_layout_analyzer.py src/zero_cc/parser/ast_lowering.c

# Check all source files
python3 tools/frame_layout_analyzer.py --check-all

# Generate full report
python3 tools/frame_layout_analyzer.py --check-all --generate-report

# Show all findings
python3 tools/frame_layout_analyzer.py src/zero_cc/parser/ast_lowering.c --verbose
```

**Example Output**:
```
Analyzing src/zero_cc/parser/ast_lowering.c...
[WARNING] src/zero_cc/parser/ast_lowering.c:135
  Found 16-byte alignment calculation in zero_cc_lower_ast
  Suggestion: Verify this is the only frame size calculation

[WARNING] src/zero_cc/parser/ast_lowering.c:138
  Frame size patch operation detected
  Suggestion: Consider emitting prologue separately instead of patching
```

---

### 3. Stack Alignment Checker (`stack_alignment_checker.py`)

Verifies that RSP is 16-byte aligned before all CALL instructions.

**Features**:
- Tracks RSP mod 16 through function execution
- Validates alignment before all call sites
- Detects prologue alignment issues
- Reports specific line numbers for issues

**Usage**:
```bash
# Check assembly file
python3 tools/stack_alignment_checker.py --asm-file test.s

# Compile C file and check
python3 tools/stack_alignment_checker.py --test test.c

# Check all test files
python3 tools/stack_alignment_checker.py --check-all

# Show detailed output
python3 tools/stack_alignment_checker.py --test test.c --verbose
```

**Example Output**:
```
Stack Alignment Analysis for: test.s
============================================================
Functions analyzed: 2
Call sites analyzed: 5
Issues found: 0

Prologue Issues:
  ✅ All prologues have correct alignment

Call Site Issues:
  ✅ All call sites have correct alignment
```

---

### 4. Regression Test Suite (`prologue_regression_suite.py`)

Tests prologue/epilogue for various scenarios.

**Features**:
- Tests different frame sizes (0, 16, 64, 256 bytes)
- Tests nested function calls
- Tests various return types
- Tests edge cases (empty functions, recursion)
- Validates frame alignment for all cases

**Usage**:
```bash
# Run all tests
python3 tools/prologue_regression_suite.py --run-all

# Run specific category
python3 tools/prologue_regression_suite.py --test frame_sizes
python3 tools/prologue_regression_suite.py --test call_sites
python3 tools/prologue_regression_suite.py --test edge_cases

# Show detailed output
python3 tools/prologue_regression_suite.py --run-all --verbose

# List all test cases
python3 tools/prologue_regression_suite.py --list
```

**Example Output**:
```
✅ PASS no_stack_frame: Function with no stack frame
✅ PASS small_frame: Function with small stack frame (16 bytes)
✅ PASS medium_frame: Function with medium stack frame (64 bytes)
✅ PASS nested_calls: Function with nested function calls

============================================================
Test Summary: 10/10 passed
============================================================
```

---

## Workflow

### Before Making Changes

1. **Run baseline tests**:
```bash
python3 tools/prologue_regression_suite.py --run-all
python3 tools/stack_alignment_checker.py --check-all
```

2. **Analyze current code**:
```bash
python3 tools/frame_layout_analyzer.py --check-all --generate-report
```

### After Making Changes

1. **Validate generated assembly**:
```bash
python3 tools/prologue_validator.py --test test/jit/vla_test.c
```

2. **Check stack alignment**:
```bash
python3 tools/stack_alignment_checker.py --check-all
```

3. **Run regression tests**:
```bash
python3 tools/prologue_regression_suite.py --run-all --verbose
```

### Debugging a Specific Issue

1. **Compile the problematic code**:
```bash
./build/bin/zerocc -S -o /tmp/test.s test.c
```

2. **Validate the assembly**:
```bash
python3 tools/prologue_validator.py --asm-file /tmp/test.s --verbose
```

3. **Check stack alignment**:
```bash
python3 tools/stack_alignment_checker.py --asm-file /tmp/test.s --verbose
```

---

## Integration with CI

Add these checks to your CI pipeline:

```yaml
# Example GitHub Actions step
- name: Prologue/Epilogue Validation
  run: |
    python3 tools/prologue_regression_suite.py --run-all
    python3 tools/stack_alignment_checker.py --check-all
    python3 tools/frame_layout_analyzer.py --check-all
```

---

## Troubleshooting

### Issue: "Frame size not 16-byte aligned"

**Cause**: Frame size calculation uses wrong alignment (e.g., 8-byte instead of 16-byte)

**Fix**:
1. Check frame size calculation in `ast_lowering.c`
2. Ensure `(frame_size + 15) & ~15` is used for 16-byte alignment
3. Run `frame_layout_analyzer.py` to find the issue

### Issue: "RSP not aligned before call"

**Cause**: Prologue doesn't maintain 16-byte alignment

**Fix**:
1. Check prologue sequence in generated assembly
2. Verify `sub rsp, frame_size` uses 16-byte aligned frame_size
3. Run `stack_alignment_checker.py` to find the call site

### Issue: "Conditional frame size calculation detected"

**Cause**: Different frame size calculations for JIT vs Assembly

**Fix**:
1. Remove conditional logic in `ast_lowering.c`
2. Use single frame size calculation for all paths
3. Run `frame_layout_analyzer.py` to verify

---

## Future Enhancements

Planned improvements to these tools:

1. **Cross-ISA validation**: Support for aarch64 and riscv64 prologues
2. **Automated fix suggestions**: Generate code patches for common issues
3. **Performance analysis**: Measure prologue/epilogue overhead
4. **Integration with debugger**: Step through prologue generation
5. **Visualizer**: Graphical representation of stack layout

---

## References

- [System V AMD64 ABI](https://refspecs.linuxbase.org/elf/x86-64-abi-1.0.pdf) - Section 3.2.2 (Stack Layout)
- [Prologue/Epilogue Analysis](./PROLOGUE_EPILOGUE_ANALYSIS.md)
- [Improvement Plan](./PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md)
