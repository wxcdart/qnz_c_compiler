# Task 56: Pass 2 Machine Code Emission - Checklist

## Phase 1: Infrastructure Setup (Day 1)

### 1.1 IR Buffer Lifecycle
- [ ] Add `ir_clear()` function to `ir.c`
- [ ] Add `ir_clear()` declaration to `ir.h`
- [ ] Call `ir_clear()` at start of each function compilation
- [ ] Test: Verify buffer is reset between functions

### 1.2 Missing Opcode Handlers
- [ ] Add OP_PUSH_RAX handler
- [ ] Add OP_POP_RAX handler
- [ ] Add OP_PUSH_RBX handler
- [ ] Add OP_POP_RBX handler
- [ ] Add OP_ADD_IMM handler
- [ ] Add OP_SUB_IMM handler
- [ ] Add OP_INC handler
- [ ] Add OP_DEC handler
- [ ] Add OP_SAR handler
- [ ] Add OP_CALL_INDIRECT handler
- [ ] Add OP_RET handler
- [ ] Add OP_EPILOGUE handler
- [ ] Add OP_STACK_ALIGN handler

### 1.3 Backend Function Extensions
- [ ] Add `emit_push_reg()` to `isa_backend.h`
- [ ] Add `emit_pop_reg()` to `isa_backend.h`
- [ ] Add `emit_add_imm()` to `isa_backend.h`
- [ ] Add `emit_sub_imm()` to `isa_backend.h`
- [ ] Add `emit_inc()` to `isa_backend.h`
- [ ] Add `emit_dec()` to `isa_backend.h`
- [ ] Add `emit_sar()` to `isa_backend.h`
- [ ] Add `emit_call_reg()` to `isa_backend.h`
- [ ] Add `emit_ret()` to `isa_backend.h`
- [ ] Add `emit_stack_align()` to `isa_backend.h`

## Phase 2: Backend Implementation (Day 2)

### 2.1 x86-64 Backend
- [ ] Implement `x86_emit_push_reg()`
- [ ] Implement `x86_emit_pop_reg()`
- [ ] Implement `x86_emit_add_imm()`
- [ ] Implement `x86_emit_sub_imm()`
- [ ] Implement `x86_emit_inc()`
- [ ] Implement `x86_emit_dec()`
- [ ] Implement `x86_emit_sar()`
- [ ] Implement `x86_emit_call_reg()`
- [ ] Implement `x86_emit_ret()`
- [ ] Implement `x86_emit_stack_align()`
- [ ] Update `x86_64_backend` struct initialization

### 2.2 AArch64 Backend
- [ ] Implement `aarch64_emit_push_reg()`
- [ ] Implement `aarch64_emit_pop_reg()`
- [ ] Implement `aarch64_emit_add_imm()`
- [ ] Implement `aarch64_emit_sub_imm()`
- [ ] Implement `aarch64_emit_inc()`
- [ ] Implement `aarch64_emit_dec()`
- [ ] Implement `aarch64_emit_sar()`
- [ ] Implement `aarch64_emit_call_reg()`
- [ ] Implement `aarch64_emit_ret()`
- [ ] Implement `aarch64_emit_stack_align()`
- [ ] Update `aarch64_backend` struct initialization

### 2.3 RISC-V Backend
- [ ] Implement `riscv64_emit_push_reg()`
- [ ] Implement `riscv64_emit_pop_reg()`
- [ ] Implement `riscv64_emit_add_imm()`
- [ ] Implement `riscv64_emit_sub_imm()`
- [ ] Implement `riscv64_emit_inc()`
- [ ] Implement `riscv64_emit_dec()`
- [ ] Implement `riscv64_emit_sar()`
- [ ] Implement `riscv64_emit_call_reg()`
- [ ] Implement `riscv64_emit_ret()`
- [ ] Implement `riscv64_emit_stack_align()`
- [ ] Update `riscv64_backend` struct initialization
- [ ] Update `riscv32_backend` struct initialization

## Phase 3: Integration & Testing (Day 3)

### 3.1 Two-Pass Flow Verification
- [ ] Verify Pass 1 (AST→IR) populates IR buffer correctly
- [ ] Verify Pass 2 (IR→MC) reads from IR buffer
- [ ] Verify `ir_clear()` is called between functions
- [ ] Test with multi-function program

### 3.2 Test Suite Validation
- [ ] Run `make test` - all 118 tests should pass
- [ ] Run with AddressSanitizer: `make asan`
- [ ] Run with UndefinedBehaviorSanitizer: `make ubsan`

### 3.3 Edge Cases
- [ ] Test function with no return statement
- [ ] Test function with multiple return statements
- [ ] Test indirect function calls
- [ ] Test variadic functions
- [ ] Test functions with >6 arguments

## Success Criteria

- [ ] All 118 tests pass
- [ ] No compiler warnings
- [ ] No memory leaks (ASan clean)
- [ ] No undefined behavior (UBSan clean)
- [ ] Two-pass flow verified with debug output

## Notes

- Missing handlers found by `task56_precheck.py`: 15 opcodes
- Backend functions need to be added to all 3 ISAs (x86-64, aarch64, riscv)
- Estimated effort: 3 days
