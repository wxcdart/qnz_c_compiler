/* zero_san v1 (Sprint 161): compound assignment += -= *= that do NOT
 * overflow must not trap. Exercises the integer compound-assignment paths
 * (local, global, member, pointer-deref, array-element) that now carry
 * OP_OVF_TRAP checks. Must exit 0 with AND without -fsanitize=undefined. */
#include <stdint.h>

int g = 10;
struct S { int f; long h; };
static struct S s;

int main(void) {
    int x = 10;
    long y = 20L;
    x += 5; x -= 3; x *= 2;             /* 15 -> 12 -> 24 */
    y += 100L; y -= 50L; y *= 3L;       /* 120 -> 70 -> 210 */
    g += 1;                             /* 11 */
    s.f += 2; s.h += 4L;
    int a[4] = {1, 2, 3, 4};
    int *p = a; *p += 10;               /* a[0] = 11 */
    uint32_t u32 = 7u; u32 += 3u;       /* unsigned, no trap */
    if (x != 24) return 1;
    if (y != 210L) return 1;
    if (g != 11) return 1;
    if (s.f != 2 || s.h != 4L) return 1;
    if (*p != 11) return 1;
    if (u32 != 10u) return 1;
    return 0;
}
