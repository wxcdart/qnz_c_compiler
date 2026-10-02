/* Variable Declarations and Assignments */
#ifndef ZERO_CC_STATEMENTS_VARIABLES_H
#define ZERO_CC_STATEMENTS_VARIABLES_H

#include "zero_cc/statements.h"

/* Variable Declaration */
static inline void zero_cc_stmt_decl(ZeroCCCompiler *c, ZeroCCTokType t, size_t decl_align_spec) {
    (void)decl_align_spec; /* unused for now */
    const char *tname = c->tokens[c->token_pos].str_val;
    ZeroCCType *found_type = zero_cc_find_typedef(c, tname);
    if (found_type || t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT || t == TK_UNION ||
        t == TK_FLOAT_KW || t == TK_DOUBLE_KW || t == TK_BOOL_KW ||
        t == TK_SIGNED || t == TK_UNSIGNED || t == TK_SHORT || t == TK_LONG || t == TK_AUTO || t == TK_ATOMIC) {
        bool is_auto = (t == TK_AUTO);
        bool is_atomic_decl = (t == TK_ATOMIC);
        ZeroCCTokType decl_type_tok = t;
        c->token_pos++;

        if (is_atomic_decl) {
            if (c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_INT) { t = TK_INT; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_CHAR) { t = TK_CHAR; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_VOID) { t = TK_VOID; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_FLOAT_KW) { t = TK_FLOAT_KW; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) { t = TK_DOUBLE_KW; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_BOOL_KW) { t = TK_BOOL_KW; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_SHORT) { t = TK_SHORT; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_LONG) { t = TK_LONG; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_SIGNED) { t = TK_SIGNED; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_UNSIGNED) { t = TK_UNSIGNED; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_IDENT) { t = TK_IDENT; c->token_pos++; }
                else { t = TK_INT; }
                if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            } else {
                if (c->tokens[c->token_pos].type == TK_INT) { t = TK_INT; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_CHAR) { t = TK_CHAR; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_VOID) { t = TK_VOID; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_FLOAT_KW) { t = TK_FLOAT_KW; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) { t = TK_DOUBLE_KW; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_BOOL_KW) { t = TK_BOOL_KW; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_SHORT) { t = TK_SHORT; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_LONG) { t = TK_LONG; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_SIGNED) { t = TK_SIGNED; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_UNSIGNED) { t = TK_UNSIGNED; c->token_pos++; }
                else if (c->tokens[c->token_pos].type == TK_IDENT) { t = TK_IDENT; c->token_pos++; }
                else { t = TK_INT; }
            }
            tname = c->tokens[c->token_pos - 1].str_val;
            found_type = zero_cc_find_typedef(c, tname);
        }

        if (c->tokens[c->token_pos].type == TK_LPAREN && c->tokens[c->token_pos + 1].type == TK_STAR &&
            c->tokens[c->token_pos + 2].type == TK_IDENT) {
            char fn_ptr_name[128];
            snprintf(fn_ptr_name, sizeof(fn_ptr_name), "%s", c->tokens[c->token_pos + 2].str_val);
            c->token_pos += 3;
            if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_LPAREN) {
                while (c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
                    c->token_pos++;
                }
                if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            }
            ZeroCCSym *sym = zero_cc_add_local(c, fn_ptr_name, zero_cc_type_ptr(NULL));
            if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                c->token_pos++;
                zero_cc_parse_expr(c);
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x89);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        }

        if (decl_type_tok == TK_STRUCT || decl_type_tok == TK_UNION) {
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                tname = c->tokens[c->token_pos].str_val;
                found_type = zero_cc_find_struct(c, tname);
                if (!found_type) found_type = zero_cc_find_typedef(c, tname);
                c->token_pos++;
            }
        }

        bool is_ptr = false;
        while (c->tokens[c->token_pos].type == TK_STAR) {
            is_ptr = true;
            c->token_pos++;
        }

        if (c->tokens[c->token_pos].type == TK_IDENT) {
            char var_name[128];
            snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;

            ZeroCCType vtype;
            if (found_type) {
                vtype = *found_type;
            } else if (decl_type_tok == TK_CHAR || decl_type_tok == TK_BOOL_KW) {
                vtype = zero_cc_type_char();
            } else if (decl_type_tok == TK_SHORT) {
                vtype = zero_cc_type_int16();
            } else if (decl_type_tok == TK_INT) {
                vtype = zero_cc_type_int();
            } else if (decl_type_tok == TK_FLOAT_KW) {
                vtype = zero_cc_type_float();
            } else if (decl_type_tok == TK_DOUBLE_KW) {
                vtype = zero_cc_type_double();
            } else if (decl_type_tok == TK_VOID) {
                vtype = zero_cc_type_void();
            } else {
                vtype = zero_cc_type_int64();
            }
            if (is_ptr) {
                vtype = zero_cc_type_ptr(&vtype);
            }
            if (is_atomic_decl) {
                vtype.is_atomic = true;
            }
            if (found_type) {
                snprintf(vtype.struct_name, sizeof(vtype.struct_name), "%s", tname);
            }
            ZeroCCSym *sym = zero_cc_add_local(c, var_name, vtype);

            if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_LBRACE) {
                    c->token_pos++;
                    size_t m_idx = 0;
                    while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
                        if (c->tokens[c->token_pos].type == TK_DOT && c->tokens[c->token_pos + 1].type == TK_IDENT) {
                            c->token_pos++;
                            const char *dname = c->tokens[c->token_pos].str_val;
                            c->token_pos++;
                            if (c->tokens[c->token_pos].type == TK_ASSIGN) c->token_pos++;
                            for (size_t k = 0; k < sym->type.member_count; ++k) {
                                if (strcmp(sym->type.members[k].name, dname) == 0) {
                                    m_idx = k;
                                    break;
                                }
                            }
                        }
                        zero_cc_parse_expr(c);
                        size_t off = (m_idx < sym->type.member_count) ? sym->type.members[m_idx].offset : (m_idx * 8);
                        int32_t target_disp = -(int32_t)(sym->stack_offset - off);
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)target_disp);
                        m_idx++;
                        if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
                    }
                    if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
                } else {
                    zero_cc_parse_expr(c);
                    if (is_auto) {
                        sym->type = c->last_expr_is_float ? zero_cc_type_double() : zero_cc_type_int64();
                    }
                    if (c->last_expr_is_float) {
                        zero_cc_emit_u8(&c->text_section, 0xF2);
                        zero_cc_emit_u8(&c->text_section, 0x0F);
                        zero_cc_emit_u8(&c->text_section, 0x11);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    } else {
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                        if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8) {
                            int32_t off2 = (int32_t)sym->stack_offset - 8;
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0x89);
                            zero_cc_emit_u8(&c->text_section, 0x95);
                            zero_cc_emit_u32(&c->text_section, (uint32_t)(-off2));
                        }
                    }
                }
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        }
    }
}

/* Variable Post-Increment/Decrement */
static inline void zero_cc_stmt_var_incdec(ZeroCCCompiler *c, char *var_name, bool is_inc) {
    c->token_pos += 2;
    ZeroCCSym *sym = zero_cc_find_local(c, var_name);
    if (sym) {
        if (is_inc) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x83);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            zero_cc_emit_u8(&c->text_section, 0x01);
        } else {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x83);
            zero_cc_emit_u8(&c->text_section, 0xAD);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            zero_cc_emit_u8(&c->text_section, 0x01);
        }
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Variable Assignment */
static inline void zero_cc_stmt_var_assign(ZeroCCCompiler *c, char *var_name, ZeroCCTokType aop) {
    c->token_pos += 2;
    zero_cc_parse_expr(c);
    ZeroCCSym *sym = zero_cc_find_local(c, var_name);
    if (sym) {
        if (aop == TK_ADD_ASSIGN) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x01);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
        } else if (aop == TK_SUB_ASSIGN) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x29);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
        } else if (aop == TK_AND_ASSIGN) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x21);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
        } else if (aop == TK_OR_ASSIGN) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x09);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
        } else if (aop == TK_XOR_ASSIGN) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x31);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
        } else if (aop == TK_MUL_ASSIGN) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xAF);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
        } else if (aop == TK_DIV_ASSIGN || aop == TK_MOD_ASSIGN) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0xC1);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x8B);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x99);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xF7);
            zero_cc_emit_u8(&c->text_section, 0xF9);
            if (aop == TK_MOD_ASSIGN) {
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x89);
                zero_cc_emit_u8(&c->text_section, 0x95);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            } else {
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x89);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            }
        } else if (aop == TK_SHL_ASSIGN || aop == TK_SHR_ASSIGN) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0xC1);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x8B);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xD3);
            zero_cc_emit_u8(&c->text_section, (aop == TK_SHL_ASSIGN) ? 0xE0 : 0xE8);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
        } else {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8) {
                int32_t off2 = (int32_t)sym->stack_offset - 8;
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x89);
                zero_cc_emit_u8(&c->text_section, 0x95);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-off2));
            }
        }
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Struct Member Assignment */
static inline void zero_cc_stmt_member_assign(ZeroCCCompiler *c, char *ptr_name, bool is_arrow, char *mem_name) {
    c->token_pos += 2;
    if (c->tokens[c->token_pos].type == TK_IDENT) {
        strncpy(mem_name, c->tokens[c->token_pos].str_val, 127);
        mem_name[127] = '\0';
        c->token_pos++;
        ZeroCCTokType assign_op = c->tokens[c->token_pos].type;
        if (assign_op == TK_ASSIGN || assign_op == TK_ADD_ASSIGN || assign_op == TK_SUB_ASSIGN ||
            assign_op == TK_MUL_ASSIGN || assign_op == TK_DIV_ASSIGN) {
            c->token_pos++;
            zero_cc_parse_expr(c);
            ZeroCCSym *sym = zero_cc_find_local(c, ptr_name);
            if (sym) {
                ZeroCCType *st = NULL;
                if (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) st = &sym->type;
                else if (sym->type.tag == TYPE_PTR && sym->type.base) st = sym->type.base;
                if (!st) st = zero_cc_find_struct(c, sym->type.struct_name);
                if (!st) st = zero_cc_find_typedef(c, sym->type.struct_name);
                size_t offset = 0;
                if (st) {
                    for (size_t m = 0; m < st->member_count; m++) {
                        if (strcmp(st->members[m].name, mem_name) == 0) {
                            offset = st->members[m].offset;
                            break;
                        }
                    }
                }
                if (!is_arrow) {
                    int32_t eff_offset = (int32_t)sym->stack_offset - (int32_t)offset;
                    if (assign_op == TK_ADD_ASSIGN) {
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x01);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-eff_offset));
                    } else if (assign_op == TK_SUB_ASSIGN) {
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x29);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-eff_offset));
                    } else {
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-eff_offset));
                    }
                } else {
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x8B);
                    zero_cc_emit_u8(&c->text_section, 0x8D);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    if (assign_op == TK_ADD_ASSIGN) {
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x01);
                        zero_cc_emit_u8(&c->text_section, 0x81);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
                    } else if (assign_op == TK_SUB_ASSIGN) {
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x29);
                        zero_cc_emit_u8(&c->text_section, 0x81);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
                    } else {
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0x81);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
                    }
                }
            }
        }
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Pointer Dereference Assignment */
static inline void zero_cc_stmt_ptr_deref_assign(ZeroCCCompiler *c, char *var_name) {
    c->token_pos += 3;
    zero_cc_parse_expr(c);
    ZeroCCSym *sym = zero_cc_find_local(c, var_name);
    if (sym) {
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x8B);
        zero_cc_emit_u8(&c->text_section, 0x8D);
        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
        zero_cc_emit_u8(&c->text_section, 0x89);
        zero_cc_emit_u8(&c->text_section, 0x01);
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Local Variable Array Assignment */
static inline void zero_cc_stmt_var_array_assign(ZeroCCCompiler *c, char *var_name) {
    c->token_pos += 2;
    zero_cc_parse_expr(c);
    if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0xC1);
    zero_cc_emit_u8(&c->text_section, 0xE0);
    zero_cc_emit_u8(&c->text_section, 0x02);
    ZeroCCSym *sym = zero_cc_find_local(c, var_name);
    if (sym) {
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x8B);
        zero_cc_emit_u8(&c->text_section, 0x8D);
        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x01);
        zero_cc_emit_u8(&c->text_section, 0xC8);
    }
    zero_cc_emit_u8(&c->text_section, 0x50);
    if (c->tokens[c->token_pos].type == TK_ASSIGN) {
        c->token_pos++;
        zero_cc_parse_expr(c);
        zero_cc_emit_u8(&c->text_section, 0x59);
        zero_cc_emit_u8(&c->text_section, 0x89);
        zero_cc_emit_u8(&c->text_section, 0x01);
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Struct Member Array Assignment */
static inline void zero_cc_stmt_member_array_assign(ZeroCCCompiler *c, char *base_name, bool is_arrow, char *mem_name) {
    c->token_pos += 4;
    zero_cc_parse_expr(c);
    if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0xC1);
    zero_cc_emit_u8(&c->text_section, 0xE0);
    zero_cc_emit_u8(&c->text_section, 0x02);
    zero_cc_emit_u8(&c->text_section, 0x50);
    ZeroCCSym *sym = zero_cc_find_local(c, base_name);
    size_t offset = 0;
    if (sym) {
        ZeroCCType *st = sym->type.base ? sym->type.base : zero_cc_find_struct(c, sym->type.struct_name);
        if (!st) st = zero_cc_find_typedef(c, sym->type.struct_name);
        if (st) {
            for (size_t m = 0; m < st->member_count; m++) {
                if (strcmp(st->members[m].name, mem_name) == 0) {
                    offset = st->members[m].offset;
                    break;
                }
            }
        }
        if (!is_arrow) {
            int32_t eff_offset = (int32_t)sym->stack_offset - (int32_t)offset;
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x8B);
            zero_cc_emit_u8(&c->text_section, 0x8D);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-eff_offset));
        } else {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x8B);
            zero_cc_emit_u8(&c->text_section, 0x8D);
            zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x8B);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
        }
    }
    zero_cc_emit_u8(&c->text_section, 0x58);
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x01);
    zero_cc_emit_u8(&c->text_section, 0xC8);
    zero_cc_emit_u8(&c->text_section, 0x50);
    if (c->tokens[c->token_pos].type == TK_ASSIGN) {
        c->token_pos++;
        zero_cc_parse_expr(c);
        zero_cc_emit_u8(&c->text_section, 0x59);
        zero_cc_emit_u8(&c->text_section, 0x89);
        zero_cc_emit_u8(&c->text_section, 0x01);
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Struct Member Post-Increment/Decrement */
static inline void zero_cc_stmt_member_incdec(ZeroCCCompiler *c, char *ptr_name, char *mem_name, bool is_inc) {
    c->token_pos += 4;
    ZeroCCSym *sym = zero_cc_find_local(c, ptr_name);
    if (sym) {
        ZeroCCType *st = sym->type.base ? sym->type.base : zero_cc_find_struct(c, sym->type.struct_name);
        if (!st) st = zero_cc_find_typedef(c, sym->type.struct_name);
        size_t offset = 0;
        if (st) {
            for (size_t m = 0; m < st->member_count; m++) {
                if (strcmp(st->members[m].name, mem_name) == 0) {
                    offset = st->members[m].offset;
                    break;
                }
            }
        }
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x8B);
        zero_cc_emit_u8(&c->text_section, 0x8D);
        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
        if (is_inc) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x83);
            zero_cc_emit_u8(&c->text_section, 0x81);
            zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
            zero_cc_emit_u8(&c->text_section, 0x01);
        } else {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x83);
            zero_cc_emit_u8(&c->text_section, 0xA9);
            zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
            zero_cc_emit_u8(&c->text_section, 0x01);
        }
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

#endif /* ZERO_CC_STATEMENTS_VARIABLES_H */
