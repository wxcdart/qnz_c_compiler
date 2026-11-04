/* __int128 / __uint128_t / __int128_t type foundation (Sprint 109 Phase 1).
 *
 * GNU's 128-bit integer types must be a real 16-byte scalar with 16-byte
 * alignment.  Before this fix zerocc silently typed `__int128` as a 4-byte
 * `int`, so `sizeof(__int128)` was 4 (GCC says 16) -- a latent miscompile.
 *
 * This test only exercises the TYPE system (sizeof / _Alignof / layout),
 * which is what Phase 1 delivers.  128-bit arithmetic / codegen / ABI are
 * a follow-up phase; the c-torture int128 tests that need arithmetic remain
 * SKIP-TARGET until that lands.  Run under both zerocc (test-opt) and host
 * gcc (make test): both must print the same 16/16/16/16.
 */
#include <stdio.h>

typedef unsigned __int128 u128;
typedef __int128 s128;

u128 g_u;   /* global, 16-byte object */
s128 g_s;

int main(void)
{
    int ok = 1;
    if (sizeof(u128) != 16)            { printf("u128 size %zu\n", sizeof(u128));    ok = 0; }
    if (sizeof(s128) != 16)            { printf("s128 size %zu\n", sizeof(s128));    ok = 0; }
    if (sizeof(__int128) != 16)        { printf("__int128 size %zu\n", sizeof(__int128));        ok = 0; }
    if (sizeof(__uint128_t) != 16)     { printf("__uint128_t size %zu\n", sizeof(__uint128_t));  ok = 0; }
    if (_Alignof(u128) != 16)          { printf("align %zu\n", _Alignof(u128));     ok = 0; }
    if (sizeof(g_u) != 16)             { printf("global size %zu\n", sizeof(g_u));  ok = 0; }
    printf("%s\n", ok ? "int128 type OK" : "int128 type MISMATCH");
    return ok ? 0 : 1;
}
