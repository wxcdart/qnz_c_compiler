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


# C1 is a *correctness* bug (silent wrong values), not a quantity boundary:
# there is no N at which a correct compiler starts failing. The probe is an
# ordered regression battery; N = how many assertions to enable, and a
# failure returns the 1-based index of the first broken assertion, so
# WRONG(rc=k) identifies the regression directly. --bisect is not meaningful
# for this family (a correct compiler passes every N).
#
# NOTE: gcc 13.3 misattributes a bogus "no semicolon at end of struct or
# union" warning to the struct definition whenever ANY function body in the
# TU has a local declaration (the generated code is strictly conforming;
# the warning is a gcc position bug and does not affect the rc cross-check).
C1_CHECKS = [
    ("int add overflow",          "int a=2000000000;int b=a+a",                 "b!=-294967296"),
    ("int inc to INT_MIN",        "int x=2147483647;x++",                       "x!=-2147483648"),
    ("int dec to INT_MIN",        "int x=-2147483647;x--",                      "x!=-2147483648"),
    ("char 127+1 wraps",          "char c=127;c++",                             "c!=-128"),
    ("uchar 255+1 wraps to 0",    "unsigned char uc=255;uc++",                  "uc!=0"),
    ("schar -128-1 wraps",        "signed char sc=-128;sc--",                   "sc!=127"),
    ("short 32767+1 wraps",       "short s=32767;s++",                          "s!=-32768"),
    ("ushort 65535+1 wraps",      "unsigned short us=65535;us++",               "us!=0"),
    ("short -32768-1 wraps",      "short s=-32768;s--",                         "s!=32767"),
    ("int 1<<31",                 "int x=1",                                    "(x<<31)!=-2147483648"),
    ("INT_MIN>>1 arithmetic",     "int x=-2147483648",                          "(x>>1)!=-1073741824"),
    ("uint 1u<<31",               "unsigned u=1u",                              "(u<<31)!=2147483648u"),
    ("-16>>2 arithmetic",         "int x=-16",                                  "(x>>2)!=-4"),
    ("int add across INT_MAX",    "int x=2147483647;int y=x+100",               "y!=-2147483549"),
    ("uint 2^32-1 +1 wraps",      "unsigned u=4294967295u;u++",                 "u!=0"),
    ("cast u32->int narrows",     "unsigned u=4000000000u;int i=(int)u",        "i!=-294967296"),
    ("cast int->uchar",           "int x=-5;unsigned char uc=(unsigned char)x","uc!=251"),
    ("cast int->char",            "int x=300;char c=(char)x",                   "c!=44"),
    ("param passing narrow mix",  "",                                           "__PARAM__"),
    ("return uchar value",        "",                                           "__RET_UCHAR__"),
    ("return schar value",        "",                                           "__RET_SCHAR__"),
    ("global narrow assign",      "char gc;unsigned char guc;gc=127;guc=255",   "gc!=127||guc!=255"),
    ("global narrow init",        "",                                           "__GINIT__"),
    ("local struct narrow mem",   "",                                           "__LSM__"),
    ("global struct narrow mem",  "",                                           "__GSM__"),
    ("int compound += wrap",      "int x=2147483640;x+=100",                    "x!=-2147483556"),
    ("~ on narrow int",           "unsigned char uc=(unsigned char)~0x7f",      "uc!=0x80"),
    ("mixed uchar+ushort add",    "",                                           "__MIXED__"),
    ("long long 64-bit range",    "long long ll=4000000000000000000LL",         "!(ll>0)"),
]

# Assertions that need helper functions / file-scope objects, keyed above.
def _c1_helpers():
    return ("static int c1_param(int a,unsigned b,short c,unsigned char d){"
            "return (a==2000000000&&b==4294967295u&&c==-32768&&d==255)?0:1;}\n"
            "static int c1_ruchar(void){unsigned char x=255;return x;}\n"
            "static int c1_rschar(void){signed char x=-128;return x;}\n"
            "char c1_gc=127;unsigned char c1_guc=255;short c1_gs=-32768;\n"
            "struct C1S{short a;unsigned char b;unsigned short c;int d};\n"
            "static struct C1S c1_gs_s;\n"
            "static int c1_mixed(unsigned char a,unsigned short b){"
            "int s=(int)a+(int)b;return s==320?0:1;}\n")

_C1_SPECIAL = {
    "__PARAM__":     "c1_param(2000000000,4294967295u,-32768,255)!=0",
    "__RET_UCHAR__": "c1_ruchar()!=255",
    "__RET_SCHAR__": "c1_rschar()!=-128",
    "__GINIT__":     "c1_gc!=127||c1_guc!=255||c1_gs!=-32768",
    "__LSM__":       "(c1_local_ok()!=0)",
    "__GSM__":       "(c1_global_ok()!=0)",
    "__MIXED__":     "c1_mixed(160,160)!=0",
}
_C1_LOCAL_HELPERS = (
    "static int c1_local_ok(void){struct C1S s;s.a=-32768;s.b=255;"
    "s.c=65535;s.d=2000000000;"
    "return (s.a==-32768&&s.b==255&&s.c==65535&&s.d==2000000000)?0:1;}\n"
    "static int c1_global_ok(void){c1_gs_s.a=-32768;c1_gs_s.b=255;"
    "c1_gs_s.c=65535;c1_gs_s.d=2000000000;"
    "return (c1_gs_s.a==-32768&&c1_gs_s.b==255&&c1_gs_s.c==65535"
    "&&c1_gs_s.d==2000000000)?0:1;}\n")


def probe_c1_wrap(n):
    """C1: narrow-int wraparound/signedness regression battery (see C1_CHECKS)."""
    k = min(max(n, 0), len(C1_CHECKS))
    blocks = []
    for i, (label, decls, cond) in enumerate(C1_CHECKS[:k], 1):
        c = _C1_SPECIAL.get(cond, cond)
        if decls:
            blocks.append("  {%s; if(%s) return %d; /* %s */ }" % (decls, c, i, label))
        else:
            blocks.append("  {if(%s) return %d; /* %s */ }" % (c, i, label))
    return (_c1_helpers() + _C1_LOCAL_HELPERS + "int main(void){\n"
            + "\n".join(blocks) + "\n  return 0;\n}\n")


PROBES = {
    "c1-wrap":        (probe_c1_wrap,        "C1  int wraparound/signedness battery (rc = failing check #)"),
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
