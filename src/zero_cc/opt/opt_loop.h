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
    int *dominators;     /* dominators[i] = index of immediate dominator,
                          * -1 = unreachable from entry.  NON-OWNING view
                          * into the cached CFG (Sprint 149): filled by
                          * loop_analyze / detect_loops, freed by the CFG
                          * cache, never by loop_analysis_free. */
    int *post_dominators; /* post-dominator analogue, same ownership; -1 =
                           * cannot reach any exit. */
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
 * Fill state->dominators / state->post_dominators with non-owning views
 * into the cached CFG's idom / post-idom arrays (Sprint 149: meet-over-
 * predecessors dataflow on the block CFG, see cfg.h).  The views stay
 * valid until the cached CFG is rebuilt (cfg_invalidate) or freed.
 * Returns false on OOM (the views remain NULL).
 */
bool loop_analyze(ZeroCCCompiler *c, LoopAnalysisState *state);

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
