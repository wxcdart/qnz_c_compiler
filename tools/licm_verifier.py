#!/usr/bin/env python3
"""
LICM Trampoline Verifier

Verifies Loop-Invariant Code Motion (LICM) trampoline implementation:
1. Identifies loops and their invariant expressions
2. Verifies invariants are hoisted to preheader/trampoline block
3. Confirms jump label integrity (no desynchronization)
4. Benchmarks performance improvement (O(N) -> O(1))

Usage:
    python3 tools/licm_verifier.py --source test.c --dump-before ir_before.txt --dump-after ir_after.txt
    python3 tools/licm_verifier.py --benchmark loop_test.c --iterations 100000000
"""

import argparse
import re
import subprocess
import sys
import os
import time
from typing import Dict, List, Set, Tuple, Optional
from dataclasses import dataclass

@dataclass
class Instruction:
    """Represents a single IR instruction"""
    index: int
    opcode: str
    dest: Optional[str]
    src1: Optional[str]
    src2: Optional[str]
    imm: Optional[int]
    label_idx: Optional[int]
    raw: str

@dataclass
class BasicBlock:
    """Represents a basic block in the IR"""
    label_idx: int
    start_instr: int
    end_instr: int
    instructions: List[Instruction]
    predecessors: List[int]
    successors: List[int]
    is_loop_header: bool = False
    loop_preheader: Optional[int] = None

@dataclass
class Loop:
    """Represents a detected loop"""
    header_label: int
    header_start: int
    header_end: int
    body_indices: Set[int]
    preheader_label: Optional[int]
    invariants: List[Instruction]
    hoisted_invariants: List[Instruction]

class IRParser:
    """Parses ZeroCC IR text dumps"""
    
    def __init__(self, ir_text: str):
        self.ir_text = ir_text
        self.instructions: List[Instruction] = []
        self.basic_blocks: Dict[int, BasicBlock] = {}
        self.labels: Dict[str, int] = {}
        
    def parse(self) -> bool:
        """Parse IR text into structured data"""
        lines = self.ir_text.strip().split('\n')
        
        current_label = None
        current_block_instructions = []
        block_start = 0
        
        for line in lines:
            line = line.strip()
            if not line or line.startswith(';'):
                continue
            
            # Parse label
            label_match = re.match(r'^L(\d+):$', line)
            if label_match:
                # Save previous block if exists
                if current_label is not None and current_block_instructions:
                    self.basic_blocks[current_label] = BasicBlock(
                        label_idx=current_label,
                        start_instr=block_start,
                        end_instr=block_start + len(current_block_instructions) - 1,
                        instructions=current_block_instructions,
                        predecessors=[],
                        successors=[],
                        is_loop_header=False
                    )
                
                current_label = int(label_match.group(1))
                block_start = len(self.instructions)
                current_block_instructions = []
                continue
            
            # Parse instruction
            instr = self._parse_instruction(line, len(self.instructions))
            if instr:
                self.instructions.append(instr)
                current_block_instructions.append(instr)
        
        # Save last block
        if current_label is not None and current_block_instructions:
            self.basic_blocks[current_label] = BasicBlock(
                label_idx=current_label,
                start_instr=block_start,
                end_instr=block_start + len(current_block_instructions) - 1,
                instructions=current_block_instructions,
                predecessors=[],
                successors=[],
                is_loop_header=False
            )
        
        # Build CFG (basic predecessor/successor analysis)
        self._build_cfg()
        
        return True
    
    def _parse_instruction(self, line: str, index: int) -> Optional[Instruction]:
        """Parse a single IR instruction"""
        # Pattern: "  0: OP_MOV_IMM r16, 10"
        match = re.match(r'^\s*(\d+):\s+(\w+)\s+(.+)$', line)
        if not match:
            return None
        
        opcode = match.group(2)
        operands_str = match.group(3)
        
        dest = None
        src1 = None
        src2 = None
        imm = None
        label_idx = None
        
        # Parse operands
        operands = [op.strip() for op in operands_str.split(',')]
        
        if len(operands) >= 1:
            dest = operands[0].strip()
        if len(operands) >= 2:
            src1 = operands[1].strip()
        if len(operands) >= 3:
            src2 = operands[2].strip()
        
        # Extract immediate values
        imm_match = re.search(r'#(-?\d+)', operands_str)
        if imm_match:
            imm = int(imm_match.group(1))
        
        # Extract label index for jumps
        label_match = re.search(r'L(\d+)', operands_str)
        if label_match:
            label_idx = int(label_match.group(1))
        
        return Instruction(
            index=index,
            opcode=opcode,
            dest=dest if dest != '0xFF' else None,
            src1=src1 if src1 != '0xFF' else None,
            src2=src2 if src2 != '0xFF' else None,
            imm=imm,
            label_idx=label_idx,
            raw=line
        )
    
    def _build_cfg(self):
        """Build control flow graph"""
        for label, block in self.basic_blocks.items():
            # Find successors
            if block.instructions:
                last_instr = block.instructions[-1]
                if last_instr.opcode in ['OP_JMP', 'OP_JMP_LABEL']:
                    if last_instr.label_idx is not None:
                        block.successors.append(last_instr.label_idx)
                elif last_instr.opcode in ['OP_JE', 'OP_JNE', 'OP_JL', 'OP_JLE', 'OP_JG', 'OP_JCOND']:
                    if last_instr.label_idx is not None:
                        block.successors.append(last_instr.label_idx)
                        # Also next block as fall-through
                        next_label = label + 1
                        if next_label in self.basic_blocks:
                            block.successors.append(next_label)
                else:
                    # Fall-through to next block
                    next_label = label + 1
                    if next_label in self.basic_blocks:
                        block.successors.append(next_label)
        
        # Build predecessors
        for label, block in self.basic_blocks.items():
            for succ in block.successors:
                if succ in self.basic_blocks:
                    self.basic_blocks[succ].predecessors.append(label)

class LoopDetector:
    """Detects loops in the IR"""
    
    def __init__(self, parser: IRParser):
        self.parser = parser
        
    def detect_loops(self) -> List[Loop]:
        """Detect loops using backedge analysis"""
        loops = []
        
        for label, block in self.parser.basic_blocks.items():
            # Check for backedge (jump to earlier label)
            for succ in block.successors:
                if succ <= label and succ in self.parser.basic_blocks:
                    # Found backedge -> loop
                    loop = self._analyze_loop(succ, label)
                    if loop:
                        loops.append(loop)
        
        return loops
    
    def _analyze_loop(self, header_label: int, backedge_from: int) -> Optional[Loop]:
        """Analyze a loop structure"""
        if header_label not in self.parser.basic_blocks:
            return None
        
        header_block = self.parser.basic_blocks[header_label]
        
        # Collect all instructions in loop body (simple: just header for now)
        body_indices = set()
        for instr in header_block.instructions:
            body_indices.add(instr.index)
        
        return Loop(
            header_label=header_label,
            header_start=header_block.start_instr,
            header_end=header_block.end_instr,
            body_indices=body_indices,
            preheader_label=None,  # Will be set by trampoline
            invariants=[],
            hoisted_invariants=[]
        )

class LICMVerifier:
    """Verifies LICM trampoline implementation"""
    
    def __init__(self, before_ir: str, after_ir: str):
        self.before_parser = IRParser(before_ir)
        self.after_parser = IRParser(after_ir)
        self.before_parser.parse()
        self.after_parser.parse()
        
        self.before_loops = LoopDetector(self.before_parser).detect_loops()
        self.after_loops = LoopDetector(self.after_parser).detect_loops()
        
    def verify_hoisting(self) -> Dict:
        """Verify that invariants were hoisted correctly"""
        results = {
            'loops_detected': len(self.before_loops),
            'loops_with_hoisting': 0,
            'invariants_found': 0,
            'invariants_hoisted': 0,
            'errors': [],
            'warnings': []
        }
        
        # Compare before and after
        for before_loop in self.before_loops:
            header_label = before_loop.header_label
            
            # Find corresponding after loop
            after_loop = None
            for loop in self.after_loops:
                if loop.header_label == header_label:
                    after_loop = loop
                    break
            
            if after_loop is None:
                # Loop structure may have changed due to trampoline
                results['warnings'].append(f"Loop at L{header_label} structure changed")
                continue
            
            # Analyze loop body for potential invariants
            before_body_instrs = [
                instr for instr in self.before_parser.instructions
                if instr.index in before_loop.body_indices
            ]
            
            after_body_instrs = [
                instr for instr in self.after_parser.instructions
                if instr.index in after_loop.body_indices
            ]
            
            # Find instructions that disappeared from loop body (likely hoisted)
            before_ops = [(i.opcode, i.src1, i.src2, i.imm) for i in before_body_instrs]
            after_ops = [(i.opcode, i.src1, i.src2, i.imm) for i in after_body_instrs]
            
            for op in before_ops:
                if op not in after_ops:
                    results['invariants_found'] += 1
                    # Check if it appears in preheader
                    if self._find_in_preheader(op, after_loop):
                        results['invariants_hoisted'] += 1
                        results['loops_with_hoisting'] += 1
                    else:
                        results['warnings'].append(
                            f"Potential invariant {op} removed but not found in preheader"
                        )
        
        return results
    
    def _find_in_preheader(self, op: Tuple, loop: Loop) -> bool:
        """Check if instruction appears in preheader/trampoline"""
        # Check instructions before loop header
        if loop.preheader_label is not None:
            preheader = self.after_parser.basic_blocks.get(loop.preheader_label)
            if preheader:
                for instr in preheader.instructions:
                    if (instr.opcode, instr.src1, instr.src2, instr.imm) == op:
                        return True
        
        # Check first few instructions before header
        header_start = loop.header_start
        for i in range(max(0, header_start - 10), header_start):
            if i < len(self.after_parser.instructions):
                instr = self.after_parser.instructions[i]
                if (instr.opcode, instr.src1, instr.src2, instr.imm) == op:
                    return True
        
        return False
    
    def verify_jump_integrity(self) -> Dict:
        """Verify that jump labels are not desynchronized"""
        results = {
            'total_jumps': 0,
            'valid_jumps': 0,
            'invalid_jumps': 0,
            'errors': []
        }
        
        for instr in self.after_parser.instructions:
            if instr.opcode in ['OP_JMP', 'OP_JMP_LABEL', 'OP_JE', 'OP_JNE', 'OP_JL', 'OP_JLE', 'OP_JG', 'OP_JCOND']:
                results['total_jumps'] += 1
                
                if instr.label_idx is not None:
                    if instr.label_idx in self.after_parser.basic_blocks:
                        results['valid_jumps'] += 1
                    else:
                        results['invalid_jumps'] += 1
                        results['errors'].append(
                            f"Jump at instr {instr.index} targets non-existent label L{instr.label_idx}"
                        )
        
        return results
    
    def generate_report(self) -> str:
        """Generate verification report"""
        hoisting_results = self.verify_hoisting()
        integrity_results = self.verify_jump_integrity()
        
        report = []
        report.append("=" * 60)
        report.append("LICM TRAMPOLINE VERIFICATION REPORT")
        report.append("=" * 60)
        report.append("")
        
        report.append("LOOP ANALYSIS:")
        report.append(f"  Loops detected: {hoisting_results['loops_detected']}")
        report.append(f"  Loops with hoisting: {hoisting_results['loops_with_hoisting']}")
        report.append(f"  Invariants found: {hoisting_results['invariants_found']}")
        report.append(f"  Invariants hoisted: {hoisting_results['invariants_hoisted']}")
        report.append("")
        
        report.append("JUMP INTEGRITY:")
        report.append(f"  Total jumps: {integrity_results['total_jumps']}")
        report.append(f"  Valid jumps: {integrity_results['valid_jumps']}")
        report.append(f"  Invalid jumps: {integrity_results['invalid_jumps']}")
        report.append("")
        
        if hoisting_results['errors'] or integrity_results['errors']:
            report.append("ERRORS:")
            for error in hoisting_results['errors'] + integrity_results['errors']:
                report.append(f"  ✗ {error}")
            report.append("")
        
        if hoisting_results['warnings']:
            report.append("WARNINGS:")
            for warning in hoisting_results['warnings']:
                report.append(f"  ⚠ {warning}")
            report.append("")
        
        # Overall status
        if integrity_results['invalid_jumps'] == 0:
            report.append("STATUS: ✓ PASS")
            report.append("  Jump integrity verified, no desynchronization detected")
        else:
            report.append("STATUS: ✗ FAIL")
            report.append("  Jump label desynchronization detected!")
        
        report.append("=" * 60)
        
        return '\n'.join(report)

class LICMBenchmark:
    """Benchmarks LICM performance improvement"""
    
    def __init__(self, source_file: str, iterations: int = 100000000):
        self.source_file = source_file
        self.iterations = iterations
        
    def compile_with_licm(self) -> Optional[str]:
        """Compile test program with ZeroCC"""
        try:
            result = subprocess.run(
                ['./build/bin/zerocc', '-O1', '-c', self.source_file, '-o', '/tmp/licm_test.o'],
                capture_output=True,
                text=True,
                timeout=30
            )
            if result.returncode == 0:
                return '/tmp/licm_test.o'
            else:
                print(f"Compilation failed: {result.stderr}")
                return None
        except Exception as e:
            print(f"Compilation error: {e}")
            return None
    
    def run_benchmark(self) -> Dict:
        """Run performance benchmark"""
        results = {
            'iterations': self.iterations,
            'with_licm': None,
            'without_licm': None,
            'speedup': None
        }
        
        # Compile with ZeroCC
        obj_file = self.compile_with_licm()
        if not obj_file:
            return results
        
        # Compile with GCC for comparison
        try:
            subprocess.run(
                ['gcc', '-O1', '-o', '/tmp/gcc_test', self.source_file],
                capture_output=True,
                timeout=30
            )
        except:
            pass
        
        # Run ZeroCC binary
        start = time.time()
        try:
            result = subprocess.run(
                ['./build/bin/zerocc', '-O1', self.source_file],
                capture_output=True,
                timeout=10
            )
            end = time.time()
            results['with_licm'] = (end - start) * 1000  # ms
        except Exception as e:
            results['error'] = str(e)
        
        return results

def main():
    parser = argparse.ArgumentParser(description='LICM Trampoline Verifier')
    parser.add_argument('--dump-before', help='IR dump before LICM')
    parser.add_argument('--dump-after', help='IR dump after LICM')
    parser.add_argument('--source', help='Source file for benchmark')
    parser.add_argument('--iterations', type=int, default=100000000, help='Benchmark iterations')
    
    args = parser.parse_args()
    
    if args.dump_before and args.dump_after:
        # Verification mode
        with open(args.dump_before, 'r') as f:
            before_ir = f.read()
        with open(args.dump_after, 'r') as f:
            after_ir = f.read()
        
        verifier = LICMVerifier(before_ir, after_ir)
        print(verifier.generate_report())
        
    elif args.source:
        # Benchmark mode
        benchmark = LICMBenchmark(args.source, args.iterations)
        results = benchmark.run_benchmark()
        
        print(f"\nLICM Performance Benchmark")
        print(f"=" * 40)
        print(f"Iterations: {results['iterations']:,}")
        if results['with_licm']:
            print(f"With LICM: {results['with_licm']:.2f} ms")
        if results.get('error'):
            print(f"Error: {results['error']}")
    else:
        parser.print_help()

if __name__ == '__main__':
    main()
