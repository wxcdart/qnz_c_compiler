#include "arena.h"
#include <stdlib.h>

/* Helper: round up size to 8‑byte alignment */
static size_t align8(size_t size) {
    return (size + 7U) & ~((size_t)7U);
}

static ArenaBlock *block_new(size_t size) {
    ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock));
    if (!b) return NULL;
    b->data = (uint8_t *)malloc(size ? size : 1);
    if (!b->data) { free(b); return NULL; }
    b->size = size;
    b->next = NULL;
    return b;
}

Arena *arena_create(size_t capacity) {
    if (capacity == 0) capacity = 1024; // default minimal size
    Arena *a = (Arena *)malloc(sizeof(Arena));
    if (!a) return NULL;
    ArenaBlock *b = block_new(capacity);
    if (!b) { free(a); return NULL; }
    a->first = b;
    a->cur   = b;
    a->cur_off = 0;
    return a;
}

void arena_destroy(Arena *arena) {
    if (!arena) return;
    ArenaBlock *b = arena->first;
    while (b) {
        ArenaBlock *next = b->next;
        free(b->data);
        free(b);
        b = next;
    }
    free(arena);
}

void *arena_alloc(Arena *arena, size_t size) {
    if (!arena || size > (size_t)1024 * 1024 * 1024) return NULL;
    size_t aligned = align8(size);
    if (arena->cur_off + aligned > arena->cur->size) {
        /* Append a larger block; earlier blocks (and all pointers into
         * them) remain valid. Grow geometrically so the number of blocks
         * stays O(log total). */
        size_t new_size = arena->cur->size * 2;
        if (new_size < aligned) new_size = aligned;
        ArenaBlock *b = block_new(new_size);
        if (!b) return NULL; /* OOM: arena keeps working at current size */
        b->next = arena->cur->next; /* cur is always the last block */
        arena->cur->next = b;
        arena->cur   = b;
        arena->cur_off = 0;
    }
    void *ptr = arena->cur->data + arena->cur_off;
    arena->cur_off += aligned;
    return ptr;
}

void arena_reset(Arena *arena) {
    if (arena && arena->first) {
        arena->cur = arena->first;
        arena->cur_off = 0;
    }
}

size_t arena_index_of(const Arena *arena, const void *ptr) {
    if (!arena || !ptr) return (size_t)-1;
    const uint8_t *p = (const uint8_t *)ptr;
    size_t running = 0;
    for (const ArenaBlock *b = arena->first; b; b = b->next) {
        if (p >= b->data && p < b->data + b->size) {
            return running + (size_t)(p - b->data);
        }
        running += b->size;
    }
    return (size_t)-1;
}

void *arena_index_to_ptr(const Arena *arena, size_t index) {
    if (!arena) return NULL;
    size_t running = 0; /* bytes in earlier blocks */
    for (ArenaBlock *b = arena->first; b; b = b->next) {
        if (index < running + b->size) return (void *)(b->data + (index - running));
        running += b->size;
    }
    return NULL;
}
