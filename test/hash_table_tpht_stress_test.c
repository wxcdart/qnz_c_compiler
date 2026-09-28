#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "zero_cc/data_structures/hash_table.h"

/* Stress test: insert many keys until the table reports failure. */
int main(void) {
    HashTable32 ht;
    if (!ht32_init(&ht, 0)) {
        fprintf(stderr, "Failed to init hash table\n");
        return 1;
    }
    /* The TPHT has 64 primary blocks (8 slots each = 512) plus an overflow
     * table (~61 blocks), so the real capacity exceeds 512.  We just verify
     * that all 512 inserts succeed and lookups are correct. */
    const size_t max_slots = 512;
    size_t inserted = 0;
    for (uint32_t i = 0; i < max_slots; ++i) {
        if (!ht32_put(&ht, i, i * 10)) {
            fprintf(stderr, "Insertion failed early at i=%u\n", i);
            ht32_free(&ht);
            return 1;
        }
        ++inserted;
    }
    if (ht32_len(&ht) != inserted) {
        fprintf(stderr, "Size mismatch after bulk insert: expected %zu got %zu\n", inserted, ht32_len(&ht));
        ht32_free(&ht);
        return 1;
    }
    /* Verify a few random lookups */
    uint32_t val;
    if (!ht32_get(&ht, 123, &val) || val != 1230) {
        fprintf(stderr, "Lookup failure for key 123, got %u\n", val);
        ht32_free(&ht);
        return 1;
    }
    if (!ht32_get(&ht, 511, &val) || val != 5110) {
        fprintf(stderr, "Lookup failure for key 511, got %u\n", val);
        ht32_free(&ht);
        return 1;
    }
    ht32_free(&ht);
    printf("hash_table_tpht_stress_test passed\n");
    return 0;
}
