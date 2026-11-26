/**
 * @file pe_writer.c
 * @brief PE/COFF (PE32+) Executable and Object File Writer for ZeroCC (Sprint 246)
 */

#include "zero_cc/linker/pe_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zero_cc.h"
#include "zero_cc/types/global_vars.h"

static inline uint32_t pe_align_up(uint32_t val, uint32_t align) {
    if (align == 0) return val;
    return (val + align - 1) & ~(align - 1);
}

static bool zero_cc_pe_write_exe_internal(const char *filename,
                                          const uint8_t *full_text, size_t total_text_size,
                                          const uint8_t *rodata, size_t rodata_size,
                                          const uint8_t *data, size_t data_size,
                                          size_t bss_size,
                                          uint32_t entry_rva)
{
    if (!filename || !full_text) return false;

    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    /* 4 Sections: .text, .rdata, .data, .idata */
    uint16_t num_sections = 4;
    uint32_t file_align = ZERO_CC_PE_FILE_ALIGNMENT;
    uint32_t sec_align = ZERO_CC_PE_SECTION_ALIGNMENT;

    /* Headers size: DOS(64) + stub(64) + PE sig(4) + COFF(20) + Opt(240) + SecHdrs(4*40=160) = 548 */
    uint32_t raw_headers_size = (uint32_t)(sizeof(ZeroCCDosHeader) + 64 + 4 +
                                           sizeof(ZeroCCCoffFileHeader) +
                                           sizeof(ZeroCCOptionalHeader64) +
                                           num_sections * sizeof(ZeroCCSectionHeader));
    uint32_t headers_size_aligned = pe_align_up(raw_headers_size, file_align);

    /* Section RVAs and file offsets */
    uint32_t text_rva = sec_align;
    uint32_t text_raw_off = headers_size_aligned;
    uint32_t text_raw_size = pe_align_up((uint32_t)total_text_size, file_align);
    uint32_t text_virt_size = pe_align_up((uint32_t)total_text_size, sec_align);

    uint32_t rdata_rva = text_rva + text_virt_size;
    uint32_t rdata_raw_off = text_raw_off + text_raw_size;
    uint32_t rdata_raw_size = pe_align_up((uint32_t)rodata_size, file_align);
    uint32_t rdata_virt_size = pe_align_up((uint32_t)(rodata_size > 0 ? rodata_size : 1), sec_align);

    uint32_t data_rva = rdata_rva + rdata_virt_size;
    uint32_t data_raw_off = rdata_raw_off + rdata_raw_size;
    uint32_t data_raw_size = pe_align_up((uint32_t)data_size, file_align);
    uint32_t data_virt_size = pe_align_up((uint32_t)(data_size + bss_size > 0 ? data_size + bss_size : 1), sec_align);

    uint32_t idata_rva = data_rva + data_virt_size;
    uint32_t idata_raw_off = data_raw_off + data_raw_size;

    /* Build .idata section contents:
     * - Import Directory Table (2 * 20 = 40 bytes)
     * - ILT (2 * 8 = 16 bytes)
     * - IAT (2 * 8 = 16 bytes)
     * - Hint/Name ("ExitProcess\0" = 14 bytes -> 16 bytes)
     * - DLL Name ("KERNEL32.dll\0" = 13 bytes -> 16 bytes)
     * Total idata size = 104 bytes -> aligned to file_align (512)
     */
    uint32_t idata_raw_size = file_align;
    uint32_t idata_virt_size = sec_align;
    uint8_t *idata_buf = (uint8_t *)calloc(1, idata_raw_size);
    if (!idata_buf) {
        fclose(f);
        return false;
    }

    uint32_t off_desc = 0;
    uint32_t off_ilt = 40;
    uint32_t off_iat = off_ilt + 16;
    uint32_t off_hint_name = off_iat + 16;
    uint32_t off_dll_name = off_hint_name + 16;

    /* Hint/Name entry: hint = 0, name = "ExitProcess" */
    idata_buf[off_hint_name] = 0;
    idata_buf[off_hint_name + 1] = 0;
    memcpy(idata_buf + off_hint_name + 2, "ExitProcess", 12);

    /* DLL Name: "KERNEL32.dll" */
    memcpy(idata_buf + off_dll_name, "KERNEL32.dll", 13);

    /* ILT and IAT entries point to hint/name RVA */
    uint64_t hint_name_rva = (uint64_t)(idata_rva + off_hint_name);
    memcpy(idata_buf + off_ilt, &hint_name_rva, sizeof(hint_name_rva));
    memcpy(idata_buf + off_iat, &hint_name_rva, sizeof(hint_name_rva));

    /* Import Descriptor */
    ZeroCCImportDescriptor *desc = (ZeroCCImportDescriptor *)(idata_buf + off_desc);
    desc->original_first_thunk = idata_rva + off_ilt;
    desc->name_rva = idata_rva + off_dll_name;
    desc->first_thunk = idata_rva + off_iat;

    /* Total virtual image size */
    uint32_t image_size = idata_rva + idata_virt_size;

    /* Construct Headers */
    ZeroCCDosHeader dos;
    memset(&dos, 0, sizeof(dos));
    dos.e_magic = ZERO_CC_PE_DOS_MAGIC;
    dos.e_cblp = 0x90;
    dos.e_cp = 0x03;
    dos.e_cparhdr = 0x04;
    dos.e_maxalloc = 0xFFFF;
    dos.e_sp = 0xB8;
    dos.e_lfarlc = 0x40;
    dos.e_lfanew = (uint32_t)(sizeof(ZeroCCDosHeader) + 64);

    uint8_t dos_stub[64] = {
        0x0e, 0x1f, 0xba, 0x0e, 0x00, 0xb4, 0x09, 0xcd, 0x21, 0xb8, 0x01, 0x4c, 0xcd, 0x21,
        'T', 'h', 'i', 's', ' ', 'p', 'r', 'o', 'g', 'r', 'a', 'm', ' ', 'c', 'a', 'n', 'n',
        'o', 't', ' ', 'b', 'e', ' ', 'r', 'u', 'n', ' ', 'i', 'n', ' ', 'D', 'O', 'S', ' ',
        'm', 'o', 'd', 'e', '.', '\r', '\r', '\n', '$', 0, 0, 0, 0, 0, 0, 0
    };

    uint32_t pe_sig = ZERO_CC_PE_SIGNATURE;

    ZeroCCCoffFileHeader coff;
    memset(&coff, 0, sizeof(coff));
    coff.machine = ZERO_CC_IMAGE_FILE_MACHINE_AMD64;
    coff.number_of_sections = num_sections;
    coff.time_date_stamp = (uint32_t)time(NULL);
    coff.size_of_optional_header = (uint16_t)sizeof(ZeroCCOptionalHeader64);
    coff.characteristics = ZERO_CC_IMAGE_FILE_EXECUTABLE_IMAGE |
                           ZERO_CC_IMAGE_FILE_LARGE_ADDRESS_AWARE;

    ZeroCCOptionalHeader64 opt;
    memset(&opt, 0, sizeof(opt));
    opt.magic = ZERO_CC_PE32PLUS_MAGIC;
    opt.major_linker_version = 1;
    opt.minor_linker_version = 0;
    opt.size_of_code = text_raw_size;
    opt.size_of_initialized_data = rdata_raw_size + data_raw_size + idata_raw_size;
    opt.size_of_uninitialized_data = (uint32_t)pe_align_up((uint32_t)bss_size, file_align);
    opt.address_of_entry_point = entry_rva;
    opt.base_of_code = text_rva;
    opt.image_base = ZERO_CC_PE_DEFAULT_IMAGE_BASE;
    opt.section_alignment = sec_align;
    opt.file_alignment = file_align;
    opt.major_operating_system_version = 6;
    opt.minor_operating_system_version = 0;
    opt.major_image_version = 0;
    opt.minor_image_version = 0;
    opt.major_subsystem_version = 6;
    opt.minor_subsystem_version = 0;
    opt.size_of_image = image_size;
    opt.size_of_headers = headers_size_aligned;
    opt.subsystem = ZERO_CC_IMAGE_SUBSYSTEM_WINDOWS_CUI;
    opt.dll_characteristics = 0x8160; /* DYNAMIC_BASE | HIGH_ENTROPY_VA | NX_COMPAT | TERMINAL_SERVER_AWARE */
    opt.size_of_stack_reserve = 0x100000; /* 1 MB */
    opt.size_of_stack_commit  = 0x10000;
    opt.size_of_heap_reserve  = 0x100000;
    opt.size_of_heap_commit   = 0x1000;
    opt.number_of_rva_and_sizes = 16;

    /* Directory 1: Import Directory */
    opt.data_directory[1].virtual_address = idata_rva;
    opt.data_directory[1].size = (uint32_t)sizeof(ZeroCCImportDescriptor) * 2;

    /* Directory 12: IAT */
    opt.data_directory[12].virtual_address = idata_rva + off_iat;
    opt.data_directory[12].size = 16;

    /* Section Headers */
    ZeroCCSectionHeader sec[4];
    memset(sec, 0, sizeof(sec));

    /* 1. .text */
    memcpy(sec[0].name, ".text\0\0\0", 8);
    sec[0].virtual_size = (uint32_t)total_text_size;
    sec[0].virtual_address = text_rva;
    sec[0].size_of_raw_data = text_raw_size;
    sec[0].pointer_to_raw_data = text_raw_off;
    sec[0].characteristics = ZERO_CC_IMAGE_SCN_CNT_CODE |
                             ZERO_CC_IMAGE_SCN_MEM_EXECUTE |
                             ZERO_CC_IMAGE_SCN_MEM_READ;

    /* 2. .rdata */
    memcpy(sec[1].name, ".rdata\0\0", 8);
    sec[1].virtual_size = (uint32_t)(rodata_size > 0 ? rodata_size : 1);
    sec[1].virtual_address = rdata_rva;
    sec[1].size_of_raw_data = rdata_raw_size;
    sec[1].pointer_to_raw_data = rdata_raw_off;
    sec[1].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA |
                             ZERO_CC_IMAGE_SCN_MEM_READ;

    /* 3. .data */
    memcpy(sec[2].name, ".data\0\0\0", 8);
    sec[2].virtual_size = (uint32_t)(data_size + bss_size > 0 ? data_size + bss_size : 1);
    sec[2].virtual_address = data_rva;
    sec[2].size_of_raw_data = data_raw_size;
    sec[2].pointer_to_raw_data = data_raw_off;
    sec[2].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA |
                             ZERO_CC_IMAGE_SCN_MEM_READ |
                             ZERO_CC_IMAGE_SCN_MEM_WRITE;

    /* 4. .idata */
    memcpy(sec[3].name, ".idata\0\0", 8);
    sec[3].virtual_size = idata_raw_size;
    sec[3].virtual_address = idata_rva;
    sec[3].size_of_raw_data = idata_raw_size;
    sec[3].pointer_to_raw_data = idata_raw_off;
    sec[3].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA |
                             ZERO_CC_IMAGE_SCN_MEM_READ |
                             ZERO_CC_IMAGE_SCN_MEM_WRITE;

    /* Write headers */
    fwrite(&dos, 1, sizeof(dos), f);
    fwrite(dos_stub, 1, sizeof(dos_stub), f);
    fwrite(&pe_sig, 1, sizeof(pe_sig), f);
    fwrite(&coff, 1, sizeof(coff), f);
    fwrite(&opt, 1, sizeof(opt), f);
    fwrite(sec, 1, sizeof(sec), f);

    /* Pad headers block to headers_size_aligned */
    size_t written_hdr = sizeof(dos) + sizeof(dos_stub) + sizeof(pe_sig) +
                         sizeof(coff) + sizeof(opt) + sizeof(sec);
    while (written_hdr < headers_size_aligned) {
        fputc(0, f);
        written_hdr++;
    }

    /* Write .text */
    fwrite(full_text, 1, total_text_size, f);
    for (size_t i = total_text_size; i < text_raw_size; i++) fputc(0x90, f); /* nop pad */

    /* Write .rdata */
    if (rodata && rodata_size > 0) fwrite(rodata, 1, rodata_size, f);
    for (size_t i = rodata_size; i < rdata_raw_size; i++) fputc(0, f);

    /* Write .data */
    if (data && data_size > 0) fwrite(data, 1, data_size, f);
    for (size_t i = data_size; i < data_raw_size; i++) fputc(0, f);

    /* Write .idata */
    fwrite(idata_buf, 1, idata_raw_size, f);

    fclose(f);
    free(idata_buf);
    return true;
}

bool zero_cc_pe_write_exe(const char *filename,
                          const uint8_t *text, size_t text_size,
                          const uint8_t *rodata, size_t rodata_size,
                          const uint8_t *data, size_t data_size,
                          size_t bss_size,
                          uint32_t entry_text_offset)
{
    if (!filename) return false;

    size_t stub_size = 32;
    size_t total_text_size = stub_size + text_size;
    uint8_t *full_text = (uint8_t *)calloc(1, total_text_size > 0 ? total_text_size : 32);
    if (!full_text) return false;

    uint32_t sec_align = ZERO_CC_PE_SECTION_ALIGNMENT;
    uint32_t text_rva = sec_align;
    uint32_t text_virt_size = pe_align_up((uint32_t)total_text_size, sec_align);
    uint32_t rdata_rva = text_rva + text_virt_size;
    uint32_t rdata_virt_size = pe_align_up((uint32_t)(rodata_size > 0 ? rodata_size : 1), sec_align);
    uint32_t data_rva = rdata_rva + rdata_virt_size;
    uint32_t data_virt_size = pe_align_up((uint32_t)(data_size + bss_size > 0 ? data_size + bss_size : 1), sec_align);
    uint32_t idata_rva = data_rva + data_virt_size;
    uint32_t off_iat = 56;

    /* Assemble CRT stub:
     * sub rsp, 40
     * call <main>
     * mov ecx, eax
     * call [rip + iat_disp]
     * ret
     */
    full_text[0] = 0x48; full_text[1] = 0x83; full_text[2] = 0xEC; full_text[3] = 0x28;
    full_text[4] = 0xE8;
    int32_t call_main_disp = (int32_t)(stub_size + entry_text_offset - 9);
    memcpy(full_text + 5, &call_main_disp, 4);
    full_text[9] = 0x89; full_text[10] = 0xC1;
    full_text[11] = 0xFF; full_text[12] = 0x15;
    int32_t iat_disp = (int32_t)((idata_rva + off_iat) - (text_rva + 17));
    memcpy(full_text + 13, &iat_disp, 4);
    full_text[17] = 0xC3;

    if (text && text_size > 0) {
        memcpy(full_text + stub_size, text, text_size);
    }

    bool ok = zero_cc_pe_write_exe_internal(filename,
                                            full_text, total_text_size,
                                            rodata, rodata_size,
                                            data, data_size,
                                            bss_size,
                                            text_rva);
    free(full_text);
    return ok;
}

bool zero_cc_pe_write_exe_from_comp(const char *filename, ZeroCCCompiler *comp) {
    if (!filename || !comp) return false;

    size_t bss_size = 0;
    for (size_t i = 0; i < comp->global_var_count; i++) {
        ZeroCCGlobalVar *gv = &comp->global_vars[i];
        if (gv->is_bss && !gv->is_tls && !gv->lto_eliminated) {
            size_t end = gv->data_offset + gv->size;
            if (end > bss_size) bss_size = end;
        }
    }

    uint32_t main_offset = 0;
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *f = &comp->functions[i];
        if (strcmp(f->name, "main") == 0) {
            main_offset = (uint32_t)f->text_offset;
            break;
        }
    }

    size_t stub_size = 32;
    size_t total_text_size = stub_size + comp->text_section.length;
    uint8_t *full_text = (uint8_t *)calloc(1, total_text_size > 0 ? total_text_size : 32);
    if (!full_text) return false;

    if (comp->text_section.data && comp->text_section.length > 0) {
        memcpy(full_text + stub_size, comp->text_section.data, comp->text_section.length);
    }

    uint32_t sec_align = ZERO_CC_PE_SECTION_ALIGNMENT;
    uint32_t text_rva = sec_align;
    uint32_t text_virt_size = pe_align_up((uint32_t)total_text_size, sec_align);

    uint32_t rdata_rva = text_rva + text_virt_size;
    uint32_t rdata_virt_size = pe_align_up((uint32_t)(comp->rodata_section.length > 0 ? comp->rodata_section.length : 1), sec_align);

    uint32_t data_rva = rdata_rva + rdata_virt_size;
    uint32_t data_virt_size = pe_align_up((uint32_t)(comp->data_section.length + bss_size > 0 ? comp->data_section.length + bss_size : 1), sec_align);
    uint32_t idata_rva = data_rva + data_virt_size;
    uint32_t off_iat = 56;

    /* Assemble CRT stub */
    full_text[0] = 0x48; full_text[1] = 0x83; full_text[2] = 0xEC; full_text[3] = 0x28;
    full_text[4] = 0xE8;
    int32_t call_main_disp = (int32_t)(stub_size + main_offset - 9);
    memcpy(full_text + 5, &call_main_disp, 4);
    full_text[9] = 0x89; full_text[10] = 0xC1;
    full_text[11] = 0xFF; full_text[12] = 0x15;
    int32_t iat_disp = (int32_t)((idata_rva + off_iat) - (text_rva + 17));
    memcpy(full_text + 13, &iat_disp, 4);
    full_text[17] = 0xC3;

    /* Apply relocations */
    for (size_t i = 0; i < comp->reloc_count; i++) {
        ZeroCCReloc *r = &comp->relocs[i];
        if (r->is_call) {
            const char *sym = r->target_symbol;
            const char *alias = (strncmp(sym, "__builtin_", 10) == 0) ? sym + 10 : NULL;
            ZeroCCFunction *tgt = zero_cc_find_function_by_symbol(comp, sym);
            if ((!tgt || tgt->is_prototype) && alias) tgt = zero_cc_find_function_by_symbol(comp, alias);
            if (tgt && !tgt->is_prototype) {
                int32_t rel = (int32_t)(tgt->text_offset - (r->text_patch_offset + 4));
                memcpy(full_text + stub_size + r->text_patch_offset, &rel, 4);
            }
        } else if (strncmp(r->target_symbol, "__data_", 7) == 0) {
            ZeroCCGlobalVar *gv = find_global_var(comp, r->target_symbol + 7);
            if (gv) {
                uint32_t target_rva = data_rva + (uint32_t)gv->data_offset;
                if (gv->is_bss) target_rva += (uint32_t)comp->data_section.length;
                int32_t rel = (int32_t)(target_rva - (text_rva + stub_size + r->text_patch_offset + 4));
                memcpy(full_text + stub_size + r->text_patch_offset, &rel, 4);
            }
        } else {
            ZeroCCGlobalVar *gv = find_global_var(comp, r->target_symbol);
            if (gv) {
                uint64_t va = ZERO_CC_PE_DEFAULT_IMAGE_BASE + data_rva + (uint64_t)gv->data_offset;
                if (gv->is_bss) va += (uint64_t)comp->data_section.length;
                memcpy(full_text + stub_size + r->text_patch_offset, &va, 8);
            }
        }
    }

    for (size_t i = 0; i < comp->str_reloc_count; i++) {
        ZeroCCStringReloc *sr = &comp->str_relocs[i];
        uint32_t target_rva = rdata_rva + (uint32_t)sr->rodata_offset;
        uint32_t rip_at_next = text_rva + (uint32_t)stub_size + (uint32_t)sr->text_patch_offset + 4;
        int32_t rel = (int32_t)(target_rva - rip_at_next);
        memcpy(full_text + stub_size + sr->text_patch_offset, &rel, 4);
    }

    bool ok = zero_cc_pe_write_exe_internal(filename,
                                            full_text, total_text_size,
                                            (const uint8_t*)comp->rodata_section.data, comp->rodata_section.length,
                                            (const uint8_t*)comp->data_section.data, comp->data_section.length,
                                            bss_size,
                                            text_rva);
    free(full_text);
    return ok;
}

bool zero_cc_pe_write_coff_obj(const char *filename,
                               const uint8_t *text, size_t text_size,
                               const uint8_t *rodata, size_t rodata_size,
                               const uint8_t *data, size_t data_size)
{
    if (!filename) return false;

    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    uint16_t num_sections = 3; /* .text, .data, .rdata */
    ZeroCCCoffFileHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.machine = ZERO_CC_IMAGE_FILE_MACHINE_AMD64;
    hdr.number_of_sections = num_sections;
    hdr.time_date_stamp = (uint32_t)time(NULL);
    hdr.size_of_optional_header = 0;
    hdr.characteristics = 0;

    uint32_t header_size = (uint32_t)(sizeof(ZeroCCCoffFileHeader) +
                                      num_sections * sizeof(ZeroCCSectionHeader));
    uint32_t text_offset = header_size;
    uint32_t data_offset = text_offset + (uint32_t)text_size;
    uint32_t rdata_offset = data_offset + (uint32_t)data_size;

    ZeroCCSectionHeader sec[3];
    memset(sec, 0, sizeof(sec));

    /* .text */
    memcpy(sec[0].name, ".text\0\0\0", 8);
    sec[0].size_of_raw_data = (uint32_t)text_size;
    sec[0].pointer_to_raw_data = (text_size > 0) ? text_offset : 0;
    sec[0].characteristics = ZERO_CC_IMAGE_SCN_CNT_CODE |
                             ZERO_CC_IMAGE_SCN_MEM_EXECUTE |
                             ZERO_CC_IMAGE_SCN_MEM_READ;

    /* .data */
    memcpy(sec[1].name, ".data\0\0\0", 8);
    sec[1].size_of_raw_data = (uint32_t)data_size;
    sec[1].pointer_to_raw_data = (data_size > 0) ? data_offset : 0;
    sec[1].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA |
                             ZERO_CC_IMAGE_SCN_MEM_READ |
                             ZERO_CC_IMAGE_SCN_MEM_WRITE;

    /* .rdata */
    memcpy(sec[2].name, ".rdata\0\0", 8);
    sec[2].size_of_raw_data = (uint32_t)rodata_size;
    sec[2].pointer_to_raw_data = (rodata_size > 0) ? rdata_offset : 0;
    sec[2].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA |
                             ZERO_CC_IMAGE_SCN_MEM_READ;

    fwrite(&hdr, 1, sizeof(hdr), f);
    fwrite(sec, 1, sizeof(sec), f);

    if (text && text_size > 0) fwrite(text, 1, text_size, f);
    if (data && data_size > 0) fwrite(data, 1, data_size, f);
    if (rodata && rodata_size > 0) fwrite(rodata, 1, rodata_size, f);

    fclose(f);
    return true;
}

static uint32_t add_coff_symbol(ZeroCCCoffSymbol *syms, uint32_t *sym_count,
                                char *strtab, uint32_t *strtab_size,
                                const char *name, uint32_t value,
                                int16_t section, uint16_t type, uint8_t storage_class)
{
    uint32_t idx = (*sym_count)++;
    ZeroCCCoffSymbol *s = &syms[idx];
    memset(s, 0, sizeof(*s));
    s->value = value;
    s->section_number = section;
    s->type = type;
    s->storage_class = storage_class;

    size_t len = strlen(name);
    if (len <= 8) {
        memcpy(s->name.short_name, name, len);
    } else {
        s->name.long_name.zeroes = 0;
        s->name.long_name.offset = *strtab_size;
        memcpy(strtab + *strtab_size, name, len + 1);
        *strtab_size += (uint32_t)(len + 1);
    }
    return idx;
}

bool zero_cc_pe_write_coff_obj_from_comp(const char *filename, ZeroCCCompiler *comp) {
    if (!filename || !comp) return false;

    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    uint16_t num_sections = 3; /* .text, .data, .rdata */
    ZeroCCCoffFileHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.machine = ZERO_CC_IMAGE_FILE_MACHINE_AMD64;
    hdr.number_of_sections = num_sections;
    hdr.time_date_stamp = (uint32_t)time(NULL);
    hdr.size_of_optional_header = 0;
    hdr.characteristics = 0;

    #define MAX_COFF_SYMS 2048
    #define MAX_COFF_RELOCS 4096
    ZeroCCCoffSymbol syms[MAX_COFF_SYMS];
    uint32_t sym_count = 0;
    char strtab[65536];
    uint32_t strtab_size = 4;

    /* Section symbols: .text (1), .data (2), .rdata (3) */
    add_coff_symbol(syms, &sym_count, strtab, &strtab_size, ".text", 0, 1, 0, ZERO_CC_IMAGE_SYM_CLASS_STATIC);
    add_coff_symbol(syms, &sym_count, strtab, &strtab_size, ".data", 0, 2, 0, ZERO_CC_IMAGE_SYM_CLASS_STATIC);
    add_coff_symbol(syms, &sym_count, strtab, &strtab_size, ".rdata", 0, 3, 0, ZERO_CC_IMAGE_SYM_CLASS_STATIC);

    /* Defined function symbols */
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *fn = &comp->functions[i];
        if (fn->name[0] && !fn->is_prototype && !fn->is_external && !fn->lto_eliminated) {
            add_coff_symbol(syms, &sym_count, strtab, &strtab_size,
                            fn->name, (uint32_t)fn->text_offset, 1, 0x20, ZERO_CC_IMAGE_SYM_CLASS_EXTERNAL);
        }
    }

    /* Global variable symbols */
    for (size_t i = 0; i < comp->global_var_count; i++) {
        ZeroCCGlobalVar *gv = &comp->global_vars[i];
        if (gv->name[0] && !gv->alias_target[0] && !gv->lto_eliminated) {
            int16_t sec_num = gv->type.is_const ? 3 : 2;
            add_coff_symbol(syms, &sym_count, strtab, &strtab_size,
                            gv->name, (uint32_t)gv->data_offset, sec_num, 0, ZERO_CC_IMAGE_SYM_CLASS_EXTERNAL);
        }
    }

    /* Relocations for .text */
    ZeroCCCoffReloc text_relocs[MAX_COFF_RELOCS];
    uint32_t text_reloc_count = 0;

    /* String relocations (.rdata symbol is index 2) */
    for (size_t i = 0; i < comp->str_reloc_count && text_reloc_count < MAX_COFF_RELOCS; i++) {
        ZeroCCStringReloc *sr = &comp->str_relocs[i];
        text_relocs[text_reloc_count].virtual_address = (uint32_t)sr->text_patch_offset;
        text_relocs[text_reloc_count].symbol_table_index = 2;
        text_relocs[text_reloc_count].type = ZERO_CC_IMAGE_REL_AMD64_REL32;
        text_reloc_count++;
    }

    /* Relocations from comp->relocs */
    for (size_t i = 0; i < comp->reloc_count && text_reloc_count < MAX_COFF_RELOCS; i++) {
        ZeroCCReloc *r = &comp->relocs[i];
        const char *target = r->target_symbol;
        if (strncmp(target, "__data_", 7) == 0) target += 7;

        /* Find symbol in syms */
        uint32_t target_sym_idx = 0;
        bool found = false;
        for (uint32_t s = 0; s < sym_count; s++) {
            const char *sname = (syms[s].name.long_name.zeroes == 0 && syms[s].name.long_name.offset >= 4)
                                ? strtab + syms[s].name.long_name.offset
                                : syms[s].name.short_name;
            if (strncmp(sname, target, strlen(target)) == 0) {
                target_sym_idx = s;
                found = true;
                break;
            }
        }
        if (!found) {
            target_sym_idx = add_coff_symbol(syms, &sym_count, strtab, &strtab_size,
                                             target, 0, 0, r->is_call ? 0x20 : 0, ZERO_CC_IMAGE_SYM_CLASS_EXTERNAL);
        }

        text_relocs[text_reloc_count].virtual_address = (uint32_t)r->text_patch_offset;
        text_relocs[text_reloc_count].symbol_table_index = target_sym_idx;
        text_relocs[text_reloc_count].type = r->is_call ? ZERO_CC_IMAGE_REL_AMD64_REL32 : ZERO_CC_IMAGE_REL_AMD64_ADDR64;
        text_reloc_count++;
    }

    /* Calculate layout */
    uint32_t header_size = (uint32_t)(sizeof(ZeroCCCoffFileHeader) + num_sections * sizeof(ZeroCCSectionHeader));
    uint32_t text_offset = header_size;
    uint32_t text_size = (uint32_t)comp->text_section.length;
    uint32_t text_relocs_offset = text_offset + text_size;
    uint32_t text_relocs_size = text_reloc_count * (uint32_t)sizeof(ZeroCCCoffReloc);

    uint32_t data_offset = text_relocs_offset + text_relocs_size;
    uint32_t data_size = (uint32_t)comp->data_section.length;

    uint32_t rdata_offset = data_offset + data_size;
    uint32_t rdata_size = (uint32_t)comp->rodata_section.length;

    uint32_t symtab_offset = rdata_offset + rdata_size;

    /* Fill string table size in first 4 bytes */
    memcpy(strtab, &strtab_size, 4);

    hdr.pointer_to_symbol_table = symtab_offset;
    hdr.number_of_symbols = sym_count;

    ZeroCCSectionHeader sec[3];
    memset(sec, 0, sizeof(sec));

    /* .text */
    memcpy(sec[0].name, ".text\0\0\0", 8);
    sec[0].size_of_raw_data = text_size;
    sec[0].pointer_to_raw_data = (text_size > 0) ? text_offset : 0;
    sec[0].pointer_to_relocations = (text_reloc_count > 0) ? text_relocs_offset : 0;
    sec[0].number_of_relocations = (uint16_t)text_reloc_count;
    sec[0].characteristics = ZERO_CC_IMAGE_SCN_CNT_CODE |
                             ZERO_CC_IMAGE_SCN_MEM_EXECUTE |
                             ZERO_CC_IMAGE_SCN_MEM_READ;

    /* .data */
    memcpy(sec[1].name, ".data\0\0\0", 8);
    sec[1].size_of_raw_data = data_size;
    sec[1].pointer_to_raw_data = (data_size > 0) ? data_offset : 0;
    sec[1].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA |
                             ZERO_CC_IMAGE_SCN_MEM_READ |
                             ZERO_CC_IMAGE_SCN_MEM_WRITE;

    /* .rdata */
    memcpy(sec[2].name, ".rdata\0\0", 8);
    sec[2].size_of_raw_data = rdata_size;
    sec[2].pointer_to_raw_data = (rdata_size > 0) ? rdata_offset : 0;
    sec[2].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA |
                             ZERO_CC_IMAGE_SCN_MEM_READ;

    fwrite(&hdr, 1, sizeof(hdr), f);
    fwrite(sec, 1, sizeof(sec), f);

    if (comp->text_section.data && text_size > 0) {
        fwrite(comp->text_section.data, 1, text_size, f);
    }
    if (text_reloc_count > 0) {
        fwrite(text_relocs, sizeof(ZeroCCCoffReloc), text_reloc_count, f);
    }
    if (comp->data_section.data && data_size > 0) {
        fwrite(comp->data_section.data, 1, data_size, f);
    }
    if (comp->rodata_section.data && rdata_size > 0) {
        fwrite(comp->rodata_section.data, 1, rdata_size, f);
    }
    if (sym_count > 0) {
        fwrite(syms, sizeof(ZeroCCCoffSymbol), sym_count, f);
    }
    if (strtab_size > 4) {
        fwrite(strtab, 1, strtab_size, f);
    }

    fclose(f);
    return true;
}
