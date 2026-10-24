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
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/parser/static_assert.h"
#include "zero_cc/parser/definitions.h"
#include "zero_cc/parser/init_common.h"

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
/* Sprint 44: recursive aggregate-initializer writer for global/static
 * initializers (C99 6.7.8/6.7.9).  Fills buf[buf_off .. buf_off+obj_type.size)
 * from the brace list at c->token_pos (pointing at the opening '{').
 *
 * Nested sub-aggregate designators (`.m = { .f = v }`), element designators
 * (`.m[i]`, `[i..j]` ranges), designator chains (`.a.b = v`) and elided
 * braces all resolve through the shared engine in parser/init_common.h,
 * keeping offsets identical to the AST/lowering path.
 * `data_patch_off` is the .data offset that receives data-reloc patches for
 * string pointers (element 0 of the object). */
static void zero_cc_emit_brace_init_global(ZeroCCCompiler *c, ZeroCCType obj_type,
                                           uint8_t *buf, size_t buf_off, size_t buf_cap,
                                           size_t data_patch_off, size_t *cursor) {
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) c->token_pos++;
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) {
        size_t iter_start = c->token_pos;
        bool has_desig = false;
        bool is_range = false;
        int64_t desig_lo = 0, desig_hi = 0;
        size_t step = 0;
        ZcInitTarget tgt;
        memset(&tgt, 0, sizeof(tgt));
        if (!zc_init_parse_designators(c, &c->token_pos, &obj_type, buf_off, &tgt,
                                       &has_desig, &is_range, &desig_lo, &desig_hi, &step)) {
            int depth = 0;
            while (c->token_pos < c->token_count) {
                if (c->tokens[c->token_pos].type == TK_LBRACE) depth++;
                else if (c->tokens[c->token_pos].type == TK_RBRACE) {
                    if (depth == 0) break;
                    depth--;
                }
                c->token_pos++;
            }
            break;
        }
        if (!has_desig) {
            size_t cur = cursor ? *cursor : 0;
            if (!zc_init_resolve_positional(&obj_type, &cur, buf_off, &tgt)) {
                /* Excess elements: warn, stop (the rest stays zero). */
                diag_report_warning_at_token(c, "excess elements in aggregate initializer");
                int depth = 0;
                while (c->token_pos < c->token_count) {
                    if (c->tokens[c->token_pos].type == TK_LBRACE) depth++;
                    else if (c->tokens[c->token_pos].type == TK_RBRACE) {
                        if (depth == 0) break;
                        depth--;
                    }
                    c->token_pos++;
                }
                break;
            }
            if (cursor) *cursor = cur;
        }

        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACE) {
            /* Sub-aggregate value; transparent only for POSITIONAL scalar
             * targets (grouping braces, C99 6.7.9).  Designated scalar
             * targets (`.arr[i] = { v }`) take the sub-list path so that
             * ranges re-apply the value per element. */
            bool transparent = tgt.is_scalar && !has_desig;
            ZeroCCType sub_type = transparent ? obj_type : tgt.val_type;
            size_t sub_start = c->token_pos;
            size_t sub_cursor = 0;
            zero_cc_emit_brace_init_global(c, sub_type, buf,
                                           transparent ? buf_off : tgt.abs_off,
                                           buf_cap, data_patch_off,
                                           transparent ? cursor : &sub_cursor);
            if (cursor && !transparent) {
                if (has_desig) {
                    size_t cnt = is_range ? (size_t)(desig_hi - desig_lo + 1) : 1;
                    size_t nc = tgt.leaf_idx + cnt * tgt.advance;
                    if (nc > *cursor) *cursor = nc;
                } else {
                    *cursor += tgt.advance;
                }
            }
            if (is_range && !transparent) {
                for (int64_t k = desig_lo + 1; k <= desig_hi; k++) {
                    c->token_pos = sub_start;
                    sub_cursor = 0;
                    zero_cc_emit_brace_init_global(c, sub_type, buf,
                                                   tgt.abs_off + (size_t)(k - desig_lo) * step,
                                                   buf_cap, data_patch_off, &sub_cursor);
                }
            }
        } else if (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type == TK_STRING) {
            if (tgt.arr_ctx_size > 0) {
                /* String into a char array: copy (NUL-terminated when it
                 * fits), starting at the value target. */
                const char *sv = c->tokens[c->token_pos].allocated_str ?
                    c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                size_t sl = strlen(sv);
                size_t off_in_ctx = (tgt.abs_off >= tgt.arr_ctx_off) ?
                    (tgt.abs_off - tgt.arr_ctx_off) : 0;
                size_t cap = (off_in_ctx < tgt.arr_ctx_size) ?
                    (tgt.arr_ctx_size - off_in_ctx) : 0;
                size_t copy = (sl + 1 <= cap) ? (sl + 1) : cap;
                size_t base = tgt.abs_off;
                for (size_t bi = 0; bi < copy; bi++) {
                    if (base + bi < buf_cap) buf[base + bi] = (bi < sl) ? (uint8_t)sv[bi] : 0;
                }
                c->token_pos++;
                if (cursor) {
                    /* A string fully consumes an aggregate char-array
                     * target (row / member) and a char-array member; into
                     * a top-level char-array element it consumes just the
                     * copied bytes. */
                    size_t str_adv = tgt.is_scalar ?
                        (tgt.in_member ? tgt.arr_ctx_size : copy) : tgt.advance;
                    if (has_desig) {
                        size_t nc = tgt.leaf_idx + str_adv;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += str_adv;
                    }
                }
            } else if (tgt.val_type.tag == TYPE_PTR ||
                       (tgt.is_scalar && tgt.val_type.tag == TYPE_INT64 &&
                        obj_type.tag == TYPE_ARRAY &&
                        obj_type.base && obj_type.base->tag == TYPE_PTR)) {
                /* Pointer element: string literal -> rodata address patched
                 * by a data reloc (the slot stays zero until patched). */
                const char *sv = c->tokens[c->token_pos].allocated_str ?
                    c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                size_t ro_off = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, sv, strlen(sv) + 1);
                c->token_pos++;
                if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                    ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                    dr->data_patch_offset = data_patch_off + tgt.abs_off;
                    dr->target_kind = 0; /* .rodata target */
                    dr->rodata_offset = ro_off;
                    dr->target_data_offset = 0;
                    dr->target_global_idx = -1;
                }
                if (cursor) {
                    if (has_desig) {
                        size_t nc = tgt.leaf_idx + 1;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += tgt.advance;
                    }
                }
            } else {
                c->token_pos++;
                if (cursor) {
                    if (has_desig) {
                        size_t nc = tgt.leaf_idx + 1;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += 1;
                    }
                }
            }
        } else {
            /* Scalar constant value (int/char/float/enum/sizeof). */
            bool is_f = (c->tokens[c->token_pos].type == TK_FLOAT ||
                         (tgt.val_type.tag == TYPE_FLOAT || tgt.val_type.tag == TYPE_DOUBLE));
            /* Elided braces: a scalar into an aggregate target fills the
             * first leaf of that aggregate (C99 6.7.9p14). */
            size_t off = tgt.abs_off;
            size_t w = tgt.is_scalar ? tgt.scalar_sz : 0;
            if (!tgt.is_scalar) {
                size_t loff = 0, lsz = 0;
                zc_object_resolve_scalar_leaf(&tgt.val_type, tgt.local_leaf, &loff, &lsz);
                off += loff;
                w = lsz ? lsz : 4;
                if (cursor) {
                    if (has_desig) {
                        size_t cnt = is_range ? (size_t)(desig_hi - desig_lo + 1) : 1;
                        size_t nc = tgt.leaf_idx + cnt;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += 1; /* one leaf, not the whole sub-object */
                    }
                }
            } else {
                if (w > 8) w = 8;
                if (w == 0) w = 4;
                if (cursor) {
                    if (has_desig) {
                        size_t cnt = is_range ? (size_t)(desig_hi - desig_lo + 1) : 1;
                        size_t nc = tgt.leaf_idx + cnt;
                        if (nc > *cursor) *cursor = nc;
                    } else {
                        *cursor += tgt.advance;
                    }
                }
            }
            if (is_f) {
                double dv = zero_cc_eval_const_expr_float(c);
                uint64_t bits = 0;
                if (w <= 4) {
                    float fv = (float)dv;
                    memcpy(&bits, &fv, sizeof(fv));
                } else {
                    memcpy(&bits, &dv, sizeof(dv));
                }
                for (int64_t k = 0; k <= (is_range ? (desig_hi - desig_lo) : 0); k++) {
                    size_t eo = off + (size_t)k * step;
                    for (size_t i = 0; i < w && i < 8 && eo + i < buf_cap; i++) {
                        buf[eo + i] = (uint8_t)(bits >> (8 * i));
                    }
                }
            } else {
                int64_t iv = zero_cc_eval_const_expr(c);
                for (int64_t k = 0; k <= (is_range ? (desig_hi - desig_lo) : 0); k++) {
                    size_t eo = off + (size_t)k * step;
                    for (size_t i = 0; i < w && i < 8 && eo + i < buf_cap; i++) {
                        buf[eo + i] = (uint8_t)((uint64_t)iv >> (8 * i));
                    }
                }
            }
        }
        if (c->token_pos == iter_start) break; /* safety: no progress */
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
    }
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
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

    /* Initialize translation-unit AST arena for inlinable functions */
    if (!c->tu_ast_arena) {
        ZeroCCASTArena *tu_arena = (ZeroCCASTArena *)malloc(sizeof(ZeroCCASTArena));
        if (tu_arena) {
            zero_cc_ast_arena_init(tu_arena, 1024);
            c->tu_ast_arena = tu_arena;
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

    size_t file_scope_prev_pos = (size_t)-1; /* sentinel: first iteration always has "progress" */
    /* Multi-error recovery (dev-plan §11): do not stop at the first error.
     * Continue parsing/compiling subsequent top-level declarations until the
     * error budget is spent, so a broken file reports all its faults in one
     * pass.  had_error stays set, so rc/error_msg behavior is unchanged. */
    while (c->tokens[c->token_pos].type != TK_EOF &&
           c->diag_buf.error_count < c->diag_buf.error_budget &&
           shared_diags.error_count < shared_diags.error_budget) {
        /* Hang guard: if the previous iteration consumed no tokens (a
         * handler that gave up without erroring, e.g. on truncated or
         * unsupported input), report and advance instead of looping
         * forever. */
        if (c->token_pos == file_scope_prev_pos) {
            diag_report_error_at_token(c, "unexpected token at file scope");
            c->token_pos++;
        }
        file_scope_prev_pos = c->token_pos;
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
            continue;
        }
        if (c->tokens[c->token_pos].type == TK_STATIC_ASSERT) {
            zero_cc_stmt_static_assert(c);
            continue;
        }

        /* Extern symbol declarations */
        if (c->tokens[c->token_pos].type == TK_EXTERN) {
            c->token_pos++; /* skip extern */
            if (c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_THREAD_LOCAL) {
                c->token_pos++; /* skip _Thread_local / __thread */
            }
            size_t after_extern = c->token_pos;
            /* Anonymous struct/union extern: `extern struct { ... } name;` */
            bool ext_is_union = (c->tokens[c->token_pos].type == TK_UNION);
            /* Parse type and identifier */
            ZeroCCType ext_type = zero_cc_parse_type_name(c);
            if (c->tokens[c->token_pos].type == TK_LBRACE) {
                /* Consume the body so the member layout is known; the
                 * declarator name follows the closing brace. */
                ext_type = zero_cc_parse_struct_body(c, ext_is_union);
            }
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
                /* Syntax error: report error and skip until semicolon */
                diag_report_error_at_token(c, "expected identifier in extern declaration");
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
            c->tokens[c->token_pos].type == TK_ATOMIC ||
            c->tokens[c->token_pos].type == TK_TYPEOF || c->tokens[c->token_pos].type == TK_TYPEOF_UNQUAL ||
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
            /* Tag-only forward declaration: `struct A;` / `union B;` /
             * `enum C;`.  Legal C, ubiquitous in shared headers; nothing to
             * register (the tag is created by the later definition). */
            if ((c->tokens[c->token_pos].type == TK_STRUCT ||
                 c->tokens[c->token_pos].type == TK_UNION ||
                 c->tokens[c->token_pos].type == TK_ENUM) &&
                c->token_pos + 2 < c->token_count &&
                c->tokens[c->token_pos + 1].type == TK_IDENT &&
                c->tokens[c->token_pos + 2].type == TK_SEMI) {
                c->token_pos += 3;
                continue;
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
                    if (gtype.tag == TYPE_VOID) {
                        diag_report_error_at_token(c, "variable declared with void type");
                        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                        continue;
                    }
                    size_t elem_size = gtype.size > 0 ? gtype.size : 8;
                    size_t elem_align = gtype.align > 0 ? gtype.align : 8;
                    if (elem_align < 8) elem_align = 8;
                    do {
                        char var_name[128];
                        snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
                        c->token_pos++;

                        /* Optional array suffix: name[N] */
                        int64_t array_len = 1;
                        int64_t dims[8]; int dim_count = 0; /* per-dimension sizes (C99 6.7.9) */
                        bool array_unsized = false;
                        bool has_array_suffix = false;
                        bool is_char_array = (ptr_depth == 0 &&
                                              (gtype.tag == TYPE_INT8 || gtype.tag == TYPE_UINT8));
                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                            has_array_suffix = true;
                            /* Multi-dimensional: T name[d1][d2]... (C99 6.7.9) */
                            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                                c->token_pos++;
                                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                                    /* Unsized: T name[] = { ... } (C99 6.7.9) - the
                                     * size is implied by the initializer. */
                                    c->token_pos++;
                                    array_unsized = true;
                                    if (dim_count < 8) dims[dim_count++] = 1; /* patched after counting */
                                    break;
                                }
                                int64_t dim = zero_cc_eval_const_expr(c);
                                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                                if (dim <= 0) dim = 1;
                                array_len *= dim;
                                if (dim_count < 8) dims[dim_count++] = dim;
                            }
                        }
                        if (array_unsized &&
                            c->token_pos + 1 < c->token_count &&
                            c->tokens[c->token_pos].type == TK_ASSIGN &&
                            c->tokens[c->token_pos + 1].type == TK_LBRACE) {
                            /* Count top-level elements of the brace list */
                            size_t q = c->token_pos + 2;
                            int64_t commas = 0;
                            int depth = 1;
                            bool empty = true;
                            while (q < c->token_count && depth > 0) {
                                ZeroCCTokType qt = c->tokens[q].type;
                                if (qt == TK_LBRACE) {
                                    depth++;
                                } else if (qt == TK_RBRACE) {
                                    depth--;
                                    if (depth == 0) break;
                                } else if (depth == 1) {
                                    empty = false;
                                    if (qt == TK_COMMA) commas++;
                                }
                                q++;
                            }
                            array_len = empty ? 1 : (commas + 1);
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
                            bool is_addr_of_clit = (it == TK_AMP);
                            size_t clit_lparen = is_addr_of_clit ? (c->token_pos + 1) : c->token_pos;
                            bool is_clit = false;
                            ZeroCCType clit_type = {0};
                            size_t clit_brace_pos = 0;
                            if (clit_lparen < c->token_count && c->tokens[clit_lparen].type == TK_LPAREN &&
                                clit_lparen + 1 < c->token_count &&
                                zero_cc_tok_is_type_start(c, clit_lparen + 1)) {
                                size_t type_pos = clit_lparen + 1;
                                clit_type = zero_cc_parse_type_name_at_pos(c, &type_pos);
                                if (type_pos < c->token_count && c->tokens[type_pos].type == TK_RPAREN &&
                                    type_pos + 1 < c->token_count && c->tokens[type_pos + 1].type == TK_LBRACE) {
                                    is_clit = true;
                                    clit_brace_pos = type_pos + 1;
                                }
                            }

                            if (is_clit && !is_addr_of_clit && gtype.tag != TYPE_PTR && (gtype.tag == TYPE_STRUCT || gtype.tag == TYPE_UNION)) {
                                c->token_pos = clit_brace_pos;
                                it = TK_LBRACE;
                            }
                            if (is_clit && (is_addr_of_clit || gtype.tag == TYPE_PTR)) {
                                /* Pointer initialized from compound literal: emit 8-byte pointer slot */
                                c->token_pos = clit_brace_pos; /* at '{' */
                                zero_cc_emit_u64(&c->data_section, 0);
                                size = 8;
                                static int s_anon_clit_id = 0;
                                char anon_name[48];
                                snprintf(anon_name, sizeof(anon_name), "__clit_anon_%d", s_anon_clit_id++);

                                if (clit_type.tag == TYPE_STRUCT || clit_type.tag == TYPE_UNION) {
                                    size_t clit_sz = clit_type.size ? clit_type.size : 8;
                                    size_t clit_al = clit_type.align ? clit_type.align : 8;
                                    size_t clit_pad = (clit_al - (c->data_section.length % clit_al)) % clit_al;
                                    for (size_t p = 0; p < clit_pad; p++) zero_cc_emit_u8(&c->data_section, 0);
                                    size_t anon_offset = c->data_section.length;
                                    c->token_pos++; /* skip '{' */
                                    uint8_t *abuf = calloc(clit_sz > 0 ? clit_sz : 1, 1);
                                    size_t next_m = 0;
                                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) {
                                        if (next_m >= clit_type.member_count) break;
                                        size_t m_off = clit_type.members[next_m].offset;
                                        size_t m_sz = clit_type.members[next_m].size ? clit_type.members[next_m].size : 8;
                                        bool m_flt = (clit_type.members[next_m].tag == TYPE_FLOAT || clit_type.members[next_m].tag == TYPE_DOUBLE);
                                        next_m++;
                                        if (m_flt) {
                                            double dv = zero_cc_eval_const_expr_float(c);
                                            uint64_t bits = 0;
                                            if (m_sz <= 4) { float fv = (float)dv; memcpy(&bits, &fv, 4); }
                                            else { memcpy(&bits, &dv, 8); }
                                            for (size_t b = 0; b < m_sz && m_off + b < clit_sz; b++) abuf[m_off + b] = (uint8_t)(bits >> (8 * b));
                                        } else {
                                            int64_t iv = zero_cc_eval_const_expr(c);
                                            for (size_t b = 0; b < m_sz && m_off + b < clit_sz; b++) abuf[m_off + b] = (uint8_t)((uint64_t)iv >> (8 * b));
                                        }
                                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
                                    }
                                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
                                    if (abuf) {
                                        zero_cc_buf_put_bytes(&c->data_section, abuf, clit_sz);
                                        free(abuf);
                                    }
                                    int anon_idx = add_global_var(anon_name, clit_type, anon_offset, clit_sz, true, true);
                                    if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                                        ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                                        dr->data_patch_offset = var_offset;
                                        dr->target_kind = 1;
                                        dr->target_data_offset = anon_offset;
                                        dr->target_global_idx = anon_idx;
                                    }
                                } else {
                                    /* Array compound literal */
                                    if (clit_type.tag != TYPE_ARRAY) {
                                        clit_type = zero_cc_type_array(&clit_type, 0);
                                    }
                                    size_t clit_elem_sz = (clit_type.base && clit_type.base->size > 0) ? clit_type.base->size : 4;
                                    size_t clit_elem_al = (clit_type.base && clit_type.base->align > 0) ? clit_type.base->align : 4;
                                    if (clit_type.array_len == 0) {
                                        size_t cur = c->token_pos + 1;
                                        int depth = 1;
                                        size_t count = 0;
                                        while (cur < c->token_count && depth > 0) {
                                            if (c->tokens[cur].type == TK_LBRACE) depth++;
                                            else if (c->tokens[cur].type == TK_RBRACE) depth--;
                                            else if (c->tokens[cur].type == TK_COMMA && depth == 1) count++;
                                            else if (depth == 1 && count == 0 && c->tokens[cur].type != TK_COMMA) count = 1;
                                            cur++;
                                        }
                                        if (count == 0) count = 1;
                                        clit_type.array_len = count;
                                    }
                                    size_t clit_sz = clit_type.array_len * clit_elem_sz;
                                    clit_type.size = clit_sz;
                                    clit_type.align = clit_elem_al;
                                    size_t clit_pad = (clit_elem_al - (c->data_section.length % clit_elem_al)) % clit_elem_al;
                                    for (size_t p = 0; p < clit_pad; p++) zero_cc_emit_u8(&c->data_section, 0);
                                    size_t anon_offset = c->data_section.length;
                                    c->token_pos++; /* skip '{' */
                                    uint8_t *abuf = calloc(clit_sz > 0 ? clit_sz : 1, 1);
                                    size_t a_off = 0;
                                    bool clit_is_flt = clit_type.base && (clit_type.base->tag == TYPE_FLOAT || clit_type.base->tag == TYPE_DOUBLE);
                                    bool clit_is_ptr = clit_type.base && (clit_type.base->tag == TYPE_PTR);
                                    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACE) {
                                        if (c->tokens[c->token_pos].type == TK_STRING && clit_is_ptr) {
                                            const char *sv = c->tokens[c->token_pos].allocated_str ?
                                                c->tokens[c->token_pos].allocated_str : c->tokens[c->token_pos].str_val;
                                            size_t ro_off = c->rodata_section.length;
                                            zero_cc_buf_put_bytes(&c->rodata_section, sv, strlen(sv) + 1);
                                            c->token_pos++;
                                            if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                                                ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                                                dr->data_patch_offset = anon_offset + a_off;
                                                dr->target_kind = 0;
                                                dr->rodata_offset = ro_off;
                                                dr->target_data_offset = 0;
                                                dr->target_global_idx = -1;
                                            }
                                        } else if (clit_is_flt) {
                                            double dv = zero_cc_eval_const_expr_float(c);
                                            uint64_t bits = 0;
                                            if (clit_elem_sz <= 4) { float fv = (float)dv; memcpy(&bits, &fv, 4); }
                                            else { memcpy(&bits, &dv, 8); }
                                            for (size_t b = 0; b < clit_elem_sz && a_off + b < clit_sz; b++) abuf[a_off + b] = (uint8_t)(bits >> (8 * b));
                                        } else {
                                            int64_t iv = zero_cc_eval_const_expr(c);
                                            for (size_t b = 0; b < clit_elem_sz && a_off + b < clit_sz; b++) abuf[a_off + b] = (uint8_t)((uint64_t)iv >> (8 * b));
                                        }
                                        a_off += clit_elem_sz;
                                        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
                                    }
                                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
                                    if (abuf) {
                                        zero_cc_buf_put_bytes(&c->data_section, abuf, clit_sz);
                                        free(abuf);
                                    }
                                    int anon_idx = add_global_var(anon_name, clit_type, anon_offset, clit_sz, true, true);
                                    if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                                        ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                                        dr->data_patch_offset = var_offset;
                                        dr->target_kind = 1;
                                        dr->target_data_offset = anon_offset;
                                        dr->target_global_idx = anon_idx;
                                    }
                                }
                            } else if (it == TK_STRING && !is_char_array) {
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
                                 * positional, designated, nested sub-aggregates,
                                 * [i..j] ranges, elided braces): recursive walker
                                 * (Sprint 44) with the shared designator engine. */
                                uint8_t *vbuf = calloc(size > 0 ? size : 1, 1);
                                if (vbuf) {
                                    ZeroCCType init_type = gtype;
                                    if (has_array_suffix) {
                                        ZeroCCType lv[8];
                                        lv[0] = gtype;
                                        size_t nd = (dim_count < 1) ? 1 : (size_t)dim_count;
                                        if (nd > 7) nd = 7;
                                        for (size_t k = 1; k <= nd; k++) {
                                            lv[k].tag = TYPE_ARRAY;
                                            lv[k].base = &lv[k - 1];
                                            lv[k].array_len = (size_t)dims[nd - k];
                                            lv[k].size = lv[k - 1].size * (size_t)dims[nd - k];
                                            lv[k].align = lv[k - 1].align;
                                        }
                                        init_type = lv[nd];
                                        if (nd == 1) {
                                            init_type.array_len = (size_t)array_len;
                                            init_type.size = size;
                                            init_type.align = elem_align;
                                        }
                                    }
                                    size_t top_cursor = 0;
                                    zero_cc_emit_brace_init_global(c, init_type, vbuf, 0,
                                                                   size, var_offset,
                                                                   &top_cursor);
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
                                /* C11 6.7.9p13: a fixed-size char array smaller than the
                                 * string is truncated (excess chars + the NUL) - warn
                                 * like GCC's "initializer-string ... is too long". */
                                if (!array_unsized && sl + 1 > size) {
                                    diag_report_warning_at_token(c,
                                        "initializer-string for array of %s is too long",
                                        gtype.tag == TYPE_UINT8 ? "unsigned char" : "char");
                                }
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
                                    ZeroCCFunction *ref_fn = NULL;
                                    if (ref_idx < 0) {
                                        /* Cross-TU: the referenced global is only
                                         * declared extern in this TU (registered in
                                         * the function table with is_external=true).
                                         * Emit a by-name (kind-2) data reloc: AOT
                                         * turns it into a .rela..data against the
                                         * UND symbol; the JIT resolves it against
                                         * the merged registry at patch time. */
                                        for (size_t fi = 0; fi < c->function_count; fi++) {
                                            if (c->functions[fi].is_external &&
                                                strcmp(c->functions[fi].name, ref_name) == 0) {
                                                ref_fn = &c->functions[fi];
                                                break;
                                            }
                                        }
                                    }
                                    zero_cc_emit_u64(&c->data_section, 0);
                                    if (c->data_reloc_count < ZERO_CC_MAX_DATA_RELOCS) {
                                        ZeroCCDataReloc *dr = &c->data_relocs[c->data_reloc_count++];
                                        dr->data_patch_offset = var_offset;
                                        dr->rodata_offset = 0;
                                        if (ref_idx >= 0) {
                                            dr->target_kind = 1; /* .data global target */
                                            dr->target_data_offset = ref_gv->data_offset;
                                            dr->target_global_idx = ref_idx;
                                        } else if (ref_fn) {
                                            dr->target_kind = 2; /* extern object by name */
                                            dr->target_data_offset = 0;
                                            dr->target_global_idx = -1;
                                            snprintf(dr->target_extern_name,
                                                     sizeof(dr->target_extern_name),
                                                     "%s", ref_name);
                                        } else {
                                            /* Unresolvable reference: plain zero, no
                                             * reloc (pre-existing behavior). */
                                            c->data_reloc_count--;
                                        }
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
                                uint64_t bits = 0;
                                if (elem_size <= 4) {
                                    float fv = (float)dv;
                                    memcpy(&bits, &fv, sizeof(fv));
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
                            for (int d = dim_count - 1; d >= 0; d--) {
                                int64_t dv = dims[d];
                                if (array_unsized && d == 0) dv = array_len; /* size inferred */
                                reg_type = zero_cc_type_array(&reg_type, (ssize_t)(dv > 0 ? dv : 1));
                            }
                            if (add_global_var(var_name, reg_type, var_offset, size, is_static, has_initializer) < 0) {
                                diag_report_error_at_token(c, "out of memory registering global variable");
                            }
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
                            for (int d = dim_count - 1; d >= 0; d--) {
                                int64_t dv = dims[d];
                                if (array_unsized && d == 0) dv = array_len; /* size inferred */
                                reg_type = zero_cc_type_array(&reg_type, (ssize_t)(dv > 0 ? dv : 1));
                            }
                            if (add_global_var(var_name, reg_type, var_offset, size, is_static, has_initializer) < 0) {
                                diag_report_error_at_token(c, "out of memory registering global variable");
                            }
                        }
                        break; /* done with this declaration statement */
                    } while (1);
                    continue; /* Skip function parser for variables */
                }
            } else {
                /* Syntax error: report error and skip until semicolon */
                diag_report_error_at_token(c, "expected identifier in declaration");
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
                // Syntax error: report error and skip until semicolon
                diag_report_error_at_token(c, "expected identifier in _Thread_local declaration");
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
        if (c->token_pos == prev_pos) {
            diag_report_error_at_token(c, "unexpected token at file scope");
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
    /* Struct member arrays must go while the pool is still alive (the
     * pool nodes hold the nested anonymous-struct arrays). */
    zero_cc_free_all_struct_arrays(c);
    zero_cc_type_pool_free();
}


#endif /* ZERO_CC_JIT_COMPILE_UNIT_H */
