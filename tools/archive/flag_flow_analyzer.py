#!/usr/bin/env python3
"""
Flag Flow Analyzer - Deep analysis of flag producer-consumer relationships

This tool specifically analyzes flag dependencies to find:
- Unused flag producers (DCE candidates)
- Dead flag producers (overwritten before use)
- Flag consumer chains
- Optimization opportunities

Usage:
    python3 flag_flow_analyzer.py --dump ir_dump.txt
    python3 flag_flow_analyzer.py --dump ir_dump.txt --visualize
    python3 flag_flow_analyzer.py --dump ir_dump.txt --fix-candidates
"""

import argparse
import re
import sys
import json
from dataclasses import dataclass, field
from typing import List, Dict, Set, Optional, Tuple
from collections import defaultdict

@dataclass
class FlagProducer:
    """Represents a flag-producing instruction"""
    index: int
    op: str
    temp_src1: int
    temp_src2: int
    consumers: List[int] = field(default_factory=list)
    is_dead: bool = False
    is_unused: bool = False

@dataclass
class FlagConsumer:
    """Represents a flag-consuming instruction"""
    index: int
    op: str
    producer_idx: int
    condition: Optional[int] = None

@dataclass
class FlagFlowAnalysis:
    """Complete flag flow analysis"""
    producers: List[FlagProducer] = field(default_factory=list)
    consumers: List[FlagConsumer] = field(default_factory=list)
    issues: List[str] = field(default_factory=list)
    dce_candidates: List[int] = field(default_factory=list)
    cse_candidates: List[Tuple[int, int]] = field(default_factory=list)

class FlagFlowAnalyzer:
    """Analyzes flag producer-consumer relationships"""
    
    def __init__(self, instructions: List[Dict]):
        self.instructions = instructions
        self.analysis = FlagFlowAnalysis()
        
    def analyze(self) -> FlagFlowAnalysis:
        """Perform complete flag flow analysis"""
        # Find all flag producers (CMP instructions)
        self._find_producers()
        
        # Find all flag consumers (COND_SET, JCOND, etc.)
        self._find_consumers()
        
        # Match producers to consumers
        self._match_producer_consumer()
        
        # Detect issues
        self._detect_issues()
        
        return self.analysis
    
    def _find_producers(self):
        """Find all flag-producing instructions"""
        for i, instr in enumerate(self.instructions):
            if instr.get('op') == 'CMP':
                producer = FlagProducer(
                    index=i,
                    op='CMP',
                    temp_src1=instr.get('temp_src1', 0),
                    temp_src2=instr.get('temp_src2', 0)
                )
                self.analysis.producers.append(producer)
    
    def _find_consumers(self):
        """Find all flag-consuming instructions"""
        for i, instr in enumerate(self.instructions):
            op = instr.get('op', '')
            
            # Check if this instruction consumes flags
            is_consumer = False
            condition = None
            
            if op == 'COND_SET':
                is_consumer = True
                condition = instr.get('cond')
            elif op == 'JCOND':
                is_consumer = True
                condition = instr.get('cond')
            elif op in ['JE', 'JNE', 'JL', 'JLE', 'JG']:
                is_consumer = True
            
            if is_consumer:
                consumer = FlagConsumer(
                    index=i,
                    op=op,
                    producer_idx=-1,  # Will be matched later
                    condition=condition
                )
                self.analysis.consumers.append(consumer)
    
    def _match_producer_consumer(self):
        """Match flag producers to their consumers"""
        for consumer in self.analysis.consumers:
            # Find the most recent flag producer before this consumer
            closest_producer = None
            
            for producer in self.analysis.producers:
                if producer.index < consumer.index:
                    if closest_producer is None or producer.index > closest_producer.index:
                        closest_producer = producer
            
            if closest_producer:
                consumer.producer_idx = closest_producer.index
                closest_producer.consumers.append(consumer.index)
    
    def _detect_issues(self):
        """Detect flag-related issues"""
        # Find unused producers
        for producer in self.analysis.producers:
            if not producer.consumers:
                producer.is_unused = True
                self.analysis.dce_candidates.append(producer.index)
                self.analysis.issues.append(
                    f"Instruction {producer.index}: CMP has no consumers (DCE candidate)"
                )
        
        # Find dead producers (overwritten before use)
        for i, producer in enumerate(self.analysis.producers):
            if producer.consumers:
                # Check if there's another CMP between this producer and its consumers
                for consumer_idx in producer.consumers:
                    for other_producer in self.analysis.producers:
                        if (producer.index < other_producer.index < consumer_idx and
                            other_producer != producer):
                            # This producer's flags are overwritten before use
                            producer.is_dead = True
                            self.analysis.issues.append(
                                f"Instruction {producer.index}: CMP flags overwritten "
                                f"by instruction {other_producer.index} before consumer {consumer_idx}"
                            )
                            break
        
        # Find CSE candidates (duplicate CMPs)
        cmp_signatures = defaultdict(list)
        for producer in self.analysis.producers:
            sig = (producer.temp_src1, producer.temp_src2)
            cmp_signatures[sig].append(producer.index)
        
        for sig, indices in cmp_signatures.items():
            if len(indices) > 1:
                self.analysis.cse_candidates.append((indices[0], indices[1]))
                self.analysis.issues.append(
                    f"CSE candidate: CMP r{sig[0]}, r{sig[1]} at instructions {indices}"
                )


def parse_ir_dump(dump_file: str) -> List[Dict]:
    """Parse IR dump into instruction list"""
    with open(dump_file, 'r') as f:
        content = f.read()
    
    # Extract IR from stderr format
    ir_match = re.search(r'=== IR for function.*?===\n(.*?)\n===', content, re.DOTALL)
    if ir_match:
        ir_text = ir_match.group(1)
    else:
        ir_text = content
    
    instructions = []
    current_idx = 0
    
    for line in ir_text.strip().split('\n'):
        line = line.strip()
        
        # Skip header/footer
        if not line or line.startswith('IR Dump') or line.startswith('===') or line.startswith('---'):
            continue
        
        # Parse instruction
        instr = _parse_instruction(line, current_idx)
        if instr:
            instructions.append(instr)
            current_idx += 1
    
    return instructions


def _parse_instruction(line: str, idx: int) -> Optional[Dict]:
    """Parse a single instruction"""
    # Instruction: "  N: OP ..."
    instr_match = re.match(r'^\s*(\d+):\s+(\w+)\s+(.*)$', line)
    if not instr_match:
        return None
    
    op = instr_match.group(2)
    rest = instr_match.group(3)
    
    instr = {'index': idx, 'op': op, 'raw': line}
    
    # Parse specific fields
    if op == 'CMP':
        cmp_match = re.match(r'r(\d+),\s*\[flags\]?\s*r(\d+)', rest)
        if cmp_match:
            instr['temp_src1'] = int(cmp_match.group(1))
            instr['temp_src2'] = int(cmp_match.group(2))
    
    elif op == 'COND_SET':
        cond_match = re.match(r'r(\d+),\s*cc_(\d+)', rest)
        if cond_match:
            instr['temp_dest'] = int(cond_match.group(1))
            instr['cond'] = int(cond_match.group(2))
    
    elif op == 'JCOND':
        jcond_match = re.match(r'cc_(\d+),\s*L(\d+)', rest)
        if jcond_match:
            instr['cond'] = int(jcond_match.group(1))
            instr['label_idx'] = int(jcond_match.group(2))
    
    elif op in ['JE', 'JNE']:
        je_match = re.match(r'r(\d+),\s*L(\d+)', rest)
        if je_match:
            instr['temp_src1'] = int(je_match.group(1))
            instr['label_idx'] = int(je_match.group(2))
    
    return instr


def generate_report(analysis: FlagFlowAnalysis, format: str = 'text') -> str:
    """Generate analysis report"""
    if format == 'json':
        return json.dumps({
            'producers': [
                {
                    'index': p.index,
                    'op': p.op,
                    'operands': f"r{p.temp_src1}, r{p.temp_src2}",
                    'consumers': p.consumers,
                    'is_dead': p.is_dead,
                    'is_unused': p.is_unused
                }
                for p in analysis.producers
            ],
            'consumers': [
                {
                    'index': c.index,
                    'op': c.op,
                    'producer': c.producer_idx,
                    'condition': c.condition
                }
                for c in analysis.consumers
            ],
            'dce_candidates': analysis.dce_candidates,
            'cse_candidates': [
                {'first': c[0], 'second': c[1]}
                for c in analysis.cse_candidates
            ],
            'issues': analysis.issues
        }, indent=2)
    
    # Text format
    lines = []
    lines.append("=" * 80)
    lines.append("FLAG FLOW ANALYSIS REPORT")
    lines.append("=" * 80)
    lines.append("")
    
    # Summary
    lines.append("SUMMARY")
    lines.append("-" * 40)
    lines.append(f"Flag producers (CMP): {len(analysis.producers)}")
    lines.append(f"Flag consumers: {len(analysis.consumers)}")
    lines.append(f"DCE candidates: {len(analysis.dce_candidates)}")
    lines.append(f"CSE candidates: {len(analysis.cse_candidates)}")
    lines.append("")
    
    # Producers
    if analysis.producers:
        lines.append("FLAG PRODUCERS (CMP)")
        lines.append("-" * 40)
        for p in analysis.producers:
            status = []
            if p.is_dead:
                status.append("DEAD")
            if p.is_unused:
                status.append("UNUSED")
            status_str = f" [{', '.join(status)}]" if status else ""
            consumers_str = ', '.join(map(str, p.consumers)) if p.consumers else 'none'
            lines.append(f"  #{p.index}: CMP r{p.temp_src1}, r{p.temp_src2}{status_str}")
            lines.append(f"           Consumers: {consumers_str}")
        lines.append("")
    
    # Consumers
    if analysis.consumers:
        lines.append("FLAG CONSUMERS")
        lines.append("-" * 40)
        for c in analysis.consumers:
            lines.append(f"  #{c.index}: {c.op} (from producer #{c.producer_idx})")
        lines.append("")
    
    # DCE candidates
    if analysis.dce_candidates:
        lines.append("DCE CANDIDATES (Unused CMP)")
        lines.append("-" * 40)
        for idx in analysis.dce_candidates:
            lines.append(f"  #{idx}")
        lines.append("")
    
    # CSE candidates
    if analysis.cse_candidates:
        lines.append("CSE CANDIDATES (Duplicate CMP)")
        lines.append("-" * 40)
        for first, second in analysis.cse_candidates:
            lines.append(f"  #{first} and #{second}")
        lines.append("")
    
    # Issues
    if analysis.issues:
        lines.append("ISSUES")
        lines.append("-" * 40)
        for issue in analysis.issues:
            lines.append(f"  [ISSUE] {issue}")
        lines.append("")
    
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Flag Flow Analyzer - Deep flag dependency analysis'
    )
    parser.add_argument('--dump', required=True, help='IR dump file')
    parser.add_argument('--json', action='store_true', help='Output JSON format')
    parser.add_argument('--output', help='Output file (default: stdout)')
    parser.add_argument('--visualize', action='store_true', help='Print visual flow diagram')
    
    args = parser.parse_args()
    
    # Parse IR
    instructions = parse_ir_dump(args.dump)
    
    if not instructions:
        print("Error: No instructions found in IR dump", file=sys.stderr)
        sys.exit(1)
    
    # Analyze
    analyzer = FlagFlowAnalyzer(instructions)
    analysis = analyzer.analyze()
    
    # Generate report
    report = generate_report(analysis, 'json' if args.json else 'text')
    
    # Visualize if requested
    if args.visualize:
        report += "\n" + "=" * 80 + "\n"
        report += "FLAG FLOW DIAGRAM\n"
        report += "=" * 80 + "\n\n"
        
        for producer in analysis.producers:
            report += f"#{producer.index} [CMP] ──┐\n"
            for consumer_idx in producer.consumers:
                report += f"           └──> #{consumer_idx}\n"
            if not producer.consumers:
                report += f"           └──> [UNUSED]\n"
            report += "\n"
    
    # Output
    if args.output:
        with open(args.output, 'w') as f:
            f.write(report)
        print(f"Report saved to: {args.output}")
    else:
        print(report)


if __name__ == '__main__':
    main()
