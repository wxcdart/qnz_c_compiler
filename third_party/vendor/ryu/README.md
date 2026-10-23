# Ryū (vendored)

Shortest/correctly-rounded decimal conversion for `double`/`float`, by
Ulf Adams. Vendored from https://github.com/ulfjack/ryu (dual-licensed
Apache-2.0 OR Boost-1.0 — see `LICENSE-Apache2` / `LICENSE-Boost`).

The zero-rt plan noted "MIT"; the actual license is Apache-2.0/Boost
(equally permissive for our use).

Used by zero_rt's `printf` float conversions:
- `d2fixed_buffered(d, precision, buf)` → correctly-rounded `%f`
- `d2exp_buffered(d, precision, buf)` → correctly-rounded `%e` (and the
  digit source for `%g`)
- `d2s_buffered_n(d, buf)` → shortest round-trip (future `zeroc_dtoa`)

Compile the `*.c` files straight into zero_rt (they are plain C99; the
128-bit arithmetic is portable `umul128`, no `__int128` needed).
