#!/usr/bin/env python3
"""
ZeroCC GNU/Clang C Extension Verification Suite
Automated differential testing for pervasive C language extensions:
  - stmt_expr: GNU statement expressions ({ stmt; expr; })
  - case_range: Case ranges in switch statements (case '0' ... '9':)
  - init_range: Designated initializer index ranges ([0 ... 9] = 1)
  - offsetof: __builtin_offsetof(type, member)

Usage:
  python3 tools/extension_verifier.py --all
  python3 tools/extension_verifier.py --feature stmt_expr
  python3 tools/extension_verifier.py --feature case_range
  python3 tools/extension_verifier.py --feature init_range
  python3 tools/extension_verifier.py --feature offsetof
  python3 tools/extension_verifier.py --diff-gcc
"""

import argparse
import subprocess
import sys
import os
import tempfile
from dataclasses import dataclass
from typing import List, Optional

@dataclass
class ExtensionTestCase:
    name: str
    feature: str  # "stmt_expr", "case_range", "init_range", "offsetof"
    source: str
    expected_exit: int = 0
    should_compile: bool = True
    expect_error: Optional[str] = None

TEST_CASES = [
    # -------------------------------------------------------------
    # 1. Statement Expressions: ({ ... })
    # -------------------------------------------------------------
    ExtensionTestCase(
        name="stmt_expr_basic",
        feature="stmt_expr",
        source="""
int main(void) {
    int x = ({ int a = 10; int b = 20; a + b; });
    if (x != 30) return 1;
    return 0;
}
"""
    ),
    ExtensionTestCase(
        name="stmt_expr_with_loop",
        feature="stmt_expr",
        source="""
int main(void) {
    int sum = ({
        int s = 0;
        for (int i = 1; i <= 5; i++) {
            s += i;
        }
        s;
    });
    if (sum != 15) return 1;
    return 0;
}
"""
    ),
    ExtensionTestCase(
        name="stmt_expr_macro_max",
        feature="stmt_expr",
        source="""
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })

int main(void) {
    int a = 10;
    int b = 25;
    int m = max(a++, b++);
    if (m != 25) return 1;
    if (a != 11 || b != 26) return 2;
    return 0;
}
"""
    ),
    ExtensionTestCase(
        name="stmt_expr_nested",
        feature="stmt_expr",
        source="""
int main(void) {
    int res = ({
        int x = ({ int a = 2; a * 3; });
        int y = ({ int b = 4; b * 5; });
        x + y;
    });
    if (res != 26) return 1;
    return 0;
}
"""
    ),
    ExtensionTestCase(
        name="stmt_expr_struct_return",
        feature="stmt_expr",
        source="""
struct Point {
    int x;
    int y;
};
int main(void) {
    struct Point pt = ({
        struct Point p;
        p.x = 100;
        p.y = 200;
        p;
    });
    if (pt.x != 100 || pt.y != 200) return 1;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 2. Case Ranges: case low ... high:
    # -------------------------------------------------------------
    ExtensionTestCase(
        name="case_range_chars",
        feature="case_range",
        source="""
int classify(char c) {
    switch (c) {
        case '0' ... '9':
            return 1;
        case 'a' ... 'z':
            return 2;
        case 'A' ... 'Z':
            return 3;
        default:
            return 0;
    }
}
int main(void) {
    if (classify('5') != 1) return 1;
    if (classify('m') != 2) return 2;
    if (classify('Z') != 3) return 3;
    if (classify('!') != 0) return 4;
    return 0;
}
"""
    ),
    ExtensionTestCase(
        name="case_range_integers",
        feature="case_range",
        source="""
int check_bucket(int val) {
    switch (val) {
        case 1 ... 10:
            return 10;
        case 11 ... 20:
            return 20;
        case 21 ... 30:
            return 30;
        default:
            return -1;
    }
}
int main(void) {
    if (check_bucket(1) != 10) return 1;
    if (check_bucket(10) != 10) return 2;
    if (check_bucket(11) != 20) return 3;
    if (check_bucket(20) != 20) return 4;
    if (check_bucket(25) != 30) return 5;
    if (check_bucket(50) != -1) return 6;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 3. Designated Initializer Ranges: [low ... high] = val
    # -------------------------------------------------------------
    ExtensionTestCase(
        name="init_range_basic",
        feature="init_range",
        source="""
int main(void) {
    int arr[10] = { [0 ... 4] = 1, [5 ... 9] = 2 };
    for (int i = 0; i < 5; i++) {
        if (arr[i] != 1) return 1;
    }
    for (int i = 5; i < 10; i++) {
        if (arr[i] != 2) return 2;
    }
    return 0;
}
"""
    ),
    ExtensionTestCase(
        name="init_range_ascii_table",
        feature="init_range",
        source="""
int main(void) {
    char is_digit[256] = {
        ['0' ... '9'] = 1
    };
    if (is_digit['0'] != 1) return 1;
    if (is_digit['5'] != 1) return 2;
    if (is_digit['9'] != 1) return 3;
    if (is_digit['a'] != 0) return 4;
    if (is_digit[' '] != 0) return 5;
    return 0;
}
"""
    ),
    ExtensionTestCase(
        name="init_range_file_scope",
        feature="init_range",
        source="""
static int global_arr[10] = { [0 ... 4] = 42, [5 ... 9] = 99 };
int main(void) {
    for (int i = 0; i < 5; i++) {
        if (global_arr[i] != 42) return 1;
    }
    for (int i = 5; i < 10; i++) {
        if (global_arr[i] != 99) return 2;
    }
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 4. __builtin_offsetof
    # -------------------------------------------------------------
    ExtensionTestCase(
        name="offsetof_basic",
        feature="offsetof",
        source="""
struct S {
    char a;
    int b;
    double c;
};
int main(void) {
    if (__builtin_offsetof(struct S, a) != 0) return 1;
    if (__builtin_offsetof(struct S, b) != 4) return 2;
    if (__builtin_offsetof(struct S, c) != 8) return 3;
    return 0;
}
"""
    ),
    ExtensionTestCase(
        name="offsetof_nested_struct",
        feature="offsetof",
        source="""
struct Inner {
    int x;
    int y;
};
struct Outer {
    char c;
    struct Inner in;
};
int main(void) {
    if (__builtin_offsetof(struct Outer, c) != 0) return 1;
    if (__builtin_offsetof(struct Outer, in) != 4) return 2;
    if (__builtin_offsetof(struct Outer, in.x) != 4) return 3;
    if (__builtin_offsetof(struct Outer, in.y) != 8) return 4;
    return 0;
}
"""
    ),
    ExtensionTestCase(
        name="offsetof_array_member",
        feature="offsetof",
        source="""
struct ArrStruct {
    char pad;
    int buf[4];
};
int main(void) {
    if (__builtin_offsetof(struct ArrStruct, buf[0]) != 4) return 1;
    if (__builtin_offsetof(struct ArrStruct, buf[1]) != 8) return 2;
    if (__builtin_offsetof(struct ArrStruct, buf[3]) != 16) return 3;
    return 0;
}
"""
    ),
    ExtensionTestCase(
        name="offsetof_static_assert",
        feature="offsetof",
        source="""
struct S {
    char c;
    int x;
};
_Static_assert(__builtin_offsetof(struct S, x) == 4, "offset check");
int main(void) {
    return 0;
}
"""
    ),
]

def run_test(compiler: str, test: ExtensionTestCase, verbose: bool = False, diff_gcc: bool = False) -> bool:
    with tempfile.NamedTemporaryFile(suffix=".c", mode="w", delete=False) as f:
        f.write(test.source)
        src_path = f.name

    bin_path = src_path + ".bin"
    gcc_bin = src_path + ".gcc.bin"
    try:
        # Differential verification against host GCC if requested
        if diff_gcc and test.should_compile:
            gcc_res = subprocess.run(["gcc", "-std=gnu11", "-O2", src_path, "-o", gcc_bin],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if gcc_res.returncode == 0:
                gcc_run = subprocess.run([gcc_bin], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                if gcc_run.returncode != test.expected_exit:
                    print(f"  [WARN] GCC exit code {gcc_run.returncode} != test expected {test.expected_exit}")

        # 1. Compile AOT with ZeroCC
        cmd = [compiler, "-I.", src_path, "-o", bin_path]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if not test.should_compile:
            if proc.returncode != 0:
                print(f"  [PASS] {test.name} (negative compile check as expected)")
                return True
            else:
                print(f"  [FAIL] {test.name}: expected compile error but succeeded")
                return False

        if proc.returncode != 0:
            if verbose:
                print(f"  [FAIL] {test.name}: compile error:\n{proc.stderr}")
            else:
                first_line = proc.stderr.strip().splitlines()[0] if proc.stderr else "unknown error"
                print(f"  [FAIL] {test.name}: compile error: {first_line}")
            return False

        # 2. Run AOT binary
        run_proc = subprocess.run([bin_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if run_proc.returncode != test.expected_exit:
            print(f"  [FAIL] {test.name}: AOT exit {run_proc.returncode} != expected {test.expected_exit}")
            return False

        # 3. Run JIT (--run)
        jit_cmd = [compiler, "-I.", "--run", src_path]
        jit_proc = subprocess.run(jit_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if jit_proc.returncode != test.expected_exit:
            print(f"  [FAIL] {test.name}: JIT exit {jit_proc.returncode} != expected {test.expected_exit}")
            return False

        print(f"  [PASS] {test.name}")
        return True
    finally:
        if os.path.exists(src_path): os.unlink(src_path)
        if os.path.exists(bin_path): os.unlink(bin_path)
        if os.path.exists(gcc_bin): os.unlink(gcc_bin)

def main():
    parser = argparse.ArgumentParser(description="ZeroCC C Extension Verification Suite")
    parser.add_argument("--compiler", default="./build/bin/zerocc", help="Path to zerocc")
    parser.add_argument("--all", action="store_true", help="Run all tests")
    parser.add_argument("--feature", choices=["stmt_expr", "case_range", "init_range", "offsetof"], help="Filter by feature")
    parser.add_argument("--diff-gcc", action="store_true", help="Cross-verify test with GCC")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    tests = TEST_CASES
    if args.feature:
        tests = [t for t in tests if t.feature == args.feature]

    passed = 0
    total = len(tests)
    print(f"Running {total} GNU/Clang extension verification test(s)...")
    for t in tests:
        if run_test(args.compiler, t, args.verbose, args.diff_gcc):
            passed += 1

    print("=" * 60)
    print(f"Extension Test Results: {passed}/{total} passed")
    print("=" * 60)
    sys.exit(0 if passed == total else 1)

if __name__ == "__main__":
    main()
