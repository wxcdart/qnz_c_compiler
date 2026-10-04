# Two-Pass IR Architecture - Visual Guide

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         ZEROCC COMPILER                              │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                            PASS 1                                    │
│                      AST → IR (Lowering)                             │
│                                                                      │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────────┐  │
│  │   AST Tree   │───▶│ ast_lowering │───▶│  IR Buffer           │  │
│  │              │    │ .c           │    │  (ir_instructions[]) │  │
│  │  parse()     │    │              │    │  (ir_count)          │  │
│  └──────────────┘    └──────────────┘    └──────────────────────┘  │
│                                  │                                  │
│                                  ▼                                  │
│                       ┌──────────────────┐                         │
│                       │  Register Alloc  │                         │
│                       │  regalloc.h      │                         │
│                       │  - Live ranges   │                         │
│                       │  - Spill slots   │                         │
│                       └──────────────────┘                         │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                            PASS 2                                    │
│                    IR → Machine Code (Emission)                      │
│                                                                      │
│  ┌──────────────────┐    ┌──────────────────┐    ┌──────────────┐  │
│  │  IR Buffer       │───▶│ ast_lowering_mc  │───▶│  ISA Backend │  │
│  │  (ir_count)      │    │ .c               │    │  (x86/aarch  │  │
│  │  (ir_instrs[])   │    │                  │    │  /riscv)     │  │
│  └──────────────────┘    │  switch(op)      │    └──────────────┘  │
│                          │  114 opcodes     │                      │
│                          └──────────────────┘                      │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
                          ┌──────────────┐
                          │  JIT Exec    │
                          │  (W^X mem)   │
                          └──────────────┘
```

## Data Flow

### Pass 1: AST → IR

```c
// Entry point: parser.c
void compile_function(ZeroCCCompiler *c) {
    // 1. Parse AST
    ASTRef ast = parse_function(c);
    
    // 2. Lower AST to IR (Pass 1)
    zero_cc_lower_ast(c, arena, ast);
    //    └─> Emits IR instructions via ir_emit()
    //    └─> Populates ir_instructions[] buffer
    //    └─> Sets ir_count
    
    // 3. Register allocation
    regalloc_analyze(c);
    regalloc_allocate(c);
    //    └─> Assigns physical registers to VREGs
    //    └─> Calculates spill slots
    
    // 4. Emit machine code (Pass 2)
    emit_ir_to_machine_code(c, spill_base_offset);
    //    └─> Reads IR from ir_instructions[]
    //    └─> Calls backend functions via BE(c)->emit_xxx()
    //    └─> Emits machine code to JIT buffer
    
    // 5. Clear IR buffer for next function
    ir_clear();
}
```

### Pass 2: IR → Machine Code

```c
// Entry point: ast_lowering_mc.c
void emit_ir_to_machine_code(ZeroCCCompiler *c, int32_t spill_base_offset) {
    const Instr *instrs = ir_get_buffer();  // Get IR buffer
    size_t count = ir_get_count();          // Get instruction count
    
    for (size_t i = 0; i < count; i++) {
        const Instr *instr = &instrs[i];
        
        switch (instr->op) {
            case OP_MOV_IMM:
                // Emit: mov reg, imm
                BE(c)->emit_mov_imm(c, reg, instr->imm);
                break;
                
            case OP_CALL:
                // Emit: call function
                BE(c)->emit_call(c, instr->label_idx);
                break;
                
            // ... 112 more opcodes ...
            
            case OP_RET:
                // Emit: ret
                BE(c)->emit_ret(c);
                break;
        }
    }
}
```

## IR Buffer Lifecycle

```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│  Function A │───▶│  Function B │───▶│  Function C │───▶│    ...      │
│  Pass 1     │    │  Pass 1     │    │  Pass 1     │    │             │
│  ir_emit()  │    │  ir_emit()  │    │  ir_emit()  │    │             │
└─────────────┘    └─────────────┘    └─────────────┘    └─────────────┘
       │                  │                  │
       ▼                  ▼                  ▼
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│  Pass 2     │    │  Pass 2     │    │  Pass 2     │
│  ir_clear() │    │  ir_clear() │    │  ir_clear() │
└─────────────┘    └─────────────┘    └─────────────┘
```

## Backend Abstraction

```c
// isa_backend.h - Backend interface
typedef struct ZeroCCISABackend {
    // Prologue/Epilogue
    void (*emit_prologue)(ZeroCCCompiler *c, size_t stack_size);
    void (*emit_leave)(ZeroCCCompiler *c);
    void (*emit_epilogue)(ZeroCCCompiler *c);
    
    // Integer operations
    void (*emit_mov_imm)(ZeroCCCompiler *c, int dst, int64_t imm);
    void (*emit_add)(ZeroCCCompiler *c, int dst, int src1, int src2);
    // ... more ...
    
    // Calling convention
    const ISACallingConvention *conv;
} ZeroCCISABackend;

// Usage in Pass 2
BE(c)->emit_mov_imm(c, reg, imm);  // Calls x86/aarch64/riscv implementation
```

## Opcode Categories

### Data Movement (10 opcodes)
- `OP_MOV_IMM`, `OP_MOV_MEM_RAX`, `OP_MOV_RAX_MEM`, `OP_MOV_RAX_RBX`
- `OP_LEA_RAX_MEM`, `OP_PUSH_RAX`, `OP_POP_RAX`, `OP_PUSH_RBX`, `OP_POP_RBX`

### Arithmetic (10 opcodes)
- `OP_ADD_IMM`, `OP_SUB_IMM`, `OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_MOD`
- `OP_NEG`, `OP_INC`, `OP_DEC`

### Logical (8 opcodes)
- `OP_AND`, `OP_OR`, `OP_XOR`, `OP_NOT`, `OP_LNOT`, `OP_SHL`, `OP_SHR`, `OP_SAR`

### Comparison (6 opcodes)
- `OP_CMP`, `OP_TEST`, `OP_SETL`, `OP_SETLE`, `OP_SETG`, `OP_SETGE`

### Control Flow (6 opcodes)
- `OP_JMP`, `OP_JE`, `OP_JNE`, `OP_JL`, `OP_JLE`, `OP_JG`, `OP_JMP_LABEL`

### Function (4 opcodes)
- `OP_CALL`, `OP_CALL_INDIRECT`, `OP_RET`, `OP_PROLOGUE`, `OP_EPILOGUE`

### Frame Management (5 opcodes)
- `OP_PROLOGUE`, `OP_EPILOGUE`, `OP_PARAM_SAVE_INT`, `OP_PARAM_SAVE_FP`, `OP_VARIADIC_SPILL`

### Special (21 opcodes)
- `OP_LABEL`, `OP_DEREF`, `OP_LOAD_BYTE`, `OP_CMP_ZERO`, `OP_STACK_ALIGN`
- `OP_CVTTSD2SI`, `OP_CVTSI2SD`, `OP_VLA_ALLOC`, atomic ops, FP ops, etc.

## Task 56 Focus Areas

### Missing Handlers (15 opcodes)
```
Critical:
  OP_RET, OP_EPILOGUE, OP_CALL_INDIRECT

Stack (ISA-specific):
  OP_PUSH_RAX, OP_POP_RAX, OP_PUSH_RBX, OP_POP_RBX

Arithmetic (ISA-specific):
  OP_ADD_IMM, OP_SUB_IMM, OP_INC, OP_DEC, OP_SAR

Special:
  OP_STACK_ALIGN
```

### Backend Functions to Add (10 functions)
```c
// Stack
void (*emit_push_reg)(ZeroCCCompiler *c, int reg);
void (*emit_pop_reg)(ZeroCCCompiler *c, int reg);

// Arithmetic
void (*emit_add_imm)(ZeroCCCompiler *c, int dst, int32_t imm);
void (*emit_sub_imm)(ZeroCCCompiler *c, int dst, int32_t imm);
void (*emit_inc)(ZeroCCCompiler *c, int reg);
void (*emit_dec)(ZeroCCCompiler *c, int reg);
void (*emit_sar)(ZeroCCCompiler *c, int dst, int shift);

// Control flow
void (*emit_call_reg)(ZeroCCCompiler *c, int reg);
void (*emit_ret)(ZeroCCCompiler *c);

// Special
void (*emit_stack_align)(ZeroCCCompiler *c, uint8_t alignment);
```

## Verification Checklist

After Task 56 completion:

```
[ ] All 15 opcode handlers implemented
[ ] All 10 backend functions implemented
[ ] All 3 ISAs updated (x86-64, aarch64, riscv)
[ ] All 118 tests pass
[ ] No ASan/UBSan errors
[ ] ir_clear() called between functions
[ ] Two-pass flow verified
```
