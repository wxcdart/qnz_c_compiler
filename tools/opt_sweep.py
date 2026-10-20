#!/usr/bin/env python3
"""
opt_sweep.py - Optimization-level consistency sweep (Sprint 35 Phase 0)

Compiles every standalone program in the test corpus with zerocc at
-O0/-O1/-O2/-O3 in both JIT (--run) and AOT (-o) modes, runs each result,
and checks that (exit code, stdout) is stable across levels and matches the
-O0 baseline. Also cross-checks against a gcc build where possible.

Why: `make test` builds test binaries with the host compiler, so it never
exercises zerocc's optimizer. This sweep is the direct-zerocc regression
guard for the optimizer passes (mem2reg, LICM, inlining, vectorizer, ...).

Rules per test:
  * Baseline = -O0 JIT run. A second -O0 JIT run detects nondeterministic
    programs (timing/ASLR-dependent output); those are flagged NONDET and
    excluded from level comparison (still reported).
  * Any -O1..-O3 JIT or AOT run whose (rc, stdout) differs from the -O0
    baseline of the same mode is a MISMATCH (regression).
  * A level that fails to compile when -O0 compiles is a COMPILE-FAIL.
  * A level that times out when the baseline did not is a TIMEOUT.
  * A test that does not compile/run at -O0 is SKIPPED (pre-existing gap,
    not a regression; still listed).
  * gcc reference: a gcc -O2 build's (rc, stdout) is compared with the -O0
    JIT baseline; differences are flagged as REF-DIFF (informational).

Exit status: 0 = no MISMATCH/COMPILE-FAIL/TIMEOUT, 1 = at least one.

Usage:
    python3 tools/opt_sweep.py [--zerocc PATH] [--gcc CC]
                               [--corpus DIR ...] [--only GLOB]
                               [--timeout SEC] [--no-gcc] [--json]
                               [--jit-only|--aot-only] [--keep]
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CORPUS = ["test/jit", "test/aot", "test/licm", "test/inline_audit"]
LEVELS = [0, 1, 2, 3]


@dataclass
class RunResult:
    status: str          # ok, compile-fail, timeout, crash
    rc: Optional[int] = None
    stdout: str = ""
    stderr: str = ""


@dataclass
class TestReport:
    name: str            # corpus-relative, e.g. jit/break_continue
    path: str
    baseline: Optional[RunResult] = None
    aot_baseline: Optional[RunResult] = None
    gcc: Optional[RunResult] = None
    nondet: bool = False
    skipped: str = ""                     # reason if skipped
    mismatches: List[str] = field(default_factory=list)
    details: List[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.mismatches


def short(s: str, n: int = 160) -> str:
    s = s.replace("\n", "\\n")
    return s if len(s) <= n else s[: n - 3] + "..."


def run_cmd(cmd: List[str], timeout: float) -> RunResult:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return RunResult("ok", p.returncode, p.stdout, p.stderr)
    except subprocess.TimeoutExpired:
        return RunResult("timeout")
    except Exception as e:  # noqa: BLE001
        return RunResult("compile-fail", None, "", str(e))


def jit_run(zerocc: str, src: str, level: int, timeout: float) -> RunResult:
    r = run_cmd([zerocc, "--run", f"-O{level}", src], timeout)
    if r.status == "ok" and r.rc is None:
        r.status = "compile-fail"
    return r


def aot_run(zerocc: str, src: str, level: int, out: str, timeout: float) -> Tuple[RunResult, RunResult]:
    """Returns (compile result, run result)."""
    cr = run_cmd([zerocc, f"-O{level}", src, "-o", out], timeout)
    if cr.status != "ok":
        return cr, RunResult(cr.status)
    rr = run_cmd([out], timeout)
    return cr, rr


def main() -> int:
    ap = argparse.ArgumentParser(description="zerocc -O0..-O3 consistency sweep")
    ap.add_argument("--zerocc", default=os.path.join(ROOT, "build/bin/zerocc"))
    ap.add_argument("--gcc", default="gcc")
    ap.add_argument("--corpus", nargs="*", default=DEFAULT_CORPUS,
                    help="directories containing standalone .c tests")
    ap.add_argument("--only", default=None, help="substring or glob filter on test name")
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--no-gcc", action="store_true", help="skip gcc reference builds")
    ap.add_argument("--jit-only", action="store_true")
    ap.add_argument("--aot-only", action="store_true")
    ap.add_argument("--json", action="store_true", help="emit machine-readable summary")
    ap.add_argument("--keep", action="store_true", help="keep AOT artifacts in /tmp")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.isfile(args.zerocc):
        print(f"error: zerocc not found at {args.zerocc} (build it first: make build/bin/zerocc)",
              file=sys.stderr)
        return 2

    # ---- corpus discovery: standalone .c files (have int main) ----------
    tests: List[str] = []
    for d in args.corpus:
        dp = d if os.path.isabs(d) else os.path.join(ROOT, d)
        for f in sorted(glob.glob(os.path.join(dp, "*.c"))):
            name = os.path.relpath(f, ROOT).replace(os.sep, "/")
            try:
                with open(f) as fh:
                    src = fh.read()
            except OSError:
                continue
            if not re.search(r"\bint\s+main\s*\(", src):
                continue
            if args.only and (args.only not in name and not re.match(args.only, name)):
                continue
            tests.append(name)

    reports: List[TestReport] = []
    tmpdir = tempfile.mkdtemp(prefix="opt_sweep_")

    for name in tests:
        path = os.path.join(ROOT, *name.split("/"))
        rep = TestReport(name=name, path=path)
        reports.append(rep)

        # Baseline: -O0 JIT, twice (nondeterminism probe)
        b1 = jit_run(args.zerocc, path, 0, args.timeout)
        b2 = jit_run(args.zerocc, path, 0, args.timeout)
        if b1.status != "ok" or b2.status != "ok":
            rep.skipped = f"baseline -O0 JIT {b1.status or b2.status}"
            continue
        if (b1.rc, b1.stdout) != (b2.rc, b2.stdout):
            rep.nondet = True
            rep.baseline = b1
            rep.details.append(f"NONDET: -O0 JIT run differs between repetitions "
                               f"(rc {b1.rc}/{b2.rc}); level comparison skipped")
            continue
        rep.baseline = b1

        # gcc reference (vs -O0 JIT baseline)
        if not args.no_gcc:
            g = os.path.join(tmpdir, f"{name.replace('/', '_')}.gcc")
            gc = run_cmd([args.gcc, "-O2", "-o", g, path], args.timeout)
            if gc.status == "ok" and gc.rc == 0:
                gr = run_cmd([g], args.timeout)
                rep.gcc = gr
                if gr.status == "ok" and (gr.rc, gr.stdout) != (b1.rc, b1.stdout):
                    rep.details.append(
                        f"REF-DIFF: gcc -O2 rc={gr.rc} out={short(gr.stdout)} "
                        f"vs zerocc -O0 JIT rc={b1.rc} out={short(b1.stdout)}")
            else:
                rep.details.append(f"gcc reference unavailable (compile rc={gc.rc})")

        # JIT sweep
        if not args.aot_only:
            for lvl in LEVELS:
                if lvl == 0:
                    continue
                r = jit_run(args.zerocc, path, lvl, args.timeout)
                if r.status == "timeout":
                    rep.mismatches.append(f"JIT -O{lvl}: TIMEOUT (baseline ran in time)")
                elif r.status != "ok":
                    rep.mismatches.append(f"JIT -O{lvl}: {r.status.upper()} (baseline OK)")
                elif (r.rc, r.stdout) != (b1.rc, b1.stdout):
                    rep.mismatches.append(
                        f"JIT -O{lvl}: MISMATCH rc={r.rc} out={short(r.stdout)} "
                        f"(baseline rc={b1.rc} out={short(b1.stdout)})")

        # AOT sweep
        if not args.jit_only:
            a0c, a0r = aot_run(args.zerocc, path, 0, os.path.join(tmpdir, name.replace("/", "_") + ".z0"), args.timeout)
            if a0c.status != "ok" or a0c.rc not in (0, None) or a0r.status != "ok":
                rep.details.append(f"AOT -O0 baseline unavailable ({a0c.status} rc={a0c.rc}); AOT sweep skipped")
                continue
            rep.aot_baseline = a0r
            for lvl in LEVELS:
                if lvl == 0:
                    continue
                out = os.path.join(tmpdir, f"{name.replace('/', '_')}.z{lvl}")
                cc, rr = aot_run(args.zerocc, path, lvl, out, args.timeout)
                if cc.status == "timeout":
                    rep.mismatches.append(f"AOT -O{lvl}: COMPILE TIMEOUT")
                elif cc.status != "ok":
                    rep.mismatches.append(f"AOT -O{lvl}: COMPILE-FAIL")
                elif rr.status == "timeout":
                    rep.mismatches.append(f"AOT -O{lvl}: TIMEOUT (baseline ran in time)")
                elif rr.status != "ok":
                    rep.mismatches.append(f"AOT -O{lvl}: {rr.status.upper()}")
                elif (rr.rc, rr.stdout) != (a0r.rc, a0r.stdout):
                    rep.mismatches.append(
                        f"AOT -O{lvl}: MISMATCH rc={rr.rc} out={short(rr.stdout)} "
                        f"(baseline rc={a0r.rc} out={short(a0r.stdout)})")

    # ---- report ----------------------------------------------------------
    if args.json:
        data = [{
            "test": r.name, "ok": r.ok, "skipped": r.skipped, "nondet": r.nondet,
            "mismatches": r.mismatches, "details": r.details,
            "baseline": None if not r.baseline else
                {"rc": r.baseline.rc, "stdout": r.baseline.stdout},
        } for r in reports]
        print(json.dumps(data, indent=1))
    else:
        n_fail = sum(1 for r in reports if not r.ok)
        n_skip = sum(1 for r in reports if r.skipped)
        n_nondet = sum(1 for r in reports if r.nondet)
        for r in reports:
            if r.ok and not r.details:
                continue
            tag = "OK " if r.ok else "FAIL"
            if r.skipped:
                tag = "SKIP"
            print(f"[{tag}] {r.name}" + (f"  ({r.skipped})" if r.skipped else ""))
            for d in r.details:
                print(f"        {d}")
            for m in r.mismatches:
                print(f"        !! {m}")
        print(f"\n{len(reports)} tests: {len(reports) - n_fail - n_skip} ok, "
              f"{n_fail} failed, {n_skip} skipped, {n_nondet} nondeterministic")

    if not args.keep:
        import shutil
        shutil.rmtree(tmpdir, ignore_errors=True)

    return 1 if any(not r.ok for r in reports) else 0


if __name__ == "__main__":
    sys.exit(main())
