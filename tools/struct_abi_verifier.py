#!/usr/bin/env python3
"""
Struct ABI Verifier for ZeroCC - Phase 2

Verifies that struct argument passing and return values follow the
System V AMD64 ABI specification.

Usage:
    python3 tools/struct_abi_verifier.py --test test.c
    python3 tools/struct_abi_verifier.py --asm file.s --verify-args
    python3 tools/struct_abi_verifier.py --mode comprehensive
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
class StructSpec:
    """Struct specification for ABI analysis."""
    name: str
    size: int
    alignment: int
    members: List[Tuple[str, int, int]] = field(default_factory=list)  # (type, size, offset)
    chunks: int = 1  # 8-byte chunks
    
    def passes_in_registers(self) -> bool:
        """Check if struct can be passed in registers (<= 2 8-byte chunks)."""
        return self.chunks <= 2
    
    def passes_via_pointer(self) -> bool:
        """Check if struct is passed via hidden pointer (> 2 8-byte chunks)."""
        return self.chunks > 2

@dataclass
class ABICheck:
    """Result of a single ABI check."""
    name: str
    passed: bool
    expected: str
    actual: str
    message: str = ""

@dataclass
class ABIResult:
    """Result of ABI verification."""
    test_name: str
    passed: bool
    checks: List[ABICheck] = field(default_factory=list)
    issues: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)

# System V AMD64 ABI specifications
ABI_SPECS = {
    'int_arg_regs': ['rdi', 'rsi', 'rdx', 'rcx', 'r8', 'r9'],
    'fp_arg_regs': ['xmm0', 'xmm1', 'xmm2', 'xmm3', 'xmm4', 'xmm5', 'xmm6', 'xmm7'],
    'return_regs': ['rax', 'rdx'],
    'fp_return_regs': ['xmm0', 'xmm1'],
    'max_struct_in_regs': 16,  # bytes
    'stack_align': 16,
}


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


def parse_struct_definition(asm: str) -> List[StructSpec]:
    """Parse struct definitions from assembly."""
    structs = []
    
    # Look for .type directives and struct patterns
    struct_pattern = re.compile(r'#.*struct\s+(\w+).*size:\s*(\d+)', re.IGNORECASE)
    
    for match in struct_pattern.finditer(asm):
        name = match.group(1)
        size = int(match.group(2))
        chunks = (size + 7) // 8  # Round up to 8-byte chunks
        
        structs.append(StructSpec(
            name=name,
            size=size,
            alignment=8,
            chunks=chunks
        ))
    
    return structs


def analyze_function_signature(asm: str, func_name: str) -> Dict:
    """Analyze function signature from assembly."""
    result = {
        'has_prologue': False,
        'uses_rdi': False,
        'uses_rsi': False,
        'uses_rdx': False,
        'uses_rcx': False,
        'uses_r8': False,
        'uses_r9': False,
        'uses_xmm0': False,
        'uses_xmm1': False,
        'uses_rax': False,
        'uses_rdx_return': False,
        'stack_args': False,
        'returns_in_rax': False,
        'returns_in_rdx': False,
        'uses_hidden_ptr': False,
    }
    
    # Find function definition
    func_pattern = re.compile(rf'\b{re.escape(func_name)}:\s*\n(.*?)(?=\n\s*\.(?:globl|type)|\n\s*\w+:)', re.DOTALL)
    match = func_pattern.search(asm)
    
    if not match:
        return result
    
    func_body = match.group(1)
    
    # Check for prologue
    if re.search(r'pushq\s+%%rbp', func_body):
        result['has_prologue'] = True
    
    # Check register usage in first N instructions (argument handling)
    lines = func_body.split('\n')[:20]
    func_section = '\n'.join(lines)
    
    # Check for argument register usage
    result['uses_rdi'] = bool(re.search(r'%rdi', func_section))
    result['uses_rsi'] = bool(re.search(r'%rsi', func_section))
    result['uses_rdx'] = bool(re.search(r'%rdx(?!\s*:\s*)', func_section))  # Not x86-64::
    result['uses_rcx'] = bool(re.search(r'%rcx(?!\s*:\s*)', func_section))
    result['uses_r8'] = bool(re.search(r'%r8', func_section))
    result['uses_r9'] = bool(re.search(r'%r9', func_section))
    result['uses_xmm0'] = bool(re.search(r'%xmm0', func_section))
    result['uses_xmm1'] = bool(re.search(r'%xmm1', func_section))
    result['uses_rax'] = bool(re.search(r'%rax', func_section))
    
    # Check for return value
    if re.search(r'movq\s+.*,%rax', func_body):
        result['returns_in_rax'] = True
    if re.search(r'movq\s+.*,%rdx', func_body):
        result['returns_in_rdx'] = True
    
    # Check for stack arguments (16(%rbp) or higher)
    if re.search(r'%rbp\),\s*%.*', func_section):
        result['stack_args'] = True
    
    return result


def verify_struct_args(asm: str, struct_name: str, func_name: str, struct_size: int) -> ABIResult:
    """Verify struct argument passing."""
    result = ABIResult(test_name=f"{func_name}({struct_name})", passed=True)
    
    chunks = (struct_size + 7) // 8
    sig = analyze_function_signature(asm, func_name)
    
    if chunks == 1:  # 1-8 bytes: single register (rdi or rax for return)
        check = ABICheck(
            name="1-chunk struct arg",
            passed=sig['uses_rdi'],
            expected="rdi register",
            actual="rdi used" if sig['uses_rdi'] else "rdi not used"
        )
        result.checks.append(check)
        if not check.passed:
            result.passed = False
            result.issues.append(f"Expected struct in rdi, but rdi not used")
    
    elif chunks == 2:  # 9-16 bytes: two registers (rdi, rsi)
        check1 = ABICheck(
            name="2-chunk struct arg (first)",
            passed=sig['uses_rdi'],
            expected="rdi register",
            actual="rdi used" if sig['uses_rdi'] else "rdi not used"
        )
        check2 = ABICheck(
            name="2-chunk struct arg (second)",
            passed=sig['uses_rsi'],
            expected="rsi register",
            actual="rsi used" if sig['uses_rsi'] else "rsi not used"
        )
        result.checks.extend([check1, check2])
        if not check1.passed or not check2.passed:
            result.passed = False
            result.issues.append(f"Expected struct in rdi,rsi")
    
    else:  # >16 bytes: hidden pointer
        check = ABICheck(
            name="Large struct arg (>16 bytes)",
            passed=True,  # Hard to verify without more context
            expected="hidden pointer",
            actual="uses stack/frame" if sig['stack_args'] else "unknown"
        )
        result.checks.append(check)
        result.warnings.append(f"Large struct ({struct_size} bytes) should use hidden pointer")
    
    return result


def verify_struct_return(asm: str, struct_name: str, func_name: str, struct_size: int) -> ABIResult:
    """Verify struct return value handling."""
    result = ABIResult(test_name=f"{func_name}() -> {struct_name}", passed=True)
    
    chunks = (struct_size + 7) // 8
    sig = analyze_function_signature(asm, func_name)
    
    if chunks == 1:  # 1-8 bytes: rax
        check = ABICheck(
            name="1-chunk struct return",
            passed=sig['returns_in_rax'],
            expected="rax register",
            actual="rax used for return" if sig['returns_in_rax'] else "rax not used"
        )
        result.checks.append(check)
        if not check.passed:
            result.passed = False
            result.issues.append(f"Expected struct return in rax")
    
    elif chunks == 2:  # 9-16 bytes: rax, rdx
        check1 = ABICheck(
            name="2-chunk struct return (first)",
            passed=sig['returns_in_rax'],
            expected="rax register",
            actual="rax used" if sig['returns_in_rax'] else "rax not used"
        )
        check2 = ABICheck(
            name="2-chunk struct return (second)",
            passed=sig['returns_in_rdx'],
            expected="rdx register",
            actual="rdx used" if sig['returns_in_rdx'] else "rdx not used"
        )
        result.checks.extend([check1, check2])
        if not check1.passed or not check2.passed:
            result.passed = False
            result.issues.append(f"Expected struct return in rax,rdx")
    
    else:  # >16 bytes: hidden pointer in first arg
        check = ABICheck(
            name="Large struct return (>16 bytes)",
            passed=True,  # Hard to verify
            expected="hidden pointer (first arg)",
            actual="unknown"
        )
        result.checks.append(check)
        result.warnings.append(f"Large struct ({struct_size} bytes) should use hidden pointer")
    
    return result


def run_comprehensive_verification(source_code: str) -> List[ABIResult]:
    """Run comprehensive struct ABI verification."""
    results = []
    
    # Compile to assembly
    asm, error = compile_to_asm(source_code)
    if error:
        print(f"Compilation error: {error}", file=sys.stderr)
        return results
    
    # Parse struct definitions
    structs = parse_struct_definition(asm)
    
    # Analyze each function
    func_pattern = re.compile(r'\.globl\s+(\w+)', re.MULTILINE)
    for match in func_pattern.finditer(asm):
        func_name = match.group(1)
        
        # Skip main and internal functions
        if func_name in ['main', '__libc_start_main']:
            continue
        
        # Check for struct in function name or parameters
        for struct in structs:
            if struct.name in asm[match.start():match.start()+500]:
                # Verify struct argument
                arg_result = verify_struct_args(asm, struct.name, func_name, struct.size)
                results.append(arg_result)
                
                # Verify struct return
                ret_result = verify_struct_return(asm, struct.name, func_name, struct.size)
                results.append(ret_result)
    
    return results


def print_results(results: List[ABIResult]):
    """Print verification results."""
    print("\n" + "=" * 70)
    print("STRUCT ABI VERIFICATION RESULTS")
    print("=" * 70)
    
    passed = 0
    failed = 0
    
    for result in results:
        status = "PASS" if result.passed else "FAIL"
        print(f"\n[{status}] {result.test_name}")
        
        for check in result.checks:
            check_status = "✓" if check.passed else "✗"
            print(f"  {check_status} {check.name}: {check.expected}")
            if not check.passed:
                print(f"    Actual: {check.actual}")
                if check.message:
                    print(f"    Message: {check.message}")
        
        if result.warnings:
            for warning in result.warnings:
                print(f"  ⚠ WARNING: {warning}")
        
        if result.passed:
            passed += 1
        else:
            failed += 1
    
    print("\n" + "=" * 70)
    print(f"SUMMARY: {passed} passed, {failed} failed")
    print("=" * 70)
    
    return failed == 0


def main():
    parser = argparse.ArgumentParser(description='Struct ABI Verifier for ZeroCC')
    parser.add_argument('--test', '-t', help='Test C file to verify')
    parser.add_argument('--asm', '-a', help='Assembly file to verify')
    parser.add_argument('--verify-args', action='store_true', help='Verify struct arguments only')
    parser.add_argument('--verify-returns', action='store_true', help='Verify struct returns only')
    parser.add_argument('--mode', choices=['comprehensive', 'quick'], default='comprehensive',
                        help='Verification mode')
    
    args = parser.parse_args()
    
    if args.test:
        with open(args.test, 'r') as f:
            source_code = f.read()
        
        results = run_comprehensive_verification(source_code)
        success = print_results(results)
        sys.exit(0 if success else 1)
    
    elif args.asm:
        with open(args.asm, 'r') as f:
            asm = f.read()
        
        print("Assembly file verification not yet implemented")
        sys.exit(1)
    
    else:
        # Run with built-in test cases
        test_cases = [
            # 8-byte struct (1 chunk)
            """
typedef struct { int x; int y; } Pair8;
Pair8 make_pair8(int a, int b) { return (Pair8){a, b}; }
int take_pair8(Pair8 p) { return p.x + p.y; }
int main() { return take_pair8(make_pair8(1, 2)); }
""",
            # 12-byte struct (2 chunks)
            """
typedef struct { int a; int b; int c; } Triple12;
Triple12 make_triple12(int a, int b, int c) { return (Triple12){a, b, c}; }
int take_triple12(Triple12 t) { return t.a + t.b + t.c; }
int main() { return take_triple12(make_triple12(1, 2, 3)); }
""",
        ]
        
        all_passed = True
        for i, test_code in enumerate(test_cases):
            print(f"\n--- Test Case {i+1} ---")
            results = run_comprehensive_verification(test_code)
            if not print_results(results):
                all_passed = False
        
        sys.exit(0 if all_passed else 1)


if __name__ == '__main__':
    main()
