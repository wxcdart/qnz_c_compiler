#ifndef ZERO_CC_PARSER_OPT_MEM2REG_H
#define ZERO_CC_PARSER_OPT_MEM2REG_H

/* Forward declaration */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Local Memory-to-Register Promotion Pass
 *
 * Exploits the SSA-like nature of ZeroCC temps to eliminate
 * unnecessary stack stores and loads.
 *
 * When a local variable is stored to the stack and immediately
 * loaded back in straight-line code, forward the temp directly
 * instead of generating OP_MOV_MEM + OP_LOAD.
 *
 * This is a lightweight optimization that provides significant
 * speedup for typical C code without requiring full SSA construction.
 */

/**
 * Perform local memory-to-register promotion
 *
 * @param c Compiler instance (unused, provided for API consistency)
 */
void opt_promote_mem2reg(ZeroCCCompiler *c);

#endif /* ZERO_CC_PARSER_OPT_MEM2REG_H */
