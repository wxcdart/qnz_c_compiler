#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/isa/tramp_stub.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/ir/asm_emitter.h"
#include "zero_cc/isa/aarch64_asm.c"  /* Include assembly emission functions */
#include <stdint.h>
#include <stdlib.h>

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

    if (c->lowering.frame_align > 16) {
        /* C11 _Alignas(N) realign mode: lower SP to the N-aligned base
         * SP' = align_down(E-16, N). SP cannot be the destination of a
         * logical op, so the mask is applied in x16 (IP0, free at entry)
         * and the gap subtracted. All encodings verified against GAS.
         * Slots are [SP' - k] via frame_delta = frame; the epilogue is
         * the plain FP-mode leave (x29 holds the pre-realign base). */
        uint32_t N = (uint32_t)c->lowering.frame_align;
        uint32_t mask_insn;
        /* Encodings of `and x16, x16, #-(N)`. CRITICAL: the immediate must be
         * the 64-bit mask form (GAS `#-N`, e.g. 0xFFFFFFFFFFFFFFE0). A 32-bit
         * literal (`#0xffffffe0`) encodes a pattern that fits in 32 bits and
         * is executed with zero-extension into x16, destroying SP's upper
         * half (qemu-aarch64 matches real hardware here). */
        switch (N) {
            case 32:   mask_insn = 0x927BEA10u; break;  /* and x16, x16, #-32   */
            case 64:   mask_insn = 0x927AE610u; break;  /* and x16, x16, #-64   */
            case 128:  mask_insn = 0x9279E210u; break;  /* and x16, x16, #-128  */
            case 256:  mask_insn = 0x9278DE10u; break;  /* and x16, x16, #-256  */
            case 512:  mask_insn = 0x9277DA10u; break;  /* and x16, x16, #-512  */
            case 1024: mask_insn = 0x9276D610u; break;  /* and x16, x16, #-1024 */
            case 2048: mask_insn = 0x9275D210u; break;  /* and x16, x16, #-2048 */
            default:   mask_insn = 0x9274CE10u; break;  /* and x16, x16, #-4096 */
        }
        arm64_emit_u32(c, 0xA9BF7BFDu);  /* stp x29, x30, [sp, #-16]! */
        arm64_emit_u32(c, 0x910003FDu);  /* mov x29, sp */
        arm64_emit_u32(c, 0x910003F0u);  /* mov x16, sp (add x16, sp, #0) */
        arm64_emit_u32(c, mask_insn);     /* and x16, x16, #~(N-1) */
        arm64_emit_u32(c, 0xCB1003B0u);  /* sub x16, x29, x16 */
        arm64_emit_u32(c, 0xCB3063FFu);  /* sub sp, sp, x16 */
        size_t frame = (size_t)c->lowering.frame_delta;
        if (frame > 4095) {
            aarch64_emit_mov_imm(c, 17, frame);  /* x17: x16 is consumed above */
            arm64_emit_u32(c, 0xCB3163FFu);      /* sub sp, sp, x17 */
        } else {
            arm64_emit_u32(c, 0xD10003FF | (((uint32_t)frame & 0xFFF) << 10));  /* sub sp, sp, #frame */
        }
        return;
    }

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
        if (c->lowering.omit_frame_pointer && c->lowering.frame_align <= 16) {
            /* Option 4: raise SP by the frame, restore x30, ret.
             * (_Alignas realign mode keeps the FP-mode leave below: x29
             * holds the pre-realign base, so `mov sp, x29; ldp` restores
             * entry state exactly regardless of the realign amount.) */
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
    if (c->lowering.omit_frame_pointer && c->lowering.frame_align <= 16) {
        aarch64_emit_fp_teardown(c);
        return;
    }
    /* 1. mov sp, x29
     * 2. ldp x29, x30, [sp], #16 */
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
    if ((dst & 0x1F) == 31 || (src & 0x1F) == 31) {
        /* mov involving sp (31) is an alias of add Rd, Rn, #0 (orr uses xzr for 31) */
        arm64_emit_u32(c, 0x91000000u | (((uint32_t)src & 0x1Fu) << 5) | ((uint32_t)dst & 0x1Fu));
        return;
    }
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

/* store_64 value, addr - store 64-bit value */
static void aarch64_emit_store_64(ZeroCCCompiler *c, int value, int addr) {
    /* str value, [addr] */
    arm64_emit_u32(c, 0xF9000000 | ((addr & 0x1F) << 5) | (value & 0x1F));
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
        0x9A9F57E0,  /* CC_FLT_LT: cset Xd, mi (N=1) */
        0x9A9F87E0,  /* CC_FLT_LE: cset Xd, ls (C=0 OR Z=1) */
        0x9A9FD7E0,  /* CC_FLT_GT: cset Xd, gt (Z=0 AND N=V) */
        0x9A9FB7E0,  /* CC_FLT_GE: cset Xd, ge (N=V) */
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

/* Forward decls for stores and variadic spill. */
static void aarch64_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);
static void aarch64_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);
static void aarch64_emit_store_single_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);
static void aarch64_emit_store_single_fp_raw(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);  /* Sprint 115m */

/* param_save_int - save integer parameter (or x8 sret when param_idx == 8) */
static void aarch64_emit_param_save_int(ZeroCCCompiler *c, int param_idx, int32_t offset) {
    if (param_idx < 0 || param_idx > 8) return; /* Stack params handled by caller */
    int src_reg = (param_idx == 8) ? 8 : aarch64_int_arg_regs[param_idx];
    int base = zero_cc_frame_base_reg(c);
    int32_t imm = zero_cc_frame_disp(c, -offset); /* [base, #imm] */
    aarch64_emit_store(c, src_reg, base, imm);
}

/* param_save_fp - save FP parameter */
static void aarch64_emit_param_save_fp(ZeroCCCompiler *c, int param_idx, int32_t offset, int is_single) {
    if (param_idx < 0 || param_idx >= 8) return;
    int src_reg = aarch64_fp_arg_regs[param_idx];
    int base = zero_cc_frame_base_reg(c);
    int32_t imm = zero_cc_frame_disp(c, -offset); /* [base, #imm] */
    if (is_single) {
        /* Sprint 115m: raw low-32 store - an external caller's float param
         * arrives as a float pattern in the low 32 of the register; the old
         * fcvt-narrowing store destroyed it. */
        aarch64_emit_store_single_fp_raw(c, src_reg, base, imm);
    } else {
        aarch64_emit_store_fp(c, src_reg, base, imm);
    }
}

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
        int r = BE(c)->conv->int_arg_regs[i];
        aarch64_emit_store(c, r, base, -(offset + (int32_t)i * 8));
    }
    for (int i = 0; i < fp_regs; i++) {
        int r = BE(c)->conv->fp_arg_regs[i];
        aarch64_emit_store_fp(c, r, base, -(offset + (int32_t)(gp_regs + i) * 8));
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

static void aarch64_emit_atomic_or(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    (void)width;
    arm64_emit_u32(c, 0x885F7C00 | ((addr & 0x1F) << 5) | (dst & 0x1F));
    arm64_emit_u32(c, 0xAA000000 | ((val & 0x1F) << 16) | ((dst & 0x1F) << 5) | 16);
    arm64_emit_u32(c, 0x881F7C00 | ((addr & 0x1F) << 5) | 16);
}

static void aarch64_emit_atomic_and(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    (void)width;
    arm64_emit_u32(c, 0x885F7C00 | ((addr & 0x1F) << 5) | (dst & 0x1F));
    arm64_emit_u32(c, 0x8A000000 | ((val & 0x1F) << 16) | ((dst & 0x1F) << 5) | 16);
    arm64_emit_u32(c, 0x881F7C00 | ((addr & 0x1F) << 5) | 16);
}

static void aarch64_emit_atomic_xor(ZeroCCCompiler *c, int dst, int addr, int val, int width) {
    (void)width;
    arm64_emit_u32(c, 0x885F7C00 | ((addr & 0x1F) << 5) | (dst & 0x1F));
    arm64_emit_u32(c, 0xCA000000 | ((val & 0x1F) << 16) | ((dst & 0x1F) << 5) | 16);
    arm64_emit_u32(c, 0x881F7C00 | ((addr & 0x1F) << 5) | 16);
}

static void aarch64_emit_atomic_cmpxchg_bool(ZeroCCCompiler *c, int dst, int addr, int expected, int desired, int width) {
    aarch64_emit_atomic_cmpxchg(c, dst, addr, expected, desired, width);
    /* cset dst, eq */
    arm64_emit_u32(c, 0x9A9F17E0 | (dst & 0x1F));
}

/* mfence - memory fence */
static void aarch64_emit_mfence(ZeroCCCompiler *c) {
    /* dmb ish (data memory barrier) */
    arm64_emit_u32(c, 0xD5033FB5);
}

/* __builtin_trap / __builtin_unreachable */
static void aarch64_emit_trap(ZeroCCCompiler *c) {
    /* brk #0 */
    arm64_emit_u32(c, 0xAA000000);
}

static void aarch64_emit_trap_asm(ZeroCCCompiler *c, FILE *out) {
    (void)c;
    fprintf(out, "\tbrk #0\n");
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
        uint32_t imm9 = ((uint32_t)offset) & 0x1FF;
        arm64_emit_u32(c, 0xF8400000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else {
        /* Large offset: LDR (register) [base, x16]. In this addressing mode
         * X31=SP is legal, whereas in register ADD/SUB Rn=31 encodes XZR
         * (an `add x16, sp, x16` is therefore invalid). */
        aarch64_emit_mov_imm(c, 16, offset);
        arm64_emit_u32(c, 0xF8606800u | (16u << 16) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
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
        uint32_t imm9 = ((uint32_t)offset) & 0x1FF;
        arm64_emit_u32(c, 0xF8000000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    } else {
        /* Large offset: STR (register) [base, x16] (X31=SP is legal here;
         * see aarch64_emit_load). */
        aarch64_emit_mov_imm(c, 16, offset);
        arm64_emit_u32(c, 0xF8206800u | (16u << 16) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
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
    } else {
        zero_cc_reloc_overflow_note(c);
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

/* jmp [reg] - indirect jump through register */
static void aarch64_emit_jmp_reg(ZeroCCCompiler *c, int reg) {
    /* br reg */
    arm64_emit_u32(c, 0xD61F0000 | ((reg & 0x1F) << 5));
}

/* adr dst_reg, label - load label address PC-relative */
static size_t aarch64_emit_lea_label(ZeroCCCompiler *c, int dst_reg) {
    size_t patch_pos = me_get_offset(c);
    uint32_t insn = 0x10000000 | (dst_reg & 0x1F); /* ADR dst_reg, 0 */
    arm64_emit_u32(c, insn);
    return patch_pos;
}

/* OS syscall (Linux AArch64 ABI): number in x8, args 1-6 in x0-x5, result
 * in x0 (= VREG_0, no fix-up needed). The kernel clobbers x0-x17 (x18 is
 * preserved), so every clobbered register that may hold a caller value is
 * saved before and restored after `svc #0`: x1-x7 and x11-x15 (allocatable
 * temps; x9/x10 are scratch, x16-x18 reserved) and x8 - not allocatable,
 * but the linker's _start stub parks the exit_group number there across
 * main, so it must survive the syscall too. Same caller-saved-fix idiom as
 * the OP_CALL lowering. 13 x 8-byte saves = 104 bytes, so one extra
 * 8-byte pad push keeps SP 16-byte aligned the way the kernel requires at
 * `svc #0` entry (popped first after the call, LIFO). */
static void aarch64_emit_syscall(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    static const int linux_syscall_regs[7] = {8, 0, 1, 2, 3, 4, 5}; /* x8, x0-x5 */
    static const int clobbered_live_regs[] = {1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14, 15};
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    int n = (arg_count < 7) ? arg_count : 7;
    /* Save the live values the kernel would clobber. */
    for (size_t r = 0; r < sizeof(clobbered_live_regs) / sizeof(clobbered_live_regs[0]); r++) {
        if (BE(c)->emit_push_reg) BE(c)->emit_push_reg(c, clobbered_live_regs[r]);
    }
    if (BE(c)->emit_push_reg) BE(c)->emit_push_reg(c, 16 /* x16 */); /* 16B alignment pad */
    int src_regs[8] = {0};
    /* Pass 1: resolve every source (spill reloads into scratch_reg2), push
     * the ones that still need a move. */
    for (int a = 0; a < n; a++) {
        int temp = arg_temps[a];
        int src_reg;
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg2, BE(c)->fp_reg, -off);
            src_reg = BE(c)->scratch_reg2;
        } else {
            src_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        }
        src_regs[a] = src_reg;
        if (src_reg != linux_syscall_regs[a] && BE(c)->emit_push_reg) {
            BE(c)->emit_push_reg(c, src_reg);
        }
    }
    /* Pass 2: pop into the syscall registers (reverse order - LIFO). */
    for (int a = n - 1; a >= 0; a--) {
        if (src_regs[a] == linux_syscall_regs[a]) continue;
        if (BE(c)->emit_pop_reg) BE(c)->emit_pop_reg(c, linux_syscall_regs[a]);
    }
    arm64_emit_u32(c, 0xD4000001); /* svc #0 */
    /* Restore the saved registers (reverse order - LIFO; pad first). */
    if (BE(c)->emit_pop_reg) BE(c)->emit_pop_reg(c, 16 /* x16 */);
    for (size_t r = sizeof(clobbered_live_regs) / sizeof(clobbered_live_regs[0]); r-- > 0; ) {
        if (BE(c)->emit_pop_reg) BE(c)->emit_pop_reg(c, clobbered_live_regs[r]);
    }
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
        uint32_t imm9 = ((uint32_t)offset) & 0x1FF;
        arm64_emit_u32(c, 0xFC400000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else {
        /* Large offset: LDR D (register) [base, x16] (X31=SP is legal in
         * this addressing mode; register ADD cannot use SP). */
        aarch64_emit_mov_imm(c, 16, offset);
        arm64_emit_u32(c, 0xFC606800u | (16u << 16) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
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
        uint32_t imm9 = ((uint32_t)offset) & 0x1FF;
        arm64_emit_u32(c, 0xFC000000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    } else {
        /* Large offset: STR D (register) [base, x16] (see aarch64_emit_load). */
        aarch64_emit_mov_imm(c, 16, offset);
        arm64_emit_u32(c, 0xFC206800u | (16u << 16) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
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
        uint32_t imm9 = ((uint32_t)offset) & 0x1FF;
        arm64_emit_u32(c, 0xBC400000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else {
        /* Large offset: LDR S (register) [base, x16] (X31=SP is legal in
         * this addressing mode; register ADD cannot use SP). */
        aarch64_emit_mov_imm(c, 16, offset);
        arm64_emit_u32(c, 0xBC606800u | (16u << 16) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
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
        uint32_t imm9 = ((uint32_t)offset) & 0x1FF;
        arm64_emit_u32(c, 0xBC000000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | tmp_fp);
    } else {
        /* Large offset: STR S (register) [base, x16] (see aarch64_emit_load). */
        aarch64_emit_mov_imm(c, 16, offset);
        arm64_emit_u32(c, 0xBC206800u | (16u << 16) | ((base_reg & 0x1F) << 5) | tmp_fp);
    }
}

/* Sprint 115m: raw 4-byte ABI-float access - the register's low 32 bits ARE
 * the float pattern; no fcvt through the double-width scalar model. */

/* LDR S without the trailing fcvt widen. */
static void aarch64_emit_load_single_fp_raw(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    if (offset >= 0 && offset <= 4095 && (offset % 4) == 0) {
        arm64_emit_u32(c, 0xBD400000 | (((offset / 4) & 0xFFF) << 10) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else if (offset >= -256 && offset <= 255) {
        uint32_t imm9 = ((uint32_t)offset) & 0x1FF;
        arm64_emit_u32(c, 0xBC400000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    } else {
        aarch64_emit_mov_imm(c, 16, offset);
        arm64_emit_u32(c, 0xBC606800u | (16u << 16) | ((base_reg & 0x1F) << 5) | (dst_reg & 0x1F));
    }
}

/* STR S without the leading fcvt narrow (stores the register's low 32). */
static void aarch64_emit_store_single_fp_raw(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    if (offset >= 0 && offset <= 4095 && (offset % 4) == 0) {
        arm64_emit_u32(c, 0xBD000000 | (((offset / 4) & 0xFFF) << 10) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    } else if (offset >= -256 && offset <= 255) {
        uint32_t imm9 = ((uint32_t)offset) & 0x1FF;
        arm64_emit_u32(c, 0xBC000000 | (imm9 << 12) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    } else {
        aarch64_emit_mov_imm(c, 16, offset);
        arm64_emit_u32(c, 0xBC206800u | (16u << 16) | ((base_reg & 0x1F) << 5) | (src_reg & 0x1F));
    }
}

/* fcvt s<dst>, d<src> - model double -> ABI float (in-register narrow). */
static void aarch64_emit_fp_cvttsd2ss(ZeroCCCompiler *c, int dst, int src) {
    arm64_emit_u32(c, 0x1E624000 | ((src & 0x1F) << 5) | (dst & 0x1F));
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

static void aarch64_emit_fp_cvtss2sd(ZeroCCCompiler *c, int dst, int src) {
    /* fcvt d<dst>, s<src> - widen single to double (GNU vector per-lane) */
    arm64_emit_u32(c, 0x1E22C000 | ((dst & 0x1F) << 5) | (src & 0x1F));
}

static void aarch64_emit_fp_cvtsd2ss(ZeroCCCompiler *c, int dst, int src) {
    /* fcvt s<dst>, d<src> - narrow double to single (GNU vector per-lane) */
    arm64_emit_u32(c, 0x1E624000 | ((dst & 0x1F) << 5) | (src & 0x1F));
}

static void aarch64_emit_fp_cvtsi2ss(ZeroCCCompiler *c, int fp_dst, int int_src) {
    /* scvtf s<dst>, w<src> */
    arm64_emit_u32(c, 0x1E220000 | ((int_src & 0x1F) << 5) | (fp_dst & 0x1F));
}

static void aarch64_emit_vload(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    aarch64_emit_load_fp(c, dst, base, offset);
}

static void aarch64_emit_vstore(ZeroCCCompiler *c, int src, int base, int32_t offset) {
    aarch64_emit_store_fp(c, src, base, offset);
}

static void aarch64_emit_vaddps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fadd v<dst>.4s, v<src1>.4s, v<src2>.4s */
    arm64_emit_u32(c, 0x4E20D400 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vsubps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fsub v<dst>.4s, v<src1>.4s, v<src2>.4s */
    arm64_emit_u32(c, 0x4EA0D400 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vmulps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fmul v<dst>.4s, v<src1>.4s, v<src2>.4s */
    arm64_emit_u32(c, 0x6E20DC00 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vdivps(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fdiv v<dst>.4s, v<src1>.4s, v<src2>.4s */
    arm64_emit_u32(c, 0x6E20FC00 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vaddpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fadd v<dst>.2d, v<src1>.2d, v<src2>.2d */
    arm64_emit_u32(c, 0x4E60D400 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vsubpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fsub v<dst>.2d, v<src1>.2d, v<src2>.2d */
    arm64_emit_u32(c, 0x4EE0D400 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vmulpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fmul v<dst>.2d, v<src1>.2d, v<src2>.2d */
    arm64_emit_u32(c, 0x6E60DC00 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vdivpd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* fdiv v<dst>.2d, v<src1>.2d, v<src2>.2d */
    arm64_emit_u32(c, 0x6E60FC00 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vpaddd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* add v<dst>.4s, v<src1>.4s, v<src2>.4s */
    arm64_emit_u32(c, 0x4EA08400 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vpsubd(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* sub v<dst>.4s, v<src1>.4s, v<src2>.4s */
    arm64_emit_u32(c, 0x6EA08400 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vpmulld(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* mul v<dst>.4s, v<src1>.4s, v<src2>.4s */
    arm64_emit_u32(c, 0x4EA09C00 | ((src2 & 0x1F) << 16) | ((src1 & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_vmovaps(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    /* orr v<dst>.16b, v<src>.16b, v<src>.16b */
    arm64_emit_u32(c, 0x4EA01C00 | ((src & 0x1F) << 16) | ((src & 0x1F) << 5) | (dst & 0x1F));
}

static void aarch64_emit_fp_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* fcmpe dN, dM - scalar double-precision (signaling NaN compare)
     * fcmpe raises Invalid Operation for QNaN, setting V=1 on unordered.
     * This enables IEEE-correct condition codes (mi/ls/gt/ge) for FP.
     * Base is 0x1E602000 (fcmpe d0,d0) - the Rm (second operand) field
     * is bits 16-19 and must NOT be preset: with 0x1E612000, a src2 of 0
     * (or any even reg) silently compared against the wrong register
     * (c-torture/isa_qemu 07_float cvti clamp regression). */
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
    /* str reg, [sp, #-8]!  (exactly 8 bytes - the OP_CALL lowering's
     * stack-arg cleanup and extra_sp tracking both assume 8-byte
     * pushes; the old 16-byte `stp reg, x16` pair left garbage words
     * interleaved in the caller-arg area of every >8-arg call). */
    arm64_emit_u32(c, 0xF81F8FE0u | (uint32_t)(reg & 0x1F));
}

static void aarch64_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    /* ldr reg, [sp], #8 */
    arm64_emit_u32(c, 0xF84087E0u | (uint32_t)(reg & 0x1F));
}

static void aarch64_emit_label(ZeroCCCompiler *c, const char *name) {
    (void)c; (void)name;
}

static inline void aarch64_emit_fp_sqrt(ZeroCCCompiler *c, int dst, int src) {
    /* fsqrt dD, dN - scalar double-precision */
    arm64_emit_u32(c, 0x1E61C000 | ((src & 0x1F) << 5) | (dst & 0x1F));
}



static size_t aarch64_patch_jmp(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset) {
    if (patch_offset + 4 <= c->text_section.length) {
        uint8_t *buf = (uint8_t *)c->text_section.data;
        uint32_t *insn = (uint32_t*)(buf + patch_offset);

        if ((*insn & 0x9F000000) == 0x10000000) {
            /* ADR Rd, label: 21-bit signed byte offset */
            int32_t byte_delta = (int32_t)(target_offset - patch_offset);
            *insn = (*insn & 0x9F00001F) | ((byte_delta & 3) << 29) | (((byte_delta >> 2) & 0x7FFFF) << 5);
        } else if ((*insn & 0xFF000010) == 0x54000000) {
            /* B.COND: 19-bit offset in bits [23:5], preserve condition in bits [3:0] */
            int32_t rel = (int32_t)((target_offset - patch_offset) >> 2);
            *insn = (*insn & 0xFF00001F) | ((rel & 0x7FFFF) << 5);
        } else {
            /* Unconditional B/BL: 26-bit offset */
            int32_t rel = (int32_t)((target_offset - patch_offset) >> 2);
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
     * ADD/SUB (immediate): 12-bit unscaled offset, 0 to 4095, or
     * 12-bit x 4096 (sh bit, covers up to 4095*4096 = 16 MiB).
     * CRITICAL: SP cannot be the Rm of an ADD (register) - encoding
     * register 31 there is xzr, so `add dst, dst, sp` silently becomes
     * `add dst, dst, xzr`. Large offsets must therefore stay in the
     * immediate form, never mov+add-reg. */
    if (offset >= 0 && offset <= 4095) {
        /* add dst, base, #offset */
        arm64_emit_u32(c, 0x91000000 | (((uint32_t)offset & 0xFFF) << 10) | ((base & 0x1F) << 5) | (dst & 0x1F));
    } else if (offset >= -4096 && offset < 0) {
        /* sub dst, base, #-offset */
        arm64_emit_u32(c, 0xD1000000 | (((uint32_t)(-offset) & 0xFFF) << 10) | ((base & 0x1F) << 5) | (dst & 0x1F));
    } else if (offset > 4095 && offset % 4096 == 0 && offset <= 4095 * 4096) {
        /* add dst, base, #offset/4096, LSL #12 */
        uint32_t imm12 = (uint32_t)(offset / 4096) & 0xFFF;
        arm64_emit_u32(c, 0x91000000 | (1u << 22) | (imm12 << 10) | ((base & 0x1F) << 5) | (dst & 0x1F));
    } else if (offset < -4096 && offset % 4096 == 0 && -offset <= 4095 * 4096) {
        /* sub dst, base, #-offset/4096, LSL #12 */
        uint32_t imm12 = (uint32_t)(-offset / 4096) & 0xFFF;
        arm64_emit_u32(c, 0xD1000000 | (1u << 22) | (imm12 << 10) | ((base & 0x1F) << 5) | (dst & 0x1F));
    } else if (base == c->isa_backend->sp_reg) {
        /* SP base with an arbitrary large offset: add the low 12 bits,
         * then the remainder (multiple of 4096, via LSL #12) in a
         * second immediate add (dst is a GPR here, so this is legal). */
        int32_t lo = offset % 4096;          /* -4095..4095 */
        int32_t hi = offset - lo;            /* multiple of 4096 */
        if (lo >= 0) {
            arm64_emit_u32(c, 0x91000000 | ((uint32_t)lo << 10) | ((base & 0x1F) << 5) | (dst & 0x1F));
        } else {
            arm64_emit_u32(c, 0xD1000000 | ((uint32_t)(-lo) << 10) | ((base & 0x1F) << 5) | (dst & 0x1F));
        }
        uint32_t imm12 = (uint32_t)(abs(hi) / 4096) & 0xFFF;
        if (hi >= 0) {
            arm64_emit_u32(c, 0x91000000 | (1u << 22) | (imm12 << 10) | ((dst & 0x1F) << 5) | (dst & 0x1F));
        } else {
            arm64_emit_u32(c, 0xD1000000 | (1u << 22) | (imm12 << 10) | ((dst & 0x1F) << 5) | (dst & 0x1F));
        }
    } else {
        /* Non-SP base (e.g. x29) with a large offset: load immediate then
         * register-add (SP is the only register illegal as Rm here). */
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

/* =====================================================================
 * Phase 3b (cross-ISA): trampoline stub for a capturing nested function.
 *
 * `fp = f2; fp(x)` cannot call the mangled nested function directly: its
 * ABI has the hidden capture parameters. The stub (emitted right after
 * the nested fn in the same text image) takes the USER arguments, loads
 * the captures from the static ctx object, re-places everything in the
 * nested fn's argument slots and `bl`s the real function. The real fn's
 * `ret` lands on the epilogue below, returning to the ORIGINAL caller.
 *
 * AAPCS64 streams: GPR units take consecutive x0..x7 slots, FP units
 * d0..d7 (complex = 2). >16B struct/union = one GPR unit (hidden
 * pointer - zerocc's convention, matches the prologue). A >16B by-value
 * STRUCT capture is copied from ctx into the stub's frame and its
 * address is passed. A >16B struct RETURN adds nothing: the sret
 * pointer travels in x8 (Indirect Result Location Register) and is
 * never touched by the stub.
 *
 * Frame layout (x29 = entry_sp - 16, stp x29,x30,[sp,#-16]!):
 *   [x29+16]            LR (saved by the prologue, NOT pushed - bl links
 *                       into x30 and leaves SP untouched, like jal)
 *   [x29+16+8m]         entry stack unit m (the caller's call stack;
 *                       stack arg 0 sits at [entry_sp])
 *   [x29-8-8g]          entry GPR arg saves (x0..x7)
 *   [x29-80-16f]        entry FP arg saves (d0..d7)
 *   [x29-208-8s]        entry stack unit saves
 *   [x29-208-8m-32c-8w] >16B by-value capture copies (word w of c)
 * Scratch: x9 = ctx base, x10/x11 = GPR temps, d9/d10 = FP temps.
 * ===================================================================== */

/* ldr dst, [x29, #-off] - 8B or FP (wide = d register), off <= 2048.
 * LDUR/STUR: base | imm9[20:12] | Rn[9:5] | Rt - covers -256..255
 * directly; deeper offsets go through a `sub x10` base. (The bases
 * 0xF84/0xF80/0xFC4/0xFC0 are the LDUR/STUR family - NOT the LDR/STR
 * unsigned-offset 0xF94/0xF90/0xFD4/0xFC0...0xF8C family, which is
 * 12-bit 8x-scaled.) */
static void tramp64_ldr_neg(ZeroCCCompiler *c, int dst, uint32_t off, bool fp) {
    if (off <= 256) {
        arm64_emit_u32(c, (fp ? 0xFC400000u : 0xF8400000u) |
                          (((0x200u - off) & 0x1FFu) << 12) | (29u << 5) | (uint32_t)(dst & 0x1F));
    } else {
        arm64_emit_u32(c, 0xD1000000u | ((off & 0xFFFu) << 10) | (29u << 5) | 10u); /* sub x10, x29, #off */
        arm64_emit_u32(c, (fp ? 0xFD400000u : 0xF9400000u) | (10u << 5) | (uint32_t)(dst & 0x1F));
    }
}

/* str src, [x29, #-off] - 8B or FP (wide = d register), off <= 2048 */
static void tramp64_str_neg(ZeroCCCompiler *c, int src, uint32_t off, bool fp) {
    if (off <= 256) {
        arm64_emit_u32(c, (fp ? 0xFC000000u : 0xF8000000u) |
                          (((0x200u - off) & 0x1FFu) << 12) | (29u << 5) | (uint32_t)(src & 0x1F));
    } else {
        arm64_emit_u32(c, 0xD1000000u | ((off & 0xFFFu) << 10) | (29u << 5) | 10u); /* sub x10, x29, #off */
        arm64_emit_u32(c, (fp ? 0xFD000000u : 0xF9000000u) | (10u << 5) | (uint32_t)(src & 0x1F));  /* str src, [x10] */
    }
}

void zero_cc_aarch64_emit_tramp_stub(ZeroCCCompiler *c, const ZeroCCFunction *nf) {
    size_t C = nf->nested_captured_count;
    size_t U = nf->param_count - C;

    /* ---- build the final unit list: captures + user params ---- */
    ZeroCCTrampUnit u[ZERO_CC_TRAMP_MAX_UNITS];

    /* ---- pass 1: entry geometry of the USER call's units ----
     * The user call marshals the user params exactly like zerocc's own
     * CALL lowering: GPR/FP streams in declaration order up to the 8-
     * register cap, overflow units into ONE shared stack stream in
     * declaration order (the real fn's prologue reads them at
     * [frame + 16 + 8*stack_idx]). The stub sees the user args at
     * entry: regs x0..x7 / d0..d7, stack unit k at [x29+16+8k] (bl
     * links into x30 and leaves SP untouched, so stack arg 0 = [sp]).
     * NOTE: register slot and shared-stack index are SEPARATE namespaces
     * - a single table cannot encode both (an overflow unit's shared idx
     * 0..7 would collide with entry register slots 0..7). */
    bool ue_isfp[ZERO_CC_TRAMP_MAX_UNITS];
    int  ue_reg[ZERO_CC_TRAMP_MAX_UNITS];   /* reg slot 0..7, or -1 */
    int  ue_stk[ZERO_CC_TRAMP_MAX_UNITS];   /* shared stack idx, or -1 */
    int ue_n = 0, e_gpr = 0, e_fp = 0, e_shared = 0;
    for (size_t ui = 0; ui < U && ue_n < ZERO_CC_TRAMP_MAX_UNITS; ui++) {
        const ZeroCCType *t = &nf->param_types[C + ui];
        int slots = zero_cc_tramp_unit_slots(t);
        bool isfp = zero_cc_tramp_unit_is_fp(t);
        for (int w = 0; w < slots; w++) {
            int sidx = isfp ? e_fp : e_gpr;
            ue_isfp[ue_n] = isfp;
            if (sidx < 8) { ue_reg[ue_n] = sidx; ue_stk[ue_n] = -1; }
            else          { ue_reg[ue_n] = -1;  ue_stk[ue_n] = e_shared++; }
            ue_n++;
        }
        if (isfp) e_fp += slots; else e_gpr += slots;
    }
    int entry_stack_n = e_shared;

    /* ---- pass 2: build the final unit list (captures + user params)
     * into the real nested fn's argument slots. Captures come FIRST
     * (they are the mangled fn's first params), then the user params. */
    int n = 0, gpr_run = 0, fp_run = 0, decl_stack_run = 0;
    int copy_words = 0;

    for (size_t ci = 0; ci < C; ci++) {
        if (nf->nested_capture_byref[ci]) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = false;
            u[n].fin_slot = gpr_run < 8 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            u[n].src = 0;
            u[n].src_off = (int)(32 * ci);
            n++;
            continue;
        }
        const ZeroCCType *t = &nf->nested_capture_types[ci];
        int slots = zero_cc_tramp_unit_slots(t);
        bool isfp = zero_cc_tramp_unit_is_fp(t);
        bool big_struct = (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) &&
                          !zero_cc_type_is_vector(t) && t->size > 16;
        int words = (int)((t->size + 7) / 8);
        for (int w = 0; w < slots; w++) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = isfp;
            u[n].fin_slot = isfp ? (fp_run < 8 ? fp_run++ : -1)
                                 : (gpr_run < 8 ? gpr_run++ : -1);
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            if (big_struct) {
                /* one unit total: the ADDRESS of the ctx value copied
                 * into the stub frame (see the prologue copies). */
                if (w == 0) {
                    u[n].src = 5;
                    u[n].src_off = 208 + 8 * entry_stack_n + 32 * (int)ci;
                    copy_words += words;
                }
                /* slots for a big struct is always 1, so w>0 is dead */
            } else {
                u[n].src = 0;
                u[n].src_off = (int)(32 * ci) + 8 * w;
            }
            n++;
        }
    }
    /* Sprint 99: nonlocal goto pairs, between captures and user params
     * (ctx words at 32*C + 16*i (rbp) and +8 (rsp)). */
    for (int pi = 0; pi < nf->goto_pair_count; pi++) {
        for (int w = 0; w < 2; w++) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = false;
            u[n].fin_slot = gpr_run < 8 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            u[n].src = 0;
            u[n].src_off = (int)(32 * C + 16 * pi) + 8 * w;
            n++;
        }
    }
    int entry_gpr_off = 8;    /* first entry GPR save: [x29-8] */
    int entry_fp_off = 80;    /* first entry FP save: [x29-80] */
    int entry_st_off = 208;   /* first entry stack save: [x29-208] */

    int j = 0;                /* index into the ue_* entry tables */
    for (size_t ui = 0; ui < U; ui++) {
        const ZeroCCType *t = &nf->param_types[C + ui];
        int slots = zero_cc_tramp_unit_slots(t);
        bool isfp = zero_cc_tramp_unit_is_fp(t);
        for (int w = 0; w < slots; w++) {
            if (n >= ZERO_CC_TRAMP_MAX_UNITS) return;
            u[n].is_fp = isfp;
            u[n].fin_slot = isfp ? (fp_run < 8 ? fp_run++ : -1)
                                 : (gpr_run < 8 ? gpr_run++ : -1);
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            if (ue_isfp[j]) {
                if (ue_reg[j] >= 0) {
                    u[n].src = 2;                       /* entry FP save */
                    u[n].src_off = entry_fp_off + 16 * ue_reg[j];
                } else {
                    u[n].src = 3;                       /* shared entry stack */
                    u[n].src_off = entry_st_off + 8 * ue_stk[j];
                }
            } else if (ue_reg[j] >= 0) {
                u[n].src = 1;                           /* entry GPR save */
                u[n].src_off = entry_gpr_off + 8 * ue_reg[j];
            } else {
                u[n].src = 3;                           /* shared entry stack */
                u[n].src_off = entry_st_off + 8 * ue_stk[j];
            }
            n++;
            j++;
        }
    }
    int fin_stack_n = decl_stack_run;

    size_t D = 208 + 8 * (size_t)entry_stack_n + 8 * (size_t)copy_words;
    if (decl_stack_run & 1) D += 8; /* pre-push alignment pad (CALL model) */
    D = (D + 15) & ~(size_t)15;

    /* ---- prologue ---- */
    arm64_emit_u32(c, 0xA9BF7BFDu);  /* stp x29, x30, [sp, #-16]! */
    arm64_emit_u32(c, 0x910003FDu);  /* mov x29, sp */
    arm64_emit_u32(c, 0xD10003FFu | ((D & 0xFFFu) << 10));  /* sub sp, sp, #D */

    /* save entry GPR arg regs x0..x7 */
    for (int g = 0; g < 8; g++)
        tramp64_str_neg(c, g, entry_gpr_off + 8 * g, false);
    /* save entry FP arg regs d0..d7 */
    for (int f = 0; f < 8; f++)
        tramp64_str_neg(c, f, entry_fp_off + 16 * f, true);
    /* save entry stack units: [x29+16+8m] -> [x29-208-8m] (via x10) */
    for (int s = 0; s < entry_stack_n; s++) {
        arm64_emit_u32(c, 0xF9400000u | ((((16u + 8u * (uint32_t)s) / 8u) << 10) | (29u << 5)) | 10u); /* ldr x10, [x29, #16+8s] */
        tramp64_str_neg(c, 10, entry_st_off + 8 * s, false);
    }

    /* ctx base in x9: adrp+add with the data reloc (same shape as
     * OP_MOV_GLOBAL - the ELF writer adds the companion hi21). */
    arm64_emit_u32(c, 0x90000000u | 9u);          /* adrp x9, 0 */
    arm64_emit_u32(c, 0x91000000u | (9u << 5) | 9u); /* add x9, x9, #0 */
    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = me_get_offset(c) - 4;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", nf->tramp_ctx_name);
        r->is_call = false;
    } else {
        zero_cc_reloc_overflow_note(c);
    }

    /* copy >16B by-value struct captures from ctx into the frame
     * (storage for capture ci sits at [x29 - (208 + 8m + 32ci + 8w)] -
     * same expression the pointer unit's src_off uses). */
    for (size_t ci = 0; ci < C; ci++) {
        if (nf->nested_capture_byref[ci]) continue;
        const ZeroCCType *t = &nf->nested_capture_types[ci];
        bool big_struct = (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) &&
                          !zero_cc_type_is_vector(t) && t->size > 16;
        if (!big_struct) continue;
        int words = (int)((t->size + 7) / 8);
        for (int w = 0; w < words; w++) {
            arm64_emit_u32(c, 0xF9400000u | ((((32u * (uint32_t)ci) + 8u * (uint32_t)w) / 8u) << 10) | (9u << 5) | 11u); /* ldr x11, [x9, #32ci+8w] */
            tramp64_str_neg(c, 11, entry_st_off + 8 * entry_stack_n + 32 * (int)ci + 8 * w, false);
        }
    }

    /* ---- place final REGISTER units ---- */
    for (int i = 0; i < n; i++) {
        ZeroCCTrampUnit *a = &u[i];
        if (a->fin_slot < 0) continue;
        if (a->is_fp) {
            switch (a->src) {
            case 0:  /* ctx: ldr d_dst, [x9, #off] */
                arm64_emit_u32(c, 0xFD400000u | (((uint32_t)a->src_off / 8u) << 10) | (9u << 5) | (uint32_t)(a->fin_slot & 0x1F));
                break;
            case 2:  /* entry FP save */
                tramp64_ldr_neg(c, a->fin_slot, (uint32_t)a->src_off, true);
                break;
            case 3:  /* entry stack save (FP stack unit) */
                tramp64_ldr_neg(c, a->fin_slot, (uint32_t)a->src_off, true);
                break;
            default: break;
            }
        } else {
            switch (a->src) {
            case 0:  /* ctx: ldr x_dst, [x9, #off] */
                arm64_emit_u32(c, 0xF9400000u | (((uint32_t)a->src_off / 8u) << 10) | (9u << 5) | (uint32_t)(a->fin_slot & 0x1F));
                break;
            case 1:  /* entry GPR save */
            case 3:  /* entry stack save */
                tramp64_ldr_neg(c, a->fin_slot, (uint32_t)a->src_off, false);
                break;
            case 5:  /* address of the big-capture frame storage */
                arm64_emit_u32(c, 0xD1000000u | (((uint32_t)a->src_off & 0xFFFu) << 10) | (29u << 5) | 10u); /* sub x10, x29, #off */
                arm64_emit_u32(c, 0xAA0003E0u | (10u << 16) | (uint32_t)(a->fin_slot & 0x1F));  /* mov x_dst, x10 */
                break;
            default: break;
            }
        }
    }

    /* ---- final STACK units: push in DESCENDING index order so unit 0
     * ends up at the lowest address (= [sp] just before the bl, which
     * is [sp+8] at the callee - the shared stream's slot 0) ---- */
    for (int idx = fin_stack_n - 1; idx >= 0; idx--) {
        for (int i = 0; i < n; i++) {
            ZeroCCTrampUnit *a = &u[i];
            if (a->fin_stack != idx) continue;
            if (a->is_fp) {
                switch (a->src) {
                case 0:
                    arm64_emit_u32(c, 0xFD400000u | (((uint32_t)a->src_off / 8u) << 10) | (9u << 5) | 9u); /* ldr d9, [x9, #off] */
                    break;
                case 2:
                case 3:
                    tramp64_ldr_neg(c, 9, (uint32_t)a->src_off, true);
                    break;
                default: break;
                }
                arm64_emit_u32(c, 0xFC1F8FE0u | 9u); /* str d9, [sp, #-8]! */
            } else {
                switch (a->src) {
                case 0:
                    arm64_emit_u32(c, 0xF9400000u | (((uint32_t)a->src_off / 8u) << 10) | (9u << 5) | 10u); /* ldr x10, [x9, #off] */
                    break;
                case 1:
                case 3:
                    tramp64_ldr_neg(c, 10, (uint32_t)a->src_off, false);
                    break;
                case 5:
                    arm64_emit_u32(c, 0xD1000000u | (((uint32_t)a->src_off & 0xFFFu) << 10) | (29u << 5) | 10u); /* sub x10, x29, #off */
                    break;
                default: break;
                }
                arm64_emit_u32(c, 0xF81F8FE0u | 10u); /* str x10, [sp, #-8]! */
            }
        }
    }

    /* 16-byte stack alignment at the bl: handled by the pre-push pad in
     * the prologue (same model as zerocc's OP_CALL lowering: pad 8 when
     * fin_stack_n is ODD, BEFORE the pushes - a post-push pad would shift
     * the stack units away from [entry_sp], where the real fn's prologue
     * reads the shared stream). bl leaves SP untouched. */

    /* bl the real nested function - same text image, exact imm26
     * (AArch64 bl is PC-relative with PC = the bl's own address). */
    size_t call_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x94000000u | (uint32_t)(((int64_t)(nf->text_offset - call_pos) >> 2) & 0x3FFFFFFu));

    /* the target's `ret` lands here: return to the original caller */
    arm64_emit_u32(c, 0x910003BFu);  /* mov sp, x29 */
    arm64_emit_u32(c, 0xA8C17BFDu);  /* ldp x29, x30, [sp], #16 */
    arm64_emit_u32(c, 0xD65F03C0u);  /* ret */
}

/* ARM64 backend instance */

/* AArch64 VREG mapping: direct pass-through (VREG numbers = ARM encodings) */
static int aarch64_get_int_reg(int vreg) { return vreg; }
static int aarch64_get_fp_reg(int vreg) { return vreg; }

/* ---- GNU __int128 (Sprint 109): (lo,hi) GPR-pair decompositions ----
 * On AArch64 a REG_CLASS_GPR128 value always spills (regalloc returns -1)
 * and no current test exercises 128-bit integers on this ISA, so these are
 * best-effort (lo,hi) decompositions built from the 64-bit primitives plus
 * the carry/borrow, multiply-high and sign-extract instructions. Encodings
 * verified against `aarch64-linux-gnu-gcc`: ADC=0x9A000000, SBC=0xDA000000,
 * MUL=0x9B007C00, UMULH=0x9BC07C00, ASR#63=0x937FFC00. */
static void aarch64_emit_add128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    /* add dlo, alo, blo with S=1 (0xAB): the plain `add` (0x8B) does NOT set the
     * carry flag, so the following `adc` would read a stale C. The flag-setting
     * form matches what gcc emits as `cmn` before its `adc`. */
    arm64_emit_u32(c, 0xAB000000u | ((blo & 0x1F) << 16) | ((alo & 0x1F) << 5) | (dlo & 0x1F));
    arm64_emit_u32(c, 0x9A000000u | ((bhi & 0x1F) << 16) | ((ahi & 0x1F) << 5) | (dhi & 0x1F)); /* adc dhi, ahi, bhi */
}
static void aarch64_emit_sub128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    /* sub dlo, alo, blo with S=1 (0xEB): the plain `sub` (0xCB) does NOT set the
     * carry flag, so the following `sbc` would read a stale C. The flag-setting
     * form (C = NOT-borrow) matches what gcc emits as `cmp` before its `sbc`. */
    arm64_emit_u32(c, 0xEB000000u | ((blo & 0x1F) << 16) | ((alo & 0x1F) << 5) | (dlo & 0x1F));
    arm64_emit_u32(c, 0xDA000000u | ((bhi & 0x1F) << 16) | ((ahi & 0x1F) << 5) | (dhi & 0x1F)); /* sbc dhi, ahi, bhi */
}
static void aarch64_emit_mul128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    /* low 128 bits of a 128x128 product:
     * dlo = lo(alo*blo); dhi = hi(alo*blo) + lo(alo*bhi) + lo(ahi*blo). */
    int t1 = BE(c)->scratch_reg1, t2 = BE(c)->scratch_reg2;
    arm64_emit_u32(c, 0x9B007C00u | ((blo & 0x1F) << 16) | ((alo & 0x1F) << 5) | (dlo & 0x1F)); /* mul dlo, alo, blo */
    arm64_emit_u32(c, 0x9BC07C00u | ((blo & 0x1F) << 16) | ((alo & 0x1F) << 5) | (dhi & 0x1F)); /* umulh dhi, alo, blo */
    arm64_emit_u32(c, 0x9B007C00u | ((bhi & 0x1F) << 16) | ((alo & 0x1F) << 5) | (t1 & 0x1F));  /* mul t1, alo, bhi */
    aarch64_emit_add(c, dhi, dhi, t1);
    arm64_emit_u32(c, 0x9B007C00u | ((blo & 0x1F) << 16) | ((ahi & 0x1F) << 5) | (t2 & 0x1F));  /* mul t2, ahi, blo */
    aarch64_emit_add(c, dhi, dhi, t2);
}
static void aarch64_emit_mul128_spilled(ZeroCCCompiler *c,
                                        int32_t d_lo, int32_t d_hi,
                                        int32_t a_lo, int32_t a_hi,
                                        int32_t b_lo, int32_t b_hi) {
    /* Spilled MUL128 (Sprint 109): all operands live in frame slots. Uses
     * only the reserved 128-bit scratch pairs (never hold a live value):
     * pair0 (lo,hi) holds the current product, pair1.lo the second factor. */
    int base = zero_cc_frame_base_reg(c);
    int plo = BE(c)->g128_scratch_lo[0], phi = BE(c)->g128_scratch_hi[0];
    int b1lo = BE(c)->g128_scratch_lo[1];
    /* P0 = a_lo * b_lo: phi=P0.hi (umulh FIRST - the following mul overwrites
     * plo, which holds a_lo), plo=P0.lo (mul) */
    BE(c)->emit_load(c, plo, base, a_lo);
    BE(c)->emit_load(c, b1lo, base, b_lo);
    arm64_emit_u32(c, 0x9BC07C00u | ((b1lo & 0x1F) << 16) | ((plo & 0x1F) << 5) | (phi & 0x1F)); /* umulh phi, plo, b1lo */
    arm64_emit_u32(c, 0x9B007C00u | ((b1lo & 0x1F) << 16) | ((plo & 0x1F) << 5) | (plo & 0x1F)); /* mul plo, plo, b1lo */
    BE(c)->emit_store(c, plo, base, d_lo);
    BE(c)->emit_store(c, phi, base, d_hi);
    /* P1 = a_lo * b_hi; dhi += P1.lo */
    BE(c)->emit_load(c, plo, base, a_lo);
    BE(c)->emit_load(c, b1lo, base, b_hi);
    arm64_emit_u32(c, 0x9B007C00u | ((b1lo & 0x1F) << 16) | ((plo & 0x1F) << 5) | (plo & 0x1F)); /* mul plo, plo, b1lo */
    BE(c)->emit_load(c, b1lo, base, d_hi);
    aarch64_emit_add(c, b1lo, b1lo, plo);
    BE(c)->emit_store(c, b1lo, base, d_hi);
    /* P2 = a_hi * b_lo; dhi += P2.lo */
    BE(c)->emit_load(c, plo, base, a_hi);
    BE(c)->emit_load(c, b1lo, base, b_lo);
    arm64_emit_u32(c, 0x9B007C00u | ((b1lo & 0x1F) << 16) | ((plo & 0x1F) << 5) | (plo & 0x1F)); /* mul plo, plo, b1lo */
    BE(c)->emit_load(c, b1lo, base, d_hi);
    aarch64_emit_add(c, b1lo, b1lo, plo);
    BE(c)->emit_store(c, b1lo, base, d_hi);
}
/* MUL128 with an arbitrary mix of register-pair / frame-slot operands
 * (Sprint 109 1-scratch-pair fix). Each of d/a/b is either REG (physical
 * registers) or MEM (frame displacements), per the *_mem flags. Uses only
 * the four reserved 128-bit scratch registers (pair0 = plo/phi, pair1 =
 * b1/hi1); d is written last so d may alias a or b. */
static void aarch64_emit_mul128_mixed(ZeroCCCompiler *c,
                                      int d_mem, int d_lo, int d_hi,
                                      int a_mem, int a_lo, int a_hi,
                                      int b_mem, int b_lo, int b_hi) {
    int base = zero_cc_frame_base_reg(c);
    int plo = BE(c)->g128_scratch_lo[0], phi = BE(c)->g128_scratch_hi[0];
    int b1 = BE(c)->g128_scratch_lo[1], b1h = BE(c)->g128_scratch_hi[1];
    /* load a half into a scratch reg (mov if REG, ldr if MEM) */
    #define A_LOAD(reg, home, is_mem) \
        do { if (is_mem) BE(c)->emit_load(c, (reg), base, (home)); \
             else aarch64_emit_mov_reg(c, (reg), (home)); } while (0)
    /* P0 = a_lo * b_lo: phi=P0.hi (umulh FIRST - the following mul overwrites
     * plo, which holds a_lo), plo=P0.lo (mul) */
    A_LOAD(plo, a_lo, a_mem);
    A_LOAD(b1, b_lo, b_mem);
    arm64_emit_u32(c, 0x9BC07C00u | ((b1 & 0x1F) << 16) | ((plo & 0x1F) << 5) | (phi & 0x1F)); /* umulh phi, plo, b1 */
    arm64_emit_u32(c, 0x9B007C00u | ((b1 & 0x1F) << 16) | ((plo & 0x1F) << 5) | (plo & 0x1F)); /* mul plo, plo, b1 */
    aarch64_emit_mov_reg(c, b1h, plo);  /* stash P0.lo (plo is reused below) */
    /* P1 = a_lo * b_hi; d_hi += P1.lo */
    A_LOAD(plo, a_lo, a_mem);
    A_LOAD(b1, b_hi, b_mem);
    arm64_emit_u32(c, 0x9B007C00u | ((b1 & 0x1F) << 16) | ((plo & 0x1F) << 5) | (plo & 0x1F)); /* mul plo, plo, b1 */
    if (d_mem) {
        BE(c)->emit_load(c, b1, base, d_hi);
        aarch64_emit_add(c, b1, b1, plo);
        BE(c)->emit_store(c, b1, base, d_hi);
    } else {
        aarch64_emit_add(c, d_hi, d_hi, plo);
    }
    /* P2 = a_hi * b_lo; d_hi += P2.lo */
    A_LOAD(plo, a_hi, a_mem);
    A_LOAD(b1, b_lo, b_mem);
    arm64_emit_u32(c, 0x9B007C00u | ((b1 & 0x1F) << 16) | ((plo & 0x1F) << 5) | (plo & 0x1F)); /* mul plo, plo, b1 */
    if (d_mem) {
        BE(c)->emit_load(c, b1, base, d_hi);
        aarch64_emit_add(c, b1, b1, plo);
        BE(c)->emit_store(c, b1, base, d_hi);
    } else {
        aarch64_emit_add(c, d_hi, d_hi, plo);
    }
    /* write d last (d may alias a or b) */
    if (d_mem) {
        BE(c)->emit_store(c, b1h, base, d_lo);
        BE(c)->emit_store(c, phi, base, d_hi);
    } else {
        aarch64_emit_mov_reg(c, d_lo, b1h);
        aarch64_emit_mov_reg(c, d_hi, phi);
    }
    #undef A_LOAD
}
static void aarch64_emit_and128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    aarch64_emit_and(c, dlo, alo, blo); aarch64_emit_and(c, dhi, ahi, bhi);
}
static void aarch64_emit_or128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    aarch64_emit_or(c, dlo, alo, blo); aarch64_emit_or(c, dhi, ahi, bhi);
}
static void aarch64_emit_xor128(ZeroCCCompiler *c, int dlo, int dhi, int alo, int ahi, int blo, int bhi) {
    aarch64_emit_xor(c, dlo, alo, blo); aarch64_emit_xor(c, dhi, ahi, bhi);
}
static void aarch64_emit_not128(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi) {
    aarch64_emit_not(c, dlo, slo); aarch64_emit_not(c, dhi, shi);
}
static void aarch64_emit_neg128(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi) {
    /* -x = 0 - x. low = 0-slo must SET the C flag (borrow) for the high sbc, so
     * use the flag-setting SUBS (0xEB S=1), not plain SUB (0xCB S=0).
     * Extended-register SUBS: Rn = xzr (31), Rm = slo. (The earlier encoding
     * put slo in Rn and nothing in Rm - `subs dlo, slo, xzr` - which computed
     * dlo = slo instead of 0 - slo.) */
    arm64_emit_u32(c, 0xEB000000u | ((slo & 0x1F) << 16) | (31u << 5) | (dlo & 0x1F));        /* subs dlo, xzr, slo (set CF) */
    arm64_emit_u32(c, 0xDA000000u | ((shi & 0x1F) << 16) | (31u << 5) | (dhi & 0x1F)); /* sbc dhi, xzr, shi */
}
static void aarch64_emit_mov128(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi) {
    aarch64_emit_mov_reg(c, dlo, slo); aarch64_emit_mov_reg(c, dhi, shi);
}
static void aarch64_emit_load128(ZeroCCCompiler *c, int dlo, int dhi, int base, int32_t offset) {
    /* Aliasing: if dlo == base, load the hi half first (see x86 note); the
     * self-referential lo load `ldr dlo, [base]` is safe on AArch64. */
    if (dlo == base) {
        aarch64_emit_load(c, dhi, base, offset + 8);
        aarch64_emit_load(c, dlo, base, offset);
    } else {
        aarch64_emit_load(c, dlo, base, offset);
        aarch64_emit_load(c, dhi, base, offset + 8);
    }
}
static void aarch64_emit_store128(ZeroCCCompiler *c, int slo, int shi, int base, int32_t offset) {
    aarch64_emit_store(c, slo, base, offset);
    aarch64_emit_store(c, shi, base, offset + 8);
}
static void aarch64_emit_zext64_128(ZeroCCCompiler *c, int dlo, int dhi, int src) {
    aarch64_emit_mov_reg(c, dlo, src);
    aarch64_emit_mov_imm(c, dhi, 0);
}
static void aarch64_emit_sext64_128(ZeroCCCompiler *c, int dlo, int dhi, int src) {
    aarch64_emit_mov_reg(c, dlo, src);
    arm64_emit_u32(c, 0x937FFC00u | ((src & 0x1F) << 5) | (dhi & 0x1F));  /* asr dhi, src, #63 */
}
static void aarch64_emit_trunc128_64(ZeroCCCompiler *c, int dst, int slo) {
    aarch64_emit_mov_reg(c, dst, slo);
}
static void aarch64_emit_mov128_imm(ZeroCCCompiler *c, int dlo, int dhi, int64_t imm) {
    aarch64_emit_mov_imm(c, dlo, imm);
    aarch64_emit_mov_imm(c, dhi, 0);
}

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
    /* 128-bit spill scratch (reserved; not in the 64-bit or GPR128 pools, and
     * NOT x9/x10 which the AArch64 mul128 uses as internal scratch, and NOT
     * x16 which is the dedicated large-offset address scratch):
     * pair0 = (x8, x17), pair1 = (x15, x18). */
    .g128_scratch_lo = {8, 15},
    .g128_scratch_hi = {17, 18},
    .g128_scratch_pair_count = 2,
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

    /* GNU __int128 (Sprint 109) */
    .emit_add128 = aarch64_emit_add128,
    .emit_sub128 = aarch64_emit_sub128,
    .emit_mul128 = aarch64_emit_mul128,
    .emit_mul128_spilled = aarch64_emit_mul128_spilled,
    .emit_mul128_mixed = aarch64_emit_mul128_mixed,
    .emit_and128 = aarch64_emit_and128,
    .emit_or128 = aarch64_emit_or128,
    .emit_xor128 = aarch64_emit_xor128,
    .emit_not128 = aarch64_emit_not128,
    .emit_neg128 = aarch64_emit_neg128,
    .emit_mov128 = aarch64_emit_mov128,
    .emit_load128 = aarch64_emit_load128,
    .emit_store128 = aarch64_emit_store128,
    .emit_zext64_128 = aarch64_emit_zext64_128,
    .emit_sext64_128 = aarch64_emit_sext64_128,
    .emit_trunc128_64 = aarch64_emit_trunc128_64,
    .emit_mov128_imm = aarch64_emit_mov128_imm,

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
    .emit_store_64 = aarch64_emit_store_64,
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
    .emit_jmp_reg = aarch64_emit_jmp_reg,
    .emit_lea_label = aarch64_emit_lea_label,
    .emit_syscall = aarch64_emit_syscall,
    .emit_ret = aarch64_emit_ret,

    .emit_load_fp = aarch64_emit_load_fp,
    .emit_store_fp = aarch64_emit_store_fp,
    .emit_vload = aarch64_emit_vload,
    .emit_vstore = aarch64_emit_vstore,
    .emit_vloadsd = aarch64_emit_load_fp,
    .emit_vstoredsd = aarch64_emit_store_fp,
    .emit_vloadss = aarch64_emit_load_single_fp,
    .emit_vstoress = aarch64_emit_store_single_fp,
    .emit_vloadss_raw = aarch64_emit_load_single_fp_raw,
    .emit_vstoress_raw = aarch64_emit_store_single_fp_raw,
    .emit_cvttsd2ss = aarch64_emit_fp_cvttsd2ss,
    .emit_vaddps = aarch64_emit_vaddps,
    .emit_vsubps = aarch64_emit_vsubps,
    .emit_vmulps = aarch64_emit_vmulps,
    .emit_vdivps = aarch64_emit_vdivps,
    .emit_vaddpd = aarch64_emit_vaddpd,
    .emit_vsubpd = aarch64_emit_vsubpd,
    .emit_vmulpd = aarch64_emit_vmulpd,
    .emit_vdivpd = aarch64_emit_vdivpd,
    .emit_vpaddd = aarch64_emit_vpaddd,
    .emit_vpsubd = aarch64_emit_vpsubd,
    .emit_vpmulld = aarch64_emit_vpmulld,
    .emit_vmovaps = aarch64_emit_vmovaps,
    .emit_load_fp_rodata = aarch64_emit_load_fp_rodata,
    .emit_fp_add = aarch64_emit_fp_add,
    .emit_fp_sub = aarch64_emit_fp_sub,
    .emit_fp_mul = aarch64_emit_fp_mul,
    .emit_fp_div = aarch64_emit_fp_div,
    .emit_fp_neg = aarch64_emit_fp_neg,
    .emit_cvtss2sd = aarch64_emit_fp_cvtss2sd,
    .emit_cvtsi2ss = aarch64_emit_fp_cvtsi2ss,
    .emit_cvtsd2ss = aarch64_emit_fp_cvtsd2ss,
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
    .emit_atomic_cmpxchg_bool = aarch64_emit_atomic_cmpxchg_bool,
    .emit_atomic_xadd = aarch64_emit_atomic_xadd,
    .emit_atomic_or = aarch64_emit_atomic_or,
    .emit_atomic_and = aarch64_emit_atomic_and,
    .emit_atomic_xor = aarch64_emit_atomic_xor,
    .emit_atomic_xchg = aarch64_emit_atomic_xchg,
    .emit_mfence = aarch64_emit_mfence,
    .emit_trap = aarch64_emit_trap,
    .emit_trap_asm = aarch64_emit_trap_asm,

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

/* Sprint 99: OP_NONLOCAL_GOTO absolute jump (AOT).
 * adrp x16, <label>  (R_AARCH64_ADR_PREL_PG_HI21 against a local label
 * symbol at text_offset, added by the .o writer)
 * add  x16, x16, #off (R_AARCH64_ADD_ABS_LO12_NC, same symbol)
 * br   x16
 * x16 is the AArch64 PCS indirect-branch scratch; caller-saved and
 * never live here (FP/SP were just restored, and the target is an
 * ancestor mid-function with no AArch64 callee-save contract across a
 * nonlocal jump - the target frame's callee-saved regs are the ones the
 * ancestor saved on its own frame, untouched by the jump). */
void zero_cc_aarch64_emit_nonlocal_goto_jump(ZeroCCCompiler *c, size_t text_offset) {
    arm64_emit_u32(c, 0x90000000u | 16u);           /* adrp x16, 0 */
    size_t patch_pos = me_get_offset(c);
    arm64_emit_u32(c, 0x91000000u | (16u << 5) | 16u); /* add x16, x16, #0 */
    if (c->nl_goto_reloc_count < 64) {
        c->nl_goto_relocs[c->nl_goto_reloc_count].text_patch_offset = patch_pos;
        c->nl_goto_relocs[c->nl_goto_reloc_count].text_target_offset = text_offset;
        c->nl_goto_reloc_count++;
    }
    arm64_emit_u32(c, 0xD61F0000u | (16u << 5));    /* br x16 */
}
