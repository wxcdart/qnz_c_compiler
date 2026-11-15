#!/usr/bin/env python3
"""
tools/verify/self_host_cycle.py — Self-Hosting Fixed-Point Verification Gate (Sprint 184).

Verifies the complete 3-stage bootstrap cycle (Stage 0 -> Stage 1 -> Stage 2):
1. Verifies/builds Stage 1 objects and binary (zerocc -> zerocc-stage1).
2. Verifies/builds Stage 2 objects and binary (zerocc-stage1 -> zerocc-stage2).
3. Compares every object file between Stage 1 and Stage 2 for bit-for-bit identity.
4. Compares the final linked binaries (zerocc-stage1 vs zerocc-stage2) for bit-for-bit identity.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

# Path setup
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

ROOT = Path(find_repo_root())
sys.path.insert(0, str(ROOT / "tools" / "toolchain"))
import build_stage1

def ensure_stages(own_linker=False):
    suffix = "-ownlink" if own_linker else ""
    dir_suffix = "_ownlink" if own_linker else ""
    s1_bin = ROOT / "build" / "bin" / f"zerocc-stage1{suffix}"
    s1_obj_dir = ROOT / "build" / f"stage1{dir_suffix}" / "obj"

    if not s1_bin.exists() or not s1_obj_dir.exists():
        print(f"Stage 1 binary not found. Building Stage 1{suffix}...")
        build_stage1.build_stage(1, own_linker=own_linker)

    print(f"Building Stage 2 with {s1_bin.name}...")
    build_stage1.build_stage(2, own_linker=own_linker)

def compare_objects(own_linker=False):
    suffix = "-ownlink" if own_linker else ""
    dir_suffix = "_ownlink" if own_linker else ""
    s1_bin = ROOT / "build" / "bin" / f"zerocc-stage1{suffix}"
    s2_bin = ROOT / "build" / "bin" / f"zerocc-stage2{suffix}"
    s1_obj_dir = ROOT / "build" / f"stage1{dir_suffix}" / "obj"
    s2_obj_dir = ROOT / "build" / f"stage2{dir_suffix}" / "obj"

    lib_objs = build_stage1.parse_makefile_objects()
    all_obj_names = [Path(rel_o).name for rel_o in lib_objs]
    all_obj_names.append("main.o")
    host_rt_files = ["int128.o", "san_report_host.o", "dlfcn.o", "dynlink.o", "complex.o"]
    all_obj_names.extend(host_rt_files)

    total = len(all_obj_names)
    matched = 0
    diffs = []
    missing = []

    print(f"\nComparing {total} object files between Stage 1{suffix} and Stage 2{suffix}...")

    for name in all_obj_names:
        s1_file = s1_obj_dir / name
        s2_file = s2_obj_dir / name

        if not s1_file.exists() or not s2_file.exists():
            missing.append(name)
            continue

        s1_bytes = s1_file.read_bytes()
        s2_bytes = s2_file.read_bytes()

        if s1_bytes == s2_bytes:
            matched += 1
        else:
            diffs.append((name, len(s1_bytes), len(s2_bytes)))

    print("\n" + "=" * 70)
    tag = "OWN-LINKER SELF-HOSTING BOOTSTRAP FIXED-POINT CONVERGENCE" if own_linker else "SELF-HOSTING BOOTSTRAP FIXED-POINT CONVERGENCE"
    print(tag)
    print("=" * 70)
    print(f"Total objects compared : {total}")
    print(f"Bit-for-bit identical  : {matched} ({matched * 100.0 / total:.1f}%)")
    print(f"Differences            : {len(diffs)}")
    print(f"Missing                : {len(missing)}")

    if diffs:
        print("\nDIFFERING OBJECTS:")
        for name, sz1, sz2 in diffs:
            print(f"  FAIL: {name} (stage1={sz1} bytes, stage2={sz2} bytes)")

    if missing:
        print("\nMISSING OBJECTS:")
        for name in missing:
            print(f"  MISSING: {name}")

    # Binary comparison
    print("-" * 70)
    binary_identical = False
    if s1_bin.exists() and s2_bin.exists():
        bin1_bytes = s1_bin.read_bytes()
        bin2_bytes = s2_bin.read_bytes()
        if bin1_bytes == bin2_bytes:
            binary_identical = True
            print(f"Linked Binaries        : BIT-FOR-BIT IDENTICAL ({len(bin1_bytes)} bytes)")
        else:
            print(f"Linked Binaries        : DIFFER (stage1={len(bin1_bytes)}, stage2={len(bin2_bytes)})")
    else:
        print("Linked Binaries        : One or both binaries missing!")

    print("=" * 70)

    success = (matched == total) and binary_identical
    if success:
        print(f"\n[SUCCESS] Stage 1{suffix} and Stage 2{suffix} are 100% bit-for-bit identical (fixed point reached)!")
        return 0
    else:
        print(f"\n[FAIL] Fixed-point divergence detected between Stage 1{suffix} and Stage 2{suffix}.")
        return 1

def main():
    parser = argparse.ArgumentParser(description="ZeroCC Self-Hosting Cycle Verifier")
    parser.add_argument("--own-linker", action="store_true", help="Verify own-linked stages (zerocc-stage1-ownlink vs zerocc-stage2-ownlink)")
    args = parser.parse_args()
    ensure_stages(own_linker=args.own_linker)
    rc = compare_objects(own_linker=args.own_linker)
    sys.exit(rc)

if __name__ == "__main__":
    main()
