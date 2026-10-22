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

/**
 * Flag DCE: Remove CMP instructions whose flags are never used
 *
 * This pass runs BEFORE general DCE and specifically targets flag-setting
 * instructions (OP_CMP, OP_CMP_ZERO) that have no flag-reading successors.
 */
void opt_flag_dce(ZeroCCCompiler *c);

#endif /* ZERO_CC_PARSER_OPT_DCE_H */
