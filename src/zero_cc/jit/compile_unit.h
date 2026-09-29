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

/* Master Translation Unit Compiler */
static inline void zero_cc_compile_c99(ZeroCCCompiler *c) {
    zero_cc_buf_init(&c->text_section, 4096);
    zero_cc_buf_init(&c->rodata_section, 1024);
    zero_cc_buf_init(&c->data_section, 1024);
    c->function_count = 0;
    // Create permanent and temporary arenas for semantic analysis
    Arena *perm = arena_create(1024 * 1024);
    Arena *tmp = arena_create(1024 * 1024);
    if (!perm || !tmp) {
        fprintf(stderr, "zero_cc: arena allocation failed\n");
        c->had_error = true;
        return;
    }
    c->reloc_count = 0;
    zero_cc_init_standard_types(c);
    c->thread_local_count = 0;
    /* Initialise default TLS backend */
    zero_cc_set_tls_backend(c, NULL);

    /* Create shared semantic context for all functions */
    DiagBuf shared_diags;
    diag_init(&shared_diags, tmp);
    SemContext shared_ctx;
    if (!sem_context_init(&shared_ctx, perm, tmp, &shared_diags, NULL)) {
        c->had_error = true;
        return;
    }
    // Save the global scope pointer (root of scope chain)
    Scope *global_scope = shared_ctx.current_scope;
    // Register standard library functions if any

    while (c->tokens[c->token_pos].type != TK_EOF && !c->had_error) {
        if (c->tokens[c->token_pos].type == TK_TYPEDEF) {
            zero_cc_parse_stmt(c);
            if (c->had_error) break;
            continue;
        }
        if (c->tokens[c->token_pos].type == TK_STATIC_ASSERT) {
            zero_cc_parse_stmt(c);
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
        /* Top level functions */
        zero_cc_compile_function(c, &shared_ctx, global_scope, perm, tmp);
        if (c->had_error) break;
        if (c->token_pos == prev_pos) {
            c->token_pos++;
        }
    }
    sem_context_destroy(&shared_ctx);
    arena_destroy(tmp);
    arena_destroy(perm);
    // End of compilation
    }


#endif /* ZERO_CC_JIT_COMPILE_UNIT_H */
