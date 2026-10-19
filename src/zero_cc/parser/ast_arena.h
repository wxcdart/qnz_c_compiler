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

static inline size_t zero_cc_ast_node_count_impl(ZeroCCASTArena *arena, ASTRef ref, int depth) {
    if (!arena || ref == AST_NULL || depth > 64) return 0;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return 0;

    size_t cnt = 1;
    if (n->left != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->left, depth + 1);
    if (n->right != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->right, depth + 1);
    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        if (n->aux.alt != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->aux.alt, depth + 1);
    } else if (n->tag == NODE_FOR) {
        if (n->aux.step != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->aux.step, depth + 1);
        if (n->aux.alt != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->aux.alt, depth + 1);
    }
    if (n->next != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->next, depth + 1);
    return cnt;
}

size_t zero_cc_ast_node_count(ZeroCCASTArena *arena, ASTRef root) {
    return zero_cc_ast_node_count_impl(arena, root, 0);
}

static inline ASTRef zero_cc_ast_clone_impl(ZeroCCASTArena *src_arena, ASTRef src_ref, ZeroCCASTArena *dst_arena, int depth) {
    if (!src_arena || src_ref == AST_NULL || !dst_arena || depth > 64) return AST_NULL;
    ZeroCCASTNode *src_n = zero_cc_ast_get(src_arena, src_ref);
    if (!src_n) return AST_NULL;

    ASTRef dst_ref = zero_cc_ast_alloc_node(dst_arena, (ZeroCCASTTag)src_n->tag);
    if (dst_ref == AST_NULL) return AST_NULL;
    ZeroCCASTNode *dst_n = zero_cc_ast_get(dst_arena, dst_ref);
    if (!dst_n) return AST_NULL;

    /* Copy entire node structure */
    *dst_n = *src_n;

    /* Recursively clone children */
    if (src_n->left != AST_NULL) {
        dst_n->left = zero_cc_ast_clone_impl(src_arena, src_n->left, dst_arena, depth + 1);
    }
    if (src_n->right != AST_NULL) {
        dst_n->right = zero_cc_ast_clone_impl(src_arena, src_n->right, dst_arena, depth + 1);
    }
    if (src_n->tag == NODE_IF || src_n->tag == NODE_TERNARY) {
        if (src_n->aux.alt != AST_NULL) {
            dst_n->aux.alt = zero_cc_ast_clone_impl(src_arena, src_n->aux.alt, dst_arena, depth + 1);
        }
    } else if (src_n->tag == NODE_FOR) {
        if (src_n->aux.step != AST_NULL) {
            dst_n->aux.step = zero_cc_ast_clone_impl(src_arena, src_n->aux.step, dst_arena, depth + 1);
        }
        if (src_n->aux.alt != AST_NULL) {
            dst_n->aux.alt = zero_cc_ast_clone_impl(src_arena, src_n->aux.alt, dst_arena, depth + 1);
        }
    }
    if (src_n->next != AST_NULL) {
        dst_n->next = zero_cc_ast_clone_impl(src_arena, src_n->next, dst_arena, depth + 1);
    }

    return dst_ref;
}

ASTRef zero_cc_ast_clone(ZeroCCASTArena *src_arena, ASTRef root, ZeroCCASTArena *dst_arena) {
    return zero_cc_ast_clone_impl(src_arena, root, dst_arena, 0);
}

static inline bool zero_cc_ast_has_call_impl(ZeroCCASTArena *arena, ASTRef ref, const char *target_name, int depth) {
    if (!arena || ref == AST_NULL || depth > 64) return false;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;

    if (n->tag == NODE_CALL) {
        if (!target_name) return true;
        ZeroCCASTNode *callee = zero_cc_ast_get(arena, n->left);
        if (callee && callee->tag == NODE_IDENT && callee->s.str) {
            if (strcmp(callee->s.str, target_name) == 0) return true;
        }
    }

    if (n->left != AST_NULL && zero_cc_ast_has_call_impl(arena, n->left, target_name, depth + 1)) return true;
    if (n->right != AST_NULL && zero_cc_ast_has_call_impl(arena, n->right, target_name, depth + 1)) return true;
    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        if (n->aux.alt != AST_NULL && zero_cc_ast_has_call_impl(arena, n->aux.alt, target_name, depth + 1)) return true;
    } else if (n->tag == NODE_FOR) {
        if (n->aux.step != AST_NULL && zero_cc_ast_has_call_impl(arena, n->aux.step, target_name, depth + 1)) return true;
        if (n->aux.alt != AST_NULL && zero_cc_ast_has_call_impl(arena, n->aux.alt, target_name, depth + 1)) return true;
    }
    if (n->next != AST_NULL && zero_cc_ast_has_call_impl(arena, n->next, target_name, depth + 1)) return true;
    return false;
}

bool zero_cc_ast_has_call(ZeroCCASTArena *arena, ASTRef root, const char *target_name) {
    return zero_cc_ast_has_call_impl(arena, root, target_name, 0);
}

#endif /* ZERO_CC_PARSER_AST_ARENA_H */
