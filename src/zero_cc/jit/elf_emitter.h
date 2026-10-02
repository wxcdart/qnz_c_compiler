#ifndef ZERO_CC_JIT_ELF_EMITTER_H
#define ZERO_CC_JIT_ELF_EMITTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <elf.h>
#include <dlfcn.h>

#include "zero_cc/core/core.h"
#include "zero_cc/types/types.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/diagnostics.h"


/* --------------------------------------------------------------------------
 *  5. ELF64 EMITTERS
 *  -------------------------------------------------------------------------- */
#if defined(__has_include) && __has_include(<elf.h>)
#include <elf.h>
#else
#include "zero_elf.h"
#endif

#ifdef __linux__

static inline bool zero_cc_emit_elf_obj(ZeroCCCompiler *c, const char *filepath) {
    FILE *f = fopen(filepath, "wb");
    if (!f) return false;

    Elf64_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_ident[EI_OSABI] = ELFOSABI_SYSV;
    ehdr.e_type = ET_REL;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum = 6;
    ehdr.e_shstrndx = 3;

    /* Write section headers and body */
    size_t offset = sizeof(Elf64_Ehdr);
    Elf64_Shdr shdrs[6];
    memset(shdrs, 0, sizeof(shdrs));

    shdrs[1].sh_name = 1; shdrs[1].sh_type = SHT_PROGBITS; shdrs[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    shdrs[1].sh_offset = offset; shdrs[1].sh_size = c->text_section.length; shdrs[1].sh_addralign = 16;
    offset += shdrs[1].sh_size;

    shdrs[2].sh_name = 7; shdrs[2].sh_type = SHT_PROGBITS; shdrs[2].sh_flags = SHF_ALLOC;
    shdrs[2].sh_offset = offset; shdrs[2].sh_size = c->rodata_section.length; shdrs[2].sh_addralign = 8;
    offset += shdrs[2].sh_size;

    const char shstrtab[] = "\0.text\0.rodata\0.shstrtab\0.symtab\0.strtab";
    size_t shstrtab_len = sizeof(shstrtab);
    shdrs[3].sh_name = 15; shdrs[3].sh_type = SHT_STRTAB; shdrs[3].sh_offset = offset;
    shdrs[3].sh_size = shstrtab_len; shdrs[3].sh_addralign = 1;
    offset += shdrs[3].sh_size;

    Elf64_Sym syms[2];
    memset(syms, 0, sizeof(syms));
    syms[1].st_name = 1;
    syms[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    syms[1].st_shndx = 1;
    syms[1].st_size = c->text_section.length;

    const char strtab[] = "\0main";
    size_t strtab_len = sizeof(strtab);

    shdrs[4].sh_name = 25; shdrs[4].sh_type = SHT_SYMTAB; shdrs[4].sh_offset = offset;
    shdrs[4].sh_size = sizeof(syms); shdrs[4].sh_link = 5; shdrs[4].sh_info = 1;
    shdrs[4].sh_entsize = sizeof(Elf64_Sym); shdrs[4].sh_addralign = 8;
    offset += shdrs[4].sh_size;

    shdrs[5].sh_name = 33; shdrs[5].sh_type = SHT_STRTAB; shdrs[5].sh_offset = offset;
    shdrs[5].sh_size = strtab_len; shdrs[5].sh_addralign = 1;
    offset += shdrs[5].sh_size;

    ehdr.e_shoff = offset;

    fwrite(&ehdr, 1, sizeof(ehdr), f);
    fwrite(c->text_section.data, 1, c->text_section.length, f);
    if (c->rodata_section.length) fwrite(c->rodata_section.data, 1, c->rodata_section.length, f);
    fwrite(shstrtab, 1, shstrtab_len, f);
    fwrite(syms, 1, sizeof(syms), f);
    fwrite(strtab, 1, strtab_len, f);
    fwrite(shdrs, 1, sizeof(shdrs), f);

    fclose(f);
    return true;
}

static inline bool zero_cc_emit_elf_exe(ZeroCCCompiler *c, const char *filepath) {
    unlink(filepath);
    FILE *f = fopen(filepath, "wb");
    if (!f) return false;

    /* Patch internal function calls and string literal RIP offsets */
    for (size_t i = 0; i < c->reloc_count; i++) {
        if (c->relocs[i].is_call) {
            ZeroCCFunction *target_fn = zero_cc_find_function(c, c->relocs[i].target_symbol);
            if (target_fn && !target_fn->is_prototype) {
                int32_t rel = (int32_t)(target_fn->text_offset - (c->relocs[i].text_patch_offset + 4));
                memcpy(c->text_section.data + c->relocs[i].text_patch_offset, &rel, 4);
            }
        }
    }
    for (size_t i = 0; i < c->str_reloc_count; i++) {
        int32_t rel = (int32_t)((c->text_section.length + c->str_relocs[i].rodata_offset) - (c->str_relocs[i].text_patch_offset + 4));
        memcpy(c->text_section.data + c->str_relocs[i].text_patch_offset, &rel, 4);
    }

    uint64_t base_vaddr = 0x400000;
    size_t header_sz = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);

    ZeroCCFunction *main_fn = zero_cc_find_function(c, "main");
    int32_t main_rel = (int32_t)(main_fn ? main_fn->text_offset : 0);

    /* Freestanding Linux x86_64 CRT0: unpacks argc/argv, aligns stack, calls main(), exits cleanly */
    uint8_t start_code[] = {
        0x31, 0xED,                                 /* xor ebp, ebp (mark outermost frame) */
        0x5F,                                       /* pop rdi (argc into 1st arg) */
        0x48, 0x89, 0xE6,                           /* mov rsi, rsp (argv into 2nd arg) */
        0x48, 0x83, 0xE4, 0xF0,                     /* and rsp, -16 (16-byte stack alignment) */
        0xE8, 0x00, 0x00, 0x00, 0x00,               /* call main (offset patched below) */
        0x89, 0xC7,                                 /* mov edi, eax (exit code) */
        0xB8, 0x3C, 0x00, 0x00, 0x00,               /* mov eax, 60 (sys_exit) */
        0x0F, 0x05                                  /* syscall */
    };
    size_t start_len = sizeof(start_code);

    /* Patch call offset into crt0 start_code: relative offset from next instruction (start_code + 15) to main_fn */
    int32_t call_offset = (int32_t)(main_rel - (15 - start_len));
    memcpy(start_code + 11, &call_offset, 4);

    Elf64_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_ident[EI_OSABI] = ELFOSABI_SYSV;
    ehdr.e_type = ET_EXEC;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_entry = base_vaddr + header_sz;
    ehdr.e_phoff = sizeof(Elf64_Ehdr);
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum = 1;

    size_t total_payload = start_len + c->text_section.length + c->rodata_section.length;
    size_t file_sz = header_sz + total_payload;

    Elf64_Phdr phdr;
    memset(&phdr, 0, sizeof(phdr));
    phdr.p_type = PT_LOAD;
    phdr.p_flags = PF_R | PF_W | PF_X;
    phdr.p_offset = 0;
    phdr.p_vaddr = base_vaddr;
    phdr.p_paddr = base_vaddr;
    phdr.p_filesz = file_sz;
    phdr.p_memsz = file_sz;
    phdr.p_align = 0x1000;

    fwrite(&ehdr, 1, sizeof(ehdr), f);
    fwrite(&phdr, 1, sizeof(phdr), f);
    fwrite(start_code, 1, start_len, f);
    fwrite(c->text_section.data, 1, c->text_section.length, f);
    if (c->rodata_section.length) fwrite(c->rodata_section.data, 1, c->rodata_section.length, f);

    fclose(f);
    chmod(filepath, 0755);
    return true;
}

#else

static inline bool zero_cc_emit_elf_obj(ZeroCCCompiler *c, const char *filepath) {
    (void)c; (void)filepath;
    fprintf(stderr, "zero_cc: warning: ELF object generation is supported natively on Linux targets.\n");
    return false;
}

static inline bool zero_cc_emit_elf_exe(ZeroCCCompiler *c, const char *filepath) {
    (void)c; (void)filepath;
    fprintf(stderr, "zero_cc: warning: ELF executable generation is supported natively on Linux targets.\n");
    return false;
}

#endif /* __linux__ */

static inline bool zero_cc_emit_ape_exe(ZeroCCCompiler *c, const char *filepath) {
    return zero_cc_emit_elf_exe(c, filepath);
}


#endif /* ZERO_CC_JIT_ELF_EMITTER_H */
