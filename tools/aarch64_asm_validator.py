#!/usr/bin/env python3
"""
AArch64 Assembly Validator
Validates AArch64 assembly output from ZeroCC -S flag.

This tool checks that the generated assembly:
1. Uses correct AArch64 instruction syntax
2. Does not contain x86-64 leakage (movq, push, etc.)
3. Uses correct register naming (x0-x30, not %rax)
4. Has correct prologue/epilogue patterns
5. Follows AArch64 calling convention

Usage:
    python3 tools/aarch64_asm_validator.py --asm-file test.s
    python3 tools/aarch64_asm_validator.py --input test.c --target aarch64
"""

import argparse
import subprocess
import sys
import re
from pathlib import Path


# x86-64 patterns that should NOT appear in AArch64 output
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

# AArch64 expected patterns
AArch64_PROLOGUE_PATTERNS = [
    (r'\stp[dx]\d+,.*\[sp', 'AArch64 stack save'),
    (r'\tadd\s+sp,sp,-?\d+', 'AArch64 stack allocation'),
    (r'\tstp\s+x29', 'AArch64 frame pointer save'),
]

AArch64_EPILOGUE_PATTERNS = [
    (r'\tldp\s+x[0-9]+', 'AArch64 stack restore'),
    (r'\tret\b', 'AArch64 return'),
    (r'\tadd\s+sp,sp,\d+', 'AArch64 stack deallocation'),
]

# AArch64 register patterns
AArch64_REG_PATTERN = r'\b(x[0-9]{1,2}|w[0-9]{1,2}|sp|lr|fp)\b'


def run_zerocc_asm(source_file: Path, target: str = 'aarch64', opt_level: str = '-O3'):
    """Run ZeroCC with -S flag to generate AArch64 assembly."""
    cmd = ['./build/bin/zerocc', '-S', opt_level, '--target', target, str(source_file), '-o', '-']
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return -1, "", str(e)


def check_x86_leakage(asm_content: str) -> list:
    """Check for x86-64 leakage in AArch64 assembly."""
    issues = []
    
    for pattern, desc in X86_LEAKAGE_PATTERNS:
        matches = re.findall(pattern, asm_content)
        if matches:
            # Find line numbers
            for i, line in enumerate(asm_content.split('\n'), 1):
                if re.search(pattern, line):
                    issues.append(f"⚠️  Line {i}: Found {desc}: {line.strip()}")
    
    return issues


def check_aarch64_syntax(asm_content: str) -> list:
    """Check for valid AArch64 syntax patterns."""
    issues = []
    
    # Check for AArch64 instructions
    aarch64_instrs = [
        r'\t(stp|ldp|str|ldr)\s+',
        r'\t(add|sub|mov)\s+sp',
        r'\t(ret|blr)\s+',
        r'\tb\.(eq|ne|lt|gt|le|ge|al)\s+',
        r'\t(and|orr|eor)\s+',
        r'\t(lsl|lsr|asr)\s+',
    ]
    
    has_instructions = any(re.search(instr, asm_content) for instr in aarch64_instrs)
    if not has_instructions:
        issues.append("⚠️  No AArch64 instructions detected")
    
    # Check for correct register naming
    bad_regs = re.findall(r'%[a-z]+', asm_content)
    if bad_regs:
        issues.append(f"❌ Found x86-64 style registers: {set(bad_regs)}")
    
    return issues


def check_prologue_epilogue(asm_content: str) -> list:
    """Check prologue/epilogue patterns."""
    issues = []
    
    # Check prologue
    has_prologue = any(re.search(p, asm_content) for p in AArch64_PROLOGUE_PATTERNS)
    if not has_prologue:
        issues.append("⚠️  No AArch64 prologue pattern detected")
    
    # Check epilogue
    has_epilogue = any(re.search(p, asm_content) for p in AArch64_EPILOGUE_PATTERNS)
    if not has_epilogue:
        issues.append("⚠️  No AArch64 epilogue pattern detected")
    
    return issues


def check_calling_convention(asm_content: str) -> list:
    """Check for AArch64 calling convention compliance."""
    issues = []
    
    # Check argument passing (x0-x7 for int args)
    # This is a basic check - full verification requires semantic analysis
    arg_pattern = r'\t(mov|orr)\s+(x[0-7]),'
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
            for instr in ['stp', 'ldp', 'str', 'ldr', 'add', 'sub', 'mov', 'ret', 'bl', 'b.']:
                if re.search(rf'\t{instr}', line):
                    stats['instruction_counts'][instr] = stats['instruction_counts'].get(instr, 0) + 1
            
            # Find functions
            if re.match(r'^\s*\.\s*globl\s+\w+', line):
                match = re.search(r'\.\s*globl\s+(\w+)', line)
                if match:
                    stats['functions'].append(match.group(1))
    
    return stats


def validate_assembly(asm_content: str, filename: str = "input") -> dict:
    """Validate AArch64 assembly content."""
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
    
    # Check AArch64 syntax
    results['syntax_issues'] = check_aarch64_syntax(asm_content)
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
    print(f"AArch64 Assembly Validation Report")
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
        print("✅ AArch64 syntax valid\n")
    
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
    parser = argparse.ArgumentParser(description='Validate AArch64 assembly output')
    parser.add_argument('--asm-file', help='Assembly file to validate')
    parser.add_argument('--input', help='C source file to compile and validate')
    parser.add_argument('--target', default='aarch64', help='Target ISA (default: aarch64)')
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
