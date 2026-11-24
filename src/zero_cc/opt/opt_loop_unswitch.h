/*
 * opt_loop_unswitch.h - Loop Unswitching and Loop Peeling Optimization Passes (Sprint 216)
 *
 * Loop Unswitching:
 * Hoists loop-invariant conditional branches outside of loop bodies by cloning
 * the loop into two specialized loops (one for the true branch, one for the false branch),
 * eliminating branch evaluations on every iteration and exposing straight-line loop bodies
 * for downstream vectorization and unrolling.
 *
 * Loop Peeling:
 * Peels the first iteration of a countable loop into the preheader, resolving initial
 * boundary conditions (e.g. i == 0 checks) and aligning data accesses.
 */

#ifndef OPT_LOOP_UNSWITCH_H
#define OPT_LOOP_UNSWITCH_H

typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Loop Unswitching optimization pass */
void opt_loop_unswitch(ZeroCCCompiler *c);

/* Loop Peeling optimization pass */
void opt_loop_peel(ZeroCCCompiler *c);

#endif /* OPT_LOOP_UNSWITCH_H */
