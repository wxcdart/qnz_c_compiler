#include "zero_cc/opt/opt_gvn.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/opt/opt_alias.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/types/symbols.h"
#include "zero_cc/common/hash_table.h"
#include "zero_cc/opt/cfg.h"

/**
 * Global Value Numbering (GVN), Diamond Hoisting & PRE (Sprint 226)
 *
 * Expands GVN beyond local basic blocks to optimize across control-flow
 * graphs and dominator tree frontiers:
 * 1. Local Basic-Block GVN & Redundant Store/Load Forwarding
 * 2. Diamond Hoisting: Identical pure computations across conditional branches
 *    hoisted into their common immediate dominator.
 * 3. Dominator-Tree GVN: Global value numbering across basic blocks dominated
 *    by earlier computation blocks (accounting for operator commutativity).
 * 4. Partial Redundancy Elimination (PRE): Hoisting partially redundant
 *    expressions across CFG merge points.
 */

/* Value number table for local block GVN */
typedef struct {
    uint16_t op;
    uint32_t src1;
    uint32_t src2;
    int16_t imm;
    uint32_t result_temp;
} ValueEntry;

static ValueEntry g_values[4096];
static int g_value_count;
static HashTable32 *g_value_map;
static uint8_t g_temp_has_value[65536];
static uint16_t g_temp_value_num[65536];

/**
 * Initialize GVN system
 */
void opt_init_gvn(void) {
    g_value_map = malloc(sizeof(HashTable32));
    if (g_value_map) {
        ht32_init(g_value_map, 4096);
    }
}

/**
 * Shutdown GVN system
 */
void opt_shutdown_gvn(void) {
    if (g_value_map) {
        ht32_free(g_value_map);
        free(g_value_map);
        g_value_map = NULL;
    }
}

/**
 * Reset GVN state for a new basic block
 */
static void gvn_reset_block(void) {
    g_value_count = 0;
    memset(g_temp_has_value, 0, sizeof(g_temp_has_value));
    if (g_value_map) {
        ht32_free(g_value_map);
        ht32_init(g_value_map, 4096);
    }
}

/**
 * Check if instruction is a pure expression (can be numbered locally)
 */
static int instr_is_numberable(Instr *instr) {
    switch (instr->op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_SHL:
        case OP_SHR:
        case OP_NEG:
        case OP_NOT:
        case OP_MOV_REG:
        case OP_MOV_IMM:
            return 1;
        default:
            return 0;
    }
}

/**
 * Pure arithmetic/logical operations safe for cross-block GVN, hoisting, and PRE.
 * Excludes constant materialization (MOV_IMM) and register copies (MOV_REG)
 * to prevent cross-block variable aliasing.
 */
static int is_cross_block_pure_op(uint16_t op) {
    switch (op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_SHL:
        case OP_SHR:
        case OP_SAR:
        case OP_NEG:
        case OP_NOT:
            return 1;
        default:
            return 0;
    }
}

static int is_commutative(uint16_t op) {
    return (op == OP_ADD || op == OP_MUL || op == OP_AND || op == OP_OR || op == OP_XOR);
}

/**
 * Compute hash key for an instruction
 */
static uint32_t gvn_key(Instr *instr) {
    uint32_t key = instr->op;
    key = key * 31 + instr->temp_src1;
    key = key * 31 + instr->temp_src2;
    key = key * 31 + (uint32_t)(instr->imm & 0xFFFF);
    return key;
}

/**
 * Find existing value with same computation
 */
static int gvn_find(Instr *instr) {
    if (!g_value_map) return -1;
    uint32_t key = gvn_key(instr);
    uint32_t val_num;

    if (ht32_get(g_value_map, key, &val_num)) {
        return (int)val_num;
    }
    return -1;
}

/**
 * Add a new value to the table
 */
static void gvn_add(Instr *instr, int val_num) {
    if (!g_value_map) return;
    uint32_t key = gvn_key(instr);

    g_values[val_num].op = instr->op;
    g_values[val_num].src1 = instr->temp_src1;
    g_values[val_num].src2 = instr->temp_src2;
    g_values[val_num].imm = instr->imm;
    g_values[val_num].result_temp = instr->temp_dest;

    ht32_put(g_value_map, key, (uint32_t)val_num);
}

/**
 * Replace instruction with MOV from existing value
 */
static void replace_with_gvn_copy(ZeroCCCompiler *c, int instr_idx, uint32_t src_temp) {
    Instr *instr = ir_get(c, instr_idx);
    if (!instr) return;

    instr->op = OP_MOV_REG;
    instr->temp_src1 = src_temp;
    instr->temp_src2 = TEMP_NONE;
    instr->imm = 0;
    instr->label_idx = 0;
}

/* =========================================================================
 * Restrict-aware load elimination / store-to-load forwarding (block scoped)
 * ========================================================================= */
#define MF_MAX_ENTRIES 128
#define MF_MAX_SLOTS   64

typedef struct {
    uint16_t op;       /* load opcode this entry satisfies */
    uint16_t addr;     /* canonical address temp */
    uint16_t val;      /* temp holding the value at addr */
    uint8_t group;     /* restrict alias group (0 = unconstrained) */
} MemEntry;

static MemEntry g_mf[MF_MAX_ENTRIES];
static int g_mf_n;
static int16_t g_slot_off[MF_MAX_SLOTS];
static uint16_t g_slot_can[MF_MAX_SLOTS];
static int g_slot_n;

typedef struct {
    uint32_t gen;  /* generation this slot was written in (0 = never) */
    uint16_t val;  /* canonical temp */
} CanonSlot;
static CanonSlot g_canon[65536];
static uint32_t g_canon_gen = 1;

static inline uint16_t canon_get(uint16_t t) {
    return (g_canon[t].gen == g_canon_gen) ? g_canon[t].val : (uint16_t)t;
}
static inline void canon_set(uint16_t t, uint16_t v) {
    g_canon[t].gen = g_canon_gen;
    g_canon[t].val = v;
}

static void mf_reset(void) {
    g_mf_n = 0;
    g_slot_n = 0;
    g_canon_gen++;
    if (g_canon_gen == 0) {
        for (int i = 0; i < 65536; i++) g_canon[i].gen = 0;
        g_canon_gen = 1;
    }
}

static void mf_remove_if_group(int only_group) {
    int w = 0;
    for (int r = 0; r < g_mf_n; r++) {
        int kill = (only_group < 0) || g_mf[r].group == 0 || g_mf[r].group == (uint8_t)only_group;
        if (!kill) g_mf[w++] = g_mf[r];
    }
    g_mf_n = w;
}

static void mf_forget_temp(uint16_t t) {
    int w = 0;
    for (int r = 0; r < g_mf_n; r++) {
        if (g_mf[r].addr != t && g_mf[r].val != t) g_mf[w++] = g_mf[r];
    }
    g_mf_n = w;
}

static void mf_invalidate_slots_near(int16_t off) {
    int w = 0;
    for (int r = 0; r < g_slot_n; r++) {
        int d = g_slot_off[r] - off;
        if (d <= -8 || d >= 8) {
            g_slot_off[w] = g_slot_off[r];
            g_slot_can[w] = g_slot_can[r];
            w++;
        }
    }
    g_slot_n = w;
}

static int mf_is_load(uint16_t op) {
    return op == OP_DEREF || op == OP_LOAD_BYTE || op == OP_LOAD_16 || op == OP_LOAD_32;
}

static void opt_restrict_memory_forward(ZeroCCCompiler *c) {
    int count = (int)ir_get_count(c);
    mf_reset();

    for (int i = 0; i < count; i++) {
        Instr *in = ir_get(c, i);
        if (!in) continue;

        switch (in->op) {
            case OP_LABEL: case OP_JMP: case OP_JMP_LABEL: case OP_JMP_INDIRECT: case OP_JE: case OP_JNE:
            case OP_JL: case OP_JLE: case OP_JG: case OP_JCOND:
            case OP_CALL: case OP_CALL_INDIRECT: case OP_SYSCALL: case OP_CLONE:
            case OP_DIV64: case OP_MOD64: case OP_UDIV64: case OP_UMOD64:
            case OP_SETJMP: case OP_LONGJMP:
            case OP_RET: case OP_EPILOGUE:
            case OP_VLA_ALLOC: case OP_PUSH: case OP_POP:
            case OP_PARAM_SAVE_INT: case OP_PARAM_SAVE_FP: case OP_VARIADIC_SPILL:
            case OP_CALLEE_SAVE_STORES: case OP_CALLEE_SAVE_LOADS:
            case OP_ATOMIC_XADD: case OP_ATOMIC_OR: case OP_ATOMIC_AND: case OP_ATOMIC_XOR:
            case OP_ATOMIC_CMPXCHG: case OP_ATOMIC_CMPXCHG_BOOL:
            case OP_ATOMIC_XCHG: case OP_MFENCE: case OP_TRAP:
            case OP_ASM:
            case OP_FSTORE: case OP_VSTORE: case OP_V256_STORE: case OP_V512_STORE: case OP_V512_MASK_STORE: case OP_VSTORESS: case OP_VSTORESD:
                mf_reset();
                continue;
            default:
                break;
        }

        if (zc_temp_is_user(in->temp_dest) && in->temp_dest < 65536) {
            mf_forget_temp((uint16_t)in->temp_dest);
            canon_set((uint16_t)in->temp_dest, (uint16_t)in->temp_dest);
        }

        if (in->op == OP_MOV && in->temp_src1 == TEMP_NONE &&
            zc_temp_is_user(in->temp_dest) && in->temp_dest < 65536) {
            int found = -1;
            for (int s = 0; s < g_slot_n; s++) {
                if (g_slot_off[s] == in->imm) { found = s; break; }
            }
            if (found >= 0) {
                canon_set((uint16_t)in->temp_dest, g_slot_can[found]);
            } else if (g_slot_n < MF_MAX_SLOTS) {
                g_slot_off[g_slot_n] = in->imm;
                g_slot_can[g_slot_n] = (uint16_t)in->temp_dest;
                g_slot_n++;
            }
            continue;
        }

        if (in->op == OP_MOV_MEM) {
            mf_invalidate_slots_near(in->imm);
            continue;
        }

        if (in->op == OP_MOV_REG &&
            zc_temp_is_user(in->temp_dest) && in->temp_dest < 65536 &&
            zc_temp_is_user(in->temp_src1) && in->temp_src1 < 65536) {
            canon_set((uint16_t)in->temp_dest, canon_get((uint16_t)in->temp_src1));
            continue;
        }

        if (mf_is_load(in->op) &&
            zc_temp_is_user(in->temp_dest) && in->temp_dest < 65536 &&
            zc_temp_is_user(in->temp_src1) && in->temp_src1 < 65536) {
            uint16_t addr = canon_get((uint16_t)in->temp_src1);
            int hit = -1;
            for (int e = 0; e < g_mf_n; e++) {
                if (g_mf[e].op == in->op && g_mf[e].addr == addr) { hit = e; break; }
            }
            if (hit >= 0) {
                uint16_t val = g_mf[hit].val;
                in->op = OP_MOV_REG;
                in->temp_src1 = val;
                in->temp_src2 = TEMP_NONE;
                in->imm = 0;
                in->imm64 = 0;
                in->label_idx = 0;
                in->alias_group = 0;
                canon_set((uint16_t)in->temp_dest, canon_get(val));
                opt_profile_count_gvn_eliminated();
            } else if (g_mf_n < MF_MAX_ENTRIES) {
                g_mf[g_mf_n].op = in->op;
                g_mf[g_mf_n].addr = addr;
                g_mf[g_mf_n].val = (uint16_t)in->temp_dest;
                g_mf[g_mf_n].group = in->alias_group;
                g_mf_n++;
            }
            continue;
        }

        if (in->op == OP_STORE_BYTE || in->op == OP_STORE_16 ||
            in->op == OP_STORE_32 || in->op == OP_STORE_64) {
            if (in->alias_group == 0) {
                /* TBAA: preserve pending loads that cannot alias this store */
                uint32_t s_addr = (in->temp_src2 != TEMP_NONE) ?
                    canon_get((uint16_t)in->temp_src2) : TEMP_NONE;
                int write_idx = 0;
                for (int e = 0; e < g_mf_n; e++) {
                    AliasResult ar = zero_cc_alias_query(c,
                        in->op, s_addr, in->alias_group,
                        g_mf[e].op, g_mf[e].addr, g_mf[e].group);
                    if (ar == ALIAS_NO) {
                        if (write_idx != e) g_mf[write_idx] = g_mf[e];
                        write_idx++;
                    }
                }
                g_mf_n = write_idx;
                g_slot_n = 0;
            } else {
                mf_remove_if_group(in->alias_group);
            }
            if (in->op == OP_STORE_64 &&
                zc_temp_is_user(in->temp_src1) && in->temp_src1 < 65536 &&
                zc_temp_is_user(in->temp_src2) && in->temp_src2 < 65536 &&
                g_mf_n < MF_MAX_ENTRIES) {
                g_mf[g_mf_n].op = OP_DEREF;
                g_mf[g_mf_n].addr = canon_get((uint16_t)in->temp_src2);
                g_mf[g_mf_n].val = (uint16_t)in->temp_src1;
                g_mf[g_mf_n].group = in->alias_group;
                g_mf_n++;
            }
            continue;
        }
    }
}

/**
 * Basic block-level local GVN
 */
static void opt_gvn_local_bb(ZeroCCCompiler *c) {
    int count = (int)ir_get_count(c);

    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(c, i);
        if (!instr) continue;

        opt_profile_count_gvn_instruction();

        if (instr->op == OP_LABEL || instr->op == OP_SETJMP || instr->op == OP_LONGJMP) {
            gvn_reset_block();
            continue;
        }

        if (!instr_is_numberable(instr)) {
            continue;
        }

        if (!zc_temp_is_user(instr->temp_dest) || instr->temp_dest >= 65536 ||
            (instr->temp_src1 != TEMP_NONE && !zc_temp_is_user(instr->temp_src1)) ||
            (instr->temp_src2 != TEMP_NONE && !zc_temp_is_user(instr->temp_src2))) {
            continue;
        }

        int existing = gvn_find(instr);

        if (existing >= 0 && g_temp_has_value[g_values[existing].result_temp]) {
            replace_with_gvn_copy(c, i, g_values[existing].result_temp);
            opt_profile_count_gvn_eliminated();

            g_temp_has_value[instr->temp_dest] = 1;
            g_temp_value_num[instr->temp_dest] = (uint16_t)existing;
        } else {
            if (g_value_count < 4096) {
                int val_num = g_value_count++;
                gvn_add(instr, val_num);
                g_temp_has_value[instr->temp_dest] = 1;
                g_temp_value_num[instr->temp_dest] = (uint16_t)val_num;
            }
        }
    }
}

/* =========================================================================
 * Dominator-Tree Cross-Block GVN, Diamond Hoisting & PRE (Sprint 226)
 * ========================================================================= */

typedef struct {
    uint16_t def_count;
    int16_t def_block;
    int32_t def_idx;
} TempDefInfo;

static TempDefInfo g_temp_defs[65536];

static void collect_temp_definitions(ZeroCCCompiler *c, const ZeroCCCFG *cfg) {
    memset(g_temp_defs, 0, sizeof(g_temp_defs));
    for (int b = 0; b < cfg->block_count; b++) {
        for (size_t i = cfg->blocks[b].start; i < cfg->blocks[b].end; i++) {
            Instr *in = ir_get(c, i);
            if (!in) continue;
            if (zc_temp_is_user(in->temp_dest) && in->temp_dest < 65536) {
                g_temp_defs[in->temp_dest].def_count++;
                g_temp_defs[in->temp_dest].def_block = (int16_t)b;
                g_temp_defs[in->temp_dest].def_idx = (int32_t)i;
            }
        }
    }
}

static int is_terminal_or_branch(uint16_t op) {
    switch (op) {
        case OP_JMP:
        case OP_JMP_LABEL:
        case OP_JMP_INDIRECT:
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_JG:
        case OP_JCOND:
        case OP_COND_SET:
        case OP_CMP:
        case OP_CMP_ZERO:
        case OP_RET:
        case OP_EPILOGUE:
            return 1;
        default:
            return 0;
    }
}

static int find_safe_branch_insert_pos(const ZeroCCCFG *cfg, ZeroCCCompiler *c, int block_idx) {
    if (block_idx < 0 || block_idx >= cfg->block_count) return -1;
    size_t start = cfg->blocks[block_idx].start;
    size_t end = cfg->blocks[block_idx].end;
    if (end <= start) return (int)start;

    int pos = (int)end;
    for (int i = (int)end - 1; i >= (int)start; i--) {
        Instr *in = ir_get(c, (size_t)i);
        if (!in) break;
        if (is_terminal_or_branch(in->op)) {
            pos = i;
        } else {
            break;
        }
    }
    return pos;
}

static int operand_available_at(uint32_t temp, int block_idx, int insert_pos, const ZeroCCCFG *cfg) {
    if (temp == TEMP_NONE) return 1;
    if (!zc_temp_is_user(temp) || temp >= 65536) return 0;
    if (g_temp_defs[temp].def_count != 1) return 0;

    int d_blk = g_temp_defs[temp].def_block;
    if (d_blk == block_idx) {
        return g_temp_defs[temp].def_idx < insert_pos;
    }
    return cfg_block_dominates(cfg, d_blk, block_idx);
}

/**
 * Diamond Hoisting:
 * Detects identical computations in both arms of an if-else diamond
 * and hoists them into the predecessor block P before the branch.
 */
static int opt_gvn_diamond_hoist(ZeroCCCompiler *c) {
    if (!c || !cfg_build(c)) return 0;
    const int *idom = cfg_idom(c);
    if (!idom) return 0;
    ZeroCCCFG *cfg = &c->cfg_cache;
    int num_blocks = cfg->block_count;
    if (num_blocks <= 2) return 0;

    collect_temp_definitions(c, cfg);
    int total_hoisted = 0;

    for (int p = 0; p < num_blocks; p++) {
        if (cfg->blocks[p].succ_count != 2) continue;
        int b1 = cfg->blocks[p].succ[0];
        int b2 = cfg->blocks[p].succ[1];
        if (b1 == b2 || b1 < 0 || b2 < 0 || b1 >= num_blocks || b2 >= num_blocks) continue;

        /* Verify b1 and b2 are exclusive arms with p as their sole predecessor */
        if (cfg->blocks[b1].pred_count != 1 || cfg->blocks[b2].pred_count != 1) continue;
        if (cfg->blocks[b1].preds[0] != p || cfg->blocks[b2].preds[0] != p) continue;

        int ins_pos = find_safe_branch_insert_pos(cfg, c, p);
        if (ins_pos < (int)cfg->blocks[p].start) continue;

        /* Scan instructions in b1 */
        for (size_t i = cfg->blocks[b1].start; i < cfg->blocks[b1].end; i++) {
            Instr *in1 = ir_get(c, i);
            if (!in1 || !is_cross_block_pure_op(in1->op)) continue;
            if (!zc_temp_is_user(in1->temp_dest) || in1->temp_dest >= 65536) continue;
            if (g_temp_defs[in1->temp_dest].def_count != 1) continue;

            /* Check operand availability in p */
            if (!operand_available_at(in1->temp_src1, p, ins_pos, cfg)) continue;
            if (!operand_available_at(in1->temp_src2, p, ins_pos, cfg)) continue;

            /* Search matching instruction in b2 */
            int found_b2 = -1;
            for (size_t j = cfg->blocks[b2].start; j < cfg->blocks[b2].end; j++) {
                Instr *in2 = ir_get(c, j);
                if (!in2 || in2->op != in1->op || in2->imm != in1->imm) continue;
                if (!zc_temp_is_user(in2->temp_dest) || in2->temp_dest >= 65536) continue;
                if (g_temp_defs[in2->temp_dest].def_count != 1) continue;

                int match = 0;
                if (in1->temp_src1 == in2->temp_src1 && in1->temp_src2 == in2->temp_src2) {
                    match = 1;
                } else if (is_commutative(in1->op) &&
                           in1->temp_src1 == in2->temp_src2 && in1->temp_src2 == in2->temp_src1) {
                    match = 1;
                }

                if (match) {
                    found_b2 = (int)j;
                    break;
                }
            }

            if (found_b2 >= 0) {
                /* Hoist computation to p */
                uint32_t t_hoist = (uint32_t)zc_alloc_temp(c);
                Instr hoist_instr = *in1;
                hoist_instr.temp_dest = t_hoist;

                ir_insert_at(c, (size_t)ins_pos, hoist_instr);

                /* Adjust indices shifted by the insertion */
                size_t adj_i = (i >= (size_t)ins_pos) ? (i + 1) : i;
                size_t adj_j = ((size_t)found_b2 >= (size_t)ins_pos) ? ((size_t)found_b2 + 1) : (size_t)found_b2;

                replace_with_gvn_copy(c, (int)adj_i, t_hoist);
                replace_with_gvn_copy(c, (int)adj_j, t_hoist);

                opt_profile_count_gvn_eliminated();
                total_hoisted++;

                /* Invalidate CFG and refresh definition table */
                cfg_invalidate(c);
                if (!cfg_build(c)) return total_hoisted;
                cfg = &c->cfg_cache;
                collect_temp_definitions(c, cfg);
                break;
            }
        }
    }
    return total_hoisted;
}

/**
 * Dominator-Tree Cross-Block GVN
 */
#define MAX_DOM_ENTRIES 2048

typedef struct {
    uint16_t op;
    uint32_t src1;
    uint32_t src2;
    int16_t imm;
    uint32_t result_temp;
    int block_idx;
    int instr_idx;
} DomValueEntry;

static DomValueEntry g_dom_entries[MAX_DOM_ENTRIES];
static int g_dom_entry_count;

static void opt_gvn_dom_tree(ZeroCCCompiler *c) {
    if (!c || !cfg_build(c)) return;
    const int *idom = cfg_idom(c);
    if (!idom) return;
    ZeroCCCFG *cfg = &c->cfg_cache;
    int num_blocks = cfg->block_count;
    if (num_blocks <= 1) return;

    collect_temp_definitions(c, cfg);
    g_dom_entry_count = 0;

    for (int b = 0; b < num_blocks; b++) {
        for (size_t i = cfg->blocks[b].start; i < cfg->blocks[b].end; i++) {
            Instr *in = ir_get(c, i);
            if (!in || !is_cross_block_pure_op(in->op)) continue;
            if (!zc_temp_is_user(in->temp_dest) || in->temp_dest >= 65536) continue;
            if (g_temp_defs[in->temp_dest].def_count != 1) continue;

            /* Cross-block pure operations must have valid single-def user temps */
            if (in->temp_src1 == TEMP_NONE || !zc_temp_is_user(in->temp_src1) || in->temp_src1 >= 65536) continue;
            if (g_temp_defs[in->temp_src1].def_count != 1) continue;

            if (in->temp_src2 != TEMP_NONE) {
                if (!zc_temp_is_user(in->temp_src2) || in->temp_src2 >= 65536) continue;
                if (g_temp_defs[in->temp_src2].def_count != 1) continue;
            }

            uint32_t s1 = in->temp_src1;
            uint32_t s2 = in->temp_src2;
            if (is_commutative(in->op) && s1 != TEMP_NONE && s2 != TEMP_NONE && s1 > s2) {
                uint32_t tmp = s1; s1 = s2; s2 = tmp;
            }

            /* Look for a dominating existing computation */
            int found = -1;
            for (int k = 0; k < g_dom_entry_count; k++) {
                DomValueEntry *e = &g_dom_entries[k];
                if (e->op == in->op && e->src1 == s1 && e->src2 == s2 && e->imm == in->imm) {
                    if (e->block_idx == b) {
                        if (e->instr_idx < (int)i) { found = k; break; }
                    } else if (cfg_block_dominates(cfg, e->block_idx, b)) {
                        found = k;
                        break;
                    }
                }
            }

            if (found >= 0) {
                replace_with_gvn_copy(c, (int)i, g_dom_entries[found].result_temp);
                opt_profile_count_gvn_eliminated();
            } else if (g_dom_entry_count < MAX_DOM_ENTRIES) {
                DomValueEntry *e = &g_dom_entries[g_dom_entry_count++];
                e->op = in->op;
                e->src1 = s1;
                e->src2 = s2;
                e->imm = in->imm;
                e->result_temp = in->temp_dest;
                e->block_idx = b;
                e->instr_idx = (int)i;
            }
        }
    }
}

/**
 * Partial Redundancy Elimination (PRE):
 * When an expression is computed along one path to a merge point but not the other,
 * hoist/insert computation on the missing path to make it fully redundant.
 */
static void opt_gvn_pre(ZeroCCCompiler *c) {
    if (!c || !cfg_build(c)) return;
    ZeroCCCFG *cfg = &c->cfg_cache;
    int num_blocks = cfg->block_count;
    if (num_blocks <= 2) return;

    collect_temp_definitions(c, cfg);

    for (int m = 0; m < num_blocks; m++) {
        if (cfg->blocks[m].pred_count != 2) continue;
        int p1 = cfg->blocks[m].preds[0];
        int p2 = cfg->blocks[m].preds[1];
        if (p1 < 0 || p2 < 0 || p1 >= num_blocks || p2 >= num_blocks) continue;

        for (size_t i = cfg->blocks[m].start; i < cfg->blocks[m].end; i++) {
            Instr *in = ir_get(c, i);
            if (!in || !is_cross_block_pure_op(in->op)) continue;
            if (!zc_temp_is_user(in->temp_dest) || in->temp_dest >= 65536) continue;
            if (g_temp_defs[in->temp_dest].def_count != 1) continue;

            if (in->temp_src1 == TEMP_NONE || !zc_temp_is_user(in->temp_src1) || in->temp_src1 >= 65536) continue;
            if (g_temp_defs[in->temp_src1].def_count != 1) continue;

            if (in->temp_src2 != TEMP_NONE) {
                if (!zc_temp_is_user(in->temp_src2) || in->temp_src2 >= 65536) continue;
                if (g_temp_defs[in->temp_src2].def_count != 1) continue;
            }

            uint32_t s1 = in->temp_src1;
            uint32_t s2 = in->temp_src2;
            if (is_commutative(in->op) && s1 != TEMP_NONE && s2 != TEMP_NONE && s1 > s2) {
                uint32_t tmp = s1; s1 = s2; s2 = tmp;
            }

            /* Check if available along p1 */
            uint32_t res_p1 = TEMP_NONE;
            for (int k = 0; k < g_dom_entry_count; k++) {
                DomValueEntry *e = &g_dom_entries[k];
                if (e->op == in->op && e->src1 == s1 && e->src2 == s2 && e->imm == in->imm) {
                    if (e->block_idx == p1 || cfg_block_dominates(cfg, e->block_idx, p1)) {
                        res_p1 = e->result_temp;
                        break;
                    }
                }
            }

            /* Check if available along p2 */
            uint32_t res_p2 = TEMP_NONE;
            for (int k = 0; k < g_dom_entry_count; k++) {
                DomValueEntry *e = &g_dom_entries[k];
                if (e->op == in->op && e->src1 == s1 && e->src2 == s2 && e->imm == in->imm) {
                    if (e->block_idx == p2 || cfg_block_dominates(cfg, e->block_idx, p2)) {
                        res_p2 = e->result_temp;
                        break;
                    }
                }
            }

            if (res_p1 != TEMP_NONE && res_p2 == TEMP_NONE) {
                int ins_pos2 = find_safe_branch_insert_pos(cfg, c, p2);
                if (ins_pos2 >= (int)cfg->blocks[p2].start &&
                    operand_available_at(in->temp_src1, p2, ins_pos2, cfg) &&
                    operand_available_at(in->temp_src2, p2, ins_pos2, cfg)) {
                    uint32_t t_common = (uint32_t)zc_alloc_temp(c);
                    Instr comp_instr = *in;
                    comp_instr.temp_dest = t_common;
                    ir_insert_at(c, (size_t)ins_pos2, comp_instr);

                    int ins_pos1 = find_safe_branch_insert_pos(cfg, c, p1);
                    if (ins_pos1 >= (int)cfg->blocks[p1].start) {
                        Instr mov_instr = {
                            .op = OP_MOV_REG,
                            .temp_dest = t_common,
                            .temp_src1 = res_p1,
                            .temp_src2 = TEMP_NONE,
                            .imm = 0,
                            .imm64 = 0,
                            .label_idx = 0
                        };
                        ir_insert_at(c, (size_t)ins_pos1, mov_instr);
                        replace_with_gvn_copy(c, (int)i, t_common);
                        opt_profile_count_gvn_eliminated();
                        cfg_invalidate(c);
                        return;
                    }
                }
            } else if (res_p2 != TEMP_NONE && res_p1 == TEMP_NONE) {
                int ins_pos1 = find_safe_branch_insert_pos(cfg, c, p1);
                if (ins_pos1 >= (int)cfg->blocks[p1].start &&
                    operand_available_at(in->temp_src1, p1, ins_pos1, cfg) &&
                    operand_available_at(in->temp_src2, p1, ins_pos1, cfg)) {
                    uint32_t t_common = (uint32_t)zc_alloc_temp(c);
                    Instr comp_instr = *in;
                    comp_instr.temp_dest = t_common;
                    ir_insert_at(c, (size_t)ins_pos1, comp_instr);

                    int ins_pos2 = find_safe_branch_insert_pos(cfg, c, p2);
                    if (ins_pos2 >= (int)cfg->blocks[p2].start) {
                        Instr mov_instr = {
                            .op = OP_MOV_REG,
                            .temp_dest = t_common,
                            .temp_src1 = res_p2,
                            .temp_src2 = TEMP_NONE,
                            .imm = 0,
                            .imm64 = 0,
                            .label_idx = 0
                        };
                        ir_insert_at(c, (size_t)ins_pos2, mov_instr);
                        replace_with_gvn_copy(c, (int)i, t_common);
                        opt_profile_count_gvn_eliminated();
                        cfg_invalidate(c);
                        return;
                    }
                }
            }
        }
    }
}

/**
 * Global Value Numbering pass
 */
void opt_eliminate_gvn(ZeroCCCompiler *c) {
    /* Step 1: Basic-block local GVN */
    opt_gvn_local_bb(c);

    /* Step 2: Diamond Hoisting */
    opt_gvn_diamond_hoist(c);

    /* Step 3: Dominator-tree Cross-Block GVN */
    opt_gvn_dom_tree(c);

    /* Step 4: Partial Redundancy Elimination (PRE) */
    opt_gvn_pre(c);

    /* Step 5: Restrict memory forwarding */
    opt_restrict_memory_forward(c);
}
