/* Parse and Compile a Top-Level Function */
#ifndef ZERO_CC_JIT_LINKER_H
#define ZERO_CC_JIT_LINKER_H

static inline void zero_cc_compile_function(ZeroCCCompiler *c) {
    /* Skip static / inline / const / _Noreturn */
    bool fn_noreturn = false;
    while (c->tokens[c->token_pos].type == TK_STATIC || c->tokens[c->token_pos].type == TK_INLINE ||
           c->tokens[c->token_pos].type == TK_NORETURN ||
           (c->tokens[c->token_pos].type == TK_IDENT && strcmp(c->tokens[c->token_pos].str_val, "const") == 0)) {
        if (c->tokens[c->token_pos].type == TK_NORETURN) fn_noreturn = true;
        c->token_pos++;
    }

    /* Return type */
    ZeroCCType fn_ret_type = zero_cc_type_int();
    if (c->tokens[c->token_pos].type == TK_STRUCT) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            ZeroCCType *st = zero_cc_find_struct(c, c->tokens[c->token_pos].str_val);
            if (!st) st = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
            if (st) fn_ret_type = *st;
            c->token_pos++;
        }
    } else if (c->tokens[c->token_pos].type == TK_IDENT) {
        ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
        if (tdef) fn_ret_type = *tdef;
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_INT || c->tokens[c->token_pos].type == TK_CHAR ||
               c->tokens[c->token_pos].type == TK_VOID || c->tokens[c->token_pos].type == TK_FLOAT_KW ||
               c->tokens[c->token_pos].type == TK_DOUBLE_KW || c->tokens[c->token_pos].type == TK_BOOL_KW ||
               c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
               c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG) {
        c->token_pos++;
    }
    while (c->tokens[c->token_pos].type == TK_STAR) {
        fn_ret_type = zero_cc_type_ptr(NULL);
        c->token_pos++;
    }

    if (c->tokens[c->token_pos].type != TK_IDENT) return;
    char fn_name[128];
    snprintf(fn_name, sizeof(fn_name), "%s", c->tokens[c->token_pos].str_val);
    c->token_pos++;

    if (c->tokens[c->token_pos].type != TK_LPAREN) return;
    c->token_pos++;

    /* Parse Parameters in signature */
    ZeroCCType param_types[8];
    char param_names[8][128];
    size_t param_count = 0;
    bool is_variadic_fn = false;

    while (c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
        size_t ppos = c->token_pos;
        if (c->tokens[c->token_pos].type == TK_ELLIPSIS) {
            is_variadic_fn = true;
            c->token_pos++;
        } else if (c->tokens[c->token_pos].type == TK_IDENT || c->tokens[c->token_pos].type == TK_INT ||
            c->tokens[c->token_pos].type == TK_CHAR || c->tokens[c->token_pos].type == TK_VOID ||
            c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_FLOAT_KW ||
            c->tokens[c->token_pos].type == TK_DOUBLE_KW || c->tokens[c->token_pos].type == TK_BOOL_KW ||
            c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
            c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG) {
            const char *ptname = c->tokens[c->token_pos].str_val;
            ZeroCCType *pfound = zero_cc_find_typedef(c, ptname);
            if (!pfound) pfound = zero_cc_find_struct(c, ptname);
            c->token_pos++;
            bool is_ptr = false;
            while (c->tokens[c->token_pos].type == TK_STAR) { is_ptr = true; c->token_pos++; }
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                if (param_count < 8) {
                    snprintf(param_names[param_count], sizeof(param_names[param_count]), "%s", c->tokens[c->token_pos].str_val);
                    param_types[param_count] = is_ptr ? zero_cc_type_ptr(pfound) : (pfound ? *pfound : zero_cc_type_int64());
                    if (pfound) snprintf(param_types[param_count].struct_name, sizeof(param_types[param_count].struct_name), "%s", ptname);
                    param_count++;
                }
                c->token_pos++;
            }
        }
        if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        if (c->token_pos == ppos) c->token_pos++;
    }
    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;

    if (c->tokens[c->token_pos].type == TK_SEMI) {
        /* Function prototype declaration: do not emit prologue */
        c->token_pos++;
        return;
    }

    if (c->tokens[c->token_pos].type == TK_LBRACE) {
        c->token_pos++;

        if (c->function_count >= ZERO_CC_MAX_FUNCTIONS) return;
        ZeroCCFunction *fn = &c->functions[c->function_count++];
        snprintf(fn->name, sizeof(fn->name), "%s", fn_name);
        fn->text_offset = c->text_section.length;
        fn->param_count = param_count;
        fn->return_type = fn_ret_type;
        fn->returns_struct = (fn_ret_type.tag == TYPE_STRUCT);
        fn->ret_struct_sz = fn_ret_type.size;
        fn->is_variadic = is_variadic_fn;
        fn->noreturn = fn_noreturn;

        c->local_count = 0;
        c->current_stack_frame = 16;

        /* Function Prologue */
        zero_cc_emit_u8(&c->text_section, 0x55);          /* push rbp */
        zero_cc_emit_u8(&c->text_section, 0x48);          /* mov rbp, rsp */
        zero_cc_emit_u8(&c->text_section, 0x89);
        zero_cc_emit_u8(&c->text_section, 0xE5);
        zero_cc_emit_u8(&c->text_section, 0x48);          /* sub rsp, 256 */
        zero_cc_emit_u8(&c->text_section, 0x81);
        zero_cc_emit_u8(&c->text_section, 0xEC);
        zero_cc_emit_u32(&c->text_section, 256);

        /* Save parameter registers (RDI, RSI, RDX, RCX, R8, R9) to stack */
        static const uint8_t save_param_regs[6][3] = {
            {0x48, 0x89, 0xBD}, /* mov [rbp - offset], rdi */
            {0x48, 0x89, 0xB5}, /* mov [rbp - offset], rsi */
            {0x48, 0x89, 0x95}, /* mov [rbp - offset], rdx */
            {0x48, 0x89, 0x8D}, /* mov [rbp - offset], rcx */
            {0x4C, 0x89, 0x85}, /* mov [rbp - offset], r8  */
            {0x4C, 0x89, 0x8D}  /* mov [rbp - offset], r9  */
        };

        for (size_t p = 0; p < param_count; p++) {
            ZeroCCSym *sym = zero_cc_add_local(c, param_names[p], param_types[p]);
            sym->is_param = true;
            sym->param_reg_idx = (int)p;
            if (p < 6) {
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][0]);
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][1]);
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][2]);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            }
        }

        /* Variadic spill area: spill remaining registers for va_start */
        if (is_variadic_fn) {
            for (size_t p = param_count; p < 6; p++) {
                c->current_stack_frame += 8;
                int32_t spill_offset = c->current_stack_frame;
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][0]);
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][1]);
                zero_cc_emit_u8(&c->text_section, save_param_regs[p][2]);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-spill_offset));
            }
        }

        while (c->tokens[c->token_pos].type != TK_RBRACE && c->tokens[c->token_pos].type != TK_EOF) {
            size_t spos = c->token_pos;
            zero_cc_parse_stmt(c);
            if (c->token_pos == spos) c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;

        /* Function Epilogue */
        zero_cc_emit_u8(&c->text_section, 0xC9);
        zero_cc_emit_u8(&c->text_section, 0xC3);
    }
}

/* Master Translation Unit Compiler */
static inline void zero_cc_compile_c99(ZeroCCCompiler *c) {
    zero_cc_buf_init(&c->text_section, 4096);
    zero_cc_buf_init(&c->rodata_section, 1024);
    zero_cc_buf_init(&c->data_section, 1024);
    c->function_count = 0;
    c->reloc_count = 0;
    zero_cc_init_standard_types(c);

    while (c->tokens[c->token_pos].type != TK_EOF && !c->had_error) {
        if (c->tokens[c->token_pos].type == TK_TYPEDEF) {
            zero_cc_parse_stmt(c);
            if (c->had_error) break;
            continue;
        }
        if (c->tokens[c->token_pos].type == TK_STATIC_ASSERT) {
            zero_cc_parse_stmt(c);
            if (c->had_error) break;
            continue;
        }

        size_t prev_pos = c->token_pos;
        /* Top level functions */
        zero_cc_compile_function(c);
        if (c->had_error) break;
        if (c->token_pos == prev_pos) {
            c->token_pos++;
        }
    }
}

#include "zero_mmap_adapter.h"

/* --------------------------------------------------------------------------
 *  4. JIT EXECUTION ENGINE
 *  -------------------------------------------------------------------------- */
static inline int zero_cc_execute_jit(ZeroCCCompiler *c, void *context) {
    if (!c || c->text_section.length == 0) return -1;

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__) || defined(__posix) || defined(__MACH__) || defined(_WIN32)
    size_t page_sz = (c->text_section.length + c->rodata_section.length + 4095) & ~4095;
    void *mem = zero_mmap_alloc_rw(page_sz);
    if (mem == MAP_FAILED || !mem) return -1;

    /* Patch internal function calls and string literal RIP offsets */
    for (size_t i = 0; i < c->reloc_count; i++) {
        if (c->relocs[i].is_call) {
            ZeroCCFunction *target_fn = zero_cc_find_function(c, c->relocs[i].target_symbol);
            if (target_fn) {
                int32_t rel = (int32_t)(target_fn->text_offset - (c->relocs[i].text_patch_offset + 4));
                memcpy(c->text_section.data + c->relocs[i].text_patch_offset, &rel, 4);
            }
        }
    }
    for (size_t i = 0; i < c->str_reloc_count; i++) {
        int32_t rel = (int32_t)((c->text_section.length + c->str_relocs[i].rodata_offset) - (c->str_relocs[i].text_patch_offset + 4));
        memcpy(c->text_section.data + c->str_relocs[i].text_patch_offset, &rel, 4);
    }

    memcpy(mem, c->text_section.data, c->text_section.length);
    if (c->rodata_section.length > 0) {
        memcpy((char*)mem + c->text_section.length, c->rodata_section.data, c->rodata_section.length);
    }

    if (zero_mmap_make_rx(mem, page_sz) != 0) {
        zero_mmap_free(mem, page_sz);
        return -1;
    }

    /* Find 'main' entry point */
    ZeroCCFunction *main_fn = zero_cc_find_function(c, "main");
    size_t main_offset = main_fn ? main_fn->text_offset : 0;

    int (*jit_main)(void *) = (int (*)(void *))((char*)mem + main_offset);
    int result = jit_main(context);
    zero_mmap_free(mem, page_sz);
    return result;
#else
    (void)context;
    return 0;
#endif
}

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
            if (target_fn) {
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

#endif /* ZERO_CC_JIT_LINKER_H */

