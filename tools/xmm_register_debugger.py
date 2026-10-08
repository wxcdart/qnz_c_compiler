#!/usr/bin/env python3
"""
XMM Register Debugger for ZeroCC
Debugs XMM register allocation and usage patterns

Usage:
    python3 xmm_register_debugger.py --trace <assembly_file>
    python3 xmm_register_debugger.py --analyze <assembly_file>
    python3 xmm_register_debugger.py --visualize <assembly_file>
"""

import sys
import re
import argparse
from typing import Dict, List, Set, Optional, Tuple
from dataclasses import dataclass, field
from collections import defaultdict


@dataclass
class XMMInstruction:
    """Represents an instruction using XMM registers"""
    line_num: int
    mnemonic: str
    operands: List[str]
    xmm_regs_used: List[int]
    xmm_regs_written: List[int]
    is_move: bool
    is_arithmetic: bool
    is_comparison: bool


@dataclass
class XMMRegisterState:
    """Tracks the state of an XMM register"""
    reg_num: int
    defined_at: List[int] = field(default_factory=list)
    used_at: List[int] = field(default_factory=list)
    killed_at: List[int] = field(default_factory=list)
    current_value: Optional[str] = None
    is_live: bool = False


@dataclass
class DebugReport:
    """Debug report for XMM register usage"""
    total_instructions: int = 0
    xmm_instructions: int = 0
    register_usage: Dict[int, XMMRegisterState] = field(default_factory=dict)
    potential_issues: List[str] = field(default_factory=list)
    live_ranges: Dict[int, Tuple[int, int]] = field(default_factory=dict)


class XMMRegisterDebugger:
    def __init__(self):
        self.instructions: List[XMMInstruction] = []
        self.register_states: Dict[int, XMMRegisterState] = {}
        self.current_function: str = ""
        self.function_start_line: int = 0
        
        # Initialize XMM register states (xmm0-xmm15)
        for i in range(16):
            self.register_states[i] = XMMRegisterState(reg_num=i)
    
    def parse_instruction(self, line: str, line_num: int) -> Optional[XMMInstruction]:
        """Parse a single assembly instruction"""
        line = line.strip()
        
        # Skip empty lines, comments, labels, directives
        if not line or line.startswith('#') or line.startswith('.') or line.endswith(':'):
            return None
        
        # Parse instruction
        parts = line.split(None, 1)
        if not parts:
            return None
        
        mnemonic = parts[0].lower()
        operands_str = parts[1] if len(parts) > 1 else ""
        
        # Extract operands
        operands = [op.strip() for op in re.split(r'[,\s]+', operands_str) if op.strip()]
        
        # Find XMM registers
        xmm_pattern = re.compile(r'xmm(\d+)')
        xmm_regs = [int(m.group(1)) for m in xmm_pattern.finditer(line)]
        
        if not xmm_regs:
            return None
        
        # Determine instruction type
        is_move = mnemonic in ['movaps', 'movups', 'movss', 'movsd', 'movdqa', 'movdqu', 
                               'movd', 'movq', 'movhlps', 'movlhps', 'movshdup', 'movsldup']
        is_arithmetic = mnemonic in ['addss', 'addsd', 'subss', 'subsd', 'mulss', 'mulsd',
                                     'divss', 'divsd', 'sqrtsd', 'rsqrtsd', 'rcpss']
        is_comparison = mnemonic in ['ucomiss', 'ucomisd', 'comiss', 'comisd', 'cmpeqss',
                                     'cmpltss', 'cmpless', 'cmpeqsd', 'cmpltsd', 'cmplesd']
        
        # Determine which registers are read vs written
        xmm_written = []
        xmm_read = []
        
        if is_move:
            # First operand is destination (written)
            if len(operands) >= 1:
                dest_match = xmm_pattern.search(operands[0])
                if dest_match:
                    xmm_written.append(int(dest_match.group(1)))
            # Second operand is source (read)
            if len(operands) >= 2:
                src_match = xmm_pattern.search(operands[1])
                if src_match:
                    xmm_read.append(int(src_match.group(1)))
        else:
            # For most instructions, first operand is both read and written
            if len(operands) >= 1:
                dest_match = xmm_pattern.search(operands[0])
                if dest_match:
                    reg = int(dest_match.group(1))
                    xmm_written.append(reg)
                    xmm_read.append(reg)
            if len(operands) >= 2:
                src_match = xmm_pattern.search(operands[1])
                if src_match:
                    xmm_read.append(int(src_match.group(1)))
        
        return XMMInstruction(
            line_num=line_num,
            mnemonic=mnemonic,
            operands=operands,
            xmm_regs_used=xmm_read,
            xmm_regs_written=xmm_written,
            is_move=is_move,
            is_arithmetic=is_arithmetic,
            is_comparison=is_comparison
        )
    
    def analyze_file(self, filepath: str) -> DebugReport:
        """Analyze XMM register usage in assembly file"""
        try:
            with open(filepath, 'r') as f:
                lines = f.readlines()
        except IOError as e:
            print(f"Error reading file: {e}", file=sys.stderr)
            return DebugReport()
        
        self.instructions = []
        
        for line_num, line in enumerate(lines, 1):
            instr = self.parse_instruction(line, line_num)
            if instr:
                self.instructions.append(instr)
                self._update_register_state(instr)
        
        return self._generate_report()
    
    def _update_register_state(self, instr: XMMInstruction):
        """Update register state based on instruction"""
        # Process writes first (definitions)
        for reg in instr.xmm_regs_written:
            if reg in self.register_states:
                self.register_states[reg].defined_at.append(instr.line_num)
                self.register_states[reg].is_live = True
                self.register_states[reg].current_value = instr.mnemonic
        
        # Process reads (uses)
        for reg in instr.xmm_regs_used:
            if reg in self.register_states:
                self.register_states[reg].used_at.append(instr.line_num)
    
    def _generate_report(self) -> DebugReport:
        """Generate debug report"""
        report = DebugReport()
        report.total_instructions = len(self.instructions)
        report.xmm_instructions = sum(1 for i in self.instructions if i.xmm_regs_used)
        
        # Copy register states
        for reg_num, state in self.register_states.items():
            report.register_usage[reg_num] = state
        
        # Calculate live ranges
        for reg_num, state in self.register_states.items():
            if state.defined_at and state.used_at:
                first_def = min(state.defined_at)
                last_use = max(state.used_at)
                report.live_ranges[reg_num] = (first_def, last_use)
        
        # Detect potential issues
        self._detect_issues(report)
        
        return report
    
    def _detect_issues(self, report: DebugReport):
        """Detect potential XMM register issues"""
        for reg_num, state in self.register_states.items():
            # Check for use before definition
            if state.used_at and state.defined_at:
                first_use = min(state.used_at)
                first_def = min(state.defined_at)
                if first_use < first_def:
                    report.potential_issues.append(
                        f"xmm{reg_num}: Used at line {first_use} before definition at line {first_def}"
                    )
            
            # Check for definition without use (dead code)
            if state.defined_at and not state.used_at:
                report.potential_issues.append(
                    f"xmm{reg_num}: Defined at line {state.defined_at[0]} but never used"
                )
            
            # Check for very long live ranges
            if reg_num in report.live_ranges:
                first_def, last_use = report.live_ranges[reg_num]
                live_range = last_use - first_def
                if live_range > 100:
                    report.potential_issues.append(
                        f"xmm{reg_num}: Very long live range ({live_range} lines)"
                    )
    
    def generate_trace(self, report: DebugReport) -> str:
        """Generate instruction trace"""
        lines = []
        lines.append("=" * 70)
        lines.append("XMM Register Instruction Trace")
        lines.append("=" * 70)
        
        for instr in self.instructions:
            if not instr.xmm_regs_used:
                continue
            
            line_marker = ">>>" if any(
                reg in instr.xmm_regs_written 
                for reg in range(16)
            ) else "   "
            
            lines.append(f"{line_marker} Line {instr.line_num:4d}: {instr.mnemonic:12s} {', '.join(instr.operands)}")
            lines.append(f"       XMM used: {instr.xmm_regs_used}, written: {instr.xmm_regs_written}")
        
        lines.append("=" * 70)
        return '\n'.join(lines)
    
    def generate_analysis(self, report: DebugReport) -> str:
        """Generate analysis report"""
        lines = []
        lines.append("=" * 70)
        lines.append("XMM Register Analysis Report")
        lines.append("=" * 70)
        
        lines.append(f"\nTotal Instructions: {report.total_instructions}")
        lines.append(f"XMM Instructions: {report.xmm_instructions}")
        lines.append(f"XMM Coverage: {report.xmm_instructions/report.total_instructions*100:.1f}%" if report.total_instructions > 0 else "XMM Coverage: 0%")
        
        lines.append("\nRegister Usage:")
        lines.append("-" * 70)
        
        for reg_num in range(16):
            state = report.register_usage.get(reg_num)
            if not state:
                continue
            
            if state.defined_at or state.used_at:
                live_range = report.live_ranges.get(reg_num, (0, 0))
                lines.append(f"\nxmm{reg_num:2d}:")
                lines.append(f"  Defined at: {state.defined_at[:5]}{'...' if len(state.defined_at) > 5 else ''} ({len(state.defined_at)} times)")
                lines.append(f"  Used at:    {state.used_at[:5]}{'...' if len(state.used_at) > 5 else ''} ({len(state.used_at)} times)")
                lines.append(f"  Live range: {live_range[0]} - {live_range[1]} ({live_range[1]-live_range[0]} lines)")
                lines.append(f"  Status:     {'LIVE' if state.is_live else 'DEAD'}")
        
        if report.potential_issues:
            lines.append(f"\nPotential Issues ({len(report.potential_issues)}):")
            lines.append("-" * 70)
            for issue in report.potential_issues:
                lines.append(f"  ⚠ {issue}")
        
        lines.append("=" * 70)
        return '\n'.join(lines)
    
    def generate_visualization(self, report: DebugReport) -> str:
        """Generate ASCII visualization of register usage"""
        lines = []
        lines.append("=" * 70)
        lines.append("XMM Register Usage Visualization")
        lines.append("=" * 70)
        
        # Find max line number
        max_line = max(
            (ranges[1] for ranges in report.live_ranges.values()),
            default=100
        )
        
        # Create timeline
        timeline_width = min(60, max_line)
        
        lines.append(f"\nTimeline (lines 0-{max_line}):")
        lines.append("Reg |" + "-" * timeline_width + "|")
        lines.append("----|" + "-" * timeline_width + "|")
        
        for reg_num in range(16):
            if reg_num not in report.live_ranges:
                continue
            
            first_def, last_use = report.live_ranges[reg_num]
            
            # Create bar
            bar = [' '] * timeline_width
            start = min(first_def, timeline_width - 1)
            end = min(last_use, timeline_width - 1)
            
            for i in range(start, end + 1):
                bar[i] = '#'
            
            bar_str = ''.join(bar)
            lines.append(f"xmm{reg_num:2d}|{bar_str}|")
        
        lines.append("\nLegend: # = register live")
        lines.append("=" * 70)
        return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Debug XMM register allocation and usage in ZeroCC'
    )
    parser.add_argument('file', nargs='?', help='Assembly file to analyze')
    parser.add_argument('--trace', action='store_true', help='Show instruction trace')
    parser.add_argument('--analyze', action='store_true', help='Show analysis report')
    parser.add_argument('--visualize', action='store_true', help='Show visualization')
    parser.add_argument('--all', action='store_true', help='Show all outputs')
    
    args = parser.parse_args()
    
    if not args.file:
        parser.print_help()
        sys.exit(1)
    
    debugger = XMMRegisterDebugger()
    report = debugger.analyze_file(args.file)
    
    if args.all or (not args.trace and not args.analyze and not args.visualize):
        print(debugger.generate_analysis(report))
    else:
        if args.trace:
            print(debugger.generate_trace(report))
        if args.analyze:
            print(debugger.generate_analysis(report))
        if args.visualize:
            print(debugger.generate_visualization(report))


if __name__ == '__main__':
    main()
