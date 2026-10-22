#!/usr/bin/env python3
"""Driver-level multi-file tests (T1) for the ZeroCC compiler binary.

Exercises the multi-input driver paths that the in-process unit suite
cannot reach:
  - multi-TU JIT execution (--run / default)
  - multi-TU AOT link (-o) + execution of the result
  - -c / -S: one object / assembly output per input
  - error cases: duplicate definition, missing main, -o with -c + multi

Run:  python3 tools/driver_tests.py     (requires `make build/bin/zerocc`)
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(ROOT, "test", "driver")
BIN = os.path.join(ROOT, "build", "bin", "zerocc")


def run_zerocc(args, cwd=None, timeout=120):
    p = subprocess.run([BIN] + args, capture_output=True, text=True,
                       cwd=cwd, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def src(name):
    return os.path.join(DRIVER, name)


def run_exe(path, timeout=30):
    return subprocess.run([path], capture_output=True, text=True, timeout=timeout)


RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok))
    tag = "ok  " if ok else "FAIL"
    line = f"{tag} {name}"
    if detail and not ok:
        line += f"  ({detail})"
    print(line)


def expect_program(name, sources, expect_out=None, expect_rc=0):
    """Compile with both JIT and AOT and run the program."""
    exe = f"/tmp/zerocc_driver_{name}"
    rc, out, err = run_zerocc(sources + ["--run"])
    if expect_rc is not None and expect_rc != 0:
        check(name + "-jit", rc != 0, f"rc={rc} out={out!r} err={err!r}")
    else:
        ok = rc == expect_rc
        if expect_out is not None:
            ok = ok and expect_out in out
        check(name + "-jit", ok, f"rc={rc} out={out!r} err={err!r}")

    rc, out, err = run_zerocc(sources + ["-o", exe])
    ok = rc == 0
    if ok:
        r = run_exe(exe)
        ok = r.returncode == expect_rc
        if ok and expect_out is not None:
            ok = expect_out in r.stdout
        if ok:
            ok = (r.returncode == expect_rc)
        detail = f"link rc={rc} err={err!r}"
        if ok:
            detail = f"run rc={r.returncode} out={r.stdout!r} err={r.stderr!r}"
    check(name + "-aot", ok, detail)
    if os.path.exists(exe):
        os.unlink(exe)


def main():
    if not os.path.exists(BIN):
        print(f"error: {BIN} not found - run `make` first", file=sys.stderr)
        return 1

    tmp = tempfile.mkdtemp(prefix="zerocc_driver_")
    try:
        # -- multi-TU execution ------------------------------------------------
        expect_program("hello2", [src("hello_main.c"), src("hello_impl.c")],
                       expect_out="result=42")
        expect_program("global2", [src("glob_main.c"), src("glob_impl.c")])
        expect_program("float2", [src("fglob_main.c"), src("fglob_impl.c")])
        expect_program("chain3", [src("chain_top.c"), src("chain_mid.c"),
                                  src("chain_leaf.c")])
        expect_program("str2", [src("str_main.c"), src("str_impl.c")],
                       expect_out="hello-from-tu2 42")
        # Unnamed parameters in prototypes (C99 6.7.6.3) - regression: the
        # file-scope parser dropped unnamed params, so `int anon(int, int *);`
        # became a 0-parameter prototype and the call was rejected.
        expect_program("anonparams", [src("anon_main.c"), src("anon_impl.c")])

        # -- -c: one object per input ------------------------------------------
        # (Derived output names are placed next to the input file, so run on
        # copies in the scratch dir.)
        for f in ("hello_main.c", "hello_impl.c"):
            shutil.copyfile(src(f), os.path.join(tmp, f))
        rc, out, err = run_zerocc(["-c", "hello_main.c", "hello_impl.c"], cwd=tmp)
        ok = rc == 0 and os.path.exists(os.path.join(tmp, "hello_main.o")) \
            and os.path.exists(os.path.join(tmp, "hello_impl.o"))
        check("c-multi", ok, f"rc={rc} err={err!r} ls={os.listdir(tmp)!r}")
        if ok:
            # The per-input objects must link (with the system linker) to a
            # working program.
            exe = os.path.join(tmp, "c_linked")
            lr = subprocess.run(
                ["gcc", "hello_main.o", "hello_impl.o", "-o", exe],
                cwd=tmp, capture_output=True, text=True)
            ok2 = lr.returncode == 0
            if ok2:
                rr = run_exe(exe)
                ok2 = rr.returncode == 0 and "result=42" in rr.stdout
            check("c-multi-links", ok2, f"gcc: {lr.stderr!r}")

        # -- -S: one assembly file per input ------------------------------------
        for f in ("hello_main.s", "hello_impl.s"):
            if os.path.exists(os.path.join(tmp, f)):
                os.unlink(os.path.join(tmp, f))
        rc, out, err = run_zerocc(["-S", "hello_main.c", "hello_impl.c"], cwd=tmp)
        ok = rc == 0 and os.path.exists(os.path.join(tmp, "hello_main.s")) \
            and os.path.exists(os.path.join(tmp, "hello_impl.s"))
        check("s-multi", ok, f"rc={rc} err={err!r} ls={os.listdir(tmp)!r}")

        # -- error cases ---------------------------------------------------------
        rc, out, err = run_zerocc(
            [src("dup_a.c"), src("dup_b.c"), "--run"])
        check("dup-def", rc != 0 and "multiple definition" in err,
              f"rc={rc} err={err!r}")

        rc, out, err = run_zerocc(
            [src("nomain_a.c"), src("nomain_b.c"), "--run"])
        check("no-main", rc != 0 and "undefined reference to 'main'" in err,
              f"rc={rc} err={err!r}")

        rc, out, err = run_zerocc(
            ["-c", src("hello_main.c"), src("hello_impl.c"),
             "-o", os.path.join(tmp, "one.o")])
        check("c-o-multi", rc != 0 and "cannot specify -o" in err,
              f"rc={rc} err={err!r}")

        # single input keeps the -o semantics
        rc, out, err = run_zerocc(
            ["-c", src("hello_impl.c"), "-o", os.path.join(tmp, "impl.o")])
        ok = rc == 0 and os.path.exists(os.path.join(tmp, "impl.o"))
        check("c-o-single", ok, f"rc={rc} err={err!r}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    failed = [n for n, ok in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} driver tests passed")
    if failed:
        print("failed: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
