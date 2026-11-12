#ifndef ZERO_CC_PARSER_REGALLOC_H
#define ZERO_CC_PARSER_REGALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "zero_cc/ir/ir.h"
#include "zero_cc/isa/isa_backend.h"

#include "zero_cc/types/vregs.h"
#include "zero_cc/opt/opt_profile.h"

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
    REG_CLASS_GPR128, /* GNU __int128: a pair of GPRs (lo = assigned_reg, hi =
                        * assigned_reg_hi). 128-bit integer value model. */
    REG_CLASS_GPR64,  /* Sprint 170 (rv32 i64): a 64-bit integer in a pair of
                        * 32-bit GPRs (lo = assigned_reg, hi = assigned_reg_hi).
                        * rv32 only; spills to ONE 8-byte slot (two 4-byte words). */
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

/* FP spill scratch registers - outside the XMM pool (0-7). Must match the
 * ISA backends' fp_scratch_reg1/2 initializers (C8). */
#define REGALLOC_FP_SCRATCH1 15
#define REGALLOC_FP_SCRATCH2 14

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
    int assigned_reg;         /* Assigned register index (-1 if spilled). For
                               * REG_CLASS_GPR128 this is the LOW half. */
    int assigned_reg_hi;      /* HIGH half for REG_CLASS_GPR128 (-1 otherwise) */
    int spill_slot;           /* Spill slot index (-1 if not spilled) */
    bool is_spilled;          /* Whether this interval is spilled */
    bool spans_call;          /* Whether this interval spans a function call */
    bool no_spill;            /* Sprint 135: inline-asm operand temp - the
                               * OP_ASM template references this value in a
                               * register at the asm site, so it can never be
                               * spilled (pressure failure instead) */
    uint32_t forbidden_mask;  /* Sprint 135: hardware GPRs the template clobbers
                               * - an asm operand must never be placed there */
    size_t spill_cost;        /* Spill cost (higher = more important to keep in reg) */
    int fixed_reg;            /* Pre-colored physical register (ABI parameter slot
                               * pre-coloring, Sprint 117); -1 = not pinned. */
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
    int32_t spill_base_offset; /* Base frame offset for spill slots */

    /* Instruction count for interval tracking */
    size_t instruction_count;

    /* Mask of callee-saved GPR allocator indices that were assigned.
     * Callee-saved indices in the pool: 1=rbx, 8=r12, 9=r13, 10=r14, 11=r15
     * Bit N is set if pool index N was assigned to some live interval. */
    uint32_t used_callee_saved_gpr_mask;

    /* Cost-based allocation flag */
    bool use_cost_based;  /* If true, use spill cost priority instead of start position */
    bool spill_across_calls; /* True for has_setjmp functions: force spans_call intervals onto stack */

    /* Sprint 135: set by regalloc_allocate when an inline-asm operand temp
     * (no_spill) could not be kept in a register and no spillable victim
     * existed. The lowering reports a clean diagnostic afterwards. */
    bool asm_pressure_fail;

    /* O(1) lookup table from 16-bit temp_id to interval index (-1 if none) */
    int32_t temp_to_interval[65536];
    size_t fixed_interval_indices[256];
    size_t fixed_interval_count;
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

/* Set target ISA and configure reserved registers */
static void regalloc_set_isa(RegAllocator *ra, ZeroCCISA isa) __attribute__((unused));
static void regalloc_set_isa(RegAllocator *ra, ZeroCCISA isa) {
    if (!ra) return;
    ra->target_isa = isa;
    for (int i = 0; i < GPR_COUNT; i++) ra->gpr_free[i] = true;
    if (isa == ISA_AARCH64) {
        ra->gpr_free[8] = false;  /* x8 reserved for indirect result location (sret) */
        ra->gpr_free[9] = false;  /* x9 reserved for scratch_reg1 */
        ra->gpr_free[10] = false; /* x10 reserved for scratch_reg2 */
        ra->gpr_free[15] = false; /* x15 reserved for g128 scratch */
        ra->gpr_free[16] = false; /* x16 (IP0) reserved for linker/scratch */
        ra->gpr_free[17] = false; /* x17 (IP1) reserved for linker/scratch */
        ra->gpr_free[18] = false; /* x18 reserved platform register */
        ra->gpr_free[29] = false; /* x29 reserved for frame pointer */
        ra->gpr_free[30] = false; /* x30 reserved for link register */
        ra->gpr_free[31] = false; /* sp reserved for stack pointer */
    } else if (isa == ISA_RISCV64 || isa == ISA_RISCV32) {
        ra->gpr_free[0] = false;  /* zero */
        ra->gpr_free[1] = false;  /* ra */
        ra->gpr_free[2] = false;  /* sp */
        ra->gpr_free[3] = false;  /* gp */
        ra->gpr_free[4] = false;  /* tp */
        ra->gpr_free[5] = false;  /* t0 (scratch_reg1) */
        ra->gpr_free[6] = false;  /* t1 (scratch_reg2) */
        ra->gpr_free[8] = false;  /* s0 / fp */
        ra->gpr_free[30] = false; /* t5: large-immediate/offset address scratch
                                    (Sprint 109) - never holds a live value */
    } else {
        ra->gpr_free[1] = false;  /* RCX is reserved for shift count (CL) on x86-64 */
        ra->gpr_free[4] = false;  /* RSP is reserved for stack pointer */
        ra->gpr_free[5] = false;  /* RBP is reserved for frame pointer */
        ra->gpr_free[10] = false; /* r10 is reserved for backend scratch1 (spill reload/store) */
        ra->gpr_free[11] = false; /* r11 is reserved for backend scratch2 (spill reload) */
    }
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
    ra->use_cost_based = true;   /* Enable cost-based allocation for better loop performance */
    ra->spill_across_calls = false;
    ra->asm_pressure_fail = false;
    memset(ra->temp_to_interval, -1, sizeof(ra->temp_to_interval));
    ra->fixed_interval_count = 0;
    regalloc_set_isa(ra, ISA_X86_64);  /* Default to x86-64, caller can call regalloc_set_isa */
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
    interval->assigned_reg_hi = -1;
    interval->spill_slot = -1;
    interval->is_spilled = false;
    interval->no_spill = false;
    interval->forbidden_mask = 0;
    interval->spans_call = false;
    interval->spill_cost = 1;  /* Default spill cost */
    interval->fixed_reg = -1;

    size_t idx = ra->interval_count - 1;
    if (temp_id >= 0 && temp_id < 65536) {
        if (ra->temp_to_interval[temp_id] < 0) {
            ra->temp_to_interval[temp_id] = (int32_t)idx;
        }
    }
    if ((temp_id == VREG_0 || temp_id == VREG_2) && ra->fixed_interval_count < 256) {
        ra->fixed_interval_indices[ra->fixed_interval_count++] = idx;
    }

    return (int)idx;
}

/* Mark an interval as spanning a function call */
static void regalloc_set_interval_spans_call(RegAllocator *ra, int interval_idx, bool spans_call) __attribute__((unused));
static void regalloc_set_interval_spans_call(RegAllocator *ra, int interval_idx, bool spans_call) {
    if (ra && interval_idx >= 0 && (size_t)interval_idx < ra->interval_count) {
        ra->intervals[interval_idx].spans_call = spans_call;
    }
}

/* Pre-color an interval to a fixed physical register (Sprint 117 parameter
 * pre-coloring: an ABI argument value arrives in its slot register at function
 * entry, so the parameter interval's natural home is the slot itself). The
 * interval is also registered in fixed_interval_indices so
 * regalloc_reg_conflicts_fixed keeps every other interval off that register
 * for the whole overlap window. */
static void regalloc_set_fixed_reg(RegAllocator *ra, int interval_idx, int reg) __attribute__((unused));
static void regalloc_set_fixed_reg(RegAllocator *ra, int interval_idx, int reg) {
    if (!ra || interval_idx < 0 || (size_t)interval_idx >= ra->interval_count) return;
    ra->intervals[interval_idx].fixed_reg = reg;
    if (ra->fixed_interval_count < 256) {
        ra->fixed_interval_indices[ra->fixed_interval_count++] = (size_t)interval_idx;
    }
}

/* Mark an interval non-spillable (inline-asm operand, Sprint 135) */
static void regalloc_set_no_spill(RegAllocator *ra, int interval_idx) __attribute__((unused));
static void regalloc_set_no_spill(RegAllocator *ra, int interval_idx) {
    if (!ra || interval_idx < 0 || (size_t)interval_idx >= ra->interval_count) return;
    ra->intervals[interval_idx].no_spill = true;
}

/* Forbid a set of hardware GPRs for an interval (inline-asm operand whose
 * OP_ASM clobbers those registers, Sprint 135). */
static void regalloc_set_forbidden(RegAllocator *ra, int interval_idx, uint32_t mask) __attribute__((unused));
static void regalloc_set_forbidden(RegAllocator *ra, int interval_idx, uint32_t mask) {
    if (!ra || interval_idx < 0 || (size_t)interval_idx >= ra->interval_count) return;
    ra->intervals[interval_idx].forbidden_mask |= mask;
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
        if (active->end < current_pos) {
            /* Free the register only if no other remaining active interval holds it */
            if (active->assigned_reg >= 0) {
                bool still_in_use = false;
                for (size_t j = 0; j < ra->active_count; j++) {
                    if (j != i && ra->active[j]->end >= current_pos &&
                        ra->active[j]->reg_class == active->reg_class &&
                        ra->active[j]->assigned_reg == active->assigned_reg) {
                        still_in_use = true;
                        break;
                    }
                }
                if (!still_in_use) {
                    if (active->reg_class == REG_CLASS_GPR) {
                        ra->gpr_free[active->assigned_reg] = true;
                    } else if (active->reg_class == REG_CLASS_GPR128 ||
                               active->reg_class == REG_CLASS_GPR64) {
                        ra->gpr_free[active->assigned_reg] = true;
                        if (active->assigned_reg_hi >= 0) {
                            ra->gpr_free[active->assigned_reg_hi] = true;
                        }
                    } else if (active->reg_class == REG_CLASS_XMM) {
                        ra->xmm_free[active->assigned_reg] = true;
                        if (ra->target_isa != ISA_RISCV64 && ra->target_isa != ISA_RISCV32 &&
                            active->assigned_reg < 32) {
                            ra->vec_free[active->assigned_reg] = true;
                        }
                    } else if (active->reg_class == REG_CLASS_VEC) {
                        ra->vec_free[active->assigned_reg] = true;
                        if (ra->target_isa != ISA_RISCV64 && ra->target_isa != ISA_RISCV32 &&
                            active->assigned_reg < XMM_COUNT) {
                            ra->xmm_free[active->assigned_reg] = true;
                        }
                    }
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

/* Check if a physical GPR is callee-saved for the target ISA */
static inline bool regalloc_is_callee_saved_gpr(const RegAllocator *ra, int phys_reg) {
    if (!ra || phys_reg < 0) return false;
    if (ra->target_isa == ISA_AARCH64) {
        return phys_reg >= 19 && phys_reg <= 28;
    } else if (ra->target_isa == ISA_RISCV64 || ra->target_isa == ISA_RISCV32) {
        return phys_reg == 9 || (phys_reg >= 18 && phys_reg <= 27);
    } else {
        return phys_reg == 3 || (phys_reg >= 12 && phys_reg <= 15);
    }
}

/* Physical register an interval is pinned to: VREG_0/VREG_2 are the
 * pre-colored return registers; other intervals are pinned via fixed_reg
 * (ABI parameter slot pre-coloring). -1 = unpinned. */
static inline int regalloc_fixed_reg_of(const RegAllocator *ra, const LiveInterval *other) {
    if (other->temp_id == VREG_0) {
        return (ra->target_isa == ISA_RISCV64 || ra->target_isa == ISA_RISCV32) ? 10 : 0;
    }
    if (other->temp_id == VREG_2) {
        if (ra->target_isa == ISA_RISCV64 || ra->target_isa == ISA_RISCV32) return 11;
        return (ra->target_isa == ISA_AARCH64) ? 1 : 2;
    }
    return other->fixed_reg;
}

/* Check if a physical GPR conflicts with a pre-colored fixed interval
 * (VREG_0 / VREG_2 return registers, or an ABI parameter slot pinned via
 * regalloc_set_fixed_reg). The pinned register must not be handed to a
 * non-fixed interval whose live range overlaps the fixed interval's. */
static inline bool regalloc_reg_conflicts_fixed(const RegAllocator *ra, int phys_reg,
                                                const LiveInterval *interval) {
    if (!ra || !interval) return false;
    if (ra->fixed_interval_count < 256) {
        for (size_t k = 0; k < ra->fixed_interval_count; k++) {
            const LiveInterval *other = &ra->intervals[ra->fixed_interval_indices[k]];
            if (other->reg_class != REG_CLASS_GPR) continue;
            if (regalloc_fixed_reg_of(ra, other) != phys_reg) continue;
            if (!(interval->end < other->start || interval->start > other->end)) {
                return true;
            }
        }
        return false;
    }
    for (size_t i = 0; i < ra->interval_count; i++) {
        const LiveInterval *other = &ra->intervals[i];
        if (other->reg_class != REG_CLASS_GPR) continue;
        if (other->temp_id != VREG_0 && other->temp_id != VREG_2 && other->fixed_reg < 0) continue;
        if (regalloc_fixed_reg_of(ra, other) != phys_reg) continue;
        if (!(interval->end < other->start || interval->start > other->end)) {
            return true;
        }
    }
    return false;
}

/* Allocate a PAIR of GPRs for a REG_CLASS_GPR128 temp: `lo` is returned and
 * `hi` is stored on the interval. The two halves are distinct, free registers
 * drawn from `pool`; any in `cs_mask` are callee-saved (recorded in
 * used_callee_saved_gpr_mask so the prologue preserves the pair across calls).
 * Returns the low-half register, or -1 to spill. */
static int gpr128_alloc_pair(RegAllocator *ra, LiveInterval *interval,
                             const int *pool, size_t npool, uint32_t cs_mask) {
    int lo = -1, hi = -1;
    for (size_t i = 0; i < npool && lo < 0; i++) {
        int r = pool[i];
        if (r >= 0 && r < GPR_COUNT && ra->gpr_free[r] &&
            !regalloc_reg_conflicts_fixed(ra, r, interval)) {
            lo = r; ra->gpr_free[lo] = false;
            if ((cs_mask >> lo) & 1u) ra->used_callee_saved_gpr_mask |= (1u << lo);
        }
    }
    for (size_t i = 0; i < npool && hi < 0; i++) {
        int r = pool[i];
        if (r >= 0 && r != lo && r < GPR_COUNT && ra->gpr_free[r] &&
            !regalloc_reg_conflicts_fixed(ra, r, interval)) {
            hi = r; ra->gpr_free[hi] = false;
            if ((cs_mask >> hi) & 1u) ra->used_callee_saved_gpr_mask |= (1u << hi);
        }
    }
    if (lo >= 0 && hi >= 0) {
        interval->assigned_reg_hi = hi;
        return lo;
    }
    if (lo >= 0) ra->gpr_free[lo] = true;  /* release lo if no hi */
    return -1; /* spill */
}

/* Allocate a register for the given interval */
static int alloc_reg(RegAllocator *ra, LiveInterval *interval) __attribute__((unused));
static int alloc_reg(RegAllocator *ra, LiveInterval *interval) {
    if (!ra || !interval) return -1;

    /* Check target ISA */
    bool is_aarch64 = (ra->target_isa == ISA_AARCH64);
    bool is_riscv = (ra->target_isa == ISA_RISCV64 || ra->target_isa == ISA_RISCV32);

    /* Find a free register in the appropriate class */
    if (interval->reg_class == REG_CLASS_GPR) {
        if (interval->spans_call) {
            if (ra->spill_across_calls) {
                return -1;
            }
            if (is_aarch64) {
                /* AArch64: strictly allocate callee-saved registers (x19-x28) */
                for (int i = 19; i <= 28; i++) {
                    if (i < GPR_COUNT && ra->gpr_free[i]) {
                        ra->gpr_free[i] = false;
                        ra->used_callee_saved_gpr_mask |= (1u << i);
                        return i;
                    }
                }
            } else if (is_riscv) {
                /* RISC-V: strictly allocate callee-saved registers s1 (9), s2-s11 (18-27) */
                static const int rv_callee_saved[] = {9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
                for (size_t k = 0; k < sizeof(rv_callee_saved)/sizeof(rv_callee_saved[0]); k++) {
                    int phys_reg = rv_callee_saved[k];
                    if (ra->gpr_free[phys_reg]) {
                        ra->gpr_free[phys_reg] = false;
                        ra->used_callee_saved_gpr_mask |= (1u << phys_reg);
                        return phys_reg;
                    }
                }
            } else {
                /* x86-64: strictly allocate callee-saved registers (rbx, r12-r15) */
                static const int callee_saved_pool_to_phys[] = {3, 12, 13, 14, 15};
                for (size_t k = 0; k < sizeof(callee_saved_pool_to_phys)/sizeof(callee_saved_pool_to_phys[0]); k++) {
                    int phys_reg = callee_saved_pool_to_phys[k];
                    if (ra->gpr_free[phys_reg]) {
                        ra->gpr_free[phys_reg] = false;
                        ra->used_callee_saved_gpr_mask |= (1u << phys_reg);
                        return phys_reg;
                    }
                }
            }
            /* Never assign a caller-saved GPR to an interval that spans a call; spill instead */
            return -1;
        }

        if (is_aarch64) {
            /* AArch64 general pool: x0-x7, x11-x15, x19-x28 */
            static const int aarch64_pool[] = {
                0, 1, 2, 3, 4, 5, 6, 7,
                11, 12, 13, 14,
                19, 20, 21, 22, 23, 24, 25, 26, 27, 28
            };
            for (size_t pool_idx = 0; pool_idx < sizeof(aarch64_pool)/sizeof(aarch64_pool[0]); pool_idx++) {
                int phys_reg = aarch64_pool[pool_idx];
                if (ra->gpr_free[phys_reg] && !regalloc_reg_conflicts_fixed(ra, phys_reg, interval)) {
                    ra->gpr_free[phys_reg] = false;
                    if (phys_reg >= 19 && phys_reg <= 28) {
                        ra->used_callee_saved_gpr_mask |= (1u << phys_reg);
                    }
                    return phys_reg;
                }
            }
        } else if (is_riscv) {
            /* RISC-V general pool: a0-a7 (10-17), t2 (7), s1-s11 (9, 18-27).
             * NOTE: t0/t1 (5,6), t3/t4 (28,29) and t6 (31) are reserved for 128-bit
             * spill scratch (Sprint 109 GPR128 spill fix) so they hold no 64-bit value
             * (t0 is also the RISC-V 128-bit add/sub/neg/mul carry scratch). t5 (30)
             * is the large-immediate/offset address scratch (Sprint 109): li/add
             * address materialization for out-of-range 12-bit displacements must
             * never use a register that can hold a live reloaded value (t0/t1 hold
             * 64-bit spill reloads, t3/t4/t6 hold 128-bit halves), or it clobbers
             * the operand (FAIL 45: src2's address li overwrote src1's reload in t0).
             * It is therefore not allocatable.
             * Sprint 170 (rv32 i64): t2 (7) is ALSO reserved on rv32 - the 64-bit
             * pair emitters use it as the third carry/multiply scratch alongside
             * t0/t5, so a live value there would be clobbered mid-op. */
            static const int riscv_pool[] = {
                10, 11, 12, 13, 14, 15, 16, 17,
                7,
                9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27
            };
            for (size_t pool_idx = 0; pool_idx < sizeof(riscv_pool)/sizeof(riscv_pool[0]); pool_idx++) {
                int phys_reg = riscv_pool[pool_idx];
                if (ra->target_isa == ISA_RISCV32 && phys_reg == 7) continue;  /* t2 = 64-bit-op scratch (Sprint 170) */
                if (ra->gpr_free[phys_reg] && !regalloc_reg_conflicts_fixed(ra, phys_reg, interval)) {
                    ra->gpr_free[phys_reg] = false;
                    if (phys_reg == 9 || (phys_reg >= 18 && phys_reg <= 27)) {
                        ra->used_callee_saved_gpr_mask |= (1u << phys_reg);
                    }
                    return phys_reg;
                }
            }
        } else {
            /* x86-64 general pool (TEMP DIAG: original order to isolate tls.c -O1
             * regression). */
            static const int pool_to_phys[] = {7, 6, 2, 8, 9, 0, 3, 12, 13, 14, 15};
            for (size_t pool_idx = 0; pool_idx < sizeof(pool_to_phys)/sizeof(pool_to_phys[0]); pool_idx++) {
                int phys_reg = pool_to_phys[pool_idx];
                if (interval->forbidden_mask && (interval->forbidden_mask & (1u << phys_reg))) continue;
                if (ra->gpr_free[phys_reg] && !regalloc_reg_conflicts_fixed(ra, phys_reg, interval)) {
                    ra->gpr_free[phys_reg] = false;
                    static const uint32_t x86_callee_saved_mask =
                        (1u << 3) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15);
                    if ((x86_callee_saved_mask >> phys_reg) & 1u) {
                        ra->used_callee_saved_gpr_mask |= (1u << phys_reg);
                    }
                    return phys_reg;
                }
            }
        }
    } else if (interval->reg_class == REG_CLASS_GPR128) {
        /* 128-bit integer: allocate a PAIR of GPRs (lo returned, hi stored on
         * the interval). The pair is kept in callee-saved registers so it
         * survives across calls (a 128-bit value outlives the calls between its
         * producer and its uses). */
        if (ra->target_isa == ISA_X86_64) {
            /* Exclude rax(0)/rdx(2): OP_MUL128 / 128-bit div use them as scratch. */
            static const int cs_pool[] = {3, 12, 13, 14, 15};
            static const int gp_pool[] = {7, 6, 8, 9, 3, 12, 13, 14, 15};
            static const uint32_t x86_cs_mask =
                (1u << 3) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15);
            const int *pool = interval->spans_call ? cs_pool : gp_pool;
            size_t npool = interval->spans_call ? 5
                        : sizeof(gp_pool) / sizeof(gp_pool[0]);
            return gpr128_alloc_pair(ra, interval, pool, npool, x86_cs_mask);
        }
        if (ra->target_isa == ISA_AARCH64) {
            /* (lo,hi) pair from callee-saved x19-x28. */
            static const int cs_pool[] = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
            static const uint32_t cs_mask =
                (1u<<19)|(1u<<20)|(1u<<21)|(1u<<22)|(1u<<23)|(1u<<24)|
                (1u<<25)|(1u<<26)|(1u<<27)|(1u<<28);
            return gpr128_alloc_pair(ra, interval, cs_pool,
                                     sizeof(cs_pool)/sizeof(cs_pool[0]), cs_mask);
        }
        if (ra->target_isa == ISA_RISCV64) {
            /* (lo,hi) pair from callee-saved s2-s11 (18-27). */
            static const int cs_pool[] = {18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
            static const uint32_t cs_mask =
                (1u<<18)|(1u<<19)|(1u<<20)|(1u<<21)|(1u<<22)|(1u<<23)|
                (1u<<24)|(1u<<25)|(1u<<26)|(1u<<27);
            return gpr128_alloc_pair(ra, interval, cs_pool,
                                     sizeof(cs_pool)/sizeof(cs_pool[0]), cs_mask);
        }
        return -1; /* other ISAs (e.g. riscv32, 32-bit GPRs): spill for now */
    } else if (interval->reg_class == REG_CLASS_GPR64) {
        /* Sprint 170 (rv32 i64): a 64-bit value in a (lo,hi) pair of 32-bit
         * GPRs. Allocate from the RISC-V callee-saved pool (s1, s2-s11) so a
         * 64-bit value survives the calls between producer and use. */
        if (ra->target_isa == ISA_RISCV32) {
            static const int cs_pool[] = {9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
            static const uint32_t cs_mask =
                (1u<<9)|(1u<<18)|(1u<<19)|(1u<<20)|(1u<<21)|(1u<<22)|
                (1u<<23)|(1u<<24)|(1u<<25)|(1u<<26)|(1u<<27);
            return gpr128_alloc_pair(ra, interval, cs_pool,
                                     sizeof(cs_pool)/sizeof(cs_pool[0]), cs_mask);
        }
        return -1; /* GPR64 is rv32-only */
    } else if (interval->reg_class == REG_CLASS_XMM) {
        /* All FP registers are caller-saved on every supported ABI: an FP
         * interval that spans a call cannot live in a register - force the
         * (class-aware) spill path (C8). */
        if (interval->spans_call) return -1;
        for (int i = 0; i < XMM_COUNT; i++) {
            if (ra->xmm_free[i]) {
                ra->xmm_free[i] = false;
                /* x86/AArch64: VEC index i (i<8) is the same physical
                 * register as xmm i - never double-allocate (C8). */
                if (ra->target_isa != ISA_RISCV64 && ra->target_isa != ISA_RISCV32 && i < 32) {
                    ra->vec_free[i] = false;
                }
                return i;
            }
        }
    } else if (interval->reg_class == REG_CLASS_VEC) {
        /* Vector register allocation (ISA-specific count) */
        int vec_count = VEC_COUNT(ra);
        for (int i = 0; i < vec_count; i++) {
            if (!ra->vec_free[i]) continue;
            if (ra->target_isa != ISA_RISCV64 && ra->target_isa != ISA_RISCV32) {
                /* x86/AArch64: vec i (i<8) is the same physical register
                 * as xmm i; 14/15 are the FP spill scratch (C8). */
                if (i < XMM_COUNT && !ra->xmm_free[i]) continue;
                if (i == REGALLOC_FP_SCRATCH1 || i == REGALLOC_FP_SCRATCH2) continue;
            }
            ra->vec_free[i] = false;
            if (ra->target_isa != ISA_RISCV64 && ra->target_isa != ISA_RISCV32 && i < XMM_COUNT) {
                ra->xmm_free[i] = false;
            }
            return i;
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
    } else if (interval->reg_class == REG_CLASS_GPR128) {
        /* 128-bit integer: reserve TWO 8-byte GPR slots (16 bytes). The
         * backend loads/stores the (lo,hi) pair at offset (slot+1)*8. */
        interval->spill_slot = ra->next_spill_slot;
        ra->next_spill_slot += 2;
    } else if (interval->reg_class == REG_CLASS_GPR64) {
        /* Sprint 170 (rv32 i64): a 64-bit value fits ONE 8-byte slot (two
         * 4-byte words). The backend stores lo at slot*8 and hi at slot*8+4. */
        interval->spill_slot = ra->next_spill_slot++;
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

    /* Profile: count total intervals */
    for (size_t i = 0; i < ra->interval_count; i++) {
        opt_profile_count_regalloc_interval();
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

        /* Try to allocate a register (VREG_0 is pre-colored to return register) */
        int reg;
        if (current->temp_id == VREG_0) {
            reg = (ra->target_isa == ISA_RISCV64 || ra->target_isa == ISA_RISCV32) ? 10 : 0;
            ra->gpr_free[reg] = false;
        } else if (current->temp_id == VREG_2) {
            reg = (ra->target_isa == ISA_RISCV64 || ra->target_isa == ISA_RISCV32) ? 11 :
                  (ra->target_isa == ISA_AARCH64 ? 1 : 2);
            ra->gpr_free[reg] = false;
        } else if (current->fixed_reg >= 0) {
            /* ABI parameter slot pre-coloring (Sprint 117): the argument value
             * arrives in that register at function entry, so the interval's
             * natural home is the slot itself — no materialization move is
             * needed, and no other interval can hold the slot during the
             * parameter's live range (conflict check). The LRA clash check
             * guarantees no other fixed interval overlaps on this register;
             * if the slot is somehow held, fall back to a normal register —
             * the OP_PARAM_LOAD emission then moves the value in. */
            reg = current->fixed_reg;
            if (current->no_spill && current->forbidden_mask &&
                (current->forbidden_mask & (1u << reg))) {
                /* Sprint 135: a fixed-register asm operand (a/b/d/S/D) whose
                 * register the template itself clobbers is unsatisfiable. */
                ra->asm_pressure_fail = true;
                reg = -1;
            } else if (!ra->gpr_free[reg]) {
                if (current->no_spill) {
                    ra->asm_pressure_fail = true;
                    reg = -1;
                } else {
                    reg = alloc_reg(ra, current);
                }
            }
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
                /* Compare current with all active intervals of the SAME register class */
                spill = current;

                for (size_t j = 0; j < ra->active_count; j++) {
                    LiveInterval *active = ra->active[j];
                    if (active->reg_class != current->reg_class) continue;
                    /* Pre-colored return value intervals cannot be spilled/stolen */
                    if (active->temp_id == VREG_0 || active->temp_id == VREG_2) continue;
                    /* Already-spilled intervals hold no register to hand over */
                    if (active->is_spilled) continue;
                    /* Sprint 135: inline-asm operand intervals never spill */
                    if (active->no_spill) continue;
                    if (regalloc_reg_conflicts_fixed(ra, active->assigned_reg, current)) continue;
                    if (current->spans_call && current->reg_class == REG_CLASS_GPR &&
                        !regalloc_is_callee_saved_gpr(ra, active->assigned_reg)) {
                        continue;
                    }
                    /* All FP/SIMD registers are caller-saved: an interval
                     * spanning a call must spill, never steal (C8). Same for
                     * has_setjmp functions where call-spanning GPRs must stay
                     * on the stack across longjmp. */
                    if (current->spans_call &&
                        (ra->spill_across_calls || current->reg_class == REG_CLASS_XMM || current->reg_class == REG_CLASS_VEC)) {
                        continue;
                    }

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
                /* Classic linear-scan: spill the interval with furthest end point of same class */
                spill = current;
                for (size_t j = 0; j < ra->active_count; j++) {
                    LiveInterval *active = ra->active[j];
                    if (active->reg_class != current->reg_class) continue;
                    /* Pre-colored return value intervals cannot be spilled/stolen */
                    if (active->temp_id == VREG_0 || active->temp_id == VREG_2) continue;
                    /* Already-spilled intervals hold no register to hand over */
                    if (active->is_spilled) continue;
                    /* Sprint 135: inline-asm operand intervals never spill */
                    if (active->no_spill) continue;
                    if (regalloc_reg_conflicts_fixed(ra, active->assigned_reg, current)) continue;
                    if (current->spans_call && current->reg_class == REG_CLASS_GPR &&
                        !regalloc_is_callee_saved_gpr(ra, active->assigned_reg)) {
                        continue;
                    }
                    /* All FP/SIMD registers are caller-saved: an interval
                     * spanning a call must spill, never steal (C8). Same for
                     * has_setjmp functions where call-spanning GPRs must stay
                     * on the stack across longjmp. */
                    if (current->spans_call &&
                        (ra->spill_across_calls || current->reg_class == REG_CLASS_XMM || current->reg_class == REG_CLASS_VEC)) {
                        continue;
                    }
                    if (active->end > spill->end) {
                        spill = active;
                    }
                }
            }

            if (spill->no_spill) {
                ra->asm_pressure_fail = true;
            }

            if (spill == current || spill->assigned_reg < 0) {
                /* Spill current interval (also when the "stolen" interval
                 * holds no register - stealing -1 leaves a dangling temp) */
                opt_profile_count_regalloc_spill(ra->use_cost_based);
                spill_interval(ra, current);
            } else {
                /* Spill the active interval and give its register to current */
                opt_profile_count_regalloc_spill(ra->use_cost_based);
                spill_interval(ra, spill);
                current->assigned_reg = spill->assigned_reg;
                if (current->reg_class == REG_CLASS_GPR128 &&
                    spill->reg_class == REG_CLASS_GPR128) {
                    /* Hand over the FULL (lo,hi) pair: the 128-bit value model
                     * needs both halves, and the spilled interval must release
                     * its claim on the hi register so its expiry (which only
                     * releases when assigned_reg >= 0) and any later lookup
                     * cannot treat it as still owned. (Sprint 109 GPR128
                     * steal fix: a stolen-lo-only interval left hi = -1, and
                     * the MC emitter then received register -1 for the high
                     * half, miscompiling under register pressure.) */
                    current->assigned_reg_hi = spill->assigned_reg_hi;
                    spill->assigned_reg_hi = -1;
                }
                if (current->reg_class == REG_CLASS_GPR &&
                    regalloc_is_callee_saved_gpr(ra, current->assigned_reg)) {
                    ra->used_callee_saved_gpr_mask |= (1u << current->assigned_reg);
                }
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
static size_t regalloc_detect_loops(RegAllocator *ra, int **loop_headers, int **loop_exits, ZeroCCCompiler *c) __attribute__((unused));
static size_t regalloc_detect_loops(RegAllocator *ra, int **loop_headers, int **loop_exits, ZeroCCCompiler *c) {
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
        Instr *instr = ir_get(c, i);
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
    size_t loop_count = regalloc_detect_loops(ra, &loop_headers, &loop_exits, NULL);

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
    int def_ret_reg = (ra && (ra->target_isa == ISA_RISCV64 || ra->target_isa == ISA_RISCV32)) ? 10 : 0;
    if (!ra || (uint32_t)temp_id == TEMP_NONE) return def_ret_reg;

    /* VREG_0 (rax / x0 / a0) and VREG_F0 (xmm0 / v0 / fa0) are fixed return registers */
    if (temp_id == VREG_0 || temp_id == VREG_F0) {
        return def_ret_reg;
    }

    if (temp_id == VREG_1 || temp_id == VREG_F1) {
        if (ra->target_isa == ISA_RISCV64 || ra->target_isa == ISA_RISCV32) return 11; /* a1 / fa1 */
        if (ra->target_isa == ISA_AARCH64) return 1; /* x1 / v1 */
        return 1;
    }

    /* VREG_2 (rdx / x1 / a1) is a fixed register for 9-16 byte struct returns */
    if (temp_id == VREG_2) {
        if (ra->target_isa == ISA_RISCV64 || ra->target_isa == ISA_RISCV32) return 11; /* a1 */
        if (ra->target_isa == ISA_AARCH64) return 1; /* x1 */
        return 2;  /* Physical rdx encoding on x86-64 */
    }

    if (zc_temp_is_vreg((uint32_t)temp_id)) {
        return zc_vreg_hw_num((uint32_t)temp_id);
    }

    if (temp_id >= 0 && temp_id < 65536) {
        int32_t idx = ra->temp_to_interval[temp_id];
        if (idx >= 0 && (size_t)idx < ra->interval_count) {
            if (ra->intervals[idx].is_spilled || ra->intervals[idx].assigned_reg < 0) {
                return def_ret_reg;  /* Spilled - use default */
            }
            return ra->intervals[idx].assigned_reg;
        }
        return def_ret_reg;
    }
    return def_ret_reg;  /* Not found - use default */
}

/* Get the HIGH-half physical register for a REG_CLASS_GPR128 / REG_CLASS_GPR64
 * temp (the low half is what regalloc_get_reg_for_temp returns). Returns -1
 * for non-pair temps or spilled pair temps (the caller must handle the
 * spilled case). */
static int regalloc_get_reg_hi_for_temp(RegAllocator *ra, int temp_id) __attribute__((unused));
static int regalloc_get_reg_hi_for_temp(RegAllocator *ra, int temp_id) {
    if (!ra || (uint32_t)temp_id == TEMP_NONE) return -1;
    if (temp_id >= 0 && temp_id < 65536) {
        int32_t idx = ra->temp_to_interval[temp_id];
        if (idx >= 0 && (size_t)idx < ra->interval_count) {
            LiveInterval *iv = &ra->intervals[idx];
            if ((iv->reg_class == REG_CLASS_GPR128 ||
                 iv->reg_class == REG_CLASS_GPR64) && !iv->is_spilled &&
                iv->assigned_reg >= 0 && iv->assigned_reg_hi >= 0) {
                return iv->assigned_reg_hi;
            }
        }
    }
    return -1;
}

/* Check if a temporary is spilled, returning its spill slot (or -1 if not spilled) */
static int regalloc_get_spill_slot_for_temp(RegAllocator *ra, int temp_id) __attribute__((unused));
static int regalloc_get_spill_slot_for_temp(RegAllocator *ra, int temp_id) {
    if (!ra || (uint32_t)temp_id == TEMP_NONE || zc_temp_is_vreg((uint32_t)temp_id)) return -1;
    if (temp_id >= 0 && temp_id < 65536) {
        int32_t idx = ra->temp_to_interval[temp_id];
        if (idx >= 0 && (size_t)idx < ra->interval_count) {
            if (ra->intervals[idx].is_spilled || ra->intervals[idx].assigned_reg < 0) {
                return ra->intervals[idx].spill_slot;
            }
            return -1;
        }
    }
    return -1;
}

/* Get register class for a temporary variable */
static RegClass regalloc_get_class_for_temp(RegAllocator *ra, int temp_id) __attribute__((unused));
static RegClass regalloc_get_class_for_temp(RegAllocator *ra, int temp_id) {
    if (!ra || (uint32_t)temp_id == TEMP_NONE) return REG_CLASS_GPR;
    if (zc_temp_is_fp_vreg((uint32_t)temp_id)) return REG_CLASS_XMM;
    if (zc_temp_is_vreg((uint32_t)temp_id)) return REG_CLASS_GPR;
    if (temp_id >= 0 && temp_id < 65536) {
        int32_t idx = ra->temp_to_interval[temp_id];
        if (idx >= 0 && (size_t)idx < ra->interval_count) {
            return ra->intervals[idx].reg_class;
        }
    }
    return REG_CLASS_GPR;
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
