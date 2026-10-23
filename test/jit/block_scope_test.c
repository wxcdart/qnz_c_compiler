#include <stdio.h>
#include <assert.h>

/* Block-scope shadowing regression test (C11 6.2.1).
 *
 * Guards the flat latest-wins local symbol table: a name redeclared in a
 * later sibling block (or a later for-init) must not shadow references in
 * an earlier scope. Pre-fix this miscompiled the earlier scope to the newer
 * symbol's stack slot (wrong values) or, with a pointer involved, segfaulted
 * on a byte load from the stale slot.
 */
struct P { int a; float f; int b; };

int f1(int pick) {
    if (pick == 1) { int x = 1; return x; }
    if (pick == 2) { int x = 2; return x; }
    return 0;
}

int vla_sum(int n) {
    int arr[n];
    for (int i = 0; i < n; i++) arr[i] = i * 2;
    int sum = 0;
    for (int i = 0; i < n; i++) sum += arr[i];
    return sum;
}

int main(void) {
    /* 1. sibling blocks, same name, different types */
    {
        { struct P s = { 10, 2.0f, 30 };
          s.a++;
          assert(s.a == 11);
          assert(s.f == 2.0f && s.b == 30);
        }
        { char s[] = "hello";
          char *c = s;
          c++;
          c++;
          assert(*c == 'l' && c - s == 2);
        }
    }

    /* 2. struct member access must use its own block's slot even when a
     *    later block reuses the name for a char array */
    {
        { struct P s = { 10, 2.0f, 30 };
          s.a = s.a + 1;
          assert(s.a == 11);
        }
        { char s[] = "world";
          assert(s[0] == 'w' && s[4] == 'd');
        }
    }

    /* 3. simple int shadowing in sibling blocks */
    {
        assert(f1(1) == 1);
        assert(f1(2) == 2);
    }

    /* 4. for-init loop variable must not be shadowed by a later sibling for
     *    redeclaring the same name (header + body must use the same slot) */
    {
        int total = 0;
        for (int i = 0; i < 3; i++) total += i;
        for (int i = 0; i < 2; i++) total += i * 10;
        assert(total == 13);
    }

    /* 5. for-init loop variable + array, two loops, same name */
    {
        int arr[10];
        int sum = 0;
        for (int i = 0; i < 5; i++) arr[i] = i * 2;
        for (int i = 0; i < 5; i++) sum += arr[i];
        assert(sum == 20);
    }

    /* 6. VLA + for-init with the loop variable redeclared in a second for */
    {
        assert(vla_sum(5) == 20);
        assert(vla_sum(4) == 12);
    }

    /* 7. nested block: the inner reference must see the inner decl
     *    (references before the nested block see the outer value) */
    {
        int outer = 100;
        assert(outer == 100);
        {
            int outer = 5;
            assert(outer == 5);
        }
    }

    printf("Block-scope shadowing test passed\n");
    return 0;
}
