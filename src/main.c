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
#include "zero_cc/opt/opt_pgo.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/jit/elf_writer.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/parser/decl_struct.h"  /* zero_cc_parse_struct_body (Sprint 114: was transitive via compile.h) */

#include "zero_cc/types/global_vars.h"
#include "zero_cc/core/multi_tu.h"
#include "zero_cc/core/lto.h"
#include "zero_cc/linker/elf_reader.h"
#include "zero_cc/linker/ar_reader.h"
#include "zero_cc/linker/linker.h"
#include "zero_cc/linker/ar_writer.h"
#include "zero_cc/linker/pe_writer.h"
#include "zero_cc/linker/macho_writer.h"
#include "zero_cc/linker/foreign_loader.h"

void __zc_pgo_reset(void);
void __zc_pgo_set_file(const char *path);
void __zc_pgo_dump(void);

#include "zero_cc/driver/driver_opts.h"
#include "zero_cc/driver/driver_asm.h"
#include "zero_cc/dwarf/dwarf.h"

static ZeroCCISA parse_target_arch(const char *target);
static void zero_cc_init_compiler_with_target(ZeroCCCompiler *comp, ZeroCCISA target_isa);

static int compile_object_single(CompileOptions *opts, const char *input, const char *output);
static int write_compiled_object(ZeroCCCompiler *comp, CompileOptions *opts,
                                 const char *output_path, const char *lto_prep_source);

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
    } else if (target_isa == ISA_AARCH32) {
        defines.defs[defines.count].name = strdup("__x86_64__");
        defines.defs[defines.count].value = NULL;
        defines.undef[defines.count++] = 1;

        defines.defs[defines.count].name = strdup("__LP64__");
        defines.defs[defines.count].value = NULL;
        defines.undef[defines.count++] = 1;

        defines.defs[defines.count].name = strdup("__arm__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;

        defines.defs[defines.count].name = strdup("__ARM_ARCH_7A__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;

        defines.defs[defines.count].name = strdup("__ARM_EABI__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;

        defines.defs[defines.count].name = strdup("__ILP32__");
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

    if (opts->baremetal) {
        defines.defs[defines.count].name = strdup("__linux__");
        defines.defs[defines.count].value = NULL;
        defines.undef[defines.count++] = 1;

        defines.defs[defines.count].name = strdup("__unix__");
        defines.defs[defines.count].value = NULL;
        defines.undef[defines.count++] = 1;

        defines.defs[defines.count].name = strdup("__STDC_HOSTED__");
        defines.defs[defines.count].value = strdup("0");
        defines.undef[defines.count++] = 0;

        defines.defs[defines.count].name = strdup("__BAREMETAL__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    } else {
        defines.defs[defines.count].name = strdup("__STDC_HOSTED__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    }

    if (opts->has_avx) {
        defines.defs[defines.count].name = strdup("__AVX__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    }
    if (opts->has_avx2) {
        defines.defs[defines.count].name = strdup("__AVX2__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    }
    if (opts->has_avx512f) {
        defines.defs[defines.count].name = strdup("__AVX512F__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    }
    if (opts->has_avx512bw) {
        defines.defs[defines.count].name = strdup("__AVX512BW__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    }
    if (opts->has_avx512dq) {
        defines.defs[defines.count].name = strdup("__AVX512DQ__");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    }
    if (opts->has_sve) {
        defines.defs[defines.count].name = strdup("__ARM_FEATURE_SVE");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    }
    if (opts->has_sve2) {
        defines.defs[defines.count].name = strdup("__ARM_FEATURE_SVE2");
        defines.defs[defines.count].value = strdup("1");
        defines.undef[defines.count++] = 0;
    }
    if (opts->has_rvv) {
        defines.defs[defines.count].name = strdup("__riscv_v_intrinsic");
        defines.defs[defines.count].value = strdup("1000000");
        defines.undef[defines.count++] = 0;
        defines.defs[defines.count].name = strdup("__riscv_vector");
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
    /* __STDC_VERSION__ follows the -std= dialect */
    const char *std_version =
        opts->std_dialect == ZC_STD_C99 || opts->std_dialect == ZC_STD_GNU99 ? "199901L" :
        opts->std_dialect == ZC_STD_C23 || opts->std_dialect == ZC_STD_GNU23 ? "202311L" :
        /* ZC_STD_C11/ZC_STD_GNU11/ZC_STD_C17/ZC_STD_GNU17 */
        "201112L";
    char *out = zero_cc_preprocess_ext3(source, &inc_dirs, &defines,
                                        main_file, include_err, sizeof(include_err),
                                        final_dep_target, final_dep_file, opts->gen_deps,
                                        opts->force_include_count > 0 ? (const char **)opts->force_includes : NULL,
                                        opts->force_include_count,
                                        std_version,
                                        opts->emit_preproc);
    if (!out && include_err[0]) {
        fprintf(stderr, "%s\n", include_err);
    }

    /* The defines are fully consumed by the preprocessor call above; free
     * the strdup'd name/value strings (make asan: LeakSanitizer). */
    for (size_t d = 0; d < defines.count; d++) {
        free(defines.defs[d].name);
        free(defines.defs[d].value);
    }
    return out;
}

/* Compile one translation unit for JIT execution. Returns the compiler on
 * success (caller owns it), NULL on failure (diagnostics already printed). */
static ZeroCCCompiler *jit_compile_tu(CompileOptions *opts, const char *input, int tu_index) {
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
    comp->tu_index = tu_index;

    ZeroCCISA target_isa = parse_target_arch(opts->target_arch);
    zero_cc_init_compiler_with_target(comp, target_isa);
    comp->is_win64_abi = opts->is_win64_abi || (opts->is_pe && (target_isa == ISA_X86_64));
    comp->opt_level = (OptLevel)opts->opt_level;
    comp->omit_frame_pointer = opts->omit_frame_pointer;
    comp->no_builtin_all = opts->no_builtin_all != 0;
    comp->no_builtin_count = opts->no_builtin_count;
    for (int nb = 0; nb < opts->no_builtin_count; nb++) {
        snprintf(comp->no_builtin_names[nb], sizeof(comp->no_builtin_names[nb]),
                 "%s", opts->no_builtin[nb]);
    }
    comp->sanitize_overflow = opts->sanitize_overflow != 0;
    comp->sanitize_report = opts->sanitize_report != 0;
    comp->sanitize_address = opts->sanitize_address != 0;
    comp->stack_protector = opts->stack_protector;
    comp->std_dialect = opts->std_dialect;
    comp->debug_info = opts->debug_info;
    comp->dwarf_version = opts->dwarf_version ? opts->dwarf_version : 4;
    comp->function_sections = opts->function_sections;
    comp->data_sections = opts->data_sections;
    comp->has_avx = opts->has_avx != 0;
    comp->has_avx2 = opts->has_avx2 != 0;
    comp->has_avx512f = opts->has_avx512f != 0;
    comp->has_avx512bw = opts->has_avx512bw != 0;
    comp->has_avx512dq = opts->has_avx512dq != 0;
    comp->has_sve = opts->has_sve != 0;
    comp->has_sve2 = opts->has_sve2 != 0;
    comp->has_rvv = opts->has_rvv != 0;
    comp->profile_generate = opts->profile_generate;
    comp->profile_use = opts->profile_use;
    comp->profile_file = opts->profile_generate ? opts->profile_generate_path : opts->profile_use_path;
    if (opts->profile_use && opts->profile_use_path && !comp->pgo_profile) {
        comp->pgo_profile = zc_pgo_load_profile(opts->profile_use_path);
    }
    comp->source_filename = input;
    static char cwd_buf[1024];
    if (getcwd(cwd_buf, sizeof(cwd_buf))) {
        comp->comp_dir = cwd_buf;
    }

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

static ZeroCCCompiler *lto_compile_tu_phase1(CompileOptions *opts, const char *source_name,
                                               const char *prep_source, int tu_index) {
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) {
        perror("calloc");
        return NULL;
    }

    ZeroCCISA target_isa = parse_target_arch(opts->target_arch);
    zero_cc_init_compiler_with_target(comp, target_isa);
    comp->is_win64_abi = opts->is_win64_abi || (opts->is_pe && (target_isa == ISA_X86_64));

    comp->opt_level = (OptLevel)opts->opt_level;
    comp->omit_frame_pointer = opts->omit_frame_pointer;
    comp->no_builtin_all = opts->no_builtin_all != 0;
    comp->no_builtin_count = opts->no_builtin_count;
    for (int nb = 0; nb < opts->no_builtin_count; nb++) {
        snprintf(comp->no_builtin_names[nb], sizeof(comp->no_builtin_names[nb]),
                 "%s", opts->no_builtin[nb]);
    }
    comp->sanitize_overflow = opts->sanitize_overflow != 0;
    comp->sanitize_report = opts->sanitize_report != 0;
    comp->sanitize_address = opts->sanitize_address != 0;
    comp->stack_protector = opts->stack_protector;
    comp->std_dialect = opts->std_dialect;
    comp->debug_info = opts->debug_info;
    comp->dwarf_version = opts->dwarf_version ? opts->dwarf_version : 4;
    comp->function_sections = opts->function_sections;
    comp->data_sections = opts->data_sections;
    comp->has_avx = opts->has_avx != 0;
    comp->has_avx2 = opts->has_avx2 != 0;
    comp->has_avx512f = opts->has_avx512f != 0;
    comp->has_avx512bw = opts->has_avx512bw != 0;
    comp->has_avx512dq = opts->has_avx512dq != 0;
    comp->has_sve = opts->has_sve != 0;
    comp->has_sve2 = opts->has_sve2 != 0;
    comp->has_rvv = opts->has_rvv != 0;
    comp->profile_generate = opts->profile_generate;
    comp->profile_use = opts->profile_use;
    comp->profile_file = opts->profile_generate ? opts->profile_generate_path : opts->profile_use_path;
    if (opts->profile_use && opts->profile_use_path && !comp->pgo_profile) {
        comp->pgo_profile = zc_pgo_load_profile(opts->profile_use_path);
    }
    comp->source_filename = zero_cc_intern_string(comp, source_name ? source_name : "<lto>");
    static char cwd_buf[1024];
    if (getcwd(cwd_buf, sizeof(cwd_buf))) {
        comp->comp_dir = cwd_buf;
    }

    bool is_jit = (opts->run_mode || (!opts->output_file && !opts->emit_object &&
                                      !opts->emit_asm && !opts->emit_preproc));
    comp->jit_mode = is_jit ? 1 : 0;
    comp->lto_mode = true;
    comp->lto_phase = 1;
    comp->tu_index = tu_index;

    zero_cc_tokenize_c99(prep_source, comp);
    if (opts->dump_ir) comp->dump_ir = 1;
    if (opts->dump_cfg) comp->dump_cfg = 1;
    if (opts->profile_opts) {
        opt_profile_enable(1);
        opt_profile_reset();
    }

    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
        zero_cc_compiler_free(comp);
        return NULL;
    }
    return comp;
}

static int compile_jit(CompileOptions *opts) {
    if (opts->lto) {
        ZeroCCLTOContext lto;
        zero_cc_lto_init(&lto);
        for (int i = 0; i < opts->input_count; i++) {
            char *source = read_file(opts->input_files[i]);
            if (!source) {
                zero_cc_lto_cleanup(&lto);
                return 1;
            }
            char *prep = preprocess_with_options(source, opts, opts->input_files[i], opts->dep_target);
            free(source);
            if (!prep) {
                zero_cc_lto_cleanup(&lto);
                return 1;
            }
            ZeroCCCompiler *tu = lto_compile_tu_phase1(opts, opts->input_files[i], prep, lto.tu_count);
            free(prep);
            if (!tu || zero_cc_lto_add_tu(&lto, tu) != 0) {
                if (tu) zero_cc_compiler_free(tu);
                if (lto.had_error) fprintf(stderr, "zerocc: error: %s\n", lto.error_msg);
                zero_cc_lto_cleanup(&lto);
                return 1;
            }
        }
        if (zero_cc_lto_analyze(&lto) != 0 || zero_cc_lto_optimize_and_lower(&lto) != 0) {
            fprintf(stderr, "zerocc: error: %s\n", lto.error_msg[0] ? lto.error_msg : "LTO failed");
            zero_cc_lto_cleanup(&lto);
            return 1;
        }
        ZeroCCCompiler *base = lto.tus[0];
        for (int i = 1; i < lto.tu_count; i++) {
            char err[256];
            if (zero_cc_tu_merge(base, lto.tus[i], err, sizeof(err)) != 0) {
                fprintf(stderr, "zerocc: error: %s\n", err);
                zero_cc_lto_cleanup(&lto);
                return 1;
            }
        }
        ZeroCCFunction *main_fn = zero_cc_find_function(base, "main");
        if (!main_fn || main_fn->is_prototype) {
            fprintf(stderr, "zerocc: error: undefined reference to 'main'\n");
            zero_cc_lto_cleanup(&lto);
            return 1;
        }
        if (opts->profile_opts) {
            opt_profile_dump(stderr);
        }
        base->jit_argc = opts->jit_argc;
        base->jit_argv = opts->jit_argv;
        if (opts->profile_generate) {
            __zc_pgo_reset();
            if (opts->profile_generate_path) __zc_pgo_set_file(opts->profile_generate_path);
        }
        int result = zero_cc_execute_jit(base, NULL);
        if (opts->profile_generate) {
            __zc_pgo_dump();
        }
        if (opts->verbose) {
            printf("JIT execution result: %d\n", result);
        }
        zero_cc_lto_cleanup(&lto);
        return result;
    }

    ZeroCCCompiler *comp = jit_compile_tu(opts, opts->input_files[0], 0);
    if (!comp) {
        return 1;
    }
    comp->jit_argc = opts->jit_argc;
    comp->jit_argv = opts->jit_argv;

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
            ZeroCCCompiler *tu = jit_compile_tu(opts, opts->input_files[i], i);
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

        if (opts->profile_generate) {
            __zc_pgo_reset();
            if (opts->profile_generate_path) __zc_pgo_set_file(opts->profile_generate_path);
        }
        int result = zero_cc_execute_jit(comp, NULL);
        if (opts->profile_generate) {
            __zc_pgo_dump();
        }
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

    if (opts->profile_generate) {
        __zc_pgo_reset();
        if (opts->profile_generate_path) __zc_pgo_set_file(opts->profile_generate_path);
    }
    int result = zero_cc_execute_jit(comp, NULL);
    if (opts->profile_generate) {
        __zc_pgo_dump();
    }
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
    if (strcmp(target, "aarch32") == 0 || strcmp(target, "arm32") == 0 || strcmp(target, "armv7") == 0) return ISA_AARCH32;
    if (strcmp(target, "riscv64") == 0 || strcmp(target, "riscv") == 0) return ISA_RISCV64;
    if (strcmp(target, "riscv32") == 0) return ISA_RISCV32;
    if (strstr(target, "aarch64") || strstr(target, "arm64")) return ISA_AARCH64;
    if (strstr(target, "arm") || strstr(target, "aarch32") || strstr(target, "cortex")) return ISA_AARCH32;
    if (strstr(target, "riscv32") || strstr(target, "rv32")) return ISA_RISCV32;
    if (strstr(target, "riscv64") || strstr(target, "rv64") || strstr(target, "riscv")) return ISA_RISCV64;
    if (strstr(target, "x86_64") || strstr(target, "x86-64")) return ISA_X86_64;
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
    int ptr_sz = zc_target_ptr_size(comp);
    type_set_target_metrics(ptr_sz, ptr_sz);
    comp->omit_frame_pointer = -1; /* default: auto (omit at -O1+); CLI overrides */
    comp->local_lookup_bound = -1; /* default: unbounded */
    comp->parse_struct_body = zero_cc_parse_struct_body;

    /* Initialize IR arenas */
    if (!ir_arena_init(&comp->ir_arena, 4096)) {
        fprintf(stderr, "Error: Failed to initialize IR arena\n");
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

static inline ZeroCCFunction *find_fn_for_text_offset(ZeroCCCompiler *comp,
                                                      const size_t *order,
                                                      const size_t *extent_end,
                                                      size_t nreal,
                                                      size_t offset) {
    for (size_t i = 0; i < nreal; i++) {
        size_t fi = order[i];
        ZeroCCFunction *f = &comp->functions[fi];
        if (offset >= f->text_offset && offset < extent_end[fi]) {
            return f;
        }
    }
    return NULL;
}

static inline void resolve_text_reloc_loc(ZeroCCCompiler *comp,
                                          ElfWriterCtx *elf_ctx,
                                          const size_t *order,
                                          const size_t *extent_end,
                                          size_t nreal,
                                          uint16_t text_idx,
                                          size_t patch_offset,
                                          uint16_t *out_sec_idx,
                                          uint64_t *out_sec_offset) {
    ZeroCCFunction *f = find_fn_for_text_offset(comp, order, extent_end, nreal, patch_offset);
    if (f && f->section[0]) {
        uint16_t sidx = elf_writer_section_index(elf_ctx, f->section);
        if (sidx != 0) {
            *out_sec_idx = sidx;
            *out_sec_offset = patch_offset - f->text_offset + f->named_text_sec_off;
            return;
        }
    }
    *out_sec_idx = text_idx;
    *out_sec_offset = patch_offset;
}

static inline void resolve_data_reloc_loc(ZeroCCCompiler *comp,
                                          ElfWriterCtx *elf_ctx,
                                          uint16_t data_section_idx,
                                          size_t patch_offset,
                                          uint16_t *out_sec_idx,
                                          uint64_t *out_sec_offset) {
    for (size_t i = 0; i < comp->global_var_count; i++) {
        ZeroCCGlobalVar *gv = &comp->global_vars[i];
        if (!gv->name[0] || gv->alias_target[0] || gv->is_tls || gv->lto_eliminated) continue;
        size_t sz = gv->size ? gv->size : gv->type.size;
        if (patch_offset >= gv->data_offset && patch_offset < gv->data_offset + sz) {
            if (gv->section[0]) {
                uint16_t sidx = elf_writer_section_index(elf_ctx, gv->section);
                if (sidx != 0) {
                    *out_sec_idx = sidx;
                    *out_sec_offset = patch_offset - gv->data_offset + gv->named_sec_off;
                    return;
                }
            }
            break;
        }
    }
    *out_sec_idx = data_section_idx;
    *out_sec_offset = patch_offset;
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
    snprintf(buf + len, cap - len, "%s", ext);
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
    const char *output_path = derive_default_output(input, output, opts->is_pe ? ".obj" : ".o",
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
    comp->is_win64_abi = opts->is_win64_abi || (opts->is_pe && (target_isa == ISA_X86_64));

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
    comp->sanitize_report = opts->sanitize_report != 0;
    comp->sanitize_address = opts->sanitize_address != 0;
    comp->stack_protector = opts->stack_protector;
    comp->std_dialect = opts->std_dialect;
    comp->debug_info = opts->debug_info;
    comp->dwarf_version = opts->dwarf_version ? opts->dwarf_version : 4;
    comp->function_sections = opts->function_sections;
    comp->data_sections = opts->data_sections;
    comp->has_avx = opts->has_avx != 0;
    comp->has_avx2 = opts->has_avx2 != 0;
    comp->has_avx512f = opts->has_avx512f != 0;
    comp->has_avx512bw = opts->has_avx512bw != 0;
    comp->has_avx512dq = opts->has_avx512dq != 0;
    comp->has_sve = opts->has_sve != 0;
    comp->has_sve2 = opts->has_sve2 != 0;
    comp->has_rvv = opts->has_rvv != 0;
    comp->profile_generate = opts->profile_generate;
    comp->profile_use = opts->profile_use;
    comp->profile_file = opts->profile_generate ? opts->profile_generate_path : opts->profile_use_path;
    if (opts->profile_use && opts->profile_use_path && !comp->pgo_profile) {
        comp->pgo_profile = zc_pgo_load_profile(opts->profile_use_path);
    }
    comp->source_filename = input;
    static char cwd_buf[1024];
    if (getcwd(cwd_buf, sizeof(cwd_buf))) {
        comp->comp_dir = cwd_buf;
    }

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

    char *lto_prep_copy = (opts->lto && !opts->emit_asm) ? strdup(prep) : NULL;

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
            free(lto_prep_copy);
            zero_cc_compiler_free(comp);
            return 1;
        }
        if (comp->isa == ISA_AARCH32) {
            fprintf(comp->asm_out, "\t.arch\tarmv7ve\n\t.fpu\tvfpv3-d16\n");
        }
        if (opts->verbose) {
            printf("Writing assembly to: %s\n", out_path);
        }
    }

    /* Compile */
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
        free(lto_prep_copy);
        zero_cc_compiler_free(comp);
        return 1;
    }

    /* Dump optimization profile if requested */
    if (opts->profile_opts) {
        opt_profile_dump(stderr);
    }

    int write_rc = write_compiled_object(comp, opts, output_path, lto_prep_copy);
    free(lto_prep_copy);
    zero_cc_compiler_free(comp);
    return write_rc;
}

static int write_compiled_object(ZeroCCCompiler *comp, CompileOptions *opts,
                                 const char *output_path, const char *lto_prep_source) {
    if (opts->verbose) {
        printf("Writing object file: %s\n", output_path);
    }

    if (opts->is_pe) {
        if (!zero_cc_pe_write_coff_obj_from_comp(output_path, comp)) {
            fprintf(stderr, "zerocc: error writing COFF object file: %s\n", output_path);
            return 1;
        }
        return 0;
    }

    if (opts->is_macho) {
        if (!zero_cc_macho_write_obj_from_comp(output_path, comp)) {
            fprintf(stderr, "zerocc: error writing Mach-O object file: %s\n", output_path);
            return 1;
        }
        return 0;
    }

    /* Initialize ELF writer from target traits */
    const TargetTraits *traits = zc_target_traits(comp);
    ElfWriterCtx elf_ctx;
    elf_writer_init(&elf_ctx, (ElfMachineType)traits->elf_machine, traits->is_32bit);

    /* If -ffunction-sections is enabled, assign each function to its own section .text.<name> */
    if (comp->function_sections) {
        for (size_t i = 0; i < comp->function_count; i++) {
            ZeroCCFunction *f = &comp->functions[i];
            if (f->name[0] && !f->is_prototype && !f->is_external && !f->is_object && !f->lto_eliminated) {
                if (!f->section[0]) {
                    const char *fn_sym = zero_cc_fn_emit_symbol(f);
                    snprintf(f->section, sizeof(f->section), ".text.%s", fn_sym);
                }
            }
        }
    }

    /* If -fdata-sections is enabled, assign each global variable to .bss.<name>, .rodata.<name>, or .data.<name> */
    if (comp->data_sections) {
        for (size_t i = 0; i < comp->global_var_count; i++) {
            ZeroCCGlobalVar *gv = &comp->global_vars[i];
            if (gv->name[0] && !gv->alias_target[0] && !gv->is_tls && !gv->lto_eliminated) {
                if (!gv->section[0]) {
                    if (gv->is_bss) {
                        snprintf(gv->section, sizeof(gv->section), ".bss.%s", gv->name);
                    } else if (gv->type.is_const) {
                        snprintf(gv->section, sizeof(gv->section), ".rodata.%s", gv->name);
                    } else {
                        snprintf(gv->section, sizeof(gv->section), ".data.%s", gv->name);
                    }
                }
            }
        }
    }

    /* Add .text section (if any functions remain in default .text) */
    bool has_default_text = false;
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *f = &comp->functions[i];
        if (f->name[0] && !f->is_prototype && !f->is_external && !f->is_object && !f->lto_eliminated) {
            if (!f->section[0]) { has_default_text = true; break; }
        }
    }
    if (has_default_text && comp->text_section.length > 0) {
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
    bool has_default_data = false;
    for (size_t gi = 0; gi < comp->global_var_count; gi++) {
        ZeroCCGlobalVar *gv = &comp->global_vars[gi];
        if (gv->is_bss || gv->is_tls || gv->lto_eliminated) continue;
        if (!gv->section[0]) has_default_data = true;
        size_t ga = gv->type.align > 0 ? gv->type.align : 8;
        if (ga > data_align) data_align = ga;
    }
    if (has_default_data && comp->data_section.length > 0) {
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
    bool has_default_bss = false;
    for (size_t i = 0; i < comp->global_var_count; i++) {
        if (comp->global_vars[i].is_bss && !comp->global_vars[i].is_tls &&
            !comp->global_vars[i].lto_eliminated) {
            if (!comp->global_vars[i].section[0]) has_default_bss = true;
            size_t end = comp->global_vars[i].data_offset + comp->global_vars[i].size + (comp->sanitize_address ? 32 : 0);
            if (end > bss_total) bss_total = end;
            size_t ga = comp->global_vars[i].type.align > 0 ? comp->global_vars[i].type.align : 8;
            if (ga > bss_align) bss_align = ga;
        }
    }
    if (has_default_bss && bss_total > 0) {
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

    /* ---- Named sections: GNU __attribute__((section("name"))) & -fdata-sections ----
     * Objects placed in a named ELF section.  Their bytes are copied out of
     * data_section into a dedicated PROGBITS section (NOBITS for BSS-like),
     * and each object's symbol is emitted in that named section (below).
     * Relocations target the symbol, so every reference resolves to the named
     * section. */
    {
        size_t gvn = comp->global_var_count;
        size_t max_ns = gvn + 16;
        char (*ns_names)[64] = calloc(max_ns, 64);
        size_t *ns_size = calloc(max_ns, sizeof(size_t));
        size_t *ns_align = calloc(max_ns, sizeof(size_t));
        bool *ns_bss = calloc(max_ns, sizeof(bool));
        bool *ns_rodata = calloc(max_ns, sizeof(bool));
        int ns_count = 0;
        if (ns_names && ns_size && ns_align && ns_bss && ns_rodata) {
            for (size_t k = 0; k < max_ns; k++) { ns_size[k] = 0; ns_align[k] = 8; ns_bss[k] = false; ns_rodata[k] = false; }

            for (size_t i = 0; i < gvn; i++) {
                ZeroCCGlobalVar *gv = &comp->global_vars[i];
                if (!gv->name[0] || !gv->section[0] || gv->alias_target[0] || gv->is_tls || gv->lto_eliminated) continue;
                int si = -1;
                for (int k = 0; k < ns_count; k++)
                    if (strcmp(ns_names[k], gv->section) == 0) { si = k; break; }
                if (si < 0) {
                    if (ns_count >= (int)max_ns) { gv->section[0] = '\0'; continue; } /* degrade to .data */
                    si = ns_count++;
                    strncpy(ns_names[si], gv->section, 63);
                    ns_names[si][63] = '\0';
                    ns_size[si] = 0; ns_align[si] = 8; ns_bss[si] = false; ns_rodata[si] = false;
                }
                size_t al = gv->type.align > 0 ? gv->type.align : 8;
                if (al > ns_align[si]) ns_align[si] = al;
                size_t cur = ns_size[si] + ((al - (ns_size[si] % al)) % al);
                gv->named_sec_off = cur;
                cur += gv->size;
                if (cur > ns_size[si]) ns_size[si] = cur;
                if (gv->is_bss) ns_bss[si] = true;
                if (strncmp(ns_names[si], ".rodata", 7) == 0) ns_rodata[si] = true;
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
                        if (gv->lto_eliminated || strcmp(gv->section, ns_names[k]) != 0) continue;
                        if (!gv->is_bss && gv->size > 0 &&
                            gv->data_offset + gv->size <= comp->data_section.length) {
                            memcpy(buf + gv->named_sec_off,
                                   comp->data_section.data + gv->data_offset, gv->size);
                        }
                    }
                    uint64_t sflags = ns_rodata[k] ? ELF_SHF_ALLOC : (ELF_SHF_ALLOC | ELF_SHF_WRITE);
                    elf_writer_add_section(&elf_ctx, ns_names[k], buf, total,
                                           ELF_SHT_PROGBITS, sflags,
                                           ns_align[k]);
                    free(buf); /* copied into the writer */
                }
            }
        }
        free(ns_names);
        free(ns_size);
        free(ns_align);
        free(ns_bss);
        free(ns_rodata);
    }

    /* ---- Named text sections: GNU __attribute__((section("name"))) & -ffunction-sections ----
     * A function's code is emitted into text_section as usual (JIT-safe).  At -c
     * emission, each function with a section is copied out of text_section into a
     * dedicated PROGBITS|EXECINSTR section and its symbol is emitted there (below).
     * The function's extent in text_section is [text_offset, next-function-text_offset)
     * (or to the end of the section for the last one); a contiguous copy preserves
     * the function's internal relative offsets. */
    size_t fcn = comp->function_count;
    size_t max_nts = fcn + 16;
    char (*nts_names)[64] = calloc(max_nts, 64);
    size_t *nts_size = calloc(max_nts, sizeof(size_t));
    int nts_count = 0;

    size_t *extent_end = (size_t *)calloc(fcn ? fcn : 1, sizeof(size_t));
    size_t *order = (size_t *)calloc(fcn ? fcn : 1, sizeof(size_t));
    size_t nreal = 0;
    for (size_t i = 0; i < fcn; i++) {
        ZeroCCFunction *f = &comp->functions[i];
        if (f->name[0] && !f->is_prototype && !f->is_external && !f->is_object && !f->lto_eliminated) {
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

    if (nts_names && nts_size) {
        /* lay out sectioned functions in named sections (in text_offset order) */
        for (size_t i = 0; i < nreal; i++) {
            size_t fi = order[i];
            ZeroCCFunction *f = &comp->functions[fi];
            if (!f->section[0]) continue;
            int si = -1;
            for (int k = 0; k < nts_count; k++)
                if (strcmp(nts_names[k], f->section) == 0) { si = k; break; }
            if (si < 0) {
                if (nts_count >= (int)max_nts) { f->section[0] = '\0'; continue; } /* degrade to .text */
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
    }
    free(nts_names);
    free(nts_size);

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
    int rodata_symbol_idx = 0;
    if (comp->rodata_section.length > 0 && rodata_idx != 0) {
        elf_writer_add_symbol(&elf_ctx, ".rodata",
                              ELF_STB_LOCAL, ELF_STT_SECTION,
                              rodata_idx,  /* .rodata section index */
                              0, 0);
        rodata_symbol_idx = elf_ctx.symbol_count;
    }
    if (comp->data_section.length > 0 && data_idx != 0) {
        elf_writer_add_symbol(&elf_ctx, ".data",
                              ELF_STB_LOCAL, ELF_STT_SECTION,
                              data_idx,  /* .data section index */
                              0, 0);
    }
    if (comp->text_section.length > 0 && text_idx != 0) {
        elf_writer_add_symbol(&elf_ctx, ".text",
                              ELF_STB_LOCAL, ELF_STT_SECTION,
                              text_idx,  /* .text section index */
                              0, 0);
    }

    /* Add symbols for functions */
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *fn = &comp->functions[i];
        if (fn->lto_eliminated) continue;
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
                        !comp->functions[k].lto_eliminated &&
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
            int binding = fn->is_static ? ELF_STB_LOCAL : (fn->is_weak ? ELF_STB_WEAK : ELF_STB_GLOBAL);
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
        if (gv->lto_eliminated) continue;
        if (gv->name[0]) {
            /* GNU alias entry: no storage, no ELF symbol (uses resolve to
             * the target's symbol at lowering time). */
            if (gv->alias_target[0]) continue;
            int binding = gv->is_static ? ELF_STB_LOCAL : (gv->is_weak ? ELF_STB_WEAK : ELF_STB_GLOBAL);
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
        case ISA_AARCH32:
            reloc_type_call = 28;   /* R_ARM_CALL */
            reloc_type_rodata = 43; /* R_ARM_MOVW_ABS_NC */
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
            bool is_chk_alias = (strncmp(alias, "__", 2) == 0 && strstr(alias, "_chk") != NULL);
            if (symbol_idx == 0 && (dlsym(RTLD_DEFAULT, alias) || is_chk_alias)) {
                elf_writer_add_symbol(&elf_ctx, alias, ELF_STB_GLOBAL,
                                      ELF_STT_FUNC, SHN_UNDEF, 0, 0);
                symbol_idx = (int)elf_ctx.symbol_count;
            }
        }
        if (symbol_idx == 0 && r->target_symbol[0]) {
            int sym_type = r->is_call ? ELF_STT_FUNC : ELF_STT_NOTYPE;
            elf_writer_add_symbol(&elf_ctx, r->target_symbol, ELF_STB_GLOBAL,
                                  sym_type, SHN_UNDEF, 0, 0);
            symbol_idx = (int)elf_ctx.symbol_count;
        }
        uint16_t cur_sec = text_idx;
        uint64_t cur_off = r->text_patch_offset;
        resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, r->text_patch_offset, &cur_sec, &cur_off);

        if (r->is_call) {
            elf_writer_add_relocation(&elf_ctx, cur_sec,
                                      cur_off,
                                      symbol_idx,
                                      reloc_type_call,
                                      addend_call);
        } else if (r->is_tls) {
            /* Static TLS (initial-exec, x86-64): the 32-bit displacement of
             * lea reg, [fs+disp32] gets R_X86_64_TPOFF32 against the
             * variable's .tdata/.tbss symbol. */
            elf_writer_add_relocation(&elf_ctx, cur_sec,
                                      cur_off,
                                      symbol_idx,
                                      0x17 /* R_X86_64_TPOFF32 */,
                                      0);
        } else if (symbol_idx > 0) {
            if ((comp->isa == ISA_RISCV64 || comp->isa == ISA_RISCV32) &&
                r->text_patch_offset >= 4) {
                uint16_t hi_sec = text_idx; uint64_t hi_off = r->text_patch_offset - 4;
                resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, r->text_patch_offset - 4, &hi_sec, &hi_off);
                /* RISC-V psABI: The auipc instruction at (patch_offset - 4) gets
                 * R_RISCV_PCREL_HI20 targeting symbol_idx (with addend 0).
                 * The following instruction at patch_offset gets R_RISCV_PCREL_LO12_I
                 * targeting a local label placed at the auipc's address (with addend 0). */
                char lbl[64];
                snprintf(lbl, sizeof(lbl), ".Lpcrel_sym_%zu", i);
                elf_writer_add_symbol(&elf_ctx, lbl,
                                      ELF_STB_LOCAL, ELF_STT_NOTYPE,
                                      hi_sec,
                                      hi_off,
                                      0);
                int label_sym_idx = elf_ctx.symbol_count;

                elf_writer_add_relocation(&elf_ctx, hi_sec,
                                          hi_off,
                                          symbol_idx,
                                          reloc_type_hi20,
                                          0);
                elf_writer_add_relocation(&elf_ctx, cur_sec,
                                          cur_off,
                                          label_sym_idx,
                                          reloc_type_rodata, /* R_RISCV_PCREL_LO12_I (24) */
                                          0);
            } else if (comp->isa == ISA_AARCH64 && r->text_patch_offset >= 4) {
                uint16_t hi_sec = text_idx; uint64_t hi_off = r->text_patch_offset - 4;
                resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, r->text_patch_offset - 4, &hi_sec, &hi_off);
                /* AArch64: adrp at (patch_offset - 4) gets R_AARCH64_ADR_PREL_PG_HI21 (275),
                 * add at patch_offset gets R_AARCH64_ADD_ABS_LO12_NC (277). */
                elf_writer_add_relocation(&elf_ctx, hi_sec,
                                          hi_off,
                                          symbol_idx,
                                          275 /* R_AARCH64_ADR_PREL_PG_HI21 */,
                                          0);
                elf_writer_add_relocation(&elf_ctx, cur_sec,
                                          cur_off,
                                          symbol_idx,
                                          277 /* R_AARCH64_ADD_ABS_LO12_NC */,
                                          0);
            } else if (comp->isa == ISA_AARCH32 && r->text_patch_offset >= 4) {
                uint16_t hi_sec = text_idx; uint64_t hi_off = r->text_patch_offset - 4;
                resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, r->text_patch_offset - 4, &hi_sec, &hi_off);
                /* AArch32: movw at (patch_offset - 4) gets R_ARM_MOVW_ABS_NC (43),
                 * movt at patch_offset gets R_ARM_MOVT_ABS (44). */
                elf_writer_add_relocation(&elf_ctx, hi_sec,
                                          hi_off,
                                          symbol_idx,
                                          43 /* R_ARM_MOVW_ABS_NC */,
                                          0);
                elf_writer_add_relocation(&elf_ctx, cur_sec,
                                          cur_off,
                                          symbol_idx,
                                          44 /* R_ARM_MOVT_ABS */,
                                          0);
            } else {
                int reloc_type_abs64 = (comp->isa == ISA_X86_64) ? 1 /* R_X86_64_64 */ : reloc_type_rodata;
                elf_writer_add_relocation(&elf_ctx, cur_sec,
                                          cur_off,
                                          symbol_idx,
                                          reloc_type_abs64,
                                          0);
            }
        }
    }

    /* Add relocation entries for string literals (.rodata references) */
    if (rodata_symbol_idx == 0) rodata_symbol_idx = 1;

    for (size_t i = 0; i < comp->str_reloc_count; i++) {
        ZeroCCStringReloc *sr = &comp->str_relocs[i];
        uint16_t cur_sec = text_idx;
        uint64_t cur_off = sr->text_patch_offset;
        resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, sr->text_patch_offset, &cur_sec, &cur_off);

        if ((comp->isa == ISA_RISCV64 || comp->isa == ISA_RISCV32) &&
            sr->text_patch_offset >= 4) {
            uint16_t hi_sec = text_idx; uint64_t hi_off = sr->text_patch_offset - 4;
            resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, sr->text_patch_offset - 4, &hi_sec, &hi_off);
            /* RISC-V psABI: The auipc instruction at (patch_offset - 4) gets
             * R_RISCV_PCREL_HI20 targeting .rodata section symbol with rodata_offset.
             * The following instruction (addi/fld) at patch_offset gets
             * R_RISCV_PCREL_LO12_I targeting a local label at the auipc with addend 0. */
            char lbl[64];
            snprintf(lbl, sizeof(lbl), ".Lpcrel_str_%zu", i);
            elf_writer_add_symbol(&elf_ctx, lbl,
                                  ELF_STB_LOCAL, ELF_STT_NOTYPE,
                                  hi_sec,
                                  hi_off,
                                  0);
            int label_sym_idx = elf_ctx.symbol_count;

            elf_writer_add_relocation(&elf_ctx, hi_sec,
                                      hi_off,
                                      rodata_symbol_idx,
                                      reloc_type_hi20,
                                      (int64_t)sr->rodata_offset + addend_rodata);
            elf_writer_add_relocation(&elf_ctx, cur_sec,
                                      cur_off,
                                      label_sym_idx,
                                      reloc_type_rodata, /* R_RISCV_PCREL_LO12_I (24) */
                                      0);
        } else if (comp->isa == ISA_AARCH64 && sr->text_patch_offset >= 4) {
            uint16_t hi_sec = text_idx; uint64_t hi_off = sr->text_patch_offset - 4;
            resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, sr->text_patch_offset - 4, &hi_sec, &hi_off);
            /* AArch64: adrp at (patch_offset - 4) gets R_AARCH64_ADR_PREL_PG_HI21 (275),
             * add at patch_offset gets R_AARCH64_ADD_ABS_LO12_NC (277). */
            elf_writer_add_relocation(&elf_ctx, hi_sec,
                                      hi_off,
                                      rodata_symbol_idx,
                                      275 /* R_AARCH64_ADR_PREL_PG_HI21 */,
                                      (int64_t)sr->rodata_offset);
            elf_writer_add_relocation(&elf_ctx, cur_sec,
                                      cur_off,
                                      rodata_symbol_idx,
                                      277 /* R_AARCH64_ADD_ABS_LO12_NC */,
                                      (int64_t)sr->rodata_offset);
        } else if (comp->isa == ISA_AARCH32 && sr->text_patch_offset >= 4) {
            uint16_t hi_sec = text_idx; uint64_t hi_off = sr->text_patch_offset - 4;
            resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, sr->text_patch_offset - 4, &hi_sec, &hi_off);
            /* AArch32: movw at (patch_offset - 4) gets R_ARM_MOVW_ABS_NC (43),
             * movt at patch_offset gets R_ARM_MOVT_ABS (44). */
            elf_writer_add_relocation(&elf_ctx, hi_sec,
                                      hi_off,
                                      rodata_symbol_idx,
                                      43 /* R_ARM_MOVW_ABS_NC */,
                                      (int64_t)sr->rodata_offset);
            elf_writer_add_relocation(&elf_ctx, cur_sec,
                                      cur_off,
                                      rodata_symbol_idx,
                                      44 /* R_ARM_MOVT_ABS */,
                                      (int64_t)sr->rodata_offset);
        } else {
            elf_writer_add_relocation(&elf_ctx, cur_sec,
                                      cur_off,
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
        uint16_t tgt_sec = text_idx; uint64_t tgt_off = comp->nl_goto_relocs[i].text_target_offset;
        resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, comp->nl_goto_relocs[i].text_target_offset, &tgt_sec, &tgt_off);

        char lbl[64];
        snprintf(lbl, sizeof(lbl), ".Lnl_goto_%zu", i);
        elf_writer_add_symbol(&elf_ctx, lbl,
                              ELF_STB_LOCAL, ELF_STT_NOTYPE,
                              tgt_sec,
                              tgt_off,
                              0);
        int label_sym_idx = elf_ctx.symbol_count;

        uint16_t cur_sec = text_idx; uint64_t cur_off = comp->nl_goto_relocs[i].text_patch_offset;
        resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, comp->nl_goto_relocs[i].text_patch_offset, &cur_sec, &cur_off);

        if (comp->isa == ISA_RISCV64 || comp->isa == ISA_RISCV32) {
            uint16_t hi_sec = text_idx; uint64_t hi_off = comp->nl_goto_relocs[i].text_patch_offset - 4;
            resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, comp->nl_goto_relocs[i].text_patch_offset - 4, &hi_sec, &hi_off);

            char auipc_lbl[64];
            snprintf(auipc_lbl, sizeof(auipc_lbl), ".Lnl_goto_auipc_%zu", i);
            elf_writer_add_symbol(&elf_ctx, auipc_lbl,
                                  ELF_STB_LOCAL, ELF_STT_NOTYPE,
                                  hi_sec,
                                  hi_off,
                                  0);
            int auipc_sym_idx = elf_ctx.symbol_count;
            elf_writer_add_relocation(&elf_ctx, hi_sec,
                                      hi_off,
                                      label_sym_idx,
                                      reloc_type_hi20, /* R_RISCV_PCREL_HI20 (23) */
                                      0);
            elf_writer_add_relocation(&elf_ctx, cur_sec,
                                      cur_off,
                                      auipc_sym_idx,
                                      reloc_type_rodata, /* R_RISCV_PCREL_LO12_I (24) */
                                      0);
        } else if (comp->isa == ISA_AARCH64) {
            uint16_t hi_sec = text_idx; uint64_t hi_off = comp->nl_goto_relocs[i].text_patch_offset - 4;
            resolve_text_reloc_loc(comp, &elf_ctx, order, extent_end, nreal, text_idx, comp->nl_goto_relocs[i].text_patch_offset - 4, &hi_sec, &hi_off);
            elf_writer_add_relocation(&elf_ctx, hi_sec,
                                      hi_off,
                                      label_sym_idx,
                                      275 /* R_AARCH64_ADR_PREL_PG_HI21 */,
                                      0);
            elf_writer_add_relocation(&elf_ctx, cur_sec,
                                      cur_off,
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
        case ISA_AARCH32: reloc_type_data_abs = 2;   /* R_ARM_ABS32 */ break;
        case ISA_RISCV64: reloc_type_data_abs = 2;   /* R_RISCV_64 */ break;
        case ISA_RISCV32: reloc_type_data_abs = 1;   /* R_RISCV_32 */ break;
        default: break;
    }
    if (comp->data_section.length > 0) {
        for (size_t i = 0; i < comp->data_reloc_count; i++) {
            ZeroCCDataReloc *dr = &comp->data_relocs[i];
            bool in_elim_gv = false;
            for (size_t gi = 0; gi < comp->global_var_count; gi++) {
                const ZeroCCGlobalVar *egv = &comp->global_vars[gi];
                if (!egv->lto_eliminated || egv->is_bss || egv->is_tls || egv->alias_target[0]) continue;
                size_t esz = egv->size ? egv->size : egv->type.size;
                if (dr->data_patch_offset >= egv->data_offset &&
                    dr->data_patch_offset < egv->data_offset + esz) {
                    in_elim_gv = true;
                    break;
                }
            }
            if (in_elim_gv) continue;

            uint16_t d_sec = data_section_idx;
            uint64_t d_off = dr->data_patch_offset;
            resolve_data_reloc_loc(comp, &elf_ctx, data_section_idx, dr->data_patch_offset, &d_sec, &d_off);

            if (dr->target_kind == 0) {
                /* .rodata + rodata_offset + target_data_offset: target the .rodata section symbol (idx 1) */
                if (comp->rodata_section.length > 0) {
                    elf_writer_add_relocation(&elf_ctx, d_sec, d_off,
                                              rodata_symbol_idx, reloc_type_data_abs,
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
                    elf_writer_add_relocation(&elf_ctx, d_sec, d_off,
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
                elf_writer_add_relocation(&elf_ctx, d_sec, d_off,
                                          sym_idx, reloc_type_data_abs, (int64_t)dr->target_data_offset);
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
                elf_writer_add_relocation(&elf_ctx, d_sec, d_off,
                                          sym_idx, reloc_type_data_abs, (int64_t)dr->target_data_offset);
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
                elf_writer_add_relocation(&elf_ctx, d_sec, d_off,
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
                elf_writer_add_relocation(&elf_ctx, d_sec, d_off,
                                          tsym, reloc_type_data_abs,
                                          (int64_t)dr->target_data_offset);
            }
        }
    }

    free(extent_end);
    free(order);

    /* Fat-LTO payload (.zc.lto, non-allocatable preprocessed C source) */
    if (lto_prep_source && lto_prep_source[0] && !opts->emit_asm) {
        elf_writer_add_section(&elf_ctx, ".zc.lto",
                               (const uint8_t *)lto_prep_source,
                               strlen(lto_prep_source) + 1,
                               ELF_SHT_PROGBITS, 0, 1);
    }

    /* Emit DWARF debug info (.debug_line, .debug_info, .debug_abbrev, .debug_str) */
    if (comp->debug_info && !opts->emit_asm) {
        zero_cc_dwarf_emit(comp, &elf_ctx);
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
        return 1;
    } else if (opts->verbose) {
        printf("Object file written successfully\n");
    }

    /* Cleanup */
    elf_writer_cleanup(&elf_ctx);

    return 0;
}

/*
 * RV32 system-linker musl-cross prefix (Sprint 157, plan item #10):
 * the Debian riscv64 cross gcc ships no rv32 musl, so the RV32 -nostdlib
 * link must name an explicit musl crt/libc/libgcc.  Discovery follows the
 * zero_rt-archive pattern (env var first, then a path relative to this
 * binary):  (1) $ZC_RV32_MUSL = a prefix dir containing lib/; (2) the
 * in-tree vendored prefix <exe_dir>/../../third_party/vendor/musl-cross/
 * riscv32 (committed, so the tree links RV32 on any machine without setup;
 * rebuild it in place with tools/setup_musl_cross.sh).  The prefix is
 * validated (lib/crt1.o readable) before being used.
 */
static bool find_rv32_musl_prefix(char *out, size_t out_len) {
    const char *env = getenv("ZC_RV32_MUSL");
    char cand[512];
    if (env && env[0]) {
        if (strlen(env) >= sizeof cand) return false;
        snprintf(cand, sizeof cand, "%s", env);
    } else {
        char self[512];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0) return false;
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (!slash) return false;
        *slash = '\0';
        snprintf(cand, sizeof cand,
                 "%s/../../third_party/vendor/musl-cross/riscv32", self);
    }
    char probe[560];
    snprintf(probe, sizeof probe, "%s/lib/crt1.o", cand);
    if (access(probe, R_OK) != 0) return false;
    snprintf(out, out_len, "%s", cand);
    return true;
}

static int compile_and_link(CompileOptions *opts) {
    if (opts->is_pe) {
        if (opts->input_count == 0) return 1;
        const char *output = opts->output_file ? opts->output_file : (opts->is_shared ? "a.dll" : "a.exe");

        bool has_obj_input = false;
        for (int i = 0; i < opts->input_count; i++) {
            const char *in = opts->input_files[i];
            size_t len = strlen(in);
            if ((len >= 2 && strcmp(in + len - 2, ".o") == 0) ||
                (len >= 4 && strcmp(in + len - 4, ".obj") == 0)) {
                has_obj_input = true;
                break;
            }
            FILE *fprobe = fopen(in, "rb");
            if (fprobe) {
                uint16_t machine = 0;
                if (fread(&machine, 2, 1, fprobe) == 1 && machine == ZERO_CC_IMAGE_FILE_MACHINE_AMD64) {
                    has_obj_input = true;
                }
                fclose(fprobe);
                if (has_obj_input) break;
            }
        }

        if (has_obj_input || opts->input_count > 1) {
            char obj_paths[ZERO_CC_MAX_INPUTS][512];
            int obj_temp[ZERO_CC_MAX_INPUTS];
            int obj_count = 0;
            CompileOptions obj_opts = *opts;
            obj_opts.emit_object = 1;
            obj_opts.emit_asm = 0;
            obj_opts.output_file = NULL;
            obj_opts.verbose = 0;

            for (int i = 0; i < opts->input_count; i++) {
                const char *in = opts->input_files[i];
                size_t len = strlen(in);
                bool is_obj = (len >= 2 && strcmp(in + len - 2, ".o") == 0) ||
                              (len >= 4 && strcmp(in + len - 4, ".obj") == 0);
                if (!is_obj) {
                    FILE *fp = fopen(in, "rb");
                    if (fp) {
                        uint16_t machine = 0;
                        if (fread(&machine, 2, 1, fp) == 1 && machine == ZERO_CC_IMAGE_FILE_MACHINE_AMD64) {
                            is_obj = true;
                        }
                        fclose(fp);
                    }
                }

                if (is_obj) {
                    snprintf(obj_paths[obj_count], sizeof(obj_paths[0]), "%s", in);
                    obj_temp[obj_count] = 0;
                    obj_count++;
                } else {
                    snprintf(obj_paths[obj_count], sizeof(obj_paths[0]), "/tmp/zerocc_pe_%d_%d.obj",
                             getpid(), obj_count);
                    obj_temp[obj_count] = 1;
                    int r = compile_object_single(&obj_opts, in, obj_paths[obj_count]);
                    if (r != 0) {
                        for (int k = 0; k < obj_count; k++) {
                            if (obj_temp[k]) unlink(obj_paths[k]);
                        }
                        return r;
                    }
                    obj_count++;
                }
            }

            const char *obj_ptrs[ZERO_CC_MAX_INPUTS];
            for (int i = 0; i < obj_count; i++) obj_ptrs[i] = obj_paths[i];

            ZeroCCPeOptions pe_opts;
            memset(&pe_opts, 0, sizeof(pe_opts));
            pe_opts.is_dll = (opts->is_shared != 0);
            pe_opts.is_gui = (opts->is_windows_gui != 0);
            pe_opts.dll_name = output;

            bool ok = zero_cc_pe_link_objects(output, obj_ptrs, (size_t)obj_count, &pe_opts);

            for (int k = 0; k < obj_count; k++) {
                if (obj_temp[k]) unlink(obj_paths[k]);
            }

            if (!ok) {
                fprintf(stderr, "zerocc: error linking PE %s: %s\n",
                        opts->is_shared ? "DLL" : "executable", output);
                return 1;
            }
            if (opts->verbose) {
                printf("Successfully generated PE32+ %s: %s\n",
                       opts->is_shared ? "DLL" : "executable", output);
            }
            return 0;
        }

        ZeroCCCompiler *comp = jit_compile_tu(opts, opts->input_files[0], 0);
        if (!comp) return 1;
        ZeroCCPeOptions pe_opts;
        memset(&pe_opts, 0, sizeof(pe_opts));
        pe_opts.is_dll = (opts->is_shared != 0);
        pe_opts.is_gui = (opts->is_windows_gui != 0);
        pe_opts.dll_name = output;
        bool ok = zero_cc_pe_write_binary_from_comp(output, comp, &pe_opts);
        zero_cc_compiler_free(comp);
        if (!ok) {
            fprintf(stderr, "zerocc: error writing PE %s: %s\n", opts->is_shared ? "DLL" : "executable", output);
            return 1;
        }
        if (opts->verbose) {
            printf("Successfully generated PE32+ %s: %s\n", opts->is_shared ? "DLL" : "executable", output);
        }
        return 0;
    }

    if (opts->is_macho) {
        if (opts->input_count == 0) return 1;
        const char *output = opts->output_file ? opts->output_file : (opts->is_shared ? "a.dylib" : "a.out");

        bool has_obj_input = false;
        for (int i = 0; i < opts->input_count; i++) {
            const char *in = opts->input_files[i];
            size_t len = strlen(in);
            if ((len >= 2 && strcmp(in + len - 2, ".o") == 0) ||
                (len >= 4 && strcmp(in + len - 4, ".obj") == 0)) {
                has_obj_input = true;
                break;
            }
            FILE *fprobe = fopen(in, "rb");
            if (fprobe) {
                uint32_t magic = 0;
                if (fread(&magic, 4, 1, fprobe) == 1 && magic == ZERO_CC_MH_MAGIC_64) {
                    has_obj_input = true;
                }
                fclose(fprobe);
                if (has_obj_input) break;
            }
        }

        if (has_obj_input || opts->input_count > 1) {
            char obj_paths[ZERO_CC_MAX_INPUTS][512];
            int obj_temp[ZERO_CC_MAX_INPUTS];
            int obj_count = 0;
            CompileOptions obj_opts = *opts;
            obj_opts.emit_object = 1;
            obj_opts.emit_asm = 0;
            obj_opts.output_file = NULL;
            obj_opts.verbose = 0;

            for (int i = 0; i < opts->input_count; i++) {
                const char *in = opts->input_files[i];
                size_t len = strlen(in);
                bool is_obj = (len >= 2 && strcmp(in + len - 2, ".o") == 0) ||
                              (len >= 4 && strcmp(in + len - 4, ".obj") == 0);
                if (!is_obj) {
                    FILE *fp = fopen(in, "rb");
                    if (fp) {
                        uint32_t magic = 0;
                        if (fread(&magic, 4, 1, fp) == 1 && magic == ZERO_CC_MH_MAGIC_64) {
                            is_obj = true;
                        }
                        fclose(fp);
                    }
                }

                if (is_obj) {
                    snprintf(obj_paths[obj_count], sizeof(obj_paths[0]), "%s", in);
                    obj_temp[obj_count] = 0;
                    obj_count++;
                } else {
                    snprintf(obj_paths[obj_count], sizeof(obj_paths[0]), "/tmp/zerocc_macho_%d_%d.o",
                             getpid(), obj_count);
                    obj_temp[obj_count] = 1;
                    int r = compile_object_single(&obj_opts, in, obj_paths[obj_count]);
                    if (r != 0) {
                        for (int k = 0; k < obj_count; k++) {
                            if (obj_temp[k]) unlink(obj_paths[k]);
                        }
                        return r;
                    }
                    obj_count++;
                }
            }

            const char *obj_ptrs[ZERO_CC_MAX_INPUTS];
            for (int i = 0; i < obj_count; i++) obj_ptrs[i] = obj_paths[i];

            ZeroCCMachoOptions macho_opts;
            memset(&macho_opts, 0, sizeof(macho_opts));
            macho_opts.is_dylib = (opts->is_shared != 0);
            macho_opts.dylib_name = output;
            ZeroCCISA target_isa = parse_target_arch(opts->target_arch);
            macho_opts.cputype = (target_isa == ISA_AARCH64) ? ZERO_CC_CPU_TYPE_ARM64 : ZERO_CC_CPU_TYPE_X86_64;

            bool ok = zero_cc_macho_link_objects(output, obj_ptrs, (size_t)obj_count, &macho_opts);

            for (int k = 0; k < obj_count; k++) {
                if (obj_temp[k]) unlink(obj_paths[k]);
            }

            if (!ok) {
                fprintf(stderr, "zerocc: error linking Mach-O %s: %s\n",
                        opts->is_shared ? "dylib" : "executable", output);
                return 1;
            }
            if (opts->verbose) {
                printf("Successfully generated Mach-O 64 %s: %s\n",
                       opts->is_shared ? "dylib" : "executable", output);
            }
            return 0;
        }

        /* Single C source file fast-path */
        ZeroCCCompiler *comp = jit_compile_tu(opts, opts->input_files[0], 0);
        if (!comp) return 1;
        ZeroCCMachoOptions macho_opts;
        memset(&macho_opts, 0, sizeof(macho_opts));
        macho_opts.is_dylib = (opts->is_shared != 0);
        macho_opts.dylib_name = output;
        macho_opts.cputype = (comp->isa == ISA_AARCH64) ? ZERO_CC_CPU_TYPE_ARM64 : ZERO_CC_CPU_TYPE_X86_64;
        bool ok = zero_cc_macho_write_binary_from_comp(output, comp, &macho_opts);
        zero_cc_compiler_free(comp);
        if (!ok) {
            fprintf(stderr, "zerocc: error writing Mach-O %s: %s\n", opts->is_shared ? "dylib" : "executable", output);
            return 1;
        }
        if (opts->verbose) {
            printf("Successfully generated Mach-O 64 %s: %s\n", opts->is_shared ? "dylib" : "executable", output);
        }
        return 0;
    }

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
    const char *arch_paths[64];
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

    if (opts->lto && !use_text) {
        ZeroCCLTOContext lto;
        zero_cc_lto_init(&lto);

        for (int i = 0; i < opts->input_count; i++) {
            const char *in = opts->input_files[i];
            size_t len = strlen(in);

            if (len >= 2 && strcmp(in + len - 2, ".a") == 0) {
                ZcAr ar;
                char ar_err[256];
                if (zc_ar_open(in, &ar, ar_err, sizeof(ar_err)) == 0) {
                    bool has_lto_member = false;
                    bool has_non_lto_member = false;
                    for (size_t m = 0; m < ar.member_count; m++) {
                        ZcElfObj mobj;
                        char m_err[256];
                        const char *mname = ar.members[m].name ? ar.members[m].name : in;
                        if (zc_elf_obj_load_mem(ar.members[m].data, ar.members[m].size,
                                                mname, &mobj, m_err, sizeof(m_err)) != 0) {
                            continue;
                        }
                        ZcSection *sec = zc_elf_find_section(&mobj, ".zc.lto");
                        if (sec && sec->size > 0 && sec->offset + sec->size <= mobj.size) {
                            has_lto_member = true;
                            char *lbuf = (char *)malloc(sec->size + 1);
                            if (lbuf) {
                                memcpy(lbuf, mobj.data + sec->offset, sec->size);
                                lbuf[sec->size] = '\0';
                                ZeroCCCompiler *tu = lto_compile_tu_phase1(opts, mname, lbuf, lto.tu_count);
                                free(lbuf);
                                if (!tu || zero_cc_lto_add_tu(&lto, tu) != 0) {
                                    if (tu) zero_cc_compiler_free(tu);
                                    if (lto.had_error) fprintf(stderr, "zerocc: error: %s\n", lto.error_msg);
                                    zc_elf_obj_free(&mobj);
                                    zc_ar_free(&ar);
                                    zero_cc_lto_cleanup(&lto);
                                    for (int k = 0; k < obj_count; k++) if (obj_temp[k]) unlink(obj_paths[k]);
                                    return 1;
                                }
                            }
                        } else {
                            has_non_lto_member = true;
                            const ZcSection *symtab = zc_elf_symtab(&mobj);
                            if (symtab) {
                                for (size_t s = 0; s < symtab->sym_count; s++) {
                                    const ZcSymbol *sym = &symtab->syms[s];
                                    if (sym->shndx == SHN_UNDEF && sym->name && sym->name[0]) {
                                        zero_cc_lto_add_ext_root(&lto, sym->name);
                                    }
                                }
                            }
                        }
                        zc_elf_obj_free(&mobj);
                    }
                    if ((has_non_lto_member || !has_lto_member) && arch_count < 64) {
                        arch_paths[arch_count++] = in;
                    }
                    zc_ar_free(&ar);
                } else {
                    if (arch_count < 64) arch_paths[arch_count++] = in;
                }
                continue;
            }

            if (len >= 2 && strcmp(in + len - 2, ".o") == 0) {
                ZcElfObj oobj;
                char o_err[256];
                if (zc_elf_obj_load(in, &oobj, o_err, sizeof(o_err)) == 0) {
                    ZcSection *sec = zc_elf_find_section(&oobj, ".zc.lto");
                    if (sec && sec->size > 0 && sec->offset + sec->size <= oobj.size) {
                        char *lbuf = (char *)malloc(sec->size + 1);
                        if (lbuf) {
                            memcpy(lbuf, oobj.data + sec->offset, sec->size);
                            lbuf[sec->size] = '\0';
                            ZeroCCCompiler *tu = lto_compile_tu_phase1(opts, in, lbuf, lto.tu_count);
                            free(lbuf);
                            if (!tu || zero_cc_lto_add_tu(&lto, tu) != 0) {
                                if (tu) zero_cc_compiler_free(tu);
                                if (lto.had_error) fprintf(stderr, "zerocc: error: %s\n", lto.error_msg);
                                zc_elf_obj_free(&oobj);
                                zero_cc_lto_cleanup(&lto);
                                for (int k = 0; k < obj_count; k++) if (obj_temp[k]) unlink(obj_paths[k]);
                                return 1;
                            }
                        }
                    } else {
                        const ZcSection *symtab = zc_elf_symtab(&oobj);
                        if (symtab) {
                            for (size_t s = 0; s < symtab->sym_count; s++) {
                                const ZcSymbol *sym = &symtab->syms[s];
                                if (sym->shndx == SHN_UNDEF && sym->name && sym->name[0]) {
                                    zero_cc_lto_add_ext_root(&lto, sym->name);
                                }
                            }
                        }
                        if (obj_count < ZERO_CC_MAX_INPUTS) {
                            snprintf(obj_paths[obj_count], sizeof(obj_paths[0]), "%s", in);
                            obj_temp[obj_count] = 0;
                            obj_count++;
                        }
                    }
                    zc_elf_obj_free(&oobj);
                } else if (obj_count < ZERO_CC_MAX_INPUTS) {
                    snprintf(obj_paths[obj_count], sizeof(obj_paths[0]), "%s", in);
                    obj_temp[obj_count] = 0;
                    obj_count++;
                }
                continue;
            }

            char *source = read_file(in);
            if (!source) {
                zero_cc_lto_cleanup(&lto);
                for (int k = 0; k < obj_count; k++) if (obj_temp[k]) unlink(obj_paths[k]);
                return 1;
            }
            char *prep = preprocess_with_options(source, opts, in, obj_opts.dep_target);
            free(source);
            if (!prep) {
                zero_cc_lto_cleanup(&lto);
                for (int k = 0; k < obj_count; k++) if (obj_temp[k]) unlink(obj_paths[k]);
                return 1;
            }
            ZeroCCCompiler *tu = lto_compile_tu_phase1(opts, in, prep, lto.tu_count);
            free(prep);
            if (!tu || zero_cc_lto_add_tu(&lto, tu) != 0) {
                if (tu) zero_cc_compiler_free(tu);
                if (lto.had_error) fprintf(stderr, "zerocc: error: %s\n", lto.error_msg);
                zero_cc_lto_cleanup(&lto);
                for (int k = 0; k < obj_count; k++) if (obj_temp[k]) unlink(obj_paths[k]);
                return 1;
            }
        }

        if (lto.tu_count > 0) {
            if (zero_cc_lto_analyze(&lto) != 0 || zero_cc_lto_optimize_and_lower(&lto) != 0) {
                fprintf(stderr, "zerocc: error: %s\n",
                        lto.error_msg[0] ? lto.error_msg : "LTO optimization failed");
                zero_cc_lto_cleanup(&lto);
                for (int k = 0; k < obj_count; k++) if (obj_temp[k]) unlink(obj_paths[k]);
                return 1;
            }
            for (int t = 0; t < lto.tu_count; t++) {
                if (obj_count >= ZERO_CC_MAX_INPUTS) {
                    fprintf(stderr, "zerocc: error: too many object files after LTO\n");
                    zero_cc_lto_cleanup(&lto);
                    for (int k = 0; k < obj_count; k++) if (obj_temp[k]) unlink(obj_paths[k]);
                    return 1;
                }
                snprintf(obj_paths[obj_count], sizeof(obj_paths[0]), "/tmp/zerocc_%d_lto_%d.o",
                         getpid(), t);
                obj_temp[obj_count] = 1;
                int wr = write_compiled_object(lto.tus[t], &obj_opts, obj_paths[obj_count], NULL);
                if (wr != 0) {
                    zero_cc_lto_cleanup(&lto);
                    for (int k = 0; k <= obj_count; k++) if (obj_temp[k]) unlink(obj_paths[k]);
                    return wr;
                }
                obj_count++;
            }
        }
        zero_cc_lto_cleanup(&lto);
    } else {
        for (int i = 0; i < opts->input_count; i++) {
            const char *in = opts->input_files[i];
            size_t len = strlen(in);
            /* T2: pass through prebuilt objects and archives to the linker. */
            if (len >= 2 && strcmp(in + len - 2, ".a") == 0) {
                if (opts->zerocc_link && arch_count < 64) {
                    arch_paths[arch_count++] = in;
                } else if (use_text) {
                    fprintf(stderr, "Error: %s: archive input with -S is not "
                            "supported\n", in);
                    return 1;
                } else {
                    /* System linker: pass the archive through in the link cmd. */
                    if (arch_count < 64) arch_paths[arch_count++] = in;
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
    }

    char all_objs[65536] = "";
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
                if (mach == 40) target_isa = ISA_AARCH32;      /* EM_ARM */
                else if (mach == 183) target_isa = ISA_AARCH64;
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
    if (opts->baremetal) {
        rt_filename = "zero_rt_baremetal.a";
        env_var = "ZERO_RT_BAREMETAL_A";
        if (target_isa == ISA_RISCV32) {
            rt_filename = "zero_rt_baremetal_rv32.a";
            env_var = "ZERO_RT_BAREMETAL_RV32_A";
        } else if (target_isa == ISA_AARCH32) {
            rt_filename = "zero_rt_baremetal_arm.a";
            env_var = "ZERO_RT_BAREMETAL_ARM_A";
        } else if (target_isa == ISA_RISCV64) {
            rt_filename = "zero_rt_baremetal_riscv64.a";
            env_var = "ZERO_RT_BAREMETAL_RISCV64_A";
        } else if (target_isa == ISA_AARCH64) {
            rt_filename = "zero_rt_baremetal_aarch64.a";
            env_var = "ZERO_RT_BAREMETAL_AARCH64_A";
        }
    } else {
        if (target_isa == ISA_AARCH64) {
            rt_filename = "zero_rt_aarch64.a";
            env_var = "ZERO_RT_AARCH64_A";
        } else if (target_isa == ISA_AARCH32) {
            rt_filename = "zero_rt_aarch32.a";
            env_var = "ZERO_RT_AARCH32_A";
        } else if (target_isa == ISA_RISCV64) {
            rt_filename = "zero_rt_riscv64.a";
            env_var = "ZERO_RT_RISCV64_A";
        } else if (target_isa == ISA_RISCV32) {
            rt_filename = "zero_rt_riscv32.a";
            env_var = "ZERO_RT_RISCV32_A";
        }
    }

    char rt_path[2048] = "";
    const char *rt_env = getenv(env_var);
    if (rt_env && rt_env[0]) {
        snprintf(rt_path, sizeof rt_path, "%s", rt_env);
    } else if (target_isa == ISA_X86_64 || target_isa == ISA_AARCH64 ||
               target_isa == ISA_AARCH32 ||
               target_isa == ISA_RISCV64 || target_isa == ISA_RISCV32) {
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

    /* T2: ZeroCC's own linker (x86-64, AArch64, AArch32, RISC-V 64/32; zero_rt is its
     * first customer). */
    if (opts->zerocc_link) {
        if (target_isa != ISA_X86_64 && target_isa != ISA_AARCH64 &&
            target_isa != ISA_AARCH32 && target_isa != ISA_RISCV64 &&
            target_isa != ISA_RISCV32) {
            fprintf(stderr,
                    "Error: --zerocc-link currently links x86-64, aarch64, "
                    "riscv64, and riscv32 objects only\n");
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

        /* -fsanitize=leak (Sprint 169): the leak report lives in the
         * freestanding zero_rt heap, so the stub must route main through
         * zeroc_leak_main_wrapper (which enables the atexit report). The
         * flag is a no-op for host-linked / JIT builds - the host allocator
         * cannot be inspected (use the host LSan/Valgrind there). */
        const char *entry_wrapper = NULL;
        if (opts->sanitize_leak) {
            if (!opts->no_zerocc_rt && rt_path[0] && access(rt_path, R_OK) == 0) {
                entry_wrapper = "zeroc_leak_main_wrapper";
            } else {
                fprintf(stderr,
                        "zerocc: note: -fsanitize=leak is freestanding-only "
                        "and zero_rt is not linked (--no-zerocc-rt or zero_rt "
                        "missing); the atexit leak report is disabled\n");
            }
        }

        if (!opts->no_zerocc_rt && rt_path[0] && access(rt_path, R_OK) == 0
            && arch_count < 64) {
            arch_paths[arch_count++] = rt_path;
            if (opts->verbose) printf("Linking zero_rt: %s\n", rt_path);
        }

        /* Sprint 173 (rv32 i64 Phase 3): 64-bit div/rem lowers to a libgcc
         * __*di3 call. On rv32 the committed musl-cross prefix's libgcc.a
         * (the shim - single provider, Sprint 167) supplies them for the
         * own-link path, mirroring the system link line. (Skipped in baremetal). */
        if (!opts->baremetal && target_isa == ISA_RISCV32 && arch_count < 64) {
            char musl_prefix[512];
            char gcc_path[600];
            if (find_rv32_musl_prefix(musl_prefix, sizeof musl_prefix)) {
                snprintf(gcc_path, sizeof gcc_path, "%s/lib/libgcc.a", musl_prefix);
                if (access(gcc_path, R_OK) == 0) {
                    arch_paths[arch_count++] = gcc_path;
                    if (opts->verbose) printf("Linking rv32 libgcc shim: %s\n", gcc_path);
                }
            }
        }

        ZcLinkOpts lopts = {0};
        lopts.linker_script = opts->linker_script;
        lopts.baremetal = opts->baremetal ? true : false;
        lopts.entry_wrapper = entry_wrapper;
        lopts.pie = opts->zerocc_link_pie ? true : false;
        lopts.gc_sections = (opts->gc_sections || (opts->lto && !opts->gc_sections_explicit)) ? true : false;
        lopts.print_gc_sections = opts->print_gc_sections ? true : false;
        lopts.entry_symbol = opts->entry_symbol;
        lopts.undefined_syms = opts->undefined_syms;
        lopts.undefined_sym_count = opts->undefined_sym_count;

        int lr = zero_cc_link_exec_ext(output, objs_ref, obj_count,
                                       arch_paths, arch_count,
                                       &lopts, link_err,
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
    char cmd[131072];

    /* Select appropriate linker based on target ISA.  (RV32 is NOT here:
     * it has no stock cross musl and links through the explicit-musl
     * command below, which discovers its prefix - see find_rv32_musl_prefix.) */
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
    } else if (target_isa == ISA_AARCH32) {
        linker = "arm-linux-gnueabihf-gcc";
        abi_flags = "-mabi=aapcs-linux -march=armv7ve -mfloat-abi=hard";
        static_flag = "-static";
    }

    char lib_args[1024] = "";
    for (int i = 0; i < opts->lib_count; i++) {
        strncat(lib_args, " ", sizeof(lib_args) - strlen(lib_args) - 1);
        strncat(lib_args, opts->libs[i], sizeof(lib_args) - strlen(lib_args) - 1);
    }

    char gc_args[1024] = "";
    if (opts->gc_sections || (opts->lto && !opts->gc_sections_explicit)) {
        strncat(gc_args, " -Wl,--gc-sections", sizeof(gc_args) - strlen(gc_args) - 1);
    }
    if (opts->print_gc_sections) {
        strncat(gc_args, " -Wl,--print-gc-sections", sizeof(gc_args) - strlen(gc_args) - 1);
    }
    if (opts->entry_symbol) {
        char e_buf[256];
        snprintf(e_buf, sizeof(e_buf), " -Wl,-e,%s", opts->entry_symbol);
        strncat(gc_args, e_buf, sizeof(gc_args) - strlen(gc_args) - 1);
    }
    for (int i = 0; i < opts->undefined_sym_count; i++) {
        char u_buf[256];
        snprintf(u_buf, sizeof(u_buf), " -Wl,-u,%s", opts->undefined_syms[i]);
        strncat(gc_args, u_buf, sizeof(gc_args) - strlen(gc_args) - 1);
    }

    if (target_isa == ISA_RISCV32) {
        char musl_prefix[512];
        if (!find_rv32_musl_prefix(musl_prefix, sizeof musl_prefix)) {
            fprintf(stderr,
                    "Error: RV32 musl-cross prefix not found.  Set "
                    "ZC_RV32_MUSL to a prefix containing lib/ (crt1.o, "
                    "crti.o, libc.a, libgcc.a, crtn.o) - see "
                    "tools/setup_musl_cross.sh - or install one at the "
                    "in-tree default third_party/vendor/musl-cross/riscv32.\n");
            for (int i = 0; i < obj_count; i++) unlink(obj_paths[i]);
            return 1;
        }
        /* zero_rt_riscv32.a as a fallback archive after libc (parity with
         * the other ISAs' system link): supplies zero_rt-only symbols
         * (threads, __int128 math, zero_san __zerocsan_report, ...).  It
         * never shadows musl (listed after libc.a). */
        char rt_arg[2100] = "";
        if (!opts->no_zerocc_rt && rt_path[0] && access(rt_path, R_OK) == 0) {
            snprintf(rt_arg, sizeof(rt_arg), " %s", rt_path);
        }
        snprintf(cmd, sizeof(cmd),
                 "riscv64-linux-gnu-gcc -march=rv32gc -mabi=ilp32 -static -no-pie -Wl,-m,elf32lriscv -nostdlib "
                 "%s/lib/crt1.o %s/lib/crti.o "
                 "%s "
                 "%s/lib/libc.a %s/lib/libgcc.a%s %s/lib/crtn.o "
                 "-o %s%s%s 2>&1",
                 musl_prefix, musl_prefix,
                 all_objs,
                 musl_prefix, musl_prefix, rt_arg, musl_prefix,
                 output, lib_args, gc_args);
    } else {
        char rt_arg[2100] = "";
        if (!opts->no_zerocc_rt && rt_path[0] && access(rt_path, R_OK) == 0) {
            snprintf(rt_arg, sizeof(rt_arg), " %s", rt_path);
        }
        char pgo_arg[1024] = "";
        if (opts->profile_generate) {
            char self_exe[512];
            ssize_t n_self = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
            if (n_self > 0) {
                self_exe[n_self] = '\0';
                char *slash = strrchr(self_exe, '/');
                if (slash) {
                    *slash = '\0';
                    char pgo_obj[1024];
                    snprintf(pgo_obj, sizeof(pgo_obj), "%s/../obj/src/zero_cc/jit/pgo_runtime.o", self_exe);
                    if (access(pgo_obj, R_OK) == 0) {
                        snprintf(pgo_arg, sizeof(pgo_arg), " %s", pgo_obj);
                    }
                }
            }
        }
        snprintf(cmd, sizeof(cmd),
                 "%s %s %s -Wl,--defsym=link_error=0 -o %s %s%s -lc -lm%s%s%s 2>&1",
                 linker, abi_flags, static_flag, output, all_objs, pgo_arg, rt_arg, lib_args, gc_args);
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
    const char *as = (tisa == ISA_AARCH64) ? "aarch64-linux-gnu-as" :
                     (tisa == ISA_AARCH32) ? "arm-linux-gnueabihf-as" :
                                             "riscv64-linux-gnu-as";
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
    /* Check for `zerocc --ar` or `zerocc -ar` or `zerocc-ar` tool invocation */
    if (argc >= 2 && (strcmp(argv[1], "--ar") == 0 || strcmp(argv[1], "-ar") == 0)) {
        return zc_ar_main(argc - 1, argv + 1);
    }
    if (strstr(argv[0], "zerocc-ar") != NULL) {
        return zc_ar_main(argc, argv);
    }

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
    if (opts.sanitize_leak && !opts.zerocc_link && !opts.emit_object &&
        !opts.emit_asm && !opts.emit_preproc) {
        fprintf(stderr,
                "zerocc: note: -fsanitize=leak is freestanding-only "
                "(--zerocc-link): the host allocator cannot be inspected, "
                "so the atexit leak report is disabled for this build\n");
    }
    int rc;
    if (opts.emit_preproc) {
        rc = compile_preprocess(&opts);
    } else if (opts.emit_object || opts.emit_asm) {
        /* When ZC_TEXT_AOT=1 is set for cross targets, route -c through the text
         * assembler escape hatch. Default is direct raw binary object generation. */
        if (opts.emit_object && getenv("ZC_TEXT_AOT") && parse_target_arch(opts.target_arch) != ISA_X86_64) {
            rc = compile_object_cross(&opts);
        } else {
            rc = compile_object(&opts);
        }
    } else if (opts.run_pe || opts.run_macho) {
        if (opts.input_count == 0) {
            fprintf(stderr, "zerocc: error: no input file specified\n");
            rc = 1;
        } else if (opts.run_pe) {
            rc = zero_cc_run_pe_file(opts.input_files[0], opts.jit_argc, opts.jit_argv);
        } else {
            rc = zero_cc_run_macho_file(opts.input_files[0], opts.jit_argc, opts.jit_argv);
        }
    } else if (opts.run_mode && (opts.is_pe || opts.is_macho)) {
        char tmp_exe[512];
        const char *target_exe = opts.output_file;
        bool is_temp = false;
        if (!target_exe) {
            snprintf(tmp_exe, sizeof(tmp_exe), "/tmp/zerocc_run_%d_%s",
                     getpid(), opts.is_pe ? "bin.exe" : "bin.macho");
            target_exe = tmp_exe;
            opts.output_file = target_exe;
            is_temp = true;
        }
        rc = compile_and_link(&opts);
        if (rc == 0) {
            if (opts.is_pe) {
                rc = zero_cc_run_pe_file(target_exe, opts.jit_argc, opts.jit_argv);
            } else {
                rc = zero_cc_run_macho_file(target_exe, opts.jit_argc, opts.jit_argv);
            }
        }
        if (is_temp) {
            unlink(tmp_exe);
        }
    } else if (opts.run_mode && opts.input_count == 1 &&
               zero_cc_is_foreign_binary(opts.input_files[0])) {
        rc = zero_cc_run_foreign_file(opts.input_files[0], opts.jit_argc, opts.jit_argv);
    } else if (opts.output_file || opts.is_pe || opts.is_macho) {
        /* Generate executable via system linker or PE/Mach-O writer */
        rc = compile_and_link(&opts);
    } else {
        rc = compile_jit(&opts);
    }
    /* strdup'd -I directories (driver_opts.h) */
    for (int d = 0; d < opts.include_count; d++) free(opts.include_dirs[d]);
    return rc;
}
