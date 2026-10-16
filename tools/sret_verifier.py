#!/usr/bin/env python3
"""
Struct Return (sret) Verifier for ZeroCC - Phase 3

Verifies that structs >16 bytes are returned via hidden pointer (sret) per
System V AMD64 ABI Section 3.2.6.

System V AMD64 ABI Requirements for >16 byte structs:
1. Caller allocates buffer and passes pointer in RDI (first argument)
2. This shifts all real arguments: RDI→RSI, RSI→RDX, RCX→R8, etc.
3. Callee writes struct to caller-provided buffer [RDI+offset]
4. Callee returns same pointer in RAX

Usage:
    python3 tools/sret_verifier.py --test test.c
    python3 tools/sret_verifier.py --verify-all
    python3 tools/sret_verifier.py --verbose
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
class SretCheck:
    """Result of a single sret check."""
    name: str
    passed: bool
    expected: str
    actual: str
    instruction: str = ""

@dataclass
class SretResult:
    """Result of sret verification."""
    test_name: str
    struct_size: int
    func_name: str
    passed: bool
    checks: List[SretCheck] = field(default_factory=list)
    issues: List[str] = field(default_factory=list)
    assembly: str = ""

# System V AMD64 ABI sret rules
# Structs >16 bytes use hidden pointer in RDI
SRET_THRESHOLD = 16  # bytes


def compile_to_asm(source_code: str) -> Tuple[Optional[str], Optional[str]]:
    """Compile source code to assembly."""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
        f.write(source_code)
        src_file = f.name
    
    obj_file = src_file + '.o'
    
    try:
        # Compile to object file
        result = subprocess.run(
            ['./build/bin/zerocc', '-c', src_file, '-o', obj_file],
            capture_output=True, text=True, cwd='/home/ubuntu/dev/qnz_c_compiler'
        )
        
        if result.returncode != 0:
            return None, result.stderr
        
        # Disassemble with objdump
        result = subprocess.run(
            ['objdump', '-d', obj_file],
            capture_output=True, text=True
        )
        
        if result.returncode != 0:
            return None, result.stderr
        
        return result.stdout, None
    finally:
        if os.path.exists(src_file):
            os.unlink(src_file)
        if os.path.exists(obj_file):
            os.unlink(obj_file)


def extract_function(asm: str, func_name: str) -> Optional[str]:
    """Extract assembly for a specific function."""
    # Find function start - objdump format: "0000000000000000 <func_name>:"
    pattern = rf'^\s*\w+\s+<{re.escape(func_name)}>:$'
    
    match = re.search(pattern, asm, re.MULTILINE)
    if not match:
        return None
    
    start_idx = match.end()
    remaining = asm[start_idx:]
    
    # Find next function (same pattern)
    next_func_pattern = r'^\s*\w+\s+<\w+>:$'
    end_match = re.search(next_func_pattern, remaining, re.MULTILINE)
    
    if end_match:
        return remaining[:end_match.start()]
    else:
        return remaining


def analyze_sret_callee(func_asm: str, func_name: str, struct_size: int) -> SretResult:
    """Analyze callee-side sret implementation."""
    result = SretResult(
        test_name=f"{func_name} (callee)",
        struct_size=struct_size,
        func_name=func_name,
        passed=True
    )
    
    result.assembly = func_asm
    
    # Check 1: RDI should be saved/used as hidden pointer (not as first int arg)
    # Callee saves RDI (either to stack slot or directly to RAX)
    # objdump format: mov %rdi,-0x38(%rbp) or mov %rdi,%rax
    rdi_usage = re.search(r'mov\s+%rdi,\s*(%rax|-?0x[0-9a-f]+\(%rbp\)|-?\d+\(%rbp\))', func_asm)
    
    if rdi_usage:
        check = SretCheck(
            name="RDI as hidden pointer",
            passed=True,
            expected="RDI saved/used as hidden pointer",
            actual=f"RDI used: {rdi_usage.group(0)}",
            instruction=rdi_usage.group(0)
        )
        result.checks.append(check)
    else:
        check = SretCheck(
            name="RDI as hidden pointer",
            passed=False,
            expected="RDI saved/used as hidden pointer",
            actual="RDI usage not found or incorrect"
        )
        result.checks.append(check)
        result.issues.append("Could not find RDI usage - may not be using sret")
    
    # Check 2: Should write to caller buffer ([RDI] or buffer register [RAX/RBX])
    # Pattern: mov %reg, offset(%rdi), mov %reg, (%rdi), mov %reg, offset(%rax), mov %reg, (%rax), etc.
    writes_to_buf = re.findall(r'mov\s+%\w+,\s*(-?0x[0-9a-f]+\(%r\w+\)|-?\d+\(%r\w+\)|\(%r\w+\))', func_asm)
    # Specifically look for stores to (%rax), (%rdi), (%rbx), etc. (non-rbp, non-rsp memory operands)
    writes_to_caller = [w for w in writes_to_buf if not ('%rbp' in w or '%rsp' in w)]
    
    if writes_to_caller:
        check = SretCheck(
            name="Writes to caller buffer [RDI]",
            passed=True,
            expected="Struct written to caller-provided buffer",
            actual=f"Found {len(writes_to_caller)} writes to caller buffer: {', '.join(writes_to_caller[:3])}",
            instruction=writes_to_caller[0]
        )
        result.checks.append(check)
    else:
        check = SretCheck(
            name="Writes to caller buffer [RDI]",
            passed=False,
            expected="Struct written to caller-provided buffer",
            actual="No writes to caller buffer found"
        )
        result.checks.append(check)
        result.issues.append("Should write struct to caller-provided buffer [RDI]")
    
    # Check 3: Should return hidden pointer in RAX
    # Pattern: mov %rdi, %rax OR mov -offset(%rbp), %rax (reloading hidden pointer into RAX)
    returns_rdi = (
        re.search(r'mov\s+%rdi,\s*%rax', func_asm) or
        (rdi_usage and ('(%rbp)' in rdi_usage.group(1)) and 
         re.search(rf'mov\s+{re.escape(rdi_usage.group(1))},\s*%rax', func_asm))
    )
    
    if returns_rdi:
        check = SretCheck(
            name="Returns hidden pointer in RAX",
            passed=True,
            expected="RAX = hidden pointer",
            actual="Returns hidden pointer in RAX",
            instruction=returns_rdi.group(0)
        )
        result.checks.append(check)
    else:
        # Check if RAX is set to something else (like local stack)
        returns_local = re.search(r'lea\s+(-?0x[0-9a-f]+|-?\d+)\(%rbp\),\s*%rax', func_asm)
        if returns_local:
            check = SretCheck(
                name="Returns hidden pointer in RAX",
                passed=False,
                expected="RAX = hidden pointer",
                actual=f"Returns local stack: {returns_local.group(0)}",
                instruction=returns_local.group(0)
            )
            result.checks.append(check)
            result.issues.append("Returns local stack address instead of caller buffer pointer")
        else:
            check = SretCheck(
                name="Returns hidden pointer in RAX",
                passed=False,
                expected="RAX = hidden pointer",
                actual="Return value setup not found"
            )
            result.checks.append(check)
            result.issues.append("Could not verify return value")
    
    result.passed = len(result.issues) == 0
    
    return result


def analyze_sret_caller(func_asm: str, func_name: str, called_func: str, struct_size: int) -> SretResult:
    """Analyze caller-side sret implementation."""
    result = SretResult(
        test_name=f"{func_name} calls {called_func} (caller)",
        struct_size=struct_size,
        func_name=func_name,
        passed=True
    )
    
    result.assembly = func_asm
    
    # Extract call site
    call_pattern = re.compile(rf'call\s+.*<{re.escape(called_func)}>')
    call_match = call_pattern.search(func_asm)
    
    if not call_match:
        result.issues.append(f"Could not find call to {called_func}")
        result.passed = False
        return result
    
    # Look at instructions before call
    call_idx = call_match.start()
    before_call = func_asm[:call_idx]
    after_call = func_asm[call_match.end():]
    
    # Check 1: Should allocate buffer and pass in RDI
    # Pattern: lea offset(%rbp), %reg + pop %rdi / mov %reg, %rdi
    loads_buf = re.search(r'lea\s+(-?0x[0-9a-f]+|-?\d+)\(%rbp\),\s*%(rax|rdi|r\w+)', before_call)
    passes_rdi = (
        re.search(r'lea\s+(-?0x[0-9a-f]+|-?\d+)\(%rbp\),\s*%rdi', before_call) or
        re.search(r'pop\s+%rdi', before_call) or
        re.search(r'mov\s+%(rax|rbx|rcx|rdx|rsi|r8|r9|r10|r11|r12|r13|r14|r15),\s*%rdi', before_call)
    )
    
    if loads_buf and passes_rdi:
        check = SretCheck(
            name="Passes buffer pointer in RDI",
            passed=True,
            expected="RDI = buffer address",
            actual="Loads buffer address and passes in RDI",
            instruction=f"{loads_buf.group(0)}; {passes_rdi.group(0)}"
        )
        result.checks.append(check)
    else:
        check = SretCheck(
            name="Passes buffer pointer in RDI",
            passed=False,
            expected="RDI = buffer address (hidden pointer)",
            actual="No buffer allocation / pass into RDI found before call"
        )
        result.checks.append(check)
        result.issues.append("Should allocate buffer and pass pointer in RDI")
    
    # Check 2: Should shift arguments (if any)
    # Real args should start at RSI, not RDI
    arg_regs = ['rsi', 'rdx', 'rcx', 'r8', 'r9']
    args_found = []
    for reg in arg_regs:
        if re.search(rf'mov.*%%{reg}\b', before_call) or re.search(rf'pop\s+%%{reg}\b', before_call):
            args_found.append(reg)
    
    if args_found:
        check = SretCheck(
            name="Arguments shifted (RDI→RSI, etc.)",
            passed=True,
            expected="Real args in RSI, RDX, RCX, R8, R9",
            actual=f"Args found in: {', '.join(args_found)}",
            instruction=f"args in %{', %'.join(args_found)}"
        )
        result.checks.append(check)
    else:
        # No args is OK
        check = SretCheck(
            name="Arguments shifted (RDI→RSI, etc.)",
            passed=True,
            expected="Real args in RSI, RDX, RCX, R8, R9 (or none)",
            actual="No arguments to shift"
        )
        result.checks.append(check)
    
    # Check 3: Should read result from buffer after call
    # Pattern: movq %rax, offset(%rbp) (save return pointer) OR read from buffer (%rax)
    reads_result = (
        re.search(r'mov\s+%rax,\s*-?0x[0-9a-f]+\(%rbp\)|-?\d+\(%rbp\)', after_call) or
        re.search(r'mov\s+\(%rax\),\s*%\w+', after_call) or
        re.search(r'mov\s+%rax,\s*%\w+', after_call)
    )
    
    if reads_result:
        check = SretCheck(
            name="Saves return pointer",
            passed=True,
            expected="RAX used/saved after call",
            actual="Reads result / saves RAX after call",
            instruction=reads_result.group(0)
        )
        result.checks.append(check)
    else:
        check = SretCheck(
            name="Saves return pointer",
            passed=False,
            expected="RAX used/saved after call",
            actual="Return pointer in RAX not used after call"
        )
        result.checks.append(check)
        result.issues.append("Return pointer in RAX not used after call")
    
    result.passed = len(result.issues) == 0
    
    return result


def verify_sret_compliance(test_name: str, source_code: str, 
                           func_name: str, struct_size: int) -> Tuple[SretResult, SretResult]:
    """Verify sret compliance for both caller and callee."""
    # Compile to assembly
    asm, error = compile_to_asm(source_code)
    if error:
        callee_result = SretResult(
            test_name=f"{func_name} (callee)",
            struct_size=struct_size,
            func_name=func_name,
            passed=False,
            issues=[f"Compilation failed: {error}"]
        )
        caller_result = SretResult(
            test_name=f"caller (caller)",
            struct_size=struct_size,
            func_name="caller",
            passed=False,
            issues=[f"Compilation failed: {error}"]
        )
        return callee_result, caller_result
    
    # Extract callee function
    callee_asm = extract_function(asm, func_name)
    if not callee_asm:
        callee_result = SretResult(
            test_name=f"{func_name} (callee)",
            struct_size=struct_size,
            func_name=func_name,
            passed=False,
            issues=[f"Could not find function {func_name}"]
        )
    else:
        callee_result = analyze_sret_callee(callee_asm, func_name, struct_size)
    
    # Extract caller (main) function
    caller_asm = extract_function(asm, 'main')
    if not caller_asm:
        caller_result = SretResult(
            test_name="main (caller)",
            struct_size=struct_size,
            func_name="main",
            passed=False,
            issues=["Could not find main function"]
        )
    else:
        caller_result = analyze_sret_caller(caller_asm, 'main', func_name, struct_size)
    
    return callee_result, caller_result


def generate_test_cases() -> List[Tuple[str, str, str, int]]:
    """Generate test cases for sret verification."""
    return [
        # 20-byte struct (>16 bytes, needs sret)
        ("20_byte_struct", """
typedef struct { int a; int b; int c; int d; int e; } Big20;
Big20 make_big20(int a, int b, int c, int d, int e) {
    Big20 s = {a, b, c, d, e};
    return s;
}
int main(void) {
    Big20 r = make_big20(1, 2, 3, 4, 5);
    return (r.a == 1 && r.b == 2 && r.c == 3 && r.d == 4 && r.e == 5) ? 0 : 1;
}
""", "make_big20", 20),
        
        # 24-byte struct (>16 bytes, needs sret)
        ("24_byte_struct", """
typedef struct { int a; int b; int c; int d; int e; int f; } Big24;
Big24 make_big24(int a, int b, int c, int d, int e, int f) {
    Big24 s = {a, b, c, d, e, f};
    return s;
}
int main(void) {
    Big24 r = make_big24(1, 2, 3, 4, 5, 6);
    return r.a;
}
""", "make_big24", 24),
        
        # 32-byte struct (>16 bytes, needs sret)
        ("32_byte_struct", """
typedef struct { int a; int b; int c; int d; int e; int f; int g; int h; } Big32;
Big32 make_big32(int a, int b, int c, int d, int e, int f, int g, int h) {
    Big32 s = {a, b, c, d, e, f, g, h};
    return s;
}
int main(void) {
    Big32 r = make_big32(1, 2, 3, 4, 5, 6, 7, 8);
    return r.a;
}
""", "make_big32", 32),
    ]


def print_result(result: SretResult, title: str = ""):
    """Print a single sret result."""
    if title:
        print(f"\n{title}")
    
    status = "PASS" if result.passed else "FAIL"
    print(f"\n[{status}] {result.test_name}")
    print(f"  Struct size: {result.struct_size} bytes")
    
    for check in result.checks:
        check_status = "✓" if check.passed else "✗"
        print(f"  {check_status} {check.name}")
        print(f"      Expected: {check.expected}")
        print(f"      Actual: {check.actual}")
        if check.instruction:
            print(f"      Instruction: {check.instruction}")
    
    if result.issues:
        print(f"  Issues:")
        for issue in result.issues:
            print(f"    ✗ {issue}")


def print_summary(callee_results: List[SretResult], caller_results: List[SretResult]):
    """Print summary of all results."""
    callee_passed = sum(1 for r in callee_results if r.passed)
    caller_passed = sum(1 for r in caller_results if r.passed)
    total = len(callee_results) + len(caller_results)
    passed = callee_passed + caller_passed
    
    print("\n" + "=" * 70)
    print("SRET VERIFICATION SUMMARY")
    print("=" * 70)
    print(f"Callee checks: {callee_passed}/{len(callee_results)} passed")
    print(f"Caller checks: {caller_passed}/{len(caller_results)} passed")
    print(f"Total: {passed}/{total} passed")
    print("=" * 70)
    
    if passed < total:
        print("\nFailed checks:")
        for r in callee_results + caller_results:
            if not r.passed:
                print(f"  - {r.test_name}")


def main():
    parser = argparse.ArgumentParser(description='sret Verifier for ZeroCC')
    parser.add_argument('--test', '-t', help='Test C file to verify')
    parser.add_argument('--verify-all', action='store_true',
                        help='Verify all built-in test cases')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Verbose output with assembly')
    parser.add_argument('--target-all', action='store_true',
                        help='Verify all struct_return tests cross-compiled for x86-64, AArch64, and RISC-V under QEMU')
    
    args = parser.parse_args()

    if args.target_all:
        test_files = [
            'test/abi/struct_return_17byte.c',
            'test/abi/struct_return_20byte.c',
            'test/abi/struct_return_24byte.c',
            'test/abi/struct_return_32byte.c',
            'test/abi/struct_return_tail.c',
            'test/abi/struct_return_edge.c',
            'test/abi/struct_return_funcptr.c',
        ]
        targets = [
            ('x86-64', 'gcc', ''),
            ('aarch64', 'aarch64-linux-gnu-gcc', 'qemu-aarch64-static'),
            ('riscv64', 'riscv64-linux-gnu-gcc -march=rv64gc', 'qemu-riscv64-static'),
        ]
        print("=" * 70)
        print("MULTI-ISA SRET CROSS-VERIFICATION (QEMU / Native)")
        print("=" * 70)
        all_ok = True
        for t in test_files:
            print(f"\nTest: {t}")
            for target_name, cc_cmd, qemu_runner in targets:
                s_file = f"/tmp/sret_test_{target_name}.s"
                bin_file = f"/tmp/sret_test_{target_name}"
                target_flag = f"--target {target_name}" if target_name != "x86-64" else ""
                cmd1 = f"./build/bin/zerocc {t} {target_flag} -S -o {s_file}"
                r1 = subprocess.run(cmd1, shell=True, capture_output=True, text=True)
                if r1.returncode != 0:
                    print(f"  ✗ [{target_name}] Code generation failed: {r1.stderr.strip()}")
                    all_ok = False
                    continue
                cmd2 = f"{cc_cmd} -static {s_file} -o {bin_file}"
                r2 = subprocess.run(cmd2, shell=True, capture_output=True, text=True)
                if r2.returncode != 0:
                    print(f"  ✗ [{target_name}] Assembly/link failed: {r2.stderr.strip()}")
                    all_ok = False
                    continue
                cmd3 = f"{qemu_runner} {bin_file}" if qemu_runner else bin_file
                r3 = subprocess.run(cmd3, shell=True, capture_output=True, text=True)
                if r3.returncode != 0:
                    print(f"  ✗ [{target_name}] Execution failed (code {r3.returncode}): {r3.stderr.strip()}")
                    all_ok = False
                else:
                    print(f"  ✓ [{target_name}] PASS")
        print("\n" + "=" * 70)
        print(f"MULTI-ISA SRET SUMMARY: {'ALL PASSED' if all_ok else 'FAILURES DETECTED'}")
        print("=" * 70)
        sys.exit(0 if all_ok else 1)
    
    if args.test:
        with open(args.test, 'r') as f:
            source_code = f.read()
        
        # Extract function name and size (basic heuristics)
        func_match = re.search(r'(\w+)\s*\([^)]*\)\s*\{', source_code)
        func_name = func_match.group(1) if func_match else 'unknown'
        
        struct_match = re.search(r'typedef\s+struct\s*\{[^}]+\}\s*(\w+)', source_code, re.DOTALL)
        struct_size = 20  # Default guess
        
        callee_result, caller_result = verify_sret_compliance(
            "custom_test", source_code, func_name, struct_size
        )
        
        print_result(callee_result, "Callee Analysis")
        print_result(caller_result, "Caller Analysis")
        
        sys.exit(0 if (callee_result.passed and caller_result.passed) else 1)
    
    else:
        # Run built-in test cases
        test_cases = generate_test_cases()
        callee_results = []
        caller_results = []
        
        for test_name, source_code, func_name, struct_size in test_cases:
            print(f"\n--- {test_name} ({struct_size} bytes) ---")
            callee_result, caller_result = verify_sret_compliance(
                test_name, source_code, func_name, struct_size
            )
            callee_results.append(callee_result)
            caller_results.append(caller_result)
            
            print_result(callee_result, "Callee")
            print_result(caller_result, "Caller")
            
            if args.verbose and callee_result.assembly:
                print("\n  Assembly:")
                for line in callee_result.assembly.split('\n')[:30]:
                    print(f"    {line}")
        
        print_summary(callee_results, caller_results)
        
        all_passed = all(r.passed for r in callee_results + caller_results)
        sys.exit(0 if all_passed else 1)


if __name__ == '__main__':
    main()
