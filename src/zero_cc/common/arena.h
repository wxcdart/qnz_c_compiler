#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Segmented bump-allocator arena used by the ZeroCC compiler.
 *
 * The arena owns a *chain* of malloc'd blocks. Allocation is linear within
 * the current block; when it fills, a larger block is appended and
 * allocation continues there. Crucially, **existing pointers stay valid
 * across growth** (the classic single-buffer arena invalidated every
 * pointer on realloc - e.g. a raw TypeInfo* held across an initializer
 * evaluation once the symbol arena grew past ~8k declarations).
 *
 * `arena_reset` rewinds to the first block without freeing memory, which
 * is ideal for the temporary arena reused between functions.
 *
 * Indexes (`arena_index_of` / `arena_index_to_ptr`) are byte offsets in
 * block order; because blocks are only ever appended, the index of an
 * existing allocation is stable.
 */

typedef struct ArenaBlock {
    size_t        size;   // bytes in this block
    uint8_t      *data;   // block storage
    struct ArenaBlock *next;
} ArenaBlock;

typedef struct {
    ArenaBlock *first;    // head of the block chain
    ArenaBlock *cur;      // block currently being filled
    size_t      cur_off;  // bytes used in the current block
} Arena;

/** Create a new arena with an initial block of at least `capacity` bytes. Returns NULL on OOM. */
Arena *arena_create(size_t capacity);

/** Destroy the arena, freeing all blocks. */
void arena_destroy(Arena *arena);

/** Allocate `size` bytes from the arena, returned pointer is 8‑byte aligned. */
void *arena_alloc(Arena *arena, size_t size);

/** Reset the arena to empty (reuse existing blocks) without freeing memory. */
void arena_reset(Arena *arena);

/** Return the stable arena-wide byte index for a pointer previously returned by `arena_alloc`. */
size_t arena_index_of(const Arena *arena, const void *ptr);

/** Convert a previously‑saved index back into a pointer. Returns NULL if out of bounds. */
void *arena_index_to_ptr(const Arena *arena, size_t index);
