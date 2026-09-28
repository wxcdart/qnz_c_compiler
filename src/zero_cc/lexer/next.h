#ifndef ZERO_CC_LEXER_NEXT_H
#define ZERO_CC_LEXER_NEXT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../lexer.h"

    memset(out, 0, sizeof(*out));
    out->src = s;
    out->filename = lx->filename;
    out->line = line;
    out->col = col;

    /* Handle comments first */
    if (s[0] == '/' && s[1] == '/') {
        /* Line comment */
        s += 2; offset += 2; col += 2;
        while (*s && *s != '\n') { s++; offset++; col++; }
        lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
        return zero_cc_lexer_next(lx, out);
    }
    if (s[0] == '/' && s[1] == '*') {
        /* Block comment */
        s += 2; offset += 2; col += 2;
        while (*s && !(s[0] == '*' && s[1] == '/')) {
            if (*s == '\n') { line++; col = 1; }
            else col++;
            s++; offset++;
        }
        if (*s) { s += 2; offset += 2; col += 2; }
        lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
        return zero_cc_lexer_next(lx, out);
    }

    /* Check for preprocessor line directives: # <line> ["<file>"] or #line <line> */
    if (*s == '#') {
        const char *p = s + 1;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "line", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
            p += 4;
            while (*p == ' ' || *p == '\t') p++;
        }
        if (isdigit((unsigned char)*p)) {
            char *endp = NULL;
            long new_line = strtol(p, &endp, 10);
            p = endp;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                const char *fn_start = p;
                while (*p && *p != '"' && *p != '\n') p++;
                size_t fn_len = p - fn_start;
                char *fn = (char *)malloc(fn_len + 1);
                if (fn) {
                    memcpy(fn, fn_start, fn_len);
                    fn[fn_len] = '\0';
                    lx->filename = fn;
                }
                if (*p == '"') p++;
            }
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            offset += (p - s);
            line = (new_line > 0) ? (size_t)new_line : line;
            col = 1;
            s = p;
            lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
            return zero_cc_lexer_next(lx, out);
        }
    }

    /* Try each token type in order of specificity */

    /* Prefixed character / string literals: L'...', L"...", u8"...", u"...", U"..." */
    if ((s[0] == 'L' || s[0] == 'u' || s[0] == 'U') && (s[1] == '\'' || s[1] == '"')) {
        int is_char = (s[1] == '\'');
        s++; offset++; col++;
        lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
        int ret = is_char ? scan_char(lx, out) : scan_string(lx, out);
        out->src = s - 1;
        out->len++;
        return ret;
    }
    if (s[0] == 'u' && s[1] == '8' && (s[2] == '\'' || s[2] == '"')) {
        int is_char = (s[2] == '\'');
        s += 2; offset += 2; col += 2;
        lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
        int ret = is_char ? scan_char(lx, out) : scan_string(lx, out);
        out->src = s - 2;
        out->len += 2;
        return ret;
    }

    /* Character literal */
    if (*s == '\'') {
        int ret = scan_char(lx, out);
        return ret;
    }

    /* String literal */
    if (*s == '"') {
        int ret = scan_string(lx, out);
        return ret;
    }

    /* Numeric literal */
    if (isdigit((unsigned char)*s) || (*s == '.' && isdigit((unsigned char)s[1]))) {
        int ret = scan_number(lx, out);
        return ret;
    }

    /* Identifier or keyword */
    if (isalpha((unsigned char)*s) || *s == '_') {
        int ret = scan_identifier(lx, out);
        return ret;
    }

    /* Operators and punctuation */
    int ret = scan_operator(lx, out);
    return ret;
}

/* Scan a character literal */
static int scan_char(ZeroCCLexer *lx, ZeroCCToken *tok) {
    const char *start = lx->cur;
    size_t line = lx->line;
    size_t col = lx->col;

#endif /* ZERO_CC_LEXER_NEXT_H */
