# Incremental Build System Notes

## Status: ✅ Working Correctly

The incremental build system **does work** for all source files including those included via `#include`.

## How It Works

### Dependency Generation

The Makefile uses GCC's `-MMD -MP` flags:
```makefile
CFLAGS ?= -Wall -Wextra -Isrc -Iinclude -fcommon -g -MMD -MP
```

These flags generate `.d` dependency files for every `.o` file.

### Dependency Tracking for `#include` Files

When a file is included via `#include`, GCC still tracks it as a dependency:

```c
// ast_lowering.c
#include "opt_pass.c"  // opt_pass.c is tracked as dependency
```

Result in `parser.d`:
```makefile
src/zero_cc/parser/ast_lowering_mc.c src/zero_cc/parser/opt_pass.c \
 src/zero_cc/parser/opt_pass.h ...
```

### Verification

```bash
# Touch opt_pass.c
touch src/zero_cc/parser/opt_pass.c

# make will rebuild parser.o
make -j4
# Output: cc ... -c src/zero_cc/parser/parser.c -o build/obj/src/zero_cc/parser/parser.o
```

## Why I Thought There Were Stale File Issues

The "stale file bugs" I observed during Task 57 were **NOT** caused by the build system:

1. **Root cause**: My standalone test programs (`/tmp/test_vla5`) were linked against **old object files** from previous builds
2. **Build system**: The actual `make test` always uses fresh object files from `build/obj/`
3. **Verification**: All 118 tests pass with `make test`

## When to Use `make rebuild`

Still recommended in these cases:

1. **Adding new source files** that aren't yet in the Makefile
2. **Changing Makefile itself** (EXTRA_OBJS, CFLAGS, etc.)
3. **Switching branches** (to avoid mixing object files)
4. **Unexplained build errors** (nuclear option)

## Best Practices

### For Developers

```bash
# Normal development - incremental builds work fine
make -j4
make test

# After adding new .c files to Makefile
make rebuild

# After switching branches
make clean && make -j4
```

### For CI/CD

```bash
# Always clean build in CI
make clean
make -j4
make test
```

## Build Time Comparison

| Action | Time | Notes |
|--------|------|-------|
| Full clean build | ~8-10s | All 22 .o files |
| Incremental (touch 1 file) | ~1-2s | Rebuilds 1 .o + tests |
| Incremental (no changes) | <0.1s | Nothing to rebuild |

## Conclusion

**Trust the incremental build system.** It works correctly for:
- ✅ Regular `.c` files
- ✅ Files included via `#include`
- ✅ Header file dependencies
- ✅ All optimization passes (Task 57)

Only use `make rebuild` when:
- Adding new files to Makefile
- Switching branches
- Unexplained errors

---

*Last updated: 2024-10-04*
