/*
 * Loop Fusion / Jamming Optimization Pass (Sprint 215)
 *
 * Merges adjacent loops with identical iteration spaces and compatible data
 * dependencies into a single loop, eliminating redundant loop index increments,
 * comparisons, and branch overhead, and enabling downstream passes (GVN store-to-load
 * forwarding, SLP, and SIMD auto-vectorization) to co-optimize the combined loop body.
 */

#ifndef ZERO_CC_OPT_LOOP_FUSION_H
#define ZERO_CC_OPT_LOOP_FUSION_H

#include "zero_cc/core/core.h"

/* Loop fusion pass registered at -O3 */
void opt_loop_fusion(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_LOOP_FUSION_H */
