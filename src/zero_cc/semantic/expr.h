#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "../utils/arena.h"
#include "../parser.h"
#include "diagnostics.h"
#include "scope.h"
#include "type.h"
#include "promote.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "../types/token.h"

/* Token values matching ZeroCCTokType */
#define EXPR_TK_ASSIGN       TK_ASSIGN
#define EXPR_TK_PLUS         TK_PLUS
#define EXPR_TK_MINUS        TK_MINUS
#define EXPR_TK_STAR         TK_STAR
#define EXPR_TK_SLASH        TK_SLASH
#define EXPR_TK_PERCENT      TK_PERCENT
#define EXPR_TK_AMP          TK_AMP
#define EXPR_TK_PIPE         TK_PIPE
#define EXPR_TK_CARET        TK_CARET
#define EXPR_TK_TILDE        TK_TILDE
#define EXPR_TK_BANG         TK_BANG
#define EXPR_TK_EQ           TK_EQ
#define EXPR_TK_NE           TK_NE
#define EXPR_TK_LT           TK_LT
#define EXPR_TK_LE           TK_LE
#define EXPR_TK_GT           TK_GT
#define EXPR_TK_GE           TK_GE
#define EXPR_TK_LOGAND       TK_LOGAND
#define EXPR_TK_LOGOR        TK_LOGOR
#define EXPR_TK_SHL          TK_SHL
#define EXPR_TK_SHR          TK_SHR
#define EXPR_TK_INC          TK_INC
#define EXPR_TK_DEC          TK_DEC
#define EXPR_TK_ADD_ASSIGN   TK_ADD_ASSIGN
#define EXPR_TK_SUB_ASSIGN   TK_SUB_ASSIGN
#define EXPR_TK_MUL_ASSIGN   TK_MUL_ASSIGN
#define EXPR_TK_DIV_ASSIGN   TK_DIV_ASSIGN
#define EXPR_TK_MOD_ASSIGN   TK_MOD_ASSIGN
#define EXPR_TK_AND_ASSIGN   TK_AND_ASSIGN
#define EXPR_TK_OR_ASSIGN    TK_OR_ASSIGN
#define EXPR_TK_XOR_ASSIGN   TK_XOR_ASSIGN
#define EXPR_TK_SHL_ASSIGN   TK_SHL_ASSIGN
#define EXPR_TK_SHR_ASSIGN   TK_SHR_ASSIGN

/* Compatibility Aliases for AST Node Tags */
#ifndef AST_INT_LITERAL
#define AST_INT_LITERAL    NODE_INT
#endif
#ifndef AST_FLOAT_LITERAL
#define AST_FLOAT_LITERAL  NODE_FLOAT
#endif
#ifndef AST_STRING_LITERAL
#define AST_STRING_LITERAL NODE_STRING
#endif
#ifndef AST_IDENTIFIER
#define AST_IDENTIFIER     NODE_IDENT
#endif
#ifndef AST_UNARY
#define AST_UNARY          NODE_UNOP
#endif
#ifndef AST_BINARY
#define AST_BINARY         NODE_BINOP
#endif
#ifndef AST_CALL
#define AST_CALL           NODE_CALL
#endif
#ifndef AST_CAST
#define AST_CAST           NODE_CAST
#endif
#ifndef AST_MEMBER_ACCESS
#define AST_MEMBER_ACCESS  NODE_MEMBER
#endif

/**
 * Forward declaration of SemContext.
 * Defined in visitor.h if present, or self-contained.
 */
#ifndef SEM_CONTEXT_DEFINED
#define SEM_CONTEXT_DEFINED
typedef struct SemContext {
    Arena          *perm_arena;           /* Long-lived arena for types, symbols, permanent AST metadata */
    Arena          *tmp_arena;            /* Scratch arena for formatting diagnostics, temporary structures */
    DiagBuf        *diags;                /* Diagnostic collector for errors and warnings */
    Scope          *current_scope;        /* Active symbol scope */
    TypeInfo       *current_fn_ret_type;  /* Return type of the enclosing function, or NULL if outside */
    bool            in_loop;              /* True if inside a loop body */
    bool            in_switch;            /* True if inside a switch statement */
    ZeroCCASTArena *ast_arena;            /* AST Arena owning the ZeroCCASTNode pool (optional, may be NULL) */
} SemContext;
#endif

/**
 * Retrieve the TypeInfo pointer associated with an AST node.
 * Uses node->aux.extra as an arena byte offset into ctx->perm_arena.
 * Returns NULL if no type has been set or if node is NULL.
 */
TypeInfo *expr_node_type(SemContext *ctx, const ZeroCCASTNode *node);

/**
 * Associate a TypeInfo pointer with an AST node by storing its arena byte offset
 * in node->aux.extra.
 */
void expr_node_set_type(SemContext *ctx, ZeroCCASTNode *node, TypeInfo *type);

/**
 * Expression handler dispatch: evaluates an expression node, determines its type,
 * verifies semantic correctness (types, operands, symbol resolution), records diagnostics
 * on failure, annotates node->aux.extra, and returns the computed TypeInfo*.
 */
TypeInfo *expr_handle_expression(SemContext *ctx, ZeroCCASTNode *node);

/* Specific Expression Handlers */
TypeInfo *expr_handle_literal(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_identifier(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_unary(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_binary(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_assignment(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_call(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_cast(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_va_start(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_va_arg(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_member(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_index(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_ternary(SemContext *ctx, ZeroCCASTNode *node);

#ifdef __cplusplus
}
#endif
