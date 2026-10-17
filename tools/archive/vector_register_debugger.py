#!/usr/bin/env python3
"""
Vector Register Debugger for ZeroCC
Debugs vector register allocation and usage patterns for x86-64 (XMM), AArch64 (NEON), and RISC-V (RVV)

Usage:
    python3 vector_register_debugger.py --trace <assembly_file> --isa x86_64
    python3 vector_register_debugger.py --analyze <assembly_file> --isa aarch64
    python3 vector_register_debugger.py --visualize <assembly_file> --isa riscv64
    python3 vector_register_debugger.py --all <assembly_file> --isa x86_64
"""

import sys
import re
import argparse
from typing import Dict, List, Set, Optional, Tuple
from dataclasses import dataclass, field
from collections import defaultdict
from enum import Enum


class ISA(Enum):
    X86_64 = "x86_64"
    AARCH64 = "aarch64"
    RISCV64 = "riscv64"


@dataclass
class VectorInstruction:
    """Represents a vector instruction"""
    line_num: int
    mnemonic: str
    operands: List[str]
    vec_regs_used: List[int]
    vec_regs_written: List[int]
    is_move: bool
    is_arithmetic: bool
    is_comparison: bool
    is_load: bool
    is_store: bool


@dataclass
class VectorRegisterState:
    """Tracks the state of a vector register"""
    reg_num: int
    defined_at: List[int] = field(default_factory=list)
    used_at: List[int] = field(default_factory=list)
    killed_at: List[int] = field(default_factory=list)
    current_value: Optional[str] = None
    is_live: bool = False


@dataclass
class VectorDebugReport:
    """Debug report for vector register usage"""
    total_instructions: int = 0
    vector_instructions: int = 0
    register_usage: Dict[int, VectorRegisterState] = field(default_factory=dict)
    potential_issues: List[str] = field(default_factory=list)
    live_ranges: Dict[int, Tuple[int, int]] = field(default_factory=dict)
    isa: ISA = ISA.X86_64


class VectorRegisterDebugger:
    def __init__(self, isa: ISA = ISA.X86_64):
        self.isa = isa
        self.instructions: List[VectorInstruction] = []
        self.register_states: Dict[int, VectorRegisterState] = {}
        self.current_function: str = ""
        self.function_start_line: int = 0
        
        # Initialize vector register states based on ISA
        if isa == ISA.X86_64:
            self.num_vec_regs = 16
            self.reg_prefix = "xmm"
            # x86-64 SSE/AVX vector instructions
            self.move_mnemonics = ['movaps', 'movups', 'movss', 'movsd', 'movdqa', 'movdqu', 
                                   'movd', 'movq', 'movhlps', 'movlhps', 'movshdup', 'movsldup',
                                   'movapd', 'movupd', 'movlpd', 'movhpd']
            self.arithmetic_mnemonics = ['addss', 'addsd', 'subss', 'subsd', 'mulss', 'mulsd',
                                         'divss', 'divsd', 'sqrtsd', 'rsqrtsd', 'rcpss',
                                         'addps', 'subps', 'mulps', 'divps', 'addpd', 'subpd', 'mulpd', 'divpd']
            self.comparison_mnemonics = ['ucomiss', 'ucomisd', 'comiss', 'comisd', 'cmpeqss',
                                         'cmpltss', 'cmpless', 'cmpeqsd', 'cmpltsd', 'cmplesd',
                                         'cmpps', 'cmpsd']
            self.load_mnemonics = ['movaps', 'movups', 'movss', 'movsd', 'movapd', 'movupd', 'movlpd', 'movhpd']
            self.store_mnemonics = ['movaps', 'movups', 'movss', 'movsd', 'movapd', 'movupd', 'movlpd', 'movhpd']
        elif isa == ISA.AARCH64:
            self.num_vec_regs = 32
            self.reg_prefix = "v"
            # AArch64 NEON vector instructions
            self.move_mnemonics = ['mov', 'dup', 'ld1', 'st1', 'ld2', 'st2', 'ld3', 'st3', 'ld4', 'st4']
            self.arithmetic_mnemonics = ['fadd', 'fsub', 'fmul', 'fdiv', 'fmax', 'fmin',
                                         'add', 'sub', 'mul', 'sadd', 'smul', 'smax', 'smin',
                                         'uadd', 'umul', 'umax', 'umin']
            self.comparison_mnemonics = ['fcmp', 'cmge', 'cmgt', 'cmle', 'cm.lt', 'cmeq', 'cmne']
            self.load_mnemonics = ['ld1', 'ld2', 'ld3', 'ld4', 'ld1r', 'ld2r', 'ld3r', 'ld4r']
            self.store_mnemonics = ['st1', 'st2', 'st3', 'st4']
        else:  # RISCV64
            self.num_vec_regs = 32
            self.reg_prefix = "v"
            # RISC-V RVV vector instructions
            self.move_mnemonics = ['vmv', 'vfmv']
            self.arithmetic_mnemonics = ['vfadd', 'vfsub', 'vfmul', 'vfdiv', 'vfmin', 'vfmax',
                                         'vadd', 'vsub', 'vmul', 'vdiv', 'vaddi', 'vmuli']
            self.comparison_mnemonics = ['vfsgnj', 'vfsgnjn', 'vfsgnjx', 'vfclass']
            self.load_mnemonics = ['vle32.v', 'vle64.v', 'vle16.v', 'vlse32.v', 'vlse64.v']
            self.store_mnemonics = ['vse32.v', 'vse64.v', 'vse16.v', 'vsse32.v', 'vsse64.v']
        
        # Initialize register states
        for i in range(self.num_vec_regs):
            self.register_states[i] = VectorRegisterState(reg_num=i)
    
    def parse_instruction(self, line: str, line_num: int) -> Optional[VectorInstruction]:
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
        
        # Find vector registers based on ISA
        reg_pattern = re.compile(rf'{self.reg_prefix}(\d+)')
        vec_regs = [int(m.group(1)) for m in reg_pattern.finditer(line)]
        
        if not vec_regs:
            return None
        
        # Determine instruction type
        is_move = mnemonic in self.move_mnemonics
        is_arithmetic = mnemonic in self.arithmetic_mnemonics
        is_comparison = mnemonic in self.comparison_mnemonics
        is_load = mnemonic in self.load_mnemonics
        is_store = mnemonic in self.store_mnemonics
        
        # Determine which registers are read vs written
        vec_written = []
        vec_read = []
        
        if is_move or is_load:
            # First operand is destination (written)
            if len(operands) >= 1:
                dest_match = reg_pattern.search(operands[0])
                if dest_match:
                    vec_written.append(int(dest_match.group(1)))
            # Second operand is source (read) for moves
            if is_move and len(operands) >= 2:
                src_match = reg_pattern.search(operands[1])
                if src_match:
                    vec_read.append(int(src_match.group(1)))
        elif is_store:
            # First operand is source (read)
            if len(operands) >= 1:
                src_match = reg_pattern.search(operands[0])
                if src_match:
                    vec_read.append(int(src_match.group(1)))
        else:
            # For arithmetic/comparison, first operand is both read and written
            if len(operands) >= 1:
                dest_match = reg_pattern.search(operands[0])
                if dest_match:
                    reg = int(dest_match.group(1))
                    vec_written.append(reg)
                    vec_read.append(reg)
            if len(operands) >= 2:
                src_match = reg_pattern.search(operands[1])
                if src_match:
                    vec_read.append(int(src_match.group(1)))
        
        return VectorInstruction(
            line_num=line_num,
            mnemonic=mnemonic,
            operands=operands,
            vec_regs_used=vec_read,
            vec_regs_written=vec_written,
            is_move=is_move,
            is_arithmetic=is_arithmetic,
            is_comparison=is_comparison,
            is_load=is_load,
            is_store=is_store
        )
    
    def analyze_file(self, filepath: str) -> VectorDebugReport:
        """Analyze vector register usage in assembly file"""
        try:
            with open(filepath, 'r') as f:
                lines = f.readlines()
        except IOError as e:
            print(f"Error reading file: {e}", file=sys.stderr)
            return VectorDebugReport(isa=self.isa)
        
        self.instructions = []
        
        for line_num, line in enumerate(lines, 1):
            instr = self.parse_instruction(line, line_num)
            if instr:
                self.instructions.append(instr)
                self._update_register_state(instr)
        
        return self._generate_report()
    
    def _update_register_state(self, instr: VectorInstruction):
        """Update register state based on instruction"""
        # Process writes first (definitions)
        for reg in instr.vec_regs_written:
            if reg in self.register_states:
                self.register_states[reg].defined_at.append(instr.line_num)
                self.register_states[reg].is_live = True
                self.register_states[reg].current_value = instr.mnemonic
        
        # Process reads (uses)
        for reg in instr.vec_regs_used:
            if reg in self.register_states:
                self.register_states[reg].used_at.append(instr.line_num)
    
    def _generate_report(self) -> VectorDebugReport:
        """Generate debug report"""
        report = VectorDebugReport(isa=self.isa)
        report.total_instructions = len(self.instructions)
        report.vector_instructions = sum(1 for i in self.instructions if i.vec_regs_used)
        
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
    
    def _detect_issues(self, report: VectorDebugReport):
        """Detect potential vector register issues"""
        reg_name = f"{self.reg_prefix}"
        
        for reg_num, state in self.register_states.items():
            # Check for use before definition
            if state.used_at and state.defined_at:
                first_use = min(state.used_at)
                first_def = min(state.defined_at)
                if first_use < first_def:
                    report.potential_issues.append(
                        f"{reg_name}{reg_num}: Used at line {first_use} before definition at line {first_def}"
                    )
            
            # Check for definition without use (dead code)
            if state.defined_at and not state.used_at:
                report.potential_issues.append(
                    f"{reg_name}{reg_num}: Defined at line {state.defined_at[0]} but never used"
                )
            
            # Check for very long live ranges
            if reg_num in report.live_ranges:
                first_def, last_use = report.live_ranges[reg_num]
                live_range = last_use - first_def
                if live_range > 100:
                    report.potential_issues.append(
                        f"{reg_name}{reg_num}: Very long live range ({live_range} lines)"
                    )
    
    def generate_trace(self, report: VectorDebugReport) -> str:
        """Generate instruction trace"""
        lines = []
        lines.append("=" * 70)
        lines.append(f"{self.reg_prefix.upper()} Register Instruction Trace ({self.isa.value.upper()})")
        lines.append("=" * 70)
        
        for instr in self.instructions:
            if not instr.vec_regs_used:
                continue
            
            line_marker = ">>>" if any(
                reg in instr.vec_regs_written 
                for reg in range(self.num_vec_regs)
            ) else "   "
            
            lines.append(f"{line_marker} Line {instr.line_num:4d}: {instr.mnemonic:15s} {', '.join(instr.operands)}")
            lines.append(f"       Vec regs used: {instr.vec_regs_used}, written: {instr.vec_regs_written}")
        
        lines.append("=" * 70)
        return '\n'.join(lines)
    
    def generate_analysis(self, report: VectorDebugReport) -> str:
        """Generate analysis report"""
        lines = []
        lines.append("=" * 70)
        lines.append(f"Vector Register Analysis Report ({self.isa.value.upper()})")
        lines.append("=" * 70)
        
        lines.append(f"\nTotal Instructions: {report.total_instructions}")
        lines.append(f"Vector Instructions: {report.vector_instructions}")
        coverage = report.vector_instructions/report.total_instructions*100 if report.total_instructions > 0 else 0
        lines.append(f"Vector Coverage: {coverage:.1f}%")
        
        lines.append(f"\nISA: {self.isa.value}")
        lines.append(f"Vector Registers: {self.num_vec_regs} ({self.reg_prefix}0-{self.reg_prefix}{self.num_vec_regs-1})")
        
        lines.append("\nRegister Usage:")
        lines.append("-" * 70)
        
        for reg_num in range(self.num_vec_regs):
            state = report.register_usage.get(reg_num)
            if not state:
                continue
            
            if state.defined_at or state.used_at:
                live_range = report.live_ranges.get(reg_num, (0, 0))
                lines.append(f"\n{self.reg_prefix}{reg_num:2d}:")
                lines.append(f"  Defined at: {state.defined_at[:5]}{'...' if len(state.defined_at) > 5 else ''} ({len(state.defined_at)} times)")
                lines.append(f"  Used at:    {state.used_at[:5]}{'...' if len(state.used_at) > 5 else ''} ({len(state.used_at)} times)")
                lines.append(f"  Live range: {live_range[0]} - {live_range[1]} ({live_range[1]-live_range[0]} lines)")
                lines.append(f"  Status:     {'LIVE' if state.is_live else 'DEAD'}")
        
        if report.potential_issues:
            lines.append(f"\nPotential Issues ({len(report.potential_issues)}):")
            lines.append("-" * 70)
            for issue in report.potential_issues:
                lines.append(f"  ! {issue}")
        
        lines.append("=" * 70)
        return '\n'.join(lines)
    
    def generate_visualization(self, report: VectorDebugReport) -> str:
        """Generate ASCII visualization of register usage"""
        lines = []
        lines.append("=" * 70)
        lines.append(f"Vector Register Usage Visualization ({self.isa.value.upper()})")
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
        
        for reg_num in range(self.num_vec_regs):
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
            lines.append(f"{self.reg_prefix}{reg_num:2d}|{bar_str}|")
        
        lines.append("\nLegend: # = register live")
        lines.append("=" * 70)
        return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Debug vector register allocation and usage in ZeroCC (x86-64 XMM, AArch64 NEON, RISC-V RVV)'
    )
    parser.add_argument('file', nargs='?', help='Assembly file to analyze')
    parser.add_argument('--isa', choices=['x86_64', 'aarch64', 'riscv64'], default='x86_64',
                        help='Target ISA (default: x86_64)')
    parser.add_argument('--trace', action='store_true', help='Show instruction trace')
    parser.add_argument('--analyze', action='store_true', help='Show analysis report')
    parser.add_argument('--visualize', action='store_true', help='Show visualization')
    parser.add_argument('--all', action='store_true', help='Show all outputs')
    
    args = parser.parse_args()
    
    if not args.file:
        parser.print_help()
        sys.exit(1)
    
    isa = ISA(args.isa)
    debugger = VectorRegisterDebugger(isa=isa)
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
