#ifndef ZERO_CC_LEXER_INIT_H
#define ZERO_CC_LEXER_INIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../types_lexer.h"
#include "../lexer.h"

/* Forward declarations for scan functions */
static int scan_number(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_string(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_char(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_identifier(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_operator(ZeroCCLexer *lx, ZeroCCToken *tok);
static int handle_escape(const char **s, size_t *len, char *out, int *out_val);

/* Lexer implementation for C99 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include "lexer.h"
#include "types_lexer.h"
#include "keywords.inc"

/* Forward declarations */
static int scan_number(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_string(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_char(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_identifier(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_operator(ZeroCCLexer *lx, ZeroCCToken *tok);
static int handle_escape(const char **s, size_t *len, char *out, int *out_val);

/* Initialize lexer */
void zero_cc_lexer_init_file(ZeroCCLexer *lx, const char *src, const char *filename) {
    if (!lx || !src) return;
    lx->src = src;
    lx->cur = src;
    lx->filename = filename;
    lx->line = 1;
    lx->col = 1;
    lx->offset = 0;
}

void zero_cc_lexer_init(ZeroCCLexer *lx, const char *src) {
    zero_cc_lexer_init_file(lx, src, NULL);
}

/* Keyword lookup */
ZeroCCTokType zero_cc_keyword_lookup(const char *s, size_t len) {
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

/* Main tokenization function */
void zero_cc_tokenize_c99(const char *src, ZeroCCCompiler *c) {
    if (!src || !c) return;

#endif /* ZERO_CC_LEXER_INIT_H */
