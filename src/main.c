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
#include <limits.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#include "zero_cc.h"
#include "zero_cc/opt/opt_pass.h"
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/jit/elf_writer.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/parser/decl_struct.h"  /* zero_cc_parse_struct_body (Sprint 114: was transitive via compile.h) */

#include "zero_cc/types/global_vars.h"
#include "zero_cc/core/multi_tu.h"
#include "zero_cc/linker/linker.h"

#include "zero_cc/driver/driver_opts.h"
#include "zero_cc/driver/driver_asm.h"

static ZeroCCISA parse_target_arch(const char *target);
static void zero_cc_init_compiler_with_target(ZeroCCCompiler *comp, ZeroCCISA target_isa);

static int compile_object_single(CompileOptions *opts, const char *input, const char *output);

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


/* Run the preprocessor with the CLI-provided -I include dirs and -D
 * defines (Sprint 41).  Both are optional; with neither present this
 * is exactly the old `zero_cc_preprocess(source, NULL)` behavior.
 * Also handles dependency generation for -MMD/-MF/-MT (Toolchain T0). */
static char *preprocess_with_options(const char *source, const CompileOptions *opts,
                                     const char *main_file, const char *dep_target) {
    ZeroCCIncludeDirs inc_dirs;
    ZeroCCDefines defines;
    memset(&inc_dirs, 0, sizeof(inc_dirs));
    for (int d = 0; d < opts->include_count; d++) {
        if (inc_dirs.count < ZERO_CC_MAX_INCLUDE_PATHS) {
            inc_dirs.paths[inc_dirs.count++] = opts->include_dirs[d];
        }
    }
    memset(&defines, 0, sizeof(defines));

    ZeroCCISA target_isa = parse_target_arch(opts->target_arch);
    if (target_isa == ISA_AARCH64) {
        defines.defs[defines.count].name = strdup("__x86_64__");
        defines.defs[defines.count].value = NULL;
        defines.undef[defines.count++] = 1;

        defines.defs[defines.count].name = strdup("__aarch64__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;

        defines.defs[defines.count].name = strdup("__LP64__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    } else if (target_isa == ISA_RISCV64) {
        defines.defs[defines.count].name = strdup("__x86_64__");
        defines.defs[defines.count].value = NULL;
        defines.undef[defines.count++] = 1;

        defines.defs[defines.count].name = strdup("__riscv");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;

        defines.defs[defines.count].name = strdup("__riscv_xlen");
        defines.defs[defines.count].value = strdup("64");
        defines.undef[defines.count++] = 0;

        defines.defs[defines.count].name = strdup("__LP64__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    } else if (target_isa == ISA_RISCV32) {
        defines.defs[defines.count].name = strdup("__x86_64__");
        defines.defs[defines.count].value = NULL;
        defines.undef[defines.count++] = 1;

        defines.defs[defines.count].name = strdup("__LP64__");
        defines.defs[defines.count].value = NULL;
        defines.undef[defines.count++] = 1;

        defines.defs[defines.count].name = strdup("__riscv");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;

        defines.defs[defines.count].name = strdup("__riscv_xlen");
        defines.defs[defines.count].value = strdup("32");
        defines.undef[defines.count++] = 0;

        defines.defs[defines.count].name = strdup("__ILP32__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    }

    for (int d = 0; d < opts->macro_opt_count; d++) {
        if (defines.count >= 128) break;
        /* Copy the option string: the caller's CompileOptions must stay
         * intact for a potential second compile in the same process. */
        char *name_copy = strdup(opts->macro_opts[d]);
        char *value_copy = NULL;
        if (opts->macro_opt_kind[d] == 'D') {
            char *eq = strchr(name_copy, '=');
            if (eq) {
                *eq = '\0';
                value_copy = strdup(eq + 1);
            }
        }
        defines.defs[defines.count].name = name_copy;
        defines.defs[defines.count].value = value_copy;
        defines.undef[defines.count] = (opts->macro_opt_kind[d] == 'U');
        defines.count++;
    }

    const char *final_dep_target = dep_target;
    if (!final_dep_target || !final_dep_target[0]) {
        final_dep_target = opts->dep_target;
    }
    char default_target_buf[512];
    if (!final_dep_target || !final_dep_target[0]) {
        const char *ref = opts->output_file;
        if (ref) {
            const char *slash = strrchr(ref, '/');
            final_dep_target = slash ? slash + 1 : ref;
        } else if (main_file) {
            const char *slash = strrchr(main_file, '/');
            const char *base = slash ? slash + 1 : main_file;
            const char *dot = strrchr(base, '.');
            if (dot) {
                size_t len = dot - base;
                snprintf(default_target_buf, sizeof(default_target_buf), "%.*s.o", (int)len, base);
            } else {
                snprintf(default_target_buf, sizeof(default_target_buf), "%s.o", base);
            }
            final_dep_target = default_target_buf;
        }
    }

    const char *final_dep_file = opts->dep_file;
    char default_dep_file_buf[512];
    if ((!final_dep_file || !final_dep_file[0]) && opts->gen_deps) {
        const char *ref = opts->output_file;
        if (ref) {
            const char *dot = strrchr(ref, '.');
            if (dot) {
                size_t len = dot - ref;
                snprintf(default_dep_file_buf, sizeof(default_dep_file_buf), "%.*s.d", (int)len, ref);
            } else {
                snprintf(default_dep_file_buf, sizeof(default_dep_file_buf), "%s.d", ref);
            }
            final_dep_file = default_dep_file_buf;
        } else if (main_file) {
            const char *slash = strrchr(main_file, '/');
            const char *base = slash ? slash + 1 : main_file;
            const char *dot = strrchr(base, '.');
            if (dot) {
                size_t len = dot - base;
                snprintf(default_dep_file_buf, sizeof(default_dep_file_buf), "%.*s.d", (int)len, base);
            } else {
                snprintf(default_dep_file_buf, sizeof(default_dep_file_buf), "%s.d", base);
            }
            final_dep_file = default_dep_file_buf;
        }
    }

    /* Sprint 42: pass the main file path so (a) quoted includes in the
     * main file resolve relative to its own directory (C99 6.10.2p2,
     * not the process CWD), and (b) a missing include file becomes a
     * hard error with a diagnostic instead of a silent skip. */
    char include_err[PATH_MAX + 96];
    include_err[0] = '\0';
    char *out = zero_cc_preprocess_ext3(source, &inc_dirs, &defines,
                                        main_file, include_err, sizeof(include_err),
                                        final_dep_target, final_dep_file, opts->gen_deps,
                                        opts->force_include_count > 0 ? (const char **)opts->force_includes : NULL,
                                        opts->force_include_count);
    if (!out && include_err[0]) {
        fprintf(stderr, "%s\n", include_err);
    }
    return out;
}

/* Compile one translation unit for JIT execution. Returns the compiler on
 * success (caller owns it), NULL on failure (diagnostics already printed). */
static ZeroCCCompiler *jit_compile_tu(CompileOptions *opts, const char *input) {
    char *source = read_file(input);
    if (!source) {
        return NULL;
    }

    if (opts->verbose) {
        const char *opt_name = "O0";
        switch (opts->opt_level) {
            case 0: opt_name = "O0"; break;
            case 1: opt_name = "O1"; break;
            case 2: opt_name = "O2"; break;
            case 3: opt_name = "O3"; break;
        }
        printf("Compiling %s with -O%s (target: %s)\n", input, opt_name,
               opts->target_arch ? opts->target_arch : "x86-64");
    }

    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) {
        perror("calloc");
        free(source);
        return NULL;
    }

    ZeroCCISA target_isa = parse_target_arch(opts->target_arch);
    zero_cc_init_compiler_with_target(comp, target_isa);
    comp->opt_level = (OptLevel)opts->opt_level;
    comp->omit_frame_pointer = opts->omit_frame_pointer;
    comp->no_builtin_all = opts->no_builtin_all != 0;
    comp->no_builtin_count = opts->no_builtin_count;
    for (int nb = 0; nb < opts->no_builtin_count; nb++) {
        snprintf(comp->no_builtin_names[nb], sizeof(comp->no_builtin_names[nb]),
                 "%s", opts->no_builtin[nb]);
    }
    comp->sanitize_overflow = opts->sanitize_overflow != 0;

    char *prep = preprocess_with_options(source, opts, input, opts->dep_target);
    free(source);
    if (getenv("ZERO_CC_DUMP_CPP") && prep) {
        FILE *df = fopen(getenv("ZERO_CC_DUMP_CPP"), "w");
        if (df) { fputs(prep, df); fclose(df); }
    }
    if (!prep) {  /* include error already diagnosed by preprocess_with_options */
        zero_cc_compiler_free(comp);
        return NULL;
    }

    zero_cc_tokenize_c99(prep, comp);
    free(prep);

    if (opts->dump_ir) comp->dump_ir = 1;
    if (opts->dump_cfg) comp->dump_cfg = 1;
    if (opts->profile_opts) {
        opt_profile_enable(1);
        opt_profile_reset();
    }
    /* JIT mode: TLS variables live in the data section (see tls_backend.c) */
    comp->jit_mode = 1;

    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
        zero_cc_compiler_free(comp);
        return NULL;
    }
    return comp;
}

static int compile_jit(CompileOptions *opts) {
    ZeroCCCompiler *comp = jit_compile_tu(opts, opts->input_files[0]);
    if (!comp) {
        return 1;
    }

    if (opts->input_count > 1) {
        /* Multi-TU (T1): merge each remaining TU into the first one, then
         * run the single merged image (text/rodata/data appended with
         * re-based relocations; see zero_cc/core/multi_tu.h).
         *
         * Global-registry bookkeeping: every zero_cc_compile_c99 starts
         * with reset_global_vars(), so compiling TU_i wipes the merged
         * registry. We snapshot it before each compile and, after the
         * TU's own entries are re-based into the merged data layout,
         * re-append the snapshot (already in merged layout). */
        for (int i = 1; i < opts->input_count; i++) {
            ZeroCCCompiler *tu = jit_compile_tu(opts, opts->input_files[i]);
            if (!tu) {
                zero_cc_compiler_free(comp);
                return 1;
            }

            char err[256];
            if (zero_cc_tu_merge(comp, tu, err, sizeof(err)) != 0) {
                fprintf(stderr, "zerocc: error: %s\n", err);
                zero_cc_compiler_free(comp);
                zero_cc_compiler_free(tu);
                return 1;
            }
            /* Sections/tables were copied into `comp`; drop the per-TU one. */
            zero_cc_compiler_free(tu);
        }

        ZeroCCFunction *main_fn = zero_cc_find_function(comp, "main");
        if (!main_fn || main_fn->is_prototype) {
            fprintf(stderr, "zerocc: error: undefined reference to 'main'\n");
            zero_cc_compiler_free(comp);
            return 1;
        }

        int result = zero_cc_execute_jit(comp, NULL);
        if (opts->verbose) {
            printf("JIT execution result: %d\n", result);
        }
        zero_cc_compiler_free(comp);
        return result;
    }

    /* Single-TU: execute (the legacy -S branch here was unreachable - main()
     * routes emit_asm through compile_object - so it is gone). */
    if (opts->profile_opts) {
        opt_profile_dump(stderr);
    }

    int result = zero_cc_execute_jit(comp, NULL);
    if (opts->verbose) {
        printf("JIT execution result: %d\n", result);
    }

    zero_cc_compiler_free(comp);
    return result;
}

ZeroCCISA parse_target_arch(const char *target) {
    if (!target) return ISA_X86_64;  // Default
    if (strcmp(target, "x86-64") == 0 || strcmp(target, "x86_64") == 0) return ISA_X86_64;
    if (strcmp(target, "aarch64") == 0 || strcmp(target, "arm64") == 0) return ISA_AARCH64;
    if (strcmp(target, "riscv64") == 0 || strcmp(target, "riscv") == 0) return ISA_RISCV64;
    if (strcmp(target, "riscv32") == 0) return ISA_RISCV32;
    fprintf(stderr, "Warning: Unknown target architecture '%s', defaulting to x86-64\n", target);
    return ISA_X86_64;
}

void zero_cc_init_compiler_with_target(ZeroCCCompiler *comp, ZeroCCISA target_isa) {
    const ZeroCCISABackend *backend = zero_cc_get_backend(target_isa);
    if (!backend) {
        fprintf(stderr, "Error: Target architecture not supported\n");
        return;
    }
    comp->isa = target_isa;
    comp->isa_backend = backend;
    comp->omit_frame_pointer = -1; /* default: auto (omit at -O1+); CLI overrides */
    comp->local_lookup_bound = -1; /* default: unbounded */
    comp->parse_struct_body = zero_cc_parse_struct_body;

    /* Initialize IR arenas */
    if (!ir_arena_init(&comp->ir_arena, 4096)) {
        fprintf(stderr, "Error: Failed to initialize IR arena\n");
        return;
    }
    if (!ir_call_args_arena_init(&comp->ir_call_args)) {
        fprintf(stderr, "Error: Failed to initialize IR call args arena\n");
        ir_arena_free(&comp->ir_arena);
        return;
    }
}

static int compile_preprocess(CompileOptions *opts) {
    if (opts->output_file && opts->input_count > 1) {
        fprintf(stderr, "zerocc: error: cannot specify -o with -E and multiple input files\n");
        return 1;
    }
    int rc = 0;
    for (int i = 0; i < opts->input_count; i++) {
        const char *input = opts->input_files[i];
        char *source = read_file(input);
        if (!source) {
            perror(input);
            return 1;
        }
        /* gcc parity: for -E the default -MMD target/dep file are derived
         * from the input name (a.c -> a.d, target a.o), not from -o, so
         * clear output_file from the dependency defaults. */
        CompileOptions eopts = *opts;
        eopts.output_file = NULL;
        char *prep = preprocess_with_options(source, &eopts, input, NULL);
        free(source);
        if (!prep) {  /* include error already diagnosed by preprocess_with_options */
            return 1;
        }
        if (opts->output_file && i == 0) {
            FILE *f = fopen(opts->output_file, "w");
            if (!f) {
                perror(opts->output_file);
                free(prep);
                return 1;
            }
            fputs(prep, f);
            fclose(f);
        } else {
            fputs(prep, stdout);
        }
        free(prep);
    }
    return rc;
}

static int compile_object(CompileOptions *opts) {
    if (opts->output_file && opts->input_count > 1) {
        fprintf(stderr, "zerocc: error: cannot specify -o with -c/-S and multiple input files\n");
        return 1;
    }
    /* T1: one object/assembly output per input file. A failure in one
     * input does not stop the rest (matching gcc's batch behavior). */
    int rc = 0;
    for (int i = 0; i < opts->input_count; i++) {
        int r = compile_object_single(opts, opts->input_files[i],
                                      opts->output_file);
        if (r != 0) rc = r;
    }
    return rc;
}

/* Derive a default output name from an input path: replace the extension
 * with `ext` (append if the input has no extension).  Returns `output`
 * unchanged when non-NULL; otherwise writes the derived name into `buf`. */
static const char *derive_default_output(const char *input, const char *output,
                                         const char *ext, char *buf, size_t cap) {
    if (output) return output;
    const char *base = input;
    const char *dot = strrchr(base, '.');
    const char *slash = strrchr(base, '/');
    if (slash && (!dot || slash > dot)) base = slash + 1;
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    if (len > cap - 4) len = cap - 4;
    memcpy(buf, base, len);
    strcpy(buf + len, ext);
    return buf;
}

static int compile_object_single(CompileOptions *opts, const char *input,
                                 const char *output) {
    /* Read source file */
    char *source = read_file(input);
    if (!source) {
        return 1;
    }

    /* Determine output filename */
    static char output_buf[512];
    const char *output_path = derive_default_output(input, output, ".o",
                                                    output_buf, sizeof(output_buf));

    if (opts->verbose) {
        printf("Compiling %s to object file (target: %s)\n", input,
               opts->target_arch ? opts->target_arch : "x86-64");
    }

    /* Determine dep_target for -MMD/-MF/-MT: defaults to the output
     * basename, ".s" under -S (matching gcc's default target). */
    const char *dep_target = opts->dep_target;
    if (!dep_target) {
        static char dep_target_buf[512];
        const char *ref = derive_default_output(input, output,
                                                opts->emit_asm ? ".s" : ".o",
                                                dep_target_buf,
                                                sizeof(dep_target_buf));
        const char *slash = strrchr(ref, '/');
        dep_target = slash ? slash + 1 : ref;
    }

    /* Create compiler */
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) {
        perror("calloc");
        free(source);
        return 1;
    }

    /* Initialize target ISA */
    ZeroCCISA target_isa = parse_target_arch(opts->target_arch);
    zero_cc_init_compiler_with_target(comp, target_isa);

    /* Store optimization level in compiler for use by ast_lowering */
    comp->opt_level = (OptLevel)opts->opt_level;
    comp->omit_frame_pointer = opts->omit_frame_pointer;
    comp->no_builtin_all = opts->no_builtin_all != 0;
    comp->no_builtin_count = opts->no_builtin_count;
    for (int nb = 0; nb < opts->no_builtin_count; nb++) {
        snprintf(comp->no_builtin_names[nb], sizeof(comp->no_builtin_names[nb]),
                 "%s", opts->no_builtin[nb]);
    }
    comp->sanitize_overflow = opts->sanitize_overflow != 0;

    /* Preprocess (with -I include dirs, -D defines, and -MMD/-MF/-MT) */
    char *prep = preprocess_with_options(source, opts, input, dep_target);
    free(source);
    if (getenv("ZERO_CC_DUMP_CPP") && prep) {
        FILE *df = fopen(getenv("ZERO_CC_DUMP_CPP"), "w");
        if (df) { fputs(prep, df); fclose(df); }
    }
    if (!prep) {  /* include error already diagnosed by preprocess_with_options */
        zero_cc_compiler_free(comp);
        return 1;
    }

    /* Tokenize */
    zero_cc_tokenize_c99(prep, comp);
    free(prep);

    /* Enable IR dump if requested */
    if (opts->dump_ir) {
        comp->dump_ir = 1;
    }

    /* Enable CFG dump if requested (Sprint 35 Phase 2) */
    if (opts->dump_cfg) {
        comp->dump_cfg = 1;
    }

    /* Enable optimization profiling if requested (Sprint 35 Phase 4: the
     * AOT path previously dumped a disabled profile). */
    if (opts->profile_opts) {
        opt_profile_enable(1);
        opt_profile_reset();
    }

    /* Enable assembly emission if requested */
    if (opts->emit_asm) {
        comp->emit_asm = 1;
        /* Open output file for assembly (source.c -> source.s) */
        static char out_buf[512];
        const char *out_path = derive_default_output(input, output, ".s",
                                                     out_buf, sizeof(out_buf));
        comp->asm_out = fopen(out_path, "w");
        if (!comp->asm_out) {
            perror("fopen");
            zero_cc_compiler_free(comp);
            return 1;
        }
        if (opts->verbose) {
            printf("Writing assembly to: %s\n", out_path);
        }
    }

    /* Compile */
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
        zero_cc_compiler_free(comp);
        return 1;
    }

    /* Dump optimization profile if requested */
    if (opts->profile_opts) {
        opt_profile_dump(stderr);
    }

    if (opts->verbose) {
        printf("Writing object file: %s\n", output_path);
    }

    /* Determine ELF machine type for target */
    ElfMachineType elf_machine;
    switch (comp->isa) {
        case ISA_X86_64: elf_machine = ELF_MACHINE_X86_64; break;
        case ISA_AARCH64: elf_machine = ELF_MACHINE_AARCH64; break;
        case ISA_RISCV64: elf_machine = ELF_MACHINE_RISCV64; break;
        case ISA_RISCV32: elf_machine = ELF_MACHINE_RISCV32; break;
        default: elf_machine = ELF_MACHINE_X86_64; break;
    }

    /* Initialize ELF writer */
    ElfWriterCtx elf_ctx;
    elf_writer_init(&elf_ctx, elf_machine);

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

    /* Add .data section - the section alignment must be at least the
     * maximum alignment of any object it holds, or the linker places
     * the section (and its _Alignas(N) objects) too low (C11 6.7.5). */
    size_t data_align = 8;
    for (size_t gi = 0; gi < comp->global_var_count; gi++) {
        ZeroCCGlobalVar *gv = &comp->global_vars[gi];
        if (gv->is_bss || gv->is_tls) continue;
        size_t ga = gv->type.align > 0 ? gv->type.align : 8;
        if (ga > data_align) data_align = ga;
    }
    if (comp->data_section.length > 0) {
        elf_writer_add_section(&elf_ctx, ".data",
                               (uint8_t*)comp->data_section.data,
                               comp->data_section.length,
                               ELF_SHT_PROGBITS,
                               ELF_SHF_ALLOC | ELF_SHF_WRITE,
                               data_align);
    }

    /* Add .bss section (zero-initialized globals above threshold) */
    size_t bss_total = 0;
    size_t bss_align = 8;
    for (size_t i = 0; i < comp->global_var_count; i++) {
        if (comp->global_vars[i].is_bss && !comp->global_vars[i].is_tls) {
            size_t end = comp->global_vars[i].data_offset + comp->global_vars[i].size;
            if (end > bss_total) bss_total = end;
            size_t ga = comp->global_vars[i].type.align > 0 ? comp->global_vars[i].type.align : 8;
            if (ga > bss_align) bss_align = ga;
        }
    }
    if (bss_total > 0) {
        /* NOBITS section - no file payload, just zero-filled at load */
        elf_writer_add_section(&elf_ctx, ".bss",
                               NULL, bss_total,
                               ELF_SHT_NOBITS,
                               ELF_SHF_ALLOC | ELF_SHF_WRITE,
                               bss_align);
    }

    /* Add .tdata (initialized TLS) and .tbss (uninitialized TLS) sections.
     * The TLS backend (tls_backend.c) packs the block; each _Thread_local
     * variable gets a symbol in its section, and OP_TLS_LEA accesses get
     * R_X86_64_TPOFF32 relocations (initial-exec static TLS). */
    size_t tls_align = comp->tls_block_align > 0 ? comp->tls_block_align : 8;
    if (comp->tls_data_section.length > 0) {
        elf_writer_add_section(&elf_ctx, ".tdata",
                               (uint8_t*)comp->tls_data_section.data,
                               comp->tls_data_section.length,
                               ELF_SHT_PROGBITS,
                               ELF_SHF_ALLOC | ELF_SHF_WRITE | 0x400 /* SHF_TLS */,
                               tls_align);
    }
    if (comp->tls_bss_size > 0) {
        uint8_t *tbss_zeros = (uint8_t *)calloc(comp->tls_bss_size, 1);
        if (tbss_zeros) {
            elf_writer_add_section(&elf_ctx, ".tbss",
                                   tbss_zeros, comp->tls_bss_size,
                                   ELF_SHT_NOBITS,
                                   ELF_SHF_ALLOC | ELF_SHF_WRITE | 0x400 /* SHF_TLS */,
                                   tls_align);
            free(tbss_zeros);
        }
    }

    /* ---- Named sections: GNU __attribute__((section("name"))) ----
     * Objects placed in a named ELF section.  Their bytes are copied out of
     * data_section into a dedicated PROGBITS section (NOBITS for BSS-like),
     * and each object's symbol is emitted in that named section (below).
     * Relocations target the symbol, so every reference resolves to the named
     * section.  The .data copy of the bytes is left in place (dead, and no
     * symbol points to it).  Function-section routing is not supported. */
    {
        size_t gvn = comp->global_var_count;
        enum { MAX_NS = 16 };
        char ns_names[MAX_NS][64];
        size_t ns_size[MAX_NS];
        size_t ns_align[MAX_NS];
        bool ns_bss[MAX_NS];
        int ns_count = 0;
        for (int k = 0; k < MAX_NS; k++) { ns_size[k] = 0; ns_align[k] = 8; ns_bss[k] = false; }

        for (size_t i = 0; i < gvn; i++) {
            ZeroCCGlobalVar *gv = &comp->global_vars[i];
            if (!gv->name[0] || !gv->section[0] || gv->alias_target[0] || gv->is_tls) continue;
            int si = -1;
            for (int k = 0; k < ns_count; k++)
                if (strcmp(ns_names[k], gv->section) == 0) { si = k; break; }
            if (si < 0) {
                if (ns_count >= MAX_NS) { gv->section[0] = '\0'; continue; } /* degrade to .data */
                si = ns_count++;
                strncpy(ns_names[si], gv->section, 63);
                ns_names[si][63] = '\0';
                ns_size[si] = 0; ns_align[si] = 8; ns_bss[si] = false;
            }
            size_t al = gv->type.align > 0 ? gv->type.align : 8;
            if (al > ns_align[si]) ns_align[si] = al;
            size_t cur = ns_size[si] + ((al - (ns_size[si] % al)) % al);
            gv->named_sec_off = cur;
            cur += gv->size;
            if (cur > ns_size[si]) ns_size[si] = cur;
            if (gv->is_bss) ns_bss[si] = true;
        }

        for (int k = 0; k < ns_count; k++) {
            size_t total = ns_size[k];
            if (total == 0) continue;
            if (ns_bss[k]) {
                elf_writer_add_section(&elf_ctx, ns_names[k], NULL, total,
                                       ELF_SHT_NOBITS, ELF_SHF_ALLOC | ELF_SHF_WRITE,
                                       ns_align[k]);
            } else {
                uint8_t *buf = (uint8_t *)calloc(total, 1);
                if (!buf) continue;
                for (size_t i = 0; i < gvn; i++) {
                    ZeroCCGlobalVar *gv = &comp->global_vars[i];
                    if (strcmp(gv->section, ns_names[k]) != 0) continue;
                    if (!gv->is_bss && gv->size > 0 &&
                        gv->data_offset + gv->size <= comp->data_section.length) {
                        memcpy(buf + gv->named_sec_off,
                               comp->data_section.data + gv->data_offset, gv->size);
                    }
                }
                elf_writer_add_section(&elf_ctx, ns_names[k], buf, total,
                                       ELF_SHT_PROGBITS, ELF_SHF_ALLOC | ELF_SHF_WRITE,
                                       ns_align[k]);
                free(buf); /* copied into the writer */
            }
        }
    }

    /* ---- Named text sections: GNU __attribute__((section("name"))) on functions ----
     * A function's code is emitted into text_section as usual (JIT-safe).  At -c
     * emission, each function with a section is copied out of text_section into a
     * dedicated PROGBITS|EXECINSTR section and its symbol is emitted there (below).
     * The .text copy is left in place (dead, and no symbol points to it).  A
     * function's extent in text_section is [text_offset, next-function-text_offset)
     * (or to the end of the section for the last one); a contiguous copy preserves
     * the function's internal relative offsets. */
    {
        size_t fcn = comp->function_count;
        enum { MAX_NTS = 16 };
        char nts_names[MAX_NTS][64];
        size_t nts_size[MAX_NTS];
        int nts_count = 0;
        for (int k = 0; k < MAX_NTS; k++) { nts_size[k] = 0; }

        size_t *extent_end = (size_t *)calloc(fcn ? fcn : 1, sizeof(size_t));
        size_t *order = (size_t *)calloc(fcn ? fcn : 1, sizeof(size_t));
        size_t nreal = 0;
        for (size_t i = 0; i < fcn; i++) {
            ZeroCCFunction *f = &comp->functions[i];
            if (f->name[0] && !f->is_prototype && !f->is_external && !f->is_object) {
                order[nreal++] = i;
            }
        }
        /* insertion sort by text_offset (n is small) */
        for (size_t i = 1; i < nreal; i++) {
            size_t key = order[i]; size_t j = i;
            while (j > 0 && comp->functions[order[j - 1]].text_offset > comp->functions[key].text_offset) {
                order[j] = order[j - 1]; j--;
            }
            order[j] = key;
        }
        for (size_t i = 0; i < nreal; i++) {
            size_t fi = order[i];
            size_t end = (i + 1 < nreal) ? comp->functions[order[i + 1]].text_offset
                                         : comp->text_section.length;
            extent_end[fi] = end;
        }

        /* lay out sectioned functions in named sections (in text_offset order) */
        for (size_t i = 0; i < nreal; i++) {
            size_t fi = order[i];
            ZeroCCFunction *f = &comp->functions[fi];
            if (!f->section[0]) continue;
            int si = -1;
            for (int k = 0; k < nts_count; k++)
                if (strcmp(nts_names[k], f->section) == 0) { si = k; break; }
            if (si < 0) {
                if (nts_count >= MAX_NTS) { f->section[0] = '\0'; continue; } /* degrade to .text */
                si = nts_count++;
                strncpy(nts_names[si], f->section, 63);
                nts_names[si][63] = '\0';
                nts_size[si] = 0;
            }
            f->named_text_sec_off = nts_size[si];
            nts_size[si] += extent_end[fi] - f->text_offset;
        }

        /* emit the named text sections (copy the code bytes out of text_section) */
        for (int k = 0; k < nts_count; k++) {
            size_t total = nts_size[k];
            if (total == 0) continue;
            uint8_t *buf = (uint8_t *)calloc(total, 1);
            if (!buf) continue;
            for (size_t i = 0; i < nreal; i++) {
                size_t fi = order[i];
                ZeroCCFunction *f = &comp->functions[fi];
                if (strcmp(f->section, nts_names[k]) != 0) continue;
                size_t sz = extent_end[fi] - f->text_offset;
                if (f->text_offset + sz <= comp->text_section.length)
                    memcpy(buf + f->named_text_sec_off,
                           comp->text_section.data + f->text_offset, sz);
            }
            elf_writer_add_section(&elf_ctx, nts_names[k], buf, total,
                                   ELF_SHT_PROGBITS, ELF_SHF_ALLOC | ELF_SHF_EXECINSTR, 16);
            free(buf); /* copied into the writer */
        }
        free(extent_end);
        free(order);
    }

    /* Resolve real section indices: empty sections are not emitted, so the
     * positions of later sections shift (a no-.text object has .data at 1). */
    uint16_t text_idx = elf_writer_section_index(&elf_ctx, ".text");
    uint16_t rodata_idx = elf_writer_section_index(&elf_ctx, ".rodata");
    uint16_t data_idx = elf_writer_section_index(&elf_ctx, ".data");
    uint16_t tdata_idx = elf_writer_section_index(&elf_ctx, ".tdata");
    uint16_t tbss_idx = elf_writer_section_index(&elf_ctx, ".tbss");
    uint16_t bss_idx = elf_writer_section_index(&elf_ctx, ".bss");

    /* Add .rodata section symbol first (needed for string literal relocations) */
    /* Section symbols must come before global symbols for correct sh_info calculation */
    if (comp->rodata_section.length > 0) {
        elf_writer_add_symbol(&elf_ctx, ".rodata",
                              ELF_STB_LOCAL, ELF_STT_SECTION,
                              rodata_idx,  /* .rodata section index */
                              0, 0);
    }
    if (comp->data_section.length > 0) {
        elf_writer_add_symbol(&elf_ctx, ".data",
                              ELF_STB_LOCAL, ELF_STT_SECTION,
                              data_idx,  /* .data section index */
                              0, 0);
    }
    if (comp->text_section.length > 0) {
        elf_writer_add_symbol(&elf_ctx, ".text",
                              ELF_STB_LOCAL, ELF_STT_SECTION,
                              text_idx,  /* .text section index */
                              0, 0);
    }

    /* Add symbols for functions */
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *fn = &comp->functions[i];
        /* Nested functions may share source names across enclosing
         * functions; they are emitted (and referenced by call relocs) under
         * their unique mangled name. `__asm`-labeled functions are renamed
         * the same way (zero_cc_fn_emit_symbol covers both). */
        const char *fn_sym = zero_cc_fn_emit_symbol(fn);
        if (fn->name[0]) {
            if (fn->is_prototype) {
                /* Skip forward prototype if a real definition exists in this TU */
                bool has_def = false;
                for (size_t k = 0; k < comp->function_count; k++) {
                    if (k != i && !comp->functions[k].is_prototype && !comp->functions[k].is_external &&
                        strcmp(comp->functions[k].name, fn->name) == 0) {
                        has_def = true;
                        break;
                    }
                }
                if (has_def) continue;
                /* Prototypes for functions that are declared but never CALLED
                 * in this TU must not become undefined global symbols in the
                 * object: GNU ld pulls archive members to resolve them, so a
                 * mere <stdio.h> include would drag in musl's vfprintf/vfscanf
                 * objects (whose soft-float 128-bit helpers are absent from
                 * the riscv32 musl cross shim) and break the link. */
                bool prot_ref = false;
                for (size_t r = 0; r < comp->reloc_count; r++) {
                    if (strcmp(comp->relocs[r].target_symbol, fn_sym) == 0) {
                        prot_ref = true;
                        break;
                    }
                }
                if (!prot_ref) continue;
            }
            if (fn->is_external) {
                bool referenced = false;
                for (size_t r = 0; r < comp->reloc_count; r++) {
                    if (strcmp(comp->relocs[r].target_symbol, fn_sym) == 0) {
                        referenced = true;
                        break;
                    }
                }
                if (!referenced) continue;
            }

            /* Estimate function size (simplified - in reality would need more analysis) */
            size_t fn_size = 0;  /* Could be computed from next function's offset */
            int binding = fn->is_static ? ELF_STB_LOCAL : ELF_STB_GLOBAL;
            int sym_type = fn->is_external ? ELF_STT_NOTYPE : ELF_STT_FUNC;
            if (fn->section[0]) {
                /* Sprint 108: function in a named section - symbol lives in the
                 * named text section at its placed offset.  If the section was not
                 * emitted (zero size), fall through to .text where the code is. */
                uint16_t nti = elf_writer_section_index(&elf_ctx, fn->section);
                if (nti != 0) {
                    elf_writer_add_symbol(&elf_ctx, fn_sym,
                                          binding, sym_type,
                                          nti, fn->named_text_sec_off, fn_size);
                    continue;
                }
            }
            int section_idx = SHN_UNDEF;  // Default to undefined
            if (!fn->is_prototype && !fn->is_external) {
                section_idx = text_idx;  // Compiled functions go in .text
            }
            elf_writer_add_symbol(&elf_ctx, fn_sym,
                                  binding, sym_type,
                                  section_idx,
                                  fn->text_offset,
                                  fn_size);
        }
    }

    /* Add symbols for global variables (.data / .bss sections) */
    int data_section_idx = data_idx;
    int bss_section_idx = bss_idx;
    for (size_t i = 0; i < comp->global_var_count; i++) {
        ZeroCCGlobalVar *gv = &comp->global_vars[i];
        if (gv->name[0]) {
            /* GNU alias entry: no storage, no ELF symbol (uses resolve to
             * the target's symbol at lowering time). */
            if (gv->alias_target[0]) continue;
            int binding = gv->is_static ? ELF_STB_LOCAL : ELF_STB_GLOBAL;
            if (gv->is_tls) {
                /* TLS variable: symbol lives in .tdata or .tbss at its
                 * in-block offset (data_offset was patched by finalize). */
                size_t tdata_len = comp->tls_data_section.length;
                if (gv->data_offset < tdata_len) {
                    elf_writer_add_symbol(&elf_ctx, gv->name,
                                          binding, ELF_STT_OBJECT,
                                          tdata_idx,
                                          gv->data_offset,
                                          gv->size);
                } else {
                    elf_writer_add_symbol(&elf_ctx, gv->name,
                                          binding, ELF_STT_OBJECT,
                                          tbss_idx,
                                          gv->data_offset - tdata_len,
                                          gv->size);
                }
                continue;
            }
            if (gv->section[0]) {
                /* Named-section object: symbol lives in the named section at
                 * its placed offset.  If the section was not emitted (zero
                 * total size), fall through to .data/.bss where the bytes are. */
                uint16_t nsi = elf_writer_section_index(&elf_ctx, gv->section);
                if (nsi != 0) {
                    elf_writer_add_symbol(&elf_ctx, gv->name,
                                          binding, ELF_STT_OBJECT,
                                          nsi, gv->named_sec_off, gv->size);
                    continue;
                }
            }
            if (gv->is_bss) {
                elf_writer_add_symbol(&elf_ctx, gv->name,
                                      binding, ELF_STT_OBJECT,
                                      bss_section_idx,
                                      gv->data_offset,
                                      gv->size);
            } else {
                elf_writer_add_symbol(&elf_ctx, gv->name,
                                      binding, ELF_STT_OBJECT,
                                      data_section_idx,
                                      gv->data_offset,
                                      gv->size);
            }
        }
    }

    /* Architecture-specific relocation types */
    int reloc_type_call, reloc_type_rodata;
    int addend_call, addend_rodata;

    switch (comp->isa) {
        case ISA_X86_64:
            reloc_type_call = 4;   /* R_X86_64_PLT32 */
            reloc_type_rodata = 2; /* R_X86_64_PC32 */
            addend_call = -4;
            addend_rodata = -4;
            break;
        case ISA_AARCH64:
            reloc_type_call = 283;  /* R_AARCH64_CALL26 */
            reloc_type_rodata = 274; /* R_AARCH64_ADR_PREL_LO21 */
            addend_call = 0;
            addend_rodata = 0;
            break;
        case ISA_RISCV64:
        case ISA_RISCV32:
            reloc_type_call = 17;   /* R_RISCV_JAL (single 4-byte jal ra) */
            reloc_type_rodata = 24; /* R_RISCV_PCREL_LO12_I */
            addend_call = 0;
            addend_rodata = 0;
            break;
        default:
            reloc_type_call = 4;
            reloc_type_rodata = 2;
            addend_call = -4;
            addend_rodata = -4;
            break;
    }

    /* Add relocation entries for external function calls and symbol address loads */
    int reloc_type_hi20 = 23;  /* R_RISCV_PCREL_HI20 (auipc half of a global load) */
    for (size_t i = 0; i < comp->reloc_count; i++) {
        ZeroCCReloc *r = &comp->relocs[i];
        int symbol_idx = 0;
        for (int j = 0; j < elf_ctx.symbol_count; j++) {
            if (strcmp(elf_ctx.strtab + elf_ctx.symbols[j].name_offset, r->target_symbol) == 0) {
                symbol_idx = j + 1;
                break;
            }
        }
        /* GCC __builtin_X -> X call alias: at lowering time the alias target
         * may not have been visible yet (cross-TU: X is defined in another
         * input file, or is a libc symbol - the FORTIFY __X_chk family).
         * Without this the call keeps the __builtin_X name, matches no
         * object symbol, and the binary carries a `call +0` stub that
         * segfaults if executed (c-torture execute/builtins *chk.c). The
         * dlsym probe keeps unknown __builtin_X names on the legacy stub
         * path rather than inventing phantom undefined symbols that could
         * break static cross links. */
        if (symbol_idx == 0 && r->is_call &&
            strncmp(r->target_symbol, "__builtin_", 10) == 0) {
            const char *alias = r->target_symbol + 10;
            for (int j = 0; j < elf_ctx.symbol_count; j++) {
                if (strcmp(elf_ctx.strtab + elf_ctx.symbols[j].name_offset, alias) == 0) {
                    symbol_idx = j + 1;
                    break;
                }
            }
            if (symbol_idx == 0 && dlsym(RTLD_DEFAULT, alias)) {
                elf_writer_add_symbol(&elf_ctx, alias, ELF_STB_GLOBAL,
                                      ELF_STT_FUNC, SHN_UNDEF, 0, 0);
                symbol_idx = (int)elf_ctx.symbol_count;
            }
        }
        if (r->is_call) {
            elf_writer_add_relocation(&elf_ctx, text_idx,  /* .text section */
                                      r->text_patch_offset,
                                      symbol_idx,
                                      reloc_type_call,
                                      addend_call);
        } else if (r->is_tls) {
            /* Static TLS (initial-exec, x86-64): the 32-bit displacement of
             * lea reg, [fs+disp32] gets R_X86_64_TPOFF32 against the
             * variable's .tdata/.tbss symbol. */
            elf_writer_add_relocation(&elf_ctx, text_idx,
                                      r->text_patch_offset,
                                      symbol_idx,
                                      0x17 /* R_X86_64_TPOFF32 */,
                                      0);
        } else if (symbol_idx > 0) {
            if ((comp->isa == ISA_RISCV64 || comp->isa == ISA_RISCV32) &&
                r->text_patch_offset >= 4) {
                /* RISC-V psABI: The auipc instruction at (patch_offset - 4) gets
                 * R_RISCV_PCREL_HI20 targeting symbol_idx (with addend 0).
                 * The following instruction at patch_offset gets R_RISCV_PCREL_LO12_I
                 * targeting a local label placed at the auipc's address (with addend 0). */
                char lbl[64];
                snprintf(lbl, sizeof(lbl), ".Lpcrel_sym_%zu", i);
                elf_writer_add_symbol(&elf_ctx, lbl,
                                      ELF_STB_LOCAL, ELF_STT_NOTYPE,
                                      text_idx /* .text */,
                                      r->text_patch_offset - 4,
                                      0);
                int label_sym_idx = elf_ctx.symbol_count;

                elf_writer_add_relocation(&elf_ctx, text_idx,
                                          r->text_patch_offset - 4,
                                          symbol_idx,
                                          reloc_type_hi20,
                                          0);
                elf_writer_add_relocation(&elf_ctx, text_idx,
                                          r->text_patch_offset,
                                          label_sym_idx,
                                          reloc_type_rodata, /* R_RISCV_PCREL_LO12_I (24) */
                                          0);
            } else if (comp->isa == ISA_AARCH64 && r->text_patch_offset >= 4) {
                /* AArch64: adrp at (patch_offset - 4) gets R_AARCH64_ADR_PREL_PG_HI21 (275),
                 * add at patch_offset gets R_AARCH64_ADD_ABS_LO12_NC (277). */
                elf_writer_add_relocation(&elf_ctx, text_idx,
                                          r->text_patch_offset - 4,
                                          symbol_idx,
                                          275 /* R_AARCH64_ADR_PREL_PG_HI21 */,
                                          0);
                elf_writer_add_relocation(&elf_ctx, text_idx,
                                          r->text_patch_offset,
                                          symbol_idx,
                                          277 /* R_AARCH64_ADD_ABS_LO12_NC */,
                                          0);
            } else {
                int reloc_type_abs64 = (comp->isa == ISA_X86_64) ? 1 /* R_X86_64_64 */ : reloc_type_rodata;
                elf_writer_add_relocation(&elf_ctx, text_idx,
                                          r->text_patch_offset,
                                          symbol_idx,
                                          reloc_type_abs64,
                                          0);
            }
        }
    }

    /* Add relocation entries for string literals (.rodata references) */
    int rodata_symbol_idx = 1;

    for (size_t i = 0; i < comp->str_reloc_count; i++) {
        ZeroCCStringReloc *sr = &comp->str_relocs[i];
        if ((comp->isa == ISA_RISCV64 || comp->isa == ISA_RISCV32) &&
            sr->text_patch_offset >= 4) {
            /* RISC-V psABI: The auipc instruction at (patch_offset - 4) gets
             * R_RISCV_PCREL_HI20 targeting .rodata section symbol with rodata_offset.
             * The following instruction (addi/fld) at patch_offset gets
             * R_RISCV_PCREL_LO12_I targeting a local label at the auipc with addend 0. */
            char lbl[64];
            snprintf(lbl, sizeof(lbl), ".Lpcrel_str_%zu", i);
            elf_writer_add_symbol(&elf_ctx, lbl,
                                  ELF_STB_LOCAL, ELF_STT_NOTYPE,
                                  text_idx /* .text */,
                                  sr->text_patch_offset - 4,
                                  0);
            int label_sym_idx = elf_ctx.symbol_count;

            elf_writer_add_relocation(&elf_ctx, text_idx,
                                      sr->text_patch_offset - 4,
                                      rodata_symbol_idx,
                                      reloc_type_hi20,
                                      (int64_t)sr->rodata_offset + addend_rodata);
            elf_writer_add_relocation(&elf_ctx, text_idx,
                                      sr->text_patch_offset,
                                      label_sym_idx,
                                      reloc_type_rodata, /* R_RISCV_PCREL_LO12_I (24) */
                                      0);
        } else if (comp->isa == ISA_AARCH64 && sr->text_patch_offset >= 4) {
            /* AArch64: adrp at (patch_offset - 4) gets R_AARCH64_ADR_PREL_PG_HI21 (275),
             * add at patch_offset gets R_AARCH64_ADD_ABS_LO12_NC (277). */
            elf_writer_add_relocation(&elf_ctx, text_idx,
                                      sr->text_patch_offset - 4,
                                      rodata_symbol_idx,
                                      275 /* R_AARCH64_ADR_PREL_PG_HI21 */,
                                      (int64_t)sr->rodata_offset);
            elf_writer_add_relocation(&elf_ctx, text_idx,
                                      sr->text_patch_offset,
                                      rodata_symbol_idx,
                                      277 /* R_AARCH64_ADD_ABS_LO12_NC */,
                                      (int64_t)sr->rodata_offset);
        } else {
            elf_writer_add_relocation(&elf_ctx, text_idx,
                                      sr->text_patch_offset,
                                      rodata_symbol_idx,
                                      reloc_type_rodata,
                                      (int64_t)sr->rodata_offset + addend_rodata);
        }
    }

    /* Sprint 99: nonlocal goto absolute jumps (adrp/auipc + add/jalr pairs
     * targeting an ancestor function's label). Each jump gets a local
     * label symbol at the target offset in .text; the ISA pair relocations
     * carry addend 0 (the label symbol IS the exact target). */
    for (size_t i = 0; i < comp->nl_goto_reloc_count; i++) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), ".Lnl_goto_%zu", i);
        elf_writer_add_symbol(&elf_ctx, lbl,
                              ELF_STB_LOCAL, ELF_STT_NOTYPE,
                              text_idx /* .text */,
                              comp->nl_goto_relocs[i].text_target_offset,
                              0);
        int label_sym_idx = elf_ctx.symbol_count;
        if (comp->isa == ISA_RISCV64 || comp->isa == ISA_RISCV32) {
            char auipc_lbl[64];
            snprintf(auipc_lbl, sizeof(auipc_lbl), ".Lnl_goto_auipc_%zu", i);
            elf_writer_add_symbol(&elf_ctx, auipc_lbl,
                                  ELF_STB_LOCAL, ELF_STT_NOTYPE,
                                  text_idx,
                                  comp->nl_goto_relocs[i].text_patch_offset - 4,
                                  0);
            int auipc_sym_idx = elf_ctx.symbol_count;
            elf_writer_add_relocation(&elf_ctx, text_idx,
                                      comp->nl_goto_relocs[i].text_patch_offset - 4,
                                      label_sym_idx,
                                      reloc_type_hi20, /* R_RISCV_PCREL_HI20 (23) */
                                      0);
            elf_writer_add_relocation(&elf_ctx, text_idx,
                                      comp->nl_goto_relocs[i].text_patch_offset,
                                      auipc_sym_idx,
                                      reloc_type_rodata, /* R_RISCV_PCREL_LO12_I (24) */
                                      0);
        } else if (comp->isa == ISA_AARCH64) {
            elf_writer_add_relocation(&elf_ctx, text_idx,
                                      comp->nl_goto_relocs[i].text_patch_offset - 4,
                                      label_sym_idx,
                                      275 /* R_AARCH64_ADR_PREL_PG_HI21 */,
                                      0);
            elf_writer_add_relocation(&elf_ctx, text_idx,
                                      comp->nl_goto_relocs[i].text_patch_offset,
                                      label_sym_idx,
                                      277 /* R_AARCH64_ADD_ABS_LO12_NC */,
                                      0);
        }
    }

    /* Relocations for .data pointer initializers (string literals -> .rodata,
     * &global -> another global's address). */
    int reloc_type_data_abs = 1; /* R_X86_64_64 */
    switch (comp->isa) {
        case ISA_X86_64:  reloc_type_data_abs = 1;   /* R_X86_64_64 */ break;
        case ISA_AARCH64: reloc_type_data_abs = 257; /* R_AARCH64_ABS64 */ break;
        case ISA_RISCV64: reloc_type_data_abs = 2;   /* R_RISCV_64 */ break;
        case ISA_RISCV32: reloc_type_data_abs = 1;   /* R_RISCV_32 */ break;
        default: break;
    }
    if (comp->data_section.length > 0) {
        for (size_t i = 0; i < comp->data_reloc_count; i++) {
            ZeroCCDataReloc *dr = &comp->data_relocs[i];
            if (dr->target_kind == 0) {
                /* .rodata + rodata_offset + target_data_offset: target the .rodata section symbol (idx 1) */
                if (comp->rodata_section.length > 0) {
                    elf_writer_add_relocation(&elf_ctx, data_section_idx, dr->data_patch_offset,
                                              1, reloc_type_data_abs,
                                              (int64_t)dr->rodata_offset + (int64_t)dr->target_data_offset);
                }
            } else if (dr->target_kind == 1 && dr->target_global_idx >= 0 &&
                       dr->target_global_idx < (int)comp->global_var_count) {
                /* &global: target the global variable's own symbol (value = data_offset).
                 * target_data_offset may carry a member/element extra offset
                 * (`&((arr + N)->member)`) - emitted as the reloc addend. */
                const char *tname = comp->global_vars[dr->target_global_idx].name;
                int sym_idx = 0;
                for (int j = 0; j < elf_ctx.symbol_count; j++) {
                    if (strcmp(elf_ctx.strtab + elf_ctx.symbols[j].name_offset, tname) == 0) { sym_idx = j + 1; break; }
                }
                if (sym_idx > 0) {
                    int64_t extra = (dr->target_data_offset == (size_t)-1) ? 0 :
                                    (int64_t)dr->target_data_offset -
                                    (int64_t)comp->global_vars[dr->target_global_idx].data_offset;
                    elf_writer_add_relocation(&elf_ctx, data_section_idx, dr->data_patch_offset,
                                              sym_idx, reloc_type_data_abs, extra);
                }
            } else if (dr->target_kind == 2 && dr->target_extern_name[0]) {
                /* &extern-global (defined in another TU): target the UND symbol
                 * by name. A definition visible in this object (e.g. a forward
                 * reference) is preferred; otherwise register the UND. */
                int sym_idx = 0;
                for (int j = 0; j < elf_ctx.symbol_count; j++) {
                    if (strcmp(elf_ctx.strtab + elf_ctx.symbols[j].name_offset,
                               dr->target_extern_name) == 0) { sym_idx = j + 1; break; }
                }
                if (sym_idx == 0) {
                    elf_writer_add_symbol(&elf_ctx, dr->target_extern_name,
                                          ELF_STB_GLOBAL, ELF_STT_NOTYPE,
                                          SHN_UNDEF, 0, 0);
                    sym_idx = elf_ctx.symbol_count;
                }
                elf_writer_add_relocation(&elf_ctx, data_section_idx, dr->data_patch_offset,
                                          sym_idx, reloc_type_data_abs, 0);
            } else if (dr->target_kind == 3 && dr->target_extern_name[0]) {
                /* Function pointer initializer: reloc against the function
                 * symbol (defined in this TU or UND). */
                int sym_idx = 0;
                for (int j = 0; j < elf_ctx.symbol_count; j++) {
                    if (strcmp(elf_ctx.strtab + elf_ctx.symbols[j].name_offset,
                               dr->target_extern_name) == 0) { sym_idx = j + 1; break; }
                }
                if (sym_idx == 0) {
                    elf_writer_add_symbol(&elf_ctx, dr->target_extern_name,
                                          ELF_STB_GLOBAL, ELF_STT_FUNC,
                                          SHN_UNDEF, 0, 0);
                    sym_idx = elf_ctx.symbol_count;
                }
                elf_writer_add_relocation(&elf_ctx, data_section_idx, dr->data_patch_offset,
                                          sym_idx, reloc_type_data_abs, 0);
            } else if (dr->target_kind == 4) {
                /* Absolute .data offset: address of an in-TU static local.
                 * Reloc against the .data section symbol (added above, before
                 * global symbols, for correct sh_info). */
                int dsym = 0;
                for (int j = 0; j < elf_ctx.symbol_count; j++) {
                    if (strcmp(elf_ctx.strtab + elf_ctx.symbols[j].name_offset, ".data") == 0) { dsym = j + 1; break; }
                }
                if (dsym == 0) {
                    elf_writer_add_symbol(&elf_ctx, ".data",
                                          ELF_STB_LOCAL, ELF_STT_SECTION,
                                          data_section_idx, 0, 0);
                    dsym = elf_ctx.symbol_count;
                }
                elf_writer_add_relocation(&elf_ctx, data_section_idx, dr->data_patch_offset,
                                          dsym, reloc_type_data_abs,
                                          (int64_t)dr->target_data_offset);
            } else if (dr->target_kind == 5) {
                /* Code label target: address in .text section */
                int tsym = 0;
                for (int j = 0; j < elf_ctx.symbol_count; j++) {
                    if (strcmp(elf_ctx.strtab + elf_ctx.symbols[j].name_offset, ".text") == 0) { tsym = j + 1; break; }
                }
                if (tsym == 0) {
                    elf_writer_add_symbol(&elf_ctx, ".text",
                                          ELF_STB_LOCAL, ELF_STT_SECTION,
                                          text_idx, 0, 0);
                    tsym = elf_ctx.symbol_count;
                }
                elf_writer_add_relocation(&elf_ctx, data_section_idx, dr->data_patch_offset,
                                          tsym, reloc_type_data_abs,
                                          (int64_t)dr->target_data_offset);
            }
        }
    }

    /* Write ELF file or emit assembly */
    if (opts->emit_asm) {
        /* Assembly emission is handled in ast_lowering.c; emit .rodata and .data */
        emit_asm_data_sections(comp);
        if (opts->verbose) {
            printf("Assembly emission complete\n");
        }
    } else if (!elf_writer_write(&elf_ctx, output_path)) {
        fprintf(stderr, "ELF writer error: %s\n", elf_writer_get_error(&elf_ctx));
        elf_writer_cleanup(&elf_ctx);
        zero_cc_compiler_free(comp);
        return 1;
    } else if (opts->verbose) {
        printf("Object file written successfully\n");
    }

    /* Cleanup */
    elf_writer_cleanup(&elf_ctx);
    zero_cc_compiler_free(comp);

    return 0;
}

static int compile_and_link(CompileOptions *opts) {
    /* Get target ISA (also decides the intermediate-object strategy below). */
    ZeroCCISA target_isa = parse_target_arch(opts->target_arch);

    /* All target ISAs (x86-64, aarch64, riscv64, riscv32) default to direct
     * RAW binary object emission via elf_writer, validated by the 44/44 cross-ISA
     * QEMU gate.
     *
     * ZC_TEXT_AOT=1 serves as an escape hatch to route through text assembly +
     * the system cross-assembler. */
    int use_text = (getenv("ZC_TEXT_AOT") != NULL);

    /* T1: one intermediate object per input file; the system linker
     * combines them. (With ZC_TEXT_AOT the intermediates are .s files, which
     * gcc assembles and links in one step.) */
    if (opts->verbose) {
        printf("Generating intermediate %s...\n",
               use_text ? "assembly (per input)" : "object file(s)");
    }

    char obj_paths[ZERO_CC_MAX_INPUTS][512];
    int obj_temp[ZERO_CC_MAX_INPUTS]; /* 1 = driver-created temp, safe to unlink */
    int obj_count = 0;
    const char *arch_paths[32];
    int arch_count = 0;
    CompileOptions obj_opts = *opts;
    obj_opts.emit_object = !use_text;
    obj_opts.emit_asm = use_text;
    obj_opts.output_file = NULL;  /* explicit per-TU path passed below */
    obj_opts.verbose = 0;  /* Suppress verbose during object generation */

    char default_link_dep_file[512];
    if (opts->gen_deps) {
        if (!obj_opts.dep_target && opts->output_file) {
            obj_opts.dep_target = opts->output_file;
        }
        if (!obj_opts.dep_file) {
            const char *ref = opts->output_file ? opts->output_file : "a.out";
            const char *dot = strrchr(ref, '.');
            if (dot) {
                size_t len = dot - ref;
                snprintf(default_link_dep_file, sizeof(default_link_dep_file), "%.*s.d", (int)len, ref);
            } else {
                snprintf(default_link_dep_file, sizeof(default_link_dep_file), "%s.d", ref);
            }
            obj_opts.dep_file = default_link_dep_file;
        }
    }

    for (int i = 0; i < opts->input_count; i++) {
        const char *in = opts->input_files[i];
        size_t len = strlen(in);
        /* T2: pass through prebuilt objects and archives to the linker. */
        if (len >= 2 && strcmp(in + len - 2, ".a") == 0) {
            if (opts->zerocc_link && arch_count < 32) {
                arch_paths[arch_count++] = in;
            } else if (use_text) {
                fprintf(stderr, "Error: %s: archive input with -S is not "
                        "supported\n", in);
                return 1;
            } else {
                /* System linker: pass the archive through in the link cmd. */
                if (arch_count < 32) arch_paths[arch_count++] = in;
            }
            continue;
        }
        if (len >= 2 && strcmp(in + len - 2, ".o") == 0) {
            snprintf(obj_paths[obj_count], sizeof(obj_paths[0]), "%s", in);
            obj_temp[obj_count] = 0;
            obj_count++;
            continue;
        }
        snprintf(obj_paths[obj_count], sizeof(obj_paths[0]), "/tmp/zerocc_%d_%d%s",
                 getpid(), obj_count, use_text ? ".s" : ".o");
        obj_temp[obj_count] = 1;
        int r = compile_object_single(&obj_opts, opts->input_files[i],
                                      obj_paths[obj_count]);
        if (r != 0) {
            fprintf(stderr, "Error: Compilation to object file failed\n");
            return r;
        }
        obj_count++;
    }

    char all_objs[4096] = "";
    for (int i = 0; i < obj_count; i++) {
        strncat(all_objs, " ", sizeof(all_objs) - strlen(all_objs) - 1);
        strncat(all_objs, obj_paths[i], sizeof(all_objs) - strlen(all_objs) - 1);
    }
    for (int i = 0; i < arch_count; i++) {
        strncat(all_objs, " ", sizeof(all_objs) - strlen(all_objs) - 1);
        strncat(all_objs, arch_paths[i], sizeof(all_objs) - strlen(all_objs) - 1);
    }

    const char *output = opts->output_file ? opts->output_file : "a.out";

    /* If target_arch wasn't explicitly set and we have input objects,
     * inspect the first ELF object to determine target_isa for linking. */
    if (!opts->target_arch && obj_count > 0) {
        FILE *f = fopen(obj_paths[0], "rb");
        if (f) {
            uint8_t hdr[20];
            if (fread(hdr, 1, 20, f) == 20 && hdr[0] == 0x7f && hdr[1] == 'E' &&
                hdr[2] == 'L' && hdr[3] == 'F') {
                uint16_t mach = (uint16_t)hdr[18] | ((uint16_t)hdr[19] << 8);
                if (mach == 183) target_isa = ISA_AARCH64;
                else if (mach == 243) target_isa = (hdr[4] == 1) ? ISA_RISCV32 : ISA_RISCV64;
                else if (mach == 62) target_isa = ISA_X86_64;
            }
            fclose(f);
        }
    }

    /* zero_rt runtime archive: build/lib/zero_rt.a, zero_rt_aarch64.a, or
     * zero_rt_riscv64.a next to this binary, or $ZERO_RT_A / $ZERO_RT_AARCH64_A /
     * $ZERO_RT_RISCV64_A. Auto-linked with --zerocc-link and provided as a
     * fallback archive after -lc -lm for the host/cross linker. */
    const char *rt_filename = "zero_rt.a";
    const char *env_var = "ZERO_RT_A";
    if (target_isa == ISA_AARCH64) {
        rt_filename = "zero_rt_aarch64.a";
        env_var = "ZERO_RT_AARCH64_A";
    } else if (target_isa == ISA_RISCV64) {
        rt_filename = "zero_rt_riscv64.a";
        env_var = "ZERO_RT_RISCV64_A";
    }

    char rt_path[2048] = "";
    const char *rt_env = getenv(env_var);
    if (rt_env && rt_env[0]) {
        snprintf(rt_path, sizeof rt_path, "%s", rt_env);
    } else if (target_isa == ISA_X86_64 || target_isa == ISA_AARCH64 ||
               target_isa == ISA_RISCV64) {
        char self[512];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n > 0) {
            self[n] = 0;
            char *slash = strrchr(self, '/');
            if (slash) {
                *slash = '\0';
                snprintf(rt_path, sizeof rt_path, "%s/../lib/%s",
                         self, rt_filename);
            }
        }
    }

    /* T2: ZeroCC's own linker (x86-64 + AArch64; zero_rt Phase 0 is its
     * first customer). */
    if (opts->zerocc_link) {
        if (target_isa != ISA_X86_64 && target_isa != ISA_AARCH64 &&
            target_isa != ISA_RISCV64) {
            fprintf(stderr,
                    "Error: --zerocc-link currently links x86-64, aarch64, "
                    "and riscv64 objects only\n");
            for (int i = 0; i < obj_count; i++) unlink(obj_paths[i]);
            return 1;
        }
        if (use_text) {
            fprintf(stderr, "Error: --zerocc-link cannot combine with "
                    "ZC_TEXT_AOT\n");
            for (int i = 0; i < obj_count; i++) unlink(obj_paths[i]);
            return 1;
        }
        if (opts->verbose) {
            printf("Linking with ZeroCC's own linker...\n");
        }
        char link_err[1024];
        const char *objs_ref[ZERO_CC_MAX_INPUTS];
        for (int i = 0; i < obj_count; i++) objs_ref[i] = obj_paths[i];

        if (!opts->no_zerocc_rt && rt_path[0] && access(rt_path, R_OK) == 0
            && arch_count < 32) {
            arch_paths[arch_count++] = rt_path;
            if (opts->verbose) printf("Linking zero_rt: %s\n", rt_path);
        }

        int lr = zero_cc_link_exec(output, objs_ref, obj_count,
                                   arch_paths, arch_count, link_err,
                                   sizeof link_err);
        for (int i = 0; i < obj_count; i++) {
            if (obj_temp[i] && !getenv("ZC_KEEP_OBJ")) unlink(obj_paths[i]);
        }
        if (lr != 0) {
            fprintf(stderr, "Error: linking failed: %s\n", link_err);
            return 1;
        }
        return 0;
    }

    if (opts->verbose) {
        printf("Linking with system linker...\n");
    }

    /* Invoke system linker (gcc) to create executable. cmd is sized to
     * hold all_objs (up to ZERO_CC_MAX_INPUTS temp paths) plus flags/libs. */
    char cmd[16384];

    /* Select appropriate linker based on target ISA */
    const char *linker = "gcc";
    const char *abi_flags = "-no-pie";
    const char *static_flag = "";   /* cross targets need a static binary to run under QEMU */
    if (target_isa == ISA_AARCH64) {
        linker = "aarch64-linux-gnu-gcc";
        abi_flags = "-mabi=lp64 -march=armv8-a";
        static_flag = "-static";
    } else if (target_isa == ISA_RISCV64) {
        linker = "riscv64-linux-gnu-gcc";
        abi_flags = "-mabi=lp64d -march=rv64gc";
        static_flag = "-static";
    } else if (target_isa == ISA_RISCV32) {
        linker = "riscv64-linux-gnu-gcc";
        abi_flags = "-mabi=ilp32d -march=rv32gc";
        static_flag = "-static";
    }

    char lib_args[1024] = "";
    for (int i = 0; i < opts->lib_count; i++) {
        strncat(lib_args, " ", sizeof(lib_args) - strlen(lib_args) - 1);
        strncat(lib_args, opts->libs[i], sizeof(lib_args) - strlen(lib_args) - 1);
    }

    if (target_isa == ISA_RISCV32) {
        snprintf(cmd, sizeof(cmd),
                 "riscv64-linux-gnu-gcc -march=rv32gc -mabi=ilp32 -static -no-pie -Wl,-m,elf32lriscv -nostdlib "
                 "/home/ubuntu/dev/musl-cross/riscv32/lib/crt1.o "
                 "/home/ubuntu/dev/musl-cross/riscv32/lib/crti.o "
                 "%s "
                 "/home/ubuntu/dev/musl-cross/riscv32/lib/libc.a "
                 "/home/ubuntu/dev/musl-cross/riscv32/lib/libgcc.a "
                 "/home/ubuntu/dev/musl-cross/riscv32/lib/crtn.o "
                 "-o %s%s 2>&1",
                 all_objs, output, lib_args);
    } else {
        char rt_arg[2100] = "";
        if (!opts->no_zerocc_rt && rt_path[0] && access(rt_path, R_OK) == 0) {
            snprintf(rt_arg, sizeof(rt_arg), " %s", rt_path);
        }
        snprintf(cmd, sizeof(cmd),
                 "%s %s %s -Wl,--defsym=link_error=0 -o %s %s -lc -lm%s%s 2>&1",
                 linker, abi_flags, static_flag, output, all_objs, rt_arg, lib_args);
    }

    if (opts->verbose) {
        printf("  %s\n", cmd);
    }

    int ret = system(cmd);

    /* Clean up temporary object files */
    for (int i = 0; i < obj_count; i++) {
        if (!getenv("ZC_KEEP_OBJ")) unlink(obj_paths[i]);
    }

    if (ret != 0) {
        fprintf(stderr, "Error: Linking failed\n");
        return 1;
    }

    if (opts->verbose) {
        printf("Executable created: %s\n", output);
    }

    return 0;
}

/* Text AOT escape hatch for -c (relocatable object): when ZC_TEXT_AOT=1 is set,
 * emit TEXT assembly and let the system cross-assembler produce the .o. */
static int compile_object_cross(CompileOptions *opts) {
    char tmp_s[512];
    snprintf(tmp_s, sizeof(tmp_s), "/tmp/zerocc_%d.s", getpid());

    CompileOptions s_opts = *opts;
    s_opts.emit_asm = 1;
    s_opts.emit_object = 0;
    s_opts.output_file = tmp_s;
    s_opts.verbose = 0;

    int ret = compile_object(&s_opts);
    if (ret != 0) {
        unlink(tmp_s);
        return ret;
    }

    ZeroCCISA tisa = parse_target_arch(opts->target_arch);
    const char *as = (tisa == ISA_AARCH64) ? "aarch64-linux-gnu-as" : "riscv64-linux-gnu-as";
    const char *output = opts->output_file ? opts->output_file : "a.out";
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s -o %s 2>&1", as, tmp_s, output);

    ret = system(cmd);
    unlink(tmp_s);
    if (ret != 0) {
        fprintf(stderr, "Error: cross-target object assembly failed\n");
        return 1;
    }
    if (opts->verbose) {
        printf("Object file written: %s\n", output);
    }
    return 0;
}

int main(int argc, char **argv) {
    /* Stale-build guard: catch object files compiled against an older
     * ZeroCCASTNode layout before they corrupt the AST at runtime. */
    if (zero_cc_ast_node_size() != sizeof(ZeroCCASTNode)) {
        fprintf(stderr,
                "zerocc: AST node layout mismatch (stale build objects).\n"
                "Run: make clean && make build/bin/zerocc\n");
        return 1;
    }
#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0) {
        rlim_t target = 64 * 1024 * 1024;
        if (rl.rlim_cur < target) {
            rl.rlim_cur = (rl.rlim_max != RLIM_INFINITY && target > rl.rlim_max) ? rl.rlim_max : target;
            setrlimit(RLIMIT_STACK, &rl);
        }
    }
#endif
    CompileOptions opts;

    int ret = parse_args(argc, argv, &opts);
    if (ret <= 0) {
        return ret == 0 ? 0 : 1;
    }

    /* Determine mode: -E -> preprocess only, -c -> object file,
     * -S -> assembly, -o without -c -> executable, else JIT */
    if (opts.emit_preproc) {
        return compile_preprocess(&opts);
    } else if (opts.emit_object || opts.emit_asm) {
        /* When ZC_TEXT_AOT=1 is set for cross targets, route -c through the text
         * assembler escape hatch. Default is direct raw binary object generation. */
        if (opts.emit_object && getenv("ZC_TEXT_AOT") && parse_target_arch(opts.target_arch) != ISA_X86_64) {
            return compile_object_cross(&opts);
        }
        return compile_object(&opts);
    } else if (opts.output_file) {
        /* Generate executable via system linker */
        return compile_and_link(&opts);
    } else {
        return compile_jit(&opts);
    }
}
