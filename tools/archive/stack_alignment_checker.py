#!/usr/bin/env python3
"""
Stack Alignment Checker
Verifies that stack is properly aligned before CALL instructions for multiple ISAs.

This tool tracks stack pointer changes through function execution and validates
16-byte alignment before all call sites.

Usage:
    python3 tools/stack_alignment_checker.py --asm-file test.s --target x86_64
    python3 tools/stack_alignment_checker.py --input test.c --target aarch64
"""

import argparse
import subprocess
import sys
import re
from pathlib import Path


# ISA-specific stack alignment requirements
ISA_ALIGNMENT = {
    'x86_64': {
        'align': 16,
        'sp_reg': 'rsp',
        'prologue_adjust': 8,  # push rbp
        'call_align': 16,  # RSP must be 16-byte aligned before call
    },
    'aarch64': {
        'align': 16,
        'sp_reg': 'sp',
        'prologue_adjust': 0,  # No implicit adjustment
        'call_align': 16,  # SP must be 16-byte aligned before call
    },
    'riscv64': {
        'align': 16,
        'sp_reg': 'sp',
        'prologue_adjust': 0,  # No implicit adjustment
        'call_align': 16,  # SP must be 16-byte aligned before call
    },
}


def run_zerocc_asm(source_file, target='x86_64', opt_level='-O3'):
    """Run ZeroCC with -S flag to generate assembly."""
    cmd = ['./build/bin/zerocc', '-S', opt_level, '--target', target, str(source_file), '-o', '-']
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return -1, "", str(e)


def track_stack_alignment(asm_content, target='x86_64'):
    """Track stack pointer changes and check alignment at call sites."""
    issues = []
    config = ISA_ALIGNMENT.get(target, ISA_ALIGNMENT['x86_64'])
    align = config['align']
    prologue_adjust = config['prologue_adjust']
    
    # Initial alignment state (after function entry)
    # We track offset from 16-byte boundary
    current_offset = prologue_adjust % align
    
    lines = asm_content.split('\n')
    in_function = False
    call_line = 0
    
    for i, line in enumerate(lines, 1):
        stripped = line.strip()
        
        # Track function boundaries
        if re.search(r'^\s*\.\s*globl\s+\w+', line) or re.match(r'^\s*\w+:', line):
            in_function = True
            current_offset = prologue_adjust % align
        
        if not in_function:
            continue
        
        # Track stack pointer changes
        if target == 'x86_64':
            # subq $N, %rsp
            sub_match = re.search(r'\tsubq\s+\$(\d+),\s*%rsp', line)
            if sub_match:
                current_offset = (current_offset + int(sub_match.group(1))) % align
            
            # addq $N, %rsp
            add_match = re.search(r'\taddq\s+\$(\d+),\s*%rsp', line)
            if add_match:
                current_offset = (current_offset - int(add_match.group(1))) % align
            
            # pushq
            if re.search(r'\tpushq\s', line):
                current_offset = (current_offset + 8) % align
            
            # popq
            if re.search(r'\tpopq\s', line):
                current_offset = (current_offset - 8) % align
            
            # Check alignment before call
            if re.search(r'\t(call|callq)\s', line):
                if current_offset != 0:
                    issues.append(f"⚠️  Line {i}: RSP not 16-byte aligned before call (offset={current_offset})")
                call_line = i
        
        elif target == 'aarch64':
            # sub sp, sp, #N
            sub_match = re.search(r'\tsub\s+sp,sp,-?(\d+)', line)
            if sub_match:
                current_offset = (current_offset + int(sub_match.group(1))) % align
            
            # add sp, sp, #N
            add_match = re.search(r'\tadd\s+sp,sp,(\d+)', line)
            if add_match:
                current_offset = (current_offset - int(add_match.group(1))) % align
            
            # Check alignment before call
            if re.search(r'\tbl\s', line):
                if current_offset != 0:
                    issues.append(f"⚠️  Line {i}: SP not 16-byte aligned before call (offset={current_offset})")
                call_line = i
        
        else:  # riscv64
            # addi sp, sp, #N
            addi_match = re.search(r'\taddi\s+sp,sp,-?(\d+)', line)
            if addi_match:
                current_offset = (current_offset + int(addi_match.group(1))) % align
            
            # Check alignment before call
            if re.search(r'\tjal\s+ra,', line):
                if current_offset != 0:
                    issues.append(f"⚠️  Line {i}: SP not 16-byte aligned before call (offset={current_offset})")
                call_line = i
    
    return issues


def validate_assembly(asm_content, target='x86_64'):
    """Validate stack alignment for assembly."""
    results = {
        'target': target,
        'valid': True,
        'issues': [],
    }
    
    # Track stack alignment
    results['issues'] = track_stack_alignment(asm_content, target)
    
    if results['issues']:
        results['valid'] = False
    
    return results


def print_report(results):
    """Print validation report."""
    print(f"\n{'='*70}")
    print(f"Stack Alignment Validation Report")
    print(f"{'='*70}\n")
    
    print(f"Target: {results['target']}")
    print(f"Status: {'✅ VALID' if results['valid'] else '❌ INVALID'}\n")
    
    if results['issues']:
        print(f"Alignment Issues ({len(results['issues'])}):")
        for issue in results['issues']:
            print(f"  {issue}")
        print()
    else:
        print("✅ All call sites have proper stack alignment\n")


def main():
    parser = argparse.ArgumentParser(description='Validate stack alignment at call sites')
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
    
    # Get assembly content
    if args.asm_file:
        asm_file = Path(args.asm_file)
        if not asm_file.exists():
            print(f"Error: {asm_file} not found", file=sys.stderr)
            sys.exit(1)
        with open(asm_file) as f:
            asm_content = f.read()
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
    
    # Validate
    results = validate_assembly(asm_content, args.target)
    
    # Print report
    print_report(results)
    
    # Exit with appropriate code
    sys.exit(0 if results['valid'] else 1)


if __name__ == "__main__":
    main()
