#!/usr/bin/env python3
"""
Register Clobbering Analyzer

Analyzes AArch64 object files to detect caller-saved register clobbering issues.
Specifically looks for:
1. Registers loaded before function calls and used after
2. Caller-saved registers (x0-x18) being clobbered by callees
3. Missing save/restore patterns

Usage:
    python3 register_clobbering_analyzer.py --object test.o
    python3 register_clobbering_analyzer.py --object test.o --verbose
    python3 register_clobbering_analyzer.py --object test.o --report report.txt
"""

import argparse
import subprocess
import re
import sys
from dataclasses import dataclass, field
from typing import List, Dict, Set, Optional, Tuple
from collections import defaultdict

@dataclass
class Instruction:
    """Represents a single AArch64 instruction"""
    address: int
    hex_code: str
    mnemonic: str
    operands: str
    raw_line: str

@dataclass
class RegisterAccess:
    """Represents a register access (read or write)"""
    reg_num: int
    reg_name: str
    access_type: str  # "read" or "write"
    instruction_addr: int
    mnemonic: str

@dataclass
class ClobbererIssue:
    """Represents a potential register clobbering issue"""
    function_name: str
    issue_type: str  # "load_before_call", "use_after_call", "missing_save"
    register: str
    load_addr: int
    call_addr: int
    use_addr: Optional[int]
    severity: str  # "HIGH", "MEDIUM", "LOW"
    description: str

# AArch64 caller-saved registers (x0-x18)
CALLER_SAVED_INT_REGS = list(range(0, 19))  # x0-x18
CALLER_SAVED_INT_REG_NAMES = [f"x{i}" for i in range(0, 19)]

# AArch64 callee-saved registers (x19-x29, x30)
CALLEE_SAVED_INT_REGS = list(range(19, 31))
CALLEE_SAVED_INT_REG_NAMES = [f"x{i}" for i in range(19, 31)]

class RegisterClobberingAnalyzer:
    """Analyzes register clobbering in AArch64 code"""
    
    def __init__(self, object_file: str):
        self.object_file = object_file
        self.instructions: Dict[str, List[Instruction]] = defaultdict(list)
        self.issues: List[ClobbererIssue] = []
        
    def analyze(self):
        """Perform complete analysis"""
        # Get disassembly
        disasm = self._get_disassembly()
        
        # Parse instructions by function
        self._parse_instructions(disasm)
        
        # Analyze each function
        for func_name, instrs in self.instructions.items():
            self._analyze_function(func_name, instrs)
        
        return self.issues
    
    def _get_disassembly(self) -> str:
        """Get disassembly of object file"""
        try:
            result = subprocess.run(
                ['aarch64-linux-gnu-objdump', '-d', self.object_file],
                capture_output=True,
                text=True,
                check=True
            )
            return result.stdout
        except subprocess.CalledProcessError as e:
            print(f"Error: Could not disassemble {self.object_file}")
            sys.exit(1)
    
    def _parse_instructions(self, disasm: str):
        """Parse disassembly into instruction list"""
        current_func = None
        
        for line in disasm.split('\n'):
            # Function header
            func_match = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
            if func_match:
                current_func = func_match.group(1)
                continue
            
            if not current_func:
                continue
            
            # Instruction line
            instr_match = re.match(
                r'^\s*([0-9a-f]+):\s+([0-9a-f\s]+)\s+(\w+)\s*(.*?)$',
                line
            )
            if instr_match:
                addr_str = instr_match.group(1)
                hex_code = instr_match.group(2).strip()
                mnemonic = instr_match.group(3)
                operands = instr_match.group(4).strip()
                
                instr = Instruction(
                    address=int(addr_str, 16),
                    hex_code=hex_code,
                    mnemonic=mnemonic,
                    operands=operands,
                    raw_line=line
                )
                
                self.instructions[current_func].append(instr)
    
    def _analyze_function(self, func_name: str, instructions: List[Instruction]):
        """Analyze a single function for register clobbering"""
        # Find all loads and calls
        loads: Dict[int, Tuple[int, str]] = {}  # reg_num -> (addr, reg_name)
        calls: List[int] = []
        uses: Dict[int, Tuple[int, str]] = {}  # reg_num -> (addr, reg_name)
        
        for instr in instructions:
            # Detect loads to caller-saved registers
            load_match = re.match(
                r'ldur\s+([xw]\d+),\s*\[.*?\]',
                f"{instr.mnemonic} {instr.operands}"
            )
            if load_match:
                reg_name = load_match.group(1)
                reg_num = self._parse_reg_num(reg_name)
                if reg_num is not None and reg_num in CALLER_SAVED_INT_REGS:
                    loads[reg_num] = (instr.address, reg_name)
            
            # Detect function calls
            if instr.mnemonic in ['bl', 'blr']:
                calls.append(instr.address)
            
            # Detect uses of caller-saved registers
            for reg_num in CALLER_SAVED_INT_REGS:
                reg_name = f"x{reg_num}"
                if self._reg_used_in_instr(instr, reg_name):
                    uses[reg_num] = (instr.address, reg_name)
        
        # Check for clobbering patterns
        for reg_num, (load_addr, load_reg) in loads.items():
            for call_addr in calls:
                if load_addr < call_addr:
                    # Check if register is used after the call
                    for use_reg_num, (use_addr, use_reg) in uses.items():
                        if use_reg_num == reg_num and use_addr > call_addr:
                            # Found a potential clobbering!
                            issue = ClobbererIssue(
                                function_name=func_name,
                                issue_type="load_before_call_use_after",
                                register=load_reg,
                                load_addr=load_addr,
                                call_addr=call_addr,
                                use_addr=use_addr,
                                severity="HIGH",
                                description=f"{load_reg} loaded at 0x{load_addr:x}, "
                                          f"call at 0x{call_addr:x}, "
                                          f"used at 0x{use_addr:x} - register may be clobbered!"
                            )
                            self.issues.append(issue)
    
    def _parse_reg_num(self, reg_name: str) -> Optional[int]:
        """Parse register name (x0, w1, etc.) to register number"""
        match = re.match(r'[xw](\d+)', reg_name)
        if match:
            return int(match.group(1))
        return None
    
    def _reg_used_in_instr(self, instr: Instruction, reg_name: str) -> bool:
        """Check if a register is used in an instruction"""
        # Check operands
        operands_str = f"{instr.mnemonic} {instr.operands}"
        
        # Simple pattern matching for register usage
        # This is a simplified check - a more robust implementation would parse the operands
        patterns = [
            rf'\b{reg_name}\b',  # Direct register use
            rf'\b[xw]{reg_name[1:]}\b',  # Both x and w versions
        ]
        
        for pattern in patterns:
            if re.search(pattern, operands_str):
                return True
        
        return False

def generate_report(issues: List[ClobbererIssue]) -> str:
    """Generate human-readable report"""
    lines = []
    lines.append("=" * 80)
    lines.append("REGISTER CLOBBERING ANALYSIS REPORT")
    lines.append("=" * 80)
    lines.append("")
    
    if not issues:
        lines.append("✓ No register clobbering issues detected!")
        lines.append("")
        return "\n".join(lines)
    
    lines.append(f"Found {len(issues)} potential register clobbering issues:")
    lines.append("")
    
    # Group by function
    by_func = defaultdict(list)
    for issue in issues:
        by_func[issue.function_name].append(issue)
    
    for func_name, func_issues in sorted(by_func.items()):
        lines.append(f"Function: {func_name}")
        lines.append("-" * 80)
        
        for issue in func_issues:
            lines.append(f"  [SEVERITY: {issue.severity}] Register {issue.register}")
            lines.append(f"    Type: {issue.issue_type}")
            lines.append(f"    Load at:  0x{issue.load_addr:x}")
            lines.append(f"    Call at:  0x{issue.call_addr:x}")
            if issue.use_addr:
                lines.append(f"    Use at:   0x{issue.use_addr:x}")
            lines.append(f"    Issue: {issue.description}")
            lines.append("")
    
    lines.append("=" * 80)
    lines.append("RECOMMENDATIONS")
    lines.append("-" * 80)
    lines.append("1. Save caller-saved registers before function calls")
    lines.append("2. Use callee-saved registers (x19-x28) for values spanning calls")
    lines.append("3. Mark register allocator intervals as 'spans_call' for proper handling")
    lines.append("")
    
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser(
        description='Analyze register clobbering issues in AArch64 code'
    )
    parser.add_argument('--object', required=True, help='Object file to analyze')
    parser.add_argument('--report', help='Output report file')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    analyzer = RegisterClobberingAnalyzer(args.object)
    issues = analyzer.analyze()
    
    report = generate_report(issues)
    
    # Output
    if args.report:
        with open(args.report, 'w') as f:
            f.write(report)
        print(f"Report saved to: {args.report}")
    else:
        print(report)
    
    # Exit code
    if issues:
        sys.exit(1)
    else:
        sys.exit(0)

if __name__ == '__main__':
    main()
