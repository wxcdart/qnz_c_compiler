/* Tokenization and Utility Functions */
#ifndef ZERO_CC_LEXER_TOKENIZE_H
#define ZERO_CC_LEXER_TOKENIZE_H

#include "zero_cc/types/symbols.h"
#include "zero_cc/lexer/next.h"

/* Main tokenization function */
static inline void zero_cc_tokenize_c99(const char *src, ZeroCCCompiler *c) {
    if (!src || !c) return;

    ZeroCCLexer lx;
    zero_cc_lexer_init(&lx, src);

    size_t cap = 1024;
    c->tokens = (ZeroCCToken *)malloc(cap * sizeof(ZeroCCToken));
    if (!c->tokens) {
        const char *file = c->diag_buf.default_file ? c->diag_buf.default_file : "<stdin>";
        diag_report_error_file(&c->diag_buf, file, 0, 0, NULL, "Failed to allocate token buffer");
        return;
    }
    c->token_count = 0;
    c->token_pos = 0;

    while (1) {
        if (c->token_count + 1 >= cap) {
            cap *= 2;
            ZeroCCToken *new_tokens = (ZeroCCToken *)realloc(c->tokens, cap * sizeof(ZeroCCToken));
            if (!new_tokens) {
                const char *file = c->diag_buf.default_file ? c->diag_buf.default_file : "<stdin>";
                diag_report_error_file(&c->diag_buf, file, 0, 0, NULL, "Failed to grow token buffer");
                break;
            }
            c->tokens = new_tokens;
        }

        ZeroCCToken *tok = &c->tokens[c->token_count];
        int ret = zero_cc_lexer_next(&lx, tok);
        if (ret == 1) {
            c->token_count++;
            break;  /* EOF */
        }
        if (ret == -1) {      /* Error */
            const char *file = c->diag_buf.default_file ? c->diag_buf.default_file : "<stdin>";
            diag_report_error_file(&c->diag_buf, file, lx.line, lx.col, NULL, "Lexer error");
            break;
        }

        c->token_count++;
        if (tok->type == TK_EOF) break;
    }
}

/* Free token memory */
static inline void zero_cc_token_free(ZeroCCToken *tok) {
    if (tok && tok->allocated_str) {
        free(tok->allocated_str);
        tok->allocated_str = NULL;
    }
}

/* Free all compiler tokens */
static inline void zero_cc_compiler_free_tokens(ZeroCCCompiler *c) {
    if (!c) return;
    if (c->tokens) {
        for (size_t i = 0; i < c->token_count; i++) {
            zero_cc_token_free(&c->tokens[i]);
        }
        free(c->tokens);
        c->tokens = NULL;
    }
    c->token_count = 0;
    c->token_pos = 0;
}

#endif /* ZERO_CC_LEXER_TOKENIZE_H */
