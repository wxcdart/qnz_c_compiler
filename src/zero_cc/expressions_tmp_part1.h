/* Recursive Descent Parser Declarations */
#ifndef ZERO_CC_EXPRESSIONS_H
#define ZERO_CC_EXPRESSIONS_H

static inline void zero_cc_parse_expr(ZeroCCCompiler *c);
static inline void zero_cc_parse_stmt(ZeroCCCompiler *c);
static inline int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c);

/* Primary Expression Parser */
static inline void zero_cc_parse_primary(ZeroCCCompiler *c) {
    ZeroCCTokenV2 tok = c->tokens[c->token_pos];

    if (tok.type == TK_NUM || tok.type == TK_CHAR_LIT || tok.type == TK_NULLPTR || tok.type == TK_TRUE || tok.type == TK_FALSE) {
        c->token_pos++;
        int64_t val = tok.num_val;
        if (tok.type == TK_NULLPTR) val = 0;
        else if (tok.type == TK_TRUE) val = 1;
        else if (tok.type == TK_FALSE) val = 0;
        /* mov rax, immediate64 */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0xB8);
        zero_cc_emit_u64(&c->text_section, (uint64_t)val);
        c->last_expr_is_float = 0;
        return;
    }

    if (tok.type == TK_FLOAT) {
        c->token_pos++;
        /* place double in rodata and movsd into xmm0 via RIP-relative addressing */
        size_t float_offset = c->rodata_section.length;
        zero_cc_buf_put_bytes(&c->rodata_section, &tok.float_val, sizeof(tok.float_val));
        /* movsd xmm0, [rip + disp32] -> F2 0F 10 05 disp32 */
        zero_cc_emit_u8(&c->text_section, 0xF2);
        zero_cc_emit_u8(&c->text_section, 0x0F);
        zero_cc_emit_u8(&c->text_section, 0x10);
        zero_cc_emit_u8(&c->text_section, 0x05);
        size_t patch_pos = c->text_section.length;
        zero_cc_emit_u32(&c->text_section, 0);
        if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
            c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
            c->str_relocs[c->str_reloc_count].rodata_offset = float_offset;
            c->str_reloc_count++;
        }
        c->last_expr_is_float = 1;
        return;
    }

    if (tok.type == TK_STRING) {
        c->token_pos++;
        size_t str_offset = c->rodata_section.length;
        zero_cc_buf_put_bytes(&c->rodata_section, tok.str_val, strlen(tok.str_val) + 1);
        
        /* lea rax, [rip + offset] */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x8D);
        zero_cc_emit_u8(&c->text_section, 0x05);
        size_t patch_pos = c->text_section.length;
        zero_cc_emit_u32(&c->text_section, 0);

        if (c->str_reloc_count < 512) {
            c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
            c->str_relocs[c->str_reloc_count].rodata_offset = str_offset;
            c->str_reloc_count++;
        }
        return;
    }

    /* GNU Statement Expression: ({ ... }) */
    if (tok.type == TK_LPAREN && c->tokens[c->token_pos + 1].type == TK_LBRACE) {
        c->token_pos += 2;
        while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
            zero_cc_parse_stmt(c);
        }
        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        return;
    }

    /* Type Casts: (type)expr or (type*)expr */
    if (tok.type == TK_LPAREN) {
        size_t cast_start = c->token_pos;
        ZeroCCTokType next_t = c->tokens[c->token_pos + 1].type;
        if (next_t == TK_IDENT || next_t == TK_INT || next_t == TK_CHAR || next_t == TK_VOID ||
            next_t == TK_FLOAT_KW || next_t == TK_DOUBLE_KW || next_t == TK_BOOL_KW ||
            next_t == TK_SIGNED || next_t == TK_UNSIGNED || next_t == TK_SHORT || next_t == TK_LONG) {
            ZeroCCType *cast_t = zero_cc_find_typedef(c, c->tokens[c->token_pos + 1].str_val);
            if (cast_t || next_t != TK_IDENT) {
                c->token_pos += 2; /* Skip '(' and type */
                while (c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_LBRACE) {
                        /* Compound Literal: (Type){ ... } */
                        c->token_pos++;
                        c->current_stack_frame += 64;
                        int32_t temp_offset = c->current_stack_frame;
                        size_t m_idx = 0;
                        while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
                            bool is_designated = false;
                            char dname[128] = "";
                            if (c->tokens[c->token_pos].type == TK_DOT && c->tokens[c->token_pos + 1].type == TK_IDENT) {
                                is_designated = true;
                                zero_strlcpy(dname, c->tokens[c->token_pos + 1].str_val, sizeof(dname));
                                c->token_pos += 2;
                                if (c->tokens[c->token_pos].type == TK_ASSIGN) c->token_pos++;
                            }
                            zero_cc_parse_expr(c);
                            size_t member_off = m_idx * 8;
                            if (cast_t && cast_t->tag == TYPE_STRUCT && is_designated) {
                                for (size_t k = 0; k < cast_t->member_count; ++k) {
                                    if (strcmp(cast_t->members[k].name, dname) == 0) {
                                        member_off = cast_t->members[k].offset;
                                        break;
                                    }
                                }
                            }
                            int32_t target_disp = -(int32_t)(temp_offset - (int32_t)member_off);
                            /* mov [rbp + target_disp], rax */
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0x89);
                            zero_cc_emit_u8(&c->text_section, 0x85);
                            zero_cc_emit_u32(&c->text_section, (uint32_t)target_disp);
                            if (!is_designated) m_idx++;
                            if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
                        }
                        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
                        /* lea rax, [rbp - temp_offset] */
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x8D);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-temp_offset));
                        return;
                    }
                    zero_cc_parse_primary(c);
                    return;
                }
                c->token_pos = cast_start; /* backtrack to '(' if not a cast */
            }
        }
    }

    /* C11 _Generic Compile-Time Polymorphism: _Generic(controlling_expr, type1: expr1, type2: expr2, default: expr_def) */
    if (tok.type == TK_GENERIC) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++;

        /* 1. Evaluate controlling expression's type */
        size_t save_text_len = c->text_section.length;
        c->last_expr_is_float = 0;
        zero_cc_parse_expr(c);
        bool ctrl_is_float = c->last_expr_is_float;
        /* Discard generated code for controlling expression since _Generic is pure compile-time selection */
        c->text_section.length = save_text_len;

        if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;

        /* 2. Scan association list */
        bool matched = false;
        size_t default_expr_start = 0;

        while (c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
            if (c->tokens[c->token_pos].type == TK_DEFAULT) {
                c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_COLON) c->token_pos++;
                default_expr_start = c->token_pos;
                /* Skip expression tokens until comma or rparen */
                int paren_depth = 0;
                while (c->tokens[c->token_pos].type != TK_EOF) {
                    ZeroCCTokType tt = c->tokens[c->token_pos].type;
                    if (tt == TK_LPAREN) paren_depth++;
                    else if (tt == TK_RPAREN) {
                        if (paren_depth == 0) break;
                        paren_depth--;
                    } else if (tt == TK_COMMA && paren_depth == 0) {
                        break;
                    }
                    c->token_pos++;
                }
            } else {
                /* Type association: match type against controlling expression type */
                ZeroCCTokType assoc_tok = c->tokens[c->token_pos].type;
                c->token_pos++;
                while (c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_COLON) c->token_pos++;

                bool is_match = false;
                if (ctrl_is_float && (assoc_tok == TK_FLOAT_KW || assoc_tok == TK_DOUBLE_KW)) {
                    is_match = true;
                } else if (!ctrl_is_float && (assoc_tok == TK_INT || assoc_tok == TK_CHAR || assoc_tok == TK_LONG || assoc_tok == TK_SHORT || assoc_tok == TK_BOOL_KW || assoc_tok == TK_IDENT)) {
                    is_match = true;
                }

                if (is_match && !matched) {
                    matched = true;
                    zero_cc_parse_expr(c);
                } else {
                    /* Skip non-matching branch tokens */
                    int paren_depth = 0;
                    while (c->tokens[c->token_pos].type != TK_EOF) {
                        ZeroCCTokType tt = c->tokens[c->token_pos].type;
                        if (tt == TK_LPAREN) paren_depth++;
                        else if (tt == TK_RPAREN) {
                            if (paren_depth == 0) break;
                            paren_depth--;
                        } else if (tt == TK_COMMA && paren_depth == 0) {
                            break;
                        }
                        c->token_pos++;
                    }
                }
            }
            if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        }

        if (!matched && default_expr_start > 0) {
            c->token_pos = default_expr_start;
            zero_cc_parse_expr(c);
            /* skip to RPAREN */
            while (c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
                c->token_pos++;
            }
        }

        if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        return;
    }

    /* Parenthesized Expression: ( expr ) or ( expr1, expr2, ... ) */
    if (tok.type == TK_LPAREN) {
        c->token_pos++;
        zero_cc_parse_expr(c);
        while (c->tokens[c->token_pos].type == TK_COMMA) {
            c->token_pos++;
            zero_cc_parse_expr(c);
        }
        if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        return;
    }

    /* Identifier: Variable, Member Access, Array Access, Enum Constant, or Function Call */
    if (tok.type == TK_IDENT) {
        ZeroCCEnumConst *ec = zero_cc_find_enum_const(c, tok.str_val);
        if (ec && c->tokens[c->token_pos + 1].type != TK_LPAREN &&
            c->tokens[c->token_pos + 1].type != TK_DOT && c->tokens[c->token_pos + 1].type != TK_ARROW) {
            c->token_pos++;
            /* mov rax, imm64 */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xB8);
            zero_cc_emit_u64(&c->text_section, (uint64_t)ec->value);
            return;
        }

        char name[128];
        snprintf(name, sizeof(name), "%s", tok.str_val);
        c->token_pos++;

        /* Top-level function address (function pointer value) */
        ZeroCCFunction *fn_val = zero_cc_find_function(c, name);
        if (fn_val && c->tokens[c->token_pos].type != TK_LPAREN) {
            /* lea rax, [rip + offset] */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x8D);
            zero_cc_emit_u8(&c->text_section, 0x05);
            int32_t rel = (int32_t)(fn_val->text_offset - (c->text_section.length + 4));
            zero_cc_emit_u32(&c->text_section, (uint32_t)rel);
            return;
        }

        /* Multi-argument Function Call (Direct or Indirect Function Pointer) */
        if (c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
            static const uint8_t arg_regs[6][3] = {
                {0x48, 0x89, 0xC7}, /* mov rdi, rax */
                {0x48, 0x89, 0xC6}, /* mov rsi, rax */
                {0x48, 0x89, 0xC2}, /* mov rdx, rax */
                {0x48, 0x89, 0xC1}, /* mov rcx, rax */
                {0x49, 0x89, 0xC0}, /* mov r8,  rax */
                {0x49, 0x89, 0xC1}  /* mov r9,  rax */
            };

            int arg_idx = 0;
            int int_arg_count = 0;
            int float_arg_count = 0;
            while (c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
                zero_cc_parse_expr(c);
                if (c->last_expr_is_float) {
                    /* assign to xmm registers in-order for floating args */
                    if (float_arg_count < 8) {
                        if (float_arg_count != 0) {
                            /* movsd xmmN, xmm0  -> F2 0F 10 C0+ (N<<3) */
                            zero_cc_emit_u8(&c->text_section, 0xF2);
                            zero_cc_emit_u8(&c->text_section, 0x0F);
                            zero_cc_emit_u8(&c->text_section, 0x10);
                            uint8_t modrm = (uint8_t)(0xC0 + ((float_arg_count & 7) << 3) + 0);
                            zero_cc_emit_u8(&c->text_section, modrm);
                        }
                        /* already in xmm0 for float_arg_count == 0 */
                    } else {
                        /* fallback: place on stack (sub rsp,8; movsd [rsp], xmm0) */
                        zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x83); zero_cc_emit_u8(&c->text_section, 0xEC); zero_cc_emit_u8(&c->text_section, 0x08);
                        zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x11); zero_cc_emit_u8(&c->text_section, 0x04); zero_cc_emit_u8(&c->text_section, 0x24);
                    }
                    float_arg_count++;
                } else {
                    /* integer/pointer arg: push rax for later register assignment or stack */
                    if (int_arg_count < 6) {
                        zero_cc_emit_u8(&c->text_section, 0x50); /* push rax */
                    } else {
                        zero_cc_emit_u8(&c->text_section, 0x50); /* push rax for stack args too */
                    }
                    int_arg_count++;
                }
                arg_idx++;
                if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
            }
            if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;

            int reg_args = (int_arg_count > 6) ? 6 : int_arg_count;
            for (int i = reg_args - 1; i >= 0; i--) {
                zero_cc_emit_u8(&c->text_section, 0x58); /* pop rax */
                zero_cc_emit_u8(&c->text_section, arg_regs[i][0]);
                zero_cc_emit_u8(&c->text_section, arg_regs[i][1]);
                zero_cc_emit_u8(&c->text_section, arg_regs[i][2]);
            }

            /* Clear al for System V AMD64 ABI variadic compatibility */
            zero_cc_emit_u8(&c->text_section, 0x31);
            zero_cc_emit_u8(&c->text_section, 0xC0);

            /* Resolve Internal Function, Function Pointer, or Host Dynamic Symbol */
            ZeroCCSym *sym = zero_cc_find_local(c, name);
            if (sym) {
                /* Indirect function pointer call: mov r11, [rbp - offset]; call r11 */
                zero_cc_emit_u8(&c->text_section, 0x4C);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x9D);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                zero_cc_emit_u8(&c->text_section, 0x41);
                zero_cc_emit_u8(&c->text_section, 0xFF);
                zero_cc_emit_u8(&c->text_section, 0xD3);
            } else {
                ZeroCCFunction *fn = zero_cc_find_function(c, name);
                void *host_fn = zero_cc_resolve_symbol(name);
                if (fn) {
                    /* Internal function call: call rel32 (record reloc for final link/JIT patch) */
                    zero_cc_emit_u8(&c->text_section, 0xE8);
                    size_t patch_pos = c->text_section.length;
                    int32_t rel = (int32_t)(fn->text_offset - (patch_pos + 4));
                    zero_cc_emit_u32(&c->text_section, (uint32_t)rel);
                    if (c->reloc_count < 512) {
                        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                        r->text_patch_offset = patch_pos;
                        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", name);
                        r->is_call = true;
                    }
                } else if (host_fn) {
                    /* Host libc function call in JIT mode: mov r11, host_fn; call r11 */
                    zero_cc_emit_u8(&c->text_section, 0x49);
                    zero_cc_emit_u8(&c->text_section, 0xBB);
                    zero_cc_emit_u64(&c->text_section, (uint64_t)host_fn);
                    zero_cc_emit_u8(&c->text_section, 0x41);
                    zero_cc_emit_u8(&c->text_section, 0xFF);
                    zero_cc_emit_u8(&c->text_section, 0xD3);
                } else {
                    /* Forward or external symbol: call rel32 with placeholder */
                    zero_cc_emit_u8(&c->text_section, 0xE8);
                    size_t patch_pos = c->text_section.length;
                    zero_cc_emit_u32(&c->text_section, 0);
                    if (c->reloc_count < 512) {
                        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                        r->text_patch_offset = patch_pos;
                        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", name);
                        r->is_call = true;
                    }
                }
            }
            return;
        }

        /* Local Variable Access & Struct Pointer De-referencing (ptr->member) */
        ZeroCCSym *sym = zero_cc_find_local(c, name);
        if (sym) {
            bool sym_is_direct_struct = (sym->type.tag == TYPE_STRUCT);
            if (sym_is_direct_struct) {
                /* lea rax, [rbp - offset] */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8D);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            } else {
                /* mov rax, [rbp - offset] */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            }

            /* Handle in-expression assignment: sym = expr, sym += expr, sym -= expr */
            if (c->tokens[c->token_pos].type == TK_ASSIGN ||
                c->tokens[c->token_pos].type == TK_ADD_ASSIGN ||
                c->tokens[c->token_pos].type == TK_SUB_ASSIGN) {
                ZeroCCTokType aop = c->tokens[c->token_pos].type;
                c->token_pos++;
                zero_cc_parse_expr(c); /* RHS in rax */
                if (aop == TK_ADD_ASSIGN) {
                    /* add [rbp - offset], rax */
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x01);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    /* mov rax, [rbp - offset] */
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x8B);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                } else if (aop == TK_SUB_ASSIGN) {
                    /* sub [rbp - offset], rax */
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x29);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    /* mov rax, [rbp - offset] */
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x8B);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                } else {
                    /* mov [rbp - offset], rax */
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                }
                return;
            }

            /* Handle arrow / dot: sym->member or sym.member */
            while (c->tokens[c->token_pos].type == TK_ARROW || c->tokens[c->token_pos].type == TK_DOT) {
                bool is_arrow = (c->tokens[c->token_pos].type == TK_ARROW);
                c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    const char *mem_name = c->tokens[c->token_pos].str_val;
                    c->token_pos++;

                    /* Find struct definition */
                    ZeroCCType *st = NULL;
                    if (sym->type.tag == TYPE_STRUCT) {
                        st = &sym->type;
                    } else if (sym->type.tag == TYPE_PTR && sym->type.base) {
                        st = sym->type.base;
                    }
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
                        /* Local struct instance: sym.member is at [rbp - (sym_offset - offset)] */
                        int32_t eff_offset = (int32_t)sym->stack_offset - (int32_t)offset;
                        /* mov rax, [rbp - eff_offset] */
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x8B);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-eff_offset));
                    } else {
                        /* Pointer to struct: ptr->member */
                        /* mov rax, [rax + offset] */
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x8B);
                        zero_cc_emit_u8(&c->text_section, 0x80);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
                    }
                }
            }

            /* Handle array indexing: sym[index] */
            while (c->tokens[c->token_pos].type == TK_LBRACKET) {
                c->token_pos++;
                zero_cc_emit_u8(&c->text_section, 0x50); /* push base ptr (rax) */
                zero_cc_parse_expr(c);                   /* index in rax */
                if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;

                /* shl rax, 2 (scale 4-byte or 8-byte) */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0xC1);
                zero_cc_emit_u8(&c->text_section, 0xE0);
                zero_cc_emit_u8(&c->text_section, 0x02);

                /* pop base ptr to rcx */
                zero_cc_emit_u8(&c->text_section, 0x59); /* pop rcx */
                /* add rax, rcx */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x01);
                zero_cc_emit_u8(&c->text_section, 0xC8);

                /* movsxd rax, dword ptr [rax] (load 32-bit int with sign extension) */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x63);
                zero_cc_emit_u8(&c->text_section, 0x00);
            }
        }
        return;
    }
}

/* Unary Expression Parser */
static inline void zero_cc_parse_unary(ZeroCCCompiler *c) {
    ZeroCCTokType t = c->tokens[c->token_pos].type;

    if (t == TK_AMP) { /* Address-of: &var or &ptr->member or &(Type){ ... } or &(var) */
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) {
            if (c->tokens[c->token_pos + 1].type == TK_IDENT && c->tokens[c->token_pos + 2].type == TK_RPAREN) {
                ZeroCCSym *sym = zero_cc_find_local(c, c->tokens[c->token_pos + 1].str_val);
                if (sym) {
                    c->token_pos += 3; /* skip '(', ident, ')' */
                    /* lea rax, [rbp - offset] */
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x8D);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    return;
                }
            }
            zero_cc_parse_primary(c);
            return;
        }
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            ZeroCCSym *sym = zero_cc_find_local(c, c->tokens[c->token_pos].str_val);
            c->token_pos++;
            if (sym) {
                /* lea rax, [rbp - offset] */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8D);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));

                /* Handle &ptr->member or &arr[idx] */
                if (c->tokens[c->token_pos].type == TK_ARROW) {
                    c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_IDENT) {
                        const char *mem_name = c->tokens[c->token_pos].str_val;
                        c->token_pos++;
                        ZeroCCType *st = sym->type.base ? sym->type.base : zero_cc_find_struct(c, sym->type.struct_name);
                        size_t offset = 0;
                        if (st) {
                            for (size_t m = 0; m < st->member_count; m++) {
                                if (strcmp(st->members[m].name, mem_name) == 0) {
                                    offset = st->members[m].offset;
                                    break;
                                }
                            }
                        }
                        /* mov rax, [rax]; add rax, offset */
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x8B);
                        zero_cc_emit_u8(&c->text_section, 0x00);
                        if (offset > 0) {
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0x05);
                            zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
                        }
                    }
                }
                if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                    c->token_pos++;
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x8B);
                    zero_cc_emit_u8(&c->text_section, 0x00); /* mov rax, [rax] */
                    zero_cc_emit_u8(&c->text_section, 0x50); /* push base */
                    zero_cc_parse_expr(c);                   /* index */
                    if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                    zero_cc_emit_u8(&c->text_section, 0x59); /* pop rcx */
                    /* add rax, rcx */
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x01);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                }
            }
        }
        return;
    }

    if (t == TK_STAR) { /* Dereference: *ptr */
        c->token_pos++;
        zero_cc_parse_unary(c);
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x8B);
        zero_cc_emit_u8(&c->text_section, 0x00);
        return;
    }

    if (t == TK_BANG) { /* Logical NOT: !expr */
        c->token_pos++;
        zero_cc_parse_unary(c);
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x85);
        zero_cc_emit_u8(&c->text_section, 0xC0);
        zero_cc_emit_u8(&c->text_section, 0x0F);
        zero_cc_emit_u8(&c->text_section, 0x94);
        zero_cc_emit_u8(&c->text_section, 0xC0);
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x0F);
        zero_cc_emit_u8(&c->text_section, 0xB6);
        zero_cc_emit_u8(&c->text_section, 0xC0);
        return;
    }

    if (t == TK_MINUS) {
        c->token_pos++;
        zero_cc_parse_unary(c);
        /* neg rax: 48 F7 D8 */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0xF7);
        zero_cc_emit_u8(&c->text_section, 0xD8);
        return;
    }

    if (t == TK_TILDE) {
        c->token_pos++;
        zero_cc_parse_unary(c);
        /* not rax: 48 F7 D0 */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0xF7);
        zero_cc_emit_u8(&c->text_section, 0xD0);
        return;
    }

    if (t == TK_PLUS) {
        c->token_pos++;
        zero_cc_parse_unary(c);
        return;
    }

    if (t == TK_SIZEOF) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) c->token_pos++;
        size_t sz = 8;
        ZeroCCTokType st = c->tokens[c->token_pos].type;
        if (st == TK_IDENT) {
            ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
            if (!tdef) tdef = zero_cc_find_struct(c, c->tokens[c->token_pos].str_val);
            if (tdef) sz = tdef->size;
        } else if (st == TK_CHAR || st == TK_BOOL_KW) sz = 1;
        else if (st == TK_SHORT) sz = 2;
        else if (st == TK_INT || st == TK_FLOAT_KW) sz = 4;
        else if (st == TK_LONG || st == TK_DOUBLE_KW || st == TK_VOID) sz = 8;
        c->token_pos++;
        while (c->tokens[c->token_pos].type == TK_STAR) {
            sz = 8; /* Pointer size */
            c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;

        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0xB8);
        zero_cc_emit_u64(&c->text_section, (uint64_t)sz);
        return;
    }

    zero_cc_parse_primary(c);
}

/* Binary Arithmetic & Relational Expression Parser */
static inline void zero_cc_parse_expr(ZeroCCCompiler *c) {
    zero_cc_parse_unary(c);

    while (true) {
        ZeroCCTokType op = c->tokens[c->token_pos].type;
        if (op != TK_PLUS && op != TK_MINUS && op != TK_STAR && op != TK_SLASH && op != TK_PERCENT &&
            op != TK_AMP && op != TK_PIPE && op != TK_CARET &&
            op != TK_SHL && op != TK_SHR &&
            op != TK_EQ && op != TK_NE && op != TK_LT && op != TK_LE && op != TK_GT && op != TK_GE &&
            op != TK_LOGAND && op != TK_LOGOR) {
            break;
        }
        /* detect float-ness of LHS before consuming operator */
        bool lhs_is_float = c->last_expr_is_float;
        c->token_pos++;

        /* Short-circuit Logical Operators evaluated before pushing LHS */
        if (op == TK_LOGOR) {
            /* Short-circuit Logical OR: LHS in rax */
            /* test rax, rax */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            /* jnz to true_branch */
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x85);
            size_t jnz_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);

            /* RHS in rax */
            zero_cc_parse_unary(c);
            while (c->tokens[c->token_pos].type == TK_EQ || c->tokens[c->token_pos].type == TK_NE ||
                   c->tokens[c->token_pos].type == TK_LT || c->tokens[c->token_pos].type == TK_LE ||
                   c->tokens[c->token_pos].type == TK_GT || c->tokens[c->token_pos].type == TK_GE ||
                   c->tokens[c->token_pos].type == TK_PLUS || c->tokens[c->token_pos].type == TK_MINUS ||
                   c->tokens[c->token_pos].type == TK_STAR || c->tokens[c->token_pos].type == TK_SLASH) {
                ZeroCCTokType sub_op = c->tokens[c->token_pos].type;
                c->token_pos++;
                zero_cc_emit_u8(&c->text_section, 0x50); /* push rax */
                zero_cc_parse_unary(c);
                zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x89); zero_cc_emit_u8(&c->text_section, 0xC1); /* mov rcx, rax */
                zero_cc_emit_u8(&c->text_section, 0x58); /* pop rax */
                if (sub_op == TK_PLUS) {
                    zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x01); zero_cc_emit_u8(&c->text_section, 0xC8);
                } else if (sub_op == TK_MINUS) {
                    zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x29); zero_cc_emit_u8(&c->text_section, 0xC8);
                } else {
                    /* cmp rax, rcx */
                    zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x39); zero_cc_emit_u8(&c->text_section, 0xC8);
                    zero_cc_emit_u8(&c->text_section, 0x0F);
                    if (sub_op == TK_EQ) zero_cc_emit_u8(&c->text_section, 0x94);
                    else if (sub_op == TK_NE) zero_cc_emit_u8(&c->text_section, 0x95);
                    else if (sub_op == TK_LT) zero_cc_emit_u8(&c->text_section, 0x9C);
                    else if (sub_op == TK_LE) zero_cc_emit_u8(&c->text_section, 0x9E);
                    else if (sub_op == TK_GT) zero_cc_emit_u8(&c->text_section, 0x9F);
                    else if (sub_op == TK_GE) zero_cc_emit_u8(&c->text_section, 0x9D);
                    zero_cc_emit_u8(&c->text_section, 0xC0);
                    zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0xB6); zero_cc_emit_u8(&c->text_section, 0xC0);
                }
            }
            /* test rax, rax; setne al; movzx rax, al */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x95);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xB6);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            /* jmp done */
            zero_cc_emit_u8(&c->text_section, 0xEB);
            zero_cc_emit_u8(&c->text_section, 0x07); /* skip 7 bytes of mov rax, 1 */

            /* true_branch: */
            int32_t jnz_rel = (int32_t)(c->text_section.length - (jnz_pos + 4));
            memcpy(c->text_section.data + jnz_pos, &jnz_rel, 4);
            /* mov rax, 1 */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xC7);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u32(&c->text_section, 1);
            continue;
        }

        if (op == TK_LOGAND) {
            /* Short-circuit Logical AND: LHS in rax */
            /* test rax, rax */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            /* jz to false_branch */
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x84);
            size_t jz_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);

            /* RHS in rax */
            zero_cc_parse_unary(c);
            while (c->tokens[c->token_pos].type == TK_EQ || c->tokens[c->token_pos].type == TK_NE ||
                   c->tokens[c->token_pos].type == TK_LT || c->tokens[c->token_pos].type == TK_LE ||
                   c->tokens[c->token_pos].type == TK_GT || c->tokens[c->token_pos].type == TK_GE ||
                   c->tokens[c->token_pos].type == TK_PLUS || c->tokens[c->token_pos].type == TK_MINUS ||
                   c->tokens[c->token_pos].type == TK_STAR || c->tokens[c->token_pos].type == TK_SLASH) {
                ZeroCCTokType sub_op = c->tokens[c->token_pos].type;
                c->token_pos++;
                zero_cc_emit_u8(&c->text_section, 0x50); /* push rax */
                zero_cc_parse_unary(c);
                zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x89); zero_cc_emit_u8(&c->text_section, 0xC1); /* mov rcx, rax */
                zero_cc_emit_u8(&c->text_section, 0x58); /* pop rax */
                if (sub_op == TK_PLUS) {
                    zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x01); zero_cc_emit_u8(&c->text_section, 0xC8);
                } else if (sub_op == TK_MINUS) {
                    zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x29); zero_cc_emit_u8(&c->text_section, 0xC8);
                } else {
                    /* cmp rax, rcx */
                    zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x39); zero_cc_emit_u8(&c->text_section, 0xC8);
                    zero_cc_emit_u8(&c->text_section, 0x0F);
                    if (sub_op == TK_EQ) zero_cc_emit_u8(&c->text_section, 0x94);
                    else if (sub_op == TK_NE) zero_cc_emit_u8(&c->text_section, 0x95);
                    else if (sub_op == TK_LT) zero_cc_emit_u8(&c->text_section, 0x9C);
                    else if (sub_op == TK_LE) zero_cc_emit_u8(&c->text_section, 0x9E);
                    else if (sub_op == TK_GT) zero_cc_emit_u8(&c->text_section, 0x9F);
                    else if (sub_op == TK_GE) zero_cc_emit_u8(&c->text_section, 0x9D);
                    zero_cc_emit_u8(&c->text_section, 0xC0);
                    zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0xB6); zero_cc_emit_u8(&c->text_section, 0xC0);
                }
            }
            /* test rax, rax; setne al; movzx rax, al */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x95);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xB6);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            /* jmp done */
            zero_cc_emit_u8(&c->text_section, 0xEB);
            zero_cc_emit_u8(&c->text_section, 0x07); /* skip 7 bytes of mov rax, 0 */

            /* false_branch: */
            int32_t jz_rel = (int32_t)(c->text_section.length - (jz_pos + 4));
            memcpy(c->text_section.data + jz_pos, &jz_rel, 4);
            /* mov rax, 0 */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xC7);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u32(&c->text_section, 0);
            continue;
        }

        /* store LHS on stack: if float, store 8-byte double; else push rax (8-byte int) */
        if (lhs_is_float) {
            /* sub rsp,8; movsd [rsp], xmm0 */
            zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x83); zero_cc_emit_u8(&c->text_section, 0xEC); zero_cc_emit_u8(&c->text_section, 0x08);
            zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x11);
            /* modrm for [rsp] -> 0x04 0x24 addressing: use movsd [rsp], xmm0 -> F2 0F 11 04 24 */
            zero_cc_emit_u8(&c->text_section, 0x04); zero_cc_emit_u8(&c->text_section, 0x24);
        } else {
            zero_cc_emit_u8(&c->text_section, 0x50); /* push rax (LHS int) */
        }

        zero_cc_parse_unary(c);                  /* RHS in rax or xmm0 */

        bool rhs_is_float = c->last_expr_is_float;

        /* If either operand is float, perform SSE double ops */
        if (lhs_is_float || rhs_is_float) {
            /* Load LHS from stack into xmm0 as double (cvtsi2sd if int, movsd if float) */
            if (lhs_is_float) {
                /* movsd xmm0, [rsp] -> F2 0F 10 04 24 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x10);
                zero_cc_emit_u8(&c->text_section, 0x04); zero_cc_emit_u8(&c->text_section, 0x24);
            } else {
                /* cvtsi2sd xmm0, qword [rsp] -> F2 0F 2A 04 24 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x2A);
                zero_cc_emit_u8(&c->text_section, 0x04); zero_cc_emit_u8(&c->text_section, 0x24);
            }

            /* Load RHS into xmm1: if RHS is float, xmm0 currently holds RHS, so move xmm0->xmm1; else convert rax->xmm1 */
            if (rhs_is_float) {
                /* movsd xmm1, xmm0 -> F2 0F 11 C8? We'll use movsd xmm1, xmm0: F2 0F 10 C8? Use movapd variant: 66 0F 28 C8? Simpler: movlhps can't be used. We'll use movsd with reg-reg: F2 0F 10 C8+ (reg=dest<<3|src)
                 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x10);
                /* modrm: dest=1 (xmm1), src=0 (xmm0) -> 0xC8 */
                zero_cc_emit_u8(&c->text_section, 0xC8);
            } else {
                /* cvtsi2sd xmm1, rax -> F2 0F 2A C8 (modrm dest=1 src=0) */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x2A);
                zero_cc_emit_u8(&c->text_section, 0xC8);
            }

            /* perform operation on xmm0 (LHS) and xmm1 (RHS), result in xmm0 */
            if (op == TK_PLUS) {
                /* addsd xmm0, xmm1 -> F2 0F 58 C1? modrm reg=0 dest=0? For xmm0,xmm1: reg=0 dest=0 src=1 -> 0xC1? We'll use reg=0, rm=1 -> 0xC1 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x58); zero_cc_emit_u8(&c->text_section, 0xC1);
            } else if (op == TK_MINUS) {
                /* subsd xmm0, xmm1 -> F2 0F 5C C1 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x5C); zero_cc_emit_u8(&c->text_section, 0xC1);
            } else if (op == TK_STAR) {
                /* mulsd xmm0, xmm1 -> F2 0F 59 C1 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x59); zero_cc_emit_u8(&c->text_section, 0xC1);
            } else if (op == TK_SLASH) {
                /* divsd xmm0, xmm1 -> F2 0F 5E C1 */
                zero_cc_emit_u8(&c->text_section, 0xF2); zero_cc_emit_u8(&c->text_section, 0x0F); zero_cc_emit_u8(&c->text_section, 0x5E); zero_cc_emit_u8(&c->text_section, 0xC1);
            }

            /* cleanup stack slot used for LHS */
            if (lhs_is_float) {
                zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x83); zero_cc_emit_u8(&c->text_section, 0xC4); zero_cc_emit_u8(&c->text_section, 0x08);
            } else {
                /* pop original LHS int placeholder -> adjust rsp by 8 (pop into rax) and then discard; we'll use add rsp,8 */
                zero_cc_emit_u8(&c->text_section, 0x48); zero_cc_emit_u8(&c->text_section, 0x83); zero_cc_emit_u8(&c->text_section, 0xC4); zero_cc_emit_u8(&c->text_section, 0x08);
            }

            c->last_expr_is_float = 1;
            continue;
        }

        /* Non-floating binary ops (existing integer/path) */
        /* mov rcx, rax (RHS in rcx) */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x89);
        zero_cc_emit_u8(&c->text_section, 0xC1);
