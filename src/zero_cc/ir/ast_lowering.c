/* AST Lowering - AST to Machine Code Pipeline (Coordinator)
 *
 * This file coordinates the lowering of AST to IR, live range analysis,
 * linear-scan register allocation, callee-save preservation, stack spill
 * slot lowering, and machine code generation.
 *
 * The implementation is organized into modular source files:
 *   - ast_lowering_stmt.c: AST statement lowering to IR (lower_stmt_*, lower_node)
 *   - ast_lowering_expr.c: AST expression lowering to IR (lower_expr)
 *   - asm_emitter.c:      IR to assembly emission (emit_ir_to_asm)
 *   - ast_lowering_mc.c:   IR to machine code emission (emit_ir_to_machine_code)
 */

#include <stdio.h>
#include <string.h>
#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/parser/vla_struct.h"
#include "zero_cc/ir/asm_emitter.h"
#include "zero_cc/opt/opt_pass.h"
#include "zero_cc/opt/opt_temp_renumber.h"
#include "zero_cc/dwarf/dwarf.h"

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

/* =========================================================================
 * Sprint 37: static-local mirror
 *
 * A `static` block-scope local is a data-section object (registered in
 * the global table under sym->static_global_name) whose stack slot is a
 * per-call mirror. The prologue (before the body) copies
 * data -> slot; the single shared epilogue (every return jumps to the
 * epilogue label, and natural fallthrough reaches it) copies
 * slot -> data. All access sites remain frame-relative, and the value
 * persists across calls. Frame slots are aligned to >=8 bytes, and the
 * data allocation is rounded up to 8, so 8-byte chunks (with a possibly
 * smaller final chunk) never spill into a neighbouring object.
 *
 * The global's base address is loaded ONCE per direction and each chunk
 * adds its own offset: a K-byte static costs 1 relocation instead of
 * K/8. c-torture execute/memclr.c (2048 statics x 88 B) hit the 32k text
 * reloc cap with the per-chunk version - the overflow was a SILENT drop
 * (unpatched imm64 = 0 -> load from address 64 -> segfault).
 * ========================================================================= */

/* =========================================================================
 * Sprint 72: pre-scan for address-taken static locals
 *
 * A `static` block-scope local is normally a data-section object mirrored
 * into a per-call stack slot (Sprint 37). That mirror is UNSOUND when the
 * object's ADDRESS is materialized in the function (address-of, array
 * decay, or a member/pointer escape): the escaped pointer must be the
 * stable data-section object, and every access (value AND address) must
 * target it - a frame-relative slot is invalid once the function returns
 * (c-torture execute/20041218-1: `static struct V v; return &v;` handed
 * out a dead stack address and the epilogue mirror-out clobbered the
 * object a callee had written).
 *
 * We therefore run a pre-scan over the body BEFORE lowering and mark every
 * static local whose address can be materialized (sym->static_addr_taken).
 * For marked statics the mirror is skipped and all accesses go to the data
 * section directly. The scan is conservative (marks whole arrays, marks on
 * any `&`), which is safe: over-marking only loses the mirror fast path.
 * ========================================================================= */
/* If `operand` is the operand of an address-of, mark the static local it
 * names (walking through member chains to the base identifier).  Marks ALL
 * static locals with the name (shadowing is rare; over-marking is safe). */
static void zc_prescan_mark_addr_of(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef operand) {
    if (operand == AST_NULL) return;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, operand);
    if (!n) return;
    if (n->tag == NODE_MEMBER || n->tag == NODE_INDEX) {
        zc_prescan_mark_addr_of(c, arena, n->left);
        return;
    }
    if (n->tag == NODE_IDENT && n->s.str) {
        for (size_t i = 0; i < c->local_count; i++) {
            ZeroCCSym *s = &c->locals[i];
            if (strcmp(s->name, n->s.str) == 0) {
                if (s->is_static_local) {
                    s->static_addr_taken = true;
                } else {
                    s->addr_taken = true;
                }
            }
        }
    }
}

static bool zc_type_contains_array(const ZeroCCType *t) {
    if (!t) return false;
    if (t->tag == TYPE_ARRAY) return true;
    if (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) {
        if (!t->members) return false;
        for (size_t i = 0; i < t->member_count; i++) {
            const ZeroCCStructMember *m = &t->members[i];
            if (m->tag == TYPE_ARRAY) return true;
            if ((m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) && m->base) {
                if (zc_type_contains_array(m->base)) return true;
            }
        }
    }
    return false;
}

/* Recursively visit every expression/statement sub-node of `ref`, marking
 * statics whose address is taken via `&`. */
static void zc_prescan_visit(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return;
    switch (n->tag) {
        case NODE_UNOP:
            if (n->aux.op == '&' || n->aux.op == TK_AMP) zc_prescan_mark_addr_of(c, arena, n->left);
            zc_prescan_visit(c, arena, n->left);
            break;
        case NODE_BINOP:
        case NODE_COMMA:
            zc_prescan_visit(c, arena, n->left);
            zc_prescan_visit(c, arena, n->right);
            break;
        case NODE_ASSIGN:
            zc_prescan_visit(c, arena, n->left);
            zc_prescan_visit(c, arena, n->right);
            break;
        case NODE_TERNARY:
            zc_prescan_visit(c, arena, n->left);
            zc_prescan_visit(c, arena, n->right);
            zc_prescan_visit(c, arena, n->aux.alt);
            break;
        case NODE_CAST:
        case NODE_MEMBER:
            zc_prescan_visit(c, arena, n->left);
            break;
        case NODE_INDEX:
            zc_prescan_visit(c, arena, n->left);
            zc_prescan_visit(c, arena, n->right);
            break;
        case NODE_CALL: {
            zc_prescan_visit(c, arena, n->left);
            ASTRef a = n->right;
            while (a != AST_NULL) {
                ZeroCCASTNode *an = zero_cc_ast_get(arena, a);
                zc_prescan_visit(c, arena, a);
                a = an ? an->next : AST_NULL;
            }
            break;
        }
        case NODE_COMPOUND_LIT:
        case NODE_VA_ARG:
        case NODE_VA_START:
            zc_prescan_visit(c, arena, n->left);
            zc_prescan_visit(c, arena, n->right);
            break;
        case NODE_STMT_EXPR:
            zc_prescan_visit(c, arena, n->left);
            break;
        case NODE_BLOCK: {
            ASTRef s = n->left;
            while (s != AST_NULL) {
                ZeroCCASTNode *sn = zero_cc_ast_get(arena, s);
                zc_prescan_visit(c, arena, s);
                s = sn ? sn->next : AST_NULL;
            }
            break;
        }
        case NODE_EXPR_STMT:
        case NODE_RETURN:
        case NODE_LABEL:
            zc_prescan_visit(c, arena, n->left);
            break;
        case NODE_ASM: {
            /* Sprint 135: operand expressions live in the asm payload */
            if (n->int_val >= 0 && (size_t)n->int_val < c->asm_stmt_count) {
                ZeroCCAsmStmt *as = &c->asm_stmts[n->int_val];
                for (int i = 0; i < as->out_count; i++) {
                    zc_prescan_visit(c, arena, as->out_exprs[i]);
                }
                for (int i = 0; i < as->in_count; i++) {
                    zc_prescan_visit(c, arena, as->in_exprs[i]);
                }
            }
            break;
        }
        case NODE_IF:
            zc_prescan_visit(c, arena, n->left);
            zc_prescan_visit(c, arena, n->right);
            zc_prescan_visit(c, arena, n->aux.alt);
            break;
        case NODE_WHILE:
        case NODE_DO_WHILE:
            zc_prescan_visit(c, arena, n->left);
            zc_prescan_visit(c, arena, n->right);
            break;
        case NODE_FOR:
            zc_prescan_visit(c, arena, n->left);
            zc_prescan_visit(c, arena, n->right);
            zc_prescan_visit(c, arena, n->aux.alt);   /* body */
            zc_prescan_visit(c, arena, n->aux.step);  /* step */
            break;
        case NODE_SWITCH:
            zc_prescan_visit(c, arena, n->left);
            zc_prescan_visit(c, arena, n->right);
            break;
        case NODE_CASE:
            zc_prescan_visit(c, arena, n->left);
            zc_prescan_visit(c, arena, n->right);
            break;
        case NODE_DEFAULT:
        case NODE_DECL:
        case NODE_STATIC_ASSERT:
            zc_prescan_visit(c, arena, n->left);
            break;
        default:
            break;
    }
}

/* Mark every static local in the current function whose address can be
 * materialized: any static array (decays to a pointer) and any static that
 * appears as the operand of `&` (directly or as the base of a member chain).
 * Must run before lowering so the flag is visible to every access site. */
static void zero_cc_prescan_static_addr_taken(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef body) {
    for (size_t i = 0; i < c->local_count; i++) {
        ZeroCCSym *s = &c->locals[i];
        if (s->is_static_local && s->type.tag == TYPE_ARRAY) s->static_addr_taken = true;
    }
    zc_prescan_visit(c, arena, body);
}

/* data -> stack slot (prologue) */
static void static_local_mirror_in(ZeroCCCompiler *c, ZeroCCSym *ss) {
    size_t sz = ss->type.size ? (size_t)ss->type.size : 8;
    size_t alloc_sz = (sz + 7) & ~(size_t)7;  /* mirror copies in 8-byte chunks */
    int base = zc_alloc_temp(c);
    ir_emit_mov_global(c, (uint32_t)base, ss->static_global_name);
    for (size_t off = 0; off < alloc_sz; off += 8) {
        int addr = base;
        if (off != 0) {
            int o = zc_alloc_temp(c);
            ir_emit_mov_imm(c, (uint32_t)o, (int64_t)off);
            addr = zc_alloc_temp(c);
            ir_emit_add(c, (uint32_t)addr, (uint32_t)base, (uint32_t)o);
        }
        int val = zc_alloc_temp(c);
        ir_emit_deref(c, val, addr);
        int sa = zc_alloc_temp(c);
        ir_emit_lea(c, sa, BE(c)->fp_reg, (int32_t)off - ss->stack_offset);
        ir_emit_store_64(c, (uint32_t)val, (uint32_t)sa);
    }
}

/* stack slot -> data (epilogue). Plain allocator temps are fine here
 * because the caller wraps the whole block with a VREG_0 (return value)
 * save/restore through the frame - see zero_cc_lower_ast(). */
static void static_local_mirror_out(ZeroCCCompiler *c, ZeroCCSym *ss) {
    size_t sz = ss->type.size ? (size_t)ss->type.size : 8;
    size_t alloc_sz = (sz + 7) & ~(size_t)7;  /* mirror copies in 8-byte chunks */
    int base = zc_alloc_temp(c);
    ir_emit_mov_global(c, (uint32_t)base, ss->static_global_name);
    for (size_t off = 0; off < alloc_sz; off += 8) {
        int sa = zc_alloc_temp(c);
        ir_emit_lea(c, sa, BE(c)->fp_reg, (int32_t)off - ss->stack_offset);
        int val = zc_alloc_temp(c);
        ir_emit_deref(c, val, sa);
        int addr = base;
        if (off != 0) {
            int o = zc_alloc_temp(c);
            ir_emit_mov_imm(c, (uint32_t)o, (int64_t)off);
            addr = zc_alloc_temp(c);
            ir_emit_add(c, (uint32_t)addr, (uint32_t)base, (uint32_t)o);
        }
        ir_emit_store_64(c, (uint32_t)val, (uint32_t)addr);
    }
}

/* Phase 3a: by-reference capture takes the ADDRESS of the captured
 * enclosing local. If any nested function defined in this body captures
 * one of THIS function's parameters by reference, that parameter needs a
 * stable frame slot - parameter promotion (value kept in a vreg, no slot)
 * would make the captured address bogus. The snapshot index ci refers to
 * the enclosing function's locals at parse time, params first, so
 * ci < param_count identifies a captured parameter. */
static bool zero_cc_param_captured_by_nested(const ZeroCCCompiler *c, const ZeroCCFunction *fn) {
    size_t fn_idx = (size_t)(fn - c->functions);
    for (size_t f = 0; f < c->function_count; f++) {
        const ZeroCCFunction *nf = &c->functions[f];
        if (!nf->is_nested || nf->nested_enclosing_fn_idx != fn_idx) continue;
        if (!nf->nested_capture_byref) continue;
        for (size_t ci = 0; ci < nf->nested_captured_count; ci++) {
            if (nf->nested_capture_byref[ci] && ci < fn->param_count) return true;
        }
    }
    return false;
}

/* Phase 3b: true when this (nested) function's own hidden params include
 * a by-reference capture pointer. Such params must NOT be promoted: a
 * promoted param lives in a vreg holding the POINTER value, and body
 * accesses would use the vreg directly instead of dereferencing it
 * (c-torture execute/20030501-1: frameless nested fn read the enclosing
 * local's address as the local's value). */
static bool zero_cc_fn_has_capture_mirror(const ZeroCCCompiler *c, const ZeroCCFunction *fn) {
    (void)c;
    if (!fn->is_nested || !fn->nested_capture_byref) return false;
    for (size_t ci = 0; ci < fn->nested_captured_count; ci++) {
        if (fn->nested_capture_byref[ci]) return true;
    }
    return false;
}

/* Phase 3a: true when any nested function in this body captures ANY local
 * of this function by reference. The captured locals must keep stable
 * frame slots in this function: the nested call site materializes their
 * addresses (zc_sym_addr) and the nested mirror-out writes back through
 * them - mem2reg promoting a captured local would leave both dangling.
 * Used to disable mem2reg for the enclosing function. */
static bool zero_cc_any_byref_capture(const ZeroCCCompiler *c, const ZeroCCFunction *fn) {
    size_t fn_idx = (size_t)(fn - c->functions);
    for (size_t f = 0; f < c->function_count; f++) {
        const ZeroCCFunction *nf = &c->functions[f];
        if (!nf->is_nested || nf->nested_enclosing_fn_idx != fn_idx) continue;
        if (!nf->nested_capture_byref) continue;
        for (size_t ci = 0; ci < nf->nested_captured_count; ci++) {
            if (nf->nested_capture_byref[ci]) return true;
        }
    }
    return false;
}

void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    /* The function being lowered is the most recently registered one at
     * entry (unless the caller pinned it explicitly via
     * pending_lowering_fn_idx - nested functions registered while parsing
     * the enclosing body would otherwise shadow it in c->functions).
     * Body lowering can append entries to c->functions afterwards
     * (builtins mapped to libc symbols register their extern during
     * lowering), so capture the identity now for the implicit-`return 0`
     * check, diagnostics, and the -dump-ir label below. */
    size_t current_fn_idx;
    if (c->has_pending_lowering_fn && c->pending_lowering_fn_idx < c->function_count) {
        current_fn_idx = c->pending_lowering_fn_idx;
    } else {
        current_fn_idx = (c->function_count > 0) ? (c->function_count - 1) : 0;
        while (current_fn_idx > 0 && c->functions[current_fn_idx].is_prototype) {
            current_fn_idx--;
        }
    }
    c->has_pending_lowering_fn = false;
    ZeroCCFunction *lowering_fn = (c->function_count > 0)
        ? &c->functions[current_fn_idx] : NULL;

    /* Initialize lowering state */
    memset(&c->lowering, 0, sizeof(c->lowering));
    c->lowering.current_func_idx = current_fn_idx;
    memset(&c->label_patcher, 0, sizeof(c->label_patcher));
    ir_emitter_init(&c->lowering.ir_ctx);



    /* Block scope starts unbounded at the function top level (-1 = unbounded) */
    c->local_lookup_bound = -1;

    /* Initialize VLA state */
    c->lowering.vla.has_runtime_vla = false;
    c->lowering.vla.vla_count = 0;

    /* Sprint 135: per-function inline-asm operand temp lists */
    c->asm_operand_count = 0;
    c->lowering.vla.block_depth = 0;
    c->lowering.vla.loop_depth = 0;

    /* Reset register allocator for this function */
    regalloc_init(&c->regalloc);
    regalloc_set_isa(&c->regalloc, c->isa);  /* Set target ISA and reserved registers */
    c->next_temp_id = VREG_USER_START;
    c->ir_next_label = 0;

    /* Reset label patcher state */
    label_patcher_init(&c->label_patcher);

    /* Reset IR buffer */
    ir_reset(c);

    /* Prescan AST for address-taken variables (both static locals and automatic locals) */
    zero_cc_prescan_static_addr_taken(c, arena, root);

    /* Determine stack protector requirements */
    bool needs_protect = false;
    if (c->stack_protector == 2) {
        /* -fstack-protector-all: protect every function */
        needs_protect = true;
    } else if (c->stack_protector != 0) {
        for (size_t i = 0; i < c->local_count; i++) {
            ZeroCCSym *s = &c->locals[i];
            if (s->is_vla) {
                needs_protect = true;
                break;
            }
            if (s->type.tag == TYPE_ARRAY) {
                if (c->stack_protector == 3 || s->type.size >= 8) {
                    needs_protect = true;
                    break;
                }
            }
            if (c->stack_protector == 3) {
                if (zc_type_contains_array(&s->type)) {
                    needs_protect = true;
                    break;
                }
                if (s->addr_taken) {
                    needs_protect = true;
                    break;
                }
            }
        }
    }
    c->lowering.has_stack_protector = needs_protect;
    c->lowering.canary_stack_off = needs_protect ? c->canary_stack_offset : 0;

    /* Determine if incoming parameters can be kept directly in virtual registers (frameless leaf) */
    bool can_promote_params = false;
    if (!needs_protect && lowering_fn && !c->debug_info) {
        ZeroCCFunction *fn = lowering_fn;
        int max_int_params = (c->isa == ISA_X86_64) ? 3 : 8;
        if (!fn->is_variadic && fn->ret_struct_sz <= 16 &&
            c->local_count == fn->param_count &&
            !zero_cc_ast_has_call(arena, root, NULL) &&
            !zero_cc_ast_has_asm(arena, root) &&
            !zero_cc_param_captured_by_nested(c, fn) &&
            !zero_cc_fn_has_capture_mirror(c, fn)) {
            /* Member access on a parameter (p->m / s.m) is rejected: the
             * member base-load paths read the parameter from its frame slot
             * (stack_offset, which is 0 when promoted) instead of the
             * promoted vreg -- in a frameless function that slot is the
             * caller's saved %rbp. See zero_cc_ast_member_on_param. */
            if (!zero_cc_ast_addr_of_param(arena, root, fn->param_names, (int)fn->param_count) &&
                !zero_cc_ast_member_on_param(arena, root, fn->param_names, (int)fn->param_count) &&
                !zero_cc_ast_assigns_to_param(arena, root, fn->param_names, (int)fn->param_count)) {
                can_promote_params = true;
                int chk_int = 0;
                int chk_fp = 0;
                for (size_t p = 0; p < fn->param_count && p < (size_t)c->local_count; p++) {
                    ZeroCCSym *sym = &c->locals[p];
                    if (!sym->is_param) continue;
                    /* GNU 128-bit parameters consume TWO consecutive GP
                     * argument units and the promoted-vreg model has no
                     * (lo,hi) pair form - use the frame path (chunked
                     * PARAM_SAVE_INT x2), which handles both the register
                     * window and the caller-stack overflow. */
                    if (sym->type.tag == TYPE_INT128 || sym->type.tag == TYPE_UINT128) {
                        can_promote_params = false; break;
                    }
                    /* Sprint 172 (rv32 i64 ABI): 64-bit parameters consume TWO
                     * GP argument units on rv32 - use the frame path (is_64_scalar). */
                    if (c->isa == ISA_RISCV32 &&
                        (sym->type.tag == TYPE_INT64 || sym->type.tag == TYPE_UINT64)) {
                        can_promote_params = false; break;
                    }
                    /* C99 complex parameters consume TWO consecutive FP
                     * argument registers and need an addressable frame
                     * object (address-form value) - no promotion. */
                    if (zero_cc_type_is_complex(&sym->type)) {
                        can_promote_params = false; break;
                    }
                    bool is_fp = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
                    if (is_fp) {
                        if (sym->type.tag == TYPE_FLOAT) { can_promote_params = false; break; }
                        if (chk_fp >= BE(c)->conv->fp_arg_reg_count) { can_promote_params = false; break; }
                        chk_fp++;
                    } else {
                        if (((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8) ||
                            (sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&sym->type))) {
                            /* Structs >8B and vectors of any size: the value
                             * in a register has no addressable object; the
                             * per-lane / copy machinery needs the frame slot. */
                            can_promote_params = false; break;
                        }
                        if (chk_int >= max_int_params) { can_promote_params = false; break; }
                        chk_int++;
                    }
                }
            }
        }
    }

    /* C99 6.7.6.2: a VLA parameter dimension expression (e.g.
     * `int f(int i, int a[i++])`) is evaluated at entry in parameter
     * order; it may read/write earlier parameters, which requires the
     * params in frame slots - so promotion must be off. */
    if (can_promote_params && lowering_fn && lowering_fn->param_dim_exprs) {
        for (size_t p = 0; p < lowering_fn->param_count; p++) {
            if (lowering_fn->param_dim_exprs[p] != AST_NULL) {
                can_promote_params = false;
                break;
            }
        }
    }

    /* Sprint 99: nonlocal goto pairs must stay in frame slots - the
     * goto lowering and multi-level forwarding reload them by name at
     * the jump/call sites, where a promoted vreg would be dead (clobbered
     * by intervening calls). */
    if (can_promote_params && lowering_fn && lowering_fn->is_nested &&
        lowering_fn->goto_pair_count > 0) {
        can_promote_params = false;
    }

    /* C99 6.7.6.2: reserve a runtime size slot for each VLA parameter
     * (the dim value evaluated at entry below) - runtime `sizeof` on a
     * VLA param type (e.g. `sizeof(a[0])` for `char a[2][N]`) loads it
     * via NODE_VLA_SLOT. Must run before the body lowers so the frame
     * still grows - and BEFORE the param-save loop: a VLA struct
     * parameter's size word arrives as an ABI argument stored into this
     * slot during the save. */
    if (lowering_fn && lowering_fn->param_count > 0) {
        for (size_t p = 0; p < lowering_fn->param_count && p < c->local_count; p++) {
            if (!c->locals[p].is_param || c->locals[p].vla_slot_off) continue;
            ZeroCCType t = c->locals[p].type;
            while (t.tag == TYPE_PTR && t.base) t = *t.base;
            if (zc_type_chain_has_vla(t)) {
                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                c->current_stack_frame += 8;
                c->locals[p].vla_slot_off = (int32_t)c->current_stack_frame;
            }
        }
    }

    /* VLA struct parameters must stay in frame slots: the slot holds the
     * object pointer (member access loads it) and the size word lives in
     * a separate VLA slot - a single promoted vreg cannot model both. */
    if (can_promote_params && lowering_fn) {
        for (size_t p = 0; p < lowering_fn->param_count && p < (size_t)c->local_count; p++) {
            ZeroCCSym *ps = &c->locals[p];
            if (ps->is_param && zc_type_is_vla_struct_obj(&ps->type)) {
                can_promote_params = false;
                break;
            }
        }
    }

    if (can_promote_params) {
        c->current_stack_frame = 0;
        ZeroCCFunction *fn = lowering_fn;
        int int_idx = 0;
        int fp_idx = 0;
        for (size_t p = 0; p < fn->param_count && p < (size_t)c->local_count; p++) {
            ZeroCCSym *sym = &c->locals[p];
            if (!sym->is_param) continue;
            sym->stack_offset = 0;
            sym->is_promoted = true;
            sym->promoted_vreg = zc_alloc_temp(c);
            /* Phase 3b: a by-ref capture's ABI param is a POINTER (GPR),
             * but sym->type is retyped to the pointee for body lowering -
             * a double capture would misclassify as an FP arg here. */
            bool cap_byref = (p < fn->nested_captured_count && fn->nested_capture_byref &&
                              fn->nested_capture_byref[p]);
            bool is_fp = !cap_byref && (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
            int arg_idx = is_fp ? fp_idx++ : int_idx++;
            ir_emit_param_load(c, sym->promoted_vreg, arg_idx, is_fp);
            if (!is_fp) {
                zero_cc_emit_int_extend(c, sym->promoted_vreg, &sym->type);
            }
        }
    } else if (lowering_fn) {
        ZeroCCFunction *fn = lowering_fn;
        int xmm_idx = 0;
        int int_idx = 0;
        int stack_idx = 0;

        /* >16 byte struct returns use hidden sret pointer:
         * - System V AMD64 ABI (x86-64): RDI (param_idx=0), real params start at RSI (int_idx=1)
         * - RISC-V LP64D ABI: a0 (param_idx=0), real params start at a1 (int_idx=1)
         * - AArch64 AAPCS64 ABI: x8 (Indirect Result Location Register, param_idx=8), real params start at x0 (int_idx=0) */
        bool uses_sret = fn->ret_struct_sz > 16;
        /* >16B struct/union params: pending hidden-pointer copies to run
         * after ALL param saves (see the branch below for why). */
        int32_t big_param_pt_off[64];
        int32_t big_param_dst_off[64];
        size_t big_param_size[64];
        int big_param_copy_count = 0;
        /* FP params arriving on the CALLER STACK (beyond the FP arg
         * register window): their loads use LEA'd integer temps that the
         * register allocator may place in not-yet-spilled argument
         * registers (rdi/rsi/...), clobbering the variadic spill - so
         * defer them until after it (same pattern as the big-param
         * hidden-pointer copies). */
        int32_t fpstack_src_off[64];
        int32_t fpstack_dst_off[64];
        uint8_t fpstack_is_flt[64];
        int fpstack_copy_count = 0;
        if (uses_sret) {
            if (c->isa == ISA_AARCH64) {
                ir_emit_param_save_int(c, 8, fn->sret_stack_offset);  /* param_idx=8 means x8 on AArch64 */
                int_idx = 0;  /* Real params still start at x0 */
            } else {
                ir_emit_param_save_int(c, 0, fn->sret_stack_offset);  /* param_idx=0 means RDI / a0 */
                int_idx = 1;  /* Real params start at RSI / a1 */
            }
        }

        for (size_t p = 0; p < fn->param_count && p < (size_t)c->local_count; p++) {
            ZeroCCSym *sym = &c->locals[p];
            if (!sym->is_param) continue;
            /* Phase 3b: a by-ref capture's ABI param is a POINTER (GPR),
             * but sym->type is retyped to the pointee for body lowering -
             * a double/float capture would misclassify as an FP arg and
             * the stub's GPR-pointer placement would not match. */
            bool cap_byref = (p < fn->nested_captured_count && fn->nested_capture_byref &&
                              fn->nested_capture_byref[p]);
            bool is_fp = !cap_byref && (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
            bool is_multi_chunk_struct = (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION ||
                                          (sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&sym->type))) && sym->type.size > 8;
            /* GNU 128-bit integer parameter: TWO consecutive GP units (lo
             * then hi), exactly the 16-byte struct chunk pattern. */
            bool is_128_scalar = (sym->type.tag == TYPE_INT128 || sym->type.tag == TYPE_UINT128);
            if (is_128_scalar) is_multi_chunk_struct = true;
            bool is_64_scalar = (c->isa == ISA_RISCV32 &&
                                 (sym->type.tag == TYPE_INT64 || sym->type.tag == TYPE_UINT64));

            if (is_64_scalar) {
                if (int_idx & 1) int_idx++; /* 2*XLEN arg aligns to an even argument register */
                if (int_idx + 1 < BE(c)->conv->int_arg_reg_count) {
                    ir_emit_param_save_int(c, int_idx, sym->stack_offset);
                    ir_emit_param_save_int(c, int_idx + 1, sym->stack_offset - 4);
                    int_idx += 2;
                } else {
                    int_idx = BE(c)->conv->int_arg_reg_count;
                    if (stack_idx & 1) stack_idx++; /* 8-byte aligned on stack */
                    int32_t caller_off_lo = 16 + 4 * stack_idx;
                    int32_t caller_off_hi = 16 + 4 * (stack_idx + 1);
                    ir_emit_load(c, VREG_0, BE(c)->fp_reg, caller_off_lo);
                    ir_emit_store(c, VREG_0, BE(c)->fp_reg, -sym->stack_offset);
                    ir_emit_load(c, VREG_0, BE(c)->fp_reg, caller_off_hi);
                    ir_emit_store(c, VREG_0, BE(c)->fp_reg, -(sym->stack_offset - 4));
                    stack_idx += 2;
                }
            } else if (!cap_byref && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) &&
                zc_type_is_vla_struct_obj(&sym->type)) {
                /* VLA struct parameter (GNU): two consecutive GPR units -
                 * the object pointer (stored in the sym slot; member
                 * access loads it through sym->is_vla_struct) and the
                 * total runtime size (stored in the reserved VLA slot for
                 * runtime sizeof). */
                for (int unit = 0; unit < 2; unit++) {
                    int32_t dst = (unit == 0) ? sym->stack_offset
                                               : (sym->vla_slot_off > 0 ? sym->vla_slot_off : sym->stack_offset);
                    if (int_idx < BE(c)->conv->int_arg_reg_count) {
                        ir_emit_param_save_int(c, int_idx, dst);
                        int_idx++;
                    } else {
                        int32_t caller_off = 16 + 8 * stack_idx;
                        ir_emit_load(c, VREG_0, BE(c)->fp_reg, caller_off);
                        ir_emit_store(c, VREG_0, BE(c)->fp_reg, -dst);
                        stack_idx++;
                    }
                }
            } else if (is_multi_chunk_struct && sym->type.size > 16 &&
                !(sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&sym->type))) {
                if (c->isa == ISA_X86_64) {
                    /* Sprint 158 (x86-64): a >16B struct/union parameter is
                     * a System V MEMORY argument - the caller placed the
                     * value in the CALLER STACK at this argument's order
                     * position (8-byte aligned, ceil(size/8) slots, no
                     * register consumed). Read the chunks from there; the
                     * pre-158 hidden-pointer convention only applies to the
                     * psABI ISAs (AArch64/RISC-V). */
                    int nch = (int)((sym->type.size + 7) / 8);
                    for (int ch = 0; ch < nch; ch++) {
                        int32_t chunk_off = sym->stack_offset - ch * 8;
                        int32_t caller_off = 16 + 8 * stack_idx + 8 * ch;
                        ir_emit_load(c, VREG_0, BE(c)->fp_reg, caller_off);
                        ir_emit_store(c, VREG_0, BE(c)->fp_reg, -chunk_off);
                    }
                    stack_idx += nch;
                } else {
                /* psABI ABI: a >16B struct/union parameter arrives as a hidden
                 * pointer in ONE argument slot. Save the pointer into the
                 * first chunk's frame slot now (while every argument
                 * register is still live) but defer the pointer->storage
                 * copy until AFTER the whole param-save loop: the copy loop
                 * emits fresh temps that the register allocator may place in
                 * not-yet-saved argument registers (rsi/rdx/...), corrupting
                 * the later PARAM_SAVE_INTs (c-torture execute/20030914-2). */
                int32_t pt_off = sym->stack_offset;
                if (int_idx < BE(c)->conv->int_arg_reg_count) {
                    ir_emit_param_save_int(c, int_idx, pt_off);
                    int_idx++;
                } else {
                    int32_t caller_off = 16 + 8 * stack_idx;
                    ir_emit_load(c, VREG_0, BE(c)->fp_reg, caller_off);
                    ir_emit_store(c, VREG_0, BE(c)->fp_reg, -pt_off);
                    stack_idx++;
                }
                if (big_param_copy_count < 64) {
                    big_param_pt_off[big_param_copy_count] = pt_off;
                    big_param_dst_off[big_param_copy_count] = sym->stack_offset;
                    big_param_size[big_param_copy_count] = sym->type.size;
                    big_param_copy_count++;
                }
                }
            } else if (is_multi_chunk_struct) {
                int num_chunks = (int)((sym->type.size + 7) / 8);
                for (int ch = 0; ch < num_chunks; ch++) {
                    int32_t chunk_off = sym->stack_offset - ch * 8;
                    /* Sprint 158 (x86-64): per-unit class merge - a 9-16B
                     * struct/union unit that holds a float/double member
                     * rides the SSE stream (XMM registers, then the shared
                     * caller-stack FP stream), the other units ride the GPR
                     * stream. Matches the caller-side placement in
                     * ast_lowering_call.c (s03_struct16 interop). */
                    bool unit_fp = (c->isa == ISA_X86_64 &&
                                    (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) &&
                                    zc_struct_unit_is_fp(&sym->type, ch));
                    if (unit_fp) {
                        if (xmm_idx < BE(c)->conv->fp_arg_reg_count) {
                            ir_emit_param_save_fp(c, xmm_idx, chunk_off, 0);
                        } else {
                            if (fpstack_copy_count < 64) {
                                fpstack_src_off[fpstack_copy_count] = 16 + 8 * stack_idx;
                                fpstack_dst_off[fpstack_copy_count] = chunk_off;
                                fpstack_is_flt[fpstack_copy_count] = 0;
                                fpstack_copy_count++;
                            }
                            stack_idx++;
                        }
                        xmm_idx++;
                    } else if (int_idx < BE(c)->conv->int_arg_reg_count) {
                        ir_emit_param_save_int(c, int_idx, chunk_off);
                        int_idx++;
                    } else {
                        int32_t caller_off = 16 + 8 * stack_idx;
                        ir_emit_load(c, VREG_0, BE(c)->fp_reg, caller_off);
                        ir_emit_store(c, VREG_0, BE(c)->fp_reg, -chunk_off);
                        stack_idx++;
                    }
                }
            } else if (is_fp && zero_cc_type_is_complex(&sym->type)) {
                /* C99 complex parameter.
                 * psABI (x86-64/AArch64/RV64): _Complex float arrives in
                 * ONE FP register - real in the low 32 bits, imaginary in
                 * the HIGH 32 bits - and the 8-byte bit pattern matches the
                 * {real, imag} object layout, so one 8-byte param save
                 * captures both halves. _Complex double (and RV32
                 * _Complex float) use TWO consecutive FP slots, one per
                 * half: registers first, then the shared caller-stack
                 * stream in argument order. */
                int32_t esz = (sym->type.base && sym->type.base->size == 4) ? 4 : 8;
                bool is_flt = (esz == 4);
                if (is_flt && c->isa != ISA_RISCV32) {
                    if (xmm_idx < BE(c)->conv->fp_arg_reg_count) {
                        ir_emit_param_save_fp(c, xmm_idx, sym->stack_offset, 0);
                    } else {
                        if (fpstack_copy_count < 64) {
                            fpstack_src_off[fpstack_copy_count] = 16 + 8 * stack_idx;
                            fpstack_dst_off[fpstack_copy_count] = sym->stack_offset;
                            fpstack_is_flt[fpstack_copy_count] = 0;
                            fpstack_copy_count++;
                        }
                        stack_idx++;
                    }
                    xmm_idx++;
                } else {
                    for (int half = 0; half < 2; half++) {
                        int32_t half_off = sym->stack_offset - (int32_t)half * esz;
                        if (xmm_idx < BE(c)->conv->fp_arg_reg_count) {
                            ir_emit_param_save_fp(c, xmm_idx, half_off, is_flt);
                        } else {
                            /* Stack half: defer the load until after the
                             * variadic spill (see fpstack_* above). */
                            if (fpstack_copy_count < 64) {
                                fpstack_src_off[fpstack_copy_count] = 16 + 8 * stack_idx;
                                fpstack_dst_off[fpstack_copy_count] = half_off;
                                fpstack_is_flt[fpstack_copy_count] = is_flt ? 1 : 0;
                                fpstack_copy_count++;
                            }
                            stack_idx++;
                        }
                        xmm_idx++;
                    }
                }
            } else if (!is_fp && zero_cc_type_is_complex(&sym->type)) {
                /* Integer complex parameter:
                 * <= 8 bytes (_Complex char/short/int) fits in 1 GPR.
                 * > 8 bytes (_Complex long, 16B) consumes 2 consecutive GPRs (real then imag). */
                int num_chunks = (int)((sym->type.size + 7) / 8);
                for (int ch = 0; ch < num_chunks; ch++) {
                    int32_t chunk_off = sym->stack_offset - ch * 8;
                    if (int_idx < BE(c)->conv->int_arg_reg_count) {
                        ir_emit_param_save_int(c, int_idx, chunk_off);
                        int_idx++;
                    } else {
                        int32_t caller_off = 16 + 8 * stack_idx;
                        ir_emit_load(c, VREG_0, BE(c)->fp_reg, caller_off);
                        ir_emit_store(c, VREG_0, BE(c)->fp_reg, -chunk_off);
                        stack_idx++;
                    }
                }
            } else if (is_fp && xmm_idx < BE(c)->conv->fp_arg_reg_count) {
                ir_emit_param_save_fp(c, xmm_idx, sym->stack_offset, sym->type.tag == TYPE_FLOAT ? 1 : 0);
                xmm_idx++;
            } else if (is_fp) {
                /* FP argument beyond the register window: shared
                 * caller-stack stream in argument order.  Deferred like
                 * the complex stack halves (see fpstack_* above). */
                if (fpstack_copy_count < 64) {
                    fpstack_src_off[fpstack_copy_count] = 16 + 8 * stack_idx;
                    fpstack_dst_off[fpstack_copy_count] = sym->stack_offset;
                    fpstack_is_flt[fpstack_copy_count] = (sym->type.tag == TYPE_FLOAT) ? 1 : 0;
                    fpstack_copy_count++;
                }
                stack_idx++;
                xmm_idx++;
            } else if (!is_fp && int_idx < BE(c)->conv->int_arg_reg_count) {
                ir_emit_param_save_int(c, int_idx, sym->stack_offset);
                int_idx++;
            } else if (!is_fp) {
                int32_t caller_off = 16 + (c->isa == ISA_RISCV32 ? 4 : 8) * stack_idx;
                ir_emit_load(c, VREG_0, BE(c)->fp_reg, caller_off);
                ir_emit_store(c, VREG_0, BE(c)->fp_reg, -sym->stack_offset);
                stack_idx++;
            }
        }

        if (fn->is_variadic) {
            /* Save area: (int_arg_reg_count + fp_arg_reg_count) 8-byte slots
             * above ALL named locals - the integer arg registers first, then
             * the FP arg registers (16 slots on x86-64/AArch64/RV64).
             * va_start/va_arg walk them as two streams (see the NODE_VA_START
             * lowering below). Placed above the locals (not right after the
             * last parameter, as before) so body stores to locals cannot
             * clobber spilled variadic args. */
            int va_slots = BE(c)->conv->int_arg_reg_count + BE(c)->conv->fp_arg_reg_count;
            int32_t base_spill_offset = c->current_stack_frame + 8;
            c->current_stack_frame = (int32_t)(((size_t)(base_spill_offset + va_slots * 8) + 15) & ~(size_t)15);
            ir_emit_variadic_spill(c, (int)fn->param_count, base_spill_offset);
        }

        /* Deferred hidden-pointer copies (see the param-save loop above):
         * all argument registers are now saved AND the variadic spill has
         * captured the arg registers, so the copy loop's temps can be
         * allocated freely (they may reuse the arg registers). */
        for (int bp = 0; bp < big_param_copy_count; bp++) {
            int pt = zc_alloc_temp(c);
            ir_emit_load(c, pt, BE(c)->fp_reg, -big_param_pt_off[bp]);
            int dst_addr = zc_alloc_temp(c);
            ir_emit_lea(c, dst_addr, BE(c)->fp_reg, -big_param_dst_off[bp]);
            zc_emit_mem_copy(c, pt, dst_addr, big_param_size[bp]);
        }

        /* Deferred caller-stack FP param loads (see fpstack_* above): all
         * argument registers are now saved / spilled, so the LEA temps
         * can reuse them freely. */
        for (int fs = 0; fs < fpstack_copy_count; fs++) {
            int ca = zc_alloc_temp(c);
            ir_emit_lea(c, ca, BE(c)->fp_reg, fpstack_src_off[fs]);
            int hv = zc_alloc_temp(c);
            if (fpstack_is_flt[fs]) ir_emit_vloadss(c, (uint32_t)hv, (uint32_t)ca);
            else ir_emit_vloadsd(c, (uint32_t)hv, (uint32_t)ca);
            int la = zc_alloc_temp(c);
            ir_emit_lea(c, la, BE(c)->fp_reg, -fpstack_dst_off[fs]);
            if (fpstack_is_flt[fs]) ir_emit_vstoress(c, (uint32_t)la, (uint32_t)hv);
            else ir_emit_vstoresd(c, (uint32_t)la, (uint32_t)hv);
        }
    }

    /* Phase 3b: by-reference captures - the hidden pointer params are
     * routed through directly by zc_sym_load/store/addr (no mirror slot),
     * but mem2reg must still stay off: the pointer param's frame slot is
     * a stable address every access reloads. */
    if (lowering_fn && lowering_fn->is_nested && lowering_fn->nested_captured_count > 0 &&
        lowering_fn->nested_capture_byref) {
        for (size_t ci = 0; ci < lowering_fn->nested_captured_count; ci++) {
            if (lowering_fn->nested_capture_byref[ci]) { c->lowering.has_capture_mirror = true; break; }
        }
    }

    /* Phase 3a: the ENCLOSING function must also keep its captured locals
     * in stack slots (see zero_cc_any_byref_capture) - disable mem2reg for
     * it as well. */
    if (lowering_fn && !lowering_fn->is_nested && zero_cc_any_byref_capture(c, lowering_fn)) {
        c->lowering.has_capture_mirror = true;
    }

    /* C99 6.7.6.2: evaluate VLA parameter dimension expressions in
     * parameter order; all params are already in their frame slots and
     * a dimension may reference earlier params (the array storage lives
     * in the caller's frame, so only the side effects matter). */
    if (lowering_fn && lowering_fn->param_dim_arena && lowering_fn->param_dim_exprs) {
        ZeroCCASTArena *dim_arena = (ZeroCCASTArena *)lowering_fn->param_dim_arena;
        for (size_t p = 0; p < lowering_fn->param_count; p++) {
            /* A param can have several VLA dims (chained via next,
             * outermost-to-innermost): evaluate them all (side effects),
             * and save the INNERMOST value - it determines the pointed-to
             * array size that runtime sizeof reads. */
            ASTRef dim = lowering_fn->param_dim_exprs[p];
            int last_val = -1;
            while (dim != AST_NULL) {
                ZeroCCASTNode *dn = zero_cc_ast_get(dim_arena, dim);
                last_val = lower_expr(c, dim_arena, dim);
                dim = dn ? dn->next : AST_NULL;
            }
            if (last_val >= 0 && p < c->local_count && c->locals[p].vla_slot_off > 0) {
                ir_emit_store(c, last_val, BE(c)->fp_reg, -c->locals[p].vla_slot_off);
            }
        }
    }

    c->lowering.epilogue_label = c->ir_next_label++;

    /* Sprint 37: static locals. When the function has at least one, reserve
     * an 8-byte frame slot that shields VREG_0 (return value / sret
     * pointer) across the epilogue mirror-out below: the mirror-out uses
     * allocator temps that may be mapped onto the return register, and the
     * return value is already live in VREG_0 by the time the epilogue runs.
     * (Must happen before the frame size is computed below.) */
    c->lowering.static_local_rt_off = 0;
    c->lowering.static_local_rt2_off = 0;
    bool need_rt_slot = false;
    for (size_t i = 0; i < c->local_count; i++) {
        if (c->locals[i].is_static_local) { need_rt_slot = true; break; }
    }
    if (need_rt_slot) {
        c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
        c->current_stack_frame += 8;
        c->lowering.static_local_rt_off = (int)c->current_stack_frame;
        c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
        c->current_stack_frame += 8;
        c->lowering.static_local_rt2_off = (int)c->current_stack_frame;
    }

    /* Sprint 99: nonlocal goto - if a nested function with goto pairs is
     * defined in THIS function, call sites (direct call or address-taking)
     * must pass this function's body-time SP as the pair's rsp half. The
     * exact value depends on the prologue mode (OFP / red-zone / realign),
     * so the prologue stores SP into a reserved frame slot right after
     * frame setup and the call site loads that slot. MUST run before
     * lower_node - the call sites read the offset while lowering the body.
     * current_stack_frame is final here (params + locals + VLA/variadic
     * extensions all happened earlier). */
    c->lowering.sp_snap_off = 0;
    c->lowering.fp_snap_off = 0;
    if (lowering_fn && c->function_count > 0) {
        size_t cur_idx = c->lowering.current_func_idx;
        for (size_t fi = 0; fi < c->function_count; fi++) {
            const ZeroCCFunction *nf2 = &c->functions[fi];
            if (nf2->is_nested && nf2->goto_pair_count > 0 &&
                nf2->nested_enclosing_fn_idx == cur_idx) {
                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                c->current_stack_frame += 8;
                c->lowering.sp_snap_off = (int32_t)c->current_stack_frame;
                c->current_stack_frame += 8;
                c->lowering.fp_snap_off = (int32_t)c->current_stack_frame;
                break;
            }
        }
    }

    /* Sprint 37: static locals - data section -> stack slot, once per call,
     * before any body statement can read the (mirrored) slot.  Sprint 72:
     * skip the mirror for address-taken statics - their slot is never used
     * (every access goes to the data section directly). */
    for (size_t i = 0; i < c->local_count; i++) {
        if (c->locals[i].is_static_local && !c->locals[i].static_addr_taken)
            static_local_mirror_in(c, &c->locals[i]);
    }

    /* Lower AST to IR (instruction selection) */
    lower_node(c, arena, root);
    /* C99 5.1.2.2.3p1: reaching the '}' that terminates main returns 0.
     * Test the captured entry-time identity: lowering the body may have
     * appended extern entries after main (builtins mapped to libc symbols),
     * so re-reading functions[function_count - 1] here would miss main. */
    if (lowering_fn && strcmp(lowering_fn->name, "main") == 0) {
        ir_emit_mov_imm(c, VREG_0, 0);
    }
    ir_emit_label(c, c->lowering.epilogue_label);

    /* Sprint 37: static locals - stack slot -> data section. Every return
     * jumps to the epilogue label and natural fallthrough reaches it, so
     * this single block runs on every exit path.
     *
     * The mirror-out's allocator temps may be assigned to the return
     * registers, so protect VREG_0 AND VREG_2 in reserved frame slots
     * around the block (9-16B integer struct returns live in
     * VREG_0/VREG_2 = rax/rdx; c-torture execute/pr92618). FP results
     * live in XMM/FP registers, which the integer-only mirror-out
     * cannot touch. CALLEE_SAVE_LOADS runs after the restore and
     * re-establishes all callee-saved state. */
    if (c->lowering.static_local_rt_off != 0) {
        ir_emit_store(c, VREG_0, BE(c)->fp_reg, -c->lowering.static_local_rt_off);
        ir_emit_store(c, VREG_2, BE(c)->fp_reg, -c->lowering.static_local_rt2_off);
        for (size_t i = 0; i < c->local_count; i++) {
            /* Sprint 72: skip the mirror-out for address-taken statics - the
             * slot is stale scratch and flushing it would clobber the data
             * section object a callee may have written through the escaped
             * pointer. */
            if (c->locals[i].is_static_local && !c->locals[i].static_addr_taken)
                static_local_mirror_out(c, &c->locals[i]);
        }
        ir_emit_load(c, VREG_2, BE(c)->fp_reg, -c->lowering.static_local_rt2_off);
        ir_emit_load(c, VREG_0, BE(c)->fp_reg, -c->lowering.static_local_rt_off);
    }

    /* Sprint 151 (plan item 9): temp-ID renumbering by liveness. Compacts
     * the function's churned id space (32-bit storage may now hold well
     * past the old 16-bit budget) down to the PEAK-LIVE set before any
     * optimizer pass or register allocation sees it. This replaces the
     * old C2b churn guard: the backstop now trips on peak-live temps
     * (>= 65535) - a true resource limit - instead of total allocation
     * churn (~3 ids per `t = t + 1` statement). */
    if (!opt_temp_renumber(c, lowering_fn ? lowering_fn->name : "<anon>")) {
        return;
    }

    /* 1. Run IR optimization passes FIRST (before register allocation) */
    opt_init(c->opt_level);
    opt_run_passes(c);
    opt_shutdown();

    /* Debug: dump the post-optimization IR (before register allocation),
     * e.g. to inspect loop-unroll / LICM output: ZC_DUMP_IR_OPT=1 zerocc ... */
    if (getenv("ZC_DUMP_IR_OPT")) {
        ir_dump(c, stderr);
    }

    /* 2. Compute live ranges and allocate registers on optimized IR */
    LiveRangeAnalyzer lra;
    live_range_init(&lra);
    live_range_scan_ir(&lra, ir_get_buffer(c), ir_get_count(c), c);
    live_range_compute(&lra, &c->regalloc, c);

    c->regalloc.spill_across_calls = c->lowering.has_setjmp;
    regalloc_allocate(&c->regalloc);

    /* Sprint 135: inline-asm register pressure - an operand temp that could
     * not be kept in a register is a clean reject (the template text needs
     * the value in a register at the asm site). */
    if (c->regalloc.asm_pressure_fail) {
        diag_report_error_at_token(c,
            "inline asm: register pressure - cannot keep an asm operand in a register");
        c->had_error = 1;
    }

    /* Sprint 135: clobbered registers. An interval live across an OP_ASM
     * whose assigned register the template clobbers cannot keep its value
     * there - mark it spilled (the emitters already materialize spilled
     * temps through the frame spill slots). The asm's own operand temps are
     * exempt (they must be in registers at the site); the pre-colored
     * return-value temps are a clean reject (their value would be lost). */
    {
        bool asm_clobber_lost = false;
        size_t icount = ir_get_count(c);
        for (size_t i = 0; i < icount; i++) {
            const Instr *ins = ir_get(c, i);
            if (!ins || ins->op != OP_ASM) continue;
            /* Sprint 154: the clobber mask is packed into the anchor's imm
             * (14-bit GPR pool mask). */
            uint32_t clobber_mask = (uint32_t)(uint16_t)ins->imm;
            if (clobber_mask == 0) continue;
            for (int pool = 0; pool < 16; pool++) {
                if (!(clobber_mask & (1u << pool))) continue;
                for (size_t k = 0; k < c->regalloc.interval_count; k++) {
                    LiveInterval *iv = &c->regalloc.intervals[k];
                    if (iv->assigned_reg != pool || iv->is_spilled) continue;
                    if (iv->start > i || iv->end < i) continue;  /* not live at asm */
                    if (iv->temp_id == VREG_0 || iv->temp_id == VREG_2) {
                        /* Pending return value: dangerous only if the
                         * materializing store can FLOW into the asm. Every
                         * return site the lowering emits either jumps to the
                         * epilogue right after storing the return value or
                         * is the function's trailing store (epilogue
                         * directly after - no asm between). Scan back to the
                         * nearest def of this temp before the asm. */
                        bool def_flows = true;
                        for (size_t d = i; d > 0; d--) {
                            const Instr *di = ir_get(c, d - 1);
                            if (!di) continue;
                            if (di->temp_dest != iv->temp_id) continue;
                            const Instr *dn = ir_get(c, d);
                            def_flows = !(dn && (dn->op == OP_JMP ||
                                dn->op == OP_JMP_LABEL || dn->op == OP_JMP_INDIRECT ||
                                dn->op == OP_JE || dn->op == OP_JNE ||
                                dn->op == OP_JL || dn->op == OP_JLE ||
                                dn->op == OP_JG || dn->op == OP_JCOND ||
                                dn->op == OP_RET));
                            break;
                        }
                        if (def_flows) asm_clobber_lost = true;
                        continue;
                    }
                    bool is_operand = false;
                    /* Sprint 154: the operand temps are the in-stream
                     * OP_ASM_ARG run following the anchor. */
                    int op_count = (int)ins->temp_dest;
                    for (int o = 0; o < op_count; o++) {
                        const Instr *arg = ir_get(c, i + 1 + (size_t)o);
                        if (!arg || arg->op != OP_ASM_ARG) break;
                        if ((uint32_t)iv->temp_id == arg->temp_src1) { is_operand = true; break; }
                    }
                    if (is_operand) {
                        /* Safety net: allocation already forbids clobbered
                         * registers for asm operands (forbidden_mask); if one
                         * still landed here the value would be destroyed. */
                        asm_clobber_lost = true;
                        continue;
                    }
                    spill_interval(&c->regalloc, iv);
                }
            }
        }
        if (asm_clobber_lost) {
            diag_report_error_at_token(c,
                "inline asm: clobbers a register holding the pending return value or an asm operand");
            c->had_error = 1;
        }
    }

    /* Functions calling setjmp must save and restore all allocatable callee-saved
     * GPRs in their prologue/epilogue: a child frame may modify any CSR before
     * calling longjmp (which unwinds past the child's epilogue), so this frame's
     * epilogue is responsible for restoring its caller's full CSR state. */
    if (c->lowering.has_setjmp && BE(c)->conv && BE(c)->conv->callee_saved_gpr) {
        const int *callee_regs = BE(c)->conv->callee_saved_gpr;
        int count = BE(c)->conv->callee_saved_gpr_count;
        for (int i = 0; i < count; i++) {
            int reg = callee_regs[i];
            if (regalloc_is_callee_saved_gpr(&c->regalloc, reg)) {
                c->regalloc.used_callee_saved_gpr_mask |= (1u << reg);
            }
        }
    }

    if (c->dump_ir) {
        fprintf(stderr, "=== Allocated Intervals (%zu) ===\n", c->regalloc.interval_count);
        for (size_t i = 0; i < c->regalloc.interval_count; i++) {
            LiveInterval *inv = &c->regalloc.intervals[i];
            fprintf(stderr, "temp r%d (class %d): start=%zu, end=%zu -> reg %d (hi=%d) (spill=%d)\n",
                    inv->temp_id, inv->reg_class, inv->start, inv->end, inv->assigned_reg, inv->assigned_reg_hi, inv->spill_slot);
        }
    }

    /* Emit callee-save IR instructions after register allocation (Phase D: IR-based callee-save) */
    uint32_t callee_save_mask = c->regalloc.used_callee_saved_gpr_mask;
    if (callee_save_mask) {
        ir_emit_callee_save_stores(c, callee_save_mask);
        ir_emit_callee_save_loads(c, callee_save_mask);
    }

    /* Callee-save IR append (Phase D) mutated the IR: invalidate CFG cache. */
    c->cfg_version++;

    /* x86 float-compare CC remap (C12): on this host class (WSL2),
     * comisd/ucomisd sets CF/ZF but NOT SF/OF, so the signed condition
     * codes (setl/setle/setg/setge) misfire on float compares while
     * integer cmp (which sets SF) is unaffected. For ordered values the
     * CF/ZF bits encode the total order, so remap CC_LT/LE/GT/GE on the
     * COND_SET that follows an FP CMP to the CF/ZF-only variants
     * (setb/setbe/seta/setae) - the same scheme gcc uses for doubles. */
    if (c->isa == ISA_X86_64) {
        size_t n = ir_get_count(c);
        for (size_t i = 1; i < n; i++) {
            Instr *cur = ir_get(c, i);
            Instr *prev = ir_get(c, i - 1);
            if (!cur || !prev) break;
            if (cur->op == OP_COND_SET && prev->op == OP_CMP && cur->cond <= 5) {
                RegClass rc1 = regalloc_get_class_for_temp(&c->regalloc, prev->temp_src1);
                RegClass rc2 = regalloc_get_class_for_temp(&c->regalloc, prev->temp_src2);
                if (rc1 == REG_CLASS_XMM || rc1 == REG_CLASS_VEC ||
                    rc2 == REG_CLASS_XMM || rc2 == REG_CLASS_VEC) {
                    if (cur->cond == 0) { /* CC_LT: swap operands and use CC_A (8) */
                        uint32_t tmp = prev->temp_src1;
                        prev->temp_src1 = prev->temp_src2;
                        prev->temp_src2 = tmp;
                        ir_get(c, i)->cond = 8;
                    } else if (cur->cond == 1) { /* CC_LE: swap operands and use CC_AE (9) */
                        uint32_t tmp = prev->temp_src1;
                        prev->temp_src1 = prev->temp_src2;
                        prev->temp_src2 = tmp;
                        ir_get(c, i)->cond = 9;
                    } else if (cur->cond == 2) { /* CC_GT: use CC_A (8) */
                        ir_get(c, i)->cond = 8;
                    } else if (cur->cond == 3) { /* CC_GE: use CC_AE (9) */
                        ir_get(c, i)->cond = 9;
                    } else if (cur->cond == 4) { /* CC_EQ: use CC_FP_EQ (10) */
                        ir_get(c, i)->cond = 10;
                    } else if (cur->cond == 5) { /* CC_NE: use CC_FP_NE (11) */
                        ir_get(c, i)->cond = 11;
                    }
                }
            }
        }
        /* In-place IR mutation: invalidate the cached CFG (Phase 2). */
        c->cfg_version++;
    }

    live_range_free(&lra);

    /* Allocate frame spill slots for any temps that were spilled */
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
    /* Ensure spill_base_offset is at least CALLEE_SAVE_FRAME_RESERVE to avoid overlapping with callee-saved register area,
     * unless the function has no frame needs (no stack slots, no spills, and no callee-saved registers). */
    if (c->current_stack_frame == 0 && c->regalloc.next_spill_slot == 0 && c->regalloc.used_callee_saved_gpr_mask == 0) {
        c->current_stack_frame = 0;
    } else if (c->current_stack_frame < BE(c)->callee_save_frame_reserve) {
        c->current_stack_frame = BE(c)->callee_save_frame_reserve;
    }
    int32_t spill_base_offset = c->current_stack_frame;
    c->regalloc.spill_base_offset = spill_base_offset;
    if (c->regalloc.next_spill_slot > 0) {
        c->current_stack_frame += c->regalloc.next_spill_slot * 8;
    }

    /* Calculate total 16-byte aligned stack frame size */
    /* Both JIT and assembly need 16-byte alignment for proper call alignment */
    /* After push rbp (8 bytes), RSP is 0 mod 16. After sub rsp, frame_size, RSP should be 0 mod 16. */
    /* So frame_size must be 0 mod 16 (16-byte aligned). */
    /* C11 6.7.5: local alignment N > 16 needs a runtime SP realign (the
     * frame base is only 16-aligned and its mod-N residue varies per call
     * site, so an N-aligned slot in an N-rounded frame is not actually
     * N-aligned). The frame is rounded to N and anchored to a realigned
     * base SP'; every slot stays delta-based with frame_delta = frame,
     * so slot k lands at [SP + frame - k] = [SP' - k]. The realign is
     * emitted per-ISA in the prologue:
     *   x86-64   : push rbp; mov rbp, rsp; and $-N, rsp; sub $frame, rsp
     *              (epilogue: leave - rbp = entry-SP-8 restored exactly)
     *   aarch64  : stp x29,x30,[sp,#-16]!; mov x29, sp; then
     *              mov x16, sp; and x16, x16, #mask; sub x16, x29, x16;
     *              sub sp, sp, x16; sub sp, sp, #frame
     *              (SP cannot be the destination of a logical op - the
     *              mask goes through x16 = IP0; epilogue: mov sp, x29;
     *              ldp x29, x30, [sp], #16 - the plain FP-mode leave)
     *   riscv    : addi sp, sp, -16; sd/w ra, 8(sp); sd/w s0, 0(sp);
     *              mv s0, sp; andi sp, sp, mask; sub sp, sp, frame
     *              (andi's 12-bit immediate caps N at 4096; epilogue:
     *              the plain FP-mode leave restores sp from the s0 value)
     * Eligible: power-of-two N <= 4096, no VLA / variadic / frame-address
     * (they assume the plain frame geometry); otherwise the local is
     * capped at 16 with a warning (never over-claimed). */
    size_t max_local_align = 0;
    for (size_t i = 0; i < c->local_count; i++) {
        if ((size_t)c->locals[i].type.align > max_local_align)
            max_local_align = (size_t)c->locals[i].type.align;
    }
    bool align_realign = false;
    if (max_local_align > 16 &&
        (max_local_align & (max_local_align - 1)) == 0 &&
        max_local_align <= 4096) {
        bool vla_local = false;
        for (size_t i = 0; i < c->local_count; i++) {
            if (c->locals[i].is_vla) { vla_local = true; break; }
        }
        bool va_fn = (lowering_fn != NULL) && lowering_fn->is_variadic;
        if (!vla_local && !va_fn && !c->lowering.has_frame_address) {
            align_realign = true;
        } else {
            diag_report_warning_at_token(c,
                "local _Alignas(%zu) cannot be guaranteed in this function "
                "(VLA/variadic/frame-address); using alignment 16",
                (unsigned long)max_local_align);
        }
    } else if (max_local_align > 16) {
        diag_report_warning_at_token(c,
            "local _Alignas(%zu) above 4096 or not a power of two; "
            "using alignment 16",
            (unsigned long)max_local_align);
    }
    size_t total_frame_size = (size_t)((c->current_stack_frame + 15) & ~15);
    if (align_realign) {
        size_t N = max_local_align;
        total_frame_size = (c->current_stack_frame + N - 1) & ~(N - 1);
        c->lowering.frame_align = (int32_t)N;
    }

    /* Determine if function is a leaf (no calls, no VLAs). With
     * -fsanitize-trap, an OP_OVF_TRAP site emits a runtime call to
     * __zerocsan_report (inside the emitter, not as an OP_CALL), so such
     * a function must not be treated as leaf/frameless: the call needs
     * 16-byte stack alignment and clobbers the red zone. */
    bool is_leaf = true;
    size_t ir_count = ir_get_count(c);
    const Instr *ir_buf = ir_get_buffer(c);
    for (size_t i = 0; i < ir_count; i++) {
        if (ir_buf[i].op == OP_CALL || ir_buf[i].op == OP_CALL_INDIRECT ||
            ir_buf[i].op == OP_VLA_ALLOC || ir_buf[i].op == OP_SYSCALL ||
            ir_buf[i].op == OP_CLONE ||
            ir_buf[i].op == OP_SETJMP || ir_buf[i].op == OP_LONGJMP ||
            (ir_buf[i].op == OP_OVF_TRAP && c->sanitize_report)) {
            is_leaf = false;
            break;
        }
    }

    /* Option 1: Leaf functions with no stack needs are frameless */
    c->lowering.is_frameless = (!c->lowering.has_stack_protector &&
                                !c->debug_info &&
                                is_leaf && total_frame_size == 0 &&
                                c->regalloc.used_callee_saved_gpr_mask == 0 &&
                                c->lowering.static_local_rt_off == 0 &&
                                !c->lowering.has_frame_address);

    /* Option 3: x86-64 System V Red Zone optimization:
     * Leaf functions with <= 128 bytes can use the 128-byte Red Zone below %rsp directly
     * without adjusting %rsp. */
    size_t alloc_frame_size = total_frame_size;
    if (c->isa == ISA_X86_64 && !c->lowering.has_stack_protector && !c->debug_info && is_leaf && total_frame_size <= 128 && !c->lowering.has_frame_address &&
        !align_realign) {
        alloc_frame_size = 0;
    }

    /* Option 4: Frame pointer omission for fixed-frame functions.
     * The frame pointer (%rbp / x29 / s0) is a constant anchor for every
     * frame slot. For a function whose frame is fully static (no runtime
     * VLA, which lowers SP dynamically on some targets) the same anchor
     * role is served by SP plus a per-function constant delta, so the
     * prologue/epilogue drop the FP setup/teardown and every slot offset
     * is rebased: disp_SP = disp_FP + (FP - final_SP).
     *
     * Per-ISA geometry (E = entry SP, alloc = alloc_frame_size, aligned =
     * 16-round-up(alloc)):
     *   x86-64  : FP = E-8 (push rbp); the prologue subtracts X = alloc+8
     *             (alloc==0: X = 8 to keep 16-byte call alignment; red-zone
     *             leaf: X = 0). delta = X - 8.
     *   aarch64 : FP = E-16 (stp x29,x30); X = aligned+16. delta = aligned.
     *   riscv   : FP = E-16 (s0 = sp+aligned); X = aligned+16. delta = aligned.
     * RISC-V additionally requires aligned+16 <= 2047 so the FP-less
     * epilogue (ld ra, aligned+8(sp); addi sp, sp, aligned+16) fits in
     * immediates; larger frames keep the FP.
     * Policy: on by default at -O1 and above (like gcc's -fomit-frame-
     * pointer); force with -fomit-frame-pointer / -fno-omit-frame-pointer. */
    c->lowering.omit_frame_pointer = false;
    c->lowering.frame_delta = 0;
    {
        bool omit_wanted = c->omit_frame_pointer == 1 ||
                           (c->omit_frame_pointer < 0 && c->opt_level >= OPT_LEVEL_O1 && !c->debug_info);
        /* Disable frame pointer omission for variadic functions:
         * va_start offset computation assumes a stable frame pointer. */
        bool is_variadic_fn = (lowering_fn != NULL) && lowering_fn->is_variadic;
        if (align_realign) {
            /* C11 _Alignas(N>16) realign mode: the frame is anchored to the
             * realigned SP', so slots are SP-relative with delta = frame
             * (slot k at [SP' - k]). rbp is still set up by the prologue
             * (push rbp / mov rbp, rsp) - `leave` restores entry state -
             * so this is FP-omission geometry with a realign prefix. */
            c->lowering.omit_frame_pointer = true;
            c->lowering.frame_delta = (int32_t)total_frame_size;
        } else if (omit_wanted && !c->lowering.is_frameless && !is_variadic_fn && !c->lowering.has_frame_address) {
            bool has_vla = false;
            for (size_t i = 0; i < ir_count; i++) {
                if (ir_buf[i].op == OP_VLA_ALLOC) { has_vla = true; break; }
            }
            if (!has_vla) {
                size_t aligned = (alloc_frame_size + 15) & ~(size_t)15;
                if (c->isa == ISA_X86_64) {
                    size_t X = (alloc_frame_size == 0 && is_leaf) ? 0 : alloc_frame_size + 8;
                    c->lowering.frame_delta = (int32_t)X - 8;
                    c->lowering.omit_frame_pointer = true;
                } else {
                    c->lowering.frame_delta = (int32_t)aligned;
                    bool fits = (c->isa != ISA_RISCV64 && c->isa != ISA_RISCV32) ||
                                (aligned + 16 <= 2047);
                    if (fits) {
                        c->lowering.omit_frame_pointer = true;
                    } else {
                        c->lowering.frame_delta = 0;
                    }
                }
            }
        }
    }

    /* Sprint 99: persist this function's user label name -> IR label
     * mapping into the fn record, so a later-compiled nested function can
     * emit OP_NONLOCAL_GOTO to one of them (the label_patcher state is
     * per-lowering and would be clobbered by the next function). Runs for
     * both the MC and -S emission paths. */
    {
        ZeroCCFunction *rec = &c->functions[c->lowering.current_func_idx];
        for (int li = 0; li < rec->nl_label_count; li++) {
            for (int ul = 0; ul < c->label_patcher.user_label_count; ul++) {
                if (strcmp(c->label_patcher.user_labels[ul].name,
                           rec->nl_labels[li]) == 0) {
                    rec->nl_label_ir[li] = c->label_patcher.user_labels[ul].ir_label;
                    break;
                }
            }
        }
    }

    /* Emit machine code or assembly from IR with allocated registers */
    if (c->emit_asm) {
        /* For assembly: emit function label, then prologue, then IR (param saves in IR) */
        emit_function_label_asm(c);
        emit_prologue_asm(c, alloc_frame_size);
        emit_ir_to_asm(c, spill_base_offset);
    } else {
        /* For machine code: emit prologue first, then IR (param saves in IR) */
        if (c->debug_info && lowering_fn && lowering_fn->line > 0) {
            zero_cc_dwarf_add_line(c, c->text_section.length, lowering_fn->line, lowering_fn->col, true);
        }
        emit_prologue_mc(c, alloc_frame_size);
        if (c->debug_info) {
            zero_cc_dwarf_mark_prologue_end(c);
        }
        emit_ir_to_machine_code(c, spill_base_offset);
    }


    /* Emit return jumps after IR-to-MC */
    emit_return_jumps(c);

    /* Resolve all label patches */
    if (BE(c)->resolve_labels) {
        BE(c)->resolve_labels(c);
    }

    /* Callee-saved register loads are now handled by OP_CALLEE_SAVE_LOADS IR instruction */

    /* Emit epilogue */
    if (!c->emit_asm && c->lowering.has_stack_protector && BE(c)->emit_stack_guard_check) {
        BE(c)->emit_stack_guard_check(c, c->lowering.canary_stack_off);
    }
    BE(c)->emit_leave(c);
    BE(c)->emit_ret(c);
    regalloc_destroy(&c->regalloc);

    /* Debug: Dump IR if requested */
    if (c->dump_ir) {
        fprintf(stderr, "DEBUG: dump_ir flag is SET, function_count=%zu\n", c->function_count);
        if (lowering_fn) {
            fprintf(stderr, "=== IR for function '%s' ===\n", lowering_fn->name);
            ir_dump(c, stderr);
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "DEBUG: No functions found!\n");
        }
    }

    /* Debug: Dump CFG if requested (Sprint 35 Phase 2) */
    if (c->dump_cfg) {
        cfg_dump(c, stderr);
        fprintf(stderr, "\n");
    }

    /* Clear IR buffer for next function */
    ir_clear(c);

    /* Resolve kind-5 and kind-6 data relocations (static label tables: &&label and &&l1 - &&l2) for this function */
    for (size_t r = 0; r < c->data_reloc_count; r++) {
        ZeroCCDataReloc *dr = &c->data_relocs[r];
        if (dr->target_kind == 5 && (size_t)dr->target_global_idx == current_fn_idx) {
            for (int u = 0; u < c->label_patcher.user_label_count; u++) {
                if (strcmp(dr->target_extern_name, c->label_patcher.user_labels[u].name) == 0) {
                    uint16_t ir_lbl = c->label_patcher.user_labels[u].ir_label;
                    size_t text_off = 0;
                    for (size_t lo = 0; lo < c->user_label_offset_count; lo++) {
                        if (c->user_label_offsets[lo].func_idx == current_fn_idx &&
                            c->user_label_offsets[lo].ir_label == ir_lbl) {
                            text_off = c->user_label_offsets[lo].text_offset;
                            break;
                        }
                    }
                    dr->target_data_offset = text_off;
                    snprintf(dr->target_extern_name, sizeof(dr->target_extern_name),
                             ".L%zu_%u", current_fn_idx, (unsigned)ir_lbl);
                    break;
                }
            }
        } else if (dr->target_kind == 6 && (size_t)dr->target_global_idx == current_fn_idx) {
            char name1[64] = {0}, name2[64] = {0};
            if (sscanf(dr->target_extern_name, "%63s - %63s", name1, name2) == 2) {
                size_t off1 = 0, off2 = 0;
                uint16_t ir_lbl1 = 0, ir_lbl2 = 0;
                for (int u = 0; u < c->label_patcher.user_label_count; u++) {
                    if (strcmp(name1, c->label_patcher.user_labels[u].name) == 0) {
                        ir_lbl1 = c->label_patcher.user_labels[u].ir_label;
                        for (size_t lo = 0; lo < c->user_label_offset_count; lo++) {
                            if (c->user_label_offsets[lo].func_idx == current_fn_idx &&
                                c->user_label_offsets[lo].ir_label == ir_lbl1) {
                                off1 = c->user_label_offsets[lo].text_offset;
                                break;
                            }
                        }
                    }
                    if (strcmp(name2, c->label_patcher.user_labels[u].name) == 0) {
                        ir_lbl2 = c->label_patcher.user_labels[u].ir_label;
                        for (size_t lo = 0; lo < c->user_label_offset_count; lo++) {
                            if (c->user_label_offsets[lo].func_idx == current_fn_idx &&
                                c->user_label_offsets[lo].ir_label == ir_lbl2) {
                                off2 = c->user_label_offsets[lo].text_offset;
                                break;
                            }
                        }
                    }
                }
                int64_t diff = (int64_t)off1 - (int64_t)off2;
                dr->target_data_offset = (size_t)diff;
                snprintf(dr->target_extern_name, sizeof(dr->target_extern_name),
                         ".L%zu_%u - .L%zu_%u", current_fn_idx, (unsigned)ir_lbl1,
                         current_fn_idx, (unsigned)ir_lbl2);
                size_t patch_sz = dr->rodata_offset ? dr->rodata_offset : 4;
                if (dr->data_patch_offset + patch_sz <= c->data_section.length) {
                    if (patch_sz == 8) {
                        memcpy(c->data_section.data + dr->data_patch_offset, &diff, 8);
                    } else {
                        int32_t diff32 = (int32_t)diff;
                        memcpy(c->data_section.data + dr->data_patch_offset, &diff32, 4);
                    }
                }
            }
        }
    }

    /* text_offset was already set in compile.h BEFORE the prologue was emitted,
     * so fn->text_offset correctly points to the function's first byte. */
}
