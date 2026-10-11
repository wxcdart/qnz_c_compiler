#!/usr/bin/env python3
"""
VLA Stack Tracker

Tracks VLA stack allocations and deallocations to detect:
- Stack imbalance (rsp not restored)
- Missing cleanup on early returns
- Nested VLA violations
- Scope-based cleanup issues

Usage:
    python3 vla_stack_track.py --asm test.s --verify
    python3 vla_stack_track.py --ir test.ir --dump-ir zerocc test.c
"""

import argparse
import re
import sys
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Set, Tuple
from enum import Enum


class StackOp(Enum):
    SUB = "sub"
    ADD = "add"
    PUSH = "push"
    POP = "pop"
    LEAVE = "leave"
    MOV_RSP = "mov rsp"


@dataclass
class StackFrame:
    """Represents a stack frame state"""
    rsp_offset: int = 0
    vla_base: Optional[int] = None  # Saved rsp before VLA allocation
    vla_size: Optional[int] = None
    function: str = ""
    block_depth: int = 0


@dataclass
class VLAAllocation:
    """Represents a VLA allocation"""
    function: str
    size_expr: str
    alloc_site: int  # Instruction/line number
    cleanup_site: Optional[int] = None
    is_comptime: bool = False
    size_value: Optional[int] = None


@dataclass
class StackTrace:
    """Complete stack trace for a function"""
    function: str
    allocations: List[VLAAllocation] = field(default_factory=list)
    stack_ops: List[Tuple[int, StackOp, int]] = field(default_factory=list)  # (line, op, delta)
    early_returns: List[int] = field(default_factory=list)
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)


def parse_asm_file(filepath: str) -> Dict[str, StackTrace]:
    """Parse assembly file to track VLA stack operations"""
    traces = {}
    current_func = None
    current_trace = None
    
    with open(filepath, 'r') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            
            # Function label
            if line.startswith('.globl ') or line.startswith('globl '):
                continue
            
            if line.endswith(':') and not line.startswith('.') and not line.startswith('#'):
                # New function
                func_name = line[:-1]
                current_func = func_name
                current_trace = StackTrace(function=func_name)
                traces[func_name] = current_trace
                continue
            
            if current_trace:
                parse_asm_instruction(line, line_num, current_trace)
    
    return traces


def parse_asm_instruction(line: str, line_num: int, trace: StackTrace):
    """Parse a single assembly instruction"""
    # Stack pointer operations
    sub_match = re.match(r'subq?\s+\$?(-?\d+),?\s*%rsp', line)
    if sub_match:
        delta = -int(sub_match.group(1))
        trace.stack_ops.append((line_num, StackOp.SUB, delta))
        return
    
    add_match = re.match(r'addq?\s+\$?(\d+),?\s*%rsp', line)
    if add_match:
        delta = int(add_match.group(1))
        trace.stack_ops.append((line_num, StackOp.ADD, delta))
        return
    
    # VLA allocation pattern: sub rsp, reg (runtime size)
    vla_sub_match = re.match(r'subq?\s+%\w+,\s*%rsp', line)
    if vla_sub_match:
        trace.stack_ops.append((line_num, StackOp.SUB, 'vla'))
        return
    
    # VLA save pattern: mov [rbp-8], rsp
    vla_save_match = re.match(r'movq?\s+\[?%rbp-(\d+)\]?,\s*%rsp', line)
    if vla_save_match:
        offset = int(vla_save_match.group(1))
        trace.vla_base = offset
        return
    
    # VLA restore pattern: mov rsp, [rbp-8]
    vla_restore_match = re.match(r'movq?\s*%rsp,\s*\[?%rbp-(\d+)\]?', line)
    if vla_restore_match:
        offset = int(vla_restore_match.group(1))
        trace.stack_ops.append((line_num, StackOp.MOV_RSP, offset))
        return
    
    # Push/pop
    if re.match(r'pushq?\s*', line):
        trace.stack_ops.append((line_num, StackOp.PUSH, -8))
        return
    
    if re.match(r'popq?\s*', line):
        trace.stack_ops.append((line_num, StackOp.POP, 8))
        return
    
    # Leave (equivalent to pop rbp; mov rsp, rbp)
    if re.match(r'leave', line):
        trace.stack_ops.append((line_num, StackOp.LEAVE, 0))
        return
    
    # Early return detection
    if re.match(r'ret', line):
        trace.early_returns.append(line_num)


def analyze_stack_balance(trace: StackTrace) -> bool:
    """Analyze if stack is balanced at function exit"""
    total_delta = 0
    vla_allocs = []
    vla_restores = []
    
    for line_num, op, delta in trace.stack_ops:
        if op == StackOp.SUB:
            if delta == 'vla':
                vla_allocs.append(line_num)
                total_delta += 'vla'  # Special marker
            else:
                total_delta += delta
        elif op == StackOp.ADD:
            total_delta += delta
        elif op == StackOp.PUSH:
            total_delta += delta
        elif op == StackOp.POP:
            total_delta += delta
        elif op == StackOp.MOV_RSP:
            # VLA restore - save the offset
            vla_restores.append((line_num, delta))
    
    # Check for VLA cleanup
    if vla_allocs and not vla_restores:
        trace.errors.append(f"VLA allocated but not restored in {trace.function}")
        return False
    
    # Check stack balance
    if isinstance(total_delta, int) and total_delta != 0:
        trace.errors.append(f"Stack unbalanced in {trace.function}: delta={total_delta}")
        return False
    
    return True


def detect_nested_vla(trace: StackTrace) -> bool:
    """Detect nested VLA allocations"""
    vla_depth = 0
    max_depth = 0
    
    for line_num, op, delta in trace.stack_ops:
        if op == StackOp.SUB and delta == 'vla':
            vla_depth += 1
            max_depth = max(max_depth, vla_depth)
        elif op == StackOp.MOV_RSP:
            vla_depth -= 1
    
    if max_depth > 1:
        trace.warnings.append(f"Nested VLA detected in {trace.function} (depth={max_depth})")
        return True
    
    return False


def verify_comptime_vla(trace: StackTrace) -> bool:
    """Verify comptime VLA (should have constant stack allocation)"""
    vla_found = False
    for line_num, op, delta in trace.stack_ops:
        if op == StackOp.SUB:
            if delta == 'vla':
                vla_found = True
                # Runtime VLA - check if it's allowed
                trace.warnings.append(f"Runtime VLA in {trace.function} at line {line_num}")
                return False
            elif isinstance(delta, int) and delta < 0:
                # Comptime VLA (constant size)
                vla_found = True
    
    return True


def analyze_all_traces(traces: Dict[str, StackTrace]) -> Dict:
    """Analyze all function traces"""
    analysis = {
        'total_functions': len(traces),
        'functions_with_vla': 0,
        'stack_unbalanced': 0,
        'nested_vla': 0,
        'runtime_vla': 0,
        'comptime_vla': 0,
        'errors': [],
        'warnings': [],
        'function_details': []
    }
    
    for func_name, trace in traces.items():
        func_info = {
            'name': func_name,
            'vla_count': 0,
            'has_vla': False,
            'stack_balanced': True,
            'errors': [],
            'warnings': []
        }
        
        # Check for VLA
        has_runtime_vla = any(op == StackOp.SUB and delta == 'vla' 
                             for _, op, delta in trace.stack_ops)
        has_comptime_vla = any(op == StackOp.SUB and isinstance(delta, int) and delta < -64 
                              for _, op, delta in trace.stack_ops)
        
        if has_runtime_vla or has_comptime_vla:
            func_info['has_vla'] = True
            analysis['functions_with_vla'] += 1
        
        if has_runtime_vla:
            analysis['runtime_vla'] += 1
            func_info['vla_count'] += 1
        
        if has_comptime_vla:
            analysis['comptime_vla'] += 1
        
        # Analyze stack balance
        if not analyze_stack_balance(trace):
            analysis['stack_unbalanced'] += 1
            func_info['stack_balanced'] = False
            func_info['errors'].extend(trace.errors)
            analysis['errors'].extend(trace.errors)
        
        # Detect nested VLA
        if detect_nested_vla(trace):
            analysis['nested_vla'] += 1
        
        # Verify comptime VLA
        if not verify_comptime_vla(trace):
            func_info['warnings'].extend(trace.warnings)
            analysis['warnings'].extend(trace.warnings)
        
        analysis['function_details'].append(func_info)
    
    return analysis


def generate_report(analysis: Dict) -> str:
    """Generate human-readable report"""
    lines = []
    lines.append("=" * 70)
    lines.append("VLA Stack Trace Analysis Report")
    lines.append("=" * 70)
    lines.append("")
    lines.append(f"Total functions analyzed: {analysis['total_functions']}")
    lines.append(f"Functions with VLA: {analysis['functions_with_vla']}")
    lines.append(f"  - Runtime VLA: {analysis['runtime_vla']}")
    lines.append(f"  - Comptime VLA: {analysis['comptime_vla']}")
    lines.append("")
    lines.append("Issues Found:")
    lines.append(f"  - Stack unbalanced: {analysis['stack_unbalanced']}")
    lines.append(f"  - Nested VLA: {analysis['nested_vla']}")
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
                status = "✅" if func['stack_balanced'] else "❌"
                lines.append(f"  {status} {func['name']}: {func['vla_count']} VLA(s)")
                if func['errors']:
                    for err in func['errors']:
                        lines.append(f"      - {err}")
                if func['warnings']:
                    for warn in func['warnings']:
                        lines.append(f"      - {warn}")
        lines.append("")
    
    # Compliance check
    lines.append("Hybrid VLA Compliance:")
    compliant = (analysis['stack_unbalanced'] == 0 and 
                analysis['nested_vla'] == 0)
    lines.append(f"  Status: {'✅ PASS' if compliant else '❌ FAIL'}")
    if not compliant:
        if analysis['stack_unbalanced'] > 0:
            lines.append(f"  - {analysis['stack_unbalanced']} function(s) have unbalanced stack")
        if analysis['nested_vla'] > 0:
            lines.append(f"  - {analysis['nested_vla']} function(s) have nested VLA")
    
    lines.append("")
    lines.append("=" * 70)
    
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description='VLA Stack Tracker')
    parser.add_argument('--asm', help='Assembly file to analyze')
    parser.add_argument('--ir', help='IR file to analyze')
    parser.add_argument('--dump-ir', nargs=2, metavar=('COMPILER', 'SOURCE'),
                       help='Dump IR from compiler')
    parser.add_argument('--output', '-o', help='Output report file')
    parser.add_argument('--verify', action='store_true', help='Verify VLA compliance')
    
    args = parser.parse_args()
    
    if not args.asm and not args.ir:
        parser.print_help()
        sys.exit(1)
    
    # Parse assembly
    if args.asm:
        traces = parse_asm_file(args.asm)
        analysis = analyze_all_traces(traces)
        report = generate_report(analysis)
        
        if args.output:
            with open(args.output, 'w') as f:
                f.write(report)
            print(f"Report written to {args.output}")
        else:
            print(report)
    
    # TODO: Add IR analysis
    if args.ir:
        print("IR analysis not yet implemented")
        sys.exit(1)


if __name__ == '__main__':
    main()
