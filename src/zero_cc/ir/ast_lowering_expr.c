/* AST Lowering - Expression Lowering (AST to IR) */

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/isa/isa_backend.h"

static bool struct_resolve_scalar_leaf(const ZeroCCType *st, size_t target_idx, size_t *cur_idx,
                                       size_t base_off, size_t *out_off, size_t *out_sz,
                                       const char **out_flex_name) {
    for (size_t i = 0; i < st->member_count; i++) {
        const ZeroCCStructMember *m = &st->members[i];
        if (m->is_flexible) {
            if (*cur_idx == target_idx) {
                if (out_flex_name) *out_flex_name = m->name;
                return false;
            }
            continue;
        }
        if (m->tag == TYPE_ARRAY && m->base && m->base->size > 0) {
            size_t count = m->size / m->base->size;
            if (target_idx < *cur_idx + count) {
                size_t a_idx = target_idx - *cur_idx;
                *out_off = base_off + m->offset + a_idx * m->base->size;
                *out_sz = m->base->size;
                return true;
            }
            *cur_idx += count;
        } else if ((m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) && m->base && m->base->member_count > 0) {
            if (struct_resolve_scalar_leaf(m->base, target_idx, cur_idx, base_off + m->offset, out_off, out_sz, out_flex_name)) {
                return true;
            }
            if (out_flex_name && *out_flex_name) return false;
        } else {
            if (*cur_idx == target_idx) {
                *out_off = base_off + m->offset;
                *out_sz = m->size;
                return true;
            }
            (*cur_idx)++;
        }
    }
    return false;
}

/* Number of scalar leaves in one object of type t (C layout order).
 * Sprint 40: the parser flattens nested brace-initializer lists into a
 * leaf-level chain, so initializers are placed by leaf index. */
static size_t object_count_scalar_leaves(const ZeroCCType *t) {
    if (!t) return 0;
    if (t->tag == TYPE_ARRAY) {
        if (!t->base || t->array_len == 0) return 0;
        return t->array_len * object_count_scalar_leaves(t->base);
    }
    if (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) {
        size_t n = 0;
        for (size_t i = 0; i < t->member_count; i++) {
            const ZeroCCStructMember *m = &t->members[i];
            if (m->is_flexible) continue;
            if (m->tag == TYPE_ARRAY && m->base && m->base->size > 0) {
                n += (m->size / m->base->size) * object_count_scalar_leaves(m->base);
            } else if (m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) {
                n += m->base ? object_count_scalar_leaves(m->base) : 1;
            } else {
                n++;
            }
        }
        return n;
    }
    return 1;
}

/* Resolve the (offset, size) of the target_idx-th scalar leaf of one
 * object of type t (array or struct) in C layout order.  Used by the
 * compound-literal initializer to place flattened positional elements.
 * Pre-Sprint-40, array initializers placed element k at k * base_size
 * with a base_size store - correct only for 1-D arrays of scalars; a
 * 2-D `int m[2][4]` (base_size = 16) stored element 1 at byte 16, and
 * `struct P v[2]` (flattened to 8 ints) stored each int 16 bytes apart.
 * Returns false when the index is out of range or the layout is
 * unresolvable (caller falls back to the legacy behavior). */
static bool object_resolve_scalar_leaf(const ZeroCCType *t, size_t target_idx,
                                       size_t *out_off, size_t *out_sz) {
    if (!t) return false;
    if (t->tag == TYPE_ARRAY) {
        const ZeroCCType *el = t->base;
        if (!el || el->size == 0 || t->array_len == 0) return false;
        if (el->tag == TYPE_ARRAY || el->tag == TYPE_STRUCT || el->tag == TYPE_UNION) {
            size_t per = object_count_scalar_leaves(el);
            if (per == 0 || target_idx >= t->array_len * per) return false;
            size_t ei = target_idx / per;
            size_t wi = target_idx % per;
            size_t sub_off, sub_sz;
            if (el->tag == TYPE_ARRAY) {
                if (!object_resolve_scalar_leaf(el, wi, &sub_off, &sub_sz)) return false;
            } else {
                size_t ci = 0;
                const char *flex = NULL;
                if (!struct_resolve_scalar_leaf(el, wi, &ci, 0, &sub_off, &sub_sz, &flex))
                    return false;
            }
            *out_off = ei * el->size + sub_off;
            *out_sz = sub_sz;
            return true;
        }
        /* scalar elements */
        if (target_idx >= t->array_len) return false;
        *out_off = target_idx * el->size;
        *out_sz = el->size;
        return true;
    }
    if (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) {
        size_t ci = 0;
        const char *flex = NULL;
        return struct_resolve_scalar_leaf(t, target_idx, &ci, 0, out_off, out_sz, &flex);
    }
    if (target_idx == 0) {
        *out_off = 0;
        *out_sz = t->size ? t->size : 4;
        return true;
    }
    return false;
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
            if (v >= INT32_MIN && v <= INT32_MAX) { *sign = 1; return 4; }
            if (v > INT32_MAX && v <= INT64_C(0xFFFFFFFF)) { *sign = 0; return 4; }
            return 0;  /* long / long long literal */
        }
        case NODE_BOOL:  *sign = 0; return 1;  /* _Bool is 1-byte, 0/1 */
        case NODE_NULLPTR: return 0;  /* 8-byte pointer value */
        case NODE_IDENT: {
            ZeroCCSym *sym = zero_cc_find_local(c, n->s.str);
            if (sym) return zero_cc_narrow_int_width(&sym->type, sign);
            ZeroCCGlobalVar *gv = find_global_var(n->s.str);
            if (gv) return zero_cc_narrow_int_width(&gv->type, sign);
            return 0;
        }
        default:
            return 0;
    }
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
        ZeroCCGlobalVar *ggv = sym ? NULL : find_global_var(base_node->s.str);
        if (sym) {
            addr = c->next_temp_id++;
            ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
        } else if (ggv) {
            addr = c->next_temp_id++;
            ir_emit_mov_global(addr, ggv->name);
        } else {
            generic_base = true;
        }
    } else if (base_node->tag == NODE_COMPOUND_LIT) {
        (void)lower_expr(c, arena, base_ref);
        ZeroCCSym *sym = (base_node->s.str && base_node->s.str[0]) ? zero_cc_find_local(c, base_node->s.str) : NULL;
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
        addr = lower_expr(c, arena, base_ref);
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
        ti->kind != TYPE_KIND_SHORT && ti->kind != TYPE_KIND_INT) return;
    size_t sz = type_size(ti);
    if (sz != 1 && sz != 2 && sz != 4) return;
    if (type_is_unsigned(ti)) ir_emit_zext(temp, (int)sz);
    else ir_emit_sext(temp, (int)sz);
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
                size_t len = strlen(node->s.str);
                size_t rodata_offset = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, node->s.str, len);
                uint8_t zero = 0;
                zero_cc_buf_put_bytes(&c->rodata_section, &zero, 1);
                ir_emit_lea_rip(temp, rodata_offset);
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
                if (c->function_count > 0) {
                    fn_name = c->functions[c->function_count - 1].name;
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
            if (sym) {
                if (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE) {
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
                } else {
                    int temp = c->next_temp_id++;
                    ir_emit_load(temp, BE(c)->fp_reg, -sym->stack_offset);
                    /* 32-bit int semantics: extend narrow int locals to full
                     * 64-bit width (sign/zero per type) so overflowed values
                     * compare and store correctly. No-op for ptr/struct/8-byte. */
                    zero_cc_emit_int_extend(c, temp, &sym->type);
                    if (sym->is_restrict && sym->type.tag == TYPE_PTR && temp < 256) {
                        /* Each restrict pointer gets its own alias group (locals index + 1) */
                        c->lowering.temp_alias_group[temp] = (uint8_t)((sym - c->locals) + 1);
                    }
                    c->last_expr_is_float = 0;
                    return temp;
                }
            } else {
                ZeroCCGlobalVar *gv = find_global_var(name);
                if (gv) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_mov_global(addr_temp, gv->name);
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
            /* Logical AND (&&) / OR (||): compute (left != 0) and (right != 0)
             * as 0/1 integers, then combine with bitwise & / |. Straight-line
             * (no embedded branches) so it stays safe under the -O2/-O3 optimizer
             * passes. Result is a 0/1 integer, matching C. (Sprint 41) Previously
             * these fell through to the default arm, which returned only the LEFT
             * operand -- e.g. `a && b > 5` was parsed as `(a && b) > 5` and lowered
             * to just `a`, dropping the `b > 5` (and the `while(s[n] && n<7)` bound).
             * Known limitation: no short-circuit, so the right operand is always
             * evaluated (`p && *p` with p==NULL still derefs p). */
            if (node->aux.op == TK_LOGAND || node->aux.op == TK_LOGOR) {
                int result_temp = c->next_temp_id++;
                int lt = c->next_temp_id++;
                int rt = c->next_temp_id++;
                int lc = lower_expr(c, arena, node->left);
                ir_emit_cmp_zero(lc);
                ir_emit_cond_set(lt, CC_NE);                 /* lt = (a != 0) */
                int rc = lower_expr(c, arena, node->right);
                ir_emit_cmp_zero(rc);
                ir_emit_cond_set(rt, CC_NE);                 /* rt = (b != 0) */
                if (node->aux.op == TK_LOGAND)
                    ir_emit_and(result_temp, lt, rt);        /* a && b = (a!=0) & (b!=0) */
                else
                    ir_emit_or(result_temp, lt, rt);         /* a || b = (a!=0) | (b!=0) */
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
                case TK_SLASH:
                    ir_emit_div(result_temp, left_temp, right_temp);
                    break;
                case TK_PERCENT:
                    ir_emit_mod(result_temp, left_temp, right_temp);
                    break;
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
                    int lsign = 1;
                    if (zero_cc_operand_int_width(c, arena, node->left, &lsign) > 0 && lsign) {
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
                    int ls = 1, rs = 1;
                    int lw = zero_cc_operand_int_width(c, arena, node->left, &ls);
                    int rw = zero_cc_operand_int_width(c, arena, node->right, &rs);
                    if (lw != 0 && rw != 0) {
                        int w, s;
                        if (lw > rw) { w = lw; s = ls; }
                        else if (rw > lw) { w = rw; s = rs; }
                        else { w = lw; s = (ls && rs) ? 1 : 0; }  /* equal width: any unsigned -> unsigned */
                        ir_mark_last_width(s ? (uint8_t)w : (uint8_t)(8 + w));
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
                            if (node->flags == 1) {
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
                        /* Store new value back to stack */
                        ir_emit_store(res_temp, BE(c)->fp_reg, -sym->stack_offset);

                        c->last_expr_is_float = 0;
                        /* Postfix x++ / x-- yields the OLD value; prefix
                         * ++x / --x yields the new value (node->flags == 1
                         * marks postfix, matching the global/atomic paths
                         * below). Pre-fix the local path always returned the
                         * new value, miscompiling `return x++;` etc. */
                        return (node->flags == 1) ? val_temp : res_temp;
                    }
                    ZeroCCGlobalVar *gv = find_global_var(var);
                    if (gv) {
                        int gaddr_temp = c->next_temp_id++;
                        ir_emit_mov_global(gaddr_temp, gv->name);
                        int val_temp = c->next_temp_id++;
                        int one_temp = c->next_temp_id++;
                        int res_temp = c->next_temp_id++;
                        size_t gv_sz = gv->size > 0 ? gv->size : 4;
                        if (gv_sz == 1) ir_emit_load_byte(val_temp, gaddr_temp);
                        else if (gv_sz == 2) ir_emit_load_16(val_temp, gaddr_temp);
                        else if (gv_sz == 4) ir_emit_load_32(val_temp, gaddr_temp);
                        else ir_emit_deref(val_temp, gaddr_temp);

                        ir_emit_mov_imm(one_temp, (node->aux.op == TK_INC) ?
                            lvalue_inc_step(&gv->type) : -lvalue_inc_step(&gv->type));
                        ir_emit_add(res_temp, val_temp, one_temp);

                        if (gv_sz == 1) ir_emit_store_byte(res_temp, gaddr_temp);
                        else if (gv_sz == 2) ir_emit_store_16(res_temp, gaddr_temp);
                        else if (gv_sz == 4) ir_emit_store_32(res_temp, gaddr_temp);
                        else ir_emit_store_64(res_temp, gaddr_temp);

                        c->last_expr_is_float = 0;
                        return (node->flags == 1) ? val_temp : res_temp;
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
                                    ir_emit_mov_global(base_addr, bgv->name);
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
                        return (node->flags == 1) ? old_t : new_t;
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
                    return (node->flags == 1) ? old_t : new_t;
                }
                return TEMP_NONE;
            }

            if (node->aux.op == '&' || node->aux.op == TK_AMP) {
                ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
                if (operand && operand->tag == NODE_IDENT) {
                    ZeroCCSym *sym = zero_cc_find_local(c, operand->s.str);
                    if (sym) {
                        int addr_temp = c->next_temp_id++;
                        if (sym->is_vla) {
                            ir_emit_load(addr_temp, BE(c)->fp_reg, -sym->stack_offset);
                        } else {
                            ir_emit_lea(addr_temp, TEMP_NONE, -sym->stack_offset);
                        }
                        c->last_expr_is_float = 0;
                        return addr_temp;
                    }
                    ZeroCCGlobalVar *gv = find_global_var(operand->s.str);
                    if (gv) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_mov_global(addr_temp, gv->name);
                        c->last_expr_is_float = 0;
                        return addr_temp;
                    }
                    ZeroCCFunction *fn = zero_cc_find_function(c, operand->s.str);
                    if (fn) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_mov_global(addr_temp, fn->name);
                        c->last_expr_is_float = 0;
                        return addr_temp;
                    }
                } else if (operand && operand->tag == NODE_COMPOUND_LIT) {
                    (void)lower_expr(c, arena, node->left);
                    ZeroCCSym *sym = (operand->s.str && operand->s.str[0]) ? zero_cc_find_local(c, operand->s.str) : NULL;
                    if (sym) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, -sym->stack_offset);
                        c->last_expr_is_float = 0;
                        return addr_temp;
                    }
                } else if (operand && operand->tag == NODE_MEMBER) {
                    /* Address-of struct/union member (&s.m or &ptr->m) */
                    int32_t member_offset = 0;
                    int chain_depth = 0;
                    ZeroCCASTNode *curr = operand;
                    ASTRef base_ref = node->left;
                    while (curr && curr->tag == NODE_MEMBER) {
                        member_offset += (int32_t)curr->aux.alt;
                        chain_depth++;
                        base_ref = curr->left;
                        curr = zero_cc_ast_get(arena, curr->left);
                    }
                    ZeroCCASTNode *base_node = curr;
                    int addr_temp = c->next_temp_id++;
                    if (chain_depth >= 2) {
                        /* &o.p->v: walk the chain (intermediate pointer
                         * members are dereferenced) - C5. */
                        int ch = lower_member_chain_addr(c, arena, operand);
                        if (ch != TEMP_NONE) {
                            c->last_expr_is_float = 0;
                            return ch;
                        }
                    }
                    if (base_node && base_node->tag == NODE_IDENT) {
                        ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
                        if (sym) {
                            int32_t total_offset = -sym->stack_offset + member_offset;
                            ir_emit_lea(addr_temp, TEMP_NONE, total_offset);
                            c->last_expr_is_float = 0;
                            return addr_temp;
                        }
                    } else if (base_node && base_node->tag == NODE_COMPOUND_LIT) {
                        (void)lower_expr(c, arena, base_ref);
                        ZeroCCSym *sym = (base_node->s.str && base_node->s.str[0]) ? zero_cc_find_local(c, base_node->s.str) : NULL;
                        if (sym) {
                            int32_t total_offset = -sym->stack_offset + member_offset;
                            ir_emit_lea(addr_temp, TEMP_NONE, total_offset);
                            c->last_expr_is_float = 0;
                            return addr_temp;
                        }
                    }
                    /* General base expression (pointer or expression) */
                    int base_ptr = lower_expr(c, arena, base_ref);
                    if (member_offset != 0) {
                        int off_temp = c->next_temp_id++;
                        ir_emit_mov_imm(off_temp, member_offset);
                        ir_emit_add(addr_temp, base_ptr, off_temp);
                    } else {
                        ir_emit_mov_reg(addr_temp, base_ptr);
                    }
                    c->last_expr_is_float = 0;
                    return addr_temp;
                } else if (operand && operand->tag == NODE_INDEX) {
                    /* Address-of array index (&arr[idx]) */
                    int array_temp = lower_expr(c, arena, operand->left);
                    int index_temp = lower_expr(c, arena, operand->right);

                    size_t elem_size = 4;
                    TypeInfo *elem_ti = ast_node_get_typeinfo(c, operand);
                    if (elem_ti) {
                        elem_size = type_size(elem_ti);
                    } else {
                        ZeroCCASTNode *ln = zero_cc_ast_get(arena, operand->left);
                        if (ln && ln->tag == NODE_IDENT) {
                            ZeroCCSym *sym = zero_cc_find_local(c, ln->s.str);
                            if (sym && sym->type.base && sym->type.base->size > 0) {
                                elem_size = sym->type.base->size;
                            }
                        } else if (ln && ln->tag == NODE_COMPOUND_LIT) {
                            ZeroCCSym *sym = (ln->s.str && ln->s.str[0]) ? zero_cc_find_local(c, ln->s.str) : NULL;
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
                    c->last_expr_is_float = 0;
                    return addr_temp;
                }
            }

            int operand_temp = lower_expr(c, arena, node->left);
            int result_temp = c->next_temp_id++;

            switch (node->aux.op) {
                case TK_STAR: { /* Dereference */
                    size_t p_size = 0;
                    TypeInfo *ti = ast_node_get_typeinfo(c, node);
                    if (ti) p_size = type_size(ti);
                    else {
                        ZeroCCASTNode *inner = zero_cc_ast_get(arena, node->left);
                        if (inner && inner->tag == NODE_IDENT) {
                            ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                            if (ptr_sym && ptr_sym->type.base && ptr_sym->type.base->size > 0) {
                                p_size = ptr_sym->type.base->size;
                            }
                        }
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
                        /* Float negation as (0.0 - x). An integer NEG on the
                         * IEEE-754 bit pattern is wrong, and the 128-bit
                         * sign-mask trick (xorpd [m128]) faults on WSL2
                         * (16-byte reads from RX pages raise a GPF). */
                        double zero = 0.0;
                        size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
                        if (pad) { uint8_t z8[8] = {0}; zero_cc_buf_put_bytes(&c->rodata_section, z8, pad); }
                        size_t zero_off = c->rodata_section.length;
                        zero_cc_buf_put_bytes(&c->rodata_section, &zero, sizeof(double));
                        int zero_temp = c->next_temp_id++;
                        ir_emit_fmov_imm((uint16_t)zero_temp, (int64_t)zero_off);
                        ir_emit_fsub((uint16_t)result_temp, (uint16_t)zero_temp, (uint16_t)operand_temp);
                        c->last_expr_is_float = 1;
                        return result_temp;
                    }
                    ir_emit_neg(result_temp, operand_temp);
                    break;
                case TK_BANG:  /* Logical NOT: !operand */
                    ir_emit_cmp_zero(operand_temp);
                    ir_emit_cond_set(result_temp, CC_EQ);
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
            int result = lower_expr(c, arena, node->left);
            TypeInfo *cast_ti = ast_node_get_typeinfo(c, node);
            bool target_is_float = type_is_floating(cast_ti);
            if (target_is_float) {
                if (!c->last_expr_is_float) {
                    /* int/pointer -> float/double: convert (C9). Previously
                     * this case fell through and returned the raw integer
                     * bits as if they were a double bit pattern (int 3 ->
                     * 3.0e-323), with last_expr_is_float left 0 so callers
                     * (e.g. the call-argument path) took the integer route. */
                    int fp_res = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)fp_res, (uint16_t)result);
                    c->last_expr_is_float = 1;
                    return fp_res;
                }
                /* float -> float/double: no-op under the 64-bit double FP
                 * model (previously the float-source branch below truncated
                 * the value through cvttsd2si). */
                return result;
            }
            /* Float to int conversion: emit via IR for correct sequencing */
            if (c->last_expr_is_float) {
                int int_res_temp = c->next_temp_id++;
                ir_emit_cvttsd2si((uint16_t)int_res_temp, (uint8_t)result);
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
            /* va_start(ap, last) - initialize va_list to point to variadic args */
            /* The variadic args are in the spill area after the fixed params */
            /* For System V AMD64: after saving rdi, rsi, rdx, rcx, r8, r9 */
            /* The first variadic arg is at offset after the last fixed param */

            /* Get the ap variable name - node->left is an ASTRef to the ap identifier */
            ZeroCCASTNode *ap_node = zero_cc_ast_get(arena, node->left);
            const char *ap_var = (ap_node && ap_node->tag == NODE_IDENT) ? ap_node->s.str : "";
            ZeroCCSym *ap_sym = zero_cc_find_local(c, ap_var);

            if (ap_sym) {
                /* Get the 'last' parameter to find where variadic args start */
                ZeroCCASTNode *last_node = zero_cc_ast_get(arena, node->right);
                const char *last_var = (last_node && last_node->tag == NODE_IDENT) ? last_node->s.str : "";
                ZeroCCSym *last_sym = zero_cc_find_local(c, last_var);

                if (last_sym) {
                    /* variadic args start 8 bytes after the last fixed param in stack */
                    int32_t va_start_offset = -(last_sym->stack_offset + 8);

                    /* Store address into ap: ap = (char*)fp_reg + va_start_offset */
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, va_start_offset);
                    ir_emit_store(addr_temp, BE(c)->fp_reg, -ap_sym->stack_offset);
                }
            }

            c->last_expr_is_float = 0;
            return VREG_0;
        }

        case NODE_VA_ARG: {
            /* va_arg(ap, type) - extract next variadic argument */
            /* Simplified: bump pointer by 8 bytes and load */
            /* Full implementation would check gp_offset for register args */

            /* Get the va_list pointer */
            int ap_temp = lower_expr(c, arena, node->left);

            /* Load the value from [ap] */
            int result_temp = c->next_temp_id++;
            ir_emit_deref(result_temp, ap_temp);

            /* Bump ap by 8 bytes downward: ap -= 8 */
            int eight_temp = c->next_temp_id++;
            ir_emit_mov_imm(eight_temp, -8);
            int new_ap_temp = c->next_temp_id++;
            ir_emit_add(new_ap_temp, ap_temp, eight_temp);

            /* Store back to ap variable */
            ZeroCCASTNode *ap_node = zero_cc_ast_get(arena, node->left);
            const char *ap_var = ap_node ? ap_node->s.str : "";
            ZeroCCSym *ap_sym = zero_cc_find_local(c, ap_var);
            if (ap_sym) {
                ir_emit_store(new_ap_temp, BE(c)->fp_reg, -ap_sym->stack_offset);
            }

            c->last_expr_is_float = 0;
            return result_temp;
        }

        case NODE_ASSIGN: {
            /* Check if this is a compound assignment (+=, -=, etc.) */
            bool is_compound = (node->aux.op != TK_ASSIGN && node->aux.op != '=');

            int right_temp = lower_expr(c, arena, node->right);
            ZeroCCASTNode *left_node = zero_cc_ast_get(arena, node->left);

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
                        ir_emit_mov_global(gaddr, ggv->name);
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
                        size_t m_size = 4;
                        TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                        if (member_ti) m_size = type_size(member_ti);
                        if (is_compound)
                            member_apply_compound(c, node->aux.op, addr_temp, &right_temp,
                                                   m_size,
                                                   (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float);
                        if (c->last_expr_is_float) {
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
                        size_t m_size = 4;
                        TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                        if (member_ti) m_size = type_size(member_ti);
                        if (is_compound)
                            member_apply_compound(c, node->aux.op, addr_temp, &right_temp,
                                                   m_size,
                                                   (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float);
                        if (c->last_expr_is_float) {
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
                    ZeroCCSym *sym = (base_node->s.str && base_node->s.str[0]) ? zero_cc_find_local(c, base_node->s.str) : NULL;
                    if (sym) {
                        base_stack_offset = -sym->stack_offset;
                    }
                } else {
                    /* General base expression evaluating to struct pointer/address (e.g. p[i][j]) */
                    int base_ptr = lower_expr(c, arena, base_ref);
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
                    if (is_compound)
                        member_apply_compound(c, node->aux.op, addr_temp, &right_temp,
                                               m_size,
                                               (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float);
                    if (c->last_expr_is_float) {
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
                    uint8_t bit_offset = (uint8_t)(left_node->aux.step & 0xFF);
                    uint8_t bit_width = (uint8_t)((left_node->aux.step >> 8) & 0xFF);
                    bool is_signed = ((left_node->aux.step >> 16) & 1) != 0;
                    uint64_t val_mask = (bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1);

                    /* 1. Load current storage unit */
                    int curr_temp = c->next_temp_id++;
                    ir_emit_load(curr_temp, BE(c)->fp_reg, total_offset);

                    /* Handle compound assignment on bitfield */
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
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, right_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, right_temp); break;
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
                    ir_emit_mov_imm(clear_mask_temp, (int32_t)clear_mask);
                    int cleared_temp = c->next_temp_id++;
                    ir_emit_and(cleared_temp, curr_temp, clear_mask_temp);

                    /* 3. Mask incoming value to bit_width */
                    int val_mask_temp = c->next_temp_id++;
                    ir_emit_mov_imm(val_mask_temp, (int32_t)val_mask);
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
                    ir_emit_store(combined_temp, BE(c)->fp_reg, total_offset);

                    c->last_expr_is_float = 0;
                    return right_temp;
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

                if (c->last_expr_is_float) {
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
                        ZeroCCSym *sym = zero_cc_find_local(c, ln->s.str);
                        if (sym && sym->type.base && sym->type.base->size > 0) {
                            elem_size = sym->type.base->size;
                        }
                    } else if (ln && ln->tag == NODE_COMPOUND_LIT) {
                        ZeroCCSym *sym = (ln->s.str && ln->s.str[0]) ? zero_cc_find_local(c, ln->s.str) : NULL;
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
                        ZeroCCSym *sym = zero_cc_find_local(c, ln->s.str);
                        if (sym && sym->type.base && (sym->type.base->tag == TYPE_FLOAT || sym->type.base->tag == TYPE_DOUBLE)) {
                            is_elem_float = true;
                        }
                    }
                }

                if (is_elem_float) {
                    if (elem_size == 4) {
                        ir_emit_vstoress(addr_temp, (uint16_t)right_temp);
                    } else {
                        ir_emit_vstoresd(addr_temp, (uint16_t)right_temp);
                    }
                    c->last_expr_is_float = 1;
                } else {
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
                int final_val = right_temp;
                if (is_compound) {
                    int curr_val = c->next_temp_id++;
                    ir_emit_deref(curr_val, ptr_temp);
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
                size_t p_size = 4;
                TypeInfo *target_ti = ast_node_get_typeinfo(c, left_node);
                if (target_ti) p_size = type_size(target_ti);
                else {
                    ZeroCCASTNode *inner = zero_cc_ast_get(arena, left_node->left);
                    if (inner && inner->tag == NODE_IDENT) {
                        ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                        if (ptr_sym && ptr_sym->type.base && ptr_sym->type.base->size > 0) {
                            p_size = ptr_sym->type.base->size;
                        }
                    }
                }
                if (c->last_expr_is_float) {
                    if (p_size == 4) ir_emit_vstoress((uint16_t)ptr_temp, (uint16_t)final_val);
                    else ir_emit_vstoresd((uint16_t)ptr_temp, (uint16_t)final_val);
                    c->last_expr_is_float = 1;
                } else {
                    if (p_size == 1) ir_emit_store_byte(final_val, ptr_temp);
                    else if (p_size == 2) ir_emit_store_16(final_val, ptr_temp);
                    else if (p_size == 4) ir_emit_store_32(final_val, ptr_temp);
                    else ir_emit_store_64(final_val, ptr_temp);
                    c->last_expr_is_float = 0;
                }
                return final_val;
            }

            const char *var = left_node ? left_node->s.str : "";
            ZeroCCSym *sym = zero_cc_find_local(c, var);
            if (sym) {
                if (sym->is_const || sym->type.is_const) {
                    diag_report_error_at_token(c, "cannot assign to const-qualified variable '%s'", var);
                    return TEMP_NONE;
                }
                bool target_is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
                if (target_is_float) {
                    int float_temp = right_temp;
                    if (!c->last_expr_is_float) {
                        /* Convert int to float via IR */
                        float_temp = c->next_temp_id++;
                        ir_emit_cvtsi2sd((uint16_t)float_temp, (uint16_t)right_temp);
                    }
                    /* For compound assignment, load current value, operate, then store */
                    if (is_compound) {
                        int lhs_temp = c->next_temp_id++;
                        ir_emit_fload((uint16_t)lhs_temp, TEMP_NONE, -sym->stack_offset);
                        int op_res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_fdiv((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                            default: op_res = float_temp; break;
                        }
                        ir_emit_fstore((uint16_t)op_res, TEMP_NONE, -sym->stack_offset);
                        c->last_expr_is_float = 1;
                        return op_res;
                    } else {
                        /* Simple assignment: just store */
                        ir_emit_fstore((uint16_t)float_temp, TEMP_NONE, -sym->stack_offset);
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
                                case TK_DIV_ASSIGN: ir_emit_div(op_res, left_temp, right_temp); break;
                                case TK_MOD_ASSIGN: ir_emit_mod(op_res, left_temp, right_temp); break;
                                case TK_AND_ASSIGN: ir_emit_and(op_res, left_temp, right_temp); break;
                                case TK_OR_ASSIGN:  ir_emit_or(op_res, left_temp, right_temp); break;
                                case TK_XOR_ASSIGN: ir_emit_xor(op_res, left_temp, right_temp); break;
                                case TK_SHL_ASSIGN: ir_emit_shl(op_res, left_temp, right_temp); break;
                                case TK_SHR_ASSIGN: ir_emit_shr(op_res, left_temp, right_temp); break;
                            }
                            right_temp = op_res;
                        }
                        if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 16 && !is_compound) {
                            /* System V AMD64 ABI: sret buffer pointer in right_temp.
                             * Spill right_temp to stack so offset additions don't clobber it.
                             * Copy chunks from [right_temp + i*8] to local stack -(sym->stack_offset - i*8) */
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            int32_t buf_ptr_off = c->current_stack_frame;
                            ir_emit_store(right_temp, BE(c)->fp_reg, -buf_ptr_off);

                            int num_chunks = (sym->type.size + 7) / 8;
                            for (int i = 0; i < num_chunks; i++) {
                                size_t rem = sym->type.size - (size_t)(i * 8);
                                int chunk_temp = c->next_temp_id++;
                                int addr_temp;
                                if (i == 0) {
                                    addr_temp = c->next_temp_id++;
                                    ir_emit_load(addr_temp, BE(c)->fp_reg, -buf_ptr_off);
                                } else {
                                    int base_temp = c->next_temp_id++;
                                    ir_emit_load(base_temp, BE(c)->fp_reg, -buf_ptr_off);
                                    int off_temp = c->next_temp_id++;
                                    ir_emit_mov_imm(off_temp, i * 8);
                                    addr_temp = c->next_temp_id++;
                                    ir_emit_add(addr_temp, base_temp, off_temp);
                                }
                                int32_t dst_off = -(sym->stack_offset - i * 8);
                                if (rem >= 8) {
                                    ir_emit_deref(chunk_temp, addr_temp);
                                    ir_emit_store(chunk_temp, BE(c)->fp_reg, dst_off);
                                } else if (rem >= 4) {
                                    ir_emit_load_32(chunk_temp, addr_temp);
                                    int dst_addr = c->next_temp_id++;
                                    ir_emit_lea(dst_addr, TEMP_NONE, dst_off);
                                    ir_emit_store_32(chunk_temp, dst_addr);
                                } else if (rem >= 2) {
                                    ir_emit_load_16(chunk_temp, addr_temp);
                                    int dst_addr = c->next_temp_id++;
                                    ir_emit_lea(dst_addr, TEMP_NONE, dst_off);
                                    ir_emit_store_16(chunk_temp, dst_addr);
                                } else {
                                    ir_emit_load_byte(chunk_temp, addr_temp);
                                    int dst_addr = c->next_temp_id++;
                                    ir_emit_lea(dst_addr, TEMP_NONE, dst_off);
                                    ir_emit_store_byte(chunk_temp, dst_addr);
                                }
                            }
                        } else {
                            ir_emit_store(right_temp, BE(c)->fp_reg, -sym->stack_offset);
                            if (c->isa == ISA_RISCV32 && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 4 && sym->type.size <= 8 && !is_compound) {
                                /* RV32 ILP32 ABI: second 4-byte chunk returned in a1 (VREG_2) */
                                ir_emit_store(VREG_2, BE(c)->fp_reg, -(sym->stack_offset - 4));
                            } else if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16 && !is_compound) {
                                int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? 1 : 2;
                                ir_emit_store(reg2, BE(c)->fp_reg, -(sym->stack_offset - 8));
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
                    ir_emit_mov_global(gaddr_temp, gv->name);
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
                            if (gv_is_float) ir_emit_vloadss((uint8_t)curr_val, (uint16_t)gaddr_temp);
                            else ir_emit_vloadsd((uint8_t)curr_val, (uint16_t)gaddr_temp);
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
                        if (gv_is_float) ir_emit_vstoress(gaddr_temp, (uint8_t)final_val);
                        else ir_emit_vstoresd(gaddr_temp, (uint8_t)final_val);
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
                            if (xsz == 4) ir_emit_vloadss((uint8_t)curr_val, (uint16_t)xaddr);
                            else ir_emit_vloadsd((uint8_t)curr_val, (uint16_t)xaddr);
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
                        if (xsz == 4) ir_emit_vstoress(xaddr, (uint8_t)xfinal);
                        else ir_emit_vstoresd(xaddr, (uint8_t)xfinal);
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
                size_t elem_offset = 0;
                size_t elem_size = 4;
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
                            }
                        }
                    } else {
                        size_t cur_leaf = 0;
                        const char *flex_name = NULL;
                        if (!struct_resolve_scalar_leaf(&sym->type, idx, &cur_leaf, 0, &elem_offset, &elem_size, &flex_name)) {
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
                        size_t per = object_count_scalar_leaves(sym->type.base);
                        leaf_idx = a_idx * (per ? per : 1);
                    } else {
                        leaf_idx = idx;
                    }
                    if (object_resolve_scalar_leaf(&sym->type, leaf_idx, &off, &sz)) {
                        elem_offset = off;
                        elem_size = sz;
                    } else {
                        size_t unit_sz = (sym->type.base && sym->type.base->size > 0) ? sym->type.base->size : 4;
                        elem_offset = a_idx * unit_sz;
                        elem_size = unit_sz;
                    }
                } else {
                    elem_offset = 0;
                    elem_size = sym->type.size ? sym->type.size : 4;
                }
                int32_t store_offset = -(sym->stack_offset - (int32_t)elem_offset);
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
                    if (c->last_expr_is_float) {
                        ir_emit_vstoress((uint16_t)addr_temp, (uint16_t)val_temp);
                        c->last_expr_is_float = 0;
                    } else {
                        ir_emit_store_32(val_temp, addr_temp);
                    }
                } else {
                    if (c->last_expr_is_float) {
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
                    ir_emit_load(2, BE(c)->fp_reg, -(sym->stack_offset - 8));
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
                    ZeroCCSym *sym = zero_cc_find_local(c, ln->s.str);
                    if (sym && sym->type.base && sym->type.base->size > 0) {
                        elem_size = sym->type.base->size;
                    }
                } else if (ln && ln->tag == NODE_COMPOUND_LIT) {
                    ZeroCCSym *sym = (ln->s.str && ln->s.str[0]) ? zero_cc_find_local(c, ln->s.str) : NULL;
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
                    ZeroCCSym *sym = zero_cc_find_local(c, ln->s.str);
                    if (sym && sym->type.base && (sym->type.base->tag == TYPE_FLOAT || sym->type.base->tag == TYPE_DOUBLE)) {
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
                if (bs) {
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
                    if (member_is_array) {
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
                ZeroCCGlobalVar *ggv = sym ? NULL : find_global_var(base_node->s.str);
                if (ggv) {
                        /* Global struct/union base: load the variable address,
                         * add the member offset, then load/deref. */
                        int gaddr = c->next_temp_id++;
                        ir_emit_mov_global(gaddr, ggv->name);
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
                            if (member_sz == 4) ir_emit_vloadss((uint16_t)val_temp, (uint8_t)gaddr2);
                            else ir_emit_vloadsd((uint16_t)val_temp, (uint8_t)gaddr2);
                            c->last_expr_is_float = 1;
                            return val_temp;
                        }
                        if (member_is_array) {
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
                    if (member_is_array) {
                        ir_emit_mov_reg(val_temp, addr_temp);
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
                    if (member_is_array) {
                        ir_emit_lea(val_temp, BE(c)->fp_reg, total_offset);
                    } else {
                        ir_emit_load(val_temp, BE(c)->fp_reg, total_offset);
                    }
                }
            } else if (multi_addr == TEMP_NONE && base_node && base_node->tag == NODE_COMPOUND_LIT) {
                (void)lower_expr(c, arena, base_ref);
                ZeroCCSym *sym = (base_node->s.str && base_node->s.str[0]) ? zero_cc_find_local(c, base_node->s.str) : NULL;
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
                if (member_is_array) {
                    ir_emit_lea(val_temp, BE(c)->fp_reg, total_offset);
                } else {
                    ir_emit_load(val_temp, BE(c)->fp_reg, total_offset);
                }
            } else if (multi_addr == TEMP_NONE) {
                /* General expression returning struct (e.g. NODE_CALL returning buffer pointer) */
                int base_ptr = lower_expr(c, arena, base_ref);
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
                if (member_is_array) {
                    ir_emit_mov_reg(val_temp, addr_temp);
                } else if (member_is_float) {
                    if (member_sz == 4) ir_emit_vloadss((uint16_t)val_temp, (uint16_t)addr_temp);
                    else ir_emit_vloadsd((uint16_t)val_temp, (uint16_t)addr_temp);
                    c->last_expr_is_float = 1;
                    return val_temp;
                } else {
                    ir_emit_deref(val_temp, addr_temp);
                }
            }

            if (member_is_array) {
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
                ir_emit_mov_imm(mask_temp, (int32_t)mask);
                int masked_temp = c->next_temp_id++;
                ir_emit_and(masked_temp, val_temp, mask_temp);
                val_temp = masked_temp;

                if (is_signed && bit_width < 32) {
                    int32_t sign_bit = (int32_t)(1ULL << (bit_width - 1));
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
            /* Function call - handle float args directly in machine code, int args via IR */
            ZeroCCASTNode *callee = zero_cc_ast_get(arena, node->left);
            if (callee && callee->tag == NODE_IDENT) {
                const char *name = callee->s.str;

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

                /* Intercept atomic builtins */
                if (strcmp(name, "__sync_fetch_and_add") == 0 || strcmp(name, "__atomic_fetch_add") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int res_temp = c->next_temp_id++;
                    ir_emit_atomic_xadd(res_temp, ptr_temp, val_temp, 4);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_add_and_fetch") == 0 || strcmp(name, "__atomic_add_fetch") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int val_copy = c->next_temp_id++;
                    ir_emit_mov_reg(val_copy, val_temp);
                    int old_temp = c->next_temp_id++;
                    ir_emit_atomic_xadd(old_temp, ptr_temp, val_temp, 4);
                    int res_temp = c->next_temp_id++;
                    ir_emit_add(res_temp, old_temp, val_copy);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_fetch_and_sub") == 0 || strcmp(name, "__atomic_fetch_sub") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int neg_temp = c->next_temp_id++;
                    ir_emit_neg(neg_temp, val_temp);
                    int res_temp = c->next_temp_id++;
                    ir_emit_atomic_xadd(res_temp, ptr_temp, neg_temp, 4);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_sub_and_fetch") == 0 || strcmp(name, "__atomic_sub_fetch") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int val_copy = c->next_temp_id++;
                    ir_emit_mov_reg(val_copy, val_temp);
                    int neg_temp = c->next_temp_id++;
                    ir_emit_neg(neg_temp, val_temp);
                    int old_temp = c->next_temp_id++;
                    ir_emit_atomic_xadd(old_temp, ptr_temp, neg_temp, 4);
                    int res_temp = c->next_temp_id++;
                    ir_emit_sub(res_temp, old_temp, val_copy);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_val_compare_and_swap") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    ASTRef arg2 = (arg1 != AST_NULL) ? zero_cc_ast_get(arena, arg1)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int old_temp = lower_expr(c, arena, arg1);
                    int new_temp = lower_expr(c, arena, arg2);
                    int res_temp = c->next_temp_id++;
                    ir_emit_atomic_cmpxchg(res_temp, ptr_temp, old_temp, new_temp, 4);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_bool_compare_and_swap") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    ASTRef arg2 = (arg1 != AST_NULL) ? zero_cc_ast_get(arena, arg1)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int old_temp = lower_expr(c, arena, arg1);
                    int new_temp = lower_expr(c, arena, arg2);
                    int res_temp = c->next_temp_id++;
                    ir_emit_atomic_cmpxchg_bool(res_temp, ptr_temp, old_temp, new_temp, 4);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_lock_test_and_set") == 0 || strcmp(name, "__atomic_exchange_n") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int res_temp = c->next_temp_id++;
                    ir_emit_atomic_xchg(res_temp, ptr_temp, val_temp, 4);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_lock_release") == 0) {
                    ASTRef arg0 = node->right;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int zero_temp = c->next_temp_id++;
                    ir_emit_mov_imm(zero_temp, 0);
                    ir_emit_store_32(zero_temp, ptr_temp);
                    c->last_expr_is_float = 0;
                    return zero_temp;
                }
                if (strcmp(name, "__sync_synchronize") == 0) {
                    ir_emit_mfence();
                    int res_temp = c->next_temp_id++;
                    ir_emit_mov_imm(res_temp, 0);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__atomic_load_n") == 0) {
                    ASTRef arg0 = node->right;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int res_temp = c->next_temp_id++;
                    ir_emit_load_32(res_temp, ptr_temp);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__atomic_store_n") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int dummy_res = c->next_temp_id++;
                    ir_emit_atomic_xchg(dummy_res, ptr_temp, val_temp, 4);
                    c->last_expr_is_float = 0;
                    return dummy_res;
                }
                if (strcmp(name, "__atomic_compare_exchange_n") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    ASTRef arg2 = (arg1 != AST_NULL) ? zero_cc_ast_get(arena, arg1)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int exp_ptr_temp = lower_expr(c, arena, arg1);
                    int exp_val_temp = c->next_temp_id++;
                    ir_emit_load_32(exp_val_temp, exp_ptr_temp);
                    int desired_temp = lower_expr(c, arena, arg2);
                    int actual_old = c->next_temp_id++;
                    ir_emit_atomic_cmpxchg(actual_old, ptr_temp, exp_val_temp, desired_temp, 4);
                    ir_emit_store_32(actual_old, exp_ptr_temp);
                    int bool_res = c->next_temp_id++;
                    ir_emit_cmp(actual_old, exp_val_temp);
                    ir_emit_cond_set(bool_res, CC_EQ);
                    c->last_expr_is_float = 0;
                    return bool_res;
                }

                /* Collect argument temps for both integer and FP args */
                ASTRef arg = node->right;
                int all_arg_temps[IR_CALL_MAX_ARGS];
                int all_arg_is_fp[IR_CALL_MAX_ARGS];
                int all_arg_count = 0;
                int32_t int_spill_offsets[IR_CALL_MAX_ARGS];
                int32_t fp_spill_offsets[8];
                int int_arg_idx = 0;
                int fp_arg_idx = 0;
                int param_idx = 0;  /* declaration-order index into callee param_types */

                ZeroCCFunction *callee_fn = zero_cc_find_function(c, name);
                ZeroCCSym *callee_sym = (!callee_fn) ? zero_cc_find_local(c, name) : NULL;
                bool is_indirect_call = (callee_fn == NULL && callee_sym != NULL);

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
                            int prev_depth = c->lowering.inlining.active_depth;
                            int prev_ret_temp = c->lowering.inlining.return_target_temp;
                            uint16_t prev_ret_label = c->lowering.inlining.return_target_label;
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
                            c->lowering.inlining.param_count = (int)callee_fn->param_count;
                            for (int p = 0; p < (int)callee_fn->param_count && p < 8; p++) {
                                c->lowering.inlining.param_names[p] = callee_fn->param_names[p];
                                c->lowering.inlining.param_temps[p] = inlined_param_temps[p];
                            }

                            /* Lower inlined body statements into IR */
                            lower_node(c, arena, cloned_body);
                            ir_emit_label(inline_exit_label);

                            /* Restore previous inlining context */
                            c->lowering.inlining.active_depth = prev_depth;
                            c->lowering.inlining.return_target_temp = prev_ret_temp;
                            c->lowering.inlining.return_target_label = prev_ret_label;
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

                while (arg != AST_NULL) {
                    ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                    int temp = lower_expr(c, arena, arg);
                    /* C99 int width semantics: narrow int args are extended
                     * to full 64-bit width before being passed (the callee
                     * must see a true 32-bit value, not 64-bit overflow
                     * residue). No-op for pointers/structs/8-byte args. */
                    if (!c->last_expr_is_float && callee_fn &&
                        param_idx < (int)callee_fn->param_count) {
                        zero_cc_emit_int_extend(c, temp, &callee_fn->param_types[param_idx]);
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
                        c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                        c->current_stack_frame += 8;
                        int32_t slot = c->current_stack_frame;
                        ir_emit_fstore((uint16_t)temp, TEMP_NONE, -slot);
                        if (int_arg_idx < IR_CALL_MAX_ARGS) {
                            int_spill_offsets[int_arg_idx] = slot;
                            int_arg_idx++;
                        }
                    } else if (c->last_expr_is_float) {
                        /* Float argument: spill it to a stack slot via IR */
                        if (fp_arg_idx < 8) {
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            fp_spill_offsets[fp_arg_idx] = c->current_stack_frame;
                            ir_emit_fstore((uint16_t)temp, TEMP_NONE, -fp_spill_offsets[fp_arg_idx]);
                            fp_arg_idx++;
                        }
                    } else {
                        /* Integer or struct argument: spill to stack slot(s) */
                        if (int_arg_idx < IR_CALL_MAX_ARGS) {
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            int_spill_offsets[int_arg_idx] = c->current_stack_frame;
                            ir_emit_store(temp, BE(c)->fp_reg, -int_spill_offsets[int_arg_idx]);
                            int_arg_idx++;
                        }
                        if (arg_node && arg_node->tag == NODE_IDENT) {
                            ZeroCCSym *arg_sym = zero_cc_find_local(c, arg_node->s.str);
                            if (arg_sym && (arg_sym->type.tag == TYPE_STRUCT || arg_sym->type.tag == TYPE_UNION) && arg_sym->type.size > 8) {
                                int num_chunks = (int)((arg_sym->type.size + 7) / 8);
                                for (int ch = 1; ch < num_chunks && int_arg_idx < IR_CALL_MAX_ARGS; ch++) {
                                    int ch_temp = c->next_temp_id++;
                                    ir_emit_load(ch_temp, BE(c)->fp_reg, -(arg_sym->stack_offset - ch * 8));
                                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                    c->current_stack_frame += 8;
                                    int_spill_offsets[int_arg_idx] = c->current_stack_frame;
                                    ir_emit_store(ch_temp, BE(c)->fp_reg, -int_spill_offsets[int_arg_idx]);
                                    int_arg_idx++;
                                }
                            }
                        }
                    }
                    arg_node = zero_cc_ast_get(arena, arg);
                    arg = arg_node ? arg_node->next : AST_NULL;
                }

                /* Reload arguments from stack slots immediately before call */
                int int_reload_idx = 0;
                int fp_reload_idx = 0;

                bool callee_uses_sret = (call_ret_type.tag == TYPE_STRUCT || call_ret_type.tag == TYPE_UNION) && (call_ret_type.size > 16);
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

                    all_arg_temps[all_arg_count] = sret_buf_temp;
                    all_arg_is_fp[all_arg_count] = (c->isa == ISA_AARCH64) ? 3 : 0;
                    all_arg_count++;
                }

                /* Then reload user integer arguments */
                for (int a = 0; a < int_arg_idx; a++) {
                    int reload_temp = c->next_temp_id++;
                    ir_emit_load(reload_temp, BE(c)->fp_reg, -int_spill_offsets[a]);
                    all_arg_temps[all_arg_count] = reload_temp;
                    all_arg_is_fp[all_arg_count] = 0;
                    all_arg_count++;
                    int_reload_idx++;
                }

                /* Then reload FP arguments */
                for (int f = 0; f < fp_arg_idx; f++) {
                    int reload_temp = c->next_temp_id++;
                    ir_emit_fload(reload_temp, TEMP_NONE, -fp_spill_offsets[f]);
                    all_arg_temps[all_arg_count] = reload_temp;
                    all_arg_is_fp[all_arg_count] = 1;
                    all_arg_count++;
                    fp_reload_idx++;
                }

                /* Reload indirect callee pointer if needed */
                if (is_indirect_call && all_arg_count < IR_CALL_MAX_ARGS) {
                    int callee_reload_temp = c->next_temp_id++;
                    ir_emit_load(callee_reload_temp, BE(c)->fp_reg, -indirect_callee_spill_off);
                    all_arg_temps[all_arg_count] = callee_reload_temp;
                    all_arg_is_fp[all_arg_count] = 2; /* 2 marks indirect callee register */
                    all_arg_count++;
                }

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
                    ir_call_args_record(ir_get_count() - 1, all_arg_count, all_arg_temps, all_arg_is_fp);
                    /* Store fp_arg_idx in label_idx for variadic handling */
                    last_instr->label_idx = (uint16_t)fp_arg_idx;
                }

                /* Record float spill offsets for this call */
                if (c->lowering.ir_ctx.call_count < 64) {
                    int cidx = c->lowering.ir_ctx.call_count++;
                    c->lowering.ir_ctx.call_fp_spill_counts[cidx] = fp_arg_idx;
                    for (int f = 0; f < fp_arg_idx && f < BE(c)->conv->fp_arg_reg_count; f++) {
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
                return call_res_temp;
            }
            c->last_expr_is_float = 0;
            return VREG_0;
        }

        default:
            return TEMP_NONE;
    }
}

