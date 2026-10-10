#include "opt_copy_prop.h"
#include "opt_profile.h"
#include "ir.h"
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

/* Copy map: copy_map[temp] = source_temp (0xFF if no copy) */
static uint8_t g_copy_map[256];

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
    memset(g_copy_map, 0xFF, sizeof(g_copy_map));
}

/**
 * Check if temp is the return register (temp 0)
 */
static int copy_prop_is_return_reg(uint8_t temp) {
    return temp == 0;
}

/**
 * Get the ultimate source of a temp (follow copy chain)
 * Returns the original source temp
 */
static uint8_t copy_prop_get_source(uint8_t temp) {
    if (temp == 0xFF || temp == 0) {
        return temp;
    }
    
    /* Follow copy chain */
    int max_depth = 256;  /* Prevent infinite loops */
    while (g_copy_map[temp] != 0xFF && max_depth-- > 0) {
        temp = g_copy_map[temp];
    }
    
    return temp;
}

/**
 * Invalidate copy for a temp and anything that copies it
 */
static void copy_prop_invalidate(uint8_t temp) {
    if (temp == 0xFF || temp == 0) {
        return;
    }
    
    /* Invalidate this temp */
    g_copy_map[temp] = 0xFF;
    
    /* Invalidate anything that copies from this temp */
    for (int i = 0; i < 256; i++) {
        if (g_copy_map[i] == temp) {
            g_copy_map[i] = 0xFF;
        }
    }
}

/**
 * Check if instruction defines a temp (is a destination)
 */
static int instr_defines_temp(Instr *instr) {
    switch (instr->op) {
        case OP_MOV_REG:
        case OP_MOV_IMM:
        case OP_MOV_MEM:
        case OP_MOV:
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_NOT:
        case OP_NEG:
        case OP_SHL:
        case OP_SHR:
        case OP_SAR:
        case OP_LOAD_BYTE:
        case OP_LOAD_16:
        case OP_LOAD_32:
        case OP_VLOAD:
        case OP_VLOADSS:
        case OP_VLOADSD:
        case OP_VMOV:
        case OP_VMOVSS:
        case OP_VMOVSD:
        case OP_CALL:
        case OP_CALL_INDIRECT:
            return 1;
        default:
            return 0;
    }
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
    if (instr->temp_src1 != 0xFF && instr->temp_src1 != 0) {
        uint8_t new_src1 = copy_prop_get_source(instr->temp_src1);
        if (new_src1 != instr->temp_src1 && new_src1 != 0xFF && new_src1 != 0) {
            instr->temp_src1 = new_src1;
            changed = 1;
        }
    }
    
    /* Replace second source */
    if (instr->temp_src2 != 0xFF && instr->temp_src2 != 0) {
        uint8_t new_src2 = copy_prop_get_source(instr->temp_src2);
        if (new_src2 != instr->temp_src2 && new_src2 != 0xFF && new_src2 != 0) {
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
            /* Don't track copies for return register */
            if (copy_prop_is_return_reg(instr->temp_dest)) {
                continue;
            }
            
            /* Invalidate the destination temp */
            copy_prop_invalidate(instr->temp_dest);
            
            /* If this is a MOV between temps, record the copy */
            if (instr->op == OP_MOV_REG) {
                uint8_t src = copy_prop_get_source(instr->temp_src1);
                /* Only record if source is valid and not return reg */
                if (src != instr->temp_dest && src != 0xFF && src != 0) {
                    g_copy_map[instr->temp_dest] = src;
                }
            }
        }
    }
    
    opt_profile_set_copy_prop_changes(total_changes);
}
