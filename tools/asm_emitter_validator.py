#!/usr/bin/env python3
"""
Assembly Emitter Validator Tool
Validates and compares assembly output from ZeroCC -S flag.

This tool helps verify that the -S flag correctly emits text assembly
instead of binary ELF output. Supports x86-64, AArch64, and RISC-V.

Usage:
    python3 tools/asm_emitter_validator.py --input test.c --target x86_64
    python3 tools/asm_emitter_validator.py --input test.c --target aarch64
    python3 tools/asm_emitter_validator.py --input test.c --target riscv64
"""

import os
import sys
import subprocess
import re
from pathlib import Path

# ISA-specific assembly patterns
ISA_PATTERNS = {
    'x86_64': {
        'instructions': [
            r'\t(mov|call|jmp|add|sub|push|pop|lea|cmp|test)\s',
            r'\t(movq|movl|movw|movb)\s',
        ],
        'registers': r'%[a-z]+',
        'prologue': [r'\tpush\s+%rbp', r'\tmovq\s+%rsp,%rbp'],
        'epilogue': [r'\tleave', r'\tret'],
        'section': r'\.\s*(text|data|rodata)',
        'function': r'^\s*\.\s*globl\s+\w+',
    },
    'aarch64': {
        'instructions': [
            r'\t(stp|ldp|str|ldr|add|sub|mov|ret|bl|b\.|and|orr)\s',
        ],
        'registers': r'\b(x[0-9]{1,2}|w[0-9]{1,2}|sp|lr|fp)\b',
        'prologue': [r'\stp[dx]\d+,.*\[sp', r'\tadd\s+sp,sp,-?\d+'],
        'epilogue': [r'\tldp\s+x[0-9]+', r'\tret'],
        'section': r'\.\s*(text|data|rodata)',
        'function': r'^\s*\.\s*globl\s+\w+',
    },
    'riscv64': {
        'instructions': [
            r'\t(addi|sd|ld|sw|lw|add|sub|mv|ret|jal|beq|bne)\s',
        ],
        'registers': r'\b(a[0-7]|s[0-9]{0,2}|t[0-6]|sp|fp|ra|gp|tp)\b',
        'prologue': [r'\taddi\s+sp,sp,-?\d+', r'\tsd\s+s[0-9]+'],
        'epilogue': [r'\tld\s+s[0-9]+', r'\tret'],
        'section': r'\.\s*(text|data|rodata)',
        'function': r'^\s*\.\s*globl\s+\w+',
    },
}

# Binary detection (ELF magic bytes)
ELF_MAGIC = b'\x7fELF'


def run_zerocc_asm(source_file, target='x86_64', opt_level="-O3", output_file=None):
    """Run ZeroCC with -S flag to generate assembly."""
    cmd = ['./build/bin/zerocc', '-S', opt_level, '--target', target, source_file]
    if output_file:
        cmd.extend(['-o', output_file])
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return -1, "", str(e)


def check_assembly_format(asm_content, filename, target='x86_64'):
    """Check if content is valid assembly text for the target ISA."""
    issues = []
    patterns = ISA_PATTERNS.get(target, ISA_PATTERNS['x86_64'])
    
    # Check for binary content (ELF magic bytes)
    if isinstance(asm_content, bytes):
        if asm_content[:4] == ELF_MAGIC:
            issues.append("❌ OUTPUT IS BINARY ELF (not text assembly)")
            return issues
    elif asm_content.startswith('\x7fELF'):
        issues.append("❌ OUTPUT IS BINARY ELF (not text assembly)")
        return issues
    
    # Check for valid assembly patterns
    lines = asm_content.split('\n')
    
    has_section = any(re.search(patterns['section'], line) for line in lines)
    has_function = any(re.search(patterns['function'], line) for line in lines)
    has_instructions = any(re.search(pattern, asm_content) for pattern in patterns['instructions'])
    has_registers = re.search(patterns['registers'], asm_content) is not None
    
    if not has_section:
        issues.append(f"⚠️  No .text/.data/.rodata sections found")
    if not has_function:
        issues.append(f"⚠️  No function declarations found")
    if not has_instructions:
        issues.append(f"⚠️  No {target} instructions found")
    if not has_registers:
        issues.append(f"⚠️  No {target} registers found")
    
    # Check prologue/epilogue
    has_prologue = any(re.search(p, asm_content) for p in patterns['prologue'])
    has_epilogue = any(re.search(p, asm_content) for p in patterns['epilogue'])
    
    if not has_prologue:
        issues.append(f"⚠️  No {target} prologue pattern found")
    if not has_epilogue:
        issues.append(f"⚠️  No {target} epilogue pattern found")
    
    if not issues:
        issues.append(f"✅ Valid {target} assembly format detected")
    
    return issues


def analyze_assembly_stats(asm_content, target='x86_64'):
    """Analyze assembly statistics."""
    lines = asm_content.split('\n')
    
    stats = {
        'total_lines': len(lines),
        'code_lines': 0,
        'comment_lines': 0,
        'blank_lines': 0,
        'instruction_counts': {},
        'functions': [],
        'simd_instructions': 0,
    }
    
    # ISA-specific instruction patterns
    if target == 'x86_64':
        instruction_patterns = {
            'mov': r'\t(movq|movl|movw|movb|movabs)\s',
            'call': r'\tcall\s',
            'jmp': r'\t(jmp|je|jne|jl|jle|jg|jge|ja|jb)\s',
            'add': r'\taddq\s',
            'sub': r'\tsubq\s',
            'imul': r'\timul\s',
            'push': r'\tpushq\s',
            'pop': r'\tpopq\s',
            'lea': r'\tleaq\s',
            'cmp': r'\tcmpq\s',
            'test': r'\ttest\s',
        }
        simd_patterns = [r'\t(addss|subss|mulss|divss|movss|movsd|cvtsi2ss)',
                        r'\t(addps|subps|mulpd|divpd|movaps|movapd)']
    elif target == 'aarch64':
        instruction_patterns = {
            'mov': r'\t(mov|orr|movk)\s',
            'call': r'\tbl\s',
            'jmp': r'\tb\.(eq|ne|lt|gt|le|ge|al)\s',
            'add': r'\tadd\s',
            'sub': r'\tsub\s',
            'str': r'\tstr\s',
            'ldr': r'\tldr\s',
            'stp': r'\stp[dx]\s',
            'ldp': r'\tldp\s',
        }
        simd_patterns = [r'\t(fadd|fsub|fmul|fdiv|ld1|st1)\s']
    else:  # riscv64
        instruction_patterns = {
            'addi': r'\taddi\s',
            'call': r'\tjal\s',
            'jmp': r'\t(beq|bne|blt|bge)\s',
            'add': r'\tadd\s',
            'sub': r'\tsub\s',
            'sd': r'\tsd\s',
            'ld': r'\tld\s',
        }
        simd_patterns = [r'\t(fadd|fsub|fmul|fdiv)\.d\s']
    
    for line in lines:
        stripped = line.strip()
        if not stripped:
            stats['blank_lines'] += 1
        elif stripped.startswith('#') or stripped.startswith('//'):
            stats['comment_lines'] += 1
        else:
            stats['code_lines'] += 1
            
            # Count instructions
            for name, pattern in instruction_patterns.items():
                if re.search(pattern, line):
                    stats['instruction_counts'][name] = stats['instruction_counts'].get(name, 0) + 1
            
            # Count SIMD
            for pattern in simd_patterns:
                if re.search(pattern, line):
                    stats['simd_instructions'] += 1
                    break
            
            # Find functions
            if re.search(r'^\s*\.\s*globl\s+\w+', line):
                match = re.search(r'\.\s*globl\s+(\w+)', line)
                if match:
                    stats['functions'].append(match.group(1))
    
    return stats


def validate_assembly_file(asm_file, target='x86_64'):
    """Validate assembly from file."""
    with open(asm_file) as f:
        content = f.read()
    
    issues = check_assembly_format(content, str(asm_file), target)
    stats = analyze_assembly_stats(content, target)
    
    return {
        'file': str(asm_file),
        'target': target,
        'valid': not any(i.startswith('❌') for i in issues),
        'issues': issues,
        'stats': stats,
    }


def validate_zerocc_output(source_file, target='x86_64', opt_level='-O3'):
    """Validate assembly from ZeroCC -S output."""
    returncode, stdout, stderr = run_zerocc_asm(source_file, target, opt_level)
    
    if returncode != 0:
        return {
            'file': str(source_file),
            'target': target,
            'valid': False,
            'issues': [f"❌ ZeroCC failed: {stderr}"],
            'stats': {},
        }
    
    issues = check_assembly_format(stdout, str(source_file), target)
    stats = analyze_assembly_stats(stdout, target)
    
    return {
        'file': str(source_file),
        'target': target,
        'valid': not any(i.startswith('❌') for i in issues),
        'issues': issues,
        'stats': stats,
    }


def print_report(result):
    """Print validation report."""
    print(f"\n{'='*70}")
    print(f"Assembly Validation Report")
    print(f"{'='*70}\n")
    
    print(f"File: {result['file']}")
    print(f"Target: {result['target']}")
    print(f"Status: {'✅ VALID' if result['valid'] else '❌ INVALID'}\n")
    
    print("Issues:")
    for issue in result['issues']:
        print(f"  {issue}")
    print()
    
    # Statistics
    stats = result.get('stats', {})
    if stats:
        print(f"Statistics:")
        print(f"  Total lines: {stats.get('total_lines', 0)}")
        print(f"  Code lines: {stats.get('code_lines', 0)}")
        print(f"  Functions: {len(stats.get('functions', []))}")
        if stats.get('instruction_counts'):
            instr = stats['instruction_counts']
            print(f"  Top instructions: {', '.join(f'{k}={v}' for k, v in list(instr.items())[:5])}")
        if stats.get('simd_instructions', 0) > 0:
            print(f"  SIMD instructions: {stats['simd_instructions']}")
        print()


def main():
    import argparse
    
    parser = argparse.ArgumentParser(description='Validate assembly output from ZeroCC -S flag')
    parser.add_argument('--asm-file', help='Assembly file to validate')
    parser.add_argument('--input', help='C source file to compile and validate')
    parser.add_argument('--target', default='x86_64', choices=['x86_64', 'aarch64', 'riscv64'],
                        help='Target ISA (default: x86_64)')
    parser.add_argument('-O', '--opt-level', default='-O3', help='Optimization level')
    parser.add_argument('-v', '--verbose', action='store_true', help='Verbose output')
    args = parser.parse_args()
    
    # Validate arguments
    if not args.asm_file and not args.input:
        parser.error("Either --asm-file or --input is required")
    
    # Validate
    if args.asm_file:
        result = validate_assembly_file(args.asm_file, args.target)
    else:
        result = validate_zerocc_output(args.input, args.target, args.opt_level)
    
    # Print report
    print_report(result)
    
    # Exit with appropriate code
    sys.exit(0 if result['valid'] else 1)


if __name__ == "__main__":
    main()
