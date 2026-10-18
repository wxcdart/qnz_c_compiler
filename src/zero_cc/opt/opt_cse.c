#include "opt_cse.h"
#include "opt_profile.h"
#include "../ir/ir.h"
#include "../ir/regalloc.h"
#include "zero_cc/common/hash_table.h"
#include <stdlib.h>
#include <string.h>

/**
 * CSE state
 */
static HashTable32 *g_cse_table;
static int g_cse_bb_start;  /* Current basic block start index */

/**
 * Initialize CSE system
 */
static void cse_init(void) {
    g_cse_table = malloc(sizeof(HashTable32));
    ht32_init(g_cse_table, 4096);
    g_cse_bb_start = 0;
}

/**
 * Clear CSE table for new basic block
 * Note: TPHT is immutable, so we need to free and reinitialize
 */
static void cse_clear_block(void) {
    if (g_cse_table) {
        ht32_free(g_cse_table);
        free(g_cse_table);
        g_cse_table = NULL;
    }
    g_cse_table = malloc(sizeof(HashTable32));
    if (g_cse_table) {
        ht32_init(g_cse_table, 4096);
    }
    g_cse_bb_start = 0;
}

/**
 * Shutdown CSE system
 */
static void cse_shutdown(void) {
    if (g_cse_table) {
        ht32_free(g_cse_table);
        free(g_cse_table);
        g_cse_table = NULL;
    }
}

/**
 * Check if instruction is a pure expression (no side effects)
 */
static int instr_is_pure(Instr *instr) {
    switch (instr->op) {
        // Pure arithmetic/logic operations
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_SHL:
        case OP_SHR:
        case OP_SAR:
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
 * Encodes op + src1 + src2 + imm into a 32-bit key
 */
static uint32_t cse_key(Instr *instr) {
    // Use a simple hash combining the instruction fields
    uint32_t key = instr->op;
    key = key * 31 + instr->temp_src1;
    key = key * 31 + instr->temp_src2;
    key = key * 31 + (uint32_t)(instr->imm & 0xFFFF);
    return key;
}

/**
 * Check if two instructions compute the same expression
 */
static int cse_equal(Instr *a, Instr *b) {
    if (a->op != b->op) return 0;
    if (a->temp_src1 != b->temp_src1) return 0;
    if (a->temp_src2 != b->temp_src2) return 0;
    if (a->imm != b->imm) return 0;
    return 1;
}

/**
 * Find existing computation of same expression
 * Returns the result temp if found, -1 otherwise
 */
static int cse_find(Instr *instr) {
    uint32_t key = cse_key(instr);
    uint32_t existing_key;
    
    /* Search for exact match */
    for (size_t i = 0; i < g_cse_table->capacity; i++) {
        size_t slot = (key + i) % g_cse_table->capacity;
        existing_key = g_cse_table->keys[slot];
        
        if (existing_key == UINT32_MAX) {
            /* Empty slot - not found */
            return -1;
        }
        
        /* The value stores the instruction index */
        int existing_idx = (int)g_cse_table->vals[slot];
        Instr *existing = ir_get(existing_idx);
        if (existing && cse_equal(instr, existing)) {
            return existing->temp_dest;  /* Return result temp */
        }
    }
    
    return -1;
}

/**
 * Add instruction to CSE table
 */
static void cse_add(Instr *instr, int idx) {
    uint32_t key = cse_key(instr);
    
    /* Store instruction index as the value */
    /* When we find a match, we look up the instruction to get its result temp */
    ht32_put(g_cse_table, key, (uint32_t)idx);
}

/**
 * Replace instruction with MOV from existing result
 */
static void replace_with_cse_copy(int instr_idx, int src_temp) {
    Instr *instr = ir_get(instr_idx);
    if (!instr) return;
    
    instr->op = OP_MOV_REG;
    instr->temp_src1 = src_temp;
    instr->temp_src2= TEMP_NONE;
    instr->imm = 0;
    instr->label_idx = 0;
}

/**
 * Common Subexpression Elimination pass
 */
void opt_eliminate_cse(ZeroCCCompiler *c) {
    (void)c;
    
    cse_init();
    
    int count = (int)ir_get_count();
    
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        /* Profile: count basic blocks */
        if (instr->op == OP_LABEL) {
            cse_clear_block();
            g_cse_bb_start = i;
            opt_profile_count_cse_basic_block();
            continue;
        }
        
        /* Profile: count instructions */
        opt_profile_count_cse_instruction();
        
        /* Skip non-pure operations */
        if (!instr_is_pure(instr)) {
            continue;
        }
        
        /* Skip if destination or source is a reserved physical register (< 16) or invalid (0xFF) */
        if (instr->temp_dest== TEMP_NONE || instr->temp_dest < 16 ||
            (instr->temp_src1!= TEMP_NONE && instr->temp_src1 < 16) ||
            (instr->temp_src2!= TEMP_NONE && instr->temp_src2 < 16)) {
            continue;
        }
        
        /* Check if we've seen this expression before */
        int existing = cse_find(instr);
        
        if (existing >= 0) {
            /* Found duplicate - replace with copy */
            replace_with_cse_copy(i, existing);
            opt_profile_count_cse_eliminated();
        } else {
            /* First time - add to table */
            cse_add(instr, i);
        }
    }
    
    cse_shutdown();
}
