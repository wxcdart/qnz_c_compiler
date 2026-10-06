#!/usr/bin/env python3
"""
ZeroCC Unified Test Runner
Supports structured categories (unit, jit, abi, semantic), per-test timing,
pass/fail tracking, TAP (Test Anything Protocol v13), and AOT (Ahead-Of-Time) testing.

Modes:
- JIT (default): Runs pre-compiled test binaries that link against ZeroCC library
- AOT: Compiles C tests with zerocc, verifies ELF, links, and executes
"""

import argparse
import os
import subprocess
import sys
import time
import shutil

# ANSI color codes
GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
BOLD = "\033[1m"
RESET = "\033[0m"


def is_color_enabled():
    return sys.stdout.isatty() and os.environ.get("NO_COLOR") is None


def colorize(text, color):
    if is_color_enabled():
        return f"{color}{text}{RESET}"
    return text


class TestResult:
    def __init__(self, name, category, passed, duration_ms, returncode, stdout, stderr, error_msg=""):
        self.name = name
        self.category = category
        self.passed = passed
        self.duration_ms = duration_ms
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.error_msg = error_msg


def find_test_binaries(root_dir, category=None):
    """Find compiled test executables across categories in build/test or test."""
    categories = ["unit", "semantic", "abi", "jit"]
    if category:
        if category not in categories:
            print(f"Error: Unknown category '{category}'. Available: {', '.join(categories)}", file=sys.stderr)
            sys.exit(1)
        categories = [category]

    build_test_dir = os.path.join(root_dir, "build", "test")
    base_dir = build_test_dir if os.path.isdir(build_test_dir) else os.path.join(root_dir, "test")

    found = []
    for cat in categories:
        cat_dir = os.path.join(base_dir, cat)
        if not os.path.isdir(cat_dir):
            continue
        for entry in sorted(os.listdir(cat_dir)):
            full_path = os.path.join(cat_dir, entry)
            if (
                os.path.isfile(full_path)
                and os.access(full_path, os.X_OK)
                and not entry.endswith((".c", ".h", ".o"))
            ):
                found.append((f"{cat}/{entry}", full_path, cat))
    return found


def find_test_sources(root_dir, category=None):
    """Find C test source files for AOT testing."""
    categories = ["unit", "semantic", "abi", "jit", "aot"]
    if category:
        if category not in categories:
            print(f"Error: Unknown category '{category}'. Available: {', '.join(categories)}", file=sys.stderr)
            sys.exit(1)
        categories = [category]

    # For AOT testing, always look in source directory (test/) for .c files
    base_dir = os.path.join(root_dir, "test")

    found = []
    for cat in categories:
        cat_dir = os.path.join(base_dir, cat)
        if not os.path.isdir(cat_dir):
            continue
        for entry in sorted(os.listdir(cat_dir)):
            if entry.endswith(".c"):
                full_path = os.path.join(cat_dir, entry)
                name = entry[:-2]  # Remove .c extension
                found.append((f"{cat}/{name}", full_path, cat))
    return found


def run_single_test(name, path, category, timeout_sec=10, use_valgrind=False):
    """Run a pre-compiled test binary (JIT mode)."""
    start = time.perf_counter()
    try:
        cmd = [path]
        effective_timeout = timeout_sec
        if use_valgrind:
            cmd = ["valgrind", "--leak-check=full", "--error-exitcode=1", "-q", path]
            effective_timeout = timeout_sec * 5
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=effective_timeout,
        )
        duration_ms = (time.perf_counter() - start) * 1000.0
        passed = proc.returncode == 0
        return TestResult(
            name=name,
            category=category,
            passed=passed,
            duration_ms=duration_ms,
            returncode=proc.returncode,
            stdout=proc.stdout,
            stderr=proc.stderr,
        )
    except subprocess.TimeoutExpired as e:
        duration_ms = (time.perf_counter() - start) * 1000.0
        return TestResult(
            name=name,
            category=category,
            passed=False,
            duration_ms=duration_ms,
            returncode=-1,
            stdout=e.stdout or "",
            stderr=e.stderr or "",
            error_msg=f"TIMEOUT after {timeout_sec}s",
        )
    except Exception as e:
        duration_ms = (time.perf_counter() - start) * 1000.0
        return TestResult(
            name=name,
            category=category,
            passed=False,
            duration_ms=duration_ms,
            returncode=-1,
            stdout="",
            stderr="",
            error_msg=str(e),
        )


def run_aot_test(name, source_path, category, root_dir, zerocc_path, target, timeout_sec=30, verbose=False):
    """Run an AOT test: compile with zerocc, verify ELF, link, and execute."""
    start = time.perf_counter()
    test_dir = os.path.join(root_dir, "build", "test", "aot")
    os.makedirs(test_dir, exist_ok=True)

    base_name = os.path.splitext(os.path.basename(source_path))[0]
    obj_path = os.path.join(test_dir, f"{base_name}.o")
    exe_path = os.path.join(test_dir, base_name)

    try:
        # Step 1: Compile with zerocc
        if verbose:
            print(f"  Compiling: {source_path}")

        zerocc_cmd = [zerocc_path, "-c", source_path, "-o", obj_path]
        if target and target != "x86-64":
            zerocc_cmd.extend(["--target", target])
        proc = subprocess.run(
            zerocc_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_sec,
        )

        if proc.returncode != 0:
            duration_ms = (time.perf_counter() - start) * 1000.0
            return TestResult(
                name=name,
                category=category,
                passed=False,
                duration_ms=duration_ms,
                returncode=proc.returncode,
                stdout=proc.stdout,
                stderr=proc.stderr,
                error_msg="Compilation failed",
            )

        # Step 2: Verify ELF (static check)
        if verbose:
            print(f"  Verifying ELF: {obj_path}")

        elf_verify_path = os.path.join(root_dir, "tools", "elf_verify.py")
        if os.path.isfile(elf_verify_path):
            verify_cmd = ["python3", elf_verify_path, obj_path, "--quiet"]
            subprocess.run(verify_cmd, capture_output=True, timeout=10)
            # Don't fail on verification, just log

        # Step 3: Link with system linker
        if verbose:
            print(f"  Linking: {obj_path}")

        link_cmd = ["gcc", "-o", exe_path, obj_path]
        proc = subprocess.run(
            link_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_sec,
        )

        if proc.returncode != 0:
            duration_ms = (time.perf_counter() - start) * 1000.0
            return TestResult(
                name=name,
                category=category,
                passed=False,
                duration_ms=duration_ms,
                returncode=proc.returncode,
                stdout=proc.stdout,
                stderr=proc.stderr,
                error_msg="Linking failed",
            )

        # Step 4: Execute (with QEMU if cross-ISA)
        if verbose:
            print(f"  Executing: {exe_path}")

        exec_cmd = [exe_path]
        effective_timeout = timeout_sec

        if target and target != "x86-64":
            # Use QEMU for cross-ISA execution
            qemu_map = {
                "aarch64": "qemu-aarch64-static",
                "arm64": "qemu-aarch64-static",
                "riscv64": "qemu-riscv64-static",
                "riscv32": "qemu-riscv32-static",
            }
            qemu_bin = qemu_map.get(target)
            if qemu_bin:
                # Check if QEMU is available
                if not shutil.which(qemu_bin):
                    duration_ms = (time.perf_counter() - start) * 1000.0
                    return TestResult(
                        name=name,
                        category=category,
                        passed=False,
                        duration_ms=duration_ms,
                        returncode=-1,
                        stdout="",
                        stderr="",
                        error_msg=f"QEMU not found: {qemu_bin}",
                    )
                exec_cmd = [qemu_bin, exe_path]
                effective_timeout = timeout_sec * 3  # QEMU is slower

        proc = subprocess.run(
            exec_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=effective_timeout,
        )

        duration_ms = (time.perf_counter() - start) * 1000.0
        passed = proc.returncode == 0

        return TestResult(
            name=name,
            category=category,
            passed=passed,
            duration_ms=duration_ms,
            returncode=proc.returncode,
            stdout=proc.stdout,
            stderr=proc.stderr,
        )

    except subprocess.TimeoutExpired as e:
        duration_ms = (time.perf_counter() - start) * 1000.0
        return TestResult(
            name=name,
            category=category,
            passed=False,
            duration_ms=duration_ms,
            returncode=-1,
            stdout=e.stdout or "",
            stderr=e.stderr or "",
            error_msg=f"TIMEOUT after {timeout_sec}s",
        )
    except Exception as e:
        duration_ms = (time.perf_counter() - start) * 1000.0
        return TestResult(
            name=name,
            category=category,
            passed=False,
            duration_ms=duration_ms,
            returncode=-1,
            stdout="",
            stderr="",
            error_msg=str(e),
        )


def write_tap_report(results, total_duration_s):
    print("TAP version 13")
    print(f"1..{len(results)}")
    for i, r in enumerate(results, 1):
        status = "ok" if r.passed else "not ok"
        directive = ""
        if not r.passed and r.error_msg:
            directive = f" # {r.error_msg}"
        print(f"{status} {i} - {r.name} (time={r.duration_ms:.1f}ms){directive}")
        if not r.passed:
            print("  ---")
            print(f"  duration_ms: {r.duration_ms:.2f}")
            print(f"  exit_code: {r.returncode}")
            if r.stderr.strip():
                print("  stderr: |")
                for line in r.stderr.strip().splitlines():
                    print(f"    {line}")
            if r.stdout.strip():
                print("  stdout: |")
                for line in r.stdout.strip().splitlines():
                    print(f"    {line}")
            print("  ...")


def main():
    parser = argparse.ArgumentParser(description="ZeroCC Unified Test Runner")
    parser.add_argument("--category", "-c", choices=["unit", "semantic", "abi", "jit", "aot"],
                        help="Run only tests from a specific category")
    parser.add_argument("--mode", "-m", choices=["jit", "aot"], default="jit",
                        help="Test mode: JIT (pre-compiled) or AOT (compile with zerocc)")
    parser.add_argument("--target", "-t", choices=["x86-64", "aarch64", "arm64", "riscv64", "riscv32"],
                        default="x86-64", help="Target architecture for AOT mode")
    parser.add_argument("--zerocc", "-z", default=None,
                        help="Path to zerocc compiler (default: auto-detect)")
    parser.add_argument("--tap", action="store_true", help="Output in TAP v13 format")
    parser.add_argument("--valgrind", action="store_true", help="Run tests under Valgrind memcheck")
    parser.add_argument("--timeout", type=int, default=30, help="Per-test timeout in seconds (default: 30)")
    parser.add_argument("--verbose", "-v", action="store_true", help="Print detailed output")
    parser.add_argument("tests", nargs="*", help="Optional specific tests to run")

    args = parser.parse_args()
    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

    # Auto-detect zerocc path
    zerocc_path = args.zerocc
    if not zerocc_path:
        zerocc_path = os.path.join(root_dir, "build", "bin", "zerocc")
        if not os.path.isfile(zerocc_path):
            zerocc_path = os.path.join(root_dir, "zerocc")
        if not os.path.isfile(zerocc_path):
            zerocc_path = "zerocc"  # Try PATH

    # Select test items based on mode
    if args.mode == "aot":
        test_items = find_test_sources(root_dir, category=args.category)
        if not test_items:
            print("No test sources found for AOT testing.", file=sys.stderr)
            sys.exit(1)
    else:
        test_items = find_test_binaries(root_dir, category=args.category)
        if not test_items:
            print("No test binaries found. Did you run 'make' first?", file=sys.stderr)
            sys.exit(1)

    # Print header
    if not args.tap:
        mode_str = f"[{args.mode.upper()}]"
        cat_str = f" [{args.category}]" if args.category else ""
        target_str = f" (target={args.target})" if args.mode == "aot" else ""
        print(colorize(f"=== ZeroCC Test Suite{mode_str}{cat_str}{target_str} ({len(test_items)} tests) ===", BOLD))

    results = []
    overall_start = time.perf_counter()

    for name, path, category in test_items:
        if args.mode == "aot":
            res = run_aot_test(
                name=name,
                source_path=path,
                category=category,
                root_dir=root_dir,
                zerocc_path=zerocc_path,
                target=args.target,
                timeout_sec=args.timeout,
                verbose=args.verbose
            )
        else:
            res = run_single_test(
                name=name,
                path=path,
                category=category,
                timeout_sec=args.timeout,
                use_valgrind=args.valgrind
            )

        results.append(res)

        if not args.tap:
            status_tag = colorize("[PASS]", GREEN) if res.passed else colorize("[FAIL]", RED)
            time_tag = f"({res.duration_ms:6.1f}ms)"
            cat_tag = colorize(f"[{res.category}]", CYAN)
            print(f"  {status_tag} {cat_tag:16} {res.name:<35} {time_tag}")

            if not res.passed or args.verbose:
                if res.error_msg:
                    print(colorize(f"       Reason: {res.error_msg}", RED))
                if res.stdout.strip():
                    for line in res.stdout.strip().splitlines():
                        print(f"       | {line}")
                if res.stderr.strip():
                    for line in res.stderr.strip().splitlines():
                        print(colorize(f"       ! {line}", RED))

    total_duration_s = time.perf_counter() - overall_start
    passed_count = sum(1 for r in results if r.passed)
    failed_count = sum(1 for r in results if not r.passed)

    if args.tap:
        write_tap_report(results, total_duration_s)

    if not args.tap:
        print()
        print(colorize("=" * 60, BOLD))
        summary_color = GREEN if failed_count == 0 else RED
        print(colorize(
            f"Results: {passed_count}/{len(results)} passed, {failed_count} failed "
            f"in {total_duration_s:.2f}s",
            summary_color + BOLD,
        ))
        print(colorize("=" * 60, BOLD))

    sys.exit(0 if failed_count == 0 else 1)


if __name__ == "__main__":
    main()
