#!/usr/bin/env python3
"""
tools/verify/pe_tests.py — Windows PE/COFF Verification Battery (Sprint 246).

Validates native Windows PE32+ (x86-64) executable (.exe) and relocatable
COFF object (.obj) generation:
- MS-DOS header ("MZ"), stub ("This program cannot be run in DOS mode"), PE signature ("PE\\0\\0")
- COFF File Header (IMAGE_FILE_MACHINE_AMD64 = 0x8664)
- PE32+ Optional Header 64 (magic = 0x020B, subsystem = CUI, image_base = 0x140000000)
- Section tables (.text, .rdata, .data, .idata)
- KERNEL32.dll import table (ExitProcess)
- Relocatable COFF object (.obj) sections, symbols, and relocations
- Host binutils objdump inspection (pei-x86-64, pe-x86-64)
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


def report(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  [PASS] {name}")
    else:
        failed += 1
        print(f"  [FAIL] {name}: {detail}")


def test_pe_exe_headers():
    """Verify raw PE32+ header layout and fields."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "test.c"
        exe = Path(td) / "test.exe"
        src.write_text("int main(void) { return 42; }\n")

        cmd = [str(ZEROCC), str(src), "-o", str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            report("PE32+ .exe compilation", False, f"zerocc failed: {proc.stderr}")
            return

        report("PE32+ .exe compilation", True)

        data = exe.read_bytes()
        # 1. DOS Header
        if len(data) < 64:
            report("DOS header size", False, "file too small")
            return
        e_magic, = struct.unpack_from("<H", data, 0)
        e_lfanew, = struct.unpack_from("<I", data, 60)
        report("DOS magic 'MZ' (0x5A4D)", e_magic == 0x5A4D, f"got 0x{e_magic:04X}")
        report("DOS e_lfanew offset", e_lfanew >= 64, f"got {e_lfanew}")

        # 2. PE Signature
        pe_sig, = struct.unpack_from("<I", data, e_lfanew)
        report("PE signature 'PE\\0\\0' (0x00004550)", pe_sig == 0x00004550, f"got 0x{pe_sig:08X}")

        # 3. COFF File Header
        coff_offset = e_lfanew + 4
        machine, num_sec, time_stamp, ptr_sym, num_sym, opt_size, charact = struct.unpack_from(
            "<HHIIIHH", data, coff_offset
        )
        report("COFF Machine AMD64 (0x8664)", machine == 0x8664, f"got 0x{machine:04X}")
        report("COFF section count == 4", num_sec == 4, f"got {num_sec}")
        report("COFF optional header size == 240", opt_size == 240, f"got {opt_size}")

        # 4. Optional Header 64
        opt_offset = coff_offset + 20
        magic, major_link, minor_link, code_sz, init_sz, uninit_sz, entry_rva, base_code = struct.unpack_from(
            "<HBBIIIII", data, opt_offset
        )
        report("PE32+ Magic (0x020B)", magic == 0x020B, f"got 0x{magic:04X}")
        report("Entry point RVA == 0x1000", entry_rva == 0x1000, f"got 0x{entry_rva:08X}")

        img_base, sec_align, file_align = struct.unpack_from("<QII", data, opt_offset + 24)
        report("ImageBase == 0x140000000", img_base == 0x140000000, f"got 0x{img_base:016X}")
        report("SectionAlignment == 0x1000", sec_align == 0x1000, f"got 0x{sec_align:X}")
        report("FileAlignment == 0x200", file_align == 0x200, f"got 0x{file_align:X}")

        subsys, = struct.unpack_from("<H", data, opt_offset + 68)
        report("Subsystem Windows CUI (3)", subsys == 3, f"got {subsys}")

        # 5. Section Headers
        sec_offset = opt_offset + opt_size
        names = []
        for i in range(num_sec):
            name_raw = data[sec_offset + i * 40 : sec_offset + i * 40 + 8]
            name = name_raw.split(b"\x00")[0].decode("ascii", errors="ignore")
            names.append(name)

        report("Section names [.text, .rdata, .data, .idata]",
               names == [".text", ".rdata", ".data", ".idata"],
               f"got {names}")

        # 6. IDATA Import table check
        idata_raw = data[data.find(b"KERNEL32.dll") :]
        report("Imports KERNEL32.dll", len(idata_raw) > 0)
        report("Imports ExitProcess", b"ExitProcess" in data)


def test_objdump_inspection():
    """Verify that host binutils objdump parses the emitted PE32+ and COFF files."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "hello.c"
        exe = Path(td) / "hello.exe"
        obj = Path(td) / "hello.obj"

        src.write_text("""
        int global_var = 123;
        const char *msg = "Windows PE/COFF test";
        int get_val(void) { return global_var; }
        int main(void) {
            return get_val() + (int)msg[0];
        }
        """)

        # 1. Compile to PE32+ executable
        cmd_exe = [str(ZEROCC), str(src), "-o", str(exe)]
        proc = subprocess.run(cmd_exe, capture_output=True, text=True)
        report("Compile hello.c to hello.exe", proc.returncode == 0, proc.stderr)

        # Inspect with objdump -p (private headers)
        dump_p = subprocess.run(["objdump", "-p", str(exe)], capture_output=True, text=True)
        report("objdump -p recognises pei-x86-64",
               dump_p.returncode == 0 and "pei-x86-64" in dump_p.stdout,
               dump_p.stderr[:200])

        # Inspect disassembly of entry point
        dump_d = subprocess.run(["objdump", "-d", str(exe)], capture_output=True, text=True)
        report("objdump -d disassembles CRT entry stub",
               dump_d.returncode == 0 and "sub" in dump_d.stdout and "call" in dump_d.stdout,
               dump_d.stderr[:200])

        # 2. Compile to relocatable COFF object (.obj)
        cmd_obj = [str(ZEROCC), "-c", str(src), "-o", str(obj)]
        proc_obj = subprocess.run(cmd_obj, capture_output=True, text=True)
        report("Compile hello.c to hello.obj (-c)", proc_obj.returncode == 0, proc_obj.stderr)

        # Inspect with objdump -x (all headers, symbols)
        dump_x = subprocess.run(["objdump", "-x", str(obj)], capture_output=True, text=True)
        report("objdump -x recognises pe-x86-64 COFF object",
               dump_x.returncode == 0 and "pe-x86-64" in dump_x.stdout,
               dump_x.stderr[:200])
        report("objdump -x contains symbol table",
               "SYMBOL TABLE" in dump_x.stdout and "main" in dump_x.stdout,
               dump_x.stdout[:300])

        # Inspect relocations with objdump -r
        dump_r = subprocess.run(["objdump", "-r", str(obj)], capture_output=True, text=True)
        report("objdump -r contains COFF relocations",
               dump_r.returncode == 0 and "RELOCATION RECORDS" in dump_r.stdout,
               dump_r.stdout[:300])


def test_cli_flags():
    """Verify --pe, -mpe, and --target=x86_64-w64-mingw32 driver options."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "test.c"
        src.write_text("int main(void) { return 0; }\n")

        # --pe flag with default output
        out1 = Path(td) / "out1.exe"
        cmd1 = [str(ZEROCC), "--pe", str(src), "-o", str(out1)]
        proc1 = subprocess.run(cmd1, capture_output=True, text=True)
        report("CLI --pe flag generates valid PE",
               proc1.returncode == 0 and out1.exists() and out1.read_bytes()[:2] == b"MZ",
               proc1.stderr)

        # -mpe flag
        out2 = Path(td) / "out2.exe"
        cmd2 = [str(ZEROCC), "-mpe", str(src), "-o", str(out2)]
        proc2 = subprocess.run(cmd2, capture_output=True, text=True)
        report("CLI -mpe flag generates valid PE",
               proc2.returncode == 0 and out2.exists() and out2.read_bytes()[:2] == b"MZ",
               proc2.stderr)

        # --target=x86_64-w64-mingw32
        out3 = Path(td) / "out3.exe"
        cmd3 = [str(ZEROCC), "--target=x86_64-w64-mingw32", str(src), "-o", str(out3)]
        proc3 = subprocess.run(cmd3, capture_output=True, text=True)
        report("CLI --target=x86_64-w64-mingw32 generates valid PE",
               proc3.returncode == 0 and out3.exists() and out3.read_bytes()[:2] == b"MZ",
               proc3.stderr)

        # --target=pe-x86-64
        out4 = Path(td) / "out4.exe"
        cmd4 = [str(ZEROCC), "--target=pe-x86-64", str(src), "-o", str(out4)]
        proc4 = subprocess.run(cmd4, capture_output=True, text=True)
        report("CLI --target=pe-x86-64 generates valid PE",
               proc4.returncode == 0 and out4.exists() and out4.read_bytes()[:2] == b"MZ",
               proc4.stderr)


def test_optimization_sweep():
    """Verify PE generation across optimization levels -O0 to -O3."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "opt.c"
        src.write_text("""
        static int helper(int a, int b) {
            int sum = 0;
            for (int i = a; i <= b; i++) sum += i;
            return sum;
        }
        int main(void) {
            return helper(1, 10);
        }
        """)

        for opt in ["-O0", "-O1", "-O2", "-O3"]:
            exe = Path(td) / f"opt{opt}.exe"
            cmd = [str(ZEROCC), opt, str(src), "-o", str(exe)]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            ok = proc.returncode == 0 and exe.exists() and exe.read_bytes()[:2] == b"MZ"
            report(f"PE32+ compilation at {opt}", ok, proc.stderr)


def test_dll_exports_and_headers():
    """Verify Windows DLL (.dll) emission with .edata export table (Sprint 249)."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "math.c"
        dll = Path(td) / "math.dll"
        src.write_text("""
        int add(int a, int b) { return a + b; }
        int mul(int a, int b) { return a * b; }
        """)

        cmd = [str(ZEROCC), "-shared", str(src), "-o", str(dll)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("DLL compilation with -shared", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        data = dll.read_bytes()
        e_lfanew, = struct.unpack_from("<I", data, 60)
        coff_offset = e_lfanew + 4
        machine, num_sec, _, _, _, opt_size, charact = struct.unpack_from("<HHIIIHH", data, coff_offset)

        report("DLL IMAGE_FILE_DLL characteristic (0x2000)", bool(charact & 0x2000), f"charact=0x{charact:04X}")
        report("DLL section count == 5 (with .edata)", num_sec == 5, f"got {num_sec}")

        # Section headers inspection
        sec_offset = coff_offset + 20 + opt_size
        names = []
        for i in range(num_sec):
            name_raw = data[sec_offset + i * 40 : sec_offset + i * 40 + 8]
            names.append(name_raw.split(b"\x00")[0].decode("ascii", errors="ignore"))
        report("Section table contains .edata", ".edata" in names, f"sections={names}")

        # Private headers via objdump -p
        dump_p = subprocess.run(["objdump", "-p", str(dll)], capture_output=True, text=True)
        report("objdump -p recognizes DLL export table",
               "The Export Tables" in dump_p.stdout and "math.dll" in dump_p.stdout,
               dump_p.stdout[:300])
        report("Export table contains 'add' and 'mul'",
               "add" in dump_p.stdout and "mul" in dump_p.stdout,
               dump_p.stdout[:300])


def test_gui_subsystem():
    """Verify -mwindows GUI subsystem flag and WinMain entry point (Sprint 249)."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "gui.c"
        exe = Path(td) / "gui.exe"
        src.write_text("""
        int WinMain(void) {
            return 77;
        }
        """)

        cmd = [str(ZEROCC), "-mwindows", str(src), "-o", str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("GUI executable compilation (-mwindows)", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        data = exe.read_bytes()
        e_lfanew, = struct.unpack_from("<I", data, 60)
        opt_offset = e_lfanew + 4 + 20
        subsys, = struct.unpack_from("<H", data, opt_offset + 68)
        report("Subsystem Windows GUI == 2", subsys == 2, f"got {subsys}")

        dump_d = subprocess.run(["objdump", "-d", str(exe)], capture_output=True, text=True)
        report("CRT stub disassembles and invokes WinMain",
               dump_d.returncode == 0 and "call" in dump_d.stdout,
               dump_d.stderr[:200])


def test_multi_dll_msvcrt_imports():
    """Verify multi-DLL import routing (KERNEL32.dll + msvcrt.dll) and IAT thunks (Sprint 249)."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "app.c"
        exe = Path(td) / "app.exe"
        src.write_text("""
        extern int puts(const char *);
        extern void *malloc(unsigned long);
        extern void free(void *);
        int main(void) {
            puts("Hello Windows PE");
            void *p = malloc(64);
            free(p);
            return 0;
        }
        """)

        cmd = [str(ZEROCC), str(src), "-o", str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Multi-DLL app compilation with CRT calls", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        dump_p = subprocess.run(["objdump", "-p", str(exe)], capture_output=True, text=True)
        report("Imports KERNEL32.dll (ExitProcess)",
               "KERNEL32.dll" in dump_p.stdout and "ExitProcess" in dump_p.stdout,
               dump_p.stdout[:300])
        report("Imports msvcrt.dll (puts, malloc, free)",
               "msvcrt.dll" in dump_p.stdout and "puts" in dump_p.stdout and "malloc" in dump_p.stdout,
               dump_p.stdout[:300])

        dump_d = subprocess.run(["objdump", "-d", str(exe)], capture_output=True, text=True)
        report("Disassembly contains IAT indirect jump thunks",
               "jmp" in dump_d.stdout and "(%rip)" in dump_d.stdout,
               dump_d.stdout[:300])


def test_multi_tu_source_linking():
    """Verify multi-TU compilation and native PE linking from multiple .c sources (Sprint 259)."""
    with tempfile.TemporaryDirectory() as td:
        src1 = Path(td) / "main.c"
        src2 = Path(td) / "helper.c"
        exe = Path(td) / "multi.exe"

        src1.write_text("""
        extern int helper(int);
        extern int g_shared_var;
        int main(void) {
            return helper(g_shared_var);
        }
        """)

        src2.write_text("""
        int g_shared_var = 40;
        int helper(int a) {
            return a + 2;
        }
        """)

        cmd = [str(ZEROCC), "-mpe", str(src1), str(src2), "-o", str(exe)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        report("Multi-TU .c source compilation and linking (-mpe)", proc.returncode == 0, proc.stderr)
        if proc.returncode != 0:
            return

        report("Multi-TU executable generated with MZ magic",
               exe.exists() and exe.read_bytes()[:2] == b"MZ")

        dump_p = subprocess.run(["objdump", "-p", str(exe)], capture_output=True, text=True)
        report("Multi-TU pei-x86-64 recognized by objdump",
               dump_p.returncode == 0 and "pei-x86-64" in dump_p.stdout,
               dump_p.stderr[:200])

        dump_d = subprocess.run(["objdump", "-d", str(exe)], capture_output=True, text=True)
        report("Multi-TU binary disassembles cleanly",
               dump_d.returncode == 0 and "call" in dump_d.stdout,
               dump_d.stderr[:200])


def test_multi_tu_obj_linking():
    """Verify separate compilation to COFF .obj and subsequent PE multi-object linking (Sprint 259)."""
    with tempfile.TemporaryDirectory() as td:
        src1 = Path(td) / "part1.c"
        src2 = Path(td) / "part2.c"
        obj1 = Path(td) / "part1.obj"
        obj2 = Path(td) / "part2.obj"
        exe = Path(td) / "app.exe"

        src1.write_text("""
        extern int compute(int x, int y);
        extern const char *get_greeting(void);
        int main(void) {
            const char *msg = get_greeting();
            return compute(10, 20) + (int)msg[0];
        }
        """)

        src2.write_text("""
        static const char message[] = "Hello Multi-TU COFF";
        const char *get_greeting(void) {
            return message;
        }
        int compute(int x, int y) {
            return x * y + 5;
        }
        """)

        # Compile part1.c -> part1.obj
        cmd1 = [str(ZEROCC), "-mpe", "-c", str(src1), "-o", str(obj1)]
        proc1 = subprocess.run(cmd1, capture_output=True, text=True)
        report("Compile part1.c to part1.obj", proc1.returncode == 0, proc1.stderr)

        # Compile part2.c -> part2.obj
        cmd2 = [str(ZEROCC), "-mpe", "-c", str(src2), "-o", str(obj2)]
        proc2 = subprocess.run(cmd2, capture_output=True, text=True)
        report("Compile part2.c to part2.obj", proc2.returncode == 0, proc2.stderr)

        # Verify .obj inspection with objdump
        dump_x1 = subprocess.run(["objdump", "-x", str(obj1)], capture_output=True, text=True)
        dump_x2 = subprocess.run(["objdump", "-x", str(obj2)], capture_output=True, text=True)
        report("part1.obj has COFF symbol table", "SYMBOL TABLE" in dump_x1.stdout)
        report("part2.obj has COFF symbol table", "SYMBOL TABLE" in dump_x2.stdout)

        # Link part1.obj + part2.obj -> app.exe
        cmd_link = [str(ZEROCC), "-mpe", str(obj1), str(obj2), "-o", str(exe)]
        proc_link = subprocess.run(cmd_link, capture_output=True, text=True)
        report("Link part1.obj + part2.obj to app.exe", proc_link.returncode == 0, proc_link.stderr)
        if proc_link.returncode != 0:
            return

        report("Linked app.exe has MZ magic and size > 1KB",
               exe.exists() and exe.stat().st_size > 1024 and exe.read_bytes()[:2] == b"MZ")

        dump_p = subprocess.run(["objdump", "-p", str(exe)], capture_output=True, text=True)
        report("Linked app.exe imports KERNEL32.dll ExitProcess",
               "KERNEL32.dll" in dump_p.stdout and "ExitProcess" in dump_p.stdout,
               dump_p.stdout[:300])


def test_mixed_c_and_obj_linking():
    """Verify mixed compilation and linking (.c + .obj) in a single driver invocation (Sprint 259)."""
    with tempfile.TemporaryDirectory() as td:
        src1 = Path(td) / "driver.c"
        src2 = Path(td) / "module.c"
        obj2 = Path(td) / "module.obj"
        exe = Path(td) / "mixed.exe"

        src1.write_text("""
        extern int module_fn(void);
        int main(void) {
            return module_fn();
        }
        """)

        src2.write_text("""
        int module_fn(void) {
            return 99;
        }
        """)

        # Compile module.c to module.obj
        cmd_obj = [str(ZEROCC), "-mpe", "-c", str(src2), "-o", str(obj2)]
        proc_obj = subprocess.run(cmd_obj, capture_output=True, text=True)
        report("Compile module.c to module.obj", proc_obj.returncode == 0, proc_obj.stderr)

        # Compile driver.c + module.obj in one step
        cmd_mixed = [str(ZEROCC), "-mpe", str(src1), str(obj2), "-o", str(exe)]
        proc_mixed = subprocess.run(cmd_mixed, capture_output=True, text=True)
        report("Mixed .c and .obj linking in single invocation", proc_mixed.returncode == 0, proc_mixed.stderr)
        if proc_mixed.returncode != 0:
            return

        report("Mixed .exe has MZ header",
               exe.exists() and exe.read_bytes()[:2] == b"MZ")


def main():
    if not ZEROCC.exists():
        print(f"Error: {ZEROCC} not found. Run make first.", file=sys.stderr)
        sys.exit(1)

    print("=== ZeroCC Windows PE/COFF Verification Battery ===")
    test_pe_exe_headers()
    test_objdump_inspection()
    test_cli_flags()
    test_optimization_sweep()
    test_dll_exports_and_headers()
    test_gui_subsystem()
    test_multi_dll_msvcrt_imports()
    test_multi_tu_source_linking()
    test_multi_tu_obj_linking()
    test_mixed_c_and_obj_linking()

    total = passed + failed
    print(f"\nResults: {passed}/{total} passed, {failed} failed.")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
