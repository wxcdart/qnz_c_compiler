#!/usr/bin/env python3
"""
C23 Differential Conformance Gate (Sprint 134)

Feature-tagged differential tests for the C23 completion track (plan item
#11). Every case is compiled and run with ZeroCC at -O0..-O3 in BOTH AOT
and JIT and compared (exit code + stdout) against a reference gcc built
with the matching `-std=` flag. Cases run under the dialect(s) they are
tagged with:

  - "gnu23"  : zerocc `-std=gnu23`  vs  gcc `-std=gnu23`
  - "c11"    : zerocc `-std=c11`    vs  gcc `-std=c11`
  - "c99"    : zerocc `-std=c99`    vs  gcc `-std=c99`
  - "legacy" : zerocc (no -std, default gnu11) vs gcc `-std=gnu11`

Negative cases (`expect_compile=False`) require accept/reject AGREEMENT:
the reference must reject the source, and zerocc must reject it too
(optionally with `expect_error_substr` in zerocc's stderr).

`stderr_must_contain` (warning cases) is checked in BOTH compilers'
stderr — a format-agnostic substring (the message core), not the full
diagnostic line.

Reference compiler: C23_GCC env override, else gcc-15, else gcc-14,
else gcc (the C23 features need a C23-capable reference; gcc-15 is the
pinned one).

Usage:
  python3 tools/c23/test_c23.py --all
  python3 tools/c23/test_c23.py --feature true_false
  python3 tools/c23/test_c23.py --feature warning --verbose
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from typing import List, Optional

ZEROCC = os.environ.get("ZEROCC", "./build/bin/zerocc")
GCC = os.environ.get("C23_GCC") or next(
    (c for c in ("gcc-15", "gcc-14", "gcc") if shutil.which(c)), "gcc")

OPT_LEVELS = [0, 1, 2, 3]
MODES = ["aot", "jit"]

# (zerocc -std flag or None, gcc -std flag)
DIALECTS = {
    "gnu23": ("-std=gnu23", "-std=gnu23"),
    "c11": ("-std=c11", "-std=c11"),
    "c99": ("-std=c99", "-std=c99"),
    "legacy": (None, "-std=gnu11"),
}


@dataclass
class C23Case:
    name: str
    feature: str            # std_version, true_false, constexpr, nullptr,
                            # designator, warning, c23_misc
    source: str
    dialects: List[str] = field(default_factory=lambda: ["gnu23"])
    expect_compile: bool = True
    expect_error_substr: Optional[str] = None   # in zerocc stderr (reject cases)
    stderr_must_contain: Optional[str] = None   # in BOTH stderrs (warning cases)


TEST_CASES: List[C23Case] = [
    # -------------------------------------------------------------
    # __STDC_VERSION__ follows the dialect (Sprint 130)
    # -------------------------------------------------------------
    C23Case("std_version_gnu23", "std_version", """
#if __STDC_VERSION__ != 202311L
#error "expected 202311L under gnu23"
#endif
int main(void) { return 0; }
""", dialects=["gnu23"]),
    C23Case("std_version_c11", "std_version", """
#if __STDC_VERSION__ != 201112L
#error "expected 201112L under c11"
#endif
int main(void) { return 0; }
""", dialects=["c11"]),
    C23Case("std_version_c99", "std_version", """
#if __STDC_VERSION__ != 199901L
#error "expected 199901L under c99"
#endif
int main(void) { return 0; }
""", dialects=["c99"]),
    C23Case("std_version_legacy_gnu11", "std_version", """
#if __STDC_VERSION__ != 201112L
#error "expected 201112L under default (gnu11)"
#endif
int main(void) { return 0; }
""", dialects=["legacy"]),

    # -------------------------------------------------------------
    # true / false keyword constants (C23 6.4.4.1, Sprint 130)
    # -------------------------------------------------------------
    C23Case("true_false_gnu23_keywords", "true_false", """
#include <stdbool.h>
int main(void) {
    bool b = true;
    if (true != 1) return 1;
    if (false != 0) return 2;
    if (sizeof(true) != sizeof(_Bool)) return 3;
    if (sizeof(true) != 1) return 4;
    int x = true + false;
    b = false;
    if (b == true) return 5;
    if (x != 1) return 6;
    return 0;
}
""", dialects=["gnu23"]),
    # the loop-9.c shape: `int false()` must keep working OUTSIDE C23
    C23Case("true_false_legacy_function", "true_false", """
int false(void) { return 42; }
int main(void) { return false() == 42 ? 0 : 1; }
""", dialects=["legacy"]),
    C23Case("true_false_legacy_enum", "true_false", """
enum E { false, true };
int main(void) { return (true == 1 && false == 0) ? 0 : 1; }
""", dialects=["legacy"]),
    C23Case("true_false_legacy_identifier", "true_false", """
int true = 10;
int main(void) { return true == 10 ? 0 : 1; }
""", dialects=["legacy"]),
    # under gnu23 the keywords are NOT identifiers: both compilers reject
    C23Case("true_false_gnu23_function_rejected", "true_false", """
int false(void) { return 42; }
int main(void) { return 0; }
""", dialects=["gnu23"], expect_compile=False),

    # -------------------------------------------------------------
    # constexpr data declarations (C23 6.7.2.4, Sprint 131)
    # -------------------------------------------------------------
    C23Case("constexpr_file_data", "constexpr", """
constexpr int n = 42;
constexpr int a[3] = { 1, 2, 3 };
struct S { int x, y; };
constexpr struct S s = { 7, 8 };
int main(void) {
    if (n != 42) return 1;
    if (a[0] != 1 || a[2] != 3) return 2;
    if (s.x != 7 || s.y != 8) return 3;
    return 0;
}
""", dialects=["gnu23"]),
    C23Case("constexpr_local_data", "constexpr", """
int main(void) {
    constexpr double d = 1.5;
    constexpr int i = 9;
    if (d != 1.5) return 1;
    if (i != 9) return 2;
    return 0;
}
""", dialects=["gnu23"]),
    # -------------------------------------------------------------
    # auto type deduction (C23 6.7.2.4, Sprint 179+)
    # Note: `auto *p = &x;` (pointer declarator directly after `auto`)
    # is a ZeroCC extension rejected by gcc, so it is NOT in this gate.
    # -------------------------------------------------------------
    C23Case("auto_local_basic", "auto", """
int main(void) {
    auto x = 42;
    auto d = 3.14;
    auto sum = x + (int)d;
    const auto cx = 100;
    if (x != 42) return 1;
    if ((int)d != 3) return 2;
    if (sum != 45) return 3;
    if (cx != 100) return 4;
    return 0;
}
""", dialects=["gnu23"]),
    C23Case("auto_noinit_rejected", "auto", """
int main(void) {
    auto x;
    return 0;
}
""", dialects=["gnu23"], expect_compile=False,
            expect_error_substr="cannot deduce type for 'auto' variable 'x' without initializer"),

    # -------------------------------------------------------------
    # nullptr (C23 6.4.4.3)
    # -------------------------------------------------------------
    C23Case("nullptr_semantics", "nullptr", """
int main(void) {
    int *p = nullptr;
    char *c = nullptr;
    if (p != 0) return 1;
    if (c != 0) return 2;
    if ((void *)nullptr != (void *)0) return 3;
    if (sizeof(nullptr) != sizeof(void *)) return 4;
    void *v = (void *)1;
    if (nullptr) return 5;
    if (!v) return 6;
    return 0;
}
""", dialects=["gnu23"]),

    # -------------------------------------------------------------
    # Unsized-array sizing from designators (C99 6.7.8p12 / C23 6.7.10,
    # Sprint 132) - verified in both dialects
    # -------------------------------------------------------------
    C23Case("designator_int", "designator", """
int fa[] = { [3] = 7, [0] = 1 };
int main(void) {
    int la[] = { [3] = 7, [0] = 1 };
    if (sizeof(fa) != 4 * sizeof(int)) return 1;
    if (fa[0] != 1 || fa[3] != 7 || fa[1] != 0) return 2;
    if (sizeof(la) != 4 * sizeof(int)) return 3;
    if (la[0] != 1 || la[3] != 7 || la[1] != 0) return 4;
    return 0;
}
""", dialects=["gnu23", "legacy"]),
    C23Case("designator_range", "designator", """
int r[] = { [1 ... 4] = 0, [0] = 9 };
int main(void) {
    if (sizeof(r) != 5 * sizeof(int)) return 1;
    if (r[0] != 9 || r[1] != 0 || r[4] != 0) return 2;
    return 0;
}
""", dialects=["gnu23", "legacy"]),
    C23Case("designator_struct_rows", "designator", """
struct S { int x, y; };
struct S s[] = { [3] = { 5, 6 }, [0] = { 9, 8 } };
int main(void) {
    if (sizeof(s) != 4 * sizeof(struct S)) return 1;
    if (s[0].x != 9 || s[3].x != 5 || s[3].y != 6) return 2;
    if (s[1].x != 0 || s[2].y != 0) return 3;
    return 0;
}
""", dialects=["gnu23", "legacy"]),

    # -------------------------------------------------------------
    # #warning directive (C23 6.9.1, Sprint 133)
    # -------------------------------------------------------------
    C23Case("warning_basic", "warning", """
#warning "c23 gate check"
int main(void) { return 0; }
""", dialects=["gnu23"], stderr_must_contain='#warning "c23 gate check"'),
    C23Case("warning_suppressed_by_pragma", "warning", """
#pragma GCC diagnostic ignored "-Wcpp"
#warning "suppressed c23 gate check"
int main(void) { return 0; }
""", dialects=["gnu23"],
            stderr_must_contain=None),  # compiled; message must NOT print -
                                        # checked explicitly below

    # -------------------------------------------------------------
    # Existing C23 features (regression guards)
    # -------------------------------------------------------------
    C23Case("typeof_basic", "c23_misc", """
int main(void) {
    typeof(int) x = 5;
    typeof(double) y = 1.5;
    if (x != 5) return 1;
    if (y != 1.5) return 2;
    return 0;
}
""", dialects=["gnu23"]),
    C23Case("typeof_unqual_basic", "c23_misc", """
struct P { int a; };
int main(void) {
    typeof_unqual(int) x = 3;
    typeof_unqual(struct P) p = { 4 };
    if (x != 3) return 1;
    if (p.a != 4) return 2;
    return 0;
}
""", dialects=["gnu23"]),
    C23Case("empty_braces_initializer", "c23_misc", """
struct S { int a, b; };
int main(void) {
    struct S s = {};
    int arr[3] = {};
    if (s.a != 0 || s.b != 0) return 1;
    if (arr[0] != 0 || arr[2] != 0) return 2;
    return 0;
}
""", dialects=["gnu23"]),
    C23Case("attribute_list_skip", "c23_misc", """
[[nodiscard]] int f(void) { return 1; }
int main(void) {
    [[maybe_unused]] int u = 5;
    int r;
    r = f();
    if (r != 1) return 1;
    return 0;
}
""", dialects=["gnu23"]),
    C23Case("typeof_dependent_param", "c23_misc", """
typeof(int) add(typeof(int) a, typeof(a) b) { return a + b; }
typeof(double) ptrdep(int *a, typeof(a) b) { (void)b; return 1.5; }
int main(void) {
    if (add(20, 22) != 42) return 1;
    int *p = (int *)0;
    if (ptrdep(p, p) != 1.5) return 2;
    return 0;
}
""", dialects=["gnu23"]),
    C23Case("enum_underlying_type", "c23_misc", """
enum E : unsigned { A = 2500 };
int main(void) {
    enum E e = A;
    if (e != 2500) return 1;
    if (sizeof(e) < 2) return 2;
    return 0;
}
""", dialects=["gnu23"]),
]

# The suppressed-warning case needs a NEGATIVE stderr check (the message
# must NOT appear). Track the substring to forbid per case name.
NEGATIVE_STDERR = {
    "warning_suppressed_by_pragma": 'suppressed c23 gate check',
}


def _run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, **kw)


def reference_out(src_path: str, gcc_std: str):
    """Build + run the reference. Returns (ok, rc, stdout, stderr).

    No -w: #warning cases assert on the reference's stderr message.
    """
    bin_path = src_path + ".ref"
    c = _run([GCC, gcc_std, "-O2", src_path, "-o", bin_path])
    if c.returncode != 0:
        return False, c.returncode, "", c.stderr
    r = _run([bin_path])
    return True, r.returncode, r.stdout, c.stderr


def zerocc_config(src_path: str, std_flag: Optional[str], mode: str, opt: int):
    """Compile (aot) or run (jit) one zerocc config. Returns (ok, rc, stdout, stderr)."""
    base = [ZEROCC]
    if std_flag:
        base.append(std_flag)
    base.append(f"-O{opt}")
    if mode == "aot":
        bin_path = f"{src_path}.z{opt}"
        c = _run(base + [src_path, "-o", bin_path])
        if c.returncode != 0:
            return False, c.returncode, "", c.stderr
        r = _run([bin_path])
        return True, r.returncode, r.stdout, c.stderr
    else:  # jit
        p = _run(base + ["--run", src_path])
        return True, p.returncode, p.stdout, p.stderr


def run_case(case: C23Case, verbose: bool) -> bool:
    with tempfile.NamedTemporaryFile(suffix=".c", mode="w", delete=False) as f:
        f.write(case.source)
        src = f.name
    artifacts = [src]
    ok = True
    try:
        for dialect in case.dialects:
            z_std, g_std = DIALECTS[dialect]
            ref_ok, ref_rc, ref_out, ref_err = reference_out(src, g_std)

            if not case.expect_compile:
                # accept/reject agreement: the reference must reject too
                if ref_ok:
                    print(f"  [FAIL] {case.name} [{dialect}]: reference compiled "
                          f"but the case expects rejection")
                    return False
                z_ok, z_rc, _, z_err = zerocc_config(src, z_std, "aot", 0)
                if z_ok:
                    print(f"  [FAIL] {case.name} [{dialect}]: zerocc compiled "
                          f"but the reference rejects")
                    ok = False
                    continue
                if case.expect_error_substr and \
                        case.expect_error_substr not in z_err:
                    print(f"  [FAIL] {case.name} [{dialect}]: zerocc rejected "
                          f"without '{case.expect_error_substr}' in stderr:\n{z_err}")
                    ok = False
                    continue
                tag = f" (rejected by both, '{case.expect_error_substr}' present)" \
                    if case.expect_error_substr else " (rejected by both)"
                if verbose:
                    print(f"  [PASS] {case.name} [{dialect}]{tag}")
                continue

            if not ref_ok:
                print(f"  [FAIL] {case.name} [{dialect}]: reference rejected:\n"
                      f"{ref_err}")
                return False

            neg = NEGATIVE_STDERR.get(case.name)
            if case.stderr_must_contain and \
                    case.stderr_must_contain not in ref_err:
                print(f"  [FAIL] {case.name} [{dialect}]: reference stderr "
                      f"missing '{case.stderr_must_contain}'")
                ok = False
                continue

            for mode in MODES:
                for opt in OPT_LEVELS:
                    z_ok, z_rc, z_out, z_err = zerocc_config(src, z_std, mode, opt)
                    cfg = f"{mode}-O{opt}"
                    if not z_ok:
                        print(f"  [FAIL] {case.name} [{dialect}] {cfg}: "
                              f"zerocc compile error:\n{z_err}")
                        ok = False
                        continue
                    if case.stderr_must_contain and \
                            case.stderr_must_contain not in z_err:
                        print(f"  [FAIL] {case.name} [{dialect}] {cfg}: zerocc "
                              f"stderr missing '{case.stderr_must_contain}':\n{z_err}")
                        ok = False
                        continue
                    if neg and neg in z_err:
                        print(f"  [FAIL] {case.name} [{dialect}] {cfg}: zerocc "
                              f"stderr must NOT contain '{neg}':\n{z_err}")
                        ok = False
                        continue
                    if z_rc != ref_rc or z_out != ref_out:
                        print(f"  [FAIL] {case.name} [{dialect}] {cfg}: "
                              f"rc {z_rc} (ref {ref_rc}) or stdout "
                              f"{z_out!r} (ref {ref_out!r})")
                        ok = False
                        continue
                    if verbose:
                        print(f"  [PASS] {case.name} [{dialect}] {cfg}")
    finally:
        for a in artifacts:
            if os.path.exists(a):
                os.unlink(a)
        for i in range(4):
            for suf in (".ref", f".z{i}"):
                if os.path.exists(src + suf):
                    os.unlink(src + suf)
    return ok


def main():
    parser = argparse.ArgumentParser(description="ZeroCC C23 differential gate")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--feature",
                        choices=sorted({t.feature for t in TEST_CASES}),
                        help="filter by feature tag")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if not os.path.exists(ZEROCC):
        print(f"error: zerocc not found at {ZEROCC} (run `make build/bin/zerocc`)")
        sys.exit(2)

    tests = TEST_CASES
    if args.feature:
        tests = [t for t in TEST_CASES if t.feature == args.feature]

    n_conf = sum(len(t.dialects) for t in tests)
    print(f"Running {len(tests)} C23 case(s) ({n_conf} dialect configs) | "
          f"reference: {GCC} | zerocc: {ZEROCC}")
    passed = 0
    for t in tests:
        if run_case(t, args.verbose):
            passed += 1
        elif not args.verbose:
            print(f"  [FAIL] {t.name}")

    print("=" * 60)
    print(f"C23 Gate: {passed}/{len(tests)} cases passed "
          f"({len(MODES)} modes x {len(OPT_LEVELS)} opt levels per config)")
    print("=" * 60)
    sys.exit(0 if passed == len(tests) else 1)


if __name__ == "__main__":
    main()
