#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Simple open-addressing hash table with linear probing.
 * Keys and values are 32‑bit integers. An empty slot is marked with
 * UINT32_MAX for the key. This design avoids heavy pointer chasing –
 * the entire table is a contiguous pair of arrays stored in a single
 * allocation, which gives good cache locality.
 *
 * The table automatically grows when the load factor exceeds 0.7.
 */

typedef struct {
    uint32_t *keys;   // array of keys, length = capacity
    uint32_t *vals;   // parallel array of values
    size_t    capacity; // always a power of two
    size_t    size;     // number of occupied entries (excluding tombstones)
    size_t    tombstones; // number of deleted slots
} HashTable32;

/** Initialise a hash table. The capacity will be the next power‑of‑two
 *  greater than or equal to `initial_capacity`. Returns true on success. */
bool ht32_init(HashTable32 *ht, size_t initial_capacity);

/** Free resources associated with the table. */
void ht32_free(HashTable32 *ht);

/** Insert or update a key/value pair. Returns true on success, false on OOM. */
bool ht32_put(HashTable32 *ht, uint32_t key, uint32_t value);

/** Retrieve the value for `key`. Returns true if found and stores the
 *  value into `out_value`. */
bool ht32_get(const HashTable32 *ht, uint32_t key, uint32_t *out_value);

/** Delete a key/value pair. Returns true if the key was present. */
bool ht32_remove(HashTable32 *ht, uint32_t key);

/** Current number of entries (excluding tombstones). */
static inline size_t ht32_len(const HashTable32 *ht) { return ht->size; }
