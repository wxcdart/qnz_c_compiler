# Python Tools Summary

This document summarizes all the Python analysis tools created for debugging and verifying the zero_cc compiler.

## New Tools Created (This Session)

### 1. register_clobbering_analyzer.py
**Purpose**: Detect caller-saved register clobbering in AArch64 object files

**Usage**:
```bash
python3 tools/register_clobbering_analyzer.py --object test.o
python3 tools/register_clobbering_analyzer.py --object test.o --report report.txt
```

**Output**: Identifies patterns where:
- Registers are loaded before function calls
- Same registers are used after the call
- Potential for clobbering if not saved/restored

**Key Feature**: Analyzes raw disassembly without needing source code

### 2. register_allocator_analyzer.py
**Purpose**: Analyze register allocator decisions using IR and object files

**Usage**:
```bash
python3 tools/register_allocator_analyzer.py --ir ir_dump.txt --object test.o
```

**Output**: Detects:
- Caller-saved registers assigned to temps spanning calls
- Missing spill slots for preserved registers
- Register allocation inefficiencies

### 3. verify_caller_saved_fix.py
**Purpose**: Verify that caller-saved register fix is working

**Usage**:
```bash
python3 tools/verify_caller_saved_fix.py --object test.o
python3 tools/verify_caller_saved_fix.py --object test.o --detailed
```

**Output**: Checks for:
- Save/restore patterns around function calls
- Proper register preservation across calls
- PASSED/FAILED status

## Related Tools Created Earlier

### IR Analysis Tools

- **ir_analyzer.py** - Comprehensive IR analysis
  - CFG construction
  - Flag dependency tracking
  - Optimization opportunity detection

- **flag_flow_analyzer.py** - Deep flag dependency analysis
  - Producer-consumer tracking
  - CSE/DCE candidate detection

### AST Lowering Analysis

- **ast_lowering_analyzer.py** - ISA assumption detection
  - Scans for ISA-specific patterns (x86_64, AArch64)
  - Identifies critical issues in AST lowering layer
  - Generates reports and JSON output

### Backend Interface Analysis

- **backend_interface_analyzer.py** - Backend coverage verification
  - Checks which functions are implemented
  - Identifies ISA-specific functions
  - Tracks implementation gaps

- **design_verification_suite.py** - Comprehensive design verification
  - Runs all verification checks
  - Generates HTML/JSON reports
  - Checks build system, IR layer, backend interface

### Stack Frame Analysis

- **stack_frame_layout_analyzer.py** - Cross-ISA stack layout analysis
  - Compares stack frames between ISAs
  - Detects layout inconsistencies
  - Verifies offset conventions

## How to Use These Tools

### 1. Find Register Clobbering Issues
```bash
./build/bin/zerocc -c test.c -o test.o --target aarch64
python3 tools/register_clobbering_analyzer.py --object test.o
```

### 2. Analyze IR and Compare with Assembly
```bash
./build/bin/zerocc -c test.c -o test.o --target aarch64 -dump-ir 2> ir_dump.txt
python3 tools/ir_analyzer.py --dump ir_dump.txt --analyze all
```

### 3. Verify Cross-ISA Consistency
```bash
python3 tools/design_verification_suite.py --full --output report.html
```

### 4. Analyze AST Lowering Issues
```bash
python3 tools/ast_lowering_analyzer.py --analyze-dir src/zero_cc/parser/ --json issues.json
```

## Integration with Compiler

The tools are designed to work with compiler output:

1. **IR Dump** (-dump-ir flag):
   - Prints IR to stderr
   - Can be captured: `2> ir_dump.txt`
   - Used by: ir_analyzer.py, flag_flow_analyzer.py

2. **Object Files** (.o):
   - Generated with: `./build/bin/zerocc -c test.c -o test.o`
   - Analyzed by: register_clobbering_analyzer.py, verify_caller_saved_fix.py

3. **Source Code**:
   - Scanned by: ast_lowering_analyzer.py, backend_interface_analyzer.py

## Tool Features

### Common Features
- **JSON Output**: Most tools support `--json` for integration with other tools
- **Report Generation**: Formatted text reports with recommendations
- **Verbose Mode**: `-v` or `--verbose` for detailed output
- **Selective Analysis**: Filter by function, ISA, or type

### Analysis Types
- **Pattern Detection**: Regex-based pattern matching in disassembly/code
- **Graph Analysis**: CFG, dependency graphs, dominance analysis
- **Statistical Analysis**: Coverage, missing implementations
- **Cross-ISA Comparison**: Compare patterns across x86-64, AArch64, RISC-V

## Workflow Recommendations

### For Debugging
1. Run `register_clobbering_analyzer.py` to find immediate issues
2. Use `verify_caller_saved_fix.py` to check fix status
3. Use `ir_analyzer.py` to understand IR structure

### For Architecture Verification
1. Run `ast_lowering_analyzer.py` to find ISA assumptions
2. Run `design_verification_suite.py` for comprehensive check
3. Run `backend_interface_analyzer.py` to verify implementations

### For Optimization
1. Use `flag_flow_analyzer.py` to find flag-related optimizations
2. Use `ir_analyzer.py` to identify CSE/DCE opportunities

## Future Tool Ideas

1. **Register Usage Tracker**: Track register usage per function
2. **Spill Analyzer**: Analyze spill patterns and efficiency
3. **Call Graph Analyzer**: Build and analyze function call graphs
4. **Memory Access Analyzer**: Track stack and memory access patterns
5. **Instruction Pattern Analyzer**: Find common instruction sequences for optimization

## Tool Maintenance

All tools include:
- Docstrings with usage examples
- Error handling with helpful messages
- Consistent output formatting
- Cross-platform compatibility (tested on Linux)

For issues or improvements, update the docstring and add tests as needed.
