/* Statement Parser - Modular Headers */
#ifndef ZERO_CC_STATEMENTS_H
#define ZERO_CC_STATEMENTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "zero_cc/types_lexer.h"
#include "zero_cc/parser.h"
#include "zero_cc/expressions.h"

/* Forward declaration */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Forward declarations for expression parsing functions used by statements */
void zero_cc_parse_expr(ZeroCCCompiler *c);
int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c);

/* Main statement parser */
void zero_cc_parse_stmt(ZeroCCCompiler *c);

#endif /* ZERO_CC_STATEMENTS_H */
