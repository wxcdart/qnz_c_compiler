/* Variable Declarations and Assignments */
#ifndef ZERO_CC_STATEMENTS_VARIABLES_H
#define ZERO_CC_STATEMENTS_VARIABLES_H

/* Compound Block: { ... } */
static inline void zero_cc_stmt_block(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_LBRACE */
    while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
        zero_cc_parse_stmt(c);
    }
    if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
}

/* Inline Assembly: __asm__ (...) or __asm (...) or asm (...) */
static inline void zero_cc_stmt_asm(ZeroCCCompiler *c) {
    while (c->tokens[c->token_pos].type != TK_SEMI && c->tokens[c->token_pos].type != TK_EOF) {
        c->token_pos++;
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Variable Declaration with initialization and codegen */
static inline void zero_cc_stmt_decl(ZeroCCCompiler *c, ZeroCCTokType t, size_t decl_align_spec) {
    const char *tname = c->tokens[c->token_pos].str_val;
    ZeroCCType *found_type = zero_cc_find_typedef(c, tname);
    if (found_type || t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT ||
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

        bool is_ptr = false;
        while (c->tokens[c->token_pos].type == TK_STAR) {
            is_ptr = true;
            c->token_pos++;
        }

        if (c->tokens[c->token_pos].type == TK_IDENT) {
            char var_name[128];
            snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;

            ZeroCCType var_type = is_ptr ? zero_cc_type_ptr(NULL) :
                (decl_type_tok == TK_CHAR || decl_type_tok == TK_BOOL_KW) ? zero_cc_type_char() :
                (decl_type_tok == TK_SHORT) ? zero_cc_type_int16() :
                (decl_type_tok == TK_FLOAT_KW) ? zero_cc_type_float() :
                (decl_type_tok == TK_DOUBLE_KW) ? zero_cc_type_double() :
                zero_cc_type_int64();

            ZeroCCSym *sym = zero_cc_add_local(c, var_name, var_type);
            sym->is_auto = is_auto;

            if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                c->token_pos++;
                size_t arr_size = 0;
                if (c->tokens[c->token_pos].type == TK_NUM) {
                    arr_size = (size_t)c->tokens[c->token_pos].num_val;
                    c->token_pos++;
                }
                if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;

                size_t arr_bytes = arr_size * 8;
                sym->stack_offset = c->current_stack_frame;
                c->current_stack_frame += (int32_t)arr_bytes;
                sym->is_array = 1;
                sym->array_size = arr_size;

                if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                    c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_LBRACE) {
                        c->token_pos++;
                        size_t idx = 0;
                        while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF && idx < arr_size) {
                            if (c->tokens[c->token_pos].type == TK_NUM) {
                                int64_t val = c->tokens[c->token_pos].num_val;
                                c->token_pos++;
                                int32_t off = (int32_t)sym->stack_offset + (int32_t)(idx * 8);
                                zero_cc_emit_u8(&c->text_section, 0x48);
                                zero_cc_emit_u8(&c->text_section, 0xC7);
                                zero_cc_emit_u8(&c->text_section, 0x85);
                                zero_cc_emit_u32(&c->text_section, (uint32_t)(-off));
                                zero_cc_emit_u64(&c->text_section, (uint64_t)val);
                                idx++;
                            }
                            if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
                        }
                        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
                    }
                }
            } else {
                sym->stack_offset = c->current_stack_frame;
                c->current_stack_frame += (int32_t)var_type.size;

                if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                    c->token_pos++;
                    zero_cc_parse_expr(c);
                    if (var_type.tag == TYPE_STRUCT && var_type.size > 8) {
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0x8D);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                        int32_t off2 = (int32_t)sym->stack_offset - 8;
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0x95);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-off2));
                    } else {
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0x8D);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    }
                }
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        }
    }
}

/* Struct Member Post-Increment/Decrement: ptr->member++; or ptr->member--; */
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

/* Variable Post-Increment/Decrement: x++; or x--; */
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

/* Struct Member Array Assignment: sym.member[index] = expr; or ptr->member[index] = expr; */
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

/* Local Variable Array Assignment: sym[index] = expr; */
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

/* Assignment to Struct Member: sym.member = expr; or ptr->member = expr; */
static inline void zero_cc_stmt_member_assign(ZeroCCCompiler *c, char *ptr_name, bool is_arrow, char *mem_name) {
    c->token_pos += 2;
    if (c->tokens[c->token_pos].type == TK_IDENT) {
        snprintf(mem_name, sizeof(mem_name), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos++;

        size_t arr_index_expr = 0;
        bool member_has_idx = false;
        if (c->tokens[c->token_pos].type == TK_LBRACKET) {
            member_has_idx = true;
            c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_NUM) {
                arr_index_expr = (size_t)c->tokens[c->token_pos].num_val;
                c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
        }

        ZeroCCTokType assign_op = c->tokens[c->token_pos].type;
        if (assign_op == TK_ASSIGN || assign_op == TK_ADD_ASSIGN || assign_op == TK_SUB_ASSIGN ||
            assign_op == TK_MUL_ASSIGN || assign_op == TK_DIV_ASSIGN) {
            c->token_pos++;
            zero_cc_parse_expr(c);

            ZeroCCSym *sym = zero_cc_find_local(c, ptr_name);
            if (sym) {
                ZeroCCType *st = NULL;
                if (sym->type.tag == TYPE_STRUCT) st = &sym->type;
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
                if (member_has_idx) {
                    offset += (arr_index_expr * 4);
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

/* Pointer Dereference Assignment: *p = expr; */
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

/* Variable Assignment: x = expr; or compound assign */
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
            if (sym->type.tag == TYPE_STRUCT && sym->type.size > 8) {
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

#endif /* ZERO_CC_STATEMENTS_VARIABLES_H */
