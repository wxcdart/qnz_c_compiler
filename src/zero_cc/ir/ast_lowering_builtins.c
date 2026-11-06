/* AST Lowering - GCC/Clang Builtins, Intrinsics, and Atomics (AST to IR) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/parser/vla_struct.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"


/* Builtin emit helpers (bitops/overflow/atomics) -> ast_lowering_builtin_helpers.c,
 * va_start/va_arg -> ast_lowering_varargs.c (Sprint 114). */

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

/* --- __builtin_object_size compile-time object-size walker --------------
 *
 * GCC never emits a call to __builtin_object_size: it folds the builtin to
 * a compile-time constant. Calibrated against gcc-14 (-O1, the level the
 * c-torture reference binaries are built at):
 *   - a pointer into a known local/global array/scalar/struct reports the
 *     distance from the pointer to the END OF THE ENCLOSING OBJECT: the
 *     whole array, the whole struct (member sub-objects do NOT create
 *     nearer boundaries - &a.member[i] measures to the struct end), or a
 *     single struct/union element of an array-of-structs;
 *   - &arr[i] / &a.b / &a.b[i] / ptr+N / ptr-N combine constant offsets
 *     exactly (pointer step = pointed-to element size);
 *   - a pointer variable (no statically known origin) yields -1
 *     (SIZE_MAX, the "unknown size" sentinel FORTIFY callers check);
 *   - an out-of-range pointer (before the start or at/past the end)
 *     yields 0.
 * Modes 0/1/2 all reduce to the enclosing-object distance in the shapes
 * the walker supports. Unknown shapes (runtime offsets, calls, unknown
 * pointer bases, flexible members) fall back to -1.
 */
typedef struct {
    int64_t cont_start;    /* enclosing-object start (outer-object frame)  */
    int64_t cont_size;     /* enclosing-object size                        */
    int64_t off;           /* pointer offset in the same frame             */
    int64_t ptr_esz;       /* pointee size for further ptr-arith/index     */
    const ZeroCCType *elem;      /* current array element type (NULL)      */
    const ZeroCCType *cont_struct; /* enclosing struct/union type for a
                                    * further member lookup (NULL)         */
} ZcObjInfo;

static bool zc_objsize_walk(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                            ZeroCCASTNode *n, ZcObjInfo *o);
static int64_t zc_objsize_value(const ZcObjInfo *o);

/* Evaluate a `__builtin_object_size(ptr, mode)` call node as a constant:
 * the folded size, or -1 when the object is unknown. Always succeeds
 * (the builtin is a compile-time constant by definition). */
static bool zc_objsize_fold(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                            ZeroCCASTNode *call, int64_t *out) {
    ZeroCCASTNode *p_node = (call->right != AST_NULL)
        ? zero_cc_ast_get(arena, call->right) : NULL;
    ZcObjInfo o;
    if (p_node && zc_objsize_walk(c, arena, p_node, &o))
        *out = zc_objsize_value(&o);
    else
        *out = -1;
    return true;
}

/* Evaluate an AST node as a compile-time integer constant, recognizing
 * the __builtin_constant_p / __builtin_choose_expr builtins (which are
 * themselves constant expressions).  Returns 1 (with *out set) when the
 * node is a constant, 0 otherwise.  Used by __builtin_choose_expr's
 * runtime lowering to decide which operand to evaluate. */
bool zc_ast_const_eval(ZeroCCCompiler *c, ZeroCCASTArena *arena,
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
            int64_t cond;
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
            if (strcmp(cal->s.str, "__builtin_object_size") == 0) {
                return zc_objsize_fold(c, arena, n, out);
            }
            if (zc_is_bitop_builtin_name(cal->s.str)) {
                int64_t arg_v = 0;
                if (!zc_ast_const_eval(c, arena, n->right, &arg_v)) return false;
                int nbits = zc_bitop_width(c, cal->s.str);
                *out = (int64_t)zc_eval_bitop_const(cal->s.str, (uint64_t)arg_v, nbits);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

static bool zc_objsize_walk(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                            ZeroCCASTNode *n, ZcObjInfo *o) {
    if (!n) return false;
    switch (n->tag) {
        case NODE_IDENT: {
            const ZeroCCType *t = NULL;
            ZeroCCSym *sym = lowering_local_sym(c, n, n->s.str);
            if (sym && !sym->is_extern && !sym->is_vla && !sym->is_vla_struct &&
                !sym->is_label_value && !sym->is_capture_mirror) {
                t = &sym->type;
            } else {
                ZeroCCGlobalVar *gv = find_global_var(c, n->s.str);
                if (gv && !gv->is_tls) t = &gv->type;
            }
            if (!t) return false;
            o->off = 0;
            o->cont_start = 0;
            o->elem = NULL;
            o->cont_struct = NULL;
            switch (t->tag) {
                case TYPE_ARRAY: {
                    if (!t->base || t->array_len == 0 || t->base->size == 0)
                        return false;
                    int64_t esz = (int64_t)t->base->size;
                    bool elem_agg = (t->base->tag == TYPE_STRUCT ||
                                     t->base->tag == TYPE_UNION);
                    o->cont_size = elem_agg ? esz : esz * (int64_t)t->array_len;
                    o->ptr_esz = esz;
                    o->elem = t->base;
                    return true;
                }
                case TYPE_STRUCT:
                case TYPE_UNION:
                    if (t->size == 0) return false;
                    o->cont_size = (int64_t)t->size;
                    o->ptr_esz = (int64_t)t->size;
                    o->cont_struct = t;
                    return true;
                default:
                    if (t->size == 0) return false;
                    o->cont_size = (int64_t)t->size;
                    o->ptr_esz = (int64_t)t->size;
                    return true;
            }
        }
        case NODE_UNOP:
            if (n->aux.op != TK_AMP) return false;
            return zc_objsize_walk(c, arena, zero_cc_ast_get(arena, n->left), o);
        case NODE_CAST:
            return zc_objsize_walk(c, arena, zero_cc_ast_get(arena, n->left), o);
        case NODE_INDEX: {
            ZcObjInfo b;
            if (!zc_objsize_walk(c, arena, zero_cc_ast_get(arena, n->left), &b))
                return false;
            if (!b.elem || b.ptr_esz <= 0) return false;
            int64_t i;
            if (!zc_ast_const_eval(c, arena, n->right, &i)) return false;
            o->cont_start = b.cont_start;
            o->cont_size = b.cont_size;
            o->cont_struct = b.cont_struct;
            o->off = b.off + i * b.ptr_esz;
            const ZeroCCType *e = b.elem;
            if (e->tag == TYPE_STRUCT || e->tag == TYPE_UNION) {
                /* a struct/union element IS the enclosing object */
                o->cont_start = o->off;
                o->cont_size = b.ptr_esz;
                o->ptr_esz = b.ptr_esz;
                o->elem = NULL;
                o->cont_struct = e;
            } else if (e->tag == TYPE_ARRAY) {
                if (!e->base || e->base->size == 0) return false;
                o->ptr_esz = (int64_t)e->base->size;
                o->elem = e->base;
            } else {
                o->ptr_esz = (int64_t)e->size;
                o->elem = NULL;
            }
            return true;
        }
        case NODE_MEMBER: {
            ZcObjInfo b;
            if (!zc_objsize_walk(c, arena, zero_cc_ast_get(arena, n->left), &b))
                return false;
            if (!b.cont_struct || !b.cont_struct->members) return false;
            int64_t m_off = (int32_t)n->aux.alt;
            const ZeroCCStructMember *m = NULL;
            for (size_t i = 0; i < b.cont_struct->member_count; i++) {
                if ((int64_t)b.cont_struct->members[i].offset == m_off) {
                    m = &b.cont_struct->members[i];
                    break;
                }
            }
            if (!m || m->is_flexible || m->size == 0) return false;
            o->cont_start = b.cont_start;
            o->cont_size = b.cont_size;
            o->cont_struct = NULL; /* a member's own table is unreachable */
            o->off = b.off + m_off;
            if (m->tag == TYPE_ARRAY && m->base && m->base->size) {
                o->ptr_esz = (int64_t)m->base->size;
                o->elem = m->base;
            } else {
                o->ptr_esz = (int64_t)m->size;
                o->elem = NULL;
            }
            return true;
        }
        case NODE_BINOP: {
            if (n->aux.op != TK_PLUS && n->aux.op != TK_MINUS) return false;
            ZcObjInfo b;
            int64_t k;
            bool base_left;
            if (zc_objsize_walk(c, arena, zero_cc_ast_get(arena, n->left), &b) &&
                zc_ast_const_eval(c, arena, n->right, &k)) {
                base_left = true;
            } else if (zc_objsize_walk(c, arena, zero_cc_ast_get(arena, n->right), &b) &&
                       zc_ast_const_eval(c, arena, n->left, &k)) {
                base_left = false;
            } else {
                return false;
            }
            if (b.ptr_esz <= 0) return false;
            *o = b;
            int64_t step = b.ptr_esz * k;
            if (n->aux.op == TK_PLUS) o->off += base_left ? step : -step;
            else o->off += base_left ? -step : step;
            return true;
        }
        default:
            return false;
    }
}

/* Final size from a successful walk: 0 for out-of-range pointers (gcc
 * behavior), the distance to the enclosing-object end otherwise. */
static int64_t zc_objsize_value(const ZcObjInfo *o) {
    if (o->off < o->cont_start || o->off >= o->cont_start + o->cont_size)
        return 0;
    return (o->cont_start + o->cont_size) - o->off;
}

bool lower_expr_builtin(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                        ZeroCCASTNode *node, const char *name,
                        int *out_temp) {
    if (!name || !out_temp) return false;
    ZeroCCASTNode *callee = zero_cc_ast_get(arena, node->left);
    bool callee_is_ident = (callee && callee->tag == NODE_IDENT);
    (void)callee_is_ident;

        /* __builtin_object_size(ptr, mode) - folded to a compile-time
         * constant (see the walker above). Without this the builtin stayed
         * an external call: the JIT failed symbol resolution ("undefined
         * reference to __builtin_object_size") and the AOT link left dead
         * `call 0 <link_error>` stubs (c-torture execute/builtins *chk.c).
         * Non-constant arguments are still lowered for side effects. */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_object_size") == 0) {
            int64_t sz = -1;
            ASTRef ptr_ref = node->right;
            ZeroCCASTNode *p0 = (ptr_ref != AST_NULL) ? zero_cc_ast_get(arena, ptr_ref) : NULL;
            bool folded = false;
            if (p0) {
                ZcObjInfo o;
                if (zc_objsize_walk(c, arena, p0, &o)) {
                    sz = zc_objsize_value(&o);
                    folded = true;
                }
            }
            if (!folded) {
                /* side effects in an unknown-shape pointer argument */
                if (ptr_ref != AST_NULL) (void)lower_expr(c, arena, ptr_ref);
            }
            ASTRef mode_ref = p0 ? p0->next : AST_NULL;
            int64_t mode_dummy;
            if (mode_ref != AST_NULL &&
                !zc_ast_const_eval(c, arena, mode_ref, &mode_dummy)) {
                (void)lower_expr(c, arena, mode_ref); /* side effects */
            }
            int res = c->next_temp_id++;
            ir_emit_mov_imm(c, res, (uint64_t)sz);
            c->last_expr_is_float = 0;
            *out_temp = res;
            return true;
        }

        /* strncmp() / __builtin_strncmp() with n == 0: GCC folds the result
         * to 0 at every -O level without emitting a call (calibrated against
         * gcc-14 at -O0: `movl $0,%eax`, no strncmp call). The c-torture
         * user strncmp (lib/strncmp.c) returns the *uninitialized* c1 - c2
         * when n == 0, so a real call is UB-dependent - execute/builtins/
         * strncmp.c aborted on its first `strncmp (++s2, ++s3, 0)`. The
         * string arguments are still lowered for their side effects
         * (`++s2`); the result is the int constant 0. -fno-builtin-strncmp
         * (or -fno-builtin) keeps the real call, like the abs family. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "strncmp") == 0 ||
             strcmp(callee->s.str, "__builtin_strncmp") == 0) &&
            !zero_cc_no_builtin(c, "strncmp")) {
            ZeroCCASTNode *sa0 = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
            ZeroCCASTNode *sa1 = (sa0 && sa0->next != AST_NULL) ? zero_cc_ast_get(arena, sa0->next) : NULL;
            if (sa0 && sa1 && sa1->next != AST_NULL) {
                int64_t snval = 0;
                if (zc_ast_const_eval(c, arena, sa1->next, &snval) && snval == 0) {
                    (void)lower_expr(c, arena, node->right); /* side effects */
                    (void)lower_expr(c, arena, sa0->next);
                    int res = c->next_temp_id++;
                    ir_emit_mov_imm(c, res, 0);
                    c->last_expr_is_float = 0;
                    *out_temp = res;
                    return true;
                }
            }
        }

        /* __builtin_return_address(level) / __builtin_frame_address(level) */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_return_address") == 0 ||
             strcmp(callee->s.str, "__builtin_frame_address") == 0)) {
            bool is_ret_addr = (strcmp(callee->s.str, "__builtin_return_address") == 0);
            c->lowering.has_frame_address = true;

            int cur_fp = c->next_temp_id++;
            ir_emit_lea(c, cur_fp, BE(c)->fp_reg, 0);

            int fp = cur_fp;
            int64_t const_lvl = 0;
            if (node->right != AST_NULL && zc_ast_const_eval(c, arena, node->right, &const_lvl)) {
                if (const_lvl < 0) const_lvl = 0;
                if (const_lvl > 100) const_lvl = 100;
                for (int64_t i = 0; i < const_lvl; i++) {
                    int next_fp = c->next_temp_id++;
                    ir_emit_deref(c, next_fp, fp);
                    fp = next_fp;
                }
            } else if (node->right != AST_NULL) {
                int lvl_temp = lower_expr(c, arena, node->right);
                int cnt_temp = c->next_temp_id++;
                ir_emit_mov_reg(c, cnt_temp, lvl_temp);
                uint16_t loop_lbl = (uint16_t)c->ir_next_label++;
                uint16_t end_lbl = (uint16_t)c->ir_next_label++;

                ir_emit_label(c, loop_lbl);
                int zero_t = c->next_temp_id++;
                ir_emit_mov_imm(c, zero_t, 0);
                ir_emit_cmp(c, cnt_temp, zero_t);
                int is_le = c->next_temp_id++;
                ir_emit_cond_set(c, is_le, CC_LE);
                ir_emit_jne(c, (uint16_t)is_le, end_lbl);

                int next_fp = c->next_temp_id++;
                ir_emit_deref(c, next_fp, fp);
                fp = next_fp;

                int one = c->next_temp_id++;
                ir_emit_mov_imm(c, one, 1);
                ir_emit_sub(c, cnt_temp, cnt_temp, one);
                ir_emit_jmp(c, loop_lbl);

                ir_emit_label(c, end_lbl);
            }

            int res = fp;
            if (is_ret_addr) {
                int ra_off = (c->isa == ISA_RISCV32) ? 4 : 8;
                int off_temp = c->next_temp_id++;
                ir_emit_mov_imm(c, off_temp, ra_off);
                int ra_ptr = c->next_temp_id++;
                ir_emit_add(c, ra_ptr, fp, off_temp);
                res = c->next_temp_id++;
                ir_emit_deref(c, res, ra_ptr);
            }

            c->last_expr_is_float = 0;
            *out_temp = res;
            return true;
        }

        /* __builtin_apply_args() / __builtin_apply(fn, args, nbytes) - GNU
         * function application with an explicit argument block
         * (c-torture execute/pr47237).
         *
         * `__builtin_apply_args()` returns a pointer to a fresh frame block
         * with the CURRENT function's arguments copied in, one 8-byte slot
         * per parameter (the ABI register width; only the parameter's type
         * width is written, so narrow args are not padded with garbage).
         *
         * `__builtin_apply(fn, p, n)` calls the function pointer `fn`,
         * taking its arguments from successive 8-byte slots of `p` using
         * the pointee's declared parameter types (the `n` byte hint is the
         * caller's block size and is ignored - the signature is the truth).
         * Struct/union/complex/array parameters cannot be represented in
         * the 8-byte slot model and are a clean REJECT (no sret handling
         * either). */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_apply_args") == 0) {
            ZeroCCFunction *cur_fn = zero_cc_get_current_fn(c);
            int nparams = (cur_fn && cur_fn->param_types && cur_fn->param_count > 0)
                           ? (int)cur_fn->param_count : 0;
            if (nparams > 16) nparams = 16;
            int block_sz = (nparams * 8) < 16 ? 16 : (nparams * 8);
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += block_sz;
            int32_t block_off = c->current_stack_frame;
            int base = c->next_temp_id++;
            ir_emit_lea(c, base, BE(c)->fp_reg, -block_off);
            for (int p = 0; p < nparams && p < (int)c->local_count; p++) {
                ZeroCCSym *psym = &c->locals[p];
                if (!psym->is_param) continue;
                const ZeroCCType *pt = (cur_fn && cur_fn->param_types) ? &cur_fn->param_types[p] : NULL;
                int w = (pt && pt->size >= 5) ? 8 : 4; /* int/float -> 4, long/ptr/double -> 8 */
                int slot_addr = c->next_temp_id++;
                ir_emit_lea(c, slot_addr, BE(c)->fp_reg, -(block_off - 8 * p));
                int val = c->next_temp_id++;
                zc_sym_load(c, psym, 0, val);
                if (w == 8) ir_emit_store_64(c, (uint16_t)val, (uint16_t)slot_addr);
                else ir_emit_store_32(c, (uint16_t)val, (uint16_t)slot_addr);
            }
            c->last_expr_is_float = 0;
            *out_temp = base;
            return true;
        }
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_apply") == 0) {
            ASTRef arg0 = node->right;
            ZeroCCASTNode *a0n = zero_cc_ast_get(arena, arg0);
            ASTRef arg1 = (a0n && a0n->next != AST_NULL) ? a0n->next : AST_NULL;
            int fn_temp = lower_expr(c, arena, arg0);
            c->last_expr_is_float = 0;
            int p_temp = (arg1 != AST_NULL) ? lower_expr(c, arena, arg1) : -1;
            c->last_expr_is_float = 0;
            /* Third arg (nbytes) - lowered for side effects, value unused. */
            ZeroCCASTNode *a1n = (arg1 != AST_NULL) ? zero_cc_ast_get(arena, arg1) : NULL;
            ASTRef arg2 = (a1n && a1n->next != AST_NULL) ? a1n->next : AST_NULL;
            if (arg2 != AST_NULL) { lower_expr(c, arena, arg2); c->last_expr_is_float = 0; }
            if (p_temp < 0) {
                diag_report_error_at_token(c, "__builtin_apply requires (fn, args, nbytes)");
                *out_temp = VREG_0;
                return true;
            }
            /* The pointee's declared signature drives the argument layout.
             * For an identifier callee the ZeroCCFunction table is the
             * authoritative parameter list (the semantic TypeInfo often
             * carries no prototype), mirroring the direct-call path. */
            int npp = 0;
            ZeroCCType apply_pparams[IR_CALL_MAX_ARGS];
            ZeroCCType apply_ret = zero_cc_type_int();
            ZeroCCFunction *apply_callee_fn = NULL;
            if (a0n && a0n->tag == NODE_IDENT && a0n->s.str) {
                apply_callee_fn = zero_cc_find_function(c, a0n->s.str);
            }
            if (apply_callee_fn) {
                if (apply_callee_fn->param_types && apply_callee_fn->param_count > 0) {
                    npp = (int)apply_callee_fn->param_count;
                    if (npp > IR_CALL_MAX_ARGS - 2) npp = IR_CALL_MAX_ARGS - 2;
                    for (int j = 0; j < npp; j++) apply_pparams[j] = apply_callee_fn->param_types[j];
                }
                apply_ret = apply_callee_fn->return_type;
            } else {
                TypeInfo *cal_ti = ast_node_get_typeinfo(c, a0n);
                TypeInfo *pointee = NULL;
                if (cal_ti && cal_ti->kind == TYPE_KIND_POINTER &&
                    cal_ti->details.pointer.base &&
                    cal_ti->details.pointer.base->kind == TYPE_KIND_FUNCTION)
                    pointee = cal_ti->details.pointer.base;
                else if (cal_ti && cal_ti->kind == TYPE_KIND_FUNCTION)
                    pointee = cal_ti;
                if (pointee) {
                    npp = (int)pointee->details.function.param_cnt;
                    if (npp > IR_CALL_MAX_ARGS - 2) npp = IR_CALL_MAX_ARGS - 2;
                    for (int j = 0; j < npp; j++)
                        apply_pparams[j] = typeinfo_to_zero_cc_type(pointee->details.function.params[j]);
                    if (pointee->details.function.ret)
                        apply_ret = typeinfo_to_zero_cc_type(pointee->details.function.ret);
                }
                /* else: no recoverable pointee signature (e.g. the function
                 * pointer was cast through void *).  Fall back to a
                 * K&R-style indirect call with no argument metadata -
                 * the same shape the compiler already emits for calls
                 * through unprototyped pointers. */
            }
            /* Reject the shapes the 8-byte slot model cannot represent. */
            for (int j = 0; j < npp; j++) {
                const ZeroCCType *pt = &apply_pparams[j];
                if (pt->tag == TYPE_STRUCT || pt->tag == TYPE_UNION ||
                    pt->tag == TYPE_ARRAY || pt->is_complex) {
                    diag_report_error_at_token(c, "__builtin_apply with struct/complex parameters unsupported");
                    *out_temp = VREG_0;
                    return true;
                }
            }
            if ((apply_ret.tag == TYPE_STRUCT || apply_ret.tag == TYPE_UNION) && apply_ret.size > 16) {
                diag_report_error_at_token(c, "__builtin_apply of an sret-returning function unsupported");
                *out_temp = VREG_0;
                return true;
            }
            int gp_regs_avail = BE(c)->conv ? BE(c)->conv->int_arg_reg_count : 8;
            int fp_regs_avail = BE(c)->conv ? BE(c)->conv->fp_arg_reg_count : 8;
            int gp_stream = 0, fp_stream = 0, stack_arg_count = 0;
            int int_arg_idx = 0, fp_arg_idx = 0;
            int32_t fp_spill_offsets[IR_CALL_MAX_ARGS];
            int all_arg_temps[IR_CALL_MAX_ARGS];
            int all_arg_is_fp[IR_CALL_MAX_ARGS];   /* int[] - see ir_call_args_record */
            int all_arg_stack_pos[IR_CALL_MAX_ARGS];
            int all_arg_count = 0;
            for (int j = 0; j < npp; j++) {
                ZeroCCType pt = apply_pparams[j];
                int slot_addr = c->next_temp_id++;
                if (j == 0) slot_addr = p_temp;
                else {
                    int off_t = c->next_temp_id++;
                    ir_emit_mov_imm(c, off_t, 8 * j);
                    int add_t = c->next_temp_id++;
                    ir_emit_add(c, add_t, p_temp, off_t);
                    slot_addr = add_t;
                }
                bool param_is_fp = (pt.tag == TYPE_FLOAT || pt.tag == TYPE_DOUBLE);
                int val = c->next_temp_id++;
                if (param_is_fp) {
                    /* FP argument: load the bit pattern, spill it to a frame
                     * slot the MC emitter reads into the FP arg register
                     * (same mechanism as the direct-call path). */
                    if (pt.tag == TYPE_FLOAT) ir_emit_load_32(c, (uint16_t)val, (uint16_t)slot_addr);
                    else ir_emit_deref(c, val, slot_addr);
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    fp_spill_offsets[fp_arg_idx] = c->current_stack_frame;
                    ir_emit_store(c, val, BE(c)->fp_reg, -fp_spill_offsets[fp_arg_idx]);
                    uint8_t sp = (fp_stream < fp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                    all_arg_temps[all_arg_count] = val;
                    all_arg_is_fp[all_arg_count] = 1;
                    all_arg_stack_pos[all_arg_count] = sp;
                    all_arg_count++;
                    fp_stream++;
                    fp_arg_idx++;
                } else {
                    size_t w = pt.size;
                    if (w <= 4) ir_emit_load_32(c, (uint16_t)val, (uint16_t)slot_addr);
                    else ir_emit_deref(c, val, slot_addr);
                    uint8_t sp = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                    all_arg_temps[all_arg_count] = val;
                    all_arg_is_fp[all_arg_count] = 0;
                    all_arg_stack_pos[all_arg_count] = sp;
                    all_arg_count++;
                    gp_stream++;
                    int_arg_idx++;
                }
            }
            /* Spill the callee pointer (indirect-call convention). */
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t callee_spill_off = c->current_stack_frame;
            ir_emit_store(c, fn_temp, BE(c)->fp_reg, -callee_spill_off);
            int callee_reload = c->next_temp_id++;
            ir_emit_load(c, callee_reload, BE(c)->fp_reg, -callee_spill_off);
            all_arg_temps[all_arg_count] = callee_reload;
            all_arg_is_fp[all_arg_count] = 2;
            all_arg_stack_pos[all_arg_count] = 255;
            all_arg_count++;
            int total_int_args = int_arg_idx;
            ir_emit_call(c, NULL);
            if (ir_get_count(c) > 0) {
                Instr *last_instr = (Instr*)&ir_get_buffer(c)[ir_get_count(c) - 1];
                last_instr->temp_dest = (uint16_t)total_int_args;
                if (total_int_args > 0) last_instr->temp_src1 = (uint16_t)all_arg_temps[0];
                if (total_int_args > 1) last_instr->temp_src2 = (uint16_t)all_arg_temps[1];
                ir_call_args_record(c, ir_get_count(c) - 1, all_arg_count,
                                    all_arg_temps, all_arg_is_fp, all_arg_stack_pos);
                last_instr->label_idx = (uint16_t)(fp_arg_idx < fp_regs_avail ? fp_arg_idx : fp_regs_avail);
            }
            if (c->lowering.ir_ctx.call_count < 64) {
                int cidx = c->lowering.ir_ctx.call_count++;
                c->lowering.ir_ctx.call_fp_spill_counts[cidx] = fp_arg_idx;
                for (int f = 0; f < fp_arg_idx && f < 16; f++) {
                    c->lowering.ir_ctx.call_fp_spills[cidx][f] = fp_spill_offsets[f];
                }
            }
            /* Result: mirror the direct-call path's materialization. */
            ZeroCCType call_ret = apply_ret;
            if (call_ret.tag == TYPE_FLOAT || call_ret.tag == TYPE_DOUBLE) {
                c->last_expr_is_float = 1;
                int fp_res = c->next_temp_id++;
                ir_emit_fmov(c, (uint16_t)fp_res, (uint16_t)VREG_F0);
                *out_temp = fp_res;
                return true;
            }
            c->last_expr_is_float = 0;
            *out_temp = VREG_0;
            return true;
        }

        /* __builtin_setjmp(buf) / __builtin_longjmp(buf, val) - non-local
         * jumps lowered inline via OP_SETJMP / OP_LONGJMP across all target
         * ISAs (x86-64, AArch64, RISC-V64, RISC-V32) in both JIT and AOT.
         * Buffer layout (3 pointer-sized words, fitting GCC's 5-word buf):
         *   buf[0] = caller FP (%rbp / x29 / s0)
         *   buf[1] = resume PC (.Lsetjmp_resume)
         *   buf[2] = caller SP (%rsp / sp / x2)
         * Functions calling setjmp keep a real frame pointer (has_frame_address),
         * keep automatics and call-spanning temps on the stack (has_setjmp), and
         * save/restore the full ISA callee-saved GPR set in prologue/epilogue. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_setjmp") == 0 ||
             strcmp(callee->s.str, "__builtin_longjmp") == 0)) {
            bool is_longjmp = (strcmp(callee->s.str, "__builtin_longjmp") == 0);
            c->lowering.has_frame_address = true;
            if (!is_longjmp) {
                c->lowering.has_setjmp = true;
            }

            ASTRef buf_arg = node->right;                 /* first arg: the buf */
            int buf_temp = lower_expr(c, arena, buf_arg);
            c->last_expr_is_float = 0;

            if (is_longjmp) {
                ZeroCCASTNode *bn = zero_cc_ast_get(arena, buf_arg);
                ASTRef val_arg = (bn && bn->next != AST_NULL) ? bn->next : AST_NULL;
                int val_raw = lower_expr(c, arena, val_arg);
                /* C99 7.13.2.1: if val == 0, setjmp returns 1.
                 * Branchless normalization: val = val_raw | (val_raw == 0). */
                int is_zero = c->next_temp_id++;
                ir_emit_cmp_zero(c, val_raw);
                ir_emit_cond_set(c, is_zero, CC_EQ);
                int val_norm = c->next_temp_id++;
                ir_emit_or(c, val_norm, val_raw, is_zero);

                ir_emit_longjmp(c, (uint16_t)buf_temp, (uint16_t)val_norm);
                /* longjmp never returns: block fall-through with an infinite
                 * loop (the setjmp site in the target function resumes). */
                uint16_t loop_lbl = (uint16_t)c->ir_next_label++;
                ir_emit_label(c, loop_lbl);
                ir_emit_jmp(c, loop_lbl);
                *out_temp = VREG_0;
                return true;
            }

            /* setjmp: emit inline OP_SETJMP (sets VREG_0 = 0 on first pass,
             * or lands at resume label with VREG_0 = val from OP_LONGJMP),
             * then capture VREG_0 into a fresh allocatable temp. */
            uint16_t resume_lbl = (uint16_t)c->ir_next_label++;
            ir_emit_setjmp(c, (uint16_t)buf_temp, resume_lbl);
            int res = c->next_temp_id++;
            ir_emit_mov_reg(c, res, VREG_0);
            *out_temp = res;
            return true;
        }

        /* __builtin_alloca(size) / alloca(size) - dynamic stack allocation */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_alloca") == 0 ||
             strcmp(callee->s.str, "alloca") == 0)) {
            int size_temp = lower_expr(c, arena, node->right);
            int add15 = c->next_temp_id++;
            ir_emit_mov_imm(c, add15, 15);
            int size_p15 = c->next_temp_id++;
            ir_emit_add(c, size_p15, size_temp, add15);
            int mask16 = c->next_temp_id++;
            ir_emit_mov_imm(c, mask16, -16);
            int aligned_size = c->next_temp_id++;
            ir_emit_and(c, aligned_size, size_p15, mask16);
            int ptr_temp = c->next_temp_id++;
            ir_emit_vla_alloc(c, ptr_temp, aligned_size);
            c->last_expr_is_float = 0;
            *out_temp = ptr_temp;
            return true;
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
            const TypeInfo *el_ti = (cx_ti && cx_ti->kind == TYPE_KIND_COMPLEX) ? cx_ti->details.array.base : NULL;
            const ZeroCCType *el_zt = NULL;
            if (!el_ti && cx_node && cx_node->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, cx_node->s.str);
                if (sym && zero_cc_type_is_complex(&sym->type)) el_zt = sym->type.base;
                else {
                    ZeroCCGlobalVar *gv = find_global_var(c, cx_node->s.str);
                    if (gv && zero_cc_type_is_complex(&gv->type)) el_zt = gv->type.base;
                }
            }
            size_t esz = el_ti ? type_size(el_ti) : (el_zt ? el_zt->size : 8);
            if (esz == 0) esz = 8;
            bool is_elem_fp = el_ti ? type_is_floating(el_ti) :
                              (el_zt ? (el_zt->tag == TYPE_FLOAT || el_zt->tag == TYPE_DOUBLE) : true);
            if (is_elem_fp && esz != 4 && esz != 8) esz = 8; /* long double: double model */
            int addr = lower_expr(c, arena, node->right);
            int res = c->next_temp_id++;
            int la = addr;
            if (is_imag) {
                int off = c->next_temp_id++;
                ir_emit_mov_imm(c, off, (int)esz);
                la = c->next_temp_id++;
                ir_emit_add(c, la, addr, off);
            }
            if (is_elem_fp) {
                if (esz == 4) ir_emit_vloadss(c, res, (uint16_t)la);
                else ir_emit_vloadsd(c, res, (uint16_t)la);
                c->last_expr_is_float = 1;
            } else {
                bool is_u = el_ti ? type_is_unsigned(el_ti) :
                            (el_zt ? zero_cc_type_is_unsigned(el_zt) : false);
                if (esz == 1) {
                    ir_emit_load_byte(c, res, la);
                    if (!is_u) ir_emit_sext(c, res, 8);
                } else if (esz == 2) {
                    ir_emit_load_16(c, res, la);
                    if (!is_u) ir_emit_sext(c, res, 16);
                } else if (esz == 4) {
                    ir_emit_load_32(c, res, la);
                    if (!is_u) ir_emit_sext(c, res, 32);
                } else {
                    ir_emit_deref(c, res, la);
                }
                c->last_expr_is_float = 0;
            }
            *out_temp = res;
            return true;
        }

        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_conj") == 0 ||
             strcmp(callee->s.str, "__builtin_conjf") == 0 ||
             strcmp(callee->s.str, "__builtin_conjl") == 0)) {
            ZeroCCASTNode *cx_node = zero_cc_ast_get(arena, node->right);
            TypeInfo *cx_ti = cx_node ? ast_node_get_typeinfo(c, cx_node) : NULL;
            const TypeInfo *el_ti = (cx_ti && cx_ti->kind == TYPE_KIND_COMPLEX) ? cx_ti->details.array.base : NULL;
            const ZeroCCType *el_zt = NULL;
            if (!el_ti && cx_node && cx_node->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, cx_node->s.str);
                if (sym && zero_cc_type_is_complex(&sym->type)) el_zt = sym->type.base;
                else {
                    ZeroCCGlobalVar *gv = find_global_var(c, cx_node->s.str);
                    if (gv && zero_cc_type_is_complex(&gv->type)) el_zt = gv->type.base;
                }
            }
            size_t esz = el_ti ? type_size(el_ti) : (el_zt ? el_zt->size :
                         (strcmp(callee->s.str, "__builtin_conjf") == 0 ? 4 : 8));
            if (esz == 0) esz = 8;
            bool is_elem_fp = el_ti ? type_is_floating(el_ti) :
                              (el_zt ? (el_zt->tag == TYPE_FLOAT || el_zt->tag == TYPE_DOUBLE) : true);
            if (is_elem_fp && esz != 4 && esz != 8) esz = 8;
            int addr = lower_expr(c, arena, node->right);
            int rh = c->next_temp_id++, li = c->next_temp_id++;
            int off = c->next_temp_id++;
            ir_emit_mov_imm(c, off, (int)esz);
            int la = c->next_temp_id++;
            ir_emit_add(c, la, addr, off);
            if (is_elem_fp) {
                if (esz == 4) {
                    ir_emit_vloadss(c, (uint16_t)rh, (uint16_t)addr);
                    ir_emit_vloadss(c, (uint16_t)li, (uint16_t)la);
                } else {
                    ir_emit_vloadsd(c, (uint16_t)rh, (uint16_t)addr);
                    ir_emit_vloadsd(c, (uint16_t)li, (uint16_t)la);
                }
            } else {
                if (esz == 1) {
                    ir_emit_load_byte(c, rh, addr);
                    ir_emit_load_byte(c, li, la);
                } else if (esz == 2) {
                    ir_emit_load_16(c, rh, addr);
                    ir_emit_load_16(c, li, la);
                } else if (esz == 4) {
                    ir_emit_load_32(c, rh, addr);
                    ir_emit_load_32(c, li, la);
                } else {
                    ir_emit_deref(c, rh, addr);
                    ir_emit_deref(c, li, la);
                }
            }
            int neg_li = c->next_temp_id++;
            if (is_elem_fp) {
                int zero_i = c->next_temp_id++;
                ir_emit_mov_imm(c, zero_i, 0);
                int zero_f = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
                ir_emit_fsub(c, (uint16_t)neg_li, (uint16_t)zero_f, (uint16_t)li);
            } else {
                ir_emit_neg(c, neg_li, li);
            }

            size_t csz = esz * 2;
            c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
            c->current_stack_frame += (int32_t)csz;
            int32_t res_off = c->current_stack_frame;
            int res_addr = c->next_temp_id++;
            ir_emit_lea(c, res_addr, BE(c)->fp_reg, -res_off);
            int off_m = c->next_temp_id++;
            ir_emit_mov_imm(c, off_m, (int)esz);
            int res_a2 = c->next_temp_id++;
            ir_emit_add(c, res_a2, res_addr, off_m);
            if (is_elem_fp) {
                if (esz == 4) {
                    ir_emit_vstoress(c, (uint16_t)res_addr, (uint16_t)rh);
                    ir_emit_vstoress(c, (uint16_t)res_a2, (uint16_t)neg_li);
                } else {
                    ir_emit_vstoresd(c, (uint16_t)res_addr, (uint16_t)rh);
                    ir_emit_vstoresd(c, (uint16_t)res_a2, (uint16_t)neg_li);
                }
            } else {
                if (esz == 1) {
                    ir_emit_store_byte(c, rh, res_addr);
                    ir_emit_store_byte(c, neg_li, res_a2);
                } else if (esz == 2) {
                    ir_emit_store_16(c, rh, res_addr);
                    ir_emit_store_16(c, neg_li, res_a2);
                } else if (esz == 4) {
                    ir_emit_store_32(c, rh, res_addr);
                    ir_emit_store_32(c, neg_li, res_a2);
                } else {
                    ir_emit_store_64(c, rh, res_addr);
                    ir_emit_store_64(c, neg_li, res_a2);
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
        }

        /* __builtin_expect(cond, val) is a branch-hint builtin: it
         * evaluates to `cond` (the hint is ignored by ZeroCC's codegen).
         * Lower to the condition expression and evaluate `val` for side effects. */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_expect", 16) == 0) {
            ASTRef expect_arg = node->right;
            int res = lower_expr(c, arena, expect_arg);
            ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, expect_arg);
            if (arg_node && arg_node->next != AST_NULL) {
                (void)lower_expr(c, arena, arg_node->next);
            }
            c->last_expr_is_float = 0;
            *out_temp = res;
            return true;
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
            *out_temp = VREG_0;
            return true;
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
                    ZeroCCGlobalVar *ag = as ? NULL : find_global_var(c, sh_a->s.str);
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
                            if (i == 0) { ir_emit_mov_reg(c, bi_off, b_addr); }
                            else {
                                int bo = c->next_temp_id++;
                                ir_emit_mov_imm(c, bo, (int32_t)(i * idsz));
                                ir_emit_add(c, bi_off, b_addr, bo);
                            }
                            int idx = c->next_temp_id++;
                            if (idsz == 1) { ir_emit_load_byte(c, idx, bi_off); ir_emit_zext(c, idx, 1); }
                            else if (idsz == 2) { ir_emit_load_16(c, idx, bi_off); ir_emit_zext(c, idx, 2); }
                            else if (idsz == 4) { ir_emit_load_32(c, idx, bi_off); ir_emit_zext(c, idx, 4); }
                            else { ir_emit_deref(c, idx, bi_off); }
                            /* m = idx & (n-1) */
                            int m_mask = c->next_temp_id++;
                            ir_emit_mov_imm(c, m_mask, mask_nm1);
                            int m = c->next_temp_id++;
                            ir_emit_and(c, m, idx, m_mask);
                            /* off = m * elsz */
                            int off = c->next_temp_id++;
                            if (elsz > 1) {
                                int es = c->next_temp_id++;
                                ir_emit_mov_imm(c, es, (int32_t)elsz);
                                ir_emit_mul(c, off, m, es);
                            } else {
                                ir_emit_mov_reg(c, off, m);
                            }
                            int aa = c->next_temp_id++;
                            ir_emit_add(c, aa, a_addr, off);
                            int ba = c->next_temp_id++;
                            ir_emit_add(c, ba, b_addr, off);
                            int av = c->next_temp_id++;
                            if (elsz == 1) { ir_emit_load_byte(c, av, aa); ir_emit_zext(c, av, 1); }
                            else if (elsz == 2) { ir_emit_load_16(c, av, aa); ir_emit_zext(c, av, 2); }
                            else if (elsz == 4) { ir_emit_load_32(c, av, aa); ir_emit_zext(c, av, 4); }
                            else { ir_emit_deref(c, av, aa); }
                            int bv = c->next_temp_id++;
                            if (elsz == 1) { ir_emit_load_byte(c, bv, ba); ir_emit_zext(c, bv, 1); }
                            else if (elsz == 2) { ir_emit_load_16(c, bv, ba); ir_emit_zext(c, bv, 2); }
                            else if (elsz == 4) { ir_emit_load_32(c, bv, ba); ir_emit_zext(c, bv, 4); }
                            else { ir_emit_deref(c, bv, ba); }
                            /* sel = (idx & n) == 0  ->  0/1 */
                            int n_bit = c->next_temp_id++;
                            ir_emit_mov_imm(c, n_bit, bit_n);
                            int ib = c->next_temp_id++;
                            ir_emit_and(c, ib, idx, n_bit);
                            int zero_t = c->next_temp_id++;
                            ir_emit_mov_imm(c, zero_t, 0);
                            ir_emit_cmp(c, (uint16_t)ib, (uint16_t)zero_t);
                            int cmp_t = c->next_temp_id++;
                            ir_emit_cond_set(c, (uint16_t)cmp_t, CC_EQ);
                            int sel = c->next_temp_id++;
                            ir_emit_mov_imm(c, sel, -1);
                            int selm = c->next_temp_id++;
                            ir_emit_mul(c, selm, cmp_t, sel);
                            int nv = c->next_temp_id++;
                            int all_ones = c->next_temp_id++;
                            ir_emit_mov_imm(c, all_ones, -1);
                            ir_emit_xor(c, nv, selm, all_ones);
                            int s1 = c->next_temp_id++;
                            ir_emit_and(c, s1, selm, av);
                            int s2 = c->next_temp_id++;
                            ir_emit_and(c, s2, nv, bv);
                            int lv = c->next_temp_id++;
                            ir_emit_or(c, lv, s1, s2);
                            /* store lane i into the result temp */
                            int ra = c->next_temp_id++;
                            if (i == 0) {
                                ir_emit_lea(c, ra, BE(c)->fp_reg, -res_off);
                            } else {
                                ir_emit_lea(c, ra, BE(c)->fp_reg, -(int32_t)(res_off - (int32_t)(i * elsz)));
                            }
                            if (elsz == 1) ir_emit_store_byte(c, lv, ra);
                            else if (elsz == 2) ir_emit_store_16(c, lv, ra);
                            else if (elsz == 4) ir_emit_store_32(c, lv, ra);
                            else ir_emit_store_64(c, lv, ra);
                        }
                        int addr = c->next_temp_id++;
                        ir_emit_lea(c, addr, BE(c)->fp_reg, -res_off);
                        c->last_expr_is_float = 0;
                        *out_temp = addr;
                        return true;
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
            ir_emit_trap(c);
            c->last_expr_is_float = 0;
            *out_temp = VREG_0;
            return true;
        }

        /* __builtin_{ffs,ctz,clz,clrsb,popcount,parity}{,l,ll}(x) -
         * 32-bit and 64-bit integer bit manipulation builtins.
         * Constant arguments fold at compile time; runtime arguments
         * use branchless binary-search / SWAR sequences (~20-40 IR ops). */
        if (callee_is_ident && callee->s.str &&
            zc_is_bitop_builtin_name(callee->s.str)) {
            ASTRef b_arg = node->right;
            const char *bname = callee->s.str;
            int nbits = zc_bitop_width(c, bname);
            int64_t cval = 0;
            if (zc_ast_const_eval(c, arena, b_arg, &cval)) {
                int folded = zc_eval_bitop_const(bname, (uint64_t)cval, nbits);
                int t_res = c->next_temp_id++;
                ir_emit_mov_imm(c, t_res, folded);
                c->last_expr_is_float = 0;
                *out_temp = t_res;
                return true;
            }
            int t_x = lower_expr(c, arena, b_arg);
            int t_result = 0;
            if (strncmp(bname, "__builtin_ctz", 13) == 0) {
                t_result = zc_emit_ctz_nbits(c, t_x, nbits);
            } else if (strncmp(bname, "__builtin_clz", 13) == 0) {
                t_result = zc_emit_clz_nbits(c, t_x, nbits);
            } else if (strncmp(bname, "__builtin_clrsb", 15) == 0) {
                t_result = zc_emit_clrsb_nbits(c, t_x, nbits);
            } else if (strncmp(bname, "__builtin_popcount", 18) == 0) {
                t_result = zc_emit_popcount_nbits(c, t_x, nbits);
            } else if (strncmp(bname, "__builtin_parity", 16) == 0) {
                int t_pc = zc_emit_popcount_nbits(c, t_x, nbits);
                int t_one = c->next_temp_id++;
                ir_emit_mov_imm(c, t_one, 1);
                t_result = c->next_temp_id++;
                ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)t_result,
                                   .temp_src1 = (uint16_t)t_pc, .temp_src2 = (uint16_t)t_one});
            } else {
                /* __builtin_ffs / ffsl / ffsll */
                int t_in = c->next_temp_id++;
                ir_emit_mov_reg(c, t_in, t_x);
                if (nbits == 32) ir_emit_zext(c, t_in, 4);
                int t_ctz = zc_emit_ctz_nbits(c, t_in, nbits);
                int t_one = c->next_temp_id++;
                ir_emit_mov_imm(c, t_one, 1);
                int t_zero = c->next_temp_id++;
                ir_emit_mov_imm(c, t_zero, 0);
                int t_plus1 = c->next_temp_id++;
                ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint16_t)t_plus1,
                                   .temp_src1 = (uint16_t)t_ctz, .temp_src2 = (uint16_t)t_one});
                int t_nz = c->next_temp_id++;
                ir_emit_cmp(c, t_in, t_zero);
                ir_emit(c, (Instr){.op = OP_COND_SET, .temp_dest = (uint16_t)t_nz,
                                   .temp_src1 = (uint16_t)t_in, .temp_src2 = (uint16_t)t_zero,
                                   .cond = CC_NE});
                t_result = c->next_temp_id++;
                ir_emit(c, (Instr){.op = OP_MUL, .temp_dest = (uint16_t)t_result,
                                   .temp_src1 = (uint16_t)t_plus1, .temp_src2 = (uint16_t)t_nz});
            }
            c->last_expr_is_float = 0;
            *out_temp = t_result;
            return true;
        }

        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_va_arg_pack") == 0 ||
             strcmp(callee->s.str, "__builtin_va_arg_pack_len") == 0)) {
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(c, temp, 0);
            c->last_expr_is_float = 0;
            *out_temp = temp;
            return true;
        }

        /* __builtin_constant_p(expr) returns 1 if expr is a compile-time
         * constant, 0 otherwise (execute/bcp-1). */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_constant_p") == 0) {
            ASTRef cp_arg = node->right;
            int res = zc_node_is_const_expr(c, arena, cp_arg) ? 1 : 0;
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(c, temp, res);
            c->last_expr_is_float = 0;
            *out_temp = temp;
            return true;
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
            ir_emit_mov_imm(c, temp, (int32_t)al);
            c->last_expr_is_float = 0;
            *out_temp = temp;
            return true;
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
                *out_temp = sel;
                return true;
            }
            /* Dynamic: evaluate both, select with a conditional
             * move (cv != 0 ? bv : av). */
            int cv = lower_expr(c, arena, r1);
            int av = (r2 != AST_NULL) ? lower_expr(c, arena, r2) : 0;
            int bv = (r3 != AST_NULL) ? lower_expr(c, arena, r3) : 0;
            int sel = c->next_temp_id++;
            ir_emit_mov_reg(c, sel, av);
            int zt = c->next_temp_id++;
            ir_emit_mov_imm(c, zt, 0);
            ir_emit_cmp(c, cv, zt);
            int nz = c->next_temp_id++;
            ir_emit_cond_set(c, (uint16_t)nz, CC_NE);
            uint16_t done_lbl = (uint16_t)c->ir_next_label++;
            ir_emit_je(c, (uint16_t)nz, done_lbl);
            ir_emit_mov_reg(c, sel, bv);
            ir_emit_label(c, done_lbl);
            c->last_expr_is_float = 0;
            *out_temp = sel;
            return true;
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
            ir_emit_mov_imm(c, temp, class_id);
            c->last_expr_is_float = 0;
            *out_temp = temp;
            return true;
        }

        /* __builtin_bswap16(x) - byte-swap the low 16 bits:
         * result = ((x & 0xFF) << 8) | ((x >> 8) & 0xFF).
         * (c-torture execute/pr97386-1) */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_bswap16") == 0) {
            ASTRef b16_arg = node->right;
            int u_a = lower_expr(c, arena, b16_arg);
            int u_ff = c->next_temp_id++; ir_emit_mov_imm(c, u_ff, 0xFF);
            int u_8  = c->next_temp_id++; ir_emit_mov_imm(c, u_8,  8);
            int u_b0 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)u_b0,
                .temp_src1=(uint16_t)u_a,.temp_src2=(uint16_t)u_ff});
            int u_a8 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint16_t)u_a8,
                .temp_src1=(uint16_t)u_a,.temp_src2=(uint16_t)u_8});
            int u_b1 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)u_b1,
                .temp_src1=(uint16_t)u_a8,.temp_src2=(uint16_t)u_ff});
            int u_b0s = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint16_t)u_b0s,
                .temp_src1=(uint16_t)u_b0,.temp_src2=(uint16_t)u_8});
            int u_result = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint16_t)u_result,
                .temp_src1=(uint16_t)u_b0s,.temp_src2=(uint16_t)u_b1});
            c->last_expr_is_float = 0;
            *out_temp = u_result;
            return true;
        }

        /* __builtin_bswap32(x) - byte-swap a 32-bit value.
         * result = ((x & 0xFF) << 24) | ((x >> 8) & 0xFF) << 16 |
         *          ((x >> 16) & 0xFF) << 8  | (x >> 24) */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_bswap32") == 0) {
            ASTRef b_arg = node->right;
            int t_a = lower_expr(c, arena, b_arg);
            int t_ff = c->next_temp_id++; ir_emit_mov_imm(c, t_ff, 0xFF);
            int t_8  = c->next_temp_id++; ir_emit_mov_imm(c, t_8,  8);
            int t_16 = c->next_temp_id++; ir_emit_mov_imm(c, t_16, 16);
            int t_24 = c->next_temp_id++; ir_emit_mov_imm(c, t_24, 24);
            int t_b0 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b0,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_ff});
            int t_a8 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint16_t)t_a8,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_8});
            int t_b1 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b1,
                .temp_src1=(uint16_t)t_a8,.temp_src2=(uint16_t)t_ff});
            int t_a16 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint16_t)t_a16,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_16});
            int t_b2 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b2,
                .temp_src1=(uint16_t)t_a16,.temp_src2=(uint16_t)t_ff});
            int t_a24 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint16_t)t_a24,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_24});
            int t_b3 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b3,
                .temp_src1=(uint16_t)t_a24,.temp_src2=(uint16_t)t_ff});
            int t_b0s = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_b0s,
                .temp_src1=(uint16_t)t_b0,.temp_src2=(uint16_t)t_24});
            int t_b1s = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_b1s,
                .temp_src1=(uint16_t)t_b1,.temp_src2=(uint16_t)t_16});
            int t_b2s = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_b2s,
                .temp_src1=(uint16_t)t_b2,.temp_src2=(uint16_t)t_8});
            int t_r1 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint16_t)t_r1,
                .temp_src1=(uint16_t)t_b0s,.temp_src2=(uint16_t)t_b1s});
            int t_r2 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint16_t)t_r2,
                .temp_src1=(uint16_t)t_r1,.temp_src2=(uint16_t)t_b2s});
            int t_result = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint16_t)t_result,
                .temp_src1=(uint16_t)t_r2,.temp_src2=(uint16_t)t_b3});
            c->last_expr_is_float = 0;
            *out_temp = t_result;
            return true;
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
            ir_emit_fmov_imm(c, (uint16_t)ft, (int64_t)fro_off);
            c->last_expr_is_float = 1;
            *out_temp = ft;
            return true;
        }

        /* __builtin_bswap64(x) - byte-swap a 64-bit value:
         * byte i of the result is byte (7-i) of x.
         * (c-torture execute/bswap-1.c - previously emitted as an
         * undefined external call and segfaulted at runtime). */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_bswap64") == 0) {
            ASTRef b64_arg = node->right;
            int s_a = lower_expr(c, arena, b64_arg);
            int s_ff = c->next_temp_id++; ir_emit_mov_imm(c, s_ff, 0xFF);
            int s_b[8];
            int sh[8] = {0, 8, 16, 24, 32, 40, 48, 56};
            int shd[8] = {56, 48, 40, 32, 24, 16, 8, 0};
            for (int i = 0; i < 8; i++) {
                int s_s = c->next_temp_id++; ir_emit_mov_imm(c, s_s, sh[i]);
                int s_t = c->next_temp_id++;
                ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint16_t)s_t,
                    .temp_src1=(uint16_t)s_a,.temp_src2=(uint16_t)s_s});
                s_b[i] = c->next_temp_id++;
                ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)s_b[i],
                    .temp_src1=(uint16_t)s_t,.temp_src2=(uint16_t)s_ff});
            }
            int s_r = c->next_temp_id++;
            for (int i = 0; i < 8; i++) {
                int s_s = c->next_temp_id++; ir_emit_mov_imm(c, s_s, shd[i]);
                int s_t = c->next_temp_id++;
                ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint16_t)s_t,
                    .temp_src1=(uint16_t)s_b[i],.temp_src2=(uint16_t)s_s});
                if (i == 0) {
                    ir_emit_mov_reg(c, s_r, s_t);
                    continue;
                }
                ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint16_t)s_r,
                    .temp_src1=(uint16_t)s_r,.temp_src2=(uint16_t)s_t});
            }
            c->last_expr_is_float = 0;
            *out_temp = s_r;
            return true;
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
            ir_emit_fmov_imm(c, (uint16_t)temp, (int64_t)rodata_offset);
            c->last_expr_is_float = 1;
            *out_temp = temp;
            return true;
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
            ir_emit_fmov_imm(c, (uint16_t)temp, (int64_t)rodata_offset);
            c->last_expr_is_float = 1;
            *out_temp = temp;
            return true;
        }

        /* llabs() / labs() / imaxabs() / abs() - integer absolute value.
         *
         * GCC semantics (calibrated against gcc-14 at -O0/-O1/-O2, 2026):
         * the builtin is used at every builtin-recognized call site -
         * even when the program declares/defines the function itself and
         * even for non-constant (volatile, global) arguments - as long as
         * it is not disabled with -fno-builtin / -fno-builtin-<name>.
         * GCC emits the inline abs sequence (neg/cmovs) at every -O level
         * and never calls the user's definition from those sites (the user
         * function still exists in the binary; -fno-builtin[-name] forces
         * the real call in every case, even for constant arguments). The
         * explicit __builtin_<name> form always uses the builtin, even
         * under -fno-builtin. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_llabs") == 0 ||
             strcmp(callee->s.str, "llabs") == 0 ||
             strcmp(callee->s.str, "__builtin_labs") == 0 ||
             strcmp(callee->s.str, "labs") == 0 ||
             strcmp(callee->s.str, "__builtin_imaxabs") == 0 ||
             strcmp(callee->s.str, "imaxabs") == 0 ||
             strcmp(callee->s.str, "__builtin_abs") == 0 ||
             strcmp(callee->s.str, "abs") == 0)) {
            const char *bname = callee->s.str;
            const char *bplain =
                (strncmp(bname, "__builtin_", 10) == 0) ? bname + 10 : bname;
            bool b_builtin_form = (bname != bplain);
            bool b_builtin = b_builtin_form || !zero_cc_no_builtin(c, bplain);
            if (b_builtin) {
            bool is_32bit = (strcmp(callee->s.str, "__builtin_abs") == 0 ||
                             strcmp(callee->s.str, "abs") == 0);
            int arg_temp = lower_expr(c, arena, node->right);
            if (is_32bit) {
                ir_emit_sext(c, arg_temp, 4);
            }
            int zero_temp = c->next_temp_id++;
            ir_emit_mov_imm(c, zero_temp, 0);
            int cond_temp = c->next_temp_id++;
            ir_emit_cmp(c, arg_temp, zero_temp);
            ir_emit_cond_set(c, cond_temp, CC_LT);
            uint16_t pos_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;
            ir_emit_je(c, (uint16_t)cond_temp, pos_label);
            int neg_temp = c->next_temp_id++;
            ir_emit_sub(c, neg_temp, zero_temp, arg_temp);
            int res_temp = c->next_temp_id++;
            ir_emit_mov_reg(c, res_temp, neg_temp);
            ir_emit_jmp(c, end_label);
            ir_emit_label(c, pos_label);
            ir_emit_mov_reg(c, res_temp, arg_temp);
            ir_emit_label(c, end_label);
            if (is_32bit) {
                ir_emit_sext(c, res_temp, 4);
            }
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
            } /* b_builtin */
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
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)arg_temp);
                arg_temp = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore(c, (uint16_t)arg_temp, TEMP_NONE, -slot);
            int int_temp = c->next_temp_id++;
            ir_emit_load(c, int_temp, BE(c)->fp_reg, -slot);
            int mask_temp = c->next_temp_id++;
            ir_emit_mov_imm(c, mask_temp, 0x7FFFFFFFFFFFFFFFLL);
            int abs_int = c->next_temp_id++;
            ir_emit_and(c, abs_int, int_temp, mask_temp);
            ir_emit_store(c, abs_int, BE(c)->fp_reg, -slot);
            int res_temp = c->next_temp_id++;
            ir_emit_fload(c, (uint16_t)res_temp, TEMP_NONE, -slot);
            c->last_expr_is_float = 1;
            *out_temp = res_temp;
            return true;
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
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)arg_temp);
                arg_temp = conv;
            }
            int two_i = c->next_temp_id++;
            ir_emit_mov_imm(c, two_i, 2);
            int two_f = c->next_temp_id++;
            ir_emit_cvtsi2sd(c, (uint16_t)two_f, (uint16_t)two_i);

            int zero_i = c->next_temp_id++;
            ir_emit_mov_imm(c, zero_i, 0);
            int zero_f = c->next_temp_id++;
            ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);

            int mul_temp = c->next_temp_id++;
            ir_emit_fmul(c, (uint16_t)mul_temp, (uint16_t)arg_temp, (uint16_t)two_f);

            int c1 = c->next_temp_id++;
            ir_emit_cmp(c, arg_temp, zero_f);
            ir_emit_cond_set(c, c1, CC_NE);

            int c2 = c->next_temp_id++;
            ir_emit_cmp(c, mul_temp, arg_temp);
            ir_emit_cond_set(c, c2, CC_EQ);

            int res_temp = c->next_temp_id++;
            ir_emit_and(c, res_temp, c1, c2);
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
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
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)arg_temp);
                arg_temp = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore(c, (uint16_t)arg_temp, TEMP_NONE, -slot);
            int int_temp = c->next_temp_id++;
            ir_emit_load(c, int_temp, BE(c)->fp_reg, -slot);
            int shift_imm = c->next_temp_id++;
            ir_emit_mov_imm(c, shift_imm, 63);
            int res_temp = c->next_temp_id++;
            ir_emit_shr(c, res_temp, int_temp, shift_imm);
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
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
                    ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_x);
                    t_x = conv;
                }
                int t_y = lower_expr(c, arena, ia2);
                if (!c->last_expr_is_float) {
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_y);
                    t_y = conv;
                }
                int res_temp = c->next_temp_id++;
                if (strncmp(bn, "__builtin_isunordered", 19) == 0) {
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, 12); /* CC_FP_UNORD */
                } else if (strncmp(bn, "__builtin_islessgreater", 21) == 0) {
                    int t_lt = c->next_temp_id++;
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, t_lt, CC_LT);
                    int t_gt = c->next_temp_id++;
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, t_gt, CC_GT);
                    ir_emit_or(c, res_temp, t_lt, t_gt);
                } else if (strncmp(bn, "__builtin_islessequal", 19) == 0) {
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, CC_LE);
                } else if (strncmp(bn, "__builtin_isgreaterequal", 22) == 0) {
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, CC_GE);
                } else if (strncmp(bn, "__builtin_isless", 14) == 0) {
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, CC_LT);
                } else { /* __builtin_isgreater[f/l] */
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, CC_GT);
                }
                c->last_expr_is_float = 0;
                *out_temp = res_temp;
                return true;
            }
        }

        /* __builtin_isnan(x) - NaN is the only value that is not
         * equal to itself. */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_isnan", 13) == 0) {
            int t_x = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int res_temp = c->next_temp_id++;
            ir_emit_cmp(c, t_x, t_x);
            ir_emit_cond_set(c, res_temp, 12); /* CC_FP_UNORD (setp) */
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
        }

        /* __builtin_isfinite(x) - neither inf nor NaN:
         * (x * 0 == 0) && (x == x).  x*0 is NaN for inf/NaN (unordered
         * -> false), 0 for finite (ordered -> true). */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_isfinite", 18) == 0) {
            int t_x = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int zero_i = c->next_temp_id++;
            ir_emit_mov_imm(c, zero_i, 0);
            int zero_f = c->next_temp_id++;
            ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
            int t_x0 = c->next_temp_id++;
            ir_emit_fmul(c, (uint16_t)t_x0, (uint16_t)t_x, (uint16_t)zero_f);
            int t_zero = c->next_temp_id++;
            ir_emit_cmp(c, t_x0, zero_f);
            ir_emit_cond_set(c, t_zero, CC_EQ);
            int t_eq = c->next_temp_id++;
            ir_emit_cmp(c, t_x, t_x);
            ir_emit_cond_set(c, t_eq, CC_EQ);
            int res_temp = c->next_temp_id++;
            ir_emit_and(c, res_temp, t_zero, t_eq);
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
        }

        /* __builtin_isnormal(x) - neither zero, subnormal, inf, nor
         * NaN: x != 0 && x == x && x/2^30 != x (subnormals flush to 0
         * at /2^30; inf scales to itself). */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_isnormal", 18) == 0) {
            int t_x = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int zero_i = c->next_temp_id++;
            ir_emit_mov_imm(c, zero_i, 0);
            int zero_f = c->next_temp_id++;
            ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
            int p230_i = c->next_temp_id++;
            ir_emit_mov_imm(c, p230_i, 1073741824); /* 2^30 */
            int p230_f = c->next_temp_id++;
            ir_emit_cvtsi2sd(c, (uint16_t)p230_f, (uint16_t)p230_i);
            int t_xd = c->next_temp_id++;
            ir_emit_fdiv(c, (uint16_t)t_xd, (uint16_t)t_x, (uint16_t)p230_f);
            int t_nz = c->next_temp_id++;
            ir_emit_cmp(c, t_x, zero_f);
            ir_emit_cond_set(c, t_nz, CC_NE);
            int t_ne = c->next_temp_id++;
            ir_emit_cmp(c, t_xd, t_x);
            ir_emit_cond_set(c, t_ne, CC_NE);
            int t_eq = c->next_temp_id++;
            ir_emit_cmp(c, t_x, t_x);
            ir_emit_cond_set(c, t_eq, CC_EQ);
            int t_a = c->next_temp_id++;
            ir_emit_and(c, t_a, t_nz, t_ne);
            int res_temp = c->next_temp_id++;
            ir_emit_and(c, res_temp, t_a, t_eq);
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
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
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int t_y = lower_expr(c, arena, ca2);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_y);
                t_y = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore(c, (uint16_t)t_y, TEMP_NONE, -slot);
            int y_int = c->next_temp_id++;
            ir_emit_load(c, y_int, BE(c)->fp_reg, -slot);
            int sh63 = c->next_temp_id++;
            ir_emit_mov_imm(c, sh63, 63);
            int y_sign = c->next_temp_id++;
            ir_emit_shr(c, y_sign, y_int, sh63);
            int sign_mask = c->next_temp_id++;
            ir_emit_shl(c, sign_mask, y_sign, sh63);
            ir_emit_fstore(c, (uint16_t)t_x, TEMP_NONE, -slot);
            int x_int = c->next_temp_id++;
            ir_emit_load(c, x_int, BE(c)->fp_reg, -slot);
            int nosig_mask = c->next_temp_id++;
            ir_emit_mov_imm(c, nosig_mask, 0x7FFFFFFFFFFFFFFFLL);
            int res_int = c->next_temp_id++;
            ir_emit_and(c, res_int, x_int, nosig_mask);
            ir_emit_or(c, res_int, res_int, sign_mask);
            ir_emit_store(c, res_int, BE(c)->fp_reg, -slot);
            int res_temp = c->next_temp_id++;
            ir_emit_fload(c, (uint16_t)res_temp, TEMP_NONE, -slot);
            c->last_expr_is_float = 1;
            *out_temp = res_temp;
            return true;
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
                ir_emit_mov_imm(c, t_ovf, 0);
                c->last_expr_is_float = 0;
                *out_temp = t_ovf;
                return true;
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
                    ZeroCCGlobalVar *gv = find_global_var(c, target->s.str);
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
            ir_emit_mov_reg(c, t_a64, t_a);
            if (aw < 8) {
                if (au) {
                    int m = c->next_temp_id++;
                    ir_emit_mov_imm(c, m, (int64_t)((1ULL << (aw * 8)) - 1));
                    ir_emit_and(c, t_a64, t_a64, m);
                } else {
                    ir_emit_sext(c, t_a64, aw);
                }
            }
            int t_b64 = c->next_temp_id++;
            ir_emit_mov_reg(c, t_b64, t_b);
            if (bw < 8) {
                if (bu) {
                    int m = c->next_temp_id++;
                    ir_emit_mov_imm(c, m, (int64_t)((1ULL << (bw * 8)) - 1));
                    ir_emit_and(c, t_b64, t_b64, m);
                } else {
                    ir_emit_sext(c, t_b64, bw);
                }
            }
            int t_res = c->next_temp_id++;
            if (is_add) {
                ir_emit(c, (Instr){.op=OP_ADD,.temp_dest=(uint16_t)t_res,
                    .temp_src1=(uint16_t)t_a64,.temp_src2=(uint16_t)t_b64});
            } else if (is_sub) {
                ir_emit(c, (Instr){.op=OP_SUB,.temp_dest=(uint16_t)t_res,
                    .temp_src1=(uint16_t)t_a64,.temp_src2=(uint16_t)t_b64});
            } else {
                ir_emit(c, (Instr){.op=OP_MUL,.temp_dest=(uint16_t)t_res,
                    .temp_src1=(uint16_t)t_a64,.temp_src2=(uint16_t)t_b64});
            }
            if (width == 1) ir_emit_store_byte(c, t_res, t_ptr);
            else if (width == 2) ir_emit_store_16(c, t_res, t_ptr);
            else if (width == 8) ir_emit_store_64(c, t_res, t_ptr);
            else ir_emit_store_32(c, t_res, t_ptr);
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
                    ir_emit_mov_imm(c, t_lim, (int64_t)limit);
                    int t_bz = zc_ovf_cmp_set_imm(c, t_b64, 0, CC_EQ);
                    int lab_no = c->ir_next_label++;
                    ir_emit_jne(c, (uint16_t)t_bz, (uint16_t)lab_no);
                    int t_q = c->next_temp_id++;
                    ir_emit_div(c, t_q, t_lim, t_b64);
                    ir_mark_last_unsigned(c, 1);
                    int t_gt = zc_ovf_cmp_set(c, t_a64, t_q, CC_FLT_GT);
                    ir_emit_label(c, lab_no);
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
                    ir_emit_jne(c, (uint16_t)t_az, (uint16_t)lab_no);
                    int t_q = c->next_temp_id++;
                    ir_emit_div(c, t_q, t_res, t_a64);
                    int t_ne = zc_ovf_cmp_set(c, t_q, t_b64, CC_NE);
                    ir_emit_label(c, lab_no);
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
            *out_temp = t_ovf;
            return true;
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
                    *out_temp = VREG_0;
                    return true;
                }
                if (sys_arg_count > 7) {
                    diag_report_error_at_token(c,
                        "__zerocc_syscall takes at most 7 arguments (number + 6 operands)");
                    *out_temp = VREG_0;
                    return true;
                }
                ir_emit_syscall(c, sys_arg_count, sys_arg_temps);
                c->last_expr_is_float = 0;
                *out_temp = VREG_0;
                return true;
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
                    ir_emit_mfence(c);
                    res_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, res_temp, 0);
                } else if (kind == ZC_AT_IS_LOCK_FREE) {
                    /* C11 7.17.2: aligned integer atomics up to 8 bytes are
                     * lock-free on x86-64. */
                    res_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, res_temp, 1);
                } else {
                    int ptr_temp = lower_expr(c, arena, a0);
                    switch (kind) {
                    case ZC_AT_LOAD: {
                        res_temp = c->next_temp_id++;
                        if (width == 1) ir_emit_load_byte(c, res_temp, ptr_temp);
                        else if (width == 2) ir_emit_load_16(c, res_temp, ptr_temp);
                        else if (width == 8) ir_emit_deref(c, res_temp, ptr_temp);
                        else ir_emit_load_32(c, res_temp, ptr_temp);
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
                        ir_emit_atomic_xchg(c, res_temp, ptr_temp, val_temp, width);
                        break;
                    }
                    case ZC_AT_STORE_0: {
                        /* __sync_lock_release: store 0 at the pointee. */
                        int zero_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, zero_temp, 0);
                        if (width == 1) ir_emit_store_byte(c, zero_temp, ptr_temp);
                        else if (width == 2) ir_emit_store_16(c, zero_temp, ptr_temp);
                        else if (width == 8) ir_emit_store_64(c, zero_temp, ptr_temp);
                        else ir_emit_store_32(c, zero_temp, ptr_temp);
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
                            ir_emit_neg(c, neg_temp, val_temp);
                            val_temp = neg_temp;
                        }
                        int val_copy = c->next_temp_id++;
                        ir_emit_mov_reg(c, val_copy, val_temp);
                        int old_temp = c->next_temp_id++;
                        ir_emit_atomic_xadd(c, old_temp, ptr_temp, val_temp, width);
                        if (kind == ZC_AT_FETCH_ADD || kind == ZC_AT_FETCH_SUB) {
                            res_temp = old_temp;
                        } else {
                            res_temp = c->next_temp_id++;
                            if (kind == ZC_AT_ADD_FETCH) ir_emit_add(c, res_temp, old_temp, val_copy);
                            else if (kind == ZC_AT_SUB_FETCH) ir_emit_add(c, res_temp, old_temp, val_copy);
                            else ir_emit_sub(c, res_temp, old_temp, val_copy);
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
                        if (kind == ZC_AT_FETCH_OR || kind == ZC_AT_OR_FETCH) ir_emit_atomic_or(c, old_temp, ptr_temp, val_temp, width);
                        else if (kind == ZC_AT_FETCH_AND || kind == ZC_AT_AND_FETCH) ir_emit_atomic_and(c, old_temp, ptr_temp, val_temp, width);
                        else ir_emit_atomic_xor(c, old_temp, ptr_temp, val_temp, width);
                        if (kind == ZC_AT_OR_FETCH || kind == ZC_AT_AND_FETCH || kind == ZC_AT_XOR_FETCH) {
                            res_temp = c->next_temp_id++;
                            if (kind == ZC_AT_OR_FETCH) ir_emit_or(c, res_temp, old_temp, val_temp);
                            else if (kind == ZC_AT_AND_FETCH) ir_emit_and(c, res_temp, old_temp, val_temp);
                            else ir_emit_xor(c, res_temp, old_temp, val_temp);
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
                        ir_emit_atomic_cmpxchg(c, res_temp, ptr_temp, exp_temp, des_temp, width);
                        break;
                    }
                    case ZC_AT_CAS_BOOL_SYNC: {
                        /* __sync_bool_compare_and_swap(ptr, expected, desired) */
                        int exp_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, exp_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        int des_temp = lower_expr(c, arena, a2);
                        zc_atomic_extend_value(c, des_temp, a2_node, ast_node_get_typeinfo(c, a2_node));
                        res_temp = c->next_temp_id++;
                        ir_emit_atomic_cmpxchg_bool(c, res_temp, ptr_temp, exp_temp, des_temp, width);
                        break;
                    }
                    case ZC_AT_CAS_BOOL: {
                        /* __atomic_compare_exchange[_n](ptr, &expected, desired, ...) */
                        int exp_ptr_temp = lower_expr(c, arena, a1);
                        int exp_temp = c->next_temp_id++;
                        if (width == 1) ir_emit_load_byte(c, exp_temp, exp_ptr_temp);
                        else if (width == 2) ir_emit_load_16(c, exp_temp, exp_ptr_temp);
                        else if (width == 8) ir_emit_deref(c, exp_temp, exp_ptr_temp);
                        else ir_emit_load_32(c, exp_temp, exp_ptr_temp);
                        TypeInfo *epti = ast_node_get_typeinfo(c, a1_node);
                        if (epti && epti->kind == TYPE_KIND_POINTER && epti->details.pointer.base) {
                            lower_extend_narrow(c, exp_temp, epti->details.pointer.base);
                        }
                        int des_temp = lower_expr(c, arena, a2);
                        zc_atomic_extend_value(c, des_temp, a2_node, ast_node_get_typeinfo(c, a2_node));
                        int actual_old = c->next_temp_id++;
                        ir_emit_atomic_cmpxchg(c, actual_old, ptr_temp, exp_temp, des_temp, width);
                        if (width == 1) ir_emit_store_byte(c, actual_old, exp_ptr_temp);
                        else if (width == 2) ir_emit_store_16(c, actual_old, exp_ptr_temp);
                        else if (width == 8) ir_emit_store_64(c, actual_old, exp_ptr_temp);
                        else ir_emit_store_32(c, actual_old, exp_ptr_temp);
                        res_temp = c->next_temp_id++;
                        ir_emit_cmp(c, actual_old, exp_temp);
                        ir_emit_cond_set(c, res_temp, CC_EQ);
                        break;
                    }
                    case ZC_AT_TEST_AND_SET: {
                        /* exchange(ptr, 1), return (old != 0). */
                        int one_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, one_temp, 1);
                        int zero_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, zero_temp, 0);
                        int old_temp = c->next_temp_id++;
                        ir_emit_atomic_xchg(c, old_temp, ptr_temp, one_temp, width);
                        res_temp = c->next_temp_id++;
                        ir_emit_cmp(c, old_temp, zero_temp);
                        ir_emit_cond_set(c, res_temp, CC_NE);
                        break;
                    }
                    case ZC_AT_CLEAR: {
                        /* xchg(ptr, 0); the old value is dead (void op). */
                        int zero_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, zero_temp, 0);
                        res_temp = c->next_temp_id++;
                        ir_emit_atomic_xchg(c, res_temp, ptr_temp, zero_temp, width);
                        break;
                    }
                    default:
                        break;
                    }
                }
                c->last_expr_is_float = 0;
                *out_temp = res_temp;
                return true;
            }
        }

    return false;
}
