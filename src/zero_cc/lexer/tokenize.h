#ifndef ZERO_CC_LEXER_TOKENIZE_H
#define ZERO_CC_LEXER_TOKENIZE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../lexer.h"


    ZeroCCLexer lx;
    zero_cc_lexer_init(&lx, src);

    size_t cap = 1024;
    c->tokens = (ZeroCCToken *)malloc(cap * sizeof(ZeroCCToken));
    if (!c->tokens) {
        c->had_error = 1;
        snprintf(c->error_msg, sizeof(c->error_msg), "Failed to allocate token buffer");
        return;
    }
    c->token_count = 0;
    c->token_pos = 0;

    while (1) {
        if (c->token_count + 1 >= cap) {
            cap *= 2;
            ZeroCCToken *new_tokens = (ZeroCCToken *)realloc(c->tokens, cap * sizeof(ZeroCCToken));
            if (!new_tokens) {
                c->had_error = 1;
                snprintf(c->error_msg, sizeof(c->error_msg), "Failed to grow token buffer");
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
            c->had_error = 1;
            snprintf(c->error_msg, sizeof(c->error_msg), "Lexer error at line %zu, col %zu", tok->line, tok->col);
            break;
        }

        c->token_count++;
        if (tok->type == TK_EOF) break;
    }
}

void zero_cc_token_free(ZeroCCToken *tok) {
    if (tok && tok->allocated_str) {
        free(tok->allocated_str);
        tok->allocated_str = NULL;
    }
}

void zero_cc_compiler_free_tokens(ZeroCCCompiler *c) {
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

/* Get next token */
int zero_cc_lexer_next(ZeroCCLexer *lx, ZeroCCToken *out) {
    if (!lx || !out || !lx->src || !lx->cur) return -1;

    const char *s = lx->cur;
    size_t line = lx->line;
    size_t col = lx->col;
    size_t offset = lx->offset;

    /* Skip whitespace */
    while (*s && isspace((unsigned char)*s)) {
        if (*s == '\n') { line++; col = 1; }
        else col++;
        s++; offset++;
    }

    lx->cur = s;
    lx->line = line;
    lx->col = col;
    lx->offset = offset;

    if (!*s) {
        /* EOF token */
        memset(out, 0, sizeof(*out));
        out->type = TK_EOF;
        out->line = line;
        out->col = col;
        out->src = s;
        out->len = 0;
        lx->cur = s;
        lx->line = line;
        lx->col = col;
        lx->offset = offset;
        return 1;
    }

    /* Initialize token */

#endif /* ZERO_CC_LEXER_TOKENIZE_H */
