#ifndef ZERO_CC_OPT_PGO_H
#define ZERO_CC_OPT_PGO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

typedef struct ZeroCCPGORecord {
    char *fn_name;
    uint32_t block_id;
    uint64_t count;
} ZeroCCPGORecord;

typedef struct ZeroCCPGOProfile {
    ZeroCCPGORecord *records;
    size_t count;
    size_t capacity;
    uint64_t total_hits;
} ZeroCCPGOProfile;

/* Load profile records from file. Returns NULL on failure. */
ZeroCCPGOProfile *zc_pgo_load_profile(const char *path);

/* Free profile memory */
void zc_pgo_free_profile(ZeroCCPGOProfile *prof);

/* Query execution count for a specific block in a function */
uint64_t zc_pgo_get_block_count(const ZeroCCPGOProfile *prof, const char *fn_name, uint32_t block_id);

/* Query execution count for function entry (block 0) */
uint64_t zc_pgo_get_fn_count(const ZeroCCPGOProfile *prof, const char *fn_name);

/* Check if function is considered hot (entry count >= 10) */
bool zc_pgo_is_fn_hot(const ZeroCCPGOProfile *prof, const char *fn_name);

/* Check if function is considered cold (entry count == 0) */
bool zc_pgo_is_fn_cold(const ZeroCCPGOProfile *prof, const char *fn_name);

/* Emit instrumentation call __zc_pgo_hit(fn_name, block_id) */
void zc_pgo_emit_hit(ZeroCCCompiler *c, const char *fn_name, uint32_t block_id);

#endif /* ZERO_CC_OPT_PGO_H */
