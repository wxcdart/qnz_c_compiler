#ifndef ZERO_CC_OPT_ALIAS_H
#define ZERO_CC_OPT_ALIAS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;
typedef struct ZeroCCType ZeroCCType;

/**
 * Type-Based Alias Analysis (TBAA - Tier 3, Item #15)
 *
 * Implements ISO C99 6.5p7 strict aliasing rules:
 * - Two pointers of incompatible types cannot alias.
 * - Character pointers (char, uint8_t, signed char) can alias any type.
 * - Restrict qualifiers with distinct groups cannot alias.
 * - Distinct stack slots and disjoint globals cannot alias.
 */

typedef enum {
    ALIAS_NO = 0,      /* Proven not to alias */
    ALIAS_MAYBE = 1,   /* May alias */
    ALIAS_MUST = 2     /* Proven to alias identical location */
} AliasResult;

typedef enum {
    TBAA_UNKNOWN = 0,
    TBAA_CHAR,         /* char, signed char, unsigned char (universal aliaser) */
    TBAA_INT16,        /* short, unsigned short */
    TBAA_INT32,        /* int, unsigned int */
    TBAA_INT64,        /* long, unsigned long, long long, pointer */
    TBAA_FLOAT,        /* float */
    TBAA_DOUBLE        /* double */
} TBAATypeClass;

/* Query TBAA type classification from IR opcode */
TBAATypeClass zero_cc_tbaa_class_from_op(int op);

/* Check if two TBAA type classes can alias under C99 6.5p7 */
bool zero_cc_tbaa_can_alias(TBAATypeClass t1, TBAATypeClass t2);

/* Check if two semantic types can alias under C99 6.5p7 */
bool zero_cc_tbaa_types_can_alias(const ZeroCCType *t1, const ZeroCCType *t2);

/* Universal alias query between two memory operations */
AliasResult zero_cc_alias_query(ZeroCCCompiler *c,
                                int op1, uint32_t addr1, uint8_t group1,
                                int op2, uint32_t addr2, uint8_t group2);

#endif /* ZERO_CC_OPT_ALIAS_H */
