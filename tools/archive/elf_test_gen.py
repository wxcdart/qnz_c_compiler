#!/usr/bin/env python3
"""
Generate known-good ELF files for testing the ELF writer.
Compares generated ELF files against reference outputs.
"""

import struct
import sys
import subprocess
from pathlib import Path

def create_minimal_elf64(text_code, text_offset=0x100):
    """Create a minimal valid ELF64 file for testing."""
    
    # ELF Header (64 bytes)
    e_ident = b'\x7fELF' + bytes([2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0])  # 16 bytes
    ehdr = e_ident
    ehdr += struct.pack('<HHI', 2, 0x3E, 1)  # e_type, e_machine, e_version
    ehdr += struct.pack('<Q', 0)  # e_entry
    ehdr += struct.pack('<Q', 0)  # e_phoff
    ehdr += struct.pack('<Q', 64)  # e_shoff (section headers at 64)
    ehdr += struct.pack('<I', 0)  # e_flags
    ehdr += struct.pack('<HHHHHH', 64, 0, 0, 64, 4, 3)  # sizes and counts
    
    # Section headers (4 x 64 bytes = 256 bytes)
    # .null (index 0)
    null_shdr = b'\x00' * 64
    
    # .text (index 1)
    text_shdr = struct.pack('<IIQQQQIIQQ',
        1, 1, 6, 0, text_offset, len(text_code), 0, 0, 16, 0)
    
    # .shstrtab (index 2)
    shstrtab_shdr = struct.pack('<IIQQQQIIQQ',
        7, 3, 0, 0, 0, 0, 0, 0, 1, 0)  # sh_name=7 (".text\0")
    
    # .symtab (index 3)
    symtab_shdr = struct.pack('<IIQQQQIIQQ',
        16, 2, 0, 0, 0, 24, 0, 0, 8, 24)  # sh_link=0, sh_entsize=24
    
    # Section name string table
    shstrtab = b'\x00.text\x00.shstrtab\x00.symtab\x00'
    
    # .symtab (null symbol)
    symtab = b'\x00' * 24
    
    # Build final file
    elf = ehdr + null_shdr + text_shdr + shstrtab_shdr + symtab_shdr
    elf += text_code.ljust(0x100 - text_offset, b'\x00') if text_offset > 64 else text_code
    elf += shstrtab
    elf += symtab
    
    return elf

def main():
    # Test code: mov rax, 42; ret
    test_code = bytes([0x48, 0xc7, 0xc0, 0x2a, 0x00, 0x00, 0x00, 0xc3])
    
    elf = create_minimal_elf64(test_code)
    
    output = sys.argv[1] if len(sys.argv) > 1 else '/tmp/ref_elf.o'
    with open(output, 'wb') as f:
        f.write(elf)
    
    print(f"Created reference ELF: {output}")
    print(f"Size: {len(elf)} bytes")
    
    # Verify with readelf
    result = subprocess.run(['readelf', '-h', output], capture_output=True, text=True)
    print("\nELF Header:")
    print(result.stdout)

if __name__ == '__main__':
    main()
