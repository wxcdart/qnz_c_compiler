#!/usr/bin/env python3
"""
Assembly Opcode Coverage Tracker
Tracks which IR opcodes have assembly emission implemented per ISA.

This tool scans the assembly emitter source code and tracks:
- Which IR opcodes are handled
- Which ISAs have implementations
- Which opcodes are missing

Usage:
    python3 tools/asm_opcode_coverage.py --target x86_64
    python3 tools/asm_opcode_coverage.py --target aarch64
    python3 tools/asm_opcode_coverage.py --target riscv64
"""

import argparse
import re
from pathlib import Path
from dataclasses import dataclass, field
from typing import Dict, List, Set


@dataclass
class OpcodeCoverage:
    """Coverage information for a single opcode."""
    opcode: str
    category: str
    x86_64_implemented: bool = False
    aarch64_implemented: bool = False
    riscv64_implemented: bool = False
    x86_64_location: str = ""
    aarch64_location: str = ""
    riscv64_location: str = ""


# IR opcode categories
OPCODE_CATEGORIES = {
    'data_movement': [
        'OP_MOV', 'OP_MOV_IMM', 'OP_MOV_REG', 'OP_MOV_MEM',
        'OP_LEA', 'OP_DEREF', 'OP_STORE_32', 'OP_STORE_64',
        'OP_LOAD', 'OP_LOAD_32', 'OP_LOAD_16', 'OP_LOAD_BYTE',
        'OP_STORE', 'OP_STORE_BYTE', 'OP_STORE_16',
    ],
    'arithmetic': [
        'OP_ADD', 'OP_SUB', 'OP_MUL', 'OP_DIV', 'OP_MOD',
        'OP_NEG', 'OP_INC', 'OP_DEC', 'OP_ADD_IMM', 'OP_SUB_IMM',
    ],
    'bitwise': [
        'OP_AND', 'OP_OR', 'OP_XOR', 'OP_NOT',
        'OP_SHL', 'OP_SHR', 'OP_SAR',
    ],
    'comparison': [
        'OP_CMP', 'OP_CMP_ZERO', 'OP_COND_SET',
        'OP_SETL', 'OP_SETLE', 'OP_SETG', 'OP_SETGE', 'OP_SETE', 'OP_SETNE',
    ],
    'control_flow': [
        'OP_JMP', 'OP_JE', 'OP_JNE', 'OP_JL', 'OP_JLE', 'OP_JG', 'OP_JGE',
        'OP_JCOND', 'OP_LABEL',
    ],
    'function': [
        'OP_CALL', 'OP_CALL_INDIRECT', 'OP_RET',
        'OP_PARAM_SAVE_INT', 'OP_PARAM_SAVE_FP',
        'OP_PROLOGUE', 'OP_EPILOGUE',
        'OP_CALLEE_SAVE_STORES', 'OP_CALLEE_SAVE_LOADS',
    ],
    'floating_point': [
        'OP_FADD', 'OP_FSUB', 'OP_FMUL', 'OP_FDIV',
        'OP_CVTSI2SD', 'OP_CVTTSD2SI', 'OP_CVTSI2SS', 'OP_CVTTSS2SI',
        'OP_FLOAD', 'OP_FSTORE',
    ],
    'simd': [
        'OP_VADDSS', 'OP_VSUBSS', 'OP_VMULSS', 'OP_VDIVSS',
        'OP_VADDSD', 'OP_VSUBSD', 'OP_VMULSD', 'OP_VDIVSD',
        'OP_VMOV', 'OP_VMOVSS', 'OP_VMOVSD',
        'OP_VLOAD', 'OP_VSTORE', 'OP_VLOADSS', 'OP_VLOADSD',
        'OP_VSTORESS', 'OP_VSTORESD',
    ],
    'special': [
        'OP_VLA_ALLOC', 'OP_LEA_RIP', 'OP_MOV_GLOBAL',
        'OP_ATOMIC_XADD', 'OP_ATOMIC_XCHG', 'OP_ATOMIC_CMPXCHG',
        'OP_MFENCE', 'OP_DEAD',
    ],
}


def scan_asm_emitter(source_dir: Path) -> Dict[str, Set[str]]:
    """Scan asm_emitter.c for opcode handling."""
    coverage = {}
    
    # Try multiple possible paths
    possible_paths = [
        source_dir / 'parser' / 'asm_emitter.c',
        source_dir / 'zero_cc' / 'parser' / 'asm_emitter.c',
    ]
    
    emitter_file = None
    for path in possible_paths:
        if path.exists():
            emitter_file = path
            break
    
    if not emitter_file:
        return coverage
    
    with open(emitter_file) as f:
        content = f.read()
    
    # Find all case statements
    case_pattern = r'case\s+(OP_\w+):'
    for match in re.finditer(case_pattern, content):
        opcode = match.group(1)
        if opcode not in coverage:
            coverage[opcode] = {'x86_64': [], 'aarch64': [], 'riscv64': []}
        
        # Get line number
        line_num = content[:match.start()].count('\n') + 1
        coverage[opcode]['x86_64'].append(line_num)
    
    return coverage


def scan_backend_files(source_dir: Path, isa: str) -> Dict[str, List[int]]:
    """Scan ISA backend files for assembly emission functions."""
    coverage = {}
    
    backend_file = source_dir / 'isa' / f'{isa}.c'
    if not backend_file.exists():
        return coverage
    
    with open(backend_file) as f:
        content = f.read()
    
    # Look for emit_*_asm functions
    func_pattern = r'static\s+void\s+(emit_\w+_asm)\s*\('
    for match in re.finditer(func_pattern, content):
        func_name = match.group(1)
        line_num = content[:match.start()].count('\n') + 1
        
        # Map function to opcode
        opcode_map = {
            'emit_mov_imm_asm': 'OP_MOV_IMM',
            'emit_mov_reg_asm': 'OP_MOV_REG',
            'emit_add_asm': 'OP_ADD',
            'emit_sub_asm': 'OP_SUB',
            'emit_mul_asm': 'OP_MUL',
            'emit_div_asm': 'OP_DIV',
            'emit_prologue_asm': 'OP_PROLOGUE',
            'emit_epilogue_asm': 'OP_EPILOGUE',
        }
        
        if func_name in opcode_map:
            opcode = opcode_map[func_name]
            if opcode not in coverage:
                coverage[opcode] = []
            coverage[opcode].append(line_num)
    
    return coverage


def build_coverage_report(source_dir: Path) -> List[OpcodeCoverage]:
    """Build coverage report for all opcodes."""
    report = []
    
    # Scan asm_emitter.c
    emitter_coverage = scan_asm_emitter(source_dir)
    
    # Scan backend files
    x86_64_coverage = scan_backend_files(source_dir, 'x86_64')
    aarch64_coverage = scan_backend_files(source_dir, 'aarch64')
    riscv64_coverage = scan_backend_files(source_dir, 'riscv')
    
    # Build report
    for category, opcodes in OPCODE_CATEGORIES.items():
        for opcode in opcodes:
            coverage = OpcodeCoverage(
                opcode=opcode,
                category=category,
                x86_64_implemented=opcode in emitter_coverage,
                aarch64_implemented=opcode in aarch64_coverage,
                riscv64_implemented=opcode in riscv64_coverage,
                x86_64_location=f":{emitter_coverage[opcode]['x86_64'][0]}" if opcode in emitter_coverage and emitter_coverage[opcode]['x86_64'] else "",
                aarch64_location=f":{aarch64_coverage[opcode][0]}" if opcode in aarch64_coverage else "",
                riscv64_location=f":{riscv64_coverage[opcode][0]}" if opcode in riscv64_coverage else "",
            )
            report.append(coverage)
    
    return report


def print_report(report: List[OpcodeCoverage], target_isa: str = None):
    """Print coverage report."""
    print(f"\n{'='*80}")
    print(f"Assembly Opcode Coverage Report")
    print(f"{'='*80}\n")
    
    # Group by category
    by_category = {}
    for cov in report:
        if cov.category not in by_category:
            by_category[cov.category] = []
        by_category[cov.category].append(cov)
    
    # Print each category
    total_implemented = 0
    total_opcodes = len(report)
    
    for category, opcodes in sorted(by_category.items()):
        print(f"\n{category.upper()} ({len(opcodes)} opcodes)")
        print("-" * 60)
        
        for cov in opcodes:
            if target_isa:
                # Show only target ISA
                if target_isa == 'x86_64':
                    implemented = cov.x86_64_implemented
                    loc = cov.x86_64_location
                elif target_isa == 'aarch64':
                    implemented = cov.aarch64_implemented
                    loc = cov.aarch64_location
                else:
                    implemented = cov.riscv64_implemented
                    loc = cov.riscv64_location
                
                status = "✓" if implemented else "✗"
                print(f"  {status} {cov.opcode:<25} {loc}")
                
                if implemented:
                    total_implemented += 1
            else:
                # Show all ISAs
                x86_status = "✓" if cov.x86_64_implemented else "✗"
                aarch_status = "✓" if cov.aarch64_implemented else "✗"
                riscv_status = "✓" if cov.riscv64_implemented else "✗"
                print(f"  {cov.opcode:<25} x86:{x86_status} aarch64:{aarch_status} riscv:{riscv_status}")
                
                if cov.x86_64_implemented:
                    total_implemented += 1
    
    # Summary
    print(f"\n{'='*80}")
    print(f"SUMMARY")
    print(f"{'='*80}")
    
    if target_isa:
        print(f"Target ISA: {target_isa}")
        print(f"Implemented: {total_implemented}/{total_opcodes} ({total_implemented/total_opcodes*100:.1f}%)")
    else:
        # Count per ISA
        x86_count = sum(1 for cov in report if cov.x86_64_implemented)
        aarch64_count = sum(1 for cov in report if cov.aarch64_implemented)
        riscv64_count = sum(1 for cov in report if cov.riscv64_implemented)
        
        print(f"x86-64:    {x86_count}/{total_opcodes} ({x86_count/total_opcodes*100:.1f}%)")
        print(f"AArch64:   {aarch64_count}/{total_opcodes} ({aarch64_count/total_opcodes*100:.1f}%)")
        print(f"RISC-V:    {riscv64_count}/{total_opcodes} ({riscv64_count/total_opcodes*100:.1f}%)")
    
    print()


def main():
    parser = argparse.ArgumentParser(description='Track assembly opcode coverage per ISA')
    parser.add_argument('--target', choices=['x86_64', 'aarch64', 'riscv64'],
                        help='Show coverage for specific ISA only')
    parser.add_argument('--source', '-s', default='src', help='Source directory')
    args = parser.parse_args()
    
    source_dir = Path(args.source)
    if not source_dir.exists():
        print(f"Error: {source_dir} not found", file=sys.stderr)
        sys.exit(1)
    
    # Build report
    report = build_coverage_report(source_dir)
    
    # Print report
    print_report(report, args.target)


if __name__ == "__main__":
    main()
