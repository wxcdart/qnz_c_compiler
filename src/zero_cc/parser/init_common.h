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
                                              ZcInitTarget *out) {
    if (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) {
        if (!t->members) return false;
        size_t member_start = 0;
        for (size_t mi = 0; mi < t->member_count; mi++) {
            const ZeroCCStructMember *m = &t->members[mi];
            size_t leaves_m = 1;
            if (m->is_flexible) continue;
            zc_init_member_leaf_count(m, &leaves_m);
            if (*cursor < member_start + leaves_m) {
                size_t local = *cursor - member_start;
                out->in_member = true;
                if (m->tag == TYPE_ARRAY && m->base && m->base->size > 0) {
                    const ZeroCCType *el = m->base;
                    size_t per = zc_object_count_scalar_leaves(el);
                    if (per <= 1) {
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
                        size_t ei = local / per;
                        out->abs_off = base_off + m->offset + ei * el->size;
                        out->val_type = *el;
                        out->is_scalar = false;
                        out->scalar_sz = 0;
                        out->local_leaf = local % per;
                        out->advance = per;
                        if (el->tag == TYPE_ARRAY && el->base &&
                            (el->base->tag == TYPE_INT8 || el->base->tag == TYPE_UINT8)) {
                            out->arr_ctx_off = base_off + m->offset + ei * el->size;
                            out->arr_ctx_size = el->size;
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
    } else {
        *pos = c->token_pos;
        return true;
    }

    bool desig_err = false;
    bool first_step = true;
    for (;;) {
        if (c->token_pos + 1 < c->token_count &&
            c->tokens[c->token_pos].type == TK_DOT &&
            c->tokens[c->token_pos + 1].type == TK_IDENT) {
            /* member designator */
            c->token_pos += 2;
            const char *name = c->tokens[c->token_pos - 1].str_val;
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
 * unary +/-, parenthesized). Advances token_pos past the consumed value. */
static inline double zero_cc_eval_const_expr_float(ZeroCCCompiler *c) {
    double sign = 1.0;
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_PLUS || c->tokens[c->token_pos].type == TK_MINUS)) {
        if (c->tokens[c->token_pos].type == TK_MINUS) sign = -sign;
        c->token_pos++;
    }
    double v = 0.0;
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
        c->token_pos++;
        v = zero_cc_eval_const_expr_float(c);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_FLOAT) {
        v = c->tokens[c->token_pos].float_val;
        c->token_pos++;
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_NUM) {
        v = (double)c->tokens[c->token_pos].num_val;
        c->token_pos++;
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_CHAR_LIT) {
        v = (double)(int)c->tokens[c->token_pos].num_val;
        c->token_pos++;
    }
    return sign * v;
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
 * string pointers (element 0 of the object). */
static inline void zero_cc_emit_brace_init_global(ZeroCCCompiler *c, ZeroCCType obj_type,
                                                  uint8_t *buf, size_t buf_off, size_t buf_cap,
                                                  size_t data_patch_off, size_t *cursor) {
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) c->token_pos++;
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) {
        size_t iter_start = c->token_pos;
        bool has_desig = false;
        bool is_range = false;
        int64_t desig_lo = 0, desig_hi = 0;
        size_t step = 0;
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
            if (!zc_init_resolve_positional(&obj_type, &cur, buf_off, &tgt)) {
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
                                           transparent ? cursor : &sub_cursor);
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
                                                   buf_cap, data_patch_off, &sub_cursor);
                }
            }
        } else if (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type == TK_STRING) {
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
        } else {
            /* Scalar constant value (int/char/float/enum/sizeof). */
            bool is_f = (c->tokens[c->token_pos].type == TK_FLOAT ||
                         (tgt.val_type.tag == TYPE_FLOAT || tgt.val_type.tag == TYPE_DOUBLE));
            /* Elided braces: a scalar into an aggregate target fills the
             * first leaf of that aggregate (C99 6.7.9p14). */
            size_t off = tgt.abs_off;
            size_t w = tgt.is_scalar ? tgt.scalar_sz : 0;
            bool is_bf = tgt.is_bitfield;
            uint8_t boff = tgt.bit_offset;
            uint8_t bw = tgt.bit_width;
            if (!tgt.is_scalar) {
                size_t loff = 0, lsz = 0;
                zc_object_resolve_scalar_leaf_bf(&tgt.val_type, tgt.local_leaf, &loff, &lsz,
                                                 &is_bf, &boff, &bw, NULL);
                off += loff;
                w = lsz ? lsz : 4;
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
                    for (size_t i = 0; i < w && i < 8 && eo + i < buf_cap; i++) {
                        buf[eo + i] = (uint8_t)(bits >> (8 * i));
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
