#ifndef ZERO_CC_PARSER_AST_ARENA_H
#define ZERO_CC_PARSER_AST_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../types_lexer.h"
#include "../parser.h"

void zero_cc_ast_arena_init(ZeroCCASTArena *arena, size_t initial_cap) {
    if (!arena) return;
    arena->capacity = initial_cap ? initial_cap : 1024;
    arena->nodes = (ZeroCCASTNode *)malloc(arena->capacity * sizeof(ZeroCCASTNode));
    /* Reserve index 0 for AST_NULL */
    if (arena->nodes) {
        memset(&arena->nodes[0], 0, sizeof(ZeroCCASTNode));
        arena->count = 1;
    } else {
        arena->count = 0;
        arena->capacity = 0;
    }
}

void zero_cc_ast_arena_free(ZeroCCASTArena *arena) {
    if (!arena) return;
    if (arena->nodes) {
        free(arena->nodes);
        arena->nodes = NULL;
    }
    arena->count = 0;
    arena->capacity = 0;
}

void zero_cc_ast_arena_reset(ZeroCCASTArena *arena) {
    if (!arena) return;
    if (arena->nodes) {
        memset(&arena->nodes[0], 0, sizeof(ZeroCCASTNode));
        arena->count = 1; /* Keep index 0 as AST_NULL */
    } else {
        arena->count = 0;
    }
}

ASTRef zero_cc_ast_alloc_node(ZeroCCASTArena *arena, ZeroCCASTTag tag) {
    if (!arena) return AST_NULL;
    if (arena->count >= arena->capacity) {
        size_t new_cap = arena->capacity ? arena->capacity * 2 : 1024;
        ZeroCCASTNode *new_nodes = (ZeroCCASTNode *)realloc(arena->nodes, new_cap * sizeof(ZeroCCASTNode));
        if (!new_nodes) return AST_NULL;
        arena->nodes = new_nodes;
        arena->capacity = new_cap;
    }

    ASTRef ref = (ASTRef)arena->count++;
    ZeroCCASTNode *node = &arena->nodes[ref];
    memset(node, 0, sizeof(*node));
    node->tag = (uint16_t)tag;
    return ref;
}


#endif /* ZERO_CC_PARSER_AST_ARENA_H */
