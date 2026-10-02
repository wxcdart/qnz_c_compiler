#include "arena.h"
#include <stdlib.h>
#include <string.h>

/* Helper: round up size to 8‑byte alignment */
static size_t align8(size_t size) {
    return (size + 7U) & ~((size_t)7U);
}

Arena *arena_create(size_t capacity) {
    if (capacity == 0) capacity = 1024; // default minimal size
    Arena *a = (Arena *)malloc(sizeof(Arena));
    if (!a) return NULL;
    a->buffer = (uint8_t *)malloc(capacity);
    if (!a->buffer) { free(a); return NULL; }
    a->capacity = capacity;
    a->offset   = 0;
    return a;
}

void arena_destroy(Arena *arena) {
    if (!arena) return;
    free(arena->buffer);
    free(arena);
}

void *arena_alloc(Arena *arena, size_t size) {
    if (!arena || size > (size_t)1024 * 1024 * 1024) return NULL;
    size_t aligned = align8(size);
    if (arena->offset + aligned > arena->capacity) {
        /* Optional: grow the buffer (simple reallocation) */
        size_t new_cap = arena->capacity * 2;
        if (new_cap == 0) new_cap = 1024;
        while (new_cap < arena->offset + aligned) {
            size_t next_cap = new_cap * 2;
            if (next_cap <= new_cap) return NULL; /* overflow guard */
            new_cap = next_cap;
        }
        uint8_t *new_buf = (uint8_t *)realloc(arena->buffer, new_cap);
        if (!new_buf) return NULL;
        arena->buffer = new_buf;
        arena->capacity = new_cap;
    }
    void *ptr = arena->buffer + arena->offset;
    arena->offset += aligned;
    return ptr;
}

void arena_reset(Arena *arena) {
    if (arena) arena->offset = 0;
}

size_t arena_index_of(const Arena *arena, const void *ptr) {
    if (!arena || !ptr) return (size_t)-1;
    const uint8_t *base = arena->buffer;
    const uint8_t *p = (const uint8_t *)ptr;
    if (p < base || p >= base + arena->capacity) return (size_t)-1;
    return (size_t)(p - base);
}

void *arena_index_to_ptr(const Arena *arena, size_t index) {
    if (!arena || index >= arena->capacity) return NULL;
    return (void *)(arena->buffer + index);
}
