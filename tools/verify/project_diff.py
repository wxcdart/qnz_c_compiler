#!/usr/bin/env python3
"""Project-differential compile gate (Verification & Scale, item #1).

Compiles real-world C projects with zerocc and diffs the outcome against
a reference gcc compile of the same translation units:

  * test/projects/cjson   - cJSON 1.7.18 (cJSON.c, cJSON_Utils.c)
  * test/projects/zlib    - zlib 1.3.1 core (14 .c files)
  * test/projects/sqlite  - SQLite 3.4x amalgamation (sqlite3.c, ~265k lines)

Reference semantics: gcc -c must accept the TU; zerocc -c must accept it
too (0 diagnostics of class "error").  A run FAILS if zerocc rejects a
TU that gcc accepts (a compiler gap) - this is the gate the sqlite
burn-down ratchets against.

Usage:
  python3 tools/project_diff.py                 # all projects, -O0 and -O2
  python3 tools/project_diff.py --projects sqlite
  python3 tools/project_diff.py --levels 0 2 3
"""
import sys
from pathlib import Path

# Make the tools package importable so we can use find_repo_root.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = find_repo_root()
ZEROC = os.path.join(ROOT, "build", "bin", "zerocc")

PROJECTS = {
    "cjson": [
        "test/projects/cjson/cJSON.c",
        "test/projects/cjson/cJSON_Utils.c",
    ],
    "zlib": [
        "test/projects/zlib/adler32.c",
        "test/projects/zlib/crc32.c",
        "test/projects/zlib/deflate.c",
        "test/projects/zlib/inffast.c",
        "test/projects/zlib/inflate.c",
        "test/projects/zlib/trees.c",
        "test/projects/zlib/zutil.c",
        "test/projects/zlib/compress.c",
        "test/projects/zlib/uncompr.c",
        "test/projects/zlib/gzclose.c",
        "test/projects/zlib/gzlib.c",
        "test/projects/zlib/gzread.c",
        "test/projects/zlib/gzwrite.c",
        "test/projects/zlib/infback.c",
        "test/projects/zlib/inftrees.c",
    ],
    "sqlite": [
        "test/projects/sqlite/sqlite3.c",
    ],
}


def run(cmd, timeout=600):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT"


def gcc_accepts(src, flags):
    """Reference: does host gcc accept this TU (warnings allowed)?"""
    with tempfile.NamedTemporaryFile(suffix=".o", delete=False) as tf:
        obj = tf.name
    try:
        rc, _ = run(["gcc"] + flags + ["-c", src, "-o", obj])
        return rc == 0
    finally:
        if os.path.exists(obj):
            os.unlink(obj)


def zerocc_compiles(src, level):
    """zerocc -c must produce 0 'error:' diagnostics and exit 0."""
    with tempfile.NamedTemporaryFile(suffix=".o", delete=False) as tf:
        obj = tf.name
    try:
        rc, out = run([ZEROC, f"-O{level}", "-c", src, "-o", obj])
        errs = [l for l in out.splitlines() if "error:" in l or "Compile error" in l]
        ok = rc == 0 and not errs
        return ok, errs[:5], os.path.exists(obj)
    finally:
        if os.path.exists(obj):
            os.unlink(obj)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--projects", nargs="*", default=sorted(PROJECTS))
    ap.add_argument("--levels", nargs="*", default=["0", "2"])
    ap.add_argument("--no-gcc-ref", action="store_true",
                    help="skip the gcc reference check (offline mode)")
    args = ap.parse_args()

    if not os.path.exists(ZEROC):
        print(f"missing {ZEROC} - run `make build/bin/zerocc` first")
        return 2

    total = fail = 0
    for proj in args.projects:
        if proj not in PROJECTS:
            print(f"unknown project {proj}")
            return 2
        print(f"== {proj} ==")
        for src in PROJECTS[proj]:
            path = os.path.join(ROOT, src)
            ref_ok = True
            if not args.no_gcc_ref:
                ref_ok = gcc_accepts(path, ["-O2"])
                if not ref_ok:
                    print(f"  [REF-REJECT] {src} (gcc does not accept it; "
                          "skipped as reference bug)")
                    continue
            for lvl in args.levels:
                total += 1
                ok, errs, _ = zerocc_compiles(path, lvl)
                name = f"{os.path.basename(src)}@-O{lvl}"
                if ok:
                    print(f"  [OK]   {name}")
                else:
                    fail += 1
                    print(f"  [FAIL] {name}:")
                    for e in errs:
                        print(f"         {e}")
    print()
    print(f"project-diff: {total - fail}/{total} TUs compiled "
          f"({fail} failures)")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
