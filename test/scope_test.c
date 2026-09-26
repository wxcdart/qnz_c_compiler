#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../src/zero_cc/arena.h"
#include "../src/zero_cc/semantic/type.h"
#include "../src/zero_cc/semantic/scope.h"

int main(void) {
    Arena *perm = arena_create(64 * 1024);
    assert(perm != NULL);

    Scope *cur = NULL;

    /* 1. Enter global scope */
    Scope *global = scope_enter(&cur, perm);
    assert(global != NULL);
    assert(cur == global);
    assert(global->parent == NULL);

    /* 2. Insert symbols into global scope */
    TypeInfo *t_int = type_int(perm);
    TypeInfo *t_float = type_float(perm);
    TypeInfo *t_void = type_void(perm);

    Symbol *s_x = scope_put_symbol(cur, "x", SYMBOL_VAR, t_int);
    assert(s_x != NULL);
    assert(strcmp(s_x->name, "x") == 0);
    assert(s_x->kind == SYMBOL_VAR);
    assert(s_x->type == t_int);

    /* Duplicate in same scope should fail */
    Symbol *s_x_dup = scope_put_symbol(cur, "x", SYMBOL_VAR, t_float);
    assert(s_x_dup == NULL);

    /* Function symbol */
    TypeInfo *t_fn = type_function(perm, t_void, NULL, 0, false);
    Symbol *s_foo = scope_put_symbol(cur, "foo", SYMBOL_FUNC, t_fn);
    assert(s_foo != NULL);
    assert(s_foo->kind == SYMBOL_FUNC);

    /* Typedef symbol */
    Symbol *s_my_int = scope_put_symbol(cur, "my_int", SYMBOL_TYPEDEF, t_int);
    assert(s_my_int != NULL);
    assert(s_my_int->kind == SYMBOL_TYPEDEF);

    /* Tag symbol */
    Symbol *s_tag = scope_put_symbol(cur, "Point", SYMBOL_TAG, NULL);
    assert(s_tag != NULL);
    assert(s_tag->kind == SYMBOL_TAG);

    /* scope_put convenience function */
    assert(scope_put(cur, "y", t_float) == 0);
    assert(scope_put(cur, "y", t_int) != 0);

    /* 3. Lookup in global scope */
    assert(scope_lookup_current(cur, "x") == s_x);
    assert(scope_lookup(cur, "x") == s_x);
    assert(scope_get(cur, "x") == t_int);

    assert(scope_lookup_current(cur, "foo") == s_foo);
    assert(scope_lookup(cur, "foo") == s_foo);

    assert(scope_lookup_current(cur, "my_int") == s_my_int);
    assert(scope_lookup(cur, "my_int") == s_my_int);

    assert(scope_lookup_current(cur, "Point") == s_tag);
    assert(scope_lookup(cur, "Point") == s_tag);

    assert(scope_lookup_current(cur, "y") != NULL);
    assert(scope_get(cur, "y") == t_float);

    assert(scope_lookup(cur, "non_existent") == NULL);
    assert(scope_lookup_current(cur, "non_existent") == NULL);
    assert(scope_get(cur, "non_existent") == NULL);

    /* 4. Enter nested scope */
    Scope *inner = scope_enter(&cur, perm);
    assert(inner != NULL);
    assert(cur == inner);
    assert(inner->parent == global);

    /* Current scope lookup should NOT find global 'x' */
    assert(scope_lookup_current(cur, "x") == NULL);
    /* Hierarchy lookup SHOULD find global 'x' */
    assert(scope_lookup(cur, "x") == s_x);
    assert(scope_get(cur, "x") == t_int);

    /* Shadow 'x' in inner scope */
    Symbol *s_x_inner = scope_put_symbol(cur, "x", SYMBOL_VAR, t_float);
    assert(s_x_inner != NULL);
    assert(scope_lookup_current(cur, "x") == s_x_inner);
    assert(scope_lookup(cur, "x") == s_x_inner);
    assert(scope_get(cur, "x") == t_float);

    /* Add inner-only symbol 'z' */
    Symbol *s_z = scope_put_symbol(cur, "z", SYMBOL_VAR, t_int);
    assert(s_z != NULL);
    assert(scope_lookup_current(cur, "z") == s_z);
    assert(scope_lookup(cur, "z") == s_z);

    /* 5. Enter a deeper level (scope 3) */
    Scope *inner2 = scope_enter(&cur, perm);
    assert(inner2 != NULL);
    assert(cur == inner2);
    assert(inner2->parent == inner);

    assert(scope_lookup_current(cur, "x") == NULL);
    assert(scope_lookup(cur, "x") == s_x_inner);
    assert(scope_lookup(cur, "z") == s_z);
    assert(scope_lookup(cur, "foo") == s_foo);

    /* Leave scope 3 */
    scope_leave(&cur);
    assert(cur == inner);

    /* Leave scope 2 (inner) */
    scope_leave(&cur);
    assert(cur == global);

    /* Back at global: 'x' is global, 'z' does not exist */
    assert(scope_lookup_current(cur, "x") == s_x);
    assert(scope_lookup(cur, "x") == s_x);
    assert(scope_get(cur, "x") == t_int);
    assert(scope_lookup(cur, "z") == NULL);
    assert(scope_lookup_current(cur, "z") == NULL);

    /* Leave global scope */
    scope_leave(&cur);
    assert(cur == NULL);

    arena_destroy(perm);
    printf("scope_test passed\n");
    return 0;
}
