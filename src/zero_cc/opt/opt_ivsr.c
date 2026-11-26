#include "zero_cc/opt/opt_ivsr.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/opt/opt_loop.h"
#include "zero_cc/opt/cfg.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ast_lowering_internal.h"

/* =========================================================================
 * Sprint 235: Loop Induction Variable Strength Reduction (Tier 4, Item #18)
 *
 * Replaces derived induction expressions for array element addressing:
 *   addr = base + (i * scale)
 * with a dedicated induction pointer:
 *   Preheader:  ptr = base + (i_0 * scale)
 *   Loop Body:  addr = ptr  (or direct ptr access)
 *   Latch:      ptr += (step * scale)
 *
 * Eliminates shifts and multiplications from hot loop bodies.
 * ========================================================================= */

#define MAX_IVSR_CANDS 16

typedef struct {
    int header_idx;
    int latch_idx;
    int loop_start;
    int loop_end;
    uint32_t counter_temp;
    int step_val;
    int add_idx;
    int scale_idx;
    uint32_t addr_temp;
    uint32_t base_temp;
    int scale_bytes;
    uint32_t new_ptr_temp;
} IVSRCand;

/* Find defining instruction of temp searching backward */
static int ivsr_find_def(ZeroCCCompiler *c, int start, int end, uint32_t temp) {
    if (temp == TEMP_NONE) return -1;
    for (int i = end; i >= start; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
        if (ins->temp_dest == temp) return i;
    }
    return -1;
}

/* Check if temp is invariant across a loop span */
static bool ivsr_is_invariant(ZeroCCCompiler *c, int H, int E, uint32_t temp) {
    if (temp == TEMP_NONE) return false;
    for (int i = H; i <= E; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
        if (ins->temp_dest == temp) return false;
    }
    return true;
}

/* Trace a temporary back through SEXT32, ZEXT32, and MOV_REG */
static uint32_t ivsr_trace_origin(ZeroCCCompiler *c, int H, int pos, uint32_t temp) {
    uint32_t curr = temp;
    int limit = 16;
    while (curr != TEMP_NONE && limit-- > 0) {
        int def = ivsr_find_def(c, H, pos - 1, curr);
        if (def < 0) break;
        Instr *ins = ir_get(c, def);
        if (!ins) break;
        if (ins->op == OP_SEXT32 || ins->op == OP_ZEXT32 || ins->op == OP_MOV_REG) {
            curr = ins->temp_src1;
            pos = def;
        } else {
            break;
        }
    }
    return curr;
}

/* Count uses of a temporary within a loop span */
static int ivsr_count_uses(ZeroCCCompiler *c, int H, int E, uint32_t temp) {
    int count = 0;
    for (int i = H; i <= E; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
        if (ins->temp_src1 == temp) count++;
        if (ins->temp_src2 == temp) count++;
    }
    return count;
}

/* Analyze a single natural loop for IVSR candidate addressing expressions */
static int ivsr_analyze_loop(ZeroCCCompiler *c, const LoopInfo *loop,
                             IVSRCand *cands, int max_cands) {
    int H = loop->header_idx;
    int E = loop->exit_idx;
    int count = (int)ir_get_count(c);
    if (H < 0 || E < 0 || E >= count || E <= H) return 0;

    /* Purity check: reject loops with calls or barriers */
    for (int i = H; i <= E; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_CALL || ins->op == OP_CALL_INDIRECT ||
            ins->op == OP_SYSCALL || ins->op == OP_CLONE ||
            ins->op == OP_ASM || ins->op == OP_SETJMP) {
            return 0;
        }
    }

    /* Step 1: Identify primary induction variable and latch */
    int latch_idx = E;
    uint32_t counter_temp = TEMP_NONE;
    int step_val = 1;

    /* Look at backedge instruction (usually OP_JMP header) */
    Instr *jmp_ins = ir_get(c, E);
    if (!jmp_ins || jmp_ins->op != OP_JMP) return 0;

    /* Look backward from latch for counter update */
    for (int i = E - 1; i > H; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
        if (ins->op == OP_ADD && ins->temp_dest != TEMP_NONE) {
            uint32_t c_cand = ins->temp_dest;
            if (ins->temp_src1 == c_cand || ins->temp_src2 == c_cand) {
                uint32_t step_op = (ins->temp_src1 == c_cand) ? ins->temp_src2 : ins->temp_src1;
                int step_def = ivsr_find_def(c, 0, i - 1, step_op);
                if (step_def >= 0) {
                    Instr *si = ir_get(c, step_def);
                    if (si && si->op == OP_MOV_IMM && si->imm64 > 0 && si->imm64 <= 64) {
                        counter_temp = c_cand;
                        step_val = (int)si->imm64;
                        break;
                    }
                }
            }
        } else if (ins->op == OP_INC && ins->temp_dest != TEMP_NONE) {
            counter_temp = ins->temp_dest;
            step_val = 1;
            break;
        }
    }

    if (counter_temp == TEMP_NONE) return 0;

    /* Verify counter is tested in loop header */
    bool counter_tested = false;
    for (int i = H + 1; i <= E; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_CMP) {
            uint32_t o1 = ivsr_trace_origin(c, H, i, ins->temp_src1);
            uint32_t o2 = ivsr_trace_origin(c, H, i, ins->temp_src2);
            if (o1 == counter_temp || o2 == counter_temp) {
                counter_tested = true;
                break;
            }
        }
        if (ins->op == OP_JE || ins->op == OP_JNE) break;
    }
    if (!counter_tested) return 0;

    /* Step 2: Scan loop body for address computations: addr = base + scaled */
    int cand_count = 0;
    for (int i = H + 1; i < E; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op != OP_ADD) continue;
        if (ins->temp_dest == TEMP_NONE || ins->temp_dest == counter_temp) continue;

        uint32_t op1 = ins->temp_src1;
        uint32_t op2 = ins->temp_src2;
        if (op1 == TEMP_NONE || op2 == TEMP_NONE) continue;

        for (int pass = 0; pass < 2; pass++) {
            uint32_t cand_base = (pass == 0) ? op1 : op2;
            uint32_t cand_scaled = (pass == 0) ? op2 : op1;

            /* Check if base is loop-invariant */
            if (!ivsr_is_invariant(c, H, E, cand_base)) continue;

            /* Check if cand_scaled is computed from counter_temp */
            int scale_def = ivsr_find_def(c, H, i - 1, cand_scaled);
            if (scale_def < 0) continue;
            Instr *si = ir_get(c, scale_def);
            if (!si) continue;

            int scale_bytes = 0;
            if (si->op == OP_SHL) {
                int sh_def = ivsr_find_def(c, 0, scale_def - 1, si->temp_src2);
                int shift = -1;
                if (sh_def >= 0) {
                    Instr *shi = ir_get(c, sh_def);
                    if (shi && shi->op == OP_MOV_IMM) shift = (int)shi->imm64;
                }
                if (shift == 1) scale_bytes = 2;
                else if (shift == 2) scale_bytes = 4;
                else if (shift == 3) scale_bytes = 8;
                else continue;
            } else if (si->op == OP_MUL) {
                int mul_def = ivsr_find_def(c, 0, scale_def - 1, si->temp_src2);
                if (mul_def >= 0) {
                    Instr *mi = ir_get(c, mul_def);
                    if (mi && mi->op == OP_MOV_IMM &&
                        (mi->imm64 == 2 || mi->imm64 == 4 || mi->imm64 == 8)) {
                        scale_bytes = (int)mi->imm64;
                    }
                }
                if (scale_bytes == 0) {
                    mul_def = ivsr_find_def(c, 0, scale_def - 1, si->temp_src1);
                    if (mul_def >= 0) {
                        Instr *mi = ir_get(c, mul_def);
                        if (mi && mi->op == OP_MOV_IMM &&
                            (mi->imm64 == 2 || mi->imm64 == 4 || mi->imm64 == 8)) {
                            scale_bytes = (int)mi->imm64;
                        }
                    }
                }
                if (scale_bytes == 0) continue;
            } else {
                continue;
            }

            uint32_t scale_src = (si->op == OP_MUL && scale_bytes != 0) ?
                ((si->temp_src1 == cand_scaled) ? si->temp_src2 : si->temp_src1) : si->temp_src1;
            uint32_t origin = ivsr_trace_origin(c, H, scale_def, scale_src);
            if (origin != counter_temp) continue;

            /* Verify cand_scaled is only used here */
            if (ivsr_count_uses(c, H, E, cand_scaled) > 1) continue;

            /* Valid candidate! */
            if (cand_count < max_cands) {
                IVSRCand *cand = &cands[cand_count++];
                cand->header_idx = H;
                cand->latch_idx = latch_idx;
                cand->loop_start = H;
                cand->loop_end = E;
                cand->counter_temp = counter_temp;
                cand->step_val = step_val;
                cand->add_idx = i;
                cand->scale_idx = scale_def;
                cand->addr_temp = ins->temp_dest;
                cand->base_temp = cand_base;
                cand->scale_bytes = scale_bytes;
                cand->new_ptr_temp = TEMP_NONE;
            }
            break;
        }
    }

    return cand_count;
}

/* Apply IVSR candidates using Two-Pass Arena Rebuild */
static bool ivsr_apply_cands(ZeroCCCompiler *c, IVSRCand *cands, int cand_count) {
    if (cand_count == 0) return false;

    /* Allocate pointer temps for each candidate */
    for (int k = 0; k < cand_count; k++) {
        cands[k].new_ptr_temp = (uint32_t)zc_alloc_temp(c);
        zc_temp_tag_set(c, cands[k].new_ptr_temp, ZC_TAG_PTR);
    }

    int count = (int)ir_get_count(c);
    IRArena new_arena;
    if (!ir_arena_init(&new_arena, count + cand_count * 8 + 32)) return false;

    for (int i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;

        /* Preheader: emit initial pointer assignments immediately before header */
        for (int k = 0; k < cand_count; k++) {
            if (i == cands[k].header_idx) {
                Instr init_ptr = {
                    .op = OP_MOV_REG,
                    .temp_dest = cands[k].new_ptr_temp,
                    .temp_src1 = cands[k].base_temp,
                    .temp_src2 = TEMP_NONE,
                    .cond = 0,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF,
                    .imm = 0,
                    .imm64 = 0,
                    .label_idx = 0
                };
                ir_arena_emit(&new_arena, init_ptr);
            }
        }

        /* Check if instruction is candidate scale or add */
        bool handled = false;
        for (int k = 0; k < cand_count; k++) {
            if (i == cands[k].scale_idx) {
                /* NOP out the now-dead scale instruction */
                handled = true;
                break;
            }
            if (i == cands[k].add_idx) {
                /* Replace OP_ADD addr, base, scaled with OP_MOV_REG addr, new_ptr */
                Instr mov_ptr = {
                    .op = OP_MOV_REG,
                    .temp_dest = cands[k].addr_temp,
                    .temp_src1 = cands[k].new_ptr_temp,
                    .temp_src2 = TEMP_NONE,
                    .cond = 0,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF,
                    .imm = 0,
                    .imm64 = 0,
                    .label_idx = 0
                };
                ir_arena_emit(&new_arena, mov_ptr);
                handled = true;
                break;
            }
        }

        if (handled) continue;

        /* Latch: emit pointer step immediately before backedge jump */
        for (int k = 0; k < cand_count; k++) {
            if (i == cands[k].latch_idx) {
                int delta = cands[k].step_val * cands[k].scale_bytes;
                int delta_temp = zc_alloc_temp(c);
                zc_temp_tag_set(c, delta_temp, ZC_TAG_INT(ZC_TAGW_8));
                Instr mov_delta = {
                    .op = OP_MOV_IMM,
                    .temp_dest = (uint32_t)delta_temp,
                    .temp_src1 = TEMP_NONE,
                    .temp_src2 = TEMP_NONE,
                    .cond = 0,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF,
                    .imm = (int16_t)delta,
                    .imm64 = delta,
                    .label_idx = 0
                };
                ir_arena_emit(&new_arena, mov_delta);

                Instr step_ptr = {
                    .op = OP_ADD,
                    .temp_dest = cands[k].new_ptr_temp,
                    .temp_src1 = cands[k].new_ptr_temp,
                    .temp_src2 = (uint32_t)delta_temp,
                    .cond = 0,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF,
                    .imm = 0,
                    .imm64 = 0,
                    .label_idx = 0
                };
                ir_arena_emit(&new_arena, step_ptr);
            }
        }

        /* Emit original instruction */
        ir_arena_emit(&new_arena, *ins);
    }

    ir_arena_free(&c->ir_arena);
    c->ir_arena = new_arena;
    c->cfg_version++;
    return true;
}

bool opt_ivsr(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp) return false;

    LoopAnalysisState state;
    loop_analysis_init(&state);
    loop_detect(c, &state);

    if (state.loop_count == 0) {
        loop_analysis_free(&state);
        return false;
    }

    IVSRCand all_cands[MAX_IVSR_CANDS];
    int total_cands = 0;

    for (int l = 0; l < state.loop_count; l++) {
        IVSRCand loop_cands[MAX_IVSR_CANDS];
        int num = ivsr_analyze_loop(c, &state.loops[l], loop_cands,
                                    MAX_IVSR_CANDS - total_cands);
        for (int k = 0; k < num; k++) {
            all_cands[total_cands++] = loop_cands[k];
        }
        if (total_cands >= MAX_IVSR_CANDS) break;
    }

    loop_analysis_free(&state);

    if (total_cands == 0) return false;
    return ivsr_apply_cands(c, all_cands, total_cands);
}
