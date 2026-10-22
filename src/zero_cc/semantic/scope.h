#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "zero_cc/common/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct TypeInfo TypeInfo;
typedef struct ScopeBucket ScopeBucket;

/* Symbol kinds supported in symbol table */
typedef enum SymbolKind {
    SYMBOL_VAR,
    SYMBOL_FUNC,
    SYMBOL_TYPEDEF,
    SYMBOL_TAG      /* struct, union, enum tags */
} SymbolKind;

/* Symbol entry stored in scope */
typedef struct Symbol {
    const char *name;
    SymbolKind  kind;
    TypeInfo   *type;
    uint32_t    flags;
    void       *node;   /* optional AST node or definition pointer */
    struct Symbol *next_in_bucket; /* chained in hash bucket */
} Symbol;

/* Scope representation */
typedef struct Scope {
    Symbol        **buckets;     /* dynamic hash table buckets (calloc'd) */
    size_t          bucket_count;/* number of buckets (power of two) */
    size_t          count;       /* number of symbols in this scope */
    struct Scope   *parent;      /* lexical nesting chain */
    Arena          *perm_arena;  /* arena for symbol and scope allocations */
} Scope;

/**
 * Push a new scope onto the scope hierarchy stack.
 * If *cur is not NULL, the new scope's parent will be *cur.
 * *cur is updated to point to the new scope.
 * Returns the newly created scope (or NULL on OOM).
 */
Scope *scope_enter(Scope **cur, Arena *perm);

/**
 * Pop the current scope, updating *cur to point to its parent.
 * Resources tied to the scope's TPHT are cleaned up.
 */
void scope_leave(Scope **cur);

/**
 * Insert a symbol into the current (innermost) scope.
 * Allocates a Symbol struct and copies `name` using `perm`.
 * Returns the created Symbol on success, or NULL on failure (duplicate or OOM).
 */
Symbol *scope_put_symbol(Scope *s, const char *name, SymbolKind kind, TypeInfo *type);

/**
 * Convenience function to insert a variable/type into the current scope (Milestone M3 spec).
 * Returns 0 on success, non-zero on failure.
 */
int scope_put(Scope *s, const char *name, TypeInfo *type);

/**
 * Look up a symbol by name only in the current (innermost) scope.
 * Returns pointer to Symbol if found, NULL otherwise.
 */
Symbol *scope_lookup_current(const Scope *s, const char *name);

/**
 * Look up a symbol by name in the full scope hierarchy (innermost to outermost).
 * Returns pointer to Symbol if found, NULL otherwise.
 */
Symbol *scope_lookup(const Scope *s, const char *name);

/**
 * Compatibility helper to retrieve TypeInfo* by name in the scope hierarchy (Milestone M3 spec).
 * Returns TypeInfo* if found, NULL otherwise.
 */
TypeInfo *scope_get(const Scope *s, const char *name);

#ifdef __cplusplus
}
#endif
