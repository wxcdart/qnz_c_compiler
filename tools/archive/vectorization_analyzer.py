#!/usr/bin/env python3
"""
Vectorization Analyzer for ZeroCC
Analyzes C code for SIMD vectorization opportunities

Usage:
    python3 vectorization_analyzer.py <source_file>
    python3 vectorization_analyzer.py --analyze-dir <directory>
    python3 vectorization_analyzer.py --report-json > report.json
"""

import sys
import os
import re
import json
import argparse
from typing import Dict, List, Set, Optional, Tuple
from dataclasses import dataclass, asdict, field
from pathlib import Path


@dataclass
class LoopInfo:
    """Information about a detected loop"""
    line_start: int
    line_end: int
    loop_type: str  # 'for', 'while', 'do_while'
    has_array_access: bool
    array_indices: List[str]
    has_reduction: bool
    reduction_vars: List[str]
    has_data_dependency: bool
    dependency_type: str  # 'none', 'flow', 'anti', 'output'
    vectorizable: bool
    vectorization_reason: str
    estimated_speedup: float


@dataclass
class VectorizationReport:
    """Report on vectorization opportunities"""
    total_loops: int = 0
    vectorizable_loops: int = 0
    non_vectorizable_loops: int = 0
    loops: List[LoopInfo] = field(default_factory=list)
    recommendations: List[str] = field(default_factory=list)


# Patterns for loop detection
FOR_LOOP_PATTERN = re.compile(
    r'\bfor\s*\(\s*(?:int\s+)?(\w+)\s*=\s*([^;]+)\s*;\s*([^;]+)\s*;\s*([^)]+)\)',
    re.MULTILINE
)

WHILE_LOOP_PATTERN = re.compile(
    r'\bwhile\s*\(([^)]+)\)',
    re.MULTILINE
)

ARRAY_ACCESS_PATTERN = re.compile(
    r'(\w+)\s*\[\s*(\w+)\s*\]'
)

REDUCTION_PATTERN = re.compile(
    r'(\w+)\s*=\s*(\w+)\s*([\+\-\*\/])\s*([^;]+)'
)

DATA_DEP_PATTERN = re.compile(
    r'(\w+)\s*\[\s*(\w+)\s*([\+\-]\s*\d+)?\s*\]\s*=.*(\w+)\s*\[\s*(\w+)'
)


class VectorizationAnalyzer:
    def __init__(self):
        self.loops: List[LoopInfo] = []
        self.current_file: str = ""
    
    def analyze_file(self, filepath: str) -> VectorizationReport:
        """Analyze a single C file for vectorization opportunities"""
        self.current_file = filepath
        
        try:
            with open(filepath, 'r') as f:
                content = f.read()
                lines = content.split('\n')
        except IOError as e:
            print(f"Error reading {filepath}: {e}", file=sys.stderr)
            return VectorizationReport()
        
        # Detect loops
        self._detect_for_loops(content, lines)
        self._detect_while_loops(content, lines)
        
        # Analyze each loop
        for loop in self.loops:
            self._analyze_loop(loop, content, lines)
        
        # Generate report
        report = VectorizationReport()
        report.total_loops = len(self.loops)
        report.vectorizable_loops = sum(1 for l in self.loops if l.vectorizable)
        report.non_vectorizable_loops = report.total_loops - report.vectorizable_loops
        report.loops = self.loops
        report.recommendations = self._generate_recommendations()
        
        return report
    
    def _detect_for_loops(self, content: str, lines: List[str]):
        """Detect for loops in code"""
        for match in FOR_LOOP_PATTERN.finditer(content):
            var_name = match.group(1)
            init = match.group(2)
            cond = match.group(3)
            incr = match.group(4)
            
            # Find line number
            start_pos = match.start()
            line_num = content[:start_pos].count('\n') + 1
            
            loop = LoopInfo(
                line_start=line_num,
                line_end=line_num + 10,  # Estimate
                loop_type='for',
                has_array_access=False,
                array_indices=[],
                has_reduction=False,
                reduction_vars=[],
                has_data_dependency=False,
                dependency_type='none',
                vectorizable=False,
                vectorization_reason='',
                estimated_speedup=0.0
            )
            
            self.loops.append(loop)
    
    def _detect_while_loops(self, content: str, lines: List[str]):
        """Detect while loops in code"""
        for match in WHILE_LOOP_PATTERN.finditer(content):
            cond = match.group(1)
            
            # Find line number
            start_pos = match.start()
            line_num = content[:start_pos].count('\n') + 1
            
            # Skip if it looks like a for loop condition
            if 'for' in content[max(0, start_pos-20):start_pos]:
                continue
            
            loop = LoopInfo(
                line_start=line_num,
                line_end=line_num + 10,
                loop_type='while',
                has_array_access=False,
                array_indices=[],
                has_reduction=False,
                reduction_vars=[],
                has_data_dependency=False,
                dependency_type='none',
                vectorizable=False,
                vectorization_reason='',
                estimated_speedup=0.0
            )
            
            self.loops.append(loop)
    
    def _analyze_loop(self, loop: LoopInfo, content: str, lines: List[str]):
        """Analyze a loop for vectorization potential"""
        # Extract loop body (simplified - assumes 10 lines)
        start = max(0, loop.line_start - 1)
        end = min(len(lines), loop.line_end)
        loop_body = '\n'.join(lines[start:end])
        
        # Check for array accesses
        array_matches = ARRAY_ACCESS_PATTERN.findall(loop_body)
        if array_matches:
            loop.has_array_access = True
            loop.array_indices = [idx for _, idx in array_matches]
        
        # Check for reductions
        for match in REDUCTION_PATTERN.finditer(loop_body):
            var = match.group(1)
            op = match.group(3)
            if op in '+-*':
                loop.has_reduction = True
                if var not in loop.reduction_vars:
                    loop.reduction_vars.append(var)
        
        # Check for data dependencies
        for match in DATA_DEP_PATTERN.finditer(loop_body):
            lhs_var = match.group(1)
            lhs_idx = match.group(2)
            rhs_var = match.group(4)
            rhs_idx = match.group(5)
            
            # Check if indices depend on loop variable
            if lhs_idx in loop.array_indices or rhs_idx in loop.array_indices:
                loop.has_data_dependency = True
                
                # Determine dependency type
                if lhs_var == rhs_var:
                    loop.dependency_type = 'flow'
                elif lhs_idx == rhs_idx:
                    loop.dependency_type = 'output'
                else:
                    loop.dependency_type = 'anti'
        
        # Determine vectorizability
        self._determine_vectorizability(loop)
    
    def _determine_vectorizability(self, loop: LoopInfo):
        """Determine if a loop can be vectorized"""
        reasons = []
        
        # Good signs
        if loop.has_array_access:
            reasons.append("Has array accesses")
        
        if not loop.has_data_dependency:
            reasons.append("No data dependencies")
        
        if loop.has_reduction:
            reasons.append("Has reduction (supported)")
        
        # Bad signs
        if loop.has_data_dependency:
            loop.vectorizable = False
            loop.vectorization_reason = f"Data dependency: {loop.dependency_type}"
            return
        
        if not loop.has_array_access:
            loop.vectorizable = False
            loop.vectorization_reason = "No array accesses to vectorize"
            return
        
        # Check for pointer aliasing (simplified check)
        if 'pointer' in loop.vectorization_reason.lower():
            loop.vectorizable = False
            loop.vectorization_reason = "Potential pointer aliasing"
            return
        
        # Loop is vectorizable
        loop.vectorizable = True
        loop.vectorization_reason = "; ".join(reasons)
        
        # Estimate speedup (very rough)
        if loop.has_array_access and not loop.has_reduction:
            loop.estimated_speedup = 4.0  # SIMD width estimate
        elif loop.has_reduction:
            loop.estimated_speedup = 2.0  # Reduction overhead
        else:
            loop.estimated_speedup = 1.5
    
    def _generate_recommendations(self) -> List[str]:
        """Generate vectorization recommendations"""
        recommendations = []
        
        vectorizable = [l for l in self.loops if l.vectorizable]
        non_vectorizable = [l for l in self.loops if not l.vectorizable]
        
        if vectorizable:
            recommendations.append(
                f"Found {len(vectorizable)} vectorizable loop(s). "
                "Consider enabling auto-vectorization (-O3)."
            )
        
        if non_vectorizable:
            dep_loops = [l for l in non_vectorizable if l.has_data_dependency]
            if dep_loops:
                recommendations.append(
                    f"{len(dep_loops)} loop(s) have data dependencies. "
                    "Consider loop transformation or pragma omp simd."
                )
            
            no_array = [l for l in non_vectorizable if not l.has_array_access]
            if no_array:
                recommendations.append(
                    f"{len(no_array)} loop(s) have no array accesses. "
                    "These may not benefit from SIMD."
                )
        
        return recommendations
    
    def analyze_directory(self, dirpath: str) -> List[Tuple[str, VectorizationReport]]:
        """Analyze all C files in a directory"""
        results = []
        path = Path(dirpath)
        
        for c_file in path.rglob('*.c'):
            report = self.analyze_file(str(c_file))
            if report.total_loops > 0:
                results.append((str(c_file), report))
        
        return results
    
    def generate_report_text(self, reports: List[Tuple[str, VectorizationReport]]) -> str:
        """Generate text report"""
        lines = []
        lines.append("=" * 70)
        lines.append("Vectorization Analysis Report")
        lines.append("=" * 70)
        
        total_loops = sum(r.total_loops for _, r in reports)
        total_vec = sum(r.vectorizable_loops for _, r in reports)
        
        lines.append(f"\nTotal Loops Analyzed: {total_loops}")
        lines.append(f"Vectorizable: {total_vec} ({total_vec/total_loops*100:.1f}%)" if total_loops > 0 else "Vectorizable: 0")
        lines.append(f"Non-Vectorizable: {total_loops - total_vec}")
        
        for filepath, report in reports:
            lines.append(f"\n{'-' * 70}")
            lines.append(f"File: {os.path.basename(filepath)}")
            lines.append(f"Loops: {report.total_loops} total, {report.vectorizable_loops} vectorizable")
            
            for loop in report.loops:
                vec_status = "✓" if loop.vectorizable else "✗"
                lines.append(f"\n  Loop at line {loop.line_start} [{vec_status}]")
                lines.append(f"    Type: {loop.loop_type}")
                lines.append(f"    Reason: {loop.vectorization_reason}")
                if loop.estimated_speedup > 0:
                    lines.append(f"    Est. Speedup: {loop.estimated_speedup:.1f}x")
            
            for rec in report.recommendations:
                lines.append(f"\n  Recommendation: {rec}")
        
        lines.append("=" * 70)
        return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Analyze C code for SIMD vectorization opportunities'
    )
    parser.add_argument('file', nargs='?', help='C source file to analyze')
    parser.add_argument('--analyze-dir', metavar='DIR',
                       help='Analyze all C files in directory')
    parser.add_argument('--report-json', action='store_true',
                       help='Output report as JSON')
    parser.add_argument('--output', '-o', metavar='FILE',
                       help='Output file for report')
    
    args = parser.parse_args()
    
    analyzer = VectorizationAnalyzer()
    
    if args.file:
        report = analyzer.analyze_file(args.file)
        results = [(args.file, report)]
    elif args.analyze_dir:
        results = analyzer.analyze_directory(args.analyze_dir)
    else:
        parser.print_help()
        sys.exit(1)
    
    if args.report_json:
        json_output = []
        for filepath, report in results:
            json_output.append({
                'file': filepath,
                'total_loops': report.total_loops,
                'vectorizable_loops': report.vectorizable_loops,
                'loops': [asdict(l) for l in report.loops],
                'recommendations': report.recommendations
            })
        output = json.dumps(json_output, indent=2)
    else:
        output = analyzer.generate_report_text(results)
    
    if args.output:
        with open(args.output, 'w') as f:
            f.write(output)
        print(f"Report written to {args.output}")
    else:
        print(output)


if __name__ == '__main__':
    main()
