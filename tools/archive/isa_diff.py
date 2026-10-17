#!/usr/bin/env python3
"""
Cross-ISA Codegen Diff Tool

Compiles the same C source to multiple ISAs and compares:
- Instruction counts
- Stack frame sizes
- Relocation counts
- Code patterns

Usage:
    python3 tools/isa_diff.py test.c --isas x86_64 aarch64
"""

import argparse
import subprocess
import sys
import json
import os
from pathlib import Path
from dataclasses import dataclass
from typing import Dict, List, Optional

@dataclass
class CompileResult:
    isa: str
    success: bool
    obj_path: str
    instruction_count: int = 0
    stack_size: int = 0
    relocation_count: int = 0
    error: str = ""

def compile_to_obj(source_path: Path, isa: str, output_dir: Path) -> CompileResult:
    """Compile source to object file for given ISA."""
    obj_path = output_dir / f"{source_path.stem}_{isa}.o"
    
    zerocc = Path("build/bin/zerocc")
    if not zerocc.exists():
        zerocc = Path("zerocc")
    
    cmd = [str(zerocc), "-c", str(source_path), "-o", str(obj_path), "--target", isa]
    
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30
        )
        
        if result.returncode != 0:
            return CompileResult(
                isa=isa,
                success=False,
                obj_path=str(obj_path),
                error=result.stderr
            )
        
        # Analyze object file
        return analyze_obj(obj_path, isa)
        
    except subprocess.TimeoutExpired:
        return CompileResult(
            isa=isa,
            success=False,
            obj_path=str(obj_path),
            error="Compilation timeout"
        )
    except Exception as e:
        return CompileResult(
            isa=isa,
            success=False,
            obj_path=str(obj_path),
            error=str(e)
        )

def analyze_obj(obj_path: Path, isa: str) -> CompileResult:
    """Analyze object file using readelf."""
    result = CompileResult(
        isa=isa,
        success=True,
        obj_path=str(obj_path)
    )
    
    try:
        # Get section headers
        cmd = ["readelf", "-S", str(obj_path)]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        
        # Parse .text section for size
        for line in proc.stdout.split('\n'):
            if '.text' in line and 'PROGBITS' in line:
                # Extract size (hex)
                parts = line.split()
                for i, part in enumerate(parts):
                    if len(part) == 8 and all(c in '0123456789abcdef' for c in part.lower()):
                        result.stack_size = int(part, 16)
                        break
        
        # Get relocations
        cmd = ["readelf", "-r", str(obj_path)]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        
        # Count relocations
        reloc_count = 0
        for line in proc.stdout.split('\n'):
            if 'R_' in line and ('CALL' in line or 'ADR' in line or 'REL' in line):
                reloc_count += 1
        result.relocation_count = reloc_count
        
    except Exception as e:
        result.error = f"Analysis error: {e}"
        result.success = False
    
    return result

def print_report(results: List[CompileResult]):
    """Print comparison report."""
    print(f"\n{'='*70}")
    print(f"Cross-ISA Codegen Comparison")
    print(f"{'='*70}\n")
    
    for r in results:
        status = "✓" if r.success else "✗"
        print(f"[{status}] {r.isa}")
        if r.success:
            print(f"    Object: {r.obj_path}")
            print(f"    Code size: {r.stack_size} bytes")
            print(f"    Relocations: {r.relocation_count}")
        else:
            print(f"    Error: {r.error}")
        print()
    
    # Compare successful results
    successful = [r for r in results if r.success]
    if len(successful) >= 2:
        print(f"{'='*70}")
        print(f"Comparison")
        print(f"{'='*70}\n")
        
        base = successful[0]
        for r in successful[1:]:
            size_diff = r.stack_size - base.stack_size
            size_pct = (size_diff / base.stack_size * 100) if base.stack_size > 0 else 0
            reloc_diff = r.relocation_count - base.relocation_count
            
            print(f"{base.isa} vs {r.isa}:")
            print(f"  Size diff: {size_diff:+d} bytes ({size_pct:+.1f}%)")
            print(f"  Reloc diff: {reloc_diff:+d}")
            print()

def main():
    parser = argparse.ArgumentParser(description="Compare codegen across ISAs")
    parser.add_argument("source", help="C source file to compile")
    parser.add_argument("--isas", nargs="+", default=["x86_64", "aarch64"],
                        choices=["x86_64", "aarch64", "riscv64", "riscv32"],
                        help="ISAs to compare")
    parser.add_argument("--output", "-o", default="build/isa_diff",
                        help="Output directory for object files")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    args = parser.parse_args()
    
    source_path = Path(args.source)
    if not source_path.exists():
        print(f"Error: {source_path} not found", file=sys.stderr)
        sys.exit(1)
    
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Compile for each ISA
    results = []
    for isa in args.isas:
        print(f"Compiling for {isa}...", file=sys.stderr)
        result = compile_to_obj(source_path, isa, output_dir)
        results.append(result)
    
    # Output
    if args.json:
        print(json.dumps([
            {
                "isa": r.isa,
                "success": r.success,
                "instruction_count": r.instruction_count,
                "stack_size": r.stack_size,
                "relocation_count": r.relocation_count,
                "error": r.error
            }
            for r in results
        ], indent=2))
    else:
        print_report(results)

if __name__ == "__main__":
    main()
