#!/usr/bin/env python3
"""Own-linker (T2) differential tests for the ZeroCC compiler.

Links zerocc-emitted .o files (and .a archives) with ZeroCC's own linker
(`--zerocc-link`) into a directly-executable static ET_EXEC, then:
  - runs the result and checks exit code
  - differentially compares against the system-linker reference
    (same objects, gcc -no-pie link)
  - for aarch64: links `--target aarch64` objects, runs under
    qemu-aarch64-static, compares against musl-gcc-aarch64 -static
  - exercises the error paths: multiple definition, missing main,
    undefined symbol, unsupported-ISA target

Run:  python3 tools/linker_tests.py     (requires `make build/bin/zerocc`)
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LDIR = os.path.join(ROOT, "test", "linker")
BIN = os.path.join(ROOT, "build", "bin", "zerocc")

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok))
    tag = "ok  " if ok else "FAIL"
    line = f"{tag} {name}"
    if detail and not ok:
        line += f"  ({detail})"
    print(line)


def src(name):
    return os.path.join(LDIR, name)


def run_exe(path, timeout=30):
    p = subprocess.run([path], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout


def zc(args, timeout=120):
    p = subprocess.run([BIN] + args, capture_output=True, text=True,
                       timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def compile_objs(workdir, names):
    """Compile each source to <workdir>/<stem>.o with zerocc. -> [paths]"""
    objs = []
    for n in names:
        o = os.path.join(workdir, n[:-2] + ".o")
        rc, out, err = zc(["-c", src(n), "-o", o])
        if rc != 0:
            raise RuntimeError(f"compile {n} failed: {err}")
        objs.append(o)
    return objs


def link_own(workdir, objs, archives=(), exe="app_own", no_rt=False):
    out = os.path.join(workdir, exe)
    args = list(objs) + list(archives) + ["--zerocc-link", "-o", out]
    if no_rt:
        args.append("--no-zerocc-rt")
    rc, o, e = zc(args)
    if rc != 0:
        return None, e
    return out, ""


def link_system(workdir, objs, exe="app_sys"):
    """Reference: the same objects linked by the system linker (gcc)."""
    out = os.path.join(workdir, exe)
    p = subprocess.run(["gcc", "-no-pie"] + list(objs) + ["-o", out],
                       capture_output=True, text=True)
    if p.returncode != 0:
        return None, p.stderr
    return out, ""


def case_link_run(name, sources, expect_rc=0, diff=True, archives_of=(),
                  gcc_sources=()):
    """Compile -> own-link -> run; optionally compare against gcc link.

    archives_of: indices into `sources` whose objects are packed into a
    .a that the linker must extract members from.
    gcc_sources: indices into `sources` compiled with the system gcc
    instead of zerocc (used to exercise linker paths that zerocc's own
    emitter does not produce, e.g. .data relocations against UND).
    """
    with tempfile.TemporaryDirectory(prefix="zerocc_t2_") as work:
        objs = []
        for i, n in enumerate(sources):
            o = os.path.join(work, n[:-2] + ".o")
            if i in gcc_sources:
                p = subprocess.run(["gcc", "-c", src(n), "-o", o],
                                   capture_output=True, text=True)
                if p.returncode != 0:
                    raise RuntimeError(f"gcc {n} failed: {p.stderr}")
            else:
                rc, out, err = zc(["-c", src(n), "-o", o])
                if rc != 0:
                    raise RuntimeError(f"compile {n} failed: {err}")
            objs.append(o)
        direct, ar_members = [], []
        for i, o in enumerate(objs):
            (ar_members if i in archives_of else direct).append(o)
        archives = []
        if ar_members:
            lib = os.path.join(work, "libmembers.a")
            p = subprocess.run(["ar", "rcs", lib] + ar_members,
                               capture_output=True, text=True)
            if p.returncode != 0:
                check(name, False, f"ar failed: {p.stderr}")
                return
            archives.append(lib)

        exe, err = link_own(work, direct, archives)
        if exe is None:
            check(name, False, f"link failed: {err.strip()}")
            return
        rc, out = run_exe(exe)
        ok = rc == expect_rc
        if not ok:
            check(name, False, f"rc={rc} out={out!r} (want rc={expect_rc})")
            return

        if diff:
            sexe, serr = link_system(work, objs)
            if sexe is None:
                check(name, False, f"system link failed: {serr.strip()}")
                return
            src_rc, sout = run_exe(sexe)
            if (sout, src_rc) != (out, rc):
                check(name, False,
                      f"system rc={src_rc} out={sout!r} vs own "
                      f"rc={rc} out={out!r}")
                return
        check(name, True)


def case_link_error(name, sources, want_substr, no_rt=False):
    with tempfile.TemporaryDirectory(prefix="zerocc_t2_") as work:
        objs = compile_objs(work, sources)
        exe, err = link_own(work, objs, no_rt=no_rt)
        if exe is not None:
            check(name, False, "link unexpectedly succeeded")
            return
        if want_substr not in err:
            check(name, False, f"error {err!r} missing {want_substr!r}")
            return
        check(name, True)


def case_aarch64_run(name, sources, expect_rc=0, archives_of=()):
    """Cross-link AArch64 objects with the own linker, run under
    qemu-aarch64-static, and compare against a musl-gcc-aarch64 -static
    reference (same differential discipline as the isa_qemu battery).
    Skips (passing) when the QEMU/musl toolchain is unavailable."""
    qemu = shutil.which("qemu-aarch64-static")
    refcc = shutil.which("musl-gcc-aarch64")
    if not qemu or not refcc:
        check(name, True, "skipped: qemu-aarch64-static / musl-gcc-aarch64 absent")
        return
    with tempfile.TemporaryDirectory(prefix="zerocc_t2_a64_") as work:
        objs = []
        for n in sources:
            o = os.path.join(work, n[:-2] + ".o")
            rc, out, err = zc(["--target", "aarch64", "-c", src(n), "-o", o])
            if rc != 0:
                check(name, False, f"compile {n} failed: {err}")
                return
            objs.append(o)
        direct, ar_members = [], []
        for i, o in enumerate(objs):
            (ar_members if i in archives_of else direct).append(o)
        archives = []
        if ar_members:
            lib = os.path.join(work, "libmembers.a")
            p = subprocess.run(["ar", "rcs", lib] + ar_members,
                               capture_output=True, text=True)
            if p.returncode != 0:
                check(name, False, f"ar failed: {p.stderr}")
                return
            archives.append(lib)
        exe = os.path.join(work, "app_own")
        rc, out, err = zc(direct + archives + ["--target", "aarch64",
                                               "--zerocc-link", "-o", exe])
        if rc != 0:
            check(name, False, f"link failed: {err.strip()}")
            return
        p = subprocess.run([qemu, exe], capture_output=True, text=True,
                           timeout=30)
        if p.returncode != expect_rc:
            check(name, False,
                  f"rc={p.returncode} out={p.stdout!r} (want rc={expect_rc})")
            return
        # Reference: same sources, musl static link.
        rexe = os.path.join(work, "app_ref")
        p = subprocess.run([refcc, "-static"] + [src(n) for n in sources]
                           + ["-o", rexe], capture_output=True, text=True)
        if p.returncode != 0:
            check(name, False, f"reference build failed: {p.stderr.strip()}")
            return
        rp = subprocess.run([qemu, rexe], capture_output=True, text=True,
                            timeout=30)
        if (rp.returncode, rp.stdout) != (p.returncode, p.stdout):
            check(name, False, f"ref rc={rp.returncode} out={rp.stdout!r} "
                               f"vs own rc={p.returncode} out={p.stdout!r}")
            return
        check(name, True)


def case_riscv64_run(name, sources, expect_rc=0, archives_of=()):
    """Cross-link RISC-V64 objects with the own linker, run under
    qemu-riscv64-static, and compare against a musl-gcc-riscv64 -static
    reference (same differential discipline as the isa_qemu battery).
    Skips (passing) when the QEMU/musl toolchain is unavailable."""
    qemu = shutil.which("qemu-riscv64-static")
    refcc = shutil.which("musl-gcc-riscv64")
    if not qemu or not refcc:
        check(name, True, "skipped: qemu-riscv64-static / musl-gcc-riscv64 absent")
        return
    with tempfile.TemporaryDirectory(prefix="zerocc_t2_rv64_") as work:
        objs = []
        for n in sources:
            o = os.path.join(work, n[:-2] + ".o")
            rc, out, err = zc(["--target", "riscv64", "-c", src(n), "-o", o])
            if rc != 0:
                check(name, False, f"compile {n} failed: {err}")
                return
            objs.append(o)
        direct, ar_members = [], []
        for i, o in enumerate(objs):
            (ar_members if i in archives_of else direct).append(o)
        archives = []
        if ar_members:
            lib = os.path.join(work, "libmembers.a")
            p = subprocess.run(["ar", "rcs", lib] + ar_members,
                               capture_output=True, text=True)
            if p.returncode != 0:
                check(name, False, f"ar failed: {p.stderr}")
                return
            archives.append(lib)
        exe = os.path.join(work, "app_own")
        rc, out, err = zc(direct + archives + ["--target", "riscv64",
                                               "--zerocc-link", "-o", exe])
        if rc != 0:
            check(name, False, f"link failed: {err.strip()}")
            return
        p = subprocess.run([qemu, exe], capture_output=True, text=True,
                           timeout=30)
        if p.returncode != expect_rc:
            check(name, False,
                  f"rc={p.returncode} out={p.stdout!r} (want rc={expect_rc})")
            return
        # Reference: same sources, musl static link.
        rexe = os.path.join(work, "app_ref")
        p = subprocess.run([refcc, "-static"] + [src(n) for n in sources]
                           + ["-o", rexe], capture_output=True, text=True)
        if p.returncode != 0:
            check(name, False, f"reference build failed: {p.stderr.strip()}")
            return
        rp = subprocess.run([qemu, rexe], capture_output=True, text=True,
                            timeout=30)
        if (rp.returncode, rp.stdout) != (p.returncode, p.stdout):
            check(name, False, f"ref rc={rp.returncode} out={rp.stdout!r} "
                               f"vs own rc={p.returncode} out={p.stdout!r}")
            return
        check(name, True)


def main():
    if not os.path.exists(BIN):
        print("build/bin/zerocc missing - run `make build/bin/zerocc` first")
        return 2
    if shutil.which("ar") is None:
        print("ar missing - cannot run the archive cases")
        return 2

    print("-- functional (own linker + gcc differential) --")
    case_link_run("hi", ["hi_main.c", "hi_impl.c"])
    case_link_run("cross_global", ["cg_main.c", "cg_impl.c"])
    case_link_run("cross_double", ["dfg_main.c", "dfg_impl.c"])
    case_link_run("uninit", ["uninit.c"])
    # cross-TU pointer initializer `int *p = &x;`: the emitter now registers a
    # by-name (kind-2) data reloc for extern targets, emitted as a .rela..data
    # R_X86_64_64 against the UND symbol and resolved by both linkers.
    case_link_run("cross_ptr", ["ptr_main.c", "ptr_impl.c"])
    # same shape, but the main TU is gcc-compiled: also exercises the linker's
    # handling of gcc-style section families (.data.rel.ro, .eh_frame).
    case_link_run("cross_ptr_gccobj", ["ptr_main.c", "ptr_impl.c"],
                  gcc_sources=(0,))
    case_link_run("big", ["big_main.c", "big_impl.c"])
    case_link_run("str2", ["str2_main.c", "str2_impl.c"])
    case_link_run("chain3", ["c3_top.c", "c3_mid.c", "c3_leaf.c"])

    print("-- archives (member extraction) --")
    case_link_run("ar_str2", ["str2_main.c", "str2_impl.c"], archives_of=(1,))
    case_link_run("ar_hi", ["hi_main.c", "hi_impl.c"], archives_of=(1,))

    print("-- error paths --")
    case_link_error("dup_def", ["dup_a.c", "dup_b.c"], "multiple definition")
    case_link_error("nomain", ["nomain.c"], "main")
    case_link_error("undef", ["undef_main.c"], "undefined symbol", no_rt=True)

    print("-- unsupported-ISA rejection --")
    with tempfile.TemporaryDirectory(prefix="zerocc_t2_") as work:
        objs = compile_objs(work, ["uninit.c"])
        rc, out, err = zc([objs[0], "--target", "riscv32", "--zerocc-link",
                           "-o", os.path.join(work, "x")])
        check("riscv32_rejected", rc != 0 and ("x86-64" in err or "aarch64" in err or "riscv64" in err),
              f"rc={rc} err={err!r}")

    print("-- aarch64 (QEMU differential) --")
    case_aarch64_run("a64_hi", ["hi_main.c", "hi_impl.c"])
    case_aarch64_run("a64_data", ["a64_data.c"])
    case_aarch64_run("a64_ar", ["hi_main.c", "hi_impl.c"], archives_of=(1,))

    print("-- riscv64 (QEMU differential) --")
    case_riscv64_run("rv64_hi", ["hi_main.c", "hi_impl.c"])
    case_riscv64_run("rv64_data", ["a64_data.c"])
    case_riscv64_run("rv64_ar", ["hi_main.c", "hi_impl.c"], archives_of=(1,))

    passed = sum(1 for _, ok in RESULTS if ok)
    total = len(RESULTS)
    print(f"\n{passed}/{total} own-linker tests passed")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
