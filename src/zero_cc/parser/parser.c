/* =========================================================================
 *  parser.c
 *  C99 AST Parser and Code Generator
 *  
 *  This file acts as a wrapper that includes the split parser modules.
 *  ========================================================================= */

#include "zero_cc/types/types.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/parser/ast_arena.h"
#include "zero_cc/parser/ast_expr.h"
#include "zero_cc/parser/ast_stmt.h"
#include "zero_cc/parser/ast_dump.h"

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

/* Build-integrity canary: lets the driver detect stale object files compiled
 * against an older ZeroCCASTNode layout. A struct-size change without a full
 * rebuild silently corrupts AST thin pointers (designated-init string
 * pointers, int64 constants) and crashes far from the real cause. */
size_t zero_cc_ast_node_size(void) { return sizeof(ZeroCCASTNode); }
