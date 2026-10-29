/* Get Next Token - Main Lexer Function */
#ifndef ZERO_CC_LEXER_NEXT_H
#define ZERO_CC_LEXER_NEXT_H

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "zero_cc/lexer/state.h"
#include "zero_cc/types/token.h"

/* Get next token - main lexer function */
static inline int zero_cc_lexer_next(ZeroCCLexer *lx, ZeroCCToken *out) {
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
        out->is_wide = 1;
        s++; offset++; col++;
        lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
        int ret = is_char ? zero_cc_scan_char(lx, out) : zero_cc_scan_string(lx, out);
        out->src = s - 1;
        out->len++;
        return ret;
    }
    if (s[0] == 'u' && s[1] == '8' && (s[2] == '\'' || s[2] == '"')) {
        int is_char = (s[2] == '\'');
        out->is_wide = 0;
        s += 2; offset += 2; col += 2;
        lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
        int ret = is_char ? zero_cc_scan_char(lx, out) : zero_cc_scan_string(lx, out);
        out->src = s - 2;
        out->len += 2;
        return ret;
    }

    /* Character literal */
    if (*s == '\'') {
        out->is_wide = 0;
        int ret = zero_cc_scan_char(lx, out);
        return ret;
    }

    /* String literal */
    if (*s == '"') {
        out->is_wide = 0;
        int ret = zero_cc_scan_string(lx, out);
        return ret;
    }

    /* Numeric literal */
    if (isdigit((unsigned char)*s) || (*s == '.' && isdigit((unsigned char)s[1]))) {
        int ret = zero_cc_scan_number(lx, out);
        return ret;
    }

    /* Identifier or keyword */
    if (isalpha((unsigned char)*s) || *s == '_') {
        int ret = zero_cc_scan_identifier(lx, out);
        return ret;
    }

    /* Operators and punctuation */
    int ret = zero_cc_scan_operator(lx, out);
    return ret;
}

#endif /* ZERO_CC_LEXER_NEXT_H */
