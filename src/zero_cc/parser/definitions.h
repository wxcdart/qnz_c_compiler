/* Type Definitions: typedef struct, typedef enum, standalone enum, scalar typedef */
#ifndef ZERO_CC_PARSER_DEFINITIONS_H
#define ZERO_CC_PARSER_DEFINITIONS_H

#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/const_eval.h"

/* Forward declaration */
#include "zero_cc/parser/decl_enum.h"
#include "zero_cc/parser/decl_struct.h"

/* Typedef or Standalone Struct/Union Definition */
static inline void zero_cc_stmt_typedef_struct(ZeroCCCompiler *c) {
    /* Sprint 68: storage-class prefixes on file-scope struct/union
     * definitions with declarators: `static struct S { ... } u;`,
     * `const static union { ... } v;`.  The specifiers may interleave in
     * any order (C99 6.7.1); `static` makes the variable internal-linkage.
     * (Qualifiers on the variable itself are ignored.) */
    bool st_is_static = false;
    for (int sc = 0; sc < 8 && c->token_pos < c->token_count; sc++) {
        ZeroCCTokType st = c->tokens[c->token_pos].type;
        if (st == TK_STATIC) { st_is_static = true; c->token_pos++; }
        else if (st == TK_INLINE || st == TK_CONST || st == TK_VOLATILE || st == TK_RESTRICT) c->token_pos++;
        else break;
    }
    bool is_typedef = false;
    if (c->tokens[c->token_pos].type == TK_TYPEDEF) {
        is_typedef = true;
        c->token_pos++; /* consume TK_TYPEDEF */
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_CONST ||
                c->tokens[c->token_pos].type == TK_VOLATILE ||
                c->tokens[c->token_pos].type == TK_RESTRICT)) {
            c->token_pos++;
        }
    }
    bool is_union = (c->tokens[c->token_pos].type == TK_UNION);
    if (c->tokens[c->token_pos].type == TK_STRUCT || is_union) {
        c->token_pos++;
        /* GNU aligned(N) between the tag keyword and the tag name:
         * `struct __attribute__((aligned(4))) S { ... };` (the alignment
         * usually matches the natural alignment; consume the marker). */
        {
            size_t probe = 0;
            while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) { /* skip */ }
        }
        char st_name[128] = "";
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            snprintf(st_name, sizeof(st_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_LBRACE) {
            ZeroCCType st = zero_cc_parse_struct_body(c, is_union);
            /* GNU aligned(N) trailing the struct body:
             * `struct x { ... } __attribute__((aligned(32))) X;`
             * (preprocessor marker __zc_aligned__(N)). Raise the struct's
             * alignment and pad its size to a multiple of N before the
             * tag/alias/typedef registrations so every later use sees it. */
            {
                size_t probe = 0;
                if (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                    if (probe > st.align) {
                        st.align = probe;
                        if (st.size < probe) st.size = probe;
                        if ((st.size % probe) != 0) st.size = (st.size + probe - 1) / probe * probe;
                    }
                }
            }
            /* GNU `__attribute__((scalar_storage_order("big-endian")))`
             * (preprocessor marker __zc_sso_be__): bit-fields are assigned
             * from the MSB of each allocation unit and the unit's bytes
             * are stored big-endian (c-torture execute/20230630-2/4).
             * Remap the bit offsets (value-domain, from the unit LSB) and
             * flag the members; load/store/initialization byte-swap the
             * unit accordingly. */
            if (c->tokens[c->token_pos].type == TK_IDENT &&
                strcmp(c->tokens[c->token_pos].str_val, "__zc_sso_be__") == 0) {
                c->token_pos++;
                size_t si = 0;
                while (si < st.member_count) {
                    ZeroCCStructMember *sm = &st.members[si];
                    if (!sm->is_bitfield) { si++; continue; }
                    size_t unit_off = sm->offset;
                    int unit_bits = (int)(sm->size * 8);
                    int used = 0;
                    while (si < st.member_count) {
                        ZeroCCStructMember *mm = &st.members[si];
                        if (!mm->is_bitfield || mm->offset != unit_off) break;
                        int w = mm->bit_width;
                        if (w > 0 && used > 0 && used + w > unit_bits) break;
                        mm->bit_offset = (uint8_t)(unit_bits - used - w);
                        mm->sso_be = true;
                        mm->sso_unit = (uint8_t)sm->size;
                        used += w;
                        si++;
                    }
                }
            }
            if (st_name[0] != '\0') {
                snprintf(st.struct_name, sizeof(st.struct_name), "%s", st_name);
                zero_cc_register_struct(c, st_name, st);
                /* Forward typedefs of this tag (typedef struct S S;) hold
                 * incomplete snapshots; refresh them with the definition. */
                zero_cc_sync_typedefs_with_struct(c, st_name, &st);
                zero_cc_sync_globals_with_struct(c, st_name, &st);
            }
            /* Declarators follow the definition (C99 6.7.2.1).  The first
             * declarator may begin with `*` (pointer to an anonymous
             * struct: `struct { ... } *p;`; typedef: `typedef struct x
             * { ... } *tree;`). */
            /* Sprint 68: a storage class may sit in the declarator:
             * `struct a { ... } static g = {9, 5};` (C99 6.7.4).  Probe
             * past a short specifier run when an identifier/pointer
             * follows, and consume it.  Sprint 72: `extern`/`__thread` in
             * the declarator too (`struct S { ... } extern __thread a;`,
             * c-torture compile/pr78694). */
            bool dcl_is_static = false;
            bool dcl_is_extern = false;
            {
                size_t sc_probe = c->token_pos;
                for (int k = 0; k < 6 && sc_probe < c->token_count; k++) {
                    ZeroCCTokType pk = c->tokens[sc_probe].type;
                    if (pk == TK_STATIC || pk == TK_INLINE ||
                        pk == TK_CONST || pk == TK_VOLATILE ||
                        pk == TK_EXTERN || pk == TK_THREAD_LOCAL) sc_probe++;
                    else break;
                }
                if (sc_probe > c->token_pos && sc_probe < c->token_count &&
                    (c->tokens[sc_probe].type == TK_IDENT ||
                     (c->tokens[sc_probe].type == TK_STAR &&
                      sc_probe + 1 < c->token_count &&
                      c->tokens[sc_probe + 1].type == TK_IDENT))) {
                    while (c->token_pos < sc_probe) {
                        if (c->tokens[c->token_pos].type == TK_STATIC) dcl_is_static = true;
                        if (c->tokens[c->token_pos].type == TK_EXTERN) dcl_is_extern = true;
                        c->token_pos++;
                    }
                }
            }
            bool dcl_star = (c->tokens[c->token_pos].type == TK_STAR &&
                             c->token_pos + 1 < c->token_count &&
                             c->tokens[c->token_pos + 1].type == TK_IDENT);
            if (c->tokens[c->token_pos].type == TK_IDENT || dcl_star) {
                int dcl_ptr_depth = 0;
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                    dcl_ptr_depth++;
                    c->token_pos++;
                    while (c->token_pos < c->token_count &&
                           (c->tokens[c->token_pos].type == TK_CONST || c->tokens[c->token_pos].type == TK_VOLATILE)) {
                        c->token_pos++;
                    }
                }
                if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) {
                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                    return;
                }
                char alias[128];
                snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                if (is_typedef) {
                    /* typedef struct S {...} name; / typedef struct {...} *name;
                     * Array-suffixed declarators too: `typedef struct T
                     * {...} jb[1];` (the typedef names an ARRAY of T).
                     * Comma declarators allowed: `typedef struct T {...} a, *pa;` */
                    do {
                        if (st.struct_name[0] == '\0') {
                            snprintf(st.struct_name, sizeof(st.struct_name), "%s", alias);
                        }
                        zero_cc_register_struct(c, alias, st);
                        zero_cc_sync_typedefs_with_struct(c, st.struct_name, &st);
                        zero_cc_sync_globals_with_struct(c, st.struct_name, &st);
                        ZeroCCType tt = st;
                        for (int pd = 0; pd < dcl_ptr_depth; pd++) tt = zero_cc_type_ptr(&tt);
                        /* Array suffixes on the declarator (C99 6.7.5): the
                         * typedef names an array of the (pointer) type. */
                        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                            c->token_pos++;
                            int64_t td = 1;
                            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACKET) {
                                td = zero_cc_eval_const_expr(c);
                            }
                            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                            ZeroCCType elem = tt;
                            tt = zero_cc_type_array(&elem, td);
                        }
                        zero_cc_register_typedef(c, alias, tt);
                        dcl_ptr_depth = 0;
                        if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_COMMA) break;
                        c->token_pos++;
                        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                            dcl_ptr_depth++;
                            c->token_pos++;
                        }
                        if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) {
                            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                            return;
                        }
                        snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                        c->token_pos++;
                    } while (1);
                } else {
                    /* C99 6.7.2.1: after a struct/union *definition* an
                     * identifier is a VARIABLE declarator (optionally
                     * comma-separated), not a tag alias:
                     *   struct S { int x; } a, b = { 42 };
                     * (Previously the name was registered as a struct tag
                     * alias and no variable was created - latent bug, the
                     * two-line form `struct S {...}; struct S a;` masked it.) */
                    ZeroCCType vt = st;
                    int vptr_depth = dcl_ptr_depth;
                    for (int pd = 0; pd < vptr_depth; pd++) vt = zero_cc_type_ptr(&vt);
                    int64_t varray_len = 1;
                    int vdim_count = 0;
                    int64_t vdims[8];
                    if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                        while (c->tokens[c->token_pos].type == TK_LBRACKET) {
                            c->token_pos++;
                            if (c->tokens[c->token_pos].type == TK_RBRACKET) {
                                /* Unsized `struct S {...} a[] = { ... };`:
                                 * infer the bound from the brace list when
                                 * one follows (c-torture compile/pr38343);
                                 * otherwise it is an error. */
                                int64_t inferred = -1;
                                size_t ub = c->token_pos + 1;
                                if (ub < c->token_count && c->tokens[ub].type == TK_ASSIGN &&
                                    ub + 1 < c->token_count && c->tokens[ub + 1].type == TK_LBRACE) {
                                    size_t q = ub + 2;
                                    int64_t cnt = 0;
                                    int depth = 0;
                                    bool trailing = false;
                                    while (q < c->token_count) {
                                        ZeroCCTokType qt = c->tokens[q].type;
                                        if (qt == TK_LBRACE) { depth++; if (depth == 1) trailing = false; }
                                        else if (qt == TK_RBRACE) {
                                            if (depth == 0) break;
                                            depth--;
                                        } else if (depth == 0) {
                                            if (qt == TK_COMMA) { cnt++; trailing = true; }
                                            else trailing = false;
                                        }
                                        q++;
                                    }
                                    if (trailing) cnt--;
                                    inferred = (cnt > 0) ? cnt : 1;
                                }
                                if (inferred < 0) {
                                    diag_report_error_at_token(c,
                                        "unsized array of %s with no initializer list is not supported",
                                        is_union ? "union" : "struct");
                                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                                    return;
                                }
                                int64_t dim = inferred;
                                c->token_pos++;
                                varray_len *= dim;
                                if (vdim_count < 8) vdims[vdim_count++] = dim;
                                continue;
                            }
                            int64_t dim = zero_cc_eval_const_expr(c);
                            if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                            if (dim < 0) dim = 0;
                            varray_len *= dim;
                            if (vdim_count < 8) vdims[vdim_count++] = dim;
                        }
                    }
                    if (dcl_is_extern) {
                        /* Sprint 72: C99 6.7.2.1 + 6.9.2 - `struct S
                         * { ... } extern __thread a;`: the declarator
                         * names an EXTERNAL object: no storage is
                         * reserved, a function-table entry carries the
                         * variable's type (same shape as `extern int x;`;
                         * c-torture compile/pr78694). */
                        ZeroCCType vreg_type = vt;
                        for (int d = vdim_count - 1; d >= 0; d--) {
                            vreg_type = zero_cc_type_array(&vreg_type, (ssize_t)vdims[d]);
                        }
                        do {
                            if (c->function_count < ZERO_CC_MAX_FUNCTIONS) {
                                ZeroCCFunction *fn = &c->functions[c->function_count++];
                                fn->text_offset = 0; /* External, no code */
                                snprintf(fn->name, sizeof(fn->name), "%s", alias);
                                fn->return_type = vreg_type;
                                fn->is_external = true;
                                fn->is_prototype = true;
                                fn->is_object = true; /* extern variable, not a function */
                            }
                            if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                                /* extern with initializer (C11 inline
                                 * functions): the value is ignored.  Skip
                                 * the initializer expression (balanced
                                 * ()/{} kept out of the comma/semi scan). */
                                c->token_pos++;
                                int dep = 0;
                                while (c->token_pos < c->token_count) {
                                    ZeroCCTokType qt = c->tokens[c->token_pos].type;
                                    if (qt == TK_LPAREN || qt == TK_LBRACE) dep++;
                                    else if (qt == TK_RPAREN || qt == TK_RBRACE) dep--;
                                    else if (dep == 0 && (qt == TK_COMMA || qt == TK_SEMI)) break;
                                    c->token_pos++;
                                }
                            }
                            if (c->token_pos < c->token_count &&
                                c->tokens[c->token_pos].type == TK_COMMA) {
                                c->token_pos++;
                                while (c->token_pos < c->token_count &&
                                       (c->tokens[c->token_pos].type == TK_STAR ||
                                        c->tokens[c->token_pos].type == TK_CONST ||
                                        c->tokens[c->token_pos].type == TK_VOLATILE)) c->token_pos++;
                                if (c->token_pos >= c->token_count ||
                                    c->tokens[c->token_pos].type != TK_IDENT) {
                                    while (c->token_pos < c->token_count &&
                                           c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                                    if (c->token_pos < c->token_count &&
                                        c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                                    break;
                                }
                                snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                                c->token_pos++;
                                continue;
                            }
                            break;
                        } while (1);
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                        return;
                    }
                    do {
                        size_t elem_size = vt.size > 0 ? vt.size : 8;
                        size_t elem_align = vt.align > 0 ? vt.align : 8;
                        if (elem_align < 8) elem_align = 8;
                        size_t vsize = (size_t)varray_len * elem_size;
                        size_t vpad = (elem_align - (c->data_section.length % elem_align)) % elem_align;
                        for (size_t i = 0; i < vpad; i++) zero_cc_emit_u8(&c->data_section, 0);
                        size_t var_offset = c->data_section.length;
                        bool vhas_init = false;
                        ZeroCCType vreg_type = vt;
                        for (int d = vdim_count - 1; d >= 0; d--) {
                            vreg_type = zero_cc_type_array(&vreg_type, (ssize_t)vdims[d]);
                        }
                        if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                            c->token_pos++;
                            vhas_init = true;
                            if (c->tokens[c->token_pos].type == TK_LBRACE) {
                                /* C99 6.7.2.1p3: a trailing flexible-array
                                 * member is initialized past the struct's fixed
                                 * size - grow the object to cover the FAM data
                                 * run (c-torture execute/20010924-1). */
                                size_t fam_extra = (vdim_count == 0 && vptr_depth == 0) ?
                                    zc_measure_fam_init_bytes(c, &vt) : 0;
                                if (fam_extra > 0) vsize += fam_extra;
                                for (size_t i = 0; i < vsize; i++) zero_cc_emit_u8(&c->data_section, 0);
                                uint8_t *vbuf = calloc(vsize > 0 ? vsize : 1, 1);
                                if (vbuf) {
                                    size_t top_cursor = 0;
                                    zero_cc_emit_brace_init_global(c, vreg_type, vbuf, 0, vsize, var_offset, &top_cursor, fam_extra);
                                    memcpy((uint8_t *)c->data_section.data + var_offset, vbuf, vsize);
                                } else {
                                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) c->token_pos++;
                                    if (c->token_pos < c->token_count) c->token_pos++;
                                }
                                free(vbuf);
                            } else if (vptr_depth > 0 &&
                                       (c->tokens[c->token_pos].type == TK_AMP ||
                                        c->tokens[c->token_pos].type == TK_LPAREN ||
                                        c->tokens[c->token_pos].type == TK_STRING ||
                                        c->tokens[c->token_pos].type == TK_NUM)) {
                                /* Sprint 65: pointer initializer on a
                                 * struct-definition declarator -
                                 * `struct c { struct b b; } *cptr = (void *)&val;`,
                                 * `static struct Ustr *s1 = ((struct Ustr *) "");`.
                                 * An optional `(type)` cast prefix is skipped, then:
                                 *   &global   -> data reloc (kind 1 / kind 2)
                                 *   "literal" -> reloc into .rodata (kind 0)
                                 *   number    -> constant (0/NULL) */
                                if (c->tokens[c->token_pos].type == TK_LPAREN) {
                                    size_t q = c->token_pos + 1;
                                    if (q < c->token_count && (c->tokens[q].type == TK_LPAREN || zero_cc_tok_is_type_start(c, q))) {
                                        if (c->tokens[q].type == TK_LPAREN) q++;
                                        int pdepth = 1;
                                        q++;
                                        while (q < c->token_count && pdepth > 0) {
                                            if (c->tokens[q].type == TK_LPAREN) pdepth++;
                                            else if (c->tokens[q].type == TK_RPAREN) pdepth--;
                                            q++;
                                        }
                                        while (q < c->token_count && c->tokens[q].type == TK_STAR) q++;
                                        while (q < c->token_count && c->tokens[q].type == TK_RPAREN) q++;
                                        if (q < c->token_count) c->token_pos = q;
                                    }
                                }
                                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_AMP) {
                                    c->token_pos++;
                                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                                        char ref_name[128];
                                        snprintf(ref_name, sizeof(ref_name), "%s", c->tokens[c->token_pos].str_val);
                                        c->token_pos++;
                                        ZeroCCGlobalVar *ref_gv = find_global_var(c, ref_name);
                                        int ref_idx = ref_gv ? (int)(ref_gv - c->global_vars) : -1;
                                        zero_cc_emit_u64(&c->data_section, 0);
                                        if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                                            ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                                            dr->data_patch_offset = var_offset;
                                            dr->rodata_offset = 0;
                                            if (ref_idx >= 0) {
                                                dr->target_kind = 1;
                                                dr->target_data_offset = ref_gv->data_offset;
                                                dr->target_global_idx = ref_idx;
                                            } else {
                                                dr->target_kind = 2;
                                                dr->target_data_offset = 0;
                                                dr->target_global_idx = -1;
                                                snprintf(dr->target_extern_name,
                                                         sizeof(dr->target_extern_name), "%s", ref_name);
                                            }
                                        }
                                    } else {
                                        while (c->token_pos < c->token_count &&
                                               c->tokens[c->token_pos].type != TK_COMMA &&
                                               c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                                    }
                                } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STRING) {
                                    const char *sv = c->tokens[c->token_pos].allocated_str ?
                                        c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                                    size_t ro_off = c->rodata_section.length;
                                    zero_cc_buf_put_bytes(&c->rodata_section, sv,
                                                          zero_cc_token_str_len(&c->tokens[c->token_pos]) + 1);
                                    c->token_pos++;
                                    zero_cc_emit_u64(&c->data_section, 0);
                                    if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                                        ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                                        dr->data_patch_offset = var_offset;
                                        dr->target_kind = 0; /* .rodata target */
                                        dr->rodata_offset = ro_off;
                                        dr->target_data_offset = 0;
                                        dr->target_global_idx = -1;
                                    }
                                } else {
                                    uint64_t bits = (uint64_t)zero_cc_eval_const_expr(c);
                                    for (size_t b = 0; b < 8; b++) {
                                        zero_cc_emit_u8(&c->data_section, (uint8_t)(bits >> (8 * b)));
                                    }
                                }
                            } else {
                                bool vfl = (vt.tag == TYPE_FLOAT || vt.tag == TYPE_DOUBLE);
                                uint64_t bits;
                                if (vfl) { double dv = zero_cc_eval_const_expr_float(c); memcpy(&bits, &dv, sizeof(bits)); }
                                else { int64_t iv = zero_cc_eval_const_expr(c); bits = (uint64_t)iv; }
                                for (size_t b = 0; b < (elem_size > 8 ? 8 : elem_size); b++) {
                                    zero_cc_emit_u8(&c->data_section, (uint8_t)(bits >> (8 * b)));
                                }
                                for (size_t i = (elem_size > 8 ? 8 : elem_size); i < vsize; i++) zero_cc_emit_u8(&c->data_section, 0);
                            }
                        } else {
                            if (!(vhas_init || vsize < ZERO_CC_BSS_THRESHOLD)) {
                                /* Large zero-init -> .bss (no data-section bytes) */
                                if (vpad > 0 && c->data_section.length >= vpad) c->data_section.length -= vpad;
                                size_t bss_pad = (elem_align - (c->bss_size % elem_align)) % elem_align;
                                c->bss_size += bss_pad;
                                var_offset = c->bss_size;
                                c->bss_size += vsize;
                            } else {
                                for (size_t i = 0; i < vsize; i++) zero_cc_emit_u8(&c->data_section, 0);
                            }
                        }
                        if (add_global_var(c, alias, vreg_type, var_offset, vsize, st_is_static || dcl_is_static, vhas_init) < 0) {
                            diag_report_error_at_token(c, "out of memory registering global variable '%s'", alias);
                        }
                        if (c->tokens[c->token_pos].type == TK_COMMA) {
                            c->token_pos++;
                            bool cstar = (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR &&
                                          c->token_pos + 1 < c->token_count &&
                                          c->tokens[c->token_pos + 1].type == TK_IDENT);
                            if (c->token_pos >= c->token_count ||
                                (c->tokens[c->token_pos].type != TK_IDENT && !cstar)) {
                                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                                break;
                            }
                            int cstar_depth = 0;
                            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                                cstar_depth++;
                                c->token_pos++;
                            }
                            if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) {
                                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                                break;
                            }
                            snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                            c->token_pos++;
                            /* Comma declarator may carry its own pointer/array suffixes */
                            vt = st;
                            for (int cd = 0; cd < cstar_depth; cd++) vt = zero_cc_type_ptr(&vt);
                            while (c->tokens[c->token_pos].type == TK_STAR) { vt = zero_cc_type_ptr(&vt); c->token_pos++; }
                            varray_len = 1; vdim_count = 0;
                            if (c->tokens[c->token_pos].type == TK_LBRACKET) {
                                while (c->tokens[c->token_pos].type == TK_LBRACKET) {
                                    c->token_pos++;
                                    if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                                    else {
                                        int64_t dim = zero_cc_eval_const_expr(c);
                                        if (c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                                        if (dim < 0) dim = 0;
                                        varray_len *= dim;
                                        if (vdim_count < 8) vdims[vdim_count++] = dim;
                                    }
                                }
                            }
                            continue;
                        }
                        break;
                    } while (1);
                }
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        } else if (st_name[0] != '\0') {
            /* Opaque / forward struct typedef or declaration: e.g. typedef struct FILE FILE; */
            ZeroCCType *existing = zero_cc_find_struct(c, st_name);
            ZeroCCType st;
            if (existing) {
                st = *existing;
            } else {
                memset(&st, 0, sizeof(st));
                st.tag = is_union ? TYPE_UNION : TYPE_STRUCT;
                st.size = 8;
                st.align = 8;
                snprintf(st.struct_name, sizeof(st.struct_name), "%s", st_name);
                zero_cc_register_struct(c, st_name, st);
            }
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_CONST ||
                    c->tokens[c->token_pos].type == TK_VOLATILE ||
                    c->tokens[c->token_pos].type == TK_RESTRICT)) {
                c->token_pos++;
            }
            int ptr_depth = 0;
            while (c->tokens[c->token_pos].type == TK_STAR) {
                ptr_depth++;
                c->token_pos++;
                while (c->token_pos < c->token_count &&
                       (c->tokens[c->token_pos].type == TK_CONST ||
                        c->tokens[c->token_pos].type == TK_VOLATILE ||
                        c->tokens[c->token_pos].type == TK_RESTRICT)) {
                    c->token_pos++;
                }
            }
            /* Function pointer typedef over an opaque struct:
             * `typedef struct JSObjectMap *(*Op)(struct JSObject *);`
             * (c-torture pta-1). The skipped stars belong to the
             * declarator `(*Op)`, so the type is pointer-to-function.
             * `typedef struct Foo *Op;` (plain pointer alias) must not
             * take this branch - it requires the `(*` shape. */
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN &&
                c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_STAR) {
                c->token_pos++; /* '(' */
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
                char fp_alias[128] = "";
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                    snprintf(fp_alias, sizeof(fp_alias), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                    int pdepth = 1;
                    c->token_pos++;
                    while (c->token_pos < c->token_count && pdepth > 0) {
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                        else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) pdepth--;
                        c->token_pos++;
                    }
                }
                if (is_typedef && fp_alias[0] != '\0') {
                    /* C99 6.7.5.3: the `ptr_depth` stars before `(` belong
                     * to the RETURN type (a `typedef struct S *(*Op)(args);`
                     * call returns struct S*), so the funcptr's pointee is
                     * the fully-starred type (c-torture pta-1). */
                    ZeroCCType ret_t = st;
                    for (int pd = 0; pd < ptr_depth; pd++) ret_t = zero_cc_type_ptr(&ret_t);
                    ZeroCCType fp_type = zero_cc_type_ptr(&ret_t);
                    fp_type.is_func_ptr = true;
                    zero_cc_register_typedef(c, fp_alias, fp_type);
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                return;
            }
            for (int pd = 0; pd < ptr_depth; pd++) {
                st = zero_cc_type_ptr(&st);
            }
            if (is_typedef) {
                char alias[128] = "";
                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                } else if (ptr_depth == 0) {
                    /* Handle case where st_name itself was the alias */
                    snprintf(alias, sizeof(alias), "%s", st_name);
                }
                if (alias[0] != '\0') {
                    if (st.tag == TYPE_STRUCT || st.tag == TYPE_UNION) {
                        zero_cc_register_struct(c, alias, st);
                    }
                    zero_cc_register_typedef(c, alias, st);
                }
            }
            if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            return;
        }
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

/* Scalar Typedef */
static inline void zero_cc_stmt_typedef_scalar(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    c->token_pos++; /* consume TK_TYPEDEF */
    ZeroCCTokType t = c->tokens[c->token_pos].type;
    ZeroCCType ttype; memset(&ttype, 0, sizeof(ttype));
    bool is_unsigned = false;
    bool is_signed = false;
    bool is_atomic = false;

    /* Handle _Atomic qualifier at the start: typedef _Atomic int x; */
    if (t == TK_ATOMIC) {
        is_atomic = true;
        c->token_pos++;
        t = c->tokens[c->token_pos].type;
    }

    /* Type qualifiers on the typedef base (C99 6.7.5 flexible order): a
     * leading `const`/`volatile` (`typedef const int Q;`, c-torture
     * pr22013-1) was not handled, so the typedef alias was never
     * registered and every later use of the name was an undeclared
     * identifier. Record the leading qualifiers and re-read the base
     * token; trailing qualifiers are handled after the base type. */
    bool td_const = false, td_volatile = false;
    while (t == TK_CONST || t == TK_VOLATILE) {
        if (t == TK_CONST) td_const = true;
        if (t == TK_VOLATILE) td_volatile = true;
        c->token_pos++;
        t = c->tokens[c->token_pos].type;
    }

    /* GNU vector attribute leading the typedef base:
     * `typedef __attribute__((__vector_size__(16))) short V;`
     * Same for mode(X): `typedef __attribute__((mode(QI))) unsigned int V;` */
    size_t vec_attr_size = 0;
    size_t align_attr_val = 0;
    size_t lead_mode_bytes = 0;
    {
        size_t save = c->token_pos;
        size_t probe = 0;
        bool consumed = false;
        /* Markers may chain in any order: consume them all, then require a
         * base-type keyword (rewind otherwise). */
        for (int mk = 0; mk < 4; mk++) {
            bool got = false;
            if (vec_attr_size == 0 &&
                zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
                vec_attr_size = probe;
                got = true;
            }
            if (align_attr_val == 0 &&
                zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                align_attr_val = probe;
                got = true;
            }
            if (zero_cc_parse_mode_attr(c, &c->token_pos, &probe)) {
                lead_mode_bytes = probe;
                got = true;
            }
            if (!got) break;
            consumed = true;
        }
        if (consumed) {
            ZeroCCTokType nt = c->token_pos < c->token_count ? c->tokens[c->token_pos].type : TK_EOF;
            if (nt == TK_INT || nt == TK_CHAR || nt == TK_SHORT || nt == TK_LONG ||
                nt == TK_SIGNED || nt == TK_UNSIGNED || nt == TK_FLOAT_KW ||
                nt == TK_DOUBLE_KW || nt == TK_BOOL_KW || nt == TK_VOID) {
                t = nt;
            } else {
                c->token_pos = save;
                vec_attr_size = 0;
                align_attr_val = 0;
                lead_mode_bytes = 0;
            }
        }
    }

    if (t == TK_UNSIGNED) { is_unsigned = true; c->token_pos++; t = c->tokens[c->token_pos].type; }
    else if (t == TK_SIGNED) { is_signed = true; c->token_pos++; t = c->tokens[c->token_pos].type; }

    if (t == TK_TYPEOF || t == TK_TYPEOF_UNQUAL) {
        ttype = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
    }
    else if (t == TK_IDENT && c->tokens[c->token_pos].str_val &&
             zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
        /* C99 6.7.2.3 prefix complex: `typedef _Complex double C;` */
        c->token_pos++;
        ZeroCCType inner = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
        ttype = zero_cc_type_complex(&inner);
    }
    else if (t == TK_INT) { ttype = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int(); c->token_pos++; }
    else if (t == TK_CHAR) { ttype = is_unsigned ? zero_cc_type_uint8() : zero_cc_type_char(); c->token_pos++; }
    else if (t == TK_VOID) { ttype = zero_cc_type_void(); c->token_pos++; }
    else if (t == TK_FLOAT_KW) { ttype = zero_cc_type_float(); c->token_pos++; }
    else if (t == TK_DOUBLE_KW) { ttype = zero_cc_type_double(); c->token_pos++; }
    else if (t == TK_BOOL_KW) { ttype = zero_cc_type_bool(); c->token_pos++; }
    else if (t == TK_SHORT) {
        c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
        ttype = is_unsigned ? zero_cc_type_uint16() : zero_cc_type_int16();
    }
    else if (t == TK_LONG) {
        c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
        ttype = is_unsigned ? zero_cc_type_uint64() : zero_cc_type_int64();
    }
    else if (t == TK_IDENT) {
        const char *tname = c->tokens[c->token_pos].str_val;
        if (tname && strcmp(tname, "__builtin_va_list") == 0) {
            /* C99/GNU __builtin_va_list: the 40-byte va_list object (see
             * zero_cc_type_va_list). Recognize it directly so
             * `typedef __builtin_va_list va_list;` yields the correct size
             * instead of falling through to an integer type. */
            ttype = zero_cc_type_va_list();
            c->token_pos++;
        } else {
            ZeroCCType *found = zero_cc_find_typedef(c, tname);
            if (found) {
                ttype = *found;
                c->token_pos++;
            } else if (!is_unsigned && !is_signed) {
                ttype = zero_cc_type_int();
                /* C89 implicit-int typedef: `typedef a;` / `typedef f, g;`
                 * - the identifier is the ALIAS of an int typedef, not a
                 * type name (the declaration ends right after it).  Leave
                 * the token unconsumed so the alias reader below picks it
                 * up; otherwise consume it as a defaulted type name. */
                if (!(c->token_pos + 1 < c->token_count &&
                      (c->tokens[c->token_pos + 1].type == TK_SEMI ||
                       c->tokens[c->token_pos + 1].type == TK_COMMA))) {
                    c->token_pos++;
                }
            } else {
                ttype = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
            }
        }
    } else {
        ttype = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
    }
    (void)is_signed;
    if (is_atomic) ttype.is_atomic = true;
    if (lead_mode_bytes > 0) {
        ttype = zero_cc_type_apply_mode(ttype, lead_mode_bytes);
    }
    /* GNU vector attribute trailing the base: `typedef signed char V
     * __attribute__((__vector_size__(16)));` - wraps the element type in a
     * vector ZeroCCType (TYPE_USER_0) with the given total width. */
    for (int mk = 0; mk < 4; mk++) {
        size_t probe = 0;
        bool got = false;
        if (vec_attr_size == 0 && zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
            vec_attr_size = probe;
            got = true;
        }
        if (align_attr_val == 0 && zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
            align_attr_val = probe;
            got = true;
        }
        /* GNU mode(X) trailing the base: `typedef unsigned int
         * __attribute__((mode(QI))) u8;` (c-torture execute/memclr) -
         * resize the integer base before the alias name is read. The loop
         * also handles chained attributes in any order (pr23135: mode +
         * vector_size). */
        if (zero_cc_parse_mode_attr(c, &c->token_pos, &probe)) {
            ttype = zero_cc_type_apply_mode(ttype, probe);
            got = true;
        }
        if (!got) break;
    }
    if (vec_attr_size > 0 && !zero_cc_type_is_vector(&ttype)) {
        ttype = zero_cc_type_vector(&ttype, vec_attr_size);
    }
    /* C99 6.7.2.3 suffix complex: `typedef double _Complex C;` */
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT &&
        c->tokens[c->token_pos].str_val &&
        zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val) &&
        !ttype.is_complex) {
        c->token_pos++;
        ttype = zero_cc_type_complex(&ttype);
    }
    if (align_attr_val > 0 && align_attr_val > ttype.align) {
        ttype.align = align_attr_val;
        if (ttype.size < align_attr_val) ttype.size = align_attr_val;
        if ((ttype.size % align_attr_val) != 0) {
            ttype.size = (ttype.size + align_attr_val - 1) / align_attr_val * align_attr_val;
        }
    }
    ZeroCCType base_ttype = ttype; /* pre-pointer-wrap, per comma declarator */
    /* Trailing type qualifiers before the pointer run (C99 6.7.5):
     * `typedef int const Q;` / `typedef char volatile V;`. */
    if (td_const) ttype.is_const = true;
    if (td_volatile) ttype.is_volatile = true;
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_CONST ||
            c->tokens[c->token_pos].type == TK_VOLATILE)) {
        if (c->tokens[c->token_pos].type == TK_CONST) ttype.is_const = true;
        if (c->tokens[c->token_pos].type == TK_VOLATILE) ttype.is_volatile = true;
        c->token_pos++;
    }
    int ttype_star_count = 0;
    while (c->tokens[c->token_pos].type == TK_STAR) { ttype_star_count++; c->token_pos++; }
    for (int ts = 0; ts < ttype_star_count; ts++) ttype = zero_cc_type_ptr(&ttype);
    bool is_ptr = (ttype_star_count > 0);

    /* Function pointer typedef: typedef RetType (*Alias)(params...);
     * C99 6.7.5.3 spiral rule: the stars BEFORE `(` belong to the RETURN
     * type - `T * (*f)(args)` is a pointer to a function returning T*.
     * The funcptr's pointee must therefore be the fully-starred return
     * type, not the bare base (c-torture pta-1: `JSObject *(*Op)(...)`
     * calls came back as by-value structs and REJECTed on assignment). */
    if (c->tokens[c->token_pos].type == TK_LPAREN) {
        c->token_pos++; /* consume '(' */
        while (c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
        char alias[128] = "";
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) {
            int pdepth = 1;
            c->token_pos++;
            while (c->token_pos < c->token_count && pdepth > 0) {
                if (c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                else if (c->tokens[c->token_pos].type == TK_RPAREN) pdepth--;
                c->token_pos++;
            }
        }
        ZeroCCType fp_type = zero_cc_type_ptr(&ttype);
        fp_type.is_func_ptr = true;
        if (alias[0] != '\0') {
            zero_cc_register_typedef(c, alias, fp_type);
        }
        if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
        return;
    }

    if (c->tokens[c->token_pos].type == TK_IDENT) {
        /* C99 6.7.8: comma-separated declarators
         * (`typedef unsigned char UCHAR, *PUCHAR;`) - each gets its own
         * pointer chain (c-torture pr44831). */
        do {
            char alias[128];
            memset(alias, 0, sizeof(alias));
            snprintf(alias, sizeof(alias), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            /* GNU vector attribute trailing the typedef name (a declarator
             * attribute): `typedef signed char V __attribute__((__vector_size__(16)));`
             * Must be consumed before the function-typedef check below so the
             * marker's `(` is not read as a parameter list. Same for mode(X)
             * in position 2 (`typedef unsigned u32 __attribute__((mode(SI)));` -
             * c-torture cbrt): the marker is skipped, the width stays.
             * (Position-1 mode, i.e. before the base type, is applied above.) */
            {
                size_t probe = 0;
                if (vec_attr_size == 0 && zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
                    vec_attr_size = probe;
                }
                zero_cc_skip_mode_attr(c);
            }
            /* C99 6.7.8: array typedef `typedef T name[N];` / `typedef char A28[28];`
             * Non-constant dimensions are VLA typedefs (C99 6.7.8p5):
             * the alias carries the dimension expression and `sizeof name`
             * is evaluated at runtime (c-torture pr54552-1, 20040411-1). */
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                int64_t t_dims[8];
                bool t_vla[8];
                size_t t_vla_expr[8]; /* ASTRef of the runtime dimension */
                int t_dim_count = 0;
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                    c->token_pos++;
                    int64_t td = 0;
                    bool td_vla = false;
                    size_t td_expr = 0;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACKET) {
                        if (zc_typedef_dim_is_const(c, c->token_pos)) {
                            td = zero_cc_eval_const_expr(c);
                        } else {
                            td_expr = parse_expr_ast_internal(c, arena);
                            td_vla = true;
                        }
                    }
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                        c->token_pos++;
                    } else {
                        /* Malformed dimension: skip to ']' or ';' to recover. */
                        while (c->token_pos < c->token_count &&
                               c->tokens[c->token_pos].type != TK_RBRACKET &&
                               c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                        if (c->token_pos < c->token_count &&
                            c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                        break;
                    }
                    if (td < 0) td = 0;
                    if (t_dim_count < 8) {
                        t_dims[t_dim_count] = td;
                        t_vla[t_dim_count] = td_vla;
                        t_vla_expr[t_dim_count] = td_expr;
                        t_dim_count++;
                    }
                }
                ZeroCCType tt2 = ttype;
                for (int d = t_dim_count - 1; d >= 0; d--) {
                    tt2 = zero_cc_type_array(&tt2, (ssize_t)t_dims[d]);
                }
                /* VLA dimension: mark it, keep the outermost runtime
                 * expression, and put the STATIC factor (element size times
                 * the product of the constant dims) in `size` so
                 * `sizeof name` = vla_dim * size at runtime. */
                if (t_dim_count > 0) {
                    int first_vla = -1;
                    int64_t static_prod = (ttype.size > 0) ? (int64_t)ttype.size : 1;
                    for (int d = 0; d < t_dim_count; d++) {
                        if (t_vla[d]) {
                            if (first_vla < 0) first_vla = d;
                        } else {
                            static_prod *= t_dims[d];
                        }
                    }
                    if (first_vla >= 0) {
                        tt2.is_vla = true;
                        tt2.vla_dim = t_vla_expr[first_vla];
                        tt2.size = (size_t)(static_prod > 0 ? static_prod : 1);
                    }
                }
                ttype = tt2;
            }
            /* Function-type typedef (C99 6.7): `typedef int f(void);` or
             * `typedef int binop(int, int);`.  Consume the balanced parameter
             * list so the declarator is not left dangling at file scope, and
             * mark the alias as a function type; `f *p` then yields a
             * function pointer via zero_cc_chunk_apply. */
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                int pdepth = 1;
                c->token_pos++;
                while (c->token_pos < c->token_count && pdepth > 0) {
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                    else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) pdepth--;
                    c->token_pos++;
                }
                ttype.is_func = true;
            }
            if (vec_attr_size > 0 && !zero_cc_type_is_vector(&ttype)) {
                ttype = zero_cc_type_vector(&ttype, vec_attr_size);
            }
            zero_cc_register_typedef(c, alias, ttype);
            if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_COMMA) break;
            c->token_pos++; /* ',' */
            ttype = base_ttype; /* next declarator: re-wrap its own stars */
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                ttype = zero_cc_type_ptr(&ttype);
                c->token_pos++;
            }
            if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) {
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                return;
            }
        } while (1);
    }
    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
}

#endif /* ZERO_CC_STATEMENTS_DEFINITIONS_H */
