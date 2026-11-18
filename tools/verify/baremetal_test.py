#!/usr/bin/env python3
"""baremetal_test.py - Verification suite for ZeroCC baremetal CPU support.

Tests:
1. Linker script parsing (MEMORY regions with ORIGIN/LENGTH, ENTRY, stack_top)
2. LMA vs VMA separation in ELF Program Headers (PT_LOAD)
3. Boundary symbols (_sidata, _sdata, _edata, _sbss, _ebss, _stack_top)
4. Freestanding macros (__STDC_HOSTED__=0, __BAREMETAL__=1, undef __linux__)
5. Memory region capacity overflow error reporting
6. Multi-ISA baremetal support (x86-64, aarch64, aarch32, riscv32, riscv64)
"""

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

ROOT = Path(find_repo_root())
ZEROCC = ROOT / "build" / "bin" / "zerocc"

def parse_elf(path):
    with open(path, "rb") as f:
        data = f.read()

    assert data[:4] == b"\x7fELF", "Not an ELF file"
    is_32 = data[4] == 1

    if is_32:
        (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff,
         e_flags, e_ehsize, e_phentsize, e_phnum,
         e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    else:
        (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff,
         e_flags, e_ehsize, e_phentsize, e_phnum,
         e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)

    phdrs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if is_32:
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from("<IIIIIIII", data, off)
        else:
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from("<IIQQQQQQ", data, off)
        phdrs.append({
            "type": p_type,
            "flags": p_flags,
            "offset": p_offset,
            "vaddr": p_vaddr,
            "paddr": p_paddr,
            "filesz": p_filesz,
            "memsz": p_memsz,
            "align": p_align
        })

    # Read section headers
    shdrs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if is_32:
            sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack_from("<IIIIIIIIII", data, off)
        else:
            sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack_from("<IIQQQQIIQQ", data, off)
        shdrs.append({
            "name_idx": sh_name,
            "type": sh_type,
            "flags": sh_flags,
            "addr": sh_addr,
            "offset": sh_offset,
            "size": sh_size,
            "link": sh_link,
            "info": sh_info,
            "entsize": sh_entsize
        })

    # Get section names from shstrtab
    shstrtab = b""
    if e_shstrndx < len(shdrs):
        s = shdrs[e_shstrndx]
        shstrtab = data[s["offset"]:s["offset"] + s["size"]]

    for s in shdrs:
        name_end = shstrtab.find(b"\0", s["name_idx"])
        s["name"] = shstrtab[s["name_idx"]:name_end].decode("ascii", errors="replace") if name_end >= 0 else ""

    # Find symtab & strtab
    syms = {}
    symtab_sec = next((s for s in shdrs if s["type"] == 2), None) # SHT_SYMTAB
    if symtab_sec:
        strtab_sec = shdrs[symtab_sec["link"]]
        strtab = data[strtab_sec["offset"]:strtab_sec["offset"] + strtab_sec["size"]]
        sym_entsize = symtab_sec["entsize"]
        sym_count = symtab_sec["size"] // sym_entsize
        for i in range(sym_count):
            off = symtab_sec["offset"] + i * sym_entsize
            if is_32:
                st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", data, off)
            else:
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, off)
            st_type = st_info & 0xf
            if st_type == 3: # SHT_SECTION / STT_SECTION
                continue
            name_end = strtab.find(b"\0", st_name)
            sym_name = strtab[st_name:name_end].decode("ascii", errors="replace") if name_end >= 0 else ""
            if sym_name:
                syms[sym_name] = {"value": st_value, "size": st_size, "shndx": st_shndx}

    return {
        "is_32": is_32,
        "entry": e_entry,
        "phdrs": phdrs,
        "shdrs": shdrs,
        "syms": syms
    }

def test_baremetal_pipeline(isa):
    test_src = ROOT / "test" / "baremetal" / "test_baremetal.c"
    ld_script = ROOT / "test" / "baremetal" / "flash_ram.ld"

    with tempfile.TemporaryDirectory() as tmpdir:
        out_elf = Path(tmpdir) / f"baremetal_{isa}.elf"
        cmd = [
            str(ZEROCC),
            str(test_src),
            "--target", isa,
            "-T", str(ld_script),
            "-o", str(out_elf)
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 0, f"Compilation failed for {isa}: {res.stderr}\n{res.stdout}"
        assert out_elf.exists(), f"Output binary {out_elf} not found"

        elf = parse_elf(out_elf)

        # 1. Check entry point
        assert "Reset_Handler" in elf["syms"], f"Reset_Handler not in symtab for {isa}"
        reset_addr = elf["syms"]["Reset_Handler"]["value"]
        assert elf["entry"] == reset_addr, f"ELF entry 0x{elf['entry']:x} != Reset_Handler 0x{reset_addr:x} on {isa}"
        assert 0x08000000 <= elf["entry"] < 0x08000000 + 128 * 1024, f"Entry 0x{elf['entry']:x} not in Flash region on {isa}"

        # 2. Check PT_LOAD segments: RX (Flash) and RW (RAM with Flash LMA)
        pt_loads = [p for p in elf["phdrs"] if p["type"] == 1]
        assert len(pt_loads) >= 2, f"Expected at least 2 PT_LOAD segments on {isa}, got {len(pt_loads)}"

        rx = pt_loads[0]
        rw = pt_loads[1]

        # RX segment (Flash)
        assert rx["vaddr"] == 0x08000000, f"RX vaddr 0x{rx['vaddr']:x} != 0x08000000 on {isa}"
        assert rx["paddr"] == 0x08000000, f"RX paddr (LMA) 0x{rx['paddr']:x} != 0x08000000 on {isa}"

        # RW segment (RAM VMA, Flash LMA)
        assert rw["vaddr"] == 0x20000000, f"RW vaddr 0x{rw['vaddr']:x} != 0x20000000 on {isa}"
        assert rw["paddr"] >= 0x08000000 and rw["paddr"] < 0x08020000, f"RW paddr (LMA) 0x{rw['paddr']:x} not in Flash on {isa}"
        assert rw["paddr"] != rw["vaddr"], f"RW paddr (LMA) 0x{rw['paddr']:x} should NOT equal vaddr 0x{rw['vaddr']:x} on {isa}"

        # 3. Check boundary symbols
        syms = elf["syms"]
        for sym_name in ("_sidata", "_sdata", "_edata", "_sbss", "_ebss", "_stack_top"):
            assert sym_name in syms, f"Required boundary symbol {sym_name} missing on {isa}"

        sidata = syms["_sidata"]["value"]
        sdata = syms["_sdata"]["value"]
        edata = syms["_edata"]["value"]
        sbss = syms["_sbss"]["value"]
        ebss = syms["_ebss"]["value"]
        stack_top = syms["_stack_top"]["value"]

        # _sidata must match RW paddr (LMA in Flash)
        assert sidata == rw["paddr"], f"_sidata (0x{sidata:x}) != RW paddr (0x{rw['paddr']:x}) on {isa}"

        # _sdata must match RW vaddr (0x20000000)
        assert sdata == 0x20000000, f"_sdata (0x{sdata:x}) != 0x20000000 on {isa}"

        # _edata must be sdata + data size
        assert edata > sdata, f"_edata (0x{edata:x}) should be > _sdata (0x{sdata:x}) on {isa}"
        assert edata - sdata == rw["filesz"], f"_edata - _sdata ({edata - sdata}) != rw filesz ({rw['filesz']}) on {isa}"

        # _sbss and _ebss
        assert sbss >= edata, f"_sbss (0x{sbss:x}) should be >= _edata (0x{edata:x}) on {isa}"
        assert ebss >= sbss, f"_ebss (0x{ebss:x}) should be >= _sbss (0x{sbss:x}) on {isa}"

        # _stack_top must match ORIGIN(RAM) + LENGTH(RAM) = 0x20000000 + 32K = 0x20008000
        assert stack_top == 0x20008000, f"_stack_top (0x{stack_top:x}) != 0x20008000 on {isa}"

        print(f"ok   {isa:8s} entry=0x{elf['entry']:x} Flash(LMA)=0x{sidata:x} RAM(VMA)=0x{sdata:x}..0x{ebss:x} stack_top=0x{stack_top:x}")

def test_overflow_check():
    test_src = ROOT / "test" / "baremetal" / "test_baremetal.c"
    with tempfile.TemporaryDirectory() as tmpdir:
        tiny_ld = Path(tmpdir) / "tiny.ld"
        tiny_ld.write_text("""
MEMORY {
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 16
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 16
}
""")
        cmd = [
            str(ZEROCC),
            str(test_src),
            "-T", str(tiny_ld),
            "-o", str(Path(tmpdir) / "overflow.elf")
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode != 0, "Expected link to fail on region overflow"
        assert "region FLASH overflowed" in res.stderr, f"Expected overflow message in stderr, got: {res.stderr}"
        print("ok   region capacity overflow check")

def test_baremetal_preproc():
    # Test __STDC_HOSTED__=0 and __BAREMETAL__=1 in -ffreestanding mode
    with tempfile.NamedTemporaryFile("w", suffix=".c") as f:
        f.write("""
#if __STDC_HOSTED__ != 0
#error "__STDC_HOSTED__ should be 0"
#endif
#ifndef __BAREMETAL__
#error "__BAREMETAL__ should be defined"
#endif
#ifdef __linux__
#error "__linux__ should not be defined in baremetal mode"
#endif
int baremetal_ok = 1;
""")
        f.flush()
        cmd = [
            str(ZEROCC),
            "-E",
            "--baremetal",
            f.name
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 0, f"Preprocessor failed for baremetal macros: {res.stderr}"
        assert "baremetal_ok" in res.stdout
        print("ok   freestanding macro environment (__STDC_HOSTED__=0, __BAREMETAL__=1)")

def main():
    print("=== ZeroCC Baremetal CPU Scope Verification ===")
    assert ZEROCC.exists(), f"Compiler binary {ZEROCC} does not exist"

    test_baremetal_preproc()
    test_overflow_check()

    for isa in ("x86-64", "aarch64", "aarch32", "riscv32", "riscv64"):
        test_baremetal_pipeline(isa)

    print("\nAll baremetal CPU scope verification tests PASSED.")

if __name__ == "__main__":
    main()
