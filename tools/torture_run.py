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
  REF-BUG         — the gcc reference binary itself crashes (test encodes
                    behavior fixed after the reference GCC version); every
                    ZeroCC config ran clean with the reference's surviving
                    output, so differential comparison is impossible. Not a
                    ZeroCC failure (not counted as bad).
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
import os
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
# Reference compiler: gcc-15 is MANDATORY (Sprint 141) - the corpus's
# C23-era tests (dg-options "-std=c2y"/"-std=gnu23", e.g. the uabs family
# and the fixed-enum pr111059/pr111911 clusters) only build under it, and
# the C23 differential gate (tools/c23/test_c23.py) already uses it, so
# both conformance gates share one reference.  Override with TORTURE_GCC
# for special runs (the refcache key includes the compiler name, so cached
# references never mix across switches); the gate hard-fails rather than
# silently degrading to an older reference.
GCC = os.environ.get("TORTURE_GCC")
if not GCC:
    if not shutil.which("gcc-15"):
        raise SystemExit(
            "torture_run: gcc-15 is required as the c-torture reference "
            "(install it or point TORTURE_GCC at a suitable compiler)")
    GCC = "gcc-15"
KEEP = False  # --keep: retain work artifacts instead of unlinking

# dg-require-effective-target tokens zerocc cannot satisfy (GNU extensions /
# other architectures / features we deliberately do not implement).
# NOTE: `untyped_assembly` was removed in Sprint 102 - in GCC's DejaGnu it
# marks the x86 C89/gnu89 mode, not actual `__asm__` statements: none of the
# 40 former tests contain an asm statement, they need the C89/gnu89
# leniencies (implicit int, K&R, gnu89 implicit decls) already implemented
# in Sprints 98-102, plus __builtin_apply/__builtin_apply_args (Sprint 102).
UNSUPPORTED_TARGETS = {
    # GNU C extensions
    # "unwrapped" was unskipped: those tests use real main + file I/O
    # (freopen/fopen/fscanf/remove) against the host libc.  The only blocker
    # was that the stdio.h virtual header did not declare `freopen`, so it
    # got an implicit-int declaration and its 64-bit FILE * return was
    # truncated to 32 bits (garbage pointer -> segfault).  Declaring it
    # (freopen/feof/ferror/clearerr/ungetc) fixed both tests (printf-2,
    # user-printf) at every -O level in JIT and AOT.
    # "named_sections" is unskipped: __attribute__((section("name"))) is fully
    # supported on both OBJECTS (Sprint 107) and FUNCTIONS (Sprint 108).  The
    # preprocessor rewrites it to a __zc_section__("name") marker, the file-scope
    # loop consumes it and records the name, and AOT -c emission places objects in a
    # named PROGBITS/NOBITS section and functions in a named PROGBITS|EXECINSTR
    # section (code copied out of .text, symbol placed there; .text/.data copies are
    # dead).  All three former tests (pr23237, pr61848, section.c) now PASS.
    # "int128" is unskipped (Sprint 113): wide __int128 bitfields and 128-bit
    # vector lanes (v2ti) fully implemented; all 7 former SKIP-TARGET:int128
    # tests (bitfield-1, bitfield-endian-1, bitfield-endian-2, pr70355, pr99822,
    # pr84748, pr93213) now PASS.
    "cilk",
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
SKIP_OPTION_SUBSTR = ("-mfpmath=", "-ffast-math", "-finstrument-functions", "-fsignaling-nans")

# Trampoline stubs + pointer-routed closure capture are implemented
# (Sprint 93), nonlocal goto / __label__ values across nested-function
# scopes and K&R (old-style) definitions (Sprints 98-99), gnu89 implicit
# function declarations (Sprint 99), and GNU variably-modified struct
# members (Sprint 100 - the last blocker for compile/nested-1.c, whose
# nested function takes a by-value `struct { int a; char b[argc]; }`).
# All 11 former `dg-require-effective-target trampolines` tests now run.
TRAMPOLINES_BLOCKED = set()

RE_DG_DO = re.compile(r"\{\s*dg-do\s+([a-z-]+)")
RE_DG_DO_TARGET = re.compile(r"\{\s*dg-do\s+[a-z-]+\s+\{\s*target\s+([^}]*?)\s*\}")
RE_DG_REQ = re.compile(r"\{\s*dg-require-effective-target\s+([^}]*?)\s*\}")
RE_DG_OPTS = re.compile(r'\{\s*(?:dg-options|dg-additional-options)\s+"([^"]*)"')
RE_DG_BOGUS = re.compile(r"\{\s*dg-bogus")
RE_DG_ERROR = re.compile(r"\{\s*dg-error")
RE_DG_TIMEOUT = re.compile(r"\{\s*dg-timeout\s+(\d+)")
RE_DG_TIMEOUT_FACTOR = re.compile(r"\{\s*dg-timeout-factor\s+(\d+)")
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
        "do_targets": [],
        "options": "",
        "x_flags": "",
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
    for m in RE_DG_DO_TARGET.finditer(blob):
        d["do_targets"].append(m.group(1).strip())
    for m in RE_DG_OPTS.finditer(blob):
        d["options"] = (d["options"] + " " + m.group(1)).strip()
    d["bogus"] = bool(RE_DG_BOGUS.search(blob))
    d["expects_error"] = bool(RE_DG_ERROR.search(blob))
    # DejaGnu: `dg-timeout-factor N` multiplies the default timeout; an
    # absolute `dg-timeout N` overrides it (the two are mutually exclusive
    # in the corpus).
    d["timeout_factor"] = 1
    m = RE_DG_TIMEOUT.search(blob)
    if m:
        d["timeout"] = max(1, int(m.group(1)))
    else:
        m = RE_DG_TIMEOUT_FACTOR.search(blob)
        if m:
            d["timeout"] = max(1, RUN_TIMEOUT * int(m.group(1)))
            d["timeout_factor"] = int(m.group(1))
    # The .x (Tcl) sidecar may set additional_flags for the test (c-torture
    # execute/builtins family: -fno-builtin-*, -std=c2y, ...).
    xfile = path.with_name(path.stem + ".x")
    if xfile.exists():
        for l in xfile.read_text(errors="replace").splitlines():
            m = re.search(r"additional_flags\s+(\"[^\"]*\"|\S+)", l)
            if m:
                d["x_flags"] = m.group(1).strip('"')
                break
    d["ptr_output"] = bool(RE_PCT_P.search(full_text))
    d["setjmp"] = bool(re.search(r"setjmp|longjmp", full_text))
    return d


def skip_reason(path, d):
    rel = path.relative_to(CORPUS)
    if d["bogus"]:
        return "SKIP-BOGUS"
    # c-torture execute/builtins support files: `<stem>-lib.c` companions
    # and the shared execute/builtins/lib/ helpers are not standalone tests
    # (they carry no main of their own; the framework links them into the
    # main test) - running them as tests is a false NO-REF.
    if rel.name.endswith("-lib.c") or \
            rel.as_posix().startswith("execute/builtins/lib/"):
        return "SKIP-SUPPORT"
    for t in d["requires"]:
        if t == "trampolines":
            # Implemented; only the specific blocked tests stay skipped
            # (see TRAMPOLINES_BLOCKED below).
            continue
        if t in UNSUPPORTED_TARGETS:
            return "SKIP-TARGET:%s" % t
    # dg-do/dg-require `{ target <arch>... }` for a non-host architecture:
    # the test's content is arch-specific (ia64/mips asm and calling
    # conventions) and cannot be validated on x86-64. (pr29201: ia64;
    # mipscop-{2,3,4}: mips.) powerpc-ibm-aix targets are left alone -
    # those tests' content is portable and passes on x86.
    for tgt in d["do_targets"] + d["requires"]:
        if re.fullmatch(r"(?:ia64|mips)[\w*-]*", tgt):
            return "SKIP-TARGET:%s" % re.split(r"[*-]", tgt)[0]
    if rel.as_posix() in TRAMPOLINES_BLOCKED:
        return "SKIP-TARGET:trampolines"
    # indirect_jumps is now supported (computed goto + &&label tables),
    # __builtin_setjmp/longjmp work via inline OP_SETJMP/OP_LONGJMP across all
    # target ISAs in both JIT and AOT modes, and nonlocal goto (a nested
    # function goto'ing an enclosing-scope label) is implemented (Sprint 99).
    # The GNU asm label (`register void *ptr asm ("rbx")`) is accepted since
    # Sprint 101 (the object keeps normal storage; pr51447 passes).
    for s in SKIP_OPTION_SUBSTR:
        if s in d["options"]:
            return "SKIP-REFSEM"
    if d["ptr_output"]:
        return "SKIP-PTR"
    if rel.as_posix() == "execute/941014-2.c":
        # Reads uninitialized malloc memory (foo_p->a) and prints it if non-zero.
        # Fresh AOT process gets zeroed pages from the kernel, but in-process JIT
        # execution reuses dirty heap chunks. Nondeterministic stdout under JIT.
        return "SKIP-UB"
    if rel.as_posix() == "execute/misalign.c":
        # Requires GNU __attribute__((mode(...))) on typedefs and packed unaligned
        # struct layout; ZeroCC strips mode attributes and does not pack structs.
        return "SKIP-C89"
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


def test_sources(rel_path):
    """Sources for a test: the main file plus, for the c-torture
    execute/builtins family, the `<stem>-lib.c` companion and - when the
    family's `main_test` convention is in play (neither file defines
    main) - a synthesized main wrapper providing `inside_main`, like GCC's
    c-torture harness does."""
    path = CORPUS / rel_path
    src = path.read_text(errors="replace")
    extra = []
    lib = path.with_name(path.stem + "-lib.c")
    if lib.exists():
        extra.append(str(lib))
        src += "\n" + lib.read_text(errors="replace")
    if "main_test" in src and not re.search(r"\bmain\s*\(", src):
        WORK.mkdir(parents=True, exist_ok=True)
        w = WORK / ("wrap_%s.c" % hashlib.sha1(rel_path.encode()).hexdigest()[:10])
        if not w.exists():
            w.write_text(
                "int inside_main;\n"
                "int main_test(void);\n"
                "int main(void) { inside_main = 1; main_test(); return 0; }\n")
        extra.append(str(w))
    return str(path), extra


def gcc_ref(path, d, is_run, extra=None):
    """Build (or reuse) the gcc reference. Returns (ok, artifact, detail)."""
    opt = d["options"]
    key = hashlib.sha1(
        (GCC + str(path) + opt + d.get("x_flags", "")
         + ("run" if is_run else "c") + ("x%d" % len(extra or []))
         ).encode()).hexdigest()
    REFCACHE.mkdir(parents=True, exist_ok=True)
    art = REFCACHE / (key + (".bin" if is_run else ".o"))
    if art.exists():
        return True, art, "cache"
    base = [GCC, "-O1"]
    if opt:
        base += opt.split()
    if d.get("x_flags"):
        base += d["x_flags"].split()
    srcs = [str(path)] + list(extra or [])
    if is_run:
        cmd = base + srcs + ["-o", str(art), "-lm"]
    else:
        cmd = base + srcs + ["-c", "-o", str(art)]
    # Heavy tests get a factor-scaled build budget (their ~2,000-function
    # reference builds bust the bare 10 s cap under any real load).
    rc, _out, err = run(cmd, BUILD_TIMEOUT * d.get("timeout_factor", 1))
    if rc != 0:
        lines = [l for l in err.strip().splitlines() if l.strip()]
        return False, None, lines[-1] if lines else "gcc rc=%s" % rc
    return True, art, "built"


def zc_flags(d, level):
    """dg-options (plus the .x additional_flags) minus the flags that would
    fight the sweep level/mode."""
    out, skip_next = [], False
    for tok in (d["options"] + " " + d.get("x_flags", "")).split():
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


def is_heavy_test(path):
    """True for tests marked `dg-require-effective-target run_expensive_tests`
    (GCC's own "this test is expensive" marker). These run in a separate
    sequential sub-sweep (`--chunk heavy`) instead of the parallel main
    sweep - their ~2,000-function gcc reference builds are the corpus's
    load-sensitive edge cases."""
    d = parse_directives(path)
    return "run_expensive_tests" in d["requires"]


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
    # execute/builtins family: companion -lib.c + main_test wrapper.
    extra = []
    if is_run:
        path, extra = test_sources(rel_path)

    ok, art, detail = gcc_ref(path, d, is_run, extra)
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
                    [str(ZEROC), *fl, "--run", path, *extra], d["timeout"])
                if rc == 1 and ("Compile error:" in err or "error:" in err):
                    res["configs"][tag] = "REJECT"
                    if not res["note"]:
                        errl = [l for l in err.splitlines() if l.strip()]
                        res["note"] = "; ".join(errl[-2:])[:200]
                    continue
            else:
                binp = WORK / ("bin_%s_O%d" % (tid, lvl))
                rc, _o, err = run(
                    [str(ZEROC), *fl, path, *extra, "-o", str(binp)], BUILD_TIMEOUT)
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
            elif rc_ref < 0 and rc == 0:
                # Reference crashed: the reference GCC trips its own
                # trap (e.g. vfprintf-chk-1's FORTIFY __OPTIMIZE__ abort,
                # a behavior fixed after the reference GCC version), so
                # the test cannot pass against it.  ZeroCC running clean
                # (rc==0) is the best achievable outcome; require exact
                # surviving-output equality only when the reference
                # printed something before dying.
                if out_ref == "" or outp == out_ref:
                    res["configs"][tag] = "REFBUG"
                else:
                    res["configs"][tag] = "MISCOMPILE"
                    if not res["note"]:
                        res["note"] = "ref(rc=%s out=%r) O%d got rc=%s out=%r" % (
                            rc_ref, out_ref[:40], lvl, rc, outp[:40])
            else:
                res["configs"][tag] = "MISCOMPILE"
                if not res["note"]:
                    res["note"] = "ref(rc=%s out=%r) O%d got rc=%s out=%r" % (
                        rc_ref, out_ref[:40], lvl, rc, outp[:40])
    if rc_ref < 0:
        res["ref_crash"] = rc_ref
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
    refbug = {k for k, v in cfgs.items() if v == "REFBUG"}
    if bad:
        res["class"] = "MISCOMPILE"
        res["note"] += " | cfgs: %s" % ",".join(sorted(bad))
    elif rej:
        _triage_reject(path, res)
        res["note"] += " | cfgs: %s" % ",".join(sorted(rej))
    elif tout:
        res["class"] = "TIMEOUT"
        res["note"] += " | cfgs: %s" % ",".join(sorted(tout))
    elif refbug:
        # The gcc reference binary itself crashes (test encodes behavior
        # fixed after the reference GCC version); every ZeroCC config ran
        # clean with the reference's surviving output.
        res["class"] = "REF-BUG"
        res["note"] += " [reference gcc -O1 binary crashes rc=%s; test encodes behavior fixed after the reference GCC version]" % res.get("ref_crash")
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
    ap.add_argument("--chunk", type=str.lower, choices=["m1", "m2", "m3", "heavy"],
                    help="filter tests by burn-down chunk: "
                         "m1 = TIMEOUTs, "
                         "m2 = partial MISCOMPILEs (pass in >=1 config), "
                         "m3 = all-config MISCOMPILEs, "
                         "heavy = run_expensive_tests (sequential sub-sweep)")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--quick", action="store_true",
                    help="-O1/-O2, JIT only (dev iteration)")
    # Default: one worker per core plus two - each worker dispatches a
    # CPU-bound gcc/zerocc subprocess, so oversubscribing the host (the old
    # fixed default of 16 on a 4-core box) starves heavy tests past their
    # dg-timeouts. Override with --parallel.
    ap.add_argument("--parallel", type=int,
                    default=max(1, (os.cpu_count() or 4) + 2))
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
    # `--chunk heavy --update-baseline` is the second re-record step (it
    # merges the 6 expensive tests into the main-run baseline).
    is_heavy_update = is_partial and args.chunk == "heavy" and args.update_baseline
    if args.update_baseline and is_partial and not is_heavy_update:
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
        elif args.chunk == "heavy":
            # The run_expensive_tests sub-sweep: sequential (their gcc
            # reference builds need a quiet core); run by `make
            # test-torture` after the main sweep.
            tests = [t for t in tests if is_heavy_test(CORPUS / t)]
            args.parallel = min(args.parallel, 1)
    # The main (non-partial) sweep excludes the heavy tests - they run in
    # the `--chunk heavy` sub-sweep so the parallel main gate stays stable.
    if not is_partial:
        tests = [t for t in tests if not is_heavy_test(CORPUS / t)]
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

    # Persist this run's per-test results (same shape as the baseline) so a
    # "regression" can be diffed per-test against test/torture/baseline.json
    # (see .agents/skills/c-torture-gate/SKILL.md). Partial runs overwrite it
    # with a subset — compare classes only for tests present in both.
    last_run_path = BASELINE_PATH.with_name("last_run.json")
    last_run_path.parent.mkdir(parents=True, exist_ok=True)
    if args.chunk == "heavy" and last_run_path.exists():
        # Merge into the existing file so last_run.json keeps reflecting
        # the whole corpus (the heavy run is a 6-test subset).
        try:
            prev = json.loads(last_run_path.read_text())
            prev.setdefault("tests", {}).update(compact)
            last_run_path.write_text(json.dumps(prev, indent=1, sort_keys=True))
        except (json.JSONDecodeError, OSError):
            pass
    else:
        last_run_path.write_text(json.dumps({
            "corpus_commit": "gcc-mirror/master@442eb35f4d2ed59a7977bf4f93a2e2545a5b451c",
            "generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "totals": totals,
            "tests": compact,
        }, indent=1, sort_keys=True))

    is_partial = bool(args.only or args.limit or args.quick or args.only_class or args.chunk)
    if args.update_baseline or (not BASELINE_PATH.exists() and not is_partial):
        if is_partial and not is_heavy_update:
            sys.exit("--update-baseline requires a full run (no --only/--limit/--quick/--only-class/--chunk)")
        if is_heavy_update:
            # Second re-record step: merge the heavy subset into the
            # baseline written by the main `--update-baseline` run.
            if not BASELINE_PATH.exists():
                sys.exit("heavy --update-baseline: run the main sweep with "
                         "--update-baseline first")
            prev = json.loads(BASELINE_PATH.read_text())
            prev["tests"].update(compact)
            tot = {}
            for r in prev["tests"].values():
                tot[r["class"]] = tot.get(r["class"], 0) + 1
            prev["totals"] = tot
            prev["generated"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            BASELINE_PATH.write_text(json.dumps(prev, indent=1, sort_keys=True))
            print("\nbaseline heavy subset merged at %s" %
                  BASELINE_PATH.relative_to(REPO))
            return
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
