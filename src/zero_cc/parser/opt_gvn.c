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
}
