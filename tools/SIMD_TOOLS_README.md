# SIMD Vectorization Debugging Tools

This directory contains Python tools for debugging and verifying SIMD vectorization in ZeroCC.

## Tools

### 1. simd_instruction_validator.py
Validates SSE2/SIMD instruction generation for x86-64 backend.

```bash
# Validate assembly output
python3 simd_instruction_validator.py output.s

# Validate IR dump
python3 simd_instruction_validator.py --check-ir ir_dump.txt
```

**Features:**
- Validates XMM register usage (xmm0-xmm15)
- Checks SSE2 instruction opcodes
- Detects invalid SIMD instruction sequences
- Generates validation report

### 2. simd_coverage_tracker.py
Tracks SIMD instruction implementation and test coverage.

```bash
# Check backend implementation
python3 simd_coverage_tracker.py --check-backend src/zero_cc/isa/x86_64.c

# Analyze test coverage
python3 simd_coverage_tracker.py --analyze-tests test/

# Generate report
python3 simd_coverage_tracker.py --report -o coverage_report.txt
```

**Features:**
- Tracks implemented vs missing SIMD instructions
- Categorizes by type (arithmetic, comparison, data movement, etc.)
- Analyzes test file coverage
- Generates detailed coverage reports

### 3. vectorization_analyzer.py
Analyzes C code for SIMD vectorization opportunities.

```bash
# Analyze single file
python3 vectorization_analyzer.py source.c

# Analyze directory
python3 vectorization_analyzer.py --analyze-dir src/

# JSON output
python3 vectorization_analyzer.py source.c --report-json > report.json
```

**Features:**
- Detects loops in C code
- Identifies array accesses and reductions
- Checks for data dependencies
- Determines vectorizability
- Estimates potential speedup
- Generates recommendations

## Usage Example

```bash
# 1. Analyze code for vectorization opportunities
python3 vectorization_analyzer.py test/jit/vla_test.c

# 2. Compile with SIMD
./build/bin/zerocc -O3 test/jit/vla_test.c -o test_simd

# 3. Check SIMD instruction coverage
python3 simd_coverage_tracker.py --check-backend src/zero_cc/isa/x86_64.c

# 4. Validate generated assembly
./build/bin/zerocc -S test/jit/vla_test.c -o test.s
python3 simd_instruction_validator.py test.s
```

## Planned Tools (Sprint 10)

- xmm_register_debugger.py - Debug XMM register state
- simd_ir_validator.py - Validate SIMD IR opcodes
- auto_vectorization_tester.py - Test vectorization heuristics
- simd_asm_generator.py - Generate SIMD assembly test cases
- xmm_abi_verifier.py - Verify XMM register ABI compliance
- simd_benchmark_runner.py - Measure SIMD performance
- simd_regression_suite.py - SIMD regression testing

## Dependencies

- Python 3.6+
- No external dependencies required

## License

Same as ZeroCC project.
