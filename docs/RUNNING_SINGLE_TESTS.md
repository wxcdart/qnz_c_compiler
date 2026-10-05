# Running Single Tests

## Why Run Single Tests?

Running individual tests ensures:
- **Fresh compilation** - No stale object files
- **Faster feedback** - Test specific functionality quickly
- **Debugging** - Isolate issues in specific tests

## Methods

### Method 1: Make Shortcut (Recommended)

```bash
# Format: make test-<category>-<test_name>
make test-jit-vla_test
make test-unit-ir_test
make test-abi-abi_sum_jit
```

**Benefits:**
- ✅ Fresh compilation guaranteed
- ✅ Clean output
- ✅ Simple syntax

### Method 2: Python Wrapper

```bash
# Format: python3 tools/test_wrapper.py <category>/<test_name>
python3 tools/test_wrapper.py jit/vla_test
python3 tools/test_wrapper.py unit/ir_test
python3 tools/test_wrapper.py abi/abi_sum_jit
```

**Benefits:**
- ✅ Same as Method 1
- ✅ Can be called from scripts

### Method 3: Make run-test Target

```bash
# Format: make run-test CATEGORY=<category> TEST=<test_name>
make run-test CATEGORY=jit TEST=vla_test
make run-test CATEGORY=unit TEST=ir_test
```

**Benefits:**
- ✅ Explicit category/test separation
- ✅ Good for Makefile integration

### Method 4: Manual Build + Run

```bash
# Build specific test
make build/test/jit/vla_test

# Run it
./build/test/jit/vla_test
```

**Benefits:**
- ✅ Full control
- ⚠️ Two steps required

## Available Tests

```bash
# List all tests
make list-tests
```

Example output:
```
Available tests:

JIT Tests:
  - test-anon_struct_test
  - test-vla_test
  - test-ternary_jit
  ...

Unit Tests:
  - test-ir_test
  - test-hash_table_tpht_test
  ...
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

### Test ABI calling convention
```bash
make test-abi-abi_sum_jit
```

### Test after making changes
```bash
# Make your changes
vim src/zero_cc/parser/ast_lowering.c

# Run affected test
make test-jit-vla_test
```

## Troubleshooting

### Test fails with stale object files?

Use one of the methods above - they all ensure fresh compilation.

**Don't do this:**
```bash
# ❌ This may use stale .o files
gcc -Isrc test/jit/vla_test.c ... -o /tmp/vla_test
/tmp/vla_test
```

**Do this instead:**
```bash
# ✅ This guarantees fresh build
make test-jit-vla_test
```

### Build fails?

Try a clean rebuild:
```bash
make rebuild
make test-jit-vla_test
```

## Comparison with Full Test Suite

| Method | Time | Use Case |
|--------|------|----------|
| `make test` | ~0.5s | Full regression testing |
| `make test-jit-vla_test` | ~0.1s | Quick iteration on VLA fixes |
| `make test-unit-ir_test` | ~0.05s | IR infrastructure changes |

## Best Practices

1. **During development**: Use `make test-<category>-<test_name>`
2. **Before committing**: Run `make test` (full suite)
3. **After major changes**: Run `make rebuild && make test`
4. **Debugging**: Use `make test-<specific_test>` to isolate issues

---

*Last updated: 2024-10-04*
