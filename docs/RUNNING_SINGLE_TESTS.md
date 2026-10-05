# Running Single Tests

## Overview

Run individual tests with **fresh compilation** to ensure no stale object files.

## Key Point: No Explicit Targets Needed!

The pattern rule `test-%` automatically handles **all tests** - you don't need to add new targets when creating new tests.

## Usage

### Method 1: Make Shortcut (Recommended)

```bash
# Format: make test-<category>-<test_name>
make test-jit-vla_test
make test-unit-ir_test
make test-abi-abi_sum_jit
```

**Works for any test automatically:**
```bash
# Existing tests
make test-jit-vla_test      # ✅ Works
make test-unit-ir_test      # ✅ Works

# New tests (no target needed!)
make test-jit-new_feature   # ✅ Works automatically
make test-unit-future_test  # ✅ Works automatically
```

### Method 2: Python Wrapper

```bash
python3 tools/test_wrapper.py <category>/<test_name>

# Examples
python3 tools/test_wrapper.py jit/vla_test
python3 tools/test_wrapper.py unit/ir_test
python3 tools/test_wrapper.py abi/abi_sum_jit
```

### Method 3: Make run-test Target

```bash
make run-test CATEGORY=<category> TEST=<test_name>

# Examples
make run-test CATEGORY=jit TEST=vla_test
make run-test CATEGORY=unit TEST=ir_test
```

### Method 4: List Available Tests

```bash
make list-tests
```

## How It Works

### Pattern Rule Magic

The Makefile contains:
```makefile
test-%:
	@CATEGORY=$(shell echo $* | sed 's/-.*//') && \
	 TEST=$(shell echo $* | sed 's/.*-//') && \
	 python3 tools/test_wrapper.py $$CATEGORY/$$TEST
```

This pattern rule:
1. **Matches any test name** - `test-jit-*`, `test-unit-*`, etc.
2. **Extracts category and test name** - `jit/vla_test`
3. **Calls the wrapper** - Which builds and runs the test

### No Explicit Targets Needed

When you create a new test file:
```bash
# Create new test
touch test/jit/my_new_feature.c

# Run it immediately - no Makefile changes needed!
make test-jit-my_new_feature  # ✅ Works!
```

## Examples

### Test VLA functionality
```bash
make test-jit-vla_test
```

### Test IR infrastructure
```bash
make test-unit-ir_test
```

### Test after making changes
```bash
# Make your changes
vim src/zero_cc/parser/ast_lowering.c

# Run affected test (fresh compilation guaranteed)
make test-jit-vla_test
```

## Why Use Single Tests?

| Benefit | Explanation |
|---------|-------------|
| **Fresh compilation** | No stale `.o` file issues |
| **Fast feedback** | ~0.1s vs ~0.5s for full suite |
| **Easy debugging** | Isolate specific functionality |
| **No manual build** | One command does everything |

## Comparison

| Approach | Time | Stale Files? |
|----------|------|--------------|
| `make test` | ~0.5s | No |
| `make test-jit-vla_test` | ~0.1s | No |
| Standalone gcc | Varies | **Yes** ❌ |

## Troubleshooting

### Test fails?

Try a clean rebuild:
```bash
make rebuild
make test-jit-vla_test
```

### Test not found?

Check the test exists:
```bash
ls test/jit/vla_test.c  # Should exist
```

### Build errors?

Check for syntax errors in the test file itself.

## Best Practices

1. **During development**: Use `make test-<category>-<name>`
2. **Before committing**: Run `make test` (full suite)
3. **After major changes**: Run `make rebuild && make test`
4. **Creating new tests**: No Makefile changes needed!

## Available Test Categories

| Category | Location | Purpose |
|----------|----------|---------|
| `jit` | `test/jit/` | JIT compilation tests |
| `unit` | `test/unit/` | Unit tests |
| `semantic` | `test/semantic/` | Semantic analysis tests |
| `abi` | `test/abi/` | ABI/calling convention tests |

## Summary

- ✅ **No explicit targets needed** - pattern rule handles everything
- ✅ **Fresh compilation** - no stale `.o` file issues
- ✅ **Fast feedback** - ~0.1s per test
- ✅ **Simple syntax** - `make test-<category>-<name>`

---

*Last updated: 2024-10-04*
