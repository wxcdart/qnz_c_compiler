/* ABI interop s10: gcc-side __int128 provider. */
__int128 gc_i128_add(__int128 a, __int128 b) { return a + b; }
__int128 gc_i128_shift(__int128 v, int k) { return v << k; }
