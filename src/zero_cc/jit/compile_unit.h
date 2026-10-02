#ifndef ZERO_CC_JIT_COMPILE_UNIT_H
#define ZERO_CC_JIT_COMPILE_UNIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "zero_cc/core.h"
#include "zero_cc/types_lexer.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/visitor.h"
#include "zero_cc/types/global_vars.h"
#include "../isa_backend.h"
#include "zero_cc/parser/ir_emitter.h"
#include "zero_cc/statements/static_assert.h"
#include "zero_cc/statements/definitions.h"

/* Forward declaration */
static inline bool zero_cc_compile_top_level_decl(ZeroCCCompiler *c);
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
    
    /* Standalone Enum Declaration */
    if (t == TK_ENUM) {
        zero_cc_stmt_enum(c);
        return true;
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
        c->had_error = true;
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

    /* Initialize ISA backend */
    c->isa = zero_cc_get_native_backend()->isa;
    c->isa_backend = zero_cc_get_native_backend();

    /* Create shared semantic context for all functions */
    DiagBuf shared_diags;
    diag_init(&shared_diags, tmp);
    SemContext shared_ctx;
    if (!sem_context_init(&shared_ctx, perm, tmp, &shared_diags, NULL)) {
        c->had_error = true;
        zero_cc_type_pool_free();
        return;
    }
    c->sem_ctx = &shared_ctx;  /* Save for lowering */
    // Save the global scope pointer (root of scope chain)
    // Register standard library functions if any

    while (c->tokens[c->token_pos].type != TK_EOF && !c->had_error) {
        if (c->tokens[c->token_pos].type == TK_TYPEDEF ||
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
            /* Parse type and identifier */
            ZeroCCType ext_type = zero_cc_parse_type_name(c);
            (void)ext_type; /* unused for now */
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                char ext_name[128];
                snprintf(ext_name, sizeof(ext_name), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                /* Consume trailing semicolon */
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                /* Register as external symbol (no code generation, just declaration) */
                if (c->function_count < ZERO_CC_MAX_FUNCTIONS) {
                    ZeroCCFunction *fn = &c->functions[c->function_count++];
                    fn->text_offset = 0; /* External, no code */
                    snprintf(fn->name, sizeof(fn->name), "%s", ext_name);
                    fn->is_external = true;
                }
            } else {
                /* Syntax error: skip until semicolon */
                while (c->tokens[c->token_pos].type != TK_EOF && c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
            }
            continue;
        }

        /* Global variable declarations */
        if (c->tokens[c->token_pos].type == TK_STATIC ||
            c->tokens[c->token_pos].type == TK_INT || c->tokens[c->token_pos].type == TK_CHAR ||
            c->tokens[c->token_pos].type == TK_VOID || c->tokens[c->token_pos].type == TK_FLOAT_KW ||
            c->tokens[c->token_pos].type == TK_DOUBLE_KW || c->tokens[c->token_pos].type == TK_BOOL_KW ||
            c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
            c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG ||
            c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION ||
            c->tokens[c->token_pos].type == TK_IDENT) {
            /* Save position before parsing - needed to back up for function definitions */
            size_t global_start = c->token_pos;
            /* Check for static keyword */
            bool is_static = false;
            if (c->tokens[c->token_pos].type == TK_STATIC) {
                is_static = true;
                c->token_pos++;
            }
            /* Parse type */
            ZeroCCType gtype = zero_cc_parse_type_name(c);
            /* Expect identifier */
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                /* Check if this is a function (followed by '(') or variable (followed by ';' or '=') */
                if (c->tokens[c->token_pos + 1].type == TK_LPAREN) {
                    /* This is a function definition, back up to start and let function parser handle it */
                    c->token_pos = global_start;
                } else {
                    /* This is a variable declaration */
                    char var_name[128];
                    snprintf(var_name, sizeof(var_name), "%s", c->tokens[c->token_pos].str_val);
                    c->token_pos++;
                    /* Optional initializer */
                    size_t init_offset = c->data_section.length;
                    bool has_initializer = false;
                    if (c->tokens[c->token_pos].type == TK_ASSIGN) {
                        c->token_pos++; /* skip = */
                        has_initializer = true;
                        /* Evaluate constant expression */
                        int64_t init_val = zero_cc_eval_const_expr(c);
                        /* Emit to data_section */
                        if (gtype.tag == TYPE_INT8 || gtype.tag == TYPE_UINT8 ||
                            gtype.tag == TYPE_INT16 || gtype.tag == TYPE_UINT16 ||
                            gtype.tag == TYPE_INT32 || gtype.tag == TYPE_UINT32 ||
                            gtype.tag == TYPE_INT64 || gtype.tag == TYPE_UINT64) {
                            zero_cc_emit_u64(&c->data_section, (uint64_t)init_val);
                        } else if (gtype.tag == TYPE_PTR) {
                            /* String literal or pointer - emit as 64-bit value */
                            zero_cc_emit_u64(&c->data_section, (uint64_t)init_val);
                        } else {
                            /* Default to 64-bit for now */
                            zero_cc_emit_u64(&c->data_section, (uint64_t)init_val);
                        }
                    }
                    /* Consume trailing semicolon */
                    if (c->tokens[c->token_pos].type == TK_SEMI) c->token_pos++;
                    /* Register global variable */
                    size_t size = gtype.size > 0 ? gtype.size : 8; /* Default to 8 bytes */
                    /* Align to 8 bytes */
                    size_t padding = (8 - (init_offset % 8)) % 8;
                    for (size_t i = 0; i < padding; i++) {
                        zero_cc_emit_u8(&c->data_section, 0);
                    }
                    add_global_var(var_name, gtype, init_offset + padding, size, is_static, has_initializer);
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
    sem_context_destroy(&shared_ctx);
    arena_destroy(tmp);
    arena_destroy(perm);
    zero_cc_type_pool_free();
    // End of compilation
}


#endif /* ZERO_CC_JIT_COMPILE_UNIT_H */
