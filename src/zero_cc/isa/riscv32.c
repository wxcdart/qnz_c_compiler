/* =========================================================================
 * RISC-V 32-bit backend (Sprint 114, extracted from riscv.c).
 *
 * RV32 reuses the RV64 MC emitters wholesale - they are shared code that
 * branches on c->isa at the 32/64-bit boundary (lw vs ld, sw vs sd, ...).
 * The only RV32-specific MC code is the 4-byte prologue/leave/epilogue/
 * push/pop frame sequence below.
 *
 * Because the two vtables are identical except for a handful of fields,
 * the RV32 vtable is built at init time by copying the fully-populated
 * RV64 table and overriding the differences (riscv32_init_backend, called
 * from zero_cc_init_backends_once before the asm-pointer fill-in). This
 * keeps a single definition of every shared emitter in riscv64.c.
 * ========================================================================= */
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/isa/riscv_common.h"

/* Shared 64-bit immediate loader (also used by the RV32 prologue for
 * large frames); defined in riscv64.c. */
extern void riscv64_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm);

/* RISC-V 32-bit calling convention (similar but different register sizes) */
static const int riscv32_int_arg_regs[] = {10, 11, 12, 13, 14, 15, 16, 17};  /* a0-a7 */
static const int riscv32_fp_arg_regs[] = {10, 11, 12, 13, 14, 15, 16, 17};  /* fa0-fa7 */
static const int riscv32_callee_saved_gpr[] = {8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};  /* s0-s11 */

static const ISACallingConvention riscv32_conv = {
    .int_arg_regs = riscv32_int_arg_regs,
    .int_arg_reg_count = 8,
    .fp_arg_regs = riscv32_fp_arg_regs,
    .fp_arg_reg_count = 8,
    .callee_saved_gpr = riscv32_callee_saved_gpr,
    .callee_saved_gpr_count = 12,
    .callee_save_slot_offsets = (const int32_t[]){8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96},
};

/* RISC-V 32-bit backend (for ESP32-C3/C6/H2, rv32gc) */
static void riscv32_emit_prologue(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask) {
    if (c->lowering.is_frameless) return;
    (void)callee_save_mask;

    if (c->lowering.frame_align > 16) {
        /* C11 _Alignas(N) realign mode (32-bit variant): save the ra/s0 pair,
         * realign SP to N, then allocate the frame below SP'. */
        size_t frame = (size_t)c->lowering.frame_delta;
        riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, -16, 0x13));         /* addi sp, sp, -16 */
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_RA, 2, 4, 0x23));           /* sw ra, 4(sp) - rv32 leave expects ra at +4 */
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_S0, 2, 0, 0x23));           /* sw s0, 0(sp) */
        riscv_emit_u32(c, rv64_I(RV64_S0, RV64_SP, 0, 0, 0x13));           /* mv s0, sp */
        int mask = (~((int32_t)c->lowering.frame_align - 1)) & 0xFFF;      /* ~(N-1) */
        if (mask != 0) {
            riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 7, mask, 0x13));   /* andi sp, sp, mask */
        } else {
            /* N == 4096: 12-bit andi immediate is 0 (would zero SP). Use a
             * register mask instead. */
            riscv64_emit_mov_imm(c, RV64_T0, (int64_t)~((int64_t)c->lowering.frame_align - 1));
            riscv_emit_u32(c, rv64_R(RV64_SP, RV64_SP, RV64_T0, 7, 0));    /* and sp, sp, t0 */
        }
        if (frame <= 2047) {
            riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, (int16_t)(-(int32_t)frame), 0x13));
        } else {
            riscv64_emit_mov_imm(c, RV64_T0, (int64_t)frame);
            riscv_emit_u32(c, rv64_R(RV64_SP, RV64_SP, RV64_T0, 0, 0x20)); /* sub sp, sp, t0 */
        }
        return;
    }

    size_t aligned = (stack_size + 15) & ~(size_t)15;
    size_t total = aligned + 16;
    int big = (total > 2047);
    if (big) {
        riscv64_emit_mov_imm(c, RV64_T0, (int64_t)total);
        riscv_emit_u32(c, rv64_R(RV64_SP, RV64_SP, RV64_T0, 0, 0x20));  /* sub sp, sp, t0 (f7=0x20) */
        if (aligned > 2047) {
            riscv64_emit_mov_imm(c, 6 /* t1 */, (int64_t)aligned);
            riscv_emit_u32(c, rv64_R(6 /* t1 */, RV64_SP, 6 /* t1 */, 0, 0)); /* add t1, sp, t1 */
        } else {
            riscv_emit_u32(c, rv64_I(6 /* t1 */, RV64_SP, 0, (int16_t)aligned, 0x13));
        }
        riscv_emit_u32(c, rv64_S(6 /* t1 */, RV64_RA, 2, 4, 0x23)); /* sw ra, 4(t1) */
        riscv_emit_u32(c, rv64_S(6 /* t1 */, RV64_S0, 2, 0, 0x23)); /* sw s0, 0(t1) */
        riscv_emit_u32(c, rv64_R(RV64_S0, 6 /* t1 */, 0, 0, 0));    /* add s0, t1, zero */
    } else {
        riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, (int16_t)(-(int32_t)total), 0x13));
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_RA, 2, (int16_t)(aligned + 4), 0x23)); /* sw ra, aligned+4(sp) */
        riscv_emit_u32(c, rv64_S(RV64_SP, RV64_S0, 2, (int16_t)aligned, 0x23));       /* sw s0, aligned(sp) */
        riscv_emit_u32(c, rv64_I(RV64_S0, RV64_SP, 0, (int16_t)aligned, 0x13));       /* addi s0, sp, aligned */
    }
}

static void riscv32_emit_leave(ZeroCCCompiler *c) {
    if (c->lowering.is_frameless) return;
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_S0, 0, 0, 0x13));  /* addi sp, s0, 0 */
    riscv_emit_u32(c, rv64_I(RV64_RA, RV64_SP, 2, 4, 0x03));  /* lw ra, 4(sp) */
    riscv_emit_u32(c, rv64_I(RV64_S0, RV64_SP, 2, 0, 0x03));  /* lw s0, 0(sp) */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, 16, 0x13)); /* addi sp, sp, 16 */
}

static void riscv32_emit_epilogue(ZeroCCCompiler *c) {
    riscv32_emit_leave(c);
    riscv_emit_u32(c, 0x00008067); /* ret: jalr zero, 0(ra) */
}

static void riscv32_emit_push_reg(ZeroCCCompiler *c, int reg) {
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, -4, 0x13));     /* addi sp, sp, -4 */
    riscv_emit_u32(c, rv64_S(RV64_SP, RV_REG(reg), 2, 0, 0x23));  /* sw reg, 0(sp) */
}

static void riscv32_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    riscv_emit_u32(c, rv64_I(RV_REG(reg), RV64_SP, 2, 0, 0x03));  /* lw reg, 0(sp) */
    riscv_emit_u32(c, rv64_I(RV64_SP, RV64_SP, 0, 4, 0x13));      /* addi sp, sp, 4 */
}

/* RISC-V 32-bit backend instance.
 *
 * Zero-initialized here; riscv32_init_backend() populates it from the
 * RV64 table (see zero_cc_init_backends_once in backend_registry.c).
 * All lookups go through zero_cc_get_backend(), which always runs the
 * init first, so the table is never observed half-built. */
ZeroCCISABackend riscv32_backend;

void riscv32_init_backend(void) {
    /* Inherit every shared MC emitter from the fully static-initialized
     * RV64 table (asm pointers are filled in afterwards by
     * riscv64_init_asm_emission, which is called for both tables). */
    riscv32_backend = riscv64_backend;
    riscv32_backend.isa = ISA_RISCV32;
    riscv32_backend.conv = &riscv32_conv;
    riscv32_backend.emit_prologue = riscv32_emit_prologue;
    riscv32_backend.emit_leave = riscv32_emit_leave;
    riscv32_backend.emit_epilogue = riscv32_emit_epilogue;
    riscv32_backend.emit_push_reg = riscv32_emit_push_reg;
    riscv32_backend.emit_pop_reg = riscv32_emit_pop_reg;
    /* RV32 has no GPR128 pair pool and no 128-bit multiply helpers
     * (32-bit words cannot host a (lo, hi) scratch pair the way RV64's
     * (t1,t3)/(t4,t5) do). */
    riscv32_backend.g128_scratch_lo[0] = 0;
    riscv32_backend.g128_scratch_lo[1] = 0;
    riscv32_backend.g128_scratch_hi[0] = 0;
    riscv32_backend.g128_scratch_hi[1] = 0;
    riscv32_backend.g128_scratch_pair_count = 0;
    riscv32_backend.emit_mul128_spilled = NULL;
    riscv32_backend.emit_mul128_mixed = NULL;
}
