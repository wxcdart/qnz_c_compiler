#!/usr/bin/env python3
"""
ZeroCC Unified Test Runner
Supports structured categories (unit, jit, abi, semantic), per-test timing,
pass/fail tracking, and TAP (Test Anything Protocol v13).
"""

import argparse
import os
import subprocess
import sys
import time

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
            # Must be an executable file and not a source file (.c, .h, .o)
            if (
                os.path.isfile(full_path)
                and os.access(full_path, os.X_OK)
                and not entry.endswith((".c", ".h", ".o"))
            ):
                found.append((f"{cat}/{entry}", full_path, cat))
    return found


def run_single_test(name, path, category, timeout_sec=10, use_valgrind=False):
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
    parser.add_argument("--category", "-c", choices=["unit", "semantic", "abi", "jit"],
                        help="Run only tests from a specific category")
    parser.add_argument("--tap", action="store_true", help="Output in TAP v13 format")
    parser.add_argument("--valgrind", action="store_true", help="Run tests under Valgrind memcheck")
    parser.add_argument("--timeout", type=int, default=10, help="Per-test timeout in seconds (default: 10)")
    parser.add_argument("--verbose", "-v", action="store_true", help="Print stdout/stderr for passed tests")
    parser.add_argument("tests", nargs="*", help="Optional specific test executables to run")

    args = parser.parse_args()
    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

    test_items = []
    if args.tests:
        for t in args.tests:
            # Can be "test/jit/basic", "build/test/jit/basic", "jit/basic", or "basic"
            path = t
            if not os.path.exists(path):
                # Try finding under build/test/* or test/*
                for base in [os.path.join(root_dir, "build", "test"), os.path.join(root_dir, "test")]:
                    for cat in ["unit", "semantic", "abi", "jit"]:
                        cand = os.path.join(base, cat, os.path.basename(t))
                        if os.path.isfile(cand):
                            path = cand
                            break
                    if os.path.isfile(path):
                        break
            if os.path.isfile(path):
                # Determine category and relative name
                rel = None
                for base in [os.path.join(root_dir, "build", "test"), os.path.join(root_dir, "test")]:
                    if path.startswith(base):
                        rel = os.path.relpath(path, base)
                        break
                if not rel:
                    rel = os.path.basename(path)
                cat = rel.split(os.sep)[0] if os.sep in rel else "custom"
                test_items.append((rel, path, cat))
            else:
                print(f"Error: Test binary '{t}' not found.", file=sys.stderr)
                sys.exit(1)
    else:
        test_items = find_test_binaries(root_dir, category=args.category)

    if not test_items:
        print("No test binaries found to run. Did you run 'make' first?", file=sys.stderr)
        sys.exit(1)

    if not args.tap:
        cat_str = f" [{args.category}]" if args.category else ""
        print(colorize(f"=== ZeroCC Test Suite{cat_str} ({len(test_items)} tests) ===", BOLD))

    results = []
    overall_start = time.perf_counter()

    for name, path, category in test_items:
        res = run_single_test(name, path, category, timeout_sec=args.timeout, use_valgrind=args.valgrind)
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
