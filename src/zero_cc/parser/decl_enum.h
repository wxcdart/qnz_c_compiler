#ifndef ZERO_CC_PARSER_DECL_ENUM_H
#define ZERO_CC_PARSER_DECL_ENUM_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/const_eval.h"

void skip_c23_attributes_at(ZeroCCCompiler *c, size_t *pos);

/* Typedef Enum Definition */
static inline void zero_cc_stmt_typedef_enum(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_TYPEDEF */
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_CONST ||
            c->tokens[c->token_pos].type == TK_VOLATILE ||
            c->tokens[c->token_pos].type == TK_RESTRICT)) {
        c->token_pos++;
    }
    if (c->tokens[c->token_pos].type == TK_ENUM) {
        c->token_pos++;
        char en_name[128] = "";
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            snprintf(en_name, sizeof(en_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
        }
        /* C23: `typedef enum e : <type> { ... }` / `typedef enum : <type> {...}` */
        ZCEnumUnderlying und = zero_cc_parse_enum_underlying(c);
        if (c->tokens[c->token_pos].type == TK_LBRACE) {
            c->token_pos++;
            ZeroCCType et; memset(&et, 0, sizeof(et));
            et.tag = TYPE_ENUM;
            et.size = 4;
            et.align = 4;
            if (en_name[0] != '\0') {
                snprintf(et.struct_name, sizeof(et.struct_name), "%s", en_name);
            } else {
                snprintf(et.struct_name, sizeof(et.struct_name), "__anon_enum_%zu", c->anon_struct_seq++);
            }
            zc_enum_apply_underlying(&et, &und);
            zero_cc_parse_enum_members(c, &und);
            if (en_name[0] != '\0') {
                zero_cc_register_enum(c, en_name, et);
            }
            /* GNU mode(X) after the enum body: consume the marker (the width
             * is not applied to enums; before marker emission the attribute
             * was stripped, so skipping preserves the old behavior). */
            zero_cc_skip_mode_attr(c);
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char alias[128];
                snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                zero_cc_register_typedef(c, alias, et);
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        } else if (en_name[0] != '\0') {
            ZeroCCType *existing = zero_cc_find_enum(c, en_name);
            ZeroCCType et = existing ? *existing : zero_cc_type_enum(en_name);
            /* C23 redeclaration: `typedef enum e : <type> E;` - the
             * underlying type must match the definition (if one exists). */
            if (und.size && existing && existing->size != und.size) {
                diag_report_error_at_token(c, "conflicting underlying type for enum");
            }
            zc_enum_apply_underlying(&et, &und);
            zero_cc_register_enum(c, en_name, et);
            zero_cc_skip_mode_attr(c);
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char alias[128];
                snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                zero_cc_register_typedef(c, alias, et);
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        }
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Standalone Enum Declaration */
static inline void zero_cc_stmt_enum(ZeroCCCompiler *c) {
    c->token_pos++; /* consume TK_ENUM */
    skip_c23_attributes(c);
    char en_name[128] = "";
    if (c->tokens[c->token_pos].type == TK_IDENT) {
        snprintf(en_name, sizeof(en_name), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos++;
    }
    skip_c23_attributes(c);
    /* C23: `enum e : <type> { ... }` / `enum e : <type> x;` */
    ZCEnumUnderlying und = zero_cc_parse_enum_underlying(c);
    if (c->tokens[c->token_pos].type == TK_LBRACE) {
        c->token_pos++;
        ZeroCCType et; memset(&et, 0, sizeof(et));
        et.tag = TYPE_ENUM;
        et.size = 4;
        et.align = 4;
        if (en_name[0] != '\0') {
            snprintf(et.struct_name, sizeof(et.struct_name), "%s", en_name);
        } else {
            snprintf(et.struct_name, sizeof(et.struct_name), "__anon_enum_%zu", c->anon_struct_seq++);
        }
        zc_enum_apply_underlying(&et, &und);
        zero_cc_parse_enum_members(c, &und);
        if (en_name[0] != '\0') {
            zero_cc_register_enum(c, en_name, et);
        }
        /* After enum body, handle variable declarators (not typedef - typedef
         * case is handled by zero_cc_stmt_typedef_enum). */
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
            char var_name[128];
            snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            bool has_init = false;
            int64_t init_val = 0;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                c->token_pos++;
                init_val = zero_cc_eval_const_expr(c);
                has_init = true;
            }
            /* Register as a variable of the enum type */
            size_t var_offset = c->data_section.length;
            /* Emit data for the enum variable */
            for (size_t i = 0; i < et.size; i++) {
                zero_cc_emit_u8(&c->data_section, (uint8_t)((uint64_t)init_val >> (8 * i)));
            }
            if (add_global_var(c, var_name, et, var_offset, et.size, false, has_init) < 0) {
                diag_report_error_at_token(c, "out of memory registering global variable");
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
                c->token_pos++;
            } else {
                break;
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    } else if (en_name[0] != '\0') {
        /* C23 redeclaration: `enum e : <type> x;` */
        ZeroCCType *existing = zero_cc_find_enum(c, en_name);
        if (!existing) {
            ZeroCCType et = zero_cc_type_enum(en_name);
            zc_enum_apply_underlying(&et, &und);
            zero_cc_register_enum(c, en_name, et);
        } else if (und.size && existing->size != und.size) {
            diag_report_error_at_token(c, "conflicting underlying type for enum");
        }
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}


#endif /* ZERO_CC_PARSER_DECL_ENUM_H */
