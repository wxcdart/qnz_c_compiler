#!/usr/bin/env python3
"""Task 56 Workspace Setup - Create missing infrastructure

This script:
1. Creates ir_clear() function
2. Generates missing opcode handler templates
3. Creates a task56_checklist.md for tracking progress
"""

import os

def create_ir_clear_function():
    """Create ir_clear() function for ir.c"""
    
    ir_c_path = 'src/zero_cc/parser/ir.c'
    
    if not os.path.exists(ir_c_path):
        print(f"❌ {ir_c_path} not found")
        return False
    
    with open(ir_c_path, 'r') as f:
        content = f.read()
    
    # Check if ir_clear already exists
    if 'ir_clear' in content:
        print("✅ ir_clear() already exists")
        return True
    
    # Find the end of the file and add ir_clear
    ir_clear_func = '''
/* Clear IR buffer for next function */
void ir_clear(void) {
    ir_count = 0;
}
'''
    
    with open(ir_c_path, 'a') as f:
        f.write(ir_clear_func)
    
    print("✅ Created ir_clear() in ir.c")
    
    # Also add declaration to ir.h
    ir_h_path = 'src/zero_cc/parser/ir.h'
    if os.path.exists(ir_h_path):
        with open(ir_h_path, 'r') as f:
            header = f.read()
        
        if 'ir_clear' not in header:
            # Add declaration after ir_get_count
            header = header.replace(
                'size_t ir_get_count(void);',
                'size_t ir_get_count(void);\nvoid ir_clear(void);'
            )
            with open(ir_h_path, 'w') as f:
                f.write(header)
            print("✅ Added ir_clear() declaration to ir.h")
    
    return True

def generate_missing_handler_templates():
    """Generate template code for missing opcode handlers"""
    
    missing_handlers = {
        'OP_PUSH_RAX': '''/* Push register to stack */
case OP_PUSH_RAX:
    if (BE(c)->emit_push_reg) {
        BE(c)->emit_push_reg(c, regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1));
    }
    break;''',
        
        'OP_POP_RAX': '''/* Pop register from stack */
case OP_POP_RAX:
    if (BE(c)->emit_pop_reg) {
        BE(c)->emit_pop_reg(c, regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest));
    }
    break;''',
        
        'OP_PUSH_RBX': '''/* Push register to stack */
case OP_PUSH_RBX:
    if (BE(c)->emit_push_reg) {
        BE(c)->emit_push_reg(c, regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1));
    }
    break;''',
        
        'OP_POP_RBX': '''/* Pop register from stack */
case OP_POP_RBX:
    if (BE(c)->emit_pop_reg) {
        BE(c)->emit_pop_reg(c, regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest));
    }
    break;''',
        
        'OP_ADD_IMM': '''/* Add immediate to register */
case OP_ADD_IMM:
    phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
    if (BE(c)->emit_add_imm) {
        BE(c)->emit_add_imm(c, phys_dst, (int16_t)instr->imm);
    }
    break;''',
        
        'OP_SUB_IMM': '''/* Subtract immediate from register */
case OP_SUB_IMM:
    phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
    if (BE(c)->emit_sub_imm) {
        BE(c)->emit_sub_imm(c, phys_dst, (int16_t)instr->imm);
    }
    break;''',
        
        'OP_INC': '''/* Increment register */
case OP_INC:
    phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
    if (BE(c)->emit_inc) {
        BE(c)->emit_inc(c, phys_dst);
    }
    break;''',
        
        'OP_DEC': '''/* Decrement register */
case OP_DEC:
    phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
    if (BE(c)->emit_dec) {
        BE(c)->emit_dec(c, phys_dst);
    }
    break;''',
        
        'OP_SAR': '''/* Arithmetic shift right */
case OP_SAR:
    phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
    phys_src = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src2);
    if (BE(c)->emit_sar) {
        BE(c)->emit_sar(c, phys_dst, phys_src);
    }
    break;''',
        
        'OP_CALL_INDIRECT': '''/* Call through register pointer */
case OP_CALL_INDIRECT:
    /* Load address from register and call */
    addr_reg = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1);
    if (BE(c)->emit_call_reg) {
        BE(c)->emit_call_reg(c, addr_reg);
    }
    break;''',
        
        'OP_RET': '''/* Return from function */
case OP_RET:
    if (BE(c)->emit_ret) {
        BE(c)->emit_ret(c);
    }
    break;''',
        
        'OP_EPILOGUE': '''/* Function epilogue (callee-save loads, leave, ret) */
case OP_EPILOGUE:
    if (BE(c)->emit_epilogue) {
        BE(c)->emit_epilogue(c);
    }
    break;''',
        
        'OP_STACK_ALIGN': '''/* Align stack pointer */
case OP_STACK_ALIGN:
    if (BE(c)->emit_stack_align) {
        BE(c)->emit_stack_align(c, (uint8_t)instr->imm);
    }
    break;''',
    }
    
    # Create a template file
    template_path = 'tools/task56_missing_handlers.txt'
    with open(template_path, 'w') as f:
        f.write("# Missing Opcode Handler Templates\n")
        f.write("# Copy these into ast_lowering_mc.c switch statement\n\n")
        for opcode, handler in missing_handlers.items():
            f.write(f"\n// {opcode}\n")
            f.write(handler + "\n\n")
    
    print(f"✅ Created handler templates in {template_path}")
    return True

def create_task56_checklist():
    """Create a detailed checklist for Task 56"""
    
    checklist = '''# Task 56: Pass 2 Machine Code Emission - Checklist

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
'''
    
    checklist_path = 'tools/task56_checklist.md'
    with open(checklist_path, 'w') as f:
        f.write(checklist)
    
    print(f"✅ Created checklist at {checklist_path}")

def main():
    print("=" * 70)
    print("Task 56 Workspace Setup")
    print("=" * 70)
    print()
    
    print("Step 1: Creating ir_clear() function...")
    create_ir_clear_function()
    
    print("\nStep 2: Generating missing handler templates...")
    generate_missing_handler_templates()
    
    print("\nStep 3: Creating Task 56 checklist...")
    create_task56_checklist()
    
    print("\n" + "=" * 70)
    print("Workspace Setup Complete!")
    print("=" * 70)
    print("""
Next steps:
1. Review tools/task56_checklist.md for detailed task breakdown
2. Review tools/task56_missing_handlers.txt for handler templates
3. Start with Phase 1: Infrastructure Setup
4. Run 'make test' after each phase to verify
""")

if __name__ == '__main__':
    main()
