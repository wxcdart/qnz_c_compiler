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

/* Owner-set helpers for zero_cc_free_all_struct_arrays: collect distinct
 * member-array pointers; an array referenced by several types (the
 * canonical struct def + typedef aliases + locals/params of that type)
 * is freed exactly once. */
#define ZC_STRUCT_OWNER_SET_CAP 1024 /* > any sane distinct-struct count */

static void zc_struct_owner_add(ZeroCCStructMember **set, size_t *n,
                                ZeroCCStructMember *m) {
    if (!m) return;
    for (size_t i = 0; i < *n; i++)
        if (set[i] == m) return;
    if (*n < ZC_STRUCT_OWNER_SET_CAP) set[(*n)++] = m;
    /* Set full: skip — leaking one array beats risking a double free. */
}

void zero_cc_free_all_struct_arrays(ZeroCCCompiler *c) {
    ZeroCCStructMember *owners[ZC_STRUCT_OWNER_SET_CAP];
    size_t n = 0;
    if (c) {
        for (size_t i = 0; i < c->struct_count; i++)
            zc_struct_owner_add(owners, &n, c->structs[i].type.members);
        for (size_t i = 0; i < c->typedef_count; i++)
            zc_struct_owner_add(owners, &n, c->typedefs[i].type.members);
        for (size_t i = 0; i < c->local_count; i++)
            zc_struct_owner_add(owners, &n, c->locals[i].type.members);
        for (size_t i = 0; i < c->function_count; i++) {
            zc_struct_owner_add(owners, &n, c->functions[i].return_type.members);
            for (size_t j = 0; j < c->functions[i].param_count && j < 8; j++)
                zc_struct_owner_add(owners, &n,
                                    c->functions[i].param_types[j].members);
        }
        for (size_t i = 0; i < c->thread_local_count && i < 64; i++)
            zc_struct_owner_add(owners, &n, c->thread_locals[i].type.members);
    }
    /* The pool nodes are the live holders of nested anonymous-struct
     * member arrays — iterate the list directly (chasing `base` pointers
     * from the tables would dangle once the pool is freed). */
    for (ZeroCCTypeNode *node = g_type_pool_head; node; node = node->next)
        zc_struct_owner_add(owners, &n, node->type.members);
    for (size_t i = 0; i < n; i++) free(owners[i]);
    /* The structs/typedefs tables persist past the compile: null the
     * now-dangling owners so a stale entry can't be freed twice. */
    if (c) {
        for (size_t i = 0; i < c->struct_count; i++) {
            c->structs[i].type.members = NULL;
            c->structs[i].type.members_cap = 0;
        }
        for (size_t i = 0; i < c->typedef_count; i++) {
            c->typedefs[i].type.members = NULL;
            c->typedefs[i].type.members_cap = 0;
        }
    }
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
