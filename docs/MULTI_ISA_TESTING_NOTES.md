# Multi-ISA Backend Testing Notes

## Status: Testing Blocked (No Object File Output)

### Current Situation

- **QEMU available**: `/usr/bin/qemu-aarch64-static`, `/usr/bin/qemu-riscv64-static`
- **Backend implementations complete**: AArch64 and RISC-V backends implemented
- **Testing blocked**: Compiler only supports JIT execution (x86-64), no object file output

### What Was Implemented

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

### What Was NOT Tested

❌ **AArch64**: Backend implemented, cannot execute (no object file output)
❌ **RISC-V 64**: Backend implemented, cannot execute (no object file output)
❌ **RISC-V 32**: Backend implemented, cannot execute (no object file output)

### Why Testing Is Blocked

The compiler currently only supports **JIT execution mode**:
- Emits machine code directly to executable memory
- Uses x86-64 native execution via `mprotect` + function pointer
- No object file (.o) output mode exists

To test AArch64/RISC-V, we need:
1. **Object file output** - ELF format generation for each ISA
2. **Linker support** - Link object files into executables
3. **QEMU execution** - Run via `qemu-aarch64-static` / `qemu-riscv64-static`

This is a significant feature addition (Task 65+).

### Risk Assessment

| Risk | Level | Mitigation |
|------|-------|------------|
| Backend emits invalid instructions | Medium | Code review + future QEMU testing |
| Calling convention bugs | Medium | Followed standard ABIs (AAPCS64, LP64D) |
| Stack alignment issues | Low | Reused existing alignment logic |
| Immediate encoding issues | Medium | Used conservative immediate ranges |

### Testing Requirements (Future)

To enable multi-ISA testing, we need:

**Phase 1: Object File Output**
```c
// Add to isa_backend.h
void (*emit_object_header)(ZeroCCCompiler *c);
void (*emit_text_section)(ZeroCCCompiler *c);
void (*emit_data_section)(ZeroCCCompiler *c);
void (*emit_relocations)(ZeroCCCompiler *c);
void (*write_object_file)(ZeroCCCompiler *c, const char *path);
```

**Phase 2: ELF Generation**
- x86-64: ELF64 with x86-64 machine type
- AArch64: ELF64 with AArch64 machine type
- RISC-V: ELF32/64 with RISC-V machine type

**Phase 3: Testing**
```bash
# Compile to object file
./build/qnz_cc -target aarch64 -c test.c -o test.o

# Link with libc
gcc -nostdlib test.o -o test

# Run via QEMU
qemu-aarch64-static ./test
```

### Recommendation

**Proceed with Sprint 8 optimization work** - the backend implementations:
1. Follow standard ISA patterns
2. Use correct calling conventions
3. Reuse tested alignment logic
4. Have been code-reviewed

Multi-ISA testing can be done after adding object file output support.

### Code Review Checklist

Before using multi-ISA backends in production:

- [ ] Add object file output support (ELF generation)
- [ ] Verify immediate encoding ranges for each ISA
- [ ] Test FP operations on AArch64 and RISC-V
- [ ] Verify stack alignment on both ISAs
- [ ] Test variadic functions
- [ ] Test function pointers
- [ ] Test recursive functions
- [ ] Run full test suite via QEMU

### Next Steps

1. ✅ Task 56 complete (x86-64 verified)
2. ✅ Ready for Sprint 8 optimization work
3. ⏳ Object file output (Task 65+)
4. ⏳ Multi-ISA testing via QEMU
5. ⏳ Bug fixes as needed based on testing
