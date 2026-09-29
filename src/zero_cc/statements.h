/* Statement Parser - Modular Headers */
#ifndef ZERO_CC_STATEMENTS_H
#define ZERO_CC_STATEMENTS_H

#include "statements/control.h"
#include "statements/definitions.h"
#include "statements/variables.h"

/* Main statement parser - dispatches to modular handlers */
static inline void zero_cc_parse_stmt(ZeroCCCompiler *c) {
    size_t decl_align_spec = 0;

    /* Skip C23 attributes: [[ ... ]] */
    while (c->tokens[c->token_pos].type == TK_LDBRACKET) {
        c->token_pos++;
        int depth = 1;
        while (c->tokens[c->token_pos].type != TK_EOF && depth > 0) {
            if (c->tokens[c->token_pos].type == TK_LDBRACKET) depth++;
            else if (c->tokens[c->token_pos].type == TK_RDBRACKET) depth--;
            c->token_pos++;
        }
    }

    ZeroCCTokType t = c->tokens[c->token_pos].type;

    /* Compound Block: { ... } */
    if (t == TK_LBRACE) {
        zero_cc_stmt_block(c);
        return;
    }

    /* Inline Assembly: __asm__ (...) or __asm (...) or asm (...) */
    if (t == TK_IDENT && (strcmp(c->tokens[c->token_pos].str_val, "__asm__") == 0 ||
                          strcmp(c->tokens[c->token_pos].str_val, "asm") == 0 ||
                          strcmp(c->tokens[c->token_pos].str_val, "__asm") == 0)) {
        zero_cc_stmt_asm(c);
        return;
    }

    /* Typedef Struct Definition */
    if (t == TK_TYPEDEF && c->tokens[c->token_pos + 1].type == TK_STRUCT) {
        zero_cc_stmt_typedef_struct(c);
        return;
    }

    /* Typedef Enum Definition */
    if (t == TK_TYPEDEF && c->tokens[c->token_pos + 1].type == TK_ENUM) {
        zero_cc_stmt_typedef_enum(c);
        return;
    }

    /* Scalar Typedef */
    if (t == TK_TYPEDEF) {
        zero_cc_stmt_typedef_scalar(c);
        return;
    }

    /* Standalone Enum Declaration */
    if (t == TK_ENUM) {
        zero_cc_stmt_enum(c);
        return;
    }

    /* Handle leading _Alignas specifiers */
    while (t == TK_ALIGNAS) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
            int64_t align_val = zero_cc_eval_const_expr(c);
            if (align_val > 0 && (decl_align_spec == 0 || (size_t)align_val > decl_align_spec)) {
                decl_align_spec = (size_t)align_val;
            }
            if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        }
        t = c->tokens[c->token_pos].type;
    }

    /* Skip storage/constness specifiers */
    while (t == TK_CONSTEXPR || t == TK_CONST || t == TK_STATIC || t == TK_EXTERN || t == TK_REGISTER) {
        c->token_pos++;
        t = c->tokens[c->token_pos].type;
    }

    /* Variable Declaration */
    if (t == TK_IDENT || t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT ||
        t == TK_FLOAT_KW || t == TK_DOUBLE_KW || t == TK_BOOL_KW ||
        t == TK_SIGNED || t == TK_UNSIGNED || t == TK_SHORT || t == TK_LONG || t == TK_AUTO || t == TK_ATOMIC) {
        zero_cc_stmt_decl(c, t, decl_align_spec);
        return;
    }

    /* Return Statement */
    if (t == TK_RETURN) {
        zero_cc_stmt_return(c);
        return;
    }

    /* If Statement */
    if (t == TK_IF) {
        zero_cc_stmt_if(c);
        return;
    }

    /* Static Assert */
    if (t == TK_STATIC_ASSERT) {
        zero_cc_stmt_static_assert(c);
        return;
    }

    /* Switch Statement */
    if (t == TK_SWITCH) {
        zero_cc_stmt_switch(c);
        return;
    }

    /* While Loop */
    if (t == TK_WHILE) {
        zero_cc_stmt_while(c);
        return;
    }

    /* Do-While Loop */
    if (t == TK_DO) {
        zero_cc_stmt_do_while(c);
        return;
    }

    /* For Loop */
    if (t == TK_FOR) {
        zero_cc_stmt_for(c);
        return;
    }

    /* Struct Member Post-Increment/Decrement */
    if (t == TK_IDENT && c->tokens[c->token_pos + 1].type == TK_ARROW &&
        c->tokens[c->token_pos + 2].type == TK_IDENT &&
        (c->tokens[c->token_pos + 3].type == TK_INC || c->tokens[c->token_pos + 3].type == TK_DEC)) {
        char ptr_name[128], mem_name[128];
        snprintf(ptr_name, sizeof(ptr_name), "%s", c->tokens[c->token_pos].str_val);
        snprintf(mem_name, sizeof(mem_name), "%s", c->tokens[c->token_pos + 2].str_val);
        bool is_inc = (c->tokens[c->token_pos + 3].type == TK_INC);
        zero_cc_stmt_member_incdec(c, ptr_name, mem_name, is_inc);
        return;
    }

    /* Variable Post-Increment/Decrement */
    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_INC || c->tokens[c->token_pos + 1].type == TK_DEC)) {
        char var_name[128];
        snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
        bool is_inc = (c->tokens[c->token_pos + 1].type == TK_INC);
        zero_cc_stmt_var_incdec(c, var_name, is_inc);
        return;
    }

    /* Struct Member Array Assignment */
    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_DOT || c->tokens[c->token_pos + 1].type == TK_ARROW) &&
        c->tokens[c->token_pos + 2].type == TK_IDENT && c->tokens[c->token_pos + 3].type == TK_LBRACKET) {
        char base_name[128], mem_name[128];
        snprintf(base_name, sizeof(base_name), "%s", c->tokens[c->token_pos].str_val);
        bool is_arrow = (c->tokens[c->token_pos + 1].type == TK_ARROW);
        snprintf(mem_name, sizeof(mem_name), "%s", c->tokens[c->token_pos + 2].str_val);
        zero_cc_stmt_member_array_assign(c, base_name, is_arrow, mem_name);
        return;
    }

    /* Local Variable Array Assignment */
    if (t == TK_IDENT && c->tokens[c->token_pos + 1].type == TK_LBRACKET) {
        char var_name[128];
        snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
        zero_cc_stmt_var_array_assign(c, var_name);
        return;
    }

    /* Assignment to Struct Member */
    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_ARROW || c->tokens[c->token_pos + 1].type == TK_DOT)) {
        char ptr_name[128], mem_name[128];
        snprintf(ptr_name, sizeof(ptr_name), "%s", c->tokens[c->token_pos].str_val);
        bool is_arrow = (c->tokens[c->token_pos + 1].type == TK_ARROW);
        snprintf(mem_name, sizeof(mem_name), "%s", c->tokens[c->token_pos + 2].str_val);
        zero_cc_stmt_member_assign(c, ptr_name, is_arrow, mem_name);
        return;
    }

    /* Pointer Dereference Assignment */
    if (t == TK_STAR && c->tokens[c->token_pos + 1].type == TK_IDENT && c->tokens[c->token_pos + 2].type == TK_ASSIGN) {
        char var_name[128];
        snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos + 1].str_val);
        zero_cc_stmt_ptr_deref_assign(c, var_name);
        return;
    }

    /* Variable Assignment */
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
        zero_cc_stmt_var_assign(c, var_name, aop);
        return;
    }

    /* General Expression Statement */
    zero_cc_parse_expr(c);
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

#endif /* ZERO_CC_STATEMENTS_H */
