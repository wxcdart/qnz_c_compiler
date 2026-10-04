/* Two-Pass IR: Instruction Buffer */
#ifndef ZERO_CC_IR_H
#define ZERO_CC_IR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

/* Maximum instructions per function: 64K = 65536 */
#define MAX_INSTRS 65536

/* Opcode definitions - 51 opcodes */
typedef enum {
    /* Data Movement (10) */
    OP_NOP = 0,
    OP_MOV_IMM,        /* mov rax, imm16 */
    OP_MOV_MEM_RAX,    /* mov [rbp - offset], rax */
    OP_MOV_RAX_MEM,    /* mov rax, [rbp - offset] */
    OP_MOV_RAX_RBX,    /* mov rax, rbx */
    OP_LEA_RAX_MEM,    /* lea rax, [rbp - offset] */
    OP_PUSH_RAX,       /* push rax */
    OP_POP_RAX,        /* pop rax */
    OP_PUSH_RBX,       /* push rbx */
    OP_POP_RBX,        /* pop rbx */

    /* Arithmetic (10) */
    OP_ADD_IMM,        /* add rax, imm16 */
    OP_SUB_IMM,        /* sub rax, imm16 */
    OP_ADD,            /* add rax, rbx */
    OP_SUB,            /* sub rax, rbx */
    OP_MUL,            /* imul rax, rbx */
    OP_DIV,            /* idiv rbx */
    OP_MOD,            /* idiv rbx, result in rdx */
    OP_NEG,            /* neg rax */
    OP_INC,            /* inc rax */
    OP_DEC,            /* dec rax */

    /* Logical (8) */
    OP_AND,            /* and rax, rbx */
    OP_OR,             /* or rax, rbx */
    OP_XOR,            /* xor rax, rbx */
    OP_NOT,            /* not rax */
    OP_LNOT,           /* !rax (test + sete + movzx) */
    OP_SHL,            /* shl rax, rbx */
    OP_SHR,            /* shr rax, rbx */
    OP_SAR,            /* sar rax, rbx */

    /* Comparison (6) */
    OP_CMP,            /* cmp rax, rbx */
    OP_TEST,           /* test rax, rax */
    OP_SETL,           /* setl al */
    OP_SETLE,          /* setle al */
    OP_SETG,           /* setg al */
    OP_SETGE,          /* setge al */

    /* Control Flow (6) */
    OP_JMP,            /* jmp label */
    OP_JE,             /* je label */
    OP_JNE,            /* jne label */
    OP_JL,             /* jl label */
    OP_JLE,            /* jle label */
    OP_JG,             /* jg label */
    /* Jump with label index (for break/continue/goto) */
    OP_JMP_LABEL,      /* jmp [label_idx] - break/continue/goto */

    /* Function (4) */
    OP_CALL,           /* call function */
    OP_CALL_INDIRECT,  /* call [r11] */
    OP_RET,            /* ret */
    /* Frame Management (5) */
    OP_PROLOGUE,       /* push rbp; mov rbp,rsp; sub rsp,frame_size (imm64) */
    OP_EPILOGUE,       /* callee-save loads; leave; ret */
    OP_PARAM_SAVE_INT, /* imm = reg_idx, imm64 = offset */
    OP_PARAM_SAVE_FP,  /* imm = reg_idx, imm64 = offset */
    OP_VARIADIC_SPILL, /* imm = param_count, imm64 = base_offset */

    /* Special (8) */
    OP_LABEL,          /* label marker */
    OP_DEREF,          /* mov rax, [rax] */
    OP_LOAD_BYTE,      /* movzx rax, byte ptr [rax] */
    OP_CMP_ZERO,       /* cmp rax, 0 */
    OP_SET_E,          /* sete al */
    OP_SET_NE,         /* setne al */
    OP_STACK_ALIGN,    /* sub rsp, imm8 */
    OP_CVTTSD2SI,      /* cvttsd2si eax, xmm0 */
    OP_CVTSI2SD,       /* cvtsi2sd xmm0, eax */
    OP_LOAD_16,        /* movzx rax, word ptr [rax] */
    OP_LOAD_32,        /* mov eax, dword ptr [rax] */
    OP_STORE_BYTE,     /* mov byte ptr [addr], src */
    OP_STORE_16,       /* mov word ptr [addr], src */
    OP_STORE_32,       /* mov dword ptr [addr], src */
    OP_STORE_64,       /* mov qword ptr [addr], src */
    OP_VLA_ALLOC,      /* sub rsp, size; mov dst, rsp */
    OP_ATOMIC_XADD,         /* lock xadd [addr], val */
    OP_ATOMIC_CMPXCHG,      /* lock cmpxchg [addr], desired (rax=expected) -> val */
    OP_ATOMIC_CMPXCHG_BOOL, /* lock cmpxchg [addr], desired (rax=expected) -> bool */
    OP_ATOMIC_XCHG,         /* xchg [addr], val */
    OP_MFENCE,              /* mfence */
    /* Floating-point operations (8) */
    OP_FMOV_IMM,       /* movsd xmm0, [rip + offset] - load float literal */
    OP_FLOAD,          /* movsd xmm0, [rbp - offset] - load float from stack */
    OP_FSTORE,         /* movsd [rbp - offset], xmm0 - store float to stack */
    OP_FADD,           /* addsd xmm0, xmm1 - float add */
    OP_FSUB,           /* subsd xmm0, xmm1 - float sub */
    OP_FMUL,           /* mulsd xmm0, xmm1 - float mul */
    OP_FDIV,           /* divsd xmm0, xmm1 - float div */
    OP_FMOV,           /* movsd xmm0, xmm1 - float move */
    /* RIP-relative addressing (2) */
    OP_LEA_RIP,        /* lea rax, [rip + offset] - string literal address */
    OP_MOV_GLOBAL,     /* mov rax, [rip + global] - global variable load with relocation */
} Opcode;

/* Helper: Get opcode name for debugging */
static inline const char* ir_opcode_name(Opcode op) {
    switch (op) {
        case OP_NOP: return "NOP";
        case OP_MOV_IMM: return "MOV_IMM";
        case OP_MOV_MEM_RAX: return "MOV_MEM_RAX";
        case OP_MOV_RAX_MEM: return "MOV_RAX_MEM";
        case OP_MOV_RAX_RBX: return "MOV_RAX_RBX";
        case OP_LEA_RAX_MEM: return "LEA_RAX_MEM";
        case OP_PUSH_RAX: return "PUSH_RAX";
        case OP_POP_RAX: return "POP_RAX";
        case OP_PUSH_RBX: return "PUSH_RBX";
        case OP_POP_RBX: return "POP_RBX";
        case OP_ADD_IMM: return "ADD_IMM";
        case OP_SUB_IMM: return "SUB_IMM";
        case OP_ADD: return "ADD";
        case OP_SUB: return "SUB";
        case OP_MUL: return "MUL";
        case OP_DIV: return "DIV";
        case OP_MOD: return "MOD";
        case OP_NEG: return "NEG";
        case OP_INC: return "INC";
        case OP_DEC: return "DEC";
        case OP_AND: return "AND";
        case OP_OR: return "OR";
        case OP_XOR: return "XOR";
        case OP_NOT: return "NOT";
        case OP_LNOT: return "LNOT";
        case OP_SHL: return "SHL";
        case OP_SHR: return "SHR";
        case OP_SAR: return "SAR";
        case OP_CMP: return "CMP";
        case OP_TEST: return "TEST";
        case OP_SETL: return "SETL";
        case OP_SETLE: return "SETLE";
        case OP_SETG: return "SETG";
        case OP_SETGE: return "SETGE";
        case OP_JMP: return "JMP";
        case OP_JE: return "JE";
        case OP_JNE: return "JNE";
        case OP_JL: return "JL";
        case OP_JLE: return "JLE";
        case OP_JG: return "JG";
        case OP_JMP_LABEL: return "JMP_LABEL";
        case OP_CALL: return "CALL";
        case OP_CALL_INDIRECT: return "CALL_INDIRECT";
        case OP_RET: return "RET";
        case OP_PROLOGUE: return "PROLOGUE";
        case OP_EPILOGUE: return "EPILOGUE";
        case OP_PARAM_SAVE_INT: return "PARAM_SAVE_INT";
        case OP_PARAM_SAVE_FP: return "PARAM_SAVE_FP";
        case OP_VARIADIC_SPILL: return "VARIADIC_SPILL";
        case OP_LABEL: return "LABEL";
        case OP_DEREF: return "DEREF";
        case OP_LOAD_BYTE: return "LOAD_BYTE";
        case OP_CMP_ZERO: return "CMP_ZERO";
        case OP_SET_E: return "SET_E";
        case OP_SET_NE: return "SET_NE";
        case OP_STACK_ALIGN: return "STACK_ALIGN";
        case OP_CVTTSD2SI: return "CVTTSD2SI";
        case OP_CVTSI2SD: return "CVTSI2SD";
        case OP_LOAD_16: return "LOAD_16";
        case OP_LOAD_32: return "LOAD_32";
        case OP_STORE_BYTE: return "STORE_BYTE";
        case OP_STORE_16: return "STORE_16";
        case OP_STORE_32: return "STORE_32";
        case OP_STORE_64: return "STORE_64";
        case OP_VLA_ALLOC: return "VLA_ALLOC";
        case OP_ATOMIC_XADD: return "ATOMIC_XADD";
        case OP_ATOMIC_CMPXCHG: return "ATOMIC_CMPXCHG";
        case OP_ATOMIC_CMPXCHG_BOOL: return "ATOMIC_CMPXCHG_BOOL";
        case OP_ATOMIC_XCHG: return "ATOMIC_XCHG";
        case OP_MFENCE: return "MFENCE";
        case OP_FMOV_IMM: return "FMOV_IMM";
        case OP_FLOAD: return "FLOAD";
        case OP_FSTORE: return "FSTORE";
        case OP_FADD: return "FADD";
        case OP_FSUB: return "FSUB";
        case OP_FMUL: return "FMUL";
        case OP_FDIV: return "FDIV";
        case OP_FMOV: return "FMOV";
        case OP_LEA_RIP: return "LEA_RIP";
        case OP_MOV_GLOBAL: return "MOV_GLOBAL";
        default: return "UNKNOWN";
    }
}

/* Check if opcode is a floating-point operation */
static inline bool ir_opcode_is_float(Opcode op) {
    switch (op) {
        case OP_FMOV_IMM:
        case OP_FLOAD:
        case OP_FSTORE:
        case OP_FADD:
        case OP_FSUB:
        case OP_FMUL:
        case OP_FDIV:
        case OP_FMOV:
        case OP_CVTSI2SD:  /* int -> float conversion uses xmm0 as dest */
        case OP_CVTTSD2SI: /* float -> int conversion uses xmm0 as src */
            return true;
        default:
            return false;
    }
}

/* Instruction structure - 8 bytes packed */
typedef struct __attribute__((packed)) {
    uint8_t  op;        /* Operation code (0-255) */
    uint8_t  temp_dest; /* Destination temp (0-255, 0xFF if none) */
    uint8_t  temp_src1; /* Source temp 1 (0-255, 0xFF if none) */
    uint8_t  temp_src2; /* Source temp 2 (0-255, 0xFF if none) */
    int16_t  imm;       /* Immediate value (-32768 to 32767) */
    int64_t  imm64;     /* 64-bit immediate value */
    uint16_t label_idx; /* Label index for jumps (0-65535) */
} Instr;

/* Global instruction buffer (shared across all functions) */
/* Defined in ir.c, declared extern here */
extern Instr instrs[MAX_INSTRS];
extern size_t instr_count;

/* Integer argument temps for OP_CALL instructions. The packed Instr can only
 * hold 4 argument temps, so every call records its full list here, keyed by
 * the IR instruction index of the OP_CALL. Defined in ir.c. */
#define IR_CALL_MAX_ARGS 16
#define IR_CALL_MAX_SITES 1024
typedef struct {
    size_t instr_index;
    int count;
    uint8_t temps[IR_CALL_MAX_ARGS];
} IRCallArgs;
extern IRCallArgs ir_call_args_table[IR_CALL_MAX_SITES];
extern size_t ir_call_args_count;

/* Record the integer argument temps of the OP_CALL at instr_index */
static inline void ir_call_args_record(size_t instr_index, int count, const int *temps) {
    if (ir_call_args_count >= IR_CALL_MAX_SITES) return;
    IRCallArgs *e = &ir_call_args_table[ir_call_args_count++];
    e->instr_index = instr_index;
    e->count = count > IR_CALL_MAX_ARGS ? IR_CALL_MAX_ARGS : count;
    for (int i = 0; i < e->count; i++) e->temps[i] = (uint8_t)temps[i];
}

/* Find the recorded argument list for the OP_CALL at instr_index (or NULL) */
static inline const IRCallArgs *ir_call_args_find(size_t instr_index) {
    for (size_t i = 0; i < ir_call_args_count; i++) {
        if (ir_call_args_table[i].instr_index == instr_index) return &ir_call_args_table[i];
    }
    return NULL;
}

/* Get instruction buffer (for live range analysis) */
static inline const Instr *ir_get_buffer(void) {
    return instrs;
}

/* Emit instruction - O(1) */
void ir_emit(Instr instr);

/* Label and jump instructions */
void ir_emit_label(uint16_t label_idx);
void ir_emit_jmp(uint16_t label_idx);
void ir_emit_je(uint8_t temp_cond, uint16_t label_idx);

/* Reset for next function - O(1) */
static inline void ir_reset(void) {
    instr_count = 0;
    ir_call_args_count = 0;
}

/* Get instruction count */
static inline size_t ir_get_count(void) {
    return instr_count;
}

/* Access instruction by index */
static inline Instr* ir_get(size_t idx) {
    if (idx >= instr_count) return NULL;
    return &instrs[idx];
}

/* Helper functions for common patterns */
static inline void ir_emit_mov_imm(uint8_t temp_dest, int64_t imm) {
    ir_emit((Instr){.op = OP_MOV_IMM, .temp_dest = temp_dest, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = (int16_t)imm, .imm64 = imm, .label_idx = 0});
}

static inline void ir_emit_add(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_ADD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_sub(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_SUB, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_mul(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_MUL, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Label and jump instructions */
/* Defined in ir.c */
extern void ir_emit_label(uint16_t label_idx);
extern void ir_emit_jmp(uint16_t label_idx);
extern void ir_emit_je(uint8_t temp_cond, uint16_t label_idx);
extern void ir_emit_jne(uint8_t temp_cond, uint16_t label_idx);
extern void ir_emit_jmp_label(uint16_t label_idx);

static inline void ir_emit_ret(void) {
    ir_emit((Instr){.op = OP_RET, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

/* Emit function prologue (frame_size in imm64) */
static inline void ir_emit_prologue(int64_t frame_size) {
    ir_emit((Instr){.op = OP_PROLOGUE, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .imm64 = frame_size, .label_idx = 0});
}

/* Emit function epilogue */
static inline void ir_emit_epilogue(void) {
    ir_emit((Instr){.op = OP_EPILOGUE, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_param_save_int(int reg_idx, int32_t offset) {
    ir_emit((Instr){.op = OP_PARAM_SAVE_INT, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = (int16_t)reg_idx, .imm64 = (int64_t)offset, .label_idx = 0});
}

static inline void ir_emit_param_save_fp(int reg_idx, int32_t offset) {
    ir_emit((Instr){.op = OP_PARAM_SAVE_FP, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = (int16_t)reg_idx, .imm64 = (int64_t)offset, .label_idx = 0});
}

static inline void ir_emit_variadic_spill(int param_count, int32_t base_offset) {
    ir_emit((Instr){.op = OP_VARIADIC_SPILL, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = (int16_t)param_count, .imm64 = (int64_t)base_offset, .label_idx = 0});
}

/* Emit function call - fn_name stored in imm64 as pointer during lowering */
static inline void ir_emit_call(const char *fn_name) {
    ir_emit((Instr){.op = OP_CALL, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .imm64 = (int64_t)(intptr_t)fn_name, .label_idx = 0});
}

/* Float to int conversion (cvttsd2si) */
static inline void ir_emit_cvttsd2si(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_CVTTSD2SI, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

/* Int to float conversion (cvtsi2sd) */
static inline void ir_emit_cvtsi2sd(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_CVTSI2SD, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_load_16(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_LOAD_16, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_load_32(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_LOAD_32, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_store_byte(uint8_t temp_val, uint8_t temp_addr) {
    ir_emit((Instr){.op = OP_STORE_BYTE, .temp_dest = 0xFF, .temp_src1 = temp_val, .temp_src2 = temp_addr, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_store_16(uint8_t temp_val, uint8_t temp_addr) {
    ir_emit((Instr){.op = OP_STORE_16, .temp_dest = 0xFF, .temp_src1 = temp_val, .temp_src2 = temp_addr, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_store_32(uint8_t temp_val, uint8_t temp_addr) {
    ir_emit((Instr){.op = OP_STORE_32, .temp_dest = 0xFF, .temp_src1 = temp_val, .temp_src2 = temp_addr, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_store_64(uint8_t temp_val, uint8_t temp_addr) {
    ir_emit((Instr){.op = OP_STORE_64, .temp_dest = 0xFF, .temp_src1 = temp_val, .temp_src2 = temp_addr, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_vla_alloc(uint8_t temp_dest, uint8_t temp_size) {
    ir_emit((Instr){.op = OP_VLA_ALLOC, .temp_dest = temp_dest, .temp_src1 = temp_size, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_atomic_xadd(uint8_t temp_dest, uint8_t temp_addr, uint8_t temp_val, int16_t width) {
    ir_emit((Instr){.op = OP_ATOMIC_XADD, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_val, .imm = width, .label_idx = 0});
}

static inline void ir_emit_atomic_cmpxchg(uint8_t temp_dest, uint8_t temp_addr, uint8_t temp_expected, uint8_t temp_desired, int16_t width) {
    ir_emit((Instr){.op = OP_ATOMIC_CMPXCHG, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_desired, .imm = width, .label_idx = (uint16_t)temp_expected});
}

static inline void ir_emit_atomic_cmpxchg_bool(uint8_t temp_dest, uint8_t temp_addr, uint8_t temp_expected, uint8_t temp_desired, int16_t width) {
    ir_emit((Instr){.op = OP_ATOMIC_CMPXCHG_BOOL, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_desired, .imm = width, .label_idx = (uint16_t)temp_expected});
}

static inline void ir_emit_atomic_xchg(uint8_t temp_dest, uint8_t temp_addr, uint8_t temp_val, int16_t width) {
    ir_emit((Instr){.op = OP_ATOMIC_XCHG, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_val, .imm = width, .label_idx = 0});
}

static inline void ir_emit_mfence(void) {
    ir_emit((Instr){.op = OP_MFENCE, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

/* Float literal load via RIP-relative (imm64 holds rodata offset) */
static inline void ir_emit_fmov_imm(uint8_t temp_dest, int64_t rodata_offset) {
    ir_emit((Instr){.op = OP_FMOV_IMM, .temp_dest = temp_dest, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .imm64 = rodata_offset, .label_idx = 0});
}

/* Float load from stack */
static inline void ir_emit_fload(uint8_t temp_dest, uint8_t temp_base, int16_t offset) {
    ir_emit((Instr){.op = OP_FLOAD, .temp_dest = temp_dest, .temp_src1 = temp_base, .temp_src2 = 0xFF, .imm = offset, .label_idx = 0});
}

/* Float store to stack */
static inline void ir_emit_fstore(uint8_t temp_src, uint8_t temp_base, int16_t offset) {
    ir_emit((Instr){.op = OP_FSTORE, .temp_dest = 0xFF, .temp_src1 = temp_src, .temp_src2 = temp_base, .imm = offset, .label_idx = 0});
}

/* Float add: xmm0 = xmm0 + xmm1 */
static inline void ir_emit_fadd(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_FADD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float sub: xmm0 = xmm0 - xmm1 */
static inline void ir_emit_fsub(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_FSUB, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float mul: xmm0 = xmm0 * xmm1 */
static inline void ir_emit_fmul(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_FMUL, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float div: xmm0 = xmm0 / xmm1 */
static inline void ir_emit_fdiv(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_FDIV, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float move: xmm0 = xmm1 */
static inline void ir_emit_fmov(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_FMOV, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

/* LEA RIP-relative (imm64 holds rodata offset) */
static inline void ir_emit_lea_rip(uint8_t temp_dest, int64_t rodata_offset) {
    ir_emit((Instr){.op = OP_LEA_RIP, .temp_dest = temp_dest, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .imm64 = rodata_offset, .label_idx = 0});
}

/* Global variable load with relocation (imm64 holds symbol name pointer) */
static inline void ir_emit_mov_global(uint8_t temp_dest, const char *symbol_name) {
    ir_emit((Instr){.op = OP_MOV_GLOBAL, .temp_dest = temp_dest, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .imm64 = (int64_t)(intptr_t)symbol_name, .label_idx = 0});
}

#endif /* ZERO_CC_IR_H */

/* Debug: Print IR count */