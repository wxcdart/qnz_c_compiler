/* zero_san v1 (Sprint 161): ++/-- that do NOT overflow must not trap.
 * Exercises the integer inc/dec lowering paths (local, global, member,
 * array-element, pointer, unsigned-wrap) that now carry OP_OVF_TRAP checks.
 * Must exit 0 with AND without -fsanitize=undefined (behavior preservation). */
#include <stdint.h>

int g_int = 5;
long g_long = 5L;
struct S { int f; long g; };
static struct S s;
int arr[4];

int main(void) {
    int x = 10;
    long y = 20L;
    x++; x--; y++; y--;
    g_int++; g_long++;
    s.f++; s.g++;
    arr[0]++;
    int *p = &x; p++; p--;
    unsigned int uu = 0; uu--;          /* wraps to UINT_MAX: defined, no trap */
    uint32_t u32 = 0; u32++;            /* unsigned, no trap */
    if (x != 10 || y != 20L) return 1;
    if (g_int != 6 || g_long != 6L) return 1;
    if (s.f != 1 || s.g != 1L) return 1;
    if (arr[0] != 1) return 1;
    if (p != &x) return 1;
    if (uu != 0xFFFFFFFFu) return 1;
    if (u32 != 1u) return 1;
    return 0;
}
