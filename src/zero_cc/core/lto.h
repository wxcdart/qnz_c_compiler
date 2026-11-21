/* ==========================================================================
 * Whole-Program Link-Time Optimization (-flto) Engine (Sprint 195).
 *
 * Implements two-phase cross-translation-unit LTO across .c sources,
 * Fat-LTO .o objects (.zc.lto section), and Fat-LTO .a archives:
 *   Phase 1: Parse + semantic check + capture AST bodies & locals.
 *   Analysis: Duplicate/tentative merge, global constant propagation,
 *     cross-TU inline safety refinement, 3-round pure const-return &
 *     IPCP fixpoint with constant-branch pruning.
 *   Phase 2: Demand-driven worklist lowering from program roots &
 *     eliminating unreferenced functions/globals.
 * ========================================================================== */
#ifndef ZERO_CC_CORE_LTO_H
#define ZERO_CC_CORE_LTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/core/core.h"
#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/parser.h"

#define ZERO_CC_MAX_LTO_TUS       256
#define ZERO_CC_MAX_LTO_EXT_ROOTS 4096
#define ZERO_CC_LTO_MAX_DEPTH     128

extern void zero_cc_lower_deferred_fn(ZeroCCCompiler *c, ZeroCCFunction *fn);

typedef struct ZeroCCLTOContext {
    ZeroCCCompiler *tus[ZERO_CC_MAX_LTO_TUS];
    int tu_count;
    char *ext_roots[ZERO_CC_MAX_LTO_EXT_ROOTS];
    size_t ext_root_count;
    bool keep_all_exports; /* -shared / -r / export-dynamic */
    bool had_error;
    char error_msg[512];
} ZeroCCLTOContext;

static inline void zero_cc_lto_init(ZeroCCLTOContext *lto) {
    if (!lto) return;
    memset(lto, 0, sizeof(*lto));
}

static inline int zero_cc_lto_add_tu(ZeroCCLTOContext *lto, ZeroCCCompiler *tu) {
    if (!lto || !tu) return -1;
    if (lto->tu_count >= ZERO_CC_MAX_LTO_TUS) {
        lto->had_error = true;
        snprintf(lto->error_msg, sizeof(lto->error_msg),
                 "too many translation units for -flto (max %d)", ZERO_CC_MAX_LTO_TUS);
        return -1;
    }
    tu->tu_index = lto->tu_count;
    lto->tus[lto->tu_count++] = tu;
    return 0;
}

static inline void zero_cc_lto_add_ext_root(ZeroCCLTOContext *lto, const char *sym) {
    if (!lto || !sym || !sym[0]) return;
    for (size_t i = 0; i < lto->ext_root_count; i++) {
        if (strcmp(lto->ext_roots[i], sym) == 0) return;
    }
    if (lto->ext_root_count >= ZERO_CC_MAX_LTO_EXT_ROOTS) return;
    char *copy = strdup(sym);
    if (copy) {
        lto->ext_roots[lto->ext_root_count++] = copy;
    }
}

static inline bool zero_cc_lto_is_ext_root(const ZeroCCLTOContext *lto, const char *sym) {
    if (!lto || !sym || !sym[0]) return false;
    for (size_t i = 0; i < lto->ext_root_count; i++) {
        if (strcmp(lto->ext_roots[i], sym) == 0) return true;
    }
    return false;
}

/* Resolve a non-static function definition across all LTO TUs (prefers strong over weak). */
static inline ZeroCCFunction *zero_cc_lto_find_function_loc(
    const ZeroCCLTOContext *lto, const char *name, int *tu_out, size_t *fn_idx_out) {
    if (!lto || !name || !name[0]) return NULL;
    ZeroCCFunction *weak_hit = NULL;
    int weak_tu = -1;
    size_t weak_idx = 0;
    for (int t = 0; t < lto->tu_count; t++) {
        ZeroCCCompiler *tu = lto->tus[t];
        if (!tu) continue;
        for (size_t i = 0; i < tu->function_count; i++) {
            ZeroCCFunction *fn = &tu->functions[i];
            if (fn->is_static || fn->is_prototype || fn->is_external ||
                fn->is_object || fn->is_nested || fn->lto_eliminated) {
                continue;
            }
            const char *sym = zero_cc_fn_emit_symbol(fn);
            if (strcmp(fn->name, name) == 0 || (sym && strcmp(sym, name) == 0)) {
                if (!fn->is_weak) {
                    if (tu_out) *tu_out = t;
                    if (fn_idx_out) *fn_idx_out = i;
                    return fn;
                }
                if (!weak_hit) {
                    weak_hit = fn;
                    weak_tu = t;
                    weak_idx = i;
                }
            }
        }
    }
    if (weak_hit) {
        if (tu_out) *tu_out = weak_tu;
        if (fn_idx_out) *fn_idx_out = weak_idx;
    }
    return weak_hit;
}

static inline ZeroCCFunction *zero_cc_lto_find_function(const void *lto_ptr, const char *name) {
    return zero_cc_lto_find_function_loc((const ZeroCCLTOContext *)lto_ptr, name, NULL, NULL);
}

/* Resolve a non-static global variable definition across all LTO TUs. */
static inline ZeroCCGlobalVar *zero_cc_lto_find_global_loc(
    const ZeroCCLTOContext *lto, const char *name, int *tu_out, size_t *gv_idx_out) {
    if (!lto || !name || !name[0]) return NULL;
    ZeroCCGlobalVar *weak_hit = NULL;
    int weak_tu = -1;
    size_t weak_idx = 0;
    ZeroCCGlobalVar *tent_hit = NULL;
    int tent_tu = -1;
    size_t tent_idx = 0;
    for (int t = 0; t < lto->tu_count; t++) {
        ZeroCCCompiler *tu = lto->tus[t];
        if (!tu) continue;
        for (size_t i = 0; i < tu->global_var_count; i++) {
            ZeroCCGlobalVar *gv = &tu->global_vars[i];
            if (gv->is_static || gv->lto_eliminated || gv->alias_target[0]) continue;
            if (strcmp(gv->name, name) != 0) continue;
            if (!gv->is_weak && gv->has_initializer) {
                if (tu_out) *tu_out = t;
                if (gv_idx_out) *gv_idx_out = i;
                return gv;
            }
            if (gv->is_weak && !weak_hit) {
                weak_hit = gv;
                weak_tu = t;
                weak_idx = i;
            } else if (!tent_hit) {
                tent_hit = gv;
                tent_tu = t;
                tent_idx = i;
            }
        }
    }
    if (weak_hit) {
        if (tu_out) *tu_out = weak_tu;
        if (gv_idx_out) *gv_idx_out = weak_idx;
        return weak_hit;
    }
    if (tent_hit) {
        if (tu_out) *tu_out = tent_tu;
        if (gv_idx_out) *gv_idx_out = tent_idx;
        return tent_hit;
    }
    return NULL;
}

static inline ZeroCCGlobalVar *zero_cc_lto_find_global(const void *lto_ptr, const char *name) {
    return zero_cc_lto_find_global_loc((const ZeroCCLTOContext *)lto_ptr, name, NULL, NULL);
}

static inline bool zc_lto_is_scalar_int_type(const ZeroCCCompiler *c, const ZeroCCType *t) {
    if (!t || t->is_volatile || t->is_complex || t->is_func_ptr ||
        t->is_long_double || t->is_float128) {
        return false;
    }
    switch (t->tag) {
        case TYPE_BOOL:
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_INT32:
        case TYPE_UINT32:
        case TYPE_ENUM:
            return true;
        case TYPE_INT64:
        case TYPE_UINT64:
            return c ? !zc_target_is_32bit(c) : true;
        default:
            return false;
    }
}

static inline bool zc_lto_expr_is_pure_impl(ZeroCCASTArena *arena, ASTRef ref, int depth) {
    if (!arena || ref == AST_NULL || depth > ZERO_CC_LTO_MAX_DEPTH) return true;
    const ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return true;
    switch (n->tag) {
        case NODE_ASSIGN:
        case NODE_ASM:
        case NODE_VA_START:
        case NODE_VA_ARG:
            return false;
        case NODE_UNOP:
            if (n->aux.op == TK_INC || n->aux.op == TK_DEC) return false;
            break;
        case NODE_CALL: {
            const ZeroCCASTNode *cal = zero_cc_ast_get(arena, n->left);
            if (!cal || cal->tag != NODE_IDENT || !cal->s.str) return false;
            if (strcmp(cal->s.str, "__builtin_constant_p") != 0 &&
                strcmp(cal->s.str, "__builtin_object_size") != 0 &&
                strcmp(cal->s.str, "__builtin_choose_expr") != 0) {
                return false;
            }
            break;
        }
        default:
            break;
    }
    if (!zc_lto_expr_is_pure_impl(arena, n->left, depth + 1)) return false;
    if (!zc_lto_expr_is_pure_impl(arena, n->right, depth + 1)) return false;
    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        if (!zc_lto_expr_is_pure_impl(arena, (ASTRef)n->aux.alt, depth + 1)) return false;
    }
    return true;
}

static inline bool zero_cc_lto_args_are_pure(ZeroCCASTArena *arena, ASTRef arg_head) {
    ASTRef cur = arg_head;
    int guard = 0;
    while (cur != AST_NULL && guard++ < 64) {
        const ZeroCCASTNode *an = zero_cc_ast_get(arena, cur);
        if (!an) break;
        /* Only check argument subtree (not chained siblings all over again). */
        if (!zc_lto_expr_is_pure_impl(arena, an->left, 0)) return false;
        if (an->tag == NODE_ASSIGN || an->tag == NODE_ASM ||
            an->tag == NODE_VA_START || an->tag == NODE_VA_ARG) {
            return false;
        }
        if (an->tag == NODE_UNOP && (an->aux.op == TK_INC || an->aux.op == TK_DEC)) return false;
        if (an->tag == NODE_CALL) {
            const ZeroCCASTNode *cal = zero_cc_ast_get(arena, an->left);
            if (!cal || cal->tag != NODE_IDENT || !cal->s.str) return false;
            if (strcmp(cal->s.str, "__builtin_constant_p") != 0 &&
                strcmp(cal->s.str, "__builtin_object_size") != 0 &&
                strcmp(cal->s.str, "__builtin_choose_expr") != 0) {
                return false;
            }
        }
        if (!zc_lto_expr_is_pure_impl(arena, an->right, 0)) return false;
        if ((an->tag == NODE_IF || an->tag == NODE_TERNARY) &&
            !zc_lto_expr_is_pure_impl(arena, (ASTRef)an->aux.alt, 0)) {
            return false;
        }
        cur = an->next;
    }
    return true;
}

/* Check whether identifier matches a captured local/param in cur_fn (and return param idx or -1). */
static inline int zc_lto_lookup_local_param(const ZeroCCFunction *cur_fn, const char *name,
                                            bool *is_local_or_shadowed_out) {
    if (is_local_or_shadowed_out) *is_local_or_shadowed_out = false;
    if (!cur_fn || !name || !name[0]) return -1;
    int matches = 0;
    int param_idx = -1;
    for (size_t i = 0; i < cur_fn->lto_local_count; i++) {
        const ZeroCCSym *sym = &cur_fn->lto_locals[i];
        if (strcmp(sym->name, name) == 0) {
            matches++;
            if (sym->is_param && sym->param_reg_idx >= 0 && sym->param_reg_idx < 16) {
                param_idx = sym->param_reg_idx;
            }
        }
    }
    if (matches > 0 && is_local_or_shadowed_out) {
        *is_local_or_shadowed_out = true;
    }
    if (matches == 1 && param_idx >= 0) return param_idx;
    return -1;
}

static inline int64_t zc_lto_cast_int_val(const TypeInfo *ti, int64_t v) {
    if (!ti) return v;
    bool is_u = (ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
    switch (ti->kind) {
        case TYPE_KIND_BOOL:
            return v ? 1 : 0;
        case TYPE_KIND_CHAR:
            return is_u ? (int64_t)(uint8_t)v : (int64_t)(int8_t)v;
        case TYPE_KIND_SHORT:
            return is_u ? (int64_t)(uint16_t)v : (int64_t)(int16_t)v;
        case TYPE_KIND_INT:
            return is_u ? (int64_t)(uint32_t)v : (int64_t)(int32_t)v;
        case TYPE_KIND_LONG:
            if (type_size((TypeInfo *)ti) == 4) {
                return is_u ? (int64_t)(uint32_t)v : (int64_t)(int32_t)v;
            }
            return v;
        default:
            return v;
    }
}

static inline bool zero_cc_lto_eval_expr(
    const ZeroCCLTOContext *lto, ZeroCCCompiler *tu, const ZeroCCFunction *cur_fn,
    ZeroCCASTArena *arena, ASTRef ref, int64_t *out, int depth) {
    if (!arena || ref == AST_NULL || !out || depth > ZERO_CC_LTO_MAX_DEPTH) return false;
    const ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;
    switch (n->tag) {
        case NODE_INT:
        case NODE_BOOL:
            *out = n->int_val;
            return true;
        case NODE_CAST: {
            int64_t sub = 0;
            if (!zero_cc_lto_eval_expr(lto, tu, cur_fn, arena, n->left, &sub, depth + 1)) {
                return false;
            }
            if (!cur_fn || !cur_fn->perm_arena || n->aux.extra == 0) return false;
            TypeInfo *ti = (TypeInfo *)arena_index_to_ptr((Arena *)cur_fn->perm_arena,
                                                          (size_t)(n->aux.extra - 1));
            if (!ti) return false;
            if (ti->kind != TYPE_KIND_BOOL && ti->kind != TYPE_KIND_CHAR &&
                ti->kind != TYPE_KIND_SHORT && ti->kind != TYPE_KIND_INT &&
                ti->kind != TYPE_KIND_LONG) {
                return false;
            }
            *out = zc_lto_cast_int_val(ti, sub);
            return true;
        }
        case NODE_IDENT: {
            if (!n->s.str) return false;
            bool is_local = false;
            int p_idx = zc_lto_lookup_local_param(cur_fn, n->s.str, &is_local);
            if (p_idx >= 0 && cur_fn->lto_param_is_const[p_idx]) {
                *out = cur_fn->lto_param_const_val[p_idx];
                return true;
            }
            if (is_local) return false;
            ZeroCCEnumConst *ec = tu ? zero_cc_find_enum_const(tu, n->s.str) : NULL;
            if (ec) {
                *out = ec->value;
                return true;
            }
            ZeroCCGlobalVar *gv = tu ? find_global_var(tu, n->s.str) : NULL;
            if ((!gv || (!gv->is_static && !gv->has_initializer)) && lto) {
                ZeroCCGlobalVar *lgv = zero_cc_lto_find_global(lto, n->s.str);
                if (lgv) gv = lgv;
            }
            if (gv && gv->lto_is_const_val) {
                *out = gv->lto_const_val;
                return true;
            }
            return false;
        }
        case NODE_UNOP: {
            int64_t v = 0;
            if (!zero_cc_lto_eval_expr(lto, tu, cur_fn, arena, n->left, &v, depth + 1)) {
                return false;
            }
            switch (n->aux.op) {
                case TK_PLUS: *out = v; return true;
                case TK_MINUS: *out = -v; return true;
                case TK_BANG: *out = !v; return true;
                case TK_TILDE: *out = ~v; return true;
                default: return false;
            }
        }
        case NODE_BINOP: {
            int64_t l = 0, r = 0;
            if (!zero_cc_lto_eval_expr(lto, tu, cur_fn, arena, n->left, &l, depth + 1)) return false;
            if (!zero_cc_lto_eval_expr(lto, tu, cur_fn, arena, n->right, &r, depth + 1)) return false;
            switch (n->aux.op) {
                case TK_PLUS: *out = l + r; return true;
                case TK_MINUS: *out = l - r; return true;
                case TK_STAR: *out = l * r; return true;
                case TK_SLASH: if (r == 0 || (l == INT64_MIN && r == -1)) return false; *out = l / r; return true;
                case TK_PERCENT: if (r == 0 || (l == INT64_MIN && r == -1)) return false; *out = l % r; return true;
                case TK_AMP: *out = l & r; return true;
                case TK_PIPE: *out = l | r; return true;
                case TK_CARET: *out = l ^ r; return true;
                case TK_SHL: if (r < 0 || r >= 64) return false; *out = (int64_t)((uint64_t)l << r); return true;
                case TK_SHR: if (r < 0 || r >= 64) return false; *out = l >> r; return true;
                case TK_EQ: *out = (l == r); return true;
                case TK_NE: *out = (l != r); return true;
                case TK_LT: *out = (l < r); return true;
                case TK_GT: *out = (l > r); return true;
                case TK_LE: *out = (l <= r); return true;
                case TK_GE: *out = (l >= r); return true;
                case TK_LOGAND: *out = (l && r); return true;
                case TK_LOGOR: *out = (l || r); return true;
                default: return false;
            }
        }
        case NODE_TERNARY: {
            int64_t cond = 0;
            if (!zero_cc_lto_eval_expr(lto, tu, cur_fn, arena, n->left, &cond, depth + 1)) return false;
            ASTRef arm = cond ? n->right : (ASTRef)n->aux.alt;
            return zero_cc_lto_eval_expr(lto, tu, cur_fn, arena, arm, out, depth + 1);
        }
        case NODE_CALL: {
            const ZeroCCASTNode *cal = zero_cc_ast_get(arena, n->left);
            if (!cal || cal->tag != NODE_IDENT || !cal->s.str) return false;
            if (!zero_cc_lto_args_are_pure(arena, n->right)) return false;
            ZeroCCFunction *cfn = tu ? zero_cc_find_function(tu, cal->s.str) : NULL;
            if ((!cfn || cfn->is_prototype || cfn->is_external || cfn->is_weak) &&
                (!cfn || !cfn->is_static) && lto) {
                ZeroCCFunction *lfn = zero_cc_lto_find_function(lto, cal->s.str);
                if (lfn) cfn = lfn;
            }
            if (cfn && cfn->lto_is_const_ret) {
                *out = cfn->lto_const_ret_val;
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

typedef struct {
    ZeroCCLTOContext *lto;
    uint8_t *dirty_gv[ZERO_CC_MAX_LTO_TUS];
    uint8_t *addr_taken_fn[ZERO_CC_MAX_LTO_TUS];
} ZeroCCLTOAttrCtx;

static inline void zc_lto_scan_dirty_ast(
    ZeroCCLTOAttrCtx *ac, ZeroCCCompiler *tu, const ZeroCCFunction *cur_fn,
    ZeroCCASTArena *arena, ASTRef ref, bool is_call_tgt, bool is_write_or_addr, int depth) {
    if (!arena || ref == AST_NULL || depth > ZERO_CC_LTO_MAX_DEPTH) return;
    const ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return;

    if (n->tag == NODE_IDENT && n->s.str) {
        bool is_local = false;
        (void)zc_lto_lookup_local_param(cur_fn, n->s.str, &is_local);
        if (!is_local) {
            if (is_write_or_addr) {
                ZeroCCGlobalVar *gv = find_global_var(tu, n->s.str);
                if (gv && gv->is_static) {
                    size_t gi = (size_t)(gv - tu->global_vars);
                    ac->dirty_gv[tu->tu_index][gi] = 1;
                } else {
                    int gt = -1;
                    size_t gidx = 0;
                    if (zero_cc_lto_find_global_loc(ac->lto, n->s.str, &gt, &gidx)) {
                        ac->dirty_gv[gt][gidx] = 1;
                    }
                }
            }
            if (!is_call_tgt) {
                ZeroCCFunction *fn = zero_cc_find_function(tu, n->s.str);
                if (fn && fn->is_static && !fn->is_prototype && !fn->is_external) {
                    size_t fi = (size_t)(fn - tu->functions);
                    ac->addr_taken_fn[tu->tu_index][fi] = 1;
                } else {
                    int ft = -1;
                    size_t fidx = 0;
                    if (zero_cc_lto_find_function_loc(ac->lto, n->s.str, &ft, &fidx)) {
                        ac->addr_taken_fn[ft][fidx] = 1;
                    }
                }
            }
        }
    } else if (n->tag == NODE_ASSIGN) {
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->left, false, true, depth + 1);
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->right, false, false, depth + 1);
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->next, false, false, depth + 1);
        return;
    } else if (n->tag == NODE_UNOP &&
               (n->aux.op == TK_INC || n->aux.op == TK_DEC || n->aux.op == TK_AMP)) {
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->left, false, true, depth + 1);
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->right, false, false, depth + 1);
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->next, false, false, depth + 1);
        return;
    } else if (n->tag == NODE_CALL) {
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->left, true, false, depth + 1);
        ASTRef arg = n->right;
        int guard = 0;
        while (arg != AST_NULL && guard++ < 64) {
            const ZeroCCASTNode *an = zero_cc_ast_get(arena, arg);
            if (!an) break;
            zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, an->left, false, false, depth + 1);
            zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, an->right, false, false, depth + 1);
            if (an->tag == NODE_IF || an->tag == NODE_TERNARY) {
                zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, (ASTRef)an->aux.alt, false, false, depth + 1);
            }
            arg = an->next;
        }
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->next, false, false, depth + 1);
        return;
    } else if (n->tag == NODE_MEMBER || n->tag == NODE_INDEX) {
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->left, false, is_write_or_addr, depth + 1);
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->right, false, false, depth + 1);
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->next, false, false, depth + 1);
        return;
    }

    zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->left, false, false, depth + 1);
    zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->right, false, false, depth + 1);
    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, (ASTRef)n->aux.alt, false, false, depth + 1);
    } else if (n->tag == NODE_FOR) {
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->aux.step, false, false, depth + 1);
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, (ASTRef)n->aux.alt, false, false, depth + 1);
    } else if (n->tag == NODE_CASE) {
        zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->aux.step, false, false, depth + 1);
    }
    zc_lto_scan_dirty_ast(ac, tu, cur_fn, arena, n->next, false, false, depth + 1);
}

static inline bool zc_lto_inline_body_is_cross_safe(
    ZeroCCCompiler *tu, const ZeroCCFunction *fn, ZeroCCASTArena *arena, ASTRef ref, int depth) {
    if (!arena || ref == AST_NULL || depth > ZERO_CC_LTO_MAX_DEPTH) return true;
    const ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return true;
    if (n->tag == NODE_IDENT && n->s.str) {
        bool is_local = false;
        (void)zc_lto_lookup_local_param(fn, n->s.str, &is_local);
        if (!is_local) {
            ZeroCCFunction *sf = zero_cc_find_function(tu, n->s.str);
            if (sf && sf->is_static && !sf->is_prototype && !sf->is_external) {
                return false;
            }
            ZeroCCGlobalVar *sg = find_global_var(tu, n->s.str);
            if (sg && sg->is_static && !sg->lto_is_const_val) {
                return false;
            }
        }
    }
    if (!zc_lto_inline_body_is_cross_safe(tu, fn, arena, n->left, depth + 1)) return false;
    if (!zc_lto_inline_body_is_cross_safe(tu, fn, arena, n->right, depth + 1)) return false;
    if ((n->tag == NODE_IF || n->tag == NODE_TERNARY) &&
        !zc_lto_inline_body_is_cross_safe(tu, fn, arena, (ASTRef)n->aux.alt, depth + 1)) {
        return false;
    }
    if (n->tag == NODE_FOR &&
        (!zc_lto_inline_body_is_cross_safe(tu, fn, arena, n->aux.step, depth + 1) ||
         !zc_lto_inline_body_is_cross_safe(tu, fn, arena, (ASTRef)n->aux.alt, depth + 1))) {
        return false;
    }
    return zc_lto_inline_body_is_cross_safe(tu, fn, arena, n->next, depth + 1);
}

static inline bool zc_lto_stmt_const_return(
    const ZeroCCLTOContext *lto, ZeroCCCompiler *tu, const ZeroCCFunction *fn,
    ZeroCCASTArena *arena, ASTRef ref, int64_t *val_out, int depth) {
    if (!arena || ref == AST_NULL || depth > 16) return false;
    const ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;

    if (n->tag == NODE_BLOCK) {
        ASTRef cur = n->left;
        while (cur != AST_NULL) {
            const ZeroCCASTNode *sn = zero_cc_ast_get(arena, cur);
            if (!sn) break;
            if (sn->tag == NODE_NONE) {
                cur = sn->next;
                continue;
            }
            if (zc_lto_stmt_const_return(lto, tu, fn, arena, cur, val_out, depth + 1)) {
                return true;
            }
            if (sn->tag == NODE_IF && zc_lto_expr_is_pure_impl(arena, sn->left, 0)) {
                int64_t cond = 0;
                if (zero_cc_lto_eval_expr(lto, tu, fn, arena, sn->left, &cond, 0)) {
                    ASTRef taken = cond ? sn->right : (ASTRef)sn->aux.alt;
                    if (taken == AST_NULL) {
                        cur = sn->next;
                        continue;
                    }
                }
            }
            return false;
        }
        return false;
    }
    if (n->tag == NODE_RETURN) {
        if (n->left == AST_NULL) return false;
        if (!zc_lto_expr_is_pure_impl(arena, n->left, 0)) return false;
        return zero_cc_lto_eval_expr(lto, tu, fn, arena, n->left, val_out, 0);
    }
    if (n->tag == NODE_IF) {
        int64_t cond = 0;
        if (!zc_lto_expr_is_pure_impl(arena, n->left, 0)) return false;
        if (!zero_cc_lto_eval_expr(lto, tu, fn, arena, n->left, &cond, 0)) return false;
        ASTRef taken = cond ? n->right : (ASTRef)n->aux.alt;
        return zc_lto_stmt_const_return(lto, tu, fn, arena, taken, val_out, depth + 1);
    }
    return false;
}

typedef struct {
    int tu_idx;
    size_t fn_idx;
} ZeroCCLTOFnItem;

typedef struct {
    ZeroCCLTOContext *lto;
    uint8_t *reach[ZERO_CC_MAX_LTO_TUS];
    ZeroCCLTOFnItem *q;
    size_t q_head;
    size_t q_tail;
    size_t q_cap;
} ZeroCCLTOPreReach;

static inline void zc_lto_prereach_push(ZeroCCLTOPreReach *pr, int tu_idx, size_t fn_idx) {
    if (tu_idx < 0 || tu_idx >= pr->lto->tu_count) return;
    ZeroCCCompiler *tu = pr->lto->tus[tu_idx];
    if (!tu || fn_idx >= tu->function_count) return;
    ZeroCCFunction *fn = &tu->functions[fn_idx];
    if (fn->is_prototype || fn->is_external || fn->is_object || fn->lto_eliminated) return;
    if (pr->reach[tu_idx][fn_idx]) return;
    pr->reach[tu_idx][fn_idx] = 1;
    if (pr->q_tail >= pr->q_cap) {
        size_t ncap = pr->q_cap ? pr->q_cap * 2 : 256;
        ZeroCCLTOFnItem *nq = (ZeroCCLTOFnItem *)realloc(pr->q, ncap * sizeof(ZeroCCLTOFnItem));
        if (!nq) return;
        pr->q = nq;
        pr->q_cap = ncap;
    }
    pr->q[pr->q_tail++] = (ZeroCCLTOFnItem){tu_idx, fn_idx};
}

static inline void zc_lto_prereach_walk_ast(
    ZeroCCLTOPreReach *pr, ZeroCCCompiler *tu, const ZeroCCFunction *cur_fn,
    ZeroCCASTArena *arena, ASTRef ref, int depth) {
    if (!arena || ref == AST_NULL || depth > ZERO_CC_LTO_MAX_DEPTH) return;
    const ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return;

    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, n->left, depth + 1);
        int64_t cond = 0;
        if (zero_cc_lto_eval_expr(pr->lto, tu, cur_fn, arena, n->left, &cond, 0)) {
            ASTRef taken = cond ? n->right : (ASTRef)n->aux.alt;
            ASTRef dead = cond ? (ASTRef)n->aux.alt : n->right;
            if (taken != AST_NULL) {
                zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, taken, depth + 1);
            }
            if (dead != AST_NULL && n->tag == NODE_IF && zero_cc_ast_has_label_goto(arena, dead)) {
                zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, dead, depth + 1);
            }
        } else {
            zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, n->right, depth + 1);
            zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, (ASTRef)n->aux.alt, depth + 1);
        }
        zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, n->next, depth + 1);
        return;
    }

    if (n->tag == NODE_IDENT && n->s.str) {
        bool is_local = false;
        (void)zc_lto_lookup_local_param(cur_fn, n->s.str, &is_local);
        if (!is_local) {
            ZeroCCFunction *f = zero_cc_find_function(tu, n->s.str);
            if (f && !f->is_prototype && !f->is_external && !f->is_object) {
                zc_lto_prereach_push(pr, tu->tu_index, (size_t)(f - tu->functions));
            } else if (!f || !f->is_static) {
                int ft = -1;
                size_t fi = 0;
                if (zero_cc_lto_find_function_loc(pr->lto, n->s.str, &ft, &fi)) {
                    zc_lto_prereach_push(pr, ft, fi);
                }
            }
        }
    }

    zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, n->left, depth + 1);
    zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, n->right, depth + 1);
    if (n->tag == NODE_FOR) {
        zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, n->aux.step, depth + 1);
        zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, (ASTRef)n->aux.alt, depth + 1);
    } else if (n->tag == NODE_CASE) {
        zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, n->aux.step, depth + 1);
    }
    zc_lto_prereach_walk_ast(pr, tu, cur_fn, arena, n->next, depth + 1);
}

typedef struct {
    bool seen[16];
    bool unsat[16];
    int64_t val[16];
} ZeroCCLTOIpcpAccum;

static inline void zc_lto_ipcp_collect_calls(
    ZeroCCLTOContext *lto, ZeroCCCompiler *tu, const ZeroCCFunction *cur_fn,
    ZeroCCASTArena *arena, ASTRef ref,
    ZeroCCLTOIpcpAccum *acc[ZERO_CC_MAX_LTO_TUS], int depth) {
    if (!arena || ref == AST_NULL || depth > ZERO_CC_LTO_MAX_DEPTH) return;
    const ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return;

    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, n->left, acc, depth + 1);
        int64_t cond = 0;
        if (zero_cc_lto_eval_expr(lto, tu, cur_fn, arena, n->left, &cond, 0)) {
            ASTRef taken = cond ? n->right : (ASTRef)n->aux.alt;
            ASTRef dead = cond ? (ASTRef)n->aux.alt : n->right;
            if (taken != AST_NULL) {
                zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, taken, acc, depth + 1);
            }
            if (dead != AST_NULL && n->tag == NODE_IF && zero_cc_ast_has_label_goto(arena, dead)) {
                zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, dead, acc, depth + 1);
            }
        } else {
            zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, n->right, acc, depth + 1);
            zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, (ASTRef)n->aux.alt, acc, depth + 1);
        }
        zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, n->next, acc, depth + 1);
        return;
    }

    if (n->tag == NODE_CALL) {
        const ZeroCCASTNode *cal = zero_cc_ast_get(arena, n->left);
        if (cal && cal->tag == NODE_IDENT && cal->s.str) {
            ZeroCCFunction *target = NULL;
            int tt = -1;
            size_t tfi = 0;
            ZeroCCFunction *local_fn = zero_cc_find_function(tu, cal->s.str);
            if (local_fn && !local_fn->is_prototype && !local_fn->is_external &&
                !local_fn->is_object && !local_fn->lto_eliminated) {
                target = local_fn;
                tt = tu->tu_index;
                tfi = (size_t)(local_fn - tu->functions);
            } else if (!local_fn || !local_fn->is_static) {
                target = zero_cc_lto_find_function_loc(lto, cal->s.str, &tt, &tfi);
            }
            if (target && tt >= 0 && target->lto_defer && !target->is_variadic && !target->is_weak) {
                size_t argc = 0;
                ASTRef arg = n->right;
                int guard = 0;
                while (arg != AST_NULL && guard++ < 64) {
                    const ZeroCCASTNode *an = zero_cc_ast_get(arena, arg);
                    if (!an) break;
                    if (argc < 16 && argc < target->param_count) {
                        int64_t aval = 0;
                        if (zero_cc_lto_eval_expr(lto, tu, cur_fn, arena, arg, &aval, 0)) {
                            if (!acc[tt][tfi].seen[argc]) {
                                acc[tt][tfi].seen[argc] = true;
                                acc[tt][tfi].val[argc] = aval;
                            } else if (acc[tt][tfi].val[argc] != aval) {
                                acc[tt][tfi].unsat[argc] = true;
                            }
                        } else {
                            acc[tt][tfi].unsat[argc] = true;
                        }
                    }
                    argc++;
                    arg = an->next;
                }
                if (argc != target->param_count) {
                    for (size_t p = 0; p < 16 && p < target->param_count; p++) {
                        acc[tt][tfi].unsat[p] = true;
                    }
                }
            }
        }
    }

    zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, n->left, acc, depth + 1);
    zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, n->right, acc, depth + 1);
    if (n->tag == NODE_FOR) {
        zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, n->aux.step, acc, depth + 1);
        zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, (ASTRef)n->aux.alt, acc, depth + 1);
    } else if (n->tag == NODE_CASE) {
        zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, n->aux.step, acc, depth + 1);
    }
    zc_lto_ipcp_collect_calls(lto, tu, cur_fn, arena, n->next, acc, depth + 1);
}

/* Whole-program LTO analysis: duplicate checking, constant globals, cross-TU
 * inlining safety, pure constant-return inference, and IPCP. */
static inline int zero_cc_lto_analyze(ZeroCCLTOContext *lto) {
    if (!lto || lto->tu_count <= 0) return 0;

    /* 1. Duplicate check and tentative/weak cross-TU symbol resolution. */
    for (int i = 0; i < lto->tu_count; i++) {
        ZeroCCCompiler *ta = lto->tus[i];
        if (!ta) continue;
        for (size_t fi = 0; fi < ta->function_count; fi++) {
            ZeroCCFunction *fa = &ta->functions[fi];
            if (fa->is_static || fa->is_prototype || fa->is_external ||
                fa->is_object || fa->is_nested || fa->lto_eliminated) continue;
            for (int j = i + 1; j < lto->tu_count; j++) {
                ZeroCCCompiler *tb = lto->tus[j];
                if (!tb) continue;
                for (size_t fj = 0; fj < tb->function_count; fj++) {
                    ZeroCCFunction *fb = &tb->functions[fj];
                    if (fb->is_static || fb->is_prototype || fb->is_external ||
                        fb->is_object || fb->is_nested || fb->lto_eliminated) continue;
                    if (strcmp(fa->name, fb->name) != 0) continue;
                    if (!fa->is_weak && !fb->is_weak) {
                        lto->had_error = true;
                        snprintf(lto->error_msg, sizeof(lto->error_msg),
                                 "multiple definition of '%s'", fa->name);
                        return -1;
                    }
                    if (fa->is_weak && !fb->is_weak) fa->lto_eliminated = true;
                    else fb->lto_eliminated = true;
                }
            }
        }
        for (size_t gi = 0; gi < ta->global_var_count; gi++) {
            ZeroCCGlobalVar *ga = &ta->global_vars[gi];
            if (ga->is_static || ga->lto_eliminated || ga->alias_target[0]) continue;
            for (int j = i + 1; j < lto->tu_count; j++) {
                ZeroCCCompiler *tb = lto->tus[j];
                if (!tb) continue;
                for (size_t gj = 0; gj < tb->global_var_count; gj++) {
                    ZeroCCGlobalVar *gb = &tb->global_vars[gj];
                    if (gb->is_static || gb->lto_eliminated || gb->alias_target[0]) continue;
                    if (strcmp(ga->name, gb->name) != 0) continue;
                    if (ga->has_initializer && gb->has_initializer) {
                        if (!ga->is_weak && !gb->is_weak) {
                            lto->had_error = true;
                            snprintf(lto->error_msg, sizeof(lto->error_msg),
                                     "multiple definition of '%s'", ga->name);
                            return -1;
                        }
                        if (ga->is_weak && !gb->is_weak) ga->lto_eliminated = true;
                        else gb->lto_eliminated = true;
                    } else if (!ga->has_initializer && gb->has_initializer) {
                        ga->lto_eliminated = true;
                    } else {
                        gb->lto_eliminated = true;
                    }
                }
            }
        }
    }

    ZeroCCLTOAttrCtx ac;
    memset(&ac, 0, sizeof(ac));
    ac.lto = lto;
    bool tu_has_opaque[ZERO_CC_MAX_LTO_TUS];
    memset(tu_has_opaque, 0, sizeof(tu_has_opaque));

    for (int t = 0; t < lto->tu_count; t++) {
        ZeroCCCompiler *tu = lto->tus[t];
        ac.dirty_gv[t] = (uint8_t *)calloc(tu->global_var_count + 1, 1);
        ac.addr_taken_fn[t] = (uint8_t *)calloc(tu->function_count + 1, 1);
        if (tu->file_scope_asm_count > 0) tu_has_opaque[t] = true;
        for (size_t fi = 0; fi < tu->function_count; fi++) {
            ZeroCCFunction *fn = &tu->functions[fi];
            if (!fn->is_prototype && !fn->is_external && !fn->is_object && !fn->lto_defer) {
                tu_has_opaque[t] = true;
                break;
            }
        }
    }

    /* 2. Scan data relocations and ASTs for writes / address-taken globals and functions. */
    for (int t = 0; t < lto->tu_count; t++) {
        ZeroCCCompiler *tu = lto->tus[t];
        for (size_t di = 0; di < tu->data_reloc_count; di++) {
            const ZeroCCDataReloc *dr = &tu->data_relocs[di];
            if (dr->target_kind == 1 && dr->target_global_idx >= 0 &&
                (size_t)dr->target_global_idx < tu->global_var_count) {
                ac.dirty_gv[t][dr->target_global_idx] = 1;
            } else if ((dr->target_kind == 2 || dr->target_kind == 3) && dr->target_extern_name[0]) {
                int gt = -1, ft = -1;
                size_t gi = 0, fi = 0;
                if (zero_cc_lto_find_global_loc(lto, dr->target_extern_name, &gt, &gi)) {
                    ac.dirty_gv[gt][gi] = 1;
                }
                ZeroCCFunction *lf = zero_cc_find_function(tu, dr->target_extern_name);
                if (lf && lf->is_static && !lf->is_prototype && !lf->is_external) {
                    ac.addr_taken_fn[t][(size_t)(lf - tu->functions)] = 1;
                } else if (zero_cc_lto_find_function_loc(lto, dr->target_extern_name, &ft, &fi)) {
                    ac.addr_taken_fn[ft][fi] = 1;
                }
            }
        }
        for (size_t fi = 0; fi < tu->function_count; fi++) {
            ZeroCCFunction *fn = &tu->functions[fi];
            if (!fn->lto_defer || fn->lto_eliminated || !fn->ast_arena || fn->lto_body == AST_NULL) {
                continue;
            }
            zc_lto_scan_dirty_ast(&ac, tu, fn, (ZeroCCASTArena *)fn->ast_arena,
                                  (ASTRef)fn->lto_body, false, false, 0);
        }
    }

    /* 3. Propagate read-only constant globals. */
    for (int t = 0; t < lto->tu_count; t++) {
        ZeroCCCompiler *tu = lto->tus[t];
        for (size_t gi = 0; gi < tu->global_var_count; gi++) {
            ZeroCCGlobalVar *gv = &tu->global_vars[gi];
            if (gv->lto_eliminated || ac.dirty_gv[t][gi] || tu_has_opaque[t]) continue;
            if (gv->is_tls || gv->is_weak || gv->alias_target[0] || gv->section[0]) continue;
            if (!zc_lto_is_scalar_int_type(tu, &gv->type)) continue;
            if (!gv->is_static && !gv->type.is_const &&
                (lto->keep_all_exports || lto->ext_root_count > 0)) {
                continue;
            }
            if (!gv->is_static && zero_cc_lto_is_ext_root(lto, gv->name) && !gv->type.is_const) {
                continue;
            }

            size_t sz = gv->size ? gv->size : gv->type.size;
            if (sz != 1 && sz != 2 && sz != 4 && sz != 8) continue;

            int64_t val = 0;
            bool ok = false;
            if (gv->is_bss) {
                val = 0;
                ok = true;
            } else if (gv->data_offset + sz <= tu->data_section.length && tu->data_section.data) {
                bool has_reloc = false;
                for (size_t di = 0; di < tu->data_reloc_count; di++) {
                    size_t ro = tu->data_relocs[di].data_patch_offset;
                    if (ro + 8 > gv->data_offset && ro < gv->data_offset + sz) {
                        has_reloc = true;
                        break;
                    }
                }
                if (!has_reloc) {
                    uint64_t raw = 0;
                    const uint8_t *p = (const uint8_t *)tu->data_section.data + gv->data_offset;
                    for (size_t b = 0; b < sz; b++) {
                        raw |= ((uint64_t)p[b]) << (b * 8);
                    }
                    switch (gv->type.tag) {
                        case TYPE_BOOL:   val = raw ? 1 : 0; break;
                        case TYPE_INT8:   val = (int64_t)(int8_t)raw; break;
                        case TYPE_UINT8:  val = (int64_t)(uint8_t)raw; break;
                        case TYPE_INT16:  val = (int64_t)(int16_t)raw; break;
                        case TYPE_UINT16: val = (int64_t)(uint16_t)raw; break;
                        case TYPE_INT32:
                        case TYPE_ENUM:   val = (int64_t)(int32_t)raw; break;
                        case TYPE_UINT32: val = (int64_t)(uint32_t)raw; break;
                        default:          val = (int64_t)raw; break;
                    }
                    ok = true;
                }
            }
            if (ok) {
                gv->lto_is_const_val = true;
                gv->lto_const_val = val;
            }
        }
    }

    /* 4. Refine cross-TU inlining safety: reject inline bodies touching local-TU static refs. */
    for (int t = 0; t < lto->tu_count; t++) {
        ZeroCCCompiler *tu = lto->tus[t];
        for (size_t fi = 0; fi < tu->function_count; fi++) {
            ZeroCCFunction *fn = &tu->functions[fi];
            if (!fn->can_inline || !fn->lto_can_cross_inline ||
                fn->is_static || !fn->ast_arena || fn->inline_body == AST_NULL) {
                if (fn->is_static) fn->lto_can_cross_inline = false;
                continue;
            }
            fn->lto_can_cross_inline = zc_lto_inline_body_is_cross_safe(
                tu, fn, (ZeroCCASTArena *)fn->ast_arena, (ASTRef)fn->inline_body, 0);
        }
    }

    /* 5. 3-round pure const-return & IPCP fixpoint with live-branch reachability. */
    ZeroCCLTOIpcpAccum *acc[ZERO_CC_MAX_LTO_TUS];
    memset(acc, 0, sizeof(acc));
    for (int t = 0; t < lto->tu_count; t++) {
        acc[t] = (ZeroCCLTOIpcpAccum *)calloc(lto->tus[t]->function_count + 1,
                                              sizeof(ZeroCCLTOIpcpAccum));
    }

    for (int round = 0; round < 3; round++) {
        bool changed = false;

        /* 5a. Approximate reachability pruning dead functions & provably untaken branches. */
        ZeroCCLTOPreReach pr;
        memset(&pr, 0, sizeof(pr));
        pr.lto = lto;
        for (int t = 0; t < lto->tu_count; t++) {
            pr.reach[t] = (uint8_t *)calloc(lto->tus[t]->function_count + 1, 1);
        }

        for (int t = 0; t < lto->tu_count; t++) {
            ZeroCCCompiler *tu = lto->tus[t];
            for (size_t fi = 0; fi < tu->function_count; fi++) {
                ZeroCCFunction *fn = &tu->functions[fi];
                if (fn->is_prototype || fn->is_external || fn->is_object || fn->lto_eliminated) continue;
                bool is_root = (!fn->is_static && strcmp(fn->name, "main") == 0) ||
                               (!fn->is_static && strcmp(fn->name, "_start") == 0) ||
                               fn->section[0] != '\0' ||
                               (!fn->is_static && lto->keep_all_exports) ||
                               (!fn->is_static && zero_cc_lto_is_ext_root(lto, fn->name)) ||
                               tu_has_opaque[t];
                if (is_root) zc_lto_prereach_push(&pr, t, fi);
            }
            for (size_t di = 0; di < tu->data_reloc_count; di++) {
                const ZeroCCDataReloc *dr = &tu->data_relocs[di];
                if (dr->target_kind == 3 && dr->target_extern_name[0]) {
                    int ft = -1;
                    size_t fi = 0;
                    ZeroCCFunction *lf = zero_cc_find_function(tu, dr->target_extern_name);
                    if (lf && lf->is_static && !lf->is_prototype && !lf->is_external) {
                        zc_lto_prereach_push(&pr, t, (size_t)(lf - tu->functions));
                    } else if (zero_cc_lto_find_function_loc(lto, dr->target_extern_name, &ft, &fi)) {
                        zc_lto_prereach_push(&pr, ft, fi);
                    }
                }
            }
        }
        while (pr.q_head < pr.q_tail) {
            ZeroCCLTOFnItem it = pr.q[pr.q_head++];
            ZeroCCCompiler *tu = lto->tus[it.tu_idx];
            ZeroCCFunction *fn = &tu->functions[it.fn_idx];
            if (fn->lto_defer && fn->ast_arena && fn->lto_body != AST_NULL) {
                zc_lto_prereach_walk_ast(&pr, tu, fn, (ZeroCCASTArena *)fn->ast_arena,
                                         (ASTRef)fn->lto_body, 0);
            }
        }

        /* 5b. Pure constant-return check. */
        for (int t = 0; t < lto->tu_count; t++) {
            ZeroCCCompiler *tu = lto->tus[t];
            for (size_t fi = 0; fi < tu->function_count; fi++) {
                ZeroCCFunction *fn = &tu->functions[fi];
                if (!fn->lto_defer || fn->lto_eliminated || fn->is_weak || fn->is_nested ||
                    fn->section[0] != '\0' || fn->lto_is_const_ret) {
                    continue;
                }
                if (!zc_lto_is_scalar_int_type(tu, &fn->return_type)) continue;
                int64_t rval = 0;
                if (zc_lto_stmt_const_return(lto, tu, fn, (ZeroCCASTArena *)fn->ast_arena,
                                             (ASTRef)fn->lto_body, &rval, 0)) {
                    fn->lto_is_const_ret = true;
                    fn->lto_const_ret_val = rval;
                    changed = true;
                }
            }
        }

        /* 5c. IPCP accumulation across live functions & live branches. */
        for (int t = 0; t < lto->tu_count; t++) {
            memset(acc[t], 0, (lto->tus[t]->function_count + 1) * sizeof(ZeroCCLTOIpcpAccum));
        }
        for (int t = 0; t < lto->tu_count; t++) {
            ZeroCCCompiler *tu = lto->tus[t];
            for (size_t fi = 0; fi < tu->function_count; fi++) {
                ZeroCCFunction *fn = &tu->functions[fi];
                if (!pr.reach[t][fi] || !fn->lto_defer || fn->lto_eliminated ||
                    !fn->ast_arena || fn->lto_body == AST_NULL) {
                    continue;
                }
                zc_lto_ipcp_collect_calls(lto, tu, fn, (ZeroCCASTArena *)fn->ast_arena,
                                          (ASTRef)fn->lto_body, acc, 0);
            }
        }

        for (int t = 0; t < lto->tu_count; t++) {
            ZeroCCCompiler *tu = lto->tus[t];
            for (size_t fi = 0; fi < tu->function_count; fi++) {
                ZeroCCFunction *fn = &tu->functions[fi];
                if (!fn->lto_defer || fn->lto_eliminated || fn->is_variadic || fn->is_weak ||
                    ac.addr_taken_fn[t][fi] || tu_has_opaque[t] ||
                    strcmp(fn->name, "main") == 0 || fn->section[0] != '\0') {
                    continue;
                }
                if (!fn->is_static && (lto->keep_all_exports || zero_cc_lto_is_ext_root(lto, fn->name))) {
                    continue;
                }
                ZeroCCASTArena *fa = (ZeroCCASTArena *)fn->ast_arena;
                ASTRef fbody = (ASTRef)fn->lto_body;
                for (size_t p = 0; p < fn->param_count && p < 16; p++) {
                    bool new_c = false;
                    int64_t new_v = 0;
                    if (acc[t][fi].seen[p] && !acc[t][fi].unsat[p] &&
                        fn->param_types && zc_lto_is_scalar_int_type(tu, &fn->param_types[p]) &&
                        fn->param_names && fn->param_names[p]) {
                        const char *pname = fn->param_names[p];
                        bool shadowed = false;
                        int p_local = zc_lto_lookup_local_param(fn, pname, &shadowed);
                        if (p_local == (int)p &&
                            !zero_cc_ast_addr_of_param(fa, fbody, &pname, 1) &&
                            !zero_cc_ast_assigns_to_param(fa, fbody, &pname, 1)) {
                            new_c = true;
                            new_v = acc[t][fi].val[p];
                        }
                    }
                    if (fn->lto_param_is_const[p] != new_c ||
                        (new_c && fn->lto_param_const_val[p] != new_v)) {
                        fn->lto_param_is_const[p] = new_c;
                        fn->lto_param_const_val[p] = new_v;
                        changed = true;
                    }
                }
            }
        }

        for (int t = 0; t < lto->tu_count; t++) free(pr.reach[t]);
        free(pr.q);
        if (!changed) break;
    }

    for (int t = 0; t < lto->tu_count; t++) {
        free(ac.dirty_gv[t]);
        free(ac.addr_taken_fn[t]);
        free(acc[t]);
    }
    return 0;
}

typedef struct {
    ZeroCCLTOContext *lto;
    uint8_t *live_fn[ZERO_CC_MAX_LTO_TUS];
    uint8_t *live_gv[ZERO_CC_MAX_LTO_TUS];
    ZeroCCLTOFnItem *q;
    size_t q_head;
    size_t q_tail;
    size_t q_cap;
} ZeroCCLTODemandCtx;

static inline void zc_lto_demand_push_fn(ZeroCCLTODemandCtx *dc, int tu_idx, size_t fn_idx) {
    if (tu_idx < 0 || tu_idx >= dc->lto->tu_count) return;
    ZeroCCCompiler *tu = dc->lto->tus[tu_idx];
    if (!tu || fn_idx >= tu->function_count) return;
    ZeroCCFunction *fn = &tu->functions[fn_idx];
    if (fn->is_prototype || fn->is_external || fn->is_object || fn->lto_eliminated) return;
    if (dc->live_fn[tu_idx][fn_idx]) return;
    dc->live_fn[tu_idx][fn_idx] = 1;
    if (dc->q_tail >= dc->q_cap) {
        size_t ncap = dc->q_cap ? dc->q_cap * 2 : 256;
        ZeroCCLTOFnItem *nq = (ZeroCCLTOFnItem *)realloc(dc->q, ncap * sizeof(ZeroCCLTOFnItem));
        if (!nq) return;
        dc->q = nq;
        dc->q_cap = ncap;
    }
    dc->q[dc->q_tail++] = (ZeroCCLTOFnItem){tu_idx, fn_idx};
}

static inline void zc_lto_demand_mark_sym(ZeroCCLTODemandCtx *dc, int owner_tu, const char *name);

static inline void zc_lto_demand_mark_gv(ZeroCCLTODemandCtx *dc, int tu_idx, size_t gv_idx) {
    if (tu_idx < 0 || tu_idx >= dc->lto->tu_count) return;
    ZeroCCCompiler *tu = dc->lto->tus[tu_idx];
    if (!tu || gv_idx >= tu->global_var_count) return;
    ZeroCCGlobalVar *gv = &tu->global_vars[gv_idx];
    if (gv->lto_eliminated || dc->live_gv[tu_idx][gv_idx]) return;
    dc->live_gv[tu_idx][gv_idx] = 1;

    if (gv->alias_target[0]) {
        zc_lto_demand_mark_sym(dc, tu_idx, gv->alias_target);
    }

    size_t gsz = gv->size ? gv->size : gv->type.size;
    if (!gv->is_bss && gsz > 0) {
        for (size_t di = 0; di < tu->data_reloc_count; di++) {
            const ZeroCCDataReloc *dr = &tu->data_relocs[di];
            if (dr->data_patch_offset >= gv->data_offset &&
                dr->data_patch_offset < gv->data_offset + gsz) {
                if (dr->target_kind == 1 && dr->target_global_idx >= 0) {
                    zc_lto_demand_mark_gv(dc, tu_idx, (size_t)dr->target_global_idx);
                } else if ((dr->target_kind == 2 || dr->target_kind == 3) && dr->target_extern_name[0]) {
                    zc_lto_demand_mark_sym(dc, tu_idx, dr->target_extern_name);
                } else if (dr->target_kind == 5 && dr->target_global_idx >= 0) {
                    zc_lto_demand_push_fn(dc, tu_idx, (size_t)dr->target_global_idx);
                }
            }
        }
    }
}

static inline void zc_lto_demand_mark_sym(ZeroCCLTODemandCtx *dc, int owner_tu, const char *name) {
    if (!dc || !name || !name[0]) return;
    ZeroCCCompiler *tu = (owner_tu >= 0 && owner_tu < dc->lto->tu_count) ? dc->lto->tus[owner_tu] : NULL;
    if (tu) {
        ZeroCCFunction *lf = zero_cc_find_function(tu, name);
        if (!lf) lf = zero_cc_find_function_by_symbol(tu, name);
        if (lf && !lf->is_prototype && !lf->is_external && !lf->is_object && !lf->lto_eliminated) {
            zc_lto_demand_push_fn(dc, owner_tu, (size_t)(lf - tu->functions));
        } else if (!lf || !lf->is_static) {
            int ft = -1;
            size_t fi = 0;
            if (zero_cc_lto_find_function_loc(dc->lto, name, &ft, &fi)) {
                zc_lto_demand_push_fn(dc, ft, fi);
            }
        }

        ZeroCCGlobalVar *lg = find_global_var(tu, name);
        if (lg && !lg->lto_eliminated) {
            zc_lto_demand_mark_gv(dc, owner_tu, (size_t)(lg - tu->global_vars));
            return;
        }
    }
    int gt = -1;
    size_t gi = 0;
    if (zero_cc_lto_find_global_loc(dc->lto, name, &gt, &gi)) {
        zc_lto_demand_mark_gv(dc, gt, gi);
    }
}

/* Demand-driven Phase 2 lowering and dead function/global elimination. */
static inline int zero_cc_lto_optimize_and_lower(ZeroCCLTOContext *lto) {
    if (!lto || lto->tu_count <= 0) return 0;

    ZeroCCLTODemandCtx dc;
    memset(&dc, 0, sizeof(dc));
    dc.lto = lto;

    for (int t = 0; t < lto->tu_count; t++) {
        ZeroCCCompiler *tu = lto->tus[t];
        tu->lto_ctx = lto;
        tu->lto_phase = 2;
        dc.live_fn[t] = (uint8_t *)calloc(tu->function_count + 1, 1);
        dc.live_gv[t] = (uint8_t *)calloc(tu->global_var_count + 1, 1);

        /* Non-deferred functions (e.g. GNU nested functions) already lowered in Phase 1. */
        bool has_opaque = (tu->file_scope_asm_count > 0);
        for (size_t fi = 0; fi < tu->function_count; fi++) {
            ZeroCCFunction *fn = &tu->functions[fi];
            if (fn->is_prototype || fn->is_external || fn->is_object || fn->lto_eliminated) continue;
            if (!fn->lto_defer) {
                has_opaque = true;
                zc_lto_demand_push_fn(&dc, t, fi);
            }
            if ((!fn->is_static && strcmp(fn->name, "main") == 0) ||
                (!fn->is_static && strcmp(fn->name, "_start") == 0) ||
                fn->section[0] != '\0' ||
                (!fn->is_static && lto->keep_all_exports) ||
                (!fn->is_static && zero_cc_lto_is_ext_root(lto, fn->name))) {
                zc_lto_demand_push_fn(&dc, t, fi);
            }
        }
        for (size_t gi = 0; gi < tu->global_var_count; gi++) {
            ZeroCCGlobalVar *gv = &tu->global_vars[gi];
            if (gv->lto_eliminated) continue;
            if (gv->section[0] != '\0' || has_opaque ||
                (!gv->is_static && lto->keep_all_exports) ||
                (!gv->is_static && zero_cc_lto_is_ext_root(lto, gv->name))) {
                zc_lto_demand_mark_gv(&dc, t, gi);
            }
        }
        /* Pre-existing relocs from any eagerly-lowered (nested) functions. */
        for (size_t ri = 0; ri < tu->reloc_count; ri++) {
            zc_lto_demand_mark_sym(&dc, t, tu->relocs[ri].target_symbol);
        }
    }

    while (dc.q_head < dc.q_tail) {
        ZeroCCLTOFnItem it = dc.q[dc.q_head++];
        ZeroCCCompiler *tu = lto->tus[it.tu_idx];
        ZeroCCFunction *fn = &tu->functions[it.fn_idx];
        if (fn->lto_defer && !fn->lto_lowered && !fn->lto_eliminated) {
            size_t reloc_before = tu->reloc_count;
            zero_cc_lower_deferred_fn(tu, fn);
            if (tu->had_error) {
                lto->had_error = true;
                if (tu->error_msg[0]) {
                    snprintf(lto->error_msg, sizeof(lto->error_msg), "%s", tu->error_msg);
                }
                break;
            }
            for (size_t ri = reloc_before; ri < tu->reloc_count; ri++) {
                zc_lto_demand_mark_sym(&dc, it.tu_idx, tu->relocs[ri].target_symbol);
            }
            if (tu->emit_asm && tu->ir_arena.buffer) {
                for (size_t ii = 0; ii < tu->ir_arena.count; ii++) {
                    const Instr *in = &tu->ir_arena.buffer[ii];
                    if ((in->op == OP_CALL || in->op == OP_MOV_GLOBAL || in->op == OP_TLS_LEA) &&
                        in->imm64 != 0) {
                        const char *sym = (const char *)(intptr_t)in->imm64;
                        zc_lto_demand_mark_sym(&dc, it.tu_idx, sym);
                    }
                }
            }
        }
    }

    for (int t = 0; t < lto->tu_count; t++) {
        ZeroCCCompiler *tu = lto->tus[t];
        for (size_t fi = 0; fi < tu->function_count; fi++) {
            ZeroCCFunction *fn = &tu->functions[fi];
            if (!fn->is_prototype && !fn->is_external && !fn->is_object &&
                !dc.live_fn[t][fi] && fn->lto_defer && !fn->lto_lowered) {
                fn->lto_eliminated = true;
            }
        }
        for (size_t gi = 0; gi < tu->global_var_count; gi++) {
            ZeroCCGlobalVar *gv = &tu->global_vars[gi];
            if (!dc.live_gv[t][gi] && !gv->lto_eliminated) {
                gv->lto_eliminated = true;
            }
        }
        free(dc.live_fn[t]);
        free(dc.live_gv[t]);
    }
    free(dc.q);
    return lto->had_error ? -1 : 0;
}

static inline void zero_cc_lto_cleanup(ZeroCCLTOContext *lto) {
    if (!lto) return;
    for (size_t i = 0; i < lto->ext_root_count; i++) {
        free(lto->ext_roots[i]);
        lto->ext_roots[i] = NULL;
    }
    lto->ext_root_count = 0;
    for (int t = 0; t < lto->tu_count; t++) {
        if (lto->tus[t]) {
            zero_cc_compiler_free(lto->tus[t]);
            lto->tus[t] = NULL;
        }
    }
    lto->tu_count = 0;
    zero_cc_type_pool_free();
}

#endif /* ZERO_CC_CORE_LTO_H */
