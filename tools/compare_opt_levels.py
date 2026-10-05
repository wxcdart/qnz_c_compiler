#!/usr/bin/env python3
"""
Task 59: Optimization Level Comparison
Compares how different optimization levels affect ZeroCC and GCC.
"""

import os
import sys
import subprocess
from pathlib import Path

def run_zerocc(source_file, opt_level):
    """Run ZeroCC and capture output."""
    try:
        result = subprocess.run(
            ['./build/bin/zerocc', opt_level, source_file],
            capture_output=True, timeout=10
        )
        return {
            'exit_code': result.returncode,
            'stdout': result.stdout.decode('utf-8', errors='replace'),
            'stderr': result.stderr.decode('utf-8', errors='replace'),
        }
    except Exception as e:
        return {'exit_code': -1, 'error': str(e)}

def run_gcc(source_file, opt_level):
    """Compile and run with GCC."""
    try:
        # Compile
        compile_result = subprocess.run(
            ['gcc', opt_level, source_file, '-o', '/tmp/compare_test'],
            capture_output=True, timeout=30
        )
        if compile_result.returncode != 0:
            return {'exit_code': -1, 'error': 'compile'}
        
        # Run
        run_result = subprocess.run(['/tmp/compare_test'], capture_output=True, timeout=5)
        return {
            'exit_code': run_result.returncode,
            'stdout': run_result.stdout.decode('utf-8', errors='replace'),
            'stderr': run_result.stderr.decode('utf-8', errors='replace'),
        }
    except Exception as e:
        return {'exit_code': -1, 'error': str(e)}

def compare_opt_levels(source_file):
    """Compare all optimization levels."""
    filename = Path(source_file).name
    print(f"\n{'='*60}")
    print(f"Optimization Level Comparison: {filename}")
    print(f"{'='*60}")
    
    opt_levels = ['-O0', '-O1', '-O2', '-O3']
    
    print(f"\n{'Level':<8} {'ZeroCC':<12} {'GCC':<12} {'Match':<8}")
    print("-" * 60)
    
    for opt in opt_levels:
        zerocc = run_zerocc(source_file, opt)
        gcc = run_gcc(source_file, opt)
        
        z_code = zerocc.get('exit_code', -1)
        g_code = gcc.get('exit_code', -1)
        
        match = "✅" if z_code == g_code and z_code >= 0 else "❌"
        
        print(f"{opt:<8} {z_code:<12} {g_code:<12} {match:<8}")
    
    # Summary
    print(f"\nSummary:")
    print("-" * 60)
    matches = 0
    for opt in opt_levels:
        z = run_zerocc(source_file, opt)
        g = run_gcc(source_file, opt)
        if z.get('exit_code', -1) == g.get('exit_code', -1) and z.get('exit_code', -1) >= 0:
            matches += 1
    
    print(f"Matching optimization levels: {matches}/{len(opt_levels)}")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 tools/compare_opt_levels.py <test_file>")
        print("\nExamples:")
        print("  python3 tools/compare_opt_levels.py test/bench/arith_heavy.c")
        print("  python3 tools/compare_opt_levels.py test/bench/loop_heavy.c")
        sys.exit(1)
    
    source_file = sys.argv[1]
    if not os.path.exists(source_file):
        print(f"Error: {source_file} not found")
        sys.exit(1)
    
    compare_opt_levels(source_file)

if __name__ == '__main__':
    main()
