#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/ir/asm_emitter.h"
#include "zero_cc/isa/aarch64_asm.c"  /* Include assembly emission functions */
#include <stdint.h>

/* ARM64 register info */
static const ISARegInfo aarch64_regs = {
    .int_regs = {
        "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
        "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
        "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
        "x24", "x25", "x26", "x27", "x28", "x30",  /* x29 reserved as FP */
    },
    .fp_regs = {
        "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
        "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
        "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
        "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
    },
    .int_arg_regs = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"},
    .fp_arg_regs = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"},
    .int_ret_reg = "x0",
    .fp_ret_reg = "v0",
    .num_int_regs = 30,  /* Reduced from 31 (excluded x29) */
    .num_fp_regs = 32,
    .vec_regs = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"},
    .num_vec_regs = 32,
};

/* AArch64 AAPCS64 calling convention */
/* Register numbers: 0-30=x0-x30, 31=sp */
static const int aarch64_int_arg_regs[] = {0, 1, 2, 3, 4, 5, 6, 7};  /* x0-x7 */
static const int aarch64_fp_arg_regs[] = {0, 1, 2, 3, 4, 5, 6, 7};  /* v0-v7 */
static const int aarch64_callee_saved_gpr[] = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30};  /* x19-x29, x30 */

/* Callee-save slot offsets (AArch64 specific) */
/* Each callee-saved register gets 8 bytes */
static const int32_t aarch64_callee_save_slot_offset[] = {
    /* x19 */ 8,
    /* x20 */ 16,
    /* x21 */ 24,
    /* x22 */ 32,
    /* x23 */ 40,
    /* x24 */ 48,
    /* x25 */ 56,
    /* x26 */ 64,
    /* x27 */ 72,
    /* x28 */ 80,
    /* x29 */ 88,
    /* x30 */ 96,
};

static const ISACallingConvention aarch64_conv = {
    .int_arg_regs = aarch64_int_arg_regs,
    .int_arg_reg_count = 8,
    .fp_arg_regs = aarch64_fp_arg_regs,
    .fp_arg_reg_count = 8,
    .callee_saved_gpr = aarch64_callee_saved_gpr,
    .callee_saved_gpr_count = 12,
    .callee_save_slot_offsets = aarch64_callee_save_slot_offset,
};

/* ARM64 instruction encoding helpers */
static inline void arm64_emit_u32(ZeroCCCompiler *c, uint32_t insn) {
    me_emit_u8(c, insn & 0xFF);
    me_emit_u8(c, (insn >> 8) & 0xFF);
    me_emit_u8(c, (insn >> 16) & 0xFF);
    me_emit_u8(c, (insn >> 24) & 0xFF);
}

static void aarch64_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm);

/* ARM64 backend implementation */
static void aarch64_emit_prologue(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask) {
    if (c->lowering.is_frameless) return;
    (void)callee_save_mask;
    /* Ensure 16-byte alignment as required by AArch64 ABI */
    stack_size = (stack_size + 15) & ~15;

    if (c->lowering.omit_frame_pointer) {
        /* Option 4: no frame pointer. Only the return address is saved
         * (16 bytes to keep SP 16-byte aligned); slots are addressed from
         * SP with a delta of `aligned` (see ast_lowering.c). */
        arm64_emit_u32(c, 0xF81F0FFEu);  /* str x30, [sp, #-16]! */
        if (stack_size > 0) {
            if (stack_size <= 4095) {
                uint32_t imm12 = (uint32_t)stack_size & 0xFFF;
                arm64_emit_u32(c, 0xD10003FF | (imm12 << 10));  /* sub sp, sp, #stack_size */
            } else {
                aarch64_emit_mov_imm(c, 16, stack_size);
                arm64_emit_u32(c, 0xCB3063FF);  /* sub sp, sp, x16 */
            }
        }
        return;
    }

    /* Frame setup with x29 at top of local frame so negative offsets [x29, #-off] lie above sp:
     * 1. stp x29, x30, [sp, #-16]!
     * 2. mov x29, sp
     * 3. sub sp, sp, #stack_size
     */
    arm64_emit_u32(c, 0xA9BF7BFD);  /* stp x29, x30, [sp, #-16]! */
    arm64_emit_u32(c, 0x910003FD);  /* mov x29, sp */
    if (stack_size > 0) {
        if (stack_size <= 4095) {
            uint32_t imm12 = (uint32_t)stack_size & 0xFFF;
            arm64_emit_u32(c, 0xD10003FF | (imm12 << 10));  /* sub sp, sp, #stack_size */
        } else {
            aarch64_emit_mov_imm(c, 16, stack_size);
            arm64_emit_u32(c, 0xCB3063FF);  /* sub sp, sp, x16 */
        }
    }
}

static void aarch64_emit_fp_teardown(ZeroCCCompiler *c); /* fwd (Option 4 teardown) */

static void aarch64_emit_epilogue(ZeroCCCompiler *c) {
    if (!c->lowering.is_frameless) {
        if (c->lowering.omit_frame_pointer) {
            /* Option 4: raise SP by the frame, restore x30, ret. */
            aarch64_emit_fp_teardown(c);
        } else {
            /* 1. mov sp, x29
             * 2. ldp x29, x30, [sp], #16
             */
            arm64_emit_u32(c, 0x910003BF);  /* mov sp, x29 */
            arm64_emit_u32(c, 0xA8C17BFD);  /* ldp x29, x30, [sp], #16 */
        }
    }
    /* 3. ret */
    arm64_emit_u32(c, 0xD65F03C0);  /* ret */
}

/* Option 4: SP-based teardown (no x29). frame_delta == aligned frame size. */
static void aarch64_emit_fp_teardown(ZeroCCCompiler *c) {
    size_t aligned = (size_t)c->lowering.frame_delta;
    if (aligned > 0) {
        if (aligned <= 4095) {
            arm64_emit_u32(c, 0x910003FF | (((uint32_t)aligned & 0xFFF) << 10));  /* add sp, sp, #aligned */
        } else {
            aarch64_emit_mov_imm(c, 16, aligned);
            /* add x16, sp, x16 ; mov sp, x16  (SP is not a valid ADD-extended Rd) */
            arm64_emit_u32(c, 0x8B3063F0u);
            arm64_emit_u32(c, 0x9100021Fu);
        }
    }
    arm64_emit_u32(c, 0xF84107FEu);  /* ldr x30, [sp], #16 */
}

static void aarch64_emit_leave(ZeroCCCompiler *c) {
    if (c->lowering.is_frameless) return;
    if (c->lowering.omit_frame_pointer) {
        aarch64_emit_fp_teardown(c);
        return;
    }
    /* 1. mov sp, x29
     * 2. ldp x29, x30, [sp], #16
     */
    arm64_emit_u32(c, 0x910003BF);  /* mov sp, x29 */
    arm64_emit_u32(c, 0xA8C17BFD);  /* ldp x29, x30, [sp], #16 */
}

/* Assembly emission functions for text output - defined in aarch64_asm.c */
/* static void aarch64_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) { ... } */
/* static void aarch64_emit_epilogue_asm(ZeroCCCompiler *c) { ... } */
/* static const char *aarch64_get_hw_reg_name(int phys_reg) { ... } */

static size_t aarch64_get_code_offset(ZeroCCCompiler *c) {
    return me_get_offset(c);
}

static void aarch64_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
    uint64_t v = (uint64_t)imm;
    uint32_t w0 = (uint32_t)(v & 0xFFFF);
    uint32_t w1 = (uint32_t)((v >> 16) & 0xFFFF);
    uint32_t w2 = (uint32_t)((v >> 32) & 0xFFFF);
    uint32_t w3 = (uint32_t)((v >> 48) & 0xFFFF);
    int r = dst_reg & 0x1F;
    arm64_emit_u32(c, 0xD2800000 | (w0 << 5) | r);  /* movz */
    if (w1 || w2 || w3) arm64_emit_u32(c, 0xF2A00000 | (w1 << 5) | r);  /* movk lsl 16 */
    if (w2 || w3)       arm64_emit_u32(c, 0xF2C00000 | (w2 << 5) | r);  /* movk lsl 32 */
    if (w3)             arm64_emit_u32(c, 0xF2E00000 | (w3 << 5) | r);  /* movk lsl 48 */
}

static void aarch64_emit_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* add dst, src1, src2 */
    arm64_emit_u32(c, 0x8B000000 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* sub dst, src1, src2 */
    arm64_emit_u32(c, 0xCB000000 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* mul dst, src1, src2 */
    arm64_emit_u32(c, 0x9B007C00 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* udiv/sdiv dst, src1, src2 */
    arm64_emit_u32(c, 0x9AC00800 | (src2 << 16) | (src1 << 5) | dst);  /* udiv */
}

static void aarch64_emit_and(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm64_emit_u32(c, 0x8A000000 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_or(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm64_emit_u32(c, 0xAA000000 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_xor(ZeroCCCompiler *c, int dst, int src1, int src2) {
    arm64_emit_u32(c, 0xCA000000 | (src2 << 16) | (src1 << 5) | dst);
}

static void aarch64_emit_shl(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* lsl dst, src1, src2: LSLV (variable shift) */
    arm64_emit_u32(c, 0x9AC02000 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* cmp src1, src2 */
    arm64_emit_u32(c, 0xEB00001F | (src2 << 16) | (src1 << 5));
}

static void aarch64_emit_neg(ZeroCCCompiler *c, int dst, int src) {
    /* neg dst, src */
    arm64_emit_u32(c, 0xCB000000 | (src << 16) | (31 << 5) | dst);  /* sub dst, xzr, src */
}

/* In-place width extension: SXTB/SXTH/SXTW or UXTB/UXTH/UXTW dst, src */
static void aarch64_emit_extend(ZeroCCCompiler *c, int dst, int src, int width, int sign) {
    /* AArch64 sign/zero-extend encodings, verified against `aarch64-linux-gnu-as`
     * for every (dst,src) in 0..30 (Sprint 41). The SXTB/SXTH/SXTW/UXTB/UXTH forms
     * place the source (Rn) at bits[9:5]; UXTW is the MOV(Xd,Wn) form with Rn at
     * bits[20:16] (writing a 32-bit value zero-extends it into the 64-bit dest). */
    uint32_t insn;
    if (width == 4) {
        if (sign)
            insn = 0x93407C00u | (dst & 0x1F) | ((src & 0x1F) << 5);   /* SXTW Xd, Wn */
        else
            insn = 0x2A0003E0u | (dst & 0x1F) | ((src & 0x1F) << 16);  /* UXTW Xd, Wn (MOV Xd, Wn) */
    } else if (width == 2) {
        insn = (sign ? 0x13003C00u : 0x53003C00u) | (dst & 0x1F) | ((src & 0x1F) << 5); /* SXTH/UXTH Wd, Wn */
    } else {
        insn = (sign ? 0x13001C00u : 0x53001C00u) | (dst & 0x1F) | ((src & 0x1F) << 5); /* SXTB/UXTB Wd, Wn */
    }
    arm64_emit_u32(c, insn);
}

static void aarch64_emit_mov_reg(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    /* mov dst, src - implemented as orr dst, xzr, src */
    arm64_emit_u32(c, 0xAA0003E0 | (src << 16) | (31 << 5) | dst);  /* orr dst, xzr, src */
}

/* not dst, src - bitwise NOT */
static void aarch64_emit_not(ZeroCCCompiler *c, int dst, int src) {
    /* mvn dst, src (move NOT) = orn dst, xzr, src */
    /* orn: 0xAA2003E0 | (src << 16) | dst */
    arm64_emit_u32(c, 0xAA2003E0 | ((src & 0x1F) << 16) | (dst & 0x1F));
}

/* shr dst, src1, src2 - logical right shift */
static void aarch64_emit_shr(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* lsr dst, src1, src2: LSRV */
    arm64_emit_u32(c, 0x9AC02400 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

/* deref - load from memory address in register */
static void aarch64_emit_deref(ZeroCCCompiler *c, int dst, int addr) {
    /* ldr dst, [addr] - use LDUR for unscaled offset 0 */
    arm64_emit_u32(c, 0xF8400000 | ((addr & 0x1F) << 5) | (dst & 0x1F));
}

/* load_32 dst, addr - load 32-bit value */
static void aarch64_emit_load_32(ZeroCCCompiler *c, int dst, int addr) {
    /* ldrw dst, [addr] */
    arm64_emit_u32(c, 0xB9400000 | (addr << 5) | dst);
}

/* load_16 dst, addr - load 16-bit value (zero-extended) */
static void aarch64_emit_load_16(ZeroCCCompiler *c, int dst, int addr) {
    /* ldrh dst, [addr] - halfword load */
    arm64_emit_u32(c, 0x79400000 | ((addr & 0x1F) << 5) | (dst & 0x1F));
}

/* load_byte dst, addr - load 8-bit value (zero-extended) */
static void aarch64_emit_load_byte(ZeroCCCompiler *c, int dst, int addr) {
    /* ldrb dst, [addr] - byte load */
    arm64_emit_u32(c, 0x39400000 | ((addr & 0x1F) << 5) | (dst & 0x1F));
}

/* store_32 value, addr - store 32-bit value */
static void aarch64_emit_store_32(ZeroCCCompiler *c, int value, int addr) {
    /* strw value, [addr] */
    arm64_emit_u32(c, 0xB9000000 | (addr << 5) | value);
}

/* store_16 value, addr - store 16-bit value */
static void aarch64_emit_store_16(ZeroCCCompiler *c, int value, int addr) {
    /* strh value, [addr] */
    arm64_emit_u32(c, 0x79000000 | ((addr & 0x1F) << 5) | (value & 0x1F));
}

/* store_byte value, addr - store 8-bit value */
static void aarch64_emit_store_byte(ZeroCCCompiler *c, int value, int addr) {
    /* strb value, [addr] */
    arm64_emit_u32(c, 0x39000000 | ((addr & 0x1F) << 5) | (value & 0x1F));
}

/* lea_rip dst, offset - load address relative to instruction pointer */
static void aarch64_emit_lea_rip(ZeroCCCompiler *c, int dst, size_t offset) {
    int r = dst & 0x1F;
    arm64_emit_u32(c, 0x90000000u | (uint32_t)r);  /* adrp dst, 0 */
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x91000000u | ((uint32_t)r << 5) | (uint32_t)r);  /* add dst, dst, 0 */
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = offset;
        c->str_relocs[c->str_reloc_count].dst_reg = dst;
        c->str_reloc_count++;
    }
}

/* cmp_zero src - compare with zero */
static void aarch64_emit_cmp_zero(ZeroCCCompiler *c, int src) {
    /* cmp src, xzr */
    arm64_emit_u32(c, 0xEB00001F | (31 << 16) | (src << 5));
}

/* setl dst - set byte if less than (signed) */
static void aarch64_emit_setl(ZeroCCCompiler *c, int dst) {
    /* cset dst, lt (CSINC dst, xzr, xzr, ge) */
    arm64_emit_u32(c, 0x9A9FA7E0 | (dst & 0x1F));
}

/* setle dst - set byte if less or equal (signed) */
static void aarch64_emit_setle(ZeroCCCompiler *c, int dst) {
    /* cset dst, le (CSINC dst, xzr, xzr, gt) */
    arm64_emit_u32(c, 0x9A9FC7E0 | (dst & 0x1F));
}

/* setg dst - set byte if greater than (signed) */
static void aarch64_emit_setg(ZeroCCCompiler *c, int dst) {
    /* cset dst, gt (CSINC dst, xzr, xzr, le) */
    arm64_emit_u32(c, 0x9A9FD7E0 | (dst & 0x1F));
}

/* setge dst - set byte if greater or equal (signed) */
static void aarch64_emit_setge(ZeroCCCompiler *c, int dst) {
    /* cset dst, ge (CSINC dst, xzr, xzr, lt) */
    arm64_emit_u32(c, 0x9A9FB7E0 | (dst & 0x1F));
}

/* sete dst - set byte if equal */
static void aarch64_emit_sete(ZeroCCCompiler *c, int dst) {
    /* cset dst, eq (CSINC dst, xzr, xzr, ne) */
    arm64_emit_u32(c, 0x9A9F17E0 | (dst & 0x1F));
}

/* setne dst - set byte if not equal */
static void aarch64_emit_setne(ZeroCCCompiler *c, int dst) {
    /* cset dst, ne (CSINC dst, xzr, xzr, eq) */
    arm64_emit_u32(c, 0x9A9F07E0 | (dst & 0x1F));
}

/* Unified conditional set based on IR condition code
 * IR condition codes:
 *   CC_LT = 0
 *   CC_LE = 1
 *   CC_GT = 2
 *   CC_GE = 3
 *   CC_EQ = 4
 *   CC_NE = 5
 *
 * CSET Xd, cond -> CSINC Xd, XZR, XZR, !cond
 * Bits 15:12 hold the INVERTED condition code.
 */
static void aarch64_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    static const uint32_t cset_encodings[] = {
        0x9A9FA7E0,  /* CC_LT: cset Xd, lt (CSINC Xd, XZR, XZR, ge) */
        0x9A9FC7E0,  /* CC_LE: cset Xd, le (CSINC Xd, XZR, XZR, gt) */
        0x9A9FD7E0,  /* CC_GT: cset Xd, gt (CSINC Xd, XZR, XZR, le) */
        0x9A9FB7E0,  /* CC_GE: cset Xd, ge (CSINC Xd, XZR, XZR, lt) */
        0x9A9F17E0,  /* CC_EQ: cset Xd, eq (CSINC Xd, XZR, XZR, ne) */
        0x9A9F07E0,  /* CC_NE: cset Xd, ne (CSINC Xd, XZR, XZR, eq) */
        0x9A9F27E0,  /* CC_FLT_LT/ULT: cset Xd, lo (CSINC Xd, XZR, XZR, hs) */
        0x9A9F87E0,  /* CC_FLT_LE/ULE: cset Xd, ls (CSINC Xd, XZR, XZR, hi) */
        0x9A9F97E0,  /* CC_FLT_GT/UGT: cset Xd, hi (CSINC Xd, XZR, XZR, ls) */
        0x9A9F37E0,  /* CC_FLT_GE/UGE: cset Xd, hs (CSINC Xd, XZR, XZR, lo) */
    };

    if (cond < 0 || cond > 9) {
        fprintf(stderr, "Invalid condition code: %d\n", cond);
        abort();
    }

    arm64_emit_u32(c, cset_encodings[cond] | (dst & 0x1F));
}

/* Unified conditional jump based on IR condition code
 * IR condition codes map directly to B.cond bits [3:0]:
 *   CC_LT = 0 -> b.lt (0xB)
 *   CC_LE = 1 -> b.le (0xD)
 *   CC_GT = 2 -> b.gt (0xC)
 *   CC_GE = 3 -> b.ge (0xA)
 *   CC_EQ = 4 -> b.eq (0x0)
 *   CC_NE = 5 -> b.ne (0x1)
 */
static size_t aarch64_emit_jcond(ZeroCCCompiler *c, int cond, const char *label) {
    static const uint32_t b_cond_encodings[] = {
        0x5400000B,  /* CC_LT: b.lt */
        0x5400000D,  /* CC_LE: b.le */
        0x5400000C,  /* CC_GT: b.gt */
        0x5400000A,  /* CC_GE: b.ge */
        0x54000000,  /* CC_EQ: b.eq */
        0x54000001,  /* CC_NE: b.ne */
    };

    if (cond < 0 || cond > 5) {
        fprintf(stderr, "Invalid condition code: %d\n", cond);
        abort();
    }

    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, b_cond_encodings[cond]);
    (void)label;
    return patch_pos;
}

/* lnot dst, src - logical NOT (dst = src == 0 ? 1 : 0) */
static void aarch64_emit_lnot(ZeroCCCompiler *c, int dst, int src) {
    /* cmp src, #0 */
    aarch64_emit_cmp_zero(c, src);
    /* cset dst, eq (inverted: 0x1 at bits [15:12]) */
    aarch64_emit_sete(c, dst);
}

/* param_save_int - save integer parameter (or x8 sret when param_idx == 8) */
static void aarch64_emit_param_save_int(ZeroCCCompiler *c, int param_idx, int32_t offset) {
    if (param_idx < 0 || param_idx > 8) return; /* Stack params handled by caller */
    int src_reg = (param_idx == 8) ? 8 : aarch64_int_arg_regs[param_idx];
    int base = zero_cc_frame_base_reg(c);
    int32_t imm = zero_cc_frame_disp(c, -offset); /* [base, #imm] */
    if (imm < -255 || imm > 255) {
        /* Outside stur's +/-255 range: materialize the address in x9. */
        if (imm <= 4095) {
            arm64_emit_u32(c, 0x91000529u | ((uint32_t)imm << 10));  /* add x9, sp, #imm */
        } else {
            aarch64_emit_mov_imm(c, 16, imm);
            arm64_emit_u32(c, 0x8B000000u | (16u << 16) | (31u << 5) | 9u);  /* add x9, sp, x16 */
        }
        arm64_emit_u32(c, 0xF8000000 | (9u << 5) | (src_reg & 0x1F));  /* stur xN, [x9] */
        return;
    }
    /* stur x<src_reg>, [base, #imm] */
    arm64_emit_u32(c, 0xF8000000 | ((uint32_t)imm & 0x1FF) << 12 | ((uint32_t)base << 5) | (src_reg & 0x1F));
}

/* param_save_fp - save FP parameter */
static void aarch64_emit_param_save_fp(ZeroCCCompiler *c, int param_idx, int32_t offset) {
    if (param_idx < 0 || param_idx >= 8) return;
    int src_reg = aarch64_fp_arg_regs[param_idx];
    int base = zero_cc_frame_base_reg(c);
    int32_t imm = zero_cc_frame_disp(c, -offset); /* [base, #imm] */
    if (imm < -255 || imm > 255) {
        if (imm <= 4095) {
            arm64_emit_u32(c, 0x91000529u | ((uint32_t)imm << 10));  /* add x9, sp, #imm */
        } else {
            aarch64_emit_mov_imm(c, 16, imm);
            arm64_emit_u32(c, 0x8B000000u | (16u << 16) | (31u << 5) | 9u);  /* add x9, sp, x16 */
        }
        arm64_emit_u32(c, 0xFC000000 | (9u << 5) | (src_reg & 0x1F));  /* stur dN, [x9] */
        return;
    }
    /* stur d<src_reg>, [base, #imm] */
    arm64_emit_u32(c, 0xFC000000 | ((uint32_t)imm & 0x1FF) << 12 | ((uint32_t)base << 5) | (src_reg & 0x1F));
}

/* Forward decls (defined further below) for the variadic spill. */
static void aarch64_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);
static void aarch64_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);

/* variadic_spill - spill ALL argument registers (x0..x7, then f0..f7 as
 * 64-bit doubles) into the 16-slot save area. va_start points the streams
 * just past the slots consumed by named parameters, so the spill is
 * unconditional. (Was a no-op: variadic args were never accessible via
 * va_arg on AArch64.) */
static void aarch64_emit_variadic_spill(ZeroCCCompiler *c, int count, int32_t offset) {
    (void)count;
    int base = zero_cc_frame_base_reg(c);
    int gp_regs = BE(c)->conv->int_arg_reg_count;
    int fp_regs = BE(c)->conv->fp_arg_reg_count;
    for (int i = 0; i < gp_regs; i++) {
        aarch64_emit_store(c, i, base, -(offset + (int32_t)i * 8));
    }
    for (int i = 0; i < fp_regs; i++) {
        aarch64_emit_store_fp(c, i, base, -(offset + (int32_t)(gp_regs + i) * 8));
    }
}

/* variadic_al_setup - setup for variadic argument access */
static void aarch64_emit_variadic_al_setup(ZeroCCCompiler *c, int num_fp_args) {
    /* No-op for AArch64 */
    (void)c; (void)num_fp_args;
}

/* vla_alloc - variable length array allocation */
static void aarch64_emit_vla_alloc(ZeroCCCompiler *c, int dst_reg, int size_reg) {
    /* sub dst, sp, size */
    arm64_emit_u32(c, 0xCB0003E0 | (size_reg << 16) | 31 | dst_reg);
}

/* atomic_cmpxchg - atomic compare and exchange */
static void aarch64_emit_atomic_cmpxchg(ZeroCCCompiler *c, int dst, int addr, int expected, int desired, int width) {
    (void)width; /* fixed-width 32-bit exclusive sequence below */
    /* ldxr dst, [addr] (load exclusive) */
    arm64_emit_u32(c, 0x885F7C00 | (addr << 5) | dst);
    /* cmp dst, expected */
    arm64_emit_u32(c, 0xEB00001F | (expected << 16) | (dst << 5));
    /* b.ne skip (placeholder) */
    arm64_emit_u32(c, 0x54000000);
    /* stxr wtmp, desired, [addr] (store exclusive) */
    arm64_emit_u32(c, 0x88007C00 | (addr << 5) | desired);
}

/* atomic_xadd - atomic add and return old value */
static void aarch64_emit_atomic_xadd(ZeroCCCompiler *c, int dst, int addr, int value, int width) {
    (void)width; /* fixed-width 32-bit exclusive sequence below */
    /* ldxr dst, [addr] */
    arm64_emit_u32(c, 0x885F7C00 | (addr << 5) | dst);
    /* add tmp, dst, value */
    arm64_emit_u32(c, 0x8B000000 | (value << 16) | (dst << 5) | dst);
    /* stlxr wtmp, dst, [addr] */
    arm64_emit_u32(c, 0x881F7C00 | (addr << 5) | dst);
}

/* atomic_xchg - atomic exchange */
static void aarch64_emit_atomic_xchg(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    /* ldaxr dst, [addr] (load acquire exclusive) */
    arm64_emit_u32(c, 0x887F7C00 | (addr_reg << 5) | dst_reg);
    /* stlxr wtmp, val, [addr] (store release exclusive) */
    arm64_emit_u32(c, 0x881F7C00 | (addr_reg << 5) | val_reg);
    (void)width;
}

/* mfence - memory fence */
static void aarch64_emit_mfence(ZeroCCCompiler *c) {
    /* dmb ish (data memory barrier) */
    arm64_emit_u32(c, 0xD5033FB5);
}

static void aarch64_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* Load from [base_reg + offset]
     * For positive 8-byte aligned offsets (0 to 4095): use LDR (scaled)
     * For negative offsets (-256 to 255): use LDUR (unscaled)
     */
    if (offset >= 0 && offset <= 4095 && (offset % 8) == 0) {
        /* LDR (64-bit, scaled): 0xF9400000 | ((offset/8) << 10) | (base << 5) | dst */
        arm64_emit_u32(c, 0xF9400000 | (((offset / 8) & 0xFFF) << 10) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else if (offset >= -256 && offset <= 255) {
        /* LDUR (64-bit, unscaled): 0xF8400000 | (imm9<<12) | (base<<5) | dst */
        /* imm9 is 9-bit signed offset in two's complement */
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xF8400000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else {
        /* For large offsets, need to load address into register first */
        /* Simplified: use LDUR with truncation (may be incorrect for very large offsets) */
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xF8400000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    }
}

static void aarch64_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* Store to [base_reg + offset]
     * For positive 8-byte aligned offsets (0 to 4095): use STR (scaled)
     * For negative offsets (-256 to 255): use STUR (unscaled)
     */
    if (offset >= 0 && offset <= 4095 && (offset % 8) == 0) {
        /* STR (64-bit, scaled): 0xF9000000 | ((offset/8) << 10) | (base << 5) | src */
        arm64_emit_u32(c, 0xF9000000 | (((offset / 8) & 0xFFF) << 10) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    } else if (offset >= -256 && offset <= 255) {
        /* STUR (64-bit, unscaled): 0xF8000000 | (imm9<<12) | (base<<5) | src */
        /* imm9 is 9-bit signed offset in two's complement */
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xF8000000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    } else {
        /* For large offsets, need to load address into register first */
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xF8000000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    }
}

static size_t aarch64_emit_jmp(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    /* b label - placeholder for patching */
    arm64_emit_u32(c, 0x14000000);  /* b . (placeholder) */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jeq(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0x0);  /* b.eq */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jne(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0x1);  /* b.ne */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jlt(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0xB);  /* b.lt (signed) */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jle(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0xD);  /* b.le (signed) */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jgt(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0xC);  /* b.gt (signed) */
    (void)label;
    return patch_pos;
}

static size_t aarch64_emit_jge(ZeroCCCompiler *c, const char *label) {
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x54000000 | 0xA);  /* b.ge (signed) */
    (void)label;
    return patch_pos;
}

static void aarch64_emit_call(ZeroCCCompiler *c, const char *func_name) {
    /* bl label */
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x94000000);  /* bl . (placeholder) */

    /* Add relocation entry for linker to patch */
    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = patch_pos;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", func_name);
        r->is_call = true;
    }
}

static void aarch64_emit_ret(ZeroCCCompiler *c) {
    /* ret */
    arm64_emit_u32(c, 0xD65F03C0);
}

/* sar dst_reg, src1, src2 (arithmetic shift right: dst = src1 >> src2) */
static void aarch64_emit_sar(ZeroCCCompiler *c, int dst_reg, int src1, int src2) {
    /* asr dst, src1, src2: ASRV */
    arm64_emit_u32(c, 0x9AC02800 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst_reg & 0x1F));
}

/* call [reg] - call through register */
static void aarch64_emit_call_reg(ZeroCCCompiler *c, int reg) {
    /* blr reg */
    arm64_emit_u32(c, 0xD63F0000 | (reg << 5));
}

/* stack_align - ensure 16-byte stack alignment (AArch64 already handles this) */
static void aarch64_emit_stack_align(ZeroCCCompiler *c, uint8_t alignment) {
    /* AArch64 ABI requires 16-byte alignment.
     * This is typically handled by prologue/epilogue. */
    (void)c;
    (void)alignment;
}

/* add_imm dst_reg, imm (add immediate) */
static void aarch64_emit_add_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    if (imm >= 0) {
        /* add dst, dst, #imm - Rn=Rd=dst_reg */
        arm64_emit_u32(c, 0x91000000 | (((uint32_t)imm & 0xFFF) << 10) | ((dst_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else {
        /* sub dst, dst, #-imm - Rn=Rd=dst_reg */
        arm64_emit_u32(c, 0xD1000000 | (((uint32_t)(-imm) & 0xFFF) << 10) | ((dst_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    }
}

/* sub_imm dst_reg, imm (subtract immediate) */
static void aarch64_emit_sub_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    if (imm >= 0) {
        /* sub dst, dst, #imm - Rn=Rd=dst_reg */
        arm64_emit_u32(c, 0xD1000000 | (((uint32_t)imm & 0xFFF) << 10) | ((dst_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else {
        /* add dst, dst, #-imm - Rn=Rd=dst_reg */
        arm64_emit_u32(c, 0x91000000 | (((uint32_t)(-imm) & 0xFFF) << 10) | ((dst_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    }
}

/* inc dst_reg (increment) */
static void aarch64_emit_inc(ZeroCCCompiler *c, int dst_reg) {
    /* add dst, dst, #1 */
    arm64_emit_u32(c, 0x91000400 | (dst_reg << 5) | dst_reg);
}

/* dec dst_reg (decrement) */
static void aarch64_emit_dec(ZeroCCCompiler *c, int dst_reg) {
    /* sub dst, dst, #1 */
    arm64_emit_u32(c, 0xD1000400 | (dst_reg << 5) | dst_reg);
}

/* Floating point operations */
static void aarch64_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* ldr dX, [base, #offset] - scalar double-precision load
     * For positive 8-byte aligned offsets (0 to 4095): use LDR (scaled by 8)
     * For negative offsets (-256 to 255): use LDUR (unscaled)
     */
    if (offset >= 0 && offset <= 4095 && (offset % 8) == 0) {
        /* LDR D (scaled): 0xFD400000 | ((offset/8) << 10) | (base << 5) | dst */
        arm64_emit_u32(c, 0xFD400000 | (((offset / 8) & 0xFFF) << 10) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else if (offset >= -256 && offset <= 255) {
        /* LDUR D (unscaled): 0xFC400000 | (imm9<<12) | (base<<5) | dst */
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xFC400000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else {
        /* For large offsets, use LDUR with truncation */
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xFC400000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    }
}

static void aarch64_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* str dX, [base, #offset] - scalar double-precision store
     * For positive 8-byte aligned offsets (0 to 4095): use STR (scaled by 8)
     * For negative offsets (-256 to 255): use STUR (unscaled)
     */
    if (offset >= 0 && offset <= 4095 && (offset % 8) == 0) {
        /* STR D (scaled): 0xFD000000 | ((offset/8) << 10) | (base << 5) | src */
        arm64_emit_u32(c, 0xFD000000 | (((offset / 8) & 0xFFF) << 10) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    } else if (offset >= -256 && offset <= 255) {
        /* STUR D (unscaled): 0xFC000000 | (imm9<<12) | (base<<5) | src */
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xFC000000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    } else {
        /* For large offsets, use STUR with truncation */
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xFC000000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    }
}

static void aarch64_emit_load_fp_rodata(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset) {
    int tmp = 16;  /* x16 */
    arm64_emit_u32(c, 0x90000000u | (uint32_t)tmp);  /* adrp x16, 0 */
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x91000000u | ((uint32_t)tmp << 5) | (uint32_t)tmp);  /* add x16, x16, 0 */
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_relocs[c->str_reloc_count].dst_reg = tmp;
        c->str_reloc_count++;
    }
    arm64_emit_u32(c, 0xFD400200 | (dst_reg & 0x1F));  /* ldr d_dst, [x16] */
}

static void aarch64_emit_load_single_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    if (offset >= 0 && offset <= 4095 && (offset % 4) == 0) {
        arm64_emit_u32(c, 0xBD400000 | (((offset / 4) & 0xFFF) << 10) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else if (offset >= -256 && offset <= 255) {
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xBC400000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else {
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xBC400000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    }
    /* Widen single to double: fcvt d<dst>, s<dst> */
    arm64_emit_u32(c, 0x1E22C000 | ((dst_reg & 0x1F) << 5) | (dst_reg & 0x1F));
}

static void aarch64_emit_store_single_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    int tmp_fp = 16;  /* s16 */
    arm64_emit_u32(c, 0x1E624000 | ((src_reg & 0x1F) << 5) | tmp_fp);  /* fcvt s16, d<src> */
    if (offset >= 0 && offset <= 4095 && (offset % 4) == 0) {
        arm64_emit_u32(c, 0xBD000000 | (((offset / 4) & 0xFFF) << 10) | ((base_reg & 0x1F) << 5) | tmp_fp);
    } else if (offset >= -256 && offset <= 255) {
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xBC000000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | tmp_fp);
    } else {
        uint32_t imm9 = (offset >= 0) ? (uint32_t)offset : (uint32_t)(512 + offset);
        arm64_emit_u32(c, 0xBC000000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | tmp_fp);
    }
}

static void aarch64_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fadd dD, dN, dM - scalar double-precision */
    arm64_emit_u32(c, 0x1E602800 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_fp_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fsub dD, dN, dM - scalar double-precision */
    arm64_emit_u32(c, 0x1E603800 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_fp_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fmul dD, dN, dM - scalar double-precision */
    arm64_emit_u32(c, 0x1E600800 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_fp_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fdiv dD, dN, dM - scalar double-precision */
    arm64_emit_u32(c, 0x1E601800 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_fp_neg(ZeroCCCompiler *c, int dst, int src) {
    /* fneg dD, dN - scalar double-precision */
    arm64_emit_u32(c, 0x1E614000 | ((src & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_fp_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* fcmp dN, dM - scalar double-precision */
    arm64_emit_u32(c, 0x1E602000 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5));
}

static void aarch64_emit_fp_cvtsi(ZeroCCCompiler *c, int int_dst, int fp_src) {
    /* fcvtzs x<dst>, d<src> (convert double to 64-bit signed int) */
    arm64_emit_u32(c, 0x9E780000 | ((fp_src & 0x1F) << 5) | (int_dst & 0x1F));
}

static void aarch64_emit_fp_cvtsf(ZeroCCCompiler *c, int fp_dst, int int_src) {
    /* scvtf d<dst>, x<src> (convert 64-bit signed int to double) */
    arm64_emit_u32(c, 0x9E620000 | ((int_src & 0x1F) << 5) | (fp_dst & 0x1F));
}

static void aarch64_emit_fp_mov(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    /* fmov dD, dN - scalar double-precision move */
    arm64_emit_u32(c, 0x1E604000 | ((src & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_const(ZeroCCCompiler *c, int64_t value) {
    aarch64_emit_mov_imm(c, 0, value);
}

static void aarch64_emit_stack_adjust(ZeroCCCompiler *c, int32_t delta) {
    if (delta == 0) return;

    /* ADD/SUB (immediate): unscaled 12-bit byte offset
     * Base for add sp, sp, #0: 0x910003FF
     * Base for sub sp, sp, #0: 0xD10003FF
     * Rn = Rd = sp = 31, imm12 in bits 21:10
     */
    if (delta > 0) {
        arm64_emit_u32(c, 0x910003FF | (((uint32_t)delta & 0xFFF) << 10));
    } else {
        arm64_emit_u32(c, 0xD10003FF | (((uint32_t)(-delta) & 0xFFF) << 10));
    }
}

static void aarch64_emit_push_reg(ZeroCCCompiler *c, int reg) {
    /* stp reg, x16, [sp, #-16]! */
    /* Use x16 (IP0 - caller-saved scratch) as second register */
    /* Base: 0xA9BF07E0 for stp x0, x1, [sp, #-16]! */
    /* Clear Rt (bits 4:0) and Rt2 (bits 14:10), set to reg and 16 */
    arm64_emit_u32(c, (0xA9BF07E0 & ~((0x1F << 10) | 0x1F)) | (16 << 10) | (reg & 0x1F));
}

static void aarch64_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    /* ldp reg, x16, [sp], #16 */
    /* Base: 0xA8C17FE0 for ldp x0, x1, [sp], #16 */
    /* Clear Rt (bits 4:0) and Rt2 (bits 14:10), set to reg and 16 */
    arm64_emit_u32(c, (0xA8C17FE0 & ~((0x1F << 10) | 0x1F)) | (16 << 10) | (reg & 0x1F));
}

static void aarch64_emit_label(ZeroCCCompiler *c, const char *name) {
    (void)c; (void)name;
}

static inline void aarch64_emit_fp_sqrt(ZeroCCCompiler *c, int dst, int src) {
    /* fsqrt dD, dN - scalar double-precision */
    arm64_emit_u32(c, 0x1E61C000 | ((src & 0x1F) << 5) | (dst & 0x1F));
}



static size_t aarch64_patch_jmp(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset) {
    /* ARM64 branch instruction encoding:
     * - Unconditional B/BL: 26-bit signed offset (bits 25:0)
     * - Conditional B.COND: 19-bit signed offset (bits 23:5), condition in bits [3:0]
     */
    int32_t rel = (int32_t)((target_offset - patch_offset) >> 2);

    if (patch_offset + 4 <= c->text_section.length) {
        uint8_t *buf = (uint8_t *)c->text_section.data;
        uint32_t *insn = (uint32_t*)(buf + patch_offset);

        /* Check if this is a conditional branch (0x54xxxxxx) or unconditional (0x14xxxxxx, 0x94xxxxxx) */
        if ((*insn & 0xFF000010) == 0x54000000) {
            /* B.COND: 19-bit offset in bits [23:5], preserve condition in bits [3:0] */
            *insn = (*insn & 0xFF00001F) | ((rel & 0x7FFFF) << 5);
        } else {
            /* Unconditional B/BL: 26-bit offset */
            *insn = (*insn & 0xFC000000) | (rel & 0x03FFFFFF);
        }
    }
    return patch_offset;
}

/* Resolve all label patches for AArch64 */
static void aarch64_resolve_labels(ZeroCCCompiler *c) {
    for (int i = 0; i < c->label_count; i++) {
        size_t target = c->labels[i].offset;
        for (int j = 0; j < c->labels[i].patch_count; j++) {
            size_t patch_offset = c->labels[i].patches[j];

            if (patch_offset + 4 <= c->text_section.length) {
                uint8_t *buf = (uint8_t *)c->text_section.data;
                uint32_t *insn = (uint32_t*)(buf + patch_offset);
                int32_t rel = (int32_t)((target - patch_offset) >> 2);

                /* Check if this is a conditional branch (0x54xxxxxx) or unconditional */
                if ((*insn & 0xFF000010) == 0x54000000) {
                    /* B.COND: 19-bit offset in bits [23:5], preserve condition in bits [3:0] */
                    *insn = (*insn & 0xFF00001F) | ((rel & 0x7FFFF) << 5);
                } else {
                    /* Unconditional B/BL: 26-bit offset */
                    *insn = (*insn & 0xFC000000) | (rel & 0x03FFFFFF);
                }
            }
        }
    }
}



static void aarch64_emit_test(ZeroCCCompiler *c, int src1, int src2) {
    /* tst Xn, Xm -> ands xzr, Xn, Xm (64-bit) */
    arm64_emit_u32(c, 0xEA00001F | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5));
}

static void aarch64_emit_cqo(ZeroCCCompiler *c) {
    (void)c;
}

static void aarch64_emit_idiv(ZeroCCCompiler *c, int src) {
    /* sdiv X0, X0, Xn */
    arm64_emit_u32(c, 0x9AC00800 | ((src & 0x1F) << 16) | 0x0);
}

static void aarch64_emit_lea(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* lea dst, [base + offset]
     * ADD (immediate): 12-bit unscaled offset, 0 to 4095
     * SUB (immediate): 12-bit unscaled offset, 0 to 4095
     */
    if (offset >= 0 && offset <= 4095) {
        /* add dst, base, #offset */
        arm64_emit_u32(c, 0x91000000 | (((uint32_t)offset & 0xFFF) << 10) | ((base & 0x1F) << 5) | (dst & 0x1F));
    } else if (offset >= -4096 && offset < 0) {
        /* sub dst, base, #-offset */
        arm64_emit_u32(c, 0xD1000000 | (((uint32_t)(-offset) & 0xFFF) << 10) | ((base & 0x1F) << 5) | (dst & 0x1F));
    } else {
        /* Large offset: load immediate then add */
        aarch64_emit_mov_imm(c, dst, offset);
        arm64_emit_u32(c, 0x8B000000 | ((base & 0x1F) << 16) | ((dst & 0x1F) << 5) | (dst & 0x1F));
    }
}



static void aarch64_emit_mod(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* Modulo: dst = src1 % src2
     * ARM64: tmp = src1 / src2; dst = src1 - tmp * src2
     * Use x16 (IP0 - caller-saved scratch) as temporary */
    int tmp = 16; /* x16 */

    /* tmp = src1 / src2 (sdiv tmp, src1, src2)
     * sdiv: 0x9AC00800 | (Rm << 16) | (Rn << 5) | Rd */
    arm64_emit_u32(c, 0x9AC00800 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (tmp & 0x1F));

    /* dst = src1 - (tmp * src2) (msub dst, tmp, src2, src1)
     * msub: 0x9B008000 | (Rm << 16) | (Ra << 10) | (Rn << 5) | Rd */
    arm64_emit_u32(c, 0x9B008000 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 10) | ((tmp & 0x1F) << 5) | (dst & 0x1F));
}

/* ARM64 backend instance */

/* AArch64 VREG mapping: direct pass-through (VREG numbers = ARM encodings) */
static int aarch64_get_int_reg(int vreg) { return vreg; }
static int aarch64_get_fp_reg(int vreg) { return vreg; }

ZeroCCISABackend aarch64_backend = {  /* writable: ASM emit pointers are filled in lazily */
    .isa = ISA_AARCH64,
    .regs = &aarch64_regs,
    .conv = &aarch64_conv,
    .fp_reg = 29,  /* x29 */
    .sp_reg = 31,  /* sp */
    .scratch_reg1 = 9,                         /* x9 (non-argument/return) */
    .scratch_reg2 = 10,                        /* x10 (non-argument/return) */
    .fp_scratch_reg1 = REGALLOC_FP_SCRATCH1,  /* d15 - outside XMM pool 0-7 */
    .fp_scratch_reg2 = REGALLOC_FP_SCRATCH2,   /* d14 - outside XMM pool 0-7 */
    .callee_save_frame_reserve = 96,           /* 12 callee-saved regs: x19-x30 */

    .emit_prologue = aarch64_emit_prologue,
    .emit_leave = aarch64_emit_leave,
    .emit_epilogue = aarch64_emit_epilogue,
    .get_code_offset = aarch64_get_code_offset,

    .emit_prologue_asm = aarch64_emit_prologue_asm,
    .emit_epilogue_asm = aarch64_emit_epilogue_asm,
    .get_hw_reg_name = aarch64_get_hw_reg_name,
    .get_vec_reg_name = aarch64_get_vec_reg_name,

    .emit_mov_imm = aarch64_emit_mov_imm,
    .emit_add = aarch64_emit_add,
    .emit_sub = aarch64_emit_sub,
    .emit_mul = aarch64_emit_mul,
    .emit_div = aarch64_emit_div,
    .emit_mod = aarch64_emit_mod,
    .emit_and = aarch64_emit_and,
    .emit_or = aarch64_emit_or,
    .emit_xor = aarch64_emit_xor,
    .emit_shl = aarch64_emit_shl,
    .emit_cmp = aarch64_emit_cmp,
    .emit_cond_set = aarch64_emit_cond_set,
    .emit_neg = aarch64_emit_neg,
    .emit_extend = aarch64_emit_extend,
    .emit_mov_reg = aarch64_emit_mov_reg,
    .emit_not = aarch64_emit_not,
    .emit_shr = aarch64_emit_shr,
    .emit_test = aarch64_emit_test,
    .emit_cqo = aarch64_emit_cqo,
    .emit_idiv = aarch64_emit_idiv,
    .emit_lea = aarch64_emit_lea,
    .patch_jmp = aarch64_patch_jmp,
    .resolve_labels = aarch64_resolve_labels,

    /* VREG mapping */
    .get_int_reg = aarch64_get_int_reg,
    .get_fp_reg = aarch64_get_fp_reg,

    .emit_load = aarch64_emit_load,
    .emit_store = aarch64_emit_store,

    .emit_deref = aarch64_emit_deref,
    .emit_load_32 = aarch64_emit_load_32,
    .emit_load_16 = aarch64_emit_load_16,
    .emit_load_byte = aarch64_emit_load_byte,
    .emit_store_32 = aarch64_emit_store_32,
    .emit_store_16 = aarch64_emit_store_16,
    .emit_store_byte = aarch64_emit_store_byte,
    .emit_lea_rip = aarch64_emit_lea_rip,

    .emit_jmp = aarch64_emit_jmp,
    .emit_jeq = aarch64_emit_jeq,
    .emit_jne = aarch64_emit_jne,
    .emit_jlt = aarch64_emit_jlt,
    .emit_jle = aarch64_emit_jle,
    .emit_jgt = aarch64_emit_jgt,
    .emit_jge = aarch64_emit_jge,
    .emit_jcond = aarch64_emit_jcond,
    .emit_setl = aarch64_emit_setl,
    .emit_setle = aarch64_emit_setle,
    .emit_setg = aarch64_emit_setg,
    .emit_setge = aarch64_emit_setge,
    .emit_sete = aarch64_emit_sete,
    .emit_setne = aarch64_emit_setne,
    .emit_cmp_zero = aarch64_emit_cmp_zero,
    .emit_lnot = aarch64_emit_lnot,
    .emit_call = aarch64_emit_call,
    .emit_call_reg = aarch64_emit_call_reg,
    .emit_ret = aarch64_emit_ret,

    .emit_load_fp = aarch64_emit_load_fp,
    .emit_store_fp = aarch64_emit_store_fp,
    .emit_vloadsd = aarch64_emit_load_fp,
    .emit_vstoredsd = aarch64_emit_store_fp,
    .emit_vloadss = aarch64_emit_load_single_fp,
    .emit_vstoress = aarch64_emit_store_single_fp,
    .emit_load_fp_rodata = aarch64_emit_load_fp_rodata,
    .emit_fp_add = aarch64_emit_fp_add,
    .emit_fp_sub = aarch64_emit_fp_sub,
    .emit_fp_mul = aarch64_emit_fp_mul,
    .emit_fp_div = aarch64_emit_fp_div,
    .emit_fp_neg = aarch64_emit_fp_neg,
    .emit_fp_cmp = aarch64_emit_fp_cmp,
    .emit_fp_cvtsi = aarch64_emit_fp_cvtsi,
    .emit_fp_cvtsf = aarch64_emit_fp_cvtsf,
    .emit_fp_mov = aarch64_emit_fp_mov,

    .emit_const = aarch64_emit_const,
    .emit_stack_adjust = aarch64_emit_stack_adjust,
    .emit_stack_align = aarch64_emit_stack_align,
    .emit_push_reg = aarch64_emit_push_reg,
    .emit_pop_reg = aarch64_emit_pop_reg,
    .emit_add_imm = aarch64_emit_add_imm,
    .emit_sub_imm = aarch64_emit_sub_imm,
    .emit_inc = aarch64_emit_inc,
    .emit_dec = aarch64_emit_dec,
    .emit_sar = aarch64_emit_sar,
    .emit_label = aarch64_emit_label,

    /* ABI and special operations */
    .emit_param_save_int = aarch64_emit_param_save_int,
    .emit_param_save_fp = aarch64_emit_param_save_fp,
    .emit_variadic_spill = aarch64_emit_variadic_spill,
    .emit_variadic_al_setup = aarch64_emit_variadic_al_setup,
    .emit_vla_alloc = aarch64_emit_vla_alloc,
    .emit_atomic_cmpxchg = aarch64_emit_atomic_cmpxchg,
    .emit_atomic_xadd = aarch64_emit_atomic_xadd,
    .emit_atomic_xchg = aarch64_emit_atomic_xchg,
    .emit_mfence = aarch64_emit_mfence,

    /* Assembly emission for IR instructions (Option A) */
    .emit_mov_imm_asm = aarch64_emit_mov_imm_asm,
    .emit_mov_reg_asm = aarch64_emit_mov_reg_asm,
    .emit_load_asm = aarch64_emit_load_asm,
    .emit_store_asm = aarch64_emit_store_asm,
    .emit_add_asm = aarch64_emit_add_asm,
    .emit_sub_asm = aarch64_emit_sub_asm,
    .emit_mul_asm = aarch64_emit_mul_asm,
    .emit_div_asm = aarch64_emit_div_asm,
    .emit_and_asm = aarch64_emit_and_asm,
    .emit_or_asm = aarch64_emit_or_asm,
    .emit_xor_asm = aarch64_emit_xor_asm,
    .emit_shl_asm = aarch64_emit_shl_asm,
    .emit_shr_asm = aarch64_emit_shr_asm,
    .emit_sar_asm = aarch64_emit_sar_asm,
    .emit_extend_asm = aarch64_emit_extend_asm,
    .emit_cmp_asm = aarch64_emit_cmp_asm,
    .emit_cond_set_asm = aarch64_emit_cond_set_asm,
    .emit_neg_asm = aarch64_emit_neg_asm,
    .emit_lea_asm = aarch64_emit_lea_asm,
    .emit_deref_asm = aarch64_emit_deref_asm,
    .emit_call_asm = aarch64_emit_call_asm,
    .emit_ret_asm = aarch64_emit_ret_asm,
    .emit_label_asm = aarch64_emit_label_asm,
    .emit_jmp_asm = aarch64_emit_jmp_asm,
    .emit_jeq_asm = aarch64_emit_jeq_asm,
    .emit_jne_asm = aarch64_emit_jne_asm,
    .emit_jlt_asm = aarch64_emit_jlt_asm,
    .emit_jle_asm = aarch64_emit_jle_asm,
    .emit_jgt_asm = aarch64_emit_jgt_asm,
    .emit_jge_asm = aarch64_emit_jge_asm,
    .emit_load_fp_asm = aarch64_emit_load_fp_asm,
    .emit_store_fp_asm = aarch64_emit_store_fp_asm,
    .emit_fp_add_asm = aarch64_emit_fp_add_asm,
    .emit_fp_sub_asm = aarch64_emit_fp_sub_asm,
    .emit_fp_mul_asm = aarch64_emit_fp_mul_asm,
    .emit_fp_div_asm = aarch64_emit_fp_div_asm,
    .emit_fp_mov_asm = aarch64_emit_fp_mov_asm,
    .emit_vla_alloc_asm = aarch64_emit_vla_alloc_asm,
    .emit_param_save_int_asm = aarch64_emit_param_save_int_asm,
    .emit_param_save_fp_asm = aarch64_emit_param_save_fp_asm,
};
/* =========================================================================
 * Virtual Register (VREG) Mapping for AArch64
 * =========================================================================
 *
 * The lowering code uses virtual registers defined in ../types/vregs.h.
 * Each VREG number is interpreted as the AArch64 hardware encoding:
 *
 *   VREG_0  = 0 = x0    (result / return value / 1st arg)
 *   VREG_1  = 1 = x1    (temp / 2nd arg)
 *   VREG_2  = 2 = x2    (temp / 3rd arg)
 *   VREG_3  = 3 = x3    (temp / 4th arg)
 *   VREG_4  = 4 = x4    (temp / 5th arg)
 *   VREG_5  = 5 = x5    (temp / 6th arg)
 *   VREG_6  = 6 = x6    (temp / 7th arg)
 *   VREG_7  = 7 = x7    (temp / 8th arg)
 *   VREG_8  = 8 = x8    (temp / return address register)
 *   VREG_9  = 9 = x9    (temp / caller-saved)
 *   VREG_10 = 10 = x10  (temp / caller-saved)
 *   VREG_11 = 11 = x11  (temp / caller-saved)
 *   VREG_12 = 12 = x12  (temp / caller-saved)
 *   VREG_13 = 13 = x13  (temp / caller-saved)
 *   VREG_14 = 14 = x14  (temp / caller-saved)
 *   VREG_15 = 15 = x15  (temp / caller-saved)
 *
 * Float registers (VREG_F0, VREG_F1, ...):
 *   VREG_F0 = 0 = d0    (float result / return value / 1st float arg)
 *   VREG_F1 = 1 = d1    (float temp / 2nd float arg)
 *   VREG_F2 = 2 = d2    (float temp / 3rd float arg)
 *   ...
 *
 * Special registers:
 *   BE(c)->sp_reg = 31 (sp) - use this instead of VREG_31
 *   BE(c)->fp_reg = 29 (fp/x29) - use this instead of VREG_29
 *
 * Note: The lowering code should use VREG_0-VREG_15 for general purposes,
 * and BE(c)->sp_reg / BE(c)->fp_reg for stack/frame pointers.
 */
