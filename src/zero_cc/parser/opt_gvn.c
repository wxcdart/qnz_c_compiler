#include "opt_gvn.h"
#include "opt_profile.h"
#include "ir.h"
#include "zero_cc/common/hash_table.h"
#include <stdlib.h>
#include <string.h>

/**
 * Global Value Numbering (GVN) - Conservative Implementation
 * 
 * This is a simplified GVN that works within basic blocks.
 * It tracks which temps hold the same value and eliminates
 * redundant computations.
 * 
 * Note: This is a conservative implementation. Full GVN would require
 * more sophisticated data-flow analysis across basic blocks.
 */

/* Value number table */
typedef struct {
    uint8_t op;
    uint8_t src1;
    uint8_t src2;
    int16_t imm;
    uint8_t result_temp;
} ValueEntry;

static ValueEntry g_values[4096];
static int g_value_count;
static HashTable32 *g_value_map;
static uint8_t g_temp_has_value[256];
static uint8_t g_temp_value_num[256];

/**
 * Initialize GVN system
 */
void opt_init_gvn(void) {
    g_value_map = malloc(sizeof(HashTable32));
    ht32_init(g_value_map, 4096);
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
    ht32_free(g_value_map);
    ht32_init(g_value_map, 4096);
}

/**
 * Check if instruction is a pure expression (can be numbered)
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
static void replace_with_gvn_copy(int instr_idx, uint8_t src_temp) {
    Instr *instr = ir_get(instr_idx);
    if (!instr) return;
    
    instr->op = OP_MOV_REG;
    instr->temp_src1 = src_temp;
    instr->temp_src2 = 0xFF;
    instr->imm = 0;
    instr->label_idx = 0;
}

/* =========================================================================
 * restrict-aware load elimination / store-to-load forwarding (block scoped)
 *
 * Memory accesses are keyed on (load width, canonical address temp). Temps
 * loaded from the same unmodified stack slot are given the same canonical
 * temp so `*a` evaluated twice is recognised as the same address.
 *
 * Instr.alias_group != 0 identifies accesses through a `restrict` pointer
 * (set during lowering). A store through restrict group G can only conflict
 * with entries of group G (or untagged entries); entries of other restrict
 * groups survive. Untagged stores conservatively clobber everything.
 * ========================================================================= */
#define MF_MAX_ENTRIES 128
#define MF_MAX_SLOTS   64

typedef struct {
    uint8_t op;       /* load opcode this entry satisfies */
    uint8_t addr;     /* canonical address temp */
    uint8_t val;      /* temp holding the value at addr */
    uint8_t group;    /* restrict alias group (0 = unconstrained) */
} MemEntry;

static MemEntry g_mf[MF_MAX_ENTRIES];
static int g_mf_n;
static int16_t g_slot_off[MF_MAX_SLOTS];
static uint8_t g_slot_can[MF_MAX_SLOTS];
static int g_slot_n;
static uint8_t g_canon[256];

static void mf_reset(void) {
    g_mf_n = 0;
    g_slot_n = 0;
    for (int i = 0; i < 256; i++) g_canon[i] = (uint8_t)i;
}

static void mf_remove_if_group(int only_group) {
    int w = 0;
    for (int r = 0; r < g_mf_n; r++) {
        int kill = (only_group < 0) || g_mf[r].group == 0 || g_mf[r].group == (uint8_t)only_group;
        if (!kill) g_mf[w++] = g_mf[r];
    }
    g_mf_n = w;
}

static void mf_forget_temp(uint8_t t) {
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

static int mf_is_load(uint8_t op) {
    return op == OP_DEREF || op == OP_LOAD_BYTE || op == OP_LOAD_16 || op == OP_LOAD_32;
}

static void opt_restrict_memory_forward(void) {
    int count = (int)ir_get_count();
    mf_reset();

    for (int i = 0; i < count; i++) {
        Instr *in = ir_get(i);
        if (!in) continue;

        switch (in->op) {
            /* Block boundaries / unknown memory effects: forget everything */
            case OP_LABEL: case OP_JMP: case OP_JMP_LABEL: case OP_JE: case OP_JNE:
            case OP_JL: case OP_JLE: case OP_JG: case OP_JCOND:
            case OP_CALL: case OP_CALL_INDIRECT: case OP_RET: case OP_EPILOGUE:
            case OP_VLA_ALLOC: case OP_PUSH: case OP_POP:
            case OP_PARAM_SAVE_INT: case OP_PARAM_SAVE_FP: case OP_VARIADIC_SPILL:
            case OP_CALLEE_SAVE_STORES: case OP_CALLEE_SAVE_LOADS:
            case OP_ATOMIC_XADD: case OP_ATOMIC_CMPXCHG: case OP_ATOMIC_CMPXCHG_BOOL:
            case OP_ATOMIC_XCHG: case OP_MFENCE:
            case OP_FSTORE: case OP_VSTORE: case OP_VSTORESS: case OP_VSTORESD:
                mf_reset();
                continue;
            default:
                break;
        }

        /* Redefinition of a temp invalidates anything that referenced it */
        if (in->temp_dest != 0xFF) {
            mf_forget_temp(in->temp_dest);
            g_canon[in->temp_dest] = in->temp_dest;
        }

        if (in->op == OP_MOV && in->temp_src1 == 0xFF && in->temp_dest != 0xFF) {
            /* Stack slot load: same unmodified slot => same value => same canonical temp */
            int found = -1;
            for (int s = 0; s < g_slot_n; s++) {
                if (g_slot_off[s] == in->imm) { found = s; break; }
            }
            if (found >= 0) {
                g_canon[in->temp_dest] = g_slot_can[found];
            } else if (g_slot_n < MF_MAX_SLOTS) {
                g_slot_off[g_slot_n] = in->imm;
                g_slot_can[g_slot_n] = in->temp_dest;
                g_slot_n++;
            }
            continue;
        }

        if (in->op == OP_MOV_MEM) {
            mf_invalidate_slots_near(in->imm);
            continue;
        }

        if (in->op == OP_MOV_REG && in->temp_dest != 0xFF && in->temp_src1 != 0xFF) {
            g_canon[in->temp_dest] = g_canon[in->temp_src1];
            continue;
        }

        if (mf_is_load(in->op) && in->temp_dest != 0xFF && in->temp_src1 != 0xFF) {
            uint8_t addr = g_canon[in->temp_src1];
            int hit = -1;
            for (int e = 0; e < g_mf_n; e++) {
                if (g_mf[e].op == in->op && g_mf[e].addr == addr) { hit = e; break; }
            }
            if (hit >= 0) {
                uint8_t val = g_mf[hit].val;
                in->op = OP_MOV_REG;
                in->temp_src1 = val;
                in->temp_src2 = 0xFF;
                in->imm = 0;
                in->imm64 = 0;
                in->label_idx = 0;
                in->alias_group = 0;
                g_canon[in->temp_dest] = g_canon[val];
                opt_profile_count_gvn_eliminated();
            } else if (g_mf_n < MF_MAX_ENTRIES) {
                g_mf[g_mf_n].op = in->op;
                g_mf[g_mf_n].addr = addr;
                g_mf[g_mf_n].val = in->temp_dest;
                g_mf[g_mf_n].group = in->alias_group;
                g_mf_n++;
            }
            continue;
        }

        if (in->op == OP_STORE_BYTE || in->op == OP_STORE_16 ||
            in->op == OP_STORE_32 || in->op == OP_STORE_64) {
            if (in->alias_group == 0) {
                /* Untagged store: may alias anything, including stack slots */
                g_mf_n = 0;
                g_slot_n = 0;
            } else {
                /* restrict store: only same-group / untagged entries can conflict */
                mf_remove_if_group(in->alias_group);
            }
            /* A 64-bit store forwards its value to later 64-bit loads of the same address */
            if (in->op == OP_STORE_64 && in->temp_src1 != 0xFF && in->temp_src2 != 0xFF &&
                g_mf_n < MF_MAX_ENTRIES) {
                g_mf[g_mf_n].op = OP_DEREF;
                g_mf[g_mf_n].addr = g_canon[in->temp_src2];
                g_mf[g_mf_n].val = in->temp_src1;
                g_mf[g_mf_n].group = in->alias_group;
                g_mf_n++;
            }
            continue;
        }
    }
}

/**
 * Global Value Numbering pass - basic block scoped
 */
void opt_eliminate_gvn(ZeroCCCompiler *c) {
    (void)c;
    
    int count = (int)ir_get_count();
    
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        opt_profile_count_gvn_instruction();
        
        /* Reset at basic block boundaries */
        if (instr->op == OP_LABEL) {
            gvn_reset_block();
            continue;
        }
        
        /* Skip non-numberable instructions */
        if (!instr_is_numberable(instr)) {
            continue;
        }
        
        /* Skip if destination is 0xFF or 0 */
        if (instr->temp_dest == 0xFF || instr->temp_dest == 0) {
            continue;
        }
        
        /* Check if we've seen this computation before */
        int existing = gvn_find(instr);
        
        if (existing >= 0 && g_temp_has_value[g_values[existing].result_temp]) {
            /* Found duplicate - replace with copy */
            replace_with_gvn_copy(i, g_values[existing].result_temp);
            opt_profile_count_gvn_eliminated();
            
            /* Update: this temp now holds the same value as the existing one */
            g_temp_has_value[instr->temp_dest] = 1;
            g_temp_value_num[instr->temp_dest] = (uint8_t)existing;
        } else {
            /* New computation - record it */
            if (g_value_count < 4096) {
                int val_num = g_value_count++;
                gvn_add(instr, val_num);
                g_temp_has_value[instr->temp_dest] = 1;
                g_temp_value_num[instr->temp_dest] = (uint8_t)val_num;
            }
        }
    }

    /* restrict-aware redundant load elimination / store forwarding */
    opt_restrict_memory_forward();
}
