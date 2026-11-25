/*
 * Loop Collapsing & Outer Loop Auto-Vectorization (Sprint 222)
 *
 * Collapses perfectly nested countable loops (e.g. for i = 0..N, for j = 0..M)
 * into a single flat loop (k = 0..N*M) with affine unit-stride access.
 * Enables SIMD auto-vectorization across outer loop boundaries when inner
 * trip counts are small or non-multiple of vector widths.
 */

#ifndef ZERO_CC_OPT_LOOP_COLLAPSE_H
#define ZERO_CC_OPT_LOOP_COLLAPSE_H

#include "zero_cc/core/core.h"

/* Loop collapse optimization pass registered at -O3 */
void opt_loop_collapse(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_LOOP_COLLAPSE_H */
