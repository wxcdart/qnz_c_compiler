/* Lexer Initialization */
#ifndef ZERO_CC_LEXER_INIT_H
#define ZERO_CC_LEXER_INIT_H

#include <string.h>
#include "zero_cc/lexer/state.h"
#include "zero_cc/types/token.h"
#include "keywords.inc"

/* Initialize lexer with source and filename */
static inline void zero_cc_lexer_init_file(ZeroCCLexer *lx, const char *src, const char *filename) {
    if (!lx || !src) return;
    lx->src = src;
    lx->cur = src;
    lx->filename = filename;
    lx->line = 1;
    lx->col = 1;
    lx->offset = 0;
    lx->err_msg[0] = '\0';
}

/* Initialize lexer with source only */
static inline void zero_cc_lexer_init(ZeroCCLexer *lx, const char *src) {
    zero_cc_lexer_init_file(lx, src, NULL);
}

/* Keyword lookup using generated table */
static inline ZeroCCTokType zero_cc_keyword_lookup(const char *s, size_t len) {
    if (!s || len == 0) return TK_IDENT;
    for (size_t i = 0; i < KEYWORD_COUNT; i++) {
        const char *kw = keyword_table[i].name;
        if (kw[0] == s[0] && strlen(kw) == len) {
            if (memcmp(kw, s, len) == 0)
                return keyword_table[i].type;
        }
    }
    return TK_IDENT;
}

#endif /* ZERO_CC_LEXER_INIT_H */
