#!/usr/bin/env python3
"""Compiler Crash Analyzer - Systematic analysis of compiler crashes.

Runs the compiler with various inputs and configurations to identify crash patterns.

Usage:
    python3 tools/compiler_crash_analyzer.py --isa aarch64 test/aot/
"""

import subprocess
import sys
import signal
from pathlib import Path
from dataclasses import dataclass
from typing import Optional
from collections import defaultdict

@dataclass
class CrashInfo:
    source_file: str
    exit_code: int
    signal: Optional[int]
    stderr: str
    duration: float

def test_compilation(source: Path, target: str, timeout: int = 30) -> CrashInfo:
    """Test compilation of a single source file."""
    obj_path = Path(f"/tmp/crash_test_{source.stem}.o")
    
    cmd = [
        "./build/bin/zerocc",
        "--target", target,
        "-c", str(source),
        "-o", str(obj_path)
    ]
    
    import time
    start = time.time()
    
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout
        )
        duration = time.time() - start
        
        # Determine signal if crashed
        sig = None
        if result.returncode < 0:
            sig = -result.returncode
        
        return CrashInfo(
            source_file=source.name,
            exit_code=result.returncode,
            signal=sig,
            stderr=result.stderr,
            duration=duration
        )
    except subprocess.TimeoutExpired:
        return CrashInfo(
            source_file=source.name,
            exit_code=-1,
            signal=signal.SIGKILL,
            stderr="TIMEOUT",
            duration=timeout
        )

def analyze_crash_patterns(results: list[CrashInfo]) -> dict:
    """Analyze patterns in crash data."""
    patterns = {
        'total': len(results),
        'crashes': 0,
        'successes': 0,
        'timeouts': 0,
        'signals': defaultdict(int),
        'error_messages': defaultdict(int),
    }
    
    for r in results:
        if r.exit_code == 0:
            patterns['successes'] += 1
        elif r.signal:
            patterns['crashes'] += 1
            patterns['signals'][r.signal] += 1
        elif 'TIMEOUT' in r.stderr:
            patterns['timeouts'] += 1
        else:
            patterns['crashes'] += 1
            # Extract error message
            msg = r.stderr.split('\n')[0][:100]
            patterns['error_messages'][msg] += 1
    
    return patterns

def print_report(results: list[CrashInfo], patterns: dict, target: str) -> int:
    """Print analysis report."""
    print(f"\n=== Compiler Crash Analysis for {target.upper()} ===")
    print(f"Total files tested: {patterns['total']}")
    print(f"Successful compilations: {patterns['successes']}")
    print(f"Crashes: {patterns['crashes']}")
    print(f"Timeouts: {patterns['timeouts']}")
    print()
    
    if patterns['signals']:
        print("Crash signals:")
        sig_names = {
            11: 'SIGSEGV (Segmentation fault)',
            6: 'SIGABRT (Abort)',
            8: 'SIGFPE (Floating point exception)',
            4: 'SIGILL (Illegal instruction)',
        }
        for sig, count in patterns['signals'].items():
            name = sig_names.get(sig, f'Signal {sig}')
            print(f"  {name}: {count} times")
        print()
    
    if patterns['error_messages']:
        print("Common error messages:")
        for msg, count in sorted(patterns['error_messages'].items(), key=lambda x: -x[1])[:5]:
            print(f"  [{count}x] {msg}")
        print()
    
    # Show crash details
    crashes = [r for r in results if r.exit_code != 0 and r.exit_code != -1]
    if crashes:
        print("Crash details:")
        for r in crashes[:5]:
            print(f"  {r.source_file}: exit={r.exit_code}, signal={r.signal}")
            if r.stderr and len(r.stderr) < 200:
                print(f"    {r.stderr.strip()}")
    
    return 1 if patterns['crashes'] > 0 else 0

def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description='Analyze compiler crashes')
    parser.add_argument('--isa', required=True, choices=['x86_64', 'aarch64', 'riscv64'],
                       help='Target ISA')
    parser.add_argument('test_dir', type=Path, nargs='?', default=Path("test/aot"),
                       help='Directory containing test files')
    parser.add_argument('--timeout', type=int, default=30,
                       help='Timeout per test in seconds')
    args = parser.parse_args()
    
    if not args.test_dir.exists():
        print(f"[error] Test directory not found: {args.test_dir}")
        return 1
    
    print(f"Testing {args.isa} compilation...")
    print(f"Test directory: {args.test_dir}")
    print()
    
    # Find all test files
    test_files = list(args.test_dir.rglob("*.c"))
    print(f"Found {len(test_files)} test files")
    
    results = []
    for source in test_files:
        print(f"Testing {source.name}...", end=" ")
        result = test_compilation(source, args.isa, args.timeout)
        results.append(result)
        
        if result.exit_code == 0:
            print("✓")
        elif result.signal:
            print(f"✗ SIG{result.signal}")
        elif result.exit_code == -1:
            print("⏰ TIMEOUT")
        else:
            print(f"✗ exit {result.exit_code}")
    
    patterns = analyze_crash_patterns(results)
    return print_report(results, patterns, args.isa)

if __name__ == '__main__':
    sys.exit(main())
