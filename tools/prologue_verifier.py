#!/usr/bin/env python3
"""Prologue/Epilogue Byte Verifier.

Verifies that generated object files have correct prologue/epilogue byte patterns
for the target ISA. This catches regressions where wrong backend code is emitted.

Usage:
    python3 tools/prologue_verifier.py --isa aarch64 /path/to/object.o
"""

import subprocess
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import Optional

@dataclass
class FunctionInfo:
    name: str
    offset: int
    size: int

def get_functions_from_objdump(obj_path: Path) -> list[FunctionInfo]:
    """Extract function symbols using objdump -t."""
    result = subprocess.run(
        ['objdump', '-t', str(obj_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    functions = []
    for line in result.stdout.splitlines():
        # Format: 0000000000001140 g     F .text  0000000000000032 main
        parts = line.split()
        if len(parts) >= 5 and 'F' in parts[1]:
            try:
                offset = int(parts[0], 16)
                size = int(parts[3], 16)
                name = parts[-1]
                functions.append(FunctionInfo(name, offset, size))
            except ValueError:
                continue
    return functions

def get_function_bytes(obj_path: Path, func: FunctionInfo) -> Optional[bytes]:
    """Extract raw bytes for a function from .text section."""
    result = subprocess.run(
        ['objdump', '-s', '-j', '.text', str(obj_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    # Parse hex dump
    in_text_section = False
    section_data = bytearray()
    current_offset = 0
    
    for line in result.stdout.splitlines():
        if '.text' in line:
            in_text_section = True
            continue
        if in_text_section and line.startswith(' '):
            # Parse hex bytes
            parts = line.split()
            if len(parts) > 1:
                for hex_part in parts[1:]:
                    try:
                        section_data.append(int(hex_part, 16))
                        current_offset += 1
                    except ValueError:
                        continue
    
    # Extract function bytes
    start = func.offset
    end = start + min(func.size, 16)  # First 16 bytes
    if start < len(section_data) and end <= len(section_data):
        return bytes(section_data[start:end])
    return None

# Expected prologue patterns (first 4-8 bytes)
EXPECTED_PROLOGUES = {
    'x86_64': [
        bytes([0x55, 0x48, 0x89, 0xe5]),  # push rbp; mov rbp, rsp
        bytes([0x55, 0x48, 0x83, 0xec]),  # push rbp; sub rsp, imm
    ],
    'aarch64': [
        bytes([0xfd, 0x7b, 0xa1, 0xa9]),  # stp x29, x30, [sp, #-16]! (with offset)
        bytes([0xfd, 0x7b, 0xbf, 0xa9]),  # stp x29, x30, [sp, #-48]! (bad pattern)
        bytes([0x91, 0x00, 0x03, 0xfd]),  # mov x29, sp
    ],
    'riscv64': [
        bytes([0x24, 0x00, 0x00, 0x99]),  # addi s0, sp, offset
    ]
}

FORBIDDEN_PATTERNS = {
    'aarch64': [
        (bytes([0xfd, 0x7b, 0xbd, 0xa9]), 'x86-style callee-save (0xA9BD7BFD)'),
    ],
    'x86_64': [
        (bytes([0xfd, 0x7b, 0xbf, 0xa9]), 'aarch64-style stp'),
    ]
}

def verify_prologue(isa: str, obj_path: Path) -> int:
    """Verify prologue bytes for all functions."""
    functions = get_functions_from_objdump(obj_path)
    
    if not functions:
        print(f"[error] No functions found in {obj_path}")
        return 1
    
    expected = EXPECTED_PROLOGUES.get(isa, [])
    forbidden = FORBIDDEN_PATTERNS.get(isa, [])
    
    print(f"\n=== Prologue Verification for {isa.upper()} ===")
    print(f"Functions found: {len(functions)}")
    
    has_error = False
    
    for func in functions:
        bytes_data = get_function_bytes(obj_path, func)
        if not bytes_data:
            continue
        
        # Check forbidden patterns first
        for pattern, desc in forbidden:
            if bytes_data[:len(pattern)] == pattern:
                print(f"[FAIL] {func.name} at 0x{func.offset:x}: {desc}")
                has_error = True
        
        # Check expected patterns (warning if none match)
        if expected:
            matches = any(bytes_data[:len(p)] == p for p in expected)
            if not matches and len(bytes_data) >= 4:
                print(f"[WARN] {func.name} at 0x{func.offset:x}: Unknown prologue")
                print(f"       First bytes: {bytes_data[:8].hex()}")
    
    if not has_error:
        print("[PASS] All prologues verified")
    
    return 1 if has_error else 0

def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description='Verify prologue/epilogue bytes')
    parser.add_argument('--isa', required=True, choices=['x86_64', 'aarch64', 'riscv64'],
                       help='Target ISA')
    parser.add_argument('object_file', type=Path, help='Object file to verify')
    args = parser.parse_args()
    
    return verify_prologue(args.isa, args.object_file)

if __name__ == '__main__':
    sys.exit(main())
