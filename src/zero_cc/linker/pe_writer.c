/**
 * @file pe_writer.c
 * @brief PE/COFF (PE32+) Executable and Object File Writer for ZeroCC (Sprint 246)
 */

#include "zero_cc/linker/pe_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zero_cc/linker/coff_reader.h"
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

static const char *pe_get_dll_basename(const char *path) {
    if (!path || !path[0]) return "module.dll";
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *p = path;
    if (slash && slash >= p) p = slash + 1;
    if (bslash && bslash >= p) p = bslash + 1;
    return p;
}

static const char *pe_classify_symbol_dll(const char *sym) {
    if (!sym) return "msvcrt.dll";
    if (strncmp(sym, "__imp_", 6) == 0) sym += 6;
    if (strcmp(sym, "ExitProcess") == 0 ||
        strcmp(sym, "GetModuleHandleA") == 0 ||
        strcmp(sym, "GetModuleHandleW") == 0 ||
        strcmp(sym, "GetProcAddress") == 0 ||
        strcmp(sym, "LoadLibraryA") == 0 ||
        strcmp(sym, "LoadLibraryW") == 0 ||
        strcmp(sym, "VirtualAlloc") == 0 ||
        strcmp(sym, "VirtualFree") == 0 ||
        strcmp(sym, "Sleep") == 0 ||
        strcmp(sym, "GetLastError") == 0 ||
        strcmp(sym, "SetLastError") == 0 ||
        strcmp(sym, "GetStdHandle") == 0 ||
        strcmp(sym, "CreateFileA") == 0 ||
        strcmp(sym, "CloseHandle") == 0 ||
        strcmp(sym, "ReadFile") == 0 ||
        strcmp(sym, "WriteFile") == 0) {
        return "KERNEL32.dll";
    }
    if (strncmp(sym, "MessageBox", 10) == 0 ||
        strncmp(sym, "DefWindowProc", 13) == 0 ||
        strncmp(sym, "PostQuit", 8) == 0) {
        return "USER32.dll";
    }
    return "msvcrt.dll";
}

typedef struct {
    char name[128];
    uint32_t rva_target_kind; /* 0 = function text_offset, 1 = global data_offset */
    uint32_t offset;
    bool is_bss;
} PeExportItem;

static int compare_export_items(const void *a, const void *b) {
    const PeExportItem *ea = (const PeExportItem *)a;
    const PeExportItem *eb = (const PeExportItem *)b;
    return strcmp(ea->name, eb->name);
}

bool zero_cc_pe_write_binary_from_comp(const char *filename,
                                       ZeroCCCompiler *comp,
                                       const ZeroCCPeOptions *opts)
{
    if (!filename || !comp) return false;

    bool is_dll = opts ? opts->is_dll : false;
    bool is_gui = opts ? opts->is_gui : false;
    const char *dll_name_param = (opts && opts->dll_name) ? opts->dll_name : filename;
    const char *dll_name = pe_get_dll_basename(dll_name_param);

    /* 1. BSS calculation */
    size_t bss_size = 0;
    for (size_t i = 0; i < comp->global_var_count; i++) {
        ZeroCCGlobalVar *gv = &comp->global_vars[i];
        if (gv->is_bss && !gv->is_tls && !gv->lto_eliminated) {
            size_t end = gv->data_offset + gv->size;
            if (end > bss_size) bss_size = end;
        }
    }

    /* 2. Collect Exports (for DLL, or non-static functions if DLL, or __declspec(dllexport)) */
    PeExportItem exports[1024];
    size_t num_exports = 0;
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *f = &comp->functions[i];
        if (!f->name[0] || f->is_prototype || f->is_external || f->is_object || f->lto_eliminated) continue;
        bool do_export = f->is_dllexport || (is_dll && !f->is_static);
        if (do_export && num_exports < 1024) {
            strncpy(exports[num_exports].name, f->name, sizeof(exports[num_exports].name) - 1);
            exports[num_exports].name[sizeof(exports[num_exports].name) - 1] = '\0';
            exports[num_exports].rva_target_kind = 0;
            exports[num_exports].offset = (uint32_t)f->text_offset;
            exports[num_exports].is_bss = false;
            num_exports++;
        }
    }
    for (size_t i = 0; i < comp->global_var_count; i++) {
        ZeroCCGlobalVar *gv = &comp->global_vars[i];
        if (!gv->name[0] || gv->lto_eliminated || gv->is_tls) continue;
        bool do_export = gv->is_dllexport || (is_dll && !gv->is_static);
        if (do_export && num_exports < 1024) {
            strncpy(exports[num_exports].name, gv->name, sizeof(exports[num_exports].name) - 1);
            exports[num_exports].name[sizeof(exports[num_exports].name) - 1] = '\0';
            exports[num_exports].rva_target_kind = 1;
            exports[num_exports].offset = (uint32_t)gv->data_offset;
            exports[num_exports].is_bss = gv->is_bss;
            num_exports++;
        }
    }
    if (num_exports > 1) {
        qsort(exports, num_exports, sizeof(PeExportItem), compare_export_items);
    }
    bool has_edata = is_dll || (num_exports > 0);

    /* 3. Entry point detection */
    uint32_t entry_offset = 0;
    bool entry_found = false;
    if (is_dll) {
        for (size_t i = 0; i < comp->function_count; i++) {
            ZeroCCFunction *f = &comp->functions[i];
            if (strcmp(f->name, "DllMain") == 0) {
                entry_offset = (uint32_t)f->text_offset;
                entry_found = true;
                break;
            }
        }
    } else if (is_gui) {
        for (size_t i = 0; i < comp->function_count; i++) {
            ZeroCCFunction *f = &comp->functions[i];
            if (strcmp(f->name, "WinMain") == 0) {
                entry_offset = (uint32_t)f->text_offset;
                entry_found = true;
                break;
            }
        }
        if (!entry_found) {
            for (size_t i = 0; i < comp->function_count; i++) {
                ZeroCCFunction *f = &comp->functions[i];
                if (strcmp(f->name, "main") == 0) {
                    entry_offset = (uint32_t)f->text_offset;
                    entry_found = true;
                    break;
                }
            }
        }
    } else {
        for (size_t i = 0; i < comp->function_count; i++) {
            ZeroCCFunction *f = &comp->functions[i];
            if (strcmp(f->name, "main") == 0) {
                entry_offset = (uint32_t)f->text_offset;
                entry_found = true;
                break;
            }
        }
        if (!entry_found) {
            for (size_t i = 0; i < comp->function_count; i++) {
                ZeroCCFunction *f = &comp->functions[i];
                if (strcmp(f->name, "WinMain") == 0) {
                    entry_offset = (uint32_t)f->text_offset;
                    entry_found = true;
                    break;
                }
            }
        }
    }

    /* 4. Collect Imports */
    typedef struct {
        char name[128];
        const char *dll;
        uint32_t iat_rva;
        uint32_t thunk_offset;
        uint32_t hint_name_off;
    } PeImportEntry;

    PeImportEntry imports[256];
    size_t num_imports = 0;

    if (!is_dll) {
        snprintf(imports[num_imports].name, sizeof(imports[num_imports].name), "ExitProcess");
        imports[num_imports].dll = "KERNEL32.dll";
        num_imports++;
    }

    for (size_t i = 0; i < comp->reloc_count; i++) {
        ZeroCCReloc *r = &comp->relocs[i];
        if (r->is_call || r->is_abs_call) {
            const char *sym = r->target_symbol;
            if (strncmp(sym, "__imp_", 6) == 0) sym += 6;
            const char *alias = (strncmp(sym, "__builtin_", 10) == 0) ? sym + 10 : NULL;
            ZeroCCFunction *tgt = zero_cc_find_function_by_symbol(comp, sym);
            if ((!tgt || tgt->is_prototype) && alias) tgt = zero_cc_find_function_by_symbol(comp, alias);
            if (!tgt || tgt->is_prototype || tgt->is_dllimport) {
                bool found = false;
                for (size_t j = 0; j < num_imports; j++) {
                    if (strcmp(imports[j].name, sym) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found && num_imports < 256) {
                    strncpy(imports[num_imports].name, sym, sizeof(imports[num_imports].name) - 1);
                    imports[num_imports].name[sizeof(imports[num_imports].name) - 1] = '\0';
                    imports[num_imports].dll = pe_classify_symbol_dll(sym);
                    num_imports++;
                }
            }
        } else if (strncmp(r->target_symbol, "__imp_", 6) == 0) {
            const char *sym = r->target_symbol + 6;
            bool found = false;
            for (size_t j = 0; j < num_imports; j++) {
                if (strcmp(imports[j].name, sym) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found && num_imports < 256) {
                strncpy(imports[num_imports].name, sym, sizeof(imports[num_imports].name) - 1);
                imports[num_imports].name[sizeof(imports[num_imports].name) - 1] = '\0';
                imports[num_imports].dll = pe_classify_symbol_dll(sym);
                num_imports++;
            }
        }
    }

    /* Group imports by DLL */
    const char *dll_names[16];
    size_t num_dlls = 0;
    size_t dll_import_indices[16][256];
    size_t dll_import_counts[16];
    memset(dll_import_counts, 0, sizeof(dll_import_counts));

    for (size_t i = 0; i < num_imports; i++) {
        int d_idx = -1;
        for (size_t d = 0; d < num_dlls; d++) {
            if (strcmp(dll_names[d], imports[i].dll) == 0) {
                d_idx = (int)d;
                break;
            }
        }
        if (d_idx < 0 && num_dlls < 16) {
            d_idx = (int)num_dlls;
            dll_names[num_dlls++] = imports[i].dll;
        }
        if (d_idx >= 0 && dll_import_counts[d_idx] < 256) {
            dll_import_indices[d_idx][dll_import_counts[d_idx]++] = i;
        }
    }

    /* 5. Section and Header Sizing */
    uint16_t num_sections = has_edata ? 5 : 4;
    uint32_t file_align = ZERO_CC_PE_FILE_ALIGNMENT;
    uint32_t sec_align = ZERO_CC_PE_SECTION_ALIGNMENT;

    size_t stub_size = is_dll ? 16 : 32;
    size_t thunks_size = num_imports * 8;
    size_t total_text_size = stub_size + comp->text_section.length + thunks_size;

    uint32_t raw_headers_size = (uint32_t)(sizeof(ZeroCCDosHeader) + 64 + 4 +
                                           sizeof(ZeroCCCoffFileHeader) +
                                           sizeof(ZeroCCOptionalHeader64) +
                                           num_sections * sizeof(ZeroCCSectionHeader));
    uint32_t headers_size_aligned = pe_align_up(raw_headers_size, file_align);

    uint32_t text_rva = sec_align;
    uint32_t text_raw_off = headers_size_aligned;
    uint32_t text_raw_size = pe_align_up((uint32_t)total_text_size, file_align);
    uint32_t text_virt_size = pe_align_up((uint32_t)total_text_size, sec_align);

    uint32_t rdata_rva = text_rva + text_virt_size;
    uint32_t rdata_raw_off = text_raw_off + text_raw_size;
    uint32_t rdata_raw_size = pe_align_up((uint32_t)comp->rodata_section.length, file_align);
    uint32_t rdata_virt_size = pe_align_up((uint32_t)(comp->rodata_section.length > 0 ? comp->rodata_section.length : 1), sec_align);

    uint32_t data_rva = rdata_rva + rdata_virt_size;
    uint32_t data_raw_off = rdata_raw_off + rdata_raw_size;
    uint32_t data_raw_size = pe_align_up((uint32_t)comp->data_section.length, file_align);
    uint32_t data_virt_size = pe_align_up((uint32_t)(comp->data_section.length + bss_size > 0 ? comp->data_section.length + bss_size : 1), sec_align);

    uint32_t idata_rva = data_rva + data_virt_size;
    uint32_t idata_raw_off = data_raw_off + data_raw_size;

    /* 6. Lay out .idata */
    uint32_t cur_idata_off = (uint32_t)((num_dlls + 1) * sizeof(ZeroCCImportDescriptor));
    uint32_t off_ilt[16];
    uint32_t off_iat[16];
    for (size_t d = 0; d < num_dlls; d++) {
        off_ilt[d] = cur_idata_off;
        cur_idata_off += (uint32_t)((dll_import_counts[d] + 1) * sizeof(uint64_t));
    }
    for (size_t d = 0; d < num_dlls; d++) {
        off_iat[d] = cur_idata_off;
        cur_idata_off += (uint32_t)((dll_import_counts[d] + 1) * sizeof(uint64_t));
    }
    for (size_t i = 0; i < num_imports; i++) {
        imports[i].hint_name_off = cur_idata_off;
        uint32_t hsz = 2 + (uint32_t)strlen(imports[i].name) + 1;
        if (hsz & 1) hsz++; /* 2-byte alignment */
        cur_idata_off += hsz;
    }
    uint32_t off_dll_name[16];
    for (size_t d = 0; d < num_dlls; d++) {
        off_dll_name[d] = cur_idata_off;
        cur_idata_off += (uint32_t)(strlen(dll_names[d]) + 1);
    }

    uint32_t idata_raw_size = pe_align_up(cur_idata_off > 0 ? cur_idata_off : file_align, file_align);
    uint32_t idata_virt_size = pe_align_up(cur_idata_off > 0 ? cur_idata_off : sec_align, sec_align);
    uint8_t *idata_buf = (uint8_t *)calloc(1, idata_raw_size);
    if (!idata_buf) return false;

    /* Populate .idata */
    for (size_t d = 0; d < num_dlls; d++) {
        memcpy(idata_buf + off_dll_name[d], dll_names[d], strlen(dll_names[d]) + 1);
    }
    for (size_t i = 0; i < num_imports; i++) {
        idata_buf[imports[i].hint_name_off + 0] = 0;
        idata_buf[imports[i].hint_name_off + 1] = 0;
        memcpy(idata_buf + imports[i].hint_name_off + 2, imports[i].name, strlen(imports[i].name) + 1);
    }
    for (size_t d = 0; d < num_dlls; d++) {
        for (size_t k = 0; k < dll_import_counts[d]; k++) {
            size_t imp_idx = dll_import_indices[d][k];
            uint64_t entry_rva = (uint64_t)(idata_rva + imports[imp_idx].hint_name_off);
            memcpy(idata_buf + off_ilt[d] + k * 8, &entry_rva, sizeof(uint64_t));
            memcpy(idata_buf + off_iat[d] + k * 8, &entry_rva, sizeof(uint64_t));
            imports[imp_idx].iat_rva = idata_rva + off_iat[d] + (uint32_t)(k * 8);
        }
        ZeroCCImportDescriptor *desc = (ZeroCCImportDescriptor *)(idata_buf + d * sizeof(ZeroCCImportDescriptor));
        desc->original_first_thunk = idata_rva + off_ilt[d];
        desc->time_date_stamp = 0;
        desc->forwarder_chain = 0;
        desc->name_rva = idata_rva + off_dll_name[d];
        desc->first_thunk = idata_rva + off_iat[d];
    }

    /* 7. Lay out .edata (if DLL) */
    uint32_t edata_rva = 0;
    uint32_t edata_raw_off = 0;
    uint32_t edata_raw_size = 0;
    uint32_t edata_virt_size = 0;
    uint8_t *edata_buf = NULL;
    uint32_t total_edata_bytes = 0;

    if (has_edata) {
        edata_rva = idata_rva + idata_virt_size;
        edata_raw_off = idata_raw_off + idata_raw_size;

        uint32_t off_eat = 40;
        uint32_t off_ent = off_eat + (uint32_t)(num_exports * 4);
        uint32_t off_eot = off_ent + (uint32_t)(num_exports * 4);
        uint32_t off_edata_dll_name = off_eot + (uint32_t)(num_exports * 2);
        uint32_t cur_str = off_edata_dll_name + (uint32_t)(strlen(dll_name) + 1);

        uint32_t exp_name_offsets[1024];
        for (size_t i = 0; i < num_exports; i++) {
            exp_name_offsets[i] = cur_str;
            cur_str += (uint32_t)(strlen(exports[i].name) + 1);
        }

        total_edata_bytes = cur_str;
        edata_raw_size = pe_align_up(total_edata_bytes > 0 ? total_edata_bytes : file_align, file_align);
        edata_virt_size = pe_align_up(total_edata_bytes > 0 ? total_edata_bytes : sec_align, sec_align);
        edata_buf = (uint8_t *)calloc(1, edata_raw_size);
        if (!edata_buf) {
            free(idata_buf);
            return false;
        }

        ZeroCCExportDirectory *edir = (ZeroCCExportDirectory *)edata_buf;
        edir->characteristics = 0;
        edir->time_date_stamp = (uint32_t)time(NULL);
        edir->major_version = 0;
        edir->minor_version = 0;
        edir->name_rva = edata_rva + off_edata_dll_name;
        edir->ordinal_base = 1;
        edir->number_of_functions = (uint32_t)num_exports;
        edir->number_of_names = (uint32_t)num_exports;
        edir->address_of_functions = edata_rva + off_eat;
        edir->address_of_names = edata_rva + off_ent;
        edir->address_of_name_ordinals = edata_rva + off_eot;

        memcpy(edata_buf + off_edata_dll_name, dll_name, strlen(dll_name) + 1);

        for (size_t i = 0; i < num_exports; i++) {
            uint32_t sym_rva = 0;
            if (exports[i].rva_target_kind == 0) {
                sym_rva = text_rva + (uint32_t)stub_size + exports[i].offset;
            } else {
                sym_rva = data_rva + exports[i].offset;
                if (exports[i].is_bss) sym_rva += (uint32_t)comp->data_section.length;
            }
            memcpy(edata_buf + off_eat + i * 4, &sym_rva, 4);

            uint32_t name_rva = edata_rva + exp_name_offsets[i];
            memcpy(edata_buf + off_ent + i * 4, &name_rva, 4);

            uint16_t ordinal = (uint16_t)i;
            memcpy(edata_buf + off_eot + i * 2, &ordinal, 2);

            memcpy(edata_buf + exp_name_offsets[i], exports[i].name, strlen(exports[i].name) + 1);
        }
    }

    uint32_t image_size = has_edata ? (edata_rva + edata_virt_size) : (idata_rva + idata_virt_size);

    /* 8. Build .text */
    uint8_t *full_text = (uint8_t *)calloc(1, total_text_size > 0 ? total_text_size : 32);
    if (!full_text) {
        free(idata_buf);
        if (edata_buf) free(edata_buf);
        return false;
    }

    if (is_dll) {
        if (entry_found) {
            full_text[0] = 0xE8;
            int32_t disp = (int32_t)(stub_size + entry_offset - 5);
            memcpy(full_text + 1, &disp, 4);
            full_text[5] = 0xC3;
        } else {
            full_text[0] = 0xB8; full_text[1] = 0x01; /* mov eax, 1; ret */
            full_text[5] = 0xC3;
        }
    } else {
        full_text[0] = 0x48; full_text[1] = 0x83; full_text[2] = 0xEC; full_text[3] = 0x28;
        full_text[4] = 0xE8;
        int32_t call_main_disp = (int32_t)(stub_size + entry_offset - 9);
        memcpy(full_text + 5, &call_main_disp, 4);
        full_text[9] = 0x89; full_text[10] = 0xC1;
        full_text[11] = 0xFF; full_text[12] = 0x15;
        size_t exit_idx = 0;
        for (size_t j = 0; j < num_imports; j++) {
            if (strcmp(imports[j].name, "ExitProcess") == 0) { exit_idx = j; break; }
        }
        int32_t iat_disp = (int32_t)(imports[exit_idx].iat_rva - (text_rva + 17));
        memcpy(full_text + 13, &iat_disp, 4);
        full_text[17] = 0xC3;
    }

    if (comp->text_section.data && comp->text_section.length > 0) {
        memcpy(full_text + stub_size, comp->text_section.data, comp->text_section.length);
    }

    /* Build thunks */
    for (size_t k = 0; k < num_imports; k++) {
        uint32_t thunk_off = (uint32_t)(stub_size + comp->text_section.length + k * 8);
        imports[k].thunk_offset = thunk_off;
        full_text[thunk_off + 0] = 0xFF;
        full_text[thunk_off + 1] = 0x25;
        int32_t disp = (int32_t)(imports[k].iat_rva - (text_rva + thunk_off + 6));
        memcpy(full_text + thunk_off + 2, &disp, 4);
        full_text[thunk_off + 6] = 0x90;
        full_text[thunk_off + 7] = 0x90;
    }

    /* Apply relocations */
    for (size_t i = 0; i < comp->reloc_count; i++) {
        ZeroCCReloc *r = &comp->relocs[i];
        if (r->is_call) {
            const char *sym = r->target_symbol;
            const char *lookup_sym = sym;
            if (strncmp(lookup_sym, "__imp_", 6) == 0) lookup_sym += 6;
            const char *alias = (strncmp(lookup_sym, "__builtin_", 10) == 0) ? lookup_sym + 10 : NULL;
            ZeroCCFunction *tgt = zero_cc_find_function_by_symbol(comp, lookup_sym);
            if ((!tgt || tgt->is_prototype) && alias) tgt = zero_cc_find_function_by_symbol(comp, alias);
            if (tgt && !tgt->is_prototype && !tgt->is_dllimport) {
                int32_t rel = (int32_t)(tgt->text_offset - (r->text_patch_offset + 4));
                memcpy(full_text + stub_size + r->text_patch_offset, &rel, 4);
            } else {
                for (size_t k = 0; k < num_imports; k++) {
                    if (strcmp(imports[k].name, lookup_sym) == 0) {
                        int32_t rel = (int32_t)((imports[k].thunk_offset - stub_size) - (r->text_patch_offset + 4));
                        memcpy(full_text + stub_size + r->text_patch_offset, &rel, 4);
                        break;
                    }
                }
            }
        } else if (r->is_abs_call) {
            const char *sym = r->target_symbol;
            const char *lookup_sym = sym;
            if (strncmp(lookup_sym, "__imp_", 6) == 0) lookup_sym += 6;
            const char *alias = (strncmp(lookup_sym, "__builtin_", 10) == 0) ? lookup_sym + 10 : NULL;
            ZeroCCFunction *tgt = zero_cc_find_function_by_symbol(comp, lookup_sym);
            if ((!tgt || tgt->is_prototype) && alias) tgt = zero_cc_find_function_by_symbol(comp, alias);
            if (tgt && !tgt->is_prototype && !tgt->is_dllimport) {
                uint64_t va = ZERO_CC_PE_DEFAULT_IMAGE_BASE + text_rva + (uint64_t)stub_size + (uint64_t)tgt->text_offset;
                memcpy(full_text + stub_size + r->text_patch_offset, &va, 8);
            } else {
                for (size_t k = 0; k < num_imports; k++) {
                    if (strcmp(imports[k].name, lookup_sym) == 0) {
                        uint64_t va = ZERO_CC_PE_DEFAULT_IMAGE_BASE + text_rva + (uint64_t)imports[k].thunk_offset;
                        memcpy(full_text + stub_size + r->text_patch_offset, &va, 8);
                        break;
                    }
                }
            }
        } else if (strncmp(r->target_symbol, "__data_", 7) == 0) {
            ZeroCCGlobalVar *gv = find_global_var(comp, r->target_symbol + 7);
            if (gv) {
                uint32_t target_rva = data_rva + (uint32_t)gv->data_offset;
                if (gv->is_bss) target_rva += (uint32_t)comp->data_section.length;
                int32_t rel = (int32_t)(target_rva - (text_rva + stub_size + r->text_patch_offset + 4));
                memcpy(full_text + stub_size + r->text_patch_offset, &rel, 4);
            }
        } else if (strncmp(r->target_symbol, "__imp_", 6) == 0) {
            const char *sym = r->target_symbol + 6;
            for (size_t k = 0; k < num_imports; k++) {
                if (strcmp(imports[k].name, sym) == 0) {
                    uint64_t va = ZERO_CC_PE_DEFAULT_IMAGE_BASE + imports[k].iat_rva;
                    memcpy(full_text + stub_size + r->text_patch_offset, &va, 8);
                    break;
                }
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

    /* 9. Construct Headers */
    FILE *f = fopen(filename, "wb");
    if (!f) {
        free(full_text);
        free(idata_buf);
        if (edata_buf) free(edata_buf);
        return false;
    }

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
    if (is_dll) coff.characteristics |= ZERO_CC_IMAGE_FILE_DLL;

    ZeroCCOptionalHeader64 opt;
    memset(&opt, 0, sizeof(opt));
    opt.magic = ZERO_CC_PE32PLUS_MAGIC;
    opt.major_linker_version = 1;
    opt.minor_linker_version = 0;
    opt.size_of_code = text_raw_size;
    opt.size_of_initialized_data = rdata_raw_size + data_raw_size + idata_raw_size + edata_raw_size;
    opt.size_of_uninitialized_data = (uint32_t)pe_align_up((uint32_t)bss_size, file_align);
    opt.address_of_entry_point = text_rva;
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
    opt.subsystem = is_gui ? ZERO_CC_IMAGE_SUBSYSTEM_WINDOWS_GUI : ZERO_CC_IMAGE_SUBSYSTEM_WINDOWS_CUI;
    opt.dll_characteristics = 0x8160;
    opt.size_of_stack_reserve = 0x100000;
    opt.size_of_stack_commit  = 0x10000;
    opt.size_of_heap_reserve  = 0x100000;
    opt.size_of_heap_commit   = 0x1000;
    opt.number_of_rva_and_sizes = 16;

    if (has_edata) {
        opt.data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_EXPORT].virtual_address = edata_rva;
        opt.data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_EXPORT].size = total_edata_bytes;
    }
    if (num_dlls > 0) {
        opt.data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_IMPORT].virtual_address = idata_rva;
        opt.data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_IMPORT].size = (uint32_t)((num_dlls + 1) * sizeof(ZeroCCImportDescriptor));

        opt.data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_IAT].virtual_address = idata_rva + off_iat[0];
        uint32_t total_iat_len = (off_iat[num_dlls - 1] + (uint32_t)((dll_import_counts[num_dlls - 1] + 1) * 8)) - off_iat[0];
        opt.data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_IAT].size = total_iat_len;
    }

    ZeroCCSectionHeader sec[5];
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
    sec[1].virtual_size = (uint32_t)(comp->rodata_section.length > 0 ? comp->rodata_section.length : 1);
    sec[1].virtual_address = rdata_rva;
    sec[1].size_of_raw_data = rdata_raw_size;
    sec[1].pointer_to_raw_data = rdata_raw_off;
    sec[1].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA |
                             ZERO_CC_IMAGE_SCN_MEM_READ;

    /* 3. .data */
    memcpy(sec[2].name, ".data\0\0\0", 8);
    sec[2].virtual_size = (uint32_t)(comp->data_section.length + bss_size > 0 ? comp->data_section.length + bss_size : 1);
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

    /* 5. .edata (if DLL) */
    if (has_edata) {
        memcpy(sec[4].name, ".edata\0\0", 8);
        sec[4].virtual_size = edata_virt_size;
        sec[4].virtual_address = edata_rva;
        sec[4].size_of_raw_data = edata_raw_size;
        sec[4].pointer_to_raw_data = edata_raw_off;
        sec[4].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA |
                                 ZERO_CC_IMAGE_SCN_MEM_READ;
    }

    /* Write headers */
    fwrite(&dos, 1, sizeof(dos), f);
    fwrite(dos_stub, 1, sizeof(dos_stub), f);
    fwrite(&pe_sig, 1, sizeof(pe_sig), f);
    fwrite(&coff, 1, sizeof(coff), f);
    fwrite(&opt, 1, sizeof(opt), f);
    fwrite(sec, 1, num_sections * sizeof(ZeroCCSectionHeader), f);

    size_t written_hdr = sizeof(dos) + sizeof(dos_stub) + sizeof(pe_sig) +
                         sizeof(coff) + sizeof(opt) + num_sections * sizeof(ZeroCCSectionHeader);
    while (written_hdr < headers_size_aligned) {
        fputc(0, f);
        written_hdr++;
    }

    /* Write .text */
    fwrite(full_text, 1, total_text_size, f);
    for (size_t i = total_text_size; i < text_raw_size; i++) fputc(0x90, f);

    /* Write .rdata */
    if (comp->rodata_section.data && comp->rodata_section.length > 0) {
        fwrite(comp->rodata_section.data, 1, comp->rodata_section.length, f);
    }
    for (size_t i = comp->rodata_section.length; i < rdata_raw_size; i++) fputc(0, f);

    /* Write .data */
    if (comp->data_section.data && comp->data_section.length > 0) {
        fwrite(comp->data_section.data, 1, comp->data_section.length, f);
    }
    for (size_t i = comp->data_section.length; i < data_raw_size; i++) fputc(0, f);

    /* Write .idata */
    fwrite(idata_buf, 1, idata_raw_size, f);

    /* Write .edata (if DLL) */
    if (has_edata && edata_buf) {
        fwrite(edata_buf, 1, edata_raw_size, f);
    }

    fclose(f);
    free(full_text);
    free(idata_buf);
    if (edata_buf) free(edata_buf);
    return true;
}

bool zero_cc_pe_write_exe_from_comp(const char *filename, ZeroCCCompiler *comp) {
    ZeroCCPeOptions opts;
    memset(&opts, 0, sizeof(opts));
    return zero_cc_pe_write_binary_from_comp(filename, comp, &opts);
}

bool zero_cc_pe_write_dll_from_comp(const char *filename, ZeroCCCompiler *comp) {
    ZeroCCPeOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.is_dll = true;
    opts.dll_name = filename;
    return zero_cc_pe_write_binary_from_comp(filename, comp, &opts);
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
        if (comp->text_section.data && sr->text_patch_offset + 4 <= comp->text_section.length) {
            uint32_t val = (uint32_t)sr->rodata_offset;
            memcpy(comp->text_section.data + sr->text_patch_offset, &val, 4);
        }
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

/* ========================================================================= */
/* Windows PE/COFF Multi-TU Relocatable Object Linker (Sprint 259)           */
/* ========================================================================= */

typedef struct {
    char name[128];
    int sec_type; /* 1 = .text, 2 = .rdata, 3 = .data, 4 = .bss */
    uint32_t offset;
    uint32_t rva;
    bool is_defined;
} PeLinkSym;

typedef struct {
    char name[128];
    char dll[64];
    uint32_t iat_rva;
    uint32_t ilt_rva;
    uint32_t hint_name_rva;
    uint32_t hint_name_offset;
    uint32_t stub_rva;
    uint32_t stub_text_offset;
} PeLinkImport;

typedef struct {
    char name[64];
    size_t first_imp;
    size_t imp_count;
    uint32_t desc_rva;
    uint32_t dll_name_rva;
    uint32_t dll_name_offset;
    uint32_t ilt_rva;
    uint32_t ilt_offset;
    uint32_t iat_rva;
    uint32_t iat_offset;
} PeLinkDll;

bool zero_cc_pe_link_objects(const char *filename,
                             const char **obj_files,
                             size_t obj_count,
                             const ZeroCCPeOptions *opts)
{
    if (!filename || !obj_files || obj_count == 0) return false;

    ZeroCCCoffParsedObject *objs = (ZeroCCCoffParsedObject *)calloc(obj_count, sizeof(ZeroCCCoffParsedObject));
    if (!objs) return false;

    char err[256];
    for (size_t i = 0; i < obj_count; i++) {
        if (!zero_cc_coff_read_object_file(obj_files[i], &objs[i], err, sizeof(err))) {
            fprintf(stderr, "zerocc: error reading COFF object '%s': %s\n", obj_files[i], err);
            for (size_t k = 0; k < i; k++) zero_cc_coff_parsed_object_free(&objs[k]);
            free(objs);
            return false;
        }
    }

    bool is_dll = (opts && opts->is_dll);
    bool is_gui = (opts && opts->is_gui);
    size_t stub_size = is_dll ? 16 : 32;

    uint32_t *text_offs = (uint32_t *)calloc(obj_count, sizeof(uint32_t));
    uint32_t *rdata_offs = (uint32_t *)calloc(obj_count, sizeof(uint32_t));
    uint32_t *data_offs = (uint32_t *)calloc(obj_count, sizeof(uint32_t));
    uint32_t *bss_offs = (uint32_t *)calloc(obj_count, sizeof(uint32_t));

    uint32_t total_text_size = (uint32_t)stub_size;
    uint32_t total_rdata_size = 0;
    uint32_t total_data_size = 0;
    uint32_t total_bss_size = 0;

    for (size_t i = 0; i < obj_count; i++) {
        const ZeroCCCoffSectParsed *st = zero_cc_coff_find_section(&objs[i], ".text");
        if (st && st->size_of_raw_data > 0) {
            total_text_size = pe_align_up(total_text_size, 16);
            text_offs[i] = total_text_size;
            total_text_size += st->size_of_raw_data;
        }
        const ZeroCCCoffSectParsed *sr = zero_cc_coff_find_section(&objs[i], ".rdata");
        if (sr && sr->size_of_raw_data > 0) {
            total_rdata_size = pe_align_up(total_rdata_size, 16);
            rdata_offs[i] = total_rdata_size;
            total_rdata_size += sr->size_of_raw_data;
        }
        const ZeroCCCoffSectParsed *sd = zero_cc_coff_find_section(&objs[i], ".data");
        if (sd && sd->size_of_raw_data > 0) {
            total_data_size = pe_align_up(total_data_size, 16);
            data_offs[i] = total_data_size;
            total_data_size += sd->size_of_raw_data;
        }
        const ZeroCCCoffSectParsed *sb = zero_cc_coff_find_section(&objs[i], ".bss");
        if (sb && sb->size_of_raw_data > 0) {
            total_bss_size = pe_align_up(total_bss_size, 16);
            bss_offs[i] = total_bss_size;
            total_bss_size += sb->size_of_raw_data;
        }
    }

    #define MAX_PE_SYMS 8192
    PeLinkSym *syms = (PeLinkSym *)calloc(MAX_PE_SYMS, sizeof(PeLinkSym));
    size_t sym_count = 0;

    for (size_t i = 0; i < obj_count; i++) {
        for (size_t s = 0; s < objs[i].symbol_count; s++) {
            ZeroCCCoffSymParsed *cs = &objs[i].symbols[s];
            if (cs->section_number > 0 && cs->name[0] != '\0') {
                int16_t sidx = cs->section_number - 1;
                if (sidx >= 0 && (size_t)sidx < objs[i].section_count) {
                    const char *sname = objs[i].sections[sidx].name;
                    int sec_type = 0;
                    uint32_t sym_offset = 0;
                    if (strcmp(sname, ".text") == 0) {
                        sec_type = 1;
                        sym_offset = text_offs[i] + cs->value;
                    } else if (strcmp(sname, ".rdata") == 0) {
                        sec_type = 2;
                        sym_offset = rdata_offs[i] + cs->value;
                    } else if (strcmp(sname, ".data") == 0) {
                        sec_type = 3;
                        sym_offset = data_offs[i] + cs->value;
                    } else if (strcmp(sname, ".bss") == 0) {
                        sec_type = 4;
                        sym_offset = bss_offs[i] + cs->value;
                    }
                    if (sec_type > 0 && sym_count < MAX_PE_SYMS) {
                        bool exists = false;
                        for (size_t k = 0; k < sym_count; k++) {
                            if (strcmp(syms[k].name, cs->name) == 0) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            strncpy(syms[sym_count].name, cs->name, sizeof(syms[sym_count].name) - 1);
                            syms[sym_count].sec_type = sec_type;
                            syms[sym_count].offset = sym_offset;
                            syms[sym_count].is_defined = true;
                            sym_count++;
                        }
                    }
                }
            }
        }
    }

    #define MAX_PE_IMPORTS 512
    PeLinkImport *imports = (PeLinkImport *)calloc(MAX_PE_IMPORTS, sizeof(PeLinkImport));
    size_t num_imports = 0;

    if (!is_dll) {
        strncpy(imports[num_imports].name, "ExitProcess", sizeof(imports[0].name) - 1);
        strncpy(imports[num_imports].dll, "KERNEL32.dll", sizeof(imports[0].dll) - 1);
        num_imports++;
    }

    for (size_t i = 0; i < obj_count; i++) {
        for (size_t s = 0; s < objs[i].section_count; s++) {
            ZeroCCCoffSectParsed *sec = &objs[i].sections[s];
            for (size_t r = 0; r < sec->reloc_count; r++) {
                ZeroCCCoffRelocParsed *rel = &sec->relocs[r];
                if (rel->symbol_index >= objs[i].symbol_count) continue;
                ZeroCCCoffSymParsed *cs = &objs[i].symbols[rel->symbol_index];
                if (cs->section_number == 0 && cs->name[0] != '\0') {
                    const char *target = cs->name;
                    if (strncmp(target, "__imp_", 6) == 0) target += 6;
                    bool found = false;
                    for (size_t k = 0; k < sym_count; k++) {
                        if (strcmp(syms[k].name, target) == 0) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        bool already = false;
                        for (size_t m = 0; m < num_imports; m++) {
                            if (strcmp(imports[m].name, target) == 0) {
                                already = true;
                                break;
                            }
                        }
                        if (!already && num_imports < MAX_PE_IMPORTS) {
                            strncpy(imports[num_imports].name, target, sizeof(imports[0].name) - 1);
                            if (strcmp(target, "ExitProcess") == 0 ||
                                strcmp(target, "GetStdHandle") == 0 ||
                                strcmp(target, "WriteFile") == 0 ||
                                strcmp(target, "GetCommandLineA") == 0 ||
                                strcmp(target, "GetCommandLineW") == 0 ||
                                strcmp(target, "GetModuleHandleA") == 0 ||
                                strcmp(target, "GetModuleHandleW") == 0 ||
                                strcmp(target, "Sleep") == 0 ||
                                strcmp(target, "GetLastError") == 0 ||
                                strcmp(target, "VirtualAlloc") == 0 ||
                                strcmp(target, "VirtualFree") == 0) {
                                strncpy(imports[num_imports].dll, "KERNEL32.dll", sizeof(imports[0].dll) - 1);
                            } else if (strcmp(target, "MessageBoxA") == 0 || strcmp(target, "MessageBoxW") == 0) {
                                strncpy(imports[num_imports].dll, "USER32.dll", sizeof(imports[0].dll) - 1);
                            } else {
                                strncpy(imports[num_imports].dll, "msvcrt.dll", sizeof(imports[0].dll) - 1);
                            }
                            num_imports++;
                        }
                    }
                }
            }
        }
    }

    uint32_t file_align = ZERO_CC_PE_FILE_ALIGNMENT;
    uint32_t sec_align = ZERO_CC_PE_SECTION_ALIGNMENT;
    uint32_t text_rva = sec_align;

    /* Assign import stubs in .text */
    for (size_t m = 0; m < num_imports; m++) {
        total_text_size = pe_align_up(total_text_size, 16);
        imports[m].stub_text_offset = total_text_size;
        imports[m].stub_rva = text_rva + total_text_size;
        total_text_size += 16;
    }

    uint32_t text_raw_size = pe_align_up(total_text_size, file_align);
    uint32_t text_virt_size = pe_align_up(total_text_size, sec_align);

    uint32_t rdata_rva = text_rva + text_virt_size;
    uint32_t rdata_raw_size = pe_align_up(total_rdata_size, file_align);
    uint32_t rdata_virt_size = pe_align_up(total_rdata_size > 0 ? total_rdata_size : 1, sec_align);

    uint32_t data_rva = rdata_rva + rdata_virt_size;
    uint32_t data_raw_size = pe_align_up(total_data_size, file_align);
    uint32_t total_data_virt = total_data_size + total_bss_size;
    uint32_t data_virt_size = pe_align_up(total_data_virt > 0 ? total_data_virt : 1, sec_align);

    uint32_t idata_rva = data_rva + data_virt_size;

    /* Update RVAs in syms */
    for (size_t s = 0; s < sym_count; s++) {
        if (syms[s].sec_type == 1) syms[s].rva = text_rva + syms[s].offset;
        else if (syms[s].sec_type == 2) syms[s].rva = rdata_rva + syms[s].offset;
        else if (syms[s].sec_type == 3) syms[s].rva = data_rva + syms[s].offset;
        else if (syms[s].sec_type == 4) syms[s].rva = data_rva + total_data_size + syms[s].offset;
    }

    /* Group imports by DLL */
    #define MAX_PE_DLLS 16
    PeLinkDll dlls[MAX_PE_DLLS];
    size_t num_dlls = 0;

    PeLinkImport sorted_imports[MAX_PE_IMPORTS];
    size_t sorted_count = 0;

    for (size_t m = 0; m < num_imports; m++) {
        const char *dname = imports[m].dll;
        bool dll_seen = false;
        for (size_t d = 0; d < num_dlls; d++) {
            if (strcmp(dlls[d].name, dname) == 0) {
                dll_seen = true;
                break;
            }
        }
        if (!dll_seen && num_dlls < MAX_PE_DLLS) {
            strncpy(dlls[num_dlls].name, dname, sizeof(dlls[0].name) - 1);
            num_dlls++;
        }
    }

    for (size_t d = 0; d < num_dlls; d++) {
        dlls[d].first_imp = sorted_count;
        dlls[d].imp_count = 0;
        for (size_t m = 0; m < num_imports; m++) {
            if (strcmp(imports[m].dll, dlls[d].name) == 0) {
                sorted_imports[sorted_count] = imports[m];
                sorted_count++;
                dlls[d].imp_count++;
            }
        }
    }
    for (size_t m = 0; m < sorted_count; m++) {
        imports[m] = sorted_imports[m];
    }

    /* Calculate layout of .idata */
    uint32_t cur_idata = (uint32_t)((num_dlls + 1) * sizeof(ZeroCCImportDescriptor));
    for (size_t d = 0; d < num_dlls; d++) {
        dlls[d].ilt_offset = cur_idata;
        cur_idata += (uint32_t)((dlls[d].imp_count + 1) * 8);
        dlls[d].iat_offset = cur_idata;
        cur_idata += (uint32_t)((dlls[d].imp_count + 1) * 8);
    }
    for (size_t m = 0; m < num_imports; m++) {
        imports[m].hint_name_offset = cur_idata;
        uint32_t hn_len = 2 + (uint32_t)strlen(imports[m].name) + 1;
        if (hn_len & 1) hn_len++;
        cur_idata += hn_len;
    }
    for (size_t d = 0; d < num_dlls; d++) {
        dlls[d].dll_name_offset = cur_idata;
        cur_idata += (uint32_t)strlen(dlls[d].name) + 1;
    }

    uint32_t idata_raw_size = pe_align_up(cur_idata, file_align);
    uint32_t idata_virt_size = pe_align_up(cur_idata, sec_align);

    for (size_t d = 0; d < num_dlls; d++) {
        dlls[d].desc_rva = idata_rva + (uint32_t)(d * sizeof(ZeroCCImportDescriptor));
        dlls[d].ilt_rva = idata_rva + dlls[d].ilt_offset;
        dlls[d].iat_rva = idata_rva + dlls[d].iat_offset;
        dlls[d].dll_name_rva = idata_rva + dlls[d].dll_name_offset;
        for (size_t k = 0; k < dlls[d].imp_count; k++) {
            size_t m = dlls[d].first_imp + k;
            imports[m].hint_name_rva = idata_rva + imports[m].hint_name_offset;
            imports[m].ilt_rva = dlls[d].ilt_rva + (uint32_t)(k * 8);
            imports[m].iat_rva = dlls[d].iat_rva + (uint32_t)(k * 8);
        }
    }

    /* Allocate section buffers */
    uint8_t *merged_text = (uint8_t *)calloc(1, text_raw_size > 0 ? text_raw_size : 16);
    uint8_t *merged_rdata = (uint8_t *)calloc(1, rdata_raw_size > 0 ? rdata_raw_size : 16);
    uint8_t *merged_data = (uint8_t *)calloc(1, data_raw_size > 0 ? data_raw_size : 16);
    uint8_t *idata_buf = (uint8_t *)calloc(1, idata_raw_size > 0 ? idata_raw_size : 512);

    /* Copy object payloads */
    for (size_t i = 0; i < obj_count; i++) {
        const ZeroCCCoffSectParsed *st = zero_cc_coff_find_section(&objs[i], ".text");
        if (st && st->size_of_raw_data > 0 && st->data) {
            memcpy(merged_text + text_offs[i], st->data, st->size_of_raw_data);
        }
        const ZeroCCCoffSectParsed *sr = zero_cc_coff_find_section(&objs[i], ".rdata");
        if (sr && sr->size_of_raw_data > 0 && sr->data) {
            memcpy(merged_rdata + rdata_offs[i], sr->data, sr->size_of_raw_data);
        }
        const ZeroCCCoffSectParsed *sd = zero_cc_coff_find_section(&objs[i], ".data");
        if (sd && sd->size_of_raw_data > 0 && sd->data) {
            memcpy(merged_data + data_offs[i], sd->data, sd->size_of_raw_data);
        }
    }

    /* Build .idata */
    for (size_t d = 0; d < num_dlls; d++) {
        ZeroCCImportDescriptor *desc = (ZeroCCImportDescriptor *)(idata_buf + d * sizeof(ZeroCCImportDescriptor));
        desc->original_first_thunk = dlls[d].ilt_rva;
        desc->time_date_stamp = 0;
        desc->forwarder_chain = 0;
        desc->name_rva = dlls[d].dll_name_rva;
        desc->first_thunk = dlls[d].iat_rva;

        for (size_t k = 0; k < dlls[d].imp_count; k++) {
            size_t m = dlls[d].first_imp + k;
            uint64_t val = (uint64_t)imports[m].hint_name_rva;
            memcpy(idata_buf + dlls[d].ilt_offset + k * 8, &val, 8);
            memcpy(idata_buf + dlls[d].iat_offset + k * 8, &val, 8);
        }
        memcpy(idata_buf + dlls[d].dll_name_offset, dlls[d].name, strlen(dlls[d].name) + 1);
    }
    for (size_t m = 0; m < num_imports; m++) {
        uint16_t hint = 0;
        memcpy(idata_buf + imports[m].hint_name_offset, &hint, 2);
        memcpy(idata_buf + imports[m].hint_name_offset + 2, imports[m].name, strlen(imports[m].name) + 1);
    }

    /* Build import jump stubs */
    for (size_t m = 0; m < num_imports; m++) {
        uint8_t *st = merged_text + imports[m].stub_text_offset;
        st[0] = 0xFF;
        st[1] = 0x25;
        int32_t disp = (int32_t)(imports[m].iat_rva - (imports[m].stub_rva + 6));
        memcpy(st + 2, &disp, 4);
        for (size_t p = 6; p < 16; p++) st[p] = 0x90;
    }

    /* Build CRT entry stub */
    if (is_dll) {
        merged_text[0] = 0xB8; merged_text[1] = 0x01;
        merged_text[2] = 0x00; merged_text[3] = 0x00;
        merged_text[4] = 0x00; merged_text[5] = 0xC3;
        for (size_t p = 6; p < stub_size; p++) merged_text[p] = 0x90;
    } else {
        uint32_t main_rva = text_rva + 32;
        for (size_t s = 0; s < sym_count; s++) {
            if (strcmp(syms[s].name, "main") == 0 ||
                strcmp(syms[s].name, "_main") == 0 ||
                strcmp(syms[s].name, "WinMain") == 0) {
                main_rva = syms[s].rva;
                break;
            }
        }
        uint32_t exit_iat_rva = idata_rva;
        for (size_t m = 0; m < num_imports; m++) {
            if (strcmp(imports[m].name, "ExitProcess") == 0) {
                exit_iat_rva = imports[m].iat_rva;
                break;
            }
        }
        merged_text[0] = 0x48; merged_text[1] = 0x83; merged_text[2] = 0xEC; merged_text[3] = 0x28;
        merged_text[4] = 0xE8;
        int32_t call_main_disp = (int32_t)(main_rva - (text_rva + 9));
        memcpy(merged_text + 5, &call_main_disp, 4);
        merged_text[9] = 0x89; merged_text[10] = 0xC1;
        merged_text[11] = 0xFF; merged_text[12] = 0x15;
        int32_t iat_disp = (int32_t)(exit_iat_rva - (text_rva + 17));
        memcpy(merged_text + 13, &iat_disp, 4);
        merged_text[17] = 0xC3;
        for (size_t p = 18; p < stub_size; p++) merged_text[p] = 0x90;
    }

    /* Apply Relocations */
    for (size_t i = 0; i < obj_count; i++) {
        for (size_t s = 0; s < objs[i].section_count; s++) {
            ZeroCCCoffSectParsed *sec = &objs[i].sections[s];
            if (sec->reloc_count == 0) continue;

            uint8_t *sec_buf = NULL;
            uint32_t sec_base_rva = 0;
            if (strcmp(sec->name, ".text") == 0) {
                sec_buf = merged_text + text_offs[i];
                sec_base_rva = text_rva + text_offs[i];
            } else if (strcmp(sec->name, ".data") == 0) {
                sec_buf = merged_data + data_offs[i];
                sec_base_rva = data_rva + data_offs[i];
            } else {
                continue;
            }

            for (size_t r = 0; r < sec->reloc_count; r++) {
                ZeroCCCoffRelocParsed *rel = &sec->relocs[r];
                if (rel->virtual_address >= sec->size_of_raw_data) continue;
                uint8_t *patch_loc = sec_buf + rel->virtual_address;
                uint32_t src_rva = sec_base_rva + rel->virtual_address;

                if (rel->symbol_index >= objs[i].symbol_count) continue;
                ZeroCCCoffSymParsed *cs = &objs[i].symbols[rel->symbol_index];
                uint32_t tgt_rva = 0;

                if (cs->section_number > 0) {
                    int16_t sidx = cs->section_number - 1;
                    if (sidx >= 0 && (size_t)sidx < objs[i].section_count) {
                        const char *sn = objs[i].sections[sidx].name;
                        if (strcmp(sn, ".text") == 0) tgt_rva = text_rva + text_offs[i] + cs->value;
                        else if (strcmp(sn, ".rdata") == 0) tgt_rva = rdata_rva + rdata_offs[i] + cs->value;
                        else if (strcmp(sn, ".data") == 0) tgt_rva = data_rva + data_offs[i] + cs->value;
                        else if (strcmp(sn, ".bss") == 0) tgt_rva = data_rva + total_data_size + bss_offs[i] + cs->value;
                    }
                } else {
                    bool found = false;
                    for (size_t k = 0; k < sym_count; k++) {
                        if (strcmp(syms[k].name, cs->name) == 0) {
                            tgt_rva = syms[k].rva;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        const char *iname = cs->name;
                        bool is_imp_sym = false;
                        if (strncmp(iname, "__imp_", 6) == 0) {
                            iname += 6;
                            is_imp_sym = true;
                        }
                        for (size_t m = 0; m < num_imports; m++) {
                            if (strcmp(imports[m].name, iname) == 0) {
                                tgt_rva = is_imp_sym ? imports[m].iat_rva : imports[m].stub_rva;
                                found = true;
                                break;
                            }
                        }
                    }
                    if (!found) {
                        fprintf(stderr, "zerocc: undefined reference to '%s' in '%s'\n", cs->name, obj_files[i]);
                    }
                }

                if (rel->type == ZERO_CC_IMAGE_REL_AMD64_REL32) {
                    int32_t addend = 0;
                    memcpy(&addend, patch_loc, 4);
                    int32_t disp = (int32_t)(tgt_rva + addend - (src_rva + 4));
                    memcpy(patch_loc, &disp, 4);
                } else if (rel->type == ZERO_CC_IMAGE_REL_AMD64_ADDR64) {
                    int64_t addend = 0;
                    memcpy(&addend, patch_loc, 8);
                    uint64_t va = ZERO_CC_PE_DEFAULT_IMAGE_BASE + tgt_rva + (uint64_t)addend;
                    memcpy(patch_loc, &va, 8);
                } else if (rel->type == ZERO_CC_IMAGE_REL_AMD64_ADDR32NB) {
                    int32_t addend = 0;
                    memcpy(&addend, patch_loc, 4);
                    uint32_t rva = tgt_rva + (uint32_t)addend;
                    memcpy(patch_loc, &rva, 4);
                } else if (rel->type == ZERO_CC_IMAGE_REL_AMD64_ADDR32) {
                    int32_t addend = 0;
                    memcpy(&addend, patch_loc, 4);
                    uint32_t va = (uint32_t)(ZERO_CC_PE_DEFAULT_IMAGE_BASE + tgt_rva + addend);
                    memcpy(patch_loc, &va, 4);
                }
            }
        }
    }

    /* Write headers & sections to output file */
    FILE *f = fopen(filename, "wb");
    if (!f) {
        free(merged_text); free(merged_rdata); free(merged_data); free(idata_buf);
        free(syms); free(imports);
        free(text_offs); free(rdata_offs); free(data_offs); free(bss_offs);
        for (size_t k = 0; k < obj_count; k++) zero_cc_coff_parsed_object_free(&objs[k]);
        free(objs);
        return false;
    }

    uint16_t num_sections = 4;
    uint32_t raw_headers_size = (uint32_t)(sizeof(ZeroCCDosHeader) + 64 + 4 +
                                           sizeof(ZeroCCCoffFileHeader) +
                                           sizeof(ZeroCCOptionalHeader64) +
                                           num_sections * sizeof(ZeroCCSectionHeader));
    uint32_t headers_size_aligned = pe_align_up(raw_headers_size, file_align);

    uint32_t text_raw_off = headers_size_aligned;
    uint32_t rdata_raw_off = text_raw_off + text_raw_size;
    uint32_t data_raw_off = rdata_raw_off + rdata_raw_size;
    uint32_t idata_raw_off = data_raw_off + data_raw_size;

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
    coff.characteristics = ZERO_CC_IMAGE_FILE_EXECUTABLE_IMAGE | ZERO_CC_IMAGE_FILE_LARGE_ADDRESS_AWARE;
    if (is_dll) coff.characteristics |= ZERO_CC_IMAGE_FILE_DLL;

    ZeroCCOptionalHeader64 opt;
    memset(&opt, 0, sizeof(opt));
    opt.magic = ZERO_CC_PE32PLUS_MAGIC;
    opt.major_linker_version = 1;
    opt.minor_linker_version = 0;
    opt.size_of_code = text_raw_size;
    opt.size_of_initialized_data = rdata_raw_size + data_raw_size + idata_raw_size;
    opt.size_of_uninitialized_data = (uint32_t)pe_align_up(total_bss_size, file_align);
    opt.address_of_entry_point = text_rva;
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
    opt.size_of_image = idata_rva + idata_virt_size;
    opt.size_of_headers = headers_size_aligned;
    opt.subsystem = is_gui ? ZERO_CC_IMAGE_SUBSYSTEM_WINDOWS_GUI : ZERO_CC_IMAGE_SUBSYSTEM_WINDOWS_CUI;
    opt.dll_characteristics = 0x8160;
    opt.size_of_stack_reserve = 0x100000;
    opt.size_of_stack_commit  = 0x10000;
    opt.size_of_heap_reserve  = 0x100000;
    opt.size_of_heap_commit   = 0x1000;
    opt.number_of_rva_and_sizes = 16;

    if (num_dlls > 0) {
        opt.data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_IMPORT].virtual_address = idata_rva;
        opt.data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_IMPORT].size = (uint32_t)((num_dlls + 1) * sizeof(ZeroCCImportDescriptor));

        opt.data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_IAT].virtual_address = dlls[0].iat_rva;
        uint32_t total_iat_len = (dlls[num_dlls - 1].iat_rva + (uint32_t)((dlls[num_dlls - 1].imp_count + 1) * 8)) - dlls[0].iat_rva;
        opt.data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_IAT].size = total_iat_len;
    }

    ZeroCCSectionHeader sec[4];
    memset(sec, 0, sizeof(sec));

    /* 1. .text */
    memcpy(sec[0].name, ".text\0\0\0", 8);
    sec[0].virtual_size = total_text_size;
    sec[0].virtual_address = text_rva;
    sec[0].size_of_raw_data = text_raw_size;
    sec[0].pointer_to_raw_data = text_raw_off;
    sec[0].characteristics = ZERO_CC_IMAGE_SCN_CNT_CODE | ZERO_CC_IMAGE_SCN_MEM_EXECUTE | ZERO_CC_IMAGE_SCN_MEM_READ;

    /* 2. .rdata */
    memcpy(sec[1].name, ".rdata\0\0", 8);
    sec[1].virtual_size = total_rdata_size > 0 ? total_rdata_size : 1;
    sec[1].virtual_address = rdata_rva;
    sec[1].size_of_raw_data = rdata_raw_size;
    sec[1].pointer_to_raw_data = rdata_raw_size > 0 ? rdata_raw_off : 0;
    sec[1].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA | ZERO_CC_IMAGE_SCN_MEM_READ;

    /* 3. .data */
    memcpy(sec[2].name, ".data\0\0\0", 8);
    sec[2].virtual_size = total_data_virt > 0 ? total_data_virt : 1;
    sec[2].virtual_address = data_rva;
    sec[2].size_of_raw_data = data_raw_size;
    sec[2].pointer_to_raw_data = data_raw_size > 0 ? data_raw_off : 0;
    sec[2].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA | ZERO_CC_IMAGE_SCN_MEM_READ | ZERO_CC_IMAGE_SCN_MEM_WRITE;

    /* 4. .idata */
    memcpy(sec[3].name, ".idata\0\0", 8);
    sec[3].virtual_size = idata_raw_size;
    sec[3].virtual_address = idata_rva;
    sec[3].size_of_raw_data = idata_raw_size;
    sec[3].pointer_to_raw_data = idata_raw_off;
    sec[3].characteristics = ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA | ZERO_CC_IMAGE_SCN_MEM_READ | ZERO_CC_IMAGE_SCN_MEM_WRITE;

    fwrite(&dos, 1, sizeof(dos), f);
    fwrite(dos_stub, 1, sizeof(dos_stub), f);
    fwrite(&pe_sig, 1, sizeof(pe_sig), f);
    fwrite(&coff, 1, sizeof(coff), f);
    fwrite(&opt, 1, sizeof(opt), f);
    fwrite(sec, 1, num_sections * sizeof(ZeroCCSectionHeader), f);

    size_t written_hdr = sizeof(dos) + sizeof(dos_stub) + sizeof(pe_sig) +
                         sizeof(coff) + sizeof(opt) + num_sections * sizeof(ZeroCCSectionHeader);
    while (written_hdr < headers_size_aligned) {
        fputc(0, f);
        written_hdr++;
    }

    fwrite(merged_text, 1, total_text_size, f);
    for (size_t i = total_text_size; i < text_raw_size; i++) fputc(0x90, f);

    if (merged_rdata && total_rdata_size > 0) {
        fwrite(merged_rdata, 1, total_rdata_size, f);
    }
    for (size_t i = total_rdata_size; i < rdata_raw_size; i++) fputc(0, f);

    if (merged_data && total_data_size > 0) {
        fwrite(merged_data, 1, total_data_size, f);
    }
    for (size_t i = total_data_size; i < data_raw_size; i++) fputc(0, f);

    fwrite(idata_buf, 1, idata_raw_size, f);
    fclose(f);

    /* Free all resources */
    free(merged_text);
    free(merged_rdata);
    free(merged_data);
    free(idata_buf);
    free(syms);
    free(imports);
    free(text_offs);
    free(rdata_offs);
    free(data_offs);
    free(bss_offs);
    for (size_t k = 0; k < obj_count; k++) zero_cc_coff_parsed_object_free(&objs[k]);
    free(objs);

    return true;
}
