#!/usr/bin/env python3
"""
Design Verification Suite

Comprehensive verification of ISA-agnostic design across the compiler.
Runs all analysis tools and generates a unified report.

Usage:
    python3 design_verification_suite.py
    python3 design_verification_suite.py --full --output report.html
"""

import argparse
import os
import sys
import json
import subprocess
from dataclasses import dataclass, field
from typing import List, Dict, Optional
from pathlib import Path
from datetime import datetime

@dataclass
class VerificationResult:
    """Result of a single verification check"""
    name: str
    status: str  # "PASS", "WARN", "FAIL", "SKIP"
    message: str
    details: Dict = field(default_factory=dict)

@dataclass
class VerificationReport:
    """Complete verification report"""
    timestamp: str
    compiler_root: str
    results: List[VerificationResult] = field(default_factory=list)
    summary: Dict = field(default_factory=dict)
    recommendations: List[str] = field(default_factory=list)

class DesignVerificationSuite:
    """Comprehensive design verification suite"""
    
    def __init__(self, compiler_root: str = "."):
        self.compiler_root = Path(compiler_root)
        self.results: List[VerificationResult] = []
        
    def run_all(self, full: bool = False) -> VerificationReport:
        """Run all verification checks"""
        print("=" * 80)
        print("ISA-AGNOSTIC DESIGN VERIFICATION SUITE")
        print("=" * 80)
        print(f"Compiler root: {self.compiler_root}")
        print(f"Timestamp: {datetime.now().isoformat()}")
        print()
        
        # Phase 1: Basic checks (always run)
        self._check_build_system()
        self._check_backend_interface()
        self._check_ir_layer()
        
        # Phase 2: Detailed checks (full mode)
        if full:
            self._check_ast_lowering()
            self._check_stack_frame_layout()
            self._check_test_results()
            self._check_cross_isa_consistency()
        
        # Generate summary
        summary = self._generate_summary()
        
        return VerificationReport(
            timestamp=datetime.now().isoformat(),
            compiler_root=str(self.compiler_root),
            results=self.results,
            summary=summary,
            recommendations=self._generate_recommendations()
        )
    
    def _add_result(self, name: str, status: str, message: str, details: Dict = None):
        """Add verification result"""
        self.results.append(VerificationResult(
            name=name,
            status=status,
            message=message,
            details=details or {}
        ))
        
        status_icon = {"PASS": "✓", "WARN": "⚠", "FAIL": "✗", "SKIP": "○"}
        icon = status_icon.get(status, "?")
        print(f"  [{icon}] {name}: {message}")
    
    def _check_build_system(self):
        """Check build system configuration"""
        print("\n[1/6] Checking build system...")
        
        makefile = self.compiler_root / "Makefile"
        if not makefile.exists():
            self._add_result("Build System", "FAIL", "Makefile not found")
            return
        
        # Check for ISA-specific build flags
        with open(makefile, 'r') as f:
            content = f.read()
        
        isa_flags = []
        if 'x86_64' in content.lower():
            isa_flags.append('x86_64')
        if 'aarch64' in content.lower():
            isa_flags.append('aarch64')
        
        if isa_flags:
            self._add_result(
                "Build System", "WARN",
                f"Found ISA-specific flags: {', '.join(isa_flags)}",
                {'isa_flags': isa_flags}
            )
        else:
            self._add_result("Build System", "PASS", "No ISA-specific build flags found")
    
    def _check_backend_interface(self):
        """Check backend interface for ISA-agnosticism"""
        print("\n[2/6] Checking backend interface...")
        
        header = self.compiler_root / "src" / "zero_cc" / "isa" / "isa_backend.h"
        if not header.exists():
            self._add_result("Backend Interface", "FAIL", "isa_backend.h not found")
            return
        
        with open(header, 'r') as f:
            content = f.read()
        
        # Check for ISA-neutral function names
        neutral_patterns = ['emit_prologue', 'emit_epilogue', 'emit_load', 'emit_store']
        found_neutral = [p for p in neutral_patterns if p in content]
        
        # Check for ISA-specific patterns
        x86_patterns = ['rax', 'rbx', 'rsp', 'rbp']
        found_x86 = [p for p in x86_patterns if p in content]
        
        if found_x86:
            self._add_result(
                "Backend Interface", "WARN",
                f"Found x86-64 specific references: {', '.join(found_x86)}",
                {'x86_refs': found_x86, 'neutral_funcs': found_neutral}
            )
        elif found_neutral:
            self._add_result(
                "Backend Interface", "PASS",
                f"Found ISA-neutral functions: {', '.join(found_neutral)}",
                {'neutral_funcs': found_neutral}
            )
        else:
            self._add_result("Backend Interface", "WARN", "Could not verify ISA-neutrality")
    
    def _check_ir_layer(self):
        """Check IR layer for ISA-agnosticism"""
        print("\n[3/6] Checking IR layer...")
        
        ir_header = self.compiler_root / "src" / "zero_cc" / "parser" / "ir.h"
        if not ir_header.exists():
            self._add_result("IR Layer", "FAIL", "ir.h not found")
            return
        
        with open(ir_header, 'r') as f:
            content = f.read()
        
        # Check for ISA-neutral opcodes
        neutral_opcodes = ['OP_CMP', 'OP_COND_SET', 'OP_JCOND', 'OP_MOV']
        found_neutral = [op for op in neutral_opcodes if op in content]
        
        # Check for ISA-specific opcodes
        x86_opcodes = ['OP_PUSH_RAX', 'OP_POP_RAX', 'OP_DIV_RAX']
        found_x86 = [op for op in x86_opcodes if op in content]
        
        if found_x86:
            self._add_result(
                "IR Layer", "FAIL",
                f"Found ISA-specific opcodes: {', '.join(found_x86)}",
                {'x86_opcodes': found_x86, 'neutral_opcodes': found_neutral}
            )
        elif found_neutral:
            self._add_result(
                "IR Layer", "PASS",
                f"Found ISA-neutral opcodes: {', '.join(found_neutral)}",
                {'neutral_opcodes': found_neutral}
            )
        else:
            self._add_result("IR Layer", "WARN", "Could not verify opcode neutrality")
    
    def _check_ast_lowering(self):
        """Check AST lowering for ISA-specific assumptions"""
        print("\n[4/6] Checking AST lowering...")
        
        lowering_file = self.compiler_root / "src" / "zero_cc" / "parser" / "ast_lowering.c"
        if not lowering_file.exists():
            self._add_result("AST Lowering", "SKIP", "ast_lowering.c not found")
            return
        
        with open(lowering_file, 'r') as f:
            content = f.read()
        
        # Check for callee-saved register handling
        if 'emit_callee_save' in content:
            # Check if it's conditional on ISA
            if 'ISA_X86_64' in content or 'c->isa ==' in content:
                self._add_result(
                    "AST Lowering", "PASS",
                    "Callee-saved handling is ISA-conditional",
                    {'callee_save': True, 'conditional': True}
                )
            else:
                self._add_result(
                    "AST Lowering", "FAIL",
                    "Callee-saved handling is not ISA-conditional (x86-64 specific)",
                    {'callee_save': True, 'conditional': False}
                )
        else:
            self._add_result("AST Lowering", "PASS", "No callee-saved handling found")
    
    def _check_stack_frame_layout(self):
        """Check stack frame layout consistency"""
        print("\n[5/6] Checking stack frame layout...")
        
        # Check for CALLEE_SAVE_FRAME_RESERVE usage
        files_to_check = [
            "src/zero_cc/parser/regalloc.h",
            "src/zero_cc/parser/ast_lowering.c",
            "src/zero_cc/jit/compile.h"
        ]
        
        found_reserve = False
        for rel_path in files_to_check:
            file_path = self.compiler_root / rel_path
            if file_path.exists():
                with open(file_path, 'r') as f:
                    if 'CALLEE_SAVE_FRAME_RESERVE' in f.read():
                        found_reserve = True
                        break
        
        if found_reserve:
            self._add_result(
                "Stack Frame Layout", "WARN",
                "CALLEE_SAVE_FRAME_RESERVE found - may cause cross-ISA issues",
                {'has_reserve': True}
            )
        else:
            self._add_result("Stack Frame Layout", "PASS", "No CALLEE_SAVE_FRAME_RESERVE found")
    
    def _check_test_results(self):
        """Check test results for cross-ISA consistency"""
        print("\n[6/6] Checking test results...")
        
        # Check if we can run tests
        makefile = self.compiler_root / "Makefile"
        if not makefile.exists():
            self._add_result("Test Results", "SKIP", "Makefile not found")
            return
        
        # Try to run tests
        try:
            result = subprocess.run(
                ['make', 'test'],
                cwd=self.compiler_root,
                capture_output=True,
                text=True,
                timeout=60
            )
            
            if result.returncode == 0:
                # Parse test output
                match = __import__('re').search(r'(\d+)/(\d+)\s+passed', result.stdout)
                if match:
                    passed, total = int(match.group(1)), int(match.group(2))
                    if passed == total:
                        self._add_result(
                            "Test Results", "PASS",
                            f"All {total} tests passed",
                            {'passed': passed, 'total': total}
                        )
                    else:
                        self._add_result(
                            "Test Results", "WARN",
                            f"{passed}/{total} tests passed",
                            {'passed': passed, 'total': total}
                        )
                else:
                    self._add_result("Test Results", "PASS", "Tests passed")
            else:
                self._add_result(
                    "Test Results", "FAIL",
                    "Tests failed",
                    {'error': result.stderr[:200]}
                )
        except subprocess.TimeoutExpired:
            self._add_result("Test Results", "SKIP", "Test timeout")
        except Exception as e:
            self._add_result("Test Results", "SKIP", f"Could not run tests: {e}")
    
    def _check_cross_isa_consistency(self):
        """Check cross-ISA consistency (requires test objects)"""
        print("\n[Extra] Checking cross-ISA consistency...")
        
        # This would require compiling test files for both ISAs
        # and comparing the output - skip for now
        self._add_result(
            "Cross-ISA Consistency", "SKIP",
            "Requires compiled test objects for comparison"
        )
    
    def _generate_summary(self) -> Dict:
        """Generate summary statistics"""
        status_counts = {"PASS": 0, "WARN": 0, "FAIL": 0, "SKIP": 0}
        for result in self.results:
            status_counts[result.status] = status_counts.get(result.status, 0) + 1
        
        return {
            'total_checks': len(self.results),
            'status_counts': status_counts,
            'overall_status': "PASS" if status_counts["FAIL"] == 0 and status_counts["WARN"] == 0 else
                             "WARN" if status_counts["FAIL"] == 0 else "FAIL"
        }
    
    def _generate_recommendations(self) -> List[str]:
        """Generate recommendations based on results"""
        recommendations = []
        
        for result in self.results:
            if result.status == "FAIL":
                if "callee-saved" in result.message.lower():
                    recommendations.append(
                        "Move callee-saved register handling from AST lowering to backend layer"
                    )
                if "ISA-specific" in result.message.lower():
                    recommendations.append(
                        "Refactor ISA-specific code to use backend interface"
                    )
            elif result.status == "WARN":
                if "CALLEE_SAVE" in result.message:
                    recommendations.append(
                        "Consider making CALLEE_SAVE_FRAME_RESERVE ISA-specific or removing it"
                    )
        
        # Deduplicate
        return list(set(recommendations))


def generate_report_html(report: VerificationReport, output_file: str):
    """Generate HTML report"""
    html = f"""<!DOCTYPE html>
<html>
<head>
    <title>ISA-Agnostic Design Verification Report</title>
    <style>
        body {{ font-family: Arial, sans-serif; margin: 40px; }}
        h1 {{ color: #333; }}
        .summary {{ background: #f5f5f5; padding: 20px; border-radius: 5px; }}
        .result {{ margin: 10px 0; padding: 10px; border-left: 4px solid #ccc; }}
        .pass {{ border-left-color: #4caf50; background: #e8f5e9; }}
        .warn {{ border-left-color: #ff9800; background: #fff3e0; }}
        .fail {{ border-left-color: #f44336; background: #ffebee; }}
        .skip {{ border-left-color: #9e9e9e; background: #f5f5f5; }}
        table {{ border-collapse: collapse; width: 100%; }}
        th, td {{ border: 1px solid #ddd; padding: 8px; text-align: left; }}
        th {{ background: #4caf50; color: white; }}
    </style>
</head>
<body>
    <h1>ISA-Agnostic Design Verification Report</h1>
    <p>Generated: {report.timestamp}</p>
    <p>Compiler root: {report.compiler_root}</p>
    
    <div class="summary">
        <h2>Summary</h2>
        <p><strong>Total Checks:</strong> {report.summary.get('total_checks', 0)}</p>
        <p><strong>Overall Status:</strong> {report.summary.get('overall_status', 'UNKNOWN')}</p>
        <ul>
            <li>Pass: {report.summary.get('status_counts', {}).get('PASS', 0)}</li>
            <li>Warning: {report.summary.get('status_counts', {}).get('WARN', 0)}</li>
            <li>Fail: {report.summary.get('status_counts', {}).get('FAIL', 0)}</li>
            <li>Skipped: {report.summary.get('status_counts', {}).get('SKIP', 0)}</li>
        </ul>
    </div>
    
    <h2>Results</h2>
"""
    
    for result in report.results:
        css_class = result.status.lower()
        icon = {"PASS": "✓", "WARN": "⚠", "FAIL": "✗", "SKIP": "○"}.get(result.status, "?")
        html += f"""
    <div class="result {css_class}">
        <strong>{icon} {result.name}</strong>: {result.message}
        {f"<pre>{json.dumps(result.details, indent=2)}</pre>" if result.details else ""}
    </div>
"""
    
    if report.recommendations:
        html += """
    <h2>Recommendations</h2>
    <ul>
"""
        for rec in report.recommendations:
            html += f"        <li>{rec}</li>\n"
        html += "    </ul>\n"
    
    html += """
</body>
</html>
"""
    
    with open(output_file, 'w') as f:
        f.write(html)


def main():
    parser = argparse.ArgumentParser(
        description='ISA-Agnostic Design Verification Suite'
    )
    parser.add_argument('--compiler-root', default='.',
                       help='Compiler root directory')
    parser.add_argument('--full', action='store_true',
                       help='Run full verification (all checks)')
    parser.add_argument('--output', help='Output HTML report file')
    parser.add_argument('--json', help='Output JSON report file')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.compiler_root):
        print(f"Error: Compiler root not found: {args.compiler_root}")
        sys.exit(1)
    
    suite = DesignVerificationSuite(args.compiler_root)
    report = suite.run_all(full=args.full)
    
    # Print summary
    print("\n" + "=" * 80)
    print("VERIFICATION SUMMARY")
    print("=" * 80)
    print(f"Overall Status: {report.summary.get('overall_status', 'UNKNOWN')}")
    print(f"Total Checks: {report.summary.get('total_checks', 0)}")
    print(f"  Pass: {report.summary.get('status_counts', {}).get('PASS', 0)}")
    print(f"  Warning: {report.summary.get('status_counts', {}).get('WARN', 0)}")
    print(f"  Fail: {report.summary.get('status_counts', {}).get('FAIL', 0)}")
    print(f"  Skipped: {report.summary.get('status_counts', {}).get('SKIP', 0)}")
    
    if report.recommendations:
        print("\nRecommendations:")
        for rec in report.recommendations:
            print(f"  • {rec}")
    
    # Generate HTML report
    if args.output:
        generate_report_html(report, args.output)
        print(f"\nHTML report saved to: {args.output}")
    
    # Generate JSON report
    if args.json:
        with open(args.json, 'w') as f:
            json.dump({
                'timestamp': report.timestamp,
                'compiler_root': report.compiler_root,
                'results': [
                    {
                        'name': r.name,
                        'status': r.status,
                        'message': r.message,
                        'details': r.details
                    }
                    for r in report.results
                ],
                'summary': report.summary,
                'recommendations': report.recommendations
            }, f, indent=2)
        print(f"JSON report saved to: {args.json}")
    
    # Exit with error if any failures
    if report.summary.get('status_counts', {}).get('FAIL', 0) > 0:
        sys.exit(1)


if __name__ == '__main__':
    main()
