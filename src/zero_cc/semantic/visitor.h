#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "zero_cc/common/arena.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/types/token.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/promote.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Semantic Analysis Context (SemContext)
 * Encapsulates the compilation state during AST traversal and type checking.
 */
#ifndef SEM_CONTEXT_DEFINED
#define SEM_CONTEXT_DEFINED
typedef struct SemContext {
    Arena        *perm_arena;           /* Long-lived arena for types, symbols, permanent AST metadata */
    Arena        *tmp_arena;            /* Scratch arena for formatting diagnostics, temporary structures */
    DiagBuf      *diags;                /* Diagnostic collector for errors and warnings */
    Scope        *current_scope;        /* Active symbol scope */
    TypeInfo     *current_fn_ret_type;  /* Return type of the enclosing function, or NULL if outside */
    bool          in_loop;              /* True if inside a loop body (for break / continue validation) */
    bool          in_switch;            /* True if inside a switch statement (for break / case validation) */
    ZeroCCASTArena *ast_arena;          /* AST Arena owning the ZeroCCASTNode pool (optional, may be NULL) */
    struct ZeroCCCompiler *compiler;    /* Back-pointer to compiler for TLS/globals access */
    /* User labels of the function currently being analyzed, collected
     * before traversal so forward gotos validate (C99 6.8.4.3). */
    size_t          label_count;
    size_t          label_cap;
    char          **labels;
} SemContext;
#endif

/**
 * Initialize a semantic analysis context.
 * Returns true on success, false on failure (e.g. invalid arguments or scope allocation failure).
 */
bool sem_context_init(SemContext *ctx, Arena *perm, Arena *tmp, DiagBuf *diags, ZeroCCASTArena *ast_arena, struct ZeroCCCompiler *compiler);

/**
 * Clear the per-function user-label table (call before analyzing each body).
 */
void sem_reset_labels(SemContext *ctx);

/**
 * Walk a function body and record every NODE_LABEL name so that goto
 * targets can be validated in one pass (labels may appear after their
 * goto in the source). */
void sem_collect_labels(SemContext *ctx, ASTRef root);

/**
 * True if a user label with the given name was collected for the current
 * function body (see sem_collect_labels).
 */
bool sem_find_label(SemContext *ctx, const char *name, size_t len);

/**
 * Destroy/clean up a semantic analysis context, leaving any open scopes.
 */
void sem_context_destroy(SemContext *ctx);

/**
 * Helper to record a type error at a specific AST node.
 */
void sem_node_error(SemContext *ctx, const ZeroCCASTNode *node, const char *fmt, ...);

/**
 * Retrieve the TypeInfo pointer associated with an AST node.
 * Uses node->aux.extra as an arena byte offset into ctx->perm_arena.
 * Returns NULL if no type has been set or if node is NULL.
 */
TypeInfo *node_type(SemContext *ctx, const ZeroCCASTNode *node);

/**
 * Associate a TypeInfo pointer with an AST node by storing its arena byte offset
 * in node->aux.extra.
 */
void node_set_type(SemContext *ctx, ZeroCCASTNode *node, TypeInfo *type);

/**
 * Convenience helper to get a ZeroCCASTNode* from an ASTRef if ast_arena is present.
 */
ZeroCCASTNode *sem_get_node(SemContext *ctx, ASTRef ref);

/**
 * Main AST traversal / dispatch function.
 * Recursively analyzes the given AST node, updates symbol scopes, checks types,
 * sets node->aux.extra to the computed TypeInfo index, and returns the computed TypeInfo*.
 */
TypeInfo *sem_analyze_node(SemContext *ctx, ZeroCCASTNode *node);

/**
 * Alternative walk function matching SEMANTIC.md naming.
 */
void sem_walk_node(SemContext *ctx, ZeroCCASTNode *node);

/**
 * Walk an ASTRef if ctx->ast_arena is provided.
 */
TypeInfo *sem_analyze_ref(SemContext *ctx, ASTRef ref);

#ifdef __cplusplus
}
#endif
