#include <stdio.h>
#include <stdlib.h>
#include "../src/zero_cc/hash_table.h"

int main(void) {
    HashTable32 ht;
    if (!ht32_init(&ht, 0)) {
        fprintf(stderr, "Failed to init hash table\n");
        return 1;
    }

    // Insert some key/value pairs
    if (!ht32_put(&ht, 1, 100) || !ht32_put(&ht, 2, 200) || !ht32_put(&ht, 3, 300)) {
        fprintf(stderr, "Insert failed\n");
        ht32_free(&ht);
        return 1;
    }

    // Verify size
    if (ht32_len(&ht) != 3) {
        fprintf(stderr, "Size mismatch: expected 3 got %zu\n", ht32_len(&ht));
        ht32_free(&ht);
        return 1;
    }

    // Retrieve values
    uint32_t val;
    if (!ht32_get(&ht, 1, &val) || val != 100) {
        fprintf(stderr, "Lookup key 1 failed (got %u)\n", val);
        ht32_free(&ht);
        return 1;
    }
    if (!ht32_get(&ht, 2, &val) || val != 200) {
        fprintf(stderr, "Lookup key 2 failed (got %u)\n", val);
        ht32_free(&ht);
        return 1;
    }
    if (!ht32_get(&ht, 3, &val) || val != 300) {
        fprintf(stderr, "Lookup key 3 failed (got %u)\n", val);
        ht32_free(&ht);
        return 1;
    }

    // Missing key should return false
    if (ht32_get(&ht, 99, &val)) {
        fprintf(stderr, "Unexpected success for missing key\n");
        ht32_free(&ht);
        return 1;
    }

    // Delete is not supported; should return false
    if (ht32_remove(&ht, 1)) {
        fprintf(stderr, "Remove unexpectedly succeeded\n");
        ht32_free(&ht);
        return 1;
    }

    ht32_free(&ht);
    printf("hash_table_tpht_test passed\n");
    return 0;
}
