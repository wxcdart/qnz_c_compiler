---
name: c-torture-gate
description: >-
  Run and maintain the GCC c-torture differential conformance gate (tools/torture_run.py).
  Use when burning down c-torture REJECT/SKIP buckets, unskipping a test category,
  ratcheting test/torture/baseline.json, classifying a test, or diagnosing a gate
  "regression" after a compiler change.
---

# c-torture Conformance Gate

The gate (`tools/torture_run.py`) differentially tests ZeroCC against the GCC
reference over the vendored corpus in `third_party/vendor/c-torture/`. A run
**regresses** if it loses a class, drops an error count, or makes a
previously-rejected file compile silently. `MISCOMPILE`, segfaults, and
`PASS → bad` transitions are always blockers; an honest clean `REJECT` is not.

## Running the Gate

```bash
python3 tools/torture_run.py --parallel 4              # gate on baseline
python3 tools/torture_run.py --parallel 4 --update-baseline   # re-record (only when green)
python3 tools/torture_run.py --timeout 30 --parallel 4        # override per-test timeout
```

Baseline: `test/torture/baseline.json` (`tests` map + `totals`). Commit the
baseline **together with** the source/harness change that caused the movement.

## Sibling Gates (same ratchet discipline)

Three other differential baselines ratchet the same way — **re-record only
when green and idle, commit the baseline with its cause**:

```bash
python3 tools/diag_battery.py                 # compiler-diagnostics gate (test/diag/baseline.json)
python3 tools/diag_battery.py --update-baseline
python3 tools/sret_verifier.py --verify-all   # sret ABI verifier (no baseline file)
make test-isa-qemu                            # raw-emitter differential vs static-musl ref under QEMU
python3 tools/isa_qemu_diff.py --isa riscv64  # one ISA
```

- **diag_battery** differentially classifies zerocc's diagnostics for
  deliberately-broken programs (`test/diag/*.c`) against gcc; a run regresses
  if it loses a category, drops an error count, or makes a rejected file
  compile silently. Run after any parser/semantic/diagnostic change.
- **test-isa-qemu** needs `qemu-user-static` + per-ISA static-musl reference
  wrappers (`musl-gcc-<arch>`, built by `tools/setup_musl_cross.sh`); the
  harness preflights and prints the setup command if missing. The raw path is
  `ZC_RAW_AOT=1`.
- The phantom-baseline diagnosis (per-test diff, idle re-run) applies to the
  diag battery the same way it applies to c-torture.

## Class Taxonomy (what is a "bad" result)

| Class | Meaning | Bad? |
|---|---|---|
| `PASS` | ZeroCC output matches GCC reference | — |
| `REJECT` | GCC accepts, ZeroCC rejects (core-C gap) | counted as bad |
| `MISCOMPILE` | compiles but wrong result | always a blocker |
| `TIMEOUT` | exceeded per-test timeout (compile or run) | tracked, not counted bad |
| `NO-REF` | GCC reference missing/unbuildable | tracked |
| `SKIP-C89` | GNU extension rejected by `gcc -std=c99 -pedantic-errors` | out of scope |
| `SKIP-TARGET:<feature>` | requires an unimplemented ZeroCC feature | tracked per feature |
| `SKIP-REFSEM` / `SKIP-UB` / `SKIP-BOGUS` / `SKIP-PTR` / `SKIP-TARGET:arm` | intentional skips | out of scope |
| `EXPECTED-REJECT` | ZeroCC correctly rejects invalid C that GCC (mis)accepts | not bad |
| `REF-BUG` | GCC reference binary itself crashes | not a ZeroCC failure |

**Reference rule:** `gcc -std=c99/-c11 -pedantic-errors` is the reference for
deciding `SKIP-C89`. If GCC rejects the file pedantically, it is out of scope —
do not "fix" it in ZeroCC.

## Baseline Ratchet Discipline

1. **Re-record only when green** — no regressions in a full gate run first.
2. **Re-record only on an idle machine.** High load bakes phantom
   `TIMEOUT` and `REJECT` classes into the baseline (observed: a load-affected
   baseline recorded `TIMEOUT 73` / `REJECT 15` where the true clean run was
   `TIMEOUT 16` / `REJECT 13`).
3. After any ratchet, verify the new totals by class and confirm the delta is
   fully accounted for.

## Diagnosing a "Regression" (before blaming your change)

A run reporting `N bad (baseline M)` is often a **baseline phantom**, not a
real regression. Procedure:

1. The gate prints per-test lines **only for `PASS → bad` transitions**. If
   it reports `N bad (baseline M)` with no lines listed, the growth came from
   a test that was previously in a *non-bad* class (often a load-phantom
   `NO-REF`/`TIMEOUT` now classifying as `REJECT`). Every full run now dumps
   its per-test results to `test/torture/last_run.json` (same shape as the
   baseline), so diff them directly:
   ```bash
   python3 - <<'PY'
   import json
   last = json.load(open('test/torture/last_run.json'))['tests']
   base = json.load(open('test/torture/baseline.json'))['tests']
   for p in sorted(set(last) | set(base)):
       a = base.get(p, {}).get('class', 'NEW')
       b = last.get(p, {}).get('class', 'GONE')
       if a != b:
           print(f"{p}: {a} -> {b}")
   PY
   ```
   (A partial run's `last_run.json` is a subset — only compare tests present
   in both.)
2. For each changed test: check the **baseline class**. A baseline `NO-REF`
   or `TIMEOUT` that is now `REJECT`/`PASS` is usually a load phantom.
3. Verify the new class is correct: compile the test with the GCC reference
   and run ZeroCC on it directly (JIT and AOT):
   ```bash
   gcc -std=c11 -c third_party/vendor/c-torture/<path> -o /tmp/ref.o
   ./build/bin/zerocc third_party/vendor/c-torture/<path> -o /tmp/t && /tmp/t
   ./build/bin/zerocc --run third_party/vendor/c-torture/<path>
   ```
4. Re-run the gate on an idle machine. If the "regression" disappears, it was
   a phantom; re-record the baseline (idle) to correct it.
5. Only if the test is genuinely worse after the change do you have a real
   regression — bisect/revert.

Known genuine hard-limit `REJECT`s (do not chase): `compile/20001226-1.c`
(16-bit temp-id cap), `compile/limits-*.c` (language limits),
`compile/pr110386-2.c` (missing `immintrin.h`), `execute/va-arg-pack-1.c` +
`compile/pr37669.c` (intentional `__builtin_va_arg_pack` rejects),
`execute/990413-2.c` (intentional non-empty GNU asm reject).

## Unskip Protocol (removing a `SKIP-TARGET:<feature>`)

1. Prove the feature actually works with minimal standalone tests (JIT + AOT)
   before touching the harness — not by running the whole category.
2. Edit `tools/torture_run.py`:
   - Global category skip → remove the category from `UNSUPPORTED_TARGETS`.
   - Otherwise remove/adjust the targeted skip.
3. **Targeted skips must be content-based** (scan test source for the blocking
   construct, e.g. `__builtin_setjmp`, `longjmp`) and scoped to the category.
   **Never filename-based** — c-torture filenames encode dates/PR numbers, not
   features.
4. Run the **full** sweep on an idle machine and **account for every
   previously-skipped test**: each must land in a known class
   (e.g. 37 unskipped → 24 PASS + 8 SKIP-TARGET:setjmp + 1 SKIP-TARGET:nonlocal_goto
   + 4 SKIP-C89). Unaccounted tests = bug in the skip logic or a new failure.
5. If green, `--update-baseline` (idle), then commit **harness + baseline
   together** with a message stating the pass delta and the per-class split.

## Before Ratcheting (mandatory pre-flight)

The change must already pass the full battery (see AGENTS.md §2):
`make build/bin/zerocc`, `make test`, `make test-opt`, `make test-driver`,
`make test-isa-qemu`, `python3 tools/diag_battery.py`,
`python3 tools/style_check.py`, and `grep -rn "ZC_DBG" src/zero_cc/ | wc -l`
must be `0`. A ratchet on top of a red battery records a broken baseline.
