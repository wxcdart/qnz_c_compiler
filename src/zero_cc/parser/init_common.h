/*
 * ZeroCC - Shared C99 6.7.8/6.7.9 aggregate-initializer designator engine
 * (Sprint 44)
 *
 * Both aggregate-initializer walkers must agree on designator semantics:
 *   - the AST parser (src/zero_cc/parser/ast_expr.h) stamps elements with
 *     their absolute placement in the outermost aggregate (ZC_INIT_ABS_TAG
 *     in desig_idx) for the lowering pass, and
 *   - the global/static walker (src/zero_cc/jit/compile_unit.h) writes
 *     constant values directly into the .data/.rodata buffer.
 *
 * This header centralizes the type-level resolution so both paths place
 * nested sub-aggregate designators (`.m = { .f = v }`, `.m[i]`, `[i..j]`
 * ranges, designator chains `.a.b = v`) at the same offsets.
 */
#ifndef ZERO_CC_PARSER_INIT_COMMON_H
#define ZERO_CC_PARSER_INIT_COMMON_H

#include "zero_cc/parser/parser.h"
#include "zero_cc/parser/const_eval.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/types/emit_helpers.h"

/* Probe if c->token_pos starts a compound-literal pointer initializer:
 *   [ (cast) ]* & [ ( ) ]* (Type) { ... } [ .member | [idx] ]*
 * Returns true if matched. */
static inline bool zc_probe_clit_init(ZeroCCCompiler *c, size_t pos,
                                      size_t *out_clit_type_pos,
                                      size_t *out_brace_pos) {
    size_t p = pos;
    while (p < c->token_count) {
        if (c->tokens[p].type == TK_AMP) {
            p++;
            continue;
        }
        if (c->tokens[p].type == TK_LPAREN) {
            if (p + 1 < c->token_count && zero_cc_tok_is_type_start(c, p + 1)) {
                size_t q = p + 1;
                size_t type_start = q;
                (void)zero_cc_parse_type_name_at_pos(c, &q);
                while (q < c->token_count && c->tokens[q].type == TK_STAR) q++;
                if (q < c->token_count && c->tokens[q].type == TK_RPAREN) {
                    if (q + 1 < c->token_count && c->tokens[q + 1].type == TK_LBRACE) {
                        /* Found (Type) { */
                        if (out_clit_type_pos) *out_clit_type_pos = type_start;
                        if (out_brace_pos) *out_brace_pos = q + 1;
                        return true;
                    }
                    /* It was a cast: (type) ... */
                    p = q + 1;
                    continue;
                }
            }
            /* Plain '(' wrapper */
            p++;
            continue;
        }
        break;
    }
    return false;
}

/* Materialize a member's value-target type (for recursion into
 * sub-aggregates).  A struct/union member keeps its full type in m->base;
 * an array member's element type in m->base with array_len derived from
 * the stored size. */
static inline ZeroCCType zc_init_member_value_type(const ZeroCCStructMember *m) {
    ZeroCCType t;
    memset(&t, 0, sizeof(t));
    if ((m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) && m->base) {
        t = *m->base;
    } else if (m->tag == TYPE_ARRAY && m->base) {
        t.tag = TYPE_ARRAY;
        t.base = m->base;
        t.array_len = (m->base->size > 0 && m->size >= m->base->size) ? m->size / m->base->size : 0;
        t.size = m->size;
        t.align = m->align;
    } else {
        t.tag = m->tag;
        t.size = m->size;
        t.align = m->align;
        t.base = m->base;
    }
    return t;
}

static inline bool zc_init_member_leaf_count(const ZeroCCStructMember *m, size_t *out) {
    if (m->is_complex) {
        /* C99 6.7.9: a complex member initializes from two scalar leaves
         * ({ re, im }); a single scalar sets (value, 0). */
        *out = 2;
        return true;
    }
    if (m->tag == TYPE_ARRAY && m->base && m->base->size > 0) {
        size_t count = m->size / m->base->size;
        *out = count * zc_object_count_scalar_leaves(m->base);
        if (*out == 0) *out = count;
        return true;
    }
    if ((m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) && m->base && m->base->member_count > 0) {
        *out = zc_object_count_scalar_leaves(m->base);
        if (*out == 0) *out = 1;
        return true;
    }
    *out = 1;
    return true;
}

/* Placement target of one initializer value at a brace-init level. */
typedef struct {
    size_t abs_off;      /* absolute byte offset of the value target */
    ZeroCCType val_type; /* type of the value target (aggregate or scalar) */
    bool is_scalar;      /* value target is a scalar leaf (grouping braces transparent) */
    size_t scalar_sz;    /* store width for a scalar value (1/2/4/8) */
    bool is_bitfield;    /* value target is a bitfield member */
    uint8_t bit_offset;  /* bit offset within the storage unit */
    uint8_t bit_width;   /* bit width of the bitfield */
    size_t local_leaf;   /* leaf index within an aggregate target (elided braces) */
    size_t leaf_idx;     /* leaf index (in the level) of the target start */
    bool in_member;      /* target is inside a struct/union member */
    size_t advance;      /* leaves consumed in this level by one value/sub-list */
    size_t arr_ctx_off;  /* start of the enclosing char array (string fills) */
    size_t arr_ctx_size; /* its size in bytes (0 = not a char-array context) */
} ZcInitTarget;

/* Leaf index of member mi within struct/union type st (sum of the leaf
 * counts of the preceding members). */
static inline size_t zc_init_member_leaf_start(const ZeroCCType *st, size_t mi) {
    size_t acc = 0;
    if (!st->members) return 0;
    for (size_t i = 0; i < mi && i < st->member_count; i++) {
        size_t lc;
        zc_init_member_leaf_count(&st->members[i], &lc);
        acc += lc;
    }
    return acc;
}

/* Positional (no designator) element at *cursor of type t: resolve the
 * value target.  For a struct/union member or an array element that is an
 * aggregate, the target is that sub-object (a nested brace list belongs
 * there); scalar leaves are direct value targets. */
static inline bool zc_init_resolve_positional(const ZeroCCType *t, size_t *cursor, size_t base_off,
                                              size_t fam_bytes, ZcInitTarget *out,
                                              const ZeroCCToken *tok, size_t tok_pos,
                                              size_t tok_count) {
    if (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) {
        if (!t->members) return false;
        size_t member_start = 0;
        for (size_t mi = 0; mi < t->member_count; mi++) {
            const ZeroCCStructMember *m = &t->members[mi];
            size_t leaves_m = 1;
            if (m->is_flexible) {
                /* C99 6.7.2.1p3: a trailing flexible array member receives
                 * the remaining initializer elements, placed after the
                 * struct's fixed portion (it adds nothing to sizeof).
                 * Reachable only once every fixed member is filled. */
                if (*cursor >= member_start && m->tag == TYPE_ARRAY &&
                    m->base && m->base->size > 0) {
                    const ZeroCCType *el = m->base;
                    size_t local = *cursor - member_start;
                    out->in_member = true;
                    out->abs_off = base_off + t->size + local * el->size;
                    out->val_type.tag = el->tag;
                    out->val_type.size = el->size;
                    out->val_type.align = el->align;
                    out->val_type.base = el->base;
                    out->is_scalar = true;
                    out->scalar_sz = (el->size > 8) ? 8 : (el->size ? el->size : 4);
                    out->advance = 1;
                    if (el->tag == TYPE_INT8 || el->tag == TYPE_UINT8) {
                        /* String-fill context: the caller sized the object
                         * for the whole FAM data run, so report it in full. */
                        out->arr_ctx_off = base_off + t->size;
                        out->arr_ctx_size = fam_bytes;
                    }
                    return true;
                }
                continue;
            }
            zc_init_member_leaf_count(m, &leaves_m);
            if (*cursor < member_start + leaves_m) {
                size_t local = *cursor - member_start;
                out->in_member = true;
                if (m->tag == TYPE_ARRAY && m->base && m->base->size > 0) {
                    const ZeroCCType *el = m->base;
                    size_t per = zc_object_count_scalar_leaves(el);
                    /* A designator nested inside the member's brace group
                     * (`{{ [0 ... 87] = 0xaa }}`) requires the WHOLE member
                     * as the value target so the range is broadcast per
                     * element (c-torture execute/memclr). Every other shape
                     * (plain scalars, strings, elided groups) keeps the
                     * historic element-`local` scalar target (pr87053). */
                    bool nested_desig = (per <= 1 && tok);
                    if (nested_desig) {
                        size_t p = tok_pos;
                        while (p < tok_count && tok[p].type == TK_LBRACE) p++;
                        nested_desig = (p < tok_count && tok[p].type == TK_LBRACKET);
                    }
                    if (per <= 1 && !nested_desig) {
                        out->abs_off = base_off + m->offset + local * el->size;
                        out->val_type.tag = el->tag;
                        out->val_type.size = el->size;
                        out->val_type.align = el->align;
                        out->val_type.base = el->base;
                        out->is_scalar = true;
                        out->scalar_sz = (el->size > 8) ? 8 : (el->size ? el->size : 4);
                        out->advance = 1;
                        if (el->tag == TYPE_INT8 || el->tag == TYPE_UINT8) {
                            out->arr_ctx_off = base_off + m->offset;
                            out->arr_ctx_size = m->size;
                        } else if (el->tag == TYPE_ARRAY && el->base &&
                                   (el->base->tag == TYPE_INT8 || el->base->tag == TYPE_UINT8)) {
                            out->arr_ctx_off = base_off + m->offset + local * el->size;
                            out->arr_ctx_size = el->size;
                        }
                    } else {
                        /* Array member with aggregate elements (`char m[2][2]`,
                         * `struct S0 m2[2]`), or nested designators: the value
                         * target is the WHOLE member, so a nested brace list
                         * ({ {row0}, {row1} }) iterates the elements exactly
                         * like a top-level array does (c-torture pr60017).
                         * Scalar values (elided braces) address leaves by
                         * `local` within the member. */
                        ZeroCCType full = zc_init_member_value_type(m);
                        out->abs_off = base_off + m->offset;
                        out->val_type = full;
                        out->is_scalar = false;
                        out->scalar_sz = 0;
                        out->local_leaf = local;
                        out->advance = leaves_m;
                        if (el->tag == TYPE_INT8 || el->tag == TYPE_UINT8) {
                            out->arr_ctx_off = base_off + m->offset;
                            out->arr_ctx_size = m->size;
                        } else if (el->tag == TYPE_ARRAY && el->base &&
                                   (el->base->tag == TYPE_INT8 || el->base->tag == TYPE_UINT8)) {
                            out->arr_ctx_off = base_off + m->offset;
                            out->arr_ctx_size = m->size;
                        }
                    }
                } else if ((m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) &&
                           m->base && m->base->member_count > 0) {
                    out->abs_off = base_off + m->offset;
                    out->val_type = *m->base;
                    out->is_scalar = false;
                    out->scalar_sz = 0;
                    out->local_leaf = local;
                    out->advance = leaves_m;
                } else if (m->is_complex) {
                    /* C99 6.7.9: complex member = two scalar halves.
                     * The value target is the whole member modeled as an
                     * array of two element scalars, so `{ re, im }` iterates
                     * the halves and a bare scalar initializes (value, 0). */
                    out->abs_off = base_off + m->offset;
                    memset(&out->val_type, 0, sizeof(out->val_type));
                    out->val_type.tag = TYPE_ARRAY;
                    out->val_type.size = m->size;
                    out->val_type.align = m->align;
                    out->val_type.array_len = 2;
                    ZeroCCType cx_el;
                    memset(&cx_el, 0, sizeof(cx_el));
                    cx_el.tag = m->tag;
                    cx_el.size = m->size / 2;
                    cx_el.align = m->align;
                    out->val_type.base = zero_cc_type_alloc_base(&cx_el);
                    out->is_scalar = false;
                    out->scalar_sz = 0;
                    out->local_leaf = local;
                    out->advance = leaves_m;
                } else {
                    out->abs_off = base_off + m->offset;
                    out->val_type.tag = m->tag;
                    out->val_type.size = m->size;
                    out->val_type.align = m->align;
                    out->val_type.base = m->base;
                    out->is_scalar = true;
                    out->scalar_sz = (m->size > 8) ? 8 : (m->size ? m->size : 4);
                    out->is_bitfield = m->is_bitfield;
                    out->bit_offset = m->bit_offset;
                    out->bit_width = m->bit_width;
                    out->advance = 1;
                }
                return true;
            }
            member_start += leaves_m;
        }
        return false;
    }
    if (t->tag == TYPE_ARRAY) {
        const ZeroCCType *el = t->base;
        if (!el || el->size == 0) return false;
        size_t per = zc_object_count_scalar_leaves(el);
        if (per <= 1) {
            /* array_len == 0: unsized array (T a[] = { ... }) - unbounded */
            if (t->array_len > 0 && *cursor >= t->array_len) return false;
            out->abs_off = base_off + (*cursor) * el->size;
            out->val_type.tag = el->tag;
            out->val_type.size = el->size;
            out->val_type.align = el->align;
            out->val_type.base = el->base;
            out->is_scalar = true;
            out->scalar_sz = (el->size > 8) ? 8 : (el->size ? el->size : 4);
            out->advance = 1;
            if (el->tag == TYPE_INT8 || el->tag == TYPE_UINT8) {
                out->arr_ctx_off = base_off;
                out->arr_ctx_size = t->size;
            } else if (el->tag == TYPE_ARRAY && el->base &&
                       (el->base->tag == TYPE_INT8 || el->base->tag == TYPE_UINT8)) {
                out->arr_ctx_off = base_off + (*cursor) * el->size;
                out->arr_ctx_size = el->size;
            }
        } else {
            size_t ei = *cursor / per;
            if (t->array_len > 0 && ei >= t->array_len) return false;
            out->abs_off = base_off + ei * el->size;
            out->val_type = *el;
            out->is_scalar = false;
            out->scalar_sz = 0;
            out->local_leaf = *cursor % per;
            out->advance = per;
            if (el->tag == TYPE_ARRAY && el->base &&
                (el->base->tag == TYPE_INT8 || el->base->tag == TYPE_UINT8)) {
                out->arr_ctx_off = base_off + ei * el->size;
                out->arr_ctx_size = el->size;
            }
        }
        return true;
    }
    if (t->tag == TYPE_USER_0 && zero_cc_type_is_vector(t)) {
        /* GNU vector: behave like a fixed-length array of lanes. */
        const ZeroCCType *el = t->base;
        if (!el || el->size == 0) return false;
        size_t per = zc_object_count_scalar_leaves(el);
        if (per <= 1) {
            if (t->array_len > 0 && *cursor >= t->array_len) return false;
            out->abs_off = base_off + (*cursor) * el->size;
            out->val_type.tag = el->tag;
            out->val_type.size = el->size;
            out->val_type.align = el->align;
            out->val_type.base = el->base;
            out->is_scalar = true;
            out->scalar_sz = (el->size > 8) ? 8 : (el->size ? el->size : 4);
            out->advance = 1;
        } else {
            size_t ei = *cursor / per;
            if (t->array_len > 0 && ei >= t->array_len) return false;
            out->abs_off = base_off + ei * el->size;
            out->val_type = *el;
            out->is_scalar = false;
            out->scalar_sz = 0;
            out->local_leaf = *cursor % per;
            out->advance = per;
        }
        return true;
    }
    /* C99 6.7.9: a top-level complex scalar initializes from two halves:
     * `_Complex double z = { re, im };` (a bare scalar yields (v, 0) in
     * the elided-leaf path below). */
    if (zero_cc_type_is_complex(t)) {
        size_t esz = (t->size >= 2) ? t->size / 2 : (t->base ? t->base->size : 8);
        if (*cursor >= 2) return false;
        out->abs_off = base_off + *cursor * esz;
        out->val_type.tag = t->tag;
        out->val_type.size = esz;
        out->val_type.align = t->align;
        out->val_type.base = NULL;
        out->is_scalar = true;
        out->scalar_sz = (esz > 8) ? 8 : (esz ? esz : 4);
        out->advance = 1;
        return true;
    }
    if (*cursor == 0) {
        out->abs_off = base_off;
        out->val_type = *t;
        out->is_scalar = true;
        out->scalar_sz = (t->size > 8) ? 8 : (t->size ? t->size : 4);
        out->advance = 1;
        return true;
    }
    return false;
}

/* Parse C99 6.7.8 designators against an object of obj_type located at
 * base_off within the outermost aggregate.
 *
 * A designator is any interleaving of member (`.name`) and element
 * (`[index]`) designators: `.a.b`, `.a[3]`, `.a[3].b`, `[5]`, `[1 ... 2]`.
 * A range `[lo ... hi]` may only appear in the final element designator;
 * the caller applies the value (or re-parses a sub-list) once per element.
 *
 * On entry *pos points at the first designator token (TK_DOT or
 * TK_LBRACKET) or at the value token (no designator).  On success *pos
 * points at the value token (any '=' already consumed) and returns true:
 *   - no designator: *has_desig stays false, *tgt zeroed;
 *   - designator: *has_desig = true, *tgt filled (abs_off is the start of
 *     the final value target; for a range that is element lo; *step holds
 *     the element stride when a range was the final element designator).
 * On a designator error a diagnostic is emitted, *pos is left at the
 * failing token, and false is returned (the caller performs recovery).
 *
 * All token access goes through c->token_pos (constant-expression
 * evaluation also consumes from there); the caller passes a pointer to
 * c->token_pos. */
static bool zc_init_parse_designators(ZeroCCCompiler *c, size_t *pos,
                                      const ZeroCCType *obj_type, size_t base_off,
                                      ZcInitTarget *tgt, bool *has_desig,
                                      bool *is_range, int64_t *lo, int64_t *hi,
                                      size_t *step) {
    /* abs_off accumulates from the level base so recursed sub-lists
     * (base_off != 0) resolve member offsets correctly. */
    tgt->abs_off = base_off;
    tgt->val_type = *obj_type;
    tgt->is_scalar = (obj_type->tag != TYPE_STRUCT && obj_type->tag != TYPE_UNION &&
                      obj_type->tag != TYPE_ARRAY);
    tgt->scalar_sz = tgt->is_scalar ?
        ((obj_type->size > 8) ? 8 : (obj_type->size ? obj_type->size : 4)) : 0;
    tgt->advance = 0;
    tgt->leaf_idx = 0;
    tgt->arr_ctx_off = 0;
    tgt->arr_ctx_size = 0;
    *has_desig = false;
    *is_range = false;
    *lo = 0;
    *hi = 0;
    *step = 0;

    if (c->token_pos + 1 < c->token_count &&
        c->tokens[c->token_pos].type == TK_DOT &&
        c->tokens[c->token_pos + 1].type == TK_IDENT) {
        *has_desig = true;
    } else if (c->token_pos < c->token_count &&
               c->tokens[c->token_pos].type == TK_LBRACKET) {
        *has_desig = true;
    } else if (c->token_pos + 1 < c->token_count &&
               c->tokens[c->token_pos].type == TK_IDENT &&
               c->tokens[c->token_pos + 1].type == TK_COLON) {
        /* GNU member designator: `field: value` */
        *has_desig = true;
    } else {
        *pos = c->token_pos;
        return true;
    }

    bool desig_err = false;
    bool first_step = true;
    for (;;) {
        if ((c->token_pos + 1 < c->token_count &&
             c->tokens[c->token_pos].type == TK_DOT &&
             c->tokens[c->token_pos + 1].type == TK_IDENT) ||
            (c->token_pos + 1 < c->token_count &&
             c->tokens[c->token_pos].type == TK_IDENT &&
             c->tokens[c->token_pos + 1].type == TK_COLON)) {
            /* member designator (.field or field:) */
            bool is_colon = (c->tokens[c->token_pos].type == TK_IDENT);
            const char *name = is_colon ? c->tokens[c->token_pos].str_val : c->tokens[c->token_pos + 1].str_val;
            c->token_pos += 2;
            ZeroCCType *cur = &tgt->val_type;
            bool found = false;
            if (cur->tag == TYPE_STRUCT || cur->tag == TYPE_UNION) {
                for (size_t mi = 0; mi < cur->member_count; mi++) {
                    if (strcmp(cur->members[mi].name, name) == 0) {
                        const ZeroCCStructMember *m = &cur->members[mi];
                        if (m->is_flexible) {
                            diag_report_error_at_token(c,
                                "initialization of flexible array member '%s'", name);
                            desig_err = true;
                            break;
                        }
                        tgt->abs_off += m->offset;
                        tgt->leaf_idx += zc_init_member_leaf_start(cur, mi);
                        ZeroCCType vt = zc_init_member_value_type(m);
                        size_t adv;
                        zc_init_member_leaf_count(m, &adv);
                        tgt->val_type = vt;
                        tgt->advance = adv;
                        tgt->is_scalar = (m->tag != TYPE_STRUCT && m->tag != TYPE_UNION &&
                                          m->tag != TYPE_ARRAY);
                        tgt->scalar_sz = tgt->is_scalar ?
                            ((m->size > 8) ? 8 : (m->size ? m->size : 4)) : 0;
                        tgt->is_bitfield = tgt->is_scalar && m->is_bitfield;
                        tgt->bit_offset = tgt->is_bitfield ? m->bit_offset : 0;
                        tgt->bit_width = tgt->is_bitfield ? m->bit_width : 0;
                        if (m->is_complex) {
                            /* C99 6.7.9: complex member = two scalar halves
                             * (`.a = { re, im }`), same as positional path. */
                            memset(&tgt->val_type, 0, sizeof(tgt->val_type));
                            tgt->val_type.tag = TYPE_ARRAY;
                            tgt->val_type.size = m->size;
                            tgt->val_type.align = m->align;
                            tgt->val_type.array_len = 2;
                            ZeroCCType cx_el;
                            memset(&cx_el, 0, sizeof(cx_el));
                            cx_el.tag = m->tag;
                            cx_el.size = m->size / 2;
                            cx_el.align = m->align;
                            tgt->val_type.base = zero_cc_type_alloc_base(&cx_el);
                            tgt->is_scalar = false;
                            tgt->scalar_sz = 0;
                        }
                        if (m->tag == TYPE_ARRAY && m->base &&
                            (m->base->tag == TYPE_INT8 || m->base->tag == TYPE_UINT8)) {
                            tgt->arr_ctx_off = base_off + (tgt->abs_off - m->offset);
                            tgt->arr_ctx_size = m->size;
                        } else if (m->tag == TYPE_ARRAY && m->base &&
                                   m->base->tag == TYPE_ARRAY && m->base->base &&
                                   (m->base->base->tag == TYPE_INT8 ||
                                    m->base->base->tag == TYPE_UINT8)) {
                            tgt->arr_ctx_off = base_off + (tgt->abs_off - m->offset);
                            tgt->arr_ctx_size = m->base->size; /* first row */
                        }
                        *lo = 0; *hi = 0; *is_range = false; *step = 0;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                diag_report_error_at_token(c,
                    "designated initializer names a non-existent member");
                desig_err = true;
            }
            first_step = false;
        } else if (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type == TK_LBRACKET) {
            /* element designator (or [N] member designation at struct level) */
            c->token_pos++; /* [ */
            int64_t dlo = zero_cc_eval_const_expr(c);
            int64_t dhi = dlo;
            bool d_range = false;
            if (c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_ELLIPSIS) {
                c->token_pos++;
                dhi = zero_cc_eval_const_expr(c);
                d_range = true;
            }
            if (c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
            if (dlo < 0 || dhi < dlo) {
                diag_report_error_at_token(c,
                    "invalid range in designated initializer: %ld ... %ld",
                    (long)dlo, (long)dhi);
                desig_err = true;
            } else if (tgt->val_type.tag == TYPE_ARRAY) {
                const ZeroCCType *el = tgt->val_type.base;
                if (!el || el->size == 0) {
                    desig_err = true;
                } else if (tgt->val_type.array_len > 0 &&
                           (size_t)dhi >= tgt->val_type.array_len) {
                    diag_report_error_at_token(c,
                        "array initializer index %ld exceeds array bound %zu",
                        (long)dhi, tgt->val_type.array_len);
                    desig_err = true;
                } else {
                    size_t per = zc_object_count_scalar_leaves(el);
                    size_t arr_start = tgt->abs_off;
                    size_t arr_size = tgt->val_type.size;
                    tgt->abs_off += (size_t)dlo * el->size;
                    tgt->leaf_idx += (size_t)dlo * per;
                    ZeroCCType vt = *el;
                    tgt->val_type = vt;
                    tgt->is_scalar = (per <= 1);
                    tgt->scalar_sz = (per <= 1) ?
                        ((el->size > 8) ? 8 : (el->size ? el->size : 4)) : 0;
                    tgt->is_bitfield = false;
                    tgt->bit_offset = 0;
                    tgt->bit_width = 0;
                    tgt->advance = (per > 1) ? per : 1;
                    if (d_range) {
                        *step = el->size;
                    }
                    if (per <= 1 && (el->tag == TYPE_INT8 || el->tag == TYPE_UINT8) &&
                        arr_size >= (size_t)dlo * el->size) {
                        tgt->arr_ctx_off = arr_start + (size_t)dlo * el->size;
                        tgt->arr_ctx_size = arr_size - (size_t)dlo * el->size;
                    }
                }
                *lo = dlo; *hi = dhi; *is_range = d_range;
            } else if ((tgt->val_type.tag == TYPE_STRUCT || tgt->val_type.tag == TYPE_UNION) &&
                       first_step) {
                /* C99 6.7.9: [N] at struct level designates member N */
                if ((size_t)dlo >= tgt->val_type.member_count) {
                    diag_report_error_at_token(c,
                        "designated initializer index %ld exceeds member count",
                        (long)dlo);
                    desig_err = true;
                } else {
                    const ZeroCCStructMember *m = &tgt->val_type.members[dlo];
                    if (m->is_flexible) {
                        diag_report_error_at_token(c,
                            "initialization of flexible array member '%s'", m->name);
                        desig_err = true;
                    } else {
                        tgt->abs_off = base_off + m->offset;
                        tgt->leaf_idx = zc_init_member_leaf_start(&tgt->val_type, dlo);
                        ZeroCCType vt = zc_init_member_value_type(m);
                        size_t adv;
                        zc_init_member_leaf_count(m, &adv);
                        tgt->val_type = vt;
                        tgt->advance = adv;
                        tgt->is_scalar = (m->tag != TYPE_STRUCT && m->tag != TYPE_UNION &&
                                          m->tag != TYPE_ARRAY);
                        tgt->scalar_sz = tgt->is_scalar ?
                            ((m->size > 8) ? 8 : (m->size ? m->size : 4)) : 0;
                        tgt->is_bitfield = tgt->is_scalar && m->is_bitfield;
                        tgt->bit_offset = tgt->is_bitfield ? m->bit_offset : 0;
                        tgt->bit_width = tgt->is_bitfield ? m->bit_width : 0;
                        if (m->is_complex) {
                            /* C99 6.7.9: complex member = two scalar halves
                             * (`.a = { re, im }`), same as positional path. */
                            memset(&tgt->val_type, 0, sizeof(tgt->val_type));
                            tgt->val_type.tag = TYPE_ARRAY;
                            tgt->val_type.size = m->size;
                            tgt->val_type.align = m->align;
                            tgt->val_type.array_len = 2;
                            ZeroCCType cx_el;
                            memset(&cx_el, 0, sizeof(cx_el));
                            cx_el.tag = m->tag;
                            cx_el.size = m->size / 2;
                            cx_el.align = m->align;
                            tgt->val_type.base = zero_cc_type_alloc_base(&cx_el);
                            tgt->is_scalar = false;
                            tgt->scalar_sz = 0;
                        }
                        *lo = 0; *hi = 0; *is_range = false; *step = 0;
                    }
                }
                first_step = false;
            } else {
                diag_report_error_at_token(c,
                    "array index designator on a non-array member");
                desig_err = true;
            }
        } else {
            break;
        }
        if (desig_err) break;
    }

    if (desig_err) return false;
    /* Consume '=' after designators */
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) c->token_pos++;
    *pos = c->token_pos;
    return true;
}

/* Evaluate a floating-point constant initializer (literal, char literal,
 * unary plus/minus, parenthesized, binary add/sub/mul/div - e.g.
 * `+1.0/0.0`, `1.0/0.0 - 1.0/0.0` (c-torture execute/ieee/fp-cmp-1,
 * mzero2)).  Advances token_pos past the consumed expression. */
static double zc_eval_const_fp_additive(ZeroCCCompiler *c);

/* IEEE-754 exact negation = flip bit 63.  `v *= -1.0` is NOT equivalent
 * for NaN: on x86-64 `mulsd` clears the sign bit of a NaN result, so a
 * constant like `-__builtin_nan("")` lost its sign (c-torture
 * execute/ieee/copysign1/2 compare copysign's bit-surgery result against
 * the table with memcmp).  Sign-flip is exact for every class (±0,
 * ±inf, ±NaN). */
static inline double zc_fp_negate(double v) {
    uint64_t b;
    memcpy(&b, &v, sizeof(b));
    b ^= 0x8000000000000000ULL;
    memcpy(&v, &b, sizeof(v));
    return v;
}
static double zc_eval_const_fp_multiplicative(ZeroCCCompiler *c) {
    double v = 0.0;
    double sign = 1.0;
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_PLUS || c->tokens[c->token_pos].type == TK_MINUS)) {
        if (c->tokens[c->token_pos].type == TK_MINUS) sign = -sign;
        c->token_pos++;
    }
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
        if (c->token_pos + 1 < c->token_count &&
            zero_cc_tok_is_type_start(c, c->token_pos + 1)) {
            /* Cast prefix: `(type) operand` - skip the cast type and
             * evaluate the operand (Sprint 68: pr83222 `1 + (double) a`). */
            c->token_pos++;
            zero_cc_parse_type_name_at_pos(c, &c->token_pos);
            while (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
            if (c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            v = zc_eval_const_fp_additive(c);
        } else {
            c->token_pos++;
            v = zc_eval_const_fp_additive(c);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        }
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_FLOAT) {
        v = c->tokens[c->token_pos].float_val;
        c->token_pos++;
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_NUM) {
        v = (double)c->tokens[c->token_pos].num_val;
        c->token_pos++;
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_CHAR_LIT) {
        v = (double)(int)c->tokens[c->token_pos].num_val;
        c->token_pos++;
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
        /* FP special constants (Sprint 70, c-torture ieee/copysign1/2):
         * __builtin_inf() / __builtin_inff() -> +/-INFINITY, __builtin_nan(s)
         * / __builtin_nanf(s) -> quiet NaN. The string argument (when
         * present) is consumed and ignored; the call parens are optional
         * in the constant context. */
        const char *nm = c->tokens[c->token_pos].str_val;
        if (strcmp(nm, "__builtin_inf") == 0 || strcmp(nm, "__builtin_inff") == 0 ||
            strcmp(nm, "__builtin_infl") == 0 ||
            strcmp(nm, "__builtin_nan") == 0 || strcmp(nm, "__builtin_nanf") == 0 ||
            strcmp(nm, "__builtin_nanl") == 0) {
            bool is_nan = (strcmp(nm, "__builtin_nan") == 0 ||
                           strcmp(nm, "__builtin_nanf") == 0 ||
                           strcmp(nm, "__builtin_nanl") == 0);
            c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                int pd = 1;
                c->token_pos++;
                while (c->token_pos < c->token_count && pd > 0) {
                    if (c->tokens[c->token_pos].type == TK_LPAREN) pd++;
                    else if (c->tokens[c->token_pos].type == TK_RPAREN) pd--;
                    c->token_pos++;
                }
            }
            uint64_t fpbits = is_nan ? 0x7FF8000000000000ULL : 0x7FF0000000000000ULL;
            memcpy(&v, &fpbits, sizeof(v));
        } else {
        /* Earlier global's value: `double d = (double) a;` where
         * `const char a = 0x42;` (Sprint 68). */
        bool g_is_f = false;
        int64_t g_i = 0;
        double g_d = 0.0;
        if (zc_const_global_scalar(c, c->tokens[c->token_pos].str_val, &g_is_f, &g_i, &g_d)) {
            v = g_is_f ? g_d : (double)g_i;
            c->token_pos++;
        }
        }
    }
    if (sign < 0.0) v = zc_fp_negate(v);
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_STAR || c->tokens[c->token_pos].type == TK_SLASH)) {
        bool div = (c->tokens[c->token_pos].type == TK_SLASH);
        c->token_pos++;
        double sign2 = 1.0;
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_PLUS || c->tokens[c->token_pos].type == TK_MINUS)) {
            if (c->tokens[c->token_pos].type == TK_MINUS) sign2 = -sign2;
            c->token_pos++;
        }
        double r = 0.0;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_FLOAT) {
            r = c->tokens[c->token_pos].float_val;
            c->token_pos++;
        } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_NUM) {
            r = (double)c->tokens[c->token_pos].num_val;
            c->token_pos++;
        } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
            r = zc_eval_const_fp_additive(c);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        }
        if (sign2 < 0.0) r = zc_fp_negate(r);
        if (div) v = v / r; else v = v * r;
    }
    return v;
}
static double zc_eval_const_fp_additive(ZeroCCCompiler *c) {
    double v = zc_eval_const_fp_multiplicative(c);
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_PLUS || c->tokens[c->token_pos].type == TK_MINUS)) {
        bool sub = (c->tokens[c->token_pos].type == TK_MINUS);
        c->token_pos++;
        double r = zc_eval_const_fp_multiplicative(c);
        if (sub) v = v - r; else v = v + r;
    }
    return v;
}
static inline double zero_cc_eval_const_expr_float(ZeroCCCompiler *c) {
    return zc_eval_const_fp_additive(c);
}

/* C99 6.7.2.3 complex constant initializer: evaluate the expression as a
 * (re, im) pair - imaginary literals (`1.0i`), the four binary
 * arithmetic ops, unary +/-, parens, cast prefixes, and earlier global
 * references.  Advances token_pos past the consumed expression; returns
 * 1 on success, 0 when the current token does not start a constant
 * expression (token_pos restored).  Used for file-scope complex objects
 * and static local complex initializers. */
static int zc_eval_const_cx_primary(ZeroCCCompiler *c, double *re, double *im);
static int zc_eval_const_cx_multiplicative(ZeroCCCompiler *c, double *re, double *im) {
    size_t save = c->token_pos;
    double v = 0.0, s = 0.0;
    if (!zc_eval_const_cx_primary(c, &v, &s)) { c->token_pos = save; return 0; }
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_STAR || c->tokens[c->token_pos].type == TK_SLASH)) {
        bool div = (c->tokens[c->token_pos].type == TK_SLASH);
        c->token_pos++;
        double r = 0.0, t = 0.0;
        if (!zc_eval_const_cx_primary(c, &r, &t)) { c->token_pos = save; return 0; }
        if (div) {
            double d = r * r + t * t;
            double nr = (v * r + s * t) / d;
            double ni = (s * r - v * t) / d;
            v = nr; s = ni;
        } else {
            double nr = v * r - s * t;
            double ni = v * t + s * r;
            v = nr; s = ni;
        }
    }
    *re = v; *im = s;
    return 1;
}
static int zc_eval_const_cx_additive(ZeroCCCompiler *c, double *re, double *im) {
    size_t save = c->token_pos;
    double v = 0.0, s = 0.0;
    if (!zc_eval_const_cx_multiplicative(c, &v, &s)) { c->token_pos = save; return 0; }
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_PLUS || c->tokens[c->token_pos].type == TK_MINUS)) {
        bool sub = (c->tokens[c->token_pos].type == TK_MINUS);
        c->token_pos++;
        double r = 0.0, t = 0.0;
        if (!zc_eval_const_cx_multiplicative(c, &r, &t)) { c->token_pos = save; return 0; }
        if (sub) { v -= r; s -= t; } else { v += r; s += t; }
    }
    *re = v; *im = s;
    return 1;
}
static int zc_eval_const_cx_primary(ZeroCCCompiler *c, double *re, double *im) {
    size_t save = c->token_pos;
    double v = 0.0, s = 0.0;
    bool ok = true;
    double sign = 1.0;
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_PLUS || c->tokens[c->token_pos].type == TK_MINUS)) {
        if (c->tokens[c->token_pos].type == TK_MINUS) sign = -sign;
        c->token_pos++;
    }
    if (c->token_pos >= c->token_count) ok = false;
    else if (c->tokens[c->token_pos].type == TK_LPAREN) {
        c->token_pos++;
        if (c->token_pos < c->token_count &&
            zero_cc_tok_is_type_start(c, c->token_pos)) {
            /* Cast prefix: skip the cast type, evaluate the operand. */
            zero_cc_parse_type_name_at_pos(c, &c->token_pos);
            while (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
            if (c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            ok = zc_eval_const_cx_additive(c, &v, &s);
        } else {
            ok = zc_eval_const_cx_additive(c, &v, &s);
            if (c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            else ok = false;
        }
    } else if (c->tokens[c->token_pos].type == TK_FLOAT) {
        v = c->tokens[c->token_pos].float_val;
        s = 0.0;
        if ((c->tokens[c->token_pos].int_suffix & 8) != 0) { s = v; v = 0.0; }
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_NUM) {
        v = (double)c->tokens[c->token_pos].num_val;
        s = 0.0;
        if ((c->tokens[c->token_pos].int_suffix & 8) != 0) { s = v; v = 0.0; }
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_CHAR_LIT) {
        v = (double)(int)c->tokens[c->token_pos].num_val;
        s = 0.0;
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_IDENT) {
        bool g_is_f = false;
        int64_t g_i = 0;
        double g_d = 0.0;
        if (zc_const_global_scalar(c, c->tokens[c->token_pos].str_val, &g_is_f, &g_i, &g_d)) {
            v = g_is_f ? g_d : (double)g_i;
            s = 0.0;
            c->token_pos++;
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }
    if (!ok) {
        c->token_pos = save;
        return 0;
    }
    if (sign < 0.0) { v = zc_fp_negate(v); s = zc_fp_negate(s); }
    *re = v; *im = s;
    return 1;
}
static inline int zero_cc_eval_const_expr_complex(ZeroCCCompiler *c, double *re, double *im) {
    return zc_eval_const_cx_additive(c, re, im);
}

/* C99 6.7.2.1p3: a trailing flexible-array member is initialized from the
 * LAST top-level element of the brace list and placed after the struct's
 * fixed portion, so the object's allocation must be grown by the bytes that
 * element contributes: string literal -> len+1; brace list -> item count x
 * element size; single scalar -> element size. Returns 0 when the struct has
 * no trailing flexible member or no initializer brace list is present.
 * Pure token scan (no evaluation, token_pos restored). */
static inline size_t zc_measure_fam_init_bytes(ZeroCCCompiler *c, const ZeroCCType *st) {
    if (!st || (st->tag != TYPE_STRUCT && st->tag != TYPE_UNION) || !st->members) return 0;
    size_t fam_esz = 0;
    for (size_t mi = 0; mi < st->member_count; mi++) {
        const ZeroCCStructMember *m = &st->members[mi];
        if (m->is_flexible && m->tag == TYPE_ARRAY && m->base && m->base->size > 0)
            fam_esz = m->base->size;
    }
    if (fam_esz == 0) return 0;
    if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_LBRACE) return 0;
    size_t pos = c->token_pos + 1; /* past '{' */
    size_t last_start = pos;
    int depth = 0;
    while (pos < c->token_count) {
        int tt = c->tokens[pos].type;
        if (tt == TK_LBRACE) { depth++; pos++; }
        else if (tt == TK_RBRACE) { if (depth == 0) break; depth--; pos++; }
        else if (tt == TK_COMMA && depth == 0) { last_start = pos + 1; pos++; }
        else pos++;
    }
    size_t bytes = 0;
    if (last_start < c->token_count) {
        int tt = c->tokens[last_start].type;
        if (tt == TK_STRING) {
            bytes = zero_cc_token_str_len(&c->tokens[last_start]) + 1;
        } else if (tt == TK_LBRACE) {
            size_t p = last_start + 1;
            int d = 0;
            size_t count = 0;
            bool any = false;
            while (p < c->token_count) {
                int t2 = c->tokens[p].type;
                if (t2 == TK_LBRACE) d++;
                else if (t2 == TK_RBRACE) { if (d == 0) break; d--; }
                else if (d == 0) {
                    if (t2 == TK_COMMA) { if (any) count++; }
                    else if (t2 != TK_ASSIGN && t2 != TK_DOT) any = true;
                }
                p++;
            }
            if (any) count++;
            bytes = count * fam_esz;
        } else if (tt != TK_RBRACE) {
            bytes = fam_esz; /* single scalar (char literal / int / enum) */
        }
    }
    return bytes;
}

/* Sprint 44: recursive aggregate-initializer writer for global/static
 * initializers (C99 6.7.8/6.7.9).  Fills buf[buf_off .. buf_off+obj_type.size)
 * from the brace list at c->token_pos (pointing at the opening '{').
 *
 * Nested sub-aggregate designators (`.m = { .f = v }`), element designators
 * (`.m[i]`, `[i..j]` ranges), designator chains (`.a.b = v`) and elided
 * braces all resolve through the shared engine in parser/init_common.h,
 * keeping offsets identical to the AST/lowering path.
 * `data_patch_off` is the .data offset that receives data-reloc patches for
 * string pointers (element 0 of the object).
 * `fam_bytes` is the extra storage a trailing flexible-array member (C99
 * 6.7.2.1p3) may write past the struct's fixed size (0 when the object has
 * no such member or it is not initialized); the object's allocation must
 * already include it. */
static inline bool zc_probe_cast_amp(ZeroCCCompiler *c) {
    size_t cast_probe = c->token_pos;
    while (cast_probe < c->token_count && c->tokens[cast_probe].type == TK_LPAREN &&
           zero_cc_tok_is_type_start(c, cast_probe + 1)) {
        size_t tp = cast_probe + 1;
        (void)zero_cc_parse_type_name_at_pos(c, &tp);
        while (tp < c->token_count && c->tokens[tp].type == TK_STAR) tp++;
        if (tp < c->token_count && c->tokens[tp].type == TK_RPAREN) {
            cast_probe = tp + 1;
        } else {
            break;
        }
    }
    return (cast_probe < c->token_count &&
            (c->tokens[cast_probe].type == TK_AMP ||
             (c->tokens[cast_probe].type == TK_LOGAND && cast_probe + 1 < c->token_count &&
              c->tokens[cast_probe + 1].type == TK_IDENT)));
}

static inline void zero_cc_emit_brace_init_global(ZeroCCCompiler *c, ZeroCCType obj_type,
                                                  uint8_t *buf, size_t buf_off, size_t buf_cap,
                                                  size_t data_patch_off, size_t *cursor,
                                                  size_t fam_bytes) {
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) c->token_pos++;
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) {
        size_t iter_start = c->token_pos;
        bool has_desig = false;
        bool is_range = false;
        int64_t desig_lo = 0, desig_hi = 0;
        size_t step = 0;
        size_t clit_t_pos = 0, clit_b_pos = 0;
        ZcInitTarget tgt;
        memset(&tgt, 0, sizeof(tgt));
        if (!zc_init_parse_designators(c, &c->token_pos, &obj_type, buf_off, &tgt,
                                       &has_desig, &is_range, &desig_lo, &desig_hi, &step)) {
            int depth = 0;
            while (c->token_pos < c->token_count) {
                if (c->tokens[c->token_pos].type == TK_LBRACE) depth++;
                else if (c->tokens[c->token_pos].type == TK_RBRACE) {
                    if (depth == 0) break;
                    depth--;
                }
                c->token_pos++;
            }
            break;
        }
        if (!has_desig) {
            size_t cur = cursor ? *cursor : 0;
            if (!zc_init_resolve_positional(&obj_type, &cur, buf_off, fam_bytes, &tgt,
                                            c->tokens, c->token_pos, c->token_count)) {
                /* Excess elements: warn, stop (the rest stays zero). */
                diag_report_warning_at_token(c, "excess elements in aggregate initializer");
                int depth = 0;
                while (c->token_pos < c->token_count) {
                    if (c->tokens[c->token_pos].type == TK_LBRACE) depth++;
                    else if (c->tokens[c->token_pos].type == TK_RBRACE) {
                        if (depth == 0) break;
                        depth--;
                    }
                    c->token_pos++;
                }
                break;
            }
            if (cursor) *cursor = cur;
        }

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
            /* Sub-aggregate value; transparent only for POSITIONAL scalar
             * targets (grouping braces, C99 6.7.9).  Designated scalar
             * targets (`.arr[i] = { v }`) take the sub-list path so that
             * ranges re-apply the value per element. */
            bool transparent = tgt.is_scalar && !has_desig;
            ZeroCCType sub_type = transparent ? obj_type : tgt.val_type;
            size_t sub_start = c->token_pos;
            size_t sub_cursor = 0;
            zero_cc_emit_brace_init_global(c, sub_type, buf,
                                           transparent ? buf_off : tgt.abs_off,
                                           buf_cap, data_patch_off,
                                           transparent ? cursor : &sub_cursor,
                                           fam_bytes);
            if (cursor && !transparent) {
                if (has_desig) {
                    size_t cnt = is_range ? (size_t)(desig_hi - desig_lo + 1) : 1;
                    size_t nc = tgt.leaf_idx + cnt * tgt.advance;
                    if (nc > *cursor) *cursor = nc;
                } else {
                    *cursor += tgt.advance;
                }
            }
            if (is_range && !transparent) {
                for (int64_t k = desig_lo + 1; k <= desig_hi; k++) {
                    c->token_pos = sub_start;
                    sub_cursor = 0;
                    zero_cc_emit_brace_init_global(c, sub_type, buf,
                                                   tgt.abs_off + (size_t)(k - desig_lo) * step,
                                                   buf_cap, data_patch_off, &sub_cursor,
                                                   fam_bytes);
                }
            }
        } else if (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type == TK_STRING &&
                   !(c->token_pos + 2 < c->token_count &&
                     c->tokens[c->token_pos + 1].type == TK_PLUS &&
                     c->tokens[c->token_pos + 2].type == TK_NUM)) {
            if (tgt.arr_ctx_size > 0) {
                /* String into a char array: copy (NUL-terminated when it
                 * fits), starting at the value target. */
                const char *sv = c->tokens[c->token_pos].allocated_str ?
                    c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                size_t sl = zero_cc_token_str_len(&c->tokens[c->token_pos]);
                size_t off_in_ctx = (tgt.abs_off >= tgt.arr_ctx_off) ?
                    (tgt.abs_off - tgt.arr_ctx_off) : 0;
                size_t cap = (off_in_ctx < tgt.arr_ctx_size) ?
                    (tgt.arr_ctx_size - off_in_ctx) : 0;
                size_t copy = (sl + 1 <= cap) ? (sl + 1) : cap;
                size_t base = tgt.abs_off;
                for (size_t bi = 0; bi < copy; bi++) {
                    if (base + bi < buf_cap) buf[base + bi] = (bi < sl) ? (uint8_t)sv[bi] : 0;
                }
                c->token_pos++;
                if (cursor) {
                    /* A string fully consumes an aggregate char-array
                     * target (row / member) and a char-array member; into
                     * a top-level char-array element it consumes just the
                     * copied bytes. */
                    size_t str_adv = tgt.is_scalar ?
                        (tgt.in_member ? tgt.arr_ctx_size : copy) : tgt.advance;
                    if (has_desig) {
                        size_t nc = tgt.leaf_idx + str_adv;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += str_adv;
                    }
                }
            } else if (tgt.val_type.tag == TYPE_PTR ||
                       (tgt.is_scalar && tgt.val_type.tag == TYPE_INT64 &&
                        obj_type.tag == TYPE_ARRAY &&
                        obj_type.base && obj_type.base->tag == TYPE_PTR)) {
                /* Pointer element: string literal -> rodata address patched
                 * by a data reloc (the slot stays zero until patched). */
                const char *sv = c->tokens[c->token_pos].allocated_str ?
                    c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                size_t ro_off = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, sv, zero_cc_token_str_len(&c->tokens[c->token_pos]) + 1);
                c->token_pos++;
                if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                    ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                    dr->data_patch_offset = data_patch_off + tgt.abs_off;
                    dr->target_kind = 0; /* .rodata target */
                    dr->rodata_offset = ro_off;
                    dr->target_data_offset = 0;
                    dr->target_global_idx = -1;
                }
                if (cursor) {
                    if (has_desig) {
                        size_t nc = tgt.leaf_idx + 1;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += tgt.advance;
                    }
                }
            } else {
                c->token_pos++;
                if (cursor) {
                    if (has_desig) {
                        size_t nc = tgt.leaf_idx + 1;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += 1;
                    }
                }
            }
        } else if (c->token_pos + 2 < c->token_count &&
               c->tokens[c->token_pos + 1].type == TK_PLUS &&
               ((c->tokens[c->token_pos].type == TK_STRING &&
                 c->tokens[c->token_pos + 2].type == TK_NUM) ||
                (c->tokens[c->token_pos].type == TK_NUM &&
                 c->tokens[c->token_pos + 2].type == TK_STRING)) &&
               (tgt.val_type.tag == TYPE_PTR ||
                (tgt.is_scalar && tgt.val_type.tag == TYPE_INT64 &&
                 obj_type.tag == TYPE_ARRAY &&
                 obj_type.base && obj_type.base->tag == TYPE_PTR))) {
        /* C99 6.6 pointer constant expression with an integer offset
         * ("foo" + 1, c-torture pr53084): the string's rodata address
         * is only known at emit time, so emit it as a data reloc with
         * target_data_offset carrying the offset. */
        size_t str_idx = (c->tokens[c->token_pos].type == TK_STRING) ?
            c->token_pos : c->token_pos + 2;
        int64_t off = c->tokens[c->token_pos + ((str_idx == c->token_pos) ? 2 : 0)].num_val;
        const char *sv = c->tokens[str_idx].allocated_str ?
            c->tokens[str_idx].allocated_str : c->tokens[str_idx].str_val;
        size_t ro_off = c->rodata_section.length;
        zero_cc_buf_put_bytes(&c->rodata_section, sv, zero_cc_token_str_len(&c->tokens[str_idx]) + 1);
        c->token_pos += 3; /* N + "str" | "str" + N */
        if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
            ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
            dr->data_patch_offset = data_patch_off + tgt.abs_off;
            dr->target_kind = 0; /* .rodata target */
            dr->rodata_offset = ro_off;
            dr->target_data_offset = (size_t)off;
            dr->target_global_idx = -1;
        }
        if (cursor) {
            if (has_desig) {
                size_t nc = tgt.leaf_idx + 1;
                if (nc > *cursor) *cursor = nc;
            } else {
                *cursor += tgt.advance;
            }
        }
        } else if ((c->tokens[c->token_pos].type == TK_AMP ||
                    (c->tokens[c->token_pos].type == TK_LOGAND && c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_IDENT) ||
                   (c->tokens[c->token_pos].type == TK_LPAREN && zc_probe_cast_amp(c))) &&
                   !zc_probe_clit_init(c, c->token_pos, NULL, NULL)) {
            while (c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
                (void)zero_cc_parse_type_name_at_pos(c, &c->token_pos);
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_LOGAND &&
                c->token_pos + 1 < c->token_count &&
                c->tokens[c->token_pos + 1].type == TK_IDENT) {
                const char *lbl_name = c->tokens[c->token_pos + 1].str_val;
                c->token_pos += 2; /* consume '&&' and ident */
                const char *lbl_name2 = NULL;
                if (c->token_pos + 2 < c->token_count &&
                    c->tokens[c->token_pos].type == TK_MINUS &&
                    c->tokens[c->token_pos + 1].type == TK_LOGAND &&
                    c->tokens[c->token_pos + 2].type == TK_IDENT) {
                    lbl_name2 = c->tokens[c->token_pos + 2].str_val;
                    c->token_pos += 3; /* consume '-', '&&', and ident */
                }
                if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                    ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                    dr->data_patch_offset = data_patch_off + tgt.abs_off;
                    if (lbl_name2) {
                        dr->target_kind = 6; /* code label difference (&&l1 - &&l2) */
                        dr->rodata_offset = tgt.scalar_sz ? tgt.scalar_sz : 4;
                        snprintf(dr->target_extern_name, sizeof(dr->target_extern_name), "%s - %s", lbl_name, lbl_name2);
                    } else {
                        dr->target_kind = 5; /* code label target */
                        dr->rodata_offset = 0;
                        snprintf(dr->target_extern_name, sizeof(dr->target_extern_name), "%s", lbl_name);
                    }
                    dr->target_data_offset = 0; /* resolved after function IR lowering */
                    dr->target_global_idx = (c->function_count > 0) ? (int)(c->function_count - 1) : 0;
                }
                if (cursor) {
                    if (has_desig) {
                        size_t nc = tgt.leaf_idx + 1;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += tgt.advance;
                    }
                }
            } else {
            size_t ap = c->token_pos + 1; /* past '&' */
            int pdepth = 0;
            while (ap < c->token_count && c->tokens[ap].type == TK_LPAREN) {
                pdepth++;
                ap++;
            }
            if (ap < c->token_count && c->tokens[ap].type == TK_STRING) {
                const char *sv = c->tokens[ap].allocated_str ?
                    c->tokens[ap].allocated_str : c->tokens[ap].str_val;
                size_t ro_off = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, sv, zero_cc_token_str_len(&c->tokens[ap]) + 1);
                ap++;
                size_t addoff = 0;
                if (ap < c->token_count && c->tokens[ap].type == TK_LBRACKET) {
                    ap++;
                    c->token_pos = ap;
                    int64_t n = zero_cc_eval_const_expr(c);
                    ap = c->token_pos;
                    if (ap < c->token_count && c->tokens[ap].type == TK_RBRACKET) ap++;
                    if (n > 0) addoff = (size_t)n;
                }
                while (pdepth > 0 && ap < c->token_count && c->tokens[ap].type == TK_RPAREN) {
                    pdepth--;
                    ap++;
                }
                c->token_pos = ap;
                if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                    ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                    dr->data_patch_offset = data_patch_off + tgt.abs_off;
                    dr->target_kind = 0; /* .rodata target */
                    dr->rodata_offset = ro_off;
                    dr->target_data_offset = addoff;
                    dr->target_global_idx = -1;
                }
                if (cursor) {
                    if (has_desig) {
                        size_t nc = tgt.leaf_idx + 1;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += tgt.advance;
                    }
                }
            } else if (ap < c->token_count && c->tokens[ap].type == TK_IDENT) {
                char refn[128];
                snprintf(refn, sizeof(refn), "%s", c->tokens[ap].str_val);
                ZeroCCGlobalVar *rgv = find_global_var(c, refn);
                ap++; /* past the ident */
                size_t addoff = 0;
                if (rgv && ap < c->token_count && c->tokens[ap].type == TK_LBRACKET) {
                    ZeroCCType rtt = rgv->type;
                    int64_t resz = (rtt.tag == TYPE_ARRAY && rtt.base && rtt.base->size > 0) ?
                        (int64_t)rtt.base->size : (int64_t)(rtt.size > 0 ? rtt.size : 8);
                    ap++; /* past '[' */
                    c->token_pos = ap;
                    int64_t n = zero_cc_eval_const_expr(c);
                    ap = c->token_pos;
                    if (ap < c->token_count && c->tokens[ap].type == TK_RBRACKET) ap++;
                    addoff = (size_t)(n * (resz > 0 ? resz : 8));
                }
                while (pdepth > 0 && ap < c->token_count && c->tokens[ap].type == TK_RPAREN) {
                    pdepth--;
                    ap++;
                }
                if (rgv) {
                    c->token_pos = ap;
                    if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                        ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                        dr->data_patch_offset = data_patch_off + tgt.abs_off;
                        dr->target_kind = 1; /* .data global target */
                        dr->rodata_offset = 0;
                        dr->target_data_offset = (addoff > 0)
                            ? rgv->data_offset + addoff : (size_t)-1; /* global base */
                        dr->target_global_idx = (int)(rgv - c->global_vars);
                    }
                    if (cursor) {
                        if (has_desig) {
                            size_t nc = tgt.leaf_idx + 1;
                            if (nc > *cursor) *cursor = nc;
                        } else {
                            *cursor += tgt.advance;
                        }
                    }
                } else {
                    /* Not a known global: skip & ident (zero-fill, no reloc). */
                    c->token_pos = ap;
                    if (cursor) {
                        if (has_desig) {
                            size_t nc = tgt.leaf_idx + 1;
                            if (nc > *cursor) *cursor = nc;
                        } else {
                            *cursor += tgt.advance;
                        }
                    }
                }
            } else {
                c->token_pos = ap;
            }
            }
        } else if (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type == TK_IDENT &&
                   find_global_var(c, c->tokens[c->token_pos].str_val) &&
                   (tgt.val_type.tag == TYPE_PTR || tgt.scalar_sz == 8 ||
                    find_global_var(c, c->tokens[c->token_pos].str_val)->type.tag == TYPE_ARRAY)) {
            /* Global array or variable decay to pointer in aggregate initializer (C99 6.3.2.1p3) */
            char refn[128];
            snprintf(refn, sizeof(refn), "%s", c->tokens[c->token_pos].str_val);
            ZeroCCGlobalVar *rgv = find_global_var(c, refn);
            c->token_pos++;
            size_t addoff = 0;
            if (rgv && c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                ZeroCCType rtt = rgv->type;
                int64_t resz = (rtt.tag == TYPE_ARRAY && rtt.base && rtt.base->size > 0) ?
                    (int64_t)rtt.base->size : (int64_t)(rtt.size > 0 ? rtt.size : 8);
                c->token_pos++; /* past '[' */
                int64_t n = zero_cc_eval_const_expr(c);
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                if (n > 0) addoff = (size_t)(n * resz);
            }
            if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                dr->data_patch_offset = data_patch_off + tgt.abs_off;
                int gidx = -1;
                for (size_t gi = 0; gi < c->global_var_count; gi++) {
                    if (strcmp(c->global_vars[gi].name, refn) == 0) { gidx = (int)gi; break; }
                }
                dr->target_kind = 1; /* global variable target */
                dr->rodata_offset = 0;
                dr->target_data_offset = rgv->data_offset + addoff;
                dr->target_global_idx = gidx;
            }
            if (cursor) {
                if (has_desig) {
                    size_t nc = tgt.leaf_idx + 1;
                    if (nc > *cursor) *cursor = nc;
                } else {
                    *cursor += tgt.advance;
                }
            }
        } else if (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type == TK_IDENT &&
                   zero_cc_find_function(c, c->tokens[c->token_pos].str_val)) {
            /* Function name as a pointer member initializer (C99 6.7.9: a
             * function designator decays to a pointer).  Emit a kind-3
             * data reloc against the function symbol (c-torture
             * execute/ieee/fp-cmp-5/8: `static struct { int (*pos)(...); }
             * t[] = { { f, g }, ... };`). */
            char fnm[128];
            snprintf(fnm, sizeof(fnm), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                dr->data_patch_offset = data_patch_off + tgt.abs_off;
                dr->target_kind = 3; /* function symbol */
                dr->rodata_offset = 0;
                dr->target_data_offset = 0;
                dr->target_global_idx = -1;
                snprintf(dr->target_extern_name, sizeof(dr->target_extern_name), "%s", fnm);
            }
            if (cursor) {
                if (has_desig) {
                    size_t nc = tgt.leaf_idx + 1;
                    if (nc > *cursor) *cursor = nc;
                } else {
                    *cursor += tgt.advance;
                }
            }
        } else if (zc_probe_clit_init(c, c->token_pos, &clit_t_pos, &clit_b_pos)) {
            /* Compound literal in aggregate initializer (c-torture compile/init-1, compile/pr64067) */
            size_t tp = clit_t_pos;
            ZeroCCType clit_type = zero_cc_parse_type_name_at_pos(c, &tp);
            while (tp < c->token_count && c->tokens[tp].type == TK_STAR) {
                clit_type = zero_cc_type_ptr(&clit_type);
                tp++;
            }
            static int s_anon_clit_g_id = 0;
            char anon_name[48];
            snprintf(anon_name, sizeof(anon_name), "__clit_anon_g_%d", s_anon_clit_g_id++);

            size_t clit_sz = clit_type.size ? clit_type.size : 8;
            size_t clit_al = clit_type.align ? clit_type.align : 8;
            size_t clit_pad = (clit_al - (c->data_section.length % clit_al)) % clit_al;
            for (size_t p = 0; p < clit_pad; p++) zero_cc_emit_u8(&c->data_section, 0);
            size_t anon_offset = c->data_section.length;
            for (size_t p = 0; p < clit_sz; p++) zero_cc_emit_u8(&c->data_section, 0);

            c->token_pos = clit_b_pos; /* at '{' */
            uint8_t *abuf = calloc(clit_sz > 0 ? clit_sz : 1, 1);
            size_t clit_cursor = 0;
            zero_cc_emit_brace_init_global(c, clit_type, abuf, 0, clit_sz, anon_offset, &clit_cursor, 0);
            memcpy((uint8_t *)c->data_section.data + anon_offset, abuf, clit_sz);
            free(abuf);
            int anon_idx = add_global_var(c, anon_name, clit_type, anon_offset, clit_sz, true, true);

            size_t extra_offset = 0;
            ZeroCCType cur_t = clit_type;
            while (c->token_pos < c->token_count) {
                if (c->tokens[c->token_pos].type == TK_DOT || c->tokens[c->token_pos].type == TK_ARROW) {
                    bool is_arrow = (c->tokens[c->token_pos].type == TK_ARROW);
                    c->token_pos++;
                    if (is_arrow && cur_t.tag == TYPE_PTR && cur_t.base) cur_t = *cur_t.base;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                        const char *mn = c->tokens[c->token_pos].str_val;
                        c->token_pos++;
                        bool mfound = false;
                        if (cur_t.tag == TYPE_STRUCT || cur_t.tag == TYPE_UNION) {
                            for (size_t mi = 0; mi < cur_t.member_count; mi++) {
                                if (strcmp(cur_t.members[mi].name, mn) == 0) {
                                    extra_offset += cur_t.members[mi].offset;
                                    if (cur_t.members[mi].base) cur_t = *cur_t.members[mi].base;
                                    else cur_t.tag = cur_t.members[mi].tag;
                                    mfound = true;
                                    break;
                                }
                            }
                        }
                        if (!mfound) break;
                    } else {
                        break;
                    }
                } else if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                    c->token_pos++;
                    int64_t idx = zero_cc_eval_const_expr(c);
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                    size_t elsz = (cur_t.base && cur_t.base->size > 0) ? cur_t.base->size : 4;
                    extra_offset += (size_t)idx * elsz;
                    if (cur_t.base) cur_t = *cur_t.base;
                } else if (c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++;
                } else {
                    break;
                }
            }

            for (size_t b = 0; b < 8 && tgt.abs_off + b < buf_cap; b++) {
                buf[tgt.abs_off + b] = 0;
            }

            if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                dr->data_patch_offset = data_patch_off + tgt.abs_off;
                dr->target_kind = 1; /* .data global target */
                dr->rodata_offset = 0;
                dr->target_data_offset = anon_offset + extra_offset;
                dr->target_global_idx = anon_idx;
            }
            if (cursor) {
                if (has_desig) {
                    size_t nc = tgt.leaf_idx + 1;
                    if (nc > *cursor) *cursor = nc;
                } else {
                    *cursor += tgt.advance;
                }
            }
        } else {
            /* Scalar constant value (int/char/float/enum/sizeof). */
            bool is_f = (c->tokens[c->token_pos].type == TK_FLOAT ||
                         (tgt.val_type.tag == TYPE_FLOAT || tgt.val_type.tag == TYPE_DOUBLE));
            /* Elided braces: a scalar into an aggregate target fills the
             * first leaf of that aggregate (C99 6.7.9p14). */
            size_t off = tgt.abs_off;
            size_t w = tgt.is_scalar ? tgt.scalar_sz : 0;
            size_t wfull = w; /* pre-clamp object width (16 for long double) */
            bool is_bf = tgt.is_bitfield;
            uint8_t boff = tgt.bit_offset;
            uint8_t bw = tgt.bit_width;
            if (!tgt.is_scalar) {
                size_t loff = 0, lsz = 0;
                zc_object_resolve_scalar_leaf_bf(&tgt.val_type, tgt.local_leaf, &loff, &lsz,
                                                 &is_bf, &boff, &bw, NULL);
                off += loff;
                w = lsz ? lsz : 4;
                wfull = w;
                if (cursor) {
                    if (has_desig) {
                        size_t cnt = is_range ? (size_t)(desig_hi - desig_lo + 1) : 1;
                        size_t nc = tgt.leaf_idx + cnt;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += 1; /* one leaf, not the whole sub-object */
                    }
                }
            } else {
                if (w > 8) w = 8;
                if (w == 0) w = 4;
                if (cursor) {
                    if (has_desig) {
                        size_t cnt = is_range ? (size_t)(desig_hi - desig_lo + 1) : 1;
                        size_t nc = tgt.leaf_idx + cnt;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += tgt.advance;
                    }
                }
            }
            size_t wv = (w < 8) ? w : 8; /* value width actually written */
            if (is_f) {
                double dv = zero_cc_eval_const_expr_float(c);
                uint64_t bits = 0;
                if (w <= 4) {
                    float fv = (float)dv;
                    memcpy(&bits, &fv, sizeof(fv));
                } else {
                    memcpy(&bits, &dv, sizeof(dv));
                }
                for (int64_t k = 0; k <= (is_range ? (desig_hi - desig_lo) : 0); k++) {
                    size_t eo = off + (size_t)k * step;
                    for (size_t i = 0; i < wv && eo + i < buf_cap; i++) {
                        buf[eo + i] = (uint8_t)(bits >> (8 * i));
                    }
                    /* A long double member is 16 bytes but the value is an
                     * 8-byte double - zero the top half so full-size
                     * memcmps see clean storage (c-torture ieee/copysign*). */
                    for (size_t i = wv; i < wfull && eo + i < buf_cap; i++) {
                        buf[eo + i] = 0;
                    }
                }
            } else {
                int64_t iv = zero_cc_eval_const_expr(c);
                for (int64_t k = 0; k <= (is_range ? (desig_hi - desig_lo) : 0); k++) {
                    size_t eo = off + (size_t)k * step;
                    if (is_bf) {
                        uint64_t cur = 0;
                        for (size_t i = 0; i < w && i < 8 && eo + i < buf_cap; i++) {
                            cur |= ((uint64_t)buf[eo + i]) << (8 * i);
                        }
                        uint64_t vmask = (bw >= 64) ? ~0ULL : ((1ULL << bw) - 1);
                        uint64_t fmask = (bw >= 64) ? ~0ULL : (vmask << boff);
                        cur = (cur & ~fmask) | ((((uint64_t)iv) & vmask) << boff);
                        for (size_t i = 0; i < w && i < 8 && eo + i < buf_cap; i++) {
                            buf[eo + i] = (uint8_t)(cur >> (8 * i));
                        }
                    } else {
                        for (size_t i = 0; i < w && i < 8 && eo + i < buf_cap; i++) {
                            buf[eo + i] = (uint8_t)((uint64_t)iv >> (8 * i));
                        }
                    }
                }
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
            c->token_pos++;
        } else if (c->token_pos == iter_start ||
                   (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE)) {
            diag_report_error_at_token(c, "unexpected token in brace initializer");
            int depth = 0;
            while (c->token_pos < c->token_count) {
                if (c->tokens[c->token_pos].type == TK_LBRACE) depth++;
                else if (c->tokens[c->token_pos].type == TK_RBRACE) {
                    if (depth == 0) break;
                    depth--;
                }
                c->token_pos++;
            }
            break;
        }
    }
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
}

#endif
