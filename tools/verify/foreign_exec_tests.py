#!/usr/bin/env python3
"""
tools/verify/foreign_exec_tests.py — In-Process Foreign Binary Execution Suite (Sprint 261).

Differential and standalone in-process execution test suite for Windows PE32+
and macOS Mach-O 64-bit executables running natively on Linux host without Wine/Darwin:
- Direct file dispatch (--run-pe, --run-macho)
- Auto-detect binary file dispatch (--run <file>)
- On-the-fly source compilation and execution (--pe --run, --macho --run)
- Windows x64 ms_abi runtime shims (puts, printf, malloc, free, strcmp, abs)
- Exit code interception (ExitProcess / exit / main return)
- Optimization sweep: -O0, -O1, -O2, -O3
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

# Path setup via tools __init__
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

ROOT = Path(find_repo_root())
ZEROCC = ROOT / "build" / "bin" / "zerocc"

passed = 0
failed = 0


def report(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  [PASS] {name}")
    else:
        failed += 1
        print(f"  [FAIL] {name}: {detail}")


def test_pe_basic_exit():
    """Verify basic PE32+ exit code return via --run-pe and --run."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "test.c"
        exe = Path(td) / "test.exe"
        src.write_text("int main(void) { return 42; }\n")

        res = subprocess.run([str(ZEROCC), "--pe", "-o", str(exe), str(src)], capture_output=True, text=True)
        if res.returncode != 0:
            report("PE basic build", False, res.stderr)
            return
        report("PE basic build", True)

        # 1. Direct --run-pe
        r1 = subprocess.run([str(ZEROCC), "--run-pe", str(exe)], capture_output=True, text=True)
        report("PE --run-pe exit code", r1.returncode == 42, f"expected 42, got {r1.returncode}")

        # 2. Auto-detect --run
        r2 = subprocess.run([str(ZEROCC), "--run", str(exe)], capture_output=True, text=True)
        report("PE --run auto-detect exit code", r2.returncode == 42, f"expected 42, got {r2.returncode}")

        # 3. Source --pe --run
        r3 = subprocess.run([str(ZEROCC), "--pe", "--run", str(src)], capture_output=True, text=True)
        report("PE --pe --run source execution", r3.returncode == 42, f"expected 42, got {r3.returncode}")


def test_pe_ms_abi_shims():
    """Verify MS x64 calling convention shims for libc functions."""
    code = """
    int puts(const char *s);
    int printf(const char *fmt, ...);
    int strcmp(const char *s1, const char *s2);
    int abs(int n);
    void *malloc(unsigned long sz);
    void free(void *p);

    int main(void) {
        puts("PE_SHIM_PUTS_OK");
        printf("PE_PRINTF_VAL=%d_%s\\n", 12345, "TEST");
        int cmp = strcmp("abc", "abc");
        int a = abs(-99);
        char *buf = (char *)malloc(16);
        buf[0] = 'H'; buf[1] = 'I'; buf[2] = 0;
        puts(buf);
        free(buf);
        if (cmp != 0 || a != 99) return 1;
        return 55;
    }
    """
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "shim.c"
        exe = Path(td) / "shim.exe"
        src.write_text(code)

        res = subprocess.run([str(ZEROCC), "--pe", "-o", str(exe), str(src)], capture_output=True, text=True)
        if res.returncode != 0:
            report("PE shim build", False, res.stderr)
            return

        run_res = subprocess.run([str(ZEROCC), "--run", str(exe)], capture_output=True, text=True)
        ok = (run_res.returncode == 55 and
              "PE_SHIM_PUTS_OK" in run_res.stdout and
              "PE_PRINTF_VAL=12345_TEST" in run_res.stdout and
              "HI" in run_res.stdout)
        report("PE ms_abi stdio & malloc shims", ok,
               f"rc={run_res.returncode}, stdout={run_res.stdout.strip()!r}")


def test_pe_dllimport_execution():
    """Verify in-process execution of PE with __declspec(dllimport) Win32 APIs."""
    code = """
    __declspec(dllimport) unsigned int GetLastError(void);
    __declspec(dllimport) void Sleep(unsigned int ms);
    int main(void) {
        Sleep(1);
        unsigned int err = GetLastError();
        if (err != 0) return 1;
        return 42;
    }
    """
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "import_test.c"
        exe = Path(td) / "import_test.exe"
        src.write_text(code)

        res = subprocess.run([str(ZEROCC), "--pe", "-o", str(exe), str(src)], capture_output=True, text=True)
        if res.returncode != 0:
            report("PE dllimport build", False, res.stderr)
            return

        run_res = subprocess.run([str(ZEROCC), "--run", str(exe)], capture_output=True, text=True)
        report("PE __declspec(dllimport) execution", run_res.returncode == 42, f"rc={run_res.returncode}")


def test_pe_opt_levels():
    """Verify PE execution across optimization levels -O0 to -O3."""
    code = """
    int puts(const char *s);
    int sum_arr(int *arr, int n) {
        int s = 0;
        for (int i = 0; i < n; i++) s += arr[i];
        return s;
    }
    int main(void) {
        int vals[5] = { 10, 20, 30, 40, 50 };
        int total = sum_arr(vals, 5);
        if (total != 150) return 1;
        return 70;
    }
    """
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "opt.c"
        src.write_text(code)

        for opt in ["-O0", "-O1", "-O2", "-O3"]:
            exe = Path(td) / f"opt_{opt}.exe"
            bld = subprocess.run([str(ZEROCC), opt, "--pe", "-o", str(exe), str(src)],
                                 capture_output=True, text=True)
            if bld.returncode != 0:
                report(f"PE {opt} build", False, bld.stderr)
                continue
            r = subprocess.run([str(ZEROCC), "--run", str(exe)], capture_output=True, text=True)
            report(f"PE {opt} execution", r.returncode == 70, f"rc={r.returncode}")


def test_macho_basic_exit():
    """Verify Mach-O 64-bit execution via --run-macho and --run."""
    code = """
    int fib(int n) {
        if (n <= 1) return n;
        return fib(n - 1) + fib(n - 2);
    }
    int main(void) {
        return fib(10); /* 55 */
    }
    """
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "test.c"
        bin_out = Path(td) / "test.macho"
        src.write_text(code)

        res = subprocess.run([str(ZEROCC), "--macho", "-o", str(bin_out), str(src)], capture_output=True, text=True)
        if res.returncode != 0:
            report("Mach-O basic build", False, res.stderr)
            return
        report("Mach-O basic build", True)

        # 1. Direct --run-macho
        r1 = subprocess.run([str(ZEROCC), "--run-macho", str(bin_out)], capture_output=True, text=True)
        report("Mach-O --run-macho exit code", r1.returncode == 55, f"expected 55, got {r1.returncode}")

        # 2. Auto-detect --run
        r2 = subprocess.run([str(ZEROCC), "--run", str(bin_out)], capture_output=True, text=True)
        report("Mach-O --run auto-detect exit code", r2.returncode == 55, f"expected 55, got {r2.returncode}")

        # 3. Source --macho --run
        r3 = subprocess.run([str(ZEROCC), "--macho", "--run", str(src)], capture_output=True, text=True)
        report("Mach-O --macho --run source execution", r3.returncode == 55, f"expected 55, got {r3.returncode}")


def test_macho_opt_levels():
    """Verify Mach-O execution across optimization levels -O0 to -O3."""
    code = """
    int compute(int x) {
        int r = 0;
        for (int i = 0; i < 100; i++) {
            r = (r + x) ^ i;
        }
        return r & 0x7F;
    }
    int main(void) {
        int v = compute(42);
        return v;
    }
    """
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "opt.c"
        src.write_text(code)

        # Find expected value
        expected = None
        for opt in ["-O0", "-O1", "-O2", "-O3"]:
            bin_out = Path(td) / f"opt_{opt}.macho"
            bld = subprocess.run([str(ZEROCC), opt, "--macho", "-o", str(bin_out), str(src)],
                                 capture_output=True, text=True)
            if bld.returncode != 0:
                report(f"Mach-O {opt} build", False, bld.stderr)
                continue
            r = subprocess.run([str(ZEROCC), "--run", str(bin_out)], capture_output=True, text=True)
            if expected is None:
                expected = r.returncode
            report(f"Mach-O {opt} execution", r.returncode == expected and r.returncode >= 0,
                   f"rc={r.returncode}, expected={expected}")


def main():
    print("=== Foreign Binary In-Process Execution Battery (Sprint 261) ===")
    test_pe_basic_exit()
    test_pe_ms_abi_shims()
    test_pe_dllimport_execution()
    test_pe_opt_levels()
    test_macho_basic_exit()
    test_macho_opt_levels()

    print(f"\nResults: {passed} passed, {failed} failed")
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
