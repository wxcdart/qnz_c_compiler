#ifndef ZERO_CC_PARSER_OPT_CONSTANT_FOLD_H
#define ZERO_CC_PARSER_OPT_CONSTANT_FOLD_H

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Fold constant expressions in IR
 *
 * Performs:
 * - Constant arithmetic (2 + 3 → 5)
 * - Algebraic simplification (x + 0 → x, x * 1 → x)
 * - Constant propagation
 */
void opt_fold_constants(ZeroCCCompiler *c);

/**
 * Check if a temp holds a constant value
 */
int ir_is_constant(int temp);

/**
 * Get constant value from temp
 */
int ir_get_constant(int temp);

/**
 * Replace instruction with constant
 */
void ir_replace_with_const(int instr_idx, int value);

/**
 * Replace instruction with MOV from another temp
 */
void ir_replace_with_mov(int instr_idx, int src_temp);

#endif /* ZERO_CC_PARSER_OPT_CONSTANT_FOLD_H */
