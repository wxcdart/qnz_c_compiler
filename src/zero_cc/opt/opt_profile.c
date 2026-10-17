#include "opt_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Optimization Profiling Implementation
 */

/* Global profiling state */
static OptProfileStats g_stats;
static int g_profile_enabled = 0;  /* Disabled by default for production */

void opt_profile_reset(void) {
    memset(&g_stats, 0, sizeof(g_stats));
}

const OptProfileStats *opt_profile_get_stats(void) {
    return &g_stats;
}

void opt_profile_enable(int enable) {
    g_profile_enabled = enable;
}

void opt_profile_dump(FILE *f) {
    if (!g_profile_enabled) {
        fprintf(f, "[Profiling disabled]\n");
        return;
    }
    
    fprintf(f, "\n=== Optimization Profile Statistics ===\n");
    
    /* CSE statistics */
    fprintf(f, "\nCSE (Common Subexpression Elimination):\n");
    fprintf(f, "  Total instructions scanned: %zu\n", g_stats.cse_total_instructions);
    fprintf(f, "  Eliminated (replaced with MOV): %zu\n", g_stats.cse_eliminated);
    fprintf(f, "  Basic blocks processed: %zu\n", g_stats.cse_basic_blocks);
    if (g_stats.cse_total_instructions > 0) {
        double elim_rate = (double)g_stats.cse_eliminated / g_stats.cse_total_instructions * 100.0;
        fprintf(f, "  Elimination rate: %.2f%%\n", elim_rate);
    }
    
    /* GVN statistics */
    fprintf(f, "\nGVN (Global Value Numbering):\n");
    fprintf(f, "  Total instructions scanned: %zu\n", g_stats.gvn_total_instructions);
    fprintf(f, "  Eliminated (replaced with MOV): %zu\n", g_stats.gvn_eliminated);
    if (g_stats.gvn_total_instructions > 0) {
        double elim_rate = (double)g_stats.gvn_eliminated / g_stats.gvn_total_instructions * 100.0;
        fprintf(f, "  Elimination rate: %.2f%%\n", elim_rate);
    }
    
    /* Copy propagation statistics */
    fprintf(f, "\nCopy Propagation:\n");
    fprintf(f, "  Total instructions scanned: %zu\n", g_stats.copy_prop_instructions);
    fprintf(f, "  Changes made: %zu\n", g_stats.copy_prop_changes);
    if (g_stats.copy_prop_instructions > 0) {
        double change_rate = (double)g_stats.copy_prop_changes / g_stats.copy_prop_instructions * 100.0;
        fprintf(f, "  Change rate: %.2f%%\n", change_rate);
    }
    
    /* DCE statistics */
    fprintf(f, "\nDCE (Dead Code Elimination):\n");
    fprintf(f, "  Instructions eliminated: %zu\n", g_stats.dce_eliminated);
    fprintf(f, "  Iterations: %zu\n", g_stats.dce_iterations);
    
    /* LICM statistics */
    fprintf(f, "\nLICM (Loop-Invariant Code Motion):\n");
    fprintf(f, "  Loops detected: %zu\n", g_stats.licm_loops_detected);
    fprintf(f, "  Invariants found: %zu\n", g_stats.licm_invariants_found);
    fprintf(f, "  Instructions moved: %zu\n", g_stats.licm_instructions_moved);
    
    /* Register allocation statistics */
    fprintf(f, "\nRegister Allocation:\n");
    fprintf(f, "  Total live intervals: %zu\n", g_stats.regalloc_total_intervals);
    fprintf(f, "  Total spills: %zu\n", g_stats.regalloc_spilled);
    fprintf(f, "  Cost-based spills: %zu\n", g_stats.regalloc_cost_based_spills);
    fprintf(f, "  Max loop depth: %zu\n", g_stats.regalloc_max_loop_depth);
    if (g_stats.regalloc_total_intervals > 0) {
        double spill_rate = (double)g_stats.regalloc_spilled / g_stats.regalloc_total_intervals * 100.0;
        fprintf(f, "  Spill rate: %.2f%%\n", spill_rate);
    }
    
    /* General statistics */
    fprintf(f, "\nGeneral:\n");
    fprintf(f, "  Instructions before optimization: %zu\n", g_stats.total_instructions_before);
    fprintf(f, "  Instructions after optimization: %zu\n", g_stats.total_instructions_after);
    fprintf(f, "  Optimization iterations: %zu\n", g_stats.optimization_iterations);
    if (g_stats.total_instructions_before > 0) {
        double reduction = (double)(g_stats.total_instructions_before - g_stats.total_instructions_after) 
                          / g_stats.total_instructions_before * 100.0;
        fprintf(f, "  Instruction reduction: %.2f%%\n", reduction);
    }
    
    fprintf(f, "\n=======================================\n");
}

/* Inline implementations */
void opt_profile_count_cse_instruction(void) {
    if (g_profile_enabled) g_stats.cse_total_instructions++;
}

void opt_profile_count_cse_eliminated(void) {
    if (g_profile_enabled) g_stats.cse_eliminated++;
}

void opt_profile_count_cse_basic_block(void) {
    if (g_profile_enabled) g_stats.cse_basic_blocks++;
}

void opt_profile_count_dce_eliminated(void) {
    if (g_profile_enabled) g_stats.dce_eliminated++;
}

void opt_profile_count_dce_iteration(void) {
    if (g_profile_enabled) g_stats.dce_iterations++;
}

void opt_profile_count_licm_loop(void) {
    if (g_profile_enabled) g_stats.licm_loops_detected++;
}

void opt_profile_count_licm_invariant(void) {
    if (g_profile_enabled) g_stats.licm_invariants_found++;
}

void opt_profile_count_licm_moved(void) {
    if (g_profile_enabled) g_stats.licm_instructions_moved++;
}

void opt_profile_count_regalloc_interval(void) {
    if (g_profile_enabled) g_stats.regalloc_total_intervals++;
}

void opt_profile_count_regalloc_spill(int cost_based) {
    if (g_profile_enabled) {
        g_stats.regalloc_spilled++;
        if (cost_based) g_stats.regalloc_cost_based_spills++;
    }
}

void opt_profile_set_regalloc_max_depth(size_t depth) {
    if (g_profile_enabled && depth > g_stats.regalloc_max_loop_depth) {
        g_stats.regalloc_max_loop_depth = depth;
    }
}

void opt_profile_set_instructions_before(size_t count) {
    if (g_profile_enabled) g_stats.total_instructions_before = count;
}

void opt_profile_set_instructions_after(size_t count) {
    if (g_profile_enabled) g_stats.total_instructions_after = count;
}

void opt_profile_count_iteration(void) {
    if (g_profile_enabled) g_stats.optimization_iterations++;
}

/* GVN profiling */
void opt_profile_count_gvn_instruction(void) {
    if (g_profile_enabled) g_stats.gvn_total_instructions++;
}
void opt_profile_count_gvn_eliminated(void) {
    if (g_profile_enabled) g_stats.gvn_eliminated++;
}

void opt_profile_print_gvn(void) {
    printf("\nGVN (Global Value Numbering):\n");
    printf("  Total instructions scanned: %zu\n", g_stats.gvn_total_instructions);
    printf("  Eliminated (replaced with MOV): %zu\n", g_stats.gvn_eliminated);
    if (g_stats.gvn_total_instructions > 0) {
        printf("  Elimination rate: %.2f%%\n", (100.0 * g_stats.gvn_eliminated) / g_stats.gvn_total_instructions);
    } else {
        printf("  Elimination rate: 0.00%%\n");
    }
}

/* Copy propagation profiling */
void opt_profile_count_copy_prop_instruction(void) {
    if (g_profile_enabled) g_stats.copy_prop_instructions++;
}
void opt_profile_count_copy_prop_changed(void) {
    if (g_profile_enabled) g_stats.copy_prop_changes++;
}
void opt_profile_set_copy_prop_changes(size_t changes) {
    if (g_profile_enabled) g_stats.copy_prop_changes = changes;
}

void opt_profile_print_copy_prop(void) {
    printf("\nCopy Propagation:\n");
    printf("  Total instructions scanned: %zu\n", g_stats.copy_prop_instructions);
    printf("  Changes made: %zu\n", g_stats.copy_prop_changes);
    if (g_stats.copy_prop_instructions > 0) {
        printf("  Change rate: %.2f%%\n", (100.0 * g_stats.copy_prop_changes) / g_stats.copy_prop_instructions);
    } else {
        printf("  Change rate: 0.00%%\n");
    }
}
