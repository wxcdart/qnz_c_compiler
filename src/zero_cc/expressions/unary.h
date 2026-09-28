#ifndef ZERO_CC_EXPRESSIONS_UNARY_H
#define ZERO_CC_EXPRESSIONS_UNARY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations */
static inline void zero_cc_parse_expr(ZeroCCCompiler *c);
static inline void zero_cc_parse_stmt(ZeroCCCompiler *c);
static inline int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c);
static inline ZeroCCType zero_cc_parse_type_name(ZeroCCCompiler *c);
static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name);

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
        size_t pos = c->token_pos;
        pos++; /* consume TK_SIZEOF */
        if (c->tokens[pos].type == TK_LPAREN) pos++;
        ZeroCCType ty = zero_cc_type_of_equality(c, &pos);
        if (c->tokens[pos].type == TK_RPAREN) pos++;
        c->token_pos = pos;
        size_t sz = ty.size;

        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0xB8);
        zero_cc_emit_u64(&c->text_section, (uint64_t)sz);
        return;
    }
    if (t == TK_ALIGNOF) {
        size_t pos = c->token_pos;
        pos++; /* consume TK_ALIGNOF */
        if (c->tokens[pos].type == TK_LPAREN) pos++;
        ZeroCCType ty = zero_cc_type_of_equality(c, &pos);
        if (c->tokens[pos].type == TK_RPAREN) pos++;
        c->token_pos = pos;
        size_t align = ty.align ? ty.align : 1;

        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0xB8);
        zero_cc_emit_u64(&c->text_section, (uint64_t)align);
        return;
    }

    zero_cc_parse_primary(c);
}

/* Binary Arithmetic & Relational Expression Parser */

#endif /* ZERO_CC_EXPRESSIONS_UNARY_H */
