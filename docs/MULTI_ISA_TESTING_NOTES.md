# Multi-ISA Backend Testing Notes

## Status: Scheduled for Sprint 9

### Current Situation

- **QEMU available**: `/usr/bin/qemu-aarch64-static`, `/usr/bin/qemu-riscv64-static`
- **Backend implementations complete**: AArch64 and RISC-V backends implemented (Task 56)
- **Testing blocked**: Compiler only supports JIT execution (x86-64), no object file output

### What Was Implemented (Task 56)

✅ **AArch64 Backend** (7 new functions):
- `aarch64_emit_sar` - Arithmetic shift right
- `aarch64_emit_call_reg` - Call through register
- `aarch64_emit_stack_align` - Stack alignment
- `aarch64_emit_add_imm` - Add immediate
- `aarch64_emit_sub_imm` - Subtract immediate
- `aarch64_emit_inc` - Increment
- `aarch64_emit_dec` - Decrement

✅ **RISC-V Backend** (14 new functions - riscv64 + riscv32):
- `riscv64/riscv32_emit_sar` - Arithmetic shift right
- `riscv64/riscv32_emit_call_reg` - Call through register
- `riscv64/riscv32_emit_stack_align` - Stack alignment
- `riscv64/riscv32_emit_add_imm` - Add immediate
- `riscv64/riscv32_emit_sub_imm` - Subtract immediate
- `riscv64/riscv32_emit_inc` - Increment
- `riscv64/riscv32_emit_dec` - Decrement

### What Was Tested

✅ **x86-64**: All 118 tests passing (native JIT execution)

### What Is Scheduled (Sprint 9)

📅 **Task 65: Object File Output (ELF Generation)**
- Add .o output mode for all ISAs
- Generate ELF64 with sections, symbols, relocations
- Support -c flag for compile-only mode

📅 **Task 66: Linker Support & Standalone Execution**
- Add executable output mode with system linker
- Support -o output flag for final executables

📅 **Task 67: QEMU Testing Pipeline**
- Integrate QEMU user-mode emulation
- Add `make test TARGET=aarch64/riscv64`
- Run 118 tests via qemu-aarch64-static/qemu-riscv64-static

📅 **Task 68: Multi-ISA Test Suite Expansion**
- Add ISA-specific test cases
- Validate AArch64 AAPCS64 and RISC-V LP64D/ILP32D ABIs

### Risk Assessment

| Risk | Level | Mitigation |
|------|-------|------------|
| Backend emits invalid instructions | Medium | Code review + QEMU testing in Sprint 9 |
| Calling convention bugs | Medium | Followed standard ABIs (AAPCS64, LP64D) |
| Stack alignment issues | Low | Reused existing alignment logic |
| Immediate encoding issues | Medium | Used conservative immediate ranges |

### Recommendation

**Proceed with Sprint 8 optimization work** - the backends follow standard ABIs:
- AArch64: AAPCS64 (x0-x7 int args, v0-v7 FP args)
- RISC-V: LP64D/ILP32D (a0-a7 int args, fa0-fa7 FP args)

Multi-ISA testing is scheduled for Sprint 9 after object file output is added.

### Next Steps

1. ✅ Task 56 complete (x86-64 verified)
2. ✅ Sprint 8 optimization work ready to start
3. 📅 Sprint 9: Object file output + QEMU testing
4. 📅 Bug fixes as needed based on testing

---

*Last updated: 2024-10-04*
*Scheduled for: Sprint 9 (Weeks 34.5-36)*
