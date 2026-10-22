#!/usr/bin/env python3
"""Differential diagnostics battery (zerocc vs gcc).

Compiles every program in test/diag/*.c with both zerocc (-S, compile-only —
no execution, no linking) and gcc (-fsyntax-only), classifies each reported
diagnostic into an error CATEGORY, and compares coverage.

Categories — not message wording — are compared, because the compilers phrase
the same fault differently.  What the battery tracks is: which classes of
invalid C each compiler actually diagnoses, and how many errors a multi-fault
file produces in one pass (the multi-error-recovery target).

Gate semantics (baseline ratchet, test/diag/baseline.json):
  * per case: zerocc error count, zerocc category set, and rc expectation
    are recorded at baseline time
  * a current run REGRESSES if it loses a category, drops the error count,
    or flips the expected rc
  * --update-baseline re-records the current state (run after an intentional
    improvement, e.g. multi-error recovery)
  * gcc output is informational only — its behavior varies by version

Usage:
    python3 tools/diag_battery.py                 # run + gate on baseline
    python3 tools/diag_battery.py --list
    python3 tools/diag_battery.py --only 14       # subset by name prefix
    python3 tools/diag_battery.py --update-baseline
    python3 tools/diag_battery.py --json          # machine-readable state
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ZEROC = REPO / "build" / "bin" / "zerocc"
CORPUS = REPO / "test" / "diag"
BASELINE = CORPUS / "baseline.json"

# file:line(:col)?: severity: message   (line/col optional — some zerocc
# semantic errors are position-less)
DIAG_RE = re.compile(r"^[^:\s][^:]*?(?::\d+)?(?::\d+)?:\s+(fatal error|error|warning):\s+(.*\S)\s*$")

# (category, regex applied to the message text)
CATEGORIES = [
    ("expected-punctuator", re.compile(
        r"expected\s+['\u2018][,;]|expected\s+['\u2018,;]\s*(or|before|after)")),
    ("missing-delimiter", re.compile(
        r"expected\s+'\}'|expected\s+\u2018\}\u2019|expected\s+'\)'|expected\s+\u2018\)\u2019|"
        r"to end (block|input|function)|end of input")),
    ("undeclared-ident", re.compile(
        r"undeclared identifier|undeclared \(first use")),
    ("arg-count", re.compile(
        r"too few arguments|too many arguments|argument count mismatch|expects \d+ arguments")),
    ("no-member", re.compile(r"has no member named")),
    ("bad-member-base", re.compile(
        r"not a (structure or union|pointer)|invalid type argument of|cannot take address")),
    ("undefined-label", re.compile(r"used but not defined")),
    ("incompatible-types", re.compile(
        r"incompatible types|incompatible return type|makes (integer|pointer) from")),
    ("implicit-decl", re.compile(r"implicit declaration of (function|variable)")),
    ("called-not-func", re.compile(r"called object is not a function|is not a (function|callable)")),
]


def classify(msg):
    cats = set()
    for name, rx in CATEGORIES:
        if rx.search(msg):
            cats.add(name)
    return cats


def run_zerocc(src):
    p = subprocess.run([str(ZEROC), "-S", str(src), "-o", os.devnull],
                       capture_output=True, text=True, timeout=60)
    return p.returncode, parse_diagnostics(p.stderr)


def run_gcc(src):
    p = subprocess.run(["gcc", "-fsyntax-only", str(src)],
                       capture_output=True, text=True, timeout=60)
    return p.returncode, parse_diagnostics(p.stderr)


def parse_diagnostics(stderr):
    """Return (errors, warnings): lists of message strings."""
    errors, warnings = [], []
    for line in stderr.splitlines():
        if line.startswith("Compile "):          # zerocc's summary line
            continue
        m = DIAG_RE.match(line)
        if not m:
            continue
        sev, msg = m.groups()
        (warnings if sev == "warning" else errors).append(msg)
    return errors, warnings


def run_case(path):
    name = path.name[:-2]
    zrc, (zerr, zwarn) = run_zerocc(path)
    src = path.read_text()
    gsrc = path.parent / (".gccprobe_" + path.name)
    try:
        gsrc.write_text(src)   # gcc needs a .c path it can read normally
        grc, (gerr, gwarn) = run_gcc(gsrc)
    finally:
        gsrc.unlink(missing_ok=True)
    return {
        "case": name,
        "zerocc_rc": zrc,
        "zerocc_errors": zerr,
        "zerocc_error_count": len(zerr),
        "zerocc_categories": sorted(classify_all(zerr)),
        "zerocc_warnings": zwarn,
        "gcc_rc": grc,
        "gcc_errors": gerr,
        "gcc_error_count": len(gerr),
        "gcc_categories": sorted(classify_all(gerr)),
    }


def classify_all(msgs):
    out = set()
    for m in msgs:
        out |= classify(m)
    return out


def load_baseline():
    if BASELINE.exists():
        return json.loads(BASELINE.read_text())
    return None


def save_baseline(results):
    cases = {}
    for r in results:
        cases[r["case"]] = {
            "expect_rc_zero": r["zerocc_rc"] == 0,
            "zerocc_error_count": r["zerocc_error_count"],
            "zerocc_categories": r["zerocc_categories"],
            "gcc_error_count": r["gcc_error_count"],
            "gcc_categories": r["gcc_categories"],
        }
    BASELINE.write_text(json.dumps({
        "generated_by": "tools/diag_battery.py --update-baseline",
        "note": "ratchet baseline: current runs must be a superset (no lost category, no dropped error count, no rc flip). Re-record after intentional improvements.",
        "cases": cases,
    }, indent=2) + "\n")


def gate(results, baseline, expect_all=True):
    """Return a list of regression strings (empty = pass).

    expect_all: when True (full corpus run) a baseline case absent from the
    run is a regression (deleted file).  When False (--only subset) absent
    cases are simply not part of this run and are not flagged.
    """
    problems = []
    if baseline is None:
        return ["no baseline recorded — run --update-baseline to record one"]
    bcases = baseline.get("cases", {})
    for r in results:
        name = r["case"]
        if name not in bcases:
            continue  # new case: informational until baselined
        b = bcases[name]
        if b.get("expect_rc_zero") and r["zerocc_rc"] != 0:
            problems.append(f"{name}: now fails to compile (rc={r['zerocc_rc']})")
        if (not b.get("expect_rc_zero")) and r["zerocc_rc"] == 0:
            problems.append(f"{name}: now compiles silently (rc=0) — a constraint check was lost")
        if r["zerocc_error_count"] < b["zerocc_error_count"]:
            problems.append(f"{name}: error count dropped {b['zerocc_error_count']} -> {r['zerocc_error_count']}")
        lost = set(b["zerocc_categories"]) - set(r["zerocc_categories"])
        if lost:
            problems.append(f"{name}: lost categories: {', '.join(sorted(lost))}")
    if expect_all:
        gone = set(bcases) - {r["case"] for r in results}
        for g in sorted(gone):
            problems.append(f"{g}: case file missing from corpus")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list cases and exit")
    ap.add_argument("--only", metavar="PREFIX", help="run only cases whose name starts with PREFIX")
    ap.add_argument("--update-baseline", action="store_true", help="record current state as the baseline")
    ap.add_argument("--json", action="store_true", help="emit machine-readable state")
    args = ap.parse_args()

    if not ZEROC.exists():
        sys.exit(f"error: {ZEROC} not found — run `make build/bin/zerocc` first")
    if shutil.which("gcc") is None:
        sys.exit("error: gcc not found (needed as the differential reference)")

    cases = sorted(CORPUS.glob("*.c"))
    if args.only:
        cases = [c for c in cases if c.name.startswith(args.only)]
    if not cases:
        sys.exit(f"error: no cases in {CORPUS}")

    if args.list:
        for c in cases:
            print(c.name)
        return

    results = [run_case(c) for c in cases]

    if args.json:
        print(json.dumps(results, indent=2))
    else:
        print(f"Diagnostics battery: {len(results)} cases  "
              f"(zerocc -S  vs  gcc -fsyntax-only)\n")
        print(f"{'case':<26} {'rc':>3} {'zerr':>5} {'gerr':>5}   zerocc categories  /  gcc categories")
        print("-" * 100)
        for r in results:
            print(f"{r['case']:<26} {r['zerocc_rc']:>3} {r['zerocc_error_count']:>5} {r['gcc_error_count']:>5}   "
                  f"{', '.join(r['zerocc_categories']) or '-':<40} / {', '.join(r['gcc_categories']) or '-'}")
        print()

        # coverage summary
        z_all = g_all = set()
        for r in results:
            z_all |= set(r["zerocc_categories"])
            g_all |= set(r["gcc_categories"])
        if g_all:
            hit = z_all & g_all
            print(f"category coverage vs gcc: {len(hit)}/{len(g_all)}  "
                  f"(zerocc-only: {sorted(z_all - g_all) or '-'};  gcc-only gap: {sorted(g_all - z_all) or '-'})\n")

        baseline = load_baseline()
        if args.update_baseline:
            save_baseline(results)
            print(f"baseline updated: {BASELINE}")
            return

        problems = gate(results, baseline, expect_all=not args.only)
        if problems:
            print("BASELINE REGRESSIONS:")
            for p in problems:
                print(f"  - {p}")
            sys.exit(1)
        if baseline is not None:
            print("baseline: OK (no regressions)")
    sys.exit(0)


if __name__ == "__main__":
    main()
