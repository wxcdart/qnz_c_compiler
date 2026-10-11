#!/usr/bin/env python3
"""
VLA Optimization Verifier

Verifies that VLA optimizations are correct:
- Stack pointer is properly restored
- VLA cleanup happens on all paths
- No memory leaks (stack imbalance)
- Hybrid VLA rules are followed

Usage:
    python3 verify_opt.py --asm test.s --verify-hybrid
    python3 verify_opt.py --test test.c --compile zerocc
"""

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Set, Tuple
from collections import defaultdict


@dataclass
class CFGNode:
    """Control flow graph node"""
    label: str
    instructions: List[str] = field(default_factory=list)
    successors: List[str] = field(default_factory=list)
    predecessors: List[str] = field(default_factory=list)
    has_return: bool = False
    vla_alloc: Optional[int] = None  # Line number of VLA alloc
    vla_cleanup: Optional[int] = None  # Line number of VLA cleanup


@dataclass
class FunctionCFG:
    """CFG for a single function"""
    name: str
    nodes: Dict[str, CFGNode] = field(default_factory=dict)
    entry: str = ""
    vla_allocs: List[Tuple[str, int]] = field(default_factory=list)  # (node, line)
    vla_cleanups: List[Tuple[str, int]] = field(default_factory=list)
    returns: List[str] = field(default_factory=list)  # Nodes with returns


def parse_asm_to_cfg(filepath: str) -> Dict[str, FunctionCFG]:
    """Parse assembly to CFG"""
    functions = {}
    current_func = None
    current_cfg = None
    current_node = None
    node_labels = {}
    
    with open(filepath, 'r') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            
            # Function label
            if line.endswith(':') and not line.startswith('.') and not line.startswith('#'):
                # Save previous function
                if current_cfg:
                    functions[current_func] = current_cfg
                
                func_name = line[:-1]
                current_func = func_name
                current_cfg = FunctionCFG(name=func_name)
                current_node = None
                continue
            
            if current_cfg:
                # Node label
                if line.endswith(':') and line.startswith('.L'):
                    label = line[:-1]
                    current_node = CFGNode(label=label)
                    current_cfg.nodes[label] = current_node
                    node_labels[line_num] = label
                    if not current_cfg.entry:
                        current_cfg.entry = label
                    continue
                
                # Function entry (first instruction after label)
                if not current_node and line and not line.startswith('.'):
                    current_node = CFGNode(label=f"{current_func}_entry")
                    current_cfg.nodes[current_node.label] = current_node
                    current_cfg.entry = current_node.label
                
                if current_node:
                    current_node.instructions.append(line)
                    
                    # Detect return
                    if re.match(r'ret', line):
                        current_node.has_return = True
                        current_cfg.returns.append(current_node.label)
                    
                    # Detect VLA allocation (sub reg, rsp)
                    if re.match(r'subq?\s+%\w+,\s*%rsp', line):
                        current_cfg.vla_allocs.append((current_node.label, line_num))
                        current_node.vla_alloc = line_num
                    
                    # Detect VLA cleanup (mov rsp, [rbp-x])
                    if re.match(r'movq?\s*%rsp,\s*\[?%rbp-\d+\]?', line):
                        current_cfg.vla_cleanups.append((current_node.label, line_num))
                        current_node.vla_cleanup = line_num
    
    # Save last function
    if current_cfg:
        functions[current_func] = current_cfg
    
    return functions


def build_cfg_edges(cfg: FunctionCFG) -> None:
    """Build CFG edges from instructions"""
    node_list = list(cfg.nodes.values())
    
    for i, node in enumerate(node_list):
        # Find successors
        for instr in node.instructions:
            # Conditional jump
            jump_match = re.match(r'j(\w+)\s+(\w+)', instr)
            if jump_match:
                cond = jump_match.group(1)
                target = jump_match.group(2)
                
                if target in cfg.nodes:
                    node.successors.append(target)
                    cfg.nodes[target].predecessors.append(node.label)
                
                # If not unconditional, add next node
                if i + 1 < len(node_list):
                    next_node = node_list[i + 1]
                    if next_node.label not in node.successors:
                        node.successors.append(next_node.label)
                        next_node.predecessors.append(node.label)
                break
            elif re.match(r'jmp\s+(\w+)', instr):
                # Unconditional jump
                target = re.match(r'jmp\s+(\w+)', instr).group(1)
                if target in cfg.nodes:
                    node.successors.append(target)
                    cfg.nodes[target].predecessors.append(node.label)
                break
            elif re.match(r'ret', instr):
                # Return - no successors
                break
        else:
            # No jump found, add next node
            if i + 1 < len(node_list):
                next_node = node_list[i + 1]
                node.successors.append(next_node.label)
                next_node.predecessors.append(node.label)


def verify_vla_cleanup(cfg: FunctionCFG) -> List[str]:
    """Verify VLA cleanup on all paths"""
    errors = []
    
    if not cfg.vla_allocs:
        return errors
    
    # For each VLA allocation, check all paths reach cleanup
    for alloc_node, alloc_line in cfg.vla_allocs:
        alloc_node_obj = cfg.nodes.get(alloc_node)
        if not alloc_node_obj:
            continue
        
        # Find all return nodes
        return_nodes = [cfg.nodes[r] for r in cfg.returns if r in cfg.nodes]
        
        # Check if each return path has cleanup
        for ret_node in return_nodes:
            # Simple check: is there a cleanup between alloc and return?
            # This is a simplified check - real implementation needs full path analysis
            has_cleanup = any(cleanup_node == ret_node.label or 
                            cleanup_node in ret_node.predecessors
                            for cleanup_node, _ in cfg.vla_cleanups)
            
            if not has_cleanup:
                errors.append(
                    f"VLA allocated at line {alloc_line} in {cfg.name} may not be cleaned up before return"
                )
    
    return errors


def verify_stack_balance(cfg: FunctionCFG) -> List[str]:
    """Verify stack is balanced at all returns"""
    errors = []
    
    # Calculate stack delta for each node
    node_deltas = {}
    for node_label, node in cfg.nodes.items():
        delta = 0
        for instr in node.instructions:
            # sub rsp, imm
            sub_match = re.match(r'subq?\s+\$?(-?\d+),?\s*%rsp', instr)
            if sub_match:
                delta += int(sub_match.group(1))
            
            # add rsp, imm
            add_match = re.match(r'addq?\s+\$?(\d+),?\s*%rsp', instr)
            if add_match:
                delta += int(add_match.group(1))
            
            # push
            if re.match(r'pushq?\s*', instr):
                delta -= 8
            
            # pop
            if re.match(r'popq?\s*', instr):
                delta += 8
        
        node_deltas[node_label] = delta
    
    # Check returns
    for ret_label in cfg.returns:
        ret_node = cfg.nodes.get(ret_label)
        if not ret_node:
            continue
        
        # Calculate total delta to this return
        total_delta = 0
        visited = set()
        queue = [cfg.entry]
        
        while queue:
            current = queue.pop(0)
            if current in visited:
                continue
            visited.add(current)
            
            if current == ret_label:
                total_delta += node_deltas.get(current, 0)
                break
            
            total_delta += node_deltas.get(current, 0)
            queue.extend(cfg.nodes[current].successors)
        
        if total_delta != 0:
            errors.append(
                f"Stack unbalanced at return in {cfg.name}: delta={total_delta}"
            )
    
    return errors


def verify_hybrid_vla_rules(cfg: FunctionCFG) -> List[str]:
    """Verify hybrid VLA rules"""
    errors = []
    warnings = []
    
    # Rule 1: Only one VLA per function
    if len(cfg.vla_allocs) > 1:
        errors.append(
            f"Multiple VLA allocations in {cfg.name} ({len(cfg.vla_allocs)} found, max 1 allowed)"
        )
    
    # Rule 2: VLA must be at function scope (not in nested blocks)
    # This is hard to detect from assembly alone
    # We'll check if VLA is in the entry node
    for alloc_node, alloc_line in cfg.vla_allocs:
        if alloc_node != cfg.entry and not alloc_node.endswith('_entry'):
            warnings.append(
                f"VLA at line {alloc_line} in {cfg.name} may not be at function scope"
            )
    
    return errors, warnings


def analyze_all_functions(functions: Dict[str, FunctionCFG]) -> Dict:
    """Analyze all functions"""
    analysis = {
        'total_functions': len(functions),
        'functions_with_vla': 0,
        'vla_cleanup_errors': 0,
        'stack_unbalanced': 0,
        'hybrid_violations': 0,
        'errors': [],
        'warnings': [],
        'function_details': []
    }
    
    for func_name, cfg in functions.items():
        # Build CFG edges
        build_cfg_edges(cfg)
        
        func_info = {
            'name': func_name,
            'has_vla': len(cfg.vla_allocs) > 0,
            'vla_count': len(cfg.vla_allocs),
            'errors': [],
            'warnings': []
        }
        
        if func_info['has_vla']:
            analysis['functions_with_vla'] += 1
        
        # Verify VLA cleanup
        cleanup_errors = verify_vla_cleanup(cfg)
        func_info['errors'].extend(cleanup_errors)
        analysis['errors'].extend(cleanup_errors)
        if cleanup_errors:
            analysis['vla_cleanup_errors'] += 1
        
        # Verify stack balance
        balance_errors = verify_stack_balance(cfg)
        func_info['errors'].extend(balance_errors)
        analysis['errors'].extend(balance_errors)
        if balance_errors:
            analysis['stack_unbalanced'] += 1
        
        # Verify hybrid rules
        hybrid_errors, hybrid_warnings = verify_hybrid_vla_rules(cfg)
        func_info['errors'].extend(hybrid_errors)
        func_info['warnings'].extend(hybrid_warnings)
        analysis['errors'].extend(hybrid_errors)
        analysis['warnings'].extend(hybrid_warnings)
        if hybrid_errors or hybrid_warnings:
            analysis['hybrid_violations'] += 1
        
        analysis['function_details'].append(func_info)
    
    return analysis


def generate_report(analysis: Dict) -> str:
    """Generate human-readable report"""
    lines = []
    lines.append("=" * 70)
    lines.append("VLA Optimization Verification Report")
    lines.append("=" * 70)
    lines.append("")
    lines.append(f"Total functions: {analysis['total_functions']}")
    lines.append(f"Functions with VLA: {analysis['functions_with_vla']}")
    lines.append("")
    lines.append("Issues Found:")
    lines.append(f"  - VLA cleanup errors: {analysis['vla_cleanup_errors']}")
    lines.append(f"  - Stack unbalanced: {analysis['stack_unbalanced']}")
    lines.append(f"  - Hybrid VLA violations: {analysis['hybrid_violations']}")
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
    
    # Compliance
    lines.append("Hybrid VLA Compliance:")
    compliant = (analysis['vla_cleanup_errors'] == 0 and 
                analysis['stack_unbalanced'] == 0 and
                analysis['hybrid_violations'] == 0)
    lines.append(f"  Status: {'✅ PASS' if compliant else '❌ FAIL'}")
    if not compliant:
        if analysis['vla_cleanup_errors'] > 0:
            lines.append(f"  - {analysis['vla_cleanup_errors']} function(s) have VLA cleanup errors")
        if analysis['stack_unbalanced'] > 0:
            lines.append(f"  - {analysis['stack_unbalanced']} function(s) have unbalanced stack")
        if analysis['hybrid_violations'] > 0:
            lines.append(f"  - {analysis['hybrid_violations']} function(s) violate hybrid rules")
    
    lines.append("")
    lines.append("=" * 70)
    
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description='VLA Optimization Verifier')
    parser.add_argument('--asm', help='Assembly file to verify')
    parser.add_argument('--ir', help='IR file to verify')
    parser.add_argument('--test', help='Test C file')
    parser.add_argument('--compile', help='Compiler path for compilation')
    parser.add_argument('--output', '-o', help='Output report file')
    parser.add_argument('--verify-hybrid', action='store_true', help='Verify hybrid VLA rules')
    
    args = parser.parse_args()
    
    if not args.asm and not args.ir and not args.test:
        parser.print_help()
        sys.exit(1)
    
    # Compile if test file provided
    asm_file = args.asm
    if args.test and args.compile:
        import tempfile
        import os
        
        with tempfile.NamedTemporaryFile(mode='w', suffix='.s', delete=False) as f:
            asm_file = f.name
        
        result = subprocess.run(
            [args.compile, '-S', args.test, '-o', asm_file],
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            print(f"Compilation failed: {result.stderr}")
            sys.exit(1)
    
    if not asm_file or not os.path.exists(asm_file):
        print("Error: Assembly file not found")
        sys.exit(1)
    
    # Parse and analyze
    functions = parse_asm_to_cfg(asm_file)
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
