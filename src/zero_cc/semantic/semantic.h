#pragma once

#include <stdbool.h>
#include <stdio.h>
#include "zero_cc/common/arena.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/semantic/diagnostics.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Main entry point for the semantic analysis pass.
 *
 * Initializes the semantic analysis context, traverses the AST starting
 * from root, checks diagnostics, and cleans up the context.
 *
 * @param ast_arena   AST arena owning the node pool
 * @param root        AST reference to the root node (e.g. program/translation unit or statement/expression)
 * @param perm_arena  Permanent arena for types, symbols, and persistent metadata
 * @param tmp_arena   Temporary arena for diagnostics formatting and scratch allocations
 * @param diags       Diagnostic buffer collecting errors and warnings
 * @return            true if analysis completed without errors, false otherwise
 */
bool semantic_analyze(ZeroCCASTArena *ast_arena,
                      ASTRef root,
                      Arena *perm_arena,
                      Arena *tmp_arena,
                      DiagBuf *diags);

/**
 * Helper to print diagnostics formatted to a specific file stream.
 *
 * @param diags Diagnostic buffer to print
 * @param out   Output stream (e.g. stdout or stderr)
 */
void semantic_print_diagnostics(const DiagBuf *diags, FILE *out);

#ifdef __cplusplus
}
#endif
