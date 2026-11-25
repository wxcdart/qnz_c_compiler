#ifndef ZERO_CC_OPT_DSE_H
#define ZERO_CC_OPT_DSE_H

#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/*
 * Dead Store Elimination (DSE) - Sprint 231
 *
 * Eliminates redundant and dead memory stores:
 * 1. Intra-block and global overwritten stores: If a store to a stack slot or
 *    memory address is followed by another store to the same location without
 *    any intervening read or clobbering call, the earlier store is dead.
 * 2. Dead stores at function exit: If a store writes to a non-address-taken
 *    local stack slot and that slot is not read on any path before function exit
 *    (OP_RET / OP_EPILOGUE), the store is dead and can be eliminated.
 */
bool opt_dse(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_DSE_H */
