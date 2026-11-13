#ifndef ZERO_CC_PARSER_DECL_ENUM_H
#define ZERO_CC_PARSER_DECL_ENUM_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/const_eval.h"

void skip_c23_attributes(ZeroCCCompiler *c);
void skip_c23_attributes_at(ZeroCCCompiler *c, size_t *pos);

/* Parse one enum body `c->token_pos` at the first member (LBRACE consumed);
 * consumes through RBRACE. M1: two fixes vs the old per-site loops —
 *   (1) `false`/`true` are accepted as member names: in C they are plain
 *       identifiers unless <stdbool.h> is included (TK_FALSE/TK_TRUE tokens
 *       carried str_val; the old TK_IDENT-only check stalled the loop on
 *       `enum { false, true }` — compile/pr94238.c); and
 *   (2) a progress guard: any token that neither member nor comma parsing
 *       consumes is diagnosed and skipped, so a malformed body
 *       (e.g. `enum { 1, 2 }`) can never spin the parser forever.
 * `und` (C23 fixed underlying type, NULL = traditional) wraps each member
 * value into the fixed width. */
static inline void zero_cc_parse_enum_members(ZeroCCCompiler *c,
                                              const ZCEnumUnderlying *und) {
    int64_t enum_val = 0;
    while (c->token_pos < c->token_count &&
           c->tokens[c->token_pos].type != TK_RBRACE &&
           c->tokens[c->token_pos].type != TK_EOF) {
        skip_c23_attributes(c);
        size_t saved = c->token_pos;
        if (c->tokens[c->token_pos].type == TK_IDENT ||
            (c->tokens[c->token_pos].type == TK_FALSE &&
             c->std_dialect != ZC_STD_C23 && c->std_dialect != ZC_STD_GNU23) ||
            (c->tokens[c->token_pos].type == TK_TRUE &&
             c->std_dialect != ZC_STD_C23 && c->std_dialect != ZC_STD_GNU23)) {
            char ename[128];
            snprintf(ename, sizeof(ename), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            skip_c23_attributes(c);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                c->token_pos++;
                enum_val = zero_cc_eval_const_expr(c);
            }
            skip_c23_attributes(c);
            enum_val = zc_enum_wrap_value(enum_val, und);
            zero_cc_register_enum_const(c, ename, enum_val);
            if (enum_val == INT64_MAX) {
                diag_report_error_at_token(c, "overflow in enumeration values");
            } else {
                enum_val++;
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        if (c->token_pos == saved) {
            diag_report_error_at_token(c, "expected identifier in enum definition");
            c->token_pos++;
        }
    }
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
}


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
            snprintf(et.struct_name, sizeof(et.struct_name), "%s", en_name);
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
        snprintf(et.struct_name, sizeof(et.struct_name), "%s", en_name);
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
            /* Register as a variable of the enum type */
            size_t var_offset = c->data_section.length;
            /* Emit zero-initialized data for the enum variable (4 bytes) */
            for (size_t i = 0; i < et.size; i++) {
                zero_cc_emit_u8(&c->data_section, 0);
            }
            if (add_global_var(c, var_name, et, var_offset, et.size, false, false) < 0) {
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
