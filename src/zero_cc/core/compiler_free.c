/* ==========================================================================
 * zero_cc_compiler_free — full teardown of a ZeroCCCompiler instance.
 *
 * The compile API is one-directional (calloc -> preprocess -> tokenize ->
 * compile) with no matching destroy; this closes that gap so embedders and
 * the multi-compile test harnesses can release every per-instance
 * allocation (LeakSanitizer-clean under `make asan-semantic`).
 *
 * Lifetime contract:
 *  - Call AFTER you no longer need the compiled code (JIT execution and
 *    AOT section reads both use the object's buffers).
 *  - `asm_out` is caller-owned and is NOT closed here.
 *  - `isa_backend`/`tls_backend` are const pointers to static backends.
 *  - Pointers into the global type pool (every `ZeroCCType.base`, function
 *    parameter/return types) are already released by the
 *    zero_cc_type_pool_free() at the end of each zero_cc_compile_c99.
 * ========================================================================== */
#include "zero_cc/lexer/lexer.h"   /* first: establishes the scan.h statics before next.h's calls */
#include "zero_cc/types/symbols.h" /* pulls parser.h (ZeroCCASTArena + arena prototypes) via expr.h */
#include "zero_cc/lexer/tokenize.h"

/* NOTE: struct/union member arrays are freed per-compilation by
 * zero_cc_free_all_struct_arrays (compile_unit.h teardown) — they must go
 * while the type pool is still alive, so nothing here touches them. */

void zero_cc_compiler_free(ZeroCCCompiler *c) {
    if (!c) return;

    /* Tokens + per-token allocated strings */
    zero_cc_compiler_free_tokens(c);

    /* Local symbol table + hash tables (malloc'd by zero_cc_add_local) */
    free(c->locals);
    free(c->local_hash_buckets);
    free(c->local_hash_next);
    c->locals = NULL;
    c->local_hash_buckets = NULL;
    c->local_hash_next = NULL;
    c->local_count = 0;
    c->local_capacity = 0;
    c->local_hash_bucket_count = 0;

    /* (struct member arrays are freed per-compile — see note above) */

    /* IR arenas (one-time init, per-instance) */
    ir_arena_free(&c->ir_arena);
    ir_call_args_arena_free(&c->ir_call_args);

    /* Translation-unit AST arena (inlinable function bodies) */
    if (c->tu_ast_arena) {
        zero_cc_ast_arena_free((ZeroCCASTArena *)c->tu_ast_arena);
        free(c->tu_ast_arena);
        c->tu_ast_arena = NULL;
    }

    /* Interned string pool (chunked) */
    zero_cc_free_string_pool(c);

    /* Function parameter slices arena */
    if (c->fn_arena) {
        arena_destroy(c->fn_arena);
        c->fn_arena = NULL;
    }

    /* Register allocator + CFG cache (last-built snapshot) */
    regalloc_destroy(&c->regalloc);
    zero_cc_cfg_cache_free(&c->cfg_cache);

    /* Machine-code sections */
    zero_cc_buf_free(&c->text_section);
    zero_cc_buf_free(&c->rodata_section);
    zero_cc_buf_free(&c->data_section);

    /* Enum constants hash table */
    if (c->enum_constants) {
        free(c->enum_constants);
        c->enum_constants = NULL;
        c->enum_const_capacity = 0;
        c->enum_const_count = 0;
    }

    /* diag_buf.entries pointed at the per-compile tmp arena, destroyed
     * inside zero_cc_compile_c99 — clear, never free. */
    c->diag_buf.entries = NULL;
    c->diag_buf.count = 0;
    c->diag_buf.capacity = 0;
    c->sem_ctx = NULL; /* stack-lifetime, destroyed per compile */
    for (int i = 0; i < c->diag_pragma_state.disabled_warning_count && i < 64; i++) {
        free(c->diag_pragma_state.disabled_warnings[i]); /* ownership was
            * transferred from the global at tokenize time */
        c->diag_pragma_state.disabled_warnings[i] = NULL;
    }
    c->diag_pragma_state.disabled_warning_count = 0;

    free(c);
}
