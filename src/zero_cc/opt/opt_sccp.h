#ifndef ZERO_CC_OPT_SCCP_H
#define ZERO_CC_OPT_SCCP_H

#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Sparse Conditional Constant Propagation (SCCP - Tier 2, Item #8)
 *
 * Implements the classic Wegman-Zadeck algorithm with a 3-level lattice
 * (Top, Constant, Bottom). Simultaneously discovers and propagates constants
 * across basic blocks while optimistically pruning unreachable CFG edges
 * and folding conditional branches.
 */
bool opt_sccp(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_SCCP_H */
