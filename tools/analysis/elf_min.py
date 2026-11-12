#!/usr/bin/env python3
"""Minimal read-only ELF parser shared by the raw-emitter check tools.

Reads just enough of an ELF file (ELF32/ELF64, little/big endian) for
riscv_enc_check.py and reloc_pair_check.py:

  * section headers (name, type, addr, offset, size, link, info, entsize)
  * the primary .symtab and its strtab
  * SHT_REL / SHT_RELA entries (offset, symbol index, type, addend,
    the section index being relocated via sh_info)

Deliberately not supported: the shnum==0 indirection through section 0,
extended numbering (sh_info > 1 symbol tables), program headers, compressed
sections.  Every object zerocc or the cross toolchain produces for our tests
is a plain file, so this is sufficient.

Usage:
    import elf_min
    elf = elf_min.load("/tmp/foo.o")
    text = elf.section_by_name(".text")
    raw  = elf.bytes(text)
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

SHT_NULL = 0
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_REL = 5

SHN_UNDEF = 0
SHN_ABS = 0xFFF1

STT_NOTYPE = 0
STT_OBJECT = 1
STT_FUNC = 2
STT_SECTION = 3


@dataclass
class Section:
    name: str = ""
    sh_type: int = 0
    sh_flags: int = 0
    sh_addr: int = 0
    sh_offset: int = 0
    sh_size: int = 0
    sh_link: int = 0
    sh_info: int = 0
    sh_entsize: int = 0


@dataclass
class Symbol:
    name: str = ""
    value: int = 0
    size: int = 0
    info: int = 0
    other: int = 0
    shndx: int = 0

    @property
    def st_type(self) -> int:
        return self.info & 0xF

    @property
    def st_bind(self) -> int:
        return self.info >> 4

    @property
    def is_section_symbol(self) -> bool:
        """STT_SECTION, or the unnamed-section-symbol convention (empty name
        pointing at a real section). GNU ld's 'section symbol with an addend'
        rejection keys on this."""
        return (self.st_type == STT_SECTION
                or (self.name == "" and self.shndx not in (SHN_UNDEF, SHN_ABS)))


@dataclass
class Reloc:
    offset: int = 0
    sym: int = 0
    r_type: int = 0
    addend: int = 0
    section_idx: int = 0   # sh_info of the rela section: section being relocated


@dataclass
class ElfFile:
    path: str
    is64: bool
    endian: str
    e_machine: int
    sections: list = field(default_factory=list)
    symbols: list = field(default_factory=list)     # primary symtab, in order
    symtab_section: int = -1
    relocations: list = field(default_factory=list)  # all REL/RELA entries
    raw: bytes = b""

    def section_by_name(self, name: str) -> Section | None:
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def bytes(self, sec: Section) -> bytes:
        return self.raw[sec.sh_offset:sec.sh_offset + sec.sh_size]


def _cstr(buf: bytes, off: int) -> str:
    end = buf.find(b"\0", off)
    return buf[off:end if end >= 0 else len(buf)].decode("utf-8", "replace")


def load(path: str) -> ElfFile:
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF file")

    e = ElfFile(path=path, is64=(raw[4] == 2),
                endian="<" if raw[5] == 1 else ">", e_machine=0, raw=raw)
    u = lambda off, fmt: struct.unpack_from(e.endian + fmt, raw, off)

    if e.is64:
        (_, e.e_machine, _, _, _, shoff, _, _, _, _, shentsize, shstrndx, shnum) = \
            u(16, "HHIQQQIHHHHHH")
    else:
        (_, e.e_machine, _, _, _, shoff, _, _, _, _, shentsize, shstrndx, shnum) = \
            u(16, "HHIIIIIHHHHHH")

    # The zerocc raw writer appends the shstrtab header beyond e_shnum in
    # some objects; read trailing non-NULL section headers so names resolve.
    if shnum > 0:
        fmt_sh = "IIQQQQIIQQ" if e.is64 else "IIIIIIIIII"
        n = shnum
        while shoff + n * shentsize + shentsize <= len(raw):
            hdr = u(shoff + n * shentsize, fmt_sh)
            if all(v == 0 for v in hdr):
                break
            n += 1
        shnum = n

    name_offs = []
    for i in range(shnum):
        off = shoff + i * shentsize
        if e.is64:
            (name_off, styp, flags, addr, offset, size, link, info, _align, entsize) = \
                u(off, "IIQQQQIIQQ")
        else:
            (name_off, styp, flags, addr, offset, size, link, info, _align, entsize) = \
                u(off, "IIIIIIIIII")
        name_offs.append(name_off)
        e.sections.append(Section(name="", sh_type=styp, sh_flags=flags,
                                  sh_addr=addr, sh_offset=offset, sh_size=size,
                                  sh_link=link, sh_info=info, sh_entsize=entsize))

    # Section names live in .shstrtab. Tolerate (as GNU readelf does) the
    # zerocc raw writer's off-by-one e_shstrndx/e_shnum: fall back to the
    # STRTAB section not referenced as a symtab strtab.
    shstr = None
    if 0 < shstrndx < len(e.sections):
        shstr = e.sections[shstrndx]
    elif shstrndx:
        if shoff + shstrndx * shentsize + shentsize <= len(raw):
            (noff, styp, _fl, _ad, soff, ssize, _lk, _inf, _al, _es) = \
                u(shoff + shstrndx * shentsize, "IIQQQQIIQQ")
            shstr = Section(name="", sh_type=styp, sh_flags=_fl, sh_addr=_ad,
                            sh_offset=soff, sh_size=ssize)
        else:
            used = {s.sh_link for s in e.sections if s.sh_type == SHT_SYMTAB}
            cands = [s for s in e.sections if s.sh_type == SHT_STRTAB
                     and e.sections.index(s) not in used]
            if len(cands) == 1:
                shstr = cands[0]
    if shstr is not None:
        buf = raw[shstr.sh_offset:shstr.sh_offset + shstr.sh_size]
        for sec, name_off in zip(e.sections, name_offs):
            sec.name = _cstr(buf, name_off)

    # Primary symbol table.
    for idx, sec in enumerate(e.sections):
        if sec.sh_type != SHT_SYMTAB or sec.sh_link >= len(e.sections):
            continue
        e.symtab_section = idx
        link = e.sections[sec.sh_link]
        strtab = raw[link.sh_offset:link.sh_offset + link.sh_size]
        entsize = sec.sh_entsize or (24 if e.is64 else 16)
        for i in range(sec.sh_size // entsize):
            off = sec.sh_offset + i * entsize
            if e.is64:
                noff, info, other, shndx, value, size = u(off, "IBBHQQ")
            else:
                noff, info, other, shndx, value, size = u(off, "IBBHII")
            e.symbols.append(Symbol(name=_cstr(strtab, noff), value=value,
                                    size=size, info=info, other=other,
                                    shndx=shndx))
        break  # primary symtab only

    # Relocations.
    for idx, sec in enumerate(e.sections):
        if sec.sh_type not in (SHT_REL, SHT_RELA):
            continue
        has_addend = sec.sh_type == SHT_RELA
        entsize = sec.sh_entsize or (24 if e.is64 else 12)
        for i in range(sec.sh_size // entsize):
            off = sec.sh_offset + i * entsize
            if e.is64:
                if has_addend:
                    roff, rinfo, radd = u(off, "QQq")
                else:
                    roff, rinfo = u(off, "QQ")
                    radd = 0
            else:
                if has_addend:
                    roff, rinfo, radd = u(off, "IIi")
                else:
                    roff, rinfo = u(off, "II")
                    radd = 0
            if e.is64:
                sym, rtype = rinfo >> 32, rinfo & 0xFFFFFFFF
            else:
                sym, rtype = rinfo >> 8, rinfo & 0xFF
            e.relocations.append(Reloc(offset=roff, sym=sym, r_type=rtype,
                                       addend=radd, section_idx=sec.sh_info))
    return e


def text_bytes(e: ElfFile) -> tuple[Section, bytes]:
    """Return (.text section, its bytes); raise if missing."""
    sec = e.section_by_name(".text")
    if sec is None:
        raise ValueError(f"{e.path}: no .text section")
    return sec, e.bytes(sec)


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        f = load(p)
        print(f"{p}: ELF{'64' if f.is64 else '32'} machine={f.e_machine} "
              f"sections={len(f.sections)} syms={len(f.symbols)} "
              f"relocs={len(f.relocations)}")
