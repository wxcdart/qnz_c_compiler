/* AST Lowering - Binary Operator, Unary Operator, and Cast Lowering (AST to IR) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"

/* Branchless fabs of a double-width FP-model temp (IEEE sign-bit clear).
 * The scalar FP model always holds doubles, so float-complex parts work
 * through the same path. Used by the scaled complex division to compute
 * the scaling factor k = |br| + |bi| without branches. */
static int zc_cx_fabs(ZeroCCCompiler *c, int ftemp) {
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
    c->current_stack_frame += 8;
    int32_t slot = c->current_stack_frame;
    ir_emit_fstore((uint16_t)ftemp, TEMP_NONE, -slot);
    int it = c->next_temp_id++;
    ir_emit_load(it, BE(c)->fp_reg, -slot);
    int mk = c->next_temp_id++;
    ir_emit_mov_imm(mk, 0x7FFFFFFFFFFFFFFFLL);
    int at = c->next_temp_id++;
    ir_emit_and(at, it, mk);
    ir_emit_store(at, BE(c)->fp_reg, -slot);
    int rt = c->next_temp_id++;
    ir_emit_fload((uint16_t)rt, TEMP_NONE, -slot);
    return rt;
}

/* Branchless FP select of two double-width FP-model temps: returns va if
 * cond != 0, else vb (bit-level select on the IEEE bits, the same
 * technique __builtin_shuffle uses for vector lanes). */
static int zc_cx_fselect(ZeroCCCompiler *c, int cond, int va, int vb) {
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
    c->current_stack_frame += 16;
    int32_t s_a = c->current_stack_frame;
    int32_t s_b = s_a - 8;
    ir_emit_fstore((uint16_t)va, TEMP_NONE, -s_a);
    ir_emit_fstore((uint16_t)vb, TEMP_NONE, -s_b);
    int ia = c->next_temp_id++;
    ir_emit_load(ia, BE(c)->fp_reg, -s_a);
    int ib = c->next_temp_id++;
    ir_emit_load(ib, BE(c)->fp_reg, -s_b);
    int ones = c->next_temp_id++;
    ir_emit_mov_imm(ones, (int32_t)-1);
    int m = c->next_temp_id++;
    ir_emit_mul(m, cond, ones);          /* -1 or 0 */
    int nm = c->next_temp_id++;
    ir_emit_xor(nm, m, ones);            /* ~m */
    int sa = c->next_temp_id++;
    ir_emit_and(sa, ia, m);
    int sb = c->next_temp_id++;
    ir_emit_and(sb, ib, nm);
    int r = c->next_temp_id++;
    ir_emit_or(r, sa, sb);
    ir_emit_store(r, BE(c)->fp_reg, -s_a);
    int rt = c->next_temp_id++;
    ir_emit_fload((uint16_t)rt, TEMP_NONE, -s_a);
    return rt;
}

/* Infer the C99 narrow-int width of an operand expression when it can be
 * determined cheaply: 1/2/4 (with *sign set) or 0 (unknown / 8-byte /
 * pointer / non-int). Used for binop result-width marking and signed >>.
 * Covers the common operand shapes; anything exotic returns 0 (no mark,
 * conservative). */
static int zero_cc_operand_int_width(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                     ASTRef operand, int *sign) {
    *sign = 1;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, operand);
    if (!n) return 0;
    switch (n->tag) {
        case NODE_INT: {
            int64_t v = n->int_val;
            /* Suffix (saved in flags at parse time: bit0=u, bit1=l,
             * bit2=ll) fixes the literal's type per C11 6.4.4.1, even
             * when the value fits in a narrower type: 1ULL is long long
             * (64-bit), 0xFFFFFFFF is unsigned int (not signed int). */
            uint8_t suf = (uint8_t)(n->flags & 7u);
            if (suf & 6u) {              /* l / ll: 64-bit on LP64 */
                *sign = !(suf & 1u);
                return 0;                /* no narrowing */
            }
            if (suf & 1u) {              /* u: unsigned int, or unsigned
                                          * long (64-bit) if too large */
                *sign = 0;
                return (v > INT64_C(0xFFFFFFFF)) ? 0 : 4;
            }
            if (v >= INT32_MIN && v <= INT32_MAX) { *sign = 1; return 4; }
            if (v > INT32_MAX && v <= INT64_C(0xFFFFFFFF)) { *sign = 0; return 4; }
            return 0;  /* 64-bit literal: no narrowing */
        }
        case NODE_BOOL:  *sign = 0; return 1;  /* _Bool is 1-byte, 0/1 */
        case NODE_NULLPTR: return 0;  /* 8-byte pointer value */
        case NODE_UNOP:
            if (n->aux.op == TK_BANG) { *sign = 1; return 4; }
            /* &expr is a pointer (8 bytes) - no narrow-int width. Reporting
             * the pointed-to value's width here would make a binop like
             * `&arr[0] + i` get marked as 32-bit int math and the MC layer
             * would sign-extend the 64-bit pointer result (SEXT32),
             * truncating it. */
            if (n->aux.op == TK_AMP || n->aux.op == '&') return 0;
            return zero_cc_operand_int_width(c, arena, n->left, sign);
        case NODE_CAST: {
            TypeInfo *ti = ast_node_get_typeinfo(c, n);
            if (ti && type_is_integer(ti)) {
                size_t sz = type_size(ti);
                if (sz < 4) { *sign = 1; return 4; }
                if (sz == 4) { *sign = type_is_unsigned(ti) ? 0 : 1; return 4; }
            }
            return 0;
        }
        case NODE_IDENT: {
            ZeroCCSym *sym = zero_cc_find_local(c, n->s.str);
            if (sym) return zero_cc_narrow_int_width(&sym->type, sign);
            ZeroCCGlobalVar *gv = find_global_var(c, n->s.str);
            if (gv) return zero_cc_narrow_int_width(&gv->type, sign);
            return 0;
        }
        default: {
            TypeInfo *ti = ast_node_get_typeinfo(c, n);
            if (ti && type_is_integer(ti)) {
                size_t sz = type_size(ti);
                if (sz < 4) {
                    *sign = 1;
                    return 4;
                }
                if (sz == 4) {
                    *sign = type_is_unsigned(ti) ? 0 : 1;
                    return 4;
                }
            }
            return 0;
        }
    }
}

static int zero_cc_ztype_int_info(const ZeroCCType *zt, int *is_unsigned) {
    if (!zt) { *is_unsigned = 0; return 4; }
    if (zt->tag == TYPE_UINT32) { *is_unsigned = 1; return 4; }
    if (zt->tag == TYPE_UINT64 || zt->tag == TYPE_PTR || zt->tag == TYPE_ARRAY) {
        *is_unsigned = 1;
        return 8;
    }
    if (zt->tag == TYPE_INT64) { *is_unsigned = 0; return 8; }
    *is_unsigned = 0;
    return 4;
}

/* Determine promoted integer width (4 or 8) and signedness (*is_unsigned)
 * of an expression for C99 usual arithmetic conversions on /, %, <, <=, >, >=. */
int zero_cc_expr_int_info(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                 ASTRef ref, int *is_unsigned) {
    *is_unsigned = 0;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return 4;
    TypeInfo *ti = ast_node_get_typeinfo(c, n);
    if (ti) {
        if (type_is_pointer(ti) || ti->kind == TYPE_KIND_ARRAY) {
            *is_unsigned = 1;
            return 8;
        }
        if (type_is_integer(ti)) {
            size_t sz = type_size(ti);
            if (sz < 4) {
                *is_unsigned = 0;
                return 4;
            }
            *is_unsigned = type_is_unsigned(ti) ? 1 : 0;
            return (int)sz;
        }
    }
    switch (n->tag) {
        case NODE_INT: {
            int64_t v = n->int_val;
            uint8_t suf = (uint8_t)(n->flags & 7u);
            if (suf & 6u) {
                *is_unsigned = (suf & 1u) ? 1 : 0;
                return 8;
            }
            if (suf & 1u) {
                *is_unsigned = 1;
                return (v > INT64_C(0xFFFFFFFF) || v < 0) ? 8 : 4;
            }
            if (v >= INT32_MIN && v <= INT32_MAX) { *is_unsigned = 0; return 4; }
            if (v > INT32_MAX && v <= INT64_C(0xFFFFFFFF)) { *is_unsigned = 1; return 4; }
            *is_unsigned = 0;
            return 8;
        }
        case NODE_IDENT: {
            ZeroCCSym *sym = zero_cc_find_local(c, n->s.str);
            const ZeroCCType *zt = sym ? &sym->type : NULL;
            if (!zt) {
                ZeroCCGlobalVar *gv = find_global_var(c, n->s.str);
                if (gv) zt = &gv->type;
            }
            return zero_cc_ztype_int_info(zt, is_unsigned);
        }
        case NODE_CALL: {
            ZeroCCASTNode *cal = zero_cc_ast_get(arena, n->left);
            if (cal && cal->tag == NODE_IDENT) {
                ZeroCCFunction *f = zero_cc_find_function(c, cal->s.str);
                if (f) return zero_cc_ztype_int_info(&f->return_type, is_unsigned);
            }
            return 4;
        }
        case NODE_INDEX: {
            ZeroCCASTNode *base = zero_cc_ast_get(arena, n->left);
            if (base && base->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, base->s.str);
                const ZeroCCType *zt = sym ? &sym->type : NULL;
                if (!zt) {
                    ZeroCCGlobalVar *gv = find_global_var(c, base->s.str);
                    if (gv) zt = &gv->type;
                }
                if (zt && zt->base) return zero_cc_ztype_int_info(zt->base, is_unsigned);
            }
            return 4;
        }
        case NODE_UNOP:
            if (n->aux.op == TK_BANG) { *is_unsigned = 0; return 4; }
            /* &expr is a pointer (8 bytes), not a narrow int. */
            if (n->aux.op == TK_AMP || n->aux.op == '&') { *is_unsigned = 1; return 8; }
            return zero_cc_expr_int_info(c, arena, n->left, is_unsigned);
        case NODE_TERNARY: {
            int lu = 0, ru = 0;
            int lw = zero_cc_expr_int_info(c, arena, n->right, &lu);
            int rw = zero_cc_expr_int_info(c, arena, n->aux.alt, &ru);
            if (lw > rw) { *is_unsigned = lu; return lw; }
            if (rw > lw) { *is_unsigned = ru; return rw; }
            *is_unsigned = (lu || ru) ? 1 : 0;
            return lw;
        }
        case NODE_BINOP: {
            switch (n->aux.op) {
                case TK_EQ: case TK_NE: case TK_LT: case TK_LE:
                case TK_GT: case TK_GE: case TK_LOGAND: case TK_LOGOR:
                    *is_unsigned = 0;
                    return 4;
                case TK_SHL: case TK_SHR:
                    return zero_cc_expr_int_info(c, arena, n->left, is_unsigned);
                default: break;
            }
            int lu = 0, ru = 0;
            int lw = zero_cc_expr_int_info(c, arena, n->left, &lu);
            int rw = zero_cc_expr_int_info(c, arena, n->right, &ru);
            if (lw > rw) { *is_unsigned = lu; return lw; }
            if (rw > lw) { *is_unsigned = ru; return rw; }
            *is_unsigned = (lu || ru) ? 1 : 0;
            return lw;
        }
        default:
            return 4;
    }
}

bool zero_cc_binop_is_unsigned(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                      ASTRef left_ref, ASTRef right_ref) {
    int lu = 0, ru = 0;
    int lw = zero_cc_expr_int_info(c, arena, left_ref, &lu);
    int rw = zero_cc_expr_int_info(c, arena, right_ref, &ru);
    if (lw > rw) return lu != 0;
    if (rw > lw) return ru != 0;
    return (lu || ru);
}

/* C11 6.5.7.3p2 / 6.5.5p5: the result of an arithmetic operation on a
 * bit-field is converted back to the bit-field type (bit pattern reduced
 * to the field width; signed fields wrap in two's complement). Operands
 * are integer-promoted first, so without this `a.u33 * a.u33` on a 33-bit
 * field keeps the full 64-bit product (c-torture bitfld-3).  GCC's rule:
 *  - one operand a bit-field, the other a plain integer -> reduce to the
 *    bit-field type;
 *  - both operands bit-fields -> reduce only when they are the SAME
 *    declared field type (same struct + member), else the result keeps
 *    the common integer type (33-bit + 40-bit fields do NOT reduce).
 * Returns the (possibly new) result temp. */
static int zc_binop_bitfield_reduce(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                    ASTRef left_ref, ASTRef right_ref, int result_temp) {
    /* Extract (struct name, member offset, width, signedness) of a
     * bit-field NODE_MEMBER operand; 0 when not a bit-field. */
    struct BFKey { const char *st_name; int32_t offset; uint8_t width; bool signed_; } k[2];
    for (int side = 0; side < 2; side++) {
        k[side].st_name = NULL;
        k[side].offset = 0;
        k[side].width = 0;
        k[side].signed_ = false;
        ASTRef ref = side == 0 ? left_ref : right_ref;
        if (ref == AST_NULL) continue;
        ZeroCCASTNode *op = zero_cc_ast_get(arena, ref);
        if (!op || op->tag != NODE_MEMBER || !(op->flags & NODE_MEMBER_IS_BITFIELD)) continue;
        k[side].width = (uint8_t)((op->aux.step >> 8) & 0xFF);
        k[side].signed_ = ((op->aux.step >> 16) & 1) != 0;
        k[side].offset = (int32_t)op->aux.alt;
        ZeroCCASTNode *base = zero_cc_ast_get(arena, op->left);
        const ZeroCCType *bt = NULL;
        if (base && base->tag == NODE_IDENT) {
            ZeroCCSym *bs = zero_cc_find_local(c, base->s.str);
            ZeroCCGlobalVar *bg = bs ? NULL : find_global_var(c, base->s.str);
            const ZeroCCType *raw = bs ? &bs->type : (bg ? &bg->type : NULL);
            if (raw) {
                bt = (raw->tag == TYPE_PTR) ? raw->base : raw;
            }
        } else if (base && (base->tag == NODE_UNOP) &&
                   (base->aux.op == '*' || base->aux.op == TK_STAR)) {
            /* *p / *arr: the pointee struct's type is not recoverable
             * here without lowering - conservatively leave st_name NULL
             * (only blocks the both-bitfield-same-type reduction). */
            bt = NULL;
        }
        if (bt && (bt->tag == TYPE_STRUCT || bt->tag == TYPE_UNION)) {
            k[side].st_name = bt->struct_name;
        }
    }
    bool l_bf = k[0].width != 0, r_bf = k[1].width != 0;
    bool reduce = false;
    int use_side = 0;
    if (l_bf && r_bf) {
        bool same_member = k[0].st_name && k[1].st_name &&
            k[0].st_name[0] && k[0].offset == k[1].offset &&
            strcmp(k[0].st_name, k[1].st_name) == 0 &&
            k[0].width == k[1].width && k[0].signed_ == k[1].signed_;
        if (same_member) {
            reduce = true;
            use_side = 0;
        } else if (k[0].width != k[1].width) {
            /* Different field types: GCC converts the result to the WIDER
             * field type (2^40 stays in a 41-bit product, vanishes in a
             * 40-bit one). */
            reduce = true;
            use_side = (k[0].width > k[1].width) ? 0 : 1;
        } else if (k[0].signed_ != k[1].signed_) {
            /* Equal width, mixed signedness: the unsigned field wins, as in
             * the usual arithmetic conversions. */
            reduce = true;
            use_side = k[0].signed_ ? 1 : 0;
        }
        /* Equal width + equal signedness, different members: the common
         * type is a plain integer type - no reduction (x40a + x40b). */
    } else if (l_bf) {
        reduce = true;
        use_side = 0;
    } else if (r_bf) {
        reduce = true;
        use_side = 1;
    }
    if (!reduce) return result_temp;
    uint8_t bit_width = k[use_side].width;
    bool is_signed = k[use_side].signed_;
    /* C11 6.3.1.1p2: a bit-field whose values fit in int / unsigned int
     * (width <= 32 on this LP64 platform) is INTEGER-PROMOTED before the
     * operation - the arithmetic then happens on plain ints, so there is
     * no bit-field result to convert (c-torture bf-sign-2: `x.u15 - 2`
     * must be the int -2, not the 15-bit pattern 0x7FFF).  Only fields
     * wider than unsigned int stay un-promoted and get the reduction. */
    if (bit_width == 0 || bit_width <= 32 || bit_width >= 64) return result_temp;
    uint64_t mask = (1ULL << bit_width) - 1;
    int mask_t = c->next_temp_id++;
    ir_emit_mov_imm(mask_t, (int64_t)mask);
    int red = c->next_temp_id++;
    ir_emit_and(red, result_temp, mask_t);
    result_temp = red;
    if (is_signed) {
        int64_t sign_bit = (int64_t)(1ULL << (bit_width - 1));
        int sign_t = c->next_temp_id++;
        ir_emit_mov_imm(sign_t, sign_bit);
        int x = c->next_temp_id++;
        ir_emit_xor(x, result_temp, sign_t);
        int e = c->next_temp_id++;
        ir_emit_sub(e, x, sign_t);
        result_temp = e;
    }
    return result_temp;
}

int lower_expr_binop(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        /* C99 6.5.5 complex arithmetic: both operands are complex
         * (an address-form 2-part object: real half first) or scalar
         * (converted to (scalar, 0)).  The result is a fresh aligned
         * stack object returned as its ADDRESS. */
        {
            ZeroCCASTNode *cl_node = zero_cc_ast_get(arena, node->left);
            ZeroCCASTNode *cr_node = zero_cc_ast_get(arena, node->right);
            TypeInfo *cres_ti = ast_node_get_typeinfo(c, node);
            TypeInfo *cl_ti = ast_node_get_typeinfo(c, cl_node);
            TypeInfo *cr_ti = ast_node_get_typeinfo(c, cr_node);
            bool is_cx_eqne = (node->aux.op == TK_EQ || node->aux.op == TK_NE) &&
                              cl_ti && cr_ti &&
                              (cl_ti->kind == TYPE_KIND_COMPLEX || cr_ti->kind == TYPE_KIND_COMPLEX);
            if ((cres_ti && cres_ti->kind == TYPE_KIND_COMPLEX) || is_cx_eqne) {
                const TypeInfo *cel = (cres_ti && cres_ti->kind == TYPE_KIND_COMPLEX) ? cres_ti->details.array.base :
                                      (cl_ti && cl_ti->kind == TYPE_KIND_COMPLEX) ? cl_ti->details.array.base : cr_ti->details.array.base;
                size_t cesz = cel ? type_size(cel) : 8;
                if (cesz != 4 && cesz != 8) cesz = 8; /* long double: double model */
                bool cflt = cel ? type_is_floating(cel) : true;
                uint32_t cop = node->aux.op;
                int32_t save_frame = c->current_stack_frame;
                c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + 16 + 7) & ~7;
                int32_t croff = c->current_stack_frame;
                int clv = lower_expr(c, arena, node->left);
                bool left_cx = (cl_ti && cl_ti->kind == TYPE_KIND_COMPLEX);
                bool left_is_float = c->last_expr_is_float;
                int crv = lower_expr(c, arena, node->right);
                bool right_cx = (cr_ti && cr_ti->kind == TYPE_KIND_COMPLEX);
                bool right_is_float = c->last_expr_is_float;


                /* Load parts (or materialize (scalar, 0)).  Complex
                 * operands are address-form: use register-indirect
                 * SSE loads (OP_FLOAD is frame-relative only). */
                int zero_f_off = zc_rodata_double_offset(c, 0.0);
                int ar = c->next_temp_id++, ai = c->next_temp_id++;
                int br = c->next_temp_id++, bi = c->next_temp_id++;
                if (left_cx) {
                    if (cesz == 4) { ir_emit_vloadss(ar, (uint16_t)clv); int a2 = c->next_temp_id++; ir_emit_mov_imm(a2, 4); int ai2 = c->next_temp_id++; ir_emit_add(ai2, clv, a2); ir_emit_vloadss(ai, (uint16_t)ai2); }
                    else { ir_emit_vloadsd(ar, (uint16_t)clv); int a2 = c->next_temp_id++; ir_emit_mov_imm(a2, 8); int ai2 = c->next_temp_id++; ir_emit_add(ai2, clv, a2); ir_emit_vloadsd(ai, (uint16_t)ai2); }
                } else {
                    int lpart = clv;
                    if (!left_is_float) {
                        /* Integer scalar -> double (C99 6.3.1.2). */
                        lpart = zc_emit_int_to_double(c, clv, cl_ti);
                    }
                    ir_emit_fmov(ar, (uint16_t)lpart);
                    ir_emit_fmov_imm((uint16_t)ai, (int64_t)zero_f_off);
                }
                if (right_cx) {
                    if (cesz == 4) { ir_emit_vloadss(br, (uint16_t)crv); int a2 = c->next_temp_id++; ir_emit_mov_imm(a2, 4); int ai2 = c->next_temp_id++; ir_emit_add(ai2, crv, a2); ir_emit_vloadss(bi, (uint16_t)ai2); }
                    else { ir_emit_vloadsd(br, (uint16_t)crv); int a2 = c->next_temp_id++; ir_emit_mov_imm(a2, 8); int ai2 = c->next_temp_id++; ir_emit_add(ai2, crv, a2); ir_emit_vloadsd(bi, (uint16_t)ai2); }
                } else {
                    int rpart = crv;
                    /* right_is_float was captured right after the right
                     * operand's lowering (a later complex-address
                     * lowering would clear last_expr_is_float and make
                     * a float scalar look integral). */
                    if (!right_is_float) {
                        rpart = zc_emit_int_to_double(c, crv, cr_ti);
                    }
                    ir_emit_fmov(br, (uint16_t)rpart);
                    ir_emit_fmov_imm((uint16_t)bi, (int64_t)zero_f_off);
                }
                /* Per-operator part results */
                int rr = c->next_temp_id++, ri = c->next_temp_id++;
                switch (cop) {
                    case TK_PLUS: {
                        ir_emit_fadd(rr, ar, br);
                        ir_emit_fadd(ri, ai, bi);
                        break;
                    }
                    case TK_MINUS: {
                        ir_emit_fsub(rr, ar, br);
                        ir_emit_fsub(ri, ai, bi);
                        break;
                    }
                    case TK_STAR: {
                        int t1 = c->next_temp_id++; ir_emit_fmul(t1, ar, br);
                        int t2 = c->next_temp_id++; ir_emit_fmul(t2, ai, bi);
                        int t3 = c->next_temp_id++; ir_emit_fmul(t3, ar, bi);
                        int t4 = c->next_temp_id++; ir_emit_fmul(t4, ai, br);
                        ir_emit_fsub(rr, t1, t2);
                        ir_emit_fadd(ri, t3, t4);
                        break;
                    }
                    case TK_SLASH: {
                        if (cesz == 8) {
                            /* C99 _Complex double division: delegate to
                             * libgcc __divdc3, the same helper GCC itself
                             * calls at -O0 (the ieee/cdivchk* expected
                             * values are generated that way).  The
                             * in-compiler formulas cannot match its
                             * accuracy for extreme subnormal inputs
                             * (products like ar*br underflow to 0 while
                             * contributing ~2^-32 relative to the
                             * quotient), and __divdc3 handles the
                             * inf/nan/+-0 edge cases per IEEE.  The call
                             * is ABI-identical to a normal zerocc complex
                             * call: two complex args = four consecutive
                             * FP registers (a.re, a.im, b.re, b.im),
                             * result in xmm0/xmm1. */
                            int div_args[4];
                            int div_vals[4] = { ar, ai, br, bi };
                            if (c->function_count < ZERO_CC_MAX_FUNCTIONS &&
                                !zero_cc_find_function(c, "__divdc3")) {
                                ZeroCCFunction *fn = &c->functions[c->function_count++];
                                memset(fn, 0, sizeof(*fn));
                                snprintf(fn->name, sizeof(fn->name), "%s", "__divdc3");
                                fn->is_prototype = true;
                                fn->is_external = true;
                            }
                            for (int h = 0; h < 4; h++) {
                                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                c->current_stack_frame += 8;
                                int32_t slot = c->current_stack_frame;
                                ir_emit_fstore((uint16_t)div_vals[h], TEMP_NONE, -slot);
                                div_args[h] = c->next_temp_id++;
                                ir_emit_fload((uint16_t)div_args[h], TEMP_NONE, -slot);
                            }
                            ir_emit_call("__divdc3");
                            if (ir_get_count() > 0) {
                                Instr *last_instr = (Instr*)&ir_get_buffer()[ir_get_count() - 1];
                                last_instr->temp_dest = 0;  /* no int args */
                                last_instr->temp_src1 = (uint16_t)div_args[0];
                                last_instr->temp_src2 = (uint16_t)div_args[1];
                                last_instr->imm = 0;
                                int div_is_fp[4] = { 1, 1, 1, 1 };
                                uint8_t div_pos[4] = { 255, 255, 255, 255 };
                                ir_call_args_record(ir_get_count() - 1, 4, div_args, div_is_fp, div_pos);
                                last_instr->label_idx = 4; /* 4 FP register args */
                                if (c->lowering.ir_ctx.call_count < 64) {
                                    int cidx = c->lowering.ir_ctx.call_count++;
                                    c->lowering.ir_ctx.call_fp_spill_counts[cidx] = 4;
                                    for (int h = 0; h < 4; h++) {
                                        c->lowering.ir_ctx.call_fp_spills[cidx][h] =
                                            c->current_stack_frame - (3 - h) * 8;
                                    }
                                }
                            }
                            /* Complex return: xmm0/xmm1 -> stack object */
                            c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                            c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                            int32_t dres_off = c->current_stack_frame;
                            if (ir_get_count() > 0) {
                                Instr *li = (Instr*)&ir_get_buffer()[ir_get_count() - 1];
                                li->label_idx = 0x0100 | (16 & 0xFF);
                            }
                            int daddr = c->next_temp_id++;
                            ir_emit_lea(daddr, BE(c)->fp_reg, -dres_off);
                            ir_emit_vstoresd((uint16_t)daddr, (uint16_t)VREG_F0);
                            int do2 = c->next_temp_id++;
                            ir_emit_mov_imm(do2, 8);
                            int da2 = c->next_temp_id++;
                            ir_emit_add(da2, daddr, do2);
                            ir_emit_vstoresd((uint16_t)da2, (uint16_t)VREG_F1);
                            c->current_stack_frame = save_frame;
                            c->last_expr_is_float = 0;
                            return daddr;
                        }
                        /* _Complex float division: in-compiler scaled
                         * formula (float has far more headroom between
                         * its normal and subnormal ranges, and
                         * __divsc3's packed-float argument ABI differs
                         * from the double-model parts held here).  Scale
                         * by the divisor's dominant component, forming
                         * each mixed product as (a_dom/b_dom)*b_min so
                         * the tiny ratio never underflows first:
                           |br| >= |bi|:  re = (ar + (ai/br)*bi)/d
                                          im = (ai - (ar/br)*bi)/d
                                          d  = br + (bi/br)*bi
                           otherwise:     re = (ai + (ar/bi)*br)/d
                                          im = ((ai/bi)*br - ar)/d
                                          d  = bi + (br/bi)*br
                         Both branches may divide by zero on their
                         * unused path (-> nan), so the parts are
                         * selected branchlessly. */
                        int fab_br = zc_cx_fabs(c, br);
                        int fab_bi = zc_cx_fabs(c, bi);
                        int ge = c->next_temp_id++;
                        ir_emit_cmp(fab_br, fab_bi);
                        ir_emit_cond_set(ge, CC_GE);
                        int qa1 = c->next_temp_id++; ir_emit_fdiv(qa1, ai, br);
                        int qa1b = c->next_temp_id++; ir_emit_fmul(qa1b, qa1, bi);
                        int reA = c->next_temp_id++; ir_emit_fadd(reA, ar, qa1b);
                        int qa0 = c->next_temp_id++; ir_emit_fdiv(qa0, ar, br);
                        int qa0b = c->next_temp_id++; ir_emit_fmul(qa0b, qa0, bi);
                        int imA = c->next_temp_id++; ir_emit_fsub(imA, ai, qa0b);
                        int da_m = c->next_temp_id++; ir_emit_fdiv(da_m, bi, br);
                        int da_m2 = c->next_temp_id++; ir_emit_fmul(da_m2, da_m, bi);
                        int dA = c->next_temp_id++; ir_emit_fadd(dA, br, da_m2);
                        int qb0a = c->next_temp_id++; ir_emit_fdiv(qb0a, ar, bi);
                        int qb0ab = c->next_temp_id++; ir_emit_fmul(qb0ab, qb0a, br);
                        int reB = c->next_temp_id++; ir_emit_fadd(reB, ai, qb0ab);
                        int qb0b = c->next_temp_id++; ir_emit_fdiv(qb0b, ai, bi);
                        int qb0bb = c->next_temp_id++; ir_emit_fmul(qb0bb, qb0b, br);
                        int imB = c->next_temp_id++; ir_emit_fsub(imB, qb0bb, ar);
                        int db_m = c->next_temp_id++; ir_emit_fdiv(db_m, br, bi);
                        int db_m2 = c->next_temp_id++; ir_emit_fmul(db_m2, db_m, br);
                        int dB = c->next_temp_id++; ir_emit_fadd(dB, bi, db_m2);
                        int num_r = zc_cx_fselect(c, ge, reA, reB);
                        int num_i = zc_cx_fselect(c, ge, imA, imB);
                        int den = zc_cx_fselect(c, ge, dA, dB);
                        ir_emit_fdiv(rr, num_r, den);
                        ir_emit_fdiv(ri, num_i, den);
                        break;
                    }
                    case TK_EQ:
                    case TK_NE: {
                        int eqr = c->next_temp_id++, eqi = c->next_temp_id++;
                        ir_emit_cmp(ar, br);
                        ir_emit_cond_set(eqr, CC_EQ);
                        ir_emit_cmp(ai, bi);
                        ir_emit_cond_set(eqi, CC_EQ);
                        int both = c->next_temp_id++;
                        ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)both,
                            .temp_src1=(uint16_t)eqr,.temp_src2=(uint16_t)eqi});
                        if (cop == TK_NE) {
                            int one = c->next_temp_id++;
                            ir_emit_mov_imm(one, 1);
                            int nz = c->next_temp_id++;
                            ir_emit((Instr){.op=OP_XOR,.temp_dest=(uint16_t)nz,
                                .temp_src1=(uint16_t)both,.temp_src2=(uint16_t)one});
                            rr = nz;
                        } else {
                            rr = both;
                        }
                        ir_emit_mov_imm(ri, 0);
                        c->current_stack_frame = save_frame; /* int result, no object */
                        c->last_expr_is_float = 0;
                        return rr;
                    }
                    default:
                        ir_emit_fmov(rr, ar);
                        ir_emit_fmov(ri, ai);
                        break;
                }
                if (cesz == 4) {
                    int sa = c->next_temp_id++;
                    ir_emit_lea(sa, BE(c)->fp_reg, -croff);
                    ir_emit_vstoress(sa, (uint16_t)rr);
                    int sa2 = c->next_temp_id++;
                    ir_emit_mov_imm(sa2, 4);
                    int sa3 = c->next_temp_id++;
                    ir_emit_add(sa3, sa, sa2);
                    ir_emit_vstoress(sa3, (uint16_t)ri);
                } else {
                    ir_emit_fstore((uint16_t)rr, TEMP_NONE, -croff);
                    ir_emit_fstore((uint16_t)ri, TEMP_NONE, -(croff - (int)cesz));
                }
                int addr = c->next_temp_id++;
                ir_emit_lea(addr, BE(c)->fp_reg, -croff);
                c->last_expr_is_float = 0;
                return addr;
            }
        }
        /* GNU vector arithmetic: per-lane scalar expansion. A
         * vector-typed operand lowers to the ADDRESS of the vector
         * object (ident decay, the per-lane temp of an inner vector
         * expression); a scalar operand is broadcast across all
         * lanes. The result is a fresh aligned stack temp, returned
         * as its ADDRESS - consumers (assignment, call args, returns,
         * nested vector ops) load through it. Integer lanes use
         * width-correct loads/sign-extension and the scalar int ops
         * (C div/mod semantics); float lanes run in the double-width
         * FP model (cvtss2sd/cvtsd2ss) so 4-byte float vectors reuse
         * the double ops. Comparisons yield per-lane -1/0 predicate
         * lanes (GCC vector-compare semantics). */
        {
            ZeroCCASTNode *l_node = zero_cc_ast_get(arena, node->left);
            ZeroCCASTNode *r_node = zero_cc_ast_get(arena, node->right);
            TypeInfo *res_ti = ast_node_get_typeinfo(c, node);
            TypeInfo *l_ti = ast_node_get_typeinfo(c, l_node);
            TypeInfo *r_ti = ast_node_get_typeinfo(c, r_node);
            if (res_ti && type_is_vector(res_ti) &&
                ((l_ti && type_is_vector(l_ti)) || (r_ti && type_is_vector(r_ti)))) {
                const TypeInfo *vec_ti = (l_ti && type_is_vector(l_ti)) ? l_ti : r_ti;
                const TypeInfo *el_ti = vec_ti->details.array.base;
                size_t esz = el_ti ? type_size(el_ti) : 0;
                size_t lanes = (esz > 0 && type_size(vec_ti) > 0) ? type_size(vec_ti) / esz : 0;
                if (esz < 1 || esz > 8 || lanes < 1 || lanes > 32) {
                    diag_report_error_at_token(c, "unsupported vector operand size");
                    int zt = c->next_temp_id++;
                    ir_emit_mov_imm(zt, 0);
                    c->last_expr_is_float = 0;
                    return zt;
                }
                bool elem_float = type_is_floating(el_ti);
                bool elem_unsigned = type_is_unsigned(el_ti);
                uint32_t vop = node->aux.op;
                bool is_cmp = (vop == TK_LT || vop == TK_GT || vop == TK_LE || vop == TK_GE ||
                               vop == TK_EQ || vop == TK_NE);

                /* Result temp object in the current frame (16-byte
                 * aligned for <=16B vectors, like SSE objects). */
                size_t vsize = lanes * esz;
                if (vsize > 16) c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + (int32_t)vsize + 7) & ~7;
                int32_t res_off = c->current_stack_frame;

                int left_temp = lower_expr(c, arena, node->left);
                bool left_vec = (l_ti && type_is_vector(l_ti));
                bool left_flt = c->last_expr_is_float;
                int right_temp = lower_expr(c, arena, node->right);
                bool right_vec = (r_ti && type_is_vector(r_ti));
                bool right_flt = c->last_expr_is_float;

                /* C integer conversions: a scalar operand is converted
                 * to the vector element type before the operation. For
                 * div/mod this is observable (64-bit residue != 32-bit
                 * value), so truncate/extend the scalar to lane width. */
                if (!elem_float && esz < 8) {
                    if (!left_vec) {
                        if (esz == 4) { if (elem_unsigned) ir_emit_zext(left_temp, 4); else ir_emit_sext(left_temp, 4); }
                        else if (esz == 2) { if (elem_unsigned) ir_emit_zext(left_temp, 2); else ir_emit_sext(left_temp, 2); }
                        else if (esz == 1) { if (elem_unsigned) ir_emit_zext(left_temp, 1); else ir_emit_sext(left_temp, 1); }
                    }
                    if (!right_vec) {
                        if (esz == 4) { if (elem_unsigned) ir_emit_zext(right_temp, 4); else ir_emit_sext(right_temp, 4); }
                        else if (esz == 2) { if (elem_unsigned) ir_emit_zext(right_temp, 2); else ir_emit_sext(right_temp, 2); }
                        else if (esz == 1) { if (elem_unsigned) ir_emit_zext(right_temp, 1); else ir_emit_sext(right_temp, 1); }
                    }
                }

                int cmp_cc = CC_EQ;
                if (vop == TK_LT) cmp_cc = CC_LT;
                else if (vop == TK_GT) cmp_cc = CC_GT;
                else if (vop == TK_LE) cmp_cc = CC_LE;
                else if (vop == TK_GE) cmp_cc = CC_GE;
                else if (vop == TK_NE) cmp_cc = CC_NE;

                for (size_t i = 0; i < lanes; i++) {
                    /* Load lane i of each vector operand (or take the
                     * scalar-broadcast value). */
                    int lv = left_temp, rv = right_temp;
                    bool lv_flt = left_flt, rv_flt = right_flt;
                    if (left_vec) {
                        int a = left_temp;
                        if (i > 0) {
                            int o = c->next_temp_id++;
                            ir_emit_mov_imm(o, (int32_t)(i * esz));
                            a = c->next_temp_id++;
                            ir_emit_add(a, left_temp, o);
                        }
                        lv = c->next_temp_id++;
                        if (elem_float) {
                            if (esz == 4) ir_emit_vloadss((uint16_t)lv, (uint16_t)a);
                            else ir_emit_vloadsd((uint16_t)lv, (uint16_t)a);
                            lv_flt = 1;
                        } else if (esz == 1) {
                            ir_emit_load_byte(lv, a);
                            if (elem_unsigned) ir_emit_zext(lv, 1); else ir_emit_sext(lv, 1);
                        } else if (esz == 2) {
                            ir_emit_load_16(lv, a);
                            if (elem_unsigned) ir_emit_zext(lv, 2); else ir_emit_sext(lv, 2);
                        } else if (esz == 4) {
                            ir_emit_load_32(lv, a);
                            if (elem_unsigned) ir_emit_zext(lv, 4); else ir_emit_sext(lv, 4);
                        } else {
                            ir_emit_deref(lv, a);
                        }
                    }
                    if (right_vec) {
                        int a = right_temp;
                        if (i > 0) {
                            int o = c->next_temp_id++;
                            ir_emit_mov_imm(o, (int32_t)(i * esz));
                            a = c->next_temp_id++;
                            ir_emit_add(a, right_temp, o);
                        }
                        rv = c->next_temp_id++;
                        if (elem_float) {
                            if (esz == 4) ir_emit_vloadss((uint16_t)rv, (uint16_t)a);
                            else ir_emit_vloadsd((uint16_t)rv, (uint16_t)a);
                            rv_flt = 1;
                        } else if (esz == 1) {
                            ir_emit_load_byte(rv, a);
                            if (elem_unsigned) ir_emit_zext(rv, 1); else ir_emit_sext(rv, 1);
                        } else if (esz == 2) {
                            ir_emit_load_16(rv, a);
                            if (elem_unsigned) ir_emit_zext(rv, 2); else ir_emit_sext(rv, 2);
                        } else if (esz == 4) {
                            ir_emit_load_32(rv, a);
                            if (elem_unsigned) ir_emit_zext(rv, 4); else ir_emit_sext(rv, 4);
                        } else {
                            ir_emit_deref(rv, a);
                        }
                    }

                    int ov = c->next_temp_id++;
                    bool ov_flt = false;
                    if (is_cmp) {
                        if (elem_float) {
                            /* Double FP model: VLOADSS already widened the
                             * lane single to double; a double scalar with
                             * float lanes is first narrowed to the element
                             * type (C: scalar converts to the vector
                             * element type) then re-widened; int scalars
                             * widen with cvtsi2sd. */
                            if (esz == 4) {
                                if (!left_vec && left_flt) { int t = c->next_temp_id++; ir_emit_cvtsd2ss((uint16_t)t, (uint16_t)lv); lv = t; int t2 = c->next_temp_id++; ir_emit_cvtss2sd((uint16_t)t2, (uint16_t)lv); lv = t2; }
                                if (!right_vec && right_flt) { int t = c->next_temp_id++; ir_emit_cvtsd2ss((uint16_t)t, (uint16_t)rv); rv = t; int t2 = c->next_temp_id++; ir_emit_cvtss2sd((uint16_t)t2, (uint16_t)rv); rv = t2; }
                            }
                            if (!left_vec && !left_flt) { int t = c->next_temp_id++; ir_emit_cvtsi2sd((uint16_t)t, (uint16_t)lv); lv = t; }
                            if (!right_vec && !right_flt) { int t = c->next_temp_id++; ir_emit_cvtsi2sd((uint16_t)t, (uint16_t)rv); rv = t; }
                            ir_emit_cmp((uint16_t)lv, (uint16_t)rv);
                        } else {
                            ir_emit_cmp((uint16_t)lv, (uint16_t)rv);
                        }
                        ir_emit_cond_set((uint16_t)ov, cmp_cc);
                        /* Predicate lane: full -1 mask / 0 (GCC). The
                         * result vector has an integer element type
                         * (see type_vector_cmp_result); the mask stores
                         * as int lanes of the result element width. */
                        int m = c->next_temp_id++;
                        ir_emit_mov_imm(m, -1);
                        int ov2 = c->next_temp_id++;
                        ir_emit_mul(ov2, ov, m);
                        ov = ov2;
                    } else if (elem_float) {
                        int ls = lv, rs = rv;
                        /* Double FP model: VLOADSS/VLOADSD lane values
                         * are already double. A double scalar with float
                         * lanes narrows to the element type first (C
                         * semantics) then re-widens; int scalars widen
                         * with cvtsi2sd. */
                        if (esz == 4) {
                            if (!left_vec && left_flt) { int t = c->next_temp_id++; ir_emit_cvtsd2ss((uint16_t)t, (uint16_t)lv); lv = t; int t2 = c->next_temp_id++; ir_emit_cvtss2sd((uint16_t)t2, (uint16_t)lv); ls = t2; }
                            if (!right_vec && right_flt) { int t = c->next_temp_id++; ir_emit_cvtsd2ss((uint16_t)t, (uint16_t)rv); rv = t; int t2 = c->next_temp_id++; ir_emit_cvtss2sd((uint16_t)t2, (uint16_t)rv); rs = t2; }
                        }
                        if (!left_vec && !left_flt) { int t = c->next_temp_id++; ir_emit_cvtsi2sd((uint16_t)t, (uint16_t)lv); ls = t; }
                        if (!right_vec && !right_flt) { int t = c->next_temp_id++; ir_emit_cvtsi2sd((uint16_t)t, (uint16_t)rv); rs = t; }
                        switch (vop) {
                            case TK_PLUS:  ir_emit_fadd((uint16_t)ov, (uint16_t)ls, (uint16_t)rs); break;
                            case TK_MINUS: ir_emit_fsub((uint16_t)ov, (uint16_t)ls, (uint16_t)rs); break;
                            case TK_STAR:  ir_emit_fmul((uint16_t)ov, (uint16_t)ls, (uint16_t)rs); break;
                            default:       ir_emit_fdiv((uint16_t)ov, (uint16_t)ls, (uint16_t)rs); break;
                        }
                        ov_flt = 1;
                    } else {
                        switch (vop) {
                            case TK_PLUS:  ir_emit_add(ov, lv, rv); break;
                            case TK_MINUS: ir_emit_sub(ov, lv, rv); break;
                            case TK_STAR:  ir_emit_mul(ov, lv, rv); break;
                            case TK_SLASH: ir_emit_div(ov, lv, rv); if (elem_unsigned) ir_mark_last_unsigned(1); break;
                            case TK_PERCENT: ir_emit_mod(ov, lv, rv); if (elem_unsigned) ir_mark_last_unsigned(1); break;
                            case TK_AMP:  ir_emit_and(ov, lv, rv); break;
                            case TK_PIPE: ir_emit_or(ov, lv, rv); break;
                            case TK_CARET: ir_emit_xor(ov, lv, rv); break;
                            case TK_SHL:  ir_emit_shl(ov, lv, rv); break;
                            case TK_SHR:  ir_emit_shr(ov, lv, rv); break;
                            default: ir_emit_mov_reg(ov, lv); break;
                        }
                    }

                    /* Store lane i of the result. Comparison results
                     * have an integer element type; arithmetic results
                     * keep the operand element type. */
                    int32_t lane_off = -(res_off - (int32_t)(i * esz));
                    int lane_addr = c->next_temp_id++;
                    ir_emit_lea(lane_addr, BE(c)->fp_reg, lane_off);
                    if (is_cmp) {
                        if (esz == 1) ir_emit_store_byte(ov, lane_addr);
                        else if (esz == 2) ir_emit_store_16(ov, lane_addr);
                        else if (esz == 4) ir_emit_store_32(ov, lane_addr);
                        else ir_emit_store_64(ov, lane_addr);
                    } else if (elem_float) {
                        if (esz == 4) {
                            if (!ov_flt) { int t = c->next_temp_id++; ir_emit_cvtsi2sd((uint16_t)t, (uint16_t)ov); ov = t; ov_flt = 1; }
                            /* Double FP model: VSTORESS narrows the
                             * double to single internally. */
                            ir_emit_vstoress((uint16_t)lane_addr, (uint16_t)ov);
                        } else {
                            ir_emit_vstoresd((uint16_t)lane_addr, (uint16_t)ov);
                        }
                    } else if (esz == 1) {
                        ir_emit_store_byte(ov, lane_addr);
                    } else if (esz == 2) {
                        ir_emit_store_16(ov, lane_addr);
                    } else if (esz == 4) {
                        ir_emit_store_32(ov, lane_addr);
                    } else {
                        ir_emit_store_64(ov, lane_addr);
                    }
                }
                int res_addr = c->next_temp_id++;
                ir_emit_lea(res_addr, BE(c)->fp_reg, -res_off);
                c->last_expr_is_float = 0;
                return res_addr;
            }
        }

        /* Logical AND (&&) / OR (||): C99 6.5.13/6.5.14 short-circuit
         * evaluation. The right operand is evaluated ONLY if the left
         * operand does not determine the result (`p && *p` must not
         * dereference a null `p`). */
        if (node->aux.op == TK_LOGAND || node->aux.op == TK_LOGOR) {
            int result_temp = c->next_temp_id++;
            uint16_t rhs_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;
            int lc = lower_expr(c, arena, node->left);
            if (c->last_expr_is_float) {
                int zero_f = c->next_temp_id++;
                ir_emit_fmov_imm((uint16_t)zero_f, 0.0);
                int cmp_i = c->next_temp_id++;
                ir_emit_cmp(lc, zero_f);
                ir_emit_cond_set(cmp_i, CC_NE);
                lc = cmp_i;
                c->last_expr_is_float = 0;
            }
            if (node->aux.op == TK_LOGAND) {
                ir_emit_mov_imm(result_temp, 0);
                ir_emit_je((uint16_t)lc, end_label);
            } else {
                ir_emit_mov_imm(result_temp, 1);
                ir_emit_jne((uint16_t)lc, end_label);
            }
            ir_emit_label(rhs_label);
            int rc = lower_expr(c, arena, node->right);
            int rt = c->next_temp_id++;
            if (c->last_expr_is_float) {
                int zero_f = c->next_temp_id++;
                ir_emit_fmov_imm((uint16_t)zero_f, 0.0);
                ir_emit_cmp(rc, zero_f);
                ir_emit_cond_set(rt, CC_NE);
                c->last_expr_is_float = 0;
            } else {
                ir_emit_cmp_zero(rc);
                ir_emit_cond_set(rt, CC_NE);
            }
            ir_emit_mov_reg(result_temp, rt);
            ir_emit_label(end_label);
            c->last_expr_is_float = 0;
            return result_temp;
        }

        /* Evaluate left operand first */
        int left_temp = lower_expr(c, arena, node->left);
        bool left_is_float = c->last_expr_is_float;

        /* Evaluate right operand */
        int right_temp = lower_expr(c, arena, node->right);
        bool right_is_float = c->last_expr_is_float;

        /* Mixed float/int operands (e.g. `double_member * 2`, `d + 1`):
         * promote the integer operand to the FLOATING-POINT operand's
         * type (C usual arithmetic conversions): double for a double
         * operand, but FLOAT for a float operand. Converting straight
         * with cvtsi2sd keeps integer precision that float cannot
         * represent (16777217L == 16777216.0f must be TRUE - the long
         * is first rounded to 16777216.0f, c-torture 920710-1). In the
         * double-width FP model, round the int to single precision
         * first (VCVT_SI2SS) then widen (CVTSS2SD). Only x86-64 has a
         * VCVT_SI2SS emitter; other ISAs keep the (corner-case-wrong)
         * direct cvtsi2sd until their emitters land. */
        if (left_is_float != right_is_float) {
            const ZeroCCASTNode *fnode = right_is_float ?
                zero_cc_ast_get(arena, node->right) : zero_cc_ast_get(arena, node->left);
            TypeInfo *f_ti = fnode ? ast_node_get_typeinfo(c, fnode) : NULL;
            bool fp_is_float = (f_ti && f_ti->kind == TYPE_KIND_FLOAT) && c->isa == ISA_X86_64;
            if (right_is_float) {
                int conv_temp = c->next_temp_id++;
                if (fp_is_float) {
                    ir_emit((Instr){.op = OP_VCVT_SI2SS, .temp_dest = (uint16_t)conv_temp,
                                    .temp_src1 = (uint16_t)left_temp, .temp_src2 = TEMP_NONE,
                                    .imm = 0, .label_idx = 0});
                    int wid = c->next_temp_id++;
                    ir_emit_cvtss2sd((uint16_t)wid, (uint16_t)conv_temp);
                    conv_temp = wid;
                } else {
                    ir_emit_cvtsi2sd((uint16_t)conv_temp, (uint16_t)left_temp);
                }
                left_temp = conv_temp;
                left_is_float = 1;
            } else {
                int conv_temp = c->next_temp_id++;
                if (fp_is_float) {
                    ir_emit((Instr){.op = OP_VCVT_SI2SS, .temp_dest = (uint16_t)conv_temp,
                                    .temp_src1 = (uint16_t)right_temp, .temp_src2 = TEMP_NONE,
                                    .imm = 0, .label_idx = 0});
                    int wid = c->next_temp_id++;
                    ir_emit_cvtss2sd((uint16_t)wid, (uint16_t)conv_temp);
                    conv_temp = wid;
                } else {
                    ir_emit_cvtsi2sd((uint16_t)conv_temp, (uint16_t)right_temp);
                }
                right_temp = conv_temp;
                right_is_float = 1;
            }
        }

        /* Float arithmetic and comparisons - use IR for proper register allocation */
        if (left_is_float && right_is_float) {
            /* At this point: left_temp and right_temp are float temps in XMM registers */
            int result_temp = c->next_temp_id++;
            switch (node->aux.op) {
                case TK_PLUS:
                    ir_emit_fadd((uint16_t)result_temp, (uint16_t)left_temp, (uint16_t)right_temp);
                    c->last_expr_is_float = 1;
                    return result_temp;
                case TK_MINUS:
                    ir_emit_fsub((uint16_t)result_temp, (uint16_t)left_temp, (uint16_t)right_temp);
                    c->last_expr_is_float = 1;
                    return result_temp;
                case TK_STAR:
                    ir_emit_fmul((uint16_t)result_temp, (uint16_t)left_temp, (uint16_t)right_temp);
                    c->last_expr_is_float = 1;
                    return result_temp;
                case TK_SLASH:
                    ir_emit_fdiv((uint16_t)result_temp, (uint16_t)left_temp, (uint16_t)right_temp);
                    c->last_expr_is_float = 1;
                    return result_temp;
                case TK_EQ:
                case TK_NE:
                case TK_LT:
                case TK_LE:
                case TK_GT:
                case TK_GE: {
                    int cond;
                    switch (node->aux.op) {
                        case TK_EQ: cond = CC_EQ; break;
                        case TK_NE: cond = CC_NE; break;
                        case TK_LT: cond = CC_LT; break;
                        case TK_LE: cond = CC_LE; break;
                        case TK_GT: cond = CC_GT; break;
                        case TK_GE: cond = CC_GE; break;
                        default: cond = CC_EQ; break;
                    }
                    ir_emit_cmp(left_temp, right_temp);
                    ir_emit_cond_set(result_temp, cond);
                    c->last_expr_is_float = 0;
                    return result_temp;
                }
                default:
                    break;
            }
            c->last_expr_is_float = 1;
            return result_temp;
        }

        /* C11 6.5.6: pointer arithmetic scales by sizeof(pointee).
         * ptr + n / n + ptr -> ptr + n*sizeof(*ptr);  ptr - n ->
         * ptr - n*sizeof(*ptr);  ptr - ptr -> element difference
         * (signed division by sizeof).  Array operands decay to
         * pointers, so an array type on either side counts too. */
        if ((node->aux.op == TK_PLUS || node->aux.op == TK_MINUS) &&
            !left_is_float && !right_is_float) {
            ZeroCCASTNode *ln2 = zero_cc_ast_get(arena, node->left);
            ZeroCCASTNode *rn2 = zero_cc_ast_get(arena, node->right);
            TypeInfo *lti2 = ln2 ? ast_node_get_typeinfo(c, ln2) : NULL;
            TypeInfo *rti2 = rn2 ? ast_node_get_typeinfo(c, rn2) : NULL;
            bool l_ptr = (lti2 && (type_is_pointer(lti2) || lti2->kind == TYPE_KIND_ARRAY));
            bool r_ptr = (rti2 && (type_is_pointer(rti2) || rti2->kind == TYPE_KIND_ARRAY));
            if (l_ptr && r_ptr && node->aux.op == TK_MINUS) {
                int d = c->next_temp_id++;
                ir_emit_sub(d, left_temp, right_temp);
                size_t psz = type_size(lti2->kind == TYPE_KIND_ARRAY ?
                    lti2->details.array.base : lti2->details.pointer.base);
                if (psz > 1) {
                    int ds = c->next_temp_id++;
                    ir_emit_mov_imm(ds, (int32_t)psz);
                    int dq = c->next_temp_id++;
                    ir_emit_div(dq, d, ds);
                    d = dq;
                }
                c->last_expr_is_float = 0;
                return d;
            }
            if (l_ptr || (r_ptr && node->aux.op == TK_PLUS)) {
                const TypeInfo *pti = l_ptr ? lti2 : rti2;
                size_t psz = type_size(pti->kind == TYPE_KIND_ARRAY ?
                    pti->details.array.base : pti->details.pointer.base);
                if (psz > 1) {
                    int si = c->next_temp_id++;
                    ir_emit_mov_imm(si, (int32_t)psz);
                    int s = c->next_temp_id++;
                    if (l_ptr) {
                        ir_emit_mul(s, right_temp, si);
                        right_temp = s;
                    } else {
                        ir_emit_mul(s, left_temp, si);
                        left_temp = s;
                    }
                }
                /* fall through: raw add/sub on the scaled operand */
            }
        }

        /* Integer arithmetic - use IR */
        int result_temp = c->next_temp_id++;
        switch (node->aux.op) {
            case TK_PLUS:
                ir_emit_add(result_temp, left_temp, right_temp);
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, node->right, result_temp);
                break;
            case TK_MINUS:
                ir_emit_sub(result_temp, left_temp, right_temp);
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, node->right, result_temp);
                break;
            case TK_STAR:
                ir_emit_mul(result_temp, left_temp, right_temp);
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, node->right, result_temp);
                break;
            case TK_SLASH: {
                int lu = 0, ru = 0;
                int lw = zero_cc_expr_int_info(c, arena, node->left, &lu);
                int rw = zero_cc_expr_int_info(c, arena, node->right, &ru);
                if (lw == 4 && rw == 4 && (lu != ru)) {
                    /* C99 6.3.1.8: both operands convert to the common
                     * unsigned type - reduce BOTH to the low 32 bits (a
                     * sign-extended negative int has its high bits set).
                     * A zexted negative value is its 32-bit bit pattern. */
                    ir_emit_zext(left_temp, 4);
                    ir_emit_zext(right_temp, 4);
                }
                ir_emit_div(result_temp, left_temp, right_temp);
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, AST_NULL, result_temp);
                if (zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) {
                    ir_mark_last_unsigned(1);
                }
                break;
            }
            case TK_PERCENT: {
                int lu = 0, ru = 0;
                int lw = zero_cc_expr_int_info(c, arena, node->left, &lu);
                int rw = zero_cc_expr_int_info(c, arena, node->right, &ru);
                if (lw == 4 && rw == 4 && (lu != ru)) {
                    /* C99 6.3.1.8: both operands convert to the common
                     * unsigned type - reduce BOTH to the low 32 bits. */
                    ir_emit_zext(left_temp, 4);
                    ir_emit_zext(right_temp, 4);
                }
                ir_emit_mod(result_temp, left_temp, right_temp);
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, AST_NULL, result_temp);
                if (zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) {
                    ir_mark_last_unsigned(1);
                }
                break;
            }
            case TK_AMP:
                ir_emit_and(result_temp, left_temp, right_temp);
                break;
            case TK_PIPE:
                ir_emit_or(result_temp, left_temp, right_temp);
                break;
            case TK_CARET:
                ir_emit_xor(result_temp, left_temp, right_temp);
                break;
            case TK_SHL:
                ir_emit_shl(result_temp, left_temp, right_temp);
                /* C11 6.5.8: the shift result is reduced to the width
                 * of the (un-promoted) bit-field when the left operand
                 * is one wider than unsigned int - `x.b << 36` on a
                 * 40-bit field keeps only the low 40 bits (pr32244-1).
                 * Bitwise & | ^ do NOT reduce. */
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, AST_NULL, result_temp);
                break;
            case TK_SHR: {
                /* Signed >> is an arithmetic shift (C: implementation-
                 * defined; gcc defines it as arithmetic for signed). */
                int lu = 0;
                zero_cc_expr_int_info(c, arena, node->left, &lu);
                if (!lu) {
                    ir_emit_sar(result_temp, left_temp, right_temp);
                } else {
                    ir_emit_shr(result_temp, left_temp, right_temp);
                }
                /* Same bit-field result reduction as TK_SHL
                 * (C11 6.5.8; signed fields re-sign-extend). */
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, AST_NULL, result_temp);
                break;
            }
            case TK_EQ:
            case TK_NE:
            case TK_LT:
            case TK_LE:
            case TK_GT:
            case TK_GE: {
                /* Comparison: emit cmp_branch (ISA-neutral) */
                int cond;
                switch (node->aux.op) {
                    case TK_EQ: cond = CC_EQ; break;
                    case TK_NE: cond = CC_NE; break;
                    case TK_LT: cond = CC_LT; break;
                    case TK_LE: cond = CC_LE; break;
                    case TK_GT: cond = CC_GT; break;
                    case TK_GE: cond = CC_GE; break;
                    default: cond = CC_EQ; break;
                }
                if (cond >= CC_LT && cond <= CC_GE &&
                    zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) {
                    cond += 6;
                }
                if (!c->last_expr_is_float) {
                    int lu = 0, ru = 0;
                    int lw = zero_cc_expr_int_info(c, arena, node->left, &lu);
                    int rw = zero_cc_expr_int_info(c, arena, node->right, &ru);
                    if (lw == 4 && rw == 4 && (lu != ru)) {
                        /* C99 6.3.1.8: both operands convert to the common
                         * unsigned type - reduce BOTH to the low 32 bits so
                         * a sign-extended negative int compares as its
                         * 32-bit unsigned bit pattern. */
                        ir_emit_zext(left_temp, 4);
                        ir_emit_zext(right_temp, 4);
                    }
                }
                ir_emit_cmp(left_temp, right_temp);
                ir_emit_cond_set(result_temp, cond);
                break;
            }
            default:
                ir_emit_mov_reg(result_temp, left_temp);
                break;
        }
        /* C99 width semantics: derive the result width from the operand
         * types (C usual arithmetic conversions: the wider operand wins;
         * equal width -> unsigned wins). Marks the op so the MC layer
         * extends the 64-bit result to true N-bit wrap semantics.
         * Comparisons yield 0/1 (already in range) - unmarked. */
        {
            int bop = node->aux.op;
            if (!(bop == TK_EQ || bop == TK_NE || bop == TK_LT ||
                  bop == TK_LE || bop == TK_GT || bop == TK_GE)) {
                if (bop == TK_SHL || bop == TK_SHR) {
                    /* C99 6.5.7: shift result type and width depend ONLY on the left operand */
                    int ls = 1;
                    int lw = zero_cc_operand_int_width(c, arena, node->left, &ls);
                    if (lw > 0 && lw < 4) { lw = 4; ls = 1; }
                    if (lw != 0) {
                        ir_mark_last_width(ls ? (uint8_t)lw : (uint8_t)(8 + lw));
                    }
                } else {
                    int ls = 1, rs = 1;
                    int lw = zero_cc_operand_int_width(c, arena, node->left, &ls);
                    int rw = zero_cc_operand_int_width(c, arena, node->right, &rs);
                    /* In C, integer promotions promote types narrower than int (1, 2)
                     * to signed int (4). */
                    if (lw > 0 && lw < 4) { lw = 4; ls = 1; }
                    if (rw > 0 && rw < 4) { rw = 4; rs = 1; }
                    if (lw != 0 && rw != 0) {
                        int w, s;
                        if (lw > rw) { w = lw; s = ls; }
                        else if (rw > lw) { w = rw; s = rs; }
                        else { w = lw; s = (ls && rs) ? 1 : 0; }  /* equal width: any unsigned -> unsigned */
                        ir_mark_last_width(s ? (uint8_t)w : (uint8_t)(8 + w));
                    }
                }
            }
        }
        c->last_expr_is_float = 0;
        return result_temp;
}

int lower_expr_unop(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        if (node->aux.op == TK_INC || node->aux.op == TK_DEC) {
            ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
            if (operand && operand->tag == NODE_IDENT) {
                const char *var = operand->s.str;
                ZeroCCSym *sym = zero_cc_find_local(c, var);
                if (sym) {
                    if (sym->is_const || sym->type.is_const) {
                        diag_report_error_at_token(c, "cannot modify const-qualified variable '%s'", var);
                        return TEMP_NONE;
                    }
                    if (sym->is_atomic || sym->type.is_atomic) {
                        int step = lvalue_inc_step(&sym->type);
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, -sym->stack_offset);
                        int delta_temp = c->next_temp_id++;
                        int width = (sym->type.size > 0) ? (int)sym->type.size : 4;
                        ir_emit_mov_imm(delta_temp, (node->aux.op == TK_INC) ? step : -step);
                        int old_temp = c->next_temp_id++;
                        ir_emit_atomic_xadd(old_temp, addr_temp, delta_temp, width);
                        c->last_expr_is_float = 0;
                        if (node->flags & 1) {
                            /* Postfix x++ / x--: return old value */
                            return old_temp;
                        } else {
                            /* Prefix ++x / --x: return new value (old ± step) */
                            int new_temp = c->next_temp_id++;
                            int step_temp = c->next_temp_id++;
                            ir_emit_mov_imm(step_temp, (node->aux.op == TK_INC) ? step : -step);
                            ir_emit_add(new_temp, old_temp, step_temp);
                            return new_temp;
                        }
                    }

                    if (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE) {
                        int val_temp = c->next_temp_id++;
                        int one_i = c->next_temp_id++;
                        int one_f = c->next_temp_id++;
                        int res_temp = c->next_temp_id++;
                        if (sym->type.tag == TYPE_FLOAT) {
                            int addr = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                            ir_emit_vloadss((uint16_t)val_temp, (uint16_t)addr);
                        } else {
                            int a = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                            ir_emit_vloadsd((uint16_t)val_temp, (uint16_t)a);
                        }
                        ir_emit_mov_imm(one_i, 1);
                        ir_emit_cvtsi2sd((uint16_t)one_f, (uint16_t)one_i);
                        if (node->aux.op == TK_INC) {
                            ir_emit_fadd((uint16_t)res_temp, (uint16_t)val_temp, (uint16_t)one_f);
                        } else {
                            ir_emit_fsub((uint16_t)res_temp, (uint16_t)val_temp, (uint16_t)one_f);
                        }
                        if (sym->type.tag == TYPE_FLOAT) {
                            int addr = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                            ir_emit_vstoress((uint16_t)addr, (uint16_t)res_temp);
                        } else {
                            int a = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                            ir_emit_vstoresd((uint16_t)a, (uint16_t)res_temp);
                        }
                        c->last_expr_is_float = 1;
                        return (node->flags & 1) ? val_temp : res_temp;
                    }

                    int step = lvalue_inc_step(&sym->type);
                    int val_temp = c->next_temp_id++;
                    int one_temp = c->next_temp_id++;
                    int res_temp = c->next_temp_id++;

                    /* Load current variable value from stack */
                    zc_sym_load(c, sym, 0, val_temp);  /* Sprint 72 */
                    /* Step: sizeof(*x) for pointer locals (C11 6.5.6), else 1 */
                    ir_emit_mov_imm(one_temp, (node->aux.op == TK_INC) ? step : -step);
                    /* res = val ± step */
                    ir_emit_add(res_temp, val_temp, one_temp);
                    zero_cc_emit_int_extend(c, res_temp, &sym->type);
                    /* Store new value back to stack */
                    zc_sym_store(c, sym, 0, res_temp);  /* Sprint 72 */

                    c->last_expr_is_float = 0;
                    /* Postfix x++ / x-- yields the OLD value; prefix
                     * ++x / --x yields the new value ((node->flags & 1)
                     * marks postfix, matching the global/atomic paths
                     * below). Pre-fix the local path always returned the
                     * new value, miscompiling `return x++;` etc. */
                    return (node->flags & 1) ? val_temp : res_temp;
                }
                ZeroCCGlobalVar *gv = find_global_var(c, var);
                if (gv) {
                    int gaddr_temp = c->next_temp_id++;
                    ir_emit_global_addr(c, gaddr_temp, gv);
                    if (gv->type.tag == TYPE_FLOAT || gv->type.tag == TYPE_DOUBLE) {
                        int val_temp = c->next_temp_id++;
                        int one_i = c->next_temp_id++;
                        int one_f = c->next_temp_id++;
                        int res_temp = c->next_temp_id++;
                        if (gv->type.tag == TYPE_FLOAT) ir_emit_vloadss((uint16_t)val_temp, (uint16_t)gaddr_temp);
                        else ir_emit_vloadsd((uint16_t)val_temp, (uint16_t)gaddr_temp);
                        ir_emit_mov_imm(one_i, 1);
                        ir_emit_cvtsi2sd((uint16_t)one_f, (uint16_t)one_i);
                        if (node->aux.op == TK_INC) {
                            ir_emit_fadd((uint16_t)res_temp, (uint16_t)val_temp, (uint16_t)one_f);
                        } else {
                            ir_emit_fsub((uint16_t)res_temp, (uint16_t)val_temp, (uint16_t)one_f);
                        }
                        if (gv->type.tag == TYPE_FLOAT) ir_emit_vstoress((uint16_t)gaddr_temp, (uint16_t)res_temp);
                        else ir_emit_vstoresd((uint16_t)gaddr_temp, (uint16_t)res_temp);
                        c->last_expr_is_float = 1;
                        return (node->flags & 1) ? val_temp : res_temp;
                    }
                    int val_temp = c->next_temp_id++;
                    int one_temp = c->next_temp_id++;
                    int res_temp = c->next_temp_id++;
                    size_t gv_sz = gv->size > 0 ? gv->size : 4;
                    if (gv_sz == 1) ir_emit_load_byte(val_temp, gaddr_temp);
                    else if (gv_sz == 2) ir_emit_load_16(val_temp, gaddr_temp);
                    else if (gv_sz == 4) ir_emit_load_32(val_temp, gaddr_temp);
                    else ir_emit_deref(val_temp, gaddr_temp);
                    zero_cc_emit_int_extend(c, val_temp, &gv->type);

                    ir_emit_mov_imm(one_temp, (node->aux.op == TK_INC) ?
                        lvalue_inc_step(&gv->type) : -lvalue_inc_step(&gv->type));
                    ir_emit_add(res_temp, val_temp, one_temp);
                    zero_cc_emit_int_extend(c, res_temp, &gv->type);

                    if (gv_sz == 1) ir_emit_store_byte(res_temp, gaddr_temp);
                    else if (gv_sz == 2) ir_emit_store_16(res_temp, gaddr_temp);
                    else if (gv_sz == 4) ir_emit_store_32(res_temp, gaddr_temp);
                    else ir_emit_store_64(res_temp, gaddr_temp);

                    c->last_expr_is_float = 0;
                    return (node->flags & 1) ? val_temp : res_temp;
                }
            }
            /* Non-ident lvalue (C11 6.5.2.4): a[i]++, (*p)++, s.f++,
             * p->f++.  Pre-fix these lowered to nothing (silent no-op).
             * Compute the lvalue address the same way the NODE_ASSIGN
             * branches do, then load, modify by the pointer-scaled
             * step, store; postfix returns the OLD value. */
            if (operand) {
                TypeInfo *lval_ti = ast_node_get_typeinfo(c, operand);
                size_t lval_size = lval_ti ? type_size(lval_ti) : 4;
                bool lval_float = (lval_ti && type_is_floating(lval_ti)) || c->last_expr_is_float;
                if (lval_size == 0) lval_size = 4;
                int addr = TEMP_NONE;

                if (operand->tag == NODE_INDEX) {
                    int array_temp = lower_expr(c, arena, operand->left);
                    int index_temp = lower_expr(c, arena, operand->right);
                    int scaled_index;
                    if (lval_size == 1) {
                        scaled_index = index_temp;
                    } else if (lval_size == 2 || lval_size == 4 || lval_size == 8) {
                        int shift = (lval_size == 2) ? 1 : ((lval_size == 4) ? 2 : 3);
                        int sh_temp = c->next_temp_id++;
                        ir_emit_mov_imm(sh_temp, shift);
                        scaled_index = c->next_temp_id++;
                        ir_emit_shl(scaled_index, index_temp, sh_temp);
                    } else {
                        int sz_temp = c->next_temp_id++;
                        ir_emit_mov_imm(sz_temp, (int32_t)lval_size);
                        scaled_index = c->next_temp_id++;
                        ir_emit_mul(scaled_index, index_temp, sz_temp);
                    }
                    addr = c->next_temp_id++;
                    ir_emit_add(addr, array_temp, scaled_index);
                } else if (operand->tag == NODE_UNOP &&
                           (operand->aux.op == '*' || operand->aux.op == TK_STAR)) {
                    addr = lower_expr(c, arena, operand->left); /* the pointer value */
                } else if (operand->tag == NODE_MEMBER) {
                    /* Single level: base address + member offset.
                     * Multi-segment chains (o.p->v) use the generic walker. */
                    ZeroCCASTNode *m = operand;
                    int depth = 0;
                    while (m && m->tag == NODE_MEMBER && depth < 128) {
                        depth++;
                        m = zero_cc_ast_get(arena, m->left);
                    }
                    if (depth >= 2) {
                        addr = lower_member_chain_addr(c, arena, operand);
                    } else {
                        ZeroCCASTNode *base_node = zero_cc_ast_get(arena, operand->left);
                        int32_t member_offset = operand->aux.alt;
                        bool is_arrow = (operand->flags & NODE_MEMBER_IS_ARROW) != 0;
                        int base_addr = TEMP_NONE;
                        if (base_node && base_node->tag == NODE_IDENT) {
                            ZeroCCSym *bsym = zero_cc_find_local(c, base_node->s.str);
                            ZeroCCGlobalVar *bgv = bsym ? NULL : find_global_var(c, base_node->s.str);
                            if (bsym) {
                                base_addr = c->next_temp_id++;
                                if (is_arrow) {
                                    /* p->m: load the pointer value */
                                    ir_emit_load(base_addr, BE(c)->fp_reg, -bsym->stack_offset);
                                } else {
                                    /* s.m: address of the struct object */
                                    ir_emit_lea(base_addr, TEMP_NONE, -(int32_t)bsym->stack_offset);
                                }
                            } else if (bgv) {
                                base_addr = c->next_temp_id++;
                                ir_emit_global_addr(c, base_addr, bgv);
                                if (is_arrow) {
                                    /* global pointer var: deref to get value */
                                    base_addr = c->next_temp_id++;
                                    ir_emit_deref(base_addr, base_addr);
                                }
                            }
                        } else if (base_node && base_node->tag == NODE_INDEX) {
                            /* arr[i].m: element address + member offset
                             * (c-torture execute/pr55750: `arr[i].n++`). */
                            int array_temp = lower_expr(c, arena, base_node->left);
                            int index_temp = lower_expr(c, arena, base_node->right);
                            TypeInfo *elti = ast_node_get_typeinfo(c, base_node);
                            size_t esz = elti ? type_size(elti) : 0;
                            if (esz == 0) esz = 4;
                            int scaled_index;
                            if (esz == 1) {
                                scaled_index = index_temp;
                            } else if (esz == 2 || esz == 4 || esz == 8) {
                                int shift = (esz == 2) ? 1 : ((esz == 4) ? 2 : 3);
                                int sh_temp = c->next_temp_id++;
                                ir_emit_mov_imm(sh_temp, shift);
                                scaled_index = c->next_temp_id++;
                                ir_emit_shl(scaled_index, index_temp, sh_temp);
                            } else {
                                int sz_temp = c->next_temp_id++;
                                ir_emit_mov_imm(sz_temp, (int32_t)esz);
                                scaled_index = c->next_temp_id++;
                                ir_emit_mul(scaled_index, index_temp, sz_temp);
                            }
                            base_addr = c->next_temp_id++;
                            ir_emit_add(base_addr, array_temp, scaled_index);
                        }
                        if (base_addr == TEMP_NONE && is_arrow) {
                            base_addr = lower_expr(c, arena, operand->left);
                        }
                        if (base_addr != TEMP_NONE) {
                            addr = base_addr;
                            if (member_offset != 0) {
                                int off_temp = c->next_temp_id++;
                                ir_emit_mov_imm(off_temp, member_offset);
                                addr = c->next_temp_id++;
                                ir_emit_add(addr, base_addr, off_temp);
                            }
                        }
                    }
                }
                if (addr == TEMP_NONE) return TEMP_NONE;

                /* Bit-field member (`++c.u33`, `--c.u40`): the lvalue
                 * address is the storage UNIT, not the field's bit
                 * position - do the field RMW with read-modify-write,
                 * and convert the result to the bit-field type (C11
                 * 6.5.6 / 6.5.7.3p2; c-torture bitfld-3). */
                if (operand->tag == NODE_MEMBER &&
                    (operand->flags & NODE_MEMBER_IS_BITFIELD)) {
                    uint8_t bf_off = (uint8_t)(operand->aux.step & 0xFF);
                    uint8_t bf_w = (uint8_t)((operand->aux.step >> 8) & 0xFF);
                    bool bf_signed = ((operand->aux.step >> 16) & 1) != 0;
                    TypeInfo *bti = ast_node_get_typeinfo(c, operand);
                    size_t bm_size = bti ? type_size(bti) : 4;
                    bool bf64 = (bm_size > 4 || (bf_off + bf_w > 32));
                    uint64_t wmask = (bf_w >= 64) ? ~0ULL : ((1ULL << bf_w) - 1);
                    int word_t = c->next_temp_id++;
                    if (bf64) ir_emit_deref(word_t, addr);
                    else ir_emit_load_32(word_t, addr);
                    int fld = word_t;
                    if (bf_off > 0) {
                        int sh = c->next_temp_id++;
                        ir_emit_mov_imm(sh, (int32_t)bf_off);
                        int s2 = c->next_temp_id++;
                        ir_emit_shr(s2, fld, sh);
                        fld = s2;
                    }
                    int m1 = c->next_temp_id++;
                    ir_emit_mov_imm(m1, (int64_t)wmask);
                    int fm = c->next_temp_id++;
                    ir_emit_and(fm, fld, m1);
                    int old_val = fm;
                    if (bf_signed && bf_w > 0 && bf_w < 64) {
                        int64_t sb = (int64_t)(1ULL << (bf_w - 1));
                        int st = c->next_temp_id++;
                        ir_emit_mov_imm(st, sb);
                        int x1 = c->next_temp_id++;
                        ir_emit_xor(x1, old_val, st);
                        int e1 = c->next_temp_id++;
                        ir_emit_sub(e1, x1, st);
                        old_val = e1;
                    }
                    int one_t = c->next_temp_id++;
                    ir_emit_mov_imm(one_t, (node->aux.op == TK_INC) ? 1 : -1);
                    int new_val = c->next_temp_id++;
                    ir_emit_add(new_val, old_val, one_t);
                    int m2 = c->next_temp_id++;
                    ir_emit_mov_imm(m2, (int64_t)wmask);
                    int red = c->next_temp_id++;
                    ir_emit_and(red, new_val, m2);
                    if (bf_signed && bf_w > 0 && bf_w < 64) {
                        int64_t sb2 = (int64_t)(1ULL << (bf_w - 1));
                        int st2 = c->next_temp_id++;
                        ir_emit_mov_imm(st2, sb2);
                        int x2 = c->next_temp_id++;
                        ir_emit_xor(x2, red, st2);
                        int e2 = c->next_temp_id++;
                        ir_emit_sub(e2, x2, st2);
                        red = e2;
                    }
                    uint64_t fmask = (bf_w >= 64) ? ~0ULL : (wmask << bf_off);
                    int cm = c->next_temp_id++;
                    ir_emit_mov_imm(cm, (int64_t)(~fmask));
                    int cleared = c->next_temp_id++;
                    ir_emit_and(cleared, word_t, cm);
                    int mv = red;
                    if (bf_off > 0) {
                        int sh2 = c->next_temp_id++;
                        ir_emit_mov_imm(sh2, (int32_t)bf_off);
                        int s3 = c->next_temp_id++;
                        ir_emit_shl(s3, mv, sh2);
                        mv = s3;
                    }
                    int comb = c->next_temp_id++;
                    ir_emit_or(comb, cleared, mv);
                    if (bf64) ir_emit_store_64(comb, addr);
                    else ir_emit_store_32(comb, addr);
                    c->last_expr_is_float = 0;
                    return (node->flags & 1) ? old_val : red;
                }

                int step = 1;
                if (lval_ti && type_is_pointer(lval_ti)) {
                    step = (int)type_size(lval_ti->details.pointer.base);
                    if (step <= 0) step = 8;
                }
                int old_t = c->next_temp_id++;
                int new_t = c->next_temp_id++;
                if (lval_float) {
                    if (lval_size == 4) ir_emit_vloadss((uint16_t)old_t, (uint16_t)addr);
                    else ir_emit_vloadsd((uint16_t)old_t, (uint16_t)addr);
                    int one_i = c->next_temp_id++;
                    ir_emit_mov_imm(one_i, 1);
                    int one_f = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)one_f, (uint16_t)one_i);
                    if (node->aux.op == TK_INC) {
                        ir_emit_fadd((uint16_t)new_t, (uint16_t)old_t, (uint16_t)one_f);
                    } else {
                        ir_emit_fsub((uint16_t)new_t, (uint16_t)old_t, (uint16_t)one_f);
                    }
                    if (lval_size == 4) ir_emit_vstoress((uint16_t)addr, (uint16_t)new_t);
                    else ir_emit_vstoresd((uint16_t)addr, (uint16_t)new_t);
                    c->last_expr_is_float = 1;
                    return (node->flags & 1) ? old_t : new_t;
                }
                if (lval_size == 1) ir_emit_load_byte(old_t, addr);
                else if (lval_size == 2) ir_emit_load_16(old_t, addr);
                else if (lval_size == 4) ir_emit_load_32(old_t, addr);
                else ir_emit_deref(old_t, addr);
                lower_extend_narrow(c, old_t, lval_ti);
                int step_t = c->next_temp_id++;
                ir_emit_mov_imm(step_t, (node->aux.op == TK_INC) ? step : -step);
                ir_emit_add(new_t, old_t, step_t);
                if (lval_size == 1) ir_emit_store_byte(new_t, addr);
                else if (lval_size == 2) ir_emit_store_16(new_t, addr);
                else if (lval_size == 4) ir_emit_store_32(new_t, addr);
                else ir_emit_store_64(new_t, addr);
                c->last_expr_is_float = 0;
                return (node->flags & 1) ? old_t : new_t;
            }
            return TEMP_NONE;
        }

        if (node->aux.op == '&' || node->aux.op == TK_AMP) {
            int addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
            if (addr != TEMP_NONE) {
                c->last_expr_is_float = 0;
                return addr;
            }
        }

        /* GNU vector: per-lane unary +/-/~. The operand lowers to the
         * address of the vector object; the result is a fresh stack
         * temp returned as its address (like the vector binops). */
        {
            TypeInfo *un_ti = ast_node_get_typeinfo(c, node);
            if (un_ti && type_is_vector(un_ti) &&
                (node->aux.op == TK_MINUS || node->aux.op == TK_TILDE ||
                 node->aux.op == TK_PLUS || node->aux.op == '+')) {
                const TypeInfo *el_ti = un_ti->details.array.base;
                size_t esz = el_ti ? type_size(el_ti) : 0;
                size_t lanes = (esz > 0 && type_size(un_ti) > 0) ? type_size(un_ti) / esz : 0;
                if (esz < 1 || esz > 8 || lanes < 1 || lanes > 32) {
                    diag_report_error_at_token(c, "unsupported vector operand size");
                    int zt = c->next_temp_id++;
                    ir_emit_mov_imm(zt, 0);
                    c->last_expr_is_float = 0;
                    return zt;
                }
                bool elem_float = type_is_floating(el_ti);
                bool elem_unsigned = type_is_unsigned(el_ti);
                size_t vsize = lanes * esz;
                if (vsize > 16) c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + (int32_t)vsize + 7) & ~7;
                int32_t res_off = c->current_stack_frame;
                int op_addr = lower_expr(c, arena, node->left);
                for (size_t i = 0; i < lanes; i++) {
                    int a = op_addr;
                    if (i > 0) {
                        int o = c->next_temp_id++;
                        ir_emit_mov_imm(o, (int32_t)(i * esz));
                        a = c->next_temp_id++;
                        ir_emit_add(a, op_addr, o);
                    }
                    int lv = c->next_temp_id++;
                    if (elem_float) {
                        if (esz == 4) ir_emit_vloadss((uint16_t)lv, (uint16_t)a);
                        else ir_emit_vloadsd((uint16_t)lv, (uint16_t)a);
                    } else if (esz == 1) {
                        ir_emit_load_byte(lv, a);
                        if (elem_unsigned) ir_emit_zext(lv, 1); else ir_emit_sext(lv, 1);
                    } else if (esz == 2) {
                        ir_emit_load_16(lv, a);
                        if (elem_unsigned) ir_emit_zext(lv, 2); else ir_emit_sext(lv, 2);
                    } else if (esz == 4) {
                        ir_emit_load_32(lv, a);
                        if (elem_unsigned) ir_emit_zext(lv, 4); else ir_emit_sext(lv, 4);
                    } else {
                        ir_emit_deref(lv, a);
                    }
                    int ov = c->next_temp_id++;
                    bool ov_flt = false;
                    if (elem_float) {
                        int ls = lv;
                        if (esz == 4) { int t = c->next_temp_id++; ir_emit_cvtss2sd((uint16_t)t, (uint16_t)lv); ls = t; }
                        if (node->aux.op == TK_MINUS || node->aux.op == '-') {
                            int zero_i = c->next_temp_id++;
                            ir_emit_mov_imm(zero_i, 0);
                            int zero_f = c->next_temp_id++;
                            ir_emit_cvtsi2sd((uint16_t)zero_f, (uint16_t)zero_i);
                            ir_emit_fsub((uint16_t)ov, (uint16_t)zero_f, (uint16_t)ls);
                        } else {
                            /* Unary + (and ~, undefined for floats): identity */
                            ir_emit_fmov((uint16_t)ov, (uint16_t)ls);
                        }
                        ov_flt = 1;
                    } else if (node->aux.op == TK_TILDE || node->aux.op == '~') {
                        ir_emit_not(ov, lv);
                    } else if (node->aux.op == TK_MINUS || node->aux.op == '-') {
                        ir_emit_neg(ov, lv);
                    } else {
                        ir_emit_mov_reg(ov, lv);
                    }
                    int32_t lane_off = -(res_off - (int32_t)(i * esz));
                    int lane_addr = c->next_temp_id++;
                    ir_emit_lea(lane_addr, BE(c)->fp_reg, lane_off);
                    if (elem_float) {
                        if (esz == 4) {
                            if (ov_flt) { int t = c->next_temp_id++; ir_emit_cvtsd2ss((uint16_t)t, (uint16_t)ov); ov = t; }
                            ir_emit_vstoress((uint16_t)lane_addr, (uint16_t)ov);
                        } else {
                            ir_emit_vstoresd((uint16_t)lane_addr, (uint16_t)ov);
                        }
                    } else if (esz == 1) {
                        ir_emit_store_byte(ov, lane_addr);
                    } else if (esz == 2) {
                        ir_emit_store_16(ov, lane_addr);
                    } else if (esz == 4) {
                        ir_emit_store_32(ov, lane_addr);
                    } else {
                        ir_emit_store_64(ov, lane_addr);
                    }
                }
                int res_addr = c->next_temp_id++;
                ir_emit_lea(res_addr, BE(c)->fp_reg, -res_off);
                c->last_expr_is_float = 0;
                return res_addr;
            }
        }

        int operand_temp = lower_expr(c, arena, node->left);
        int result_temp = c->next_temp_id++;

        switch (node->aux.op) {
            case TK_STAR: { /* Dereference */
                /* Dereferencing a pointer to function yields a function
                 * lvalue (C99 6.5.3.2): its value is the pointer itself,
                 * and [fp] holds the function's machine code, not data.
                 * So `*fp` (and e.g. `(*fp)(args)`) evaluates to the
                 * pointer value with no load. */
                TypeInfo *op_ti = ast_node_get_typeinfo(c, zero_cc_ast_get(arena, node->left));
                TypeInfo *ti = ast_node_get_typeinfo(c, node);

                if (ti && type_is_vector(ti)) {
                    /* Dereference of a pointer-to-vector: the vector value
                     * is in address form - the pointer itself is the value
                     * (consumers read lanes through it). */
                    c->last_expr_is_float = 0;
                    return operand_temp;
                }
                if ((ti && ti->kind == TYPE_KIND_ARRAY) ||
                    (op_ti && op_ti->kind == TYPE_KIND_POINTER && op_ti->details.pointer.base &&
                     (op_ti->details.pointer.base->kind == TYPE_KIND_ARRAY ||
                      type_is_function(op_ti->details.pointer.base)))) {
                    c->last_expr_is_float = 0;
                    return operand_temp;
                }
                size_t p_size = 0;
                if (ti) p_size = type_size(ti);
                else {
                    ZeroCCASTNode *inner = zero_cc_ast_get(arena, node->left);
                    if (inner && inner->tag == NODE_IDENT) {
                        ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                        if (ptr_sym && ptr_sym->type.base && ptr_sym->type.base->size > 0) {
                            p_size = ptr_sym->type.base->size;
                        }
                        ZeroCCGlobalVar *gv = find_global_var(c, inner->s.str);
                        if (gv && gv->type.base && gv->type.base->size > 0) {
                            p_size = gv->type.base->size;
                        }
                    }
                }
                bool is_struct = (ti && (ti->kind == TYPE_KIND_STRUCT || ti->kind == TYPE_KIND_UNION));
                if (!is_struct) {
                    ZeroCCASTNode *inner = zero_cc_ast_get(arena, node->left);
                    if (inner && inner->tag == NODE_IDENT) {
                        ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                        if (ptr_sym && ptr_sym->type.base && (ptr_sym->type.base->tag == TYPE_STRUCT || ptr_sym->type.base->tag == TYPE_UNION)) {
                            is_struct = true;
                        }
                        ZeroCCGlobalVar *gv = find_global_var(c, inner->s.str);
                        if (gv && gv->type.base && (gv->type.base->tag == TYPE_STRUCT || gv->type.base->tag == TYPE_UNION)) {
                            is_struct = true;
                        }
                    }
                }
                if (is_struct && p_size > 16) {
                    c->last_expr_is_float = 0;
                    return operand_temp;
                }
                /* Zero-size struct (C11 6.7.2.1p17: `struct g {}`) -
                 * the dereference has no bytes to load; yielding the
                 * pointer itself keeps the lvalue usable and, crucially,
                 * must NOT emit an 8-byte read past the empty object
                 * (c-torture execute/zero-struct-1: the phantom load
                 * read 8 bytes of a zero-size object and the matching
                 * store scribbled on whatever follows it). */
                if (is_struct && p_size == 0) {
                    c->last_expr_is_float = 0;
                    return operand_temp;
                }
                bool is_fp = (ti && type_is_floating(ti));
                if (!is_fp) {
                    ZeroCCASTNode *inner = zero_cc_ast_get(arena, node->left);
                    if (inner && inner->tag == NODE_IDENT) {
                        ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                        if (ptr_sym && ptr_sym->type.base &&
                            (ptr_sym->type.base->tag == TYPE_FLOAT || ptr_sym->type.base->tag == TYPE_DOUBLE)) {
                            is_fp = true;
                            if (p_size == 0) p_size = (ptr_sym->type.base->tag == TYPE_FLOAT) ? 4 : 8;
                        }
                        ZeroCCGlobalVar *gv = find_global_var(c, inner->s.str);
                        if (gv && gv->type.base &&
                            (gv->type.base->tag == TYPE_FLOAT || gv->type.base->tag == TYPE_DOUBLE)) {
                            is_fp = true;
                            if (p_size == 0) p_size = (gv->type.base->tag == TYPE_FLOAT) ? 4 : 8;
                        }
                    }
                }
                if (is_fp) {
                    if (p_size == 4) ir_emit_vloadss((uint16_t)result_temp, (uint16_t)operand_temp);
                    else ir_emit_vloadsd((uint16_t)result_temp, (uint16_t)operand_temp);
                    c->last_expr_is_float = 1;
                    return result_temp;
                }
                if (p_size == 1) ir_emit_load_byte(result_temp, operand_temp);
                else if (p_size == 2) ir_emit_load_16(result_temp, operand_temp);
                else if (p_size == 4) ir_emit_load_32(result_temp, operand_temp);
                else ir_emit_deref(result_temp, operand_temp);
                lower_extend_narrow(c, result_temp, ti);
                break;
            }
            case TK_MINUS:  /* Negation */
                if (c->last_expr_is_float) {
                    /* IEEE 754 float negation: flip the sign bit (bit 63).
                     * (0.0 - x) does not preserve -0.0 because 0.0 - 0.0 = +0.0
                     * per IEEE 754 §6.3. */
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    int32_t slot = c->current_stack_frame;
                    ir_emit_fstore((uint16_t)operand_temp, TEMP_NONE, -slot);
                    int int_temp = c->next_temp_id++;
                    ir_emit_load(int_temp, BE(c)->fp_reg, -slot);
                    int sign_mask = c->next_temp_id++;
                    ir_emit_mov_imm(sign_mask, (int64_t)0x8000000000000000ULL);
                    int flipped = c->next_temp_id++;
                    ir_emit_xor(flipped, int_temp, sign_mask);
                    ir_emit_store(flipped, BE(c)->fp_reg, -slot);
                    ir_emit_fload((uint16_t)result_temp, TEMP_NONE, -slot);
                    c->last_expr_is_float = 1;
                    return result_temp;
                }
                ir_emit_neg(result_temp, operand_temp);
                /* C99 6.5.3.3: the result type is the promoted operand
                 * type, so a 32-bit unsigned negation wraps at 32 bits
                 * (-1U == 0xFFFFFFFF, not 0xFFFFFFFFFFFFFFFF) and a
                 * 1/2-byte operand promotes to signed int.  Without the
                 * mark the full-width negation poisons later 32-bit
                 * compares/divides (c-torture pr49039, pr56250). */
                {
                    TypeInfo *neg_ti = ast_node_get_typeinfo(c, node);
                    if (neg_ti) {
                        size_t nwsz = type_size(neg_ti);
                        if (nwsz == 4) {
                            bool wu = (neg_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                            ir_mark_last_width(wu ? (uint8_t)(8 + 4) : (uint8_t)4);
                        } else if (nwsz == 1 || nwsz == 2) {
                            ir_mark_last_width(4); /* integer promotion */
                        }
                    }
                }
                break;
            case TK_BANG: { /* Logical NOT: !operand */
                TypeInfo *op_ti = ast_node_get_typeinfo(c, zero_cc_ast_get(arena, node->left));
                if (op_ti && op_ti->kind == TYPE_KIND_COMPLEX) {
                    size_t esz = (op_ti->details.array.base) ? type_size(op_ti->details.array.base) : 8;
                    if (esz != 4 && esz != 8) esz = 8;
                    int rh = c->next_temp_id++, li = c->next_temp_id++;
                    if (esz == 4) {
                        ir_emit_vloadss((uint16_t)rh, (uint16_t)operand_temp);
                        int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 4);
                        int a2 = c->next_temp_id++; ir_emit_add(a2, operand_temp, o2);
                        ir_emit_vloadss((uint16_t)li, (uint16_t)a2);
                    } else {
                        ir_emit_vloadsd((uint16_t)rh, (uint16_t)operand_temp);
                        int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 8);
                        int a2 = c->next_temp_id++; ir_emit_add(a2, operand_temp, o2);
                        ir_emit_vloadsd((uint16_t)li, (uint16_t)a2);
                    }
                    int zf = c->next_temp_id++;
                    ir_emit_fmov_imm((uint16_t)zf, (int64_t)zc_rodata_double_offset(c, 0.0));
                    int er = c->next_temp_id++;
                    ir_emit_cmp(rh, zf);
                    ir_emit_cond_set(er, CC_EQ);
                    int ei = c->next_temp_id++;
                    ir_emit_cmp(li, zf);
                    ir_emit_cond_set(ei, CC_EQ);
                    ir_emit((Instr){.op = OP_AND, .temp_dest = (uint16_t)result_temp,
                                    .temp_src1 = (uint16_t)er, .temp_src2 = (uint16_t)ei});
                    c->last_expr_is_float = 0;
                } else if (c->last_expr_is_float) {
                    int zero_i = c->next_temp_id++;
                    ir_emit_mov_imm(zero_i, 0);
                    int zero_f = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)zero_f, (uint16_t)zero_i);
                    ir_emit_cmp(operand_temp, zero_f);
                    ir_emit_cond_set(result_temp, CC_EQ);
                    c->last_expr_is_float = 0;
                } else {
                    ir_emit_cmp_zero(operand_temp);
                    ir_emit_cond_set(result_temp, CC_EQ);
                }
                break;
            }
            case TK_TILDE:  /* Bitwise NOT */
                ir_emit_not(result_temp, operand_temp);
                break;
            default:
                /* Unary + (identity): keep the operand's float-ness so a
                 * following mixed int/FP op does not cvtsi2sd the bits
                 * (c-torture execute/ieee/mzero2: `+1.00 / pzero`). An FP
                 * operand must move through an FP-class temp (fmov), not
                 * a GP mov_reg. */
                if (c->last_expr_is_float) {
                    ir_emit_fmov((uint16_t)result_temp, (uint16_t)operand_temp);
                    return result_temp;
                }
                ir_emit_mov_reg(result_temp, operand_temp);
                break;
        }
        /* C99 width semantics: bitwise NOT produces a full-width bit
         * pattern that must be re-extended to the operand's int width.
         * (Negation of an already-extended operand stays extended; the
         * INT_MIN case is UB in C.) */
        if (node->aux.op == TK_TILDE) {
            TypeInfo *u_ti = ast_node_get_typeinfo(c, node);
            if (u_ti) {
                size_t wsz = type_size(u_ti);
                if (wsz == 1 || wsz == 2 || wsz == 4) {
                    bool wu = (u_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                    ir_mark_last_width(wu ? (uint8_t)(8 + wsz) : (uint8_t)wsz);
                }
            }
        }
        if (node->aux.op != TK_PLUS && node->aux.op != '+')
            c->last_expr_is_float = 0;
        return result_temp;
}

int lower_expr_cast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        TypeInfo *cast_ti = ast_node_get_typeinfo(c, node);
        bool target_is_float = type_is_floating(cast_ti);
        ZeroCCASTNode *cast_src = zero_cc_ast_get(arena, node->left);
        if (cast_src && cast_src->tag == NODE_FLOAT && !target_is_float &&
            !(cast_ti && cast_ti->kind == TYPE_KIND_COMPLEX)) {
            double f = cast_src->float_val;
            int64_t ival;
            size_t sz = cast_ti ? type_size(cast_ti) : 4;
            bool is_u = cast_ti && (cast_ti->modifiers & TYPE_MOD_UNSIGNED);
            if (sz == 4 && !is_u) {
                if (f >= 2147483647.0) ival = 2147483647;
                else if (f <= -2147483648.0) ival = -2147483648LL;
                else ival = (int32_t)f;
            } else if (sz == 4 && is_u) {
                if (f >= 4294967295.0) ival = 4294967295ULL;
                else if (f <= 0.0) ival = 0;
                else ival = (uint32_t)f;
            } else if (sz == 8 && !is_u) {
                if (f >= 9223372036854775807.0) ival = 9223372036854775807LL;
                else if (f <= -9223372036854775808.0) ival = (-9223372036854775807LL - 1);
                else ival = (int64_t)f;
            } else if (sz == 8 && is_u) {
                if (f >= 18446744073709551615.0) ival = (int64_t)18446744073709551615ULL;
                else if (f <= 0.0) ival = 0;
                else ival = (int64_t)(uint64_t)f;
            } else if (sz == 1 && !is_u) {
                if (f >= 127.0) ival = 127;
                else if (f <= -128.0) ival = -128;
                else ival = (int8_t)f;
            } else if (sz == 1 && is_u) {
                if (f >= 255.0) ival = 255;
                else if (f <= 0.0) ival = 0;
                else ival = (uint8_t)f;
            } else if (sz == 2 && !is_u) {
                if (f >= 32767.0) ival = 32767;
                else if (f <= -32768.0) ival = -32768;
                else ival = (int16_t)f;
            } else if (sz == 2 && is_u) {
                if (f >= 65535.0) ival = 65535;
                else if (f <= 0.0) ival = 0;
                else ival = (uint16_t)f;
            } else {
                ival = (int64_t)f;
            }
            int int_res_temp = c->next_temp_id++;
            ir_emit_mov_imm(int_res_temp, ival);
            c->last_expr_is_float = 0;
            return int_res_temp;
        }
        int result = lower_expr(c, arena, node->left);
        /* Scalar -> vector cast (GCC requires matching sizes): the vector's bit
         * pattern is the low sizeof(vector) bytes of the scalar value.
         * The result must be a fresh vector object in ADDRESS form
         * (vector expressions are always address form) - e.g.
         * `(V2HI)0` / `(V2SI)(long long)x` in c-torture
         * execute/20050316-1/-3. */
        {
            ZeroCCASTNode *csrc2 = zero_cc_ast_get(arena, node->left);
            TypeInfo *cst2 = csrc2 ? ast_node_get_typeinfo(c, csrc2) : NULL;
            /* C99 6.5.4: cast to a complex type.  A scalar operand
             * becomes (value, 0); a complex operand is copied.  The
             * result is a fresh address-form complex object. */
            if (cast_ti && cast_ti->kind == TYPE_KIND_COMPLEX) {
                const TypeInfo *cel = cast_ti->details.array.base;
                size_t cesz = cel ? type_size(cel) : 8;
                if (cesz != 4 && cesz != 8) cesz = 8; /* long double: double model */
                c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + (int32_t)(cesz * 2) + 7) & ~7;
                int32_t coff = c->current_stack_frame;
                if (cst2 && cst2->kind == TYPE_KIND_COMPLEX) {
                    /* complex -> complex: copy both halves from the
                     * source object address. */
                    int ca = c->next_temp_id++;
                    ir_emit_lea(ca, BE(c)->fp_reg, -coff);
                    int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, (int)cesz);
                    int a2 = c->next_temp_id++; ir_emit_add(a2, ca, o2);
                    if (cesz == 4) {
                        int sr = c->next_temp_id++, si = c->next_temp_id++;
                        ir_emit_vloadss((uint16_t)sr, (uint16_t)result);
                        int so2 = c->next_temp_id++; ir_emit_mov_imm(so2, (int)cesz);
                        int sa2 = c->next_temp_id++; ir_emit_add(sa2, result, so2);
                        ir_emit_vloadss((uint16_t)si, (uint16_t)sa2);
                        ir_emit_vstoress((uint16_t)ca, (uint16_t)sr);
                        ir_emit_vstoress((uint16_t)a2, (uint16_t)si);
                    } else {
                        int sr = c->next_temp_id++, si = c->next_temp_id++;
                        ir_emit_vloadsd((uint16_t)sr, (uint16_t)result);
                        int so2 = c->next_temp_id++; ir_emit_mov_imm(so2, (int)cesz);
                        int sa2 = c->next_temp_id++; ir_emit_add(sa2, result, so2);
                        ir_emit_vloadsd((uint16_t)si, (uint16_t)sa2);
                        ir_emit_vstoresd((uint16_t)ca, (uint16_t)sr);
                        ir_emit_vstoresd((uint16_t)a2, (uint16_t)si);
                    }
                } else if (c->last_expr_is_float) {
                    int ca = c->next_temp_id++;
                    ir_emit_lea(ca, BE(c)->fp_reg, -coff);
                    int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, (int)cesz);
                    int a2 = c->next_temp_id++; ir_emit_add(a2, ca, o2);
                    if (cesz == 4) {
                        ir_emit_vstoress((uint16_t)ca, (uint16_t)result);
                        int zr = c->next_temp_id++;
                        ir_emit_fmov_imm((uint16_t)zr, (int64_t)zc_rodata_double_offset(c, 0.0));
                        ir_emit_vstoress((uint16_t)a2, (uint16_t)zr);
                    } else {
                        ir_emit_fstore((uint16_t)result, TEMP_NONE, -coff);
                        int zr = c->next_temp_id++;
                        ir_emit_fmov_imm((uint16_t)zr, (int64_t)zc_rodata_double_offset(c, 0.0));
                        ir_emit_fstore((uint16_t)zr, TEMP_NONE, -coff + (int)cesz);
                    }
                } else {
                    /* Integer scalar -> (double value, 0). */
                    int dv = zc_emit_int_to_double(c, result, cst2);
                    int ca = c->next_temp_id++;
                    ir_emit_lea(ca, BE(c)->fp_reg, -coff);
                    int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, (int)cesz);
                    int a2 = c->next_temp_id++; ir_emit_add(a2, ca, o2);
                    if (cesz == 4) {
                        ir_emit_vstoress((uint16_t)ca, (uint16_t)dv);
                        int zr = c->next_temp_id++;
                        ir_emit_fmov_imm((uint16_t)zr, (int64_t)zc_rodata_double_offset(c, 0.0));
                        ir_emit_vstoress((uint16_t)a2, (uint16_t)zr);
                    } else {
                        ir_emit_fstore((uint16_t)dv, TEMP_NONE, -coff);
                        int zr = c->next_temp_id++;
                        ir_emit_fmov_imm((uint16_t)zr, (int64_t)zc_rodata_double_offset(c, 0.0));
                        ir_emit_fstore((uint16_t)zr, TEMP_NONE, -coff + (int)cesz);
                    }
                }
                int caddr = c->next_temp_id++;
                ir_emit_lea(caddr, BE(c)->fp_reg, -coff);
                c->last_expr_is_float = 0;
                return caddr;
            }
            if (cast_ti && type_is_vector(cast_ti) && (!cst2 || !type_is_vector(cst2))) {
                size_t vsz = type_size(cast_ti);
                if (vsz > 0 && vsz <= 128) {
                    c->current_stack_frame = (c->current_stack_frame + (int32_t)vsz + 7) & ~7;
                    int32_t voff = c->current_stack_frame;
                    const TypeInfo *el2 = cast_ti->details.array.base;
                    bool efl = el2 && type_is_floating(el2);
                    if (efl && !c->last_expr_is_float) {
                        int convf = c->next_temp_id++;
                        ir_emit_cvtsi2sd((uint16_t)convf, (uint16_t)result);
                        result = convf;
                        c->last_expr_is_float = 1;
                    }
                    size_t sz = (vsz <= 8) ? vsz : 8;
                    if (efl) {
                        if (sz == 4) {
                            int fa = c->next_temp_id++;
                            ir_emit_lea(fa, BE(c)->fp_reg, -voff);
                            ir_emit_vstoress((uint16_t)fa, (uint16_t)result);
                        } else {
                            ir_emit_fstore((uint16_t)result, BE(c)->fp_reg, -voff);
                        }
                    } else {
                        if (sz == 1) ir_emit_store_byte((uint16_t)result, (uint16_t)voff);
                        else if (sz == 2) {
                            int a2 = c->next_temp_id++;
                            ir_emit_lea(a2, TEMP_NONE, -voff);
                            ir_emit_store_16((uint16_t)result, (uint16_t)a2);
                        } else if (sz == 4) {
                            int a2 = c->next_temp_id++;
                            ir_emit_lea(a2, TEMP_NONE, -voff);
                            ir_emit_store_32((uint16_t)result, (uint16_t)a2);
                        } else {
                            ir_emit_store((uint16_t)result, BE(c)->fp_reg, -voff);
                        }
                    }
                    /* Any bytes beyond the scalar's width (only possible
                     * when the vector is wider than the source type) are
                     * zero. */
                    if (vsz > sz) {
                        int zt = c->next_temp_id++;
                        ir_emit_mov_imm(zt, 0);
                        for (size_t off = sz; off < vsz; off += 8) {
                            size_t rem = vsz - off;
                            if (rem >= 8) {
                                ir_emit_store((uint16_t)zt, BE(c)->fp_reg, -(int32_t)(voff - (int32_t)off));
                            } else {
                                int a2 = c->next_temp_id++;
                                ir_emit_lea(a2, TEMP_NONE, -(int32_t)(voff - (int32_t)off));
                                if (rem >= 4) ir_emit_store_32((uint16_t)zt, (uint16_t)a2);
                                else if (rem >= 2) ir_emit_store_16((uint16_t)zt, (uint16_t)a2);
                                else ir_emit_store_byte((uint16_t)zt, (uint16_t)a2);
                            }
                        }
                    }
                    int vaddr2 = c->next_temp_id++;
                    ir_emit_lea(vaddr2, BE(c)->fp_reg, -voff);
                    c->last_expr_is_float = 0;
                    return vaddr2;
                }
            }
        }
        /* Vector -> same-size scalar cast (GCC requires matching sizes):
         * the vector lowers to the ADDRESS of the vector object; read
         * the whole vector as one scalar value of the target width.
         * Float targets use the FP load (widening in the double model). */
        {
            ZeroCCASTNode *csrc = zero_cc_ast_get(arena, node->left);
            TypeInfo *cst = csrc ? ast_node_get_typeinfo(c, csrc) : NULL;
            if (cst && type_is_vector(cst) && cast_ti && !type_is_vector(cast_ti)) {
                size_t wsz = type_size(cast_ti);
                int wv = c->next_temp_id++;
                if (target_is_float) {
                    if (wsz == 8) { ir_emit_vloadsd((uint16_t)wv, (uint16_t)result); c->last_expr_is_float = 1; }
                    else { ir_emit_vloadss((uint16_t)wv, (uint16_t)result); c->last_expr_is_float = 1; }
                } else {
                    if (wsz == 1) ir_emit_load_byte(wv, result);
                    else if (wsz == 2) ir_emit_load_16(wv, result);
                    else if (wsz == 4) ir_emit_load_32(wv, result);
                    else ir_emit_deref(wv, result);
                    c->last_expr_is_float = 0;
                }
                result = wv;
            }
        }
        if (target_is_float) {
            if (!c->last_expr_is_float) {
                /* int/pointer -> float/double: convert (C9). Previously
                 * this case fell through and returned the raw integer
                 * bits as if they were a double bit pattern (int 3 ->
                 * 3.0e-323), with last_expr_is_float left 0 so callers
                 * (e.g. the call-argument path) took the integer route.
                 *
                 * 64-bit *unsigned* sources (unsigned long / size_t /
                 * pointers on LP64) need the two-term form: cvtsi2sd
                 * reads the bits as *signed*, so values with the high
                 * bit set come out as (v - 2^64); add back 2^64 scaled
                 * by the top bit:  r = cvtsi2sd(v) + (double)(v >> 63) * 2^64. */
                ZeroCCASTNode *cast_src = zero_cc_ast_get(arena, node->left);
                TypeInfo *src_ti = cast_src ? ast_node_get_typeinfo(c, cast_src) : NULL;
                bool src_u64 = src_ti && type_size(src_ti) == 8 &&
                    ((src_ti->kind == TYPE_KIND_LONG && (src_ti->modifiers & TYPE_MOD_UNSIGNED) != 0) ||
                     src_ti->kind == TYPE_KIND_POINTER);
                /* 64-bit unsigned int literal (ULL/UL suffix): semantic
                 * analysis types every int literal plain 'int', so fall
                 * back to the stored suffix (bit0=u, bit1=l, bit2=ll). */
                if (!src_u64 && cast_src && cast_src->tag == NODE_INT &&
                    (cast_src->flags & 0x1) && (cast_src->flags & 0x6)) {
                    src_u64 = true;
                }
                if (src_u64) {
                    /* When casting to single-precision float (type_size 4), collapse sticky
                     * bits below bit 31 into bit 31 to prevent double-rounding when converting
                     * to float via intermediate double. */
                    if (cast_ti && type_size(cast_ti) == 4) {
                        int mask_lo = c->next_temp_id++;
                        ir_emit_mov_imm(mask_lo, 0x7FFFFFFFLL);
                        int has_sticky = c->next_temp_id++;
                        ir_emit_and(has_sticky, result, mask_lo);
                        int zero_sticky = c->next_temp_id++;
                        ir_emit_mov_imm(zero_sticky, 0);
                        int cond_sticky = c->next_temp_id++;
                        ir_emit_cmp(has_sticky, zero_sticky);
                        ir_emit_cond_set(cond_sticky, CC_NE);
                        /* Seed the collapsed result with the identity so
                         * the join has a defined value on BOTH paths -
                         * defining it only inside the taken branch left an
                         * undefined temp when the branch was skipped
                         * (c-torture execute/conversion: (float)2^63). */
                        int clean_res = c->next_temp_id++;
                        ir_emit_mov_reg(clean_res, result);
                        uint16_t skip_sticky = c->ir_next_label++;
                        ir_emit_je((uint16_t)cond_sticky, skip_sticky);
                        int bit31 = c->next_temp_id++;
                        ir_emit_mov_imm(bit31, 0x80000000LL);
                        int or_res = c->next_temp_id++;
                        ir_emit_or(or_res, result, bit31);
                        int mask_hi = c->next_temp_id++;
                        ir_emit_mov_imm(mask_hi, ~0x7FFFFFFFLL);
                        ir_emit_and(clean_res, or_res, mask_hi);
                        result = clean_res;
                        ir_emit_label(skip_sticky);
                    }

                    /* Standard uint64 -> double IEEE-754 conversion:
                     * If high bit is not set (result >= 0), cvtsi2sd directly.
                     * If high bit is set (< 0), compute ((result >> 1) | (result & 1)) * 2.0
                     * to preserve the sticky bit and avoid double-rounding. */
                    int zero_temp = c->next_temp_id++;
                    ir_emit_mov_imm(zero_temp, 0);
                    int is_ge = c->next_temp_id++;
                    ir_emit_cmp(result, zero_temp);
                    ir_emit_cond_set(is_ge, CC_GE);

                    int fp_res = c->next_temp_id++;
                    uint16_t neg_label = c->ir_next_label++;
                    uint16_t done_label = c->ir_next_label++;

                    ir_emit_je((uint16_t)is_ge, neg_label);

                    /* Positive (high bit clear): */
                    int pos_d = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)pos_d, (uint16_t)result);
                    ir_emit_fmov((uint16_t)fp_res, (uint16_t)pos_d);
                    ir_emit_jmp(done_label);

                    /* Negative (high bit set): */
                    ir_emit_label(neg_label);
                    int one_temp = c->next_temp_id++;
                    ir_emit_mov_imm(one_temp, 1);
                    int odd_temp = c->next_temp_id++;
                    ir_emit_and(odd_temp, result, one_temp);

                    int shr_temp = c->next_temp_id++;
                    ir_emit((Instr){.op = OP_SHR, .temp_dest = (uint16_t)shr_temp,
                        .temp_src1 = (uint16_t)result, .temp_src2 = (uint16_t)one_temp});

                    int adj_temp = c->next_temp_id++;
                    ir_emit_or(adj_temp, shr_temp, odd_temp);

                    int half_d = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)half_d, (uint16_t)adj_temp);

                    int double_d = c->next_temp_id++;
                    ir_emit_fadd((uint16_t)double_d, (uint16_t)half_d, (uint16_t)half_d);
                    ir_emit_fmov((uint16_t)fp_res, (uint16_t)double_d);

                    ir_emit_label(done_label);
                    c->last_expr_is_float = 1;
                    result = fp_res;
                } else {
                    int fp_res = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)fp_res, (uint16_t)result);
                    c->last_expr_is_float = 1;
                    result = fp_res;
                }
            }
            if (cast_ti && type_size(cast_ti) == 4) {
                /* Explicit cast to float: round double to single-precision float */
                int addr = c->next_temp_id++;
                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                c->current_stack_frame += 8;
                int32_t off = c->current_stack_frame;
                ir_emit_lea(addr, BE(c)->fp_reg, -off);
                ir_emit_vstoress(addr, result);
                int rounded = c->next_temp_id++;
                ir_emit_vloadss(rounded, addr);
                result = rounded;
            }
            return result;
        }
        /* Float to int conversion: emit via IR for correct sequencing */
        if (c->last_expr_is_float) {
            bool is_u64 = cast_ti && type_size(cast_ti) == 8 &&
                          (cast_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
            if (is_u64) {
                double two63 = 9223372036854775808.0;
                size_t ro63 = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, &two63, sizeof(double));
                int two63_f = c->next_temp_id++;
                ir_emit_fmov_imm((uint16_t)two63_f, (int64_t)ro63);

                int cond_ge = c->next_temp_id++;
                ir_emit_cmp(result, two63_f);
                ir_emit_cond_set(cond_ge, CC_GE);

                int sub_f = c->next_temp_id++;
                ir_emit_fsub((uint16_t)sub_f, (uint16_t)result, (uint16_t)two63_f);

                int hi_i = c->next_temp_id++;
                ir_emit_cvttsd2si((uint16_t)hi_i, (uint16_t)sub_f);

                int bit63 = c->next_temp_id++;
                ir_emit_mov_imm(bit63, (int64_t)0x8000000000000000ULL);
                int hi_adj = c->next_temp_id++;
                ir_emit_xor(hi_adj, hi_i, bit63);

                int lo_i = c->next_temp_id++;
                ir_emit_cvttsd2si((uint16_t)lo_i, (uint16_t)result);

                uint16_t label_lo = c->ir_next_label++;
                uint16_t label_done = c->ir_next_label++;
                ir_emit_cmp_zero(cond_ge);
                ir_emit_je((uint16_t)cond_ge, label_lo);

                int int_res_temp = c->next_temp_id++;
                ir_emit_mov_reg(int_res_temp, hi_adj);
                ir_emit_jmp(label_done);

                ir_emit_label(label_lo);
                ir_emit_mov_reg(int_res_temp, lo_i);

                ir_emit_label(label_done);
                c->last_expr_is_float = 0;
                return int_res_temp;
            }
            int int_res_temp = c->next_temp_id++;
            /* Out-of-range float -> int: clamp to the target's range
             * (GCC -fno-trapping-math semantics; C99 6.3.1.4 leaves it
             * undefined): (int)2147483648.0f must be INT_MAX,
             * (unsigned)(-1.0) must be 0. Two FP compares and
             * conditional FP replaces keep the value in the FP model. */
            if (cast_ti) {
                size_t w = type_size(cast_ti);
                bool uns = (cast_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                if (w == 1 || w == 2 || w == 4 || w == 8) {
                    double upper_b, lower_b, upper_c, lower_c;
                    if (uns) {
                        upper_b = (w == 1) ? 256.0 : (w == 2) ? 65536.0 : (w == 4) ? 4294967296.0 : 18446744073709551616.0;
                        upper_c = (w == 1) ? 255.0 : (w == 2) ? 65535.0 : (w == 4) ? 4294967295.0 : 18446744073709551615.0;
                        lower_b = 0.0;
                        lower_c = 0.0;
                    } else {
                        upper_b = (w == 1) ? 128.0 : (w == 2) ? 32768.0 : (w == 4) ? 2147483648.0 : 9223372036854775808.0;
                        upper_c = (w == 1) ? 127.0 : (w == 2) ? 32767.0 : (w == 4) ? 2147483647.0 : 9223372036854775807.0;
                        lower_b = (w == 1) ? -128.0 : (w == 2) ? -32768.0 : (w == 4) ? -2147483648.0 : -9223372036854775808.0;
                        lower_c = (w == 1) ? -128.0 : (w == 2) ? -32768.0 : (w == 4) ? -2147483648.0 : -9223372036854775808.0;
                    }
                    int n_bounds = (w == 8) ? 1 : 2;
                    double bnd[2] = { upper_b, lower_b };
                    double clp[2] = { upper_c, lower_c };
                    size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
                    if (pad) { uint8_t z8[8] = {0}; zero_cc_buf_put_bytes(&c->rodata_section, z8, pad); }
                    size_t ro = c->rodata_section.length;
                    for (int k = 0; k < n_bounds; k++) {
                        zero_cc_buf_put_bytes(&c->rodata_section, &bnd[k], sizeof(double));
                        zero_cc_buf_put_bytes(&c->rodata_section, &clp[k], sizeof(double));
                    }
                    for (int k = 0; k < n_bounds; k++) {
                        int bt = c->next_temp_id++;
                        ir_emit_fmov_imm((uint16_t)bt, (int64_t)(ro + 16 * k));
                        int ct = c->next_temp_id++;
                        ir_emit_cmp(result, bt);
                        ir_emit_cond_set(ct, k == 0 ? CC_GE : CC_LT);
                        uint16_t skip = c->ir_next_label++;
                        ir_emit_je((uint16_t)ct, skip);
                        int cv = c->next_temp_id++;
                        ir_emit_fmov_imm((uint16_t)cv, (int64_t)(ro + 16 * k + 8));
                        ir_emit_fmov((uint16_t)result, (uint16_t)cv);
                        ir_emit_label(skip);
                    }
                }
            }
            ir_emit_cvttsd2si((uint16_t)int_res_temp, (uint16_t)result);
            c->last_expr_is_float = 0;
            /* cvttsd2si yields a 64-bit int; extend to the cast target
             * width when it is a narrow int ((int)d, (short)d). */
            if (cast_ti) {
                size_t f2isz = type_size(cast_ti);
                if (f2isz == 1 || f2isz == 2 || f2isz == 4) {
                    bool f2isu = (cast_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                    if (f2isu) ir_emit_zext(int_res_temp, (int)f2isz);
                    else ir_emit_sext(int_res_temp, (int)f2isz);
                }
            }
            return int_res_temp;
        }
        /* Vector->vector cast: same-size bit reinterpret (GCC allows
         * casts between vectors of different element types, e.g.
         * `(vector float)(vector int)x`). The source lowers to the
         * address of the vector object; copy its raw bytes into a
         * fresh temp object of the target vector type. */
        if (cast_ti && type_is_vector(cast_ti)) {
            ZeroCCASTNode *cs = zero_cc_ast_get(arena, node->left);
            TypeInfo *cst = cs ? ast_node_get_typeinfo(c, cs) : NULL;
            if (cst && type_is_vector(cst)) {
            size_t ctsz = type_size(cast_ti);
            if (ctsz >= 1 && ctsz <= 128) {
                c->current_stack_frame = (c->current_stack_frame + (int32_t)ctsz + 7) & ~7;
                int32_t vres_off = c->current_stack_frame;
                int saddr = result; /* source vector in address form */
                for (size_t off = 0; off < ctsz; off += 8) {
                    size_t rem = ctsz - off;
                    int ca = c->next_temp_id++;
                    if (off == 0) { ir_emit_mov_reg(ca, saddr); }
                    else {
                        int o = c->next_temp_id++;
                        ir_emit_mov_imm(o, (int32_t)off);
                        ir_emit_add(ca, saddr, o);
                    }
                    int cv = c->next_temp_id++;
                    if (rem >= 8) ir_emit_deref(cv, ca);
                    else if (rem >= 4) ir_emit_load_32(cv, ca);
                    else if (rem >= 2) ir_emit_load_16(cv, ca);
                    else ir_emit_load_byte(cv, ca);
                    int da = c->next_temp_id++;
                    if (off == 0) { ir_emit_lea(da, BE(c)->fp_reg, -vres_off); }
                    else { ir_emit_lea(da, BE(c)->fp_reg, -(int32_t)(vres_off - (int32_t)off)); }
                    if (rem >= 8) ir_emit_store_64(cv, da);
                    else if (rem >= 4) ir_emit_store_32(cv, da);
                    else if (rem >= 2) ir_emit_store_16(cv, da);
                    else ir_emit_store_byte(cv, da);
                }
                int vaddr = c->next_temp_id++;
                ir_emit_lea(vaddr, BE(c)->fp_reg, -vres_off);
                c->last_expr_is_float = 0;
                return vaddr;
            }
            }
        }
        /* int->int narrowing cast ((int)longv, (short)x): extend the
         * 64-bit value to the target width so it carries true N-bit
         * semantics (a plain cast previously did nothing). The
         * truncation must go to a FRESH temp - in-place zext would
         * destroy a source operand that is used again after the cast
         * (`if (((int)x | (int)y) != 0) ...; return x + y;` -
         * c-torture execute/pr59014-2). */
        if (cast_ti) {
            size_t ctsz = type_size(cast_ti);
            if (ctsz == 1 || ctsz == 2 || ctsz == 4) {
                bool ctsu = (cast_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                int cast_t = c->next_temp_id++;
                ir_emit_mov_reg(cast_t, result);
                if (ctsu) ir_emit_zext(cast_t, (int)ctsz);
                else ir_emit_sext(cast_t, (int)ctsz);
                result = cast_t;
            }
        }
        return result;
}
