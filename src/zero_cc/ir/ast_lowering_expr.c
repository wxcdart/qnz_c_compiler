/* AST Lowering - Expression Lowering (AST to IR) */

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/isa/isa_backend.h"

/* Address of a global variable into a temp. TLS statics live in the
 * program's TLS block (AOT x86-64: .tdata/.tbss, not .data), so they need
 * OP_TLS_LEA (R_X86_64_TPOFF64). In JIT mode the TLS block is the data
 * section and TLS variables are plain globals, so OP_MOV_GLOBAL works for
 * both. Subsequent loads/stores use the ordinary width-aware ops. */
static inline void ir_emit_global_addr(ZeroCCCompiler *c, uint16_t temp, const ZeroCCGlobalVar *gv) {
    if (gv->is_tls && !c->jit_mode) ir_emit_tls_lea(temp, gv->name);
    else ir_emit_mov_global(temp, gv->name);
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
            ZeroCCGlobalVar *gv = find_global_var(n->s.str);
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
static int zero_cc_expr_int_info(ZeroCCCompiler *c, ZeroCCASTArena *arena,
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
                ZeroCCGlobalVar *gv = find_global_var(n->s.str);
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
                    ZeroCCGlobalVar *gv = find_global_var(base->s.str);
                    if (gv) zt = &gv->type;
                }
                if (zt && zt->base) return zero_cc_ztype_int_info(zt->base, is_unsigned);
            }
            return 4;
        }
        case NODE_UNOP:
            if (n->aux.op == TK_BANG) { *is_unsigned = 0; return 4; }
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

static bool zero_cc_binop_is_unsigned(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                      ASTRef left_ref, ASTRef right_ref) {
    int lu = 0, ru = 0;
    int lw = zero_cc_expr_int_info(c, arena, left_ref, &lu);
    int rw = zero_cc_expr_int_info(c, arena, right_ref, &ru);
    if (lw > rw) return lu != 0;
    if (rw > lw) return ru != 0;
    return (lu || ru);
}

/* Compute the address of the final member of a multi-segment member
 * chain (o.p->v, a.b.c, p->a->b, ...) - C5. The flat "sum all offsets"
 * model cannot dereference intermediate pointer members, so the segments
 * are processed innermost-first: a dot segment adds its offset to the
 * current address; an arrow segment first loads the pointer at the
 * current address, then adds its offset. A non-identifier base (call
 * result, array index, ...) yields a value that is already the pointee
 * address, so the innermost arrow is not loaded again. Returns the temp
 * holding the final member's address, or TEMP_NONE on failure. */
static int lower_member_chain_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *head) {
    ZeroCCASTNode *stack[128];
    int n = 0;
    ASTRef base_ref = AST_NULL;
    ZeroCCASTNode *curr = head;
    while (curr && curr->tag == NODE_MEMBER && n < 128) {
        stack[n++] = curr;
        base_ref = curr->left;
        curr = zero_cc_ast_get(arena, curr->left);
    }
    ZeroCCASTNode *base_node = curr;
    if (!base_node || n < 2) return TEMP_NONE;

    int addr = TEMP_NONE;
    bool generic_base = false;
    if (base_node->tag == NODE_IDENT) {
        ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
        ZeroCCGlobalVar *ggv = (sym && !sym->is_extern) ? NULL : find_global_var(base_node->s.str);
        if (sym && !sym->is_extern) {
            addr = c->next_temp_id++;
            ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
        } else if (ggv) {
            addr = c->next_temp_id++;
            ir_emit_global_addr(c, addr, ggv);
        } else {
            generic_base = true;
        }
    } else if (base_node->tag == NODE_COMPOUND_LIT) {
        (void)lower_expr(c, arena, base_ref);
        ZeroCCSym *sym = lowering_local_sym(c, base_node, base_node->s.str); /* index-based: name lookup misses post-bound __clit syms */
        if (sym) {
            addr = c->next_temp_id++;
            ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
        } else {
            generic_base = true;
        }
    } else {
        generic_base = true;
    }

    if (generic_base) {
        bool innermost_is_ptr = (stack[n - 1]->flags & NODE_MEMBER_IS_ARROW) != 0;
        if (!innermost_is_ptr) {
            addr = zero_cc_lower_lvalue_addr(c, arena, base_ref);
        }
        if (addr == TEMP_NONE) {
            addr = lower_expr(c, arena, base_ref);
        }
        if (addr == TEMP_NONE) return TEMP_NONE;
    }

    /* stack[0] = outermost segment ... stack[n-1] = innermost (adjacent
     * to the base). Process innermost-first: walk from the base outward. */
    for (int i = n - 1; i >= 0 && addr != TEMP_NONE; i--) {
        ZeroCCASTNode *m = stack[i];
        int32_t off = (int32_t)m->aux.alt;
        bool skip_load = (generic_base && i == n - 1);
        if ((m->flags & NODE_MEMBER_IS_ARROW) && !skip_load) {
            int ptr = c->next_temp_id++;
            ir_emit_deref(ptr, addr);
            addr = ptr;
        }
        if (off != 0) {
            int o = c->next_temp_id++;
            ir_emit_mov_imm(o, off);
            int na = c->next_temp_id++;
            ir_emit_add(na, addr, o);
            addr = na;
        }
    }
    return addr;
}

int zero_cc_lower_lvalue_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return TEMP_NONE;
    ZeroCCASTNode *operand = zero_cc_ast_get(arena, ref);
    if (!operand) return TEMP_NONE;

    if (operand->tag == NODE_IDENT) {
        ZeroCCSym *sym = zero_cc_find_local(c, operand->s.str);
        if (sym && !sym->is_extern) {
            int addr_temp = c->next_temp_id++;
            if (sym->is_vla) {
                ir_emit_load(addr_temp, BE(c)->fp_reg, -sym->stack_offset);
            } else {
                ir_emit_lea(addr_temp, TEMP_NONE, -sym->stack_offset);
            }
            return addr_temp;
        }
        ZeroCCGlobalVar *gv = find_global_var(operand->s.str);
        if (gv) {
            int addr_temp = c->next_temp_id++;
            ir_emit_global_addr(c, addr_temp, gv);
            return addr_temp;
        }
        ZeroCCFunction *fn = zero_cc_find_function(c, operand->s.str);
        if (fn) {
            int addr_temp = c->next_temp_id++;
            ir_emit_mov_global(addr_temp, fn->name);
            return addr_temp;
        }
    } else if (operand->tag == NODE_COMPOUND_LIT) {
        (void)lower_expr(c, arena, ref);
        ZeroCCSym *sym = lowering_local_sym(c, operand, operand->s.str); /* index-based: name lookup misses post-bound __clit syms */
        if (sym) {
            int addr_temp = c->next_temp_id++;
            ir_emit_lea(addr_temp, TEMP_NONE, -sym->stack_offset);
            return addr_temp;
        }
    } else if (operand->tag == NODE_MEMBER) {
        int32_t member_offset = 0;
        int chain_depth = 0;
        ZeroCCASTNode *curr = operand;
        ZeroCCASTNode *innermost = operand;
        ASTRef base_ref = ref;
        while (curr && curr->tag == NODE_MEMBER) {
            member_offset += (int32_t)curr->aux.alt;
            chain_depth++;
            innermost = curr;
            base_ref = curr->left;
            curr = zero_cc_ast_get(arena, curr->left);
        }
        ZeroCCASTNode *base_node = curr;
        int addr_temp = c->next_temp_id++;
        if (chain_depth >= 2) {
            int ch = lower_member_chain_addr(c, arena, operand);
            if (ch != TEMP_NONE) return ch;
        }
        bool base_is_ptr = (innermost && (innermost->flags & NODE_MEMBER_IS_ARROW)) != 0;
        if (!base_is_ptr && base_node && base_node->tag == NODE_IDENT) {
            ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
            if (sym && !sym->is_extern) {
                int32_t total_offset = -sym->stack_offset + member_offset;
                ir_emit_lea(addr_temp, TEMP_NONE, total_offset);
                return addr_temp;
            }
            ZeroCCGlobalVar *bgv = find_global_var(base_node->s.str);
            if (bgv) {
                ir_emit_global_addr(c, addr_temp, bgv);
                if (member_offset != 0) {
                    int off_temp = c->next_temp_id++;
                    ir_emit_mov_imm(off_temp, member_offset);
                    int sum_temp = c->next_temp_id++;
                    ir_emit_add(sum_temp, addr_temp, off_temp);
                    addr_temp = sum_temp;
                }
                return addr_temp;
            }
        } else if (!base_is_ptr && base_node && base_node->tag == NODE_COMPOUND_LIT) {
            (void)lower_expr(c, arena, base_ref);
            ZeroCCSym *sym = lowering_local_sym(c, base_node, base_node->s.str); /* index-based: name lookup misses post-bound __clit syms */
            if (sym) {
                int32_t total_offset = -sym->stack_offset + member_offset;
                ir_emit_lea(addr_temp, TEMP_NONE, total_offset);
                return addr_temp;
            }
        }
        int base_ptr = TEMP_NONE;
        if (!base_is_ptr) {
            base_ptr = zero_cc_lower_lvalue_addr(c, arena, base_ref);
        }
        if (base_ptr == TEMP_NONE) {
            base_ptr = lower_expr(c, arena, base_ref);
        }
        if (member_offset != 0) {
            int off_temp = c->next_temp_id++;
            ir_emit_mov_imm(off_temp, member_offset);
            ir_emit_add(addr_temp, base_ptr, off_temp);
        } else {
            ir_emit_mov_reg(addr_temp, base_ptr);
        }
        return addr_temp;
    } else if (operand->tag == NODE_INDEX) {
        int array_temp = lower_expr(c, arena, operand->left);
        int index_temp = lower_expr(c, arena, operand->right);
        size_t elem_size = 4;
        TypeInfo *elem_ti = ast_node_get_typeinfo(c, operand);
        if (elem_ti) {
            elem_size = type_size(elem_ti);
        } else {
            ZeroCCASTNode *ln = zero_cc_ast_get(arena, operand->left);
            if (ln && ln->tag == NODE_IDENT) {
                const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                if (zt && zt->base && zt->base->size > 0) {
                    elem_size = zt->base->size;
                }
            }
        }
        if (elem_size == 0) elem_size = 4;
        int scaled_index;
        if (elem_size == 1) {
            scaled_index = index_temp;
        } else if (elem_size == 2 || elem_size == 4 || elem_size == 8) {
            int shift = (elem_size == 2) ? 1 : ((elem_size == 4) ? 2 : 3);
            int sh_temp = c->next_temp_id++;
            ir_emit_mov_imm(sh_temp, shift);
            scaled_index = c->next_temp_id++;
            ir_emit_shl(scaled_index, index_temp, sh_temp);
        } else {
            int sz_temp = c->next_temp_id++;
            ir_emit_mov_imm(sz_temp, (int32_t)elem_size);
            scaled_index = c->next_temp_id++;
            ir_emit_mul(scaled_index, index_temp, sz_temp);
        }
        int addr_temp = c->next_temp_id++;
        ir_emit_add(addr_temp, array_temp, scaled_index);
        return addr_temp;
    } else if (operand->tag == NODE_UNOP && (operand->aux.op == '*' || operand->aux.op == TK_STAR)) {
        return lower_expr(c, arena, operand->left);
    }
    return TEMP_NONE;
}

void zero_cc_emit_struct_copy_to_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                      int dst_addr, size_t struct_sz,
                                      ASTRef rhs_ref, int rhs_temp) {
    int num_chunks = (int)((struct_sz + 7) / 8);
    ZeroCCASTNode *rnode = zero_cc_ast_get(arena, rhs_ref);
    ZeroCCSym *src_sym = (rnode && rnode->tag == NODE_IDENT) ? zero_cc_find_local(c, rnode->s.str) : NULL;
    bool rhs_is_struct_addr = ast_expr_is_struct_addr(c, arena, rhs_ref);

    for (int i = 0; i < num_chunks; i++) {
        size_t rem = struct_sz - (size_t)(i * 8);
        int ch_val = c->next_temp_id++;
        int cur_dst = dst_addr;
        if (i > 0) {
            int off_t = c->next_temp_id++;
            ir_emit_mov_imm(off_t, i * 8);
            cur_dst = c->next_temp_id++;
            ir_emit_add(cur_dst, dst_addr, off_t);
        }
        if (src_sym && (src_sym->type.tag == TYPE_STRUCT || src_sym->type.tag == TYPE_UNION)) {
            ir_emit_load(ch_val, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
        } else if (rhs_is_struct_addr) {
            int cur_src = rhs_temp;
            if (i > 0) {
                int off_t = c->next_temp_id++;
                ir_emit_mov_imm(off_t, i * 8);
                cur_src = c->next_temp_id++;
                ir_emit_add(cur_src, rhs_temp, off_t);
            }
            if (rem >= 8) ir_emit_deref(ch_val, cur_src);
            else if (rem >= 4) ir_emit_load_32(ch_val, cur_src);
            else if (rem >= 2) ir_emit_load_16(ch_val, cur_src);
            else ir_emit_load_byte(ch_val, cur_src);
        } else if (i == 0) {
            ch_val = rhs_temp;
        } else if (i == 1 && struct_sz <= 16) {
            int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? 1 : 2;
            ch_val = reg2;
        }
        if (rem >= 8) ir_emit_store_64(ch_val, cur_dst);
        else if (rem >= 4) ir_emit_store_32(ch_val, cur_dst);
        else if (rem >= 2) ir_emit_store_16(ch_val, cur_dst);
        else ir_emit_store_byte(ch_val, cur_dst);
    }
}

/* Apply a compound-assignment op (+=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=)
 * to the member value at address temp `addr` (m_size bytes; float members
 * use the double-width FP model). Loads the current value with the
 * width-correct load, applies the op, and updates *right_temp. Mirrors the
 * multi-segment chain handling (C5). */
/* C99 6.3.1.2/6.5: extend a 1/2/4-byte integer temp to full 64-bit width at
 * a load/use boundary (sext for signed, zext for unsigned).  Narrow loads
 * (mov r32, (addr)) zero-extend into the 64-bit register, so without this a
 * negative int loaded from an array element or pointer deref becomes a large
 * positive 64-bit value and miscompiles comparisons, division (idiv's cqo
 * sign-extends from bit 63), and 64-bit arithmetic.  No-op for 8-byte,
 * floating, pointer, and aggregate types. */
static void lower_extend_narrow(ZeroCCCompiler *c, int temp, const TypeInfo *ti) {
    (void)c;
    if (!ti) return;
    if (ti->kind != TYPE_KIND_BOOL && ti->kind != TYPE_KIND_CHAR &&
        ti->kind != TYPE_KIND_SHORT && ti->kind != TYPE_KIND_INT &&
        ti->kind != TYPE_KIND_ENUM) return; /* enums are signed ints */
    size_t sz = type_size(ti);
    if (sz != 1 && sz != 2 && sz != 4) return;
    if (type_is_unsigned(ti)) ir_emit_zext(temp, (int)sz);
    else ir_emit_sext(temp, (int)sz);
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

/* C11 6.5.2.4/6.5.6: the step of ++/-- on an lvalue of type t is
 * sizeof(pointee) for pointer types (pointer arithmetic is scaled by the
 * object size) and 1 otherwise.  A pointer to an incomplete/zero-sized
 * pointee (e.g. a function pointer) uses the pointer size itself —
 * standard C forbids the arithmetic (constraint violation); be permissive. */
static int lvalue_inc_step(const ZeroCCType *t) {
    if (!t) return 1;
    if (t->tag == TYPE_PTR) {
        if (t->base && t->base->size > 0) return (int)t->base->size;
        return 8;
    }
    return 1;
}

static void member_apply_compound(ZeroCCCompiler *c, int op_tok, int addr,
                                  int *right_temp, size_t m_size, bool is_flt) {
    if (is_flt) {
        int lhs_val = c->next_temp_id++;
        if (m_size == 4) ir_emit_vloadss((uint16_t)lhs_val, (uint16_t)addr);
        else ir_emit_vloadsd((uint16_t)lhs_val, (uint16_t)addr);
        int float_rhs = *right_temp;
        if (!c->last_expr_is_float) {
            float_rhs = c->next_temp_id++;
            ir_emit_cvtsi2sd((uint16_t)float_rhs, (uint16_t)*right_temp);
        }
        int op_res = c->next_temp_id++;
        switch (op_tok) {
            case TK_ADD_ASSIGN: ir_emit_fadd((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
            case TK_SUB_ASSIGN: ir_emit_fsub((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
            case TK_MUL_ASSIGN: ir_emit_fmul((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
            case TK_DIV_ASSIGN: ir_emit_fdiv((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
            default: op_res = float_rhs; break;
        }
        *right_temp = op_res;
        c->last_expr_is_float = 1;
    } else {
        int curr_val = c->next_temp_id++;
        if (m_size == 1) ir_emit_load_byte(curr_val, addr);
        else if (m_size == 2) ir_emit_load_16(curr_val, addr);
        else if (m_size == 4) ir_emit_load_32(curr_val, addr);
        else ir_emit_deref(curr_val, addr);
        int op_res = c->next_temp_id++;
        switch (op_tok) {
            case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, *right_temp); break;
            case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, *right_temp); break;
            case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, *right_temp); break;
            case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, *right_temp); break;
            case TK_MOD_ASSIGN: ir_emit_mod(op_res, curr_val, *right_temp); break;
            case TK_AND_ASSIGN: ir_emit_and(op_res, curr_val, *right_temp); break;
            case TK_OR_ASSIGN:  ir_emit_or(op_res, curr_val, *right_temp); break;
            case TK_XOR_ASSIGN: ir_emit_xor(op_res, curr_val, *right_temp); break;
            case TK_SHL_ASSIGN: ir_emit_shl(op_res, curr_val, *right_temp); break;
            case TK_SHR_ASSIGN: ir_emit_shr(op_res, curr_val, *right_temp); break;
            default: ir_emit_mov_reg(op_res, *right_temp); break;
        }
        *right_temp = op_res;
    }
}

static int assign_member_bitfield(ZeroCCCompiler *c,
                                  ZeroCCASTNode *assign_node, ZeroCCASTNode *left_node,
                                  int addr_temp, int right_temp) {
    uint8_t bit_offset = (uint8_t)(left_node->aux.step & 0xFF);
    uint8_t bit_width = (uint8_t)((left_node->aux.step >> 8) & 0xFF);
    bool is_signed = ((left_node->aux.step >> 16) & 1) != 0;
    uint64_t val_mask = (bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1);
    TypeInfo *ti = ast_node_get_typeinfo(c, left_node);
    size_t m_size = ti ? type_size(ti) : 4;
    bool is_64 = (m_size > 4 || (bit_offset + bit_width > 32));

    /* 1. Load current storage unit */
    int curr_temp = c->next_temp_id++;
    if (is_64) ir_emit_deref(curr_temp, addr_temp);
    else ir_emit_load_32(curr_temp, addr_temp);

    bool is_compound = (assign_node->aux.op != TK_ASSIGN && assign_node->aux.op != '=');
    if (is_compound) {
        int curr_val = curr_temp;
        if (bit_offset > 0) {
            int shift_amt = c->next_temp_id++;
            ir_emit_mov_imm(shift_amt, (int32_t)bit_offset);
            int shifted = c->next_temp_id++;
            ir_emit_shr(shifted, curr_val, shift_amt);
            curr_val = shifted;
        }
        int mask_t = c->next_temp_id++;
        ir_emit_mov_imm(mask_t, (int32_t)val_mask);
        int masked = c->next_temp_id++;
        ir_emit_and(masked, curr_val, mask_t);
        curr_val = masked;
        if (is_signed && bit_width < 32) {
            int32_t sign_bit = (int32_t)(1ULL << (bit_width - 1));
            int s_temp = c->next_temp_id++;
            ir_emit_mov_imm(s_temp, sign_bit);
            int x_temp = c->next_temp_id++;
            ir_emit_xor(x_temp, curr_val, s_temp);
            int e_temp = c->next_temp_id++;
            ir_emit_sub(e_temp, x_temp, s_temp);
            curr_val = e_temp;
        }
        int op_res = c->next_temp_id++;
        switch (assign_node->aux.op) {
            case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, right_temp); break;
            case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, right_temp); break;
            case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, right_temp); break;
            case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, right_temp); break;
            case TK_MOD_ASSIGN: ir_emit_mod(op_res, curr_val, right_temp); break;
            case TK_AND_ASSIGN: ir_emit_and(op_res, curr_val, right_temp); break;
            case TK_OR_ASSIGN:  ir_emit_or(op_res, curr_val, right_temp); break;
            case TK_XOR_ASSIGN: ir_emit_xor(op_res, curr_val, right_temp); break;
            case TK_SHL_ASSIGN: ir_emit_shl(op_res, curr_val, right_temp); break;
            case TK_SHR_ASSIGN: ir_emit_shr(op_res, curr_val, right_temp); break;
            default: ir_emit_mov_reg(op_res, right_temp); break;
        }
        right_temp = op_res;
    }

    /* 2. Clear target bits */
    uint64_t field_mask = (bit_width >= 64) ? ~0ULL : (val_mask << bit_offset);
    uint64_t clear_mask = ~field_mask;
    int clear_mask_temp = c->next_temp_id++;
    ir_emit_mov_imm(clear_mask_temp, (int64_t)clear_mask);
    int cleared_temp = c->next_temp_id++;
    ir_emit_and(cleared_temp, curr_temp, clear_mask_temp);

    /* 3. Mask incoming value to bit_width */
    int val_mask_temp = c->next_temp_id++;
    ir_emit_mov_imm(val_mask_temp, (int64_t)val_mask);
    int masked_val = c->next_temp_id++;
    ir_emit_and(masked_val, right_temp, val_mask_temp);

    /* 4. Shift into position */
    if (bit_offset > 0) {
        int shl_amt = c->next_temp_id++;
        ir_emit_mov_imm(shl_amt, (int32_t)bit_offset);
        int shifted_val = c->next_temp_id++;
        ir_emit_shl(shifted_val, masked_val, shl_amt);
        masked_val = shifted_val;
    }

    /* 5. Combine and store */
    int combined_temp = c->next_temp_id++;
    ir_emit_or(combined_temp, cleared_temp, masked_val);
    if (is_64) ir_emit_store_64(combined_temp, addr_temp);
    else ir_emit_store_32(combined_temp, addr_temp);

    c->last_expr_is_float = 0;
    return right_temp;
}

/* Conservative AST probe (Sprint 40): does this expression evaluate to a
 * float/double under the double-width scalar FP model?  Used by the
 * ternary operator to pick the result temp's register class BEFORE the
 * then-arm move is emitted (the else arm's class is only known after it
 * is lowered, which happens after the then move).  Unrecognized shapes
 * return 0 (treat as int) - a wrong 0 keeps the pre-Sprint-40 behavior
 * (no regression) and never over-converts. */
static int ast_expr_is_fp(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return 0;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return 0;
    switch (n->tag) {
        case NODE_FLOAT:
            return 1;
        case NODE_INT:
        case NODE_STRING:
        case NODE_BOOL:
        case NODE_NULLPTR:
            return 0;
        case NODE_CAST: {
            TypeInfo *ti = ast_node_get_typeinfo(c, n);
            return ti ? type_is_floating(ti) : 0;
        }
        case NODE_IDENT: {
            ZeroCCSym *s = zero_cc_find_local(c, n->s.str);
            if (s) return s->type.tag == TYPE_FLOAT || s->type.tag == TYPE_DOUBLE;
            ZeroCCGlobalVar *g = find_global_var(n->s.str);
            if (g) return g->type.tag == TYPE_FLOAT || g->type.tag == TYPE_DOUBLE;
            return 0;
        }
        case NODE_BINOP: {
            if (ast_expr_is_fp(c, arena, n->left)) return 1;
            switch (n->aux.op) {
                case TK_EQ: case TK_NE: case TK_LT: case TK_LE:
                case TK_GT: case TK_GE:
                    return 0;  /* comparisons yield int */
                default:
                    return ast_expr_is_fp(c, arena, n->right);
            }
        }
        case NODE_UNOP:
            switch (n->aux.op) {
                case TK_BANG:
                    return 0;  /* !x yields int even for FP x */
                default:
                    return ast_expr_is_fp(c, arena, n->left);
            }
        case NODE_TERNARY:
            return ast_expr_is_fp(c, arena, n->right) ||
                   ast_expr_is_fp(c, arena, n->aux.alt);
        case NODE_CALL: {
            ZeroCCASTNode *cal = zero_cc_ast_get(arena, n->left);
            if (cal && cal->tag == NODE_IDENT) {
                ZeroCCFunction *f = zero_cc_find_function(c, cal->s.str);
                if (f) return f->return_type.tag == TYPE_FLOAT ||
                            f->return_type.tag == TYPE_DOUBLE;
            }
            return 0;
        }
        default:
            return 0;
    }
}

static bool zc_type_offset_is_float(const ZeroCCType *t, size_t off) {
    if (!t) return false;
    if (t->tag == TYPE_FLOAT || t->tag == TYPE_DOUBLE) return true;
    if (t->tag == TYPE_ARRAY) {
        if (!t->base) return false;
        size_t el_sz = t->base->size ? t->base->size : 4;
        return zc_type_offset_is_float(t->base, el_sz ? (off % el_sz) : 0);
    }
    if (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) {
        for (size_t i = 0; i < t->member_count; i++) {
            const ZeroCCStructMember *m = &t->members[i];
            if (off >= m->offset && off < m->offset + m->size) {
                if (m->tag == TYPE_FLOAT || m->tag == TYPE_DOUBLE) return true;
                if (m->tag == TYPE_ARRAY && m->base &&
                    (m->base->tag == TYPE_FLOAT || m->base->tag == TYPE_DOUBLE)) return true;
                if ((m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) && m->base) {
                    return zc_type_offset_is_float(m->base, off - m->offset);
                }
                return false;
            }
        }
        return false;
    }
    return false;
}

int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return TEMP_NONE;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return TEMP_NONE;

    switch (node->tag) {
        case NODE_INT: {
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, node->int_val);
            c->last_expr_is_float = 0;
            return temp;
        }

        case NODE_BOOL: {
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, node->int_val);
            c->last_expr_is_float = 0;
            return temp;
        }

        case NODE_NULLPTR: {
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, 0);
            c->last_expr_is_float = 0;
            return temp;
        }

        case NODE_FLOAT: {
            /* Float literals: store in rodata and load via RIP-relative addressing.
             * A double must be 8-byte aligned in .rodata (AArch64 LDR D requires 8-
             * byte alignment; equally required on x86-64), so pad to the next 8-byte
             * boundary before emitting it. (Sprint 41) */
            double val = node->float_val;
            size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
            if (pad) { uint8_t z8[8] = {0}; zero_cc_buf_put_bytes(&c->rodata_section, z8, pad); }
            size_t rodata_offset = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, &val, sizeof(double));
            int temp = c->next_temp_id++;
            ir_emit_fmov_imm((uint16_t)temp, (int64_t)rodata_offset);
            c->last_expr_is_float = 1;
            return temp;
        }

        case NODE_STRING: {
            int temp = c->next_temp_id++;
            if (node->s.str) {
                size_t len = node->s_len; /* may contain embedded NULs */
                if (node->flags & NODE_FLAG_WIDE) {
                    while (c->rodata_section.length % 4 != 0) {
                        zero_cc_emit_u8(&c->rodata_section, 0);
                    }
                    size_t rodata_offset = c->rodata_section.length;
                    zero_cc_buf_put_bytes(&c->rodata_section, node->s.str, len);
                    uint32_t zero = 0;
                    zero_cc_buf_put_bytes(&c->rodata_section, &zero, 4);
                    ir_emit_lea_rip(temp, rodata_offset);
                } else {
                    size_t rodata_offset = c->rodata_section.length;
                    zero_cc_buf_put_bytes(&c->rodata_section, node->s.str, len);
                    uint8_t zero = 0;
                    zero_cc_buf_put_bytes(&c->rodata_section, &zero, 1);
                    ir_emit_lea_rip(temp, rodata_offset);
                }
            } else {
                ir_emit_mov_imm(temp, 0);
            }
            c->last_expr_is_float = 0;
            return temp;
        }

        case NODE_IDENT: {
            const char *name = node->s.str;

            /* Special case: __func__ returns pointer to function name string */
            if (strcmp(name, "__func__") == 0) {
                const char *fn_name = "";
                ZeroCCFunction *cur_fn = zero_cc_get_current_fn(c);
                if (cur_fn) {
                    fn_name = cur_fn->name;
                }
                size_t len = strlen(fn_name);
                size_t rodata_offset = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, fn_name, len);
                zero_cc_buf_put_bytes(&c->rodata_section, "\0", 1);
                int temp = c->next_temp_id++;
                ir_emit_lea_rip(temp, rodata_offset);
                c->last_expr_is_float = 0;
                return temp;
            }

            /* Inlined function parameter substitution */
            if (c->lowering.inlining.active_depth > 0) {
                for (int p = 0; p < c->lowering.inlining.param_count; p++) {
                    if (c->lowering.inlining.param_names[p] &&
                        strcmp(name, c->lowering.inlining.param_names[p]) == 0) {
                        int p_temp = c->lowering.inlining.param_temps[p];
                        c->last_expr_is_float = 0;
                        return p_temp;
                    }
                }
            }

            ZeroCCSym *sym = zero_cc_find_local(c, name);
            if (sym && !sym->is_extern) {
                if (sym->is_promoted) {
                    c->last_expr_is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE) ? 1 : 0;
                    return sym->promoted_vreg;
                }
                if (sym->type.tag == TYPE_FLOAT) {
                    int addr = c->next_temp_id++;
                    ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
                    int temp = c->next_temp_id++;
                    ir_emit_vloadss((uint16_t)temp, (uint16_t)addr);
                    c->last_expr_is_float = 1;
                    return temp;
                } else if (sym->type.tag == TYPE_DOUBLE) {
                    /* Float load via IR for proper register allocation */
                    int temp = c->next_temp_id++;
                    ir_emit_fload((uint16_t)temp, TEMP_NONE, -sym->stack_offset);
                    c->last_expr_is_float = 1;
                    return temp;
                } else if (sym->type.tag == TYPE_ARRAY && !sym->is_vla) {
                    /* Fixed-size array decays to pointer via LEA */
                    int temp = c->next_temp_id++;
                    ir_emit_lea(temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return temp;
                } else if (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) {
                    /* By-value struct rvalue.  <=16B follows the ABI
                     * secondary-register convention used by struct returns
                     * and by-value argument marshaling: <=8B value in a
                     * temp; 9-16B value in a temp + second chunk in VREG_2
                     * (the 9-16B dereference-assignment store reads the
                     * second half from VREG_2).  >16B: first 8 bytes as a
                     * value - the by-value argument marshaling path takes
                     * the first chunk from this value and the remaining
                     * chunks directly from the local's stack slot, so the
                     * value must NOT be an address here (va-arg-4.c). */
                    if (sym->type.size <= 8) {
                        int temp = c->next_temp_id++;
                        ir_emit_load(temp, BE(c)->fp_reg, -sym->stack_offset);
                        c->last_expr_is_float = 0;
                        return temp;
                    } else if (sym->type.size <= 16) {
                        int temp = c->next_temp_id++;
                        ir_emit_load(temp, BE(c)->fp_reg, -sym->stack_offset);
                        ir_emit_load(VREG_2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                        c->last_expr_is_float = 0;
                        return temp;
                    } else {
                        int temp = c->next_temp_id++;
                        ir_emit_load(temp, BE(c)->fp_reg, -sym->stack_offset);
                        c->last_expr_is_float = 0;
                        return temp;
                    }
                } else {
                    int temp = c->next_temp_id++;
                    ir_emit_load(temp, BE(c)->fp_reg, -sym->stack_offset);
                    /* 32-bit int semantics: extend narrow int locals to full
                     * 64-bit width (sign/zero per type) so overflowed values
                     * compare and store correctly. No-op for ptr/8-byte. */
                    zero_cc_emit_int_extend(c, temp, &sym->type);
                    if (sym->is_restrict && sym->type.tag == TYPE_PTR && temp < 256) {
                        /* Each restrict pointer gets its own alias group (locals index + 1) */
                        c->lowering.temp_alias_group[temp] = (uint8_t)((sym - c->locals) + 1);
                    }
                    c->last_expr_is_float = 0;
                    return temp;
                }
            } else {
                /* TLS statics are registered globals (is_tls) - the address
                 * op below selects OP_TLS_LEA (AOT) automatically. */
                ZeroCCGlobalVar *gv = find_global_var(name);
                if (gv) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_global_addr(c, addr_temp, gv);
                    /* Arrays and structs decay/return address, scalars load value */
                    if (gv->type.tag == TYPE_ARRAY || gv->type.tag == TYPE_STRUCT || gv->type.tag == TYPE_UNION) {
                        c->last_expr_is_float = 0;
                        return addr_temp;
                    }
                    int val_temp = c->next_temp_id++;
                    if (gv->type.tag == TYPE_FLOAT) {
                        ir_emit_vloadss((uint16_t)val_temp, (uint16_t)addr_temp);
                        c->last_expr_is_float = 1;
                        return val_temp;
                    }
                    if (gv->type.tag == TYPE_DOUBLE) {
                        ir_emit_vloadsd((uint16_t)val_temp, (uint16_t)addr_temp);
                        c->last_expr_is_float = 1;
                        return val_temp;
                    }
                    size_t gv_sz = gv->size > 0 ? gv->size : 4;
                    if (gv_sz == 1) ir_emit_load_byte(val_temp, addr_temp);
                    else if (gv_sz == 2) ir_emit_load_16(val_temp, addr_temp);
                    else if (gv_sz == 4) ir_emit_load_32(val_temp, addr_temp);
                    else ir_emit_deref(val_temp, addr_temp);
                    /* Width-aware loads zero-extend; complete sign extension
                     * for signed narrow ints (e.g. `int g=-1`). */
                    zero_cc_emit_int_extend(c, val_temp, &gv->type);
                    c->last_expr_is_float = 0;
                    return val_temp;
                }
                ZeroCCEnumConst *ec = zero_cc_find_enum_const(c, name);
                if (ec) {
                    int temp = c->next_temp_id++;
                    ir_emit_mov_imm(temp, ec->value);
                    c->last_expr_is_float = 0;
                    return temp;
                }
                ZeroCCFunction *fn = zero_cc_find_function(c, name);
                if (fn) {
                    int temp = c->next_temp_id++;
                    ir_emit_mov_global(temp, fn->name);
                    if (fn->is_external) {
                        /* Extern object: load its value through the patched
                         * address. FP types must stay in the FP pipeline -
                         * an integer deref would load the IEEE-754 bits
                         * (cross-TU `double g;` regression). */
                        if (fn->return_type.tag == TYPE_FLOAT) {
                            int ftemp = c->next_temp_id++;
                            ir_emit_vloadss((uint16_t)ftemp, (uint16_t)temp);
                            c->last_expr_is_float = 1;
                            return ftemp;
                        }
                        if (fn->return_type.tag == TYPE_DOUBLE) {
                            int ftemp = c->next_temp_id++;
                            ir_emit_vloadsd((uint16_t)ftemp, (uint16_t)temp);
                            c->last_expr_is_float = 1;
                            return ftemp;
                        }
                        int val_temp = c->next_temp_id++;
                        size_t ext_sz = fn->return_type.size > 0 ? fn->return_type.size : 8;
                        if (ext_sz == 1) ir_emit_load_byte(val_temp, temp);
                        else if (ext_sz == 2) ir_emit_load_16(val_temp, temp);
                        else if (ext_sz == 4) ir_emit_load_32(val_temp, temp);
                        else ir_emit_deref(val_temp, temp);
                        c->last_expr_is_float = 0;
                        return val_temp;
                    }
                    c->last_expr_is_float = 0;
                    return temp;
                }
                return TEMP_NONE;
            }
        }

        case NODE_BINOP: {
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
             * promote the integer operand to double (C usual arithmetic
             * conversions) so the float op path is taken instead of an
             * integer op on the raw bit pattern. */
            if (left_is_float != right_is_float) {
                if (right_is_float) {
                    int conv_temp = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)conv_temp, (uint16_t)left_temp);
                    left_temp = conv_temp;
                    left_is_float = 1;
                } else {
                    int conv_temp = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)conv_temp, (uint16_t)right_temp);
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
                    break;
                case TK_MINUS:
                    ir_emit_sub(result_temp, left_temp, right_temp);
                    break;
                case TK_STAR:
                    ir_emit_mul(result_temp, left_temp, right_temp);
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

        case NODE_COMMA: {
            /* Comma operator: evaluate left, discard result, evaluate right, return right */
            lower_expr(c, arena, node->left);
            int right_temp = lower_expr(c, arena, node->right);
            return right_temp;
        }

        case NODE_UNOP: {
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
                                int addr = c->next_temp_id++;
                                ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
                                ir_emit_vloadss((uint16_t)val_temp, (uint16_t)addr);
                            } else {
                                ir_emit_fload((uint16_t)val_temp, TEMP_NONE, -sym->stack_offset);
                            }
                            ir_emit_mov_imm(one_i, 1);
                            ir_emit_cvtsi2sd((uint16_t)one_f, (uint16_t)one_i);
                            if (node->aux.op == TK_INC) {
                                ir_emit_fadd((uint16_t)res_temp, (uint16_t)val_temp, (uint16_t)one_f);
                            } else {
                                ir_emit_fsub((uint16_t)res_temp, (uint16_t)val_temp, (uint16_t)one_f);
                            }
                            if (sym->type.tag == TYPE_FLOAT) {
                                int addr = c->next_temp_id++;
                                ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
                                ir_emit_vstoress((uint16_t)addr, (uint16_t)res_temp);
                            } else {
                                ir_emit_fstore((uint16_t)res_temp, TEMP_NONE, -sym->stack_offset);
                            }
                            c->last_expr_is_float = 1;
                            return (node->flags & 1) ? val_temp : res_temp;
                        }

                        int step = lvalue_inc_step(&sym->type);
                        int val_temp = c->next_temp_id++;
                        int one_temp = c->next_temp_id++;
                        int res_temp = c->next_temp_id++;

                        /* Load current variable value from stack */
                        ir_emit_load(val_temp, BE(c)->fp_reg, -sym->stack_offset);
                        /* Step: sizeof(*x) for pointer locals (C11 6.5.6), else 1 */
                        ir_emit_mov_imm(one_temp, (node->aux.op == TK_INC) ? step : -step);
                        /* res = val ± step */
                        ir_emit_add(res_temp, val_temp, one_temp);
                        zero_cc_emit_int_extend(c, res_temp, &sym->type);
                        /* Store new value back to stack */
                        ir_emit_store(res_temp, BE(c)->fp_reg, -sym->stack_offset);

                        c->last_expr_is_float = 0;
                        /* Postfix x++ / x-- yields the OLD value; prefix
                         * ++x / --x yields the new value ((node->flags & 1)
                         * marks postfix, matching the global/atomic paths
                         * below). Pre-fix the local path always returned the
                         * new value, miscompiling `return x++;` etc. */
                        return (node->flags & 1) ? val_temp : res_temp;
                    }
                    ZeroCCGlobalVar *gv = find_global_var(var);
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
                                ZeroCCGlobalVar *bgv = bsym ? NULL : find_global_var(base_node->s.str);
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
                            ZeroCCGlobalVar *gv = find_global_var(inner->s.str);
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
                            ZeroCCGlobalVar *gv = find_global_var(inner->s.str);
                            if (gv && gv->type.base && (gv->type.base->tag == TYPE_STRUCT || gv->type.base->tag == TYPE_UNION)) {
                                is_struct = true;
                            }
                        }
                    }
                    if (is_struct && p_size > 16) {
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
                            ZeroCCGlobalVar *gv = find_global_var(inner->s.str);
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
                case TK_BANG:  /* Logical NOT: !operand */
                    if (c->last_expr_is_float) {
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
                case TK_TILDE:  /* Bitwise NOT */
                    ir_emit_not(result_temp, operand_temp);
                    break;
                default:
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
            c->last_expr_is_float = 0;
            return result_temp;
        }

        case NODE_CAST: {
            TypeInfo *cast_ti = ast_node_get_typeinfo(c, node);
            bool target_is_float = type_is_floating(cast_ti);
            ZeroCCASTNode *cast_src = zero_cc_ast_get(arena, node->left);
            if (cast_src && cast_src->tag == NODE_FLOAT && !target_is_float) {
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
                            uint16_t skip_sticky = c->ir_next_label++;
                            ir_emit_je((uint16_t)cond_sticky, skip_sticky);
                            int bit31 = c->next_temp_id++;
                            ir_emit_mov_imm(bit31, 0x80000000LL);
                            int or_res = c->next_temp_id++;
                            ir_emit_or(or_res, result, bit31);
                            int mask_hi = c->next_temp_id++;
                            ir_emit_mov_imm(mask_hi, ~0x7FFFFFFFLL);
                            int clean_res = c->next_temp_id++;
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
            /* int->int narrowing cast ((int)longv, (short)x): extend the
             * 64-bit value to the target width so it carries true N-bit
             * semantics (a plain cast previously did nothing). */
            if (cast_ti) {
                size_t ctsz = type_size(cast_ti);
                if (ctsz == 1 || ctsz == 2 || ctsz == 4) {
                    bool ctsu = (cast_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                    if (ctsu) ir_emit_zext(result, (int)ctsz);
                    else ir_emit_sext(result, (int)ctsz);
                }
            }
            return result;
        }

        case NODE_VA_START: {
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
                    bool pmulti = (ps->type.tag == TYPE_STRUCT || ps->type.tag == TYPE_UNION) && ps->type.size > 8;
                    if (pmulti) {
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

        case NODE_VA_ARG: {
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

                int t_ptr = c->next_temp_id++;
                ir_emit_deref(t_ptr, addr_ptr_field);
                int t_end = c->next_temp_id++;
                ir_emit_deref(t_end, addr_end_field);
                int t_diff = c->next_temp_id++;
                ir_emit_sub(t_diff, t_ptr, t_end);
                ir_emit_je((uint16_t)t_diff, ovf_label);

                size_t struct_sz = (t_t && (t_t->kind == TYPE_KIND_STRUCT || t_t->kind == TYPE_KIND_UNION)) ? type_size(t_t) : 0;

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
                } else if (struct_sz > 8 && struct_sz <= 16) {
                    ir_emit_deref(result_temp, t_ptr);
                    int t_neg8 = c->next_temp_id++;
                    ir_emit_mov_imm(t_neg8, -8);
                    int t_ptr2 = c->next_temp_id++;
                    ir_emit_add(t_ptr2, t_ptr, t_neg8);
                    ir_emit_deref(VREG_2, t_ptr2);
                    int t_neg16 = c->next_temp_id++;
                    ir_emit_mov_imm(t_neg16, -16);
                    int t_ptr3 = c->next_temp_id++;
                    ir_emit_add(t_ptr3, t_ptr, t_neg16);
                    ir_emit_store_64(t_ptr3, addr_ptr_field);
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
                } else if (struct_sz > 8 && struct_sz <= 16) {
                    ir_emit_deref(result_temp, t_ovf);
                    int t_pos8 = c->next_temp_id++;
                    ir_emit_mov_imm(t_pos8, 8);
                    int t_ovf2 = c->next_temp_id++;
                    ir_emit_add(t_ovf2, t_ovf, t_pos8);
                    ir_emit_deref(VREG_2, t_ovf2);
                    int t_pos16 = c->next_temp_id++;
                    ir_emit_mov_imm(t_pos16, 16);
                    int t_ovf3 = c->next_temp_id++;
                    ir_emit_add(t_ovf3, t_ovf, t_pos16);
                    ir_emit_store_64(t_ovf3, addr_ovf_field);
                } else {
                    ir_emit_deref(result_temp, t_ovf);
                    int t_pos8 = c->next_temp_id++;
                    ir_emit_mov_imm(t_pos8, 8);
                    int t_ovf2 = c->next_temp_id++;
                    ir_emit_add(t_ovf2, t_ovf, t_pos8);
                    ir_emit_store_64(t_ovf2, addr_ovf_field);
                }
                ir_emit_label(end_label);

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

        case NODE_ASSIGN: {
            /* Check if this is a compound assignment (+=, -=, etc.) */
            bool is_compound = (node->aux.op != TK_ASSIGN && node->aux.op != '=');
            ZeroCCASTNode *left_node = zero_cc_ast_get(arena, node->left);

            /* Dereference assignment (*ptr = expr): evaluate LHS target address
             * BEFORE RHS value expression so that RHS calling-convention return
             * registers (VREG_0 / VREG_2 / VREG_F0) are not clobbered by any
             * function calls or statement expressions inside the LHS address
             * computation (e.g. GCC c-torture execute/20000917-1.c:
             * `*({ ({ one(); &a; }); }) = zero();`). Also supports full by-value
             * struct copies for 9-16B (register pair) and >16B (memory buffer). */
            if (left_node && left_node->tag == NODE_UNOP && (left_node->aux.op == '*' || left_node->aux.op == TK_STAR)) {
                ZeroCCASTNode *inner = zero_cc_ast_get(arena, left_node->left);
                if (inner && inner->tag == NODE_IDENT) {
                    ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                    if (ptr_sym && ptr_sym->type.base && ptr_sym->type.base->is_const) {
                        diag_report_error_at_token(c, "cannot assign to const-qualified pointer target '%s'", inner->s.str);
                        return TEMP_NONE;
                    }
                }
                int ptr_temp = lower_expr(c, arena, left_node->left);

                /* Target size up front (it decides the RHS lowering form).
                 * The rhs-type fallback below is filled in after the RHS is
                 * lowered. */
                size_t p_size = 4;
                TypeInfo *target_ti = ast_node_get_typeinfo(c, left_node);
                if (target_ti) {
                    p_size = type_size(target_ti);
                } else if (inner && inner->tag == NODE_IDENT) {
                    ZeroCCSym *ps = zero_cc_find_local(c, inner->s.str);
                    if (ps && ps->type.base && ps->type.base->size > 0) {
                        p_size = ps->type.base->size;
                    }
                }

                ZeroCCASTNode *rhs_node = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                int right_temp;
                if (p_size > 16 && rhs_node && rhs_node->tag == NODE_IDENT) {
                    ZeroCCSym *rhs_sym = zero_cc_find_local(c, rhs_node->s.str);
                    if (rhs_sym && (rhs_sym->type.tag == TYPE_STRUCT || rhs_sym->type.tag == TYPE_UNION) &&
                        rhs_sym->type.size > 16) {
                        /* >16B by-value struct RHS must be a buffer address
                         * for the chunked copy below. A local struct ident
                         * normally lowers to an 8-byte value load (the form
                         * by-value argument marshaling needs), so take its
                         * address instead (c-torture pr35472.c: `*p = a;`
                         * with a 64-byte local struct). */
                        right_temp = c->next_temp_id++;
                        ir_emit_lea(right_temp, BE(c)->fp_reg, -rhs_sym->stack_offset);
                        c->last_expr_is_float = 0;
                    } else {
                        right_temp = lower_expr(c, arena, node->right);
                    }
                } else {
                    right_temp = lower_expr(c, arena, node->right);
                }
                if (p_size == 4 && !target_ti) {
                    TypeInfo *rhs_ti = (rhs_node) ? ast_node_get_typeinfo(c, rhs_node) : NULL;
                    if (rhs_ti && type_size(rhs_ti) > 0) p_size = type_size(rhs_ti);
                }

                /* Pointer compound assignment scaling (p += n / p -= n) */
                if (is_compound && (node->aux.op == TK_ADD_ASSIGN || node->aux.op == TK_SUB_ASSIGN)) {
                    TypeInfo *lvti = ast_node_get_typeinfo(c, left_node);
                    if (lvti && (type_is_pointer(lvti) || lvti->kind == TYPE_KIND_ARRAY)) {
                        size_t psz = type_size(lvti->kind == TYPE_KIND_ARRAY ?
                            lvti->details.array.base : lvti->details.pointer.base);
                        if (psz > 1) {
                            int si = c->next_temp_id++;
                            ir_emit_mov_imm(si, (int32_t)psz);
                            int s = c->next_temp_id++;
                            ir_emit_mul(s, right_temp, si);
                            right_temp = s;
                        }
                    }
                }

                int final_val = right_temp;
                if (is_compound) {
                    int curr_val = c->next_temp_id++;
                    if (p_size == 1) ir_emit_load_byte(curr_val, ptr_temp);
                    else if (p_size == 2) ir_emit_load_16(curr_val, ptr_temp);
                    else if (p_size == 4) ir_emit_load_32(curr_val, ptr_temp);
                    else ir_emit_deref(curr_val, ptr_temp);
                    lower_extend_narrow(c, curr_val, target_ti);
                    int op_res = c->next_temp_id++;
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, right_temp); break;
                        case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, right_temp); break;
                        case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, right_temp); break;
                        case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, right_temp); break;
                        case TK_MOD_ASSIGN: ir_emit_mod(op_res, curr_val, right_temp); break;
                        case TK_AND_ASSIGN: ir_emit_and(op_res, curr_val, right_temp); break;
                        case TK_OR_ASSIGN:  ir_emit_or(op_res, curr_val, right_temp); break;
                        case TK_XOR_ASSIGN: ir_emit_xor(op_res, curr_val, right_temp); break;
                        case TK_SHL_ASSIGN: ir_emit_shl(op_res, curr_val, right_temp); break;
                        case TK_SHR_ASSIGN: ir_emit_shr(op_res, curr_val, right_temp); break;
                        default: op_res = right_temp; break;
                    }
                    final_val = op_res;
                }
                if (c->last_expr_is_float) {
                    if (p_size == 4) ir_emit_vstoress((uint16_t)ptr_temp, (uint16_t)final_val);
                    else ir_emit_vstoresd((uint16_t)ptr_temp, (uint16_t)final_val);
                    c->last_expr_is_float = 1;
                } else if (p_size <= 8) {
                    if (p_size == 1) ir_emit_store_byte(final_val, ptr_temp);
                    else if (p_size == 2) ir_emit_store_16(final_val, ptr_temp);
                    else if (p_size == 4) ir_emit_store_32(final_val, ptr_temp);
                    else ir_emit_store_64(final_val, ptr_temp);
                    c->last_expr_is_float = 0;
                } else if (p_size > 8 && !is_compound) {
                    zero_cc_emit_struct_copy_to_addr(c, arena, ptr_temp, p_size, node->right, right_temp);
                    c->last_expr_is_float = 0;
                }
                if (!c->last_expr_is_float) {
                    lower_extend_narrow(c, final_val, target_ti);
                }
                return final_val;
            }

            int right_temp = lower_expr(c, arena, node->right);
            int right_is_float = c->last_expr_is_float;

            /* C11 6.5.16: pointer compound assignment (p += n / p -= n)
             * scales by sizeof(pointee), like pointer +/- arithmetic.  The
             * plain-assignment forms (p = e) get their scaling from the BINOP
             * lowering of the RHS; compound forms have a bare RHS. */
            if (is_compound && (node->aux.op == TK_ADD_ASSIGN || node->aux.op == TK_SUB_ASSIGN)) {
                TypeInfo *lvti = ast_node_get_typeinfo(c, left_node);
                if (lvti && (type_is_pointer(lvti) || lvti->kind == TYPE_KIND_ARRAY)) {
                    size_t psz = type_size(lvti->kind == TYPE_KIND_ARRAY ?
                        lvti->details.array.base : lvti->details.pointer.base);
                    if (psz > 1) {
                        int si = c->next_temp_id++;
                        ir_emit_mov_imm(si, (int32_t)psz);
                        int s = c->next_temp_id++;
                        ir_emit_mul(s, right_temp, si);
                        right_temp = s;
                    }
                }
            }

            if (left_node && left_node->tag == NODE_MEMBER) {
                /* Struct member assignment: traverse chain of member accesses */
                int32_t member_offset = 0;
                ZeroCCASTNode *curr = left_node;
                ZeroCCASTNode *innermost = left_node;
                int chain_depth = 0;
                ASTRef base_ref = node->left;
                while (curr && curr->tag == NODE_MEMBER) {
                    member_offset += (int32_t)curr->aux.alt;
                    chain_depth++;
                    innermost = curr;
                    base_ref = curr->left;
                    curr = zero_cc_ast_get(arena, curr->left);
                }
                ZeroCCASTNode *base_node = curr;
                /* The member node directly above the base decides whether the
                 * base is a pointer (->) or a struct object (.). */
                bool base_is_ptr = (innermost && (innermost->flags & NODE_MEMBER_IS_ARROW)) != 0;
                int32_t base_stack_offset = 0;

                /* Multi-segment chains (o.p->v = x): compute the address
                 * generically - the flat paths below cannot dereference
                 * intermediate pointer members (C5). */
                if (chain_depth >= 2) {
                    int addr = lower_member_chain_addr(c, arena, left_node);
                    if (addr == TEMP_NONE) return TEMP_NONE;
                    if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                        return assign_member_bitfield(c, node, left_node, addr, right_temp);
                    }
                    size_t m_size = 4;
                    TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                    if (member_ti) m_size = type_size(member_ti);
                    bool is_flt = (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float;
                    if (is_compound) {
                        if (is_flt) {
                            int lhs_val = c->next_temp_id++;
                            if (m_size == 4) ir_emit_vloadss((uint16_t)lhs_val, (uint16_t)addr);
                            else ir_emit_vloadsd((uint16_t)lhs_val, (uint16_t)addr);
                            int float_rhs = right_temp;
                            if (!c->last_expr_is_float) {
                                float_rhs = c->next_temp_id++;
                                ir_emit_cvtsi2sd((uint16_t)float_rhs, (uint16_t)right_temp);
                            }
                            int op_res = c->next_temp_id++;
                            switch (node->aux.op) {
                                case TK_ADD_ASSIGN: ir_emit_fadd((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
                                case TK_SUB_ASSIGN: ir_emit_fsub((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
                                case TK_MUL_ASSIGN: ir_emit_fmul((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
                                case TK_DIV_ASSIGN: ir_emit_fdiv((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
                                default: op_res = float_rhs; break;
                            }
                            right_temp = op_res;
                            c->last_expr_is_float = 1;
                        } else {
                            int curr_val = c->next_temp_id++;
                            if (m_size == 1) ir_emit_load_byte(curr_val, addr);
                            else if (m_size == 2) ir_emit_load_16(curr_val, addr);
                            else if (m_size == 4) ir_emit_load_32(curr_val, addr);
                            else ir_emit_deref(curr_val, addr);
                            int op_res = c->next_temp_id++;
                            switch (node->aux.op) {
                                case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, right_temp); break;
                                case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, right_temp); break;
                                case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, right_temp); break;
                                case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, right_temp); break;
                                case TK_MOD_ASSIGN: ir_emit_mod(op_res, curr_val, right_temp); break;
                                case TK_AND_ASSIGN: ir_emit_and(op_res, curr_val, right_temp); break;
                                case TK_OR_ASSIGN:  ir_emit_or(op_res, curr_val, right_temp); break;
                                case TK_XOR_ASSIGN: ir_emit_xor(op_res, curr_val, right_temp); break;
                                case TK_SHL_ASSIGN: ir_emit_shl(op_res, curr_val, right_temp); break;
                                case TK_SHR_ASSIGN: ir_emit_shr(op_res, curr_val, right_temp); break;
                                default: ir_emit_mov_reg(op_res, right_temp); break;
                            }
                            right_temp = op_res;
                        }
                    }
                    if (m_size > 8 && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, addr, m_size, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                    if (c->last_expr_is_float) {
                        if (m_size == 4) ir_emit_vstoress(addr, (uint16_t)right_temp);
                        else ir_emit_vstoresd(addr, (uint16_t)right_temp);
                        c->last_expr_is_float = 1;
                    } else {
                        if (m_size == 1) ir_emit_store_byte(right_temp, addr);
                        else if (m_size == 2) ir_emit_store_16(right_temp, addr);
                        else if (m_size == 4) ir_emit_store_32(right_temp, addr);
                        else ir_emit_store_64(right_temp, addr);
                        c->last_expr_is_float = 0;
                    }
                    return right_temp;
                }

                // Find the base variable's stack offset if it's a local identifier
                if (base_node && base_node->tag == NODE_IDENT) {
                    ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
                    ZeroCCGlobalVar *ggv = sym ? NULL : find_global_var(base_node->s.str);
                    if (ggv) {
                        /* Global struct/union or pointer base: `g.member = val` or `p->member = val`. */
                        int gaddr = c->next_temp_id++;
                        ir_emit_global_addr(c, gaddr, ggv);
                        if (base_is_ptr) {
                            int ptr_temp = c->next_temp_id++;
                            ir_emit_deref(ptr_temp, gaddr);
                            gaddr = ptr_temp;
                        }
                        int addr_temp = gaddr;
                        if (member_offset != 0) {
                            int off_temp = c->next_temp_id++;
                            ir_emit_mov_imm(off_temp, member_offset);
                            addr_temp = c->next_temp_id++;
                            ir_emit_add(addr_temp, gaddr, off_temp);
                        }
                        if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                            return assign_member_bitfield(c, node, left_node, addr_temp, right_temp);
                        }
                        size_t m_size = 4;
                        TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                        if (member_ti) m_size = type_size(member_ti);
                        if (is_compound)
                            member_apply_compound(c, node->aux.op, addr_temp, &right_temp,
                                                   m_size,
                                                   (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float);
                        if (m_size > 8 && !is_compound) {
                            zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, m_size, node->right, right_temp);
                            c->last_expr_is_float = 0;
                            return right_temp;
                        }
                        if (right_is_float || c->last_expr_is_float) {
                            if (m_size == 4) {
                                ir_emit_vstoress(addr_temp, (uint16_t)right_temp);
                            } else {
                                ir_emit_vstoresd(addr_temp, (uint16_t)right_temp);
                            }
                            c->last_expr_is_float = 1;
                        } else {
                            if (m_size == 1) {
                                ir_emit_store_byte(right_temp, addr_temp);
                            } else if (m_size == 2) {
                                ir_emit_store_16(right_temp, addr_temp);
                            } else if (m_size == 4) {
                                ir_emit_store_32(right_temp, addr_temp);
                            } else {
                                ir_emit_store_64(right_temp, addr_temp);
                            }
                            c->last_expr_is_float = 0;
                        }
                        return right_temp;
                    }
                    if (!sym) {
                        diag_report_error_at_token(c, "undefined local variable '%s' in struct assignment", base_node->s.str);
                        return TEMP_NONE;
                    }
                    if (base_is_ptr) {
                        /* p->member = val: store through the pointer */
                        int ptr_temp = c->next_temp_id++;
                        ir_emit_load(ptr_temp, BE(c)->fp_reg, -sym->stack_offset);
                        int addr_temp = ptr_temp;
                        if (member_offset != 0) {
                            int off_temp = c->next_temp_id++;
                            ir_emit_mov_imm(off_temp, member_offset);
                            addr_temp = c->next_temp_id++;
                            ir_emit_add(addr_temp, ptr_temp, off_temp);
                        }
                        if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                            return assign_member_bitfield(c, node, left_node, addr_temp, right_temp);
                        }
                        size_t m_size = 4;
                        TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                        if (member_ti) m_size = type_size(member_ti);
                        if (is_compound)
                            member_apply_compound(c, node->aux.op, addr_temp, &right_temp,
                                                   m_size,
                                                   (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float);
                        if (m_size > 8 && !is_compound) {
                            zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, m_size, node->right, right_temp);
                            c->last_expr_is_float = 0;
                            return right_temp;
                        }
                        if (right_is_float || c->last_expr_is_float) {
                            if (m_size == 4) {
                                ir_emit_vstoress(addr_temp, (uint16_t)right_temp);
                            } else {
                                ir_emit_vstoresd(addr_temp, (uint16_t)right_temp);
                            }
                            c->last_expr_is_float = 1;
                        } else {
                            if (m_size == 1) {
                                ir_emit_store_byte(right_temp, addr_temp);
                            } else if (m_size == 2) {
                                ir_emit_store_16(right_temp, addr_temp);
                            } else if (m_size == 4) {
                                ir_emit_store_32(right_temp, addr_temp);
                            } else {
                                ir_emit_store_64(right_temp, addr_temp);
                            }
                            c->last_expr_is_float = 0;
                        }
                        return right_temp;
                    }
                    if (sym->is_const || sym->type.is_const) {
                        diag_report_error_at_token(c, "cannot assign to member of const-qualified struct '%s'", base_node->s.str);
                        return TEMP_NONE;
                    }
                    TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                    if (member_ti && (member_ti->modifiers & TYPE_MOD_CONST)) {
                        diag_report_error_at_token(c, "cannot assign to const-qualified member");
                        return TEMP_NONE;
                    }
                    for (size_t m_i = 0; m_i < sym->type.member_count; m_i++) {
                        if (sym->type.members[m_i].offset == (size_t)member_offset) {
                            if (sym->type.members[m_i].is_const) {
                                diag_report_error_at_token(c, "cannot assign to const-qualified member '%s'", sym->type.members[m_i].name);
                                return TEMP_NONE;
                            }
                            break;
                        }
                    }
                    base_stack_offset = -sym->stack_offset;
                } else if (base_node && base_node->tag == NODE_COMPOUND_LIT) {
                    (void)lower_expr(c, arena, base_ref);
                    ZeroCCSym *sym = lowering_local_sym(c, base_node, base_node->s.str); /* index-based: name lookup misses post-bound __clit syms */
                    if (sym) {
                        base_stack_offset = -sym->stack_offset;
                    }
                } else {
                    /* General base expression evaluating to struct pointer/address (e.g. p[i][j]) */
                    int base_ptr = TEMP_NONE;
        if (!base_is_ptr) {
            base_ptr = zero_cc_lower_lvalue_addr(c, arena, base_ref);
        }
        if (base_ptr == TEMP_NONE) {
            base_ptr = lower_expr(c, arena, base_ref);
        }
                    int addr_temp = base_ptr;
                    if (member_offset != 0) {
                        int off_temp = c->next_temp_id++;
                        ir_emit_mov_imm(off_temp, member_offset);
                        addr_temp = c->next_temp_id++;
                        ir_emit_add(addr_temp, base_ptr, off_temp);
                    }
                    size_t m_size = 4;
                    TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                    if (member_ti) m_size = type_size(member_ti);
                    if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                        return assign_member_bitfield(c, node, left_node, addr_temp, right_temp);
                    }
                    if (is_compound)
                        member_apply_compound(c, node->aux.op, addr_temp, &right_temp,
                                               m_size,
                                               (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float);
                    if (m_size > 8 && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, m_size, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                    if (right_is_float || c->last_expr_is_float) {
                        if (m_size == 4) {
                            ir_emit_vstoress(addr_temp, (uint16_t)right_temp);
                        } else {
                            ir_emit_vstoresd(addr_temp, (uint16_t)right_temp);
                        }
                        c->last_expr_is_float = 1;
                    } else {
                        if (m_size == 1) {
                            ir_emit_store_byte(right_temp, addr_temp);
                        } else if (m_size == 2) {
                            ir_emit_store_16(right_temp, addr_temp);
                        } else if (m_size == 4) {
                            ir_emit_store_32(right_temp, addr_temp);
                        } else {
                            ir_emit_store_64(right_temp, addr_temp);
                        }
                        c->last_expr_is_float = 0;
                    }
                    return right_temp;
                }

                int32_t total_offset = base_stack_offset + member_offset;

                if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, total_offset);
                    return assign_member_bitfield(c, node, left_node, addr_temp, right_temp);
                }

                /* Compound assignment on a plain (non-bitfield) single-segment
                 * member: load the current member value width-correct, apply
                 * the op, then the store below writes the combined result.
                 * (C7 - previously only the RHS was stored, so `s.w += 5`
                 * stored 5.) */
                if (is_compound) {
                    int mc_addr = c->next_temp_id++;
                    ir_emit_lea(mc_addr, TEMP_NONE, total_offset);
                    TypeInfo *mc_ti = ast_node_get_typeinfo(c, left_node);
                    size_t mc_size = mc_ti ? type_size(mc_ti) : 4;
                    member_apply_compound(c, node->aux.op, mc_addr, &right_temp, mc_size,
                                           (mc_ti && type_is_floating(mc_ti)) || c->last_expr_is_float);
                }

                if (right_is_float || c->last_expr_is_float) {
                    /* Float value: store via IR. A 4-byte float member must
                     * use the width-correct VSTORESS - the 8-byte FSTORE
                     * writes the raw double bits and clobbers the bytes
                     * following the member (adjacent-member destruction). */
                    size_t m_size_f = 4;
                    TypeInfo *member_ti_f = ast_node_get_typeinfo(c, left_node);
                    if (member_ti_f) m_size_f = type_size(member_ti_f);
                    if (m_size_f == 4) {
                        int a4 = c->next_temp_id++;
                        ir_emit_lea(a4, TEMP_NONE, total_offset);
                        ir_emit_vstoress((uint16_t)a4, (uint16_t)right_temp);
                    } else {
                        ir_emit_fstore((uint16_t)right_temp, TEMP_NONE, total_offset); /* TEMP_NONE = frame-pointer base */
                    }
                    c->last_expr_is_float = 1;
                    return right_temp;
                } else {
                    /* Integer/pointer value in GPR - width-correct store
                     * (a 64-bit store into a 1/2/4-byte member clobbers the
                     * adjacent member). */
                    TypeInfo *mc_ti2 = ast_node_get_typeinfo(c, left_node);
                    size_t mc_size2 = mc_ti2 ? type_size(mc_ti2) : 8;
                    int mc_addr2 = c->next_temp_id++;
                    ir_emit_lea(mc_addr2, TEMP_NONE, total_offset);
                    if (mc_size2 > 8 && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, mc_addr2, mc_size2, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                    if (mc_size2 == 1) ir_emit_store_byte((uint16_t)right_temp, (uint16_t)mc_addr2);
                    else if (mc_size2 == 2) ir_emit_store_16((uint16_t)right_temp, (uint16_t)mc_addr2);
                    else if (mc_size2 == 4) ir_emit_store_32((uint16_t)right_temp, (uint16_t)mc_addr2);
                    else ir_emit_store_64((uint16_t)right_temp, (uint16_t)mc_addr2);
                    c->last_expr_is_float = 0;
                    return right_temp;
                }
            }

            if (left_node && left_node->tag == NODE_INDEX) {
                /* Array element assignment: arr[idx] = expr or arr[idx] op= expr */
                int array_temp = lower_expr(c, arena, left_node->left);
                int index_temp = lower_expr(c, arena, left_node->right);

                size_t elem_size = 4;
                TypeInfo *elem_ti = ast_node_get_typeinfo(c, left_node);
                if (elem_ti) {
                    elem_size = type_size(elem_ti);
                } else {
                    ZeroCCASTNode *ln = zero_cc_ast_get(arena, left_node->left);
                    if (ln && ln->tag == NODE_IDENT) {
                        const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                        if (zt && zt->base && zt->base->size > 0) {
                            elem_size = zt->base->size;
                        }
                    } else if (ln && ln->tag == NODE_COMPOUND_LIT) {
                        ZeroCCSym *sym = lowering_local_sym(c, ln, ln->s.str); /* index-based: name lookup misses post-bound __clit syms */
                        if (sym && sym->type.base && sym->type.base->size > 0) {
                            elem_size = sym->type.base->size;
                        }
                    }
                }
                if (elem_size == 0) elem_size = 4;

                int scaled_index;
                if (elem_size == 1) {
                    scaled_index = index_temp;
                } else if (elem_size == 2 || elem_size == 4 || elem_size == 8) {
                    int shift = (elem_size == 2) ? 1 : ((elem_size == 4) ? 2 : 3);
                    int sh_temp = c->next_temp_id++;
                    ir_emit_mov_imm(sh_temp, shift);
                    scaled_index = c->next_temp_id++;
                    ir_emit_shl(scaled_index, index_temp, sh_temp);
                } else {
                    int sz_temp = c->next_temp_id++;
                    ir_emit_mov_imm(sz_temp, (int32_t)elem_size);
                    scaled_index = c->next_temp_id++;
                    ir_emit_mul(scaled_index, index_temp, sz_temp);
                }

                int addr_temp = c->next_temp_id++;
                ir_emit_add(addr_temp, array_temp, scaled_index);

                if (is_compound) {
                    int curr_val = c->next_temp_id++;
                    if (elem_size == 1) {
                        ir_emit_load_byte(curr_val, addr_temp);
                    } else if (elem_size == 2) {
                        ir_emit_load_16(curr_val, addr_temp);
                    } else if (elem_size == 4) {
                        ir_emit_load_32(curr_val, addr_temp);
                    } else {
                        ir_emit_deref(curr_val, addr_temp);
                    }
                    int op_res = c->next_temp_id++;
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, right_temp); break;
                        case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, right_temp); break;
                        case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, right_temp); break;
                        case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, right_temp); break;
                        case TK_MOD_ASSIGN: ir_emit_mod(op_res, curr_val, right_temp); break;
                        case TK_AND_ASSIGN: ir_emit_and(op_res, curr_val, right_temp); break;
                        case TK_OR_ASSIGN:  ir_emit_or(op_res, curr_val, right_temp); break;
                        case TK_XOR_ASSIGN: ir_emit_xor(op_res, curr_val, right_temp); break;
                        case TK_SHL_ASSIGN: ir_emit_shl(op_res, curr_val, right_temp); break;
                        case TK_SHR_ASSIGN: ir_emit_shr(op_res, curr_val, right_temp); break;
                        default: ir_emit_mov_reg(op_res, right_temp); break;
                    }
                    right_temp = op_res;
                }

                bool is_elem_float = false;
                if (elem_ti && type_is_floating(elem_ti)) {
                    is_elem_float = true;
                } else {
                    ZeroCCASTNode *ln = zero_cc_ast_get(arena, left_node->left);
                    if (ln && ln->tag == NODE_IDENT) {
                        const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                        if (zt && zt->base && (zt->base->tag == TYPE_FLOAT || zt->base->tag == TYPE_DOUBLE)) {
                            is_elem_float = true;
                        }
                    }
                }

                if (is_elem_float) {
                    if (!right_is_float) {
                        int flt_temp = c->next_temp_id++;
                        ir_emit_cvtsi2sd((uint16_t)flt_temp, (uint16_t)right_temp);
                        right_temp = flt_temp;
                    }
                    if (elem_size == 4) {
                        ir_emit_vstoress(addr_temp, (uint16_t)right_temp);
                    } else {
                        ir_emit_vstoresd(addr_temp, (uint16_t)right_temp);
                    }
                    c->last_expr_is_float = 1;
                } else {
                    bool is_elem_struct = (elem_ti && (elem_ti->kind == TYPE_KIND_STRUCT || elem_ti->kind == TYPE_KIND_UNION));
                    if (!is_elem_struct) {
                        ZeroCCASTNode *ln = zero_cc_ast_get(arena, left_node->left);
                        if (ln && ln->tag == NODE_IDENT) {
                            const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                            if (zt && zt->base && (zt->base->tag == TYPE_STRUCT || zt->base->tag == TYPE_UNION)) {
                                is_elem_struct = true;
                            }
                        }
                    }
                    if (is_elem_struct && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, elem_size, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                    if (elem_size == 1) {
                        ir_emit_store_byte(right_temp, addr_temp);
                    } else if (elem_size == 2) {
                        ir_emit_store_16(right_temp, addr_temp);
                    } else if (elem_size == 4) {
                        ir_emit_store_32(right_temp, addr_temp);
                    } else {
                        ir_emit_store_64(right_temp, addr_temp);
                    }
                    c->last_expr_is_float = 0;
                }
                return right_temp;
            }

            const char *var = left_node ? left_node->s.str : "";
            ZeroCCSym *sym = zero_cc_find_local(c, var);
            if (sym && !sym->is_extern) {
                if (sym->is_const || sym->type.is_const) {
                    diag_report_error_at_token(c, "cannot assign to const-qualified variable '%s'", var);
                    return TEMP_NONE;
                }
                bool target_is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
                if (target_is_float) {
                    int float_temp = right_temp;
                    if (!right_is_float) {
                        /* Convert int to float via IR */
                        float_temp = c->next_temp_id++;
                        ir_emit_cvtsi2sd((uint16_t)float_temp, (uint16_t)right_temp);
                    }
                    /* For compound assignment, load current value, operate, then store */
                    if (is_compound) {
                        int lhs_temp = c->next_temp_id++;
                        if (sym->type.tag == TYPE_FLOAT) {
                            int addr = c->next_temp_id++;
                            ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
                            ir_emit_vloadss((uint16_t)lhs_temp, (uint16_t)addr);
                        } else {
                            ir_emit_fload((uint16_t)lhs_temp, TEMP_NONE, -sym->stack_offset);
                        }
                        int op_res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_fdiv((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                            default: op_res = float_temp; break;
                        }
                        if (sym->type.tag == TYPE_FLOAT) {
                            int addr = c->next_temp_id++;
                            ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
                            ir_emit_vstoress((uint16_t)addr, (uint16_t)op_res);
                        } else {
                            ir_emit_fstore((uint16_t)op_res, TEMP_NONE, -sym->stack_offset);
                        }
                        c->last_expr_is_float = 1;
                        return op_res;
                    } else {
                        /* Simple assignment: just store */
                        if (sym->type.tag == TYPE_FLOAT) {
                            int addr = c->next_temp_id++;
                            ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
                            ir_emit_vstoress((uint16_t)addr, (uint16_t)float_temp);
                        } else {
                            ir_emit_fstore((uint16_t)float_temp, TEMP_NONE, -sym->stack_offset);
                        }
                        c->last_expr_is_float = 1;
                        return float_temp;
                    }
                } else {
                    if (c->last_expr_is_float) {
                        /* Convert float to int via IR */
                        int int_temp = c->next_temp_id++;
                        ir_emit_cvttsd2si((uint16_t)int_temp, (uint16_t)right_temp);
                        if (is_compound) {
                            /* Load current value and perform operation */
                            int left_temp = c->next_temp_id++;
                            ir_emit_load(left_temp, BE(c)->fp_reg, -sym->stack_offset);
                            switch (node->aux.op) {
                                case TK_ADD_ASSIGN: ir_emit_add(int_temp, left_temp, int_temp); break;
                                case TK_SUB_ASSIGN: ir_emit_sub(int_temp, left_temp, int_temp); break;
                                case TK_MUL_ASSIGN: ir_emit_mul(int_temp, left_temp, int_temp); break;
                                case TK_DIV_ASSIGN: ir_emit_div(int_temp, left_temp, int_temp); break;
                                case TK_MOD_ASSIGN: ir_emit_mod(int_temp, left_temp, int_temp); break;
                                case TK_AND_ASSIGN: ir_emit_and(int_temp, left_temp, int_temp); break;
                                case TK_OR_ASSIGN:  ir_emit_or(int_temp, left_temp, int_temp); break;
                                case TK_XOR_ASSIGN: ir_emit_xor(int_temp, left_temp, int_temp); break;
                                case TK_SHL_ASSIGN: ir_emit_shl(int_temp, left_temp, int_temp); break;
                                case TK_SHR_ASSIGN: ir_emit_shr(int_temp, left_temp, int_temp); break;
                            }
                        }
                        ir_emit_store((uint16_t)int_temp, BE(c)->fp_reg, -sym->stack_offset);
                        c->last_expr_is_float = 0;
                        return int_temp;
                    } else {
                        if (sym && (sym->is_atomic || sym->type.is_atomic)) {
                            int addr_temp = c->next_temp_id++;
                            ir_emit_lea(addr_temp, TEMP_NONE, -sym->stack_offset);
                            int width = (sym->type.size > 0) ? (int)sym->type.size : 4;
                            if (is_compound && (node->aux.op == TK_ADD_ASSIGN || node->aux.op == TK_SUB_ASSIGN)) {
                                int delta_temp = right_temp;
                                if (node->aux.op == TK_SUB_ASSIGN) {
                                    delta_temp = c->next_temp_id++;
                                    ir_emit_neg(delta_temp, right_temp);
                                }
                                int delta_copy = c->next_temp_id++;
                                ir_emit_mov_reg(delta_copy, delta_temp);
                                int old_temp = c->next_temp_id++;
                                ir_emit_atomic_xadd(old_temp, addr_temp, delta_temp, width);
                                int new_temp = c->next_temp_id++;
                                ir_emit_add(new_temp, old_temp, delta_copy);
                                c->last_expr_is_float = 0;
                                return new_temp;
                            } else if (!is_compound) {
                                int old_temp = c->next_temp_id++;
                                ir_emit_atomic_xchg(old_temp, addr_temp, right_temp, width);
                                c->last_expr_is_float = 0;
                                return right_temp;
                            }
                        }

                        if (is_compound) {
                            /* Load current value and perform operation.
                             * The result goes into a fresh temp: right_temp may
                             * hold a loop-invariant constant that must survive
                             * repeated uses, so it must not be used as dest. */
                            int left_temp = c->next_temp_id++;
                            ir_emit_load(left_temp, BE(c)->fp_reg, -sym->stack_offset);
                            int op_res = c->next_temp_id++;
                            switch (node->aux.op) {
                                case TK_ADD_ASSIGN: ir_emit_add(op_res, left_temp, right_temp); break;
                                case TK_SUB_ASSIGN: ir_emit_sub(op_res, left_temp, right_temp); break;
                                case TK_MUL_ASSIGN: ir_emit_mul(op_res, left_temp, right_temp); break;
                                case TK_DIV_ASSIGN:
                                    ir_emit_div(op_res, left_temp, right_temp);
                                    if (zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) ir_mark_last_unsigned(1);
                                    break;
                                case TK_MOD_ASSIGN:
                                    ir_emit_mod(op_res, left_temp, right_temp);
                                    if (zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) ir_mark_last_unsigned(1);
                                    break;
                                case TK_AND_ASSIGN: ir_emit_and(op_res, left_temp, right_temp); break;
                                case TK_OR_ASSIGN:  ir_emit_or(op_res, left_temp, right_temp); break;
                                case TK_XOR_ASSIGN: ir_emit_xor(op_res, left_temp, right_temp); break;
                                case TK_SHL_ASSIGN: ir_emit_shl(op_res, left_temp, right_temp); break;
                                case TK_SHR_ASSIGN: ir_emit_shr(op_res, left_temp, right_temp); break;
                            }
                            right_temp = op_res;
                        }
                        if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 16 && !is_compound) {
                            int dst_addr = c->next_temp_id++;
                            ir_emit_lea(dst_addr, TEMP_NONE, -sym->stack_offset);
                            zero_cc_emit_struct_copy_to_addr(c, arena, dst_addr, sym->type.size, node->right, right_temp);
                        } else {
                            bool rhs_is_struct_addr = (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) &&
                                                      !is_compound && ast_expr_is_struct_addr(c, arena, node->right);
                            if (rhs_is_struct_addr) {
                                if (sym->type.size <= 8) {
                                    int val_temp = c->next_temp_id++;
                                    if (sym->type.size == 1) ir_emit_load_byte(val_temp, right_temp);
                                    else if (sym->type.size == 2) ir_emit_load_16(val_temp, right_temp);
                                    else if (sym->type.size == 4) ir_emit_load_32(val_temp, right_temp);
                                    else ir_emit_deref(val_temp, right_temp);
                                    ir_emit_store(val_temp, BE(c)->fp_reg, -sym->stack_offset);
                                } else {
                                    int val1 = c->next_temp_id++;
                                    ir_emit_deref(val1, right_temp);
                                    ir_emit_store(val1, BE(c)->fp_reg, -sym->stack_offset);

                                    int off8 = c->next_temp_id++;
                                    ir_emit_mov_imm(off8, 8);
                                    int addr2 = c->next_temp_id++;
                                    ir_emit_add(addr2, right_temp, off8);

                                    int val2 = c->next_temp_id++;
                                    size_t rem = sym->type.size - 8;
                                    if (rem == 1) ir_emit_load_byte(val2, addr2);
                                    else if (rem == 2) ir_emit_load_16(val2, addr2);
                                    else if (rem == 4) ir_emit_load_32(val2, addr2);
                                    else ir_emit_deref(val2, addr2);
                                    ir_emit_store(val2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                                }
                            } else {
                                ir_emit_store(right_temp, BE(c)->fp_reg, -sym->stack_offset);
                                if (c->isa == ISA_RISCV32 && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 4 && sym->type.size <= 8 && !is_compound) {
                                    /* RV32 ILP32 ABI: second 4-byte chunk returned in a1 (VREG_2) */
                                    ir_emit_store(VREG_2, BE(c)->fp_reg, -(sym->stack_offset - 4));
                                } else if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16 && !is_compound) {
                                    int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? 1 : 2;
                                    size_t rem = sym->type.size - 8;
                                    int32_t tail_off = -(sym->stack_offset - 8);
                                    if (rem >= 8) {
                                        ir_emit_store(reg2, BE(c)->fp_reg, tail_off);
                                    } else {
                                        int dst_addr = c->next_temp_id++;
                                        ir_emit_lea(dst_addr, TEMP_NONE, tail_off);
                                        if (rem >= 4) ir_emit_store_32(reg2, dst_addr);
                                        else if (rem >= 2) ir_emit_store_16(reg2, dst_addr);
                                        else ir_emit_store_byte(reg2, dst_addr);
                                    }
                                }
                            }
                        }
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                }
            } else {
                ZeroCCGlobalVar *gv = find_global_var(var);
                if (gv) {
                    int gaddr_temp = c->next_temp_id++;
                    ir_emit_global_addr(c, gaddr_temp, gv);
                    int final_val = right_temp;
                    size_t gv_sz = gv->size > 0 ? gv->size : 4;
                    bool gv_is_float = (gv->type.tag == TYPE_FLOAT);
                    bool gv_is_double = (gv->type.tag == TYPE_DOUBLE);
                    bool gv_is_flt = gv_is_float || gv_is_double;
                    if (gv_is_flt && c->last_expr_is_float) {
                        /* Float/double global: keep the value in the FP pipeline
                         * (an integer load/add/store on the raw bits would be
                         * arithmetic on the IEEE-754 pattern, not the value). */
                        if (is_compound) {
                            int curr_val = c->next_temp_id++;
                            if (gv_is_float) ir_emit_vloadss((uint16_t)curr_val, (uint16_t)gaddr_temp);
                            else ir_emit_vloadsd((uint16_t)curr_val, (uint16_t)gaddr_temp);
                            int op_res = c->next_temp_id++;
                            switch (node->aux.op) {
                                case TK_ADD_ASSIGN: ir_emit_fadd(op_res, curr_val, right_temp); break;
                                case TK_SUB_ASSIGN: ir_emit_fsub(op_res, curr_val, right_temp); break;
                                case TK_MUL_ASSIGN: ir_emit_fmul(op_res, curr_val, right_temp); break;
                                case TK_DIV_ASSIGN: ir_emit_fdiv(op_res, curr_val, right_temp); break;
                                default: ir_emit_fmov(op_res, right_temp); break; /* bit-ops are invalid on floats */
                            }
                            final_val = op_res;
                        }
                        if (gv_is_float) ir_emit_vstoress((uint16_t)gaddr_temp, (uint16_t)final_val);
                        else ir_emit_vstoresd((uint16_t)gaddr_temp, (uint16_t)final_val);
                        c->last_expr_is_float = 1;
                        return final_val;
                    }
                    if (gv_is_flt) {
                        /* int RHS -> float global: convert, then store */
                        int flt_temp = c->next_temp_id++;
                        ir_emit_cvtsi2sd((uint16_t)flt_temp, (uint16_t)right_temp);
                        if (gv_is_float) ir_emit_vstoress(gaddr_temp, (uint16_t)flt_temp);
                        else ir_emit_vstoresd(gaddr_temp, (uint16_t)flt_temp);
                        c->last_expr_is_float = 1;
                        return flt_temp;
                    }
                    if (is_compound) {
                        int curr_val = c->next_temp_id++;
                        if (gv_sz == 1) ir_emit_load_byte(curr_val, gaddr_temp);
                        else if (gv_sz == 2) ir_emit_load_16(curr_val, gaddr_temp);
                        else if (gv_sz == 4) ir_emit_load_32(curr_val, gaddr_temp);
                        else ir_emit_deref(curr_val, gaddr_temp);

                        int op_res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, right_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, right_temp); break;
                            case TK_MOD_ASSIGN: ir_emit_mod(op_res, curr_val, right_temp); break;
                            case TK_AND_ASSIGN: ir_emit_and(op_res, curr_val, right_temp); break;
                            case TK_OR_ASSIGN:  ir_emit_or(op_res, curr_val, right_temp); break;
                            case TK_XOR_ASSIGN: ir_emit_xor(op_res, curr_val, right_temp); break;
                            case TK_SHL_ASSIGN: ir_emit_shl(op_res, curr_val, right_temp); break;
                            case TK_SHR_ASSIGN: ir_emit_shr(op_res, curr_val, right_temp); break;
                            default: op_res = right_temp; break;
                        }
                        final_val = op_res;
                    }
                    if ((gv->type.tag == TYPE_STRUCT || gv->type.tag == TYPE_UNION) && gv_sz > 8 && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, gaddr_temp, gv_sz, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                    bool rhs_is_struct_addr = (gv->type.tag == TYPE_STRUCT || gv->type.tag == TYPE_UNION) &&
                                              !is_compound && ast_expr_is_struct_addr(c, arena, node->right);
                    if (rhs_is_struct_addr) {
                        int val_temp = c->next_temp_id++;
                        if (gv_sz == 1) ir_emit_load_byte(val_temp, final_val);
                        else if (gv_sz == 2) ir_emit_load_16(val_temp, final_val);
                        else if (gv_sz == 4) ir_emit_load_32(val_temp, final_val);
                        else ir_emit_deref(val_temp, final_val);
                        final_val = val_temp;
                    }
                    if (gv_sz == 1) ir_emit_store_byte(final_val, gaddr_temp);
                    else if (gv_sz == 2) ir_emit_store_16(final_val, gaddr_temp);
                    else if (gv_sz == 4) ir_emit_store_32(final_val, gaddr_temp);
                    else ir_emit_store_64(final_val, gaddr_temp);

                    c->last_expr_is_float = 0;
                    return final_val;
                }
                /* Extern object not defined in this TU (multi-TU / extern
                 * declaration): the parser registered it in the function
                 * table (is_external) - store through its address, mirroring
                 * the NODE_IDENT load path above. Previously this fell
                 * through and the store was silently dropped, so `g += 1`
                 * on a cross-TU global compiled to a no-op. */
                ZeroCCFunction *xfn = zero_cc_find_function(c, var);
                if (xfn && xfn->is_external) {
                    int xaddr = c->next_temp_id++;
                    ir_emit_mov_global(xaddr, xfn->name);
                    int xfinal = right_temp;
                    /* The extern registration stores the declared scalar
                     * type in return_type (same source the NODE_IDENT load
                     * path uses). */
                    const ZeroCCType *xt = &xfn->return_type;
                    size_t xsz = (xt->size > 0) ? xt->size : 8;
                    bool xflt = (xt->tag == TYPE_FLOAT || xt->tag == TYPE_DOUBLE);
                    if (xflt && c->last_expr_is_float) {
                        if (is_compound) {
                            int curr_val = c->next_temp_id++;
                            if (xsz == 4) ir_emit_vloadss((uint16_t)curr_val, (uint16_t)xaddr);
                            else ir_emit_vloadsd((uint16_t)curr_val, (uint16_t)xaddr);
                            int op_res = c->next_temp_id++;
                            switch (node->aux.op) {
                                case TK_ADD_ASSIGN: ir_emit_fadd(op_res, curr_val, right_temp); break;
                                case TK_SUB_ASSIGN: ir_emit_fsub(op_res, curr_val, right_temp); break;
                                case TK_MUL_ASSIGN: ir_emit_fmul(op_res, curr_val, right_temp); break;
                                case TK_DIV_ASSIGN: ir_emit_fdiv(op_res, curr_val, right_temp); break;
                                default: ir_emit_fmov(op_res, right_temp); break;
                            }
                            xfinal = op_res;
                        }
                        if (xsz == 4) ir_emit_vstoress((uint16_t)xaddr, (uint16_t)xfinal);
                        else ir_emit_vstoresd((uint16_t)xaddr, (uint16_t)xfinal);
                        c->last_expr_is_float = 1;
                        return xfinal;
                    }
                    if (xflt) {
                        int flt_temp = c->next_temp_id++;
                        ir_emit_cvtsi2sd((uint16_t)flt_temp, (uint16_t)right_temp);
                        if (xsz == 4) ir_emit_vstoress(xaddr, (uint16_t)flt_temp);
                        else ir_emit_vstoresd(xaddr, (uint16_t)flt_temp);
                        c->last_expr_is_float = 1;
                        return flt_temp;
                    }
                    if (is_compound) {
                        int curr_val = c->next_temp_id++;
                        if (xsz == 1) ir_emit_load_byte(curr_val, xaddr);
                        else if (xsz == 2) ir_emit_load_16(curr_val, xaddr);
                        else if (xsz == 4) ir_emit_load_32(curr_val, xaddr);
                        else ir_emit_deref(curr_val, xaddr);
                        int op_res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, right_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, right_temp); break;
                            case TK_MOD_ASSIGN: ir_emit_mod(op_res, curr_val, right_temp); break;
                            case TK_AND_ASSIGN: ir_emit_and(op_res, curr_val, right_temp); break;
                            case TK_OR_ASSIGN:  ir_emit_or(op_res, curr_val, right_temp); break;
                            case TK_XOR_ASSIGN: ir_emit_xor(op_res, curr_val, right_temp); break;
                            case TK_SHL_ASSIGN: ir_emit_shl(op_res, curr_val, right_temp); break;
                            case TK_SHR_ASSIGN: ir_emit_shr(op_res, curr_val, right_temp); break;
                            default: op_res = right_temp; break;
                        }
                        xfinal = op_res;
                    }
                    if (xsz == 1) ir_emit_store_byte(xfinal, xaddr);
                    else if (xsz == 2) ir_emit_store_16(xfinal, xaddr);
                    else if (xsz == 4) ir_emit_store_32(xfinal, xaddr);
                    else ir_emit_store_64(xfinal, xaddr);
                    c->last_expr_is_float = 0;
                    return xfinal;
                }
            }
            c->last_expr_is_float = 0;
            return right_temp;
        }

        case NODE_TERNARY: {
            /* Ternary (cond ? then : else) using standard IR.
             *
             * FP arms (Sprint 40): the result temp must be XMM-class, so
             * it must be DEFINED by an FP op (the allocator derives a
             * temp's register class from its defining opcode).  The
             * pre-Sprint-40 code always emitted a GPR MOV_REG and marked
             * the result non-float, so `c ? fa : fb` read an uninitialized
             * GPR and a trailing `(int)` cast lowered to SEXT32 instead
             * of CVTTSD2SI.  Mixed int/FP arms follow the C usual
             * arithmetic conversion: the int arm is cvtsi2sd'd.  The
             * then-arm move is emitted before the else arm is lowered, so
             * the else arm's floatness is predicted with ast_expr_is_fp
             * (conservative: unknown shapes stay on the old int path). */
            int cond_temp = lower_expr(c, arena, node->left);
            if (c->last_expr_is_float) {
                int zero_i = c->next_temp_id++;
                ir_emit_mov_imm(zero_i, 0);
                int zero_f = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)zero_f, (uint16_t)zero_i);
                int cmp_i = c->next_temp_id++;
                ir_emit_cmp(cond_temp, zero_f);
                ir_emit_cond_set(cmp_i, CC_NE);
                cond_temp = cmp_i;
                c->last_expr_is_float = 0;
            }
            int res_temp = c->next_temp_id++;
            uint16_t else_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;

            int else_may_be_fp = ast_expr_is_fp(c, arena, node->aux.alt);

            ir_emit_je((uint16_t)cond_temp, else_label);

            int then_temp = lower_expr(c, arena, node->right);
            int then_fp = c->last_expr_is_float;

            if (then_fp || else_may_be_fp) {
                if (then_fp) {
                    ir_emit_fmov((uint16_t)res_temp, (uint16_t)then_temp);
                } else {
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)then_temp);
                    ir_emit_fmov((uint16_t)res_temp, (uint16_t)conv);
                }
            } else {
                ir_emit_mov_reg(res_temp, then_temp);
            }
            ir_emit_jmp(end_label);

            ir_emit_label(else_label);
            int else_temp = lower_expr(c, arena, node->aux.alt);
            int else_fp = c->last_expr_is_float;
            int fp_result = then_fp || else_fp || else_may_be_fp;

            if (fp_result) {
                if (else_fp) {
                    ir_emit_fmov((uint16_t)res_temp, (uint16_t)else_temp);
                } else {
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)else_temp);
                    ir_emit_fmov((uint16_t)res_temp, (uint16_t)conv);
                }
            } else {
                ir_emit_mov_reg(res_temp, else_temp);
            }

            ir_emit_label(end_label);
            c->last_expr_is_float = fp_result;
            return res_temp;
        }

        case NODE_COMPOUND_LIT: {
            ZeroCCSym *sym = lowering_local_sym(c, node, node->s.str);
            if (!sym) {
                if (node->left != AST_NULL) return lower_expr(c, arena, node->left);
                return TEMP_NONE;
            }
            /* C99: zero-initialize entire object before designated/positional stores */
            size_t total_size = sym->type.size ? sym->type.size : 4;
            if (total_size > 0) {
                int zero_temp = c->next_temp_id++;
                ir_emit_mov_imm(zero_temp, 0);
                size_t offset = 0;
                while (offset < total_size) {
                    size_t chunk = total_size - offset;
                    if (chunk >= 8) chunk = 8;
                    else if (chunk >= 4) chunk = 4;
                    else if (chunk >= 2) chunk = 2;
                    else chunk = 1;
                    int32_t store_offset = -(sym->stack_offset - (int32_t)offset);
                    if (chunk == 8) {
                        ir_emit_store(zero_temp, BE(c)->fp_reg, store_offset);
                    } else if (chunk == 4) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, store_offset);
                        ir_emit_store_32(zero_temp, addr_temp);
                    } else if (chunk == 2) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, store_offset);
                        ir_emit_store_16(zero_temp, addr_temp);
                    } else {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, store_offset);
                        ir_emit_store_byte(zero_temp, addr_temp);
                    }
                    offset += chunk;
                }
            }
            ASTRef cur_ref = node->left;
            size_t idx = 0;
            /* C11 6.7.9: a string literal initializing a char-array member
             * is expanded by the parser into a run of byte nodes sharing the
             * member designator; track the fill position within that member. */
            int32_t desig_char_member = -1;
            size_t desig_char_pos = 0;
            while (cur_ref != AST_NULL) {
                ZeroCCASTNode *elem_node = zero_cc_ast_get(arena, cur_ref);
                if (!elem_node) break;
                int val_temp = lower_expr(c, arena, cur_ref);
                /* Sprint 44: elements stamped with an absolute placement by
                 * the brace-initializer parser (nested sub-aggregate
                 * designators, .member[i], [i..j] ranges) bypass the
                 * flat-leaf cursor logic below. */
                if ((elem_node->desig_idx & ZC_INIT_ABS_TAG) != 0) {
                    size_t abs_off = ZC_INIT_ABS_OFF(elem_node->desig_idx);
                    size_t abs_sz = ZC_INIT_ABS_SIZE(elem_node->desig_idx);
                    int32_t abs_store_off = -(sym->stack_offset - (int32_t)abs_off);
                    if (ZC_INIT_IS_BF(elem_node->desig_idx)) {
                        uint8_t boff = ZC_INIT_BF_BOFF(elem_node->desig_idx);
                        uint8_t bw = ZC_INIT_BF_BW(elem_node->desig_idx);
                        uint64_t vmask = (bw >= 64) ? ~0ULL : ((1ULL << bw) - 1);
                        uint64_t fmask = (bw >= 64) ? ~0ULL : (vmask << boff);
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, abs_store_off);
                        int cur_temp = c->next_temp_id++;
                        if (abs_sz == 1) ir_emit_load_byte(cur_temp, addr_temp);
                        else if (abs_sz == 2) ir_emit_load_16(cur_temp, addr_temp);
                        else if (abs_sz == 4) ir_emit_load_32(cur_temp, addr_temp);
                        else ir_emit_deref(cur_temp, addr_temp);
                        int cmask_temp = c->next_temp_id++;
                        ir_emit_mov_imm(cmask_temp, (int64_t)(~fmask));
                        int cleared_temp = c->next_temp_id++;
                        ir_emit_and(cleared_temp, cur_temp, cmask_temp);
                        int vmask_temp = c->next_temp_id++;
                        ir_emit_mov_imm(vmask_temp, (int64_t)vmask);
                        int mval_temp = c->next_temp_id++;
                        ir_emit_and(mval_temp, val_temp, vmask_temp);
                        if (boff > 0) {
                            int sh_temp = c->next_temp_id++;
                            ir_emit_mov_imm(sh_temp, (int32_t)boff);
                            int shval_temp = c->next_temp_id++;
                            ir_emit_shl(shval_temp, mval_temp, sh_temp);
                            mval_temp = shval_temp;
                        }
                        int new_temp = c->next_temp_id++;
                        ir_emit_or(new_temp, cleared_temp, mval_temp);
                        int saddr_temp = c->next_temp_id++;
                        ir_emit_lea(saddr_temp, TEMP_NONE, abs_store_off);
                        if (abs_sz == 1) ir_emit_store_byte(new_temp, saddr_temp);
                        else if (abs_sz == 2) ir_emit_store_16(new_temp, saddr_temp);
                        else if (abs_sz == 4) ir_emit_store_32(new_temp, saddr_temp);
                        else ir_emit_store(new_temp, BE(c)->fp_reg, abs_store_off);
                        cur_ref = elem_node->next;
                        continue;
                    }
                    bool abs_is_float = zc_type_offset_is_float(&sym->type, abs_off);
                    if (abs_is_float && !c->last_expr_is_float) {
                        int conv = c->next_temp_id++;
                        ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)val_temp);
                        val_temp = conv;
                        c->last_expr_is_float = 1;
                    } else if (!abs_is_float && c->last_expr_is_float) {
                        int conv = c->next_temp_id++;
                        ir_emit_cvttsd2si((uint16_t)conv, (uint16_t)val_temp);
                        val_temp = conv;
                        c->last_expr_is_float = 0;
                    }

                    if (abs_sz == 1) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, abs_store_off);
                        ir_emit_store_byte(val_temp, addr_temp);
                    } else if (abs_sz == 2) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, abs_store_off);
                        ir_emit_store_16(val_temp, addr_temp);
                    } else if (abs_sz == 4) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, abs_store_off);
                        if (abs_is_float) {
                            ir_emit_vstoress((uint16_t)addr_temp, (uint16_t)val_temp);
                            c->last_expr_is_float = 0;
                        } else {
                            ir_emit_store_32(val_temp, addr_temp);
                        }
                    } else {
                        if (abs_is_float) {
                            int addr_temp = c->next_temp_id++;
                            ir_emit_lea(addr_temp, TEMP_NONE, abs_store_off);
                            ir_emit_vstoresd((uint16_t)addr_temp, (uint16_t)val_temp);
                            c->last_expr_is_float = 0;
                        } else {
                            ir_emit_store(val_temp, BE(c)->fp_reg, abs_store_off);
                        }
                    }
                    cur_ref = elem_node->next;
                    continue;
                }
                size_t elem_offset = 0;
                size_t elem_size = 4;
                bool elem_is_float = false;
                if (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) {
                    if ((elem_node->flags & NODE_FLAG_DESIGNATED) != 0) {
                        size_t m_idx = (size_t)elem_node->desig_idx;
                        if (m_idx < sym->type.member_count) {
                            if (sym->type.members[m_idx].is_flexible) {
                                diag_report_error_at_token(c,
                                    "initialization of flexible array member '%s'",
                                    sym->type.members[m_idx].name);
                                return TEMP_NONE;
                            }
                            const ZeroCCStructMember *dm = &sym->type.members[m_idx];
                            if (dm->tag == TYPE_ARRAY && dm->base &&
                                (dm->base->tag == TYPE_INT8 || dm->base->tag == TYPE_UINT8)) {
                                /* Byte node of an expanded string literal: one
                                 * byte per node, consecutive within member. */
                                if ((int32_t)m_idx == desig_char_member) {
                                    desig_char_pos++;
                                } else {
                                    desig_char_member = (int32_t)m_idx;
                                    desig_char_pos = 0;
                                }
                                if (desig_char_pos >= dm->size) {
                                    cur_ref = elem_node->next; /* past member end */
                                    continue;
                                }
                                elem_offset = dm->offset + desig_char_pos;
                                elem_size = 1;
                            } else {
                                elem_offset = dm->offset;
                                elem_size = dm->size;
                                elem_is_float = (dm->tag == TYPE_FLOAT || dm->tag == TYPE_DOUBLE);
                            }
                        }
                    } else {
                        size_t cur_leaf = 0;
                        const char *flex_name = NULL;
                        if (!zc_struct_resolve_scalar_leaf(&sym->type, idx, &cur_leaf, 0, &elem_offset, &elem_size, &flex_name, &elem_is_float)) {
                            if (flex_name) {
                                diag_report_error_at_token(c,
                                    "initialization of flexible array member '%s'",
                                    flex_name);
                                return TEMP_NONE;
                            }
                        }
                    }
                } else if (sym->type.tag == TYPE_ARRAY) {
                    size_t a_idx = idx;
                    if ((elem_node->flags & NODE_FLAG_DESIGNATED) != 0) {
                        a_idx = (size_t)elem_node->desig_idx;
                        idx = a_idx;
                    }
                    size_t off, sz;
                    /* Positional elements are flattened by the parser to
                     * leaf level: place by leaf index.  Designated [idx]
                     * names an ELEMENT, so map it to the element's first
                     * leaf (1-D scalar arrays: identical to before). */
                    size_t leaf_idx;
                    if ((elem_node->flags & NODE_FLAG_DESIGNATED) &&
                        sym->type.base) {
                        size_t per = zc_object_count_scalar_leaves(sym->type.base);
                        leaf_idx = a_idx * (per ? per : 1);
                    } else {
                        leaf_idx = idx;
                    }
                    if (zc_object_resolve_scalar_leaf(&sym->type, leaf_idx, &off, &sz, &elem_is_float)) {
                        elem_offset = off;
                        elem_size = sz;
                    } else {
                        size_t unit_sz = (sym->type.base && sym->type.base->size > 0) ? sym->type.base->size : 4;
                        elem_offset = a_idx * unit_sz;
                        elem_size = unit_sz;
                        elem_is_float = (sym->type.base && (sym->type.base->tag == TYPE_FLOAT || sym->type.base->tag == TYPE_DOUBLE));
                    }
                } else {
                    elem_offset = 0;
                    elem_size = sym->type.size ? sym->type.size : 4;
                    elem_is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
                }
                int32_t store_offset = -(sym->stack_offset - (int32_t)elem_offset);
                if (elem_is_float && !c->last_expr_is_float) {
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)conv, (uint16_t)val_temp);
                    val_temp = conv;
                    c->last_expr_is_float = 1;
                } else if (!elem_is_float && c->last_expr_is_float) {
                    int conv = c->next_temp_id++;
                    ir_emit_cvttsd2si((uint16_t)conv, (uint16_t)val_temp);
                    val_temp = conv;
                    c->last_expr_is_float = 0;
                }
                /* A float element is held in an XMM temp (double form): the
                 * 4/8-byte stores must be the width-correct VSTORESS/VSTORESD
                 * (cvt + movss/movsd), not the GPR store_32/store. */
                if (elem_size == 1) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, store_offset);
                    ir_emit_store_byte(val_temp, addr_temp);
                } else if (elem_size == 2) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, store_offset);
                    ir_emit_store_16(val_temp, addr_temp);
                } else if (elem_size == 4) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, store_offset);
                    if (elem_is_float) {
                        ir_emit_vstoress((uint16_t)addr_temp, (uint16_t)val_temp);
                        c->last_expr_is_float = 0;
                    } else {
                        ir_emit_store_32(val_temp, addr_temp);
                    }
                } else {
                    if (elem_is_float) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, store_offset);
                        ir_emit_vstoresd((uint16_t)addr_temp, (uint16_t)val_temp);
                        c->last_expr_is_float = 0;
                    } else {
                        ir_emit_store(val_temp, BE(c)->fp_reg, store_offset);
                    }
                }
                idx++;
                cur_ref = elem_node->next;
            }
            if (sym->type.tag == TYPE_ARRAY) {
                int ptr_temp = c->next_temp_id++;
                ir_emit_lea(ptr_temp, BE(c)->fp_reg, -sym->stack_offset);
                c->last_expr_is_float = 0;
                return ptr_temp;
            } else if (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) {
                if (sym->type.size <= 8) {
                    int val_temp = c->next_temp_id++;
                    ir_emit_load(val_temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return val_temp;
                } else if (sym->type.size <= 16) {
                    int val_temp = c->next_temp_id++;
                    ir_emit_load(val_temp, BE(c)->fp_reg, -sym->stack_offset);
                    /* Second chunk in the ABI secondary value register
                     * (VREG_2: rdx on x86-64, x1 on AArch64, a1 on RISC-V)
                     * — same convention as 9-16 byte struct returns and
                     * by-value argument marshaling. */
                    ir_emit_load(VREG_2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                    c->last_expr_is_float = 0;
                    return val_temp;
                } else {
                    int ptr_temp = c->next_temp_id++;
                    ir_emit_lea(ptr_temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return ptr_temp;
                }
            } else {
                int val_temp = c->next_temp_id++;
                if (sym->type.size == 1) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, -sym->stack_offset);
                    ir_emit_load_byte(val_temp, addr_temp);
                } else if (sym->type.size == 2) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, -sym->stack_offset);
                    ir_emit_load_16(val_temp, addr_temp);
                } else if (sym->type.size == 4) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, -sym->stack_offset);
                    ir_emit_load_32(val_temp, addr_temp);
                } else {
                    ir_emit_load(val_temp, BE(c)->fp_reg, -sym->stack_offset);
                }
                c->last_expr_is_float = 0;
                return val_temp;
            }
        }

        case NODE_STMT_EXPR: {
            ZeroCCASTNode *block = zero_cc_ast_get(arena, node->left);
            if (!block) return TEMP_NONE;

            c->lowering.vla.block_depth++;

            int res_temp = TEMP_NONE;
            ASTRef stmt = block->left;
            while (stmt != AST_NULL) {
                ZeroCCASTNode *snode = zero_cc_ast_get(arena, stmt);
                if (!snode) break;
                if (snode->next == AST_NULL && snode->tag == NODE_EXPR_STMT && snode->left != AST_NULL) {
                    res_temp = lower_expr(c, arena, snode->left);
                } else {
                    lower_node(c, arena, stmt);
                }
                stmt = snode->next;
            }

            c->lowering.vla.block_depth--;
            return res_temp;
        }

        case NODE_INDEX: {
            /* Array indexing: base[index] */
            int array_temp = lower_expr(c, arena, node->left);
            int index_temp = lower_expr(c, arena, node->right);

            /* Determine element size */
            size_t elem_size = 4;
            TypeInfo *elem_ti = ast_node_get_typeinfo(c, node);
            if (elem_ti) {
                elem_size = type_size(elem_ti);
            } else {
                ZeroCCASTNode *ln = zero_cc_ast_get(arena, node->left);
                if (ln && ln->tag == NODE_IDENT) {
                    const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                    if (zt && zt->base && zt->base->size > 0) {
                        elem_size = zt->base->size;
                    }
                } else if (ln && ln->tag == NODE_STRING) {
                    elem_size = (ln->flags & NODE_FLAG_WIDE) ? 4 : 1;
                } else if (ln && ln->tag == NODE_COMPOUND_LIT) {
                    ZeroCCSym *sym = lowering_local_sym(c, ln, ln->s.str); /* index-based: name lookup misses post-bound __clit syms */
                    if (sym && sym->type.base && sym->type.base->size > 0) {
                        elem_size = sym->type.base->size;
                    }
                }
            }
            if (elem_size == 0) elem_size = 4;

            int scaled_index;
            if (elem_size == 1) {
                scaled_index = index_temp;
            } else if (elem_size == 2 || elem_size == 4 || elem_size == 8) {
                int shift = (elem_size == 2) ? 1 : ((elem_size == 4) ? 2 : 3);
                int sh_temp = c->next_temp_id++;
                ir_emit_mov_imm(sh_temp, shift);
                scaled_index = c->next_temp_id++;
                ir_emit_shl(scaled_index, index_temp, sh_temp);
            } else {
                int sz_temp = c->next_temp_id++;
                ir_emit_mov_imm(sz_temp, (int32_t)elem_size);
                scaled_index = c->next_temp_id++;
                ir_emit_mul(scaled_index, index_temp, sz_temp);
            }

            int addr_temp = c->next_temp_id++;
            ir_emit_add(addr_temp, array_temp, scaled_index);  /* addr = base + scaled_index */

            if (elem_ti && (type_is_array(elem_ti) || elem_ti->kind == TYPE_KIND_STRUCT || elem_ti->kind == TYPE_KIND_UNION)) {
                /* Array element is an array (e.g. a[i] in a[i][j]) or a struct/union (e.g. p[i][j] in p[i][j].x).
                 * In C, array expressions decay to pointers to their elements, and composite elements
                 * in lvalue/member contexts need the address directly without loading. */
                c->last_expr_is_float = 0;
                return addr_temp;
            }

            bool is_elem_float = false;
            if (elem_ti && type_is_floating(elem_ti)) {
                is_elem_float = true;
            } else {
                ZeroCCASTNode *ln = zero_cc_ast_get(arena, node->left);
                if (ln && ln->tag == NODE_IDENT) {
                    const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                    if (zt && zt->base && (zt->base->tag == TYPE_FLOAT || zt->base->tag == TYPE_DOUBLE)) {
                        is_elem_float = true;
                    }
                }
            }

            int result_temp = c->next_temp_id++;
            if (is_elem_float) {
                if (elem_size == 4) {
                    ir_emit_vloadss((uint16_t)result_temp, (uint16_t)addr_temp);
                } else {
                    ir_emit_vloadsd((uint16_t)result_temp, (uint16_t)addr_temp);
                }
                c->last_expr_is_float = 1;
            } else {
                if (elem_size == 1) {
                    ir_emit_load_byte(result_temp, addr_temp);
                } else if (elem_size == 2) {
                    ir_emit_load_16(result_temp, addr_temp);
                } else if (elem_size == 4) {
                    ir_emit_load_32(result_temp, addr_temp);
                } else {
                    ir_emit_deref(result_temp, addr_temp);
                }
                lower_extend_narrow(c, result_temp, elem_ti);
                if (!elem_ti) {
                    ZeroCCASTNode *ln = zero_cc_ast_get(arena, node->left);
                    if (ln && ln->tag == NODE_IDENT) {
                        const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                        if (zt && zt->base) {
                            zero_cc_emit_int_extend(c, result_temp, zt->base);
                        }
                    }
                }
                c->last_expr_is_float = 0;
            }
            return result_temp;
        }

        case NODE_MEMBER: {
            /* Load member value from memory (Rvalue context) */
            int32_t member_offset = 0;
            int chain_depth = 0;
            ZeroCCASTNode *curr = node;
            ZeroCCASTNode *innermost = node;
            ASTRef base_ref = ref;
            while (curr && curr->tag == NODE_MEMBER) {
                member_offset += (int32_t)curr->aux.alt;
                chain_depth++;
                innermost = curr;
                base_ref = curr->left;
                curr = zero_cc_ast_get(arena, curr->left);
            }
            ZeroCCASTNode *base_node = curr;
            /* The member node directly above the base decides whether the
             * base is a pointer (->) or a struct object (.). */
            bool base_is_ptr = (innermost && (innermost->flags & NODE_MEMBER_IS_ARROW)) != 0;
            TypeInfo *member_ti = ast_node_get_typeinfo(c, node);
            bool member_is_array = (member_ti && member_ti->kind == TYPE_KIND_ARRAY);
            bool member_is_struct = (member_ti && (member_ti->kind == TYPE_KIND_STRUCT || member_ti->kind == TYPE_KIND_UNION));
            bool member_is_float = (member_ti && type_is_floating(member_ti) && !member_is_array);
            size_t member_sz = member_ti ? type_size(member_ti) : 0;
            /* Fallback: derive member float-ness from the base struct's layout.
             * Only for a single (un-chained) member access - for chained
             * access the typeinfo (aux.extra) is authoritative.
             * NOTE: do NOT read innermost->s.str here - for NODE_MEMBER the
             * name (s.str) shares a union with aux.alt (offset), and semantic
             * analysis overwrites it. Match the member by offset instead. */
            if (!member_is_float && !member_is_array && innermost &&
                base_node && base_node->tag == NODE_IDENT) {
                ZeroCCType *st = NULL;
                ZeroCCSym *bs = zero_cc_find_local(c, base_node->s.str);
                if (bs && !bs->is_extern) {
                    st = (base_is_ptr && bs->type.tag == TYPE_PTR) ? bs->type.base : &bs->type;
                } else {
                    ZeroCCGlobalVar *bg = find_global_var(base_node->s.str);
                    if (bg) {
                        st = (bg->type.tag == TYPE_PTR) ? bg->type.base : &bg->type;
                    }
                }
                if (st && (st->tag == TYPE_STRUCT || st->tag == TYPE_UNION) && st->member_count <= 64) {
                    size_t in_off = (size_t)innermost->aux.alt;
                    for (size_t m = 0; m < st->member_count; m++) {
                        if (st->members[m].offset == in_off) {
                            if (st->members[m].tag == TYPE_FLOAT || st->members[m].tag == TYPE_DOUBLE) {
                                member_is_float = true;
                                member_sz = st->members[m].size ? st->members[m].size :
                                    (st->members[m].tag == TYPE_FLOAT ? 4 : 8);
                            }
                            break;
                        }
                    }
                }
            }
            int32_t base_stack_offset = 0;
            int val_temp = c->next_temp_id++;
            /* Multi-segment chains (o.p->v, a.b.c, p->a->b): the flat
             * offset-summing paths below cannot dereference intermediate
             * pointer members, so compute the address generically (C5). */
            int multi_addr = TEMP_NONE;
            if (chain_depth >= 2) {
                multi_addr = lower_member_chain_addr(c, arena, node);
                if (multi_addr != TEMP_NONE) {
                    if (member_is_array || member_is_struct) {
                        ir_emit_mov_reg(val_temp, multi_addr);
                        c->last_expr_is_float = 0;
                        return val_temp;
                    }
                    if (member_is_float) {
                        if (member_sz == 4) ir_emit_vloadss((uint16_t)val_temp, (uint16_t)multi_addr);
                        else ir_emit_vloadsd((uint16_t)val_temp, (uint16_t)multi_addr);
                        c->last_expr_is_float = 1;
                        return val_temp;
                    }
                    /* 64-bit load; the shared width-narrowing tail below
                     * fixes up sub-8-byte members. */
                    ir_emit_deref(val_temp, multi_addr);
                }
            }
            if (multi_addr == TEMP_NONE && base_node && base_node->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
                ZeroCCGlobalVar *ggv = (sym && !sym->is_extern) ? NULL : find_global_var(base_node->s.str);
                if (ggv) {
                        /* Global struct/union base: load the variable address,
                         * add the member offset, then load/deref. */
                        int gaddr = c->next_temp_id++;
                        ir_emit_global_addr(c, gaddr, ggv);
                        if (base_is_ptr) {
                            int ptr_temp = c->next_temp_id++;
                            ir_emit_deref(ptr_temp, gaddr);
                            gaddr = ptr_temp;
                        }
                        int gaddr2 = gaddr;
                        if (member_offset != 0) {
                            int goff = c->next_temp_id++;
                            ir_emit_mov_imm(goff, member_offset);
                            gaddr2 = c->next_temp_id++;
                            ir_emit_add(gaddr2, gaddr, goff);
                        }
                        if (member_is_float) {
                            if (member_sz == 4) ir_emit_vloadss((uint16_t)val_temp, (uint16_t)gaddr2);
                            else ir_emit_vloadsd((uint16_t)val_temp, (uint16_t)gaddr2);
                            c->last_expr_is_float = 1;
                            return val_temp;
                        }
                        if (member_is_array || member_is_struct) {
                            ir_emit_mov_reg(val_temp, gaddr2);
                            c->last_expr_is_float = 0;
                            return val_temp;
                        }
                        /* Plain scalar member: 64-bit deref, then fall through
                         * to the width-narrowing below (the struct layout may
                         * pack smaller members - e.g. `int a; float f;` - so
                         * the upper bytes of the load are not zero and an
                         * early return would leak them into the value). */
                        ir_emit_deref(val_temp, gaddr2);
                } else if (sym && base_is_ptr) {
                    /* Unknown identifier falls through to the local-style
                     * load below with base_stack_offset 0. */
                    /* p->member: base is a pointer. Load the pointer value,
                     * add the member offset, and dereference. (The trailing
                     * size-narrowing below handles sub-8-byte members.) */
                    int ptr_temp = c->next_temp_id++;
                    ir_emit_load(ptr_temp, BE(c)->fp_reg, -sym->stack_offset);
                    int addr_temp = ptr_temp;
                    if (member_offset != 0) {
                        int off_temp = c->next_temp_id++;
                        ir_emit_mov_imm(off_temp, member_offset);
                        addr_temp = c->next_temp_id++;
                        ir_emit_add(addr_temp, ptr_temp, off_temp);
                    }
                    if (member_is_array || member_is_struct) {
                        ir_emit_mov_reg(val_temp, addr_temp);
                        c->last_expr_is_float = 0;
                        return val_temp;
                    } else if (member_is_float) {
                        if (member_sz == 4) ir_emit_vloadss((uint16_t)val_temp, (uint16_t)addr_temp);
                        else ir_emit_vloadsd((uint16_t)val_temp, (uint16_t)addr_temp);
                        c->last_expr_is_float = 1;
                        return val_temp;
                    } else {
                        ir_emit_deref(val_temp, addr_temp);
                    }
                } else {
                    if (sym) {
                        base_stack_offset = -sym->stack_offset;
                    }
                    int32_t total_offset = base_stack_offset + member_offset;
                    if (member_is_float) {
                        if (member_sz == 4) {
                            /* 4-byte float member: width-correct load - the
                             * 8-byte FLOAD reads past the member into the
                             * adjacent bytes of the struct. */
                            int a4 = c->next_temp_id++;
                            ir_emit_lea(a4, TEMP_NONE, (int16_t)total_offset);
                            ir_emit_vloadss((uint16_t)val_temp, (uint16_t)a4);
                        } else {
                            ir_emit_fload((uint16_t)val_temp, TEMP_NONE, (int16_t)total_offset);
                        }
                        c->last_expr_is_float = 1;
                        return val_temp;
                    }
                    if (member_is_array || member_is_struct) {
                        ir_emit_lea(val_temp, BE(c)->fp_reg, total_offset);
                        c->last_expr_is_float = 0;
                        return val_temp;
                    } else {
                        ir_emit_load(val_temp, BE(c)->fp_reg, total_offset);
                    }
                }
            } else if (multi_addr == TEMP_NONE && base_node && base_node->tag == NODE_COMPOUND_LIT) {
                (void)lower_expr(c, arena, base_ref);
                ZeroCCSym *sym = lowering_local_sym(c, base_node, base_node->s.str); /* index-based: name lookup misses post-bound __clit syms */
                if (sym) {
                    base_stack_offset = -sym->stack_offset;
                }
                int32_t total_offset = base_stack_offset + member_offset;
                if (member_is_float) {
                    if (member_sz == 4) {
                        int a4 = c->next_temp_id++;
                        ir_emit_lea(a4, TEMP_NONE, (int16_t)total_offset);
                        ir_emit_vloadss((uint16_t)val_temp, (uint16_t)a4);
                    } else {
                        ir_emit_fload((uint16_t)val_temp, TEMP_NONE, (int16_t)total_offset);
                    }
                    c->last_expr_is_float = 1;
                    return val_temp;
                }
                if (member_is_array || member_is_struct) {
                    ir_emit_lea(val_temp, BE(c)->fp_reg, total_offset);
                    c->last_expr_is_float = 0;
                    return val_temp;
                } else {
                    ir_emit_load(val_temp, BE(c)->fp_reg, total_offset);
                }
            } else if (multi_addr == TEMP_NONE) {
                /* General expression returning struct (e.g. NODE_CALL returning buffer pointer) */
                int base_ptr = TEMP_NONE;
                if (!base_is_ptr) {
                    base_ptr = zero_cc_lower_lvalue_addr(c, arena, base_ref);
                }
                if (base_ptr == TEMP_NONE) {
                    base_ptr = lower_expr(c, arena, base_ref);
                    if (!base_is_ptr && !ast_expr_is_struct_addr(c, arena, base_ref)) {
                        c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                        c->current_stack_frame += 16;
                        int32_t spill_slot = c->current_stack_frame;
                        ir_emit_store(base_ptr, BE(c)->fp_reg, -spill_slot);
                        ir_emit_store(VREG_2, BE(c)->fp_reg, -(spill_slot - 8));
                        int struct_addr = c->next_temp_id++;
                        ir_emit_lea(struct_addr, BE(c)->fp_reg, -spill_slot);
                        base_ptr = struct_addr;
                    }
                }
                int addr_temp = base_ptr;
                if (member_offset != 0) {
                    /* Spill base_ptr to stack to avoid clobbering in addition */
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    int32_t bptr_off = c->current_stack_frame;
                    ir_emit_store(base_ptr, BE(c)->fp_reg, -bptr_off);
                    int b_temp = c->next_temp_id++;
                    ir_emit_load(b_temp, BE(c)->fp_reg, -bptr_off);
                    int off_temp = c->next_temp_id++;
                    ir_emit_mov_imm(off_temp, member_offset);
                    addr_temp = c->next_temp_id++;
                    ir_emit_add(addr_temp, b_temp, off_temp);
                }
                if (member_is_array || member_is_struct) {
                    ir_emit_mov_reg(val_temp, addr_temp);
                    c->last_expr_is_float = 0;
                    return val_temp;
                } else if (member_is_float) {
                    if (member_sz == 4) ir_emit_vloadss((uint16_t)val_temp, (uint16_t)addr_temp);
                    else ir_emit_vloadsd((uint16_t)val_temp, (uint16_t)addr_temp);
                    c->last_expr_is_float = 1;
                    return val_temp;
                } else {
                    ir_emit_deref(val_temp, addr_temp);
                }
            }

            if (member_is_array || member_is_struct) {
                c->last_expr_is_float = 0;
                return val_temp;
            }

            if (node->flags & NODE_MEMBER_IS_BITFIELD) {
                uint8_t bit_offset = (uint8_t)(node->aux.step & 0xFF);
                uint8_t bit_width = (uint8_t)((node->aux.step >> 8) & 0xFF);
                bool is_signed = ((node->aux.step >> 16) & 1) != 0;

                if (bit_offset > 0) {
                    int shift_temp = c->next_temp_id++;
                    ir_emit_mov_imm(shift_temp, (int32_t)bit_offset);
                    int shifted_temp = c->next_temp_id++;
                    ir_emit_shr(shifted_temp, val_temp, shift_temp);
                    val_temp = shifted_temp;
                }

                uint64_t mask = (bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1);
                int mask_temp = c->next_temp_id++;
                ir_emit_mov_imm(mask_temp, (int64_t)mask);
                int masked_temp = c->next_temp_id++;
                ir_emit_and(masked_temp, val_temp, mask_temp);
                val_temp = masked_temp;

                if (is_signed && bit_width > 0 && bit_width < 64) {
                    int64_t sign_bit = (int64_t)(1ULL << (bit_width - 1));
                    int sign_temp = c->next_temp_id++;
                    ir_emit_mov_imm(sign_temp, sign_bit);
                    int xor_temp = c->next_temp_id++;
                    ir_emit_xor(xor_temp, val_temp, sign_temp);
                    int ext_temp = c->next_temp_id++;
                    ir_emit_sub(ext_temp, xor_temp, sign_temp);
                    val_temp = ext_temp;
                }
            } else {
                /* Single-instruction in-place extension (replaces the old
                 * AND/xor/sub arithmetic narrowing - same semantics, no extra
                 * temps): sext for signed narrow members, zext for unsigned. */
                TypeInfo *ti = ast_node_get_typeinfo(c, node);
                size_t sz = ti ? type_size(ti) : 4;
                bool is_u = ti && (ti->modifiers & TYPE_MOD_UNSIGNED);
                if (sz == 1 || sz == 2 || sz == 4) {
                    if (is_u) ir_emit_zext(val_temp, (int)sz);
                    else ir_emit_sext(val_temp, (int)sz);
                }
            }

            c->last_expr_is_float = 0;
            return val_temp;
        }

        case NODE_CALL: {
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
                    strcmp(base_fn, "sqrtf") == 0) {
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
             * constant, 0 otherwise. Conservative: return 1 only for integer
             * literals, 0 otherwise. */
            if (callee_is_ident && callee->s.str &&
                strcmp(callee->s.str, "__builtin_constant_p") == 0) {
                ASTRef cp_arg = node->right;
                ZeroCCASTNode *an = zero_cc_ast_get(arena, cp_arg);
                int res = (an && an->tag == NODE_INT) ? 1 : 0;
                int temp = c->next_temp_id++;
                ir_emit_mov_imm(temp, res);
                c->last_expr_is_float = 0;
                return temp;
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

            /* __builtin_ffs(x) - return 1 + index of least significant set
             * bit (1-indexed), or 0 if x == 0.  Branchless bit scan:
             * result = 0; for i in 0..31: if ((x>>i)&1) && result==0:
             * result = i+1 */
            if (callee_is_ident && callee->s.str &&
                strcmp(callee->s.str, "__builtin_ffs") == 0) {
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
             *    Computes a OP b, stores result to *r according to target width,
             *    and returns 0 in a fresh temp register (conservative: no overflow). */
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
                ASTRef mo_arg = node->right;
                int t_a = lower_expr(c, arena, mo_arg);
                ZeroCCASTNode *an0 = zero_cc_ast_get(arena, mo_arg);
                mo_arg = (an0 && an0->next != AST_NULL) ? an0->next : AST_NULL;
                int t_b = lower_expr(c, arena, mo_arg);
                ZeroCCASTNode *an1 = zero_cc_ast_get(arena, mo_arg);
                mo_arg = (an1 && an1->next != AST_NULL) ? an1->next : AST_NULL;
                int t_ptr = lower_expr(c, arena, mo_arg);
                ZeroCCASTNode *an2 = zero_cc_ast_get(arena, mo_arg);
                int width = an2 ? zc_atomic_access_width(c, an2) : 4;
                if (an2 && an2->tag == NODE_UNOP && an2->aux.op == TK_AMP) {
                    ZeroCCASTNode *target = zero_cc_ast_get(arena, an2->left);
                    if (target && target->tag == NODE_IDENT) {
                        ZeroCCSym *sym = zero_cc_find_local(c, target->s.str);
                        if (sym && (sym->type.size == 1 || sym->type.size == 2 ||
                                    sym->type.size == 4 || sym->type.size == 8)) {
                            width = (int)sym->type.size;
                        } else {
                            ZeroCCGlobalVar *gv = find_global_var(target->s.str);
                            if (gv && (gv->type.size == 1 || gv->type.size == 2 ||
                                        gv->type.size == 4 || gv->type.size == 8)) {
                                width = (int)gv->type.size;
                            }
                        }
                    }
                }
                int t_res = c->next_temp_id++;
                if (is_add) {
                    ir_emit((Instr){.op=OP_ADD,.temp_dest=(uint16_t)t_res,
                        .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_b});
                } else if (is_sub) {
                    ir_emit((Instr){.op=OP_SUB,.temp_dest=(uint16_t)t_res,
                        .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_b});
                } else {
                    ir_emit((Instr){.op=OP_MUL,.temp_dest=(uint16_t)t_res,
                        .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_b});
                }
                if (width == 1) ir_emit_store_byte(t_res, t_ptr);
                else if (width == 2) ir_emit_store_16(t_res, t_ptr);
                else if (width == 8) ir_emit_store_64(t_res, t_ptr);
                else ir_emit_store_32(t_res, t_ptr);
                int t_ovf = c->next_temp_id++;
                ir_emit_mov_imm(t_ovf, 0);
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
                bool is_indirect_call = !callee_is_ident || (callee_fn == NULL &&
                    (callee_sym != NULL || (callee_gv != NULL && callee_gv->type.is_func_ptr)));

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
                            for (int p = 0; p < 8; p++) {
                                prev_param_names[p] = c->lowering.inlining.param_names[p];
                                prev_param_temps[p] = c->lowering.inlining.param_temps[p];
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
                    TypeInfo *ret_ti = ast_node_get_typeinfo(c, node);
                    if (ret_ti) {
                        call_ret_type = typeinfo_to_zero_cc_type(ret_ti);
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
                bool callee_uses_sret = (call_ret_type.tag == TYPE_STRUCT || call_ret_type.tag == TYPE_UNION) && (call_ret_type.size > 16);
                if (callee_uses_sret && c->isa != ISA_AARCH64) gp_stream = 1;  /* hidden sret ptr takes GP slot 0 */

                while (arg != AST_NULL) {
                    ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                    int temp = lower_expr(c, arena, arg);
                    if (callee_fn && param_idx < (int)callee_fn->param_count) {
                        const ZeroCCType *pt = &callee_fn->param_types[param_idx];
                        bool param_is_fp = (pt->tag == TYPE_FLOAT || pt->tag == TYPE_DOUBLE);
                        if (param_is_fp && !c->last_expr_is_float) {
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
                                                   arg_struct_sym->type.tag == TYPE_UNION)) {
                                arg_struct_sz = arg_struct_sym->type.size;
                            } else if (!arg_struct_sym) {
                                ZeroCCGlobalVar *gv = find_global_var(arg_node->s.str);
                                if (gv && (gv->type.tag == TYPE_STRUCT || gv->type.tag == TYPE_UNION)) {
                                    arg_struct_sz = gv->type.size;
                                }
                            }
                        } else if (arg_node) {
                            TypeInfo *arg_ti = ast_node_get_typeinfo(c, arg_node);
                            if (arg_ti && (arg_ti->kind == TYPE_KIND_STRUCT || arg_ti->kind == TYPE_KIND_UNION)) {
                                arg_struct_sz = type_size(arg_ti);
                            }
                        }
                        bool is_mem_struct = (arg_struct_sz > 0 &&
                                              ast_expr_is_struct_addr(c, arena, arg));
                        int mem_struct_addr = is_mem_struct ? temp : TEMP_NONE;
                        if (is_mem_struct) {
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
                         *  - >16 byte values: `temp` already holds the
                         *    address of the value (ABI: passed by pointer),
                         *    so no extra chunks are passed. */
                        if (arg_struct_sz > 8) {
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

        default:
            return TEMP_NONE;
    }
}

