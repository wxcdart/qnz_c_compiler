#ifndef ZERO_CC_PARSER_AST_ARENA_H
#define ZERO_CC_PARSER_AST_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../types/types.h"
#include "parser.h"

void zero_cc_ast_arena_init(ZeroCCASTArena *arena, size_t initial_cap) {
    if (!arena) return;
    (void)initial_cap;
    arena->chunk_capacity = 8;
    arena->chunks = (ZeroCCASTNode **)calloc(arena->chunk_capacity, sizeof(ZeroCCASTNode *));
    if (!arena->chunks) {
        arena->chunk_count = 0;
        arena->count = 0;
        return;
    }
    arena->chunks[0] = (ZeroCCASTNode *)calloc(ZERO_CC_AST_CHUNK_SIZE, sizeof(ZeroCCASTNode));
    if (!arena->chunks[0]) {
        free(arena->chunks);
        arena->chunks = NULL;
        arena->chunk_capacity = 0;
        arena->chunk_count = 0;
        arena->count = 0;
        return;
    }
    arena->chunk_count = 1;
    arena->count = 1; /* Reserve index 0 for AST_NULL */
}

void zero_cc_ast_arena_free(ZeroCCASTArena *arena) {
    if (!arena) return;
    if (arena->chunks) {
        for (size_t i = 0; i < arena->chunk_count; i++) {
            if (arena->chunks[i]) free(arena->chunks[i]);
        }
        free(arena->chunks);
        arena->chunks = NULL;
    }
    arena->chunk_count = 0;
    arena->chunk_capacity = 0;
    arena->count = 0;
}

void zero_cc_ast_arena_reset(ZeroCCASTArena *arena) {
    if (!arena) return;
    if (arena->chunk_count > 0 && arena->chunks && arena->chunks[0]) {
        memset(&arena->chunks[0][0], 0, sizeof(ZeroCCASTNode));
        arena->count = 1; /* Keep index 0 as AST_NULL, reuse allocated chunks */
    } else {
        arena->count = 0;
    }
}

ASTRef zero_cc_ast_alloc_node(ZeroCCASTArena *arena, ZeroCCASTTag tag) {
    if (!arena || !arena->chunks) return AST_NULL;

    size_t chunk_idx = arena->count >> ZERO_CC_AST_CHUNK_BITS;
    size_t node_idx  = arena->count & ZERO_CC_AST_CHUNK_MASK;

    if (chunk_idx >= arena->chunk_count) {
        if (chunk_idx >= arena->chunk_capacity) {
            size_t new_cap = arena->chunk_capacity ? arena->chunk_capacity * 2 : 8;
            ZeroCCASTNode **new_chunks = (ZeroCCASTNode **)realloc(arena->chunks, new_cap * sizeof(ZeroCCASTNode *));
            if (!new_chunks) return AST_NULL;
            arena->chunks = new_chunks;
            arena->chunk_capacity = new_cap;
        }
        ZeroCCASTNode *new_chunk = (ZeroCCASTNode *)calloc(ZERO_CC_AST_CHUNK_SIZE, sizeof(ZeroCCASTNode));
        if (!new_chunk) return AST_NULL;
        arena->chunks[chunk_idx] = new_chunk;
        arena->chunk_count++;
    }

    ASTRef ref = (ASTRef)arena->count++;
    ZeroCCASTNode *node = &arena->chunks[chunk_idx][node_idx];
    memset(node, 0, sizeof(*node));
    node->tag = (uint16_t)tag;
    return ref;
}

#endif /* ZERO_CC_PARSER_AST_ARENA_H */
