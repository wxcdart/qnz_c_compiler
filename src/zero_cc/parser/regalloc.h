#ifndef ZERO_CC_PARSER_REGALLOC_H
#define ZERO_CC_PARSER_REGALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration to avoid circular includes */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/*
 * Linear-Scan Register Allocator
 * 
 * Implements a simple linear-scan register allocation algorithm for x86-64.
 * Supports both general-purpose registers (GPR) and XMM registers for floats.
 * 
 * Algorithm:
 * 1. Compute live intervals for each temporary
 * 2. Sort intervals by start position
 * 3. Allocate registers greedily, spilling when necessary
 */

/* Register classes */
typedef enum RegClass {
    REG_CLASS_GPR,    /* General-purpose registers: RAX, RCX, RDX, RBX, RSI, RDI, R8-R15 */
    REG_CLASS_XMM,    /* XMM registers for SSE: XMM0-XMM7 */
    REG_CLASS_COUNT
} RegClass;

/* Available registers per class (System V AMD64 ABI) */
static const char *gpr_registers[] __attribute__((unused)) = {
    "rax", "rcx", "rdx", "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};
#define GPR_COUNT 14

static const char *xmm_registers[] __attribute__((unused)) = {
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"
};
#define XMM_COUNT 8

/* Live interval for a temporary variable */
typedef struct LiveInterval {
    int temp_id;              /* Temporary variable identifier */
    size_t start;             /* Start position (instruction index) */
    size_t end;               /* End position (instruction index) */
    RegClass reg_class;       /* Required register class */
    int assigned_reg;         /* Assigned register index (-1 if spilled) */
    int spill_slot;           /* Spill slot index (-1 if not spilled) */
    bool is_spilled;          /* Whether this interval is spilled */
} LiveInterval;

/* Register allocator state */
typedef struct RegAllocator {
    LiveInterval *intervals;      /* Array of live intervals */
    size_t interval_count;        /* Number of intervals */
    size_t interval_capacity;     /* Capacity of intervals array */
    
    /* Active intervals (sorted by end position) */
    LiveInterval **active;
    size_t active_count;
    size_t active_capacity;
    
    /* Free registers per class */
    bool gpr_free[GPR_COUNT];
    bool xmm_free[XMM_COUNT];
    
    /* Spill slot allocation */
    int next_spill_slot;
    
    /* Instruction count for interval tracking */
    size_t instruction_count;
} RegAllocator;

/* Initialize register allocator */
static void regalloc_init(RegAllocator *ra) __attribute__((unused));
static void regalloc_init(RegAllocator *ra) {
    if (!ra) return;
    
    
    /* Free old intervals */
    free(ra->intervals);
    free(ra->active);
    
    ra->intervals = NULL;
    ra->interval_count = 0;
    ra->interval_capacity = 0;
    
    ra->active = NULL;
    ra->active_count = 0;
    ra->active_capacity = 0;
    
    ra->next_spill_slot = 0;
    ra->instruction_count = 0;
    
    /* Initialize all registers as free */
    for (int i = 0; i < GPR_COUNT; i++) ra->gpr_free[i] = true;
    for (int i = 0; i < XMM_COUNT; i++) ra->xmm_free[i] = true;
}

/* Add a new live interval */
static int regalloc_add_interval(RegAllocator *ra, int temp_id, size_t start, size_t end, RegClass reg_class) __attribute__((unused));
static int regalloc_add_interval(RegAllocator *ra, int temp_id, size_t start, size_t end, RegClass reg_class) {
    if (!ra) return -1;
    
    
    if (ra->interval_count >= ra->interval_capacity) {
        size_t new_cap = ra->interval_capacity ? ra->interval_capacity * 2 : 64;
        LiveInterval *new_intervals = realloc(ra->intervals, new_cap * sizeof(LiveInterval));
        if (!new_intervals) return -1;
        ra->intervals = new_intervals;
        ra->interval_capacity = new_cap;
    }
    
    LiveInterval *interval = &ra->intervals[ra->interval_count++];
    interval->temp_id = temp_id;
    interval->start = start;
    interval->end = end;
    interval->reg_class = reg_class;
    interval->assigned_reg = -1;
    interval->spill_slot = -1;
    interval->is_spilled = false;
    
    return (int)(ra->interval_count - 1);
}

/* Comparison function for sorting intervals by start position */
static int compare_intervals_by_start(const void *a, const void *b) __attribute__((unused));
static int compare_intervals_by_start(const void *a, const void *b) {
    const LiveInterval *ia = *(const LiveInterval **)a;
    const LiveInterval *ib = *(const LiveInterval **)b;
    if (ia->start < ib->start) return -1;
    if (ia->start > ib->start) return 1;
    return 0;
}

/* Expire old intervals (those that end before current position) */
static void expire_old_intervals(RegAllocator *ra, size_t current_pos) __attribute__((unused));
static void expire_old_intervals(RegAllocator *ra, size_t current_pos) {
    if (!ra) return;
    
    for (size_t i = 0; i < ra->active_count; ) {
        LiveInterval *active = ra->active[i];
        if (active->end <= current_pos) {
            /* Free the register */
            if (active->assigned_reg >= 0) {
                if (active->reg_class == REG_CLASS_GPR) {
                    ra->gpr_free[active->assigned_reg] = true;
                } else {
                    ra->xmm_free[active->assigned_reg] = true;
                }
            }
            /* Remove from active list */
            ra->active[i] = ra->active[ra->active_count - 1];
            ra->active_count--;
        } else {
            i++;
        }
    }
}

/* Allocate a register for the given interval */
static int alloc_reg(RegAllocator *ra, LiveInterval *interval) __attribute__((unused));
static int alloc_reg(RegAllocator *ra, LiveInterval *interval) {
    if (!ra || !interval) return -1;
    
    /* Find a free register in the appropriate class */
    if (interval->reg_class == REG_CLASS_GPR) {
        for (int i = 0; i < GPR_COUNT; i++) {
            if (ra->gpr_free[i]) {
                ra->gpr_free[i] = false;
                return i;
            }
        }
    } else {
        for (int i = 0; i < XMM_COUNT; i++) {
            if (ra->xmm_free[i]) {
                ra->xmm_free[i] = false;
                return i;
            }
        }
    }
    
    return -1; /* No free register */
}

/* Spill an interval to memory */
static void spill_interval(RegAllocator *ra, LiveInterval *interval) __attribute__((unused));
static void spill_interval(RegAllocator *ra, LiveInterval *interval) {
    if (!ra || !interval) return;
    
    interval->spill_slot = ra->next_spill_slot++;
    interval->is_spilled = true;
}

/* Perform linear-scan register allocation */
static void regalloc_allocate(RegAllocator *ra) __attribute__((unused));
static void regalloc_allocate(RegAllocator *ra) {
    if (!ra || ra->interval_count == 0) return;
    
    
    /* Sort intervals by start position */
    LiveInterval **sorted = malloc(ra->interval_count * sizeof(LiveInterval *));
    if (!sorted) return;
    
    for (size_t i = 0; i < ra->interval_count; i++) {
        sorted[i] = &ra->intervals[i];
    }
    
    qsort(sorted, ra->interval_count, sizeof(LiveInterval *), compare_intervals_by_start);
    
    /* Allocate active array */
    ra->active = malloc(ra->interval_count * sizeof(LiveInterval *));
    if (!ra->active) {
        free(sorted);
        return;
    }
    ra->active_capacity = ra->interval_count;
    
    /* Process each interval */
    for (size_t i = 0; i < ra->interval_count; i++) {
        LiveInterval *current = sorted[i];
        
        /* Expire old intervals */
        expire_old_intervals(ra, current->start);
        
        /* Try to allocate a register */
        int reg = alloc_reg(ra, current);
        
        if (reg >= 0) {
            /* Successfully allocated a register */
            current->assigned_reg = reg;
            
            /* Add to active list */
            ra->active[ra->active_count++] = current;
            
            /* Keep active list sorted by end position */
            for (size_t j = ra->active_count - 1; j > 0; j--) {
                if (ra->active[j]->end < ra->active[j-1]->end) {
                    LiveInterval *tmp = ra->active[j];
                    ra->active[j] = ra->active[j-1];
                    ra->active[j-1] = tmp;
                } else {
                    break;
                }
            }
        } else {
            /* No free register - need to spill */
            /* Spill the interval with the furthest end point */
            LiveInterval *spill = current;
            if (ra->active_count > 0 && ra->active[ra->active_count - 1]->end > current->end) {
                spill = ra->active[ra->active_count - 1];
            }
            
            if (spill == current) {
                /* Spill current interval */
                spill_interval(ra, current);
            } else {
                /* Spill the active interval and give its register to current */
                spill_interval(ra, spill);
                current->assigned_reg = spill->assigned_reg;
                spill->assigned_reg = -1;
                
                /* Remove spilled interval from active list */
                for (size_t j = 0; j < ra->active_count; j++) {
                    if (ra->active[j] == spill) {
                        ra->active[j] = ra->active[ra->active_count - 1];
                        ra->active_count--;
                        break;
                    }
                }
                
                /* Add current to active list */
                ra->active[ra->active_count++] = current;
            }
        }
    }
    
    free(sorted);
}

#endif /* ZERO_CC_PARSER_REGALLOC_H */
