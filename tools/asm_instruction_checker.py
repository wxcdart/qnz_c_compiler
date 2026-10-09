#!/usr/bin/env python3
"""
Assembly Instruction Checker
Validates that emitted assembly uses correct instructions for the target ISA.
Supports x86-64 SSE2, AArch64 NEON, and RISC-V V extension.

Usage:
    python3 tools/asm_instruction_checker.py test.c --target x86_64
    python3 tools/asm_instruction_checker.py test.c --target aarch64
"""

import os
import sys
import subprocess
import re
from pathlib import Path


# ISA-specific instruction patterns
ISA_INSTRUCTIONS = {
    'x86_64': {
        'arithmetic': {
            'addss': r'\taddss\s',
            'subss': r'\tsubss\s',
            'mulss': r'\tmulss\s',
            'divss': r'\tdivss\s',
            'addps': r'\taddps\s',
            'subps': r'\tsubps\s',
            'mulps': r'\tmulps\s',
            'divps': r'\tdivps\s',
            'addpd': r'\taddpd\s',
            'subpd': r'\tsubpd\s',
            'mulpd': r'\tmulpd\s',
            'divpd': r'\tdivpd\s',
        },
        'move': {
            'movss': r'\tmovss\s',
            'movsd': r'\tmovsd\s',
            'movaps': r'\tmovaps\s',
            'movapd': r'\tmovapd\s',
            'movups': r'\tmovups\s',
            'movupd': r'\tmovupd\s',
            'movdqa': r'\tmovdqa\s',
            'movdqu': r'\tmovdqu\s',
        },
        'conversion': {
            'cvtsi2ss': r'\tcvtsi2ss\s',
            'cvtsi2sd': r'\tcvtsi2sd\s',
            'cvtss2si': r'\tcvtss2si\s',
            'cvtsd2si': r'\tcvtsd2si\s',
            'cvttss2si': r'\tcvttss2si\s',
            'cvttsd2si': r'\tcvttsd2si\s',
            'cvtss2sd': r'\tcvtss2sd\s',
            'cvtsd2ss': r'\tcvtsd2ss\s',
        },
        'comparison': {
            'cmpss': r'\tcmpss\s',
            'cmpps': r'\tcmpps\s',
            'cmpsd': r'\tcmpsd\s',
            'cmppd': r'\tcmppd\s',
            'ucomiss': r'\tucomiss\s',
            'ucomisd': r'\tucomisd\s',
        },
        'logical': {
            'andps': r'\tandps\s',
            'andpd': r'\tandpd\s',
            'orps': r'\torps\s',
            'orpd': r'\torpd\s',
            'xorps': r'\txorps\s',
            'xorpd': r'\txorpd\s',
        },
        'shuffle': {
            'shufps': r'\tshufps\s',
            'shufpd': r'\tshufpd\s',
        },
        'misc': {
            'sqrtsd': r'\tsqrtsd\s',
            'sqrtss': r'\tsqrtss\s',
        },
    },
    'aarch64': {
        'arithmetic': {
            'fadd': r'\tfadd\s+v\d+,\s*v\d+,\s*v\d+',
            'fsub': r'\tfsub\s+v\d+,\s*v\d+,\s*v\d+',
            'fmul': r'\tfmul\s+v\d+,\s*v\d+,\s*v\d+',
            'fdiv': r'\tfdiv\s+v\d+,\s*v\d+,\s*v\d+',
        },
        'move': {
            'mov': r'\tmov\s+v\d+,\s*v\d+',
            'ld1': r'\tld1\s+\{v\d+\.\w+\}',
            'st1': r'\tst1\s+\{v\d+\.\w+\}',
        },
        'conversion': {
            'scvtf': r'\tscvtf\s+v\d+,\s*v\d+',
            'fcvtzs': r'\tfcvtzs\s+v\d+,\s*v\d+',
        },
        'comparison': {
            'fcmp': r'\tfcmp\s+v\d+,\s*v\d+',
            'fcmgt': r'\tfcmgt\s+v\d+,\s*v\d+',
        },
        'logical': {
            'and': r'\tand\s+v\d+,\s*v\d+,\s*v\d+',
            'orr': r'\torr\s+v\d+,\s*v\d+,\s*v\d+',
            'eor': r'\teor\s+v\d+,\s*v\d+,\s*v\d+',
        },
        'shuffle': {
            'tbl': r'\ttbl\s+v\d+,\s+\{v\d+\},\s+v\d+',
            'ext': r'\text\s+v\d+,\s*v\d+,\s*v\d+',
        },
    },
    'riscv64': {
        'arithmetic': {
            'fadd.d': r'\tfadd\.d\s+fa\d+,\s*fa\d+,\s*fa\d+',
            'fsub.d': r'\tfsub\.d\s+fa\d+,\s*fa\d+,\s*fa\d+',
            'fmul.d': r'\tfmul\.d\s+fa\d+,\s*fa\d+,\s*fa\d+',
            'fdiv.d': r'\tfdiv\.d\s+fa\d+,\s*fa\d+,\s*fa\d+',
        },
        'move': {
            'fmv.d': r'\tfmv\.d\s+fa\d+,\s*fa\d+',
        },
        'conversion': {
            'fcvt.w.d': r'\tfcvt\.w\.d\s+a\d+,\s*fa\d+',
            'fcvt.d.w': r'\tfcvt\.d\.w\s+fa\d+,\s*a\d+',
        },
        'comparison': {
            'fcmp.d': r'\tfcmp\.d\s+fa\d+,\s*fa\d+',
        },
    },
}


def run_zerocc_asm(source_file, target='x86_64', opt_level='-O3'):
    """Extract assembly from ZeroCC."""
    cmd = ['./build/bin/zerocc', '-S', opt_level, '--target', target, str(source_file), '-o', '-']
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            print(f"Error: ZeroCC failed: {result.stderr}")
            return None
        return result.stdout
    except Exception as e:
        print(f"Error: {e}")
        return None


def check_instructions(asm_content, target='x86_64'):
    """Check for SIMD instructions in assembly."""
    patterns = ISA_INSTRUCTIONS.get(target, ISA_INSTRUCTIONS['x86_64'])
    
    found = {category: {} for category in patterns}
    not_found = {category: [] for category in patterns}
    
    for category, instructions in patterns.items():
        for instr_name, pattern in instructions.items():
            if re.search(pattern, asm_content, re.IGNORECASE):
                found[category][instr_name] = True
            else:
                not_found[category].append(instr_name)
    
    return found, not_found


def print_report(source_file, target, found, not_found):
    """Print instruction coverage report."""
    print(f"\n{'='*70}")
    print(f"Assembly Instruction Coverage Report")
    print(f"{'='*70}\n")
    
    print(f"Source: {source_file}")
    print(f"Target: {target}\n")
    
    total_found = 0
    total_not_found = 0
    
    for category in found:
        found_count = len(found[category])
        not_found_count = len(not_found[category])
        total = found_count + not_found_count
        total_found += found_count
        total_not_found += not_found_count
        
        print(f"{category.upper()} ({found_count}/{total}):")
        
        if found[category]:
            for instr in found[category]:
                print(f"  ✓ {instr}")
        
        if not_found[category]:
            for instr in not_found[category][:5]:  # Show first 5
                print(f"  ✗ {instr}")
            if len(not_found[category]) > 5:
                print(f"  ... and {len(not_found[category]) - 5} more")
        
        print()
    
    print(f"{'='*70}")
    print(f"SUMMARY: {total_found} found, {total_not_found} not found")
    print(f"{'='*70}\n")


def main():
    parser = argparse.ArgumentParser(description='Check SIMD instruction coverage')
    parser.add_argument('source', help='C source file to compile')
    parser.add_argument('--target', default='x86_64', choices=['x86_64', 'aarch64', 'riscv64'],
                        help='Target ISA (default: x86_64)')
    parser.add_argument('-O', '--opt-level', default='-O3', help='Optimization level')
    args = parser.parse_args()
    
    source_file = Path(args.source)
    if not source_file.exists():
        print(f"Error: {source_file} not found", file=sys.stderr)
        sys.exit(1)
    
    # Extract assembly
    asm_content = run_zerocc_asm(source_file, args.target, args.opt_level)
    if asm_content is None:
        sys.exit(1)
    
    # Check instructions
    found, not_found = check_instructions(asm_content, args.target)
    
    # Print report
    print_report(source_file, args.target, found, not_found)


if __name__ == "__main__":
    main()
