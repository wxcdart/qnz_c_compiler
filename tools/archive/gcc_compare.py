#!/usr/bin/env python3
"""
Task 59: GCC Comparison Tool
Directly compares ZeroCC JIT output with GCC output.
"""

import os
import sys
import subprocess
import tempfile
from pathlib import Path

def run_zerocc(source_file, opt_level="-O2", verbose=False):
    """Run ZeroCC JIT on source file."""
    cmd = ['./build/bin/zerocc', opt_level, '-v' if verbose else '', source_file]
    cmd = [c for c in cmd if c]  # Remove empty strings
    
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=10)
        return {
            'success': result.returncode >= 0,  # JIT returns the program's exit code
            'exit_code': result.returncode,
            'stdout': result.stdout.decode('utf-8', errors='replace'),
            'stderr': result.stderr.decode('utf-8', errors='replace'),
        }
    except Exception as e:
        return {'success': False, 'exit_code': -1, 'error': str(e)}

def run_gcc(source_file, opt_level="-O2"):
    """Compile and run with GCC."""
    try:
        # Compile
        compile_result = subprocess.run([
            'gcc', opt_level, source_file, '-o', '/tmp/gcc_compare_test'
        ], capture_output=True, timeout=30)
        
        if compile_result.returncode != 0:
            return {'success': False, 'exit_code': -1, 'error': 'compile'}
        
        # Run
        run_result = subprocess.run(['/tmp/gcc_compare_test'], capture_output=True, timeout=5)
        
        return {
            'success': True,
            'exit_code': run_result.returncode,
            'stdout': run_result.stdout.decode('utf-8', errors='replace'),
            'stderr': run_result.stderr.decode('utf-8', errors='replace'),
        }
    except Exception as e:
        return {'success': False, 'exit_code': -1, 'error': str(e)}

def compare(source_file, opt_level="-O2"):
    """Compare ZeroCC and GCC outputs."""
    print(f"  {Path(source_file).name}...")
    
    zerocc_result = run_zerocc(source_file, opt_level)
    gcc_result = run_gcc(source_file, opt_level)
    
    # Compare exit codes
    if zerocc_result.get('success') and gcc_result.get('success'):
        if zerocc_result['exit_code'] == gcc_result['exit_code']:
            return True, 'Match'
        else:
            return False, f"Exit code mismatch: {zerocc_result['exit_code']} vs {gcc_result['exit_code']}"
    
    return False, 'One or both failed'

def main():
    test_dir = Path('test/bench')
    if not test_dir.exists():
        print(f"Error: {test_dir} not found")
        sys.exit(1)
    
    test_files = sorted(test_dir.glob("*.c"))
    
    print(f"\n{'='*60}")
    print(f"ZeroCC vs GCC Comparison")
    print(f"{'='*60}")
    print(f"Tests: {len(test_files)} files")
    print(f"Optimization level: -O2")
    print(f"{'='*60}\n")
    
    passed = 0
    failed = 0
    
    for source_file in test_files:
        match, message = compare(source_file, "-O2")
        if match:
            print(f"    ✅ {message}")
            passed += 1
        else:
            print(f"    ❌ {message}")
            failed += 1
    
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    print(f"Total: {passed + failed}")
    print(f"Passed: {passed} ✅")
    print(f"Failed: {failed} ❌")
    if passed + failed > 0:
        print(f"Pass rate: {(passed / (passed + failed)) * 100:.1f}%")
    print(f"{'='*60}\n")

if __name__ == '__main__':
    main()
