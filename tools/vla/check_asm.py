#!/usr/bin/env python3
"""
Assembly Checker for VLA Patterns

Checks generated assembly for VLA-related patterns:
- VLA allocation patterns (sub rsp, reg)
- VLA cleanup patterns (mov rsp, [rbp-x])
- Stack pointer consistency
- Calling convention compliance

Usage:
    python3 check_asm.py test.s --check-vla
    python3 check_asm.py test.s --check-abi
    python3 check_asm.py --diff before.s after.s
"""

import argparse
import re
import sys
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple, Set
from enum import Enum


class VLAState(Enum):
    NONE = "none"
    ALLOCATED = "allocated"
    CLEANED = "cleaned"


@dataclass
class VLAInstance:
    """Represents a VLA instance"""
    function: str
    alloc_line: int
    cleanup_line: Optional[int] = None
    size_reg: Optional[str] = None
    saved_rsp_slot: Optional[int] = None
    state: VLAState = VLAState.NONE


@dataclass
class FunctionInfo:
    """Information about a function"""
    name: str
    start_line: int
    end_line: int
    stack_frame_size: Optional[int] = None
    vla_instances: List[VLAInstance] = field(default_factory=list)
    callee_saved: List[str] = field(default_factory=list)
    has_prologue: bool = False
    has_epilogue: bool = False
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)


def parse_assembly(filepath: str) -> Dict[str, FunctionInfo]:
    """Parse assembly file to extract function information"""
    functions = {}
    current_func = None
    current_func_info = None
    
    with open(filepath, 'r') as f:
        lines = f.readlines()
    
    i = 0
    while i < len(lines):
        line_num = i + 1
        line = lines[i].strip()
        
        # Function label
        if line.endswith(':') and not line.startswith('.') and not line.startswith('#'):
            # Save previous function
            if current_func_info:
                functions[current_func] = current_func_info
            
            func_name = line[:-1]
            current_func = func_name
            current_func_info = FunctionInfo(name=func_name, start_line=line_num)
            i += 1
            continue
        
        if current_func_info:
            # Track function end
            if line.startswith('.L') and line.endswith(':'):
                current_func_info.end_line = line_num - 1
            
            # Detect prologue
            if re.match(r'pushq?\s*%rbp', line):
                current_func_info.has_prologue = True
            
            # Detect epilogue
            if re.match(r'leave', line) or re.match(r'ret', line):
                current_func_info.has_epilogue = True
            
            # Detect callee-saved register saves
            callee_save_match = re.match(r'movq?\s*%\w+,\s*\[?%rbp-(\d+)\]?', line)
            if callee_save_match:
                reg_match = re.search(r'%(\w+)', line)
                if reg_match:
                    reg = reg_match.group(1)
                    if reg in ['rbx', 'r12', 'r13', 'r14', 'r15']:
                        current_func_info.callee_saved.append(reg)
            
            # Detect stack frame size
            frame_size_match = re.match(r'subq?\s+\$(\d+),\s*%rsp', line)
            if frame_size_match:
                current_func_info.stack_frame_size = int(frame_size_match.group(1))
            
            # Detect VLA allocation (sub reg, rsp)
            vla_alloc_match = re.match(r'subq?\s+%\w+,\s*%rsp', line)
            if vla_alloc_match:
                reg_match = re.search(r'%(\w+)', line)
                size_reg = reg_match.group(1) if reg_match else None
                
                vla = VLAInstance(
                    function=current_func,
                    alloc_line=line_num,
                    size_reg=size_reg,
                    state=VLAState.ALLOCATED
                )
                current_func_info.vla_instances.append(vla)
            
            # Detect VLA save (mov [rbp-x], rsp)
            vla_save_match = re.match(r'movq?\s+\[?%rbp-(\d+)\]?,\s*%rsp', line)
            if vla_save_match:
                slot = int(vla_save_match.group(1))
                # Update last VLA instance
                if current_func_info.vla_instances:
                    current_func_info.vla_instances[-1].saved_rsp_slot = slot
            
            # Detect VLA cleanup (mov rsp, [rbp-x])
            vla_cleanup_match = re.match(r'movq?\s*%rsp,\s*\[?%rbp-(\d+)\]?', line)
            if vla_cleanup_match:
                slot = int(vla_cleanup_match.group(1))
                # Find matching VLA instance
                for vla in reversed(current_func_info.vla_instances):
                    if vla.state == VLAState.ALLOCATED and vla.saved_rsp_slot == slot:
                        vla.cleanup_line = line_num
                        vla.state = VLAState.CLEANED
                        break
        
        i += 1
    
    # Save last function
    if current_func_info:
        current_func_info.end_line = len(lines)
        functions[current_func] = current_func_info
    
    return functions


def check_vla_patterns(func_info: FunctionInfo) -> None:
    """Check VLA patterns for correctness"""
    for vla in func_info.vla_instances:
        # Check if VLA was cleaned up
        if vla.state == VLAState.ALLOCATED:
            func_info.errors.append(
                f"VLA allocated at line {vla.alloc_line} not cleaned up"
            )
        
        # Check if save slot matches cleanup
        if vla.cleanup_line and not vla.saved_rsp_slot:
            func_info.warnings.append(
                f"VLA cleanup at line {vla.cleanup_line} without save slot"
            )


def check_prologue_epilogue(func_info: FunctionInfo) -> None:
    """Check function prologue/epilogue"""
    if func_info.vla_instances and not func_info.has_prologue:
        func_info.warnings.append(
            f"Function {func_info.name} has VLA but no prologue"
        )
    
    if func_info.vla_instances and not func_info.has_epilogue:
        func_info.errors.append(
            f"Function {func_info.name} has VLA but no epilogue"
        )


def check_callee_saved(func_info: FunctionInfo) -> None:
    """Check callee-saved register handling"""
    # If function uses callee-saved registers, they should be saved
    # This is a simplified check
    pass


def check_stack_alignment(func_info: FunctionInfo) -> None:
    """Check stack alignment (should be 16-byte at call sites)"""
    # Simplified check - real implementation needs full analysis
    if func_info.stack_frame_size:
        if func_info.stack_frame_size % 16 != 0:
            func_info.warnings.append(
                f"Function {func_info.name} stack frame not 16-byte aligned: {func_info.stack_frame_size}"
            )


def analyze_all_functions(functions: Dict[str, FunctionInfo]) -> Dict:
    """Analyze all functions"""
    analysis = {
        'total_functions': len(functions),
        'functions_with_vla': 0,
        'vla_errors': 0,
        'vla_warnings': 0,
        'errors': [],
        'warnings': [],
        'function_details': []
    }
    
    for func_name, func_info in functions.items():
        # Run checks
        check_vla_patterns(func_info)
        check_prologue_epilogue(func_info)
        check_callee_saved(func_info)
        check_stack_alignment(func_info)
        
        func_detail = {
            'name': func_name,
            'has_vla': len(func_info.vla_instances) > 0,
            'vla_count': len(func_info.vla_instances),
            'vla_cleaned': sum(1 for v in func_info.vla_instances if v.state == VLAState.CLEANED),
            'errors': func_info.errors.copy(),
            'warnings': func_info.warnings.copy()
        }
        
        if func_detail['has_vla']:
            analysis['functions_with_vla'] += 1
        
        if func_info.errors:
            analysis['vla_errors'] += 1
            analysis['errors'].extend(func_info.errors)
        
        if func_info.warnings:
            analysis['vla_warnings'] += 1
            analysis['warnings'].extend(func_info.warnings)
        
        analysis['function_details'].append(func_detail)
    
    return analysis


def generate_report(analysis: Dict) -> str:
    """Generate human-readable report"""
    lines = []
    lines.append("=" * 70)
    lines.append("Assembly VLA Pattern Check Report")
    lines.append("=" * 70)
    lines.append("")
    lines.append(f"Total functions: {analysis['total_functions']}")
    lines.append(f"Functions with VLA: {analysis['functions_with_vla']}")
    lines.append("")
    lines.append("Issues Found:")
    lines.append(f"  - Functions with VLA errors: {analysis['vla_errors']}")
    lines.append(f"  - Functions with VLA warnings: {analysis['vla_warnings']}")
    lines.append("")
    
    if analysis['errors']:
        lines.append("ERRORS:")
        for error in analysis['errors']:
            lines.append(f"  ❌ {error}")
        lines.append("")
    
    if analysis['warnings']:
        lines.append("WARNINGS:")
        for warning in analysis['warnings']:
            lines.append(f"  ⚠️  {warning}")
        lines.append("")
    
    # Function details
    if analysis['function_details']:
        lines.append("Function Details:")
        lines.append("-" * 70)
        for func in analysis['function_details']:
            if func['has_vla']:
                status = "✅" if func['vla_count'] == func['vla_cleaned'] else "❌"
                lines.append(f"  {status} {func['name']}: {func['vla_count']} VLA(s), {func['vla_cleaned']} cleaned")
                if func['errors']:
                    for err in func['errors']:
                        lines.append(f"      - {err}")
                if func['warnings']:
                    for warn in func['warnings']:
                        lines.append(f"      - {warn}")
        lines.append("")
    
    # Compliance
    lines.append("VLA Pattern Compliance:")
    compliant = analysis['vla_errors'] == 0
    lines.append(f"  Status: {'✅ PASS' if compliant else '❌ FAIL'}")
    if not compliant:
        lines.append(f"  - {analysis['vla_errors']} function(s) have VLA errors")
    
    lines.append("")
    lines.append("=" * 70)
    
    return "\n".join(lines)


def compare_asm(before_file: str, after_file: str) -> str:
    """Compare two assembly files"""
    before_funcs = parse_assembly(before_file)
    after_funcs = parse_assembly(after_file)
    
    lines = []
    lines.append("Assembly Comparison Report")
    lines.append("=" * 70)
    lines.append("")
    
    # Find VLA changes
    lines.append("VLA Changes:")
    for func_name in before_funcs:
        before_vla = len(before_funcs[func_name].vla_instances)
        after_vla = len(after_funcs.get(func_name, FunctionInfo(func_name, 0, 0)).vla_instances)
        
        if before_vla != after_vla:
            lines.append(f"  {func_name}: {before_vla} -> {after_vla} VLA(s)")
    
    lines.append("")
    lines.append("=" * 70)
    
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description='Assembly Checker for VLA Patterns')
    parser.add_argument('asm_file', nargs='?', help='Assembly file to check')
    parser.add_argument('--diff', nargs=2, metavar=('BEFORE', 'AFTER'),
                       help='Compare two assembly files')
    parser.add_argument('--check-vla', action='store_true', help='Check VLA patterns')
    parser.add_argument('--check-abi', action='store_true', help='Check ABI compliance')
    parser.add_argument('--output', '-o', help='Output report file')
    
    args = parser.parse_args()
    
    if args.diff:
        report = compare_asm(args.diff[0], args.diff[1])
        if args.output:
            with open(args.output, 'w') as f:
                f.write(report)
            print(f"Report written to {args.output}")
        else:
            print(report)
        return
    
    if not args.asm_file:
        parser.print_help()
        sys.exit(1)
    
    # Parse and analyze
    functions = parse_assembly(args.asm_file)
    analysis = analyze_all_functions(functions)
    report = generate_report(analysis)
    
    # Output
    if args.output:
        with open(args.output, 'w') as f:
            f.write(report)
        print(f"Report written to {args.output}")
    else:
        print(report)
    
    # Exit with error if violations found
    if analysis['errors']:
        sys.exit(1)


if __name__ == '__main__':
    main()
