/*
 * opt_loop_unswitch.c - Loop Unswitching and Loop Peeling Optimization Passes (Sprint 216)
 *
 * Loop Unswitching:
 * Hoists loop-invariant conditional branches outside of loop bodies by cloning
 * the loop into two specialized loops (one for the true branch, one for the false branch),
 * eliminating branch evaluations on every iteration and exposing straight-line loop bodies
 * for downstream vectorization and unrolling.
 *
 * Loop Peeling:
 * Peels the first iteration of a countable loop into the preheader, resolving initial
 * boundary conditions (e.g. i == 0 checks) and eliminating initial-iteration branches.
 */

#include "zero_cc/opt/opt_loop_unswitch.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/opt/cfg.h"
#include "zero_cc/opt/opt_loop.h"
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/types/symbols.h"

#define UNSWITCH_MARKER 0x5EE7
#define PEEL_MARKER     0x5EE8

#define MAX_UNSWITCH_BODY 128
#define MAX_PEEL_BODY     64

/* Check if instruction is a branch or terminal */
static inline bool is_terminal_or_branch(uint16_t op) {
    return (op == OP_LABEL || op == OP_JMP || op == OP_JE || op == OP_JNE ||
            op == OP_JL || op == OP_JLE || op == OP_JG || op == OP_JCOND ||
            op == OP_RET || op == OP_EPILOGUE || op == OP_CALL ||
            op == OP_CALL_INDIRECT || op == OP_SYSCALL || op == OP_CLONE ||
            op == OP_ASM || op == OP_SETJMP || op == OP_LONGJMP);
}

/* Check if instruction has moving side effects */
static inline bool has_side_effects(uint16_t op) {
    return (op == OP_CALL || op == OP_CALL_INDIRECT || op == OP_SYSCALL ||
            op == OP_CLONE || op == OP_ASM || op == OP_SETJMP ||
            op == OP_LONGJMP || op == OP_NONLOCAL_GOTO || op == OP_RET ||
            op == OP_EPILOGUE);
}

/* Find instruction position of a label index */
static int find_label_pos(ZeroCCCompiler *c, int label_idx) {
    size_t count = ir_get_count(c);
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->op == OP_LABEL && (int)ins->label_idx == label_idx) {
            return (int)i;
        }
    }
    return -1;
}

/* Check if a virtual register temporary is loop-invariant in [start .. end] */
static bool is_temp_loop_invariant(ZeroCCCompiler *c, uint32_t temp, int start, int end) {
    if (temp == TEMP_NONE || temp == 0) return true;
    for (int i = start; i <= end; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->temp_dest == temp) {
            return false;
        }
    }
    return true;
}

/* ========================================================================= */
/* Loop Unswitching Implementation                                           */
/* ========================================================================= */

typedef struct {
    int header_idx;       /* Loop header index (OP_LABEL) */
    int backedge_idx;     /* Backedge jump index (OP_JMP to header) */
    int test_idx;         /* Loop condition test exit jump */
    int exit_label;       /* Label to exit the loop */
    int cmp_idx;          /* Invariant comparison instruction index */
    int br_idx;           /* Invariant conditional branch instruction index */
    int taken_label;      /* Target label when branch is taken */
} UnswitchPlan;

/* Scan for an unswitchable loop candidate */
static bool find_unswitch_candidate(ZeroCCCompiler *c, int start, int end, UnswitchPlan *plan) {
    if (end - start < 4 || end - start > MAX_UNSWITCH_BODY) return false;

    Instr *hdr = ir_get(c, start);
    if (!hdr || hdr->op != OP_LABEL) return false;

    Instr *back = ir_get(c, end);
    if (!back || back->op != OP_JMP || (int)back->label_idx != (int)hdr->label_idx) return false;

    /* Purity and marker check */
    for (int i = start; i <= end; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_NOP && (ins->imm == UNSWITCH_MARKER || ins->imm == PEEL_MARKER)) {
            return false; /* Already unswitched or peeled */
        }
        if (has_side_effects(ins->op)) {
            return false;
        }
    }

    /* Find loop exit test jump near header */
    int test_idx = -1;
    int exit_label = -1;
    for (int i = start + 1; i <= start + 8 && i < end; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_JE || ins->op == OP_JNE || ins->op == OP_JL ||
            ins->op == OP_JLE || ins->op == OP_JG || ins->op == OP_JCOND) {
            test_idx = i;
            exit_label = (int)ins->label_idx;
            break;
        }
    }
    if (test_idx < 0 || exit_label < 0) return false;

    int exit_pos = find_label_pos(c, exit_label);
    if (exit_pos >= start && exit_pos <= end) return false; /* exit must be outside */

    /* Find invariant branch in loop body */
    int best_cmp = -1;
    int best_br = -1;
    int best_taken = -1;

    for (int i = test_idx + 1; i < end - 1; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;

        if (ins->op == OP_JE || ins->op == OP_JNE || ins->op == OP_JL ||
            ins->op == OP_JLE || ins->op == OP_JG || ins->op == OP_JCOND) {
            int taken = (int)ins->label_idx;
            int taken_pos = find_label_pos(c, taken);
            if (taken_pos < start || taken_pos > end) {
                continue; /* Must branch within the loop */
            }

            /* Check preceding comparison */
            int cmp_i = -1;
            for (int k = i - 1; k >= test_idx && k >= i - 4; k--) {
                Instr *prev = ir_get(c, k);
                if (!prev) continue;
                if (prev->op == OP_CMP || prev->op == OP_CMP_ZERO) {
                    cmp_i = k;
                    break;
                }
            }
            if (cmp_i < 0) continue;

            Instr *cmp_ins = ir_get(c, cmp_i);
            if (!is_temp_loop_invariant(c, cmp_ins->temp_src1, start, end)) continue;
            if (!is_temp_loop_invariant(c, cmp_ins->temp_src2, start, end)) continue;

            best_cmp = cmp_i;
            best_br = i;
            best_taken = taken;
            break;
        }
    }

    if (best_cmp < 0 || best_br < 0) return false;

    plan->header_idx = start;
    plan->backedge_idx = end;
    plan->test_idx = test_idx;
    plan->exit_label = exit_label;
    plan->cmp_idx = best_cmp;
    plan->br_idx = best_br;
    plan->taken_label = best_taken;
    return true;
}

/* Execute loop unswitching transformation */
static bool unswitch_loop(ZeroCCCompiler *c, const UnswitchPlan *plan) {
    size_t count = ir_get_count(c);
    int H = plan->header_idx;
    int E = plan->backedge_idx;
    int loop_len = E - H + 1;

    /* Collect internal labels that need remapping for True loop */
    uint16_t remap_label[65536];
    memset(remap_label, 0, sizeof(remap_label));

    int L_true_head = (int)c->ir_next_label++;
    Instr *hdr = ir_get(c, H);
    remap_label[hdr->label_idx] = (uint16_t)L_true_head;

    for (int i = H; i <= E; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->op == OP_LABEL) {
            if (ins->label_idx != hdr->label_idx) {
                remap_label[ins->label_idx] = (uint16_t)c->ir_next_label++;
            }
        }
    }

    /* Allocate new arena: prefix + preheader test + Loop False + Loop True + suffix */
    IRArena new_arena;
    if (!ir_arena_init(&new_arena, count + loop_len + 32)) {
        return false;
    }

    /* 1. Emit Prefix [0 .. H - 1] */
    for (int i = 0; i < H; i++) {
        ir_arena_emit(&new_arena, *ir_get(c, i));
    }

    /* 2. Emit Preheader Condition Test:
     *    If condition holds, jump to L_true_head;
     *    Else fallthrough into False loop. */
    Instr cmp_copy = *ir_get(c, plan->cmp_idx);
    ir_arena_emit(&new_arena, cmp_copy);

    Instr br_copy = *ir_get(c, plan->br_idx);
    br_copy.label_idx = (uint16_t)L_true_head;
    ir_arena_emit(&new_arena, br_copy);

    /* 3. Emit Loop False (condition is FALSE, branch NOT taken) */
    for (int i = H; i <= E; i++) {
        Instr ins = *ir_get(c, i);
        if (i == H) {
            ir_arena_emit(&new_arena, ins);
            Instr marker = {
                .op = OP_NOP, .temp_dest = TEMP_NONE, .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF,
                .flags_in = 0xFF, .imm = UNSWITCH_MARKER, .imm64 = UNSWITCH_MARKER,
                .label_idx = 0
            };
            ir_arena_emit(&new_arena, marker);
            continue;
        }

        if (i == plan->br_idx) {
            /* Branch is not taken: replace with NOP to fallthrough */
            Instr nop = {
                .op = OP_NOP, .temp_dest = TEMP_NONE, .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF,
                .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0
            };
            ir_arena_emit(&new_arena, nop);
            continue;
        }

        ir_arena_emit(&new_arena, ins);
    }

    /* 4. Emit Loop True (condition is TRUE, branch is TAKEN) */
    for (int i = H; i <= E; i++) {
        Instr ins = *ir_get(c, i);

        if (i == H) {
            Instr true_hdr = {
                .op = OP_LABEL, .temp_dest = TEMP_NONE, .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF,
                .flags_in = 0xFF, .imm = 0, .imm64 = 0,
                .label_idx = (uint16_t)L_true_head
            };
            ir_arena_emit(&new_arena, true_hdr);
            Instr marker = {
                .op = OP_NOP, .temp_dest = TEMP_NONE, .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF,
                .flags_in = 0xFF, .imm = UNSWITCH_MARKER, .imm64 = UNSWITCH_MARKER,
                .label_idx = 0
            };
            ir_arena_emit(&new_arena, marker);
            continue;
        }

        if (ins.op == OP_LABEL) {
            ins.label_idx = remap_label[ins.label_idx];
            ir_arena_emit(&new_arena, ins);
            continue;
        }

        if (i == plan->br_idx) {
            /* Branch is always taken: replace with unconditional JMP to remapped target */
            Instr jmp_taken = {
                .op = OP_JMP, .temp_dest = TEMP_NONE, .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF,
                .flags_in = 0xFF, .imm = 0, .imm64 = 0,
                .label_idx = remap_label[plan->taken_label]
            };
            ir_arena_emit(&new_arena, jmp_taken);
            continue;
        }

        /* Remap internal jumps */
        if (ins.op == OP_JMP || ins.op == OP_JE || ins.op == OP_JNE ||
            ins.op == OP_JL || ins.op == OP_JLE || ins.op == OP_JG || ins.op == OP_JCOND) {
            if (remap_label[ins.label_idx] != 0) {
                ins.label_idx = remap_label[ins.label_idx];
            }
        }

        ir_arena_emit(&new_arena, ins);
    }

    /* 5. Emit Suffix [E + 1 .. count - 1] */
    for (size_t i = (size_t)(E + 1); i < count; i++) {
        ir_arena_emit(&new_arena, *ir_get(c, i));
    }

    ir_arena_free(&c->ir_arena);
    c->ir_arena = new_arena;
    c->cfg_version++;
    return true;
}

/* Public entry point: Loop Unswitching Optimization Pass */
void opt_loop_unswitch(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp) return;
    if (!cfg_build(c)) return;

    ZeroCCCFG *cfg = &c->cfg_cache;
    if (cfg->loop_count == 0) return;

    for (int l = 0; l < cfg->loop_count; l++) {
        int h = cfg->loops[l].header;
        int u = cfg->loops[l].backedge_from;
        if (h < 0 || h >= cfg->block_count || u < 0 || u >= cfg->block_count) continue;

        int start = (int)cfg->blocks[h].start;
        int end = (int)cfg->blocks[u].end - 1;

        UnswitchPlan plan;
        if (find_unswitch_candidate(c, start, end, &plan)) {
            if (unswitch_loop(c, &plan)) {
                break; /* Unswitch one loop per pass iteration */
            }
        }
    }
}

/* ========================================================================= */
/* Loop Peeling Implementation                                               */
/* ========================================================================= */

typedef struct {
    int header_idx;
    int backedge_idx;
    int test_idx;
    int exit_label;
    int step_idx;
    uint32_t var_temp;
    uint32_t bound_temp;
    int64_t init_val;
    bool has_const_init;
} PeelPlan;

/* Scan for a peelable loop candidate */
static bool find_peel_candidate(ZeroCCCompiler *c, int start, int end, PeelPlan *plan) {
    if (end - start < 4 || end - start > MAX_PEEL_BODY) return false;

    Instr *hdr = ir_get(c, start);
    if (!hdr || hdr->op != OP_LABEL) return false;

    Instr *back = ir_get(c, end);
    if (!back || back->op != OP_JMP || (int)back->label_idx != (int)hdr->label_idx) return false;

    /* Check marker & purity */
    for (int i = start; i <= end; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_NOP && (ins->imm == PEEL_MARKER || ins->imm == UNSWITCH_MARKER)) {
            return false;
        }
        if (has_side_effects(ins->op)) {
            return false;
        }
    }

    /* Find exit test */
    int test_idx = -1;
    int exit_label = -1;
    for (int i = start + 1; i <= start + 8 && i < end; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_JE || ins->op == OP_JNE || ins->op == OP_JL ||
            ins->op == OP_JLE || ins->op == OP_JG || ins->op == OP_JCOND) {
            test_idx = i;
            exit_label = (int)ins->label_idx;
            break;
        }
    }
    if (test_idx < 0 || exit_label < 0) return false;

    int exit_pos = find_label_pos(c, exit_label);
    if (exit_pos >= start && exit_pos <= end) return false;

    /* Find induction step before backedge */
    int step_idx = -1;
    uint32_t var_temp = TEMP_NONE;
    for (int i = end - 1; i > test_idx; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP) continue;
        if ((ins->op == OP_ADD || ins->op == OP_ADD_IMM) && ins->temp_dest != TEMP_NONE) {
            step_idx = i;
            var_temp = ins->temp_dest;
            break;
        }
    }
    if (step_idx < 0 || var_temp == TEMP_NONE) return false;

    /* Find comparison bound */
    int cmp_idx = -1;
    for (int i = test_idx - 1; i >= start; i--) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->op == OP_CMP) {
            cmp_idx = i;
            break;
        }
    }
    if (cmp_idx < 0) return false;

    Instr *cmp_ins = ir_get(c, cmp_idx);
    uint32_t bound_temp = (cmp_ins->temp_src1 == var_temp) ? cmp_ins->temp_src2 : cmp_ins->temp_src1;

    /* Find initial value of induction variable before loop */
    int64_t init_val = 0;
    bool has_const_init = false;
    for (int i = start - 1; i >= 0 && i >= start - 16; i--) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->temp_dest == var_temp && ins->op == OP_MOV_IMM) {
            init_val = ins->imm64 ? ins->imm64 : ins->imm;
            has_const_init = true;
            break;
        }
    }
    if (!has_const_init) return false;

    /* Check if loop body has conditions testing var_temp against init_val (e.g. i == 0) */
    bool beneficial = false;
    for (int i = test_idx + 1; i < step_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_CMP) {
            if ((ins->temp_src1 == var_temp) || (ins->temp_src2 == var_temp)) {
                beneficial = true;
                break;
            }
        }
    }
    if (!beneficial) return false;

    plan->header_idx = start;
    plan->backedge_idx = end;
    plan->test_idx = test_idx;
    plan->exit_label = exit_label;
    plan->step_idx = step_idx;
    plan->var_temp = var_temp;
    plan->bound_temp = bound_temp;
    plan->init_val = init_val;
    plan->has_const_init = has_const_init;
    return true;
}

/* Execute loop peeling transformation */
static bool peel_loop(ZeroCCCompiler *c, const PeelPlan *plan) {
    size_t count = ir_get_count(c);
    int H = plan->header_idx;
    int E = plan->backedge_idx;
    int body_len = plan->step_idx - (plan->test_idx + 1);
    if (body_len <= 0) return false;

    /* Collect labels in peeled body to remap */
    uint16_t remap_label[65536];
    memset(remap_label, 0, sizeof(remap_label));

    for (int i = plan->test_idx + 1; i < plan->step_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->op == OP_LABEL) {
            remap_label[ins->label_idx] = (uint16_t)c->ir_next_label++;
        }
    }

    IRArena new_arena;
    if (!ir_arena_init(&new_arena, count + body_len + 32)) {
        return false;
    }

    /* 1. Prefix [0 .. H - 1] */
    for (int i = 0; i < H; i++) {
        ir_arena_emit(&new_arena, *ir_get(c, i));
    }

    /* 2. Guard peeled iteration against 0-trip:
     *    Compare bound against init_val; if not continuing, jump to exit. */
    int t_init = zc_alloc_temp(c);
    Instr mov_init = {
        .op = OP_MOV_IMM, .temp_dest = (uint32_t)t_init, .temp_src1 = TEMP_NONE,
        .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF,
        .imm = (int16_t)plan->init_val, .imm64 = plan->init_val, .label_idx = 0
    };
    ir_arena_emit(&new_arena, mov_init);

    Instr cmp_guard = {
        .op = OP_CMP, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)t_init,
        .temp_src2 = plan->bound_temp, .cond = 0, .flags_out = REG_FLAGS,
        .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0
    };
    ir_arena_emit(&new_arena, cmp_guard);

    Instr br_guard = *ir_get(c, plan->test_idx);
    ir_arena_emit(&new_arena, br_guard);

    /* 3. Emit peeled iteration body with var_temp holding init_val */
    for (int i = plan->test_idx + 1; i < plan->step_idx; i++) {
        Instr ins = *ir_get(c, i);
        if (ins.op == OP_LABEL) {
            ins.label_idx = remap_label[ins.label_idx];
        } else if (ins.op == OP_JMP || ins.op == OP_JE || ins.op == OP_JNE ||
                   ins.op == OP_JL || ins.op == OP_JLE || ins.op == OP_JG || ins.op == OP_JCOND) {
            if (remap_label[ins.label_idx] != 0) {
                ins.label_idx = remap_label[ins.label_idx];
            }
        }
        ir_arena_emit(&new_arena, ins);
    }

    /* 4. Update var_temp for remaining loop to init_val + 1 */
    Instr mov_next = {
        .op = OP_MOV_IMM, .temp_dest = plan->var_temp, .temp_src1 = TEMP_NONE,
        .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF,
        .imm = (int16_t)(plan->init_val + 1), .imm64 = plan->init_val + 1,
        .label_idx = 0
    };
    ir_arena_emit(&new_arena, mov_next);

    /* 5. Emit remainder loop with PEEL_MARKER */
    for (int i = H; i <= E; i++) {
        Instr ins = *ir_get(c, i);
        ir_arena_emit(&new_arena, ins);

        if (i == H) {
            Instr marker = {
                .op = OP_NOP, .temp_dest = TEMP_NONE, .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF,
                .flags_in = 0xFF, .imm = PEEL_MARKER, .imm64 = PEEL_MARKER,
                .label_idx = 0
            };
            ir_arena_emit(&new_arena, marker);
        }
    }

    /* 6. Suffix [E + 1 .. count - 1] */
    for (size_t i = (size_t)(E + 1); i < count; i++) {
        ir_arena_emit(&new_arena, *ir_get(c, i));
    }

    ir_arena_free(&c->ir_arena);
    c->ir_arena = new_arena;
    c->cfg_version++;
    return true;
}

/* Public entry point: Loop Peeling Optimization Pass */
void opt_loop_peel(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp) return;
    if (!cfg_build(c)) return;

    ZeroCCCFG *cfg = &c->cfg_cache;
    if (cfg->loop_count == 0) return;

    for (int l = 0; l < cfg->loop_count; l++) {
        int h = cfg->loops[l].header;
        int u = cfg->loops[l].backedge_from;
        if (h < 0 || h >= cfg->block_count || u < 0 || u >= cfg->block_count) continue;

        int start = (int)cfg->blocks[h].start;
        int end = (int)cfg->blocks[u].end - 1;

        PeelPlan plan;
        if (find_peel_candidate(c, start, end, &plan)) {
            if (peel_loop(c, &plan)) {
                break;
            }
        }
    }
}
