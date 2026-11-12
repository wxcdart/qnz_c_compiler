/* Sprint 164: signed int ++ at INT_MAX must trap on every target ISA, raw/MC
 * path included (the RV64 raw path previously missed it: INT_MAX was loaded as
 * 0xFFFFFFFF7FFFFFFF, so the 32-bit round-trip check saw a fitting result). */
#include <limits.h>
int main(void) {
    volatile int a = INT_MAX;
    a++;
    return 0;
}
