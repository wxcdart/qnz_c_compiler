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
#include "zero_cc/types/type_funcs.h"
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

/* C99 6.3.1.5: integer -> floating conversion. cvtsi2sd reads 64-bit
 * *unsigned* sources (unsigned long / pointers) as signed, so values with
 * the high bit set come out as (v - 2^64); the u64-aware path computes
 * cvtsi2sd((v>>1)|(v&1)) * 2.0 instead. Returns the fresh FP temp and sets
 * c->last_expr_is_float. If the source is not a 64-bit unsigned, this is a
 * plain cvtsi2sd. */
int zc_emit_int_to_double(ZeroCCCompiler *c, int src_temp, const TypeInfo *src_ti);

/* Rodata offset of a double literal (8-byte aligned), registered on demand. */
int zc_rodata_double_offset(ZeroCCCompiler *c, double val);

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

/* Sprint 72: a static local whose ADDRESS is materialized in this function
 * (sym->static_addr_taken, set by the pre-scan) must be reached through its
 * stable data-section object, not its frame-relative mirror slot - the slot
 * is dead once the function returns and the epilogue mirror is skipped for
 * such statics. Value-only statics (and every non-static local) keep the
 * frame slot, so this is a no-op for the common case. */
static inline int zc_sym_uses_data_section(const ZeroCCSym *sym) {
    return sym && sym->is_static_local && sym->static_addr_taken;
}

/* Materialize the address of a local symbol (base + `extra` bytes) in a
 * fresh temp.  Uses the data-section object for address-taken statics and
 * the frame-relative slot otherwise. `extra` must be a compile-time
 * constant (runtime offsets are handled by the caller). */
static inline int zc_sym_addr(ZeroCCCompiler *c, const ZeroCCSym *sym, int32_t extra) {
    int a = c->next_temp_id++;
    if (zc_sym_uses_data_section(sym)) {
        ir_emit_mov_global(c, (uint16_t)a, sym->static_global_name);
        if (extra != 0) {
            int o = c->next_temp_id++;
            ir_emit_mov_imm(c, (uint16_t)o, (int64_t)extra);
            int na = c->next_temp_id++;
            ir_emit_add(c, (uint16_t)na, (uint16_t)a, (uint16_t)o);
            a = na;
        }
    } else {
        ir_emit_lea(c, a, BE(c)->fp_reg, -sym->stack_offset + extra);
    }
    return a;
}

/* Load the 8 bytes at a local symbol's storage (+ `extra`) into `dest`.
 * Data-section object for address-taken statics, frame slot otherwise. */
static inline void zc_sym_load(ZeroCCCompiler *c, const ZeroCCSym *sym, int32_t extra, int dest) {
    if (zc_sym_uses_data_section(sym)) {
        int a = zc_sym_addr(c, sym, extra);
        ir_emit_deref(c, dest, a);
    } else {
        ir_emit_load(c, dest, BE(c)->fp_reg, -sym->stack_offset + extra);
    }
}

/* Store the 8 bytes in `val` at a local symbol's storage (+ `extra`). */
static inline void zc_sym_store(ZeroCCCompiler *c, const ZeroCCSym *sym, int32_t extra, int val) {
    if (zc_sym_uses_data_section(sym)) {
        int a = zc_sym_addr(c, sym, extra);
        ir_emit_store_64(c, (uint16_t)val, (uint16_t)a);
    } else {
        ir_emit_store(c, val, BE(c)->fp_reg, -sym->stack_offset + extra);
    }
}

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
    if (sign) ir_emit_sext(c, temp, w);
    else ir_emit_zext(c, temp, w);
}

/* Find type of a local or global identifier */
static inline const ZeroCCType *find_ident_zero_type(ZeroCCCompiler *c, const char *name) {
    ZeroCCSym *sym = zero_cc_find_local(c, name);
    if (sym) return &sym->type;
    ZeroCCGlobalVar *gv = find_global_var(c, name);
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
        if (type_is_struct_or_union(ti) && type_size(ti) > 16) return true;
    } else if (node->tag == NODE_COMPOUND_LIT) {
        TypeInfo *ti = ast_node_get_typeinfo(c, node);
        if (type_is_struct_or_union(ti) && type_size(ti) > 16) return true;
    } else if (node->tag == NODE_UNOP && (node->aux.op == '*' || node->aux.op == TK_STAR)) {
        TypeInfo *ti = ast_node_get_typeinfo(c, node);
        if (type_is_struct_or_union(ti) && type_size(ti) > 16) return true;
        ZeroCCASTNode *in = zero_cc_ast_get(arena, node->left);
        if (in && in->tag == NODE_IDENT) {
            ZeroCCSym *sym = zero_cc_find_local(c, in->s.str);
            if (sym && sym->type.base && zero_cc_type_is_struct_or_union(sym->type.base) && sym->type.base->size > 16) return true;
            ZeroCCGlobalVar *gv = find_global_var(c, in->s.str);
            if (gv && gv->type.base && zero_cc_type_is_struct_or_union(gv->type.base) && gv->type.base->size > 16) return true;
        }
    } else if (node->tag == NODE_INDEX) {
        TypeInfo *ti = ast_node_get_typeinfo(c, node);
        if (type_is_struct_or_union(ti)) return true;
        ZeroCCASTNode *ln = zero_cc_ast_get(arena, node->left);
        if (ln && ln->tag == NODE_IDENT) {
            ZeroCCSym *sym = zero_cc_find_local(c, ln->s.str);
            if (sym && sym->type.base && zero_cc_type_is_struct_or_union(sym->type.base)) return true;
        }
    } else if (node->tag == NODE_IDENT) {
        if (!zero_cc_find_local(c, node->s.str)) {
            ZeroCCGlobalVar *gv = find_global_var(c, node->s.str);
            if (gv && zero_cc_type_is_struct_or_union(&gv->type)) return true;
        }
    } else if (node->tag == NODE_MEMBER) {
        TypeInfo *ti = ast_node_get_typeinfo(c, node);
        if (type_is_struct_or_union(ti)) return true;
    } else if (node->tag == NODE_VA_ARG) {
        /* va_arg(ap, struct S) with sizeof(S) > 16 yields the hidden
         * pointer to the caller's variadic copy (SysV ABI) - an address. */
        TypeInfo *ti = ast_node_get_typeinfo(c, node);
        if (type_is_struct_or_union(ti) && type_size(ti) > 16) return true;
    }
    return false;
}

/* Copy n bytes from src (address temp) to dst (address temp) with a real
 * IR loop of 8-byte chunks. A straight-line unroll would emit 4+ insns per
 * chunk - a 128KB by-value struct argument (c-torture execute/pr23135) is
 * 16K chunks and would blow both the JIT compile time and the 16-bit
 * per-function temp budget. A fixed set of temps is reused per iteration. */
/* Store the LOW `rem` (1..7) bytes of the 64-bit value `val` starting at
 * address temp `base_addr`.  rem 1,2,4 are a single narrow store; rem 3,5,6,7
 * spill the value to a scratch frame slot and copy byte-by-byte -
 * extracting a byte at offset >= 2 needs a shift temp, whose allocation
 * collides with live values in large functions and silently drops the byte
 * (c-torture execute/va-arg-22: an 11-byte struct's 3rd tail byte was
 * stored twice as the first). Shared by the struct-copy / 9-16B
 * secondary-register store paths in ast_lowering_expr.c and
 * ast_lowering_stmt.c. */
static inline void zc_store_low_bytes(ZeroCCCompiler *c, int val, int base_addr, size_t rem) {
    if (rem <= 2 || rem == 4) {
        if (rem == 4) ir_emit_store_32(c, val, base_addr);
        else if (rem == 2) ir_emit_store_16(c, val, base_addr);
        else ir_emit_store_byte(c, val, base_addr);
        return;
    }
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
    c->current_stack_frame += 8;
    int32_t scratch = c->current_stack_frame;
    ir_emit_store(c, val, BE(c)->fp_reg, -scratch);
    int sa = c->next_temp_id++;
    ir_emit_lea(c, sa, BE(c)->fp_reg, -scratch);
    for (size_t b = 0; b < rem; b++) {
        int bv = c->next_temp_id++;
        int saddr = sa;
        int da = base_addr;
        if (b) {
            int t = c->next_temp_id++;
            ir_emit_mov_imm(c, t, (int)b);
            saddr = c->next_temp_id++;
            ir_emit_add(c, saddr, sa, t);
            int t2 = c->next_temp_id++;
            ir_emit_mov_imm(c, t2, (int)b);
            da = c->next_temp_id++;
            ir_emit_add(c, da, base_addr, t2);
        }
        ir_emit_load_byte(c, bv, saddr);
        ir_emit_store_byte(c, bv, da);
    }
}

static void zc_emit_mem_copy(ZeroCCCompiler *c, int src, int dst, size_t n) {
    if (n == 0) return;
    size_t full = n / 8;
    size_t tail = n % 8;
    int s = c->next_temp_id++;
    int d = c->next_temp_id++;
    int v = c->next_temp_id++;
    int eight = c->next_temp_id++;
    ir_emit_mov_reg(c, s, src);
    ir_emit_mov_reg(c, d, dst);
    ir_emit_mov_imm(c, eight, 8);
    if (full > 0) {
        int cnt = c->next_temp_id++;
        int one = c->next_temp_id++;
        ir_emit_mov_imm(c, cnt, (int32_t)full);
        ir_emit_mov_imm(c, one, 1);
        uint16_t loop = c->ir_next_label++;
        ir_emit_label(c, loop);
        ir_emit_deref(c, v, s);
        ir_emit_store_64(c, v, d);
        ir_emit_add(c, s, s, eight);
        ir_emit_add(c, d, d, eight);
        ir_emit_sub(c, cnt, cnt, one);
        ir_emit_jne(c, cnt, loop);
    }
    if (tail > 0) {
        /* Tail: copy ALL `tail` bytes, decomposed into 4+2+1 units -
         * a single 4/2/1 unit drops the bytes beyond its width (a
         * 35-byte struct lost its 35th byte; c-torture
         * execute/va-arg-22). */
        size_t rem = tail;
        if (rem >= 4) {
            ir_emit_load_32(c, v, s);
            ir_emit_store_32(c, v, d);
            int o4 = c->next_temp_id++;
            ir_emit_mov_imm(c, o4, 4);
            ir_emit_add(c, s, s, o4);
            ir_emit_add(c, d, d, o4);
            rem -= 4;
        }
        if (rem >= 2) {
            ir_emit_load_16(c, v, s);
            ir_emit_store_16(c, v, d);
            int o2 = c->next_temp_id++;
            ir_emit_mov_imm(c, o2, 2);
            ir_emit_add(c, s, s, o2);
            ir_emit_add(c, d, d, o2);
            rem -= 2;
        }
        if (rem >= 1) {
            ir_emit_load_byte(c, v, s);
            ir_emit_store_byte(c, v, d);
        }
    }
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

static inline void ir_emit_global_addr(ZeroCCCompiler *c, uint16_t temp, const ZeroCCGlobalVar *gv) {
    if (!gv) return;
    if (gv->is_tls && !c->jit_mode) ir_emit_tls_lea(c, temp, gv->name);
    else ir_emit_mov_global(c, temp, gv->name);
}

static inline void lower_extend_narrow(ZeroCCCompiler *c, int temp, const TypeInfo *ti) {
    (void)c;
    if (!ti) return;
    if (ti->kind != TYPE_KIND_BOOL && ti->kind != TYPE_KIND_CHAR &&
        ti->kind != TYPE_KIND_SHORT && ti->kind != TYPE_KIND_INT &&
        ti->kind != TYPE_KIND_ENUM) return; /* enums are signed ints */
    size_t sz = type_size(ti);
    if (sz != 1 && sz != 2 && sz != 4) return;
    if (type_is_unsigned(ti)) ir_emit_zext(c, temp, (int)sz);
    else ir_emit_sext(c, temp, (int)sz);
}

static inline void lower_assign_conv_zt(ZeroCCCompiler *c, int temp, const ZeroCCType *zt) {
    (void)c;
    if (!zt) return;
    if (zt->tag == TYPE_STRUCT || zt->tag == TYPE_UNION) return;
    if (zt->tag == TYPE_USER_0 && zero_cc_type_is_vector(zt)) return;
    if (zt->size != 1 && zt->size != 2 && zt->size != 4) return;
    if (zt->tag == TYPE_UINT8 || zt->tag == TYPE_UINT16 || zt->tag == TYPE_UINT32)
        ir_emit_zext(c, temp, (int)zt->size);
    else
        ir_emit_sext(c, temp, (int)zt->size);
}

static inline int lvalue_inc_step(const ZeroCCType *t) {
    if (!t) return 1;
    if (t->tag == TYPE_PTR) {
        if (t->base && t->base->size > 0) return (int)t->base->size;
        return 8;
    }
    return 1;
}

static inline bool zc_is_array_base(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *innermost, ZeroCCASTNode *base_node) {
    if (!innermost || !(innermost->flags & NODE_MEMBER_IS_ARROW)) return false;
    ZeroCCASTNode *bl = zero_cc_ast_get(arena, innermost->left);
    if (bl) {
        TypeInfo *ti = ast_node_get_typeinfo(c, bl);
        if (ti && ti->kind == TYPE_KIND_ARRAY) return true;
    }
    if (base_node && base_node->tag == NODE_IDENT) {
        ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
        if (sym && !sym->is_extern && sym->type.tag == TYPE_ARRAY) return true;
        ZeroCCGlobalVar *gv = find_global_var(c, base_node->s.str);
        if (gv && gv->type.tag == TYPE_ARRAY) return true;
    }
    return false;
}

int lower_member_chain_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *head);
int zero_cc_expr_int_info(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref, int *is_unsigned);
bool zero_cc_binop_is_unsigned(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef left_ref, ASTRef right_ref);

int zero_cc_lower_lvalue_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
void zero_cc_emit_struct_copy_to_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                      int dst_addr, size_t struct_sz,
                                      ASTRef rhs_ref, int rhs_temp);

bool lower_expr_builtin(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                        ZeroCCASTNode *node, const char *name,
                        int *out_temp);
int lower_expr_call(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node);
int lower_expr_va_start(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node);
int lower_expr_va_arg(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node);
int lower_expr_assign(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node);
int lower_expr_binop(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node);
int lower_expr_unop(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node);
int lower_expr_cast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node);

#endif /* ZERO_CC_PARSER_AST_LOWERING_INTERNAL_H */
