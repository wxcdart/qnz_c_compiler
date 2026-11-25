#ifndef ZERO_CC_OPT_ADCE_H
#define ZERO_CC_OPT_ADCE_H

#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/*
 * Aggressive Dead Code Elimination (ADCE) - Sprint 230
 *
 * Classical reverse-mark worklist algorithm based on Ferrante, Ottenstein & Warren (1987)
 * and Cytron et al. (1991).
 *
 * Unlike standard DCE which assumes instructions are live unless proved dead,
 * ADCE assumes all instructions and branches are DEAD unless proved live from
 * observable roots (memory stores, volatile accesses, function calls, returns).
 *
 * Propagates liveness backwards along data dependencies and control dependencies
 * (via the post-dominator tree from cfg_post_idom). Unneeded conditional branches
 * controlling dead regions/loops are redirected to their immediate post-dominators,
 * allowing entire dead loops and unused induction variables to be deleted without
 * trip-count evaluation.
 */
bool opt_adce(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_ADCE_H */
