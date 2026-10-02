#include <stdio.h>
#include <stdlib.h>
#include "zero_cc/common/hash_table.h"

int main(void) {
    HashTable32 ht;
    if (!ht32_init(&ht, 0)) {
        fprintf(stderr, "Failed to init hash table\n");
        return 1;
    }
    // Insert duplicate key with updated value
    if (!ht32_put(&ht, 42, 1000)) {
        fprintf(stderr, "Initial insert failed\n");
        ht32_free(&ht);
        return 1;
    }
    if (!ht32_put(&ht, 42, 2000)) {
        fprintf(stderr, "Update insert failed\n");
        ht32_free(&ht);
        return 1;
    }
    uint32_t val;
    if (!ht32_get(&ht, 42, &val) || val != 2000) {
        fprintf(stderr, "Duplicate key not updated, got %u\n", val);
        ht32_free(&ht);
        return 1;
    }
    ht32_free(&ht);
    printf("hash_table_tpht_duplicate_test passed\n");
    return 0;
}
