#!/usr/bin/env python3
"""
ELF AArch64 Verification Tool

Verifies that ELF output files have the correct machine type for AArch64.

Usage:
    python3 tools/verify_elf_aarch64.py <elf_file>

Example:
    ./build/bin/zerocc -c test.c -o test.o --target aarch64
    python3 tools/verify_elf_aarch64.py test.o
"""

import sys
import struct
from pathlib import Path

# ELF constants
ELFMAG = b'\x7fELF'
ELFCLASS64 = 2
EM_X86_64 = 62
EM_AARCH64 = 183
EM_RISCV = 243

def verify_elf(filepath: Path) -> bool:
    """Verify ELF file has correct machine type."""
    if not filepath.exists():
        print(f"Error: File not found: {filepath}")
        return False
    
    with open(filepath, 'rb') as f:
        # Read ELF header
        e_ident = f.read(16)
        
        # Check magic number
        if e_ident[:4] != ELFMAG:
            print(f"Error: Not a valid ELF file (bad magic)")
            return False
        
        # Check class (32/64 bit)
        if e_ident[4] != ELFCLASS64:
            print(f"Error: Not a 64-bit ELF file")
            return False
        
        # Read e_machine (offset 18, 2 bytes)
        f.seek(18)
        e_machine = struct.unpack('<H', f.read(2))[0]
        
        # Check machine type
        if e_machine == EM_X86_64:
            print(f"✓ ELF machine type: x86-64 (0x3E)")
            return True
        elif e_machine == EM_AARCH64:
            print(f"✓ ELF machine type: AArch64 (0xB7)")
            return True
        elif e_machine == EM_RISCV:
            print(f"✓ ELF machine type: RISC-V (0xF3)")
            return True
        else:
            print(f"✗ Unknown ELF machine type: 0x{e_machine:04X}")
            return False

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 tools/verify_elf_aarch64.py <elf_file>")
        print()
        print("Example:")
        print("  ./build/bin/zerocc -c test.c -o test.o --target aarch64")
        print("  python3 tools/verify_elf_aarch64.py test.o")
        return 1
    
    filepath = Path(sys.argv[1])
    
    print("=" * 60)
    print("ELF AArch64 Verification")
    print("=" * 60)
    print()
    print(f"File: {filepath}")
    print()
    
    if verify_elf(filepath):
        print()
        print("✓ ELF file is valid")
        return 0
    else:
        print()
        print("✗ ELF file verification failed")
        return 1

if __name__ == "__main__":
    sys.exit(main())
