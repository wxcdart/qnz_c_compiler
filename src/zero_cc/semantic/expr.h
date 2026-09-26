#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "../arena.h"
#include "../parser.h"
#include "diagnostics.h"
#include "scope.h"
#include "type.h"
#include "promote.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Token values matching ZeroCCTokType from types_lexer.h (without colliding with ZeroTypeTag) */
#define EXPR_TK_ASSIGN       38
#define EXPR_TK_PLUS         39
#define EXPR_TK_MINUS        40
#define EXPR_TK_STAR         41
#define EXPR_TK_SLASH        42
#define EXPR_TK_PERCENT      43
#define EXPR_TK_AMP          44
#define EXPR_TK_PIPE         45
#define EXPR_TK_CARET        46
#define EXPR_TK_TILDE        47
#define EXPR_TK_BANG         48
#define EXPR_TK_EQ           49
#define EXPR_TK_NE           50
#define EXPR_TK_LT           51
#define EXPR_TK_LE           52
#define EXPR_TK_GT           53
#define EXPR_TK_GE           54
#define EXPR_TK_LOGAND       55
#define EXPR_TK_LOGOR        56
#define EXPR_TK_SHL          57
#define EXPR_TK_SHR          58
#define EXPR_TK_INC          63
#define EXPR_TK_DEC          64
#define EXPR_TK_ADD_ASSIGN   65
#define EXPR_TK_SUB_ASSIGN   66
#define EXPR_TK_MUL_ASSIGN   67
#define EXPR_TK_DIV_ASSIGN   68
#define EXPR_TK_MOD_ASSIGN   69
#define EXPR_TK_AND_ASSIGN   70
#define EXPR_TK_OR_ASSIGN    71
#define EXPR_TK_XOR_ASSIGN   72
#define EXPR_TK_SHL_ASSIGN   73
#define EXPR_TK_SHR_ASSIGN   74

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
TypeInfo *expr_handle_member(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_index(SemContext *ctx, ZeroCCASTNode *node);
TypeInfo *expr_handle_ternary(SemContext *ctx, ZeroCCASTNode *node);

#ifdef __cplusplus
}
#endif
