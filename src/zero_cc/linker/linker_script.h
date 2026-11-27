#ifndef ZERO_CC_LINKER_LINKER_SCRIPT_H
#define ZERO_CC_LINKER_LINKER_SCRIPT_H

/*
 * linker_script.h - Extended GNU Linker Script Parser for Baremetal Firmware.
 *
 * Supports:
 *   - MEMORY regions (FLASH/ROM, RAM/SRAM, CCMRAM, etc.) with ORIGIN and LENGTH
 *   - Numeric expressions with K/M/G multipliers, operators (+, -, *, /), and
 *     parentheses, plus ORIGIN(), LENGTH(), and ALIGN() calls
 *   - ENTRY(symbol) directive
 *   - SECTIONS block with output section definitions:
 *       .sec_name [vma] [(attr)] : [AT(lma)] { *(patterns) } [> REGION] [AT> LMA_REGION]
 *   - Section attributes: (NOLOAD), ALIGN(expr), SUBALIGN(expr)
 *   - Symbol assignments: PROVIDE(sym = expr), PROVIDE_HIDDEN(sym = expr), sym = expr;
 *   - Boundary symbols: _sidata = LOADADDR(.data), _sdata = ., _edata = ., _stack_top
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define ZC_SCRIPT_MAX_REGIONS  16
#define ZC_SCRIPT_MAX_SECTIONS 32
#define ZC_SCRIPT_MAX_SYMBOLS  32

typedef struct {
    char     name[32];
    uint64_t origin;
    uint64_t length;
    uint32_t flags;
} ZcMemoryRegion;

typedef struct {
    char     name[32];
    char     vma_region[32];
    char     lma_region[32];
    bool     has_vma_region;
    bool     has_lma_region;
    bool     is_noload;
    uint64_t align;
    uint64_t subalign;
    uint64_t explicit_vma;
    bool     has_explicit_vma;
    uint64_t explicit_lma;
    bool     has_explicit_lma;
    char     keep_patterns[8][64];
    int      keep_count;
} ZcScriptSection;

typedef struct {
    char     name[64];
    uint64_t value;
    bool     is_provide;
    bool     is_hidden;
    bool     has_value;
    bool     is_at_end;
    char     expr_str[64];
    char     section_ref[32];
} ZcScriptSymbol;

typedef struct {
    ZcMemoryRegion  flash;
    bool            has_flash;
    ZcMemoryRegion  ram;
    bool            has_ram;
    ZcMemoryRegion  regions[ZC_SCRIPT_MAX_REGIONS];
    int             region_count;
    char            entry[64];
    bool            has_entry;
    uint64_t        stack_top;
    bool            has_stack_top;
    char            keep_patterns[16][64];
    int             keep_count;
    ZcScriptSection sections[ZC_SCRIPT_MAX_SECTIONS];
    int             section_count;
    ZcScriptSymbol  symbols[ZC_SCRIPT_MAX_SYMBOLS];
    int             symbol_count;
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

static inline const ZcMemoryRegion *zc_script_find_region(const ZcLinkScript *script, const char *name) {
    if (!script || !name || !*name) return NULL;
    for (int i = 0; i < script->region_count; i++) {
        if (strcasecmp(script->regions[i].name, name) == 0) {
            return &script->regions[i];
        }
    }
    if (script->has_ram && (strcasecmp(name, "RAM") == 0 || strcasecmp(name, script->ram.name) == 0)) {
        return &script->ram;
    }
    if (script->has_flash && (strcasecmp(name, "FLASH") == 0 || strcasecmp(name, "ROM") == 0 ||
                             strcasecmp(name, script->flash.name) == 0)) {
        return &script->flash;
    }
    return NULL;
}

static inline const ZcScriptSection *zc_script_find_section(const ZcLinkScript *script, const char *name) {
    if (!script || !name || !*name) return NULL;
    for (int i = 0; i < script->section_count; i++) {
        if (strcmp(script->sections[i].name, name) == 0) {
            return &script->sections[i];
        }
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

static inline const char *zc_parse_expr(const char *p, const ZcLinkScript *script, uint64_t *val);

static inline const char *zc_parse_term(const char *p, const ZcLinkScript *script, uint64_t *val) {
    p = zc_skip_ws_and_comments(p);
    if (!p || !*p) return NULL;

    /* ( expr ) */
    if (*p == '(') {
        p = zc_skip_ws_and_comments(p + 1);
        p = zc_parse_expr(p, script, val);
        p = zc_skip_ws_and_comments(p);
        if (p && *p == ')') p++;
        return p;
    }

    /* ORIGIN(name) or org(name) */
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
        const ZcMemoryRegion *mr = zc_script_find_region(script, regname);
        *val = mr ? mr->origin : 0;
        return p;
    }

    /* LENGTH(name) or len(name) */
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
        const ZcMemoryRegion *mr = zc_script_find_region(script, regname);
        *val = mr ? mr->length : 0;
        return p;
    }

    /* ALIGN(expr) */
    if (strncasecmp(p, "ALIGN", 5) == 0 && (p[5] == '(' || isspace((unsigned char)p[5]))) {
        p = zc_skip_ws_and_comments(p + 5);
        if (*p == '(') {
            p = zc_skip_ws_and_comments(p + 1);
            if (*p == '.') {
                p = zc_skip_ws_and_comments(p + 1);
                if (*p == ',') p = zc_skip_ws_and_comments(p + 1);
            }
            uint64_t al = 0;
            p = zc_parse_expr(p, script, &al);
            p = zc_skip_ws_and_comments(p);
            if (p && *p == ')') p++;
            *val = al;
            return p;
        }
    }

    return zc_parse_num(p, val);
}

static inline const char *zc_parse_factor(const char *p, const ZcLinkScript *script, uint64_t *val) {
    p = zc_parse_term(p, script, val);
    if (!p) return NULL;
    while (1) {
        p = zc_skip_ws_and_comments(p);
        if (*p == '*') {
            uint64_t rhs = 0;
            p = zc_parse_term(p + 1, script, &rhs);
            if (!p) return NULL;
            *val *= rhs;
        } else if (*p == '/') {
            uint64_t rhs = 0;
            p = zc_parse_term(p + 1, script, &rhs);
            if (!p || rhs == 0) return NULL;
            *val /= rhs;
        } else {
            break;
        }
    }
    return p;
}

static inline const char *zc_parse_expr(const char *p, const ZcLinkScript *script, uint64_t *val) {
    p = zc_parse_factor(p, script, val);
    if (!p) return NULL;
    while (1) {
        p = zc_skip_ws_and_comments(p);
        if (*p == '+') {
            uint64_t rhs = 0;
            p = zc_parse_factor(p + 1, script, &rhs);
            if (!p) return NULL;
            *val += rhs;
        } else if (*p == '-') {
            uint64_t rhs = 0;
            p = zc_parse_factor(p + 1, script, &rhs);
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

                if (script->region_count < ZC_SCRIPT_MAX_REGIONS) {
                    ZcMemoryRegion *mr = &script->regions[script->region_count++];
                    snprintf(mr->name, sizeof(mr->name), "%s", rname);
                    mr->origin = origin;
                    mr->length = length;
                    mr->flags = attr_w ? 2 : 1;
                }

                if (is_ram && !script->has_ram) {
                    snprintf(script->ram.name, sizeof(script->ram.name), "%s", rname);
                    script->ram.origin = origin;
                    script->ram.length = length;
                    script->has_ram = true;
                } else if (!is_ram && !script->has_flash) {
                    snprintf(script->flash.name, sizeof(script->flash.name), "%s", rname);
                    script->flash.origin = origin;
                    script->flash.length = length;
                    script->has_flash = true;
                }
            }
            if (*p == '}') p = zc_skip_ws_and_comments(p + 1);
            continue;
        }

        /* SECTIONS { ... } block */
        if (strncasecmp(p, "SECTIONS", 8) == 0 && (isspace((unsigned char)p[8]) || p[8] == '{')) {
            p = zc_skip_ws_and_comments(p + 8);
            if (*p != '{') {
                snprintf(err, errsz, "expected '{' after SECTIONS");
                return -1;
            }
            p = zc_skip_ws_and_comments(p + 1);
            while (*p && *p != '}') {
                p = zc_skip_ws_and_comments(p);
                if (!*p || *p == '}') break;

                /* Top-level PROVIDE / PROVIDE_HIDDEN */
                if (strncasecmp(p, "PROVIDE", 7) == 0) {
                    bool is_hidden = (strncasecmp(p, "PROVIDE_HIDDEN", 14) == 0);
                    p = zc_skip_ws_and_comments(p + (is_hidden ? 14 : 7));
                    if (*p == '(') {
                        p = zc_skip_ws_and_comments(p + 1);
                        char sname[64];
                        size_t slen = 0;
                        while (*p && (isalnum((unsigned char)*p) || *p == '_') && slen + 1 < sizeof(sname)) {
                            sname[slen++] = *p++;
                        }
                        sname[slen] = '\0';
                        p = zc_skip_ws_and_comments(p);
                        if (*p == '=') {
                            p = zc_skip_ws_and_comments(p + 1);
                            char expr_buf[64];
                            size_t elen = 0;
                            while (*p && *p != ')' && *p != ';' && elen + 1 < sizeof(expr_buf)) {
                                expr_buf[elen++] = *p++;
                            }
                            expr_buf[elen] = '\0';
                            if (script->symbol_count < ZC_SCRIPT_MAX_SYMBOLS) {
                                ZcScriptSymbol *sym = &script->symbols[script->symbol_count++];
                                snprintf(sym->name, sizeof(sym->name), "%s", sname);
                                snprintf(sym->expr_str, sizeof(sym->expr_str), "%s", expr_buf);
                                sym->is_provide = true;
                                sym->is_hidden = is_hidden;
                            }
                        }
                        while (*p && *p != ')') p++;
                        if (*p == ')') p++;
                        p = zc_skip_ws_and_comments(p);
                        if (*p == ';') p++;
                    }
                    continue;
                }

                /* Symbol assignment: name = expr; */
                if ((isalpha((unsigned char)*p) || *p == '_') && zc_strcasestr(p, "=") != NULL) {
                    const char *eq = strchr(p, '=');
                    const char *semi = strchr(p, ';');
                    const char *brace = strchr(p, '{');
                    if (eq && (!brace || eq < brace) && (!semi || eq < semi)) {
                        char sname[64];
                        size_t slen = 0;
                        while (*p && (isalnum((unsigned char)*p) || *p == '_') && slen + 1 < sizeof(sname)) {
                            sname[slen++] = *p++;
                        }
                        sname[slen] = '\0';
                        p = zc_skip_ws_and_comments(p);
                        if (*p == '=') {
                            p = zc_skip_ws_and_comments(p + 1);
                            char expr_buf[64];
                            size_t elen = 0;
                            while (*p && *p != ';' && elen + 1 < sizeof(expr_buf)) {
                                expr_buf[elen++] = *p++;
                            }
                            expr_buf[elen] = '\0';
                            if (script->symbol_count < ZC_SCRIPT_MAX_SYMBOLS) {
                                ZcScriptSymbol *sym = &script->symbols[script->symbol_count++];
                                snprintf(sym->name, sizeof(sym->name), "%s", sname);
                                snprintf(sym->expr_str, sizeof(sym->expr_str), "%s", expr_buf);
                                sym->is_provide = false;
                                sym->is_hidden = false;
                            }
                            if (strcmp(sname, "_stack_top") == 0 || strcmp(sname, "__stack_top") == 0) {
                                uint64_t st = 0;
                                if (zc_parse_expr(expr_buf, script, &st)) {
                                    script->stack_top = st;
                                    script->has_stack_top = true;
                                }
                            }
                        }
                        p = zc_skip_ws_and_comments(p);
                        if (*p == ';') p++;
                        continue;
                    }
                }

                /* Location counter assignment: . = ...; */
                if (*p == '.' && *(p + 1) != '.' && !isalnum((unsigned char)*(p + 1))) {
                    while (*p && *p != ';') p++;
                    if (*p == ';') p = zc_skip_ws_and_comments(p + 1);
                    continue;
                }

                /* Section definition: .sec_name [vma] [(attr)] : [AT(lma)] { ... } [> REGION] [AT> LMA_REGION] */
                char sec_name[32];
                size_t snlen = 0;
                while (*p && !isspace((unsigned char)*p) && *p != ':' && *p != '{' && *p != '(' &&
                       snlen + 1 < sizeof(sec_name)) {
                    sec_name[snlen++] = *p++;
                }
                sec_name[snlen] = '\0';
                if (snlen == 0) {
                    p++;
                    continue;
                }

                ZcScriptSection sec;
                memset(&sec, 0, sizeof(sec));
                snprintf(sec.name, sizeof(sec.name), "%s", sec_name);

                p = zc_skip_ws_and_comments(p);
                /* Attributes before colon: (NOLOAD) */
                if (*p == '(') {
                    if (strncasecmp(p, "(NOLOAD)", 8) == 0) {
                        sec.is_noload = true;
                        p = zc_skip_ws_and_comments(p + 8);
                    } else {
                        while (*p && *p != ')') p++;
                        if (*p == ')') p = zc_skip_ws_and_comments(p + 1);
                    }
                }

                /* Colon */
                if (*p == ':') {
                    p = zc_skip_ws_and_comments(p + 1);
                }

                while (*p && *p != '{') {
                    p = zc_skip_ws_and_comments(p);
                    if (strncasecmp(p, "ALIGN", 5) == 0 && (p[5] == '(' || isspace((unsigned char)p[5]))) {
                        p = zc_skip_ws_and_comments(p + 5);
                        if (*p == '(') {
                            p = zc_skip_ws_and_comments(p + 1);
                            uint64_t al = 0;
                            p = zc_parse_expr(p, script, &al);
                            if (p && *p == ')') p++;
                            sec.align = al;
                        }
                        continue;
                    }
                    if (strncasecmp(p, "SUBALIGN", 8) == 0 && (p[8] == '(' || isspace((unsigned char)p[8]))) {
                        p = zc_skip_ws_and_comments(p + 8);
                        if (*p == '(') {
                            p = zc_skip_ws_and_comments(p + 1);
                            uint64_t sal = 0;
                            p = zc_parse_expr(p, script, &sal);
                            if (p && *p == ')') p++;
                            sec.subalign = sal;
                        }
                        continue;
                    }
                    if (strncasecmp(p, "AT", 2) == 0 && (p[2] == '(' || isspace((unsigned char)p[2]))) {
                        p = zc_skip_ws_and_comments(p + 2);
                        if (*p == '(') {
                            p = zc_skip_ws_and_comments(p + 1);
                            uint64_t lma_val = 0;
                            p = zc_parse_expr(p, script, &lma_val);
                            if (p && *p == ')') p++;
                            sec.explicit_lma = lma_val;
                            sec.has_explicit_lma = true;
                        }
                        continue;
                    }
                    break;
                }

                /* Section body { ... } */
                bool sec_pattern_seen = false;
                if (*p == '{') {
                    p = zc_skip_ws_and_comments(p + 1);
                    while (*p && *p != '}') {
                        p = zc_skip_ws_and_comments(p);
                        if (!*p || *p == '}') break;

                        /* KEEP(*(pattern)) */
                        if (strncasecmp(p, "KEEP", 4) == 0 && (p[4] == '(' || isspace((unsigned char)p[4]))) {
                            sec_pattern_seen = true;
                            const char *kp = p + 4;
                            kp = zc_skip_ws_and_comments(kp);
                            if (*kp == '(') {
                                kp++;
                                while (*kp && (*kp == '*' || *kp == '(' || isspace((unsigned char)*kp))) kp++;
                                const char *start = kp;
                                while (*kp && *kp != ')' && *kp != ' ' && *kp != '\t' && *kp != '\n' && *kp != ';') kp++;
                                size_t len = (size_t)(kp - start);
                                if (len > 0 && len < 64 && script->keep_count < 16) {
                                    memcpy(script->keep_patterns[script->keep_count], start, len);
                                    script->keep_patterns[script->keep_count][len] = '\0';
                                    script->keep_count++;
                                }
                                if (len > 0 && len < 64 && sec.keep_count < 8) {
                                    memcpy(sec.keep_patterns[sec.keep_count], start, len);
                                    sec.keep_patterns[sec.keep_count][len] = '\0';
                                    sec.keep_count++;
                                }
                            }
                            while (*p && *p != ';') p++;
                            if (*p == ';') p++;
                            continue;
                        }

                        /* Alignment inside body: ALIGN(expr) or . = ALIGN(expr); */
                        if (strncasecmp(p, "ALIGN", 5) == 0 || (p[0] == '.' && strstr(p, "ALIGN"))) {
                            const char *ap = zc_strcasestr(p, "ALIGN");
                            if (ap) {
                                ap = zc_skip_ws_and_comments(ap + 5);
                                if (*ap == '(') {
                                    uint64_t al = 0;
                                    zc_parse_expr(ap + 1, script, &al);
                                    if (al > sec.align) sec.align = al;
                                }
                            }
                            while (*p && *p != ';') p++;
                            if (*p == ';') p++;
                            continue;
                        }

                        /* Symbol assignment inside body: _sdata = .; or PROVIDE(...) */
                        if (strncasecmp(p, "PROVIDE", 7) == 0 ||
                            ((isalpha((unsigned char)*p) || *p == '_') && strchr(p, '='))) {
                            const char *eq = strchr(p, '=');
                            const char *semi = strchr(p, ';');
                            if (eq && (!semi || eq < semi)) {
                                bool is_prov = (strncasecmp(p, "PROVIDE", 7) == 0);
                                bool is_hid = (strncasecmp(p, "PROVIDE_HIDDEN", 14) == 0);
                                const char *sp = p;
                                if (is_hid) sp = zc_skip_ws_and_comments(sp + 14);
                                else if (is_prov) sp = zc_skip_ws_and_comments(sp + 7);
                                if (*sp == '(') sp = zc_skip_ws_and_comments(sp + 1);

                                char sym_name[64];
                                size_t slen = 0;
                                while (*sp && (isalnum((unsigned char)*sp) || *sp == '_') && slen + 1 < sizeof(sym_name)) {
                                    sym_name[slen++] = *sp++;
                                }
                                sym_name[slen] = '\0';
                                sp = zc_skip_ws_and_comments(sp);
                                if (*sp == '=') {
                                    sp = zc_skip_ws_and_comments(sp + 1);
                                    char expr_buf[64];
                                    size_t elen = 0;
                                    while (*sp && *sp != ')' && *sp != ';' && elen + 1 < sizeof(expr_buf)) {
                                        expr_buf[elen++] = *sp++;
                                    }
                                    expr_buf[elen] = '\0';
                                    if (script->symbol_count < ZC_SCRIPT_MAX_SYMBOLS) {
                                        ZcScriptSymbol *sym = &script->symbols[script->symbol_count++];
                                        snprintf(sym->name, sizeof(sym->name), "%s", sym_name);
                                        snprintf(sym->expr_str, sizeof(sym->expr_str), "%s", expr_buf);
                                        snprintf(sym->section_ref, sizeof(sym->section_ref), "%s", sec.name);
                                        sym->is_provide = is_prov;
                                        sym->is_hidden = is_hid;
                                        sym->is_at_end = sec_pattern_seen;
                                    }
                                }
                                while (*p && *p != ';') p++;
                                if (*p == ';') p++;
                                continue;
                            }
                        }

                        /* Input section pattern: *(.sec) or .*(.sec) */
                        if (*p == '*' || *p == '.') {
                            sec_pattern_seen = true;
                            while (*p && *p != '\n' && *p != ';' && *p != '}') {
                                if (*p == '(') {
                                    p++;
                                    while (*p && *p != ')') p++;
                                    if (*p == ')') p++;
                                    break;
                                }
                                p++;
                            }
                            while (*p && *p != '\n' && *p != ';' && *p != '}') p++;
                            if (*p == ';') p++;
                            continue;
                        }

                        /* Skip other statements */
                        while (*p && *p != ';' && *p != '}') p++;
                        if (*p == ';') p++;
                    }
                    if (*p == '}') p = zc_skip_ws_and_comments(p + 1);
                }

                /* After body: check for (NOLOAD), > REGION, AT> LMA_REGION */
                if (strncasecmp(p, "(NOLOAD)", 8) == 0) {
                    sec.is_noload = true;
                    p = zc_skip_ws_and_comments(p + 8);
                }

                if (*p == '>') {
                    p = zc_skip_ws_and_comments(p + 1);
                    char rname[32];
                    size_t rnlen = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && rnlen + 1 < sizeof(rname)) {
                        rname[rnlen++] = *p++;
                    }
                    rname[rnlen] = '\0';
                    snprintf(sec.vma_region, sizeof(sec.vma_region), "%s", rname);
                    sec.has_vma_region = (rnlen > 0);
                    p = zc_skip_ws_and_comments(p);
                }

                if (strncasecmp(p, "AT>", 3) == 0) {
                    p = zc_skip_ws_and_comments(p + 3);
                    char rname[32];
                    size_t rnlen = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && rnlen + 1 < sizeof(rname)) {
                        rname[rnlen++] = *p++;
                    }
                    rname[rnlen] = '\0';
                    snprintf(sec.lma_region, sizeof(sec.lma_region), "%s", rname);
                    sec.has_lma_region = (rnlen > 0);
                    p = zc_skip_ws_and_comments(p);
                }

                if (*p == ';') p++;

                if (script->section_count < ZC_SCRIPT_MAX_SECTIONS) {
                    script->sections[script->section_count++] = sec;
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

        /* Generic top-level { ... } block scan, capturing any KEEP(*(pattern)) directives */
        if (*p == '{') {
            int depth = 1;
            p++;
            while (*p && depth > 0) {
                if (*p == '{') {
                    depth++;
                    p++;
                } else if (*p == '}') {
                    depth--;
                    p++;
                } else if (strncasecmp(p, "KEEP", 4) == 0 &&
                           (p[4] == '(' || isspace((unsigned char)p[4]))) {
                    const char *kp = p + 4;
                    kp = zc_skip_ws_and_comments(kp);
                    if (*kp == '(') {
                        kp++;
                        while (*kp && (*kp == '*' || *kp == '(' || isspace((unsigned char)*kp))) kp++;
                        const char *start = kp;
                        while (*kp && *kp != ')' && *kp != ' ' && *kp != '\t' && *kp != '\n' && *kp != ';') kp++;
                        size_t len = (size_t)(kp - start);
                        if (len > 0 && len < 64 && script->keep_count < 16) {
                            memcpy(script->keep_patterns[script->keep_count], start, len);
                            script->keep_patterns[script->keep_count][len] = '\0';
                            script->keep_count++;
                        }
                    }
                    p += 4;
                } else {
                    p++;
                }
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
