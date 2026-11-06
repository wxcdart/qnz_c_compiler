#ifndef ZERO_CC_JIT_CONST_PTR_EVAL_H
#define ZERO_CC_JIT_CONST_PTR_EVAL_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/const_eval.h"

/* Sprint 65: constant pointer/address initializer expressions
 * (c-torture compile/20001116-1, execute/const-addr-expr-1,
 * compile/pr43679): built from &global, &global[N], &"str"[N],
 * (cast) prefixes, and integer pointer arithmetic. A pointer-pointer
 * difference of the same base yields an integer. Non-consuming: on
 * failure token_pos is restored. Returns 1 for a pointer result,
 * 2 for an integer (pointer difference) result, 0 on non-match. */
typedef struct {
    int kind;        /* 0 rodata, 1 global, 2 extern, -1 none */
    size_t ro_off;
    int gidx;
    char ext_name[64];
    char base_str[64]; /* string-literal base content (kind 0 identity) */
    int64_t off;     /* byte offset from the base */
    int64_t esz;     /* pointee size for subsequent +/- scaling */
} ZCPAddr;

static inline bool zc_cpe_primary(ZeroCCCompiler *c, ZCPAddr *a);
static inline bool zc_cpe_cast_type(ZeroCCCompiler *c, int64_t *psz) {
    /* At `(`: parse `(type)` if the inside is a type name (plus pointer
     * stars: `(char *)`); sets *psz to the pointee size when the type is a
     * pointer. Non-consuming on fail. */
    size_t save = c->token_pos;
    if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_LPAREN) return false;
    if (!zero_cc_tok_is_type_start(c, c->token_pos + 1)) return false;
    c->token_pos++;
    ZeroCCType ty = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
    if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_RPAREN) {
        c->token_pos = save; return false;
    }
    c->token_pos++;
    if (ty.tag == TYPE_PTR && ty.base) *psz = ty.base->size > 0 ? (int64_t)ty.base->size : 1;
    else if (ty.size > 0) *psz = ty.size;
    else *psz = 1;
    return true;
}
static inline bool zc_cpe_primary(ZeroCCCompiler *c, ZCPAddr *a) {
    size_t save = c->token_pos;
    memset(a, 0, sizeof(*a));
    a->kind = -1;
    a->esz = 1;
    /* Optional cast prefix(es): `(char *) &...` / `((char *) &...)` */
    int64_t cast_esz = 0;
    for (;;) {
        int64_t psz = 0;
        bool at_amp = false;
        size_t p2 = c->token_pos;
        /* `(` cast `)` followed by & (or another `(`) */
        if (p2 < c->token_count && c->tokens[p2].type == TK_LPAREN &&
            zero_cc_tok_is_type_start(c, p2 + 1)) {
            size_t p3 = p2 + 1;
            (void)zero_cc_parse_type_name_at_pos(c, &p3);
            while (p3 < c->token_count && c->tokens[p3].type == TK_STAR) p3++;
            if (p3 < c->token_count && c->tokens[p3].type == TK_RPAREN &&
                p3 + 1 < c->token_count && c->tokens[p3 + 1].type == TK_AMP) {
                at_amp = true;
            }
        }
        if (!at_amp) break;
        if (!zc_cpe_cast_type(c, &psz)) break;
        cast_esz = psz;
    }
    if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_AMP) {
        /* A bare `ident [.member | [i]]*` primary (no &): an array or
         * struct-member global decays to its address (c-torture compile/
         * 930326-1: `long i = s.f - &s.b;`).  Only a known global
         * identifier is accepted, so ordinary constant expressions are
         * not swallowed.  A `(` primary is accepted too when the inside
         * is a global address expression - `(arr + N)` / `((arr + N)->m)`
         * (c-torture execute/builtins/mempcpy-2: `char *p = (char *)(arr
         * + 32);` - the initializer dispatch strips the cast prefix
         * before CPE runs). The LPAREN branch below validates the inside
         * and restores token_pos on any mismatch. */
        if (c->token_pos < c->token_count &&
            (c->tokens[c->token_pos].type == TK_IDENT &&
             find_global_var(c, c->tokens[c->token_pos].str_val) ||
             c->tokens[c->token_pos].type == TK_LPAREN)) {
            /* fall through to the IDENT / LPAREN branch below */
        } else {
            c->token_pos = save;
            return false;
        }
    } else {
        c->token_pos++;
    }
    if (cast_esz > 0) a->esz = cast_esz;
    if (c->token_pos >= c->token_count) { c->token_pos = save; return false; }
    if (c->tokens[c->token_pos].type == TK_STRING) {
        size_t ro_off = c->rodata_section.length;
        zero_cc_buf_put_bytes(&c->rodata_section,
            c->tokens[c->token_pos].allocated_str ? c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val,
            zero_cc_token_str_len(&c->tokens[c->token_pos]) + 1);
        snprintf(a->base_str, sizeof(a->base_str), "%s",
            c->tokens[c->token_pos].allocated_str ? c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val);
        c->token_pos++;
        a->kind = 0;
        a->ro_off = ro_off;
        a->esz = 1;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
            c->token_pos++;
            int64_t n = zero_cc_eval_const_expr(c);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
            a->off += n;
        }
        return true;
    }
    if (c->tokens[c->token_pos].type == TK_IDENT) {
        char nm[128];
        snprintf(nm, sizeof(nm), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos++;
        ZeroCCGlobalVar *gv = find_global_var(c, nm);
        int64_t esz = 1;
        if (gv) {
            a->kind = 1;
            a->gidx = (int)(gv - c->global_vars);
            a->off = (int64_t)gv->data_offset;
            esz = gv->type.size > 0 ? (int64_t)gv->type.size : 1;
            if (gv->type.tag == TYPE_ARRAY && gv->type.base) {
                esz = gv->type.base->size > 0 ? (int64_t)gv->type.base->size : esz;
            }
        } else {
            bool is_ext = false;
            for (size_t fi = 0; fi < c->function_count; fi++) {
                if (c->functions[fi].is_external &&
                    strcmp(c->functions[fi].name, nm) == 0) { is_ext = true; break; }
            }
            if (!is_ext) { c->token_pos = save; return false; }
            a->kind = 2;
            snprintf(a->ext_name, sizeof(a->ext_name), "%s", nm);
        }
        a->esz = esz;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
            c->token_pos++;
            int64_t n = zero_cc_eval_const_expr(c);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
            a->off += n * esz;
        }
        /* Dot/arrow member chains (`&v.d.b`, `&p->m`, pr43784): each member
         * adds its offset; the pointee type updates (arrays decay) for the
         * next step.  Arrow on an array/pointer decays to the struct. */
        if (gv) {
            ZeroCCType mt = gv->type;
            if (mt.tag == TYPE_ARRAY && mt.base) mt = *mt.base;
            for (;;) {
                if (c->token_pos >= c->token_count ||
                    (c->tokens[c->token_pos].type != TK_DOT &&
                     c->tokens[c->token_pos].type != TK_ARROW)) {
                    break;
                }
                bool arrow = (c->tokens[c->token_pos].type == TK_ARROW);
                c->token_pos++;
                if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) {
                    c->token_pos = save; return false;
                }
                const char *mnm = c->tokens[c->token_pos].str_val;
                c->token_pos++;
                /* Arrow requires a pointer-to-struct (or array); dot may see
                 * an array base which decays. */
                if (arrow && mt.tag == TYPE_ARRAY && mt.base) mt = *mt.base;
                if (arrow && mt.tag == TYPE_PTR && mt.base) mt = *mt.base;
                if (mt.tag == TYPE_ARRAY && mt.base) mt = *mt.base;
                if (mt.tag != TYPE_STRUCT && mt.tag != TYPE_UNION) {
                    c->token_pos = save; return false;
                }
                bool found = false;
                ZeroCCType next_mt = zero_cc_type_void();
                bool has_next = false;
                for (size_t mi = 0; mi < mt.member_count; mi++) {
                    if (strcmp(mt.members[mi].name, mnm) == 0) {
                        a->off += (int64_t)mt.members[mi].offset;
                        if (mt.members[mi].base) {
                            next_mt = *mt.members[mi].base;
                            has_next = true;
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) { c->token_pos = save; return false; }
                bool more_chain = (c->token_pos < c->token_count &&
                    (c->tokens[c->token_pos].type == TK_DOT ||
                     c->tokens[c->token_pos].type == TK_ARROW));
                if (more_chain && !has_next) { c->token_pos = save; return false; }
                mt = next_mt;
            }
        }
        return true;
    }
    if (c->tokens[c->token_pos].type == TK_LPAREN) {
        /* & ( balanced ) - the inside is an address expression:
         * IDENT (+N|-N)* (->member)?  (const-addr-expr-1). */
        c->token_pos++;
        int depth = 1;
        /* Nested opening parens: `((B + 1)->m)` / `(B + (N))`. */
        while (c->token_pos < c->token_count &&
               c->tokens[c->token_pos].type == TK_LPAREN) {
            depth++;
            c->token_pos++;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STRING) {
            size_t ro_off = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section,
                c->tokens[c->token_pos].allocated_str ? c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val,
                zero_cc_token_str_len(&c->tokens[c->token_pos]) + 1);
            snprintf(a->base_str, sizeof(a->base_str), "%s",
                c->tokens[c->token_pos].allocated_str ? c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val);
            c->token_pos++;
            a->kind = 0;
            a->ro_off = ro_off;
            a->esz = 1;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                c->token_pos++;
                int64_t n = zero_cc_eval_const_expr(c);
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                a->off += n;
            }
            while (depth > 0 && c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                depth--;
                c->token_pos++;
            }
            return true;
        }
        if (c->token_pos >= c->token_count ||
            c->tokens[c->token_pos].type != TK_IDENT) {
            c->token_pos = save; return false;
        }
        char base_nm[128];
        snprintf(base_nm, sizeof(base_nm), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos++;
        int64_t off = 0;
        ZeroCCGlobalVar *bgv = find_global_var(c, base_nm);
        int64_t esz = bgv && bgv->type.tag == TYPE_ARRAY && bgv->type.base ?
            (int64_t)(bgv->type.base->size > 0 ? bgv->type.base->size : 1) : 1;
        for (;;) {
            if (c->token_pos >= c->token_count) { c->token_pos = save; return false; }
            ZeroCCTokType tt = c->tokens[c->token_pos].type;
            if (tt == TK_PLUS || tt == TK_MINUS) {
                c->token_pos++;
                int64_t n = zero_cc_eval_const_expr(c);
                off += (tt == TK_PLUS ? n : -n) * esz;
            } else if (tt == TK_LBRACKET) {
                /* `&arr[i]` / `&x[2*8 + 2]` */
                c->token_pos++;
                int64_t n = zero_cc_eval_const_expr(c);
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                off += n * esz;
            } else if (tt == TK_RPAREN) {
                depth--;
                c->token_pos++;
                if (depth == 0) break;
            } else if (tt == TK_LPAREN) {
                depth++;
                c->token_pos++;
            } else if (tt == TK_ARROW || tt == TK_DOT) {
                /* `(B + N)->m` / `B.m` inside the parens
                 * (const-addr-expr-1: `&((arr + 1)->uaattrid)`). */
                bool arrow = (tt == TK_ARROW);
                c->token_pos++;
                if (c->token_pos >= c->token_count ||
                    c->tokens[c->token_pos].type != TK_IDENT) {
                    c->token_pos = save; return false;
                }
                const char *mnm = c->tokens[c->token_pos].str_val;
                c->token_pos++;
                if (!bgv) { c->token_pos = save; return false; }
                ZeroCCType mt = bgv->type;
                if (mt.tag == TYPE_ARRAY && mt.base) mt = *mt.base;
                if (arrow && mt.tag == TYPE_PTR && mt.base) mt = *mt.base;
                if (mt.tag != TYPE_STRUCT && mt.tag != TYPE_UNION) {
                    c->token_pos = save; return false;
                }
                bool mfound = false;
                for (size_t mi = 0; mi < mt.member_count; mi++) {
                    if (strcmp(mt.members[mi].name, mnm) == 0) {
                        off += (int64_t)mt.members[mi].offset;
                        mfound = true;
                        break;
                    }
                }
                if (!mfound) { c->token_pos = save; return false; }
            } else {
                c->token_pos = save; return false;
            }
        }
        if (depth != 0) { c->token_pos = save; return false; }
        if (!bgv) { c->token_pos = save; return false; }
        a->kind = 1;
        a->gidx = (int)(bgv - c->global_vars);
        a->off = (int64_t)bgv->data_offset + off;
        a->esz = esz;
        if (cast_esz > 0) a->esz = cast_esz;
        return true;
    }
    c->token_pos = save;
    return false;
}
/* Full expression: primary (('+'|'-') (primary | const-int))*;
 * two primaries of the same base -> integer difference (returns 2). */
static inline int zc_eval_const_ptr_expr(ZeroCCCompiler *c, ZCPAddr *out, int64_t *diff_out) {
    size_t save = c->token_pos;
    ZCPAddr a;
    if (!zc_cpe_primary(c, &a)) { c->token_pos = save; return 0; }
    for (;;) {
        if (c->token_pos >= c->token_count) break;
        ZeroCCTokType t = c->tokens[c->token_pos].type;
        if (t == TK_PLUS || t == TK_MINUS) {
            size_t p2 = c->token_pos + 1;
            bool minus = (t == TK_MINUS);
            /* A cast prefix before the & is fine: zc_cpe_primary handles
             * `(char *) &...` (c-torture compile/920928-1). */
            if (p2 < c->token_count &&
                (c->tokens[p2].type == TK_AMP || c->tokens[p2].type == TK_LPAREN)) {
                /* pointer - pointer: same base -> integer difference */
                ZCPAddr b;
                size_t before = c->token_pos;
                c->token_pos++;
                if (!zc_cpe_primary(c, &b)) { c->token_pos = before; break; }
                bool same_base =
                    (a.kind == b.kind) &&
                    (a.kind != 1 || a.gidx == b.gidx) &&
                    (a.kind != 0 || strcmp(a.base_str, b.base_str) == 0) &&
                    (a.kind != 2 || strcmp(a.ext_name, b.ext_name) == 0);
                if (!same_base) { c->token_pos = save; return 0; }
                /* Byte difference (c-torture forms cast to char*; the
                 * common case). */
                *diff_out = (a.off) - (b.off);
                return 2;
            }
            c->token_pos++;
            int64_t n = zero_cc_eval_const_expr(c);
            a.off += minus ? -n * a.esz : n * a.esz;
        } else if (t == TK_RPAREN) {
            /* closing an outer parenthesized initializer `(...)` */
            c->token_pos++;
            break;
        } else {
            break;
        }
    }
    *out = a;
    return 1;
}

/* `(...)`-parenthesized pointer-expression initializer: consume the outer
 * `(`, evaluate, and (if the evaluator did not consume its own closing
 * paren) consume one here. Returns 1 (pointer) / 2 (integer) / 0 (no match,
 * token_pos restored). */
static inline int zc_try_paren_ptr_init(ZeroCCCompiler *c, ZCPAddr *cpa, int64_t *diff_out) {
    size_t save = c->token_pos;
    if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_LPAREN) return 0;
    /* The leading paren may be a cast `(T *)` rather than a wrapper:
     * `(int *) &((arr + 1)->m)` (const-addr-expr-1).  Detect it and let
     * zc_eval's cast loop consume it in place. */
    size_t q = c->token_pos + 1;
    bool looks_cast = false;
    if (q < c->token_count && zero_cc_tok_is_type_start(c, q)) {
        size_t q2 = q;
        (void)zero_cc_parse_type_name_at_pos(c, &q2);
        while (q2 < c->token_count && c->tokens[q2].type == TK_STAR) q2++;
        if (q2 < c->token_count && c->tokens[q2].type == TK_RPAREN) looks_cast = true;
    }
    int r;
    if (looks_cast) {
        r = zc_eval_const_ptr_expr(c, cpa, diff_out);
    } else {
        c->token_pos++;
        r = zc_eval_const_ptr_expr(c, cpa, diff_out);
        if (r != 0 && c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }
    }
    if (r != 0) return r;
    c->token_pos = save;
    return 0;
}

#endif /* ZERO_CC_JIT_CONST_PTR_EVAL_H */
