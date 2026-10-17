#!/usr/bin/env python3
"""
IR Load/Store Analyzer

Analyzes IR to detect missing loads after stores, which can cause values to be lost.
Specifically looks for patterns like:
1. Value stored to memory
2. Value needed later but not reloaded
3. Store to same location without reload

Usage:
    python3 tools/ir_load_store_analyzer.py --ir ir_dump.txt
    python3 tools/ir_load_store_analyzer.py --ir ir_dump.txt --verbose
    python3 tools/ir_load_store_analyzer.py --ir ir_dump.txt --report report.txt
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
    temp_src1: Optional[int] = None
    temp_src2: Optional[int] = None
    mem_offset: Optional[int] = None
    imm: Optional[int] = None

@dataclass
class MemoryAccess:
    """Represents a memory access (load or store)"""
    instr_index: int
    access_type: str  # "load" or "store"
    temp: int
    offset: int
    value_temp: Optional[int] = None  # For stores, which temp holds the value

@dataclass
class MissingLoadIssue:
    """Represents a missing load issue"""
    function_name: str
    issue_type: str
    description: str
    store_instr: int
    use_instr: int
    offset: int
    severity: str  # "HIGH", "MEDIUM", "LOW"

class IRLoadStoreAnalyzer:
    """Analyzes IR for load/store bugs"""
    
    def __init__(self, ir_dump: str):
        self.ir_dump = ir_dump
        self.functions: Dict[str, List[IRInstruction]] = {}
        self.issues: List[MissingLoadIssue] = []
        self.current_function = None
        
    def analyze(self) -> List[MissingLoadIssue]:
        """Perform complete analysis"""
        # Parse IR
        self._parse_ir()
        
        # Analyze each function
        for func_name, instructions in self.functions.items():
            self._analyze_function(func_name, instructions)
        
        return self.issues
    
    def _parse_ir(self):
        """Parse IR dump into instructions by function"""
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
                temp_dest = None
                temp_src1 = None
                temp_src2 = None
                mem_offset = None
                imm = None
                
                # Extract temp IDs
                temps = re.findall(r'r(\d+)', operands)
                if temps:
                    temp_dest = int(temps[0]) if len(temps) > 0 else None
                    temp_src1 = int(temps[1]) if len(temps) > 1 else None
                    temp_src2 = int(temps[2]) if len(temps) > 2 else None
                
                # Extract memory offset
                offset_match = re.search(r'\[sp(-?\d+)\]', operands)
                if offset_match:
                    mem_offset = int(offset_match.group(1))
                
                # Extract immediate value
                imm_match = re.search(r',\s*(\d+)$', operands)
                if imm_match:
                    imm = int(imm_match.group(1))
                
                instr = IRInstruction(
                    index=idx,
                    op=op,
                    operands=operands,
                    temp_dest=temp_dest,
                    temp_src1=temp_src1,
                    temp_src2=temp_src2,
                    mem_offset=mem_offset,
                    imm=imm
                )
                
                instructions.append(instr)
        
        # Save last function
        if current_func and instructions:
            self.functions[current_func] = instructions
    
    def _analyze_function(self, func_name: str, instructions: List[IRInstruction]):
        """Analyze a single function for load/store bugs"""
        # Track memory stores: offset -> (temp, instr_index)
        stores: Dict[int, Tuple[int, int]] = {}
        
        # Track which temps have been "killed" (stored to memory and not reloaded)
        killed_temps: Set[int] = set()
        
        for instr in instructions:
            # Check for stores to memory
            if instr.op == 'MOV_MEM' and instr.mem_offset is not None:
                offset = instr.mem_offset
                src_temp = instr.temp_src1
                
                if src_temp is not None:
                    stores[offset] = (src_temp, instr.index)
                    killed_temps.add(src_temp)
            
            # Check for loads from memory
            elif instr.op == 'MOV' and instr.mem_offset is not None:
                offset = instr.mem_offset
                dst_temp = instr.temp_dest
                
                if dst_temp is not None:
                    # This is a load - mark the temp as "alive" again
                    if dst_temp in killed_temps:
                        killed_temps.remove(dst_temp)
            
            # Check for uses of killed temps
            elif instr.op in ['ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'AND', 'OR', 'XOR']:
                # Check if any source temp is killed
                for src_temp in [instr.temp_src1, instr.temp_src2]:
                    if src_temp is not None and src_temp in killed_temps:
                        # Find which offset killed this temp
                        for offset, (temp, store_idx) in stores.items():
                            if temp == src_temp:
                                issue = MissingLoadIssue(
                                    function_name=func_name,
                                    issue_type="missing_load_after_store",
                                    description=f"Temp r{src_temp} stored to [sp{offset}] at instr {store_idx} "
                                              f"but used at instr {instr.index} without reload",
                                    store_instr=store_idx,
                                    use_instr=instr.index,
                                    offset=offset,
                                    severity="HIGH"
                                )
                                self.issues.append(issue)
                                break
            
            # Check for function calls that might clobber temps
            elif instr.op == 'CALL':
                # After a call, all killed temps need to be reloaded
                # (callee may have clobbered registers, but memory should be preserved)
                pass
        
        # Final check: look for stores that are never used
        for offset, (temp, store_idx) in stores.items():
            # Check if this temp is ever loaded back
            found_load = False
            for instr in instructions:
                if instr.op == 'MOV' and instr.mem_offset == offset:
                    found_load = True
                    break
            
            if not found_load:
                # Check if temp is used elsewhere
                used_elsewhere = False
                for instr in instructions:
                    if instr.temp_src1 == temp or instr.temp_src2 == temp:
                        if instr.index > store_idx:
                            used_elsewhere = True
                            break
                
                if used_elsewhere:
                    issue = MissingLoadIssue(
                        function_name=func_name,
                        issue_type="store_never_reloaded",
                        description=f"Temp r{temp} stored to [sp{offset}] at instr {store_idx} "
                                  f"but never reloaded (used later in function)",
                        store_instr=store_idx,
                        use_instr=-1,
                        offset=offset,
                        severity="HIGH"
                    )
                    self.issues.append(issue)

def generate_report(issues: List[MissingLoadIssue], verbose: bool = False) -> str:
    """Generate human-readable report"""
    lines = []
    lines.append("=" * 80)
    lines.append("IR LOAD/STORE ANALYSIS REPORT")
    lines.append("=" * 80)
    lines.append("")
    
    if not issues:
        lines.append("✓ No load/store issues detected!")
        lines.append("")
        return "\n".join(lines)
    
    lines.append(f"Found {len(issues)} potential load/store issues:")
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
            lines.append(f"    Issue: {issue.description}")
            if verbose:
                lines.append(f"    Store at IR instruction: {issue.store_instr}")
                if issue.use_instr >= 0:
                    lines.append(f"    Use at IR instruction: {issue.use_instr}")
                lines.append(f"    Memory offset: [sp{issue.offset}]")
            lines.append("")
    
    lines.append("=" * 80)
    lines.append("RECOMMENDATIONS")
    lines.append("-" * 80)
    lines.append("1. Add MOV instructions to reload values from memory before use")
    lines.append("2. Check AST lowering code for missing load operations")
    lines.append("3. Verify stack slot management in expression evaluation")
    lines.append("")
    
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser(
        description='Analyze IR for missing load/store issues'
    )
    parser.add_argument('--ir', required=True, help='IR dump file')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    parser.add_argument('--report', help='Output report file')
    
    args = parser.parse_args()
    
    # Read IR dump
    with open(args.ir, 'r') as f:
        ir_dump = f.read()
    
    analyzer = IRLoadStoreAnalyzer(ir_dump)
    issues = analyzer.analyze()
    
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
