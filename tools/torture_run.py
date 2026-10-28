#!/usr/bin/env python3
"""tools/torture_run.py — GCC c-torture differential conformance gate for ZeroCC.

Runs the pristine GCC c-torture suite (third_party/vendor/c-torture) through zerocc
and differentially compares against a gcc reference (default language mode, -O1,
which is how c-torture's own dg driver invokes the compiler).

Test sets:
  c-torture/execute  — run tests: zerocc must compile AND produce (rc, stdout)
                       identical to the gcc reference at every -O0..-O3 in both
                       JIT (--run) and AOT (-o) modes.
  c-torture/compile  — acceptance tests: zerocc must compile the file at -O0 and -O2.

Classification:
  PASS            — accepted (compile) / matches reference on all configs (run)
  MISCOMPILE      — compiled cleanly but (rc, stdout) diverged on some config
  REJECT          — zerocc rejected a file gcc accepts; triaged with
                    gcc -std=c99/-c11 -Wpedantic: if the file is clean C99 or
                    clean C11 it is a conformance gap (counted, ratcheted);
                    otherwise SKIP-C89 (obsolescent / GNU-extended).
  EXPECTED-REJECT — file carries dg-error (gcc itself rejects it).
  NO-REF          — gcc could not build the reference.
  TIMEOUT         — zerocc run exceeded its time budget.
  SKIP-*          — pre-skipped before any work (GNU-extension targets, dg-bogus,
                    pointer output, reference-semantics flags we cannot emulate).

Baseline (test/torture/baseline.json) ratchets: MISCOMPILE and REJECT(conformance)
counts may never increase, and no test may regress from PASS. Use --update-baseline
to re-record after an intentional improvement.

Usage:
  python3 tools/torture_run.py               # gate against baseline (or record first)
  python3 tools/torture_run.py --update-baseline
  python3 tools/torture_run.py --only "execute/pr" --limit 50   # dev iteration
  python3 tools/torture_run.py --only "pr109386,mzero4,fp-cmp-7" # comma-separated patterns
  python3 tools/torture_run.py --chunk m2    # batch-run partial-mode MISCOMPILEs (16 tests)
  python3 tools/torture_run.py --quick       # -O1/-O2, JIT only

M1 (TIMEOUT hang) burn-down tools:
  python3 tools/torture_run.py --only-class TIMEOUT            # re-run just the 10 M1 hangs
  python3 tools/torture_run.py --only "execute/pr94238" --repro # exact repro commands
  python3 tools/torture_run.py --hang-debug "execute/pr94238"  # reproduce a hang and
                                                               # gdb-attach: backtrace,
                                                               # source line, pc disasm
  python3 tools/torture_run.py --hang-debug T --hang-config O2-jit --hang-all
  python3 tools/torture_run.py --keep                    # retain artifacts in /tmp/torture_work
  python3 tools/torture_run.py --timeout 30              # override run/build timeout (sec)
"""

import argparse
import concurrent.futures
import hashlib
import json
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CORPUS = REPO / "third_party/vendor/c-torture"
BASELINE_PATH = REPO / "test/torture/baseline.json"
ZEROC = REPO / "build/bin/zerocc"
REFCACHE = REPO / "build/test/torture/refcache"
WORK = Path("/tmp/torture_work")

RUN_LEVELS = [0, 1, 2, 3]
RUN_MODES = ["jit", "aot"]
COMPILE_LEVELS = [0, 2]
RUN_TIMEOUT = 5
BUILD_TIMEOUT = 10
KEEP = False  # --keep: retain work artifacts instead of unlinking

# dg-require-effective-target tokens zerocc cannot satisfy (GNU extensions /
# other architectures / features we deliberately do not implement).
UNSUPPORTED_TARGETS = {
    # GNU C extensions
    "label_values", "untyped_assembly", "indirect_jumps", "trampolines",
    "nonlocal_goto", "return_address", "named_sections", "unwrapped",
    "int128", "cilk",
    # other languages
    "c++", "cxx", "c2a", "go", "fortran", "ada", "objc", "java", "openmp",
    # other architectures / feature targets
    "arm", "armeb", "armel", "aarch64", "powerpc", "powerpc64", "ppc",
    "ppc64", "ia64", "mips", "mipsel", "s390", "s390x", "hppa", "sparc",
    "sparc64", "vax", "m68k", "msp430", "cris", "alpha", "avrgcc",
    "ieee-float128",
}

# dg-options that change the reference's observable semantics in ways zerocc
# cannot emulate (we have no x87 mode and no fast-math pipeline).
SKIP_OPTION_SUBSTR = ("-mfpmath=", "-ffast-math")

RE_DG_DO = re.compile(r"\{\s*dg-do\s+([a-z-]+)")
RE_DG_REQ = re.compile(r"\{\s*dg-require-effective-target\s+([^}]*?)\s*\}")
RE_DG_OPTS = re.compile(r'\{\s*dg-options\s+"([^"]*)"')
RE_DG_BOGUS = re.compile(r"\{\s*dg-bogus")
RE_DG_ERROR = re.compile(r"\{\s*dg-error")
RE_DG_TIMEOUT = re.compile(r"\{\s*dg-timeout\s+(\d+)")
RE_PCT_P = re.compile(r'("%p|%p"|[^"%]%p)')


def parse_directives(path):
    """Scan the first 60 lines for dg directives (they all live up top)."""
    try:
        full_text = path.read_text(errors="replace")
        text = full_text.splitlines()[:60]
    except OSError:
        full_text = ""
        text = []
    blob = "\n".join(text)
    d = {
        "do": None,
        "requires": [],
        "options": "",
        "bogus": False,
        "expects_error": False,
        "timeout": RUN_TIMEOUT,
        "ptr_output": False,
    }
    m = RE_DG_DO.search(blob)
    if m:
        d["do"] = m.group(1)
    for m in RE_DG_REQ.finditer(blob):
        d["requires"].extend(
            t for t in re.split(r"[^A-Za-z0-9_]+", m.group(1)) if t)
    m = RE_DG_OPTS.search(blob)
    if m:
        d["options"] = m.group(1)
    d["bogus"] = bool(RE_DG_BOGUS.search(blob))
    d["expects_error"] = bool(RE_DG_ERROR.search(blob))
    m = RE_DG_TIMEOUT.search(blob)
    if m:
        d["timeout"] = max(1, int(m.group(1)))
    d["ptr_output"] = bool(RE_PCT_P.search(full_text))
    return d


def skip_reason(path, d):
    rel = path.relative_to(CORPUS)
    if d["bogus"]:
        return "SKIP-BOGUS"
    for t in d["requires"]:
        if t in UNSUPPORTED_TARGETS:
            return "SKIP-TARGET:%s" % t
    for s in SKIP_OPTION_SUBSTR:
        if s in d["options"]:
            return "SKIP-REFSEM"
    if d["ptr_output"]:
        return "SKIP-PTR"
    top = rel.parts[0]
    if top in ("compat", "unsorted"):
        return "SKIP-DIR:%s" % top
    if d["do"] == "none":
        return "SKIP-DG-NONE"
    return None


def run(cmd, timeout):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "TIMEOUT", "", ""


def keep_or_unlink(p):
    """Remove a work artifact unless --keep is active (hang/miscompile retention)."""
    if KEEP:
        return
    p.unlink(missing_ok=True)


def gcc_ref(path, d, is_run):
    """Build (or reuse) the gcc reference. Returns (ok, artifact, detail)."""
    opt = d["options"]
    key = hashlib.sha1(
        (str(path.relative_to(CORPUS)) + opt + ("run" if is_run else "c")
         ).encode()).hexdigest()
    REFCACHE.mkdir(parents=True, exist_ok=True)
    art = REFCACHE / (key + (".bin" if is_run else ".o"))
    if art.exists():
        return True, art, "cache"
    base = ["gcc", "-O1"]
    if opt:
        base += opt.split()
    if is_run:
        cmd = base + [str(path), "-o", str(art), "-lm"]
    else:
        cmd = base + [str(path), "-c", "-o", str(art)]
    rc, _out, err = run(cmd, BUILD_TIMEOUT)
    if rc != 0:
        lines = [l for l in err.strip().splitlines() if l.strip()]
        return False, None, lines[-1] if lines else "gcc rc=%s" % rc
    return True, art, "built"


def zc_flags(d, level):
    """dg-options minus the flags that would fight the sweep level/mode."""
    out, skip_next = [], False
    for tok in d["options"].split():
        if skip_next:
            skip_next = False
            continue
        if tok == "-o":
            skip_next = True
        elif tok in ("-S", "-c", "-E", "-m32"):
            continue
        elif re.fullmatch(r"-O[0-3sg]*", tok):
            continue
        out.append(tok)
    return ["-O%d" % level] + out


def run_one(rel_path):
    """Full classification of one test. Returns a result dict."""
    path = CORPUS / rel_path
    d = parse_directives(path)
    res = {"path": rel_path, "class": None, "configs": {}, "note": ""}
    sk = skip_reason(path, d)
    if sk:
        res["class"] = sk
        return res

    in_execute = rel_path.startswith("execute/")
    do = d["do"]
    # dg-do overrides the directory's default semantics
    is_run = (do == "run") if do else in_execute

    ok, art, detail = gcc_ref(path, d, is_run)
    if d["expects_error"]:
        if not ok:
            res["class"] = "EXPECTED-REJECT"
            res["note"] = detail
            return res
        res["class"] = "NO-REF"
        res["note"] = "dg-error but gcc accepted"
        return res
    if not ok:
        res["class"] = "NO-REF"
        res["note"] = detail
        return res

    WORK.mkdir(parents=True, exist_ok=True)
    tid = hashlib.sha1(rel_path.encode()).hexdigest()[:12]
    if not is_run:
        levels = COMPILE_LEVELS
        for lvl in levels:
            fl = zc_flags(d, lvl)
            out = WORK / ("obj_%s_O%d.o" % (tid, lvl))
            rc, _o, err = run(
                [str(ZEROC), *fl, "-c", str(path), "-o", str(out)], BUILD_TIMEOUT)
            keep_or_unlink(out)
            if rc == "TIMEOUT":
                res["configs"]["O%d-aot" % lvl] = "TIMEOUT"
            else:
                res["configs"]["O%d-aot" % lvl] = "OK" if rc == 0 else "REJECT"
            if rc != 0 and rc != "TIMEOUT" and not res["note"]:
                errl = [l for l in err.splitlines() if l.strip()]
                res["note"] = "; ".join(errl[-2:])[:200]
        _finalize(res, path)
        return res

    # Run test: capture the reference output once.
    rc_ref, out_ref, _e = run([str(art)], d["timeout"])
    for lvl in RUN_LEVELS:
        fl = zc_flags(d, lvl)
        for mode in RUN_MODES:
            tag = "O%d-%s" % (lvl, mode)
            if mode == "jit":
                rc, outp, err = run(
                    [str(ZEROC), *fl, "--run", str(path)], d["timeout"])
                if rc == 1 and ("Compile error:" in err or "error:" in err):
                    res["configs"][tag] = "REJECT"
                    if not res["note"]:
                        errl = [l for l in err.splitlines() if l.strip()]
                        res["note"] = "; ".join(errl[-2:])[:200]
                    continue
            else:
                binp = WORK / ("bin_%s_O%d" % (tid, lvl))
                rc, _o, err = run(
                    [str(ZEROC), *fl, str(path), "-o", str(binp)], BUILD_TIMEOUT)
                if rc == "TIMEOUT":
                    keep_or_unlink(binp)
                    res["configs"][tag] = "TIMEOUT"
                    continue
                if rc != 0:
                    keep_or_unlink(binp)
                    res["configs"][tag] = "REJECT"
                    if not res["note"]:
                        errl = [l for l in err.splitlines() if l.strip()]
                        res["note"] = "; ".join(errl[-2:])[:200]
                    continue
                rc, outp, err = run([str(binp)], d["timeout"])
                keep_or_unlink(binp)
            if rc == "TIMEOUT":
                res["configs"][tag] = "TIMEOUT"
                continue
            if rc == rc_ref and outp == out_ref:
                res["configs"][tag] = "OK"
            else:
                res["configs"][tag] = "MISCOMPILE"
                if not res["note"]:
                    res["note"] = "ref(rc=%s out=%r) O%d got rc=%s out=%r" % (
                        rc_ref, out_ref[:40], lvl, rc, outp[:40])
    _finalize(res, path)
    return res


def _triage_reject(path, res):
    """Split conformance REJECTs from C89-obsolescent / GNU-extension files."""
    for std in ("c11", "c99"):
        rc, _o, _err = run(
            ["gcc", "-std=%s" % std, "-pedantic-errors", "-fsyntax-only", str(path)],
            BUILD_TIMEOUT)
        if rc == 0:
            res["class"] = "REJECT"
            res["note"] = (res.get("note") or "") + " [clean -std=%s]" % std
            return
    res["class"] = "SKIP-C89"


def _finalize(res, path):
    cfgs = res["configs"]
    bad = {k for k, v in cfgs.items() if v == "MISCOMPILE"}
    rej = {k for k, v in cfgs.items() if v == "REJECT"}
    tout = {k for k, v in cfgs.items() if v == "TIMEOUT"}
    if bad:
        res["class"] = "MISCOMPILE"
        res["note"] += " | cfgs: %s" % ",".join(sorted(bad))
    elif rej:
        _triage_reject(path, res)
        res["note"] += " | cfgs: %s" % ",".join(sorted(rej))
    elif tout:
        res["class"] = "TIMEOUT"
        res["note"] += " | cfgs: %s" % ",".join(sorted(tout))
    else:
        res["class"] = "PASS"


def repro_cmd(rel_path, d, cfg):
    """Exact shell command reproducing one (test, config) pair."""
    lvl_s, mode = cfg.rsplit("-", 1)
    lvl = int(lvl_s[1:])
    path = CORPUS / rel_path
    fl = " ".join(zc_flags(d, lvl))
    if rel_path.startswith("compile/"):
        return "%s %s -c %s -o /tmp/torture.o" % (ZEROC, fl, path)
    if mode == "jit":
        return "%s %s --run %s" % (ZEROC, fl, path)
    return "%s %s %s -o /tmp/torture.bin && /tmp/torture.bin" % (ZEROC, fl, path)


def gdb_probe(cmd, wait):
    """Re-run cmd under gdb (gdb is the parent, so ptrace works even under
    yama ptrace_scope=1). If the inferior survives `wait` seconds, interrupt
    it (SIGINT, like terminal Ctrl-C) and grab bt + source line + pc disasm.
    Returns the gdb transcript."""
    p = subprocess.Popen(
        ["gdb", "-q", "-nx",
         "-iex", "set debuginfod enabled off",
         "-ex", "set pagination off",
         "-ex", "set confirm off",
         "--args", *cmd],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)
    p.stdin.write("run\n")
    p.stdin.flush()
    try:
        p.wait(timeout=wait)
    except subprocess.TimeoutExpired:
        p.send_signal(signal.SIGINT)
        time.sleep(0.5)  # let gdb stop the inferior and reach the prompt
        p.stdin.write("bt 25\ninfo line\nx/4i $pc\nquit\n")
        p.stdin.flush()
        try:
            p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            p.kill()
    out, _ = p.communicate()
    return out


def spawn_and_watch(cmd, wait, label):
    """Run cmd with a wall-clock budget. Returns (hung, rc).
    On hang: print a marker and kill the process."""
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    try:
        p.wait(timeout=wait)
        return False, p.returncode
    except subprocess.TimeoutExpired:
        print("  HUNG after %ds (%s, pid %d)" % (wait, label, p.pid))
        p.kill()
        p.wait()
        return True, None


def hang_debug(test_arg, cfgs_override, hang_all):
    """M1 triage: reproduce a test's hanging configs and capture the hang point.

    Two phases per config: (1) a plain run confirms the hang and reports it;
    (2) a re-run under gdb (gdb as parent, ptrace-safe under yama) interrupts
    the hung inferior and prints bt + source line + pc disasm. Stops after the
    first hang unless --hang-all. AOT binaries are kept under
    /tmp/torture_work/hangdebug/<tid>/ for manual gdb/objdump.
    """
    if not shutil.which("gdb"):
        sys.exit("--hang-debug requires gdb on PATH")
    all_tests = sorted(
        str(p.relative_to(CORPUS))
        for p in list((CORPUS / "execute").rglob("*.c"))
        + list((CORPUS / "compile").glob("*.c")))
    matches = [t for t in all_tests if t == test_arg or test_arg in t]
    if not matches:
        sys.exit("no c-torture test matches %r" % test_arg)
    if len(matches) > 1:
        sys.exit("ambiguous %r: %s" % (test_arg, ", ".join(matches[:10])))
    rel = matches[0]
    path = CORPUS / rel
    d = parse_directives(path)
    in_execute = rel.startswith("execute/")
    is_run = (d["do"] == "run") if d["do"] else in_execute

    probe_cfgs = (["O%d-%s" % (l, m) for l in RUN_LEVELS for m in RUN_MODES]
                  if is_run else ["O%d-aot" % l for l in COMPILE_LEVELS])
    cfgs = list(cfgs_override)
    if not cfgs and BASELINE_PATH.exists():
        base = json.loads(BASELINE_PATH.read_text())["tests"].get(rel)
        if base:
            cfgs = sorted(c for c, v in base.get("configs", {}).items()
                          if v == "TIMEOUT")
            if cfgs:
                print("baseline TIMEOUT configs: %s" % ",".join(cfgs))
            else:
                print("baseline class %s (no TIMEOUT configs); probing: %s"
                      % (base["class"], ",".join(probe_cfgs)))
    if not cfgs:
        cfgs = probe_cfgs
    print("hang-debug: %s (%s test) — trying: %s"
          % (rel, "run" if is_run else "compile", ",".join(cfgs)))

    hdir = WORK / "hangdebug" / hashlib.sha1(rel.encode()).hexdigest()[:12]
    hdir.mkdir(parents=True, exist_ok=True)
    hung_any = False

    def probe(cmd, wait, label):
        """Phase 1: confirm the hang. Phase 2: capture it under gdb."""
        nonlocal hung_any
        hung, rc = spawn_and_watch(cmd, wait, label)
        if not hung:
            if rc is not None:
                print("  finished rc=%d (did not hang)" % rc)
            return False
        hung_any = True
        print("  re-running under gdb to capture the hang point...")
        transcript = gdb_probe(cmd, wait)
        for line in transcript.splitlines():
            if line.strip():
                print("  " + line)
        return True

    for cfg in cfgs:
        lvl_s, mode = cfg.rsplit("-", 1)
        lvl = int(lvl_s[1:])
        fl = zc_flags(d, lvl)
        print("\n=== %s @ %s ===" % (rel, cfg))
        if is_run and mode == "aot":
            binp = hdir / ("bin_O%d" % lvl)
            bcmd = [str(ZEROC), *fl, str(path), "-o", str(binp)]
            print("  build: %s   (timeout %ds)" % (" ".join(map(str, bcmd)),
                                                    BUILD_TIMEOUT))
            hung, rc = spawn_and_watch(bcmd, BUILD_TIMEOUT, "build")
            if hung:
                hung_any = True
                print("  re-running build under gdb to capture the hang point...")
                for line in gdb_probe(bcmd, BUILD_TIMEOUT).splitlines():
                    if line.strip():
                        print("  " + line)
            elif rc != 0:
                print("  build rc=%d (not a build hang)" % rc)
            else:
                print("  run: %s   (timeout %ds)" % (binp, d["timeout"]))
                if probe([str(binp)], d["timeout"], "runtime"):
                    print("  hung binary kept: %s" % binp)
        else:
            if is_run:
                cmd = [str(ZEROC), *fl, "--run", str(path)]
                wait, label = d["timeout"], "jit build+run"
            else:
                cmd = [str(ZEROC), *fl, "-c", str(path),
                       "-o", str(hdir / "obj.o")]
                wait, label = BUILD_TIMEOUT, "compile"
            print("  cmd: %s   (timeout %ds)" % (" ".join(map(str, cmd)),
                                                 wait))
            probe(cmd, wait, label)
        if hung_any and not hang_all:
            print("\n(first hang captured; re-run with --hang-all for the rest)")
            break
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--only",
                    help="substring filter on test path (comma-separated for "
                         "multiple patterns, e.g. pr109386,mzero4,fp-cmp-7)")
    ap.add_argument("--chunk", type=str.lower, choices=["m1", "m2", "m3"],
                    help="filter tests by burn-down chunk: "
                         "m1 = TIMEOUTs, "
                         "m2 = partial MISCOMPILEs (pass in >=1 config), "
                         "m3 = all-config MISCOMPILEs")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--quick", action="store_true",
                    help="-O1/-O2, JIT only (dev iteration)")
    ap.add_argument("--parallel", type=int, default=16)
    ap.add_argument("--show", type=int, default=25,
                    help="max issues to print in the report")
    ap.add_argument("--only-class", metavar="CLASS",
                    help="only re-run tests whose baseline class is CLASS "
                         "(e.g. TIMEOUT = the M1 chunk)")
    ap.add_argument("--keep", action="store_true",
                    help="keep work artifacts (objects/binaries) in "
                         "/tmp/torture_work for inspection")
    ap.add_argument("--timeout", type=int, default=0, metavar="SEC",
                    help="override the run/build timeout in seconds "
                         "(defaults 5 run / 10 build)")
    ap.add_argument("--repro", action="store_true",
                    help="print the exact repro command for each failing "
                         "config in the report")
    ap.add_argument("--hang-debug", metavar="TEST",
                    help="standalone: reproduce a test's hanging configs and "
                         "gdb-attach when one hangs (M1 triage); no sweep, "
                         "no baseline update")
    ap.add_argument("--hang-config", metavar="CFG", action="append",
                    help="force a config for --hang-debug (e.g. O2-jit)")
    ap.add_argument("--hang-all", action="store_true",
                    help="--hang-debug: keep going after the first hang")
    args = ap.parse_args()

    global RUN_LEVELS, RUN_MODES, KEEP, RUN_TIMEOUT, BUILD_TIMEOUT
    if args.quick:
        RUN_LEVELS = [1, 2]
        RUN_MODES = ["jit"]
    KEEP = bool(args.keep)
    if args.timeout > 0:
        RUN_TIMEOUT = args.timeout
        BUILD_TIMEOUT = args.timeout

    if not ZEROC.exists():
        sys.exit("build/bin/zerocc missing; run `make build/bin/zerocc` first")

    if args.hang_debug:
        hang_debug(args.hang_debug, args.hang_config or [], args.hang_all)
        return

    is_partial = bool(args.only or args.limit or args.quick or args.only_class or args.chunk)
    if args.update_baseline and is_partial:
        sys.exit("--update-baseline requires a full run (no --only/--limit/--quick/--only-class/--chunk)")

    tests = sorted(
        str(p.relative_to(CORPUS))
        for p in list((CORPUS / "execute").rglob("*.c"))
        + list((CORPUS / "compile").glob("*.c")))
    if args.only:
        patterns = [p.strip() for p in args.only.split(",") if p.strip()]
        tests = [t for t in tests if any(p in t for p in patterns)]
    if args.only_class:
        if not BASELINE_PATH.exists():
            sys.exit("--only-class requires an existing baseline")
        cls = args.only_class.upper()
        base = json.loads(BASELINE_PATH.read_text())["tests"]
        tests = [t for t in tests if base.get(t, {}).get("class") == cls]
    if args.chunk:
        if not BASELINE_PATH.exists():
            sys.exit("--chunk requires an existing baseline")
        base = json.loads(BASELINE_PATH.read_text())["tests"]
        if args.chunk == "m1":
            tests = [t for t in tests if base.get(t, {}).get("class") == "TIMEOUT"]
        elif args.chunk == "m2":
            def is_m2(t):
                b = base.get(t, {})
                if b.get("class") != "MISCOMPILE":
                    return False
                cfgs = b.get("configs", {})
                return any(r == "OK" for r in cfgs.values()) and any(r != "OK" for r in cfgs.values())
            tests = [t for t in tests if is_m2(t)]
        elif args.chunk == "m3":
            def is_m3(t):
                b = base.get(t, {})
                if b.get("class") != "MISCOMPILE":
                    return False
                cfgs = b.get("configs", {})
                return len(cfgs) > 0 and all(r != "OK" for r in cfgs.values())
            tests = [t for t in tests if is_m3(t)]
    if args.limit:
        tests = tests[: args.limit]

    t0 = time.time()
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.parallel) as ex:
        for res in ex.map(run_one, tests):
            results[res["path"]] = res
    elapsed = time.time() - t0

    totals = {}
    for r in results.values():
        totals[r["class"]] = totals.get(r["class"], 0) + 1

    print("c-torture sweep: %d tests in %.1fs (%s, parallel=%d)" % (
        len(tests), elapsed, "quick" if args.quick else "full", args.parallel))
    for k in sorted(totals, key=lambda x: (-totals[x], x)):
        print("  %-16s %5d" % (k, totals[k]))

    if KEEP:
        print("\nwork artifacts kept under %s (obj_<tid>_O<lvl>.o, "
              "bin_<tid>_O<lvl>)" % WORK)

    for k in ("MISCOMPILE", "REJECT", "TIMEOUT"):
        items = [(p, r) for p, r in sorted(results.items())
                 if r["class"] == k]
        if not items:
            continue
        print("\n%s — %d:" % (k, len(items)))
        for p, r in items[: args.show]:
            print("  %s  %s" % (p, r.get("note", "")))
            if args.repro:
                d = parse_directives(CORPUS / p)
                bad = sorted(c for c, v in r["configs"].items()
                             if v in ("MISCOMPILE", "REJECT", "TIMEOUT"))
                for cfg in bad:
                    print("      %s: %s" % (cfg, repro_cmd(p, d, cfg)))
        if len(items) > args.show:
            print("  ... and %d more" % (len(items) - args.show))

    compact = {
        p: {"class": r["class"], "configs": r["configs"], "note": r["note"]}
        for p, r in results.items()
    }

    is_partial = bool(args.only or args.limit or args.quick or args.only_class or args.chunk)
    if args.update_baseline or (not BASELINE_PATH.exists() and not is_partial):
        if is_partial:
            sys.exit("--update-baseline requires a full run (no --only/--limit/--quick/--only-class/--chunk)")
        BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "corpus_commit": "gcc-mirror/master@442eb35f4d2ed59a7977bf4f93a2e2545a5b451c",
            "generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "totals": totals,
            "tests": compact,
        }
        BASELINE_PATH.write_text(json.dumps(payload, indent=1, sort_keys=True))
        action = "written" if args.update_baseline else "recorded initial"
        print("\nbaseline %s at %s" % (action, BASELINE_PATH.relative_to(REPO)))
        return

    if not BASELINE_PATH.exists():
        print("\n(partial run; no baseline yet at %s)" %
              BASELINE_PATH.relative_to(REPO))
        return

    base = json.loads(BASELINE_PATH.read_text())["tests"]
    regress = []
    for p, r in results.items():
        b = base.get(p)
        if b is None:
            continue
        if b["class"] == "PASS" and r["class"] in ("MISCOMPILE", "REJECT"):
            regress.append("%s: PASS -> %s (%s)" % (
                p, r["class"], r.get("note", "")))
    base_bad = sum(1 for b in base.values()
                   if b["class"] in ("MISCOMPILE", "REJECT"))
    now_bad = sum(1 for r in results.values()
                  if r["class"] in ("MISCOMPILE", "REJECT"))
    if regress or now_bad > base_bad:
        print("\nREGRESSION vs baseline: %d bad (baseline %d)" % (
            now_bad, base_bad))
        for line in regress[: args.show]:
            print("  " + line)
        sys.exit(1)
    print("\ngate OK: no regressions vs baseline (%d bad, baseline %d)"
          % (now_bad, base_bad))


if __name__ == "__main__":
    main()
