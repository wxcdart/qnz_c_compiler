/* AST Lowering - Expression Lowering (AST to IR) */

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/isa/isa_backend.h"

/* C99 6.3.1.5 integer -> floating conversion, u64-aware (see header). */
int zc_emit_int_to_double(ZeroCCCompiler *c, int src_temp, const TypeInfo *src_ti) {
    bool u64 = src_ti && type_size(src_ti) == 8 &&
               ((src_ti->kind == TYPE_KIND_LONG && (src_ti->modifiers & TYPE_MOD_UNSIGNED) != 0) ||
                src_ti->kind == TYPE_KIND_POINTER ||
                (src_ti->modifiers & TYPE_MOD_UNSIGNED) != 0);
    int fp_res = c->next_temp_id++;
    if (!u64) {
        ir_emit_cvtsi2sd(c, (uint16_t)fp_res, (uint16_t)src_temp);
        c->last_expr_is_float = 1;
        return fp_res;
    }
    /* uint64 -> double: positive values convert directly; for values with
     * the high bit set, ((v >> 1) | (v & 1)) * 2.0 preserves the value.
     * (The >> must be logical: OP_SHR is.) */
    int zero_temp = c->next_temp_id++;
    ir_emit_mov_imm(c, zero_temp, 0);
    int is_ge = c->next_temp_id++;
    ir_emit_cmp(c, src_temp, zero_temp);
    ir_emit_cond_set(c, is_ge, CC_GE);
    uint16_t neg_label = c->ir_next_label++;
    uint16_t done_label = c->ir_next_label++;
    ir_emit_je(c, (uint16_t)is_ge, neg_label);
    ir_emit_cvtsi2sd(c, (uint16_t)fp_res, (uint16_t)src_temp);
    ir_emit_jmp(c, done_label);
    ir_emit_label(c, neg_label);
    int one_temp = c->next_temp_id++;
    ir_emit_mov_imm(c, one_temp, 1);
    int odd_temp = c->next_temp_id++;
    ir_emit_and(c, odd_temp, src_temp, one_temp);
    int shr_temp = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)shr_temp,
        .temp_src1 = (uint16_t)src_temp, .temp_src2 = (uint16_t)one_temp});
    int adj_temp = c->next_temp_id++;
    ir_emit_or(c, adj_temp, shr_temp, odd_temp);
    int half_d = c->next_temp_id++;
    ir_emit_cvtsi2sd(c, (uint16_t)half_d, (uint16_t)adj_temp);
    ir_emit_fadd(c, (uint16_t)fp_res, (uint16_t)half_d, (uint16_t)half_d);
    ir_emit_label(c, done_label);
    c->last_expr_is_float = 1;
    return fp_res;
}

/* Rodata offset of a double literal (8-byte aligned). Callers that emit
 * the same literal many times (e.g. the 0.0 imaginary part of complex
 * scalar conversions) should reuse one temp per loop rather than re-emit. */
int zc_rodata_double_offset(ZeroCCCompiler *c, double val) {
    size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
    if (pad) {
        uint8_t z8[8] = {0};
        zero_cc_buf_put_bytes(&c->rodata_section, z8, pad);
    }
    size_t off = c->rodata_section.length;
    zero_cc_buf_put_bytes(&c->rodata_section, &val, sizeof(double));
    return (int)off;
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
int lower_member_chain_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *head) {
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
        ZeroCCGlobalVar *ggv = (sym && !sym->is_extern) ? NULL : find_global_var(c, base_node->s.str);
        if (sym && !sym->is_extern) {
            addr = zc_sym_addr(c, sym, 0);  /* Sprint 72: data section for address-taken statics */
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
            ir_emit_lea(c, addr, BE(c)->fp_reg, -sym->stack_offset);
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
        bool is_arr_base = (i == n - 1) && zc_is_array_base(c, arena, m, base_node);
        if ((m->flags & NODE_MEMBER_IS_ARROW) && !skip_load && !is_arr_base) {
            int ptr = c->next_temp_id++;
            ir_emit_deref(c, ptr, addr);
            addr = ptr;
        }
        if (off != 0) {
            int o = c->next_temp_id++;
            ir_emit_mov_imm(c, o, off);
            int na = c->next_temp_id++;
            ir_emit_add(c, na, addr, o);
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
            if (sym->is_vla) {
                int addr_temp = c->next_temp_id++;
                ir_emit_load(c, addr_temp, BE(c)->fp_reg, -sym->stack_offset);
                return addr_temp;
            }
            /* Sprint 72: address-taken statics live in the data section. */
            return zc_sym_addr(c, sym, 0);
        }
        ZeroCCGlobalVar *gv = find_global_var(c, operand->s.str);
        if (gv) {
            int addr_temp = c->next_temp_id++;
            ir_emit_global_addr(c, addr_temp, gv);
            return addr_temp;
        }
        ZeroCCFunction *fn = zero_cc_find_function(c, operand->s.str);
        if (fn) {
            int addr_temp = zc_lower_fn_value(c, fn);
            if (addr_temp >= 0) return addr_temp;
            return -1;
        }
    } else if (operand->tag == NODE_COMPOUND_LIT) {
        (void)lower_expr(c, arena, ref);
        ZeroCCSym *sym = lowering_local_sym(c, operand, operand->s.str); /* index-based: name lookup misses post-bound __clit syms */
        if (sym) {
            int addr_temp = c->next_temp_id++;
            ir_emit_lea(c, addr_temp, TEMP_NONE, -sym->stack_offset);
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
        if (base_is_ptr && zc_is_array_base(c, arena, innermost, base_node)) base_is_ptr = false;
        if (!base_is_ptr && base_node && base_node->tag == NODE_IDENT) {
            ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
            if (sym && !sym->is_extern) {
                if (zc_sym_uses_data_section(sym)) {
                    /* Sprint 72: address-taken static member address goes to
                     * the data-section object + member offset. */
                    ir_emit_mov_global(c, (uint16_t)addr_temp, sym->static_global_name);
                    if (member_offset != 0) {
                        int off_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, off_temp, member_offset);
                        int sum_temp = c->next_temp_id++;
                        ir_emit_add(c, sum_temp, addr_temp, off_temp);
                        return sum_temp;
                    }
                    return addr_temp;
                }
                int32_t total_offset = -sym->stack_offset + member_offset;
                ir_emit_lea(c, addr_temp, TEMP_NONE, total_offset);
                return addr_temp;
            }
            ZeroCCGlobalVar *bgv = find_global_var(c, base_node->s.str);
            if (bgv) {
                ir_emit_global_addr(c, addr_temp, bgv);
                if (member_offset != 0) {
                    int off_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, off_temp, member_offset);
                    int sum_temp = c->next_temp_id++;
                    ir_emit_add(c, sum_temp, addr_temp, off_temp);
                    addr_temp = sum_temp;
                }
                return addr_temp;
            }
        } else if (!base_is_ptr && base_node && base_node->tag == NODE_COMPOUND_LIT) {
            (void)lower_expr(c, arena, base_ref);
            ZeroCCSym *sym = lowering_local_sym(c, base_node, base_node->s.str); /* index-based: name lookup misses post-bound __clit syms */
            if (sym) {
                int32_t total_offset = -sym->stack_offset + member_offset;
                ir_emit_lea(c, addr_temp, TEMP_NONE, total_offset);
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
            ir_emit_mov_imm(c, off_temp, member_offset);
            ir_emit_add(c, addr_temp, base_ptr, off_temp);
        } else {
            ir_emit_mov_reg(c, addr_temp, base_ptr);
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
        /* Mirror-form subscript `int[(ptr)]` (C a[b] == *(a+b) == b[a]):
         * the POINTER is the index operand, so the integer base is an
         * ELEMENT OFFSET scaled by the pointee size and added to the
         * pointer value:  N[(int*)y] == *(y + N*4) == y[N].  Scaling the
         * index instead would scale the pointer (wild dereference -
         * c-torture pr22061-1 `N[(char*)x]` only worked by luck because
         * the char element size is 1; `N[(int*)y]` needs this). */
        bool mirror_form = false;
        size_t mirror_esz = 1;
        {
            TypeInfo *idx_ti = ast_node_get_typeinfo(c, zero_cc_ast_get(arena, operand->right));
            mirror_form = (idx_ti != NULL && idx_ti->kind == TYPE_KIND_POINTER);
            if (mirror_form && elem_ti &&
                elem_ti->kind != TYPE_KIND_VOID && type_size(elem_ti) > 0)
                mirror_esz = type_size(elem_ti);
        }
        ZeroCCSym *vla_2d_sym = NULL;
        {
            ZeroCCASTNode *ln = zero_cc_ast_get(arena, operand->left);
            if (ln && ln->tag == NODE_IDENT) {
                ZeroCCSym *ls = zero_cc_find_local(c, ln->s.str);
                if (ls && ls->is_vla && ls->type.base && ls->type.base->tag == TYPE_ARRAY && ls->type.base->is_vla) {
                    vla_2d_sym = ls;
                }
            }
        }
        int scaled_index;
        if (mirror_form) {
            scaled_index = TEMP_NONE; /* addr = index + scaled BASE below */
        } else if (vla_2d_sym) {
            int stride_temp = c->next_temp_id++;
            if (vla_2d_sym->is_param && vla_2d_sym->vla_slot_off > 0) {
                /* VLA PARAMETER: the entry code stores the raw innermost
                 * dim value in vla_slot_off (C99 6.7.6.2); the row stride
                 * is dim * leaf element size.  Reading the local-VLA
                 * -stack_offset+16 slot instead picks up uninitialised
                 * stack (c-torture pr22061-1: `char a[2][N]`). */
                ir_emit_load(c, stride_temp, BE(c)->fp_reg, -vla_2d_sym->vla_slot_off);
                const ZeroCCType *leaf = vla_2d_sym->type.base;
                while (leaf && leaf->tag == TYPE_ARRAY) leaf = leaf->base;
                size_t leaf_sz = (leaf && leaf->size > 0) ? leaf->size : 1;
                if (leaf_sz != 1) {
                    int ls_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, ls_temp, (int32_t)leaf_sz);
                    int s2 = c->next_temp_id++;
                    ir_emit_mul(c, s2, stride_temp, ls_temp);
                    stride_temp = s2;
                }
            } else {
                ir_emit_load(c, stride_temp, BE(c)->fp_reg, -(vla_2d_sym->stack_offset - 16));
            }
            scaled_index = c->next_temp_id++;
            ir_emit_mul(c, scaled_index, index_temp, stride_temp);
        } else if (elem_size == 1) {
            scaled_index = index_temp;
        } else if (elem_size == 2 || elem_size == 4 || elem_size == 8) {
            int shift = (elem_size == 2) ? 1 : ((elem_size == 4) ? 2 : 3);
            int sh_temp = c->next_temp_id++;
            ir_emit_mov_imm(c, sh_temp, shift);
            scaled_index = c->next_temp_id++;
            ir_emit_shl(c, scaled_index, index_temp, sh_temp);
        } else {
            int sz_temp = c->next_temp_id++;
            ir_emit_mov_imm(c, sz_temp, (int32_t)elem_size);
            scaled_index = c->next_temp_id++;
            ir_emit_mul(c, scaled_index, index_temp, sz_temp);
        }
        int addr_temp = c->next_temp_id++;
        if (mirror_form) {
            int base_scaled = array_temp;
            if (mirror_esz != 1) {
                if (mirror_esz == 2 || mirror_esz == 4 || mirror_esz == 8) {
                    int shift = (mirror_esz == 2) ? 1 : ((mirror_esz == 4) ? 2 : 3);
                    int sh_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, sh_temp, shift);
                    base_scaled = c->next_temp_id++;
                    ir_emit_shl(c, base_scaled, array_temp, sh_temp);
                } else {
                    int sz_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, sz_temp, (int32_t)mirror_esz);
                    base_scaled = c->next_temp_id++;
                    ir_emit_mul(c, base_scaled, array_temp, sz_temp);
                }
            }
            ir_emit_add(c, addr_temp, index_temp, base_scaled);
        } else {
            ir_emit_add(c, addr_temp, array_temp, scaled_index);
        }
        return addr_temp;
    } else if (operand->tag == NODE_UNOP && (operand->aux.op == '*' || operand->aux.op == TK_STAR)) {
        return lower_expr(c, arena, operand->left);
    } else if (operand->tag == NODE_CALL) {
        ZeroCCASTNode *cal = zero_cc_ast_get(arena, operand->left);
        if (cal && cal->tag == NODE_IDENT && cal->s.str &&
            (strcmp(cal->s.str, "__real") == 0 ||
             strcmp(cal->s.str, "__real__") == 0 ||
             strcmp(cal->s.str, "__imag") == 0 ||
             strcmp(cal->s.str, "__imag__") == 0)) {
            bool is_imag = (cal->s.str[2] == 'i');
            int addr = lower_expr(c, arena, operand->right);
            if (!is_imag) return addr;
            ZeroCCASTNode *cx_node = zero_cc_ast_get(arena, operand->right);
            TypeInfo *cx_ti = ast_node_get_typeinfo(c, cx_node);
            size_t esz = (cx_ti && cx_ti->kind == TYPE_KIND_COMPLEX && cx_ti->details.array.base)
                         ? type_size(cx_ti->details.array.base) : 8;
            if (!cx_ti && cx_node && cx_node->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, cx_node->s.str);
                if (sym && zero_cc_type_is_complex(&sym->type) && sym->type.base)
                    esz = sym->type.base->size;
                else {
                    ZeroCCGlobalVar *gv = find_global_var(c, cx_node->s.str);
                    if (gv && zero_cc_type_is_complex(&gv->type) && gv->type.base)
                        esz = gv->type.base->size;
                }
            }
            if (esz != 1 && esz != 2 && esz != 4 && esz != 8) esz = 8;
            int off = c->next_temp_id++;
            ir_emit_mov_imm(c, off, (int)esz);
            int la = c->next_temp_id++;
            ir_emit_add(c, la, addr, off);
            return la;
        }
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
    bool rhs_is_vec_addr = ast_expr_is_vector_addr(c, arena, rhs_ref);
    bool src_is_vec = src_sym && src_sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&src_sym->type);

    /* 9-16B by-value CALL result: the value is the VREG_0/VREG_2 register
     * pair. Capture BOTH halves into fresh allocatable temps immediately -
     * any address arithmetic emitted later in this copy (dst+8) may legally
     * reuse rax/rdx and would otherwise destroy the live return value
     * (c-torture execute/pr92904: `v.c = f2(0, u.c)` stored the destination
     * ADDRESS as the upper chunk). Vector-typed calls are NOT included:
     * they return in address form like every other vector value. */
    {
        TypeInfo *c_ti = ast_node_get_typeinfo(c, rnode);
        bool struct_ret_call = rnode && rnode->tag == NODE_CALL && c_ti &&
            (c_ti->kind == TYPE_KIND_STRUCT || c_ti->kind == TYPE_KIND_UNION);
        if (struct_ret_call && struct_sz > 8 && struct_sz <= 16 && !src_sym) {
            int pair0 = c->next_temp_id++;
            ir_emit_mov_reg(c, pair0, 0);
            int pair1 = c->next_temp_id++;
            int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? 1 : 2;
            ir_emit_mov_reg(c, pair1, reg2);
            if (struct_sz >= 8) ir_emit_store_64(c, pair0, dst_addr);
            else if (struct_sz >= 4) ir_emit_store_32(c, pair0, dst_addr);
            else if (struct_sz >= 2) ir_emit_store_16(c, pair0, dst_addr);
            else ir_emit_store_byte(c, pair0, dst_addr);
            if (struct_sz > 8) {
                int off_t = c->next_temp_id++;
                ir_emit_mov_imm(c, off_t, 8);
                int cur_dst = c->next_temp_id++;
                ir_emit_add(c, cur_dst, dst_addr, off_t);
                size_t rem = struct_sz - 8;
                if (rem >= 8) ir_emit_store_64(c, pair1, cur_dst);
                else zc_store_low_bytes(c, pair1, cur_dst, rem);
            }
            return;
        }
    }

    for (int i = 0; i < num_chunks; i++) {
        size_t rem = struct_sz - (size_t)(i * 8);
        int ch_val = c->next_temp_id++;
        int cur_dst = dst_addr;
        if (i > 0) {
            int off_t = c->next_temp_id++;
            ir_emit_mov_imm(c, off_t, i * 8);
            cur_dst = c->next_temp_id++;
            ir_emit_add(c, cur_dst, dst_addr, off_t);
        }
        if (src_sym && (src_sym->type.tag == TYPE_STRUCT || src_sym->type.tag == TYPE_UNION)) {
            ir_emit_load(c, ch_val, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
        } else if (src_is_vec) {
            /* GNU vector local: 8-byte chunks from its frame slot */
            ir_emit_load(c, ch_val, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
        } else if (rhs_is_struct_addr || rhs_is_vec_addr) {
            int cur_src = rhs_temp;
            if (i > 0) {
                int off_t = c->next_temp_id++;
                ir_emit_mov_imm(c, off_t, i * 8);
                cur_src = c->next_temp_id++;
                ir_emit_add(c, cur_src, rhs_temp, off_t);
            }
            if (rem >= 8) ir_emit_deref(c, ch_val, cur_src);
            else if (rem >= 4) ir_emit_load_32(c, ch_val, cur_src);
            else if (rem >= 2) ir_emit_load_16(c, ch_val, cur_src);
            else ir_emit_load_byte(c, ch_val, cur_src);
        } else if (i == 0) {
            ch_val = rhs_temp;
        } else if (i == 1 && struct_sz <= 16) {
            /* 9-16B by-value call result: upper 8 bytes in the ABI
             * secondary register. Copy it into a fresh allocatable temp
             * first - using the raw VREG_2 temp id as a store source
             * collides with the allocator assigning that same physical
             * register (rdx) to the destination-address temp below
             * (c-torture execute/pr92904: `v.c = f2(0, u.c)`). */
            int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? 1 : 2;
            int ch2 = c->next_temp_id++;
            ir_emit_mov_reg(c, ch2, reg2);
            ch_val = ch2;
        }
        if (rem >= 8) {
            ir_emit_store_64(c, ch_val, cur_dst);
        } else if (rem == 4 || rem == 2 || rem == 1) {
            /* Tail width matches the single narrow load above. */
            if (rem == 4) ir_emit_store_32(c, ch_val, cur_dst);
            else if (rem == 2) ir_emit_store_16(c, ch_val, cur_dst);
            else ir_emit_store_byte(c, ch_val, cur_dst);
        } else if ((src_sym && (src_sym->type.tag == TYPE_STRUCT || src_sym->type.tag == TYPE_UNION)) || src_is_vec) {
            /* Tail of 3,5,6,7 bytes from a local frame object: byte-by-byte
             * via a frame-relative LEA (no shifts - the 8-byte load's value
             * would need shuffles, and those temps trip the allocator in
             * large functions; c-torture execute/va-arg-22). */
            int tail_a = c->next_temp_id++;
            ir_emit_lea(c, tail_a, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
            for (size_t b = 0; b < rem; b++) {
                int sv = c->next_temp_id++;
                int sa = tail_a;
                int da = cur_dst;
                if (b) {
                    int t = c->next_temp_id++;
                    ir_emit_mov_imm(c, t, (int)b);
                    sa = c->next_temp_id++;
                    ir_emit_add(c, sa, tail_a, t);
                    int t2 = c->next_temp_id++;
                    ir_emit_mov_imm(c, t2, (int)b);
                    da = c->next_temp_id++;
                    ir_emit_add(c, da, cur_dst, t2);
                }
                ir_emit_load_byte(c, sv, sa);
                ir_emit_store_byte(c, sv, da);
            }
        } else if (rhs_is_struct_addr || rhs_is_vec_addr) {
            /* Tail of 3,5,6,7 bytes from a memory source: byte-by-byte (an
             * 8-byte load would overrun the object). */
            for (size_t b = 0; b < rem; b++) {
                int sv = c->next_temp_id++;
                int sa = rhs_temp;
                size_t soff = (size_t)(i * 8 + b);
                if (soff) {
                    int t = c->next_temp_id++;
                    ir_emit_mov_imm(c, t, (int)soff);
                    sa = c->next_temp_id++;
                    ir_emit_add(c, sa, rhs_temp, t);
                }
                ir_emit_load_byte(c, sv, sa);
                int da = cur_dst;
                if (b) {
                    int t = c->next_temp_id++;
                    ir_emit_mov_imm(c, t, (int)b);
                    da = c->next_temp_id++;
                    ir_emit_add(c, da, cur_dst, t);
                }
                ir_emit_store_byte(c, sv, da);
            }
        } else {
            /* Small struct (<8B) by-value / register-pair result: the value
             * lives in a temp - store its low rem bytes (shifted). */
            zc_store_low_bytes(c, ch_val, cur_dst, rem);
        }
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
        case NODE_VLA_SLOT:
            return 0;
        case NODE_CAST: {
            TypeInfo *ti = ast_node_get_typeinfo(c, n);
            return ti ? type_is_floating(ti) : 0;
        }
        case NODE_IDENT: {
            ZeroCCSym *s = zero_cc_find_local(c, n->s.str);
            if (s) return s->type.tag == TYPE_FLOAT || s->type.tag == TYPE_DOUBLE;
            ZeroCCGlobalVar *g = find_global_var(c, n->s.str);
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
    if (t->tag == TYPE_USER_0 && zero_cc_type_is_vector(t)) {
        if (!t->base) return false;
        size_t el_sz = t->base->size ? t->base->size : 4;
        return zc_type_offset_is_float(t->base, el_sz ? (off % el_sz) : 0);
    }
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
            ir_emit_mov_imm(c, temp, node->int_val);
            c->last_expr_is_float = 0;
            return temp;
        }

        case NODE_VLA_SLOT: {
            /* Runtime VLA object size (C99 6.5.3.4): load the per-
             * declaration frame slot (VLA param: dim value evaluated at
             * entry; VLA local: allocation size written at the
             * declaration). int_val = c->locals index. */
            int li = (int)node->int_val;
            int temp = c->next_temp_id++;
            if (li >= 0 && (size_t)li < c->local_count && c->locals[li].vla_slot_off > 0) {
                ir_emit_load(c, temp, BE(c)->fp_reg, -c->locals[li].vla_slot_off);
            } else {
                ir_emit_mov_imm(c, temp, 0);  /* defensive: no slot reserved */
            }
            c->last_expr_is_float = 0;
            return temp;
        }

        case NODE_BOOL: {
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(c, temp, node->int_val);
            c->last_expr_is_float = 0;
            return temp;
        }

        case NODE_NULLPTR: {
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(c, temp, 0);
            c->last_expr_is_float = 0;
            return temp;
        }

        case NODE_FLOAT: {
            /* C99 6.2.5p10: an imaginary floating literal (`1.0iF`) is the
             * complex value 0 + v*i (value in the imaginary part).
             * Materialize an address-form complex object: (0.0, val). */
            if (node->flags & 8u) {
                size_t cesz = (node->flags & 1u) ? 4 : 8; /* l/L: double model */
                c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + (int32_t)(cesz * 2) + 7) & ~7;
                int32_t coff = c->current_stack_frame;
                double val = node->float_val;
                size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
                if (pad) { uint8_t z8[8] = {0}; zero_cc_buf_put_bytes(&c->rodata_section, z8, pad); }
                size_t ro_off = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, &val, sizeof(double));
                int ca = c->next_temp_id++;
                ir_emit_lea(c, ca, BE(c)->fp_reg, -coff);
                int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, (int)cesz);
                int a2 = c->next_temp_id++; ir_emit_add(c, a2, ca, o2);
                int zr = c->next_temp_id++;
                ir_emit_fmov_imm(c, (uint16_t)zr, (int64_t)zc_rodata_double_offset(c, 0.0));
                int vv = c->next_temp_id++;
                ir_emit_fmov_imm(c, (uint16_t)vv, (int64_t)ro_off);
                if (cesz == 4) {
                    ir_emit_vstoress(c, (uint16_t)ca, (uint16_t)zr);
                    ir_emit_vstoress(c, (uint16_t)a2, (uint16_t)vv);
                } else {
                    ir_emit_fstore(c, (uint16_t)zr, TEMP_NONE, -coff);
                    ir_emit_fstore(c, (uint16_t)vv, TEMP_NONE, -coff + (int)cesz);
                }
                c->last_expr_is_float = 0;
                return ca;
            }
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
            ir_emit_fmov_imm(c, (uint16_t)temp, (int64_t)rodata_offset);
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
                    ir_emit_lea_rip(c, temp, rodata_offset);
                } else {
                    size_t rodata_offset = c->rodata_section.length;
                    zero_cc_buf_put_bytes(&c->rodata_section, node->s.str, len);
                    uint8_t zero = 0;
                    zero_cc_buf_put_bytes(&c->rodata_section, &zero, 1);
                    ir_emit_lea_rip(c, temp, rodata_offset);
                }
            } else {
                ir_emit_mov_imm(c, temp, 0);
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
                ir_emit_lea_rip(c, temp, rodata_offset);
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
                if (zero_cc_type_is_complex(&sym->type)) {
                    /* C99 _Complex: address-form value (16-byte object,
                     * real half first) - like a vector, consumers load
                     * the parts from this address. */
                    int temp = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                    c->last_expr_is_float = 0;
                    return temp;
                }
                if (sym->type.tag == TYPE_FLOAT) {
                    int addr = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                    int temp = c->next_temp_id++;
                    ir_emit_vloadss(c, (uint16_t)temp, (uint16_t)addr);
                    c->last_expr_is_float = 1;
                    return temp;
                } else if (sym->type.tag == TYPE_DOUBLE) {
                    /* Float load via IR for proper register allocation.
                     * Sprint 72: address-based (data section for address-
                     * taken statics, frame slot otherwise). */
                    int a = zc_sym_addr(c, sym, 0);
                    int temp = c->next_temp_id++;
                    ir_emit_vloadsd(c, (uint16_t)temp, (uint16_t)a);
                    c->last_expr_is_float = 1;
                    return temp;
                } else if (sym->type.tag == TYPE_ARRAY && !sym->is_vla) {
                    /* Fixed-size array decays to pointer (Sprint 72: data
                     * section for address-taken statics). */
                    int temp = zc_sym_addr(c, sym, 0);
                    c->last_expr_is_float = 0;
                    return temp;
                } else if (sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&sym->type)) {
                    /* GNU vector: decays to its address, like an array.
                     * Sprint 72: data section for address-taken statics. */
                    int temp = zc_sym_addr(c, sym, 0);
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
                        zc_sym_load(c, sym, 0, temp);  /* Sprint 72 */
                        c->last_expr_is_float = 0;
                        return temp;
                    } else if (sym->type.size <= 16) {
                        int temp = c->next_temp_id++;
                        zc_sym_load(c, sym, 0, temp);  /* Sprint 72 */
                        zc_sym_load(c, sym, 8, VREG_2);  /* Sprint 72 */
                        c->last_expr_is_float = 0;
                        return temp;
                    } else {
                        int temp = c->next_temp_id++;
                        zc_sym_load(c, sym, 0, temp);  /* Sprint 72 */
                        c->last_expr_is_float = 0;
                        return temp;
                    }
                } else {
                    int temp = c->next_temp_id++;
                    zc_sym_load(c, sym, 0, temp);  /* Sprint 72 */
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

                ZeroCCGlobalVar *gv = find_global_var(c, name);
                if (gv) {

                    int addr_temp = c->next_temp_id++;
                    ir_emit_global_addr(c, addr_temp, gv);
                    /* Arrays, structs and vectors decay/return address, scalars load value */
                    if (gv->type.tag == TYPE_ARRAY || gv->type.tag == TYPE_STRUCT || gv->type.tag == TYPE_UNION ||
                        (gv->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&gv->type)) ||
                        zero_cc_type_is_complex(&gv->type)) {
                        /* _Complex global: address-form value (16-byte
                         * object, real half first) - consumers load the
                         * two parts from this address. */
                        c->last_expr_is_float = 0;
                        return addr_temp;
                    }
                    int val_temp = c->next_temp_id++;
                    if (gv->type.tag == TYPE_FLOAT) {
                        ir_emit_vloadss(c, (uint16_t)val_temp, (uint16_t)addr_temp);
                        c->last_expr_is_float = 1;
                        return val_temp;
                    }
                    if (gv->type.tag == TYPE_DOUBLE) {
                        ir_emit_vloadsd(c, (uint16_t)val_temp, (uint16_t)addr_temp);
                        c->last_expr_is_float = 1;
                        return val_temp;
                    }
                    size_t gv_sz = gv->size > 0 ? gv->size : 4;
                    if (gv_sz == 1) ir_emit_load_byte(c, val_temp, addr_temp);
                    else if (gv_sz == 2) ir_emit_load_16(c, val_temp, addr_temp);
                    else if (gv_sz == 4) ir_emit_load_32(c, val_temp, addr_temp);
                    else ir_emit_deref(c, val_temp, addr_temp);
                    /* Width-aware loads zero-extend; complete sign extension
                     * for signed narrow ints (e.g. `int g=-1`). */
                    zero_cc_emit_int_extend(c, val_temp, &gv->type);
                    c->last_expr_is_float = 0;
                    return val_temp;
                }
                ZeroCCEnumConst *ec = zero_cc_find_enum_const(c, name);
                if (ec) {
                    int temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, temp, ec->value);
                    c->last_expr_is_float = 0;
                    return temp;
                }
                ZeroCCFunction *fn = zero_cc_find_function(c, name);
                if (fn) {

                    int temp = zc_lower_fn_value(c, fn);
                    if (temp < 0) return -1;
                    if (fn->is_external) {
                        /* Extern object: load its value through the patched
                         * address. FP types must stay in the FP pipeline -
                         * an integer deref would load the IEEE-754 bits
                         * (cross-TU `double g;` regression). */
                        if (fn->return_type.tag == TYPE_FLOAT) {
                            int ftemp = c->next_temp_id++;
                            ir_emit_vloadss(c, (uint16_t)ftemp, (uint16_t)temp);
                            c->last_expr_is_float = 1;
                            return ftemp;
                        }
                        if (fn->return_type.tag == TYPE_DOUBLE) {
                            int ftemp = c->next_temp_id++;
                            ir_emit_vloadsd(c, (uint16_t)ftemp, (uint16_t)temp);
                            c->last_expr_is_float = 1;
                            return ftemp;
                        }
                        if (fn->return_type.tag == TYPE_ARRAY) {
                            /* Extern array (`extern unsigned long aa[];`): the
                             * identifier decays to the array's address - no
                             * value load (a load + later deref would
                             * double-indirect through the first element;
                             * c-torture execute/20000403-1). */
                            c->last_expr_is_float = 0;
                            return temp;
                        }
                        int val_temp = c->next_temp_id++;
                        size_t ext_sz = fn->return_type.size > 0 ? fn->return_type.size : 8;
                        if (ext_sz == 1) ir_emit_load_byte(c, val_temp, temp);
                        else if (ext_sz == 2) ir_emit_load_16(c, val_temp, temp);
                        else if (ext_sz == 4) ir_emit_load_32(c, val_temp, temp);
                        else ir_emit_deref(c, val_temp, temp);
                        c->last_expr_is_float = 0;
                        return val_temp;
                    }
                    c->last_expr_is_float = 0;
                    return temp;
                }
                return TEMP_NONE;
            }
        }

        case NODE_BINOP:
            return lower_expr_binop(c, arena, node);

        case NODE_COMMA:
            lower_expr(c, arena, node->left);
            return lower_expr(c, arena, node->right);

        case NODE_LABEL_ADDR: {
            uint16_t lbl = get_or_create_user_ir_label(c, node->s.str);
            int temp = c->next_temp_id++;
            ir_emit_lea_label(c, (uint16_t)temp, lbl);
            c->last_expr_is_float = 0;
            return temp;
        }

        case NODE_UNOP:
            return lower_expr_unop(c, arena, node);

        case NODE_CAST:
            return lower_expr_cast(c, arena, node);

        case NODE_VA_START:
            return lower_expr_va_start(c, arena, node);

        case NODE_VA_ARG:
            return lower_expr_va_arg(c, arena, node);

        case NODE_ASSIGN:
            return lower_expr_assign(c, arena, node);

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
                ir_emit_mov_imm(c, zero_i, 0);
                int zero_f = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
                int cmp_i = c->next_temp_id++;
                ir_emit_cmp(c, cond_temp, zero_f);
                ir_emit_cond_set(c, cmp_i, CC_NE);
                cond_temp = cmp_i;
                c->last_expr_is_float = 0;
            }
            int res_temp = c->next_temp_id++;
            uint16_t else_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;

            int else_may_be_fp = ast_expr_is_fp(c, arena, node->aux.alt);

            ir_emit_je(c, (uint16_t)cond_temp, else_label);

            int then_temp = lower_expr(c, arena, node->right);
            int then_fp = c->last_expr_is_float;

            if (then_fp || else_may_be_fp) {
                if (then_fp) {
                    ir_emit_fmov(c, (uint16_t)res_temp, (uint16_t)then_temp);
                } else {
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)then_temp);
                    ir_emit_fmov(c, (uint16_t)res_temp, (uint16_t)conv);
                }
            } else {
                ir_emit_mov_reg(c, res_temp, then_temp);
            }
            ir_emit_jmp(c, end_label);

            ir_emit_label(c, else_label);
            int else_temp = lower_expr(c, arena, node->aux.alt);
            int else_fp = c->last_expr_is_float;
            int fp_result = then_fp || else_fp || else_may_be_fp;

            if (fp_result) {
                if (else_fp) {
                    ir_emit_fmov(c, (uint16_t)res_temp, (uint16_t)else_temp);
                } else {
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)else_temp);
                    ir_emit_fmov(c, (uint16_t)res_temp, (uint16_t)conv);
                }
            } else {
                ir_emit_mov_reg(c, res_temp, else_temp);
            }

            ir_emit_label(c, end_label);
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
                ir_emit_mov_imm(c, zero_temp, 0);
                size_t offset = 0;
                while (offset < total_size) {
                    size_t chunk = total_size - offset;
                    if (chunk >= 8) chunk = 8;
                    else if (chunk >= 4) chunk = 4;
                    else if (chunk >= 2) chunk = 2;
                    else chunk = 1;
                    int32_t store_offset = -(sym->stack_offset - (int32_t)offset);
                    if (chunk == 8) {
                        ir_emit_store(c, zero_temp, BE(c)->fp_reg, store_offset);
                    } else if (chunk == 4) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(c, addr_temp, TEMP_NONE, store_offset);
                        ir_emit_store_32(c, zero_temp, addr_temp);
                    } else if (chunk == 2) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(c, addr_temp, TEMP_NONE, store_offset);
                        ir_emit_store_16(c, zero_temp, addr_temp);
                    } else {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(c, addr_temp, TEMP_NONE, store_offset);
                        ir_emit_store_byte(c, zero_temp, addr_temp);
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
                        ir_emit_lea(c, addr_temp, TEMP_NONE, abs_store_off);
                        int cur_temp = c->next_temp_id++;
                        if (abs_sz == 1) ir_emit_load_byte(c, cur_temp, addr_temp);
                        else if (abs_sz == 2) ir_emit_load_16(c, cur_temp, addr_temp);
                        else if (abs_sz == 4) ir_emit_load_32(c, cur_temp, addr_temp);
                        else ir_emit_deref(c, cur_temp, addr_temp);
                        int cmask_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, cmask_temp, (int64_t)(~fmask));
                        int cleared_temp = c->next_temp_id++;
                        ir_emit_and(c, cleared_temp, cur_temp, cmask_temp);
                        int vmask_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, vmask_temp, (int64_t)vmask);
                        int mval_temp = c->next_temp_id++;
                        ir_emit_and(c, mval_temp, val_temp, vmask_temp);
                        if (boff > 0) {
                            int sh_temp = c->next_temp_id++;
                            ir_emit_mov_imm(c, sh_temp, (int32_t)boff);
                            int shval_temp = c->next_temp_id++;
                            ir_emit_shl(c, shval_temp, mval_temp, sh_temp);
                            mval_temp = shval_temp;
                        }
                        int new_temp = c->next_temp_id++;
                        ir_emit_or(c, new_temp, cleared_temp, mval_temp);
                        int saddr_temp = c->next_temp_id++;
                        ir_emit_lea(c, saddr_temp, TEMP_NONE, abs_store_off);
                        if (abs_sz == 1) ir_emit_store_byte(c, new_temp, saddr_temp);
                        else if (abs_sz == 2) ir_emit_store_16(c, new_temp, saddr_temp);
                        else if (abs_sz == 4) ir_emit_store_32(c, new_temp, saddr_temp);
                        else ir_emit_store(c, new_temp, BE(c)->fp_reg, abs_store_off);
                        cur_ref = elem_node->next;
                        continue;
                    }
                    /* Struct element whose RHS lowers to an address (a struct
                     * member like `s->d` in `{ ..., s->d }`): copy the whole
                     * element, not the 8-byte pointer (c-torture 20071202-1).
                     * The ABS size code caps at 8, so recover the true member
                     * size from the type layout when the target is a struct. */
                    if (abs_sz >= 8 && ast_expr_is_struct_addr(c, arena, cur_ref)) {
                        size_t real_sz = abs_sz;
                        if (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) {
                            for (size_t mi = 0; mi < sym->type.member_count; mi++) {
                                if (sym->type.members[mi].offset == abs_off) {
                                    if (sym->type.members[mi].tag == TYPE_STRUCT ||
                                        sym->type.members[mi].tag == TYPE_UNION) {
                                        real_sz = sym->type.members[mi].size;
                                    }
                                    break;
                                }
                            }
                        }
                        int dst_t = c->next_temp_id++;
                        ir_emit_lea(c, dst_t, TEMP_NONE, abs_store_off);
                        zero_cc_emit_struct_copy_to_addr(c, arena, dst_t, real_sz, cur_ref, val_temp);
                        c->last_expr_is_float = 0;
                        cur_ref = elem_node->next;
                        continue;
                    }
                    bool abs_is_float = zc_type_offset_is_float(&sym->type, abs_off);
                    if (abs_is_float && !c->last_expr_is_float) {
                        int conv = c->next_temp_id++;
                        ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)val_temp);
                        val_temp = conv;
                        c->last_expr_is_float = 1;
                    } else if (!abs_is_float && c->last_expr_is_float) {
                        int conv = c->next_temp_id++;
                        ir_emit_cvttsd2si(c, (uint16_t)conv, (uint16_t)val_temp);
                        val_temp = conv;
                        c->last_expr_is_float = 0;
                    }

                    if (abs_sz == 1) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(c, addr_temp, TEMP_NONE, abs_store_off);
                        ir_emit_store_byte(c, val_temp, addr_temp);
                    } else if (abs_sz == 2) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(c, addr_temp, TEMP_NONE, abs_store_off);
                        ir_emit_store_16(c, val_temp, addr_temp);
                    } else if (abs_sz == 4) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(c, addr_temp, TEMP_NONE, abs_store_off);
                        if (abs_is_float) {
                            ir_emit_vstoress(c, (uint16_t)addr_temp, (uint16_t)val_temp);
                            c->last_expr_is_float = 0;
                        } else {
                            ir_emit_store_32(c, val_temp, addr_temp);
                        }
                    } else {
                        if (abs_is_float) {
                            int addr_temp = c->next_temp_id++;
                            ir_emit_lea(c, addr_temp, TEMP_NONE, abs_store_off);
                            ir_emit_vstoresd(c, (uint16_t)addr_temp, (uint16_t)val_temp);
                            c->last_expr_is_float = 0;
                        } else {
                            ir_emit_store(c, val_temp, BE(c)->fp_reg, abs_store_off);
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
                } else if (sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&sym->type)) {
                    /* GNU vector: lanes at idx * elem_size */
                    size_t unit_sz = (sym->type.base && sym->type.base->size > 0) ? sym->type.base->size : 4;
                    elem_offset = idx * unit_sz;
                    elem_size = unit_sz;
                    elem_is_float = (sym->type.base && (sym->type.base->tag == TYPE_FLOAT || sym->type.base->tag == TYPE_DOUBLE));
                } else {
                    elem_offset = 0;
                    elem_size = sym->type.size ? sym->type.size : 4;
                    elem_is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
                }
                int32_t store_offset = -(sym->stack_offset - (int32_t)elem_offset);
                /* Struct element whose RHS lowers to an address (a struct
                 * member like `s->d`): copy the whole element, not the
                 * 8-byte pointer (c-torture 20071202-1). */
                if (elem_size >= 8 && ast_expr_is_struct_addr(c, arena, cur_ref)) {
                    int dst_t = c->next_temp_id++;
                    ir_emit_lea(c, dst_t, TEMP_NONE, store_offset);
                    zero_cc_emit_struct_copy_to_addr(c, arena, dst_t, elem_size, cur_ref, val_temp);
                    c->last_expr_is_float = 0;
                    idx++;
                    cur_ref = elem_node->next;
                    continue;
                }
                if (elem_is_float && !c->last_expr_is_float) {
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)val_temp);
                    val_temp = conv;
                    c->last_expr_is_float = 1;
                } else if (!elem_is_float && c->last_expr_is_float) {
                    int conv = c->next_temp_id++;
                    ir_emit_cvttsd2si(c, (uint16_t)conv, (uint16_t)val_temp);
                    val_temp = conv;
                    c->last_expr_is_float = 0;
                }
                /* A float element is held in an XMM temp (double form): the
                 * 4/8-byte stores must be the width-correct VSTORESS/VSTORESD
                 * (cvt + movss/movsd), not the GPR store_32/store. */
                if (elem_size == 1) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(c, addr_temp, TEMP_NONE, store_offset);
                    ir_emit_store_byte(c, val_temp, addr_temp);
                } else if (elem_size == 2) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(c, addr_temp, TEMP_NONE, store_offset);
                    ir_emit_store_16(c, val_temp, addr_temp);
                } else if (elem_size == 4) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(c, addr_temp, TEMP_NONE, store_offset);
                    if (elem_is_float) {
                        ir_emit_vstoress(c, (uint16_t)addr_temp, (uint16_t)val_temp);
                        c->last_expr_is_float = 0;
                    } else {
                        ir_emit_store_32(c, val_temp, addr_temp);
                    }
                } else {
                    if (elem_is_float) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(c, addr_temp, TEMP_NONE, store_offset);
                        ir_emit_vstoresd(c, (uint16_t)addr_temp, (uint16_t)val_temp);
                        c->last_expr_is_float = 0;
                    } else {
                        ir_emit_store(c, val_temp, BE(c)->fp_reg, store_offset);
                    }
                }
                idx++;
                cur_ref = elem_node->next;
            }
            if (sym->type.tag == TYPE_ARRAY) {
                int ptr_temp = c->next_temp_id++;
                ir_emit_lea(c, ptr_temp, BE(c)->fp_reg, -sym->stack_offset);
                c->last_expr_is_float = 0;
                return ptr_temp;
            } else if (sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&sym->type)) {
                /* GNU vector compound literal: the value is in place; consumers
                 * (binops, calls, assignments) read lanes through the address. */
                int ptr_temp = c->next_temp_id++;
                ir_emit_lea(c, ptr_temp, BE(c)->fp_reg, -sym->stack_offset);
                c->last_expr_is_float = 0;
                return ptr_temp;
            } else if (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) {
                if (sym->type.size <= 8) {
                    int val_temp = c->next_temp_id++;
                    ir_emit_load(c, val_temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return val_temp;
                } else if (sym->type.size <= 16) {
                    int val_temp = c->next_temp_id++;
                    ir_emit_load(c, val_temp, BE(c)->fp_reg, -sym->stack_offset);
                    /* Second chunk in the ABI secondary value register
                     * (VREG_2: rdx on x86-64, x1 on AArch64, a1 on RISC-V)
                     * — same convention as 9-16 byte struct returns and
                     * by-value argument marshaling. */
                    ir_emit_load(c, VREG_2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                    c->last_expr_is_float = 0;
                    return val_temp;
                } else {
                    int ptr_temp = c->next_temp_id++;
                    ir_emit_lea(c, ptr_temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return ptr_temp;
                }
            } else {
                int val_temp = c->next_temp_id++;
                if (sym->type.size == 1) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(c, addr_temp, TEMP_NONE, -sym->stack_offset);
                    ir_emit_load_byte(c, val_temp, addr_temp);
                } else if (sym->type.size == 2) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(c, addr_temp, TEMP_NONE, -sym->stack_offset);
                    ir_emit_load_16(c, val_temp, addr_temp);
                } else if (sym->type.size == 4) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(c, addr_temp, TEMP_NONE, -sym->stack_offset);
                    ir_emit_load_32(c, val_temp, addr_temp);
                } else {
                    ir_emit_load(c, val_temp, BE(c)->fp_reg, -sym->stack_offset);
                }
                c->last_expr_is_float = 0;
                return val_temp;
            }
        }

        case NODE_STMT_EXPR: {
            ZeroCCASTNode *block = zero_cc_ast_get(arena, node->left);
            if (!block) return TEMP_NONE;

            c->lowering.vla.block_depth++;

            /* Statement-expression scope (C11 6.9.1.6): the block's
             * declarations are invisible to later sibling statement
             * expressions redeclaring the same name. The parser recorded
             * the block's local-count range in aux.alt/aux.step (same
             * metadata as NODE_BLOCK); cap the lookup here because this
             * path lowers the block's statements directly instead of going
             * through lower_node(NODE_BLOCK). */
            int32_t saved_se_bound = c->local_lookup_bound;
            if (block->aux.step != 0) c->local_lookup_bound = (int32_t)(block->aux.step - 1);

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

            c->local_lookup_bound = saved_se_bound;
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
            /* Mirror-form subscript `int[(ptr)]` (C a[b] == *(a+b) ==
             * b[a]): the pointer is the index operand - the integer base
             * is an element offset scaled by the pointee size (see the
             * lvalue-addr path for the pr22061-1 explanation). */
            bool mirror_form = false;
            size_t mirror_esz = 1;
            {
                TypeInfo *idx_ti = ast_node_get_typeinfo(c, zero_cc_ast_get(arena, node->right));
                mirror_form = (idx_ti != NULL && idx_ti->kind == TYPE_KIND_POINTER);
                if (mirror_form && elem_ti &&
                    elem_ti->kind != TYPE_KIND_VOID && type_size(elem_ti) > 0)
                    mirror_esz = type_size(elem_ti);
            }

            ZeroCCSym *vla_2d_sym = NULL;
            {
                ZeroCCASTNode *ln = zero_cc_ast_get(arena, node->left);
                if (ln && ln->tag == NODE_IDENT) {
                    ZeroCCSym *ls = zero_cc_find_local(c, ln->s.str);
                    if (ls && ls->is_vla && ls->type.base && ls->type.base->tag == TYPE_ARRAY && ls->type.base->is_vla) {
                        vla_2d_sym = ls;
                    }
                }
            }

            int scaled_index;
            if (mirror_form) {
                scaled_index = TEMP_NONE; /* addr = index + scaled BASE below */
            } else if (vla_2d_sym) {
                int stride_temp = c->next_temp_id++;
                if (vla_2d_sym->is_param && vla_2d_sym->vla_slot_off > 0) {
                    /* VLA PARAMETER: entry stores the raw innermost dim in
                     * vla_slot_off; row stride = dim * leaf element size
                     * (c-torture pr22061-1: `char a[2][N]`). */
                    ir_emit_load(c, stride_temp, BE(c)->fp_reg, -vla_2d_sym->vla_slot_off);
                    const ZeroCCType *leaf = vla_2d_sym->type.base;
                    while (leaf && leaf->tag == TYPE_ARRAY) leaf = leaf->base;
                    size_t leaf_sz = (leaf && leaf->size > 0) ? leaf->size : 1;
                    if (leaf_sz != 1) {
                        int ls_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, ls_temp, (int32_t)leaf_sz);
                        int s2 = c->next_temp_id++;
                        ir_emit_mul(c, s2, stride_temp, ls_temp);
                        stride_temp = s2;
                    }
                } else {
                    ir_emit_load(c, stride_temp, BE(c)->fp_reg, -(vla_2d_sym->stack_offset - 16));
                }
                scaled_index = c->next_temp_id++;
                ir_emit_mul(c, scaled_index, index_temp, stride_temp);
            } else if (elem_size == 1) {
                scaled_index = index_temp;
            } else if (elem_size == 2 || elem_size == 4 || elem_size == 8) {
                int shift = (elem_size == 2) ? 1 : ((elem_size == 4) ? 2 : 3);
                int sh_temp = c->next_temp_id++;
                ir_emit_mov_imm(c, sh_temp, shift);
                scaled_index = c->next_temp_id++;
                ir_emit_shl(c, scaled_index, index_temp, sh_temp);
            } else {
                int sz_temp = c->next_temp_id++;
                ir_emit_mov_imm(c, sz_temp, (int32_t)elem_size);
                scaled_index = c->next_temp_id++;
                ir_emit_mul(c, scaled_index, index_temp, sz_temp);
            }

            int addr_temp = c->next_temp_id++;
            if (mirror_form) {
                int base_scaled = array_temp;
                if (mirror_esz != 1) {
                    if (mirror_esz == 2 || mirror_esz == 4 || mirror_esz == 8) {
                        int shift = (mirror_esz == 2) ? 1 : ((mirror_esz == 4) ? 2 : 3);
                        int sh_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, sh_temp, shift);
                        base_scaled = c->next_temp_id++;
                        ir_emit_shl(c, base_scaled, array_temp, sh_temp);
                    } else {
                        int sz_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, sz_temp, (int32_t)mirror_esz);
                        base_scaled = c->next_temp_id++;
                        ir_emit_mul(c, base_scaled, array_temp, sz_temp);
                    }
                }
                ir_emit_add(c, addr_temp, index_temp, base_scaled);
            } else {
                ir_emit_add(c, addr_temp, array_temp, scaled_index);  /* addr = base + scaled_index */
            }

            if (elem_ti && (type_is_array(elem_ti) || elem_ti->kind == TYPE_KIND_STRUCT || elem_ti->kind == TYPE_KIND_UNION || elem_ti->kind == TYPE_KIND_COMPLEX || type_is_vector(elem_ti))) {
                /* Array element is an array (e.g. a[i] in a[i][j]) or a struct/union/complex (e.g. p[i][j] in p[i][j].x).
                 * In C, array expressions decay to pointers to their elements, and composite elements
                 * in lvalue/member contexts need the address directly without loading. */
                c->last_expr_is_float = 0;
                return addr_temp;
            }
            if (!elem_ti) {
                ZeroCCASTNode *ln = zero_cc_ast_get(arena, node->left);
                if (ln && ln->tag == NODE_IDENT) {
                    const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                    if (zt && zt->base && zero_cc_type_is_complex(zt->base)) {
                        c->last_expr_is_float = 0;
                        return addr_temp;
                    }
                }
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
                    ir_emit_vloadss(c, (uint16_t)result_temp, (uint16_t)addr_temp);
                } else {
                    ir_emit_vloadsd(c, (uint16_t)result_temp, (uint16_t)addr_temp);
                }
                c->last_expr_is_float = 1;
            } else {
                if (elem_size == 1) {
                    ir_emit_load_byte(c, result_temp, addr_temp);
                } else if (elem_size == 2) {
                    ir_emit_load_16(c, result_temp, addr_temp);
                } else if (elem_size == 4) {
                    ir_emit_load_32(c, result_temp, addr_temp);
                } else {
                    ir_emit_deref(c, result_temp, addr_temp);
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
            if (base_is_ptr && zc_is_array_base(c, arena, innermost, base_node)) base_is_ptr = false;
            TypeInfo *member_ti = ast_node_get_typeinfo(c, node);
            /* C99 6.7.2.3: a complex member is an address-form value, just
             * like a complex local - consumers (__real__/__imag__, complex
             * arithmetic and assignment) load the two halves from the
             * address.  Loading it as a scalar here would read the real
             * part as the "value" and drop the imaginary half. */
            if (member_ti && member_ti->kind == TYPE_KIND_COMPLEX) {
                int cx_addr = zero_cc_lower_lvalue_addr(c, arena, ref);
                if (cx_addr != TEMP_NONE) return cx_addr;
            }
            bool member_is_array = (member_ti && member_ti->kind == TYPE_KIND_ARRAY);
            bool member_is_struct = (member_ti && (member_ti->kind == TYPE_KIND_STRUCT || member_ti->kind == TYPE_KIND_UNION));
            bool member_is_float = (member_ti && type_is_floating(member_ti) && !member_is_array);
            size_t member_sz = member_ti ? type_size(member_ti) : 0;
            /* Fallback: derive member float-ness from the base struct's layout.
             * Only when the typeinfo (aux.extra) is unavailable AND for a
             * single (un-chained) member access - when typeinfo exists it is
             * authoritative (e.g. a `ull` union member must not be treated
             * as float just because a double member shares its offset).
             * NOTE: do NOT read innermost->s.str here - for NODE_MEMBER the
             * name (s.str) shares a union with aux.alt (offset), and semantic
             * analysis overwrites it.  member_name (set by the semantic pass)
             * is used first: union members ALL share offsets, so offset-only
             * matching would pick whichever member is recorded first (a
             * miscompile for `u.ll` on `union { double d; ull ll; }`). */
            if (!member_ti && !member_is_array && innermost &&
                base_node && base_node->tag == NODE_IDENT) {
                ZeroCCType *st = NULL;
                ZeroCCSym *bs = zero_cc_find_local(c, base_node->s.str);
                if (bs && !bs->is_extern) {
                    st = (base_is_ptr && bs->type.tag == TYPE_PTR) ? bs->type.base : &bs->type;
                } else {
                    ZeroCCGlobalVar *bg = find_global_var(c, base_node->s.str);
                    if (bg) {
                        st = (bg->type.tag == TYPE_PTR) ? bg->type.base : &bg->type;
                    }
                }
                if (st && (st->tag == TYPE_STRUCT || st->tag == TYPE_UNION) && st->member_count <= 64) {
                    size_t in_off = (size_t)innermost->aux.alt;
                    const char *mname = innermost->member_name;
                    size_t match = st->member_count; /* none */
                    for (size_t m = 0; m < st->member_count; m++) {
                        if (mname && st->members[m].name &&
                            strcmp(mname, st->members[m].name) == 0) {
                            match = m;
                            break;
                        }
                    }
                    if (match == st->member_count) {
                        for (size_t m = 0; m < st->member_count; m++) {
                            if (st->members[m].offset == in_off) {
                                match = m;
                                break;
                            }
                        }
                    }
                    if (match < st->member_count &&
                        (st->members[match].tag == TYPE_FLOAT || st->members[match].tag == TYPE_DOUBLE)) {
                        member_is_float = true;
                        member_sz = st->members[match].size ? st->members[match].size :
                            (st->members[match].tag == TYPE_FLOAT ? 4 : 8);
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
                        ir_emit_mov_reg(c, val_temp, multi_addr);
                        c->last_expr_is_float = 0;
                        return val_temp;
                    }
                    if (member_is_float) {
                        if (member_sz == 4) ir_emit_vloadss(c, (uint16_t)val_temp, (uint16_t)multi_addr);
                        else ir_emit_vloadsd(c, (uint16_t)val_temp, (uint16_t)multi_addr);
                        c->last_expr_is_float = 1;
                        return val_temp;
                    }
                    /* 64-bit load; the shared width-narrowing tail below
                     * fixes up sub-8-byte members. */
                    ir_emit_deref(c, val_temp, multi_addr);
                }
            }
            if (multi_addr == TEMP_NONE && base_node && base_node->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
                ZeroCCGlobalVar *ggv = (sym && !sym->is_extern) ? NULL : find_global_var(c, base_node->s.str);
                if (ggv) {
                        /* Global struct/union base: load the variable address,
                         * add the member offset, then load/deref. */
                        int gaddr = c->next_temp_id++;
                        ir_emit_global_addr(c, gaddr, ggv);
                        if (base_is_ptr) {
                            int ptr_temp = c->next_temp_id++;
                            ir_emit_deref(c, ptr_temp, gaddr);
                            gaddr = ptr_temp;
                        }
                        int gaddr2 = gaddr;
                        if (member_offset != 0) {
                            int goff = c->next_temp_id++;
                            ir_emit_mov_imm(c, goff, member_offset);
                            gaddr2 = c->next_temp_id++;
                            ir_emit_add(c, gaddr2, gaddr, goff);
                        }
                        if (member_is_float) {
                            if (member_sz == 4) ir_emit_vloadss(c, (uint16_t)val_temp, (uint16_t)gaddr2);
                            else ir_emit_vloadsd(c, (uint16_t)val_temp, (uint16_t)gaddr2);
                            c->last_expr_is_float = 1;
                            return val_temp;
                        }
                        if (member_is_array || member_is_struct) {
                            ir_emit_mov_reg(c, val_temp, gaddr2);
                            c->last_expr_is_float = 0;
                            return val_temp;
                        }
                        /* Plain scalar member: 64-bit deref, then fall through
                         * to the width-narrowing below (the struct layout may
                         * pack smaller members - e.g. `int a; float f;` - so
                         * the upper bytes of the load are not zero and an
                         * early return would leak them into the value). */
                        ir_emit_deref(c, val_temp, gaddr2);
                } else if (sym && base_is_ptr) {
                    /* Unknown identifier falls through to the local-style
                     * load below with base_stack_offset 0. */
                    /* p->member: base is a pointer. Load the pointer value,
                     * add the member offset, and dereference. (The trailing
                     * size-narrowing below handles sub-8-byte members.) */
                    int ptr_temp = c->next_temp_id++;
                    ir_emit_load(c, ptr_temp, BE(c)->fp_reg, -sym->stack_offset);
                    int addr_temp = ptr_temp;
                    if (member_offset != 0) {
                        int off_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, off_temp, member_offset);
                        addr_temp = c->next_temp_id++;
                        ir_emit_add(c, addr_temp, ptr_temp, off_temp);
                    }
                    if (member_is_array || member_is_struct) {
                        ir_emit_mov_reg(c, val_temp, addr_temp);
                        c->last_expr_is_float = 0;
                        return val_temp;
                    } else if (member_is_float) {
                        if (member_sz == 4) ir_emit_vloadss(c, (uint16_t)val_temp, (uint16_t)addr_temp);
                        else ir_emit_vloadsd(c, (uint16_t)val_temp, (uint16_t)addr_temp);
                        c->last_expr_is_float = 1;
                        return val_temp;
                    } else {
                        ir_emit_deref(c, val_temp, addr_temp);
                    }
                } else if (sym) {
                    /* Local struct/union base accessed via `.`: frame-
                     * relative member load (base_stack_offset from the
                     * local's slot). */
                    base_stack_offset = -sym->stack_offset;
                    int32_t total_offset = base_stack_offset + member_offset;
                    if (member_is_float) {
                        if (member_sz == 4) {
                            /* 4-byte float member: width-correct load - the
                             * 8-byte FLOAD reads past the member into the
                             * adjacent bytes of the struct. */
                            int a4 = c->next_temp_id++;
                            ir_emit_lea(c, a4, TEMP_NONE, (int16_t)total_offset);
                            ir_emit_vloadss(c, (uint16_t)val_temp, (uint16_t)a4);
                        } else {
                            ir_emit_fload(c, (uint16_t)val_temp, TEMP_NONE, (int16_t)total_offset);
                        }
                        c->last_expr_is_float = 1;
                        return val_temp;
                    }
                    if (member_is_array || member_is_struct) {
                        ir_emit_lea(c, val_temp, BE(c)->fp_reg, total_offset);
                        c->last_expr_is_float = 0;
                        return val_temp;
                    } else {
                        ir_emit_load(c, val_temp, BE(c)->fp_reg, total_offset);
                    }
                } else {
                    /* Base is a plain identifier that is neither a local
                     * nor a global - most importantly an INLINED function
                     * parameter (resolved by the inliner's parameter
                     * substitution when lowered as an expression; the
                     * param temp holds the argument's pointer value).
                     * Lower the base to its address/pointer value, add the
                     * member offset, and load through it. (The old code
                     * here assumed a frame-local struct and emitted a
                     * frame-relative load at the member offset - reading
                     * an unrelated slot; c-torture execute/20071120-1
                     * segfaulted at -O2 when `vec_->num` in an inlined
                     * function read [fp+0] instead of [vec_+0].) */
                    int base_addr = lower_expr(c, arena, base_ref);
                    int addr_temp = base_addr;
                    if (member_offset != 0) {
                        int off_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, off_temp, member_offset);
                        addr_temp = c->next_temp_id++;
                        ir_emit_add(c, addr_temp, base_addr, off_temp);
                    }
                    if (member_is_array || member_is_struct) {
                        ir_emit_mov_reg(c, val_temp, addr_temp);
                        c->last_expr_is_float = 0;
                        return val_temp;
                    } else if (member_is_float) {
                        if (member_sz == 4) ir_emit_vloadss(c, (uint16_t)val_temp, (uint16_t)addr_temp);
                        else ir_emit_vloadsd(c, (uint16_t)val_temp, (uint16_t)addr_temp);
                        c->last_expr_is_float = 1;
                        return val_temp;
                    } else {
                        ir_emit_deref(c, val_temp, addr_temp);
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
                        ir_emit_lea(c, a4, TEMP_NONE, (int16_t)total_offset);
                        ir_emit_vloadss(c, (uint16_t)val_temp, (uint16_t)a4);
                    } else {
                        ir_emit_fload(c, (uint16_t)val_temp, TEMP_NONE, (int16_t)total_offset);
                    }
                    c->last_expr_is_float = 1;
                    return val_temp;
                }
                if (member_is_array || member_is_struct) {
                    ir_emit_lea(c, val_temp, BE(c)->fp_reg, total_offset);
                    c->last_expr_is_float = 0;
                    return val_temp;
                } else {
                    ir_emit_load(c, val_temp, BE(c)->fp_reg, total_offset);
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
                        ir_emit_store(c, base_ptr, BE(c)->fp_reg, -spill_slot);
                        ir_emit_store(c, VREG_2, BE(c)->fp_reg, -(spill_slot - 8));
                        int struct_addr = c->next_temp_id++;
                        ir_emit_lea(c, struct_addr, BE(c)->fp_reg, -spill_slot);
                        base_ptr = struct_addr;
                    }
                }
                int addr_temp = base_ptr;
                if (member_offset != 0) {
                    /* Spill base_ptr to stack to avoid clobbering in addition */
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    int32_t bptr_off = c->current_stack_frame;
                    ir_emit_store(c, base_ptr, BE(c)->fp_reg, -bptr_off);
                    int b_temp = c->next_temp_id++;
                    ir_emit_load(c, b_temp, BE(c)->fp_reg, -bptr_off);
                    int off_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, off_temp, member_offset);
                    addr_temp = c->next_temp_id++;
                    ir_emit_add(c, addr_temp, b_temp, off_temp);
                }
                if (member_is_array || member_is_struct) {
                    ir_emit_mov_reg(c, val_temp, addr_temp);
                    c->last_expr_is_float = 0;
                    return val_temp;
                } else if (member_is_float) {
                    if (member_sz == 4) ir_emit_vloadss(c, (uint16_t)val_temp, (uint16_t)addr_temp);
                    else ir_emit_vloadsd(c, (uint16_t)val_temp, (uint16_t)addr_temp);
                    c->last_expr_is_float = 1;
                    return val_temp;
                } else {
                    ir_emit_deref(c, val_temp, addr_temp);
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
                    ir_emit_mov_imm(c, shift_temp, (int32_t)bit_offset);
                    int shifted_temp = c->next_temp_id++;
                    ir_emit_shr(c, shifted_temp, val_temp, shift_temp);
                    val_temp = shifted_temp;
                }

                uint64_t mask = (bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1);
                int mask_temp = c->next_temp_id++;
                ir_emit_mov_imm(c, mask_temp, (int64_t)mask);
                int masked_temp = c->next_temp_id++;
                ir_emit_and(c, masked_temp, val_temp, mask_temp);
                val_temp = masked_temp;

                if (is_signed && bit_width > 0 && bit_width < 64) {
                    int64_t sign_bit = (int64_t)(1ULL << (bit_width - 1));
                    int sign_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, sign_temp, sign_bit);
                    int xor_temp = c->next_temp_id++;
                    ir_emit_xor(c, xor_temp, val_temp, sign_temp);
                    int ext_temp = c->next_temp_id++;
                    ir_emit_sub(c, ext_temp, xor_temp, sign_temp);
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
                    if (is_u) ir_emit_zext(c, val_temp, (int)sz);
                    else ir_emit_sext(c, val_temp, (int)sz);
                }
            }

            c->last_expr_is_float = 0;
            return val_temp;
        }

        case NODE_CALL:
            return lower_expr_call(c, arena, node);

        default:
            return TEMP_NONE;
    }
}

