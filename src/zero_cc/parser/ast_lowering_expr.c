/* AST Lowering - Expression Lowering (AST to IR) */

#include "ast_lowering_internal.h"
#include "../isa/isa_backend.h"

int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return 0xFF;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return 0xFF;
    
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
            /* Float literals: store in rodata and load via RIP-relative addressing */
            double val = node->float_val;
            size_t rodata_offset = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, &val, sizeof(double));
            int temp = c->next_temp_id++;
            ir_emit_fmov_imm((uint8_t)temp, (int64_t)rodata_offset);
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
            
            ZeroCCSym *sym = zero_cc_find_local(c, name);
            if (sym) {
                if (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE) {
                    /* Float load via IR for proper register allocation */
                    int temp = c->next_temp_id++;
                    ir_emit_fload((uint8_t)temp, 0xFF, -sym->stack_offset);
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
                    int temp = c->next_temp_id++;
                    ir_emit_mov_global(temp, gv->name);
                    c->last_expr_is_float = 0;
                    return temp;
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
                    c->last_expr_is_float = 0;
                    return temp;
                }
                return 0xFF;
            }
        }
        
        case NODE_BINOP: {
            /* Evaluate left operand first */
            int left_temp = lower_expr(c, arena, node->left);
            bool left_is_float = c->last_expr_is_float;

            /* Evaluate right operand */
            int right_temp = lower_expr(c, arena, node->right);
            bool right_is_float = c->last_expr_is_float;

            /* Float arithmetic and comparisons - use IR for proper register allocation */
            if (left_is_float && right_is_float) {
                /* At this point: left_temp and right_temp are float temps in XMM registers */
                int result_temp = c->next_temp_id++;
                switch (node->aux.op) {
                    case TK_PLUS:
                        ir_emit_fadd((uint8_t)result_temp, (uint8_t)left_temp, (uint8_t)right_temp);
                        c->last_expr_is_float = 1;
                        return result_temp;
                    case TK_MINUS:
                        ir_emit_fsub((uint8_t)result_temp, (uint8_t)left_temp, (uint8_t)right_temp);
                        c->last_expr_is_float = 1;
                        return result_temp;
                    case TK_STAR:
                        ir_emit_fmul((uint8_t)result_temp, (uint8_t)left_temp, (uint8_t)right_temp);
                        c->last_expr_is_float = 1;
                        return result_temp;
                    case TK_SLASH:
                        ir_emit_fdiv((uint8_t)result_temp, (uint8_t)left_temp, (uint8_t)right_temp);
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
                case TK_SHR:
                    ir_emit_shr(result_temp, left_temp, right_temp);
                    break;
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
                            return 0xFF;
                        }
                        if (sym->is_atomic || sym->type.is_atomic) {
                            int addr_temp = c->next_temp_id++;
                            ir_emit_lea(addr_temp, 0xFF, -sym->stack_offset);
                            int delta_temp = c->next_temp_id++;
                            int width = (sym->type.size > 0) ? (int)sym->type.size : 4;
                            if (node->aux.op == TK_INC) {
                                ir_emit_mov_imm(delta_temp, 1);
                            } else {
                                ir_emit_mov_imm(delta_temp, -1);
                            }
                            int old_temp = c->next_temp_id++;
                            ir_emit_atomic_xadd(old_temp, addr_temp, delta_temp, width);
                            c->last_expr_is_float = 0;
                            if (node->flags == 1) {
                                /* Postfix x++ / x--: return old value */
                                return old_temp;
                            } else {
                                /* Prefix ++x / --x: return new value (old + 1 / -1) */
                                int new_temp = c->next_temp_id++;
                                int step_temp = c->next_temp_id++;
                                ir_emit_mov_imm(step_temp, (node->aux.op == TK_INC) ? 1 : -1);
                                ir_emit_add(new_temp, old_temp, step_temp);
                                return new_temp;
                            }
                        }

                        int val_temp = c->next_temp_id++;
                        int one_temp = c->next_temp_id++;
                        int res_temp = c->next_temp_id++;

                        /* Load current variable value from stack */
                        ir_emit_load(val_temp, BE(c)->fp_reg, -sym->stack_offset);
                        /* Constant 1 */
                        ir_emit_mov_imm(one_temp, 1);
                        /* res = val +/- 1 */
                        if (node->aux.op == TK_INC) {
                            ir_emit_add(res_temp, val_temp, one_temp);
                        } else {
                            ir_emit_sub(res_temp, val_temp, one_temp);
                        }
                        /* Store new value back to stack */
                        ir_emit_store(res_temp, BE(c)->fp_reg, -sym->stack_offset);

                        c->last_expr_is_float = 0;
                        return res_temp;
                    }
                }
                return 0xFF;
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
                            ir_emit_lea(addr_temp, 0xFF, -sym->stack_offset);
                        }
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
                        ir_emit_lea(addr_temp, 0xFF, -sym->stack_offset);
                        c->last_expr_is_float = 0;
                        return addr_temp;
                    }
                }
            }

            int operand_temp = lower_expr(c, arena, node->left);
            int result_temp = c->next_temp_id++;
            
            switch (node->aux.op) {
                case TK_STAR:  /* Dereference */
                    ir_emit_deref(result_temp, operand_temp);
                    break;
                case TK_MINUS:  /* Negation */
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
            c->last_expr_is_float = 0;
            return result_temp;
        }
        
        case NODE_CAST: {
            int result = lower_expr(c, arena, node->left);
            /* Float to int conversion: emit via IR for correct sequencing */
            if (c->last_expr_is_float) {
                int int_res_temp = c->next_temp_id++;
                ir_emit_cvttsd2si((uint8_t)int_res_temp, (uint8_t)result);
                c->last_expr_is_float = 0;
                return int_res_temp;
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
                    ir_emit_lea(addr_temp, 0xFF, va_start_offset);
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
            
            if (left_node && left_node->tag == NODE_MEMBER) {
                /* Struct member assignment: traverse chain of member accesses */
                int32_t member_offset = 0;
                ZeroCCASTNode *curr = left_node;
                ASTRef base_ref = node->left;
                while (curr && curr->tag == NODE_MEMBER) {
                    member_offset += (int32_t)curr->aux.alt;
                    base_ref = curr->left;
                    curr = zero_cc_ast_get(arena, curr->left);
                }
                ZeroCCASTNode *base_node = curr;
                int32_t base_stack_offset = 0;
                
                // Find the base variable's stack offset if it's a local identifier
                if (base_node && base_node->tag == NODE_IDENT) {
                    ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
                    if (!sym) {
                        diag_report_error_at_token(c, "undefined local variable '%s' in struct assignment", base_node->s.str);
                        return 0xFF;
                    }
                    if (sym->is_const || sym->type.is_const) {
                        diag_report_error_at_token(c, "cannot assign to member of const-qualified struct '%s'", base_node->s.str);
                        return 0xFF;
                    }
                    if (c->sem_ctx && left_node->aux.extra > 0) {
                        TypeInfo *ti = (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(left_node->aux.extra - 1));
                        if (ti && (ti->modifiers & TYPE_MOD_CONST)) {
                            diag_report_error_at_token(c, "cannot assign to const-qualified member");
                            return 0xFF;
                        }
                    }
                    for (size_t m_i = 0; m_i < sym->type.member_count; m_i++) {
                        if (sym->type.members[m_i].offset == (size_t)member_offset) {
                            if (sym->type.members[m_i].is_const) {
                                diag_report_error_at_token(c, "cannot assign to const-qualified member '%s'", sym->type.members[m_i].name);
                                return 0xFF;
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
                    diag_report_error_at_token(c, "unsupported base expression in struct member assignment");
                    return 0xFF;
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

                if (c->last_expr_is_float) {
                    /* Float value: store via IR */
                    ir_emit_fstore((uint8_t)right_temp, BE(c)->fp_reg, total_offset);
                    c->last_expr_is_float = 1;
                    return right_temp;
                } else {
                    /* Integer/pointer value in GPR - use IR store */
                    ir_emit_store(right_temp, BE(c)->fp_reg, total_offset);
                    c->last_expr_is_float = 0;
                    return right_temp;
                }
            }

            if (left_node && left_node->tag == NODE_INDEX) {
                /* Array element assignment: arr[idx] = expr or arr[idx] op= expr */
                int array_temp = lower_expr(c, arena, left_node->left);
                int index_temp = lower_expr(c, arena, left_node->right);

                size_t elem_size = 4;
                TypeInfo *elem_ti = NULL;
                if (c->sem_ctx && left_node->aux.extra > 0) {
                    elem_ti = (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(left_node->aux.extra - 1));
                }
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
                        ir_emit_vstoress(addr_temp, (uint8_t)right_temp);
                    } else {
                        ir_emit_vstoresd(addr_temp, (uint8_t)right_temp);
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
                        return 0xFF;
                    }
                }
                int ptr_temp = lower_expr(c, arena, left_node->left);
                ir_emit_store_64(right_temp, ptr_temp);
                c->last_expr_is_float = 0;
                return right_temp;
            }

            const char *var = left_node ? left_node->s.str : "";
            ZeroCCSym *sym = zero_cc_find_local(c, var);
            if (sym) {
                if (sym->is_const || sym->type.is_const) {
                    diag_report_error_at_token(c, "cannot assign to const-qualified variable '%s'", var);
                    return 0xFF;
                }
                bool target_is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
                if (target_is_float) {
                    int float_temp = right_temp;
                    if (!c->last_expr_is_float) {
                        /* Convert int to float via IR */
                        float_temp = c->next_temp_id++;
                        ir_emit_cvtsi2sd((uint8_t)float_temp, (uint8_t)right_temp);
                    }
                    /* For compound assignment, load current value, operate, then store */
                    if (is_compound) {
                        int lhs_temp = c->next_temp_id++;
                        ir_emit_fload((uint8_t)lhs_temp, 0xFF, -sym->stack_offset);
                        int op_res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd((uint8_t)op_res, (uint8_t)lhs_temp, (uint8_t)float_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub((uint8_t)op_res, (uint8_t)lhs_temp, (uint8_t)float_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul((uint8_t)op_res, (uint8_t)lhs_temp, (uint8_t)float_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_fdiv((uint8_t)op_res, (uint8_t)lhs_temp, (uint8_t)float_temp); break;
                            default: op_res = float_temp; break;
                        }
                        ir_emit_fstore((uint8_t)op_res, 0xFF, -sym->stack_offset);
                        c->last_expr_is_float = 1;
                        return op_res;
                    } else {
                        /* Simple assignment: just store */
                        ir_emit_fstore((uint8_t)float_temp, 0xFF, -sym->stack_offset);
                        c->last_expr_is_float = 1;
                        return float_temp;
                    }
                } else {
                    if (c->last_expr_is_float) {
                        /* Convert float to int via IR */
                        int int_temp = c->next_temp_id++;
                        ir_emit_cvttsd2si((uint8_t)int_temp, (uint8_t)right_temp);
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
                        ir_emit_store((uint8_t)int_temp, BE(c)->fp_reg, -sym->stack_offset);
                        c->last_expr_is_float = 0;
                        return int_temp;
                    } else {
                        if (sym && (sym->is_atomic || sym->type.is_atomic)) {
                            int addr_temp = c->next_temp_id++;
                            ir_emit_lea(addr_temp, 0xFF, -sym->stack_offset);
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
                            /* Load current value and perform operation */
                            int left_temp = c->next_temp_id++;
                            ir_emit_load(left_temp, BE(c)->fp_reg, -sym->stack_offset);
                            switch (node->aux.op) {
                                case TK_ADD_ASSIGN: ir_emit_add(right_temp, left_temp, right_temp); break;
                                case TK_SUB_ASSIGN: ir_emit_sub(right_temp, left_temp, right_temp); break;
                                case TK_MUL_ASSIGN: ir_emit_mul(right_temp, left_temp, right_temp); break;
                                case TK_DIV_ASSIGN: ir_emit_div(right_temp, left_temp, right_temp); break;
                                case TK_MOD_ASSIGN: ir_emit_mod(right_temp, left_temp, right_temp); break;
                                case TK_AND_ASSIGN: ir_emit_and(right_temp, left_temp, right_temp); break;
                                case TK_OR_ASSIGN:  ir_emit_or(right_temp, left_temp, right_temp); break;
                                case TK_XOR_ASSIGN: ir_emit_xor(right_temp, left_temp, right_temp); break;
                                case TK_SHL_ASSIGN: ir_emit_shl(right_temp, left_temp, right_temp); break;
                                case TK_SHR_ASSIGN: ir_emit_shr(right_temp, left_temp, right_temp); break;
                            }
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
                                    ir_emit_lea(dst_addr, 0xFF, dst_off);
                                    ir_emit_store_32(chunk_temp, dst_addr);
                                } else if (rem >= 2) {
                                    ir_emit_load_16(chunk_temp, addr_temp);
                                    int dst_addr = c->next_temp_id++;
                                    ir_emit_lea(dst_addr, 0xFF, dst_off);
                                    ir_emit_store_16(chunk_temp, dst_addr);
                                } else {
                                    ir_emit_load_byte(chunk_temp, addr_temp);
                                    int dst_addr = c->next_temp_id++;
                                    ir_emit_lea(dst_addr, 0xFF, dst_off);
                                    ir_emit_store_byte(chunk_temp, dst_addr);
                                }
                            }
                        } else {
                            ir_emit_store(right_temp, BE(c)->fp_reg, -sym->stack_offset);
                            if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16 && !is_compound) {
                                /* System V AMD64 ABI: second eightbyte returned in rdx (physical register 2) */
                                ir_emit_store(2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                            }
                        }
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                }
            }
            c->last_expr_is_float = 0;
            return right_temp;
        }
        
        case NODE_TERNARY: {
            /* Ternary (cond ? then : else) using standard IR */
            int cond_temp = lower_expr(c, arena, node->left);
            int res_temp = c->next_temp_id++;
            uint16_t else_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;

            ir_emit_je((uint8_t)cond_temp, else_label);

            int then_temp = lower_expr(c, arena, node->right);
            ir_emit_mov_reg(res_temp, then_temp);
            ir_emit_jmp(end_label);

            ir_emit_label(else_label);
            int else_temp = lower_expr(c, arena, node->aux.alt);
            ir_emit_mov_reg(res_temp, else_temp);

            ir_emit_label(end_label);
            c->last_expr_is_float = 0;
            return res_temp;
        }
        
        case NODE_COMPOUND_LIT: {
            ZeroCCSym *sym = (node->s.str && node->s.str[0]) ? zero_cc_find_local(c, node->s.str) : NULL;
            if (!sym) {
                if (node->left != AST_NULL) return lower_expr(c, arena, node->left);
                return 0xFF;
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
                        ir_emit_lea(addr_temp, 0xFF, store_offset);
                        ir_emit_store_32(zero_temp, addr_temp);
                    } else if (chunk == 2) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, 0xFF, store_offset);
                        ir_emit_store_16(zero_temp, addr_temp);
                    } else {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, 0xFF, store_offset);
                        ir_emit_store_byte(zero_temp, addr_temp);
                    }
                    offset += chunk;
                }
            }
            ASTRef cur_ref = node->left;
            size_t idx = 0;
            while (cur_ref != AST_NULL) {
                ZeroCCASTNode *elem_node = zero_cc_ast_get(arena, cur_ref);
                if (!elem_node) break;
                int val_temp = lower_expr(c, arena, cur_ref);
                size_t elem_offset = 0;
                size_t elem_size = 4;
                if (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) {
                    size_t m_idx = idx;
                    if ((elem_node->flags & NODE_FLAG_DESIGNATED) != 0) m_idx = (size_t)elem_node->aux.alt;
                    if (m_idx < sym->type.member_count) {
                        /* C99: flexible array members cannot be initialized */
                        if (sym->type.members[m_idx].is_flexible) {
                            diag_report_error_at_token(c,
                                "initialization of flexible array member '%s'",
                                sym->type.members[m_idx].name);
                            return 0xFF;
                        }
                        elem_offset = sym->type.members[m_idx].offset;
                        elem_size = sym->type.members[m_idx].size;
                    }
                } else if (sym->type.tag == TYPE_ARRAY) {
                    size_t a_idx = idx;
                    if ((elem_node->flags & NODE_FLAG_DESIGNATED) != 0) {
                        a_idx = (size_t)elem_node->aux.alt;
                        idx = a_idx;
                    }
                    size_t unit_sz = (sym->type.base && sym->type.base->size > 0) ? sym->type.base->size : 4;
                    elem_offset = a_idx * unit_sz;
                    elem_size = unit_sz;
                } else {
                    elem_offset = 0;
                    elem_size = sym->type.size ? sym->type.size : 4;
                }
                int32_t store_offset = -(sym->stack_offset - (int32_t)elem_offset);
                if (elem_size == 1) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, 0xFF, store_offset);
                    ir_emit_store_byte(val_temp, addr_temp);
                } else if (elem_size == 2) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, 0xFF, store_offset);
                    ir_emit_store_16(val_temp, addr_temp);
                } else if (elem_size == 4) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, 0xFF, store_offset);
                    ir_emit_store_32(val_temp, addr_temp);
                } else {
                    ir_emit_store(val_temp, BE(c)->fp_reg, store_offset);
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
                    ir_emit_lea(addr_temp, 0xFF, -sym->stack_offset);
                    ir_emit_load_byte(val_temp, addr_temp);
                } else if (sym->type.size == 2) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, 0xFF, -sym->stack_offset);
                    ir_emit_load_16(val_temp, addr_temp);
                } else if (sym->type.size == 4) {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, 0xFF, -sym->stack_offset);
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
            TypeInfo *elem_ti = NULL;
            if (c->sem_ctx && node->aux.extra > 0) {
                elem_ti = (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(node->aux.extra - 1));
            }
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
                    ir_emit_vloadss((uint8_t)result_temp, (uint8_t)addr_temp);
                } else {
                    ir_emit_vloadsd((uint8_t)result_temp, (uint8_t)addr_temp);
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
                c->last_expr_is_float = 0;
            }
            return result_temp;
        }
        
        case NODE_MEMBER: {
            /* Load member value from memory (Rvalue context) */
            int32_t member_offset = 0;
            ZeroCCASTNode *curr = node;
            ASTRef base_ref = ref;
            while (curr && curr->tag == NODE_MEMBER) {
                member_offset += (int32_t)curr->aux.alt;
                base_ref = curr->left;
                curr = zero_cc_ast_get(arena, curr->left);
            }
            ZeroCCASTNode *base_node = curr;
            int32_t base_stack_offset = 0;
            int val_temp = c->next_temp_id++;
            if (base_node && base_node->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
                if (sym) {
                    base_stack_offset = -sym->stack_offset;
                }
                int32_t total_offset = base_stack_offset + member_offset;
                ir_emit_load(val_temp, BE(c)->fp_reg, total_offset);
            } else if (base_node && base_node->tag == NODE_COMPOUND_LIT) {
                (void)lower_expr(c, arena, base_ref);
                ZeroCCSym *sym = (base_node->s.str && base_node->s.str[0]) ? zero_cc_find_local(c, base_node->s.str) : NULL;
                if (sym) {
                    base_stack_offset = -sym->stack_offset;
                }
                int32_t total_offset = base_stack_offset + member_offset;
                ir_emit_load(val_temp, BE(c)->fp_reg, total_offset);
            } else {
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
                ir_emit_deref(val_temp, addr_temp);
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
                TypeInfo *ti = NULL;
                if (c->sem_ctx && node->aux.extra > 0) {
                    ti = (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(node->aux.extra - 1));
                }
                size_t sz = ti ? type_size(ti) : 4;
                bool is_u = ti && (ti->modifiers & TYPE_MOD_UNSIGNED);
                if (sz == 1) {
                    int mask_temp = c->next_temp_id++;
                    ir_emit_mov_imm(mask_temp, 0xFF);
                    int masked = c->next_temp_id++;
                    ir_emit_and(masked, val_temp, mask_temp);
                    val_temp = masked;
                    if (!is_u) {
                        int sign_temp = c->next_temp_id++;
                        ir_emit_mov_imm(sign_temp, 0x80);
                        int xor_temp = c->next_temp_id++;
                        ir_emit_xor(xor_temp, val_temp, sign_temp);
                        int ext_temp = c->next_temp_id++;
                        ir_emit_sub(ext_temp, xor_temp, sign_temp);
                        val_temp = ext_temp;
                    }
                } else if (sz == 2) {
                    int mask_temp = c->next_temp_id++;
                    ir_emit_mov_imm(mask_temp, 0xFFFF);
                    int masked = c->next_temp_id++;
                    ir_emit_and(masked, val_temp, mask_temp);
                    val_temp = masked;
                    if (!is_u) {
                        int sign_temp = c->next_temp_id++;
                        ir_emit_mov_imm(sign_temp, 0x8000);
                        int xor_temp = c->next_temp_id++;
                        ir_emit_xor(xor_temp, val_temp, sign_temp);
                        int ext_temp = c->next_temp_id++;
                        ir_emit_sub(ext_temp, xor_temp, sign_temp);
                        val_temp = ext_temp;
                    }
                } else if (sz == 4) {
                    int mask_temp = c->next_temp_id++;
                    ir_emit_mov_imm(mask_temp, 0xFFFFFFFFLL);
                    int masked = c->next_temp_id++;
                    ir_emit_and(masked, val_temp, mask_temp);
                    val_temp = masked;
                    if (!is_u) {
                        int sign_temp = c->next_temp_id++;
                        ir_emit_mov_imm(sign_temp, 0x80000000LL);
                        int xor_temp = c->next_temp_id++;
                        ir_emit_xor(xor_temp, val_temp, sign_temp);
                        int ext_temp = c->next_temp_id++;
                        ir_emit_sub(ext_temp, xor_temp, sign_temp);
                        val_temp = ext_temp;
                    }
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
                
                ZeroCCFunction *callee_fn = zero_cc_find_function(c, name);
                ZeroCCSym *callee_sym = (!callee_fn) ? zero_cc_find_local(c, name) : NULL;
                bool is_indirect_call = (callee_fn == NULL && callee_sym != NULL);

                /* Determine return type of the call */
                ZeroCCType call_ret_type;
                memset(&call_ret_type, 0, sizeof(call_ret_type));
                if (callee_fn) {
                    call_ret_type = callee_fn->return_type;
                } else if (callee_sym && callee_sym->type.tag == TYPE_PTR && callee_sym->type.base) {
                    call_ret_type = *callee_sym->type.base;
                } else if (c->sem_ctx && node->aux.extra > 0) {
                    TypeInfo *ret_ti = (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(node->aux.extra - 1));
                    if (ret_ti) {
                        if (ret_ti->kind == TYPE_KIND_STRUCT) call_ret_type.tag = TYPE_STRUCT;
                        else if (ret_ti->kind == TYPE_KIND_UNION) call_ret_type.tag = TYPE_UNION;
                        else if (ret_ti->kind == TYPE_KIND_FLOAT) call_ret_type.tag = TYPE_FLOAT;
                        else if (ret_ti->kind == TYPE_KIND_DOUBLE) call_ret_type.tag = TYPE_DOUBLE;
                        call_ret_type.size = type_size(ret_ti);
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
                    int temp = lower_expr(c, arena, arg);
                    if (c->last_expr_is_float) {
                        /* Float argument: spill it to a stack slot via IR */
                        if (fp_arg_idx < 8) {
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            fp_spill_offsets[fp_arg_idx] = c->current_stack_frame;
                            ir_emit_fstore((uint8_t)temp, 0xFF, -fp_spill_offsets[fp_arg_idx]);
                            fp_arg_idx++;
                        }
                    } else {
                        /* Integer argument: spill to a stack slot so subsequent argument
                         * evaluations containing function calls do not clobber caller-saved registers. */
                        if (int_arg_idx < IR_CALL_MAX_ARGS) {
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            int_spill_offsets[int_arg_idx] = c->current_stack_frame;
                            ir_emit_store(temp, BE(c)->fp_reg, -int_spill_offsets[int_arg_idx]);
                            int_arg_idx++;
                        }
                    }
                    ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                    arg = arg_node ? arg_node->next : AST_NULL;
                }
                
                /* Reload arguments from stack slots immediately before call */
                int int_reload_idx = 0;
                int fp_reload_idx = 0;
                
                bool callee_uses_sret = (call_ret_type.tag == TYPE_STRUCT || call_ret_type.tag == TYPE_UNION) && (call_ret_type.size > 16);
                int sret_buf_temp = 0;

                /* System V AMD64 ABI: >16 byte struct return requires caller to allocate buffer
                 * and pass its address in RDI (as first integer argument). */
                if (callee_uses_sret) {
                    c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                    int32_t sret_alloc_sz = (int32_t)((call_ret_type.size + 7) & ~((size_t)7));
                    c->current_stack_frame += sret_alloc_sz;
                    int32_t buf_stack_off = c->current_stack_frame;

                    sret_buf_temp = c->next_temp_id++;
                    ir_emit_lea(sret_buf_temp, BE(c)->fp_reg, -buf_stack_off);

                    all_arg_temps[all_arg_count] = sret_buf_temp;
                    all_arg_is_fp[all_arg_count] = 0;
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
                    ir_emit_fload(reload_temp, 0xFF, -fp_spill_offsets[f]);
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
                
                /* Total integer arguments including sret if present */
                int total_int_args = int_arg_idx + (callee_uses_sret ? 1 : 0);

                /* Emit IR call (NULL name for indirect calls) */
                ir_emit_call(is_indirect_call ? NULL : name);
                
                /* Pack argument info into the instruction */
                if (ir_get_count() > 0) {
                    Instr *last_instr = (Instr*)&ir_get_buffer()[ir_get_count() - 1];
                    last_instr->temp_dest = (uint8_t)total_int_args;
                    if (total_int_args > 0) last_instr->temp_src1 = (uint8_t)all_arg_temps[0];
                    if (total_int_args > 1) last_instr->temp_src2 = (uint8_t)all_arg_temps[1];
                    if (total_int_args > 2) {
                        last_instr->imm = 0;
                        if (total_int_args > 2) last_instr->imm |= ((int16_t)(uint8_t)all_arg_temps[2]) << 0;
                        if (total_int_args > 3) last_instr->imm |= ((int16_t)(uint8_t)all_arg_temps[3]) << 8;
                    }
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
                    return VREG_F0;  /* Return value in xmm0 */
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
                
                /* If function returns >16 byte struct, return pointer to allocated buffer (returned in RAX) */
                if (callee_uses_sret) {
                    int call_res_temp = c->next_temp_id++;
                    ir_emit_mov_reg(call_res_temp, VREG_0);
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
            return 0xFF;
    }
}

