#ifndef ZERO_CC_PARSER_LIVE_RANGE_H
#define ZERO_CC_PARSER_LIVE_RANGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"
#include "regalloc.h"

/*
 * Live Range Analysis for Two-Pass Code Generation
 * 
 * Computes live intervals for temporary variables from IR instructions.
 * Used by the linear-scan register allocator to assign physical registers.
 */

/* Track definition and use of temporaries */
typedef struct TempUse {
    size_t instr_index;     /* Instruction index where temp is used/defined */
    int temp_id;            /* Temporary variable ID */
    bool is_def;            /* true = definition, false = use */
} TempUse;

/* Live range analysis state */
typedef struct LiveRangeAnalyzer {
    TempUse *uses;              /* Array of temp uses/defs */
    size_t use_count;           /* Number of uses */
    size_t use_capacity;        /* Capacity of uses array */
    
    /* Live intervals per temp */
    size_t *temp_def_pos;       /* Definition position per temp */
    size_t *temp_live_end;      /* Last use position per temp */
    size_t temp_capacity;       /* Capacity of temp arrays */
} LiveRangeAnalyzer;

/* Initialize live range analyzer */
static void live_range_init(LiveRangeAnalyzer *lra) __attribute__((unused));
static void live_range_init(LiveRangeAnalyzer *lra) {
    if (!lra) return;
    
    lra->uses = NULL;
    lra->use_count = 0;
    lra->use_capacity = 0;
    
    lra->temp_def_pos = NULL;
    lra->temp_live_end = NULL;
    lra->temp_capacity = 0;
}

/* Free live range analyzer */
static void live_range_free(LiveRangeAnalyzer *lra) __attribute__((unused));
static void live_range_free(LiveRangeAnalyzer *lra) {
    if (!lra) return;
    
    free(lra->uses);
    free(lra->temp_def_pos);
    free(lra->temp_live_end);
    
    lra->uses = NULL;
    lra->temp_def_pos = NULL;
    lra->temp_live_end = NULL;
}

/* Add a temp use/def */
static void live_range_add_use(LiveRangeAnalyzer *lra, size_t instr_index, int temp_id, bool is_def) __attribute__((unused));
static void live_range_add_use(LiveRangeAnalyzer *lra, size_t instr_index, int temp_id, bool is_def) {
    if (!lra || temp_id < 0 || temp_id >= 256) return;
    
    /* Expand uses array if needed */
    if (lra->use_count >= lra->use_capacity) {
        size_t new_cap = lra->use_capacity ? lra->use_capacity * 2 : 64;
        TempUse *new_uses = realloc(lra->uses, new_cap * sizeof(TempUse));
        if (!new_uses) return;
        lra->uses = new_uses;
        lra->use_capacity = new_cap;
    }
    
    TempUse *use = &lra->uses[lra->use_count++];
    use->instr_index = instr_index;
    use->temp_id = temp_id;
    use->is_def = is_def;
    
    /* Expand temp arrays if needed */
    if ((size_t)temp_id >= lra->temp_capacity) {
        size_t new_cap = lra->temp_capacity ? lra->temp_capacity * 2 : 256;
        size_t new_temp = (size_t)temp_id + 1;
        if (new_cap < new_temp) new_cap = new_temp;
        
        size_t *new_def_pos = realloc(lra->temp_def_pos, new_cap * sizeof(size_t));
        if (!new_def_pos) return;
        
        size_t *new_live_end = realloc(lra->temp_live_end, new_cap * sizeof(size_t));
        if (!new_live_end) {
            free(new_def_pos);
            return;
        }
        
        /* Initialize new entries */
        for (size_t i = lra->temp_capacity; i < new_cap; i++) {
            new_def_pos[i] = SIZE_MAX;  /* Undefined */
            new_live_end[i] = 0;
        }
        
        lra->temp_def_pos = new_def_pos;
        lra->temp_live_end = new_live_end;
        lra->temp_capacity = new_cap;
    }
    
    /* Track definition position (first def) */
    if (is_def && lra->temp_def_pos[temp_id] == SIZE_MAX) {
        lra->temp_def_pos[temp_id] = instr_index;
    }
    
    /* Update live end (last use) */
    if (instr_index > lra->temp_live_end[temp_id]) {
        lra->temp_live_end[temp_id] = instr_index;
    }
}

/* Compute live intervals from IR and add to register allocator */
static void live_range_compute(LiveRangeAnalyzer *lra, RegAllocator *ra) __attribute__((unused));
static void live_range_compute(LiveRangeAnalyzer *lra, RegAllocator *ra) {
    if (!lra || !ra) return;
    
    
    /* Process each temp that was defined */
    for (size_t temp_id = 0; temp_id < lra->temp_capacity; temp_id++) {
        if (lra->temp_def_pos[temp_id] == SIZE_MAX) {
            continue;  /* Temp was never defined */
        }
        
        size_t start = lra->temp_def_pos[temp_id];
        size_t end = lra->temp_live_end[temp_id];
        
        
        /* Skip if start > end (shouldn't happen, but be safe) */
        if (start > end) continue;
        
        /* Determine register class based on usage pattern */
        /* For now, assume all temps are GPR (we'll refine this later) */
        RegClass reg_class = REG_CLASS_GPR;
        
        /* Add interval to register allocator */
        regalloc_add_interval(ra, (int)temp_id, start, end, reg_class);
    }
}

/* Scan IR and record temp uses/defs */
static void live_range_scan_ir(LiveRangeAnalyzer *lra, const Instr *instrs, size_t instr_count) __attribute__((unused));
static void live_range_scan_ir(LiveRangeAnalyzer *lra, const Instr *instrs, size_t instr_count) {
    if (!lra || !instrs) return;
    
    for (size_t i = 0; i < instr_count; i++) {
        const Instr *instr = &instrs[i];
        
        /* Skip labels and jumps (they don't use temps) */
        if (instr->op == OP_LABEL || instr->op == OP_JMP || 
            instr->op == OP_JE || instr->op == OP_JNE ||
            instr->op == OP_JL || instr->op == OP_JLE ||
            instr->op == OP_JG) {
            continue;
        }
        
        /* Record destination temp (definition) - skip 0xFF (no temp) */
        if (instr->temp_dest != 0xFF) {
            live_range_add_use(lra, i, instr->temp_dest, true);
        }
        
        /* Record source temps (uses) - skip 0xFF (no temp) */
        if (instr->temp_src1 != 0xFF) {
            live_range_add_use(lra, i, instr->temp_src1, false);
        }
        if (instr->temp_src2 != 0xFF) {
            live_range_add_use(lra, i, instr->temp_src2, false);
        }
    }
}

/* Get register name for a register index and class */
static const char *live_range_get_reg_name(RegClass reg_class, int reg_index) __attribute__((unused));
static const char *live_range_get_reg_name(RegClass reg_class, int reg_index) {
    static const char *gpr_names[] = {"rax", "rcx", "rdx", "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    static const char *xmm_names[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};
    
    if (reg_class == REG_CLASS_GPR && reg_index >= 0 && reg_index < 14) {
        return gpr_names[reg_index];
    } else if (reg_class == REG_CLASS_XMM && reg_index >= 0 && reg_index < 8) {
        return xmm_names[reg_index];
    }
    return "spilled";
}

#endif /* ZERO_CC_PARSER_LIVE_RANGE_H */


