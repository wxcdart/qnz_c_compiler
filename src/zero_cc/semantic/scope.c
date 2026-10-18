#include "scope.h"
#include <string.h>

#define SCOPE_TPHT_BLOCK_COUNT 256

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

    if (zero_flatten_tpht_init(&s->tpht, SCOPE_TPHT_BLOCK_COUNT) != 0) {
        return NULL;
    }

    if (cur) {
        *cur = s;
    }
    return s;
}

void scope_leave(Scope **cur) {
    if (!cur || !*cur) return;
    Scope *s = *cur;
    *cur = s->parent;
    zero_flatten_tpht_destroy(&s->tpht);
}

Symbol *scope_put_symbol(Scope *s, const char *name, SymbolKind kind, TypeInfo *type) {
    if (!s || !name || !s->perm_arena) return NULL;

    /* Check for duplicate in the current scope */
    if (scope_lookup_current(s, name) != NULL) {
        return NULL;
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

    /* Get arena index thin-pointer for Symbol */
    size_t idx = arena_index_of(s->perm_arena, sym);
    uint64_t key = hash_str(name);

    if (zero_flatten_tpht_put(&s->tpht, key, (uint32_t)idx) != 0) {
        return NULL;
    }

    s->count++;
    return sym;
}

int scope_put(Scope *s, const char *name, TypeInfo *type) {
    Symbol *sym = scope_put_symbol(s, name, SYMBOL_VAR, type);
    return sym ? 0 : -1;
}

Symbol *scope_lookup_current(const Scope *s, const char *name) {
    if (!s || !name || !s->perm_arena) return NULL;

    uint64_t key = hash_str(name);
    uint32_t val = 0;
    if (zero_flatten_tpht_get(&s->tpht, key, &val) != 0) {
        return NULL;
    }

    Symbol *sym = (Symbol *)arena_index_to_ptr(s->perm_arena, (size_t)val);
    if (!sym || !sym->name) return NULL;

    /* Verify key collision */
    if (strcmp(sym->name, name) != 0) {
        return NULL;
    }

    return sym;
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
