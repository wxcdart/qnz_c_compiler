#ifndef ZERO_CC_IR_ARENA_H
#define ZERO_CC_IR_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"

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
 * Call argument tracking for OP_CALL instructions
 * 
 * Each call with more than 4 args needs to track all temps separately.
 */
/* Call argument tracking for OP_CALL instructions */
/* (Defined in ir.h to avoid circular dependency) */

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
 * Initialize call args arena
 * 
 * @param arena The arena to initialize
 * @return true on success, false on failure
 */
static inline bool ir_call_args_arena_init(IRCallArgsArena *arena) {
    if (!arena) return false;
    
    arena->capacity = IR_CALL_MAX_SITES;
    arena->table = (IRCallArgs *)malloc(arena->capacity * sizeof(IRCallArgs));
    
    if (!arena->table) {
        arena->capacity = 0;
        arena->count = 0;
        return false;
    }
    
    memset(arena->table, 0, arena->capacity * sizeof(IRCallArgs));
    arena->count = 0;
    return true;
}

/**
 * Free call args arena
 * 
 * @param arena The arena to free
 */
static inline void ir_call_args_arena_free(IRCallArgsArena *arena) {
    if (!arena) return;
    
    if (arena->table) {
        free(arena->table);
        arena->table = NULL;
    }
    arena->count = 0;
    arena->capacity = 0;
}

/**
 * Reset call args arena (reuse memory)
 * 
 * @param arena The arena to reset
 */
static inline void ir_call_args_arena_reset(IRCallArgsArena *arena) {
    if (!arena || !arena->table) return;
    arena->count = 0;
}

/**
 * Record call args for a call instruction
 * 
 * @param arena The arena
 * @param instr_index Index of the OP_CALL instruction
 * @param count Number of arguments
 * @param temps Array of temp registers
 * @param is_fp Array indicating FP vs integer args
 */
static inline void ir_call_args_arena_record(IRCallArgsArena *arena, 
                                              size_t instr_index, int count,
                                              const int *temps, const int *is_fp) {
    if (!arena || !arena->table) return;
    if (arena->count >= arena->capacity) return;
    
    IRCallArgs *e = &arena->table[arena->count++];
    e->instr_index = instr_index;
    e->count = count > IR_CALL_MAX_ARGS ? IR_CALL_MAX_ARGS : count;
    
    for (int i = 0; i < e->count; i++) {
        e->temps[i] = (uint8_t)temps[i];
        e->is_fp[i] = is_fp ? (uint8_t)is_fp[i] : 0;
    }
}

/**
 * Find call args for a call instruction
 * 
 * @param arena The arena
 * @param instr_index Index of the OP_CALL instruction
 * @return Pointer to call args, or NULL if not found
 */
static inline const IRCallArgs *ir_call_args_arena_find(IRCallArgsArena *arena, 
                                                         size_t instr_index) {
    if (!arena || !arena->table) return NULL;
    
    for (size_t i = 0; i < arena->count; i++) {
        if (arena->table[i].instr_index == instr_index) {
            return &arena->table[i];
        }
    }
    return NULL;
}

#endif /* ZERO_CC_IR_ARENA_H */
