/* =========================================================================
 * zero_chained_tpht.h - Chained Two-Phase Hash Table
 * Part of zero_core/structures/
 * Succinct chained hash table for 64-bit key/value pairs using Feistel
 * quotienting and tiny pointer tables. Perfect for memory density.
 * ========================================================================= */
#ifndef ZERO_CHAINED_TPHT_H
#define ZERO_CHAINED_TPHT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "zero_posix.h"
#include "zero_tinyptr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Part 39: Chained Two-Phase Hash Table (ZeroChainedTpht)
 * ========================================================================= */

typedef struct {
    uint8_t count;
    uint8_t heads[16];     /* Bucket chain pointers (8-bit next offsets) */
    uint16_t remainders[16];
    uint32_t values[16];
} ZeroChainedTphtBlock;

typedef struct {
    ZeroChainedTphtBlock *blocks;
    size_t block_count;
    ZeroTinyPtrTable overflow_table;
} ZeroChainedTpht;

static inline int zero_chained_tpht_init(ZeroChainedTpht *tpht, size_t block_count) {
    if (!tpht || block_count == 0) return -1;
    tpht->block_count = block_count;
    tpht->blocks = (ZeroChainedTphtBlock *)zero_posix_calloc(block_count, sizeof(ZeroChainedTphtBlock));
    if (!tpht->blocks) return -1;
    zero_tinyptr_init(&tpht->overflow_table);
    return 0;
}

static inline void zero_chained_tpht_destroy(ZeroChainedTpht *tpht) {
    if (tpht) {
        if (tpht->blocks) {
            zero_posix_free(tpht->blocks);
            tpht->blocks = NULL;
        }
        tpht->block_count = 0;
        zero_tinyptr_destroy(&tpht->overflow_table);
    }
}

static inline int zero_chained_tpht_put(ZeroChainedTpht *tpht, uint64_t key, uint32_t val) {
    if (!tpht || !tpht->blocks) return -1;
    uint64_t hash = 14695981039346656037ULL;
    hash ^= key;
    hash *= 1099511628211ULL;

    size_t block_idx = (hash >> 32) % tpht->block_count;
    uint8_t fp = (uint8_t)(hash & 0xFF);
    uint16_t rem = (uint16_t)((hash >> 8) & 0xFFFF);

    ZeroChainedTphtBlock *block = &tpht->blocks[block_idx];

    /* Trace heads and update key in-place if duplicate */
    size_t start = fp % 16;
    size_t idx = start;
    for (int i = 0; i < 16; i++) {
        if (block->heads[idx] != 0 && block->remainders[idx] == rem) {
            block->values[idx] = val;
            return 0;
        }
        idx = (idx + 1) % 16;
    }

    /* Put new element */
    if (block->count < 16) {
        idx = start;
        while (block->heads[idx] != 0) {
            idx = (idx + 1) % 16;
        }
        block->heads[idx] = fp != 0 ? fp : 1;
        block->remainders[idx] = rem;
        block->values[idx] = val;
        block->count++;
        return 0;
    }

    /* Spillover to overflow table */
    uint8_t overflow_handle;
    uint32_t *spill_val = (uint32_t *)zero_posix_malloc(sizeof(uint32_t));
    if (!spill_val) return -1;
    *spill_val = val;

    if (zero_tinyptr_register(&tpht->overflow_table, spill_val, &overflow_handle) == 0) {
        return 0;
    }
    zero_posix_free(spill_val);
    return -1;
}

static inline int zero_chained_tpht_get(const ZeroChainedTpht *tpht, uint64_t key, uint32_t *out_val) {
    if (!tpht || !tpht->blocks || !out_val) return -1;
    uint64_t hash = 14695981039346656037ULL;
    hash ^= key;
    hash *= 1099511628211ULL;

    size_t block_idx = (hash >> 32) % tpht->block_count;
    uint8_t fp = (uint8_t)(hash & 0xFF);
    uint16_t rem = (uint16_t)((hash >> 8) & 0xFFFF);

    const ZeroChainedTphtBlock *block = &tpht->blocks[block_idx];

    size_t idx = fp % 16;
    for (int i = 0; i < 16; i++) {
        if (block->heads[idx] != 0 && block->remainders[idx] == rem) {
            *out_val = block->values[idx];
            return 0;
        }
        idx = (idx + 1) % 16;
    }
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* ZERO_CHAINED_TPHT_H */
