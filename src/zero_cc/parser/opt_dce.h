#ifndef ZERO_CC_PARSER_OPT_DCE_H
#define ZERO_CC_PARSER_OPT_DCE_H

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Eliminate dead code (unused instructions)
 * 
 * Algorithm:
 * 1. Backward pass: compute liveness (which temps are used)
 * 2. Mark instructions whose results are never used
 * 3. Remove dead instructions (except those with side effects)
 */
void opt_eliminate_dead_code(ZeroCCCompiler *c);

#endif /* ZERO_CC_PARSER_OPT_DCE_H */
