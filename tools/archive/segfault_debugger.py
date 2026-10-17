#!/usr/bin/env python3
"""Segfault Debugger - Automatically runs compiler under gdb and extracts backtrace.

Helps identify the exact location of segmentation faults in the compiler.

Usage:
    python3 tools/segfault_debugger.py --target aarch64 test/aot/hello_world.c
"""

import subprocess
import sys
import re
from pathlib import Path
from typing import Optional

def run_under_gdb(target: str, source: Path, output: Path, binary: str = "./build/bin/zerocc") -> Optional[str]:
    """Run compiler under gdb and capture backtrace."""
    
    gdb_commands = [
        "break main",
        "run --target " + target + " -c " + str(source) + " -o " + str(output),
        "bt 50",
        "quit"
    ]
    
    gdb_script = "\n".join(gdb_commands)
    
    try:
        result = subprocess.run(
            ["gdb", "-batch", "-x", "/dev/stdin", binary],
            input=gdb_script,
            capture_output=True,
            text=True,
            timeout=60
        )
        return result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return "TIMEOUT: gdb execution timed out"
    except FileNotFoundError:
        return "ERROR: gdb not found in PATH"

def run_under_valgrind(target: str, source: Path, output: Path, binary: str = "./build/bin/zerocc") -> Optional[str]:
    """Run compiler under valgrind to detect memory errors."""
    
    try:
        result = subprocess.run(
            ["valgrind", "--leak-check=full", "--track-origins=yes",
             binary, "--target", target, "-c", str(source), "-o", str(output)],
            capture_output=True,
            text=True,
            timeout=120
        )
        return result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return "TIMEOUT: valgrind execution timed out"
    except FileNotFoundError:
        return "ERROR: valgrind not found in PATH"

def extract_backtrace(output: str) -> list[str]:
    """Extract backtrace from gdb output."""
    backtrace = []
    in_backtrace = False
    
    for line in output.splitlines():
        if "#0" in line or "backtrace" in line.lower():
            in_backtrace = True
        if in_backtrace:
            backtrace.append(line)
            if line.strip().startswith("quit") or "#99" in line:
                break
    
    return backtrace

def identify_crash_site(backtrace: list[str]) -> Optional[str]:
    """Identify the most likely crash site from backtrace."""
    # Look for frames in our code (not system libraries)
    for line in backtrace:
        if "zerocc" in line or "zero_cc" in line:
            return line
    return None

def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description='Debug compiler segfaults')
    parser.add_argument('--target', required=True, help='Target ISA')
    parser.add_argument('source', type=Path, help='Source file that causes segfault')
    parser.add_argument('-o', dest='output', default='/tmp/segfault_test.o', type=Path)
    parser.add_argument('--valgrind', action='store_true', help='Use valgrind instead of gdb')
    parser.add_argument('--binary', default='./build/bin/zerocc', help='Compiler binary path')
    args = parser.parse_args()
    
    if not args.source.exists():
        print(f"[error] Source file not found: {args.source}")
        return 1
    
    print(f"=== Segfault Debugger ===")
    print(f"Target: {args.target}")
    print(f"Source: {args.source}")
    print(f"Binary: {args.binary}")
    print()
    
    if args.valgrind:
        print("Running under valgrind...")
        output = run_under_valgrind(args.target, args.source, args.output, args.binary)
    else:
        print("Running under gdb...")
        output = run_under_gdb(args.target, args.source, args.output, args.binary)
    
    if not output:
        print("[error] Failed to run debugger")
        return 1
    
    if "ERROR:" in output or "TIMEOUT:" in output:
        print(output)
        return 1
    
    backtrace = extract_backtrace(output)
    
    print("\n=== Backtrace ===")
    for line in backtrace[:30]:  # Show first 30 frames
        print(line)
    
    crash_site = identify_crash_site(backtrace)
    if crash_site:
        print(f"\n=== Likely Crash Site ===")
        print(crash_site)
    
    # Check for common issues
    print("\n=== Analysis ===")
    if "SIGSEGV" in output or "Segmentation fault" in output:
        print("✓ Confirmed: Segmentation fault detected")
    if "null pointer" in output.lower():
        print("⚠ Possible null pointer dereference")
    if "stack" in output.lower() and "overflow" in output.lower():
        print("⚠ Possible stack overflow")
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
