/* Sprint 164: 32-bit immediate materialization edges. On RV64, LUI sign-extends
 * bit 31, so LUI+ADDI is wrong for 0x7FFFF800..0x7FFFFFFF (hi == 0x80000);
 * the emitter must use ADDIW. INT_MAX previously loaded as 0xFFFFFFFF7FFFFFFF. */
#include <stdio.h>
static int id(int x) { return x; }
int main(void) {
  volatile int a = 2147483647, b = 0x7ffff800, c = 0x7ffffc00, d = -2147483647 - 1;
  volatile int e = 0x7ffff7ff, f = 2047, g = 2048, h = -2048, i = -2049;
  long wa = a, wb = b, wc = c, wd = d, we = e;
  printf("%lx %lx %lx %lx %lx\n", wa, wb, wc, wd, we);
  printf("%d %d %d %d\n", f, g, h, i);
  printf("%d %d\n", id(2147483647), id(0x7ffffffe));
  printf("%d\n", (int)((unsigned)id(2147483647) + 1u == 0x80000000u));
  return 0;
}
