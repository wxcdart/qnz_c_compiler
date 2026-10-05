/*
 * ZeroCC CLI Driver
 * 
 * A GCC-compatible command-line interface for the ZeroCC C compiler.
 * Supports JIT mode, object file output, and various optimization levels.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zero_cc.h"
#include "zero_cc/parser/opt_pass.h"
#include "zero_cc/jit/elf_writer.h"

#define VERSION "0.1.0-dev"

typedef struct {
    const char *input_file;
    const char *output_file;
    int emit_object;      /* -c */
    int emit_asm;         /* -S */
    int run_mode;         /* --run */
    int verbose;          /* -v */
    int opt_level;        /* 0-3 for -O0 to -O3 */
    int include_dirs[10]; /* -I (placeholder) */
    int include_count;
    int define_macros[10]; /* -D (placeholder) */
    int define_count;
    int dump_ir;          /* -dump-ir */
    int link_mode;        /* Link with system linker (default when -o specified) */
} CompileOptions;

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <source.c>\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <file>       Output file (executable or object with -c)\n");
    fprintf(stderr, "  -c              Compile to object file (.o) only\n");
    fprintf(stderr, "  -S              Emit assembly\n");
    fprintf(stderr, "  -O0             No optimization\n");
    fprintf(stderr, "  -O1             Basic optimization (default)\n");
    fprintf(stderr, "  -O2             Standard optimization\n");
    fprintf(stderr, "  -O3             Aggressive optimization\n");
    fprintf(stderr, "  -I<dir>         Add include directory\n");
    fprintf(stderr, "  -D<macro>       Define macro\n");
    fprintf(stderr, "  -run, --run     Run in JIT mode (default)\n");
    fprintf(stderr, "  -dump-ir        Dump IR before optimization\n");
    fprintf(stderr, "  -v, --verbose   Verbose output\n");
    fprintf(stderr, "  --version       Print version\n");
    fprintf(stderr, "  -h, --help      Show this help\n");
}

static void print_version(void) {
    printf("ZeroCC C Compiler version %s\n", VERSION);
    printf("Multi-ISA JIT compiler with optimization pipeline\n");
}

static int parse_args(int argc, char **argv, CompileOptions *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->opt_level = 1; /* Default -O1 */
    
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        
        if (strcmp(arg, "-o") == 0 && i + 1 < argc) {
            opts->output_file = argv[++i];
        }
        else if (strcmp(arg, "-c") == 0) {
            opts->emit_object = 1;
        }
        else if (strcmp(arg, "-S") == 0) {
            opts->emit_asm = 1;
        }
        else if (strcmp(arg, "-run") == 0 || strcmp(arg, "--run") == 0) {
            opts->run_mode = 1;
        }
        else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            opts->verbose = 1;
        }
        else if (strcmp(arg, "--version") == 0) {
            print_version();
            return 0;
        }
        else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(arg, "-O0") == 0) {
            opts->opt_level = 0;
        }
        else if (strcmp(arg, "-O1") == 0) {
            opts->opt_level = 1;
        }
        else if (strcmp(arg, "-O2") == 0) {
            opts->opt_level = 2;
        }
        else if (strcmp(arg, "-O3") == 0) {
            opts->opt_level = 3;
        }
        else if (strncmp(arg, "-I", 2) == 0) {
            if (opts->include_count < 10) {
                opts->include_dirs[opts->include_count++] = 1; /* Placeholder */
            }
        }
        else if (strncmp(arg, "-D", 2) == 0) {
            if (opts->define_count < 10) {
                opts->define_macros[opts->define_count++] = 1; /* Placeholder */
            }
        }
        else if (strcmp(arg, "-dump-ir") == 0) {
            opts->dump_ir = 1;
        }
        else if (arg[0] != '-') {
            opts->input_file = arg;
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return -1;
        }
    }
    
    if (!opts->input_file) {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(argv[0]);
        return -1;
    }
    
    return 1; /* Continue with compilation */
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    size_t read = fread(buf, 1, size, f);
    buf[read] = '\0';
    fclose(f);
    
    return buf;
}

static int compile_jit(CompileOptions *opts) {
    /* Read source file */
    char *source = read_file(opts->input_file);
    if (!source) {
        return 1;
    }
    
    if (opts->verbose) {
        const char *opt_name = "O0";
        switch (opts->opt_level) {
            case 0: opt_name = "O0"; break;
            case 1: opt_name = "O1"; break;
            case 2: opt_name = "O2"; break;
            case 3: opt_name = "O3"; break;
        }
        printf("Compiling %s with -O%s\n", opts->input_file, opt_name);
    }
    
    /* Create compiler */
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) {
        perror("calloc");
        free(source);
        return 1;
    }
    
    /* Preprocess */
    char *prep = zero_cc_preprocess(source, NULL);
    free(source);
    if (!prep) {
        fprintf(stderr, "Preprocessing failed\n");
        free(comp);
        return 1;
    }
    
    /* Tokenize */
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    
    /* Initialize optimization level */
    if (opts->opt_level >= 2) {
        opt_init(OPT_LEVEL_O2);
    } else if (opts->opt_level >= 1) {
        opt_init(OPT_LEVEL_O1);
    } else {
        opt_init(OPT_LEVEL_NONE);
    }
    
    /* Compile */
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
        zero_cc_buf_free(&comp->text_section);
        zero_cc_buf_free(&comp->rodata_section);
        zero_cc_buf_free(&comp->data_section);
        free(comp->tokens);
        free(comp);
        return 1;
    }
    
    if (opts->dump_ir) {
        printf("\n=== IR (before optimization) ===\n");
        /* TODO: Add IR dump function */
        printf("(IR dump not yet implemented)\n");
    }
    
    /* Execute */
    int result = zero_cc_execute_jit(comp, NULL);
    
    if (opts->verbose) {
        printf("JIT execution result: %d\n", result);
    }
    
    /* Cleanup */
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    
    return result;
}

static int compile_object(CompileOptions *opts) {
    /* Read source file */
    char *source = read_file(opts->input_file);
    if (!source) {
        return 1;
    }
    
    if (opts->verbose) {
        printf("Compiling %s to object file\n", opts->input_file);
    }
    
    /* Create compiler */
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) {
        perror("calloc");
        free(source);
        return 1;
    }
    
    /* Preprocess */
    char *prep = zero_cc_preprocess(source, NULL);
    free(source);
    if (!prep) {
        fprintf(stderr, "Preprocessing failed\n");
        free(comp);
        return 1;
    }
    
    /* Tokenize */
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    
    /* Initialize optimization */
    if (opts->opt_level >= 2) {
        opt_init(OPT_LEVEL_O2);
    } else if (opts->opt_level >= 1) {
        opt_init(OPT_LEVEL_O1);
    } else {
        opt_init(OPT_LEVEL_NONE);
    }
    
    /* Compile */
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
        zero_cc_buf_free(&comp->text_section);
        zero_cc_buf_free(&comp->rodata_section);
        zero_cc_buf_free(&comp->data_section);
        free(comp->tokens);
        free(comp);
        return 1;
    }
    
    /* Determine output filename */
    const char *output_path = opts->output_file;
    if (!output_path) {
        /* Generate output name from input: source.c -> source.o */
        static char output_buf[512];
        const char *base = opts->input_file;
        const char *dot = strrchr(base, '.');
        const char *slash = strrchr(base, '/');
        if (slash && (!dot || slash > dot)) {
            base = slash + 1;
        }
        if (dot) {
            size_t len = dot - base;
            if (len > sizeof(output_buf) - 4) len = sizeof(output_buf) - 4;
            strncpy(output_buf, base, len);
            output_buf[len] = '\0';
            strcat(output_buf, ".o");
        } else {
            strncpy(output_buf, base, sizeof(output_buf) - 3);
            strcat(output_buf, ".o");
        }
        output_path = output_buf;
    }
    
    if (opts->verbose) {
        printf("Writing object file: %s\n", output_path);
    }
    
    /* Initialize ELF writer */
    ElfWriterCtx elf_ctx;
    elf_writer_init(&elf_ctx, ELF_MACHINE_X86_64);
    
    /* Add .text section */
    if (comp->text_section.length > 0) {
        elf_writer_add_section(&elf_ctx, ".text",
                               (uint8_t*)comp->text_section.data,
                               comp->text_section.length,
                               ELF_SHT_PROGBITS,
                               ELF_SHF_ALLOC | ELF_SHF_EXECINSTR,
                               16);
    }
    
    /* Add .rodata section */
    if (comp->rodata_section.length > 0) {
        elf_writer_add_section(&elf_ctx, ".rodata",
                               (uint8_t*)comp->rodata_section.data,
                               comp->rodata_section.length,
                               ELF_SHT_PROGBITS,
                               ELF_SHF_ALLOC,
                               8);
    }
    
    /* Add .data section */
    if (comp->data_section.length > 0) {
        elf_writer_add_section(&elf_ctx, ".data",
                               (uint8_t*)comp->data_section.data,
                               comp->data_section.length,
                               ELF_SHT_PROGBITS,
                               ELF_SHF_ALLOC | ELF_SHF_WRITE,
                               8);
    }
    
    /* Add .rodata section symbol first (needed for string literal relocations) */
    /* Section symbols must come before global symbols for correct sh_info calculation */
    if (comp->rodata_section.length > 0) {
        elf_writer_add_symbol(&elf_ctx, ".rodata",
                              ELF_STB_LOCAL, ELF_STT_SECTION,
                              2,  /* .rodata section index */
                              0, 0);
    }
    
    /* Add symbols for functions */
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *fn = &comp->functions[i];
        if (fn->name[0]) {
            /* Estimate function size (simplified - in reality would need more analysis) */
            size_t fn_size = 0;  /* Could be computed from next function's offset */
            int section_idx = SHN_UNDEF;  // Default to undefined
            if (!fn->is_prototype) {
                section_idx = 1;  // Compiled functions go in .text section
            }
            elf_writer_add_symbol(&elf_ctx, fn->name,
                                  ELF_STB_GLOBAL, ELF_STT_FUNC,
                                  section_idx,
                                  fn->text_offset,
                                  fn_size);
        }
    }
    
    /* Add relocation entries for external function calls */
    /* R_X86_64_PC32 = 2 for x86-64 (standard PC-relative call) */
    #define R_X86_64_PC32 2
    for (size_t i = 0; i < comp->reloc_count; i++) {
        ZeroCCReloc *r = &comp->relocs[i];
        if (r->is_call) {
            int symbol_idx = 0;  /* Default to null symbol */
            
            /* Find symbol in ELF writer's symbol table */
            for (int j = 0; j < elf_ctx.symbol_count; j++) {
                if (strcmp(elf_ctx.symbols[j].name, r->target_symbol) == 0) {
                    symbol_idx = j + 1;  /* +1 for null symbol */
                    break;
                }
            }
            
            /* Add relocation entry */
            /* Use PC32 for function calls - linker will resolve to PLT for external functions */
            elf_writer_add_relocation(&elf_ctx, 1,  /* .text section */
                                      r->text_patch_offset,
                                      symbol_idx,
                                      R_X86_64_PC32,
                                      0);  /* addend */
        }
    }
    
    /* Add relocation entries for string literals (.rodata references) */
    /* The .rodata section symbol was added earlier (index 1 after null symbol) */
    int rodata_symbol_idx = 1;  /* .rodata section symbol is always at index 1 if present */
    
    for (size_t i = 0; i < comp->str_reloc_count; i++) {
        ZeroCCStringReloc *sr = &comp->str_relocs[i];
        
        /* Add relocation entry with addend = rodata_offset - 4 */
        /* The -4 accounts for the PC being at patch_offset + 4 (end of 4-byte displacement) */
        elf_writer_add_relocation(&elf_ctx, 1,  /* .text section */
                                  sr->text_patch_offset,
                                  rodata_symbol_idx,
                                  R_X86_64_PC32,
                                  (int64_t)sr->rodata_offset - 4);  /* addend */
    }
    
    /* Write ELF file */
    if (!elf_writer_write(&elf_ctx, output_path)) {
        fprintf(stderr, "ELF writer error: %s\n", elf_writer_get_error(&elf_ctx));
        elf_writer_cleanup(&elf_ctx);
        zero_cc_buf_free(&comp->text_section);
        zero_cc_buf_free(&comp->rodata_section);
        zero_cc_buf_free(&comp->data_section);
        free(comp->tokens);
        free(comp);
        return 1;
    }
    
    if (opts->verbose) {
        printf("Object file written successfully\n");
    }
    
    /* Cleanup */
    elf_writer_cleanup(&elf_ctx);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    
    return 0;
}

static int compile_and_link(CompileOptions *opts) {
    /* Generate temporary object file */
    char obj_path[512];
    if (opts->verbose) {
        printf("Generating intermediate object file...\n");
    }
    
    /* Create temp object file path */
    snprintf(obj_path, sizeof(obj_path), "/tmp/zerocc_%d.o", getpid());
    
    /* First, compile to object file */
    CompileOptions obj_opts = *opts;
    obj_opts.emit_object = 1;
    obj_opts.output_file = obj_path;
    obj_opts.verbose = 0;  /* Suppress verbose during object generation */
    
    int ret = compile_object(&obj_opts);
    if (ret != 0) {
        fprintf(stderr, "Error: Compilation to object file failed\n");
        return ret;
    }
    
    if (opts->verbose) {
        printf("Linking with system linker...\n");
    }
    
    /* Invoke system linker (gcc) to create executable */
    char cmd[1024];
    const char *output = opts->output_file ? opts->output_file : "a.out";
    
    snprintf(cmd, sizeof(cmd),
             "gcc -o %s %s -lc 2>&1",
             output, obj_path);
    
    if (opts->verbose) {
        printf("  %s\n", cmd);
    }
    
    ret = system(cmd);
    
    /* Clean up temporary object file */
    unlink(obj_path);
    
    if (ret != 0) {
        fprintf(stderr, "Error: Linking failed\n");
        return 1;
    }
    
    if (opts->verbose) {
        printf("Executable created: %s\n", output);
    }
    
    return 0;
}

int main(int argc, char **argv) {
    CompileOptions opts;
    
    int ret = parse_args(argc, argv, &opts);
    if (ret <= 0) {
        return ret == 0 ? 0 : 1;
    }
    
    /* Determine mode: -c -> object file, -o without -c -> executable, else JIT */
    if (opts.emit_object) {
        return compile_object(&opts);
    } else if (opts.output_file) {
        /* Generate executable via system linker */
        return compile_and_link(&opts);
    } else {
        return compile_jit(&opts);
    }
}
