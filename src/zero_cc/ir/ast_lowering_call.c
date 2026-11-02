/* AST Lowering - Call, Builtin, and Vararg Lowering (AST to IR) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"

/* Does the node denote a compile-time constant expression?  Used by
 * __builtin_constant_p (c-torture execute/bcp-1): literals and
 * sub-expressions built only from them are constant; anything touching an
 * identifier, call, member, or side effect is not. */
static bool zc_node_is_const_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    (void)c;
    if (ref == AST_NULL) return false;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;
    switch (n->tag) {
        case NODE_INT:
        case NODE_FLOAT:
        case NODE_STRING:
        case NODE_NULLPTR:
        case NODE_BOOL:
            return true;
        case NODE_CAST:
            return zc_node_is_const_expr(c, arena, n->left);
        case NODE_UNOP:
            switch (n->aux.op) {
                case TK_PLUS: case TK_MINUS: case TK_BANG: case TK_TILDE:
                    return zc_node_is_const_expr(c, arena, n->left);
                default:
                    return false;
            }
        case NODE_BINOP:
            switch (n->aux.op) {
                case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH:
                case TK_PERCENT: case TK_AMP: case TK_PIPE: case TK_CARET:
                case TK_SHL: case TK_SHR:
                case TK_EQ: case TK_NE: case TK_LT: case TK_GT:
                case TK_LE: case TK_GE:
                case TK_LOGAND: case TK_LOGOR:
                    return zc_node_is_const_expr(c, arena, n->left) &&
                           zc_node_is_const_expr(c, arena, n->right);
                default:
                    return false;
            }
        case NODE_TERNARY:
            return zc_node_is_const_expr(c, arena, n->left) &&
                   zc_node_is_const_expr(c, arena, n->right) &&
                   zc_node_is_const_expr(c, arena, (ASTRef)n->aux.alt);
        case NODE_INDEX: {
            ZeroCCASTNode *base = zero_cc_ast_get(arena, n->left);
            return (base && base->tag == NODE_STRING) &&
                   zc_node_is_const_expr(c, arena, n->right);
        }
        default:
            return false;
    }
}

/* Evaluate an AST node as a compile-time integer constant, recognizing
 * the __builtin_constant_p / __builtin_choose_expr builtins (which are
 * themselves constant expressions).  Returns 1 (with *out set) when the
 * node is a constant, 0 otherwise.  Used by __builtin_choose_expr's
 * runtime lowering to decide which operand to evaluate. */
static bool zc_ast_const_eval(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                              ASTRef ref, int64_t *out) {
    (void)c;
    if (ref == AST_NULL) return false;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;
    switch (n->tag) {
        case NODE_INT:
        case NODE_BOOL:
            *out = n->int_val;
            return true;
        case NODE_FLOAT:
            *out = (int64_t)n->float_val;
            return true;
        case NODE_CAST:
            return zc_ast_const_eval(c, arena, n->left, out);
        case NODE_UNOP:
            if (!zc_ast_const_eval(c, arena, n->left, out)) return false;
            switch (n->aux.op) {
                case TK_PLUS: break;
                case TK_MINUS: *out = -*out; break;
                case TK_BANG: *out = !*out; break;
                case TK_TILDE: *out = ~*out; break;
                default: return false;
            }
            return true;
        case NODE_BINOP: {
            int64_t l, r;
            if (!zc_ast_const_eval(c, arena, n->left, &l)) return false;
            if (!zc_ast_const_eval(c, arena, n->right, &r)) return false;
            switch (n->aux.op) {
                case TK_PLUS: *out = l + r; break;
                case TK_MINUS: *out = l - r; break;
                case TK_STAR: *out = l * r; break;
                case TK_SLASH: if (r == 0) return false; *out = l / r; break;
                case TK_PERCENT: if (r == 0) return false; *out = l % r; break;
                case TK_AMP: *out = l & r; break;
                case TK_PIPE: *out = l | r; break;
                case TK_CARET: *out = l ^ r; break;
                case TK_SHL: *out = l << r; break;
                case TK_SHR: *out = l >> r; break;
                case TK_EQ: *out = (l == r); break;
                case TK_NE: *out = (l != r); break;
                case TK_LT: *out = (l < r); break;
                case TK_GT: *out = (l > r); break;
                case TK_LE: *out = (l <= r); break;
                case TK_GE: *out = (l >= r); break;
                case TK_LOGAND: *out = (l && r); break;
                case TK_LOGOR: *out = (l || r); break;
                default: return false;
            }
            return true;
        }
        case NODE_TERNARY: {
            int64_t cond, t, f;
            if (!zc_ast_const_eval(c, arena, n->left, &cond)) return false;
            if (cond) return zc_ast_const_eval(c, arena, n->right, out);
            return zc_ast_const_eval(c, arena, (ASTRef)n->aux.alt, out);
        }
        case NODE_CALL: {
            ZeroCCASTNode *cal = zero_cc_ast_get(arena, n->left);
            if (!cal || cal->tag != NODE_IDENT || !cal->s.str) return false;
            if (strcmp(cal->s.str, "__builtin_constant_p") == 0) {
                *out = zc_node_is_const_expr(c, arena, n->right) ? 1 : 0;
                return true;
            }
            if (strcmp(cal->s.str, "__builtin_choose_expr") == 0) {
                ASTRef r1 = n->right;
                if (r1 == AST_NULL) return false;
                ZeroCCASTNode *an1 = zero_cc_ast_get(arena, r1);
                ASTRef r2 = an1 ? an1->next : AST_NULL;
                ZeroCCASTNode *an2 = (r2 != AST_NULL) ? zero_cc_ast_get(arena, r2) : NULL;
                ASTRef r3 = an2 ? an2->next : AST_NULL;
                if (r2 == AST_NULL || r3 == AST_NULL) return false;
                int64_t cond;
                if (!zc_ast_const_eval(c, arena, r1, &cond)) return false;
                return zc_ast_const_eval(c, arena, cond ? r2 : r3, out);
            }
            return false;
        }
        default:
            return false;
    }
}


/* --- __builtin_{add,sub,mul}_overflow helpers ------------------------- */
/* 0/1 temp holding (a <cc> b) over already-computed values. */
static int zc_ovf_cmp_set(ZeroCCCompiler *c, int a, int b, int cc) {
    int t = c->next_temp_id++;
    ir_emit_cmp(a, b);
    ir_emit_cond_set(t, cc);
    return t;
}

/* 0/1 temp holding (a <cc> imm). */
static int zc_ovf_cmp_set_imm(ZeroCCCompiler *c, int a, int64_t imm, int cc) {
    int b = c->next_temp_id++;
    ir_emit_mov_imm(b, imm);
    return zc_ovf_cmp_set(c, a, b, cc);
}

static int zc_ovf_or(ZeroCCCompiler *c, int a, int b) {
    int t = c->next_temp_id++;
    ir_emit_or(t, a, b);
    return t;
}

static int zc_ovf_and(ZeroCCCompiler *c, int a, int b) {
    int t = c->next_temp_id++;
    ir_emit_and(t, a, b);
    return t;
}

/* 1 - a for a 0/1 temp (logical NOT). */
static int zc_ovf_not1(ZeroCCCompiler *c, int a) {
    int one = c->next_temp_id++;
    ir_emit_mov_imm(one, 1);
    int t = c->next_temp_id++;
    ir_emit_xor(t, a, one);
    return t;
}


/* ---- C11/GCC atomic-builtin dispatch (C11 7.6 / GNU __sync + __atomic) ----
 *
 * Every __sync_* / __atomic_* builtin is a flat name -> fixed-arity -> IR
 * mapping: there is no context-dependent token stream, so the dispatch is a
 * static table + one switch (a state machine would track state that never
 * transitions). Previously ~14 of the builtins were individual
 * strcmp if-blocks hardcoding a 32-bit access width, and every name that
 * was not listed (e.g. __atomic_thread_fence, __atomic_fetch_or, the
 * non-_n variants) fell through to the generic call emitter and became an
 * external call to a nonexistent symbol — a runtime segfault.
 *
 * Width: the access width is derived from the pointer operand's semantic
 * TypeInfo (C11 requires every integer width, not just 32-bit). Memory-
 * order arguments are accepted and ignored: x86-64 TSO makes every locked
 * RMW seq_cst-strength, and OP_MFENCE is a valid (over-strong) lowering of
 * any fence order. */
typedef enum ZCAtomicKind {
    ZC_AT_FETCH_ADD, ZC_AT_ADD_FETCH,
    ZC_AT_FETCH_SUB, ZC_AT_SUB_FETCH,
    ZC_AT_FETCH_OR,  ZC_AT_OR_FETCH,
    ZC_AT_FETCH_AND, ZC_AT_AND_FETCH,
    ZC_AT_FETCH_XOR, ZC_AT_XOR_FETCH,
    ZC_AT_CAS_OLD,   ZC_AT_CAS_BOOL,   ZC_AT_CAS_BOOL_SYNC,
    ZC_AT_EXCHANGE,  ZC_AT_STORE,
    ZC_AT_LOAD,      ZC_AT_STORE_0,
    ZC_AT_FENCE,     ZC_AT_TEST_AND_SET,
    ZC_AT_CLEAR,     ZC_AT_IS_LOCK_FREE
} ZCAtomicKind;

static const struct {
    const char   *name;
    ZCAtomicKind  kind;
} zc_atomic_builtin_table[] = {
    { "__sync_fetch_and_add",          ZC_AT_FETCH_ADD },
    { "__atomic_fetch_add",            ZC_AT_FETCH_ADD },
    { "__sync_add_and_fetch",          ZC_AT_ADD_FETCH },
    { "__atomic_add_fetch",            ZC_AT_ADD_FETCH },
    { "__sync_fetch_and_sub",          ZC_AT_FETCH_SUB },
    { "__atomic_fetch_sub",            ZC_AT_FETCH_SUB },
    { "__sync_sub_and_fetch",          ZC_AT_SUB_FETCH },
    { "__atomic_sub_fetch",            ZC_AT_SUB_FETCH },
    { "__sync_fetch_and_or",           ZC_AT_FETCH_OR },
    { "__atomic_fetch_or",             ZC_AT_FETCH_OR },
    { "__sync_or_and_fetch",           ZC_AT_OR_FETCH },
    { "__atomic_or_fetch",             ZC_AT_OR_FETCH },
    { "__sync_fetch_and_and",          ZC_AT_FETCH_AND },
    { "__atomic_fetch_and",            ZC_AT_FETCH_AND },
    { "__sync_and_and_fetch",          ZC_AT_AND_FETCH },
    { "__atomic_and_fetch",            ZC_AT_AND_FETCH },
    { "__sync_fetch_and_xor",          ZC_AT_FETCH_XOR },
    { "__atomic_fetch_xor",            ZC_AT_FETCH_XOR },
    { "__sync_xor_and_fetch",          ZC_AT_XOR_FETCH },
    { "__atomic_xor_fetch",            ZC_AT_XOR_FETCH },
    { "__sync_val_compare_and_swap",   ZC_AT_CAS_OLD },
    { "__sync_bool_compare_and_swap",  ZC_AT_CAS_BOOL_SYNC },
    { "__atomic_compare_exchange_n",   ZC_AT_CAS_BOOL },
    { "__atomic_compare_exchange",     ZC_AT_CAS_BOOL },
    { "__sync_lock_test_and_set",      ZC_AT_EXCHANGE },
    { "__atomic_exchange_n",           ZC_AT_EXCHANGE },
    { "__atomic_exchange",             ZC_AT_EXCHANGE },
    { "__atomic_store_n",              ZC_AT_STORE },
    { "__atomic_store",                ZC_AT_STORE },
    { "__atomic_load_n",               ZC_AT_LOAD },
    { "__atomic_load",                 ZC_AT_LOAD },
    { "__sync_lock_release",           ZC_AT_STORE_0 },
    { "__sync_synchronize",            ZC_AT_FENCE },
    { "__atomic_thread_fence",         ZC_AT_FENCE },
    { "__atomic_test_and_set",         ZC_AT_TEST_AND_SET },
    { "__atomic_clear",                ZC_AT_CLEAR },
    { "__atomic_is_lock_free",         ZC_AT_IS_LOCK_FREE },
};

/* Access width for an atomic builtin: the size of the pointer operand's
 * pointee, clamped to the x86-64 atomic widths (1/2/4/8). Falls back to 4
 * when the operand type is unavailable (matches the pre-table behavior). */
static int zc_atomic_access_width(ZeroCCCompiler *c, const ZeroCCASTNode *arg0_node) {
    TypeInfo *ti = ast_node_get_typeinfo(c, arg0_node);
    if (ti && ti->kind == TYPE_KIND_POINTER && ti->details.pointer.base) {
        size_t sz = type_size(ti->details.pointer.base);
        if (sz == 1 || sz == 2 || sz == 4 || sz == 8) return (int)sz;
    }
    return 4;
}

/* Extend a value operand for a wider atomic access (C99 6.3.1.2). Skips
 * 64-bit integer literals: the semantic model types every NODE_INT as
 * 32-bit int, so SEXT32-ing a literal whose value needs 64 bits would
 * corrupt it (e.g. __atomic_store_n(&l, 0x1122334455667788LL)). A literal
 * whose int64 value fits in signed int32 is exactly C's int-typed literal
 * (decimal in-range, or hex/unsigned within uint32 where the mathematical
 * value already equals its C-converted form), so normal extension applies. */
static void zc_atomic_extend_value(ZeroCCCompiler *c, int temp,
                                   const ZeroCCASTNode *node, const TypeInfo *ti) {
    if (node && node->tag == NODE_INT) {
        int64_t v = node->int_val;
        if (v < INT32_MIN || v > INT32_MAX) return;  /* genuine 64-bit literal */
    }
    lower_extend_narrow(c, temp, ti);
}


int lower_expr_va_start(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        /* va_start(ap, last) - initialize the ABI-correct va_list.
         *
         * The callee prologue spilled ALL argument registers into a
         * 16-slot save area above the locals (see the variadic spill in
         * zero_cc_lower_ast): slots 0..5 = integer arg regs in order,
         * slots 6..13 = FP arg regs in order. Named parameters consume
         * the streams in order, so the streams start just past the
         * named count. Once a stream is exhausted (ptr == end) reads
         * continue from ovf - the shared stack-overflow area (the
         * caller's stack args, in argument order).
         *
         * va_list layout (5 x 8-byte pointers):
         *   +0 gp, +8 gp_end, +16 fp, +24 fp_end, +32 ovf. */
        int ap_addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
        ZeroCCFunction *fn = zero_cc_get_current_fn(c);

        if (ap_addr != TEMP_NONE && fn) {

            /* Save area base: above all named locals (mirrors the
             * prologue's base_spill_offset = locals-top + 8). */
            int32_t top_local = (int32_t)BE(c)->callee_save_frame_reserve;
            for (size_t i = 0; i < c->local_count; i++) {
                if (c->locals[i].stack_offset > top_local) top_local = c->locals[i].stack_offset;
            }
            int32_t save_base = top_local + 8;

            /* Named params consume the GP/FP streams in order (mirror
             * the prologue's param-save loop, incl. the hidden sret
             * pointer and multi-chunk struct params). */
            int gp_regs = BE(c)->conv->int_arg_reg_count;
            int fp_regs = BE(c)->conv->fp_arg_reg_count;
            int gp_idx = (fn->ret_struct_sz > 16 && c->isa != ISA_AARCH64) ? 1 : 0;
            int fp_idx = 0;
            int stack_named = 0;  /* stack-passed named params (ovf base) */
            for (size_t p = 0; p < fn->param_count && p < c->local_count; p++) {
                ZeroCCSym *ps = &c->locals[p];
                if (!ps->is_param) continue;
                bool pfp = (ps->type.tag == TYPE_FLOAT || ps->type.tag == TYPE_DOUBLE);
                /* >16B struct/union params take ONE hidden-pointer slot
                 * (SysV ABI), not one slot per 8-byte chunk. */
                bool phidden = (ps->type.tag == TYPE_STRUCT || ps->type.tag == TYPE_UNION) && ps->type.size > 16;
                bool pmulti = (ps->type.tag == TYPE_STRUCT || ps->type.tag == TYPE_UNION) && ps->type.size > 8 && !phidden;
                if (phidden) {
                    if (gp_idx < gp_regs) gp_idx++; else stack_named++;
                } else if (pmulti) {
                    int chunks = (int)((ps->type.size + 7) / 8);
                    for (int ch = 0; ch < chunks; ch++) { if (gp_idx < gp_regs) gp_idx++; else stack_named++; }
                } else if (pfp) {
                    if (fp_idx < fp_regs) fp_idx++; else stack_named++;
                } else {
                    if (gp_idx < gp_regs) gp_idx++; else stack_named++;
                }
            }

            int t;
            t = c->next_temp_id++;
            ir_emit_lea(t, TEMP_NONE, -(save_base + gp_idx * 8));
            ir_emit_store_64(t, ap_addr);             /* +0 gp */

            t = c->next_temp_id++;
            ir_emit_lea(t, TEMP_NONE, -(save_base + gp_regs * 8));
            int off8 = c->next_temp_id++;
            ir_emit_mov_imm(off8, 8);
            int ap_8 = c->next_temp_id++;
            ir_emit_add(ap_8, ap_addr, off8);
            ir_emit_store_64(t, ap_8);                /* +8 gp_end */

            t = c->next_temp_id++;
            ir_emit_lea(t, TEMP_NONE, -(save_base + (gp_regs + fp_idx) * 8));
            int off16 = c->next_temp_id++;
            ir_emit_mov_imm(off16, 16);
            int ap_16 = c->next_temp_id++;
            ir_emit_add(ap_16, ap_addr, off16);
            ir_emit_store_64(t, ap_16);               /* +16 fp */

            t = c->next_temp_id++;
            ir_emit_lea(t, TEMP_NONE, -(save_base + (gp_regs + fp_regs) * 8));
            int off24 = c->next_temp_id++;
            ir_emit_mov_imm(off24, 24);
            int ap_24 = c->next_temp_id++;
            ir_emit_add(ap_24, ap_addr, off24);
            ir_emit_store_64(t, ap_24);               /* +24 fp_end */

            t = c->next_temp_id++;
            ir_emit_lea(t, TEMP_NONE, 16 + 8 * stack_named);
            int off32 = c->next_temp_id++;
            ir_emit_mov_imm(off32, 32);
            int ap_32 = c->next_temp_id++;
            ir_emit_add(ap_32, ap_addr, off32);
            ir_emit_store_64(t, ap_32);               /* +32 ovf */
        }

        c->last_expr_is_float = 0;
        return VREG_0;
}

int lower_expr_va_arg(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        /* va_arg(ap, type) - extract the next variadic argument.
         * Integer/pointer targets read the GP stream (va_list +0/+8),
         * float/double targets the FP stream (+16/+24); when a stream
         * is exhausted (ptr == end) both continue from the shared
         * overflow pointer (+32). Values are read through the stream
         * pointer (frame-independent dereference). */
        TypeInfo *t_t = ast_node_get_typeinfo(c, node);
        bool is_fp = t_t && (t_t->kind == TYPE_KIND_FLOAT || t_t->kind == TYPE_KIND_DOUBLE);
        bool use_fp_stream = is_fp && !(c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32);

        int result_temp = c->next_temp_id++;
        int ap_addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
        if (ap_addr != TEMP_NONE) {
            int32_t ptr_field = use_fp_stream ? 16 : 0;    /* va_list member offset of the stream ptr */
            int32_t end_field = use_fp_stream ? 24 : 8;   /* ...and of its end sentinel */
            uint16_t ovf_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;

            int addr_ptr_field = ap_addr;
            if (ptr_field != 0) {
                int off_pf = c->next_temp_id++;
                ir_emit_mov_imm(off_pf, ptr_field);
                addr_ptr_field = c->next_temp_id++;
                ir_emit_add(addr_ptr_field, ap_addr, off_pf);
            }
            int off_ef = c->next_temp_id++;
            ir_emit_mov_imm(off_ef, end_field);
            int addr_end_field = c->next_temp_id++;
            ir_emit_add(addr_end_field, ap_addr, off_ef);

            int off_ovf = c->next_temp_id++;
            ir_emit_mov_imm(off_ovf, 32);
            int addr_ovf_field = c->next_temp_id++;
            ir_emit_add(addr_ovf_field, ap_addr, off_ovf);

            size_t struct_sz = (t_t && (t_t->kind == TYPE_KIND_STRUCT || t_t->kind == TYPE_KIND_UNION)) ? type_size(t_t) : 0;
            bool is_struct_hidden = (!is_fp && struct_sz > 16);
            bool is_struct2 = (!is_fp && struct_sz > 8 && struct_sz <= 16);
            if (is_struct_hidden) {
                /* >16B struct va_arg: the caller passed ONE hidden
                 * pointer slot (SysV ABI) - read it like a pointer;
                 * the result is the address of the caller's copy. */
                int hp = c->next_temp_id++;
                ir_emit_deref(hp, addr_ptr_field);    /* fresh stream ptr */
                int he = c->next_temp_id++;
                ir_emit_deref(he, addr_end_field);    /* fresh end sentinel */
                int hd = c->next_temp_id++;
                ir_emit_sub(hd, hp, he);
                uint16_t ho_lbl = c->ir_next_label++;
                uint16_t ha_lbl = c->ir_next_label++;
                ir_emit_je((uint16_t)hd, ho_lbl);
                /* hp = ap[0] = the ADDRESS of the save slot; the slot
                 * holds the hidden POINTER to the caller's copy. The
                 * va_arg value for a >16B struct is that hidden pointer
                 * (an address the consumers copy from; see
                 * ast_expr_is_struct_addr) - one dereference of the slot. */
                ir_emit_deref(result_temp, hp);
                int hn8 = c->next_temp_id++;
                ir_emit_mov_imm(hn8, -8);
                int hp2 = c->next_temp_id++;
                ir_emit_add(hp2, hp, hn8);
                ir_emit_store_64(hp2, addr_ptr_field);
                ir_emit_jmp(ha_lbl);
                ir_emit_label(ho_lbl);
                int ho = c->next_temp_id++;
                ir_emit_deref(ho, addr_ovf_field);
                ir_emit_deref(result_temp, ho);
                int hp8 = c->next_temp_id++;
                ir_emit_mov_imm(hp8, 8);
                int ho2 = c->next_temp_id++;
                ir_emit_add(ho2, ho, hp8);
                ir_emit_store_64(ho2, addr_ovf_field);
                ir_emit_label(ha_lbl);
            } else if (is_struct2) {
                /* 16B (or 9-16B) struct va_arg: two 8-byte chunks, but
                 * each chunk needs its OWN GP-exhaustion check - the
                 * chunks can straddle the register-stack boundary (the
                 * last register chunk and the first stack chunk, e.g.
                 * 4 x 16B structs passed variadically: chunks 0-4 in
                 * registers, chunks 5-7 on the caller's stack). */
                for (int ch = 0; ch < 2; ch++) {
                    int cp = c->next_temp_id++;
                    ir_emit_deref(cp, addr_ptr_field);    /* fresh stream ptr */
                    int ce = c->next_temp_id++;
                    ir_emit_deref(ce, addr_end_field);    /* fresh end sentinel */
                    int cd = c->next_temp_id++;
                    ir_emit_sub(cd, cp, ce);
                    uint16_t o_lbl = c->ir_next_label++;
                    uint16_t a_lbl = c->ir_next_label++;
                    ir_emit_je((uint16_t)cd, o_lbl);
                    int dst = (ch == 0) ? result_temp : VREG_2;
                    ir_emit_deref(dst, cp);
                    int n8 = c->next_temp_id++;
                    ir_emit_mov_imm(n8, -8);
                    int cp2 = c->next_temp_id++;
                    ir_emit_add(cp2, cp, n8);
                    ir_emit_store_64(cp2, addr_ptr_field);
                    ir_emit_jmp(a_lbl);
                    ir_emit_label(o_lbl);
                    int co = c->next_temp_id++;
                    ir_emit_deref(co, addr_ovf_field);
                    ir_emit_deref(dst, co);
                    int p8 = c->next_temp_id++;
                    ir_emit_mov_imm(p8, 8);
                    int co2 = c->next_temp_id++;
                    ir_emit_add(co2, co, p8);
                    ir_emit_store_64(co2, addr_ovf_field);
                    ir_emit_label(a_lbl);
                }
            } else {
            int t_ptr = c->next_temp_id++;
            ir_emit_deref(t_ptr, addr_ptr_field);
            int t_end = c->next_temp_id++;
            ir_emit_deref(t_end, addr_end_field);
            int t_diff = c->next_temp_id++;
            ir_emit_sub(t_diff, t_ptr, t_end);
            ir_emit_je((uint16_t)t_diff, ovf_label);

            /* Save-area path. The save slots extend toward MORE
             * negative offsets (slot i at -(base + i*8)), so the
             * stream pointer DECREASES by 8 per argument; it is
             * exhausted when it reaches the end sentinel. */
            if (is_fp) {
                if (t_t->kind == TYPE_KIND_FLOAT) ir_emit_vloadss((uint16_t)result_temp, (uint16_t)t_ptr);
                else ir_emit_vloadsd((uint16_t)result_temp, (uint16_t)t_ptr);
                int t_neg8 = c->next_temp_id++;
                ir_emit_mov_imm(t_neg8, -8);
                int t_ptr2 = c->next_temp_id++;
                ir_emit_add(t_ptr2, t_ptr, t_neg8);
                ir_emit_store_64(t_ptr2, addr_ptr_field);
            } else {
                ir_emit_deref(result_temp, t_ptr);
                int t_neg8 = c->next_temp_id++;
                ir_emit_mov_imm(t_neg8, -8);
                int t_ptr2 = c->next_temp_id++;
                ir_emit_add(t_ptr2, t_ptr, t_neg8);
                ir_emit_store_64(t_ptr2, addr_ptr_field);
            }
            ir_emit_jmp(end_label);

            /* Stack-overflow path: the caller's stack args sit at
             * fp+16, fp+24, ... - the pointer INCREASES by 8. */
            ir_emit_label(ovf_label);
            int t_ovf = c->next_temp_id++;
            ir_emit_deref(t_ovf, addr_ovf_field);
            if (is_fp) {
                if (t_t->kind == TYPE_KIND_FLOAT) ir_emit_vloadss((uint16_t)result_temp, (uint16_t)t_ovf);
                else ir_emit_vloadsd((uint16_t)result_temp, (uint16_t)t_ovf);
                int t_pos8 = c->next_temp_id++;
                ir_emit_mov_imm(t_pos8, 8);
                int t_ovf2 = c->next_temp_id++;
                ir_emit_add(t_ovf2, t_ovf, t_pos8);
                ir_emit_store_64(t_ovf2, addr_ovf_field);
            } else {
                ir_emit_deref(result_temp, t_ovf);
                int t_pos8 = c->next_temp_id++;
                ir_emit_mov_imm(t_pos8, 8);
                int t_ovf2 = c->next_temp_id++;
                ir_emit_add(t_ovf2, t_ovf, t_pos8);
                ir_emit_store_64(t_ovf2, addr_ovf_field);
            }
            ir_emit_label(end_label);
            }

            /* Narrow/widen integer targets to their true C width
             * (values arrived int-promoted / zero-extended). */
            if (!is_fp && t_t) {
                size_t tsz = type_size(t_t);
                bool tu = (t_t->modifiers & TYPE_MOD_UNSIGNED) != 0;
                if (tsz == 4) { if (tu) ir_emit_zext(result_temp, 4); else ir_emit_sext(result_temp, 4); }
                else if (tsz == 2) { if (tu) ir_emit_zext(result_temp, 2); else ir_emit_sext(result_temp, 2); }
                else if (tsz == 1) { if (tu) ir_emit_zext(result_temp, 1); else ir_emit_sext(result_temp, 1); }
            }
            c->last_expr_is_float = is_fp;
        } else {
            /* No identifiable va_list - legacy single-pointer
             * behaviour (dereference and bump) as a fallback. */
            int ap_temp = lower_expr(c, arena, node->left);
            ir_emit_deref(result_temp, ap_temp);
            int eight_temp = c->next_temp_id++;
            ir_emit_mov_imm(eight_temp, 8);
            int new_ap_temp = c->next_temp_id++;
            ir_emit_add(new_ap_temp, ap_temp, eight_temp);
            c->last_expr_is_float = is_fp;
        }
        return result_temp;
}

int lower_expr_call(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        /* Function call - handle float args directly in machine code, int args via IR.
         * The callee may be an identifier (direct call, or indirect through a
         * named function pointer) or a general expression (a[i](), p->cb(),
         * (t.fn)(), ...): any non-identifier callee lowers to an indirect
         * call through the evaluated pointer value. */
        ZeroCCASTNode *callee = zero_cc_ast_get(arena, node->left);
        bool callee_is_ident = (callee && callee->tag == NODE_IDENT);
        const char *name = callee_is_ident ? callee->s.str : NULL;
        if (name && strncmp(name, "__builtin_", 10) == 0) {
            const char *base_fn = name + 10;
            if (strcmp(base_fn, "memcpy") == 0 || strcmp(base_fn, "memset") == 0 ||
                strcmp(base_fn, "memmove") == 0 || strcmp(base_fn, "memcmp") == 0 ||
                strcmp(base_fn, "memchr") == 0 ||
                strcmp(base_fn, "strlen") == 0 || strcmp(base_fn, "strcmp") == 0 ||
                strcmp(base_fn, "strncmp") == 0 || strcmp(base_fn, "strcpy") == 0 ||
                strcmp(base_fn, "strncpy") == 0 || strcmp(base_fn, "strcat") == 0 ||
                strcmp(base_fn, "strchr") == 0 || strcmp(base_fn, "strrchr") == 0 ||
                strcmp(base_fn, "strstr") == 0 || strcmp(base_fn, "abort") == 0 ||
                strcmp(base_fn, "exit") == 0 || strcmp(base_fn, "malloc") == 0 ||
                strcmp(base_fn, "calloc") == 0 || strcmp(base_fn, "realloc") == 0 ||
                strcmp(base_fn, "free") == 0 || strcmp(base_fn, "printf") == 0 ||
                strcmp(base_fn, "fprintf") == 0 || strcmp(base_fn, "sprintf") == 0 ||
                strcmp(base_fn, "snprintf") == 0 || strcmp(base_fn, "puts") == 0 ||
                strcmp(base_fn, "putchar") == 0 || strcmp(base_fn, "fputc") == 0 ||
                strcmp(base_fn, "fputs") == 0 || strcmp(base_fn, "sqrt") == 0 ||
                strcmp(base_fn, "sqrtf") == 0 ||
                strcmp(base_fn, "fabsl") == 0 || strcmp(base_fn, "fabsf") == 0 ||
                strcmp(base_fn, "fabs") == 0 || strcmp(base_fn, "fmax") == 0 ||
                strcmp(base_fn, "fmaxf") == 0 || strcmp(base_fn, "fmaxl") == 0 ||
                strcmp(base_fn, "fmin") == 0 || strcmp(base_fn, "fminf") == 0 ||
                strcmp(base_fn, "fminl") == 0 || strcmp(base_fn, "floor") == 0 ||
                strcmp(base_fn, "floorf") == 0 || strcmp(base_fn, "floorl") == 0 ||
                strcmp(base_fn, "ceil") == 0 || strcmp(base_fn, "ceilf") == 0 ||
                strcmp(base_fn, "ceill") == 0 || strcmp(base_fn, "round") == 0 ||
                strcmp(base_fn, "roundf") == 0 || strcmp(base_fn, "roundl") == 0 ||
                strcmp(base_fn, "trunc") == 0 || strcmp(base_fn, "truncf") == 0 ||
                strcmp(base_fn, "truncl") == 0 || strcmp(base_fn, "fmod") == 0 ||
                strcmp(base_fn, "fmodf") == 0 || strcmp(base_fn, "fmodl") == 0 ||
                strcmp(base_fn, "pow") == 0 || strcmp(base_fn, "powf") == 0 ||
                strcmp(base_fn, "powl") == 0 || strcmp(base_fn, "exp") == 0 ||
                strcmp(base_fn, "expf") == 0 || strcmp(base_fn, "expl") == 0 ||
                strcmp(base_fn, "log") == 0 || strcmp(base_fn, "logf") == 0 ||
                strcmp(base_fn, "logl") == 0 || strcmp(base_fn, "log2") == 0 ||
                strcmp(base_fn, "log2f") == 0 || strcmp(base_fn, "log2l") == 0 ||
                strcmp(base_fn, "log10") == 0 || strcmp(base_fn, "log10f") == 0 ||
                strcmp(base_fn, "log10l") == 0 || strcmp(base_fn, "sin") == 0 ||
                strcmp(base_fn, "sinf") == 0 || strcmp(base_fn, "sinl") == 0 ||
                strcmp(base_fn, "cos") == 0 || strcmp(base_fn, "cosf") == 0 ||
                strcmp(base_fn, "cosl") == 0 || strcmp(base_fn, "tan") == 0 ||
                strcmp(base_fn, "tanf") == 0 || strcmp(base_fn, "tanl") == 0 ||
                strcmp(base_fn, "atan2") == 0 || strcmp(base_fn, "atan2f") == 0 ||
                strcmp(base_fn, "atan2l") == 0) {
                name = base_fn;
            }
        }

        /* Directly-lowered __builtin_* intrinsics (expect, prefetch,
         * constant_p, bswap32, ffs, inf, trap, ...) return before the
         * generic call path, so no external symbol is registered for
         * them.  Extern registration for genuinely-called externs
         * (__builtin_memcpy -> memcpy, implicit-declaration calls, ...)
         * happens below, after the callee is classified: registering
         * here would also capture function-pointer variables (e.g.
         * `fp(...)` where fp is a global fptr), turning indirect calls
         * into direct calls to an undefined symbol (c-torture
         * func-ptr-1.c, 991112-1.c). */

        /* __builtin_alloca(size) / alloca(size) - dynamic stack allocation */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_alloca") == 0 ||
             strcmp(callee->s.str, "alloca") == 0)) {
            int size_temp = lower_expr(c, arena, node->right);
            int add15 = c->next_temp_id++;
            ir_emit_mov_imm(add15, 15);
            int size_p15 = c->next_temp_id++;
            ir_emit_add(size_p15, size_temp, add15);
            int mask16 = c->next_temp_id++;
            ir_emit_mov_imm(mask16, -16);
            int aligned_size = c->next_temp_id++;
            ir_emit_and(aligned_size, size_p15, mask16);
            int ptr_temp = c->next_temp_id++;
            ir_emit_vla_alloc(ptr_temp, aligned_size);
            c->last_expr_is_float = 0;
            return ptr_temp;
        }

        /* GNU complex part extractors: __real(E) / __real__(E) load
         * the first half of the address-form complex value, __imag / __imag__
         * the second. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__real") == 0 ||
             strcmp(callee->s.str, "__real__") == 0 ||
             strcmp(callee->s.str, "__imag") == 0 ||
             strcmp(callee->s.str, "__imag__") == 0)) {
            bool is_imag = (callee->s.str[2] == 'i');
            ZeroCCASTNode *cx_node = zero_cc_ast_get(arena, node->right);
            TypeInfo *cx_ti = ast_node_get_typeinfo(c, cx_node);
            size_t esz = (cx_ti && cx_ti->kind == TYPE_KIND_COMPLEX &&
                          cx_ti->details.array.base)
                             ? type_size(cx_ti->details.array.base) : 8;
            if (esz != 4 && esz != 8) esz = 8; /* long double: double model */
            int addr = lower_expr(c, arena, node->right);
            int res = c->next_temp_id++;
            int la = addr;
            if (is_imag) {
                int off = c->next_temp_id++;
                ir_emit_mov_imm(off, (int)esz);
                la = c->next_temp_id++;
                ir_emit_add(la, addr, off);
            }
            if (esz == 4) ir_emit_vloadss(res, (uint16_t)la);
            else ir_emit_vloadsd(res, (uint16_t)la);
            c->last_expr_is_float = 1;
            return res;
        }

        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_conj") == 0 ||
             strcmp(callee->s.str, "__builtin_conjf") == 0 ||
             strcmp(callee->s.str, "__builtin_conjl") == 0)) {
            ZeroCCASTNode *cx_node = zero_cc_ast_get(arena, node->right);
            TypeInfo *cx_ti = cx_node ? ast_node_get_typeinfo(c, cx_node) : NULL;
            size_t esz = (cx_ti && cx_ti->kind == TYPE_KIND_COMPLEX &&
                          cx_ti->details.array.base)
                             ? type_size(cx_ti->details.array.base) :
                         (strcmp(callee->s.str, "__builtin_conjf") == 0) ? 4 : 8;
            if (esz != 4 && esz != 8) esz = 8;
            int addr = lower_expr(c, arena, node->right);
            int rh = c->next_temp_id++, li = c->next_temp_id++;
            int off = c->next_temp_id++;
            ir_emit_mov_imm(off, (int)esz);
            int la = c->next_temp_id++;
            ir_emit_add(la, addr, off);
            if (esz == 4) {
                ir_emit_vloadss((uint16_t)rh, (uint16_t)addr);
                ir_emit_vloadss((uint16_t)li, (uint16_t)la);
            } else {
                ir_emit_vloadsd((uint16_t)rh, (uint16_t)addr);
                ir_emit_vloadsd((uint16_t)li, (uint16_t)la);
            }
            int zero_i = c->next_temp_id++;
            ir_emit_mov_imm(zero_i, 0);
            int zero_f = c->next_temp_id++;
            ir_emit_cvtsi2sd((uint16_t)zero_f, (uint16_t)zero_i);
            int neg_li = c->next_temp_id++;
            ir_emit_fsub((uint16_t)neg_li, (uint16_t)zero_f, (uint16_t)li);

            size_t csz = esz * 2;
            c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
            c->current_stack_frame += (int32_t)csz;
            int32_t res_off = c->current_stack_frame;
            int res_addr = c->next_temp_id++;
            ir_emit_lea(res_addr, BE(c)->fp_reg, -res_off);
            int off_m = c->next_temp_id++;
            ir_emit_mov_imm(off_m, (int)esz);
            int res_a2 = c->next_temp_id++;
            ir_emit_add(res_a2, res_addr, off_m);
            if (esz == 4) {
                ir_emit_vstoress((uint16_t)res_addr, (uint16_t)rh);
                ir_emit_vstoress((uint16_t)res_a2, (uint16_t)neg_li);
            } else {
                ir_emit_vstoresd((uint16_t)res_addr, (uint16_t)rh);
                ir_emit_vstoresd((uint16_t)res_a2, (uint16_t)neg_li);
            }
            c->last_expr_is_float = 0;
            return res_addr;
        }

        /* __builtin_expect(cond, val) is a branch-hint builtin: it
         * evaluates to `cond` (the hint is ignored by ZeroCC's codegen).
         * Lower to just the condition expression so no external "expect"
         * call is emitted. */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_expect", 16) == 0) {
            ASTRef expect_arg = node->right;
            int res = lower_expr(c, arena, expect_arg);
            c->last_expr_is_float = 0;
            return res;
        }

        /* __builtin_prefetch(addr, rw, locality) is a cache hint.
         * It is a no-op for ZeroCC: evaluate the arguments (so any
         * side effects occur) but emit no memory access. */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_prefetch", 18) == 0) {
            ASTRef pa = node->right;
            while (pa != AST_NULL) {
                lower_expr(c, arena, pa);
                ZeroCCASTNode *an = zero_cc_ast_get(arena, pa);
                pa = (an && an->next != AST_NULL) ? an->next : AST_NULL;
            }
            c->last_expr_is_float = 0;
            return VREG_0;
        }

        /* __builtin_shuffle(a, b) - lane selection: result[i] =
         * (b[i] < n) ? a[b[i]] : b[b[i]-n], n = lanes of a (power of
         * two). Both operands are n-lane vectors (address form); b is
         * the index vector (any integer element width). GCC tolerates
         * out-of-range index values (folded through the low log2(2n)
         * bits), so lower branchlessly per lane on the raw index bits:
         *   m = b[i] & (n-1)              shared element offset
         *   from_a = ((b[i] & n) == 0)
         *   res = (sel & a[m]) | (~sel & b[m])  bit-level select (valid
         *   for integer and float lanes alike). */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_shuffle") == 0) {
            ASTRef sh_a_ref = node->right;
            ZeroCCASTNode *sh_a = sh_a_ref ? zero_cc_ast_get(arena, sh_a_ref) : NULL;
            ZeroCCASTNode *sh_b = (sh_a && sh_a->next != AST_NULL) ?
                zero_cc_ast_get(arena, sh_a->next) : NULL;
            if (sh_a && sh_b) {
                ZeroCCType sh_a_t;
                memset(&sh_a_t, 0, sizeof(sh_a_t));
                bool have_a_t = false;
                TypeInfo *a_ti = ast_node_get_typeinfo(c, sh_a);
                ZeroCCType sh_el_t;
                if (a_ti && type_is_vector(a_ti)) {
                    sh_a_t.tag = TYPE_USER_0;
                    sh_a_t.size = (int32_t)type_size(a_ti);
                    if (a_ti->details.array.base) {
                        sh_el_t = typeinfo_to_zero_cc_type(a_ti->details.array.base);
                        sh_a_t.base = &sh_el_t;
                    }
                    have_a_t = true;
                } else if (sh_a->tag == NODE_IDENT) {
                    ZeroCCSym *as = zero_cc_find_local(c, sh_a->s.str);
                    ZeroCCGlobalVar *ag = as ? NULL : find_global_var(sh_a->s.str);
                    if (as && as->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&as->type)) {
                        sh_a_t = as->type; have_a_t = true;
                    } else if (ag && ag->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&ag->type)) {
                        sh_a_t = ag->type; have_a_t = true;
                    }
                }
                if (have_a_t && sh_a_t.size >= 1 && sh_a_t.size <= 128) {
                    size_t a_sz = (size_t)sh_a_t.size;
                    const ZeroCCType *ael = sh_a_t.base;
                    size_t elsz = (ael && ael->size > 0) ? ael->size : 4;
                    size_t n = a_sz / (elsz ? elsz : 1);
                    /* index element size from b */
                    size_t idsz = 4;
                    TypeInfo *b_ti = ast_node_get_typeinfo(c, sh_b);
                    if (b_ti && type_is_vector(b_ti) && b_ti->details.array.base) {
                        idsz = type_size(b_ti->details.array.base);
                    } else if (sh_b->tag == NODE_IDENT) {
                        ZeroCCSym *bs = zero_cc_find_local(c, sh_b->s.str);
                        if (bs && bs->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&bs->type) &&
                            bs->type.base && bs->type.base->size > 0) {
                            idsz = bs->type.base->size;
                        }
                    }
                    if (n >= 1 && (n & (n - 1)) == 0 && n <= 16) {
                        int a_addr = lower_expr(c, arena, sh_a_ref);
                        int b_addr = lower_expr(c, arena, sh_a->next);
                        c->current_stack_frame = (c->current_stack_frame + (int32_t)a_sz + 7) & ~7;
                        int32_t res_off = c->current_stack_frame;
                        int64_t bit_n = (int64_t)n;              /* k-th bit */
                        int64_t mask_nm1 = (int64_t)(n - 1);
                        for (size_t i = 0; i < n; i++) {
                            /* idx = b[i] (raw bits, width-correct load) */
                            int bi_off = c->next_temp_id++;
                            if (i == 0) { ir_emit_mov_reg(bi_off, b_addr); }
                            else {
                                int bo = c->next_temp_id++;
                                ir_emit_mov_imm(bo, (int32_t)(i * idsz));
                                ir_emit_add(bi_off, b_addr, bo);
                            }
                            int idx = c->next_temp_id++;
                            if (idsz == 1) { ir_emit_load_byte(idx, bi_off); ir_emit_zext(idx, 1); }
                            else if (idsz == 2) { ir_emit_load_16(idx, bi_off); ir_emit_zext(idx, 2); }
                            else if (idsz == 4) { ir_emit_load_32(idx, bi_off); ir_emit_zext(idx, 4); }
                            else { ir_emit_deref(idx, bi_off); }
                            /* m = idx & (n-1) */
                            int m_mask = c->next_temp_id++;
                            ir_emit_mov_imm(m_mask, mask_nm1);
                            int m = c->next_temp_id++;
                            ir_emit_and(m, idx, m_mask);
                            /* off = m * elsz */
                            int off = c->next_temp_id++;
                            if (elsz > 1) {
                                int es = c->next_temp_id++;
                                ir_emit_mov_imm(es, (int32_t)elsz);
                                ir_emit_mul(off, m, es);
                            } else {
                                ir_emit_mov_reg(off, m);
                            }
                            int aa = c->next_temp_id++;
                            ir_emit_add(aa, a_addr, off);
                            int ba = c->next_temp_id++;
                            ir_emit_add(ba, b_addr, off);
                            int av = c->next_temp_id++;
                            if (elsz == 1) { ir_emit_load_byte(av, aa); ir_emit_zext(av, 1); }
                            else if (elsz == 2) { ir_emit_load_16(av, aa); ir_emit_zext(av, 2); }
                            else if (elsz == 4) { ir_emit_load_32(av, aa); ir_emit_zext(av, 4); }
                            else { ir_emit_deref(av, aa); }
                            int bv = c->next_temp_id++;
                            if (elsz == 1) { ir_emit_load_byte(bv, ba); ir_emit_zext(bv, 1); }
                            else if (elsz == 2) { ir_emit_load_16(bv, ba); ir_emit_zext(bv, 2); }
                            else if (elsz == 4) { ir_emit_load_32(bv, ba); ir_emit_zext(bv, 4); }
                            else { ir_emit_deref(bv, ba); }
                            /* sel = (idx & n) == 0  ->  0/1 */
                            int n_bit = c->next_temp_id++;
                            ir_emit_mov_imm(n_bit, bit_n);
                            int ib = c->next_temp_id++;
                            ir_emit_and(ib, idx, n_bit);
                            int zero_t = c->next_temp_id++;
                            ir_emit_mov_imm(zero_t, 0);
                            ir_emit_cmp((uint16_t)ib, (uint16_t)zero_t);
                            int cmp_t = c->next_temp_id++;
                            ir_emit_cond_set((uint16_t)cmp_t, CC_EQ);
                            int sel = c->next_temp_id++;
                            ir_emit_mov_imm(sel, -1);
                            int selm = c->next_temp_id++;
                            ir_emit_mul(selm, cmp_t, sel);
                            int nv = c->next_temp_id++;
                            int all_ones = c->next_temp_id++;
                            ir_emit_mov_imm(all_ones, -1);
                            ir_emit_xor(nv, selm, all_ones);
                            int s1 = c->next_temp_id++;
                            ir_emit_and(s1, selm, av);
                            int s2 = c->next_temp_id++;
                            ir_emit_and(s2, nv, bv);
                            int lv = c->next_temp_id++;
                            ir_emit_or(lv, s1, s2);
                            /* store lane i into the result temp */
                            int ra = c->next_temp_id++;
                            if (i == 0) {
                                ir_emit_lea(ra, BE(c)->fp_reg, -res_off);
                            } else {
                                ir_emit_lea(ra, BE(c)->fp_reg, -(int32_t)(res_off - (int32_t)(i * elsz)));
                            }
                            if (elsz == 1) ir_emit_store_byte(lv, ra);
                            else if (elsz == 2) ir_emit_store_16(lv, ra);
                            else if (elsz == 4) ir_emit_store_32(lv, ra);
                            else ir_emit_store_64(lv, ra);
                        }
                        int addr = c->next_temp_id++;
                        ir_emit_lea(addr, BE(c)->fp_reg, -res_off);
                        c->last_expr_is_float = 0;
                        return addr;
                    }
                }
            }
        }

        /* __builtin_unreachable() / __builtin_trap() - no value, no libc
         * symbol: emit the target's trap instruction (x86-64 ud2,
         * AArch64 brk #0, RISC-V unimp). Reaching __builtin_unreachable
         * is UB; __builtin_trap guarantees a fault either way. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_unreachable") == 0 ||
             strcmp(callee->s.str, "__builtin_trap") == 0)) {
            ir_emit_trap();
            c->last_expr_is_float = 0;
            return VREG_0;
        }

        /* __builtin_ctz(x) - index of the least significant set bit
         * (0..31). Bit-scan, same shape as __builtin_ffs: result = b
         * when bit b is the first set bit scanning upward. 32-bit
         * scan for all width variants (conservative for _l/_ll). */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_ctz") == 0 ||
             strcmp(callee->s.str, "__builtin_ctzl") == 0 ||
             strcmp(callee->s.str, "__builtin_ctzll") == 0)) {
            ASTRef ct_arg = node->right;
            int t_x = lower_expr(c, arena, ct_arg);
            int t_zero = c->next_temp_id++; ir_emit_mov_imm(t_zero, 0);
            int t_result = c->next_temp_id++; ir_emit_mov_imm(t_result, 0);
            for (int b = 0; b < 32; b++) {
                int t_bit = c->next_temp_id++;
                ir_emit_mov_imm(t_bit, 1);
                int t_bimm = c->next_temp_id++; ir_emit_mov_imm(t_bimm, b);
                ir_emit((Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_bit,
                    .temp_src1=(uint16_t)t_bit,.temp_src2=(uint16_t)t_bimm});
                int t_masked = c->next_temp_id++;
                ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_masked,
                    .temp_src1=(uint16_t)t_x,.temp_src2=(uint16_t)t_bit});
                int t_nonzero = c->next_temp_id++;
                ir_emit((Instr){.op=OP_COND_SET,.temp_dest=(uint16_t)t_nonzero,
                    .temp_src1=(uint16_t)t_masked,.temp_src2=(uint16_t)t_zero,
                    .cond=CC_NE});
                int t_res_zero = c->next_temp_id++;
                ir_emit_cmp(t_result, t_zero);
                ir_emit((Instr){.op=OP_COND_SET,.temp_dest=(uint16_t)t_res_zero,
                    .temp_src1=(uint16_t)t_result,.temp_src2=(uint16_t)t_zero,
                    .cond=CC_EQ});
                int t_set = c->next_temp_id++;
                ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_set,
                    .temp_src1=(uint16_t)t_nonzero,.temp_src2=(uint16_t)t_res_zero});
                int t_val = c->next_temp_id++; ir_emit_mov_imm(t_val, b);
                int t_val_set = c->next_temp_id++;
                ir_emit((Instr){.op=OP_MUL,.temp_dest=(uint16_t)t_val_set,
                    .temp_src1=(uint16_t)t_val,.temp_src2=(uint16_t)t_set});
                ir_emit((Instr){.op=OP_OR,.temp_dest=(uint16_t)t_result,
                    .temp_src1=(uint16_t)t_result,.temp_src2=(uint16_t)t_val_set});
            }
            c->last_expr_is_float = 0;
            return t_result;
        }

        /* __builtin_clz(x) - index of the most significant set bit
         * distance from the top (31..0). Scans downward: result = 31-b
         * for the first set bit from bit 31. 32-bit scan for all
         * width variants (conservative for _l/_ll). */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_clz") == 0 ||
             strcmp(callee->s.str, "__builtin_clzl") == 0 ||
             strcmp(callee->s.str, "__builtin_clzll") == 0)) {
            ASTRef cl_arg = node->right;
            int t_x = lower_expr(c, arena, cl_arg);
            int t_zero = c->next_temp_id++; ir_emit_mov_imm(t_zero, 0);
            int t_result = c->next_temp_id++; ir_emit_mov_imm(t_result, 0);
            for (int b = 31; b >= 0; b--) {
                int t_bit = c->next_temp_id++;
                ir_emit_mov_imm(t_bit, 1);
                int t_bimm = c->next_temp_id++; ir_emit_mov_imm(t_bimm, b);
                ir_emit((Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_bit,
                    .temp_src1=(uint16_t)t_bit,.temp_src2=(uint16_t)t_bimm});
                int t_masked = c->next_temp_id++;
                ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_masked,
                    .temp_src1=(uint16_t)t_x,.temp_src2=(uint16_t)t_bit});
                int t_nonzero = c->next_temp_id++;
                ir_emit((Instr){.op=OP_COND_SET,.temp_dest=(uint16_t)t_nonzero,
                    .temp_src1=(uint16_t)t_masked,.temp_src2=(uint16_t)t_zero,
                    .cond=CC_NE});
                int t_res_zero = c->next_temp_id++;
                ir_emit_cmp(t_result, t_zero);
                ir_emit((Instr){.op=OP_COND_SET,.temp_dest=(uint16_t)t_res_zero,
                    .temp_src1=(uint16_t)t_result,.temp_src2=(uint16_t)t_zero,
                    .cond=CC_EQ});
                int t_set = c->next_temp_id++;
                ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_set,
                    .temp_src1=(uint16_t)t_nonzero,.temp_src2=(uint16_t)t_res_zero});
                int t_val = c->next_temp_id++; ir_emit_mov_imm(t_val, 31 - b);
                int t_val_set = c->next_temp_id++;
                ir_emit((Instr){.op=OP_MUL,.temp_dest=(uint16_t)t_val_set,
                    .temp_src1=(uint16_t)t_val,.temp_src2=(uint16_t)t_set});
                ir_emit((Instr){.op=OP_OR,.temp_dest=(uint16_t)t_result,
                    .temp_src1=(uint16_t)t_result,.temp_src2=(uint16_t)t_val_set});
            }
            c->last_expr_is_float = 0;
            return t_result;
        }

        /* __builtin_popcount(x) - number of set bits (0..32). Bit-scan
         * accumulation: result += (x >> b) & 1 for b in 0..31. 32-bit
         * scan for all width variants (conservative for _l/_ll). */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_popcount") == 0 ||
             strcmp(callee->s.str, "__builtin_popcountl") == 0 ||
             strcmp(callee->s.str, "__builtin_popcountll") == 0)) {
            ASTRef pc_arg = node->right;
            int t_x = lower_expr(c, arena, pc_arg);
            int t_one = c->next_temp_id++; ir_emit_mov_imm(t_one, 1);
            int t_result = c->next_temp_id++; ir_emit_mov_imm(t_result, 0);
            for (int b = 0; b < 32; b++) {
                int t_bit = c->next_temp_id++;
                ir_emit_mov_imm(t_bit, 1);
                int t_bimm = c->next_temp_id++; ir_emit_mov_imm(t_bimm, b);
                ir_emit((Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_bit,
                    .temp_src1=(uint16_t)t_bit,.temp_src2=(uint16_t)t_bimm});
                int t_masked = c->next_temp_id++;
                ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_masked,
                    .temp_src1=(uint16_t)t_x,.temp_src2=(uint16_t)t_bit});
                ir_emit((Instr){.op=OP_ADD,.temp_dest=(uint16_t)t_result,
                    .temp_src1=(uint16_t)t_result,.temp_src2=(uint16_t)t_masked});
            }
            c->last_expr_is_float = 0;
            return t_result;
        }

        /* __builtin_constant_p(expr) returns 1 if expr is a compile-time
         * constant, 0 otherwise (execute/bcp-1). */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_constant_p") == 0) {
            ASTRef cp_arg = node->right;
            int res = zc_node_is_const_expr(c, arena, cp_arg) ? 1 : 0;
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, res);
            c->last_expr_is_float = 0;
            return temp;
        }

        /* __alignof__(E) / _Alignof(E) (C99 6.5.3.4): the alignment of
         * E's type.  A function name operand uses the function's
         * __attribute__((aligned(N))) when present (execute/align-3). */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__alignof__") == 0 ||
             strcmp(callee->s.str, "_Alignof") == 0 ||
             strcmp(callee->s.str, "__builtin_alignof") == 0)) {
            size_t al = 0;
            ZeroCCFunction *aln_fn = NULL;
            if (callee_is_ident && node->right != AST_NULL) {
                ZeroCCASTNode *an = zero_cc_ast_get(arena, node->right);
                if (an && an->tag == NODE_IDENT && an->s.str) {
                    aln_fn = zero_cc_find_function(c, an->s.str);
                }
                if (!aln_fn) {
                    TypeInfo *at = ast_node_get_typeinfo(c, an);
                    if (at) {
                        ZeroCCType aty = typeinfo_to_zero_cc_type(at);
                        if (aty.align > 0) al = aty.align;
                    }
                }
            }
            if (aln_fn) al = aln_fn->align > 0 ? aln_fn->align : 1;
            if (al == 0) al = 8;
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, (int32_t)al);
            c->last_expr_is_float = 0;
            return temp;
        }

        /* __builtin_choose_expr(cond, a, b) - the condition is a
         * constant expression (it may itself contain
         * __builtin_constant_p).  When it folds at compile time, lower
         * ONLY the selected operand (execute/pr19449: the unselected
         * `y++` must not be evaluated).  A genuinely dynamic condition
         * evaluates both operands and selects at runtime. */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_choose_expr") == 0) {
            ASTRef r1 = node->right;
            ZeroCCASTNode *an1 = (r1 != AST_NULL) ? zero_cc_ast_get(arena, r1) : NULL;
            ASTRef r2 = an1 ? an1->next : AST_NULL;
            ZeroCCASTNode *an2 = (r2 != AST_NULL) ? zero_cc_ast_get(arena, r2) : NULL;
            ASTRef r3 = an2 ? an2->next : AST_NULL;
            int64_t cval = 0;
            if (zc_ast_const_eval(c, arena, r1, &cval) &&
                r2 != AST_NULL && r3 != AST_NULL) {
                int sel = lower_expr(c, arena, cval ? r2 : r3);
                return sel;
            }
            /* Dynamic: evaluate both, select with a conditional
             * move (cv != 0 ? bv : av). */
            int cv = lower_expr(c, arena, r1);
            int av = (r2 != AST_NULL) ? lower_expr(c, arena, r2) : 0;
            int bv = (r3 != AST_NULL) ? lower_expr(c, arena, r3) : 0;
            int sel = c->next_temp_id++;
            ir_emit_mov_reg(sel, av);
            int zt = c->next_temp_id++;
            ir_emit_mov_imm(zt, 0);
            ir_emit_cmp(cv, zt);
            int nz = c->next_temp_id++;
            ir_emit_cond_set((uint16_t)nz, CC_NE);
            uint16_t done_lbl = (uint16_t)c->ir_next_label++;
            ir_emit_je((uint16_t)nz, done_lbl);
            ir_emit_mov_reg(sel, bv);
            ir_emit_label(done_lbl);
            c->last_expr_is_float = 0;
            return sel;
        }

        /* __builtin_classify_type(expr) returns the GCC type_class enum code:
         * no_type_class = -1, void = 0, integer = 1, char = 2, enum = 3,
         * boolean = 4, pointer = 5, real = 8, complex = 9, function = 10,
         * record (struct) = 12, union = 13, array = 14. */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_classify_type") == 0) {
            ASTRef ct_arg = node->right;
            ZeroCCASTNode *an = zero_cc_ast_get(arena, ct_arg);
            TypeInfo *ti = an ? ast_node_get_typeinfo(c, an) : NULL;
            int class_id = 1; /* default integer */
            if (ti) {
                if (type_is_floating(ti)) class_id = 8;
                else if (type_is_pointer(ti)) class_id = 5;
                else if (ti->kind == TYPE_KIND_ARRAY) class_id = 14;
                else if (ti->kind == TYPE_KIND_STRUCT) class_id = 12;
                else if (ti->kind == TYPE_KIND_UNION) class_id = 13;
                else if (ti->kind == TYPE_KIND_VOID) class_id = 0;
                else if (ti->kind == TYPE_KIND_BOOL) class_id = 4;
                else if (ti->kind == TYPE_KIND_CHAR) class_id = 2;
                else if (ti->kind == TYPE_KIND_ENUM) class_id = 3;
                else if (ti->kind == TYPE_KIND_FUNCTION) class_id = 10;
                else class_id = 1;
            } else if (an) {
                if (an->tag == NODE_FLOAT) class_id = 8;
                else if (an->tag == NODE_STRING) class_id = 5;
            }
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, class_id);
            c->last_expr_is_float = 0;
            return temp;
        }

        /* __builtin_bswap16(x) - byte-swap the low 16 bits:
         * result = ((x & 0xFF) << 8) | ((x >> 8) & 0xFF).
         * (c-torture execute/pr97386-1) */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_bswap16") == 0) {
            ASTRef b16_arg = node->right;
            int u_a = lower_expr(c, arena, b16_arg);
            int u_ff = c->next_temp_id++; ir_emit_mov_imm(u_ff, 0xFF);
            int u_8  = c->next_temp_id++; ir_emit_mov_imm(u_8,  8);
            int u_b0 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)u_b0,
                .temp_src1=(uint16_t)u_a,.temp_src2=(uint16_t)u_ff});
            int u_a8 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_SHR,.temp_dest=(uint16_t)u_a8,
                .temp_src1=(uint16_t)u_a,.temp_src2=(uint16_t)u_8});
            int u_b1 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)u_b1,
                .temp_src1=(uint16_t)u_a8,.temp_src2=(uint16_t)u_ff});
            int u_b0s = c->next_temp_id++;
            ir_emit((Instr){.op=OP_SHL,.temp_dest=(uint16_t)u_b0s,
                .temp_src1=(uint16_t)u_b0,.temp_src2=(uint16_t)u_8});
            int u_result = c->next_temp_id++;
            ir_emit((Instr){.op=OP_OR,.temp_dest=(uint16_t)u_result,
                .temp_src1=(uint16_t)u_b0s,.temp_src2=(uint16_t)u_b1});
            c->last_expr_is_float = 0;
            return u_result;
        }

        /* __builtin_bswap32(x) - byte-swap a 32-bit value.
         * result = ((x & 0xFF) << 24) | ((x >> 8) & 0xFF) << 16 |
         *          ((x >> 16) & 0xFF) << 8  | (x >> 24) */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_bswap32") == 0) {
            ASTRef b_arg = node->right;
            int t_a = lower_expr(c, arena, b_arg);
            int t_ff = c->next_temp_id++; ir_emit_mov_imm(t_ff, 0xFF);
            int t_8  = c->next_temp_id++; ir_emit_mov_imm(t_8,  8);
            int t_16 = c->next_temp_id++; ir_emit_mov_imm(t_16, 16);
            int t_24 = c->next_temp_id++; ir_emit_mov_imm(t_24, 24);
            int t_b0 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b0,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_ff});
            int t_a8 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_SHR,.temp_dest=(uint16_t)t_a8,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_8});
            int t_b1 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b1,
                .temp_src1=(uint16_t)t_a8,.temp_src2=(uint16_t)t_ff});
            int t_a16 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_SHR,.temp_dest=(uint16_t)t_a16,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_16});
            int t_b2 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b2,
                .temp_src1=(uint16_t)t_a16,.temp_src2=(uint16_t)t_ff});
            int t_a24 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_SHR,.temp_dest=(uint16_t)t_a24,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_24});
            int t_b3 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b3,
                .temp_src1=(uint16_t)t_a24,.temp_src2=(uint16_t)t_ff});
            int t_b0s = c->next_temp_id++;
            ir_emit((Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_b0s,
                .temp_src1=(uint16_t)t_b0,.temp_src2=(uint16_t)t_24});
            int t_b1s = c->next_temp_id++;
            ir_emit((Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_b1s,
                .temp_src1=(uint16_t)t_b1,.temp_src2=(uint16_t)t_16});
            int t_b2s = c->next_temp_id++;
            ir_emit((Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_b2s,
                .temp_src1=(uint16_t)t_b2,.temp_src2=(uint16_t)t_8});
            int t_r1 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_OR,.temp_dest=(uint16_t)t_r1,
                .temp_src1=(uint16_t)t_b0s,.temp_src2=(uint16_t)t_b1s});
            int t_r2 = c->next_temp_id++;
            ir_emit((Instr){.op=OP_OR,.temp_dest=(uint16_t)t_r2,
                .temp_src1=(uint16_t)t_r1,.temp_src2=(uint16_t)t_b2s});
            int t_result = c->next_temp_id++;
            ir_emit((Instr){.op=OP_OR,.temp_dest=(uint16_t)t_result,
                .temp_src1=(uint16_t)t_r2,.temp_src2=(uint16_t)t_b3});
            c->last_expr_is_float = 0;
            return t_result;
        }

        /* __builtin_inf() / __builtin_inff() / __builtin_nan(s) /
         * __builtin_nanf(s): FP special constants (c-torture
         * ieee/copysign1/2). The string argument (when present) is
         * ignored; ZeroCC models float as double, so the result is
         * always an 8-byte double constant - the caller's conversion
         * does any narrowing. Previously emitted as undefined external
         * calls (runtime crash). */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_inf") == 0 ||
             strcmp(callee->s.str, "__builtin_inff") == 0 ||
             strcmp(callee->s.str, "__builtin_infl") == 0 ||
             strcmp(callee->s.str, "__builtin_nan") == 0 ||
             strcmp(callee->s.str, "__builtin_nanf") == 0 ||
             strcmp(callee->s.str, "__builtin_nanl") == 0)) {
            bool is_nan = (strcmp(callee->s.str, "__builtin_nan") == 0 ||
                           strcmp(callee->s.str, "__builtin_nanf") == 0 ||
                           strcmp(callee->s.str, "__builtin_nanl") == 0);
            uint64_t fpbits = is_nan ? 0x7FF8000000000000ULL : 0x7FF0000000000000ULL;
            double fpc; memcpy(&fpc, &fpbits, sizeof(fpc));
            size_t fpad = (8 - (c->rodata_section.length % 8)) % 8;
            if (fpad) {
                uint8_t z8[8] = {0};
                zero_cc_buf_put_bytes(&c->rodata_section, z8, fpad);
            }
            size_t fro_off = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, &fpc, sizeof(double));
            int ft = c->next_temp_id++;
            ir_emit_fmov_imm((uint16_t)ft, (int64_t)fro_off);
            c->last_expr_is_float = 1;
            return ft;
        }

        /* __builtin_bswap64(x) - byte-swap a 64-bit value:
         * byte i of the result is byte (7-i) of x.
         * (c-torture execute/bswap-1.c - previously emitted as an
         * undefined external call and segfaulted at runtime). */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_bswap64") == 0) {
            ASTRef b64_arg = node->right;
            int s_a = lower_expr(c, arena, b64_arg);
            int s_ff = c->next_temp_id++; ir_emit_mov_imm(s_ff, 0xFF);
            int s_b[8];
            int sh[8] = {0, 8, 16, 24, 32, 40, 48, 56};
            int shd[8] = {56, 48, 40, 32, 24, 16, 8, 0};
            for (int i = 0; i < 8; i++) {
                int s_s = c->next_temp_id++; ir_emit_mov_imm(s_s, sh[i]);
                int s_t = c->next_temp_id++;
                ir_emit((Instr){.op=OP_SHR,.temp_dest=(uint16_t)s_t,
                    .temp_src1=(uint16_t)s_a,.temp_src2=(uint16_t)s_s});
                s_b[i] = c->next_temp_id++;
                ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)s_b[i],
                    .temp_src1=(uint16_t)s_t,.temp_src2=(uint16_t)s_ff});
            }
            int s_r = c->next_temp_id++;
            for (int i = 0; i < 8; i++) {
                int s_s = c->next_temp_id++; ir_emit_mov_imm(s_s, shd[i]);
                int s_t = c->next_temp_id++;
                ir_emit((Instr){.op=OP_SHL,.temp_dest=(uint16_t)s_t,
                    .temp_src1=(uint16_t)s_b[i],.temp_src2=(uint16_t)s_s});
                if (i == 0) {
                    ir_emit_mov_reg(s_r, s_t);
                    continue;
                }
                ir_emit((Instr){.op=OP_OR,.temp_dest=(uint16_t)s_r,
                    .temp_src1=(uint16_t)s_r,.temp_src2=(uint16_t)s_t});
            }
            c->last_expr_is_float = 0;
            return s_r;
        }

        /* __builtin_ffs(x) - return 1 + index of least significant set
         * bit (1-indexed), or 0 if x == 0.  Branchless bit scan:
         * result = 0; for i in 0..31: if ((x>>i)&1) && result==0:
         * result = i+1 */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_ffs") == 0 ||
             strcmp(callee->s.str, "__builtin_ffsl") == 0 ||
             strcmp(callee->s.str, "__builtin_ffsll") == 0)) {
            ASTRef f_arg = node->right;
            int t_x = lower_expr(c, arena, f_arg);
            /* t_one = 1 */
            int t_one = c->next_temp_id++; ir_emit_mov_imm(t_one, 1);
            /* t_zero = 0 */
            int t_zero = c->next_temp_id++; ir_emit_mov_imm(t_zero, 0);
            /* result = 0 */
            int t_result = c->next_temp_id++; ir_emit_mov_imm(t_result, 0);
            for (int b = 0; b < 32; b++) {
                /* t_bit = 1 << b */
                int t_bit = c->next_temp_id++;
                ir_emit_mov_imm(t_bit, 1);
                int t_bimm = c->next_temp_id++; ir_emit_mov_imm(t_bimm, b);
                ir_emit((Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_bit,
                    .temp_src1=(uint16_t)t_bit,.temp_src2=(uint16_t)t_bimm});
                /* t_masked = x & (1<<b) */
                int t_masked = c->next_temp_id++;
                ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_masked,
                    .temp_src1=(uint16_t)t_x,.temp_src2=(uint16_t)t_bit});
                /* t_nonzero = (masked != 0) */
                int t_nonzero = c->next_temp_id++;
                ir_emit((Instr){.op=OP_COND_SET,.temp_dest=(uint16_t)t_nonzero,
                    .temp_src1=(uint16_t)t_masked,.temp_src2=(uint16_t)t_zero,
                    .cond=CC_NE});
                /* t_res_zero = (result == 0) */
                int t_res_zero = c->next_temp_id++;
                ir_emit_cmp(t_result, t_zero);
                ir_emit((Instr){.op=OP_COND_SET,.temp_dest=(uint16_t)t_res_zero,
                    .temp_src1=(uint16_t)t_result,.temp_src2=(uint16_t)t_zero,
                    .cond=CC_EQ});
                /* t_set = nonzero && res_zero */
                int t_set = c->next_temp_id++;
                ir_emit((Instr){.op=OP_AND,.temp_dest=(uint16_t)t_set,
                    .temp_src1=(uint16_t)t_nonzero,.temp_src2=(uint16_t)t_res_zero});
                /* t_val = b + 1 */
                int t_val = c->next_temp_id++; ir_emit_mov_imm(t_val, b + 1);
                /* t_val_set = val * set  (val if set, 0 otherwise) */
                int t_val_set = c->next_temp_id++;
                ir_emit((Instr){.op=OP_MUL,.temp_dest=(uint16_t)t_val_set,
                    .temp_src1=(uint16_t)t_val,.temp_src2=(uint16_t)t_set});
                /* result = result | val_set */
                ir_emit((Instr){.op=OP_OR,.temp_dest=(uint16_t)t_result,
                    .temp_src1=(uint16_t)t_result,.temp_src2=(uint16_t)t_val_set});
            }
            /* If x == 0, result should be 0 (all bits clear, loop sets nothing,
             * but result stays 0 anyway).  For x != 0, result = first set bit + 1.
             * The loop above already gives 0 when x == 0 since no t_set is 1. */
            c->last_expr_is_float = 0;
            return t_result;
        }

        /* __builtin_inf() / __builtin_inff() - return infinity. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_inf") == 0 ||
             strcmp(callee->s.str, "__builtin_inff") == 0 ||
             strcmp(callee->s.str, "__builtin_infl") == 0 ||
             strcmp(callee->s.str, "__builtin_huge_val") == 0 ||
             strcmp(callee->s.str, "__builtin_huge_valf") == 0 ||
             strcmp(callee->s.str, "__builtin_huge_vall") == 0)) {
            /* Infinity bit pattern: sign=0, exp=all-ones, mantissa=0 */
            union { double d; int64_t i; } u;
            u.i = 0x7FF0000000000000LL;
            double inf = u.d;
            size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
            if (pad) { uint8_t z8[8] = {0}; zero_cc_buf_put_bytes(&c->rodata_section, z8, pad); }
            size_t rodata_offset = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, &inf, sizeof(double));
            int temp = c->next_temp_id++;
            ir_emit_fmov_imm((uint16_t)temp, (int64_t)rodata_offset);
            c->last_expr_is_float = 1;
            return temp;
        }

        /* __builtin_nan() / __builtin_nanf() - return quiet NaN. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_nan") == 0 ||
             strcmp(callee->s.str, "__builtin_nanf") == 0 ||
             strcmp(callee->s.str, "__builtin_nanl") == 0)) {
            union { double d; int64_t i; } u;
            u.i = 0x7FF8000000000000LL;
            double qnan = u.d;
            size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
            if (pad) { uint8_t z8[8] = {0}; zero_cc_buf_put_bytes(&c->rodata_section, z8, pad); }
            size_t rodata_offset = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, &qnan, sizeof(double));
            int temp = c->next_temp_id++;
            ir_emit_fmov_imm((uint16_t)temp, (int64_t)rodata_offset);
            c->last_expr_is_float = 1;
            return temp;
        }

        /* llabs() / labs() / imaxabs() / abs() - integer absolute value */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_llabs") == 0 ||
             strcmp(callee->s.str, "llabs") == 0 ||
             strcmp(callee->s.str, "__builtin_labs") == 0 ||
             strcmp(callee->s.str, "labs") == 0 ||
             strcmp(callee->s.str, "__builtin_imaxabs") == 0 ||
             strcmp(callee->s.str, "imaxabs") == 0 ||
             strcmp(callee->s.str, "__builtin_abs") == 0 ||
             strcmp(callee->s.str, "abs") == 0)) {
            bool is_32bit = (strcmp(callee->s.str, "__builtin_abs") == 0 ||
                             strcmp(callee->s.str, "abs") == 0);
            int arg_temp = lower_expr(c, arena, node->right);
            if (is_32bit) {
                ir_emit_sext(arg_temp, 4);
            }
            int zero_temp = c->next_temp_id++;
            ir_emit_mov_imm(zero_temp, 0);
            int cond_temp = c->next_temp_id++;
            ir_emit_cmp(arg_temp, zero_temp);
            ir_emit_cond_set(cond_temp, CC_LT);
            uint16_t pos_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;
            ir_emit_je((uint16_t)cond_temp, pos_label);
            int neg_temp = c->next_temp_id++;
            ir_emit_sub(neg_temp, zero_temp, arg_temp);
            int res_temp = c->next_temp_id++;
            ir_emit_mov_reg(res_temp, neg_temp);
            ir_emit_jmp(end_label);
            ir_emit_label(pos_label);
            ir_emit_mov_reg(res_temp, arg_temp);
            ir_emit_label(end_label);
            if (is_32bit) {
                ir_emit_sext(res_temp, 4);
            }
            c->last_expr_is_float = 0;
            return res_temp;
        }

        /* __builtin_fabs() / __builtin_fabsf() - absolute value.
         * Clears the sign bit (bit 63) in IEEE 754 representation. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_fabs") == 0 ||
             strcmp(callee->s.str, "__builtin_fabsf") == 0 ||
             strcmp(callee->s.str, "__builtin_fabsl") == 0)) {
            int arg_temp = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)arg_temp);
                arg_temp = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore((uint16_t)arg_temp, TEMP_NONE, -slot);
            int int_temp = c->next_temp_id++;
            ir_emit_load(int_temp, BE(c)->fp_reg, -slot);
            int mask_temp = c->next_temp_id++;
            ir_emit_mov_imm(mask_temp, 0x7FFFFFFFFFFFFFFFLL);
            int abs_int = c->next_temp_id++;
            ir_emit_and(abs_int, int_temp, mask_temp);
            ir_emit_store(abs_int, BE(c)->fp_reg, -slot);
            int res_temp = c->next_temp_id++;
            ir_emit_fload((uint16_t)res_temp, TEMP_NONE, -slot);
            c->last_expr_is_float = 1;
            return res_temp;
        }

        /* __builtin_isinf() / __builtin_isinff() - infinity test.
         * In IEEE 754, x != 0.0 && (x * 2.0 == x) holds iff x is +/-inf. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_isinf") == 0 ||
             strcmp(callee->s.str, "__builtin_isinff") == 0 ||
             strcmp(callee->s.str, "__builtin_isinfl") == 0)) {
            int arg_temp = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)arg_temp);
                arg_temp = conv;
            }
            int two_i = c->next_temp_id++;
            ir_emit_mov_imm(two_i, 2);
            int two_f = c->next_temp_id++;
            ir_emit_cvtsi2sd((uint16_t)two_f, (uint16_t)two_i);

            int zero_i = c->next_temp_id++;
            ir_emit_mov_imm(zero_i, 0);
            int zero_f = c->next_temp_id++;
            ir_emit_cvtsi2sd((uint16_t)zero_f, (uint16_t)zero_i);

            int mul_temp = c->next_temp_id++;
            ir_emit_fmul((uint16_t)mul_temp, (uint16_t)arg_temp, (uint16_t)two_f);

            int c1 = c->next_temp_id++;
            ir_emit_cmp(arg_temp, zero_f);
            ir_emit_cond_set(c1, CC_NE);

            int c2 = c->next_temp_id++;
            ir_emit_cmp(mul_temp, arg_temp);
            ir_emit_cond_set(c2, CC_EQ);

            int res_temp = c->next_temp_id++;
            ir_emit_and(res_temp, c1, c2);
            c->last_expr_is_float = 0;
            return res_temp;
        }

        /* __builtin_signbit() / __builtin_signbitf() - sign bit test.
         * Returns 1 if sign bit is set (negative), 0 otherwise. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_signbit") == 0 ||
             strcmp(callee->s.str, "__builtin_signbitf") == 0 ||
             strcmp(callee->s.str, "__builtin_signbitl") == 0)) {
            int arg_temp = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)arg_temp);
                arg_temp = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore((uint16_t)arg_temp, TEMP_NONE, -slot);
            int int_temp = c->next_temp_id++;
            ir_emit_load(int_temp, BE(c)->fp_reg, -slot);
            int shift_imm = c->next_temp_id++;
            ir_emit_mov_imm(shift_imm, 63);
            int res_temp = c->next_temp_id++;
            ir_emit_shr(res_temp, int_temp, shift_imm);
            c->last_expr_is_float = 0;
            return res_temp;
        }

        /* IEEE 754-2008 comparison predicates (C11 <math.h>), also
         * available as GCC builtins.  All reduce to ordinary FP
         * compares: in IEEE 754 a relational compare being true
         * already implies both operands are ordered (non-NaN).
         *   isless(x,y)         == x < y
         *   islessequal(x,y)    == x <= y
         *   isgreater(x,y)      == x > y
         *   isgreaterequal(x,y) == x >= y
         *   islessgreater(x,y)  == (x < y) || (x > y)
         *   isunordered(x,y)    == !((x <= y) || (x >= y))
         * Return int 0/1; the f/l width variants are accepted
         * (ZeroCC holds all FP values as 64-bit double).  Note the
         * prefix order below: islessequal/islessgreater must be
         * tested before isless, isgreaterequal before isgreater. */
        if (callee_is_ident && callee->s.str) {
            const char *bn = callee->s.str;
            if (strncmp(bn, "__builtin_islessequal", 19) == 0 ||
                strncmp(bn, "__builtin_islessgreater", 21) == 0 ||
                strncmp(bn, "__builtin_isunordered", 19) == 0 ||
                strncmp(bn, "__builtin_isless", 14) == 0 ||
                strncmp(bn, "__builtin_isgreater", 17) == 0 ||
                strncmp(bn, "__builtin_isgreaterequal", 22) == 0) {
                ASTRef ia1 = node->right;
                ZeroCCASTNode *a1n = zero_cc_ast_get(arena, ia1);
                ASTRef ia2 = (a1n && a1n->next != AST_NULL) ? a1n->next : AST_NULL;
                int t_x = lower_expr(c, arena, ia1);
                if (!c->last_expr_is_float) {
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)t_x);
                    t_x = conv;
                }
                int t_y = lower_expr(c, arena, ia2);
                if (!c->last_expr_is_float) {
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)t_y);
                    t_y = conv;
                }
                int res_temp = c->next_temp_id++;
                if (strncmp(bn, "__builtin_isunordered", 19) == 0) {
                    ir_emit_cmp(t_x, t_y);
                    ir_emit_cond_set(res_temp, 12); /* CC_FP_UNORD */
                } else if (strncmp(bn, "__builtin_islessgreater", 21) == 0) {
                    int t_lt = c->next_temp_id++;
                    ir_emit_cmp(t_x, t_y);
                    ir_emit_cond_set(t_lt, CC_LT);
                    int t_gt = c->next_temp_id++;
                    ir_emit_cmp(t_x, t_y);
                    ir_emit_cond_set(t_gt, CC_GT);
                    ir_emit_or(res_temp, t_lt, t_gt);
                } else if (strncmp(bn, "__builtin_islessequal", 19) == 0) {
                    ir_emit_cmp(t_x, t_y);
                    ir_emit_cond_set(res_temp, CC_LE);
                } else if (strncmp(bn, "__builtin_isgreaterequal", 22) == 0) {
                    ir_emit_cmp(t_x, t_y);
                    ir_emit_cond_set(res_temp, CC_GE);
                } else if (strncmp(bn, "__builtin_isless", 14) == 0) {
                    ir_emit_cmp(t_x, t_y);
                    ir_emit_cond_set(res_temp, CC_LT);
                } else { /* __builtin_isgreater[f/l] */
                    ir_emit_cmp(t_x, t_y);
                    ir_emit_cond_set(res_temp, CC_GT);
                }
                c->last_expr_is_float = 0;
                return res_temp;
            }
        }

        /* __builtin_isnan(x) - NaN is the only value that is not
         * equal to itself. */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_isnan", 13) == 0) {
            int t_x = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int res_temp = c->next_temp_id++;
            ir_emit_cmp(t_x, t_x);
            ir_emit_cond_set(res_temp, 12); /* CC_FP_UNORD (setp) */
            c->last_expr_is_float = 0;
            return res_temp;
        }

        /* __builtin_isfinite(x) - neither inf nor NaN:
         * (x * 0 == 0) && (x == x).  x*0 is NaN for inf/NaN (unordered
         * -> false), 0 for finite (ordered -> true). */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_isfinite", 18) == 0) {
            int t_x = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int zero_i = c->next_temp_id++;
            ir_emit_mov_imm(zero_i, 0);
            int zero_f = c->next_temp_id++;
            ir_emit_cvtsi2sd((uint16_t)zero_f, (uint16_t)zero_i);
            int t_x0 = c->next_temp_id++;
            ir_emit_fmul((uint16_t)t_x0, (uint16_t)t_x, (uint16_t)zero_f);
            int t_zero = c->next_temp_id++;
            ir_emit_cmp(t_x0, zero_f);
            ir_emit_cond_set(t_zero, CC_EQ);
            int t_eq = c->next_temp_id++;
            ir_emit_cmp(t_x, t_x);
            ir_emit_cond_set(t_eq, CC_EQ);
            int res_temp = c->next_temp_id++;
            ir_emit_and(res_temp, t_zero, t_eq);
            c->last_expr_is_float = 0;
            return res_temp;
        }

        /* __builtin_isnormal(x) - neither zero, subnormal, inf, nor
         * NaN: x != 0 && x == x && x/2^30 != x (subnormals flush to 0
         * at /2^30; inf scales to itself). */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_isnormal", 18) == 0) {
            int t_x = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int zero_i = c->next_temp_id++;
            ir_emit_mov_imm(zero_i, 0);
            int zero_f = c->next_temp_id++;
            ir_emit_cvtsi2sd((uint16_t)zero_f, (uint16_t)zero_i);
            int p230_i = c->next_temp_id++;
            ir_emit_mov_imm(p230_i, 1073741824); /* 2^30 */
            int p230_f = c->next_temp_id++;
            ir_emit_cvtsi2sd((uint16_t)p230_f, (uint16_t)p230_i);
            int t_xd = c->next_temp_id++;
            ir_emit_fdiv((uint16_t)t_xd, (uint16_t)t_x, (uint16_t)p230_f);
            int t_nz = c->next_temp_id++;
            ir_emit_cmp(t_x, zero_f);
            ir_emit_cond_set(t_nz, CC_NE);
            int t_ne = c->next_temp_id++;
            ir_emit_cmp(t_xd, t_x);
            ir_emit_cond_set(t_ne, CC_NE);
            int t_eq = c->next_temp_id++;
            ir_emit_cmp(t_x, t_x);
            ir_emit_cond_set(t_eq, CC_EQ);
            int t_a = c->next_temp_id++;
            ir_emit_and(t_a, t_nz, t_ne);
            int res_temp = c->next_temp_id++;
            ir_emit_and(res_temp, t_a, t_eq);
            c->last_expr_is_float = 0;
            return res_temp;
        }

        /* __builtin_copysign(x, y) - magnitude of x with the sign
         * of y (IEEE 754-2008).  Bit surgery: clear x's sign bit and
         * set it from y's. */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_copysign", 16) == 0) {
            ASTRef ca1 = node->right;
            ZeroCCASTNode *ca1n = zero_cc_ast_get(arena, ca1);
            ASTRef ca2 = (ca1n && ca1n->next != AST_NULL) ? ca1n->next : AST_NULL;
            int t_x = lower_expr(c, arena, ca1);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int t_y = lower_expr(c, arena, ca2);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)t_y);
                t_y = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore((uint16_t)t_y, TEMP_NONE, -slot);
            int y_int = c->next_temp_id++;
            ir_emit_load(y_int, BE(c)->fp_reg, -slot);
            int sh63 = c->next_temp_id++;
            ir_emit_mov_imm(sh63, 63);
            int y_sign = c->next_temp_id++;
            ir_emit_shr(y_sign, y_int, sh63);
            int sign_mask = c->next_temp_id++;
            ir_emit_shl(sign_mask, y_sign, sh63);
            ir_emit_fstore((uint16_t)t_x, TEMP_NONE, -slot);
            int x_int = c->next_temp_id++;
            ir_emit_load(x_int, BE(c)->fp_reg, -slot);
            int nosig_mask = c->next_temp_id++;
            ir_emit_mov_imm(nosig_mask, 0x7FFFFFFFFFFFFFFFLL);
            int res_int = c->next_temp_id++;
            ir_emit_and(res_int, x_int, nosig_mask);
            ir_emit_or(res_int, res_int, sign_mask);
            ir_emit_store(res_int, BE(c)->fp_reg, -slot);
            int res_temp = c->next_temp_id++;
            ir_emit_fload((uint16_t)res_temp, TEMP_NONE, -slot);
            c->last_expr_is_float = 1;
            return res_temp;
        }

        /* GCC Integer Overflow Builtins:
         * 1. Value predicates: __builtin_{add,sub,mul}_overflow_p(a, b, (type)0)
         *    Evaluates arguments and returns 0 (conservative: no overflow).
         * 2. Store variants: __builtin_{add,sub,mul}_overflow(a, b, &r)
         *    Computes a OP b (operands normalized to 64 bits), stores the
         *    truncated result to *r, and returns 1 when the mathematical
         *    result does not fit in the type of *r. */
        if (callee_is_ident && callee->s.str &&
            (strstr(callee->s.str, "overflow_p") != NULL ||
             strstr(callee->s.str, "add_overflow") != NULL ||
             strstr(callee->s.str, "sub_overflow") != NULL ||
             strstr(callee->s.str, "mul_overflow") != NULL)) {
            if (strstr(callee->s.str, "overflow_p") != NULL) {
                ASTRef mo_arg = node->right;
                while (mo_arg != AST_NULL) {
                    lower_expr(c, arena, mo_arg);
                    ZeroCCASTNode *an = zero_cc_ast_get(arena, mo_arg);
                    mo_arg = (an && an->next != AST_NULL) ? an->next : AST_NULL;
                }
                int t_ovf = c->next_temp_id++;
                ir_emit_mov_imm(t_ovf, 0);
                c->last_expr_is_float = 0;
                return t_ovf;
            }

            int is_add = (strstr(callee->s.str, "add_overflow") != NULL);
            int is_sub = (strstr(callee->s.str, "sub_overflow") != NULL);
            int is_mul = !(is_add || is_sub);
            ASTRef mo_arg = node->right;
            ASTRef arg_a_ref = mo_arg;
            int t_a = lower_expr(c, arena, mo_arg);
            ZeroCCASTNode *an0 = zero_cc_ast_get(arena, mo_arg);
            mo_arg = (an0 && an0->next != AST_NULL) ? an0->next : AST_NULL;
            ASTRef arg_b_ref = mo_arg;
            int t_b = lower_expr(c, arena, mo_arg);
            ZeroCCASTNode *an1 = zero_cc_ast_get(arena, mo_arg);
            mo_arg = (an1 && an1->next != AST_NULL) ? an1->next : AST_NULL;
            int t_ptr = lower_expr(c, arena, mo_arg);
            ZeroCCASTNode *an2 = zero_cc_ast_get(arena, mo_arg);
            int width = an2 ? zc_atomic_access_width(c, an2) : 4;
            int t_signed = 1;
            /* *r target width/signedness: the argument may be `&x`
             * (address-of) or a bare pointer variable/parameter. */
            ZeroCCASTNode *target = NULL;
            if (an2 && an2->tag == NODE_UNOP && an2->aux.op == TK_AMP) {
                target = zero_cc_ast_get(arena, an2->left);
            } else if (an2 && an2->tag == NODE_IDENT) {
                target = an2;
            }
            if (target && target->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, target->s.str);
                const ZeroCCType *zt = NULL;
                if (sym) zt = &sym->type;
                if (!zt) {
                    ZeroCCGlobalVar *gv = find_global_var(target->s.str);
                    if (gv) zt = &gv->type;
                }
                if (zt) {
                    const ZeroCCType *pt = zt;
                    if (pt->tag == TYPE_PTR && pt->base) pt = pt->base; /* bare pointer arg: look at the pointee */
                    if (pt->size == 1 || pt->size == 2 || pt->size == 4 || pt->size == 8) {
                        width = (int)pt->size;
                        t_signed = !(pt->tag == TYPE_UINT8 || pt->tag == TYPE_UINT16 ||
                                      pt->tag == TYPE_UINT32 || pt->tag == TYPE_UINT64);
                    }
                }
            }
            /* Real overflow detection (replaces the old conservative 0).
             * Each operand is normalized to 64 bits - zero-extended when
             * unsigned, sign-extended when signed - and the mathematical
             * a OP b is formed with 64-bit wrap; the overflow flag is 1
             * when that value cannot be represented in the type of *r
             * (width bytes, signedness t_signed). Mixed signed/unsigned
             * operand pairs use signed 64-bit math (exact for the operand
             * widths that occur in practice). */
            int au = 0, bu = 0;
            int aw = zero_cc_expr_int_info(c, arena, arg_a_ref, &au);
            int bw = zero_cc_expr_int_info(c, arena, arg_b_ref, &bu);
            int use_unsigned_math = (au && bu);
            int t_a64 = c->next_temp_id++;
            ir_emit_mov_reg(t_a64, t_a);
            if (aw < 8) {
                if (au) {
                    int m = c->next_temp_id++;
                    ir_emit_mov_imm(m, (int64_t)((1ULL << (aw * 8)) - 1));
                    ir_emit_and(t_a64, t_a64, m);
                } else {
                    ir_emit_sext(t_a64, aw);
                }
            }
            int t_b64 = c->next_temp_id++;
            ir_emit_mov_reg(t_b64, t_b);
            if (bw < 8) {
                if (bu) {
                    int m = c->next_temp_id++;
                    ir_emit_mov_imm(m, (int64_t)((1ULL << (bw * 8)) - 1));
                    ir_emit_and(t_b64, t_b64, m);
                } else {
                    ir_emit_sext(t_b64, bw);
                }
            }
            int t_res = c->next_temp_id++;
            if (is_add) {
                ir_emit((Instr){.op=OP_ADD,.temp_dest=(uint16_t)t_res,
                    .temp_src1=(uint16_t)t_a64,.temp_src2=(uint16_t)t_b64});
            } else if (is_sub) {
                ir_emit((Instr){.op=OP_SUB,.temp_dest=(uint16_t)t_res,
                    .temp_src1=(uint16_t)t_a64,.temp_src2=(uint16_t)t_b64});
            } else {
                ir_emit((Instr){.op=OP_MUL,.temp_dest=(uint16_t)t_res,
                    .temp_src1=(uint16_t)t_a64,.temp_src2=(uint16_t)t_b64});
            }
            if (width == 1) ir_emit_store_byte(t_res, t_ptr);
            else if (width == 2) ir_emit_store_16(t_res, t_ptr);
            else if (width == 8) ir_emit_store_64(t_res, t_ptr);
            else ir_emit_store_32(t_res, t_ptr);
            int t_ovf;
            if (use_unsigned_math) {
                uint64_t target_hi = (width == 8)
                    ? (t_signed ? (uint64_t)1 << 63 : UINT64_MAX)
                    : ((uint64_t)1 << (t_signed ? (width * 8 - 1) : (width * 8)));
                uint64_t target_lo = t_signed
                    ? ((width == 8) ? (uint64_t)1 << 63 : (uint64_t)(UINT64_MAX - (((uint64_t)1 << (width * 8 - 1)) - 1)))
                    : 0;
                if (is_mul) {
                    /* P = a*b (a,b >= 0) fits the target iff
                     * (b == 0) || (a <= LIMIT / b); the division is
                     * branch-guarded against a zero divisor. */
                    uint64_t limit = t_signed ? (target_hi - 1)
                                              : (width == 8 ? UINT64_MAX : ((uint64_t)1 << (width * 8)) - 1);
                    int t_lim = c->next_temp_id++;
                    ir_emit_mov_imm(t_lim, (int64_t)limit);
                    int t_bz = zc_ovf_cmp_set_imm(c, t_b64, 0, CC_EQ);
                    int lab_no = c->ir_next_label++;
                    ir_emit_jne((uint16_t)t_bz, (uint16_t)lab_no);
                    int t_q = c->next_temp_id++;
                    ir_emit_div(t_q, t_lim, t_b64);
                    ir_mark_last_unsigned(1);
                    int t_gt = zc_ovf_cmp_set(c, t_a64, t_q, CC_FLT_GT);
                    ir_emit_label(lab_no);
                    t_ovf = zc_ovf_and(c, zc_ovf_not1(c, t_bz), t_gt);
                } else if (is_add) {
                    /* carry64 = (res < a) unsigned; the mathematical sum
                     * exceeds the target when carry64 or the top bits
                     * (above the target width) are set. */
                    int t_carry = zc_ovf_cmp_set(c, t_res, t_a64, CC_FLT_LT);
                    if (width < 8) {
                        int t_hi = zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_GE);
                        t_ovf = zc_ovf_or(c, t_carry, t_hi);
                    } else {
                        t_ovf = (t_signed ? zc_ovf_or(c, t_carry, zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_GE)) : t_carry);
                    }
                } else {
                    /* borrow = (a < b) unsigned; the mathematical
                     * difference a-b (negative when borrow) fits the
                     * target per the target's range. */
                    int t_borrow = zc_ovf_cmp_set(c, t_a64, t_b64, CC_FLT_LT);
                    if (!t_signed) {
                        if (width < 8) {
                            t_ovf = zc_ovf_or(c, t_borrow, zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_GE));
                        } else {
                            t_ovf = t_borrow;
                        }
                    } else if (width < 8) {
                        /* -2^(W-1) <= a-b < 2^(W-1): with borrow, res
                         * must be >= 2^64 - 2^(W-1); without, < 2^(W-1). */
                        int t_ge = zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_GE);
                        int t_lt = zc_ovf_cmp_set_imm(c, t_res, target_lo, CC_FLT_LT);
                        t_ovf = zc_ovf_or(c, zc_ovf_and(c, t_borrow, t_lt),
                                          zc_ovf_and(c, zc_ovf_not1(c, t_borrow), t_ge));
                    } else {
                        int t_ge = zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_GE);
                        int t_lt = zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_LT);
                        t_ovf = zc_ovf_or(c, zc_ovf_and(c, t_borrow, t_lt),
                                          zc_ovf_and(c, zc_ovf_not1(c, t_borrow), t_ge));
                    }
                }
            } else {
                /* Signed 64-bit math: t_res is the exact a OP b for the
                 * operand widths in use (|a|,|b| <= 2^32). */
                if (is_mul) {
                    /* Extra wrap guard: if the true product exceeded
                     * 64 bits, res/a != b catches it (division guarded). */
                    int t_az = zc_ovf_cmp_set_imm(c, t_a64, 0, CC_EQ);
                    int lab_no = c->ir_next_label++;
                    ir_emit_jne((uint16_t)t_az, (uint16_t)lab_no);
                    int t_q = c->next_temp_id++;
                    ir_emit_div(t_q, t_res, t_a64);
                    int t_ne = zc_ovf_cmp_set(c, t_q, t_b64, CC_NE);
                    ir_emit_label(lab_no);
                    int t_wrap = zc_ovf_and(c, zc_ovf_not1(c, t_az), t_ne);
                    if (!t_signed) {
                        int t_neg = zc_ovf_cmp_set_imm(c, t_res, 0, CC_LT);
                        t_ovf = zc_ovf_or(c, t_neg, t_wrap);
                        if (width < 8) {
                            t_ovf = zc_ovf_or(c, t_ovf, zc_ovf_cmp_set_imm(c, t_res, ((uint64_t)1 << (width * 8)), CC_FLT_GE));
                        }
                    } else if (width < 8) {
                        int t_lo = zc_ovf_cmp_set_imm(c, t_res, (-((uint64_t)1 << (width * 8 - 1))), CC_LT);
                        int t_hi = zc_ovf_cmp_set_imm(c, t_res, (((uint64_t)1 << (width * 8 - 1)) - 1), CC_GT);
                        t_ovf = zc_ovf_or(c, zc_ovf_or(c, t_lo, t_hi), t_wrap);
                    } else {
                        t_ovf = t_wrap;
                    }
                } else {
                    if (!t_signed) {
                        int t_neg = zc_ovf_cmp_set_imm(c, t_res, 0, CC_LT);
                        t_ovf = t_neg;
                        if (width < 8) {
                            t_ovf = zc_ovf_or(c, t_ovf, zc_ovf_cmp_set_imm(c, t_res, ((uint64_t)1 << (width * 8)), CC_FLT_GE));
                        }
                    } else if (width < 8) {
                        int t_lo = zc_ovf_cmp_set_imm(c, t_res, (-((uint64_t)1 << (width * 8 - 1))), CC_LT);
                        int t_hi = zc_ovf_cmp_set_imm(c, t_res, (((uint64_t)1 << (width * 8 - 1)) - 1), CC_GT);
                        t_ovf = zc_ovf_or(c, t_lo, t_hi);
                    } else {
                        /* 64-bit target: the exact sum/diff fits unless
                         * both operands share the sign that res lost. */
                        if (is_add) {
                            int t1 = zc_ovf_and(c, zc_ovf_cmp_set_imm(c, t_a64, 0, CC_GT),
                                                zc_ovf_cmp_set_imm(c, t_b64, 0, CC_GT));
                            t1 = zc_ovf_and(c, t1, zc_ovf_cmp_set_imm(c, t_res, 0, CC_LT));
                            int t2 = zc_ovf_and(c, zc_ovf_cmp_set_imm(c, t_a64, 0, CC_LT),
                                                zc_ovf_cmp_set_imm(c, t_b64, 0, CC_LT));
                            t2 = zc_ovf_and(c, t2, zc_ovf_cmp_set(c, t_res, 0, CC_GE));
                            t_ovf = zc_ovf_or(c, t1, t2);
                        } else {
                            int t1 = zc_ovf_and(c, zc_ovf_cmp_set_imm(c, t_a64, 0, CC_GT),
                                                zc_ovf_cmp_set_imm(c, t_b64, 0, CC_LT));
                            t1 = zc_ovf_and(c, t1, zc_ovf_cmp_set_imm(c, t_res, 0, CC_LT));
                            int t2 = zc_ovf_and(c, zc_ovf_cmp_set_imm(c, t_a64, 0, CC_LT),
                                                zc_ovf_cmp_set_imm(c, t_b64, 0, CC_GT));
                            t2 = zc_ovf_and(c, t2, zc_ovf_cmp_set(c, t_res, 0, CC_GE));
                            t_ovf = zc_ovf_or(c, t1, t2);
                        }
                    }
                }
            }
            c->last_expr_is_float = 0;
            return t_ovf;
        }

        if (callee_is_ident) {
            /* Intercept __zerocc_syscall (zero_rt OS boundary):
             * __zerocc_syscall(number, a, b, c, d, e, f) - 1..7 integer
             * arguments, returns the kernel result. Lowers to OP_SYSCALL;
             * the backend moves the arguments into the target's syscall
             * argument registers and issues the instruction. The result
             * is in VREG_0 (the ABI return register). */
            if (strcmp(name, "__zerocc_syscall") == 0) {
                int sys_arg_temps[8] = {0};
                int sys_arg_count = 0;
                ASTRef sys_arg = node->right;
                while (sys_arg != AST_NULL && sys_arg_count < 8) {
                    sys_arg_temps[sys_arg_count++] = (int)lower_expr(c, arena, sys_arg);
                    ZeroCCASTNode *an = zero_cc_ast_get(arena, sys_arg);
                    sys_arg = (an && an->next != AST_NULL) ? an->next : AST_NULL;
                }
                if (sys_arg_count == 0) {
                    diag_report_error_at_token(c,
                        "__zerocc_syscall requires a syscall number argument");
                    return VREG_0;
                }
                if (sys_arg_count > 7) {
                    diag_report_error_at_token(c,
                        "__zerocc_syscall takes at most 7 arguments (number + 6 operands)");
                    return VREG_0;
                }
                ir_emit_syscall(sys_arg_count, sys_arg_temps);
                c->last_expr_is_float = 0;
                return VREG_0;
            }

            /* C11/GNU atomic-builtin intercept (table: zc_atomic_builtin_table).
             * Access width comes from the pointer operand's pointee type;
             * narrow integer value operands are extended at the value
             * boundary (C99 6.3.1.2, see lower_extend_narrow). */
            for (size_t zc_at_i = 0;
                 zc_at_i < sizeof(zc_atomic_builtin_table) / sizeof(zc_atomic_builtin_table[0]);
                 zc_at_i++) {
                if (strcmp(name, zc_atomic_builtin_table[zc_at_i].name) != 0) continue;
                ZCAtomicKind kind = zc_atomic_builtin_table[zc_at_i].kind;
                ASTRef a0 = node->right;
                ASTRef a1 = (a0 != AST_NULL) ? zero_cc_ast_get(arena, a0)->next : AST_NULL;
                ASTRef a2 = (a1 != AST_NULL) ? zero_cc_ast_get(arena, a1)->next : AST_NULL;
                ZeroCCASTNode *a0_node = zero_cc_ast_get(arena, a0);
                ZeroCCASTNode *a1_node = (a1 != AST_NULL) ? zero_cc_ast_get(arena, a1) : NULL;
                ZeroCCASTNode *a2_node = (a2 != AST_NULL) ? zero_cc_ast_get(arena, a2) : NULL;
                int width = zc_atomic_access_width(c, a0_node);
                int res_temp = -1;

                if (kind == ZC_AT_FENCE) {
                    /* __sync_synchronize / __atomic_thread_fence: x86-64 TSO
                     * gives full ordering, so MFENCE is a valid (over-strong)
                     * lowering for every memory order, incl. seq_cst. */
                    ir_emit_mfence();
                    res_temp = c->next_temp_id++;
                    ir_emit_mov_imm(res_temp, 0);
                } else if (kind == ZC_AT_IS_LOCK_FREE) {
                    /* C11 7.17.2: aligned integer atomics up to 8 bytes are
                     * lock-free on x86-64. */
                    res_temp = c->next_temp_id++;
                    ir_emit_mov_imm(res_temp, 1);
                } else {
                    int ptr_temp = lower_expr(c, arena, a0);
                    switch (kind) {
                    case ZC_AT_LOAD: {
                        res_temp = c->next_temp_id++;
                        if (width == 1) ir_emit_load_byte(res_temp, ptr_temp);
                        else if (width == 2) ir_emit_load_16(res_temp, ptr_temp);
                        else if (width == 8) ir_emit_deref(res_temp, ptr_temp);
                        else ir_emit_load_32(res_temp, ptr_temp);
                        TypeInfo *pti = ast_node_get_typeinfo(c, a0_node);
                        if (pti && pti->kind == TYPE_KIND_POINTER && pti->details.pointer.base) {
                            lower_extend_narrow(c, res_temp, pti->details.pointer.base);
                        }
                        break;
                    }
                    case ZC_AT_STORE:
                    case ZC_AT_EXCHANGE: {
                        /* (ptr, val, order) for both __atomic_store_n and the
                         * bare __atomic_store (GCC's documented convention).
                         * xchg returns the old value (exchange semantics);
                         * store discards it (returned as a dead temp). */
                        int val_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, val_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        res_temp = c->next_temp_id++;
                        ir_emit_atomic_xchg(res_temp, ptr_temp, val_temp, width);
                        break;
                    }
                    case ZC_AT_STORE_0: {
                        /* __sync_lock_release: store 0 at the pointee. */
                        int zero_temp = c->next_temp_id++;
                        ir_emit_mov_imm(zero_temp, 0);
                        if (width == 1) ir_emit_store_byte(zero_temp, ptr_temp);
                        else if (width == 2) ir_emit_store_16(zero_temp, ptr_temp);
                        else if (width == 8) ir_emit_store_64(zero_temp, ptr_temp);
                        else ir_emit_store_32(zero_temp, ptr_temp);
                        res_temp = zero_temp;
                        break;
                    }
                    case ZC_AT_FETCH_ADD:
                    case ZC_AT_ADD_FETCH:
                    case ZC_AT_FETCH_SUB:
                    case ZC_AT_SUB_FETCH: {
                        int is_sub = (kind == ZC_AT_FETCH_SUB || kind == ZC_AT_SUB_FETCH);
                        int val_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, val_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        if (is_sub) {
                            int neg_temp = c->next_temp_id++;
                            ir_emit_neg(neg_temp, val_temp);
                            val_temp = neg_temp;
                        }
                        int val_copy = c->next_temp_id++;
                        ir_emit_mov_reg(val_copy, val_temp);
                        int old_temp = c->next_temp_id++;
                        ir_emit_atomic_xadd(old_temp, ptr_temp, val_temp, width);
                        if (kind == ZC_AT_FETCH_ADD || kind == ZC_AT_FETCH_SUB) {
                            res_temp = old_temp;
                        } else {
                            res_temp = c->next_temp_id++;
                            if (kind == ZC_AT_ADD_FETCH) ir_emit_add(res_temp, old_temp, val_copy);
                            else if (kind == ZC_AT_SUB_FETCH) ir_emit_add(res_temp, old_temp, val_copy);
                            else ir_emit_sub(res_temp, old_temp, val_copy);
                        }
                        break;
                    }
                    case ZC_AT_FETCH_OR:
                    case ZC_AT_OR_FETCH:
                    case ZC_AT_FETCH_AND:
                    case ZC_AT_AND_FETCH:
                    case ZC_AT_FETCH_XOR:
                    case ZC_AT_XOR_FETCH: {
                        int val_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, val_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        int old_temp = c->next_temp_id++;
                        if (kind == ZC_AT_FETCH_OR || kind == ZC_AT_OR_FETCH) ir_emit_atomic_or(old_temp, ptr_temp, val_temp, width);
                        else if (kind == ZC_AT_FETCH_AND || kind == ZC_AT_AND_FETCH) ir_emit_atomic_and(old_temp, ptr_temp, val_temp, width);
                        else ir_emit_atomic_xor(old_temp, ptr_temp, val_temp, width);
                        if (kind == ZC_AT_OR_FETCH || kind == ZC_AT_AND_FETCH || kind == ZC_AT_XOR_FETCH) {
                            res_temp = c->next_temp_id++;
                            if (kind == ZC_AT_OR_FETCH) ir_emit_or(res_temp, old_temp, val_temp);
                            else if (kind == ZC_AT_AND_FETCH) ir_emit_and(res_temp, old_temp, val_temp);
                            else ir_emit_xor(res_temp, old_temp, val_temp);
                        } else {
                            res_temp = old_temp;
                        }
                        break;
                    }
                    case ZC_AT_CAS_OLD: {
                        /* __sync_val_compare_and_swap(ptr, expected, desired) */
                        int exp_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, exp_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        int des_temp = lower_expr(c, arena, a2);
                        zc_atomic_extend_value(c, des_temp, a2_node, ast_node_get_typeinfo(c, a2_node));
                        res_temp = c->next_temp_id++;
                        ir_emit_atomic_cmpxchg(res_temp, ptr_temp, exp_temp, des_temp, width);
                        break;
                    }
                    case ZC_AT_CAS_BOOL_SYNC: {
                        /* __sync_bool_compare_and_swap(ptr, expected, desired) */
                        int exp_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, exp_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        int des_temp = lower_expr(c, arena, a2);
                        zc_atomic_extend_value(c, des_temp, a2_node, ast_node_get_typeinfo(c, a2_node));
                        res_temp = c->next_temp_id++;
                        ir_emit_atomic_cmpxchg_bool(res_temp, ptr_temp, exp_temp, des_temp, width);
                        break;
                    }
                    case ZC_AT_CAS_BOOL: {
                        /* __atomic_compare_exchange[_n](ptr, &expected, desired, ...) */
                        int exp_ptr_temp = lower_expr(c, arena, a1);
                        int exp_temp = c->next_temp_id++;
                        if (width == 1) ir_emit_load_byte(exp_temp, exp_ptr_temp);
                        else if (width == 2) ir_emit_load_16(exp_temp, exp_ptr_temp);
                        else if (width == 8) ir_emit_deref(exp_temp, exp_ptr_temp);
                        else ir_emit_load_32(exp_temp, exp_ptr_temp);
                        TypeInfo *epti = ast_node_get_typeinfo(c, a1_node);
                        if (epti && epti->kind == TYPE_KIND_POINTER && epti->details.pointer.base) {
                            lower_extend_narrow(c, exp_temp, epti->details.pointer.base);
                        }
                        int des_temp = lower_expr(c, arena, a2);
                        zc_atomic_extend_value(c, des_temp, a2_node, ast_node_get_typeinfo(c, a2_node));
                        int actual_old = c->next_temp_id++;
                        ir_emit_atomic_cmpxchg(actual_old, ptr_temp, exp_temp, des_temp, width);
                        if (width == 1) ir_emit_store_byte(actual_old, exp_ptr_temp);
                        else if (width == 2) ir_emit_store_16(actual_old, exp_ptr_temp);
                        else if (width == 8) ir_emit_store_64(actual_old, exp_ptr_temp);
                        else ir_emit_store_32(actual_old, exp_ptr_temp);
                        res_temp = c->next_temp_id++;
                        ir_emit_cmp(actual_old, exp_temp);
                        ir_emit_cond_set(res_temp, CC_EQ);
                        break;
                    }
                    case ZC_AT_TEST_AND_SET: {
                        /* exchange(ptr, 1), return (old != 0). */
                        int one_temp = c->next_temp_id++;
                        ir_emit_mov_imm(one_temp, 1);
                        int zero_temp = c->next_temp_id++;
                        ir_emit_mov_imm(zero_temp, 0);
                        int old_temp = c->next_temp_id++;
                        ir_emit_atomic_xchg(old_temp, ptr_temp, one_temp, width);
                        res_temp = c->next_temp_id++;
                        ir_emit_cmp(old_temp, zero_temp);
                        ir_emit_cond_set(res_temp, CC_NE);
                        break;
                    }
                    case ZC_AT_CLEAR: {
                        /* xchg(ptr, 0); the old value is dead (void op). */
                        int zero_temp = c->next_temp_id++;
                        ir_emit_mov_imm(zero_temp, 0);
                        res_temp = c->next_temp_id++;
                        ir_emit_atomic_xchg(res_temp, ptr_temp, zero_temp, width);
                        break;
                    }
                    default:
                        break;
                    }
                }
                c->last_expr_is_float = 0;
                return res_temp;
            }
        }

        /* Collect argument temps for both integer and FP args */
            ASTRef arg = node->right;
            int all_arg_temps[IR_CALL_MAX_ARGS];
            int all_arg_is_fp[IR_CALL_MAX_ARGS];
            int all_arg_count = 0;
            int32_t int_spill_offsets[IR_CALL_MAX_ARGS];
            int32_t fp_spill_offsets[IR_CALL_MAX_ARGS];
            uint8_t int_spill_stack_pos[IR_CALL_MAX_ARGS];
            uint8_t fp_spill_stack_pos[IR_CALL_MAX_ARGS];
            int int_arg_idx = 0;
            int fp_arg_idx = 0;
            int param_idx = 0;  /* declaration-order index into callee param_types */

            ZeroCCFunction *callee_fn = callee_is_ident ? zero_cc_find_function(c, name) : NULL;
            ZeroCCSym *callee_sym = (callee_is_ident && !callee_fn) ? zero_cc_find_local(c, name) : NULL;
            /* Indirect call cases: (a) the callee is a local/global
             * function-pointer variable, so the callee is the pointer
             * VALUE stored there; (b) the callee is a general expression
             * (a[i](), p->cb(), ...) - its lowered value IS the function
             * pointer to call through. */
            ZeroCCGlobalVar *callee_gv = (callee_is_ident && !callee_fn && !callee_sym) ? find_global_var(name) : NULL;
            /* An inlined body may call through its own function-pointer
             * parameter by name (`fun(depth)` inside an inlined
             * `g(int (*fun)(int))`): in the caller's scope `fun` is
             * neither a function nor a local, so without this check the
             * call would be emitted as a DIRECT call to a symbol named
             * `fun` (c-torture execute/pr42231, O2 inlining). */
            bool callee_is_inlined_fnptr = false;
            if (callee_is_ident && !callee_fn && c->lowering.inlining.active_depth > 0) {
                for (int p = 0; p < c->lowering.inlining.param_count; p++) {
                    if (c->lowering.inlining.param_names[p] &&
                        c->lowering.inlining.param_is_func_ptr[p] &&
                        strcmp(name, c->lowering.inlining.param_names[p]) == 0) {
                        callee_is_inlined_fnptr = true;
                        break;
                    }
                }
            }
            bool is_indirect_call = !callee_is_ident || (callee_fn == NULL &&
                (callee_sym != NULL || (callee_gv != NULL && callee_gv->type.is_func_ptr) ||
                 callee_is_inlined_fnptr));

            /* Register genuinely-called externs (mapped __builtin_*
             * routines like memcpy/memset/abort/exit, implicit-declaration
             * calls) as external functions so AOT codegen emits a
             * symbol-table entry and a PLT relocation for them. Without
             * this, e.g. __builtin_memcpy() lowers to a `call`
             * relocation whose target symbol is missing from the ELF
             * symbol table, so the system linker leaves `call +0` and
             * the binary segfaults at runtime (Chunk M2 root cause).
             * Runs only for direct calls to names with no known
             * function/local/global entry, so function-pointer
             * variables are never registered as externs.
             * __zerocc_* intrinsics and unmapped __builtin_* names are
             * excluded: the semantic analyzer treats them as implicitly
             * declared functions, and a phantom external entry would
             * pollute later functions' global scope (c-torture
             * compile/fold-popcount-1.c). */
            if (!is_indirect_call && name &&
                c->function_count < ZERO_CC_MAX_FUNCTIONS &&
                !zero_cc_find_function(c, name) &&
                strncmp(name, "__zerocc_", 9) != 0 &&
                strncmp(name, "__builtin_", 10) != 0) {
                ZeroCCFunction *fn = &c->functions[c->function_count++];
                memset(fn, 0, sizeof(*fn));
                snprintf(fn->name, sizeof(fn->name), "%s", name);
                fn->text_offset = 0;
                /* return type is unused for external prototypes without an
                 * inline body; zero-initialized to TYPE_VOID by memset above. */
                fn->returns_struct = false;
                fn->ret_struct_sz = 0;
                fn->is_prototype = true;
                fn->has_prototype = false;
                fn->is_external = true;
                fn->is_variadic = (strcmp(name, "printf") == 0 ||
                                   strcmp(name, "fprintf") == 0 ||
                                   strcmp(name, "sprintf") == 0 ||
                                   strcmp(name, "snprintf") == 0);
                fn->noreturn = (strcmp(name, "abort") == 0 ||
                                strcmp(name, "exit") == 0);
                fn->is_inline = false;
                fn->is_static = false;
                fn->can_inline = false;
                fn->inline_body = AST_NULL;
                fn->ast_arena = NULL;
                fn->param_count = 0;
                fn->param_names = NULL;
                fn->param_types = NULL;
                fn->sret_stack_offset = 0;
            }

            /* Small Function Inlining: Expand candidate body directly at call site */
            if (callee_fn && callee_fn->can_inline && callee_fn->inline_body != AST_NULL &&
                c->lowering.inlining.active_depth < 2 &&
                (c->opt_level >= OPT_LEVEL_O2)) {
                ZeroCCASTArena *callee_arena = (ZeroCCASTArena *)callee_fn->ast_arena;
                if (callee_arena) {
                    /* 1. Evaluate arguments */
                    int inlined_param_temps[8];
                    int p_idx = 0;
                    ASTRef a_ref = node->right;
                    while (a_ref != AST_NULL && p_idx < (int)callee_fn->param_count && p_idx < 8) {
                        int a_temp = lower_expr(c, arena, a_ref);
                        /* Extend integer argument if narrow */
                        if (!c->last_expr_is_float) {
                            zero_cc_emit_int_extend(c, a_temp, &callee_fn->param_types[p_idx]);
                        }
                        inlined_param_temps[p_idx++] = a_temp;
                        ZeroCCASTNode *an = zero_cc_ast_get(arena, a_ref);
                        a_ref = an ? an->next : AST_NULL;
                    }

                    /* 2. Clone callee body AST into caller's current arena */
                    ASTRef cloned_body = zero_cc_ast_clone(callee_arena, (ASTRef)callee_fn->inline_body, arena);
                    if (cloned_body != AST_NULL) {
                        int inline_res_temp = c->next_temp_id++;
                        uint16_t inline_exit_label = c->ir_next_label++;

                        /* Save previous inlining context */
                        int prev_bound = c->local_lookup_bound;
                        int prev_depth = c->lowering.inlining.active_depth;
                        int prev_ret_temp = c->lowering.inlining.return_target_temp;
                        uint16_t prev_ret_label = c->lowering.inlining.return_target_label;
                        ZeroCCType prev_ret_type = c->lowering.inlining.return_type;
                        int prev_param_count = c->lowering.inlining.param_count;
                        const char *prev_param_names[8];
                        int prev_param_temps[8];
                        bool prev_param_is_func_ptr[8];
                        for (int p = 0; p < 8; p++) {
                            prev_param_names[p] = c->lowering.inlining.param_names[p];
                            prev_param_temps[p] = c->lowering.inlining.param_temps[p];
                            prev_param_is_func_ptr[p] = c->lowering.inlining.param_is_func_ptr[p];
                        }

                        /* Setup new inlining context */
                        c->lowering.inlining.active_depth = prev_depth + 1;
                        c->lowering.inlining.return_target_temp = inline_res_temp;
                        c->lowering.inlining.return_target_label = inline_exit_label;
                        c->lowering.inlining.return_type = callee_fn->return_type;
                        c->lowering.inlining.param_count = (int)callee_fn->param_count;
                        for (int p = 0; p < (int)callee_fn->param_count && p < 8; p++) {
                            c->lowering.inlining.param_names[p] = callee_fn->param_names[p];
                            c->lowering.inlining.param_temps[p] = inlined_param_temps[p];
                            c->lowering.inlining.param_is_func_ptr[p] =
                                (callee_fn->param_types &&
                                 callee_fn->param_types[p].is_func_ptr);
                        }

                        /* Lower inlined body statements into IR */
                        lower_node(c, arena, cloned_body);
                        ir_emit_label(inline_exit_label);

                        /* Restore previous inlining context */
                        c->local_lookup_bound = prev_bound;
                        c->lowering.inlining.active_depth = prev_depth;
                        c->lowering.inlining.return_target_temp = prev_ret_temp;
                        c->lowering.inlining.return_target_label = prev_ret_label;
                        c->lowering.inlining.return_type = prev_ret_type;
                        c->lowering.inlining.param_count = prev_param_count;
                        for (int p = 0; p < 8; p++) {
                            c->lowering.inlining.param_names[p] = prev_param_names[p];
                            c->lowering.inlining.param_temps[p] = prev_param_temps[p];
                            c->lowering.inlining.param_is_func_ptr[p] = prev_param_is_func_ptr[p];
                        }

                        c->last_expr_is_float = (callee_fn->return_type.tag == TYPE_FLOAT ||
                                                 callee_fn->return_type.tag == TYPE_DOUBLE);
                        return inline_res_temp;
                    }
                }
            }

            /* Determine return type of the call */
            ZeroCCType call_ret_type;
            memset(&call_ret_type, 0, sizeof(call_ret_type));
            if (callee_fn) {
                call_ret_type = callee_fn->return_type;
            } else if (callee_sym && callee_sym->type.tag == TYPE_PTR && callee_sym->type.base) {
                call_ret_type = *callee_sym->type.base;
            } else if (callee_gv && callee_gv->type.tag == TYPE_PTR && callee_gv->type.base) {
                call_ret_type = *callee_gv->type.base;
            } else {
                /* General-expression callee (p->f(), arr[i].g(), ...):
                 * derive the signature from the callee's own type when
                 * it is (a pointer to) a function - without this the
                 * return type defaults to int and an FP return is read
                 * from RAX instead of XMM0 (c-torture execute/ieee/
                 * fp-cmp-8: `tests[j].pos(x, y, 1.0, 2.0)` passed as an
                 * argument to printf). */
                TypeInfo *c_ti = ast_node_get_typeinfo(c, zero_cc_ast_get(arena, node->left));
                if (c_ti && c_ti->kind == TYPE_KIND_POINTER &&
                    c_ti->details.pointer.base &&
                    c_ti->details.pointer.base->kind == TYPE_KIND_FUNCTION) {
                    call_ret_type = typeinfo_to_zero_cc_type(
                        c_ti->details.pointer.base->details.function.ret);
                } else if (c_ti && c_ti->kind == TYPE_KIND_FUNCTION) {
                    call_ret_type = typeinfo_to_zero_cc_type(c_ti->details.function.ret);
                } else {
                    TypeInfo *ret_ti = ast_node_get_typeinfo(c, node);
                    if (ret_ti) {
                        call_ret_type = typeinfo_to_zero_cc_type(ret_ti);
                    }
                }
            }

            /* Spill indirect callee pointer if needed */
            int32_t indirect_callee_spill_off = 0;
            if (is_indirect_call) {
                int callee_ptr_temp = lower_expr(c, arena, node->left);
                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                c->current_stack_frame += 8;
                indirect_callee_spill_off = c->current_stack_frame;
                ir_emit_store(callee_ptr_temp, BE(c)->fp_reg, -indirect_callee_spill_off);
            }

            /* ABI stream positions: integer arg slots and FP arg slots are
             * each consumed in argument order; a hidden sret pointer takes
             * the first integer slot on x86-64/RISC-V (dedicated x8 on
             * AArch64). Args beyond the register slots go to the call
             * stack in argument order - stack_pos records that position
             * (255 = register arg). */
            int gp_regs_avail = BE(c)->conv->int_arg_reg_count;
            int fp_regs_avail = BE(c)->conv->fp_arg_reg_count;
            int gp_stream = 0, fp_stream = 0, stack_arg_count = 0;
            bool callee_uses_sret = ((call_ret_type.tag == TYPE_STRUCT || call_ret_type.tag == TYPE_UNION) ||
                (call_ret_type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&call_ret_type))) &&
                (call_ret_type.size > 16);
            if (callee_uses_sret && c->isa != ISA_AARCH64) gp_stream = 1;  /* hidden sret ptr takes GP slot 0 */

            while (arg != AST_NULL) {
                ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                int temp = lower_expr(c, arena, arg);
                if (callee_fn && param_idx < (int)callee_fn->param_count) {
                    const ZeroCCType *pt = &callee_fn->param_types[param_idx];
                    bool param_is_fp = (pt->tag == TYPE_FLOAT || pt->tag == TYPE_DOUBLE);
                    if (pt->is_complex) {
                        /* complex argument: temp is the object's address;
                             * marshaling happens below (two FP slots). */
                    } else if (param_is_fp && !c->last_expr_is_float) {
                        int conv_temp = c->next_temp_id++;
                        ir_emit_cvtsi2sd((uint16_t)conv_temp, (uint16_t)temp);
                        temp = conv_temp;
                        c->last_expr_is_float = 1;
                    } else if (!param_is_fp && c->last_expr_is_float) {
                        int conv_temp = c->next_temp_id++;
                        ir_emit_cvttsd2si((uint16_t)conv_temp, (uint16_t)temp);
                        zero_cc_emit_int_extend(c, conv_temp, pt);
                        temp = conv_temp;
                        c->last_expr_is_float = 0;
                    } else if (!param_is_fp && !c->last_expr_is_float) {
                        zero_cc_emit_int_extend(c, temp, pt);
                    }
                }
                param_idx++;
                /* C99 complex argument: passed in TWO consecutive FP
                 * registers (real, then imaginary). The argument
                 * expression lowered to the ADDRESS of the complex
                 * object; load the halves and spill them to the FP
                 * stream. */
                {
                    bool param_is_cx = (callee_fn && param_idx < (int)callee_fn->param_count &&
                                        zero_cc_type_is_complex(&callee_fn->param_types[param_idx - 1]));
                    ZeroCCASTNode *cx_an = arg_node;
                    TypeInfo *cx_ati = cx_an ? ast_node_get_typeinfo(c, cx_an) : NULL;
                    bool arg_is_cx = (cx_ati && cx_ati->kind == TYPE_KIND_COMPLEX);
                    if (param_is_cx || arg_is_cx) {
                        size_t cesz = (cx_ati && cx_ati->details.array.base) ? type_size(cx_ati->details.array.base) :
                                       (param_is_cx && callee_fn->param_types[param_idx - 1].base) ? callee_fn->param_types[param_idx - 1].base->size : 8;
                        if (cesz != 4 && cesz != 8) cesz = 8;
                        for (int half = 0; half < 2; half++) {
                            int hv = c->next_temp_id++;
                            int la = temp;
                            if (half == 1) {
                                int o2 = c->next_temp_id++;
                                ir_emit_mov_imm(o2, (int)cesz);
                                la = c->next_temp_id++;
                                ir_emit_add(la, temp, o2);
                            }
                            if (cesz == 4) ir_emit_vloadss((uint16_t)hv, (uint16_t)la);
                            else ir_emit_vloadsd((uint16_t)hv, (uint16_t)la);
                            fp_spill_stack_pos[fp_arg_idx] = (fp_stream < fp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                            fp_stream++;
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            fp_spill_offsets[fp_arg_idx] = c->current_stack_frame;
                            if (cesz == 4 && fp_spill_stack_pos[fp_arg_idx] < 255) {
                                /* Stack-passed float half: the slot must
                                 * hold the FLOAT bits (low 4 bytes) -
                                 * the callee reads it as a float.  A
                                 * double-width store would leave the
                                 * double's low 32 bits, which are not
                                 * the float pattern. */
                                int sa = c->next_temp_id++;
                                ir_emit_lea(sa, BE(c)->fp_reg, -fp_spill_offsets[fp_arg_idx]);
                                ir_emit_vstoress((uint16_t)sa, (uint16_t)hv);
                            } else {
                                ir_emit_fstore((uint16_t)hv, TEMP_NONE, -fp_spill_offsets[fp_arg_idx]);
                            }
                            fp_arg_idx++;
                        }
                        arg = arg_node ? arg_node->next : AST_NULL;
                        continue;
                    }
                }
                bool is_callee_variadic = (callee_fn && callee_fn->is_variadic) ||
                    (name && (strcmp(name, "printf") == 0 || strcmp(name, "sprintf") == 0 ||
                              strcmp(name, "snprintf") == 0 || strcmp(name, "fprintf") == 0));
                int named_params = callee_fn ? (int)callee_fn->param_count :
                    (name && strcmp(name, "printf") == 0 ? 1 :
                     (name && (strcmp(name, "sprintf") == 0 || strcmp(name, "fprintf") == 0) ? 2 :
                      (name && strcmp(name, "snprintf") == 0 ? 3 : 0)));
                bool is_var_arg = is_callee_variadic && (param_idx > named_params);

                if (c->last_expr_is_float && (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) && is_var_arg) {
                    /* In RISC-V psABI, variadic FP arguments are passed in integer registers.
                     * Spill the float temp into a stack slot, then pass that slot as an integer argument. */
                    int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                    gp_stream++;
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    int32_t slot = c->current_stack_frame;
                    ir_emit_fstore((uint16_t)temp, TEMP_NONE, -slot);
                    if (int_arg_idx < IR_CALL_MAX_ARGS) {
                        int_spill_offsets[int_arg_idx] = slot;
                        int_arg_idx++;
                    }
                } else if (c->last_expr_is_float) {
                    /* Float argument: spill it to a stack slot via IR. The first
                     * fp_regs_avail go into FP registers (stream order); the rest
                     * are stack-passed in argument order (stack_pos) - the call
                     * site pushes them from this same spill slot. */
                    fp_spill_stack_pos[fp_arg_idx] = (fp_stream < fp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                    fp_stream++;
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    fp_spill_offsets[fp_arg_idx] = c->current_stack_frame;
                    ir_emit_fstore((uint16_t)temp, TEMP_NONE, -fp_spill_offsets[fp_arg_idx]);
                    fp_arg_idx++;
                } else {
                    size_t arg_struct_sz = 0;
                    ZeroCCSym *arg_struct_sym = NULL;
                    if (arg_node && arg_node->tag == NODE_IDENT) {
                        arg_struct_sym = zero_cc_find_local(c, arg_node->s.str);
                        if (arg_struct_sym && (arg_struct_sym->type.tag == TYPE_STRUCT ||
                                               arg_struct_sym->type.tag == TYPE_UNION ||
                                               (arg_struct_sym->type.tag == TYPE_USER_0 &&
                                                zero_cc_type_is_vector(&arg_struct_sym->type)))) {
                            arg_struct_sz = arg_struct_sym->type.size;
                        } else if (!arg_struct_sym) {
                            ZeroCCGlobalVar *gv = find_global_var(arg_node->s.str);
                            if (gv && (gv->type.tag == TYPE_STRUCT || gv->type.tag == TYPE_UNION ||
                                       (gv->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&gv->type)))) {
                                arg_struct_sz = gv->type.size;
                            }
                        }
                    } else if (arg_node) {
                        TypeInfo *arg_ti = ast_node_get_typeinfo(c, arg_node);
                        if (arg_ti && (arg_ti->kind == TYPE_KIND_STRUCT || arg_ti->kind == TYPE_KIND_UNION ||
                                       type_is_vector(arg_ti))) {
                            arg_struct_sz = type_size(arg_ti);
                        }
                    }
                    /* A vector argument <=16B passes as 8-byte chunk(s),
                     * exactly like a 9-16B struct; the vector IDENT /
                     * binop temp / compound literal all yield an address
                     * the chunks are read from. */
                    bool is_mem_struct = (arg_struct_sz > 0 &&
                                          (ast_expr_is_struct_addr(c, arena, arg) ||
                                           ast_expr_is_vector_addr(c, arena, arg)));
                    int mem_struct_addr = is_mem_struct ? temp : TEMP_NONE;
                    /* SysV ABI: a by-value struct/union argument LARGER
                     * than 16 bytes is passed by hidden pointer (one GP
                     * slot), not as a chunk stream - a 128KB struct must
                     * not become 16K register/stack arguments (c-torture
                     * execute/pr23135). The caller copies the value into
                     * a stack buffer and passes its address; the callee
                     * prologue reads the pointer back and copies.
                     * Vector arguments keep the chunk convention. */
                    bool arg_is_struct_u = false;
                    if (arg_node && arg_node->tag == NODE_IDENT && arg_struct_sym) {
                        arg_is_struct_u = (arg_struct_sym->type.tag == TYPE_STRUCT ||
                                           arg_struct_sym->type.tag == TYPE_UNION);
                    } else if (arg_node) {
                        TypeInfo *ati = ast_node_get_typeinfo(c, arg_node);
                        arg_is_struct_u = (arg_struct_sz > 0 && ati &&
                                           (ati->kind == TYPE_KIND_STRUCT ||
                                            ati->kind == TYPE_KIND_UNION));
                    }
                    bool big_struct_ptr_conv = false;
                    int big_src_addr = TEMP_NONE;
                    if (arg_struct_sz > 16 && arg_is_struct_u) {
                        if (arg_node && arg_node->tag == NODE_IDENT && arg_struct_sym) {
                            big_src_addr = c->next_temp_id++;
                            ir_emit_lea(big_src_addr, BE(c)->fp_reg, -arg_struct_sym->stack_offset);
                            big_struct_ptr_conv = true;
                        } else if (arg_node && arg_node->tag == NODE_IDENT && !arg_struct_sym) {
                            ZeroCCGlobalVar *bgv = find_global_var(arg_node->s.str);
                            if (bgv && (bgv->type.tag == TYPE_STRUCT || bgv->type.tag == TYPE_UNION)) {
                                big_src_addr = c->next_temp_id++;
                                ir_emit_global_addr(c, big_src_addr, bgv);
                                big_struct_ptr_conv = true;
                            }
                        } else if (is_mem_struct) {
                            /* call result / compound literal / deref / index:
                             * `temp` already holds the value's address. */
                            big_src_addr = temp;
                            big_struct_ptr_conv = true;
                        }
                    }
                    /* glibc x86-64 va_list shadow: ZeroCC's private
                     * 5-pointer va_list is unreadable by libc's variadic
                     * functions (vprintf/vfprintf/...), which expect the
                     * 4-field glibc layout (u32 gp_offset, u32 fp_offset,
                     * overflow_arg_area, reg_save_area) with a 176-byte
                     * register save region: GP slots 0..5 at +0..+47, FP
                     * slots 0..7 as 16-byte XMM words at +48..+175,
                     * growing UP from the RDI slot (slot k at +8k).
                     * ZeroCC's prologue spill grows DOWN from slot 0
                     * (slot k at base-8k) and stores FP as 8-byte
                     * doubles at 8B spacing, so the shadow materializes
                     * a glibc-ordered COPY of the region: GP slot l at
                     * region+8l, FP slot l at region+48+16l (low 8 bytes
                     * of the double, high 8 bytes zeroed; FP slots 6..7
                     * zero-filled). Buffer = 32B header + 176B region
                     * (6 GP x 8 + 8 FP x 16, the glibc size).
                     *  - gp_offset = save_abs - gp_ptr, always in [0,48]
                     *    (48 == the glibc "exhausted" value);
                     *  - fp_offset = 48 + 16 * (FP slots consumed)
                     *    (= 48 + 2 * (gp_end - fp_ptr)), always in
                     *    [48,176]; the zero-filled slots 6..7 make the
                     *    exhausted value read 0.0 cleanly;
                     *  - overflow_arg_area = the zerocc overflow pointer
                     *    (the caller's stack args).  NOTE: glibc steps
                     *    the overflow pointer by 16 while ZeroCC's
                     *    caller packs stack args at 8B spacing - overflow
                     *    (more than 6 GP or 6 FP variadics) to an extern
                     *    is unsupported; the tested printf-family cases
                     *    fit in the register region.
                     * Zerocc-compiled callees keep the native layout. */
                    TypeInfo *vati = arg_node ? ast_node_get_typeinfo(c, arg_node) : NULL;
                    bool arg_is_vl = (vati && vati->kind == TYPE_KIND_STRUCT &&
                                      vati->details.record.tag &&
                                      strcmp(vati->details.record.tag, "va_list") == 0 &&
                                      vati->details.record.size > 16);
                    bool callee_extern_vl = (callee_fn != NULL && callee_fn->is_prototype);
                    bool emit_vl_shadow = (arg_is_vl && callee_extern_vl && c->isa == ISA_X86_64);
                    if (big_struct_ptr_conv) {
                        c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                        int32_t asz = emit_vl_shadow ? 208 : (int32_t)((arg_struct_sz + 7) & ~(size_t)7);
                        c->current_stack_frame += asz;
                        int buf_t = c->next_temp_id++;
                        ir_emit_lea(buf_t, BE(c)->fp_reg, -c->current_stack_frame);
                        if (emit_vl_shadow) {
                            /* The save region lives in the frame of the
                             * ORIGINAL variadic function (the va_list may
                             * be forwarded through wrapper frames, e.g.
                             * __vprintf_chk -> vprintf), so derive the
                             * slot-0 base from the va_list itself:
                             * gp_end (ap+8) = slot0 - 48 (6 GP x 8), so
                             * slot0 = gp_end + 48. */
                            int o8a = c->next_temp_id++;
                            ir_emit_mov_imm(o8a, 8);
                            int ap8a = c->next_temp_id++;
                            ir_emit_add(ap8a, big_src_addr, o8a);
                            int gpend = c->next_temp_id++;
                            ir_emit_deref(gpend, ap8a);        /* *(ap+8) */
                            int o48a = c->next_temp_id++;
                            ir_emit_mov_imm(o48a, 48);
                            int save_abs = c->next_temp_id++;
                            ir_emit_add(save_abs, gpend, o48a);
                            /* region base = buf + 32 */
                            int o32h = c->next_temp_id++;
                            ir_emit_mov_imm(o32h, 32);
                            int reg_base = c->next_temp_id++;
                            ir_emit_add(reg_base, buf_t, o32h);
                            /* GP region copy: origin slot l at
                             * save_abs - 8l -> region + 8l. */
                            for (int l = 0; l < 6; l++) {
                                int sv = c->next_temp_id++;
                                int nl = c->next_temp_id++;
                                ir_emit_mov_imm(nl, -8 * l);
                                ir_emit_add(sv, save_abs, nl);
                                int val = c->next_temp_id++;
                                ir_emit_deref(val, sv);
                                if (l == 0) {
                                    ir_emit_store_64((uint16_t)val, (uint16_t)reg_base);
                                } else {
                                    int ol = c->next_temp_id++;
                                    ir_emit_mov_imm(ol, 8 * l);
                                    int dl = c->next_temp_id++;
                                    ir_emit_add(dl, reg_base, ol);
                                    ir_emit_store_64((uint16_t)val, (uint16_t)dl);
                                }
                            }
                            /* FP region copy: origin slot l (8-byte
                             * double) at save_abs - 48 - 8l (= gp_end -
                             * 8l) -> region + 48 + 16l, high 8 bytes
                             * zeroed.  glibc sizes the region for 8 XMMs,
                             * so slots 6..7 are zero-filled: an exhausted
                             * FP stream (fp_offset 144..176) then reads
                             * 0.0 from the shadow instead of running past
                             * the buffer. */
                            for (int l = 0; l < 8; l++) {
                                int val;
                                if (l < 6) {
                                    int sv = c->next_temp_id++;
                                    int nl = c->next_temp_id++;
                                    ir_emit_mov_imm(nl, -8 * l);
                                    ir_emit_add(sv, gpend, nl);
                                    val = c->next_temp_id++;
                                    ir_emit_deref(val, sv);
                                } else {
                                    val = c->next_temp_id++;
                                    ir_emit_mov_imm(val, 0);
                                }
                                int ol = c->next_temp_id++;
                                ir_emit_mov_imm(ol, 48 + 16 * l);
                                int dl = c->next_temp_id++;
                                ir_emit_add(dl, reg_base, ol);
                                ir_emit_store_64((uint16_t)val, (uint16_t)dl);
                                int z0 = c->next_temp_id++;
                                ir_emit_mov_imm(z0, 0);
                                int oh = c->next_temp_id++;
                                ir_emit_mov_imm(oh, 8);
                                int dh = c->next_temp_id++;
                                ir_emit_add(dh, dl, oh);
                                ir_emit_store_64((uint16_t)z0, (uint16_t)dh);
                            }
                            /* gp_offset = (save_abs - gp_ptr): always in
                             * [0, 48] (gp_ptr runs from slot 0 down to
                             * gp_end = slot 5), and 48 IS the exhausted
                             * value glibc needs (48+8 > 48 -> overflow),
                             * so no clamp/branch is required.  Branchy
                             * clamp patterns here were fragile: at -O1 the
                             * redundant-move elimination/LRA dropped the
                             * non-exhausted restore and stored (gpd-48) -
                             * vprintf-1 printed (null)/segfaulted. */
                            int gp0 = c->next_temp_id++;
                            ir_emit_deref(gp0, big_src_addr);       /* *(ap+0) */
                            int gpd = c->next_temp_id++;
                            ir_emit_sub(gpd, save_abs, gp0);        /* 8*idx, 0..48 */
                            ir_emit_store_32((uint16_t)gpd, (uint16_t)buf_t);
                            /* fp_offset = 48 + 16*(FP consumed) = 48 +
                             * 2*(gp_end - fp_ptr), always in [48, 176];
                             * the 8-slot zero-filled region above makes
                             * the exhausted value (176) read 0.0 cleanly -
                             * no branch needed. */
                            int o16a = c->next_temp_id++;
                            ir_emit_mov_imm(o16a, 16);
                            int ap16a = c->next_temp_id++;
                            ir_emit_add(ap16a, big_src_addr, o16a);
                            int fp0 = c->next_temp_id++;
                            ir_emit_deref(fp0, ap16a);              /* *(ap+16) */
                            int fpd = c->next_temp_id++;
                            ir_emit_sub(fpd, gpend, fp0);           /* 8*idx, 0..48 */
                            int fpo = c->next_temp_id++;
                            ir_emit_shl(fpo, fpd, 1);               /* 16 * idx */
                            int o48b = c->next_temp_id++;
                            ir_emit_mov_imm(o48b, 48);
                            ir_emit_add(fpo, o48b, fpo);            /* 48 + 16*idx */
                            int o4 = c->next_temp_id++;
                            ir_emit_mov_imm(o4, 4);
                            int b4 = c->next_temp_id++;
                            ir_emit_add(b4, buf_t, o4);
                            ir_emit_store_32((uint16_t)fpo, (uint16_t)b4);
                            /* +8: overflow_arg_area = *(ap+32) */
                            int o32 = c->next_temp_id++;
                            ir_emit_mov_imm(o32, 32);
                            int ap32a = c->next_temp_id++;
                            ir_emit_add(ap32a, big_src_addr, o32);
                            int ov0 = c->next_temp_id++;
                            ir_emit_deref(ov0, ap32a);
                            int o8 = c->next_temp_id++;
                            ir_emit_mov_imm(o8, 8);
                            int b8 = c->next_temp_id++;
                            ir_emit_add(b8, buf_t, o8);
                            ir_emit_store_64((uint16_t)ov0, (uint16_t)b8);
                            /* +16: reg_save_area = region base */
                            int o16 = c->next_temp_id++;
                            ir_emit_mov_imm(o16, 16);
                            int b16 = c->next_temp_id++;
                            ir_emit_add(b16, buf_t, o16);
                            ir_emit_store_64((uint16_t)reg_base, (uint16_t)b16);
                        } else {
                            zc_emit_mem_copy(c, big_src_addr, buf_t, arg_struct_sz);
                        }
                        temp = buf_t;
                    } else if (is_mem_struct) {
                        int val0 = c->next_temp_id++;
                        if (arg_struct_sz < 8) {
                            if (arg_struct_sz == 1) ir_emit_load_byte(val0, mem_struct_addr);
                            else if (arg_struct_sz == 2) ir_emit_load_16(val0, mem_struct_addr);
                            else if (arg_struct_sz == 4) ir_emit_load_32(val0, mem_struct_addr);
                            else ir_emit_deref(val0, mem_struct_addr);
                        } else {
                            ir_emit_deref(val0, mem_struct_addr);
                        }
                        temp = val0;
                    }

                    /* Integer or struct argument: spill to stack slot(s) */
                    int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                    gp_stream++;
                    if (int_arg_idx < IR_CALL_MAX_ARGS) {
                        c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                        c->current_stack_frame += 8;
                        int_spill_offsets[int_arg_idx] = c->current_stack_frame;
                        ir_emit_store(temp, BE(c)->fp_reg, -int_spill_offsets[int_arg_idx]);
                        int_arg_idx++;
                    }
                    /* By-value struct/union argument larger than one 8-byte
                     * unit: the remaining 8-byte units travel in the
                     * following GP registers (SysV/psABI: every 8-byte
                     * unit of a MEMORY class type is one INT unit in
                     * argument order). Chunk source by expression shape:
                     *  - struct/union local (NODE_IDENT): lower_expr
                     *    loaded only chunk 0 into `temp`, so the other
                     *    chunks are read from the local's stack slot;
                     *  - struct/union in memory (NODE_INDEX, global, member):
                     *    chunks are read from mem_struct_addr + ch * 8;
                     *  - any other 9-16 byte value (compound literal,
                     *    struct-returning call): chunk 1 sits in the ABI
                     *    secondary value register (rdx / a1 / x1);
                     *  - >16 byte struct/union values take the hidden
                     *    pointer convention (big_struct_ptr_conv above),
                     *    so no extra chunks are passed. */
                    if (arg_struct_sz > 8 && !big_struct_ptr_conv) {
                        int num_chunks = (int)((arg_struct_sz + 7) / 8);
                        for (int ch = 1; ch < num_chunks && int_arg_idx < IR_CALL_MAX_ARGS; ch++) {
                            int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                            gp_stream++;
                            int ch_temp = c->next_temp_id++;
                            if (arg_node && arg_node->tag == NODE_IDENT && arg_struct_sym) {
                                ir_emit_load(ch_temp, BE(c)->fp_reg, -(arg_struct_sym->stack_offset - ch * 8));
                            } else if (is_mem_struct) {
                                int off_t = c->next_temp_id++;
                                ir_emit_mov_imm(off_t, ch * 8);
                                int ch_addr = c->next_temp_id++;
                                ir_emit_add(ch_addr, mem_struct_addr, off_t);
                                int ch_val = c->next_temp_id++;
                                size_t rem = arg_struct_sz - ch * 8;
                                if (rem == 1) ir_emit_load_byte(ch_val, ch_addr);
                                else if (rem == 2) ir_emit_load_16(ch_val, ch_addr);
                                else if (rem == 4) ir_emit_load_32(ch_val, ch_addr);
                                else ir_emit_deref(ch_val, ch_addr);
                                ch_temp = ch_val;
                            } else {
                                ir_emit_mov_reg(ch_temp, VREG_2);
                            }
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            int_spill_offsets[int_arg_idx] = c->current_stack_frame;
                            ir_emit_store(ch_temp, BE(c)->fp_reg, -int_spill_offsets[int_arg_idx]);
                            int_arg_idx++;
                        }
                    }
                }
                arg_node = zero_cc_ast_get(arena, arg);
                arg = arg_node ? arg_node->next : AST_NULL;
            }

            /* Reload arguments from stack slots immediately before call */
            int int_reload_idx = 0;
            int fp_reload_idx = 0;

            /* callee_uses_sret was computed before the arg loop (it also
             * sets the initial gp_stream for the sret slot). */
            int sret_buf_temp = 0;
            int32_t buf_stack_off = 0;

            /* >16 byte struct return requires caller to allocate buffer:
             * - System V AMD64 (x86-64) & RISC-V LP64D: pass address as first integer argument (RDI / a0, is_fp = 0)
             * - AArch64 AAPCS64: pass address in x8 (Indirect Result Location Register, is_fp = 3), user args start at x0 */
            if (callee_uses_sret) {
                c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                int32_t sret_alloc_sz = (int32_t)((call_ret_type.size + 7) & ~((size_t)7));
                c->current_stack_frame += sret_alloc_sz;
                buf_stack_off = c->current_stack_frame;

                sret_buf_temp = c->next_temp_id++;
                ir_emit_lea(sret_buf_temp, BE(c)->fp_reg, -buf_stack_off);

                if (all_arg_count < IR_CALL_MAX_ARGS) {
                    all_arg_temps[all_arg_count] = sret_buf_temp;
                    all_arg_is_fp[all_arg_count] = (c->isa == ISA_AARCH64) ? 3 : 0;
                    all_arg_count++;
                }
            }

            /* Then reload user integer arguments */
            uint8_t all_arg_stack_pos[IR_CALL_MAX_ARGS];
            for (int a = 0; a < int_arg_idx && all_arg_count < IR_CALL_MAX_ARGS; a++) {
                int reload_temp = c->next_temp_id++;
                ir_emit_load(reload_temp, BE(c)->fp_reg, -int_spill_offsets[a]);
                all_arg_temps[all_arg_count] = reload_temp;
                all_arg_is_fp[all_arg_count] = 0;
                all_arg_stack_pos[all_arg_count] = int_spill_stack_pos[a];
                all_arg_count++;
                int_reload_idx++;
            }

            /* Then reload FP arguments (f >= fp_regs_avail are stack args;
             * the FLOAD reload is unused for those - the call site pushes
             * them from the same spill slot via a GPR). */
            for (int f = 0; f < fp_arg_idx && all_arg_count < IR_CALL_MAX_ARGS; f++) {
                int reload_temp = c->next_temp_id++;
                ir_emit_fload(reload_temp, TEMP_NONE, -fp_spill_offsets[f]);
                all_arg_temps[all_arg_count] = reload_temp;
                all_arg_is_fp[all_arg_count] = 1;
                all_arg_stack_pos[all_arg_count] = fp_spill_stack_pos[f];
                all_arg_count++;
                fp_reload_idx++;
            }

            /* Reload indirect callee pointer if needed */
            if (is_indirect_call && all_arg_count < IR_CALL_MAX_ARGS) {
                int callee_reload_temp = c->next_temp_id++;
                ir_emit_load(callee_reload_temp, BE(c)->fp_reg, -indirect_callee_spill_off);
                all_arg_temps[all_arg_count] = callee_reload_temp;
                all_arg_is_fp[all_arg_count] = 2; /* 2 marks indirect callee register */
                all_arg_stack_pos[all_arg_count] = 255;
                all_arg_count++;
            }
            if (callee_uses_sret) all_arg_stack_pos[0] = 255;  /* sret entry is always a register arg */

            /* Total integer arguments in standard arg registers (x0..x7 on AArch64 excludes x8 sret) */
            int total_int_args = int_arg_idx + ((callee_uses_sret && c->isa != ISA_AARCH64) ? 1 : 0);

            /* Emit IR call (NULL name for indirect calls) */
            ir_emit_call(is_indirect_call ? NULL : name);

            /* Pack argument info into the instruction.
             * C2b: args 0/1 go in the widened temp_src1/src2 fields;
             * the full argument list (all args, indirect callee, sret)
             * always lives in the IRCallArgs side table recorded below.
             * (The pre-C2b 8-bit imm packing of args 2/3 is gone: it
             * could not represent 16-bit temp ids. Both consumers -
             * LRA and MC emission - read the side table first and fall
             * back to src1/src2 only if it is missing.) */
            if (ir_get_count() > 0) {
                Instr *last_instr = (Instr*)&ir_get_buffer()[ir_get_count() - 1];
                last_instr->temp_dest = (uint16_t)total_int_args;
                if (total_int_args > 0) last_instr->temp_src1 = (uint16_t)all_arg_temps[0];
                if (total_int_args > 1) last_instr->temp_src2 = (uint16_t)all_arg_temps[1];
                last_instr->imm = 0;  /* unused for OP_CALL since C2b */
                /* Full argument list (needed for 5+ args or indirect calls) */
                ir_call_args_record(ir_get_count() - 1, all_arg_count, all_arg_temps, all_arg_is_fp, all_arg_stack_pos);
                /* Store the number of FP REGISTER args (clamped: %al / the
                 * callee spill only covers the register slots) in label_idx */
                last_instr->label_idx = (uint16_t)(fp_arg_idx < fp_regs_avail ? fp_arg_idx : fp_regs_avail);
            }

            /* Record float spill offsets for this call (INCLUDING the
             * stack-passed FP args, index >= fp_arg_reg_count - the call
             * site pushes them from these slots). */
            if (c->lowering.ir_ctx.call_count < 64) {
                int cidx = c->lowering.ir_ctx.call_count++;
                c->lowering.ir_ctx.call_fp_spill_counts[cidx] = fp_arg_idx;
                for (int f = 0; f < fp_arg_idx && f < 16; f++) {
                    c->lowering.ir_ctx.call_fp_spills[cidx][f] = fp_spill_offsets[f];
                }
            }

            /* C99 complex return: real in xmm0 (VREG_F0), imaginary in
             * xmm1 (VREG_F1). Materialize into an aligned stack object
             * and hand back its ADDRESS (like vector returns). */
            if (call_ret_type.is_complex) {
                size_t cesz = (call_ret_type.base && call_ret_type.base->size == 4) ? 4 : 8;
                size_t csz = cesz * 2;
                c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + (int32_t)csz + 7) & ~7;
                int32_t cres_off = c->current_stack_frame;
                if (ir_get_count() > 0) {
                    /* Keep VREG_F0/VREG_F1 (xmm0/xmm1) live across the
                     * call: the complex result lives in that pair. */
                    Instr *li = (Instr*)&ir_get_buffer()[ir_get_count() - 1];
                    li->label_idx = 0x0100 | (csz & 0xFF);
                }
                int caddr = c->next_temp_id++;
                ir_emit_lea(caddr, BE(c)->fp_reg, -cres_off);
                if (cesz == 4) {
                    ir_emit_vstoress((uint16_t)caddr, (uint16_t)VREG_F0);
                    int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 4);
                    int a2 = c->next_temp_id++; ir_emit_add(a2, caddr, o2);
                    ir_emit_vstoress((uint16_t)a2, (uint16_t)VREG_F1);
                } else {
                    ir_emit_vstoresd((uint16_t)caddr, (uint16_t)VREG_F0);
                    int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 8);
                    int a2 = c->next_temp_id++; ir_emit_add(a2, caddr, o2);
                    ir_emit_vstoresd((uint16_t)a2, (uint16_t)VREG_F1);
                }
                c->last_expr_is_float = 0;
                return caddr;
            }

            if (call_ret_type.tag == TYPE_FLOAT || call_ret_type.tag == TYPE_DOUBLE) {
                c->last_expr_is_float = 1;
                /* Materialize the FP result into a tracked temp (C8):
                 * returning VREG_F0 directly leaves xmm0 untracked by
                 * the LRA, so the allocator may assign a later temp
                 * (e.g. a float literal loaded for the RHS of the same
                 * expression) to xmm0 and clobber the result before it
                 * is consumed. */
                int fp_res = c->next_temp_id++;
                ir_emit_fmov((uint16_t)fp_res, (uint16_t)VREG_F0);
                return fp_res;
            }

            c->last_expr_is_float = 0;

            /* Vector return: materialize into an aligned stack temp
             * object and hand back its ADDRESS (consumers - subscript,
             * assignment, nested ops - load through it). 9-16B vectors
             * arrive in the VREG_0/VREG_2 pair; <=8B in VREG_0 alone. */
            if (call_ret_type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&call_ret_type) &&
                call_ret_type.size >= 1 && call_ret_type.size <= 16) {
                size_t vsz = call_ret_type.size;
                if (vsz > 16) c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + (int32_t)vsz + 7) & ~7;
                int32_t res_off = c->current_stack_frame;
                if (vsz > 8) {
                    if (ir_get_count() > 0) {
                        Instr *li = (Instr*)&ir_get_buffer()[ir_get_count() - 1];
                        li->label_idx = 0x0100 | (call_ret_type.size & 0xFF);
                    }
                    int t0 = c->next_temp_id++;
                    ir_emit_mov_reg(t0, VREG_0);
                    ir_emit_store(t0, BE(c)->fp_reg, -res_off);
                    int t1 = c->next_temp_id++;
                    ir_emit_mov_reg(t1, VREG_2);
                    ir_emit_store(t1, BE(c)->fp_reg, -res_off + 8);
                } else {
                    int t0 = c->next_temp_id++;
                    ir_emit_mov_reg(t0, VREG_0);
                    int a0 = c->next_temp_id++;
                    ir_emit_lea(a0, BE(c)->fp_reg, -res_off);
                    if (vsz == 1) ir_emit_store_byte(t0, a0);
                    else if (vsz == 2) ir_emit_store_16(t0, a0);
                    else if (vsz == 4) ir_emit_store_32(t0, a0);
                    else ir_emit_store_64(t0, a0);
                }
                int addr = c->next_temp_id++;
                ir_emit_lea(addr, BE(c)->fp_reg, -res_off);
                return addr;
            }

            /* Check if function returns a 9-16 byte struct/union */
            if ((call_ret_type.tag == TYPE_STRUCT || call_ret_type.tag == TYPE_UNION) &&
                call_ret_type.size > 8 && call_ret_type.size <= 16) {
                /* Store struct return info in label_idx: high byte = 1, low byte = size */
                if (ir_get_count() > 0) {
                    Instr *last_instr = (Instr*)&ir_get_buffer()[ir_get_count() - 1];
                    last_instr->label_idx = 0x0100 | (call_ret_type.size & 0xFF);
                }
                /* 9-16 byte struct return uses integer register pair */
                return VREG_0;
            }

            /* If function returns >16 byte struct, return pointer to allocated buffer */
            if (callee_uses_sret) {
                int call_res_temp = c->next_temp_id++;
                if (c->isa == ISA_AARCH64) {
                    /* AAPCS64 does not require callee to preserve x8 or return the address in x0;
                     * recompute the caller's sret buffer address directly from -buf_stack_off */
                    ir_emit_lea(call_res_temp, BE(c)->fp_reg, -buf_stack_off);
                } else {
                    ir_emit_mov_reg(call_res_temp, VREG_0);
                }
                return call_res_temp;
            }

            int call_res_temp = c->next_temp_id++;
            ir_emit_mov_reg(call_res_temp, VREG_0);
            zero_cc_emit_int_extend(c, call_res_temp, &call_ret_type);
            return call_res_temp;
}
