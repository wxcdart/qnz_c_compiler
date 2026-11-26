#ifndef ZERO_CC_OPT_MEM2REG_JOIN_H
#define ZERO_CC_OPT_MEM2REG_JOIN_H

#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Join / Diamond-Merging Mem2Reg Promotion (Tier 2, Item #7)
 *
 * Promotes stack slots stored in conditional diamond branches (if-then-else)
 * into a unified virtual register (lowered phi-node) across join blocks.
 */
bool opt_promote_mem2reg_join(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_MEM2REG_JOIN_H */
