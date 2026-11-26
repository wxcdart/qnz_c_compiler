/* =========================================================================
 * ARMv7-A 32-bit backend (AArch32, Sprint 185 Phase 1).
 *
 * AAPCS32 calling convention: r0-r3 args, r0 return, 8-byte stack alignment,
 * no red zone. va_list is plain char* (no x86-64 glibc shadow).
 * ELF32 object writer (EM_ARM=40), own-linker EM_ARM + ELFCLASS32,
 * relocations: R_ARM_{ABS32,PC32,REL32}.
 *
 * Registers (ARMv7-A, 16 GPRs):
 *   r0-r3    argument/result registers (caller-saved)
 *   r4-r11   callee-saved registers
 *   r12      intra-procedure call scratch (IP, caller-saved)
 *   r13 (sp) stack pointer
 *   r14 (lr) link register
 *   r15 (pc) program counter
 *
 * VFPv3/NEON: d0-d31 (64-bit FP registers), q0-q15 (128-bit vector registers)
 *   d0-d7 / q0-q3  FP args/results (AAPCS VFP variant)
 *   d8-d15         callee-saved (VFP)
 *
 * This file mirrors the AArch64 backend structure but with 32-bit ARM encodings.
 * ========================================================================= */
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/isa/tramp_stub.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/ir/asm_emitter.h"
#include "zero_cc/types/vregs.h"

static void aarch32_emit_push_reg(ZeroCCCompiler *c, int reg);
static void aarch32_emit_pop_reg(ZeroCCCompiler *c, int reg);

/* =========================================================================
 * ARMv7-A instruction encoding helpers (32-bit ARM, not Thumb-2)
 * ========================================================================= */
static inline void arm32_emit_u32(ZeroCCCompiler *c, uint32_t insn) {
    me_emit_u8(c, insn & 0xFF);
    me_emit_u8(c, (insn >> 8) & 0xFF);
    me_emit_u8(c, (insn >> 16) & 0xFF);
    me_emit_u8(c, (insn >> 24) & 0xFF);
}

/* Condition codes (ARMv7-A): 0=EQ, 1=NE, 2=CS/HS, 3=CC/LO, 4=MI, 5=PL, 6=VS, 7=VC,
 * 8=HI, 9=LS, 10=GE, 11=LT, 12=GT, 13=LE, 14=AL, 15=NV */
#define ARM_CC_EQ  0
#define ARM_CC_NE  1
#define ARM_CC_CS  2
#define ARM_CC_CC  3
#define ARM_CC_MI  4
#define ARM_CC_PL  5
#define ARM_CC_VS  6
#define ARM_CC_VC  7
#define ARM_CC_HI  8
#define ARM_CC_LS  9
#define ARM_CC_GE  10
#define ARM_CC_LT  11
#define ARM_CC_GT  12
#define ARM_CC_LE  13
#define ARM_CC_AL  14

/* Helpers: ARM 32-bit instruction field encoding */
#define ARM_COND(cc)     ((uint32_t)(cc) << 28)
#define ARM_DP_I_IMM     (1u << 25)    /* data-processing immediate */
#define ARM_DP_SHIFT_IMM (1u << 25)    /* also for shift-immediate */
#define ARM_RD(rd)       (((uint32_t)(rd) & 0xFu) << 12)
#define ARM_RN(rn)       (((uint32_t)(rn) & 0xFu) << 16)
#define ARM_RM(rm)       ((uint32_t)(rm) & 0xFu)
#define ARM_RS(rs)       (((uint32_t)(rs) & 0xFu) << 8)
#define ARM_OPCODE(op)   ((op) << 21)  /* op in bits 24:21 */
#define ARM_S_BIT        (1u << 20)    /* set condition flags */
#define ARM_IMM8(imm)    (imm)
#define ARM_IMM12(imm)   (imm)
#define ARM_SHIFT_LSL(i) ((i) << 7)
#define ARM_SHIFT_LSR(i) ((i) << 7 | 1u << 5)
#define ARM_SHIFT_ASR(i) ((i) << 7 | 2u << 5)
#define ARM_SHIFT_ROR(i) ((i) << 7 | 3u << 5)

/* Data processing opcodes (bits 24:21): AND=0, EOR=1, SUB=2, RSB=3, ADD=4, ADC=5, SBC=6, RSC=7,
 * TST=8, TEQ=9, CMP=10, CMN=11, ORR=12, MOV=13, BIC=14, MVN=15 */
#define ARM_OP_AND  0
#define ARM_OP_EOR  1
#define ARM_OP_SUB  2
#define ARM_OP_RSB  3
#define ARM_OP_ADD  4
#define ARM_OP_ADC  5
#define ARM_OP_SBC  6
#define ARM_OP_RSC  7
#define ARM_OP_TST  8
#define ARM_OP_TEQ  9
#define ARM_OP_CMP  10
#define ARM_OP_CMN  11
#define ARM_OP_ORR  12
#define ARM_OP_MOV  13
#define ARM_OP_BIC  14
#define ARM_OP_MVN  15

/* Multiply opcodes: MUL=0, MLA=1, UMAAL=2, MLS=3, UMULL=4, UMLAL=5, SMULL=6, SMLAL=7 */
#define ARM_MUL_MUL  0
#define ARM_MUL_MLA  1
#define ARM_MUL_UMAAL 2
#define ARM_MUL_MLS  3
#define ARM_MUL_UMULL 4
#define ARM_MUL_UMLAL 5
#define ARM_MUL_SMULL 6
#define ARM_MUL_SMLAL 7

/* Branch: B=0, BL=1, BLX=3 (bit 24=1) */
#define ARM_B_B   0
#define ARM_B_BL  1

/* Load/Store: LDR/STR, byte/word, pre/post-index, up/down */
/* LDR/STR: bit 26=1 (0x04000000), P=bit 24, U=bit 23, B=bit 22, W=bit 21, L=bit 20 */
#define ARM_LDR_STR(p,u,b,w,l) (((uint32_t)1<<26)|((p)<<24)|((u)<<23)|((b)<<22)|((w)<<21)|((l)<<20))

/* ========================================================================= */
/* VFPv3/NEON instruction encoding helpers (ARMv7-A) */
/* ========================================================================= */

/* VFP Double-Precision Data Processing (VFPv3) */
/* Encoding: cond(4) | 1110(4) | 1(1) | 1(1) | op(4) | Dd(5) | Dn(5) | Dm(5) | 0(1) */
/* FADDD:  0xEE300A00 | (Dd<<22) | (Dn<<12) | Dm */
/* FSUBD:  0xEE300A40 | (Dd<<22) | (Dn<<12) | Dm */
/* FMULD:  0xEE300A80 | (Dd<<22) | (Dn<<12) | Dm */
/* FDIVD:  0xEE300AC0 | (Dd<<22) | (Dn<<12) | Dm */
#define ARM_VFP_FADDD  0xEE300A00u
#define ARM_VFP_FSUBD  0xEE300A40u
#define ARM_VFP_FMULD  0xEE300A80u
#define ARM_VFP_FDIVD  0xEE300AC0u

/* VFP Unary: FNEGD, FABSD, FSQRTD */
/* Encoding: cond | 11101110 | 1010 | op(4) | Dd(5) | 0(5) | Dm(5) | 0(1) */
#define ARM_VFP_FNEGD  0xEEB00A40u  /* op=0x8 (0x180 in bits 7:4) */
#define ARM_VFP_FABSD  0xEEB00A40u  /* op=0x9 */
#define ARM_VFP_FSQRTD 0xEEB00AC0u  /* op=0xB */

/* VFP Compare: FCMPD, FCMPE (signaling) */
/* Encoding: cond | 11101110 | 1010 | 1111 | Dn(5) | 0(5) | Dm(5) | 0(1) */
#define ARM_VFP_FCMPD  0xEEB00AC0u  /* op=0xF */

/* VFP Move: FCPYD (fmov dD, dN) */
/* Encoding: cond | 11101110 | 1010 | 0100 | Dd(5) | 0(5) | Dm(5) | 0(1) */
#define ARM_VFP_FCPYD  0xEEB00A40u  /* op=0x4 */

/* VFP Load/Store Double (FLDD/FSTD) */
/* Encoding: cond | 1101 | P | U | 1 | W | 1 | Rn(4) | Dd(5) | offset_8(8) */
/* P=1, U=1, W=0, L=1 for load; L=0 for store */
#define ARM_VFP_FLDD(p,u,w) ((1u<<28)|(0xD<<25)|((p)<<24)|((u)<<23)|(1u<<22)|((w)<<21)|(1u<<20))
#define ARM_VFP_FSTD(p,u,w) ((1u<<28)|(0xD<<25)|((p)<<24)|((u)<<23)|(1u<<22)|((w)<<21)|(0u<<20))

/* VFP Conversions */
/* FTOSIZS (double -> signed int): 0xEEB80A40 | (Rd<<16) | (Dm<<12) | 0x180 */
/* SITOS   (signed int -> double): 0xEEB80A40 | (Dd<<22) | (Rm<<16) | 0x180 */
#define ARM_VFP_FTOSIZS  0xEEB80A40u
#define ARM_VFP_SITOS    0xEEB80A40u

/* NEON 128-bit Load/Store (VLD1.64 / VST1.64) */
/* Encoding is complex - VLD1.64 {qD}, [Rn, #imm] */
/* For now use: 0xF9... but need to verify exact encoding */
#define ARM_NEON_VLD1_64(qd, rn, imm)  (0xF9000000u | ((qd & 0xF) << 22) | ((rn & 0xF) << 16) | ((imm & 0xFFF) << 4))
#define ARM_NEON_VST1_64(qd, rn, imm)  (0xF9000000u | ((qd & 0xF) << 22) | ((rn & 0xF) << 16) | ((imm & 0xFFF) << 4) | (1u<<20))

/* 64-bit Multiply (UMULL/SMULL) */
/* UMULL: 0xE0800090 | (RdHi<<16) | (RdLo<<12) | (Rn<<8) | Rm */
/* SMULL: 0xE0C00090 | (RdHi<<16) | (RdLo<<12) | (Rn<<8) | Rm */
#define ARM_UMULL(rdhi, rdlo, rn, rm)  (0xE0800090u | ((rdhi & 0xF) << 16) | ((rdlo & 0xF) << 12) | ((rn & 0xF) << 8) | (rm & 0xF))
#define ARM_SMULL(rdhi, rdlo, rn, rm)  (0xE0C00090u | ((rdhi & 0xF) << 16) | ((rdlo & 0xF) << 12) | ((rn & 0xF) << 8) | (rm & 0xF))

/* =========================================================================
 * Register info for AArch32
 * ========================================================================= */
static ISARegInfo aarch32_regs = {
    .int_regs = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
    },
    .fp_regs = {
        "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
        "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15",
        "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
        "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31",
    },
    .int_arg_regs = {"r0", "r1", "r2", "r3"},
    .fp_arg_regs = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"},
    .int_ret_reg = "r0",
    .fp_ret_reg = "d0",
    .num_int_regs = 16,
    .num_fp_regs = 32,
    .vec_regs = {
        "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7",
        "q8", "q9", "q10", "q11", "q12", "q13", "q14", "q15",
    },
    .num_vec_regs = 16,
};

/* AArch32 AAPCS calling convention */
static const int aarch32_int_arg_regs[] = {0, 1, 2, 3};  /* r0-r3 */
static const int aarch32_fp_arg_regs[] = {0, 1, 2, 3, 4, 5, 6, 7};  /* d0-d7 */
static const int aarch32_callee_saved_gpr[] = {4, 5, 6, 7, 8, 9, 10, 11};  /* r4-r11 */

/* Callee-save slot offsets (4 bytes each, AArch32) */
static const int32_t aarch32_callee_save_slot_offset[] = {
    8, 12, 16, 20, 24, 28, 32, 36,  /* r4-r11 */
};

static const ISACallingConvention aarch32_conv = {
    .int_arg_regs = aarch32_int_arg_regs,
    .int_arg_reg_count = 4,
    .fp_arg_regs = aarch32_fp_arg_regs,
    .fp_arg_reg_count = 8,
    .callee_saved_gpr = aarch32_callee_saved_gpr,
    .callee_saved_gpr_count = 8,
    .callee_save_slot_offsets = aarch32_callee_save_slot_offset,
};

/* =========================================================================
 * VREG to physical register mapping
 * VREG_0..7 -> r0..r7 (argument/result/temps), VREG_8..15 -> r8..r15
 * VREG_F0..F7 -> d0..d7, etc.
 * ========================================================================= */
static inline int aarch32_get_int_reg(int vreg) {
    return zc_temp_is_vreg((uint32_t)vreg) ? zc_vreg_hw_num((uint32_t)vreg) : vreg;
}

static inline int aarch32_get_fp_reg(int vreg) {
    return zc_temp_is_vreg((uint32_t)vreg) ? zc_vreg_hw_num((uint32_t)vreg) : vreg;
}

/* Forward declarations for functions used before definition */
static void aarch32_emit_mov_reg(ZeroCCCompiler *c, int dst, int src);
static void aarch32_emit_lea(ZeroCCCompiler *c, int dst, int base, int32_t offset);
static const char *aarch32_get_hw_reg_name(int phys_reg);

/* =========================================================================
 * ARMv7-A immediate encoding: 8-bit value rotated right by even number of bits
 * Returns 0xFFFFFFFF if the value cannot be encoded as an ARM immediate.
 * ========================================================================= */
static inline uint32_t arm32_encode_imm(int64_t val) {
    uint32_t v = (uint32_t)val;
    if ((v & ~0xFFu) == 0) return v;
    for (int rot = 2; rot < 32; rot += 2) {
        uint32_t rol = (v << rot) | (v >> (32 - rot));
        if ((rol & ~0xFFu) == 0) {
            return rol | ((uint32_t)(rot / 2) << 8);
        }
    }
    return 0xFFFFFFFFu;
}

/* =========================================================================
 * ARM32 emit_mov_imm - load 32-bit immediate into register
 * ========================================================================= */
static void aarch32_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
    uint32_t v = (uint32_t)imm;
    uint32_t arm_imm = arm32_encode_imm((int64_t)v);
    if (arm_imm != 0xFFFFFFFFu) {
        /* MOV Rd, #imm */
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) |
                       ARM_RD(dst_reg) | arm_imm);
        return;
    }
    uint32_t not_imm = arm32_encode_imm((int64_t)(~v));
    if (not_imm != 0xFFFFFFFFu) {
        /* MVN Rd, #~imm */
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MVN) |
                       ARM_RD(dst_reg) | not_imm);
        return;
    }
    /* Fall back to MOVW/MOVT (ARMv7) */
    uint32_t lo16 = v & 0xFFFFu;
    uint32_t hi16 = (v >> 16) & 0xFFFFu;
    arm32_emit_u32(c, 0xE3000000u | (((lo16 >> 12) & 0xFu) << 16) |
                      ((uint32_t)(dst_reg & 0xF) << 12) | (lo16 & 0xFFFu));
    if (hi16 != 0) {
        arm32_emit_u32(c, 0xE3400000u | (((hi16 >> 12) & 0xFu) << 16) |
                          ((uint32_t)(dst_reg & 0xF) << 12) | (hi16 & 0xFFFu));
    }
}

/* =========================================================================
 * Prologue / Epilogue / Leave
 * ========================================================================= */
static void aarch32_emit_prologue(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask) {
    ZeroCCFunction *cur_fn = (c->function_count > 0 && c->lowering.current_func_idx < c->function_count)
                             ? &c->functions[c->lowering.current_func_idx] : NULL;
    bool is_isr = cur_fn && cur_fn->is_interrupt;

    if (c->lowering.is_frameless && !is_isr) return;
    (void)callee_save_mask;

    if (is_isr) {
        arm32_emit_u32(c, 0xE92D100Fu); /* push {r0-r3, r12} */
        if (c->lowering.is_frameless) return;
    }

    /* AAPCS requires 8-byte stack alignment at call sites */
    stack_size = (stack_size + 7) & ~7;

    if (c->lowering.frame_align > 16) {
        size_t frame = (size_t)c->lowering.frame_delta;
        uint32_t N = (uint32_t)c->lowering.frame_align;
        /* STMDB sp!, {r11, lr}  (push {r11, lr}) */
        arm32_emit_u32(c, 0xE92D4800u);
        /* ADD r11, sp, #0  (set frame pointer) */
        arm32_emit_u32(c, 0xE28DB000u);
        /* Realign SP to N: BIC sp, sp, #(N-1) */
        uint32_t enc = arm32_encode_imm((int64_t)(N - 1));
        if (enc != 0xFFFFFFFFu) {
            arm32_emit_u32(c, 0xE3CDD000u | enc);
        } else {
            aarch32_emit_mov_imm(c, 12, (int64_t)~((int64_t)N - 1));
            arm32_emit_u32(c, 0xE00DD00Cu); /* AND sp, sp, r12 */
        }
        if (frame > 0) {
            uint32_t f_enc = arm32_encode_imm((int64_t)frame);
            if (f_enc != 0xFFFFFFFFu) {
                arm32_emit_u32(c, 0xE24DD000u | f_enc);
            } else {
                aarch32_emit_mov_imm(c, 12, (int64_t)frame);
                arm32_emit_u32(c, 0xE04DD00Cu);
            }
        }
        return;
    }

    /* STMDB sp!, {r11, lr}  (push {r11, lr}) */
    arm32_emit_u32(c, 0xE92D4800u);
    /* ADD r11, sp, #0  (set frame pointer) */
    arm32_emit_u32(c, 0xE28DB000u);
    if (stack_size > 0) {
        uint32_t enc = arm32_encode_imm((int64_t)stack_size);
        if (enc != 0xFFFFFFFFu) {
            /* SUB sp, sp, #stack_size */
            arm32_emit_u32(c, 0xE24DD000u | enc);
        } else {
            aarch32_emit_mov_imm(c, 12, (int64_t)stack_size);
            /* SUB sp, sp, r12 */
            arm32_emit_u32(c, 0xE04DD00Cu);
        }
    }
}

static void aarch32_emit_leave(ZeroCCCompiler *c) {
    ZeroCCFunction *cur_fn = (c->function_count > 0 && c->lowering.current_func_idx < c->function_count)
                             ? &c->functions[c->lowering.current_func_idx] : NULL;
    bool is_isr = cur_fn && cur_fn->is_interrupt;

    if (!c->lowering.is_frameless) {
        /* ADD sp, r11, #0  (restore sp from fp) */
        arm32_emit_u32(c, 0xE28BD000u);
        /* LDMIA sp!, {r11, lr}  (pop {r11, lr}) */
        arm32_emit_u32(c, 0xE8BD4800u);
    }
    if (is_isr) {
        arm32_emit_u32(c, 0xE8BD100Fu); /* pop {r0-r3, r12} */
    }
}

static void aarch32_emit_ret(ZeroCCCompiler *c); /* fwd */

static void aarch32_emit_epilogue(ZeroCCCompiler *c) {
    aarch32_emit_leave(c);
    aarch32_emit_ret(c);
}

/* =========================================================================
 * Stack guard (canary)
 * ========================================================================= */
static void aarch32_emit_stack_guard_init(ZeroCCCompiler *c, int32_t slot_off) {
    /* LDR r12, =__stack_chk_guard  (load guard address) */
    /* STR r12, [r11, #-slot_off]  (store canary in frame) */
    aarch32_emit_mov_imm(c, 12, 0);  /* placeholder - actual address from GOT */
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_LDR_STR(1,1,0,0,1) | ARM_RN(11) | ARM_RD(12) | (slot_off & 0xFFF));
}

static void aarch32_emit_stack_guard_check(ZeroCCCompiler *c, int32_t slot_off) {
    /* LDR r12, [r11, #-slot_off]  (load canary) */
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_LDR_STR(1,1,0,0,1) | ARM_RN(11) | ARM_RD(12) | (slot_off & 0xFFF));
    /* LDR r12, [r12]  (dereference guard) */
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_LDR_STR(1,1,0,0,1) | ARM_RN(12) | ARM_RD(12) | 0);
    /* CMP r12, r12  (placeholder - compare with saved canary) */
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_CMP) |
                   ARM_RN(12) | 0);
    /* BNE __stack_chk_fail - use label patching mechanism */
    size_t patch_pos = me_get_offset(c);
    arm32_emit_u32(c, ARM_COND(ARM_CC_NE) | 0xEA000000u);  /* placeholder offset */
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, "__stack_chk_fail") == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
}

/* =========================================================================
 * Integer arithmetic
 * ========================================================================= */
static void aarch32_emit_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_ADD) |
                   ARM_RD(dst) | ARM_RN(src1) | ARM_RM(src2));
}

static void aarch32_emit_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_SUB) |
                   ARM_RD(dst) | ARM_RN(src1) | ARM_RM(src2));
}

static void aarch32_emit_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* MUL dst, src1, src2 */
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | 0xE0000090u | (dst << 16) | (src2 << 8) | src1);
}

static void aarch32_emit_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* SDIV / UDIV dst, src1, src2 (ARMv7VE / ARMv7-A idiva) */
    uint32_t base = c->lowering.div_is_unsigned ? 0xE730F010u : 0xE710F010u;
    arm32_emit_u32(c, base | ((uint32_t)(dst & 0xF) << 16) |
                      ((uint32_t)(src2 & 0xF) << 8) | (uint32_t)(src1 & 0xF));
}

static void aarch32_emit_mod(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* No direct MOD instruction; compute: mod = a - (a/b)*b */
    int t = (src1 != 12 && src2 != 12) ? 12 : ((src1 != 9 && src2 != 9) ? 9 : 14);
    aarch32_emit_div(c, t, src1, src2);  /* t = a/b */
    aarch32_emit_mul(c, t, t, src2);      /* t = (a/b)*b */
    aarch32_emit_sub(c, dst, src1, t);    /* dst = a - t */
}

static void aarch32_emit_and(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_AND) |
                   ARM_RD(dst) | ARM_RN(src1) | ARM_RM(src2));
}

static void aarch32_emit_or(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_ORR) |
                   ARM_RD(dst) | ARM_RN(src1) | ARM_RM(src2));
}

static void aarch32_emit_xor(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_EOR) |
                   ARM_RD(dst) | ARM_RN(src1) | ARM_RM(src2));
}

static void aarch32_emit_shl(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* LSL dst, src1, src2 (MOV dst, src1, LSL src2) */
    arm32_emit_u32(c, 0xE1A00010u | ARM_RD(dst & 0xF) | ARM_RS(src2 & 0xF) | ARM_RM(src1 & 0xF));
}

static void aarch32_emit_shr(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* LSR dst, src1, src2 (MOV dst, src1, LSR src2) */
    arm32_emit_u32(c, 0xE1A00030u | ARM_RD(dst & 0xF) | ARM_RS(src2 & 0xF) | ARM_RM(src1 & 0xF));
}

static void aarch32_emit_sar(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* ASR dst, src1, src2 (MOV dst, src1, ASR src2) */
    arm32_emit_u32(c, 0xE1A00050u | ARM_RD(dst & 0xF) | ARM_RS(src2 & 0xF) | ARM_RM(src1 & 0xF));
}

static void aarch32_emit_cmp(ZeroCCCompiler *c, int src1, int src2) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_CMP) | ARM_S_BIT |
                   ARM_RN(src1) | ARM_RM(src2));
}

static void aarch32_emit_64_pred(ZeroCCCompiler *c, int dst, int cond) {
    int alo = c->lowering.cmp64_s1_lo, ahi = c->lowering.cmp64_s1_hi;
    int blo = c->lowering.cmp64_s2_lo, bhi = c->lowering.cmp64_s2_hi;
    int ip = 12;

    if (cond == 4) { /* CC_EQ: == */
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_CMP) | ARM_S_BIT | ARM_RN(ahi) | ARM_RM(bhi));
        arm32_emit_u32(c, ARM_COND(ARM_CC_EQ) | ARM_OPCODE(ARM_OP_CMP) | ARM_S_BIT | ARM_RN(alo) | ARM_RM(blo));
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) | ARM_RD(dst) | 0);
        arm32_emit_u32(c, ARM_COND(ARM_CC_EQ) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) | ARM_RD(dst) | 1);
        return;
    }
    if (cond == 5) { /* CC_NE: != */
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_CMP) | ARM_S_BIT | ARM_RN(ahi) | ARM_RM(bhi));
        arm32_emit_u32(c, ARM_COND(ARM_CC_EQ) | ARM_OPCODE(ARM_OP_CMP) | ARM_S_BIT | ARM_RN(alo) | ARM_RM(blo));
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) | ARM_RD(dst) | 0);
        arm32_emit_u32(c, ARM_COND(ARM_CC_NE) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) | ARM_RD(dst) | 1);
        return;
    }
    if (cond >= 6 && cond <= 9) { /* Unsigned: CC_FLT_LT(6), CC_FLT_LE(7), CC_FLT_GT(8), CC_FLT_GE(9) */
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_CMP) | ARM_S_BIT | ARM_RN(ahi) | ARM_RM(bhi));
        arm32_emit_u32(c, ARM_COND(ARM_CC_EQ) | ARM_OPCODE(ARM_OP_CMP) | ARM_S_BIT | ARM_RN(alo) | ARM_RM(blo));
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) | ARM_RD(dst) | 0);
        int cc;
        switch (cond) {
            case 6: cc = ARM_CC_CC; break; /* LO / CC: unsigned < */
            case 7: cc = ARM_CC_LS; break; /* LS: unsigned <= */
            case 8: cc = ARM_CC_HI; break; /* HI: unsigned > */
            case 9: cc = ARM_CC_CS; break; /* HS / CS: unsigned >= */
            default: cc = ARM_CC_AL; break;
        }
        arm32_emit_u32(c, ARM_COND(cc) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) | ARM_RD(dst) | 1);
        return;
    }
    /* Signed comparisons: CC_LT(0), CC_LE(1), CC_GT(2), CC_GE(3) */
    int s_alo = alo, s_ahi = ahi, s_blo = blo, s_bhi = bhi;
    int check_cc = ARM_CC_LT;
    if (cond == 0) { /* a < b */
        s_alo = alo; s_ahi = ahi; s_blo = blo; s_bhi = bhi;
        check_cc = ARM_CC_LT;
    } else if (cond == 3) { /* a >= b */
        s_alo = alo; s_ahi = ahi; s_blo = blo; s_bhi = bhi;
        check_cc = ARM_CC_GE;
    } else if (cond == 2) { /* a > b <=> b < a */
        s_alo = blo; s_ahi = bhi; s_blo = alo; s_bhi = ahi;
        check_cc = ARM_CC_LT;
    } else if (cond == 1) { /* a <= b <=> b >= a */
        s_alo = blo; s_ahi = bhi; s_blo = alo; s_bhi = ahi;
        check_cc = ARM_CC_GE;
    }
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_SUB) | ARM_S_BIT |
                   ARM_RD(ip) | ARM_RN(s_alo) | ARM_RM(s_blo));
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_SBC) | ARM_S_BIT |
                   ARM_RD(ip) | ARM_RN(s_ahi) | ARM_RM(s_bhi));
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) | ARM_RD(dst) | 0);
    arm32_emit_u32(c, ARM_COND(check_cc) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) | ARM_RD(dst) | 1);
}

static void aarch32_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    if (c->lowering.cmp_is_64) {
        c->lowering.cmp_is_64 = 0;
        aarch32_emit_64_pred(c, dst, cond);
        return;
    }
    static const int arm_cc[14] = {
        ARM_CC_LT, ARM_CC_LE, ARM_CC_GT, ARM_CC_GE, ARM_CC_EQ, ARM_CC_NE,
        ARM_CC_CC, ARM_CC_LS, ARM_CC_HI, ARM_CC_CS, ARM_CC_EQ, ARM_CC_NE,
        ARM_CC_VS, ARM_CC_VC
    };
    int cc = (cond >= 0 && cond < 14) ? arm_cc[cond] : ARM_CC_EQ;
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) |
                   ARM_RD(dst) | 0);
    arm32_emit_u32(c, ARM_COND(cc) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) |
                   ARM_RD(dst) | 1);
}

static void aarch32_emit_setl(ZeroCCCompiler *c, int dst)  { aarch32_emit_cond_set(c, dst, 0); }
static void aarch32_emit_setle(ZeroCCCompiler *c, int dst) { aarch32_emit_cond_set(c, dst, 1); }
static void aarch32_emit_setg(ZeroCCCompiler *c, int dst)  { aarch32_emit_cond_set(c, dst, 2); }
static void aarch32_emit_setge(ZeroCCCompiler *c, int dst) { aarch32_emit_cond_set(c, dst, 3); }
static void aarch32_emit_sete(ZeroCCCompiler *c, int dst)  { aarch32_emit_cond_set(c, dst, 4); }
static void aarch32_emit_setne(ZeroCCCompiler *c, int dst) { aarch32_emit_cond_set(c, dst, 5); }

static void aarch32_emit_neg(ZeroCCCompiler *c, int dst, int src) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_RSB) |
                   ARM_RD(dst) | ARM_RN(src) | 0);
}

static void aarch32_emit_extend(ZeroCCCompiler *c, int dst, int src, int width, int sign) {
    if (width == 0 || width >= 8 || width == 4) {
        aarch32_emit_mov_reg(c, dst, src);
        return;
    }
    if (width == 1) {  /* byte: SXTB / UXTB */
        uint32_t base = sign ? 0xE6AF0070u : 0xE6EF0070u;
        arm32_emit_u32(c, base | ((uint32_t)(dst & 0xF) << 12) | (uint32_t)(src & 0xF));
    } else if (width == 2) {  /* halfword: SXTH / UXTH */
        uint32_t base = sign ? 0xE6BF0070u : 0xE6FF0070u;
        arm32_emit_u32(c, base | ((uint32_t)(dst & 0xF) << 12) | (uint32_t)(src & 0xF));
    }
}

static void aarch32_emit_mov_reg(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_MOV) |
                   ARM_RD(dst) | ARM_RM(src));
}

static void aarch32_emit_not(ZeroCCCompiler *c, int dst, int src) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_MVN) |
                   ARM_RD(dst) | ARM_RM(src));
}

static void aarch32_emit_test(ZeroCCCompiler *c, int src1, int src2) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_TST) | ARM_S_BIT |
                   ARM_RN(src1) | ARM_RM(src2));
}

static void aarch32_emit_cqo(ZeroCCCompiler *c) {
    (void)c;
}

static void aarch32_emit_idiv(ZeroCCCompiler *c, int src) {
    (void)c; (void)src;
}

static void aarch32_emit_lea(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    if (offset == 0) {
        aarch32_emit_mov_reg(c, dst, base);
        return;
    }
    int is_add = (offset >= 0);
    int64_t abs_off = is_add ? (int64_t)offset : -(int64_t)offset;
    uint32_t enc = arm32_encode_imm(abs_off);
    if (enc != 0xFFFFFFFFu) {
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM |
                       ARM_OPCODE(is_add ? ARM_OP_ADD : ARM_OP_SUB) |
                       ARM_RN(base) | ARM_RD(dst) | enc);
    } else {
        int scratch = (base != 12) ? 12 : 9;
        aarch32_emit_mov_imm(c, scratch, offset);
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_ADD) |
                       ARM_RD(dst) | ARM_RN(base) | ARM_RM(scratch));
    }
}

/* =========================================================================
 * Load/Store
 * ========================================================================= */
static void aarch32_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    if (offset >= -4095 && offset <= 4095) {
        int u = (offset >= 0) ? 1 : 0;
        int imm = offset >= 0 ? offset : -offset;
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_LDR_STR(1, u, 0, 0, 1) |
                       ARM_RN(base_reg) | ARM_RD(dst_reg) | (imm & 0xFFF));
    } else {
        int scratch = (base_reg != 12) ? 12 : 9;
        aarch32_emit_mov_imm(c, scratch, offset);
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | (1u << 25) | ARM_LDR_STR(1, 1, 0, 0, 1) |
                       ARM_RN(base_reg) | ARM_RD(dst_reg) | ARM_RM(scratch));
    }
}

static void aarch32_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    if (offset >= -4095 && offset <= 4095) {
        int u = (offset >= 0) ? 1 : 0;
        int imm = offset >= 0 ? offset : -offset;
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_LDR_STR(1, u, 0, 0, 0) |
                       ARM_RN(base_reg) | ARM_RD(src_reg) | (imm & 0xFFF));
    } else {
        int scratch = (base_reg != 12 && src_reg != 12) ? 12 : ((base_reg != 9 && src_reg != 9) ? 9 : 14);
        aarch32_emit_mov_imm(c, scratch, offset);
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | (1u << 25) | ARM_LDR_STR(1, 1, 0, 0, 0) |
                       ARM_RN(base_reg) | ARM_RD(src_reg) | ARM_RM(scratch));
    }
}

/* Byte/half/word load/store */
static void aarch32_emit_load_byte(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_LDR_STR(1, 1, 1, 0, 1) | ARM_RN(src_reg) | ARM_RD(dst_reg) | 0);
}
static void aarch32_emit_load_16(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    arm32_emit_u32(c, 0xE1D000B0u | ((uint32_t)(src_reg & 0xF) << 16) | ((uint32_t)(dst_reg & 0xF) << 12));  /* LDRH */
}
static void aarch32_emit_load_32(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    aarch32_emit_load(c, dst_reg, src_reg, 0);
}
static void aarch32_emit_store_byte(ZeroCCCompiler *c, int src_reg, int base_reg) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_LDR_STR(1, 1, 1, 0, 0) | ARM_RN(base_reg) | ARM_RD(src_reg) | 0);
}
static void aarch32_emit_store_16(ZeroCCCompiler *c, int src_reg, int base_reg) {
    arm32_emit_u32(c, 0xE1C000B0u | ((uint32_t)(base_reg & 0xF) << 16) | ((uint32_t)(src_reg & 0xF) << 12));  /* STRH */
}
static void aarch32_emit_store_32(ZeroCCCompiler *c, int src_reg, int base_reg) {
    aarch32_emit_store(c, src_reg, base_reg, 0);
}
static void aarch32_emit_store_64(ZeroCCCompiler *c, int src_reg, int base_reg) {
    aarch32_emit_store(c, src_reg, base_reg, 0);
}

/* =========================================================================
 * Deref (load from address in register)
 * ========================================================================= */
static void aarch32_emit_deref(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    aarch32_emit_load(c, dst_reg, src_reg, 0);
}

/* =========================================================================
 * VLA allocation
 * ========================================================================= */
static void aarch32_emit_vla_alloc(ZeroCCCompiler *c, int dst_reg, int size_reg) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_SUB) |
                   ARM_RD(13) | ARM_RN(13) | ARM_RM(size_reg));
    aarch32_emit_mov_reg(c, dst_reg, 13);
}

/* =========================================================================
 * RIP-relative addressing for string literals
 * ========================================================================= */
static void aarch32_emit_lea_rip(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    uint32_t r = (uint32_t)(dst_reg & 0xF);
    uint16_t lo16 = (uint16_t)(rodata_offset & 0xFFFF);
    uint16_t hi16 = (uint16_t)((rodata_offset >> 16) & 0xFFFF);
    uint32_t imm4_lo = ((uint32_t)lo16 >> 12) & 0xF;
    uint32_t imm12_lo = (uint32_t)lo16 & 0xFFF;
    uint32_t imm4_hi = ((uint32_t)hi16 >> 12) & 0xF;
    uint32_t imm12_hi = (uint32_t)hi16 & 0xFFF;
    arm32_emit_u32(c, 0xE3000000u | (imm4_lo << 16) | (r << 12) | imm12_lo);  /* movw dst, #lo16 */
    size_t patch_pos = me_get_offset(c);
    arm32_emit_u32(c, 0xE3400000u | (imm4_hi << 16) | (r << 12) | imm12_hi);  /* movt dst, #hi16 */
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_relocs[c->str_reloc_count].dst_reg = dst_reg;
        c->str_reloc_count++;
    }
}

/* =========================================================================
 * Branch / Jump / Call
 * ========================================================================= */

static size_t aarch32_emit_branch(ZeroCCCompiler *c, uint32_t cond, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm32_emit_u32(c, ARM_COND(cond) | 0x0A000000u);
    if (label && label[0]) {
        for (int i = 0; i < c->label_count; i++) {
            if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
                if (c->labels[i].patch_count < 16) {
                    c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
                }
                break;
            }
        }
    }
    return patch_pos;
}

static size_t aarch32_emit_jmp(ZeroCCCompiler *c, const char *label) {
    return aarch32_emit_branch(c, ARM_CC_AL, label);
}

static size_t aarch32_emit_jeq(ZeroCCCompiler *c, const char *label) {
    return aarch32_emit_branch(c, ARM_CC_EQ, label);
}

static size_t aarch32_emit_jne(ZeroCCCompiler *c, const char *label) {
    return aarch32_emit_branch(c, ARM_CC_NE, label);
}

static size_t aarch32_emit_jlt(ZeroCCCompiler *c, const char *label) {
    return aarch32_emit_branch(c, ARM_CC_LT, label);
}

static size_t aarch32_emit_jle(ZeroCCCompiler *c, const char *label) {
    return aarch32_emit_branch(c, ARM_CC_LE, label);
}

static size_t aarch32_emit_jgt(ZeroCCCompiler *c, const char *label) {
    return aarch32_emit_branch(c, ARM_CC_GT, label);
}

static size_t aarch32_emit_jge(ZeroCCCompiler *c, const char *label) {
    return aarch32_emit_branch(c, ARM_CC_GE, label);
}

static size_t aarch32_emit_jcond(ZeroCCCompiler *c, int cond, const char *label) {
    static const int arm_cc[14] = {
        ARM_CC_LT, ARM_CC_LE, ARM_CC_GT, ARM_CC_GE, ARM_CC_EQ, ARM_CC_NE,
        ARM_CC_CC, ARM_CC_LS, ARM_CC_HI, ARM_CC_CS, ARM_CC_EQ, ARM_CC_NE,
        ARM_CC_VS, ARM_CC_VC
    };
    int cc = (cond >= 0 && cond < 14) ? arm_cc[cond] : ARM_CC_EQ;
    return aarch32_emit_branch(c, (uint32_t)cc, label);
}

static void aarch32_emit_call(ZeroCCCompiler *c, const char *func_name) {
    size_t patch_pos = me_get_offset(c);
    arm32_emit_u32(c, 0xEBFFFFFEu);  /* BL with placeholder */

    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = patch_pos;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", func_name);
        r->is_call = 1;
    }
}

static void aarch32_emit_call_reg(ZeroCCCompiler *c, int reg) {
    arm32_emit_u32(c, 0xE12FFF30u | (uint32_t)(reg & 0xF));  /* BLX reg */
}

static void aarch32_emit_jmp_reg(ZeroCCCompiler *c, int reg) {
    arm32_emit_u32(c, 0xE12FFF10u | (uint32_t)(reg & 0xF));  /* BX reg */
}

static size_t aarch32_emit_lea_label(ZeroCCCompiler *c, int dst_reg) {
    size_t patch_pos = me_get_offset(c);
    arm32_emit_u32(c, 0xE28F0000u | ((uint32_t)(dst_reg & 0xF) << 12));  /* ADR placeholder */
    return patch_pos;
}

static void aarch32_emit_ret(ZeroCCCompiler *c) {
    ZeroCCFunction *cur_fn = (c->function_count > 0 && c->lowering.current_func_idx < c->function_count)
                             ? &c->functions[c->lowering.current_func_idx] : NULL;
    if (cur_fn && cur_fn->is_interrupt) {
        if (cur_fn->interrupt_mode <= 1) {
            arm32_emit_u32(c, 0xE25EF004u); /* subs pc, lr, #4 */
        } else {
            arm32_emit_u32(c, 0xE1B0F00Eu); /* movs pc, lr */
        }
        return;
    }
    arm32_emit_u32(c, 0xE12FFF1Eu);  /* BX lr */
}

static void aarch32_emit_label(ZeroCCCompiler *c, const char *name) {
    (void)c; (void)name;
}

/* patch_offset is the position of the instruction (as returned by emit_branch) */
static size_t aarch32_patch_jmp(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset) {
    if (patch_offset + 4 <= c->text_section.length) {
        uint32_t orig = 0;
        memcpy(&orig, c->text_section.data + patch_offset, 4);
        int64_t byte_diff = (int64_t)target_offset - (int64_t)(patch_offset + 8);
        if ((orig & 0x0FE00000u) == 0x02800000u) {
            /* ADR Rd, label (ADD/SUB Rd, pc, #imm) */
            int is_add = (byte_diff >= 0);
            uint32_t enc = arm32_encode_imm(is_add ? byte_diff : -byte_diff);
            if (enc != 0xFFFFFFFFu) {
                uint32_t op = is_add ? 0x02800000u : 0x02400000u;
                uint32_t patched = (orig & 0xF00FF000u) | op | enc;
                me_patch_u32(c, patch_offset, patched);
            }
        } else {
            int32_t rel = (int32_t)(byte_diff >> 2);
            uint32_t patched = (orig & 0xFF000000u) | ((uint32_t)rel & 0x00FFFFFFu);
            me_patch_u32(c, patch_offset, patched);
        }
    }
    return patch_offset;
}

/* Resolve all recorded label patches */
static void aarch32_resolve_labels(ZeroCCCompiler *c) {
    for (int i = 0; i < c->label_count; i++) {
        size_t target = c->labels[i].offset;
        for (int j = 0; j < c->labels[i].patch_count; j++) {
            aarch32_patch_jmp(c, c->labels[i].patches[j], target);
        }
    }
}

/* =========================================================================
 * Floating point (VFPv3) - Phase 2
 * ========================================================================= */

/* VFP Load Double: VLDR dD, [Rn, #offset] */
static void aarch32_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    int D = (dst_reg >> 4) & 1;
    int Vd = dst_reg & 0xF;
    if (offset >= 0 && offset <= 1020 && (offset & 3) == 0) {
        int imm8 = offset / 4;
        arm32_emit_u32(c, 0xED900B00u | (D << 22) | ((base_reg & 0xF) << 16) | (Vd << 12) | imm8);
    } else if (offset < 0 && offset >= -1020 && ((-offset) & 3) == 0) {
        int imm8 = (-offset) / 4;
        arm32_emit_u32(c, 0xED100B00u | (D << 22) | ((base_reg & 0xF) << 16) | (Vd << 12) | imm8);
    } else {
        int scratch = (base_reg != 12) ? 12 : 9;
        aarch32_emit_lea(c, scratch, base_reg, offset);
        arm32_emit_u32(c, 0xED900B00u | (D << 22) | ((scratch & 0xF) << 16) | (Vd << 12) | 0);
    }
}

/* VFP Store Double: VSTR dD, [Rn, #offset] */
static void aarch32_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    int D = (src_reg >> 4) & 1;
    int Vd = src_reg & 0xF;
    if (offset >= 0 && offset <= 1020 && (offset & 3) == 0) {
        int imm8 = offset / 4;
        arm32_emit_u32(c, 0xED800B00u | (D << 22) | ((base_reg & 0xF) << 16) | (Vd << 12) | imm8);
    } else if (offset < 0 && offset >= -1020 && ((-offset) & 3) == 0) {
        int imm8 = (-offset) / 4;
        arm32_emit_u32(c, 0xED000B00u | (D << 22) | ((base_reg & 0xF) << 16) | (Vd << 12) | imm8);
    } else {
        int scratch = (base_reg != 12) ? 12 : 9;
        aarch32_emit_lea(c, scratch, base_reg, offset);
        arm32_emit_u32(c, 0xED800B00u | (D << 22) | ((scratch & 0xF) << 16) | (Vd << 12) | 0);
    }
}

/* VFP Load Single: VLDR.32 s(2*dst_reg), [Rn, #offset] */
static void aarch32_emit_load_single_fp_raw(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    int s_reg = dst_reg * 2;
    int D = s_reg & 1;
    int Vd = (s_reg >> 1) & 0xF;
    if (offset >= 0 && offset <= 1020 && (offset & 3) == 0) {
        int imm8 = offset / 4;
        arm32_emit_u32(c, 0xED900A00u | (D << 22) | ((base_reg & 0xF) << 16) | (Vd << 12) | imm8);
    } else if (offset < 0 && offset >= -1020 && ((-offset) & 3) == 0) {
        int imm8 = (-offset) / 4;
        arm32_emit_u32(c, 0xED100A00u | (D << 22) | ((base_reg & 0xF) << 16) | (Vd << 12) | imm8);
    } else {
        int scratch = (base_reg != 12) ? 12 : 9;
        aarch32_emit_lea(c, scratch, base_reg, offset);
        arm32_emit_u32(c, 0xED900A00u | (D << 22) | ((scratch & 0xF) << 16) | (Vd << 12) | 0);
    }
}

/* VFP Load Single and Widen to Double: VLDR.32 + VCVT.F64.F32 */
static void aarch32_emit_load_single_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    aarch32_emit_load_single_fp_raw(c, dst_reg, base_reg, offset);
    int s_reg = dst_reg * 2;
    int D = (dst_reg >> 4) & 1, Vd = dst_reg & 0xF;
    int M = s_reg & 1, Vm = (s_reg >> 1) & 0xF;
    arm32_emit_u32(c, 0xEEB70BC0u | (D << 22) | (Vd << 12) | (M << 5) | Vm);
}

/* VFP Store Single: VSTR.32 s(2*src_reg), [Rn, #offset] */
static void aarch32_emit_store_single_fp_raw(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    int s_reg = src_reg * 2;
    int D = s_reg & 1;
    int Vd = (s_reg >> 1) & 0xF;
    if (offset >= 0 && offset <= 1020 && (offset & 3) == 0) {
        int imm8 = offset / 4;
        arm32_emit_u32(c, 0xED800A00u | (D << 22) | ((base_reg & 0xF) << 16) | (Vd << 12) | imm8);
    } else if (offset < 0 && offset >= -1020 && ((-offset) & 3) == 0) {
        int imm8 = (-offset) / 4;
        arm32_emit_u32(c, 0xED000A00u | (D << 22) | ((base_reg & 0xF) << 16) | (Vd << 12) | imm8);
    } else {
        int scratch = (base_reg != 12) ? 12 : 9;
        aarch32_emit_lea(c, scratch, base_reg, offset);
        arm32_emit_u32(c, 0xED800A00u | (D << 22) | ((scratch & 0xF) << 16) | (Vd << 12) | 0);
    }
}

/* VFP Narrow Double to Single and Store: VCVT.F32.F64 s30, d<src_reg> + VSTR.32 s30 */
static void aarch32_emit_store_single_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    int tmp_d = 15; /* 15 -> s30 */
    int s_dst = tmp_d * 2;
    int D = s_dst & 1, Vd = (s_dst >> 1) & 0xF;
    int M = (src_reg >> 4) & 1, Vm = src_reg & 0xF;
    arm32_emit_u32(c, 0xEEB70AC0u | (D << 22) | (Vd << 12) | (M << 5) | Vm);
    aarch32_emit_store_single_fp_raw(c, tmp_d, base_reg, offset);
}

/* VFP In-register Narrow Double to Single: VCVT.F32.F64 */
static void aarch32_emit_fp_cvttsd2ss(ZeroCCCompiler *c, int dst, int src) {
    int s_dst = dst * 2;
    int D = s_dst & 1, Vd = (s_dst >> 1) & 0xF;
    int M = (src >> 4) & 1, Vm = src & 0xF;
    arm32_emit_u32(c, 0xEEB70AC0u | (D << 22) | (Vd << 12) | (M << 5) | Vm);
}

/* VFP In-register Widen Single to Double: VCVT.F64.F32 */
static void aarch32_emit_fp_cvtss2sd(ZeroCCCompiler *c, int dst, int src) {
    int s_src = src * 2;
    int D = (dst >> 4) & 1, Vd = dst & 0xF;
    int M = s_src & 1, Vm = (s_src >> 1) & 0xF;
    arm32_emit_u32(c, 0xEEB70BC0u | (D << 22) | (Vd << 12) | (M << 5) | Vm);
}

/* VFP Signed Int to Single: VMOV + VCVT.F32.S32 */
static void aarch32_emit_fp_cvtsi2ss(ZeroCCCompiler *c, int dst, int src) {
    /* vmov s0, Rm */
    arm32_emit_u32(c, 0xEE000A10u | ((src & 0xF) << 12));
    /* vcvt.f32.s32 s<2*dst>, s0 */
    int s_dst = dst * 2;
    int D = s_dst & 1, Vd = (s_dst >> 1) & 0xF;
    arm32_emit_u32(c, 0xEEB80AC0u | (D << 22) | (Vd << 12));
}

/* VFP Load Double from rodata: LDR dD, =address (via literal pool) */
static void aarch32_emit_load_fp_rodata(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    int scratch = 12; /* r12 (IP) */
    uint32_t r = (uint32_t)(scratch & 0xF);
    uint16_t lo16 = (uint16_t)(rodata_offset & 0xFFFF);
    uint16_t hi16 = (uint16_t)((rodata_offset >> 16) & 0xFFFF);
    uint32_t imm4_lo = ((uint32_t)lo16 >> 12) & 0xF;
    uint32_t imm12_lo = (uint32_t)lo16 & 0xFFF;
    uint32_t imm4_hi = ((uint32_t)hi16 >> 12) & 0xF;
    uint32_t imm12_hi = (uint32_t)hi16 & 0xFFF;
    arm32_emit_u32(c, 0xE3000000u | (imm4_lo << 16) | (r << 12) | imm12_lo);  /* movw r12, #lo16 */
    size_t patch_pos = me_get_offset(c);
    arm32_emit_u32(c, 0xE3400000u | (imm4_hi << 16) | (r << 12) | imm12_hi);  /* movt r12, #hi16 */
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_relocs[c->str_reloc_count].dst_reg = scratch;
        c->str_reloc_count++;
    }
    aarch32_emit_load_fp(c, dst_reg, scratch, 0);
}

/* VFP FADDD: faddd dD, dN, dM */
static void aarch32_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    int D = (dst >> 4) & 1, Vd = dst & 0xF;
    int N = (src1 >> 4) & 1, Vn = src1 & 0xF;
    int M = (src2 >> 4) & 1, Vm = src2 & 0xF;
    arm32_emit_u32(c, 0xEE300B00u | (D << 22) | (Vn << 16) | (Vd << 12) | (N << 7) | (M << 5) | Vm);
}

/* VFP FSUBD: fsubd dD, dN, dM */
static void aarch32_emit_fp_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    int D = (dst >> 4) & 1, Vd = dst & 0xF;
    int N = (src1 >> 4) & 1, Vn = src1 & 0xF;
    int M = (src2 >> 4) & 1, Vm = src2 & 0xF;
    arm32_emit_u32(c, 0xEE300B40u | (D << 22) | (Vn << 16) | (Vd << 12) | (N << 7) | (M << 5) | Vm);
}

/* VFP FMULD: fmuld dD, dN, dM */
static void aarch32_emit_fp_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    int D = (dst >> 4) & 1, Vd = dst & 0xF;
    int N = (src1 >> 4) & 1, Vn = src1 & 0xF;
    int M = (src2 >> 4) & 1, Vm = src2 & 0xF;
    arm32_emit_u32(c, 0xEE200B00u | (D << 22) | (Vn << 16) | (Vd << 12) | (N << 7) | (M << 5) | Vm);
}

/* VFP FDIVD: fdivd dD, dN, dM */
static void aarch32_emit_fp_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    int D = (dst >> 4) & 1, Vd = dst & 0xF;
    int N = (src1 >> 4) & 1, Vn = src1 & 0xF;
    int M = (src2 >> 4) & 1, Vm = src2 & 0xF;
    arm32_emit_u32(c, 0xEE800B00u | (D << 22) | (Vn << 16) | (Vd << 12) | (N << 7) | (M << 5) | Vm);
}

/* VFP FNEGD: fnegd dD, dN */
static void aarch32_emit_fp_neg(ZeroCCCompiler *c, int dst, int src) {
    int D = (dst >> 4) & 1, Vd = dst & 0xF;
    int M = (src >> 4) & 1, Vm = src & 0xF;
    arm32_emit_u32(c, 0xEEB10B40u | (D << 22) | (Vd << 12) | (M << 5) | Vm);
}

/* VFP FCMPD: fcmpd dN, dM (sets FPSCR flags, transfers to APSR) */
static void aarch32_emit_fp_cmp(ZeroCCCompiler *c, int src1, int src2) {
    int D = (src1 >> 4) & 1, Vd = src1 & 0xF;
    int M = (src2 >> 4) & 1, Vm = src2 & 0xF;
    arm32_emit_u32(c, 0xEEB40B40u | (D << 22) | (Vd << 12) | (M << 5) | Vm);
    arm32_emit_u32(c, 0xEEF1FA10u);  /* vmrs apsr_nzcv, fpscr */
}

/* VFP FTOSIZS: ftosizs Rd, dN (double -> signed int, truncate) */
static void aarch32_emit_fp_cvtsi(ZeroCCCompiler *c, int int_dst, int fp_src) {
    int M = (fp_src >> 4) & 1, Vm = fp_src & 0xF;
    arm32_emit_u32(c, 0xEEBD0BC0u | (M << 5) | Vm);
    arm32_emit_u32(c, 0xEE100A10u | ((int_dst & 0xF) << 12));
}

/* VFP SITOS: sitos dD, Rm (signed int -> double) */
static void aarch32_emit_fp_cvtsf(ZeroCCCompiler *c, int fp_dst, int int_src) {
    arm32_emit_u32(c, 0xEE000A10u | ((int_src & 0xF) << 12));
    int D = (fp_dst >> 4) & 1, Vd = fp_dst & 0xF;
    arm32_emit_u32(c, 0xEEB80BC0u | (D << 22) | (Vd << 12));
}

/* VFP FCPYD: fcpyd dD, dN (move) */
static void aarch32_emit_fp_mov(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    int D = (dst >> 4) & 1, Vd = dst & 0xF;
    int M = (src >> 4) & 1, Vm = src & 0xF;
    arm32_emit_u32(c, 0xEEB00B40u | (D << 22) | (Vd << 12) | (M << 5) | Vm);
}

/* =========================================================================
 * NEON 128-bit Vector Load/Store (Phase 2)
 * ========================================================================= */

/* NEON VLD1.64 {qD}, [Rn, #offset] - 128-bit vector load */
static void aarch32_emit_vload(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* NEON VLD1.64 encoding: 0xF9... need to verify */
    /* For now, use FLDD to load into d-register pair */
    /* q0 = d0:d1, q1 = d2:d3, etc. */
    int d_reg = dst_reg * 2;  /* qN maps to d(2N):d(2N+1) */
    aarch32_emit_load_fp(c, d_reg, base_reg, offset);
    aarch32_emit_load_fp(c, d_reg + 1, base_reg, offset + 8);
}

/* NEON VST1.64 {qD}, [Rn, #offset] - 128-bit vector store */
static void aarch32_emit_vstore(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    int d_reg = src_reg * 2;
    aarch32_emit_store_fp(c, d_reg, base_reg, offset);
    aarch32_emit_store_fp(c, d_reg + 1, base_reg, offset + 8);
}

/* =========================================================================
 * 64-bit Integer Multiply via UMULL/SMULL (Phase 2)
 * ========================================================================= */

/* UMULL: unsigned 64-bit multiply (lo,hi) = a * b */
static void aarch32_emit_mul64(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    /* Use lr (r14) as dedicated cross-term scratch register.
     * lr is caller-saved, non-allocatable by regalloc, and never used as a spill scratch pair. */
    /* 1. lr = a_lo * b_hi */
    aarch32_emit_mul(c, 14, a_lo, b_hi);
    /* 2. lr = lr + a_hi * b_lo (MLA lr, a_hi, b_lo, lr) */
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | 0x00200090u | (14 << 16) | (14 << 12) | ((b_lo & 0xF) << 8) | (a_hi & 0xF));
    /* 3. (dlo, dhi) = a_lo * b_lo */
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_UMULL(dhi, dlo, a_lo, b_lo));
    /* 4. dhi += cross terms */
    aarch32_emit_add(c, dhi, dhi, 14);
}

/* =========================================================================
 * 64-bit Integer Pair Operations (GPR64) for AArch32
 * =========================================================================
 * A 64-bit value lives in a (lo,hi) pair of 32-bit GPRs.
 * lo = lower 32 bits, hi = upper 32 bits.
 */

/* 64-bit ADD: d = a + b (with carry) */
static void aarch32_emit_add64(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_ADD) | ARM_S_BIT |
                   ARM_RD(dlo) | ARM_RN(alo) | ARM_RM(blo));  /* ADDS dlo, alo, blo */
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_ADC) |
                   ARM_RD(dhi) | ARM_RN(ahi) | ARM_RM(bhi));  /* ADC dhi, ahi, bhi */
}

/* 64-bit SUB: d = a - b (with borrow) */
static void aarch32_emit_sub64(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_SUB) | ARM_S_BIT |
                   ARM_RD(dlo) | ARM_RN(alo) | ARM_RM(blo));  /* SUBS dlo, alo, blo */
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_SBC) |
                   ARM_RD(dhi) | ARM_RN(ahi) | ARM_RM(bhi));  /* SBC dhi, ahi, bhi */
}

/* 64-bit AND */
static void aarch32_emit_and64(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_AND) |
                   ARM_RD(dlo) | ARM_RN(alo) | ARM_RM(blo));
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_AND) |
                   ARM_RD(dhi) | ARM_RN(ahi) | ARM_RM(bhi));
}

/* 64-bit OR */
static void aarch32_emit_or64(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_ORR) |
                   ARM_RD(dlo) | ARM_RN(alo) | ARM_RM(blo));
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_ORR) |
                   ARM_RD(dhi) | ARM_RN(ahi) | ARM_RM(bhi));
}

/* 64-bit XOR */
static void aarch32_emit_xor64(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_EOR) |
                   ARM_RD(dlo) | ARM_RN(alo) | ARM_RM(blo));
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_EOR) |
                   ARM_RD(dhi) | ARM_RN(ahi) | ARM_RM(bhi));
}

/* 64-bit NOT */
static void aarch32_emit_not64(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_MVN) |
                   ARM_RD(dlo) | ARM_RM(slo));
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_MVN) |
                   ARM_RD(dhi) | ARM_RM(shi));
}

/* 64-bit NEG (two's complement) */
static void aarch32_emit_neg64(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_RSB) | ARM_S_BIT |
                   ARM_RD(dlo) | ARM_RN(slo) | 0);  /* RSBS dlo, slo, #0 */
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_OPCODE(ARM_OP_RSC) |
                   ARM_RD(dhi) | ARM_RN(shi) | 0);  /* RSC dhi, shi, #0 */
}

static void aarch32_emit_64shift_call(ZeroCCCompiler *c, const char *fn,
                                      int dlo, int dhi, int slo, int shi, int cnt) {
    aarch32_emit_push_reg(c, cnt);
    aarch32_emit_push_reg(c, shi);
    aarch32_emit_push_reg(c, slo);
    aarch32_emit_pop_reg(c, 0);  /* r0 = slo */
    aarch32_emit_pop_reg(c, 1);  /* r1 = shi */
    aarch32_emit_pop_reg(c, 2);  /* r2 = cnt */
    aarch32_emit_call(c, fn);
    if (dlo == 1) {
        if (dhi != 1) aarch32_emit_mov_reg(c, dhi, 1);
        if (dlo != 0) aarch32_emit_mov_reg(c, dlo, 0);
    } else {
        if (dlo != 0) aarch32_emit_mov_reg(c, dlo, 0);
        if (dhi != 1) aarch32_emit_mov_reg(c, dhi, 1);
    }
}

/* 64-bit SHL (left shift) */
static void aarch32_emit_shl64(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, int cnt) {
    aarch32_emit_64shift_call(c, "zc_shl64", dlo, dhi, slo, shi, cnt);
}

/* 64-bit SHR (logical right shift) */
static void aarch32_emit_shr64(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, int cnt) {
    aarch32_emit_64shift_call(c, "zc_shr64", dlo, dhi, slo, shi, cnt);
}

/* 64-bit SAR (arithmetic right shift) */
static void aarch32_emit_sar64(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, int cnt) {
    aarch32_emit_64shift_call(c, "zc_sar64", dlo, dhi, slo, shi, cnt);
}

/* 64-bit CMP */
static void aarch32_emit_cmp64(ZeroCCCompiler *c, int a_lo, int a_hi, int b_lo, int b_hi) {
    c->lowering.cmp_is_64 = 1;
    c->lowering.cmp64_s1_lo = a_lo; c->lowering.cmp64_s1_hi = a_hi;
    c->lowering.cmp64_s2_lo = b_lo; c->lowering.cmp64_s2_hi = b_hi;
}

/* 64-bit MOV */
static void aarch32_emit_mov64(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi) {
    aarch32_emit_mov_reg(c, dlo, slo);
    aarch32_emit_mov_reg(c, dhi, shi);
}

/* 64-bit LOAD */
static void aarch32_emit_load64(ZeroCCCompiler *c, int dlo, int dhi, int base, int32_t offset) {
    if (dlo == base) {
        aarch32_emit_load(c, dhi, base, offset + 4);
        aarch32_emit_load(c, dlo, base, offset);
    } else {
        aarch32_emit_load(c, dlo, base, offset);
        aarch32_emit_load(c, dhi, base, offset + 4);
    }
}

/* 64-bit STORE */
static void aarch32_emit_store64(ZeroCCCompiler *c, int slo, int shi, int base, int32_t offset) {
    aarch32_emit_store(c, slo, base, offset);
    aarch32_emit_store(c, shi, base, offset + 4);
}

/* 64-bit ZEXT32_64 (zero-extend 32 to 64) */
static void aarch32_emit_zext32_64(ZeroCCCompiler *c, int dlo, int dhi, int src) {
    aarch32_emit_mov_reg(c, dlo, src);
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) |
                   ARM_RD(dhi) | 0);
}

/* 64-bit SEXT32_64 (sign-extend 32 to 64) */
static void aarch32_emit_sext32_64(ZeroCCCompiler *c, int dlo, int dhi, int src) {
    if (dhi == src) {
        aarch32_emit_mov_reg(c, dlo, src);
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | 0xE1A00000u | ARM_RD(dhi & 0xF) |
                       ARM_SHIFT_ASR(31) | ARM_RM(dlo & 0xF));  /* ASR dhi, dlo, #31 */
    } else {
        arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | 0xE1A00000u | ARM_RD(dhi & 0xF) |
                       ARM_SHIFT_ASR(31) | ARM_RM(src & 0xF));  /* ASR dhi, src, #31 */
        aarch32_emit_mov_reg(c, dlo, src);
    }
}

/* 64-bit TRUNC64_32 */
static void aarch32_emit_trunc64_32(ZeroCCCompiler *c, int dst, int slo, int shi) {
    (void)shi;
    aarch32_emit_mov_reg(c, dst, slo);
}

/* 64-bit MOV_IMM */
static void aarch32_emit_mov64_imm(ZeroCCCompiler *c, int dlo, int dhi, int64_t imm) {
    aarch32_emit_mov_imm(c, dlo, (int32_t)(imm & 0xFFFFFFFFLL));
    aarch32_emit_mov_imm(c, dhi, (int32_t)((uint64_t)imm >> 32));
}

/* =========================================================================
 * 64-bit Integer Divide/Modulo via zero_rt helpers (Phase 2)
 * ========================================================================= */

static void aarch32_emit_64div_call(ZeroCCCompiler *c, const char *fn,
                                    int dlo, int dhi,
                                    int a_lo, int a_hi, int b_lo, int b_hi) {
    aarch32_emit_push_reg(c, b_hi);
    aarch32_emit_push_reg(c, b_lo);
    aarch32_emit_push_reg(c, a_hi);
    aarch32_emit_push_reg(c, a_lo);
    aarch32_emit_pop_reg(c, 0);  /* r0 */
    aarch32_emit_pop_reg(c, 1);  /* r1 */
    aarch32_emit_pop_reg(c, 2);  /* r2 */
    aarch32_emit_pop_reg(c, 3);  /* r3 */
    aarch32_emit_call(c, fn);
    if (dlo == 1) {
        if (dhi != 1) aarch32_emit_mov_reg(c, dhi, 1);
        if (dlo != 0) aarch32_emit_mov_reg(c, dlo, 0);
    } else {
        if (dlo != 0) aarch32_emit_mov_reg(c, dlo, 0);
        if (dhi != 1) aarch32_emit_mov_reg(c, dhi, 1);
    }
}

static void aarch32_emit_div64(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    aarch32_emit_64div_call(c, "zc_sdiv64", dlo, dhi, a_lo, a_hi, b_lo, b_hi);
}

static void aarch32_emit_mod64(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    aarch32_emit_64div_call(c, "zc_smod64", dlo, dhi, a_lo, a_hi, b_lo, b_hi);
}

static void aarch32_emit_udiv64(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    aarch32_emit_64div_call(c, "zc_udiv64", dlo, dhi, a_lo, a_hi, b_lo, b_hi);
}

static void aarch32_emit_umod64(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    aarch32_emit_64div_call(c, "zc_umod64", dlo, dhi, a_lo, a_hi, b_lo, b_hi);
}

/* =========================================================================
 * Parameter spilling / function setup
 * ========================================================================= */
static void aarch32_emit_param_save_int(ZeroCCCompiler *c, int reg_idx, int32_t offset) {
    if (reg_idx < 0 || reg_idx >= 4) return;
    int reg = aarch32_int_arg_regs[reg_idx];
    aarch32_emit_store(c, reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -offset));
}
static void aarch32_emit_param_save_fp(ZeroCCCompiler *c, int reg_idx, int32_t offset, int is_single) {
    if (reg_idx < 0 || reg_idx >= 8) return;
    int reg = aarch32_fp_arg_regs[reg_idx];
    aarch32_emit_store_fp(c, reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -offset));
    (void)is_single;  /* AAPCS VFP variant uses d0-d7 for both float/double */
}
static void aarch32_emit_variadic_spill(ZeroCCCompiler *c, int param_count, int32_t base_offset) {
    (void)param_count;
    int base = zero_cc_frame_base_reg(c);
    int gp_regs = BE(c)->conv->int_arg_reg_count;
    for (int i = 0; i < gp_regs; i++) {
        int r = BE(c)->conv->int_arg_regs[i];
        aarch32_emit_store(c, r, base, -(base_offset + 16 - (int32_t)i * 4));
    }
}
static void aarch32_emit_variadic_al_setup(ZeroCCCompiler *c, int num_fp_args) {
    (void)c; (void)num_fp_args;
}

/* =========================================================================
 * Syscall / Clone (Linux ARM 32-bit syscall numbers)
 * ========================================================================= */
static void aarch32_emit_syscall(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    static const int syscall_regs[7] = {7, 0, 1, 2, 3, 4, 5};
    int n = (arg_count < 7) ? arg_count : 7;
    int32_t spill_base_offset = c->regalloc.spill_base_offset;

    int save_regs[4];
    int n_save = 0;
    save_regs[n_save++] = 7;
    if (n >= 6) save_regs[n_save++] = 4;
    if (n >= 7) save_regs[n_save++] = 5;

    for (int r = 0; r < n_save; r++) {
        aarch32_emit_push_reg(c, save_regs[r]);
    }
    int32_t extra_sp = n_save * 4;

    /* Pass 1: resolve each source (spill reloads into scratch_reg1 = r12),
     * push the ones that need a move. */
    int src_regs[8] = {0};
    for (int a = 0; a < n; a++) {
        int temp = arg_temps[a];
        int src_reg;
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            aarch32_emit_load(c, 12 /* r12 */, zero_cc_frame_base_reg(c),
                              zero_cc_frame_disp_at(c, -off, extra_sp));
            src_reg = 12;
        } else {
            src_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        }
        src_regs[a] = src_reg;
        if (src_reg != syscall_regs[a]) {
            aarch32_emit_push_reg(c, src_reg);
            extra_sp += 4;
        }
    }
    /* Pass 2: pop into the syscall registers (reverse order - LIFO). */
    for (int a = n - 1; a >= 0; a--) {
        if (src_regs[a] == syscall_regs[a]) continue;
        aarch32_emit_pop_reg(c, syscall_regs[a]);
        extra_sp -= 4;
    }

    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | 0xEF000000u);  /* SVC #0 */

    /* Restore callee-saved registers (reverse order - LIFO). */
    for (int r = n_save - 1; r >= 0; r--) {
        aarch32_emit_pop_reg(c, save_regs[r]);
    }
}
static void aarch32_emit_clone(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    aarch32_emit_syscall(c, arg_count, arg_temps);
}

static void aarch32_emit_cmp_zero(ZeroCCCompiler *c, int src_reg) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_CMP) |
                   ARM_S_BIT | ARM_RN(src_reg) | 0);
}
static void aarch32_emit_lnot(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    aarch32_emit_cmp_zero(c, src_reg);
    arm32_emit_u32(c, ARM_COND(ARM_CC_EQ) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) |
                   ARM_RD(dst_reg) | 1);
    arm32_emit_u32(c, ARM_COND(ARM_CC_NE) | ARM_DP_I_IMM | ARM_OPCODE(ARM_OP_MOV) |
                   ARM_RD(dst_reg) | 0);
}

/* =========================================================================
 * Atomic operations (placeholder)
 * ========================================================================= */
static void aarch32_emit_atomic_xadd(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) { (void)c; (void)dst_reg; (void)addr_reg; (void)val_reg; (void)width; }
static void aarch32_emit_atomic_or(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) { (void)c; (void)dst_reg; (void)addr_reg; (void)val_reg; (void)width; }
static void aarch32_emit_atomic_and(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) { (void)c; (void)dst_reg; (void)addr_reg; (void)val_reg; (void)width; }
static void aarch32_emit_atomic_xor(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) { (void)c; (void)dst_reg; (void)addr_reg; (void)val_reg; (void)width; }
static void aarch32_emit_atomic_cmpxchg(ZeroCCCompiler *c, int dst_reg, int addr_reg, int expected_reg, int desired_reg, int width) { (void)c; (void)dst_reg; (void)addr_reg; (void)expected_reg; (void)desired_reg; (void)width; }
static void aarch32_emit_atomic_cmpxchg_bool(ZeroCCCompiler *c, int dst_reg, int addr_reg, int expected_reg, int desired_reg, int width) { (void)c; (void)dst_reg; (void)addr_reg; (void)expected_reg; (void)desired_reg; (void)width; }
static void aarch32_emit_atomic_xchg(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) { (void)c; (void)dst_reg; (void)addr_reg; (void)val_reg; (void)width; }
static void aarch32_emit_mfence(ZeroCCCompiler *c) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | 0xF57FF05Fu);  /* DMB ISH */
}

/* =========================================================================
 * Trap / Overflow checks (zero_san)
 * ========================================================================= */
static void aarch32_emit_trap(ZeroCCCompiler *c) {
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | 0xE7F000F0u);  /* UDF #0 */
}
static void aarch32_emit_trap_asm(ZeroCCCompiler *c, FILE *out) {
    (void)c;
    fprintf(out, "\tudf #0\n");
}

static void aarch32_emit_ovf_trap(ZeroCCCompiler *c, int res, int a, int b, int width, int op) {
    int t = BE(c)->scratch_reg1;
    if (op == ZC_OVF_OP_ADD || op == ZC_OVF_OP_SUB) {
        aarch32_emit_xor(c, t, res, a);
        int t2 = BE(c)->scratch_reg2;
        aarch32_emit_xor(c, t2, res, b);
        aarch32_emit_and(c, t, t, t2);
        if (width == 4) {
            aarch32_emit_cmp_zero(c, t);
            size_t br = c->text_section.length;
            arm32_emit_u32(c, ARM_COND(ARM_CC_GE) | 0x0A000000u);
            aarch32_emit_trap(c);
            aarch32_patch_jmp(c, br, c->text_section.length);
        }
    }
    if (op == ZC_OVF_OP_DIV_ZERO || op == ZC_OVF_OP_REM_ZERO) {
        aarch32_emit_cmp_zero(c, res);
        size_t br = c->text_section.length;
        arm32_emit_u32(c, ARM_COND(ARM_CC_NE) | 0x0A000000u);
        aarch32_emit_trap(c);
        aarch32_patch_jmp(c, br, c->text_section.length);
    }
    if (op == ZC_OVF_OP_SHIFT_COUNT) {
        int t = BE(c)->scratch_reg1;
        aarch32_emit_mov_imm(c, t, width * 8);
        aarch32_emit_cmp(c, res, t);  /* count >= width*8 */
        size_t br1 = c->text_section.length;
        arm32_emit_u32(c, ARM_COND(ARM_CC_CC) | 0x0A000000u);
        aarch32_emit_trap(c);
        aarch32_patch_jmp(c, br1, c->text_section.length);
    }
}
static void aarch32_emit_ovf_trap_asm(ZeroCCCompiler *c, int res, int a, int b, int width, int op, FILE *out) {
    (void)c; (void)res; (void)a; (void)b; (void)width; (void)op; (void)out;
}

static void aarch32_emit_push_reg(ZeroCCCompiler *c, int reg) {
    /* str reg, [sp, #-4]! -> 0xE52D0004 | (reg << 12) */
    arm32_emit_u32(c, 0xE52D0004u | ((uint32_t)(reg & 0xF) << 12));
}

static void aarch32_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    /* ldr reg, [sp], #4 -> 0xE49D0004 | (reg << 12) */
    arm32_emit_u32(c, 0xE49D0004u | ((uint32_t)(reg & 0xF) << 12));
}

static void aarch32_emit_add_imm(ZeroCCCompiler *c, int dst, int16_t offset) {
    aarch32_emit_lea(c, dst, dst, offset);
}

static void aarch32_emit_sub_imm(ZeroCCCompiler *c, int dst, int16_t offset) {
    aarch32_emit_lea(c, dst, dst, -(int32_t)offset);
}

static void aarch32_emit_inc(ZeroCCCompiler *c, int dst) {
    aarch32_emit_lea(c, dst, dst, 1);
}

static void aarch32_emit_dec(ZeroCCCompiler *c, int dst) {
    aarch32_emit_lea(c, dst, dst, -1);
}

/* =========================================================================
 * Const / Stack adjust / Stack align wrappers for function pointer compatibility
 * ========================================================================= */
static void aarch32_emit_const(ZeroCCCompiler *c, int64_t value) {
    (void)c; (void)value;
}

static void aarch32_emit_stack_adjust(ZeroCCCompiler *c, int32_t delta) {
    aarch32_emit_lea(c, 13, 13, delta);  /* add sp, sp, delta */
}

static void aarch32_emit_stack_align(ZeroCCCompiler *c, uint8_t alignment) {
    (void)c; (void)alignment;
}

/* =========================================================================
 * ASM emission functions (text output) - Phase 1 AOT
 * ========================================================================= */
static const char *aarch32_get_hw_reg_name(int phys_reg);
static void aarch32_emit_mov_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out);
static void aarch32_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out);
static void aarch32_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out);
static void aarch32_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out);

static void aarch32_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    ZeroCCFunction *cur_fn = (c->function_count > 0 && c->lowering.current_func_idx < c->function_count)
                             ? &c->functions[c->lowering.current_func_idx] : NULL;
    bool is_isr = cur_fn && cur_fn->is_interrupt;
    if (c->lowering.is_frameless && !is_isr) return;
    if (is_isr) {
        fprintf(out, "\tpush\t{r0, r1, r2, r3, r12}\n");
        if (c->lowering.is_frameless) return;
    }
    if (c->lowering.frame_align > 16) {
        size_t frame = (size_t)c->lowering.frame_delta;
        uint32_t N = (uint32_t)c->lowering.frame_align;
        fprintf(out, "\tpush\t{r11, lr}\n");
        fprintf(out, "\tadd\tr11, sp, #0\n");
        uint32_t enc = arm32_encode_imm((int64_t)(N - 1));
        if (enc != 0xFFFFFFFFu) {
            fprintf(out, "\tbic\tsp, sp, #%u\n", N - 1);
        } else {
            uint32_t mask = (uint32_t)(~((int64_t)N - 1));
            fprintf(out, "\tmovw\tr12, #%u\n", mask & 0xFFFFu);
            fprintf(out, "\tmovt\tr12, #%u\n", (mask >> 16) & 0xFFFFu);
            fprintf(out, "\tand\tsp, sp, r12\n");
        }
        if (frame > 0) {
            if (arm32_encode_imm((int64_t)frame) != 0xFFFFFFFFu) {
                fprintf(out, "\tsub\tsp, sp, #%zu\n", frame);
            } else {
                aarch32_emit_mov_imm_asm(c, 12, (int64_t)frame, out);
                fprintf(out, "\tsub\tsp, sp, r12\n");
            }
        }
    } else {
        size_t sz = (stack_size + 7) & ~7;
        fprintf(out, "\tpush\t{r11, lr}\n");
        fprintf(out, "\tadd\tr11, sp, #0\n");
        if (stack_size > 0) {
            if (arm32_encode_imm((int64_t)sz) != 0xFFFFFFFFu) {
                fprintf(out, "\tsub\tsp, sp, #%zu\n", sz);
            } else {
                aarch32_emit_mov_imm_asm(c, 12, (int64_t)sz, out);
                fprintf(out, "\tsub\tsp, sp, r12\n");
            }
        }
    }
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
    if (mask && BE(c)->conv && BE(c)->conv->callee_saved_gpr) {
        const int *callee_saved = BE(c)->conv->callee_saved_gpr;
        const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
        int count = BE(c)->conv->callee_saved_gpr_count;
        for (int i = 0; i < count; i++) {
            int reg = callee_saved[i];
            if (reg == 11) continue;
            if (!((mask >> reg) & 1u)) continue;
            int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
            aarch32_emit_store_asm(c, reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot), out);
        }
    }
}
static void aarch32_emit_epilogue_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    ZeroCCFunction *cur_fn = (c->function_count > 0 && c->lowering.current_func_idx < c->function_count)
                             ? &c->functions[c->lowering.current_func_idx] : NULL;
    bool is_isr = cur_fn && cur_fn->is_interrupt;
    if (c->lowering.is_frameless && !is_isr) {
        fprintf(out, "\tmov\tpc, lr\n");
        return;
    }
    if (!c->lowering.is_frameless) {
        uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
        if (mask && BE(c)->conv && BE(c)->conv->callee_saved_gpr) {
            const int *callee_saved = BE(c)->conv->callee_saved_gpr;
            const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
            int count = BE(c)->conv->callee_saved_gpr_count;
            for (int i = 0; i < count; i++) {
                int reg = callee_saved[i];
                if (reg == 11) continue;
                if (!((mask >> reg) & 1u)) continue;
                int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
                aarch32_emit_load_asm(c, reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot), out);
            }
        }
        fprintf(out, "\tmov\tsp, r11\n");
        fprintf(out, "\tpop\t{r11, lr}\n");
    }
    if (is_isr) {
        fprintf(out, "\tpop\t{r0, r1, r2, r3, r12}\n");
        if (cur_fn->interrupt_mode <= 1) {
            fprintf(out, "\tsubs\tpc, lr, #4\n");
        } else {
            fprintf(out, "\tmovs\tpc, lr\n");
        }
        return;
    }
    fprintf(out, "\tmov\tpc, lr\n");
}
static void aarch32_emit_stack_guard_init_asm(ZeroCCCompiler *c, int32_t slot_off, FILE *out) {
    (void)c;
    fprintf(out, "\tldr\tr12, =__stack_chk_guard\n");
    fprintf(out, "\tstr\tr12, [r11, #-%d]\n", slot_off);
}
static void aarch32_emit_stack_guard_check_asm(ZeroCCCompiler *c, int32_t slot_off, FILE *out) {
    (void)c;
    fprintf(out, "\tldr\tr12, [r11, #-%d]\n", slot_off);
    fprintf(out, "\tldr\tr12, [r12]\n");
    fprintf(out, "\tcmp\tr12, r12\n");
    fprintf(out, "\tbne\t__stack_chk_fail\n");
}
static const char *aarch32_get_hw_reg_name(int phys_reg) {
    static const char *names[16] = {"r0","r1","r2","r3","r4","r5","r6","r7",
                                     "r8","r9","r10","r11","r12","sp","lr","pc"};
    if (phys_reg >= 0 && phys_reg < 16) return names[phys_reg];
    return "??";
}
static const char *aarch32_get_vec_reg_name(int vec_reg_idx) {
    static const char *names[16] = {"q0","q1","q2","q3","q4","q5","q6","q7",
                                     "q8","q9","q10","q11","q12","q13","q14","q15"};
    if (vec_reg_idx >= 0 && vec_reg_idx < 16) return names[vec_reg_idx];
    return "??";
}

/* Basic asm emitters for Phase 1 */
static void aarch32_emit_mov_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    (void)c;
    const char *rname = aarch32_get_hw_reg_name(dst);
    uint32_t v = (uint32_t)imm;
    if (arm32_encode_imm(imm) != 0xFFFFFFFFu) {
        fprintf(out, "\tmov\t%s, #%lld\n", rname, (long long)(int32_t)v);
    } else if (imm < 0 && arm32_encode_imm(~imm) != 0xFFFFFFFFu) {
        fprintf(out, "\tmvn\t%s, #%lld\n", rname, (long long)(uint32_t)(~imm));
    } else {
        uint32_t lo16 = v & 0xFFFFu;
        uint32_t hi16 = (v >> 16) & 0xFFFFu;
        fprintf(out, "\tmovw\t%s, #%u\n", rname, lo16);
        if (hi16 != 0) {
            fprintf(out, "\tmovt\t%s, #%u\n", rname, hi16);
        }
    }
}
static void aarch32_emit_mov_reg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    if (dst != src) fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    if (offset >= -4095 && offset <= 4095) {
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), offset);
    } else {
        int scratch = (base != 12) ? 12 : 9;
        aarch32_emit_mov_imm_asm(c, scratch, offset, out);
        fprintf(out, "\tldr\t%s, [%s, %s]\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), aarch32_get_hw_reg_name(scratch));
    }
}
static void aarch32_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    if (offset >= -4095 && offset <= 4095) {
        fprintf(out, "\tstr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(base), offset);
    } else {
        int scratch = (base != 12 && src != 12) ? 12 : ((base != 9 && src != 9) ? 9 : 14);
        aarch32_emit_mov_imm_asm(c, scratch, offset, out);
        fprintf(out, "\tstr\t%s, [%s, %s]\n", aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(base), aarch32_get_hw_reg_name(scratch));
    }
}
static void aarch32_emit_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tadd\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tsub\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tmul\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    fprintf(out, "\t%s\t%s, %s, %s\n",
            c->lowering.div_is_unsigned ? "udiv" : "sdiv",
            aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_mod_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    int t = (src1 != 12 && src2 != 12) ? 12 : ((src1 != 9 && src2 != 9) ? 9 : 14);
    aarch32_emit_div_asm(c, t, src1, src2, out);
    aarch32_emit_mul_asm(c, t, t, src2, out);
    aarch32_emit_sub_asm(c, dst, src1, t, out);
}

/* 64-bit multiply/divide ASM emitters */
static void aarch32_emit_mul64_asm(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    (void)c;
    fprintf(out, "\tmul\tlr, %s, %s\n", aarch32_get_hw_reg_name(a_lo), aarch32_get_hw_reg_name(b_hi));
    fprintf(out, "\tmla\tlr, %s, %s, lr\n", aarch32_get_hw_reg_name(a_hi), aarch32_get_hw_reg_name(b_lo));
    fprintf(out, "\tumull\t%s, %s, %s, %s\n",
            aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(dhi),
            aarch32_get_hw_reg_name(a_lo), aarch32_get_hw_reg_name(b_lo));
    fprintf(out, "\tadd\t%s, %s, lr\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(dhi));
}

static void aarch32_emit_64div_call_asm(ZeroCCCompiler *c, const char *fn,
                                        int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    (void)c;
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(b_hi));
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(b_lo));
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(a_hi));
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(a_lo));
    fprintf(out, "\tldr\tr0, [sp], #4\n");
    fprintf(out, "\tldr\tr1, [sp], #4\n");
    fprintf(out, "\tldr\tr2, [sp], #4\n");
    fprintf(out, "\tldr\tr3, [sp], #4\n");
    fprintf(out, "\tbl\t%s\n", fn);
    if (dlo == 1) {
        if (dhi != 1) fprintf(out, "\tmov\t%s, r1\n", aarch32_get_hw_reg_name(dhi));
        if (dlo != 0) fprintf(out, "\tmov\t%s, r0\n", aarch32_get_hw_reg_name(dlo));
    } else {
        if (dlo != 0) fprintf(out, "\tmov\t%s, r0\n", aarch32_get_hw_reg_name(dlo));
        if (dhi != 1) fprintf(out, "\tmov\t%s, r1\n", aarch32_get_hw_reg_name(dhi));
    }
}

static void aarch32_emit_div64_asm(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    aarch32_emit_64div_call_asm(c, "zc_sdiv64", dlo, dhi, a_lo, a_hi, b_lo, b_hi, out);
}
static void aarch32_emit_mod64_asm(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    aarch32_emit_64div_call_asm(c, "zc_smod64", dlo, dhi, a_lo, a_hi, b_lo, b_hi, out);
}
static void aarch32_emit_udiv64_asm(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    aarch32_emit_64div_call_asm(c, "zc_udiv64", dlo, dhi, a_lo, a_hi, b_lo, b_hi, out);
}
static void aarch32_emit_umod64_asm(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    aarch32_emit_64div_call_asm(c, "zc_umod64", dlo, dhi, a_lo, a_hi, b_lo, b_hi, out);
}

static void aarch32_emit_64shift_call_asm(ZeroCCCompiler *c, const char *fn,
                                          int dlo, int dhi, int slo, int shi, int cnt, FILE *out) {
    (void)c;
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(cnt));
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(shi));
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(slo));
    fprintf(out, "\tldr\tr0, [sp], #4\n");
    fprintf(out, "\tldr\tr1, [sp], #4\n");
    fprintf(out, "\tldr\tr2, [sp], #4\n");
    fprintf(out, "\tbl\t%s\n", fn);
    if (dlo == 1) {
        if (dhi != 1) fprintf(out, "\tmov\t%s, r1\n", aarch32_get_hw_reg_name(dhi));
        if (dlo != 0) fprintf(out, "\tmov\t%s, r0\n", aarch32_get_hw_reg_name(dlo));
    } else {
        if (dlo != 0) fprintf(out, "\tmov\t%s, r0\n", aarch32_get_hw_reg_name(dlo));
        if (dhi != 1) fprintf(out, "\tmov\t%s, r1\n", aarch32_get_hw_reg_name(dhi));
    }
}

static void aarch32_emit_shl64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, int cnt, FILE *out) {
    aarch32_emit_64shift_call_asm(c, "zc_shl64", dlo, dhi, slo, shi, cnt, out);
}
static void aarch32_emit_shr64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, int cnt, FILE *out) {
    aarch32_emit_64shift_call_asm(c, "zc_shr64", dlo, dhi, slo, shi, cnt, out);
}
static void aarch32_emit_sar64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, int cnt, FILE *out) {
    aarch32_emit_64shift_call_asm(c, "zc_sar64", dlo, dhi, slo, shi, cnt, out);
}

static void aarch32_emit_add64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo1, int shi1, int slo2, int shi2, FILE *out) {
    (void)c;
    fprintf(out, "\tadds\t%s, %s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo1), aarch32_get_hw_reg_name(slo2));
    fprintf(out, "\tadc\t%s, %s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi1), aarch32_get_hw_reg_name(shi2));
}
static void aarch32_emit_sub64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo1, int shi1, int slo2, int shi2, FILE *out) {
    (void)c;
    fprintf(out, "\tsubs\t%s, %s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo1), aarch32_get_hw_reg_name(slo2));
    fprintf(out, "\tsbc\t%s, %s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi1), aarch32_get_hw_reg_name(shi2));
}
static void aarch32_emit_and64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo1, int shi1, int slo2, int shi2, FILE *out) {
    (void)c;
    fprintf(out, "\tand\t%s, %s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo1), aarch32_get_hw_reg_name(slo2));
    fprintf(out, "\tand\t%s, %s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi1), aarch32_get_hw_reg_name(shi2));
}
static void aarch32_emit_or64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo1, int shi1, int slo2, int shi2, FILE *out) {
    (void)c;
    fprintf(out, "\torr\t%s, %s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo1), aarch32_get_hw_reg_name(slo2));
    fprintf(out, "\torr\t%s, %s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi1), aarch32_get_hw_reg_name(shi2));
}
static void aarch32_emit_xor64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo1, int shi1, int slo2, int shi2, FILE *out) {
    (void)c;
    fprintf(out, "\teor\t%s, %s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo1), aarch32_get_hw_reg_name(slo2));
    fprintf(out, "\teor\t%s, %s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi1), aarch32_get_hw_reg_name(shi2));
}
static void aarch32_emit_not64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, FILE *out) {
    (void)c;
    fprintf(out, "\tmvn\t%s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo));
    fprintf(out, "\tmvn\t%s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi));
}
static void aarch32_emit_neg64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, FILE *out) {
    (void)c;
    fprintf(out, "\trsbs\t%s, %s, #0\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo));
    fprintf(out, "\trsc\t%s, %s, #0\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi));
}
static void aarch32_emit_cmp64_asm(ZeroCCCompiler *c, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    (void)out;
    c->lowering.cmp_is_64 = 1;
    c->lowering.cmp64_s1_lo = a_lo; c->lowering.cmp64_s1_hi = a_hi;
    c->lowering.cmp64_s2_lo = b_lo; c->lowering.cmp64_s2_hi = b_hi;
}
static void aarch32_emit_mov64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, FILE *out) {
    (void)c;
    if (dlo == shi) {
        fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi));
        fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo));
    } else {
        fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo));
        fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi));
    }
}
static void aarch32_emit_load64_asm(ZeroCCCompiler *c, int dlo, int dhi, int base, int32_t offset, FILE *out) {
    (void)c;
    if (dlo == base) {
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(base), offset + 4);
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(base), offset);
    } else {
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(base), offset);
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(base), offset + 4);
    }
}
static void aarch32_emit_store64_asm(ZeroCCCompiler *c, int slo, int shi, int base, int32_t offset, FILE *out) {
    (void)c;
    fprintf(out, "\tstr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(slo), aarch32_get_hw_reg_name(base), offset);
    fprintf(out, "\tstr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(shi), aarch32_get_hw_reg_name(base), offset + 4);
}
static void aarch32_emit_zext32_64_asm(ZeroCCCompiler *c, int dlo, int dhi, int src, FILE *out) {
    (void)c;
    if (dlo != src) fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(src));
    fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dhi));
}
static void aarch32_emit_sext32_64_asm(ZeroCCCompiler *c, int dlo, int dhi, int src, FILE *out) {
    (void)c;
    if (dlo != src) fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(src));
    fprintf(out, "\tasr\t%s, %s, #31\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_trunc64_32_asm(ZeroCCCompiler *c, int dst, int slo, int shi, FILE *out) {
    (void)c; (void)shi;
    if (dst != slo) fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(slo));
}
static void aarch32_emit_mov64_imm_asm(ZeroCCCompiler *c, int dlo, int dhi, int64_t val, FILE *out) {
    (void)c;
    uint32_t vlo = (uint32_t)val;
    uint32_t vhi = (uint32_t)(val >> 32);
    fprintf(out, "\tmovw\t%s, #%u\n", aarch32_get_hw_reg_name(dlo), vlo & 0xFFFF);
    if (vlo >> 16) fprintf(out, "\tmovt\t%s, #%u\n", aarch32_get_hw_reg_name(dlo), vlo >> 16);
    fprintf(out, "\tmovw\t%s, #%u\n", aarch32_get_hw_reg_name(dhi), vhi & 0xFFFF);
    if (vhi >> 16) fprintf(out, "\tmovt\t%s, #%u\n", aarch32_get_hw_reg_name(dhi), vhi >> 16);
}

static void aarch32_emit_and_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tand\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_or_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\torr\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_xor_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\teor\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_shl_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    (void)c;
    fprintf(out, "\tlsl\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(shift));
}
static void aarch32_emit_shr_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    (void)c;
    fprintf(out, "\tlsr\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(shift));
}
static void aarch32_emit_sar_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    (void)c;
    fprintf(out, "\tasr\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(shift));
}
static void aarch32_emit_extend_asm(ZeroCCCompiler *c, int dst, int src, int width, int sign, FILE *out) {
    (void)c;
    if (width == 1 && sign) fprintf(out, "\tsxtb\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
    else if (width == 1) fprintf(out, "\tuxtb\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
    else if (width == 2 && sign) fprintf(out, "\tsxth\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
    else if (width == 2) fprintf(out, "\tuxth\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tcmp\t%s, %s\n", aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_64_pred_asm(ZeroCCCompiler *c, int dst, int cond, FILE *out) {
    int alo = c->lowering.cmp64_s1_lo, ahi = c->lowering.cmp64_s1_hi;
    int blo = c->lowering.cmp64_s2_lo, bhi = c->lowering.cmp64_s2_hi;

    if (cond == 4) { /* CC_EQ */
        fprintf(out, "\tcmp\t%s, %s\n", aarch32_get_hw_reg_name(ahi), aarch32_get_hw_reg_name(bhi));
        fprintf(out, "\tcmpeq\t%s, %s\n", aarch32_get_hw_reg_name(alo), aarch32_get_hw_reg_name(blo));
        fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
        fprintf(out, "\tmoveq\t%s, #1\n", aarch32_get_hw_reg_name(dst));
        return;
    }
    if (cond == 5) { /* CC_NE */
        fprintf(out, "\tcmp\t%s, %s\n", aarch32_get_hw_reg_name(ahi), aarch32_get_hw_reg_name(bhi));
        fprintf(out, "\tcmpeq\t%s, %s\n", aarch32_get_hw_reg_name(alo), aarch32_get_hw_reg_name(blo));
        fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
        fprintf(out, "\tmovne\t%s, #1\n", aarch32_get_hw_reg_name(dst));
        return;
    }
    if (cond >= 6 && cond <= 9) { /* Unsigned */
        fprintf(out, "\tcmp\t%s, %s\n", aarch32_get_hw_reg_name(ahi), aarch32_get_hw_reg_name(bhi));
        fprintf(out, "\tcmpeq\t%s, %s\n", aarch32_get_hw_reg_name(alo), aarch32_get_hw_reg_name(blo));
        fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
        const char *cc_str = (cond == 6) ? "cc" : (cond == 7) ? "ls" : (cond == 8) ? "hi" : "cs";
        fprintf(out, "\tmov%s\t%s, #1\n", cc_str, aarch32_get_hw_reg_name(dst));
        return;
    }
    int s_alo = alo, s_ahi = ahi, s_blo = blo, s_bhi = bhi;
    const char *check_cc = "lt";
    if (cond == 0) {
        check_cc = "lt";
    } else if (cond == 3) {
        check_cc = "ge";
    } else if (cond == 2) {
        s_alo = blo; s_ahi = bhi; s_blo = alo; s_bhi = ahi;
        check_cc = "lt";
    } else if (cond == 1) {
        s_alo = blo; s_ahi = bhi; s_blo = alo; s_bhi = ahi;
        check_cc = "ge";
    }
    fprintf(out, "\tsubs\tr12, %s, %s\n", aarch32_get_hw_reg_name(s_alo), aarch32_get_hw_reg_name(s_blo));
    fprintf(out, "\tsbcs\tr12, %s, %s\n", aarch32_get_hw_reg_name(s_ahi), aarch32_get_hw_reg_name(s_bhi));
    fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
    fprintf(out, "\tmov%s\t%s, #1\n", check_cc, aarch32_get_hw_reg_name(dst));
}

static void aarch32_emit_cond_set_asm(ZeroCCCompiler *c, int dst, int cond, FILE *out) {
    if (c->lowering.cmp_is_64) {
        c->lowering.cmp_is_64 = 0;
        aarch32_emit_64_pred_asm(c, dst, cond, out);
        return;
    }
    static const char *arm_cc_str[14] = {
        "lt", "le", "gt", "ge", "eq", "ne",
        "cc", "ls", "hi", "cs", "eq", "ne",
        "vs", "vc"
    };
    const char *cc = (cond >= 0 && cond < 14) ? arm_cc_str[cond] : "eq";
    fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
    fprintf(out, "\tmov%s\t%s, #1\n", cc, aarch32_get_hw_reg_name(dst));
}
static void aarch32_emit_neg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\trsb\t%s, %s, #0\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_add_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    (void)c;
    fprintf(out, "\tadd\t%s, %s, #%ld\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(dst), imm);
}
static void aarch32_emit_sub_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    (void)c;
    fprintf(out, "\tsub\t%s, %s, #%ld\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(dst), imm);
}
static void aarch32_emit_inc_asm(ZeroCCCompiler *c, int dst, FILE *out) {
    (void)c;
    fprintf(out, "\tadd\t%s, %s, #1\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(dst));
}
static void aarch32_emit_dec_asm(ZeroCCCompiler *c, int dst, FILE *out) {
    (void)c;
    fprintf(out, "\tsub\t%s, %s, #1\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(dst));
}
static void aarch32_emit_not_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tmvn\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    if (offset >= 0 && arm32_encode_imm((int64_t)offset) != 0xFFFFFFFFu) {
        fprintf(out, "\tadd\t%s, %s, #%d\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), offset);
    } else if (offset < 0 && arm32_encode_imm(-(int64_t)offset) != 0xFFFFFFFFu) {
        fprintf(out, "\tsub\t%s, %s, #%d\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), -offset);
    } else {
        int scratch = (base != 12) ? 12 : 9;
        aarch32_emit_mov_imm_asm(c, scratch, offset, out);
        fprintf(out, "\tadd\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), aarch32_get_hw_reg_name(scratch));
    }
}
static void aarch32_emit_deref_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tldr\t%s, [%s]\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_call_asm(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, int *stack_pos, FILE *out) {
    static const char *int_arg_regs[] = {"r0", "r1", "r2", "r3"};
    static const char *fp_arg_regs[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};

    int int_arg_idx = 0;
    int fp_arg_idx = 0;
    int indirect_temp = -1;
    int int_src_regs[8];
    int int_arg_temps[IR_CALL_MAX_ARGS];
    int total_int_count = 0;
    int32_t spill_base_offset = c->regalloc.spill_base_offset;

    for (int i = 0; i < arg_count; i++) {
        int temp = arg_temps[i];
        int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        int arg_is_fp = is_fp ? is_fp[i] : 0;

        if (arg_is_fp == 2) {
            indirect_temp = temp;
        } else if (arg_is_fp == 1 && (stack_pos ? stack_pos[i] : 255) >= 255 && fp_arg_idx < 8) {
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                fprintf(out, "\tfldd\t%s, [%s, #%d]\n", fp_arg_regs[fp_arg_idx],
                        BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                        (int)zero_cc_frame_disp(c, -off));
            } else if (phys_reg >= 0) {
                fprintf(out, "\tfcpyd\t%s, d%d\n", fp_arg_regs[fp_arg_idx], phys_reg);
            }
            fp_arg_idx++;
        } else if (arg_is_fp == 0) {
            int_arg_temps[total_int_count++] = temp;
            if (int_arg_idx < 4) {
                int_src_regs[int_arg_idx++] = phys_reg;
            }
        }
    }

    /* Save indirect callee into r12 (ip) before clobbering argument registers */
    if (!func_name && indirect_temp >= 0) {
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, indirect_temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            fprintf(out, "\tldr\tr12, [%s, #%d]\n",
                    BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                    (int)zero_cc_frame_disp(c, -off));
        } else {
            int r_callee = regalloc_get_reg_for_temp(&c->regalloc, indirect_temp);
            fprintf(out, "\tmov\tr12, %s\n", BE(c)->get_hw_reg_name(r_callee));
        }
    }

    /* Stack args (from stack_pos): integer args beyond r0..r3 AND FP args
     * beyond d0..d7, in original argument order.
     * In AArch32 AAPCS, stack arguments are 4-byte aligned, but overall SP
     * must be 8-byte aligned at the call boundary. */
    int n_stack = 0;
    int stack_pos_list[IR_CALL_MAX_ARGS];
    for (int i = 0; i < arg_count && i < IR_CALL_MAX_ARGS; i++) {
        int arg_pos = stack_pos ? stack_pos[i] : 255;
        if (arg_pos < 255 && arg_pos < IR_CALL_MAX_ARGS) {
            stack_pos_list[arg_pos] = i;
            if (arg_pos + 1 > n_stack) n_stack = arg_pos + 1;
        }
    }
    int legacy_n_stack = (!stack_pos && total_int_count > 4) ? total_int_count - 4 : 0;
    int stack_alloc = 0;
    if (stack_pos) {
        stack_alloc = ((n_stack * 4) + 7) & ~7;
        if (n_stack > 0) {
            fprintf(out, "\tsub\tsp, sp, #%d\n", stack_alloc);
            for (int pos = 0; pos < n_stack; pos++) {
                int i = stack_pos_list[pos];
                int arg_is_fp = is_fp ? is_fp[i] : 0;
                if (arg_is_fp == 1) {
                    int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[i]);
                    fprintf(out, "\tfstd\td%d, [sp, #%d]\n", phys_reg, pos * 4);
                } else {
                    int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, arg_temps[i]);
                    if (sp >= 0) {
                        int32_t off = spill_base_offset + (sp + 1) * 8;
                        fprintf(out, "\tldr\tr12, [%s, #%d]\n",
                                BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                                (int)zero_cc_frame_disp_at(c, -off, stack_alloc));
                        fprintf(out, "\tstr\tr12, [sp, #%d]\n", pos * 4);
                    } else {
                        int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[i]);
                        fprintf(out, "\tstr\t%s, [sp, #%d]\n", BE(c)->get_hw_reg_name(phys_reg), pos * 4);
                    }
                }
            }
        }
    } else {
        /* Legacy: int args beyond 4, in order */
        n_stack = legacy_n_stack;
        stack_alloc = ((n_stack * 4) + 7) & ~7;
        if (n_stack > 0) {
            fprintf(out, "\tsub\tsp, sp, #%d\n", stack_alloc);
            for (int a = 4; a < total_int_count; a++) {
                int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, int_arg_temps[a]);
                if (sp >= 0) {
                    int32_t off = spill_base_offset + (sp + 1) * 8;
                    fprintf(out, "\tldr\tr12, [%s, #%d]\n",
                            BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                            (int)zero_cc_frame_disp_at(c, -off, stack_alloc));
                    fprintf(out, "\tstr\tr12, [sp, #%d]\n", (a - 4) * 4);
                } else {
                    int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, int_arg_temps[a]);
                    fprintf(out, "\tstr\t%s, [sp, #%d]\n", BE(c)->get_hw_reg_name(phys_reg), (a - 4) * 4);
                }
            }
        }
    }

    /* Parallel move of integer arguments via stack so r0..r3 don't clobber each other */
    if (int_arg_idx > 0) {
        int alloc_bytes = ((int_arg_idx * 4) + 7) & ~7;
        fprintf(out, "\tsub\tsp, sp, #%d\n", alloc_bytes);
        for (int a = 0; a < int_arg_idx; a++) {
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, int_arg_temps[a]);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                fprintf(out, "\tldr\tr12, [%s, #%d]\n",
                        BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                        (int)zero_cc_frame_disp_at(c, -off, stack_alloc + alloc_bytes));
                fprintf(out, "\tstr\tr12, [sp, #%d]\n", a * 4);
            } else {
                fprintf(out, "\tstr\t%s, [sp, #%d]\n", BE(c)->get_hw_reg_name(int_src_regs[a]), a * 4);
            }
        }
        for (int a = 0; a < int_arg_idx; a++) {
            fprintf(out, "\tldr\t%s, [sp, #%d]\n", int_arg_regs[a], a * 4);
        }
        fprintf(out, "\tadd\tsp, sp, #%d\n", alloc_bytes);
    }

    if (!func_name && indirect_temp >= 0) {
        fprintf(out, "\tblx\tr12\n");
    } else if (func_name) {
        fprintf(out, "\tbl\t%s\n", func_name);
    }

    /* Clean up stack arguments after call */
    if (stack_alloc > 0) {
        fprintf(out, "\tadd\tsp, sp, #%d\n", stack_alloc);
    }
}
static void aarch32_emit_ret_asm(ZeroCCCompiler *c, FILE *out) {
    ZeroCCFunction *cur_fn = (c->function_count > 0 && c->lowering.current_func_idx < c->function_count)
                             ? &c->functions[c->lowering.current_func_idx] : NULL;
    if (cur_fn && cur_fn->is_interrupt) {
        if (cur_fn->interrupt_mode <= 1) {
            fprintf(out, "\tsubs\tpc, lr, #4\n");
        } else {
            fprintf(out, "\tmovs\tpc, lr\n");
        }
        return;
    }
    fprintf(out, "\tbx\tlr\n");
}
static void aarch32_emit_label_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "%s:\n", label);
}
static void aarch32_emit_jmp_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tb\t%s\n", label);
}
static void aarch32_emit_jeq_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tbeq\t%s\n", label);
}
static void aarch32_emit_jne_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tbne\t%s\n", label);
}
static void aarch32_emit_jlt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tblt\t%s\n", label);
}
static void aarch32_emit_jle_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tble\t%s\n", label);
}
static void aarch32_emit_jgt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tbgt\t%s\n", label);
}
static void aarch32_emit_jge_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tbge\t%s\n", label);
}
static void aarch32_emit_load_fp_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    if (offset >= -1020 && offset <= 1020 && (offset & 3) == 0) {
        fprintf(out, "\tfldd\td%d, [%s, #%d]\n", dst, aarch32_get_hw_reg_name(base), offset);
    } else {
        int scratch = (base != 12) ? 12 : 9;
        aarch32_emit_lea_asm(c, scratch, base, offset, out);
        fprintf(out, "\tfldd\td%d, [%s, #0]\n", dst, aarch32_get_hw_reg_name(scratch));
    }
}
static void aarch32_emit_store_fp_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    if (offset >= -1020 && offset <= 1020 && (offset & 3) == 0) {
        fprintf(out, "\tfstd\td%d, [%s, #%d]\n", src, aarch32_get_hw_reg_name(base), offset);
    } else {
        int scratch = (base != 12) ? 12 : 9;
        aarch32_emit_lea_asm(c, scratch, base, offset, out);
        fprintf(out, "\tfstd\td%d, [%s, #0]\n", src, aarch32_get_hw_reg_name(scratch));
    }
}
static void aarch32_emit_fp_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tfaddd\td%d, d%d, d%d\n", dst, src1, src2);
}
static void aarch32_emit_fp_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tfsubd\td%d, d%d, d%d\n", dst, src1, src2);
}
static void aarch32_emit_fp_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tfmuld\td%d, d%d, d%d\n", dst, src1, src2);
}
static void aarch32_emit_fp_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tfdivd\td%d, d%d, d%d\n", dst, src1, src2);
}
static void aarch32_emit_fp_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tfcmpd\td%d, d%d\n", src1, src2);
}
static void aarch32_emit_fp_mov_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tfcpyd\td%d, d%d\n", dst, src);
}
static void aarch32_emit_fp_cvtsf_asm(ZeroCCCompiler *c, int fp_dst, int int_src, FILE *out) {
    (void)c;
    fprintf(out, "\tsitos\td%d, %s\n", fp_dst, aarch32_get_hw_reg_name(int_src));
}
static void aarch32_emit_fp_cvtsi_asm(ZeroCCCompiler *c, int int_dst, int fp_src, FILE *out) {
    (void)c;
    fprintf(out, "\tftosizs\t%s, d%d\n", aarch32_get_hw_reg_name(int_dst), fp_src);
}
static void aarch32_emit_cvtss2sd_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tvcvt.f64.f32\td%d, s%d\n", dst, src * 2);
}
static void aarch32_emit_cvtsd2ss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tvcvt.f32.f64\ts%d, d%d\n", dst * 2, src);
}
static void aarch32_emit_vla_alloc_asm(ZeroCCCompiler *c, int dst, int size, FILE *out) {
    (void)c; (void)dst; (void)size; (void)out;
    fprintf(out, "\tsub\tsp, sp, %s\n", aarch32_get_hw_reg_name(size));
    fprintf(out, "\tmov\t%s, sp\n", aarch32_get_hw_reg_name(dst));
}
static void aarch32_emit_param_save_int_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, FILE *out) {
    aarch32_emit_store_asm(c, reg_idx, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -offset), out);
}
static void aarch32_emit_param_save_fp_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, int is_single, FILE *out) {
    (void)is_single;
    if (reg_idx >= 0 && reg_idx < 8) {
        aarch32_emit_store_fp_asm(c, reg_idx, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -offset), out);
    }
}
static void aarch32_emit_variadic_spill_asm(ZeroCCCompiler *c, int param_count, int32_t base_offset, FILE *out) {
    (void)param_count;
    int gp_regs = BE(c)->conv->int_arg_reg_count;
    for (int i = 0; i < gp_regs; i++) {
        int r = BE(c)->conv->int_arg_regs[i];
        aarch32_emit_store_asm(c, r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -(base_offset + 16 - i * 4)), out);
    }
}
static void aarch32_emit_syscall_asm(ZeroCCCompiler *c, int arg_count, const int *arg_temps, FILE *out) {
    static const char *syscall_reg_names[7] = {"r7", "r0", "r1", "r2", "r3", "r4", "r5"};
    int n = (arg_count < 7) ? arg_count : 7;
    int32_t spill_base_offset = c->regalloc.spill_base_offset;

    /* Save callee-saved registers that we will overwrite */
    int save_regs[4];
    int n_save = 0;
    save_regs[n_save++] = 7;
    if (n >= 6) save_regs[n_save++] = 4;
    if (n >= 7) save_regs[n_save++] = 5;
    int save_bytes = ((n_save * 4) + 7) & ~7;
    fprintf(out, "\tsub\tsp, sp, #%d\n", save_bytes);
    for (int r = 0; r < n_save; r++) {
        fprintf(out, "\tstr\t%s, [sp, #%d]\n", aarch32_get_hw_reg_name(save_regs[r]), r * 4);
    }

    /* Parallel move of the syscall arguments via stack */
    if (n > 0) {
        int alloc_bytes = ((n * 4) + 7) & ~7;
        fprintf(out, "\tsub\tsp, sp, #%d\n", alloc_bytes);
        for (int a = 0; a < n; a++) {
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, arg_temps[a]);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                fprintf(out, "\tldr\tr12, [%s, #%d]\n",
                        BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                        (int)zero_cc_frame_disp_at(c, -off, save_bytes + alloc_bytes));
                fprintf(out, "\tstr\tr12, [sp, #%d]\n", a * 4);
            } else {
                int r = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[a]);
                fprintf(out, "\tstr\t%s, [sp, #%d]\n", aarch32_get_hw_reg_name(r), a * 4);
            }
        }
        for (int a = 0; a < n; a++) {
            fprintf(out, "\tldr\t%s, [sp, #%d]\n", syscall_reg_names[a], a * 4);
        }
        fprintf(out, "\tadd\tsp, sp, #%d\n", alloc_bytes);
    }

    fprintf(out, "\tsvc\t#0\n");

    /* Restore callee-saved registers (r0 has return value, leave untouched) */
    for (int r = 0; r < n_save; r++) {
        fprintf(out, "\tldr\t%s, [sp, #%d]\n", aarch32_get_hw_reg_name(save_regs[r]), r * 4);
    }
    fprintf(out, "\tadd\tsp, sp, #%d\n", save_bytes);
}
static void aarch32_emit_clone_asm(ZeroCCCompiler *c, int arg_count, const int *arg_temps, FILE *out) {
    aarch32_emit_syscall_asm(c, arg_count, arg_temps, out);
}
static void aarch32_emit_inline_asm_asm(ZeroCCCompiler *c, size_t instr_index, FILE *out) {
    (void)c; (void)instr_index; (void)out;
}
static void aarch32_emit_inline_asm_mc(ZeroCCCompiler *c, size_t instr_index) {
    (void)c; (void)instr_index;
}
static void aarch32_emit_filescope_asm_mc(ZeroCCCompiler *c, const char *asm_str) {
    (void)c; (void)asm_str;
}

/* Vector asm - NEON 128-bit load/store */
static void aarch32_emit_vload_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    (void)c;
    fprintf(out, "\tvld1.64\t{q%d}, [%s, #%d]\n", dst, aarch32_get_hw_reg_name(base), offset);
}
static void aarch32_emit_vstore_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    (void)c;
    fprintf(out, "\tvst1.64\t{q%d}, [%s, #%d]\n", src, aarch32_get_hw_reg_name(base), offset);
}

static void aarch32_emit_load_sized_asm(ZeroCCCompiler *c, int phys_dst, int phys_addr, int size_bytes, FILE *out) {
    const char *dst_name = aarch32_get_hw_reg_name(phys_dst);
    const char *addr_name = aarch32_get_hw_reg_name(phys_addr);
    (void)c;
    if (size_bytes == 1) {
        fprintf(out, "\tldrb\t%s, [%s]\n", dst_name, addr_name);
    } else if (size_bytes == 2) {
        fprintf(out, "\tldrh\t%s, [%s]\n", dst_name, addr_name);
    } else {
        fprintf(out, "\tldr\t%s, [%s]\n", dst_name, addr_name);
    }
}

static void aarch32_emit_store_sized_asm(ZeroCCCompiler *c, int phys_val, int phys_addr, int size_bytes, FILE *out) {
    const char *val_name = aarch32_get_hw_reg_name(phys_val);
    const char *addr_name = aarch32_get_hw_reg_name(phys_addr);
    (void)c;
    if (size_bytes == 1) {
        fprintf(out, "\tstrb\t%s, [%s]\n", val_name, addr_name);
    } else if (size_bytes == 2) {
        fprintf(out, "\tstrh\t%s, [%s]\n", val_name, addr_name);
    } else {
        fprintf(out, "\tstr\t%s, [%s]\n", val_name, addr_name);
    }
}

static void aarch32_emit_test_zero_asm(ZeroCCCompiler *c, int phys_reg, FILE *out) {
    const char *name = aarch32_get_hw_reg_name(phys_reg);
    (void)c;
    fprintf(out, "\tcmp\t%s, #0\n", name);
}

static void aarch32_emit_cmp_regs_asm(ZeroCCCompiler *c, int phys_reg1, int phys_reg2, FILE *out) {
    const char *r1 = aarch32_get_hw_reg_name(phys_reg1);
    const char *r2 = aarch32_get_hw_reg_name(phys_reg2);
    (void)c;
    fprintf(out, "\tcmp\t%s, %s\n", r1, r2);
}

/* =========================================================================
 * Code offset getter
 * ========================================================================= */
static size_t aarch32_get_code_offset(ZeroCCCompiler *c) {
    return c->text_section.length;
}

/* =====================================================================
 * Trampoline stub for a capturing nested function on AArch32 (ARMv7-A)
 *
 * Calling convention:
 *   GPR units: r0..r3 (4 registers)
 *   FP units: d0..d7 (8 registers)
 *   Overflow units: shared stack stream [entry_sp + 4*s]
 *
 * Frame layout (r11 = entry_sp - 8, after `push {r11, lr}; mov r11, sp`):
 *   [r11 + 4]           saved lr
 *   [r11 + 0]           saved r11
 *   [r11 + 8 + 4*s]     entry stack unit s (the caller's stack args)
 *   [r11 - 4 - 4*g]     entry GPR arg saves (r0..r3)
 *   [r11 - 24 - 8*f]    entry FP arg saves (d0..d7, 8-byte aligned)
 *   [r11 - 88 - 4*s]    entry stack unit saves
 *
 * Scratch registers:
 *   r12 (IP): scratch temp
 *   r14 (LR): saved in frame, free to use as ctx base pointer
 * ===================================================================== */

void zero_cc_aarch32_emit_tramp_stub(ZeroCCCompiler *c, const ZeroCCFunction *nf) {
    if (c->isa != ISA_AARCH32) return;
    size_t C = nf->nested_captured_count;
    size_t U = nf->param_count - C;

    /* ---- pass 1: entry geometry of USER call's units ---- */
    bool ue_isfp[ZERO_CC_TRAMP_MAX_UNITS];
    int  ue_reg[ZERO_CC_TRAMP_MAX_UNITS];
    int  ue_stk[ZERO_CC_TRAMP_MAX_UNITS];
    int ue_n = 0, e_gpr = 0, e_fp = 0, e_shared = 0;
    for (size_t ui = 0; ui < U && ue_n < ZERO_CC_TRAMP_MAX_UNITS; ui++) {
        const ZeroCCType *t = &nf->param_types[C + ui];
        int slots = zero_cc_tramp_unit_slots_isa(t, c->isa);
        bool isfp = zero_cc_tramp_unit_is_fp(t);
        for (int w = 0; w < slots; w++) {
            int sidx = isfp ? e_fp : e_gpr;
            ue_isfp[ue_n] = isfp;
            if (isfp) {
                if (sidx < 8) { ue_reg[ue_n] = sidx; ue_stk[ue_n] = -1; }
                else          { ue_reg[ue_n] = -1;  ue_stk[ue_n] = e_shared++; }
                e_fp++;
            } else {
                if (sidx < 4) { ue_reg[ue_n] = sidx; ue_stk[ue_n] = -1; }
                else          { ue_reg[ue_n] = -1;  ue_stk[ue_n] = e_shared++; }
                e_gpr++;
            }
            ue_n++;
        }
    }
    int entry_stack_n = e_shared;

    /* ---- pass 2: final unit list (captures first, then user params) ---- */
    ZeroCCTrampUnit u[ZERO_CC_TRAMP_MAX_UNITS];
    int n = 0, gpr_run = 0, fp_run = 0, decl_stack_run = 0;
    int copy_words = 0;

    for (size_t ci = 0; ci < C; ci++) {
        if (nf->nested_capture_byref[ci]) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = false;
            u[n].fin_slot = gpr_run < 4 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            u[n].src = 0;
            u[n].src_off = (int)(32 * ci);
            n++;
            continue;
        }
        const ZeroCCType *t = &nf->nested_capture_types[ci];
        int slots = zero_cc_tramp_unit_slots_isa(t, c->isa);
        bool isfp = zero_cc_tramp_unit_is_fp(t);
        bool big_struct = (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) &&
                          !zero_cc_type_is_vector(t) && t->size > 8;
        int words = (int)((t->size + 3) / 4);
        for (int w = 0; w < slots; w++) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = isfp;
            u[n].fin_slot = isfp ? (fp_run < 8 ? fp_run++ : -1)
                                 : (gpr_run < 4 ? gpr_run++ : -1);
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            if (big_struct) {
                if (w == 0) {
                    u[n].src = 5;
                    u[n].src_off = 88 + 4 * entry_stack_n + 32 * (int)ci;
                    copy_words += words;
                }
            } else {
                u[n].src = 0;
                u[n].src_off = (int)(32 * ci) + 4 * w;
            }
            n++;
        }
    }
    for (int pi = 0; pi < nf->goto_pair_count; pi++) {
        for (int w = 0; w < 2; w++) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = false;
            u[n].fin_slot = gpr_run < 4 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            u[n].src = 0;
            u[n].src_off = (int)(32 * C + 16 * pi) + 4 * w;
            n++;
        }
    }
    int entry_gpr_off = 4;
    int entry_fp_off = 24;
    int entry_st_off = 88;

    int j = 0;
    for (size_t ui = 0; ui < U; ui++) {
        const ZeroCCType *t = &nf->param_types[C + ui];
        int slots = zero_cc_tramp_unit_slots_isa(t, c->isa);
        bool isfp = zero_cc_tramp_unit_is_fp(t);
        for (int w = 0; w < slots; w++) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = isfp;
            u[n].fin_slot = isfp ? (fp_run < 8 ? fp_run++ : -1)
                                 : (gpr_run < 4 ? gpr_run++ : -1);
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            if (ue_isfp[j]) {
                if (ue_reg[j] >= 0) {
                    u[n].src = 2;
                    u[n].src_off = entry_fp_off + 8 * ue_reg[j];
                } else {
                    u[n].src = 3;
                    u[n].src_off = entry_st_off + 4 * ue_stk[j];
                }
            } else if (ue_reg[j] >= 0) {
                u[n].src = 1;
                u[n].src_off = entry_gpr_off + 4 * ue_reg[j];
            } else {
                u[n].src = 3;
                u[n].src_off = entry_st_off + 4 * ue_stk[j];
            }
            n++;
            j++;
        }
    }
    int fin_stack_n = decl_stack_run;

    size_t D = 88 + 4 * (size_t)entry_stack_n + 4 * (size_t)copy_words;
    int stack_bytes = 0;
    for (int i = 0; i < n; i++) {
        if (u[i].fin_stack >= 0)
            stack_bytes += u[i].is_fp ? 8 : 4;
    }
    int stack_pad = ((stack_bytes + 7) & ~7) - stack_bytes;
    D += (size_t)stack_pad;
    D = (D + 7) & ~(size_t)7;

    /* ---- prologue ---- */
    arm32_emit_u32(c, 0xE92D4800u); /* STMDB sp!, {r11, lr} */
    arm32_emit_u32(c, 0xE28DB000u); /* ADD r11, sp, #0 */
    if (D > 0) {
        uint32_t enc = arm32_encode_imm((int64_t)D);
        if (enc != 0xFFFFFFFFu) {
            arm32_emit_u32(c, 0xE24DD000u | enc); /* SUB sp, sp, #D */
        } else {
            aarch32_emit_mov_imm(c, 12, (int64_t)D);
            arm32_emit_u32(c, 0xE04DD00Cu); /* SUB sp, sp, r12 */
        }
    }

    /* Save entry GPRs r0..r3 */
    for (int g = 0; g < 4; g++)
        aarch32_emit_store(c, g, 11, -(int32_t)(entry_gpr_off + 4 * g));
    /* Save entry FPs d0..d7 */
    for (int f = 0; f < 8; f++)
        aarch32_emit_store_fp(c, f, 11, -(int32_t)(entry_fp_off + 8 * f));
    /* Save entry stack units: [r11+8+4s] -> [r11-entry_st_off-4s] */
    for (int s = 0; s < entry_stack_n; s++) {
        aarch32_emit_load(c, 12, 11, 8 + 4 * s);
        aarch32_emit_store(c, 12, 11, -(int32_t)(entry_st_off + 4 * s));
    }

    /* Load ctx base into r14 (lr) via movw/movt with reloc */
    arm32_emit_u32(c, 0xE300E000u); /* movw lr, #0 */
    arm32_emit_u32(c, 0xE340E000u); /* movt lr, #0 */
    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = me_get_offset(c) - 4;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", nf->tramp_ctx_name);
        r->is_call = false;
    } else {
        zero_cc_reloc_overflow_note(c);
    }

    /* Copy big struct captures if any */
    for (size_t ci = 0; ci < C; ci++) {
        if (nf->nested_capture_byref[ci]) continue;
        const ZeroCCType *t = &nf->nested_capture_types[ci];
        bool big_struct = (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) &&
                          !zero_cc_type_is_vector(t) && t->size > 8;
        if (!big_struct) continue;
        int words = (int)((t->size + 3) / 4);
        for (int w = 0; w < words; w++) {
            aarch32_emit_load(c, 12, 14, 32 * (int)ci + 4 * w);
            aarch32_emit_store(c, 12, 11,
                               -(int32_t)(entry_st_off + 4 * entry_stack_n + 32 * (int)ci + 4 * w));
        }
    }

    /* ---- place final REGISTER units ---- */
    for (int i = 0; i < n; i++) {
        ZeroCCTrampUnit *a = &u[i];
        if (a->fin_slot < 0) continue;
        if (a->is_fp) {
            int dst = a->fin_slot;
            switch (a->src) {
            case 0:
                aarch32_emit_load_fp(c, dst, 14, a->src_off);
                break;
            case 2:
            case 3:
                aarch32_emit_load_fp(c, dst, 11, -a->src_off);
                break;
            default: break;
            }
        } else {
            int dst = a->fin_slot;
            switch (a->src) {
            case 0:
                aarch32_emit_load(c, dst, 14, a->src_off);
                break;
            case 1:
            case 3:
                aarch32_emit_load(c, dst, 11, -a->src_off);
                break;
            case 5:
                aarch32_emit_lea(c, dst, 11, -a->src_off);
                break;
            default: break;
            }
        }
    }

    /* ---- place final STACK units (descending order) ---- */
    for (int idx = fin_stack_n - 1; idx >= 0; idx--) {
        for (int i = 0; i < n; i++) {
            ZeroCCTrampUnit *a = &u[i];
            if (a->fin_stack != idx) continue;
            if (a->is_fp) {
                switch (a->src) {
                case 0:
                    aarch32_emit_load_fp(c, 7, 14, a->src_off);
                    break;
                case 2:
                case 3:
                    aarch32_emit_load_fp(c, 7, 11, -a->src_off);
                    break;
                default: break;
                }
                arm32_emit_u32(c, 0xE24DD008u); /* SUB sp, sp, #8 */
                aarch32_emit_store_fp(c, 7, 13, 0); /* VSTR.64 d7, [sp] */
            } else {
                switch (a->src) {
                case 0:
                    aarch32_emit_load(c, 12, 14, a->src_off);
                    break;
                case 1:
                case 3:
                    aarch32_emit_load(c, 12, 11, -a->src_off);
                    break;
                case 5:
                    aarch32_emit_lea(c, 12, 11, -a->src_off);
                    break;
                default: break;
                }
                arm32_emit_u32(c, 0xE24DD004u); /* SUB sp, sp, #4 */
                arm32_emit_u32(c, 0xE58DC000u); /* STR r12, [sp, #0] */
            }
        }
    }

    /* Call real nested function */
    size_t call_pos = me_get_offset(c);
    int32_t diff = (int32_t)(nf->text_offset - (call_pos + 8));
    uint32_t imm24 = ((uint32_t)(diff >> 2)) & 0x00FFFFFFu;
    arm32_emit_u32(c, 0xEB000000u | imm24); /* BL nf */

    /* Epilogue */
    arm32_emit_u32(c, 0xE28BD000u); /* ADD sp, r11, #0 */
    arm32_emit_u32(c, 0xE8BD4800u); /* LDMIA sp!, {r11, lr} */
    arm32_emit_u32(c, 0xE12FFF1Eu); /* BX lr */
}

/* =========================================================================
 * AArch32 backend instance - zero-initialized, populated by aarch32_init_backend()
 * ========================================================================= */
ZeroCCISABackend aarch32_backend;

void aarch32_init_backend(void) {
    memset(&aarch32_backend, 0, sizeof(aarch32_backend));
    aarch32_backend.isa = ISA_AARCH32;
    aarch32_backend.regs = &aarch32_regs;
    aarch32_backend.conv = &aarch32_conv;
    aarch32_backend.fp_reg = 11;  /* r11 */
    aarch32_backend.sp_reg = 13;  /* sp */
    aarch32_backend.scratch_reg1 = 12;  /* r12 (ip) */
    aarch32_backend.scratch_reg2 = 9;   /* r9 (caller-saved, non-arg) */
    aarch32_backend.fp_scratch_reg1 = 15;
    aarch32_backend.fp_scratch_reg2 = 14;
    aarch32_backend.g128_scratch_lo[0] = 12;  /* r12 */
    aarch32_backend.g128_scratch_hi[0] = 9;   /* r9 */
    aarch32_backend.g128_scratch_lo[1] = 8;   /* r8 */
    aarch32_backend.g128_scratch_hi[1] = 10;  /* r10 */
    aarch32_backend.g128_scratch_pair_count = 2;
    aarch32_backend.callee_save_frame_reserve = 32;  /* 8 callee-saved * 4 bytes */

    /* MC emitters */
    aarch32_backend.emit_prologue = aarch32_emit_prologue;
    aarch32_backend.emit_leave = aarch32_emit_leave;
    aarch32_backend.emit_epilogue = aarch32_emit_epilogue;
    aarch32_backend.get_code_offset = aarch32_get_code_offset;
    aarch32_backend.emit_stack_guard_init = aarch32_emit_stack_guard_init;
    aarch32_backend.emit_stack_guard_check = aarch32_emit_stack_guard_check;

    aarch32_backend.emit_prologue_asm = aarch32_emit_prologue_asm;
    aarch32_backend.emit_epilogue_asm = aarch32_emit_epilogue_asm;
    aarch32_backend.emit_stack_guard_init_asm = aarch32_emit_stack_guard_init_asm;
    aarch32_backend.emit_stack_guard_check_asm = aarch32_emit_stack_guard_check_asm;
    aarch32_backend.get_hw_reg_name = aarch32_get_hw_reg_name;
    aarch32_backend.get_vec_reg_name = aarch32_get_vec_reg_name;
    aarch32_backend.emit_load_sized_asm = aarch32_emit_load_sized_asm;
    aarch32_backend.emit_store_sized_asm = aarch32_emit_store_sized_asm;
    aarch32_backend.emit_test_zero_asm = aarch32_emit_test_zero_asm;
    aarch32_backend.emit_cmp_regs_asm = aarch32_emit_cmp_regs_asm;

    aarch32_backend.emit_mov_imm = aarch32_emit_mov_imm;
    aarch32_backend.emit_add = aarch32_emit_add;
    aarch32_backend.emit_sub = aarch32_emit_sub;
    aarch32_backend.emit_mul = aarch32_emit_mul;
    aarch32_backend.emit_div = aarch32_emit_div;
    aarch32_backend.emit_mod = aarch32_emit_mod;
    aarch32_backend.emit_and = aarch32_emit_and;
    aarch32_backend.emit_or = aarch32_emit_or;
    aarch32_backend.emit_xor = aarch32_emit_xor;
    aarch32_backend.emit_shl = aarch32_emit_shl;
    aarch32_backend.emit_shr = aarch32_emit_shr;
    aarch32_backend.emit_sar = aarch32_emit_sar;
    aarch32_backend.emit_cmp = aarch32_emit_cmp;
    aarch32_backend.emit_cond_set = aarch32_emit_cond_set;
    aarch32_backend.emit_setl = aarch32_emit_setl;
    aarch32_backend.emit_setle = aarch32_emit_setle;
    aarch32_backend.emit_setg = aarch32_emit_setg;
    aarch32_backend.emit_setge = aarch32_emit_setge;
    aarch32_backend.emit_sete = aarch32_emit_sete;
    aarch32_backend.emit_setne = aarch32_emit_setne;
    aarch32_backend.emit_neg = aarch32_emit_neg;
    aarch32_backend.emit_extend = aarch32_emit_extend;
    aarch32_backend.emit_mov_reg = aarch32_emit_mov_reg;
    aarch32_backend.emit_not = aarch32_emit_not;
    aarch32_backend.emit_test = aarch32_emit_test;
    aarch32_backend.emit_cqo = aarch32_emit_cqo;
    aarch32_backend.emit_idiv = aarch32_emit_idiv;
    aarch32_backend.emit_lea = aarch32_emit_lea;
    aarch32_backend.emit_push_reg = aarch32_emit_push_reg;
    aarch32_backend.emit_pop_reg = aarch32_emit_pop_reg;
    aarch32_backend.emit_add_imm = aarch32_emit_add_imm;
    aarch32_backend.emit_sub_imm = aarch32_emit_sub_imm;
    aarch32_backend.emit_inc = aarch32_emit_inc;
    aarch32_backend.emit_dec = aarch32_emit_dec;

    aarch32_backend.patch_jmp = aarch32_patch_jmp;
    aarch32_backend.resolve_labels = aarch32_resolve_labels;

    aarch32_backend.get_int_reg = aarch32_get_int_reg;
    aarch32_backend.get_fp_reg = aarch32_get_fp_reg;

    aarch32_backend.emit_load = aarch32_emit_load;
    aarch32_backend.emit_store = aarch32_emit_store;

    aarch32_backend.emit_deref = aarch32_emit_deref;
    aarch32_backend.emit_load_byte = aarch32_emit_load_byte;
    aarch32_backend.emit_load_16 = aarch32_emit_load_16;
    aarch32_backend.emit_load_32 = aarch32_emit_load_32;
    aarch32_backend.emit_store_byte = aarch32_emit_store_byte;
    aarch32_backend.emit_store_16 = aarch32_emit_store_16;
    aarch32_backend.emit_store_32 = aarch32_emit_store_32;
    aarch32_backend.emit_store_64 = aarch32_emit_store_64;
    aarch32_backend.emit_lea_rip = aarch32_emit_lea_rip;

    aarch32_backend.emit_jmp = aarch32_emit_jmp;
    aarch32_backend.emit_jeq = aarch32_emit_jeq;
    aarch32_backend.emit_jne = aarch32_emit_jne;
    aarch32_backend.emit_jlt = aarch32_emit_jlt;
    aarch32_backend.emit_jle = aarch32_emit_jle;
    aarch32_backend.emit_jgt = aarch32_emit_jgt;
    aarch32_backend.emit_jge = aarch32_emit_jge;
    aarch32_backend.emit_jcond = aarch32_emit_jcond;

    aarch32_backend.emit_call = aarch32_emit_call;
    aarch32_backend.emit_call_reg = aarch32_emit_call_reg;
    aarch32_backend.emit_jmp_reg = aarch32_emit_jmp_reg;
    aarch32_backend.emit_lea_label = aarch32_emit_lea_label;
    aarch32_backend.emit_ret = aarch32_emit_ret;
    aarch32_backend.emit_label = aarch32_emit_label;

    aarch32_backend.emit_load_fp = aarch32_emit_load_fp;
    aarch32_backend.emit_store_fp = aarch32_emit_store_fp;
    aarch32_backend.emit_load_fp_rodata = aarch32_emit_load_fp_rodata;
    aarch32_backend.emit_vloadsd = aarch32_emit_load_fp;
    aarch32_backend.emit_vstoredsd = aarch32_emit_store_fp;
    aarch32_backend.emit_vloadss = aarch32_emit_load_single_fp;
    aarch32_backend.emit_vstoress = aarch32_emit_store_single_fp;
    aarch32_backend.emit_vloadss_raw = aarch32_emit_load_single_fp_raw;
    aarch32_backend.emit_vstoress_raw = aarch32_emit_store_single_fp_raw;
    aarch32_backend.emit_cvttsd2ss = aarch32_emit_fp_cvttsd2ss;
    aarch32_backend.emit_cvtsd2ss = aarch32_emit_fp_cvttsd2ss;
    aarch32_backend.emit_cvtss2sd = aarch32_emit_fp_cvtss2sd;
    aarch32_backend.emit_cvtsi2ss = aarch32_emit_fp_cvtsi2ss;

    aarch32_backend.emit_fp_add = aarch32_emit_fp_add;
    aarch32_backend.emit_fp_sub = aarch32_emit_fp_sub;
    aarch32_backend.emit_fp_mul = aarch32_emit_fp_mul;
    aarch32_backend.emit_fp_div = aarch32_emit_fp_div;
    aarch32_backend.emit_fp_neg = aarch32_emit_fp_neg;
    aarch32_backend.emit_fp_cmp = aarch32_emit_fp_cmp;
    aarch32_backend.emit_fp_cvtsi = aarch32_emit_fp_cvtsi;
    aarch32_backend.emit_fp_cvtsf = aarch32_emit_fp_cvtsf;
    aarch32_backend.emit_fp_mov = aarch32_emit_fp_mov;

    aarch32_backend.emit_param_save_int = aarch32_emit_param_save_int;
    aarch32_backend.emit_param_save_fp = aarch32_emit_param_save_fp;
    aarch32_backend.emit_variadic_spill = aarch32_emit_variadic_spill;
    aarch32_backend.emit_variadic_al_setup = aarch32_emit_variadic_al_setup;

    aarch32_backend.emit_vla_alloc = aarch32_emit_vla_alloc;

    /* Phase 2: NEON 128-bit vector load/store */
    aarch32_backend.emit_vload = aarch32_emit_vload;
    aarch32_backend.emit_vstore = aarch32_emit_vstore;

    /* Phase 2: 64-bit multiply/divide/modulo */
    aarch32_backend.emit_mul64 = aarch32_emit_mul64;
    aarch32_backend.emit_div64 = aarch32_emit_div64;
    aarch32_backend.emit_mod64 = aarch32_emit_mod64;
    aarch32_backend.emit_udiv64 = aarch32_emit_udiv64;
    aarch32_backend.emit_umod64 = aarch32_emit_umod64;

    /* Phase 2: GPR64 pair operations */
    aarch32_backend.emit_add64 = aarch32_emit_add64;
    aarch32_backend.emit_sub64 = aarch32_emit_sub64;
    aarch32_backend.emit_and64 = aarch32_emit_and64;
    aarch32_backend.emit_or64 = aarch32_emit_or64;
    aarch32_backend.emit_xor64 = aarch32_emit_xor64;
    aarch32_backend.emit_not64 = aarch32_emit_not64;
    aarch32_backend.emit_neg64 = aarch32_emit_neg64;
    aarch32_backend.emit_shl64 = aarch32_emit_shl64;
    aarch32_backend.emit_shr64 = aarch32_emit_shr64;
    aarch32_backend.emit_sar64 = aarch32_emit_sar64;
    aarch32_backend.emit_cmp64 = aarch32_emit_cmp64;
    aarch32_backend.emit_mov64 = aarch32_emit_mov64;
    aarch32_backend.emit_load64 = aarch32_emit_load64;
    aarch32_backend.emit_store64 = aarch32_emit_store64;
    aarch32_backend.emit_zext32_64 = aarch32_emit_zext32_64;
    aarch32_backend.emit_sext32_64 = aarch32_emit_sext32_64;
    aarch32_backend.emit_trunc64_32 = aarch32_emit_trunc64_32;
    aarch32_backend.emit_mov64_imm = aarch32_emit_mov64_imm;

    aarch32_backend.emit_atomic_xadd = aarch32_emit_atomic_xadd;
    aarch32_backend.emit_atomic_or = aarch32_emit_atomic_or;
    aarch32_backend.emit_atomic_and = aarch32_emit_atomic_and;
    aarch32_backend.emit_atomic_xor = aarch32_emit_atomic_xor;
    aarch32_backend.emit_atomic_cmpxchg = aarch32_emit_atomic_cmpxchg;
    aarch32_backend.emit_atomic_cmpxchg_bool = aarch32_emit_atomic_cmpxchg_bool;
    aarch32_backend.emit_atomic_xchg = aarch32_emit_atomic_xchg;
    aarch32_backend.emit_mfence = aarch32_emit_mfence;

    aarch32_backend.emit_trap = aarch32_emit_trap;
    aarch32_backend.emit_trap_asm = aarch32_emit_trap_asm;
    aarch32_backend.emit_ovf_trap = aarch32_emit_ovf_trap;
    aarch32_backend.emit_ovf_trap_asm = aarch32_emit_ovf_trap_asm;
    aarch32_backend.emit_syscall = aarch32_emit_syscall;
    aarch32_backend.emit_clone = aarch32_emit_clone;
    aarch32_backend.emit_cmp_zero = aarch32_emit_cmp_zero;
    aarch32_backend.emit_lnot = aarch32_emit_lnot;

    aarch32_backend.emit_const = aarch32_emit_const;
    aarch32_backend.emit_stack_adjust = aarch32_emit_stack_adjust;
    aarch32_backend.emit_stack_align = aarch32_emit_stack_align;

    /* ASM emitters (Phase 1) */
    aarch32_backend.emit_mov_imm_asm = aarch32_emit_mov_imm_asm;
    aarch32_backend.emit_mov_reg_asm = aarch32_emit_mov_reg_asm;
    aarch32_backend.emit_load_asm = aarch32_emit_load_asm;
    aarch32_backend.emit_store_asm = aarch32_emit_store_asm;
    aarch32_backend.emit_add_asm = aarch32_emit_add_asm;
    aarch32_backend.emit_sub_asm = aarch32_emit_sub_asm;
    aarch32_backend.emit_mul_asm = aarch32_emit_mul_asm;
    aarch32_backend.emit_div_asm = aarch32_emit_div_asm;
    aarch32_backend.emit_mod_asm = aarch32_emit_mod_asm;
    aarch32_backend.emit_and_asm = aarch32_emit_and_asm;
    aarch32_backend.emit_or_asm = aarch32_emit_or_asm;
    aarch32_backend.emit_xor_asm = aarch32_emit_xor_asm;
    aarch32_backend.emit_shl_asm = aarch32_emit_shl_asm;
    aarch32_backend.emit_shr_asm = aarch32_emit_shr_asm;
    aarch32_backend.emit_sar_asm = aarch32_emit_sar_asm;
    aarch32_backend.emit_extend_asm = aarch32_emit_extend_asm;
    aarch32_backend.emit_cmp_asm = aarch32_emit_cmp_asm;
    aarch32_backend.emit_cond_set_asm = aarch32_emit_cond_set_asm;
    aarch32_backend.emit_neg_asm = aarch32_emit_neg_asm;
    aarch32_backend.emit_add_imm_asm = aarch32_emit_add_imm_asm;
    aarch32_backend.emit_sub_imm_asm = aarch32_emit_sub_imm_asm;
    aarch32_backend.emit_inc_asm = aarch32_emit_inc_asm;
    aarch32_backend.emit_dec_asm = aarch32_emit_dec_asm;
    aarch32_backend.emit_not_asm = aarch32_emit_not_asm;
    aarch32_backend.emit_lea_asm = aarch32_emit_lea_asm;
    aarch32_backend.emit_deref_asm = aarch32_emit_deref_asm;
    aarch32_backend.emit_call_asm = aarch32_emit_call_asm;
    aarch32_backend.emit_ret_asm = aarch32_emit_ret_asm;
    aarch32_backend.emit_label_asm = aarch32_emit_label_asm;
    aarch32_backend.emit_jmp_asm = aarch32_emit_jmp_asm;
    aarch32_backend.emit_jeq_asm = aarch32_emit_jeq_asm;
    aarch32_backend.emit_jne_asm = aarch32_emit_jne_asm;
    aarch32_backend.emit_jlt_asm = aarch32_emit_jlt_asm;
    aarch32_backend.emit_jle_asm = aarch32_emit_jle_asm;
    aarch32_backend.emit_jgt_asm = aarch32_emit_jgt_asm;
    aarch32_backend.emit_jge_asm = aarch32_emit_jge_asm;
    aarch32_backend.emit_load_fp_asm = aarch32_emit_load_fp_asm;
    aarch32_backend.emit_store_fp_asm = aarch32_emit_store_fp_asm;
    aarch32_backend.emit_fp_add_asm = aarch32_emit_fp_add_asm;
    aarch32_backend.emit_fp_sub_asm = aarch32_emit_fp_sub_asm;
    aarch32_backend.emit_fp_mul_asm = aarch32_emit_fp_mul_asm;
    aarch32_backend.emit_fp_div_asm = aarch32_emit_fp_div_asm;
    aarch32_backend.emit_fp_cmp_asm = aarch32_emit_fp_cmp_asm;
    aarch32_backend.emit_fp_mov_asm = aarch32_emit_fp_mov_asm;
    aarch32_backend.emit_vaddsd_asm = aarch32_emit_fp_add_asm;
    aarch32_backend.emit_vsubsd_asm = aarch32_emit_fp_sub_asm;
    aarch32_backend.emit_vmulsd_asm = aarch32_emit_fp_mul_asm;
    aarch32_backend.emit_vdivsd_asm = aarch32_emit_fp_div_asm;
    aarch32_backend.emit_vmovsd_asm = aarch32_emit_fp_mov_asm;
    aarch32_backend.emit_fp_cvtsf_asm = aarch32_emit_fp_cvtsf_asm;
    aarch32_backend.emit_fp_cvtsi_asm = aarch32_emit_fp_cvtsi_asm;
    aarch32_backend.emit_cvtss2sd_asm = aarch32_emit_cvtss2sd_asm;
    aarch32_backend.emit_cvtsd2ss_asm = aarch32_emit_cvtsd2ss_asm;
    aarch32_backend.emit_vla_alloc_asm = aarch32_emit_vla_alloc_asm;
    aarch32_backend.emit_param_save_int_asm = aarch32_emit_param_save_int_asm;
    aarch32_backend.emit_param_save_fp_asm = aarch32_emit_param_save_fp_asm;
    aarch32_backend.emit_variadic_spill_asm = aarch32_emit_variadic_spill_asm;
    aarch32_backend.emit_syscall_asm = aarch32_emit_syscall_asm;
    aarch32_backend.emit_clone_asm = aarch32_emit_clone_asm;
    aarch32_backend.emit_inline_asm_asm = aarch32_emit_inline_asm_asm;
    aarch32_backend.emit_inline_asm_mc = aarch32_emit_inline_asm_mc;
    aarch32_backend.emit_filescope_asm_mc = aarch32_emit_filescope_asm_mc;

    aarch32_backend.emit_vload_asm = aarch32_emit_vload_asm;
    aarch32_backend.emit_vstore_asm = aarch32_emit_vstore_asm;

    /* Phase 2: 64-bit multiply/divide/modulo ASM */
    aarch32_backend.emit_mul64_asm = aarch32_emit_mul64_asm;
    aarch32_backend.emit_div64_asm = aarch32_emit_div64_asm;
    aarch32_backend.emit_mod64_asm = aarch32_emit_mod64_asm;
    aarch32_backend.emit_udiv64_asm = aarch32_emit_udiv64_asm;
    aarch32_backend.emit_umod64_asm = aarch32_emit_umod64_asm;

    /* Phase 2: 64-bit pair ASM emitters */
    aarch32_backend.emit_add64_asm = aarch32_emit_add64_asm;
    aarch32_backend.emit_sub64_asm = aarch32_emit_sub64_asm;
    aarch32_backend.emit_and64_asm = aarch32_emit_and64_asm;
    aarch32_backend.emit_or64_asm = aarch32_emit_or64_asm;
    aarch32_backend.emit_xor64_asm = aarch32_emit_xor64_asm;
    aarch32_backend.emit_not64_asm = aarch32_emit_not64_asm;
    aarch32_backend.emit_neg64_asm = aarch32_emit_neg64_asm;
    aarch32_backend.emit_shl64_asm = aarch32_emit_shl64_asm;
    aarch32_backend.emit_shr64_asm = aarch32_emit_shr64_asm;
    aarch32_backend.emit_sar64_asm = aarch32_emit_sar64_asm;
    aarch32_backend.emit_cmp64_asm = aarch32_emit_cmp64_asm;
    aarch32_backend.emit_mov64_asm = aarch32_emit_mov64_asm;
    aarch32_backend.emit_load64_asm = aarch32_emit_load64_asm;
    aarch32_backend.emit_store64_asm = aarch32_emit_store64_asm;
    aarch32_backend.emit_zext32_64_asm = aarch32_emit_zext32_64_asm;
    aarch32_backend.emit_sext32_64_asm = aarch32_emit_sext32_64_asm;
    aarch32_backend.emit_trunc64_32_asm = aarch32_emit_trunc64_32_asm;
    aarch32_backend.emit_mov64_imm_asm = aarch32_emit_mov64_imm_asm;
}
