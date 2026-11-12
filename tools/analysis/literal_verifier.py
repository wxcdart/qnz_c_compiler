#!/usr/bin/env python3
"""
ZeroCC Literal & Expression Verification Suite
Automated differential testing for C literals and constant expressions:
  - int: decimal, octal, hex, binary (0b), digit separators ('), suffixes (u, l, ll, ull)
  - float: decimal, leading/trailing dot, scientific (e/E), hex floats (0x1.8p1), suffixes (f/F, l/L)
  - char: single char, escape sequences, octal/hex escapes, multi-character constants ('abcd')
  - string: simple, escapes, adjacent string concatenation, char array initialization
  - compound: block-scope and file-scope compound literals ((type){...})
  - predefined: __func__, etc.

Usage:
  python3 tools/literal_verifier.py --all
  python3 tools/literal_verifier.py --feature int
  python3 tools/literal_verifier.py --feature float
  python3 tools/literal_verifier.py --feature char
  python3 tools/literal_verifier.py --feature string
  python3 tools/literal_verifier.py --feature compound
  python3 tools/literal_verifier.py --diff-gcc
"""

import argparse
import subprocess
import sys
import os
import tempfile
from dataclasses import dataclass
from typing import List, Optional

@dataclass
class LiteralTestCase:
    name: str
    feature: str  # "int", "float", "char", "string", "compound", "predefined"
    source: str
    expected_exit: int = 0
    should_compile: bool = True
    expect_error: Optional[str] = None

TEST_CASES = [
    # -------------------------------------------------------------
    # 1. Integer Literals
    # -------------------------------------------------------------
    LiteralTestCase(
        name="int_decimal_basic",
        feature="int",
        source="""
int main(void) {
    int x = 12345;
    if (x != 12345) return 1;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="int_octal_basic",
        feature="int",
        source="""
int main(void) {
    int x = 077;
    if (x != 63) return 1;
    int y = 00;
    if (y != 0) return 2;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="int_hex_basic",
        feature="int",
        source="""
int main(void) {
    int a = 0x2A;
    int b = 0XFF;
    int c = 0xabcdef;
    if (a != 42) return 1;
    if (b != 255) return 2;
    if (c != 11259375) return 3;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="int_binary_basic",
        feature="int",
        source="""
int main(void) {
    int x = 0b101010;
    int y = 0B11110000;
    if (x != 42) return 1;
    if (y != 240) return 2;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="int_digit_separators",
        feature="int",
        source="""
int main(void) {
    int dec = 1'000'000;
    int hex = 0xDE'AD'BE'EF;
    int bin = 0b1111'0000'1010'0101;
    int oct = 012'34;
    if (dec != 1000000) return 1;
    if ((unsigned int)hex != 0xDEADBEEFU) return 2;
    if (bin != 0xF0A5) return 3;
    if (oct != 01234) return 4;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="int_suffixes",
        feature="int",
        source="""
int main(void) {
    unsigned int u = 42U;
    long l = 100L;
    unsigned long ul = 200UL;
    long long ll = 300LL;
    unsigned long long ull = 400ULL;
    if (u != 42) return 1;
    if (l != 100) return 2;
    if (ul != 200) return 3;
    if (ll != 300) return 4;
    if (ull != 400) return 5;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 2. Floating-Point Literals
    # -------------------------------------------------------------
    LiteralTestCase(
        name="float_decimal_and_dots",
        feature="float",
        source="""
int main(void) {
    double d1 = 1.25;
    double d2 = .5;
    double d3 = 5.;
    if (d1 != 1.25) return 1;
    if (d2 != 0.5) return 2;
    if (d3 != 5.0) return 3;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="float_scientific_notation",
        feature="float",
        source="""
int main(void) {
    double a = 1e3;
    double b = 2.5e-2;
    double c = 3.0E+2;
    if (a != 1000.0) return 1;
    if (b != 0.025) return 2;
    if (c != 300.0) return 3;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="float_hex_representation",
        feature="float",
        source="""
int main(void) {
    double a = 0x1.8p1;   // 1.5 * 2^1 = 3.0
    double b = 0x1.0p-2;  // 1.0 * 2^-2 = 0.25
    if (a != 3.0) return 1;
    if (b != 0.25) return 2;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="float_suffixes_and_sizeof",
        feature="float",
        source="""
int main(void) {
    if (sizeof(1.0f) != 4) return 1;
    if (sizeof(1.0F) != 4) return 2;
    if (sizeof(1.0) != 8) return 3;
    if (sizeof(1.0L) < 8) return 4;
    float f = 1.5f;
    if (f != 1.5) return 5;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 3. Character Literals
    # -------------------------------------------------------------
    LiteralTestCase(
        name="char_basic_and_escapes",
        feature="char",
        source="""
int main(void) {
    char c = 'a';
    if (c != 97) return 1;
    if ('\\n' != 10) return 2;
    if ('\\t' != 9) return 3;
    if ('\\r' != 13) return 4;
    if ('\\0' != 0) return 5;
    if ('\\\\' != 92) return 6;
    if ('\\'' != 39) return 7;
    if ('\\"' != 34) return 8;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="char_octal_and_hex_escapes",
        feature="char",
        source="""
int main(void) {
    if ('\\101' != 65) return 1;  // 'A'
    if ('\\x41' != 65) return 2;  // 'A'
    if ('\\x7F' != 127) return 3;
    if ('\\077' != 63) return 4;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="char_multi_char_constant",
        feature="char",
        source="""
int main(void) {
    int m2 = 'AB';
    int m4 = 'ABCD';
    if (m2 != ((65 << 8) | 66)) return 1;
    if (m4 != ((((((65 << 8) | 66) << 8) | 67) << 8) | 68)) return 2;
    if (sizeof('a') != sizeof(int)) return 3;
    int esc_m = '\\n\\t';
    if (esc_m != ((10 << 8) | 9)) return 4;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 4. String Literals
    # -------------------------------------------------------------
    LiteralTestCase(
        name="string_basic_and_indexing",
        feature="string",
        source="""
int main(void) {
    char *s = "hello";
    if (s[0] != 'h') return 1;
    if (s[4] != 'o') return 2;
    if (s[5] != '\\0') return 3;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="string_concatenation",
        feature="string",
        source="""
int main(void) {
    char *s = "hello " "world" "!";
    if (s[0] != 'h') return 1;
    if (s[6] != 'w') return 2;
    if (s[11] != '!') return 3;
    if (s[12] != '\\0') return 4;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="string_octal_and_hex_escapes",
        feature="string",
        source="""
int main(void) {
    char *s1 = "\\101\\102\\103";
    char *s2 = "\\x44\\x45";
    if (s1[0] != 'A' || s1[1] != 'B' || s1[2] != 'C') return 1;
    if (s2[0] != 'D' || s2[1] != 'E') return 2;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="string_array_initialization",
        feature="string",
        source="""
int main(void) {
    char buf[] = "abc";
    if (sizeof(buf) != 4) return 1;
    if (buf[0] != 'a') return 2;
    if (buf[1] != 'b') return 3;
    if (buf[2] != 'c') return 4;
    if (buf[3] != '\\0') return 5;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 5. Compound Literals
    # -------------------------------------------------------------
    LiteralTestCase(
        name="compound_literal_array_block_scope",
        feature="compound",
        source="""
int main(void) {
    int *p = (int[]){10, 20, 30};
    if (p[0] != 10) return 1;
    if (p[1] != 20) return 2;
    if (p[2] != 30) return 3;
    int sum = ((int[]){1, 2, 3, 4})[2];
    if (sum != 3) return 4;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="compound_literal_struct_block_scope",
        feature="compound",
        source="""
struct Point {
    int x;
    int y;
};
int sum_point(struct Point pt) {
    return pt.x + pt.y;
}
int main(void) {
    struct Point pt = (struct Point){10, 20};
    if (pt.x != 10 || pt.y != 20) return 1;
    if (sum_point((struct Point){30, 40}) != 70) return 2;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="compound_literal_file_scope",
        feature="compound",
        source="""
static int *global_arr = (int[]){100, 200, 300};
int main(void) {
    if (global_arr[0] != 100) return 1;
    if (global_arr[1] != 200) return 2;
    if (global_arr[2] != 300) return 3;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="compound_literal_file_scope_struct_ptr",
        feature="compound",
        source="""
struct Point {
    int x;
    int y;
};
static struct Point *global_pt = &(struct Point){10, 20};
int main(void) {
    if (global_pt->x != 10) return 1;
    if (global_pt->y != 20) return 2;
    return 0;
}
"""
    ),
    LiteralTestCase(
        name="compound_literal_file_scope_struct_direct",
        feature="compound",
        source="""
struct Point {
    int x;
    int y;
};
static struct Point global_pt = (struct Point){30, 40};
int main(void) {
    if (global_pt.x != 30) return 1;
    if (global_pt.y != 40) return 2;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 6. Predefined Expressions
    # -------------------------------------------------------------
    LiteralTestCase(
        name="predefined_func_name",
        feature="predefined",
        source="""
int check_func(void) {
    const char *name = __func__;
    if (name[0] != 'c' || name[5] != '_' || name[9] != 'c') return 1;
    return 0;
}
int main(void) {
    const char *main_name = __func__;
    if (main_name[0] != 'm' || main_name[3] != 'n') return 1;
    return check_func();
}
"""
    ),
]

def run_test(compiler: str, test: LiteralTestCase, verbose: bool = False, diff_gcc: bool = False) -> bool:
    with tempfile.NamedTemporaryFile(suffix=".c", mode="w", delete=False) as f:
        f.write(test.source)
        src_path = f.name

    bin_path = src_path + ".bin"
    gcc_bin = src_path + ".gcc.bin"
    try:
        # Differential verification against host GCC if requested
        if diff_gcc and test.should_compile:
            gcc_res = subprocess.run(["gcc", "-std=c11", "-O2", src_path, "-o", gcc_bin],
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
            print(f"  [FAIL] {test.name}: compile error:\n{proc.stderr}")
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
    parser = argparse.ArgumentParser(description="ZeroCC Literal & Expression Verification Suite")
    parser.add_argument("--compiler", default="./build/bin/zerocc", help="Path to zerocc")
    parser.add_argument("--all", action="store_true", help="Run all tests")
    parser.add_argument("--feature", choices=["int", "float", "char", "string", "compound", "predefined"], help="Filter by feature")
    parser.add_argument("--diff-gcc", action="store_true", help="Cross-verify test with GCC")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    tests = TEST_CASES
    if args.feature:
        tests = [t for t in tests if t.feature == args.feature]

    passed = 0
    total = len(tests)
    print(f"Running {total} literal & expression verification test(s)...")
    for t in tests:
        if run_test(args.compiler, t, args.verbose, args.diff_gcc):
            passed += 1

    print("=" * 60)
    print(f"Literal Test Results: {passed}/{total} passed")
    print("=" * 60)
    sys.exit(0 if passed == total else 1)

if __name__ == "__main__":
    main()
