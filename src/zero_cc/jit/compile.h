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

/* Parse a function's parameter block (including K&R style, attributes and
 * VLA dimension expressions) into the out-params. Returns false on a
 * diagnostic. */
bool zero_cc_parse_fn_param_block(ZeroCCCompiler *c, SemContext *ctx,
    const char *fn_name,
    ZeroCCType **param_types, const char ***param_names, size_t *param_cap,
    size_t *param_count, bool *is_variadic_fn, bool *has_prototype,
    ZeroCCASTArena **dim_arena_out, ASTRef dim_exprs[]);

/* Compile one function (params + body lowering + emission + nested fns). */
void zero_cc_compile_function(ZeroCCCompiler *c, SemContext *ctx, Arena *perm);

#endif /* ZERO_CC_JIT_COMPILE_H */
