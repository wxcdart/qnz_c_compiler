---
type: Concept
title: Performance Measurements & the JIT-vs-GCC Claim
description: Measured per-pass optimizer costs (--profile-opts) before/after the Sprint 35 CFG migration, the re-measured JIT-vs-GCC compile-time comparison (the old "~78x faster than GCC" figure is retracted), the dominant mem2reg cost analysis, and the 16-bit temp-id OOB bugs the measurement exposed.
resource: concepts/perf-measurements
tags: [performance, profiling, jit, gcc, benchmark, opt-profile, mem2reg, temp-id]
timestamp: 2026-10-20T01:47:55Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Performance Measurements & the JIT-vs-GCC Claim (Sprint 35 Phase 4)

All numbers on this page were measured on the project host (x86-64 Linux, gcc 13.3.0
available as reference) with **median of 5 runs** unless noted. The workload is
`test/bench/synth_large.c` (861 lines, ~40 functions, ~122 loops, IR well past the
1024-instruction mark — deliberately larger than the regression-suite corpus).

## 1. The "~78x faster than GCC" claim is RETRACTED

Earlier docs asserted ZeroCC's JIT compiles in "~2.8 ms (78x faster than GCC)".
Re-measured, compile + run of the same file, median of 5:

| Command | Median |
|---|---|
| `zerocc --run -O0 test/bench/synth_large.c` (JIT: parse → lower → JIT emit → mmap → exec) | **234 ms** |
| `zerocc --run -O2 test/bench/synth_large.c` | 3933 ms |
| `zerocc -O0 test/bench/synth_large.c -o bin && ./bin` (AOT: asm → `as` → `ld` → run) | 326 ms |
| `zerocc -O2 ... -o bin && ./bin` | 4124 ms |
| `gcc -O0 test/bench/synth_large.c -o bin && ./bin` | **164 ms** |
| `gcc -O2 test/bench/synth_large.c -o bin && ./bin` | **365 ms** |

**ZeroCC's JIT is slower than GCC** for this workload (~0.7x of `gcc -O0`, ~0.4x of
`gcc -O2`). The 78x figure was never backed by recorded methodology; no docs page
may assert it again. Where a speed claim is needed, cite this page.

## 2. Per-pass optimizer cost (`--profile-opts`)

`--profile-opts` now reports a **Per-pass wall time** section (ms, accumulated over
all functions of the TU). Both the JIT (`--run`) and AOT (`-o`) paths enable the
profiler (the AOT path previously only *dumped* without *enabling* it — fixed in
Phase 4).

`zerocc --profile-opts -O2 test/bench/synth_large.c -o /tmp/x`, passes-only totals:

| Pass | ms |
|---|---|
| constant_fold | 0.33 |
| flag_dce | 0.12 |
| dce | 1.44 |
| copy_prop | 0.89 |
| **mem2reg** | **3374.0** |
| cse | 1.22 |
| gvn | 222.7 |
| licm | 87.2 |
| **TOTAL (passes only)** | **3687.9** |

At `-O1` (mem2reg is the only pass that does loop promotion): mem2reg **1088 ms**,
total passes 1089 ms. At `-O3`: mem2reg 4536 ms, gvn 293 ms, licm 121 ms,
auto_vectorize 9 ms, total 4964 ms.

### Before/after the Phase 2/3 CFG migration

The pre-migration baseline (`87284bd` + the same measurement patch) on the same
workload, wall time compile+run:

| Level | Baseline (87284bd) | HEAD (CFG-based passes) |
|---|---|---|
| `-O0` | 399 ms | 286 ms |
| `-O1` | 1470 ms | 1071 ms |
| `-O2` | 3949 ms | 3665 ms |
| `-O3` | 3813 ms | 3401 ms |

**Verdict: the CFG migration introduced no measurable regression**; at `-O2/-O3`
HEAD is 7-10% faster because the CFG's pred/succ edges replace the legacy
per-loop full-function jump pre-scans (entry-cleanness + first-exit in mem2reg,
`label_to_idx` in LICM).

### Where the time actually goes

~~**mem2reg's loop promotion is ~85% of all optimizer time** ... the obvious
improvement — collect the set of slots actually used by the function first,
and/or do one combined scan per loop updating state for all slots at once —
is deferred.~~ — **RESOLVED in Sprint 36**: `opt_promote_loop_carried` now does
ONE scan per loop that accumulates per-slot stats for all slots (plus the
address-taken pre-audit records `max_slot_used` to bound the candidate
iteration). Measured (median of 5, `synth_large.c`): mem2reg 1088→3.2 ms @-O1,
3374→13.8 ms @-O2, 4536→15.9 ms @-O3; total passes-only @-O2 3688→~275 ms
(~13×). The `-O1` `-dump-ir` output is byte-identical pre/post (promotion
decisions unchanged). **New dominant pass: GVN** (~184-210 ms @-O2/-O3;
per-block hash-table re-init churn) — tracked as the next optimization target.

**Measurement methodology**: wall times on the project host swing ±50% under
variable load (4 cores, load avg up to ~1.3); the per-pass profile
(in-process `CLOCK_MONOTONIC`, unaffected by page-cache/linker variance) is
the stable metric — report medians of ≥5 runs.

## 3. What the measurement exposed: 16-bit temp-id OOB bugs

The large workload (functions with >256 temps; temp ids start at
`VREG_USER_START` = 16 and grow without bound) crashed/ corrupted three passes
that all silently assumed temp ids fit in 8 bits. Fixed in Phase 4:

1. **`opt_gvn.c` (the crash)** — `g_canon[256]` indexed by `temp_dest`
   (uint16_t) → global-buffer-overflow write on any function with >256 temps,
   silently corrupting adjacent BSS (observed corrupting the optimizer-profile
   global). Also `MemEntry.addr/val`, `g_slot_can`, `ValueEntry.src1/2`,
   `mf_forget_temp()`, `replace_with_gvn_copy()` all narrowed to `uint8_t`.
   Fix: all temp storage widened to 16 bits, `g_canon[65536]`.
2. **`opt_vectorize.c`** — `scalar_to_vec_map[256]` (stack array) indexed by
   `temp_dest`/`temp_src1` without a bound check (one read site was guarded, two
   writes and two reads were not). Fix: full-range `static int16_t[65536]`
   (memset per transform).
3. **`opt_loop.c` (LICM)** — `loop->is_invariant` was `calloc(MAX_LOOP_BODY)`
   (1024) but indexed by *instruction index*; any IR > 1024 instructions went OOB
   (segfault at `-O2`). Fix: sized by IR count, `body_indices` grown dynamically.

Lesson recorded: **any table keyed by temp id must be sized for the full
16-bit temp-id space** (or bounds-checked), same for instruction-index tables
which must be sized by `ir_get_count()`, never by a loop-body cap.

## 4. Reproducing

```bash
# per-pass profile (AOT; the JIT path also honors --profile-opts)
./build/bin/zerocc --profile-opts -O2 test/bench/synth_large.c -o /tmp/synth

# JIT-vs-GCC compile-time matrix (median of 5, compile+run)
for i in 1 2 3 4 5; do t0=$(date +%s%N); ./build/bin/zerocc --run -O0 test/bench/synth_large.c >/dev/null; t1=$(date +%s%N); echo $(( (t1-t0)/1000000 )); done | sort -n | sed -n 3p
```

## Related

- [Optimization Passes](./optimization-passes.md) — pass pipeline & safety invariants.
- [IR & Register Allocation](./ir-and-regalloc.md) — temp-id space (`VREG_USER_START`).
- [Build & Verification](./build-and-verification.md) — `make test-opt` sweep.
