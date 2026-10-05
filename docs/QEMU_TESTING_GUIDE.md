# AArch64 & RISC-V Backend Testing via QEMU

## Why Test via QEMU?

After implementing Task 56 (15 new opcode handlers + backend functions), we should verify:

1. **Backend correctness** - Do aarch64/riscv implementations emit valid machine code?
2. **Calling conventions** - Do function calls work correctly?
3. **Stack alignment** - Is 16-byte alignment maintained?
4. **Multi-ISA support** - Does the abstraction actually work?

## Current State

| ISA | Backend Functions | Tested? |
|-----|------------------|---------|
| x86-64 | ✅ All implemented | ✅ Yes (native) |
| AArch64 | ✅ All implemented | ❌ No |
| RISC-V 64 | ✅ All implemented | ❌ No |
| RISC-V 32 | ✅ All implemented | ❌ No |

## Setup Steps

### 1. Install QEMU

```bash
sudo apt update
sudo apt install qemu-user-static qemu-user-binfmt
```

### 2. Enable binfmt (automatic binary execution)

```bash
# Register QEMU handlers
sudo update-binfmts --enable qemu-aarch64
sudo update-binfmts --enable qemu-riscv64
```

### 3. Test AArch64

```bash
# Compile a simple test for AArch64
./build/qnz_cc -target aarch64 -o /tmp/test_aarch64 test.c

# Run via QEMU
qemu-aarch64 /tmp/test_aarch64

# Or with test suite
make test TARGET=aarch64
```

### 4. Test RISC-V 64

```bash
# Compile a simple test for RISC-V 64
./build/qnz_cc -target riscv64 -o /tmp/test_riscv64 test.c

# Run via QEMU
qemu-riscv64 /tmp/test_riscv64

# Or with test suite
make test TARGET=riscv64
```

## Test Strategy

### Phase 1: Simple Tests (Quick Validation)

```c
// test_simple.c
int add(int a, int b) { return a + b; }
int main() { return add(2, 3) == 5 ? 0 : 1; }
```

```bash
# Test on all ISAs
./build/qnz_cc -target x86_64 -o /tmp/test_x64 test_simple.c
./build/qnz_cc -target aarch64 -o /tmp/test_aarch64 test_simple.c
./build/qnz_cc -target riscv64 -o /tmp/test_riscv64 test_simple.c

/tmp/test_x64 && echo "x86-64: PASS"
qemu-aarch64 /tmp/test_aarch64 && echo "AArch64: PASS"
qemu-riscv64 /tmp/test_riscv64 && echo "RISC-V: PASS"
```

### Phase 2: Full Test Suite

```bash
# Run all 118 tests on each ISA
make test TARGET=x86_64    # Should pass (baseline)
make test TARGET=aarch64   # New - expect some failures
make test TARGET=riscv64   # New - expect some failures
```

### Phase 3: Debug Failures

If tests fail:
```bash
# Dump IR for debugging
./build/qnz_cc -target aarch64 -dump-ir test.c

# Dump machine code
./build/qnz_cc -target aarch64 -dump-mc test.c

# Compare with x86-64 output
```

## Expected Issues

### AArch64
- **Stack alignment** - May need 16-byte alignment fixes
- **Calling convention** - x0-x7 for int args, v0-v7 for FP
- **Immediate ranges** - AArch64 has limited immediate encoding

### RISC-V
- **Stack alignment** - May need 16-byte alignment fixes
- **Calling convention** - a0-a7 for int args, fa0-fa7 for FP
- **Immediate ranges** - RISC-V has limited immediate encoding

## Recommendation

**YES, we should test via QEMU** because:

1. ✅ **Low effort** - QEMU setup takes ~5 minutes
2. ✅ **High value** - Catches ISA-specific bugs early
3. ✅ **Validates abstraction** - Confirms multi-ISA support works
4. ✅ **Prevents technical debt** - Better to fix now than later

## Quick Start

```bash
# 1. Install QEMU (5 min)
sudo apt update && sudo apt install -y qemu-user-static

# 2. Test simple program (2 min)
echo 'int main() { return 42; }' > /tmp/test.c
./build/qnz_cc -target aarch64 -o /tmp/test_aarch64 /tmp/test.c
qemu-aarch64 /tmp/test_aarch64
echo $?  # Should be 42

# 3. Run full test suite (5 min)
make test TARGET=aarch64
make test TARGET=riscv64
```

## Next Steps

1. Install QEMU
2. Run simple validation tests
3. Run full test suite
4. Fix any ISA-specific bugs
5. Commit results

**Estimated Time**: 30-60 minutes for full validation
