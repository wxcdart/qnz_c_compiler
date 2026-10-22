#ifndef ZERO_CC_PARSER_OPT_CSE_H
#define ZERO_CC_PARSER_OPT_CSE_H

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Eliminate common subexpressions
 *
 * Algorithm:
 * 1. Build hash table of expressions (op + operands)
 * 2. For each expression, check if we've seen it before
 * 3. If yes, replace with copy from existing computation
 */
void opt_eliminate_cse(ZeroCCCompiler *c);

#endif /* ZERO_CC_PARSER_OPT_CSE_H */
