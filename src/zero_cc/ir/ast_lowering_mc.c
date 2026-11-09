/* AST Lowering - IR to Machine Code Emission */

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/ir/machine_emitter.h"

/* Emit function prologue directly (without IR instruction) */
void emit_prologue_mc(ZeroCCCompiler *c, size_t frame_size) {
    /* Use the backend's emit_prologue function */
    if (BE(c)->emit_prologue) {
        BE(c)->emit_prologue(c, frame_size, 0);
    }
    if (c->lowering.has_stack_protector && BE(c)->emit_stack_guard_init) {
        BE(c)->emit_stack_guard_init(c, c->lowering.canary_stack_off);
    }
    /* Sprint 99: snapshot the body-time SP (after frame setup, before any
     * VLA adjustment) for nonlocal-goto pair marshalling. */
    if (c->lowering.sp_snap_off > 0 && BE(c)->emit_store) {
        int32_t sd = zero_cc_frame_disp(c, -c->lowering.sp_snap_off);
        BE(c)->emit_store(c, BE(c)->sp_reg, zero_cc_frame_base_reg(c), sd);
        /* Frame-pointer-omitted functions never touch the FP register,
         * so here it still holds the caller's (entry-time) value. Keep it:
         * a nonlocal-goto landing point must re-establish that value after
         * the foreign jump overwrites the register with the target anchor,
         * otherwise the caller's frame pointer is clobbered when this
         * function returns (the OFP epilogue has no `leave`). */
        if (c->lowering.fp_snap_off > 0 && c->lowering.omit_frame_pointer) {
            int32_t fd = zero_cc_frame_disp(c, -c->lowering.fp_snap_off);
            BE(c)->emit_store(c, BE(c)->fp_reg, zero_cc_frame_base_reg(c), fd);
        }
    }
    /* Save used callee-saved GPRs in the prologue before function body executes */
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
    if (mask && BE(c)->conv && BE(c)->conv->callee_saved_gpr && BE(c)->emit_store) {
        const int *callee_regs = BE(c)->conv->callee_saved_gpr;
        const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
        int count = BE(c)->conv->callee_saved_gpr_count;
        for (int i = 0; i < count; i++) {
            int reg = callee_regs[i];
            if ((mask >> reg) & 1u) {
                int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
                BE(c)->emit_store(c, reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot));
            }
        }
    }
}

typedef struct {
    bool *defined;
    size_t *offsets;
    size_t **patches;
    size_t *patch_counts;
    size_t *patch_caps;
    size_t count;
} MCLabelPatchState;

void zc_mc_record_or_patch_label(ZeroCCCompiler *c, size_t patch_pos, uint16_t label_idx) {
    MCLabelPatchState *ls = (MCLabelPatchState *)c->mc_label_state;
    if (!ls || label_idx >= ls->count) return;
    if (ls->defined[label_idx]) {
        BE(c)->patch_jmp(c, patch_pos, ls->offsets[label_idx]);
    } else {
        if (ls->patch_counts[label_idx] >= ls->patch_caps[label_idx]) {
            size_t new_cap = ls->patch_caps[label_idx] ? ls->patch_caps[label_idx] * 2 : 4;
            size_t *new_patches = (size_t *)realloc(ls->patches[label_idx], new_cap * sizeof(size_t));
            if (new_patches) {
                ls->patches[label_idx] = new_patches;
                ls->patch_caps[label_idx] = new_cap;
            }
        }
        if (ls->patches[label_idx] && ls->patch_counts[label_idx] < ls->patch_caps[label_idx]) {
            ls->patches[label_idx][ls->patch_counts[label_idx]++] = patch_pos;
        }
    }
}

/* Emit machine code from IR with allocated registers */
void emit_ir_to_machine_code(ZeroCCCompiler *c, int32_t spill_base_offset) {
    const Instr *instrs = ir_get_buffer(c);
    size_t count = ir_get_count(c);
    size_t label_count = c->ir_next_label;
    bool *label_defined = label_count ? (bool *)calloc(label_count, sizeof(bool)) : NULL;
    size_t *label_offsets = label_count ? (size_t *)calloc(label_count, sizeof(size_t)) : NULL;
    size_t **label_patches = label_count ? (size_t **)calloc(label_count, sizeof(size_t *)) : NULL;
    size_t *label_patch_counts = label_count ? (size_t *)calloc(label_count, sizeof(size_t)) : NULL;
    size_t *label_patch_caps = label_count ? (size_t *)calloc(label_count, sizeof(size_t)) : NULL;
    int call_idx = 0;

    MCLabelPatchState ls = {
        .defined = label_defined,
        .offsets = label_offsets,
        .patches = label_patches,
        .patch_counts = label_patch_counts,
        .patch_caps = label_patch_caps,
        .count = label_count,
    };
    c->mc_label_state = &ls;

    for (size_t i = 0; i < count; i++) {
        const Instr *instr = &instrs[i];
        if (instr->op == OP_DEAD || instr->op == OP_NOP) {
            continue;
        }

        /* Sprint 136, Phase 2: integrated inline assembler for machine code */
        if (instr->op == OP_ASM) {
            if (BE(c)->emit_inline_asm_mc) {
                BE(c)->emit_inline_asm_mc(c, i);
            }
            continue;
        }

        /* Frame pointer (0xFF) should NOT be allocated - use actual FP register */
        int phys_dst = (instr->temp_dest== TEMP_NONE) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
        int phys_src1 = (instr->temp_src1== TEMP_NONE) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1);
        int phys_src2 = (instr->temp_src2== TEMP_NONE) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src2);

        /* Check for spilled temps that need reloading / storing.
         * OP_CALL / OP_SYSCALL / OP_CLONE pack arg_count in temp_dest and resolve their
         * argument temps themselves (from the side table). */
        int spill_src1 = (instr->op == OP_CALL || instr->op == OP_SYSCALL || instr->op == OP_CLONE) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_src1);
        int spill_src2 = (instr->op == OP_CALL || instr->op == OP_SYSCALL || instr->op == OP_CLONE) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_src2);
        int spill_dst  = (instr->op == OP_CALL || instr->op == OP_SYSCALL || instr->op == OP_CLONE) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_dest);

        /* --- 128-bit (GPR128) spill handling (Sprint 109 GPR128 spill fix) ---
         * A spilled REG_CLASS_GPR128 temp must reload BOTH halves (slot and
         * slot+8) into a reserved (lo,hi) scratch pair, and a spilled dst must
         * store BOTH halves back. The scratch pairs are reserved (excluded from the
         * 64-bit and GPR128 value pools) so they never hold a live value. Assignment
         * order: src1 -> pair0, src2 -> pair1, dst -> pair0 if free else pair1, else
         * reuse src1's pair in-place (the 128-bit emits tolerate d==a aliasing). */
        bool op128 = (instr->op == OP_ADD128 || instr->op == OP_SUB128 ||
                      instr->op == OP_MUL128 || instr->op == OP_AND128 ||
                      instr->op == OP_OR128  || instr->op == OP_XOR128 ||
                      instr->op == OP_NOT128 || instr->op == OP_NEG128 ||
                      instr->op == OP_MOV128 || instr->op == OP_MOV128_IMM ||
                      instr->op == OP_ZEXT64_128 || instr->op == OP_SEXT64_128 ||
                      instr->op == OP_SHL128 || instr->op == OP_SHR128 ||
                      instr->op == OP_SAR128 ||
                      instr->op == OP_LO128 || instr->op == OP_HI128 ||
                      instr->op == OP_MK128 || instr->op == OP_CMP128 ||
                      instr->op == OP_LOAD128 || instr->op == OP_STORE128 ||
                      instr->op == OP_TRUNC128_64);
        /* Some 128-bit ops take operands that are SEMANTICALLY 64-bit even when
         * the temp was class-widened to GPR128: MK128's two 64-bit halves, the
         * shift COUNT (src2) of SHL128/SHR128/SAR128, the 64-bit source of
         * ZEXT64_128/SEXT64_128, and the BASE POINTER of LOAD128/STORE128. The
         * 128-bit vs 64-bit spill handling is decided from the op's operand
         * SEMANTICS, not the temp's class: a reused temp id carries ONE merged
         * interval whose class is last-write-wins, so a 64-bit pointer can wear
         * a stale REG_CLASS_GPR128 (and vice versa). Reading a widened temp's
         * hi spill slot (never written by a 64-bit producer such as MOV_IMM / a
         * 64-bit store-back) would feed stale stack garbage into the emit, and
         * skipping a 64-bit base reload because the base "looks 128-bit" stores
         * through a placeholder register (segfault). (Sprint 109.) */
        bool d_is_128 = op128 && !
            (instr->op == OP_LO128 || instr->op == OP_HI128 || instr->op == OP_TRUNC128_64);
        bool s1_is_128 = instr->op == OP_ADD128 || instr->op == OP_SUB128 ||
            instr->op == OP_MUL128 || instr->op == OP_AND128 ||
            instr->op == OP_OR128  || instr->op == OP_XOR128 ||
            instr->op == OP_NOT128 || instr->op == OP_NEG128 ||
            instr->op == OP_MOV128 || instr->op == OP_SHL128 ||
            instr->op == OP_SHR128 || instr->op == OP_SAR128 ||
            instr->op == OP_LO128  || instr->op == OP_HI128 ||
            instr->op == OP_CMP128 || instr->op == OP_STORE128 ||
            instr->op == OP_TRUNC128_64;
        bool s2_is_128 = instr->op == OP_ADD128 || instr->op == OP_SUB128 ||
            instr->op == OP_MUL128 || instr->op == OP_AND128 ||
            instr->op == OP_OR128  || instr->op == OP_XOR128 ||
            instr->op == OP_CMP128;
        bool d128_sp = d_is_128 && spill_dst >= 0;
        bool s128_sp = op128 && s1_is_128 && spill_src1 >= 0;
        bool s228_sp = op128 && s2_is_128 && spill_src2 >= 0;
        int g128_np = BE(c)->g128_scratch_pair_count;
        int pair_s1 = -1, pair_s2 = -1, pair_dst = -1;
        if (s128_sp && g128_np > 0) pair_s1 = 0;
        if (s228_sp && g128_np > 1) pair_s2 = 1;
        if (d128_sp) {
            if (g128_np > 0 && pair_s1 < 0) pair_dst = 0;
            else if (g128_np > 1 && pair_s2 < 0) pair_dst = 1;
            else pair_dst = pair_s1;  /* all three spilled: in-place into src1's pair */
        }
        int g128_dlo = phys_dst, g128_dhi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_dest);
        int g128_s1lo = phys_src1, g128_s1hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src1);
        int g128_s2lo = phys_src2, g128_s2hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src2);
        /* MUL128 with the dst AND both sources spilled: no register pair can
         * hold all three values, and d may not alias a source (MUL128 writes
         * d partway through the sequence). Compute directly between the
         * spill slots using the backend's scratch-only sequence. (Sprint 109
         * all-spilled MUL128 fix: the pair logic below would leave src2
         * unloaded and force d==src1 in-place, both of which miscompile.) */
        if (instr->op == OP_MUL128 && d128_sp && s128_sp && s228_sp &&
            BE(c)->emit_mul128_spilled) {
            int32_t off_d = spill_base_offset + (spill_dst + 1) * 8;
            int32_t off_a = spill_base_offset + (spill_src1 + 1) * 8;
            int32_t off_b = spill_base_offset + (spill_src2 + 1) * 8;
            BE(c)->emit_mul128_spilled(c,
                zero_cc_frame_disp(c, -off_d), zero_cc_frame_disp(c, -off_d - 8),
                zero_cc_frame_disp(c, -off_a), zero_cc_frame_disp(c, -off_a - 8),
                zero_cc_frame_disp(c, -off_b), zero_cc_frame_disp(c, -off_b - 8));
            continue;  /* operands and result are all in frame slots already */
        }
        /* MUL128 with a MIX of spilled/register operands on a 1-scratch-pair
         * ISA (Sprint 109): the pair logic below cannot express these (it
         * would leave a source unloaded or force an unsafe d==source alias
         * for mul). Each operand keeps its natural home (register pair or
         * spill slot); the backend sequence handles every reg/mem mix. */
        if (instr->op == OP_MUL128 && g128_np <= 1 &&
            (d128_sp || s128_sp || s228_sp) &&
            !(d128_sp && s128_sp && s228_sp) &&
            BE(c)->emit_mul128_mixed) {
            int32_t off_d = spill_base_offset + (spill_dst + 1) * 8;
            int32_t off_a = spill_base_offset + (spill_src1 + 1) * 8;
            int32_t off_b = spill_base_offset + (spill_src2 + 1) * 8;
            int d_lo = phys_dst, d_hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_dest);
            int a_lo = phys_src1, a_hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src1);
            int b_lo = phys_src2, b_hi = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src2);
            BE(c)->emit_mul128_mixed(c,
                d128_sp ? 1 : 0,
                d128_sp ? (int)zero_cc_frame_disp(c, -off_d) : d_lo,
                d128_sp ? (int)zero_cc_frame_disp(c, -off_d - 8) : d_hi,
                s128_sp ? 1 : 0,
                s128_sp ? (int)zero_cc_frame_disp(c, -off_a) : a_lo,
                s128_sp ? (int)zero_cc_frame_disp(c, -off_a - 8) : a_hi,
                s228_sp ? 1 : 0,
                s228_sp ? (int)zero_cc_frame_disp(c, -off_b) : b_lo,
                s228_sp ? (int)zero_cc_frame_disp(c, -off_b - 8) : b_hi);
            continue;
        }
        /* STORE128 with BOTH the 128-bit value and the 64-bit base spilled:
         * the generic 64-bit base reload targets scratch_reg2, which on
         * x86-64 (r11) and RISC-V (t1) IS the high/low half of the GPR128
         * pair the value is reloaded into - the base reload would clobber
         * the value half before the store. Stream instead: hold the base in
         * one scratch and move each value half from its slot with the other.
         * (Sprint 109: caught by int128_arith under register pressure -
         * the stored hi half was the base address itself.) */
        if (instr->op == OP_STORE128 && s128_sp && spill_src2 >= 0) {
            int32_t off_v = spill_base_offset + (spill_src1 + 1) * 8;
            int32_t off_b = spill_base_offset + (spill_src2 + 1) * 8;
            int base_r = BE(c)->scratch_reg2, tmp_r = BE(c)->scratch_reg1;
            BE(c)->emit_load(c, base_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_b));
            BE(c)->emit_load(c, tmp_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_v));
            BE(c)->emit_store(c, tmp_r, base_r, 0);
            BE(c)->emit_load(c, tmp_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_v - 8));
            BE(c)->emit_store(c, tmp_r, base_r, 8);
            continue;  /* result is in memory; nothing to store back */
        }
        /* MK128 with a spilled 128-bit dst: the 64-bit src reloads target
         * scratch_reg1/scratch_reg2, which alias the GPR128 dst pair's
         * halves (x86-64 r10/r11, RISC-V t1) - the half moves then become
         * self-moves and the dst keeps stale slot contents. Stream each
         * half straight from its home (register or slot) into the dst
         * slots instead. (Sprint 109.) */
        if (instr->op == OP_MK128 && d128_sp) {
            int32_t off_d = spill_base_offset + (spill_dst + 1) * 8;
            int tmp = BE(c)->scratch_reg1;
            if (spill_src1 >= 0) {
                int32_t off_s1 = spill_base_offset + (spill_src1 + 1) * 8;
                BE(c)->emit_load(c, tmp, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_s1));
            }
            BE(c)->emit_store(c, spill_src1 >= 0 ? tmp : phys_src1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_d));
            if (spill_src2 >= 0) {
                int32_t off_s2 = spill_base_offset + (spill_src2 + 1) * 8;
                BE(c)->emit_load(c, tmp, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_s2));
            }
            BE(c)->emit_store(c, spill_src2 >= 0 ? tmp : phys_src2, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_d - 8));
            continue;
        }
        /* The 5 two-step 128-bit emits (x86-64: `mov d, a; alu d, b`) are NOT
         * d==b alias-safe: the initial mov clobbers b before the alu reads it.
         * When BOTH the dst and src2 are spilled on a 1-scratch-pair ISA,
         * compute directly between frame slots (a = src1, register or slot).
         * (Sprint 109: this case previously left src2 holding a dead
         * placeholder register.) */
        if ((instr->op == OP_ADD128 || instr->op == OP_SUB128 ||
             instr->op == OP_AND128 || instr->op == OP_OR128 ||
             instr->op == OP_XOR128) &&
            d128_sp && s228_sp && g128_np <= 1 &&
            BE(c)->emit_bin128_sp) {
            int32_t off_d = spill_base_offset + (spill_dst + 1) * 8;
            int32_t off_a = spill_base_offset + (spill_src1 + 1) * 8;
            int32_t off_b = spill_base_offset + (spill_src2 + 1) * 8;
            BE(c)->emit_bin128_sp(c, (int)instr->op,
                s128_sp ? 1 : 0,
                s128_sp ? (int)zero_cc_frame_disp(c, -off_a)
                        : phys_src1,
                s128_sp ? (int)zero_cc_frame_disp(c, -off_a - 8)
                        : regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_src1),
                zero_cc_frame_disp(c, -off_b), zero_cc_frame_disp(c, -off_b - 8),
                zero_cc_frame_disp(c, -off_d), zero_cc_frame_disp(c, -off_d - 8));
            continue;
        }
        if (s128_sp && pair_s1 >= 0) {
            int32_t off = spill_base_offset + (spill_src1 + 1) * 8;
            if (BE(c)->emit_load) {
                BE(c)->emit_load(c, BE(c)->g128_scratch_lo[pair_s1], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
                BE(c)->emit_load(c, BE(c)->g128_scratch_hi[pair_s1], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 8));
            }
            g128_s1lo = BE(c)->g128_scratch_lo[pair_s1];
            g128_s1hi = BE(c)->g128_scratch_hi[pair_s1];
        }
        if (s228_sp && pair_s2 >= 0) {
            int32_t off = spill_base_offset + (spill_src2 + 1) * 8;
            if (BE(c)->emit_load) {
                BE(c)->emit_load(c, BE(c)->g128_scratch_lo[pair_s2], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
                BE(c)->emit_load(c, BE(c)->g128_scratch_hi[pair_s2], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 8));
            }
            g128_s2lo = BE(c)->g128_scratch_lo[pair_s2];
            g128_s2hi = BE(c)->g128_scratch_hi[pair_s2];
        } else if (s228_sp && !d128_sp && g128_np <= 1 && pair_s2 < 0 &&
                   (instr->op == OP_ADD128 || instr->op == OP_SUB128 ||
                    instr->op == OP_AND128 || instr->op == OP_OR128 ||
                    instr->op == OP_XOR128)) {
            /* 1-pair ISA, dst register-resident, src2 spilled: the two-step
             * emits are d==b unsafe, so src2 goes to pair0 instead of the dst
             * pair. If src1 is also spilled it owns pair0, so relocate src1
             * into the dst's register pair (d==a IS safe: the initial mov is
             * skipped and every source read precedes the destination write).
             * (Sprint 109: the old in-place d==b fallback miscompiled here.) */
            int32_t off = spill_base_offset + (spill_src2 + 1) * 8;
            if (spill_src1 < 0) {
                BE(c)->emit_load(c, BE(c)->g128_scratch_lo[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
                BE(c)->emit_load(c, BE(c)->g128_scratch_hi[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 8));
                g128_s2lo = BE(c)->g128_scratch_lo[0];
                g128_s2hi = BE(c)->g128_scratch_hi[0];
            } else {
                int dhi_r = regalloc_get_reg_hi_for_temp(&c->regalloc, instr->temp_dest);
                int32_t off_s1 = spill_base_offset + (spill_src1 + 1) * 8;
                BE(c)->emit_load(c, phys_dst, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_s1));
                BE(c)->emit_load(c, dhi_r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off_s1 - 8));
                g128_s1lo = phys_dst;
                g128_s1hi = dhi_r;
                BE(c)->emit_load(c, BE(c)->g128_scratch_lo[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
                BE(c)->emit_load(c, BE(c)->g128_scratch_hi[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 8));
                g128_s2lo = BE(c)->g128_scratch_lo[0];
                g128_s2hi = BE(c)->g128_scratch_hi[0];
            }
        } else if (s228_sp && !d128_sp && pair_s1 < 0 && spill_src1 < 0 && g128_np > 0) {
            /* 1-pair ISA (x86-64) fallback for the remaining ops (MUL128,
             * CMP128): src2 is a spilled GPR128 but pair1 does not exist.
             * Reload it into pair0, which is safe when src1 is fully
             * register-resident (it needs no scratch, and its pool registers
             * can never be the pair0 scratches). */
            int32_t off = spill_base_offset + (spill_src2 + 1) * 8;
            if (BE(c)->emit_load) {
                BE(c)->emit_load(c, BE(c)->g128_scratch_lo[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
                BE(c)->emit_load(c, BE(c)->g128_scratch_hi[0], zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off - 8));
            }
            g128_s2lo = BE(c)->g128_scratch_lo[0];
            g128_s2hi = BE(c)->g128_scratch_hi[0];
        }
        if (d128_sp) {
            if (pair_dst >= 0 && pair_dst != pair_s1) {
                g128_dlo = BE(c)->g128_scratch_lo[pair_dst];
                g128_dhi = BE(c)->g128_scratch_hi[pair_dst];
            } else if (pair_dst == pair_s1) {
                g128_dlo = g128_s1lo; g128_dhi = g128_s1hi;  /* in-place */
            }
        }
        if (op128) {
            phys_dst = g128_dlo; phys_src1 = g128_s1lo; phys_src2 = g128_s2lo;
        }


        /* Reload spilled sources into scratch registers (64-bit / FP). FP-class values
         * use the FP scratch XMMs with movsd (emit_load_fp); GPR values use
         * r10/r11 (C8). GPR128 operands are handled above (skipped here). */
        if (spill_src1 >= 0 && !s128_sp) {
            int32_t slot_off = spill_base_offset + (spill_src1 + 1) * 8;
            if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_src1) == REG_CLASS_XMM) {
                if (BE(c)->emit_load_fp) {
                    BE(c)->emit_load_fp(c, BE(c)->fp_scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off));
                }
                phys_src1 = BE(c)->fp_scratch_reg1;
            } else {
                if (BE(c)->emit_load) {
                    BE(c)->emit_load(c, BE(c)->scratch_reg1 /* r10 */, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off));
                }
                phys_src1 = BE(c)->scratch_reg1;
            }
        }
        if (spill_src2 >= 0 && !s228_sp) {
            int32_t slot_off = spill_base_offset + (spill_src2 + 1) * 8;
            if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_src2) == REG_CLASS_XMM) {
                if (BE(c)->emit_load_fp) {
                    BE(c)->emit_load_fp(c, BE(c)->fp_scratch_reg2, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off));
                }
                phys_src2 = BE(c)->fp_scratch_reg2;
            } else {
                if (BE(c)->emit_load) {
                    BE(c)->emit_load(c, BE(c)->scratch_reg2 /* r11 */, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off));
                }
                phys_src2 = BE(c)->scratch_reg2;
            }
        }
        if (spill_dst >= 0 && !d128_sp) {
            if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_dest) == REG_CLASS_XMM) {
                phys_dst = BE(c)->fp_scratch_reg1;
            } else {
                phys_dst = BE(c)->scratch_reg1;
            }
        }

        switch (instr->op) {
            /* Parameter saving */
            case OP_PARAM_SAVE_INT:
                if (BE(c)->emit_param_save_int) {
                    BE(c)->emit_param_save_int(c, (int)instr->imm, (int32_t)instr->imm64);
                }
                break;

            case OP_PARAM_SAVE_FP:
                if (BE(c)->emit_param_save_fp) {
                    BE(c)->emit_param_save_fp(c, (int)instr->imm, (int32_t)instr->imm64, (int)instr->cond);
                }
                break;

            case OP_PARAM_LOAD: {
                bool is_fp = instr->cond != 0;
                int arg_idx = (int)instr->imm;
                if (is_fp) {
                    if (arg_idx >= 0 && arg_idx < BE(c)->conv->fp_arg_reg_count) {
                        int src_reg = BE(c)->conv->fp_arg_regs[arg_idx];
                        if (phys_dst != src_reg && BE(c)->emit_fp_mov) {
                            BE(c)->emit_fp_mov(c, phys_dst, src_reg);
                        }
                    }
                } else {
                    if (arg_idx >= 0 && arg_idx < BE(c)->conv->int_arg_reg_count) {
                        int src_reg = BE(c)->conv->int_arg_regs[arg_idx];
                        if (phys_dst != src_reg && BE(c)->emit_mov_reg) {
                            BE(c)->emit_mov_reg(c, phys_dst, src_reg);
                        }
                    }
                }
                break;
            }

            case OP_VARIADIC_SPILL:
                if (BE(c)->emit_variadic_spill) {
                    BE(c)->emit_variadic_spill(c, (int)instr->imm, (int32_t)instr->imm64);
                }
                break;

            case OP_MOV_IMM:
                BE(c)->emit_mov_imm(c, phys_dst, instr->imm64);
                break;

            case OP_MOV_MEM:
                BE(c)->emit_store(c, phys_src1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, (int32_t)instr->imm64));
                break;

            case OP_MOV:
                BE(c)->emit_load(c, phys_dst, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, (int32_t)instr->imm64));
                break;

            case OP_MOV_REG:
                if (phys_dst != phys_src1) {
                    BE(c)->emit_mov_reg(c, phys_dst, phys_src1);
                }
                break;

            case OP_LEA:
                BE(c)->emit_lea(c, phys_dst, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, (int32_t)instr->imm64));
                break;

            case OP_ADD:
                BE(c)->emit_add(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_SUB:
                BE(c)->emit_sub(c, phys_dst, phys_src1, phys_src2);
                break;

            /* zero_san v1 (x86-64): signed-integer-overflow trap after signed
             * add/sub/mul. The preceding op set the CPU flags; we check whether
             * the raw 64-bit result fits in `width` signed bits and trap (ud2)
             * if not. All inline, no labels/fixups needed. */
            case OP_OVF_TRAP: {
                /* v1: x86-64 only. Lowering skips other ISAs, but this
                 * guard keeps the shared raw-emitter loop safe (cross-ISA
                 * raw AOT would otherwise embed x86 check bytes). */
                if (c->isa != ISA_X86_64) break;
                int width = zc_ovf_trap_width(c, instr);
                bool is_spilled = (spill_dst >= 0);
                int phys = phys_dst;          /* register holding the result */
                int scratch = is_spilled ? BE(c)->scratch_reg2 : BE(c)->scratch_reg1;

                /* REX prefix: 0100WRAB.
                 * W=1 (64-bit). R (bit 2, val 0x04) extends the reg field.
                 * B (bit 0, val 0x01) extends the r/m field. */
                uint8_t rex_mov  = 0x48 | ((phys  >= 8) ? 0x04 : 0) | ((scratch >= 8) ? 0x01 : 0);
                uint8_t rex_alu  = 0x48 | ((scratch >= 8) ? 0x01 : 0);   /* reg field hardcoded (4=SHL,7=SAR) */
                uint8_t rex_cmp  = 0x48 | ((scratch >= 8) ? 0x04 : 0) | ((phys  >= 8) ? 0x01 : 0);

                if (width >= 8) {
                    /* 64-bit: jno rel8,+2 skips ud2 when no overflow (OF=0).
                     * Overflow (OF=1) falls through to ud2 → SIGILL. */
                    me_emit_u8(c, 0x71);  /* jno rel8 */
                    me_emit_u8(c, 0x02);  /* +2: skip ud2 */
                    me_emit_u8(c, 0x0F);
                    me_emit_u8(c, 0x0B);  /* ud2 */
                } else {
                    /* Narrow: sign-extension round-trip check.
                     * scratch = (phys << (64-N)) >> (64-N) [arithmetic].
                     * If scratch == phys, value fits in N bits → skip trap.
                     * Otherwise → overflow → trap. */
                    int shift = 64 - width * 8;
                    /* movq %phys, %scratch  (REX + 0x89 /r, mod=11, reg=phys, r/m=scratch) */
                    me_emit_u8(c, rex_mov);
                    me_emit_u8(c, 0x89);
                    me_emit_u8(c, 0xC0 | ((phys & 7) << 3) | (scratch & 7));

                    /* shq $shift, %scratch  (REX + C1 /4 ib, mod=11, reg=4=SHL, r/m=scratch) */
                    me_emit_u8(c, rex_alu);
                    me_emit_u8(c, 0xC1);
                    me_emit_u8(c, 0xC0 | (4 << 3) | (scratch & 7));
                    me_emit_u8(c, (uint8_t)shift);

                    /* sarq $shift, %scratch  (REX + C1 /7 ib, mod=11, reg=7=SAR, r/m=scratch) */
                    me_emit_u8(c, rex_alu);
                    me_emit_u8(c, 0xC1);
                    me_emit_u8(c, 0xC0 | (7 << 3) | (scratch & 7));
                    me_emit_u8(c, (uint8_t)shift);

                    /* cmp %scratch, %phys  (REX + 0x39 /r, mod=11, reg=scratch, r/m=phys;
                     * computes phys - scratch) */
                    me_emit_u8(c, rex_cmp);
                    me_emit_u8(c, 0x39);
                    me_emit_u8(c, 0xC0 | ((scratch & 7) << 3) | (phys & 7));

                    /* je rel8,+2 : if equal (fits), skip ud2 */
                    me_emit_u8(c, 0x74);  /* je rel8 */
                    me_emit_u8(c, 0x02);  /* +2: skip ud2 */
                    me_emit_u8(c, 0x0F);
                    me_emit_u8(c, 0x0B);  /* ud2 */
                }
                break;
            }

            case OP_NEG:
                if (BE(c)->emit_neg) {
                    BE(c)->emit_neg(c, phys_dst, phys_src1);
                }
                break;

            case OP_SEXT8: case OP_SEXT16: case OP_SEXT32:
            case OP_ZEXT8: case OP_ZEXT16: case OP_ZEXT32:
                if (BE(c)->emit_extend) {
                    int ext_width = (instr->op == OP_SEXT16 || instr->op == OP_ZEXT16) ? 2
                                     : (instr->op == OP_SEXT32 || instr->op == OP_ZEXT32) ? 4 : 1;
                    int ext_sign  = (instr->op >= OP_SEXT8 && instr->op <= OP_SEXT32) ? 1 : 0;
                    BE(c)->emit_extend(c, phys_dst, phys_src1, ext_width, ext_sign);
                }
                break;

            case OP_MUL:
                BE(c)->emit_mul(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_DIV:
                c->lowering.div_is_unsigned = zc_div_is_unsigned(c, instr);
                BE(c)->emit_div(c, phys_dst, phys_src1, phys_src2);
                c->lowering.div_is_unsigned = 0;
                break;

            case OP_MOD:
                c->lowering.div_is_unsigned = zc_div_is_unsigned(c, instr);
                BE(c)->emit_mod(c, phys_dst, phys_src1, phys_src2);
                c->lowering.div_is_unsigned = 0;
                break;

            case OP_AND:
                BE(c)->emit_and(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_OR:
                BE(c)->emit_or(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_XOR:
                BE(c)->emit_xor(c, phys_dst, phys_src1, phys_src2);
                break;

            /* GNU __int128 (Sprint 109): 128-bit ops on a (lo,hi) GPR128 pair.
             * phys_dst/phys_src1/phys_src2 are the low halves and g128_dhi/g128_s1hi/
             * g128_s2hi are the high halves, both spill-aware (a spilled GPR128 temp
             * is reloaded into a reserved scratch pair; see the block above). */
            case OP_ADD128:
                BE(c)->emit_add128(c, phys_dst, g128_dhi,
                    phys_src1, g128_s1hi,
                    phys_src2, g128_s2hi);
                break;
            case OP_SUB128:
                BE(c)->emit_sub128(c, phys_dst, g128_dhi,
                    phys_src1, g128_s1hi,
                    phys_src2, g128_s2hi);
                break;
            case OP_MUL128:
                BE(c)->emit_mul128(c, phys_dst, g128_dhi,
                    phys_src1, g128_s1hi,
                    phys_src2, g128_s2hi);
                break;
            case OP_AND128:
                BE(c)->emit_and128(c, phys_dst, g128_dhi,
                    phys_src1, g128_s1hi,
                    phys_src2, g128_s2hi);
                break;
            case OP_OR128:
                BE(c)->emit_or128(c, phys_dst, g128_dhi,
                    phys_src1, g128_s1hi,
                    phys_src2, g128_s2hi);
                break;
            case OP_XOR128:
                BE(c)->emit_xor128(c, phys_dst, g128_dhi,
                    phys_src1, g128_s1hi,
                    phys_src2, g128_s2hi);
                break;
            case OP_NOT128:
                BE(c)->emit_not128(c, phys_dst, g128_dhi,
                    phys_src1, g128_s1hi);
                break;
            case OP_NEG128:
                BE(c)->emit_neg128(c, phys_dst, g128_dhi,
                    phys_src1, g128_s1hi);
                break;
            case OP_MOV128:
                BE(c)->emit_mov128(c, phys_dst, g128_dhi,
                    phys_src1, g128_s1hi);
                break;
            case OP_MOV128_IMM:
                BE(c)->emit_mov128_imm(c, phys_dst, g128_dhi,
                    instr->imm64);
                break;
            case OP_LOAD128:
                BE(c)->emit_load128(c, phys_dst, g128_dhi,
                    phys_src1, 0);
                break;
            case OP_STORE128:
                BE(c)->emit_store128(c, phys_src1,
                    g128_s1hi,
                    phys_src2, 0);
                break;
            case OP_ZEXT64_128:
                BE(c)->emit_zext64_128(c, phys_dst, g128_dhi,
                    phys_src1);
                break;
            case OP_SEXT64_128:
                BE(c)->emit_sext64_128(c, phys_dst, g128_dhi,
                    phys_src1);
                break;
            case OP_TRUNC128_64:
                BE(c)->emit_trunc128_64(c, phys_dst, phys_src1);
                break;
            case OP_LO128:
                BE(c)->emit_mov_reg(c, phys_dst, phys_src1);  /* dest = src1.lo (phys_src1 is spill-aware) */
                break;
            case OP_HI128:
                BE(c)->emit_mov_reg(c, phys_dst, g128_s1hi);
                break;
            case OP_MK128:
                BE(c)->emit_mov_reg(c, phys_dst, phys_src1);  /* dest.lo = src1 */
                BE(c)->emit_mov_reg(c, g128_dhi, phys_src2);  /* dest.hi = src2 */
                break;

            case OP_SHL:
                BE(c)->emit_shl(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_SHR:
                BE(c)->emit_shr(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_CMP: {
                /* Store operands for RISC-V backend (flags materialization) */
                c->lowering.cmp_src1 = phys_src1;
                c->lowering.cmp_src2 = phys_src2;
                RegClass rc1 = regalloc_get_class_for_temp(&c->regalloc, instr->temp_src1);
                RegClass rc2 = regalloc_get_class_for_temp(&c->regalloc, instr->temp_src2);
                int cmp_fp = (rc1 == REG_CLASS_XMM || rc1 == REG_CLASS_VEC ||
                              rc2 == REG_CLASS_XMM || rc2 == REG_CLASS_VEC);
                c->lowering.cmp_is_fp = cmp_fp;
                if (cmp_fp && BE(c)->emit_fp_cmp) {
                    BE(c)->emit_fp_cmp(c, phys_src1, phys_src2);
                } else {
                    BE(c)->emit_cmp(c, phys_src1, phys_src2);
                }
                break;
            }

            case OP_COND_SET:
                /* set dst based on flags and condition code */
                if (BE(c)->emit_cond_set) {
                    BE(c)->emit_cond_set(c, phys_dst, (int)instr->cond);
                }
                break;

            case OP_DEREF:
                BE(c)->emit_deref(c, phys_dst, phys_src1);
                break;

            case OP_LOAD_BYTE:
                BE(c)->emit_load_byte(c, phys_dst, phys_src1);
                break;

            case OP_CMP_ZERO:
                /* RISC-V flags materialization: cmp-zero records the value
                 * against the zero register so a later OP_COND_SET can
                 * re-materialize the predicate (see live-range extension in
                 * live_range_compute for RISC-V). */
                {
                    RegClass rc0 = regalloc_get_class_for_temp(&c->regalloc, instr->temp_src1);
                    c->lowering.cmp_src1 = phys_src1;
                    c->lowering.cmp_src2 = 0;  /* zero register */
                    c->lowering.cmp_is_fp = (rc0 == REG_CLASS_XMM || rc0 == REG_CLASS_VEC);
                }
                BE(c)->emit_cmp_zero(c, phys_src1);
                break;

            case OP_CVTTSD2SI:
                if (BE(c)->emit_fp_cvtsi) {
                    BE(c)->emit_fp_cvtsi(c, phys_dst, phys_src1);  /* convert xmm<src> to GPR dst */
                }
                break;

            case OP_CVTSI2SD:
                if (BE(c)->emit_fp_cvtsf) {
                    BE(c)->emit_fp_cvtsf(c, phys_dst, phys_src1);  /* convert GPR src to xmm<dst> */
                }
                break;

            case OP_LOAD_16:
                if (BE(c)->emit_load_16) {
                    BE(c)->emit_load_16(c, phys_dst, phys_src1);
                }
                break;

            case OP_LOAD_32:
                if (BE(c)->emit_load_32) {
                    BE(c)->emit_load_32(c, phys_dst, phys_src1);
                }
                break;

            case OP_STORE_BYTE:
                if (BE(c)->emit_store_byte) {
                    BE(c)->emit_store_byte(c, phys_src1, phys_src2);
                }
                break;

            case OP_STORE_16:
                if (BE(c)->emit_store_16) {
                    BE(c)->emit_store_16(c, phys_src1, phys_src2);
                }
                break;

            case OP_STORE_32:
                if (BE(c)->emit_store_32) {
                    BE(c)->emit_store_32(c, phys_src1, phys_src2);
                }
                break;

            case OP_STORE_64:
                if (BE(c)->emit_store_64) {
                    BE(c)->emit_store_64(c, phys_src1, phys_src2);
                }
                break;

            case OP_VLA_ALLOC:
                c->lowering.vla.cur_sp_slot_disp = instr->imm;
                if (BE(c)->emit_vla_alloc) {
                    BE(c)->emit_vla_alloc(c, phys_dst, phys_src1);
                }
                break;

            case OP_CALLEE_SAVE_STORES:
                /* Handled in emit_prologue_mc at function entry */
                break;

            case OP_CALLEE_SAVE_LOADS:
                /* Emit callee-saved register loads using mask from IR instruction */
                /* Generic implementation using backend's callee_saved_gpr array and slot offsets */
                if (BE(c)->conv && BE(c)->conv->callee_saved_gpr && BE(c)->emit_load) {
                    uint32_t mask = (uint32_t)instr->imm64;
                    const int *callee_regs = BE(c)->conv->callee_saved_gpr;
                    const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
                    int count = BE(c)->conv->callee_saved_gpr_count;
                    for (int i = 0; i < count; i++) {
                        int reg = callee_regs[i];
                        if ((mask >> reg) & 1u) {
                            /* slot_offsets is indexed by array position (i), not physical reg number */
                            int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
                            BE(c)->emit_load(c, reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot));
                        }
                    }
                }
                break;

            case OP_ATOMIC_XADD:
                if (BE(c)->emit_atomic_xadd) {
                    BE(c)->emit_atomic_xadd(c, phys_dst, phys_src1, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;

            case OP_ATOMIC_CMPXCHG: {
                int phys_expected = (instr->label_idx == TEMP_NONE) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->label_idx);
                if (BE(c)->emit_atomic_cmpxchg) {
                    BE(c)->emit_atomic_cmpxchg(c, phys_dst, phys_src1, phys_expected, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;
            }

            case OP_ATOMIC_CMPXCHG_BOOL: {
                int phys_expected = (instr->label_idx == TEMP_NONE) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->label_idx);
                if (BE(c)->emit_atomic_cmpxchg_bool) {
                    BE(c)->emit_atomic_cmpxchg_bool(c, phys_dst, phys_src1, phys_expected, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;
            }

            case OP_ATOMIC_XCHG:
                if (BE(c)->emit_atomic_xchg) {
                    BE(c)->emit_atomic_xchg(c, phys_dst, phys_src1, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;

            case OP_ATOMIC_OR:
                if (BE(c)->emit_atomic_or) {
                    BE(c)->emit_atomic_or(c, phys_dst, phys_src1, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;

            case OP_ATOMIC_AND:
                if (BE(c)->emit_atomic_and) {
                    BE(c)->emit_atomic_and(c, phys_dst, phys_src1, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;

            case OP_ATOMIC_XOR:
                if (BE(c)->emit_atomic_xor) {
                    BE(c)->emit_atomic_xor(c, phys_dst, phys_src1, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;

            case OP_MFENCE:
                if (BE(c)->emit_mfence) {
                    BE(c)->emit_mfence(c);
                }
                break;
            case OP_TRAP:
                if (BE(c)->emit_trap) {
                    BE(c)->emit_trap(c);
                }
                break;

            case OP_SYSCALL: {
                /* OS syscall intrinsic. Arguments are packed like OP_CALL:
                 * temp_dest = arg_count, full list in the side table. The
                 * backend moves them into the syscall argument registers and
                 * issues the instruction; the result is in VREG_0 (rax). */
                if (!BE(c)->emit_syscall) {
                    c->had_error = true;
                    snprintf(c->error_msg, sizeof(c->error_msg),
                             "__zerocc_syscall is not supported on this target");
                    break;
                }
                int s_arg_temps[8] = {0};
                int s_arg_count = 0;
                const IRCallArgs *s_args = ir_call_args_find(c, i);
                if (s_args) {
                    for (int a = 0; a < s_args->count && a < 7; a++) {
                        if (s_args->is_fp[a] != 0) continue; /* syscall args are integer */
                        s_arg_temps[s_arg_count++] = (int)s_args->temps[a];
                    }
                }
                if (s_arg_count == 0 && instr->temp_dest > 0) {
                    /* C2b fallback (side table missing - should not happen). */
                    if (instr->temp_src1 != TEMP_NONE) s_arg_temps[s_arg_count++] = (int)instr->temp_src1;
                    if (instr->temp_dest > 1 && instr->temp_src2 != TEMP_NONE) s_arg_temps[s_arg_count++] = (int)instr->temp_src2;
                }
                BE(c)->emit_syscall(c, s_arg_count, s_arg_temps);
                break;
            }

            case OP_CLONE: {
                if (!BE(c)->emit_clone) {
                    c->had_error = true;
                    snprintf(c->error_msg, sizeof(c->error_msg),
                             "__zerocc_clone is not supported on this target");
                    break;
                }
                int s_arg_temps[8] = {0};
                int s_arg_count = 0;
                const IRCallArgs *s_args = ir_call_args_find(c, i);
                if (s_args) {
                    for (int a = 0; a < s_args->count && a < 5; a++) {
                        if (s_args->is_fp[a] != 0) continue;
                        s_arg_temps[s_arg_count++] = (int)s_args->temps[a];
                    }
                }
                if (s_arg_count == 0 && instr->temp_dest > 0) {
                    if (instr->temp_src1 != TEMP_NONE) s_arg_temps[s_arg_count++] = (int)instr->temp_src1;
                    if (instr->temp_dest > 1 && instr->temp_src2 != TEMP_NONE) s_arg_temps[s_arg_count++] = (int)instr->temp_src2;
                }
                BE(c)->emit_clone(c, s_arg_count, s_arg_temps);
                break;
            }

            case OP_FMOV_IMM: {
                /* Load float literal via RIP-relative offset in rodata */
                size_t rodata_offset = (size_t)instr->imm64;
                if (BE(c)->emit_load_fp_rodata) {
                    BE(c)->emit_load_fp_rodata(c, phys_dst, rodata_offset);
                }
                break;
            }

            case OP_FLOAD:
                if (BE(c)->emit_load_fp) {
                    BE(c)->emit_load_fp(c, phys_dst, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, (int32_t)instr->imm64));
                }
                break;

            case OP_FSTORE:
                if (BE(c)->emit_store_fp) {
                    BE(c)->emit_store_fp(c, phys_src1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, (int32_t)instr->imm64));
                }
                break;

            case OP_FADD:
                if (BE(c)->emit_fp_add) {
                    BE(c)->emit_fp_add(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_FSUB:
                if (BE(c)->emit_fp_sub) {
                    BE(c)->emit_fp_sub(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_FMUL:
                if (BE(c)->emit_fp_mul) {
                    BE(c)->emit_fp_mul(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_FDIV:
                if (BE(c)->emit_fp_div) {
                    BE(c)->emit_fp_div(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_FMOV:
                if (BE(c)->emit_fp_mov) {
                    BE(c)->emit_fp_mov(c, phys_dst, phys_src1);
                }
                break;

            /* =========================================================================
             * Vector Instruction Emission (SSE2/NEON/RVV)
             * ========================================================================= */

            case OP_VLOAD:
                if (c->emit_asm && BE(c)->emit_vload_asm) {
                    /* Assembly mode: emit textual assembly */
                    BE(c)->emit_vload_asm(c, phys_dst, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vload) {
                    /* JIT binary mode: emit machine code directly */
                    BE(c)->emit_vload(c, phys_dst, phys_src1, instr->imm);
                }
                break;

            case OP_VLOADSS:
                if (c->emit_asm && BE(c)->emit_vload_asm) {
                    BE(c)->emit_vload_asm(c, phys_dst, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vloadss) {
                    /* Scalar 4-byte float load (movss) */
                    BE(c)->emit_vloadss(c, phys_dst, phys_src1, instr->imm);
                }
                break;

            case OP_VLOADSD:
                if (c->emit_asm && BE(c)->emit_vload_asm) {
                    BE(c)->emit_vload_asm(c, phys_dst, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vloadsd) {
                    /* Scalar 8-byte float load (movsd) */
                    BE(c)->emit_vloadsd(c, phys_dst, phys_src1, instr->imm);
                }
                break;

            case OP_VSTORE:
                if (c->emit_asm && BE(c)->emit_vstore_asm) {
                    /* Assembly mode: emit textual assembly: emit_vstore_asm(c, src_val, base, offset, out) */
                    BE(c)->emit_vstore_asm(c, phys_src2, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vstore) {
                    /* JIT binary mode: emit machine code directly: emit_vstore(c, src_val, base, offset) */
                    BE(c)->emit_vstore(c, phys_src2, phys_src1, instr->imm);
                }
                break;

            case OP_VSTORESS:
                if (c->emit_asm && BE(c)->emit_vstore_asm) {
                    BE(c)->emit_vstore_asm(c, phys_src2, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vstoress) {
                    /* Scalar 4-byte float store (movss) */
                    BE(c)->emit_vstoress(c, phys_src2, phys_src1, instr->imm);
                }
                break;

            case OP_VSTORESD:
                if (c->emit_asm && BE(c)->emit_vstore_asm) {
                    BE(c)->emit_vstore_asm(c, phys_src2, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vstoredsd) {
                    /* Scalar 8-byte float store (movsd) */
                    BE(c)->emit_vstoredsd(c, phys_src2, phys_src1, instr->imm);
                }
                break;

            case OP_VADDSS:
                if (c->emit_asm && BE(c)->emit_vaddss_asm) {
                    BE(c)->emit_vaddss_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vaddps) {
                    /* Use VADDPS for scalar too (lower 32 bits) */
                    BE(c)->emit_vaddps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VSUBSS:
                if (c->emit_asm && BE(c)->emit_vsubss_asm) {
                    BE(c)->emit_vsubss_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vsubps) {
                    BE(c)->emit_vsubps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VMULSS:
                if (c->emit_asm && BE(c)->emit_vmulss_asm) {
                    BE(c)->emit_vmulss_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vmulps) {
                    BE(c)->emit_vmulps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VDIVSS:
                if (c->emit_asm && BE(c)->emit_vdivss_asm) {
                    BE(c)->emit_vdivss_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vdivps) {
                    BE(c)->emit_vdivps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VADDPS:
                if (c->emit_asm && BE(c)->emit_vaddps_asm) {
                    BE(c)->emit_vaddps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vaddps) {
                    BE(c)->emit_vaddps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VSUBPS:
                if (c->emit_asm && BE(c)->emit_vsubps_asm) {
                    BE(c)->emit_vsubps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vsubps) {
                    BE(c)->emit_vsubps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VMULPS:
                if (c->emit_asm && BE(c)->emit_vmulps_asm) {
                    BE(c)->emit_vmulps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vmulps) {
                    BE(c)->emit_vmulps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VDIVPS:
                if (c->emit_asm && BE(c)->emit_vdivps_asm) {
                    BE(c)->emit_vdivps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vdivps) {
                    BE(c)->emit_vdivps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VADDSD:
                if (BE(c)->emit_vaddsd_asm) {
                    BE(c)->emit_vaddsd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VSUBSD:
                if (BE(c)->emit_vsubsd_asm) {
                    BE(c)->emit_vsubsd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VMULSD:
                if (BE(c)->emit_vmulsd_asm) {
                    BE(c)->emit_vmulsd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VDIVSD:
                if (BE(c)->emit_vdivsd_asm) {
                    BE(c)->emit_vdivsd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VADDPD:
                if (c->emit_asm && BE(c)->emit_vaddpd_asm) {
                    BE(c)->emit_vaddpd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vaddpd) {
                    BE(c)->emit_vaddpd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VSUBPD:
                if (c->emit_asm && BE(c)->emit_vsubpd_asm) {
                    BE(c)->emit_vsubpd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vsubpd) {
                    BE(c)->emit_vsubpd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VMULPD:
                if (c->emit_asm && BE(c)->emit_vmulpd_asm) {
                    BE(c)->emit_vmulpd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vmulpd) {
                    BE(c)->emit_vmulpd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VDIVPD:
                if (c->emit_asm && BE(c)->emit_vdivpd_asm) {
                    BE(c)->emit_vdivpd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vdivpd) {
                    BE(c)->emit_vdivpd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VPADDD:
                if (c->emit_asm && BE(c)->emit_vpaddd_asm) {
                    BE(c)->emit_vpaddd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vpaddd) {
                    BE(c)->emit_vpaddd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VPSUBD:
                if (c->emit_asm && BE(c)->emit_vpsubd_asm) {
                    BE(c)->emit_vpsubd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vpsubd) {
                    BE(c)->emit_vpsubd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VPMULLD:
                if (c->emit_asm && BE(c)->emit_vpmulld_asm) {
                    BE(c)->emit_vpmulld_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vpmulld) {
                    BE(c)->emit_vpmulld(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VMOVSS:
                if (BE(c)->emit_vmovss_asm) {
                    BE(c)->emit_vmovss_asm(c, phys_dst, phys_src1, c->asm_out);
                }
                break;

            case OP_VMOVAPS:
                if (c->emit_asm && BE(c)->emit_vmovaps_asm) {
                    BE(c)->emit_vmovaps_asm(c, phys_dst, phys_src1, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vmovaps) {
                    BE(c)->emit_vmovaps(c, phys_dst, phys_src1);
                }
                break;

            case OP_VMOVAPD:
                if (BE(c)->emit_vmovapd_asm) {
                    BE(c)->emit_vmovapd_asm(c, phys_dst, phys_src1, c->asm_out);
                }
                break;

            case OP_VANDPS:
                if (BE(c)->emit_vandps_asm) {
                    BE(c)->emit_vandps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VORPS:
                if (BE(c)->emit_vorps_asm) {
                    BE(c)->emit_vorps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VXORPS:
                if (BE(c)->emit_vxorps_asm) {
                    BE(c)->emit_vxorps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VSHUFPS:
                if (BE(c)->emit_vshufps_asm) {
                    BE(c)->emit_vshufps_asm(c, phys_dst, phys_src1, phys_src2, instr->imm, c->asm_out);
                }
                break;

            case OP_VCVT_SI2SS:
                if (c->emit_asm && BE(c)->emit_vcvt_si2ss_asm) {
                    BE(c)->emit_vcvt_si2ss_asm(c, phys_dst, phys_src1, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_cvtsi2ss) {
                    BE(c)->emit_cvtsi2ss(c, phys_dst, phys_src1);
                }
                break;

            case OP_VCVT_SS2SI:
                if (BE(c)->emit_vcvt_ss2si_asm) {
                    BE(c)->emit_vcvt_ss2si_asm(c, phys_dst, phys_src1, c->asm_out);
                }
                break;

            case OP_CVTSS2SD:
                if (c->emit_asm && BE(c)->emit_cvtss2sd_asm) {
                    BE(c)->emit_cvtss2sd_asm(c, phys_dst, phys_src1, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_cvtss2sd) {
                    BE(c)->emit_cvtss2sd(c, phys_dst, phys_src1);
                }
                break;

            case OP_CVTSD2SS:
                if (c->emit_asm && BE(c)->emit_cvtsd2ss_asm) {
                    BE(c)->emit_cvtsd2ss_asm(c, phys_dst, phys_src1, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_cvtsd2ss) {
                    BE(c)->emit_cvtsd2ss(c, phys_dst, phys_src1);
                }
                break;

            /* Sprint 115m: raw 4-byte ABI-float access (no scalar-model
             * conversion) - JIT machine-code path only; the -S path is in
             * asm_emitter.c. */
            case OP_VLOADSS_RAW:
                if (!c->emit_asm && BE(c)->emit_vloadss_raw) {
                    BE(c)->emit_vloadss_raw(c, phys_dst, phys_src1, instr->imm);
                }
                break;

            case OP_VSTORESS_RAW:
                if (!c->emit_asm && BE(c)->emit_vstoress_raw) {
                    BE(c)->emit_vstoress_raw(c, phys_src2, phys_src1, instr->imm);
                }
                break;

            case OP_CVTTSD2SS:
                if (!c->emit_asm && BE(c)->emit_cvttsd2ss) {
                    BE(c)->emit_cvttsd2ss(c, phys_dst, phys_src1);
                }
                break;

            case OP_VCMPSS:
                if (BE(c)->emit_vcmpss_asm) {
                    BE(c)->emit_vcmpss_asm(c, phys_dst, phys_src1, phys_src2, instr->imm, c->asm_out);
                }
                break;

            case OP_LEA_RIP: {
                /* LEA RIP-relative for string literals */
                size_t rodata_offset = (size_t)instr->imm64;
                if (BE(c)->emit_lea_rip) {
                    BE(c)->emit_lea_rip(c, phys_dst, rodata_offset);
                }
                break;
            }

            case OP_LEA_LABEL: {
                if (BE(c)->emit_lea_label) {
                    size_t patch_pos = BE(c)->emit_lea_label(c, phys_dst);
                    if (instr->imm64 != 0) {
                        /* Cross-scope `&&label`: the label's OP_LABEL was
                         * emitted in an ANCESTOR function's pass (already
                         * complete), so its global text offset is in the
                         * persistent user_label_offsets table - patch
                         * directly, exactly like OP_NONLOCAL_GOTO. The
                         * owner is stored 1-based so 0 keeps meaning
                         * "current function". */
                        int owner_fn = (int)instr->imm64 - 1;
                        bool found = false;
                        for (size_t li = 0; li < c->user_label_offset_count; li++) {
                            if (c->user_label_offsets[li].func_idx == (size_t)owner_fn &&
                                c->user_label_offsets[li].ir_label == instr->label_idx) {
                                BE(c)->patch_jmp(c, patch_pos,
                                                  c->user_label_offsets[li].text_offset);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            diag_report_error_at_token(c, "internal: cross-scope label address not found");
                        }
                    } else if (instr->label_idx < label_count) {
                        size_t label_idx = instr->label_idx;
                        if (label_defined[label_idx]) {
                            BE(c)->patch_jmp(c, patch_pos, label_offsets[label_idx]);
                        } else {
                            if (label_patch_counts[label_idx] >= label_patch_caps[label_idx]) {
                                size_t new_cap = label_patch_caps[label_idx] ? label_patch_caps[label_idx] * 2 : 4;
                                size_t *new_patches = (size_t *)realloc(label_patches[label_idx], new_cap * sizeof(size_t));
                                if (new_patches) {
                                    label_patches[label_idx] = new_patches;
                                    label_patch_caps[label_idx] = new_cap;
                                }
                            }
                            if (label_patches[label_idx] && label_patch_counts[label_idx] < label_patch_caps[label_idx]) {
                                label_patches[label_idx][label_patch_counts[label_idx]++] = patch_pos;
                            }
                        }
                    }
                }
                break;
            }

            case OP_MOV_GLOBAL: {
                /* Global variable load with relocation */
                const char *symbol_name = (const char *)(intptr_t)instr->imm64;
                if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                    /* RISC-V: auipc dst, 0; addi dst, dst, 0 */
                    int r = phys_dst & 0x1F;
                    uint32_t auipc = (uint32_t)(((uint32_t)r << 7) | 0x17u);
                    uint32_t addi  = (uint32_t)(((uint32_t)r << 15) | ((uint32_t)r << 7) | 0x13u);
                    zero_cc_emit_u32(&c->text_section, auipc);
                    zero_cc_emit_u32(&c->text_section, addi);
                } else if (c->isa == ISA_AARCH64) {
                    /* AArch64: adrp dst, 0; add dst, dst, 0 */
                    int r = phys_dst & 0x1F;
                    uint32_t adrp = 0x90000000u | (uint32_t)r;
                    uint32_t add  = 0x91000000u | ((uint32_t)r << 5) | (uint32_t)r;
                    zero_cc_emit_u32(&c->text_section, adrp);
                    zero_cc_emit_u32(&c->text_section, add);
                } else if (BE(c)->emit_mov_imm) {
                    BE(c)->emit_mov_imm(c, phys_dst, 0);
                }
                /* Store symbol name for relocation patching. x86-64 movabs is
                 * 10 bytes (8-byte reloc field at the end); RISC-V emits an
                 * auipc+addi pair; AArch64 emits an adrp+add pair (4-byte lo12 reloc
                 * field at the end, companion hi21/pcrel_hi added by ELF writer). */
                size_t reloc_field = (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32 || c->isa == ISA_AARCH64) ? 4 : 8;
                if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
                    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                    r->text_patch_offset = BE(c)->get_code_offset(c) - reloc_field;
                    snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", symbol_name);
                    r->is_call = false;
                } else {
                    zero_cc_reloc_overflow_note(c);
                }
                break;
            }

            case OP_TLS_LEA: {
                /* Address of a _Thread_local static (initial-exec, x86-64
                 * AOT only).  The canonical idiom (the same one GCC emits
                 * for &tlsvar), with both halves in the destination
                 * register so no scratch is needed:
                 *   movq %fs:0, dst            (64 48 8B 04 25 00*4)
                 *   leaq $var@tpoff(dst), dst  (48 8D modrm disp32)
                 * with an R_X86_64_TPOFF32 relocation over the lea's
                 * 32-bit displacement (S + A - G), so the register ends
                 * up holding thread_ptr + (S - G) = the variable address.
                 * JIT mode never reaches here (TLS vars are plain globals
                 * there, lowered as OP_MOV_GLOBAL). */
                const char *symbol_name = (const char *)(intptr_t)instr->imm64;
                if (c->isa != ISA_X86_64) {
                    /* Defensive: the TLS backend rejects non-x86-64 at
                     * declaration time, so this is unreachable in practice. */
                    BE(c)->emit_mov_imm(c, phys_dst, 0);
                    break;
                }
                int d = phys_dst & 7;
                int r_bit = (phys_dst >> 3) & 1;
                /* movq %fs:0, dst */
                zero_cc_emit_u8(&c->text_section, 0x64);                 /* FS override */
                zero_cc_emit_u8(&c->text_section, 0x48 | (r_bit << 2));  /* REX.W|R */
                zero_cc_emit_u8(&c->text_section, 0x8B);                 /* mov r64, r/m64 */
                zero_cc_emit_u8(&c->text_section, (d << 3) | 4);         /* mod=00 reg=dst rm=101 */
                zero_cc_emit_u8(&c->text_section, 0x25);                 /* SIB: disp32-only */
                zero_cc_emit_u32(&c->text_section, 0);                   /* disp32 = 0 */
                /* leaq $var@tpoff, dst: mod=10, reg=dst, rm=dst */
                zero_cc_emit_u8(&c->text_section, 0x48 | (r_bit << 2) | r_bit); /* REX.W|R|B */
                zero_cc_emit_u8(&c->text_section, 0x8D);                 /* lea r64, m */
                zero_cc_emit_u8(&c->text_section, 0x80 | (d << 3) | d);  /* mod=10 reg=dst rm=dst */
                if (d == 4) zero_cc_emit_u8(&c->text_section, 0x24);     /* SIB for rsp base */
                zero_cc_emit_u32(&c->text_section, 0);                   /* disp32 <- TPOFF32 */
                if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
                    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                    r->text_patch_offset = BE(c)->get_code_offset(c) - 4;
                    snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", symbol_name);
                    r->is_call = false;
                    r->is_tls = true;
                } else {
                    zero_cc_reloc_overflow_note(c);
                }
                break;
            }

            case OP_JMP_LABEL: {
                /* Unconditional jump to label (for break/continue/goto) */
                BE(c)->emit_jmp(c, "");
                /* Store patch info for later resolution */
                /* Note: label resolution is handled separately in emit_return_jumps */
                break;
            }

            case OP_CALL: {
                /* Extract function name from imm64 (stored as pointer during lowering) */
                const char *fn_name = (const char *)(intptr_t)instr->imm64;

                /* Extract integer argument info packed into the instruction */
                int arg_count = instr->temp_dest;
                int arg_temps[IR_CALL_MAX_ARGS] = {0};
                int indirect_callee_temp = -1;
                int aarch64_sret_temp = -1;
                const IRCallArgs *full_args = ir_call_args_find(c, i);
                if (full_args) {
                    int int_c = 0;
                    for (int a = 0; a < full_args->count; a++) {
                        if (full_args->is_fp[a] == 2) {
                            indirect_callee_temp = full_args->temps[a];
                        } else if (full_args->is_fp[a] == 3) {
                            aarch64_sret_temp = full_args->temps[a];
                        } else if (full_args->is_fp[a] == 0) {
                            arg_temps[int_c++] = full_args->temps[a];
                        }
                    }
                    arg_count = int_c;
                } else {
                    /* C2b fallback (side table missing - should not happen):
                     * only args 0/1 are recoverable from the instruction. */
                    if (arg_count > 2) arg_count = 2;
                    if (arg_count > 0) arg_temps[0] = instr->temp_src1;
                    if (arg_count > 1) arg_temps[1] = instr->temp_src2;
                }

                /* If indirect call, save callee register into scratch_reg2 before argument setup */
                if (!fn_name && indirect_callee_temp >= 0) {
                    int r_callee = regalloc_get_reg_for_temp(&c->regalloc, indirect_callee_temp);
                    int sp_callee = regalloc_get_spill_slot_for_temp(&c->regalloc, indirect_callee_temp);
                    if (sp_callee >= 0) {
                        int32_t off = spill_base_offset + (sp_callee + 1) * 8;
                        if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg2, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
                    } else if (BE(c)->emit_mov_reg) {
                        BE(c)->emit_mov_reg(c, BE(c)->scratch_reg2, r_callee);
                    }
                }

                /* If AArch64 sret (AAPCS64), move sret buffer address into x8 (Indirect Result Location Register) */
                if (aarch64_sret_temp >= 0) {
                    int r_sret = regalloc_get_reg_for_temp(&c->regalloc, aarch64_sret_temp);
                    int sp_sret = regalloc_get_spill_slot_for_temp(&c->regalloc, aarch64_sret_temp);
                    if (sp_sret >= 0) {
                        int32_t off = spill_base_offset + (sp_sret + 1) * 8;
                        if (BE(c)->emit_load) BE(c)->emit_load(c, 8 /* x8 */, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
                    } else if (BE(c)->emit_mov_reg) {
                        BE(c)->emit_mov_reg(c, 8 /* x8 */, r_sret);
                    }
                }

                /* Extract fp_reg_idx (number of float args) from label_idx */
                int fp_reg_idx = (int)(instr->label_idx & 0xFF);
                if ((instr->label_idx & 0xFF00) == 0x0100) {
                    fp_reg_idx = 0;
                }

                /* Load float arguments from their spill slots into the FP
                 * argument registers of the target ABI (xmm0..7 / d0..7 /
                 * fa0..7), using the calling-convention table (x86/AArch64
                 * arg regs 0..7, RISC-V fa0..fa7).
                 * MUST run before the stack-argument pushes below: those
                 * shift the stack pointer (n_stack*8 + stack_pad), and the
                 * frame-relative slot offsets would then address the wrong
                 * bytes (mispassing every float arg on 7+ int args). The
                 * -S path (x86_emit_call_asm & co) emits FP args first for
                 * the same reason. */
                int cidx = call_idx;
                bool have_fp_table = (call_idx < c->lowering.ir_ctx.call_count);
                if (have_fp_table) {
                    int fp_count = c->lowering.ir_ctx.call_fp_spill_counts[call_idx];
                    int fp_reg_cap = BE(c)->conv ? BE(c)->conv->fp_arg_reg_count : 8;
                    for (int f = 0; f < fp_count && f < fp_reg_cap; f++) {
                        int32_t spill_off = c->lowering.ir_ctx.call_fp_spills[call_idx][f];
                        int fp_arg_reg = backend_get_fp_arg_reg(BE(c), f);
                        if (fp_arg_reg < 0) fp_arg_reg = f;
                        /* Load FP arg from stack slot into FP register */
                        if (BE(c)->emit_load_fp) {
                            BE(c)->emit_load_fp(c, fp_arg_reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -spill_off));
                        }
                    }
                } else if (full_args) {
                    /* Capped-table fallback: the call_fp_spills side table
                     * covers only the first 64 calls of a function (see
                     * lowering_state.h); for calls past the cap, load the
                     * FP register args from their reload temps - the same
                     * mechanism the -S emitter uses (x86_emit_call_asm).
                     * Without this, functions with >64 calls pass garbage
                     * FP args on the MC path while -S/JIT-via-asm work. */
                    int fp_reg_cap = BE(c)->conv ? BE(c)->conv->fp_arg_reg_count : 8;
                    int fp_seen = 0;
                    for (int ai = 0; ai < full_args->count && fp_seen < fp_reg_cap; ai++) {
                        if (full_args->is_fp[ai] != 1) continue;
                        if (full_args->stack_pos[ai] < 255) continue; /* FP stack arg, pushed below */
                        int temp = full_args->temps[ai];
                        int fp_arg_reg = backend_get_fp_arg_reg(BE(c), fp_seen);
                        if (fp_arg_reg < 0) break;
                        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
                        if (sp >= 0) {
                            int32_t off = spill_base_offset + (sp + 1) * 8;
                            if (BE(c)->emit_load_fp) {
                                BE(c)->emit_load_fp(c, fp_arg_reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
                            }
                        } else {
                            int phys = regalloc_get_reg_for_temp(&c->regalloc, temp);
                            if (phys >= 0 && phys != fp_arg_reg && BE(c)->emit_fp_mov) {
                                BE(c)->emit_fp_mov(c, fp_arg_reg, phys);
                            }
                        }
                        fp_seen++;
                    }
                }
                call_idx++;

                /* Stack args: everything the lowering marked stack_pos < 255
                 * (integer args beyond the GP registers, FP args beyond the
                 * FP registers). They occupy the call stack in original
                 * argument order, so push in DESCENDING stack_pos order,
                 * padding to keep the stack 16-byte aligned. FP stack args
                 * are re-read as their 8-byte bit pattern from the same
                 * spill slot the lowering stored (their XMM temp is unused).
                 * (C2b fallback without the side table: the old int-only
                 * formula. */
                int int_arg_count = BE(c)->conv->int_arg_reg_count;
                int n_stack;
                int stack_meta_temp[IR_CALL_MAX_ARGS];  /* per stack slot: int arg temp, -1 for FP */
                int stack_meta_fp[IR_CALL_MAX_ARGS];    /* per stack slot: FP stream index, -1 for int */
                if (full_args) {
                    n_stack = 0;
                    int fp_seen = 0;
                    for (int i = 0; i < full_args->count; i++) {
                        if (full_args->is_fp[i] == 1) {
                            int f = fp_seen++;
                            int pos = full_args->stack_pos[i];
                            if (pos < 255 && pos < IR_CALL_MAX_ARGS) {
                                stack_meta_temp[pos] = -1;
                                stack_meta_fp[pos] = f;
                                if (pos + 1 > n_stack) n_stack = pos + 1;
                            }
                            continue;
                        }
                        if (full_args->is_fp[i] == 2 || full_args->is_fp[i] == 3) continue;
                        int pos = full_args->stack_pos[i];
                        if (pos < 255 && pos < IR_CALL_MAX_ARGS) {
                            stack_meta_temp[pos] = full_args->temps[i];
                            stack_meta_fp[pos] = -1;
                            if (pos + 1 > n_stack) n_stack = pos + 1;
                        }
                    }
                } else {
                    n_stack = (arg_count > int_arg_count) ? arg_count - int_arg_count : 0;
                }
                int stack_pad = (n_stack & 1) ? 8 : 0;
                int32_t extra_sp = 0;
                if (n_stack > 0) {
                    if (stack_pad) {
                        BE(c)->emit_stack_adjust(c, -stack_pad);
                        extra_sp += stack_pad;
                    }
                    for (int pos = n_stack - 1; pos >= 0; pos--) {
                        int r;
                        bool fp_stack_done = false;
                        if (full_args && stack_meta_fp[pos] >= 0) {
                            /* FP stack arg: bits live in the lowering's spill
                             * slot (or, past the 64-call table cap, in the
                             * arg's reload temp). */
                            if (have_fp_table) {
                                int f = stack_meta_fp[pos];
                                int32_t fslot = c->lowering.ir_ctx.call_fp_spills[cidx][f];
                                if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -fslot, extra_sp));
                                r = BE(c)->scratch_reg1;
                            } else {
                                int t = -1;
                                for (int ai = 0; ai < full_args->count; ai++) {
                                    if (full_args->is_fp[ai] == 1 && full_args->stack_pos[ai] == pos) {
                                        t = full_args->temps[ai];
                                        break;
                                    }
                                }
                                r = BE(c)->scratch_reg1;
                                if (t >= 0) {
                                    int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, t);
                                    if (sp >= 0) {
                                        int32_t off = spill_base_offset + (sp + 1) * 8;
                                        if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, extra_sp));
                                    } else {
                                        /* Bits live in the reload temp's FP
                                         * register: store them just below RSP
                                         * and claim that slot (same shape as
                                         * the -S emitter's FP stack args). */
                                        int phys = regalloc_get_reg_for_temp(&c->regalloc, t);
                                        if (phys >= 0 && BE(c)->emit_store_fp) {
                                            BE(c)->emit_store_fp(c, phys, BE(c)->sp_reg, -8);
                                            BE(c)->emit_stack_adjust(c, -8);
                                            extra_sp += 8;
                                            fp_stack_done = true;
                                        }
                                    }
                                }
                            }
                        } else if (full_args) {
                            int t = stack_meta_temp[pos];
                            r = regalloc_get_reg_for_temp(&c->regalloc, t);
                            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, t);
                            if (sp >= 0) {
                                int32_t off = spill_base_offset + (sp + 1) * 8;
                                if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, extra_sp));
                                r = BE(c)->scratch_reg1;
                            }
                        } else {
                            /* C2b fallback: int args only, old layout */
                            int a = int_arg_count + (n_stack - 1 - pos);
                            r = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[a]);
                            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, arg_temps[a]);
                            if (sp >= 0) {
                                int32_t off = spill_base_offset + (sp + 1) * 8;
                                if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, extra_sp));
                                r = BE(c)->scratch_reg1;
                            }
                        }
                        if (fp_stack_done) continue; /* slot already claimed via [rsp-8] store */
                        BE(c)->emit_push_reg(c, r);
                        extra_sp += 8;
                    }
                }

                /* Safe parallel move: push all src registers then pop into dst registers */
                int n_args = (arg_count < int_arg_count) ? arg_count : int_arg_count;
                int src_regs[8];
                for (int a = 0; a < n_args; a++) {
                    int r = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[a]);
                    int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, arg_temps[a]);
                    if (sp >= 0) {
                        int32_t off = spill_base_offset + (sp + 1) * 8;
                        if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, extra_sp));
                        r = BE(c)->scratch_reg1;
                    }
                    src_regs[a] = r;
                    BE(c)->emit_push_reg(c, src_regs[a]);
                    extra_sp += 8;
                }
                for (int a = n_args - 1; a >= 0; a--) {
                    int dst_reg = backend_get_int_arg_reg(BE(c), a);
                    if (dst_reg < 0) {
                        /* Argument is stack-passed, skip register assignment */
                        continue;
                    }
                    BE(c)->emit_pop_reg(c, dst_reg);
                    extra_sp -= 8;
                }

                /* System V AMD64 ABI: For variadic functions, %al must hold number of SSE registers used */
                /* TODO: Check if function is variadic - for now emit for all calls */
                if (BE(c)->emit_variadic_al_setup) {
                    BE(c)->emit_variadic_al_setup(c, fp_reg_idx);
                }

                /* Caller-saved register fix for AArch64:
                 * x0-x18 are caller-saved. The callee may clobber these registers.
                 * We need to save/restore any caller-saved registers that contain
                 * live values across the call.
                 * For now, always save x3-x7 BEFORE the call and restore after.
                 * TODO: Proper fix is to mark intervals as spanning calls in register allocator.
                 */
                if (c->isa == ISA_AARCH64) {
                    /* Save x3-x7 before the call */
                    for (int r = 3; r <= 7; r++) {
                        int spill = regalloc_get_spill_slot_for_temp(&c->regalloc, r);
                        if (spill >= 0) {
                            /* Register r has a spill slot - save it */
                            int32_t off = spill_base_offset + (spill + 1) * 8;
                            if (BE(c)->emit_store) BE(c)->emit_store(c, r, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, extra_sp));
                        }
                    }
                }

                /* Emit the call */
                if (!fn_name && indirect_callee_temp >= 0 && BE(c)->emit_call_reg) {
                    BE(c)->emit_call_reg(c, BE(c)->scratch_reg2 /* r11 */);
                } else {
                    BE(c)->emit_call(c, fn_name);
                }

                /* Restore x3-x7 after the call */
                if (c->isa == ISA_AARCH64) {
                    for (int r = 3; r <= 7; r++) {
                        int spill = regalloc_get_spill_slot_for_temp(&c->regalloc, r);
                        if (spill >= 0) {
                            /* Register r has a spill slot - restore it */
                            int32_t off = spill_base_offset + (spill + 1) * 8;
                            if (BE(c)->emit_load) BE(c)->emit_load(c, r, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, extra_sp));
                        }
                    }
                }

                /* Caller cleans up stack-passed arguments (and alignment pad) */
                if (n_stack > 0) {
                    BE(c)->emit_stack_adjust(c, n_stack * 8 + stack_pad);
                }

                /* Handle struct return (9-16 bytes) unpacking */
                if ((instr->label_idx & 0xFF00) == 0x0100) {
                    int struct_size = instr->label_idx & 0xFF;
                    if (struct_size > 8 && struct_size <= 16) {
                        /* Physical rdx contains upper 8 bytes */
                        /* This is handled by caller store logic */
                    }
                }
                break;
            }

            case OP_NOT:
                BE(c)->emit_not(c, phys_dst, phys_src1);
                break;

            case OP_LNOT:  /* Logical NOT: cmp_zero src; cond_set dst, EQ */
                BE(c)->emit_cmp_zero(c, phys_src1);
                if (BE(c)->emit_cond_set) {
                    BE(c)->emit_cond_set(c, phys_dst, CC_EQ);
                }
                break;

            case OP_LABEL:
                if (instr->label_idx < label_count) {
                    size_t label_idx = instr->label_idx;
                    /* Sprint 99: a nonlocal-goto landing point in a
                     * frame-pointer-omitted function must re-establish the
                     * entry-time FP register (the jump restored the target
                     * anchor, but the OFP epilogue never puts the caller's
                     * value back). The label must point AT the restore
                     * instruction, so record the offset first, then emit.
                     * Local jumps landing here are unaffected: in OFP mode
                     * the FP register already holds the entry value. */
                    bool nl_landing = false;
                    if (c->lowering.omit_frame_pointer && c->lowering.fp_snap_off > 0 &&
                        BE(c)->emit_load) {
                        const ZeroCCFunction *lf = &c->functions[c->lowering.current_func_idx];
                        for (int li = 0; li < lf->nl_label_count; li++) {
                            if (lf->nl_label_ir[li] == (uint16_t)label_idx) {
                                nl_landing = true;
                                break;
                            }
                        }
                    }
                    label_defined[label_idx] = true;
                    label_offsets[label_idx] = BE(c)->get_code_offset(c);
                    if (nl_landing) {
                        BE(c)->emit_load(c, BE(c)->fp_reg, zero_cc_frame_base_reg(c),
                                         zero_cc_frame_disp(c, -c->lowering.fp_snap_off));
                    }
                    for (size_t j = 0; j < label_patch_counts[label_idx]; j++) {
                        BE(c)->patch_jmp(c, label_patches[label_idx][j], label_offsets[label_idx]);
                    }
                    label_patch_counts[label_idx] = 0;
                    if (c->user_label_offset_count < 256) {
                        c->user_label_offsets[c->user_label_offset_count].func_idx = c->lowering.current_func_idx;
                        c->user_label_offsets[c->user_label_offset_count].ir_label = (uint16_t)label_idx;
                        c->user_label_offsets[c->user_label_offset_count].text_offset = label_offsets[label_idx];
                        c->user_label_offset_count++;
                    }
                }
                break;

            case OP_JMP:
            case OP_JE:
            case OP_JNE:
            case OP_JL:
            case OP_JLE:
            case OP_JG:
            case OP_JCOND: {
                if (instr->op == OP_JMP) {
                    /* Fallthrough optimization: skip jump if next active instruction is target label */
                    size_t next_idx = i + 1;
                    while (next_idx < count && (instrs[next_idx].op == OP_DEAD || instrs[next_idx].op == OP_NOP)) {
                        next_idx++;
                    }
                    if (next_idx < count && instrs[next_idx].op == OP_LABEL && instrs[next_idx].label_idx == instr->label_idx) {
                        break;
                    }
                    /* Option 2: Inline epilogue on early returns when no static-local mirror or CSR reload is required */
                    if (instr->label_idx == c->lowering.epilogue_label &&
                        !c->lowering.has_stack_protector &&
                        c->lowering.static_local_rt_off == 0 &&
                        c->regalloc.used_callee_saved_gpr_mask == 0 &&
                        BE(c)->emit_leave && BE(c)->emit_ret) {
                        BE(c)->emit_leave(c);
                        BE(c)->emit_ret(c);
                        break;
                    }
                }
                size_t patch_pos = 0;
                switch (instr->op) {
                    case OP_JMP: patch_pos = BE(c)->emit_jmp(c, ""); break;
                    case OP_JE:
                        BE(c)->emit_test(c, phys_src1, phys_src1);
                        patch_pos = BE(c)->emit_jeq(c, "");
                        break;
                    case OP_JNE:
                        BE(c)->emit_test(c, phys_src1, phys_src1);
                        patch_pos = BE(c)->emit_jne(c, "");
                        break;
                    case OP_JL:  patch_pos = BE(c)->emit_jlt(c, ""); break;
                    case OP_JLE: patch_pos = BE(c)->emit_jle(c, ""); break;
                    case OP_JG:  patch_pos = BE(c)->emit_jgt(c, ""); break;
                    case OP_JCOND:
                        patch_pos = BE(c)->emit_jcond(c, (int)instr->temp_src1, "");
                        break;
                    default: break;
                }
                if (instr->label_idx < label_count) {
                    size_t label_idx = instr->label_idx;
                    if (label_defined[label_idx]) {
                        BE(c)->patch_jmp(c, patch_pos, label_offsets[label_idx]);
                    } else {
                        if (label_patch_counts[label_idx] >= label_patch_caps[label_idx]) {
                            size_t new_cap = label_patch_caps[label_idx] ? label_patch_caps[label_idx] * 2 : 4;
                            size_t *new_patches = (size_t *)realloc(label_patches[label_idx], new_cap * sizeof(size_t));
                            if (new_patches) {
                                label_patches[label_idx] = new_patches;
                                label_patch_caps[label_idx] = new_cap;
                            }
                        }
                        if (label_patches[label_idx] && label_patch_counts[label_idx] < label_patch_caps[label_idx]) {
                            label_patches[label_idx][label_patch_counts[label_idx]++] = patch_pos;
                        }
                    }
                }
                break;
            }

            case OP_CALL_INDIRECT:
                /* Call through register pointer */
                if (BE(c)->emit_call_reg) {
                    BE(c)->emit_call_reg(c, phys_src1);
                }
                break;

            case OP_JMP_INDIRECT:
                /* Jump through register pointer */
                if (BE(c)->emit_jmp_reg) {
                    BE(c)->emit_jmp_reg(c, phys_src1);
                }
                break;

            case OP_SETJMP: {
                int s2 = BE(c)->scratch_reg2;
                int ret_reg = BE(c)->get_int_reg ? BE(c)->get_int_reg(VREG_0) : 0;
                int32_t w = (c->isa == ISA_RISCV32) ? 4 : 8;
                /* buf[0] = FP, buf[2] = SP (via s2 for AArch64 SP encoding),
                 * buf[1] = .Lsetjmp_resume PC, VREG_0 = 0, then .Lsetjmp_resume: */
                BE(c)->emit_store(c, BE(c)->fp_reg, phys_src1, 0);
                BE(c)->emit_mov_reg(c, s2, BE(c)->sp_reg);
                BE(c)->emit_store(c, s2, phys_src1, 2 * w);
                size_t patch_pos = BE(c)->emit_lea_label(c, s2);
                BE(c)->emit_store(c, s2, phys_src1, 1 * w);
                BE(c)->emit_mov_imm(c, ret_reg, 0);
                BE(c)->patch_jmp(c, patch_pos, BE(c)->get_code_offset(c));
                break;
            }

            case OP_LONGJMP: {
                int s1 = BE(c)->scratch_reg1;
                int s2 = BE(c)->scratch_reg2;
                int ret_reg = BE(c)->get_int_reg ? BE(c)->get_int_reg(VREG_0) : 0;
                int32_t w = (c->isa == ISA_RISCV32) ? 4 : 8;
                /* s1 = buf (never aliases phys_src2), VREG_0 = val,
                 * FP = buf[0], SP = buf[2], jmp *buf[1] */
                BE(c)->emit_mov_reg(c, s1, phys_src1);
                BE(c)->emit_mov_reg(c, ret_reg, phys_src2);
                BE(c)->emit_load(c, BE(c)->fp_reg, s1, 0);
                BE(c)->emit_load(c, s2, s1, 2 * w);
                BE(c)->emit_mov_reg(c, BE(c)->sp_reg, s2);
                BE(c)->emit_load(c, s2, s1, 1 * w);
                BE(c)->emit_jmp_reg(c, s2);
                break;
            }

            case OP_NONLOCAL_GOTO: {
                /* Sprint 99: restore the label's scope (rbp, rsp) from the
                 * hidden pair params, then jump to the ancestor label.
                 * The ancestor was compiled first, so its label text
                 * offsets are already in user_label_offsets. */
                int target_fn = (int)instr->imm64;
                size_t target_off = 0;
                bool found = false;
                for (size_t li = 0; li < c->user_label_offset_count; li++) {
                    if (c->user_label_offsets[li].func_idx == (size_t)target_fn &&
                        c->user_label_offsets[li].ir_label == instr->label_idx) {
                        target_off = c->user_label_offsets[li].text_offset;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    diag_report_error_at_token(c, "internal: nonlocal goto target label not found");
                    break;
                }
                BE(c)->emit_mov_reg(c, BE(c)->fp_reg, phys_src1);
                BE(c)->emit_mov_reg(c, BE(c)->sp_reg, phys_src2);
                if (c->isa == ISA_X86_64) {
                    size_t patch_pos = BE(c)->emit_jmp(c, "");
                    BE(c)->patch_jmp(c, patch_pos, target_off);
                } else if (c->isa == ISA_AARCH64) {
                    zero_cc_aarch64_emit_nonlocal_goto_jump(c, target_off);
                } else if (c->isa == ISA_RISCV64) {
                    zero_cc_riscv_emit_nonlocal_goto_jump(c, target_off);
                } else {
                    diag_report_error_at_token(c,
                        "nonlocal goto is not supported on this target "
                        "(RISC-V32 has no trampoline / pair plumbing)");
                }
                break;
            }

            case OP_PUSH:
                /* Push register to stack */
                if (BE(c)->emit_push_reg) {
                    BE(c)->emit_push_reg(c, phys_src1);
                }
                break;

            case OP_POP:
                /* Pop register from stack */
                if (BE(c)->emit_pop_reg) {
                    BE(c)->emit_pop_reg(c, phys_dst);
                }
                break;

            case OP_ADD_IMM:
                /* Add immediate to register */
                if (BE(c)->emit_add_imm) {
                    BE(c)->emit_add_imm(c, phys_dst, (int16_t)instr->imm);
                }
                break;

            case OP_SUB_IMM:
                /* Subtract immediate from register */
                if (BE(c)->emit_sub_imm) {
                    BE(c)->emit_sub_imm(c, phys_dst, (int16_t)instr->imm);
                }
                break;

            case OP_INC:
                /* Increment register */
                if (BE(c)->emit_inc) {
                    BE(c)->emit_inc(c, phys_dst);
                }
                break;

            case OP_DEC:
                /* Decrement register */
                if (BE(c)->emit_dec) {
                    BE(c)->emit_dec(c, phys_dst);
                }
                break;

            case OP_SAR:
                /* Arithmetic shift right */
                if (BE(c)->emit_sar) {
                    BE(c)->emit_sar(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_RET:
                /* Return from function - already handled by emit_leave/emit_ret after loop */
                break;

            case OP_EPILOGUE:
                /* Function epilogue - already handled by emit_leave/emit_ret after loop */
                break;

            case OP_STACK_ALIGN:
                /* Align stack pointer */
                if (BE(c)->emit_stack_align) {
                    BE(c)->emit_stack_align(c, (uint8_t)instr->imm);
                }
                break;

            default:
                break;
        }

        /* C99 width semantics (Sprint 119, slice 2): canonicalize a
         * narrow-int result to its C width before any spill-back so
         * the stored value is the extended one. Mark-first: a C99
         * width mark is a per-op parameter (byte widths 1/2/4 signed
         * or 8+1/2/4 unsigned, plus bitfield bit widths 3..7) and a
         * marked op is never OVF-adjacent - the zero_san trap
         * consumes the mark slot (see below). OP_OVF_TRAP is never
         * extended itself: its width is CHECK metadata, not a value
         * mark.
         *
         * Clean-value deferral: when the preceding ALU op's mark was
         * stolen by this OP_OVF_TRAP, the check must see the raw
         * 64-bit result and the flags the ALU op set. The trap frame
         * therefore emits the ALU result's tag-driven C99 extend
         * AFTER the check (same temp => same physical register). On
         * values that survive the check that deferred extend is a
         * provable no-op: the check traps exactly when the extension
         * would change the value (the 64-bit result of a narrow
         * signed add/sub/mul already carries the sign extension in
         * its upper bits when it fits; width-8 needs no extend). An
         * in-place SEXTn/ZEXTn on the same temp after the trap makes
         * the deferred extend redundant too. */
        {
            const Instr *ext = NULL;
            int ew = 0, esign = 0;
            if (instr->op == OP_OVF_TRAP && i > 0) {
                const Instr *pv = &instrs[i - 1];
                if (pv->op != OP_OVF_TRAP && pv->temp_dest != TEMP_NONE &&
                    pv->temp_dest == instr->temp_dest &&
                    zc_tag_extend(c, pv, &ew, &esign) &&
                    !zc_next_canonicalizes(c,
                                           i + 1 < count ? &instrs[i + 1] : NULL,
                                           instr->temp_dest)) {
                    ext = pv;
                }
            } else if (instr->op != OP_OVF_TRAP && instr->temp_dest != TEMP_NONE &&
                       zc_mark_extend(instr, &ew, &esign)) {
                ext = instr;
            }
            if (ext && BE(c)->emit_extend) {
                BE(c)->emit_extend(c, phys_dst, phys_dst, ew, esign);
            }
        }

        /* If destination temp was spilled, store the scratch register back
         * to the frame spill slot (FP scratch + movsd for FP-class). A spilled
         * GPR128 dst stores BOTH halves to the two slots (Sprint 109 spill fix).
         * OP_OVF_TRAP is a pure check: the preceding op's own store-back
         * already persisted the value, so re-storing here would overwrite
         * the slot with scratch-register garbage on ISAs where the check
         * is skipped. */
        if (spill_dst >= 0 && instr->op != OP_OVF_TRAP) {
            int32_t slot_off = spill_base_offset + (spill_dst + 1) * 8;
            if (d128_sp) {
                if (BE(c)->emit_store) {
                    BE(c)->emit_store(c, g128_dlo, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off));
                    BE(c)->emit_store(c, g128_dhi, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off - 8));
                }
            } else if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_dest) == REG_CLASS_XMM) {
                if (BE(c)->emit_store_fp) {
                    BE(c)->emit_store_fp(c, BE(c)->fp_scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off));
                }
            } else if (BE(c)->emit_store) {
                BE(c)->emit_store(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot_off));
            }
        }
    }

    c->mc_label_state = NULL;

    for (size_t i = 0; i < label_count; i++) {
        free(label_patches ? label_patches[i] : NULL);
    }
    free(label_defined);
    free(label_offsets);
    free(label_patches);
    free(label_patch_counts);
    free(label_patch_caps);
}


