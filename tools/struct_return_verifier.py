#!/usr/bin/env python3
"""
Struct Return Verifier for ZeroCC - Phase 2

Verifies that struct return values follow the System V AMD64 ABI specification:
- 1-8 bytes: returned in RAX
- 9-16 bytes: returned in RAX:RDX
- >16 bytes: passed via hidden pointer (first argument)

Usage:
    python3 tools/struct_return_verifier.py --test test.c
    python3 tools/struct_return_verifier.py --generate-tests
    python3 tools/struct_return_verifier.py --verify-all
"""

import argparse
import subprocess
import sys
import re
import tempfile
import os
from pathlib import Path
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple

@dataclass
class StructReturnSpec:
    """Specification for struct return."""
    size: int
    chunks: int
    expected_regs: List[str]
    uses_hidden_ptr: bool

@dataclass
class ReturnCheck:
    """Result of a return value check."""
    name: str
    passed: bool
    expected: str
    actual: str
    register_used: str = ""

@dataclass
class TestResult:
    """Result of a test case verification."""
    test_name: str
    struct_size: int
    struct_chunks: int
    passed: bool
    checks: List[ReturnCheck] = field(default_factory=list)
    issues: List[str] = field(default_factory=list)
    assembly: str = ""

# System V AMD64 ABI struct return rules
STRUCT_RETURN_RULES = {
    (1, 8): StructReturnSpec(size=8, chunks=1, expected_regs=['rax'], uses_hidden_ptr=False),
    (9, 16): StructReturnSpec(size=16, chunks=2, expected_regs=['rax', 'rdx'], uses_hidden_ptr=False),
    (17, 24): StructReturnSpec(size=24, chunks=3, expected_regs=[], uses_hidden_ptr=True),
    (25, 32): StructReturnSpec(size=32, chunks=4, expected_regs=[], uses_hidden_ptr=True),
}


def get_return_spec(size: int) -> Optional[StructReturnSpec]:
    """Get return specification for struct size."""
    for (min_size, max_size), spec in STRUCT_RETURN_RULES.items():
        if min_size <= size <= max_size:
            return spec
    # Default for very large structs
    return StructReturnSpec(size=size, chunks=(size + 7) // 8, expected_regs=[], uses_hidden_ptr=True)


def compile_to_asm(source_code: str) -> Tuple[Optional[str], Optional[str]]:
    """Compile source code to assembly."""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
        f.write(source_code)
        src_file = f.name
    
    try:
        result = subprocess.run(
            ['./build/bin/zerocc', '-S', src_file, '-o', '-'],
            capture_output=True, text=True, cwd='/home/ubuntu/dev/qnz_c_compiler'
        )
        
        if result.returncode != 0:
            return None, result.stderr
        
        return result.stdout, None
    finally:
        os.unlink(src_file)


def extract_return_function(asm: str, func_name: str) -> Optional[str]:
    """Extract assembly for a specific function."""
    # Find function start
    func_start = asm.find(f'\n{func_name}:')
    if func_start == -1:
        func_start = asm.find(f'\n\t.globl {func_name}\n{func_name}:')
        if func_start == -1:
            return None
    
    # Find next function or end
    next_func = asm.find('\n\t.globl', func_start + 1)
    if next_func == -1:
        next_func = asm.find('\n[0-9]:', func_start + 1)
    
    if next_func == -1:
        return asm[func_start:]
    else:
        return asm[func_start:next_func]


def analyze_return_value(func_asm: str) -> Dict:
    """Analyze how return value is set in function."""
    result = {
        'returns_in_rax': False,
        'returns_in_rdx': False,
        'returns_in_xmm0': False,
        'returns_in_xmm1': False,
        'uses_hidden_ptr': False,
        'hidden_ptr_offset': None,
        'return_instructions': [],
    }
    
    # Look for return value patterns
    lines = func_asm.split('\n')
    
    # Find instructions that set return registers
    for i, line in enumerate(lines):
        line = line.strip()
        
        # Check for mov to return registers
        if re.search(r'movq\s+.*,%rax', line):
            result['returns_in_rax'] = True
            result['return_instructions'].append(line)
        if re.search(r'movq\s+.*,%rdx', line):
            result['returns_in_rdx'] = True
            result['return_instructions'].append(line)
        if re.search(r'movsd\s+.*,%xmm0', line):
            result['returns_in_xmm0'] = True
            result['return_instructions'].append(line)
        if re.search(r'movsd\s+.*,%xmm1', line):
            result['returns_in_xmm1'] = True
            result['return_instructions'].append(line)
        
        # Check for hidden pointer usage (store to [rdi+offset])
        if re.search(r'movq\s+%rax,-?\d*\(%rdi', line):
            result['uses_hidden_ptr'] = True
            match = re.search(r'(%rdi)(?:([-+]\d+))?', line)
            if match and match.group(2):
                result['hidden_ptr_offset'] = match.group(2)
    
    return result


def verify_struct_return(test_name: str, source_code: str, func_name: str, 
                         struct_size: int) -> TestResult:
    """Verify struct return for a specific test case."""
    result = TestResult(
        test_name=test_name,
        struct_size=struct_size,
        struct_chunks=(struct_size + 7) // 8,
        passed=True
    )
    
    # Compile to assembly
    asm, error = compile_to_asm(source_code)
    if error:
        result.issues.append(f"Compilation failed: {error}")
        result.passed = False
        return result
    
    result.assembly = asm
    
    # Get return specification
    spec = get_return_spec(struct_size)
    
    # Extract function assembly
    func_asm = extract_return_function(asm, func_name)
    if not func_asm:
        result.issues.append(f"Could not find function {func_name} in assembly")
        result.passed = False
        return result
    
    # Analyze return value
    analysis = analyze_return_value(func_asm)
    
    # Verify against ABI rules
    if spec.uses_hidden_ptr:
        # Large struct: should use hidden pointer
        check = ReturnCheck(
            name="Hidden pointer usage",
            passed=analysis['uses_hidden_ptr'],
            expected="First argument is hidden pointer",
            actual="Uses hidden pointer" if analysis['uses_hidden_ptr'] else "Does not use hidden pointer"
        )
        result.checks.append(check)
        if not check.passed:
            result.passed = False
            result.issues.append("Large struct should use hidden pointer for return")
    else:
        # Small struct: should return in registers
        for reg in spec.expected_regs:
            if reg == 'rax':
                check = ReturnCheck(
                    name="Return in RAX",
                    passed=analysis['returns_in_rax'],
                    expected="RAX contains return value",
                    actual="RAX used" if analysis['returns_in_rax'] else "RAX not used",
                    register_used="rax"
                )
                result.checks.append(check)
                if not check.passed:
                    result.passed = False
                    result.issues.append("Small struct should return in RAX")
            
            elif reg == 'rdx':
                check = ReturnCheck(
                    name="Return in RDX (second chunk)",
                    passed=analysis['returns_in_rdx'],
                    expected="RDX contains second 8-byte chunk",
                    actual="RDX used" if analysis['returns_in_rdx'] else "RDX not used",
                    register_used="rdx"
                )
                result.checks.append(check)
                if not check.passed:
                    result.passed = False
                    result.issues.append("2-chunk struct should return second chunk in RDX")
    
    return result


def generate_test_cases() -> List[Tuple[str, str, str, int]]:
    """Generate test cases for struct return verification."""
    return [
        # (test_name, source_code, func_name, struct_size)
        
        # 1-chunk structs (1-8 bytes)
        ("1_chunk_int", """
typedef struct { int x; } S1;
S1 make_s1(int x) { return (S1){x}; }
int main() { S1 s = make_s1(42); return s.x; }
""", "make_s1", 4),
        
        ("1_chunk_2int", """
typedef struct { int x; int y; } S2;
S2 make_s2(int x, int y) { return (S2){x, y}; }
int main() { S2 s = make_s2(1, 2); return s.x + s.y; }
""", "make_s2", 8),
        
        # 2-chunk structs (9-16 bytes)
        ("2_chunk_3int", """
typedef struct { int a; int b; int c; } S3;
S3 make_s3(int a, int b, int c) { return (S3){a, b, c}; }
int main() { S3 s = make_s3(1, 2, 3); return s.a + s.b + s.c; }
""", "make_s3", 12),
        
        ("2_chunk_4int", """
typedef struct { int a; int b; int c; int d; } S4;
S4 make_s4(int a, int b, int c, int d) { return (S4){a, b, c, d}; }
int main() { S4 s = make_s4(1, 2, 3, 4); return s.a + s.b + s.c + s.d; }
""", "make_s4", 16),
        
        # 3-chunk structs (>16 bytes, should use hidden pointer)
        ("3_chunk_5int", """
typedef struct { int a; int b; int c; int d; int e; } S5;
S5 make_s5(int a, int b, int c, int d, int e) { 
    return (S5){a, b, c, d, e}; 
}
int main() { S5 s = make_s5(1, 2, 3, 4, 5); return s.a; }
""", "make_s5", 20),
        
        ("3_chunk_6int", """
typedef struct { int a; int b; int c; int d; int e; int f; } S6;
S6 make_s6(int a, int b, int c, int d, int e, int f) { 
    return (S6){a, b, c, d, e, f}; 
}
int main() { S6 s = make_s6(1, 2, 3, 4, 5, 6); return s.a; }
""", "make_s6", 24),
    ]


def print_result(result: TestResult):
    """Print a single test result."""
    status = "PASS" if result.passed else "FAIL"
    print(f"\n[{status}] {result.test_name}")
    print(f"  Struct size: {result.struct_size} bytes ({result.struct_chunks} chunk(s))")
    
    for check in result.checks:
        check_status = "✓" if check.passed else "✗"
        print(f"  {check_status} {check.name}")
        print(f"      Expected: {check.expected}")
        print(f"      Actual: {check.actual}")
    
    if result.issues:
        print(f"  Issues:")
        for issue in result.issues:
            print(f"    - {issue}")


def print_summary(results: List[TestResult]):
    """Print summary of all results."""
    passed = sum(1 for r in results if r.passed)
    failed = len(results) - passed
    
    print("\n" + "=" * 70)
    print("STRUCT RETURN VERIFICATION SUMMARY")
    print("=" * 70)
    print(f"Total: {len(results)} tests")
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")
    print("=" * 70)
    
    if failed > 0:
        print("\nFailed tests:")
        for r in results:
            if not r.passed:
                print(f"  - {r.test_name}")


def main():
    parser = argparse.ArgumentParser(description='Struct Return Verifier for ZeroCC')
    parser.add_argument('--test', '-t', help='Test C file to verify')
    parser.add_argument('--generate-tests', action='store_true', 
                        help='Generate and run built-in test cases')
    parser.add_argument('--verify-all', action='store_true',
                        help='Verify all built-in test cases')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Verbose output')
    
    args = parser.parse_args()
    
    if args.test:
        with open(args.test, 'r') as f:
            source_code = f.read()
        
        # Try to extract function name from source
        func_match = re.search(r'(\w+)\s*\([^)]*\)\s*\{', source_code)
        func_name = func_match.group(1) if func_match else 'main'
        
        # Try to extract struct size (basic heuristic)
        struct_match = re.search(r'typedef\s+struct\s*\{[^}]+\}\s*\w+;\s*(\w+)\s*\(', source_code, re.DOTALL)
        struct_size = 12  # Default guess
        
        result = verify_struct_return("custom_test", source_code, func_name, struct_size)
        print_result(result)
        sys.exit(0 if result.passed else 1)
    
    elif args.generate_tests or args.verify_all:
        test_cases = generate_test_cases()
        results = []
        
        for test_name, source_code, func_name, struct_size in test_cases:
            result = verify_struct_return(test_name, source_code, func_name, struct_size)
            results.append(result)
            
            if args.verbose:
                print_result(result)
        
        if not args.verbose:
            for result in results:
                print_result(result)
        
        print_summary(results)
        sys.exit(0 if all(r.passed for r in results) else 1)
    
    else:
        # Default: run built-in tests
        test_cases = generate_test_cases()
        results = []
        
        for test_name, source_code, func_name, struct_size in test_cases:
            result = verify_struct_return(test_name, source_code, func_name, struct_size)
            results.append(result)
        
        for result in results:
            print_result(result)
        
        print_summary(results)
        sys.exit(0 if all(r.passed for r in results) else 1)


if __name__ == '__main__':
    main()
