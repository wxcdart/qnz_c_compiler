#ifndef OPT_LOOP_H
#define OPT_LOOP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Loop analysis and optimization framework
 *
 * Provides:
 * - Loop detection in IR
 * - Dominator tree construction
 * - Loop-invariant code motion (LICM)
 * - Strength reduction
 * - Branch simplification
 */

/**
 * Loop structure
 */
typedef struct {
    int header_idx;      /* Index of loop header instruction */
    int preheader_idx;   /* Index of loop preheader (can be -1) */
    int exit_idx;        /* Index of loop exit jump */
    int *body_indices;   /* Indices of instructions in loop body */
    int body_count;
    int capacity;
    bool *is_invariant;  /* Per-instruction invariant flags */
} LoopInfo;

/**
 * Loop analysis state
 */
typedef struct {
    LoopInfo *loops;
    int loop_count;
    int capacity;
    int *dominators;     /* dominators[i] = index of immediate dominator */
    int *post_dominators;
    int ir_count;
} LoopAnalysisState;

/**
 * Initialize loop analysis
 */
void loop_analysis_init(LoopAnalysisState *state);

/**
 * Free loop analysis resources
 */
void loop_analysis_free(LoopAnalysisState *state);

/**
 * Analyze loops in current IR
 */
void loop_analyze(LoopAnalysisState *state);

/**
 * Loop-invariant code motion
 */
void opt_licm(ZeroCCCompiler *c);

/**
 * Strength reduction
 */
void opt_strength_reduction(ZeroCCCompiler *c);

/**
 * Branch simplification
 */
void opt_branch_simplify(ZeroCCCompiler *c);

#endif /* OPT_LOOP_H */
