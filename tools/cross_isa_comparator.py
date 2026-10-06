#!/usr/bin/env python3
"""Cross-ISA Test Comparator.

Compiles the same source code for multiple ISAs and compares the outputs.
Helps identify ISA-specific bugs by comparing stack frame sizes, instruction counts, etc.

Usage:
    python3 tools/cross_isa_comparator.py test/aot/hello_world.c
"""

import subprocess
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import Optional

@dataclass
class CompileResult:
    isa: str
    obj_path: Path
    success: bool
    stderr: str
    function_count: int = 0
    total_size: int = 0

def compile_for_isa(source: Path, isa: str, output_dir: Path) -> CompileResult:
    """Compile source for a specific ISA."""
    obj_path = output_dir / f"{source.stem}_{isa}.o"
    
    cmd = [
        "./build/bin/zerocc",
        "--target", isa,
        "-c", str(source),
        "-o", str(obj_path)
    ]
    
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=30
    )
    
    success = result.returncode == 0
    
    # Count functions if successful
    func_count = 0
    total_size = 0
    if success and obj_path.exists():
        try:
            dump_result = subprocess.run(
                ['objdump', '-t', str(obj_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=10
            )
            for line in dump_result.stdout.splitlines():
                if ' F .text' in line:
                    func_count += 1
                    try:
                        parts = line.split()
                        size = int(parts[3], 16)
                        total_size += size
                    except (ValueError, IndexError):
                        pass
        except Exception:
            pass
    
    return CompileResult(
        isa=isa,
        obj_path=obj_path,
        success=success,
        stderr=result.stderr,
        function_count=func_count,
        total_size=total_size
    )

def compare_results(results: list[CompileResult]) -> int:
    """Compare compilation results across ISAs."""
    print("\n=== Cross-ISA Compilation Comparison ===")
    print(f"Source: {results[0].obj_path.parent.name}/{results[0].obj_path.parent.stem.split('_')[0] if '_' in results[0].obj_path.parent.stem else results[0].obj_path.stem}")
    print()
    
    all_success = all(r.success for r in results)
    
    # Print summary table
    print(f"{'ISA':<12} {'Success':<10} {'Functions':<12} {'Total Size':<12} {'Object File'}")
    print("-" * 70)
    
    for r in results:
        status = "✓ PASS" if r.success else "✗ FAIL"
        obj_name = r.obj_path.name if r.success else "N/A"
        print(f"{r.isa:<12} {status:<10} {r.function_count:<12} {r.total_size:<12} {obj_name}")
    
    print()
    
    if not all_success:
        print("[WARNING] Some compilations failed:")
        for r in results:
            if not r.success:
                print(f"  {r.isa}: {r.stderr[:200]}")
        return 1
    
    # Check for anomalies
    success_results = [r for r in results if r.success]
    if len(success_results) >= 2:
        ref = success_results[0]
        print(f"\nComparing against {ref.isa} baseline:")
        for r in success_results[1:]:
            size_diff = r.total_size - ref.total_size
            size_pct = (size_diff / ref.total_size * 100) if ref.total_size > 0 else 0
            print(f"  {r.isa}: {size_diff:+d} bytes ({size_pct:+.1f}%)")
    
    return 0

def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description='Compare compilation across ISAs')
    parser.add_argument('source', type=Path, help='Source file to compile')
    parser.add_argument('--isas', nargs='+', default=['x86_64', 'aarch64'],
                       help='ISAs to compare (default: x86_64 aarch64)')
    parser.add_argument('--output-dir', type=Path, default=Path('/tmp/cross_isa'),
                       help='Output directory for object files')
    args = parser.parse_args()
    
    if not args.source.exists():
        print(f"[error] Source file not found: {args.source}")
        return 1
    
    args.output_dir.mkdir(parents=True, exist_ok=True)
    
    results = []
    for isa in args.isas:
        print(f"Compiling for {isa}...")
        result = compile_for_isa(args.source, isa, args.output_dir)
        results.append(result)
        if result.success:
            print(f"  ✓ {result.obj_path.name}")
        else:
            print(f"  ✗ Failed: {result.stderr[:100]}")
    
    return compare_results(results)

if __name__ == '__main__':
    sys.exit(main())
