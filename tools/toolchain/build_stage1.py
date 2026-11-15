#!/usr/bin/env python3
"""
tools/toolchain/build_stage1.py — ZeroCC Self-Hosting Stage Builder (Sprint 184).

Compiles all ZeroCC library translation units, main.c, and host zero_rt objects
into stage-specific objects, links them with musl-gcc, and embeds the ELF symbol
table for JIT self-resolution.

Usage:
    python3 tools/toolchain/build_stage1.py           # Build stage 1 (zerocc -> zerocc-stage1)
    python3 tools/toolchain/build_stage1.py --stage 1 # Same
    python3 tools/toolchain/build_stage1.py --stage 2 # Build stage 2 (zerocc-stage1 -> zerocc-stage2)
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

# Path setup
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

ROOT = Path(find_repo_root())
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

def compile_obj(compiler, src_path, obj_path):
    obj_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(compiler),
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

def build_stage(stage=1):
    if stage == 1:
        compiler = ROOT / "build" / "bin" / "zerocc"
        stage_dir = ROOT / "build" / "stage1"
        out_bin = ROOT / "build" / "bin" / "zerocc-stage1"
    elif stage == 2:
        compiler = ROOT / "build" / "bin" / "zerocc-stage1"
        stage_dir = ROOT / "build" / "stage2"
        out_bin = ROOT / "build" / "bin" / "zerocc-stage2"
    else:
        print(f"Error: Unknown stage {stage}", file=sys.stderr)
        sys.exit(1)

    if not compiler.exists():
        print(f"Error: Required compiler {compiler} does not exist.", file=sys.stderr)
        if stage == 1:
            print("Run 'make build/bin/zerocc' first.", file=sys.stderr)
        else:
            print("Run 'python3 tools/toolchain/build_stage1.py --stage 1' first.", file=sys.stderr)
        sys.exit(1)

    obj_dir = stage_dir / "obj"
    embed_c = stage_dir / "embedded_syms.c"
    embed_o = obj_dir / "embedded_syms.o"
    obj_dir.mkdir(parents=True, exist_ok=True)

    # 1. Gather all objects to compile
    lib_objs = parse_makefile_objects()
    print(f"[Stage {stage}] Compiling {len(lib_objs)} library objects with {compiler.name}...")
    compiled_objs = []
    for rel_o in lib_objs:
        src_c = ROOT / (rel_o.replace(".o", ".c"))
        out_o = obj_dir / Path(rel_o).name
        compile_obj(compiler, src_c, out_o)
        compiled_objs.append(out_o)

    # 2. Main object
    print(f"[Stage {stage}] Compiling main.c...")
    main_o = obj_dir / "main.o"
    compile_obj(compiler, ROOT / "src" / "main.c", main_o)
    compiled_objs.append(main_o)

    # 3. zero_rt host objects
    host_rt_files = [
        ROOT / "zero_rt" / "core" / "int128.c",
        ROOT / "zero_rt" / "jit" / "san_report_host.c",
        ROOT / "zero_rt" / "dlfcn" / "dlfcn.c",
        ROOT / "zero_rt" / "dlfcn" / "dynlink.c",
        ROOT / "zero_rt" / "complex" / "complex.c",
    ]
    print(f"[Stage {stage}] Compiling {len(host_rt_files)} host zero_rt objects...")
    for h_src in host_rt_files:
        h_obj = obj_dir / (h_src.stem + ".o")
        compile_obj(compiler, h_src, h_obj)
        compiled_objs.append(h_obj)

    # 4. Embedded syms: initial generation or reuse
    if not embed_c.exists():
        subprocess.run([
            sys.executable, str(ROOT / "tools" / "toolchain" / "embed_symtab.py"),
            "--emit-empty", str(embed_c)
        ], cwd=str(ROOT))
    compile_obj(compiler, embed_c, embed_o)
    all_objs = compiled_objs + [embed_o]

    # 5. Link binary
    print(f"[Stage {stage}] Linking {out_bin} with musl-gcc...")
    link_cmd = [
        str(MUSL_GCC),
        "-static", "-no-pie",
        "-Wl,--whole-archive", str(MUSL_LIBC), "-Wl,--no-whole-archive",
        "-o", str(out_bin)
    ] + [str(o) for o in all_objs]
    res = subprocess.run(link_cmd, cwd=str(ROOT), capture_output=True, text=True)
    if res.returncode != 0:
        print("Link failed:", file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        sys.exit(1)

    # 6. Update embedded syms
    print(f"[Stage {stage}] Regenerating embedded symbols...")
    res = subprocess.run([
        sys.executable, str(ROOT / "tools" / "toolchain" / "embed_symtab.py"),
        "--bin", str(out_bin), "--out", str(embed_c)
    ], cwd=str(ROOT))
    if res.returncode == 3:
        print(f"[Stage {stage}] Embedded symtab changed, recompiling embedded_syms.o and relinking...")
        compile_obj(compiler, embed_c, embed_o)
        res2 = subprocess.run(link_cmd, cwd=str(ROOT), capture_output=True, text=True)
        if res2.returncode != 0:
            print("Relink failed:", file=sys.stderr)
            print(res2.stderr, file=sys.stderr)
            sys.exit(1)

    print(f"[Stage {stage}] Successfully built {out_bin}!")
    return out_bin

def main():
    parser = argparse.ArgumentParser(description="ZeroCC Stage Builder")
    parser.add_argument("--stage", type=int, default=1, choices=[1, 2], help="Stage to build (1 or 2)")
    args = parser.parse_args()
    build_stage(args.stage)

if __name__ == "__main__":
    main()
