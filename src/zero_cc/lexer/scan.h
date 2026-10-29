/* Token Scanning Functions */
#ifndef ZERO_CC_LEXER_SCAN_H
#define ZERO_CC_LEXER_SCAN_H

#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include "zero_cc/lexer/state.h"
#include "zero_cc/types/token.h"

/* Forward declarations for scan functions */
static int zero_cc_scan_number(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_string(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_char(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_identifier(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_operator(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_handle_escape(const char **s, size_t *offset, char *out_str, int *out_val);

#include "zero_cc/lexer/init.h"
#include "zero_cc/lexer/tokenize.h"

/* Handle escape sequences */
static int zero_cc_handle_escape(const char **s, size_t *offset, char *out_str, int *out_val) {
    if (!s || !*s || !**s) return 0;

    char esc = **s;
    int val = 0;
    int handled = 1;

    switch (esc) {
        case 'a': val = '\a'; break;
        case 'b': val = '\b'; break;
        case 'f': val = '\f'; break;
        case 'n': val = '\n'; break;
        case 'r': val = '\r'; break;
        case 't': val = '\t'; break;
        case 'v': val = '\v'; break;
        case '\\': val = '\\'; break;
        case '\'': val = '\''; break;
        case '"': val = '"'; break;
        case '?': val = '?'; break;
        case 'e': case 'E': val = 0x1B; break; /* GNU escape: \e */
        case 'u': {
            /* \uXXXX */
            const char *p = *s + 1;
            int hex = 0, digits = 0;
            while (digits < 4 && isxdigit((unsigned char)*p)) {
                char c = *p++;
                hex = hex * 16 + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
                digits++;
            }
            if (digits == 4) {
                val = hex;
                *offset += digits;
                *s = p - 1;
            } else {
                handled = 0;
                val = (unsigned char)'u';
            }
            break;
        }
        case 'U': {
            /* \UXXXXXXXX */
            const char *p = *s + 1;
            int hex = 0, digits = 0;
            while (digits < 8 && isxdigit((unsigned char)*p)) {
                char c = *p++;
                hex = hex * 16 + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
                digits++;
            }
            if (digits == 8) {
                val = hex;
                *offset += digits;
                *s = p - 1;
            } else {
                handled = 0;
                val = (unsigned char)'U';
            }
            break;
        }
        case 'x': {
            /* Hex escape: \xhh */
            const char *p = *s + 1;
            int hex = 0;
            int digits = 0;
            while (digits < 2 && isxdigit((unsigned char)*p)) {
                char c = *p++;
                hex = hex * 16 + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
                digits++;
            }
            if (digits == 0) {
                handled = 0;
                val = (unsigned char)'x';
            } else {
                val = hex;
                *offset += digits;
                *s = p - 1;
            }
            break;
        }
        default:
            /* Octal escape: \ooo (up to 3 octal digits) */
            if (esc >= '0' && esc <= '7') {
                int oct = 0;
                int digits = 0;
                const char *p = *s;
                while (digits < 3 && *p >= '0' && *p <= '7') {
                    oct = oct * 8 + (*p - '0');
                    p++; digits++;
                }
                val = oct;
                *offset += (digits - 1);
                *s = p - 1;
            } else {
                handled = 0;
                val = (unsigned char)esc;
            }
            break;
    }

    if (out_val) *out_val = val;
    if (out_str && handled) *out_str = (char)val;
    return handled;
}

/* Scan a character literal */
static int zero_cc_scan_char(ZeroCCLexer *lx, ZeroCCToken *tok) {
    const char *start = lx->cur;
    size_t line = lx->line;
    size_t col = lx->col;
    size_t offset = lx->offset;

    const char *s = lx->cur;
    s++; offset++; col++;  /* skip opening ' */

    if (!*s) {
        tok->type = TK_EOF;
        return -1;
    }

    int val = 0;
    while (*s && *s != '\'' && *s != '\n') {
        int ch = 0;
        if (*s == '\\') {
            s++; offset++; col++;
            if (!*s) {
                tok->type = TK_EOF;
                return -1;
            }
            const char *esc_start = s;
            if (!zero_cc_handle_escape(&s, &offset, NULL, &ch)) {
                ch = (unsigned char)*s;
            }
            col += (s - esc_start);
            s++; offset++; col++;
        } else {
            ch = (unsigned char)*s;
            s++; offset++; col++;
        }
        val = (val << 8) | (ch & 0xFF);
    }

    if (*s == '\'') {
        s++; offset++; col++;
    } else {
        /* Unterminated character literal - continue anyway */
    }

    tok->type = TK_CHAR_LIT;
    tok->num_val = (int64_t)val;
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
static int zero_cc_scan_string(ZeroCCLexer *lx, ZeroCCToken *tok) {
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
                const char *esc_start = s;
                if (!zero_cc_handle_escape(&s, &offset, NULL, &char_val)) {
                    char_val = (unsigned char)esc;
                }
                col += (size_t)(s - esc_start);
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
    tok->str_len = sl;
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

/* Scan a numeric literal (integer or floating point) */
static int zero_cc_scan_number(ZeroCCLexer *lx, ZeroCCToken *tok) {
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
        /* Hex literals: 'e'/'E' are ordinary hex DIGITS, not exponent
         * markers.  Per C99 6.4.4.2 a hex literal is a floating
         * constant only when it has a '.' or a p/P exponent; `0x123456789ABCDEFLL`
         * is a long long, not a hex float.  The pre-fix naive scan saw
         * the 'E' digit and took the float path, consuming one 'L' as a
         * long-double suffix and leaving the second 'L' dangling (parse
         * error) or storing a double in an int. */
        bool is_hex_lit = (parse_str[0] == '0' &&
                           (parse_str[1] == 'x' || parse_str[1] == 'X'));
        for (char *q = (char*)parse_str; q < endptr_d; q++) {
            if (*q == '.') {
                has_fp = true;
                break;
            }
            if ((is_hex_lit && (*q == 'p' || *q == 'P')) ||
                (!is_hex_lit && (*q == 'e' || *q == 'E'))) {
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
    uint64_t uval = strtoull(parse_str, &endptr, 0);
    if (errno == ERANGE) {
        bool is_base10 = (parse_str[0] != '0' || (parse_str[1] &&
            parse_str[1] != 'x' && parse_str[1] != 'X' &&
            parse_str[1] != 'b' && parse_str[1] != 'B'));
        if (is_base10) {
            /* Decimal constant beyond unsigned long long: constraint
             * violation (gcc warns); clamp to the full unsigned range. */
            uval = ULLONG_MAX;
        } else {
            /* Hex/octal beyond LLONG_MAX: C 6.4.4.1p6 gives hex/octal
             * constants the unsigned long long domain - accumulate the
             * digits with natural two's-complement wrap at 2^64 instead
             * of saturating to LLONG_MAX (0xF0F0F0F0F0F0F0F0 must keep
             * its bit pattern, not become INT64_MAX). */
            int base = (parse_str[1] == 'x' || parse_str[1] == 'X') ? 16 : 8;
            const char *q = (base == 16) ? parse_str + 2 : parse_str;
            uval = 0;
            while (*q) {
                int d;
                if (*q >= '0' && *q <= '9') d = *q - '0';
                else if (*q >= 'a' && *q <= 'f') d = *q - 'a' + 10;
                else if (*q >= 'A' && *q <= 'F') d = *q - 'A' + 10;
                else break;
                if (d >= base) break;
                uval = uval * (uint64_t)base + (uint64_t)d;
                q++;
            }
        }
    }
    tok->num_val = (int64_t)uval;

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
static int zero_cc_scan_identifier(ZeroCCLexer *lx, ZeroCCToken *tok) {
    const char *start = lx->cur;
    size_t line = lx->line;
    size_t col = lx->col;
    size_t offset = lx->offset;

    const char *s = lx->cur;
    size_t il = 0;

    while (isalnum((unsigned char)*s) || *s == '_') {
        if (il < sizeof(tok->str_val) - 1) {
            tok->str_val[il++] = *s;
        } else {
            il++;
        }
        s++; offset++; col++;
    }
    if (il >= sizeof(tok->str_val)) {
        lx->cur = s; lx->line = line; lx->col = col; lx->offset = offset;
        snprintf(lx->err_msg, sizeof(lx->err_msg), "identifier exceeds maximum length of 127 characters (got %zu)", il);
        return -1;
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
static int zero_cc_scan_operator(ZeroCCLexer *lx, ZeroCCToken *tok) {
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
}

#endif /* ZERO_CC_LEXER_SCAN_H */
