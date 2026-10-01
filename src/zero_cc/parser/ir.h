/* Two-Pass IR: Instruction Buffer */
#ifndef ZERO_CC_IR_H
#define ZERO_CC_IR_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

/* Maximum instructions per function: 64K = 65536 */
#define MAX_INSTRS 65536

/* Opcode definitions - 50 opcodes */
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

    /* Function (4) */
    OP_CALL,           /* call function */
    OP_CALL_INDIRECT,  /* call [r11] */
    OP_RET,            /* ret */
    OP_LEAVE,          /* leave */

    /* Special (7) */
    OP_LABEL,          /* label marker */
    OP_DEREF,          /* mov rax, [rax] */
    OP_LOAD_BYTE,      /* movzx rax, byte ptr [rax] */
    OP_CMP_ZERO,       /* cmp rax, 0 */
    OP_SET_E,          /* sete al */
    OP_SET_NE,         /* setne al */
    OP_STACK_ALIGN,    /* sub rsp, imm8 */
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
        case OP_CALL: return "CALL";
        case OP_CALL_INDIRECT: return "CALL_INDIRECT";
        case OP_RET: return "RET";
        case OP_LEAVE: return "LEAVE";
        case OP_LABEL: return "LABEL";
        case OP_DEREF: return "DEREF";
        case OP_LOAD_BYTE: return "LOAD_BYTE";
        case OP_CMP_ZERO: return "CMP_ZERO";
        case OP_SET_E: return "SET_E";
        case OP_SET_NE: return "SET_NE";
        case OP_STACK_ALIGN: return "STACK_ALIGN";
        default: return "UNKNOWN";
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

/* Get instruction buffer (for live range analysis) */
static inline const Instr *ir_get_buffer(void) {
    return instrs;
}

/* Emit instruction - O(1) */
static inline void ir_emit(Instr instr) {
    if (instr_count >= MAX_INSTRS) {
        fprintf(stderr, "Error: Function too large (>64K instructions)\n");
        exit(1);
    }
    instrs[instr_count++] = instr;
}

/* Reset for next function - O(1) */
static inline void ir_reset(void) {
    instr_count = 0;
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
static inline void ir_emit_mov_imm(uint8_t temp_dest, int16_t imm) {
    ir_emit((Instr){.op = OP_MOV_IMM, .temp_dest = temp_dest, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = imm, .imm64 = imm, .label_idx = 0});
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

static inline void ir_emit_label(uint16_t label_idx) {
    ir_emit((Instr){.op = OP_LABEL, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .label_idx = label_idx});
}

static inline void ir_emit_jmp(uint16_t label_idx) {
    ir_emit((Instr){.op = OP_JMP, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .label_idx = label_idx});
}

static inline void ir_emit_je(uint8_t temp_cond, uint16_t label_idx) {
    ir_emit((Instr){.op = OP_JE, .temp_dest = 0xFF, .temp_src1 = temp_cond, .temp_src2 = 0xFF, .imm = 0, .label_idx = label_idx});
}

static inline void ir_emit_jne(uint8_t temp_cond, uint16_t label_idx) {
    ir_emit((Instr){.op = OP_JNE, .temp_dest = 0xFF, .temp_src1 = temp_cond, .temp_src2 = 0xFF, .imm = 0, .label_idx = label_idx});
}

static inline void ir_emit_ret(void) {
    ir_emit((Instr){.op = OP_RET, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

#endif /* ZERO_CC_IR_H */

/* Debug: Print IR count */
