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


/* Special register constants */
#define REG_FLAGS 255  /* Special flag register for explicit flag dependency tracking */

/* Call argument tracking constants. Must stay >= ZERO_CC_MAX_PARAMS so a
 * call can pass every parameter of the largest representable function. */
#define IR_CALL_MAX_ARGS 256
#define IR_CALL_MAX_SITES 1024

/* Call argument tracking structure */
typedef struct {
    size_t instr_index;
    int count;
    uint16_t temps[IR_CALL_MAX_ARGS];  /* 16-bit temp ids (C2b) */
    uint8_t is_fp[IR_CALL_MAX_ARGS];  /* 1 if FP argument, 0 if integer */
    /* Position within the combined (argument-order) stack-arg list, or
     * 255 when the arg goes into a register. Needed for FP args beyond
     * the 8th FP register: they must land on the call stack interleaved
     * with overflowed integer args in original argument order. */
    uint8_t stack_pos[IR_CALL_MAX_ARGS];
} IRCallArgs;

/* Call args arena for tracking */
typedef struct {
    IRCallArgs *table;
    size_t count;
    size_t capacity;
} IRCallArgsArena;

/* Special register constants */
#define REG_FLAGS 255  /* Special flag register for explicit flag dependency tracking (uint8 flags fields) */

/* No-temp sentinel for the 16-bit Instr temp fields (C2b: was 0xFF when the
 * fields were uint8_t). 65535 can no longer be a real temp id. */
#define TEMP_NONE 0xFFFF

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
    OP_OVF_TRAP,       /* zero_san v1: signed-integer-overflow trap after signed add/sub/mul */
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
    OP_PARAM_LOAD,     /* load argument register directly into virtual register (frameless leaf) */
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
    OP_TRAP,                /* __builtin_trap / __builtin_unreachable: emit a
                             * target-specific illegal/trap instruction */
    OP_SYSCALL,             /* OS syscall intrinsic (zero_rt OS boundary).
                             * Arguments (number + up to 6 operands) are packed
                             * like OP_CALL: temp_dest = arg_count, full list in
                             * the IRCallArgs side table. The backend moves them
                             * into the target's syscall argument registers and
                             * issues the syscall instruction; the result is in
                             * VREG_0 (rax). Clobbers like a call. */
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

    /* C11 atomic bitwise RMW (3) - appended so existing enum values are stable.
     * dest = pre-update memory snapshot, temp_src1 = address, temp_src2 = value,
     * imm = access width (1/2/4/8). The backend snapshots [addr] into dest,
     * then atomically updates memory in place (x86-64: lock or/and/xor),
     * matching the GCC __atomic_fetch_or/and/xor pattern. */
    OP_ATOMIC_OR,      /* atomic in-place OR; returns pre-update value */
    OP_ATOMIC_AND,     /* atomic in-place AND; returns pre-update value */
    OP_ATOMIC_XOR,     /* atomic in-place XOR; returns pre-update value */

    /* Static TLS (appended so existing enum values are stable) */
    OP_TLS_LEA,        /* dest = address of a _Thread_local static variable.
                        * imm64 holds the variable name (const char*). AOT
                        * x86-64 (initial-exec): movq %fs:0 + leaq $var@tpoff
                        * (dst) with an R_X86_64_TPOFF32 relocation. -S
                        * emits the same idiom. JIT mode never emits this
                        * op - TLS variables are plain globals (OP_MOV_GLOBAL)
                        * in the data section. */

    /* Scalar single<->double conversion (appended so existing enum values
     * are stable). Needed by the per-lane GNU vector lowering: a 4-byte
     * float lane is promoted to the compiler's double-width FP model for
     * the lane op, then demoted before the width-correct store.
     * x86-64: cvtss2sd / cvtsd2ss; AArch64: fcvt d,s / fcvt s,d;
     * RISC-V: fcvt.d.s / fcvt.s.d. */
    OP_CVTSS2SD,       /* dest (sd) = (double) src (ss) */
    OP_CVTSD2SS,       /* dest (ss) = (float) src (sd) */

    /* Computed goto & label address (GNU extensions) */
    OP_JMP_INDIRECT,   /* jmp through register temp_src1 */
    OP_LEA_LABEL,      /* temp_dest = address of label_idx */

    /* Non-local jumps (__builtin_setjmp / __builtin_longjmp) */
    OP_SETJMP,         /* save FP/PC/SP to [temp_src1], VREG_0 = 0 (or val on resume) */
    OP_LONGJMP,        /* restore FP/SP from [temp_src1], VREG_0 = temp_src2, jmp PC */

    /* Sprint 99: nonlocal goto (goto to a label in an ancestor function). */
    OP_NONLOCAL_GOTO,  /* restore (FP, SP) = (temp_src1, temp_src2), jump to the
                        * label (imm64 = target function index, label_idx = the
                        * label's IR index in that function's namespace) */

    /* GNU __int128 (Sprint 109): 128-bit integer ops. A 128-bit value lives in
     * a REG_CLASS_GPR128 temp (low half = regalloc_get_reg_for_temp, high half
     * = regalloc_get_reg_hi_for_temp). For each op the (lo,hi) pair is read
     * from the named temps; 64-bit address/result temps use the single-reg
     * accessors. Division/modulo are lowered to a __udivti3/__divti3 libgcc
     * call (no dedicated op). */
    OP_ADD128,         /* dest = src1 + src2 (128-bit, unsigned carry) */
    OP_SUB128,         /* dest = src1 - src2 (128-bit, unsigned borrow) */
    OP_MUL128,         /* dest = src1 * src2 (128x128 -> low 128 bits) */
    OP_AND128,         /* dest = src1 & src2 */
    OP_OR128,          /* dest = src1 | src2 */
    OP_XOR128,         /* dest = src1 ^ src2 */
    OP_NOT128,         /* dest = ~src1 */
    OP_NEG128,         /* dest = -src1 (two's complement) */
    OP_SHL128,         /* dest = src1 << src2 (src2 = shift count, 0..127) */
    OP_SHR128,         /* dest = src1 >> src2 (logical, unsigned) */
    OP_SAR128,         /* dest = src1 >> src2 (arithmetic, signed) */
    OP_CMP128,         /* compare src1, src2 (128-bit); sets flags for OP_COND_SET */
    OP_MOV128,         /* dest (128) = src1 (128) */
    OP_LOAD128,        /* dest (128) = *(128-bit *)src1 (address) */
    OP_STORE128,       /* *(128-bit *)src2 (address) = src1 (128) */
    OP_ZEXT64_128,     /* dest (128) = (u128)src1 (64-bit, zero-extended; hi=0) */
    OP_SEXT64_128,     /* dest (128) = (s128)src1 (64-bit, sign-extended) */
    OP_TRUNC128_64,    /* dest (64) = low 64 bits of src1 (128) */
    OP_MOV128_IMM,     /* dest (128) = {0, imm64} (hi=0, lo=imm64) */
    OP_LO128,          /* dest (64) = low 64 bits of src1 (128) */
    OP_HI128,          /* dest (64) = high 64 bits of src1 (128) */
    OP_MK128,          /* dest (128) = {hi64 = src2, lo64 = src1} */

    /* Sprint 115m: raw 4-byte (single-precision) FP access for the real
     * SysV/AArch64/RISC-V ABI float boundary: the LOW 32 bits of the FP
     * register hold the float pattern (upper bits undefined), with NO
     * double-width scalar-model conversion - unlike OP_VLOADSS/OP_VSTORESS,
     * which widen/narrow to/from the model double (x86/RV64 hold every
     * scalar float as a 64-bit double in the register). Used at external
     * call boundaries for plain `float` params/returns and `float _Complex`
     * halves (csinf(1.0F+2.0iF) was mis-marshaled as doubles). */
    OP_VLOADSS_RAW,    /* dest (low32) = *(32-bit *)src1, zero-extended (no widen) */
    OP_VSTORESS_RAW,   /* *(32-bit *)src2 (address) = low 32 bits of src1 (no narrow) */
    OP_CVTTSD2SS,      /* dest (ss, low32) = (float) src1 (sd): model double -> ABI float */
} Opcode;

/* Condition codes for OP_COND_SET */
#define CC_LT  0  /* signed less than */
#define CC_LE  1  /* signed less or equal */
#define CC_GT  2  /* signed greater */
#define CC_GE  3  /* signed greater or equal */
/* Float-safe (CF/ZF-only) variants, x86 remap for OP_CMP with FP operands
 * (C12): this host class (WSL2) comisd/ucomisd sets CF/ZF but not SF/OF,
 * so the signed CCs misbehave on float compares. For ordered values CF/ZF
 * encode the total order; these map to setb/setbe/seta/setae (gcc's choice
 * for double compares). Never emitted by the lowering - remap only. */
#define CC_FLT_LT 6  /* setb  (CF)             */
#define CC_FLT_LE 7  /* setbe (CF or ZF)       */
#define CC_FLT_GT 8  /* seta  (CF=0 and ZF=0)  */
#define CC_FLT_GE 9  /* setae (CF=0)           */
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
        case OP_OVF_TRAP: return "OVF_TRAP";
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
        case OP_ATOMIC_OR: return "ATOMIC_OR";
        case OP_ATOMIC_AND: return "ATOMIC_AND";
        case OP_ATOMIC_XOR: return "ATOMIC_XOR";
        case OP_ATOMIC_CMPXCHG: return "ATOMIC_CMPXCHG";
        case OP_SYSCALL: return "SYSCALL";
        case OP_ATOMIC_CMPXCHG_BOOL: return "ATOMIC_CMPXCHG_BOOL";
        case OP_ATOMIC_XCHG: return "ATOMIC_XCHG";
        case OP_MFENCE: return "MFENCE";
        case OP_TRAP: return "TRAP";
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
        case OP_CVTSS2SD: return "CVTSS2SD";
        case OP_CVTSD2SS: return "CVTSD2SS";
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
        case OP_JMP_INDIRECT: return "JMP_INDIRECT";
        case OP_LEA_LABEL: return "LEA_LABEL";
        case OP_SETJMP: return "SETJMP";
        case OP_LONGJMP: return "LONGJMP";
        case OP_NONLOCAL_GOTO: return "NONLOCAL_GOTO";
        case OP_ADD128: return "ADD128";
        case OP_SUB128: return "SUB128";
        case OP_MUL128: return "MUL128";
        case OP_AND128: return "AND128";
        case OP_OR128: return "OR128";
        case OP_XOR128: return "XOR128";
        case OP_NOT128: return "NOT128";
        case OP_NEG128: return "NEG128";
        case OP_SHL128: return "SHL128";
        case OP_SHR128: return "SHR128";
        case OP_SAR128: return "SAR128";
        case OP_CMP128: return "CMP128";
        case OP_MOV128: return "MOV128";
        case OP_LOAD128: return "LOAD128";
        case OP_STORE128: return "STORE128";
        case OP_ZEXT64_128: return "ZEXT64_128";
        case OP_SEXT64_128: return "SEXT64_128";
        case OP_TRUNC128_64: return "TRUNC128_64";
        case OP_MOV128_IMM: return "MOV128_IMM";
        case OP_LO128: return "LO128";
        case OP_HI128: return "HI128";
        case OP_MK128: return "MK128";
        case OP_VLOADSS_RAW: return "VLOADSS_RAW";
        case OP_VSTORESS_RAW: return "VSTORESS_RAW";
        case OP_CVTTSD2SS: return "CVTTSD2SS";
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
        case OP_CVTSS2SD:  /* single -> double (vector float-lane lowering) */
        case OP_CVTSD2SS:  /* double -> single (vector float-lane lowering) */
        case OP_VLOADSS:   /* scalar 4-byte load: result is a float temp (XMM pool) */
        case OP_VLOADSD:   /* scalar 8-byte load: result is a float temp (XMM pool) */
        case OP_VLOADSS_RAW: /* raw 4-byte float load (ABI boundary, XMM pool) */
        case OP_CVTTSD2SS:   /* double -> single (ABI float boundary) */
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

/* Instruction structure - 24 bytes packed (C2b: temp fields widened to uint16_t) */
typedef struct __attribute__((packed)) {
    uint8_t  op;          /* Operation code (0-255) */
    uint16_t temp_dest;   /* Destination temp (0-65534, TEMP_NONE if none) */
    uint16_t temp_src1;   /* Source temp 1 (0-65534, TEMP_NONE if none) */
    uint16_t temp_src2;   /* Source temp 2 (0-65534, TEMP_NONE if none) */
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


/* Get instruction buffer (for live range analysis) */
const Instr *ir_get_buffer(const ZeroCCCompiler *c);

/* Emit instruction - O(1) amortized */
void ir_emit(ZeroCCCompiler *c, Instr instr);

/* Label and jump instructions */
void ir_emit_label(ZeroCCCompiler *c, uint16_t label_idx);
void ir_emit_jmp(ZeroCCCompiler *c, uint16_t label_idx);
void ir_emit_je(ZeroCCCompiler *c, uint16_t temp_cond, uint16_t label_idx);
void ir_emit_jcond(ZeroCCCompiler *c, uint16_t temp_cond, uint16_t label_idx);

/* Reset for next function - O(1), reuses memory */
void ir_reset(ZeroCCCompiler *c);

/* Clear IR buffer after machine code emission */
void ir_clear(ZeroCCCompiler *c);

/* Debug: Dump IR to file */
void ir_dump(const ZeroCCCompiler *c, FILE *f);

/* Get instruction count */
size_t ir_get_count(const ZeroCCCompiler *c);

/* Access instruction by index */
Instr* ir_get(ZeroCCCompiler *c, size_t idx);

/* Mark the most recently emitted instruction with a C99 result width so the
 * MC layer extends the 64-bit result to true N-bit wrap semantics:
 * w in {1,2,4} = sign-extend, w in {9,10,12} = zero-extend. */
static inline void ir_mark_last_width(ZeroCCCompiler *c, uint8_t w) {
    size_t n = ir_get_count(c);
    if (n > 0) ir_get(c, n - 1)->width = w;
}

static inline void ir_mark_last_unsigned(ZeroCCCompiler *c, uint8_t u) {
    size_t n = ir_get_count(c);
    if (n > 0) ir_get(c, n - 1)->cond = u;
}

/* Sprint 118 (type-system hardening #1, slice 1): per-temp value-type tags.
 *
 * The IR is untyped: type currently flows through side channels (the C99
 * width marks on defining instructions, `last_expr_is_float`,
 * `last_expr_is_128`). Each temp id carries a tag recording the semantic
 * type of the value it holds, stamped at lowering from the AST node's
 * TypeInfo (the frontend's verdict - independent of the lowering's own UAC
 * analysis) so the side-channel conventions become CHECKABLE invariants:
 *   - tools/type_tag_verifier.py (persistent gate) cross-checks width
 *     marks, OP_OVF_TRAP metadata, FP ops and 128-bit ops against the tags;
 *   - lower_expr() asserts flag/tag agreement per expression.
 * Slice 1 is emission + verification only: no codegen or optimizer reads
 * the tags (slice 2 converts the side channels to read them).
 *
 * Keyed by temp id (not instruction index) so optimizer passes that insert,
 * remove, or move instructions never need to remap the table - each temp
 * id is allocated exactly once per function and its value type is fixed at
 * allocation. */
enum {
    ZC_TAGCLS_NONE   = 0, /* no claim: untyped temp (address, flag, fixed vreg,
                             or the node carried no TypeInfo) */
    ZC_TAGCLS_INT    = 1, /* signed integer, width in the width bits */
    ZC_TAGCLS_UINT   = 2, /* unsigned integer, width in the width bits */
    ZC_TAGCLS_FLOAT  = 3, /* double-width FP model (float, double, __float128) */
    ZC_TAGCLS_INT128 = 4, /* GNU __int128 (lo,hi) GPR128 pair */
    ZC_TAGCLS_UINT128= 5, /* unsigned __int128 */
    ZC_TAGCLS_PTR    = 6, /* pointer / address-form value (incl. decays,
                             complex/vector/struct rvalues, function addrs) */
};
enum { /* width bits (tag >> 4) & 0x7 */
    ZC_TAGW_1  = 0,
    ZC_TAGW_2  = 1,
    ZC_TAGW_4  = 2,
    ZC_TAGW_8  = 3,
    ZC_TAGW_16 = 4,
};

#define ZC_TAG_NONE   0u
#define ZC_TAG_INT(w)  ((uint8_t)(ZC_TAGCLS_INT  | ((w) << 4)))
#define ZC_TAG_UINT(w) ((uint8_t)(ZC_TAGCLS_UINT | ((w) << 4)))
#define ZC_TAG_FLOAT(w) ((uint8_t)(ZC_TAGCLS_FLOAT | ((w) << 4)))
#define ZC_TAG_INT128  ((uint8_t)(ZC_TAGCLS_INT128  | (ZC_TAGW_16 << 4)))
#define ZC_TAG_UINT128 ((uint8_t)(ZC_TAGCLS_UINT128| (ZC_TAGW_16 << 4)))
#define ZC_TAG_PTR     (uint8_t)(ZC_TAGCLS_PTR)

static inline uint8_t zc_tag_class(uint8_t tag) { return (uint8_t)(tag & 0xFu); }
static inline uint8_t zc_tag_width_code(uint8_t tag) { return (uint8_t)((tag >> 4) & 0x7u); }
/* Bytes for the width code (0 = not a byte-width class). */
static inline int zc_tag_width_bytes(uint8_t tag) {
    static const int zc_tag_wb[8] = {1, 2, 4, 8, 16, 0, 0, 0};
    return zc_tag_wb[(tag >> 4) & 0x7u];
}

/* Read the tag for temp id (0 for untagged). ids >= 65535 have no tag. */
uint8_t zc_temp_tag(const ZeroCCCompiler *c, uint16_t id);
/* Stamp temp id with a tag (no-op for id >= 65535; keeps the high-water
 * mark for the -dump-ir table). */
void zc_temp_tag_set(ZeroCCCompiler *c, uint16_t id, uint8_t tag);
/* Reset the table for the next function (called from ir_reset). */
void zc_temp_tags_reset(ZeroCCCompiler *c);

/* ------------------------------------------------------------------
 * Sprint 119 (type-system hardening #1, slice 2): codegen-side tag
 * readers. The temp tag is the frontend's verdict on the value's TYPE
 * and is the single source of truth for type-derived codegen
 * decisions; the legacy instruction side channels (C99 width mark,
 * [u] div mark, OVF check-width field) are consulted only where the
 * tag cannot answer:
 *
 *  - The C99 width mark is an op PARAMETER ("canonicalize this op's
 *    result to N bits"), not a type claim: it also carries bitfield
 *    bit widths (3..7) the byte-width tag format cannot express, and
 *    it is fresher than a lifetime tag on a temp id reused past the
 *    32768 threshold (lower_stmt_if). A PRESENT mark therefore always
 *    wins for the extend decision. The tag drives the extend in exactly
 *    one gap the mark leaves: the zero_san slot-steal, where the ALU
 *    op's mark lands on the following OP_OVF_TRAP and the (unmarked)
 *    ALU result is canonicalized from the tag, deferred PAST the check
 *    (clean-value invariant: the check sees the raw 64-bit result and
 *    the ALU-set flags; on values that survive the check the deferred
 *    extend is a provable no-op). Other unmarked ops stay unextended:
 *    canonicalization there is pre-canonicalized (an in-place
 *    SEXTn/ZEXTn right after) or restored by the narrow load/store
 *    discipline.
 *  - DIV/MOD signedness is the UAC result type; the mark IS the UAC
 *    verdict emitted at the producer site, so a present mark wins
 *    (legacy behavior). The tag fills the silent-mark gap - and only
 *    at width >= 4 (C promotion: a sub-int division is a signed int
 *    division, and the LHS-type tag compound-assign stamps on its op
 *    temp is the assignment type, not the division type).
 *  - The OVF check width is the check's own metadata; the tag is
 *    consulted only when it agrees with the field (I2), which is what
 *    makes the tag a safe source there.
 *
 * Verifier invariants I1/I2/I8 (tools/type_tag_verifier.py) gate that
 * the tag and the marks agree wherever both speak; I9 (slice 2)
 * reports the unmarked narrow-op population (OVF-deferred /
 * pre-canonicalized / load-store-discipline) as a coverage stat.
 * ------------------------------------------------------------------ */

/* Ops whose result can leave the C99 N-bit range and must be
 * canonicalized (sign/zero-extended) to the dest's width. Copy,
 * address, compare, COND_SET (0/1), already-canonizing ops (LOAD_16/
 * 32/BYTE, LNOT, SEXTn/ZEXTn) and FP/128 ops are excluded. */
static inline int zc_op_can_define_narrow_int(uint8_t op) {
    switch (op) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
        case OP_AND: case OP_OR: case OP_XOR: case OP_NOT: case OP_NEG:
        case OP_SHL: case OP_SHR: case OP_SAR:
        case OP_ADD_IMM: case OP_SUB_IMM: case OP_INC: case OP_DEC:
            return 1;
        default:
            return 0;
    }
}

/* C99 wrap-extend decision for an instruction's result (slice 2).
 * Returns 1 and sets *w_out (bytes or bit width) / *sign_out (1 =
 * sign-extend) when the result must be canonicalized; 0 = no extend.
 *
 * Mark-first (see the note above); the tag fills exactly ONE gap the
 * mark leaves: the zero_san slot-steal, where an unmarked narrow-class
 * ALU op is immediately followed by an OP_OVF_TRAP on the same temp
 * (the ALU's own mark landed on the trap instead). There the tag drives
 * the extend, deferred past the check by the trap's frame. Other
 * unmarked ops stay unextended - the lowering contract is that
 * canonicalization is either marked, pre-canonicalized (an in-place
 * SEXTn/ZEXTn right after), or restored by the narrow load/store
 * discipline; widening the gap-fill beyond the trap case would emit
 * redundant extends across the inlined-parameter population (see I9).
 */

/* The instruction's C99 width mark (a per-op parameter: byte widths
 * 1/2/4 signed or 8+1/2/4 unsigned, plus bitfield bit widths 3..7).
 * `w & 7` strips the zero-extend flag; w==0 (mark 8/16) is a width-8
 * no-op the emitter absorbs. */
static inline int zc_mark_extend(const Instr *instr, int *w_out, int *sign_out) {
    if (!instr || instr->temp_dest == TEMP_NONE) return 0;
    if (instr->width == 0) return 0;
    *w_out = (int)(instr->width & 7);
    *sign_out = (instr->width < 8) ? 1 : 0;
    return 1;
}

/* The tag-driven extend for an UNmarked narrow-class ALU op on a
 * tagged int/uint temp of width 1/2/4 (w==8 needs no extend). Used
 * only for the OVF-adjacent deferral (see zc_mark_extend's note). */
static inline int zc_tag_extend(const ZeroCCCompiler *c, const Instr *instr,
                                int *w_out, int *sign_out) {
    if (!instr || instr->temp_dest == TEMP_NONE || instr->width != 0) return 0;
    if (!zc_op_can_define_narrow_int(instr->op)) return 0;
    uint8_t tag = zc_temp_tag(c, instr->temp_dest);
    uint8_t cls = zc_tag_class(tag);
    if (cls != ZC_TAGCLS_INT && cls != ZC_TAGCLS_UINT) return 0;
    int w = zc_tag_width_bytes(tag);
    if (w != 1 && w != 2 && w != 4) return 0;
    *w_out = w;
    *sign_out = (cls == ZC_TAGCLS_INT) ? 1 : 0;
    return 1;
}

/* Is this DIV/MOD unsigned? The UAC result type decides, and the mark
 * is the UAC verdict emitted at the producer site - so a PRESENT mark
 * (the [u] cond mark or the zero-extend width flag) always wins, exactly
 * as in the legacy code. When the mark is silent (a compound-assign /
 * desugared div whose operands the width inference couldn't classify),
 * the dest tag decides - but only at width >= 4: C integer promotion
 * means a sub-int division is a signed int division, and the LHS-type
 * tag that compound-assign stamps on its op temp (e.g. UINT(1) for
 * `unsigned char x; x /= y;`) is the assignment type, not the division
 * type (c-torture execute/20030128-1.c). */
static inline int zc_div_is_unsigned(const ZeroCCCompiler *c, const Instr *instr) {
    if (instr->cond != 0 || instr->width != 0) {
        return (instr->cond != 0 || instr->width > 8) ? 1 : 0;
    }
    uint8_t tag = zc_temp_tag(c, instr->temp_dest);
    if (zc_tag_width_bytes(tag) < 4) return 0;
    uint8_t cls = zc_tag_class(tag);
    if (cls == ZC_TAGCLS_UINT) return 1;
    if (cls == ZC_TAGCLS_INT) return 0;
    return 0;
}

/* OP_OVF_TRAP check width (4 or 8): tag-first from the checked temp
 * (invariant I2: it must be a signed int of exactly the check width),
 * falling back to the instruction's check-width field. The tag is
 * trusted only when it AGREES with the field: the checked temp can
 * carry an assignment-expression stamp of a different width (a
 * `long l = a + b;` where the int add's check is width 4 but the
 * lvalue's tag says 8), and the field is the check width by
 * construction - agreement (I2) is what makes the tag a safe source. */
static inline int zc_ovf_trap_width(const ZeroCCCompiler *c, const Instr *instr) {
    uint8_t tag = zc_temp_tag(c, instr->temp_dest);
    if (zc_tag_class(tag) == ZC_TAGCLS_INT) {
        int w = zc_tag_width_bytes(tag);
        if ((w == 4 || w == 8) && w == (int)instr->width) return w;
    }
    return (int)instr->width;
}

/* Does `nx` (the instruction right after a narrow result op)
 * canonicalize the same temp in-place via SEXTn/ZEXTn - the
 * compound-assign / narrow-local-store pattern (unmarked ALU op +
 * SEXT32 + 64-bit slot store)? Matches on temp, direction (SEXT <->
 * INT tag, ZEXT <-> UINT tag) and width (n == tagged width): a wider,
 * narrower or opposite-direction extend is not a canonicalization of
 * this value. In-place extends set temp_dest == temp_src1 == temp. */
static inline int zc_next_canonicalizes(const ZeroCCCompiler *c, const Instr *nx,
                                        uint16_t temp) {
    if (!nx || temp == TEMP_NONE) return 0;
    if (nx->temp_dest != temp || nx->temp_src1 != temp) return 0;
    int w = 0, sign = 0;
    switch (nx->op) {
        case OP_SEXT8:  w = 1; sign = 1; break;
        case OP_SEXT16: w = 2; sign = 1; break;
        case OP_SEXT32: w = 4; sign = 1; break;
        case OP_ZEXT8:  w = 1; sign = 0; break;
        case OP_ZEXT16: w = 2; sign = 0; break;
        case OP_ZEXT32: w = 4; sign = 0; break;
        default: return 0;
    }
    uint8_t tag = zc_temp_tag(c, temp);
    return zc_tag_width_bytes(tag) == w &&
           ((sign && zc_tag_class(tag) == ZC_TAGCLS_INT) ||
            (!sign && zc_tag_class(tag) == ZC_TAGCLS_UINT));
}

/* Insert instruction at specific index (shifts existing instructions) */
void ir_insert_at(ZeroCCCompiler *c, size_t idx, Instr instr);

/* Helper: Insert MOV_IMM at index */
static inline void ir_insert_mov_imm(ZeroCCCompiler *c, size_t idx, uint16_t temp_dest, int64_t imm) {
    ir_insert_at(c, idx, (Instr){.op = OP_MOV_IMM, .temp_dest = temp_dest, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = (int16_t)imm, .imm64 = imm, .label_idx = 0});
}

/* Helper: Insert MOV_REG at index */
static inline void ir_insert_mov_reg(ZeroCCCompiler *c, size_t idx, uint16_t temp_dest, uint16_t temp_src) {
    ir_insert_at(c, idx, (Instr){.op = OP_MOV_REG, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

/* Helper: Insert ADD at index */
static inline void ir_insert_add(ZeroCCCompiler *c, size_t idx, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_insert_at(c, idx, (Instr){.op = OP_ADD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Helper: Insert LABEL at index */
static inline void ir_insert_label(ZeroCCCompiler *c, size_t idx, uint16_t label_idx) {
    ir_insert_at(c, idx, (Instr){.op = OP_LABEL, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx});
}

/* Call argument tracking - arena-based */
void ir_call_args_record(ZeroCCCompiler *c, size_t instr_index, int count, const int *temps, const int *is_fp, const uint8_t *stack_pos);
const IRCallArgs *ir_call_args_find(const ZeroCCCompiler *c, size_t instr_index);

/* Helper functions for common patterns */
static inline void ir_emit_mov_imm(ZeroCCCompiler *c, uint16_t temp_dest, int64_t imm) {
    ir_emit(c, (Instr){.op = OP_MOV_IMM, .temp_dest = temp_dest, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = (int16_t)imm, .imm64 = imm, .label_idx = 0});
}

static inline void ir_emit_add(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_sub(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_mul(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_MUL, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Label and jump instructions */
/* Defined in ir.c */
extern void ir_emit_label(ZeroCCCompiler *c, uint16_t label_idx);
extern void ir_emit_jmp(ZeroCCCompiler *c, uint16_t label_idx);
extern void ir_emit_je(ZeroCCCompiler *c, uint16_t temp_cond, uint16_t label_idx);
extern void ir_emit_jcond(ZeroCCCompiler *c, uint16_t temp_cond, uint16_t label_idx);
extern void ir_emit_jne(ZeroCCCompiler *c, uint16_t temp_cond, uint16_t label_idx);
extern void ir_emit_jmp_label(ZeroCCCompiler *c, uint16_t label_idx);

static inline void ir_emit_ret(ZeroCCCompiler *c) {
    ir_emit(c, (Instr){.op = OP_RET, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

/* Emit function prologue (frame_size in imm64) */
static inline void ir_emit_prologue(ZeroCCCompiler *c, int64_t frame_size) {
    ir_emit(c, (Instr){.op = OP_PROLOGUE, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = 0, .imm64 = frame_size, .label_idx = 0});
}

/* Emit function epilogue */
static inline void ir_emit_epilogue(ZeroCCCompiler *c) {
    ir_emit(c, (Instr){.op = OP_EPILOGUE, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_param_save_int(ZeroCCCompiler *c, int reg_idx, int32_t offset) {
    ir_emit(c, (Instr){.op = OP_PARAM_SAVE_INT, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = (int16_t)reg_idx, .imm64 = (int64_t)offset, .label_idx = 0});
}

static inline void ir_emit_param_save_fp(ZeroCCCompiler *c, int reg_idx, int32_t offset, uint8_t is_single) {
    ir_emit(c, (Instr){.op = OP_PARAM_SAVE_FP, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .cond = is_single, .imm = (int16_t)reg_idx, .imm64 = (int64_t)offset, .label_idx = 0});
}

static inline void ir_emit_param_load(ZeroCCCompiler *c, int temp_dest, int arg_idx, bool is_fp) {
    ir_emit(c, (Instr){.op = OP_PARAM_LOAD, .temp_dest= (uint16_t)temp_dest, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .cond = (uint8_t)(is_fp ? 1 : 0), .imm = (int16_t)arg_idx, .imm64 = 0, .label_idx = 0});
}

static inline void ir_emit_variadic_spill(ZeroCCCompiler *c, int param_count, int32_t base_offset) {
    ir_emit(c, (Instr){.op = OP_VARIADIC_SPILL, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = (int16_t)param_count, .imm64 = (int64_t)base_offset, .label_idx = 0});
}

/* Emit function call - fn_name stored in imm64 as pointer during lowering */
static inline void ir_emit_call(ZeroCCCompiler *c, const char *fn_name) {
    ir_emit(c, (Instr){.op = OP_CALL, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = 0, .imm64 = (int64_t)(intptr_t)fn_name, .label_idx = 0});
}

/* Emit the OS syscall intrinsic. Argument temps (syscall number first, then
 * up to 6 operands) are packed exactly like OP_CALL: count in temp_dest,
 * first two in temp_src1/src2, the full list in the IRCallArgs side table.
 * The result is in VREG_0 (the ABI return register). */
static inline void ir_emit_syscall(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    ir_emit(c, (Instr){.op = OP_SYSCALL,
                    .temp_dest = (uint16_t)arg_count,
                    .temp_src1 = (arg_count > 0) ? (uint16_t)arg_temps[0] : TEMP_NONE,
                    .temp_src2 = (arg_count > 1) ? (uint16_t)arg_temps[1] : TEMP_NONE,
                    .imm = 0, .imm64 = 0, .label_idx = 0});
    ir_call_args_record(c, ir_get_count(c) - 1, arg_count, arg_temps, NULL, NULL);
}

/* Float to int conversion (cvttsd2si) */
static inline void ir_emit_cvttsd2si(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

/* Int to float conversion (cvtsi2sd) */
static inline void ir_emit_cvtsi2sd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_CVTSI2SD, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

/* Scalar single -> double (cvtss2sd; GNU vector float-lane lowering) */
static inline void ir_emit_cvtss2sd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_CVTSS2SD, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

/* Scalar double -> single (cvtsd2ss; GNU vector float-lane lowering) */
static inline void ir_emit_cvtsd2ss(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_CVTSD2SS, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_load_16(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_LOAD_16, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_load_32(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_LOAD_32, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_store_byte(ZeroCCCompiler *c, uint16_t temp_val, uint16_t temp_addr) {
    ir_emit(c, (Instr){.op = OP_STORE_BYTE, .temp_dest= TEMP_NONE, .temp_src1 = temp_val, .temp_src2 = temp_addr, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_store_16(ZeroCCCompiler *c, uint16_t temp_val, uint16_t temp_addr) {
    ir_emit(c, (Instr){.op = OP_STORE_16, .temp_dest= TEMP_NONE, .temp_src1 = temp_val, .temp_src2 = temp_addr, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_store_32(ZeroCCCompiler *c, uint16_t temp_val, uint16_t temp_addr) {
    ir_emit(c, (Instr){.op = OP_STORE_32, .temp_dest= TEMP_NONE, .temp_src1 = temp_val, .temp_src2 = temp_addr, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_store_64(ZeroCCCompiler *c, uint16_t temp_val, uint16_t temp_addr) {
    ir_emit(c, (Instr){.op = OP_STORE_64, .temp_dest= TEMP_NONE, .temp_src1 = temp_val, .temp_src2 = temp_addr, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_vla_alloc(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_size) {
    ir_emit(c, (Instr){.op = OP_VLA_ALLOC, .temp_dest = temp_dest, .temp_src1 = temp_size, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

/* Emit callee-saved register stores (after register allocation) */
static inline void ir_emit_callee_save_stores(ZeroCCCompiler *c, uint32_t callee_save_mask) {
    ir_emit(c, (Instr){.op = OP_CALLEE_SAVE_STORES, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = (int16_t)(callee_save_mask & 0xFFFF), .imm64 = callee_save_mask, .label_idx = 0});
}

/* Emit callee-saved register loads (after register allocation) */
static inline void ir_emit_callee_save_loads(ZeroCCCompiler *c, uint32_t callee_save_mask) {
    ir_emit(c, (Instr){.op = OP_CALLEE_SAVE_LOADS, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = (int16_t)(callee_save_mask & 0xFFFF), .imm64 = callee_save_mask, .label_idx = 0});
}

static inline void ir_emit_atomic_xadd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr, uint16_t temp_val, int16_t width) {
    ir_emit(c, (Instr){.op = OP_ATOMIC_XADD, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_val, .imm = width, .label_idx = 0});
}

static inline void ir_emit_atomic_cmpxchg(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr, uint16_t temp_expected, uint16_t temp_desired, int16_t width) {
    ir_emit(c, (Instr){.op = OP_ATOMIC_CMPXCHG, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_desired, .imm = width, .label_idx = (uint16_t)temp_expected});
}

static inline void ir_emit_atomic_cmpxchg_bool(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr, uint16_t temp_expected, uint16_t temp_desired, int16_t width) {
    ir_emit(c, (Instr){.op = OP_ATOMIC_CMPXCHG_BOOL, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_desired, .imm = width, .label_idx = (uint16_t)temp_expected});
}

static inline void ir_emit_atomic_or(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr, uint16_t temp_val, int16_t width) {
    ir_emit(c, (Instr){.op = OP_ATOMIC_OR, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_val, .imm = width, .label_idx = 0});
}
static inline void ir_emit_atomic_and(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr, uint16_t temp_val, int16_t width) {
    ir_emit(c, (Instr){.op = OP_ATOMIC_AND, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_val, .imm = width, .label_idx = 0});
}
static inline void ir_emit_atomic_xor(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr, uint16_t temp_val, int16_t width) {
    ir_emit(c, (Instr){.op = OP_ATOMIC_XOR, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_val, .imm = width, .label_idx = 0});
}
static inline void ir_emit_atomic_xchg(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr, uint16_t temp_val, int16_t width) {
    ir_emit(c, (Instr){.op = OP_ATOMIC_XCHG, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2 = temp_val, .imm = width, .label_idx = 0});
}

static inline void ir_emit_mfence(ZeroCCCompiler *c) {
    ir_emit(c, (Instr){.op = OP_MFENCE, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_trap(ZeroCCCompiler *c) {
    ir_emit(c, (Instr){.op = OP_TRAP, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

/* Float literal load via RIP-relative (imm64 holds rodata offset) */
static inline void ir_emit_fmov_imm(ZeroCCCompiler *c, uint16_t temp_dest, int64_t rodata_offset) {
    ir_emit(c, (Instr){.op = OP_FMOV_IMM, .temp_dest = temp_dest, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = 0, .imm64 = rodata_offset, .label_idx = 0});
}

/* Float load from stack (offset may exceed int16 range for large frames) */
static inline void ir_emit_fload(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_base, int offset) {
    ir_emit(c, (Instr){.op = OP_FLOAD, .temp_dest = temp_dest, .temp_src1 = temp_base, .temp_src2= TEMP_NONE, .imm = (int16_t)offset, .imm64 = offset, .label_idx = 0});
}

/* Float store to stack (offset may exceed int16 range for large frames) */
static inline void ir_emit_fstore(ZeroCCCompiler *c, uint16_t temp_src, uint16_t temp_base, int offset) {
    ir_emit(c, (Instr){.op = OP_FSTORE, .temp_dest= TEMP_NONE, .temp_src1 = temp_src, .temp_src2 = temp_base, .imm = (int16_t)offset, .imm64 = offset, .label_idx = 0});
}

/* Float add: dest = src1 + src2 */
static inline void ir_emit_fadd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_FADD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float sub: dest = src1 - src2 */
static inline void ir_emit_fsub(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_FSUB, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float mul: dest = src1 * src2 */
static inline void ir_emit_fmul(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_FMUL, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float div: dest = src1 / src2 */
static inline void ir_emit_fdiv(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_FDIV, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* Float move: dest = src */
static inline void ir_emit_fmov(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_FMOV, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

/* SIMD emission functions */
/* Scalar single-precision operations */
static inline void ir_emit_vaddss(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VADDSS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vsubss(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VSUBSS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vmulss(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VMULSS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vdivss(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VDIVSS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
/* Scalar double-precision operations */
static inline void ir_emit_vaddsd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VADDSD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vsubsd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VSUBSD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vmulsd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VMULSD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vdivsd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VDIVSD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
/* Vector move operations */
static inline void ir_emit_vmov(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VMOV, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vmovss(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VMOVSS, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vmovsd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VMOVSD, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
/* Vector load/store operations */
static inline void ir_emit_vload(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr) {
    ir_emit(c, (Instr){.op = OP_VLOAD, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vstore(ZeroCCCompiler *c, uint16_t temp_addr, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VSTORE, .temp_dest= TEMP_NONE, .temp_src1 = temp_addr, .temp_src2 = temp_src, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vloadss(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr) {
    ir_emit(c, (Instr){.op = OP_VLOADSS, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vloadsd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr) {
    ir_emit(c, (Instr){.op = OP_VLOADSD, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vstoress(ZeroCCCompiler *c, uint16_t temp_addr, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VSTORESS, .temp_dest= TEMP_NONE, .temp_src1 = temp_addr, .temp_src2 = temp_src, .imm = 0, .label_idx = 0});
}

/* Sprint 115m: raw 4-byte ABI-float access (no double-width scalar-model
 * conversion; the FP register carries the float pattern in its low 32 bits). */
static inline void ir_emit_vloadss_raw(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_addr) {
    ir_emit(c, (Instr){.op = OP_VLOADSS_RAW, .temp_dest = temp_dest, .temp_src1 = temp_addr, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_vstoress_raw(ZeroCCCompiler *c, uint16_t temp_addr, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VSTORESS_RAW, .temp_dest= TEMP_NONE, .temp_src1 = temp_addr, .temp_src2 = temp_src, .imm = 0, .label_idx = 0});
}

static inline void ir_emit_cvttsd2ss(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_CVTTSD2SS, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vstoresd(ZeroCCCompiler *c, uint16_t temp_addr, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VSTORESD, .temp_dest= TEMP_NONE, .temp_src1 = temp_addr, .temp_src2 = temp_src, .imm = 0, .label_idx = 0});
}
/* Conversion operations */
static inline void ir_emit_vcvtsi2ss(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VCVT_SI2SS, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vcvtss2si(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VCVT_SS2SI, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vcvtsi2sd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VCVT_SI2SD, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vcvttsd2si(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src) {
    ir_emit(c, (Instr){.op = OP_VCVT_SD2SI, .temp_dest = temp_dest, .temp_src1 = temp_src, .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0});
}
/* Comparison operations */
static inline void ir_emit_vcmpss(ZeroCCCompiler *c, uint16_t temp_src1, uint16_t temp_src2, uint8_t cond) {
    ir_emit(c, (Instr){.op = OP_VCMPSS, .temp_dest= TEMP_NONE, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .cond = cond, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vcmpsd(ZeroCCCompiler *c, uint16_t temp_src1, uint16_t temp_src2, uint8_t cond) {
    ir_emit(c, (Instr){.op = OP_VCMPSD, .temp_dest= TEMP_NONE, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .cond = cond, .imm = 0, .label_idx = 0});
}
/* Bitwise operations */
static inline void ir_emit_vand(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VAND, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vor(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VOR, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vxor(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VXOR, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vshuf(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2, uint8_t imm) {
    ir_emit(c, (Instr){.op = OP_VSHUF, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = imm, .label_idx = 0});
}
static inline void ir_emit_vandn(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VANDN, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vorps(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VORPS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vsubps(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VSUBPS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vmulps(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VMULPS, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vaddpd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VADDPD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vpaddd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VPADDD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vpsubd(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VPSUBD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}
static inline void ir_emit_vpmulld(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t temp_src1, uint16_t temp_src2) {
    ir_emit(c, (Instr){.op = OP_VPMULLD, .temp_dest = temp_dest, .temp_src1 = temp_src1, .temp_src2 = temp_src2, .imm = 0, .label_idx = 0});
}

/* LEA RIP-relative (imm64 holds rodata offset) */
static inline void ir_emit_lea_rip(ZeroCCCompiler *c, uint16_t temp_dest, int64_t rodata_offset) {
    ir_emit(c, (Instr){.op = OP_LEA_RIP, .temp_dest = temp_dest, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = 0, .imm64 = rodata_offset, .label_idx = 0});
}

/* Global variable load with relocation (imm64 holds symbol name pointer) */
static inline void ir_emit_mov_global(ZeroCCCompiler *c, uint16_t temp_dest, const char *symbol_name) {
    ir_emit(c, (Instr){.op = OP_MOV_GLOBAL, .temp_dest = temp_dest, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = 0, .imm64 = (int64_t)(intptr_t)symbol_name, .label_idx = 0});
}

/* TLS variable address (imm64 holds symbol name pointer). AOT x86-64 only:
 * the relocator records an is_tls reloc resolved to R_X86_64_TPOFF64. */
static inline void ir_emit_tls_lea(ZeroCCCompiler *c, uint16_t temp_dest, const char *symbol_name) {
    ir_emit(c, (Instr){.op = OP_TLS_LEA, .temp_dest = temp_dest, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .imm = 0, .imm64 = (int64_t)(intptr_t)symbol_name, .label_idx = 0});
}

/* Computed goto (jump through register temp_src1) */
static inline void ir_emit_jmp_ind(ZeroCCCompiler *c, int temp_target) {
    ir_emit(c, (Instr){.op = OP_JMP_INDIRECT, .temp_dest = TEMP_NONE, .temp_src1 = (uint16_t)temp_target, .temp_src2 = TEMP_NONE, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}

/* Nonlocal goto: restore FP/SP from temp_src1/temp_src2 (the hidden pair
 * parameters for the label's scope) and jump to label (target_fn_idx,
 * ir_label) in the ancestor function. */
static inline void ir_emit_nonlocal_goto(ZeroCCCompiler *c, int rbp_temp, int rsp_temp,
                                         int target_fn_idx, uint16_t target_label) {
    ir_emit(c, (Instr){.op = OP_NONLOCAL_GOTO, .temp_dest = TEMP_NONE,
        .temp_src1 = (uint16_t)rbp_temp, .temp_src2 = (uint16_t)rsp_temp,
        .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0,
        .imm64 = (int64_t)target_fn_idx, .label_idx = target_label});
}

/* Address of label (temp_dest = address of label_idx). owner_fn_idx = 0
 * means the label lives in the CURRENT function; a non-zero owner is the
 * function index of an ANCESTOR owning the label (cross-scope `&&label`
 * in a nested function - the MC layer resolves it through the persistent
 * user_label_offsets table, exactly like OP_NONLOCAL_GOTO). */
static inline void ir_emit_lea_label_owner(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t label_idx, int owner_fn_idx) {
    ir_emit(c, (Instr){.op = OP_LEA_LABEL, .temp_dest = temp_dest, .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = (int64_t)owner_fn_idx, .label_idx = label_idx});
}
static inline void ir_emit_lea_label(ZeroCCCompiler *c, uint16_t temp_dest, uint16_t label_idx) {
    ir_emit_lea_label_owner(c, temp_dest, label_idx, 0);
}

/* Non-local jump setup: save [FP, PC(resume), SP] into buf (temp_src1);
 * sets VREG_0 = 0 on first pass, or resumes with VREG_0 = val from OP_LONGJMP. */
static inline void ir_emit_setjmp(ZeroCCCompiler *c, uint16_t temp_buf, uint16_t label_idx) {
    ir_emit(c, (Instr){.op = OP_SETJMP, .temp_dest = TEMP_NONE, .temp_src1 = temp_buf, .temp_src2 = TEMP_NONE, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx});
}

/* Non-local jump: restore FP/SP from buf (temp_src1), VREG_0 = temp_val (temp_src2),
 * and jump to saved PC. Never returns. */
static inline void ir_emit_longjmp(ZeroCCCompiler *c, uint16_t temp_buf, uint16_t temp_val) {
    ir_emit(c, (Instr){.op = OP_LONGJMP, .temp_dest = TEMP_NONE, .temp_src1 = temp_buf, .temp_src2 = temp_val, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
}

#endif /* ZERO_CC_IR_H */

/* Debug: Print IR count */
