#ifndef ZERO_CC_OPT_LOOP_DELETE_H
#define ZERO_CC_OPT_LOOP_DELETE_H

#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Dead Loop Deletion (Tier 4, Item #20)
 *
 * Detects natural countable loops that perform computations whose results are
 * never used outside the loop and have no observable side effects (no memory
 * stores, no volatile accesses, no external calls).
 *
 * Eliminates the loop completely by replacing the loop header with an
 * unconditional branch to the loop exit and zeroing out the loop body.
 */
bool opt_loop_delete(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_LOOP_DELETE_H */
