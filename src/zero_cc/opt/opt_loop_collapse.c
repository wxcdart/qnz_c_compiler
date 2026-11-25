/*
 * Loop Collapsing & Outer Loop Auto-Vectorization (Sprint 222)
 *
 * Collapses perfectly nested countable loops into a single flat loop,
 * enabling SIMD auto-vectorization across outer loop boundaries when
 * inner trip counts are small or non-multiple of vector widths.
 */

#include "zero_cc/opt/opt_loop_collapse.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/opt/cfg.h"
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_emitter.h"

typedef struct {
    int header_label;
    int exit_label;
    int start_idx;       /* header OP_LABEL instruction index */
    int test_idx;        /* conditional exit jump instruction index */
    int step_start_idx;  /* start of step sequence */
    int step_idx;        /* induction variable increment instruction index */
    int backedge_idx;    /* unconditional jump back to header */
    int exit_idx;        /* position of exit label in IR */
    int bound_imm_idx;   /* instruction index of MOV_IMM bound */
    int64_t bound_const; /* constant loop bound */
    int counter_slot;    /* stack slot if counter is in slot, else 0 */
    uint32_t var_temp;   /* induction variable virtual register */
} NestedLoopCandidate;

static int find_label_index(ZeroCCCompiler *c, int label_idx) {
    size_t count = ir_get_count(c);
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->op == OP_LABEL && (int)ins->label_idx == label_idx) {
            return (int)i;
        }
    }
    return -1;
}

static bool analyze_candidate_loop(ZeroCCCompiler *c, int start, int end, NestedLoopCandidate *info) {
    if (end - start < 6) return false;

    Instr *hdr = ir_get(c, start);
    if (!hdr || hdr->op != OP_LABEL) return false;

    Instr *back = ir_get(c, end);
    if (!back || back->op != OP_JMP || (int)back->label_idx != (int)hdr->label_idx) return false;

    info->header_label = (int)hdr->label_idx;
    info->start_idx = start;
    info->backedge_idx = end;
    info->counter_slot = 0;
    info->var_temp = TEMP_NONE;

    /* Find exit jump in header region */
    int test_idx = -1;
    int exit_label = -1;
    for (int i = start + 1; i <= start + 10 && i < end; i++) {
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

    int exit_pos = find_label_index(c, exit_label);
    if (exit_pos < end) return false;

    info->test_idx = test_idx;
    info->exit_label = exit_label;
    info->exit_idx = exit_pos;

    /* Find comparison instruction before test_idx */
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
    uint32_t op1 = cmp_ins->temp_src1;
    uint32_t op2 = cmp_ins->temp_src2;

    /* Determine bound temp and counter temp */
    uint32_t bound_temp = TEMP_NONE;
    int bound_imm_idx = -1;
    int64_t bound_val = 0;

    for (int i = cmp_idx - 1; i >= 0 && i >= start - 8; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_MOV_IMM && (ins->temp_dest == op1 || ins->temp_dest == op2)) {
            bound_imm_idx = i;
            bound_temp = ins->temp_dest;
            bound_val = ins->imm64 ? ins->imm64 : ins->imm;
            break;
        }
    }
    if (bound_imm_idx < 0 || bound_val <= 0) return false;

    info->bound_imm_idx = bound_imm_idx;
    info->bound_const = bound_val;

    uint32_t ctr_reg = (bound_temp == op1) ? op2 : op1;

    /* Find counter slot if loaded from memory */
    for (int i = cmp_idx - 1; i >= start; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->temp_dest == ctr_reg && ins->op == OP_MOV && ins->temp_src1 == TEMP_NONE && ins->imm < 0) {
            info->counter_slot = ins->imm;
            break;
        }
        if (ins->temp_dest == ctr_reg && (ins->op == OP_SEXT32 || ins->op == OP_ZEXT32)) {
            ctr_reg = ins->temp_src1;
        }
    }
    info->var_temp = ctr_reg;

    /* Find step increment right before backedge */
    int step_idx = -1;
    for (int i = end - 1; i > test_idx; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP) continue;
        if (ins->op == OP_ADD || ins->op == OP_ADD_IMM) {
            step_idx = i;
            break;
        }
        if (ins->op == OP_STORE_32 || ins->op == OP_VSTORE || ins->op == OP_VSTORESS) {
            break;
        }
    }
    if (step_idx < 0) return false;
    info->step_idx = step_idx;

    /* Find start of step sequence */
    int step_start_idx = step_idx;
    for (int i = step_idx - 1; i > test_idx; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP) continue;
        if (ins->op == OP_LABEL) {
            step_start_idx = i;
            break;
        }
        if (ins->op == OP_MOV || ins->op == OP_MOV_IMM || ins->op == OP_SEXT32 || ins->op == OP_ZEXT32) {
            step_start_idx = i;
        } else {
            break;
        }
    }
    info->step_start_idx = step_start_idx;

    return true;
}

/* Check if instruction is a terminal/jump/call */
static inline bool is_jump_or_call(Opcode op) {
    return (op == OP_LABEL || op == OP_JMP || op == OP_JE || op == OP_JNE ||
            op == OP_JL || op == OP_JLE || op == OP_JG || op == OP_JCOND ||
            op == OP_RET || op == OP_EPILOGUE || op == OP_CALL ||
            op == OP_CALL_INDIRECT || op == OP_SYSCALL || op == OP_CLONE ||
            op == OP_ASM || op == OP_SETJMP || op == OP_LONGJMP);
}

typedef struct {
    int mul_idx;
    int add_idx;
    uint32_t m_const_temp;
} IndexMatch;

static bool is_counter_reg(ZeroCCCompiler *c, int from_idx, int start_idx, uint32_t t,
                            int counter_slot, uint32_t var_temp) {
    if (t == var_temp) return true;
    for (int k = from_idx - 1; k >= start_idx && k >= from_idx - 8; k--) {
        Instr *ins = ir_get(c, k);
        if (!ins) continue;
        if (ins->temp_dest == t) {
            if (ins->op == OP_SEXT32 || ins->op == OP_ZEXT32) {
                return is_counter_reg(c, k, start_idx, ins->temp_src1, counter_slot, var_temp);
            }
            if (ins->op == OP_MOV && ins->temp_src1 == TEMP_NONE && counter_slot != 0 && ins->imm == counter_slot) {
                return true;
            }
            return false;
        }
    }
    return false;
}

static bool try_collapse_pair(ZeroCCCompiler *c, NestedLoopCandidate *out, NestedLoopCandidate *in) {
    /* Verify perfect nesting */
    if (!(out->start_idx < out->test_idx &&
          out->test_idx < in->start_idx &&
          in->start_idx < in->test_idx &&
          in->test_idx < in->step_start_idx &&
          in->step_start_idx <= in->step_idx &&
          in->step_idx < in->backedge_idx &&
          in->backedge_idx < out->step_start_idx &&
          out->step_start_idx <= out->step_idx &&
          out->step_idx < out->backedge_idx)) {
        return false;
    }

    /* Verify instructions between out->test_idx and in->start_idx only init inner counter */
    for (int i = out->test_idx + 1; i < in->start_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
        if (ins->op == OP_MOV_IMM && (ins->imm == 0 && ins->imm64 == 0)) continue;
        if (ins->op == OP_MOV_MEM && (in->counter_slot != 0 && ins->imm == in->counter_slot)) continue;
        if (ins->op == OP_LABEL) continue;
        return false;
    }

    /* Verify instructions between in->backedge_idx and out->step_start_idx only contain exit/continue labels */
    for (int i = in->backedge_idx + 1; i < out->step_start_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
        if (ins->op == OP_LABEL) continue;
        return false;
    }

    /* Verify inner loop body has no branches or calls */
    for (int i = in->test_idx + 1; i < in->step_start_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
        if (is_jump_or_call(ins->op)) return false;
    }

    /* Check product bound fits within signed 32-bit int */
    int64_t total_bound = out->bound_const * in->bound_const;
    if (total_bound <= 0 || total_bound > 0x7FFFFFFF) return false;

    /* Scan inner loop body for 2D index calculations:
     *   t_mul = out_var * M
     *   t_idx = t_mul + in_var  (or in_var + t_mul)
     */
    int match_count = 0;
    IndexMatch matches[16];

    for (int i = in->test_idx + 1; i < in->step_start_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op != OP_ADD) continue;

        uint32_t s1 = ins->temp_src1;
        uint32_t s2 = ins->temp_src2;

        /* Look for preceding MUL that produces s1 or s2 */
        for (int k = i - 1; k >= in->test_idx && k >= i - 16; k--) {
            Instr *d = ir_get(c, k);
            if (!d || d->op != OP_MUL) continue;
            if (d->temp_dest == s1 || d->temp_dest == s2) {
                /* Check if the other operand of ADD is the inner loop counter */
                uint32_t other_add_op = (d->temp_dest == s1) ? s2 : s1;
                if (!is_counter_reg(c, i, in->test_idx, other_add_op, in->counter_slot, in->var_temp)) {
                    continue;
                }

                /* Check if one operand of MUL is constant in->bound_const */
                uint32_t m1 = d->temp_src1;
                uint32_t m2 = d->temp_src2;
                uint32_t const_temp = TEMP_NONE;

                for (int m = k - 1; m >= in->test_idx && m >= k - 8; m--) {
                    Instr *mi = ir_get(c, m);
                    if (mi && mi->op == OP_MOV_IMM && (mi->temp_dest == m1 || mi->temp_dest == m2)) {
                        int64_t mval = mi->imm64 ? mi->imm64 : mi->imm;
                        if (mval == in->bound_const) {
                            const_temp = mi->temp_dest;
                            break;
                        }
                    }
                }

                if (const_temp == TEMP_NONE) continue;

                /* Check if the other operand of MUL is the outer loop counter */
                uint32_t other_mul_op = (m1 == const_temp) ? m2 : m1;
                if (!is_counter_reg(c, k, in->test_idx, other_mul_op, out->counter_slot, out->var_temp)) {
                    continue;
                }

                if (match_count < 16) {
                    matches[match_count].mul_idx = k;
                    matches[match_count].add_idx = i;
                    matches[match_count].m_const_temp = const_temp;
                    match_count++;
                    break;
                }
            }
        }
    }

    if (match_count == 0) return false;

    /* Perform the loop collapse!
     * 1. Update outer loop bound to total_bound (N * M).
     */
    Instr *b_ins = ir_get(c, out->bound_imm_idx);
    b_ins->imm = (int16_t)total_bound;
    b_ins->imm64 = total_bound;

    /* 2. In inner body, replace 2D index calculations with outer induction var */
    for (int a = 0; a < match_count; a++) {
        int m_idx = matches[a].mul_idx;
        int a_idx = matches[a].add_idx;

        Instr *m_ins = ir_get(c, m_idx);
        Instr *a_ins = ir_get(c, a_idx);

        uint32_t i_temp = (m_ins->temp_src1 == matches[a].m_const_temp) ? m_ins->temp_src2 : m_ins->temp_src1;
        m_ins->op = OP_MOV_REG;
        m_ins->temp_src1 = i_temp;
        m_ins->temp_src2 = TEMP_NONE;

        a_ins->op = OP_MOV_REG;
        a_ins->temp_src1 = m_ins->temp_dest;
        a_ins->temp_src2 = TEMP_NONE;
    }

    /* 3. NOP out inner loop initialization */
    for (int i = out->test_idx + 1; i < in->start_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->op != OP_LABEL) ins->op = OP_NOP;
    }

    /* 4. NOP out inner loop header and test */
    for (int i = in->start_idx; i <= in->test_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (ins) ins->op = OP_NOP;
    }

    /* 5. NOP out inner loop step and backedge */
    for (int i = in->step_start_idx; i <= in->backedge_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (ins) ins->op = OP_NOP;
    }

    /* 6. NOP out inner exit label */
    Instr *exit_lbl = ir_get(c, in->exit_idx);
    if (exit_lbl) exit_lbl->op = OP_NOP;

    return true;
}

void opt_loop_collapse(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp) return;
    if (!cfg_build(c)) return;

    ZeroCCCFG *cfg = &c->cfg_cache;
    if (cfg->loop_count < 2) return;

    NestedLoopCandidate candidates[32];
    int cand_count = 0;

    int nloops = cfg->loop_count > 32 ? 32 : cfg->loop_count;
    for (int l = 0; l < nloops; l++) {
        int h = cfg->loops[l].header;
        int u = cfg->loops[l].backedge_from;
        if (h < 0 || h >= cfg->block_count || u < 0 || u >= cfg->block_count) continue;

        int start = (int)cfg->blocks[h].start;
        int end = (int)cfg->blocks[u].end - 1;

        NestedLoopCandidate cand;
        if (analyze_candidate_loop(c, start, end, &cand)) {
            candidates[cand_count++] = cand;
        }
    }

    if (cand_count < 2) return;

    /* Search for nested pairs */
    for (int i = 0; i < cand_count; i++) {
        for (int j = 0; j < cand_count; j++) {
            if (i == j) continue;
            if (candidates[i].start_idx < candidates[j].start_idx &&
                candidates[j].backedge_idx < candidates[i].backedge_idx) {
                if (try_collapse_pair(c, &candidates[i], &candidates[j])) {
                    return;
                }
            }
        }
    }
}
