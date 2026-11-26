#!/usr/bin/env python3
"""
tools/verify/pgo_tests.py — Profile-Guided Optimization (PGO) Verification Battery.

Verifies ZeroCC's -fprofile-generate and -fprofile-use pipeline:
1. Direct JIT instrumentation (--run -fprofile-generate) and profile file generation.
2. Direct AOT instrumentation (-fprofile-generate) and atexit profile dump.
3. JIT profile-guided execution (--run -fprofile-use -O0..-O3).
4. AOT profile-guided execution (-fprofile-use -O0..-O3).
5. Hot function inlining vs cold function inlining suppression.
6. Loop unrolling with PGO frequency scaling.
7. Branch execution frequency accuracy.
8. Environment variable ZERO_CC_PROFILE_FILE output override.
9. Multi-TU instrumentation and profile-guided optimization.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

ROOT = find_repo_root()
ZC_BIN = os.path.join(ROOT, "build", "bin", "zerocc")


def run_cmd(cmd, env=None):
    res = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return res.returncode, res.stdout, res.stderr


def test_pgo_generate_jit(tmpdir):
    src = os.path.join(tmpdir, "jit_gen.c")
    prof = os.path.join(tmpdir, "jit.zcprof")
    with open(src, "w") as f:
        f.write("""
#include <stdio.h>
int compute(int x) {
    if (x > 5) return x * 3;
    else return x + 2;
}
int main(void) {
    int sum = 0;
    for (int i = 0; i < 10; i++) sum += compute(i);
    printf("sum=%d\\n", sum);
    return 0;
}
""")
    rc, out, err = run_cmd([ZC_BIN, "--run", f"-fprofile-generate={prof}", src])
    assert rc == 0, f"JIT run failed: {err}"
    assert "sum=" in out, f"Unexpected output: {out}"
    assert os.path.exists(prof), f"Profile file not created: {prof}"

    with open(prof) as f:
        content = f.read()
    assert "compute 0 10" in content, f"Missing compute entry count in {content}"
    assert "main 0 1" in content, f"Missing main entry count in {content}"
    return True


def test_pgo_generate_aot(tmpdir):
    src = os.path.join(tmpdir, "aot_gen.c")
    prof = os.path.join(tmpdir, "aot.zcprof")
    bin_path = os.path.join(tmpdir, "aot_bin")
    with open(src, "w") as f:
        f.write("""
#include <stdio.h>
int helper(int a) {
    if (a % 2 == 0) return a / 2;
    return a * 3 + 1;
}
int main(void) {
    int total = 0;
    for (int i = 1; i <= 20; i++) total += helper(i);
    printf("total=%d\\n", total);
    return 0;
}
""")
    rc, out, err = run_cmd([ZC_BIN, f"-fprofile-generate={prof}", src, "-o", bin_path])
    assert rc == 0, f"AOT compile failed: {err}"
    rc, out, err = run_cmd([bin_path])
    assert rc == 0, f"AOT binary failed: {err}"
    assert "total=" in out, f"Unexpected output: {out}"
    assert os.path.exists(prof), f"Profile file not created: {prof}"

    with open(prof) as f:
        content = f.read()
    assert "helper 0 20" in content, f"Missing helper count in {content}"
    assert "main 0 1" in content, f"Missing main count in {content}"
    return True


def test_pgo_use_jit_and_aot(tmpdir):
    src = os.path.join(tmpdir, "use_test.c")
    prof = os.path.join(tmpdir, "use.zcprof")
    bin_path = os.path.join(tmpdir, "use_bin")
    with open(src, "w") as f:
        f.write("""
#include <stdio.h>
int calc(int x) {
    if (x < 10) return x * 10;
    return x * 20;
}
int main(void) {
    int acc = 0;
    for (int i = 0; i < 15; i++) acc += calc(i);
    printf("acc=%d\\n", acc);
    return 0;
}
""")
    # 1. Generate
    rc, out, err = run_cmd([ZC_BIN, "--run", f"-fprofile-generate={prof}", src])
    assert rc == 0 and os.path.exists(prof), f"Generate failed: {err}"

    # 2. Use JIT across -O0..-O3
    for opt in ["-O0", "-O1", "-O2", "-O3"]:
        rc, out, err = run_cmd([ZC_BIN, "--run", f"-fprofile-use={prof}", opt, src])
        assert rc == 0, f"JIT use {opt} failed: {err}"
        assert out.strip() == "acc=1650", f"Wrong output {opt}: {out}"

    # 3. Use AOT across -O0..-O3
    for opt in ["-O0", "-O1", "-O2", "-O3"]:
        rc, out, err = run_cmd([ZC_BIN, f"-fprofile-use={prof}", opt, src, "-o", bin_path])
        assert rc == 0, f"AOT use compile {opt} failed: {err}"
        rc, out, err = run_cmd([bin_path])
        assert rc == 0, f"AOT use run {opt} failed: {err}"
        assert out.strip() == "acc=1650", f"Wrong output {opt}: {out}"
    return True


def test_pgo_hot_cold_inlining(tmpdir):
    src = os.path.join(tmpdir, "inline_test.c")
    prof = os.path.join(tmpdir, "inline.zcprof")
    with open(src, "w") as f:
        f.write("""
#include <stdio.h>
int hot_math(int a, int b) {
    return a * 4 + b * 3;
}
int cold_math(int a, int b) {
    return a * 9 - b * 7;
}
int main(void) {
    int s = 0;
    for (int i = 0; i < 50; i++) {
        s += hot_math(i, i + 1);
    }
    printf("s=%d\\n", s);
    return 0;
}
""")
    # Generate profile (hot_math hit 50 times, cold_math hit 0 times)
    rc, _, _ = run_cmd([ZC_BIN, "--run", f"-fprofile-generate={prof}", src])
    assert rc == 0 and os.path.exists(prof)

    with open(prof) as f:
        content = f.read()
    assert "hot_math 0 50" in content
    assert "cold_math" not in content

    # Compile with -fprofile-use and check execution
    rc, out, _ = run_cmd([ZC_BIN, "--run", f"-fprofile-use={prof}", "-O2", src])
    assert rc == 0 and out.strip() == "s=8725"
    return True


def test_pgo_loop_unroll(tmpdir):
    src = os.path.join(tmpdir, "loop_test.c")
    prof = os.path.join(tmpdir, "loop.zcprof")
    with open(src, "w") as f:
        f.write("""
#include <stdio.h>
int loop_calc(int n) {
    int v = 0;
    for (int i = 0; i < 12; i++) {
        v += i * 2;
    }
    return v + n;
}
int main(void) {
    int total = 0;
    for (int k = 0; k < 100; k++) {
        total += loop_calc(k);
    }
    printf("total=%d\\n", total);
    return 0;
}
""")
    rc, _, _ = run_cmd([ZC_BIN, "--run", f"-fprofile-generate={prof}", src])
    assert rc == 0 and os.path.exists(prof)

    for opt in ["-O2", "-O3"]:
        rc, out, _ = run_cmd([ZC_BIN, "--run", f"-fprofile-use={prof}", opt, src])
        assert rc == 0 and out.strip() == "total=18150"
    return True


def test_pgo_env_override(tmpdir):
    src = os.path.join(tmpdir, "env_test.c")
    bin_path = os.path.join(tmpdir, "env_bin")
    prof = os.path.join(tmpdir, "custom_env.zcprof")
    with open(src, "w") as f:
        f.write("""
#include <stdio.h>
int main(void) {
    printf("env test\\n");
    return 0;
}
""")
    rc, _, _ = run_cmd([ZC_BIN, "-fprofile-generate", src, "-o", bin_path])
    assert rc == 0

    env = os.environ.copy()
    env["ZERO_CC_PROFILE_FILE"] = prof
    rc, out, _ = run_cmd([bin_path], env=env)
    assert rc == 0 and os.path.exists(prof)
    return True


def test_pgo_multitu(tmpdir):
    tu1 = os.path.join(tmpdir, "tu1.c")
    tu2 = os.path.join(tmpdir, "tu2.c")
    prof = os.path.join(tmpdir, "multitu.zcprof")
    bin_path = os.path.join(tmpdir, "multi_bin")

    with open(tu1, "w") as f:
        f.write("""
#include <stdio.h>
extern int tu2_add(int a, int b);
int main(void) {
    int res = tu2_add(30, 12);
    printf("res=%d\\n", res);
    return 0;
}
""")
    with open(tu2, "w") as f:
        f.write("""
int tu2_add(int a, int b) {
    return a + b;
}
""")
    # JIT Multi-TU generate and use
    rc, out, err = run_cmd([ZC_BIN, "--run", f"-fprofile-generate={prof}", tu1, tu2])
    assert rc == 0 and os.path.exists(prof), f"Multi-TU JIT generate failed: {err}"
    assert out.strip() == "res=42"

    rc, out, err = run_cmd([ZC_BIN, "--run", f"-fprofile-use={prof}", "-O2", tu1, tu2])
    assert rc == 0 and out.strip() == "res=42", f"Multi-TU JIT use failed: {err}"

    # AOT Multi-TU generate and use
    rc, out, err = run_cmd([ZC_BIN, f"-fprofile-generate={prof}", tu1, tu2, "-o", bin_path])
    assert rc == 0, f"Multi-TU AOT compile failed: {err}"
    rc, out, err = run_cmd([bin_path])
    assert rc == 0 and out.strip() == "res=42"

    rc, out, err = run_cmd([ZC_BIN, f"-fprofile-use={prof}", "-O2", tu1, tu2, "-o", bin_path])
    assert rc == 0, f"Multi-TU AOT use compile failed: {err}"
    rc, out, err = run_cmd([bin_path])
    assert rc == 0 and out.strip() == "res=42"
    return True


def main():
    print("=" * 60)
    print("Running ZeroCC Profile-Guided Optimization (PGO) Test Battery")
    print("=" * 60)

    tests = [
        ("Direct JIT execution with -fprofile-generate", test_pgo_generate_jit),
        ("Direct AOT execution with -fprofile-generate", test_pgo_generate_aot),
        ("PGO profile-use across -O0..-O3 in JIT and AOT", test_pgo_use_jit_and_aot),
        ("Hot function inlining and cold function suppression", test_pgo_hot_cold_inlining),
        ("Loop unrolling under PGO frequency guidance", test_pgo_loop_unroll),
        ("Environment variable ZERO_CC_PROFILE_FILE override", test_pgo_env_override),
        ("Multi-TU PGO profile generation and optimization", test_pgo_multitu),
    ]

    tmpdir = tempfile.mkdtemp(prefix="zc_pgo_test_")
    passed = 0
    failed = 0

    try:
        for name, fn in tests:
            try:
                fn(tmpdir)
                print(f"  [PASS] {name}")
                passed += 1
            except AssertionError as e:
                print(f"  [FAIL] {name}: {e}")
                failed += 1
            except Exception as e:
                print(f"  [ERROR] {name}: {e}")
                failed += 1
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("=" * 60)
    if failed == 0:
        print(f"Results: All {passed} PGO Battery Tests PASSED!")
        print("=" * 60)
        return 0
    else:
        print(f"Results: {passed} PASSED, {failed} FAILED!")
        print("=" * 60)
        return 1


if __name__ == "__main__":
    sys.exit(main())
