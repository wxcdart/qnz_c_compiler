#ifndef ZERO_CC_JIT_COMPILE_UNIT_H
#define ZERO_CC_JIT_COMPILE_UNIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "zero_cc/core/core.h"
#include "zero_cc/types/types.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/visitor.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/ir/ir_arena.h"
#include "../isa/isa_backend.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/parser/static_assert.h"
#include "zero_cc/parser/definitions.h"

/* Forward declaration */
static inline bool zero_cc_compile_top_level_decl(ZeroCCCompiler *c);

/* Evaluate a floating-point constant initializer (literal, char literal,
 * unary +/-, parenthesized). Advances token_pos past the consumed value. */
static inline double zero_cc_eval_const_expr_float(ZeroCCCompiler *c) {
    double sign = 1.0;
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_PLUS || c->tokens[c->token_pos].type == TK_MINUS)) {
        if (c->tokens[c->token_pos].type == TK_MINUS) sign = -sign;
        c->token_pos++;
    }
    double v = 0.0;
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
        c->token_pos++;
        v = zero_cc_eval_const_expr_float(c);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_FLOAT) {
        v = c->tokens[c->token_pos].float_val;
        c->token_pos++;
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_NUM) {
        v = (double)c->tokens[c->token_pos].num_val;
        c->token_pos++;
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_CHAR_LIT) {
        v = (double)(int)c->tokens[c->token_pos].num_val;
        c->token_pos++;
    }
    return sign * v;
}

/* Emit `width` low bytes of `bits` (little-endian) into a section buffer. */
static inline void zero_cc_emit_scalar_bytes(ZeroCCBuffer *b, size_t width, uint64_t bits) {
    for (size_t i = 0; i < width && i < 8; i++) {
        zero_cc_emit_u8(b, (uint8_t)(bits >> (8 * i)));
    }
}
/* Try to compile a top-level declaration (typedef struct, struct/union, etc.) */
/* Returns true if a top-level declaration was consumed, false otherwise */
static inline bool zero_cc_compile_top_level_decl(ZeroCCCompiler *c) {
    if (c->token_pos >= c->token_count) return false;
    
    ZeroCCTokType t = c->tokens[c->token_pos].type;
    
    /* Typedef: check next token for struct/union/enum vs scalar */
    if (t == TK_TYPEDEF) {
        if (c->token_pos + 1 < c->token_count && 
            (c->tokens[c->token_pos + 1].type == TK_STRUCT ||
             c->tokens[c->token_pos + 1].type == TK_UNION)) {
            zero_cc_stmt_typedef_struct(c);
            return true;
        } else if (c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_ENUM) {
            zero_cc_stmt_typedef_enum(c);
            return true;
        } else {
            zero_cc_stmt_typedef_scalar(c);
            return true;
        }
    }
    
    /* Standalone Enum Declaration / Definition */
    if (t == TK_ENUM) {
        if (c->token_pos + 1 < c->token_count &&
            (c->tokens[c->token_pos + 1].type == TK_LBRACE ||
             (c->tokens[c->token_pos + 1].type == TK_IDENT &&
              c->token_pos + 2 < c->token_count &&
              (c->tokens[c->token_pos + 2].type == TK_LBRACE || c->tokens[c->token_pos + 2].type == TK_SEMI)))) {
            zero_cc_stmt_enum(c);
            return true;
        }
        return false;
    }
    
    /* Struct/Union definition */
    if (t == TK_STRUCT || t == TK_UNION) {
        if (c->token_pos + 1 < c->token_count && 
            (c->tokens[c->token_pos + 1].type == TK_LBRACE ||
             (c->tokens[c->token_pos + 1].type == TK_IDENT &&
              c->token_pos + 2 < c->token_count && c->tokens[c->token_pos + 2].type == TK_LBRACE))) {
            zero_cc_stmt_typedef_struct(c);
            return true;
        }
        /* If not a struct/union definition, it's a variable declaration */
        return false;
    }
    
    return false;
}

/* Master Translation Unit Compiler */
static inline void zero_cc_compile_c99(ZeroCCCompiler *c) {
    zero_cc_type_pool_free();
    zero_cc_buf_init(&c->text_section, 4096);
    zero_cc_buf_init(&c->rodata_section, 1024);
    zero_cc_buf_init(&c->data_section, 1024);
    c->function_count = 0;
    reset_global_vars(); /* Reset global variable state */
    // Create permanent and temporary arenas for semantic analysis
    Arena *perm = arena_create(1024 * 1024);
    Arena *tmp = arena_create(1024 * 1024);
    if (!perm || !tmp) {
        fprintf(stderr, "zero_cc: arena allocation failed\n");
        zero_cc_type_pool_free();
        return;
    }
    c->reloc_count = 0;
    zero_cc_init_standard_types(c);
    c->thread_local_count = 0;
    /* Initialise default TLS backend */
    zero_cc_set_tls_backend(c, NULL);
    /* Initialize register allocator */
    regalloc_init(&c->regalloc);
    c->next_temp_id = 0;

    /* Initialize ISA backend if not already set */
    if (!c->isa_backend) {
        c->isa = zero_cc_get_native_backend()->isa;
        c->isa_backend = zero_cc_get_native_backend();
    }
    
    /* Initialize IR arenas if not already initialized */
    if (!c->ir_arena.buffer) {
        if (!ir_arena_init(&c->ir_arena, 4096)) {
            fprintf(stderr, "Error: Failed to initialize IR arena\n");
            zero_cc_type_pool_free();
            return;
        }
    }
    if (!c->ir_call_args.table) {
        if (!ir_call_args_arena_init(&c->ir_call_args)) {
            fprintf(stderr, "Error: Failed to initialize IR call args arena\n");
            ir_arena_free(&c->ir_arena);
            zero_cc_type_pool_free();
            return;
        }
    }

    /* Create shared semantic context for all functions */
    DiagBuf shared_diags;
    diag_init(&shared_diags, tmp);
    if (c->tokens && c->token_count > 0 && c->tokens[0].filename) {
        diag_set_file(&shared_diags, c->tokens[0].filename);
    }
    /* Initialize compiler's diagnostic buffer */
    diag_init(&c->diag_buf, tmp);
    if (c->tokens && c->token_count > 0 && c->tokens[0].filename) {
        diag_set_file(&c->diag_buf, c->tokens[0].filename);
    }
    SemContext shared_ctx;
    if (!sem_context_init(&shared_ctx, perm, tmp, &shared_diags, NULL)) {
        zero_cc_type_pool_free();
        return;
    }
    c->sem_ctx = &shared_ctx;  /* Save for lowering */
    // Save the global scope pointer (root of scope chain)
    // Register standard library functions if any

    while (c->tokens[c->token_pos].type != TK_EOF && !c->had_error) {
        if (c->tokens[c->token_pos].type == TK_TYPEDEF ||
            (c->tokens[c->token_pos].type == TK_ENUM &&
             (c->token_pos + 1 < c->token_count &&
              (c->tokens[c->token_pos + 1].type == TK_LBRACE ||
               (c->tokens[c->token_pos + 1].type == TK_IDENT &&
                c->token_pos + 2 < c->token_count &&
                (c->tokens[c->token_pos + 2].type == TK_LBRACE || c->tokens[c->token_pos + 2].type == TK_SEMI))))) ||
            ((c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION) &&
             (c->tokens[c->token_pos + 1].type == TK_LBRACE ||
              (c->tokens[c->token_pos + 1].type == TK_IDENT && c->tokens[c->token_pos + 2].type == TK_LBRACE)))) {
            zero_cc_compile_top_level_decl(c);
            if (c->had_error) break;
            continue;
        }
        if (c->tokens[c->token_pos].type == TK_STATIC_ASSERT) {
            zero_cc_stmt_static_assert(c);
            if (c->had_error) break;
            continue;
        }

        /* Extern symbol declarations */
        if (c->tokens[c->token_pos].type == TK_EXTERN) {
            c->token_pos++; /* skip extern */
            size_t after_extern = c->token_pos;
            /* Parse type and identifier */
            ZeroCCType ext_type = zero_cc_parse_type_name(c);
            ZeroCCType base_ext_type = (ext_type.tag == TYPE_PTR && ext_type.base) ? *ext_type.base : ext_type;
            if (c->tokens[c->token_pos].type == TK_IDENT && c->tokens[c->token_pos + 1].type == TK_LPAREN) {
                /* Extern function declaration/prototype: let function parser handle it */
                c->token_pos = after_extern;
            } else if (c->tokens[c->token_pos].type == TK_IDENT) {
                ZeroCCType cur_ext_type = ext_type;
                while (c->tokens[c->token_pos].type == TK_IDENT) {
                    char ext_name[128];
                    snprintf(ext_name, sizeof(ext_name), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                    /* Register as external symbol (no code generation, just declaration) */
                    if (c->function_count < ZERO_CC_MAX_FUNCTIONS) {
                        ZeroCCFunction *fn = &c->functions[c->function_count++];
                        fn->text_offset = 0; /* External, no code */
                        snprintf(fn->name, sizeof(fn->name), "%s", ext_name);
                        fn->return_type = cur_ext_type;
                        fn->is_external = true;
                        fn->is_prototype = true;
                    }
                    if (c->tokens[c->token_pos].type == TK_COMMA) {
                        c->token_pos++;
                        int ptr_d = 0;
                        while (c->tokens[c->token_pos].type == TK_STAR) {
                            ptr_d++;
                            c->token_pos++;
                        }
                        cur_ext_type = base_ext_type;
                        for (int pd = 0; pd < ptr_d; pd++) {
                            cur_ext_type = zero_cc_type_ptr(&cur_ext_type);
                        }
                    } else {
                        break;
                    }
                }
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                continue;
            } else {
                /* Syntax error: skip until semicolon */
                while (c->tokens[c->token_pos].type != TK_EOF && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                continue;
            }
        }

        /* C89-style implicit-int function definition: an unknown identifier
         * directly followed by '(' (`main(){ ... }`, `static f(x){ ... }`) is
         * a function with a defaulted return type, not a variable declaration.
         * Look past storage-class prefixes so both forms are detected. Let the
         * function parser handle it (it warns, per gcc's -Wimplicit-int). */
        bool bare_fn_ident = false;
        {
            size_t probe = c->token_pos;
            while (probe < c->token_count &&
                   (c->tokens[probe].type == TK_STATIC ||
                    c->tokens[probe].type == TK_INLINE ||
                    c->tokens[probe].type == TK_CONST ||
                    c->tokens[probe].type == TK_VOLATILE ||
                    c->tokens[probe].type == TK_NORETURN)) probe++;
            bare_fn_ident = (probe + 1 < c->token_count &&
                c->tokens[probe].type == TK_IDENT &&
                c->tokens[probe + 1].type == TK_LPAREN &&
                !zero_cc_find_typedef(c, c->tokens[probe].str_val) &&
                !zero_cc_find_struct(c, c->tokens[probe].str_val) &&
                !zero_cc_find_enum(c, c->tokens[probe].str_val));
        }

        /* Global variable declarations */
        if (!bare_fn_ident && (
            c->tokens[c->token_pos].type == TK_STATIC ||
            c->tokens[c->token_pos].type == TK_INLINE ||
            c->tokens[c->token_pos].type == TK_CONST ||
            c->tokens[c->token_pos].type == TK_VOLATILE ||
            c->tokens[c->token_pos].type == TK_EXTERN ||
            c->tokens[c->token_pos].type == TK_INT || c->tokens[c->token_pos].type == TK_CHAR ||
            c->tokens[c->token_pos].type == TK_VOID || c->tokens[c->token_pos].type == TK_FLOAT_KW ||
            c->tokens[c->token_pos].type == TK_DOUBLE_KW || c->tokens[c->token_pos].type == TK_BOOL_KW ||
            c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
            c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG ||
            c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION ||
            c->tokens[c->token_pos].type == TK_ENUM ||
            c->tokens[c->token_pos].type == TK_IDENT)) {
            /* Save position before parsing - needed to back up for function definitions */
            size_t global_start = c->token_pos;
            /* Check for static / inline keywords */
            bool is_static = false;
            while (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_STATIC ||
                   c->tokens[c->token_pos].type == TK_INLINE ||
                   c->tokens[c->token_pos].type == TK_CONST ||
                   c->tokens[c->token_pos].type == TK_VOLATILE ||
                   c->tokens[c->token_pos].type == TK_EXTERN)) {
                if (c->tokens[c->token_pos].type == TK_STATIC) {
                    is_static = true;
                }
                c->token_pos++;
            }
            /* Parse type */
            ZeroCCType gtype = zero_cc_parse_type_name(c);
            /* Pointer declarators: int *p;  char **pp;  (qualifiers after stars are skipped) */
            int ptr_depth = 0;
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                ptr_depth++;
                c->token_pos++;
                while (c->token_pos < c->token_count &&
                       (c->tokens[c->token_pos].type == TK_CONST || c->tokens[c->token_pos].type == TK_VOLATILE)) {
                    c->token_pos++;
                }
            }
            for (int pd = 0; pd < ptr_depth; pd++) {
                gtype = zero_cc_type_ptr(&gtype);
            }
            /* Expect identifier */
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                /* Check if this is a function (followed by '(') or variable */
                if (c->tokens[c->token_pos + 1].type == TK_LPAREN) {
                    /* This is a function definition, back up to start and let function parser handle it */
                    c->token_pos = global_start;
                } else {
                    /* This is a variable declaration (may declare several names: int a, b = 3;) */
                    size_t elem_size = gtype.size > 0 ? gtype.size : 8;
                    size_t elem_align = gtype.align > 0 ? gtype.align : 8;
                    if (elem_align < 8) elem_align = 8;
                    do {
                        char var_name[128];
                        snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
                        c->token_pos++;

                        /* Optional array suffix: name[N] */
                        int64_t array_len = 1;
                        bool is_char_array = (ptr_depth == 0 && gtype.tag == TYPE_INT8);
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                            c->token_pos++;
                            array_len = zero_cc_eval_const_expr(c);
                            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                            if (array_len <= 0) array_len = 1;
                        }
                        size_t size = (size_t)array_len * elem_size;

                        /* Align to alignment boundary */
                        size_t padding = (elem_align - (c->data_section.length % elem_align)) % elem_align;
                        for (size_t i = 0; i < padding; i++) {
                            zero_cc_emit_u8(&c->data_section, 0);
                        }
                        size_t var_offset = c->data_section.length;
                        bool has_initializer = false;

                        /* Write one scalar (int bits or IEEE float bits) of the
                         * given width into the data section. */
                        #define CV_WRITE_SCALAR(width, is_f, fval, ival) do { \
                            uint64_t _bits; \
                            if (is_f) { double _d = (fval); memcpy(&_bits, &_d, sizeof(_bits)); } \
                            else { _bits = (uint64_t)(ival); } \
                            zero_cc_emit_scalar_bytes(&c->data_section, (width), _bits); \
                        } while (0)

                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ASSIGN) {
                            c->token_pos++; /* skip = */
                            has_initializer = true;
                            ZeroCCTokType it = c->tokens[c->token_pos].type;
                            if (it == TK_STRING && !is_char_array) {
                                /* Pointer to string: store rodata address via data reloc */
                                const char *sv = c->tokens[c->token_pos].allocated_str ?
                                    c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                                size_t ro_off = c->rodata_section.length;
                                zero_cc_buf_put_bytes(&c->rodata_section, sv, strlen(sv) + 1);
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
                                size = 8;
                            } else if (it == TK_LBRACE) {
                                /* Aggregate initializer { ... } (arrays & structs,
                                 * positional and designated). Write into a
                                 * zero-cleared scratch buffer so struct member
                                 * offsets/padding are correct, then append. */
                                c->token_pos++; /* { */
                                bool is_struct = (gtype.tag == TYPE_STRUCT || gtype.tag == TYPE_UNION);
                                uint8_t *vbuf = calloc(size > 0 ? size : 1, 1);
                                if (vbuf) {
                                    size_t pos_off = 0;      /* positional array byte offset */
                                    size_t next_member = 0;  /* positional struct member idx */
                                    while (c->token_pos < c->token_count &&
                                           c->tokens[c->token_pos].type != TK_RBRACE) {
                                        const char *field_name = NULL;
                                        size_t iter_start_pos = c->token_pos;
                                        size_t m_off = 0, m_sz = 0;
                                        bool m_float = false;
                                        if (c->tokens[c->token_pos].type == TK_DOT) {
                                            /* Designated initializer: .field = */
                                            c->token_pos++;
                                            if (c->token_pos < c->token_count &&
                                                c->tokens[c->token_pos].type == TK_IDENT) {
                                                field_name = c->tokens[c->token_pos].str_val;
                                                c->token_pos++;
                                            }
                                            if (field_name && is_struct) {
                                                bool found = false;
                                                for (size_t m = 0; m < gtype.member_count; m++) {
                                                    if (strcmp(gtype.members[m].name, field_name) == 0) {
                                                        m_off = gtype.members[m].offset;
                                                        m_sz = gtype.members[m].size ? gtype.members[m].size : 8;
                                                        m_float = (gtype.members[m].tag == TYPE_FLOAT ||
                                                                   gtype.members[m].tag == TYPE_DOUBLE);
                                                        found = true;
                                                        break;
                                                    }
                                                }
                                                if (!found) break;
                                            } else {
                                                /* Array designator [N] = : skip it */
                                                while (c->token_pos < c->token_count &&
                                                       c->tokens[c->token_pos].type != TK_ASSIGN) c->token_pos++;
                                                if (c->token_pos < c->token_count) c->token_pos++; /* = */
                                                break;
                                            }
                                            if (c->token_pos < c->token_count &&
                                                c->tokens[c->token_pos].type == TK_ASSIGN) c->token_pos++;
                                        }
                                        if (c->token_pos >= c->token_count ||
                                            c->tokens[c->token_pos].type == TK_RBRACE) break;

                                        size_t e_off, e_sz;
                                        bool e_float;
                                        if (is_struct) {
                                            if (field_name) {
                                                e_off = m_off; e_sz = m_sz; e_float = m_float;
                                            } else {
                                                if (next_member >= gtype.member_count) break;
                                                e_off = gtype.members[next_member].offset;
                                                e_sz = gtype.members[next_member].size ? gtype.members[next_member].size : 8;
                                                e_float = (gtype.members[next_member].tag == TYPE_FLOAT ||
                                                           gtype.members[next_member].tag == TYPE_DOUBLE);
                                                next_member++;
                                            }
                                        } else {
                                            e_off = pos_off;
                                            e_sz = elem_size;
                                            e_float = (gtype.tag == TYPE_FLOAT || gtype.tag == TYPE_DOUBLE);
                                        }
                                        if (e_off >= size) break;

                                        ZeroCCTokType vt = c->tokens[c->token_pos].type;
                                        if (vt == TK_STRING && is_char_array && e_off == 0) {
                                            /* char array from string literal (truncated to fit) */
                                            const char *sv = c->tokens[c->token_pos].allocated_str ?
                                                c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                                            size_t sl = strlen(sv);
                                            size_t maxc = (sl + 1 <= size) ? (sl + 1) : size;
                                            memcpy(vbuf + e_off, sv, maxc);
                                            c->token_pos++;
                                        } else if (vt == TK_FLOAT || (e_float && vt == TK_NUM)) {
                                            double dv = zero_cc_eval_const_expr_float(c);
                                            uint64_t bits;
                                            /* Float members (4 bytes) must store the
                                             * float pattern, not the low 32 bits
                                             * of the double. */
                                            if (e_sz <= 4) {
                                                float fv = (float)dv;
                                                memcpy(&bits, &fv, sizeof(bits));
                                            } else {
                                                memcpy(&bits, &dv, sizeof(bits));
                                            }
                                            size_t w = (e_sz < 8) ? e_sz : 8;
                                            if (e_off + w > size) w = size - e_off;
                                            for (size_t i = 0; i < w; i++) vbuf[e_off + i] = (uint8_t)(bits >> (8 * i));
                                        } else {
                                            int64_t iv = zero_cc_eval_const_expr(c);
                                            size_t w = (e_sz < 8) ? e_sz : 8;
                                            if (e_off + w > size) w = size - e_off;
                                            for (size_t i = 0; i < w; i++) vbuf[e_off + i] = (uint8_t)((uint64_t)iv >> (8 * i));
                                        }
                                        if (!is_struct) pos_off += elem_size;
                                        if (c->token_pos == iter_start_pos) break; /* safety: no progress */
                                        if (c->token_pos < c->token_count &&
                                            c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
                                    }
                                    if (c->token_pos < c->token_count &&
                                        c->tokens[c->token_pos].type == TK_RBRACE) {
                                        c->token_pos++; /* } */
                                    }
                                    zero_cc_buf_put_bytes(&c->data_section, vbuf, size);
                                } else {
                                    /* Allocation failed: zero-fill and skip to } */
                                    for (size_t i = 0; i < size; i++) zero_cc_emit_u8(&c->data_section, 0);
                                    while (c->token_pos < c->token_count &&
                                           c->tokens[c->token_pos].type != TK_RBRACE) c->token_pos++;
                                    if (c->token_pos < c->token_count) c->token_pos++;
                                }
                                free(vbuf);
                            } else if (it == TK_STRING && is_char_array) {
                                /* char array directly initialized from a string literal
                                 * (truncated to fit, NUL-terminated when there is room).
                                 * Also handles size inference for `char g[] = "..."`. */
                                const char *sv = c->tokens[c->token_pos].allocated_str ?
                                    c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                                c->token_pos++;
                                size_t sl = strlen(sv);
                                size_t total = size;
                                if (array_len == 1 && sl + 1 > size) total = sl + 1;
                                size_t copy = sl < total ? sl : total;
                                zero_cc_buf_put_bytes(&c->data_section, sv, copy);
                                for (size_t i = copy; i < total; i++) {
                                    zero_cc_emit_u8(&c->data_section, 0);
                                }
                                size = total;
                                if (total != (size_t)array_len) array_len = (int64_t)total; /* char g[] = "..." */
                            } else if (it == TK_AMP) {
                                /* Address-of a previously declared global:
                                 * `int *p = &x;` -> 8-byte pointer patched
                                 * at link/patch time to x's data address. */
                                c->token_pos++; /* & */
                                if (c->token_pos < c->token_count &&
                                    c->tokens[c->token_pos].type == TK_IDENT) {
                                    char ref_name[128];
                                    snprintf(ref_name, sizeof(ref_name), "%s", c->tokens[c->token_pos].str_val);
                                    c->token_pos++;
                                    ZeroCCGlobalVar *ref_gv = find_global_var(ref_name);
                                    int ref_idx = ref_gv ? (int)(ref_gv - global_vars) : -1;
                                    zero_cc_emit_u64(&c->data_section, 0);
                                    if (ref_idx >= 0 && c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                                        ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                                        dr->data_patch_offset = var_offset;
                                        dr->target_kind = 1; /* .data global target */
                                        dr->rodata_offset = 0;
                                        dr->target_data_offset = ref_gv->data_offset;
                                        dr->target_global_idx = ref_idx;
                                    }
                                    size = 8;
                                } else {
                                while (c->token_pos < c->token_count &&
                                       c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                                for (size_t i = 0; i < size; i++) zero_cc_emit_u8(&c->data_section, 0);
                                }
                            } else if (it == TK_FLOAT ||
                                       (elem_size == 4 && gtype.tag == TYPE_FLOAT) ||
                                       (elem_size == 8 && gtype.tag == TYPE_DOUBLE)) {
                                /* Float/double scalar initializer: emit IEEE-754 bits.
                                 * For a 4-byte float, convert to float first so the
                                 * stored bits are the float pattern (not the low
                                 * 32 bits of the double, which are often zero). */
                                double dv = zero_cc_eval_const_expr_float(c);
                                uint64_t bits;
                                if (elem_size <= 4) {
                                    float fv = (float)dv;
                                    memcpy(&bits, &fv, sizeof(bits));
                                } else {
                                    memcpy(&bits, &dv, sizeof(bits));
                                }
                                zero_cc_emit_scalar_bytes(&c->data_section, elem_size > 8 ? 8 : elem_size, bits);
                                for (size_t i = elem_size; i < size; i++) {
                                    zero_cc_emit_u8(&c->data_section, 0);
                                }
                            } else {
                                /* Integer scalar initializer */
                                int64_t init_val = zero_cc_eval_const_expr(c);
                                zero_cc_emit_scalar_bytes(&c->data_section, elem_size > 8 ? 8 : elem_size, (uint64_t)init_val);
                                for (size_t i = elem_size; i < size; i++) {
                                    zero_cc_emit_u8(&c->data_section, 0);
                                }
                            }
                        } else {
                            /* Zero-initialized data */
                            for (size_t i = 0; i < size; i++) {
                                zero_cc_emit_u8(&c->data_section, 0);
                            }
                        }
                        #undef CV_WRITE_SCALAR

                        /* Consume trailing comma (multi-declarator) or semicolon */
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
                            /* Register this declarator before parsing the next */
                            ZeroCCType reg_type = gtype;
                            if (array_len > 1) reg_type = zero_cc_type_array(&gtype, (ssize_t)array_len);
                            add_global_var(var_name, reg_type, var_offset, size, is_static, has_initializer);
                            c->token_pos++;
                            if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) {
                                /* malformed: skip to end of declaration */
                                while (c->token_pos < c->token_count &&
                                       c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                                break;
                            }
                            continue; /* next declarator with same base type */
                        }
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                        {
                            ZeroCCType reg_type = gtype;
                            if (array_len > 1) reg_type = zero_cc_type_array(&gtype, (ssize_t)array_len);
                            add_global_var(var_name, reg_type, var_offset, size, is_static, has_initializer);
                        }
                        break; /* done with this declaration statement */
                    } while (1);
                    continue; /* Skip function parser for variables */
                }
            } else {
                /* Syntax error: skip until semicolon */
                while (c->tokens[c->token_pos].type != TK_EOF && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                continue;
            }
            /* If we got here, it's a function - don't continue, let it fall through to function parser */
        }

        /* Thread‑local variable handling via backend */
        if (c->tokens[c->token_pos].type == TK_THREAD_LOCAL) {
            // Skip the _Thread_local token
            c->token_pos++;
            // Parse the type (may include pointers, etc.)
            ZeroCCType vtype = zero_cc_parse_type_name(c);
            // Expect identifier
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char var_name[128];
                snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                int64_t init_val = 0;
                // Optional initializer
                if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                    c->token_pos++;
                    init_val = zero_cc_eval_const_expr(c);
                }
                // Consume trailing semicolon
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                // Register via TLS backend
                if (c->tls_backend) {
                    c->tls_backend->declare(c, var_name, &vtype, init_val);
                }
            } else {
                // Syntax error: skip until semicolon
                while (c->tokens[c->token_pos].type != TK_EOF && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            }
            continue;
        }

        size_t prev_pos = c->token_pos;
        /* Try to compile top-level declarations (typedef struct, etc.) first */
        if (zero_cc_compile_top_level_decl(c)) {
            continue;
        }
        /* Top level functions */
        zero_cc_compile_function(c, &shared_ctx, perm);
        if (c->had_error) break;
        if (c->token_pos == prev_pos) {
            c->token_pos++;
        }
    }
    // End of compilation
    /* Merge semantic diagnostics into compiler buffer */
    diag_merge(&c->diag_buf, &shared_diags);
    /* Sync diagnostics to compiler for backward compatibility */
    diag_sync_compiler(c);
    /* Print diagnostics to stderr */
    diag_print_stderr(&c->diag_buf);

    sem_context_destroy(&shared_ctx);
    arena_destroy(tmp);
    arena_destroy(perm);
    zero_cc_type_pool_free();
}


#endif /* ZERO_CC_JIT_COMPILE_UNIT_H */
