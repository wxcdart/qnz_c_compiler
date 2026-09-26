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
void zero_cc_lexer_init(ZeroCCLexer *lx, const char *src) {
    if (!lx || !src) return;
    lx->src = src;
    lx->cur = src;
    lx->line = 1;
    lx->col = 1;
    lx->offset = 0;
}

/* Keyword lookup using perfect hash from generated table */
ZeroCCTokType zero_cc_keyword_lookup(const char *s, size_t len) {
    if (!s) return TK_IDENT;
    if (KEYWORD_COUNT <= 0 || KEYWORD_COUNT > 1000) return TK_IDENT;  /* Sanity check */
    for (int i = 0; i < KEYWORD_COUNT; i++) {
        const char *kw = keyword_table[i].name;
        if (!kw) continue;  /* Safety check */
        size_t kw_len = strlen(kw);
        if (kw_len == len && kw[0] == s[0]) {
            if (memcmp(kw, s, len) == 0)
                return keyword_table[i].type;
        }
    }
    return TK_IDENT;
}

/* Debug version of keyword lookup */
ZeroCCTokType zero_cc_keyword_lookup_debug(const char *s, size_t len) {
    if (!s || len == 0) {
        fprintf(stderr, "KEYWORD_LOOKUP: EMPTY STRING! len=%zu\n", len);
        return TK_IDENT;
    }
    fprintf(stderr, "KEYWORD_LOOKUP: '%.*s' (len=%zu)\n", (int)len, s, len);
    if (KEYWORD_COUNT <= 0 || KEYWORD_COUNT > 1000) return TK_IDENT;
    for (int i = 0; i < KEYWORD_COUNT; i++) {
        const char *kw = keyword_table[i].name;
        if (!kw) continue;
        size_t kw_len = strlen(kw);
        if (kw_len == len && kw[0] == s[0]) {
            if (memcmp(kw, s, len) == 0) {
                fprintf(stderr, "  -> FOUND: %d\n", keyword_table[i].type);
                return keyword_table[i].type;
            }
        }
    }
    fprintf(stderr, "  -> NOT FOUND, returning TK_IDENT\n");
    return TK_IDENT;
}

/* Main tokenization function */
void zero_cc_tokenize_c99(const char *src, ZeroCCCompiler *c) {
    if (!src || !c) return;

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
    memset(out, 0, sizeof(*out));
    out->src = s;
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

    /* Try each token type in order of specificity */

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
    tok->type = TK_STRING;

    const char *s = lx->cur;

    while (*s == '"') {
        s++; offset++; col++;  /* skip opening " */
        while (*s && *s != '"') {
            if (*s == '\\' && s[1]) {
                s++; offset++; col++;
                char esc = *s;
                int char_val = 0;
                if (!handle_escape(&s, &offset, NULL, &char_val)) {
                    /* Invalid escape, use the character as-is */
                    char_val = (unsigned char)esc;
                }
                if (sl < sizeof(tok->str_val) - 1) {
                    tok->str_val[sl++] = (char)char_val;
                }
                s++; offset++; col++;
            } else {
                if (sl < sizeof(tok->str_val) - 1) {
                    tok->str_val[sl++] = *s;
                }
                if (*s == '\n') { line++; col = 1; }
                else col++;
                s++; offset++;
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

    tok->str_val[sl] = '\0';
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
        case '0': val = '\0'; break;
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
                    p++; digits++; (*offset)++;
                }
                val = oct;
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

/* Scan a numeric literal (integer or floating point) */
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
        while (*s == '0' || *s == '1') {
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

    /* Try parsing as floating-point first if it contains '.', 'e'/'E', or hex-float exponent 'p'/'P' */
    char *endptr_d = NULL;
    double dv = strtod(s, &endptr_d);

    bool has_fp = false;
    if (endptr_d && endptr_d != s) {
        for (char *q = (char*)s; q < endptr_d; q++) {
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
        s = endptr_d;

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
    char *endptr;
    long long val = strtoll(s, &endptr, 0);
    if (errno == ERANGE) {
        /* Value out of range - clamp to max */
        val = (val < 0) ? LLONG_MIN : LLONG_MAX;
    }
    tok->num_val = val;

    /* Parse integer suffixes (U, L, UL, LL, etc.) */
    uint8_t suf = 0;
    const char *p = endptr;
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