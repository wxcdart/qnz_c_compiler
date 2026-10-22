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

/* Free one registered struct/union's member array if `m` is unique among
 * the registration sites that could own it (named struct defs and earlier
 * typedefs).  Copies that merely alias the owner (e.g. a local of struct
 * type, or `typedef struct P {...} P2;`) are skipped. */
static bool free_unique_members(ZeroCCStructMember *m,
                                const ZeroCCCompiler *c,
                                size_t typedef_before) {
    if (!m) return true;
    for (size_t i = 0; i < c->struct_count; i++) {
        if (c->structs[i].type.members == m) return false; /* struct owns it */
    }
    for (size_t i = 0; i < typedef_before; i++) {
        if (c->typedefs[i].type.members == m) return false; /* earlier typedef owns it */
    }
    free(m);
    return true;
}

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

    /* Struct/union member arrays (heap, realloc'd by
     * zero_cc_type_reserve_members).  Owner: the named struct def, else the
     * first typedef that carries the array.  Copies alias the owner and
     * must not be freed twice. */
    for (size_t i = 0; i < c->typedef_count; i++) {
        if (!c->typedefs[i].type.members) continue;
        if (free_unique_members(c->typedefs[i].type.members, c, i)) {
            c->typedefs[i].type.members = NULL;
            c->typedefs[i].type.members_cap = 0;
        }
    }
    for (size_t i = 0; i < c->struct_count; i++) {
        free(c->structs[i].type.members);
        c->structs[i].type.members = NULL;
        c->structs[i].type.members_cap = 0;
    }

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

    /* Register allocator + CFG cache (last-built snapshot) */
    regalloc_destroy(&c->regalloc);
    zero_cc_cfg_cache_free(&c->cfg_cache);

    /* Machine-code sections */
    zero_cc_buf_free(&c->text_section);
    zero_cc_buf_free(&c->rodata_section);
    zero_cc_buf_free(&c->data_section);

    /* diag_buf.entries pointed at the per-compile tmp arena, destroyed
     * inside zero_cc_compile_c99 — clear, never free. */
    c->diag_buf.entries = NULL;
    c->diag_buf.count = 0;
    c->diag_buf.capacity = 0;
    c->sem_ctx = NULL; /* stack-lifetime, destroyed per compile */
    c->diag_pragma_state.disabled_warning_count = 0; /* strings are global */

    free(c);
}
