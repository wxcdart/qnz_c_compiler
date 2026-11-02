#ifndef ZERO_CC_CORE_PREPROCESSOR_H
#define ZERO_CC_CORE_PREPROCESSOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* Global pragma state - defined in pragma_state.c */
extern int zero_cc_global_pack;
extern struct {
    bool warning_as_error;
    char *disabled_warnings[64];
    int disabled_warning_count;
} zero_cc_global_diag_state;

/* Forward declaration: this header's functions take `ZeroCCCompiler *`
 * (member access like c->diag_buf) but are typically included before
 * zero_cc/types/symbols.h defines the struct.  The forward typedef makes
 * the parameter type a pointer-to-incomplete-struct instead of an
 * unknown-identifier fallback; semantic analysis resolves members against
 * the completed registry. */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Get the pack value after preprocessing */
static inline int zero_cc_get_pack_value(void) {
    return zero_cc_global_pack;
}

/* Set the pack value (for use by tokenizer) */
static inline void zero_cc_set_pack_value(int pack) {
    zero_cc_global_pack = pack;
}

#include "zero_cc/lexer/scan.h"


#include "zero_cc/core/compiler_state.h"
#include "zero_cc/core/buffers.h"

typedef struct {
    char *name;
    char *value;
} ZeroCCMacroDef;

typedef struct {
    ZeroCCMacroDef defs[128];
    int undef[128]; /* 1 = -U entry (name only, undefine), 0 = -D entry */
    size_t count;
} ZeroCCDefines;

/* Preprocessor lifecycle and configuration */
void zero_cc_cpp_init(ZeroCCPreprocessor *cpp);
void zero_cc_cpp_destroy(ZeroCCPreprocessor *cpp);
void zero_cc_cpp_add_include_dir(ZeroCCPreprocessor *cpp, const char *path);
ZeroCCMacro* zero_cc_cpp_find_macro(ZeroCCPreprocessor *cpp, const char *name);
void zero_cc_cpp_define_obj(ZeroCCPreprocessor *cpp, const char *name, const char *value);
void zero_cc_cpp_undefine(ZeroCCPreprocessor *cpp, const char *name);
void zero_cc_cpp_define_func1(ZeroCCPreprocessor *cpp, const char *name, const char *p1, const char *value);
void zero_cc_cpp_define_func2(ZeroCCPreprocessor *cpp, const char *name, const char *p1, const char *p2, const char *value);
void zero_cc_cpp_define_func3(ZeroCCPreprocessor *cpp, const char *name, const char *p1, const char *p2, const char *p3, const char *value);
void zero_cc_cpp_define_builtin_macros(ZeroCCPreprocessor *cpp);
void zero_cc_cpp_undef(ZeroCCPreprocessor *cpp, const char *name);
void zero_cc_cpp_restore_macro(ZeroCCPreprocessor *cpp, const ZeroCCMacro *src);
void zero_cc_cpp_push_macro(ZeroCCPreprocessor *cpp, const char *name);
void zero_cc_cpp_pop_macro(ZeroCCPreprocessor *cpp, const char *name);
char* zero_cc_read_file(const char *filepath);
char* zero_cc_cpp_resolve_include(ZeroCCPreprocessor *cpp, const char *filename, bool is_system,
                                  char *out_path, size_t out_cap);
int zero_cc_cpp_push_include_dir(ZeroCCPreprocessor *cpp, const char *resolved_path);
void zero_cc_cpp_pop_include_dir(ZeroCCPreprocessor *cpp, int saved_depth);
void zero_cc_cpp_add_dependency(ZeroCCPreprocessor *cpp, const char *path);
bool zero_cc_cpp_write_dep_file(ZeroCCPreprocessor *cpp, const char *dep_file);
bool zero_cc_cpp_handle_pragma(ZeroCCPreprocessor *cpp, const char *text, size_t len);
void zero_cc_cpp_publish_diag_state(ZeroCCPreprocessor *cpp);
int64_t zero_cc_cpp_eval_expr(ZeroCCPreprocessor *cpp, const char *expr);
void zero_cc_cpp_process_text(ZeroCCPreprocessor *cpp, const char *src, ZeroCCBuffer *out);

/* Top-level preprocessing entry points */
char* zero_cc_preprocess_ext3(const char *source, ZeroCCIncludeDirs *inc_dirs,
                              ZeroCCDefines *defines, const char *main_file,
                              char *err_out, size_t err_cap,
                              const char *dep_target, const char *dep_file,
                              int gen_deps,
                              const char **force_includes,
                              int force_include_count);
char* zero_cc_preprocess_ext2(const char *source, ZeroCCIncludeDirs *inc_dirs,
                              ZeroCCDefines *defines, const char *main_file,
                              char *err_out, size_t err_cap);
char* zero_cc_preprocess_ext(const char *source, ZeroCCIncludeDirs *inc_dirs, ZeroCCDefines *defines);
char* zero_cc_preprocess(const char *source, ZeroCCIncludeDirs *inc_dirs);

#endif /* ZERO_CC_CORE_PREPROCESSOR_H */
