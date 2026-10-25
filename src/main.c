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

#include "zero_cc/types/global_vars.h"
#include "zero_cc/core/multi_tu.h"
#include "zero_cc/linker/linker.h"

#define VERSION "0.1.0-dev"

#define ZERO_CC_MAX_INPUTS 32

/* Forward declarations */
static ZeroCCISA parse_target_arch(const char *target);
static void zero_cc_init_compiler_with_target(ZeroCCCompiler *comp, ZeroCCISA target_isa);

typedef struct {
    const char *input_files[ZERO_CC_MAX_INPUTS];  /* T1: multiple inputs */
    int input_count;
    const char *output_file;
    int emit_object;      /* -c */
    int emit_asm;         /* -S */
    int emit_preproc;     /* -E */
    int run_mode;         /* --run */
    int verbose;          /* -v */
    int opt_level;        /* 0-3 for -O0 to -O3 */
    int omit_frame_pointer; /* -1 = default (omit at -O1+), 0 = keep, 1 = omit */
    char *include_dirs[32]; /* -I directories (Sprint 41: real search paths) */
    int include_count;
    char *macro_opts[128];   /* -D/-U macro arguments, in command-line order */
    char macro_opt_kind[128];/* 'D' or 'U' per macro_opts entry */
    int macro_opt_count;
    char *force_includes[8]; /* -include <file> */
    int force_include_count;
    int dump_ir;          /* -dump-ir */
    int dump_cfg;         /* -dump-cfg */
    int profile_opts;     /* --profile-opts */
    int link_mode;        /* Link with system linker (default when -o specified) */
    int zerocc_link;      /* --zerocc-link: link with ZeroCC's own linker (T2) */
    int no_zerocc_rt;     /* --no-zerocc-rt: don't auto-link zero_rt.a */
    const char *libs[32]; /* -l<lib> */
    int lib_count;
    const char *target_arch; /* --target */
    int gen_deps;           /* -MMD: generate dependency file */
    const char *dep_file;   /* -MF: dependency output file */
    const char *dep_target; /* -MT: target name in dependency file */
} CompileOptions;

static int compile_object_single(CompileOptions *opts, const char *input, const char *output);

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <source.c> [more.c ...]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <file>       Output file (executable or object with -c)\n");
    fprintf(stderr, "  -c              Compile to object file (.o) only\n");
    fprintf(stderr, "  -S              Emit assembly\n");
    fprintf(stderr, "  -E              Preprocess only, to stdout (or -o file)\n");
    fprintf(stderr, "  -O0             No optimization\n");
    fprintf(stderr, "  -O1             Basic optimization (default)\n");
    fprintf(stderr, "  -O2             Standard optimization\n");
    fprintf(stderr, "  -O3             Aggressive optimization\n");
    fprintf(stderr, "  -O / -Os / -Oz / -Ofast  Aliases: 1 / 2 / 3 / 3\n");
    fprintf(stderr, "  -fomit-frame-pointer    Omit frame pointers for fixed-frame functions (default at -O1+)\n");
    fprintf(stderr, "  -fno-omit-frame-pointer Keep frame pointers (%%rbp/x29/s0) in every non-leaf function\n");
    fprintf(stderr, "  -I<dir>         Add include directory\n");
    fprintf(stderr, "  -isystem <dir>  Add system include directory\n");
    fprintf(stderr, "  -D<macro>       Define macro\n");
    fprintf(stderr, "  -U<macro>       Undefine macro\n");
    fprintf(stderr, "  -include <file> Force-include a file before the main source\n");
    fprintf(stderr, "  -run, --run     Run in JIT mode (default)\n");
    fprintf(stderr, "  -dump-ir        Dump IR before optimization\n");
    fprintf(stderr, "  -v, --verbose   Verbose output\n");
    fprintf(stderr, "  --version       Print version\n");
    fprintf(stderr, "  -h, --help      Show this help\n");
    fprintf(stderr, "  --target <arch> Target architecture (x86-64, aarch64, riscv64)\n");
    fprintf(stderr, "  --zerocc-link  With -o: link with ZeroCC's own linker (T2; x86-64)\n");
    fprintf(stderr, "  -MMD            Generate dependency file\n");
    fprintf(stderr, "  -MF <file>      Write dependency output to <file>\n");
    fprintf(stderr, "  -MT <target>    Set target name in dependency file\n");
    fprintf(stderr, "  (common gcc flags such as -g, -Wall, -fPIC, -pthread, -static,\n");
    fprintf(stderr, "   -std=... are accepted and ignored - see verbose notes)\n");
}

static void print_version(void) {
    printf("ZeroCC C Compiler version %s\n", VERSION);
    printf("Multi-ISA JIT compiler with optimization pipeline\n");
}

static void add_macro_opt(CompileOptions *opts, char kind, const char *name) {
    if (opts->macro_opt_count < 128) {
        opts->macro_opts[opts->macro_opt_count] = strdup(name);
        opts->macro_opt_kind[opts->macro_opt_count] = kind;
        opts->macro_opt_count++;
    } else {
        fprintf(stderr, "zerocc: warning: more than 128 -D/-U options; ignoring '-%c%s'\n",
                kind, name);
    }
}

static void add_include_dir_opt(CompileOptions *opts, const char *dir) {
    if (opts->include_count < 32) {
        opts->include_dirs[opts->include_count++] = strdup(dir);
    } else {
        fprintf(stderr, "zerocc: warning: more than 32 -I options; ignoring '%s'\n", dir);
    }
}

static int parse_args(int argc, char **argv, CompileOptions *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->opt_level = 1; /* Default -O1 */
    opts->omit_frame_pointer = -1; /* Default: omit for fixed frames at -O1+ */

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
        else if (strcmp(arg, "-E") == 0) {
            opts->emit_preproc = 1;
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
        else if (strcmp(arg, "-O") == 0) {
            opts->opt_level = 1;
        }
        else if (strcmp(arg, "-Os") == 0) {
            opts->opt_level = 2;
        }
        else if (strcmp(arg, "-Oz") == 0) {
            opts->opt_level = 3;
        }
        else if (strcmp(arg, "-Ofast") == 0) {
            opts->opt_level = 3;
        }
        else if (strcmp(arg, "-fomit-frame-pointer") == 0) {
            opts->omit_frame_pointer = 1;
        }
        else if (strcmp(arg, "-fno-omit-frame-pointer") == 0) {
            opts->omit_frame_pointer = 0;
        }
        else if (strcmp(arg, "--target") == 0 && i + 1 < argc) {
            opts->target_arch = argv[++i];
        }
        else if (strcmp(arg, "--zerocc-link") == 0) {
            opts->zerocc_link = 1;
        }
        else if (strcmp(arg, "-MMD") == 0) {
            opts->gen_deps = 1;
        }
        else if (strcmp(arg, "-MF") == 0 && i + 1 < argc) {
            opts->dep_file = argv[++i];
        }
        else if (strncmp(arg, "-MF", 3) == 0) {
            opts->dep_file = arg + 3;
        }
        else if (strcmp(arg, "-MT") == 0 && i + 1 < argc) {
            opts->dep_target = argv[++i];
        }
        else if (strncmp(arg, "-MT", 3) == 0) {
            opts->dep_target = arg + 3;
        }
        else if (strcmp(arg, "-I") == 0 && i + 1 < argc) {
            add_include_dir_opt(opts, argv[++i]);
        }
        else if (strncmp(arg, "-I", 2) == 0 && arg[2]) {
            add_include_dir_opt(opts, arg + 2);
        }
        else if (strcmp(arg, "-I") == 0) {
            fprintf(stderr, "zerocc: error: '-I' requires a directory argument\n");
            return 1;
        }
        else if (strcmp(arg, "-isystem") == 0 && i + 1 < argc) {
            /* zerocc does not yet distinguish system headers in diagnostics;
             * -isystem behaves like -I. */
            add_include_dir_opt(opts, argv[++i]);
        }
        else if (strncmp(arg, "-isystem", 8) == 0 && arg[8]) {
            add_include_dir_opt(opts, arg + 8);
        }
        else if (strcmp(arg, "-D") == 0 && i + 1 < argc) {
            add_macro_opt(opts, 'D', argv[++i]);
        }
        else if (strncmp(arg, "-D", 2) == 0 && arg[2]) {
            add_macro_opt(opts, 'D', arg + 2);
        }
        else if (strcmp(arg, "-D") == 0) {
            fprintf(stderr, "zerocc: error: '-D' requires a macro argument\n");
            return 1;
        }
        else if (strcmp(arg, "-U") == 0 && i + 1 < argc) {
            add_macro_opt(opts, 'U', argv[++i]);
        }
        else if (strncmp(arg, "-U", 2) == 0 && arg[2]) {
            add_macro_opt(opts, 'U', arg + 2);
        }
        else if (strcmp(arg, "-U") == 0) {
            fprintf(stderr, "zerocc: error: '-U' requires a macro argument\n");
            return 1;
        }
        else if (strcmp(arg, "-include") == 0 && i + 1 < argc) {
            if (opts->force_include_count < 8) {
                opts->force_includes[opts->force_include_count++] = strdup(argv[++i]);
            }
        }
        else if (strncmp(arg, "-include", 8) == 0 && arg[8]) {
            if (opts->force_include_count < 8) {
                opts->force_includes[opts->force_include_count++] = strdup(arg + 8);
            }
        }
        else if (strncmp(arg, "-l", 2) == 0) {
            if (opts->lib_count < 32) {
                opts->libs[opts->lib_count++] = arg;
            }
        }
        else if (strcmp(arg, "--no-zerocc-rt") == 0) {
            opts->no_zerocc_rt = 1;
        }
        else if (strcmp(arg, "-dump-ir") == 0) {
            opts->dump_ir = 1;
        }
        else if (strcmp(arg, "-dump-cfg") == 0) {
            opts->dump_cfg = 1;
        }
        else if (strcmp(arg, "--profile-opts") == 0) {
            opts->profile_opts = 1;
        }
        else if (strcmp(arg, "-x") == 0 && i + 1 < argc) {
            const char *lang = argv[++i];
            if (strcmp(lang, "c") != 0 && strcmp(lang, "assembly") != 0 && strcmp(lang, "none") != 0) {
                fprintf(stderr, "zerocc: warning: ignoring unsupported language '%s'\n", lang);
            }
        }
        /* Build-system compatibility (Sprint 52): accept-and-ignore the
         * ubiquitous gcc flags so stock Makefile/CMake invocations work.
         * -v notes each ignored flag. */
        else if (arg[1] == 'g') {
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (DWARF debug info not implemented yet)\n", arg);
        }
        else if (arg[1] == 'W' && arg[2]) {
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (warning controls not implemented yet)\n", arg);
        }
        else if (arg[1] == 'f' && arg[2]) {
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (flag not implemented yet)\n", arg);
        }
        else if (arg[1] == 'm' && arg[2]) {
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (machine tuning not implemented yet)\n", arg);
        }
        else if (strncmp(arg, "-std=", 5) == 0 || strncmp(arg, "--std=", 6) == 0) {
            const char *v = strncmp(arg, "--std=", 6) == 0 ? arg + 6 : arg + 5;
            if (strcmp(v, "c99") != 0 && strcmp(v, "gnu99") != 0 && strcmp(v, "c11") != 0 &&
                strcmp(v, "gnu11") != 0 && strcmp(v, "c23") != 0 && strcmp(v, "gnu23") != 0 &&
                strcmp(v, "c89") != 0 && strcmp(v, "gnu89") != 0) {
                fprintf(stderr, "zerocc: warning: unknown standard '%s' (targeting C99/C11)\n", v);
            }
        }
        else if (strcmp(arg, "-pthread") == 0 || strcmp(arg, "-pipe") == 0 ||
                 strcmp(arg, "-static") == 0 || strcmp(arg, "-shared") == 0 ||
                 strcmp(arg, "-pie") == 0 || strcmp(arg, "-no-pie") == 0) {
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (not applicable to zerocc's static output)\n", arg);
        }
        else if (arg[0] != '-') {
            if (opts->input_count < ZERO_CC_MAX_INPUTS) {
                opts->input_files[opts->input_count++] = arg;
            } else {
                fprintf(stderr, "zerocc: error: too many input files (max %d)\n", ZERO_CC_MAX_INPUTS);
                print_usage(argv[0]);
                return -1;
            }
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return -1;
        }
    }

    if (opts->input_count == 0) {
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

static void emit_asm_data_sections(ZeroCCCompiler *comp) {
    if (!comp->asm_out) return;

    /* Emit .rodata if present */
    if (comp->rodata_section.length > 0) {
        fprintf(comp->asm_out, "\t.section\t.rodata\n");
        if (comp->isa == ISA_AARCH64) {
            fprintf(comp->asm_out, "\t.align\t3\n");
        } else {
            fprintf(comp->asm_out, "\t.p2align\t3\n");
        }
        fprintf(comp->asm_out, ".Lrodata:\n");
        const uint8_t *ro = (const uint8_t *)comp->rodata_section.data;
        for (size_t i = 0; i < comp->rodata_section.length; i++) {
            fprintf(comp->asm_out, "\t.byte\t0x%02x\n", ro[i]);
        }
    }

    /* Emit .data / global variables if present */
    if (global_var_count > 0 || comp->data_section.length > 0) {
        fprintf(comp->asm_out, "\t.section\t.data\n");
        const uint8_t *data_buf = (const uint8_t *)comp->data_section.data;
        size_t data_len = comp->data_section.length;
        size_t cur_offset = 0;

        for (size_t i = 0; i < global_var_count; i++) {
            ZeroCCGlobalVar *gv = &global_vars[i];
            if (!gv->name[0]) continue;
            /* TLS variables live in .tdata/.tbss (emitted below), not .data */
            if (gv->is_tls) continue;

            /* Fill any alignment padding gap before this variable */
            if (gv->data_offset > cur_offset && cur_offset < data_len) {
                size_t pad = (gv->data_offset <= data_len) ? (gv->data_offset - cur_offset) : (data_len - cur_offset);
                for (size_t b = 0; b < pad; b++) {
                    fprintf(comp->asm_out, "\t.byte\t0x%02x\n", data_buf[cur_offset + b]);
                }
                cur_offset += pad;
            }

            /* Alignment directive */
            size_t align = gv->type.align ? gv->type.align : 8;
            if (comp->isa == ISA_AARCH64) {
                int p2 = 0;
                while ((1ULL << p2) < align) p2++;
                fprintf(comp->asm_out, "\t.align\t%d\n", p2);
            } else {
                int p2 = 0;
                while ((1ULL << p2) < align) p2++;
                fprintf(comp->asm_out, "\t.p2align\t%d\n", p2);
            }

            /* Visibility / symbol label */
            if (!gv->is_static) {
                fprintf(comp->asm_out, "\t.globl\t%s\n", gv->name);
            }
            fprintf(comp->asm_out, "%s:\n", gv->name);

            /* Emit bytes / relocations */
            size_t var_end = gv->data_offset + gv->size;
            size_t byte_pos = gv->data_offset;

            while (byte_pos < var_end) {
                /* Check if there is a 64-bit pointer relocation at byte_pos */
                ZeroCCDataReloc *matching_reloc = NULL;
                for (size_t r = 0; r < comp->data_reloc_count; r++) {
                    if (comp->data_relocs[r].data_patch_offset == byte_pos) {
                        matching_reloc = &comp->data_relocs[r];
                        break;
                    }
                }

                if (matching_reloc && byte_pos + 8 <= var_end) {
                    if (matching_reloc->target_kind == 0) {
                        fprintf(comp->asm_out, "\t.quad\t.Lrodata+%zu\n", matching_reloc->rodata_offset);
                    } else if (matching_reloc->target_kind == 1 &&
                               matching_reloc->target_global_idx >= 0 &&
                               matching_reloc->target_global_idx < (int)global_var_count) {
                        const char *tgt_name = global_vars[matching_reloc->target_global_idx].name;
                        fprintf(comp->asm_out, "\t.quad\t%s\n", tgt_name);
                    } else {
                        fprintf(comp->asm_out, "\t.quad\t0\n");
                    }
                    byte_pos += 8;
                } else if (byte_pos < data_len) {
                    fprintf(comp->asm_out, "\t.byte\t0x%02x\n", data_buf[byte_pos]);
                    byte_pos++;
                } else {
                    fprintf(comp->asm_out, "\t.byte\t0x00\n");
                    byte_pos++;
                }
            }
            cur_offset = var_end;
        }

        /* Emit any remaining trailing data bytes */
        if (cur_offset < data_len) {
            for (size_t b = cur_offset; b < data_len; b++) {
                fprintf(comp->asm_out, "\t.byte\t0x%02x\n", data_buf[b]);
            }
        }
    }

    /* Emit the static TLS block (initial-exec, x86-64): .tdata holds the
     * initialized variables (packed image in tls_data_section), .tbss the
     * uninitialized ones.  In-block offsets come from the TLS backend
     * (tls_backend.c); the code references them via name@tpoff. */
    if (comp->tls_data_section.length > 0 || comp->tls_bss_size > 0) {
        const uint8_t *tdata = (const uint8_t *)comp->tls_data_section.data;
        size_t tdata_len = comp->tls_data_section.length;
        size_t emitted = 0;
        fprintf(comp->asm_out, "\t.section\t.tdata,\"awT\",@progbits\n");
        for (size_t i = 0; i < global_var_count; i++) {
            ZeroCCGlobalVar *gv = &global_vars[i];
            if (!gv->is_tls || !gv->name[0]) continue;
            if (gv->data_offset >= tdata_len) continue;
            if (gv->data_offset > emitted) {
                for (size_t b = emitted; b < gv->data_offset && b < tdata_len; b++) {
                    fprintf(comp->asm_out, "\t.byte\t0x%02x\n", tdata[b]);
                }
                emitted = gv->data_offset;
            }
            size_t talign = gv->type.align ? gv->type.align : 8;
            int tp2 = 0;
            while ((1u << tp2) < talign) tp2++;
            fprintf(comp->asm_out, "\t.p2align\t%d\n", tp2);
            if (!gv->is_static) {
                fprintf(comp->asm_out, "\t.globl\t%s\n", gv->name);
            }
            fprintf(comp->asm_out, "%s:\n", gv->name);
            for (size_t b = 0; b < gv->size && emitted + b < tdata_len; b++) {
                fprintf(comp->asm_out, "\t.byte\t0x%02x\n", tdata[emitted + b]);
            }
            emitted += gv->size;
        }
        if (comp->tls_bss_size > 0) {
            size_t tbss_off = 0;
            fprintf(comp->asm_out, "\t.section\t.tbss,\"aw\",@nobits\n");
            for (size_t i = 0; i < global_var_count; i++) {
                ZeroCCGlobalVar *gv = &global_vars[i];
                if (!gv->is_tls || !gv->name[0]) continue;
                if (gv->data_offset < tdata_len) continue;
                size_t boff = gv->data_offset - tdata_len;
                if (boff > tbss_off) {
                    fprintf(comp->asm_out, "\t.space\t%zu\n", boff - tbss_off);
                    tbss_off = boff;
                }
                size_t talign = gv->type.align ? gv->type.align : 8;
                int tp2 = 0;
                while ((1u << tp2) < talign) tp2++;
                fprintf(comp->asm_out, "\t.p2align\t%d\n", tp2);
                if (!gv->is_static) {
                    fprintf(comp->asm_out, "\t.globl\t%s\n", gv->name);
                }
                fprintf(comp->asm_out, "%s:\n", gv->name);
                fprintf(comp->asm_out, "\t.space\t%zu\n", gv->size);
                tbss_off += gv->size;
            }
        }
    }

    fprintf(comp->asm_out, "\t.text\n");
    fclose(comp->asm_out);
    comp->asm_out = NULL;
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
        ZeroCCGlobalVar *saved_globals = NULL;
        for (int i = 1; i < opts->input_count; i++) {
            size_t saved_count = global_var_count;
            if (saved_count > 0) {
                free(saved_globals);
                saved_globals = malloc(saved_count * sizeof(ZeroCCGlobalVar));
                if (saved_globals)
                    memcpy(saved_globals, global_vars, saved_count * sizeof(ZeroCCGlobalVar));
            } else {
                saved_globals = NULL;
            }

            ZeroCCCompiler *tu = jit_compile_tu(opts, opts->input_files[i]);
            if (!tu) {
                free(saved_globals);
                zero_cc_compiler_free(comp);
                return 1;
            }

            /* The registry now holds exactly TU_i's globals (offsets into
             * its own .data). Re-base them by the data base the merge will
             * use, then restore the earlier TUs' entries. */
            size_t data_base = zero_cc_align_up(comp->data_section.length, 8);
            for (size_t g = 0; g < global_var_count; g++) {
                global_vars[g].data_offset += data_base;
            }
            if (saved_globals && zero_cc_global_vars_reserve(saved_count) == 0) {
                memcpy(&global_vars[global_var_count], saved_globals,
                       saved_count * sizeof(ZeroCCGlobalVar));
                global_var_count += saved_count;
            }
            free(saved_globals);
            saved_globals = NULL;

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

    /* Add .data section */
    if (comp->data_section.length > 0) {
        elf_writer_add_section(&elf_ctx, ".data",
                               (uint8_t*)comp->data_section.data,
                               comp->data_section.length,
                               ELF_SHT_PROGBITS,
                               ELF_SHF_ALLOC | ELF_SHF_WRITE,
                               8);
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

    /* Resolve real section indices: empty sections are not emitted, so the
     * positions of later sections shift (a no-.text object has .data at 1). */
    uint16_t text_idx = elf_writer_section_index(&elf_ctx, ".text");
    uint16_t rodata_idx = elf_writer_section_index(&elf_ctx, ".rodata");
    uint16_t data_idx = elf_writer_section_index(&elf_ctx, ".data");
    uint16_t tdata_idx = elf_writer_section_index(&elf_ctx, ".tdata");
    uint16_t tbss_idx = elf_writer_section_index(&elf_ctx, ".tbss");

    /* Add .rodata section symbol first (needed for string literal relocations) */
    /* Section symbols must come before global symbols for correct sh_info calculation */
    if (comp->rodata_section.length > 0) {
        elf_writer_add_symbol(&elf_ctx, ".rodata",
                              ELF_STB_LOCAL, ELF_STT_SECTION,
                              rodata_idx,  /* .rodata section index */
                              0, 0);
    }

    /* Add symbols for functions */
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *fn = &comp->functions[i];
        if (fn->name[0]) {
            if (fn->is_external) {
                bool referenced = false;
                for (size_t r = 0; r < comp->reloc_count; r++) {
                    if (strcmp(comp->relocs[r].target_symbol, fn->name) == 0) {
                        referenced = true;
                        break;
                    }
                }
                if (!referenced) continue;
            }

            /* Estimate function size (simplified - in reality would need more analysis) */
            size_t fn_size = 0;  /* Could be computed from next function's offset */
            int section_idx = SHN_UNDEF;  // Default to undefined
            if (!fn->is_prototype && !fn->is_external) {
                section_idx = text_idx;  // Compiled functions go in .text
            }
            int binding = fn->is_static ? ELF_STB_LOCAL : ELF_STB_GLOBAL;
            int sym_type = fn->is_external ? ELF_STT_NOTYPE : ELF_STT_FUNC;
            elf_writer_add_symbol(&elf_ctx, fn->name,
                                  binding, sym_type,
                                  section_idx,
                                  fn->text_offset,
                                  fn_size);
        }
    }

    /* Add symbols for global variables (.data section) */
    int data_section_idx = data_idx;
    for (size_t i = 0; i < global_var_count; i++) {
        ZeroCCGlobalVar *gv = &global_vars[i];
        if (gv->name[0]) {
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
            elf_writer_add_symbol(&elf_ctx, gv->name,
                                  binding, ELF_STT_OBJECT,
                                  data_section_idx,
                                  gv->data_offset,
                                  gv->size);
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
                /* .rodata + rodata_offset: target the .rodata section symbol (idx 1) */
                if (comp->rodata_section.length > 0) {
                    elf_writer_add_relocation(&elf_ctx, data_section_idx, dr->data_patch_offset,
                                              1, reloc_type_data_abs, (int64_t)dr->rodata_offset);
                }
            } else if (dr->target_kind == 1 && dr->target_global_idx >= 0 &&
                       dr->target_global_idx < (int)global_var_count) {
                /* &global: target the global variable's own symbol (value = data_offset) */
                const char *tname = global_vars[dr->target_global_idx].name;
                int sym_idx = 0;
                for (int j = 0; j < elf_ctx.symbol_count; j++) {
                    if (strcmp(elf_ctx.strtab + elf_ctx.symbols[j].name_offset, tname) == 0) { sym_idx = j + 1; break; }
                }
                if (sym_idx > 0) {
                    elf_writer_add_relocation(&elf_ctx, data_section_idx, dr->data_patch_offset,
                                              sym_idx, reloc_type_data_abs, 0);
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

    /* T2: ZeroCC's own linker (x86-64 only for now; zero_rt Phase 0 is its
     * first customer). */
    if (opts->zerocc_link) {
        if (target_isa != ISA_X86_64) {
            fprintf(stderr,
                    "Error: --zerocc-link currently links x86-64 objects "
                    "only\n");
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

        /* zero_rt auto-link: with --zerocc-link, the default runtime is the
         * ZeroCC-owned zero_rt (build/lib/zero_rt.a next to this binary, or
         * $ZERO_RT_A). Disable with --no-zerocc-rt. */
        char rt_path[2048] = "";
        const char *rt_env = getenv("ZERO_RT_A");
        if (rt_env && rt_env[0]) {
            snprintf(rt_path, sizeof rt_path, "%s", rt_env);
        } else {
            char self[512];
            ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
            if (n > 0) {
                self[n] = 0;
                char *slash = strrchr(self, '/');
                if (slash) {
                    *slash = '\0';
                    snprintf(rt_path, sizeof rt_path, "%s/../lib/zero_rt.a",
                             self);
                }
            }
        }
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
        snprintf(cmd, sizeof(cmd),
                 "%s %s %s -o %s %s -lc%s 2>&1",
                 linker, abi_flags, static_flag, output, all_objs, lib_args);
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
