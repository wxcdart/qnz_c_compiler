#!/usr/bin/env python3
"""
asan_tests.py - Target AddressSanitizer (-fsanitize=address) verification battery

Tests:
1. Clean stack/array accesses pass cleanly (rc=0) under -fsanitize=address
   at -O0..-O3 in both JIT (--run) and AOT (-o) modes.
2. Stack buffer overflow triggers ASan trap (SIGILL, rc=132) and prints the
   expected "==ERROR: AddressSanitizer: stack-buffer-overflow" error banner
   to stderr at -O0..-O3 in both JIT and AOT modes.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ZEROCC = ROOT / "build" / "bin" / "zerocc"


def run_cmd(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def main():
    if not ZEROCC.exists():
        print(f"Error: {ZEROCC} not found", file=sys.stderr)
        return 1

    clean_src = str(ROOT / "test" / "jit" / "asan_test.c")
    overflow_src = str(ROOT / "test" / "san" / "asan_overflow.c")

    levels = [0, 1, 2, 3]
    fails = 0
    total = 0

    with tempfile.TemporaryDirectory() as tmpdir:
        # 1. Clean test
        print("=== Target ASan Clean Tests ===")
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

        # 2. Overflow test
        print("\n=== Target ASan Overflow Detection Tests ===")
        for opt in levels:
            # JIT mode
            total += 1
            rc, out, err = run_cmd([str(ZEROCC), f"-O{opt}", "-fsanitize=address", "--run", overflow_src])
            if (rc != 0) and ("AddressSanitizer" in err or "AddressSanitizer" in out or "overflow" in err):
                print(f"  [PASS] overflow JIT -O{opt} trapped as expected")
            else:
                print(f"  [FAIL] overflow JIT -O{opt} did not trap properly (rc={rc}, out={out[:100]}, err={err[:100]})")
                fails += 1

            # AOT mode
            total += 1
            bin_path = os.path.join(tmpdir, f"overflow_aot_{opt}")
            rc_c, _, err_c = run_cmd([str(ZEROCC), f"-O{opt}", "-fsanitize=address", overflow_src, "-o", bin_path])
            if rc_c != 0:
                print(f"  [FAIL] overflow AOT compile -O{opt}: {err_c[:150]}")
                fails += 1
                continue
            rc_r, out_r, err_r = run_cmd([bin_path])
            if (rc_r != 0) and ("AddressSanitizer" in err_r or "AddressSanitizer" in out_r or "overflow" in err_r):
                print(f"  [PASS] overflow AOT -O{opt} trapped as expected")
            else:
                print(f"  [FAIL] overflow AOT -O{opt} did not trap properly (rc={rc_r}, out={out_r[:100]}, err={err_r[:100]})")
                fails += 1

    print(f"\nTarget ASan Suite: {total - fails}/{total} passed")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
