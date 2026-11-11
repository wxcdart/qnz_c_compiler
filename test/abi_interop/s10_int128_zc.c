/* ABI interop s10: __int128 (two-unit GPR128) args & returns. */
__int128 zc_i128_mul(__int128 a, __int128 b) { return a * b; }
long long zc_i128_hi(__int128 v) { return (long long)(v >> 64); }
unsigned long long zc_i128_lo(__int128 v) { return (unsigned long long)v; }
