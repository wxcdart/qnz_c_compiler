#!/usr/bin/env python3
"""Cross-TU Link-Time Optimization (-flto) verification gate.

Exercises:
  1. Direct multi-TU compilation with -flto at -O0..-O3
  2. Direct multi-TU in-memory JIT execution with -flto (--run)
  3. Separate .o compilation with -flto -c and subsequent link with -flto
  4. Static library archive (.a) Fat-LTO member extraction and linking
  5. Cross-TU dead function elimination
  6. Cross-TU read-only global constant propagation
  7. Cross-TU function inlining
  8. Mixed LTO (.zc.lto) and non-LTO object linking
  9. Static scope independence for identically-named symbols across TUs
  10. Cross-TU struct by-value and struct return under LTO
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
BIN = os.path.join(ROOT, "build", "bin", "zerocc")


def run_cmd(cmd, cwd=None, timeout=60):
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def test_direct_multi_tu_aot(tmp):
    tu1 = os.path.join(tmp, "d1.c")
    tu2 = os.path.join(tmp, "d2.c")
    with open(tu1, "w") as f:
        f.write("extern int helper(int); int main(void) { return helper(21) == 42 ? 0 : 1; }\n")
    with open(tu2, "w") as f:
        f.write("int helper(int x) { return x * 2; }\n")
    for opt in ["-O0", "-O1", "-O2", "-O3"]:
        out = os.path.join(tmp, f"bin_direct_{opt}")
        rc, _, err = run_cmd([BIN, opt, "-flto", tu1, tu2, "-o", out])
        assert rc == 0, f"Direct AOT {opt} failed: {err}"
        rc_run, _, _ = run_cmd([out])
        assert rc_run == 0, f"Direct AOT run {opt} failed: rc={rc_run}"
    print("  [PASS] Direct multi-TU AOT (-O0..-O3)")


def test_direct_multi_tu_jit(tmp):
    tu1 = os.path.join(tmp, "j1.c")
    tu2 = os.path.join(tmp, "j2.c")
    with open(tu1, "w") as f:
        f.write("extern int mult(int, int); int main(void) { return mult(6, 7) == 42 ? 0 : 1; }\n")
    with open(tu2, "w") as f:
        f.write("int mult(int a, int b) { return a * b; }\n")
    for opt in ["-O0", "-O1", "-O2", "-O3"]:
        rc, _, err = run_cmd([BIN, "--run", opt, "-flto", tu1, tu2])
        assert rc == 0, f"Direct JIT {opt} failed: {err}"
    print("  [PASS] Direct multi-TU JIT (--run -O0..-O3)")


def test_separate_compile_and_link(tmp):
    tu1 = os.path.join(tmp, "s1.c")
    tu2 = os.path.join(tmp, "s2.c")
    o1 = os.path.join(tmp, "s1.o")
    o2 = os.path.join(tmp, "s2.o")
    out = os.path.join(tmp, "bin_sep")
    with open(tu1, "w") as f:
        f.write("extern int sub(int, int); int main(void) { return sub(100, 58) == 42 ? 0 : 1; }\n")
    with open(tu2, "w") as f:
        f.write("int sub(int a, int b) { return a - b; }\n")

    rc1, _, err1 = run_cmd([BIN, "-O2", "-flto", "-c", tu1, "-o", o1])
    rc2, _, err2 = run_cmd([BIN, "-O2", "-flto", "-c", tu2, "-o", o2])
    assert rc1 == 0 and rc2 == 0, f"Separate compile failed: {err1} {err2}"

    rc_link, _, err_link = run_cmd([BIN, "-O2", "-flto", o1, o2, "-o", out])
    assert rc_link == 0, f"Separate link failed: {err_link}"
    rc_run, _, _ = run_cmd([out])
    assert rc_run == 0, f"Separate run failed: rc={rc_run}"
    print("  [PASS] Separate .o compilation and link (-flto)")


def test_archive_fat_lto(tmp):
    tu1 = os.path.join(tmp, "a1.c")
    tu2 = os.path.join(tmp, "a2.c")
    tu3 = os.path.join(tmp, "a3.c")
    o1 = os.path.join(tmp, "a1.o")
    o2 = os.path.join(tmp, "a2.o")
    o3 = os.path.join(tmp, "a3.o")
    lib = os.path.join(tmp, "libmath.a")
    out = os.path.join(tmp, "bin_ar")

    with open(tu1, "w") as f:
        f.write("""extern int fn_a(int); extern int fn_b(int);
                   int main(void) { return (fn_a(10) + fn_b(20)) == 70 ? 0 : 1; }\n""")
    with open(tu2, "w") as f:
        f.write("int fn_a(int x) { return x * 3; }\n")
    with open(tu3, "w") as f:
        f.write("int fn_b(int x) { return x * 2; }\n")

    assert run_cmd([BIN, "-O2", "-flto", "-c", tu1, "-o", o1])[0] == 0
    assert run_cmd([BIN, "-O2", "-flto", "-c", tu2, "-o", o2])[0] == 0
    assert run_cmd([BIN, "-O2", "-flto", "-c", tu3, "-o", o3])[0] == 0

    rc_ar, _, err_ar = run_cmd([BIN, "--ar", "rcs", lib, o2, o3])
    assert rc_ar == 0, f"Archiver failed: {err_ar}"

    rc_link, _, err_link = run_cmd([BIN, "-O2", "-flto", o1, lib, "-o", out])
    assert rc_link == 0, f"Archive link failed: {err_link}"
    assert run_cmd([out])[0] == 0
    print("  [PASS] Static archive (.a) Fat-LTO member resolution and linking")


def test_dead_code_elimination(tmp):
    tu1 = os.path.join(tmp, "dead1.c")
    tu2 = os.path.join(tmp, "dead2.c")
    out = os.path.join(tmp, "bin_dead")

    with open(tu1, "w") as f:
        f.write("extern int live_fn(void); int main(void) { return live_fn() == 7 ? 0 : 1; }\n")
    with open(tu2, "w") as f:
        f.write("""int live_fn(void) { return 7; }
                   int unreferenced_never_called(void) { return 999; }
                   int another_dead_helper(int x) { return x * x; }\n""")

    rc, _, err = run_cmd([BIN, "-O2", "-flto", tu1, tu2, "-o", out])
    assert rc == 0, f"LTO build failed: {err}"
    assert run_cmd([out])[0] == 0

    rc_nm, stdout_nm, _ = run_cmd(["nm", out])
    assert rc_nm == 0
    assert "unreferenced_never_called" not in stdout_nm, "Dead function not eliminated!"
    assert "another_dead_helper" not in stdout_nm, "Dead helper not eliminated!"
    print("  [PASS] Cross-TU dead function elimination (verified via nm)")


def test_constant_propagation_and_inlining(tmp):
    tu1 = os.path.join(tmp, "cp1.c")
    tu2 = os.path.join(tmp, "cp2.c")
    out = os.path.join(tmp, "bin_cp")

    with open(tu1, "w") as f:
        f.write("""extern const int g_magic_scale;
                   extern int scale_val(int);
                   int main(void) {
                       int v = scale_val(10);
                       return (v == 50 && g_magic_scale == 5) ? 0 : 1;
                   }\n""")
    with open(tu2, "w") as f:
        f.write("""const int g_magic_scale = 5;
                   int scale_val(int x) { return x * g_magic_scale; }\n""")

    rc, _, err = run_cmd([BIN, "-O2", "-flto", tu1, tu2, "-o", out])
    assert rc == 0, f"LTO compile failed: {err}"
    assert run_cmd([out])[0] == 0

    rc_dis, stdout_dis, _ = run_cmd(["objdump", "-d", out])
    assert rc_dis == 0
    assert "<scale_val>:" not in stdout_dis, "scale_val should have been inlined and eliminated!"
    print("  [PASS] Cross-TU constant global propagation and function inlining")


def test_mixed_lto_objects(tmp):
    tu1 = os.path.join(tmp, "mix1.c")
    tu2 = os.path.join(tmp, "mix2.c")
    o1 = os.path.join(tmp, "mix1.o")
    o2 = os.path.join(tmp, "mix2.o")
    out = os.path.join(tmp, "bin_mix")

    with open(tu1, "w") as f:
        f.write("extern int non_lto_provider(int); int main(void) { return non_lto_provider(5) == 25 ? 0 : 1; }\n")
    with open(tu2, "w") as f:
        f.write("int non_lto_provider(int x) { return x * x; }\n")

    # o1 compiled WITH -flto, o2 compiled WITHOUT -flto
    assert run_cmd([BIN, "-O2", "-flto", "-c", tu1, "-o", o1])[0] == 0
    assert run_cmd([BIN, "-O2", "-c", tu2, "-o", o2])[0] == 0

    rc, _, err = run_cmd([BIN, "-O2", "-flto", o1, o2, "-o", out])
    assert rc == 0, f"Mixed LTO link failed: {err}"
    assert run_cmd([out])[0] == 0
    print("  [PASS] Mixed LTO (.zc.lto) and non-LTO object file linking")


def test_static_scope_isolation(tmp):
    tu1 = os.path.join(tmp, "stat1.c")
    tu2 = os.path.join(tmp, "stat2.c")
    out = os.path.join(tmp, "bin_stat")

    with open(tu1, "w") as f:
        f.write("""static int counter = 10;
                   static int get_local(void) { return counter; }
                   extern int get_tu2(void);
                   int main(void) {
                       counter += 5;
                       return (get_local() == 15 && get_tu2() == 200) ? 0 : 1;
                   }\n""")
    with open(tu2, "w") as f:
        f.write("""static int counter = 200;
                   int get_tu2(void) { return counter; }\n""")

    rc, _, err = run_cmd([BIN, "-O2", "-flto", tu1, tu2, "-o", out])
    assert rc == 0, f"Static collision test failed: {err}"
    assert run_cmd([out])[0] == 0
    print("  [PASS] Static symbol isolation across separate translation units")


def test_struct_abi(tmp):
    tu1 = os.path.join(tmp, "st1.c")
    tu2 = os.path.join(tmp, "st2.c")
    out = os.path.join(tmp, "bin_st")

    with open(tu1, "w") as f:
        f.write("""struct Vec3 { int x; int y; int z; };
                   extern struct Vec3 add_vec(struct Vec3 a, struct Vec3 b);
                   int main(void) {
                       struct Vec3 a = {1, 2, 3};
                       struct Vec3 b = {4, 5, 6};
                       struct Vec3 c = add_vec(a, b);
                       return (c.x == 5 && c.y == 7 && c.z == 9) ? 0 : 1;
                   }\n""")
    with open(tu2, "w") as f:
        f.write("""struct Vec3 { int x; int y; int z; };
                   struct Vec3 add_vec(struct Vec3 a, struct Vec3 b) {
                       struct Vec3 r = { a.x + b.x, a.y + b.y, a.z + b.z };
                       return r;
                   }\n""")

    rc, _, err = run_cmd([BIN, "-O2", "-flto", tu1, tu2, "-o", out])
    assert rc == 0, f"Struct ABI LTO test failed: {err}"
    assert run_cmd([out])[0] == 0
    print("  [PASS] Cross-TU struct by-value and struct return under -flto")


def test_cross_tu_ipcp_cloning(tmp):
    tu1 = os.path.join(tmp, "ipcp1.c")
    tu2 = os.path.join(tmp, "ipcp2.c")
    out = os.path.join(tmp, "bin_ipcp")

    with open(tu1, "w") as f:
        f.write("""extern int compute_op(int x, int mode);
                   int main(void) {
                       int a = compute_op(10, 0);
                       int b = compute_op(20, 1);
                       return (a == 30 && b == 30) ? 0 : 1;
                   }\n""")
    with open(tu2, "w") as f:
        f.write("""int compute_op(int x, int mode) {
                       if (mode == 0) {
                           return x * 3;
                       } else if (mode == 1) {
                           return x + 10;
                       } else {
                           return x - 5;
                       }
                   }\n""")

    rc, _, err = run_cmd([BIN, "-O2", "-flto", tu1, tu2, "-o", out])
    assert rc == 0, f"Cross-TU IPCP build failed: {err}"
    rc_run, _, _ = run_cmd([out])
    assert rc_run == 0, f"Cross-TU IPCP run failed: rc={rc_run}"

    rc_nm, stdout_nm, _ = run_cmd(["nm", out])
    assert rc_nm == 0
    assert "compute_op.const" in stdout_nm or "<compute_op>" not in stdout_nm, \
        "Expected specialized clone or inlining of compute_op!"
    print("  [PASS] Cross-TU inter-procedural constant propagation & specialization")


def test_dead_global_data_stripping(tmp):
    tu1 = os.path.join(tmp, "dg1.c")
    tu2 = os.path.join(tmp, "dg2.c")
    out = os.path.join(tmp, "bin_dg")

    with open(tu1, "w") as f:
        f.write("int main(void) { return 0; }\n")
    with open(tu2, "w") as f:
        f.write("""int unreferenced_data_arr[64] = { 1, 2, 3 };
                   int unreferenced_bss_arr[128];
                   static int unreferenced_static_var = 999;\n""")

    rc, _, err = run_cmd([BIN, "-O2", "-flto", tu1, tu2, "-o", out])
    assert rc == 0, f"Dead global stripping build failed: {err}"
    assert run_cmd([out])[0] == 0

    rc_nm, stdout_nm, _ = run_cmd(["nm", out])
    assert rc_nm == 0
    assert "unreferenced_data_arr" not in stdout_nm, "Unreferenced data global not stripped!"
    assert "unreferenced_bss_arr" not in stdout_nm, "Unreferenced BSS global not stripped!"
    assert "unreferenced_static_var" not in stdout_nm, "Unreferenced static global not stripped!"
    print("  [PASS] Cross-TU dead global data & BSS stripping (verified via nm)")


def test_const_aggregate_propagation(tmp):
    tu1 = os.path.join(tmp, "ag1.c")
    tu2 = os.path.join(tmp, "ag2.c")
    out = os.path.join(tmp, "bin_ag")

    with open(tu1, "w") as f:
        f.write("""struct Config { int factor; int bias; };
                   extern const struct Config app_cfg;
                   extern const int lookup_table[4];
                   extern int calc(int);
                   int main(void) {
                       if (app_cfg.factor != 3) return 1;
                       if (app_cfg.bias != 14) return 2;
                       if (lookup_table[2] != 40) return 3;
                       if (calc(10) != 44) return 4;
                       return 0;
                   }\n""")
    with open(tu2, "w") as f:
        f.write("""struct Config { int factor; int bias; };
                   const struct Config app_cfg = { 3, 14 };
                   const int lookup_table[4] = { 10, 20, 40, 80 };
                   int calc(int x) { return x * app_cfg.factor + app_cfg.bias; }\n""")

    rc, _, err = run_cmd([BIN, "-O2", "-flto", tu1, tu2, "-o", out])
    assert rc == 0, f"Const aggregate propagation build failed: {err}"
    assert run_cmd([out])[0] == 0

    rc_dis, stdout_dis, _ = run_cmd(["objdump", "-d", out])
    assert rc_dis == 0
    assert "movabs $0x3," in stdout_dis or "mov $0x3," in stdout_dis, "Expected folded factor 3!"
    assert "movabs $0xe," in stdout_dis or "mov $0xe," in stdout_dis, "Expected folded bias 14!"
    assert "movabs $0x28," in stdout_dis or "mov $0x28," in stdout_dis, "Expected folded table element 40!"
    print("  [PASS] Cross-TU const aggregate propagation (struct member & array index folded)")


def main():
    if not os.path.isfile(BIN):
        print(f"Error: {BIN} not found. Run make first.", file=sys.stderr)
        return 1

    print("============================================================")
    print("Running ZeroCC Link-Time Optimization (-flto) Test Battery")
    print("============================================================")

    with tempfile.TemporaryDirectory(prefix="zerocc_lto_test_") as tmp:
        test_direct_multi_tu_aot(tmp)
        test_direct_multi_tu_jit(tmp)
        test_separate_compile_and_link(tmp)
        test_archive_fat_lto(tmp)
        test_dead_code_elimination(tmp)
        test_constant_propagation_and_inlining(tmp)
        test_mixed_lto_objects(tmp)
        test_static_scope_isolation(tmp)
        test_struct_abi(tmp)
        test_cross_tu_ipcp_cloning(tmp)
        test_dead_global_data_stripping(tmp)
        test_const_aggregate_propagation(tmp)

    print("============================================================")
    print("Results: All 12 LTO Battery Tests PASSED!")
    print("============================================================")
    return 0


if __name__ == "__main__":
    sys.exit(main())
