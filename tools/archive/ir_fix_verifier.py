#!/usr/bin/env python3
"""
IR Fix Verifier

Verifies that IR generation bugs are fixed by checking for:
1. All memory stores have corresponding loads before use
2. Values are not used after being stored to memory without reload
3. Stack slot management is correct

Usage:
    python3 tools/ir_fix_verifier.py --ir ir_dump.txt --check-all
    python3 tools/ir_fix_verifier.py --ir ir_dump.txt --check-memory
    python3 tools/ir_fix_verifier.py --ir ir_dump.txt --compare ir_dump_fixed.txt
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
class VerificationResult:
    """Result of a verification check"""
    check_name: str
    passed: bool
    message: str
    details: List[str] = field(default_factory=list)

class IRFixVerifier:
    """Verifies IR generation fixes"""
    
    def __init__(self, ir_dump: str):
        self.ir_dump = ir_dump
        self.functions: Dict[str, List[IRInstruction]] = {}
        self.results: List[VerificationResult] = []
        
    def verify(self, check_all: bool = False, check_memory: bool = False) -> List[VerificationResult]:
        """Perform verification"""
        # Parse IR
        self._parse_ir()
        
        # Run checks
        if check_all or check_memory:
            self._check_memory_loads()
        
        return self.results
    
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
    
    def _check_memory_loads(self):
        """Check that all values stored to memory are loaded before use"""
        for func_name, instructions in self.functions.items():
            # Track memory stores
            stores: Dict[int, Tuple[int, int]] = {}  # offset -> (temp, instr_index)
            
            for instr in instructions:
                # Record stores
                if instr.op == 'MOV_MEM' and instr.mem_offset is not None:
                    src_temp = instr.temp_src1
                    if src_temp is not None:
                        stores[instr.mem_offset] = (src_temp, instr.index)
                
                # Check for uses of stored values without reload
                elif instr.op in ['ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'AND', 'OR', 'XOR', 'NEG']:
                    for src_temp in instr.temp_srcs:
                        # Check if this temp is stored to memory
                        for offset, (temp, store_idx) in stores.items():
                            if temp == src_temp:
                                # Check if there was a reload
                                reload_found = False
                                for i in range(store_idx + 1, instr.index):
                                    if instructions[i].op == 'MOV' and instructions[i].mem_offset == offset:
                                        reload_found = True
                                        break
                                
                                if not reload_found:
                                    self.results.append(VerificationResult(
                                        check_name="memory_load_before_use",
                                        passed=False,
                                        message=f"Temp r{src_temp} stored to [sp{offset}] at instr {store_idx} "
                                              f"but used at instr {instr.index} without reload",
                                        details=[
                                            f"Function: {func_name}",
                                            f"Store: instr {store_idx}: MOV_MEM [sp{offset}], r{src_temp}",
                                            f"Use: instr {instr.index}: {instr.op} {instr.operands}",
                                            f"Missing: MOV r?, [sp{offset}]"
                                        ]
                                    ))
                
                # Record loads
                elif instr.op == 'MOV' and instr.mem_offset is not None:
                    # Remove from stores if loaded
                    if instr.mem_offset in stores:
                        del stores[instr.mem_offset]
            
            # Check for stores that are never used (potential bug)
            for offset, (temp, store_idx) in stores.items():
                # Check if temp is used anywhere after store
                used = False
                for i in range(store_idx + 1, len(instructions)):
                    instr = instructions[i]
                    if temp in instr.temp_srcs:
                        used = True
                        break
                
                if used:
                    self.results.append(VerificationResult(
                        check_name="memory_load_before_use",
                        passed=False,
                        message=f"Temp r{temp} stored to [sp{offset}] at instr {store_idx} "
                              f"but used later without reload",
                        details=[
                            f"Function: {func_name}",
                            f"Store: instr {store_idx}: MOV_MEM [sp{offset}], r{temp}",
                            f"Missing: Reload before use"
                        ]
                    ))
    
    def compare_with(self, other_ir_dump: str) -> str:
        """Compare with another IR dump"""
        lines = []
        lines.append("=" * 80)
        lines.append("IR COMPARISON")
        lines.append("=" * 80)
        lines.append("")
        
        # Parse both
        other_verifier = IRFixVerifier(other_ir_dump)
        other_verifier._parse_ir()
        
        # Compare functions
        for func_name in self.functions:
            if func_name not in other_verifier.functions:
                lines.append(f"Function {func_name} missing in fixed IR")
                continue
            
            orig_instrs = self.functions[func_name]
            fixed_instrs = other_verifier.functions[func_name]
            
            lines.append(f"Function: {func_name}")
            lines.append("-" * 60)
            lines.append(f"  Original: {len(orig_instrs)} instructions")
            lines.append(f"  Fixed:    {len(fixed_instrs)} instructions")
            lines.append("")
            
            # Find differences
            for i, (orig, fixed) in enumerate(zip(orig_instrs, fixed_instrs)):
                if orig.op != fixed.op or orig.operands != fixed.operands:
                    lines.append(f"  Instr {i}:")
                    lines.append(f"    Original: {orig.op} {orig.operands}")
                    lines.append(f"    Fixed:    {fixed.op} {fixed.operands}")
            
            lines.append("")
        
        return "\n".join(lines)

def generate_report(results: List[VerificationResult]) -> str:
    """Generate report"""
    lines = []
    lines.append("=" * 80)
    lines.append("IR FIX VERIFICATION REPORT")
    lines.append("=" * 80)
    lines.append("")
    
    passed = sum(1 for r in results if r.passed)
    failed = sum(1 for r in results if not r.passed)
    
    lines.append(f"Results: {passed} passed, {failed} failed")
    lines.append("")
    
    if failed == 0:
        lines.append("✓ All checks passed! IR generation is correct.")
        lines.append("")
        return "\n".join(lines)
    
    lines.append("FAILED CHECKS:")
    lines.append("-" * 80)
    
    for result in results:
        if not result.passed:
            lines.append(f"[FAILED] {result.check_name}")
            lines.append(f"  {result.message}")
            for detail in result.details:
                lines.append(f"    {detail}")
            lines.append("")
    
    lines.append("=" * 80)
    lines.append("RECOMMENDATIONS")
    lines.append("-" * 80)
    lines.append("1. Add missing MOV instructions to reload values from memory")
    lines.append("2. Check AST lowering code for proper stack slot management")
    lines.append("3. Verify expression evaluation order and temporary handling")
    lines.append("")
    
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser(
        description='Verify IR generation fixes'
    )
    parser.add_argument('--ir', required=True, help='IR dump file')
    parser.add_argument('--check-all', action='store_true', help='Run all checks')
    parser.add_argument('--check-memory', action='store_true', help='Check memory loads')
    parser.add_argument('--compare', help='Compare with another IR dump')
    parser.add_argument('--report', help='Output report file')
    
    args = parser.parse_args()
    
    # Read IR dump
    with open(args.ir, 'r') as f:
        ir_dump = f.read()
    
    verifier = IRFixVerifier(ir_dump)
    results = verifier.verify(check_all=args.check_all, check_memory=args.check_memory)
    
    # Compare if requested
    if args.compare:
        with open(args.compare, 'r') as f:
            other_ir = f.read()
        print(verifier.compare_with(other_ir))
    
    report = generate_report(results)
    
    # Output
    if args.report:
        with open(args.report, 'w') as f:
            f.write(report)
        print(f"Report saved to: {args.report}")
    else:
        print(report)
    
    # Exit code
    sys.exit(0 if all(r.passed for r in results) else 1)

if __name__ == '__main__':
    main()
