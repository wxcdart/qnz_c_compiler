#include "zero_cc/opt/opt_sccp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/opt/cfg.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/types/vregs.h"

/* =========================================================================
 * Sprint 237: Sparse Conditional Constant Propagation (SCCP - Tier 2, Item #8)
 *
 * Implements the classic Wegman-Zadeck algorithm with a 3-level lattice:
 *   - LATTICE_TOP   : Undefined / not yet evaluated (optimistically constant)
 *   - LATTICE_CONST : Known compile-time constant
 *   - LATTICE_BOT   : Dynamic / overdefined non-constant
 *
 * Simultaneously discovers constants across basic blocks, evaluates conditional
 * branches, folds constant paths, and prunes dead control flow edges.
 * ========================================================================= */

typedef enum {
    LATTICE_TOP = 0,
    LATTICE_CONST = 1,
    LATTICE_BOT = 2
} LatticeStatus;

typedef struct {
    LatticeStatus status;
    int64_t val;
} LatticeValue;

/* Evaluate binary operation with constant operands */
static bool sccp_eval_binop(int op, int64_t left, int64_t right, int64_t *out_val) {
    uint64_t ur = (uint64_t)right & 63;
    switch (op) {
        case OP_ADD: *out_val = left + right; return true;
        case OP_SUB: *out_val = left - right; return true;
        case OP_MUL: *out_val = left * right; return true;
        case OP_DIV:
            if (right == 0) return false;
            *out_val = left / right;
            return true;
        case OP_MOD:
            if (right == 0) return false;
            *out_val = left % right;
            return true;
        case OP_AND: *out_val = left & right; return true;
        case OP_OR:  *out_val = left | right; return true;
        case OP_XOR: *out_val = left ^ right; return true;
        case OP_SHL: *out_val = (int64_t)((uint64_t)left << ur); return true;
        case OP_SHR: *out_val = (int64_t)((uint64_t)left >> ur); return true;
        case OP_SAR: *out_val = left >> ur; return true;
        default: return false;
    }
}

/* Evaluate unary operation with constant operand */
static bool sccp_eval_unop(int op, int64_t val, int64_t *out_val) {
    switch (op) {
        case OP_NEG: *out_val = -val; return true;
        case OP_NOT: *out_val = ~val; return true;
        case OP_LNOT: *out_val = (val == 0) ? 1 : 0; return true;
        case OP_SEXT8:  *out_val = (int64_t)(int8_t)val; return true;
        case OP_SEXT16: *out_val = (int64_t)(int16_t)val; return true;
        case OP_SEXT32: *out_val = (int64_t)(int32_t)val; return true;
        case OP_ZEXT8:  *out_val = (int64_t)(uint8_t)val; return true;
        case OP_ZEXT16: *out_val = (int64_t)(uint16_t)val; return true;
        case OP_ZEXT32: *out_val = (int64_t)(uint32_t)val; return true;
        default: return false;
    }
}

/* Meet operator: update lattice value, returns true if value transitioned */
static bool sccp_meet(LatticeValue *target, LatticeStatus new_status, int64_t new_val) {
    if (target->status == LATTICE_BOT) {
        return false; /* Bottom is an absorbing state */
    }
    if (new_status == LATTICE_BOT) {
        target->status = LATTICE_BOT;
        return true;
    }
    if (new_status == LATTICE_CONST) {
        if (target->status == LATTICE_TOP) {
            target->status = LATTICE_CONST;
            target->val = new_val;
            return true;
        }
        if (target->status == LATTICE_CONST) {
            if (target->val != new_val) {
                target->status = LATTICE_BOT;
                return true;
            }
            return false;
        }
    }
    return false;
}

/* Find the block index targeting a given label index */
static int sccp_find_label_block(const ZeroCCCFG *cfg, ZeroCCCompiler *c, uint32_t label_idx) {
    for (int b = 0; b < cfg->block_count; b++) {
        Instr *first = ir_get(c, cfg->blocks[b].start);
        if (first && first->op == OP_LABEL && first->label_idx == label_idx) {
            return b;
        }
    }
    return -1;
}

bool opt_sccp(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp) return false;
    if (!cfg_build(c)) return false;

    ZeroCCCFG *cfg = &c->cfg_cache;
    int num_blocks = cfg->block_count;
    if (num_blocks == 0) return false;

    size_t ir_count = ir_get_count(c);
    bool has_indirect = false;
    for (size_t i = 0; i < ir_count; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->op == OP_JMP_INDIRECT) {
            has_indirect = true;
            break;
        }
    }

    size_t temp_count = (size_t)c->next_temp_id + 64;
    LatticeValue *lat = (LatticeValue *)calloc(temp_count, sizeof(LatticeValue));
    if (!lat) return false;

    /* Initialize all user virtual temporaries to TOP */
    for (size_t t = 0; t < temp_count; t++) {
        lat[t].status = LATTICE_TOP;
        lat[t].val = 0;
    }

    bool *block_executable = (bool *)calloc(num_blocks, sizeof(bool));
    if (!block_executable) {
        free(lat);
        return false;
    }

    /* Entry block 0 is always executable */
    block_executable[0] = true;

    /* If the function contains computed gotos, all labeled blocks are potentially reachable */
    if (has_indirect) {
        for (int b = 0; b < num_blocks; b++) {
            Instr *first = ir_get(c, cfg->blocks[b].start);
            if (first && first->op == OP_LABEL) {
                block_executable[b] = true;
            }
        }
    }

    /* Fixed-point iteration over executable CFG blocks */
    bool any_changed = true;
    int iteration = 0;

    while (any_changed && iteration++ < 32) {
        any_changed = false;

        for (int b = 0; b < num_blocks; b++) {
            if (!block_executable[b]) continue;
            const ZeroCCBlock *blk = &cfg->blocks[b];

            for (size_t i = blk->start; i < blk->end; i++) {
                Instr *ins = ir_get(c, i);
                if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;

                /* Constant definition */
                if (ins->op == OP_MOV_IMM && zc_temp_is_user(ins->temp_dest) &&
                    ins->temp_dest < temp_count) {
                    if (sccp_meet(&lat[ins->temp_dest], LATTICE_CONST, ins->imm64)) {
                        any_changed = true;
                    }
                    continue;
                }

                /* Register copy */
                if (ins->op == OP_MOV_REG && zc_temp_is_user(ins->temp_dest) &&
                    ins->temp_dest < temp_count) {
                    if (zc_temp_is_user(ins->temp_src1) && ins->temp_src1 < temp_count) {
                        LatticeValue src_lat = lat[ins->temp_src1];
                        if (sccp_meet(&lat[ins->temp_dest], src_lat.status, src_lat.val)) {
                            any_changed = true;
                        }
                    } else {
                        if (sccp_meet(&lat[ins->temp_dest], LATTICE_BOT, 0)) {
                            any_changed = true;
                        }
                    }
                    continue;
                }

                /* Binary arithmetic operations */
                if (ins->op == OP_ADD || ins->op == OP_SUB || ins->op == OP_MUL ||
                    ins->op == OP_DIV || ins->op == OP_MOD || ins->op == OP_AND ||
                    ins->op == OP_OR  || ins->op == OP_XOR || ins->op == OP_SHL ||
                    ins->op == OP_SHR || ins->op == OP_SAR) {
                    if (zc_temp_is_user(ins->temp_dest) && ins->temp_dest < temp_count) {
                        if (zc_temp_is_user(ins->temp_src1) && ins->temp_src1 < temp_count &&
                            zc_temp_is_user(ins->temp_src2) && ins->temp_src2 < temp_count) {
                            LatticeValue v1 = lat[ins->temp_src1];
                            LatticeValue v2 = lat[ins->temp_src2];

                            /* Annihilators: x * 0 = 0, x & 0 = 0 */
                            if ((ins->op == OP_MUL || ins->op == OP_AND) &&
                                ((v1.status == LATTICE_CONST && v1.val == 0) ||
                                 (v2.status == LATTICE_CONST && v2.val == 0))) {
                                if (sccp_meet(&lat[ins->temp_dest], LATTICE_CONST, 0)) {
                                    any_changed = true;
                                }
                            } else if (v1.status == LATTICE_BOT || v2.status == LATTICE_BOT) {
                                if (sccp_meet(&lat[ins->temp_dest], LATTICE_BOT, 0)) {
                                    any_changed = true;
                                }
                            } else if (v1.status == LATTICE_CONST && v2.status == LATTICE_CONST) {
                                int64_t res = 0;
                                if (sccp_eval_binop(ins->op, v1.val, v2.val, &res)) {
                                    if (sccp_meet(&lat[ins->temp_dest], LATTICE_CONST, res)) {
                                        any_changed = true;
                                    }
                                } else {
                                    if (sccp_meet(&lat[ins->temp_dest], LATTICE_BOT, 0)) {
                                        any_changed = true;
                                    }
                                }
                            }
                        } else {
                            if (sccp_meet(&lat[ins->temp_dest], LATTICE_BOT, 0)) {
                                any_changed = true;
                            }
                        }
                    }
                    continue;
                }

                /* Unary arithmetic and sign/zero extensions */
                if (ins->op == OP_NEG || ins->op == OP_NOT || ins->op == OP_LNOT ||
                    ins->op == OP_SEXT8 || ins->op == OP_SEXT16 || ins->op == OP_SEXT32 ||
                    ins->op == OP_ZEXT8 || ins->op == OP_ZEXT16 || ins->op == OP_ZEXT32) {
                    if (zc_temp_is_user(ins->temp_dest) && ins->temp_dest < temp_count) {
                        if (zc_temp_is_user(ins->temp_src1) && ins->temp_src1 < temp_count) {
                            LatticeValue v1 = lat[ins->temp_src1];
                            if (v1.status == LATTICE_BOT) {
                                if (sccp_meet(&lat[ins->temp_dest], LATTICE_BOT, 0)) {
                                    any_changed = true;
                                }
                            } else if (v1.status == LATTICE_CONST) {
                                int64_t res = 0;
                                if (sccp_eval_unop(ins->op, v1.val, &res)) {
                                    if (sccp_meet(&lat[ins->temp_dest], LATTICE_CONST, res)) {
                                        any_changed = true;
                                    }
                                } else {
                                    if (sccp_meet(&lat[ins->temp_dest], LATTICE_BOT, 0)) {
                                        any_changed = true;
                                    }
                                }
                            }
                        } else {
                            if (sccp_meet(&lat[ins->temp_dest], LATTICE_BOT, 0)) {
                                any_changed = true;
                            }
                        }
                    }
                    continue;
                }

                /* Comparison and conditional set */
                if (ins->op == OP_CMP) {
                    continue;
                }
                if (ins->op == OP_COND_SET) {
                    if (i > blk->start) {
                        Instr *prev = ir_get(c, i - 1);
                        if (prev && prev->op == OP_CMP &&
                            zc_temp_is_user(prev->temp_src1) && prev->temp_src1 < temp_count &&
                            zc_temp_is_user(prev->temp_src2) && prev->temp_src2 < temp_count) {
                            LatticeValue v1 = lat[prev->temp_src1];
                            LatticeValue v2 = lat[prev->temp_src2];
                            if (v1.status == LATTICE_CONST && v2.status == LATTICE_CONST) {
                                int cond = ins->cond;
                                bool res = false;
                                int64_t va = v1.val, vb = v2.val;
                                switch (cond) {
                                    case CC_EQ: res = (va == vb); break;
                                    case CC_NE: res = (va != vb); break;
                                    case CC_LT: res = (va < vb); break;
                                    case CC_LE: res = (va <= vb); break;
                                    case CC_GT: res = (va > vb); break;
                                    case CC_GE: res = (va >= vb); break;
                                    case CC_FLT_LT: res = ((uint64_t)va < (uint64_t)vb); break;
                                    case CC_FLT_LE: res = ((uint64_t)va <= (uint64_t)vb); break;
                                    case CC_FLT_GT: res = ((uint64_t)va > (uint64_t)vb); break;
                                    case CC_FLT_GE: res = ((uint64_t)va >= (uint64_t)vb); break;
                                    default: continue;
                                }
                                if (zc_temp_is_user(ins->temp_dest) && ins->temp_dest < temp_count) {
                                    if (sccp_meet(&lat[ins->temp_dest], LATTICE_CONST, res ? 1 : 0)) {
                                        any_changed = true;
                                    }
                                }
                                continue;
                            }
                        }
                    }
                }

                /* Non-constant instructions writing destination */
                if (zc_temp_is_user(ins->temp_dest) && ins->temp_dest < temp_count) {
                    if (sccp_meet(&lat[ins->temp_dest], LATTICE_BOT, 0)) {
                        any_changed = true;
                    }
                }
            }

            /* Block terminator control flow analysis */
            if (blk->end > blk->start) {
                Instr *term = ir_get(c, blk->end - 1);
                if (term && (term->op == OP_JE || term->op == OP_JNE)) {
                    int target_b = sccp_find_label_block(cfg, c, term->label_idx);
                    int fallthru_b = (b + 1 < num_blocks) ? (b + 1) : -1;

                    if (zc_temp_is_user(term->temp_src1) && term->temp_src1 < temp_count) {
                        LatticeValue cond_lat = lat[term->temp_src1];
                        if (cond_lat.status == LATTICE_CONST) {
                            bool jump_taken = (term->op == OP_JE) ?
                                (cond_lat.val == 0) : (cond_lat.val != 0);
                            int active_succ = jump_taken ? target_b : fallthru_b;
                            if (active_succ >= 0 && !block_executable[active_succ]) {
                                block_executable[active_succ] = true;
                                any_changed = true;
                            }
                            continue;
                        } else if (cond_lat.status == LATTICE_TOP) {
                            /* Optimistic: neither edge executable yet */
                            continue;
                        }
                    }

                    /* Dynamic condition (LATTICE_BOT): both paths are executable */
                    if (target_b >= 0 && !block_executable[target_b]) {
                        block_executable[target_b] = true;
                        any_changed = true;
                    }
                    if (fallthru_b >= 0 && !block_executable[fallthru_b]) {
                        block_executable[fallthru_b] = true;
                        any_changed = true;
                    }
                    continue;
                }

                /* Unconditional branches or sequential fallthrough */
                for (int s = 0; s < blk->succ_count; s++) {
                    int succ_b = blk->succ[s];
                    if (succ_b >= 0 && succ_b < num_blocks && !block_executable[succ_b]) {
                        block_executable[succ_b] = true;
                        any_changed = true;
                    }
                }
            }
        }
    }

    /* Transformation Phase */
    bool ir_modified = false;

    /* 1. Fold conditional branches whose conditions are proven constants */
    for (int b = 0; b < num_blocks; b++) {
        if (!block_executable[b]) continue;
        const ZeroCCBlock *blk = &cfg->blocks[b];
        if (blk->end == blk->start) continue;

        Instr *term = ir_get(c, blk->end - 1);
        if (!term || (term->op != OP_JE && term->op != OP_JNE)) continue;
        if (!zc_temp_is_user(term->temp_src1) || term->temp_src1 >= temp_count) continue;

        LatticeValue cond_lat = lat[term->temp_src1];
        if (cond_lat.status != LATTICE_CONST) continue;

        bool jump_taken = (term->op == OP_JE) ?
            (cond_lat.val == 0) : (cond_lat.val != 0);

        if (jump_taken) {
            term->op = OP_JMP;
            term->temp_dest = TEMP_NONE;
            term->temp_src1 = TEMP_NONE;
            term->temp_src2 = TEMP_NONE;
            term->imm = 0;
            term->imm64 = 0;
        } else {
            term->op = OP_NOP;
            term->temp_dest = TEMP_NONE;
            term->temp_src1 = TEMP_NONE;
            term->temp_src2 = TEMP_NONE;
            term->imm = 0;
            term->imm64 = 0;
        }
        ir_modified = true;
    }

    /* 2. Prune unreachable blocks (unless indirect jump present) */
    if (!has_indirect) {
        for (int b = 1; b < num_blocks; b++) {
            if (block_executable[b]) continue;
            const ZeroCCBlock *blk = &cfg->blocks[b];
            for (size_t i = blk->start; i < blk->end; i++) {
                Instr *ins = ir_get(c, i);
                if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
                if (ins->op == OP_LABEL) continue;
                ins->op = OP_NOP;
                ins->temp_dest = TEMP_NONE;
                ins->temp_src1 = TEMP_NONE;
                ins->temp_src2 = TEMP_NONE;
                ir_modified = true;
            }
        }
    }

    /* 3. Constant propagation: replace constant ALU operations with OP_MOV_IMM */
    for (int b = 0; b < num_blocks; b++) {
        if (!block_executable[b]) continue;
        const ZeroCCBlock *blk = &cfg->blocks[b];
        for (size_t i = blk->start; i < blk->end; i++) {
            Instr *ins = ir_get(c, i);
            if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
            if (!zc_temp_is_user(ins->temp_dest) || ins->temp_dest >= temp_count) continue;

            LatticeValue lv = lat[ins->temp_dest];
            if (lv.status == LATTICE_CONST && ins->op != OP_MOV_IMM) {
                if (ins->op == OP_ADD || ins->op == OP_SUB || ins->op == OP_MUL ||
                    ins->op == OP_DIV || ins->op == OP_MOD || ins->op == OP_AND ||
                    ins->op == OP_OR  || ins->op == OP_XOR || ins->op == OP_SHL ||
                    ins->op == OP_SHR || ins->op == OP_SAR || ins->op == OP_COND_SET) {
                    ins->op = OP_MOV_IMM;
                    ins->temp_src1 = TEMP_NONE;
                    ins->temp_src2 = TEMP_NONE;
                    ins->imm = (int16_t)lv.val;
                    ins->imm64 = lv.val;
                    ir_modified = true;
                }
            }
        }
    }

    free(lat);
    free(block_executable);

    if (ir_modified) {
        c->cfg_version++;
    }
    return ir_modified;
}
