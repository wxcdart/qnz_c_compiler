#ifndef ZERO_CC_OPT_LOOP_IDIOM_H
#define ZERO_CC_OPT_LOOP_IDIOM_H

#include "zero_cc/core/core.h"

/**
 * Sprint 232: Loop Idiom Recognition & Memcpy/Memset Synthesis (Tier 4, Item #20)
 *
 * Detects countable scalar loops that zero memory (memset) or copy contiguous
 * blocks (memcpy) and synthesizes them into optimized library calls.
 */
void opt_loop_idiom(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_LOOP_IDIOM_H */
