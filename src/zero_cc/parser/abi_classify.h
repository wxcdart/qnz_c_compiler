/* System V AMD64 ABI Struct/Union Eightbyte Classification */
#ifndef ZERO_CC_ABI_CLASSIFY_H
#define ZERO_CC_ABI_CLASSIFY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "zero_cc/types/type_system.h"

typedef enum {
    ABI_CLASS_NO_CLASS = 0,
    ABI_CLASS_INTEGER,
    ABI_CLASS_SSE,
    ABI_CLASS_MEMORY
} ABIClass;

typedef struct {
    ABIClass classes[2];
    int count; /* Number of eightbytes (1 or 2), or 0 if MEMORY */
} ABIStructClassification;

/*
 * Classify a struct/union or scalar type according to System V AMD64 ABI §3.2.3.
 *
 * Rules:
 * 1. If size > 16 bytes or 0 bytes -> MEMORY.
 * 2. If size <= 8 bytes -> 1 eightbyte.
 * 3. If size <= 16 bytes -> 2 eightbytes.
 * 4. Each eightbyte is classified:
 *    - If all members/bytes in the eightbyte are float/double -> SSE.
 *    - If any member in the eightbyte is integer/pointer -> INTEGER (INTEGER has precedence).
 *    - If unaligned or complex fields -> MEMORY.
 */
static inline ABIStructClassification abi_classify_struct(const ZeroCCType *type) {
    ABIStructClassification result = {{ABI_CLASS_NO_CLASS, ABI_CLASS_NO_CLASS}, 0};

    if (!type) {
        result.count = 1;
        result.classes[0] = ABI_CLASS_INTEGER;
        return result;
    }

    if (type->tag != TYPE_STRUCT) {
        result.count = 1;
        if (type->tag == TYPE_FLOAT || type->tag == TYPE_DOUBLE) {
            result.classes[0] = ABI_CLASS_SSE;
        } else {
            result.classes[0] = ABI_CLASS_INTEGER;
        }
        return result;
    }

    size_t size = type->size;
    if (size > 16 || size == 0) {
        result.count = 0;
        result.classes[0] = ABI_CLASS_MEMORY;
        return result;
    }

    result.count = (size <= 8) ? 1 : 2;

    /* Classify each eightbyte based on member tags and byte ranges */
    for (int eb = 0; eb < result.count; eb++) {
        size_t eb_start = eb * 8;
        size_t eb_end = eb_start + 8;
        bool has_sse = false;
        bool has_int = false;

        for (size_t m = 0; m < type->member_count; m++) {
            const ZeroCCStructMember *mem = &type->members[m];
            size_t m_start = mem->offset;
            size_t m_end = m_start + (mem->size ? mem->size : 1);

            /* Check if this member overlaps with eightbyte eb */
            if (m_start < eb_end && m_end > eb_start) {
                if (mem->tag == TYPE_FLOAT || mem->tag == TYPE_DOUBLE) {
                    has_sse = true;
                } else {
                    has_int = true;
                }
            }
        }

        /* ABI precedence rule: If any member in the eightbyte is INTEGER,
         * the entire eightbyte is classified as INTEGER.
         * Only if all overlapping fields are float/double is it SSE. */
        if (has_int) {
            result.classes[eb] = ABI_CLASS_INTEGER;
        } else if (has_sse) {
            result.classes[eb] = ABI_CLASS_SSE;
        } else {
            result.classes[eb] = ABI_CLASS_INTEGER;
        }
    }

    return result;
}

#endif /* ZERO_CC_ABI_CLASSIFY_H */
