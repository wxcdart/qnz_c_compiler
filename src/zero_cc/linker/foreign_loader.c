/**
 * @file foreign_loader.c
 * @brief In-Process Execution Loader for PE32+ and Mach-O 64-bit Binaries (Sprint 261)
 */

#include "zero_cc/linker/foreign_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include "zero_cc/linker/pe_writer.h"
#include "zero_cc/linker/macho_writer.h"

/* Thread-safe / global exit interception for PE runtime */
static jmp_buf g_pe_exit_env;
static int g_pe_exit_code = 0;

/* Global exit interception for Mach-O runtime */
static jmp_buf g_macho_exit_env;
static int g_macho_exit_code = 0;

static void macho_exit(int code) {
    g_macho_exit_code = code;
    longjmp(g_macho_exit_env, 1);
}

static void macho_abort(void) {
    g_macho_exit_code = 134;
    longjmp(g_macho_exit_env, 1);
}

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
#define HAS_MS_ABI 1
#endif
#endif

#ifdef HAS_MS_ABI
__attribute__((ms_abi)) static void win_ExitProcess(unsigned int code) {
    g_pe_exit_code = (int)code;
    longjmp(g_pe_exit_env, 1);
}

__attribute__((ms_abi)) static void win_exit(int code) {
    g_pe_exit_code = code;
    longjmp(g_pe_exit_env, 1);
}

__attribute__((ms_abi)) static void win_abort(void) {
    g_pe_exit_code = 134;
    longjmp(g_pe_exit_env, 1);
}

__attribute__((ms_abi)) static int win_puts(const char *s) {
    return puts(s ? s : "(null)");
}

__attribute__((ms_abi)) static int win_putchar(int c) {
    return putchar(c);
}

__attribute__((ms_abi)) static int win_getchar(void) {
    return getchar();
}

__attribute__((ms_abi)) static void *win_malloc(size_t sz) {
    return malloc(sz);
}

__attribute__((ms_abi)) static void win_free(void *p) {
    free(p);
}

__attribute__((ms_abi)) static void *win_calloc(size_t num, size_t sz) {
    return calloc(num, sz);
}

__attribute__((ms_abi)) static void *win_realloc(void *ptr, size_t sz) {
    return realloc(ptr, sz);
}

__attribute__((ms_abi)) static size_t win_strlen(const char *s) {
    return s ? strlen(s) : 0;
}

__attribute__((ms_abi)) static int win_strcmp(const char *s1, const char *s2) {
    return strcmp(s1, s2);
}

__attribute__((ms_abi)) static int win_strncmp(const char *s1, const char *s2, size_t n) {
    return strncmp(s1, s2, n);
}

__attribute__((ms_abi)) static char *win_strcpy(char *dst, const char *src) {
    if (!dst || !src) return dst;
    size_t len = strlen(src);
    memcpy(dst, src, len + 1);
    return dst;
}

__attribute__((ms_abi)) static char *win_strncpy(char *dst, const char *src, size_t n) {
    return strncpy(dst, src, n);
}

__attribute__((ms_abi)) static void *win_memcpy(void *dst, const void *src, size_t n) {
    return memcpy(dst, src, n);
}

__attribute__((ms_abi)) static void *win_memset(void *s, int c, size_t n) {
    return memset(s, c, n);
}

__attribute__((ms_abi)) static void *win_memmove(void *dst, const void *src, size_t n) {
    return memmove(dst, src, n);
}

__attribute__((ms_abi)) static int win_memcmp(const void *s1, const void *s2, size_t n) {
    return memcmp(s1, s2, n);
}

__attribute__((ms_abi)) static int win_atoi(const char *s) {
    return s ? atoi(s) : 0;
}

__attribute__((ms_abi)) static int win_abs(int n) {
    return n < 0 ? -n : n;
}

__attribute__((ms_abi)) static int win_rand(void) {
    return rand();
}

__attribute__((ms_abi)) static void win_srand(unsigned int seed) {
    srand(seed);
}

__attribute__((ms_abi)) static int win_printf(const char *fmt, ...) {
    uint64_t *sp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(sp));
    /* In ms_abi with push %rbp; mov %rsp, %rbp, caller shadow space starts at [rbp+16] */
    uint64_t *args = (uint64_t *)((char *)sp + 24);
    return printf(fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
}

__attribute__((ms_abi)) static int win_sprintf(char *str, const char *fmt, ...) {
    uint64_t *sp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(sp));
    uint64_t *args = (uint64_t *)((char *)sp + 32);
    return snprintf(str, 1048576, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
}

__attribute__((ms_abi)) static int win_snprintf(char *str, size_t size, const char *fmt, ...) {
    uint64_t *sp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(sp));
    uint64_t *args = (uint64_t *)((char *)sp + 40);
    return snprintf(str, size, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
}

__attribute__((ms_abi)) static uint32_t win_GetLastError(void) {
    return 0;
}

__attribute__((ms_abi)) static void win_SetLastError(uint32_t dwErrCode) {
    (void)dwErrCode;
}

__attribute__((ms_abi)) static void *win_GetStdHandle(uint32_t nStdHandle) {
    (void)nStdHandle;
    return (void *)(uintptr_t)1;
}

__attribute__((ms_abi)) static void win_Sleep(uint32_t dwMilliseconds) {
    (void)dwMilliseconds;
}

static void *resolve_pe_symbol(const char *dll, const char *name) {
    (void)dll;
    if (!name) return NULL;
    if (strcmp(name, "ExitProcess") == 0) return (void *)win_ExitProcess;
    if (strcmp(name, "GetLastError") == 0) return (void *)win_GetLastError;
    if (strcmp(name, "SetLastError") == 0) return (void *)win_SetLastError;
    if (strcmp(name, "GetStdHandle") == 0) return (void *)win_GetStdHandle;
    if (strcmp(name, "Sleep") == 0) return (void *)win_Sleep;
    if (strcmp(name, "exit") == 0) return (void *)win_exit;
    if (strcmp(name, "abort") == 0) return (void *)win_abort;
    if (strcmp(name, "printf") == 0) return (void *)win_printf;
    if (strcmp(name, "sprintf") == 0) return (void *)win_sprintf;
    if (strcmp(name, "snprintf") == 0) return (void *)win_snprintf;
    if (strcmp(name, "puts") == 0) return (void *)win_puts;
    if (strcmp(name, "putchar") == 0) return (void *)win_putchar;
    if (strcmp(name, "getchar") == 0) return (void *)win_getchar;
    if (strcmp(name, "malloc") == 0) return (void *)win_malloc;
    if (strcmp(name, "free") == 0) return (void *)win_free;
    if (strcmp(name, "calloc") == 0) return (void *)win_calloc;
    if (strcmp(name, "realloc") == 0) return (void *)win_realloc;
    if (strcmp(name, "strlen") == 0) return (void *)win_strlen;
    if (strcmp(name, "strcmp") == 0) return (void *)win_strcmp;
    if (strcmp(name, "strncmp") == 0) return (void *)win_strncmp;
    if (strcmp(name, "strcpy") == 0) return (void *)win_strcpy;
    if (strcmp(name, "strncpy") == 0) return (void *)win_strncpy;
    if (strcmp(name, "memcpy") == 0) return (void *)win_memcpy;
    if (strcmp(name, "memset") == 0) return (void *)win_memset;
    if (strcmp(name, "memmove") == 0) return (void *)win_memmove;
    if (strcmp(name, "memcmp") == 0) return (void *)win_memcmp;
    if (strcmp(name, "atoi") == 0) return (void *)win_atoi;
    if (strcmp(name, "abs") == 0) return (void *)win_abs;
    if (strcmp(name, "rand") == 0) return (void *)win_rand;
    if (strcmp(name, "srand") == 0) return (void *)win_srand;

    /* Fallback: try host dlsym */
    return dlsym(RTLD_DEFAULT, name);
}
#endif /* HAS_MS_ABI */

ZeroCCForeignFormat zero_cc_detect_foreign_format(const uint8_t *data, size_t size) {
    if (!data || size < 4) return FOREIGN_FMT_UNKNOWN;

    /* PE/COFF DOS header starts with 'MZ' */
    if (size >= 2 && data[0] == 'M' && data[1] == 'Z') {
        if (size >= 64) {
            uint32_t e_lfanew = *(const uint32_t *)(data + 60);
            if (e_lfanew + 4 <= size) {
                uint32_t pe_sig = *(const uint32_t *)(data + e_lfanew);
                if (pe_sig == ZERO_CC_PE_SIGNATURE) {
                    return FOREIGN_FMT_PE;
                }
            } else {
                return FOREIGN_FMT_PE;
            }
        } else {
            return FOREIGN_FMT_PE;
        }
    }

    /* Mach-O 64-bit magic */
    uint32_t magic = *(const uint32_t *)data;
    if (magic == ZERO_CC_MH_MAGIC_64) {
        return FOREIGN_FMT_MACHO;
    }

    return FOREIGN_FMT_UNKNOWN;
}

int zero_cc_run_pe_mem(const uint8_t *pe_data, size_t pe_size, int argc, char **argv) {
    (void)argc;
    (void)argv;
#ifndef HAS_MS_ABI
    (void)pe_data;
    (void)pe_size;
    fprintf(stderr, "zerocc: foreign loader: ms_abi not supported on this host\n");
    return -1;
#else
    if (!pe_data || pe_size < 512) {
        fprintf(stderr, "zerocc: foreign loader: invalid PE file size\n");
        return -1;
    }

    const ZeroCCDosHeader *dos = (const ZeroCCDosHeader *)pe_data;
    if (dos->e_magic != ZERO_CC_PE_DOS_MAGIC) {
        fprintf(stderr, "zerocc: foreign loader: missing MZ header\n");
        return -1;
    }

    uint32_t pe_off = dos->e_lfanew;
    if (pe_off + 4 + sizeof(ZeroCCCoffFileHeader) + sizeof(ZeroCCOptionalHeader64) > pe_size) {
        fprintf(stderr, "zerocc: foreign loader: corrupted PE header offset\n");
        return -1;
    }

    uint32_t pe_sig = *(const uint32_t *)(pe_data + pe_off);
    if (pe_sig != ZERO_CC_PE_SIGNATURE) {
        fprintf(stderr, "zerocc: foreign loader: missing PE signature\n");
        return -1;
    }

    const ZeroCCCoffFileHeader *coff = (const ZeroCCCoffFileHeader *)(pe_data + pe_off + 4);
    if (coff->machine != ZERO_CC_IMAGE_FILE_MACHINE_AMD64) {
        fprintf(stderr, "zerocc: foreign loader: unsupported PE machine (expected x86-64 0x8664)\n");
        return -1;
    }

    const ZeroCCOptionalHeader64 *opt = (const ZeroCCOptionalHeader64 *)(pe_data + pe_off + 4 + sizeof(ZeroCCCoffFileHeader));
    if (opt->magic != ZERO_CC_PE32PLUS_MAGIC) {
        fprintf(stderr, "zerocc: foreign loader: unsupported optional header (expected PE32+ 0x20B)\n");
        return -1;
    }

    uint32_t num_sections = coff->number_of_sections;
    const ZeroCCSectionHeader *sections = (const ZeroCCSectionHeader *)(pe_data + pe_off + 4 +
                                           sizeof(ZeroCCCoffFileHeader) + coff->size_of_optional_header);

    uint64_t image_base = opt->image_base;
    uint32_t size_of_image = opt->size_of_image;
    if (size_of_image == 0) size_of_image = 1024 * 1024;

    /* Map virtual address space */
    void *mapped = mmap((void *)(uintptr_t)image_base, size_of_image,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        mapped = mmap(NULL, size_of_image, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (mapped == MAP_FAILED || !mapped) {
        fprintf(stderr, "zerocc: foreign loader: mmap failed for PE image\n");
        return -1;
    }

    uintptr_t base = (uintptr_t)mapped;
    int64_t delta = (int64_t)base - (int64_t)image_base;

    /* Copy sections */
    for (uint32_t i = 0; i < num_sections; i++) {
        const ZeroCCSectionHeader *sec = &sections[i];
        if (sec->virtual_address >= size_of_image) continue;

        uint8_t *dest = (uint8_t *)base + sec->virtual_address;
        size_t cpy_sz = sec->size_of_raw_data;
        if (sec->virtual_size > 0 && cpy_sz > sec->virtual_size) {
            cpy_sz = sec->virtual_size;
        }
        if (sec->pointer_to_raw_data + cpy_sz <= pe_size && cpy_sz > 0) {
            memcpy(dest, pe_data + sec->pointer_to_raw_data, cpy_sz);
        }
    }

    /* Resolve Imports via IAT */
    if (opt->number_of_rva_and_sizes > ZERO_CC_IMAGE_DIRECTORY_ENTRY_IMPORT &&
        opt->data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_IMPORT].virtual_address > 0) {
        uint32_t idata_rva = opt->data_directory[ZERO_CC_IMAGE_DIRECTORY_ENTRY_IMPORT].virtual_address;
        if (idata_rva < size_of_image) {
            const ZeroCCImportDescriptor *desc = (const ZeroCCImportDescriptor *)(base + idata_rva);
            while (desc->name_rva != 0 && (desc->first_thunk != 0 || desc->original_first_thunk != 0)) {
                const char *dll_name = (desc->name_rva < size_of_image) ? (const char *)(base + desc->name_rva) : "";
                uint32_t ilt_rva = desc->original_first_thunk ? desc->original_first_thunk : desc->first_thunk;
                uint32_t iat_rva = desc->first_thunk;

                if (ilt_rva < size_of_image && iat_rva < size_of_image) {
                    uint64_t *ilt = (uint64_t *)(base + ilt_rva);
                    uint64_t *iat = (uint64_t *)(base + iat_rva);
                    while (*ilt != 0) {
                        uint64_t thunk_val = *ilt;
                        if ((thunk_val & (1ULL << 63)) == 0) {
                            uint32_t hint_rva = (uint32_t)thunk_val;
                            if (hint_rva + 2 < size_of_image) {
                                const char *func_name = (const char *)(base + hint_rva + 2);
                                void *shim = resolve_pe_symbol(dll_name, func_name);
                                if (shim) {
                                    *iat = (uint64_t)(uintptr_t)shim;
                                }
                            }
                        }
                        ilt++;
                        iat++;
                    }
                }
                desc++;
            }
        }
    }

    /* Apply base relocations if mapped at a different base */
    if (delta != 0 && opt->number_of_rva_and_sizes > 5 &&
        opt->data_directory[5].virtual_address > 0) {
        uint32_t reloc_rva = opt->data_directory[5].virtual_address;
        uint32_t reloc_size = opt->data_directory[5].size;
        uint32_t cur = 0;
        while (cur + 8 <= reloc_size && reloc_rva + cur + 8 <= size_of_image) {
            uint32_t page_rva = *(const uint32_t *)(base + reloc_rva + cur);
            uint32_t block_sz = *(const uint32_t *)(base + reloc_rva + cur + 4);
            if (block_sz < 8) break;

            uint32_t count = (block_sz - 8) / 2;
            const uint16_t *entries = (const uint16_t *)(base + reloc_rva + cur + 8);
            for (uint32_t k = 0; k < count; k++) {
                uint16_t type = entries[k] >> 12;
                uint16_t off = entries[k] & 0x0FFF;
                if (type == 10) { /* IMAGE_REL_BASED_DIR64 */
                    uint64_t *target = (uint64_t *)(base + page_rva + off);
                    *target += (uint64_t)delta;
                }
            }
            cur += block_sz;
        }
    }

    /* Set section memory protections */
    for (uint32_t i = 0; i < num_sections; i++) {
        const ZeroCCSectionHeader *sec = &sections[i];
        if (sec->virtual_address >= size_of_image) continue;

        int prot = 0;
        if (sec->characteristics & ZERO_CC_IMAGE_SCN_MEM_READ) prot |= PROT_READ;
        if (sec->characteristics & ZERO_CC_IMAGE_SCN_MEM_WRITE) prot |= PROT_WRITE;
        if (sec->characteristics & ZERO_CC_IMAGE_SCN_MEM_EXECUTE) prot |= PROT_EXEC;

        size_t v_sz = sec->virtual_size;
        if (v_sz == 0) v_sz = sec->size_of_raw_data;
        if (v_sz == 0) v_sz = 4096;
        size_t aligned_sz = (v_sz + 4095) & ~4095UL;
        mprotect((void *)(base + sec->virtual_address), aligned_sz, prot);
    }

    /* Execute entry point */
    int exit_code = 0;
    if (opt->address_of_entry_point > 0 && opt->address_of_entry_point < size_of_image) {
        typedef int (*pe_entry_fn)(void);
        pe_entry_fn entry = (pe_entry_fn)(base + opt->address_of_entry_point);

        g_pe_exit_code = 0;
        if (setjmp(g_pe_exit_env) == 0) {
            int ret = entry();
            exit_code = ret;
        } else {
            exit_code = g_pe_exit_code;
        }
    }

    munmap(mapped, size_of_image);
    return exit_code;
#endif
}

int zero_cc_run_pe_file(const char *filepath, int argc, char **argv) {
    if (!filepath) return -1;
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "zerocc: foreign loader: failed to open '%s'\n", filepath);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    int rc = zero_cc_run_pe_mem(buf, rd, argc, argv);
    free(buf);
    return rc;
}

int zero_cc_run_macho_mem(const uint8_t *macho_data, size_t macho_size, int argc, char **argv) {
    if (!macho_data || macho_size < sizeof(ZeroCCMachHeader64)) {
        fprintf(stderr, "zerocc: foreign loader: invalid Mach-O file size\n");
        return -1;
    }

    const ZeroCCMachHeader64 *hdr = (const ZeroCCMachHeader64 *)macho_data;
    if (hdr->magic != ZERO_CC_MH_MAGIC_64) {
        fprintf(stderr, "zerocc: foreign loader: missing Mach-O 64-bit magic (0xfeedfacf)\n");
        return -1;
    }
    if (hdr->cputype != ZERO_CC_CPU_TYPE_X86_64) {
        fprintf(stderr, "zerocc: foreign loader: unsupported Mach-O cputype (expected x86-64)\n");
        return -1;
    }
    if (hdr->filetype != ZERO_CC_MH_EXECUTE) {
        fprintf(stderr, "zerocc: foreign loader: unsupported Mach-O filetype (expected MH_EXECUTE)\n");
        return -1;
    }

    uint32_t ncmds = hdr->ncmds;
    uint32_t cur_off = sizeof(ZeroCCMachHeader64);

    uint64_t min_vm = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t max_vm = 0;
    uint64_t entry_off = 0;
    bool found_entry = false;
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    uint32_t indirectsymoff = 0, nindirectsyms = 0;

    /* Pass 1: find VM extent, LC_MAIN, LC_SYMTAB, LC_DYSYMTAB */
    for (uint32_t i = 0; i < ncmds; i++) {
        if (cur_off + 8 > macho_size) break;
        uint32_t cmd = *(const uint32_t *)(macho_data + cur_off);
        uint32_t cmdsz = *(const uint32_t *)(macho_data + cur_off + 4);
        if (cmdsz < 8 || cur_off + cmdsz > macho_size) break;

        if (cmd == ZERO_CC_LC_SEGMENT_64) {
            const ZeroCCSegmentCommand64 *seg = (const ZeroCCSegmentCommand64 *)(macho_data + cur_off);
            if (strcmp(seg->segname, "__PAGEZERO") != 0 && seg->vmsize > 0) {
                if (seg->vmaddr < min_vm) min_vm = seg->vmaddr;
                if (seg->vmaddr + seg->vmsize > max_vm) max_vm = seg->vmaddr + seg->vmsize;
            }
        } else if (cmd == ZERO_CC_LC_MAIN) {
            const ZeroCCEntryPointCommand *ep = (const ZeroCCEntryPointCommand *)(macho_data + cur_off);
            entry_off = ep->entryoff;
            found_entry = true;
        } else if (cmd == ZERO_CC_LC_SYMTAB) {
            const ZeroCCSymtabCommand *sc = (const ZeroCCSymtabCommand *)(macho_data + cur_off);
            symoff = sc->symoff;
            nsyms = sc->nsyms;
            stroff = sc->stroff;
            strsize = sc->strsize;
        } else if (cmd == ZERO_CC_LC_DYSYMTAB) {
            const ZeroCCDysymtabCommand *dc = (const ZeroCCDysymtabCommand *)(macho_data + cur_off);
            indirectsymoff = dc->indirectsymoff;
            nindirectsyms = dc->nindirectsyms;
        }

        cur_off += cmdsz;
    }

    if (min_vm >= max_vm || !found_entry) {
        fprintf(stderr, "zerocc: foreign loader: invalid Mach-O VM bounds or missing LC_MAIN\n");
        return -1;
    }

    size_t total_vmsize = (size_t)(max_vm - min_vm);
    total_vmsize = (total_vmsize + 4095) & ~4095UL;

    void *mapped = mmap((void *)(uintptr_t)min_vm, total_vmsize,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        mapped = mmap(NULL, total_vmsize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (mapped == MAP_FAILED || !mapped) {
        fprintf(stderr, "zerocc: foreign loader: mmap failed for Mach-O image\n");
        return -1;
    }

    uintptr_t base = (uintptr_t)mapped;

    /* Pass 2: copy segment data */
    cur_off = sizeof(ZeroCCMachHeader64);
    for (uint32_t i = 0; i < ncmds; i++) {
        if (cur_off + 8 > macho_size) break;
        uint32_t cmd = *(const uint32_t *)(macho_data + cur_off);
        uint32_t cmdsz = *(const uint32_t *)(macho_data + cur_off + 4);
        if (cmdsz < 8 || cur_off + cmdsz > macho_size) break;

        if (cmd == ZERO_CC_LC_SEGMENT_64) {
            const ZeroCCSegmentCommand64 *seg = (const ZeroCCSegmentCommand64 *)(macho_data + cur_off);
            if (strcmp(seg->segname, "__PAGEZERO") != 0 && seg->vmsize > 0) {
                uint8_t *dest = (uint8_t *)base + (seg->vmaddr - min_vm);
                if (seg->filesize > 0 && seg->fileoff + seg->filesize <= macho_size) {
                    memcpy(dest, macho_data + seg->fileoff, seg->filesize);
                }
            }
        }
        cur_off += cmdsz;
    }

    /* Pass 2b: Resolve dynamic symbol pointers in __DATA (__la_symbol_ptr / __nl_symbol_ptr) */
    if (indirectsymoff > 0 && nindirectsyms > 0 && symoff > 0 && stroff > 0 &&
        indirectsymoff + nindirectsyms * sizeof(uint32_t) <= macho_size &&
        symoff + nsyms * sizeof(ZeroCCNlist64) <= macho_size &&
        stroff + strsize <= macho_size) {
        const uint32_t *indirect_table = (const uint32_t *)(macho_data + indirectsymoff);
        const ZeroCCNlist64 *sym_table = (const ZeroCCNlist64 *)(macho_data + symoff);
        const char *strtab = (const char *)(macho_data + stroff);

        cur_off = sizeof(ZeroCCMachHeader64);
        for (uint32_t i = 0; i < ncmds; i++) {
            if (cur_off + 8 > macho_size) break;
            uint32_t cmd = *(const uint32_t *)(macho_data + cur_off);
            uint32_t cmdsz = *(const uint32_t *)(macho_data + cur_off + 4);
            if (cmdsz < 8 || cur_off + cmdsz > macho_size) break;

            if (cmd == ZERO_CC_LC_SEGMENT_64) {
                const ZeroCCSegmentCommand64 *seg = (const ZeroCCSegmentCommand64 *)(macho_data + cur_off);
                if (strcmp(seg->segname, "__PAGEZERO") != 0 && seg->vmsize > 0) {
                    const ZeroCCSection64 *secs = (const ZeroCCSection64 *)(macho_data + cur_off + sizeof(ZeroCCSegmentCommand64));
                    for (uint32_t s = 0; s < seg->nsects; s++) {
                        uint32_t type = secs[s].flags & 0xFF;
                        if (type == ZERO_CC_MACHO_S_LAZY_SYMBOL_POINTERS ||
                            type == ZERO_CC_MACHO_S_NON_LAZY_SYMBOL_POINTERS) {
                            uint32_t start_indirect = secs[s].reserved1;
                            uint32_t num_ptrs = (uint32_t)(secs[s].size / 8);
                            uint8_t *slot_base = (uint8_t *)base + (secs[s].addr - min_vm);

                            for (uint32_t p = 0; p < num_ptrs; p++) {
                                if (start_indirect + p >= nindirectsyms) break;
                                uint32_t sym_idx = indirect_table[start_indirect + p];
                                if (sym_idx == ZERO_CC_INDIRECT_SYMBOL_LOCAL ||
                                    sym_idx == ZERO_CC_INDIRECT_SYMBOL_ABS) continue;
                                if (sym_idx >= nsyms) continue;

                                const ZeroCCNlist64 *sym = &sym_table[sym_idx];
                                if (sym->n_strx >= strsize) continue;
                                const char *sym_name = strtab + sym->n_strx;
                                const char *clean_name = (sym_name[0] == '_') ? sym_name + 1 : sym_name;

                                void *resolved = NULL;
                                if (strcmp(clean_name, "exit") == 0) {
                                    resolved = (void *)macho_exit;
                                } else if (strcmp(clean_name, "abort") == 0) {
                                    resolved = (void *)macho_abort;
                                } else {
                                    resolved = dlsym(RTLD_DEFAULT, clean_name);
                                    if (!resolved && sym_name[0] == '_') {
                                        resolved = dlsym(RTLD_DEFAULT, sym_name);
                                    }
                                }

                                if (resolved) {
                                    uint64_t *ptr_slot = (uint64_t *)(slot_base + p * 8);
                                    *ptr_slot = (uint64_t)(uintptr_t)resolved;
                                }
                            }
                        }
                    }
                }
            }
            cur_off += cmdsz;
        }
    }

    /* Pass 3: set protections */
    cur_off = sizeof(ZeroCCMachHeader64);
    for (uint32_t i = 0; i < ncmds; i++) {
        if (cur_off + 8 > macho_size) break;
        uint32_t cmd = *(const uint32_t *)(macho_data + cur_off);
        uint32_t cmdsz = *(const uint32_t *)(macho_data + cur_off + 4);
        if (cmdsz < 8 || cur_off + cmdsz > macho_size) break;

        if (cmd == ZERO_CC_LC_SEGMENT_64) {
            const ZeroCCSegmentCommand64 *seg = (const ZeroCCSegmentCommand64 *)(macho_data + cur_off);
            if (strcmp(seg->segname, "__PAGEZERO") != 0 && seg->vmsize > 0) {
                void *seg_addr = (void *)(base + (seg->vmaddr - min_vm));
                int prot = 0;
                if (seg->initprot & ZERO_CC_VM_PROT_READ) prot |= PROT_READ;
                if (seg->initprot & ZERO_CC_VM_PROT_WRITE) prot |= PROT_WRITE;
                if (seg->initprot & ZERO_CC_VM_PROT_EXECUTE) prot |= PROT_EXEC;
                size_t seg_sz = (seg->vmsize + 4095) & ~4095UL;
                mprotect(seg_addr, seg_sz, prot);
            }
        }
        cur_off += cmdsz;
    }

    /* Execute entry point */
    uintptr_t entry_addr = base + entry_off;
    typedef int (*macho_main_fn)(int, char **);
    macho_main_fn entry_fn = (macho_main_fn)entry_addr;

    int res = 0;
    g_macho_exit_code = 0;
    if (setjmp(g_macho_exit_env) == 0) {
        res = entry_fn(argc, argv);
    } else {
        res = g_macho_exit_code;
    }

    munmap(mapped, total_vmsize);
    return res;
}

int zero_cc_run_macho_file(const char *filepath, int argc, char **argv) {
    if (!filepath) return -1;
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "zerocc: foreign loader: failed to open '%s'\n", filepath);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    int rc = zero_cc_run_macho_mem(buf, rd, argc, argv);
    free(buf);
    return rc;
}

bool zero_cc_is_foreign_binary(const char *filepath) {
    if (!filepath) return false;
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;
    uint8_t header[1024];
    size_t rd = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (rd < 4) return false;
    return zero_cc_detect_foreign_format(header, rd) != FOREIGN_FMT_UNKNOWN;
}

int zero_cc_run_foreign_file(const char *filepath, int argc, char **argv) {
    if (!filepath) return -1;
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "zerocc: foreign loader: cannot open '%s'\n", filepath);
        return -1;
    }
    uint8_t header[1024];
    size_t rd = fread(header, 1, sizeof(header), f);
    fclose(f);

    if (rd < 4) return -1;

    ZeroCCForeignFormat fmt = zero_cc_detect_foreign_format(header, rd);
    if (fmt == FOREIGN_FMT_PE) {
        return zero_cc_run_pe_file(filepath, argc, argv);
    } else if (fmt == FOREIGN_FMT_MACHO) {
        return zero_cc_run_macho_file(filepath, argc, argv);
    }

    fprintf(stderr, "zerocc: foreign loader: unsupported or unknown format for '%s'\n", filepath);
    return -1;
}
