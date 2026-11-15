#!/usr/bin/env python3
"""
tools/verify/self_host_cycle.py — Self-Hosting Fixed-Point Verification Gate (Sprint 184).

Verifies the complete 3-stage bootstrap cycle (Stage 0 -> Stage 1 -> Stage 2):
1. Verifies/builds Stage 1 objects and binary (zerocc -> zerocc-stage1).
2. Verifies/builds Stage 2 objects and binary (zerocc-stage1 -> zerocc-stage2).
3. Compares every object file between Stage 1 and Stage 2 for bit-for-bit identity.
4. Compares the final linked binaries (zerocc-stage1 vs zerocc-stage2) for bit-for-bit identity.
"""

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

STAGE1_BIN = ROOT / "build" / "bin" / "zerocc-stage1"
STAGE2_BIN = ROOT / "build" / "bin" / "zerocc-stage2"
STAGE1_OBJ_DIR = ROOT / "build" / "stage1" / "obj"
STAGE2_OBJ_DIR = ROOT / "build" / "stage2" / "obj"

def ensure_stages():
    if not STAGE1_BIN.exists() or not STAGE1_OBJ_DIR.exists():
        print("Stage 1 binary not found. Building Stage 1...")
        build_stage1.build_stage(1)

    print("Building Stage 2 with zerocc-stage1...")
    build_stage1.build_stage(2)

def compare_objects():
    lib_objs = build_stage1.parse_makefile_objects()
    all_obj_names = [Path(rel_o).name for rel_o in lib_objs]
    all_obj_names.append("main.o")
    host_rt_files = ["int128.o", "san_report_host.o", "dlfcn.o", "dynlink.o", "complex.o"]
    all_obj_names.extend(host_rt_files)

    total = len(all_obj_names)
    matched = 0
    diffs = []
    missing = []

    print(f"\nComparing {total} object files between Stage 1 and Stage 2...")

    for name in all_obj_names:
        s1_file = STAGE1_OBJ_DIR / name
        s2_file = STAGE2_OBJ_DIR / name

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
    print("SELF-HOSTING BOOTSTRAP FIXED-POINT CONVERGENCE")
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
    if STAGE1_BIN.exists() and STAGE2_BIN.exists():
        bin1_bytes = STAGE1_BIN.read_bytes()
        bin2_bytes = STAGE2_BIN.read_bytes()
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
        print("\n[SUCCESS] Stage 1 and Stage 2 are 100% bit-for-bit identical (fixed point reached)!")
        return 0
    else:
        print("\n[FAIL] Fixed-point divergence detected between Stage 1 and Stage 2.")
        return 1

def main():
    ensure_stages()
    rc = compare_objects()
    sys.exit(rc)

if __name__ == "__main__":
    main()
