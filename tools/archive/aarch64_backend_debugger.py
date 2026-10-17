#!/usr/bin/env python3
"""AArch64 backend sanity‑checker.

This script runs the ZeroCC compiler on a given source file (or any set of
flags) targeting AArch64, captures the debug output, and inspects the produced
object file for the infamous X86‑style callee‑save prologue (bytes
0xA9BD7BFD).  If such a pattern is found the script exits with a non‑zero status
so it can be used in CI or as a quick sanity check while debugging.

Usage::

    python3 tools/aarch64_backend_debugger.py [zerocc‑flags]

All arguments are passed directly to ``./build/bin/zerocc``.  The ``-o``
argument is required (the script creates a temporary file if you omit it).
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Helper to run the compiler and capture its output
# ---------------------------------------------------------------------------
def run_zerocc(args: list[str]) -> tuple[int, str, str]:
    """Execute ``./build/bin/zerocc`` with *args*.

    Returns a tuple ``(returncode, stdout, stderr)``.
    """
    cmd = ["./build/bin/zerocc"] + args
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    return result.returncode, result.stdout, result.stderr

# ---------------------------------------------------------------------------
# Simple objdump‑based scanner for the X86 callee‑save pattern
# ---------------------------------------------------------------------------
X86_PATTERN_HEX = "fd 7b bd a9"  # little‑endian representation of 0xA9BD7BFD

def scan_objdump_for_pattern(obj_path: Path) -> bool:
    """Return True if the X86 callee‑save byte pattern appears in the .text
    section of *obj_path*.

    We invoke ``aarch64-linux-gnu-objdump -s -j .text`` which prints the raw
    hex bytes of the .text section.  A simple substring search is sufficient
    because the pattern is 4 consecutive bytes.
    """
    try:
        result = subprocess.run(
            ["aarch64-linux-gnu-objdump", "-s", "-j", ".text", str(obj_path)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
    except Exception as e:
        print(f"[error] objdump failed: {e}", file=sys.stderr)
        return False
    # Normalise whitespace so a simple "in" works
    hex_dump = " ".join(result.stdout.split())
    return X86_PATTERN_HEX in hex_dump

# ---------------------------------------------------------------------------
# Main driver
# ---------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run ZeroCC with AArch64 target and detect stray X86 callee‑save code"
    )
    parser.add_argument('zerocc_args', nargs=argparse.REMAINDER,
                        help='Arguments passed directly to ./build/bin/zerocc')
    args, _ = parser.parse_known_args()

    if not args.zerocc_args:
        print("[error] No arguments supplied for zerocc.  At minimum you need -c <src> -o <obj>", file=sys.stderr)
        return 2

    # Ensure we have an output object file; if the user omitted -o we create a temp file
    if '-o' not in args.zerocc_args:
        tmp_path = Path('/tmp/aarch64_debug.o')
        args.zerocc_args.extend(['-o', str(tmp_path)])
    else:
        # locate the file after the -o flag
        o_index = args.zerocc_args.index('-o')
        tmp_path = Path(args.zerocc_args[o_index + 1])

    # Remove any stale object file so we are sure the compiler rewrites it
    if tmp_path.exists():
        tmp_path.unlink()

    # Run the compiler
    retcode, stdout, stderr = run_zerocc(args.zerocc_args)
    # Print compiler stdout/stderr for user visibility (especially debug lines)
    sys.stdout.write(stdout)
    sys.stderr.write(stderr)
    if retcode != 0:
        print(f"[error] zerocc exited with status {retcode}", file=sys.stderr)
        return retcode

    if not tmp_path.is_file():
        print(f"[error] Expected object file {tmp_path} not produced.", file=sys.stderr)
        return 3

    # Scan the object file for the forbidden pattern
    has_pattern = scan_objdump_for_pattern(tmp_path)

    if has_pattern:
        print("\n=== DETECTED X86‑LIKE CALLEE‑SAVE IN AARCH64 OBJECT ===")
        print(f"  Pattern {X86_PATTERN_HEX} found in {tmp_path}")
        print("The object contains the X86‑style callee‑save prologue (0xA9BD7BFD) which must not appear in AArch64 code.")
        return 1
    else:
        print("\n=== AARCH64 BACKEND DEBUG REPORT ===")
        print(f"Target object: {tmp_path}")
        print("No X86‑style callee‑save pattern found.")
        # Optionally report the backend function pointers if present in stderr
        # Look for a line we added in the compiler (see docs).
        ptr_match = re.search(r"emit_prologue=0x([0-9a-fA-F]+)", stderr)
        if ptr_match:
            print(f"emit_prologue pointer = 0x{ptr_match.group(1)}")
        return 0

if __name__ == "__main__":
    sys.exit(main())
