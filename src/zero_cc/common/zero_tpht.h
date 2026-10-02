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
    uint8_t fingerprints[8];
    uint8_t _pad[4];         /* keep 64-byte block size */
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

static inline void zero_flatten_tpht_init_overflow(ZeroFlattenTpht *tpht) {
    if (!tpht || tpht->overflow_table) return;
    ZeroFlattenTpht *overflow = (ZeroFlattenTpht *)zero_posix_calloc(1, sizeof(ZeroFlattenTpht));
    if (!overflow) return;
    size_t overflow_blocks = 61; // Prime number, different from primary's 64
    if (overflow_blocks == 0) overflow_blocks = 1;
    if (zero_flatten_tpht_init(overflow, overflow_blocks) == 0) {
        tpht->overflow_table = overflow;
    } else {
        zero_posix_free(overflow);
    }
}

/* splitmix64-style finaliser: good avalanche into both halves */
static inline uint64_t zero_tpht_hash(uint64_t key, uint64_t seed) {
    uint64_t h = key + seed;
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

static inline int zero_flatten_tpht_put_overflow(ZeroFlattenTpht *tpht, uint64_t key, uint32_t val) {
    if (!tpht || !tpht->blocks) return -1;
    uint64_t hash = zero_tpht_hash(key, 0x9e3779b97f4a7c15ULL);  /* different seed for overflow */
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
    return -1; // Overflow table full (should be rare)
}

static inline int zero_flatten_tpht_get_overflow(const ZeroFlattenTpht *tpht, uint64_t key, uint32_t *out_val) {
    if (!tpht || !tpht->blocks || !out_val) return -1;
    uint64_t hash = zero_tpht_hash(key, 0x9e3779b97f4a7c15ULL);
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

static inline void zero_flatten_tpht_destroy(ZeroFlattenTpht *tpht) {
    if (tpht) {
        if (tpht->blocks) {
            zero_posix_free(tpht->blocks);
            tpht->blocks = NULL;
        }
        if (tpht->overflow_table) {
            zero_flatten_tpht_destroy(tpht->overflow_table);
            zero_posix_free(tpht->overflow_table);
            tpht->overflow_table = NULL;
        }
        tpht->block_count = 0;
    }
}

static inline int zero_flatten_tpht_put(ZeroFlattenTpht *tpht, uint64_t key, uint32_t val) {
    if (!tpht || !tpht->blocks) return -1;
    uint64_t hash = zero_tpht_hash(key, 0x517cc1b727220a95ULL);
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

    // Block is full, use overflow table
    if (!tpht->overflow_table) {
        zero_flatten_tpht_init_overflow(tpht);
    }
    if (tpht->overflow_table) {
        return zero_flatten_tpht_put_overflow(tpht->overflow_table, key, val);
    }
    return -1;
}

static inline int zero_flatten_tpht_get(const ZeroFlattenTpht *tpht, uint64_t key, uint32_t *out_val) {
    if (!tpht || !tpht->blocks || !out_val) return -1;
    uint64_t hash = zero_tpht_hash(key, 0x517cc1b727220a95ULL);
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

    // Check overflow table with different hash seed
    if (tpht->overflow_table) {
        return zero_flatten_tpht_get_overflow(tpht->overflow_table, key, out_val);
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