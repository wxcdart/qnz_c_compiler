# Stale Object File Issue - Task 57

## Problem

When adding new source files (like `opt_*.c`), the build system may not detect that existing object files are stale, leading to:
- Strange runtime bugs
- Tests failing with incorrect results
- Inconsistent behavior between clean and incremental builds

## Root Cause

The Makefile's dependency tracking may not catch all cases when new files are added, especially if:
1. New `.c` files are added to `EXTRA_OBJS`
2. New `.h` files are created but not included in header dependencies
3. Object files from previous builds remain in `build/`

## Solution

### Option 1: Use `make rebuild` (Recommended)

```bash
make rebuild
```

This performs a complete clean rebuild in one command.

### Option 2: Manual clean + build

```bash
make clean
make -j4
```

### Option 3: Use cleanup script

```bash
./tools/fix_stale_objects.sh
make -j4
```

## Prevention

### For Developers

1. **When adding new source files:**
   - Always run `make rebuild` after adding new `.c` files
   - Don't rely on `make` to detect new files automatically

2. **If you see strange bugs:**
   - First suspect: stale object files
   - Run `make rebuild` before reporting bugs

3. **Before committing:**
   - Run `make clean && make -j4` to verify clean build works
   - Run `make test` to ensure all tests pass

### For the Build System

The Makefile now includes:
- `rebuild` target: clean + all in one command
- `force-rebuild` alias: same as rebuild
- Warning comment at top of Makefile

## Verification

After running `make rebuild`:
```bash
$ make rebuild
rm -rf build
[compilation output...]

$ make test
Results: 118/118 passed, 0 failed
```

## Related Issues

- Task 57 optimization passes appeared buggy due to stale objects
- VLA compound assignment test failed (72 vs expected 42)
- Root cause: `opt_pass.c` changes not picked up by incremental build

## Lessons Learned

1. **Always do clean builds when adding new files**
2. **Don't trust incremental builds after major changes**
3. **Add `rebuild` target to Makefiles**
4. **Document the issue for future developers**
