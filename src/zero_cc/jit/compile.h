/* Function-compilation entry points (Sprint 114 Tier B: implementation
 * moved to compile.c - this was a 1,800-line header whose static function
 * bodies were being compiled into both the main.c and compile_unit.c
 * translation units). Only compile_unit.c calls these; the declarations
 * live here so the public API surface (zero_cc.h -> jit_linker.h) keeps
 * working. */
#ifndef ZERO_CC_JIT_COMPILE_H
#define ZERO_CC_JIT_COMPILE_H

#include <stddef.h>
#include <stdbool.h>

#include "zero_cc/core/core.h"
#include "zero_cc/types/types.h"
#include "zero_cc/parser/parser.h"
/* Keeps the historical include ordering for TUs that include compile.h
 * before definitions.h (parse_expr_ast_internal must be declared before
 * definitions.h's typedef-declarator path calls it). */
#include "zero_cc/parser/zero_cc_declarator_stack.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/visitor.h"

/* Sprint 122 Stage 4: parse ONE function declarator (token at the name,
 * leading stars already consumed by the caller) through the declarator
 * stack, appending its params to the shared buffers.  The sole prototype
 * parser - used by the plain-name top-level function path, the
 * multi-declarator comma loop (2nd+ declarators), and the extern/mixed
 * function path.  ctx may be NULL (ZeroCCType-only, no semantic
 * stamping).  Returns false when the declarator is not a complete
 * function declarator. */
bool zero_cc_parse_fn_declarator(ZeroCCCompiler *c, SemContext *ctx,
    ZeroCCType **param_types, const char ***param_names, size_t *param_cap,
    size_t *param_count, bool *is_variadic_fn, bool *has_prototype,
    ZeroCCASTArena **dim_arena_out, ASTRef dim_exprs[], char *asm_out);

/* Compile one function (params + body lowering + emission + nested fns). */
void zero_cc_compile_function(ZeroCCCompiler *c, SemContext *ctx, Arena *perm);

/* File-scope declaration helpers (moved from compile_unit.c). */
bool zero_cc_struct_def_is_fn(ZeroCCCompiler *c);
bool zc_constexpr_not_data(ZeroCCCompiler *c);
void zero_cc_emit_scalar_bytes(ZeroCCBuffer *b, size_t width, uint64_t bits);
bool zero_cc_tk_def_via_aligned_marker(const ZeroCCCompiler *c, size_t idx);
bool zc_filescope_struct_def_at(ZeroCCCompiler *c, size_t pos);
bool zc_filescope_enum_def_at(ZeroCCCompiler *c, size_t pos);
bool zero_cc_compile_top_level_decl(ZeroCCCompiler *c);
bool zc_unparen_declarator(ZeroCCCompiler *c);
void zero_cc_decl_thread_local(ZeroCCCompiler *c, SemContext *shared_ctx, Arena *perm, bool is_static_in);

#endif /* ZERO_CC_JIT_COMPILE_H */
