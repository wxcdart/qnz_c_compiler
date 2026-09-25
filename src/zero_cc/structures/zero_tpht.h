/* ============================================================
 * zero_tpht.h - Flattened Two-Phase Hash Table
 * Part of zero_core/structures/
 * ============================================================ */
#ifndef ZERO_TPHT_H
#define ZERO_TPHT_H

#include <stddef.h>
#include <stdint.h>
#include "zero_posix.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) {
    uint8_t fingerprints[12];
    uint16_t remainders[8];
    uint32_t values[8];
    uint8_t tiny_pointers[2];
    uint8_t count;
    uint8_t version;
} ZeroFlattenBlock;

typedef char assert_flatten_block_size[sizeof(ZeroFlattenBlock) == 64 ? 1 : -1];

typedef struct {
    ZeroFlattenBlock *blocks;
    size_t block_count;
    void *overflow_table;
} ZeroFlattenTpht;

/* Two-Phase Hash Table Functions */
static inline int zero_flatten_tpht_init(ZeroFlattenTpht *tpht, size_t block_count) {
    if (!tpht || block_count == 0) return -1;
    tpht->block_count = block_count;
    tpht->blocks = (ZeroFlattenBlock *)zero_posix_calloc(block_count, sizeof(ZeroFlattenBlock));
    if (!tpht->blocks) return -1;
    tpht->overflow_table = NULL;
    return 0;
}

static inline void zero_flatten_tpht_destroy(ZeroFlattenTpht *tpht) {
    if (tpht) {
        if (tpht->blocks) {
            zero_posix_free(tpht->blocks);
            tpht->blocks = NULL;
        }
        tpht->block_count = 0;
    }
}

static inline int zero_flatten_tpht_put(ZeroFlattenTpht *tpht, uint64_t key, uint32_t val) {
    if (!tpht || !tpht->blocks) return -1;
    uint64_t hash = 14695981039346656037ULL;
    hash ^= key;
    hash *= 1099511628211ULL;
    size_t block_idx = (hash >> 32) % tpht->block_count;
    uint8_t fp = (uint8_t)(hash & 0xFF);
    uint16_t rem = (uint16_t)((hash >> 8) & 0xFFFF);
    ZeroFlattenBlock *block = &tpht->blocks[block_idx];

    for (int i = 0; i < 8; i++) {
        if (block->fingerprints[i] == fp && block->remainders[i] == rem) {
            block->values[i] = val;
            return 0;
        }
    }

    if (block->count < 8) {
        int idx = block->count;
        block->fingerprints[idx] = fp;
        block->remainders[idx] = rem;
        block->values[idx] = val;
        block->count++;
        return 0;
    }
    return -1;
}

static inline int zero_flatten_tpht_get(const ZeroFlattenTpht *tpht, uint64_t key, uint32_t *out_val) {
    if (!tpht || !tpht->blocks || !out_val) return -1;
    uint64_t hash = 14695981039346656037ULL;
    hash ^= key;
    hash *= 1099511628211ULL;
    size_t block_idx = (hash >> 32) % tpht->block_count;
    uint8_t fp = (uint8_t)(hash & 0xFF);
    uint16_t rem = (uint16_t)((hash >> 8) & 0xFFFF);
    const ZeroFlattenBlock *block = &tpht->blocks[block_idx];

    for (int i = 0; i < 8; i++) {
        if (block->fingerprints[i] == fp && block->remainders[i] == rem) {
            *out_val = block->values[i];
            return 0;
        }
    }
    return -1;
}

static inline void zero_auto_tpht_cleanup(ZeroFlattenTpht *tpht) {
    if (tpht) {
        zero_flatten_tpht_destroy(tpht);
        zero_posix_memset(tpht, 0, sizeof(ZeroFlattenTpht));
    }
}

#define ZERO_AUTO_TPHT(name, block_count) \
    ZeroFlattenTpht name __attribute__((cleanup(zero_auto_tpht_cleanup))); \
    (void)(&name == (ZeroFlattenTpht*)&name); \
    zero_flatten_tpht_init(&name, (block_count))

#ifdef __cplusplus
}
#endif

#endif /* ZERO_TPHT_H */
