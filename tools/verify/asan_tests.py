#!/usr/bin/env python3
"""
asan_tests.py - Target AddressSanitizer (-fsanitize=address) verification battery

Tests:
1. Stack clean accesses pass cleanly (rc=0) under -fsanitize=address
   at -O0..-O3 in both JIT (--run) and AOT (-o) modes.
2. Stack buffer overflow triggers ASan trap (SIGILL, rc=132 or rc!=0) and prints
   "stack-buffer-overflow" error banner to stderr at -O0..-O3 in JIT and AOT modes.
3. Global clean accesses pass cleanly (rc=0) under -fsanitize=address
   at -O0..-O3 in both JIT (--run) and AOT (-o) modes.
4. Global buffer overflow triggers ASan trap (rc!=0) and prints
   "global-buffer-overflow" error banner to stderr at -O0..-O3 in JIT and AOT modes.
5. Cross-ISA tests under QEMU for AArch64 and RISC-V 64 (both clean and overflow).
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ZEROCC = ROOT / "build" / "bin" / "zerocc"


def run_cmd(cmd, timeout=30):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -1, "", "TIMEOUT"


def clean_core_dumps():
    for f in list(ROOT.glob("core*")) + list(Path("/tmp").glob("core*")):
        try:
            f.unlink()
        except OSError:
            pass


def main():
    if not ZEROCC.exists():
        print(f"Error: {ZEROCC} not found", file=sys.stderr)
        return 1

    clean_src = str(ROOT / "test" / "jit" / "asan_test.c")
    overflow_src = str(ROOT / "test" / "san" / "asan_overflow.c")
    clean_glob_src = str(ROOT / "test" / "jit" / "asan_global_clean.c")
    overflow_glob_src = str(ROOT / "test" / "san" / "asan_global_overflow.c")

    levels = [0, 1, 2, 3]
    fails = 0
    total = 0

    with tempfile.TemporaryDirectory() as tmpdir:
        # 1. Stack Clean test
        print("=== Target ASan Stack Clean Tests ===")
        for opt in levels:
            # JIT mode
            total += 1
            rc, out, err = run_cmd([str(ZEROCC), f"-O{opt}", "-fsanitize=address", "--run", clean_src])
            if rc == 0 and "asan_test passed" in out:
                print(f"  [PASS] clean JIT -O{opt}")
            else:
                print(f"  [FAIL] clean JIT -O{opt} (rc={rc}): {err[:150]}")
                fails += 1

            # AOT mode
            total += 1
            bin_path = os.path.join(tmpdir, f"clean_aot_{opt}")
            rc_c, _, err_c = run_cmd([str(ZEROCC), f"-O{opt}", "-fsanitize=address", clean_src, "-o", bin_path])
            if rc_c != 0:
                print(f"  [FAIL] clean AOT compile -O{opt}: {err_c[:150]}")
                fails += 1
                continue
            rc_r, out_r, err_r = run_cmd([bin_path])
            if rc_r == 0 and "asan_test passed" in out_r:
                print(f"  [PASS] clean AOT -O{opt}")
            else:
                print(f"  [FAIL] clean AOT run -O{opt} (rc={rc_r}): {err_r[:150]}")
                fails += 1

        # 2. Stack Overflow test
        print("\n=== Target ASan Stack Overflow Detection Tests ===")
        for opt in levels:
            # JIT mode
            total += 1
            rc, out, err = run_cmd([str(ZEROCC), f"-O{opt}", "-fsanitize=address", "--run", overflow_src])
            if (rc != 0) and ("AddressSanitizer" in err or "AddressSanitizer" in out or "overflow" in err):
                print(f"  [PASS] stack overflow JIT -O{opt} trapped as expected")
            else:
                print(f"  [FAIL] stack overflow JIT -O{opt} did not trap properly (rc={rc}, out={out[:100]}, err={err[:100]})")
                fails += 1

            # AOT mode
            total += 1
            bin_path = os.path.join(tmpdir, f"overflow_aot_{opt}")
            rc_c, _, err_c = run_cmd([str(ZEROCC), f"-O{opt}", "-fsanitize=address", overflow_src, "-o", bin_path])
            if rc_c != 0:
                print(f"  [FAIL] stack overflow AOT compile -O{opt}: {err_c[:150]}")
                fails += 1
                continue
            rc_r, out_r, err_r = run_cmd([bin_path])
            if (rc_r != 0) and ("AddressSanitizer" in err_r or "AddressSanitizer" in out_r or "overflow" in err_r):
                print(f"  [PASS] stack overflow AOT -O{opt} trapped as expected")
            else:
                print(f"  [FAIL] stack overflow AOT -O{opt} did not trap properly (rc={rc_r}, out={out_r[:100]}, err={err_r[:100]})")
                fails += 1

        # 3. Global Clean test
        print("\n=== Target ASan Global Clean Tests ===")
        for opt in levels:
            # JIT mode
            total += 1
            rc, out, err = run_cmd([str(ZEROCC), f"-O{opt}", "-fsanitize=address", "--run", clean_glob_src])
            if rc == 0 and "asan_test passed" in out:
                print(f"  [PASS] global clean JIT -O{opt}")
            else:
                print(f"  [FAIL] global clean JIT -O{opt} (rc={rc}): {err[:150]}")
                fails += 1

            # AOT mode
            total += 1
            bin_path = os.path.join(tmpdir, f"clean_glob_aot_{opt}")
            rc_c, _, err_c = run_cmd([str(ZEROCC), f"-O{opt}", "-fsanitize=address", clean_glob_src, "-o", bin_path])
            if rc_c != 0:
                print(f"  [FAIL] global clean AOT compile -O{opt}: {err_c[:150]}")
                fails += 1
                continue
            rc_r, out_r, err_r = run_cmd([bin_path])
            if rc_r == 0 and "asan_test passed" in out_r:
                print(f"  [PASS] global clean AOT -O{opt}")
            else:
                print(f"  [FAIL] global clean AOT run -O{opt} (rc={rc_r}): {err_r[:150]}")
                fails += 1

        # 4. Global Overflow test
        print("\n=== Target ASan Global Overflow Detection Tests ===")
        for opt in levels:
            # JIT mode
            total += 1
            rc, out, err = run_cmd([str(ZEROCC), f"-O{opt}", "-fsanitize=address", "--run", overflow_glob_src])
            if (rc != 0) and ("global-buffer-overflow" in err or "global-buffer-overflow" in out):
                print(f"  [PASS] global overflow JIT -O{opt} trapped as expected")
            else:
                print(f"  [FAIL] global overflow JIT -O{opt} did not trap properly (rc={rc}, out={out[:100]}, err={err[:100]})")
                fails += 1

            # AOT mode
            total += 1
            bin_path = os.path.join(tmpdir, f"overflow_glob_aot_{opt}")
            rc_c, _, err_c = run_cmd([str(ZEROCC), f"-O{opt}", "-fsanitize=address", overflow_glob_src, "-o", bin_path])
            if rc_c != 0:
                print(f"  [FAIL] global overflow AOT compile -O{opt}: {err_c[:150]}")
                fails += 1
                continue
            rc_r, out_r, err_r = run_cmd([bin_path])
            if (rc_r != 0) and ("global-buffer-overflow" in err_r or "global-buffer-overflow" in out_r):
                print(f"  [PASS] global overflow AOT -O{opt} trapped as expected")
            else:
                print(f"  [FAIL] global overflow AOT -O{opt} did not trap properly (rc={rc_r}, out={out_r[:100]}, err={err_r[:100]})")
                fails += 1

        # 5. Cross-ISA tests (AArch64, AArch32 & RISC-V 64)
        print("\n=== Target ASan Multi-ISA (QEMU) Tests ===")
        for arch, qemu in [("aarch64", "qemu-aarch64-static"), ("aarch32", "qemu-arm-static"), ("riscv64", "qemu-riscv64-static")]:
            if not shutil.which(qemu):
                print(f"  [SKIP] {arch} ({qemu} not found)")
                continue

            # Clean test
            total += 1
            bin_clean = os.path.join(tmpdir, f"{arch}_clean")
            rc_c, _, err_c = run_cmd([str(ZEROCC), "--target", arch, "-fsanitize=address", clean_glob_src, "-o", bin_clean])
            if rc_c != 0:
                print(f"  [FAIL] {arch} clean compile: {err_c[:150]}")
                fails += 1
            else:
                rc_r, out_r, err_r = run_cmd([qemu, bin_clean])
                if rc_r == 0 and "asan_test passed" in out_r:
                    print(f"  [PASS] {arch} clean execution")
                else:
                    print(f"  [FAIL] {arch} clean execution (rc={rc_r}): {err_r[:150]}")
                    fails += 1

            # Overflow test
            total += 1
            bin_ovf = os.path.join(tmpdir, f"{arch}_ovf")
            rc_c, _, err_c = run_cmd([str(ZEROCC), "--target", arch, "-fsanitize=address", overflow_glob_src, "-o", bin_ovf])
            if rc_c != 0:
                print(f"  [FAIL] {arch} overflow compile: {err_c[:150]}")
                fails += 1
            else:
                rc_r, out_r, err_r = run_cmd([qemu, bin_ovf])
                if (rc_r != 0) and ("global-buffer-overflow" in err_r or "global-buffer-overflow" in out_r):
                    print(f"  [PASS] {arch} global overflow trapped as expected")
                else:
                    print(f"  [FAIL] {arch} global overflow did not trap (rc={rc_r}, out={out_r[:100]}, err={err_r[:100]})")
                    fails += 1

    clean_core_dumps()
    print(f"\nTarget ASan Suite: {total - fails}/{total} passed")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    clean_core_dumps()
    sys.exit(main())
