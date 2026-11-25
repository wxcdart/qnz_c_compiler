#include "zero_cc/opt/opt_dse.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/opt/cfg.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/types/symbols.h"

#define MAX_STACK_SLOTS 256
#define BITSET_WORDS (MAX_STACK_SLOTS / 64)

static inline void bitset_set(uint64_t *b, int bit) {
    if (bit >= 0 && bit < MAX_STACK_SLOTS) {
        b[bit / 64] |= ((uint64_t)1 << (bit % 64));
    }
}

static inline void bitset_clear(uint64_t *b, int bit) {
    if (bit >= 0 && bit < MAX_STACK_SLOTS) {
        b[bit / 64] &= ~((uint64_t)1 << (bit % 64));
    }
}

static inline bool bitset_test(const uint64_t *b, int bit) {
    if (bit >= 0 && bit < MAX_STACK_SLOTS) {
        return (b[bit / 64] & ((uint64_t)1 << (bit % 64))) != 0;
    }
    return false;
}

static inline bool bitset_equal(const uint64_t *a, const uint64_t *b) {
    for (int i = 0; i < BITSET_WORDS; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static inline void bitset_copy(uint64_t *dst, const uint64_t *src) {
    for (int i = 0; i < BITSET_WORDS; i++) {
        dst[i] = src[i];
    }
}

static inline void bitset_or(uint64_t *dst, const uint64_t *src) {
    for (int i = 0; i < BITSET_WORDS; i++) {
        dst[i] |= src[i];
    }
}

static inline void bitset_zero(uint64_t *b) {
    memset(b, 0, sizeof(uint64_t) * BITSET_WORDS);
}

static int slot_from_offset(int64_t offset) {
    if (offset >= 0 || (offset % 8) != 0) return -1;
    return (int)((-offset) / 8);
}

static bool instr_is_stack_store(const Instr *ins) {
    if (!ins) return false;
    if (ins->op == OP_MOV_MEM) return true;
    if (ins->op == OP_FSTORE && ins->temp_src2 == TEMP_NONE) return true;
    return false;
}

static bool instr_is_stack_load(const Instr *ins) {
    if (!ins) return false;
    if (ins->op == OP_MOV && ins->temp_src1 == TEMP_NONE) return true;
    if (ins->op == OP_FLOAD && ins->temp_src2 == TEMP_NONE) return true;
    return false;
}




bool opt_dse(ZeroCCCompiler *c) {
    if (!c) return false;
    if (c->lowering.has_setjmp || c->lowering.has_capture_mirror || c->lowering.vla.has_runtime_vla) return false;

    size_t count = ir_get_count(c);
    if (count == 0) return false;

    bool is_candidate_slot[MAX_STACK_SLOTS];
    memset(is_candidate_slot, 0, sizeof(is_candidate_slot));
    int num_candidates = 0;

    /* 1. Whitelist candidate slots: only non-address-taken, non-volatile scalar locals and SROA slots */
    for (size_t i = 0; i < c->local_count; i++) {
        ZeroCCSym *sym = &c->locals[i];
        if (sym->is_param || sym->is_static_local || sym->is_vla || sym->is_vla_struct) continue;
        if (sym->addr_taken || sym->is_volatile) continue;
        if (sym->is_sroa) {
            for (size_t m = 0; m < sym->type.member_count && m < 16; m++) {
                int off = sym->sroa_offsets[m];
                int slot = off / 8;
                if (slot >= 0 && slot < MAX_STACK_SLOTS && (off % 8) == 0) {
                    is_candidate_slot[slot] = true;
                    num_candidates++;
                }
            }
        } else {
            if (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION || sym->type.tag == TYPE_ARRAY) continue;
            if (sym->type.size == 0 || sym->type.size > 8) continue;
            if (sym->stack_offset > 0 && (sym->stack_offset % 8) == 0) {
                int slot = sym->stack_offset / 8;
                if (slot >= 0 && slot < MAX_STACK_SLOTS) {
                    is_candidate_slot[slot] = true;
                    num_candidates++;
                }
            }
        }
    }

    if (num_candidates == 0) return false;

    /* Disqualify any slot whose address is explicitly taken via LEA */
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_LEA) {
            int slot = slot_from_offset(ins->imm64);
            if (slot >= 0 && slot < MAX_STACK_SLOTS) {
                is_candidate_slot[slot] = false;
            }
        }
    }

    /* 2. Build CFG */
    if (!cfg_build(c)) return false;
    ZeroCCCFG *cfg = &c->cfg_cache;
    int num_blocks = cfg->block_count;
    if (num_blocks <= 0) return false;

    /* Mark blocks and slots that participate in loops:
     * Loop induction/accumulator stores must be preserved for loop unrolling and vectorization */
    bool *in_loop_block = (bool *)calloc((size_t)num_blocks, sizeof(bool));
    if (in_loop_block) {
        for (int l = 0; l < cfg->loop_count; l++) {
            int h = cfg->loops[l].header;
            int b = cfg->loops[l].backedge_from;
            if (h <= b && b < num_blocks) {
                for (int k = h; k <= b; k++) in_loop_block[k] = true;
            }
        }
    }

    bool slot_in_loop[MAX_STACK_SLOTS];
    memset(slot_in_loop, 0, sizeof(slot_in_loop));
    if (in_loop_block) {
        for (int b = 0; b < num_blocks; b++) {
            if (!in_loop_block[b]) continue;
            for (size_t i = cfg->blocks[b].start; i < cfg->blocks[b].end; i++) {
                Instr *ins = ir_get(c, i);
                if (!ins) continue;
                if (instr_is_stack_store(ins)) {
                    int slot = slot_from_offset(ins->imm64);
                    if (slot >= 0 && slot < MAX_STACK_SLOTS) {
                        slot_in_loop[slot] = true;
                    }
                }
            }
        }
        free(in_loop_block);
    }

    /* 3. Allocate dataflow sets per block */
    typedef struct {
        uint64_t gen[BITSET_WORDS];
        uint64_t kill[BITSET_WORDS];
        uint64_t live_in[BITSET_WORDS];
        uint64_t live_out[BITSET_WORDS];
        bool is_exit;
    } BlockDSE;

    BlockDSE *bdata = (BlockDSE *)calloc((size_t)num_blocks, sizeof(BlockDSE));
    if (!bdata) return false;

    /* 4. Compute GEN and KILL for each basic block */
    for (int b = 0; b < num_blocks; b++) {
        size_t start = cfg->blocks[b].start;
        size_t end = cfg->blocks[b].end;
        bdata[b].is_exit = (cfg->blocks[b].succ_count == 0);

        for (size_t i = start; i < end; i++) {
            Instr *ins = ir_get(c, i);
            if (!ins) continue;
            if (ins->op == OP_RET || ins->op == OP_EPILOGUE) {
                bdata[b].is_exit = true;
            }

            if (instr_is_stack_load(ins)) {
                int slot = slot_from_offset(ins->imm64);
                if (slot >= 0 && slot < MAX_STACK_SLOTS && is_candidate_slot[slot]) {
                    if (!bitset_test(bdata[b].kill, slot)) {
                        bitset_set(bdata[b].gen, slot);
                    }
                }
            } else if (instr_is_stack_store(ins)) {
                int slot = slot_from_offset(ins->imm64);
                if (slot >= 0 && slot < MAX_STACK_SLOTS && is_candidate_slot[slot]) {
                    bitset_set(bdata[b].kill, slot);
                }
            }
        }
    }

    /* 5. Iterative backward dataflow analysis */
    bool changed_dataflow = true;
    int max_iters = 50;
    while (changed_dataflow && max_iters-- > 0) {
        changed_dataflow = false;
        for (int b = num_blocks - 1; b >= 0; b--) {
            uint64_t new_out[BITSET_WORDS];
            bitset_zero(new_out);

            if (!bdata[b].is_exit) {
                for (int s_idx = 0; s_idx < cfg->blocks[b].succ_count; s_idx++) {
                    int succ = cfg->blocks[b].succ[s_idx];
                    if (succ >= 0 && succ < num_blocks) {
                        bitset_or(new_out, bdata[succ].live_in);
                    }
                }
            }

            if (!bitset_equal(bdata[b].live_out, new_out)) {
                bitset_copy(bdata[b].live_out, new_out);
                changed_dataflow = true;
            }

            /* LiveIn = Gen | (LiveOut & ~Kill) */
            uint64_t new_in[BITSET_WORDS];
            for (int w = 0; w < BITSET_WORDS; w++) {
                new_in[w] = bdata[b].gen[w] | (bdata[b].live_out[w] & ~bdata[b].kill[w]);
            }

            if (!bitset_equal(bdata[b].live_in, new_in)) {
                bitset_copy(bdata[b].live_in, new_in);
                changed_dataflow = true;
            }
        }
    }

    /* 6. Eliminate dead stores walking backwards within each basic block */
    size_t eliminated = 0;
    for (int b = 0; b < num_blocks; b++) {
        size_t start = cfg->blocks[b].start;
        size_t end = cfg->blocks[b].end;
        if (end <= start) continue;

        uint64_t live[BITSET_WORDS];
        bitset_copy(live, bdata[b].live_out);

        for (size_t i = end; i > start; i--) {
            Instr *ins = ir_get(c, i - 1);
            if (!ins) continue;

            if (ins->op == OP_RET || ins->op == OP_EPILOGUE) {
                /* All candidate slots are dead at exit */
                bitset_zero(live);
                continue;
            }

            if (instr_is_stack_store(ins)) {
                int slot = slot_from_offset(ins->imm64);
                if (slot >= 0 && slot < MAX_STACK_SLOTS && is_candidate_slot[slot] && !slot_in_loop[slot]) {
                    if (!bitset_test(live, slot)) {
                        /* DEAD STORE: this slot is never read before exit or is overwritten */
                        ins->op = OP_DEAD;
                        eliminated++;
                    } else {
                        /* Store makes earlier writes to this slot dead */
                        bitset_clear(live, slot);
                    }
                }
            } else if (instr_is_stack_load(ins)) {
                int slot = slot_from_offset(ins->imm64);
                if (slot >= 0 && slot < MAX_STACK_SLOTS && is_candidate_slot[slot]) {
                    bitset_set(live, slot);
                }
            }
        }
    }

    free(bdata);

    /* 7. Compact IR if any dead stores were eliminated */
    if (eliminated > 0) {
        size_t write_idx = 0;
        for (size_t i = 0; i < count; i++) {
            Instr *ins = ir_get(c, i);
            if (!ins) continue;
            if (ins->op != OP_DEAD) {
                if (write_idx != i) {
                    *ir_get(c, write_idx) = *ins;
                }
                write_idx++;
            }
        }
        c->ir_arena.count = write_idx;
        cfg_invalidate(c);
        return true;
    }

    return false;
}
