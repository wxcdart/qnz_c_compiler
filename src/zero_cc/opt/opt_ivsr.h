#ifndef ZERO_CC_OPT_IVSR_H
#define ZERO_CC_OPT_IVSR_H

#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Loop Induction Variable Strength Reduction (IVSR - Tier 4, Item #18)
 *
 * Detects array addressing calculations derived from primary loop induction
 * variables (e.g. addr = base + i * scale).
 *
 * Transforms the loop by hoisting initial pointer materialization to the preheader:
 *   ptr = base + i0 * scale
 * Replacing the in-loop scaling and address addition with direct pointer usage:
 *   addr = ptr
 * And incrementing the pointer at the loop latch:
 *   ptr += step * scale
 */
bool opt_ivsr(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_IVSR_H */
