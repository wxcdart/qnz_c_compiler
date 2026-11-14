#!/usr/bin/env python3
"""
tools/verify/self_host_audit.py — Self-Hosting Audit Tool (Sprint 184).

Attempts to compile all ZeroCC translation units in src/ with ./build/bin/zerocc -c,
reporting passed/failed files, error categories, and failure details.
"""

import os
import subprocess
import sys
from pathlib import Path

# Path setup
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

ROOT = Path(find_repo_root())
ZEROCC = ROOT / "build" / "bin" / "zerocc"

def find_source_files():
    src_dir = ROOT / "src"
    sources = []
    for root, _, files in os.walk(src_dir):
        for f in sorted(files):
            if f.endswith(".c"):
                sources.append(Path(root) / f)
    return sorted(sources)

def audit_file(src_path, opt_level=0):
    rel_path = src_path.relative_to(ROOT)
    cmd = [
        str(ZEROCC),
        f"-O{opt_level}",
        "-Isrc",
        "-Iinclude",
        "-c",
        str(src_path),
        "-o",
        "/dev/null",
    ]
    proc = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr

def main():
    if not ZEROCC.exists():
        print(f"Error: {ZEROCC} does not exist. Run 'make rebuild' first.", file=sys.stderr)
        sys.exit(1)

    sources = find_source_files()
    total = len(sources)
    print(f"Auditing {total} source files with {ZEROCC} (-O0)...")

    passed = []
    failed = []

    for idx, src in enumerate(sources, 1):
        rel = src.relative_to(ROOT)
        rc, stdout, stderr = audit_file(src, opt_level=0)
        if rc == 0:
            passed.append(rel)
            print(f"[{idx:02d}/{total:02d}] PASS: {rel}")
        else:
            failed.append((rel, rc, stderr.strip()))
            print(f"[{idx:02d}/{total:02d}] FAIL: {rel} (rc={rc})")

    print("\n" + "=" * 70)
    print("SELF-HOSTING COMPILATION AUDIT SUMMARY")
    print("=" * 70)
    print(f"Total files  : {total}")
    print(f"Passed       : {len(passed)} ({len(passed)*100.0/total:.1f}%)")
    print(f"Failed       : {len(failed)} ({len(failed)*100.0/total:.1f}%)")
    print("=" * 70)

    if failed:
        print("\nFAILURE DETAILS:")
        for rel, rc, err in failed:
            print(f"\n--- {rel} (rc={rc}) ---")
            lines = err.splitlines()
            for l in lines[:10]:
                print(f"  {l}")
            if len(lines) > 10:
                print(f"  ... ({len(lines) - 10} more lines)")

    sys.exit(0 if not failed else 1)

if __name__ == "__main__":
    main()
