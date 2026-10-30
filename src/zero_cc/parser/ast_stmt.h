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
#include "zero_cc/parser/init_common.h"

static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_assignment_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_brace_init_list_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCType lit_type, size_t base_off, size_t *leaf_cursor, size_t *out_elem_count);

#include "zero_cc/parser/zero_cc_declarator_stack.h"

/* Number of scalar leaves in one element of type t (C layout order):
 * arrays recurse (len * leaves(elem)), structs sum member leaves, scalars
 * count as 1.  Mirrors object_count_scalar_leaves() in the IR lowering, so
 * an unsized array's outermost dimension can be inferred from a flattened
 * brace-initializer leaf count. */
static inline size_t zero_cc_base_leaf_count(const ZeroCCType *t) {
    if (!t) return 1;
    if (t->tag == TYPE_ARRAY) {
        if (!t->base || t->array_len == 0) return 1;
        return t->array_len * zero_cc_base_leaf_count(t->base);
    }
    if (t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) {
        size_t n = 0;
        for (size_t i = 0; i < t->member_count; i++) {
            const ZeroCCStructMember *m = &t->members[i];
            if (m->is_flexible) continue;
            if (m->tag == TYPE_ARRAY && m->base && m->base->size > 0) {
                n += (m->size / m->base->size) * zero_cc_base_leaf_count(m->base);
            } else if (m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) {
                n += m->base ? zero_cc_base_leaf_count(m->base) : 1;
            } else {
                n++;
            }
        }
        return n ? n : 1;
    }
    return 1;
}

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
    /* Static-local string initializer: = "string" for char[] / wchar_t[] */
    if (t == TK_STRING && sym->type.tag == TYPE_ARRAY && sym->type.base &&
        ((sym->type.base->tag == TYPE_INT8 || sym->type.base->tag == TYPE_UINT8) ||
         ((sym->type.base->tag == TYPE_INT32 || sym->type.base->tag == TYPE_UINT32) && c->tokens[c->token_pos].is_wide))) {
        bool is_wide = c->tokens[c->token_pos].is_wide;
        size_t term_sz = is_wide ? 4 : 1;
        c->token_pos++;
        const char *s = c->tokens[c->token_pos - 1].allocated_str ? c->tokens[c->token_pos - 1].allocated_str : c->tokens[c->token_pos - 1].str_val;
        size_t len = zero_cc_token_str_len(&c->tokens[c->token_pos - 1]) + term_sz; /* content + null terminator */
        /* C11 6.7.9p13: a fixed-size array smaller than the string is
         * truncated (excess chars AND the NUL are dropped).  Clamp the copy
         * to the array's capacity so a fixed-size array never overruns into
         * the neighbouring static, and warn like GCC's
         * "initializer-string for array of 'char' is too long". */
        size_t esz = (sym->type.base->size > 0) ? sym->type.base->size : 1;
        size_t capacity = (sym->type.array_len > 0) ? sym->type.array_len * esz : len;
        if (len > capacity) {
            diag_report_warning_at_token(c,
                "initializer-string for array of %s is too long",
                is_wide ? "wchar_t" : (sym->type.base->tag == TYPE_UINT8 ? "unsigned char" : "char"));
        }
        size_t copy_len = (len < capacity) ? len : capacity;
        /* Ensure data section has enough capacity */
        size_t max_write = sym->static_data_offset + copy_len;
        if (c->data_section.length < max_write) {
            zero_cc_buf_ensure(&c->data_section, max_write - c->data_section.length);
        }
        uint8_t *dst = (uint8_t *)c->data_section.data + sym->static_data_offset;
        memset(dst, 0, capacity);  /* zero-fill the whole object (C11 6.7.9) */
        memcpy(dst, s, copy_len);
        /* If array was unsized, update the type size and array_len */
        if (sym->type.array_len == 0) {
            sym->type.array_len = esz > 0 ? (len / esz) : len;
            sym->type.size = len;
        }
        /* Update data section length */
        size_t written_end = sym->static_data_offset + capacity;
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
                size_t slen = zero_cc_token_str_len(&c->tokens[c->token_pos]) + 1; /* content + NUL */
                c->token_pos++;
                /* C99 6.6 pointer constant expression ("foo" + 1, c-torture
                 * pr53084): a plain integer offset after the string shifts
                 * the rodata address the reloc patches. */
                int64_t str_off = 0;
                if (c->token_pos + 1 < c->token_count &&
                    (c->tokens[c->token_pos].type == TK_PLUS || c->tokens[c->token_pos].type == TK_MINUS) &&
                    c->tokens[c->token_pos + 1].type == TK_NUM) {
                    str_off = c->tokens[c->token_pos + 1].num_val;
                    if (c->tokens[c->token_pos].type == TK_MINUS) str_off = -str_off;
                    c->token_pos += 2;
                }
                if (elem_is_ptr) {
                    /* Pointer-to-string: emit literal to .rodata, patch the
                     * 8-byte data slot via a data relocation. */
                    size_t ro_off = c->rodata_section.length;
                    zero_cc_buf_put_bytes(&c->rodata_section, sv, slen);
                    if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                        ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                        dr->data_patch_offset = sym->static_data_offset + off;
                        dr->target_kind = 0; /* .rodata target */
                        dr->rodata_offset = ro_off;
                        dr->target_data_offset = (size_t)str_off;
                        dr->target_global_idx = -1;
                    }
                    off += 8;
                } else if (elem_is_char) {
                    /* Char element: write the string bytes (incl. NUL). */
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
    if (t == TK_LBRACE && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION)) {
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
        size_t init_cursor = 0;
        zero_cc_emit_brace_init_global(c, sym->type, dst, 0, total,
                                       sym->static_data_offset, &init_cursor);
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
        if (c->tokens[brace_pos].is_wide) {
            if (elem_sz != 4) return 0;
            return zero_cc_token_str_len(&c->tokens[brace_pos]) + 4;
        }
        if (!elem_is_char) return 0;
        return zero_cc_token_str_len(&c->tokens[brace_pos]) + 1;
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
            total += (elem_is_ptr) ? 8 : (elem_is_char ? zero_cc_token_str_len(&c->tokens[q]) + 1 : ((elem_sz < 8) ? elem_sz : 8));
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
            gn->s_len = strlen(gn->s.str);
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
        ln->s_len = strlen(ln->s.str);
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
        uint32_t locals_at_entry = (uint32_t)c->local_count;

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
        if (bn) {
            bn->left = head;
            /* Block-scope metadata for the lowering pass (C11 6.2.1):
             * aux.alt = locals at block entry + 1
             * aux.step = locals at block exit + 1 (0 means unannotated).
             * The lowering caps name lookup at (aux.step - 1) inside the block,
             * and restores to (aux.alt - 1) on block exit so inner declarations
             * do not leak into following outer statements. */
            bn->aux.alt = locals_at_entry + 1;
            bn->aux.step = (uint32_t)(c->local_count + 1);
        }
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

        /* For-scope metadata (C11 6.8.5): the loop variable declared in the
         * init (e.g. `for (int i = 0; ...)`) is visible for the whole for
         * statement (init/cond/step/body) but not after it.  Record the
         * symbol count right after the init so the lowering can cap name
         * lookup for the entire for: a LATER sibling for redeclaring the
         * same name must not shadow this loop's header (aux.op = count+1,
         * 0 = no init declaration). */
        {
            ZeroCCASTNode *fn_now = zero_cc_ast_get(arena, for_ref);
            if (fn_now) fn_now->aux.op = (uint32_t)(c->local_count + 1);
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

    /* Case statement: case const_expr: stmt or case low ... high: stmt */
    if (t == TK_CASE) {
        c->token_pos++;
        ASTRef case_ref = zero_cc_ast_alloc_node(arena, NODE_CASE);
        ZeroCCASTNode *cn = zero_cc_ast_get(arena, case_ref);
        cn->line = line;
        cn->col = col;

        ASTRef val = parse_expr_ast_internal(c, arena);
        ASTRef val_high = AST_NULL;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ELLIPSIS) {
            c->token_pos++; /* consume '...' */
            val_high = parse_expr_ast_internal(c, arena);
        }
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
            cn->aux.step = (uint32_t)val_high;
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
    bool is_extern = false;

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
        } else if (c->tokens[c->token_pos].type == TK_EXTERN) {
            is_extern = true;
            c->token_pos++;
        } else {
            c->token_pos++;
        }
    }

    if (c->token_pos < c->token_count) {
        t = c->tokens[c->token_pos].type;
    }

    /* GNU vector attribute marker (`__zc_vector_size__(N)`, emitted by the
     * preprocessor from `__attribute__((__vector_size__(N)))`). It may lead
     * the declaration (before the base type specifier) - probe and consume
     * it when the next token is a type specifier so the declaration gate
     * below sees the real base type. It may also trail the base specifier
     * (consumed just before the declarator below). */
    size_t vec_attr_size = 0;
    size_t align_attr_val = 0;
    {
        size_t save = c->token_pos;
        size_t probe = 0;
        if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
            if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                vec_attr_size = probe;
                t = c->tokens[c->token_pos].type;
            } else {
                c->token_pos = save; /* not a declaration; leave tokens alone */
            }
        }
        if (vec_attr_size == 0) {
            size_t save2 = c->token_pos;
            if (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                    align_attr_val = probe;
                    t = c->tokens[c->token_pos].type;
                } else {
                    c->token_pos = save2;
                }
            }
        }
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
                if (type_tok == TK_SHORT && c->token_pos < c->token_count &&
                    c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
                if (type_tok == TK_LONG) {
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++; /* long long */
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++; /* long [long] int */
                }
            } else {
                type_tok = TK_INT;  /* bare `unsigned` / `signed` */
            }
        } else if (type_tok == TK_LONG) {
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
            if (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_UNSIGNED || c->tokens[c->token_pos].type == TK_SIGNED)) {
                decl_is_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
        } else if (type_tok == TK_SHORT) {
            if (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_UNSIGNED || c->tokens[c->token_pos].type == TK_SIGNED)) {
                decl_is_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
        }
        if (type_tok == TK_STRUCT || type_tok == TK_UNION || type_tok == TK_ENUM) {
            /* GNU aligned(N) between the tag keyword and the tag name:
             * `struct __attribute__((aligned(4))) S { ... };` - consume the
             * marker so the real tag name follows. */
            {
                size_t probe = 0;
                while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) { /* skip */ }
            }
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

        ZeroCCDeclarator decl;
        zero_cc_declarator_init(&decl);
        /* Vector attribute trailing the base specifier: `signed char
         * __attribute__((__vector_size__(16))) V;` - and aligned(N):
         * `struct x { ... } __attribute__((aligned(32))) X;` */
        if (vec_attr_size == 0) {
            size_t probe = 0;
            if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) vec_attr_size = probe;
        }
        if (align_attr_val == 0) {
            size_t probe = 0;
            if (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) align_attr_val = probe;
        }
        if (!zero_cc_parse_declarator(c, arena, ctx, &decl)) {
            return AST_NULL;
        }
        /* GNU aligned(N) trailing the declarator:
         * `short a[4] __attribute__((aligned(16)));` - the marker follows
         * the full declarator; pick it up here so the application below
         * (and the ';' / initializer that follows) parses cleanly. */
        {
            size_t probe = 0;
            while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                if (probe > align_attr_val) align_attr_val = probe;
            }
        }

        if (decl.name) {
            const char *vname = decl.name;
            bool is_function = zero_cc_declarator_is_function(&decl);

            if (is_function) {
                /* Build canonical return type and function TypeInfo */
                ZeroCCType base_ret = has_typeof_base ? typeof_base_type : zero_cc_resolve_base_type(c, type_tok, c->tokens[type_start_pos].str_val);
                if (decl_is_unsigned) {
                    if (base_ret.tag == TYPE_INT8) base_ret.tag = TYPE_UINT8;
                    else if (base_ret.tag == TYPE_INT16) base_ret.tag = TYPE_UINT16;
                    else if (base_ret.tag == TYPE_INT32) base_ret.tag = TYPE_UINT32;
                    else if (base_ret.tag == TYPE_INT64) base_ret.tag = TYPE_UINT64;
                }
                base_ret.is_const = is_const;
                base_ret.is_volatile = is_volatile;
                base_ret.is_atomic = is_atomic;
                if (vec_attr_size > 0) {
                    base_ret = zero_cc_type_vector(&base_ret, vec_attr_size);
                }

                ZeroCCType ret_type = zero_cc_declarator_apply_return(&decl, base_ret);
                /* C99 6.7.5.3: a function cannot return a function type
                 * (e.g. `func f(void);` with `typedef int func(void);`). */
                if (ret_type.is_func) {
                    diag_report_error_at_token(c, "function '%s' cannot return a function", vname);
                    return AST_NULL;
                }
                TypeInfo *ret_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, ret_type);

                const ZeroCCDeclaratorChunk *fc = zero_cc_declarator_get_function_chunk(&decl);
                if (!fc) fc = &decl.stack[0];
                ASTRef param_list = fc->param_list;
                TypeInfo *fn_ti = type_function(ctx->perm_arena, ret_ti, fc->param_count > 0 ? (TypeInfo **)fc->param_tis : NULL, fc->param_count, fc->is_variadic);
                fn_ti->details.function.is_prototype = fc->has_prototype;

                /* Function definition or prototype */
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
                    /* C99 6.9p15: function definitions may only appear at
                     * file scope. A definition inside a block is the GNU
                     * nested-function (trampoline) extension: ZeroCC does
                     * not implement closure capture, AOT would emit a call
                     * to a symbol that is never defined (link error) while
                     * JIT silently aliases the register file. Reject it
                     * cleanly and skip the body (brace-depth counted) so
                     * the enclosing function keeps parsing. Local function
                     * *prototypes* (the else branch below) remain legal C.
                     * `gcc -pedantic-errors` rejects nested definitions
                     * too, so the c-torture gate triages these as SKIP-C89
                     * (execute/nestfunc-7.c, execute/pr103405.c). */
                    diag_report_error_at_token(c,
                        "nested function definition of '%s' is not supported (GNU extension)", vname);
                    c->token_pos++; /* consume '{' */
                    int bdepth = 1;
                    while (c->token_pos < c->token_count && bdepth > 0) {
                        if (c->tokens[c->token_pos].type == TK_LBRACE) bdepth++;
                        else if (c->tokens[c->token_pos].type == TK_RBRACE) bdepth--;
                        c->token_pos++;
                    }
                    return AST_NULL;
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
                    func_node->s_len = strlen(func_node->s.str);
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
                        fn->returns_struct = (ret_type.tag == TYPE_STRUCT || ret_type.tag == TYPE_UNION ||
                                               (ret_type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&ret_type)));
                        fn->ret_struct_sz = ret_type.size;
                        fn->is_prototype = true;
                    }
                    return func_ref;
                }
            } else {
                /* Not a function: treat as variable declaration */
                if (!zero_cc_declarator_validate(c, &decl)) {
                    return AST_NULL;
                }

                bool is_array = zero_cc_declarator_has_array(&decl);
                bool has_any_vla = zero_cc_declarator_has_vla(&decl);
                ASTRef vla_dim_expr = zero_cc_declarator_vla_dim_expr(&decl);

                ZeroCCType base_decl = has_typeof_base ? typeof_base_type : zero_cc_resolve_base_type(c, type_tok, c->tokens[type_start_pos].str_val);
                if (decl_is_unsigned) {
                    if (base_decl.tag == TYPE_INT8) base_decl.tag = TYPE_UINT8;
                    else if (base_decl.tag == TYPE_INT16) base_decl.tag = TYPE_UINT16;
                    else if (base_decl.tag == TYPE_INT32) base_decl.tag = TYPE_UINT32;
                    else if (base_decl.tag == TYPE_INT64) base_decl.tag = TYPE_UINT64;
                }
                base_decl.is_const = is_const;
                base_decl.is_volatile = is_volatile;
                base_decl.is_atomic = is_atomic;
                if (vec_attr_size > 0) {
                    base_decl = zero_cc_type_vector(&base_decl, vec_attr_size);
                }
                if (align_attr_val > 0) {
                    /* GNU aligned(N): raise the type's alignment to N (C:
                     * sizeof must then be a multiple of N). For struct/union
                     * types the canonical size/align live in the registry
                     * entry, so mutate it there (all later uses of the type
                     * see the padded size) and refresh the local copy. */
                    if (base_decl.tag == TYPE_STRUCT || base_decl.tag == TYPE_UNION) {
                        ZeroCCType *st = zero_cc_find_struct(c, base_decl.struct_name);
                        if (!st) st = zero_cc_find_typedef(c, base_decl.struct_name);
                        if (st && align_attr_val > st->align) {
                            st->align = align_attr_val;
                            if (st->size < align_attr_val) st->size = align_attr_val;
                            if ((st->size % align_attr_val) != 0) {
                                st->size = (st->size + align_attr_val - 1) / align_attr_val * align_attr_val;
                            }
                            base_decl = *st;
                        }
                    } else if (align_attr_val > base_decl.align) {
                        base_decl.align = align_attr_val;
                        if (base_decl.size < align_attr_val) base_decl.size = align_attr_val;
                        if ((base_decl.size % align_attr_val) != 0) {
                            base_decl.size = (base_decl.size + align_attr_val - 1) / align_attr_val * align_attr_val;
                        }
                    }
                }

                ZeroCCType decl_type = zero_cc_declarator_apply(&decl, base_decl);
                /* C99 6.7.2.1: objects cannot have function type
                 * (e.g. `func f;` with `typedef int func(void);`). */
                if (decl_type.is_func) {
                    diag_report_error_at_token(c, "cannot declare variable '%s' with function type", vname);
                    return AST_NULL;
                }

                /* C99 6.7.9: an unsized array's outermost dimension is
                 * inferred from its initializer. Measure it (without
                 * consuming tokens) BEFORE zero_cc_add_local so the stack
                 * slot and the static-local data reservation are sized to
                 * the real object (not the 8-byte minimum), preventing the
                 * per-call mirror from overflowing into a neighbour. */
                if (is_array && !has_any_vla &&
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
                int32_t prev_frame = c->current_stack_frame;
                ZeroCCSym *sym = zero_cc_add_local(c, vname, decl_type);
                if (!sym) {
                    diag_report_error_at_token(c, "out of memory allocating local variable '%s'", vname);
                    return AST_NULL;
                }
                if (is_extern) {
                    sym->is_extern = true;
                    sym->stack_offset = 0;
                    c->current_stack_frame = prev_frame;
                    if (!find_global_var(vname) && !zero_cc_find_function(c, vname) && c->function_count < ZERO_CC_MAX_FUNCTIONS) {
                        ZeroCCFunction *fn = &c->functions[c->function_count++];
                        fn->text_offset = 0;
                        snprintf(fn->name, sizeof(fn->name), "%s", vname);
                        fn->return_type = decl_type;
                        fn->is_external = true;
                        fn->is_prototype = true;
                        fn->is_object = true;
                    }
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
                dn->s_len = strlen(dn->s.str);
                /* Stash the exact local symbol for lowering: sibling blocks
                 * may legally reuse the name, and by-name lookup is
                 * latest-wins, so type-dependent initializers must resolve
                 * the symbol of this declaration (see lowering_local_sym). */
                if (sym) dn->aux.step = (uint32_t)((size_t)(sym - c->locals)) + 1;
                if (has_any_vla) {
                    dn->flags |= NODE_DECL_IS_VLA;
                    dn->right = vla_dim_expr; /* outermost runtime dimension expr */
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
                    /* C11 6.7.9p13: a whole-string initializer for a
                     * fixed-size char array that is too short is truncated
                     * (the excess characters AND the terminating NUL are
                     * dropped).  Warn like GCC's
                     * "initializer-string for array of 'char' is too long". */
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STRING &&
                        decl_type.tag == TYPE_ARRAY && decl_type.array_len > 0 && decl_type.base &&
                        (decl_type.base->tag == TYPE_INT8 || decl_type.base->tag == TYPE_UINT8)) {
                        size_t need = zero_cc_token_str_len(&c->tokens[c->token_pos]) + 1;  /* string + implicit NUL */
                        size_t esz  = (decl_type.base->size > 0) ? decl_type.base->size : 1;
                        if (need > decl_type.array_len * esz) {
                            diag_report_warning_at_token(c,
                                "initializer-string for array of %s is too long",
                                decl_type.base->tag == TYPE_UINT8 ? "unsigned char" : "char");
                        }
                    }
                    ASTRef init_val = AST_NULL;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
                        /* Brace-enclosed / designated initializer: struct Point p = { .x = 10 }; */
                        size_t elem_count = 0;
                        size_t decl_init_cursor = 0;
                        ASTRef head_expr = parse_brace_init_list_ast(c, arena, decl_type, 0,
                                                                     &decl_init_cursor, &elem_count);
                        if (decl_type.tag == TYPE_ARRAY && decl_type.array_len == 0 && elem_count > 0) {
                            /* C99 6.7.9: infer the outermost dimension.  The
                             * flattened brace list holds LEAF nodes; an element
                             * may span several leaves (char rows, struct
                             * members), so divide by the leaves-per-element. */
                            size_t inferred_len = elem_count;
                            if (decl_type.base) {
                                size_t per_leaf = zero_cc_base_leaf_count(decl_type.base);
                                if (per_leaf > 1 && elem_count >= per_leaf) {
                                    inferred_len = elem_count / per_leaf;
                                }
                            }
                            decl_type.array_len = inferred_len;
                            if (decl_type.base && decl_type.base->size > 0) {
                                decl_type.size = decl_type.base->size * inferred_len;
                            }
                            if (sym) {
                                sym->type.array_len = inferred_len;
                                if (sym->type.base && sym->type.base->size > 0) {
                                    sym->type.size = sym->type.base->size * inferred_len;
                                }
                            }
                            if (decl_ti && decl_ti->kind == TYPE_KIND_ARRAY) {
                                decl_ti->details.array.count = inferred_len;
                            }
                        }
                        ASTRef comp_ref = zero_cc_ast_alloc_node(arena, NODE_COMPOUND_LIT);
                        ZeroCCASTNode *cn = zero_cc_ast_get(arena, comp_ref);
                        cn->left = head_expr;
                        if (sym) {
                            cn->s.str = sym->name;
                            cn->s_len = strlen(sym->name);
                            cn->aux.step = (uint32_t)((size_t)(sym - c->locals)) + 1;
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
                    ZeroCCDeclarator next_decl;
                    zero_cc_declarator_init(&next_decl);
                    if (!zero_cc_parse_declarator(c, arena, ctx, &next_decl) || !next_decl.name) {
                        break;
                    }
                    if (!zero_cc_declarator_validate(c, &next_decl)) {
                        return AST_NULL;
                    }

                    const char *next_vname = next_decl.name;
                    ZeroCCType next_base = has_typeof_base ? typeof_base_type : zero_cc_resolve_base_type(c, type_tok, c->tokens[type_start_pos].str_val);
                    if (decl_is_unsigned) {
                        if (next_base.tag == TYPE_INT8) next_base.tag = TYPE_UINT8;
                        else if (next_base.tag == TYPE_INT16) next_base.tag = TYPE_UINT16;
                        else if (next_base.tag == TYPE_INT32) next_base.tag = TYPE_UINT32;
                        else if (next_base.tag == TYPE_INT64) next_base.tag = TYPE_UINT64;
                    }
                    next_base.is_const = is_const;
                    next_base.is_volatile = is_volatile;
                    next_base.is_atomic = is_atomic;
                    if (vec_attr_size > 0) {
                        next_base = zero_cc_type_vector(&next_base, vec_attr_size);
                    }

                    ZeroCCType next_type = zero_cc_declarator_apply(&next_decl, next_base);
                    bool nhas_any_vla = zero_cc_declarator_has_vla(&next_decl);
                    ASTRef nvla_dim_expr = zero_cc_declarator_vla_dim_expr(&next_decl);

                    TypeInfo *next_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, next_type);
                    int32_t prev_next_frame = c->current_stack_frame;
                    ZeroCCSym *next_sym = zero_cc_add_local(c, next_vname, next_type);
                    if (!next_sym) {
                        diag_report_error_at_token(c, "out of memory allocating local variable '%s'", next_vname);
                        return AST_NULL;
                    }
                    if (is_extern) {
                        next_sym->is_extern = true;
                        next_sym->stack_offset = 0;
                        c->current_stack_frame = prev_next_frame;
                        if (!find_global_var(next_vname) && !zero_cc_find_function(c, next_vname) && c->function_count < ZERO_CC_MAX_FUNCTIONS) {
                            ZeroCCFunction *fn = &c->functions[c->function_count++];
                            fn->text_offset = 0;
                            snprintf(fn->name, sizeof(fn->name), "%s", next_vname);
                            fn->return_type = next_type;
                            fn->is_external = true;
                            fn->is_prototype = true;
                            fn->is_object = true;
                        }
                    }

                    ASTRef next_decl_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                    ZeroCCASTNode *next_dn = zero_cc_ast_get(arena, next_decl_ref);
                    next_dn->line = line;
                    next_dn->col = col;
                    next_dn->s.str = next_vname;
                    next_dn->s_len = strlen(next_vname);
                    if (next_sym) next_dn->aux.step = (uint32_t)((size_t)(next_sym - c->locals)) + 1;
                    if (nhas_any_vla) {
                        next_dn->flags |= NODE_DECL_IS_VLA;
                        next_dn->right = nvla_dim_expr; /* outermost runtime dimension expr */
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
                        ASTRef next_init = AST_NULL;
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
                            size_t elem_count = 0;
                            size_t next_init_cursor = 0;
                            ASTRef head_expr = parse_brace_init_list_ast(c, arena, next_type, 0,
                                                                         &next_init_cursor, &elem_count);
                            if (next_type.tag == TYPE_ARRAY && next_type.array_len == 0 && elem_count > 0) {
                                size_t inferred_len = elem_count;
                                if (next_type.base) {
                                    size_t per_leaf = zero_cc_base_leaf_count(next_type.base);
                                    if (per_leaf > 1 && elem_count >= per_leaf) {
                                        inferred_len = elem_count / per_leaf;
                                    }
                                }
                                next_type.array_len = inferred_len;
                                if (next_type.base && next_type.base->size > 0) {
                                    next_type.size = next_type.base->size * inferred_len;
                                }
                                if (next_sym) {
                                    next_sym->type.array_len = inferred_len;
                                    if (next_sym->type.base && next_sym->type.base->size > 0) {
                                        next_sym->type.size = next_sym->type.base->size * inferred_len;
                                    }
                                }
                                if (next_ti && next_ti->kind == TYPE_KIND_ARRAY) {
                                    next_ti->details.array.count = inferred_len;
                                }
                            }
                            ASTRef comp_ref = zero_cc_ast_alloc_node(arena, NODE_COMPOUND_LIT);
                            ZeroCCASTNode *cn = zero_cc_ast_get(arena, comp_ref);
                            cn->left = head_expr;
                            if (next_sym) {
                                cn->s.str = next_sym->name;
                                cn->s_len = strlen(next_sym->name);
                                cn->aux.step = (uint32_t)((size_t)(next_sym - c->locals)) + 1;
                            }
                            next_init = comp_ref;
                        } else {
                            next_init = parse_assignment_ast(c, arena);
                        }
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
        } else {
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                c->token_pos++;
            }
            return AST_NULL;
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
