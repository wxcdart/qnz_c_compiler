#!/usr/bin/env python3
"""
C11 Verification Suite
Automated testing for C11 features in ZeroCC:
  - Part A: Core language features (_Alignas, _Alignof, _Static_assert, _Generic, anonymous structs/unions)
  - Part B: Threading and Atomics (<threads.h>, <stdatomic.h>, TLS, atomic builtins)

Usage:
  python3 tools/c11/test_c11.py --all
  python3 tools/c11/test_c11.py --feature align
  python3 tools/c11/test_c11.py --feature threads
  python3 tools/c11/test_c11.py --feature atomics
"""

import argparse
import subprocess
import sys
import os
import tempfile
from dataclasses import dataclass
from typing import List, Optional

@dataclass
class C11TestCase:
    name: str
    feature: str  # "align", "threads", "atomics", "generic", "keywords"
    source: str
    expected_exit: int = 0
    should_compile: bool = True
    expect_error: Optional[str] = None
    needs_pthread: bool = False

TEST_CASES = [
    # -------------------------------------------------------------
    # 1. _Alignof and _Alignas
    # -------------------------------------------------------------
    C11TestCase(
        name="alignof_fundamental_types",
        feature="align",
        source="""
int main(void) {
    if (_Alignof(char) != 1) return 1;
    if (_Alignof(short) != 2) return 2;
    if (_Alignof(int) != 4) return 3;
    if (_Alignof(double) != 8) return 4;
    return 0;
}
"""
    ),
    C11TestCase(
        name="alignof_struct_type",
        feature="align",
        source="""
struct S {
    char c;
    double d;
};
int main(void) {
    if (_Alignof(struct S) != 8) return 1;
    return 0;
}
"""
    ),
    C11TestCase(
        name="alignas_struct_member",
        feature="align",
        source="""
struct AlignedStruct {
    char a;
    _Alignas(16) int b;
};
int main(void) {
    struct AlignedStruct s;
    long pa = (long)&s.a;
    long pb = (long)&s.b;
    if ((pb - pa) < 16) return 1;
    if ((pb % 16) != 0) return 2;
    return 0;
}
"""
    ),
    C11TestCase(
        name="alignas_local_variables",
        feature="align",
        source="""
int main(void) {
    _Alignas(8) int x = 42;
    _Alignas(16) char buf[10];
    long px = (long)&x;
    long pbuf = (long)&buf[0];
    if ((px % 8) != 0) return 1;
    if ((pbuf % 16) != 0) return 2;
    if (x != 42) return 3;
    return 0;
}
"""
    ),
    C11TestCase(
        name="alignas_with_alignof_type",
        feature="align",
        source="""
int main(void) {
    _Alignas(double) char c = 'Z';
    long pc = (long)&c;
    if ((pc % _Alignof(double)) != 0) return 1;
    if (c != 'Z') return 2;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 2. Anonymous Structs and Unions
    # -------------------------------------------------------------
    C11TestCase(
        name="anonymous_struct_in_union",
        feature="keywords",
        source="""
struct Packet {
    int id;
    union {
        struct {
            short code;
            short flags;
        };
        int raw_data;
    };
};
int main(void) {
    struct Packet p;
    p.id = 100;
    p.code = 12;
    p.flags = 34;
    if (p.id != 100) return 1;
    if (p.code != 12) return 2;
    if (p.flags != 34) return 3;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 3. _Generic expressions
    # -------------------------------------------------------------
    C11TestCase(
        name="generic_selection",
        feature="generic",
        source="""
#define typename(x) _Generic((x), \
    int: 1, \
    double: 2, \
    char *: 3, \
    default: 0)

int main(void) {
    int a = 5;
    double b = 3.14;
    char *c = "test";
    if (typename(a) != 1) return 1;
    if (typename(b) != 2) return 2;
    if (typename(c) != 3) return 3;
    if (typename((float)1.0) != 0) return 4;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 4. _Static_assert
    # -------------------------------------------------------------
    C11TestCase(
        name="static_assert_pass",
        feature="keywords",
        source="""
_Static_assert(sizeof(int) == 4, "int must be 4 bytes");
_Static_assert(_Alignof(double) == 8, "double align must be 8");
int main(void) {
    _Static_assert(1 + 1 == 2, "math works");
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 5. Atomic Builtins and Operations
    # -------------------------------------------------------------
    C11TestCase(
        name="atomic_sync_fetch_and_add",
        feature="atomics",
        source="""
int main(void) {
    int count = 10;
    int prev = __sync_fetch_and_add(&count, 5);
    if (prev != 10) return 1;
    if (count != 15) return 2;
    prev = __sync_val_compare_and_swap(&count, 15, 42);
    if (prev != 15) return 3;
    if (count != 42) return 4;
    return 0;
}
"""
    ),

    # -------------------------------------------------------------
    # 6. Threads and Mutexes (<threads.h> / pthread backing)
    # -------------------------------------------------------------
    C11TestCase(
        name="threads_basic_join",
        feature="threads",
        source="""
#include "include/threads.h"

int worker(void *arg) {
    int *val = (int *)arg;
    *val += 50;
    return 0;
}

int main(void) {
    thrd_t t;
    int data = 100;
    if (thrd_create(&t, worker, &data) != thrd_success) return 1;
    int res = 0;
    if (thrd_join(t, &res) != thrd_success) return 2;
    if (data != 150) return 3;
    return 0;
}
""",
        needs_pthread=True
    ),
    C11TestCase(
        name="threads_mutex_lock",
        feature="threads",
        source="""
#include "include/threads.h"

mtx_t lock;
int counter = 0;

int worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        mtx_lock(&lock);
        counter++;
        mtx_unlock(&lock);
    }
    return 0;
}

int main(void) {
    thrd_t t1, t2;
    if (mtx_init(&lock, mtx_plain) != thrd_success) return 1;
    if (thrd_create(&t1, worker, 0) != thrd_success) return 2;
    if (thrd_create(&t2, worker, 0) != thrd_success) return 3;
    thrd_join(t1, 0);
    thrd_join(t2, 0);
    mtx_destroy(&lock);
    if (counter != 2000) return 4;
    return 0;
}
""",
        needs_pthread=True
    ),
]

def run_test(compiler: str, test: C11TestCase, verbose: bool = False) -> bool:
    with tempfile.NamedTemporaryFile(suffix=".c", mode="w", delete=False) as f:
        f.write(test.source)
        src_path = f.name

    bin_path = src_path + ".bin"
    try:
        # 1. Compile AOT
        cmd = [compiler, "-I.", src_path, "-o", bin_path]
        if test.needs_pthread:
            cmd.extend(["-lpthread"])
        
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

        # 3. Run JIT (--run) if not multi-threaded
        if not test.needs_pthread:
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

def main():
    parser = argparse.ArgumentParser(description="ZeroCC C11 Test Suite")
    parser.add_argument("--compiler", default="./build/bin/zerocc", help="Path to zerocc")
    parser.add_argument("--all", action="store_true", help="Run all tests")
    parser.add_argument("--feature", choices=["align", "threads", "atomics", "generic", "keywords"], help="Filter by feature")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    tests = TEST_CASES
    if args.feature:
        tests = [t for t in tests if t.feature == args.feature]

    passed = 0
    total = len(tests)
    print(f"Running {total} C11 verification test(s)...")
    for t in tests:
        if run_test(args.compiler, t, args.verbose):
            passed += 1

    print("=" * 60)
    print(f"C11 Test Results: {passed}/{total} passed")
    print("=" * 60)
    sys.exit(0 if passed == total else 1)

if __name__ == "__main__":
    main()
