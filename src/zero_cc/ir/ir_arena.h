#ifndef ZERO_CC_IR_ARENA_H
#define ZERO_CC_IR_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ir.h"

/**
 * IR Arena - Dynamic growing instruction buffer
 *
 * Provides:
 * - O(1) instruction emission with automatic growth
 * - Reentrancy: separate arena per function/compilation
 * - No heap fragmentation: single realloc per growth
 * - Efficient reset: just reset count, reuse memory
 *
 * Usage:
 *   IRArena arena;
 *   ir_arena_init(&arena, 1024);  // Start with 1K instructions
 *   ... emit instructions ...
 *   ir_arena_reset(&arena);        // Reset for next function (no free!)
 *   ir_arena_free(&arena);         // Free when done
 */

typedef struct {
    Instr *buffer;      /* Dynamic instruction buffer */
    size_t capacity;    /* Current buffer capacity */
    size_t count;       /* Number of instructions emitted */
} IRArena;

/**
 * Initialize IR arena with given initial capacity
 *
 * @param arena The arena to initialize
 * @param initial_cap Initial capacity (0 = default 1024)
 * @return true on success, false on allocation failure
 */
static inline bool ir_arena_init(IRArena *arena, size_t initial_cap) {
    if (!arena) return false;

    arena->capacity = initial_cap ? initial_cap : 1024;
    arena->buffer = (Instr *)malloc(arena->capacity * sizeof(Instr));

    if (!arena->buffer) {
        arena->capacity = 0;
        arena->count = 0;
        return false;
    }

    memset(arena->buffer, 0, arena->capacity * sizeof(Instr));
    arena->count = 0;
    return true;
}

/**
 * Free IR arena resources
 *
 * @param arena The arena to free
 */
static inline void ir_arena_free(IRArena *arena) {
    if (!arena) return;

    if (arena->buffer) {
        free(arena->buffer);
        arena->buffer = NULL;
    }
    arena->capacity = 0;
    arena->count = 0;
}

/**
 * Reset IR arena for next function (reuses allocated memory)
 *
 * @param arena The arena to reset
 */
static inline void ir_arena_reset(IRArena *arena) {
    if (!arena || !arena->buffer) return;

    arena->count = 0;
    memset(arena->buffer, 0, sizeof(Instr));  /* Clear first instr only */
}

/**
 * Emit instruction - O(1) amortized with automatic growth
 *
 * @param arena The arena to emit into
 * @param instr The instruction to emit
 * @return Pointer to emitted instruction, or NULL on failure
 */
static inline Instr *ir_arena_emit(IRArena *arena, Instr instr) {
    if (!arena || !arena->buffer) return NULL;

    /* Grow buffer if needed */
    if (arena->count >= arena->capacity) {
        size_t new_cap = arena->capacity * 2;
        if (new_cap == 0) new_cap = 1024;  /* Initial capacity */

        Instr *new_buffer = (Instr *)realloc(arena->buffer, new_cap * sizeof(Instr));
        if (!new_buffer) return NULL;  /* Allocation failed */

        arena->buffer = new_buffer;
        arena->capacity = new_cap;
    }

    /* Emit instruction */
    arena->buffer[arena->count] = instr;
    return &arena->buffer[arena->count++];
}

/**
 * Get instruction at index
 *
 * @param arena The arena
 * @param idx Instruction index
 * @return Pointer to instruction, or NULL if out of bounds
 */
static inline Instr *ir_arena_get(IRArena *arena, size_t idx) {
    if (!arena || !arena->buffer || idx >= arena->count) return NULL;
    return &arena->buffer[idx];
}

/**
 * Get instruction buffer (for iteration)
 *
 * @param arena The arena
 * @return Pointer to instruction buffer
 */
static inline const Instr *ir_arena_get_buffer(IRArena *arena) {
    if (!arena) return NULL;
    return arena->buffer;
}

/**
 * Get instruction count
 *
 * @param arena The arena
 * @return Number of instructions emitted
 */
static inline size_t ir_arena_get_count(IRArena *arena) {
    if (!arena) return 0;
    return arena->count;
}

/**
 * Insert instruction at specific index
 *
 * Shifts all instructions at index and beyond to make room.
 * O(n) operation where n is the number of instructions after insertion point.
 *
 * @param arena The arena
 * @param idx Insertion index (0 to count)
 * @param instr The instruction to insert
 * @return Pointer to inserted instruction, or NULL on failure
 */
static inline Instr *ir_arena_insert(IRArena *arena, size_t idx, Instr instr) {
    if (!arena || !arena->buffer) return NULL;
    if (idx > arena->count) return NULL;  /* Can insert at end, but not beyond */

    /* Grow buffer if needed */
    if (arena->count >= arena->capacity) {
        size_t new_cap = arena->capacity * 2;
        if (new_cap == 0) new_cap = 1024;  /* Initial capacity */

        Instr *new_buffer = (Instr *)realloc(arena->buffer, new_cap * sizeof(Instr));
        if (!new_buffer) return NULL;  /* Allocation failed */

        arena->buffer = new_buffer;
        arena->capacity = new_cap;
    }

    /* Shift instructions to make room */
    /* Move from [idx, count-1] to [idx+1, count] */
    memmove(&arena->buffer[idx + 1], &arena->buffer[idx],
            (arena->count - idx) * sizeof(Instr));

    /* Insert instruction */
    arena->buffer[idx] = instr;
    arena->count++;

    return &arena->buffer[idx];
}

#endif /* ZERO_CC_IR_ARENA_H */
