#ifndef ZERO_CC_EXPRESSIONS_TERNARY_H
#define ZERO_CC_EXPRESSIONS_TERNARY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations */
static inline void zero_cc_parse_expr(ZeroCCCompiler *c);
static inline int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c);
static inline ZeroCCType zero_cc_parse_type_name(ZeroCCCompiler *c);
static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name);

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
        zero_cc_emit_u8(&c->text_section, 0x58); /* pop rax (LHS in rax) */

        if (op == TK_PLUS) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x01);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_MINUS) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x29);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_STAR) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xAF);
            zero_cc_emit_u8(&c->text_section, 0xC1);
        } else if (op == TK_SLASH) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x99);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xF7);
            zero_cc_emit_u8(&c->text_section, 0xF9);
        } else if (op == TK_PERCENT) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x99);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xF7);
            zero_cc_emit_u8(&c->text_section, 0xF9);
            /* result in rdx -> mov rax, rdx */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0xD0);
        } else if (op == TK_AMP) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x21);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_PIPE) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x09);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_CARET) {
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x31);
            zero_cc_emit_u8(&c->text_section, 0xC8);
        } else if (op == TK_SHL) {
            /* shl rax, cl */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xD3);
            zero_cc_emit_u8(&c->text_section, 0xE0);
        } else if (op == TK_SHR) {
            /* shr rax, cl */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xD3);
            zero_cc_emit_u8(&c->text_section, 0xE8);
        } else {
            /* Comparisons: rax is LHS, rcx is RHS. cmp rax, rcx computes (rax - rcx) */
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x39);
            zero_cc_emit_u8(&c->text_section, 0xC8);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            if (op == TK_EQ) zero_cc_emit_u8(&c->text_section, 0x94);
            else if (op == TK_NE) zero_cc_emit_u8(&c->text_section, 0x95);
            else if (op == TK_LT) zero_cc_emit_u8(&c->text_section, 0x9C);
            else if (op == TK_LE) zero_cc_emit_u8(&c->text_section, 0x9E);
            else if (op == TK_GT) zero_cc_emit_u8(&c->text_section, 0x9F);
            else if (op == TK_GE) zero_cc_emit_u8(&c->text_section, 0x9D);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xB6);
            zero_cc_emit_u8(&c->text_section, 0xC0);
        }
    }

    /* Ternary Conditional Operator: cond ? then_val : else_val */
    if (c->tokens[c->token_pos].type == TK_QUESTION) {
        c->token_pos++;
        /* test cond in rax: test rax, rax; jz else_branch */
        zero_cc_emit_u8(&c->text_section, 0x48);
        zero_cc_emit_u8(&c->text_section, 0x85);
        zero_cc_emit_u8(&c->text_section, 0xC0);
        zero_cc_emit_u8(&c->text_section, 0x0F);
        zero_cc_emit_u8(&c->text_section, 0x84);
        size_t jz_pos = c->text_section.length;
        zero_cc_emit_u32(&c->text_section, 0);

        zero_cc_parse_expr(c); /* then_val in rax */

        /* jmp end_pos */
        zero_cc_emit_u8(&c->text_section, 0xE9);
        size_t jmp_pos = c->text_section.length;
        zero_cc_emit_u32(&c->text_section, 0);

        int32_t else_rel = (int32_t)(c->text_section.length - (jz_pos + 4));
        memcpy(c->text_section.data + jz_pos, &else_rel, 4);

        if (c->tokens[c->token_pos].type == TK_COLON) c->token_pos++;
        zero_cc_parse_expr(c); /* else_val in rax */

        int32_t end_rel = (int32_t)(c->text_section.length - (jmp_pos + 4));
        memcpy(c->text_section.data + jmp_pos, &end_rel, 4);
    }
}

/* Recursive descent parser for constant expression */

#endif /* ZERO_CC_EXPRESSIONS_TERNARY_H */
