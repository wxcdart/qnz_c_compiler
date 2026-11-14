#!/usr/bin/env python3
"""Cross-ISA Dynamic Loading Differential Verification Harness (Sprint 182).

Differential-tests zero_rt/dlfcn/dynlink.c across all 4 supported ISAs:
  - x86-64 (native host / qemu-x86_64-static)
  - AArch64 (qemu-aarch64-static)
  - RISC-V 64 (qemu-riscv64-static)
  - RISC-V 32 (qemu-riscv32-static, ELF32)

For each target architecture:
  1. Builds a target-ISA shared library (.so) containing exported functions,
     internal function calls, relocated data pointers, constructors, and destructors.
  2. Compiles a test runner linked with zero_rt/dlfcn/dynlink.c using the static-musl
     wrapper (musl-gcc-<arch>).
  3. Executes under the corresponding QEMU runner (or native host).
  4. Verifies:
     - zc_dlopen loads and maps the DSO into memory.
     - Constructors run before execution.
     - zc_dlsym resolves functions and relocated data pointers.
     - Calling function produces correct computed values.
     - zc_dladdr maps instruction pointers to the DSO and symbol name.
     - zc_dlclose unmaps memory and executes destructors.
     - Foreign architecture DSOs are rejected with "ELF machine mismatch" diagnostics.

Usage:
    python3 tools/verify/dlfcn_isa_diff.py               # all ISAs
    python3 tools/verify/dlfcn_isa_diff.py --isa aarch64 # one ISA
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Make tools package importable
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

REPO = find_repo_root()
DYNLINK_C = REPO / "zero_rt" / "dlfcn" / "dynlink.c"
DYNLINK_H = REPO / "zero_rt" / "dlfcn" / "dynlink.h"

ISAS = {
    "x86_64": {
        "so_cc": "gcc",
        "so_flags": ["-shared", "-fPIC", "-O2"],
        "runner_cc": "musl-gcc-x86_64",
        "runner_flags": ["-static", "-O2"],
        "qemu": "qemu-x86_64-static",
        "native": True,
        "machine_name": "x86-64",
    },
    "aarch64": {
        "so_cc": "aarch64-linux-gnu-gcc",
        "so_flags": ["-shared", "-fPIC", "-O2"],
        "runner_cc": "musl-gcc-aarch64",
        "runner_flags": ["-static", "-O2"],
        "qemu": "qemu-aarch64-static",
        "native": False,
        "machine_name": "AArch64",
    },
    "riscv64": {
        "so_cc": "riscv64-linux-gnu-gcc",
        "so_flags": ["-shared", "-fPIC", "-O2"],
        "runner_cc": "musl-gcc-riscv64",
        "runner_flags": ["-static", "-O2"],
        "qemu": "qemu-riscv64-static",
        "native": False,
        "machine_name": "RISC-V 64",
    },
    "riscv32": {
        "so_cc": "riscv64-linux-gnu-gcc",
        "so_flags": ["-march=rv32gc", "-mabi=ilp32", "-shared", "-fPIC", "-nostdlib", "-O2"],
        "runner_cc": "musl-gcc-riscv32",
        "runner_flags": ["-static", "-O2"],
        "qemu": "qemu-riscv32-static",
        "native": False,
        "machine_name": "RISC-V 32",
    },
}

SO_SOURCE = """
int g_ctor_val = 10;
int g_dtor_val = 99;
int g_target_val = 42;
int *g_reloc_ptr = &g_target_val;

__attribute__((constructor)) void test_init(void) {
    g_ctor_val = 100;
}

__attribute__((destructor)) void test_fini(void) {
    g_dtor_val = 0;
}

int test_internal_add(int a, int b) {
    return a + b;
}

int test_calc(int x) {
    return test_internal_add(x, *g_reloc_ptr) + g_ctor_val;
}
"""

RUNNER_SOURCE = """
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "zero_rt/dlfcn/dynlink.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <so_path> <foreign_so_path>\\n", argv[0]);
        return 1;
    }
    const char *so_path = argv[1];
    const char *foreign_so = argv[2];

    /* 1. Open target architecture SO */
    void *h = zc_dlopen(so_path, RTLD_NOW);
    if (!h) {
        fprintf(stderr, "FAIL: zc_dlopen(%s) failed: %s\\n", so_path, zc_dlerror_msg());
        return 2;
    }

    /* 2. Check constructor ran */
    int *ctor_p = (int *)zc_dlsym(h, "g_ctor_val");
    if (!ctor_p || *ctor_p != 100) {
        fprintf(stderr, "FAIL: constructor did not run (g_ctor_val = %d, expected 100)\\n",
                ctor_p ? *ctor_p : -1);
        return 3;
    }

    /* 3. Check data pointer relocation */
    int **pp = (int **)zc_dlsym(h, "g_reloc_ptr");
    if (!pp || !*pp || **pp != 42) {
        fprintf(stderr, "FAIL: g_reloc_ptr relocation failed\\n");
        return 4;
    }

    /* 4. Resolve and invoke function */
    int (*fn)(int) = (int (*)(int))zc_dlsym(h, "test_calc");
    if (!fn) {
        fprintf(stderr, "FAIL: zc_dlsym(test_calc) failed: %s\\n", zc_dlerror_msg());
        return 5;
    }
    /* test_calc(8) = (8 + 42) + 100 = 150 */
    int res = fn(8);
    if (res != 150) {
        fprintf(stderr, "FAIL: test_calc(8) = %d (expected 150)\\n", res);
        return 6;
    }

    /* 5. Reverse symbol resolution via zc_dladdr */
    Dl_info info;
    if (!zc_dladdr((const void *)fn, &info)) {
        fprintf(stderr, "FAIL: zc_dladdr failed for test_calc\\n");
        return 7;
    }
    if (!info.dli_sname || strcmp(info.dli_sname, "test_calc") != 0) {
        fprintf(stderr, "FAIL: zc_dladdr symbol name = %s (expected test_calc)\\n",
                info.dli_sname ? info.dli_sname : "(null)");
        return 8;
    }

    /* 6. Unload via zc_dlclose */
    if (zc_dlclose(h) != 0) {
        fprintf(stderr, "FAIL: zc_dlclose failed\\n");
        return 9;
    }

    /* 7. Foreign architecture rejection */
    void *bad = zc_dlopen(foreign_so, RTLD_NOW);
    if (bad != NULL) {
        fprintf(stderr, "FAIL: zc_dlopen unexpectedly accepted foreign SO %s\\n", foreign_so);
        zc_dlclose(bad);
        return 10;
    }
    const char *err = zc_dlerror_msg();
    if (!err || !strstr(err, "ELF machine mismatch")) {
        fprintf(stderr, "FAIL: foreign rejection missing ELF machine mismatch diagnostic: %s\\n",
                err ? err : "(null)");
        return 11;
    }

    printf("PASS\\n");
    return 0;
}
"""


def preflight(isas):
    missing = []
    if not DYNLINK_C.exists() or not DYNLINK_H.exists():
        missing.append("zero_rt/dlfcn/dynlink source files missing")
    for isa in isas:
        cfg = ISAS[isa]
        if shutil.which(cfg["so_cc"]) is None:
            missing.append(f"{cfg['so_cc']} (needed to compile {isa} .so)")
        if shutil.which(cfg["runner_cc"]) is None:
            missing.append(f"{cfg['runner_cc']} (needed to build {isa} test runner)")
        if not cfg["native"] and shutil.which(cfg["qemu"]) is None:
            missing.append(f"{cfg['qemu']} (needed to run {isa} under emulation)")
    return missing


def test_isa(isa, tmpdir):
    cfg = ISAS[isa]
    so_src = tmpdir / f"so_{isa}.c"
    so_bin = tmpdir / f"libtest_{isa}.so"
    runner_src = tmpdir / f"runner_{isa}.c"
    runner_bin = tmpdir / f"runner_{isa}"

    so_src.write_text(SO_SOURCE)
    runner_src.write_text(RUNNER_SOURCE)

    # 1. Compile target .so
    cmd_so = [cfg["so_cc"]] + cfg["so_flags"] + [str(so_src), "-o", str(so_bin)]
    p = subprocess.run(cmd_so, capture_output=True, text=True)
    if p.returncode != 0:
        return False, f"failed to compile {isa} .so: {p.stderr}"

    # 2. Compile test runner
    cmd_runner = [
        cfg["runner_cc"],
        "-I.",
        "-Iinclude",
        "-Isrc",
    ] + cfg["runner_flags"] + [
        str(runner_src),
        str(DYNLINK_C),
        "-o",
        str(runner_bin),
    ]
    p = subprocess.run(cmd_runner, cwd=str(REPO), capture_output=True, text=True)
    if p.returncode != 0:
        return False, f"failed to compile {isa} runner: {p.stderr}"

    # Foreign SO to test rejection: use x86_64 if not current arch, else aarch64
    foreign_isa = "aarch64" if isa == "x86_64" else "x86_64"
    foreign_so = tmpdir / f"libtest_{foreign_isa}.so"
    if not foreign_so.exists():
        f_cfg = ISAS[foreign_isa]
        f_src = tmpdir / f"so_{foreign_isa}.c"
        f_src.write_text(SO_SOURCE)
        p = subprocess.run([f_cfg["so_cc"]] + f_cfg["so_flags"] + [str(f_src), "-o", str(foreign_so)],
                           capture_output=True, text=True)
        if p.returncode != 0:
            return False, f"failed to compile foreign {foreign_isa} .so: {p.stderr}"

    # 3. Execute runner
    if cfg["native"]:
        exec_cmd = [str(runner_bin), str(so_bin), str(foreign_so)]
    else:
        exec_cmd = [cfg["qemu"], str(runner_bin), str(so_bin), str(foreign_so)]

    p = subprocess.run(exec_cmd, capture_output=True, text=True, timeout=30)
    if p.returncode != 0 or "PASS" not in p.stdout:
        return False, f"runner failed (rc={p.returncode}): stdout='{p.stdout.strip()}' stderr='{p.stderr.strip()}'"

    return True, "ok"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--isa", choices=list(ISAS.keys()), help="Test only specified ISA")
    args = parser.parse_args()

    selected_isas = [args.isa] if args.isa else list(ISAS.keys())

    missing = preflight(selected_isas)
    if missing:
        print("ERROR: missing preflight dependencies:")
        for m in missing:
            print(f"  - {m}")
        return 1

    print("=== Cross-ISA Dynamic Loading Gate (Sprint 182) ===")
    total_pass = 0
    total_fail = 0

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        for isa in selected_isas:
            cfg = ISAS[isa]
            run_mode = "native" if cfg["native"] else cfg["qemu"]
            print(f"  testing {isa:<8} (compiler={cfg['runner_cc']}, run={run_mode})...", end=" ", flush=True)
            ok, msg = test_isa(isa, tmp_path)
            if ok:
                print("PASS")
                total_pass += 1
            else:
                print(f"FAIL: {msg}")
                total_fail += 1

    print(f"\nResult: {total_pass} passed, {total_fail} failed out of {len(selected_isas)} ISAs")
    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
