# Multi-ISA Backend Testing Notes

## Status: Testing Blocked (No QEMU Access)

### Current Situation

- **QEMU not available**: Requires `sudo apt install qemu-user-static`
- **No sudo access** on this system
- **Backends implemented but untested**:
  - AArch64: 7 new functions
  - RISC-V 64: 7 new functions
  - RISC-V 32: 7 new functions

### What Was Tested

✅ **x86-64**: All 118 tests passing (native execution)

### What Was NOT Tested

❌ **AArch64**: Backend implemented, not executed
❌ **RISC-V 64**: Backend implemented, not executed
❌ **RISC-V 32**: Backend implemented, not executed

### Risk Assessment

| Risk | Level | Mitigation |
|------|-------|------------|
| Backend emits invalid instructions | Medium | Code review + QEMU testing when available |
| Calling convention bugs | Medium | Followed standard ABIs (AAPCS64, LP64D) |
| Stack alignment issues | Low | Reused existing alignment logic |
| Immediate encoding issues | Medium | Used conservative immediate ranges |

### Testing Requirements

To test AArch64 and RISC-V backends:

```bash
# On a system with sudo access
sudo apt update
sudo apt install -y qemu-user-static

# Quick validation
echo 'int main() { return 42; }' > /tmp/test.c
./build/qnz_cc -target aarch64 -o /tmp/test_aarch64 /tmp/test.c
qemu-aarch64 /tmp/test_aarch64; echo $?  # Should be 42

# Full test suite
make test TARGET=aarch64
make test TARGET=riscv64
```

### Recommendation

**Proceed with Sprint 8 work** - the backend implementations follow standard patterns:

1. **AArch64**: Uses AAPCS64 calling convention (x0-x7 for int args)
2. **RISC-V**: Uses LP64D/ILP32D calling convention (a0-a7 for int args)
3. **Stack alignment**: 16-byte alignment (standard for both ISAs)

Common issues to watch for:
- Immediate value encoding limits
- Register allocation edge cases
- FP conversion instructions

### Next Steps

1. ✅ Task 56 complete (x86-64 verified)
2. ✅ Ready for Sprint 8 optimization work
3. ⏳ Multi-ISA testing deferred until QEMU available
4. ⏳ Bug fixes as needed based on user reports

### Code Review Checklist

Before using multi-ISA backends in production:

- [ ] Verify immediate encoding ranges for each ISA
- [ ] Test FP operations on AArch64 and RISC-V
- [ ] Verify stack alignment on both ISAs
- [ ] Test variadic functions
- [ ] Test function pointers
- [ ] Test recursive functions
