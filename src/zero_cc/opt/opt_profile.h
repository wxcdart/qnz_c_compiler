#ifndef ZERO_CC_OPT_PROFILE_H
#define ZERO_CC_OPT_PROFILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Optimization Profiling
 *
 * Tracks statistics for optimization passes to measure effectiveness.
 *
 * Usage:
 *   opt_profile_reset();
 *   opt_run_passes(c);
 *   opt_profile_dump(stderr);
 */

/* Optimization counters */
typedef struct {
    /* CSE statistics */
    size_t cse_total_instructions;
    size_t cse_eliminated;
    size_t cse_basic_blocks;

    /* GVN statistics */
    size_t gvn_total_instructions;
    size_t gvn_eliminated;

    /* Branchless select (Sprint 39) */
    size_t branchless_selects;

    /* Copy propagation statistics */
    size_t copy_prop_instructions;
    size_t copy_prop_changes;

    /* DCE statistics */
    size_t dce_eliminated;
    size_t dce_iterations;

    /* LICM statistics */
    size_t licm_loops_detected;
    size_t licm_invariants_found;
    size_t licm_instructions_moved;

    /* Register allocation statistics */
    size_t regalloc_total_intervals;
    size_t regalloc_spilled;
    size_t regalloc_cost_based_spills;
    size_t regalloc_max_loop_depth;

    /* General statistics */
    size_t total_instructions_before;
    size_t total_instructions_after;
    size_t optimization_iterations;

    /* Per-pass wall time (Sprint 35 Phase 4): accumulated ms per pass,
     * keyed by index into the opt_pass.c pass table. */
    double pass_ms[16];
    char pass_names[16][32];
    size_t pass_name_count;
} OptProfileStats;

/**
 * Reset all profiling counters
 */
void opt_profile_reset(void);

/**
 * Get current profiling statistics
 */
const OptProfileStats *opt_profile_get_stats(void);

/**
 * Dump profiling statistics to file
 */
void opt_profile_dump(FILE *f);

/**
 * Enable/disable profiling (default: disabled for production)
 */
void opt_profile_enable(int enable);
int opt_profile_is_enabled(void);

/* Record the wall-clock duration of one pass execution (Sprint 35 Phase 4). */
void opt_profile_time_pass(size_t idx, const char *name, double ms);

/* Profiling helper functions (implemented in opt_profile.c) */
void opt_profile_count_cse_instruction(void);
void opt_profile_count_cse_eliminated(void);
void opt_profile_count_cse_basic_block(void);
void opt_profile_count_gvn_instruction(void);
void opt_profile_count_gvn_eliminated(void);
void opt_profile_count_branchless(void);
void opt_profile_count_copy_prop_instruction(void);
void opt_profile_count_copy_prop_changed(void);
void opt_profile_set_copy_prop_changes(size_t changes);
void opt_profile_count_dce_eliminated(void);
void opt_profile_count_dce_iteration(void);
void opt_profile_count_licm_loop(void);
void opt_profile_count_licm_invariant(void);
void opt_profile_count_licm_moved(void);
void opt_profile_count_regalloc_interval(void);
void opt_profile_count_regalloc_spill(int cost_based);
void opt_profile_set_regalloc_max_depth(size_t depth);
void opt_profile_set_instructions_before(size_t count);
void opt_profile_set_instructions_after(size_t count);
void opt_profile_count_iteration(void);

#endif /* ZERO_CC_OPT_PROFILE_H */

/* GVN profiling */
void opt_profile_count_gvn_instruction(void);
void opt_profile_count_gvn_eliminated(void);
