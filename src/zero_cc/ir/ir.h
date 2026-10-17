/* Two-Pass IR: Instruction Buffer */
#ifndef ZERO_CC_IR_H
#define ZERO_CC_IR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

/* Forward declaration */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Thread-local compiler pointer for IR emission (set during compilation) */
extern __thread ZeroCCCompiler *g_compiler;

/* Special register constants */
#define REG_FLAGS 255  /* Special flag register for explicit flag dependency tracking */

/* Call argument tracking constants */
#define IR_CALL_MAX_ARGS 16
#define IR_CALL_MAX_SITES 1024

/* Call argument tracking structure */
typedef struct {
    size_t instr_index;
    int count;
    uint8_t temps[IR_CALL_MAX_ARGS];
    uint8_t is_fp[IR_CALL_MAX_ARGS];  /* 1 if FP argument, 0 if integer */
} IRCallArgs;

/* Call args arena for tracking */
typedef struct {
    IRCallArgs *table;
    size_t count;
    size_t capacity;
} IRCallArgsArena;

/* Special register constants */
#define REG_FLAGS 255  /* Special flag register for explicit flag dependency tracking */

/* Opcode definitions - 51 opcodes */
typedef enum {
    /* Data Movement (10) */
    OP_NOP = 0,
    OP_MOV_IMM,        /* move immediate value to register */
    OP_MOV_MEM,        /* store register to stack slot */
    OP_MOV,            /* load from stack slot to register */
    OP_MOV_REG,        /* move register to register */
    OP_LEA,            /* load effective address from stack slot */
    OP_PUSH,           /* push register to stack */
    OP_POP,            /* pop register from stack */

    /* Arithmetic (10) */
    OP_ADD_IMM,        /* add immediate to register */
    OP_SUB_IMM,        /* subtract immediate from register */
    OP_ADD,            /* add two registers */
    OP_SUB,            /* subtract two registers */
    OP_MUL,            /* signed multiply two registers */
    OP_DIV,            /* signed divide by register */
    OP_MOD,            /* signed modulo (remainder of division) */
    OP_NEG,            /* negate register */
    OP_INC,            /* increment register */
    OP_DEC,            /* decrement register */

    /* Logical (8) */
    OP_AND,            /* bitwise AND two registers */
    OP_OR,             /* bitwise OR two registers */
    OP_XOR,            /* bitwise XOR two registers */
    OP_NOT,            /* bitwise NOT register */
    OP_LNOT,           /* logical NOT (test + sete + movzx) */
    OP_SHL,            /* shift left register by register */
    OP_SHR,            /* shift right register by register (unsigned) */
    OP_SAR,            /* shift right register by register (signed) */

    /* Comparison (2) */
    OP_CMP,            /* compare src1, src2; sets flags */
    OP_COND_SET,       /* set dst based on flags and condition code */

    /* Control Flow (7) */
    OP_JMP,            /* jmp label */
    OP_JE,             /* je label */
    OP_JNE,            /* jne label */
    OP_JL,             /* jl label */
    OP_JLE,            /* jle label */
    OP_JG,             /* jg label */
    OP_JCOND,          /* conditional jump with explicit flags */
    /* Jump with label index (for break/continue/goto) */
    OP_JMP_LABEL,      /* jmp [label_idx] - break/continue/goto */

    /* Function (4) */
    OP_CALL,           /* call function */
    OP_CALL_INDIRECT,  /* call through register */
    OP_RET,            /* return from function */
    /* Frame Management (5) */
    OP_PROLOGUE,       /* function prologue (frame setup) */
    OP_EPILOGUE,       /* callee-save loads; return sequence */
    OP_DEAD,           /* dead instruction (removed by DCE) */
    OP_PARAM_SAVE_INT, /* save integer argument register to stack slot */
    OP_PARAM_SAVE_FP,  /* save floating-point argument register to stack slot */
    OP_VARIADIC_SPILL, /* spill variadic argument registers to stack */

    /* Special (8) */
    OP_LABEL,          /* label marker */
    OP_DEREF,          /* load value from pointer address */
    OP_LOAD_BYTE,      /* load byte with zero-extension */
    OP_CMP_ZERO,       /* compare register with zero */
    OP_STACK_ALIGN,    /* align stack pointer */
    OP_CVTTSD2SI,      /* convert float to integer (truncate) */
    OP_CVTSI2SD,       /* convert integer to float */
    OP_LOAD_16,        /* load 16-bit value with zero-extension */
    OP_LOAD_32,        /* load 32-bit value */
    OP_STORE_BYTE,     /* store byte to address */
    OP_STORE_16,       /* store 16-bit value to address */
    OP_STORE_32,       /* store 32-bit value to address */
    OP_STORE_64,       /* store 64-bit value to address */
    OP_VLA_ALLOC,      /* allocate variable-length array on stack */
    OP_CALLEE_SAVE_STORES, /* emit callee-saved register stores */
    OP_CALLEE_SAVE_LOADS,  /* emit callee-saved register loads */
    OP_ATOMIC_XADD,         /* atomic exchange and add */
    OP_ATOMIC_CMPXCHG,      /* atomic compare and exchange (returns old value) */
    OP_ATOMIC_CMPXCHG_BOOL, /* atomic compare and exchange (returns success) */
    OP_ATOMIC_XCHG,         /* atomic exchange */
    OP_MFENCE,              /* memory fence (serialization) */
    /* SIMD/Vector operations (32) */
    OP_VADDSS,     /* scalar single-precision add */
    OP_VSUBSS,     /* scalar single-precision sub */
    OP_VMULSS,     /* scalar single-precision mul */
    OP_VDIVSS,     /* scalar single-precision div */
    OP_VADDSD,     /* scalar double-precision add */
    OP_VSUBSD,     /* scalar double-precision sub */
    OP_VMULSD,     /* scalar double-precision mul */
    OP_VDIVSD,     /* scalar double-precision div */
    OP_VMOV,       /* vector move (128-bit) */
    OP_VMOVSS,     /* scalar single-precision move */
    OP_VMOVSD,     /* scalar double-precision move */
    OP_VLOAD,      /* vector load from memory */
    OP_VSTORE,     /* vector store to memory */
    OP_VLOADSS,    /* load scalar single */
    OP_VLOADSD,    /* load scalar double */
    OP_VSTORESS,   /* store scalar single */
    OP_VSTORESD,   /* store scalar double */
    OP_VCVT_SI2SS, /* scalar int to single */
    OP_VCVT_SS2SI, /* scalar single to int */
    OP_VCVT_SI2SD, /* scalar int to double */
    OP_VCVT_SD2SI, /* scalar double to int */
    OP_VCMPSS,     /* scalar single comparison */
    OP_VCMPSD,     /* scalar double comparison */
    OP_VAND,       /* vector bitwise AND */
    OP_VOR,        /* vector bitwise OR */
    OP_VXOR,       /* vector bitwise XOR */
    OP_VSHUF,      /* vector shuffle */
    OP_VANDN,      /* vector bitwise AND NOT */
    OP_VORPS,      /* orps (merge) */
    OP_VSUBPS,     /* subps (packed) */
    OP_VMULPS,     /* mulps (packed) */
    OP_VADDPS,     /* addps (packed single) */
    OP_VDIVPS,     /* divps (packed single) */
    OP_VSUBPD,     /* subpd (packed double) */
    OP_VMULPD,     /* mulpd (packed double) */
    OP_VADDPD,     /* addpd (packed double) */
    OP_VDIVPD,     /* divpd (packed double) */
    OP_VMOVAPS,    /* movaps (aligned packed single) */
    OP_VMOVAPD,    /* movapd (aligned packed double) */
    OP_VANDPS,     /* andps (packed single) */
    OP_VXORPS,     /* xorps (packed single) */
    OP_VSHUFPS,    /* shufps (shuffle packed single) */
    OP_VPADDD,     /* paddd (packed 32-bit integer add) */
    OP_VPSUBD,     /* psubd (packed 32-bit integer subtract) */
    OP_VPMULLD,    /* pmulld (packed 32-bit integer multiply) */
    /* Floating-point operations (8) */
    OP_FMOV_IMM,       /* load floating-point literal */
    OP_FLOAD,          /* load float from stack slot */
    OP_FSTORE,         /* store float to stack slot */
    OP_FADD,           /* floating-point add */
    OP_FSUB,           /* floating-point subtract */
    OP_FMUL,           /* floating-point multiply */
    OP_FDIV,           /* floating-point divide */
    OP_FMOV,           /* move floating-point value */
    /* RIP-relative addressing (2) */
    OP_LEA_RIP,        /* load effective address (position-independent) */
    OP_MOV_GLOBAL,     /* load global variable (position-independent) */

    /* C99 width extension (6) - appended so existing enum values are stable */
    OP_SEXT8,          /* in-place sign-extend lower 8 bits of temp_src1 */
    OP_SEXT16,         /* in-place sign-extend lower 16 bits of temp_src1 */
    OP_SEXT32,         /* in-place sign-extend lower 32 bits of temp_src1 */
    OP_ZEXT8,          /* in-place zero-extend lower 8 bits of temp_src1 */
    OP_ZEXT16,         /* in-place zero-extend lower 16 bits of temp_src1 */
    OP_ZEXT32,         /* in-place zero-extend lower 32 bits of temp_src1 */
} Opcode;

/* Condition codes for OP_COND_SET */
#define CC_LT  0  /* signed less than */
#define CC_LE  1  /* signed less or equal */
#define CC_GT  2  /* signed greater */
#define CC_GE  3  /* signed greater or equal */
#define CC_EQ  4  /* equal */
#define CC_NE  5  /* not equal */

/* Helper: Get opcode name for debugging */
static inline const char* ir_opcode_name(Opcode op) {
    switch (op) {
        case OP_NOP: return "NOP";
        case OP_MOV_IMM: return "MOV_IMM";
        case OP_MOV_MEM: return "MOV_MEM";
        case OP_MOV: return "MOV";
        case OP_MOV_REG: return "MOV_REG";
        case OP_LEA: return "LEA";
        case OP_PUSH: return "PUSH";
        case OP_POP: return "POP";
        case OP_ADD_IMM: return "ADD_IMM";
        case OP_SUB_IMM: return "SUB_IMM";
        case OP_ADD: return "ADD";
        case OP_SUB: return "SUB";
        case OP_MUL: return "MUL";
        case OP_DIV: return "DIV";
        case OP_MOD: return "MOD";
        case OP_NEG: return "NEG";
        case OP_SEXT8: return "SEXT8";
        case OP_SEXT16: return "SEXT16";
        case OP_SEXT32: return "SEXT32";
        case OP_ZEXT8: return "ZEXT8";
        case OP_ZEXT16: return "ZEXT16";
        case OP_ZEXT32: return "ZEXT32";
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
        case OP_COND_SET: return "COND_SET";
        case OP_JMP: return "JMP";
        case OP_JE: return "JE";
        case OP_JNE: return "JNE";
        case OP_JL: return "JL";
        case OP_JLE: return "JLE";
        case OP_JG: return "JG";
        case OP_JCOND: return "JCOND";
        case OP_JMP_LABEL: return "JMP_LABEL";
        case OP_CALL: return "CALL";
        case OP_CALL_INDIRECT: return "CALL_INDIRECT";
        case OP_RET: return "RET";
        case OP_PROLOGUE: return "PROLOGUE";
        case OP_EPILOGUE: return "EPILOGUE";
        case OP_DEAD: return "DEAD";
        case OP_PARAM_SAVE_INT: return "PARAM_SAVE_INT";
        case OP_PARAM_SAVE_FP: return "PARAM_SAVE_FP";
        case OP_VARIADIC_SPILL: return "VARIADIC_SPILL";
        case OP_LABEL: return "LABEL";
        case OP_DEREF: return "DEREF";
        case OP_LOAD_BYTE: return "LOAD_BYTE";
        case OP_CMP_ZERO: return "CMP_ZERO";
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
        case OP_VADDSS: return "VADDSS";
        case OP_VSUBSS: return "VSUBSS";
        case OP_VMULSS: return "VMULSS";
        case OP_VDIVSS: return "VDIVSS";
        case OP_VADDSD: return "VADDSD";
        case OP_VSUBSD: return "VSUBSD";
        case OP_VMULSD: return "VMULSD";
        case OP_VDIVSD: return "VDIVSD";
        case OP_VMOV: return "VMOV";
        case OP_VMOVSS: return "VMOVSS";
        case OP_VMOVSD: return "VMOVSD";
        case OP_VLOAD: return "VLOAD";
        case OP_VSTORE: return "VSTORE";
        case OP_VLOADSS: return "VLOADSS";
        case OP_VLOADSD: return "VLOADSD";
        case OP_VSTORESS: return "VSTORESS";
        case OP_VSTORESD: return "VSTORESD";
        case OP_VCVT_SI2SS: return "VCVT_SI2SS";
        case OP_VCVT_SS2SI: return "VCVT_SS2SI";
        case OP_VCVT_SI2SD: return "VCVT_SI2SD";
        case OP_VCVT_SD2SI: return "VCVT_SD2SI";
        case OP_VCMPSS: return "VCMPSS";
        case OP_VCMPSD: return "VCMPSD";
        case OP_VAND: return "VAND";
        case OP_VOR: return "VOR";
        case OP_VXOR: return "VXOR";
        case OP_VSHUF: return "VSHUF";
        case OP_VANDN: return "VANDN";
        case OP_VORPS: return "VORPS";
        case OP_VSUBPS: return "VSUBPS";
        case OP_VMULPS: return "VMULPS";
        case OP_VADDPS: return "VADDPS";
        case OP_VDIVPS: return "VDIVPS";
        case OP_VSUBPD: return "VSUBPD";
        case OP_VMULPD: return "VMULPD";
        case OP_VADDPD: return "VADDPD";
        case OP_VDIVPD: return "VDIVPD";
        case OP_VMOVAPS: return "VMOVAPS";
        case OP_VMOVAPD: return "VMOVAPD";
        case OP_VANDPS: return "VANDPS";
        case OP_VXORPS: return "VXORPS";
        case OP_VSHUFPS: return "VSHUFPS";
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
        case OP_CVTSI2SD:  /* int -> float conversion */
        case OP_CVTTSD2SI: /* float -> int conversion (truncate) */
        case OP_VLOADSS:   /* scalar 4-byte load: result is a float temp (XMM pool) */
        case OP_VLOADSD:   /* scalar 8-byte load: result is a float temp (XMM pool) */
            return true;
        default:
            return false;
    }
}

/* Check if opcode is a SIMD operation */
static inline bool ir_opcode_is_simd(Opcode op) {
    switch (op) {
        case OP_VADDSS: case OP_VSUBSS: case OP_VMULSS: case OP_VDIVSS:
        case OP_VADDSD: case OP_VSUBSD: case OP_VMULSD: case OP_VDIVSD:
        case OP_VMOV: case OP_VMOVSS: case OP_VMOVSD:
        case OP_VLOAD: case OP_VSTORE:
        case OP_VSTORESS: case OP_VSTORESD:
        case OP_VCVT_SI2SS: case OP_VCVT_SS2SI:
        case OP_VCVT_SI2SD: case OP_VCVT_SD2SI:
        case OP_VCMPSS: case OP_VCMPSD:
        case OP_VAND: case OP_VOR: case OP_VXOR:
        case OP_VSHUF: case OP_VANDN:
        case OP_VORPS: case OP_VSUBPS: case OP_VMULPS:
        case OP_VADDPS: case OP_VDIVPS:
        case OP_VSUBPD: case OP_VMULPD: case OP_VADDPD: case OP_VDIVPD:
        case OP_VMOVAPS: case OP_VMOVAPD:
        case OP_VANDPS: case OP_VXORPS: case OP_VSHUFPS:
        case OP_VPADDD: case OP_VPSUBD: case OP_VPMULLD:
            return true;
        default:
            return false;
    }
}

/* Instruction structure - 19 bytes packed */
typedef struct __attribute__((packed)) {
    uint8_t  op;          /* Operation code (0-255) */
    uint8_t  temp_dest;   /* Destination temp (0-255, 0xFF if none) */
    uint8_t  temp_src1;   /* Source temp 1 (0-255, 0xFF if none) */
    uint8_t  temp_src2;   /* Source temp 2 (0-255, 0xFF if none) */
    uint8_t  cond;        /* Condition code for OP_COND_SET (0-5) */
    uint8_t  flags_out;   /* Flags output register (REG_FLAGS or 0xFF) */
    uint8_t  flags_in;    /* Flags input register (REG_FLAGS or 0xFF) */
    uint8_t  alias_group; /* Alias group for restrict pointer tracking (0 = unconstrained) */
    uint8_t  width;       /* C99 result width: 0=none, 1/2/4=sign-extend, 8+1/2/4=zero-extend */
    int16_t  imm;         /* Immediate value (-32768 to 32767) */
    int64_t  imm64;       /* 64-bit immediate value */
    uint16_t label_idx;   /* Label index for jumps (0-65535) */
} Instr;

/* IR Access Functions - Arena-based (per-compiler-instance) */

/* Set current compiler for IR emission (call before emitting IR) */
void ir_set_compiler(ZeroCCCompiler *c);

/* Get instruction buffer (for live range analysis) */
const Instr *ir_get_buffer(void);

/* Emit instruction - O(1) amortized */
void ir_emit(Instr instr);

/* Label and jump instructions */
void ir_emit_label(uint16_t label_idx);
void ir_emit_jmp(uint16_t label_idx);
void ir_emit_je(uint8_t temp_cond, uint16_t label_idx);
void ir_emit_jcond(uint8_t temp_cond, uint16_t label_idx);

/* Reset for next function - O(1), reuses memory */
void ir_reset(void);

/* Clear IR buffer after machine code emission */
void ir_clear(void);

/* Debug: Dump IR to file */
void ir_dump(FILE *f);

/* Get instruction count */
size_t ir_get_count(void);

/* Access instruction by index */
Instr* ir_get(size_t idx);

/* Mark the most recently emitted instruction with a C99 result width so the
 * MC layer extends the 64-bit result to true N-bit wrap semantics:
 * w in {1,2,4} = sign-extend, w in {9,10,12} = zero-extend. */
static inline void ir_mark_last_width(uint8_t w) {
    size_t n = ir_get_count();
    if (n > 0) ir_get(n - 1)->width = w;
}

/* Insert instruction at specific index (shifts existing instructions) */
void ir_insert_at(size_t idx, Instr instr);

/* Helper: Insert MOV_IMM at index */
static inline void ir_insert_mov_imm(size_t idx, uint8_t temp_dest, int64_t imm) {
    ir_insert_at(idx, (Instr){.op = OP_MOV_IMM, .temp_dest = temp_dest, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = (int16_t)imm, .imm64 = imm, .label_idx = 0});
}

/* Helper: Insert MOV_REG at index */
static inline void ir_insert_mov_reg(size_t idx, uint8_t temp_dest, uint8_t temp_src) {
    ir_insert_at(idx, (Instr){.op = OP_MOV_REG, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

/* Helper: Insert ADD at index */
static inline void ir_insert_add(size_t idx, uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_insert_at(idx, (Instr){.op = OP_ADD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Helper: Insert LABEL at index */
static inline void ir_insert_label(size_t idx, uint16_t label_idx) {
    ir_insert_at(idx, (Instr){.op = OP_LABEL, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx});
}

/* Call argument tracking - arena-based */
void ir_call_args_record(size_t instr_index, int count, const int *temps, const int *is_fp);
const IRCallArgs *ir_call_args_find(size_t instr_index);

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
extern void ir_emit_jcond(uint8_t temp_cond, uint16_t label_idx);
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

/* Emit callee-saved register stores (after register allocation) */
static inline void ir_emit_callee_save_stores(uint32_t callee_save_mask) {
    ir_emit((Instr){.op = OP_CALLEE_SAVE_STORES, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = (int16_t)(callee_save_mask & 0xFFFF), .imm64 = callee_save_mask, .label_idx = 0});
}

/* Emit callee-saved register loads (after register allocation) */
static inline void ir_emit_callee_save_loads(uint32_t callee_save_mask) {
    ir_emit((Instr){.op = OP_CALLEE_SAVE_LOADS, .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF, .imm = (int16_t)(callee_save_mask & 0xFFFF), .imm64 = callee_save_mask, .label_idx = 0});
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

/* Float add: dest = src1 + src2 */
static inline void ir_emit_fadd(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_FADD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float sub: dest = src1 - src2 */
static inline void ir_emit_fsub(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_FSUB, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float mul: dest = src1 * src2 */
static inline void ir_emit_fmul(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_FMUL, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float div: dest = src1 / src2 */
static inline void ir_emit_fdiv(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_FDIV, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float move: dest = src */
static inline void ir_emit_fmov(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_FMOV, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}

/* SIMD emission functions */
/* Scalar single-precision operations */
static inline void ir_emit_vaddss(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VADDSS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vsubss(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VSUBSS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vmulss(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VMULSS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vdivss(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VDIVSS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
/* Scalar double-precision operations */
static inline void ir_emit_vaddsd(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VADDSD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vsubsd(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VSUBSD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vmulsd(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VMULSD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vdivsd(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VDIVSD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
/* Vector move operations */
static inline void ir_emit_vmov(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_VMOV, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vmovss(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_VMOVSS, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vmovsd(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_VMOVSD, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}
/* Vector load/store operations */
static inline void ir_emit_vload(uint8_t temp_dest, uint8_t temp_addr) {
    ir_emit((Instr){.op = OP_VLOAD, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vstore(uint8_t temp_addr, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_VSTORE, .temp_dest = 0xFF, .temp_src1 = temp_addr, .temp_src2 = temp_src, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vloadss(uint8_t temp_dest, uint8_t temp_addr) {
    ir_emit((Instr){.op = OP_VLOADSS, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vloadsd(uint8_t temp_dest, uint8_t temp_addr) {
    ir_emit((Instr){.op = OP_VLOADSD, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vstoress(uint8_t temp_addr, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_VSTORESS, .temp_dest = 0xFF, .temp_src1 = temp_addr, .temp_src2 = temp_src, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vstoresd(uint8_t temp_addr, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_VSTORESD, .temp_dest = 0xFF, .temp_src1 = temp_addr, .temp_src2 = temp_src, .imm = 0, .label_idx = 0});
}
/* Conversion operations */
static inline void ir_emit_vcvtsi2ss(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_VCVT_SI2SS, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vcvtss2si(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_VCVT_SS2SI, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vcvtsi2sd(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_VCVT_SI2SD, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vcvttsd2si(uint8_t temp_dest, uint8_t temp_src) {
    ir_emit((Instr){.op = OP_VCVT_SD2SI, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2 = 0xFF, .imm = 0, .label_idx = 0});
}
/* Comparison operations */
static inline void ir_emit_vcmpss(uint8_t temp_src1, uint8_t temp_src2, uint8_t cond) {
    ir_emit((Instr){.op = OP_VCMPSS, .temp_dest = 0xFF, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .cond = cond, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vcmpsd(uint8_t temp_src1, uint8_t temp_src2, uint8_t cond) {
    ir_emit((Instr){.op = OP_VCMPSD, .temp_dest = 0xFF, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .cond = cond, .imm = 0, .label_idx = 0});
}
/* Bitwise operations */
static inline void ir_emit_vand(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VAND, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vor(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VOR, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vxor(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VXOR, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vshuf(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2, uint8_t imm) {
    ir_emit((Instr){.op = OP_VSHUF, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = imm, .label_idx = 0});
}
static inline void ir_emit_vandn(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VANDN, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vorps(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VORPS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vsubps(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VSUBPS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vmulps(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VMULPS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vaddpd(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VADDPD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vpaddd(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VPADDD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vpsubd(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VPSUBD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vpmulld(uint8_t temp_dest, uint8_t temp_src1, uint8_t temp_src2) {
    ir_emit((Instr){.op = OP_VPMULLD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
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