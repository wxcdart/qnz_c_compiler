/**
 * @file macho_writer.c
 * @brief macOS Mach-O 64-bit Executable, Dynamic Library and Object Writer for ZeroCC (Sprint 251)
 */

#include "zero_cc/linker/macho_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zero_cc.h"
#include "zero_cc/linker/macho_reader.h"
#include "zero_cc/types/global_vars.h"

#define MACHO_PAGE_SIZE 0x1000U /* 4 KB standard page alignment */

static inline uint32_t macho_align_up(uint32_t val, uint32_t align) {
    if (align == 0) return val;
    return (val + align - 1) & ~(align - 1);
}

static inline uint64_t macho_align_up64(uint64_t val, uint64_t align) {
    if (align == 0) return val;
    return (val + align - 1) & ~(align - 1);
}

typedef struct {
    char name[144];
    uint32_t str_offset;
    uint8_t type;
    uint8_t sect;
    uint16_t desc;
    uint64_t value;
} MachOSymbol;

static bool macho_write_obj_core(const char *filename,
                                 const uint8_t *text, size_t text_size,
                                 const uint8_t *rodata, size_t rodata_size,
                                 const uint8_t *data, size_t data_size,
                                 size_t bss_size,
                                 uint32_t cputype,
                                 ZeroCCCompiler *comp)
{
    if (!filename) return false;

    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    uint32_t target_cpu = (cputype != 0) ? cputype : ZERO_CC_CPU_TYPE_X86_64;
    uint32_t target_sub = (target_cpu == ZERO_CC_CPU_TYPE_ARM64) ?
                          ZERO_CC_CPU_SUBTYPE_ARM64_ALL : ZERO_CC_CPU_SUBTYPE_X86_64_ALL;

    /* Build sections for MH_OBJECT:
     * - __text in __TEXT
     * - __cstring in __TEXT (if rodata)
     * - __data in __DATA (if data)
     * - __bss in __DATA (if bss)
     */
    uint32_t nsects = 1;
    uint8_t data_sect = 0;
    uint8_t bss_sect = 0;

    if (rodata_size > 0) {
        nsects++;
    }
    if (data_size > 0) {
        nsects++;
        data_sect = (uint8_t)nsects;
    }
    if (bss_size > 0) {
        nsects++;
        bss_sect = (uint8_t)nsects;
    }

    uint32_t seg_cmd_size = (uint32_t)(sizeof(ZeroCCSegmentCommand64) + nsects * sizeof(ZeroCCSection64));
    uint32_t symtab_cmd_size = (uint32_t)sizeof(ZeroCCSymtabCommand);
    uint32_t sizeofcmds = seg_cmd_size + symtab_cmd_size;

    ZeroCCMachHeader64 hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ZERO_CC_MH_MAGIC_64;
    hdr.cputype = target_cpu;
    hdr.cpusubtype = target_sub;
    hdr.filetype = ZERO_CC_MH_OBJECT;
    hdr.ncmds = 2;
    hdr.sizeofcmds = sizeofcmds;
    hdr.flags = ZERO_CC_MH_SUBSECTIONS_VIA_SYMBOLS;

    uint32_t cur_fileoff = (uint32_t)sizeof(hdr) + sizeofcmds;
    cur_fileoff = macho_align_up(cur_fileoff, 16);

    uint64_t cur_vmaddr = 0;

    ZeroCCSegmentCommand64 seg;
    memset(&seg, 0, sizeof(seg));
    seg.cmd = ZERO_CC_LC_SEGMENT_64;
    seg.cmdsize = seg_cmd_size;
    /* segname is empty in MH_OBJECT */
    seg.vmaddr = 0;
    seg.fileoff = cur_fileoff;
    seg.maxprot = ZERO_CC_VM_PROT_ALL;
    seg.initprot = ZERO_CC_VM_PROT_ALL;
    seg.nsects = nsects;

    ZeroCCSection64 *sects = (ZeroCCSection64 *)calloc(nsects, sizeof(ZeroCCSection64));
    if (!sects) {
        fclose(f);
        return false;
    }

    uint32_t sidx = 0;

    /* 1. __text */
    snprintf(sects[sidx].sectname, sizeof(sects[sidx].sectname), "__text");
    snprintf(sects[sidx].segname, sizeof(sects[sidx].segname), "__TEXT");
    sects[sidx].addr = cur_vmaddr;
    sects[sidx].size = text_size;
    sects[sidx].offset = cur_fileoff;
    sects[sidx].align = 4; /* 2^4 = 16 bytes */
    sects[sidx].flags = ZERO_CC_MACHO_S_REGULAR | ZERO_CC_MACHO_S_ATTR_SOME_INSTRUCTIONS | ZERO_CC_MACHO_S_ATTR_PURE_INSTRUCTIONS;

    cur_fileoff += (uint32_t)text_size;
    cur_vmaddr += (uint64_t)text_size;
    sidx++;

    /* 2. __cstring */
    if (rodata_size > 0) {
        cur_fileoff = macho_align_up(cur_fileoff, 8);
        cur_vmaddr = macho_align_up64(cur_vmaddr, 8);
        snprintf(sects[sidx].sectname, sizeof(sects[sidx].sectname), "__cstring");
        snprintf(sects[sidx].segname, sizeof(sects[sidx].segname), "__TEXT");
        sects[sidx].addr = cur_vmaddr;
        sects[sidx].size = rodata_size;
        sects[sidx].offset = cur_fileoff;
        sects[sidx].align = 0;
        sects[sidx].flags = ZERO_CC_MACHO_S_CSTRING_LITERALS;
        cur_fileoff += (uint32_t)rodata_size;
        cur_vmaddr += (uint64_t)rodata_size;
        sidx++;
    }

    /* 3. __data */
    if (data_size > 0) {
        cur_fileoff = macho_align_up(cur_fileoff, 8);
        cur_vmaddr = macho_align_up64(cur_vmaddr, 8);
        snprintf(sects[sidx].sectname, sizeof(sects[sidx].sectname), "__data");
        snprintf(sects[sidx].segname, sizeof(sects[sidx].segname), "__DATA");
        sects[sidx].addr = cur_vmaddr;
        sects[sidx].size = data_size;
        sects[sidx].offset = cur_fileoff;
        sects[sidx].align = 3;
        sects[sidx].flags = ZERO_CC_MACHO_S_REGULAR;
        cur_fileoff += (uint32_t)data_size;
        cur_vmaddr += (uint64_t)data_size;
        sidx++;
    }

    /* 4. __bss */
    if (bss_size > 0) {
        cur_vmaddr = macho_align_up64(cur_vmaddr, 8);
        snprintf(sects[sidx].sectname, sizeof(sects[sidx].sectname), "__bss");
        snprintf(sects[sidx].segname, sizeof(sects[sidx].segname), "__DATA");
        sects[sidx].addr = cur_vmaddr;
        sects[sidx].size = bss_size;
        sects[sidx].offset = 0;
        sects[sidx].align = 3;
        sects[sidx].flags = ZERO_CC_MACHO_S_ZEROFILL;
        cur_vmaddr += (uint64_t)bss_size;
        sidx++;
    }

    seg.vmsize = cur_vmaddr;
    seg.filesize = cur_fileoff - seg.fileoff;

    /* Build string table and symbol table */
    char strtab[16384];
    memset(strtab, 0, sizeof(strtab));
    uint32_t str_len = 1; /* strtab starts with '\0' */

    MachOSymbol syms[512];
    uint32_t nsyms = 0;

    if (comp) {
        for (size_t i = 0; i < comp->function_count && nsyms < 450; i++) {
            ZeroCCFunction *fn = &comp->functions[i];
            if (!fn->is_prototype && fn->name[0] != '\0') {
                snprintf(syms[nsyms].name, sizeof(syms[nsyms].name), "_%s", fn->name);
                syms[nsyms].type = ZERO_CC_N_SECT | ZERO_CC_N_EXT;
                syms[nsyms].sect = 1; /* __text */
                syms[nsyms].desc = 0;
                syms[nsyms].value = fn->text_offset;
                syms[nsyms].str_offset = str_len;
                size_t slen = strlen(syms[nsyms].name) + 1;
                if (str_len + slen < sizeof(strtab)) {
                    memcpy(strtab + str_len, syms[nsyms].name, slen);
                    str_len += (uint32_t)slen;
                    nsyms++;
                }
            }
        }
        for (size_t i = 0; i < comp->global_var_count && nsyms < 450; i++) {
            ZeroCCGlobalVar *gv = &comp->global_vars[i];
            if (gv->name[0] != '\0') {
                uint8_t sect = 1;
                if (gv->is_bss && bss_sect > 0) {
                    sect = bss_sect;
                } else if (data_sect > 0) {
                    sect = data_sect;
                }
                snprintf(syms[nsyms].name, sizeof(syms[nsyms].name), "_%s", gv->name);
                syms[nsyms].type = ZERO_CC_N_SECT | ZERO_CC_N_EXT;
                syms[nsyms].sect = sect;
                syms[nsyms].desc = 0;
                syms[nsyms].value = gv->data_offset;
                syms[nsyms].str_offset = str_len;
                size_t slen = strlen(syms[nsyms].name) + 1;
                if (str_len + slen < sizeof(strtab)) {
                    memcpy(strtab + str_len, syms[nsyms].name, slen);
                    str_len += (uint32_t)slen;
                    nsyms++;
                }
            }
        }
        /* Add undefined symbols for unresolved relocations */
        for (size_t i = 0; i < comp->reloc_count && nsyms < 450; i++) {
            ZeroCCReloc *r = &comp->relocs[i];
            if (r->target_symbol[0] != '\0') {
                char mangled[144];
                snprintf(mangled, sizeof(mangled), "_%s", r->target_symbol);
                bool found = false;
                for (uint32_t s = 0; s < nsyms; s++) {
                    if (strcmp(syms[s].name, mangled) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    snprintf(syms[nsyms].name, sizeof(syms[nsyms].name), "%s", mangled);
                    syms[nsyms].type = ZERO_CC_N_UNDF | ZERO_CC_N_EXT;
                    syms[nsyms].sect = 0;
                    syms[nsyms].desc = 0;
                    syms[nsyms].value = 0;
                    syms[nsyms].str_offset = str_len;
                    size_t slen = strlen(syms[nsyms].name) + 1;
                    if (str_len + slen < sizeof(strtab)) {
                        memcpy(strtab + str_len, syms[nsyms].name, slen);
                        str_len += (uint32_t)slen;
                        nsyms++;
                    }
                }
            }
        }
    } else {
        /* Exported _main or default entry */
        snprintf(syms[nsyms].name, sizeof(syms[nsyms].name), "_main");
        syms[nsyms].type = ZERO_CC_N_SECT | ZERO_CC_N_EXT;
        syms[nsyms].sect = 1;
        syms[nsyms].desc = 0;
        syms[nsyms].value = 0;
        syms[nsyms].str_offset = str_len;
        size_t slen = strlen(syms[nsyms].name) + 1;
        memcpy(strtab + str_len, syms[nsyms].name, slen);
        str_len += (uint32_t)slen;
        nsyms++;
    }

    /* Build text section relocations */
    ZeroCCMachORelocation *text_relocs = NULL;
    uint32_t text_nreloc = 0;
    size_t total_relocs = (comp ? comp->reloc_count + comp->str_reloc_count : 0);
    if (comp && total_relocs > 0) {
        text_relocs = (ZeroCCMachORelocation *)calloc(total_relocs + 1, sizeof(ZeroCCMachORelocation));
        if (text_relocs) {
            for (size_t i = 0; i < comp->reloc_count; i++) {
                ZeroCCReloc *r = &comp->relocs[i];
                char mangled[144];
                snprintf(mangled, sizeof(mangled), "_%s", r->target_symbol);
                int sym_idx = -1;
                for (uint32_t s = 0; s < nsyms; s++) {
                    if (strcmp(syms[s].name, mangled) == 0) {
                        sym_idx = (int)s;
                        break;
                    }
                }
                if (sym_idx >= 0) {
                    text_relocs[text_nreloc].r_address = (int32_t)r->text_patch_offset;
                    uint32_t symnum = (uint32_t)sym_idx;
                    uint32_t pcrel = 1;
                    uint32_t len = 2; /* 4 bytes */
                    uint32_t ext = 1;
                    uint32_t type = (target_cpu == ZERO_CC_CPU_TYPE_ARM64) ? 2 : ZERO_CC_X86_64_RELOC_BRANCH;
                    if (!r->is_call) {
                        type = (target_cpu == ZERO_CC_CPU_TYPE_ARM64) ? 3 : ZERO_CC_X86_64_RELOC_SIGNED;
                    }
                    text_relocs[text_nreloc].r_info = (symnum & 0x00FFFFFFU) | (pcrel << 24) |
                                                      (len << 25) | (ext << 27) | (type << 28);
                    text_nreloc++;
                }
            }

            if (rodata_size > 0 && text) {
                uint8_t *mutable_text = (uint8_t *)text;
                for (size_t i = 0; i < comp->str_reloc_count; i++) {
                    ZeroCCStringReloc *sr = &comp->str_relocs[i];
                    if (sr->text_patch_offset + 4 <= text_size) {
                        int32_t off = (int32_t)sr->rodata_offset;
                        memcpy(mutable_text + sr->text_patch_offset, &off, 4);
                    }
                    text_relocs[text_nreloc].r_address = (int32_t)sr->text_patch_offset;
                    uint32_t symnum = 2; /* 1-based index of __cstring */
                    uint32_t pcrel = 1;
                    uint32_t len = 2; /* 4 bytes */
                    uint32_t ext = 0; /* local section-relative */
                    uint32_t type = (target_cpu == ZERO_CC_CPU_TYPE_ARM64) ? 3 : ZERO_CC_X86_64_RELOC_SIGNED;
                    text_relocs[text_nreloc].r_info = (symnum & 0x00FFFFFFU) | (pcrel << 24) |
                                                      (len << 25) | (ext << 27) | (type << 28);
                    text_nreloc++;
                }
            }
        }
    }

    uint32_t reloff = 0;
    if (text_nreloc > 0) {
        reloff = macho_align_up(cur_fileoff, 8);
        sects[0].reloff = reloff;
        sects[0].nreloc = text_nreloc;
        cur_fileoff = reloff + (uint32_t)(text_nreloc * sizeof(ZeroCCMachORelocation));
    }

    uint32_t symoff = macho_align_up(cur_fileoff, 8);
    uint32_t stroff = (uint32_t)(symoff + nsyms * sizeof(ZeroCCNlist64));

    ZeroCCSymtabCommand symcmd;
    memset(&symcmd, 0, sizeof(symcmd));
    symcmd.cmd = ZERO_CC_LC_SYMTAB;
    symcmd.cmdsize = symtab_cmd_size;
    symcmd.symoff = symoff;
    symcmd.nsyms = nsyms;
    symcmd.stroff = stroff;
    symcmd.strsize = str_len;

    /* Write headers */
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(&seg, sizeof(seg), 1, f);
    fwrite(sects, sizeof(ZeroCCSection64), nsects, f);
    fwrite(&symcmd, sizeof(symcmd), 1, f);

    /* Pad to first section offset */
    long cur_pos = ftell(f);
    if ((uint32_t)cur_pos < seg.fileoff) {
        size_t pad = seg.fileoff - (uint32_t)cur_pos;
        uint8_t zero = 0;
        for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    }

    /* Write section data */
    if (text && text_size > 0) fwrite(text, 1, text_size, f);
    if (rodata && rodata_size > 0) {
        cur_pos = ftell(f);
        if ((uint32_t)cur_pos < sects[1].offset) {
            size_t pad = sects[1].offset - (uint32_t)cur_pos;
            uint8_t zero = 0;
            for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
        }
        fwrite(rodata, 1, rodata_size, f);
    }
    if (data && data_size > 0) {
        uint32_t d_off = sects[rodata_size > 0 ? 2 : 1].offset;
        cur_pos = ftell(f);
        if ((uint32_t)cur_pos < d_off) {
            size_t pad = d_off - (uint32_t)cur_pos;
            uint8_t zero = 0;
            for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
        }
        fwrite(data, 1, data_size, f);
    }

    /* Write relocations */
    if (text_nreloc > 0 && text_relocs) {
        cur_pos = ftell(f);
        if ((uint32_t)cur_pos < reloff) {
            size_t pad = reloff - (uint32_t)cur_pos;
            uint8_t zero = 0;
            for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
        }
        fwrite(text_relocs, sizeof(ZeroCCMachORelocation), text_nreloc, f);
    }
    if (text_relocs) free(text_relocs);

    /* Pad to symoff */
    cur_pos = ftell(f);
    if ((uint32_t)cur_pos < symoff) {
        size_t pad = symoff - (uint32_t)cur_pos;
        uint8_t zero = 0;
        for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    }

    /* Write symbol table */
    for (uint32_t i = 0; i < nsyms; i++) {
        ZeroCCNlist64 nl;
        memset(&nl, 0, sizeof(nl));
        nl.n_strx = syms[i].str_offset;
        nl.n_type = syms[i].type;
        nl.n_sect = syms[i].sect;
        nl.n_desc = syms[i].desc;
        nl.n_value = syms[i].value;
        fwrite(&nl, sizeof(nl), 1, f);
    }

    /* Write string table */
    fwrite(strtab, 1, str_len, f);

    free(sects);
    fclose(f);
    return true;
}

bool zero_cc_macho_write_obj_arch(const char *filename,
                                  const uint8_t *text, size_t text_size,
                                  const uint8_t *rodata, size_t rodata_size,
                                  const uint8_t *data, size_t data_size,
                                  size_t bss_size,
                                  uint32_t cputype)
{
    return macho_write_obj_core(filename, text, text_size, rodata, rodata_size,
                                data, data_size, bss_size, cputype, NULL);
}

bool zero_cc_macho_write_obj(const char *filename,
                             const uint8_t *text, size_t text_size,
                             const uint8_t *rodata, size_t rodata_size,
                             const uint8_t *data, size_t data_size,
                             size_t bss_size)
{
    return zero_cc_macho_write_obj_arch(filename, text, text_size,
                                        rodata, rodata_size,
                                        data, data_size,
                                        bss_size, ZERO_CC_CPU_TYPE_X86_64);
}

bool zero_cc_macho_write_exe_arch_ext(const char *filename,
                                      const uint8_t *text, size_t text_size,
                                      const uint8_t *rodata, size_t rodata_size,
                                      const uint8_t *data, size_t data_size,
                                      size_t bss_size,
                                      uint32_t entry_offset,
                                      uint32_t cputype,
                                      const char **import_names, uint32_t import_count,
                                      const char **extra_dylibs, uint32_t extra_dylib_count)
{
    uint32_t target_cpu = (cputype != 0) ? cputype : ZERO_CC_CPU_TYPE_X86_64;
    uint32_t target_sub = (target_cpu == ZERO_CC_CPU_TYPE_ARM64) ?
                          ZERO_CC_CPU_SUBTYPE_ARM64_ALL : ZERO_CC_CPU_SUBTYPE_X86_64_ALL;

    /* Build executable container */
    if (!filename) return false;

    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    uint32_t page_size = MACHO_PAGE_SIZE;
    uint32_t stub_inst_size = (target_cpu == ZERO_CC_CPU_TYPE_ARM64) ? 12 : 6;
    uint32_t stubs_size = (import_count > 0) ? (import_count * stub_inst_size) : 0;
    uint32_t stubs_size_aligned = macho_align_up(stubs_size, 16);
    uint32_t la_size = (import_count > 0) ? (import_count * 8) : 0;

    /* Calculate load commands size:
     * 1. LC_SEGMENT_64 (__PAGEZERO) : 72 bytes
     * 2. LC_SEGMENT_64 (__TEXT)     : 72 + text_nsects * 80
     * 3. LC_SEGMENT_64 (__DATA)     : 72 + data_nsects * 80 (if any)
     * 4. LC_SEGMENT_64 (__LINKEDIT) : 72 bytes
     * 5. LC_LOAD_DYLINKER           : 32 bytes ("/usr/lib/dyld\0\0\0")
     * 6. LC_LOAD_DYLIB              : 56 bytes ("/usr/lib/libSystem.B.dylib\0...")
     *    + extra LC_LOAD_DYLIB commands for any specified dylibs
     * 7. LC_MAIN                    : 24 bytes
     * 8. LC_SYMTAB                  : 24 bytes
     * 9. LC_DYSYMTAB                : 80 bytes
     */
    uint32_t text_nsects = 1 + (stubs_size > 0 ? 1 : 0) + (rodata_size > 0 ? 1 : 0);
    uint32_t data_nsects = (la_size > 0 ? 1 : 0) + (data_size > 0 ? 1 : 0) + (bss_size > 0 ? 1 : 0);
    bool has_data_seg = (data_nsects > 0);

    uint32_t cmd_pagezero_size = sizeof(ZeroCCSegmentCommand64);
    uint32_t cmd_text_size = (uint32_t)(sizeof(ZeroCCSegmentCommand64) + text_nsects * sizeof(ZeroCCSection64));
    uint32_t cmd_data_size = has_data_seg ? (uint32_t)(sizeof(ZeroCCSegmentCommand64) + data_nsects * sizeof(ZeroCCSection64)) : 0;
    uint32_t cmd_linkedit_size = sizeof(ZeroCCSegmentCommand64);
    uint32_t cmd_dylinker_size = 32;
    uint32_t cmd_dylib_size = 56;
    uint32_t cmd_main_size = sizeof(ZeroCCEntryPointCommand);
    uint32_t cmd_symtab_size = sizeof(ZeroCCSymtabCommand);
    uint32_t cmd_dysymtab_size = sizeof(ZeroCCDysymtabCommand);

    uint32_t total_extra_dylib_size = 0;
    if (extra_dylibs && extra_dylib_count > 0) {
        for (uint32_t k = 0; k < extra_dylib_count; k++) {
            uint32_t plen = (uint32_t)strlen(extra_dylibs[k]) + 1;
            total_extra_dylib_size += macho_align_up(24 + plen, 8);
        }
    }

    uint32_t ncmds = 8 + (has_data_seg ? 1 : 0) + (extra_dylibs ? extra_dylib_count : 0);
    uint32_t sizeofcmds = cmd_pagezero_size + cmd_text_size + cmd_data_size +
                          cmd_linkedit_size + cmd_dylinker_size + cmd_dylib_size +
                          total_extra_dylib_size +
                          cmd_main_size + cmd_symtab_size + cmd_dysymtab_size;

    ZeroCCMachHeader64 hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ZERO_CC_MH_MAGIC_64;
    hdr.cputype = target_cpu;
    hdr.cpusubtype = target_sub;
    hdr.filetype = ZERO_CC_MH_EXECUTE;
    hdr.ncmds = ncmds;
    hdr.sizeofcmds = sizeofcmds;
    hdr.flags = ZERO_CC_MH_NOUNDEFS | ZERO_CC_MH_DYLDLINK | ZERO_CC_MH_TWOLEVEL | ZERO_CC_MH_PIE;

    /* Offsets & VM Layout */
    uint64_t text_vmaddr = 0x100000000ULL;
    uint32_t header_and_cmds_size = (uint32_t)sizeof(hdr) + sizeofcmds;
    uint32_t text_sec_offset = macho_align_up(header_and_cmds_size, 16);

    ZeroCCSection64 text_sects[3];
    memset(text_sects, 0, sizeof(text_sects));
    uint32_t cur_ts_idx = 0;
    uint32_t cur_text_fileoff = text_sec_offset;

    /* __text section */
    snprintf(text_sects[cur_ts_idx].sectname, sizeof(text_sects[cur_ts_idx].sectname), "__text");
    snprintf(text_sects[cur_ts_idx].segname, sizeof(text_sects[cur_ts_idx].segname), "__TEXT");
    text_sects[cur_ts_idx].addr = text_vmaddr + cur_text_fileoff;
    text_sects[cur_ts_idx].size = text_size;
    text_sects[cur_ts_idx].offset = cur_text_fileoff;
    text_sects[cur_ts_idx].align = 4;
    text_sects[cur_ts_idx].flags = ZERO_CC_MACHO_S_REGULAR | ZERO_CC_MACHO_S_ATTR_SOME_INSTRUCTIONS | ZERO_CC_MACHO_S_ATTR_PURE_INSTRUCTIONS;
    cur_ts_idx++;
    cur_text_fileoff = macho_align_up(cur_text_fileoff + (uint32_t)text_size, 16);

    /* __stubs section */
    uint32_t stubs_sec_offset = 0;
    uint64_t stubs_vmaddr = 0;
    if (stubs_size > 0) {
        stubs_sec_offset = cur_text_fileoff;
        stubs_vmaddr = text_vmaddr + stubs_sec_offset;
        snprintf(text_sects[cur_ts_idx].sectname, sizeof(text_sects[cur_ts_idx].sectname), "__stubs");
        snprintf(text_sects[cur_ts_idx].segname, sizeof(text_sects[cur_ts_idx].segname), "__TEXT");
        text_sects[cur_ts_idx].addr = stubs_vmaddr;
        text_sects[cur_ts_idx].size = stubs_size;
        text_sects[cur_ts_idx].offset = stubs_sec_offset;
        text_sects[cur_ts_idx].align = (target_cpu == ZERO_CC_CPU_TYPE_ARM64 ? 2 : 1);
        text_sects[cur_ts_idx].flags = ZERO_CC_MACHO_S_SYMBOL_STUBS | ZERO_CC_MACHO_S_ATTR_SOME_INSTRUCTIONS | ZERO_CC_MACHO_S_ATTR_PURE_INSTRUCTIONS;
        text_sects[cur_ts_idx].reserved1 = 0; /* start index in indirect symbol table */
        text_sects[cur_ts_idx].reserved2 = stub_inst_size;
        cur_ts_idx++;
        cur_text_fileoff = macho_align_up(cur_text_fileoff + stubs_size, 16);
    }

    /* __cstring section */
    uint32_t rodata_sec_offset = 0;
    if (rodata_size > 0) {
        rodata_sec_offset = cur_text_fileoff;
        snprintf(text_sects[cur_ts_idx].sectname, sizeof(text_sects[cur_ts_idx].sectname), "__cstring");
        snprintf(text_sects[cur_ts_idx].segname, sizeof(text_sects[cur_ts_idx].segname), "__TEXT");
        text_sects[cur_ts_idx].addr = text_vmaddr + rodata_sec_offset;
        text_sects[cur_ts_idx].size = rodata_size;
        text_sects[cur_ts_idx].offset = rodata_sec_offset;
        text_sects[cur_ts_idx].align = 0;
        text_sects[cur_ts_idx].flags = ZERO_CC_MACHO_S_CSTRING_LITERALS;
        cur_ts_idx++;
        cur_text_fileoff += (uint32_t)rodata_size;
    }

    uint32_t text_total_raw = cur_text_fileoff;
    uint32_t text_filesize = macho_align_up(text_total_raw, page_size);
    uint64_t text_vmsize = text_filesize;

    /* __DATA layout */
    uint64_t data_vmaddr = text_vmaddr + text_vmsize;
    uint32_t data_fileoff = text_filesize;
    uint32_t cur_data_fileoff = data_fileoff;
    uint64_t cur_data_vmaddr = data_vmaddr;

    ZeroCCSection64 data_sects[3];
    memset(data_sects, 0, sizeof(data_sects));
    uint32_t cur_ds_idx = 0;

    /* __la_symbol_ptr section */
    uint64_t la_vmaddr = 0;
    uint32_t la_fileoff = 0;
    if (la_size > 0) {
        la_vmaddr = cur_data_vmaddr;
        la_fileoff = cur_data_fileoff;
        snprintf(data_sects[cur_ds_idx].sectname, sizeof(data_sects[cur_ds_idx].sectname), "__la_symbol_ptr");
        snprintf(data_sects[cur_ds_idx].segname, sizeof(data_sects[cur_ds_idx].segname), "__DATA");
        data_sects[cur_ds_idx].addr = la_vmaddr;
        data_sects[cur_ds_idx].size = la_size;
        data_sects[cur_ds_idx].offset = la_fileoff;
        data_sects[cur_ds_idx].align = 3; /* 8-byte alignment */
        data_sects[cur_ds_idx].flags = ZERO_CC_MACHO_S_LAZY_SYMBOL_POINTERS;
        data_sects[cur_ds_idx].reserved1 = 0; /* start index in indirect symbol table */
        cur_ds_idx++;
        cur_data_fileoff = macho_align_up(cur_data_fileoff + la_size, 16);
        cur_data_vmaddr = macho_align_up64(cur_data_vmaddr + la_size, 16);
    }

    /* __data section */
    uint32_t data_sec_offset = 0;
    if (data_size > 0) {
        data_sec_offset = cur_data_fileoff;
        snprintf(data_sects[cur_ds_idx].sectname, sizeof(data_sects[cur_ds_idx].sectname), "__data");
        snprintf(data_sects[cur_ds_idx].segname, sizeof(data_sects[cur_ds_idx].segname), "__DATA");
        data_sects[cur_ds_idx].addr = cur_data_vmaddr;
        data_sects[cur_ds_idx].size = data_size;
        data_sects[cur_ds_idx].offset = data_sec_offset;
        data_sects[cur_ds_idx].align = 3;
        data_sects[cur_ds_idx].flags = ZERO_CC_MACHO_S_REGULAR;
        cur_ds_idx++;
        cur_data_fileoff += (uint32_t)data_size;
        cur_data_vmaddr += data_size;
    }

    /* __bss section */
    if (bss_size > 0) {
        snprintf(data_sects[cur_ds_idx].sectname, sizeof(data_sects[cur_ds_idx].sectname), "__bss");
        snprintf(data_sects[cur_ds_idx].segname, sizeof(data_sects[cur_ds_idx].segname), "__DATA");
        data_sects[cur_ds_idx].addr = cur_data_vmaddr;
        data_sects[cur_ds_idx].size = bss_size;
        data_sects[cur_ds_idx].offset = 0;
        data_sects[cur_ds_idx].align = 3;
        data_sects[cur_ds_idx].flags = ZERO_CC_MACHO_S_ZEROFILL;
        cur_ds_idx++;
        cur_data_vmaddr += bss_size;
    }

    uint32_t data_filesize = has_data_seg ? macho_align_up(cur_data_fileoff - data_fileoff, page_size) : 0;
    uint64_t data_vmsize = has_data_seg ? macho_align_up64(cur_data_vmaddr - data_vmaddr, page_size) : 0;

    /* __LINKEDIT layout */
    uint64_t linkedit_vmaddr = data_vmaddr + (has_data_seg ? data_vmsize : 0);
    uint32_t linkedit_fileoff = data_fileoff + data_filesize;

    /* 1. __PAGEZERO Command */
    ZeroCCSegmentCommand64 cmd_pagezero;
    memset(&cmd_pagezero, 0, sizeof(cmd_pagezero));
    cmd_pagezero.cmd = ZERO_CC_LC_SEGMENT_64;
    cmd_pagezero.cmdsize = cmd_pagezero_size;
    snprintf(cmd_pagezero.segname, sizeof(cmd_pagezero.segname), "__PAGEZERO");
    cmd_pagezero.vmaddr = 0;
    cmd_pagezero.vmsize = 0x100000000ULL;
    cmd_pagezero.fileoff = 0;
    cmd_pagezero.filesize = 0;
    cmd_pagezero.maxprot = ZERO_CC_VM_PROT_NONE;
    cmd_pagezero.initprot = ZERO_CC_VM_PROT_NONE;
    cmd_pagezero.nsects = 0;

    /* 2. __TEXT Command */
    ZeroCCSegmentCommand64 cmd_text;
    memset(&cmd_text, 0, sizeof(cmd_text));
    cmd_text.cmd = ZERO_CC_LC_SEGMENT_64;
    cmd_text.cmdsize = cmd_text_size;
    snprintf(cmd_text.segname, sizeof(cmd_text.segname), "__TEXT");
    cmd_text.vmaddr = text_vmaddr;
    cmd_text.vmsize = text_vmsize;
    cmd_text.fileoff = 0;
    cmd_text.filesize = text_filesize;
    cmd_text.maxprot = ZERO_CC_VM_PROT_ALL;
    cmd_text.initprot = ZERO_CC_VM_PROT_READ | ZERO_CC_VM_PROT_EXECUTE;
    cmd_text.nsects = text_nsects;

    /* 3. __DATA Command */
    ZeroCCSegmentCommand64 cmd_data;
    memset(&cmd_data, 0, sizeof(cmd_data));
    if (has_data_seg) {
        cmd_data.cmd = ZERO_CC_LC_SEGMENT_64;
        cmd_data.cmdsize = cmd_data_size;
        snprintf(cmd_data.segname, sizeof(cmd_data.segname), "__DATA");
        cmd_data.vmaddr = data_vmaddr;
        cmd_data.vmsize = data_vmsize;
        cmd_data.fileoff = data_fileoff;
        cmd_data.filesize = data_filesize;
        cmd_data.maxprot = ZERO_CC_VM_PROT_ALL;
        cmd_data.initprot = ZERO_CC_VM_PROT_READ | ZERO_CC_VM_PROT_WRITE;
        cmd_data.nsects = data_nsects;
    }

    /* 5. LC_LOAD_DYLINKER */
    uint8_t dylinker_buf[32];
    memset(dylinker_buf, 0, sizeof(dylinker_buf));
    ZeroCCDylinkerCommand *cmd_dylinker = (ZeroCCDylinkerCommand *)dylinker_buf;
    cmd_dylinker->cmd = ZERO_CC_LC_LOAD_DYLINKER;
    cmd_dylinker->cmdsize = cmd_dylinker_size;
    cmd_dylinker->dylinker_name_offset = 12;
    memcpy(dylinker_buf + 12, "/usr/lib/dyld", 14);

    /* 6. LC_LOAD_DYLIB (libSystem.B.dylib) */
    uint8_t dylib_buf[56];
    memset(dylib_buf, 0, sizeof(dylib_buf));
    ZeroCCDylibCommand *cmd_dylib = (ZeroCCDylibCommand *)dylib_buf;
    cmd_dylib->cmd = ZERO_CC_LC_LOAD_DYLIB;
    cmd_dylib->cmdsize = cmd_dylib_size;
    cmd_dylib->dylib_name_offset = 24;
    cmd_dylib->dylib_timestamp = 2;
    cmd_dylib->dylib_current_version = 0x05000000;
    cmd_dylib->dylib_compatibility_version = 0x01000000;
    memcpy(dylib_buf + 24, "/usr/lib/libSystem.B.dylib", 27);

    /* 7. LC_MAIN */
    ZeroCCEntryPointCommand cmd_main;
    memset(&cmd_main, 0, sizeof(cmd_main));
    cmd_main.cmd = ZERO_CC_LC_MAIN;
    cmd_main.cmdsize = cmd_main_size;
    cmd_main.entryoff = text_sec_offset + entry_offset;
    cmd_main.stacksize = 0;

    /* 8. Symbol Table & String Table */
    char strtab[8192];
    memset(strtab, 0, sizeof(strtab));
    uint32_t str_len = 1;

    MachOSymbol syms[256];
    uint32_t nsyms = 0;

    /* Defined symbol: _main */
    snprintf(syms[nsyms].name, sizeof(syms[nsyms].name), "_main");
    syms[nsyms].type = ZERO_CC_N_SECT | ZERO_CC_N_EXT;
    syms[nsyms].sect = 1;
    syms[nsyms].desc = 0;
    syms[nsyms].value = text_vmaddr + text_sec_offset + entry_offset;
    syms[nsyms].str_offset = str_len;
    size_t slen = strlen(syms[nsyms].name) + 1;
    memcpy(strtab + str_len, syms[nsyms].name, slen);
    str_len += (uint32_t)slen;
    nsyms++;

    /* Undefined imported symbols */
    uint32_t indirect_symbols[256];
    for (uint32_t i = 0; i < import_count && nsyms < 250; i++) {
        char mangled[144];
        if (import_names[i][0] == '_') {
            snprintf(mangled, sizeof(mangled), "%s", import_names[i]);
        } else {
            snprintf(mangled, sizeof(mangled), "_%s", import_names[i]);
        }
        snprintf(syms[nsyms].name, sizeof(syms[nsyms].name), "%s", mangled);
        syms[nsyms].type = ZERO_CC_N_UNDF | ZERO_CC_N_EXT;
        syms[nsyms].sect = 0;
        syms[nsyms].desc = 0x0100;
        syms[nsyms].value = 0;
        syms[nsyms].str_offset = str_len;
        slen = strlen(syms[nsyms].name) + 1;
        if (str_len + slen < sizeof(strtab)) {
            memcpy(strtab + str_len, syms[nsyms].name, slen);
            str_len += (uint32_t)slen;
        }
        indirect_symbols[i] = nsyms;
        nsyms++;
    }

    uint32_t symoff = linkedit_fileoff;
    uint32_t stroff = (uint32_t)(symoff + nsyms * sizeof(ZeroCCNlist64));
    uint32_t indirectsymoff = macho_align_up(stroff + str_len, 4);
    uint32_t indirect_bytes = (uint32_t)(import_count * sizeof(uint32_t));

    uint32_t linkedit_needed = (indirectsymoff - linkedit_fileoff) + indirect_bytes;
    uint32_t linkedit_filesize = macho_align_up(linkedit_needed, page_size);
    uint64_t linkedit_vmsize = linkedit_filesize;

    /* 4. __LINKEDIT Command */
    ZeroCCSegmentCommand64 cmd_linkedit;
    memset(&cmd_linkedit, 0, sizeof(cmd_linkedit));
    cmd_linkedit.cmd = ZERO_CC_LC_SEGMENT_64;
    cmd_linkedit.cmdsize = cmd_linkedit_size;
    snprintf(cmd_linkedit.segname, sizeof(cmd_linkedit.segname), "__LINKEDIT");
    cmd_linkedit.vmaddr = linkedit_vmaddr;
    cmd_linkedit.vmsize = linkedit_vmsize;
    cmd_linkedit.fileoff = linkedit_fileoff;
    cmd_linkedit.filesize = linkedit_filesize;
    cmd_linkedit.maxprot = ZERO_CC_VM_PROT_ALL;
    cmd_linkedit.initprot = ZERO_CC_VM_PROT_READ;
    cmd_linkedit.nsects = 0;

    ZeroCCSymtabCommand cmd_symtab;
    memset(&cmd_symtab, 0, sizeof(cmd_symtab));
    cmd_symtab.cmd = ZERO_CC_LC_SYMTAB;
    cmd_symtab.cmdsize = cmd_symtab_size;
    cmd_symtab.symoff = symoff;
    cmd_symtab.nsyms = nsyms;
    cmd_symtab.stroff = stroff;
    cmd_symtab.strsize = str_len;

    ZeroCCDysymtabCommand cmd_dysymtab;
    memset(&cmd_dysymtab, 0, sizeof(cmd_dysymtab));
    cmd_dysymtab.cmd = ZERO_CC_LC_DYSYMTAB;
    cmd_dysymtab.cmdsize = cmd_dysymtab_size;
    cmd_dysymtab.iextdefsym = 0;
    cmd_dysymtab.nextdefsym = 1; /* _main */
    cmd_dysymtab.iundefsym = 1;
    cmd_dysymtab.nundefsym = import_count;
    cmd_dysymtab.indirectsymoff = (import_count > 0 ? indirectsymoff : 0);
    cmd_dysymtab.nindirectsyms = import_count;

    /* Build stubs machine code */
    uint8_t *stubs_buf = NULL;
    if (stubs_size > 0) {
        stubs_buf = (uint8_t *)calloc(1, stubs_size_aligned);
        if (stubs_buf) {
            for (uint32_t i = 0; i < import_count; i++) {
                if (target_cpu == ZERO_CC_CPU_TYPE_ARM64) {
                    uint64_t stub_addr = stubs_vmaddr + i * 12;
                    uint64_t slot_addr = la_vmaddr + i * 8;
                    int64_t p_cur = (int64_t)(stub_addr >> 12);
                    int64_t p_tgt = (int64_t)(slot_addr >> 12);
                    int32_t p_delta = (int32_t)(p_tgt - p_cur);
                    uint32_t immlo = (uint32_t)(p_delta & 3) << 29;
                    uint32_t immhi = (uint32_t)((p_delta >> 2) & 0x7FFFF) << 5;
                    uint32_t adrp_inst = 0x90000010U | immlo | immhi;
                    uint32_t poff = (uint32_t)(slot_addr & 0xFFF);
                    uint32_t ldr_inst = 0xF9400010U | ((poff >> 3) << 10);
                    uint32_t br_inst = 0xD61F0200U;
                    memcpy(stubs_buf + i * 12 + 0, &adrp_inst, 4);
                    memcpy(stubs_buf + i * 12 + 4, &ldr_inst, 4);
                    memcpy(stubs_buf + i * 12 + 8, &br_inst, 4);
                } else {
                    /* x86-64 */
                    uint64_t stub_addr = stubs_vmaddr + i * 6;
                    uint64_t slot_addr = la_vmaddr + i * 8;
                    int32_t disp = (int32_t)(slot_addr - (stub_addr + 6));
                    stubs_buf[i * 6 + 0] = 0xFF;
                    stubs_buf[i * 6 + 1] = 0x25;
                    memcpy(stubs_buf + i * 6 + 2, &disp, 4);
                }
            }
        }
    }

    /* Write headers & commands */
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(&cmd_pagezero, sizeof(cmd_pagezero), 1, f);
    fwrite(&cmd_text, sizeof(cmd_text), 1, f);
    fwrite(text_sects, sizeof(ZeroCCSection64), text_nsects, f);
    if (has_data_seg) {
        fwrite(&cmd_data, sizeof(cmd_data), 1, f);
        fwrite(data_sects, sizeof(ZeroCCSection64), data_nsects, f);
    }
    fwrite(&cmd_linkedit, sizeof(cmd_linkedit), 1, f);
    fwrite(dylinker_buf, 1, sizeof(dylinker_buf), f);
    fwrite(dylib_buf, 1, sizeof(dylib_buf), f);

    /* Write extra dylibs */
    if (extra_dylibs && extra_dylib_count > 0) {
        for (uint32_t k = 0; k < extra_dylib_count; k++) {
            uint32_t plen = (uint32_t)strlen(extra_dylibs[k]) + 1;
            uint32_t csz = macho_align_up(24 + plen, 8);
            uint8_t *ebuf = (uint8_t *)calloc(1, csz);
            if (ebuf) {
                ZeroCCDylibCommand *dcmd = (ZeroCCDylibCommand *)ebuf;
                dcmd->cmd = ZERO_CC_LC_LOAD_DYLIB;
                dcmd->cmdsize = csz;
                dcmd->dylib_name_offset = 24;
                dcmd->dylib_timestamp = 2;
                dcmd->dylib_current_version = 0x01000000;
                dcmd->dylib_compatibility_version = 0x01000000;
                memcpy(ebuf + 24, extra_dylibs[k], plen);
                fwrite(ebuf, 1, csz, f);
                free(ebuf);
            }
        }
    }

    fwrite(&cmd_main, sizeof(cmd_main), 1, f);
    fwrite(&cmd_symtab, sizeof(cmd_symtab), 1, f);
    fwrite(&cmd_dysymtab, sizeof(cmd_dysymtab), 1, f);

    /* Pad to text_sec_offset */
    long cur_pos = ftell(f);
    if ((uint32_t)cur_pos < text_sec_offset) {
        size_t pad = text_sec_offset - (uint32_t)cur_pos;
        uint8_t zero = 0;
        for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    }

    /* Write .text */
    if (text && text_size > 0) fwrite(text, 1, text_size, f);

    /* Write .stubs */
    if (stubs_buf && stubs_size > 0) {
        cur_pos = ftell(f);
        if ((uint32_t)cur_pos < stubs_sec_offset) {
            size_t pad = stubs_sec_offset - (uint32_t)cur_pos;
            uint8_t zero = 0;
            for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
        }
        fwrite(stubs_buf, 1, stubs_size, f);
    }

    /* Write .rodata */
    if (rodata && rodata_size > 0) {
        cur_pos = ftell(f);
        if ((uint32_t)cur_pos < rodata_sec_offset) {
            size_t pad = rodata_sec_offset - (uint32_t)cur_pos;
            uint8_t zero = 0;
            for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
        }
        fwrite(rodata, 1, rodata_size, f);
    }

    /* Pad to data_fileoff */
    cur_pos = ftell(f);
    if (has_data_seg && (uint32_t)cur_pos < data_fileoff) {
        size_t pad = data_fileoff - (uint32_t)cur_pos;
        uint8_t zero = 0;
        for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    }

    /* Write __la_symbol_ptr (initialized to 0) */
    if (la_size > 0) {
        uint8_t *la_zeros = (uint8_t *)calloc(1, la_size);
        if (la_zeros) {
            fwrite(la_zeros, 1, la_size, f);
            free(la_zeros);
        }
    }

    /* Write .data */
    if (data && data_size > 0) {
        cur_pos = ftell(f);
        if ((uint32_t)cur_pos < data_sec_offset) {
            size_t pad = data_sec_offset - (uint32_t)cur_pos;
            uint8_t zero = 0;
            for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
        }
        fwrite(data, 1, data_size, f);
    }

    /* Pad to linkedit_fileoff */
    cur_pos = ftell(f);
    if ((uint32_t)cur_pos < linkedit_fileoff) {
        size_t pad = linkedit_fileoff - (uint32_t)cur_pos;
        uint8_t zero = 0;
        for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    }

    /* Write symbol table */
    for (uint32_t i = 0; i < nsyms; i++) {
        ZeroCCNlist64 nl;
        memset(&nl, 0, sizeof(nl));
        nl.n_strx = syms[i].str_offset;
        nl.n_type = syms[i].type;
        nl.n_sect = syms[i].sect;
        nl.n_desc = syms[i].desc;
        nl.n_value = syms[i].value;
        fwrite(&nl, sizeof(nl), 1, f);
    }

    /* Write string table */
    fwrite(strtab, 1, str_len, f);

    /* Write indirect symbol table */
    if (import_count > 0) {
        cur_pos = ftell(f);
        if ((uint32_t)cur_pos < indirectsymoff) {
            size_t pad = indirectsymoff - (uint32_t)cur_pos;
            uint8_t zero = 0;
            for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
        }
        fwrite(indirect_symbols, sizeof(uint32_t), import_count, f);
    }

    /* Pad file to linkedit_fileoff + linkedit_filesize */
    cur_pos = ftell(f);
    uint32_t total_expected = linkedit_fileoff + linkedit_filesize;
    if ((uint32_t)cur_pos < total_expected) {
        size_t pad = total_expected - (uint32_t)cur_pos;
        uint8_t zero = 0;
        for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    }

    if (stubs_buf) free(stubs_buf);
    fclose(f);
    return true;
}

bool zero_cc_macho_write_exe_arch(const char *filename,
                                  const uint8_t *text, size_t text_size,
                                  const uint8_t *rodata, size_t rodata_size,
                                  const uint8_t *data, size_t data_size,
                                  size_t bss_size,
                                  uint32_t entry_offset,
                                  uint32_t cputype)
{
    return zero_cc_macho_write_exe_arch_ext(filename, text, text_size,
                                            rodata, rodata_size,
                                            data, data_size,
                                            bss_size, entry_offset,
                                            cputype, NULL, 0, NULL, 0);
}

bool zero_cc_macho_write_exe(const char *filename,
                             const uint8_t *text, size_t text_size,
                             const uint8_t *rodata, size_t rodata_size,
                             const uint8_t *data, size_t data_size,
                             size_t bss_size,
                             uint32_t entry_offset)
{
    return zero_cc_macho_write_exe_arch(filename, text, text_size,
                                        rodata, rodata_size,
                                        data, data_size,
                                        bss_size, entry_offset,
                                        ZERO_CC_CPU_TYPE_X86_64);
}

static bool macho_write_dylib_core(const char *filename,
                                   const uint8_t *text, size_t text_size,
                                   const uint8_t *rodata, size_t rodata_size,
                                   const uint8_t *data, size_t data_size,
                                   size_t bss_size,
                                   const MachOSymbol *syms, uint32_t nsyms,
                                   const char *strtab, uint32_t str_len,
                                   uint32_t cputype,
                                   const char *dylib_name)
{
    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    uint32_t page_size = MACHO_PAGE_SIZE;
    uint32_t text_nsects = 1 + (rodata_size > 0 ? 1 : 0);
    uint32_t data_nsects = (data_size > 0 ? 1 : 0) + (bss_size > 0 ? 1 : 0);
    bool has_data_seg = (data_nsects > 0);

    uint32_t cmd_text_size = (uint32_t)(sizeof(ZeroCCSegmentCommand64) + text_nsects * sizeof(ZeroCCSection64));
    uint32_t cmd_data_size = has_data_seg ? (uint32_t)(sizeof(ZeroCCSegmentCommand64) + data_nsects * sizeof(ZeroCCSection64)) : 0;
    uint32_t cmd_linkedit_size = sizeof(ZeroCCSegmentCommand64);
    uint32_t cmd_id_dylib_size = 56;
    uint32_t cmd_dylib_size = 56;
    uint32_t cmd_symtab_size = sizeof(ZeroCCSymtabCommand);
    uint32_t cmd_dysymtab_size = sizeof(ZeroCCDysymtabCommand);

    uint32_t ncmds = 6 + (has_data_seg ? 1 : 0);
    uint32_t sizeofcmds = cmd_text_size + cmd_data_size + cmd_linkedit_size +
                          cmd_id_dylib_size + cmd_dylib_size +
                          cmd_symtab_size + cmd_dysymtab_size;

    ZeroCCMachHeader64 hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ZERO_CC_MH_MAGIC_64;
    hdr.cputype = cputype;
    hdr.cpusubtype = (cputype == ZERO_CC_CPU_TYPE_ARM64) ?
                     ZERO_CC_CPU_SUBTYPE_ARM64_ALL : ZERO_CC_CPU_SUBTYPE_X86_64_ALL;
    hdr.filetype = ZERO_CC_MH_DYLIB;
    hdr.ncmds = ncmds;
    hdr.sizeofcmds = sizeofcmds;
    hdr.flags = ZERO_CC_MH_NOUNDEFS | ZERO_CC_MH_DYLDLINK | ZERO_CC_MH_TWOLEVEL;

    uint64_t text_vmaddr = 0;
    uint32_t header_and_cmds_size = (uint32_t)sizeof(hdr) + sizeofcmds;
    uint32_t text_sec_offset = macho_align_up(header_and_cmds_size, 16);

    uint32_t text_total_raw = (uint32_t)(text_sec_offset + text_size + (rodata_size > 0 ? rodata_size + 16 : 0));
    uint32_t text_filesize = macho_align_up(text_total_raw, page_size);
    uint64_t text_vmsize = text_filesize;

    uint64_t data_vmaddr = text_vmaddr + text_vmsize;
    uint32_t data_fileoff = text_filesize;
    uint32_t data_filesize = has_data_seg ? macho_align_up((uint32_t)data_size, page_size) : 0;
    uint64_t data_vmsize = has_data_seg ? macho_align_up64((uint64_t)(data_size + bss_size), page_size) : 0;

    uint64_t linkedit_vmaddr = data_vmaddr + (has_data_seg ? data_vmsize : 0);
    uint32_t linkedit_fileoff = data_fileoff + data_filesize;
    uint32_t linkedit_filesize = page_size;
    uint64_t linkedit_vmsize = page_size;

    /* 1. __TEXT */
    ZeroCCSegmentCommand64 cmd_text;
    memset(&cmd_text, 0, sizeof(cmd_text));
    cmd_text.cmd = ZERO_CC_LC_SEGMENT_64;
    cmd_text.cmdsize = cmd_text_size;
    snprintf(cmd_text.segname, sizeof(cmd_text.segname), "__TEXT");
    cmd_text.vmaddr = text_vmaddr;
    cmd_text.vmsize = text_vmsize;
    cmd_text.fileoff = 0;
    cmd_text.filesize = text_filesize;
    cmd_text.maxprot = ZERO_CC_VM_PROT_ALL;
    cmd_text.initprot = ZERO_CC_VM_PROT_READ | ZERO_CC_VM_PROT_EXECUTE;
    cmd_text.nsects = text_nsects;

    ZeroCCSection64 text_sects[2];
    memset(text_sects, 0, sizeof(text_sects));
    snprintf(text_sects[0].sectname, sizeof(text_sects[0].sectname), "__text");
    snprintf(text_sects[0].segname, sizeof(text_sects[0].segname), "__TEXT");
    text_sects[0].addr = text_vmaddr + text_sec_offset;
    text_sects[0].size = text_size;
    text_sects[0].offset = text_sec_offset;
    text_sects[0].align = 4;
    text_sects[0].flags = ZERO_CC_MACHO_S_REGULAR | ZERO_CC_MACHO_S_ATTR_SOME_INSTRUCTIONS | ZERO_CC_MACHO_S_ATTR_PURE_INSTRUCTIONS;

    if (rodata_size > 0) {
        uint32_t rodata_sec_offset = macho_align_up(text_sec_offset + (uint32_t)text_size, 16);
        snprintf(text_sects[1].sectname, sizeof(text_sects[1].sectname), "__cstring");
        snprintf(text_sects[1].segname, sizeof(text_sects[1].segname), "__TEXT");
        text_sects[1].addr = text_vmaddr + rodata_sec_offset;
        text_sects[1].size = rodata_size;
        text_sects[1].offset = rodata_sec_offset;
        text_sects[1].align = 0;
        text_sects[1].flags = ZERO_CC_MACHO_S_CSTRING_LITERALS;
    }

    /* 2. __DATA */
    ZeroCCSegmentCommand64 cmd_data;
    ZeroCCSection64 data_sects[2];
    memset(&cmd_data, 0, sizeof(cmd_data));
    memset(data_sects, 0, sizeof(data_sects));

    if (has_data_seg) {
        cmd_data.cmd = ZERO_CC_LC_SEGMENT_64;
        cmd_data.cmdsize = cmd_data_size;
        snprintf(cmd_data.segname, sizeof(cmd_data.segname), "__DATA");
        cmd_data.vmaddr = data_vmaddr;
        cmd_data.vmsize = data_vmsize;
        cmd_data.fileoff = data_fileoff;
        cmd_data.filesize = data_filesize;
        cmd_data.maxprot = ZERO_CC_VM_PROT_ALL;
        cmd_data.initprot = ZERO_CC_VM_PROT_READ | ZERO_CC_VM_PROT_WRITE;
        cmd_data.nsects = data_nsects;

        uint32_t ds_idx = 0;
        if (data_size > 0) {
            snprintf(data_sects[ds_idx].sectname, sizeof(data_sects[ds_idx].sectname), "__data");
            snprintf(data_sects[ds_idx].segname, sizeof(data_sects[ds_idx].segname), "__DATA");
            data_sects[ds_idx].addr = data_vmaddr;
            data_sects[ds_idx].size = data_size;
            data_sects[ds_idx].offset = data_fileoff;
            data_sects[ds_idx].align = 3;
            data_sects[ds_idx].flags = ZERO_CC_MACHO_S_REGULAR;
            ds_idx++;
        }
        if (bss_size > 0) {
            snprintf(data_sects[ds_idx].sectname, sizeof(data_sects[ds_idx].sectname), "__bss");
            snprintf(data_sects[ds_idx].segname, sizeof(data_sects[ds_idx].segname), "__DATA");
            data_sects[ds_idx].addr = data_vmaddr + data_size;
            data_sects[ds_idx].size = bss_size;
            data_sects[ds_idx].offset = 0;
            data_sects[ds_idx].align = 3;
            data_sects[ds_idx].flags = ZERO_CC_MACHO_S_ZEROFILL;
            ds_idx++;
        }
    }

    /* 3. __LINKEDIT */
    ZeroCCSegmentCommand64 cmd_linkedit;
    memset(&cmd_linkedit, 0, sizeof(cmd_linkedit));
    cmd_linkedit.cmd = ZERO_CC_LC_SEGMENT_64;
    cmd_linkedit.cmdsize = cmd_linkedit_size;
    snprintf(cmd_linkedit.segname, sizeof(cmd_linkedit.segname), "__LINKEDIT");
    cmd_linkedit.vmaddr = linkedit_vmaddr;
    cmd_linkedit.vmsize = linkedit_vmsize;
    cmd_linkedit.fileoff = linkedit_fileoff;
    cmd_linkedit.filesize = linkedit_filesize;
    cmd_linkedit.maxprot = ZERO_CC_VM_PROT_ALL;
    cmd_linkedit.initprot = ZERO_CC_VM_PROT_READ;
    cmd_linkedit.nsects = 0;

    /* 4. LC_ID_DYLIB */
    uint8_t id_dylib_buf[56];
    memset(id_dylib_buf, 0, sizeof(id_dylib_buf));
    ZeroCCDylibCommand *cmd_id_dylib = (ZeroCCDylibCommand *)id_dylib_buf;
    cmd_id_dylib->cmd = ZERO_CC_LC_ID_DYLIB;
    cmd_id_dylib->cmdsize = cmd_id_dylib_size;
    cmd_id_dylib->dylib_name_offset = 24;
    cmd_id_dylib->dylib_timestamp = 1;
    cmd_id_dylib->dylib_current_version = 0x010000;
    cmd_id_dylib->dylib_compatibility_version = 0x010000;
    const char *id_name = dylib_name ? dylib_name : filename;
    snprintf((char *)(id_dylib_buf + 24), sizeof(id_dylib_buf) - 24, "%s", id_name);

    /* 5. LC_LOAD_DYLIB */
    uint8_t dylib_buf[56];
    memset(dylib_buf, 0, sizeof(dylib_buf));
    ZeroCCDylibCommand *cmd_dylib = (ZeroCCDylibCommand *)dylib_buf;
    cmd_dylib->cmd = ZERO_CC_LC_LOAD_DYLIB;
    cmd_dylib->cmdsize = cmd_dylib_size;
    cmd_dylib->dylib_name_offset = 24;
    cmd_dylib->dylib_timestamp = 2;
    cmd_dylib->dylib_current_version = 0x05000000;
    cmd_dylib->dylib_compatibility_version = 0x01000000;
    memcpy(dylib_buf + 24, "/usr/lib/libSystem.B.dylib", 27);

    uint32_t symoff = linkedit_fileoff;
    uint32_t stroff = (uint32_t)(symoff + nsyms * sizeof(ZeroCCNlist64));

    ZeroCCSymtabCommand cmd_symtab;
    memset(&cmd_symtab, 0, sizeof(cmd_symtab));
    cmd_symtab.cmd = ZERO_CC_LC_SYMTAB;
    cmd_symtab.cmdsize = cmd_symtab_size;
    cmd_symtab.symoff = symoff;
    cmd_symtab.nsyms = nsyms;
    cmd_symtab.stroff = stroff;
    cmd_symtab.strsize = str_len;

    ZeroCCDysymtabCommand cmd_dysymtab;
    memset(&cmd_dysymtab, 0, sizeof(cmd_dysymtab));
    cmd_dysymtab.cmd = ZERO_CC_LC_DYSYMTAB;
    cmd_dysymtab.cmdsize = cmd_dysymtab_size;
    cmd_dysymtab.iextdefsym = 0;
    cmd_dysymtab.nextdefsym = nsyms;

    /* Write headers & commands */
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(&cmd_text, sizeof(cmd_text), 1, f);
    fwrite(text_sects, sizeof(ZeroCCSection64), text_nsects, f);
    if (has_data_seg) {
        fwrite(&cmd_data, sizeof(cmd_data), 1, f);
        fwrite(data_sects, sizeof(ZeroCCSection64), data_nsects, f);
    }
    fwrite(&cmd_linkedit, sizeof(cmd_linkedit), 1, f);
    fwrite(id_dylib_buf, 1, sizeof(id_dylib_buf), f);
    fwrite(dylib_buf, 1, sizeof(dylib_buf), f);
    fwrite(&cmd_symtab, sizeof(cmd_symtab), 1, f);
    fwrite(&cmd_dysymtab, sizeof(cmd_dysymtab), 1, f);

    /* Pad to text_sec_offset */
    long cur_pos = ftell(f);
    if ((uint32_t)cur_pos < text_sec_offset) {
        size_t pad = text_sec_offset - (uint32_t)cur_pos;
        uint8_t zero = 0;
        for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    }

    if (text && text_size > 0) fwrite(text, 1, text_size, f);
    if (rodata && rodata_size > 0) {
        cur_pos = ftell(f);
        if ((uint32_t)cur_pos < text_sects[1].offset) {
            size_t pad = text_sects[1].offset - (uint32_t)cur_pos;
            uint8_t zero = 0;
            for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
        }
        fwrite(rodata, 1, rodata_size, f);
    }

    cur_pos = ftell(f);
    if (has_data_seg && (uint32_t)cur_pos < data_fileoff) {
        size_t pad = data_fileoff - (uint32_t)cur_pos;
        uint8_t zero = 0;
        for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    }
    if (data && data_size > 0) fwrite(data, 1, data_size, f);

    cur_pos = ftell(f);
    if ((uint32_t)cur_pos < linkedit_fileoff) {
        size_t pad = linkedit_fileoff - (uint32_t)cur_pos;
        uint8_t zero = 0;
        for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    }

    for (uint32_t i = 0; i < nsyms; i++) {
        ZeroCCNlist64 nl;
        memset(&nl, 0, sizeof(nl));
        nl.n_strx = syms[i].str_offset;
        nl.n_type = syms[i].type;
        nl.n_sect = syms[i].sect;
        nl.n_desc = syms[i].desc;
        nl.n_value = syms[i].value;
        fwrite(&nl, sizeof(nl), 1, f);
    }
    fwrite(strtab, 1, str_len, f);

    cur_pos = ftell(f);
    uint32_t total_expected = linkedit_fileoff + linkedit_filesize;
    if ((uint32_t)cur_pos < total_expected) {
        size_t pad = total_expected - (uint32_t)cur_pos;
        uint8_t zero = 0;
        for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    }

    fclose(f);
    return true;
}

bool zero_cc_macho_write_binary_from_comp(const char *filename,
                                         ZeroCCCompiler *comp,
                                         const ZeroCCMachoOptions *opts)
{
    if (!filename || !comp) return false;

    bool is_dylib = opts ? opts->is_dylib : false;

    /* 1. Extract sections from compiler */
    const uint8_t *text_data = (const uint8_t *)comp->text_section.data;
    size_t text_size = comp->text_section.length;
    const uint8_t *rodata_data = (const uint8_t *)comp->rodata_section.data;
    size_t rodata_size = comp->rodata_section.length;
    const uint8_t *data_data = (const uint8_t *)comp->data_section.data;
    size_t data_size = comp->data_section.length;
    size_t bss_size = 0;

    for (size_t i = 0; i < comp->global_var_count; i++) {
        ZeroCCGlobalVar *gv = &comp->global_vars[i];
        if (gv->is_bss) {
            size_t end = gv->data_offset + gv->size;
            if (end > bss_size) bss_size = end;
        }
    }

    /* 2. Locate entry point (_main) */
    uint32_t entry_offset = 0;
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *f = &comp->functions[i];
        if (f->name[0] && strcmp(f->name, "main") == 0) {
            entry_offset = (uint32_t)f->text_offset;
            break;
        }
    }

    /* 3. Patch internal function calls and external dynamic stubs */
    uint8_t *patched_text = NULL;
    const char *import_names[128];
    uint32_t import_count = 0;

    uint32_t cputype = (opts && opts->cputype) ? opts->cputype :
                       ((comp->isa == ISA_AARCH64) ? ZERO_CC_CPU_TYPE_ARM64 : ZERO_CC_CPU_TYPE_X86_64);

    if (text_size > 0) {
        patched_text = (uint8_t *)malloc(text_size);
        if (patched_text) {
            memcpy(patched_text, text_data, text_size);

            /* Pass A: collect external function symbols */
            for (size_t i = 0; i < comp->reloc_count; i++) {
                ZeroCCReloc *r = &comp->relocs[i];
                if (r->is_call || r->is_abs_call) {
                    ZeroCCFunction *tgt = zero_cc_find_function_by_symbol(comp, r->target_symbol);
                    if (!tgt || tgt->is_prototype) {
                        bool found = false;
                        for (uint32_t k = 0; k < import_count; k++) {
                            if (strcmp(import_names[k], r->target_symbol) == 0) {
                                found = true;
                                break;
                            }
                        }
                        if (!found && import_count < 128) {
                            import_names[import_count++] = r->target_symbol;
                        }
                    }
                }
            }

            /* Calculate stubs offset and VM layout */
            uint32_t page_size = MACHO_PAGE_SIZE;
            uint32_t stub_inst_size = (cputype == ZERO_CC_CPU_TYPE_ARM64) ? 12 : 6;
            uint32_t stubs_size = import_count * stub_inst_size;
            uint32_t la_size = import_count * 8;
            uint32_t text_nsects = 1 + (stubs_size > 0 ? 1 : 0) + (rodata_size > 0 ? 1 : 0);
            uint32_t data_nsects = (la_size > 0 ? 1 : 0) + (data_size > 0 ? 1 : 0) + (bss_size > 0 ? 1 : 0);
            bool has_data_seg = (data_nsects > 0);

            uint32_t cmd_pagezero_size = is_dylib ? 0 : sizeof(ZeroCCSegmentCommand64);
            uint32_t cmd_text_size = (uint32_t)(sizeof(ZeroCCSegmentCommand64) + text_nsects * sizeof(ZeroCCSection64));
            uint32_t cmd_data_size = has_data_seg ?
                (uint32_t)(sizeof(ZeroCCSegmentCommand64) + data_nsects * sizeof(ZeroCCSection64)) : 0;
            uint32_t cmd_linkedit_size = sizeof(ZeroCCSegmentCommand64);
            uint32_t cmd_dylinker_size = is_dylib ? 0 : 32;
            uint32_t cmd_dylib_size = 56;
            uint32_t cmd_id_dylib_size = is_dylib ? 56 : 0;
            uint32_t cmd_main_size = is_dylib ? 0 : sizeof(ZeroCCEntryPointCommand);
            uint32_t cmd_symtab_size = sizeof(ZeroCCSymtabCommand);
            uint32_t cmd_dysymtab_size = sizeof(ZeroCCDysymtabCommand);

            uint32_t sizeofcmds = cmd_pagezero_size + cmd_text_size + cmd_data_size +
                                  cmd_linkedit_size + cmd_dylinker_size + cmd_dylib_size +
                                  cmd_id_dylib_size + cmd_main_size + cmd_symtab_size + cmd_dysymtab_size;
            uint32_t header_and_cmds_size = (uint32_t)sizeof(ZeroCCMachHeader64) + sizeofcmds;
            uint32_t text_sec_offset = macho_align_up(header_and_cmds_size, 16);
            uint64_t text_vmaddr = is_dylib ? 0 : 0x100000000ULL;
            uint32_t stubs_rel_offset = macho_align_up((uint32_t)text_size, 16);

            uint32_t cur_calc_off = text_sec_offset + macho_align_up((uint32_t)text_size, 16);
            if (stubs_size > 0) cur_calc_off = macho_align_up(cur_calc_off + stubs_size, 16);
            uint32_t rodata_rel_offset = cur_calc_off - text_sec_offset;

            /* Pass B: patch call relocations */
            for (size_t i = 0; i < comp->reloc_count; i++) {
                ZeroCCReloc *r = &comp->relocs[i];
                if (r->is_call) {
                    ZeroCCFunction *tgt = zero_cc_find_function_by_symbol(comp, r->target_symbol);
                    if (tgt && !tgt->is_prototype) {
                        int32_t rel = (int32_t)(tgt->text_offset - (r->text_patch_offset + 4));
                        if (r->text_patch_offset + 4 <= text_size) {
                            memcpy(patched_text + r->text_patch_offset, &rel, 4);
                        }
                    } else {
                        /* External call -> wire to __stubs slot */
                        for (uint32_t k = 0; k < import_count; k++) {
                            if (strcmp(import_names[k], r->target_symbol) == 0) {
                                uint32_t stub_off = stubs_rel_offset + k * stub_inst_size;
                                int32_t rel = (int32_t)(stub_off - (r->text_patch_offset + 4));
                                if (r->text_patch_offset + 4 <= text_size) {
                                    memcpy(patched_text + r->text_patch_offset, &rel, 4);
                                }
                                break;
                            }
                        }
                    }
                } else if (r->is_abs_call) {
                    ZeroCCFunction *tgt = zero_cc_find_function_by_symbol(comp, r->target_symbol);
                    if (tgt && !tgt->is_prototype) {
                        uint64_t va = text_vmaddr + text_sec_offset + tgt->text_offset;
                        if (r->text_patch_offset + 8 <= text_size) {
                            memcpy(patched_text + r->text_patch_offset, &va, 8);
                        }
                    } else {
                        /* External call -> write stub address into the imm64 slot */
                        for (uint32_t k = 0; k < import_count; k++) {
                            if (strcmp(import_names[k], r->target_symbol) == 0) {
                                uint64_t stub_va = text_vmaddr + text_sec_offset + stubs_rel_offset + k * stub_inst_size;
                                if (r->text_patch_offset + 8 <= text_size) {
                                    memcpy(patched_text + r->text_patch_offset, &stub_va, 8);
                                }
                                break;
                            }
                        }
                    }
                } else if (strncmp(r->target_symbol, "__data_", 7) == 0) {
                    ZeroCCGlobalVar *gv = NULL;
                    for (size_t g = 0; g < comp->global_var_count; g++) {
                        if (strcmp(comp->global_vars[g].name, r->target_symbol + 7) == 0) {
                            gv = &comp->global_vars[g];
                            break;
                        }
                    }
                    if (gv) {
                        uint32_t text_total_raw = cur_calc_off + (uint32_t)rodata_size;
                        uint32_t text_filesize = macho_align_up(text_total_raw, page_size);
                        uint64_t data_vmaddr = text_vmaddr + text_filesize;
                        uint64_t cur_dvm = data_vmaddr;
                        if (la_size > 0) cur_dvm = macho_align_up64(cur_dvm + la_size, 16);
                        uint64_t target_va = cur_dvm + gv->data_offset;
                        if (gv->is_bss) target_va += data_size;
                        uint64_t rip_at_next = text_vmaddr + text_sec_offset + r->text_patch_offset + 4;
                        int32_t rel = (int32_t)(target_va - rip_at_next);
                        if (r->text_patch_offset + 4 <= text_size) {
                            memcpy(patched_text + r->text_patch_offset, &rel, 4);
                        }
                    }
                }
            }

            /* Pass C: patch string relocations */
            for (size_t i = 0; i < comp->str_reloc_count; i++) {
                ZeroCCStringReloc *sr = &comp->str_relocs[i];
                uint32_t target_off = rodata_rel_offset + (uint32_t)sr->rodata_offset;
                uint32_t rip_at_next = (uint32_t)sr->text_patch_offset + 4;
                int32_t disp = (int32_t)(target_off - rip_at_next);
                if (sr->text_patch_offset + 4 <= text_size) {
                    memcpy(patched_text + sr->text_patch_offset, &disp, 4);
                }
            }
        }
    }

    const uint8_t *final_text = patched_text ? patched_text : text_data;
    uint32_t cpusubtype = (cputype == ZERO_CC_CPU_TYPE_ARM64) ?
                          ZERO_CC_CPU_SUBTYPE_ARM64_ALL : ZERO_CC_CPU_SUBTYPE_X86_64_ALL;
    (void)cpusubtype;

    bool ok = false;
    if (is_dylib) {
        char strtab[4096];
        memset(strtab, 0, sizeof(strtab));
        uint32_t str_len = 1;

        MachOSymbol syms[64];
        uint32_t nsyms = 0;

        uint64_t text_vmaddr = 0;
        uint32_t text_nsects = 1 + (rodata_size > 0 ? 1 : 0);
        uint32_t data_nsects = (data_size > 0 ? 1 : 0) + (bss_size > 0 ? 1 : 0);
        bool has_data_seg = (data_nsects > 0);
        uint32_t cmd_text_size = (uint32_t)(sizeof(ZeroCCSegmentCommand64) + text_nsects * sizeof(ZeroCCSection64));
        uint32_t cmd_data_size = has_data_seg ? (uint32_t)(sizeof(ZeroCCSegmentCommand64) + data_nsects * sizeof(ZeroCCSection64)) : 0;
        uint32_t sizeofcmds = cmd_text_size + cmd_data_size + sizeof(ZeroCCSegmentCommand64) +
                              56 + 56 + sizeof(ZeroCCSymtabCommand) + sizeof(ZeroCCDysymtabCommand);
        uint32_t header_and_cmds_size = (uint32_t)sizeof(ZeroCCMachHeader64) + sizeofcmds;
        uint32_t text_sec_offset = macho_align_up(header_and_cmds_size, 16);

        for (size_t i = 0; i < comp->function_count && nsyms < 60; i++) {
            ZeroCCFunction *fn = &comp->functions[i];
            if (!fn->is_prototype && fn->name[0] != '\0') {
                snprintf(syms[nsyms].name, sizeof(syms[nsyms].name), "_%s", fn->name);
                syms[nsyms].type = ZERO_CC_N_SECT | ZERO_CC_N_EXT;
                syms[nsyms].sect = 1;
                syms[nsyms].desc = 0;
                syms[nsyms].value = text_vmaddr + text_sec_offset + fn->text_offset;
                syms[nsyms].str_offset = str_len;
                size_t slen = strlen(syms[nsyms].name) + 1;
                if (str_len + slen < sizeof(strtab)) {
                    memcpy(strtab + str_len, syms[nsyms].name, slen);
                    str_len += (uint32_t)slen;
                    nsyms++;
                }
            }
        }

        const char *id_name = (opts && opts->dylib_name) ? opts->dylib_name : filename;
        ok = macho_write_dylib_core(filename,
                                    final_text, text_size,
                                    rodata_data, rodata_size,
                                    data_data, data_size,
                                    bss_size,
                                    syms, nsyms,
                                    strtab, str_len,
                                    cputype,
                                    id_name);
    } else {
        ok = zero_cc_macho_write_exe_arch_ext(filename,
                                              final_text, text_size,
                                              rodata_data, rodata_size,
                                              data_data, data_size,
                                              bss_size,
                                              entry_offset,
                                              cputype,
                                              import_names, import_count,
                                              NULL, 0);
    }

    if (patched_text) free(patched_text);
    return ok;
}

bool zero_cc_macho_write_exe_from_comp(const char *filename, ZeroCCCompiler *comp) {
    ZeroCCMachoOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.is_dylib = false;
    opts.cputype = (comp && comp->isa == ISA_AARCH64) ? ZERO_CC_CPU_TYPE_ARM64 : ZERO_CC_CPU_TYPE_X86_64;
    return zero_cc_macho_write_binary_from_comp(filename, comp, &opts);
}

bool zero_cc_macho_write_dylib_from_comp(const char *filename, ZeroCCCompiler *comp) {
    ZeroCCMachoOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.is_dylib = true;
    opts.cputype = (comp && comp->isa == ISA_AARCH64) ? ZERO_CC_CPU_TYPE_ARM64 : ZERO_CC_CPU_TYPE_X86_64;
    return zero_cc_macho_write_binary_from_comp(filename, comp, &opts);
}

bool zero_cc_macho_write_obj_from_comp(const char *filename, ZeroCCCompiler *comp) {
    if (!filename || !comp) return false;

    const uint8_t *text_data = (const uint8_t *)comp->text_section.data;
    size_t text_size = comp->text_section.length;
    const uint8_t *rodata_data = (const uint8_t *)comp->rodata_section.data;
    size_t rodata_size = comp->rodata_section.length;
    const uint8_t *data_data = (const uint8_t *)comp->data_section.data;
    size_t data_size = comp->data_section.length;
    size_t bss_size = 0;

    for (size_t i = 0; i < comp->global_var_count; i++) {
        ZeroCCGlobalVar *gv = &comp->global_vars[i];
        if (gv->is_bss) {
            size_t end = gv->data_offset + gv->size;
            if (end > bss_size) bss_size = end;
        }
    }

    uint32_t cputype = (comp->isa == ISA_AARCH64) ? ZERO_CC_CPU_TYPE_ARM64 : ZERO_CC_CPU_TYPE_X86_64;
    return macho_write_obj_core(filename,
                                text_data, text_size,
                                rodata_data, rodata_size,
                                data_data, data_size,
                                bss_size,
                                cputype,
                                comp);
}

bool zero_cc_macho_link_objects(const char *filename,
                                const char **obj_files,
                                size_t obj_count,
                                const ZeroCCMachoOptions *opts)
{
    if (!filename || !obj_files || obj_count == 0) return false;

    ZeroCCMachoParsedObject *parsed = (ZeroCCMachoParsedObject *)calloc(obj_count, sizeof(ZeroCCMachoParsedObject));
    if (!parsed) return false;

    char err[256];
    uint32_t target_cpu = 0;
    for (size_t i = 0; i < obj_count; i++) {
        if (!zero_cc_macho_read_object_file(obj_files[i], &parsed[i], err, sizeof(err))) {
            fprintf(stderr, "zerocc: error reading Mach-O object %s: %s\n", obj_files[i], err);
            for (size_t j = 0; j <= i; j++) zero_cc_macho_parsed_object_free(&parsed[j]);
            free(parsed);
            return false;
        }
        if (target_cpu == 0) target_cpu = parsed[i].cputype;
    }
    if (opts && opts->cputype != 0) target_cpu = opts->cputype;
    if (target_cpu == 0) target_cpu = ZERO_CC_CPU_TYPE_X86_64;

    /* Buffers for combined sections */
    size_t text_cap = 65536, text_size = 0;
    uint8_t *text_buf = (uint8_t *)calloc(1, text_cap);
    size_t rodata_cap = 65536, rodata_size = 0;
    uint8_t *rodata_buf = (uint8_t *)calloc(1, rodata_cap);
    size_t data_cap = 65536, data_size = 0;
    uint8_t *data_buf = (uint8_t *)calloc(1, data_cap);
    size_t bss_size = 0;

    size_t *obj_text_off = (size_t *)calloc(obj_count, sizeof(size_t));
    size_t *obj_rodata_off = (size_t *)calloc(obj_count, sizeof(size_t));
    size_t *obj_data_off = (size_t *)calloc(obj_count, sizeof(size_t));
    size_t *obj_bss_off = (size_t *)calloc(obj_count, sizeof(size_t));

    /* 1. Combine sections across all objects */
    for (size_t i = 0; i < obj_count; i++) {
        /* __text */
        const ZeroCCMachoSectParsed *s_text = zero_cc_macho_find_section(&parsed[i], "__TEXT", "__text");
        if (s_text && s_text->size > 0 && s_text->data) {
            text_size = macho_align_up((uint32_t)text_size, 16);
            obj_text_off[i] = text_size;
            while (text_size + s_text->size > text_cap) {
                text_cap *= 2;
                text_buf = (uint8_t *)realloc(text_buf, text_cap);
            }
            memcpy(text_buf + text_size, s_text->data, s_text->size);
            text_size += s_text->size;
        }

        /* __cstring / rodata */
        const ZeroCCMachoSectParsed *s_ro = zero_cc_macho_find_section(&parsed[i], "__TEXT", "__cstring");
        if (s_ro && s_ro->size > 0 && s_ro->data) {
            rodata_size = macho_align_up((uint32_t)rodata_size, 8);
            obj_rodata_off[i] = rodata_size;
            while (rodata_size + s_ro->size > rodata_cap) {
                rodata_cap *= 2;
                rodata_buf = (uint8_t *)realloc(rodata_buf, rodata_cap);
            }
            memcpy(rodata_buf + rodata_size, s_ro->data, s_ro->size);
            rodata_size += s_ro->size;
        }

        /* __data */
        const ZeroCCMachoSectParsed *s_data = zero_cc_macho_find_section(&parsed[i], "__DATA", "__data");
        if (s_data && s_data->size > 0 && s_data->data) {
            data_size = macho_align_up((uint32_t)data_size, 8);
            obj_data_off[i] = data_size;
            while (data_size + s_data->size > data_cap) {
                data_cap *= 2;
                data_buf = (uint8_t *)realloc(data_buf, data_cap);
            }
            memcpy(data_buf + data_size, s_data->data, s_data->size);
            data_size += s_data->size;
        }

        /* __bss */
        const ZeroCCMachoSectParsed *s_bss = zero_cc_macho_find_section(&parsed[i], "__DATA", "__bss");
        if (s_bss && s_bss->size > 0) {
            bss_size = macho_align_up((uint32_t)bss_size, 8);
            obj_bss_off[i] = bss_size;
            bss_size += s_bss->size;
        }
    }

    /* 2. Collect defined global symbols */
    typedef struct {
        char name[128];
        int sect_kind; /* 1=TEXT, 2=RODATA, 3=DATA, 4=BSS */
        uint64_t offset;
    } GlobalMachoSym;

    GlobalMachoSym gsyms[1024];
    size_t ngsyms = 0;

    for (size_t i = 0; i < obj_count; i++) {
        for (size_t s = 0; s < parsed[i].symbol_count; s++) {
            const ZeroCCMachoSymParsed *sym = &parsed[i].symbols[s];
            if (sym->is_extern && (sym->type & ZERO_CC_N_SECT) &&
                sym->sect > 0 && sym->sect <= parsed[i].section_count) {
                const ZeroCCMachoSectParsed *sec = &parsed[i].sections[sym->sect - 1];
                int kind = 0;
                uint64_t base = 0;
                if (strcmp(sec->sectname, "__text") == 0) {
                    kind = 1; base = obj_text_off[i];
                } else if (strcmp(sec->sectname, "__cstring") == 0) {
                    kind = 2; base = obj_rodata_off[i];
                } else if (strcmp(sec->sectname, "__data") == 0) {
                    kind = 3; base = obj_data_off[i];
                } else if (strcmp(sec->sectname, "__bss") == 0) {
                    kind = 4; base = obj_bss_off[i];
                }
                if (kind > 0 && ngsyms < 1024) {
                    snprintf(gsyms[ngsyms].name, sizeof(gsyms[ngsyms].name), "%s", sym->name);
                    gsyms[ngsyms].sect_kind = kind;
                    gsyms[ngsyms].offset = base + sym->value;
                    ngsyms++;
                }
            }
        }
    }

    /* 3. Locate entry point (_main) */
    uint32_t entry_offset = 0;
    for (size_t s = 0; s < ngsyms; s++) {
        if (strcmp(gsyms[s].name, "_main") == 0 || strcmp(gsyms[s].name, "main") == 0) {
            entry_offset = (uint32_t)gsyms[s].offset;
            break;
        }
    }

    /* 3b. Collect unresolved external symbols for dynamic stubs */
    const char *import_names[128];
    uint32_t import_count = 0;
    bool is_dylib = (opts && opts->is_dylib);

    if (!is_dylib) {
        for (size_t i = 0; i < obj_count; i++) {
            for (size_t s = 0; s < parsed[i].section_count; s++) {
                const ZeroCCMachoSectParsed *sec = &parsed[i].sections[s];
                if (!sec->relocs || sec->reloc_count == 0) continue;
                for (size_t r = 0; r < sec->reloc_count; r++) {
                    const ZeroCCMachoRelocParsed *rel = &sec->relocs[r];
                    if (rel->is_extern && rel->symbolnum < parsed[i].symbol_count) {
                        const char *target_name = parsed[i].symbols[rel->symbolnum].name;
                        bool found_sym = false;
                        for (size_t g = 0; g < ngsyms; g++) {
                            if (strcmp(gsyms[g].name, target_name) == 0) {
                                found_sym = true;
                                break;
                            }
                        }
                        if (!found_sym) {
                            bool already = false;
                            for (uint32_t k = 0; k < import_count; k++) {
                                if (strcmp(import_names[k], target_name) == 0) {
                                    already = true;
                                    break;
                                }
                            }
                            if (!already && import_count < 128) {
                                import_names[import_count++] = target_name;
                            }
                        }
                    }
                }
            }
        }
    }

    /* 4. Determine VM Layout */
    uint64_t text_vmaddr = is_dylib ? 0 : 0x100000000ULL;
    uint32_t page_size = MACHO_PAGE_SIZE;
    uint32_t stub_inst_size = (target_cpu == ZERO_CC_CPU_TYPE_ARM64) ? 12 : 6;
    uint32_t stubs_size = import_count * stub_inst_size;
    uint32_t stubs_size_aligned = macho_align_up(stubs_size, 16);
    uint32_t la_size = import_count * 8;

    uint32_t text_nsects = 1 + (stubs_size > 0 ? 1 : 0) + (rodata_size > 0 ? 1 : 0);
    uint32_t data_nsects = (la_size > 0 ? 1 : 0) + (data_size > 0 ? 1 : 0) + (bss_size > 0 ? 1 : 0);
    bool has_data_seg = (data_nsects > 0);

    uint32_t cmd_pagezero_size = is_dylib ? 0 : sizeof(ZeroCCSegmentCommand64);
    uint32_t cmd_text_size = (uint32_t)(sizeof(ZeroCCSegmentCommand64) + text_nsects * sizeof(ZeroCCSection64));
    uint32_t cmd_data_size = has_data_seg ?
        (uint32_t)(sizeof(ZeroCCSegmentCommand64) + data_nsects * sizeof(ZeroCCSection64)) : 0;
    uint32_t cmd_linkedit_size = sizeof(ZeroCCSegmentCommand64);
    uint32_t cmd_dylinker_size = is_dylib ? 0 : 32;
    uint32_t cmd_dylib_size = 56;
    uint32_t cmd_id_dylib_size = is_dylib ? 56 : 0;
    uint32_t cmd_main_size = is_dylib ? 0 : sizeof(ZeroCCEntryPointCommand);
    uint32_t cmd_symtab_size = sizeof(ZeroCCSymtabCommand);
    uint32_t cmd_dysymtab_size = sizeof(ZeroCCDysymtabCommand);

    uint32_t sizeofcmds = cmd_pagezero_size + cmd_text_size + cmd_data_size +
                          cmd_linkedit_size + cmd_dylinker_size + cmd_dylib_size +
                          cmd_id_dylib_size + cmd_main_size + cmd_symtab_size + cmd_dysymtab_size;
    uint32_t header_and_cmds_size = (uint32_t)sizeof(ZeroCCMachHeader64) + sizeofcmds;
    uint32_t text_sec_offset = macho_align_up(header_and_cmds_size, 16);

    uint32_t cur_text_fileoff = text_sec_offset + (uint32_t)text_size;
    uint32_t stubs_sec_offset = 0;
    if (stubs_size > 0) {
        stubs_sec_offset = macho_align_up(cur_text_fileoff, 16);
        cur_text_fileoff = stubs_sec_offset + stubs_size_aligned;
    }
    uint32_t rodata_sec_offset = (rodata_size > 0) ? macho_align_up(cur_text_fileoff, 16) : 0;
    if (rodata_size > 0) {
        cur_text_fileoff = rodata_sec_offset + (uint32_t)rodata_size;
    }

    uint32_t text_total_raw = cur_text_fileoff;
    uint32_t text_filesize = macho_align_up(text_total_raw, page_size);
    uint64_t text_vmsize = text_filesize;

    uint64_t data_vmaddr = text_vmaddr + text_vmsize;

    /* 5. Apply Relocations */
    for (size_t i = 0; i < obj_count; i++) {
        for (size_t s = 0; s < parsed[i].section_count; s++) {
            const ZeroCCMachoSectParsed *sec = &parsed[i].sections[s];
            if (!sec->relocs || sec->reloc_count == 0) continue;

            bool is_sec_text = (strcmp(sec->sectname, "__text") == 0);
            bool is_sec_data = (strcmp(sec->sectname, "__data") == 0);

            for (size_t r = 0; r < sec->reloc_count; r++) {
                const ZeroCCMachoRelocParsed *rel = &sec->relocs[r];
                uint64_t target_vm = 0;

                if (rel->is_extern) {
                    if (rel->symbolnum < parsed[i].symbol_count) {
                        const char *target_name = parsed[i].symbols[rel->symbolnum].name;
                        for (size_t g = 0; g < ngsyms; g++) {
                            if (strcmp(gsyms[g].name, target_name) == 0) {
                                if (gsyms[g].sect_kind == 1) {
                                    target_vm = text_vmaddr + text_sec_offset + gsyms[g].offset;
                                } else if (gsyms[g].sect_kind == 2) {
                                    target_vm = text_vmaddr + rodata_sec_offset + gsyms[g].offset;
                                } else if (gsyms[g].sect_kind == 3) {
                                    target_vm = data_vmaddr + gsyms[g].offset;
                                } else if (gsyms[g].sect_kind == 4) {
                                    target_vm = data_vmaddr + data_size + gsyms[g].offset;
                                }
                                break;
                            }
                        }

                        /* Unresolved external -> route to dynamic stub */
                        if (target_vm == 0 && import_count > 0) {
                            for (uint32_t k = 0; k < import_count; k++) {
                                if (strcmp(import_names[k], target_name) == 0) {
                                    target_vm = text_vmaddr + stubs_sec_offset + k * stub_inst_size;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    /* Section-relative */
                    if (rel->symbolnum > 0 && rel->symbolnum <= parsed[i].section_count) {
                        const ZeroCCMachoSectParsed *target_sec = &parsed[i].sections[rel->symbolnum - 1];
                        if (strcmp(target_sec->sectname, "__text") == 0) {
                            target_vm = text_vmaddr + text_sec_offset + obj_text_off[i];
                        } else if (strcmp(target_sec->sectname, "__cstring") == 0) {
                            target_vm = text_vmaddr + rodata_sec_offset + obj_rodata_off[i];
                        } else if (strcmp(target_sec->sectname, "__data") == 0) {
                            target_vm = data_vmaddr + obj_data_off[i];
                        } else if (strcmp(target_sec->sectname, "__bss") == 0) {
                            target_vm = data_vmaddr + data_size + obj_bss_off[i];
                        }
                    }
                }

                if (target_vm == 0) continue;

                if (is_sec_text && text_buf) {
                    uint8_t *patch_loc = text_buf + obj_text_off[i] + rel->address;
                    uint64_t pc_vm = text_vmaddr + text_sec_offset + obj_text_off[i] + rel->address + 4;

                    if (target_cpu == ZERO_CC_CPU_TYPE_X86_64) {
                        if (rel->type == ZERO_CC_X86_64_RELOC_BRANCH || rel->type == ZERO_CC_X86_64_RELOC_SIGNED) {
                            int32_t existing = 0;
                            memcpy(&existing, patch_loc, 4);
                            int32_t disp = (int32_t)(target_vm - pc_vm) + existing;
                            memcpy(patch_loc, &disp, 4);
                        } else if (rel->type == ZERO_CC_X86_64_RELOC_UNSIGNED) {
                            uint64_t existing = 0;
                            memcpy(&existing, patch_loc, 8);
                            uint64_t val = target_vm + existing;
                            memcpy(patch_loc, &val, 8);
                        }
                    } else if (target_cpu == ZERO_CC_CPU_TYPE_ARM64) {
                        if (rel->type == 2) { /* ARM64_RELOC_BRANCH26 */
                            int64_t delta = (int64_t)(target_vm - (pc_vm - 4));
                            int32_t imm26 = (int32_t)((delta >> 2) & 0x03FFFFFF);
                            uint32_t instr = 0;
                            memcpy(&instr, patch_loc, 4);
                            instr = (instr & 0xFC000000) | (uint32_t)imm26;
                            memcpy(patch_loc, &instr, 4);
                        } else if (rel->type == 3) { /* ARM64_RELOC_PAGE21 */
                            int64_t p_cur = (int64_t)((pc_vm - 4) >> 12);
                            int64_t p_tgt = (int64_t)(target_vm >> 12);
                            int32_t p_delta = (int32_t)(p_tgt - p_cur);
                            uint32_t immlo = (uint32_t)(p_delta & 3) << 29;
                            uint32_t immhi = (uint32_t)((p_delta >> 2) & 0x7FFFF) << 5;
                            uint32_t instr = 0;
                            memcpy(&instr, patch_loc, 4);
                            instr = (instr & 0x9F00001F) | immlo | immhi;
                            memcpy(patch_loc, &instr, 4);
                        } else if (rel->type == 4) { /* ARM64_RELOC_PAGEOFF12 */
                            uint32_t poff = (uint32_t)(target_vm & 0xFFF);
                            uint32_t instr = 0;
                            memcpy(&instr, patch_loc, 4);
                            instr = (instr & 0xFFC003FF) | (poff << 10);
                            memcpy(patch_loc, &instr, 4);
                        }
                    }
                } else if (is_sec_data && data_buf) {
                    uint8_t *patch_loc = data_buf + obj_data_off[i] + rel->address;
                    uint64_t existing = 0;
                    memcpy(&existing, patch_loc, 8);
                    uint64_t val = target_vm + existing;
                    memcpy(patch_loc, &val, 8);
                }
            }
        }
    }

    /* 6. Emit binary */
    bool ok = false;
    if (is_dylib) {
        /* Build export symbol table for dylib */
        MachOSymbol *dsyms = (MachOSymbol *)calloc(ngsyms + 1, sizeof(MachOSymbol));
        char strtab[16384];
        memset(strtab, 0, sizeof(strtab));
        uint32_t str_len = 1;
        uint32_t ndsyms = 0;

        for (size_t s = 0; s < ngsyms; s++) {
            snprintf(dsyms[ndsyms].name, sizeof(dsyms[ndsyms].name), "%s", gsyms[s].name);
            dsyms[ndsyms].type = ZERO_CC_N_SECT | ZERO_CC_N_EXT;
            dsyms[ndsyms].sect = (uint8_t)gsyms[s].sect_kind;
            dsyms[ndsyms].desc = 0;
            if (gsyms[s].sect_kind == 1) {
                dsyms[ndsyms].value = text_vmaddr + text_sec_offset + gsyms[s].offset;
            } else {
                dsyms[ndsyms].value = data_vmaddr + gsyms[s].offset;
            }
            dsyms[ndsyms].str_offset = str_len;
            size_t slen = strlen(dsyms[ndsyms].name) + 1;
            if (str_len + slen < sizeof(strtab)) {
                memcpy(strtab + str_len, dsyms[ndsyms].name, slen);
                str_len += (uint32_t)slen;
                ndsyms++;
            }
        }

        const char *dylib_name = (opts && opts->dylib_name) ? opts->dylib_name : filename;
        ok = macho_write_dylib_core(filename,
                                    text_buf, text_size,
                                    rodata_buf, rodata_size,
                                    data_buf, data_size,
                                    bss_size,
                                    dsyms, ndsyms,
                                    strtab, str_len,
                                    target_cpu,
                                    dylib_name);
        free(dsyms);
    } else {
        ok = zero_cc_macho_write_exe_arch_ext(filename,
                                              text_buf, text_size,
                                              rodata_buf, rodata_size,
                                              data_buf, data_size,
                                              bss_size,
                                              entry_offset,
                                              target_cpu,
                                              import_names, import_count,
                                              NULL, 0);
    }

    /* Cleanup */
    free(text_buf);
    free(rodata_buf);
    free(data_buf);
    free(obj_text_off);
    free(obj_rodata_off);
    free(obj_data_off);
    free(obj_bss_off);
    for (size_t i = 0; i < obj_count; i++) {
        zero_cc_macho_parsed_object_free(&parsed[i]);
    }
    free(parsed);

    return ok;
}
