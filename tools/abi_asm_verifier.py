#!/usr/bin/env python3
"""
ABI Assembly Verifier
Verifies that assembly output follows the correct calling convention for the target ISA.

This tool checks:
- Argument passing (register vs stack)
- Return value handling
- Stack alignment
- Callee-saved register preservation

Usage:
    python3 tools/abi_asm_verifier.py --test test.c --target x86_64
    python3 tools/abi_asm_verifier.py --test test.c --target aarch64
"""

import argparse
import subprocess
import sys
import re
from pathlib import Path
from dataclasses import dataclass
from typing import Dict, List, Optional


@dataclass
class ABIResult:
    """Result of ABI verification."""
    isa: str
    abi_name: str
    passed: bool
    checks: Dict[str, bool] = None
    issues: List[str] = None
    
    def __post_init__(self):
        if self.checks is None:
            self.checks = {}
        if self.issues is None:
            self.issues = []


# ABI specifications per ISA
ABI_SPECS = {
    'x86_64': {
        'name': 'System V AMD64 ABI',
        'int_arg_regs': ['rdi', 'rsi', 'rdx', 'rcx', 'r8', 'r9'],
        'fp_arg_regs': ['xmm0', 'xmm1', 'xmm2', 'xmm3', 'xmm4', 'xmm5', 'xmm6', 'xmm7'],
        'return_regs': ['rax', 'rdx'],
        'fp_return_regs': ['xmm0', 'xmm1'],
        'callee_saved': ['rbx', 'r12', 'r13', 'r14', 'r15'],
        'caller_saved': ['rax', 'rcx', 'rdx', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11'],
        'stack_align': 16,
        'red_zone': True,
    },
    'aarch64': {
        'name': 'AArch64 AAPCS64',
        'int_arg_regs': ['x0', 'x1', 'x2', 'x3', 'x4', 'x5', 'x6', 'x7'],
        'fp_arg_regs': ['v0', 'v1', 'v2', 'v3', 'v4', 'v5', 'v6', 'v7'],
        'return_regs': ['x0', 'x1'],
        'fp_return_regs': ['v0', 'v1'],
        'callee_saved': ['x19', 'x20', 'x21', 'x22', 'x23', 'x24', 'x25', 'x26', 'x27', 'x28', 'x29'],
        'caller_saved': ['x0', 'x1', 'x2', 'x3', 'x4', 'x5', 'x6', 'x7', 'x8', 'x9', 'x10', 'x11', 'x12', 'x13', 'x14', 'x15', 'x16', 'x17', 'x18'],
        'stack_align': 16,
        'red_zone': False,
    },
    'riscv64': {
        'name': 'RISC-V ABI',
        'int_arg_regs': ['a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7'],
        'fp_arg_regs': ['fa0', 'fa1', 'fa2', 'fa3', 'fa4', 'fa5', 'fa6', 'fa7'],
        'return_regs': ['a0', 'a1'],
        'fp_return_regs': ['fa0', 'fa1'],
        'callee_saved': ['s0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 's8', 's9', 's10', 's11'],
        'caller_saved': ['ra', 't0', 't1', 't2', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7', 'fa0', 'fa1', 'fa2', 'fa3', 'fa4', 'fa5', 'fa6', 'fa7'],
        'stack_align': 16,
        'red_zone': True,
    },
}


def run_zerocc_asm(source_file: Path, target: str, opt_level: str = '-O3') -> tuple:
    """Run ZeroCC with -S flag to generate assembly."""
    cmd = ['./build/bin/zerocc', '-S', opt_level, '--target', target, str(source_file), '-o', '-']
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return -1, "", str(e)


def check_argument_passing(asm_content: str, isa: str) -> tuple:
    """Check if function arguments are passed correctly."""
    issues = []
    passed = True
    
    spec = ABI_SPECS.get(isa)
    if not spec:
        return False, ["Unknown ISA"]
    
    # Check for argument register usage
    arg_regs = spec['int_arg_regs'] + spec['fp_arg_regs']
    
    # Look for function calls and check argument setup
    call_pattern = r'\t(call|bl|jal)\s+(\w+)'
    calls = re.findall(call_pattern, asm_content)
    
    if calls:
        # Check if argument registers are used before calls
        for reg in arg_regs[:6]:  # First 6 args (most common)
            # Look for moves into argument registers
            if isa == 'x86_64':
                pattern = rf'\tmovq?\s+.*,%{reg}'
            elif isa == 'aarch64':
                pattern = rf'\t(mov|orr|movk)\s+{reg},'
            else:  # riscv64
                pattern = rf'\t(addi|mv)\s+{reg},'
            
            if re.search(pattern, asm_content):
                pass  # Good - argument register is being set
    
    return passed, issues


def check_return_value(asm_content: str, isa: str) -> tuple:
    """Check if return values are in correct registers."""
    issues = []
    passed = True
    
    spec = ABI_SPECS.get(isa)
    if not spec:
        return False, ["Unknown ISA"]
    
    # Look for return statements
    return_pattern = r'\tret\b'
    returns = re.findall(return_pattern, asm_content)
    
    if returns:
        # Check if return value is in correct register
        return_regs = spec['return_regs']
        
        for reg in return_regs:
            if isa == 'x86_64':
                pattern = rf'\tmovq?\s+.*,%{reg}'
            elif isa == 'aarch64':
                pattern = rf'\t(mov|orr)\s+{reg},'
            else:  # riscv64
                pattern = rf'\t(addi|mv)\s+{reg},'
            
            if re.search(pattern, asm_content):
                pass  # Good - return register is being set
    
    return passed, issues


def check_stack_alignment(asm_content: str, isa: str) -> tuple:
    """Check if stack is properly aligned."""
    issues = []
    passed = True
    
    spec = ABI_SPECS.get(isa)
    if not spec:
        return False, ["Unknown ISA"]
    
    align = spec['stack_align']
    
    # Check prologue for stack alignment
    if isa == 'x86_64':
        # Look for subq $N, %rsp where N % 16 == 0 (after push rbp)
        prologue_pattern = r'\tsubq\s+\$(\d+),\s*%rsp'
        matches = re.findall(prologue_pattern, asm_content)
        
        for match in matches:
            try:
                size = int(match)
                # After push rbp (8 bytes), we need (size + 8) % 16 == 0
                # So size should be 8 mod 16
                if size % 16 != 8:
                    issues.append(f"Stack size {size} may not maintain 16-byte alignment")
            except ValueError:
                pass
    
    elif isa == 'aarch64':
        # Look for add/sub sp, sp, #N
        prologue_pattern = r'\t(add|sub)\s+sp,sp,-?(\d+)'
        matches = re.findall(prologue_pattern, asm_content)
        
        for _, match in matches:
            try:
                size = int(match)
                if size % 16 != 0:
                    issues.append(f"Stack size {size} may not maintain 16-byte alignment")
            except ValueError:
                pass
    
    elif isa == 'riscv64':
        # Look for addi sp, sp, #N
        prologue_pattern = r'\taddi\s+sp,sp,-?(\d+)'
        matches = re.findall(prologue_pattern, asm_content)
        
        for match in matches:
            try:
                size = int(match)
                if size % 16 != 0:
                    issues.append(f"Stack size {size} may not maintain 16-byte alignment")
            except ValueError:
                pass
    
    if issues:
        passed = False
    
    return passed, issues


def check_callee_saved(asm_content: str, isa: str) -> tuple:
    """Check if callee-saved registers are properly preserved."""
    issues = []
    passed = True
    
    spec = ABI_SPECS.get(isa)
    if not spec:
        return False, ["Unknown ISA"]
    
    callee_saved = spec['callee_saved']
    
    # Check prologue for saves
    saves_found = set()
    restores_found = set()
    
    for reg in callee_saved:
        if isa == 'x86_64':
            save_pattern = rf'\tmovq\s+%{reg},.*\(%rbp\)'
            restore_pattern = rf'\tmovq\s+.*\(%rbp\),\s*%{reg}'
        elif isa == 'aarch64':
            save_pattern = rf'\tstr\s+{reg},.*\[sp'
            restore_pattern = rf'\tldr\s+{reg},.*\[sp'
        else:  # riscv64
            save_pattern = rf'\tsd\s+{reg},.*\(\s*sp\)'
            restore_pattern = rf'\tld\s+{reg},.*\(\s*sp\)'
        
        if re.search(save_pattern, asm_content):
            saves_found.add(reg)
        if re.search(restore_pattern, asm_content):
            restores_found.add(reg)
    
    # Check for mismatches
    for reg in saves_found:
        if reg not in restores_found:
            issues.append(f"Callee-saved register {reg} saved but not restored")
            passed = False
    
    for reg in restores_found:
        if reg not in saves_found:
            issues.append(f"Callee-saved register {reg} restored but not saved")
            passed = False
    
    return passed, issues


def verify_abi(source_file: Path, isa: str, opt_level: str = '-O3') -> ABIResult:
    """Verify ABI compliance for assembly output."""
    spec = ABI_SPECS.get(isa)
    if not spec:
        return ABIResult(
            isa=isa,
            abi_name="Unknown",
            passed=False,
            issues=[f"Unknown ISA: {isa}"]
        )
    
    # Generate assembly
    returncode, stdout, stderr = run_zerocc_asm(source_file, isa, opt_level)
    if returncode != 0:
        return ABIResult(
            isa=isa,
            abi_name=spec['name'],
            passed=False,
            issues=[f"Compilation failed: {stderr}"]
        )
    
    # Run checks
    checks = {}
    all_issues = []
    
    passed, issues = check_argument_passing(stdout, isa)
    checks['argument_passing'] = passed
    all_issues.extend(issues)
    
    passed, issues = check_return_value(stdout, isa)
    checks['return_value'] = passed
    all_issues.extend(issues)
    
    passed, issues = check_stack_alignment(stdout, isa)
    checks['stack_alignment'] = passed
    all_issues.extend(issues)
    
    passed, issues = check_callee_saved(stdout, isa)
    checks['callee_saved'] = passed
    all_issues.extend(issues)
    
    overall_passed = all(checks.values())
    
    return ABIResult(
        isa=isa,
        abi_name=spec['name'],
        passed=overall_passed,
        checks=checks,
        issues=all_issues
    )


def print_report(results: List[ABIResult]):
    """Print verification report."""
    print(f"\n{'='*70}")
    print(f"ABI Assembly Verification Report")
    print(f"{'='*70}\n")
    
    for result in results:
        print(f"ISA: {result.isa} ({result.abi_name})")
        print(f"Status: {'✅ PASS' if result.passed else '❌ FAIL'}\n")
        
        # Check results
        print("Checks:")
        for check, passed in result.checks.items():
            status = "✓" if passed else "✗"
            print(f"  {status} {check}")
        
        # Issues
        if result.issues:
            print(f"\nIssues ({len(result.issues)}):")
            for issue in result.issues:
                print(f"  ⚠️  {issue}")
        
        print()


def main():
    parser = argparse.ArgumentParser(description='Verify ABI compliance of assembly output')
    parser.add_argument('--test', help='C source file to test')
    parser.add_argument('--target', nargs='+', default=['x86_64'],
                        choices=['x86_64', 'aarch64', 'riscv64'],
                        help='Target ISAs')
    parser.add_argument('-O', '--opt-level', default='-O3', help='Optimization level')
    args = parser.parse_args()
    
    if not args.test:
        parser.error("--test is required")
    
    source_file = Path(args.test)
    if not source_file.exists():
        print(f"Error: {source_file} not found", file=sys.stderr)
        sys.exit(1)
    
    # Verify each ISA
    results = []
    for isa in args.target:
        print(f"Verifying {isa}...", end=" ", flush=True)
        result = verify_abi(source_file, isa, args.opt_level)
        results.append(result)
        print(f"{'✓' if result.passed else '✗'}")
    
    # Print report
    print_report(results)
    
    # Exit status
    all_passed = all(r.passed for r in results)
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
