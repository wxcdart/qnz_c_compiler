#ifndef ZERO_CC_PARSER_AST_ARENA_H
#define ZERO_CC_PARSER_AST_ARENA_H

/* AST arena lifecycle & body-audit walkers.
 *
 * The struct definitions (ZeroCCASTArena, ZeroCCASTNode, chunk macros) and
 * the prototypes live in zero_cc/parser/parser.h.  The function DEFINITIONS
 * live in zero_cc/parser/parser.c: they must be defined in exactly one
 * translation unit, because ast_arena.h is included by many modules
 * (parser, jit driver, lowering).  Including this header is safe from any
 * module; it no longer carries definitions. */

#include "zero_cc/types/types.h"
#include "zero_cc/parser/parser.h"

#endif /* ZERO_CC_PARSER_AST_ARENA_H */
