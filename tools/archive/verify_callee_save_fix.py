#!/usr/bin/env python3
"""
Callee-Saved Register Fix Verification Tool

This tool verifies that callee-saved registers are properly preserved across
function calls by:
1. Compiling test cases with the zerocc compiler
2. Disassembling the output to check for callee-save patterns
3. Running the compiled code to verify correct results
4. Comparing x86-64 vs AArch64 behavior

Usage:
    python3 tools/verify_callee_save_fix.py [--test-file TEST.c] [--full-test]
"""

import subprocess
import sys
import os
import re
import tempfile
import argparse

# Test cases that exercise callee-saved registers
TEST_CASES = {
    'accumulation': '''
int abs_val(int x) {
    return x < 0 ? -x : x;
}

int main() {
    int acc = 42;
    int c = acc + abs_val(10);
    return c;  // Expected: 52
}
''',
    'simple': '''
int main() {
    return 42;
}
''',
    'nested_calls': '''
int add(int a, int b) {
    return a + b;
}

int multiply(int a, int b) {
    return a * b;
}

int main() {
    int x = add(10, 20);
    int y = multiply(x, 2);
    return y;  // Expected: 60
}
''',
    'callee_saved_usage': '''
// Force use of callee-saved register by having a long-lived variable
int compute(int x) {
    int temp = x * 2;
    return temp + 1;
}

int main() {
    int acc = 100;
    int result = compute(5);
    acc = acc + result;
    return acc;  // Expected: 111
}
'''
}

class CalleeSaveVerifier:
    def __init__(self, target_arch='x86-64', compiler_path='./build/bin/zerocc'):
        self.target_arch = target_arch
        self.compiler_path = compiler_path
        self.test_results = []
        
    def compile_test(self, test_name, source_code):
        """Compile test case and return object file path"""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
            f.write(source_code)
            source_file = f.name
        
        obj_file = source_file.replace('.c', '.o')
        
        try:
            result = subprocess.run(
                [self.compiler_path, source_file, f'--target={self.target_arch}', '-o', obj_file, '-c'],
                capture_output=True,
                text=True
            )
            if result.returncode != 0:
                print(f"[FAIL] {test_name}: Compilation failed")
                print(f"  stderr: {result.stderr}")
                return None
            return obj_file
        except Exception as e:
            print(f"[FAIL] {test_name}: {e}")
            return None
        finally:
            # Clean up source file
            if os.path.exists(source_file):
                os.unlink(source_file)
    
    def disassemble(self, obj_file):
        """Disassemble object file and return assembly text"""
        arch_map = {
            'x86-64': 'x86_64-linux-gnu-objdump',
            'aarch64': 'aarch64-linux-gnu-objdump'
        }
        objdump = arch_map.get(self.target_arch, 'objdump')
        
        try:
            result = subprocess.run(
                [objdump, '-d', obj_file],
                capture_output=True,
                text=True
            )
            return result.stdout
        except Exception as e:
            print(f"[WARN] {self.target_arch}: Could not disassemble: {e}")
            return None
    
    def check_callee_save_pattern(self, disassembly, func_name):
        """
        Check if callee-saved registers are properly preserved in the function.
        
        For x86-64: Check for push/pop of rbx, r12-r15 or mov to/from [rbp-offset]
        For AArch64: Check for stp/ldp of x19-x28 or str/ldr to/from [fp-offset]
        """
        patterns = {
            'x86-64': {
                'save': [
                    r'push\s+%rbx',
                    r'push\s+%r12',
                    r'push\s+%r13',
                    r'push\s+%r14',
                    r'push\s+%r15',
                    r'mov\s+%rbx,-0x[0-9a-f]+\(%rbp\)',
                    r'mov\s+%r12,-0x[0-9a-f]+\(%rbp\)',
                ],
                'restore': [
                    r'pop\s+%rbx',
                    r'pop\s+%r12',
                    r'pop\s+%r13',
                    r'pop\s+%r14',
                    r'pop\s+%r15',
                    r'mov\s+-0x[0-9a-f]+\(%rbp\),%rbx',
                    r'mov\s+-0x[0-9a-f]+\(%rbp\),%r12',
                ]
            },
            'aarch64': {
                'save': [
                    r'stp\s+x[1-2][0-9],\s*x[1-2][0-9],\s*\[sp',
                    r'str\s+x[1-2][0-9],\s*\[x29',
                ],
                'restore': [
                    r'ldp\s+x[1-2][0-9],\s*x[1-2][0-9],\s*\[sp',
                    r'ldr\s+x[1-2][0-9],\s*\[x29',
                ]
            }
        }
        
        arch_patterns = patterns.get(self.target_arch, {})
        
        # Find the function
        func_pattern = rf'<{func_name}>:'
        match = re.search(func_pattern, disassembly)
        if not match:
            print(f"[WARN] {self.target_arch}: Function {func_name} not found in disassembly")
            return None
        
        # Extract function body (until next function or end)
        func_start = match.start()
        func_end = disassembly.find('<', func_start + 1)
        if func_end == -1:
            func_end = len(disassembly)
        func_body = disassembly[func_start:func_end]
        
        # Check for save patterns
        has_save = any(re.search(p, func_body) for p in arch_patterns.get('save', []))
        has_restore = any(re.search(p, func_body) for p in arch_patterns.get('restore', []))
        
        return {
            'has_save': has_save,
            'has_restore': has_restore,
            'save_patterns': arch_patterns.get('save', []),
            'restore_patterns': arch_patterns.get('restore', [])
        }
    
    def run_test(self, obj_file, expected_result=None):
        """Compile and run the object file, return exit code"""
        # Link with static libc
        exe_file = obj_file.replace('.o', '')
        
        # Use appropriate linker
        if self.target_arch == 'x86-64':
            gcc = 'gcc'
        elif self.target_arch == 'aarch64':
            gcc = 'aarch64-linux-gnu-gcc'
        else:
            gcc = 'gcc'
        
        try:
            # Link
            result = subprocess.run(
                [gcc, obj_file, '-static', '-o', exe_file],
                capture_output=True,
                text=True
            )
            if result.returncode != 0:
                print(f"[FAIL] {self.target_arch}: Linking failed")
                print(f"  stderr: {result.stderr}")
                return None
            
            # Run
            if self.target_arch == 'aarch64':
                run_cmd = ['qemu-aarch64-static', exe_file]
            else:
                run_cmd = [exe_file]
            
            result = subprocess.run(
                run_cmd,
                capture_output=True,
                text=True
            )
            return result.returncode
        except Exception as e:
            print(f"[FAIL] {self.target_arch}: {e}")
            return None
        finally:
            # Clean up
            if os.path.exists(exe_file):
                os.unlink(exe_file)
    
    def verify(self, test_name, source_code, expected_result):
        """Run full verification for a test case"""
        print(f"\n{'='*60}")
        print(f"Test: {test_name}")
        print(f"Target: {self.target_arch}")
        print(f"Expected result: {expected_result}")
        print('='*60)
        
        # Compile
        obj_file = self.compile_test(test_name, source_code)
        if not obj_file:
            self.test_results.append({
                'test': test_name,
                'status': 'FAIL',
                'stage': 'compile',
                'message': 'Compilation failed'
            })
            return False
        
        # Disassemble
        disassembly = self.disassemble(obj_file)
        if disassembly:
            # Check for callee-save patterns in functions that use them
            # For accumulation test, check abs_val function
            if 'abs_val' in disassembly:
                pattern_check = self.check_callee_save_pattern(disassembly, 'abs_val')
                if pattern_check:
                    print(f"[INFO] {self.target_arch}: abs_val callee-save analysis:")
                    print(f"  Has save pattern: {pattern_check['has_save']}")
                    print(f"  Has restore pattern: {pattern_check['has_restore']}")
                    
                    if not pattern_check['has_save'] or not pattern_check['has_restore']:
                        print(f"[WARN] {self.target_arch}: abs_val may not preserve callee-saved registers!")
        
        # Run
        result = self.run_test(obj_file, expected_result)
        
        # Clean up
        if os.path.exists(obj_file):
            os.unlink(obj_file)
        
        # Check result
        if result == expected_result:
            print(f"[PASS] {test_name}: Got expected result {result}")
            self.test_results.append({
                'test': test_name,
                'status': 'PASS',
                'result': result,
                'expected': expected_result
            })
            return True
        else:
            print(f"[FAIL] {test_name}: Got {result}, expected {expected_result}")
            self.test_results.append({
                'test': test_name,
                'status': 'FAIL',
                'result': result,
                'expected': expected_result,
                'message': 'Wrong result'
            })
            return False
    
    def run_full_test(self):
        """Run all test cases"""
        print(f"\n{'#'*70}")
        print(f"# Callee-Saved Register Fix Verification")
        print(f"# Target: {self.target_arch}")
        print(f"#{'#'*70}")
        
        results = []
        
        # Test accumulation (the main bug we're fixing)
        results.append(self.verify(
            'accumulation',
            TEST_CASES['accumulation'],
            expected_result=52
        ))
        
        # Test simple
        results.append(self.verify(
            'simple',
            TEST_CASES['simple'],
            expected_result=42
        ))
        
        # Test nested calls
        results.append(self.verify(
            'nested_calls',
            TEST_CASES['nested_calls'],
            expected_result=60
        ))
        
        # Test callee-saved usage
        results.append(self.verify(
            'callee_saved_usage',
            TEST_CASES['callee_saved_usage'],
            expected_result=111
        ))
        
        # Summary
        print(f"\n{'='*70}")
        print("SUMMARY")
        print('='*70)
        passed = sum(1 for r in results if r)
        total = len(results)
        print(f"Passed: {passed}/{total}")
        
        if passed == total:
            print(f"\n[SUCCESS] All tests passed for {self.target_arch}!")
            return True
        else:
            print(f"\n[FAILURE] {total - passed} test(s) failed for {self.target_arch}")
            return False


def main():
    parser = argparse.ArgumentParser(description='Verify callee-saved register fix')
    parser.add_argument('--test-file', help='Test C file to compile and run')
    parser.add_argument('--full-test', action='store_true', help='Run all built-in tests')
    parser.add_argument('--arch', choices=['x86-64', 'aarch64'], default='x86-64',
                       help='Target architecture (default: x86-64)')
    parser.add_argument('--compiler', default='./build/bin/zerocc',
                       help='Path to zerocc compiler (default: ./build/bin/zerocc)')
    
    args = parser.parse_args()
    
    # Change to repository root
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    
    verifier = CalleeSaveVerifier(target_arch=args.arch, compiler_path=args.compiler)
    
    if args.full_test:
        success = verifier.run_full_test()
        sys.exit(0 if success else 1)
    elif args.test_file:
        with open(args.test_file, 'r') as f:
            source = f.read()
        
        # Extract expected result from comment if present
        match = re.search(r'// Expected:\s*(\d+)', source)
        expected = int(match.group(1)) if match else None
        
        if not expected:
            print("[ERROR] No expected result found in test file (add '// Expected: N' comment)")
            sys.exit(1)
        
        success = verifier.verify('custom', source, expected)
        sys.exit(0 if success else 1)
    else:
        # Default: run accumulation test
        print("Running default accumulation test...")
        success = verifier.verify(
            'accumulation',
            TEST_CASES['accumulation'],
            expected_result=52
        )
        sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
