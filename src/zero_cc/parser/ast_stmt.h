#ifndef ZERO_CC_PARSER_AST_STMT_H
#define ZERO_CC_PARSER_AST_STMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/jit/type_info.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/parser/definitions.h"

static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_assignment_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_brace_init_list_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCType lit_type, size_t *out_elem_count);

/* =========================================================================
 * Sprint 37: static locals (block scope, static storage duration)
 *
 * The object is allocated in the data section and registered in the
 * global table under a mangled name. The declaration's local symbol
 * keeps a normal stack slot which acts as a per-call MIRROR: the
 * lowering prologue copies data -> slot and the (single, shared)
 * epilogue copies slot -> data, so all access sites stay
 * frame-relative and the value persists across calls.
 * ========================================================================= */
static inline bool zero_cc_static_local_register(ZeroCCCompiler *c, ZeroCCSym *sym) {
    ZeroCCType t = sym->type;
    size_t size = t.size ? t.size : 8;
    size_t alloc = (size + 7) & ~(size_t)7;   /* mirror copies in 8-byte chunks */
    size_t align = t.align > 8 ? (size_t)t.align : 8;
    while (c->data_section.length % align) zero_cc_emit_u8(&c->data_section, 0);
    size_t off = c->data_section.length;
    for (size_t i = 0; i < alloc; i++) zero_cc_emit_u8(&c->data_section, 0);
    char mangled[48];
    snprintf(mangled, sizeof(mangled), "__sl_%zu", c->static_local_seq++);
    if (add_global_var(mangled, t, off, alloc, true, false) < 0) {
        diag_report_error_at_token(c, "out of memory registering static local '%s'", sym->name);
        return false;
    }
    sym->is_static_local = true;
    sym->static_data_offset = off;
    snprintf(sym->static_global_name, sizeof(sym->static_global_name), "%s", mangled);
    return true;
}

/* Patch a constant-literal initializer into the data section at the
 * symbol's static offset (the zero-filled reservation made at register
 * time). Called with the token stream positioned just after '='.
 * Returns true when a supported literal initializer was consumed.
 * A non-literal initializer leaves the stream where the caller skips
 * it and reports the diagnostic (the run then fails, no binary). */
static inline bool zero_cc_static_local_patch_init(ZeroCCCompiler *c, ZeroCCSym *sym) {
    /* Empty initializer `{}` (C23): zero-fill static local storage */
    if (c->token_pos + 1 < c->token_count &&
        c->tokens[c->token_pos].type == TK_LBRACE &&
        c->tokens[c->token_pos + 1].type == TK_RBRACE) {
        c->token_pos += 2; /* consume '{' and '}' */
        size_t total = (sym->type.size > 0) ? sym->type.size : 8;
        size_t cap_end = sym->static_data_offset + total;
        if (c->data_section.length < cap_end) {
            size_t grow = cap_end - c->data_section.length;
            zero_cc_buf_ensure(&c->data_section, grow);
            memset(c->data_section.data + c->data_section.length, 0, grow);
            c->data_section.length = cap_end;
        }
        uint8_t *dst = (uint8_t *)c->data_section.data + sym->static_data_offset;
        memset(dst, 0, total);
        return true;
    }
    bool is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
    if (c->token_pos < c->token_count &&
        (c->tokens[c->token_pos].type == TK_MINUS || c->tokens[c->token_pos].type == TK_PLUS)) {
        bool neg = (c->tokens[c->token_pos].type == TK_MINUS);
        c->token_pos++;
        if (c->token_pos >= c->token_count) return false;
        ZeroCCTokType t2 = c->tokens[c->token_pos].type;
        if (is_float) {
            if (t2 != TK_FLOAT && t2 != TK_NUM) return false;
            double dv = is_float && t2 == TK_FLOAT ?
                (double)c->tokens[c->token_pos].float_val : (double)c->tokens[c->token_pos].num_val;
            c->token_pos++;
            if (neg) dv = -dv;
            uint64_t bits;
            size_t width;
            if (sym->type.tag == TYPE_FLOAT) {
                float fv = (float)dv;
                bits = 0;
                memcpy(&bits, &fv, sizeof(fv));
                width = 4;
            } else {
                memcpy(&bits, &dv, sizeof(bits));
                width = 8;
            }
            uint8_t *dst = (uint8_t *)c->data_section.data + sym->static_data_offset;
            for (size_t i = 0; i < width; i++) dst[i] = (uint8_t)(bits >> (8 * i));
            return true;
        } else {
            if (t2 != TK_NUM && t2 != TK_CHAR_LIT) return false;
            int64_t iv = (int64_t)c->tokens[c->token_pos].num_val;
            c->token_pos++;
            if (neg) iv = -iv;
            uint64_t bits = (uint64_t)iv;
            size_t width = (sym->type.size >= 8) ? 8 : ((size_t)sym->type.size > 0 ? (size_t)sym->type.size : 8);
            uint8_t *dst = (uint8_t *)c->data_section.data + sym->static_data_offset;
            for (size_t i = 0; i < width; i++) dst[i] = (uint8_t)(bits >> (8 * i));
            return true;
        }
    }
    if (c->token_pos >= c->token_count) return false;
    ZeroCCTokType t = c->tokens[c->token_pos].type;
    /* Static-local string initializer: = "string" for char[] */
    if (t == TK_STRING && sym->type.tag == TYPE_ARRAY && sym->type.base && (sym->type.base->tag == TYPE_INT8 || sym->type.base->tag == TYPE_UINT8)) {
        c->token_pos++;
        const char *s = c->tokens[c->token_pos - 1].allocated_str ? c->tokens[c->token_pos - 1].allocated_str : c->tokens[c->token_pos - 1].str_val;
        size_t len = strlen(s) + 1; /* include null terminator */
        /* Ensure data section has enough capacity */
        size_t max_write = sym->static_data_offset + len;
        if (c->data_section.length < max_write) {
            zero_cc_buf_ensure(&c->data_section, max_write - c->data_section.length);
        }
        uint8_t *dst = (uint8_t *)c->data_section.data + sym->static_data_offset;
        memcpy(dst, s, len);
        /* If array was unsized, update the type size and array_len */
        if (sym->type.array_len == 0) {
            sym->type.array_len = len;
            sym->type.size = len;
        }
        /* Update data section length */
        size_t written_end = sym->static_data_offset + len;
        if (c->data_section.length < written_end) {
            c->data_section.length = written_end;
        }
        return true;
    }
    /* Static-local array: = { elem, elem, ... } (positional). Each element
     * is either a string literal (for pointer elements the literal is stored
     * in .rodata and a data relocation patches the 8-byte slot; for char
     * elements the bytes are written directly) or a compile-time constant
     * expression (literals, shifts, casts, NULL, ...). Nested grouping
     * braces are transparent. Unsized arrays are sized from the initializer. */
    if (t == TK_LBRACE && sym->type.tag == TYPE_ARRAY) {
        c->token_pos++; /* { */
        ZeroCCType *eb = sym->type.base;
        size_t elem_sz = (eb && eb->size > 0) ? eb->size : 1;
        bool elem_is_ptr  = (eb && eb->tag == TYPE_PTR);
        bool elem_is_char = (eb && (eb->tag == TYPE_INT8 || eb->tag == TYPE_UINT8));
        size_t total = (sym->type.size > 0) ? sym->type.size : 0;
        size_t off = 0;
        /* Reserve + zero-fill data capacity for the initializer. */
        size_t reserve = total > 0 ? total : elem_sz * 64;
        size_t cap_end = sym->static_data_offset + reserve;
        if (c->data_section.length < cap_end) {
            size_t grow = cap_end - c->data_section.length;
            zero_cc_buf_ensure(&c->data_section, grow);
            memset(c->data_section.data + c->data_section.length, 0, grow);
        }
        uint8_t *dst = (uint8_t *)c->data_section.data + sym->static_data_offset;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) {
            if (c->tokens[c->token_pos].type == TK_LBRACE) {
                c->token_pos++; /* grouping brace: transparent */
                continue;
            }
            if (c->tokens[c->token_pos].type == TK_STRING) {
                const char *sv = c->tokens[c->token_pos].allocated_str ?
                    c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                c->token_pos++;
                if (elem_is_ptr) {
                    /* Pointer-to-string: emit literal to .rodata, patch the
                     * 8-byte data slot via a data relocation. */
                    size_t ro_off = c->rodata_section.length;
                    zero_cc_buf_put_bytes(&c->rodata_section, sv, strlen(sv) + 1);
                    if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                        ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                        dr->data_patch_offset = sym->static_data_offset + off;
                        dr->target_kind = 0; /* .rodata target */
                        dr->rodata_offset = ro_off;
                        dr->target_data_offset = 0;
                        dr->target_global_idx = -1;
                    }
                    off += 8;
                } else if (elem_is_char) {
                    /* Char element: write the string bytes (incl. NUL). */
                    size_t slen = strlen(sv) + 1;
                    if (total > 0 && off + slen > total) slen = (total > off) ? (total - off) : 0;
                    memcpy(dst + off, sv, slen);
                    off += slen;
                } else {
                    return false;
                }
                if (c->token_pos < c->token_count &&
                    c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
                continue;
            }
            /* Constant-expression element (literals, shifts, casts, NULL, ...) */
            size_t start = c->token_pos;
            int64_t iv = zero_cc_eval_const_expr(c);
            if (c->token_pos == start) return false; /* not a constant expression */
            /* Guard against partial consumption corrupting the token stream:
             * a complete element is terminated by ',' or '}'. */
            ZeroCCTokType after = (c->token_pos < c->token_count) ? c->tokens[c->token_pos].type : TK_EOF;
            if (after != TK_COMMA && after != TK_RBRACE) return false;
            uint64_t bits = (uint64_t)iv;
            size_t w = (elem_sz < 8) ? elem_sz : 8;
            if (total > 0 && off + w > total) w = (total > off) ? (total - off) : 0;
            for (size_t i = 0; i < w; i++) dst[off + i] = (uint8_t)(bits >> (8 * i));
            off += w;
            if (c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
        /* If array was unsized, size it from the bytes written. */
        if (sym->type.array_len == 0 && off > 0) {
            sym->type.array_len = (elem_sz > 0) ? (off / elem_sz) : off;
            if (eb && eb->size > 0) sym->type.size = off;
        }
        /* Extend the data section length to cover the written object. */
        size_t written_end = sym->static_data_offset + off;
        if (c->data_section.length < written_end) {
            c->data_section.length = written_end;
        }
        return true;
    }
    if (is_float) {
        if (t != TK_FLOAT && t != TK_NUM) return false;
        double dv = (t == TK_FLOAT) ? (double)c->tokens[c->token_pos].float_val :
            (double)c->tokens[c->token_pos].num_val;
        c->token_pos++;
        uint64_t bits;
        size_t width;
        if (sym->type.tag == TYPE_FLOAT) {
            float fv = (float)dv;
            bits = 0;
            memcpy(&bits, &fv, sizeof(fv));
            width = 4;
        } else {
            memcpy(&bits, &dv, sizeof(bits));
            width = 8;
        }
        uint8_t *dst = (uint8_t *)c->data_section.data + sym->static_data_offset;
        for (size_t i = 0; i < width; i++) dst[i] = (uint8_t)(bits >> (8 * i));
        return true;
    }
    /* Static-local scalar: accept any compile-time constant expression
     * (literals, shifts, bitwise/arith ops, enum constants, parenthesized
     * groups) and bake the value into the data section. */
    if (!is_float) {
        size_t start = c->token_pos;
        int64_t iv = zero_cc_eval_const_expr(c);
        if (c->token_pos != start) {
            /* Guard against partial consumption corrupting the token stream:
             * a complete scalar initializer is terminated by ';'. */
            ZeroCCTokType after = (c->token_pos < c->token_count) ? c->tokens[c->token_pos].type : TK_EOF;
            if (after != TK_SEMI) return false;
            uint64_t bits = (uint64_t)iv;
            size_t width = (sym->type.size >= 8) ? 8 : ((size_t)sym->type.size > 0 ? (size_t)sym->type.size : 8);
            uint8_t *dst = (uint8_t *)c->data_section.data + sym->static_data_offset;
            for (size_t i = 0; i < width; i++) dst[i] = (uint8_t)(bits >> (8 * i));
            return true;
        }
        return false;
    }
    if (t != TK_NUM && t != TK_CHAR_LIT) return false;
    int64_t iv = (int64_t)c->tokens[c->token_pos].num_val;
    c->token_pos++;
    uint64_t bits = (uint64_t)iv;
    size_t width = (sym->type.size >= 8) ? 8 : ((size_t)sym->type.size > 0 ? (size_t)sym->type.size : 8);
    uint8_t *dst = (uint8_t *)c->data_section.data + sym->static_data_offset;
    for (size_t i = 0; i < width; i++) dst[i] = (uint8_t)(bits >> (8 * i));
    return true;
}

/* Measure (WITHOUT consuming tokens) the byte size of an unsized-array
 * initializer so the array can be fully sized (stack slot + data
 * reservation) BEFORE zero_cc_add_local / zero_cc_static_local_register.
 * `brace_pos` is the token index of the initializer's first token (the
 * '{' of a braced list, or the string literal for a whole-string char
 * initializer). Returns the object's byte size, or 0 when not measurable.
 * *out_count (optional) receives the element count (0 for whole-string). */
static size_t zero_cc_measure_array_init_at(ZeroCCCompiler *c, ZeroCCType *elem,
                                            size_t brace_pos, size_t *out_count) {
    size_t count = 0, total = 0;
    if (out_count) *out_count = 0;
    if (!elem || brace_pos >= c->token_count) return 0;
    size_t elem_sz = (elem->size > 0) ? elem->size : 1;
    bool elem_is_ptr  = (elem->tag == TYPE_PTR);
    bool elem_is_char = (elem->tag == TYPE_INT8 || elem->tag == TYPE_UINT8);
    ZeroCCTokType first = c->tokens[brace_pos].type;
    if (first == TK_STRING) {
        if (!elem_is_char) return 0;
        const char *s = c->tokens[brace_pos].allocated_str ?
            c->tokens[brace_pos].allocated_str : c->tokens[brace_pos].str_val;
        return strlen(s) + 1;
    }
    if (first != TK_LBRACE) return 0;
    size_t q = brace_pos + 1; /* first token after '{' */
    while (q < c->token_count) {
        ZeroCCTokType tt = c->tokens[q].type;
        if (tt == TK_COMMA) { q++; continue; }          /* top-level separator */
        if (tt == TK_RBRACE) { q++; break; }            /* the array's '}' */
        if (tt == TK_LBRACE) {                          /* nested grouping brace */
            int d2 = 1; q++;
            while (q < c->token_count && d2 > 0) {
                if (c->tokens[q].type == TK_LBRACE) d2++;
                else if (c->tokens[q].type == TK_RBRACE) d2--;
                q++;
            }
            count++;
            total += (elem_sz < 8) ? elem_sz : 8;
            continue;
        }
        /* A top-level element. */
        count++;
        if (tt == TK_STRING) {
            const char *s = c->tokens[q].allocated_str ?
                c->tokens[q].allocated_str : c->tokens[q].str_val;
            total += (elem_is_ptr) ? 8 : (elem_is_char ? strlen(s) + 1 : ((elem_sz < 8) ? elem_sz : 8));
            q++;
        } else {
            total += (elem_sz < 8) ? elem_sz : 8;
            /* Skip the constant-expression tokens up to a top-level ',' or '}'. */
            int paren = 0;
            for (;;) {
                ZeroCCTokType ct = c->tokens[q].type;
                if (ct == TK_LPAREN) { paren++; q++; continue; }
                if (ct == TK_RPAREN) { if (paren > 0) paren--; q++; continue; }
                if (paren == 0 && (ct == TK_COMMA || ct == TK_RBRACE)) break;
                q++;
            }
        }
    }
    if (out_count) *out_count = count;
    return total;
}

static void zero_cc_synchronize_ast(ZeroCCCompiler *c) {
    while (c->token_pos < c->token_count) {
        ZeroCCTokType t = c->tokens[c->token_pos].type;
        if (t == TK_SEMI) {
            c->token_pos++;
            return;
        }
        if (t == TK_RBRACE || t == TK_EOF) {
            return;
        }
        /* Stop at statement-starting keywords */
        if (t == TK_IF || t == TK_WHILE || t == TK_DO || t == TK_FOR ||
            t == TK_SWITCH || t == TK_CASE || t == TK_DEFAULT ||
            t == TK_RETURN || t == TK_BREAK || t == TK_CONTINUE ||
            t == TK_GOTO || t == TK_STATIC_ASSERT || t == TK_LBRACE ||
            is_type_specifier(c, t)) {
            return;
        }
        c->token_pos++;
    }
}

ASTRef zero_cc_parse_stmt_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx) {
    if (!c || !arena || c->token_pos >= c->token_count) return AST_NULL;

    skip_c23_attributes(c);
    if (c->token_pos >= c->token_count) return AST_NULL;

    ZeroCCTokType t = c->tokens[c->token_pos].type;
    uint32_t line = (uint32_t)c->tokens[c->token_pos].line;
    uint32_t col = (uint32_t)c->tokens[c->token_pos].col;

    /* Empty statement */
    if (t == TK_SEMI) {
        c->token_pos++;
        return AST_NULL;
    }

    /* Goto statement: goto label; */
    if (t == TK_GOTO) {
        c->token_pos++;
        ASTRef g_ref = zero_cc_ast_alloc_node(arena, NODE_GOTO);
        ZeroCCASTNode *gn = zero_cc_ast_get(arena, g_ref);
        gn->line = line;
        gn->col = col;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
            gn->s.str = zero_cc_intern_string(c, c->tokens[c->token_pos].str_val);
            gn->s.len = strlen(gn->s.str);
            c->token_pos++;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }
        return g_ref;
    }

    /* Labeled statement: label: stmt (a statement-leading IDENT immediately
       followed by ':' is a user label, not an expression) */
    if (t == TK_IDENT && c->token_pos + 1 < c->token_count &&
        c->tokens[c->token_pos + 1].type == TK_COLON) {
        ASTRef l_ref = zero_cc_ast_alloc_node(arena, NODE_LABEL);
        ZeroCCASTNode *ln = zero_cc_ast_get(arena, l_ref);
        ln->line = line;
        ln->col = col;
        ln->s.str = zero_cc_intern_string(c, c->tokens[c->token_pos].str_val);
        ln->s.len = strlen(ln->s.str);
        c->token_pos += 2; /* ident ':' */
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) {
            ln->left = zero_cc_parse_stmt_ast(c, arena, ctx);
        }
        return l_ref;
    }

    /* Block-scoped typedef: typedef X Alias; - register into the TU-wide
       typedef table (same handlers as file scope); emits no IR. */
    if (t == TK_TYPEDEF) {
        if (c->token_pos + 1 < c->token_count &&
            (c->tokens[c->token_pos + 1].type == TK_STRUCT ||
             c->tokens[c->token_pos + 1].type == TK_UNION)) {
            zero_cc_stmt_typedef_struct(c);
        } else if (c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_ENUM) {
            zero_cc_stmt_typedef_enum(c);
        } else {
            zero_cc_stmt_typedef_scalar(c);
        }
        return AST_NULL;
    }

    /* Compound Block: { stmt* } */
    if (t == TK_LBRACE) {
        c->token_pos++;
        ASTRef block_ref = zero_cc_ast_alloc_node(arena, NODE_BLOCK);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, block_ref);
        bn->line = line;
        bn->col = col;

        ASTRef head = AST_NULL;
        ASTRef tail = AST_NULL;

        while (c->token_pos < c->token_count &&
               c->tokens[c->token_pos].type != TK_RBRACE &&
               c->tokens[c->token_pos].type != TK_EOF) {
            size_t entry_pos = c->token_pos;
            ASTRef s = zero_cc_parse_stmt_ast(c, arena, ctx);
            if (s != AST_NULL) {
                if (head == AST_NULL) {
                    head = s;
                    tail = s;
                } else {
                    ZeroCCASTNode *tail_node = zero_cc_ast_get(arena, tail);
                    if (tail_node) {
                        tail_node->next = s;
                    }
                    tail = s;
                }
                while (tail != AST_NULL) {
                    ZeroCCASTNode *cur_node = zero_cc_ast_get(arena, tail);
                    if (cur_node && cur_node->next != AST_NULL) {
                        tail = cur_node->next;
                    } else {
                        break;
                    }
                }
            } else {
                /* If statement failed to parse or did not advance token position, report error and synchronize */
                if (c->token_pos == entry_pos) {
                    diag_report_error_at_token(c, "unexpected token '%s' in statement",
                                               c->tokens[c->token_pos].str_val[0] ? c->tokens[c->token_pos].str_val : "unknown");
                    zero_cc_synchronize_ast(c);
                }
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) {
            c->token_pos++;
        } else {
            /* Block ran into end of input without its closing '}':
             * truncated source (C99 6.8p10).  Previously accepted
             * silently, so a pasted-without-the-end file compiled and
             * ran garbage. */
            diag_report_error_at_token(c, "expected '}' to end block");
        }
        bn = zero_cc_ast_get(arena, block_ref);
        if (bn) bn->left = head;
        return block_ref;
    }

    /* Return: return [expr]; */
    if (t == TK_RETURN) {
        c->token_pos++;
        ASTRef ret_ref = zero_cc_ast_alloc_node(arena, NODE_RETURN);
        ZeroCCASTNode *rn = zero_cc_ast_get(arena, ret_ref);
        rn->line = line;
        rn->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) {
            ASTRef expr = parse_expr_ast_internal(c, arena);
            rn = zero_cc_ast_get(arena, ret_ref);
            if (rn) rn->left = expr;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        } else if (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type != TK_RBRACE) {
            /* A return statement always ends in ';' (C99 6.8.4.1).  The
             * missing-';' case used to be silently swallowed, so
             * `return x + 5 return;` only surfaced as a misleading
             * "should return a value" on the next token. */
            diag_report_error_at_token(c, "expected ';' after return statement");
        }
        return ret_ref;
    }

    /* If statement: if (cond) then [else alt] */
    if (t == TK_IF) {
        c->token_pos++;
        ASTRef if_ref = zero_cc_ast_alloc_node(arena, NODE_IF);
        ZeroCCASTNode *in = zero_cc_ast_get(arena, if_ref);
        in->line = line;
        in->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
        }
        ASTRef cond = parse_expr_ast_internal(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }

        ASTRef then_stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
        ASTRef else_stmt = AST_NULL;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ELSE) {
            c->token_pos++;
            else_stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
        }

        in = zero_cc_ast_get(arena, if_ref);
        if (in) {
            in->left = cond;
            in->right = then_stmt;
            in->aux.alt = else_stmt;
        }
        return if_ref;
    }

    /* While loop: while (cond) body */
    if (t == TK_WHILE) {
        c->token_pos++;
        ASTRef while_ref = zero_cc_ast_alloc_node(arena, NODE_WHILE);
        ZeroCCASTNode *wn = zero_cc_ast_get(arena, while_ref);
        wn->line = line;
        wn->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
        }
        ASTRef cond = parse_expr_ast_internal(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }

        ASTRef body = zero_cc_parse_stmt_ast(c, arena, ctx);
        wn = zero_cc_ast_get(arena, while_ref);
        if (wn) {
            wn->left = cond;
            wn->right = body;
        }
        return while_ref;
    }

    /* Do-While loop: do body while (cond); */
    if (t == TK_DO) {
        c->token_pos++;
        ASTRef do_ref = zero_cc_ast_alloc_node(arena, NODE_DO_WHILE);
        ZeroCCASTNode *dn = zero_cc_ast_get(arena, do_ref);
        dn->line = line;
        dn->col = col;

        ASTRef body = zero_cc_parse_stmt_ast(c, arena, ctx);
        ASTRef cond = AST_NULL;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_WHILE) {
            c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
            }
            cond = parse_expr_ast_internal(c, arena);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                c->token_pos++;
            }
        }
        dn = zero_cc_ast_get(arena, do_ref);
        if (dn) {
            dn->left = cond;
            dn->right = body;
        }
        return do_ref;
    }

    /* For loop: for (init; cond; step) body */
    if (t == TK_FOR) {
        c->token_pos++;
        ASTRef for_ref = zero_cc_ast_alloc_node(arena, NODE_FOR);
        ZeroCCASTNode *fn = zero_cc_ast_get(arena, for_ref);
        fn->line = line;
        fn->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
        }

        /* 1. Init */
        ASTRef init_stmt = AST_NULL;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) {
            init_stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
        } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }

        /* 2. Cond */
        ASTRef cond_expr = AST_NULL;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) {
            cond_expr = parse_expr_ast_internal(c, arena);
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }

        /* 3. Step */
        ASTRef step_expr = AST_NULL;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RPAREN) {
            step_expr = parse_expr_ast_internal(c, arena);
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }

        /* 4. Body */
        ASTRef body_stmt = zero_cc_parse_stmt_ast(c, arena, ctx);

        fn = zero_cc_ast_get(arena, for_ref);
        if (fn) {
            fn->left = init_stmt;
            fn->right = cond_expr;
            fn->aux.step = step_expr;
            fn->aux.alt = body_stmt;
        }
        return for_ref;
    }

    /* Switch statement: switch (expr) body */
    if (t == TK_SWITCH) {
        c->token_pos++;
        ASTRef sw_ref = zero_cc_ast_alloc_node(arena, NODE_SWITCH);
        ZeroCCASTNode *sn = zero_cc_ast_get(arena, sw_ref);
        sn->line = line;
        sn->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
        }
        ASTRef expr = parse_expr_ast_internal(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }
        ASTRef body = zero_cc_parse_stmt_ast(c, arena, ctx);

        sn = zero_cc_ast_get(arena, sw_ref);
        if (sn) {
            sn->left = expr;
            sn->right = body;
        }
        return sw_ref;
    }

    /* Case statement: case const_expr: stmt */
    if (t == TK_CASE) {
        c->token_pos++;
        ASTRef case_ref = zero_cc_ast_alloc_node(arena, NODE_CASE);
        ZeroCCASTNode *cn = zero_cc_ast_get(arena, case_ref);
        cn->line = line;
        cn->col = col;

        ASTRef val = parse_expr_ast_internal(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COLON) {
            c->token_pos++;
        }
        ASTRef stmt = AST_NULL;
        if (c->token_pos < c->token_count &&
            c->tokens[c->token_pos].type != TK_CASE &&
            c->tokens[c->token_pos].type != TK_DEFAULT &&
            c->tokens[c->token_pos].type != TK_RBRACE) {
            stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
        }
        cn = zero_cc_ast_get(arena, case_ref);
        if (cn) {
            cn->left = val;
            cn->right = stmt;
        }
        return case_ref;
    }

    /* Default statement: default: stmt */
    if (t == TK_DEFAULT) {
        c->token_pos++;
        ASTRef def_ref = zero_cc_ast_alloc_node(arena, NODE_DEFAULT);
        ZeroCCASTNode *dn = zero_cc_ast_get(arena, def_ref);
        dn->line = line;
        dn->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COLON) {
            c->token_pos++;
        }
        ASTRef stmt = AST_NULL;
        if (c->token_pos < c->token_count &&
            c->tokens[c->token_pos].type != TK_CASE &&
            c->tokens[c->token_pos].type != TK_DEFAULT &&
            c->tokens[c->token_pos].type != TK_RBRACE) {
            stmt = zero_cc_parse_stmt_ast(c, arena, ctx);
        }
        dn = zero_cc_ast_get(arena, def_ref);
        if (dn) {
            dn->left = stmt;
        }
        return def_ref;
    }

    /* Break */
    if (t == TK_BREAK) {
        c->token_pos++;
        ASTRef brk_ref = zero_cc_ast_alloc_node(arena, NODE_BREAK);
        ZeroCCASTNode *bn = zero_cc_ast_get(arena, brk_ref);
        bn->line = line;
        bn->col = col;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }
        return brk_ref;
    }

    /* Continue */
    if (t == TK_CONTINUE) {
        c->token_pos++;
        ASTRef cont_ref = zero_cc_ast_alloc_node(arena, NODE_CONTINUE);
        ZeroCCASTNode *cn = zero_cc_ast_get(arena, cont_ref);
        cn->line = line;
        cn->col = col;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }
        return cont_ref;
    }

    /* Static Assert: _Static_assert(cond, [msg]); */
    if (t == TK_STATIC_ASSERT) {
        c->token_pos++;
        ASTRef sa_ref = zero_cc_ast_alloc_node(arena, NODE_STATIC_ASSERT);
        ZeroCCASTNode *san = zero_cc_ast_get(arena, sa_ref);
        san->line = line;
        san->col = col;

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
        }
        ASTRef cond = parse_assignment_ast(c, arena);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
            c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STRING) {
                c->token_pos++;
            }
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        }
        san = zero_cc_ast_get(arena, sa_ref);
        if (san) san->left = cond;
        return sa_ref;
    }

    /* Variable Declaration or Typedef Declaration */
    /* Parse alignment, storage specifiers & type qualifiers */
    bool is_const = false;
    bool is_volatile = false;
    bool is_atomic = false;
    bool is_static_local = false;   /* Sprint 37: block-scope `static` */

    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_ALIGNAS ||
            c->tokens[c->token_pos].type == TK_CONSTEXPR ||
            c->tokens[c->token_pos].type == TK_CONST ||
            c->tokens[c->token_pos].type == TK_VOLATILE ||
            c->tokens[c->token_pos].type == TK_RESTRICT ||
            c->tokens[c->token_pos].type == TK_STATIC ||
            c->tokens[c->token_pos].type == TK_EXTERN ||
            c->tokens[c->token_pos].type == TK_THREAD_LOCAL ||
            c->tokens[c->token_pos].type == TK_REGISTER)) {
        if (c->tokens[c->token_pos].type == TK_CONST || c->tokens[c->token_pos].type == TK_CONSTEXPR) {
            is_const = true;
            c->token_pos++;
        } else if (c->tokens[c->token_pos].type == TK_VOLATILE) {
            is_volatile = true;
            c->token_pos++;
        } else if (c->tokens[c->token_pos].type == TK_RESTRICT) {
            c->token_pos++;
        } else if (c->tokens[c->token_pos].type == TK_ALIGNAS) {
            c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
                int64_t align_val = 0;
                if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                    ZeroCCType atype = zero_cc_parse_type_name(c);
                    align_val = atype.align > 0 ? (int64_t)atype.align : 1;
                } else {
                    align_val = zero_cc_eval_const_expr(c);
                }
                if (align_val > 0) {
                    decl_align_spec = align_val;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++;
                }
            }
        } else if (c->tokens[c->token_pos].type == TK_STATIC) {
            is_static_local = true;  /* Sprint 37 */
            c->token_pos++;
        } else {
            c->token_pos++;
        }
    }

    if (c->token_pos < c->token_count) {
        t = c->tokens[c->token_pos].type;
    }

    /* Block-scoped enum definition / declaration */
    if (t == TK_ENUM && c->token_pos + 1 < c->token_count &&
        (c->tokens[c->token_pos + 1].type == TK_LBRACE ||
         (c->tokens[c->token_pos + 1].type == TK_IDENT &&
          c->token_pos + 2 < c->token_count &&
          (c->tokens[c->token_pos + 2].type == TK_LBRACE || c->tokens[c->token_pos + 2].type == TK_SEMI)))) {
        zero_cc_stmt_enum(c);
        return AST_NULL;
    }

    if (is_type_specifier(c, t)) {
        size_t type_start_pos = c->token_pos;
        ZeroCCTokType type_tok = t;
        bool decl_is_unsigned = false;
        c->token_pos++;
        ZeroCCType typeof_base_type;
        memset(&typeof_base_type, 0, sizeof(typeof_base_type));
        bool has_typeof_base = false;
        if (type_tok == TK_TYPEOF || type_tok == TK_TYPEOF_UNQUAL) {
            has_typeof_base = true;
            bool unqual = (type_tok == TK_TYPEOF_UNQUAL);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
                if (zero_cc_tok_is_type_start(c, c->token_pos)) {
                    typeof_base_type = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
                } else {
                    typeof_base_type = zero_cc_type_of_equality(c, &c->token_pos);
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++;
                }
            } else {
                typeof_base_type = zero_cc_type_int();
            }
            if (unqual) {
                typeof_base_type.is_const = false;
                typeof_base_type.is_volatile = false;
                typeof_base_type.is_restrict = false;
                typeof_base_type.is_atomic = false;
            }
        }
        /* Consume a leading signed/unsigned qualifier: `unsigned char x;`,
         * `signed short y;`, bare `unsigned z;`. Without this the base
         * specifier is misread and narrow-int locals/params get the wrong
         * signedness tag (e.g. `unsigned char` -> TYPE_INT8). */
        if (type_tok == TK_UNSIGNED || type_tok == TK_SIGNED) {
            decl_is_unsigned = (type_tok == TK_UNSIGNED);
            if (c->token_pos < c->token_count &&
                (c->tokens[c->token_pos].type == TK_INT ||
                 c->tokens[c->token_pos].type == TK_CHAR ||
                 c->tokens[c->token_pos].type == TK_SHORT ||
                 c->tokens[c->token_pos].type == TK_LONG)) {
                type_tok = c->tokens[c->token_pos].type;
                type_start_pos = c->token_pos;
                c->token_pos++;
                if (type_tok == TK_LONG && c->token_pos < c->token_count &&
                    c->tokens[c->token_pos].type == TK_LONG) c->token_pos++; /* long long */
            } else {
                type_tok = TK_INT;  /* bare `unsigned` / `signed` */
            }
        }
        if (type_tok == TK_STRUCT || type_tok == TK_UNION || type_tok == TK_ENUM) {
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                type_start_pos = c->token_pos;
                c->token_pos++;
            }
        }
        /* Check _Atomic(T) or _Atomic T */
        if (type_tok == TK_ATOMIC) {
            is_atomic = true;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
                if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                    type_tok = c->tokens[c->token_pos].type;
                    type_start_pos = c->token_pos;
                    c->token_pos++;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++;
                }
            } else if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                type_tok = c->tokens[c->token_pos].type;
                type_start_pos = c->token_pos;
                c->token_pos++;
            }
        }
        /* Also allow type _Atomic, const, volatile x; */
        while (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_ATOMIC ||
                                                 c->tokens[c->token_pos].type == TK_CONST ||
                                                 c->tokens[c->token_pos].type == TK_VOLATILE ||
                                                 c->tokens[c->token_pos].type == TK_RESTRICT)) {
            if (c->tokens[c->token_pos].type == TK_ATOMIC) is_atomic = true;
            if (c->tokens[c->token_pos].type == TK_CONST) is_const = true;
            if (c->tokens[c->token_pos].type == TK_VOLATILE) is_volatile = true;
            c->token_pos++;
        }

        /* Pointer asterisks and pointer qualifiers */
        int ptr_depth = 0;
        bool ptr_is_const = false;
        bool ptr_is_volatile = false;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
            ptr_depth++;
            c->token_pos++;
            while (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_CONST ||
                                                     c->tokens[c->token_pos].type == TK_VOLATILE ||
                                                     c->tokens[c->token_pos].type == TK_RESTRICT)) {
                if (c->tokens[c->token_pos].type == TK_CONST) ptr_is_const = true;
                if (c->tokens[c->token_pos].type == TK_VOLATILE) ptr_is_volatile = true;
                c->token_pos++;
            }
        }

        /* Check for inline function pointer declarator: Type (*name)(params) */
        bool is_inline_func_ptr = false;
        const char *fp_vname = NULL;
        if (c->token_pos + 1 < c->token_count &&
            c->tokens[c->token_pos].type == TK_LPAREN &&
            c->tokens[c->token_pos + 1].type == TK_STAR) {
            is_inline_func_ptr = true;
            c->token_pos += 2; /* consume '(' and '*' */
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                ZeroCCToken *var_tok = &c->tokens[c->token_pos];
                fp_vname = var_tok->allocated_str ? var_tok->allocated_str : var_tok->str_val;
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                int pdepth = 1;
                c->token_pos++;
                while (c->token_pos < c->token_count && pdepth > 0) {
                    if (c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                    else if (c->tokens[c->token_pos].type == TK_RPAREN) pdepth--;
                    c->token_pos++;
                }
            }
        }

        /* Check for identifier name */
        if (is_inline_func_ptr || (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT)) {
            const char *vname = fp_vname;
            if (!is_inline_func_ptr) {
                ZeroCCToken *var_tok = &c->tokens[c->token_pos];
                vname = var_tok->allocated_str ? var_tok->allocated_str : var_tok->str_val;
                c->token_pos++;
            }

            /* Check if this is a function declarator */
            bool is_function = false;
            ASTRef param_list = AST_NULL;
            bool has_explicit_params = false;
            if (!is_inline_func_ptr && c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                is_function = true;
                c->token_pos++; // consume '('
                /* Parse parameter list */
                ASTRef param_head = AST_NULL;
                ASTRef param_tail = AST_NULL;
                bool first_param = true;
                while (c->token_pos < c->token_count) {
                    if (c->tokens[c->token_pos].type == TK_RPAREN) {
                        break;
                    }
                    if (!first_param) {
                        if (c->tokens[c->token_pos].type == TK_COMMA) {
                            c->token_pos++; // consume comma
                        } else {
                            break; // error
                        }
                    }
                    first_param = false;
                    /* Check for void parameter list: void alone means no params */
                    if (c->tokens[c->token_pos].type == TK_VOID) {
                        c->token_pos++;
                        if (c->tokens[c->token_pos].type == TK_RPAREN) {
                            break;
                        }
                    }
                    /* Parse parameter declaration: qualifiers + type specifier + optional pointer + identifier */
                    bool param_is_const = false;
                    bool param_is_volatile = false;
                    while (c->token_pos < c->token_count &&
                           (c->tokens[c->token_pos].type == TK_CONST ||
                            c->tokens[c->token_pos].type == TK_VOLATILE ||
                            c->tokens[c->token_pos].type == TK_RESTRICT)) {
                        if (c->tokens[c->token_pos].type == TK_CONST) param_is_const = true;
                        if (c->tokens[c->token_pos].type == TK_VOLATILE) param_is_volatile = true;
                        c->token_pos++;
                    }
                    size_t ptype_pos = c->token_pos;
                    ZeroCCTokType param_type_tok = c->tokens[c->token_pos].type;
                    if (!is_type_specifier(c, param_type_tok)) {
                        break; // error
                    }
                    c->token_pos++;
                    ZeroCCType param_typeof_type;
                    memset(&param_typeof_type, 0, sizeof(param_typeof_type));
                    bool param_has_typeof = false;
                    if (param_type_tok == TK_TYPEOF || param_type_tok == TK_TYPEOF_UNQUAL) {
                        param_has_typeof = true;
                        bool unqual = (param_type_tok == TK_TYPEOF_UNQUAL);
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                            c->token_pos++;
                            if (zero_cc_tok_is_type_start(c, c->token_pos)) {
                                param_typeof_type = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
                            } else {
                                param_typeof_type = zero_cc_type_of_equality(c, &c->token_pos);
                            }
                            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                                c->token_pos++;
                            }
                        } else {
                            param_typeof_type = zero_cc_type_int();
                        }
                        if (unqual) {
                            param_typeof_type.is_const = false;
                            param_typeof_type.is_volatile = false;
                            param_typeof_type.is_restrict = false;
                            param_typeof_type.is_atomic = false;
                        }
                    }
                    /* signed/unsigned parameter qualifier (C11 6.7.5): the
                     * signedness token precedes the base specifier. */
                    bool param_is_unsigned = false;
                    if (param_type_tok == TK_UNSIGNED || param_type_tok == TK_SIGNED) {
                        param_is_unsigned = (param_type_tok == TK_UNSIGNED);
                        if (c->token_pos < c->token_count &&
                            (c->tokens[c->token_pos].type == TK_INT ||
                             c->tokens[c->token_pos].type == TK_CHAR ||
                             c->tokens[c->token_pos].type == TK_SHORT ||
                             c->tokens[c->token_pos].type == TK_LONG)) {
                            ptype_pos = c->token_pos;
                            param_type_tok = c->tokens[c->token_pos].type;
                            c->token_pos++;
                            if (param_type_tok == TK_LONG && c->token_pos < c->token_count &&
                                c->tokens[c->token_pos].type == TK_LONG) c->token_pos++; /* long long */
                        } else {
                            param_type_tok = TK_INT;
                        }
                    }
                    const char *param_tag_name = NULL;
                    if ((param_type_tok == TK_STRUCT || param_type_tok == TK_UNION || param_type_tok == TK_ENUM) &&
                        c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                        param_tag_name = c->tokens[c->token_pos].str_val;
                        c->token_pos++;
                    }
                    while (c->token_pos < c->token_count &&
                           (c->tokens[c->token_pos].type == TK_CONST ||
                            c->tokens[c->token_pos].type == TK_VOLATILE ||
                            c->tokens[c->token_pos].type == TK_RESTRICT)) {
                        if (c->tokens[c->token_pos].type == TK_CONST) param_is_const = true;
                        if (c->tokens[c->token_pos].type == TK_VOLATILE) param_is_volatile = true;
                        c->token_pos++;
                    }
                    int param_ptr_depth = 0;
                    bool param_ptr_is_const = false;
                    bool param_ptr_is_volatile = false;
                    bool param_ptr_is_restrict = false;
                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                        param_ptr_depth++;
                        c->token_pos++;
                        while (c->token_pos < c->token_count &&
                               (c->tokens[c->token_pos].type == TK_CONST ||
                                c->tokens[c->token_pos].type == TK_VOLATILE ||
                                c->tokens[c->token_pos].type == TK_RESTRICT)) {
                            if (c->tokens[c->token_pos].type == TK_CONST) param_ptr_is_const = true;
                            if (c->tokens[c->token_pos].type == TK_VOLATILE) param_ptr_is_volatile = true;
                            if (c->tokens[c->token_pos].type == TK_RESTRICT) param_ptr_is_restrict = true;
                            c->token_pos++;
                        }
                    }
                    const char *pname = NULL;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                        pname = c->tokens[c->token_pos].str_val;
                        c->token_pos++;
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                            c->token_pos++;
                            int bdepth = 1;
                            while (c->token_pos < c->token_count && bdepth > 0) {
                                if (c->tokens[c->token_pos].type == TK_LBRACKET) bdepth++;
                                else if (c->tokens[c->token_pos].type == TK_RBRACKET) bdepth--;
                                c->token_pos++;
                            }
                            param_ptr_depth++;
                    }
                    /* Create parameter node. Unnamed parameters (C99
                     * 6.7.6.3: `int f(int);`) still constrain the caller -
                     * the node is created with an empty name so the prototype
                     * keeps its parameter count; previously nodes were only
                     * created for named params, so `int f(int);` became a
                     * 0-parameter prototype and legal calls were rejected
                     * (and argument promotion to the param type was
                     * skipped). */
                    if (pname == NULL) pname = "";
                        /* Create parameter node */
                        ASTRef p_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                        ZeroCCASTNode *p_node = zero_cc_ast_get(arena, p_ref);
                        p_node->line = c->tokens[c->token_pos].line; // approximate
                        p_node->col = c->tokens[c->token_pos].col;
                        p_node->s.str = pname;
                        p_node->s.len = strlen(pname);
                        /* Store parameter type info via canonical resolver and bridge */
                        const char *ptname = param_tag_name ? param_tag_name : c->tokens[ptype_pos].str_val;
                        ZeroCCType ptype = param_has_typeof ? param_typeof_type : zero_cc_resolve_base_type(c, param_type_tok, ptname);
                        /* Apply the signedness qualifier (resolver returns signed variants). */
                        if (param_is_unsigned) {
                            if (ptype.tag == TYPE_INT8) ptype.tag = TYPE_UINT8;
                            else if (ptype.tag == TYPE_INT16) ptype.tag = TYPE_UINT16;
                            else if (ptype.tag == TYPE_INT32) ptype.tag = TYPE_UINT32;
                            else if (ptype.tag == TYPE_INT64) ptype.tag = TYPE_UINT64;
                        }
                        ptype.is_const = param_is_const;
                        ptype.is_volatile = param_is_volatile;
                        for (int pd = 0; pd < param_ptr_depth; pd++) {
                            ptype = zero_cc_type_ptr(&ptype);
                            if (pd == param_ptr_depth - 1) {
                                ptype.is_const = param_ptr_is_const;
                                ptype.is_volatile = param_ptr_is_volatile;
                                ptype.is_restrict = param_ptr_is_restrict;
                            }
                        }
                        TypeInfo *ptype_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, ptype);
                        decl_node_set_type(ctx, p_node, ptype_ti);
                        if (param_head == AST_NULL) {
                            param_head = p_ref;
                            param_tail = p_ref;
                        } else {
                            ZeroCCASTNode *tail_node = zero_cc_ast_get(arena, param_tail);
                            if (tail_node) tail_node->next = p_ref;
                            param_tail = p_ref;
                        }
                    }
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++; // consume ')'
                }
                param_list = param_head;
                /* first_param is still true only for an empty () list; an
                 * explicit parameter list (incl. (void)) is a prototype. */
                has_explicit_params = !first_param;
            }

            if (is_function) {
                /* Build canonical return type and function TypeInfo */
                ZeroCCType ret_type = has_typeof_base ? typeof_base_type : zero_cc_resolve_base_type(c, type_tok, c->tokens[type_start_pos].str_val);
                if (decl_is_unsigned) {
                    if (ret_type.tag == TYPE_INT8) ret_type.tag = TYPE_UINT8;
                    else if (ret_type.tag == TYPE_INT16) ret_type.tag = TYPE_UINT16;
                    else if (ret_type.tag == TYPE_INT32) ret_type.tag = TYPE_UINT32;
                    else if (ret_type.tag == TYPE_INT64) ret_type.tag = TYPE_UINT64;
                }
                for (int pd = 0; pd < ptr_depth; pd++) ret_type = zero_cc_type_ptr(&ret_type);
                TypeInfo *ret_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, ret_type);
                TypeInfo *param_tis[16];
                size_t param_cnt = 0;
                ASTRef cur_p = param_list;
                while (cur_p != AST_NULL && param_cnt < 16) {
                    ZeroCCASTNode *pn = zero_cc_ast_get(arena, cur_p);
                    if (!pn) break;
                    TypeInfo *pti = decl_node_type(ctx, pn);
                    param_tis[param_cnt++] = pti ? pti : type_int(ctx->perm_arena);
                    cur_p = pn->next;
                }
                TypeInfo *fn_ti = type_function(ctx->perm_arena, ret_ti, param_cnt > 0 ? param_tis : NULL, param_cnt, false);
                fn_ti->details.function.is_prototype = has_explicit_params;

                /* Function definition or prototype */
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
                    /* Function definition with body */
                    c->token_pos++; // consume '{'
                    ASTRef head = AST_NULL;
                    ASTRef tail = AST_NULL;
                    while (c->token_pos < c->token_count &&
                           c->tokens[c->token_pos].type != TK_RBRACE &&
                           c->tokens[c->token_pos].type != TK_EOF) {
                        ASTRef s = zero_cc_parse_stmt_ast(c, arena, ctx);
                        if (s != AST_NULL) {
                            if (head == AST_NULL) {
                                head = s;
                                tail = s;
                            } else {
                                ZeroCCASTNode *tail_node = zero_cc_ast_get(arena, tail);
                                if (tail_node) tail_node->next = s;
                                tail = s;
                            }
                        }
                    }
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) {
                        c->token_pos++; // consume '}'
                    }
                    ASTRef body_ref = head;

                    /* Create function node */
                    ASTRef func_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                    ZeroCCASTNode *func_node = zero_cc_ast_get(arena, func_ref);
                    func_node->line = line;
                    func_node->col = col;
                    func_node->s.str = zero_cc_intern_string(c, vname);
                    func_node->s.len = strlen(func_node->s.str);
                    func_node->left = param_list;   // parameter list
                    func_node->right = body_ref;    // body
                    decl_node_set_type(ctx, func_node, fn_ti);
                    return func_ref;
                } else {
                    /* Function prototype */
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                        c->token_pos++;
                    }
                    ASTRef func_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                    ZeroCCASTNode *func_node = zero_cc_ast_get(arena, func_ref);
                    func_node->line = line;
                    func_node->col = col;
                    func_node->s.str = zero_cc_intern_string(c, vname);
                    func_node->s.len = strlen(func_node->s.str);
                    func_node->flags |= NODE_FLAG_FUNC_PROTO;
                    func_node->left = param_list;
                    func_node->right = AST_NULL;
                    decl_node_set_type(ctx, func_node, fn_ti);

                    /* Register in c->functions if not already registered */
                    if (!zero_cc_find_function(c, vname) && c->function_count < ZERO_CC_MAX_FUNCTIONS) {
                        ZeroCCFunction *fn = &c->functions[c->function_count++];
                        snprintf(fn->name, sizeof(fn->name), "%s", vname);
                        fn->text_offset = 0;
                        fn->return_type = ret_type;
                        fn->returns_struct = (ret_type.tag == TYPE_STRUCT || ret_type.tag == TYPE_UNION);
                        fn->ret_struct_sz = ret_type.size;
                        fn->is_prototype = true;
                    }
                    return func_ref;
                }
            } else {
                /* Not a function: treat as variable declaration */
                #define MAX_ARRAY_DIMS 16
                size_t dim_counts[MAX_ARRAY_DIMS];
                ASTRef dim_exprs[MAX_ARRAY_DIMS];
                bool dim_is_vla[MAX_ARRAY_DIMS];
                size_t num_dims = 0;
                bool is_array = false;
                bool has_any_vla = false;

                if (!is_inline_func_ptr) {
                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                        c->token_pos++; // consume '['
                        is_array = true;
                        size_t fixed_count = 0;
                        ASTRef dim_expr = AST_NULL;
                        bool is_dim_vla = false;

                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                            c->token_pos++;
                            fixed_count = 0;
                        } else {
                            dim_expr = parse_expr_ast_internal(c, arena);
                            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                                c->token_pos++;
                            }
                            ZeroCCASTNode *dim_node = zero_cc_ast_get(arena, dim_expr);
                            if (dim_node && dim_node->tag == NODE_INT && dim_node->int_val >= 0) {
                                fixed_count = (size_t)dim_node->int_val;
                            } else {
                                is_dim_vla = true;
                                has_any_vla = true;
                            }
                        }

                        if (num_dims < MAX_ARRAY_DIMS) {
                            dim_counts[num_dims] = fixed_count;
                            dim_exprs[num_dims] = dim_expr;
                            dim_is_vla[num_dims] = is_dim_vla;
                            num_dims++;
                        }
                    }

                    if (has_any_vla) {
                        /* In multi-dim VLA, only the outermost dimension may be runtime-sized */
                        for (size_t d = 1; d < num_dims; d++) {
                            if (dim_is_vla[d]) {
                                diag_report_error_at_token(c, "multi-dim VLA with runtime inner dimensions is not supported");
                                return AST_NULL;
                            }
                        }
                    }
                }

                ZeroCCType decl_type = has_typeof_base ? typeof_base_type : zero_cc_resolve_base_type(c, type_tok, c->tokens[type_start_pos].str_val);
                /* Apply the signedness qualifier (zero_cc_resolve_base_type
                 * always returns the signed variant for int base types). */
                if (decl_is_unsigned) {
                    if (decl_type.tag == TYPE_INT8) decl_type.tag = TYPE_UINT8;
                    else if (decl_type.tag == TYPE_INT16) decl_type.tag = TYPE_UINT16;
                    else if (decl_type.tag == TYPE_INT32) decl_type.tag = TYPE_UINT32;
                    else if (decl_type.tag == TYPE_INT64) decl_type.tag = TYPE_UINT64;
                }
                decl_type.is_const = is_const;
                decl_type.is_volatile = is_volatile;
                decl_type.is_atomic = is_atomic;
                for (int pd = 0; pd < ptr_depth; pd++) {
                    decl_type = zero_cc_type_ptr(&decl_type);
                    if (pd == ptr_depth - 1) {
                        decl_type.is_const = ptr_is_const;
                        decl_type.is_volatile = ptr_is_volatile;
                    }
                }
                if (is_inline_func_ptr) {
                    decl_type = zero_cc_type_ptr(&decl_type);
                    decl_type.is_func_ptr = true;
                }

                if (is_array) {
                    /* Build nested array ZeroCCType from innermost to outermost dimension */
                    ZeroCCType cur_type = decl_type;
                    for (size_t d = num_dims; d > 0; d--) {
                        size_t idx = d - 1;
                        if (dim_is_vla[idx]) {
                            cur_type = zero_cc_type_array(&cur_type, 0);
                            cur_type.is_vla = true;
                            cur_type.size = 8;
                            cur_type.align = 8;
                        } else {
                            cur_type = zero_cc_type_array(&cur_type, dim_counts[idx]);
                        }
                        cur_type.is_const = is_const;
                        cur_type.is_volatile = is_volatile;
                    }
                    decl_type = cur_type;
                }

                /* C99 6.7.9: an unsized array's outermost dimension is
                 * inferred from its initializer. Measure it (without
                 * consuming tokens) BEFORE zero_cc_add_local so the stack
                 * slot and the static-local data reservation are sized to
                 * the real object (not the 8-byte minimum), preventing the
                 * per-call mirror from overflowing into a neighbour. */
                if (is_array && num_dims > 0 && !has_any_vla &&
                    decl_type.tag == TYPE_ARRAY && decl_type.array_len == 0 &&
                    c->token_pos < c->token_count &&
                    c->tokens[c->token_pos].type == TK_ASSIGN &&
                    decl_type.base) {
                    size_t pre_count = 0;
                    size_t pre_sz = zero_cc_measure_array_init_at(
                        c, decl_type.base, c->token_pos + 1, &pre_count);
                    if (pre_sz > 0) {
                        decl_type.array_len = (decl_type.base->size > 0) ?
                            (pre_sz / decl_type.base->size) : pre_sz;
                        decl_type.size = pre_sz;
                    }
                }

                /* Derive canonical TypeInfo* directly from decl_type */
                TypeInfo *decl_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, decl_type);
                ZeroCCSym *sym = zero_cc_add_local(c, vname, decl_type);
                if (!sym) {
                    diag_report_error_at_token(c, "out of memory allocating local variable '%s'", vname);
                    return AST_NULL;
                }

                /* Sprint 37: static local — the object lives in the data
                 * section; the stack slot is a per-call mirror (see the
                 * zero_cc_static_local_register docs). */
                if (is_static_local) {
                    if (has_any_vla) {
                        diag_report_error_at_token(c,
                            "VLA '%s' cannot have static storage duration", vname);
                        return AST_NULL;
                    }
                    zero_cc_static_local_register(c, sym);
                }

                ASTRef decl_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                ZeroCCASTNode *dn = zero_cc_ast_get(arena, decl_ref);
                dn->line = line;
                dn->col = col;
                dn->s.str = zero_cc_intern_string(c, vname);
                dn->s.len = strlen(dn->s.str);
                if (has_any_vla) {
                    dn->flags |= NODE_DECL_IS_VLA;
                    dn->right = dim_exprs[0]; /* outermost runtime dimension expr */
                }

                /* Attach type to AST node for semantic analysis */
                decl_node_set_type(ctx, dn, decl_ti);

                /* C99 §6.7.8 p3: VLA shall not have an initializer */
                if ((dn->flags & NODE_DECL_IS_VLA) != 0) {
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                        diag_report_error_at_token(c,
                            "variable length array '%s' cannot be initialized", dn->s.str);
                        return AST_NULL;
                    }
                }

                /* Optional initializer: = expr or = { ... } */
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                    c->token_pos++;
                    if (sym->is_static_local) {
                        /* Sprint 37: constant-literal initializers are baked
                         * into the data section at parse time. The initializer
                         * is deliberately NOT attached to the AST: the value
                         * arrives via the prologue mirror load, and running
                         * the initializer in the body would reset the static
                         * on every call (the original bug). */
                        if (!zero_cc_static_local_patch_init(c, sym)) {
                            diag_report_error_at_token(c,
                                "static local '%s' initializer must be a constant literal "
                                "(complex initializers not yet supported)", vname);
                            while (c->token_pos < c->token_count &&
                                   c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                        }
                    } else {
                    ASTRef init_val = AST_NULL;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
                        /* Brace-enclosed / designated initializer: struct Point p = { .x = 10 }; */
                        size_t elem_count = 0;
                        ASTRef head_expr = parse_brace_init_list_ast(c, arena, decl_type, &elem_count);
                        if (decl_type.tag == TYPE_ARRAY && decl_type.array_len == 0 && elem_count > 0) {
                            decl_type.array_len = elem_count;
                            if (decl_type.base && decl_type.base->size > 0) {
                                decl_type.size = decl_type.base->size * elem_count;
                            }
                            if (sym) {
                                sym->type.array_len = elem_count;
                                if (sym->type.base && sym->type.base->size > 0) {
                                    sym->type.size = sym->type.base->size * elem_count;
                                }
                            }
                            if (decl_ti && decl_ti->kind == TYPE_KIND_ARRAY) {
                                decl_ti->details.array.count = elem_count;
                            }
                        }
                        ASTRef comp_ref = zero_cc_ast_alloc_node(arena, NODE_COMPOUND_LIT);
                        ZeroCCASTNode *cn = zero_cc_ast_get(arena, comp_ref);
                        cn->left = head_expr;
                        if (sym) {
                            cn->s.str = sym->name;
                            cn->s.len = strlen(sym->name);
                        }
                        init_val = comp_ref;
                    } else {
                        init_val = parse_assignment_ast(c, arena);
                    }
                    dn = zero_cc_ast_get(arena, decl_ref);
                    if (dn) dn->left = init_val;
                    } /* !is_static_local */
                }

                /* Sprint 37 v1: static locals with multiple comma declarators
                 * are not yet supported. */
                if (sym->is_static_local &&
                    c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
                    diag_report_error_at_token(c,
                        "static local '%s' with multiple declarators not yet supported", vname);
                    while (c->token_pos < c->token_count &&
                           c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                }
                if (sym->is_static_local &&
                    c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                    c->token_pos++;
                    return decl_ref;
                }

                ASTRef head_decl = decl_ref;
                ASTRef tail_decl = decl_ref;

                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
                    c->token_pos++; /* consume ',' */
                    int next_ptr_depth = 0;
                    bool next_ptr_is_const = false;
                    bool next_ptr_is_volatile = false;
                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                        next_ptr_depth++;
                        c->token_pos++;
                        while (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_CONST ||
                                                                 c->tokens[c->token_pos].type == TK_VOLATILE ||
                                                                 c->tokens[c->token_pos].type == TK_RESTRICT)) {
                            if (c->tokens[c->token_pos].type == TK_CONST) next_ptr_is_const = true;
                            if (c->tokens[c->token_pos].type == TK_VOLATILE) next_ptr_is_volatile = true;
                            c->token_pos++;
                        }
                    }
                    if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) {
                        break;
                    }
                    ZeroCCToken *next_var_tok = &c->tokens[c->token_pos];
                    const char *next_vname = next_var_tok->allocated_str ? next_var_tok->allocated_str : next_var_tok->str_val;
                    c->token_pos++;

                    ZeroCCType next_type = has_typeof_base ? typeof_base_type : zero_cc_resolve_base_type(c, type_tok, c->tokens[type_start_pos].str_val);
                    next_type.is_const = is_const;
                    next_type.is_volatile = is_volatile;
                    next_type.is_atomic = is_atomic;
                    for (int pd = 0; pd < next_ptr_depth; pd++) {
                        next_type = zero_cc_type_ptr(&next_type);
                        if (pd == next_ptr_depth - 1) {
                            next_type.is_const = next_ptr_is_const;
                            next_type.is_volatile = next_ptr_is_volatile;
                        }
                    }

                    /* Array dimensions for the 2nd..nth declarator
                     * (Sprint 40): `float a[n], b[n];` - previously only the
                     * first declarator parsed `[...]`, so a dimension on a
                     * later declarator broke the parse. */
                    size_t ndim_counts[MAX_ARRAY_DIMS];
                    ASTRef ndim_exprs[MAX_ARRAY_DIMS];
                    bool ndim_is_vla[MAX_ARRAY_DIMS];
                    size_t nnum_dims = 0;
                    bool nis_array = false;
                    bool nhas_any_vla = false;

                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                        c->token_pos++; // consume '['
                        nis_array = true;
                        size_t nfixed_count = 0;
                        ASTRef ndim_expr = AST_NULL;
                        bool nis_dim_vla = false;

                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                            c->token_pos++;
                            nfixed_count = 0;
                        } else {
                            ndim_expr = parse_expr_ast_internal(c, arena);
                            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                                c->token_pos++;
                            }
                            ZeroCCASTNode *ndim_node = zero_cc_ast_get(arena, ndim_expr);
                            if (ndim_node && ndim_node->tag == NODE_INT && ndim_node->int_val >= 0) {
                                nfixed_count = (size_t)ndim_node->int_val;
                            } else {
                                nis_dim_vla = true;
                                nhas_any_vla = true;
                            }
                        }

                        if (nnum_dims < MAX_ARRAY_DIMS) {
                            ndim_counts[nnum_dims] = nfixed_count;
                            ndim_exprs[nnum_dims] = ndim_expr;
                            ndim_is_vla[nnum_dims] = nis_dim_vla;
                            nnum_dims++;
                        }
                    }

                    if (nhas_any_vla) {
                        for (size_t d = 1; d < nnum_dims; d++) {
                            if (ndim_is_vla[d]) {
                                diag_report_error_at_token(c, "multi-dim VLA with runtime inner dimensions is not supported");
                                return AST_NULL;
                            }
                        }
                    }

                    if (nis_array) {
                        ZeroCCType ncur_type = next_type;
                        for (size_t d = nnum_dims; d > 0; d--) {
                            size_t idx = d - 1;
                            if (ndim_is_vla[idx]) {
                                ncur_type = zero_cc_type_array(&ncur_type, 0);
                                ncur_type.is_vla = true;
                                ncur_type.size = 8;
                                ncur_type.align = 8;
                            } else {
                                ncur_type = zero_cc_type_array(&ncur_type, ndim_counts[idx]);
                            }
                            ncur_type.is_const = is_const;
                            ncur_type.is_volatile = is_volatile;
                        }
                        next_type = ncur_type;
                    }

                    TypeInfo *next_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, next_type);
                    ZeroCCSym *next_sym = zero_cc_add_local(c, next_vname, next_type);
                    if (!next_sym) {
                        diag_report_error_at_token(c, "out of memory allocating local variable '%s'", next_vname);
                        return AST_NULL;
                    }

                    ASTRef next_decl_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                    ZeroCCASTNode *next_dn = zero_cc_ast_get(arena, next_decl_ref);
                    next_dn->line = line;
                    next_dn->col = col;
                    next_dn->s.str = next_vname;
                    next_dn->s.len = strlen(next_vname);
                    if (nhas_any_vla) {
                        next_dn->flags |= NODE_DECL_IS_VLA;
                        next_dn->right = ndim_exprs[0]; /* outermost runtime dimension expr */
                    }

                    /* Attach type to AST node for semantic analysis */
                    decl_node_set_type(ctx, next_dn, next_ti);

                    /* C99 §6.7.8 p3: VLA shall not have an initializer */
                    if ((next_dn->flags & NODE_DECL_IS_VLA) != 0 &&
                        c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                        diag_report_error_at_token(c,
                            "variable length array '%s' cannot be initialized", next_vname);
                        return AST_NULL;
                    }

                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                        c->token_pos++;
                        ASTRef next_init = parse_assignment_ast(c, arena);
                        next_dn = zero_cc_ast_get(arena, next_decl_ref);
                        if (next_dn) next_dn->left = next_init;
                    }

                    ZeroCCASTNode *tail_node = zero_cc_ast_get(arena, tail_decl);
                    if (tail_node) tail_node->next = next_decl_ref;
                    tail_decl = next_decl_ref;
                }

                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                    c->token_pos++;
                } else {
                    diag_report_error_at_token(c, "expected ';' after declaration of '%s'", vname);
                    return AST_NULL;
                }
                return head_decl;
            }
        }

    }

    /* General Expression Statement */
    ASTRef expr = parse_expr_ast_internal(c, arena);
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
        c->token_pos++;
    }
    if (expr != AST_NULL) {
        ASTRef es_ref = zero_cc_ast_alloc_node(arena, NODE_EXPR_STMT);
        ZeroCCASTNode *esn = zero_cc_ast_get(arena, es_ref);
        esn->line = line;
        esn->col = col;
        esn->left = expr;
        return es_ref;
    }

    return AST_NULL;
}

/* =========================================================================
 *  AST Dumper / Pretty Printer
 * ========================================================================= */


#endif /* ZERO_CC_PARSER_AST_STMT_H */
