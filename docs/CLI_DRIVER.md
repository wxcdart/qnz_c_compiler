# ZeroCC CLI Driver

A GCC-compatible command-line interface for the ZeroCC C compiler.

## Building

```bash
make zerocc
# or
make all  # includes zerocc as a dependency
```

The CLI driver will be built at `build/bin/zerocc`.

## Usage

```bash
./build/bin/zerocc [options] <source.c>
```

## Options

### Output Control

| Option | Description |
|--------|-------------|
| `-o <file>` | Output file name |
| `-c` | Compile to object file (.o) - *Requires Task 65* |
| `-S` | Emit assembly - *Not yet implemented* |

### Optimization Levels

| Option | Description |
|--------|-------------|
| `-O0` | No optimization |
| `-O1` | Basic optimization (default) |
| `-O2` | Standard optimization |
| `-O3` | Aggressive optimization |

### Preprocessor Options

| Option | Description |
|--------|-------------|
| `-I<dir>` | Add include directory - *Not yet implemented* |
| `-D<macro>` | Define macro - *Not yet implemented* |

### Execution Mode

| Option | Description |
|--------|-------------|
| `-run, --run` | Run in JIT mode (default) |
| `-dump-ir` | Dump IR before optimization - *Not yet implemented* |

### Information

| Option | Description |
|--------|-------------|
| `-v, --verbose` | Verbose output |
| `--version` | Print version |
| `-h, --help` | Show help |

## Examples

### Basic Compilation (JIT Mode)

```bash
# Compile and run with default optimization (-O1)
./build/bin/zerocc test.c

# Compile and run with -O2 optimization
./build/bin/zerocc -O2 test.c

# Compile and run with no optimization (-O0)
./build/bin/zerocc -O0 test.c
```

### Verbose Mode

```bash
# Show compilation details
./build/bin/zerocc -v test.c
```

### Object File Output (Not Yet Implemented)

```bash
# This requires Task 65 (ELF generation)
./build/bin/zerocc -c test.c
# Error: Object file output (-c) not yet implemented
```

### Version and Help

```bash
# Print version
./build/bin/zerocc --version
# Output: ZeroCC C Compiler version 0.1.0-dev

# Show help
./build/bin/zerocc -h
```

## Optimization Pipeline

The CLI driver integrates with the optimization pipeline:

```
-O0: No optimization passes
-O1: Constant folding + DCE + CSE
-O2: Constant folding + DCE + CSE + LICM
-O3: Constant folding + DCE + CSE + LICM + Strength Reduction + Branch Simplification
```

## Architecture

```
src/main.c
├── parse_args() - Parse command-line arguments
├── compile_jit() - JIT compilation and execution
├── compile_object() - Object file output (placeholder)
└── read_file() - Read source file
```

### Argument Parsing

The CLI driver uses a simple argument parser that supports:
- Short options (`-o`, `-v`, `-c`)
- Long options (`--version`, `--help`, `--run`)
- Optimization level flags (`-O0`, `-O1`, `-O2`, `-O3`)
- Preprocessor options (`-I`, `-D`) - placeholders

### JIT Mode

JIT mode is the default execution mode:
1. Read source file
2. Preprocess
3. Tokenize
4. Initialize optimization level
5. Compile to IR
6. Run optimizations
7. Register allocation
8. Generate machine code
9. Execute

### Object File Mode

Object file mode (`-c`) is a placeholder for Task 65:
- Currently returns an error message
- Will emit ELF object files when implemented
- Will support linking with system linker (gcc/ld)

## Integration with GCC

The CLI driver is designed to be GCC-compatible:

```bash
# GCC
gcc -O2 -o test test.c

# ZeroCC (JIT mode)
./build/bin/zerocc -O2 test.c

# ZeroCC (object mode - future)
./build/bin/zerocc -O2 -c test.c
gcc -o test test.o
```

## Differential Testing

The CLI driver enables direct differential testing against GCC:

```bash
# Compile with both compilers
gcc -O2 test.c -o /tmp/gcc_test
./build/bin/zerocc -O2 test.c

# Compare outputs
/tmp/gcc_test < input > /tmp/gcc_out
./build/bin/zerocc -O2 test.c < input > /tmp/zerocc_out
diff /tmp/gcc_out /tmp/zerocc_out
```

## Limitations

### Current Limitations

- **Object file output**: `-c` flag not yet implemented (Task 65)
- **Assembly output**: `-S` flag not yet implemented
- **Preprocessor**: `-I` and `-D` flags are placeholders
- **IR dump**: `-dump-ir` flag not yet implemented
- **Linking**: No automatic linking with libc (JIT mode only)

### Future Enhancements

- Full preprocessor support
- Assembly output
- Object file emission (ELF)
- Standalone executable generation
- Cross-compilation support

## Build System

The CLI driver is built as part of the main build:

```makefile
# Makefile
ZEROCCLIB_OBJS = $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS)
build/bin/zerocc: src/main.c $(ZEROCCLIB_OBJS)
	@mkdir -p build/bin
	$(CC) $(CFLAGS) -o $@ src/main.c $(ZEROCCLIB_OBJS) $(LDFLAGS)

.PHONY: zerocc
zerocc: build/bin/zerocc

all: $(TESTS) build/bin/zerocc
```

## Testing

### Manual Testing

```bash
# Test basic compilation
./build/bin/zerocc -v /tmp/test_simple.c

# Test optimization levels
./build/bin/zerocc -O0 /tmp/test.c
./build/bin/zerocc -O1 /tmp/test.c
./build/bin/zerocc -O2 /tmp/test.c
./build/bin/zerocc -O3 /tmp/test.c
```

### Automated Testing

The CLI driver is tested via:
- Unit tests in `test/unit/`
- JIT tests in `test/jit/`
- Benchmark tests in `test/bench/`

## Files

| File | Description |
|------|-------------|
| `src/main.c` | CLI driver implementation (258 lines) |
| `docs/CLI_DRIVER.md` | This documentation |
| `Makefile` | Build rules for CLI driver |

## Version

- **Current**: 0.1.0-dev
- **Status**: Experimental
- **License**: See project LICENSE

## Roadmap

### Sprint 9 (Current)

- [x] CLI driver with argument parsing
- [x] JIT mode integration
- [x] Optimization level support
- [ ] Object file output (Task 65)
- [ ] Linker support (Task 66)

### Future

- [ ] Full preprocessor support
- [ ] Assembly output
- [ ] Cross-compilation
- [ ] Debugging support
