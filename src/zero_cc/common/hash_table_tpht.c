/* -------------------------------------------------------------------------
 * TPHT‑backed implementation of the public ht32_* API.
 *
 * The original `hash_table.h` defines `HashTable32` with fields that are
 * unused by this wrapper.  We store a pointer to a private wrapper struct
 * in the `keys` member and ignore the other members.  This keeps the public
 * API unchanged while redirecting all operations to the cache‑friendly
 * two‑phase hash table (`zero_tpht.h`).
 * ----------------------------------------------------------------------- */

#include "zero_cc/common/hash_table.h"          /* public API declarations */
#include "zero_cc/common/zero_tpht.h"  /* runtime TPHT implementation */
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    ZeroFlattenTpht tpht;   /* actual TPHT storage */
} HashTableWrapper;

/* -------------------------------------------------------------------------
 * Initialise a hash table.
 * Allocates a wrapper, initialises the TPHT with 64 blocks (512 slots),
 * and stores the wrapper pointer in `ht->keys`.  The other fields
 * (`vals`, `capacity`, `tombstones`) are unused and cleared.
 * ----------------------------------------------------------------------- */
bool ht32_init(HashTable32 *ht, size_t initial_capacity)
{
    (void)initial_capacity;                /* unused – TPHT size is fixed */

    if (!ht) return false;

    HashTableWrapper *wrap = calloc(1, sizeof(*wrap));
    if (!wrap) return false;

    // Manual TPHT initialization to avoid wrapper issues
    wrap->tpht.block_count = 64;
    wrap->tpht.blocks = (ZeroFlattenBlock *)zero_posix_calloc(64, sizeof(ZeroFlattenBlock));
    if (!wrap->tpht.blocks) {
        free(wrap);
        return false;
    }
    wrap->tpht.overflow_table = NULL;

    /* Store the wrapper pointer in the opaque `keys` field. */
    ht->keys = (uint32_t *)wrap;
    ht->vals = NULL;
    ht->capacity = 0;
    ht->size = 0;
    ht->tombstones = 0;
    return true;
}

/* -------------------------------------------------------------------------
 * Free resources associated with the table.
 * ----------------------------------------------------------------------- */
void ht32_free(HashTable32 *ht)
{
    if (!ht) return;

    HashTableWrapper *wrap = (HashTableWrapper *)ht->keys;
    if (wrap) {
        zero_flatten_tpht_destroy(&wrap->tpht);
        free(wrap);
    }
    ht->keys = NULL;
    ht->vals = NULL;
    ht->capacity = 0;
    ht->size = 0;
    ht->tombstones = 0;
}

/* -------------------------------------------------------------------------
 * Insert or update a key/value pair.
 * The TPHT returns a non‑negative slot index on success, –1 on failure.
 * On success we bump the public `size` counter.
 * ----------------------------------------------------------------------- */
bool ht32_put(HashTable32 *ht, uint32_t key, uint32_t value)
{
    if (!ht) return false;

    HashTableWrapper *wrap = (HashTableWrapper *)ht->keys;
    if (!wrap) return false;

    /* Check if key already exists — update should not bump size */
    uint32_t existing;
    bool is_update = (zero_flatten_tpht_get(&wrap->tpht, (uint64_t)key, &existing) == 0);

    int ret = zero_flatten_tpht_put(&wrap->tpht, (uint64_t)key, value);
    if (ret == 0) {
        if (!is_update) ht->size++;
        return true;
    }
    return false;               /* table full or other error */
}

/* -------------------------------------------------------------------------
 * Retrieve the value for a key.
 * ----------------------------------------------------------------------- */
bool ht32_get(const HashTable32 *ht, uint32_t key, uint32_t *out_value)
{
    if (!ht) return false;

    const HashTableWrapper *wrap = (const HashTableWrapper *)ht->keys;
    if (!wrap) return false;

    uint32_t val;
    int ret = zero_flatten_tpht_get(&wrap->tpht, (uint64_t)key, &val);
    if (ret == 0 && out_value) {
        *out_value = (uint32_t)val;
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * Delete a key/value pair.
 * The current TPHT implementation (`zero_flatten_tpht`) is immutable, so
 * deletions are not supported.  We return false to signal “not removed”.
 * ----------------------------------------------------------------------- */
bool ht32_remove(HashTable32 *ht, uint32_t key)
{
    (void)ht;
    (void)key;
    return false;   /* immutable TPHT – deletions not possible */
}

/* ht32_len is provided as a static inline in hash_table.h and will report
 * the value stored in `ht->size`, which we keep up‑to‑date in `ht32_put`. */
