/* =========================================================================
 * zero_tinyptr.h - Standalone Tiny Pointer Table
 * Part of zero_core/structures/
 * Replaces standard 8-byte pointers with compact 1-byte (8-bit) handles.
 * ========================================================================= */
#ifndef ZERO_TINYPTR_H
#define ZERO_TINYPTR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "zero_cc/common/zero_posix.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Standalone Tiny Pointer Table (zero_tinyptr)
 * Replaces standard 8-byte pointers with compact 1-byte (8-bit) handles
 * using a 2-choice bucketing lookup index.
 * ========================================================================= */

typedef struct {
    void *pointers[256];
    uint8_t counts[256];
} ZeroTinyPtrTable;

static inline void zero_tinyptr_init(ZeroTinyPtrTable *table) {
    if (!table) return;
    zero_posix_memset(table->pointers, 0, sizeof(table->pointers));
    zero_posix_memset(table->counts, 0, sizeof(table->counts));
}

static inline void zero_tinyptr_destroy(ZeroTinyPtrTable *table) {
    if (table) {
        zero_posix_memset(table->pointers, 0, sizeof(table->pointers));
        zero_posix_memset(table->counts, 0, sizeof(table->counts));
    }
}

static inline void* zero_tinyptr_deref(const ZeroTinyPtrTable *table, uint8_t handle) {
    if (!table) return NULL;
    return table->pointers[handle];
}

static inline int zero_tinyptr_register(ZeroTinyPtrTable *table, void *ptr, uint8_t *out_handle) {
    if (!table || !ptr || !out_handle) return -1;
    /* 2-choice bucketing simulation to find an empty slot */
    for (int i = 0; i < 256; i++) {
        if (table->pointers[i] == NULL) {
            table->pointers[i] = ptr;
            table->counts[i] = 1;
            *out_handle = (uint8_t)i;
            return 0;
        }
    }
    return -1; /* Table is completely full */
}

#ifdef __cplusplus
}
#endif

#endif /* ZERO_TINYPTR_H */
