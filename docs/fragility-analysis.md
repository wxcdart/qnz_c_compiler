# ZeroCC Fragility Analysis

Date: 2026-07-09 · Method: static pattern scan + boundary bisecting with 20+ generated probes, each cross-checked against host `gcc` (JIT `--run` and AOT `-o` modes).

Legend: **CRIT** = silently wrong code or compiler crash on valid C · **HIGH** = compiler crash/hang on valid C · **MED** = silent limit with no diagnostic · **LOW** = documented constraint.

---

## CRITICAL

### C1. 32-bit `int` overflow does not wrap (64-bit residue leaks into `int` values)

`int a=2000000000; int b=a+a; return b==-294967296?0:1;` → gcc 0, zerocc 1.

Integer arithmetic is performed in 64-bit registers and 32-bit `int` results are never truncated. Reloading a local `int` from its stack slot also lacks sign-extension (unsigned reloads zero-extend, which is why the unsigned variant passes). Every `int` computation whose true value exceeds the 31-bit range is silently wrong: comparisons, shifts, stores, return values.

- Probe: `p12a` (fails), `p12b` (unsigned, passes), `p12c` (in-range, passes).
- Fix direction: truncate to 32 bits when storing/reloading 32-bit signed int locals (sign-extend on load), or emit 32-bit ALU ops for `int`-typed values.

### C2. Temp-id exhaustion at 255 → hang or crash

`Instr` temp fields are `uint8_t` with `0xFF` = "no temp". `next_temp_id` has **no bound check**; a function with >255 temps wraps into the sentinel.

- Bisect: 116 nested calls ≈ 254 temps → OK; 117 (r255 reached, verified in `-dump-ir`) → JIT **hangs** (rc=124), larger depths segfault (rc=139).
- LRA temp *arrays* are growable (`live_range.h`), but the IR field width is not.
- Fix direction: explicit bound check with a diagnostic when `next_temp_id >= 0xFF`; long-term, spill/renumber.

### C3. Switch with >128 cases silently drops the rest

`ast_lowering_stmt.c:493`: `while (s != AST_NULL && n_cases < 128)` — cases 129+ never get labels, no diagnostic.

- 300-case switch: JIT **hangs** on out-of-range value; AOT binary **segfaults** (rc=139). gcc: both fine.
- Fix direction: emit all cases (dispatch is O(n) CMP/JNE already) or a diagnostic.

### C4. ~231 locals in one function → compiler hang/crash

Bisect: 230 `int` locals → OK; 232 → **hang** (rc=124), at 400+ also segfault (rc=139, ASLR-dependent). Correlates with **symbol count**, not frame size (250 `char` locals hang too). `ZERO_CC_MAX_LOCALS` is 8192, so a hidden smaller structure saturates first (scope lookup table or O(n²) pass suspected). No diagnostic. Root cause not yet pinned.

### C5. Member chains through pointer members fail to resolve

`b.p->v` (local struct) and `b->p->v` (param) both → `error: member reference base is not a structure or union`, even for the minimal 2-segment chain. Multi-level `->`/`.` chains (lists, trees) are unusable; single-segment access works.

---

## HIGH

### H1. ~115 nested expression depth → compiler segfault

110 nested parens OK, 120 → **compiler crash** (rc=139) in both JIT and AOT builds. Recursive-descent parser stack overflow (large per-level stack frames). gcc handles 120 trivially.

### H2. Diagnostics do not stop compilation

Errors such as `undefined local variable`, `failed to insert symbol`, `use of undeclared identifier` are printed, but compilation **continues and produces a binary** (often with wrong semantics, rc=0). Broken builds pass silently.

---

## MEDIUM (silent limits, no diagnostics)

| # | Limit | Behavior past limit | Probe |
|---|-------|--------------------|-------|
| M1 | identifiers ≤ **127** chars | name truncated → **collision**, silently wrong code (error diagnostics fire on the split token) | `id.c`: 127 OK, 128 fails |
| M2 | globals ≤ **256** | 257th silently dropped (`add_global_var` returns -1, ignored) | `p3` 260 globals "passes" (only checked first 13) |
| M3 | struct members ≤ **64** | 65th silently dropped (bounds-checked array, no diagnostic) | `p4` |
| M4 | IR dump shows `imm` (int16) not `imm64` | large immediates displayed truncated (e.g. `2000000000` shown as `-27648`); **emission is correct** (uses `imm64`) — misleading for debugging only | `p11` IR dump |

---

## LOW (documented / accepted)

- **WSL2 GPF**: any 128-bit SIMD read from a `PROT_READ|PROT_EXEC` page faults on kernel 6.18.54 WSL2 (see `docs/okf/scalar-float-x86.md`). 32/64-bit reads fine.
- **`-S` path** lacks `.data` section emission (AOT `-o` fine).
- Computed-size VLA, flexible-array `offsetof` (C99 audit leftovers).
- `>>` lowers to logical shift (implementation-defined for signed).

---

## Robustness positives (verified during this analysis)

- Global integer constants up to `INT_MAX` are exact (JIT + AOT).
- Struct-member width narrowing (1/2/4-byte) works for local and global bases.
- `members[64]` is bounds-checked — no OOB write from oversized structs.
- Long string literals (600B), 5000-element VLA, 1000-deep runtime recursion, `long long` INT64 boundaries, char/short sign boundaries — all correct.
- Reloc/label tables have generous caps (32K); LRA arrays grow dynamically.
- `make test` 140/140 and the C99 probe battery (30/30) still pass.

## Suggested fix order

1. **C1** int32 wrap/extension — largest semantic gap, affects real-world code (hashes, coordinates, bit math).
2. **C2** temp-id bound check + diagnostic (crash → error).
3. **C3** switch case cap (remove or diagnose).
4. **C4** root-cause the ~231-local hang.
5. **C5** multi-segment member chain type resolution.
6. **H2** make diagnostics fatal by default.
7. **M1** error (not truncate) on identifiers >127.
8. **H1** raise parser stack or iterate the expression recursion.
