/* AST Lowering - Internal Declarations
 *
 * Private internal header for AST lowering module.
 * Contains forward declarations and shared types.
 */

#ifndef ZERO_CC_PARSER_AST_LOWERING_INTERNAL_H
#define ZERO_CC_PARSER_AST_LOWERING_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "zero_cc/types/types.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/ir/ir_emit_ops.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/types/emit_helpers.h"
#include "zero_cc/types/vregs.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/common/arena.h"
#include "zero_cc/jit/type_info.h"
#include "zero_cc/ir/abi_classify.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/ir/live_range.h"
#include "zero_cc/ir/lowering_state.h"
#include "zero_cc/ir/label_patcher.h"

/* Forward declarations */
void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
void emit_ir_to_machine_code(ZeroCCCompiler *c, int32_t spill_base_offset);

/* Resolve a local variable's symbol using the index stashed at parse time
 * (node->aux.step = index into c->locals + 1, 0 = not stashed), falling
 * back to a by-name lookup.  The by-name table is latest-wins: with sibling
 * blocks reusing a name it resolves the shadowing symbol, which is wrong
 * for type-dependent initializers (aggregate/string initializers must see
 * the symbol that owned the declaration they belong to). */
static inline ZeroCCSym *lowering_local_sym(ZeroCCCompiler *c, const ZeroCCASTNode *node,
                                            const char *name) {
    if (node && node->aux.step != 0) {
        size_t i = (size_t)(node->aux.step - 1);
        if (i < c->local_count) return &c->locals[i];
    }
    return name ? zero_cc_find_local(c, name) : NULL;
}

/* Emit function prologue directly (without IR instruction) */
void emit_prologue_mc(ZeroCCCompiler *c, size_t frame_size);
void emit_return_jumps(ZeroCCCompiler *c);

/* C99 narrow-integer width semantics. The machine word is 64 bits, so
 * 1/2/4-byte integer values must be explicitly extended to full width at
 * every load/use boundary, otherwise overflowed `int` results keep 64-bit
 * residue and compare/store wrong. Returns 1/2/4 for narrow int scalars,
 * 0 otherwise; *sign gets 1 for signed, 0 for unsigned. */
static inline int zero_cc_narrow_int_width(const ZeroCCType *t, int *sign) {
    if (!t) return 0;
    switch (t->tag) {
        case TYPE_BOOL:   *sign = 0; return 1;
        case TYPE_INT8:   *sign = 1; return 1;
        case TYPE_UINT8:  *sign = 0; return 1;
        case TYPE_INT16:  *sign = 1; return 2;
        case TYPE_UINT16: *sign = 0; return 2;
        case TYPE_INT32:  *sign = 1; return 4;
        case TYPE_UINT32: *sign = 0; return 4;
        case TYPE_ENUM:   *sign = 1; return 4;
        default: return 0;
    }
}

/* Emit in-place sext/zext for a narrow int temp (no-op for 8-byte/pointer types). */
static inline void zero_cc_emit_int_extend(ZeroCCCompiler *c, int temp, const ZeroCCType *t) {
    (void)c;
    int sign = 0;
    int w = zero_cc_narrow_int_width(t, &sign);
    if (w == 0) return;
    if (sign) ir_emit_sext(temp, w);
    else ir_emit_zext(temp, w);
}

/* Find type of a local or global identifier */
static inline const ZeroCCType *find_ident_zero_type(ZeroCCCompiler *c, const char *name) {
    ZeroCCSym *sym = zero_cc_find_local(c, name);
    if (sym) return &sym->type;
    ZeroCCGlobalVar *gv = find_global_var(name);
    if (gv) return &gv->type;
    return NULL;
}

/* Helper: Get function currently being lowered */
static inline ZeroCCFunction *zero_cc_get_current_fn(ZeroCCCompiler *c) {
    if (c->function_count > 0 && c->lowering.current_func_idx < c->function_count) {
        return &c->functions[c->lowering.current_func_idx];
    }
    return NULL;
}

/* Helper: Get current function's return type */
static inline ZeroCCType zero_cc_get_current_fn_ret_type(ZeroCCCompiler *c) {
    ZeroCCFunction *fn = zero_cc_get_current_fn(c);
    if (fn) {
        return fn->return_type;
    }
    return zero_cc_type_int();
}

/* Check if an AST reference evaluates to a struct/union in memory (an address temp) */
static inline bool ast_expr_is_struct_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return false;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return false;
    if (node->tag == NODE_CALL) {
        TypeInfo *ti = ast_node_get_typeinfo(c, node);
        if (ti && (ti->kind == TYPE_KIND_STRUCT || ti->kind == TYPE_KIND_UNION) && type_size(ti) > 16) return true;
    } else if (node->tag == NODE_COMPOUND_LIT) {
        TypeInfo *ti = ast_node_get_typeinfo(c, node);
        if (ti && (ti->kind == TYPE_KIND_STRUCT || ti->kind == TYPE_KIND_UNION) && type_size(ti) > 16) return true;
    } else if (node->tag == NODE_UNOP && (node->aux.op == '*' || node->aux.op == TK_STAR)) {
        TypeInfo *ti = ast_node_get_typeinfo(c, node);
        if (ti && (ti->kind == TYPE_KIND_STRUCT || ti->kind == TYPE_KIND_UNION) && type_size(ti) > 16) return true;
        ZeroCCASTNode *in = zero_cc_ast_get(arena, node->left);
        if (in && in->tag == NODE_IDENT) {
            ZeroCCSym *sym = zero_cc_find_local(c, in->s.str);
            if (sym && sym->type.base && (sym->type.base->tag == TYPE_STRUCT || sym->type.base->tag == TYPE_UNION) && sym->type.base->size > 16) return true;
            ZeroCCGlobalVar *gv = find_global_var(in->s.str);
            if (gv && gv->type.base && (gv->type.base->tag == TYPE_STRUCT || gv->type.base->tag == TYPE_UNION) && gv->type.base->size > 16) return true;
        }
    } else if (node->tag == NODE_INDEX) {
        TypeInfo *ti = ast_node_get_typeinfo(c, node);
        if (ti && (ti->kind == TYPE_KIND_STRUCT || ti->kind == TYPE_KIND_UNION)) return true;
        ZeroCCASTNode *ln = zero_cc_ast_get(arena, node->left);
        if (ln && ln->tag == NODE_IDENT) {
            ZeroCCSym *sym = zero_cc_find_local(c, ln->s.str);
            if (sym && sym->type.base && (sym->type.base->tag == TYPE_STRUCT || sym->type.base->tag == TYPE_UNION)) return true;
        }
    } else if (node->tag == NODE_IDENT) {
        if (!zero_cc_find_local(c, node->s.str)) {
            ZeroCCGlobalVar *gv = find_global_var(node->s.str);
            if (gv && (gv->type.tag == TYPE_STRUCT || gv->type.tag == TYPE_UNION)) return true;
        }
    } else if (node->tag == NODE_MEMBER) {
        TypeInfo *ti = ast_node_get_typeinfo(c, node);
        if (ti && (ti->kind == TYPE_KIND_STRUCT || ti->kind == TYPE_KIND_UNION)) return true;
    }
    return false;
}

/* Check if an AST reference evaluates to a GNU vector object in memory
 * (an address temp). Vector-typed expressions lower to the vector's
 * address: binops/unops produce a per-lane stack temp, idents decay
 * (LEA), compound literals initialize in place. By-value vector call
 * results are materialized into an aligned stack temp (9-16B from the
 * VREG_0/VREG_2 pair, <=8B from VREG_0) and >16B (sret) results are the
 * hidden buffer address - so a NODE_CALL always yields an address. */
static inline bool ast_expr_is_vector_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return false;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return false;
    TypeInfo *ti = ast_node_get_typeinfo(c, node);
    if (!ti || !type_is_vector(ti)) return false;
    return true;
}

int zero_cc_lower_lvalue_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
void zero_cc_emit_struct_copy_to_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                      int dst_addr, size_t struct_sz,
                                      ASTRef rhs_ref, int rhs_temp);

#endif /* ZERO_CC_PARSER_AST_LOWERING_INTERNAL_H */
