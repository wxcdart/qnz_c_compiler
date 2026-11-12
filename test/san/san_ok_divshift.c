/* zero_san follow-on (Sprint 166): valid division / modulo / shift code
 * must NEVER trap under -fsanitize=undefined:
 *  - non-zero divisors (incl. -1), signed + unsigned, 32/64-bit
 *  - shift counts 0 .. width-1: 31 for int, 63 for long long. Counts
 *    4..30 must not trap - regression: the bound was once the byte width
 *    (4/8) instead of the bit width (32/64).
 *
 * Exit 0 = sound; SIGILL = false-positive. */
static int idiv(int n, int d) { return n / d; }
static int irem(int n, int d) { return n % d; }
static long lldiv(long n, long d) { return n / d; }
static long llrem(long n, long d) { return n % d; }
static int lshl(int n, int c) { return n << c; }
static int lshr(int n, int c) { return n >> c; }
static unsigned ushl(unsigned n, unsigned c) { return n << c; }
static long lllshl(long n, long c) { return n << c; }
static long lllshr(long n, long c) { return n >> c; }
static unsigned long ullshl(unsigned long n, unsigned long c) { return n << c; }
static unsigned udiv(unsigned n, unsigned d) { return n / d; }

int main(void) {
    if (idiv(42, 1) != 42) return 1;
    if (idiv(-42, -1) != 42) return 2;
    if (idiv(42, -3) != -14) return 3;
    if (irem(42, 5) != 2) return 4;
    if (irem(-42, 5) != -2) return 5;
    if (lldiv(4000000000L, 2000000000L) != 2L) return 6;
    if (lldiv(-4000000000L, -2000000000L) != 2L) return 7;
    if (llrem(-4000000000L, 3L) != -1L) return 8;
    if (udiv(100u, 7u) != 14u) return 9;
    /* Signed int shifts: counts 0..30 with representable results.
     * Counts 4..30 in particular must not trap. */
    if (lshl(1, 0) != 1) return 10;
    if (lshl(1, 4) != 16) return 11;
    if (lshl(1, 16) != 65536) return 12;
    if (lshl(1, 30) != 0x40000000) return 13;
    if (lshr(0x40000000, 30) != 1) return 14;
    if (lshr(-8, 1) != -4) return 15;
    if (lshr(-8, 30) != -1) return 16;
    /* Max valid count (31) via unsigned - signed 1<<31 would overflow. */
    if (ushl(1u, 31) != 0x80000000u) return 17;
    if (ushl(1u, 0) != 1u) return 18;
    /* 64-bit: counts 0..63 valid. */
    if (lllshl(1L, 0) != 1L) return 19;
    if (lllshl(1L, 40) != (1L << 40)) return 20;
    if (lllshl(1L, 62) != (1L << 62)) return 21;
    if (lllshr(1L << 62, 62) != 1L) return 22;
    if (ullshl(1UL, 63) != (1UL << 63)) return 23;
    return 0;
}
