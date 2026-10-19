#include "scope.h"
#include <string.h>
#include <stdlib.h>

#define SCOPE_INITIAL_BUCKETS 64

/* 64-bit FNV-1a hash function for strings */
static uint64_t hash_str(const char *str) {
    uint64_t hash = 14695981039346656037ULL;
    if (!str) return hash;
    while (*str) {
        hash ^= (uint8_t)(*str++);
        hash *= 1099511628211ULL;
    }
    return hash;
}

Scope *scope_enter(Scope **cur, Arena *perm) {
    if (!perm) return NULL;
    Scope *s = (Scope *)arena_alloc(perm, sizeof(Scope));
    if (!s) return NULL;

    s->parent = cur ? *cur : NULL;
    s->perm_arena = perm;
    s->count = 0;
    s->bucket_count = SCOPE_INITIAL_BUCKETS;
    s->buckets = (Symbol **)calloc(s->bucket_count, sizeof(Symbol *));
    if (!s->buckets) return NULL;

    if (cur) {
        *cur = s;
    }
    return s;
}

void scope_leave(Scope **cur) {
    if (!cur || !*cur) return;
    Scope *s = *cur;
    *cur = s->parent;
    if (s->buckets) {
        free(s->buckets);
        s->buckets = NULL;
    }
    s->bucket_count = 0;
    s->count = 0;
}

static void scope_grow(Scope *s) {
    size_t new_cap = s->bucket_count * 2;
    if (new_cap < SCOPE_INITIAL_BUCKETS) new_cap = SCOPE_INITIAL_BUCKETS;
    Symbol **new_buckets = (Symbol **)calloc(new_cap, sizeof(Symbol *));
    if (!new_buckets) return; /* OOM: keep old table working */

    for (size_t i = 0; i < s->bucket_count; i++) {
        Symbol *curr = s->buckets[i];
        while (curr) {
            Symbol *next = curr->next_in_bucket;
            size_t idx = (size_t)(hash_str(curr->name) & (new_cap - 1));
            curr->next_in_bucket = new_buckets[idx];
            new_buckets[idx] = curr;
            curr = next;
        }
    }

    free(s->buckets);
    s->buckets = new_buckets;
    s->bucket_count = new_cap;
}

Symbol *scope_put_symbol(Scope *s, const char *name, SymbolKind kind, TypeInfo *type) {
    if (!s || !name || !s->perm_arena || !s->buckets) return NULL;

    /* Check for duplicate in the current scope */
    if (scope_lookup_current(s, name) != NULL) {
        return NULL;
    }

    /* Grow if load factor exceeds 75% */
    if (s->count >= (s->bucket_count * 3) / 4) {
        scope_grow(s);
    }

    /* Allocate Symbol in the permanent arena */
    Symbol *sym = (Symbol *)arena_alloc(s->perm_arena, sizeof(Symbol));
    if (!sym) return NULL;

    /* Duplicate name string into permanent arena */
    size_t name_len = strlen(name);
    char *name_copy = (char *)arena_alloc(s->perm_arena, name_len + 1);
    if (!name_copy) return NULL;
    memcpy(name_copy, name, name_len + 1);

    sym->name = name_copy;
    sym->kind = kind;
    sym->type = type;
    sym->flags = 0;
    sym->node = NULL;

    size_t idx = (size_t)(hash_str(name) & (s->bucket_count - 1));
    sym->next_in_bucket = s->buckets[idx];
    s->buckets[idx] = sym;

    s->count++;
    return sym;
}

int scope_put(Scope *s, const char *name, TypeInfo *type) {
    Symbol *sym = scope_put_symbol(s, name, SYMBOL_VAR, type);
    return sym ? 0 : -1;
}

Symbol *scope_lookup_current(const Scope *s, const char *name) {
    if (!s || !name || !s->buckets || s->bucket_count == 0) return NULL;

    size_t idx = (size_t)(hash_str(name) & (s->bucket_count - 1));
    for (Symbol *curr = s->buckets[idx]; curr != NULL; curr = curr->next_in_bucket) {
        if (strcmp(curr->name, name) == 0) {
            return curr;
        }
    }
    return NULL;
}

Symbol *scope_lookup(const Scope *s, const char *name) {
    for (const Scope *curr = s; curr != NULL; curr = curr->parent) {
        Symbol *sym = scope_lookup_current(curr, name);
        if (sym != NULL) {
            return sym;
        }
    }
    return NULL;
}

TypeInfo *scope_get(const Scope *s, const char *name) {
    Symbol *sym = scope_lookup(s, name);
    return sym ? sym->type : NULL;
}
