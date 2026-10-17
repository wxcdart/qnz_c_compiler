#!/usr/bin/env python3
"""
IR Analyzer - Comprehensive IR analysis and verification tool

This tool parses IR dumps and performs various analyses:
- Instruction flow analysis
- Control flow graph construction
- Data flow analysis
- Flag dependency tracking
- Optimization opportunities detection
- ISA-specific pattern detection

Usage:
    python3 ir_analyzer.py --dump ir_dump.txt
    python3 ir_analyzer.py --dump ir_dump.txt --analyze all
    python3 ir_analyzer.py --dump ir_dump.txt --flag-analysis
    python3 ir_analyzer.py --dump ir_dump.txt --cse-analysis
"""

import argparse
import re
import sys
import json
from dataclasses import dataclass, field
from typing import List, Dict, Set, Optional, Tuple
from collections import defaultdict
from pathlib import Path

@dataclass
class IRInstruction:
    """Represents a single IR instruction"""
    index: int
    op: str
    temp_dest: Optional[int] = None
    temp_src1: Optional[int] = None
    temp_src2: Optional[int] = None
    cond: Optional[int] = None
    flags_out: Optional[int] = None
    flags_in: Optional[int] = None
    imm: Optional[int] = None
    imm64: Optional[int] = None
    label_idx: Optional[int] = None
    raw_line: str = ""

@dataclass
class BasicBlock:
    """Represents a basic block in the control flow graph"""
    label: int
    start_idx: int
    end_idx: int
    instructions: List[IRInstruction] = field(default_factory=list)
    successors: List[int] = field(default_factory=list)
    predecessors: List[int] = field(default_factory=list)

@dataclass
class FlagDependency:
    """Represents a flag dependency relationship"""
    producer_idx: int
    producer_op: str
    consumer_idx: int
    consumer_op: str
    is_live: bool = True

@dataclass
class IRAnalysis:
    """Complete IR analysis result"""
    instructions: List[IRInstruction] = field(default_factory=list)
    basic_blocks: List[BasicBlock] = field(default_factory=list)
    flag_dependencies: List[FlagDependency] = field(default_factory=list)
    issues: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    stats: Dict = field(default_factory=dict)

class IRParser:
    """Parses IR dump output"""
    
    def __init__(self, dump_text: str):
        self.dump_text = dump_text
        self.instructions: List[IRInstruction] = []
        
    def parse(self) -> List[IRInstruction]:
        """Parse IR dump into instruction list"""
        lines = self.dump_text.strip().split('\n')
        current_idx = 0
        
        for line in lines:
            line = line.strip()
            
            # Skip header/footer lines
            if not line or line.startswith('IR Dump') or line.startswith('===') or line.startswith('---'):
                continue
            
            # Parse instruction
            instr = self._parse_instruction(line, current_idx)
            if instr:
                self.instructions.append(instr)
                current_idx += 1
        
        return self.instructions
    
    def _parse_instruction(self, line: str, idx: int) -> Optional[IRInstruction]:
        """Parse a single instruction line"""
        
        # Label
        label_match = re.match(r'^L(\d+):$', line)
        if label_match:
            return IRInstruction(
                index=idx,
                op='LABEL',
                label_idx=int(label_match.group(1)),
                raw_line=line
            )
        
        # Instruction: "  N: OP ..."
        instr_match = re.match(r'^\s*(\d+):\s+(\w+)\s+(.*)$', line)
        if not instr_match:
            return None
        
        op = instr_match.group(2)
        rest = instr_match.group(3)
        
        # Parse instruction-specific fields
        instr = IRInstruction(index=idx, op=op, raw_line=line)
        
        # MOV rDst, rSrc
        mov_match = re.match(r'r(\d+),\s*\[flags\]?\s*r(\d+)', rest)
        if mov_match and op == 'MOV':
            instr.temp_dest = int(mov_match.group(1))
            instr.temp_src1 = int(mov_match.group(2))
            if '[flags]' in rest:
                instr.flags_in = 0xFE  # REG_FLAGS
        
        # ADD rDst, rSrc1, rSrc2
        add_match = re.match(r'r(\d+),\s*r(\d+),\s*r(\d+)', rest)
        if add_match and op in ['ADD', 'SUB', 'AND', 'OR', 'XOR', 'SHL', 'SHR', 'SAR', 'MUL']:
            instr.temp_dest = int(add_match.group(1))
            instr.temp_src1 = int(add_match.group(2))
            instr.temp_src2 = int(add_match.group(3))
        
        # CMP rSrc1, [flags] rSrc2
        cmp_match = re.match(r'r(\d+),\s*\[flags\]?\s*r(\d+)', rest)
        if cmp_match and op == 'CMP':
            instr.temp_src1 = int(cmp_match.group(1))
            instr.temp_src2 = int(cmp_match.group(2))
            if '[flags]' in rest:
                instr.flags_out = 0xFE  # REG_FLAGS
        
        # COND_SET rDst, cc_N
        cond_set_match = re.match(r'r(\d+),\s*cc_(\d+)', rest)
        if cond_set_match and op == 'COND_SET':
            instr.temp_dest = int(cond_set_match.group(1))
            instr.cond = int(cond_set_match.group(2))
            instr.flags_in = 0xFE  # REG_FLAGS
        
        # JCOND cc_N, LLabel
        jcond_match = re.match(r'cc_(\d+),\s*L(\d+)', rest)
        if jcond_match and op == 'JCOND':
            instr.cond = int(jcond_match.group(1))
            instr.label_idx = int(jcond_match.group(2))
            instr.flags_in = 0xFE  # REG_FLAGS
        
        # JE rCond, LLabel
        je_match = re.match(r'r(\d+),\s*L(\d+)', rest)
        if je_match and op == 'JE':
            instr.temp_src1 = int(je_match.group(1))
            instr.label_idx = int(je_match.group(2))
        
        # JMP LLabel
        jmp_match = re.match(r'L(\d+)', rest)
        if jmp_match and op == 'JMP':
            instr.label_idx = int(jmp_match.group(1))
        
        # LOAD rDst, [sp-N]
        load_match = re.match(r'r(\d+),\s*\[sp-(\d+)\]', rest)
        if load_match and op == 'LOAD':
            instr.temp_dest = int(load_match.group(1))
            instr.imm = int(load_match.group(2))
        
        # STORE [sp-N], rSrc
        store_match = re.match(r'\[sp-(\d+)\],\s*r(\d+)', rest)
        if store_match and op == 'STORE':
            instr.imm = int(store_match.group(1))
            instr.temp_src1 = int(store_match.group(2))
        
        # LOAD_IMM rDst, N
        load_imm_match = re.match(r'r(\d+),\s*(-?\d+)', rest)
        if load_imm_match and op == 'LOAD_IMM':
            instr.temp_dest = int(load_imm_match.group(1))
            instr.imm = int(load_imm_match.group(2))
        
        # CALL fn_N
        call_match = re.match(r'fn_(\d+)', rest)
        if call_match and op == 'CALL':
            instr.label_idx = int(call_match.group(1))
        
        # Flags info
        flags_match = re.search(r'\[flags_out=r(\d+)\]', rest)
        if flags_match:
            instr.flags_out = int(flags_match.group(1))
        
        flags_in_match = re.search(r'\[flags_in=r(\d+)\]', rest)
        if flags_in_match:
            instr.flags_in = int(flags_in_match.group(1))
        
        return instr


class CFGBuilder:
    """Builds control flow graph from IR"""
    
    def __init__(self, instructions: List[IRInstruction]):
        self.instructions = instructions
        self.basic_blocks: List[BasicBlock] = []
        
    def build(self) -> List[BasicBlock]:
        """Build basic blocks and control flow graph"""
        if not self.instructions:
            return []
        
        # Find block boundaries (labels and jump targets)
        block_starts: Set[int] = {0}  # First instruction is always a block start
        
        for i, instr in enumerate(self.instructions):
            if instr.op == 'LABEL':
                block_starts.add(i)
            elif instr.op in ['JMP', 'JMP_LABEL']:
                if instr.label_idx is not None:
                    block_starts.add(instr.label_idx)
            elif instr.op in ['JE', 'JNE', 'JCOND']:
                if instr.label_idx is not None:
                    block_starts.add(instr.label_idx)
                block_starts.add(i + 1)  # Fall-through
        
        # Sort block starts
        sorted_starts = sorted(block_starts)
        
        # Create basic blocks
        for j in range(len(sorted_starts)):
            start_idx = sorted_starts[j]
            end_idx = sorted_starts[j + 1] - 1 if j + 1 < len(sorted_starts) else len(self.instructions) - 1
            
            # Find label for this block
            label = start_idx
            for instr in self.instructions[start_idx:end_idx + 1]:
                if instr.op == 'LABEL':
                    label = instr.label_idx
                    break
            
            block = BasicBlock(
                label=label,
                start_idx=start_idx,
                end_idx=end_idx,
                instructions=self.instructions[start_idx:end_idx + 1]
            )
            
            # Find successors
            last_instr = self.instructions[end_idx] if end_idx < len(self.instructions) else None
            
            if last_instr:
                if last_instr.op == 'JMP':
                    block.successors.append(last_instr.label_idx)
                elif last_instr.op == 'JMP_LABEL':
                    block.successors.append(last_instr.label_idx)
                elif last_instr.op in ['JE', 'JNE', 'JCOND']:
                    block.successors.append(last_instr.label_idx)  # Branch target
                    if end_idx + 1 < len(self.instructions):
                        block.successors.append(end_idx + 1)  # Fall-through
                elif last_instr.op != 'RET':
                    if end_idx + 1 < len(self.instructions):
                        block.successors.append(end_idx + 1)  # Fall-through
            
            self.basic_blocks.append(block)
        
        # Build predecessor links
        for block in self.basic_blocks:
            for succ_idx in block.successors:
                for other_block in self.basic_blocks:
                    if other_block.start_idx == succ_idx:
                        other_block.predecessors.append(block.label)
        
        return self.basic_blocks


class FlagDependencyAnalyzer:
    """Analyzes flag dependencies in IR"""
    
    def __init__(self, instructions: List[IRInstruction]):
        self.instructions = instructions
        self.dependencies: List[FlagDependency] = []
        
    def analyze(self) -> List[FlagDependency]:
        """Analyze flag producer-consumer relationships"""
        # Track last flag producer
        last_producer: Optional[Tuple[int, str]] = None
        
        for i, instr in enumerate(self.instructions):
            # Check if this instruction produces flags
            if instr.op == 'CMP':
                last_producer = (i, 'CMP')
            
            # Check if this instruction consumes flags
            if instr.flags_in == 0xFE:  # REG_FLAGS
                if last_producer:
                    dep = FlagDependency(
                        producer_idx=last_producer[0],
                        producer_op=last_producer[1],
                        consumer_idx=i,
                        consumer_op=instr.op
                    )
                    self.dependencies.append(dep)
            
            # Check if this instruction clobbers flags without consuming
            if instr.flags_out == 0xFE and instr.flags_in != 0xFE:
                # This is a new flag producer
                if last_producer:
                    # Mark previous dependency as potentially dead
                    for dep in self.dependencies:
                        if dep.producer_idx == last_producer[0]:
                            dep.is_live = False
                last_producer = (i, instr.op)
        
        return self.dependencies


class IRAnalyzer:
    """Main IR analysis class"""
    
    def __init__(self, instructions: List[IRInstruction]):
        self.instructions = instructions
        self.analysis = IRAnalysis(instructions=instructions)
        
    def analyze(self) -> IRAnalysis:
        """Perform complete IR analysis"""
        # Build CFG
        cfg_builder = CFGBuilder(self.instructions)
        self.analysis.basic_blocks = cfg_builder.build()
        
        # Analyze flag dependencies
        flag_analyzer = FlagDependencyAnalyzer(self.instructions)
        self.analysis.flag_dependencies = flag_analyzer.analyze()
        
        # Calculate statistics
        self._calculate_stats()
        
        # Detect issues
        self._detect_issues()
        
        return self.analysis
    
    def _calculate_stats(self):
        """Calculate IR statistics"""
        op_counts = defaultdict(int)
        for instr in self.instructions:
            op_counts[instr.op] += 1
        
        self.analysis.stats = {
            'total_instructions': len(self.instructions),
            'basic_blocks': len(self.analysis.basic_blocks),
            'op_counts': dict(op_counts),
            'has_loops': any(len(bb.predecessors) > 0 for bb in self.analysis.basic_blocks),
            'has_flag_ops': any(instr.op == 'CMP' for instr in self.instructions),
            'flag_deps': len(self.analysis.flag_dependencies)
        }
    
    def _detect_issues(self):
        """Detect potential issues in IR"""
        # Check for unused flag producers
        flag_producers = {i for i, instr in enumerate(self.instructions) if instr.op == 'CMP'}
        flag_consumers = {dep.producer_idx for dep in self.analysis.flag_dependencies}
        
        unused_flags = flag_producers - flag_consumers
        for idx in unused_flags:
            self.analysis.warnings.append(
                f"Instruction {idx}: CMP may have unused flags (DCE candidate)"
            )
        
        # Check for consecutive CMPs without use
        for i in range(len(self.instructions) - 1):
            if (self.instructions[i].op == 'CMP' and 
                self.instructions[i + 1].op == 'CMP'):
                self.analysis.warnings.append(
                    f"Instructions {i}-{i+1}: Consecutive CMPs (CSE candidate)"
                )
        
        # Check for dead flag producers
        for dep in self.analysis.flag_dependencies:
            if not dep.is_live:
                self.analysis.warnings.append(
                    f"Instruction {dep.producer_idx}: Flag producer may be dead"
                )


def parse_ir_dump(dump_file: str) -> List[IRInstruction]:
    """Parse IR dump from file"""
    with open(dump_file, 'r') as f:
        content = f.read()
    
    # Extract IR from stderr format
    ir_match = re.search(r'=== IR for function.*?===\n(.*?)\n===', content, re.DOTALL)
    if ir_match:
        ir_text = ir_match.group(1)
    else:
        ir_text = content
    
    parser = IRParser(ir_text)
    return parser.parse()


def analyze_flag_dependencies(instructions: List[IRInstruction]) -> Dict:
    """Analyze and report flag dependencies"""
    analyzer = FlagDependencyAnalyzer(instructions)
    deps = analyzer.analyze()
    
    report = {
        'total_dependencies': len(deps),
        'live_dependencies': sum(1 for d in deps if d.is_live),
        'dead_dependencies': sum(1 for d in deps if not d.is_live),
        'dependencies': [
            {
                'producer': f"#{dep.producer_idx} ({dep.producer_op})",
                'consumer': f"#{dep.consumer_idx} ({dep.consumer_op})",
                'live': dep.is_live
            }
            for dep in deps
        ]
    }
    
    return report


def detect_cse_candidates(instructions: List[IRInstruction]) -> List[Dict]:
    """Detect CSE (Common Subexpression Elimination) candidates"""
    candidates = []
    
    # Group instructions by operation and operands
    op_groups = defaultdict(list)
    for i, instr in enumerate(instructions):
        if instr.op in ['ADD', 'SUB', 'MUL', 'AND', 'OR', 'XOR']:
            key = (instr.op, instr.temp_src1, instr.temp_src2)
            op_groups[key].append(i)
    
    # Find duplicates
    for key, indices in op_groups.items():
        if len(indices) > 1:
            candidates.append({
                'operation': key[0],
                'operands': f"r{key[1]}, r{key[2]}",
                'locations': indices,
                'count': len(indices)
            })
    
    return candidates


def generate_report(analysis: IRAnalysis, format: str = 'text') -> str:
    """Generate analysis report"""
    if format == 'json':
        return json.dumps({
            'stats': analysis.stats,
            'basic_blocks': [
                {
                    'label': bb.label,
                    'start': bb.start_idx,
                    'end': bb.end_idx,
                    'successors': bb.successors,
                    'predecessors': bb.predecessors
                }
                for bb in analysis.basic_blocks
            ],
            'flag_dependencies': [
                {
                    'producer': f"#{dep.producer_idx} ({dep.producer_op})",
                    'consumer': f"#{dep.consumer_idx} ({dep.consumer_op})",
                    'live': dep.is_live
                }
                for dep in analysis.flag_dependencies
            ],
            'issues': analysis.issues,
            'warnings': analysis.warnings
        }, indent=2)
    
    # Text format
    lines = []
    lines.append("=" * 80)
    lines.append("IR ANALYSIS REPORT")
    lines.append("=" * 80)
    lines.append("")
    
    # Statistics
    lines.append("STATISTICS")
    lines.append("-" * 40)
    lines.append(f"Total instructions: {analysis.stats.get('total_instructions', 0)}")
    lines.append(f"Basic blocks: {analysis.stats.get('basic_blocks', 0)}")
    lines.append(f"Has loops: {analysis.stats.get('has_loops', False)}")
    lines.append(f"Has flag ops: {analysis.stats.get('has_flag_ops', False)}")
    lines.append(f"Flag dependencies: {analysis.stats.get('flag_deps', 0)}")
    lines.append("")
    
    # Operation counts
    op_counts = analysis.stats.get('op_counts', {})
    if op_counts:
        lines.append("OPERATION COUNTS")
        lines.append("-" * 40)
        for op, count in sorted(op_counts.items()):
            lines.append(f"  {op}: {count}")
        lines.append("")
    
    # Basic blocks
    lines.append("BASIC BLOCKS")
    lines.append("-" * 40)
    for bb in analysis.basic_blocks:
        succ_str = ', '.join(map(str, bb.successors)) if bb.successors else 'none'
        pred_str = ', '.join(map(str, bb.predecessors)) if bb.predecessors else 'none'
        lines.append(f"  Block L{bb.label}: [{bb.start_idx}-{bb.end_idx}]")
        lines.append(f"    Successors: {succ_str}")
        lines.append(f"    Predecessors: {pred_str}")
    lines.append("")
    
    # Flag dependencies
    lines.append("FLAG DEPENDENCIES")
    lines.append("-" * 40)
    lines.append(f"Total: {len(analysis.flag_dependencies)}")
    lines.append(f"Live: {sum(1 for d in analysis.flag_dependencies if d.is_live)}")
    lines.append(f"Dead: {sum(1 for d in analysis.flag_dependencies if not d.is_live)}")
    if analysis.flag_dependencies:
        lines.append("")
        for dep in analysis.flag_dependencies:
            status = "LIVE" if dep.is_live else "DEAD"
            lines.append(f"  {dep.producer_op}#{dep.producer_idx} -> {dep.consumer_op}#{dep.consumer_idx} [{status}]")
    lines.append("")
    
    # Issues and warnings
    if analysis.issues:
        lines.append("ISSUES")
        lines.append("-" * 40)
        for issue in analysis.issues:
            lines.append(f"  [ISSUE] {issue}")
        lines.append("")
    
    if analysis.warnings:
        lines.append("WARNINGS")
        lines.append("-" * 40)
        for warning in analysis.warnings:
            lines.append(f"  [WARN] {warning}")
        lines.append("")
    
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='IR Analyzer - Comprehensive IR analysis and verification'
    )
    parser.add_argument('--dump', required=True, help='IR dump file or stderr output')
    parser.add_argument('--analyze', choices=['all', 'flag', 'cse', 'cfg'], default='all',
                       help='Analysis type')
    parser.add_argument('--json', action='store_true', help='Output JSON format')
    parser.add_argument('--output', help='Output file (default: stdout)')
    
    args = parser.parse_args()
    
    # Read IR dump
    try:
        with open(args.dump, 'r') as f:
            dump_content = f.read()
    except FileNotFoundError:
        print(f"Error: File not found: {args.dump}", file=sys.stderr)
        sys.exit(1)
    
    # Parse IR
    instructions = parse_ir_dump(dump_content)
    
    if not instructions:
        print("Error: No instructions found in IR dump", file=sys.stderr)
        sys.exit(1)
    
    # Perform analysis
    if args.analyze == 'all':
        analyzer = IRAnalyzer(instructions)
        analysis = analyzer.analyze()
        report = generate_report(analysis, 'json' if args.json else 'text')
    elif args.analyze == 'flag':
        deps = analyze_flag_dependencies(instructions)
        report = json.dumps(deps, indent=2) if args.json else str(deps)
    elif args.analyze == 'cse':
        candidates = detect_cse_candidates(instructions)
        report = json.dumps(candidates, indent=2) if args.json else str(candidates)
    elif args.analyze == 'cfg':
        cfg_builder = CFGBuilder(instructions)
        blocks = cfg_builder.build()
        report = json.dumps([
            {
                'label': bb.label,
                'start': bb.start_idx,
                'end': bb.end_idx,
                'successors': bb.successors,
                'predecessors': bb.predecessors
            }
            for bb in blocks
        ], indent=2) if args.json else str(blocks)
    
    # Output
    if args.output:
        with open(args.output, 'w') as f:
            f.write(report)
        print(f"Report saved to: {args.output}")
    else:
        print(report)


if __name__ == '__main__':
    main()
