#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Simple bump‑allocator arena used by the ZeroCC compiler.
 *
 * The arena owns a contiguous byte buffer. Allocation is linear and fast;
 * `arena_reset` clears the arena without freeing the underlying memory, which
 * is ideal for the temporary arena used during a single compilation unit.
 */

typedef struct {
    uint8_t *buffer;   // base of the arena memory
    size_t   capacity; // total size of the buffer in bytes
    size_t   offset;   // current allocation offset (bytes used)
} Arena;

/** Create a new arena with at least `capacity` bytes. Returns NULL on OOM. */
Arena *arena_create(size_t capacity);

/** Destroy an arena, freeing its underlying buffer. */
void arena_destroy(Arena *arena);

/** Allocate `size` bytes from the arena, returned pointer is 8‑byte aligned. */
void *arena_alloc(Arena *arena, size_t size);

/** Reset the arena to empty (offset = 0) without freeing the buffer. */
void arena_reset(Arena *arena);

/** Return the arena‑relative byte index for a pointer previously returned by `arena_alloc`. */
size_t arena_index_of(const Arena *arena, const void *ptr);

/** Convert a previously‑saved index back into a pointer. Returns NULL if out of bounds. */
void *arena_index_to_ptr(const Arena *arena, size_t index);
