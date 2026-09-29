/* Statement Parser - Modular Headers */
#ifndef ZERO_CC_STATEMENTS_H
#define ZERO_CC_STATEMENTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types_lexer.h"
#include "zero_cc/parser.h"
#include "zero_cc/expressions.h"
#include "zero_cc/types/type_system.h"
#include "zero_cc/types/symbols.h"

/* Goto/Label support */
#define MAX_LABELS 256
#define MAX_LABEL_NAME 64
static struct {
    char name[MAX_LABEL_NAME];
    size_t position;
} label_table[MAX_LABELS];
static int label_count = 0;
static size_t* goto_patch_list[MAX_LABELS];
static int goto_patch_list_count = 0;
static int block_depth = 0;

/* Reset label/goto state at function entry (block_depth == 0) */
static void reset_label_state(void) {
    label_count = 0;
    goto_patch_list_count = 0;
}

/* Find label by name, return index or -1 if not found */
static int find_label(const char *name) {
    for (int i = 0; i < label_count; i++) {
        if (strcmp(label_table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Add label to table, return index */
static int add_label(const char *name, size_t position) {
    if (label_count >= MAX_LABELS) return -1;
    strncpy(label_table[label_count].name, name, MAX_LABEL_NAME - 1);
    label_table[label_count].name[MAX_LABEL_NAME - 1] = '\0';
    label_table[label_count].position = position;
    return label_count++;
}

/* Forward declarations - needed by submodules */
static inline void zero_cc_parse_stmt(ZeroCCCompiler *c);
static inline void zero_cc_stmt_decl(ZeroCCCompiler *c, ZeroCCTokType t, size_t decl_align_spec);
static inline void zero_cc_stmt_var_incdec(ZeroCCCompiler *c, char *var_name, bool is_inc);
static inline void zero_cc_stmt_var_assign(ZeroCCCompiler *c, char *var_name, ZeroCCTokType aop);
static inline void zero_cc_stmt_member_assign(ZeroCCCompiler *c, char *ptr_name, bool is_arrow, char *mem_name);
static inline void zero_cc_stmt_ptr_deref_assign(ZeroCCCompiler *c, char *var_name);
static inline void zero_cc_stmt_var_array_assign(ZeroCCCompiler *c, char *var_name);
static inline void zero_cc_stmt_member_array_assign(ZeroCCCompiler *c, char *base_name, bool is_arrow, char *mem_name);
static inline void zero_cc_stmt_member_incdec(ZeroCCCompiler *c, char *ptr_name, char *mem_name, bool is_inc);

/* Modular statement handlers - order matters for forward declarations */
#include "statements/control.h"
#include "statements/definitions.h"
#include "statements/variables.h"

/* Helper: Check if token is a type specifier */
static inline bool is_type_specifier_token(ZeroCCTokType t) {
    return (t == TK_IDENT || t == TK_INT || t == TK_CHAR || t == TK_VOID ||
            t == TK_STRUCT || t == TK_FLOAT_KW || t == TK_DOUBLE_KW || t == TK_BOOL_KW ||
            t == TK_SIGNED || t == TK_UNSIGNED || t == TK_SHORT || t == TK_LONG ||
            t == TK_AUTO || t == TK_ATOMIC);
}

/* Helper: Check if token is an assignment operator */
static inline bool is_assignment_op(ZeroCCTokType t) {
    return (t == TK_ASSIGN || t == TK_ADD_ASSIGN || t == TK_SUB_ASSIGN ||
            t == TK_MUL_ASSIGN || t == TK_DIV_ASSIGN || t == TK_MOD_ASSIGN ||
            t == TK_AND_ASSIGN || t == TK_OR_ASSIGN || t == TK_XOR_ASSIGN ||
            t == TK_SHL_ASSIGN || t == TK_SHR_ASSIGN);
}

/* Helper: Check if identifier is assembly keyword */
static inline bool is_asm_keyword(const char *str) {
    return (strcmp(str, "__asm__") == 0 ||
            strcmp(str, "asm") == 0 ||
            strcmp(str, "__asm") == 0);
}

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

    /* Main dispatch using switch for single-token cases, if/else for compound */
    switch (t) {
        /* Compound Block: { ... } */
        case TK_LBRACE:
            zero_cc_stmt_block(c);
            return;

        /* Return Statement */
        case TK_RETURN:
            zero_cc_stmt_return(c);
            return;

        /* If Statement */
        case TK_IF:
            zero_cc_stmt_if(c);
            return;

        /* Static Assert */
        case TK_STATIC_ASSERT:
            zero_cc_stmt_static_assert(c);
            return;

        /* Switch Statement */
        case TK_SWITCH:
            zero_cc_stmt_switch(c);
            return;

        /* While Loop */
        case TK_WHILE:
            zero_cc_stmt_while(c);
            return;

        /* Do-While Loop */
        case TK_DO:
            zero_cc_stmt_do_while(c);
            return;

        /* For Loop */
        case TK_FOR:
            zero_cc_stmt_for(c);
            return;

        /* Break Statement */
        case TK_BREAK:
            zero_cc_stmt_break(c);
            return;

        /* Continue Statement */
        case TK_CONTINUE:
            zero_cc_stmt_continue(c);
            return;

        /* Goto Statement */
        case TK_GOTO: {
            c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                const char *label_name = c->tokens[c->token_pos].str_val;
                c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                /* Emit jump to label */
                int idx = find_label(label_name);
                zero_cc_emit_u8(&c->text_section, 0xE9); /* JMP rel32 */
                size_t patch = c->text_section.length;
                if (idx >= 0) {
                    /* Label already seen, patch immediately */
                    size_t rel = label_table[idx].position - (patch + 4);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)rel);
                } else {
                    /* Label not seen yet, record for later patching */
                    zero_cc_emit_u32(&c->text_section, 0); /* Placeholder */
                    size_t *patch_list = (size_t*)malloc(sizeof(size_t));
                    patch_list[0] = patch;
                    goto_patch_list[goto_patch_list_count++] = patch_list;
                }
            }
            return;
        }

        /* Typedef: check next token for struct/enum vs scalar */
        case TK_TYPEDEF:
            if (c->tokens[c->token_pos + 1].type == TK_STRUCT) {
                zero_cc_stmt_typedef_struct(c);
            } else if (c->tokens[c->token_pos + 1].type == TK_ENUM) {
                zero_cc_stmt_typedef_enum(c);
            } else {
                zero_cc_stmt_typedef_scalar(c);
            }
            return;

        /* Standalone Enum Declaration */
        case TK_ENUM:
            zero_cc_stmt_enum(c);
            return;

        /* Inline Assembly */
        case TK_IDENT:
            if (is_asm_keyword(c->tokens[c->token_pos].str_val)) {
                zero_cc_stmt_asm(c);
                return;
            }
            /* Fall through to check for declarations and compound conditions */
            break;

        /* Type specifiers: variable declaration */
        case TK_INT:
        case TK_CHAR:
        case TK_VOID:
        case TK_STRUCT:
        case TK_FLOAT_KW:
        case TK_DOUBLE_KW:
        case TK_BOOL_KW:
        case TK_SIGNED:
        case TK_UNSIGNED:
        case TK_SHORT:
        case TK_LONG:
        case TK_AUTO:
        case TK_ATOMIC:
            zero_cc_stmt_decl(c, t, decl_align_spec);
            return;

        default:
            break;
    }

    /* Compound conditions requiring lookahead (TK_IDENT cases after switch) */

    /* Label: IDENT followed by : */
    if (t == TK_IDENT && c->tokens[c->token_pos + 1].type == TK_COLON) {
        const char *label_name = c->tokens[c->token_pos].str_val;
        c->token_pos += 2; /* Skip IDENT and : */
        /* Record label position */
        int idx = find_label(label_name);
        if (idx < 0) {
            add_label(label_name, c->text_section.length);
        }
        /* Patch any pending gotos to this label */
        for (int i = 0; i < goto_patch_list_count; i++) {
            if (goto_patch_list[i] != NULL) {
                size_t patch_pos = goto_patch_list[i][0];
                size_t rel = c->text_section.length - (patch_pos + 4);
                memcpy(c->text_section.data + patch_pos, &rel, 4);
                free(goto_patch_list[i]);
                goto_patch_list[i] = NULL;
                /* Remove from list by shifting */
                for (int j = i; j < goto_patch_list_count - 1; j++) {
                    goto_patch_list[j] = goto_patch_list[j + 1];
                }
                goto_patch_list_count--;
                i--; /* Recheck this index */
            }
        }
        return;
    }

    /* Struct Member Post-Increment/Decrement: ptr->member++ or ptr->member-- */
    if (t == TK_IDENT && c->tokens[c->token_pos + 1].type == TK_ARROW &&
        c->tokens[c->token_pos + 2].type == TK_IDENT &&
        (c->tokens[c->token_pos + 3].type == TK_INC || c->tokens[c->token_pos + 3].type == TK_DEC)) {
        char ptr_name[128], mem_name[128];
        strncpy(ptr_name, c->tokens[c->token_pos].str_val, 127);
        ptr_name[127] = '\0';
        strncpy(mem_name, c->tokens[c->token_pos + 2].str_val, 127);
        mem_name[127] = '\0';
        bool is_inc = (c->tokens[c->token_pos + 3].type == TK_INC);
        zero_cc_stmt_member_incdec(c, ptr_name, mem_name, is_inc);
        return;
    }

    /* Variable Post-Increment/Decrement: x++ or x-- */
    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_INC || c->tokens[c->token_pos + 1].type == TK_DEC)) {
        char var_name[128];
        strncpy(var_name, c->tokens[c->token_pos].str_val, 127);
        var_name[127] = '\0';
        bool is_inc = (c->tokens[c->token_pos + 1].type == TK_INC);
        zero_cc_stmt_var_incdec(c, var_name, is_inc);
        return;
    }

    /* Struct Member Array Assignment: sym.member[idx] = ... or ptr->member[idx] = ... */
    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_DOT || c->tokens[c->token_pos + 1].type == TK_ARROW) &&
        c->tokens[c->token_pos + 2].type == TK_IDENT && c->tokens[c->token_pos + 3].type == TK_LBRACKET) {
        char base_name[128], mem_name[128];
        strncpy(base_name, c->tokens[c->token_pos].str_val, 127);
        base_name[127] = '\0';
        bool is_arrow = (c->tokens[c->token_pos + 1].type == TK_ARROW);
        strncpy(mem_name, c->tokens[c->token_pos + 2].str_val, 127);
        mem_name[127] = '\0';
        zero_cc_stmt_member_array_assign(c, base_name, is_arrow, mem_name);
        return;
    }

    /* Local Variable Array Assignment: sym[idx] = ... */
    if (t == TK_IDENT && c->tokens[c->token_pos + 1].type == TK_LBRACKET) {
        char var_name[128];
        strncpy(var_name, c->tokens[c->token_pos].str_val, 127);
        var_name[127] = '\0';
        zero_cc_stmt_var_array_assign(c, var_name);
        return;
    }

    /* Assignment to Struct Member: sym.member = ... or ptr->member = ... */
    if (t == TK_IDENT && (c->tokens[c->token_pos + 1].type == TK_ARROW || c->tokens[c->token_pos + 1].type == TK_DOT)) {
        char ptr_name[128], mem_name[128];
        strncpy(ptr_name, c->tokens[c->token_pos].str_val, 127);
        ptr_name[127] = '\0';
        bool is_arrow = (c->tokens[c->token_pos + 1].type == TK_ARROW);
        strncpy(mem_name, c->tokens[c->token_pos + 2].str_val, 127);
        mem_name[127] = '\0';
        zero_cc_stmt_member_assign(c, ptr_name, is_arrow, mem_name);
        return;
    }

    /* Variable Declaration (TK_IDENT as type name) */
    if (t == TK_IDENT && is_type_specifier_token(c->tokens[c->token_pos].type)) {
        zero_cc_stmt_decl(c, t, decl_align_spec);
        return;
    }

    /* Pointer Dereference Assignment: *p = ... */
    if (t == TK_STAR && c->tokens[c->token_pos + 1].type == TK_IDENT && c->tokens[c->token_pos + 2].type == TK_ASSIGN) {
        char var_name[128];
        strncpy(var_name, c->tokens[c->token_pos + 1].str_val, 127);
        var_name[127] = '\0';
        zero_cc_stmt_ptr_deref_assign(c, var_name);
        return;
    }

    /* Variable Assignment: x = ... or compound assignment */
    if (t == TK_IDENT && is_assignment_op(c->tokens[c->token_pos + 1].type)) {
        char var_name[128];
        strncpy(var_name, c->tokens[c->token_pos].str_val, 127);
        var_name[127] = '\0';
        ZeroCCTokType aop = c->tokens[c->token_pos + 1].type;
        zero_cc_stmt_var_assign(c, var_name, aop);
        return;
    }

    /* General Expression Statement */
    zero_cc_parse_expr(c);
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

#endif /* ZERO_CC_STATEMENTS_H */
