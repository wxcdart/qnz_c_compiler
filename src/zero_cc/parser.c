/* =========================================================================
 *  parser.c
 *  C99 AST Parser and Code Generator
 *  
 *  This file acts as a wrapper that includes the split parser modules.
 *  ========================================================================= */

#include "types_lexer.h"
#include "parser.h"
#include "parser/ast_arena.h"
#include "parser/ast_expr.h"
#include "parser/ast_stmt.h"
#include "parser/ast_dump.h"
#include "parser/ast_lowering.h"
