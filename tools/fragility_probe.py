#!/usr/bin/env python3
"""
Fragility probe generator + boundary bisection for ZeroCC.

Generates parameterized stress probes (one knob = N), compiles/runs them with
zerocc in JIT (--run) and/or AOT (-o) mode with a timeout, classifies the
outcome, and can binary-search the boundary between "works" and "breaks".

Probe families (mapped to docs/fragility-analysis.md):
  nest-calls    C2  nested f(f(f(...1)...))          -> temp-id exhaustion
  expr-depth    H1  deeply nested parenthesized expr -> parser/recursion crash
  locals        C4  N locals in one function         -> hang/crash
  switch-cases  C3  N cases in one switch            -> silent drop / crash
  globals       M2  N file-scope globals             -> silent drop
  struct-members M3 N members in one struct          -> silent drop
  name-len      M1  N-char identifier                -> truncation collision

Usage examples:
  # single probe, both modes
  python3 tools/fragility_probe.py nest-calls 117
  # binary search the C2 boundary (JIT): largest N that works
  python3 tools/fragility_probe.py --bisect nest-calls --min 1 --max 200
  # inspect IR for temp-id exhaustion at a given depth
  python3 tools/fragility_probe.py nest-calls 117 --max-temp
  # AOT only, longer timeout, custom zerocc
  python3 tools/fragility_probe.py locals 231 --mode aot --timeout 15
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ZEROC = os.path.join(REPO, "build", "bin", "zerocc")


# ---------------------------------------------------------------- probes ----

def probe_nest_calls(n):
    """C2: n nested calls. Each call consumes several temps; ~117 breaks."""
    return ("int f(int x){return x;}\n"
            "int main(void){int r = " + "f(" * n + "1" + ")" * n +
            "; return r==1?0:1;}\n")


def probe_expr_depth(n):
    """H1: n-deep parenthesized/operand nesting. ~110-120 crashes."""
    inner = "1"
    for _ in range(n):
        inner = "(%s+0)" % inner
    return "int main(void){return %s==1?0:1;}\n" % inner


def probe_locals(n):
    """C4: n local variables in one function. ~231 breaks.

    NOTE: the usage expression must stay O(1) in temps — an `l0+l1+...+lN`
    check chain consumes ~N temps and triggers C2 (temp-id exhaustion) first,
    masking the C4 boundary.
    """
    decls = "".join("int l%d=0;\n" % i for i in range(n))
    k = min(3, n)
    check = "l0" + "".join("+l%d" % i for i in range(1, k))
    return "int main(void){%sreturn %s==0?0:1;}\n" % (decls, check)


def probe_switch_cases(n):
    """C3: n cases in one switch. >128 silently dropped (then crash).

    The case values must stay within the signed 16-bit range: the switch
    dispatch chain compares the selector with `ir_emit_mov_imm` (16-bit
    immediate), so larger constants silently change the probe's expected
    value. The member-count knob is the real axis under test.
    """
    n = min(n, 32767)
    cases = "".join("case %d:return %d;\n" % (i, i) for i in range(n))
    hi = n - 1
    probe_sentinel = hi + 1  # within int16 for hi < 32767
    return ("int f(int x){switch(x){\n" + cases + "default:return -1;}}\n"
            "int main(void){return f(%d)==%d&&f(%d)==-1?0:1;}\n" % (hi, hi, probe_sentinel))


def probe_globals(n):
    """M2: n file-scope globals. >256 silently dropped."""
    gs = "".join("int g%d=%d;\n" % (i, i) for i in range(n))
    check = "return " + "&&".join("g%d==%d" % (i, i) for i in range(0, n, max(1, n // 32))) + "?0:1;"
    return gs + "int main(void){%s}\n" % check


def probe_struct_members(n):
    """M3: n members in one struct. >64 silently dropped."""
    ms = "".join("int m%d;\n" % i for i in range(n))
    last = n - 1
    return ("struct S{\n" + ms + "};\nstruct S s;\n"
            "int main(void){s.m0=1;s.m%d=2;return s.m0==1&&s.m%d==2?0:1;}\n" % (last, last))


def probe_name_len(n):
    """M1: identifiers of length n. >127 truncated -> collision."""
    pre = "a" * max(1, n - 1)
    a, b = pre + "X", pre + "Y"
    return "int %s=1;\nint %s=2;\nint main(void){return %s==1&&%s==2?0:1;}\n" % (a, b, a, b)


PROBES = {
    "nest-calls":     (probe_nest_calls,     "C2  nested calls (temp-id exhaustion)"),
    "expr-depth":     (probe_expr_depth,     "H1  expression depth"),
    "locals":         (probe_locals,         "C4  locals per function"),
    "switch-cases":   (probe_switch_cases,   "C3  cases per switch"),
    "globals":        (probe_globals,        "M2  file-scope globals"),
    "struct-members": (probe_struct_members, "M3  struct members"),
    "name-len":       (probe_name_len,       "M1  identifier length"),
}


# ------------------------------------------------------------- execution ----

def run(cmd, timeout):
    """Return (rc, timed_out). rc is the exit code, or negative on error."""
    try:
        p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=timeout)
        return p.returncode, False
    except subprocess.TimeoutExpired:
        return 124, True
    except Exception:
        return -1, False


def rc_status(rc):
    """Classify a process exit code (subprocess convention: negative = signal)."""
    if rc == 124:
        return "HANG"
    if rc == 0:
        return "OK"
    if rc < 0:
        name = signal.Signals(-rc).name if -rc in signal.Signals._value2member_map_ else str(-rc)
        return "CRASH(%s)" % name
    if 1 <= rc <= 255:
        return "WRONG(rc=%d)" % rc
    return "CRASH(sig~%d)" % (rc - 128 if rc > 128 else rc)


def classify_jit(zerocc, src, timeout):
    """Run `zerocc --run src`. Return a status string."""
    rc, to = run([zerocc, "-O0", "--run", src], timeout)
    if to:
        return "HANG"
    return rc_status(rc)


def classify_aot(zerocc, src, timeout, tmpdir):
    """Compile `zerocc -o bin src`, then run bin. Return (status, detail)."""
    binpath = os.path.join(tmpdir, "aot_bin")
    rc, to = run([zerocc, "-O0", "-o", binpath, src], timeout)
    if to or rc == 124:
        return "HANG(compile)", rc
    if rc != 0:
        return "COMPILE-ERR", rc
    if not os.path.exists(binpath):
        return "NO-BINARY", rc
    rc, to = run([binpath], timeout)
    if to:
        return "HANG(run)", rc
    return rc_status(rc), rc


def run_probe(zerocc, family, n, modes, timeout, src_path=None):
    """Generate, run, and report one probe. Returns dict of mode -> status."""
    gen, _ = PROBES[family]
    src = src_path or os.path.join(tempfile.mkdtemp(prefix="zprobe_"), family + ".c")
    if src_path:
        with open(src, "w") as f:
            f.write(gen(n))
    else:
        d = os.path.dirname(src)
        with open(src, "w") as f:
            f.write(gen(n))
    results = {"src": src}
    if "jit" in modes:
        results["jit"] = classify_jit(zerocc, src, timeout)
    if "aot" in modes:
        d = tempfile.mkdtemp(prefix="zaot_")
        results["aot"], results["aot_detail"] = classify_aot(zerocc, src, timeout, d)
    return results


def fmt(results, n):
    parts = []
    if "jit" in results:
        parts.append("jit=%-16s" % results["jit"])
    if "aot" in results:
        parts.append("aot=%-16s" % results["aot"])
    return "  N=%-6d %s" % (n, "  ".join(parts))


def is_ok(results):
    for mode in ("jit", "aot"):
        if mode in results:
            if results[mode] != "OK":
                return False
    return True


# --------------------------------------------------------- IR triage --------

def max_temp_ir(zerocc, src):
    """Parse `zerocc -dump-ir src` and report the highest temp id seen.

    Note: if lowering hangs (temp-id wrap -> infinite loop), this times out
    and returns (None, 'TIMEOUT') — which is itself the C2 signature.
    """
    try:
        p = subprocess.run([zerocc, "-O0", "-dump-ir", src],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT(during lowering)"
    # -dump-ir writes to stderr (plus noise); scan both streams
    out = (p.stdout + p.stderr).decode(errors="replace")
    ids = [int(m.group(1)) for m in re.finditer(r"\br(\d+)\b", out)]
    if not ids:
        return None, len(out)
    return max(ids), len(out)


# ----------------------------------------------------------- bisection ------

def bisect(zerocc, family, lo, hi, modes, timeout):
    """Find the largest N in [lo, hi] that is OK in every selected mode.

    Assumes monotonicity: works at lo, breaks at hi (verified first).
    """
    if not is_ok(run_probe(zerocc, family, lo, modes, timeout)):
        print("lo=%d already fails: %s" % (lo, fmt(run_probe(zerocc, family, lo, modes, timeout), lo)))
        return lo - 1
    if is_ok(run_probe(zerocc, family, hi, modes, timeout)):
        print("hi=%d still OK; raise --max" % hi)
        return hi
    lo_ok, hi_bad = lo, hi
    while hi_bad - lo_ok > 1:
        mid = (lo_ok + hi_bad) // 2
        r = run_probe(zerocc, family, mid, modes, timeout)
        print(fmt(r, mid))
        if is_ok(r):
            lo_ok = mid
        else:
            hi_bad = mid
    r = run_probe(zerocc, family, hi_bad, modes, timeout)
    print(fmt(r, hi_bad))
    print("boundary: max OK N = %d, first failing N = %d" % (lo_ok, hi_bad))
    return lo_ok


# ------------------------------------------------------------------ main ----

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("family", choices=sorted(PROBES), help="probe family")
    ap.add_argument("n", nargs="?", type=int, default=None, help="probe parameter N")
    ap.add_argument("--mode", choices=["jit", "aot", "both"], default="both")
    ap.add_argument("--timeout", type=float, default=8.0, help="per-run timeout (s)")
    ap.add_argument("--zerocc", default=DEFAULT_ZEROC, help="zerocc binary")
    ap.add_argument("--bisect", action="store_true", help="binary-search the boundary")
    ap.add_argument("--min", type=int, default=1, help="bisect lower bound")
    ap.add_argument("--max", type=int, default=300, help="bisect upper bound")
    ap.add_argument("--max-temp", action="store_true",
                    help="also report the highest temp id in -dump-ir (JIT only)")
    ap.add_argument("--save-dir", default=None, help="write generated probe into dir")
    args = ap.parse_args()

    modes = ["jit", "aot"] if args.mode == "both" else [args.mode]
    gen, desc = PROBES[args.family]
    if not os.path.exists(args.zerocc):
        print("error: zerocc not found at %s (run `make build/bin/zerocc`)" % args.zerocc)
        return 2

    print("probe: %s -- %s" % (args.family, desc))

    if args.bisect:
        if args.n is not None:
            print("error: --bisect does not take N; use --min/--max")
            return 2
        bisect(args.zerocc, args.family, args.min, args.max, modes, args.timeout)
        return 0

    if args.n is None:
        ap.error("N is required unless --bisect")
    src = None
    if args.save_dir:
        os.makedirs(args.save_dir, exist_ok=True)
        src = os.path.join(args.save_dir, args.family + ".c")
    results = run_probe(args.zerocc, args.family, args.n, modes, args.timeout, src_path=src)
    print(fmt(results, args.n))
    if src:
        print("  probe saved: %s" % src)
    if args.max_temp and "jit" in modes:
        mt, info = max_temp_ir(args.zerocc, src or results["src"])
        print("  max temp id in -dump-ir: %s (detail: %s)" % (mt, info))
    return 0 if is_ok(results) else 1


if __name__ == "__main__":
    sys.exit(main())
