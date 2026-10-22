#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "zero_cc/common/arena.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/promote.h"
/* SemContext is canonically defined in visitor.h (single definition to
 * guarantee one ABI layout across all TUs — historically this header
 * carried a stale 8-field copy that won the first-include race in the
 * JIT pipeline headers, silently shrinking the struct there). */
#include "zero_cc/semantic/visitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility Aliases for AST Node Tags */
#ifndef AST_DECL
#define AST_DECL            NODE_DECL
#endif
#ifndef AST_TYPEDEF_DECL
#define AST_TYPEDEF_DECL    NODE_TYPEDEF_DECL
#endif
#ifndef AST_FUNCTION_DECL
#define AST_FUNCTION_DECL   NODE_FUNCTION_DECL
#endif

/* (SemContext comes from visitor.h — see include block above.) */

/**
 * Retrieve the TypeInfo pointer associated with an AST node.
 * Uses node->aux.extra as an arena byte offset into ctx->perm_arena.
 * Returns NULL if no type has been set or if node is NULL.
 */
TypeInfo *decl_node_type(SemContext *ctx, const ZeroCCASTNode *node);

/**
 * Associate a TypeInfo pointer with an AST node by storing its arena byte offset
 * in node->aux.extra.
 */
void decl_node_set_type(SemContext *ctx, ZeroCCASTNode *node, TypeInfo *type);

/**
 * Declaration handler dispatch: evaluates a declaration node, determines its type,
 * verifies semantic correctness (storage duration, duplicates in scope, initializer compatibility),
 * registers the symbol in ctx->current_scope, annotates node->aux.extra, and returns TypeInfo*.
 */
TypeInfo *decl_handle_declaration(SemContext *ctx, ZeroCCASTNode *node);

/**
 * Handler for variable declarations (NODE_DECL).
 * - Determines TypeInfo (explicitly set on node or inferred from initializer / default int).
 * - Checks for duplicate symbol in the same scope via scope_lookup_current.
 * - If initializer exists, evaluates it and validates assignment compatibility using
 *   type_compatible_assign(var_type, init_type). Emits diagnostic on mismatch.
 * - Inserts variable into ctx->current_scope with SYMBOL_VAR.
 */
TypeInfo *decl_handle_var(SemContext *ctx, ZeroCCASTNode *node);

/**
 * Handler for typedef declarations (NODE_TYPEDEF_DECL).
 * - Determines target TypeInfo for typedef.
 * - Checks for duplicate in same scope via scope_lookup_current.
 * - Inserts into ctx->current_scope with SYMBOL_TYPEDEF.
 */
TypeInfo *decl_handle_typedef(SemContext *ctx, ZeroCCASTNode *node);

/**
 * Handler for function declarations and definitions.
 * - Handles return type and parameter types.
 * - Checks compatibility with any previous declaration in scope (via type_is_compatible).
 * - Inserts or updates SYMBOL_FUNC in current scope.
 * - If definition has a function body:
 *   - saves old ctx->current_fn_ret_type and sets new return type,
 *   - pushes a new function parameter scope (scope_enter),
 *   - registers parameter symbols into the parameter scope,
 *   - processes function body statements / block,
 *   - pops parameter scope (scope_leave) and restores previous return type.
 */
TypeInfo *decl_handle_function_proto(SemContext *ctx, ZeroCCASTNode *node);

/**
 * Handler for Struct/Union/Enum declarations and tag registrations.
 * Computes layout, field offsets, alignments, and registers tag in symbol table as SYMBOL_TAG.
 */
TypeInfo *decl_handle_tag(SemContext *ctx, const char *tag_name, TypeKind kind, size_t size, size_t align);

#ifdef __cplusplus
}
#endif
