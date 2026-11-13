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
int zc_cx_fabs(ZeroCCCompiler *c, int ftemp) {  /* Sprint 114: extern (shared with ast_lowering_unop.c) */
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
    c->current_stack_frame += 8;
    int32_t slot = c->current_stack_frame;
    ir_emit_fstore(c, (uint32_t)ftemp, TEMP_NONE, -slot);
    int it = zc_alloc_temp(c);
    ir_emit_load(c, it, BE(c)->fp_reg, -slot);
    int mk = zc_alloc_temp(c);
    ir_emit_mov_imm(c, mk, 0x7FFFFFFFFFFFFFFFLL);
    int at = zc_alloc_temp(c);
    ir_emit_and(c, at, it, mk);
    ir_emit_store(c, at, BE(c)->fp_reg, -slot);
    int rt = zc_alloc_temp(c);
    ir_emit_fload(c, (uint32_t)rt, TEMP_NONE, -slot);
    return rt;
}

/* Branchless FP select of two double-width FP-model temps: returns va if
 * cond != 0, else vb (bit-level select on the IEEE bits, the same
 * technique __builtin_shuffle uses for vector lanes). */
int zc_cx_fselect(ZeroCCCompiler *c, int cond, int va, int vb) {  /* Sprint 114: extern (shared with ast_lowering_unop.c) */
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
    c->current_stack_frame += 16;
    int32_t s_a = c->current_stack_frame;
    int32_t s_b = s_a - 8;
    ir_emit_fstore(c, (uint32_t)va, TEMP_NONE, -s_a);
    ir_emit_fstore(c, (uint32_t)vb, TEMP_NONE, -s_b);
    int ia = zc_alloc_temp(c);
    ir_emit_load(c, ia, BE(c)->fp_reg, -s_a);
    int ib = zc_alloc_temp(c);
    ir_emit_load(c, ib, BE(c)->fp_reg, -s_b);
    int ones = zc_alloc_temp(c);
    ir_emit_mov_imm(c, ones, (int32_t)-1);
    int m = zc_alloc_temp(c);
    ir_emit_mul(c, m, cond, ones);          /* -1 or 0 */
    int nm = zc_alloc_temp(c);
    ir_emit_xor(c, nm, m, ones);            /* ~m */
    int sa = zc_alloc_temp(c);
    ir_emit_and(c, sa, ia, m);
    int sb = zc_alloc_temp(c);
    ir_emit_and(c, sb, ib, nm);
    int r = zc_alloc_temp(c);
    ir_emit_or(c, r, sa, sb);
    ir_emit_store(c, r, BE(c)->fp_reg, -s_a);
    int rt = zc_alloc_temp(c);
    ir_emit_fload(c, (uint32_t)rt, TEMP_NONE, -s_a);
    return rt;
}

/* Infer the C99 narrow-int width of an operand expression when it can be
 * determined cheaply: 1/2/4 (with *sign set) or 0 (unknown / 8-byte /
 * pointer / non-int). Used for binop result-width marking and signed >>.
 * Covers the common operand shapes; anything exotic returns 0 (no mark,
 * conservative). */
int zero_cc_operand_int_width(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                              ASTRef operand, int *sign) {  /* Sprint 114: extern (shared with ast_lowering_unop.c) */
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
            if (suf & 4u) {              /* ll: 64-bit */
                *sign = !(suf & 1u);
                return 0;                /* no narrowing */
            }
            if (suf & 2u) {              /* l: 32-bit on RV32, 64-bit on LP64 */
                *sign = !(suf & 1u);
                if (c->isa == ISA_RISCV32) return 4;
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
            return (c->isa == ISA_RISCV32) ? 4 : 8;
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
            if (suf & 4u) {
                *is_unsigned = (suf & 1u) ? 1 : 0;
                return 8;
            }
            if (suf & 2u) {
                *is_unsigned = (suf & 1u) ? 1 : 0;
                return (c->isa == ISA_RISCV32) ? 4 : 8;
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
int zc_binop_bitfield_reduce(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                             ASTRef left_ref, ASTRef right_ref, int result_temp) {  /* Sprint 114: extern (shared with ast_lowering_unop.c) */
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
    int mask_t = zc_alloc_temp(c);
    ir_emit_mov_imm(c, mask_t, (int64_t)mask);
    int red = zc_alloc_temp(c);
    ir_emit_and(c, red, result_temp, mask_t);
    result_temp = red;
    if (is_signed) {
        int64_t sign_bit = (int64_t)(1ULL << (bit_width - 1));
        int sign_t = zc_alloc_temp(c);
        ir_emit_mov_imm(c, sign_t, sign_bit);
        int x = zc_alloc_temp(c);
        ir_emit_xor(c, x, result_temp, sign_t);
        int e = zc_alloc_temp(c);
        ir_emit_sub(c, e, x, sign_t);
        result_temp = e;
    }
    return result_temp;
}

/* GNU __int128 (Sprint 109): 64-bit (x op y) -> boolean temp (0/1). */
int zc_cmp64_bool(ZeroCCCompiler *c, int x, int y, int cc) {  /* Sprint 114: extern (shared with ast_lowering_unop.c) */
    int b = zc_alloc_temp(c);
    ir_emit_cmp(c, x, y);
    ir_emit_cond_set(c, b, cc);
    return b;
}

/* GNU __int128 (Sprint 109): emit a constant-count 128-bit shift of the
 * (lo,hi) pair (s_lo,s_hi) into (dlo,dhi). op is TK_SHL or TK_SHR.  A TK_SHR
 * is LOGICAL for unsigned types and ARITHMETIC (sign-filling) for signed
 * __int128 (the `signed_` flag - a signed shift must fill the vacated high
 * bits with the sign bit, which a plain logical decomposition cannot do).
 * Shared by the binop lowering and the compound-assignment (<<=, >>=) path. */
void zc_emit_128_shift(ZeroCCCompiler *c, int dlo, int dhi, int s_lo, int s_hi,
                       int64_t N, uint32_t op, bool signed_) {
    if (op == TK_SHL) {
        if (N < 64) {
            /* dlo = s_lo << N ; dhi = (s_hi << N) | (s_lo >> (64-N)) */
            int n1 = zc_alloc_temp(c); ir_emit_mov_imm(c, n1, N);
            ir_emit_shl(c, dlo, s_lo, n1);
            int n2 = zc_alloc_temp(c); ir_emit_mov_imm(c, n2, 64 - N);
            int b  = zc_alloc_temp(c); ir_emit_shr(c, b, s_lo, n2);   /* s_lo >> (64-N) */
            int n3 = zc_alloc_temp(c); ir_emit_mov_imm(c, n3, N);
            int a  = zc_alloc_temp(c); ir_emit_shl(c, a, s_hi, n3);   /* s_hi << N */
            ir_emit_or(c, dhi, a, b);
        } else if (N == 64) { ir_emit_mov_imm(c, dlo, 0); ir_emit_mov_reg(c, dhi, s_lo); }  /* s_lo moves to hi */
        else if (N < 128) { int n = zc_alloc_temp(c); ir_emit_mov_imm(c, n, N - 64); ir_emit_mov_imm(c, dlo, 0); ir_emit_shl(c, dhi, s_lo, n); }  /* dhi = s_lo << (N-64) */
        else { ir_emit_mov_imm(c, dlo, 0); ir_emit_mov_imm(c, dhi, 0); }
    } else if (signed_) { /* TK_SHR: arithmetic (signed __int128) */
        if (N < 64) {
            /* dlo = (s_lo >> N) | (s_hi << (64-N)) ; dhi = s_hi >>N (arith) */
            int n1 = zc_alloc_temp(c); ir_emit_mov_imm(c, n1, N);
            int a  = zc_alloc_temp(c); ir_emit_shr(c, a, s_lo, n1);
            int n2 = zc_alloc_temp(c); ir_emit_mov_imm(c, n2, 64 - N);
            int b  = zc_alloc_temp(c); ir_emit_shl(c, b, s_hi, n2);
            ir_emit_or(c, dlo, a, b);
            int n3 = zc_alloc_temp(c); ir_emit_mov_imm(c, n3, N);
            ir_emit_sar(c, dhi, s_hi, n3);   /* sign-fills the vacated bits */
        } else if (N == 64) {
            ir_emit_mov_reg(c, dlo, s_hi);
            int n = zc_alloc_temp(c); ir_emit_mov_imm(c, n, 63);
            ir_emit_sar(c, dhi, s_hi, n);    /* all-ones or all-zeros */
        } else if (N < 128) {
            int n  = zc_alloc_temp(c); ir_emit_mov_imm(c, n, N - 64);
            ir_emit_sar(c, dlo, s_hi, n);
            int n2 = zc_alloc_temp(c); ir_emit_mov_imm(c, n2, 63);
            ir_emit_sar(c, dhi, s_hi, n2);   /* all-ones or all-zeros */
        } else {
            /* N >= 128: the value is its sign (0 or -1) on all practical
             * targets (C11 6.5.7p3 leaves negative >> count >= width
             * implementation-defined; every real target sign-fills). */
            int n = zc_alloc_temp(c); ir_emit_mov_imm(c, n, 63);
            ir_emit_sar(c, dlo, s_hi, n);
            ir_emit_mov_reg(c, dhi, dlo);
        }
    } else { /* TK_SHR: logical (unsigned) */
        if (N < 64) {
            int n1 = zc_alloc_temp(c); ir_emit_mov_imm(c, n1, N);
            int a  = zc_alloc_temp(c); ir_emit_shr(c, a, s_lo, n1);
            int n2 = zc_alloc_temp(c); ir_emit_mov_imm(c, n2, 64 - N);
            int b  = zc_alloc_temp(c); ir_emit_shl(c, b, s_hi, n2);
            ir_emit_or(c, dlo, a, b);
            int n3 = zc_alloc_temp(c); ir_emit_mov_imm(c, n3, N);
            ir_emit_shr(c, dhi, s_hi, n3);
        } else if (N == 64) { ir_emit_mov_reg(c, dlo, s_hi); ir_emit_mov_imm(c, dhi, 0); }
        else if (N < 128) { int n = zc_alloc_temp(c); ir_emit_mov_imm(c, n, N - 64); ir_emit_shr(c, dlo, s_hi, n); ir_emit_mov_imm(c, dhi, 0); }
        else { ir_emit_mov_imm(c, dlo, 0); ir_emit_mov_imm(c, dhi, 0); }
    }
}

/* GNU __int128 (Sprint 109): lower a 128-bit binary op on (lo,hi) GPR128 pairs.
 * Arithmetic -> GPR128 result temp; comparison -> boolean temp. Returns
 * TEMP_NONE if the op is not handled here (caller falls through). Shifts
 * require a compile-time-constant count for this decomposition. */
int zc_lower_binop128(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                      ZeroCCASTNode *node, ZeroCCASTNode *cl_node,
                      ZeroCCASTNode *cr_node, const TypeInfo *cl_ti,
                      const TypeInfo *cr_ti) {  /* Sprint 114: extern (shared with ast_lowering_unop.c) */
    uint32_t op = node->aux.op;
    bool arith = (op == TK_PLUS || op == TK_MINUS || op == TK_STAR ||
                  op == TK_AMP || op == TK_PIPE || op == TK_CARET);
    bool cmp   = (op == TK_EQ || op == TK_NE || op == TK_LT ||
                  op == TK_LE || op == TK_GT || op == TK_GE);
    bool shift = (op == TK_SHL || op == TK_SHR);
    bool divmod = (op == TK_SLASH || op == TK_PERCENT);
    if (!arith && !cmp && !shift && !divmod) return TEMP_NONE;

    const TypeInfo *common = (cl_ti && type_size(cl_ti) == 16) ? cl_ti
                        : (cr_ti && type_size(cr_ti) == 16) ? cr_ti : cl_ti;
    bool unsigned_ = common ? type_is_unsigned(common) : false;

    if (shift) {
        if (!cr_node || cr_node->tag != NODE_INT) return TEMP_NONE;  /* const count only */
        int64_t N = cr_node->int_val;
        int l = lower_expr(c, arena, node->left);
        int l_is128 = c->last_expr_is_128;
        if (!l_is128) {
            int d = zc_alloc_temp(c);
            if (unsigned_) ir_emit_zext64_128(c, d, l); else ir_emit_sext64_128(c, d, l);
            l = d;
        }
        int res = zc_alloc_temp(c);
        if (N <= 0) { ir_emit_mov128(c, res, l); return res; }
        int s_lo = zc_alloc_temp(c); ir_emit_lo128(c, s_lo, l);
        int s_hi = zc_alloc_temp(c); ir_emit_hi128(c, s_hi, l);
        int dlo = zc_alloc_temp(c), dhi = zc_alloc_temp(c);
        zc_emit_128_shift(c, dlo, dhi, s_lo, s_hi, N, op, !unsigned_);
        ir_emit_mk128(c, res, dlo, dhi);
        c->last_expr_is_128 = 1;
        return res;
    }

    /* arithmetic & comparison: widen both operands to GPR128 */
    int l = lower_expr(c, arena, node->left);
    int l_is128 = c->last_expr_is_128;
    int r = lower_expr(c, arena, node->right);
    int r_is128 = c->last_expr_is_128;
    if (!l_is128) { int d = zc_alloc_temp(c); if (cl_ti ? type_is_unsigned(cl_ti) : unsigned_) ir_emit_zext64_128(c, d, l); else ir_emit_sext64_128(c, d, l); l = d; }
    if (!r_is128) { int d = zc_alloc_temp(c); if (cr_ti ? type_is_unsigned(cr_ti) : unsigned_) ir_emit_zext64_128(c, d, r); else ir_emit_sext64_128(c, d, r); r = d; }

    if (divmod) {
        /* 128-bit / and %: call the ZeroCC-owned software routine (zero_rt,
         * not libgcc). The operands are passed as four 64-bit integer args
         * (a_lo, a_hi, b_lo, b_hi) in the normal integer-arg registers; the
         * 128-bit quotient/remainder comes back in the (rax,rdx)=(VREG_0,VREG_2)
         * return pair and is recombined into a GPR128 temp. */
        int alo = zc_alloc_temp(c); ir_emit_lo128(c, alo, l);
        int ahi = zc_alloc_temp(c); ir_emit_hi128(c, ahi, l);
        int blo = zc_alloc_temp(c); ir_emit_lo128(c, blo, r);
        int bhi = zc_alloc_temp(c); ir_emit_hi128(c, bhi, r);
        bool is_div = (op == TK_SLASH);
        const char *fn = is_div ? (unsigned_ ? "zc_udiv128" : "zc_sdiv128")
                                : (unsigned_ ? "zc_umod128" : "zc_smod128");
        /* Register the zero_rt helper so the AOT object emits it as a symbol
         * (a call target absent from the TU symbol table resolves to symbol
         * index 0 -> `call 0 <link_error>`; the JIT resolves it via dlsym). */
        if (c->function_count < ZERO_CC_MAX_FUNCTIONS &&
            !zero_cc_find_function(c, fn)) {
            ZeroCCFunction *zfn = &c->functions[c->function_count++];
            memset(zfn, 0, sizeof(*zfn));
            snprintf(zfn->name, sizeof(zfn->name), "%s", fn);
            zfn->text_offset = 0;
            zfn->returns_struct = false;
            zfn->ret_struct_sz = 0;
            zfn->is_prototype = true;
            zfn->has_prototype = false;
            zfn->is_external = true;
            zfn->is_variadic = false;
        }
        ir_emit_call(c, fn);
        int at[4] = {alo, ahi, blo, bhi};
        if (ir_get_count(c) > 0) {
            Instr *li = (Instr*)&ir_get_buffer(c)[ir_get_count(c) - 1];
            li->temp_dest = 4;
            li->temp_src1 = (uint32_t)at[0];
            li->temp_src2 = (uint32_t)at[1];
            li->imm = IR_CALL_ARG01_REG;
            li->cond = 0;
            li->width = 0;
            ir_emit_call_arg(c, (uint32_t)at[2], 0, 255);
            ir_emit_call_arg(c, (uint32_t)at[3], 0, 255);
        }
        int rlo = zc_alloc_temp(c); ir_emit_mov_reg(c, rlo, VREG_0);
        int rhi = zc_alloc_temp(c); ir_emit_mov_reg(c, rhi, VREG_2);
        int res = zc_alloc_temp(c); ir_emit_mk128(c, res, rlo, rhi);
        c->last_expr_is_128 = 1;
        return res;
    }

    if (arith) {
        int res = zc_alloc_temp(c);
        switch (op) {
            case TK_PLUS:  ir_emit_add128(c, res, l, r); break;
            case TK_MINUS: ir_emit_sub128(c, res, l, r); break;
            case TK_STAR:  ir_emit_mul128(c, res, l, r); break;
            case TK_AMP:   ir_emit_and128(c, res, l, r); break;
            case TK_PIPE:  ir_emit_or128(c, res, l, r);  break;
            case TK_CARET: ir_emit_xor128(c, res, l, r); break;
        }
        c->last_expr_is_128 = 1;
        return res;
    }

    /* comparison: boolean result. hi compared signed/unsigned per type; the
     * tie-break on equal hi compares lo unsigned. */
    int lh = zc_alloc_temp(c); ir_emit_hi128(c, lh, l);
    int ll = zc_alloc_temp(c); ir_emit_lo128(c, ll, l);
    int rh = zc_alloc_temp(c); ir_emit_hi128(c, rh, r);
    int rl = zc_alloc_temp(c); ir_emit_lo128(c, rl, r);
    int hi_cc_lt = unsigned_ ? CC_FLT_LT : CC_LT;
    int hi_cc_gt = unsigned_ ? CC_FLT_GT : CC_GT;
    c->last_expr_is_128 = 0;
    switch (op) {
    case TK_EQ: {
        int eh = zc_cmp64_bool(c, lh, rh, CC_EQ);
        int el = zc_cmp64_bool(c, ll, rl, CC_EQ);
        int a = zc_alloc_temp(c); ir_emit_and(c, a, eh, el); return a;
    }
    case TK_NE: {
        int nh = zc_cmp64_bool(c, lh, rh, CC_NE);
        int nl = zc_cmp64_bool(c, ll, rl, CC_NE);
        int a = zc_alloc_temp(c); ir_emit_or(c, a, nh, nl); return a;
    }
    case TK_LT: {
        int t1 = zc_cmp64_bool(c, lh, rh, hi_cc_lt);
        int t2 = zc_cmp64_bool(c, lh, rh, CC_EQ);
        int t3 = zc_cmp64_bool(c, ll, rl, CC_FLT_LT);
        int t4 = zc_alloc_temp(c); ir_emit_and(c, t4, t2, t3);
        int a = zc_alloc_temp(c); ir_emit_or(c, a, t1, t4); return a;
    }
    case TK_GT: {
        int t1 = zc_cmp64_bool(c, lh, rh, hi_cc_gt);
        int t2 = zc_cmp64_bool(c, lh, rh, CC_EQ);
        int t3 = zc_cmp64_bool(c, ll, rl, CC_FLT_GT);
        int t4 = zc_alloc_temp(c); ir_emit_and(c, t4, t2, t3);
        int a = zc_alloc_temp(c); ir_emit_or(c, a, t1, t4); return a;
    }
    case TK_LE: {
        int t1 = zc_cmp64_bool(c, lh, rh, hi_cc_lt);
        int t2 = zc_cmp64_bool(c, lh, rh, CC_EQ);
        int t3 = zc_cmp64_bool(c, ll, rl, CC_FLT_LE);
        int t4 = zc_alloc_temp(c); ir_emit_and(c, t4, t2, t3);
        int a = zc_alloc_temp(c); ir_emit_or(c, a, t1, t4); return a;
    }
    case TK_GE: {
        int t1 = zc_cmp64_bool(c, lh, rh, hi_cc_gt);
        int t2 = zc_cmp64_bool(c, lh, rh, CC_EQ);
        int t3 = zc_cmp64_bool(c, ll, rl, CC_FLT_GE);
        int t4 = zc_alloc_temp(c); ir_emit_and(c, t4, t2, t3);
        int a = zc_alloc_temp(c); ir_emit_or(c, a, t1, t4); return a;
    }
    }
    return TEMP_NONE;
}

/* GNU __int128 (Sprint 109): true if this expression node is a 128-bit
 * integer value. The semantic TypeInfo is the primary signal, but for some
 * nodes (global idents, anonymous sub-expressions) the attached TypeInfo is
 * unreliable, so also consult the lowered symbol / global type directly. */
bool zc_node_is_int128(ZeroCCCompiler *c, const ZeroCCASTNode *node) {  /* Sprint 114: extern (shared with ast_lowering_unop.c) */
    if (!node) return false;
    const TypeInfo *ti = ast_node_get_typeinfo(c, node);
    if (ti && ti->kind == TYPE_KIND_INT128 && type_size(ti) == 16) return true;
    if (node->tag == NODE_IDENT && node->s.str) {
        ZeroCCSym *s = zero_cc_find_local(c, node->s.str);
        if (s && (s->type.tag == TYPE_INT128 || s->type.tag == TYPE_UINT128)) return true;
        ZeroCCGlobalVar *gv = find_global_var(c, node->s.str);
        if (gv && (gv->type.tag == TYPE_INT128 || gv->type.tag == TYPE_UINT128)) return true;
    }
    return false;
}

/* Sprint 170 (rv32 i64): true when the node's value is a 64-bit integer
 * (the compiler models `long`/`long long` as 64-bit = TYPE_KIND_LONG /
 * TYPE_INT64|UINT64). Pointers are a different kind and never match. */
bool zc_node_is_int64(ZeroCCCompiler *c, const ZeroCCASTNode *node) {
    if (!node) return false;
    const TypeInfo *ti = ast_node_get_typeinfo(c, node);
    if (ti && (ti->kind == TYPE_KIND_LONG || type_is_integer(ti)) && type_size(ti) == 8) return true;
    if (node->tag == NODE_IDENT && node->s.str) {
        ZeroCCSym *s = zero_cc_find_local(c, node->s.str);
        if (s && (s->type.tag == TYPE_INT64 || s->type.tag == TYPE_UINT64)) return true;
        ZeroCCGlobalVar *gv = find_global_var(c, node->s.str);
        if (gv && (gv->type.tag == TYPE_INT64 || gv->type.tag == TYPE_UINT64)) return true;
    }
    if (node->tag == NODE_INT) {
        int64_t v = node->int_val;
        uint8_t suf = (uint8_t)(node->flags & 7u);
        if (suf & 6u) return true;
        if ((suf & 1u) && (v > INT64_C(0xFFFFFFFF) || v < 0)) return true;
        if (v < INT32_MIN || v > INT32_MAX) return true;
    }
    return false;
}

/* Sprint 170 (rv32 i64): lower a 64-bit binop operand to a GPR64 (lo,hi) temp.
 * A NODE_INT constant is emitted directly as a 64-bit immediate (the 32-bit
 * mov_imm path would drop the high word); a 64-bit value (a 64-bit load or a
 * nested 64-bit op) passes through; any narrower integer is sign/zero-
 * extended to 64 per its signedness (C usual arithmetic conversions). */
int zc_operand_to64(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef oref) {
    ZeroCCASTNode *onode = zero_cc_ast_get(arena, oref);
    if (onode && onode->tag == NODE_INT) {
        int us = 0;
        int w = zero_cc_expr_int_info(c, arena, oref, &us);
        int64_t v = onode->int_val;
        if (w < 8) {
            if (us) v = (int64_t)(uint64_t)(uint32_t)(int32_t)v;  /* zero-extend 32 */
            else     v = (int64_t)(int32_t)(int32_t)v;             /* sign-extend 32 */
        }
        int d = zc_alloc_temp(c);
        ir_emit_mov64_imm(c, d, v);
        return d;
    }
    int t = lower_expr(c, arena, oref);
    if (c->last_expr_is_64) return t;  /* already a GPR64 temp */
    int us = 0;
    zero_cc_expr_int_info(c, arena, oref, &us);
    int d = zc_alloc_temp(c);
    if (us) ir_emit_zext32_64(c, d, t);
    else    ir_emit_sext32_64(c, d, t);
    return d;
}

/* Sprint 170 (rv32 i64): lower a 64-bit integer binop (add/sub/mul, bitwise
 * and/or/xor, shifts, ordered/equality compares, div/rem) on the (lo,hi)
 * GPR64 pair. Returns the result GPR64 temp, or TEMP_NONE when the op is not
 * a supported 64-bit integer op (pointer/float/logical fall through to the
 * 32-bit path). The result is a GPR64 temp and sets last_expr_is_64. */
int zc_lower_binop64(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    uint32_t op = node->aux.op;
    bool is_arith = (op == TK_PLUS || op == TK_MINUS || op == TK_STAR);
    bool is_logic = (op == TK_AMP || op == TK_PIPE || op == TK_CARET);
    bool is_shift = (op == TK_SHL || op == TK_SHR);
    bool is_cmp   = (op == TK_EQ || op == TK_NE || op == TK_LT ||
                     op == TK_LE || op == TK_GT || op == TK_GE);
    bool is_divrem = (op == TK_SLASH || op == TK_PERCENT);
    if (!is_arith && !is_logic && !is_shift && !is_cmp && !is_divrem) return TEMP_NONE;

    /* A float operand is not a 64-bit integer op. */
    ZeroCCASTNode *ln = zero_cc_ast_get(arena, node->left);
    ZeroCCASTNode *rn = zero_cc_ast_get(arena, node->right);
    if (ln && (ln->tag == NODE_FLOAT)) return TEMP_NONE;
    if (rn && (rn->tag == NODE_FLOAT)) return TEMP_NONE;

    int l, r;
    if (is_shift) {
        /* value operand -> 64-bit; count operand -> 32-bit (trunc if 64). */
        l = zc_operand_to64(c, arena, node->left);
        r = lower_expr(c, arena, node->right);
        if (c->last_expr_is_64) {
            int t = zc_alloc_temp(c);
            ir_emit_trunc64_32(c, t, r);
            r = t;
        }
    } else {
        l = zc_operand_to64(c, arena, node->left);
        r = zc_operand_to64(c, arena, node->right);
    }

    int result_temp = zc_alloc_temp(c);
    if (is_arith) {
        if (op == TK_PLUS)      ir_emit_add64(c, result_temp, l, r);
        else if (op == TK_MINUS) ir_emit_sub64(c, result_temp, l, r);
        else                     ir_emit_mul64(c, result_temp, l, r);
    } else if (is_logic) {
        if (op == TK_AMP)      ir_emit_and64(c, result_temp, l, r);
        else if (op == TK_PIPE) ir_emit_or64(c, result_temp, l, r);
        else                    ir_emit_xor64(c, result_temp, l, r);
    } else if (is_shift) {
        if (op == TK_SHL) ir_emit_shl64(c, result_temp, l, r);
        else {
            /* Signed >> is an arithmetic shift (C: gcc defines it so). */
            int lu = 0;
            zero_cc_expr_int_info(c, arena, node->left, &lu);
            if (lu) ir_emit_shr64(c, result_temp, l, r);  /* unsigned: logical */
            else    ir_emit_sar64(c, result_temp, l, r);  /* signed: arithmetic */
        }
    } else if (is_divrem) {
        /* Sprint 173 (rv32 i64 Phase 3): 64-bit / and % call libgcc's
         * __{u,}divdi3/__{u,}moddi3 (the backend emits the call; the op
         * clobbers caller-saved registers - see live_range.h). The usual
         * arithmetic conversions decide signed/unsigned (same rule as the
         * compare path). */
        bool u64 = zero_cc_binop_is_unsigned(c, arena, node->left, node->right);
        if (op == TK_SLASH) {
            if (u64) ir_emit_udiv64(c, result_temp, l, r);
            else     ir_emit_div64(c, result_temp, l, r);
        } else {
            if (u64) ir_emit_umod64(c, result_temp, l, r);
            else     ir_emit_mod64(c, result_temp, l, r);
        }
    } else { /* is_cmp */
        int cond;
        switch (op) {
            case TK_EQ: cond = CC_EQ; break;
            case TK_NE: cond = CC_NE; break;
            case TK_LT: cond = CC_LT; break;
            case TK_LE: cond = CC_LE; break;
            case TK_GT: cond = CC_GT; break;
            default:    cond = CC_GE; break;
        }
        if (cond >= CC_LT && cond <= CC_GE &&
            zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) {
            cond += 6;  /* signed -> unsigned variant (matches the 32-bit path) */
        }
        ir_emit_cmp64(c, l, r);
        ir_emit_cond_set(c, result_temp, cond);
        c->last_expr_is_64 = 0;  /* the result is a 0/1 boolean, not 64-bit */
        return result_temp;
    }
    c->last_expr_is_64 = 1;
    return result_temp;
}

/* zero_san v1 (-fsanitize=undefined): the signed-integer-overflow check
 * width. The check width is the width of the CONVERSION result (C99 6.3.1.8
 * usual arithmetic conversions) — the op computes in 64-bit registers on
 * sign-extended operands, so the narrower operand is promoted: checking
 * at the narrower operand's width would trap on valid mixed-width code
 * (an `int + long` sum that fits in `long` but not in `int`). When the
 * widths are equal the op is signed only if BOTH operands are signed
 * (mixed equal-width converts to unsigned -> defined wrap, no check).
 * Only widths the register model expresses (4, 8) are checked; exotic
 * widths (bit-fields, sub-byte, __int128) are skipped, not mis-checked.
 * Pointer operands are excluded (pointer arithmetic has no signed-overflow
 * semantics here). A *decayed* array in pointer arithmetic (arr + 1) reads
 * as unsigned-8 via int_info and is likewise skipped; but an array-ELEMENT
 * lvalue (arr[i], a[i][j], p[i]) carries the element TypeInfo and IS checked
 * at the element's width + signedness (Sprint 161).
 * Shared by the binary-op, compound-assignment, and ++/-- emitters so the
 * width rule has one source of truth (Sprint 161). Returns 0 when no check
 * applies. */
static int zc_ovf_uac_width(int lw, int lu, int rw, int ru) {
    int w;
    if (lw > rw)      w = (lu == 0) ? lw : 0;
    else if (rw > lw) w = (ru == 0) ? rw : 0;
    else              w = (lu == 0 && ru == 0) ? lw : 0;
    return (w == 4 || w == 8) ? w : 0;
}

static void zc_emit_ovf_trap(ZeroCCCompiler *c, uint32_t result_temp,
                             uint32_t a_temp, uint32_t b_temp, int w, int op) {
    if (w != 4 && w != 8) return;
    if (c->isa == ISA_X86_64) {
        /* x86-64: ADD/SUB/MUL read the CPU OF flag (width 8) / the
         * sign-extension round-trip of the result (width 4); the operands
         * are not needed, so keep the trap operand-less (no extra
         * liveness). The op code still rides in .imm: DIV_ZERO/REM_ZERO/
         * SHIFT_COUNT are register tests, not OF/round-trip checks, and
         * the emitters dispatch on it (Sprint 166 follow-on). */
        ir_emit(c, (Instr){.op = OP_OVF_TRAP, .temp_dest = result_temp,
            .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE,
            .imm = (int16_t)op,
            .label_idx = 0, .width = (uint8_t)w});
    } else {
        /* aarch64 / riscv64 / riscv32: compare-based check needs the two
         * operands (clean sign-extended, dead after this check) + the op.
         * Carrying them as source operands makes liveness keep a/b live to
         * here, so the emitter can read them. */
        if (c->isa == ISA_RISCV32 && w == 8) return;  /* rv32 64-bit: follow-up */
        ir_emit(c, (Instr){.op = OP_OVF_TRAP, .temp_dest = result_temp,
            .temp_src1 = a_temp, .temp_src2 = b_temp, .imm = (int16_t)op,
            .label_idx = 0, .width = (uint8_t)w});
    }
}

static bool zc_node_is_pointer(ZeroCCCompiler *c, const ZeroCCASTNode *n) {
    if (!n) return false;
    TypeInfo *ti = ast_node_get_typeinfo(c, n);
    return ti && type_is_pointer(ti);
}

/* zero_san v1 (-fsanitize=undefined): emit a signed-integer-overflow trap
 * immediately after the arithmetic of a signed binary `+ - *` OR a compound
 * assignment `+= -= *=` (both carry the lvalue in node->left and the RHS in
 * node->right, so the UAC width rule is identical). x86-64 only in v1. See
 * the Sprint 116/161 invariants in ir-and-regalloc.md / sanitizer-roadmap.md. */
void zc_maybe_emit_ovf_trap(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                            ZeroCCASTNode *node, uint32_t result_temp,
                            int a_temp, int b_temp, int op) {
    if (!c->sanitize_overflow) return;
    if (zc_node_is_pointer(c, zero_cc_ast_get(arena, node->left)) ||
        zc_node_is_pointer(c, zero_cc_ast_get(arena, node->right))) return;
    int lu = 0, ru = 0;
    int lw = zero_cc_expr_int_info(c, arena, node->left, &lu);
    int rw = zero_cc_expr_int_info(c, arena, node->right, &ru);
    zc_emit_ovf_trap(c, result_temp, (uint32_t)a_temp, (uint32_t)b_temp,
                     zc_ovf_uac_width(lw, lu, rw, ru), op);
}

/* zero_san v1: the ++/-- form — the op is `lvalue + 1` (or - 1); the literal
 * 1 is a signed int, so the check width is the lvalue's promoted width when
 * it is signed (unsigned lvalues wrap by definition; pointer ++/-- are not
 * checked). node is the NODE_UNOP; node->left is the lvalue. */
void zc_maybe_emit_ovf_trap_incdec(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                   ZeroCCASTNode *node, uint32_t result_temp,
                                   int a_temp, int b_temp) {
    if (!c->sanitize_overflow) return;
    if (zc_node_is_pointer(c, zero_cc_ast_get(arena, node->left))) return;
    int lu = 0;
    int lw = zero_cc_expr_int_info(c, arena, node->left, &lu);
    /* inc/dec lower to `lvalue + (±step)` (a signed-int step) => ADD. */
    zc_emit_ovf_trap(c, result_temp, (uint32_t)a_temp, (uint32_t)b_temp,
                     zc_ovf_uac_width(lw, lu, 4, 0), ZC_OVF_OP_ADD);
}

/* zero_san follow-on (Sprint 166): div-by-zero check. Emits OP_OVF_TRAP with
 * op = ZC_OVF_OP_DIV_ZERO, placed BEFORE the div (x86 idiv would raise
 * SIGFPE first). temp_dest carries the divisor temp (src1 is TEMP_NONE), so
 * the emitter tests it; the div is the temp's last real use, keeping it in
 * its register across the check. No-op when the width is not 4/8 or
 * sanitize_overflow is off. */
void zc_maybe_emit_div_zero_trap(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                 ZeroCCASTNode *node, uint32_t divisor_temp) {
    if (!c->sanitize_overflow) return;
    int lu = 0, ru = 0;
    int lw = zero_cc_expr_int_info(c, arena, node->left, &lu);
    int rw = zero_cc_expr_int_info(c, arena, node->right, &ru);
    int w;
    if (lw > rw)      w = (lu == 0) ? lw : 0;
    else if (rw > lw) w = (ru == 0) ? rw : 0;
    else              w = (lu == 0 && ru == 0) ? lw : 0;
    if (w != 4 && w != 8) return;
    zc_emit_ovf_trap(c, divisor_temp, TEMP_NONE, TEMP_NONE, w, ZC_OVF_OP_DIV_ZERO);
}

/* zero_san follow-on (Sprint 166): remainder-by-zero check. Same as
 * div-zero but for the modulo operator (op = ZC_OVF_OP_REM_ZERO). */
void zc_maybe_emit_rem_zero_trap(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                 ZeroCCASTNode *node, uint32_t divisor_temp) {
    if (!c->sanitize_overflow) return;
    int lu = 0, ru = 0;
    int lw = zero_cc_expr_int_info(c, arena, node->left, &lu);
    int rw = zero_cc_expr_int_info(c, arena, node->right, &ru);
    int w;
    if (lw > rw)      w = (lu == 0) ? lw : 0;
    else if (rw > lw) w = (ru == 0) ? rw : 0;
    else              w = (lu == 0 && ru == 0) ? lw : 0;
    if (w != 4 && w != 8) return;
    zc_emit_ovf_trap(c, divisor_temp, TEMP_NONE, TEMP_NONE, w, ZC_OVF_OP_REM_ZERO);
}

/* zero_san follow-on (Sprint 166): shift-count check, placed BEFORE the
 * shift (see div-zero above for the liveness rationale). Traps if the shift
 * count is < 0 or >= the operand bit width. temp_dest carries the count
 * temp (src1 is TEMP_NONE). No-op when the width is not 4/8 or
 * sanitize_overflow is off. */
void zc_maybe_emit_shift_count_trap(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                     ZeroCCASTNode *node, uint32_t count_temp) {
    if (!c->sanitize_overflow) return;
    int lu = 0, ru = 0;
    int lw = zero_cc_expr_int_info(c, arena, node->left, &lu);
    int rw = zero_cc_expr_int_info(c, arena, node->right, &ru);
    int w;
    if (lw > rw)      w = (lu == 0) ? lw : 0;
    else if (rw > lw) w = (ru == 0) ? rw : 0;
    else              w = (lu == 0 && ru == 0) ? lw : 0;
    if (w != 4 && w != 8) return;
    zc_emit_ovf_trap(c, count_temp, TEMP_NONE, TEMP_NONE, w, ZC_OVF_OP_SHIFT_COUNT);
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
            /* GNU __int128 (Sprint 109): an operand that is a 128-bit integer
             * (and this is not a complex op) is lowered on the (lo,hi) GPR128
             * pair. Complex is also 16 bytes, so exclude it explicitly. */
            {
                bool l128 = zc_node_is_int128(c, cl_node);
                bool r128 = zc_node_is_int128(c, cr_node);
                if ((l128 || r128) &&
                    !(cres_ti && cres_ti->kind == TYPE_KIND_COMPLEX) &&
                    !(cl_ti && cl_ti->kind == TYPE_KIND_COMPLEX) &&
                    !(cr_ti && cr_ti->kind == TYPE_KIND_COMPLEX)) {
                    int r128 = zc_lower_binop128(c, arena, node, cl_node, cr_node, cl_ti, cr_ti);
                    if (r128 != TEMP_NONE) { c->last_expr_is_float = 0; return r128; }
                }
            }
            /* Sprint 170 (rv32 i64): a 64-bit integer operand (and not a
             * complex/128 op) lowers on the (lo,hi) GPR64 pair. */
            if (c->isa == ISA_RISCV32 &&
                !((cres_ti && cres_ti->kind == TYPE_KIND_COMPLEX) ||
                  (cl_ti && cl_ti->kind == TYPE_KIND_COMPLEX) ||
                  (cr_ti && cr_ti->kind == TYPE_KIND_COMPLEX))) {
                if (zc_node_is_int64(c, cl_node) || zc_node_is_int64(c, cr_node)) {
                    int r64 = zc_lower_binop64(c, arena, node);
                    if (r64 != TEMP_NONE) { c->last_expr_is_float = 0; return r64; }
                }
            }
            bool is_cx_eqne = (node->aux.op == TK_EQ || node->aux.op == TK_NE) &&
                              cl_ti && cr_ti &&
                              (cl_ti->kind == TYPE_KIND_COMPLEX || cr_ti->kind == TYPE_KIND_COMPLEX);
            if ((cres_ti && cres_ti->kind == TYPE_KIND_COMPLEX) || is_cx_eqne) {
                const TypeInfo *cel = (cres_ti && cres_ti->kind == TYPE_KIND_COMPLEX) ? cres_ti->details.array.base :
                                      (cl_ti && cl_ti->kind == TYPE_KIND_COMPLEX) ? cl_ti->details.array.base :
                                      (cr_ti && cr_ti->kind == TYPE_KIND_COMPLEX) ? cr_ti->details.array.base : NULL;
                const ZeroCCType *czt = NULL;
                if (!cel) {
                    if (cl_node && cl_node->tag == NODE_IDENT) {
                        ZeroCCSym *s = zero_cc_find_local(c, cl_node->s.str);
                        if (s && zero_cc_type_is_complex(&s->type)) czt = s->type.base;
                        else {
                            ZeroCCGlobalVar *g = find_global_var(c, cl_node->s.str);
                            if (g && zero_cc_type_is_complex(&g->type)) czt = g->type.base;
                        }
                    }
                    if (!czt && cr_node && cr_node->tag == NODE_IDENT) {
                        ZeroCCSym *s = zero_cc_find_local(c, cr_node->s.str);
                        if (s && zero_cc_type_is_complex(&s->type)) czt = s->type.base;
                        else {
                            ZeroCCGlobalVar *g = find_global_var(c, cr_node->s.str);
                            if (g && zero_cc_type_is_complex(&g->type)) czt = g->type.base;
                        }
                    }
                }
                uint32_t cop = node->aux.op;
                c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + 16 + 7) & ~7;
                int32_t croff = c->current_stack_frame;
                int clv = lower_expr(c, arena, node->left);
                bool left_cx = (cl_ti && cl_ti->kind == TYPE_KIND_COMPLEX);
                bool left_is_float = c->last_expr_is_float;
                int crv = lower_expr(c, arena, node->right);
                bool right_cx = (cr_ti && cr_ti->kind == TYPE_KIND_COMPLEX);
                bool right_is_float = c->last_expr_is_float;

                const TypeInfo *l_el = (left_cx && cl_ti && cl_ti->details.array.base) ? cl_ti->details.array.base : NULL;
                const TypeInfo *r_el = (right_cx && cr_ti && cr_ti->details.array.base) ? cr_ti->details.array.base : NULL;
                const ZeroCCType *l_zt = NULL, *r_zt = NULL;
                if (!l_el && cl_node && cl_node->tag == NODE_IDENT) {
                    ZeroCCSym *s = zero_cc_find_local(c, cl_node->s.str);
                    if (s && zero_cc_type_is_complex(&s->type)) { l_zt = s->type.base; left_cx = true; }
                    else {
                        ZeroCCGlobalVar *g = find_global_var(c, cl_node->s.str);
                        if (g && zero_cc_type_is_complex(&g->type)) { l_zt = g->type.base; left_cx = true; }
                    }
                }
                if (!r_el && cr_node && cr_node->tag == NODE_IDENT) {
                    ZeroCCSym *s = zero_cc_find_local(c, cr_node->s.str);
                    if (s && zero_cc_type_is_complex(&s->type)) { r_zt = s->type.base; right_cx = true; }
                    else {
                        ZeroCCGlobalVar *g = find_global_var(c, cr_node->s.str);
                        if (g && zero_cc_type_is_complex(&g->type)) { r_zt = g->type.base; right_cx = true; }
                    }
                }
                bool l_is_fp = left_cx ? (l_el ? type_is_floating(l_el) : (l_zt ? (l_zt->tag == TYPE_FLOAT || l_zt->tag == TYPE_DOUBLE) : true)) :
                               (cl_ti ? type_is_floating(cl_ti) : left_is_float);
                size_t l_esz = left_cx ? (l_el ? type_size(l_el) : (l_zt ? l_zt->size : 8)) :
                               (cl_ti ? type_size(cl_ti) :
                                (cl_node && cl_node->tag == NODE_FLOAT ? ((cl_node->flags & 1) && !(cl_node->flags & 0xC0) ? 4 : 8) :
                                 (left_is_float ? 8 : 4)));
                if (l_esz == 0) l_esz = 8;
                if (l_is_fp && l_esz != 4 && l_esz != 8) l_esz = 8;

                bool r_is_fp = right_cx ? (r_el ? type_is_floating(r_el) : (r_zt ? (r_zt->tag == TYPE_FLOAT || r_zt->tag == TYPE_DOUBLE) : true)) :
                               (cr_ti ? type_is_floating(cr_ti) : right_is_float);
                size_t r_esz = right_cx ? (r_el ? type_size(r_el) : (r_zt ? r_zt->size : 8)) :
                               (cr_ti ? type_size(cr_ti) :
                                (cr_node && cr_node->tag == NODE_FLOAT ? ((cr_node->flags & 1) && !(cr_node->flags & 0xC0) ? 4 : 8) :
                                 (right_is_float ? 8 : 4)));
                if (r_esz == 0) r_esz = 8;
                if (r_is_fp && r_esz != 4 && r_esz != 8) r_esz = 8;

                bool cflt;
                size_t cesz;
                if (cel) {
                    cflt = type_is_floating(cel);
                    cesz = type_size(cel);
                    if (cflt && cesz != 4 && cesz != 8) cesz = 8;
                    if (!cflt && cesz != 1 && cesz != 2 && cesz != 4 && cesz != 8) cesz = 4;
                } else if (czt) {
                    cflt = (czt->tag == TYPE_FLOAT || czt->tag == TYPE_DOUBLE);
                    cesz = czt->size;
                    if (cflt && cesz != 4 && cesz != 8) cesz = 8;
                    if (!cflt && cesz != 1 && cesz != 2 && cesz != 4 && cesz != 8) cesz = 4;
                } else {
                    cflt = l_is_fp || r_is_fp;
                    cesz = (cflt) ? ((l_is_fp && l_esz == 8) || (r_is_fp && r_esz == 8) ? 8 : 4) :
                                    ((l_esz > r_esz) ? l_esz : r_esz);
                    if (cflt && cesz != 4 && cesz != 8) cesz = 8;
                    if (!cflt && cesz != 1 && cesz != 2 && cesz != 4 && cesz != 8) cesz = 4;
                }

                int zero_f_off = zc_rodata_double_offset(c, 0.0);
                int ar = zc_alloc_temp(c), ai = zc_alloc_temp(c);
                if (left_cx) {
                    if (l_is_fp) {
                        if (l_esz == 4) {
                            ir_emit_vloadss(c, ar, (uint32_t)clv);
                            int a2 = zc_alloc_temp(c); ir_emit_mov_imm(c, a2, 4);
                            int ai2 = zc_alloc_temp(c); ir_emit_add(c, ai2, clv, a2);
                            ir_emit_vloadss(c, ai, (uint32_t)ai2);
                        } else {
                            ir_emit_vloadsd(c, ar, (uint32_t)clv);
                            int a2 = zc_alloc_temp(c); ir_emit_mov_imm(c, a2, 8);
                            int ai2 = zc_alloc_temp(c); ir_emit_add(c, ai2, clv, a2);
                            ir_emit_vloadsd(c, ai, (uint32_t)ai2);
                        }
                    } else {
                        int a2 = zc_alloc_temp(c); ir_emit_mov_imm(c, a2, (int)l_esz);
                        int ai2 = zc_alloc_temp(c); ir_emit_add(c, ai2, clv, a2);
                        if (l_esz == 1) { ir_emit_load_byte(c, ar, clv); ir_emit_load_byte(c, ai, ai2); }
                        else if (l_esz == 2) { ir_emit_load_16(c, ar, clv); ir_emit_load_16(c, ai, ai2); }
                        else if (l_esz == 4) { ir_emit_load_32(c, ar, clv); ir_emit_load_32(c, ai, ai2); }
                        else { ir_emit_deref(c, ar, clv); ir_emit_deref(c, ai, ai2); }
                        if (l_el) { lower_extend_narrow(c, ar, l_el); lower_extend_narrow(c, ai, l_el); }
                        else if (l_zt) { lower_assign_conv_zt(c, ar, l_zt); lower_assign_conv_zt(c, ai, l_zt); }
                        else if (l_esz < 8) { ir_emit_sext(c, ar, (int)l_esz); ir_emit_sext(c, ai, (int)l_esz); }
                    }
                } else {
                    if (left_is_float) {
                        ir_emit_fmov(c, ar, (uint32_t)clv);
                        ir_emit_fmov_imm(c, (uint32_t)ai, (int64_t)zero_f_off);
                    } else {
                        ar = clv;
                        ir_emit_mov_imm(c, ai, 0);
                    }
                }

                if (cflt && !l_is_fp) {
                    int far = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)far, (uint32_t)ar); ar = far;
                    int fai = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)fai, (uint32_t)ai); ai = fai;
                } else if (!cflt && l_is_fp) {
                    int iar = zc_alloc_temp(c);
                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = iar, .temp_src1 = ar, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    ar = iar;
                    int iai = zc_alloc_temp(c);
                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = iai, .temp_src1 = ai, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    ai = iai;
                }

                int br = zc_alloc_temp(c), bi = zc_alloc_temp(c);
                if (right_cx) {
                    if (r_is_fp) {
                        if (r_esz == 4) {
                            ir_emit_vloadss(c, br, (uint32_t)crv);
                            int a2 = zc_alloc_temp(c); ir_emit_mov_imm(c, a2, 4);
                            int bi2 = zc_alloc_temp(c); ir_emit_add(c, bi2, crv, a2);
                            ir_emit_vloadss(c, bi, (uint32_t)bi2);
                        } else {
                            ir_emit_vloadsd(c, br, (uint32_t)crv);
                            int a2 = zc_alloc_temp(c); ir_emit_mov_imm(c, a2, 8);
                            int bi2 = zc_alloc_temp(c); ir_emit_add(c, bi2, crv, a2);
                            ir_emit_vloadsd(c, bi, (uint32_t)bi2);
                        }
                    } else {
                        int a2 = zc_alloc_temp(c); ir_emit_mov_imm(c, a2, (int)r_esz);
                        int bi2 = zc_alloc_temp(c); ir_emit_add(c, bi2, crv, a2);
                        if (r_esz == 1) { ir_emit_load_byte(c, br, crv); ir_emit_load_byte(c, bi, bi2); }
                        else if (r_esz == 2) { ir_emit_load_16(c, br, crv); ir_emit_load_16(c, bi, bi2); }
                        else if (r_esz == 4) { ir_emit_load_32(c, br, crv); ir_emit_load_32(c, bi, bi2); }
                        else { ir_emit_deref(c, br, crv); ir_emit_deref(c, bi, bi2); }
                        if (r_el) { lower_extend_narrow(c, br, r_el); lower_extend_narrow(c, bi, r_el); }
                        else if (r_zt) { lower_assign_conv_zt(c, br, r_zt); lower_assign_conv_zt(c, bi, r_zt); }
                        else if (r_esz < 8) { ir_emit_sext(c, br, (int)r_esz); ir_emit_sext(c, bi, (int)r_esz); }
                    }
                } else {
                    if (right_is_float) {
                        ir_emit_fmov(c, br, (uint32_t)crv);
                        ir_emit_fmov_imm(c, (uint32_t)bi, (int64_t)zero_f_off);
                    } else {
                        br = crv;
                        ir_emit_mov_imm(c, bi, 0);
                    }
                }

                if (cflt && !r_is_fp) {
                    int fbr = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)fbr, (uint32_t)br); br = fbr;
                    int fbi = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)fbi, (uint32_t)bi); bi = fbi;
                } else if (!cflt && r_is_fp) {
                    int ibr = zc_alloc_temp(c);
                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = ibr, .temp_src1 = br, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    br = ibr;
                    int ibi = zc_alloc_temp(c);
                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = ibi, .temp_src1 = bi, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    bi = ibi;
                }
                int rr = zc_alloc_temp(c), ri = zc_alloc_temp(c);
                if (cflt) {
                    switch (cop) {
                        case TK_PLUS: {
                            ir_emit_fadd(c, rr, ar, br);
                            ir_emit_fadd(c, ri, ai, bi);
                            break;
                        }
                        case TK_MINUS: {
                            ir_emit_fsub(c, rr, ar, br);
                            ir_emit_fsub(c, ri, ai, bi);
                            break;
                        }
                        case TK_STAR: {
                            int t1 = zc_alloc_temp(c); ir_emit_fmul(c, t1, ar, br);
                            int t2 = zc_alloc_temp(c); ir_emit_fmul(c, t2, ai, bi);
                            int t3 = zc_alloc_temp(c); ir_emit_fmul(c, t3, ar, bi);
                            int t4 = zc_alloc_temp(c); ir_emit_fmul(c, t4, ai, br);
                            ir_emit_fsub(c, rr, t1, t2);
                            ir_emit_fadd(c, ri, t3, t4);
                            break;
                        }
                        case TK_SLASH: {
                            if (cesz == 8) {
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
                                    ir_emit_fstore(c, (uint32_t)div_vals[h], TEMP_NONE, -slot);
                                    div_args[h] = zc_alloc_temp(c);
                                    ir_emit_fload(c, (uint32_t)div_args[h], TEMP_NONE, -slot);
                                }
                                ir_emit_call(c, "__divdc3");
                                if (ir_get_count(c) > 0) {
                                    Instr *last_instr = (Instr*)&ir_get_buffer(c)[ir_get_count(c) - 1];
                                    last_instr->temp_dest = 4;
                                    last_instr->temp_src1 = (uint32_t)div_args[0];
                                    last_instr->temp_src2 = (uint32_t)div_args[1];
                                    last_instr->imm = IR_CALL_ARG01_REG;
                                    last_instr->cond = 1;
                                    last_instr->width = 1;
                                    last_instr->label_idx = 4;
                                    ir_emit_call_arg(c, (uint32_t)div_args[2], 1, 255);
                                    ir_emit_call_arg(c, (uint32_t)div_args[3], 1, 255);
                                    if (c->lowering.ir_ctx.call_count < 64) {
                                        int cidx = c->lowering.ir_ctx.call_count++;
                                        c->lowering.ir_ctx.call_fp_spill_counts[cidx] = 4;
                                        for (int h = 0; h < 4; h++) {
                                            c->lowering.ir_ctx.call_fp_spills[cidx][h] =
                                                c->current_stack_frame - (3 - h) * 8;
                                        }
                                    }
                                }
                                c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                                c->current_stack_frame += 16;
                                int32_t dres_off = c->current_stack_frame;
                                if (ir_get_count(c) > 0) {
                                    Instr *li = (Instr*)&ir_get_buffer(c)[ir_get_count(c) - 1];
                                    li->label_idx = 0x0100 | (16 & 0xFF);
                                }
                                int daddr = zc_alloc_temp(c);
                                ir_emit_lea(c, daddr, BE(c)->fp_reg, -dres_off);
                                ir_emit_vstoresd(c, (uint32_t)daddr, (uint32_t)VREG_F0);
                                int do2 = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, do2, 8);
                                int da2 = zc_alloc_temp(c);
                                ir_emit_add(c, da2, daddr, do2);
                                ir_emit_vstoresd(c, (uint32_t)da2, (uint32_t)VREG_F1);
                                c->last_expr_is_float = 0;
                                return daddr;
                            }
                            int fab_br = zc_cx_fabs(c, br);
                            int fab_bi = zc_cx_fabs(c, bi);
                            int ge = zc_alloc_temp(c);
                            ir_emit_cmp(c, fab_br, fab_bi);
                            ir_emit_cond_set(c, ge, CC_GE);
                            int qa1 = zc_alloc_temp(c); ir_emit_fdiv(c, qa1, ai, br);
                            int qa1b = zc_alloc_temp(c); ir_emit_fmul(c, qa1b, qa1, bi);
                            int reA = zc_alloc_temp(c); ir_emit_fadd(c, reA, ar, qa1b);
                            int qa0 = zc_alloc_temp(c); ir_emit_fdiv(c, qa0, ar, br);
                            int qa0b = zc_alloc_temp(c); ir_emit_fmul(c, qa0b, qa0, bi);
                            int imA = zc_alloc_temp(c); ir_emit_fsub(c, imA, ai, qa0b);
                            int da_m = zc_alloc_temp(c); ir_emit_fdiv(c, da_m, bi, br);
                            int da_m2 = zc_alloc_temp(c); ir_emit_fmul(c, da_m2, da_m, bi);
                            int dA = zc_alloc_temp(c); ir_emit_fadd(c, dA, br, da_m2);
                            int qb0a = zc_alloc_temp(c); ir_emit_fdiv(c, qb0a, ar, bi);
                            int qb0ab = zc_alloc_temp(c); ir_emit_fmul(c, qb0ab, qb0a, br);
                            int reB = zc_alloc_temp(c); ir_emit_fadd(c, reB, ai, qb0ab);
                            int qb0b = zc_alloc_temp(c); ir_emit_fdiv(c, qb0b, ai, bi);
                            int qb0bb = zc_alloc_temp(c); ir_emit_fmul(c, qb0bb, qb0b, br);
                            int imB = zc_alloc_temp(c); ir_emit_fsub(c, imB, qb0bb, ar);
                            int db_m = zc_alloc_temp(c); ir_emit_fdiv(c, db_m, br, bi);
                            int db_m2 = zc_alloc_temp(c); ir_emit_fmul(c, db_m2, db_m, br);
                            int dB = zc_alloc_temp(c); ir_emit_fadd(c, dB, bi, db_m2);
                            int num_r = zc_cx_fselect(c, ge, reA, reB);
                            int num_i = zc_cx_fselect(c, ge, imA, imB);
                            int den = zc_cx_fselect(c, ge, dA, dB);
                            ir_emit_fdiv(c, rr, num_r, den);
                            ir_emit_fdiv(c, ri, num_i, den);
                            break;
                        }
                        case TK_EQ:
                        case TK_NE: {
                            int eqr = zc_alloc_temp(c), eqi = zc_alloc_temp(c);
                            ir_emit_cmp(c, ar, br);
                            ir_emit_cond_set(c, eqr, CC_EQ);
                            ir_emit_cmp(c, ai, bi);
                            ir_emit_cond_set(c, eqi, CC_EQ);
                            int both = zc_alloc_temp(c);
                            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint32_t)both,
                                .temp_src1=(uint32_t)eqr,.temp_src2=(uint32_t)eqi});
                            if (cop == TK_NE) {
                                int one = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, one, 1);
                                int nz = zc_alloc_temp(c);
                                ir_emit(c, (Instr){.op=OP_XOR,.temp_dest=(uint32_t)nz,
                                    .temp_src1=(uint32_t)both,.temp_src2=(uint32_t)one});
                                rr = nz;
                            } else {
                                rr = both;
                            }
                            ir_emit_mov_imm(c, ri, 0);
                            c->last_expr_is_float = 0;
                            return rr;
                        }
                        default:
                            ir_emit_fmov(c, rr, ar);
                            ir_emit_fmov(c, ri, ai);
                            break;
                    }
                } else {
                    switch (cop) {
                        case TK_PLUS:
                            ir_emit_add(c, rr, ar, br);
                            ir_emit_add(c, ri, ai, bi);
                            break;
                        case TK_MINUS:
                            ir_emit_sub(c, rr, ar, br);
                            ir_emit_sub(c, ri, ai, bi);
                            break;
                        case TK_STAR: {
                            int t1 = zc_alloc_temp(c); ir_emit_mul(c, t1, ar, br);
                            int t2 = zc_alloc_temp(c); ir_emit_mul(c, t2, ai, bi);
                            int t3 = zc_alloc_temp(c); ir_emit_mul(c, t3, ar, bi);
                            int t4 = zc_alloc_temp(c); ir_emit_mul(c, t4, ai, br);
                            ir_emit_sub(c, rr, t1, t2);
                            ir_emit_add(c, ri, t3, t4);
                            break;
                        }
                        case TK_SLASH: {
                            bool is_u = cel && (cel->modifiers & TYPE_MOD_UNSIGNED);
                            int d1 = zc_alloc_temp(c); ir_emit_mul(c, d1, br, br);
                            int d2 = zc_alloc_temp(c); ir_emit_mul(c, d2, bi, bi);
                            int den = zc_alloc_temp(c); ir_emit_add(c, den, d1, d2);

                            int nr1 = zc_alloc_temp(c); ir_emit_mul(c, nr1, ar, br);
                            int nr2 = zc_alloc_temp(c); ir_emit_mul(c, nr2, ai, bi);
                            int num_r = zc_alloc_temp(c); ir_emit_add(c, num_r, nr1, nr2);

                            int ni1 = zc_alloc_temp(c); ir_emit_mul(c, ni1, ai, br);
                            int ni2 = zc_alloc_temp(c); ir_emit_mul(c, ni2, ar, bi);
                            int num_i = zc_alloc_temp(c); ir_emit_sub(c, num_i, ni1, ni2);

                            ir_emit_div(c, rr, num_r, den);
                            if (is_u) ir_mark_last_unsigned(c, 1);
                            ir_emit_div(c, ri, num_i, den);
                            if (is_u) ir_mark_last_unsigned(c, 1);
                            break;
                        }
                        case TK_EQ:
                        case TK_NE: {
                            int eqr = zc_alloc_temp(c), eqi = zc_alloc_temp(c);
                            ir_emit_cmp(c, ar, br);
                            ir_emit_cond_set(c, eqr, CC_EQ);
                            ir_emit_cmp(c, ai, bi);
                            ir_emit_cond_set(c, eqi, CC_EQ);
                            int both = zc_alloc_temp(c);
                            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint32_t)both,
                                .temp_src1=(uint32_t)eqr,.temp_src2=(uint32_t)eqi});
                            if (cop == TK_NE) {
                                int one = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, one, 1);
                                int nz = zc_alloc_temp(c);
                                ir_emit(c, (Instr){.op=OP_XOR,.temp_dest=(uint32_t)nz,
                                    .temp_src1=(uint32_t)both,.temp_src2=(uint32_t)one});
                                rr = nz;
                            } else {
                                rr = both;
                            }
                            ir_emit_mov_imm(c, ri, 0);
                            c->last_expr_is_float = 0;
                            return rr;
                        }
                        default:
                            ir_emit_mov_reg(c, rr, ar);
                            ir_emit_mov_reg(c, ri, ai);
                            break;
                    }
                }
                if (cflt) {
                    if (cesz == 4) {
                        int sa = zc_alloc_temp(c);
                        ir_emit_lea(c, sa, BE(c)->fp_reg, -croff);
                        ir_emit_vstoress(c, sa, (uint32_t)rr);
                        int sa2 = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, sa2, 4);
                        int sa3 = zc_alloc_temp(c);
                        ir_emit_add(c, sa3, sa, sa2);
                        ir_emit_vstoress(c, sa3, (uint32_t)ri);
                    } else {
                        ir_emit_fstore(c, (uint32_t)rr, TEMP_NONE, -croff);
                        ir_emit_fstore(c, (uint32_t)ri, TEMP_NONE, -(croff - (int)cesz));
                    }
                } else {
                    int sa = zc_alloc_temp(c);
                    ir_emit_lea(c, sa, BE(c)->fp_reg, -croff);
                    int sa2 = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, sa2, (int)cesz);
                    int sa3 = zc_alloc_temp(c);
                    ir_emit_add(c, sa3, sa, sa2);
                    if (cesz == 1) {
                        ir_emit_store_byte(c, rr, sa);
                        ir_emit_store_byte(c, ri, sa3);
                    } else if (cesz == 2) {
                        ir_emit_store_16(c, rr, sa);
                        ir_emit_store_16(c, ri, sa3);
                    } else if (cesz == 4) {
                        ir_emit_store_32(c, rr, sa);
                        ir_emit_store_32(c, ri, sa3);
                    } else {
                        ir_emit_store_64(c, rr, sa);
                        ir_emit_store_64(c, ri, sa3);
                    }
                }
                int addr = zc_alloc_temp(c);
                ir_emit_lea(c, addr, BE(c)->fp_reg, -croff);
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
                /* Sprint 113: 16-byte (128-bit) integer lanes are supported
                 * via the GPR128 op set (c-torture compile/pr70355, v2ti). */
                if (esz < 1 || esz > 16 || lanes < 1 || lanes > 32) {
                    diag_report_error_at_token(c, "unsupported vector operand size");
                    int zt = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, zt, 0);
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
                        if (esz == 4) { if (elem_unsigned) ir_emit_zext(c, left_temp, 4); else ir_emit_sext(c, left_temp, 4); }
                        else if (esz == 2) { if (elem_unsigned) ir_emit_zext(c, left_temp, 2); else ir_emit_sext(c, left_temp, 2); }
                        else if (esz == 1) { if (elem_unsigned) ir_emit_zext(c, left_temp, 1); else ir_emit_sext(c, left_temp, 1); }
                    }
                    if (!right_vec) {
                        if (esz == 4) { if (elem_unsigned) ir_emit_zext(c, right_temp, 4); else ir_emit_sext(c, right_temp, 4); }
                        else if (esz == 2) { if (elem_unsigned) ir_emit_zext(c, right_temp, 2); else ir_emit_sext(c, right_temp, 2); }
                        else if (esz == 1) { if (elem_unsigned) ir_emit_zext(c, right_temp, 1); else ir_emit_sext(c, right_temp, 1); }
                    }
                } else if (!elem_float && esz == 16) {
                    /* Broadcast a 64-bit scalar across 128-bit lanes: widen
                     * it to a GPR128 with the lane's signedness. The operand
                     * is already 128-bit when its own type is __int128. */
                    bool left_128 = (l_ti && l_ti->kind == TYPE_KIND_INT128);
                    bool right_128 = (r_ti && r_ti->kind == TYPE_KIND_INT128);
                    if (!left_vec && !left_128) {
                        int d = zc_alloc_temp(c);
                        if (elem_unsigned) ir_emit_zext64_128(c, d, left_temp);
                        else ir_emit_sext64_128(c, d, left_temp);
                        left_temp = d;
                    }
                    if (!right_vec && !right_128) {
                        int d = zc_alloc_temp(c);
                        if (elem_unsigned) ir_emit_zext64_128(c, d, right_temp);
                        else ir_emit_sext64_128(c, d, right_temp);
                        right_temp = d;
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
                            int o = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o, (int32_t)(i * esz));
                            a = zc_alloc_temp(c);
                            ir_emit_add(c, a, left_temp, o);
                        }
                        lv = zc_alloc_temp(c);
                        if (elem_float) {
                            if (esz == 4) ir_emit_vloadss(c, (uint32_t)lv, (uint32_t)a);
                            else ir_emit_vloadsd(c, (uint32_t)lv, (uint32_t)a);
                            lv_flt = 1;
                        } else if (esz == 1) {
                            ir_emit_load_byte(c, lv, a);
                            if (elem_unsigned) ir_emit_zext(c, lv, 1); else ir_emit_sext(c, lv, 1);
                        } else if (esz == 2) {
                            ir_emit_load_16(c, lv, a);
                            if (elem_unsigned) ir_emit_zext(c, lv, 2); else ir_emit_sext(c, lv, 2);
                        } else if (esz == 4) {
                            ir_emit_load_32(c, lv, a);
                            if (elem_unsigned) ir_emit_zext(c, lv, 4); else ir_emit_sext(c, lv, 4);
                        } else if (esz == 16) {
                            ir_emit_load128(c, lv, a);
                        } else {
                            ir_emit_deref(c, lv, a);
                        }
                    }
                    if (right_vec) {
                        int a = right_temp;
                        if (i > 0) {
                            int o = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o, (int32_t)(i * esz));
                            a = zc_alloc_temp(c);
                            ir_emit_add(c, a, right_temp, o);
                        }
                        rv = zc_alloc_temp(c);
                        if (elem_float) {
                            if (esz == 4) ir_emit_vloadss(c, (uint32_t)rv, (uint32_t)a);
                            else ir_emit_vloadsd(c, (uint32_t)rv, (uint32_t)a);
                            rv_flt = 1;
                        } else if (esz == 1) {
                            ir_emit_load_byte(c, rv, a);
                            if (elem_unsigned) ir_emit_zext(c, rv, 1); else ir_emit_sext(c, rv, 1);
                        } else if (esz == 2) {
                            ir_emit_load_16(c, rv, a);
                            if (elem_unsigned) ir_emit_zext(c, rv, 2); else ir_emit_sext(c, rv, 2);
                        } else if (esz == 4) {
                            ir_emit_load_32(c, rv, a);
                            if (elem_unsigned) ir_emit_zext(c, rv, 4); else ir_emit_sext(c, rv, 4);
                        } else if (esz == 16) {
                            ir_emit_load128(c, rv, a);
                        } else {
                            ir_emit_deref(c, rv, a);
                        }
                    }

                    int ov = zc_alloc_temp(c);
                    bool ov_flt = false;
                    if (is_cmp) {
                        if (esz == 16) {
                            /* Sprint 113: 128-bit lane comparison - same
                             * decomposition as the scalar __int128 path
                             * (hi first, lo unsigned tie-break). */
                            int lh = zc_alloc_temp(c); ir_emit_hi128(c, lh, lv);
                            int ll = zc_alloc_temp(c); ir_emit_lo128(c, ll, lv);
                            int rh = zc_alloc_temp(c); ir_emit_hi128(c, rh, rv);
                            int rl = zc_alloc_temp(c); ir_emit_lo128(c, rl, rv);
                            int hi_cc_lt = elem_unsigned ? CC_FLT_LT : CC_LT;
                            int hi_cc_gt = elem_unsigned ? CC_FLT_GT : CC_GT;
                            if (vop == TK_EQ) {
                                int eh = zc_cmp64_bool(c, lh, rh, CC_EQ);
                                int el = zc_cmp64_bool(c, ll, rl, CC_EQ);
                                ir_emit_and(c, ov, eh, el);
                            } else if (vop == TK_NE) {
                                int nh = zc_cmp64_bool(c, lh, rh, CC_NE);
                                int nl = zc_cmp64_bool(c, ll, rl, CC_NE);
                                ir_emit_or(c, ov, nh, nl);
                            } else if (vop == TK_LT || vop == TK_LE) {
                                int t1 = zc_cmp64_bool(c, lh, rh, hi_cc_lt);
                                int t2 = zc_cmp64_bool(c, lh, rh, CC_EQ);
                                int t3 = zc_cmp64_bool(c, ll, rl, vop == TK_LT ? CC_FLT_LT : CC_FLT_LE);
                                int t4 = zc_alloc_temp(c); ir_emit_and(c, t4, t2, t3);
                                ir_emit_or(c, ov, t1, t4);
                            } else {
                                int t1 = zc_cmp64_bool(c, lh, rh, hi_cc_gt);
                                int t2 = zc_cmp64_bool(c, lh, rh, CC_EQ);
                                int t3 = zc_cmp64_bool(c, ll, rl, vop == TK_GT ? CC_FLT_GT : CC_FLT_GE);
                                int t4 = zc_alloc_temp(c); ir_emit_and(c, t4, t2, t3);
                                ir_emit_or(c, ov, t1, t4);
                            }
                        } else if (elem_float) {
                            /* Double FP model: VLOADSS already widened the
                             * lane single to double; a double scalar with
                             * float lanes is first narrowed to the element
                             * type (C: scalar converts to the vector
                             * element type) then re-widened; int scalars
                             * widen with cvtsi2sd. */
                            if (esz == 4) {
                                if (!left_vec && left_flt) { int t = zc_alloc_temp(c); ir_emit_cvtsd2ss(c, (uint32_t)t, (uint32_t)lv); lv = t; int t2 = zc_alloc_temp(c); ir_emit_cvtss2sd(c, (uint32_t)t2, (uint32_t)lv); lv = t2; }
                                if (!right_vec && right_flt) { int t = zc_alloc_temp(c); ir_emit_cvtsd2ss(c, (uint32_t)t, (uint32_t)rv); rv = t; int t2 = zc_alloc_temp(c); ir_emit_cvtss2sd(c, (uint32_t)t2, (uint32_t)rv); rv = t2; }
                            }
                            if (!left_vec && !left_flt) { int t = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)t, (uint32_t)lv); lv = t; }
                            if (!right_vec && !right_flt) { int t = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)t, (uint32_t)rv); rv = t; }
                            ir_emit_cmp(c, (uint32_t)lv, (uint32_t)rv);
                        } else {
                            ir_emit_cmp(c, (uint32_t)lv, (uint32_t)rv);
                        }
                        if (esz == 16) {
                            /* Predicate lane: full 128-bit -1 mask or 0.
                             * ov is 0 or 1. Multiply by 64-bit -1 to get 0 or ~0ULL,
                             * and replicate to both halves of GPR128. */
                            int m = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, m, -1);
                            int m64 = zc_alloc_temp(c);
                            ir_emit_mul(c, m64, ov, m);
                            int ov128 = zc_alloc_temp(c);
                            ir_emit_mk128(c, ov128, m64, m64);
                            ov = ov128;
                        } else {
                            ir_emit_cond_set(c, (uint32_t)ov, cmp_cc);
                            /* Predicate lane: full -1 mask / 0 (GCC). The
                             * result vector has an integer element type
                             * (see type_vector_cmp_result); the mask stores
                             * as int lanes of the result element width. */
                            int m = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, m, -1);
                            int ov2 = zc_alloc_temp(c);
                            ir_emit_mul(c, ov2, ov, m);
                            ov = ov2;
                        }
                    } else if (elem_float) {
                        int ls = lv, rs = rv;
                        /* Double FP model: VLOADSS/VLOADSD lane values
                         * are already double. A double scalar with float
                         * lanes narrows to the element type first (C
                         * semantics) then re-widens; int scalars widen
                         * with cvtsi2sd. */
                        if (esz == 4) {
                            if (!left_vec && left_flt) { int t = zc_alloc_temp(c); ir_emit_cvtsd2ss(c, (uint32_t)t, (uint32_t)lv); lv = t; int t2 = zc_alloc_temp(c); ir_emit_cvtss2sd(c, (uint32_t)t2, (uint32_t)lv); ls = t2; }
                            if (!right_vec && right_flt) { int t = zc_alloc_temp(c); ir_emit_cvtsd2ss(c, (uint32_t)t, (uint32_t)rv); rv = t; int t2 = zc_alloc_temp(c); ir_emit_cvtss2sd(c, (uint32_t)t2, (uint32_t)rv); rs = t2; }
                        }
                        if (!left_vec && !left_flt) { int t = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)t, (uint32_t)lv); ls = t; }
                        if (!right_vec && !right_flt) { int t = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)t, (uint32_t)rv); rs = t; }
                        switch (vop) {
                            case TK_PLUS:  ir_emit_fadd(c, (uint32_t)ov, (uint32_t)ls, (uint32_t)rs); break;
                            case TK_MINUS: ir_emit_fsub(c, (uint32_t)ov, (uint32_t)ls, (uint32_t)rs); break;
                            case TK_STAR:  ir_emit_fmul(c, (uint32_t)ov, (uint32_t)ls, (uint32_t)rs); break;
                            default:       ir_emit_fdiv(c, (uint32_t)ov, (uint32_t)ls, (uint32_t)rs); break;
                        }
                        ov_flt = 1;
                    } else if (esz == 16) {
                        /* Sprint 113: 128-bit integer lane - GPR128 op set.
                         * div/mod and variable shifts call the zero_rt
                         * software routines (same pattern as the scalar
                         * __int128 path). */
                        switch (vop) {
                            case TK_PLUS:  ir_emit_add128(c, ov, lv, rv); break;
                            case TK_MINUS: ir_emit_sub128(c, ov, lv, rv); break;
                            case TK_STAR:  ir_emit_mul128(c, ov, lv, rv); break;
                            case TK_AMP:   ir_emit_and128(c, ov, lv, rv); break;
                            case TK_PIPE:  ir_emit_or128(c, ov, lv, rv);  break;
                            case TK_CARET: ir_emit_xor128(c, ov, lv, rv); break;
                            case TK_SLASH:
                            case TK_PERCENT: {
                                bool is_div = (vop == TK_SLASH);
                                const char *fn = is_div ? (elem_unsigned ? "zc_udiv128" : "zc_sdiv128")
                                                        : (elem_unsigned ? "zc_umod128" : "zc_smod128");
                                if (c->function_count < ZERO_CC_MAX_FUNCTIONS &&
                                    !zero_cc_find_function(c, fn)) {
                                    ZeroCCFunction *zfn = &c->functions[c->function_count++];
                                    memset(zfn, 0, sizeof(*zfn));
                                    snprintf(zfn->name, sizeof(zfn->name), "%s", fn);
                                    zfn->text_offset = 0;
                                    zfn->returns_struct = false;
                                    zfn->ret_struct_sz = 0;
                                    zfn->is_prototype = true;
                                    zfn->has_prototype = false;
                                    zfn->is_external = true;
                                    zfn->is_variadic = false;
                                }
                                int alo = zc_alloc_temp(c); ir_emit_lo128(c, alo, lv);
                                int ahi = zc_alloc_temp(c); ir_emit_hi128(c, ahi, lv);
                                int blo = zc_alloc_temp(c); ir_emit_lo128(c, blo, rv);
                                int bhi = zc_alloc_temp(c); ir_emit_hi128(c, bhi, rv);
                                ir_emit_call(c, fn);
                                int at[4] = {alo, ahi, blo, bhi};
                                if (ir_get_count(c) > 0) {
                                    Instr *li = (Instr*)&ir_get_buffer(c)[ir_get_count(c) - 1];
                                    li->temp_dest = 4;
                                    li->temp_src1 = (uint32_t)at[0];
                                    li->temp_src2 = (uint32_t)at[1];
                                    li->imm = IR_CALL_ARG01_REG;
                                    li->cond = 0;
                                    li->width = 0;
                                    ir_emit_call_arg(c, (uint32_t)at[2], 0, 255);
                                    ir_emit_call_arg(c, (uint32_t)at[3], 0, 255);
                                }
                                int rlo = zc_alloc_temp(c); ir_emit_mov_reg(c, rlo, VREG_0);
                                int rhi = zc_alloc_temp(c); ir_emit_mov_reg(c, rhi, VREG_2);
                                ir_emit_mk128(c, ov, rlo, rhi);
                                if (elem_unsigned) ir_mark_last_unsigned(c, 1);
                                break;
                            }
                            case TK_SHL:
                            case TK_SHR: {
                                const char *fn = vop == TK_SHL ? "zc_shl128_v" :
                                                 (elem_unsigned ? "zc_shr128_v" : "zc_sar128_v");
                                if (c->function_count < ZERO_CC_MAX_FUNCTIONS &&
                                    !zero_cc_find_function(c, fn)) {
                                    ZeroCCFunction *zfn = &c->functions[c->function_count++];
                                    memset(zfn, 0, sizeof(*zfn));
                                    snprintf(zfn->name, sizeof(zfn->name), "%s", fn);
                                    zfn->text_offset = 0;
                                    zfn->returns_struct = false;
                                    zfn->ret_struct_sz = 0;
                                    zfn->is_prototype = true;
                                    zfn->has_prototype = false;
                                    zfn->is_external = true;
                                    zfn->is_variadic = false;
                                }
                                int alo = zc_alloc_temp(c); ir_emit_lo128(c, alo, lv);
                                int ahi = zc_alloc_temp(c); ir_emit_hi128(c, ahi, lv);
                                int n64 = zc_alloc_temp(c); ir_emit_lo128(c, n64, rv);
                                ir_emit_call(c, fn);
                                int at[3] = {alo, ahi, n64};
                                if (ir_get_count(c) > 0) {
                                    Instr *li = (Instr*)&ir_get_buffer(c)[ir_get_count(c) - 1];
                                    li->temp_dest = 3;
                                    li->temp_src1 = (uint32_t)at[0];
                                    li->temp_src2 = (uint32_t)at[1];
                                    li->imm = IR_CALL_ARG01_REG;
                                    li->cond = 0;
                                    li->width = 0;
                                    ir_emit_call_arg(c, (uint32_t)at[2], 0, 255);
                                }
                                int rlo = zc_alloc_temp(c); ir_emit_mov_reg(c, rlo, VREG_0);
                                int rhi = zc_alloc_temp(c); ir_emit_mov_reg(c, rhi, VREG_2);
                                ir_emit_mk128(c, ov, rlo, rhi);
                                break;
                            }
                            default: ir_emit_mov128(c, ov, lv); break;
                        }
                    } else {
                        switch (vop) {
                            case TK_PLUS:  ir_emit_add(c, ov, lv, rv); break;
                            case TK_MINUS: ir_emit_sub(c, ov, lv, rv); break;
                            case TK_STAR:  ir_emit_mul(c, ov, lv, rv); break;
                            case TK_SLASH: ir_emit_div(c, ov, lv, rv); if (elem_unsigned) ir_mark_last_unsigned(c, 1); break;
                            case TK_PERCENT: ir_emit_mod(c, ov, lv, rv); if (elem_unsigned) ir_mark_last_unsigned(c, 1); break;
                            case TK_AMP:  ir_emit_and(c, ov, lv, rv); break;
                            case TK_PIPE: ir_emit_or(c, ov, lv, rv); break;
                            case TK_CARET: ir_emit_xor(c, ov, lv, rv); break;
                            case TK_SHL:  ir_emit_shl(c, ov, lv, rv); break;
                            case TK_SHR:  ir_emit_shr(c, ov, lv, rv); break;
                            default: ir_emit_mov_reg(c, ov, lv); break;
                        }
                    }

                    /* Store lane i of the result. Comparison results
                     * have an integer element type; arithmetic results
                     * keep the operand element type. */
                    int32_t lane_off = -(res_off - (int32_t)(i * esz));
                    int lane_addr = zc_alloc_temp(c);
                    ir_emit_lea(c, lane_addr, BE(c)->fp_reg, lane_off);
                    if (is_cmp) {
                        if (esz == 1) ir_emit_store_byte(c, ov, lane_addr);
                        else if (esz == 2) ir_emit_store_16(c, ov, lane_addr);
                        else if (esz == 4) ir_emit_store_32(c, ov, lane_addr);
                        else if (esz == 16) ir_emit_store128(c, ov, lane_addr);
                        else ir_emit_store_64(c, ov, lane_addr);
                    } else if (elem_float) {
                        if (esz == 4) {
                            if (!ov_flt) { int t = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)t, (uint32_t)ov); ov = t; ov_flt = 1; }
                            /* Double FP model: VSTORESS narrows the
                             * double to single internally. */
                            ir_emit_vstoress(c, (uint32_t)lane_addr, (uint32_t)ov);
                        } else {
                            ir_emit_vstoresd(c, (uint32_t)lane_addr, (uint32_t)ov);
                        }
                    } else if (esz == 16) {
                        ir_emit_store128(c, ov, lane_addr);
                    } else if (esz == 1) {
                        ir_emit_store_byte(c, ov, lane_addr);
                    } else if (esz == 2) {
                        ir_emit_store_16(c, ov, lane_addr);
                    } else if (esz == 4) {
                        ir_emit_store_32(c, ov, lane_addr);
                    } else {
                        ir_emit_store_64(c, ov, lane_addr);
                    }
                }
                int res_addr = zc_alloc_temp(c);
                ir_emit_lea(c, res_addr, BE(c)->fp_reg, -res_off);
                c->last_expr_is_float = 0;
                return res_addr;
            }
        }

        /* Logical AND (&&) / OR (||): C99 6.5.13/6.5.14 short-circuit
         * evaluation. The right operand is evaluated ONLY if the left
         * operand does not determine the result (`p && *p` must not
         * dereference a null `p`). */
        if (node->aux.op == TK_LOGAND || node->aux.op == TK_LOGOR) {
            int result_temp = zc_alloc_temp(c);
            uint16_t rhs_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;
            int lc = lower_expr(c, arena, node->left);
            if (c->last_expr_is_float) {
                int zero_f = zc_alloc_temp(c);
                ir_emit_fmov_imm(c, (uint32_t)zero_f, 0.0);
                int cmp_i = zc_alloc_temp(c);
                ir_emit_cmp(c, lc, zero_f);
                ir_emit_cond_set(c, cmp_i, CC_NE);
                lc = cmp_i;
                c->last_expr_is_float = 0;
            } else if (c->isa == ISA_RISCV32 && c->last_expr_is_64) {
                int zero64 = zc_alloc_temp(c);
                ir_emit_mov64_imm(c, zero64, 0);
                ir_emit_cmp64(c, lc, zero64);
                int cmp_i = zc_alloc_temp(c);
                ir_emit_cond_set(c, cmp_i, CC_NE);
                lc = cmp_i;
                c->last_expr_is_64 = 0;
            }
            if (node->aux.op == TK_LOGAND) {
                ir_emit_mov_imm(c, result_temp, 0);
                ir_emit_je(c, (uint32_t)lc, end_label);
            } else {
                ir_emit_mov_imm(c, result_temp, 1);
                ir_emit_jne(c, (uint32_t)lc, end_label);
            }
            ir_emit_label(c, rhs_label);
            int rc = lower_expr(c, arena, node->right);
            int rt = zc_alloc_temp(c);
            if (c->last_expr_is_float) {
                int zero_f = zc_alloc_temp(c);
                ir_emit_fmov_imm(c, (uint32_t)zero_f, 0.0);
                ir_emit_cmp(c, rc, zero_f);
                ir_emit_cond_set(c, rt, CC_NE);
                c->last_expr_is_float = 0;
            } else if (c->isa == ISA_RISCV32 && c->last_expr_is_64) {
                int zero64 = zc_alloc_temp(c);
                ir_emit_mov64_imm(c, zero64, 0);
                ir_emit_cmp64(c, rc, zero64);
                ir_emit_cond_set(c, rt, CC_NE);
                c->last_expr_is_64 = 0;
            } else {
                ir_emit_cmp_zero(c, rc);
                ir_emit_cond_set(c, rt, CC_NE);
            }
            ir_emit_mov_reg(c, result_temp, rt);
            ir_emit_label(c, end_label);
            c->last_expr_is_float = 0;
            c->last_expr_is_64 = 0;
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
                int conv_temp = zc_alloc_temp(c);
                if (fp_is_float) {
                    ir_emit(c, (Instr){.op = OP_VCVT_SI2SS, .temp_dest = conv_temp,
                                    .temp_src1 = left_temp, .temp_src2 = TEMP_NONE,
                                    .imm = 0, .label_idx = 0});
                    int wid = zc_alloc_temp(c);
                    ir_emit_cvtss2sd(c, (uint32_t)wid, (uint32_t)conv_temp);
                    conv_temp = wid;
                } else {
                    ir_emit_cvtsi2sd(c, (uint32_t)conv_temp, (uint32_t)left_temp);
                }
                left_temp = conv_temp;
                left_is_float = 1;
            } else {
                int conv_temp = zc_alloc_temp(c);
                if (fp_is_float) {
                    ir_emit(c, (Instr){.op = OP_VCVT_SI2SS, .temp_dest = conv_temp,
                                    .temp_src1 = right_temp, .temp_src2 = TEMP_NONE,
                                    .imm = 0, .label_idx = 0});
                    int wid = zc_alloc_temp(c);
                    ir_emit_cvtss2sd(c, (uint32_t)wid, (uint32_t)conv_temp);
                    conv_temp = wid;
                } else {
                    ir_emit_cvtsi2sd(c, (uint32_t)conv_temp, (uint32_t)right_temp);
                }
                right_temp = conv_temp;
                right_is_float = 1;
            }
        }

        /* Float arithmetic and comparisons - use IR for proper register allocation */
        if (left_is_float && right_is_float) {
            /* At this point: left_temp and right_temp are float temps in XMM registers */
            int result_temp = zc_alloc_temp(c);
            switch (node->aux.op) {
                case TK_PLUS:
                    ir_emit_fadd(c, (uint32_t)result_temp, (uint32_t)left_temp, (uint32_t)right_temp);
                    c->last_expr_is_float = 1;
                    return result_temp;
                case TK_MINUS:
                    ir_emit_fsub(c, (uint32_t)result_temp, (uint32_t)left_temp, (uint32_t)right_temp);
                    c->last_expr_is_float = 1;
                    return result_temp;
                case TK_STAR:
                    ir_emit_fmul(c, (uint32_t)result_temp, (uint32_t)left_temp, (uint32_t)right_temp);
                    c->last_expr_is_float = 1;
                    return result_temp;
                case TK_SLASH:
                    ir_emit_fdiv(c, (uint32_t)result_temp, (uint32_t)left_temp, (uint32_t)right_temp);
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
                    ir_emit_cmp(c, left_temp, right_temp);
                    ir_emit_cond_set(c, result_temp, cond);
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
                int d = zc_alloc_temp(c);
                ir_emit_sub(c, d, left_temp, right_temp);
                size_t psz = type_size(lti2->kind == TYPE_KIND_ARRAY ?
                    lti2->details.array.base : lti2->details.pointer.base);
                if (psz > 1) {
                    int ds = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, ds, (int32_t)psz);
                    int dq = zc_alloc_temp(c);
                    ir_emit_div(c, dq, d, ds);
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
                    int si = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, si, (int32_t)psz);
                    int s = zc_alloc_temp(c);
                    if (l_ptr) {
                        ir_emit_mul(c, s, right_temp, si);
                        right_temp = s;
                    } else {
                        ir_emit_mul(c, s, left_temp, si);
                        left_temp = s;
                    }
                }
                /* fall through: raw add/sub on the scaled operand */
            }
        }

        /* Integer arithmetic - use IR */
        int result_temp = zc_alloc_temp(c);
        switch (node->aux.op) {
            case TK_PLUS:
                ir_emit_add(c, result_temp, left_temp, right_temp);
                zc_maybe_emit_ovf_trap(c, arena, node, (uint32_t)result_temp,
                                       left_temp, right_temp, ZC_OVF_OP_ADD);
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, node->right, result_temp);
                break;
            case TK_MINUS:
                ir_emit_sub(c, result_temp, left_temp, right_temp);
                zc_maybe_emit_ovf_trap(c, arena, node, (uint32_t)result_temp,
                                       left_temp, right_temp, ZC_OVF_OP_SUB);
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, node->right, result_temp);
                break;
            case TK_STAR:
                ir_emit_mul(c, result_temp, left_temp, right_temp);
                zc_maybe_emit_ovf_trap(c, arena, node, (uint32_t)result_temp,
                                       left_temp, right_temp, ZC_OVF_OP_MUL);
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
                    ir_emit_zext(c, left_temp, 4);
                    ir_emit_zext(c, right_temp, 4);
                }
                /* Check BEFORE the div: x86 idiv raises SIGFPE on a zero
                 * divisor, so a post-check is too late (aarch64/riscv div-
                 * by-zero is defined, but a pre-check is equally valid).
                 * The divisor temp is live across both instructions. */
                zc_maybe_emit_div_zero_trap(c, arena, node, right_temp);
                ir_emit_div(c, result_temp, left_temp, right_temp);
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, AST_NULL, result_temp);
                if (zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) {
                    ir_mark_last_unsigned(c, 1);
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
                    ir_emit_zext(c, left_temp, 4);
                    ir_emit_zext(c, right_temp, 4);
                }
                /* Check BEFORE the mod: see TK_SLASH (x86 idiv SIGFPE). */
                zc_maybe_emit_rem_zero_trap(c, arena, node, right_temp);
                ir_emit_mod(c, result_temp, left_temp, right_temp);
                result_temp = zc_binop_bitfield_reduce(c, arena, node->left, AST_NULL, result_temp);
                if (zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) {
                    ir_mark_last_unsigned(c, 1);
                }
                break;
            }
            case TK_AMP:
                ir_emit_and(c, result_temp, left_temp, right_temp);
                break;
            case TK_PIPE:
                ir_emit_or(c, result_temp, left_temp, right_temp);
                break;
            case TK_CARET:
                ir_emit_xor(c, result_temp, left_temp, right_temp);
                break;
            case TK_SHL:
                /* Check BEFORE the shift (uniform with TK_SLASH): the
                 * count temp's last real use is the shift, so a post-check
                 * would find its register already freed/reused. */
                zc_maybe_emit_shift_count_trap(c, arena, node, right_temp);
                ir_emit_shl(c, result_temp, left_temp, right_temp);
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
                /* Check BEFORE the shift: see TK_SHL. */
                zc_maybe_emit_shift_count_trap(c, arena, node, right_temp);
                if (!lu) {
                    ir_emit_sar(c, result_temp, left_temp, right_temp);
                } else {
                    ir_emit_shr(c, result_temp, left_temp, right_temp);
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
                        ir_emit_zext(c, left_temp, 4);
                        ir_emit_zext(c, right_temp, 4);
                    }
                }
                ir_emit_cmp(c, left_temp, right_temp);
                ir_emit_cond_set(c, result_temp, cond);
                break;
            }
            default:
                ir_emit_mov_reg(c, result_temp, left_temp);
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
                        ir_mark_last_width(c, ls ? (uint8_t)lw : (uint8_t)(8 + lw));
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
                        ir_mark_last_width(c, s ? (uint8_t)w : (uint8_t)(8 + w));
                    }
                }
            }
        }
        c->last_expr_is_float = 0;
        return result_temp;
}

/* lower_expr_unop -> ast_lowering_unop.c, lower_expr_cast -> ast_lowering_cast.c (Sprint 114). */
