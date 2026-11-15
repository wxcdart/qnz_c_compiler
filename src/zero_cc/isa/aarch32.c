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
#define ARM_COND(cc)     ((cc) << 28)
#define ARM_DP_I_IMM     (1u << 25)    /* data-processing immediate */
#define ARM_DP_SHIFT_IMM (1u << 25)    /* also for shift-immediate */
#define ARM_RD(rd)       ((rd) << 12)
#define ARM_RN(rn)       ((rn) << 16)
#define ARM_RM(rm)       (rm)
#define ARM_RS(rs)       ((rs) << 8)
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
#define ARM_LDR_STR(p,u,b,w,l) ((1u<<26)|((p)<<24)|((u)<<23)|((b)<<22)|((w)<<21)|((l)<<20))

/* =========================================================================
 * Register info for AArch32
 * ========================================================================= */
static const ISARegInfo aarch32_regs = {
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
    if (c->lowering.is_frameless) return;
    (void)callee_save_mask;

    /* AAPCS requires 8-byte stack alignment at call sites */
    stack_size = (stack_size + 7) & ~7;

    if (c->lowering.frame_align > 8) {
        /* C11 _Alignas(N) realign mode - not fully implemented for AArch32 yet */
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
    if (c->lowering.is_frameless) return;
    /* ADD sp, r11, #0  (restore sp from fp) */
    arm32_emit_u32(c, 0xE28BD000u);
    /* LDMIA sp!, {r11, lr}  (pop {r11, lr}) */
    arm32_emit_u32(c, 0xE8BD4800u);
}

static void aarch32_emit_epilogue(ZeroCCCompiler *c) {
    aarch32_emit_leave(c);
    /* BX lr  (return) */
    arm32_emit_u32(c, 0xE12FFF1Eu);
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

static void aarch32_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
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
    arm32_emit_u32(c, 0xE3000000u | (r << 12));  /* movw dst, 0 */
    size_t patch_pos = me_get_offset(c);
    arm32_emit_u32(c, 0xE3400000u | (r << 12));  /* movt dst, 0 */
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
 * Floating point (placeholder - Phase 2)
 * ========================================================================= */
static void aarch32_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    (void)c; (void)dst_reg; (void)base_reg; (void)offset;
}
static void aarch32_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    (void)c; (void)src_reg; (void)base_reg; (void)offset;
}
static void aarch32_emit_load_fp_rodata(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    (void)c; (void)dst_reg; (void)rodata_offset;
}
static void aarch32_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) { (void)c; (void)dst; (void)src1; (void)src2; }
static void aarch32_emit_fp_sub(ZeroCCCompiler *c, int dst, int src1, int src2) { (void)c; (void)dst; (void)src1; (void)src2; }
static void aarch32_emit_fp_mul(ZeroCCCompiler *c, int dst, int src1, int src2) { (void)c; (void)dst; (void)src1; (void)src2; }
static void aarch32_emit_fp_div(ZeroCCCompiler *c, int dst, int src1, int src2) { (void)c; (void)dst; (void)src1; (void)src2; }
static void aarch32_emit_fp_neg(ZeroCCCompiler *c, int dst, int src) { (void)c; (void)dst; (void)src; }
static void aarch32_emit_fp_cmp(ZeroCCCompiler *c, int src1, int src2) { (void)c; (void)src1; (void)src2; }
static void aarch32_emit_fp_cvtsi(ZeroCCCompiler *c, int int_dst, int fp_src) { (void)c; (void)int_dst; (void)fp_src; }
static void aarch32_emit_fp_cvtsf(ZeroCCCompiler *c, int fp_dst, int int_src) { (void)c; (void)fp_dst; (void)int_src; }
static void aarch32_emit_fp_mov(ZeroCCCompiler *c, int dst, int src) { (void)c; (void)dst; (void)src; }

/* =========================================================================
 * Parameter spilling / function setup
 * ========================================================================= */
static void aarch32_emit_param_save_int(ZeroCCCompiler *c, int reg_idx, int32_t offset) {
    if (reg_idx < 0 || reg_idx >= 4) return;
    int reg = aarch32_int_arg_regs[reg_idx];
    aarch32_emit_store(c, reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -offset));
}
static void aarch32_emit_param_save_fp(ZeroCCCompiler *c, int reg_idx, int32_t offset, int is_single) {
    (void)c; (void)reg_idx; (void)offset; (void)is_single;
}
static void aarch32_emit_variadic_spill(ZeroCCCompiler *c, int param_count, int32_t base_offset) {
    (void)c; (void)param_count; (void)base_offset;
}
static void aarch32_emit_variadic_al_setup(ZeroCCCompiler *c, int num_fp_args) {
    (void)c; (void)num_fp_args;
}

/* =========================================================================
 * Syscall / Clone (Linux ARM 32-bit syscall numbers)
 * ========================================================================= */
static void aarch32_emit_syscall(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    if (arg_count >= 1) {
        int sysno = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[0]);
        aarch32_emit_mov_reg(c, 7, sysno);  /* r7 = syscall number */
    }
    for (int i = 1; i < arg_count && i <= 6; i++) {
        int reg = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[i]);
        aarch32_emit_mov_reg(c, i - 1, reg);  /* r0-r5 = args */
    }
    arm32_emit_u32(c, ARM_COND(ARM_CC_AL) | 0xEF000000u);  /* SWI #0 */
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
static void aarch32_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    fprintf(c->asm_out, "\tpush\t{r11, lr}\n");
    fprintf(c->asm_out, "\tadd\tr11, sp, #0\n");
    if (stack_size > 0) {
        fprintf(c->asm_out, "\tsub\tsp, sp, #%zu\n", (stack_size + 7) & ~7);
    }
}
static void aarch32_emit_epilogue_asm(ZeroCCCompiler *c) {
    fprintf(c->asm_out, "\tpop\t{r11, lr}\n");
    fprintf(c->asm_out, "\tmov\tpc, lr\n");
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
    fprintf(out, "\tmov\t%s, #%ld\n", aarch32_get_hw_reg_name(dst), imm);
}
static void aarch32_emit_mov_reg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    if (dst != src) fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    (void)c;
    fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), offset);
}
static void aarch32_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    (void)c;
    fprintf(out, "\tstr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(base), offset);
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
    (void)c;
    fprintf(out, "\tsdiv\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_mod_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c; (void)dst; (void)src1; (void)src2; (void)out;
    fprintf(out, "\t# mod: %s = %s %% %s (expand)\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
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
static void aarch32_emit_cond_set_asm(ZeroCCCompiler *c, int dst, int cond, FILE *out) {
    (void)c;
    static const char *arm_cc_str[6] = {"lt","le","gt","ge","eq","ne"};
    fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
    fprintf(out, "\tmov%s\t%s, #1\n", arm_cc_str[cond], aarch32_get_hw_reg_name(dst));
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
    (void)c;
    fprintf(out, "\tadd\t%s, %s, #%d\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), offset);
}
static void aarch32_emit_deref_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tldr\t%s, [%s]\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_call_asm(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, int *stack_pos, FILE *out) {
    (void)c; (void)arg_count; (void)arg_temps; (void)is_fp; (void)stack_pos;
    fprintf(out, "\tbl\t%s\n", func_name);
}
static void aarch32_emit_ret_asm(ZeroCCCompiler *c, FILE *out) {
    (void)c;
    fprintf(out, "\tmov\tpc, lr\n");
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
    (void)c; (void)dst; (void)base; (void)offset; (void)out;
}
static void aarch32_emit_store_fp_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    (void)c; (void)src; (void)base; (void)offset; (void)out;
}
static void aarch32_emit_fp_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c; (void)dst; (void)src1; (void)src2; (void)out;
}
static void aarch32_emit_fp_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c; (void)dst; (void)src1; (void)src2; (void)out;
}
static void aarch32_emit_fp_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c; (void)dst; (void)src1; (void)src2; (void)out;
}
static void aarch32_emit_fp_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c; (void)dst; (void)src1; (void)src2; (void)out;
}
static void aarch32_emit_fp_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    (void)c; (void)src1; (void)src2; (void)out;
}
static void aarch32_emit_fp_mov_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c; (void)dst; (void)src; (void)out;
}
static void aarch32_emit_fp_cvtsf_asm(ZeroCCCompiler *c, int fp_dst, int int_src, FILE *out) {
    (void)c; (void)fp_dst; (void)int_src; (void)out;
}
static void aarch32_emit_fp_cvtsi_asm(ZeroCCCompiler *c, int int_dst, int fp_src, FILE *out) {
    (void)c; (void)int_dst; (void)fp_src; (void)out;
}
static void aarch32_emit_vla_alloc_asm(ZeroCCCompiler *c, int dst, int size, FILE *out) {
    (void)c; (void)dst; (void)size; (void)out;
    fprintf(out, "\tsub\tsp, sp, %s\n", aarch32_get_hw_reg_name(size));
    fprintf(out, "\tmov\t%s, sp\n", aarch32_get_hw_reg_name(dst));
}
static void aarch32_emit_param_save_int_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, FILE *out) {
    (void)c;
    fprintf(out, "\tstr\tr%d, [r11, #-%d]\n", reg_idx, offset);
}
static void aarch32_emit_param_save_fp_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, int is_single, FILE *out) {
    (void)c; (void)reg_idx; (void)offset; (void)is_single; (void)out;
}
static void aarch32_emit_variadic_spill_asm(ZeroCCCompiler *c, int param_count, int32_t base_offset, FILE *out) {
    (void)c; (void)param_count; (void)base_offset; (void)out;
}
static void aarch32_emit_syscall_asm(ZeroCCCompiler *c, int arg_count, const int *arg_temps, FILE *out) {
    (void)c; (void)arg_count; (void)arg_temps; (void)out;
    fprintf(out, "\t# syscall (arm 32-bit)\n");
}
static void aarch32_emit_clone_asm(ZeroCCCompiler *c, int arg_count, const int *arg_temps, FILE *out) {
    (void)c; (void)arg_count; (void)arg_temps; (void)out;
    fprintf(out, "\t# __zerocc_clone (arm 32-bit)\n");
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

/* Vector asm placeholders */
static void aarch32_emit_vload_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    (void)c; (void)dst; (void)base; (void)offset; (void)out;
}
static void aarch32_emit_vstore_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    (void)c; (void)src; (void)base; (void)offset; (void)out;
}

/* =========================================================================
 * Code offset getter
 * ========================================================================= */
static size_t aarch32_get_code_offset(ZeroCCCompiler *c) {
    return c->text_section.length;
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
    aarch32_backend.fp_scratch_reg1 = REGALLOC_FP_SCRATCH1;
    aarch32_backend.fp_scratch_reg2 = REGALLOC_FP_SCRATCH2;
    aarch32_backend.g128_scratch_lo[0] = 0;
    aarch32_backend.g128_scratch_lo[1] = 0;
    aarch32_backend.g128_scratch_hi[0] = 0;
    aarch32_backend.g128_scratch_hi[1] = 0;
    aarch32_backend.g128_scratch_pair_count = 0;
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
    aarch32_backend.emit_fp_cvtsf_asm = aarch32_emit_fp_cvtsf_asm;
    aarch32_backend.emit_fp_cvtsi_asm = aarch32_emit_fp_cvtsi_asm;
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
}
