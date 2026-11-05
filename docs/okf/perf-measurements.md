---
type: Concept
title: Performance Measurements & the JIT-vs-GCC Claim
description: Measured per-pass optimizer costs (--profile-opts) before/after the Sprint 35 CFG migration, the re-measured JIT-vs-GCC compile-time comparison (the old "~78x faster than GCC" figure is retracted), the dominant mem2reg cost analysis, and the 16-bit temp-id OOB bugs the measurement exposed.
resource: concepts/perf-measurements
tags: [performance, profiling, jit, gcc, benchmark, opt-profile, mem2reg, temp-id]
timestamp: 2026-11-05T02:21:07Z
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
decisions unchanged).

**New dominant pass: GVN** (~184-210 ms @-O2/-O3; per-block hash-table
re-init churn) — tracked as the next optimization target. —
**RESOLVED in Sprint 38**, with a *corrected* root cause: the churn was not in
the value-numbering loop (0.5 ms) but in `opt_restrict_memory_forward`'s
block-boundary `mf_reset()`, which rewrote the entire 65536-entry
canonical-temp table at **every** label/jump/call (447× per large function,
80-180 µs per rewrite → ~60 ms per function). Fixed with a
generation-stamped table (`CanonSlot {uint32_t gen; uint16_t val}`, reset =
generation bump, O(1)). Measured (median of 5, `synth_large.c`): gvn
184-210→**2.3 ms** @-O2/-O3 (~80×); total passes-only @-O2 ~275→~134 ms.
`-dump-ir` byte-identical at `-O2` and `-O3` pre/post. **New dominant pass:
LICM** (~103 ms @-O2) — tracked as the next optimization target if/when it
matters.

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

## 4. Algorithmic Scaling Fixes Exposed by `c-torture` Stress Tests (Sprints 58–89)

Burning down `TIMEOUT`s and `REJECT`s across the 3,940-test GCC `c-torture` corpus ([c-torture-gate.md](./c-torture-gate.md)) exposed five quadratic $O(N^2)$ bottlenecks in symbol lookup, IR optimization, and register allocation:

1. **Enum Constant Open-Addressing Hash Table (Sprint 58)**:
   - `compile/limits-enumconst.c` (100,000 enum constants) hung in linear `strcmp` scans during enum registration and lookup. Replaced with an open-addressing hash table for $O(1)$ enum constant resolution.
2. **Global Variable FNV-1a Hash Table (Sprint 65, [`src/zero_cc/types/global_vars.h`](../../src/zero_cc/types/global_vars.h))**:
   - Replaced the $O(N)$ `find_global_var` linear scan over `c->globals[]` with an FNV-1a open-addressing hash table once hundreds of file-scope globals were unblocked in `c-torture`.
3. **Function Symbol Open-Addressing Hash Table (Sprint 67, [`src/zero_cc/types/symbol_mgmt.h`](../../src/zero_cc/types/symbol_mgmt.h))**:
   - `compile/limits-externdecl.c` (100,000 `extern int x();` declarations) triggered $\approx 5 \times 10^9$ `strcmp` calls in `zero_cc_find_function` (8.6 s `TIMEOUT`). Added an incremental open-addressing hash with same-name chains (preserving definition-over-prototype preference), reducing parse time to **0.31 s**.
4. **Optimizer Superlinear Pass Guards on Giant Functions (Sprint 89, [`src/zero_cc/opt/opt_pass.c`](../../src/zero_cc/opt/opt_pass.c))**:
   - `execute/20090113-2.c` and `execute/20090113-3.c` timed out in multi-pass dataflow analyses (`cse`, `gvn`, `licm`, `mem2reg`) on giant generated functions. Added `OPT_MAX_BASIC_BLOCKS` (`512`) and `OPT_MAX_INSTRS` (`8192`) thresholds in `opt_run_passes` to bypass superlinear whole-CFG passes on massive functions while keeping linear local passes active.
5. **`RegAllocator` $O(1)$ `temp_to_interval[65536]` & Statement-Local Temp Reuse (Sprint 89, [`src/zero_cc/ir/regalloc.h`](../../src/zero_cc/ir/regalloc.h), [`src/zero_cc/ir/ast_lowering_stmt.c`](../../src/zero_cc/ir/ast_lowering_stmt.c))**:
   - `compile/limits-caselabels.c` and `compile/20001226-1.c` (8,192 sequential `if (a > b) goto gt;` statements, originally allocating 188,421 temporaries and exceeding the 16-bit `65,534` temp-id cap) exposed two issues:
     - Statement-local expression temporaries in `collect_switch_cases` and `lower_stmt_if` (outside loops once `next_temp_id > 1024`) are dead at the statement boundary and now recycle `next_temp_id`, keeping peak temp IDs around `~1,050`.
     - `regalloc_get_reg_for_temp`, `regalloc_get_spill_slot_for_temp`, `regalloc_get_class_for_temp`, and `regalloc_reg_conflicts_fixed` previously scanned `ra->intervals[0..interval_count]` linearly on every instruction operand ($1.8 \times 10^5 \text{ instrs} \times 3 \times 3.2 \times 10^4 \approx 1.7 \times 10^{10}$ iterations, causing a >10 s `TIMEOUT` in `emit_ir_to_machine_code`). Adding `temp_to_interval[65536]` and `fixed_interval_indices[256]` to `RegAllocator` made all temp-to-interval lookups **$O(1)$**, reducing `compile/20001226-1.c` compile time from **>10 s (`TIMEOUT`) to <0.4 s**.

## 5. Reproducing

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
