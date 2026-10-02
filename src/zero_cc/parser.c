/* =========================================================================
 *  parser.c
 *  C99 AST Parser and Code Generator
 *  
 *  This file acts as a wrapper that includes the split parser modules.
 *  ========================================================================= */

#include "types_lexer.h"
#include "parser.h"
#include "parser/ast_arena.h"
#include "parser/ast_expr.h"
#include "parser/ast_stmt.h"
#include "parser/ast_dump.h"
#include "parser/ast_lowering.h"

static __thread ZeroCCTypeNode *g_type_pool_head = NULL;

void zero_cc_type_pool_free(void) {
    ZeroCCTypeNode *curr = g_type_pool_head;
    while (curr) {
        ZeroCCTypeNode *next = curr->next;
        free(curr);
        curr = next;
    }
    g_type_pool_head = NULL;
}

ZeroCCType *zero_cc_type_alloc_base(const ZeroCCType *base) {
    if (!base) return NULL;
    ZeroCCTypeNode *node = (ZeroCCTypeNode *)malloc(sizeof(ZeroCCTypeNode));
    if (!node) return NULL;
    node->type = *base;
    node->next = g_type_pool_head;
    g_type_pool_head = node;
    return &node->type;
}
