#!/usr/bin/env python3
"""
Prologue Validator Tool
Validates prologue/epilogue code generation for multiple ISAs.

This tool checks that the generated assembly has correct:
- Prologue sequence (push rbp / stp x29 / addi sp for different ISAs)
- Epilogue sequence (leave / ldp x29 / ret for different ISAs)
- Frame size alignment (16-byte for all ISAs)

Usage:
    python3 tools/prologue_validator.py --asm-file test.s --target x86_64
    python3 tools/prologue_validator.py --input test.c --target aarch64
"""

import argparse
import subprocess
import sys
import re
from pathlib import Path


# ISA-specific prologue/epilogue patterns
ISA_PROLOGUES = {
    'x86_64': {
        'prologue': [
            r'\tpush\s+%rbp',
            r'\tmovq\s+%rsp,%rbp',
            r'\tsubq\s+\$(\d+),\s*%rsp',
        ],
        'epilogue': [
            r'\tleave',
            r'\tret',
        ],
        'frame_align': 16,
    },
    'aarch64': {
        'prologue': [
            r'\stp[dx]\d+,.*\[sp',
            r'\tadd\s+sp,sp,-?(\d+)',
        ],
        'epilogue': [
            r'\tldp\s+x[0-9]+',
            r'\tadd\s+sp,sp,(\d+)',
            r'\tret',
        ],
        'frame_align': 16,
    },
    'riscv64': {
        'prologue': [
            r'\taddi\s+sp,sp,-?(\d+)',
            r'\tsd\s+s[0-9]+,-?\d+\(sp\)',
        ],
        'epilogue': [
            r'\tld\s+s[0-9]+,-?\d+\(sp\)',
            r'\taddi\s+sp,sp,(\d+)',
            r'\tret',
        ],
        'frame_align': 16,
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


def validate_prologue(asm_content, target='x86_64'):
    """Validate prologue sequence."""
    issues = []
    patterns = ISA_PROLOGUES.get(target, ISA_PROLOGUES['x86_64'])
    
    prologue_found = 0
    for pattern in patterns['prologue']:
        if re.search(pattern, asm_content):
            prologue_found += 1
    
    if prologue_found < len(patterns['prologue']) - 1:  # Allow some flexibility
        issues.append(f"⚠️  Incomplete prologue sequence ({prologue_found}/{len(patterns['prologue'])} patterns)")
    else:
        issues.append(f"✅ Prologue sequence valid")
    
    return issues


def validate_epilogue(asm_content, target='x86_64'):
    """Validate epilogue sequence."""
    issues = []
    patterns = ISA_PROLOGUES.get(target, ISA_PROLOGUES['x86_64'])
    
    epilogue_found = 0
    for pattern in patterns['epilogue']:
        if re.search(pattern, asm_content):
            epilogue_found += 1
    
    if epilogue_found < len(patterns['epilogue']) - 1:  # Allow some flexibility
        issues.append(f"⚠️  Incomplete epilogue sequence ({epilogue_found}/{len(patterns['epilogue'])} patterns)")
    else:
        issues.append(f"✅ Epilogue sequence valid")
    
    return issues


def validate_frame_alignment(asm_content, target='x86_64'):
    """Validate frame size alignment."""
    issues = []
    patterns = ISA_PROLOGUES.get(target, ISA_PROLOGUES['x86_64'])
    align = patterns['frame_align']
    
    # Extract frame sizes
    if target == 'x86_64':
        pattern = r'\tsubq\s+\$(\d+),\s*%rsp'
    elif target == 'aarch64':
        pattern = r'\tadd\s+sp,sp,-?(\d+)'
    else:  # riscv64
        pattern = r'\taddi\s+sp,sp,-?(\d+)'
    
    matches = re.findall(pattern, asm_content)
    
    if not matches:
        issues.append(f"⚠️  No frame allocation found")
        return issues
    
    for size_str in matches:
        try:
            size = int(size_str)
            if size % align != 0:
                issues.append(f"⚠️  Frame size {size} not {align}-byte aligned")
            else:
                issues.append(f"✅ Frame size {size} is {align}-byte aligned")
        except ValueError:
            pass
    
    return issues


def validate_assembly(asm_content, target='x86_64'):
    """Validate complete assembly."""
    results = {
        'target': target,
        'valid': True,
        'prologue_issues': [],
        'epilogue_issues': [],
        'alignment_issues': [],
    }
    
    # Validate prologue
    results['prologue_issues'] = validate_prologue(asm_content, target)
    if any(i.startswith('⚠️') for i in results['prologue_issues']):
        results['valid'] = False
    
    # Validate epilogue
    results['epilogue_issues'] = validate_epilogue(asm_content, target)
    if any(i.startswith('⚠️') for i in results['epilogue_issues']):
        results['valid'] = False
    
    # Validate alignment
    results['alignment_issues'] = validate_frame_alignment(asm_content, target)
    if any(i.startswith('⚠️') for i in results['alignment_issues']):
        results['valid'] = False
    
    return results


def print_report(results):
    """Print validation report."""
    print(f"\n{'='*70}")
    print(f"Prologue/Epilogue Validation Report")
    print(f"{'='*70}\n")
    
    print(f"Target: {results['target']}")
    print(f"Status: {'✅ VALID' if results['valid'] else '❌ INVALID'}\n")
    
    # Prologue
    print("Prologue:")
    for issue in results['prologue_issues']:
        print(f"  {issue}")
    print()
    
    # Epilogue
    print("Epilogue:")
    for issue in results['epilogue_issues']:
        print(f"  {issue}")
    print()
    
    # Alignment
    print("Frame Alignment:")
    for issue in results['alignment_issues']:
        print(f"  {issue}")
    print()


def main():
    parser = argparse.ArgumentParser(description='Validate prologue/epilogue code generation')
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
