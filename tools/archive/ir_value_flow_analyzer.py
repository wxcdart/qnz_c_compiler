#!/usr/bin/env python3
"""
IR Value Flow Analyzer

Tracks value flow through IR instructions to detect when values are lost.
This helps identify bugs where:
1. A value is computed and stored to memory
2. The value is needed later but not reloaded
3. A register is overwritten before the value is used

Usage:
    python3 tools/ir_value_flow_analyzer.py --ir ir_dump.txt
    python3 tools/ir_value_flow_analyzer.py --ir ir_dump.txt --trace r16
    python3 tools/ir_value_flow_analyzer.py --ir ir_dump.txt --verbose
"""

import argparse
import re
import sys
from dataclasses import dataclass, field
from typing import List, Dict, Set, Optional, Tuple
from collections import defaultdict

@dataclass
class IRInstruction:
    """Represents an IR instruction"""
    index: int
    op: str
    operands: str
    temp_dest: Optional[int] = None
    temp_srcs: List[int] = field(default_factory=list)
    mem_offset: Optional[int] = None
    imm: Optional[int] = None

@dataclass
class ValueLocation:
    """Tracks where a value currently resides"""
    temp: int
    location_type: str  # "reg", "mem", "imm"
    location: int  # register number or memory offset
    defined_at: int  # instruction index where defined
    last_used_at: int = -1

@dataclass
class ValueFlowIssue:
    """Represents a value flow issue"""
    function_name: str
    issue_type: str
    temp: int
    description: str
    severity: str
    instr_index: int
    related_instrs: List[int] = field(default_factory=list)

class IRValueFlowAnalyzer:
    """Analyzes value flow through IR"""
    
    def __init__(self, ir_dump: str):
        self.ir_dump = ir_dump
        self.functions: Dict[str, List[IRInstruction]] = {}
        self.issues: List[ValueFlowIssue] = []
        
    def analyze(self, trace_temp: Optional[int] = None) -> List[ValueFlowIssue]:
        """Perform complete analysis"""
        # Parse IR
        self._parse_ir()
        
        # Analyze each function
        for func_name, instructions in self.functions.items():
            self._analyze_function(func_name, instructions, trace_temp)
        
        return self.issues
    
    def _parse_ir(self):
        """Parse IR dump"""
        current_func = None
        instructions = []
        
        for line in self.ir_dump.split('\n'):
            line = line.strip()
            
            # Function header
            func_match = re.match(r'=== IR for function \'([^\']+)\' ===', line)
            if func_match:
                if current_func and instructions:
                    self.functions[current_func] = instructions
                current_func = func_match.group(1)
                instructions = []
                continue
            
            if not current_func:
                continue
            
            # Instruction line
            instr_match = re.match(r'^(\d+):\s+(\w+)\s+(.*)$', line)
            if instr_match:
                idx = int(instr_match.group(1))
                op = instr_match.group(2)
                operands = instr_match.group(3)
                
                # Parse operands
                temps = re.findall(r'r(\d+)', operands)
                temp_dest = int(temps[0]) if len(temps) > 0 and op not in ['CALL', 'JMP', 'JE', 'JNE', 'JLT', 'JLE', 'JGT', 'JGE', 'JCOND'] else None
                temp_srcs = [int(t) for t in temps[1:]] if len(temps) > 1 else []
                
                # Extract memory offset
                offset_match = re.search(r'\[sp(-?\d+)\]', operands)
                mem_offset = int(offset_match.group(1)) if offset_match else None
                
                # Extract immediate value
                imm_match = re.search(r',\s*(\d+)$', operands)
                imm = int(imm_match.group(1)) if imm_match else None
                
                instr = IRInstruction(
                    index=idx,
                    op=op,
                    operands=operands,
                    temp_dest=temp_dest,
                    temp_srcs=temp_srcs,
                    mem_offset=mem_offset,
                    imm=imm
                )
                
                instructions.append(instr)
        
        if current_func and instructions:
            self.functions[current_func] = instructions
    
    def _analyze_function(self, func_name: str, instructions: List[IRInstruction], trace_temp: Optional[int]):
        """Analyze value flow in a function"""
        # Track where each temp's value currently resides
        value_locations: Dict[int, ValueLocation] = {}
        
        # Track which memory locations hold which temps
        mem_holds_temp: Dict[int, int] = {}  # offset -> temp
        
        for instr in instructions:
            # Check for stores to memory
            if instr.op == 'MOV_MEM' and instr.mem_offset is not None:
                src_temp = instr.temp_src1
                
                if src_temp is not None:
                    # Value moved from register to memory
                    if src_temp in value_locations:
                        loc = value_locations[src_temp]
                        loc.location_type = "mem"
                        loc.location = instr.mem_offset
                        loc.last_used_at = instr.index
                        mem_holds_temp[instr.mem_offset] = src_temp
                    
                    # Mark that this memory location holds a temp
                    mem_holds_temp[instr.mem_offset] = src_temp
            
            # Check for loads from memory
            elif instr.op == 'MOV' and instr.mem_offset is not None:
                dst_temp = instr.temp_dest
                
                if dst_temp is not None:
                    # Check if we're loading from a location that holds a temp
                    if instr.mem_offset in mem_holds_temp:
                        stored_temp = mem_holds_temp[instr.mem_offset]
                        # This is a reload - update the location
                        if stored_temp in value_locations:
                            loc = value_locations[stored_temp]
                            loc.location_type = "reg"
                            loc.location = dst_temp
                            loc.last_used_at = instr.index
            
            # Check for register-to-register moves
            elif instr.op == 'MOV_REG':
                dst_temp = instr.temp_dest
                src_temp = instr.temp_srcs[0] if instr.temp_srcs else None
                
                if src_temp is not None and dst_temp is not None:
                    if src_temp in value_locations:
                        loc = value_locations[src_temp]
                        loc.location_type = "reg"
                        loc.location = dst_temp
                        loc.last_used_at = instr.index
                        
                        # Update value location for dst_temp
                        value_locations[dst_temp] = loc
            
            # Check for arithmetic operations
            elif instr.op in ['ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'AND', 'OR', 'XOR', 'NEG']:
                dst_temp = instr.temp_dest
                src_temps = instr.temp_srcs
                
                # Check if any source temp was stored to memory and not reloaded
                for src_temp in src_temps:
                    if src_temp in value_locations:
                        loc = value_locations[src_temp]
                        if loc.location_type == "mem":
                            # BUG: Using a value that's in memory without loading it!
                            issue = ValueFlowIssue(
                                function_name=func_name,
                                issue_type="use_value_from_memory_without_load",
                                temp=src_temp,
                                description=f"Temp r{src_temp} is in memory at [sp{loc.location}] "
                                          f"(stored at instr {loc.defined_at}) but used in {instr.op} at instr {instr.index}",
                                severity="HIGH",
                                instr_index=instr.index,
                                related_instrs=[loc.defined_at]
                            )
                            self.issues.append(issue)
                
                # Mark new value
                if dst_temp is not None:
                    value_locations[dst_temp] = ValueLocation(
                        temp=dst_temp,
                        location_type="reg",
                        location=dst_temp,
                        defined_at=instr.index
                    )
            
            # Check for immediate loads
            elif instr.op == 'MOV_IMM':
                dst_temp = instr.temp_dest
                if dst_temp is not None:
                    value_locations[dst_temp] = ValueLocation(
                        temp=dst_temp,
                        location_type="imm",
                        location=instr.imm or 0,
                        defined_at=instr.index
                    )
            
            # Check for function calls
            elif instr.op == 'CALL':
                # After a call, all register values may be clobbered
                # but memory values should be preserved
                pass
            
            # Check for uses of temps
            else:
                # Check if any source temp is used
                for src_temp in instr.temp_srcs:
                    if src_temp in value_locations:
                        value_locations[src_temp].last_used_at = instr.index
        
        # Final check: look for values stored to memory that are never reloaded
        for temp, loc in value_locations.items():
            if loc.location_type == "mem":
                # Check if this temp is used anywhere after the store
                used_after = False
                for instr in instructions:
                    if instr.index > loc.defined_at:
                        if temp in instr.temp_srcs or (instr.temp_dest is not None and temp == instr.temp_dest):
                            used_after = True
                            break
                
                if used_after:
                    issue = ValueFlowIssue(
                        function_name=func_name,
                        issue_type="value_stored_never_reloaded",
                        temp=temp,
                        description=f"Temp r{temp} stored to memory at instr {loc.defined_at} "
                                  f"but used later without reload",
                        severity="HIGH",
                        instr_index=loc.defined_at
                    )
                    self.issues.append(issue)
    
    def generate_trace(self, temp_id: int) -> str:
        """Generate trace for a specific temp"""
        lines = []
        lines.append(f"=== Value Flow Trace for r{temp_id} ===")
        lines.append("")
        
        for func_name, instructions in self.functions.items():
            lines.append(f"Function: {func_name}")
            lines.append("-" * 60)
            
            for instr in instructions:
                if temp_id in instr.temp_srcs or (instr.temp_dest is not None and temp_id == instr.temp_dest):
                    lines.append(f"  Instr {instr.index:3d}: {instr.op:15s} {instr.operands}")
            
            lines.append("")
        
        return "\n".join(lines)

def generate_report(issues: List[ValueFlowIssue], verbose: bool = False) -> str:
    """Generate report"""
    lines = []
    lines.append("=" * 80)
    lines.append("IR VALUE FLOW ANALYSIS REPORT")
    lines.append("=" * 80)
    lines.append("")
    
    if not issues:
        lines.append("✓ No value flow issues detected!")
        lines.append("")
        return "\n".join(lines)
    
    lines.append(f"Found {len(issues)} value flow issues:")
    lines.append("")
    
    # Group by function
    by_func = defaultdict(list)
    for issue in issues:
        by_func[issue.function_name].append(issue)
    
    for func_name, func_issues in sorted(by_func.items()):
        lines.append(f"Function: {func_name}")
        lines.append("-" * 80)
        
        for issue in func_issues:
            lines.append(f"  [SEVERITY: {issue.severity}] {issue.issue_type}")
            lines.append(f"    Temp: r{issue.temp}")
            lines.append(f"    Issue: {issue.description}")
            if verbose:
                lines.append(f"    Instruction: {issue.instr_index}")
                if issue.related_instrs:
                    lines.append(f"    Related: {issue.related_instrs}")
            lines.append("")
    
    lines.append("=" * 80)
    lines.append("RECOMMENDATIONS")
    lines.append("-" * 80)
    lines.append("1. Add MOV instructions to reload values from memory before use")
    lines.append("2. Check expression lowering code for missing load operations")
    lines.append("3. Verify stack slot management in AST lowering")
    lines.append("")
    
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser(
        description='Analyze value flow through IR'
    )
    parser.add_argument('--ir', required=True, help='IR dump file')
    parser.add_argument('--trace', type=int, help='Trace a specific temp (e.g., --trace 16)')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    parser.add_argument('--report', help='Output report file')
    
    args = parser.parse_args()
    
    # Read IR dump
    with open(args.ir, 'r') as f:
        ir_dump = f.read()
    
    analyzer = IRValueFlowAnalyzer(ir_dump)
    issues = analyzer.analyze(trace_temp=args.trace)
    
    # Generate trace if requested
    if args.trace:
        print(analyzer.generate_trace(args.trace))
    
    report = generate_report(issues, args.verbose)
    
    # Output
    if args.report:
        with open(args.report, 'w') as f:
            f.write(report)
        print(f"Report saved to: {args.report}")
    else:
        print(report)
    
    # Exit code
    sys.exit(1 if issues else 0)

if __name__ == '__main__':
    main()
