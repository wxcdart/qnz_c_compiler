#ifndef ZERO_CC_PARSER_DECL_LOCAL_H
#define ZERO_CC_PARSER_DECL_LOCAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types/types.h"
#include "zero_cc/isa/tramp_stub.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/jit/type_info.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/parser/definitions.h"
#include "zero_cc/parser/init_common.h"
#include "zero_cc/parser/zero_cc_declarator_stack.h"

/* Forward declarations for AST expressions */
static ASTRef parse_assignment_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static ASTRef parse_brace_init_list_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCType lit_type, size_t base_off, size_t *leaf_cursor, size_t *out_elem_count);
/* K&R parameter-declaration parser (Sprint 65) - defined in jit/compile.h
 * (later in this translation unit); reused by the nested K&R path below. */
static int zero_cc_parse_kr_decl(ZeroCCCompiler *c, const char **out_names, ZeroCCType *out_types, int max);

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
    /* An initializer (even a brace aggregate) keeps the object in .data:
     * has_init=false routes >=256B statics to .bss and the baked bytes
     * are silently zeroed at runtime (c-torture execute/ieee/fp-cmp-8:
     * `static struct try const data[14] = { NAN, ... };` read as 0.0). */
    bool has_init = (c->token_pos < c->token_count &&
                     c->tokens[c->token_pos].type == TK_ASSIGN);
    if (add_global_var(c, mangled, t, off, alloc, true, has_init) < 0) {
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
    /* Aggregate brace initializer (C99 6.7.9): `static struct S d[] =
     * { {...}, ... };` / `static const int tab[] = { 1, 2, 3 };` - run the
     * global brace walker directly into the reserved data region (c-torture
     * execute/ieee/fp-cmp-4/5/8 family, pr38016, loop-ivopts-1).  The
     * region was zero-filled at register time, so elided leaves stay zero. */
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE &&
        (sym->type.tag == TYPE_ARRAY || sym->type.tag == TYPE_STRUCT ||
         sym->type.tag == TYPE_UNION)) {
        size_t total = (sym->type.size > 0) ? sym->type.size : 8;
        if (sym->static_data_offset + total > c->data_section.length) return false;
        size_t cursor = 0;
        /* buf_off carries the object's base (writes land directly in the
         * data section), so data_patch_off must be 0 - the walker makes
         * relocs at data_patch_off + abs_off where abs_off is already
         * buffer-absolute. */
        zero_cc_emit_brace_init_global(c, sym->type, c->data_section.data,
                                       sym->static_data_offset, c->data_section.length,
                                       0, &cursor, 0);
        return true;
    }
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
        /* C99 6.7.2.1p3: a trailing flexible-array member is initialized
         * past the struct's fixed size - grow the object to cover it. */
        size_t fam_extra = zc_measure_fam_init_bytes(c, &sym->type);
        if (fam_extra > 0) total += fam_extra;
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
                                       sym->static_data_offset, &init_cursor,
                                       fam_extra);
        return true;
    }
    /* Static-local pointer: `static int *hx = gx;` (an array global decays
     * to its address), `static const char *p = "x";`, `static T *q = &g;`
     * (c-torture execute/builtin-prefetch-2) - bake an 8-byte slot patched
     * by a data reloc. */
    if (sym->type.tag == TYPE_PTR) {
        size_t save = c->token_pos;
        bool ok = false;
        if (c->token_pos < c->token_count &&
            (c->tokens[c->token_pos].type == TK_AMP ||
             c->tokens[c->token_pos].type == TK_LOGAND ||
             c->tokens[c->token_pos].type == TK_IDENT ||
             c->tokens[c->token_pos].type == TK_STRING)) {
            ZeroCCDataReloc *dr = NULL;
            if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                dr = &c->data_relocs[c->data_reloc_count];
                memset(dr, 0, sizeof(*dr));
                dr->data_patch_offset = sym->static_data_offset;
                dr->rodata_offset = 0;
                dr->target_data_offset = 0;
                dr->target_global_idx = -1;
            }
            size_t cap_end = sym->static_data_offset + 8;
            if (c->data_section.length < cap_end) {
                zero_cc_buf_ensure(&c->data_section, cap_end - c->data_section.length);
                c->data_section.length = cap_end;
            }
            if (dr) dr->data_patch_offset = sym->static_data_offset;
            if (c->tokens[c->token_pos].type == TK_LOGAND &&
                c->token_pos + 1 < c->token_count &&
                c->tokens[c->token_pos + 1].type == TK_IDENT) {
                /* GNU label value: `static void *p = &&l;` - the slot is
                 * patched to the label's text offset (kind-5 data reloc,
                 * resolved after the enclosing function's IR lowering). */
                snprintf(dr->target_extern_name, sizeof(dr->target_extern_name),
                         "%s", c->tokens[c->token_pos + 1].str_val);
                c->token_pos += 2;
                if (dr) {
                    dr->target_kind = 5;
                    dr->target_data_offset = 0;
                    dr->target_global_idx = (c->function_count > 0) ?
                        (int)(c->function_count - 1) : 0;
                }
                ok = true;
            } else if (c->tokens[c->token_pos].type == TK_AMP) {
                c->token_pos++;
                if (c->token_pos < c->token_count &&
                    c->tokens[c->token_pos].type == TK_IDENT) {
                    char nm[128];
                    snprintf(nm, sizeof(nm), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                    ZeroCCGlobalVar *gv = find_global_var(c, nm);
                    if (gv) {
                        if (dr) {
                            dr->target_kind = 1;
                            dr->target_global_idx = (int)(gv - c->global_vars);
                        }
                        ok = true;
                    }
                }
            } else if (c->tokens[c->token_pos].type == TK_STRING) {
                const char *sv = c->tokens[c->token_pos].allocated_str ?
                    c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                size_t slen = zero_cc_token_str_len(&c->tokens[c->token_pos]) + 1;
                c->token_pos++;
                size_t ro_off = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, sv, slen);
                if (dr) {
                    dr->target_kind = 0;
                    dr->rodata_offset = ro_off;
                }
                ok = true;
            } else {
                char nm[128];
                snprintf(nm, sizeof(nm), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                /* Address of an in-TU static local (e.g. `static int *hx = gx;`):
                 * kind-4 data-reloc with an absolute .data offset. */
                for (size_t li = c->local_count; li > 0; li--) {
                    ZeroCCSym *ls = &c->locals[li - 1];
                    if (strcmp(ls->name, nm) == 0 && ls->is_static_local) {
                        if (dr) {
                            dr->target_kind = 4;
                            dr->target_data_offset = ls->static_data_offset;
                        }
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    ZeroCCGlobalVar *gv = find_global_var(c, nm);
                    if (gv) {
                        if (dr) {
                            dr->target_kind = 1;
                            dr->target_global_idx = (int)(gv - c->global_vars);
                        }
                        ok = true;
                    } else {
                        /* Cross-TU extern (declared in the function table) or
                         * a function designator. */
                        bool ext = false;
                        for (size_t fi = 0; fi < c->function_count; fi++) {
                            if (strcmp(c->functions[fi].name, nm) == 0) { ext = true; break; }
                        }
                        if (ext) {
                            if (dr) {
                                dr->target_kind = 3; /* function/extern symbol by name */
                                snprintf(dr->target_extern_name, sizeof(dr->target_extern_name), "%s", nm);
                            }
                            ok = true;
                        }
                    }
                }
            }
            if (ok && dr) {
                c->data_reloc_count++;
                uint8_t *dst = (uint8_t *)c->data_section.data + sym->static_data_offset;
                memset(dst, 0, 8);
                return true;
            }
        }
        c->token_pos = save;
        return false;
    }
    if (is_float) {
        /* Full constant expression (casts + global references, e.g.
         * `static const double k = (double) a;` - c-torture compile/
         * pr83222). */
        size_t start = c->token_pos;
        double dv = zero_cc_eval_const_expr_float(c);
        if (c->token_pos != start) {
            ZeroCCTokType after = (c->token_pos < c->token_count) ? c->tokens[c->token_pos].type : TK_EOF;
            if (after != TK_SEMI) { c->token_pos = start; return false; }
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
            size_t cap_end = sym->static_data_offset + width;
            if (c->data_section.length < cap_end) {
                zero_cc_buf_ensure(&c->data_section, cap_end - c->data_section.length);
                c->data_section.length = cap_end;
            }
            for (size_t i = 0; i < width; i++) dst[i] = (uint8_t)(bits >> (8 * i));
            return true;
        }
        return false;
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
            /* Each nested group initializes ONE FULL element (e.g. one 16-byte
             * struct row) - charge the full element size, not an 8-byte cap
             * (that capped size made `struct S a[] = { {..}, {..} }` infer
             * too few rows for elements > 8 bytes). */
            total += elem_sz;
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


static inline bool zero_cc_parse_local_decl(ZeroCCCompiler *c,
                                            ZeroCCASTArena *arena,
                                            SemContext *ctx,
                                            uint32_t line,
                                            uint32_t col,
                                            ASTRef *out_ref)
{
    if (!c || !arena || c->token_pos >= c->token_count) return false;
    size_t decl_save_pos = c->token_pos;
    ZeroCCTokType t = c->tokens[c->token_pos].type;
    /* GNU `__label__ x;` (Sprint 99): a local of the label-value type.
     * The variable is a plain int64 local; lowering materializes the
     * address of the label named x (like &&x) into its slot, and
     * `goto x;` resolves through it (possibly to an ancestor function -
     * nonlocal goto). */
    if (t == TK_LABEL) {
        c->token_pos++; /* __label__ */
        if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) {
            diag_report_error_at_token(c, "expected identifier after '__label__'");
            return true;
        }
        const char *lname = c->tokens[c->token_pos].str_val;
        c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
            c->token_pos++;
        } else {
            diag_report_error_at_token(c, "expected ';' after '__label__' declaration");
            while (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
            if (c->token_pos < c->token_count) c->token_pos++;
            return true;
        }
        ZeroCCType lt = zero_cc_type_int64();
        ZeroCCSym *sym = zero_cc_add_local(c, lname, lt);
        if (!sym) {
            diag_report_error_at_token(c, "out of memory allocating local variable '%s'", lname);
            *out_ref = AST_NULL; return true;
        }
        sym->is_label_value = true;
        ASTRef decl_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
        ZeroCCASTNode *dn = zero_cc_ast_get(arena, decl_ref);
        dn->line = line;
        dn->col = col;
        dn->s.str = zero_cc_intern_string(c, lname);
        dn->s_len = strlen(dn->s.str);
        dn->aux.step = (uint32_t)((size_t)(sym - c->locals)) + 1;
        decl_node_set_type(ctx, dn, zero_cc_type_to_typeinfo(ctx->perm_arena, lt));
        *out_ref = decl_ref;
        return true;
    }
    /* Variable Declaration or Typedef Declaration */
    /* Parse alignment, storage specifiers & type qualifiers */
    bool is_const = false;
    bool is_volatile = false;
    bool is_atomic = false;
    bool is_static_local = false;   /* Sprint 37: block-scope `static` */
    bool is_extern = false;
    size_t c11_align_spec = 0;      /* C11 `_Alignas(N)` - align only, no size change */

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
                    if ((size_t)align_val > c11_align_spec) c11_align_spec = (size_t)align_val;
                    /* Alignments above 16 are guaranteed at lowering time by
                     * the runtime SP realign prologue (x86-64) - see the
                     * frame-size note in zero_cc_lower_ast, which also warns
                     * when the function is ineligible (VLA/variadic/other
                     * ISA) and falls back to 16. */
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
        if (vec_attr_size == 0 && align_attr_val == 0) {
            size_t save3 = c->token_pos;
            size_t probe2 = 0;
            if (zero_cc_parse_mode_attr(c, &c->token_pos, &probe2)) {
                /* Gate-only: consume the marker so the declaration gate
                 * below sees the real base type. The width is not applied
                 * in this path (mode-on-local-decl was unsupported before
                 * marker emission, so ignoring it preserves parity). */
                if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                    t = c->tokens[c->token_pos].type;
                } else {
                    c->token_pos = save3;
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
        *out_ref = AST_NULL; return true;
    }

    /* C89 implicit-int local declaration: `register length;` / `x;` -
     * no type specifier; the identifier is the variable name and the
     * base defaults to int.  Force the base to int and skip consuming
     * the name token (the declarator parser owns it). */
    bool implicit_int_local = false;
    /* A name already declared (global, extern object, or function) makes
     * `name = expr;` an ASSIGNMENT, not an implicit-int declaration -
     * extern variables live in the function table as is_object entries,
     * so probe it too (c-torture driver float2: `ratio = ratio * v;`
     * with `extern double ratio;` mis-parsed as a shadowing declaration). */
    bool name_already_declared = !!(find_global_var(c, c->tokens[c->token_pos].str_val) ||
        zero_cc_find_local(c, c->tokens[c->token_pos].str_val));
    if (!name_already_declared) {
        for (size_t fi = 0; fi < c->function_count; fi++) {
            if (strcmp(c->functions[fi].name, c->tokens[c->token_pos].str_val) == 0) {
                name_already_declared = true;
                break;
            }
        }
    }
    if (t == TK_IDENT &&
        !zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val) &&
        !zero_cc_find_struct(c, c->tokens[c->token_pos].str_val) &&
        !zero_cc_find_enum(c, c->tokens[c->token_pos].str_val) &&
        !name_already_declared &&
        !zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
        size_t ln = c->token_pos + 1;
        if (ln < c->token_count &&
            (c->tokens[ln].type == TK_SEMI || c->tokens[ln].type == TK_COMMA ||
             c->tokens[ln].type == TK_ASSIGN || c->tokens[ln].type == TK_LBRACKET)) {
            t = TK_INT;
            implicit_int_local = true;
        }
    }
    if (is_type_specifier(c, t)) {
        size_t type_start_pos = c->token_pos;
        ZeroCCTokType type_tok = t;
        bool decl_is_unsigned = false;
        bool decl_is_long_double = false;
        bool suffix_complex = false;
        ZeroCCType typeof_base_type_early;
        memset(&typeof_base_type_early, 0, sizeof(typeof_base_type_early));
        /* C99 6.7.2.3 prefix complex: `_Complex double x;` - consume the
         * specifier and parse the rest as a normal type name. */
        bool prefix_complex = (t == TK_IDENT && c->token_pos < c->token_count &&
            zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].allocated_str ?
                                        c->tokens[c->token_pos].allocated_str
                                        : c->tokens[c->token_pos].str_val));
        if (prefix_complex) {
            c->token_pos++;
            ZeroCCType cx_inner;
            if (zero_cc_token_is_complex_inner_base(c, c->token_pos)) {
                cx_inner = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
                /* The type-name parser also consumes pointer suffixes
                 * (`double *` in `_Complex double *c`), but for a local
                 * declaration the declarator stack below must own those
                 * stars: `_Complex double *c` is a POINTER TO complex, not
                 * a complex of pointers.  Walk the position back over the
                 * consumed [star (qualifiers|attrs)]* suffix - the base
                 * specifier's last token is always a type keyword, so the
                 * walk stops exactly at its end. */
                while (c->token_pos > 0 &&
                       (c->tokens[c->token_pos - 1].type == TK_STAR ||
                        c->tokens[c->token_pos - 1].type == TK_CONST ||
                        c->tokens[c->token_pos - 1].type == TK_VOLATILE ||
                        c->tokens[c->token_pos - 1].type == TK_RESTRICT)) {
                    c->token_pos--;
                }
                /* The parser returned the suffix-included type (e.g. ptr to
                 * double); the complex element is the scalar base only. */
                while (cx_inner.tag == TYPE_PTR && cx_inner.base) {
                    cx_inner = *cx_inner.base;
                }
            } else {
                /* Standalone `_Complex` (no inner base) means `_Complex
                 * double` (GNU); the declarator - possibly `*`-led, as in
                 * `_Complex *c` - is left for the declarator stack. */
                cx_inner = zero_cc_type_double();
            }
            typeof_base_type_early = zero_cc_type_complex(&cx_inner);
        } else if (!implicit_int_local) {
            c->token_pos++;
        }
        ZeroCCType typeof_base_type;
        memset(&typeof_base_type, 0, sizeof(typeof_base_type));
        bool has_typeof_base = prefix_complex;
        if (prefix_complex) {
            typeof_base_type = typeof_base_type_early;
        }
        ZeroCCType anon_struct_type;
        memset(&anon_struct_type, 0, sizeof(anon_struct_type));
        bool has_anon_struct = false;
        /* The declaration defines a block-scope struct/union with VLA
         * members: the lowering must capture the member dimensions at
         * definition time (NODE_DECL_VLA_STRUCT_DEF). */
        bool defines_vla_struct = false;
        char defines_vla_struct_tag[128] = {0};
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
            } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT &&
                       zc_name_int128_kind(c->tokens[c->token_pos].allocated_str ?
                                           c->tokens[c->token_pos].allocated_str
                                           : c->tokens[c->token_pos].str_val) != 0) {
                /* GNU `unsigned __int128` / `signed __int128` / redundant
                 * `unsigned __uint128_t`: the 128-bit base follows the
                 * qualifier (Sprint 113 - c-torture int128 bucket). */
                type_tok = TK_IDENT;
                type_start_pos = c->token_pos;
                c->token_pos++;
            } else {
                type_tok = TK_INT;  /* bare `unsigned` / `signed` */
            }
        } else if (type_tok == TK_LONG) {
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_DOUBLE_KW) {
                /* `long double` (C99 6.2.5): model as the 16-byte
                 * is_long_double type, not a plain double (c-torture
                 * execute/ieee/copysign1/2: sizeof(long double) locals
                 * and full-size memcmps). */
                type_start_pos = c->token_pos;
                type_tok = TK_DOUBLE_KW;
                decl_is_long_double = true;
                c->token_pos++;
            }
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
        /* C99 6.7.2.1: a block-scope declaration may define its aggregate
         * type inline: `struct S { int x; } v;` / `union { int a; } w;`.
         * Parse the body NOW (it precedes the declarator), register it,
         * and use the definition as this declaration's base type.
         * (File-scope definitions go through zero_cc_stmt_typedef_struct.) */
        if ((type_tok == TK_STRUCT || type_tok == TK_UNION) &&
            c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
            ZeroCCType st = zero_cc_parse_struct_body(c, type_tok == TK_UNION);
            char tag[128];
            if (c->tokens[type_start_pos].type == TK_IDENT) {
                snprintf(tag, sizeof(tag), "%s", c->tokens[type_start_pos].str_val);
            } else {
                snprintf(tag, sizeof(tag), "__anon_%s_%zu",
                         type_tok == TK_UNION ? "union" : "struct", c->anon_struct_seq++);
            }
            snprintf(st.struct_name, sizeof(st.struct_name), "%s", tag);
            zero_cc_register_struct(c, tag, st);
            has_anon_struct = true;
            anon_struct_type = st;
            if (st.is_vla_struct) {
                defines_vla_struct = true;
                snprintf(defines_vla_struct_tag, sizeof(defines_vla_struct_tag), "%s", tag);
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
        /* C99 6.7.2.3 suffix complex: `double _Complex x;` */
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT &&
            !prefix_complex) {
            const char *cn = c->tokens[c->token_pos].allocated_str ?
                             c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
            if (cn && zero_cc_tok_is_complex_spec(cn)) {
                c->token_pos++;
                suffix_complex = true;
            }
        }

        ZeroCCDeclarator decl;
        zero_cc_declarator_init(&decl);
        /* Sprint 98: K&R (old-style) nested function probe.
         * `type name(a, b) type a; type b; { ... }` - the declarator
         * parser treats each bare identifier in the parameter list as an
         * (unnamed) type, so the K&R declaration block after `)` is never
         * consumed and the definition is mis-parsed. Detect the shape
         * BEFORE the declarator runs and stash the bare parameter names;
         * the is_function branch below overwrites the parsed function
         * chunk and consumes the declaration block. */
        bool kr_nested = false;
        int kr_nested_count = 0;
        const char *kr_nested_names[16];
        size_t kr_name_tok = 0;
        {
            size_t kp = c->token_pos;
            if (kp < c->token_count && c->tokens[kp].type == TK_IDENT &&
                kp + 1 < c->token_count && c->tokens[kp + 1].type == TK_LPAREN) {
                size_t q = kp + 2;
                int d = 1;
                int bare = 0;
                bool shape = true;
                while (q < c->token_count && d > 0) {
                    ZeroCCTokType qt = c->tokens[q].type;
                    if (qt == TK_LPAREN) { shape = false; break; }
                    else if (qt == TK_RPAREN) { d--; if (d == 0) break; }
                    else if (qt == TK_COMMA) { }
                    else if (qt == TK_IDENT) {
                        /* Bare = not a typedef/struct/enum type name, so
                         * `type name` prototypes (f(four_quarters j)) are
                         * not misread as K&R parameter lists. */
                        if (zero_cc_find_typedef(c, c->tokens[q].str_val) ||
                            zero_cc_find_struct(c, c->tokens[q].str_val) ||
                            zero_cc_find_enum(c, c->tokens[q].str_val)) {
                            shape = false;
                            break;
                        }
                        bare++;
                    }
                    else { shape = false; break; }
                    q++;
                }
                if (shape && d == 0 && bare >= 1 && q < c->token_count &&
                    c->tokens[q - 1].type == TK_IDENT) {
                    size_t after = q + 1;
                    while (after < c->token_count &&
                           (c->tokens[after].type == TK_REGISTER ||
                            c->tokens[after].type == TK_STATIC ||
                            c->tokens[after].type == TK_AUTO ||
                            c->tokens[after].type == TK_EXTERN ||
                            c->tokens[after].type == TK_CONST ||
                            c->tokens[after].type == TK_VOLATILE ||
                            c->tokens[after].type == TK_RESTRICT)) after++;
                    ZeroCCTokType at = (after < c->token_count) ? c->tokens[after].type : TK_EOF;
                    bool decl_or_body = (at == TK_INT || at == TK_CHAR || at == TK_SHORT || at == TK_LONG ||
                                          at == TK_SIGNED || at == TK_UNSIGNED || at == TK_FLOAT_KW ||
                                          at == TK_DOUBLE_KW || at == TK_VOID ||
                                          at == TK_STRUCT || at == TK_UNION || at == TK_ENUM ||
                                          at == TK_TYPEOF || at == TK_TYPEOF_UNQUAL ||
                                          at == TK_CONST || at == TK_VOLATILE || at == TK_RESTRICT ||
                                          at == TK_LBRACE ||
                                          (at == TK_IDENT && (zero_cc_find_typedef(c, c->tokens[after].str_val) ||
                                           zero_cc_find_struct(c, c->tokens[after].str_val) ||
                                           zero_cc_find_enum(c, c->tokens[after].str_val))));
                    if (decl_or_body) {
                        kr_nested = true;
                        kr_name_tok = kp;
                        size_t r = kp + 2;
                        while (r < q) {
                            if (c->tokens[r].type == TK_IDENT && kr_nested_count < 16) {
                                kr_nested_names[kr_nested_count++] = zero_cc_intern_string(c, c->tokens[r].str_val);
                            }
                            r++;
                        }
                    }
                }
            }
        }
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
            *out_ref = AST_NULL; return true;
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
                if (decl_is_long_double && base_ret.tag == TYPE_DOUBLE) {
                    base_ret = zero_cc_type_long_double();
                }
                if (suffix_complex && !zero_cc_type_is_complex(&base_ret)) {
                    base_ret = zero_cc_type_complex(&base_ret);
                }
                if (decl_is_unsigned) {
                    if (base_ret.tag == TYPE_INT8) base_ret.tag = TYPE_UINT8;
                    else if (base_ret.tag == TYPE_INT16) base_ret.tag = TYPE_UINT16;
                    else if (base_ret.tag == TYPE_INT32) base_ret.tag = TYPE_UINT32;
                    else if (base_ret.tag == TYPE_INT64) base_ret.tag = TYPE_UINT64;
                    else if (base_ret.tag == TYPE_INT128) base_ret.tag = TYPE_UINT128;
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
                    *out_ref = AST_NULL; return true;
                }
                TypeInfo *ret_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, ret_type);

                const ZeroCCDeclaratorChunk *fc = zero_cc_declarator_get_function_chunk(&decl);
                if (!fc) fc = &decl.stack[0];
                /* Sprint 98: K&R nested function - the declarator parsed the
                 * bare-identifier list as unnamed (int) parameters. Rebuild
                 * the function chunk from the probed names and consume the
                 * K&R declaration block (which the declarator left in the
                 * stream) so the body `{` follows. */
                if (kr_nested) {
                    ZeroCCDeclaratorChunk kr_fc;
                    memset(&kr_fc, 0, sizeof(kr_fc));
                    kr_fc.kind = ZERO_CC_DECL_CHUNK_FUNCTION;
                    kr_fc.param_count = (size_t)kr_nested_count;
                    kr_fc.has_prototype = false;
                    kr_fc.is_variadic = false;
                    bool kr_matched[16];
                    for (int mi = 0; mi < 16; mi++) kr_matched[mi] = false;
                    for (int pi = 0; pi < kr_nested_count; pi++) {
                        ZeroCCType kt = zero_cc_type_int(); /* int default (C89) */
                        kr_fc.param_tis[pi] = zero_cc_type_to_typeinfo(ctx->perm_arena, kt);
                    }
                    /* The declarator parsed each bare identifier as an
                     * (unnamed) type and consumed the FIRST keyword of the
                     * K&R declaration block as a bogus parameter name.  The
                     * tokens are the whole file: re-locate the matching `)`
                     * of the parameter list and rewind the stream to just
                     * past it, so the declaration block parses intact. */
                    {
                        size_t rr = kr_name_tok + 2;
                        int dd = 1;
                        while (rr < c->token_count && dd > 0) {
                            if (c->tokens[rr].type == TK_LPAREN) dd++;
                            else if (c->tokens[rr].type == TK_RPAREN) dd--;
                            rr++;
                        }
                        c->token_pos = rr;
                    }
                    while (c->token_pos < c->token_count &&
                           c->tokens[c->token_pos].type != TK_LBRACE &&
                           c->tokens[c->token_pos].type != TK_SEMI &&
                           c->tokens[c->token_pos].type != TK_EOF) {
                        const char *dn[16];
                        ZeroCCType dt[16];
                        int n = zero_cc_parse_kr_decl(c, dn, dt, 16);
                        if (n < 0) {
                            diag_report_error_at_token(c, "invalid K&R parameter declaration in function '%s'", vname);
                            while (c->token_pos < c->token_count &&
                                   c->tokens[c->token_pos].type != TK_LBRACE &&
                                   c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                            break;
                        }
                        for (int k = 0; k < n; k++) {
                            bool matched = false;
                            for (int pi = 0; pi < kr_nested_count; pi++) {
                                if (strcmp(kr_nested_names[pi], dn[k]) == 0) {
                                    if (kr_matched[pi]) {
                                        diag_report_error_at_token(c, "redefinition of parameter '%s'", dn[k]);
                                    } else {
                                        kr_matched[pi] = true;
                                        ZeroCCType kt = dt[k];
                                        kr_fc.param_tis[pi] = zero_cc_type_to_typeinfo(ctx->perm_arena, kt);
                                    }
                                    matched = true;
                                    break;
                                }
                            }
                            if (!matched) {
                                diag_report_error_at_token(c, "declaration for parameter '%s' but no such parameter", dn[k]);
                            }
                        }
                    }
                    fc = &kr_fc;
                }
                ASTRef param_list = fc->param_list;
                TypeInfo *fn_ti = type_function(ctx->perm_arena, ret_ti, fc->param_count > 0 ? (TypeInfo **)fc->param_tis : NULL, fc->param_count, fc->is_variadic);
                fn_ti->details.function.is_prototype = fc->has_prototype;

                /* Function definition or prototype */
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
                    if (zero_cc_declarator_is_function(&decl) && c->token_pos > 0 &&
                        /* Inside a block = GNU nested function (trampoline).
                         * Phase 1: JIT-only, x86-64, no closure capture.
                         * Register the signature, save the body token range,
                         * skip past the body, and return a decl node so the
                         * name is visible in scope.  The body is compiled
                         * after the enclosing function is lowered (see
                         * zero_cc_compile_function in compile.h). */
                        true) {
                        /* Save body start (points at '{') */
                        size_t body_start = c->token_pos;

                        /* Register the nested function in c->functions */
                        if (c->function_count < ZERO_CC_MAX_FUNCTIONS) {
                            ZeroCCFunction *nf = &c->functions[c->function_count++];
                            memset(nf, 0, sizeof(*nf));
                            snprintf(nf->name, sizeof(nf->name), "%s", vname);
                            nf->text_offset = 0;       /* set when compiled */
                            nf->return_type = ret_type;
                            nf->returns_struct = (ret_type.tag == TYPE_STRUCT || ret_type.tag == TYPE_UNION ||
                                                  (ret_type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&ret_type)));
                            nf->ret_struct_sz = ret_type.size;
                            nf->is_prototype = false;
                            nf->has_prototype = fc->has_prototype;
                            nf->is_variadic = fc->is_variadic;
                            nf->is_nested = true;
                            /* Nested functions are block-scoped and may share
                             * source names in different enclosing functions
                             * (e.g. three `nested` fns in c-torture
                             * execute/20061220-1.c). Emit each under a unique
                             * symbol name; call sites resolve the right one
                             * via the enclosing chain (see
                             * zero_cc_resolve_callee in ast_lowering_call.c). */
                            snprintf(nf->nested_sym_name, sizeof(nf->nested_sym_name),
                                     "__zf%u_%s", (unsigned)(c->function_count - 1), nf->name);
                            nf->nested_body_start = body_start;
                            /* Phase 2: capture all enclosing locals as hidden
                             * leading parameters. The nested function's
                             * signature becomes:
                             *   ret_type nested_fn(captured_0, captured_1, ...,
                             *                      user_param_0, user_param_1, ...)
                             * Callers pass the current values of captured
                             * variables when invoking the nested function. */
                            nf->nested_enclosing_fn_idx = (c->function_count >= 2) ? (c->function_count - 2) : (size_t)-1;
                            nf->enclosing_local_count = c->local_count;
                            if (c->local_count > 0) {
                                Arena *farena = zero_cc_get_fn_arena(c);
                                nf->enclosing_local_snapshot = (ZeroCCSym *)arena_alloc(farena, c->local_count * sizeof(ZeroCCSym));
                                memcpy(nf->enclosing_local_snapshot, c->locals, c->local_count * sizeof(ZeroCCSym));
                                nf->nested_captured_count = c->local_count;
                                /* Total parameters = captured + user params */
                                size_t total_param_count = c->local_count + fc->param_count;
                                nf->param_count = total_param_count;
                                if (total_param_count > 0) {
                                    nf->param_names = (const char **)arena_alloc(farena, total_param_count * sizeof(const char *));
                                    nf->param_types = (ZeroCCType *)arena_alloc(farena, total_param_count * sizeof(ZeroCCType));
                                    /* Phase 3a: per-capture by-ref flags +
                                     * original (pointee) types for the mirror
                                     * locals added in the nested setup. */
                                    nf->nested_capture_byref = (bool *)arena_alloc(farena, c->local_count * sizeof(bool));
                                    nf->nested_capture_types = (ZeroCCType *)arena_alloc(farena, c->local_count * sizeof(ZeroCCType));
                                    /* Captured variables come first */
                                    size_t pi = 0;
                                    for (size_t ci = 0; ci < c->local_count; ci++, pi++) {
                                        ZeroCCType lt = c->locals[ci].type;
                                        bool scalar_small = (lt.tag != TYPE_STRUCT && lt.tag != TYPE_UNION &&
                                                              lt.tag != TYPE_ARRAY && !lt.is_vla &&
                                                              !lt.is_complex && !lt.is_func &&
                                                              lt.size > 0 && lt.size <= 8);
                                        /* Phase 3a: scalar captures (int,
                                         * pointer, float, double, ...) of
                                         * size <= 8 are passed BY REFERENCE:
                                         * the hidden param is a pointer to
                                         * the enclosing slot; the nested
                                         * function mirrors the pointee into
                                         * a same-named local so writes are
                                         * visible to the enclosing function.
                                         * Aggregate captures stay by-value. */
                                        nf->nested_capture_types[ci] = lt;
                                        nf->nested_capture_byref[ci] = scalar_small;
                                        if (scalar_small) {
                                            ZeroCCType *base_copy = (ZeroCCType *)arena_alloc(farena, sizeof(ZeroCCType));
                                            *base_copy = lt;
                                            ZeroCCType pt;
                                            memset(&pt, 0, sizeof(pt));
                                            pt.tag = TYPE_PTR;
                                            pt.size = 8;
                                            pt.align = 8;
                                            pt.base = base_copy;
                                            nf->param_types[pi] = pt;
                                        } else {
                                            nf->param_types[pi] = lt;
                                        }
                                        /* Keep the capture name on the hidden param:
                                         * there is no mirror local to shadow it, and
                                         * the body's semantic scope registers the
                                         * capture name with the pointee type while
                                         * lowering routes accesses through the param
                                         * (is_capture_mirror). */
                                        nf->param_names[pi] = zero_cc_intern_string(c, c->locals[ci].name);
                                    }
                                    /* Then user parameters */
                                    if (kr_nested) {
                                        for (size_t ui = 0; ui < fc->param_count && ui < (size_t)kr_nested_count; ui++, pi++) {
                                            nf->param_names[pi] = zero_cc_intern_string(c, kr_nested_names[ui]);
                                            nf->param_types[pi] = typeinfo_to_zero_cc_type(fc->param_tis[ui]);
                                        }
                                    } else {
                                    ASTRef pl = param_list;
                                    for (size_t ui = 0; ui < fc->param_count && pl != AST_NULL; ui++, pi++, pl = zero_cc_ast_get(arena, pl) ? zero_cc_ast_get(arena, pl)->next : AST_NULL) {
                                        ZeroCCASTNode *pn = zero_cc_ast_get(arena, pl);
                                        nf->param_names[pi] = (pn && pn->s.str && pn->s.str[0])
                                            ? zero_cc_intern_string(c, pn->s.str) : "";
                                        nf->param_types[pi] = typeinfo_to_zero_cc_type(fc->param_tis[ui]);
                                    }
                                    }
                                }
                            } else {
                                nf->param_count = fc->param_count;
                                if (fc->param_count > 0) {
                                    Arena *farena = zero_cc_get_fn_arena(c);
                                    nf->param_names = (const char **)arena_alloc(farena, fc->param_count * sizeof(const char *));
                                    nf->param_types = (ZeroCCType *)arena_alloc(farena, fc->param_count * sizeof(ZeroCCType));
                                    /* Walk the param list AST to extract names;
                                     * param types come from fc->param_tis (TypeInfo). */
                                    if (kr_nested) {
                                        for (size_t pi2 = 0; pi2 < fc->param_count && pi2 < (size_t)kr_nested_count; pi2++) {
                                            nf->param_names[pi2] = zero_cc_intern_string(c, kr_nested_names[pi2]);
                                            nf->param_types[pi2] = typeinfo_to_zero_cc_type(fc->param_tis[pi2]);
                                        }
                                    } else {
                                    ASTRef pl = param_list;
                                    for (size_t pi = 0; pi < fc->param_count && pl != AST_NULL; pi++) {
                                        ZeroCCASTNode *pn = zero_cc_ast_get(arena, pl);
                                        nf->param_names[pi] = (pn && pn->s.str && pn->s.str[0])
                                            ? zero_cc_intern_string(c, pn->s.str) : "";
                                        nf->param_types[pi] = typeinfo_to_zero_cc_type(fc->param_tis[pi]);
                                        pl = pn ? pn->next : AST_NULL;
                                    }
                                    }
                                }
                            }

                            /* C99 6.7.6.2: save VLA parameter dimension
                             * expressions (the declarator stack keeps each
                             * in p_node->right). The nested function
                             * evaluates them at entry, after the capture
                             * mirror-in, so `char a[2][++N]` sees the
                             * current captured value. The AST nodes live in
                             * this function's body arena (freed at
                             * lowering), so clone them into a dedicated
                             * dim arena - same pattern as
                             * zero_cc_parse_fn_param_block. */
                            {
                                ASTRef pl2 = param_list;
                                for (size_t ui = 0; ui < fc->param_count && pl2 != AST_NULL; ui++) {
                                    ZeroCCASTNode *pn = zero_cc_ast_get(arena, pl2);
                                    pl2 = pn ? pn->next : AST_NULL;
                                    if (!pn || pn->right == AST_NULL) continue;
                                    if (!nf->param_dim_arena) {
                                        ZeroCCASTArena *da = (ZeroCCASTArena *)calloc(1, sizeof(ZeroCCASTArena));
                                        if (!da) break;
                                        zero_cc_ast_arena_init(da, 64);
                                        nf->param_dim_arena = da;
                                        Arena *fa = zero_cc_get_fn_arena(c);
                                        nf->param_dim_exprs = (ASTRef *)arena_alloc(fa, nf->param_count * sizeof(ASTRef));
                                        /* Capture-param slots are never
                                         * written; zero the whole array or
                                         * the entry-eval loop reads garbage. */
                                        memset(nf->param_dim_exprs, 0, nf->param_count * sizeof(ASTRef));
                                    }
                                    /* Clone the whole dim list (a param can
                                     * have several VLA dims, chained via
                                     * next, outermost-to-innermost). */
                                    ZeroCCASTArena *da = (ZeroCCASTArena *)nf->param_dim_arena;
                                    ASTRef head = AST_NULL;
                                    ZeroCCASTNode *prev_node = NULL;
                                    for (ASTRef orig = pn->right; orig != AST_NULL; ) {
                                        ZeroCCASTNode *on = zero_cc_ast_get(arena, orig);
                                        if (!on) break;
                                        ASTRef cl = zero_cc_ast_clone(arena, orig, da);
                                        if (cl == AST_NULL) break;
                                        if (head == AST_NULL) head = cl;
                                        else if (prev_node) prev_node->next = cl;
                                        prev_node = zero_cc_ast_get(da, cl);
                                        orig = on->next;
                                    }
                                    if (head != AST_NULL) {
                                        nf->param_dim_exprs[nf->param_count - fc->param_count + ui] = head;
                                    }
                                }
                            }

                            /* Sprint 99: nonlocal goto - the body may
                             * `goto X;` where X labels an ANCESTOR
                             * function, or `goto *expr;` where the label
                             * value points into an ancestor frame. Such a
                             * jump restores the target scope's (rbp, rsp),
                             * so the nested fn needs one hidden (rbp, rsp)
                             * PAIR param per ancestor scope (outermost
                             * first, after the captures, before the user
                             * params). A conservative token scan decides
                             * the need: any `goto IDENT` whose name is not
                             * a label defined in this body (IDENT ':'
                             * pair), or any computed `goto *` (the target
                             * is only known at runtime and may be in an
                             * ancestor). The scan can only add pairs, never
                             * miss them - a real local label is always an
                             * IDENT ':' pair, so a false positive (extra
                             * pair) is harmless while a false negative
                             * would jump with the wrong frame. */
                            {
                                size_t be = body_start + 1;
                                int bd = 1;
                                while (be < c->token_count && bd > 0) {
                                    if (c->tokens[be].type == TK_LBRACE) bd++;
                                    else if (c->tokens[be].type == TK_RBRACE) bd--;
                                    be++;
                                }
                                char gl[32][64]; int gl_n = 0;
                                char ll[32][64]; int ll_n = 0;
                                for (size_t qi = body_start; qi + 1 < be && qi < c->token_count; qi++) {
                                    if (c->tokens[qi].type == TK_GOTO) {
                                        if (c->tokens[qi + 1].type == TK_IDENT &&
                                                   gl_n < 32) {
                                            snprintf(gl[gl_n], 64, "%s", c->tokens[qi + 1].str_val);
                                            gl_n++;
                                        }
                                    }
                                    if (c->tokens[qi].type == TK_IDENT &&
                                        c->tokens[qi + 1].type == TK_COLON &&
                                        ll_n < 32) {
                                        snprintf(ll[ll_n], 64, "%s", c->tokens[qi].str_val);
                                        ll_n++;
                                    }
                                }
                                int depth = 0;
                                for (size_t ai = nf->nested_enclosing_fn_idx;
                                     ai != (size_t)-1; ) {
                                    depth++;
                                    if (!c->functions[ai].is_nested) break;
                                    ai = c->functions[ai].nested_enclosing_fn_idx;
                                }
                                bool needs = false;
                                for (int gi = 0; gi < gl_n && !needs; gi++) {
                                    bool local = false;
                                    for (int li2 = 0; li2 < ll_n; li2++) {
                                        if (strcmp(ll[li2], gl[gi]) == 0) local = true;
                                    }
                                    if (!local) needs = true;
                                }
                                if (needs && depth > 0 &&
                                    nf->param_count + 2 * (size_t)depth <= ZERO_CC_MAX_PARAMS) {
                                    Arena *farena = zero_cc_get_fn_arena(c);
                                    size_t new_total = nf->param_count + 2 * (size_t)depth;
                                    const char **nn = (const char **)arena_alloc(farena,
                                        new_total * sizeof(const char *));
                                    ZeroCCType *nt = (ZeroCCType *)arena_alloc(farena,
                                        new_total * sizeof(ZeroCCType));
                                    if (nf->param_names)
                                        memcpy(nn, nf->param_names, nf->param_count * sizeof(const char *));
                                    if (nf->param_types)
                                        memcpy(nt, nf->param_types, nf->param_count * sizeof(ZeroCCType));
                                    /* The ABI order is [captures, pairs,
                                     * user params]: splice the pair params
                                     * right after the captures (the leading
                                     * nested_captured_count entries) so the
                                     * user params keep their trailing slots. */
                                    size_t cap = nf->nested_captured_count;
                                    if (cap > nf->param_count) cap = nf->param_count;
                                    size_t user_n = nf->param_count - cap;
                                    if (user_n > 0) {
                                        memmove(nn + cap + 2 * (size_t)depth, nn + cap,
                                                user_n * sizeof(const char *));
                                        memmove(nt + cap + 2 * (size_t)depth, nt + cap,
                                                user_n * sizeof(ZeroCCType));
                                    }
                                    char pname[48];
                                    for (int pi = 0; pi < depth; pi++) {
                                        ZeroCCType it = zero_cc_type_int64();
                                        snprintf(pname, sizeof(pname), "__nlgbp_%d", pi);
                                        nn[cap + 2 * pi] = zero_cc_intern_string(c, pname);
                                        nt[cap + 2 * pi] = it;
                                        snprintf(pname, sizeof(pname), "__nlbsp_%d", pi);
                                        nn[cap + 2 * pi + 1] = zero_cc_intern_string(c, pname);
                                        nt[cap + 2 * pi + 1] = it;
                                    }
                                    if (nf->param_dim_exprs) {
                                        ASTRef *nd = (ASTRef *)arena_alloc(farena,
                                            new_total * sizeof(ASTRef));
                                        memcpy(nd, nf->param_dim_exprs, cap * sizeof(ASTRef));
                                        if (user_n > 0)
                                            memcpy(nd + cap + 2 * (size_t)depth,
                                                   nf->param_dim_exprs + cap,
                                                   user_n * sizeof(ASTRef));
                                        memset(nd + cap, 0, 2 * (size_t)depth * sizeof(ASTRef));
                                        nf->param_dim_exprs = nd;
                                    }
                                    nf->param_names = nn;
                                    nf->param_types = nt;
                                    nf->param_count = new_total;
                                    nf->goto_pair_count = depth;
                                }
                            }

                            /* Phase 3b: a capturing nested function can only
                             * be used through an indirect call via a
                             * trampoline (its ABI has the hidden capture
                             * params). Register the stub function (emitted
                             * in compile.h right after the nested fn) and
                             * its static context object (32 bytes per
                             * capture: by-reference = pointer word, by-value
                             * = value). x86-64 only: the stub is raw code
                             * emitted by the x86-64 backend; other ISAs
                             * reject address-taking of capturing nested fns
                             * with a diagnostic instead of miscompiling.
                             * Variadic captures and >32-byte by-value
                             * captures have no static-stub representation.
                             * Sprint 99: nested functions with nonlocal-goto
                             * pairs also need the stub (the pair words
                             * travel in the ctx) even without captures. */
                            nf->tramp_stub_fn_idx = -1;
                            if ((nf->nested_captured_count > 0 || nf->goto_pair_count > 0) &&
                                !nf->is_variadic &&
                                (c->isa == ISA_X86_64 || c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) &&
                                c->function_count < ZERO_CC_MAX_FUNCTIONS) {
                                bool stub_ok = true;
                                for (size_t ci = 0; ci < nf->nested_captured_count; ci++) {
                                    if (!nf->nested_capture_byref[ci]) {
                                        size_t csz = nf->nested_capture_types[ci].size;
                                        if (csz == 0 || csz > 32) { stub_ok = false; break; }
                                    }
                                }
                                if (stub_ok && zero_cc_tramp_total_units(nf) > ZERO_CC_TRAMP_MAX_UNITS)
                                    stub_ok = false;
                                if (stub_ok) {
                                    size_t nf_idx = c->function_count - 1;
                                    snprintf(nf->tramp_stub_name, sizeof(nf->tramp_stub_name),
                                             "__zf%u_%s_tramp", (unsigned)nf_idx, nf->name);
                                    snprintf(nf->tramp_ctx_name, sizeof(nf->tramp_ctx_name),
                                             "__zf%u_%s_ctx", (unsigned)nf_idx, nf->name);
                                    /* Sprint 99: pair words (16 bytes each) follow the
                                     * captures in the ctx. */
                                    nf->tramp_ctx_size = 32 * nf->nested_captured_count +
                                                          16 * (size_t)nf->goto_pair_count;
                                    /* ctx: zero-initialized static object;
                                     * only the compiler reads/writes it. */
                                    ZeroCCType ctx_t;
                                    memset(&ctx_t, 0, sizeof(ctx_t));
                                    ctx_t.tag = TYPE_ARRAY;
                                    ctx_t.size = nf->tramp_ctx_size;
                                    ctx_t.align = 16;
                                    (void)zero_cc_add_global_var(c, nf->tramp_ctx_name, ctx_t,
                                                                  0, nf->tramp_ctx_size, true, false);
                                    /* Data layout (mirrors a zero-initialized global: .data
                                     * bytes or a .bss running offset) - without it the ctx
                                     * symbol would be undefined in AOT output. */
                                    {
                                        size_t coff;
                                        if (nf->tramp_ctx_size >= 256) {
                                            coff = c->bss_size;
                                            c->bss_size += nf->tramp_ctx_size;
                                        } else {
                                            coff = c->data_section.length;
                                            for (size_t i = 0; i < nf->tramp_ctx_size; i++)
                                                zero_cc_emit_u8(&c->data_section, 0);
                                        }
                                        ZeroCCGlobalVar *ctx_gv = find_global_var(c, nf->tramp_ctx_name);
                                        if (ctx_gv) {
                                            ctx_gv->data_offset = coff;
                                            ctx_gv->is_bss = (nf->tramp_ctx_size >= 256);
                                        }
                                    }
                                    ZeroCCFunction *sf = &c->functions[c->function_count++];
                                    memset(sf, 0, sizeof(*sf));
                                    snprintf(sf->name, sizeof(sf->name), "%s", nf->tramp_stub_name);
                                    snprintf(sf->nested_sym_name, sizeof(sf->nested_sym_name),
                                             "%s", nf->tramp_stub_name);
                                    sf->is_nested = true;
                                    sf->is_tramp_stub = true;
                                    sf->return_type = nf->return_type;
                                    sf->returns_struct = nf->returns_struct;
                                    sf->ret_struct_sz = nf->ret_struct_sz;
                                    size_t uc = nf->param_count - nf->nested_captured_count -
                                                 2 * (size_t)nf->goto_pair_count;
                                    sf->param_count = uc;
                                    if (uc > 0 && nf->param_names && nf->param_types) {
                                        Arena *farena = zero_cc_get_fn_arena(c);
                                        sf->param_names = (const char **)arena_alloc(farena, uc * sizeof(const char *));
                                        sf->param_types = (ZeroCCType *)arena_alloc(farena, uc * sizeof(ZeroCCType));
                                        const char *up = nf->param_names + nf->nested_captured_count +
                                                          2 * nf->goto_pair_count;
                                        const ZeroCCType *ut = nf->param_types + nf->nested_captured_count +
                                                               2 * nf->goto_pair_count;
                                        memcpy(sf->param_names, up, uc * sizeof(const char *));
                                        memcpy(sf->param_types, ut, uc * sizeof(ZeroCCType));
                                    }
                                    nf->tramp_stub_fn_idx = (int)(c->function_count - 1);
                                }
                            }
                        }

                        /* Skip past the body (brace-depth counted) */
                        c->token_pos++; /* consume '{' */
                        int bdepth = 1;
                        while (c->token_pos < c->token_count && bdepth > 0) {
                            if (c->tokens[c->token_pos].type == TK_LBRACE) bdepth++;
                            else if (c->tokens[c->token_pos].type == TK_RBRACE) bdepth--;
                            c->token_pos++;
                        }
                        /* Store body end (points past '}') */
                        if (c->function_count > 0) {
                            c->functions[c->function_count - 1].nested_body_end = c->token_pos;
                        }

                        /* Return a declaration AST node so the name is visible */
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
                        *out_ref = func_ref; return true;
                    }
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
                    /* C89 mixed declarator list: `float fx (), inita (), a, b, c;`
                     * - function declarators and variables share one
                     * declaration.  When more declarators follow the `)`,
                     * parse them (functions become prototypes, variables
                     * become locals) and chain the decl nodes. */
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
                        ASTRef list_head = func_ref;
                        ASTRef list_tail = func_ref;
                        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
                            c->token_pos++;
                            ZeroCCDeclarator md;
                            zero_cc_declarator_init(&md);
                            if (!zero_cc_parse_declarator(c, arena, ctx, &md) || !md.name ||
                                !zero_cc_declarator_validate(c, &md)) {
                                *out_ref = AST_NULL; return true;
                            }
                            if (zero_cc_declarator_is_function(&md)) {
                                ZeroCCType mret = zero_cc_declarator_apply_return(&md, base_ret);
                                const ZeroCCDeclaratorChunk *mfc = zero_cc_declarator_get_function_chunk(&md);
                                if (!mfc) mfc = &md.stack[0];
                                TypeInfo *mret_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, mret);
                                TypeInfo *mfn_ti = type_function(ctx->perm_arena, mret_ti,
                                    mfc->param_count > 0 ? (TypeInfo **)mfc->param_tis : NULL,
                                    mfc->param_count, mfc->is_variadic);
                                mfn_ti->details.function.is_prototype = mfc->has_prototype;
                                ASTRef mref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                                ZeroCCASTNode *mn = zero_cc_ast_get(arena, mref);
                                mn->line = line; mn->col = col;
                                mn->s.str = zero_cc_intern_string(c, md.name);
                                mn->s_len = strlen(md.name);
                                mn->flags |= NODE_FLAG_FUNC_PROTO;
                                mn->left = mfc->param_list;
                                mn->right = AST_NULL;
                                decl_node_set_type(ctx, mn, mfn_ti);
                                if (!zero_cc_find_function(c, md.name) &&
                                    c->function_count < ZERO_CC_MAX_FUNCTIONS) {
                                    ZeroCCFunction *mfn = &c->functions[c->function_count++];
                                    snprintf(mfn->name, sizeof(mfn->name), "%s", md.name);
                                    mfn->text_offset = 0;
                                    mfn->return_type = mret;
                                    mfn->returns_struct = (mret.tag == TYPE_STRUCT ||
                                                           mret.tag == TYPE_UNION);
                                    mfn->ret_struct_sz = mret.size;
                                    mfn->is_prototype = true;
                                }
                                ZeroCCASTNode *tn = zero_cc_ast_get(arena, list_tail);
                                if (tn) tn->next = mref;
                                list_tail = mref;
                                continue;
                            }
                            ZeroCCType mtype = zero_cc_declarator_apply(&md, base_ret);
                            TypeInfo *mti = zero_cc_type_to_typeinfo(ctx->perm_arena, mtype);
                            ZeroCCSym *msym = zero_cc_add_local(c, md.name, mtype);
                            if (!msym) {
                                diag_report_error_at_token(c,
                                    "out of memory allocating local variable '%s'", md.name);
                                *out_ref = AST_NULL; return true;
                            }
                            if (md.asm_name[0]) {
                                snprintf(msym->asm_name, sizeof(msym->asm_name), "%s", md.asm_name);
                            }
                            if (is_static_local) zero_cc_static_local_register(c, msym);
                            ASTRef mref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                            ZeroCCASTNode *mn = zero_cc_ast_get(arena, mref);
                            mn->line = line; mn->col = col;
                            mn->s.str = zero_cc_intern_string(c, md.name);
                            mn->s_len = strlen(md.name);
                            mn->aux.step = (uint32_t)((size_t)(msym - c->locals)) + 1;
                            decl_node_set_type(ctx, mn, mti);
                            if (c->token_pos < c->token_count &&
                                c->tokens[c->token_pos].type == TK_ASSIGN) {
                                c->token_pos++;
                                mn->left = parse_assignment_ast(c, arena);
                            }
                            ZeroCCASTNode *tn = zero_cc_ast_get(arena, list_tail);
                            if (tn) tn->next = mref;
                            list_tail = mref;
                        }
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                            c->token_pos++;
                        }
                        *out_ref = list_head; return true;
                    }
                    *out_ref = func_ref; return true;
                }
            } else {
                /* Not a function: treat as variable declaration */
                if (!zero_cc_declarator_validate(c, &decl)) {
                    *out_ref = AST_NULL; return true;
                }

                bool is_array = zero_cc_declarator_has_array(&decl);
                bool has_any_vla = zero_cc_declarator_has_vla(&decl);
                /* The allocation "count" is the OUTERMOST dimension (VLA
                 * expr, or the constant dim as a NODE_INT): for `int a[2][N]`
                 * the count is 2 and the inner `N` is handled by the type
                 * chain in the lowering. */
                ASTRef vla_dim_expr = AST_NULL;
                int64_t outer_cv = 0;
                if (has_any_vla) {
                    if (!zero_cc_declarator_outer_dim(&decl, arena, &outer_cv, &vla_dim_expr)) {
                        vla_dim_expr = zero_cc_declarator_vla_dim_expr(&decl);
                    }
                } else {
                    vla_dim_expr = zero_cc_declarator_vla_dim_expr(&decl);
                }

                ZeroCCType base_decl = has_anon_struct ? anon_struct_type :
                    (has_typeof_base ? typeof_base_type : zero_cc_resolve_base_type(c, type_tok, c->tokens[type_start_pos].str_val));
                if (decl_is_long_double && base_decl.tag == TYPE_DOUBLE) {
                    base_decl = zero_cc_type_long_double();
                }
                if (suffix_complex && !zero_cc_type_is_complex(&base_decl)) {
                    base_decl = zero_cc_type_complex(&base_decl);
                }
                if (decl_is_unsigned) {
                    if (base_decl.tag == TYPE_INT8) base_decl.tag = TYPE_UINT8;
                    else if (base_decl.tag == TYPE_INT16) base_decl.tag = TYPE_UINT16;
                    else if (base_decl.tag == TYPE_INT32) base_decl.tag = TYPE_UINT32;
                    else if (base_decl.tag == TYPE_INT64) base_decl.tag = TYPE_UINT64;
                    else if (base_decl.tag == TYPE_INT128) base_decl.tag = TYPE_UINT128;
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
                    *out_ref = AST_NULL; return true;
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
                    *out_ref = AST_NULL; return true;
                }
                if (decl.asm_name[0]) {
                    snprintf(sym->asm_name, sizeof(sym->asm_name), "%s", decl.asm_name);
                }
                /* C11 6.7.5: `_Alignas(N)` on a declaration sets the
                 * object's alignment without changing its size (the frame
                 * slot is already aligned via decl_align_spec inside
                 * zero_cc_add_local; the GNU aligned(N) attribute path
                 * (align_attr_val) instead pads the type size). N > 16 is
                 * guaranteed by the runtime SP realign prologue at lowering
                 * (zero_cc_lower_ast); the type records the full N. */
                if (c11_align_spec > 0 && c11_align_spec > sym->type.align) {
                    sym->type.align = (int)c11_align_spec;
                }
                if (is_extern) {
                    sym->is_extern = true;
                    sym->stack_offset = 0;
                    c->current_stack_frame = prev_frame;
                    ZeroCCFunction *prev_fn = zero_cc_find_function(c, vname);
                    if (prev_fn && prev_fn->is_object && prev_fn->return_type.size > decl_type.size) {
                        decl_type = prev_fn->return_type;
                        sym->type = decl_type;
                        decl_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, decl_type);
                    }
                    for (int si = (int)c->local_count - 2; si >= 0; si--) {
                        if (c->locals[si].is_extern && strcmp(c->locals[si].name, vname) == 0) {
                            if (c->locals[si].type.size > decl_type.size) {
                                decl_type = c->locals[si].type;
                                sym->type = decl_type;
                                decl_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, decl_type);
                            }
                            break;
                        }
                    }
                    if (!find_global_var(c, vname) && !prev_fn && c->function_count < ZERO_CC_MAX_FUNCTIONS) {
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
                        *out_ref = AST_NULL; return true;
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
                if (has_any_vla && decl_type.tag == TYPE_ARRAY) {
                    dn->flags |= NODE_DECL_IS_VLA;
                    dn->right = vla_dim_expr; /* outermost runtime dimension expr */
                }
                if (defines_vla_struct) {
                    /* This declaration DEFINES the VLA struct: the lowering
                     * captures the member dimensions at definition time. */
                    dn->flags |= NODE_DECL_VLA_STRUCT_DEF;
                }

                /* Attach type to AST node for semantic analysis */
                decl_node_set_type(ctx, dn, decl_ti);

                /* C99 §6.7.8 p3: VLA shall not have an initializer */
                if ((dn->flags & NODE_DECL_IS_VLA) != 0) {
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                        diag_report_error_at_token(c,
                            "variable length array '%s' cannot be initialized", dn->s.str);
                        *out_ref = AST_NULL; return true;
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

                if (sym->is_static_local &&
                    c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                    c->token_pos++;
                    *out_ref = decl_ref; return true;
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
                        *out_ref = AST_NULL; return true;
                    }

                    const char *next_vname = next_decl.name;
                    /* An inline anonymous struct/union (`union { ... } a, b;`)
                     * must give EVERY comma declarator the defined type -
                     * falling through to zero_cc_resolve_base_type with the
                     * keyword token as the "name" typed the second
                     * declarator as long (and, after the incomplete-tag
                     * materialization, as an empty union: c-torture
                     * execute/bswap-1.c, cbrt.c segfaulted in AOT). */
                    ZeroCCType next_base = has_anon_struct ? anon_struct_type :
                        (has_typeof_base ? typeof_base_type : zero_cc_resolve_base_type(c, type_tok, c->tokens[type_start_pos].str_val));
                    /* C99 6.7.2.3 suffix complex applies to every
                     * comma declarator: `double _Complex a, b;`. */
                    if (suffix_complex && !zero_cc_type_is_complex(&next_base)) {
                        next_base = zero_cc_type_complex(&next_base);
                    }
                    if (decl_is_unsigned) {
                        if (next_base.tag == TYPE_INT8) next_base.tag = TYPE_UINT8;
                        else if (next_base.tag == TYPE_INT16) next_base.tag = TYPE_UINT16;
                        else if (next_base.tag == TYPE_INT32) next_base.tag = TYPE_UINT32;
                        else if (next_base.tag == TYPE_INT64) next_base.tag = TYPE_UINT64;
                        else if (next_base.tag == TYPE_INT128) next_base.tag = TYPE_UINT128;
                    }
                    next_base.is_const = is_const;
                    next_base.is_volatile = is_volatile;
                    next_base.is_atomic = is_atomic;
                    if (vec_attr_size > 0) {
                        next_base = zero_cc_type_vector(&next_base, vec_attr_size);
                    }

                    ZeroCCType next_type = zero_cc_declarator_apply(&next_decl, next_base);
                    bool nhas_any_vla = zero_cc_declarator_has_vla(&next_decl);
                    ASTRef nvla_dim_expr = AST_NULL;
                    int64_t nouter_cv = 0;
                    if (nhas_any_vla) {
                        if (!zero_cc_declarator_outer_dim(&next_decl, arena, &nouter_cv, &nvla_dim_expr)) {
                            nvla_dim_expr = zero_cc_declarator_vla_dim_expr(&next_decl);
                        }
                    } else {
                        nvla_dim_expr = zero_cc_declarator_vla_dim_expr(&next_decl);
                    }

                    TypeInfo *next_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, next_type);
                    int32_t prev_next_frame = c->current_stack_frame;
                    ZeroCCSym *next_sym = zero_cc_add_local(c, next_vname, next_type);
                    if (!next_sym) {
                        diag_report_error_at_token(c, "out of memory allocating local variable '%s'", next_vname);
                        *out_ref = AST_NULL; return true;
                    }
                    if (next_decl.asm_name[0]) {
                        snprintf(next_sym->asm_name, sizeof(next_sym->asm_name), "%s", next_decl.asm_name);
                    }
                    if (c11_align_spec > 0 && c11_align_spec > next_sym->type.align) {
                        next_sym->type.align = (int)c11_align_spec; /* C11 _Alignas (align only) */
                    }
                    if (is_static_local && !nhas_any_vla) {
                        /* Sprint 37: each comma declarator of a static
                         * declaration gets its own data object + mirror. */
                        zero_cc_static_local_register(c, next_sym);
                    }
                    if (is_extern) {
                        next_sym->is_extern = true;
                        next_sym->stack_offset = 0;
                        c->current_stack_frame = prev_next_frame;
                        if (!find_global_var(c, next_vname) && !zero_cc_find_function(c, next_vname) && c->function_count < ZERO_CC_MAX_FUNCTIONS) {
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
                    if (nhas_any_vla && next_type.tag == TYPE_ARRAY) {
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
                        *out_ref = AST_NULL; return true;
                    }

                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                        c->token_pos++;
                        ASTRef next_init = AST_NULL;
                        if (next_sym->is_static_local) {
                            /* Static local: bake into the data section (same
                             * contract as the first declarator - the value
                             * arrives via the prologue mirror load). */
                            if (!zero_cc_static_local_patch_init(c, next_sym)) {
                                diag_report_error_at_token(c,
                                    "static local '%s' initializer must be a constant literal "
                                    "(complex initializers not yet supported)", next_vname);
                                while (c->token_pos < c->token_count &&
                                       c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                            }
                        } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
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
                    *out_ref = AST_NULL; return true;
                }
                *out_ref = head_decl; return true;
            }
        } else {
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) {
                c->token_pos++;
            }
            /* Struct definition with no declarator
             * (`struct s { char b[n]; } __attribute__((packed));`):
             * emit a synthetic definition node so the lowering captures
             * the VLA member dimensions at definition time (GCC
             * semantics). */
            if (defines_vla_struct) {
                ASTRef def_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
                ZeroCCASTNode *dfn = zero_cc_ast_get(arena, def_ref);
                dfn->flags |= NODE_DECL_VLA_STRUCT_DEF;
                dfn->s.str = zero_cc_intern_string(c, defines_vla_struct_tag);
                dfn->s_len = strlen(dfn->s.str);
                *out_ref = def_ref;
                return true;
            }
            *out_ref = AST_NULL; return true;
        }

    }

    c->token_pos = decl_save_pos;
    return false;
}

#endif /* ZERO_CC_PARSER_DECL_LOCAL_H */
