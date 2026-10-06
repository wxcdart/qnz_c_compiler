# ZeroCC Debugging Tools

This directory contains Python tools for debugging and verifying the ZeroCC compiler,
especially for cross-ISA backend development (x86-64, AArch64, RISC-V64).

## Tool Summary

| Tool | Purpose | Dependencies |
|------|---------|--------------|
| `aarch64_backend_debugger.py` | Detects x86 callee-save patterns in AArch64 objects | Standard library |
| `find_callee_save_calls.py` | Finds all calls to `emit_callee_save_stores` in source | Standard library |
| `verify_prologue_bytes.py` | Verifies prologue bytes match expected patterns | pyelftools (optional) |
| `backend_call_tracker.py` | Tracks which backend functions are called | Standard library |
| `prologue_verifier.py` | Verifies prologue/epilogue patterns per ISA | Standard library |
| `cross_isa_comparator.py` | Compiles same source for multiple ISAs | Standard library |
| `qemu_test_runner.py` | Runs tests under QEMU with timeout | QEMU binaries |
| `stack_offset_analyzer.py` | Analyzes stack frame offsets | Standard library |
| `backend_coverage.py` | Tracks implemented vs missing backend functions | Standard library |
| `backend_stub_generator.py` | Generates stub implementations for missing functions | Standard library |
| `backend_impl_tracker.py` | Implementation checklist & progress tracker | Standard library |
| `backend_test_harness.py` | Generates minimal test cases per function | Standard library |
| `cross_isa_test_gen.py` | Generates ISA-specific test cases | Standard library |
| `abi_verify.py` | Validates ABI compliance across architectures | Standard library |
| `segfault_debugger.py` | Debug compiler segfaults with gdb/valgrind | gdb/valgrind |
| `backend_function_validator.py` | Validates backend function registration | Standard library |
| `backend_init_checker.py` | Checks backend initialization patterns | Standard library |
| `compiler_crash_analyzer.py` | Systematic crash pattern analysis | Standard library |

## Quick Start

### 1. Check for Stray x86 Callee-Save Calls

```bash
python3 tools/find_callee_save_calls.py
```

This should only show calls in x86-specific code paths.

### 2. Verify AArch64 Objects Don't Contain x86 Patterns

```bash
python3 tools/aarch64_backend_debugger.py --target aarch64 -c test/aot/hello_world.c -o /tmp/test.o
```

Exit code 0 = clean, exit code 1 = x86 pattern detected.

### 3. Compare Compilation Across ISAs

```bash
python3 tools/cross_isa_comparator.py test/aot/hello_world.c --isas x86_64 aarch64
```

### 4. Run Tests Under QEMU

```bash
python3 tools/qemu_test_runner.py --isa aarch64 test/aot/
```

### 5. Analyze Stack Offsets

```bash
python3 tools/stack_offset_analyzer.py /tmp/test.o --detect-overlap
```

### 6. Debug Segfaults

```bash
python3 tools/segfault_debugger.py --target aarch64 test/aot/hello_world.c
```

Runs the compiler under gdb and extracts the backtrace.

### 7. Validate Backend Function Registration

```bash
python3 tools/backend_function_validator.py --isa aarch64
```

### 8. Check Backend Initialization

```bash
python3 tools/backend_init_checker.py
```

### 9. Analyze Crash Patterns

```bash
python3 tools/compiler_crash_analyzer.py --isa aarch64 test/aot/
```

### 10. Track Backend Implementation Coverage

```bash
python3 tools/backend_coverage.py
```

## CI Integration

Add these checks to your CI pipeline:

```yaml
# Example GitHub Actions step
- name: Check for stray callee-save calls
  run: python3 tools/find_callee_save_calls.py | grep -v "ISA_X86_64" && exit 1 || exit 0

- name: Verify AArch64 backend
  run: |
    for test in test/aot/*.c; do
      python3 tools/aarch64_backend_debugger.py --target aarch64 -c "$test" -o /tmp/test.o || exit 1
    done

- name: Verify prologue bytes
  run: python3 tools/prologue_verifier.py --isa aarch64 /tmp/test.o
```

## Troubleshooting

### "ModuleNotFoundError: No module named 'elftools'"

Some tools use `pyelftools` for ELF parsing. Install it if needed:

```bash
pip install pyelftools
```

Most tools fall back to `objdump` if pyelftools is not available.

### QEMU Tests Timeout

Increase the timeout or check for infinite loops in generated code:

```bash
python3 tools/qemu_test_runner.py --isa aarch64 --timeout 60 test/aot/
```

### Object File Architecture Detection

If `objdump` fails to detect architecture, use the cross-compiler version:

```bash
aarch64-linux-gnu-objdump -d /tmp/test.o
```

## Adding New Tools

When adding new tools:
1. Add shebang: `#!/usr/bin/env python3`
2. Make executable: `chmod +x tools/your_tool.py`
3. Add to this README
4. Prefer standard library over external dependencies
5. Use `objdump`/`readelf` instead of Python libraries when possible

## Related Files

- Backend implementation: `src/zero_cc/isa/*.c`
- ISA backend interface: `src/zero_cc/isa/isa_backend.h`
- IR lowering: `src/zero_cc/parser/ast_lowering*.c`
- Coverage tracking: `tools/backend_coverage.py`
