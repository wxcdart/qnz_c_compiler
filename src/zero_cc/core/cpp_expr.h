#ifndef ZERO_CC_CORE_CPP_EXPR_H
#define ZERO_CC_CORE_CPP_EXPR_H

#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Preprocessor #if expression evaluator (C99 6.10.1) */

static inline void zero_cc_cpp_skip_expr_ws(const char **p) {
    while (**p) {
        if (isspace((unsigned char)**p)) {
            (*p)++;
        } else if (**p == '/' && (*p)[1] == '/') {
            *p += 2;
            while (**p && **p != '\n') (*p)++;
        } else if (**p == '/' && (*p)[1] == '*') {
            *p += 2;
            while (**p && !(**p == '*' && (*p)[1] == '/')) (*p)++;
            if (**p) *p += 2;
        } else {
            break;
        }
    }
}

static inline int64_t zero_cc_cpp_eval_comma(const char **p);
static inline int64_t zero_cc_cpp_eval_ternary(const char **p);
static inline int64_t zero_cc_cpp_eval_lor(const char **p);
static inline int64_t zero_cc_cpp_eval_land(const char **p);
static inline int64_t zero_cc_cpp_eval_bor(const char **p);
static inline int64_t zero_cc_cpp_eval_bxor(const char **p);
static inline int64_t zero_cc_cpp_eval_band(const char **p);
static inline int64_t zero_cc_cpp_eval_eq(const char **p);
static inline int64_t zero_cc_cpp_eval_rel(const char **p);
static inline int64_t zero_cc_cpp_eval_shift(const char **p);
static inline int64_t zero_cc_cpp_eval_add(const char **p);
static inline int64_t zero_cc_cpp_eval_mul(const char **p);
static inline int64_t zero_cc_cpp_eval_unary(const char **p);
static inline int64_t zero_cc_cpp_eval_primary(const char **p);

static inline int64_t zero_cc_cpp_eval_primary(const char **p) {
    zero_cc_cpp_skip_expr_ws(p);
    if (!**p) return 0;

    if (**p == '(') {
        (*p)++;
        int64_t val = zero_cc_cpp_eval_comma(p);
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == ')') (*p)++;
        return val;
    }

    /* Character constant: 'x', '\n', L'x', etc. */
    bool is_wide_char = false;
    if ((**p == 'L' || **p == 'u' || **p == 'U') && (*p)[1] == '\'') {
        is_wide_char = true;
        (*p)++;
    }
    if (**p == '\'') {
        (*p)++;
        int64_t val = 0;
        while (**p && **p != '\'') {
            int ch = 0;
            if (**p == '\\') {
                (*p)++;
                char esc = 0;
                if (!zero_cc_handle_escape(p, NULL, &esc, &ch)) {
                    ch = (unsigned char)(**p);
                }
                if (**p) (*p)++;
            } else {
                ch = (unsigned char)(**p);
                (*p)++;
            }
            if (is_wide_char) {
                val = ch;
            } else {
                val = (val << 8) | (ch & 0xFF);
            }
        }
        if (**p == '\'') (*p)++;
        return val;
    }

    /* Integer literal */
    if (isdigit((unsigned char)**p)) {
        char *end = NULL;
        int base = 10;
        if (**p == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
            base = 16;
            *p += 2;
        } else if (**p == '0' && ((*p)[1] == 'b' || (*p)[1] == 'B')) {
            base = 2;
            *p += 2;
        } else if (**p == '0' && isdigit((unsigned char)(*p)[1])) {
            base = 8;
        }
        uint64_t val = strtoull(*p, &end, base);
        if (end && end != *p) {
            *p = end;
            while (**p == 'u' || **p == 'U' || **p == 'l' || **p == 'L') {
                (*p)++;
            }
        }
        return (int64_t)val;
    }

    /* Identifier: remaining identifiers in preprocessor expressions evaluate to 0 */
    if (isalpha((unsigned char)**p) || **p == '_') {
        char id[128];
        size_t l = 0;
        while (**p && (isalnum((unsigned char)**p) || **p == '_')) {
            if (l + 1 < sizeof(id)) id[l++] = **p;
            (*p)++;
        }
        id[l] = '\0';
        if (strcmp(id, "true") == 0) return 1;
        return 0;
    }

    /* Skip unexpected char */
    (*p)++;
    return 0;
}

static inline int64_t zero_cc_cpp_eval_unary(const char **p) {
    zero_cc_cpp_skip_expr_ws(p);
    if (**p == '+') {
        (*p)++;
        return zero_cc_cpp_eval_unary(p);
    }
    if (**p == '-') {
        (*p)++;
        return -zero_cc_cpp_eval_unary(p);
    }
    if (**p == '~') {
        (*p)++;
        return ~zero_cc_cpp_eval_unary(p);
    }
    if (**p == '!') {
        (*p)++;
        return !zero_cc_cpp_eval_unary(p);
    }
    return zero_cc_cpp_eval_primary(p);
}

static inline int64_t zero_cc_cpp_eval_mul(const char **p) {
    int64_t left = zero_cc_cpp_eval_unary(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '*' && (*p)[1] != '=') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_unary(p);
            left = left * right;
        } else if (**p == '/' && (*p)[1] != '/' && (*p)[1] != '*' && (*p)[1] != '=') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_unary(p);
            left = (right != 0) ? (left / right) : 0;
        } else if (**p == '%' && (*p)[1] != '=') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_unary(p);
            left = (right != 0) ? (left % right) : 0;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_add(const char **p) {
    int64_t left = zero_cc_cpp_eval_mul(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '+' && (*p)[1] != '+' && (*p)[1] != '=') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_mul(p);
            left = left + right;
        } else if (**p == '-' && (*p)[1] != '-' && (*p)[1] != '=' && (*p)[1] != '>') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_mul(p);
            left = left - right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_shift(const char **p) {
    int64_t left = zero_cc_cpp_eval_add(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '<' && (*p)[1] == '<') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_add(p);
            left = (right >= 0 && right < 64) ? (left << right) : 0;
        } else if (**p == '>' && (*p)[1] == '>') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_add(p);
            left = (right >= 0 && right < 64) ? (left >> right) : 0;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_rel(const char **p) {
    int64_t left = zero_cc_cpp_eval_shift(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '<' && (*p)[1] == '=') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_shift(p);
            left = (left <= right);
        } else if (**p == '>' && (*p)[1] == '=') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_shift(p);
            left = (left >= right);
        } else if (**p == '<' && (*p)[1] != '<') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_shift(p);
            left = (left < right);
        } else if (**p == '>' && (*p)[1] != '>') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_shift(p);
            left = (left > right);
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_eq(const char **p) {
    int64_t left = zero_cc_cpp_eval_rel(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '=' && (*p)[1] == '=') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_rel(p);
            left = (left == right);
        } else if (**p == '!' && (*p)[1] == '=') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_rel(p);
            left = (left != right);
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_band(const char **p) {
    int64_t left = zero_cc_cpp_eval_eq(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '&' && (*p)[1] != '&') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_eq(p);
            left = left & right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_bxor(const char **p) {
    int64_t left = zero_cc_cpp_eval_band(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '^') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_band(p);
            left = left ^ right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_bor(const char **p) {
    int64_t left = zero_cc_cpp_eval_bxor(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '|' && (*p)[1] != '|') {
            (*p)++;
            int64_t right = zero_cc_cpp_eval_bxor(p);
            left = left | right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_land(const char **p) {
    int64_t left = zero_cc_cpp_eval_bor(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '&' && (*p)[1] == '&') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_bor(p);
            left = (left && right);
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_lor(const char **p) {
    int64_t left = zero_cc_cpp_eval_land(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == '|' && (*p)[1] == '|') {
            *p += 2;
            int64_t right = zero_cc_cpp_eval_land(p);
            left = (left || right);
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t zero_cc_cpp_eval_ternary(const char **p) {
    int64_t cond = zero_cc_cpp_eval_lor(p);
    zero_cc_cpp_skip_expr_ws(p);
    if (**p == '?') {
        (*p)++;
        int64_t then_val = zero_cc_cpp_eval_comma(p);
        zero_cc_cpp_skip_expr_ws(p);
        int64_t else_val = 0;
        if (**p == ':') {
            (*p)++;
            else_val = zero_cc_cpp_eval_ternary(p);
        }
        return cond ? then_val : else_val;
    }
    return cond;
}

static inline int64_t zero_cc_cpp_eval_comma(const char **p) {
    int64_t val = zero_cc_cpp_eval_ternary(p);
    while (1) {
        zero_cc_cpp_skip_expr_ws(p);
        if (**p == ',') {
            (*p)++;
            val = zero_cc_cpp_eval_ternary(p);
        } else {
            break;
        }
    }
    return val;
}

#endif /* ZERO_CC_CORE_CPP_EXPR_H */
