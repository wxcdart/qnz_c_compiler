#!/usr/bin/env python3
"""
IR Accumulator Bug Detector

Specifically detects the bug pattern where:
1. An accumulator value is computed (MOV_IMM)
2. Stored to memory (MOV_MEM)
3. Never reloaded before use
4. A different value is used instead

This is the exact bug pattern seen in the accumulation test.

Usage:
    python3 tools/ir_accumulator_bug_detector.py --ir ir_dump.txt
    python3 tools/ir_accumulator_bug_detector.py --ir ir_dump.txt --verbose
"""

import argparse
import re
import sys
from dataclasses import dataclass
from typing import List, Dict, Optional
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
class AccumulatorBug:
    """Represents an accumulator bug"""
    function_name: str
    acc_temp: int
    acc_value: int
    store_instr: int
    wrong_use_instr: int
    wrong_temp_used: int
    description: str

class IRAccumulatorBugDetector:
    """Detects accumulator bugs in IR"""
    
    def __init__(self, ir_dump: str):
        self.ir_dump = ir_dump
        self.functions: Dict[str, List[IRInstruction]] = {}
        self.bugs: List[AccumulatorBug] = []
        
    def detect(self) -> List[AccumulatorBug]:
        """Detect accumulator bugs"""
        self._parse_ir()
        
        for func_name, instructions in self.functions.items():
            self._detect_in_function(func_name, instructions)
        
        return self.bugs
    
    def _parse_ir(self):
        """Parse IR dump"""
        current_func = None
        instructions = []
        
        for line in self.ir_dump.split('\n'):
            line = line.strip()
            
            func_match = re.match(r'=== IR for function \'([^\']+)\' ===', line)
            if func_match:
                if current_func and instructions:
                    self.functions[current_func] = instructions
                current_func = func_match.group(1)
                instructions = []
                continue
            
            if not current_func:
                continue
            
            instr_match = re.match(r'^(\d+):\s+(\w+)\s+(.+)', line)
            if instr_match:
                idx = int(instr_match.group(1))
                op = instr_match.group(2)
                operands = instr_match.group(3).strip()
                
                # Remove [flags...] parts
                operands_clean = re.sub(r'\s*\[flags[^\]]*\]', '', operands)
                
                # Parse operands
                temps = re.findall(r'r(\d+)', operands_clean)
                
                # Special handling for MOV_MEM: format is MOV_MEM [mem], rsrc
                if op == 'MOV_MEM' and temps:
                    # rsrc is the source (being stored)
                    temp_src1 = int(temps[0])
                    temp_dest = None
                elif op not in ['CALL', 'JMP', 'JE', 'JNE', 'JLT', 'JLE', 'JGT', 'JGE', 'JCOND']:
                    temp_dest = int(temps[0]) if len(temps) > 0 else None
                    temp_src1 = int(temps[1]) if len(temps) > 1 else None
                    temp_src2 = int(temps[2]) if len(temps) > 2 else None
                else:
                    temp_dest = None
                    temp_src1 = None
                    temp_src2 = None
                
                offset_match = re.search(r'\[sp(-?\d+)\]', operands_clean)
                mem_offset = int(offset_match.group(1)) if offset_match else None
                
                imm_match = re.search(r',\s*(\d+)$', operands_clean)
                imm = int(imm_match.group(1)) if imm_match else None
                
                instr = IRInstruction(
                    index=idx,
                    op=op,
                    operands=operands_clean,
                    temp_dest=temp_dest,
                    temp_src1=temp_src1,
                    temp_src2=temp_src2,
                    mem_offset=mem_offset,
                    imm=imm
                )
                
                instructions.append(instr)
        
        if current_func and instructions:
            self.functions[current_func] = instructions
    
    def _detect_in_function(self, func_name: str, instructions: List[IRInstruction]):
        """Detect accumulator bugs in a function"""
        # Step 1: Find accumulator candidates (MOV_IMM to temp, then stored to memory)
        acc_candidates: Dict[int, Dict] = {}  # temp -> {value, store_instr, store_offset}
        
        for instr in instructions:
            if instr.op == 'MOV_IMM' and instr.temp_dest is not None and instr.imm is not None:
                # This temp holds an immediate value
                acc_candidates[instr.temp_dest] = {
                    'value': instr.imm,
                    'defined_at': instr.index,
                    'stored': False,
                    'store_instr': -1,
                    'store_offset': None
                }
            
            elif instr.op == 'MOV_MEM' and instr.mem_offset is not None and instr.temp_src1 is not None:
                temp = instr.temp_src1
                if temp in acc_candidates:
                    acc_candidates[temp]['stored'] = True
                    acc_candidates[temp]['store_instr'] = instr.index
                    acc_candidates[temp]['store_offset'] = instr.mem_offset
        
        # Step 2: Check for stores without reloads followed by arithmetic
        for temp, info in acc_candidates.items():
            if not info['stored']:
                continue
            
            store_instr = info['store_instr']
            store_offset = info['store_offset']
            acc_value = info['value']
            
            # Step 3: Look for arithmetic operations after the store
            for i in range(store_instr + 1, len(instructions)):
                instr = instructions[i]
                
                if instr.op in ['ADD', 'SUB', 'MUL', 'DIV', 'MOD']:
                    # Check if this arithmetic uses the accumulator temp
                    uses_acc = temp in [instr.temp_src1, instr.temp_src2]
                    
                    if not uses_acc:
                        # BUG: Arithmetic operation that should use accumulator
                        # but uses a different temp instead
                        wrong_temps = [t for t in [instr.temp_src1, instr.temp_src2] if t is not None and t != temp]
                        
                        if wrong_temps:
                            bug = AccumulatorBug(
                                function_name=func_name,
                                acc_temp=temp,
                                acc_value=acc_value,
                                store_instr=store_instr,
                                wrong_use_instr=i,
                                wrong_temp_used=wrong_temps[0],
                                description=f"Accumulator r{temp} (value {acc_value}) stored at instr {store_instr} "
                                          f"but r{wrong_temps[0]} used in {instr.op} at instr {i} instead of reloading"
                            )
                            self.bugs.append(bug)
                            break  # Only report first occurrence
        
        # Step 4: Check for stores that are never used
        for temp, info in acc_candidates.items():
            if not info['stored']:
                continue
            
            # Check if temp is used anywhere after store
            used_after = False
            for i in range(info['store_instr'] + 1, len(instructions)):
                instr = instructions[i]
                if temp in [instr.temp_src1, instr.temp_src2]:
                    used_after = True
                    break
            
            if not used_after:
                # Value stored but never used - might be a bug
                bug = AccumulatorBug(
                    function_name=func_name,
                    acc_temp=temp,
                    acc_value=info['value'],
                    store_instr=info['store_instr'],
                    wrong_use_instr=-1,
                    wrong_temp_used=-1,
                    description=f"Accumulator r{temp} (value {info['value']}) stored at instr {info['store_instr']} "
                              f"but never used (dead store?)"
                )
                self.bugs.append(bug)

def generate_report(bugs: List[AccumulatorBug], verbose: bool = False) -> str:
    """Generate report"""
    lines = []
    lines.append("=" * 80)
    lines.append("IR ACCUMULATOR BUG DETECTION REPORT")
    lines.append("=" * 80)
    lines.append("")
    
    if not bugs:
        lines.append("✓ No accumulator bugs detected!")
        lines.append("")
        return "\n".join(lines)
    
    lines.append(f"Found {len(bugs)} accumulator bug(s):")
    lines.append("")
    
    for i, bug in enumerate(bugs, 1):
        lines.append(f"Bug #{i}:")
        lines.append("-" * 80)
        lines.append(f"  Function: {bug.function_name}")
        lines.append(f"  Issue: {bug.description}")
        
        if verbose:
            lines.append(f"  Accumulator temp: r{bug.acc_temp}")
            lines.append(f"  Accumulator value: {bug.acc_value}")
            lines.append(f"  Store instruction: {bug.store_instr}")
            if bug.wrong_use_instr >= 0:
                lines.append(f"  Wrong use instruction: {bug.wrong_use_instr}")
                lines.append(f"  Wrong temp used: r{bug.wrong_temp_used}")
        
        lines.append("")
    
    lines.append("=" * 80)
    lines.append("RECOMMENDATIONS")
    lines.append("-" * 80)
    lines.append("1. Add MOV instruction to reload accumulator from memory before arithmetic")
    lines.append("2. Check AST lowering code for missing load operations after stores")
    lines.append("3. Verify expression evaluation order preserves accumulator values")
    lines.append("")
    lines.append("Example fix:")
    lines.append("  Before: ADD r21, r17, r20  ; r17 is flags, should be accumulator")
    lines.append("  After:  MOV rXX, [sp-...]  ; reload accumulator")
    lines.append("          ADD r21, rXX, r20  ; use reloaded accumulator")
    lines.append("")
    
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser(
        description='Detect accumulator bugs in IR'
    )
    parser.add_argument('--ir', required=True, help='IR dump file')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    parser.add_argument('--report', help='Output report file')
    
    args = parser.parse_args()
    
    # Read IR dump
    with open(args.ir, 'r') as f:
        ir_dump = f.read()
    
    detector = IRAccumulatorBugDetector(ir_dump)
    bugs = detector.detect()
    
    report = generate_report(bugs, args.verbose)
    
    # Output
    if args.report:
        with open(args.report, 'w') as f:
            f.write(report)
        print(f"Report saved to: {args.report}")
    else:
        print(report)
    
    # Exit code
    sys.exit(1 if bugs else 0)

if __name__ == '__main__':
    main()
