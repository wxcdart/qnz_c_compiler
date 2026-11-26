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
    char strtab[2048];
    memset(strtab, 0, sizeof(strtab));
    uint32_t str_len = 1; /* strtab starts with '\0' */

    MachOSymbol syms[128];
    uint32_t nsyms = 0;

    if (comp) {
        for (size_t i = 0; i < comp->function_count && nsyms < 120; i++) {
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
        for (size_t i = 0; i < comp->global_var_count && nsyms < 120; i++) {
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

bool zero_cc_macho_write_exe_arch(const char *filename,
                                  const uint8_t *text, size_t text_size,
                                  const uint8_t *rodata, size_t rodata_size,
                                  const uint8_t *data, size_t data_size,
                                  size_t bss_size,
                                  uint32_t entry_offset,
                                  uint32_t cputype)
{
    uint32_t target_cpu = (cputype != 0) ? cputype : ZERO_CC_CPU_TYPE_X86_64;
    uint32_t target_sub = (target_cpu == ZERO_CC_CPU_TYPE_ARM64) ?
                          ZERO_CC_CPU_SUBTYPE_ARM64_ALL : ZERO_CC_CPU_SUBTYPE_X86_64_ALL;

    /* Build executable container */
    if (!filename) return false;

    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    uint32_t page_size = MACHO_PAGE_SIZE;

    /* Calculate load commands size:
     * 1. LC_SEGMENT_64 (__PAGEZERO) : 72 bytes
     * 2. LC_SEGMENT_64 (__TEXT)     : 72 + (1 or 2 sects)*80
     * 3. LC_SEGMENT_64 (__DATA)     : 72 + (data or bss sects)*80 (if any)
     * 4. LC_SEGMENT_64 (__LINKEDIT) : 72 bytes
     * 5. LC_LOAD_DYLINKER           : 32 bytes ("/usr/lib/dyld\0\0\0")
     * 6. LC_LOAD_DYLIB              : 56 bytes ("/usr/lib/libSystem.B.dylib\0...")
     * 7. LC_MAIN                    : 24 bytes
     * 8. LC_SYMTAB                  : 24 bytes
     * 9. LC_DYSYMTAB                : 80 bytes
     */
    uint32_t text_nsects = 1 + (rodata_size > 0 ? 1 : 0);
    uint32_t data_nsects = (data_size > 0 ? 1 : 0) + (bss_size > 0 ? 1 : 0);
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

    uint32_t ncmds = 8 + (has_data_seg ? 1 : 0);
    uint32_t sizeofcmds = cmd_pagezero_size + cmd_text_size + cmd_data_size +
                          cmd_linkedit_size + cmd_dylinker_size + cmd_dylib_size +
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

    /* 2. __TEXT Command & Sections */
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

    /* 3. __DATA Command & Sections */
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

    /* 8. Symbol & String Tables */
    char strtab[512];
    memset(strtab, 0, sizeof(strtab));
    uint32_t str_len = 1;

    MachOSymbol syms[8];
    uint32_t nsyms = 0;

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

    /* Write .rodata */
    if (rodata && rodata_size > 0) {
        cur_pos = ftell(f);
        if ((uint32_t)cur_pos < text_sects[1].offset) {
            size_t pad = text_sects[1].offset - (uint32_t)cur_pos;
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

    /* Write .data */
    if (data && data_size > 0) fwrite(data, 1, data_size, f);

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

    /* Pad file to linkedit_fileoff + linkedit_filesize */
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

    /* 3. Patch internal function calls and relocations */
    uint8_t *patched_text = NULL;
    if (text_size > 0) {
        patched_text = (uint8_t *)malloc(text_size);
        if (patched_text) {
            memcpy(patched_text, text_data, text_size);
            for (size_t i = 0; i < comp->reloc_count; i++) {
                ZeroCCReloc *r = &comp->relocs[i];
                if (r->is_call) {
                    ZeroCCFunction *tgt = zero_cc_find_function_by_symbol(comp, r->target_symbol);
                    if (tgt && !tgt->is_prototype) {
                        int32_t rel = (int32_t)(tgt->text_offset - (r->text_patch_offset + 4));
                        if (r->text_patch_offset + 4 <= text_size) {
                            memcpy(patched_text + r->text_patch_offset, &rel, 4);
                        }
                    }
                }
            }
        }
    }

    const uint8_t *final_text = patched_text ? patched_text : text_data;

    uint32_t cputype = (opts && opts->cputype) ? opts->cputype :
                       ((comp->isa == ISA_AARCH64) ? ZERO_CC_CPU_TYPE_ARM64 : ZERO_CC_CPU_TYPE_X86_64);
    uint32_t cpusubtype = (cputype == ZERO_CC_CPU_TYPE_ARM64) ?
                          ZERO_CC_CPU_SUBTYPE_ARM64_ALL : ZERO_CC_CPU_SUBTYPE_X86_64_ALL;

    bool ok = false;
    if (is_dylib) {
        /* Mach-O dynamic library writer */
        FILE *f = fopen(filename, "wb");
        if (!f) {
            if (patched_text) free(patched_text);
            return false;
        }

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
        hdr.cpusubtype = cpusubtype;
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
        const char *id_name = (opts && opts->dylib_name) ? opts->dylib_name : filename;
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

        /* 6. Export table in symbol table */
        char strtab[1024];
        memset(strtab, 0, sizeof(strtab));
        uint32_t str_len = 1;

        MachOSymbol syms[64];
        uint32_t nsyms = 0;

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

        if (final_text && text_size > 0) fwrite(final_text, 1, text_size, f);
        if (rodata_data && rodata_size > 0) {
            cur_pos = ftell(f);
            if ((uint32_t)cur_pos < text_sects[1].offset) {
                size_t pad = text_sects[1].offset - (uint32_t)cur_pos;
                uint8_t zero = 0;
                for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
            }
            fwrite(rodata_data, 1, rodata_size, f);
        }

        cur_pos = ftell(f);
        if (has_data_seg && (uint32_t)cur_pos < data_fileoff) {
            size_t pad = data_fileoff - (uint32_t)cur_pos;
            uint8_t zero = 0;
            for (size_t i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
        }
        if (data_data && data_size > 0) fwrite(data_data, 1, data_size, f);

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
        ok = true;
    } else {
        ok = zero_cc_macho_write_exe_arch(filename,
                                         final_text, text_size,
                                         rodata_data, rodata_size,
                                         data_data, data_size,
                                         bss_size,
                                         entry_offset,
                                         cputype);
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
