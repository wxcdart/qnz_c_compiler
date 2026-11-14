#!/usr/bin/env python3
"""
tools/toolchain/build_stage1.py — ZeroCC Stage 1 Self-Hosting Builder (Sprint 184).

Compiles all ZeroCC library translation units, main.c, and host zero_rt objects
using ./build/bin/zerocc -c, links them with musl-gcc into build/bin/zerocc-stage1,
and embeds the ELF symbol table for JIT self-resolution.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

# Path setup
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

ROOT = Path(find_repo_root())
ZEROCC = ROOT / "build" / "bin" / "zerocc"
STAGE1_OBJ_DIR = ROOT / "build" / "stage1" / "obj"
STAGE1_BIN = ROOT / "build" / "bin" / "zerocc-stage1"
STAGE1_EMBED_C = ROOT / "build" / "stage1" / "embedded_syms.c"
STAGE1_EMBED_O = STAGE1_OBJ_DIR / "embedded_syms.o"
MUSL_GCC = ROOT / "build" / "bin" / "musl-gcc"
MUSL_LIBC = ROOT / "build" / "musl" / "lib" / "libc.a"

def parse_makefile_objects():
    with open(ROOT / "Makefile") as f:
        content = f.read()
    extra_matches = re.findall(r"src/zero_cc/[a-zA-Z0-9_/]+\.o", content)
    seen = set()
    objs = []
    for m in extra_matches:
        if m not in seen:
            seen.add(m)
            objs.append(m)
    return objs

def compile_obj(src_path, obj_path):
    obj_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(ZEROCC),
        "-Isrc",
        "-Iinclude",
        "-c",
        str(src_path),
        "-o",
        str(obj_path)
    ]
    res = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Error compiling {src_path}:", file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        sys.exit(1)

def main():
    if not ZEROCC.exists():
        print(f"Error: {ZEROCC} does not exist. Run 'make rebuild' first.", file=sys.stderr)
        sys.exit(1)

    STAGE1_OBJ_DIR.mkdir(parents=True, exist_ok=True)

    # 1. Gather all objects to compile
    lib_objs = parse_makefile_objects()
    print(f"Compiling {len(lib_objs)} library objects with {ZEROCC}...")
    compiled_objs = []
    for rel_o in lib_objs:
        src_c = ROOT / (rel_o.replace(".o", ".c"))
        out_o = STAGE1_OBJ_DIR / Path(rel_o).name
        compile_obj(src_c, out_o)
        compiled_objs.append(out_o)

    # 2. Main object
    print("Compiling main.c...")
    main_o = STAGE1_OBJ_DIR / "main.o"
    compile_obj(ROOT / "src" / "main.c", main_o)
    compiled_objs.append(main_o)

    # 3. zero_rt host objects
    host_rt_files = [
        ROOT / "zero_rt" / "core" / "int128.c",
        ROOT / "zero_rt" / "jit" / "san_report_host.c",
        ROOT / "zero_rt" / "dlfcn" / "dlfcn.c",
        ROOT / "zero_rt" / "dlfcn" / "dynlink.c",
        ROOT / "zero_rt" / "complex" / "complex.c",
    ]
    print(f"Compiling {len(host_rt_files)} host zero_rt objects...")
    for h_src in host_rt_files:
        h_obj = STAGE1_OBJ_DIR / (h_src.stem + ".o")
        compile_obj(h_src, h_obj)
        compiled_objs.append(h_obj)

    # 4. Embedded syms: initial generation or reuse
    if not STAGE1_EMBED_C.exists():
        subprocess.run([
            sys.executable, str(ROOT / "tools" / "toolchain" / "embed_symtab.py"),
            "--emit-empty", str(STAGE1_EMBED_C)
        ], cwd=str(ROOT))
    compile_obj(STAGE1_EMBED_C, STAGE1_EMBED_O)
    all_objs = compiled_objs + [STAGE1_EMBED_O]

    # 5. Link stage 1 binary
    print(f"Linking {STAGE1_BIN} with musl-gcc...")
    link_cmd = [
        str(MUSL_GCC),
        "-static", "-no-pie",
        "-Wl,--whole-archive", str(MUSL_LIBC), "-Wl,--no-whole-archive",
        "-o", str(STAGE1_BIN)
    ] + [str(o) for o in all_objs]
    res = subprocess.run(link_cmd, cwd=str(ROOT), capture_output=True, text=True)
    if res.returncode != 0:
        print("Link failed:", file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        sys.exit(1)

    # 6. Update embedded syms
    print("Regenerating embedded symbols...")
    res = subprocess.run([
        sys.executable, str(ROOT / "tools" / "toolchain" / "embed_symtab.py"),
        "--bin", str(STAGE1_BIN), "--out", str(STAGE1_EMBED_C)
    ], cwd=str(ROOT))
    if res.returncode == 3:
        print("Embedded symtab changed, recompiling embedded_syms.o and relinking...")
        compile_obj(STAGE1_EMBED_C, STAGE1_EMBED_O)
        res2 = subprocess.run(link_cmd, cwd=str(ROOT), capture_output=True, text=True)
        if res2.returncode != 0:
            print("Relink failed:", file=sys.stderr)
            print(res2.stderr, file=sys.stderr)
            sys.exit(1)

    print(f"Successfully built {STAGE1_BIN}!")

if __name__ == "__main__":
    main()
