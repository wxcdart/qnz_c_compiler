# AOT (Ahead-Of-Time) Test Suite

This directory contains C source files for testing ZeroCC's ELF object file generation.

## Running AOT Tests

### Using the Test Runner

```bash
# Run all AOT tests for x86-64
python3 tools/test_runner.py --mode aot

# Run AOT tests for a specific target
python3 tools/test_runner.py --mode aot --target aarch64

# Run with verbose output
python3 tools/test_runner.py --mode aot --verbose

# Run specific test
python3 tools/test_runner.py --mode aot test/aot/hello_world
```

### Manual Testing

```bash
# Compile to object file
build/bin/zerocc -c test/aot/hello_world.c -o /tmp/hello.o

# Verify ELF structure
python3 tools/elf_verify.py /tmp/hello.o --verbose

# Link and run
gcc -o /tmp/hello /tmp/hello.o
/tmp/hello
```

## Test Cases

| Test File | Description | Tests |
|-----------|-------------|-------|
| `hello_world.c` | Basic hello world | String literals, printf |
| `arithmetic.c` | Integer operations | Add, subtract, multiply |
| `loops_conditionals.c` | Control flow | for loops, if/else, arrays |
| `pointers.c` | Pointer operations | Dereference, pointer arithmetic |
| `structs.c` | Struct handling | Struct definition, member access, return by value |

## Expected Behavior

All tests should:
1. Compile successfully with `zerocc -c`
2. Produce valid ELF object files
3. Link successfully with system linker
4. Execute and return exit code 0

## Troubleshooting

### Compilation Fails
- Check that `zerocc` is built: `make build/bin/zerocc`
- Verify source syntax with GCC: `gcc -c test/aot/hello_world.c`

### ELF Verification Fails
- Check required tools: `which readelf objdump nm`
- Install binutils if missing: `sudo apt-get install binutils`

### Linking Fails
- Ensure object file is valid: `readelf -h test.o`
- Check for undefined symbols: `nm test.o`

### Execution Fails (Cross-ISA)
- Verify QEMU is installed: `which qemu-aarch64-static`
- Install QEMU: `sudo apt-get install qemu-user-static`
