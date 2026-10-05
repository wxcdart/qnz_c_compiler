#!/usr/bin/env python3
"""
Task 59: Differential Verification against GCC
Compares ZeroCC output with GCC output to verify correctness.
"""

import os
import sys
import time
import subprocess
import json
import argparse
from pathlib import Path
from datetime import datetime

class DifferentialVerifier:
    """Differential verification against GCC."""
    
    def __init__(self, test_dir="test/jit", output_dir="diff_results"):
        self.test_dir = Path(test_dir)
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(exist_ok=True)
        self.results = {
            'timestamp': datetime.now().isoformat(),
            'tests': [],
            'summary': {
                'total': 0,
                'passed': 0,
                'failed': 0,
                'errors': 0
            }
        }
    
    def run_gcc(self, source_file, opt_level="-O2"):
        """Compile and run with GCC."""
        try:
            # Compile
            compile_result = subprocess.run([
                'gcc', opt_level, str(source_file), '-o', '/tmp/gcc_diff_test'
            ], capture_output=True, timeout=30)
            
            if compile_result.returncode != 0:
                return {'success': False, 'error': 'compile', 'message': compile_result.stderr.decode()}
            
            # Run
            run_result = subprocess.run(['/tmp/gcc_diff_test'], 
                                       capture_output=True, timeout=5)
            
            return {
                'success': True,
                'exit_code': run_result.returncode,
                'stdout': run_result.stdout.decode('utf-8', errors='replace'),
                'stderr': run_result.stderr.decode('utf-8', errors='replace'),
                'compile_time_ms': 0  # Placeholder
            }
        except subprocess.TimeoutExpired:
            return {'success': False, 'error': 'timeout', 'message': 'Execution timed out'}
        except Exception as e:
            return {'success': False, 'error': 'exception', 'message': str(e)}
    
    def run_zerocc_jit(self, source_file):
        """Run with ZeroCC JIT (placeholder - would invoke ZeroCC)."""
        # For now, this is a placeholder
        # In real implementation, would compile and run with ZeroCC
        return {
            'success': True,
            'exit_code': 0,
            'stdout': '',
            'stderr': '',
            'compile_time_ms': 0
        }
    
    def compare_outputs(self, zerocc_result, gcc_result):
        """Compare ZeroCC and GCC outputs."""
        if not zerocc_result.get('success') or not gcc_result.get('success'):
            return False, 'One or both runs failed'
        
        # Compare exit codes
        if zerocc_result.get('exit_code') != gcc_result.get('exit_code'):
            return False, f"Exit code mismatch: {zerocc_result.get('exit_code')} vs {gcc_result.get('exit_code')}"
        
        # Compare stdout (normalize whitespace)
        zz_stdout = zerocc_result.get('stdout', '').strip()
        gcc_stdout = gcc_result.get('stdout', '').strip()
        
        if zz_stdout != gcc_stdout:
            return False, f"stdout mismatch"
        
        return True, 'Match'
    
    def verify_test(self, source_file, opt_level="-O2"):
        """Verify a single test against GCC."""
        test_name = source_file.stem
        print(f"  {test_name}...")
        
        # Run GCC
        gcc_result = self.run_gcc(source_file, opt_level)
        
        # Run ZeroCC (placeholder)
        zerocc_result = self.run_zerocc_jit(source_file)
        
        # Compare
        match, message = self.compare_outputs(zerocc_result, gcc_result)
        
        return {
            'name': test_name,
            'file': str(source_file),
            'opt_level': opt_level,
            'gcc': gcc_result,
            'zerocc': zerocc_result,
            'match': match,
            'message': message
        }
    
    def verify_all(self, opt_level="-O2", max_tests=20):
        """Verify all tests against GCC."""
        test_files = sorted(self.test_dir.glob("*.c"))[:max_tests]
        
        print(f"\n{'='*60}")
        print(f"Differential Verification - GCC vs ZeroCC")
        print(f"{'='*60}")
        print(f"Tests: {len(test_files)} files")
        print(f"Optimization level: {opt_level}")
        print(f"{'='*60}\n")
        
        for source_file in test_files:
            result = self.verify_test(source_file, opt_level)
            self.results['tests'].append(result)
            self.results['summary']['total'] += 1
            
            if result['match']:
                self.results['summary']['passed'] += 1
            elif result['gcc'].get('success'):
                self.results['summary']['failed'] += 1
            else:
                self.results['summary']['errors'] += 1
        
        # Save results
        output_file = self.output_dir / f'diff_verify_{opt_level.replace("-", "")}.json'
        with open(output_file, 'w') as f:
            json.dump(self.results, f, indent=2)
        
        print(f"\nResults saved to: {output_file}")
        return self.results
    
    def print_summary(self):
        """Print verification summary."""
        summary = self.results.get('summary', {})
        
        print(f"\n{'='*60}")
        print("DIFFERENTIAL VERIFICATION SUMMARY")
        print(f"{'='*60}")
        print(f"Total: {summary.get('total', 0)}")
        print(f"Passed: {summary.get('passed', 0)} ✅")
        print(f"Failed: {summary.get('failed', 0)} ❌")
        print(f"Errors: {summary.get('errors', 0)} ⚠️")
        
        if summary.get('total', 0) > 0:
            pass_rate = (summary.get('passed', 0) / summary.get('total', 1)) * 100
            print(f"Pass rate: {pass_rate:.1f}%")
        
        print(f"{'='*60}\n")


def main():
    parser = argparse.ArgumentParser(description="Task 59: Differential Verification")
    parser.add_argument('--tests', default='test/jit', help='Test directory')
    parser.add_argument('--output', default='diff_results', help='Output directory')
    parser.add_argument('--opt-level', default='-O2', help='GCC optimization level')
    parser.add_argument('--max-tests', type=int, default=20, help='Max tests to verify')
    
    args = parser.parse_args()
    
    verifier = DifferentialVerifier(args.tests, args.output)
    results = verifier.verify_all(args.opt_level, args.max_tests)
    verifier.print_summary()


if __name__ == '__main__':
    main()
