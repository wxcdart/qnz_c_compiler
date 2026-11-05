/* Local-declaration parsing (Sprint 114 Tier B: implementation moved to
 * decl_local.c - this was a 2,200-line header included into the parser.c
 * translation unit). Only the single cross-file entry point is declared
 * here; everything else is TU-local to decl_local.c. */
#ifndef ZERO_CC_PARSER_DECL_LOCAL_H
#define ZERO_CC_PARSER_DECL_LOCAL_H

#include <stdbool.h>
#include <stdint.h>

#include "zero_cc/parser/parser.h"
#include "zero_cc/semantic/decl.h"

bool zero_cc_parse_local_decl(ZeroCCCompiler *c,
                              ZeroCCASTArena *arena,
                              SemContext *ctx,
                              uint32_t line,
                              uint32_t col,
                              ASTRef *out_ref);

#endif /* ZERO_CC_PARSER_DECL_LOCAL_H */
