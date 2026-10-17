#!/usr/bin/env python3
"""
Interval Span Analyzer

Analyzes IR to identify which register intervals span function calls.
This is CRITICAL for implementing the proper fix for caller-saved register clobbering.

Usage:
    python3 tools/interval_span_analyzer.py --ir ir_dump.txt --analyze-calls
    python3 tools/interval_span_analyzer.py --ir ir_dump.txt --generate-fix
    python3 tools/interval_span_analyzer.py --ir ir_dump.txt --json output.json
"""

import argparse
import re
import sys
import json
from dataclasses import dataclass, asdict, field
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

@dataclass
class LiveInterval:
    """Represents a live interval for a temporary"""
    temp_id: int
    start: int  # First instruction that defines or uses this temp
    end: int    # Last instruction that uses this temp
    spans_call: bool = False
    call_indices: List[int] = field(default_factory=list)

@dataclass
class SpanAnalysis:
    """Analysis result for an interval spanning a call"""
    temp_id: int
    start: int
    end: int
    spans_call: bool
    call_indices: List[int]
    recommendation: str  # "use_callee_saved", "spill", "ok"
    reason: str

class IntervalSpanAnalyzer:
    """Analyzes which register intervals span function calls"""
    
    def __init__(self, ir_dump: str):
        self.ir_dump = ir_dump
        self.instructions: List[IRInstruction] = []
        self.intervals: Dict[int, LiveInterval] = {}
        self.call_indices: List[int] = []
        self.analyses: List[SpanAnalysis] = []
    
    def analyze(self, generate_fix: bool = False) -> List[SpanAnalysis]:
        """Perform complete analysis"""
        # Parse IR
        self._parse_ir()
        
        # Find call instructions
        self._find_calls()
        
        # Build live intervals
        self._build_intervals()
        
        # Analyze which intervals span calls
        self._analyze_spans()
        
        # Generate recommendations
        self._generate_recommendations()
        
        return self.analyses
    
    def _parse_ir(self):
        """Parse IR dump into instructions"""
        for line in self.ir_dump.split('\n'):
            line = line.strip()
            
            # Parse instruction: "  N: OP ..."
            match = re.match(r'^(\d+):\s+(\w+)\s+(.*)$', line)
            if match:
                idx = int(match.group(1))
                op = match.group(2)
                operands = match.group(3)
                
                # Extract temp IDs
                temps = re.findall(r'r(\d+)', operands)
                temp_dest = int(temps[0]) if len(temps) > 0 and op not in ['CALL', 'JCOND', 'JMP'] else None
                temp_srcs = [int(t) for t in temps[1:]] if len(temps) > 1 else []
                
                instr = IRInstruction(
                    index=idx,
                    op=op,
                    operands=operands,
                    temp_dest=temp_dest,
                    temp_srcs=temp_srcs
                )
                
                self.instructions.append(instr)
    
    def _find_calls(self):
        """Find all CALL instructions"""
        for i, instr in enumerate(self.instructions):
            if 'CALL' in instr.op:
                self.call_indices.append(i)
    
    def _build_intervals(self):
        """Build live intervals for all temporaries"""
        # Track first and last use of each temp
        temp_first_use: Dict[int, int] = {}
        temp_last_use: Dict[int, int] = {}
        
        for i, instr in enumerate(self.instructions):
            # Check temp_dest (definition)
            if instr.temp_dest is not None:
                tid = instr.temp_dest
                if tid not in temp_first_use:
                    temp_first_use[tid] = i
                temp_last_use[tid] = i
            
            # Check temp_srcs (uses)
            for src in instr.temp_srcs:
                if src not in temp_first_use:
                    temp_first_use[src] = i
                temp_last_use[src] = i
        
        # Create intervals
        for tid in temp_first_use:
            interval = LiveInterval(
                temp_id=tid,
                start=temp_first_use[tid],
                end=temp_last_use[tid],
                spans_call=False,
                call_indices=[]
            )
            self.intervals[tid] = interval
    
    def _analyze_spans(self):
        """Analyze which intervals span calls"""
        for tid, interval in self.intervals.items():
            # Check if interval spans any call
            spanning_calls = [
                call_idx for call_idx in self.call_indices
                if interval.start <= call_idx <= interval.end
            ]
            
            if spanning_calls:
                interval.spans_call = True
                interval.call_indices = spanning_calls
            
            # Create analysis result
            analysis = SpanAnalysis(
                temp_id=tid,
                start=interval.start,
                end=interval.end,
                spans_call=interval.spans_call,
                call_indices=interval.call_indices,
                recommendation="ok",
                reason="No calls spanned"
            )
            
            self.analyses.append(analysis)
    
    def _generate_recommendations(self):
        """Generate recommendations for fixing caller-saved register issues"""
        for analysis in self.analyses:
            if analysis.spans_call:
                if len(analysis.call_indices) >= 2:
                    analysis.recommendation = "use_callee_saved"
                    analysis.reason = f"Spans {len(analysis.call_indices)} calls - MUST use callee-saved register"
                elif len(analysis.call_indices) == 1:
                    analysis.recommendation = "use_callee_saved_or_spill"
                    analysis.reason = f"Spans 1 call - prefer callee-saved, spill if necessary"
            else:
                analysis.recommendation = "ok"
                analysis.reason = "No calls spanned - caller-saved OK"
    
    def generate_fix_code(self) -> str:
        """Generate C code for the fix"""
        lines = []
        lines.append("// Auto-generated fix code for caller-saved register handling")
        lines.append("// Generated by interval_span_analyzer.py")
        lines.append("")
        
        # Count spans-call intervals
        spans_call_count = sum(1 for a in self.analyses if a.spans_call)
        
        lines.append(f"// Analysis: {len(self.analyses)} total intervals, {spans_call_count} span calls")
        lines.append("")
        
        # Generate interval marking code
        lines.append("/* Step 1: Mark intervals that span calls */")
        lines.append("for (size_t i = 0; i < c->regalloc.interval_count; i++) {")
        lines.append("    LiveInterval *interval = &c->regalloc.intervals[i];")
        lines.append("    bool spans_call = false;")
        lines.append("    for (size_t j = 0; j < c->lowering.call_count; j++) {")
        lines.append("        size_t call_idx = c->lowering.call_instruction_indices[j];")
        lines.append("        if (interval->start <= call_idx && interval->end >= call_idx) {")
        lines.append("            spans_call = true;")
        lines.append("            break;")
        lines.append("        }")
        lines.append("    }")
        lines.append("    interval->spans_call = spans_call;")
        lines.append("}")
        lines.append("")
        
        # Generate register allocation modification
        lines.append("/* Step 2: Modify alloc_reg() to prefer callee-saved for spanning intervals */")
        lines.append("static int alloc_reg(RegAllocator *ra, LiveInterval *interval) {")
        lines.append("    // Prefer callee-saved registers for intervals spanning calls")
        lines.append("    if (interval->spans_call) {")
        lines.append("        // Try callee-saved first (x19-x28 for AArch64)")
        lines.append("        for (int reg = 19; reg <= 28; reg++) {")
        lines.append("            if (ra->reg_free[reg]) {")
        lines.append("                ra->reg_free[reg] = false;")
        lines.append("                return reg;")
        lines.append("            }")
        lines.append("        }")
        lines.append("    }")
        lines.append("")
        lines.append("    // Otherwise use caller-saved registers")
        lines.append("    for (int reg = 0; reg <= 18; reg++) {")
        lines.append("        if (ra->reg_free[reg]) {")
        lines.append("            ra->reg_free[reg] = false;")
        lines.append("            return reg;")
        lines.append("        }")
        lines.append("    }")
        lines.append("")
        lines.append("    // No free registers - spill")
        lines.append("    return -1;  // Indicates spill needed")
        lines.append("}")
        lines.append("")
        
        return '\n'.join(lines)
    
    def generate_patch(self) -> str:
        """Generate a complete patch for the fix"""
        lines = []
        lines.append("=" * 80)
        lines.append("PATCH FOR CALLER-SAVED REGISTER FIX")
        lines.append("=" * 80)
        lines.append("")
        
        # Summary
        spans_call_temps = [a for a in self.analyses if a.spans_call]
        lines.append(f"SUMMARY:")
        lines.append(f"  - Total intervals: {len(self.analyses)}")
        lines.append(f"  - Intervals spanning calls: {len(spans_call_temps)}")
        lines.append(f"  - Temps requiring callee-saved: {[a.temp_id for a in spans_call_temps]}")
        lines.append("")
        
        # Detailed analysis
        if spans_call_temps:
            lines.append("DETAILED ANALYSIS:")
            lines.append("-" * 80)
            for a in spans_call_temps:
                lines.append(f"  Temp r{a.temp_id}:")
                lines.append(f"    Start: IR instruction {a.start}")
                lines.append(f"    End:   IR instruction {a.end}")
                lines.append(f"    Calls: {a.call_indices}")
                lines.append(f"    Recommendation: {a.recommendation}")
                lines.append(f"    Reason: {a.reason}")
                lines.append("")
        
        # Generated code
        lines.append("GENERATED FIX CODE:")
        lines.append("-" * 80)
        lines.append(self.generate_fix_code())
        
        return '\n'.join(lines)
    
    def to_json(self) -> str:
        """Export analysis as JSON"""
        data = {
            "total_intervals": len(self.analyses),
            "call_indices": self.call_indices,
            "spans_call_count": sum(1 for a in self.analyses if a.spans_call),
            "intervals": [asdict(a) for a in self.analyses]
        }
        return json.dumps(data, indent=2)

def main():
    parser = argparse.ArgumentParser(
        description='Analyze which register intervals span function calls'
    )
    parser.add_argument('--ir', required=True, help='IR dump file')
    parser.add_argument('--analyze-calls', action='store_true', help='Analyze call-spanning intervals')
    parser.add_argument('--generate-fix', action='store_true', help='Generate fix code')
    parser.add_argument('--json', help='Output JSON to file')
    parser.add_argument('--patch', action='store_true', help='Generate complete patch')
    
    args = parser.parse_args()
    
    # Read IR dump
    with open(args.ir, 'r') as f:
        ir_dump = f.read()
    
    analyzer = IntervalSpanAnalyzer(ir_dump)
    analyses = analyzer.analyze(generate_fix=args.generate_fix or args.patch)
    
    # Output
    if args.patch:
        print(analyzer.generate_patch())
    elif args.json:
        with open(args.json, 'w') as f:
            f.write(analyzer.to_json())
        print(f"JSON saved to: {args.json}")
    elif args.generate_fix:
        print(analyzer.generate_fix_code())
    else:
        # Default: print summary
        print("=" * 80)
        print("INTERVAL SPAN ANALYSIS")
        print("=" * 80)
        print(f"Total intervals: {len(analyses)}")
        print(f"Call instructions at IR indices: {analyzer.call_indices}")
        print("")
        
        spans_call = [a for a in analyses if a.spans_call]
        if spans_call:
            print(f"⚠️  Found {len(spans_call)} intervals spanning calls:")
            print("-" * 80)
            for a in spans_call:
                print(f"  Temp r{a.temp_id}: spans {len(a.call_indices)} call(s) at {a.call_indices}")
                print(f"    Recommendation: {a.recommendation}")
                print(f"    Reason: {a.reason}")
            print("")
        else:
            print("✓ No intervals span calls - no fix needed")
    
    sys.exit(0)

if __name__ == '__main__':
    main()
