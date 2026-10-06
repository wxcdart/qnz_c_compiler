#!/usr/bin/env python3
"""Backend Call Tracker - Monitors which backend functions are called during compilation.

This tool instruments the compiler to track which ISA-specific backend functions
are invoked for each function being compiled. It helps identify when x86-specific
code is being called for non-x86 targets.

Usage:
    python3 tools/backend_call_tracker.py --target aarch64 test/aot/hello_world.c
"""

import subprocess
import sys
import re
from pathlib import Path
from collections import defaultdict

def run_compiler_with_tracking(target: str, source: Path, output: Path) -> tuple[int, str]:
    """Compile with debug output enabled."""
    cmd = [
        "./build/bin/zerocc",
        "--target", target,
        "-c", str(source),
        "-o", str(output)
    ]
    
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=60
    )
    
    return result.returncode, result.stderr

def parse_backend_calls(debug_output: str) -> dict[str, list[str]]:
    """Extract backend function calls from debug output."""
    calls = defaultdict(list)
    
    # Patterns to match backend calls
    patterns = {
        'emit_prologue': r'emit_prologue\(([^)]+)\)',
        'emit_epilogue': r'emit_epilogue\(([^)]+)\)',
        'emit_mov_reg': r'emit_mov_reg\(([^)]+)\)',
        'emit_load': r'emit_load\(([^)]+)\)',
        'emit_store': r'emit_store\(([^)]+)\)',
        'emit_call': r'emit_call\(([^)]+)\)',
        'emit_ret': r'emit_ret\(([^)]+)\)',
    }
    
    for pattern_name, pattern in patterns.items():
        for match in re.finditer(pattern, debug_output):
            calls[pattern_name].append(match.group(1))
    
    return dict(calls)

def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description='Track backend function calls')
    parser.add_argument('--target', default='aarch64', help='Target ISA')
    parser.add_argument('source', type=Path, help='Source file to compile')
    parser.add_argument('-o', dest='output', default='/tmp/tracked.o', type=Path, help='Output object file')
    args = parser.parse_args()
    
    print(f"Compiling {args.source} for {args.target}...")
    retcode, debug_output = run_compiler_with_tracking(args.target, args.source, args.output)
    
    if retcode != 0:
        print(f"[error] Compilation failed with code {retcode}")
        print(debug_output)
        return retcode
    
    calls = parse_backend_calls(debug_output)
    
    print(f"\n=== Backend Call Summary ===")
    print(f"Target ISA: {args.target}")
    print(f"Functions compiled: {len(set(re.findall(r'function ([^:]+):', debug_output)))}")
    
    for func_name, calls_list in calls.items():
        print(f"\n{func_name}: {len(calls_list)} calls")
        for call in calls_list[:5]:  # Show first 5
            print(f"  - {call}")
        if len(calls_list) > 5:
            print(f"  ... and {len(calls_list) - 5} more")
    
    # Check for x86-specific calls when target is not x86
    if args.target != 'x86_64':
        x86_indicators = ['push', 'pop', 'rbp', 'rsp', 'rax', 'rdi', 'rsi']
        for call in calls.get('emit_prologue', []):
            if any(ind in call.lower() for ind in x86_indicators):
                print(f"\n[warning] Possible x86-specific code detected in prologue: {call}")
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
