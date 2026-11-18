#ifndef ZERO_CC_LINKER_LINKER_SCRIPT_H
#define ZERO_CC_LINKER_LINKER_SCRIPT_H

/*
 * linker_script.h - Embedded Baremetal Linker Script Parser (Phase 6 / Scope).
 *
 * Parses GNU ld compatible linker scripts for baremetal targets:
 *   - MEMORY regions (FLASH/ROM, RAM/SRAM) with ORIGIN and LENGTH
 *   - Numeric expressions with K/M/G multipliers and ORIGIN()/LENGTH() calls
 *   - ENTRY(symbol) directive
 *   - Boundary and stack top symbol assignments (_stack_top = ...)
 *   - Skips balanced SECTIONS { ... } blocks for forward compatibility
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    char     name[32];
    uint64_t origin;
    uint64_t length;
    uint32_t flags;
} ZcMemoryRegion;

typedef struct {
    ZcMemoryRegion flash;
    bool           has_flash;
    ZcMemoryRegion ram;
    bool           has_ram;
    char           entry[64];
    bool           has_entry;
    uint64_t       stack_top;
    bool           has_stack_top;
} ZcLinkScript;

static inline const char *zc_skip_ws_and_comments(const char *p) {
    while (*p) {
        if (isspace((unsigned char)*p)) {
            p++;
        } else if (*p == '/' && *(p + 1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p + 1) == '/')) p++;
            if (*p) p += 2;
        } else if (*p == '/' && *(p + 1) == '/') {
            p += 2;
            while (*p && *p != '\n') p++;
        } else if (*p == '#') {
            while (*p && *p != '\n') p++;
        } else {
            break;
        }
    }
    return p;
}

static inline const char *zc_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return haystack;
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0) return haystack;
    }
    return NULL;
}

static inline const char *zc_parse_num(const char *p, uint64_t *val) {
    p = zc_skip_ws_and_comments(p);
    if (!p || !*p) return NULL;
    char *end = NULL;
    uint64_t v = strtoull(p, &end, 0);
    if (end == p) return NULL;
    p = end;
    if (*p == 'K' || *p == 'k') {
        v *= 1024ULL;
        p++;
    } else if (*p == 'M' || *p == 'm') {
        v *= (1024ULL * 1024ULL);
        p++;
    } else if (*p == 'G' || *p == 'g') {
        v *= (1024ULL * 1024ULL * 1024ULL);
        p++;
    }
    *val = v;
    return p;
}

static inline const char *zc_parse_term(const char *p, const ZcLinkScript *script, uint64_t *val) {
    p = zc_skip_ws_and_comments(p);
    if (!p || !*p) return NULL;
    if (strncasecmp(p, "ORIGIN", 6) == 0 || strncasecmp(p, "org", 3) == 0) {
        int adv = (strncasecmp(p, "ORIGIN", 6) == 0) ? 6 : 3;
        p = zc_skip_ws_and_comments(p + adv);
        if (*p != '(') return NULL;
        p = zc_skip_ws_and_comments(p + 1);
        char regname[32];
        size_t len = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') && len + 1 < sizeof(regname)) {
            regname[len++] = *p++;
        }
        regname[len] = '\0';
        p = zc_skip_ws_and_comments(p);
        if (*p != ')') return NULL;
        p++;
        if (script->has_ram && (strcasecmp(regname, script->ram.name) == 0 || strcasecmp(regname, "RAM") == 0)) {
            *val = script->ram.origin;
        } else if (script->has_flash && (strcasecmp(regname, script->flash.name) == 0 ||
                                         strcasecmp(regname, "FLASH") == 0 ||
                                         strcasecmp(regname, "ROM") == 0)) {
            *val = script->flash.origin;
        } else {
            *val = 0;
        }
        return p;
    }
    if (strncasecmp(p, "LENGTH", 6) == 0 || strncasecmp(p, "len", 3) == 0) {
        int adv = (strncasecmp(p, "LENGTH", 6) == 0) ? 6 : 3;
        p = zc_skip_ws_and_comments(p + adv);
        if (*p != '(') return NULL;
        p = zc_skip_ws_and_comments(p + 1);
        char regname[32];
        size_t len = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') && len + 1 < sizeof(regname)) {
            regname[len++] = *p++;
        }
        regname[len] = '\0';
        p = zc_skip_ws_and_comments(p);
        if (*p != ')') return NULL;
        p++;
        if (script->has_ram && (strcasecmp(regname, script->ram.name) == 0 || strcasecmp(regname, "RAM") == 0)) {
            *val = script->ram.length;
        } else if (script->has_flash && (strcasecmp(regname, script->flash.name) == 0 ||
                                         strcasecmp(regname, "FLASH") == 0 ||
                                         strcasecmp(regname, "ROM") == 0)) {
            *val = script->flash.length;
        } else {
            *val = 0;
        }
        return p;
    }
    return zc_parse_num(p, val);
}

static inline const char *zc_parse_expr(const char *p, const ZcLinkScript *script, uint64_t *val) {
    p = zc_parse_term(p, script, val);
    if (!p) return NULL;
    while (1) {
        p = zc_skip_ws_and_comments(p);
        if (*p == '+') {
            uint64_t rhs = 0;
            p = zc_parse_term(p + 1, script, &rhs);
            if (!p) return NULL;
            *val += rhs;
        } else if (*p == '-') {
            uint64_t rhs = 0;
            p = zc_parse_term(p + 1, script, &rhs);
            if (!p) return NULL;
            *val -= rhs;
        } else {
            break;
        }
    }
    return p;
}

static inline int zero_cc_parse_linker_script(const char *content, ZcLinkScript *script,
                                              char *err, size_t errsz) {
    memset(script, 0, sizeof(*script));
    const char *p = content;

    while (*p) {
        p = zc_skip_ws_and_comments(p);
        if (!*p) break;

        /* MEMORY { ... } block */
        if (strncasecmp(p, "MEMORY", 6) == 0 && (isspace((unsigned char)p[6]) || p[6] == '{')) {
            p = zc_skip_ws_and_comments(p + 6);
            if (*p != '{') {
                snprintf(err, errsz, "expected '{' after MEMORY");
                return -1;
            }
            p = zc_skip_ws_and_comments(p + 1);
            while (*p && *p != '}') {
                /* Parse region name */
                char rname[32];
                size_t rlen = 0;
                while (*p && (isalnum((unsigned char)*p) || *p == '_') && rlen + 1 < sizeof(rname)) {
                    rname[rlen++] = *p++;
                }
                rname[rlen] = '\0';
                if (rlen == 0) {
                    snprintf(err, errsz, "expected memory region name in MEMORY block");
                    return -1;
                }
                p = zc_skip_ws_and_comments(p);

                /* Parse optional attributes: (rx), (rwx), etc. */
                bool attr_w = false;
                if (*p == '(') {
                    p++;
                    while (*p && *p != ')') {
                        if (*p == 'w' || *p == 'W') attr_w = true;
                        p++;
                    }
                    if (*p == ')') p++;
                    p = zc_skip_ws_and_comments(p);
                }
                if (*p != ':') {
                    snprintf(err, errsz, "expected ':' after memory region '%s'", rname);
                    return -1;
                }
                p = zc_skip_ws_and_comments(p + 1);

                /* Parse ORIGIN / org / o */
                if (strncasecmp(p, "ORIGIN", 6) == 0) p += 6;
                else if (strncasecmp(p, "org", 3) == 0) p += 3;
                else if (*p == 'o' || *p == 'O') p += 1;
                else {
                    snprintf(err, errsz, "expected ORIGIN in region '%s'", rname);
                    return -1;
                }
                p = zc_skip_ws_and_comments(p);
                if (*p == '=') p = zc_skip_ws_and_comments(p + 1);
                uint64_t origin = 0;
                p = zc_parse_expr(p, script, &origin);
                if (!p) {
                    snprintf(err, errsz, "invalid ORIGIN value in region '%s'", rname);
                    return -1;
                }
                p = zc_skip_ws_and_comments(p);
                if (*p == ',') p = zc_skip_ws_and_comments(p + 1);

                /* Parse LENGTH / len / l */
                if (strncasecmp(p, "LENGTH", 6) == 0) p += 6;
                else if (strncasecmp(p, "len", 3) == 0) p += 3;
                else if (*p == 'l' || *p == 'L') p += 1;
                else {
                    snprintf(err, errsz, "expected LENGTH in region '%s'", rname);
                    return -1;
                }
                p = zc_skip_ws_and_comments(p);
                if (*p == '=') p = zc_skip_ws_and_comments(p + 1);
                uint64_t length = 0;
                p = zc_parse_expr(p, script, &length);
                if (!p) {
                    snprintf(err, errsz, "invalid LENGTH value in region '%s'", rname);
                    return -1;
                }
                p = zc_skip_ws_and_comments(p);
                if (*p == ',' || *p == ';') p = zc_skip_ws_and_comments(p + 1);

                /* Classify region as FLASH (ROM) or RAM */
                bool is_ram = false;
                if (zc_strcasestr(rname, "ram") != NULL || zc_strcasestr(rname, "sram") != NULL ||
                    zc_strcasestr(rname, "data") != NULL || attr_w) {
                    is_ram = true;
                }
                if (is_ram) {
                    snprintf(script->ram.name, sizeof(script->ram.name), "%s", rname);
                    script->ram.origin = origin;
                    script->ram.length = length;
                    script->has_ram = true;
                } else {
                    snprintf(script->flash.name, sizeof(script->flash.name), "%s", rname);
                    script->flash.origin = origin;
                    script->flash.length = length;
                    script->has_flash = true;
                }
            }
            if (*p == '}') p = zc_skip_ws_and_comments(p + 1);
            continue;
        }

        /* ENTRY(symbol) directive */
        if (strncasecmp(p, "ENTRY", 5) == 0 && (isspace((unsigned char)p[5]) || p[5] == '(')) {
            p = zc_skip_ws_and_comments(p + 5);
            if (*p == '(') {
                p = zc_skip_ws_and_comments(p + 1);
                size_t elen = 0;
                while (*p && (isalnum((unsigned char)*p) || *p == '_') && elen + 1 < sizeof(script->entry)) {
                    script->entry[elen++] = *p++;
                }
                script->entry[elen] = '\0';
                p = zc_skip_ws_and_comments(p);
                if (*p == ')') p++;
                script->has_entry = (elen > 0);
            }
            p = zc_skip_ws_and_comments(p);
            if (*p == ';') p++;
            continue;
        }

        /* _stack_top / __stack_top assignment */
        if (strncmp(p, "_stack_top", 10) == 0 || strncmp(p, "__stack_top", 11) == 0) {
            int adv = (strncmp(p, "__stack_top", 11) == 0) ? 11 : 10;
            p = zc_skip_ws_and_comments(p + adv);
            if (*p == '=') {
                p = zc_skip_ws_and_comments(p + 1);
                p = zc_parse_expr(p, script, &script->stack_top);
                if (p) script->has_stack_top = true;
            }
            if (p) {
                p = zc_skip_ws_and_comments(p);
                if (*p == ';') p++;
            }
            continue;
        }

        /* Skip balanced { ... } blocks such as SECTIONS { ... } */
        if (*p == '{') {
            int depth = 1;
            p++;
            while (*p && depth > 0) {
                if (*p == '{') depth++;
                else if (*p == '}') depth--;
                p++;
            }
            continue;
        }

        /* Advance past unknown identifier or token */
        while (*p && !isspace((unsigned char)*p) && *p != '{' && *p != '}' && *p != ';') {
            p++;
        }
        if (*p == ';') p++;
    }

    return 0;
}

static inline int zero_cc_parse_linker_script_file(const char *path, ZcLinkScript *script,
                                                   char *err, size_t errsz) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "cannot open linker script '%s'", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 1024 * 1024) {
        fclose(f);
        snprintf(err, errsz, "linker script '%s' too large or invalid", path);
        return -1;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        snprintf(err, errsz, "out of memory reading linker script '%s'", path);
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    int rc = zero_cc_parse_linker_script(buf, script, err, errsz);
    free(buf);
    return rc;
}

#endif /* ZERO_CC_LINKER_LINKER_SCRIPT_H */
