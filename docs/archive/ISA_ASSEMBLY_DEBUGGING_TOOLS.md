# ISA-Specific Assembly Debugging Tools

## Overview

This document describes the Python tools available for debugging and verifying ISA-specific assembly emission fixes (Option A: Full Backend-Driven Architecture).

## Current Tool Inventory

### ✅ Available Tools (114 total)

#### Assembly Emitter Tools (3 tools)

| Tool | Purpose | ISA Support | Status |
|------|---------|-------------|--------|
| `asm_emitter_validator.py` | Validates `-S` output is text assembly, not binary ELF | x86-64 only | ✅ Working |
| `asm_instruction_checker.py` | Checks SSE2/SIMD instruction coverage | x86-64 only | ✅ Working |
| `asm_reference_generator.py` | Generates expected x86-64 assembly patterns | x86-64 only | ✅ Working |
| `asm_diff.py` | Compares assembly outputs | x86-64 only | ✅ Working |

#### Cross-ISA Tools (4 tools)

| Tool | Purpose | ISA Support | Status |
|------|---------|-------------|--------|
| `cross_isa_comparator.py` | Compiles for multiple ISAs, compares results | x86-64, AArch64, RISC-V | ✅ Working |
| `isa_diff.py` | Compares codegen across ISAs (size, relocations) | x86-64, AArch64, RISC-V | ✅ Working |
| `cross_isa_test_gen.py` | Generates cross-ISA test cases | All ISAs | ✅ Working |
| `backend_coverage.py` | Tracks which backend functions are implemented | All ISAs | ✅ Working |

#### AArch64 Tools (3 tools)

| Tool | Purpose | ISA Support | Status |
|------|---------|-------------|--------|
| `aarch64_backend_debugger.py` | Detects x86-64 patterns in AArch64 output | AArch64 | ✅ Working |
| `aarch64_backend_helper.py` | AArch64 backend development helpers | AArch64 | ✅ Working |
| `aarch64_verify.py` | Verifies AArch64 codegen correctness | AArch64 | ✅ Working |

#### RISC-V Tools (1 tool)

| Tool | Purpose | ISA Support | Status |
|------|---------|-------------|--------|
| `riscv_backend_helper.py` | RISC-V backend development helpers | RISC-V | ✅ Working |

#### Backend Tools (10 tools)

| Tool | Purpose | ISA Support | Status |
|------|---------|-------------|--------|
| `backend_call_tracker.py` | Tracks backend function calls | All ISAs | ✅ Working |
| `backend_function_validator.py` | Validates backend function signatures | All ISAs | ✅ Working |
| `backend_impl_tracker.py` | Tracks backend implementation status | All ISAs | ✅ Working |
| `backend_init_checker.py` | Checks backend initialization | All ISAs | ✅ Working |
| `backend_interface_analyzer.py` | Analyzes backend interface | All ISAs | ✅ Working |
| `backend_stub_generator.py` | Generates backend stubs | All ISAs | ✅ Working |
| `backend_test_harness.py` | Backend test harness | All ISAs | ✅ Working |
| `callee_saved_analyzer.py` | Analyzes callee-saved register handling | All ISAs | ✅ Working |
| `verify_callee_save_fix.py` | Verifies callee-save fix | All ISAs | ✅ Working |
| `verify_caller_saved_fix.py` | Verifies caller-saved fix | All ISAs | ✅ Working |

#### Prologue/Epilogue Tools (4 tools)

| Tool | Purpose | ISA Support | Status |
|------|---------|-------------|--------|
| `prologue_validator.py` | Validates prologue/epilogue structure | x86-64 only | ✅ Working |
| `frame_layout_analyzer.py` | Analyzes frame size calculations | All ISAs | ✅ Working |
| `stack_alignment_checker.py` | Verifies stack alignment | x86-64 only | ✅ Working |
| `prologue_regression_suite.py` | Regression tests for prologue/epilogue | x86-64 only | ✅ Working |

#### SIMD Tools (8 tools)

| Tool | Purpose | ISA Support | Status |
|------|---------|-------------|--------|
| `simd_asm_generator.py` | Generates SIMD assembly test cases | x86-64 only | ✅ Working |
| `simd_benchmark_runner.py` | Runs SIMD benchmarks | x86-64 only | ✅ Working |
| `simd_coverage_tracker.py` | Tracks SIMD instruction coverage | x86-64 only | ✅ Working |
| `simd_instruction_validator.py` | Validates SIMD instructions | x86-64 only | ✅ Working |
| `simd_ir_validator.py` | Validates SIMD IR | All ISAs | ✅ Working |
| `simd_regression_suite.py` | SIMD regression tests | x86-64 only | ✅ Working |
| `vectorization_analyzer.py` | Analyzes auto-vectorization | All ISAs | ✅ Working |
| `auto_vectorization_tester.py` | Tests auto-vectorization | All ISAs | ✅ Working |

---

## Gap Analysis for Option A (ISA-Specific Assembly Emission)

### ❌ Missing Tools (Need to Build)

| Tool | Purpose | Priority | Effort |
|------|---------|----------|--------|
| `aarch64_asm_validator.py` | Validates AArch64 assembly output | 🔴 High | 4 hours |
| `riscv64_asm_validator.py` | Validates RISC-V assembly output | 🔴 High | 4 hours |
| `isa_asm_comparator.py` | Compares ISA-specific assembly patterns | 🟡 Medium | 6 hours |
| `asm_opcode_coverage.py` | Tracks which opcodes are implemented per ISA | 🟡 Medium | 4 hours |
| `abi_asm_verifier.py` | Verifies ABI-compliant assembly emission | 🟡 Medium | 6 hours |
| `asm_regression_suite.py` | Comprehensive assembly regression tests | 🟢 Low | 8 hours |

### ⚠️ Existing Tools Needing Updates

| Tool | Current Limitation | Required Update |
|------|-------------------|-----------------|
| `asm_emitter_validator.py` | x86-64 only (checks for `movq`, `push`, etc.) | Add AArch64/RISC-V pattern detection |
| `asm_instruction_checker.py` | x86-64 SSE2 only | Add AArch64 NEON, RISC-V V extension |
| `prologue_validator.py` | x86-64 only (`push rbp`, `sub rsp`) | Add AArch64 (`stp x29`), RISC-V (`addi sp`) |
| `stack_alignment_checker.py` | x86-64 only (RSP % 16) | Add AArch64 (SP % 16), RISC-V (SP % 16) |

---

## Recommended Tool Development Plan

### Phase 1: Core Validation (8 hours)

```bash
# 1. AArch64 Assembly Validator (4 hours)
python3 tools/aarch64_asm_validator.py --asm-file test_aarch64.s
# Features:
# - Validates AArch64 instruction syntax
# - Checks for x86-64 leakage
# - Verifies register naming (x0-x30, not %rax)
# - Validates prologue/epilogue patterns

# 2. RISC-V Assembly Validator (4 hours)
python3 tools/riscv64_asm_validator.py --asm-file test_riscv.s
# Features:
# - Validates RISC-V instruction syntax
# - Checks for x86-64 leakage
# - Verifies register naming (a0-a7, s0-s11, not %rax)
# - Validates prologue/epilogue patterns
```

### Phase 2: Cross-ISA Comparison (6 hours)

```bash
# 3. ISA Assembly Comparator (6 hours)
python3 tools/isa_asm_comparator.py test.c --isas x86_64 aarch64 riscv64
# Features:
# - Compares assembly patterns across ISAs
# - Detects ISA-specific bugs
# - Reports instruction count differences
# - Validates semantic equivalence
```

### Phase 3: Coverage Tracking (4 hours)

```bash
# 4. Opcode Coverage Tracker (4 hours)
python3 tools/asm_opcode_coverage.py --target x86_64
python3 tools/asm_opcode_coverage.py --target aarch64
python3 tools/asm_opcode_coverage.py --target riscv64
# Features:
# - Tracks which IR opcodes have assembly emission
# - Per-ISA implementation status
# - Missing opcode detection
```

### Phase 4: ABI Verification (6 hours)

```bash
# 5. ABI Assembly Verifier (6 hours)
python3 tools/abi_asm_verifier.py --test test.c --target aarch64
# Features:
# - Validates calling convention compliance
# - Checks argument passing (x0-x7 for AArch64)
# - Verifies stack alignment
# - Validates return value handling
```

---

## Tool Usage Examples

### Current Tools (Working)

```bash
# Validate x86-64 assembly output
python3 tools/asm_emitter_validator.py --input test/jit/vla_test.c

# Check SIMD instruction coverage
python3 tools/asm_instruction_checker.py test/jit/vla_test.c

# Compare across ISAs (binary only, not assembly)
python3 tools/cross_isa_comparator.py test/jit/vla_test.c

# Detect x86-64 leakage in AArch64
python3 tools/aarch64_backend_debugger.py -c test.c -o /tmp/test.o --target aarch64

# Track backend implementation status
python3 tools/backend_coverage.py
```

### Future Tools (After Phase 1-2)

```bash
# Validate AArch64 assembly output (new)
python3 tools/aarch64_asm_validator.py --input test.c --target aarch64

# Validate RISC-V assembly output (new)
python3 tools/riscv64_asm_validator.py --input test.c --target riscv64

# Compare assembly patterns across ISAs (new)
python3 tools/isa_asm_comparator.py test.c --isas x86_64 aarch64 riscv64

# Check opcode coverage per ISA (new)
python3 tools/asm_opcode_coverage.py --target aarch64 --verbose

# Verify ABI compliance (new)
python3 tools/abi_asm_verifier.py --test test.c --target aarch64 --abi systemv
```

---

## Summary

### ✅ What We Have

- **114 Python tools** total
- **4 cross-ISA tools** for comparing codegen
- **3 AArch64-specific tools** for debugging
- **1 RISC-V tool** for debugging
- **10 backend tools** for tracking implementation
- **4 prologue/epilogue tools** (x86-64 focused)
- **8 SIMD tools** (x86-64 focused)

### ❌ What We Need for Option A

- **5 new tools** for ISA-specific assembly validation (~24 hours)
- **4 existing tools** need updates for multi-ISA support (~8 hours)

### 🎯 Recommendation

**Proceed with Option A** - we have sufficient tooling foundation. The missing tools can be built incrementally:

1. **Week 1**: Build AArch64/RISC-V validators (8 hours)
2. **Week 2**: Build cross-ISA comparator (6 hours)
3. **Week 3**: Build coverage tracker (4 hours)
4. **Week 4**: Build ABI verifier (6 hours)

**Total**: 24 hours (3 days) - can be done in parallel with assembly emission implementation.
