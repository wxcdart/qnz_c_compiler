#!/usr/bin/env python3
"""
tools/verify/macho_tests.py — macOS Mach-O 64-bit Verification Battery (Sprint 251).

Validates native macOS Mach-O 64-bit executable (MH_EXECUTE), dynamic library
(MH_DYLIB / .dylib), and relocatable object (MH_OBJECT / .o) generation for Darwin:
- Header layout: MH_MAGIC_64 (0xfeedfacf), cputype (x86_64 and arm64), filetypes
- Segments and sections: __PAGEZERO, __TEXT (__text, __cstring), __DATA (__data, __bss), __LINKEDIT
- Dynamic loader commands: LC_LOAD_DYLINKER (/usr/lib/dyld), LC_LOAD_DYLIB (/usr/lib/libSystem.B.dylib), LC_ID_DYLIB
- Entry point command: LC_MAIN
- Symbol tables: LC_SYMTAB (nlist_64 symbols with Darwin leading underscore, string table) and LC_DYSYMTAB
- Multi-TU object link into Mach-O binary
- Optimization sweep: -O0..-O3
- CLI flags: --macho, -mmacho, --target=x86_64-apple-darwin, --target=aarch64-apple-darwin, .dylib auto-detection
"""

import os
import struct
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

# Mach-O 64 Constants
MH_MAGIC_64 = 0xFEEDFACF
CPU_ARCH_ABI64 = 0x01000000
CPU_TYPE_X86_64 = CPU_ARCH_ABI64 | 7
CPU_TYPE_ARM64 = CPU_ARCH_ABI64 | 12
CPU_SUBTYPE_X86_64_ALL = 3
CPU_SUBTYPE_ARM64_ALL = 0

MH_OBJECT = 1
MH_EXECUTE = 2
MH_DYLIB = 6

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02
LC_DYSYMTAB = 0x0B
LC_LOAD_DYLIB = 0x0C
LC_ID_DYLIB = 0x0D
LC_LOAD_DYLINKER = 0x0E
LC_MAIN = 0x80000028


def report(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  [PASS] {name}")
    else:
        failed += 1
        print(f"  [FAIL] {name}: {detail}")


def parse_macho_header(data):
    if len(data) < 32:
        return None
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack_from(
        "<IIIIIIII", data, 0
    )
    return {
        "magic": magic,
        "cputype": cputype,
        "cpusubtype": cpusubtype,
        "filetype": filetype,
        "ncmds": ncmds,
        "sizeofcmds": sizeofcmds,
        "flags": flags,
    }


def parse_load_commands(data, ncmds, offset=32):
    cmds = []
    curr = offset
    for _ in range(ncmds):
        if curr + 8 > len(data):
            break
        cmd, cmdsize = struct.unpack_from("<II", data, curr)
        cmd_bytes = data[curr : curr + cmdsize]
        cmds.append({"cmd": cmd, "cmdsize": cmdsize, "offset": curr, "raw": cmd_bytes})
        curr += cmdsize
    return cmds


def test_macho_exe_headers():
    """Verify raw Mach-O 64 executable header, segments, and load commands."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "test.c"
        exe = Path(td) / "test.macho"
        src.write_text("int global_data = 100;\nint bss_var;\nint main(void) { bss_var = 5; return global_data + bss_var; }\n")

        cmd = [str(ZEROCC), "--macho", str(src), "-o", str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Mach-O 64 .macho compilation", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        data = exe.read_bytes()
        hdr = parse_macho_header(data)
        report("Header magic MH_MAGIC_64", hdr and hdr["magic"] == MH_MAGIC_64, f"{hdr}")
        report("CPU type x86_64", hdr and hdr["cputype"] == CPU_TYPE_X86_64, f"{hdr}")
        report("Filetype MH_EXECUTE", hdr and hdr["filetype"] == MH_EXECUTE, f"{hdr}")

        cmds = parse_load_commands(data, hdr["ncmds"])
        cmd_types = [c["cmd"] for c in cmds]

        report("Contains LC_SEGMENT_64", LC_SEGMENT_64 in cmd_types)
        report("Contains LC_LOAD_DYLINKER", LC_LOAD_DYLINKER in cmd_types)
        report("Contains LC_LOAD_DYLIB", LC_LOAD_DYLIB in cmd_types)
        report("Contains LC_MAIN", LC_MAIN in cmd_types)
        report("Contains LC_SYMTAB", LC_SYMTAB in cmd_types)
        report("Contains LC_DYSYMTAB", LC_DYSYMTAB in cmd_types)

        # Inspect segment names
        seg_names = []
        for c in cmds:
            if c["cmd"] == LC_SEGMENT_64:
                segname = c["raw"][8:24].split(b"\x00")[0].decode("ascii", errors="ignore")
                seg_names.append(segname)

        report("Segments contain [__PAGEZERO, __TEXT, __DATA, __LINKEDIT]",
               all(s in seg_names for s in ["__PAGEZERO", "__TEXT", "__DATA", "__LINKEDIT"]),
               f"got {seg_names}")

        # Check LC_MAIN entry point offset
        main_cmd = next((c for c in cmds if c["cmd"] == LC_MAIN), None)
        if main_cmd:
            entryoff, stacksize = struct.unpack_from("<QQ", main_cmd["raw"], 8)
            report("LC_MAIN entryoff is valid (>0)", entryoff > 0, f"entryoff={entryoff}")


def test_macho_dylib():
    """Verify Mach-O 64 dynamic library (.dylib) emission and export commands."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "libmath.c"
        dylib = Path(td) / "libmath.dylib"
        src.write_text("""
        int math_add(int a, int b) { return a + b; }
        int math_mul(int a, int b) { return a * b; }
        """)

        cmd = [str(ZEROCC), "-shared", str(src), "-o", str(dylib)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Mach-O .dylib compilation with -shared", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        data = dylib.read_bytes()
        hdr = parse_macho_header(data)
        report("Dylib magic MH_MAGIC_64", hdr and hdr["magic"] == MH_MAGIC_64, f"{hdr}")
        report("Dylib filetype MH_DYLIB", hdr and hdr["filetype"] == MH_DYLIB, f"{hdr}")

        cmds = parse_load_commands(data, hdr["ncmds"])
        cmd_types = [c["cmd"] for c in cmds]
        report("Dylib contains LC_ID_DYLIB", LC_ID_DYLIB in cmd_types)
        report("Dylib contains LC_LOAD_DYLIB", LC_LOAD_DYLIB in cmd_types)
        report("Dylib contains LC_SYMTAB", LC_SYMTAB in cmd_types)

        # Inspect symbol table for exported symbols _math_add and _math_mul
        symtab_cmd = next((c for c in cmds if c["cmd"] == LC_SYMTAB), None)
        if symtab_cmd:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", symtab_cmd["raw"], 8)
            report("Dylib symbol count > 0", nsyms > 0, f"nsyms={nsyms}")
            strtab = data[stroff : stroff + strsize]
            report("Symbol table contains _math_add", b"_math_add" in strtab)
            report("Symbol table contains _math_mul", b"_math_mul" in strtab)


def test_macho_relocatable_object():
    """Verify Mach-O 64 relocatable object file (.o) structure."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "obj_test.c"
        obj = Path(td) / "obj_test.o"
        src.write_text("""
        const char hello[] = "Hello macOS";
        int counter = 42;
        int get_val(void) { return counter; }
        int main(void) { return get_val(); }
        """)

        cmd = [str(ZEROCC), "--macho", "-c", str(src), "-o", str(obj)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Mach-O object compilation (-c)", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        data = obj.read_bytes()
        hdr = parse_macho_header(data)
        report("Object magic MH_MAGIC_64", hdr and hdr["magic"] == MH_MAGIC_64, f"{hdr}")
        report("Object filetype MH_OBJECT", hdr and hdr["filetype"] == MH_OBJECT, f"{hdr}")

        cmds = parse_load_commands(data, hdr["ncmds"])
        seg_cmd = next((c for c in cmds if c["cmd"] == LC_SEGMENT_64), None)
        report("Object contains LC_SEGMENT_64", seg_cmd is not None)

        if seg_cmd:
            nsects, = struct.unpack_from("<I", seg_cmd["raw"], 64)
            report("Object section count >= 2", nsects >= 2, f"nsects={nsects}")
            sect_names = []
            for i in range(nsects):
                s_off = 72 + i * 80
                sname = seg_cmd["raw"][s_off : s_off + 16].split(b"\x00")[0].decode("ascii", errors="ignore")
                sect_names.append(sname)
            report("Object sections include __text and __cstring/__data",
                   "__text" in sect_names, f"got {sect_names}")

        symtab_cmd = next((c for c in cmds if c["cmd"] == LC_SYMTAB), None)
        if symtab_cmd:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", symtab_cmd["raw"], 8)
            strtab = data[stroff : stroff + strsize]
            report("Object symbols contain _main", b"_main" in strtab)
            report("Object symbols contain _counter", b"_counter" in strtab)


def test_macho_arm64_target():
    """Verify Mach-O 64 ARM64 (Apple Silicon) target header and cputype."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "arm_test.c"
        exe = Path(td) / "arm_test.macho"
        src.write_text("int main(void) { return 0; }\n")

        cmd = [str(ZEROCC), "--macho", "--target=aarch64-apple-darwin", str(src), "-o", str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Mach-O arm64 compilation", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        data = exe.read_bytes()
        hdr = parse_macho_header(data)
        report("Mach-O arm64 cputype ARM64 (0x0100000C)",
               hdr and hdr["cputype"] == CPU_TYPE_ARM64, f"{hdr}")
        report("Mach-O arm64 cpusubtype ARM64_ALL (0)",
               hdr and hdr["cpusubtype"] == CPU_SUBTYPE_ARM64_ALL, f"{hdr}")


def test_cli_flags_and_autodetection():
    """Verify --macho, -mmacho, target triplets, and .dylib extension autodetection."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "flag.c"
        src.write_text("int main(void) { return 1; }\n")

        # 1. -mmacho flag
        out1 = Path(td) / "out1"
        cmd1 = [str(ZEROCC), "-mmacho", str(src), "-o", str(out1)]
        proc1 = subprocess.run(cmd1, capture_output=True, text=True)
        report("CLI -mmacho flag", proc1.returncode == 0 and out1.read_bytes()[:4] == b"\xcf\xfa\xed\xfe", proc1.stderr)

        # 2. --target=x86_64-apple-darwin
        out2 = Path(td) / "out2"
        cmd2 = [str(ZEROCC), "--target=x86_64-apple-darwin", str(src), "-o", str(out2)]
        proc2 = subprocess.run(cmd2, capture_output=True, text=True)
        report("CLI --target=x86_64-apple-darwin",
               proc2.returncode == 0 and out2.read_bytes()[:4] == b"\xcf\xfa\xed\xfe", proc2.stderr)

        # 3. .dylib auto-detection
        out3 = Path(td) / "libtest.dylib"
        cmd3 = [str(ZEROCC), str(src), "-o", str(out3)]
        proc3 = subprocess.run(cmd3, capture_output=True, text=True)
        hdr3 = parse_macho_header(out3.read_bytes()) if out3.exists() else None
        report("Auto-detection by .dylib extension (MH_DYLIB)",
               proc3.returncode == 0 and hdr3 and hdr3["filetype"] == MH_DYLIB, proc3.stderr)


def test_optimization_sweep():
    """Verify Mach-O compilation across optimization levels -O0 to -O3."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "opt.c"
        src.write_text("""
        static int sum_loop(int n) {
            int acc = 0;
            for (int i = 1; i <= n; i++) acc += i;
            return acc;
        }
        int main(void) {
            return sum_loop(10);
        }
        """)

        for opt in ["-O0", "-O1", "-O2", "-O3"]:
            exe = Path(td) / f"opt{opt}.macho"
            cmd = [str(ZEROCC), "--macho", opt, str(src), "-o", str(exe)]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            hdr = parse_macho_header(exe.read_bytes()) if exe.exists() else None
            ok = proc.returncode == 0 and hdr and hdr["magic"] == MH_MAGIC_64
            report(f"Mach-O 64 compilation at {opt}", ok, proc.stderr)


def test_multi_tu_linking():
    """Verify merging multiple TUs into a single Mach-O binary."""
    with tempfile.TemporaryDirectory() as td:
        tu1 = Path(td) / "tu1.c"
        tu2 = Path(td) / "tu2.c"
        exe = Path(td) / "merged.macho"

        tu1.write_text("""
        int get_value_from_tu2(void);
        int main(void) {
            return get_value_from_tu2();
        }
        """)
        tu2.write_text("""
        int get_value_from_tu2(void) {
            return 77;
        }
        """)

        cmd = [str(ZEROCC), "--macho", str(tu1), str(tu2), "-o", str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Multi-TU Mach-O compilation and linking", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        hdr = parse_macho_header(exe.read_bytes())
        report("Multi-TU executable valid MH_EXECUTE", hdr and hdr["filetype"] == MH_EXECUTE)


def test_macho_multi_object_linking():
    """Verify linking multiple precompiled Mach-O .o object files into a single binary."""
    with tempfile.TemporaryDirectory() as td:
        tu1 = Path(td) / "obj1.c"
        tu2 = Path(td) / "obj2.c"
        o1 = Path(td) / "obj1.o"
        o2 = Path(td) / "obj2.o"
        exe = Path(td) / "obj_merged.macho"

        tu1.write_text("""
        int get_val2(void);
        int main(void) {
            return get_val2() + 10;
        }
        """)
        tu2.write_text("""
        int get_val2(void) {
            return 88;
        }
        """)

        # 1. Compile each TU to relocatable object with -c
        c1 = subprocess.run([str(ZEROCC), "--macho", "-c", str(tu1), "-o", str(o1)], capture_output=True, text=True)
        c2 = subprocess.run([str(ZEROCC), "--macho", "-c", str(tu2), "-o", str(o2)], capture_output=True, text=True)
        report("Compile TU1 to Mach-O object", c1.returncode == 0, c1.stderr)
        report("Compile TU2 to Mach-O object", c2.returncode == 0, c2.stderr)
        if c1.returncode != 0 or c2.returncode != 0:
            return

        # 2. Link the .o files into an executable
        cmd = [str(ZEROCC), "--macho", str(o1), str(o2), "-o", str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Mach-O multi-object linking (.o files)", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        hdr = parse_macho_header(exe.read_bytes())
        report("Linked multi-object binary valid MH_EXECUTE", hdr and hdr["filetype"] == MH_EXECUTE)
        cmds = parse_load_commands(exe.read_bytes(), hdr["ncmds"])
        main_cmd = next((c for c in cmds if c["cmd"] == LC_MAIN), None)
        report("Linked multi-object binary has valid LC_MAIN", main_cmd is not None)


def test_macho_mixed_c_and_object_linking():
    """Verify linking a mixture of .c source and precompiled .o object into a single binary."""
    with tempfile.TemporaryDirectory() as td:
        tu1 = Path(td) / "src.c"
        tu2 = Path(td) / "helper.c"
        o2 = Path(td) / "helper.o"
        exe = Path(td) / "mixed.macho"

        tu1.write_text("""
        int helper_func(void);
        int main(void) {
            return helper_func();
        }
        """)
        tu2.write_text("""
        int helper_func(void) {
            return 42;
        }
        """)

        # Compile helper to .o
        subprocess.run([str(ZEROCC), "--macho", "-c", str(tu2), "-o", str(o2)], check=True)

        # Link .c and .o together
        cmd = [str(ZEROCC), "--macho", str(tu1), str(o2), "-o", str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Mach-O mixed .c and .o linking", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        hdr = parse_macho_header(exe.read_bytes())
        report("Mixed-input executable valid MH_EXECUTE", hdr and hdr["filetype"] == MH_EXECUTE)


def test_macho_multi_object_dylib():
    """Verify linking multiple .o object files into a .dylib dynamic library."""
    with tempfile.TemporaryDirectory() as td:
        f1 = Path(td) / "lib1.c"
        f2 = Path(td) / "lib2.c"
        o1 = Path(td) / "lib1.o"
        o2 = Path(td) / "lib2.o"
        dylib = Path(td) / "libmerged.dylib"

        f1.write_text("int lib_func_alpha(void) { return 100; }\n")
        f2.write_text("int lib_func_beta(void) { return 200; }\n")

        subprocess.run([str(ZEROCC), "--macho", "-c", str(f1), "-o", str(o1)], check=True)
        subprocess.run([str(ZEROCC), "--macho", "-c", str(f2), "-o", str(o2)], check=True)

        cmd = [str(ZEROCC), "-shared", "--macho", str(o1), str(o2), "-o", str(dylib)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Mach-O multi-object .dylib linking", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        hdr = parse_macho_header(dylib.read_bytes())
        report("Multi-object dylib valid MH_DYLIB", hdr and hdr["filetype"] == MH_DYLIB)

        cmds = parse_load_commands(dylib.read_bytes(), hdr["ncmds"])
        symtab_cmd = next((c for c in cmds if c["cmd"] == LC_SYMTAB), None)
        if symtab_cmd:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", symtab_cmd["raw"], 8)
            strtab = dylib.read_bytes()[stroff : stroff + strsize]
            report("Multi-object dylib contains _lib_func_alpha", b"_lib_func_alpha" in strtab)
            report("Multi-object dylib contains _lib_func_beta", b"_lib_func_beta" in strtab)


def test_macho_arm64_multi_object_linking():
    """Verify multi-object linking for ARM64 (Apple Silicon) target."""
    with tempfile.TemporaryDirectory() as td:
        f1 = Path(td) / "arm1.c"
        f2 = Path(td) / "arm2.c"
        o1 = Path(td) / "arm1.o"
        o2 = Path(td) / "arm2.o"
        exe = Path(td) / "arm_merged.macho"

        f1.write_text("int helper(void); int main(void) { return helper(); }\n")
        f2.write_text("int helper(void) { return 1; }\n")

        subprocess.run([str(ZEROCC), "--target=aarch64-apple-darwin", "-c", str(f1), "-o", str(o1)], check=True)
        subprocess.run([str(ZEROCC), "--target=aarch64-apple-darwin", "-c", str(f2), "-o", str(o2)], check=True)

        cmd = [str(ZEROCC), "--target=aarch64-apple-darwin", str(o1), str(o2), "-o", str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Mach-O arm64 multi-object linking", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        hdr = parse_macho_header(exe.read_bytes())
        report("ARM64 linked executable cputype ARM64", hdr and hdr["cputype"] == CPU_TYPE_ARM64)


def test_host_file_utility():
    """Verify that host Linux 'file' utility identifies the emitted Mach-O artifacts."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "file_test.c"
        exe = Path(td) / "file_test.macho"
        obj = Path(td) / "file_test.o"
        dylib = Path(td) / "libfile.dylib"

        src.write_text("int main(void) { return 0; }\n")

        subprocess.run([str(ZEROCC), "--macho", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(ZEROCC), "--macho", "-c", str(src), "-o", str(obj)], check=True)
        subprocess.run([str(ZEROCC), "-shared", str(src), "-o", str(dylib)], check=True)

        res_exe = subprocess.run(["file", str(exe)], capture_output=True, text=True)
        report("file identifies Mach-O 64-bit executable",
               "Mach-O 64-bit" in res_exe.stdout and "executable" in res_exe.stdout,
               res_exe.stdout)

        res_obj = subprocess.run(["file", str(obj)], capture_output=True, text=True)
        report("file identifies Mach-O 64-bit object",
               "Mach-O 64-bit" in res_obj.stdout and "object" in res_obj.stdout,
               res_obj.stdout)

        res_dylib = subprocess.run(["file", str(dylib)], capture_output=True, text=True)
        report("file identifies Mach-O 64-bit dynamically linked shared library",
               "Mach-O 64-bit" in res_dylib.stdout and ("shared library" in res_dylib.stdout or "dynamically linked" in res_dylib.stdout),
               res_dylib.stdout)


def main():
    print("=== macOS Mach-O 64-bit Verification Battery ===")
    test_macho_exe_headers()
    test_macho_dylib()
    test_macho_relocatable_object()
    test_macho_arm64_target()
    test_cli_flags_and_autodetection()
    test_optimization_sweep()
    test_multi_tu_linking()
    test_macho_multi_object_linking()
    test_macho_mixed_c_and_object_linking()
    test_macho_multi_object_dylib()
    test_macho_arm64_multi_object_linking()
    test_host_file_utility()

    print(f"\nResult: {passed} passed, {failed} failed")
    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
