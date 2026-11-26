#ifndef ZERO_CC_PARSER_TYPE_PARSER_H
#define ZERO_CC_PARSER_TYPE_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/types/types.h"
#include "zero_cc/types/registry.h"
#include "zero_cc/types/global_vars.h"

/* Forward declarations for constant-evaluation and C23 attribute helpers */
int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos);
int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos);
int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c);
void skip_c23_attributes(ZeroCCCompiler *c);
void skip_c23_attributes_at(ZeroCCCompiler *c, size_t *pos);
int zero_cc_parse_aligned_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_align);
int zero_cc_parse_vector_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_size);
int zero_cc_parse_mode_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_bytes);

/* Apply a GNU `mode(X)` integer-width attribute (marker byte count) to a
 * base integer type, preserving signedness (c-torture execute/memclr:
 * `typedef unsigned int __attribute__((mode(QI))) int08_t;`). Non-integer
 * bases and unsupported widths (TI) are returned unchanged. */
static inline ZeroCCType zero_cc_type_apply_mode(ZeroCCType ty, size_t mode_bytes) {
    bool uns = (ty.tag == TYPE_UINT8 || ty.tag == TYPE_UINT16 ||
                ty.tag == TYPE_UINT32 || ty.tag == TYPE_UINT64);
    bool is_int = uns || ty.tag == TYPE_INT8 || ty.tag == TYPE_INT16 ||
                  ty.tag == TYPE_INT32 || ty.tag == TYPE_INT64;
    if (!is_int) return ty;
    switch (mode_bytes) {
        case 1:  return uns ? zero_cc_type_uint8()  : zero_cc_type_char();
        case 2:  return uns ? zero_cc_type_uint16() : zero_cc_type_int16();
        case 4:  return uns ? zero_cc_type_uint32() : zero_cc_type_int();
        case 8:  return uns ? zero_cc_type_uint64() : zero_cc_type_int64();
        default: return ty; /* TI and unknown: leave as-is */
    }
}
bool zero_cc_tok_is_type_start(ZeroCCCompiler *c, size_t pos);

static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos);

static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name);
/* C23 6.2.5: fixed underlying type for enums (`enum e : unsigned int`).
 * Parsed immediately after the enum tag (or after `enum` for anonymous
 * enums), i.e. the `: <type>` between the tag and the body.
 * size == 0 means "no fixed type" (traditional enum, int-sized). */
typedef struct {
    size_t size;        /* 1, 2, 4, 8; 0 = absent */
    bool is_bool;       /* underlying type is _Bool */
    bool is_unsigned;   /* underlying type is unsigned (wraps modulo 2^bits) */
} ZCEnumUnderlying;

/* Parse `: <fixed-type>` if present at c->token_pos (consuming the colon).
 * Accepts the ordinary integer type specifiers in any order
 * (_Bool|bool, char, short, int, long [long], signed/unsigned, with
 * const/volatile qualifiers); diagnoses anything else. */
static inline ZCEnumUnderlying zero_cc_parse_enum_underlying(ZeroCCCompiler *c) {
    ZCEnumUnderlying u; memset(&u, 0, sizeof(u));
    if (c->token_pos >= c->token_count ||
        c->tokens[c->token_pos].type != TK_COLON) return u;
    c->token_pos++;
    skip_c23_attributes(c);
    bool is_bool = false, is_char = false, is_short = false;
    int long_count = 0;
    int signedness = 0; /* 0 none, +1 signed, -1 unsigned */
    bool saw_any = false;
    while (c->token_pos < c->token_count) {
        ZeroCCTokType t = c->tokens[c->token_pos].type;
        const char *s = c->tokens[c->token_pos].str_val;
        if (t == TK_BOOL_KW || (t == TK_IDENT && strcmp(s, "bool") == 0)) {
            is_bool = true; saw_any = true; c->token_pos++;
        } else if (t == TK_CHAR) {
            is_char = true; saw_any = true; c->token_pos++;
        } else if (t == TK_SHORT) {
            is_short = true; saw_any = true; c->token_pos++;
        } else if (t == TK_INT) {
            saw_any = true; c->token_pos++;
        } else if (t == TK_LONG) {
            long_count++; saw_any = true; c->token_pos++;
        } else if (t == TK_SIGNED) {
            if (signedness == -1)
                diag_report_error_at_token(c, "conflicting signedness in enum underlying type");
            signedness = 1; saw_any = true; c->token_pos++;
        } else if (t == TK_UNSIGNED) {
            if (signedness == 1)
                diag_report_error_at_token(c, "conflicting signedness in enum underlying type");
            signedness = -1; saw_any = true; c->token_pos++;
        } else if (t == TK_CONST || t == TK_VOLATILE) {
            c->token_pos++; /* qualifier: accepted, no effect on width */
        } else if (t == TK_IDENT) {
            if (strcmp(s, "uint8_t") == 0) {
                is_char = true; signedness = -1; saw_any = true; c->token_pos++; break;
            } else if (strcmp(s, "int8_t") == 0) {
                is_char = true; signedness = 1; saw_any = true; c->token_pos++; break;
            } else if (strcmp(s, "uint16_t") == 0) {
                is_short = true; signedness = -1; saw_any = true; c->token_pos++; break;
            } else if (strcmp(s, "int16_t") == 0) {
                is_short = true; signedness = 1; saw_any = true; c->token_pos++; break;
            } else if (strcmp(s, "uint32_t") == 0) {
                signedness = -1; saw_any = true; c->token_pos++; break;
            } else if (strcmp(s, "int32_t") == 0) {
                signedness = 1; saw_any = true; c->token_pos++; break;
            } else if (strcmp(s, "uint64_t") == 0 || strcmp(s, "size_t") == 0 || strcmp(s, "uintptr_t") == 0) {
                long_count = 1; signedness = -1; saw_any = true; c->token_pos++; break;
            } else if (strcmp(s, "int64_t") == 0 || strcmp(s, "ptrdiff_t") == 0 || strcmp(s, "intptr_t") == 0) {
                long_count = 1; signedness = 1; saw_any = true; c->token_pos++; break;
            } else {
                ZeroCCType *td = zero_cc_find_typedef(c, s);
                if (td) {
                    if (td->tag == TYPE_BOOL) {
                        is_bool = true; saw_any = true; c->token_pos++; break;
                    } else if (td->tag == TYPE_INT8 || td->tag == TYPE_UINT8) {
                        is_char = true;
                        signedness = (td->tag == TYPE_UINT8) ? -1 : 1;
                        saw_any = true; c->token_pos++; break;
                    } else if (td->tag == TYPE_INT16 || td->tag == TYPE_UINT16) {
                        is_short = true;
                        signedness = (td->tag == TYPE_UINT16) ? -1 : 1;
                        saw_any = true; c->token_pos++; break;
                    } else if (td->tag == TYPE_INT32 || td->tag == TYPE_UINT32) {
                        signedness = (td->tag == TYPE_UINT32) ? -1 : 1;
                        saw_any = true; c->token_pos++; break;
                    } else if (td->tag == TYPE_INT64 || td->tag == TYPE_UINT64) {
                        long_count = 1;
                        signedness = (td->tag == TYPE_UINT64) ? -1 : 1;
                        saw_any = true; c->token_pos++; break;
                    } else if (td->size > 0 && td->size <= 8 && td->tag != TYPE_STRUCT &&
                               td->tag != TYPE_UNION && td->tag != TYPE_FLOAT && td->tag != TYPE_DOUBLE) {
                        if (td->size == 1) is_char = true;
                        else if (td->size == 2) is_short = true;
                        else if (td->size == 8) long_count = 1;
                        saw_any = true; c->token_pos++; break;
                    }
                }
            }
            break;
        } else {
            break;
        }
    }
    if (!saw_any) {
        diag_report_error_at_token(c, "expected fixed integer type in enum declaration");
        return u;
    }
    skip_c23_attributes(c);
    if (is_bool) {
        u.size = 1; u.is_bool = true; return u;
    }
    u.is_unsigned = (signedness == -1);
    /* Width: long [long] > char > short > int (bare signed/unsigned = int). */
    if (long_count >= 1) u.size = 8;
    else if (is_char) u.size = 1;
    else if (is_short) u.size = 2;
    else u.size = 4;
    return u;
}

/* Convert a member value to the fixed underlying type (C23 6.7.2.2:
 * out-of-range values are implementation-defined; both GCC and ZeroCC
 * wrap two's-complement / modulo). */
static inline int64_t zc_enum_wrap_value(int64_t v, const ZCEnumUnderlying *u) {
    if (!u || u->size == 0) return v;
    if (u->is_bool) return v != 0;
    if (u->is_unsigned) {
        if (u->size == 1) return (int64_t)((uint64_t)v & 0xFFu);
        if (u->size == 2) return (int64_t)((uint64_t)v & 0xFFFFu);
        if (u->size == 4) return (int64_t)((uint64_t)v & 0xFFFFFFFFu);
        return v;
    }
    if (u->size == 1) return (int64_t)(int8_t)((uint8_t)v);
    if (u->size == 2) return (int64_t)(int16_t)((uint16_t)v);
    if (u->size == 4) return (int64_t)(int32_t)((uint32_t)v);
    return v;
}

/* Apply the fixed underlying type to an enum ZeroCCType (size + scalar
 * alignment); a traditional enum stays 4/4. */
static inline void zc_enum_apply_underlying(ZeroCCType *et, const ZCEnumUnderlying *u) {
    if (u && u->size) {
        et->size = u->size;
        et->align = u->size; /* scalar int alignment == size on all targets */
        et->is_fixed_enum = true;
    }
}

/* Parse one enum body `c->token_pos` at the first member (LBRACE consumed);
 * consumes through RBRACE. M1: two fixes vs the old per-site loops —
 *   (1) `false`/`true` are accepted as member names: in C they are plain
 *       identifiers unless <stdbool.h> is included (TK_FALSE/TK_TRUE tokens
 *       carried str_val; the old TK_IDENT-only check stalled the loop on
 *       `enum { false, true }` — compile/pr94238.c); and
 *   (2) a progress guard: any token that neither member nor comma parsing
 *       consumes is diagnosed and skipped, so a malformed body
 *       (e.g. `enum { 1, 2 }`) can never spin the parser forever.
 * `und` (C23 fixed underlying type, NULL = traditional) wraps each member
 * value into the fixed width. */
static inline void zero_cc_parse_enum_members(ZeroCCCompiler *c,
                                              const ZCEnumUnderlying *und) {
    int64_t enum_val = 0;
    while (c->token_pos < c->token_count &&
           c->tokens[c->token_pos].type != TK_RBRACE &&
           c->tokens[c->token_pos].type != TK_EOF) {
        skip_c23_attributes(c);
        size_t saved = c->token_pos;
        if (c->tokens[c->token_pos].type == TK_IDENT ||
            (c->tokens[c->token_pos].type == TK_FALSE &&
             c->std_dialect != ZC_STD_C23 && c->std_dialect != ZC_STD_GNU23) ||
            (c->tokens[c->token_pos].type == TK_TRUE &&
             c->std_dialect != ZC_STD_C23 && c->std_dialect != ZC_STD_GNU23)) {
            char ename[128];
            snprintf(ename, sizeof(ename), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            skip_c23_attributes(c);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                c->token_pos++;
                enum_val = zero_cc_eval_const_expr(c);
            }
            skip_c23_attributes(c);
            enum_val = zc_enum_wrap_value(enum_val, und);
            zero_cc_register_enum_const(c, ename, enum_val);
            if (enum_val == INT64_MAX) {
                diag_report_error_at_token(c, "overflow in enumeration values");
            } else {
                enum_val++;
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        if (c->token_pos == saved) {
            diag_report_error_at_token(c, "expected identifier in enum definition");
            c->token_pos++;
        }
    }
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
}

static inline ZeroCCType zero_cc_parse_type_name_at_pos(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_parse_type_name(ZeroCCCompiler *c);
static inline ZeroCCType zc_parse_struct_or_union_spec(ZeroCCCompiler *c, size_t *pos, ZeroCCTokType kw);
static inline ZeroCCType zc_parse_enum_spec(ZeroCCCompiler *c, size_t *pos);
static inline bool zero_cc_generic_select_span(ZeroCCCompiler *c, size_t *pos,
                                               size_t *out_sel_pos, size_t *out_end_pos);
static inline ZeroCCType zc_type_from_function(const ZeroCCFunction *fn);

static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_relational(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_EQ || op == TK_NE) {
            (*pos)++;
            zero_cc_type_of_relational(c, pos);
            left = zero_cc_type_int(); /* comparison yields int */
        } else {
            break;
        }
    }
    /* C99 6.5.15: conditional expression in a type context
     * (`sizeof ((c ? a : b).c)`).  Both operands must be compatible;
     * the result type is that of the (struct/union) operands. */
    if (*pos < c->token_count && c->tokens[*pos].type == TK_QUESTION) {
        (*pos)++;
        ZeroCCType t = zero_cc_type_of_equality(c, pos);
        if (*pos < c->token_count && c->tokens[*pos].type == TK_COLON) (*pos)++;
        ZeroCCType f = zero_cc_type_of_equality(c, pos);
        if ((t.tag == TYPE_STRUCT && f.tag == TYPE_STRUCT) ||
            (t.tag == TYPE_UNION && f.tag == TYPE_UNION)) {
            left = t;
        } else if (t.tag == f.tag) {
            left = t;
        } else {
            left = zero_cc_type_int();
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_additive(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE) {
            (*pos)++;
            zero_cc_type_of_additive(c, pos);
            left = zero_cc_type_int();
        } else {
            break;
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_multiplicative(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_PLUS || op == TK_MINUS) {
            (*pos)++;
            ZeroCCType right = zero_cc_type_of_multiplicative(c, pos);
            /* Pointer arithmetic: if left is ptr and right is int, result is ptr */
            if (left.tag == TYPE_PTR && (right.tag == TYPE_INT32 || right.tag == TYPE_INT64 ||
                right.tag == TYPE_UINT32 || right.tag == TYPE_UINT64)) {
                /* result is same pointer type */
            } else {
                left = zero_cc_type_int();
            }
        } else {
            break;
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_unary(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_STAR || op == TK_SLASH || op == TK_PERCENT) {
            (*pos)++;
            zero_cc_type_of_unary(c, pos);
            left = zero_cc_type_int();
        } else {
            break;
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_INC || t == TK_DEC) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_PLUS) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_MINUS) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_TILDE) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_BANG) {
        (*pos)++;
        return zero_cc_type_int();
    }
    if (t == TK_SIZEOF) {
        /* sizeof expression is size_t (uint64_t) */
        eval_sizeof(c, pos);
        return zero_cc_type_uint64();
    }
    if (t == TK_ALIGNOF) {
        eval_alignof(c, pos);
        return zero_cc_type_uint64();
    }
    if (t == TK_STAR) { /* dereference */
        (*pos)++;
        ZeroCCType operand = zero_cc_type_of_unary(c, pos);
        if ((operand.tag == TYPE_PTR || operand.tag == TYPE_ARRAY) && operand.base) {
            return *operand.base;
        }
        return operand;
    }
    if (t == TK_AMP) { /* address-of */
        (*pos)++;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
            const char *aname = c->tokens[*pos].str_val;
            if (!zero_cc_find_local(c, aname) && !find_global_var(c, aname) &&
                !zero_cc_find_tls(c, aname)) {
                ZeroCCFunction *fn = zero_cc_find_function(c, aname);
                if (fn) {
                    (*pos)++;
                    return zc_type_from_function(fn);
                }
            }
        }
        ZeroCCType operand = zero_cc_type_of_unary(c, pos);
        return zero_cc_type_ptr(&operand);
    }
    return zero_cc_type_of_primary(c, pos);
}

static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_NUM) {
        /* C99 6.4.4.1p6: the suffix (bit0=u, bit1=l, bit2=ll) fixes the
         * literal's type and an unsuffixed constant takes the first type in
         * which its value fits, so sizeof(0x12345678ABCDEF0LLU) is 8, not
         * sizeof(int). */
        uint8_t suf = c->tokens[*pos].int_suffix;
        int64_t v = c->tokens[*pos].num_val;
        (*pos)++;
        if (suf & 16) return (suf & 0x01) ? zero_cc_type_uint64() : zero_cc_type_int64();
        if (suf & 0x04) return (suf & 0x01) ? zero_cc_type_uint64() : zero_cc_type_int64();
        if (suf & 0x02) {
            if (zc_target_is_32bit(c)) return (suf & 0x01) ? zero_cc_type_uint32() : zero_cc_type_int();
            return (suf & 0x01) ? zero_cc_type_uint64() : zero_cc_type_int64();
        }
        if (suf & 0x01) return (v > (int64_t)0xFFFFFFFF) ? zero_cc_type_uint64() : zero_cc_type_uint32();
        if (v < 0) return zero_cc_type_uint64(); /* wrapped 64-bit hex literal */
        if (v > (int64_t)0xFFFFFFFF) return zero_cc_type_int64();
        if (v > INT32_MAX) return zero_cc_type_uint32();
        return zero_cc_type_int();
    }
    if (t == TK_FLOAT) {
        uint8_t suf = c->tokens[*pos].int_suffix;
        (*pos)++;
        if (suf == 1) return zero_cc_type_float();
        return zero_cc_type_double();
    }
    if (t == TK_CHAR_LIT) {
        (*pos)++;
        return zero_cc_type_int();
    }
    if (t == TK_TRUE || t == TK_FALSE) {
        (*pos)++;
        return zero_cc_type_bool(); /* _Bool */
    }
    if (t == TK_NULLPTR) {
        (*pos)++;
        return zero_cc_type_nullptr();
    }
    if (t == TK_STRING) {
        bool is_wide = c->tokens[*pos].is_wide;
        size_t slen = zero_cc_token_str_len(&c->tokens[*pos]);
        (*pos)++;
        ZeroCCType ty;
        if (is_wide) {
            ZeroCCType elem = zero_cc_type_int();
            size_t num_elems = (slen / 4) + 1;
            ty = zero_cc_type_array(&elem, (ssize_t)num_elems);
        } else {
            size_t len = slen + 1;
            ZeroCCType elem = zero_cc_type_char();
            ty = zero_cc_type_array(&elem, (ssize_t)len);
        }
        /* Postfix subscripts on a string literal (`""[__x]` - the glibc
         * ctype.h element-size idiom; c-torture compile/split-path-5):
         * consume the index chain and peel to the element type, exactly
         * like the identifier-primary postfix loop. */
        while (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
            (*pos)++;
            zero_cc_type_of_equality(c, pos); /* consume index */
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
            if (ty.tag == TYPE_ARRAY && ty.base) {
                ty = *ty.base;
            }
        }
        return ty;
    }
    /* Type keywords as primary (for sizeof(type)/_Alignof(type)) */
    if (t == TK_STRUCT || t == TK_UNION) {
        return zc_parse_struct_or_union_spec(c, pos, t);
    } else if (t == TK_ENUM) {
        return zc_parse_enum_spec(c, pos);
    }
    if (t == TK_GENERIC) {
        size_t sel_pos = 0, end_pos = 0;
        if (zero_cc_generic_select_span(c, pos, &sel_pos, &end_pos)) {
            ZeroCCType gty = zero_cc_type_of_equality(c, &sel_pos);
            *pos = end_pos;
            return gty;
        }
        return zero_cc_type_int();
    }
    if (t == TK_INT) { (*pos)++; return zero_cc_type_int(); }
    if (t == TK_CHAR) { (*pos)++; return zero_cc_type_char(); }
    if (t == TK_VOID) { (*pos)++; return zero_cc_type_void(); }
    if (t == TK_FLOAT_KW) { (*pos)++; return zero_cc_type_float(); }
    if (t == TK_DOUBLE_KW) { (*pos)++; return zero_cc_type_double(); }
    if (t == TK_BOOL_KW) { (*pos)++; return zero_cc_type_bool(); }
    if (t == TK_TYPEOF || t == TK_TYPEOF_UNQUAL) {
        return zero_cc_parse_type_name_at_pos(c, pos);
    }
    if (t == TK_SHORT || t == TK_LONG || t == TK_SIGNED || t == TK_UNSIGNED) {
        /* Multi-word types (long long, unsigned long, signed short, ...) */
        return zero_cc_parse_type_name_at_pos(c, pos);
    }
    if (t == TK_ATOMIC) { (*pos)++; if (c->tokens[*pos].type == TK_LPAREN) { (*pos)++; } ZeroCCType ty = zero_cc_type_of_primary(c, pos); if (c->tokens[*pos].type == TK_RPAREN) (*pos)++; ty.is_atomic = true; return ty; }
    if (t == TK_LPAREN) {
        (*pos)++;
        /* Could be cast, compound literal, or parenthesized expression */
        if (*pos < c->token_count && zero_cc_tok_is_type_start(c, *pos)) {
            /* Cast: (type) expr, or Compound Literal: (type){ ... } */
            ZeroCCType cast_ty = zero_cc_parse_type_name_at_pos(c, pos);
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACE) {
                int bdepth = 1;
                (*pos)++;
                while (*pos < c->token_count && bdepth > 0) {
                    if (c->tokens[*pos].type == TK_LBRACE) bdepth++;
                    else if (c->tokens[*pos].type == TK_RBRACE) bdepth--;
                    (*pos)++;
                }
                while (*pos < c->token_count) {
                    ZeroCCTokType pt = c->tokens[*pos].type;
                    if (pt == TK_DOT || pt == TK_ARROW) {
                        (*pos)++;
                        if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
                            const char *mem_name = c->tokens[*pos].str_val;
                            (*pos)++;
                            cast_ty = zero_cc_type_find_member(c, cast_ty, mem_name);
                        }
                        continue;
                    }
                    if (pt == TK_LBRACKET) {
                        (*pos)++;
                        zero_cc_type_of_equality(c, pos);
                        if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                        if ((cast_ty.tag == TYPE_ARRAY || cast_ty.tag == TYPE_PTR) && cast_ty.base) {
                            cast_ty = *cast_ty.base;
                        }
                        continue;
                    }
                    break;
                }
            } else {
                ZeroCCType expr_ty = zero_cc_type_of_unary(c, pos);
                (void)expr_ty; /* ignore - cast result is cast_ty */
            }
            return cast_ty;
        }
        /* Parenthesized expression */
        ZeroCCType ty = zero_cc_type_of_equality(c, pos);
        if (c->tokens[*pos].type == TK_RPAREN) (*pos)++;
        /* Postfix after the closing paren: `++/--` and member access
         * (`((c ? a : b).c)` - c-torture compile/20011106-2). */
        while (*pos < c->token_count) {
            ZeroCCTokType pt = c->tokens[*pos].type;
            if (pt == TK_INC || pt == TK_DEC) { (*pos)++; continue; }
            if (pt == TK_DOT || pt == TK_ARROW) {
                (*pos)++;
                if (c->tokens[*pos].type == TK_IDENT) {
                    const char *mem_name = c->tokens[*pos].str_val;
                    (*pos)++;
                    ty = zero_cc_type_find_member(c, ty, mem_name);
                }
                continue;
            }
            break;
        }
        return ty;
    }
    if (t == TK_IDENT) {
        const char *name = c->tokens[*pos].str_val;
        (*pos)++;
        /* Check for function call: func(...) */
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
            (*pos)++;
            int pdepth = 1;
            while (*pos < c->token_count && pdepth > 0) {
                if (c->tokens[*pos].type == TK_LPAREN) pdepth++;
                else if (c->tokens[*pos].type == TK_RPAREN) pdepth--;
                (*pos)++;
            }
            ZeroCCFunction *fn = zero_cc_find_function(c, name);
            if (fn) return fn->return_type;
            return zero_cc_type_int();
        }
        /* Check for local variable */
        ZeroCCSym *sym = zero_cc_find_local(c, name);
        if (sym) {
            ZeroCCType ty = sym->type;
            /* Handle member access and postfix: sym->member, sym.member, sym[i], sym++, sym-- */
            while (true) {
                ZeroCCTokType mt = c->tokens[*pos].type;
                if (mt == TK_INC || mt == TK_DEC) {
                    (*pos)++;
                    continue;
                }
                if (mt == TK_DOT || mt == TK_ARROW) {
                    (*pos)++;
                    if (c->tokens[*pos].type == TK_IDENT) {
                        const char *mem_name = c->tokens[*pos].str_val;
                        (*pos)++;
                        ty = zero_cc_type_find_member(c, ty, mem_name);
                    }
                    continue;
                }
                if (mt == TK_LBRACKET) { /* array indexing */
                    (*pos)++;
                    zero_cc_type_of_equality(c, pos); /* consume index */
                    if (c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                    if (ty.tag == TYPE_ARRAY && ty.base) {
                        ty = *ty.base;
                    } else if (ty.tag == TYPE_PTR && ty.base) {
                        ty = *ty.base;
                    }
                    continue;
                }
                break;
            }
            return ty;
        }
        /* Check for thread-local variable */
        void *tls = zero_cc_find_tls(c, name);
        if (tls) {
            /* Get type from TLS slots array */
            for (size_t i = 0; i < c->thread_local_count; i++) {
                if (strcmp(c->thread_locals[i].name, name) == 0) {
                    ZeroCCType ty = c->thread_locals[i].type;
                    /* Handle member access and postfix on TLS variable */
                    while (true) {
                        ZeroCCTokType mt = c->tokens[*pos].type;
                        if (mt == TK_INC || mt == TK_DEC) {
                            (*pos)++;
                            continue;
                        }
                        if (mt == TK_DOT || mt == TK_ARROW) {
                            (*pos)++;
                            if (c->tokens[*pos].type == TK_IDENT) {
                                const char *mem_name = c->tokens[*pos].str_val;
                                (*pos)++;
                                ty = zero_cc_type_find_member(c, ty, mem_name);
                            }
                            continue;
                        }
                        if (mt == TK_LBRACKET) { /* array indexing */
                            (*pos)++;
                            zero_cc_type_of_equality(c, pos); /* consume index */
                            if (c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                            if (ty.tag == TYPE_ARRAY && ty.base) {
                                ty = *ty.base;
                            } else if (ty.tag == TYPE_PTR && ty.base) {
                                ty = *ty.base;
                            }
                            continue;
                        }
                        break;
                    }
                    return ty;
                }
            }
        }
        /* Check for global variable */
        ZeroCCGlobalVar *gv = find_global_var(c, name);
        if (gv) {
            ZeroCCType ty = gv->type;
            while (true) {
                ZeroCCTokType mt = c->tokens[*pos].type;
                if (mt == TK_INC || mt == TK_DEC) {
                    (*pos)++;
                    continue;
                }
                if (mt == TK_DOT || mt == TK_ARROW) {
                    (*pos)++;
                    if (c->tokens[*pos].type == TK_IDENT) {
                        const char *mem_name = c->tokens[*pos].str_val;
                        (*pos)++;
                        ty = zero_cc_type_find_member(c, ty, mem_name);
                    }
                    continue;
                }
                if (mt == TK_LBRACKET) { /* array indexing */
                    (*pos)++;
                    zero_cc_type_of_equality(c, pos); /* consume index */
                    if (c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                    if (ty.tag == TYPE_ARRAY && ty.base) {
                        ty = *ty.base;
                    } else if (ty.tag == TYPE_PTR && ty.base) {
                        ty = *ty.base;
                    }
                    continue;
                }
                break;
            }
            return ty;
        }
        /* Check for function: the operand decays to a function pointer
         * (C99 6.3.2.1) - sizeof of a function name is the pointer size. */
        ZeroCCFunction *fn = zero_cc_find_function(c, name);
        if (fn) {
            return zc_type_from_function(fn);
        }
        /* Check for typedef/struct/enum */
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
        if (!tdef) tdef = zero_cc_find_enum(c, name);
        if (tdef) {
            ZeroCCType ty = *tdef;
            /* Handle member access on type name (e.g., S.x) */
            while (c->tokens[*pos].type == TK_DOT) {
                (*pos)++;
                if (c->tokens[*pos].type == TK_IDENT) {
                    const char *mem_name = c->tokens[*pos].str_val;
                    (*pos)++;
                    ty = zero_cc_type_find_member(c, ty, mem_name);
                }
            }
            return ty;
        }
        if (zero_cc_find_enum_const(c, name)) {
            return zero_cc_type_int();
        }
        /* Unknown identifier */
        diag_report_error_at_token(c, "unknown identifier '%s'", c->tokens[*pos - 1].str_val);
        ZeroCCType unknown_ty;
        memset(&unknown_ty, 0, sizeof(unknown_ty));
        return unknown_ty;
    }
    /* Should not reach here for valid constant expression */
    diag_report_error_at_token(c, "invalid constant expression");
    ZeroCCType unknown_ty;
    memset(&unknown_ty, 0, sizeof(unknown_ty));
    return unknown_ty;
}

static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name)
{
    (void)c;
    if (base.tag == TYPE_PTR && base.base) {
        base = *base.base;
    }
    if (!zero_cc_type_is_struct_or_union(&base)) {
        return zero_cc_type_int();
    }
    const ZeroCCStructMember *m = zero_cc_type_find_member_info(&base, member_name);
    if (m) {
        if ((m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) && m->base) {
            return *m->base;
        }
        ZeroCCType member_ty;
        memset(&member_ty, 0, sizeof(member_ty));
        member_ty.tag = m->tag;
        member_ty.size = m->size;
        member_ty.align = m->align;
        member_ty.base = m->base;
        if (m->base && (m->base->tag == TYPE_STRUCT || m->base->tag == TYPE_UNION)) {
            snprintf(member_ty.struct_name, sizeof(member_ty.struct_name), "%s", m->base->struct_name);
        } else {
            snprintf(member_ty.struct_name, sizeof(member_ty.struct_name), "%s", m->name);
        }
        return member_ty;
    }
    /* Member not found - return int */
    return zero_cc_type_int();
}

/* C99 6.5.9: the basic type specifiers may appear in ANY order, so
 * `short int`, `short unsigned`, `unsigned short int`, `long unsigned`,
 * `long signed`, `int unsigned`, `int signed`, `long long signed`, ...
 * are all legal.  Consume the whole specifier window and derive the type
 * from the SET of specifiers seen (order-insensitive), instead of the
 * previous one-token-per-branch handling that left trailing specifiers
 * unconsumed ("expected identifier in declaration" on `short int a;`). */
/* GNU 128-bit base type names (tokenized as identifiers). Returns 0 if the
 * name is not one of them, 1 for the signed forms (`__int128` / `__int128_t`),
 * 2 for the always-unsigned `__uint128_t`. */
static inline int zc_name_int128_kind(const char *name) {
    if (!name) return 0;
    if (strcmp(name, "__int128") == 0 || strcmp(name, "__int128_t") == 0) return 1;
    if (strcmp(name, "__uint128_t") == 0) return 2;
    return 0;
}

static inline ZeroCCType zc_parse_basic_type_spec(ZeroCCCompiler *c, size_t *pos) {
    int has_char = 0, has_short = 0, has_long = 0;
    int has_float = 0, has_double = 0;
    int n_unsigned = 0, n_signed = 0;
    int has_int128 = 0, int128_kind = 0, has_float128 = 0;
    int has_const = 0, has_volatile = 0, has_restrict = 0;
    for (;;) {
        ZeroCCTokType u = (*pos < c->token_count) ? c->tokens[*pos].type : TK_EOF;
        if (u == TK_INT) { /* int - carries no extra information */ }
        else if (u == TK_CHAR) has_char = 1;
        else if (u == TK_SHORT) has_short = 1;
        else if (u == TK_LONG) has_long++;
        else if (u == TK_FLOAT_KW) has_float = 1;
        else if (u == TK_DOUBLE_KW) has_double = 1;
        else if (u == TK_UNSIGNED) n_unsigned++;
        else if (u == TK_SIGNED) n_signed++;
        else if (u == TK_IDENT) {
            /* GNU 128-bit base type (`__int128` / `__uint128_t` / `__int128_t`). */
            int k = zc_name_int128_kind(c->tokens[*pos].str_val);
            if (k) { has_int128 = 1; int128_kind = k; }
            /* GNU __float128 (Sprint 112). */
            else if (strcmp(c->tokens[*pos].str_val, "__float128") == 0) has_float128 = 1;
            /* C23 _FloatN / _FloatNx (Sprint 160). ZeroCC models every FP
             * width with its nearest existing FP object: _Float32 -> float
             * (4B slot, double model), _Float64/_Float32x -> double, and
             * _Float128/_Float64x -> the 16-byte __float128 object. _Float16,
             * __bf16 and _Float128x have no ZeroCC object model and stay
             * unsupported (the c-torture gate skips their tests). */
            else if (strcmp(c->tokens[*pos].str_val, "_Float32") == 0) has_float = 1;
            else if (strcmp(c->tokens[*pos].str_val, "_Float64") == 0) has_double = 1;
            else if (strcmp(c->tokens[*pos].str_val, "_Float32x") == 0) has_double = 1;
            else if (strcmp(c->tokens[*pos].str_val, "_Float128") == 0 ||
                     strcmp(c->tokens[*pos].str_val, "_Float64x") == 0) has_float128 = 1;
            else break;
        }
        /* Type qualifiers interleave with specifiers in any order
         * (C99 6.7.3): `unsigned long long const`, `int const *`, ... */
        else if (u == TK_CONST) has_const = 1;
        else if (u == TK_VOLATILE) has_volatile = 1;
        else if (u == TK_RESTRICT) has_restrict = 1;
        else break;
        (*pos)++;
    }
    bool is_unsigned = (n_unsigned > 0) && (n_signed == 0);
    ZeroCCType res;
    if (has_int128) {
        /* `__uint128_t` is always unsigned; `__int128`/`__int128_t` follow
         * the `unsigned`/`signed` specifier. */
        bool u = (int128_kind == 2) || is_unsigned;
        res = u ? zero_cc_type_uint128() : zero_cc_type_int128();
    } else if (has_float128) {
        res = zero_cc_type_float128();
    } else if (has_double || has_float) {
        if (has_double) res = has_long ? zero_cc_type_long_double() : zero_cc_type_double();
        else res = has_long ? zero_cc_type_long_double() : zero_cc_type_float();
    } else if (has_char) {
        res = is_unsigned ? zero_cc_type_uint8() : zero_cc_type_char();
    } else if (has_short) {
        res = is_unsigned ? zero_cc_type_uint16() : zero_cc_type_int16();
    } else if (has_long) {
        if (has_long == 1 && zc_target_is_32bit(c)) {
            res = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
        } else {
            res = is_unsigned ? zero_cc_type_uint64() : zero_cc_type_int64();
        }
    } else {
        res = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
    }
    if (has_const) res.is_const = true;
    if (has_volatile) res.is_volatile = true;
    if (has_restrict) res.is_restrict = true;
    return res;
}

/* Consume one array-dimension bracket group `[...]` in a type-NAME context
 * (*pos at '[').  Returns the length when the dimension is a single numeric
 * literal, else -1: an expression dimension (`sizeof (T)`, `2*8`, `n`) has
 * a runtime value that is irrelevant to a type name (casts reinterpret the
 * pointer; named declarations go through the declarator stack, which keeps
 * the real dimension expression). */
static inline ssize_t zc_consume_type_dim(ZeroCCCompiler *c, size_t *pos) {
    (*pos)++; /* '[' */
    ssize_t alen = 0;
    bool numeric = false;
    if (*pos < c->token_count && c->tokens[*pos].type == TK_NUM) {
        alen = (ssize_t)c->tokens[*pos].num_val;
        numeric = true;
        (*pos)++;
    } else if (*pos < c->token_count && c->tokens[*pos].type != TK_RBRACKET) {
        int pd = 0, bd = 0;
        while (*pos < c->token_count) {
            ZeroCCTokType dt = c->tokens[*pos].type;
            if (dt == TK_LPAREN) pd++;
            else if (dt == TK_RPAREN) { if (pd == 0) break; pd--; }
            else if (dt == TK_LBRACKET) bd++;
            else if (dt == TK_RBRACKET) { if (bd == 0) break; bd--; }
            (*pos)++;
        }
    }
    if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
    return numeric ? alen : -1;
}

/* Bitmask helpers for per-pointer-level type qualifiers (C99/C11 6.7.3). */
#define ZC_Q_CONST    1u
#define ZC_Q_VOLATILE 2u
#define ZC_Q_RESTRICT 4u
#define ZC_Q_ATOMIC   8u

static inline uint8_t zc_consume_qualifiers(ZeroCCCompiler *c, size_t *pos) {
    uint8_t qmask = 0;
    while (*pos < c->token_count) {
        ZeroCCTokType qt = c->tokens[*pos].type;
        if (qt == TK_CONST) qmask |= ZC_Q_CONST;
        else if (qt == TK_VOLATILE) qmask |= ZC_Q_VOLATILE;
        else if (qt == TK_RESTRICT) qmask |= ZC_Q_RESTRICT;
        else if (qt == TK_ATOMIC) qmask |= ZC_Q_ATOMIC;
        else break;
        (*pos)++;
    }
    return qmask;
}

static inline void zc_apply_qual_mask(ZeroCCType *ty, uint8_t qmask) {
    if (qmask & ZC_Q_CONST) ty->is_const = true;
    if (qmask & ZC_Q_VOLATILE) ty->is_volatile = true;
    if (qmask & ZC_Q_RESTRICT) ty->is_restrict = true;
    if (qmask & ZC_Q_ATOMIC) ty->is_atomic = true;
}

static inline void zc_skip_balanced_expr_tokens(ZeroCCCompiler *c, size_t *pos) {
    int pd = 0, bd = 0, brd = 0;
    while (*pos < c->token_count && c->tokens[*pos].type != TK_EOF) {
        ZeroCCTokType tt = c->tokens[*pos].type;
        if (tt == TK_LPAREN) pd++;
        else if (tt == TK_RPAREN) {
            if (pd == 0 && bd == 0 && brd == 0) break;
            if (pd > 0) pd--;
        } else if (tt == TK_LBRACKET) bd++;
        else if (tt == TK_RBRACKET) { if (bd > 0) bd--; }
        else if (tt == TK_LBRACE) brd++;
        else if (tt == TK_RBRACE) { if (brd > 0) brd--; }
        else if (tt == TK_COMMA && pd == 0 && bd == 0 && brd == 0) break;
        (*pos)++;
    }
}

/* One parenthesized abstract-declarator group: `(` stars [ nested group |
 * array ] `)` optional `(params)` - applies the pointer/fnptr/array wraps
 * to *ty. RECURSIVE: a nested group inside the parentheses is a complete
 * deeper declarator layer (`(*(*f)(A))(B)` = function pointer returning a
 * function pointer - the dlsym cast shape in sqlite3.c). The group's own
 * stars wrap OUTSIDE the nested layer; the function flag lands on the
 * innermost (leftmost) star layer, whose base is the return type. */
static inline void zc_parse_paren_declarator_ex(ZeroCCCompiler *c, size_t *pos,
                                                ZeroCCType *ty, int *out_ptr_cnt)
{
    if (*pos >= c->token_count || c->tokens[*pos].type != TK_LPAREN ||
        *pos + 1 >= c->token_count || c->tokens[*pos + 1].type != TK_STAR) {
        if (out_ptr_cnt) *out_ptr_cnt = 0;
        return;
    }
    (*pos)++; /* consume '(' */
    int ptr_cnt = 0;
    uint8_t star_quals[16];
    memset(star_quals, 0, sizeof(star_quals));
    while (*pos < c->token_count && c->tokens[*pos].type == TK_STAR) {
        int sidx = ptr_cnt;
        ptr_cnt++;
        (*pos)++;
        uint8_t qmask = zc_consume_qualifiers(c, pos);
        if (sidx < 16) star_quals[sidx] = qmask;
    }
    ssize_t inner_dims[8];
    int inner_cnt = 0;
    while (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
        ssize_t alen = zc_consume_type_dim(c, pos);
        if (alen < 0) alen = 0;
        if (inner_cnt < 8) inner_dims[inner_cnt++] = alen;
    }
    /* Nested group: parse the inner declarator FIRST (its wraps are the
     * innermost), then apply this group's star wraps around it. */
    if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
        int nested_ptrs = 0;
        zc_parse_paren_declarator_ex(c, pos, ty, &nested_ptrs);
        (void)nested_ptrs;
    }
    /* Parameter name inside the group (abstract declarator `(*name)` / `(**name)`
     * - fnptr parameters like `void (**pxFunc)(...)` in struct members). */
    if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) (*pos)++;
    if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;

    bool is_func = false;
    bool fn_has_proto = false;
    bool fn_variadic = false;
    ZeroCCType fparams[32];
    int fp_cnt = 0;

    if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
        is_func = true;
        (*pos)++; /* consume '(' */
        if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) {
            (*pos)++;
        } else if (*pos + 1 < c->token_count &&
                   c->tokens[*pos].type == TK_VOID &&
                   c->tokens[*pos + 1].type == TK_RPAREN) {
            fn_has_proto = true;
            (*pos) += 2;
        } else {
            fn_has_proto = true;
            while (*pos < c->token_count && c->tokens[*pos].type != TK_RPAREN &&
                   c->tokens[*pos].type != TK_EOF) {
                if (c->tokens[*pos].type == TK_ELLIPSIS) {
                    fn_variadic = true;
                    (*pos)++;
                    break;
                }
                size_t before = *pos;
                ZeroCCType pt = zero_cc_parse_type_name_at_pos(c, pos);
                if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
                    (*pos)++;
                }
                ssize_t p_dims[8];
                int p_dcnt = 0;
                while (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
                    ssize_t plen = zc_consume_type_dim(c, pos);
                    if (plen < 0) plen = 0;
                    if (p_dcnt < 8) p_dims[p_dcnt++] = plen;
                }
                for (int pi = p_dcnt - 1; pi >= 0; pi--) {
                    ZeroCCType pelem = pt;
                    pt = zero_cc_type_array(&pelem, p_dims[pi]);
                }
                if (pt.tag == TYPE_ARRAY && pt.base) {
                    pt = zero_cc_type_normalize_array_qualifiers(pt);
                    ZeroCCType pelem = *pt.base;
                    pt = zero_cc_type_ptr(&pelem);
                } else if (pt.is_func) {
                    pt = zero_cc_type_ptr(&pt);
                    pt.is_func_ptr = true;
                }
                if (*pos == before) {
                    zc_skip_balanced_expr_tokens(c, pos);
                    if (*pos == before) break;
                } else if (fp_cnt < 32) {
                    fparams[fp_cnt++] = pt;
                }
                if (*pos < c->token_count && c->tokens[*pos].type == TK_COMMA) {
                    (*pos)++;
                } else {
                    break;
                }
            }
            if (*pos < c->token_count && c->tokens[*pos].type != TK_RPAREN) {
                int pdepth = 1;
                while (*pos < c->token_count && pdepth > 0) {
                    if (c->tokens[*pos].type == TK_LPAREN) pdepth++;
                    else if (c->tokens[*pos].type == TK_RPAREN) {
                        pdepth--;
                        if (pdepth == 0) break;
                    }
                    (*pos)++;
                }
            }
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
        }
    } else if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
        ssize_t arr_dims[8];
        int arr_cnt = 0;
        while (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
            ssize_t arr_len = zc_consume_type_dim(c, pos);
            if (arr_len < 0) arr_len = 0;
            if (arr_cnt < 8) arr_dims[arr_cnt++] = arr_len;
        }
        for (int ai = arr_cnt - 1; ai >= 0; ai--) {
            ZeroCCType elem_ty = *ty;
            *ty = zero_cc_type_array(&elem_ty, arr_dims[ai]);
        }
    }
    for (int p = 0; p < ptr_cnt; p++) {
        *ty = zero_cc_type_ptr(ty);
        if (p < 16) zc_apply_qual_mask(ty, star_quals[p]);
        if (is_func && p == 0) {
            ty->is_func_ptr = true;
            if (ty->base) {
                ty->base->is_func = true;
                ty->base->fn_has_prototype = fn_has_proto;
                ty->base->fn_is_variadic = fn_variadic;
                if (fn_has_proto && fp_cnt > 0 && ty->base->fn_params == NULL) {
                    ZeroCCType *fp = (ZeroCCType *)zero_cc_type_pool_alloc_fn_params((size_t)fp_cnt);
                    if (fp) {
                        for (int k = 0; k < fp_cnt; k++) fp[k] = fparams[k];
                        ty->base->fn_params = fp;
                        ty->base->fn_param_count = (size_t)fp_cnt;
                    }
                }
            }
        }
    }
    for (int ai = inner_cnt - 1; ai >= 0; ai--) {
        ZeroCCType elem_ty = *ty;
        *ty = zero_cc_type_array(&elem_ty, inner_dims[ai]);
    }
    if (out_ptr_cnt) *out_ptr_cnt = ptr_cnt;
}

static inline void zc_parse_paren_declarator(ZeroCCCompiler *c, size_t *pos, ZeroCCType *ty)
{
    zc_parse_paren_declarator_ex(c, pos, ty, NULL);
}

static inline ZeroCCType zc_parse_struct_or_union_spec(ZeroCCCompiler *c, size_t *pos, ZeroCCTokType kw)
{
    ZeroCCType ty;
    (*pos)++; /* consume struct / union */
    size_t align_probe = 0;
    {
        size_t probe = 0;
        while (zero_cc_parse_aligned_attr(c, pos, &probe)) {
            if (probe > align_probe) align_probe = probe;
        }
    }
    char tag_buf[128] = "";
    if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
        snprintf(tag_buf, sizeof(tag_buf), "%s", c->tokens[*pos].str_val);
        (*pos)++;
        size_t probe = 0;
        while (zero_cc_parse_aligned_attr(c, pos, &probe)) {
            if (probe > align_probe) align_probe = probe;
        }
    }
    if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACE && c->parse_struct_body) {
        size_t save = c->token_pos;
        c->token_pos = *pos;
        ty = c->parse_struct_body(c, kw == TK_UNION);
        size_t after = c->token_pos;
        if (pos != &c->token_pos) c->token_pos = save;
        *pos = after;
        if (align_probe > ty.align) ty.align = align_probe;
        if (tag_buf[0] != '\0') {
            snprintf(ty.struct_name, sizeof(ty.struct_name), "%s", tag_buf);
        } else if (ty.struct_name[0] == '\0') {
            snprintf(ty.struct_name, sizeof(ty.struct_name), "__anon_%s_%zu",
                     (kw == TK_UNION) ? "union" : "struct", c->anon_struct_seq++);
        }
        zero_cc_register_struct(c, ty.struct_name, ty);
        zero_cc_note_members_array(c, ty.members);
        return ty;
    }
    if (tag_buf[0] != '\0') {
        ZeroCCType *st = zero_cc_find_struct(c, tag_buf);
        if (!st) st = zero_cc_find_typedef(c, tag_buf);
        if (st) return *st;
        if (strcmp(tag_buf, "struct") != 0 && strcmp(tag_buf, "union") != 0 &&
            strcmp(tag_buf, "enum") != 0) {
            /* Forward reference: materialize an incomplete (zero-size)
             * type so globals/pointees keep the struct's identity
             * instead of silently becoming long (c-torture pr46388). */
            memset(&ty, 0, sizeof(ty));
            ty.tag = (kw == TK_UNION) ? TYPE_UNION : TYPE_STRUCT;
            ty.size = 0;
            ty.align = 1;
            snprintf(ty.struct_name, sizeof(ty.struct_name), "%s", tag_buf);
            zero_cc_register_struct(c, tag_buf, ty);
            return ty;
        }
    }
    return zero_cc_type_int64();
}

static inline ZeroCCType zc_parse_enum_spec(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType ty;
    (*pos)++; /* consume enum */
    skip_c23_attributes_at(c, pos);
    char ename[128] = "";
    if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
        snprintf(ename, sizeof(ename), "%s", c->tokens[*pos].str_val);
        (*pos)++;
    }
    skip_c23_attributes_at(c, pos);
    size_t save = c->token_pos;
    c->token_pos = *pos;
    ZCEnumUnderlying und = zero_cc_parse_enum_underlying(c);
    skip_c23_attributes(c);
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
        c->token_pos++;
        if (!ename[0]) {
            snprintf(ename, sizeof(ename), "__anon_enum_%zu", c->anon_struct_seq++);
        }
        ZeroCCType *existing = (ename[0] != '\0') ? zero_cc_find_enum(c, ename) : NULL;
        if (existing && existing->is_fixed_enum && und.size == 0) {
            diag_report_error_at_token(c, "enum declared with but defined without fixed underlying type");
        } else if (existing && !existing->is_fixed_enum && und.size) {
            diag_report_error_at_token(c, "enum declared without but defined with fixed underlying type");
        } else if (existing && existing->is_fixed_enum && und.size && existing->size != und.size) {
            diag_report_error_at_token(c, "conflicting underlying type for enum");
        }
        ty = zero_cc_type_enum(ename);
        zc_enum_apply_underlying(&ty, &und);
        zero_cc_parse_enum_members(c, &und);
        if (ename[0] != '\0') {
            zero_cc_register_enum(c, ename, ty);
        }
    } else if (ename[0] != '\0') {
        ZeroCCType *et = zero_cc_find_enum(c, ename);
        if (!et) et = zero_cc_find_typedef(c, ename);
        if (et) {
            ty = *et;
            if (und.size) {
                if (et->is_fixed_enum && et->size != und.size) {
                    diag_report_error_at_token(c, "conflicting underlying type for enum");
                }
                zc_enum_apply_underlying(&ty, &und);
            }
        } else {
            ty = zero_cc_type_enum(ename);
            zc_enum_apply_underlying(&ty, &und);
            zero_cc_register_enum(c, ename, ty);
        }
    } else {
        ty = zero_cc_type_enum(NULL);
        zc_enum_apply_underlying(&ty, &und);
    }
    size_t after = c->token_pos;
    if (pos != &c->token_pos) c->token_pos = save;
    *pos = after;
    return ty;
}

static inline ZeroCCType zc_type_from_function(const ZeroCCFunction *fn)
{
    ZeroCCType ft = fn->return_type;
    ft.is_func = true;
    ft.fn_has_prototype = fn->has_prototype;
    ft.fn_is_variadic = fn->is_variadic;
    if (fn->has_prototype && fn->param_count > 0 && fn->param_types) {
        ZeroCCType *fp = (ZeroCCType *)zero_cc_type_pool_alloc_fn_params(fn->param_count);
        if (fp) {
            for (size_t i = 0; i < fn->param_count; i++) fp[i] = fn->param_types[i];
            ft.fn_params = fp;
            ft.fn_param_count = fn->param_count;
        }
    }
    ZeroCCType ptr = zero_cc_type_ptr(&ft);
    ptr.is_func_ptr = true;
    return ptr;
}

static inline ZeroCCType zero_cc_parse_type_name_at_pos(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType ty;
    /* Type qualifiers and storage specifiers may precede the type name (C99 6.7.3, C23 6.5.2.5) */
    bool q_const = false, q_volatile = false, q_restrict = false;
    bool q_static = false, q_constexpr = false;
    while (*pos < c->token_count &&
           (c->tokens[*pos].type == TK_CONST || c->tokens[*pos].type == TK_VOLATILE ||
            c->tokens[*pos].type == TK_RESTRICT || c->tokens[*pos].type == TK_STATIC ||
            c->tokens[*pos].type == TK_CONSTEXPR || c->tokens[*pos].type == TK_THREAD_LOCAL)) {
        if (c->tokens[*pos].type == TK_CONST) q_const = true;
        else if (c->tokens[*pos].type == TK_VOLATILE) q_volatile = true;
        else if (c->tokens[*pos].type == TK_RESTRICT) q_restrict = true;
        else if (c->tokens[*pos].type == TK_STATIC) q_static = true;
        else if (c->tokens[*pos].type == TK_CONSTEXPR) { q_constexpr = true; q_const = true; }
        (*pos)++;
    }
    /* GNU vector attribute marker (`__attribute__((__vector_size__(N)))`,
     * preprocessed into `__zc_vector_size__(N)`) may lead the type name:
     * `sizeof(__attribute__((__vector_size__(16))) short)`. Same for the
     * `mode(X)` integer-width marker (`__zc_mode__(QI)`). */
    size_t vec_attr_size = 0;
    size_t mode_attr_bytes = 0;
    {
        size_t probe = 0;
        for (int mk = 0; mk < 4; mk++) {
            bool got = false;
            if (vec_attr_size == 0 &&
                zero_cc_parse_vector_attr(c, pos, &probe)) {
                vec_attr_size = probe;
                got = true;
            }
            if (mode_attr_bytes == 0 &&
                zero_cc_parse_mode_attr(c, pos, &probe)) {
                mode_attr_bytes = probe;
                got = true;
            }
            if (!got) break;
        }
    }
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_STRUCT || t == TK_UNION) {
        ty = zc_parse_struct_or_union_spec(c, pos, t);
    } else if (t == TK_ENUM) {
        ty = zc_parse_enum_spec(c, pos);
    } else if (t == TK_IDENT) {
        const char *name = c->tokens[*pos].str_val;
        /* C99 6.7.2.3: `_Complex double` / `double _Complex`. */
        if (zero_cc_tok_is_complex_spec(name)) {
            (*pos)++;
            /* Standalone `_Complex` (no inner base) means `_Complex double`;
             * otherwise parse the explicit inner real type. */
            ZeroCCType inner = zero_cc_token_is_complex_inner_base(c, *pos)
                ? zero_cc_parse_type_name_at_pos(c, pos)
                : zero_cc_type_double();
            return zero_cc_type_complex(&inner);
        }
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
        if (!tdef) tdef = zero_cc_find_enum(c, name);
        if (tdef) {
            ty = *tdef;
            (*pos)++;
        } else {
            /* GNU 128-bit base type (`__int128` / `__uint128_t` / `__int128_t`). */
            int i128k = zc_name_int128_kind(name);
            ty = (i128k == 2) ? zero_cc_type_uint128()
               : (i128k == 1) ? zero_cc_type_int128()
               : (strcmp(name, "__float128") == 0) ? zero_cc_type_float128()
               : zero_cc_type_int();
            (*pos)++;
        }
    } else if (t == TK_INT || t == TK_CHAR || t == TK_SHORT || t == TK_LONG ||
               t == TK_FLOAT_KW || t == TK_DOUBLE_KW ||
               t == TK_SIGNED || t == TK_UNSIGNED) {
        /* Basic type specifier window in any order (C99 6.5.9). */
        ty = zc_parse_basic_type_spec(c, pos);
    } else if (t == TK_VOID) {
        ty = zero_cc_type_void(); (*pos)++;
    } else if (t == TK_BOOL_KW) {
        ty = zero_cc_type_bool(); (*pos)++;
    } else if (t == TK_ATOMIC) {
        (*pos)++;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
            (*pos)++;
            ty = zero_cc_parse_type_name_at_pos(c, pos);
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
        } else {
            ty = zero_cc_parse_type_name_at_pos(c, pos);
        }
        ty.is_atomic = true;
    } else if (t == TK_SIGNED || t == TK_UNSIGNED) {
        /* signed/unsigned [short|int|long|long long] */
        bool is_unsigned = (t == TK_UNSIGNED);
        (*pos)++;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_SHORT) {
            ty = is_unsigned ? zero_cc_type_uint16() : zero_cc_type_int16();
            (*pos)++;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_INT) (*pos)++;
        } else if (*pos < c->token_count && c->tokens[*pos].type == TK_CHAR) {
            ty = is_unsigned ? zero_cc_type_uint8() : zero_cc_type_char();
            (*pos)++;
        } else if (*pos < c->token_count && c->tokens[*pos].type == TK_LONG) {
            (*pos)++;
            int n_long = 1;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_LONG) { (*pos)++; n_long = 2; }
            if (*pos < c->token_count &&
                (c->tokens[*pos].type == TK_DOUBLE_KW || c->tokens[*pos].type == TK_FLOAT_KW)) {
                (*pos)++;
                ty = zero_cc_type_long_double();
            } else {
                if (n_long == 1 && zc_target_is_32bit(c)) {
                    ty = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
                } else {
                    ty = is_unsigned ? zero_cc_type_uint64() : zero_cc_type_int64();
                }
            }
            if (*pos < c->token_count && c->tokens[*pos].type == TK_INT) (*pos)++; /* unsigned long int */
        } else {
            if (*pos < c->token_count && c->tokens[*pos].type == TK_INT) (*pos)++; /* [int] */
            ty = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
        }
    } else if (t == TK_TYPEOF || t == TK_TYPEOF_UNQUAL) {
        bool unqual = (t == TK_TYPEOF_UNQUAL);
        (*pos)++;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
            (*pos)++;
            if (zero_cc_tok_is_type_start(c, *pos)) {
                ty = zero_cc_parse_type_name_at_pos(c, pos);
            } else {
                ty = zero_cc_type_of_equality(c, pos);
            }
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) {
                (*pos)++;
            }
        } else {
            ty = zero_cc_type_int();
        }
        if (unqual) {
            ty.is_const = false;
            ty.is_volatile = false;
            ty.is_restrict = false;
            ty.is_atomic = false;
        }
    } else {
        ty = zero_cc_type_int();
    }
    /* Leading qualifiers attach to the BASE type, not the outermost
     * pointer: `const char *ss` = pointer to const char (C99 6.7.3).
     * Applied now, before pointer/array wrapping. */
    if (q_const) ty.is_const = true;
    if (q_volatile) ty.is_volatile = true;
    if (q_restrict) ty.is_restrict = true;
    if (q_static) ty.is_static = true;
    if (q_constexpr) { ty.is_constexpr = true; ty.is_const = true; }
    /* Vector attribute trailing the base specifier:
     * `short __attribute__((__vector_size__(16)))` / `V __zc_vector_size__(16)`.
     * Wraps the base type in a 128-bit vector before any pointer/array
     * declarator suffixes are applied. Same for `mode(X)`:
     * `unsigned int __attribute__((mode(QI)))` resizes the integer base.
     * The loop handles chained attributes in any order. */
    for (int mk = 0; mk < 4; mk++) {
        size_t probe = 0;
        bool got = false;
        if (vec_attr_size == 0 &&
            zero_cc_parse_vector_attr(c, pos, &probe)) {
            vec_attr_size = probe;
            got = true;
        }
        if (mode_attr_bytes == 0 &&
            zero_cc_parse_mode_attr(c, pos, &probe)) {
            mode_attr_bytes = probe;
            got = true;
        }
        if (!got) break;
    }
    if (mode_attr_bytes > 0) {
        ty = zero_cc_type_apply_mode(ty, mode_attr_bytes);
    }
    if (vec_attr_size > 0 && !zero_cc_type_is_vector(&ty)) {
        ty = zero_cc_type_vector(&ty, vec_attr_size);
    }
    /* Qualifiers may also TRAIL the base name: `u32 volatile *p`,
     * `struct S const *s`, `enum E restrict *e` (C99 6.7.3 - the
     * specifier list is order-insensitive). With a pointer declarator
     * they still attach to the pointee. The keyword-base branch gets
     * these for free (zc_parse_basic_type_spec's specifier window), but
     * the typedef/struct/enum IDENT branch stopped at the name, leaving
     * `u32 volatile *` unparseable (sqlite3.c's WAL casts). */
    while (*pos < c->token_count &&
           (c->tokens[*pos].type == TK_CONST || c->tokens[*pos].type == TK_VOLATILE ||
            c->tokens[*pos].type == TK_RESTRICT)) {
        if (c->tokens[*pos].type == TK_CONST) q_const = true;
        else if (c->tokens[*pos].type == TK_VOLATILE) q_volatile = true;
        else q_restrict = true;
        (*pos)++;
    }
    if (q_const) ty.is_const = true;
    if (q_volatile) ty.is_volatile = true;
    if (q_restrict) ty.is_restrict = true;
    if (q_static) ty.is_static = true;
    if (q_constexpr) { ty.is_constexpr = true; ty.is_const = true; }
    /* Declarator suffixes (C99 6.7.5): parenthesized abstract
     * declarators (`(*)(void)`, `(*)[4]`, `(*[3])(float)`) and pointer
     * stars may alternate in any order - `int *(*)(long)`,
     * `struct A *(*) (long)` (c-torture compile/20010102-1).  Stars
     * BEFORE a function-pointer paren block belong to the function's
     * RETURN type, so the is_func_ptr layer is always the wrap applied
     * directly around the already-starred base. */
    for (;;) {
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN &&
            *pos + 1 < c->token_count && c->tokens[*pos + 1].type == TK_STAR) {
            zc_parse_paren_declarator(c, pos, &ty);
        }
        int star_waits = 0;
        while (*pos < c->token_count && c->tokens[*pos].type == TK_STAR) {
            star_waits++;
            ty = zero_cc_type_ptr(&ty);
            (*pos)++;
            zc_apply_qual_mask(&ty, zc_consume_qualifiers(c, pos));
            {
                size_t probe = 0;
                while (zero_cc_parse_aligned_attr(c, pos, &probe)) { /* skip */ }
            }
        }
        if (star_waits == 0 &&
            !(*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN &&
              *pos + 1 < c->token_count && c->tokens[*pos + 1].type == TK_STAR)) {
            break;
        }
    }
    /* Suffix complex: `double _Complex` (C99 6.7.2.3). */
    if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT &&
        c->tokens[*pos].str_val &&
        zero_cc_tok_is_complex_spec(c->tokens[*pos].str_val) &&
        !ty.is_complex) {
        (*pos)++;
        ty = zero_cc_type_complex(&ty);
    }
    {
        ssize_t arr_dims[8];
        int arr_cnt = 0;
        while (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
            ssize_t arr_len = zc_consume_type_dim(c, pos);
            if (arr_len < 0) arr_len = 0;
            if (arr_cnt < 8) arr_dims[arr_cnt++] = arr_len;
        }
        for (int ai = arr_cnt - 1; ai >= 0; ai--) {
            ZeroCCType elem_ty = ty;
            ty = zero_cc_type_array(&elem_ty, arr_dims[ai]);
        }
    }
    (void)q_const; (void)q_volatile; (void)q_restrict; /* applied at the base type */
    if (q_static) ty.is_static = true;
    if (q_constexpr) ty.is_constexpr = true;
    return ty;
}

static inline bool zero_cc_generic_select_span(ZeroCCCompiler *c, size_t *pos,
                                               size_t *out_sel_pos, size_t *out_end_pos)
{
    size_t p = *pos;
    if (p >= c->token_count || c->tokens[p].type != TK_GENERIC) return false;
    p++;
    if (p < c->token_count && c->tokens[p].type == TK_LPAREN) p++;
    ZeroCCType ctrl_ty = zero_cc_type_of_equality(c, &p);
    zc_skip_balanced_expr_tokens(c, &p);
    ctrl_ty = zero_cc_type_normalize_array_qualifiers(ctrl_ty);
    if (ctrl_ty.tag == TYPE_ARRAY) {
        ZeroCCType elem = ctrl_ty.base ? *ctrl_ty.base : zero_cc_type_int();
        ctrl_ty = zero_cc_type_ptr(&elem);
    } else if (ctrl_ty.is_func) {
        ctrl_ty = zero_cc_type_ptr(&ctrl_ty);
        ctrl_ty.is_func_ptr = true;
    }
    ctrl_ty = zero_cc_type_strip_qualifiers(ctrl_ty);
    if (p < c->token_count && c->tokens[p].type == TK_COMMA) p++;

    bool matched = false;
    size_t sel_pos = 0;
    size_t default_pos = 0;

    while (p < c->token_count && c->tokens[p].type != TK_RPAREN &&
           c->tokens[p].type != TK_EOF) {
        if (c->tokens[p].type == TK_DEFAULT) {
            p++;
            if (p < c->token_count && c->tokens[p].type == TK_COLON) p++;
            default_pos = p;
        } else {
            ZeroCCType assoc_ty = zero_cc_parse_type_name_at_pos(c, &p);
            if (p < c->token_count && c->tokens[p].type == TK_COLON) p++;
            if (!matched && zero_cc_types_compatible(ctrl_ty, assoc_ty)) {
                matched = true;
                sel_pos = p;
            }
        }
        zc_skip_balanced_expr_tokens(c, &p);
        if (p < c->token_count && c->tokens[p].type == TK_COMMA) p++;
    }
    if (p < c->token_count && c->tokens[p].type == TK_RPAREN) p++;
    *pos = p;
    if (out_end_pos) *out_end_pos = p;
    if (matched) {
        if (out_sel_pos) *out_sel_pos = sel_pos;
        return true;
    }
    if (default_pos > 0) {
        if (out_sel_pos) *out_sel_pos = default_pos;
        return true;
    }
    return false;
}

static inline ZeroCCType zero_cc_parse_type_name(ZeroCCCompiler *c) {
    // Handle parentheses
    bool lparen = false;
    if (c->last_type_ptr_depth != 0) c->last_type_ptr_depth = 0;
    if (c->tokens[c->token_pos].type == TK_LPAREN) {
        lparen = true;
        c->token_pos++;
    }
    /* Handle type qualifiers (const, volatile, restrict) at the start */
    bool q_const = false, q_volatile = false, q_restrict = false;
    bool q_static = false, q_constexpr = false;
    while (c->tokens[c->token_pos].type == TK_CONST ||
           c->tokens[c->token_pos].type == TK_VOLATILE ||
           c->tokens[c->token_pos].type == TK_RESTRICT ||
           c->tokens[c->token_pos].type == TK_STATIC ||
           c->tokens[c->token_pos].type == TK_CONSTEXPR ||
           c->tokens[c->token_pos].type == TK_THREAD_LOCAL) {
        if (c->tokens[c->token_pos].type == TK_CONST) q_const = true;
        else if (c->tokens[c->token_pos].type == TK_VOLATILE) q_volatile = true;
        else if (c->tokens[c->token_pos].type == TK_RESTRICT) q_restrict = true;
        else if (c->tokens[c->token_pos].type == TK_STATIC) q_static = true;
        else if (c->tokens[c->token_pos].type == TK_CONSTEXPR) { q_constexpr = true; q_const = true; }
        c->token_pos++;
    }
    /* GNU vector attribute marker (`__attribute__((__vector_size__(N)))`),
     * either leading (`__attribute__(...) short g;`) or trailing the base
     * specifier (`short __attribute__(...) g;`). */
    size_t vec_attr_size = 0;
    size_t align_attr_val = 0;
    size_t mode_attr_bytes = 0;
    {
        size_t save = c->token_pos;
        size_t probe = 0;
        bool consumed = false;
        /* Markers may chain in any order: consume them all, then require a
         * type start (rewind otherwise). */
        for (int mk = 0; mk < 4; mk++) {
            bool got = false;
            if (vec_attr_size == 0 &&
                zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
                vec_attr_size = probe;
                got = true;
            }
            if (align_attr_val == 0 &&
                zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                align_attr_val = probe;
                got = true;
            }
            if (mode_attr_bytes == 0 &&
                zero_cc_parse_mode_attr(c, &c->token_pos, &probe)) {
                mode_attr_bytes = probe;
                got = true;
            }
            if (!got) break;
            consumed = true;
        }
        if (consumed &&
            (c->token_pos >= c->token_count ||
             !zero_cc_tok_is_type_start(c, c->token_pos))) {
            c->token_pos = save;
            vec_attr_size = 0;
            align_attr_val = 0;
            mode_attr_bytes = 0;
        }
    }
    ZeroCCType ty;
    if (c->tokens[c->token_pos].type == TK_TYPEOF || c->tokens[c->token_pos].type == TK_TYPEOF_UNQUAL) {
        ty = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
    } else if (c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION) {
        ty = zc_parse_struct_or_union_spec(c, &c->token_pos, c->tokens[c->token_pos].type);
    } else if (c->tokens[c->token_pos].type == TK_ENUM) {
        ty = zc_parse_enum_spec(c, &c->token_pos);
    } else if (c->tokens[c->token_pos].type == TK_IDENT) {
        const char *name = c->tokens[c->token_pos].str_val;
        /* C99 6.7.2.3: the _Complex / __complex__ type specifier.
         * Prefix form: `_Complex double`; the suffix form is handled
         * after the base specifier below. */
        if (zero_cc_tok_is_complex_spec(name)) {
            c->token_pos++;
            /* Standalone `_Complex` (no inner base) means `_Complex double`;
             * otherwise parse the explicit inner real type. */
            ZeroCCType inner = zero_cc_token_is_complex_inner_base(c, c->token_pos)
                ? zero_cc_parse_type_name(c)
                : zero_cc_type_double();
            return zero_cc_type_complex(&inner);
        }
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
        if (!tdef) tdef = zero_cc_find_enum(c, name);
        if (tdef) {
            ty = *tdef;
            c->token_pos++;
        } else if (strcmp(name, "int") == 0) {
            ty = zero_cc_type_int();
            c->token_pos++;
        } else if (strcmp(name, "char") == 0) {
            ty = zero_cc_type_char();
            c->token_pos++;
        } else if (strcmp(name, "long") == 0) {
            // check if next is also long
            if (c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_LONG) {
                ty = zero_cc_type_int64();
                c->token_pos += 2;
            } else {
                ty = zc_target_is_32bit(c) ? zero_cc_type_int() : zero_cc_type_int64();
                c->token_pos++;
            }
        } else if (strcmp(name, "__int128") == 0 || strcmp(name, "__int128_t") == 0 ||
                   strcmp(name, "__uint128_t") == 0) {
            // GNU 128-bit base type reached as a bare identifier (no leading
            // unsigned/signed keyword, which would have routed the specifier
            // window to zc_parse_basic_type_spec instead). `__uint128_t` is
            // always unsigned; `__int128`/`__int128_t` are signed here.
            bool u128 = (strcmp(name, "__uint128_t") == 0);
            ty = u128 ? zero_cc_type_uint128() : zero_cc_type_int128();
            c->token_pos++;
        } else if (strcmp(name, "__float128") == 0) {
            // GNU __float128 cast target (Sprint 112).
            ty = zero_cc_type_float128();
            c->token_pos++;
        } else if (strcmp(name, "_Float32") == 0) {
            /* C23 _FloatN (Sprint 160): model _Float32 as the 4-byte
             * float object (double value model). */
            ty = zero_cc_type_float();
            c->token_pos++;
        } else if (strcmp(name, "_Float64") == 0 ||
                   strcmp(name, "_Float32x") == 0) {
            ty = zero_cc_type_double();
            c->token_pos++;
        } else if (strcmp(name, "_Float128") == 0 ||
                   strcmp(name, "_Float64x") == 0) {
            ty = zero_cc_type_float128();
            c->token_pos++;
        } else if (strcmp(name, "y") == 0) {
            // lookup local variable y
            ZeroCCSym *sym = zero_cc_find_local(c, "y");
            if (sym) {
                ty = sym->type;
            } else {
                ty = zero_cc_type_int64();
            }
            c->token_pos++;
        } else {
            // unknown identifier, default to int64
            ty = zero_cc_type_int64();
            c->token_pos++;
        }
    } else if (c->tokens[c->token_pos].type == TK_VOID) {
        ty = zero_cc_type_void();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_FLOAT_KW) {
        ty = zero_cc_type_float();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) {
        ty = zero_cc_type_double();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_BOOL_KW) {
        ty = zero_cc_type_bool();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_ATOMIC) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
            ty = zero_cc_parse_type_name(c);
            if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        } else {
            ty = zero_cc_parse_type_name(c);
        }
        ty.is_atomic = true;
    } else if (c->tokens[c->token_pos].type == TK_INT ||
               c->tokens[c->token_pos].type == TK_CHAR ||
               c->tokens[c->token_pos].type == TK_SHORT ||
               c->tokens[c->token_pos].type == TK_LONG ||
               c->tokens[c->token_pos].type == TK_FLOAT_KW ||
               c->tokens[c->token_pos].type == TK_DOUBLE_KW ||
               c->tokens[c->token_pos].type == TK_SIGNED ||
               c->tokens[c->token_pos].type == TK_UNSIGNED) {
        /* Basic type specifier window in any order (C99 6.5.9). */
        ty = zc_parse_basic_type_spec(c, &c->token_pos);
    } else {
        // default
        ty = zero_cc_type_int64();
    }
    /* Leading qualifiers attach to the BASE type, not the outermost
     * pointer: `const char *ss` = pointer to const char (C99 6.7.3).
     * Applied now, before pointer/array wrapping. */
    if (q_const) ty.is_const = true;
    if (q_volatile) ty.is_volatile = true;
    if (q_restrict) ty.is_restrict = true;
    if (q_static) ty.is_static = true;
    if (q_constexpr) { ty.is_constexpr = true; ty.is_const = true; }
    /* Vector attribute trailing the base specifier: consume it (otherwise
     * the marker identifier would be read as the variable name) and wrap
     * the base type in a vector. Same for aligned(N): raise the type's
     * alignment (and pad struct sizes) to the given value. Same for
     * mode(X): resize an integer base type (c-torture execute/memclr).
     * The loop handles chained attributes in any order. */
    for (int mk = 0; mk < 4; mk++) {
        size_t probe = 0;
        bool got = false;
        if (vec_attr_size == 0 &&
            zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
            vec_attr_size = probe;
            got = true;
        }
        if (align_attr_val == 0 &&
            zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
            align_attr_val = probe;
            got = true;
        }
        if (mode_attr_bytes == 0 &&
            zero_cc_parse_mode_attr(c, &c->token_pos, &probe)) {
            mode_attr_bytes = probe;
            got = true;
        }
        if (!got) break;
    }
    if (mode_attr_bytes > 0) {
        ty = zero_cc_type_apply_mode(ty, mode_attr_bytes);
    }
    if (vec_attr_size > 0 && !zero_cc_type_is_vector(&ty)) {
        ty = zero_cc_type_vector(&ty, vec_attr_size);
    }
    if (align_attr_val > 0) {
        if (ty.tag == TYPE_STRUCT || ty.tag == TYPE_UNION) {
            ZeroCCType *st = zero_cc_find_struct(c, ty.struct_name);
            if (!st) st = zero_cc_find_typedef(c, ty.struct_name);
            if (st && align_attr_val > st->align) {
                st->align = align_attr_val;
                if (st->size < align_attr_val) st->size = align_attr_val;
                if ((st->size % align_attr_val) != 0) {
                    st->size = (st->size + align_attr_val - 1) / align_attr_val * align_attr_val;
                }
                ty = *st;
            }
        } else if (align_attr_val > ty.align) {
            ty.align = align_attr_val;
            if (ty.size < align_attr_val) ty.size = align_attr_val;
            if ((ty.size % align_attr_val) != 0) {
                ty.size = (ty.size + align_attr_val - 1) / align_attr_val * align_attr_val;
            }
        }
    }
    /* Suffix complex: `double _Complex` (C99 6.7.2.3). */
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT &&
        c->tokens[c->token_pos].str_val &&
        zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val) &&
        !ty.is_complex) {
        c->token_pos++;
        ty = zero_cc_type_complex(&ty);
    }
    /* Qualifiers trailing base specifier (`T const *`, `struct S volatile *`). */
    zc_apply_qual_mask(&ty, zc_consume_qualifiers(c, &c->token_pos));
    /* Parenthesized abstract declarator and pointer stars (C99 6.7.5):
     * A parenthesized declarator that contains an identifier (`(*fp)(void)`)
     * is a NAMED declarator, not an abstract type: leave the tokens for the
     * declarator parser instead of swallowing the name. */
    for (;;) {
        size_t probe = c->token_pos + 2;
        while (probe < c->token_count &&
               (c->tokens[probe].type == TK_LPAREN ||
                c->tokens[probe].type == TK_STAR ||
                c->tokens[probe].type == TK_CONST ||
                c->tokens[probe].type == TK_VOLATILE ||
                c->tokens[probe].type == TK_RESTRICT ||
                c->tokens[probe].type == TK_ATOMIC)) probe++;
        bool named_declarator = (probe < c->token_count && c->tokens[probe].type == TK_IDENT);
        bool did_paren = false;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN &&
            c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_STAR &&
            !named_declarator) {
            int added_ptrs = 0;
            zc_parse_paren_declarator_ex(c, &c->token_pos, &ty, &added_ptrs);
            c->last_type_ptr_depth += added_ptrs;
            did_paren = true;
        }
        int star_waits = 0;
        /* Parse pointer stars (qualifiers and GNU aligned(N) markers may sit
         * between chunks: `int *const *p;`, `int *__attr__((aligned(16))) *p;`) */
        while (c->tokens[c->token_pos].type == TK_STAR) {
            star_waits++;
            ty = zero_cc_type_ptr(&ty);
            c->last_type_ptr_depth++;
            c->token_pos++;
            zc_apply_qual_mask(&ty, zc_consume_qualifiers(c, &c->token_pos));
            {
                size_t aprop = 0;
                while (zero_cc_parse_aligned_attr(c, &c->token_pos, &aprop)) { /* skip */ }
            }
        }
        if (!did_paren && star_waits == 0) break;
    }
    // Parse array brackets: we ignore size for alignment
    while (c->tokens[c->token_pos].type == TK_LBRACKET) {
        // skip until matching ]
        int bracket_depth = 1;
        c->token_pos++;
        while (c->tokens[c->token_pos].type != TK_EOF && bracket_depth > 0) {
            if (c->tokens[c->token_pos].type == TK_LBRACKET) bracket_depth++;
            else if (c->tokens[c->token_pos].type == TK_RBRACKET) bracket_depth--;
            c->token_pos++;
        }
        // array alignment is same as element type, so ty unchanged
    }
    if (lparen) {
        if (c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }
    }
    (void)q_const; (void)q_volatile; (void)q_restrict; /* applied at the base type */
    if (q_static) ty.is_static = true;
    if (q_constexpr) ty.is_constexpr = true;
    return ty;
}


#endif /* ZERO_CC_PARSER_TYPE_PARSER_H */
