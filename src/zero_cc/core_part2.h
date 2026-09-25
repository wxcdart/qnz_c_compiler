                        while (*p && isspace((unsigned char)*p) && *p != '\n') p++;
                        const char *body_start = p;
                        while (*p && *p != '\n') {
                            if (*p == '\\' && p[1] == '\n') { p += 2; continue; }
                            p++;
                        }
                        size_t body_len = p - body_start;
                        char *body = (char *)malloc(body_len + 1);
                        memcpy(body, body_start, body_len);
                        body[body_len] = '\0';

                        if (cpp->macro_count < ZERO_CC_MAX_MACROS) {
                            size_t idx = cpp->macro_count++;
                            ZeroCCMacro *m = &cpp->macros[idx];
                            memset(m,0,sizeof(*m));
                            snprintf(m->name, sizeof(m->name), "%s", macro_name);
                            m->param_count = pcount;
                            m->is_variadic = is_var;
                            for (int k = 0; k < pcount; k++) m->params[k] = params[k];
                            m->replacement = body;

                            uint32_t b = (uint32_t)(zero_cc_hash64(macro_name) & 4095);
                            cpp->hash_next[idx] = cpp->hash_buckets[b];
                            cpp->hash_buckets[b] = (int32_t)idx;
                        }
                    } else {
                        while (*p && isspace((unsigned char)*p) && *p != '\n') p++;
                        const char *val_start = p;
                        while (*p && *p != '\n') {
                            if (*p == '\\' && p[1] == '\n') { p += 2; continue; }
                            p++;
                        }
                        size_t val_len = p - val_start;
                        char *val = (char *)malloc(val_len + 1);
                        memcpy(val, val_start, val_len);
                        val[val_len] = '\0';
                        zero_cc_cpp_define_obj(cpp, macro_name, val);
                        free(val);
                    }
                } else if (active && strcmp(directive, "undef") == 0) {
                    char macro_name[128]; size_t ml = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && ml < 127) macro_name[ml++] = *p++;
                    macro_name[ml] = '\0';
                    zero_cc_cpp_undef(cpp, macro_name);
                } else if (active && strcmp(directive, "include") == 0) {
                    bool is_sys = (*p == '<');
                    char delim_end = is_sys ? '>' : '"';
                    p++;
                    char inc_name[256]; size_t inl = 0;
                    while (*p && *p != delim_end && *p != '\n' && inl < 255) inc_name[inl++] = *p++;
                    inc_name[inl] = '\0';
                    if (*p == delim_end) p++;
                    
                    char *inc_content = zero_cc_cpp_resolve_include(cpp, inc_name, is_sys);
                    if (inc_content) {
                        zero_cc_cpp_process_text(cpp, inc_content, out);
                        free(inc_content);
                    }
                } else if (active && strcmp(directive, "embed") == 0) {
                    while (*p && isspace((unsigned char)*p) && *p != '\n') p++;
                    bool is_sys = (*p == '<');
                    char delim_end = is_sys ? '>' : '"';
                    if (*p == '"' || *p == '<') p++;
                    char asset_path[256]; size_t apl = 0;
                    while (*p && *p != delim_end && *p != '\n' && apl < 255) asset_path[apl++] = *p++;
                    asset_path[apl] = '\0';
                    if (*p == delim_end) p++;

                    FILE *f = fopen(asset_path, "rb");
                    if (!f && cpp) {
                        /* Try resolve via include search paths */
                        char *resolved = zero_cc_cpp_resolve_include(cpp, asset_path, is_sys);
                        if (resolved) {
                            /* read from resolved string as raw bytes */
                            size_t rlen = strlen(resolved);
                            for (size_t i = 0; i < rlen; i++) {
                                char buf[16];
                                snprintf(buf, sizeof(buf), "%u%s", (unsigned char)resolved[i], (i + 1 < rlen) ? ", " : "");
                                zero_cc_buf_puts(out, buf);
                            }
                            free(resolved);
                            f = NULL;
                        }
                    }
                    if (f) {
                        fseek(f, 0, SEEK_END);
                        long fsz = ftell(f);
                        fseek(f, 0, SEEK_SET);
                        if (fsz > 0) {
                            unsigned char *data = (unsigned char *)malloc((size_t)fsz);
                            if (data) {
                                size_t r = fread(data, 1, (size_t)fsz, f);
                                for (size_t i = 0; i < r; i++) {
                                    char buf[16];
                                    snprintf(buf, sizeof(buf), "%u%s", data[i], (i + 1 < r) ? ", " : "");
                                    zero_cc_buf_puts(out, buf);
                                }
                                free(data);
                            }
                        }
                        fclose(f);
                    }
                }

                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
                at_line_start = true;
                continue;
            }
        }

        if (cpp->cond_depth > 0 && !cpp->cond_true[cpp->cond_depth]) {
            while (*p && *p != '\n') p++;
            if (*p == '\n') { p++; at_line_start = true; }
            continue;
        }

        at_line_start = (*p == '\n');

        /* String literals */
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            zero_cc_buf_putc(out, *p++);
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) {
                    zero_cc_buf_putc(out, *p++);
                }
                zero_cc_buf_putc(out, *p++);
            }
            if (*p == quote) zero_cc_buf_putc(out, *p++);
            continue;
        }

        /* Identifiers or macro substitutions */
        if (isalpha((unsigned char)*p) || *p == '_') {
            char ident[128]; size_t il = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && il < 127) ident[il++] = *p++;
            ident[il] = '\0';

            ZeroCCMacro *m = zero_cc_cpp_find_macro(cpp, ident);
            if (m) {
                if (m->param_count < 0) {
                    zero_cc_buf_puts(out, m->replacement);
                } else if (*p == '(') {
                    p++;
                    char *args[64];
                    int arg_count = 0;
                    while (*p && *p != ')') {
                        while (*p && isspace((unsigned char)*p)) p++;
                        const char *arg_s = p;
                        int paren_nest = 0;
                        while (*p && (paren_nest > 0 || (*p != ',' && *p != ')'))) {
                            if (*p == '(') paren_nest++;
                            else if (*p == ')') paren_nest--;
                            p++;
                        }
                        size_t arg_l = (size_t)(p - arg_s);
                        char *arg_str = (char *)malloc(arg_l + 1);
                        if (arg_str) {
                            memcpy(arg_str, arg_s, arg_l);
                            arg_str[arg_l] = '\0';
                            if (arg_count < 64) {
                                args[arg_count++] = arg_str;
                            } else {
                                free(arg_str);
                            }
                        }
                        if (*p == ',') p++;
                    }
                    if (*p == ')') p++;

                    /* Bundle variadic arguments into __VA_ARGS__ */
                    if (m->is_variadic && m->param_count > 0 && arg_count >= m->param_count) {
                        size_t total_va_len = 0;
                        for (int k = m->param_count - 1; k < arg_count; k++) {
                            total_va_len += strlen(args[k]) + 4;
                        }
                        char *va_str = (char *)malloc(total_va_len + 1);
                        if (va_str) {
                            va_str[0] = '\0';
                            for (int k = m->param_count - 1; k < arg_count; k++) {
                                if (k > m->param_count - 1) strcat(va_str, ", ");
                                strcat(va_str, args[k]);
                                free(args[k]);
                            }
                            args[m->param_count - 1] = va_str;
                            arg_count = m->param_count;
                        }
                    }

                    const char *b = m->replacement;
                    while (*b) {
                        /* Token pasting: ## */
                        if (b[0] == '#' && b[1] == '#') {
                            b += 2;
                            while (*b && isspace((unsigned char)*b)) b++;
                            continue;
                        }

                        /* Stringification: #param */
                        if (*b == '#' && (isalpha((unsigned char)b[1]) || b[1] == '_')) {
                            b++;
                            char param_id[128]; size_t pl = 0;
                            while (*b && (isalnum((unsigned char)*b) || *b == '_') && pl < 127) param_id[pl++] = *b++;
                            param_id[pl] = '\0';
                            int match = -1;
                            for (int k = 0; k < m->param_count; k++) {
                                if (strcmp(m->params[k], param_id) == 0) { match = k; break; }
                            }
                            if (strcmp(param_id, "__VA_ARGS__") == 0 && m->is_variadic && m->param_count > 0) {
                                match = m->param_count - 1;
                            }
                            if (match >= 0 && match < arg_count) {
                                zero_cc_buf_putc(out, '"');
                                zero_cc_buf_puts(out, args[match]);
                                zero_cc_buf_putc(out, '"');
                            }
                            continue;
                        }

                        if (isalpha((unsigned char)*b) || *b == '_') {
                            char param_id[128]; size_t pl = 0;
                            while (*b && (isalnum((unsigned char)*b) || *b == '_') && pl < 127) param_id[pl++] = *b++;
                            param_id[pl] = '\0';
                            int match = -1;
                            for (int k = 0; k < m->param_count; k++) {
                                if (strcmp(m->params[k], param_id) == 0) { match = k; break; }
                            }
                            if (strcmp(param_id, "__VA_ARGS__") == 0 && m->is_variadic && m->param_count > 0) {
                                match = m->param_count - 1;
                            }
                            if (match >= 0 && match < arg_count) {
                                zero_cc_buf_puts(out, args[match]);
                            } else {
                                zero_cc_buf_puts(out, param_id);
                            }
                        } else {
                            zero_cc_buf_putc(out, *b++);
                        }
                    }
                    for (int k = 0; k < arg_count; k++) free(args[k]);
                } else {
                    zero_cc_buf_puts(out, ident);
                }
            } else {
                zero_cc_buf_puts(out, ident);
            }
            continue;
        }

        zero_cc_buf_putc(out, *p++);
    }
}

typedef struct {
    char *name;
    char *value;
} ZeroCCMacroDef;

typedef struct {
    ZeroCCMacroDef defs[128];
    size_t count;
} ZeroCCDefines;

static inline char* zero_cc_preprocess_ext(const char *source, ZeroCCIncludeDirs *inc_dirs, ZeroCCDefines *defines) {
    ZeroCCPreprocessor *cpp = (ZeroCCPreprocessor *)calloc(1, sizeof(ZeroCCPreprocessor));
    if (!cpp) return strdup(source ? source : "");
    zero_cc_cpp_init(cpp);
    zero_cc_cpp_define_builtin_macros(cpp);
    if (defines) {
        for (size_t i = 0; i < defines->count; i++) {
            if (defines->defs[i].name) {
                zero_cc_cpp_define_obj(cpp, defines->defs[i].name, defines->defs[i].value ? defines->defs[i].value : "1");
            }
        }
    }
    if (inc_dirs) {
        for (size_t i = 0; i < inc_dirs->count; i++) {
            zero_cc_cpp_add_include_dir(cpp, inc_dirs->paths[i]);
        }
    }
    ZeroCCBuffer out;
    zero_cc_buf_init(&out, 4096);
    zero_cc_cpp_process_text(cpp, source, &out);
    free(cpp);
    return out.data;
}

static inline char* zero_cc_preprocess(const char *source, ZeroCCIncludeDirs *inc_dirs) {
    return zero_cc_preprocess_ext(source, inc_dirs, NULL);
}

#endif /* ZERO_CC_CORE_H */


