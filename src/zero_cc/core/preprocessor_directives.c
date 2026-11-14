/* Preprocessor directive & resource helpers: #include resolution, #embed
 * binary resources, dependency file emission (-MMD/-MF/-MT), #pragma
 * handling, #if/#elif expression evaluation, and #define value capture. */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zero_cc/core/preprocessor.h"
#include "zero_cc/core/utils.h"
#include "zero_cc/core/buffers.h"
#include "zero_cc/core/compiler_state.h"
#include "zero_cc/core/cpp_expr.h"
#include "zero_cc/core/virtual_headers.h"
#include "zero_cc/lexer/scan.h"

char* zero_cc_read_file(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read_bytes = fread(buf, 1, sz, f);
    buf[read_bytes] = '\0';
    fclose(f);
    return buf;
}

char* zero_cc_cpp_resolve_include(ZeroCCPreprocessor *cpp, const char *filename, bool is_system,
                                  char *out_path, size_t out_cap) {
    char path[PATH_MAX + 64];
    /* 1. First check virtual freestanding system headers */
    char *vhdr = zero_cc_cpp_get_virtual_header(filename, cpp->std_dialect);
    if (vhdr) {
        if (out_path && out_cap) out_path[0] = '\0'; /* virtual: no file directory */
        return vhdr;
    }

    /* 2. Quoted includes: directory of the including file first (C99
     *    6.10.2p2), nearest real-file directory on the stack first, then
     *    the working directory (top-level source siblings). */
    if (!is_system) {
        for (int d = cpp->include_dir_depth - 1; d >= 0; d--) {
            snprintf(path, sizeof(path), "%s/%s", cpp->include_file_dirs[d], filename);
            if (access(path, R_OK) == 0) {
                if (out_path && out_cap) snprintf(out_path, out_cap, "%s", path);
                return zero_cc_read_file(path);
            }
        }
        if (access(filename, R_OK) == 0) {
            if (out_path && out_cap) snprintf(out_path, out_cap, "%s", filename);
            return zero_cc_read_file(filename);
        }
    }
    /* 3. Check explicitly added include directories */
    for (size_t i = 0; i < cpp->include_dirs.count; i++) {
        snprintf(path, sizeof(path), "%s/%s", cpp->include_dirs.paths[i], filename);
        if (access(path, R_OK) == 0) {
            if (out_path && out_cap) snprintf(out_path, out_cap, "%s", path);
            return zero_cc_read_file(path);
        }
    }
    /* 4. Host fallback paths if needed */
    static const char *sys_paths[] = {
        "/usr/include",
        "/usr/local/include",
        "/usr/include/x86_64-linux-gnu",
        "/usr/include/aarch64-linux-gnu",
        NULL
    };
    for (int i = 0; sys_paths[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", sys_paths[i], filename);
        if (access(path, R_OK) == 0) {
            if (out_path && out_cap) snprintf(out_path, out_cap, "%s", path);
            return zero_cc_read_file(path);
        }
    }
    if (out_path && out_cap) out_path[0] = '\0';
    return NULL;
}

/* Push the directory of a real include file so nested quoted includes
 * resolve relative to it (C99 6.10.2p2).  Returns the stack depth to
 * restore afterwards. */
int zero_cc_cpp_push_include_dir(ZeroCCPreprocessor *cpp, const char *resolved_path) {
    int saved = cpp->include_dir_depth;
    if (!resolved_path || !resolved_path[0]) return saved;
    if (cpp->include_dir_depth >= 32) return saved;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", resolved_path);
    char *slash = strrchr(tmp, '/');
    if (slash && slash != tmp) {
        *slash = '\0';
        snprintf(cpp->include_file_dirs[cpp->include_dir_depth], PATH_MAX, "%s", tmp);
    } else {
        snprintf(cpp->include_file_dirs[cpp->include_dir_depth], PATH_MAX, ".");
    }
    cpp->include_dir_depth++;
    return saved;
}

void zero_cc_cpp_pop_include_dir(ZeroCCPreprocessor *cpp, int saved_depth) {
    cpp->include_dir_depth = saved_depth;
}

/* Record an included file for dependency generation (-MMD/-MF/-MT) */
void zero_cc_cpp_add_dependency(ZeroCCPreprocessor *cpp, const char *path) {
    if (!path || !path[0]) return;
    for (size_t i = 0; i < cpp->dep_file_count; i++) {
        if (strcmp(cpp->dep_files[i], path) == 0) return;
    }
    if (cpp->dep_file_count >= cpp->dep_file_capacity) {
        size_t new_cap = cpp->dep_file_capacity ? cpp->dep_file_capacity * 2 : 16;
        char **new_files = (char **)realloc(cpp->dep_files, new_cap * sizeof(char *));
        if (!new_files) return;
        cpp->dep_files = new_files;
        cpp->dep_file_capacity = new_cap;
    }
    cpp->dep_files[cpp->dep_file_count] = strdup(path);
    cpp->dep_file_count++;
}

char *zero_cc_cpp_capture_paren_tokens(const char **pp) {
    const char *p = *pp;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') { *pp = p; return NULL; }
    p++;
    const char *start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            depth--;
            if (depth == 0) break;
        }
        p++;
    }
    size_t len = (size_t)(p - start);
    char *res = (char *)malloc(len + 1);
    if (res) {
        memcpy(res, start, len);
        res[len] = '\0';
    }
    if (*p == ')') p++;
    *pp = p;
    return res;
}

static int64_t zero_cc_cpp_eval_c_attribute(const char *name) {
    if (!name || !name[0]) return 0;
    if (strcmp(name, "nodiscard") == 0 || strcmp(name, "__nodiscard__") == 0 ||
        strcmp(name, "maybe_unused") == 0 || strcmp(name, "__maybe_unused__") == 0 ||
        strcmp(name, "deprecated") == 0 || strcmp(name, "__deprecated__") == 0 ||
        strcmp(name, "fallthrough") == 0 || strcmp(name, "__fallthrough__") == 0 ||
        strcmp(name, "reproducible") == 0 || strcmp(name, "__reproducible__") == 0 ||
        strcmp(name, "unsequenced") == 0 || strcmp(name, "__unsequenced__") == 0) {
        return 202311L;
    }
    return 0;
}

bool zero_cc_cpp_is_macro_defined(ZeroCCPreprocessor *cpp, const char *name) {
    if (!name || !name[0]) return false;
    if (strcmp(name, "__has_include") == 0 ||
        strcmp(name, "__has_c_attribute") == 0 ||
        strcmp(name, "__has_embed") == 0) {
        return true;
    }
    return zero_cc_cpp_find_macro(cpp, name) != NULL;
}

unsigned char *zero_cc_cpp_read_binary_resource(ZeroCCPreprocessor *cpp,
                                                const char *asset_path,
                                                bool is_sys,
                                                size_t *out_sz,
                                                char *resolved_out,
                                                size_t resolved_cap) {
    if (!asset_path || !asset_path[0]) return NULL;
    char path[PATH_MAX + 64];
    if (resolved_out && resolved_cap) resolved_out[0] = '\0';

    if (cpp) {
        char *vhdr = zero_cc_cpp_get_virtual_header(asset_path, cpp->std_dialect);
        if (vhdr) {
            *out_sz = strlen(vhdr);
            return (unsigned char *)vhdr;
        }
    }

    #define ZC_TRY_READ(fpath) do { \
        FILE *f = fopen((fpath), "rb"); \
        if (f) { \
            fseek(f, 0, SEEK_END); \
            long sz = ftell(f); \
            fseek(f, 0, SEEK_SET); \
            if (sz >= 0) { \
                unsigned char *data = (unsigned char *)malloc((size_t)sz + 1); \
                if (data) { \
                    size_t r = fread(data, 1, (size_t)sz, f); \
                    data[r] = '\0'; \
                    fclose(f); \
                    *out_sz = r; \
                    if (resolved_out && resolved_cap) { \
                        snprintf(resolved_out, resolved_cap, "%s", (fpath)); \
                    } \
                    return data; \
                } \
            } \
            fclose(f); \
        } \
    } while (0)

    if (!is_sys && cpp) {
        for (int d = cpp->include_dir_depth - 1; d >= 0; d--) {
            snprintf(path, sizeof(path), "%s/%s", cpp->include_file_dirs[d], asset_path);
            if (access(path, R_OK) == 0) {
                ZC_TRY_READ(path);
            }
        }
    }

    if (!is_sys || asset_path[0] == '/') {
        if (access(asset_path, R_OK) == 0) {
            ZC_TRY_READ(asset_path);
        }
    }

    if (cpp) {
        for (size_t i = 0; i < cpp->include_dirs.count; i++) {
            snprintf(path, sizeof(path), "%s/%s", cpp->include_dirs.paths[i], asset_path);
            if (access(path, R_OK) == 0) {
                ZC_TRY_READ(path);
            }
        }
    }

    static const char *sys_paths[] = {
        "/usr/include",
        "/usr/local/include",
        "/usr/include/x86_64-linux-gnu",
        "/usr/include/aarch64-linux-gnu",
        NULL
    };
    for (int i = 0; sys_paths[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", sys_paths[i], asset_path);
        if (access(path, R_OK) == 0) {
            ZC_TRY_READ(path);
        }
    }

    #undef ZC_TRY_READ
    return NULL;
}

/* Write Makefile-style dependency rule (-MMD/-MF/-MT) */
bool zero_cc_cpp_write_dep_file(ZeroCCPreprocessor *cpp, const char *dep_file) {
    if (!cpp || !dep_file || !dep_file[0]) return false;
    const char *target = (cpp->dep_target && cpp->dep_target[0]) ? cpp->dep_target : "a.o";
    FILE *f = fopen(dep_file, "w");
    if (!f) {
        perror(dep_file);
        return false;
    }
    fprintf(f, "%s:", target);
    size_t col = strlen(target) + 1;
    for (size_t i = 0; i < cpp->dep_file_count; i++) {
        size_t len = strlen(cpp->dep_files[i]);
        if (col + len + 1 > 78) {
            fprintf(f, " \\\n ");
            col = 1;
        }
        fprintf(f, " %s", cpp->dep_files[i]);
        col += len + 1;
    }
    fprintf(f, "\n");
    fclose(f);
    return true;
}

/* ---- Pragma handling (message / GCC diagnostic / push_options) ---- */
static const char *zero_cc_pragma_skip_ws(const char *q) {
    while (*q == ' ' || *q == '\t') q++;
    return q;
}

static bool zero_cc_pragma_word(const char **pq, const char *w) {
    size_t n = strlen(w);
    const char *q = *pq;
    if (strncmp(q, w, n) != 0 || isalnum((unsigned char)q[n]) || q[n] == '_') return false;
    *pq = zero_cc_pragma_skip_ws(q + n);
    return true;
}

/* Copy first quoted string in q into out, stripping a leading "-W". */
static bool zero_cc_pragma_quoted(const char *q, char *out, size_t cap, bool strip_w) {
    const char *a = strchr(q, '"');
    if (!a) return false;
    a++;
    const char *b = strchr(a, '"');
    if (!b) return false;
    if (strip_w && strncmp(a, "-W", 2) == 0) a += 2;
    size_t n = (size_t)(b - a);
    if (n >= cap) n = cap - 1;
    memcpy(out, a, n);
    out[n] = '\0';
    return true;
}

static void zero_cc_diag_remove(ZeroCCPreprocessor *cpp, const char *name) {
    for (int i = 0; i < cpp->diagnostic_state.disabled_warning_count; i++) {
        if (cpp->diagnostic_state.disabled_warnings[i] &&
            strcmp(cpp->diagnostic_state.disabled_warnings[i], name) == 0) {
            free(cpp->diagnostic_state.disabled_warnings[i]);
            int last = --cpp->diagnostic_state.disabled_warning_count;
            cpp->diagnostic_state.disabled_warnings[i] = cpp->diagnostic_state.disabled_warnings[last];
            cpp->diagnostic_state.disabled_warnings[last] = NULL;
            return;
        }
    }
}

/* Returns true if the pragma was consumed by the preprocessor. */
bool zero_cc_cpp_handle_pragma(ZeroCCPreprocessor *cpp, const char *text, size_t len) {
    char buf[512];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';
    while (len > 0 && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';
    const char *q = zero_cc_pragma_skip_ws(buf);

    if (zero_cc_pragma_word(&q, "message")) {
        char msg[400];
        if (!zero_cc_pragma_quoted(q, msg, sizeof(msg), false)) msg[0] = '\0';
        fprintf(stderr, "%s:%d: note: #pragma message: %s\n",
                cpp->filename ? cpp->filename : "<stdin>", (int)cpp->line_num, msg);
        return true;
    }
    if (zero_cc_pragma_word(&q, "push_macro")) {
        char name[128];
        if (zero_cc_pragma_quoted(q, name, sizeof(name), false)) {
            zero_cc_cpp_push_macro(cpp, name);
            return true;
        }
    }
    if (zero_cc_pragma_word(&q, "pop_macro")) {
        char name[128];
        if (zero_cc_pragma_quoted(q, name, sizeof(name), false)) {
            zero_cc_cpp_pop_macro(cpp, name);
            return true;
        }
    }
    if (!zero_cc_pragma_word(&q, "GCC")) return false;
    if (zero_cc_pragma_word(&q, "push_options") || zero_cc_pragma_word(&q, "pop_options")) {
        return true; /* no per-region options are tracked yet */
    }
    if (!zero_cc_pragma_word(&q, "diagnostic")) return false;
    typeof(cpp->diagnostic_state) *d = &cpp->diagnostic_state;
    if (zero_cc_pragma_word(&q, "push")) {
        if (d->stack_depth < 16) {
            int k = d->stack_depth++;
            d->warning_as_error_stack[k] = d->warning_as_error;
            d->disabled_warning_count_stack[k] = d->disabled_warning_count;
            for (int i = 0; i < d->disabled_warning_count; i++)
                d->disabled_warnings_stack[k][i] = d->disabled_warnings[i] ? strdup(d->disabled_warnings[i]) : NULL;
        }
        return true;
    }
    if (zero_cc_pragma_word(&q, "pop")) {
        if (d->stack_depth > 0) {
            int k = --d->stack_depth;
            for (int i = 0; i < d->disabled_warning_count; i++) free(d->disabled_warnings[i]);
            d->warning_as_error = d->warning_as_error_stack[k];
            d->disabled_warning_count = d->disabled_warning_count_stack[k];
            for (int i = 0; i < d->disabled_warning_count; i++) {
                d->disabled_warnings[i] = d->disabled_warnings_stack[k][i];
                d->disabled_warnings_stack[k][i] = NULL;
            }
            d->disabled_warning_count_stack[k] = 0;
        }
        return true;
    }
    bool ignored = zero_cc_pragma_word(&q, "ignored");
    bool warning = !ignored && zero_cc_pragma_word(&q, "warning");
    bool error = !ignored && !warning && zero_cc_pragma_word(&q, "error");
    if (ignored || warning || error) {
        char name[128];
        bool is_werror = false;
        if (zero_cc_pragma_quoted(q, name, sizeof(name), true)) {
            is_werror = (strcmp(name, "error") == 0);
            if (ignored) {
                bool present = false;
                for (int i = 0; i < d->disabled_warning_count; i++)
                    if (d->disabled_warnings[i] && strcmp(d->disabled_warnings[i], name) == 0) present = true;
                if (!present && d->disabled_warning_count < 64)
                    d->disabled_warnings[d->disabled_warning_count++] = strdup(name);
            } else if (is_werror) {
                d->warning_as_error = error;
            } else {
                zero_cc_diag_remove(cpp, name); /* warning/error re-enable the diagnostic */
            }
        }
        return true;
    }
    return true; /* unknown GCC pragma: ignore like GCC does */
}

/* Publish the final diagnostic pragma state for the tokenizer/compiler. */
void zero_cc_cpp_publish_diag_state(ZeroCCPreprocessor *cpp) {
    for (int i = 0; i < zero_cc_global_diag_state.disabled_warning_count && i < 64; i++) {
        free(zero_cc_global_diag_state.disabled_warnings[i]);
        zero_cc_global_diag_state.disabled_warnings[i] = NULL;
    }
    zero_cc_global_diag_state.warning_as_error = cpp->diagnostic_state.warning_as_error;
    zero_cc_global_diag_state.disabled_warning_count = cpp->diagnostic_state.disabled_warning_count;
    for (int i = 0; i < cpp->diagnostic_state.disabled_warning_count; i++) {
        zero_cc_global_diag_state.disabled_warnings[i] =
            cpp->diagnostic_state.disabled_warnings[i] ? strdup(cpp->diagnostic_state.disabled_warnings[i]) : NULL;
    }
}

int64_t zero_cc_cpp_eval_expr(ZeroCCPreprocessor *cpp, const char *expr) {
    while (*expr && isspace((unsigned char)*expr) && *expr != '\n') expr++;
    if (!*expr || *expr == '\n') return 0;

    /* Step 1: Extract expression up to newline or line-ending comment */
    ZeroCCBuffer line_buf;
    zero_cc_buf_init(&line_buf, 256);
    const char *p = expr;
    while (*p && *p != '\n') {
        if (*p == '\\' && p[1] == '\n') {
            p += 2;
            continue;
        }
        if (*p == '/' && p[1] == '/') {
            break;
        }
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            zero_cc_buf_putc(&line_buf, ' ');
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            zero_cc_buf_putc(&line_buf, *p++);
            while (*p && *p != quote && *p != '\n') {
                if (*p == '\\' && p[1]) {
                    zero_cc_buf_putc(&line_buf, *p++);
                }
                zero_cc_buf_putc(&line_buf, *p++);
            }
            if (*p == quote) zero_cc_buf_putc(&line_buf, *p++);
            continue;
        }
        zero_cc_buf_putc(&line_buf, *p++);
    }
    zero_cc_buf_putc(&line_buf, '\0');

    /* Step 2: Replace `defined(ID)` / `defined ID` and `__has_include(...)` */
    ZeroCCBuffer def_buf;
    zero_cc_buf_init(&def_buf, line_buf.length ? line_buf.length : 256);
    const char *lp = line_buf.data;
    while (*lp) {
        if (isspace((unsigned char)*lp)) {
            zero_cc_buf_putc(&def_buf, *lp++);
            continue;
        }
        if (*lp == '"' || *lp == '\'') {
            char quote = *lp;
            zero_cc_buf_putc(&def_buf, *lp++);
            while (*lp && *lp != quote) {
                if (*lp == '\\' && lp[1]) {
                    zero_cc_buf_putc(&def_buf, *lp++);
                }
                zero_cc_buf_putc(&def_buf, *lp++);
            }
            if (*lp == quote) zero_cc_buf_putc(&def_buf, *lp++);
            continue;
        }
        if (isalpha((unsigned char)*lp) || *lp == '_') {
            char id[128];
            size_t id_len = 0;
            while (*lp && (isalnum((unsigned char)*lp) || *lp == '_')) {
                if (id_len + 1 < sizeof(id)) id[id_len++] = *lp;
                lp++;
            }
            id[id_len] = '\0';
            if (strcmp(id, "defined") == 0) {
                while (*lp && isspace((unsigned char)*lp)) lp++;
                bool has_paren = (*lp == '(');
                if (has_paren) {
                    lp++;
                    while (*lp && isspace((unsigned char)*lp)) lp++;
                }
                char macro_name[128];
                size_t ml = 0;
                while (*lp && (isalnum((unsigned char)*lp) || *lp == '_')) {
                    if (ml + 1 < sizeof(macro_name)) macro_name[ml++] = *lp;
                    lp++;
                }
                macro_name[ml] = '\0';
                if (has_paren) {
                    while (*lp && isspace((unsigned char)*lp)) lp++;
                    if (*lp == ')') lp++;
                }
                bool is_def = zero_cc_cpp_is_macro_defined(cpp, macro_name);
                zero_cc_buf_putc(&def_buf, is_def ? '1' : '0');
                zero_cc_buf_putc(&def_buf, ' ');
            } else if (strcmp(id, "__has_include") == 0) {
                char *inner = zero_cc_cpp_capture_paren_tokens(&lp);
                bool has_inc = false;
                if (inner) {
                    const char *ip = inner;
                    while (*ip && isspace((unsigned char)*ip)) ip++;
                    char *expanded_inner = NULL;
                    if (*ip != '<' && *ip != '"') {
                        ZeroCCBuffer eb;
                        zero_cc_buf_init(&eb, strlen(ip) * 2 + 32);
                        size_t sl = cpp->line_num;
                        int sd = cpp->cond_depth;
                        cpp->cond_depth = 0;
                        zero_cc_cpp_process_text(cpp, ip, &eb);
                        cpp->cond_depth = sd;
                        cpp->line_num = sl;
                        zero_cc_buf_putc(&eb, '\0');
                        expanded_inner = eb.data;
                        ip = expanded_inner;
                        while (*ip && isspace((unsigned char)*ip)) ip++;
                    }
                    bool is_sys = (*ip == '<');
                    char delim = is_sys ? '>' : '"';
                    if (*ip == '<' || *ip == '"') {
                        ip++;
                        char inc_name[256];
                        size_t inl = 0;
                        while (*ip && *ip != delim && *ip != '\n' && inl + 1 < sizeof(inc_name)) {
                            inc_name[inl++] = *ip++;
                        }
                        inc_name[inl] = '\0';
                        char res_path[PATH_MAX];
                        char *content = zero_cc_cpp_resolve_include(cpp, inc_name, is_sys, res_path, sizeof(res_path));
                        has_inc = (content != NULL);
                        if (content) free(content);
                    }
                    if (expanded_inner) free(expanded_inner);
                    free(inner);
                }
                zero_cc_buf_putc(&def_buf, has_inc ? '1' : '0');
                zero_cc_buf_putc(&def_buf, ' ');
            } else if (strcmp(id, "__has_c_attribute") == 0) {
                while (*lp && isspace((unsigned char)*lp)) lp++;
                if (*lp == '(') lp++;
                while (*lp && isspace((unsigned char)*lp)) lp++;
                char attr_name[128];
                size_t anl = 0;
                while (*lp && (isalnum((unsigned char)*lp) || *lp == '_' || *lp == ':') && anl + 1 < sizeof(attr_name)) {
                    attr_name[anl++] = *lp++;
                }
                attr_name[anl] = '\0';
                while (*lp && isspace((unsigned char)*lp)) lp++;
                if (*lp == '(') {
                    int pdepth = 1; lp++;
                    while (*lp && pdepth > 0) {
                        if (*lp == '(') pdepth++;
                        else if (*lp == ')') pdepth--;
                        lp++;
                    }
                }
                while (*lp && isspace((unsigned char)*lp)) lp++;
                if (*lp == ')') lp++;
                int64_t attr_val = zero_cc_cpp_eval_c_attribute(attr_name);
                char attr_str[32];
                snprintf(attr_str, sizeof(attr_str), "%ld ", (long)attr_val);
                zero_cc_buf_puts(&def_buf, attr_str);
            } else if (strcmp(id, "__has_embed") == 0) {
                char *inner = zero_cc_cpp_capture_paren_tokens(&lp);
                int res_code = 0; /* __STDC_EMBED_NOT_FOUND__ */
                if (inner) {
                    const char *ip = inner;
                    while (*ip && isspace((unsigned char)*ip)) ip++;
                    char *expanded_inner = NULL;
                    if (*ip != '<' && *ip != '"') {
                        ZeroCCBuffer eb;
                        zero_cc_buf_init(&eb, strlen(ip) * 2 + 32);
                        size_t sl = cpp->line_num;
                        int sd = cpp->cond_depth;
                        cpp->cond_depth = 0;
                        zero_cc_cpp_process_text(cpp, ip, &eb);
                        cpp->cond_depth = sd;
                        cpp->line_num = sl;
                        zero_cc_buf_putc(&eb, '\0');
                        expanded_inner = eb.data;
                        ip = expanded_inner;
                        while (*ip && isspace((unsigned char)*ip)) ip++;
                    }
                    bool is_sys = (*ip == '<');
                    char delim = is_sys ? '>' : '"';
                    if (*ip == '<' || *ip == '"') {
                        ip++;
                        char asset_name[256];
                        size_t anl = 0;
                        while (*ip && *ip != delim && *ip != '\n' && anl + 1 < sizeof(asset_name)) {
                            asset_name[anl++] = *ip++;
                        }
                        asset_name[anl] = '\0';
                        if (*ip == delim) ip++;

                        int64_t embed_limit = -1;
                        while (*ip && *ip != '\0') {
                            while (*ip && isspace((unsigned char)*ip)) ip++;
                            if (*ip == '\0') break;
                            char pname[64];
                            size_t pnl = 0;
                            while (*ip && (isalnum((unsigned char)*ip) || *ip == '_' || *ip == ':') && pnl + 1 < sizeof(pname)) {
                                pname[pnl++] = *ip++;
                            }
                            pname[pnl] = '\0';
                            while (*ip && isspace((unsigned char)*ip)) ip++;
                            if (*ip == '(') {
                                char *parg = zero_cc_cpp_capture_paren_tokens(&ip);
                                if (strcmp(pname, "limit") == 0 && parg) {
                                    int64_t lim = zero_cc_cpp_eval_expr(cpp, parg);
                                    embed_limit = (lim >= 0) ? lim : 0;
                                }
                                if (parg) free(parg);
                            } else {
                                if (*ip) ip++;
                            }
                        }

                        size_t rsz = 0;
                        unsigned char *rdata = zero_cc_cpp_read_binary_resource(cpp, asset_name, is_sys, &rsz, NULL, 0);
                        if (rdata) {
                            if (rsz == 0 || embed_limit == 0) res_code = 2; /* __STDC_EMBED_EMPTY__ */
                            else res_code = 1; /* __STDC_EMBED_FOUND__ */
                            free(rdata);
                        }
                    }
                    if (expanded_inner) free(expanded_inner);
                    free(inner);
                }
                char code_str[16];
                snprintf(code_str, sizeof(code_str), "%d ", res_code);
                zero_cc_buf_puts(&def_buf, code_str);
            } else {
                zero_cc_buf_puts(&def_buf, id);
            }
            continue;
        }
        zero_cc_buf_putc(&def_buf, *lp++);
    }
    zero_cc_buf_putc(&def_buf, '\0');
    zero_cc_buf_free(&line_buf);

    /* Step 3: Macro expansion on expression tokens */
    ZeroCCBuffer exp_buf;
    zero_cc_buf_init(&exp_buf, def_buf.length ? def_buf.length * 2 : 256);
    size_t saved_line = cpp->line_num;
    int saved_depth = cpp->cond_depth;
    cpp->cond_depth = 0;
    zero_cc_cpp_process_text(cpp, def_buf.data, &exp_buf);
    cpp->cond_depth = saved_depth;
    cpp->line_num = saved_line;
    zero_cc_buf_putc(&exp_buf, '\0');
    zero_cc_buf_free(&def_buf);

    /* Step 4: Parse and evaluate constant expression */
    const char *ep = exp_buf.data;
    int64_t result = zero_cc_cpp_eval_comma(&ep);
    zero_cc_buf_free(&exp_buf);
    return result;
}

char *zero_cc_cpp_capture_define_value(ZeroCCPreprocessor *cpp, const char **pp) {
    const char *p = *pp;
    char *val = (char *)malloc(1024);
    if (!val) return NULL;
    size_t vlen = 0, vcap = 1024;
    while (*p && *p != '\n') {
        if (*p == '\\' && p[1] == '\n') { p += 2; cpp->line_num++; continue; }
        if (*p == '/' && p[1] == '/') break;  /* line comment ends the value */
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) {
                if (*p == '\n') cpp->line_num++;
                p++;
            }
            if (*p) p += 2;
            if (vlen > 0 && val[vlen - 1] != ' ') {
                if (vlen + 2 > vcap) {
                    size_t nc = (vlen + 2) * 2;
                    char *nv = (char *)realloc(val, nc);
                    if (!nv) { free(val); *pp = p; return NULL; }
                    val = nv; vcap = nc;
                }
                val[vlen++] = ' ';
            }
            continue;
        }
        if (vlen + 2 > vcap) {
            size_t nc = (vlen + 2) * 2;
            char *nv = (char *)realloc(val, nc);
            if (!nv) { free(val); *pp = p; return NULL; }
            val = nv; vcap = nc;
        }
        val[vlen++] = *p++;
    }
    val[vlen] = '\0';
    *pp = p;
    return val;
}
