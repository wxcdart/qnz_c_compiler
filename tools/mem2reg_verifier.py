#!/usr/bin/env python3
"""
ZeroCC Memory-to-Register (mem2reg) Verifier & Benchmark Suite

Verifies:
1. Correctness across straight-line code, loops, branches, nested loops, and joins.
2. Safety invariants: address-taken variables (&x), function calls clobbering caller-saved regs,
   and local array aliasing on the stack.
3. GCC parity: cross-checks outputs and return codes across JIT (--run), AOT (-o), and GCC -O2.
4. Stack traffic analysis: parses generated assembly (-S) to count stack loads and stores
   in inner loop bodies before vs after cross-block mem2reg.
5. Performance benchmarking: measures loop execution time and speedup factors against GCC.

Usage:
    python3 tools/mem2reg_verifier.py --verify-all
    python3 tools/mem2reg_verifier.py --count-stack-ops
    python3 tools/mem2reg_verifier.py --benchmark
    python3 tools/mem2reg_verifier.py --test loop_simple
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from typing import Dict, List, Tuple, Optional

ZEROCC_BIN = "./build/bin/zerocc"
GCC_BIN = "gcc"

TEST_CASES = {
    "straightline": {
        "desc": "Single basic block scalar variable promotion",
        "code": """
#include <stdio.h>
int main(void) {
    int a = 10;
    int b = 20;
    int c = a + b;
    int d = c * 2;
    int e = d - a;
    if (e != 50) return 1;
    printf("OK: straightline e=%d\\n", e);
    return 0;
}
""",
        "loop_fn": None,
    },
    "loop_simple": {
        "desc": "Simple for-loop induction and accumulator",
        "code": """
#include <stdio.h>
int sum_loop(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += i;
    }
    return sum;
}
int main(void) {
    int res = sum_loop(100);
    if (res != 4950) return 1;
    printf("OK: loop_simple sum=%d\\n", res);
    return 0;
}
""",
        "loop_fn": "sum_loop",
    },
    "loop_while": {
        "desc": "While loop with counter decrement",
        "code": """
#include <stdio.h>
int countdown_loop(int n) {
    int total = 0;
    while (n > 0) {
        total += n;
        n--;
    }
    return total;
}
int main(void) {
    int res = countdown_loop(50);
    if (res != 1275) return 1;
    printf("OK: loop_while total=%d\\n", res);
    return 0;
}
""",
        "loop_fn": "countdown_loop",
    },
    "nested_loops": {
        "desc": "2D nested loop with independent induction variables",
        "code": """
#include <stdio.h>
int nested_sum(int rows, int cols) {
    int total = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            total += r * cols + c;
        }
    }
    return total;
}
int main(void) {
    int res = nested_sum(10, 10);
    if (res != 4950) return 1;
    printf("OK: nested_loops total=%d\\n", res);
    return 0;
}
""",
        "loop_fn": "nested_sum",
    },
    "branch_in_loop": {
        "desc": "Conditional update inside loop body",
        "code": """
#include <stdio.h>
int branch_loop(int n) {
    int evens = 0;
    int odds = 0;
    for (int i = 0; i < n; i++) {
        if (i % 2 == 0) {
            evens += i;
        } else {
            odds += i;
        }
    }
    return evens - odds;
}
int main(void) {
    int res = branch_loop(100);
    /* evens: 0+2+...+98 = 2450, odds: 1+3+...+99 = 2500, diff = -50 */
    if (res != -50) return 1;
    printf("OK: branch_in_loop diff=%d\\n", res);
    return 0;
}
""",
        "loop_fn": "branch_loop",
    },
    "if_else_join": {
        "desc": "Branch diamond with join point using promoted variable",
        "code": """
#include <stdio.h>
int diamond(int cond, int x) {
    int val = 0;
    if (cond) {
        val = x + 10;
    } else {
        val = x * 2;
    }
    return val + 5;
}
int main(void) {
    int r1 = diamond(1, 5);  /* 5 + 10 + 5 = 20 */
    int r2 = diamond(0, 5);  /* 5 * 2 + 5 = 15 */
    if (r1 != 20 || r2 != 15) return 1;
    printf("OK: if_else_join r1=%d r2=%d\\n", r1, r2);
    return 0;
}
""",
        "loop_fn": "diamond",
    },
    "address_taken_safe": {
        "desc": "Safety: address-taken variable modified via pointer (*p)",
        "code": """
#include <stdio.h>
int test_addr(void) {
    int x = 10;
    int *p = &x;
    *p = 42;
    return x; /* Must reload 42 from stack/memory, not use stale 10 */
}
int main(void) {
    int res = test_addr();
    if (res != 42) return 1;
    printf("OK: address_taken_safe res=%d\\n", res);
    return 0;
}
""",
        "loop_fn": "test_addr",
    },
    "call_in_loop_safe": {
        "desc": "Safety: function call inside loop body preserving state across call",
        "code": """
#include <stdio.h>
int helper(int v) {
    return v * 2;
}
int call_loop(int n) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += helper(i);
    }
    return total;
}
int main(void) {
    int res = call_loop(10); /* 2 * (0+1+...+9) = 90 */
    if (res != 90) return 1;
    printf("OK: call_in_loop_safe res=%d\\n", res);
    return 0;
}
""",
        "loop_fn": "call_loop",
    },
    "array_stack_safe": {
        "desc": "Safety: local array alongside scalar locals on stack",
        "code": """
#include <stdio.h>
int array_test(int n) {
    int arr[10];
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        arr[i] = i * 3;
    }
    for (int i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}
int main(void) {
    int res = array_test(10); /* 3 * 45 = 135 */
    if (res != 135) return 1;
    printf("OK: array_stack_safe res=%d\\n", res);
    return 0;
}
""",
        "loop_fn": "array_test",
    },
    "loop_heavy_bench": {
        "desc": "100M iterations counter & accumulator loop for benchmarking",
        "code": """
#include <stdio.h>
int compute_loop(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += (i & 1) ? 1 : 2;
    }
    return sum;
}
int main(void) {
    int res = compute_loop(100000000);
    printf("res=%d\\n", res);
    return 0;
}
""",
        "loop_fn": "compute_loop",
    },
}

def run_cmd(cmd: List[str], timeout: int = 15) -> Tuple[int, str, str]:
    """Execute a shell command and return (rc, stdout, stderr)"""
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "Command timed out after %ds" % timeout
    except Exception as e:
        return -1, "", str(e)

def verify_case(name: str, test_def: dict, verbose: bool = False) -> bool:
    """Run correctness verification for a single test case across JIT, AOT, and GCC."""
    code = test_def["code"]
    desc = test_def["desc"]

    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(code)
        src_path = f.name

    bin_gcc = src_path + ".gcc"
    bin_zerocc = src_path + ".zerocc"

    try:
        # 1. Compile with GCC -O2
        rc, out_gcc_c, err = run_cmd([GCC_BIN, "-O2", src_path, "-o", bin_gcc])
        if rc != 0:
            print(f"  [FAIL] GCC compilation failed for {name}: {err}")
            return False

        rc_gcc, out_gcc, _ = run_cmd([bin_gcc])
        if rc_gcc != 0:
            print(f"  [FAIL] GCC binary exited with {rc_gcc} for {name}")
            return False

        # 2. Run ZeroCC JIT (--run) at -O0 and -O1
        for opt_level in ["-O0", "-O1"]:
            rc_jit, out_jit, err_jit = run_cmd([ZEROCC_BIN, opt_level, "--run", src_path])
            if rc_jit != rc_gcc or out_jit.strip() != out_gcc.strip():
                print(f"  [FAIL] ZeroCC JIT {opt_level} mismatch for {name}:")
                print(f"         Expected rc={rc_gcc}, got rc={rc_jit}")
                print(f"         Expected out={out_gcc.strip()!r}")
                print(f"         Got out={out_jit.strip()!r}")
                if err_jit:
                    print(f"         stderr={err_jit}")
                return False

        # 3. Run ZeroCC AOT (-o) at -O1
        rc_aot_c, _, err_aot_c = run_cmd([ZEROCC_BIN, "-O1", src_path, "-o", bin_zerocc])
        if rc_aot_c != 0:
            print(f"  [FAIL] ZeroCC AOT compilation failed for {name}: {err_aot_c}")
            return False

        rc_aot, out_aot, _ = run_cmd([bin_zerocc])
        if rc_aot != rc_gcc or out_aot.strip() != out_gcc.strip():
            print(f"  [FAIL] ZeroCC AOT mismatch for {name}:")
            print(f"         Expected rc={rc_gcc}, got rc={rc_aot}")
            print(f"         Expected out={out_gcc.strip()!r}")
            print(f"         Got out={out_aot.strip()!r}")
            return False

        if verbose:
            print(f"  [PASS] {name} ({desc}): JIT (-O0, -O1) == AOT == GCC -O2 (output: {out_gcc.strip()!r})")
        else:
            print(f"  [PASS] {name}")
        return True

    finally:
        for path in [src_path, bin_gcc, bin_zerocc]:
            if os.path.exists(path):
                os.remove(path)

def count_loop_stack_ops(name: str, test_def: dict, opt_level: str = "-O1") -> Tuple[int, int]:
    """Compile with ZeroCC -S and count (%rbp) loads and stores in the loop body of loop_fn."""
    fn_name = test_def.get("loop_fn")
    if not fn_name:
        return 0, 0

    code = test_def["code"]
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(code)
        src_path = f.name
    asm_path = src_path + ".s"

    try:
        rc, _, err = run_cmd([ZEROCC_BIN, opt_level, "-S", src_path, "-o", asm_path])
        if rc != 0:
            print(f"  [FAIL] Compilation error for {name} ({opt_level}): {err}")
            return -1, -1

        with open(asm_path, "r") as f:
            asm_lines = f.readlines()

        # Find the function bounds
        in_fn = False
        fn_lines = []
        for line in asm_lines:
            if re.match(r"^\s*\.globl\s+" + fn_name, line) or re.match(r"^" + fn_name + r":", line):
                in_fn = True
            elif in_fn and (re.match(r"^\s*\.globl\s+", line) or (re.match(r"^[a-zA-Z_][a-zA-Z0-9_]*:", line) and not line.strip().startswith(".L"))):
                break
            if in_fn:
                fn_lines.append(line)

        # Count (%rbp) loads and stores in loop blocks
        # Loops in ZeroCC start with .L<fn>_<idx>:
        loads = 0
        stores = 0
        in_loop_body = False

        for line in fn_lines:
            line_str = line.strip()
            # Loop header/body detection
            if re.match(r"^\.L\d+_\d+:", line_str):
                in_loop_body = True
            elif in_loop_body and ("ret" in line_str or "leave" in line_str):
                in_loop_body = False

            if in_loop_body:
                # Stack slot read: mov[qlbws] -XX(%rbp), %reg
                if re.search(r"mov[a-z]*\s+-\d+\(%rbp\)\s*,", line_str):
                    loads += 1
                # Stack slot write: mov[qlbws] %reg, -XX(%rbp)
                elif re.search(r"mov[a-z]*\s+[^,]+,\s*-\d+\(%rbp\)", line_str):
                    stores += 1

        return loads, stores

    finally:
        for p in [src_path, asm_path]:
            if os.path.exists(p):
                os.remove(p)

def run_benchmark():
    """Benchmark tight 100M-iteration loop across ZeroCC and GCC optimization levels."""
    print("=" * 70)
    print("ZeroCC vs GCC Loop Performance Benchmark (100,000,000 Iterations)")
    print("=" * 70)

    test_def = TEST_CASES["loop_heavy_bench"]
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(test_def["code"])
        src_path = f.name

    results = []

    configs = [
        ("ZeroCC JIT -O0", [ZEROCC_BIN, "-O0", "--run", src_path]),
        ("ZeroCC JIT -O1", [ZEROCC_BIN, "-O1", "--run", src_path]),
        ("ZeroCC JIT -O2", [ZEROCC_BIN, "-O2", "--run", src_path]),
    ]

    bin_aot_o1 = src_path + ".zc.o1"
    run_cmd([ZEROCC_BIN, "-O1", src_path, "-o", bin_aot_o1])
    configs.append(("ZeroCC AOT -O1", [bin_aot_o1]))

    bin_aot_o2 = src_path + ".zc.o2"
    run_cmd([ZEROCC_BIN, "-O2", src_path, "-o", bin_aot_o2])
    configs.append(("ZeroCC AOT -O2", [bin_aot_o2]))

    bin_gcc_o0 = src_path + ".gcc.o0"
    run_cmd([GCC_BIN, "-O0", src_path, "-o", bin_gcc_o0])
    configs.append(("GCC -O0", [bin_gcc_o0]))

    bin_gcc_o2 = src_path + ".gcc.o2"
    run_cmd([GCC_BIN, "-O2", src_path, "-o", bin_gcc_o2])
    configs.append(("GCC -O2", [bin_gcc_o2]))

    for label, cmd in configs:
        # Warmup
        run_cmd(cmd)

        times = []
        for _ in range(3):
            t0 = time.perf_counter()
            rc, out, err = run_cmd(cmd, timeout=30)
            elapsed = time.perf_counter() - t0
            if rc == 0 and "res=" in out:
                times.append(elapsed)
            else:
                times = []
                break

        if times:
            best_time = min(times)
            results.append((label, best_time))
            print(f"  {label:<18} : {best_time*1000:7.2f} ms  ({100.0 / best_time:6.1f} M iter/sec)")
        else:
            print(f"  {label:<18} : FAILED")

    print("-" * 70)
    if len(results) >= 2:
        zc_time = next((t for l, t in results if "ZeroCC JIT -O1" in l), None)
        gcc_o2_time = next((t for l, t in results if "GCC -O2" in l), None)
        if zc_time and gcc_o2_time:
            ratio = zc_time / gcc_o2_time
            print(f"  Gap (ZeroCC -O1 vs GCC -O2): {ratio:.2f}x slower")
            print("  Target with cross-block mem2reg: close gap to ~1.0x - 1.3x")

    for p in [src_path, bin_aot_o1, bin_aot_o2, bin_gcc_o0, bin_gcc_o2]:
        if os.path.exists(p):
            os.remove(p)

def main():
    parser = argparse.ArgumentParser(description="ZeroCC mem2reg Verification & Benchmarking Tool")
    parser.add_argument("--verify-all", action="store_true", help="Run full correctness test suite against GCC")
    parser.add_argument("--count-stack-ops", action="store_true", help="Count stack loads/stores in loop bodies")
    parser.add_argument("--benchmark", action="store_true", help="Run 100M-iteration benchmark suite")
    parser.add_argument("--test", type=str, help="Run a specific test case by name")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose test reporting")

    args = parser.parse_args()

    if not os.path.exists(ZEROCC_BIN):
        print(f"Error: {ZEROCC_BIN} not found. Run 'make all' first.")
        sys.exit(1)

    if args.benchmark:
        run_benchmark()
        return

    if args.count_stack_ops:
        print("=" * 80)
        print("Loop Body Stack Traffic Analysis (-XX(%rbp) Loads and Stores)")
        print("=" * 80)
        print(f"{'Test Case':<20} | {'-O0 L/S':<10} | {'-O1 L/S':<10} | {'-O2 L/S':<10} | {'Reduction'}")
        print("-" * 80)
        for name, test_def in TEST_CASES.items():
            if not test_def.get("loop_fn"):
                continue
            l0, s0 = count_loop_stack_ops(name, test_def, "-O0")
            l1, s1 = count_loop_stack_ops(name, test_def, "-O1")
            l2, s2 = count_loop_stack_ops(name, test_def, "-O2")
            diff = f"-{l0 - l2} loads, -{s0 - s2} stores" if (l0 > l2 or s0 > s2) else "baseline"
            print(f"{name:<20} | {f'{l0}/{s0}':<10} | {f'{l1}/{s1}':<10} | {f'{l2}/{s2}':<10} | {diff}")
        print("-" * 80)
        return

    # Default or --verify-all
    tests_to_run = [args.test] if args.test else list(TEST_CASES.keys())
    if args.test and args.test not in TEST_CASES:
        print(f"Error: Unknown test '{args.test}'. Available: {list(TEST_CASES.keys())}")
        sys.exit(1)

    print("=" * 70)
    print(f"Running ZeroCC mem2reg Verification Suite ({len(tests_to_run)} tests)")
    print("=" * 70)

    passed = 0
    failed = 0
    for name in tests_to_run:
        if verify_case(name, TEST_CASES[name], verbose=args.verbose):
            passed += 1
        else:
            failed += 1

    print("-" * 70)
    print(f"Results: {passed} passed, {failed} failed (total {len(tests_to_run)})")
    if failed > 0:
        sys.exit(1)
    else:
        print("All mem2reg tests passed!")

if __name__ == "__main__":
    main()
