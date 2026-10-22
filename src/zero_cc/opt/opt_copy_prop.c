#include "zero_cc/opt/opt_copy_prop.h"
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/ir/ir.h"
#include <stdlib.h>
#include <string.h>

/**
 * Copy Propagation Implementation (Safe)
 * 
 * This implementation tracks copies only for temps that are:
 * 1. Not the return register (temp 0)
 * 2. Not set by flag-setting instructions (CMP, TEST, etc.)
 * 3. Not used as sources for flag-setting instructions
 * 
 * This is conservative but safe - it won't break ABI.
 */

/* Copy map: copy_map[temp] = source_temp (TEMP_NONE if no copy).
 * C2b: temp ids are now 16-bit, but this map stays 256 entries - temps
 * >= 256 simply are not copy-tracked (missed optimization only, never a
 * correctness issue; widening to 64K would make invalidation O(64K) per
 * flag-setting instruction). */
#define COPY_MAP_SIZE 256
static uint16_t g_copy_map[COPY_MAP_SIZE];

/* Initialize copy propagation */
void opt_init_copy_prop(void) {
    /* Nothing to initialize */
}

/* Shutdown copy propagation */
void opt_shutdown_copy_prop(void) {
    /* Nothing to shutdown */
}

/**
 * Reset copy map for a new basic block
 */
static void copy_prop_reset_block(void) {
    memset(g_copy_map, TEMP_NONE, sizeof(g_copy_map));
}

/**
 * Check if temp is a reserved physical register (temps 0..15)
 */
static int copy_prop_is_return_reg(uint16_t temp) {
    return temp < 16;
}

/**
 * Get the ultimate source of a temp (follow copy chain)
 * Returns the original source temp
 */
static uint16_t copy_prop_get_source(uint16_t temp) {
    if (temp == TEMP_NONE || temp < 16 || temp >= COPY_MAP_SIZE) {
        return temp;
    }
    
    /* Follow copy chain */
    int max_depth = COPY_MAP_SIZE;  /* Prevent infinite loops */
    while (g_copy_map[temp] != TEMP_NONE && max_depth-- > 0) {
        temp = g_copy_map[temp];
    }
    
    return temp;
}

/**
 * Invalidate copy for a temp and anything that copies it
 */
static void copy_prop_invalidate(uint16_t temp) {
    if (temp == TEMP_NONE || temp < 16 || temp >= COPY_MAP_SIZE) {
        return;
    }
    
    /* Invalidate this temp */
    g_copy_map[temp] = TEMP_NONE;
    
    /* Invalidate anything that copies from this temp */
    for (int i = 0; i < COPY_MAP_SIZE; i++) {
        if (g_copy_map[i] == temp) {
            g_copy_map[i] = TEMP_NONE;
        }
    }
}

/**
 * Check if instruction defines a temp (is a destination)
 *
 * Generic rule (matches DCE's liveness convention): any instruction with
 * a temp_dest set defines that temp.  OP_CALL/OP_CALL_INDIRECT are
 * excluded - since C2b their temp_dest is not a temp ID (call arguments
 * live in IRCallArgs; historically dest held arg_count).
 *
 * The previous explicit opcode list missed the in-place ops (ADD_IMM,
 * SUB_INC, INC, DEC, SEXT*, ZEXT*, LEA, ...), so an in-place redefine of
 * a temp that had been copy-mapped was not invalidated, and later uses
 * of the temp were replaced with the stale copy source (Sprint 39:
 * exposed by the branchless-select pass, which emits SUB_INC on a copied
 * temp).
 */
static int instr_defines_temp(Instr *instr) {
    if (instr->op == OP_CALL || instr->op == OP_CALL_INDIRECT) return 0;
    return instr->temp_dest != TEMP_NONE;
}

/**
 * Check if instruction uses flags (reads or writes)
 */
static int instr_uses_flags(Instr *instr) {
    switch (instr->op) {
        /* Writes flags */
        case OP_ADD:
        case OP_SUB:
        case OP_CMP:
        case OP_CMP_ZERO:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_SHL:
        case OP_SHR:
        case OP_SAR:
        case OP_NEG:
        /* Reads flags */
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_JG:
        case OP_JCOND:
        case OP_COND_SET:
            return 1;
        default:
            return 0;
    }
}

/**
 * Replace temps in instruction with their copy sources
 * Returns true if any changes were made
 */
static int copy_prop_replace(Instr *instr) {
    int changed = 0;
    
    /* Replace first source */
    if (instr->temp_src1 != TEMP_NONE && instr->temp_src1 >= 16 && instr->temp_src1 < COPY_MAP_SIZE) {
        uint16_t new_src1 = copy_prop_get_source(instr->temp_src1);
        if (new_src1 != instr->temp_src1 && new_src1 != TEMP_NONE && new_src1 >= 16) {
            instr->temp_src1 = new_src1;
            changed = 1;
        }
    }
    
    /* Replace second source */
    if (instr->temp_src2 != TEMP_NONE && instr->temp_src2 >= 16 && instr->temp_src2 < COPY_MAP_SIZE) {
        uint16_t new_src2 = copy_prop_get_source(instr->temp_src2);
        if (new_src2 != instr->temp_src2 && new_src2 != TEMP_NONE && new_src2 >= 16) {
            instr->temp_src2 = new_src2;
            changed = 1;
        }
    }
    
    return changed;
}

/**
 * Copy propagation pass
 */
void opt_copy_propagate(ZeroCCCompiler *c) {
    (void)c;
    
    int count = (int)ir_get_count();
    int total_changes = 0;
    
    /* Reset at the start */
    copy_prop_reset_block();
    
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        opt_profile_count_copy_prop_instruction();
        
        /* Reset at basic block boundaries */
        if (instr->op == OP_LABEL) {
            copy_prop_reset_block();
            continue;
        }
        
        /* Skip instructions that use flags */
        if (instr_uses_flags(instr)) {
            /* Invalidate all copies - flags may have changed */
            copy_prop_reset_block();
            continue;
        }
        
        /* First, replace source temps with their copy sources */
        int changed = copy_prop_replace(instr);
        if (changed) {
            opt_profile_count_copy_prop_changed();
            total_changes++;
        }
        
        /* Then, handle the definition */
        if (instr_defines_temp(instr)) {
            /* Don't track copies for physical registers */
            if (copy_prop_is_return_reg(instr->temp_dest)) {
                continue;
            }
            
            /* Invalidate the destination temp */
            copy_prop_invalidate(instr->temp_dest);
            
            /* If this is a MOV between temps, record the copy */
            if (instr->op == OP_MOV_REG) {
                uint16_t src = copy_prop_get_source(instr->temp_src1);
                /* Only record if source is a valid virtual temp in the map */
                if (src != instr->temp_dest && src != TEMP_NONE && src >= 16 &&
                    instr->temp_dest < COPY_MAP_SIZE) {
                    g_copy_map[instr->temp_dest] = src;
                }
            }
        }
    }
    
    opt_profile_set_copy_prop_changes(total_changes);
}
