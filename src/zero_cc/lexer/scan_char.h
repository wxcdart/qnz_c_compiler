#ifndef ZERO_CC_LEXER_SCAN_CHAR_H
#define ZERO_CC_LEXER_SCAN_CHAR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../lexer.h"

    size_t offset = lx->offset;

    const char *s = lx->cur;
    s++; offset++; col++;  /* skip opening ' */

    if (!*s) {
        tok->type = TK_EOF;
        return -1;
    }

    int val = 0;
    if (*s == '\\') {
        s++; offset++; col++;
        if (!*s) {
            tok->type = TK_EOF;
            return -1;
        }
        if (!handle_escape(&s, &offset, tok->str_val, &val)) {
            /* Invalid escape, but we continue */
            val = (unsigned char)*s;
        }
        s++; offset++; col++;
    } else {
        val = (unsigned char)*s;
        s++; offset++; col++;
    }

    if (*s == '\'') {
        s++; offset++; col++;
    } else {
        /* Unterminated character literal - continue anyway */
    }

    tok->type = TK_CHAR_LIT;
    tok->num_val = val;
    tok->len = s - start;
    tok->line = line;
    tok->col = col;

    lx->cur = s;
    lx->line = line;
    lx->col = col;
    lx->offset = offset;
    return 0;
}

/* Scan a string literal */
static int scan_string(ZeroCCLexer *lx, ZeroCCToken *tok) {
    const char *start = lx->cur;
    size_t line = lx->line;
    size_t col = lx->col;
    size_t offset = lx->offset;

    size_t sl = 0;
    size_t cap = sizeof(tok->str_val);
    char *buf = tok->str_val;
    tok->type = TK_STRING;
    tok->allocated_str = NULL;

    const char *s = lx->cur;

    while (*s == '"') {
        s++; offset++; col++;  /* skip opening " */
        while (*s && *s != '"') {
            char esc_ch = 0;
            if (*s == '\\' && s[1]) {
                s++; offset++; col++;
                char esc = *s;
                int char_val = 0;
                if (!handle_escape(&s, &offset, NULL, &char_val)) {
                    char_val = (unsigned char)esc;
                }
                esc_ch = (char)char_val;
                s++; offset++; col++;
            } else {
                esc_ch = *s;
                if (*s == '\n') { line++; col = 1; }
                else col++;
                s++; offset++;
            }

            /* Append char, resizing dynamically if needed */
            if (sl + 1 >= cap) {
                if (buf == tok->str_val) {
                    cap = 256;
                    buf = (char *)malloc(cap);
                    if (buf) {
                        memcpy(buf, tok->str_val, sl);
                        tok->allocated_str = buf;
                    } else {
                        buf = tok->str_val; /* fallback */
                    }
                } else {
                    cap *= 2;
                    char *new_buf = (char *)realloc(buf, cap);
                    if (new_buf) {
                        buf = new_buf;
                        tok->allocated_str = buf;
                    }
                }
            }
            if (buf != tok->str_val || sl < sizeof(tok->str_val) - 1) {
                buf[sl++] = esc_ch;
            }
        }
        if (*s == '"') {
            s++; offset++; col++;  /* skip closing " */
        }
        /* Skip whitespace between string literals (concatenation) */
        while (*s && isspace((unsigned char)*s)) {
            if (*s == '\n') { line++; col = 1; }
            else col++;
            s++; offset++;
        }
    }

    buf[sl] = '\0';
    if (buf != tok->str_val) {
        /* Keep first 127 bytes in str_val for compatibility with functions accessing str_val directly */
        memcpy(tok->str_val, buf, sizeof(tok->str_val) - 1);
        tok->str_val[sizeof(tok->str_val) - 1] = '\0';
    }
    tok->len = s - start;
    tok->line = line;
    tok->col = col;

    lx->cur = s;
    lx->line = line;
    lx->col = col;
    lx->offset = offset;
    return 0;
}

/* Handle escape sequences */
static int handle_escape(const char **s, size_t *offset, char *out_str, int *out_val) {
    if (!s || !*s || !**s) return 0;

    char esc = **s;
    int val = 0;
    int handled = 1;

#endif /* ZERO_CC_LEXER_SCAN_CHAR_H */
