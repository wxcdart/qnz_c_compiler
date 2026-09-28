#ifndef ZERO_CC_STATEMENTS_CONTROL_H
#define ZERO_CC_STATEMENTS_CONTROL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static inline void zero_cc_parse_stmt(ZeroCCCompiler *c) {

    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_ADD_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_SUB_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_MUL_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_DIV_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_MOD_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_AND_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_OR_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_XOR_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_SHL_ASSIGN ||
                          c->tokens[c->token_pos + 1].type == TK_SHR_ASSIGN)) {
        char var_name[128];
        snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
        ZeroCCTokType aop = c->tokens[c->token_pos + 1].type;
        c->token_pos += 2;
        zero_cc_parse_expr(c);

        ZeroCCSym *sym = zero_cc_find_local(c, var_name);
        if (sym) {
            if (aop == TK_ADD_ASSIGN) {
                /* add [rbp - offset], rax */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x01);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            } else if (aop == TK_SUB_ASSIGN) {
                /* sub [rbp - offset], rax */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x29);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            } else if (aop == TK_AND_ASSIGN) {
                /* and [rbp - offset], rax */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x21);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            } else if (aop == TK_OR_ASSIGN) {
                /* or [rbp - offset], rax */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x09);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            } else if (aop == TK_XOR_ASSIGN) {
                /* xor [rbp - offset], rax */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x31);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            } else if (aop == TK_MUL_ASSIGN) {
                /* imul rax, [rbp - offset]; mov [rbp - offset], rax */
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
                /* mov rcx, rax (divisor); mov rax, [rbp - offset]; cqo; idiv rcx */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x89);
                zero_cc_emit_u8(&c->text_section, 0xC1);
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x99); /* cqo */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0xF7);
                zero_cc_emit_u8(&c->text_section, 0xF9); /* idiv rcx */
                /* quotient in rax, remainder in rdx */
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
                /* mov rcx, rax (shift count); mov rax, [rbp - offset]; shl/shr rax, cl */
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
                /* mov [rbp - offset], rax */
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x89);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                if (sym->type.tag == TYPE_STRUCT && sym->type.size > 8) {
                    /* mov [rbp - (sym_offset - 8)], rdx */
                    int32_t off2 = (int32_t)sym->stack_offset - 8;
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0x95);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-off2));
                }
            }
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    /* General Expression Statement */
    zero_cc_parse_expr(c);
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

#endif /* ZERO_CC_STATEMENTS_H */
}

#endif /* ZERO_CC_STATEMENTS_CONTROL_H */
