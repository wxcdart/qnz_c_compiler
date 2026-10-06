#!/usr/bin/env python3
"""QEMU Test Runner with Timeout Support.

Runs compiled AArch64 or RISC-V64 objects under QEMU with per-test timeouts.
Useful for testing cross-ISA binaries without native hardware.

Usage:
    python3 tools/qemu_test_runner.py --isa aarch64 test/aot/
"""

import subprocess
import sys
import json
from pathlib import Path
from dataclasses import dataclass
from typing import Optional
import time

@dataclass
class TestResult:
    name: str
    success: bool
    exit_code: Optional[int]
    timeout: bool
    duration: float
    output: str

QEMU_BINARIES = {
    'aarch64': 'qemu-aarch64-static',
    'riscv64': 'qemu-riscv64-static',
    'x86_64': None,  # Native execution
}

def compile_and_link(source: Path, isa: str, output_dir: Path) -> Optional[Path]:
    """Compile source and link into executable."""
    obj_path = output_dir / f"{source.stem}.o"
    exe_path = output_dir / f"{source.stem}"
    
    # Compile
    compile_cmd = [
        "./build/bin/zerocc",
        "--target", isa,
        "-c", str(source),
        "-o", str(obj_path)
    ]
    
    result = subprocess.run(compile_cmd, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        return None
    
    # Link with system gcc
    link_cmd = [
        "gcc", "-static", "-nostdlib", "-nostartfiles",
        "-Wl,--build-id=none", "-o", str(exe_path),
        str(obj_path), "-lgcc"
    ]
    
    result = subprocess.run(link_cmd, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        return None
    
    return exe_path

def run_under_qemu(exe_path: Path, isa: str, timeout_sec: int = 30) -> TestResult:
    """Run executable under QEMU with timeout."""
    qemu_bin = QEMU_BINARIES.get(isa)
    
    if not qemu_bin:
        # Native execution
        cmd = [str(exe_path)]
    else:
        cmd = [qemu_bin, str(exe_path)]
    
    start_time = time.time()
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout_sec
        )
        duration = time.time() - start_time
        
        return TestResult(
            name=exe_path.stem,
            success=result.returncode == 0,
            exit_code=result.returncode,
            timeout=False,
            duration=duration,
            output=result.stdout + result.stderr
        )
    except subprocess.TimeoutExpired:
        duration = time.time() - start_time
        return TestResult(
            name=exe_path.stem,
            success=False,
            exit_code=None,
            timeout=True,
            duration=duration,
            output="TIMEOUT"
        )

def run_test_suite(source_dir: Path, isa: str, timeout_sec: int = 30) -> list[TestResult]:
    """Run all test files in directory."""
    output_dir = Path(f"/tmp/qemu_{isa}")
    output_dir.mkdir(parents=True, exist_ok=True)
    
    results = []
    
    # Find all .c files
    test_files = list(source_dir.rglob("*.c"))
    print(f"Found {len(test_files)} test files in {source_dir}")
    
    for source in test_files:
        print(f"Testing {source.name}...", end=" ")
        
        exe_path = compile_and_link(source, isa, output_dir)
        if not exe_path:
            print("✗ compile/link failed")
            results.append(TestResult(
                name=source.stem,
                success=False,
                exit_code=None,
                timeout=False,
                duration=0,
                output="Compilation failed"
            ))
            continue
        
        result = run_under_qemu(exe_path, isa, timeout_sec)
        results.append(result)
        
        if result.timeout:
            print("⏰ TIMEOUT")
        elif result.success:
            print("✓")
        else:
            print(f"✗ (exit {result.exit_code})")
    
    return results

def print_summary(results: list[TestResult]) -> int:
    """Print test summary and return exit code."""
    print("\n=== Test Summary ===")
    
    passed = sum(1 for r in results if r.success)
    failed = sum(1 for r in results if not r.success and not r.timeout)
    timeouts = sum(1 for r in results if r.timeout)
    
    print(f"Passed:  {passed}")
    print(f"Failed:  {failed}")
    print(f"Timeout: {timeouts}")
    print(f"Total:   {len(results)}")
    
    if timeouts > 0:
        print("\nTimeouts:")
        for r in results:
            if r.timeout:
                print(f"  - {r.name}")
    
    return 0 if failed == 0 and timeouts == 0 else 1

def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description='Run tests under QEMU')
    parser.add_argument('--isa', required=True, choices=['aarch64', 'riscv64'],
                       help='Target ISA')
    parser.add_argument('test_dir', type=Path, nargs='?', default=Path("test/aot"),
                       help='Directory containing test files')
    parser.add_argument('--timeout', type=int, default=30,
                       help='Timeout per test in seconds')
    parser.add_argument('--json', dest='json_output', action='store_true',
                       help='Output results as JSON')
    args = parser.parse_args()
    
    if not args.test_dir.exists():
        print(f"[error] Test directory not found: {args.test_dir}")
        return 1
    
    results = run_test_suite(args.test_dir, args.isa, args.timeout)
    
    if args.json_output:
        json_data = [
            {
                'name': r.name,
                'success': r.success,
                'exit_code': r.exit_code,
                'timeout': r.timeout,
                'duration': r.duration
            }
            for r in results
        ]
        print(json.dumps(json_data, indent=2))
    else:
        return print_summary(results)
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
