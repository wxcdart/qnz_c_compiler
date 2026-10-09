#ifndef ZERO_CC_PARSER_REGALLOC_H
#define ZERO_CC_PARSER_REGALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"
#include "../isa/isa_backend.h"

#include "../types/vregs.h"

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
    REG_CLASS_VEC,    /* Vector registers (XMM/NEON/RVV): xmm0-xmm15/v0-v31 */
    REG_CLASS_COUNT
} RegClass;

/* Available registers per class (System V AMD64 ABI) */
/* Note: rsp/rbp are reserved for stack, rdx is reserved for struct returns, rcx for shift counts (cl) */
static const char *gpr_registers[] __attribute__((unused)) = {
    "rax", "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};
/* GPR_COUNT: x86-64 uses 12, AArch64 uses 30 (x0-x28, x30), RISC-V uses 32 */
#define GPR_COUNT 32

static const char *xmm_registers[] __attribute__((unused)) = {
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"
};
#define XMM_COUNT 8

/* Vector registers (ISA-specific) */
/* x86-64: 16 XMM registers, AArch64: 32 NEON registers, RISC-V: 32 RVV registers */
static const char *vec_registers_x86[] __attribute__((unused)) = {
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
};
#define VEC_COUNT_X86 16

static const char *vec_registers_aarch64[] __attribute__((unused)) = {
    "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
    "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
    "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
    "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"
};
#define VEC_COUNT_AARCH64 32

static const char *vec_registers_riscv[] __attribute__((unused)) = {
    "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
    "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
    "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
    "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"
};
#define VEC_COUNT_RISCV 32

/* Vector register count macro (ISA-specific) */
#define VEC_COUNT(ra) ((ra)->target_isa == ISA_AARCH64 ? VEC_COUNT_AARCH64 : \
                       (ra)->target_isa == ISA_RISCV64 ? VEC_COUNT_RISCV : VEC_COUNT_X86)

/* Live interval for a temporary variable */
typedef struct LiveInterval {
    int temp_id;              /* Temporary variable identifier */
    size_t start;             /* Start position (instruction index) */
    size_t end;               /* End position (instruction index) */
    RegClass reg_class;       /* Required register class */
    int assigned_reg;         /* Assigned register index (-1 if spilled) */
    int spill_slot;           /* Spill slot index (-1 if not spilled) */
    bool is_spilled;          /* Whether this interval is spilled */
    bool spans_call;          /* Whether this interval spans a function call */
    size_t spill_cost;        /* Spill cost (higher = more important to keep in reg) */
} LiveInterval;

/* Register allocator state */
typedef struct RegAllocator {
    LiveInterval *intervals;      /* Array of live intervals */
    size_t interval_count;        /* Number of intervals */
    size_t interval_capacity;     /* Capacity of intervals array */
    
    /* ISA target (for ISA-specific register allocation) */
    ZeroCCISA target_isa;               /* Target ISA: ISA_X86_64, ISA_AARCH64, etc. */
    
    /* Active intervals (sorted by end position) */
    LiveInterval **active;
    size_t active_count;
    size_t active_capacity;
    
    /* Clobbered registers at each instruction position */
    bool *rax_clobbered;          /* True if rax is clobbered at instruction i */
    bool *rdx_clobbered;          /* True if rdx is clobbered at instruction i */
    size_t clobber_capacity;      /* Capacity of clobber arrays */
    
    /* Free registers per class */
    bool gpr_free[GPR_COUNT];
    bool xmm_free[XMM_COUNT];
    bool vec_free[32];  /* Vector registers (max 32 across all ISAs) */
    
    /* Spill slot allocation */
    int next_spill_slot;
    int next_vec_spill_slot;  /* Vector spill slots (16-byte aligned) */
    
    /* Instruction count for interval tracking */
    size_t instruction_count;

    /* Mask of callee-saved GPR allocator indices that were assigned.
     * Callee-saved indices in the pool: 1=rbx, 8=r12, 9=r13, 10=r14, 11=r15
     * Bit N is set if pool index N was assigned to some live interval. */
    uint32_t used_callee_saved_gpr_mask;
    
    /* Cost-based allocation flag */
    bool use_cost_based;  /* If true, use spill cost priority instead of start position */
} RegAllocator;

/* Destroy and free register allocator allocations */
static void regalloc_destroy(RegAllocator *ra) __attribute__((unused));
static void regalloc_destroy(RegAllocator *ra) {
    if (!ra) return;
    free(ra->intervals);
    free(ra->active);
    free(ra->rax_clobbered);
    free(ra->rdx_clobbered);
    ra->intervals = NULL;
    ra->active = NULL;
    ra->rax_clobbered = NULL;
    ra->rdx_clobbered = NULL;
    ra->interval_count = 0;
    ra->interval_capacity = 0;
    ra->active_count = 0;
    ra->active_capacity = 0;
    ra->clobber_capacity = 0;
}

/* Initialize register allocator */
static void regalloc_init(RegAllocator *ra) __attribute__((unused));
static void regalloc_init(RegAllocator *ra) {
    if (!ra) return;
    regalloc_destroy(ra);
    ra->next_spill_slot = 0;
    ra->next_vec_spill_slot = 0;
    ra->instruction_count = 0;
    ra->used_callee_saved_gpr_mask = 0;
    ra->target_isa = ISA_X86_64;  /* Default to x86-64, will be set by caller */
    ra->use_cost_based = true;   /* Enable cost-based allocation for better loop performance */
    for (int i = 0; i < GPR_COUNT; i++) ra->gpr_free[i] = true;
    for (int i = 0; i < XMM_COUNT; i++) ra->xmm_free[i] = true;
    for (int i = 0; i < 32; i++) ra->vec_free[i] = true;
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
    interval->spans_call = false;
    interval->spill_cost = 1;  /* Default spill cost */
    
    return (int)(ra->interval_count - 1);
}

/* Mark an interval as spanning a function call */
static void regalloc_set_interval_spans_call(RegAllocator *ra, int interval_idx, bool spans_call) __attribute__((unused));
static void regalloc_set_interval_spans_call(RegAllocator *ra, int interval_idx, bool spans_call) {
    if (ra && interval_idx >= 0 && (size_t)interval_idx < ra->interval_count) {
        ra->intervals[interval_idx].spans_call = spans_call;
    }
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

/* Comparison function for sorting intervals by spill cost (descending) */
static int compare_intervals_by_cost(const void *a, const void *b) __attribute__((unused));
static int compare_intervals_by_cost(const void *a, const void *b) {
    const LiveInterval *ia = *(const LiveInterval **)a;
    const LiveInterval *ib = *(const LiveInterval **)b;
    
    /* Higher cost = higher priority (sort descending) */
    if (ia->spill_cost > ib->spill_cost) return -1;
    if (ia->spill_cost < ib->spill_cost) return 1;
    
    /* Tie-breaker: longer intervals get higher priority */
    size_t len_a = ia->end - ia->start;
    size_t len_b = ib->end - ib->start;
    if (len_a > len_b) return -1;
    if (len_a < len_b) return 1;
    
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
                } else if (active->reg_class == REG_CLASS_XMM) {
                    ra->xmm_free[active->assigned_reg] = true;
                } else if (active->reg_class == REG_CLASS_VEC) {
                    ra->vec_free[active->assigned_reg] = true;
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
    
    /* Check if we're using AArch64 backend */
    bool is_aarch64 = (ra->target_isa == ISA_AARCH64);
    
    /* Find a free register in the appropriate class */
    if (interval->reg_class == REG_CLASS_GPR) {
        if (interval->spans_call) {
            if (is_aarch64) {
                /* AArch64: prefer callee-saved registers (x19-x28) */
                for (int i = 19; i <= 28; i++) {
                    if (i < GPR_COUNT && ra->gpr_free[i]) {
                        ra->gpr_free[i] = false;
                        return i;
                    }
                }
                /* Callee-saved registers exhausted: fall through to allocate any free GPR */
            } else {
                /* x86-64: prefer callee-saved registers (rbx, r12-r15) */
                /* Pool indices 1,8,9,10,11 map to physical regs 3,12,13,14,15 (rbx, r12-r15) */
                static const int callee_saved_pool_to_phys[] = {3, 12, 13, 14, 15};
                for (size_t k = 0; k < sizeof(callee_saved_pool_to_phys)/sizeof(callee_saved_pool_to_phys[0]); k++) {
                    int phys_reg = callee_saved_pool_to_phys[k];
                    if (ra->gpr_free[phys_reg]) {
                        ra->gpr_free[phys_reg] = false;
                        ra->used_callee_saved_gpr_mask |= (1u << phys_reg);
                        return phys_reg;
                    }
                }
                /* Callee-saved registers exhausted: fall through to allocate any free GPR */
            }
        }

        /* General pool: iterate over pool indices, not physical register numbers */
        static const int pool_to_phys[] = {0, 3, 1, 2, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 4, 5};
        for (int pool_idx = 0; pool_idx < GPR_COUNT; pool_idx++) {
            int phys_reg = pool_to_phys[pool_idx];
            if (ra->gpr_free[phys_reg]) {
                ra->gpr_free[phys_reg] = false;
                /* Track callee-saved register usage (ISA-specific) */
                if (is_aarch64) {
                    /* AArch64: x19-x28 are callee-saved */
                    if (phys_reg >= 19 && phys_reg <= 28) {
                        ra->used_callee_saved_gpr_mask |= (1u << phys_reg);
                    }
                } else {
                    /* x86-64: rbx, r12-r15 are callee-saved */
                    static const uint32_t x86_callee_saved_mask =
                        (1u << 3) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15);
                    if ((x86_callee_saved_mask >> phys_reg) & 1u) {
                        ra->used_callee_saved_gpr_mask |= (1u << phys_reg);
                    }
                }
                return phys_reg;
            }
        }
    } else if (interval->reg_class == REG_CLASS_XMM) {
        for (int i = 0; i < XMM_COUNT; i++) {
            if (ra->xmm_free[i]) {
                ra->xmm_free[i] = false;
                return i;
            }
        }
    } else if (interval->reg_class == REG_CLASS_VEC) {
        /* Vector register allocation (ISA-specific count) */
        int vec_count = VEC_COUNT(ra);
        for (int i = 0; i < vec_count; i++) {
            if (ra->vec_free[i]) {
                ra->vec_free[i] = false;
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
    
    /* Vector spill slots are 16-byte aligned */
    if (interval->reg_class == REG_CLASS_VEC) {
        /* Align to 16 bytes (4 x 32-bit floats or 2 x 64-bit doubles) */
        interval->spill_slot = ra->next_vec_spill_slot;
        ra->next_vec_spill_slot += 16;  /* Each vector spill slot is 16 bytes */
    } else {
        interval->spill_slot = ra->next_spill_slot++;
    }
    interval->is_spilled = true;
}

/* Perform linear-scan register allocation */
static void regalloc_allocate(RegAllocator *ra) __attribute__((unused));
static void regalloc_allocate(RegAllocator *ra) {
    if (!ra || ra->interval_count == 0) {
        return;
    }
    
    /* Always sort by start position for correct expiration handling */
    /* Cost-based decisions are made during spill selection, not initial ordering */
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
        
        /* Try to allocate a register (VREG_0 is pre-colored to pool index 0 = RAX) */
        int reg;
        if (current->temp_id == VREG_0) {
            reg = 0;
            ra->gpr_free[0] = false;
        } else {
            reg = alloc_reg(ra, current);
        }
        
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
            LiveInterval *spill;
            
            if (ra->use_cost_based) {
                /* Cost-based spill decision: spill the interval with lowest cost */
                /* Compare current with all active intervals */
                spill = current;
                
                for (size_t j = 0; j < ra->active_count; j++) {
                    LiveInterval *active = ra->active[j];
                    
                    /* Spill if active has lower cost than current spill candidate */
                    if (active->spill_cost < spill->spill_cost) {
                        spill = active;
                    } else if (active->spill_cost == spill->spill_cost) {
                        /* Tie-breaker: spill the one with furthest end point */
                        if (active->end > spill->end) {
                            spill = active;
                        }
                    }
                }
            } else {
                /* Classic linear-scan: spill the interval with furthest end point */
                spill = current;
                if (ra->active_count > 0 && ra->active[ra->active_count - 1]->end > current->end) {
                    spill = ra->active[ra->active_count - 1];
                }
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

/* =========================================================================
 * Cost-Based Register Allocation Helpers
 * ========================================================================= */

/**
 * Simple loop detection: find backedges in the IR
 * Returns the number of loops detected
 * 
 * Scans for jumps to earlier positions (backedges) to identify loop structure.
 */
static size_t regalloc_detect_loops(RegAllocator *ra, int **loop_headers, int **loop_exits) __attribute__((unused));
static size_t regalloc_detect_loops(RegAllocator *ra, int **loop_headers, int **loop_exits) {
    if (!ra || !loop_headers || !loop_exits || ra->instruction_count == 0) return 0;
    
    /* Allocate arrays for loop info */
    *loop_headers = malloc(ra->instruction_count * sizeof(int));
    *loop_exits = malloc(ra->instruction_count * sizeof(int));
    if (!*loop_headers || !*loop_exits) {
        free(*loop_headers);
        free(*loop_exits);
        *loop_headers = NULL;
        *loop_exits = NULL;
        return 0;
    }
    
    /* Scan for backedges: jumps to earlier positions */
    size_t loop_count = 0;
    
    for (size_t i = 0; i < ra->instruction_count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        /* Check for unconditional jumps */
        if (instr->op == OP_JMP || instr->op == OP_JMP_LABEL) {
            int target = (int)instr->label_idx;
            /* Backedge: jump to an earlier label (simplified: assume label_idx ~ position) */
            /* In reality, we'd need to track label positions, but for now use a heuristic */
            if (target < (int)i && target >= 0) {
                (*loop_headers)[loop_count] = target;
                (*loop_exits)[loop_count] = (int)i;
                loop_count++;
            }
        }
        /* Check for conditional jumps */
        else if (instr->op == OP_JE || instr->op == OP_JNE || instr->op == OP_JL || 
                 instr->op == OP_JLE || instr->op == OP_JG) {
            int target = (int)instr->label_idx;
            if (target < (int)i && target >= 0) {
                (*loop_headers)[loop_count] = target;
                (*loop_exits)[loop_count] = (int)i;
                loop_count++;
            }
        }
    }
    
    return loop_count;
}

/**
 * Compute loop nesting depth for each instruction position
 */
static int *regalloc_compute_loop_depths(RegAllocator *ra, int *loop_headers, int *loop_exits, size_t loop_count) __attribute__((unused));
static int *regalloc_compute_loop_depths(RegAllocator *ra, int *loop_headers, int *loop_exits, size_t loop_count) {
    if (!ra) return NULL;
    
    int *depths = calloc(ra->instruction_count, sizeof(int));
    if (!depths) return NULL;
    
    /* For each instruction, count how many loops contain it */
    for (size_t i = 0; i < ra->instruction_count; i++) {
        int depth = 0;
        for (size_t l = 0; l < loop_count; l++) {
            if (i >= (size_t)loop_headers[l] && i <= (size_t)loop_exits[l]) {
                depth++;
            }
        }
        depths[i] = depth;
    }
    
    return depths;
}

/**
 * Compute spill costs for all intervals based on loop nesting
 * Higher cost = more important to keep in register (e.g., inside loops)
 */
static void regalloc_compute_spill_costs(RegAllocator *ra) __attribute__((unused));
static void regalloc_compute_spill_costs(RegAllocator *ra) {
    if (!ra || ra->interval_count == 0) return;
    
    /* Detect loops */
    int *loop_headers = NULL;
    int *loop_exits = NULL;
    size_t loop_count = regalloc_detect_loops(ra, &loop_headers, &loop_exits);
    
    if (loop_count == 0) {
        /* No loops detected, use default costs */
        for (size_t i = 0; i < ra->interval_count; i++) {
            ra->intervals[i].spill_cost = 1;
        }
        free(loop_headers);
        free(loop_exits);
        return;
    }
    
    /* Compute loop depths */
    int *depths = regalloc_compute_loop_depths(ra, loop_headers, loop_exits, loop_count);
    if (!depths) {
        free(loop_headers);
        free(loop_exits);
        return;
    }
    
    /* Compute spill costs: base_cost * (10 ^ max_loop_depth) */
    for (size_t i = 0; i < ra->interval_count; i++) {
        LiveInterval *interval = &ra->intervals[i];
        
        /* Find maximum loop depth during this interval */
        int max_depth = 0;
        for (size_t pos = interval->start; pos <= interval->end && pos < ra->instruction_count; pos++) {
            if (depths[pos] > max_depth) {
                max_depth = depths[pos];
            }
        }
        
        /* Calculate cost: 10^depth (capped at depth 8) */
        if (max_depth > 8) max_depth = 8;
        size_t cost = 1;
        for (int d = 0; d < max_depth; d++) {
            cost *= 10;
        }
        
        /* Base cost of 1, multiplied by loop depth factor */
        interval->spill_cost = cost > 0 ? cost : 1;
    }
    
    free(depths);
    free(loop_headers);
    free(loop_exits);
}

/**
 * High-level cost-based register allocation
 * 
 * This function performs loop detection, spill cost computation, and
 * cost-based register allocation in one call.
 * 
 * @param ra Register allocator state
 * @return True if allocation succeeded
 */
static bool regalloc_allocate_cost_based(RegAllocator *ra) __attribute__((unused));
static bool regalloc_allocate_cost_based(RegAllocator *ra) {
    if (!ra || ra->interval_count == 0) {
        return true;  /* Nothing to allocate */
    }
    
    /* Compute spill costs based on loop nesting */
    regalloc_compute_spill_costs(ra);
    
    /* Enable cost-based mode and run allocation */
    bool old_mode = ra->use_cost_based;
    ra->use_cost_based = true;
    regalloc_allocate(ra);
    ra->use_cost_based = old_mode;
    
    return true;
}

/* =========================================================================
 * Callee-Saved Register Preservation
 * ========================================================================= */

/* Map register allocator index to VREG number (for x86-64) */
static int regalloc_idx_to_vreg(int idx) __attribute__((unused));
static int regalloc_idx_to_vreg(int idx) {
    /* Pool order: rax, rbx, rcx, rdx, rsi, rdi, r8-r15, rsp, rbp */
    /* Map pool index to physical register number */
    static const int idx_to_vreg[] = {0, 3, 1, 2, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 4, 5};
    if (idx >= 0 && idx < 16) {
        return idx_to_vreg[idx];
    }
    return 0;  /* Default to VREG_0 */
}

/* Get the physical register assigned to a temp ID */
static int regalloc_get_reg_for_temp(RegAllocator *ra, int temp_id) __attribute__((unused));
static int regalloc_get_reg_for_temp(RegAllocator *ra, int temp_id) {
    if (!ra || temp_id == 0xFF) return VREG_0;  /* Default to VREG_0 */
    
    /* VREG_0 (rax) is a fixed register for return values */
    if (temp_id == VREG_0) {
        return 0;  /* Physical rax encoding */
    }
    
    /* VREG_2 (rdx) is a fixed register for System V ABI struct returns */
    if (temp_id == VREG_2) {
        return 2;  /* Physical rdx encoding */
    }
    
    for (size_t i = 0; i < ra->interval_count; i++) {
        if (ra->intervals[i].temp_id == temp_id) {
            if (ra->intervals[i].is_spilled || ra->intervals[i].assigned_reg < 0) {
                return VREG_0;  /* Spilled - use default */
            }
            /* Return physical register based on class */
            if (ra->intervals[i].reg_class == REG_CLASS_XMM) {
                /* XMM registers: allocator index 0-7 maps directly to XMM0-XMM7 */
                return ra->intervals[i].assigned_reg;
            } else {
                /* GPR registers: allocator already returns physical register numbers */
                return ra->intervals[i].assigned_reg;
            }
        }
    }
    return VREG_0;  /* Not found - use default */
}

/* Check if a temporary is spilled, returning its spill slot (or -1 if not spilled) */
static int regalloc_get_spill_slot_for_temp(RegAllocator *ra, int temp_id) __attribute__((unused));
static int regalloc_get_spill_slot_for_temp(RegAllocator *ra, int temp_id) {
    if (!ra || temp_id == 0xFF || temp_id == VREG_0 || temp_id == VREG_2) return -1;
    for (size_t i = 0; i < ra->interval_count; i++) {
        if (ra->intervals[i].temp_id == temp_id) {
            if (ra->intervals[i].is_spilled || ra->intervals[i].assigned_reg < 0) {
                return ra->intervals[i].spill_slot;
            }
            return -1;
        }
    }
    return -1;
}

/* Get the number of intervals (for debugging) */
static size_t regalloc_get_interval_count(RegAllocator *ra) __attribute__((unused));
static size_t regalloc_get_interval_count(RegAllocator *ra) {
    return ra ? ra->interval_count : 0;
}

/* Get vector register name for a given ISA and register index */
static const char *regalloc_get_vec_reg_name(ZeroCCISA isa, int reg_idx) __attribute__((unused));
static const char *regalloc_get_vec_reg_name(ZeroCCISA isa, int reg_idx) {
    if (reg_idx < 0) return "none";
    
    if (isa == ISA_AARCH64) {
        if (reg_idx >= VEC_COUNT_AARCH64) return "invalid";
        return vec_registers_aarch64[reg_idx];
    } else if (isa == ISA_RISCV64) {
        if (reg_idx >= VEC_COUNT_RISCV) return "invalid";
        return vec_registers_riscv[reg_idx];
    } else {  /* x86_64 */
        if (reg_idx >= VEC_COUNT_X86) return "invalid";
        return vec_registers_x86[reg_idx];
    }
}

/* Get the number of vector registers for a given ISA */
static int regalloc_get_vec_reg_count(ZeroCCISA isa) __attribute__((unused));
static int regalloc_get_vec_reg_count(ZeroCCISA isa) {
    if (isa == ISA_AARCH64) return VEC_COUNT_AARCH64;
    if (isa == ISA_RISCV64) return VEC_COUNT_RISCV;
    return VEC_COUNT_X86;
}

/* =========================================================================
 * Callee-Saved Register Preservation
 *
 * AMD64 System V ABI callee-saved GPRs: rbx, r12, r13, r14, r15.
 * In the allocator pool these are indices: 1=rbx, 8=r12, 9=r13, 10=r14, 11=r15.
 * Their VREG (physical encoding) values via idx_to_vreg[]:
 *   idx 1  → vreg 3  (rbx)
 *   idx 8  → vreg 12 (r12)
 *   idx 9  → vreg 13 (r13)
 *   idx 10 → vreg 14 (r14)
 *   idx 11 → vreg 15 (r15)
 *
 * We reserve fixed frame slots for these at the top of the frame:
 *   rbx  → [rbp - 8]
 *   r12  → [rbp - 16]
 *   r13  → [rbp - 24]
 *   r14  → [rbp - 32]
 *   r15  → [rbp - 40]
 *
 * The caller must start current_stack_frame at CALLEE_SAVE_FRAME_RESERVE (48)
 * to ensure locals do not overlap with these reserved slots.
 * ========================================================================= */

/* Number of bytes reserved at the top of every frame for callee-save slots. */
#define CALLEE_SAVE_FRAME_RESERVE 48

#endif /* ZERO_CC_PARSER_REGALLOC_H */
