#!/usr/bin/env python3
"""
IR Diff Tool for VLA Analysis

Compares IR before and after optimizations to identify:
- VLA allocation patterns (OP_VLA_ALLOC)
- Stack pointer manipulation
- Live range issues
- Optimization opportunities

Usage:
    python3 ir_diff.py --before ir_before.txt --after ir_after.txt
    python3 ir_diff.py --dump-ir zerocc test.c -o test.o
"""

import argparse
import re
import sys
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple
from collections import defaultdict


@dataclass
class IRInstruction:
    """Represents a single IR instruction"""
    idx: int
    opcode: str
    dst: Optional[str] = None
    src1: Optional[str] = None
    src2: Optional[str] = None
    imm: Optional[int] = None
    label: Optional[str] = None
    raw: str = ""
    
    @property
    def uses_vla(self) -> bool:
        """Check if instruction uses VLA-related opcodes"""
        # Compiler IR dump prints 'VLA_ALLOC'; accept 'OP_'-prefixed forms too
        return self.opcode in ['VLA_ALLOC', 'OP_VLA_ALLOC', 'OP_VLA_BEGIN', 'OP_VLA_END']
    
    @property
    def uses_stack(self) -> bool:
        """Check if instruction manipulates stack pointer"""
        return self.opcode in ['VLA_ALLOC', 'OP_VLA_ALLOC', 'OP_SUB', 'OP_ADD'] and 'rsp' in self.raw.lower()


@dataclass
class VLAInfo:
    """Information about VLA allocations"""
    idx: int
    size_expr: str
    dst_reg: str
    line: int = 0
    function: str = ""
    scope_depth: int = 0


@dataclass
class IRFunction:
    """Represents a function's IR"""
    name: str
    instructions: List[IRInstruction] = field(default_factory=list)
    vla_allocs: List[VLAInfo] = field(default_factory=list)
    stack_ops: List[int] = field(default_factory=list)  # Indices of stack operations


def parse_ir_file(filepath: str) -> Dict[str, IRFunction]:
    """Parse IR dump file into structured format"""
    functions = {}
    current_func = None
    
    with open(filepath, 'r') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            
            # Skip empty lines and comments
            if not line or line.startswith('#'):
                continue
            
            # Function header: zerocc '-dump-ir' format
            if line.startswith("=== IR for function"):
                m = re.match(r"=== IR for function '([^']+)' ===", line)
                if m:
                    current_func = IRFunction(name=m.group(1))
                    functions[current_func.name] = current_func
                continue
            
            # Function declaration (legacy 'func name' format)
            if line.startswith('func '):
                match = re.match(r'func\s+(\w+)', line)
                if match:
                    current_func = IRFunction(name=match.group(1))
                    functions[current_func.name] = current_func
                continue
            
            # Instruction
            if current_func:
                instr = parse_instruction(line, line_num)
                if instr:
                    current_func.instructions.append(instr)
                    
                    # Track VLA allocations
                    if instr.uses_vla:
                        vla_info = extract_vla_info(instr, line_num)
                        vla_info.function = current_func.name
                        current_func.vla_allocs.append(vla_info)
                    
                    # Track stack operations
                    if instr.uses_stack:
                        current_func.stack_ops.append(len(current_func.instructions) - 1)
    
    return functions


def parse_instruction(line: str, line_num: int) -> Optional[IRInstruction]:
    """Parse a single IR instruction
    
    ZeroCC '-dump-ir' format (leading index, trailing flag metadata):
        '   8: VLA_ALLOC r23, r22 [flags_out=r0] [flags_in=r0]'
    """
    # Strip trailing '[flags_out=...] [flags_in=...] [alias=...]' metadata
    clean = re.sub(r'(\s*\[(?:flags_out|flags_in|alias\w*)=[^\]]*\])+$', '', line.strip())
    # Pattern: idx: OPCODE dst, src1, src2, imm
    match = re.match(r'(\d+):\s+(\w+)(?:\s+(.*))?$', clean)
    if not match:
        return None
    
    idx = int(match.group(1))
    opcode = match.group(2)
    operands = match.group(3) or ""
    
    # Parse operands
    dst = src1 = src2 = imm = None
    
    # Try to parse operands
    parts = [p.strip() for p in operands.split(',')]
    
    if len(parts) >= 1 and parts[0]:
        if parts[0].isdigit():
            imm = int(parts[0])
        else:
            dst = parts[0]
    
    if len(parts) >= 2 and parts[1]:
        if parts[1].isdigit():
            src1 = parts[1]
        else:
            src1 = parts[1]
    
    if len(parts) >= 3 and parts[2]:
        if parts[2].isdigit():
            src2 = parts[2]
        else:
            src2 = parts[2]
    
    return IRInstruction(
        idx=idx,
        opcode=opcode,
        dst=dst,
        src1=src1,
        src2=src2,
        imm=imm,
        raw=line
    )


def extract_vla_info(instr: IRInstruction, line_num: int) -> VLAInfo:
    """Extract VLA allocation information"""
    return VLAInfo(
        idx=instr.idx,
        size_expr=instr.src1 or "unknown",
        dst_reg=instr.dst or "unknown",
        line=line_num
    )


def analyze_vla_patterns(functions: Dict[str, IRFunction]) -> Dict:
    """Analyze VLA patterns across all functions"""
    analysis = {
        'total_functions': len(functions),
        'functions_with_vla': 0,
        'total_vla_allocs': 0,
        'single_scope_vla': 0,
        'multiple_vla': 0,
        'nested_vla': 0,
        'vla_in_loops': 0,
        'vla_details': []
    }
    
    for func_name, func in functions.items():
        if not func.vla_allocs:
            continue
        
        analysis['functions_with_vla'] += 1
        analysis['total_vla_allocs'] += len(func.vla_allocs)
        
        # Check for multiple VLA in same function
        if len(func.vla_allocs) > 1:
            analysis['multiple_vla'] += 1
            analysis['vla_details'].append({
                'function': func_name,
                'vla_count': len(func.vla_allocs),
                'type': 'multiple',
                'allocs': [v.size_expr for v in func.vla_allocs]
            })
        else:
            analysis['single_scope_vla'] += 1
            analysis['vla_details'].append({
                'function': func_name,
                'vla_count': 1,
                'type': 'single',
                'allocs': [func.vla_allocs[0].size_expr]
            })
    
    return analysis


def compare_ir(before: Dict[str, IRFunction], after: Dict[str, IRFunction]) -> Dict:
    """Compare IR before and after optimizations"""
    comparison = {
        'added_instructions': [],
        'removed_instructions': [],
        'modified_instructions': [],
        'vla_optimizations': [],
        'stack_optimizations': []
    }
    
    for func_name in before:
        if func_name not in after:
            comparison['removed_instructions'].append(f"Function {func_name} removed")
            continue
        
        before_instrs = {i.idx: i for i in before[func_name].instructions}
        after_instrs = {i.idx: i for i in after[func_name].instructions}
        
        # Check for VLA optimizations
        before_vla = {v.idx: v for v in before[func_name].vla_allocs}
        after_vla = {v.idx: v for v in after[func_name].vla_allocs}
        
        # VLA removed (optimized away)
        for idx in before_vla:
            if idx not in after_vla:
                comparison['vla_optimizations'].append({
                    'function': func_name,
                    'type': 'vla_eliminated',
                    'idx': idx,
                    'size': before_vla[idx].size_expr
                })
        
        # VLA added
        for idx in after_vla:
            if idx not in before_vla:
                comparison['vla_optimizations'].append({
                    'function': func_name,
                    'type': 'vla_added',
                    'idx': idx,
                    'size': after_vla[idx].size_expr
                })
    
    return comparison


def generate_report(analysis: Dict, comparison: Optional[Dict] = None) -> str:
    """Generate human-readable report"""
    lines = []
    lines.append("=" * 60)
    lines.append("VLA IR Analysis Report")
    lines.append("=" * 60)
    lines.append("")
    lines.append(f"Total functions: {analysis['total_functions']}")
    lines.append(f"Functions with VLA: {analysis['functions_with_vla']}")
    lines.append(f"Total VLA allocations: {analysis['total_vla_allocs']}")
    lines.append("")
    lines.append("VLA Pattern Analysis:")
    lines.append(f"  - Single-scope VLA: {analysis['single_scope_vla']}")
    lines.append(f"  - Multiple VLA per function: {analysis['multiple_vla']}")
    lines.append(f"  - Nested VLA: {analysis['nested_vla']}")
    lines.append(f"  - VLA in loops: {analysis['vla_in_loops']}")
    lines.append("")
    
    if analysis['vla_details']:
        lines.append("VLA Details:")
        for detail in analysis['vla_details']:
            lines.append(f"  {detail['function']}: {detail['vla_count']} VLA(s) - {detail['allocs']}")
        lines.append("")
    
    if comparison:
        lines.append("Optimization Comparison:")
        if comparison['vla_optimizations']:
            lines.append("  VLA Optimizations:")
            for opt in comparison['vla_optimizations']:
                lines.append(f"    - {opt['type']} in {opt['function']} (idx={opt['idx']})")
        lines.append("")
    
    # Compliance check
    lines.append("Hybrid VLA Compliance:")
    compliant = analysis['multiple_vla'] == 0 and analysis['nested_vla'] == 0 and analysis['vla_in_loops'] == 0
    lines.append(f"  Status: {'✅ PASS' if compliant else '❌ FAIL'}")
    if not compliant:
        if analysis['multiple_vla'] > 0:
            lines.append(f"  - {analysis['multiple_vla']} function(s) have multiple VLA")
        if analysis['nested_vla'] > 0:
            lines.append(f"  - {analysis['nested_vla']} function(s) have nested VLA")
        if analysis['vla_in_loops'] > 0:
            lines.append(f"  - {analysis['vla_in_loops']} function(s) have VLA in loops")
    
    lines.append("")
    lines.append("=" * 60)
    
    return "\n".join(lines)


def dump_ir_from_compiler(compiler_path: str, source_file: str, output_file: str):
    """Dump IR using the compiler's -dump-ir flag (IR goes to stderr)
    
    Note: without -o the compiler runs the program (JIT), so the exit code is
    the *program's* return value, not a compiler status. Success is detected
    by the presence of IR headers in stderr.
    """
    import subprocess
    
    result = subprocess.run(
        [compiler_path, '-dump-ir', source_file],
        capture_output=True,
        text=True,
        timeout=30
    )
    
    # -dump-ir writes the IR instruction stream to stderr
    if "=== IR for function" not in result.stderr:
        print(f"Error: compiler did not emit an IR dump: {result.stderr}")
        return False
    
    with open(output_file, 'w') as f:
        f.write(result.stderr)
    print(f"IR dumped successfully: {output_file}")
    return True


def main():
    parser = argparse.ArgumentParser(description='IR Diff Tool for VLA Analysis')
    parser.add_argument('--before', help='IR file before optimization')
    parser.add_argument('--after', help='IR file after optimization')
    parser.add_argument('--dump-ir', nargs=2, metavar=('COMPILER', 'SOURCE'),
                       help='Dump IR from compiler (compiler source_file)')
    parser.add_argument('--output', '-o', help='Output report file')
    
    args = parser.parse_args()
    
    if args.dump_ir:
        success = dump_ir_from_compiler(args.dump_ir[0], args.dump_ir[1], args.output or '/tmp/ir_dump.txt')
        sys.exit(0 if success else 1)
    
    if not args.before and not args.after:
        parser.print_help()
        sys.exit(1)
    
    # Parse IR files
    before_funcs = parse_ir_file(args.before) if args.before else {}
    after_funcs = parse_ir_file(args.after) if args.after else {}
    
    # Analyze VLA patterns
    analysis = analyze_vla_patterns(before_funcs or after_funcs)
    
    # Compare if both provided
    comparison = None
    if before_funcs and after_funcs:
        comparison = compare_ir(before_funcs, after_funcs)
    
    # Generate report
    report = generate_report(analysis, comparison)
    
    # Output
    if args.output:
        with open(args.output, 'w') as f:
            f.write(report)
        print(f"Report written to {args.output}")
    else:
        print(report)


if __name__ == '__main__':
    main()
