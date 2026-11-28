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
                syms[sym_name] = {"value": st_value, "size": st_size, "shndx": st_shndx, "other": st_other}

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

def test_extended_linker_script():
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        ld_path = tmp / "firmware.ld"
        ld_path.write_text("""
MEMORY {
    ROM (rx)     : ORIGIN = 0x08000000, LENGTH = 256K
    RAM (rwx)    : ORIGIN = 0x20000000, LENGTH = 64K
    CCMRAM (rw)  : ORIGIN = 0x10000000, LENGTH = 16K
}

ENTRY(Reset_Handler)

SECTIONS {
    .text : ALIGN(8) {
        _stext = .;
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
        _etext = .;
    } > ROM

    .data : ALIGN(8) {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > RAM AT> ROM

    .bss (NOLOAD) : ALIGN(8) {
        _sbss = .;
        *(.bss*)
        _ebss = .;
    } > RAM

    .noinit (NOLOAD) : ALIGN(8) {
        _snoinit = .;
        *(.noinit*)
        _enoinit = .;
    } > CCMRAM

    _sidata = LOADADDR(.data);
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);

    PROVIDE(prov_sym = 0x1234);
    PROVIDE_HIDDEN(prov_hidden_sym = 0x5678);
}
""")
        c_path = tmp / "main.c"
        c_path.write_text("""
extern int prov_sym;
extern int prov_hidden_sym;
__attribute__((section(".noinit"))) int uninit_buf[16];
int my_data = 42;
int my_bss;

void Reset_Handler(void) {
    uninit_buf[0] = prov_sym + prov_hidden_sym + my_data + my_bss;
}
""")
        out_elf = tmp / "firmware.elf"
        cmd = [
            str(ZEROCC),
            str(c_path),
            "-T", str(ld_path),
            "-o", str(out_elf)
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 0, f"Compilation failed: {res.stderr}\n{res.stdout}"
        assert out_elf.exists(), f"Output binary {out_elf} not found"

        elf = parse_elf(out_elf)

        # 1. Check entry point
        assert "Reset_Handler" in elf["syms"], "Reset_Handler missing"
        assert elf["entry"] == elf["syms"]["Reset_Handler"]["value"]
        assert 0x08000000 <= elf["entry"] < 0x08040000, "Reset_Handler not in ROM"

        # 2. Check PROVIDE and PROVIDE_HIDDEN symbols
        assert "prov_sym" in elf["syms"], "prov_sym missing"
        assert elf["syms"]["prov_sym"]["value"] == 0x1234, f"prov_sym value != 0x1234"
        assert (elf["syms"]["prov_sym"]["other"] & 3) == 0, "prov_sym should have STV_DEFAULT visibility"

        assert "prov_hidden_sym" in elf["syms"], "prov_hidden_sym missing"
        assert elf["syms"]["prov_hidden_sym"]["value"] == 0x5678, f"prov_hidden_sym value != 0x5678"
        assert (elf["syms"]["prov_hidden_sym"]["other"] & 3) == 2, "prov_hidden_sym should have STV_HIDDEN visibility (st_other=2)"

        # 3. Check stack top
        assert "_stack_top" in elf["syms"], "_stack_top missing"
        assert elf["syms"]["_stack_top"]["value"] == 0x20010000, f"_stack_top != 0x20010000"

        # 4. Check boundaries and LMA/VMA
        assert "_sidata" in elf["syms"]
        assert "_sdata" in elf["syms"]
        assert elf["syms"]["_sdata"]["value"] == 0x20000000

        # 5. Check multi-region overflow check on CCMRAM
        ld_tiny_ccm = tmp / "tiny_ccm.ld"
        ld_tiny_ccm.write_text("""
MEMORY {
    ROM (rx)     : ORIGIN = 0x08000000, LENGTH = 256K
    RAM (rwx)    : ORIGIN = 0x20000000, LENGTH = 64K
    CCMRAM (rw)  : ORIGIN = 0x10000000, LENGTH = 4
}
ENTRY(Reset_Handler)
SECTIONS {
    .text : { *(.text*) } > ROM
    .data : { *(.data*) } > RAM AT> ROM
    .bss (NOLOAD) : { *(.bss*) } > CCMRAM
    PROVIDE(prov_sym = 0);
    PROVIDE_HIDDEN(prov_hidden_sym = 0);
}
""")
        cmd2 = [
            str(ZEROCC),
            str(c_path),
            "-T", str(ld_tiny_ccm),
            "-o", str(tmp / "overflow_ccm.elf")
        ]
        res2 = subprocess.run(cmd2, capture_output=True, text=True)
        assert res2.returncode != 0, "Expected link failure on CCMRAM overflow"
        assert "region CCMRAM overflowed" in res2.stderr, f"Expected CCMRAM overflow message, got: {res2.stderr}"

        print("ok   extended linker script (MEMORY, > REGION AT> LMA, NOLOAD, PROVIDE/PROVIDE_HIDDEN, ALIGN)")

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

def test_interrupt_handlers():
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        c_file = tmp / "isr.c"
        c_file.write_text("""
volatile int g_flag;

__attribute__((interrupt)) void isr_default(void) {
    g_flag = 1;
}

__attribute__((interrupt("supervisor"))) void isr_sup(void) {
    g_flag = 2;
}

__attribute__((interrupt("user"))) void isr_usr(void) {
    g_flag = 3;
}

__attribute__((interrupt("IRQ"))) void isr_arm_irq(void) {
    g_flag = 4;
}
""")

        # 1. RISC-V 32 ASM & ELF
        s_rv32 = tmp / "isr_rv32.s"
        res = subprocess.run([str(ZEROCC), "-S", "--target=riscv32", str(c_file), "-o", str(s_rv32)], capture_output=True, text=True)
        assert res.returncode == 0, f"RISC-V 32 asm emission failed: {res.stderr}"
        txt = s_rv32.read_text()
        assert "mret" in txt, "Expected mret in RISC-V 32 ISR"
        assert "sret" in txt, "Expected sret in RISC-V 32 supervisor ISR"
        assert "uret" in txt, "Expected uret in RISC-V 32 user ISR"
        assert "addi\tsp, sp, -64" in txt, "Expected caller-saved stack allocation in RISC-V 32 ISR"

        obj_rv32 = tmp / "isr_rv32.o"
        res = subprocess.run([str(ZEROCC), "-c", "--target=riscv32", str(c_file), "-o", str(obj_rv32)], capture_output=True, text=True)
        assert res.returncode == 0, f"RISC-V 32 object emission failed: {res.stderr}"
        data_rv32 = obj_rv32.read_bytes()
        assert b"\x73\x00\x20\x30" in data_rv32, "mret opcode 0x30200073 not found in RISC-V 32 binary"
        assert b"\x73\x00\x20\x10" in data_rv32, "sret opcode 0x10200073 not found in RISC-V 32 binary"

        # 2. RISC-V 64 ASM & ELF
        s_rv64 = tmp / "isr_rv64.s"
        res = subprocess.run([str(ZEROCC), "-S", "--target=riscv64", str(c_file), "-o", str(s_rv64)], capture_output=True, text=True)
        assert res.returncode == 0, f"RISC-V 64 asm emission failed: {res.stderr}"
        txt = s_rv64.read_text()
        assert "mret" in txt, "Expected mret in RISC-V 64 ISR"
        assert "sret" in txt, "Expected sret in RISC-V 64 supervisor ISR"
        assert "uret" in txt, "Expected uret in RISC-V 64 user ISR"
        assert "addi\tsp, sp, -128" in txt, "Expected caller-saved stack allocation in RISC-V 64 ISR"

        obj_rv64 = tmp / "isr_rv64.o"
        res = subprocess.run([str(ZEROCC), "-c", "--target=riscv64", str(c_file), "-o", str(obj_rv64)], capture_output=True, text=True)
        assert res.returncode == 0, f"RISC-V 64 object emission failed: {res.stderr}"
        data_rv64 = obj_rv64.read_bytes()
        assert b"\x73\x00\x20\x30" in data_rv64, "mret opcode not found in RISC-V 64 binary"

        # 3. ARM (AArch32) ASM & ELF
        s_arm = tmp / "isr_arm.s"
        res = subprocess.run([str(ZEROCC), "-S", "--target=arm", str(c_file), "-o", str(s_arm)], capture_output=True, text=True)
        assert res.returncode == 0, f"ARM asm emission failed: {res.stderr}"
        txt = s_arm.read_text()
        assert "subs\tpc, lr, #4" in txt, "Expected subs pc, lr, #4 in ARM ISR"
        assert "push\t{r0, r1, r2, r3, r12}" in txt, "Expected push {r0..r3, r12} in ARM ISR"
        assert "pop\t{r0, r1, r2, r3, r12}" in txt, "Expected pop {r0..r3, r12} in ARM ISR"

        obj_arm = tmp / "isr_arm.o"
        res = subprocess.run([str(ZEROCC), "-c", "--target=arm", str(c_file), "-o", str(obj_arm)], capture_output=True, text=True)
        assert res.returncode == 0, f"ARM object emission failed: {res.stderr}"
        data_arm = obj_arm.read_bytes()
        assert b"\x04\xf0\x5e\xe2" in data_arm, "subs pc, lr, #4 opcode not found in ARM binary"
        assert b"\x0f\x10\x2d\xe9" in data_arm, "push {r0-r3, r12} opcode not found in ARM binary"

        print("ok   interrupt handlers & ISR prologues/epilogues (__attribute__((interrupt)) rv32/rv64/arm)")

def test_baremetal_execution():
    test_src = """
int puts(const char *s);
void *malloc(unsigned long sz);
void free(void *p);
char *strcpy(char *dst, const char *src);

int g_init = 12345;
int g_bss;

int main(void) {
    if (g_init != 12345) return 1;
    if (g_bss != 0) return 2;
    puts("HELLO BAREMETAL FROM PUTS");
    char *p = malloc(32);
    if (!p) return 3;
    strcpy(p, "MALLOC_OK");
    puts(p);
    free(p);
    return 42;
}
"""
    ld_script = ROOT / "test" / "baremetal" / "flash_ram.ld"
    with tempfile.TemporaryDirectory() as tmpdir:
        src_file = Path(tmpdir) / "prog.c"
        src_file.write_text(test_src)
        out_elf = Path(tmpdir) / "prog.elf"
        cmd = [
            str(ZEROCC),
            "--target", "riscv32",
            "-T", str(ld_script),
            str(src_file),
            "-o", str(out_elf)
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 0, f"Baremetal compile failed: {res.stderr}\n{res.stdout}"
        assert out_elf.exists(), f"Output binary {out_elf} not found"

        sim = run_rv32_sim(out_elf)
        assert sim["exit_code"] == 42, f"Expected exit_code 42, got {sim['exit_code']}"
        assert "HELLO BAREMETAL FROM PUTS\n" in sim["uart_output"]
        assert "MALLOC_OK\n" in sim["uart_output"]
        print(f"ok   baremetal runtime execution & MMIO UART output (exit=42, cycles={sim['cycles']})")

def run_rv32_sim(elf_path, max_cycles=1000000):
    with open(elf_path, "rb") as f:
        elf = f.read()

    entry = struct.unpack_from("<I", elf, 24)[0]
    phoff = struct.unpack_from("<I", elf, 28)[0]
    phentsize = struct.unpack_from("<H", elf, 42)[0]
    phnum = struct.unpack_from("<H", elf, 44)[0]

    flash = bytearray(128 * 1024)
    ram = bytearray(32 * 1024)
    FLASH_BASE = 0x08000000
    RAM_BASE = 0x20000000

    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from("<IIIIIIII", elf, off)
        data = elf[p_offset:p_offset + p_filesz]
        if FLASH_BASE <= p_paddr < FLASH_BASE + len(flash):
            idx = p_paddr - FLASH_BASE
            flash[idx:idx + p_filesz] = data
        elif RAM_BASE <= p_paddr < RAM_BASE + len(ram):
            idx = p_paddr - RAM_BASE
            ram[idx:idx + p_filesz] = data

    uart_tx = []

    def read8(addr):
        if FLASH_BASE <= addr < FLASH_BASE + len(flash):
            return flash[addr - FLASH_BASE]
        elif RAM_BASE <= addr < RAM_BASE + len(ram):
            return ram[addr - RAM_BASE]
        return 0

    def read16(addr):
        return read8(addr) | (read8(addr + 1) << 8)

    def read32(addr):
        return read8(addr) | (read8(addr + 1) << 8) | (read8(addr + 2) << 16) | (read8(addr + 3) << 24)

    def write8(addr, val):
        val &= 0xff
        if addr == 0x10000000:
            uart_tx.append(chr(val))
            return
        if RAM_BASE <= addr < RAM_BASE + len(ram):
            ram[addr - RAM_BASE] = val

    def write16(addr, val):
        write8(addr, val & 0xff)
        write8(addr + 1, (val >> 8) & 0xff)

    def write32(addr, val):
        write8(addr, val & 0xff)
        write8(addr + 1, (val >> 8) & 0xff)
        write8(addr + 2, (val >> 16) & 0xff)
        write8(addr + 3, (val >> 24) & 0xff)

    regs = [0] * 32
    regs[2] = 0x20008000
    pc = entry

    def to_signed(v):
        v &= 0xffffffff
        return v if v < 0x80000000 else v - 0x100000000

    def to_unsigned(v):
        return v & 0xffffffff

    cycles = 0
    while cycles < max_cycles:
        cycles += 1
        regs[0] = 0
        insn = read32(pc)
        opcode = insn & 0x7f
        rd = (insn >> 7) & 0x1f
        funct3 = (insn >> 12) & 0x7
        rs1 = (insn >> 15) & 0x1f
        rs2 = (insn >> 20) & 0x1f
        funct7 = (insn >> 25) & 0x7f

        next_pc = pc + 4

        if opcode == 0x37: # LUI
            imm = (insn >> 12) << 12
            if rd: regs[rd] = to_unsigned(imm)
        elif opcode == 0x17: # AUIPC
            imm = (insn >> 12) << 12
            if rd: regs[rd] = to_unsigned(pc + imm)
        elif opcode == 0x6f: # JAL
            imm = ((insn >> 31) << 20) | (((insn >> 21) & 0x3ff) << 1) | (((insn >> 20) & 1) << 11) | (((insn >> 12) & 0xff) << 12)
            if imm & (1 << 20): imm -= (1 << 21)
            if rd: regs[rd] = next_pc
            next_pc = to_unsigned(pc + imm)
        elif opcode == 0x67: # JALR
            imm = insn >> 20
            if imm & (1 << 11): imm -= (1 << 12)
            t = next_pc
            next_pc = (regs[rs1] + imm) & ~1
            if rd: regs[rd] = t
        elif opcode == 0x63: # Branch
            imm = ((insn >> 31) << 12) | (((insn >> 25) & 0x3f) << 5) | (((insn >> 8) & 0xf) << 1) | (((insn >> 7) & 1) << 11)
            if imm & (1 << 12): imm -= (1 << 13)
            taken = False
            s1, s2 = to_signed(regs[rs1]), to_signed(regs[rs2])
            u1, u2 = to_unsigned(regs[rs1]), to_unsigned(regs[rs2])
            if funct3 == 0: taken = (s1 == s2)
            elif funct3 == 1: taken = (s1 != s2)
            elif funct3 == 4: taken = (s1 < s2)
            elif funct3 == 5: taken = (s1 >= s2)
            elif funct3 == 6: taken = (u1 < u2)
            elif funct3 == 7: taken = (u1 >= u2)
            if taken: next_pc = to_unsigned(pc + imm)
        elif opcode == 0x03: # Load
            imm = insn >> 20
            if imm & (1 << 11): imm -= (1 << 12)
            addr = to_unsigned(regs[rs1] + imm)
            if funct3 == 0: v = read8(addr); v = v if v < 0x80 else v - 0x100
            elif funct3 == 1: v = read16(addr); v = v if v < 0x8000 else v - 0x10000
            elif funct3 == 2: v = read32(addr)
            elif funct3 == 4: v = read8(addr)
            elif funct3 == 5: v = read16(addr)
            else: v = 0
            if rd: regs[rd] = to_unsigned(v)
        elif opcode == 0x23: # Store
            imm = ((insn >> 25) << 5) | ((insn >> 7) & 0x1f)
            if imm & (1 << 11): imm -= (1 << 12)
            addr = to_unsigned(regs[rs1] + imm)
            val = regs[rs2]
            if funct3 == 0: write8(addr, val)
            elif funct3 == 1: write16(addr, val)
            elif funct3 == 2: write32(addr, val)
        elif opcode == 0x13: # OP-IMM
            imm = insn >> 20
            if imm & (1 << 11): imm -= (1 << 12)
            s1 = to_signed(regs[rs1])
            u1 = to_unsigned(regs[rs1])
            shamt = (insn >> 20) & 0x1f
            if funct3 == 0: res = s1 + imm
            elif funct3 == 1: res = u1 << shamt
            elif funct3 == 2: res = 1 if s1 < imm else 0
            elif funct3 == 3: res = 1 if u1 < to_unsigned(imm) else 0
            elif funct3 == 4: res = u1 ^ to_unsigned(imm)
            elif funct3 == 5:
                if (insn >> 30) & 1: res = s1 >> shamt
                else: res = u1 >> shamt
            elif funct3 == 6: res = u1 | to_unsigned(imm)
            elif funct3 == 7: res = u1 & to_unsigned(imm)
            if rd: regs[rd] = to_unsigned(res)
        elif opcode == 0x33: # OP
            s1, s2 = to_signed(regs[rs1]), to_signed(regs[rs2])
            u1, u2 = to_unsigned(regs[rs1]), to_unsigned(regs[rs2])
            if funct7 == 1:
                if funct3 == 0: res = (s1 * s2) & 0xffffffff
                elif funct3 == 1: res = (s1 * s2) >> 32
                elif funct3 == 2: res = (s1 * u2) >> 32
                elif funct3 == 3: res = (u1 * u2) >> 32
                elif funct3 == 4: res = s1 // s2 if s2 != 0 else -1
                elif funct3 == 5: res = u1 // u2 if u2 != 0 else 0xffffffff
                elif funct3 == 6: res = s1 % s2 if s2 != 0 else s1
                elif funct3 == 7: res = u1 % u2 if u2 != 0 else u1
            elif funct7 == 0:
                if funct3 == 0: res = s1 + s2
                elif funct3 == 1: res = u1 << (u2 & 0x1f)
                elif funct3 == 2: res = 1 if s1 < s2 else 0
                elif funct3 == 3: res = 1 if u1 < u2 else 0
                elif funct3 == 4: res = u1 ^ u2
                elif funct3 == 5: res = u1 >> (u2 & 0x1f)
                elif funct3 == 6: res = u1 | u2
                elif funct3 == 7: res = u1 & u2
            elif funct7 == 0x20:
                if funct3 == 0: res = s1 - s2
                elif funct3 == 5: res = s1 >> (u2 & 0x1f)
                else: res = 0
            else:
                res = 0
            if rd: regs[rd] = to_unsigned(res)
        elif opcode == 0x2f: # AMO
            funct5 = insn >> 27
            addr = regs[rs1]
            if funct5 == 2: # LR.W
                val = read32(addr)
                if rd: regs[rd] = to_unsigned(val)
            elif funct5 == 3: # SC.W
                write32(addr, regs[rs2])
                if rd: regs[rd] = 0
            elif funct5 == 1: # AMOSWAP.W
                val = read32(addr)
                write32(addr, regs[rs2])
                if rd: regs[rd] = to_unsigned(val)
            elif funct5 == 0: # AMOADD.W
                val = read32(addr)
                write32(addr, to_unsigned(val + regs[rs2]))
                if rd: regs[rd] = to_unsigned(val)
        elif opcode == 0x73:
            if insn == 0x10500073 or insn == 0x00000073 or insn == 0x00100073:
                break

        if next_pc == pc:
            break
        pc = next_pc

    parsed = parse_elf(elf_path)
    exit_code_addr = parsed["syms"].get("g_baremetal_exit_code", {}).get("value", 0)
    exit_code = read32(exit_code_addr) if exit_code_addr else None

    return {
        "cycles": cycles,
        "pc": pc,
        "exit_code": exit_code,
        "uart_output": "".join(uart_tx)
    }

def main():
    print("=== ZeroCC Baremetal CPU Scope Verification ===")
    assert ZEROCC.exists(), f"Compiler binary {ZEROCC} does not exist"

    test_baremetal_preproc()
    test_interrupt_handlers()
    test_overflow_check()
    test_extended_linker_script()

    for isa in ("x86-64", "aarch64", "aarch32", "thumb", "riscv32", "riscv64"):
        test_baremetal_pipeline(isa)

    test_baremetal_execution()

    print("\nAll baremetal CPU scope verification tests PASSED.")

if __name__ == "__main__":
    main()
