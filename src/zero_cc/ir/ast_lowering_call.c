/* AST Lowering - Call and ABI Lowering (AST to IR) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/parser/vla_struct.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"

/* GNU nested functions are block-scoped: several enclosing functions in
 * the same TU may each define a nested function with the same source
 * name (e.g. three `nested` fns in c-torture execute/20061220-1.c).
 * Resolve a call name by walking the ENCLOSING CHAIN from the function
 * currently being lowered: at each level, a nested function defined in
 * that function with the given name is the callee (innermost level wins,
 * per C block scoping). Falls back to the global function table for
 * top-level names. */
static ZeroCCFunction *zero_cc_resolve_callee(ZeroCCCompiler *c, const char *name) {
    if (c->lowering.current_func_idx < c->function_count) {
        size_t lvl = c->lowering.current_func_idx;
        while (lvl < c->function_count) {
            const ZeroCCFunction *lf = &c->functions[lvl];
            for (size_t f = 0; f < c->function_count; f++) {
                const ZeroCCFunction *nf = &c->functions[f];
                if (!nf->is_nested || nf->nested_enclosing_fn_idx != lvl) continue;
                if (strcmp(nf->name, name) == 0) return (ZeroCCFunction *)nf;
            }
            if (!lf->is_nested || lf->nested_enclosing_fn_idx == (size_t)-1) break;
            lvl = lf->nested_enclosing_fn_idx;
        }
    }
    return zero_cc_find_function(c, name);
}

/* Splice caller's trailing variadic argument AST list in place of
 * __builtin_va_arg_pack() and replace __builtin_va_arg_pack_len() with
 * va_pack_len inside an inlined variadic function body. */
static void zc_ast_splice_va_arg_pack(ZeroCCASTArena *arena, ASTRef ref,
                                      ASTRef va_pack_head, int va_pack_len, int depth) {
    if (!arena || ref == AST_NULL || depth > 128) return;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return;

    if (n->tag == NODE_CALL && n->left != AST_NULL) {
        ZeroCCASTNode *cal = zero_cc_ast_get(arena, n->left);
        if (cal && cal->tag == NODE_IDENT && cal->s.str &&
            strcmp(cal->s.str, "__builtin_va_arg_pack_len") == 0) {
            n->tag = NODE_INT;
            n->int_val = va_pack_len;
            n->left = AST_NULL;
            n->right = AST_NULL;
            if (n->next != AST_NULL) {
                zc_ast_splice_va_arg_pack(arena, n->next, va_pack_head, va_pack_len, depth + 1);
            }
            return;
        }
        /* Check argument list of this call for __builtin_va_arg_pack() */
        ASTRef prev_arg = AST_NULL;
        ASTRef cur_arg = n->right;
        while (cur_arg != AST_NULL) {
            ZeroCCASTNode *an = zero_cc_ast_get(arena, cur_arg);
            if (!an) break;
            ASTRef next_arg = an->next;
            bool is_pack = false;
            if (an->tag == NODE_CALL && an->left != AST_NULL) {
                ZeroCCASTNode *acal = zero_cc_ast_get(arena, an->left);
                if (acal && acal->tag == NODE_IDENT && acal->s.str &&
                    strcmp(acal->s.str, "__builtin_va_arg_pack") == 0) {
                    is_pack = true;
                }
            }
            if (is_pack) {
                ASTRef pack_copy = (va_pack_head != AST_NULL) ?
                    zero_cc_ast_clone(arena, va_pack_head, arena) : AST_NULL;
                n = zero_cc_ast_get(arena, ref);
                if (pack_copy != AST_NULL) {
                    ASTRef tail = pack_copy;
                    while (tail != AST_NULL) {
                        ZeroCCASTNode *tn = zero_cc_ast_get(arena, tail);
                        if (!tn || tn->next == AST_NULL) {
                            if (tn) tn->next = next_arg;
                            break;
                        }
                        tail = tn->next;
                    }
                    if (prev_arg == AST_NULL) {
                        if (n) n->right = pack_copy;
                    } else {
                        ZeroCCASTNode *pn = zero_cc_ast_get(arena, prev_arg);
                        if (pn) pn->next = pack_copy;
                    }
                    prev_arg = tail;
                } else {
                    if (prev_arg == AST_NULL) {
                        if (n) n->right = next_arg;
                    } else {
                        ZeroCCASTNode *pn = zero_cc_ast_get(arena, prev_arg);
                        if (pn) pn->next = next_arg;
                    }
                }
                cur_arg = next_arg;
                continue;
            }
            zc_ast_splice_va_arg_pack(arena, cur_arg, va_pack_head, va_pack_len, depth + 1);
            prev_arg = cur_arg;
            cur_arg = next_arg;
        }
        n = zero_cc_ast_get(arena, ref);
        if (n && n->next != AST_NULL) {
            zc_ast_splice_va_arg_pack(arena, n->next, va_pack_head, va_pack_len, depth + 1);
        }
        return;
    }

    ASTRef l = n->left;
    ASTRef r = n->right;
    ASTRef nx = n->next;
    uint32_t tag = n->tag;
    ASTRef alt = n->aux.alt;
    ASTRef step = n->aux.step;
    if (l != AST_NULL) zc_ast_splice_va_arg_pack(arena, l, va_pack_head, va_pack_len, depth + 1);
    if (r != AST_NULL) zc_ast_splice_va_arg_pack(arena, r, va_pack_head, va_pack_len, depth + 1);
    if ((tag == NODE_IF || tag == NODE_TERNARY || tag == NODE_FOR) && alt != AST_NULL) {
        zc_ast_splice_va_arg_pack(arena, alt, va_pack_head, va_pack_len, depth + 1);
    }
    if (tag == NODE_FOR && step != AST_NULL) {
        zc_ast_splice_va_arg_pack(arena, step, va_pack_head, va_pack_len, depth + 1);
    }
    if (nx != AST_NULL) zc_ast_splice_va_arg_pack(arena, nx, va_pack_head, va_pack_len, depth + 1);
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
        /* Directly-lowered GCC/Clang builtins, intrinsics, and atomics */
        if (callee_is_ident && name) {
            int builtin_temp = 0;
            if (lower_expr_builtin(c, arena, node, name, &builtin_temp)) {
                return builtin_temp;
            }
        }

        if (name && strncmp(name, "__builtin_", 10) == 0) {
            const char *base_fn = name + 10;
            if (strcmp(base_fn, "memcpy") == 0 || strcmp(base_fn, "memset") == 0 ||
                strcmp(base_fn, "memmove") == 0 || strcmp(base_fn, "memcmp") == 0 ||
                strcmp(base_fn, "memchr") == 0 ||
                strcmp(base_fn, "strlen") == 0 || strcmp(base_fn, "strcmp") == 0 ||
                strcmp(base_fn, "strncmp") == 0 || strcmp(base_fn, "strcpy") == 0 ||
                strcmp(base_fn, "strncpy") == 0 || strcmp(base_fn, "strcat") == 0 ||
                strcmp(base_fn, "strchr") == 0 || strcmp(base_fn, "strrchr") == 0 ||
                strcmp(base_fn, "strstr") == 0 || strcmp(base_fn, "strdup") == 0 ||
                strcmp(base_fn, "strndup") == 0 || strcmp(base_fn, "abort") == 0 ||
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
            } else if (strcmp(base_fn, "__snprintf_chk") == 0) {
                /* __builtin___snprintf_chk(s, n, flag, bos, fmt, ...):
                 * evaluate flag & bos for side effects, splice them out, and
                 * lower as snprintf(s, n, fmt, ...). */
                name = "snprintf";
                ASTRef a0 = node->right;
                ZeroCCASTNode *n0 = (a0 != AST_NULL) ? zero_cc_ast_get(arena, a0) : NULL;
                ASTRef a1 = n0 ? n0->next : AST_NULL;
                ZeroCCASTNode *n1 = (a1 != AST_NULL) ? zero_cc_ast_get(arena, a1) : NULL;
                ASTRef a2 = n1 ? n1->next : AST_NULL;
                ZeroCCASTNode *n2 = (a2 != AST_NULL) ? zero_cc_ast_get(arena, a2) : NULL;
                ASTRef a3 = n2 ? n2->next : AST_NULL;
                ZeroCCASTNode *n3 = (a3 != AST_NULL) ? zero_cc_ast_get(arena, a3) : NULL;
                if (n1 && n2 && n3) {
                    (void)lower_expr(c, arena, a2);
                    (void)lower_expr(c, arena, a3);
                    n1 = zero_cc_ast_get(arena, a1);
                    n3 = zero_cc_ast_get(arena, a3);
                    if (n1 && n3) n1->next = n3->next;
                }
            } else if (base_fn[0] == '_' && base_fn[1] == '_') {
                /* FORTIFY-family __builtin___X_chk(args..., bos): when bos
                 * (the LAST argument of this fixed-arity family) folds to
                 * the constant -1, GCC rewrites the call to the plain
                 * X(args...) - no object-size checking is possible. The bos
                 * argument is the __builtin_object_size placeholder and is
                 * dropped unevaluated (GCC computes bos from the dst
                 * argument itself, so the placeholder's side effects do
                 * not run twice - c-torture execute/builtins *chk.c). When
                 * bos is a known size the chk call is kept. The printf
                 * family keeps its existing handling (the __snprintf_chk
                 * branch above; the rest via the general alias below). */
                static const char *const chk_names[] = {
                    "__memcpy_chk", "__mempcpy_chk", "__memmove_chk",
                    "__memset_chk", "__strcpy_chk", "__stpcpy_chk",
                    "__strcat_chk", "__strncpy_chk", "__stpncpy_chk",
                    "__strncat_chk",
                };
                int chk_idx = -1;
                for (size_t ci = 0; ci < sizeof(chk_names) / sizeof(chk_names[0]); ci++) {
                    if (strcmp(base_fn, chk_names[ci]) == 0) { chk_idx = (int)ci; break; }
                }
                if (chk_idx >= 0) {
                    char plain_name[128];
                    snprintf(plain_name, sizeof(plain_name), "%s", base_fn + 2);
                    plain_name[strlen(plain_name) - 4] = '\0'; /* strip _chk */
                    /* The plain name must be a known function (declared or
                     * defined); its function-table entry provides STABLE
                     * name storage - the call IR records the name pointer
                     * during lowering and only copies it into the reloc at
                     * codegen time, so a stack-local would dangle. */
                    ZeroCCFunction *plain_fn = zero_cc_find_function(c, plain_name);
                    ZeroCCASTNode *prev_n = NULL;
                    ASTRef last_ref = AST_NULL;
                    ASTRef a0 = node->right;
                    ZeroCCASTNode *n0 = (a0 != AST_NULL) ? zero_cc_ast_get(arena, a0) : NULL;
                    prev_n = n0;
                    ASTRef cur_ref = n0 ? n0->next : AST_NULL;
                    while (cur_ref != AST_NULL) {
                        ZeroCCASTNode *cur = zero_cc_ast_get(arena, cur_ref);
                        if (!cur) break;
                        if (cur->next == AST_NULL) { last_ref = cur_ref; break; }
                        prev_n = cur;
                        cur_ref = cur->next;
                    }
                    int64_t bos = -2;
                    if (last_ref != AST_NULL && prev_n &&
                        zc_ast_const_eval(c, arena, last_ref, &bos) &&
                        bos == -1 && plain_fn) {
                        prev_n->next = AST_NULL; /* drop the bos placeholder */
                        name = plain_fn->name;
                    } else if (zero_cc_find_function(c, base_fn)) {
                        name = base_fn;
                    } else {
                        name = chk_names[chk_idx];
                    }
                } else {
                    if (zero_cc_find_function(c, base_fn)) {
                        name = base_fn;
                    }
                }
            } else {
                /* General __builtin_X alias: when the base name X is a
                 * known function (declared or defined), the builtin alias
                 * calls X - GCC's behavior for the `__builtin_strpbrk` /
                 * `__builtin_strcspn` / ... alias family (c-torture
                 * execute/builtins).  Unknown base names keep the literal
                 * `__builtin_X` symbol (implicit-declaration path), except
                 * FORTIFY __X_chk aliases (__sprintf_chk, __vsprintf_chk)
                 * whose library target is always X even when undeclared. */
                if (zero_cc_find_function(c, base_fn) ||
                    (strncmp(base_fn, "__", 2) == 0 && strstr(base_fn, "_chk") != NULL)) {
                    name = base_fn;
                }
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

            ZeroCCFunction *callee_fn = callee_is_ident ? zero_cc_resolve_callee(c, name) : NULL;
            ZeroCCSym *callee_sym = (callee_is_ident && !callee_fn) ? zero_cc_find_local(c, name) : NULL;
            /* Indirect call cases: (a) the callee is a local/global
             * function-pointer variable, so the callee is the pointer
             * VALUE stored there; (b) the callee is a general expression
             * (a[i](), p->cb(), ...) - its lowered value IS the function
             * pointer to call through. */
            ZeroCCGlobalVar *callee_gv = (callee_is_ident && !callee_fn && !callee_sym) ? find_global_var(c, name) : NULL;
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
                (c->opt_level >= OPT_LEVEL_O2 || callee_fn->is_variadic)) {
                ZeroCCASTArena *callee_arena = (ZeroCCASTArena *)callee_fn->ast_arena;
                if (callee_arena) {
                    /* 1. Evaluate arguments */
                    int inlined_param_temps[8];
                    int p_idx = 0;
                    ASTRef a_ref = node->right;
                    while (a_ref != AST_NULL && p_idx < (int)callee_fn->param_count && p_idx < 8) {
                        int a_temp = lower_expr(c, arena, a_ref);
                        /* Sprint 110: normalize 128-bit argument width. The
                         * inlined body substitutes the param temp directly,
                         * so the temp must carry the callee parameter's value
                         * representation: a narrow argument to a 128-bit param
                         * must be zext/sext to GPR128 (an in-place narrow
                         * extend would drop nothing here but the inlined body
                         * would read a garbage high half), and a 128-bit
                         * argument to a 64-bit param must truncate to the low
                         * half. `zero_cc_emit_int_extend` below only handles
                         * 1/2/4-byte params. */
                        bool a_is128 = c->last_expr_is_128;
                        c->last_expr_is_128 = 0;
                        const ZeroCCType *ipt = &callee_fn->param_types[p_idx];
                        bool p_is128 = (ipt->tag == TYPE_INT128 || ipt->tag == TYPE_UINT128);
                        if (!c->last_expr_is_float) {
                            if (p_is128 && !a_is128) {
                                int t128 = zc_alloc_temp(c);
                                bool src_signed = false;
                                ZeroCCASTNode *an0 = zero_cc_ast_get(arena, a_ref);
                                TypeInfo *ati = an0 ? ast_node_get_typeinfo(c, an0) : NULL;
                                if (ati) {
                                    int k = ati->kind;
                                    src_signed = (k == TYPE_KIND_CHAR || k == TYPE_KIND_SHORT ||
                                                  k == TYPE_KIND_INT || k == TYPE_KIND_LONG) &&
                                                 !(ati->modifiers & TYPE_MOD_UNSIGNED);
                                }
                                if (src_signed) ir_emit_sext64_128(c, t128, a_temp);
                                else ir_emit_zext64_128(c, t128, a_temp);
                                a_temp = t128;
                                a_is128 = true;
                            } else if (!p_is128 && a_is128) {
                                int t64 = zc_alloc_temp(c);
                                ir_emit_trunc128_64(c, t64, a_temp);
                                a_temp = t64;
                                a_is128 = false;
                            }
                            /* Extend integer argument if narrow */
                            if (!a_is128) {
                                zero_cc_emit_int_extend(c, a_temp, ipt);
                            }
                        }
                        inlined_param_temps[p_idx++] = a_temp;
                        ZeroCCASTNode *an = zero_cc_ast_get(arena, a_ref);
                        a_ref = an ? an->next : AST_NULL;
                    }
                    int va_pack_len = 0;
                    if (callee_fn->is_variadic) {
                        ASTRef vp = a_ref;
                        while (vp != AST_NULL) {
                            va_pack_len++;
                            ZeroCCASTNode *vpn = zero_cc_ast_get(arena, vp);
                            vp = vpn ? vpn->next : AST_NULL;
                        }
                    }

                    /* 2. Clone callee body AST into caller's current arena */
                    ASTRef cloned_body = zero_cc_ast_clone(callee_arena, (ASTRef)callee_fn->inline_body, arena);
                    if (cloned_body != AST_NULL && callee_fn->is_variadic) {
                        zc_ast_splice_va_arg_pack(arena, cloned_body, a_ref, va_pack_len, 0);
                    }
                    if (cloned_body != AST_NULL) {
                        int inline_res_temp = zc_alloc_temp(c);
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
                        bool prev_param_is_128[8];
                        for (int p = 0; p < 8; p++) {
                            prev_param_names[p] = c->lowering.inlining.param_names[p];
                            prev_param_temps[p] = c->lowering.inlining.param_temps[p];
                            prev_param_is_func_ptr[p] = c->lowering.inlining.param_is_func_ptr[p];
                            prev_param_is_128[p] = c->lowering.inlining.param_is_128[p];
                        }

                        /* Setup new inlining context */
                        c->local_lookup_bound = 0;
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
                            c->lowering.inlining.param_is_128[p] =
                                (callee_fn->param_types &&
                                 (callee_fn->param_types[p].tag == TYPE_INT128 ||
                                  callee_fn->param_types[p].tag == TYPE_UINT128));
                        }

                        /* Lower inlined body statements into IR */
                        lower_node(c, arena, cloned_body);

                        /* `inline_res_temp` must be DEFINED on every path
                         * reaching the exit label - the caller may read it
                         * for any call expression (a ternary arm,
                         * `x ? f() : g()`, MOVs the arm's "value" into the
                         * result temp even for void arms).  The non-inlined
                         * path always emits `MOV res = VREG_0` after the
                         * CALL, so do the same here: the fallthrough
                         * materializes VREG_0 (a body that falls off the end
                         * without a return - noreturn body or missing return,
                         * both UB but accepted - otherwise leaves the temp
                         * undefined: c-torture compile/pr37913), and an
                         * inlined `return;` in a void body is patched the same
                         * way in lower_stmt_return (c-torture
                         * compile/pr39394: `x ? f1() : f1()` with an empty
                         * inline body).  Unspecified value, matching the
                         * non-inlined case; dead code when every path
                         * returns a value. */
                        ir_emit_mov_reg(c, inline_res_temp, VREG_0);
                        ir_emit_label(c, inline_exit_label);

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
                            c->lowering.inlining.param_is_128[p] = prev_param_is_128[p];
                        }

                        c->last_expr_is_float = (callee_fn->return_type.tag == TYPE_FLOAT ||
                                                 callee_fn->return_type.tag == TYPE_DOUBLE);
                        /* Sprint 110: an inlined 128-bit return yields a
                         * GPR128 temp - tell the caller its value is 128-bit
                         * so consuming ops (binops, assigns, casts) classify
                         * it correctly. */
                        c->last_expr_is_128 = (callee_fn->return_type.tag == TYPE_INT128 ||
                                               callee_fn->return_type.tag == TYPE_UINT128) ? 1 : 0;
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
                ir_emit_store(c, callee_ptr_temp, BE(c)->fp_reg, -indirect_callee_spill_off);
            }

            /* ABI stream positions: integer arg slots and FP arg slots are
             * each consumed in argument order; a hidden sret pointer takes
             * the first integer slot on x86-64/RISC-V (dedicated x8 on
             * AArch64). Args beyond the register slots go to the call
             * stack in argument order - stack_pos records that position
             * (255 = register arg). */
            /* Effective parameter types driving the int<->FP conversions
             * below. For a direct callee that is its ZeroCCType params;
             * for an INDIRECT call it must be the pointee's declared
             * signature - the ABI follows the function pointer's
             * parameter types, not the argument expression's current value
             * class. Without this, `int (*f)(double, int); f(33, 11);`
             * passes 33 in EDI instead of converting it to a double in
             * XMM0 (c-torture execute/930702-1). */
            ZeroCCType eff_params[IR_CALL_MAX_ARGS];
            /* Single source of truth for the call's argument layout (Sprint 121,
             * prototype unification): the function table when the resolved
             * callee declares a prototype, else the callee expression's pointee
             * TypeInfo. */
            int eff_param_count = zero_cc_effective_call_params(c, callee_fn,
                                    zero_cc_ast_get(arena, node->left),
                                    eff_params, IR_CALL_MAX_ARGS);
            int gp_regs_avail = BE(c)->conv->int_arg_reg_count;
            int fp_regs_avail = BE(c)->conv->fp_arg_reg_count;
            int gp_stream = 0, fp_stream = 0, stack_arg_count = 0;
            bool callee_uses_sret = ((call_ret_type.tag == TYPE_STRUCT || call_ret_type.tag == TYPE_UNION) ||
                (call_ret_type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&call_ret_type))) &&
                (call_ret_type.size > 16);
            if (callee_uses_sret && c->isa != ISA_AARCH64) gp_stream = 1;  /* hidden sret ptr takes GP slot 0 */

            while (arg != AST_NULL) {
                ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                if (int_arg_idx + fp_arg_idx >= IR_CALL_MAX_ARGS &&
                    arg_node &&
                    (arg_node->tag == NODE_INT || arg_node->tag == NODE_FLOAT ||
                     arg_node->tag == NODE_IDENT || arg_node->tag == NODE_STRING)) {
                    param_idx++;
                    arg = arg_node->next;
                    continue;
                }
                int temp = lower_expr(c, arena, arg);
                bool abi_float_arg = false; /* Sprint 115m: set below */
                if (eff_param_count > 0 && param_idx < eff_param_count) {
                    const ZeroCCType *pt = &eff_params[param_idx];
                    bool param_is_fp = (pt->tag == TYPE_FLOAT || pt->tag == TYPE_DOUBLE);
                    /* Sprint 115m: a NON-variadic `float` parameter must be
                     * marshalled as a real 32-bit float in the low half of
                     * the FP register (the SysV float ABI). ZeroCC keeps
                     * scalar floats as doubles in registers, so the spill
                     * below must NARROW (vstoress) instead of storing the
                     * model double (fstore) - otherwise glibc sinf(1.0F)
                     * reads a double bit pattern as a float. Variadic floats
                     * are default-promoted to double and stay fstore. */
                    abi_float_arg = (pt->tag == TYPE_FLOAT);
                    if (pt->is_complex) {
                        /* complex argument: temp is the object's address;
                             * marshaling happens below (two FP slots). */
                    } else if (param_is_fp && !c->last_expr_is_float) {
                        int conv_temp = zc_alloc_temp(c);
                        ir_emit_cvtsi2sd(c, (uint32_t)conv_temp, (uint32_t)temp);
                        temp = conv_temp;
                        c->last_expr_is_float = 1;
                    } else if (!param_is_fp && c->last_expr_is_float) {
                        int conv_temp = zc_alloc_temp(c);
                        ir_emit_cvttsd2si(c, (uint32_t)conv_temp, (uint32_t)temp);
                        zero_cc_emit_int_extend(c, conv_temp, pt);
                        temp = conv_temp;
                        c->last_expr_is_float = 0;
                    } else if (!param_is_fp && !c->last_expr_is_float) {
                        zero_cc_emit_int_extend(c, temp, pt);
                    }
                }
                param_idx++;
                /* C99 complex argument: passed in TWO consecutive FP
                 * registers (real, then imaginary) for floating, or in GPR(s)
                 * for integer complex. The argument expression lowered to the
                 * ADDRESS of the complex object; load the halves and spill them. */
                {
                    bool param_is_cx = (callee_fn && (param_idx - 1) < (int)callee_fn->param_count &&
                                        zero_cc_type_is_complex(&callee_fn->param_types[param_idx - 1]));
                    ZeroCCASTNode *cx_an = arg_node;
                    TypeInfo *cx_ati = cx_an ? ast_node_get_typeinfo(c, cx_an) : NULL;
                    bool arg_is_cx = (cx_ati && cx_ati->kind == TYPE_KIND_COMPLEX);
                    if (!arg_is_cx && arg_node && arg_node->tag == NODE_IDENT) {
                        ZeroCCSym *as = zero_cc_find_local(c, arg_node->s.str);
                        if (as && zero_cc_type_is_complex(&as->type)) arg_is_cx = true;
                        else {
                            ZeroCCGlobalVar *ag = find_global_var(c, arg_node->s.str);
                            if (ag && zero_cc_type_is_complex(&ag->type)) arg_is_cx = true;
                        }
                    }
                    /* Real-part extraction (C99 6.3.1.7) applies only when
                     * the declaration actually names a non-complex parameter
                     * at this position - a no-prototype / old-style decl
                     * (`void f();`) says nothing about the parameter, and
                     * default argument promotions keep _Complex T complex,
                     * so the complex value must be marshalled as complex
                     * (c-torture execute/20010605-2). */
                    bool param_declared = callee_fn && (param_idx - 1) < (int)callee_fn->param_count;
                    if (param_declared && !param_is_cx && arg_is_cx) {
                        /* C99 6.3.1.7: complex to scalar conversion extracts the real part */
                        bool el_is_fp = cx_ati->details.array.base && type_is_floating(cx_ati->details.array.base);
                        size_t el_sz = cx_ati->details.array.base ? type_size(cx_ati->details.array.base) : 8;
                        int real_val = zc_alloc_temp(c);
                        if (el_is_fp) {
                            if (el_sz == 4) ir_emit_vloadss(c, (uint32_t)real_val, (uint32_t)temp);
                            else ir_emit_vloadsd(c, (uint32_t)real_val, (uint32_t)temp);
                            c->last_expr_is_float = 1;
                        } else {
                            if (el_sz == 1) ir_emit_load_byte(c, real_val, temp);
                            else if (el_sz == 2) ir_emit_load_16(c, real_val, temp);
                            else if (el_sz == 4) ir_emit_load_32(c, real_val, temp);
                            else ir_emit_deref(c, real_val, temp);
                            c->last_expr_is_float = 0;
                        }
                        temp = real_val;
                    } else if (param_is_cx || arg_is_cx) {
                        /* Sprint 115m: the callee's DECLARED parameter type
                         * is the ABI truth for the marshalled width - an
                         * external callee (glibc csinf) and a zerocc callee
                         * both expect the parameter's element width in the
                         * low half of each FP register. The argument's own
                         * element type is the fallback for no-prototype
                         * calls. (Before this, the argument's element won,
                         * so `float _Complex` args were always marshalled as
                         * 4-byte floats even when the callee declared
                         * `double _Complex` - and vice versa.) */
                        size_t cesz = 8;
                        if (param_is_cx && callee_fn->param_types[param_idx - 1].base &&
                            callee_fn->param_types[param_idx - 1].base->size > 0) {
                            cesz = callee_fn->param_types[param_idx - 1].base->size;
                        } else if (cx_ati && cx_ati->details.array.base) {
                            cesz = type_size(cx_ati->details.array.base);
                        }
                        /* Reading the halves out of the argument object must
                         * use the argument's own element width (a float
                         * complex object is 8 bytes even when the callee
                         * declared double complex - no OOB read). */
                        size_t read_esz = cesz;
                        if (arg_is_cx && cx_ati && cx_ati->details.array.base)
                            read_esz = type_size(cx_ati->details.array.base);
                        bool el_is_fp = (param_is_cx && callee_fn->param_types[param_idx - 1].base) ?
                                        (callee_fn->param_types[param_idx - 1].base->tag == TYPE_FLOAT ||
                                         callee_fn->param_types[param_idx - 1].base->tag == TYPE_DOUBLE) :
                                        ((cx_ati && cx_ati->details.array.base) ? type_is_floating(cx_ati->details.array.base) : true);
                        if (el_is_fp && cesz != 4 && cesz != 8) cesz = 8;
                        if (!el_is_fp && cesz != 1 && cesz != 2 && cesz != 4 && cesz != 8) cesz = 4;
                        size_t csz = cesz * 2;
                        if (el_is_fp) {
                            int r_val = TEMP_NONE, i_val = TEMP_NONE;
                            if (!arg_is_cx) {
                                r_val = temp;
                                if (!c->last_expr_is_float) {
                                    int f = zc_alloc_temp(c);
                                    ir_emit_cvtsi2sd(c, (uint32_t)f, (uint32_t)temp);
                                    r_val = f;
                                }
                                i_val = zc_alloc_temp(c);
                                ir_emit_fmov_imm(c, (uint32_t)i_val, (int64_t)zc_rodata_double_offset(c, 0.0));
                            } else {
                                /* Read both halves out of the argument object
                                 * (read_esz = the argument's own element
                                 * width; no OOB read on mixed-width calls). */
                                r_val = zc_alloc_temp(c);
                                if (read_esz == 4) ir_emit_vloadss(c, (uint32_t)r_val, (uint32_t)temp);
                                else ir_emit_vloadsd(c, (uint32_t)r_val, (uint32_t)temp);
                                int o2 = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, o2, (int)read_esz);
                                int la = zc_alloc_temp(c);
                                ir_emit_add(c, la, temp, o2);
                                i_val = zc_alloc_temp(c);
                                if (read_esz == 4) ir_emit_vloadss(c, (uint32_t)i_val, (uint32_t)la);
                                else ir_emit_vloadsd(c, (uint32_t)i_val, (uint32_t)la);
                            }
                            if (cesz == 4 && c->isa != ISA_RISCV32) {
                                /* psABI: _Complex float travels in ONE FP
                                 * slot - the register's low 32 bits hold the
                                 * real half and the HIGH 32 bits the
                                 * imaginary half (an 8-byte bit pattern that
                                 * matches the {real, imag} object layout).
                                 * The preamble's 8-byte load puts the whole
                                 * pattern in one XMM (glibc csinf reads the
                                 * imag from the high 32 of xmm0). */
                                fp_spill_stack_pos[fp_arg_idx] = (fp_stream < fp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                                fp_stream++;
                                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                c->current_stack_frame += 8;
                                fp_spill_offsets[fp_arg_idx] = c->current_stack_frame;
                                int sa = zc_alloc_temp(c);
                                ir_emit_lea(c, sa, BE(c)->fp_reg, -fp_spill_offsets[fp_arg_idx]);
                                ir_emit_vstoress(c, (uint32_t)sa, (uint32_t)r_val); /* lo32 = real */
                                int o4 = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, o4, 4);
                                int sa2 = zc_alloc_temp(c);
                                ir_emit_add(c, sa2, sa, o4);
                                ir_emit_vstoress(c, (uint32_t)sa2, (uint32_t)i_val); /* hi32 = imag */
                                fp_arg_idx++;
                            } else {
                                /* _Complex double (and RV32 _Complex float,
                                 * which the psABI passes in TWO registers):
                                 * one 8-byte slot per half, model doubles. */
                                for (int half = 0; half < 2; half++) {
                                    int hv = (half == 0) ? r_val : i_val;
                                    fp_spill_stack_pos[fp_arg_idx] = (fp_stream < fp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                                    fp_stream++;
                                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                    c->current_stack_frame += 8;
                                    fp_spill_offsets[fp_arg_idx] = c->current_stack_frame;
                                    ir_emit_fstore(c, (uint32_t)hv, TEMP_NONE, -fp_spill_offsets[fp_arg_idx]);
                                    fp_arg_idx++;
                                }
                            }
                        } else {
                            /* Integer complex argument in GPR(s) */
                            if (!arg_is_cx) {
                                int r_val = temp;
                                if (c->last_expr_is_float) {
                                    r_val = zc_alloc_temp(c);
                                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = r_val,
                                                       .temp_src1 = temp, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                                }
                                if (csz <= 8) {
                                    int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                                    gp_stream++;
                                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                    c->current_stack_frame += 8;
                                    int32_t slot = c->current_stack_frame;
                                    ir_emit_store(c, r_val, BE(c)->fp_reg, -slot);
                                    if (int_arg_idx < IR_CALL_MAX_ARGS) {
                                        int_spill_offsets[int_arg_idx] = slot;
                                        int_arg_idx++;
                                    }
                                } else {
                                    int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                                    gp_stream++;
                                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                    c->current_stack_frame += 8;
                                    int32_t slot0 = c->current_stack_frame;
                                    ir_emit_store(c, r_val, BE(c)->fp_reg, -slot0);
                                    if (int_arg_idx < IR_CALL_MAX_ARGS) {
                                        int_spill_offsets[int_arg_idx] = slot0;
                                        int_arg_idx++;
                                    }

                                    int val1 = zc_alloc_temp(c);
                                    ir_emit_mov_imm(c, val1, 0);
                                    int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                                    gp_stream++;
                                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                    c->current_stack_frame += 8;
                                    int32_t slot1 = c->current_stack_frame;
                                    ir_emit_store(c, val1, BE(c)->fp_reg, -slot1);
                                    if (int_arg_idx < IR_CALL_MAX_ARGS) {
                                        int_spill_offsets[int_arg_idx] = slot1;
                                        int_arg_idx++;
                                    }
                                }
                            } else if (csz <= 8) {
                                int val = zc_alloc_temp(c);
                                if (csz == 2) ir_emit_load_16(c, val, temp);
                                else if (csz == 4) ir_emit_load_32(c, val, temp);
                                else ir_emit_deref(c, val, temp);
                                int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                                gp_stream++;
                                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                c->current_stack_frame += 8;
                                int32_t slot = c->current_stack_frame;
                                ir_emit_store(c, val, BE(c)->fp_reg, -slot);
                                if (int_arg_idx < IR_CALL_MAX_ARGS) {
                                    int_spill_offsets[int_arg_idx] = slot;
                                    int_arg_idx++;
                                }
                            } else {
                                /* 16B integer complex: 2 GPRs (real then imag) */
                                int val0 = zc_alloc_temp(c);
                                ir_emit_deref(c, val0, temp);
                                int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                                gp_stream++;
                                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                c->current_stack_frame += 8;
                                int32_t slot0 = c->current_stack_frame;
                                ir_emit_store(c, val0, BE(c)->fp_reg, -slot0);
                                if (int_arg_idx < IR_CALL_MAX_ARGS) {
                                    int_spill_offsets[int_arg_idx] = slot0;
                                    int_arg_idx++;
                                }

                                int o2 = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, o2, (int)cesz);
                                int a2 = zc_alloc_temp(c);
                                ir_emit_add(c, a2, temp, o2);
                                int val1 = zc_alloc_temp(c);
                                ir_emit_deref(c, val1, a2);
                                int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                                gp_stream++;
                                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                c->current_stack_frame += 8;
                                int32_t slot1 = c->current_stack_frame;
                                ir_emit_store(c, val1, BE(c)->fp_reg, -slot1);
                                if (int_arg_idx < IR_CALL_MAX_ARGS) {
                                    int_spill_offsets[int_arg_idx] = slot1;
                                    int_arg_idx++;
                                }
                            }
                        }
                        arg = arg_node ? arg_node->next : AST_NULL;
                        continue;
                    }
                }
                /* GNU variably-modified struct argument: two consecutive
                 * GPR units - the object address, then the total runtime
                 * size (the callee stores the size in the parameter's VLA
                 * slot for runtime sizeof). Must run before the struct
                 * chunk / hidden-pointer branches, which key off the
                 * STATIC (placeholder) size. */
                {
                    const ZeroCCType *arg_vla = NULL;
                    if (arg_node && arg_node->tag == NODE_IDENT) {
                        ZeroCCSym *as = zero_cc_find_local(c, arg_node->s.str);
                        if (as) arg_vla = zc_find_vla_struct(&as->type);
                        else {
                            ZeroCCGlobalVar *agv = find_global_var(c, arg_node->s.str);
                            if (agv) arg_vla = zc_find_vla_struct(&agv->type);
                        }
                    } else if (arg_node) {
                        TypeInfo *ati = ast_node_get_typeinfo(c, arg_node);
                        if (ati && (ati->kind == TYPE_KIND_STRUCT || ati->kind == TYPE_KIND_UNION) &&
                            ati->details.record.is_vla_struct && ati->details.record.tag) {
                            ZeroCCType *reg = zero_cc_find_struct(c, ati->details.record.tag);
                            if (reg) arg_vla = zc_find_vla_struct(reg);
                        }
                    }
                    if (arg_vla) {
                        if (!arg_vla->members && arg_vla->struct_name[0]) {
                            ZeroCCType *reg = zero_cc_find_struct(c, arg_vla->struct_name);
                            if (reg) arg_vla = reg;
                        }
                        int obj_addr = zero_cc_lower_lvalue_addr(c, arena, arg);
                        if (obj_addr == TEMP_NONE) obj_addr = temp;
                        ASTRef sz_ast = zc_vla_struct_size_ast(arena, (ZeroCCASTArena *)c->tu_ast_arena, arg_vla, arg_vla->struct_name);
                        int size_t2 = (sz_ast != AST_NULL) ? lower_expr(c, arena, sz_ast) : TEMP_NONE;
                        if (size_t2 == TEMP_NONE) {
                            size_t2 = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, size_t2, (int32_t)arg_vla->size);
                        }
                        for (int unit = 0; unit < 2; unit++) {
                            int val = (unit == 0) ? obj_addr : size_t2;
                            int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                            gp_stream++;
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            int32_t slot = c->current_stack_frame;
                            ir_emit_store(c, val, BE(c)->fp_reg, -slot);
                            if (int_arg_idx < IR_CALL_MAX_ARGS) {
                                int_spill_offsets[int_arg_idx] = slot;
                                int_arg_idx++;
                            }
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
                    ir_emit_fstore(c, (uint32_t)temp, TEMP_NONE, -slot);
                    if (int_arg_idx < IR_CALL_MAX_ARGS) {
                        int_spill_offsets[int_arg_idx] = slot;
                        int_arg_idx++;
                    }
                } else if (c->last_expr_is_float) {
                    /* Float argument: spill it to a stack slot via IR. The first
                     * fp_regs_avail go into FP registers (stream order); the rest
                     * are stack-passed in argument order (stack_pos) - the call
                     * site pushes them from this same spill slot.
                     * Sprint 115m: a non-variadic `float` parameter is
                     * marshalled as a real 32-bit float (vstoress narrows the
                     * model double into the low 4 bytes of the slot; the
                     * preamble's 8-byte load puts the float pattern in the
                     * low half of the register, the SysV float-arg form). */
                    fp_spill_stack_pos[fp_arg_idx] = (fp_stream < fp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                    fp_stream++;
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    fp_spill_offsets[fp_arg_idx] = c->current_stack_frame;
                    if (abi_float_arg && !is_var_arg) {
                        int sa = zc_alloc_temp(c);
                        ir_emit_lea(c, sa, BE(c)->fp_reg, -fp_spill_offsets[fp_arg_idx]);
                        ir_emit_vstoress(c, (uint32_t)sa, (uint32_t)temp);
                    } else {
                        ir_emit_fstore(c, (uint32_t)temp, TEMP_NONE, -fp_spill_offsets[fp_arg_idx]);
                    }
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
                            ZeroCCGlobalVar *gv = find_global_var(c, arg_node->s.str);
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
                            big_src_addr = zc_alloc_temp(c);
                            ir_emit_lea(c, big_src_addr, BE(c)->fp_reg, -arg_struct_sym->stack_offset);
                            big_struct_ptr_conv = true;
                        } else if (arg_node && arg_node->tag == NODE_IDENT && !arg_struct_sym) {
                            ZeroCCGlobalVar *bgv = find_global_var(c, arg_node->s.str);
                            if (bgv && (bgv->type.tag == TYPE_STRUCT || bgv->type.tag == TYPE_UNION)) {
                                big_src_addr = zc_alloc_temp(c);
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
                        int buf_t = zc_alloc_temp(c);
                        ir_emit_lea(c, buf_t, BE(c)->fp_reg, -c->current_stack_frame);
                        if (emit_vl_shadow) {
                            /* The save region lives in the frame of the
                             * ORIGINAL variadic function (the va_list may
                             * be forwarded through wrapper frames, e.g.
                             * __vprintf_chk -> vprintf), so derive the
                             * slot-0 base from the va_list itself:
                             * gp_end (ap+8) = slot0 - 48 (6 GP x 8), so
                             * slot0 = gp_end + 48. */
                            int o8a = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o8a, 8);
                            int ap8a = zc_alloc_temp(c);
                            ir_emit_add(c, ap8a, big_src_addr, o8a);
                            int gpend = zc_alloc_temp(c);
                            ir_emit_deref(c, gpend, ap8a);        /* *(ap+8) */
                            int o48a = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o48a, 48);
                            int save_abs = zc_alloc_temp(c);
                            ir_emit_add(c, save_abs, gpend, o48a);
                            /* region base = buf + 32 */
                            int o32h = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o32h, 32);
                            int reg_base = zc_alloc_temp(c);
                            ir_emit_add(c, reg_base, buf_t, o32h);
                            /* GP region copy: origin slot l at
                             * save_abs - 8l -> region + 8l. */
                            for (int l = 0; l < 6; l++) {
                                int sv = zc_alloc_temp(c);
                                int nl = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, nl, -8 * l);
                                ir_emit_add(c, sv, save_abs, nl);
                                int val = zc_alloc_temp(c);
                                ir_emit_deref(c, val, sv);
                                if (l == 0) {
                                    ir_emit_store_64(c, (uint32_t)val, (uint32_t)reg_base);
                                } else {
                                    int ol = zc_alloc_temp(c);
                                    ir_emit_mov_imm(c, ol, 8 * l);
                                    int dl = zc_alloc_temp(c);
                                    ir_emit_add(c, dl, reg_base, ol);
                                    ir_emit_store_64(c, (uint32_t)val, (uint32_t)dl);
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
                                    int sv = zc_alloc_temp(c);
                                    int nl = zc_alloc_temp(c);
                                    ir_emit_mov_imm(c, nl, -8 * l);
                                    ir_emit_add(c, sv, gpend, nl);
                                    val = zc_alloc_temp(c);
                                    ir_emit_deref(c, val, sv);
                                } else {
                                    val = zc_alloc_temp(c);
                                    ir_emit_mov_imm(c, val, 0);
                                }
                                int ol = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, ol, 48 + 16 * l);
                                int dl = zc_alloc_temp(c);
                                ir_emit_add(c, dl, reg_base, ol);
                                ir_emit_store_64(c, (uint32_t)val, (uint32_t)dl);
                                int z0 = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, z0, 0);
                                int oh = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, oh, 8);
                                int dh = zc_alloc_temp(c);
                                ir_emit_add(c, dh, dl, oh);
                                ir_emit_store_64(c, (uint32_t)z0, (uint32_t)dh);
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
                            int gp0 = zc_alloc_temp(c);
                            ir_emit_deref(c, gp0, big_src_addr);       /* *(ap+0) */
                            int gpd = zc_alloc_temp(c);
                            ir_emit_sub(c, gpd, save_abs, gp0);        /* 8*idx, 0..48 */
                            ir_emit_store_32(c, (uint32_t)gpd, (uint32_t)buf_t);
                            /* fp_offset = 48 + 16*(FP consumed) = 48 +
                             * 2*(gp_end - fp_ptr), always in [48, 176];
                             * the 8-slot zero-filled region above makes
                             * the exhausted value (176) read 0.0 cleanly -
                             * no branch needed. */
                            int o16a = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o16a, 16);
                            int ap16a = zc_alloc_temp(c);
                            ir_emit_add(c, ap16a, big_src_addr, o16a);
                            int fp0 = zc_alloc_temp(c);
                            ir_emit_deref(c, fp0, ap16a);              /* *(ap+16) */
                            int fpd = zc_alloc_temp(c);
                            ir_emit_sub(c, fpd, gpend, fp0);           /* 8*idx, 0..48 */
                            int fpo = zc_alloc_temp(c);
                            ir_emit_shl(c, fpo, fpd, 1);               /* 16 * idx */
                            int o48b = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o48b, 48);
                            ir_emit_add(c, fpo, o48b, fpo);            /* 48 + 16*idx */
                            int o4 = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o4, 4);
                            int b4 = zc_alloc_temp(c);
                            ir_emit_add(c, b4, buf_t, o4);
                            ir_emit_store_32(c, (uint32_t)fpo, (uint32_t)b4);
                            /* +8: overflow_arg_area = *(ap+32) */
                            int o32 = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o32, 32);
                            int ap32a = zc_alloc_temp(c);
                            ir_emit_add(c, ap32a, big_src_addr, o32);
                            int ov0 = zc_alloc_temp(c);
                            ir_emit_deref(c, ov0, ap32a);
                            int o8 = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o8, 8);
                            int b8 = zc_alloc_temp(c);
                            ir_emit_add(c, b8, buf_t, o8);
                            ir_emit_store_64(c, (uint32_t)ov0, (uint32_t)b8);
                            /* +16: reg_save_area = region base */
                            int o16 = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, o16, 16);
                            int b16 = zc_alloc_temp(c);
                            ir_emit_add(c, b16, buf_t, o16);
                            ir_emit_store_64(c, (uint32_t)reg_base, (uint32_t)b16);
                        } else {
                            zc_emit_mem_copy(c, big_src_addr, buf_t, arg_struct_sz);
                        }
                        temp = buf_t;
                    } else if (is_mem_struct) {
                        int val0 = zc_alloc_temp(c);
                        if (arg_struct_sz < 8) {
                            if (arg_struct_sz == 1) ir_emit_load_byte(c, val0, mem_struct_addr);
                            else if (arg_struct_sz == 2) ir_emit_load_16(c, val0, mem_struct_addr);
                            else if (arg_struct_sz == 4) ir_emit_load_32(c, val0, mem_struct_addr);
                            else ir_emit_deref(c, val0, mem_struct_addr);
                        } else {
                            ir_emit_deref(c, val0, mem_struct_addr);
                        }
                        temp = val0;
                    }

                    /* GNU 128-bit (lo,hi) integer argument: the ABI unit
                     * count is TWO consecutive GP units (low half in the
                     * first register, high half in the second; on stack
                     * overflow the 16 bytes ride in argument order, low
                     * unit at the lower address). Normalize the value to
                     * the parameter's width first: a narrow int argument
                     * is converted to 128 bits (zero/sign-extended per the
                     * ARGUMENT's signedness, C 6.3.1.3); a 128-bit value
                     * passed to a 64-bit parameter truncates to the low
                     * half. `last_expr_is_128` says whether `temp` is a
                     * GPR128 pair after lower_expr + the conversions
                     * above. */
                    bool p128 = (eff_param_count > 0 && (param_idx - 1) < eff_param_count &&
                                 (eff_params[param_idx - 1].tag == TYPE_INT128 ||
                                  eff_params[param_idx - 1].tag == TYPE_UINT128));
                    bool t128 = c->last_expr_is_128;
                    c->last_expr_is_128 = 0;
                    if (p128 && !t128) {
                        int t128_new = zc_alloc_temp(c);
                        bool src_signed = false;
                        TypeInfo *ati128 = arg_node ? ast_node_get_typeinfo(c, arg_node) : NULL;
                        if (ati128) {
                            int k = ati128->kind;
                            src_signed = (k == TYPE_KIND_CHAR || k == TYPE_KIND_SHORT ||
                                          k == TYPE_KIND_INT || k == TYPE_KIND_LONG) &&
                                         !(ati128->modifiers & TYPE_MOD_UNSIGNED);
                        }
                        if (src_signed) ir_emit_sext64_128(c, t128_new, temp);
                        else ir_emit_zext64_128(c, t128_new, temp);
                        temp = t128_new;
                        t128 = true;
                    } else if (!p128 && t128) {
                        int t64 = zc_alloc_temp(c);
                        ir_emit_trunc128_64(c, t64, temp);
                        temp = t64;
                        if (eff_param_count > 0 && (param_idx - 1) < eff_param_count) {
                            zero_cc_emit_int_extend(c, temp, &eff_params[param_idx - 1]);
                        }
                        t128 = false;
                    }

                    /* Integer or struct argument: spill to stack slot(s) */
                    if (t128) {
                        int hi_temp = zc_alloc_temp(c);
                        ir_emit_hi128(c, hi_temp, temp);
                        int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                        gp_stream++;
                        if (int_arg_idx < IR_CALL_MAX_ARGS) {
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            int_spill_offsets[int_arg_idx] = c->current_stack_frame;
                            ir_emit_store(c, temp, BE(c)->fp_reg, -int_spill_offsets[int_arg_idx]);
                            int_arg_idx++;
                        }
                        int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                        gp_stream++;
                        if (int_arg_idx < IR_CALL_MAX_ARGS) {
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            int_spill_offsets[int_arg_idx] = c->current_stack_frame;
                            ir_emit_store(c, hi_temp, BE(c)->fp_reg, -int_spill_offsets[int_arg_idx]);
                            int_arg_idx++;
                        }
                    } else {
                        int_spill_stack_pos[int_arg_idx] = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                        gp_stream++;
                        if (int_arg_idx < IR_CALL_MAX_ARGS) {
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            int_spill_offsets[int_arg_idx] = c->current_stack_frame;
                            ir_emit_store(c, temp, BE(c)->fp_reg, -int_spill_offsets[int_arg_idx]);
                            int_arg_idx++;
                        }
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
                            int ch_temp = zc_alloc_temp(c);
                            if (arg_node && arg_node->tag == NODE_IDENT && arg_struct_sym) {
                                ir_emit_load(c, ch_temp, BE(c)->fp_reg, -(arg_struct_sym->stack_offset - ch * 8));
                            } else if (is_mem_struct) {
                                int off_t = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, off_t, ch * 8);
                                int ch_addr = zc_alloc_temp(c);
                                ir_emit_add(c, ch_addr, mem_struct_addr, off_t);
                                int ch_val = zc_alloc_temp(c);
                                size_t rem = arg_struct_sz - ch * 8;
                                if (rem == 1) ir_emit_load_byte(c, ch_val, ch_addr);
                                else if (rem == 2) ir_emit_load_16(c, ch_val, ch_addr);
                                else if (rem == 4) ir_emit_load_32(c, ch_val, ch_addr);
                                else ir_emit_deref(c, ch_val, ch_addr);
                                ch_temp = ch_val;
                            } else {
                                ir_emit_mov_reg(c, ch_temp, VREG_2);
                            }
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            int_spill_offsets[int_arg_idx] = c->current_stack_frame;
                            ir_emit_store(c, ch_temp, BE(c)->fp_reg, -int_spill_offsets[int_arg_idx]);
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

                sret_buf_temp = zc_alloc_temp(c);
                ir_emit_lea(c, sret_buf_temp, BE(c)->fp_reg, -buf_stack_off);

                if (all_arg_count < IR_CALL_MAX_ARGS) {
                    all_arg_temps[all_arg_count] = sret_buf_temp;
                    all_arg_is_fp[all_arg_count] = (c->isa == ISA_AARCH64) ? 3 : 0;
                    all_arg_count++;
                }
            }

            /* Then reload user integer arguments */
            uint8_t all_arg_stack_pos[IR_CALL_MAX_ARGS];

            /* Phase 2/3a: When calling a nested function, pass captured
             * variables as leading arguments. The nested function's
             * signature expects captured values first, then user args.
             * Phase 3a: scalar captures (size <= 8) are BY REFERENCE -
             * pass the ADDRESS of the enclosing storage (the nested
             * function mirrors the pointee and flushes writes back);
             * aggregate captures stay by-value. */
            if (!is_indirect_call && callee_fn && callee_fn->is_nested &&
                callee_fn->nested_captured_count > 0 &&
                callee_fn->enclosing_local_snapshot) {
                for (size_t ci = 0; ci < callee_fn->nested_captured_count &&
                     all_arg_count < IR_CALL_MAX_ARGS; ci++) {
                    /* Prefer the LIVE enclosing local (it carries
                     * is_capture_mirror, set when the enclosing function
                     * is compiled - after this nested function was
                     * registered, so the registration-time snapshot copy
                     * never has the flag; a stale flag would make a
                     * multi-level call pass &slot instead of the pointer
                     * value - c-torture execute/20061220-1).
                     * Fallback to the snapshot if the lookup fails. */
                    ZeroCCSym *cap_sym = zero_cc_find_local(c,
                                                            callee_fn->enclosing_local_snapshot[ci].name);
                    if (!cap_sym)
                        cap_sym = &callee_fn->enclosing_local_snapshot[ci];
                    bool byref = (callee_fn->nested_capture_byref &&
                                  callee_fn->nested_capture_byref[ci]);
                    int val_temp;
                    if (byref) {
                        /* zc_sym_addr returns, for an is_capture_mirror
                         * symbol (multi-level nesting), the POINTER
                         * VALUE - the enclosing storage address - and
                         * for a plain local the address of its storage.
                         * That is exactly what the nested function's
                         * pointer-routed body expects either way. */
                        val_temp = zc_sym_addr(c, cap_sym, 0);
                    } else {
                        /* Load the current value of the captured variable
                         * from the enclosing function's frame. */
                        val_temp = zc_alloc_temp(c);
                        if (cap_sym->type.tag == TYPE_FLOAT) {
                            int a = zc_sym_addr(c, cap_sym, 0);
                            ir_emit_vloadss(c, (uint32_t)val_temp, (uint32_t)a);
                        } else if (cap_sym->type.tag == TYPE_DOUBLE) {
                            int a = zc_sym_addr(c, cap_sym, 0);
                            ir_emit_vloadsd(c, (uint32_t)val_temp, (uint32_t)a);
                        } else {
                            zc_sym_load(c, cap_sym, 0, val_temp);
                        }
                    }
                    all_arg_temps[all_arg_count] = val_temp;
                    all_arg_is_fp[all_arg_count] = byref ? 0 :
                        ((cap_sym->type.tag == TYPE_FLOAT ||
                          cap_sym->type.tag == TYPE_DOUBLE) ? 1 : 0);
                    all_arg_stack_pos[all_arg_count] = 255;
                    all_arg_count++;
                }
            }

            /* Sprint 99: nonlocal goto pairs, after the captures and
             * before the user args (the callee's hidden (rbp, rsp) params
             * per ancestor scope, outermost first). For pair i: if i is
             * below this function's own depth, forward this function's
             * own pair slot i; the last pair (i == this depth) is THIS
             * function's frame: (LEA 0, SP snapshot slot). */
            if (!is_indirect_call && callee_fn && callee_fn->is_nested &&
                callee_fn->goto_pair_count > 0) {
                ZeroCCFunction *cur_fn2 = &c->functions[c->lowering.current_func_idx];
                int my_depth = 0;
                for (size_t ai = cur_fn2->nested_enclosing_fn_idx;
                     ai != (size_t)-1; ) {
                    my_depth++;
                    if (!c->functions[ai].is_nested) break;
                    ai = c->functions[ai].nested_enclosing_fn_idx;
                }
                for (int pi = 0; pi < callee_fn->goto_pair_count &&
                                 all_arg_count < IR_CALL_MAX_ARGS; pi++) {
                    int rbp_t = zc_alloc_temp(c);
                    int rsp_t = zc_alloc_temp(c);
                    if (pi < my_depth) {
                        char nm[48];
                        snprintf(nm, sizeof(nm), "__nlgbp_%d", pi);
                        ZeroCCSym *s1 = zero_cc_find_local(c, nm);
                        snprintf(nm, sizeof(nm), "__nlbsp_%d", pi);
                        ZeroCCSym *s2 = zero_cc_find_local(c, nm);
                        if (!s1 || !s2) break;
                        zc_sym_load(c, s1, 0, rbp_t);
                        zc_sym_load(c, s2, 0, rsp_t);
                    } else {
                        ir_emit_lea(c, rbp_t, BE(c)->fp_reg, 0);
                        if (c->lowering.sp_snap_off > 0) {
                            /* ir_emit_load takes an ADDITIVE displacement; the
                             * slot sits at frame offset -sp_snap_off. */
                            ir_emit_load(c, rsp_t, BE(c)->fp_reg,
                                         -(int)c->lowering.sp_snap_off);
                        } else {
                            /* no snapshot reserved (shouldn't happen): the
                             * prologue sub amount is 16-rounded frame +
                             * the ISA push distance - approximate with
                             * LEA -frame (wrong under OFP, but this path
                             * is unreachable when the reservation ran). */
                            ir_emit_lea(c, rsp_t, BE(c)->fp_reg,
                                        -(int)c->current_stack_frame);
                        }
                    }
                    all_arg_temps[all_arg_count] = rbp_t;
                    all_arg_is_fp[all_arg_count] = 0;
                    all_arg_stack_pos[all_arg_count] = 255;
                    all_arg_count++;
                    all_arg_temps[all_arg_count] = rsp_t;
                    all_arg_is_fp[all_arg_count] = 0;
                    all_arg_stack_pos[all_arg_count] = 255;
                    all_arg_count++;
                }
            }

            for (int a = 0; a < int_arg_idx && all_arg_count < IR_CALL_MAX_ARGS; a++) {
                int reload_temp = zc_alloc_temp(c);
                ir_emit_load(c, reload_temp, BE(c)->fp_reg, -int_spill_offsets[a]);
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
                int reload_temp = zc_alloc_temp(c);
                ir_emit_fload(c, reload_temp, TEMP_NONE, -fp_spill_offsets[f]);
                all_arg_temps[all_arg_count] = reload_temp;
                all_arg_is_fp[all_arg_count] = 1;
                all_arg_stack_pos[all_arg_count] = fp_spill_stack_pos[f];
                all_arg_count++;
                fp_reload_idx++;
            }

            /* Reload indirect callee pointer if needed */
            if (is_indirect_call && all_arg_count < IR_CALL_MAX_ARGS) {
                int callee_reload_temp = zc_alloc_temp(c);
                ir_emit_load(c, callee_reload_temp, BE(c)->fp_reg, -indirect_callee_spill_off);
                all_arg_temps[all_arg_count] = callee_reload_temp;
                all_arg_is_fp[all_arg_count] = 2; /* 2 marks indirect callee register */
                all_arg_stack_pos[all_arg_count] = 255;
                all_arg_count++;
            }
            if (callee_uses_sret) all_arg_stack_pos[0] = 255;  /* sret entry is always a register arg */

            /* Emit IR call (NULL name for indirect calls) */
            /* The emitted symbol is the function's actual emission name:
             * nested callees are mangled (zero_cc_resolve_callee resolved
             * them), and `__asm`-labeled callees are renamed (c-torture
             * execute/builtins/{strstr,memops}-asm.c). Unknown callees
             * (function pointers by other names) keep the raw name. */
            const char *emit_name = callee_fn ? zero_cc_fn_emit_symbol(callee_fn) : name;
            ir_emit_call(c, is_indirect_call ? NULL : emit_name);
            size_t call_ir_idx = ir_get_count(c) > 0 ? ir_get_count(c) - 1 : 0;

            /* Pack argument info into the instruction.
             * Sprint 151c: args 0/1 go in temp_src1/temp_src2; args 2+ are
             * emitted as OP_CALL_ARG pseudo-instructions that follow in the
             * stream (no instr_index-keyed side table). temp_dest = total
             * argument count so consumers know how many OP_CALL_ARG entries
             * to read. */
            if (ir_get_count(c) > 0) {
                Instr *last_instr = (Instr*)&ir_get_buffer(c)[call_ir_idx];
                last_instr->temp_dest = (uint32_t)all_arg_count;
                if (all_arg_count > 0) last_instr->temp_src1 = (uint32_t)all_arg_temps[0];
                if (all_arg_count > 1) last_instr->temp_src2 = (uint32_t)all_arg_temps[1];
                last_instr->imm = 0;  /* unused for OP_CALL since C2b */
                /* Sprint 151c: is_fp class of args 0/1 carried in cond/width
                 * (OP_CALL's cond/width are otherwise unused). */
                last_instr->cond = (uint8_t)(all_arg_count > 0 ? all_arg_is_fp[0] : 0);
                last_instr->width = (uint8_t)(all_arg_count > 1 ? all_arg_is_fp[1] : 0);
                /* Store the number of FP REGISTER args (clamped: %al / the
                 * callee spill only covers the register slots) in label_idx
                 * before emitting OP_CALL_ARG (which may reallocate the arena
                 * and shift ir_get_count(c) - 1). */
                last_instr->label_idx = (uint16_t)(fp_arg_idx < fp_regs_avail ? fp_arg_idx : fp_regs_avail);
                /* Emit OP_CALL_ARG for each argument beyond the first two. */
                for (int a = 2; a < all_arg_count; a++) {
                    ir_emit_call_arg(c, (uint32_t)all_arg_temps[a], all_arg_is_fp[a], all_arg_stack_pos[a]);
                }
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
             * xmm1 (VREG_F1) for floating, or VREG_0 / (VREG_0, VREG_2) for integer.
             * Materialize into an aligned stack object
             * and hand back its ADDRESS (like vector returns). */
            if (call_ret_type.is_complex) {
                bool is_fp = (call_ret_type.base && (call_ret_type.base->tag == TYPE_FLOAT || call_ret_type.base->tag == TYPE_DOUBLE));
                size_t cesz = (call_ret_type.base && call_ret_type.base->size > 0) ? call_ret_type.base->size : (is_fp ? 8 : 4);
                size_t csz = call_ret_type.size ? call_ret_type.size : cesz * 2;
                c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + (int32_t)csz + 7) & ~7;
                int32_t cres_off = c->current_stack_frame;
                int caddr = zc_alloc_temp(c);
                ir_emit_lea(c, caddr, BE(c)->fp_reg, -cres_off);

                if (is_fp) {
                    if (call_ir_idx < ir_get_count(c)) {
                        /* Keep VREG_F0/VREG_F1 (xmm0/xmm1) live across the call */
                        Instr *li = (Instr*)&ir_get_buffer(c)[call_ir_idx];
                        li->label_idx = 0x0100 | (csz & 0xFF);
                    }
                    if (cesz == 4 && c->isa != ISA_RISCV32) {
                        /* psABI: a _Complex float return arrives in ONE
                         * register: real in the low 32 bits, imaginary in
                         * the HIGH 32 bits. The 8-byte bit pattern matches
                         * the {real, imag} object layout, so one 8-byte
                         * store captures both halves. */
                        ir_emit_vstoresd(c, (uint32_t)caddr, (uint32_t)VREG_F0);
                    } else if (cesz == 4) {
                        /* RV32: two registers, float in each low 32. */
                        ir_emit_vstoress_raw(c, (uint32_t)caddr, (uint32_t)VREG_F0);
                        int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, 4);
                        int a2 = zc_alloc_temp(c); ir_emit_add(c, a2, caddr, o2);
                        ir_emit_vstoress_raw(c, (uint32_t)a2, (uint32_t)VREG_F1);
                    } else {
                        ir_emit_vstoresd(c, (uint32_t)caddr, (uint32_t)VREG_F0);
                        int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, 8);
                        int a2 = zc_alloc_temp(c); ir_emit_add(c, a2, caddr, o2);
                        ir_emit_vstoresd(c, (uint32_t)a2, (uint32_t)VREG_F1);
                    }
                } else {
                    /* Integer complex return */
                    if (csz > 8) {
                        if (call_ir_idx < ir_get_count(c)) {
                            Instr *li = (Instr*)&ir_get_buffer(c)[call_ir_idx];
                            li->label_idx = 0x0100 | (csz & 0xFF);
                        }
                        int t0 = zc_alloc_temp(c);
                        ir_emit_mov_reg(c, t0, VREG_0);
                        ir_emit_store(c, t0, BE(c)->fp_reg, -cres_off);
                        int t1 = zc_alloc_temp(c);
                        ir_emit_mov_reg(c, t1, VREG_2);
                        ir_emit_store(c, t1, BE(c)->fp_reg, -cres_off + 8);
                    } else {
                        int t0 = zc_alloc_temp(c);
                        ir_emit_mov_reg(c, t0, VREG_0);
                        int a0 = zc_alloc_temp(c);
                        ir_emit_lea(c, a0, BE(c)->fp_reg, -cres_off);
                        if (csz == 1) ir_emit_store_byte(c, t0, a0);
                        else if (csz == 2) ir_emit_store_16(c, t0, a0);
                        else if (csz == 4) ir_emit_store_32(c, t0, a0);
                        else ir_emit_store_64(c, t0, a0);
                    }
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
                int fp_res = zc_alloc_temp(c);
                if (call_ret_type.tag == TYPE_FLOAT) {
                    /* Sprint 115m: a `float` return arrives with the float
                     * pattern in the LOW 32 bits of xmm0 (upper bits
                     * undefined); cvtss2sd reinterprets that low half as a
                     * single and widens it to the model double. A plain
                     * fmov (movsd) would keep the stale upper half. */
                    ir_emit_cvtss2sd(c, (uint32_t)fp_res, (uint32_t)VREG_F0);
                } else {
                    ir_emit_fmov(c, (uint32_t)fp_res, (uint32_t)VREG_F0);
                }
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
                    if (call_ir_idx < ir_get_count(c)) {
                        Instr *li = (Instr*)&ir_get_buffer(c)[call_ir_idx];
                        li->label_idx = 0x0100 | (call_ret_type.size & 0xFF);
                    }
                    int t0 = zc_alloc_temp(c);
                    ir_emit_mov_reg(c, t0, VREG_0);
                    ir_emit_store(c, t0, BE(c)->fp_reg, -res_off);
                    int t1 = zc_alloc_temp(c);
                    ir_emit_mov_reg(c, t1, VREG_2);
                    ir_emit_store(c, t1, BE(c)->fp_reg, -res_off + 8);
                } else {
                    int t0 = zc_alloc_temp(c);
                    ir_emit_mov_reg(c, t0, VREG_0);
                    int a0 = zc_alloc_temp(c);
                    ir_emit_lea(c, a0, BE(c)->fp_reg, -res_off);
                    if (vsz == 1) ir_emit_store_byte(c, t0, a0);
                    else if (vsz == 2) ir_emit_store_16(c, t0, a0);
                    else if (vsz == 4) ir_emit_store_32(c, t0, a0);
                    else ir_emit_store_64(c, t0, a0);
                }
                int addr = zc_alloc_temp(c);
                ir_emit_lea(c, addr, BE(c)->fp_reg, -res_off);
                return addr;
            }

            /* Check if function returns a 9-16 byte struct/union */
            if ((call_ret_type.tag == TYPE_STRUCT || call_ret_type.tag == TYPE_UNION) &&
                call_ret_type.size > 8 && call_ret_type.size <= 16) {
                /* Store struct return info in label_idx: high byte = 1, low byte = size */
                if (call_ir_idx < ir_get_count(c)) {
                    Instr *last_instr = (Instr*)&ir_get_buffer(c)[call_ir_idx];
                    last_instr->label_idx = 0x0100 | (call_ret_type.size & 0xFF);
                }
                /* 9-16 byte struct return uses integer register pair */
                return VREG_0;
            }

            /* If function returns >16 byte struct, return pointer to allocated buffer */
            if (callee_uses_sret) {
                int call_res_temp = zc_alloc_temp(c);
                if (c->isa == ISA_AARCH64) {
                    /* AAPCS64 does not require callee to preserve x8 or return the address in x0;
                     * recompute the caller's sret buffer address directly from -buf_stack_off */
                    ir_emit_lea(c, call_res_temp, BE(c)->fp_reg, -buf_stack_off);
                } else {
                    ir_emit_mov_reg(c, call_res_temp, VREG_0);
                }
                return call_res_temp;
            }

            /* GNU 128-bit return: the (rax, rdx) = (VREG_0, VREG_2) pair holds
             * the (lo, hi) result; form a GPR128 temp from it. */
            if (call_ret_type.tag == TYPE_INT128 || call_ret_type.tag == TYPE_UINT128) {
                int lo = zc_alloc_temp(c);
                ir_emit_mov_reg(c, lo, VREG_0);
                int hi = zc_alloc_temp(c);
                ir_emit_mov_reg(c, hi, VREG_2);
                int res = zc_alloc_temp(c);
                ir_emit_mk128(c, res, lo, hi);
                c->last_expr_is_float = 0;
                c->last_expr_is_128 = 1;
                return res;
            }

            int call_res_temp = zc_alloc_temp(c);
            ir_emit_mov_reg(c, call_res_temp, VREG_0);
            zero_cc_emit_int_extend(c, call_res_temp, &call_ret_type);
            return call_res_temp;
}
