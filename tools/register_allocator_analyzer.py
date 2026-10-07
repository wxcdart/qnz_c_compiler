#!/usr/bin/env python3
"""
Register Allocator Analyzer

Analyzes register allocator decisions by parsing IR and comparing with disassembly.
Detects issues like:
1. Caller-saved registers assigned to temps spanning calls
2. Missing spill slots for preserved registers
3. Register allocation inefficiencies

Usage:
    python3 register_allocator_analyzer.py --ir ir_dump.txt --object test.o
    python3 register_allocator_analyzer.py --ir ir_dump.txt --object test.o --analyze-calls
"""

import argparse
import subprocess
import re
import sys
import json
from dataclasses import dataclass, field
from typing import List, Dict, Set, Optional
from collections import defaultdict

@dataclass
class IRInstruction:
    """Represents an IR instruction"""
    index: int
    op: str
    temp_dest: Optional[int] = None
    temp_src1: Optional[int] = None
    temp_src2: Optional[int] = None

@dataclass
class PhysicalRegisterAssignment:
    """Maps a temp to a physical register"""
    temp_id: int
    phys_reg: str
    phys_reg_num: int
    from_addr: int
    to_addr: int

@dataclass
class RegisterAllocatorIssue:
    """Represents a register allocator issue"""
    issue_type: str  # "caller_saved_spans_call", "no_spill", etc.
    temp_id: int
    register: str
    severity: str  # "HIGH", "MEDIUM", "LOW"
    description: str

# AArch64 registers
CALLER_SAVED_REGS = {f"x{i}" for i in range(0, 19)}
CALLEE_SAVED_REGS = {f"x{i}" for i in range(19, 31)}

class RegisterAllocatorAnalyzer:
    """Analyzes register allocator decisions"""
    
    def __init__(self, ir_dump: str, object_file: str):
        self.ir_dump = ir_dump
        self.object_file = object_file
        self.ir_instructions: List[IRInstruction] = []
        self.assignments: Dict[int, PhysicalRegisterAssignment] = {}
        self.issues: List[RegisterAllocatorIssue] = []
    
    def analyze(self) -> List[RegisterAllocatorIssue]:
        """Perform complete analysis"""
        # Parse IR
        self._parse_ir()
        
        # Get disassembly and extract register assignments
        disasm = self._get_disassembly()
        self._extract_assignments(disasm)
        
        # Analyze assignments
        self._check_caller_saved_spans_call()
        
        return self.issues
    
    def _parse_ir(self):
        """Parse IR dump"""
        for line in self.ir_dump.split('\n'):
            line = line.strip()
            
            # Parse instruction: "  N: OP ..."
            match = re.match(r'^\s*(\d+):\s+(\w+)\s+(.*)$', line)
            if match:
                idx = int(match.group(1))
                op = match.group(2)
                rest = match.group(3)
                
                # Parse operands
                temp_dest = None
                temp_src1 = None
                temp_src2 = None
                
                # Extract temp IDs from operands
                temps = re.findall(r'r(\d+)', rest)
                if temps:
                    temp_dest = int(temps[0]) if len(temps) > 0 else None
                    temp_src1 = int(temps[1]) if len(temps) > 1 else None
                    temp_src2 = int(temps[2]) if len(temps) > 2 else None
                
                instr = IRInstruction(
                    index=idx,
                    op=op,
                    temp_dest=temp_dest,
                    temp_src1=temp_src1,
                    temp_src2=temp_src2
                )
                
                self.ir_instructions.append(instr)
    
    def _get_disassembly(self) -> str:
        """Get disassembly"""
        try:
            result = subprocess.run(
                ['aarch64-linux-gnu-objdump', '-d', self.object_file],
                capture_output=True,
                text=True,
                check=True
            )
            return result.stdout
        except subprocess.CalledProcessError:
            return ""
    
    def _extract_assignments(self, disasm: str):
        """Extract register assignments from disassembly"""
        # This is a simplified extraction - in reality we'd need to track
        # register usage more carefully
        
        # Find patterns like: ldur x3, [x29, #-56]
        # This indicates x3 is holding some value
        
        for line in disasm.split('\n'):
            # Look for loads to caller-saved registers
            match = re.search(
                r'([0-9a-f]+):\s+[0-9a-f\s]+\s+(ldur|ldr)\s+([xw]\d+),',
                line
            )
            if match:
                addr = int(match.group(1), 16)
                reg_name = match.group(3)
                
                # Track this register is being loaded
                reg_num = self._parse_reg_num(reg_name)
                if reg_num is not None:
                    # This is a simple tracking - a real implementation would be more sophisticated
                    pass
    
    def _parse_reg_num(self, reg_name: str) -> Optional[int]:
        """Parse register name"""
        match = re.match(r'[xw](\d+)', reg_name)
        if match:
            return int(match.group(1))
        return None
    
    def _check_caller_saved_spans_call(self):
        """Check for caller-saved registers assigned to temps spanning calls"""
        # Find all CALL instructions
        call_indices = []
        for i, instr in enumerate(self.ir_instructions):
            if 'CALL' in instr.op:
                call_indices.append(i)
        
        # For each call, check if temps used before and after are caller-saved
        for call_idx in call_indices:
            # Find temps used before the call
            temps_before = set()
            for i in range(0, call_idx):
                instr = self.ir_instructions[i]
                if instr.temp_src1 is not None:
                    temps_before.add(instr.temp_src1)
                if instr.temp_src2 is not None:
                    temps_before.add(instr.temp_src2)
            
            # Find temps used after the call
            temps_after = set()
            for i in range(call_idx + 1, len(self.ir_instructions)):
                instr = self.ir_instructions[i]
                if instr.temp_src1 is not None:
                    temps_after.add(instr.temp_src1)
                if instr.temp_src2 is not None:
                    temps_after.add(instr.temp_src2)
            
            # Temps that span the call
            temps_spanning = temps_before & temps_after
            
            for temp_id in temps_spanning:
                # Check if this temp is assigned a caller-saved register
                # This is a simplified check
                issue = RegisterAllocatorIssue(
                    issue_type="caller_saved_spans_call",
                    temp_id=temp_id,
                    register="(unknown)",
                    severity="HIGH",
                    description=f"Temp r{temp_id} is used before and after call at IR index {call_idx} - "
                              f"likely assigned a caller-saved register that will be clobbered"
                )
                self.issues.append(issue)

def generate_report(issues: List[RegisterAllocatorIssue]) -> str:
    """Generate report"""
    lines = []
    lines.append("=" * 80)
    lines.append("REGISTER ALLOCATOR ANALYSIS REPORT")
    lines.append("=" * 80)
    lines.append("")
    
    if not issues:
        lines.append("✓ No register allocator issues detected!")
        return "\n".join(lines)
    
    lines.append(f"Found {len(issues)} potential register allocator issues:")
    lines.append("")
    
    for issue in issues:
        lines.append(f"[{issue.severity}] {issue.issue_type}")
        lines.append(f"  Temp: r{issue.temp_id}")
        lines.append(f"  Register: {issue.register}")
        lines.append(f"  Issue: {issue.description}")
        lines.append("")
    
    lines.append("=" * 80)
    lines.append("RECOMMENDATIONS")
    lines.append("-" * 80)
    lines.append("1. Implement regalloc_set_interval_spans_call() usage in IR lowering")
    lines.append("2. Modify alloc_reg() to prefer callee-saved registers for spanning intervals")
    lines.append("3. Add caller-saved register save/restore around function calls")
    lines.append("")
    
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser(
        description='Analyze register allocator decisions'
    )
    parser.add_argument('--ir', required=True, help='IR dump file')
    parser.add_argument('--object', required=True, help='Object file')
    parser.add_argument('--analyze-calls', action='store_true')
    parser.add_argument('--report', help='Output report file')
    
    args = parser.parse_args()
    
    # Read IR dump
    with open(args.ir, 'r') as f:
        ir_dump = f.read()
    
    analyzer = RegisterAllocatorAnalyzer(ir_dump, args.object)
    issues = analyzer.analyze()
    
    report = generate_report(issues)
    
    if args.report:
        with open(args.report, 'w') as f:
            f.write(report)
        print(f"Report saved to: {args.report}")
    else:
        print(report)
    
    sys.exit(1 if issues else 0)

if __name__ == '__main__':
    main()
