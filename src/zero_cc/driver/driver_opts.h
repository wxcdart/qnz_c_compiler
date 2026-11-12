#ifndef ZERO_CC_DRIVER_DRIVER_OPTS_H
#define ZERO_CC_DRIVER_DRIVER_OPTS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zero_cc/types/types.h"

#define VERSION "0.1.0-dev"
#define ZERO_CC_MAX_INPUTS 32

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
    char *no_builtin[16]; /* -fno-builtin-<name> function names */
    int no_builtin_count;
    int no_builtin_all;   /* -fno-builtin (all) */
    int profile_opts;     /* --profile-opts */
    int sanitize_overflow; /* -fsanitize=undefined */
    int sanitize_report;   /* -fsanitize-trap: print a UBSan-style message before each trap */
    int sanitize_leak;     /* -fsanitize=leak: freestanding atexit leak report (--zerocc-link) */
    ZeroCCStdDialect std_dialect;   /* -std= dialect (default ZC_STD_GNU11) */
    int link_mode;        /* Link with system linker (default when -o specified) */
    int zerocc_link;      /* --zerocc-link: link with ZeroCC's own linker (T2) */
    int zerocc_link_pie;  /* --zerocc-link-pie: static-PIE variant (x86-64) */
    int no_zerocc_rt;     /* --no-zerocc-rt: don't auto-link zero_rt.a */
    const char *libs[32]; /* -l<lib> */
    int lib_count;
    const char *target_arch; /* --target */
    int gen_deps;           /* -MMD: generate dependency file */
    const char *dep_file;   /* -MF: dependency output file */
    const char *dep_target; /* -MT: target name in dependency file */
    int stack_protector;    /* -fstack-protector[-all|-strong]: 0=off, 1=basic, 2=all, 3=strong */
} CompileOptions;

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
    fprintf(stderr, "  -fstack-protector       Emit stack canaries for functions with >=8B arrays or VLAs\n");
    fprintf(stderr, "  -fstack-protector-all   Emit stack canaries for all functions\n");
    fprintf(stderr, "  -fstack-protector-strong Emit stack canaries for arrays, VLAs, or address-taken locals\n");
    fprintf(stderr, "  -fno-stack-protector    Disable stack canaries (default)\n");
    fprintf(stderr, "  -fsanitize=undefined    zero_san: trap on signed overflow, div/rem by zero, bad shift count\n");
    fprintf(stderr, "  -fsanitize-trap         zero_san: same checks + a UBSan-style message before the trap\n");
    fprintf(stderr, "  -fsanitize=leak         zero_san: freestanding atexit leak report (--zerocc-link only; exit 23 on leak)\n");
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
    fprintf(stderr, "  --target <arch> Target architecture (x86-64, aarch64, riscv64, riscv32)\n");
    fprintf(stderr, "  --zerocc-link  With -o: link with ZeroCC's own linker (T2; all 4 target ISAs)\n");
    fprintf(stderr, "  --ar [options]  Archive tool (create/update static archive, e.g. --ar rcs lib.a obj...)\n");
    fprintf(stderr, "  --zerocc-link-pie  Static-PIE (ET_DYN, self-relocating _start) variant of --zerocc-link (x86-64)\n");
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
    opts->std_dialect = ZC_STD_GNU11; /* Default: gnu11 (C11 + GNU extensions) */

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
        else if (strcmp(arg, "-fstack-protector-all") == 0) {
            opts->stack_protector = 2;
        }
        else if (strcmp(arg, "-fstack-protector-strong") == 0) {
            opts->stack_protector = 3;
        }
        else if (strcmp(arg, "-fstack-protector") == 0) {
            opts->stack_protector = 1;
        }
        else if (strcmp(arg, "-fno-stack-protector") == 0) {
            opts->stack_protector = 0;
        }
        else if (strcmp(arg, "-fno-builtin") == 0) {
            /* GCC: disable all builtin function recognition - the
             * abs/labs lowering (c-torture execute/builtins) consults it. */
            opts->no_builtin_all = 1;
        }
        else if (strcmp(arg, "-fsanitize=undefined") == 0) {
            opts->sanitize_overflow = 1;
        }
        else if (strcmp(arg, "-fsanitize-trap") == 0) {
            /* GCC/Clang-compatible: enable the zero_san checks in trap mode
             * with a UBSan-style report before the trap (Sprint 167). */
            opts->sanitize_overflow = 1;
            opts->sanitize_report = 1;
        }
        else if (strcmp(arg, "-fno-sanitize-trap") == 0) {
            opts->sanitize_report = 0;
        }
        else if (strcmp(arg, "-fsanitize=leak") == 0) {
            /* Sprint 169: freestanding atexit leak report. The zero_rt heap's
             * own bookkeeping is the detector; --zerocc-link routes the stub
             * through zeroc_leak_main_wrapper, which enables it. */
            opts->sanitize_leak = 1;
        }
        else if (strcmp(arg, "-fno-sanitize=leak") == 0) {
            opts->sanitize_leak = 0;
        }
        else if (strncmp(arg, "-fno-builtin-", 13) == 0 && arg[13]) {
            if (opts->no_builtin_count < 16) {
                opts->no_builtin[opts->no_builtin_count++] = strdup(arg + 13);
            }
        }
        else if (strcmp(arg, "--target") == 0 && i + 1 < argc) {
            opts->target_arch = argv[++i];
        }
        else if (strcmp(arg, "--zerocc-link") == 0) {
            opts->zerocc_link = 1;
        }
        else if (strcmp(arg, "--zerocc-link-pie") == 0) {
            opts->zerocc_link = 1;
            opts->zerocc_link_pie = 1;
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
         * -v notes each ignored flag. NOTE: every branch below is guarded
         * by arg[0] == '-' - otherwise an input file whose second letter
         * is g/W/f/m (e.g. "hm.c") would be swallowed as a flag. */
        else if (arg[0] == '-' && arg[1] == 'g') {
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (DWARF debug info not implemented yet)\n", arg);
        }
        else if (arg[0] == '-' && arg[1] == 'W' && arg[2]) {
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (warning controls not implemented yet)\n", arg);
        }
        else if (arg[0] == '-' && arg[1] == 'f' && arg[2]) {
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (flag not implemented yet)\n", arg);
        }
        else if (arg[0] == '-' && arg[1] == 'm' && arg[2]) {
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (machine tuning not implemented yet)\n", arg);
        }
        else if (strncmp(arg, "-std=", 5) == 0 || strncmp(arg, "--std=", 6) == 0) {
            const char *v = strncmp(arg, "--std=", 6) == 0 ? arg + 6 : arg + 5;
            if (strcmp(v, "c99") == 0) {
                opts->std_dialect = ZC_STD_C99;
            } else if (strcmp(v, "gnu99") == 0) {
                opts->std_dialect = ZC_STD_GNU99;
            } else if (strcmp(v, "c11") == 0 || strcmp(v, "c17") == 0) {
                opts->std_dialect = ZC_STD_C11;
            } else if (strcmp(v, "gnu11") == 0 || strcmp(v, "gnu17") == 0) {
                opts->std_dialect = ZC_STD_GNU11;
            } else if (strcmp(v, "c23") == 0 || strcmp(v, "c2y") == 0) {
                opts->std_dialect = ZC_STD_C23;
            } else if (strcmp(v, "gnu23") == 0 || strcmp(v, "gnu2y") == 0) {
                opts->std_dialect = ZC_STD_GNU23;
            } else if (strcmp(v, "c89") == 0) {
                opts->std_dialect = ZC_STD_C99; /* C89 leniency on top of C99 */
            } else if (strcmp(v, "gnu89") == 0) {
                opts->std_dialect = ZC_STD_GNU99;
            } else {
                fprintf(stderr, "zerocc: warning: unknown standard '%s' (targeting C99/C11)\n", v);
            }
        }
        else if (strcmp(arg, "-pthread") == 0 || strcmp(arg, "-pipe") == 0 ||
                 strcmp(arg, "-static") == 0 || strcmp(arg, "-shared") == 0 ||
                 strcmp(arg, "-pie") == 0 || strcmp(arg, "-no-pie") == 0) {
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (not applicable to zerocc's static output)\n", arg);
        }
        else if (strcmp(arg, "-w") == 0 || strcmp(arg, "-Wno-pragma-float") == 0) {
            /* Warning controls (Sprint 70): -w suppresses all warnings -
             * zerocc's diagnostics are error-shaped and not gated on a
             * warning level, so accepting it is a no-op. */
            if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (warning control)\n", arg);
        }
        else if (strncmp(arg, "--param", 7) == 0) {
            /* Tuning knobs (Sprint 70): --param=NAME=VALUE or
             * --param NAME VALUE - both forms ignored. */
            if (arg[7] == '=') {
                if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '%s' (tuning knob)\n", arg);
            } else {
                /* Consume the value argument too. */
                if (i + 1 < argc) i++;
                if (opts->verbose) fprintf(stderr, "zerocc: note: ignoring '--param' (tuning knob)\n");
            }
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


#endif /* ZERO_CC_DRIVER_DRIVER_OPTS_H */
