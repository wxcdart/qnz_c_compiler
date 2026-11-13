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
import sys
from pathlib import Path

# Make the tools package importable so we can use find_repo_root.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = find_repo_root()
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
        # Cross-TU pointer initializer `int *p = &g;` (kind-2 data reloc,
        # resolved against the merged registry in JIT mode).
        expect_program("crossptr2", [src("ptr_main.c"), src("ptr_impl.c")])

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

        # -- dependency generation (-MMD / -MF / -MT) ---------------------------
        dep_dir = os.path.join(tmp, "dep_test")
        os.makedirs(os.path.join(dep_dir, "include"), exist_ok=True)
        with open(os.path.join(dep_dir, "include", "dep_hdr.h"), "w") as f:
            f.write("#ifndef DEP_HDR_H\n#define DEP_HDR_H\nint answer = 42;\n#endif\n")
        with open(os.path.join(dep_dir, "main.c"), "w") as f:
            f.write('#include "dep_hdr.h"\n#include "dep_hdr.h"\nint main(void) { return answer; }\n')

        # 1. Default -MMD: creates main.o and main.d in cwd, target main.o
        rc, out, err = run_zerocc(["-MMD", "-Iinclude", "-c", "main.c"], cwd=dep_dir)
        d_path = os.path.join(dep_dir, "main.d")
        o_path = os.path.join(dep_dir, "main.o")
        ok = rc == 0 and os.path.exists(d_path) and os.path.exists(o_path)
        d_content = ""
        if ok:
            with open(d_path) as f:
                d_content = f.read()
            ok = "main.o:" in d_content and "main.c" in d_content and "include/dep_hdr.h" in d_content
            # Deduplication: header should only appear once
            ok = ok and d_content.count("dep_hdr.h") == 1
        check("dep-mmd-default", ok, f"rc={rc} err={err!r} d={d_content!r}")

        # 2. -MMD with -o: creates custom.d and custom.o, target custom.o
        os.unlink(d_path)
        os.unlink(o_path)
        rc, out, err = run_zerocc(["-MMD", "-Iinclude", "-c", "main.c", "-o", "custom.o"], cwd=dep_dir)
        d_path = os.path.join(dep_dir, "custom.d")
        o_path = os.path.join(dep_dir, "custom.o")
        ok = rc == 0 and os.path.exists(d_path) and os.path.exists(o_path)
        d_content = ""
        if ok:
            with open(d_path) as f:
                d_content = f.read()
            ok = "custom.o:" in d_content and "main.c" in d_content and "include/dep_hdr.h" in d_content
        check("dep-mmd-output", ok, f"rc={rc} err={err!r} d={d_content!r}")

        # 3. -MMD with -MF: writes to custom.d
        os.unlink(d_path)
        os.unlink(o_path)
        rc, out, err = run_zerocc(["-MMD", "-MF", "my_rules.d", "-Iinclude", "-c", "main.c"], cwd=dep_dir)
        d_path = os.path.join(dep_dir, "my_rules.d")
        o_path = os.path.join(dep_dir, "main.o")
        ok = rc == 0 and os.path.exists(d_path) and os.path.exists(o_path)
        d_content = ""
        if ok:
            with open(d_path) as f:
                d_content = f.read()
            ok = "main.o:" in d_content and "main.c" in d_content and "include/dep_hdr.h" in d_content
        check("dep-mmd-mf", ok, f"rc={rc} err={err!r} d={d_content!r}")

        # 4. -MMD with -MT: custom target name
        os.unlink(d_path)
        os.unlink(o_path)
        rc, out, err = run_zerocc(["-MMD", "-MT", "build/target.o", "-Iinclude", "-c", "main.c"], cwd=dep_dir)
        d_path = os.path.join(dep_dir, "main.d")
        ok = rc == 0 and os.path.exists(d_path)
        d_content = ""
        if ok:
            with open(d_path) as f:
                d_content = f.read()
            ok = "build/target.o:" in d_content and "main.c" in d_content
        check("dep-mmd-mt", ok, f"rc={rc} err={err!r} d={d_content!r}")

        # 5. -MMD with -S: default target uses the .s output basename
        os.unlink(d_path)
        rc, out, err = run_zerocc(["-MMD", "-Iinclude", "-S", "main.c"], cwd=dep_dir)
        d_path = os.path.join(dep_dir, "main.d")
        ok = rc == 0 and os.path.exists(d_path) and os.path.exists(os.path.join(dep_dir, "main.s"))
        d_content = ""
        if ok:
            with open(d_path) as f:
                d_content = f.read()
            ok = "main.s:" in d_content and "main.c" in d_content
        check("dep-mmd-asm", ok, f"rc={rc} err={err!r} d={d_content!r}")

        # -- -E preprocess-only ------------------------------------------------
        # 7. -E to stdout: includes expanded, no #include lines remain
        rc, out, err = run_zerocc(["-E", "-Iinclude", "main.c"], cwd=dep_dir)
        ok = rc == 0 and "int answer = 42;" in out and "int main(void)" in out
        ok = ok and "#include" not in out
        check("e-stdout", ok, f"rc={rc} err={err!r} out[:120]={out[:120]!r}")

        # 8. -E -o: writes the preprocessed output to a file, nothing on stdout
        rc, out, err = run_zerocc(["-E", "-Iinclude", "main.c", "-o", "main.i"], cwd=dep_dir)
        i_path = os.path.join(dep_dir, "main.i")
        ok = rc == 0 and os.path.exists(i_path) and out == ""
        if ok:
            with open(i_path) as f:
                i_content = f.read()
            ok = "int answer = 42;" in i_content and "int main(void)" in i_content
        check("e-output-file", ok, f"rc={rc} err={err!r}")

        # 9. -E -MMD: dep file derived from the input name, target <base>.o
        os.unlink(d_path) if os.path.exists(d_path) else None
        rc, out, err = run_zerocc(["-E", "-MMD", "-Iinclude", "main.c"], cwd=dep_dir)
        d_path = os.path.join(dep_dir, "main.d")
        ok = rc == 0 and os.path.exists(d_path)
        d_content = ""
        if ok:
            with open(d_path) as f:
                d_content = f.read()
            ok = "main.o:" in d_content and "main.c" in d_content and "include/dep_hdr.h" in d_content
        check("e-mmd", ok, f"rc={rc} err={err!r} d={d_content!r}")

        # 10. -E -o with multiple inputs is an error (like gcc)
        rc, out, err = run_zerocc(["-E", "-Iinclude", "-o", "both.i", "main.c", "main.c"], cwd=dep_dir)
        ok = rc != 0
        check("e-o-multi-err", ok, f"rc={rc} err={err!r}")

        # -- build-system compatibility (Sprint 52) ---------------------------
        with open(os.path.join(dep_dir, "compat.c"), "w") as f:
            f.write('#ifdef FOO\nint main(void) { return 1; }\n#else\nint main(void) { return 2; }\n#endif\n')

        # 11. ubiquitous gcc flags are accepted (harmless no-ops for zerocc)
        rc, out, err = run_zerocc(["-g", "-Wall", "-O2", "-fPIC", "-pthread", "-static",
                                   "-std=gnu11", "-m64", "-pipe", "compat.c", "--run"], cwd=dep_dir)
        ok = rc == 2
        check("compat-flags-accepted", ok, f"rc={rc} err={err!r}")

        # 12. -D/-U apply in command-line order (gcc semantics)
        rc_a, _, _ = run_zerocc(["-DFOO=1", "-UFOO", "compat.c", "--run"], cwd=dep_dir)
        rc_b, _, _ = run_zerocc(["-UFOO", "-DFOO=1", "compat.c", "--run"], cwd=dep_dir)
        ok = rc_a == 2 and rc_b == 1
        check("du-ordered", ok, f"rc_a={rc_a} rc_b={rc_b}")

        # 13. -include force-includes a header before the main source
        with open(os.path.join(dep_dir, "forced.h"), "w") as f:
            f.write("#define FORCED 1\nint forced_val(void) { return 9; }\n")
        with open(os.path.join(dep_dir, "inc_user.c"), "w") as f:
            f.write('#ifdef FORCED\nint main(void) { return forced_val() - 8; }\n#else\nint main(void) { return 3; }\n#endif\n')
        rc, out, err = run_zerocc(["-include", "forced.h", "inc_user.c", "--run"], cwd=dep_dir)
        ok = rc == 1
        check("include-force", ok, f"rc={rc} err={err!r}")

        # 14. -include of a missing file is a hard error
        rc, out, err = run_zerocc(["-include", "nope.h", "inc_user.c"], cwd=dep_dir)
        ok = rc != 0 and "No such file" in err
        check("include-missing-err", ok, f"rc={rc} err={err!r}")

        # 15. -isystem works like -I
        rc, out, err = run_zerocc(["-isystem", "include", "-c", "main.c", "-o", "isys.o"], cwd=dep_dir)
        ok = rc == 0 and os.path.exists(os.path.join(dep_dir, "isys.o"))
        check("isystem", ok, f"rc={rc} err={err!r}")

        # 16. unknown flags still error
        rc, out, err = run_zerocc(["-Ztotally-bogus", "main.c"], cwd=dep_dir)
        ok = rc != 0
        check("unknown-flag-err", ok, f"rc={rc} err={err!r}")

        # 6. Full combo: -MMD -MT target.o -MF combo.d -o my.o + make verification
        os.unlink(d_path)
        rc, out, err = run_zerocc(["-MMD", "-MT", "target.o", "-MF", "combo.d", "-Iinclude", "-c", "main.c", "-o", "my.o"], cwd=dep_dir)
        d_path = os.path.join(dep_dir, "combo.d")
        o_path = os.path.join(dep_dir, "my.o")
        ok = rc == 0 and os.path.exists(d_path) and os.path.exists(o_path)
        d_content = ""
        if ok:
            with open(d_path) as f:
                d_content = f.read()
            ok = "target.o:" in d_content and "main.c" in d_content and "include/dep_hdr.h" in d_content
            with open(os.path.join(dep_dir, "Makefile"), "w") as mf:
                mf.write("include combo.d\nall:\n\t@echo make-ok\n")
            make_res = subprocess.run(["make", "-f", "Makefile", "all"], cwd=dep_dir, capture_output=True, text=True)
            ok = ok and make_res.returncode == 0 and "make-ok" in make_res.stdout
        check("dep-mmd-combo-and-make", ok, f"rc={rc} err={err!r} d={d_content!r}")

    # Sprint 153 (Host Runtime #11 Phase 1): the driver's own dlfcn must
    # resolve JIT far-call symbols from the pre-baked embedded table
    # (tools/embed_symtab.py), with and without /proc/self/exe access.
    # JIT-only: AOT binaries link zero_rt, which carries no dlfcn.
        for mode in (None, "embedded", "proc"):
            env = dict(os.environ)
            if mode:
                env["ZERO_RT_DL"] = mode
            else:
                env.pop("ZERO_RT_DL", None)
            p = subprocess.run([BIN, src("dlopen_embedded.c"), "--run"],
                               capture_output=True, text=True, env=env, timeout=60)
            label = mode or "default"
            ok = p.returncode == 0 and "dlopen-embedded ok" in p.stdout
            check(f"dlopen-embedded-{label}", ok,
                  f"rc={p.returncode} out={p.stdout!r} err={p.stderr!r}")

        # DWARF debug information (-g) tests
        dwarf_src = os.path.join(tmp, "dwarf_test.c")
        with open(dwarf_src, "w") as f:
            f.write(
                "int global_val = 42;\n"
                "int square(int a) {\n"
                "    int b = a * a;\n"
                "    return b;\n"
                "}\n"
                "int main(void) {\n"
                "    int x = 5;\n"
                "    int y = square(x);\n"
                "    return y;\n"
                "}\n"
            )

        # 1. -g -c produces .debug_* sections
        dwarf_obj = os.path.join(tmp, "dwarf_test.o")
        rc, out, err = run_zerocc(["-g", "-c", dwarf_src, "-o", dwarf_obj])
        readelf = shutil.which("readelf")
        if rc == 0 and readelf:
            p = subprocess.run([readelf, "-S", dwarf_obj], capture_output=True, text=True)
            has_sections = (".debug_info" in p.stdout and ".debug_line" in p.stdout and
                            ".debug_abbrev" in p.stdout and ".debug_str" in p.stdout)
            check("dwarf-sections-present", has_sections, p.stdout)
        else:
            check("dwarf-sections-present", rc == 0, err)

        # 2. -g0 produces no debug sections
        nog_obj = os.path.join(tmp, "dwarf_nog.o")
        rc, out, err = run_zerocc(["-g0", "-c", dwarf_src, "-o", nog_obj])
        if rc == 0 and readelf:
            p = subprocess.run([readelf, "-S", nog_obj], capture_output=True, text=True)
            no_sections = (".debug_info" not in p.stdout and ".debug_line" not in p.stdout)
            check("dwarf-g0-sections-absent", no_sections, p.stdout)
        else:
            check("dwarf-g0-sections-absent", rc == 0, err)

        # 3. Interactive GDB source-level debugging test
        gdb = shutil.which("gdb")
        if gdb:
            dwarf_exe = os.path.join(tmp, "dwarf_test_exe")
            rc, out, err = run_zerocc(["-g", dwarf_src, "-o", dwarf_exe])
            if rc == 0:
                gdb_cmd = [
                    gdb, "--batch",
                    "-ex", "b main",
                    "-ex", "r",
                    "-ex", "n",
                    "-ex", "print x",
                    "-ex", "b square",
                    "-ex", "c",
                    "-ex", "print a",
                    "-ex", "bt",
                    "-ex", "print global_val",
                    "-ex", "c",
                    dwarf_exe
                ]
                gp = subprocess.run(gdb_cmd, capture_output=True, text=True, timeout=30)
                gdb_ok = (gp.returncode == 0 and
                          "$1 = 5" in gp.stdout and
                          "$2 = 5" in gp.stdout and
                          "$3 = 42" in gp.stdout and
                          "#0  square" in gp.stdout and
                          "in main ()" in gp.stdout)
                check("dwarf-gdb-session", gdb_ok, f"gdb out={gp.stdout!r}")
            else:
                check("dwarf-gdb-session", False, f"compile failed: {err}")

        # 4. Cross-target DWARF emission
        for tgt in ("aarch64", "riscv64"):
            tgt_obj = os.path.join(tmp, f"dwarf_{tgt}.o")
            rc, out, err = run_zerocc(["--target", tgt, "-g", "-c", dwarf_src, "-o", tgt_obj])
            if rc == 0 and readelf:
                p = subprocess.run([readelf, "-S", tgt_obj], capture_output=True, text=True)
                has_secs = (".debug_info" in p.stdout and ".debug_line" in p.stdout)
                check(f"dwarf-cross-{tgt}", has_secs, p.stdout)
            else:
                check(f"dwarf-cross-{tgt}", rc == 0, err)
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
