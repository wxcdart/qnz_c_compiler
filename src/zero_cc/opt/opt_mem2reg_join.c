#include "zero_cc/opt/opt_mem2reg_join.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/types/symbols.h"
#include "zero_cc/opt/cfg.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/types/vregs.h"

#define MAX_TRACKED_SLOTS 256

static int slot_from_offset(int64_t offset) {
    if (offset >= 0 || (offset % 8) != 0) return -1;
    int s = (int)((-offset) / 8);
    return (s >= 0 && s < MAX_TRACKED_SLOTS) ? s : -1;
}

static bool block_has_call_or_barrier(ZeroCCCompiler *c, size_t start, size_t end) {
    for (size_t i = start; i < end; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        switch (ins->op) {
            case OP_CALL: case OP_CALL_INDIRECT: case OP_SYSCALL: case OP_CLONE:
            case OP_ASM: case OP_SETJMP: case OP_LONGJMP:
            case OP_VLA_ALLOC:
                return true;
            default:
                break;
        }
    }
    return false;
}

bool opt_promote_mem2reg_join(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp || c->lowering.has_capture_mirror) return false;
    if (!cfg_build(c)) return false;

    ZeroCCCFG *cfg = &c->cfg_cache;
    if (cfg->block_count < 3) return false;

    size_t count = ir_get_count(c);
    if (count == 0) return false;

    /* 1. Track address-taken slots and VLA presence */
    bool slot_addr_taken[MAX_TRACKED_SLOTS];
    memset(slot_addr_taken, 0, sizeof(slot_addr_taken));

    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_VLA_ALLOC) return false; /* VLA disables scalar mem2reg */
        if (ins->op == OP_LEA) {
            int s = slot_from_offset(ins->imm64);
            if (s >= 0) slot_addr_taken[s] = true;
        }
    }

    bool changed = false;

    /* 2. Walk CFG looking for join blocks with exactly 2 incoming predecessors */
    for (int b = 0; b < cfg->block_count; b++) {
        if (cfg->blocks[b].pred_count != 2) continue;
        if (cfg->blocks[b].is_loop_header) continue;

        int p1 = cfg->blocks[b].preds[0];
        int p2 = cfg->blocks[b].preds[1];
        if (p1 == p2 || p1 < 0 || p2 < 0 || p1 >= cfg->block_count || p2 >= cfg->block_count) continue;
        if (p1 >= b || p2 >= b) continue; /* forward control flow only (no loop backedges) */

        /* Verify neither branch has function calls or asm barriers */
        if (block_has_call_or_barrier(c, cfg->blocks[p1].start, cfg->blocks[p1].end)) continue;
        if (block_has_call_or_barrier(c, cfg->blocks[p2].start, cfg->blocks[p2].end)) continue;

        /* Check for slots stored in both predecessors and loaded in the join block */
        for (int s = 0; s < MAX_TRACKED_SLOTS; s++) {
            if (slot_addr_taken[s]) continue;
            int64_t target_offset = -(int64_t)(s * 8);

            /* Find last store in p1 */
            int st1_idx = -1;
            for (int i = (int)cfg->blocks[p1].end - 1; i >= (int)cfg->blocks[p1].start; i--) {
                Instr *ins = ir_get(c, i);
                if (ins && ins->op == OP_MOV_MEM && ins->imm64 == target_offset) {
                    st1_idx = i;
                    break;
                }
            }
            if (st1_idx < 0) continue;

            /* Find last store in p2 */
            int st2_idx = -1;
            for (int i = (int)cfg->blocks[p2].end - 1; i >= (int)cfg->blocks[p2].start; i--) {
                Instr *ins = ir_get(c, i);
                if (ins && ins->op == OP_MOV_MEM && ins->imm64 == target_offset) {
                    st2_idx = i;
                    break;
                }
            }
            if (st2_idx < 0) continue;

            Instr *st1 = ir_get(c, st1_idx);
            Instr *st2 = ir_get(c, st2_idx);
            if (!st1 || !st2) continue;

            uint32_t src1 = st1->temp_src1;
            uint32_t src2 = st2->temp_src1;
            if (src1 == TEMP_NONE || src2 == TEMP_NONE) continue;
            if (!zc_temp_is_user(src1) || !zc_temp_is_user(src2)) continue;

            /* Check that join block b has at least one load of this slot before any store */
            int first_load_idx = -1;
            bool store_before_load = false;
            for (size_t i = cfg->blocks[b].start; i < cfg->blocks[b].end; i++) {
                Instr *ins = ir_get(c, i);
                if (!ins) continue;
                if (ins->op == OP_MOV_MEM && ins->imm64 == target_offset) {
                    store_before_load = true;
                    break;
                }
                if (ins->op == OP_MOV && ins->temp_src1 == TEMP_NONE && ins->imm64 == target_offset) {
                    first_load_idx = (int)i;
                    break;
                }
            }
            if (store_before_load || first_load_idx < 0) continue;

            /* Verify that all loads of this slot in the entire function are inside block b */
            bool load_outside_b = false;
            for (int ob = 0; ob < cfg->block_count; ob++) {
                if (ob == b) continue;
                for (size_t i = cfg->blocks[ob].start; i < cfg->blocks[ob].end; i++) {
                    Instr *ins = ir_get(c, i);
                    if (ins && ins->op == OP_MOV && ins->temp_src1 == TEMP_NONE && ins->imm64 == target_offset) {
                        load_outside_b = true;
                        break;
                    }
                }
                if (load_outside_b) break;
            }
            if (load_outside_b) continue;

            /* Slot S is stored on both diamond paths and consumed at the join!
             * Promote slot S into a unified join temporary. */
            int t_join = zc_alloc_temp(c);
            zc_temp_tag_set(c, t_join, zc_temp_tag(c, src1));

            /* Rewrite p1 store to copy into t_join */
            st1->op = OP_MOV_REG;
            st1->temp_dest = (uint32_t)t_join;
            st1->temp_src2 = TEMP_NONE;
            st1->imm = 0;
            st1->imm64 = 0;

            /* Rewrite p2 store to copy into t_join */
            st2->op = OP_MOV_REG;
            st2->temp_dest = (uint32_t)t_join;
            st2->temp_src2 = TEMP_NONE;
            st2->imm = 0;
            st2->imm64 = 0;

            /* Rewrite loads in join block b to read t_join */
            for (size_t i = cfg->blocks[b].start; i < cfg->blocks[b].end; i++) {
                Instr *ins = ir_get(c, i);
                if (!ins) continue;
                if (ins->op == OP_MOV_MEM && ins->imm64 == target_offset) {
                    break; /* slot overwritten by new value */
                }
                if (ins->op == OP_MOV && ins->temp_src1 == TEMP_NONE && ins->imm64 == target_offset) {
                    ins->op = OP_MOV_REG;
                    ins->temp_src1 = (uint32_t)t_join;
                    ins->temp_src2 = TEMP_NONE;
                    ins->imm = 0;
                    ins->imm64 = 0;
                }
            }

            changed = true;
        }
    }

    if (changed) {
        c->cfg_version++;
    }

    return changed;
}
