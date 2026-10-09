#!/usr/bin/env python3
"""
RISC-V 64-bit Assembly Validator
Validates RISC-V assembly output from ZeroCC -S flag.

This tool checks that the generated assembly:
1. Uses correct RISC-V instruction syntax
2. Does not contain x86-64 leakage (movq, push, etc.)
3. Uses correct register naming (a0-a7, s0-s11, not %rax)
4. Has correct prologue/epilogue patterns
5. Follows RISC-V calling convention

Usage:
    python3 tools/riscv64_asm_validator.py --asm-file test.s
    python3 tools/riscv64_asm_validator.py --input test.c --target riscv64
"""

import argparse
import subprocess
import sys
import re
from pathlib import Path


# x86-64 patterns that should NOT appear in RISC-V output
X86_LEAKAGE_PATTERNS = [
    (r'\b(movq|movl|movw|movb)\s', 'x86-64 mov variant'),
    (r'\b(push|pop)\s+%', 'x86-64 push/pop'),
    (r'\b(call|jmp)\s+%', 'x86-64 call/jmp with %'),
    (r'%[a-z]+', 'x86-64 register syntax (%)'),
    (r'\brbp\b', 'x86-64 base pointer'),
    (r'\brsp\b', 'x86-64 stack pointer'),
    (r'\bleaq?\s', 'x86-64 LEA'),
    (r'\bcqo\b', 'x86-64 CQO'),
    (r'\bidiv\s', 'x86-64 IDIV'),
    (r'\bsubq\s', 'x86-64 SUBQ'),
    (r'\baddq\s', 'x86-64 ADDQ'),
]

# RISC-V expected patterns
RISCV_PROLOGUE_PATTERNS = [
    (r'\taddi\s+sp,sp,-?\d+', 'RISC-V stack allocation'),
    (r'\tsd\s+s[0-9]+,-?\d+\(sp\)', 'RISC-V callee-saved save'),
    (r'\taddi\s+s0,sp,\d+', 'RISC-V frame pointer setup'),
]

RISCV_EPILOGUE_PATTERNS = [
    (r'\tld\s+s[0-9]+,-?\d+\(sp\)', 'RISC-V callee-saved restore'),
    (r'\taddi\s+sp,sp,\d+', 'RISC-V stack deallocation'),
    (r'\tret\b', 'RISC-V return'),
]

# RISC-V register patterns
RISCV_REG_PATTERN = r'\b(a[0-7]|s[0-9]{0,2}|t[0-6]|sp|fp|ra|gp|tp)\b'


def run_zerocc_asm(source_file: Path, target: str = 'riscv64', opt_level: str = '-O3'):
    """Run ZeroCC with -S flag to generate RISC-V assembly."""
    cmd = ['./build/bin/zerocc', '-S', opt_level, '--target', target, str(source_file), '-o', '-']
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return -1, "", str(e)


def check_x86_leakage(asm_content: str) -> list:
    """Check for x86-64 leakage in RISC-V assembly."""
    issues = []
    
    for pattern, desc in X86_LEAKAGE_PATTERNS:
        matches = re.findall(pattern, asm_content)
        if matches:
            # Find line numbers
            for i, line in enumerate(asm_content.split('\n'), 1):
                if re.search(pattern, line):
                    issues.append(f"⚠️  Line {i}: Found {desc}: {line.strip()}")
    
    return issues


def check_riscv_syntax(asm_content: str) -> list:
    """Check for valid RISC-V syntax patterns."""
    issues = []
    
    # Check for RISC-V instructions
    riscv_instrs = [
        r'\t(addi|sd|ld|sw|lw)\s+',
        r'\t(add|sub|mv)\s+sp',
        (r'\t(ret|jr\s+ra)\s*', 'RISC-V return'),
        r'\t(jal|beq|bne|blt|bge)\s+',
        r'\t(and|or|xor)\s+',
        r'\tsll|srl|sra',
    ]
    
    has_instructions = any(re.search(instr if isinstance(instr, str) else instr[0], asm_content) for instr in riscv_instrs)
    if not has_instructions:
        issues.append("⚠️  No RISC-V instructions detected")
    
    # Check for correct register naming
    bad_regs = re.findall(r'%[a-z]+', asm_content)
    if bad_regs:
        issues.append(f"❌ Found x86-64 style registers: {set(bad_regs)}")
    
    return issues


def check_prologue_epilogue(asm_content: str) -> list:
    """Check prologue/epilogue patterns."""
    issues = []
    
    # Check prologue
    has_prologue = any(re.search(p, asm_content) for p in RISCV_PROLOGUE_PATTERNS)
    if not has_prologue:
        issues.append("⚠️  No RISC-V prologue pattern detected")
    
    # Check epilogue
    has_epilogue = any(re.search(p, asm_content) for p in RISCV_EPILOGUE_PATTERNS)
    if not has_epilogue:
        issues.append("⚠️  No RISC-V epilogue pattern detected")
    
    return issues


def check_calling_convention(asm_content: str) -> list:
    """Check for RISC-V calling convention compliance."""
    issues = []
    
    # Check argument passing (a0-a7 for int args)
    arg_pattern = r'\t(addi|mv)\s+(a[0-7]),'
    if re.search(arg_pattern, asm_content):
        pass  # Good - using argument registers
    
    # Check for x86-64 ABI leakage (rdi, rsi, etc.)
    x86_abi = re.findall(r'\b(rdi|rsi|rdx|rcx|r8|r9)\b', asm_content)
    if x86_abi:
        issues.append(f"❌ Found x86-64 ABI registers: {set(x86_abi)}")
    
    return issues


def analyze_assembly_stats(asm_content: str) -> dict:
    """Analyze assembly statistics."""
    lines = asm_content.split('\n')
    
    stats = {
        'total_lines': len(lines),
        'code_lines': 0,
        'comment_lines': 0,
        'blank_lines': 0,
        'functions': [],
        'instruction_counts': {},
    }
    
    for line in lines:
        stripped = line.strip()
        if not stripped:
            stats['blank_lines'] += 1
        elif stripped.startswith('#'):
            stats['comment_lines'] += 1
        else:
            stats['code_lines'] += 1
            
            # Count instructions
            for instr in ['addi', 'sd', 'ld', 'sw', 'lw', 'add', 'sub', 'mv', 'ret', 'jal', 'b.']:
                if re.search(rf'\t{instr}', line):
                    stats['instruction_counts'][instr] = stats['instruction_counts'].get(instr, 0) + 1
            
            # Find functions
            if re.match(r'^\s*\.\s*globl\s+\w+', line):
                match = re.search(r'\.\s*globl\s+(\w+)', line)
                if match:
                    stats['functions'].append(match.group(1))
    
    return stats


def validate_assembly(asm_content: str, filename: str = "input") -> dict:
    """Validate RISC-V assembly content."""
    results = {
        'filename': filename,
        'valid': True,
        'x86_leakage': [],
        'syntax_issues': [],
        'prologue_epilogue_issues': [],
        'calling_convention_issues': [],
        'stats': {},
    }
    
    # Check for x86-64 leakage
    results['x86_leakage'] = check_x86_leakage(asm_content)
    if results['x86_leakage']:
        results['valid'] = False
    
    # Check RISC-V syntax
    results['syntax_issues'] = check_riscv_syntax(asm_content)
    if results['syntax_issues']:
        results['valid'] = False
    
    # Check prologue/epilogue
    results['prologue_epilogue_issues'] = check_prologue_epilogue(asm_content)
    
    # Check calling convention
    results['calling_convention_issues'] = check_calling_convention(asm_content)
    if results['calling_convention_issues']:
        results['valid'] = False
    
    # Analyze stats
    results['stats'] = analyze_assembly_stats(asm_content)
    
    return results


def print_report(results: dict):
    """Print validation report."""
    print(f"\n{'='*70}")
    print(f"RISC-V 64-bit Assembly Validation Report")
    print(f"{'='*70}\n")
    
    print(f"File: {results['filename']}")
    print(f"Status: {'✅ VALID' if results['valid'] else '❌ INVALID'}\n")
    
    # X86 leakage
    if results['x86_leakage']:
        print(f"x86-64 Leakage Detected ({len(results['x86_leakage'])} issues):")
        for issue in results['x86_leakage'][:10]:  # Show first 10
            print(f"  {issue}")
        if len(results['x86_leakage']) > 10:
            print(f"  ... and {len(results['x86_leakage']) - 10} more")
        print()
    else:
        print("✅ No x86-64 leakage detected\n")
    
    # Syntax issues
    if results['syntax_issues']:
        print(f"Syntax Issues ({len(results['syntax_issues'])} issues):")
        for issue in results['syntax_issues']:
            print(f"  {issue}")
        print()
    else:
        print("✅ RISC-V syntax valid\n")
    
    # Prologue/Epilogue
    if results['prologue_epilogue_issues']:
        print(f"Prologue/Epilogue Issues:")
        for issue in results['prologue_epilogue_issues']:
            print(f"  {issue}")
        print()
    else:
        print("✅ Prologue/Epilogue patterns found\n")
    
    # Calling convention
    if results['calling_convention_issues']:
        print(f"Calling Convention Issues:")
        for issue in results['calling_convention_issues']:
            print(f"  {issue}")
        print()
    else:
        print("✅ Calling convention appears valid\n")
    
    # Statistics
    stats = results['stats']
    print(f"Statistics:")
    print(f"  Total lines: {stats['total_lines']}")
    print(f"  Code lines: {stats['code_lines']}")
    print(f"  Functions: {len(stats['functions'])} ({', '.join(stats['functions'][:5])})")
    if stats['instruction_counts']:
        print(f"  Top instructions: {', '.join(f'{k}={v}' for k, v in list(stats['instruction_counts'].items())[:5])}")
    print()


def main():
    parser = argparse.ArgumentParser(description='Validate RISC-V 64-bit assembly output')
    parser.add_argument('--asm-file', help='Assembly file to validate')
    parser.add_argument('--input', help='C source file to compile and validate')
    parser.add_argument('--target', default='riscv64', help='Target ISA (default: riscv64)')
    parser.add_argument('-O', '--opt-level', default='-O3', help='Optimization level')
    parser.add_argument('-v', '--verbose', action='store_true', help='Verbose output')
    args = parser.parse_args()
    
    # Validate arguments
    if not args.asm_file and not args.input:
        parser.error("Either --asm-file or --input is required")
    
    # Get assembly content
    if args.asm_file:
        asm_file = Path(args.asm_file)
        if not asm_file.exists():
            print(f"Error: {asm_file} not found", file=sys.stderr)
            sys.exit(1)
        with open(asm_file) as f:
            asm_content = f.read()
        filename = str(asm_file)
    else:
        source_file = Path(args.input)
        if not source_file.exists():
            print(f"Error: {source_file} not found", file=sys.stderr)
            sys.exit(1)
        
        returncode, stdout, stderr = run_zerocc_asm(source_file, args.target, args.opt_level)
        if returncode != 0:
            print(f"Error: ZeroCC failed: {stderr}", file=sys.stderr)
            sys.exit(1)
        asm_content = stdout
        filename = str(source_file)
    
    # Validate
    results = validate_assembly(asm_content, filename)
    
    # Print report
    print_report(results)
    
    # Exit with appropriate code
    sys.exit(0 if results['valid'] else 1)


if __name__ == "__main__":
    main()
