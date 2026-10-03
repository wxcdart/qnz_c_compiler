/* parser/static_assert.h — Static assertion handler
 * Only depends on const_eval.h and types_lexer.h — no machine code emission. */
#ifndef ZERO_CC_PARSER_STATIC_ASSERT_H
#define ZERO_CC_PARSER_STATIC_ASSERT_H

#include "zero_cc/types/types.h"
#include "zero_cc/parser/const_eval.h"

/* Handle _Static_assert(expr, "message"); */
static inline void zero_cc_stmt_static_assert(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_STATIC_ASSERT */
    if (c->tokens[c->token_pos].type != TK_LPAREN) {
        diag_report_error_at_token(c, "Expected '(' after _Static_assert");
        return;
    }
    c->token_pos++; /* skip '(' */
    int64_t val = zero_cc_eval_const_expr(c);
    if (c->tokens[c->token_pos].type == TK_COMMA) {
        c->token_pos++; /* skip ',' */
        if (c->tokens[c->token_pos].type == TK_STRING) c->token_pos++; /* skip message */
    }
    if (c->tokens[c->token_pos].type != TK_RPAREN) {
        diag_report_error_at_token(c, "Expected ')' after constant expression");
        return;
    }
    c->token_pos++; /* skip ')' */
    if (c->tokens[c->token_pos].type != TK_SEMI) {
        diag_report_error_at_token(c, "Expected ';' after _Static_assert");
        return;
    }
    c->token_pos++; /* consume ';' */
    if (val == 0) {
        diag_report_error_at_token(c, "Static assertion failed");
    }
}

#endif /* ZERO_CC_STATEMENTS_STATIC_ASSERT_H */
