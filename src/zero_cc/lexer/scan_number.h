#ifndef ZERO_CC_LEXER_SCAN_NUMBER_H
#define ZERO_CC_LEXER_SCAN_NUMBER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../lexer.h"

static int scan_number(ZeroCCLexer *lx, ZeroCCToken *tok) {
    const char *start = lx->cur;
    size_t line = lx->line;
    size_t col = lx->col;
    size_t offset = lx->offset;

    const char *s = lx->cur;

    /* Check for binary integer literals: 0b... or 0B... */
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B') && (s[2] == '0' || s[2] == '1')) {
        s += 2; offset += 2; col += 2;
        unsigned long long bval = 0;
        while (*s == '0' || *s == '1' || *s == '\'') {
            if (*s == '\'') {
                s++; offset++; col++;
                continue;
            }
            bval = (bval << 1) | (*s - '0');
            s++; offset++; col++;
        }
        tok->type = TK_NUM;
        tok->num_val = (int64_t)bval;

        /* Parse integer suffixes */
        uint8_t suf = 0;
        while (*s) {
            if (*s == 'u' || *s == 'U') {
                suf |= 1;
                s++; offset++; col++;
            } else if ((*s == 'l' || *s == 'L') && (s[1] == 'l' || s[1] == 'L')) {
                suf |= 4;
                s += 2; offset += 2; col += 2;
            } else if (*s == 'l' || *s == 'L') {
                suf |= 2;
                s++; offset++; col++;
            } else {
                break;
            }
        }
        tok->int_suffix = suf;
        tok->len = s - start;
        tok->line = line;
        tok->col = col;
        lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
        return 0;
    }

    /* Check if the literal contains digit separators (') */
    char clean_buf[128];
    const char *parse_str = s;
    const char *scan_ptr = s;
    bool has_sep = false;

    /* Peek ahead across the numeric literal to check for digit separators */
    bool is_hex = (scan_ptr[0] == '0' && (scan_ptr[1] == 'x' || scan_ptr[1] == 'X'));
    if (is_hex) scan_ptr += 2;
    while (*scan_ptr && (isxdigit((unsigned char)*scan_ptr) || *scan_ptr == '.' ||
           *scan_ptr == 'e' || *scan_ptr == 'E' || *scan_ptr == 'p' || *scan_ptr == 'P' ||
           *scan_ptr == '+' || *scan_ptr == '-' || *scan_ptr == '\'')) {
        if (*scan_ptr == '\'') {
            has_sep = true;
        }
        scan_ptr++;
    }

    if (has_sep) {
        size_t c_idx = 0;
        const char *p = s;
        while (p < scan_ptr && c_idx < sizeof(clean_buf) - 1) {
            if (*p != '\'') {
                clean_buf[c_idx++] = *p;
            }
            p++;
        }
        clean_buf[c_idx] = '\0';
        parse_str = clean_buf;
    }

    /* Try parsing as floating-point first if it contains '.', 'e'/'E', or hex-float exponent 'p'/'P' */
    char *endptr_d = NULL;
    double dv = strtod(parse_str, &endptr_d);

    bool has_fp = false;
    if (endptr_d && endptr_d != parse_str) {
        for (char *q = (char*)parse_str; q < endptr_d; q++) {
            if (*q == '.' || *q == 'e' || *q == 'E' || *q == 'p' || *q == 'P') {
                has_fp = true;
                break;
            }
        }
    }

    if (has_fp) {
        /* Floating point */
        tok->type = TK_FLOAT;
        tok->float_val = dv;
        size_t consumed_clean = endptr_d - parse_str;
        /* Advance in original source skipping matching characters / separators */
        size_t matched = 0;
        while (*s && matched < consumed_clean) {
            if (*s == '\'') {
                s++; offset++; col++;
                continue;
            }
            matched++;
            s++; offset++; col++;
        }

        /* Suffixes: f/F (float), l/L (long double) */
        uint8_t suf = 0;
        if (*s == 'f' || *s == 'F') {
            suf = 1; /* float */
            s++; offset++; col++;
        } else if (*s == 'l' || *s == 'L') {
            suf = 2; /* long double */
            s++; offset++; col++;
        }
        tok->int_suffix = suf;
        tok->len = s - start;
        tok->line = line;
        tok->col = col;
        lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
        return 0;
    }

    /* Integer */
    tok->type = TK_NUM;
    errno = 0;
    char *endptr = NULL;
    long long val = strtoll(parse_str, &endptr, 0);
    if (errno == ERANGE) {
        val = (val < 0) ? LLONG_MIN : LLONG_MAX;
    }
    tok->num_val = val;

    size_t consumed_clean = (endptr && endptr > parse_str) ? (size_t)(endptr - parse_str) : 0;
    size_t matched = 0;
    while (*s && matched < consumed_clean) {
        if (*s == '\'') {
            s++; offset++; col++;
            continue;
        }
        matched++;
        s++; offset++; col++;
    }

    /* Parse integer suffixes (U, L, UL, LL, etc.) */
    uint8_t suf = 0;
    const char *p = s;
    while (*p) {
        if (*p == 'u' || *p == 'U') {
            suf |= 1;
            p++; offset++; col++;
        } else if ((*p == 'l' || *p == 'L') && (p[1] == 'l' || p[1] == 'L')) {
            suf |= 4;
            p += 2; offset += 2; col += 2;
        } else if (*p == 'l' || *p == 'L') {
            suf |= 2;
            p++; offset++; col++;
        } else {
            break;
        }
    }
    tok->int_suffix = suf;
    s = p;
    tok->len = s - start;
    tok->line = line;
    tok->col = col;

    lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
    return 0;
}

/* Scan an identifier or keyword */
static int scan_identifier(ZeroCCLexer *lx, ZeroCCToken *tok) {
    const char *start = lx->cur;
    size_t line = lx->line;
    size_t col = lx->col;
    size_t offset = lx->offset;

    const char *s = lx->cur;
    size_t il = 0;

    while ((isalnum((unsigned char)*s) || *s == '_') && il < sizeof(tok->str_val) - 1) {
        tok->str_val[il++] = *s++;
        offset++; col++;
    }
    tok->str_val[il] = '\0';

    tok->len = s - start;
    tok->line = line;
    tok->col = col;

    /* Use generated keyword lookup table */
    tok->type = zero_cc_keyword_lookup(tok->str_val, il);

    lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
    return 0;
}

/* Scan operators and punctuation */
static int scan_operator(ZeroCCLexer *lx, ZeroCCToken *tok) {
    const char *start = lx->cur;
    size_t line = lx->line;
    size_t col = lx->col;
    size_t offset = lx->offset;

    const char *s = lx->cur;

    /* Three-character operators first */
    if (s[0] == '.' && s[1] == '.' && s[2] == '.') {
        tok->type = TK_ELLIPSIS;
        s += 3; offset += 3; col += 3;
    }
    else if (s[0] == '<' && s[1] == '<' && s[2] == '=') { tok->type = TK_SHL_ASSIGN; s += 3; offset += 3; col += 3; }
    else if (s[0] == '>' && s[1] == '>' && s[2] == '=') { tok->type = TK_SHR_ASSIGN; s += 3; offset += 3; col += 3; }
    /* Two-character operators */
    else if (s[0] == '+' && s[1] == '+') { tok->type = TK_INC; s += 2; offset += 2; col += 2; }
    else if (s[0] == '-' && s[1] == '-') { tok->type = TK_DEC; s += 2; offset += 2; col += 2; }
    else if (s[0] == '+' && s[1] == '=') { tok->type = TK_ADD_ASSIGN; s += 2; offset += 2; col += 2; }
    else if (s[0] == '-' && s[1] == '=') { tok->type = TK_SUB_ASSIGN; s += 2; offset += 2; col += 2; }
    else if (s[0] == '*' && s[1] == '=') { tok->type = TK_MUL_ASSIGN; s += 2; offset += 2; col += 2; }
    else if (s[0] == '/' && s[1] == '=') { tok->type = TK_DIV_ASSIGN; s += 2; offset += 2; col += 2; }
    else if (s[0] == '%' && s[1] == '=') { tok->type = TK_MOD_ASSIGN; s += 2; offset += 2; col += 2; }
    else if (s[0] == '&' && s[1] == '=') { tok->type = TK_AND_ASSIGN; s += 2; offset += 2; col += 2; }
    else if (s[0] == '|' && s[1] == '=') { tok->type = TK_OR_ASSIGN; s += 2; offset += 2; col += 2; }
    else if (s[0] == '^' && s[1] == '=') { tok->type = TK_XOR_ASSIGN; s += 2; offset += 2; col += 2; }
    else if (s[0] == '<' && s[1] == '=') { tok->type = TK_LE; s += 2; offset += 2; col += 2; }
    else if (s[0] == '>' && s[1] == '=') { tok->type = TK_GE; s += 2; offset += 2; col += 2; }
    else if (s[0] == '<' && s[1] == '<') { tok->type = TK_SHL; s += 2; offset += 2; col += 2; }
    else if (s[0] == '>' && s[1] == '>') { tok->type = TK_SHR; s += 2; offset += 2; col += 2; }
    else if (s[0] == '=' && s[1] == '=') { tok->type = TK_EQ; s += 2; offset += 2; col += 2; }
    else if (s[0] == '!' && s[1] == '=') { tok->type = TK_NE; s += 2; offset += 2; col += 2; }
    else if (s[0] == '&' && s[1] == '&') { tok->type = TK_LOGAND; s += 2; offset += 2; col += 2; }
    else if (s[0] == '|' && s[1] == '|') { tok->type = TK_LOGOR; s += 2; offset += 2; col += 2; }
    else if (s[0] == '-' && s[1] == '>') { tok->type = TK_ARROW; s += 2; offset += 2; col += 2; }
    else if (s[0] == '[' && s[1] == '[') { tok->type = TK_LDBRACKET; s += 2; offset += 2; col += 2; }
    else if (s[0] == ']' && s[1] == ']') { tok->type = TK_RDBRACKET; s += 2; offset += 2; col += 2; }
    else if (s[0] == ':' && s[1] == ':') { tok->type = TK_COLONCOLON; s += 2; offset += 2; col += 2; }
    /* Preprocessor operators */
    else if (s[0] == '#' && s[1] == '#') { tok->type = TK_PASTE; s += 2; offset += 2; col += 2; }
    else if (*s == '#') { tok->type = TK_HASH; s++; offset++; col++; }
    /* Single-character tokens */
    else if (*s == ';') { tok->type = TK_SEMI; s++; offset++; col++; }
    else if (*s == ':') { tok->type = TK_COLON; s++; offset++; col++; }
    else if (*s == '{') { tok->type = TK_LBRACE; s++; offset++; col++; }
    else if (*s == '}') { tok->type = TK_RBRACE; s++; offset++; col++; }
    else if (*s == '(') { tok->type = TK_LPAREN; s++; offset++; col++; }
    else if (*s == ')') { tok->type = TK_RPAREN; s++; offset++; col++; }
    else if (*s == '[') { tok->type = TK_LBRACKET; s++; offset++; col++; }
    else if (*s == ']') { tok->type = TK_RBRACKET; s++; offset++; col++; }
    else if (*s == ',') { tok->type = TK_COMMA; s++; offset++; col++; }
    else if (*s == '=') { tok->type = TK_ASSIGN; s++; offset++; col++; }
    else if (*s == '+') { tok->type = TK_PLUS; s++; offset++; col++; }
    else if (*s == '-') { tok->type = TK_MINUS; s++; offset++; col++; }
    else if (*s == '*') { tok->type = TK_STAR; s++; offset++; col++; }
    else if (*s == '/') { tok->type = TK_SLASH; s++; offset++; col++; }
    else if (*s == '%') { tok->type = TK_PERCENT; s++; offset++; col++; }
    else if (*s == '&') { tok->type = TK_AMP; s++; offset++; col++; }
    else if (*s == '|') { tok->type = TK_PIPE; s++; offset++; col++; }
    else if (*s == '^') { tok->type = TK_CARET; s++; offset++; col++; }
    else if (*s == '~') { tok->type = TK_TILDE; s++; offset++; col++; }
    else if (*s == '!') { tok->type = TK_BANG; s++; offset++; col++; }
    else if (*s == '<') { tok->type = TK_LT; s++; offset++; col++; }
    else if (*s == '>') { tok->type = TK_GT; s++; offset++; col++; }
    else if (*s == '.') { tok->type = TK_DOT; s++; offset++; col++; }
    else if (*s == '?') { tok->type = TK_QUESTION; s++; offset++; col++; }
    else {
        /* Unknown character - skip and continue */
        s++; offset++; col++;
        tok->type = TK_EOF;
        tok->len = 0;
        lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
        return -1;
    }

    tok->len = s - start;
    tok->line = line;
    tok->col = col;

    lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
    return 0;

#endif /* ZERO_CC_LEXER_SCAN_NUMBER_H */
