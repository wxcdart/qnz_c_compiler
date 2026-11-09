/* ar_writer.c - Classic Unix/GNU static archive writer for ZeroCC. */
#include "zero_cc/linker/ar_writer.h"
#include "zero_cc/linker/ar_reader.h"
#include "zero_cc/linker/elf_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#define AR_MAGIC "!<arch>\n"
#define AR_MAGIC_LEN 8
#define AR_HDR_SIZE 60

static inline void write_be32(uint8_t *p, uint32_t val) {
    p[0] = (uint8_t)((val >> 24) & 0xff);
    p[1] = (uint8_t)((val >> 16) & 0xff);
    p[2] = (uint8_t)((val >> 8) & 0xff);
    p[3] = (uint8_t)(val & 0xff);
}

static const char *get_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void format_ar_header(char *hdr, const char *name, size_t size, uint32_t mode) {
    memset(hdr, ' ', AR_HDR_SIZE);
    /* 0..15: name */
    size_t nlen = strlen(name);
    if (nlen > 16) nlen = 16;
    memcpy(hdr, name, nlen);
    /* 16..27: date (0 for deterministic archive) */
    hdr[16] = '0';
    /* 28..33: uid */
    hdr[28] = '0';
    /* 34..39: gid */
    hdr[34] = '0';
    /* 40..47: mode (octal) */
    char mode_buf[16];
    if (mode == 0) {
        snprintf(mode_buf, sizeof(mode_buf), "0       ");
    } else {
        snprintf(mode_buf, sizeof(mode_buf), "%-8o", (unsigned)(mode & 0777));
    }
    size_t mlen = strlen(mode_buf);
    memcpy(hdr + 40, mode_buf, mlen > 8 ? 8 : mlen);
    /* 48..57: size (decimal) */
    char sz_buf[16];
    snprintf(sz_buf, sizeof(sz_buf), "%-10zu", size);
    size_t slen = strlen(sz_buf);
    memcpy(hdr + 48, sz_buf, slen > 10 ? 10 : slen);
    /* 58..59: magic terminator */
    hdr[58] = '`';
    hdr[59] = '\n';
}

typedef struct {
    char *name;
    size_t member_idx;
} ZcArSymEntry;

int zc_ar_write(const char *out_path, const ZcArWriteMember *members,
                size_t member_count, bool write_symtab,
                char *err, size_t errsz) {
    /* 1. Classify members and build GNU long-name table if any name > 15 chars */
    char *long_names_buf = NULL;
    size_t long_names_cap = 0;
    size_t long_names_len = 0;
    size_t *name_offsets = (size_t *)calloc(member_count + 1, sizeof(size_t));
    bool *is_long_name = (bool *)calloc(member_count + 1, sizeof(bool));

    if (!name_offsets || !is_long_name) {
        if (err) snprintf(err, errsz, "out of memory");
        free(name_offsets);
        free(is_long_name);
        return -1;
    }

    bool has_long_names = false;
    for (size_t i = 0; i < member_count; i++) {
        const char *bname = get_basename(members[i].name);
        size_t blen = strlen(bname);
        if (blen > 15 || strchr(bname, ' ') != NULL) {
            has_long_names = true;
            is_long_name[i] = true;
            name_offsets[i] = long_names_len;
            /* Append bname + "/\n" */
            size_t needed = long_names_len + blen + 3;
            if (needed > long_names_cap) {
                long_names_cap = needed < 256 ? 256 : needed * 2;
                char *nb = (char *)realloc(long_names_buf, long_names_cap);
                if (!nb) {
                    if (err) snprintf(err, errsz, "out of memory");
                    free(long_names_buf);
                    free(name_offsets);
                    free(is_long_name);
                    return -1;
                }
                long_names_buf = nb;
            }
            memcpy(long_names_buf + long_names_len, bname, blen);
            long_names_len += blen;
            long_names_buf[long_names_len++] = '/';
            long_names_buf[long_names_len++] = '\n';
        }
    }

    /* 2. Extract defined global/weak symbols from ELF members if requested */
    ZcArSymEntry *syms = NULL;
    size_t sym_count = 0;
    size_t sym_cap = 0;
    size_t total_sym_names_len = 0;

    if (write_symtab) {
        for (size_t m = 0; m < member_count; m++) {
            ZcElfObj obj;
            char elf_err[256];
            if (zc_elf_obj_load_mem(members[m].data, members[m].size,
                                    members[m].name, &obj,
                                    elf_err, sizeof(elf_err)) != 0) {
                continue; /* non-ELF member: skip symbol extraction */
            }
            const ZcSection *st = zc_elf_symtab(&obj);
            if (st && st->syms) {
                for (size_t s = 0; s < st->sym_count; s++) {
                    const ZcSymbol *sym = &st->syms[s];
                    /* Export defined global (1) and weak (2) symbols */
                    if ((sym->bind == 1 || sym->bind == 2) &&
                        sym->shndx != 0 && sym->name && sym->name[0] != '\0') {
                        if (sym_count >= sym_cap) {
                            sym_cap = sym_cap == 0 ? 128 : sym_cap * 2;
                            ZcArSymEntry *ns = (ZcArSymEntry *)realloc(syms, sym_cap * sizeof(ZcArSymEntry));
                            if (!ns) {
                                zc_elf_obj_free(&obj);
                                goto oom;
                            }
                            syms = ns;
                        }
                        syms[sym_count].name = strdup(sym->name);
                        syms[sym_count].member_idx = m;
                        total_sym_names_len += strlen(sym->name) + 1;
                        sym_count++;
                    }
                }
            }
            zc_elf_obj_free(&obj);
        }
    }

    /* 3. Compute file offsets */
    size_t cur_offset = AR_MAGIC_LEN;

    /* Symbol table (member "/") */
    size_t symtab_payload_len = 0;
    if (write_symtab && sym_count > 0) {
        symtab_payload_len = 4 + (sym_count * 4) + total_sym_names_len;
        cur_offset += AR_HDR_SIZE + symtab_payload_len + (symtab_payload_len & 1);
    }

    /* Long-names table (member "//") */
    if (has_long_names) {
        cur_offset += AR_HDR_SIZE + long_names_len + (long_names_len & 1);
    }

    /* Member offsets */
    size_t *member_offsets = (size_t *)calloc(member_count + 1, sizeof(size_t));
    if (!member_offsets) goto oom;

    for (size_t i = 0; i < member_count; i++) {
        member_offsets[i] = cur_offset;
        cur_offset += AR_HDR_SIZE + members[i].size + (members[i].size & 1);
    }

    /* 4. Write archive to output file */
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        if (err) snprintf(err, errsz, "cannot open output file %s", out_path);
        free(long_names_buf);
        free(name_offsets);
        free(is_long_name);
        free(member_offsets);
        for (size_t s = 0; s < sym_count; s++) free(syms[s].name);
        free(syms);
        return -1;
    }

    /* Archive magic */
    fwrite(AR_MAGIC, 1, AR_MAGIC_LEN, f);

    /* Symbol table member */
    if (write_symtab && sym_count > 0) {
        char hdr[AR_HDR_SIZE];
        format_ar_header(hdr, "/               ", symtab_payload_len, 0);
        fwrite(hdr, 1, AR_HDR_SIZE, f);

        /* Number of symbols (big-endian) */
        uint8_t count_buf[4];
        write_be32(count_buf, (uint32_t)sym_count);
        fwrite(count_buf, 1, 4, f);

        /* Offset array (big-endian) */
        for (size_t s = 0; s < sym_count; s++) {
            uint8_t off_buf[4];
            uint32_t m_off = (uint32_t)member_offsets[syms[s].member_idx];
            write_be32(off_buf, m_off);
            fwrite(off_buf, 1, 4, f);
        }

        /* String table */
        for (size_t s = 0; s < sym_count; s++) {
            size_t slen = strlen(syms[s].name) + 1;
            fwrite(syms[s].name, 1, slen, f);
        }

        /* Padding if odd */
        if (symtab_payload_len & 1) {
            fputc('\n', f);
        }
    }

    /* Long names table member */
    if (has_long_names) {
        char hdr[AR_HDR_SIZE];
        format_ar_header(hdr, "//              ", long_names_len, 0);
        fwrite(hdr, 1, AR_HDR_SIZE, f);
        fwrite(long_names_buf, 1, long_names_len, f);
        if (long_names_len & 1) {
            fputc('\n', f);
        }
    }

    /* Object members */
    for (size_t i = 0; i < member_count; i++) {
        char hdr[AR_HDR_SIZE];
        char name_field[32];
        if (is_long_name[i]) {
            snprintf(name_field, sizeof(name_field), "/%zu", name_offsets[i]);
        } else {
            const char *bname = get_basename(members[i].name);
            snprintf(name_field, sizeof(name_field), "%s/", bname);
        }
        format_ar_header(hdr, name_field, members[i].size, 0100644);
        fwrite(hdr, 1, AR_HDR_SIZE, f);
        if (members[i].size > 0 && members[i].data) {
            fwrite(members[i].data, 1, members[i].size, f);
        }
        if (members[i].size & 1) {
            fputc('\n', f);
        }
    }

    fclose(f);

    /* Cleanup */
    free(long_names_buf);
    free(name_offsets);
    free(is_long_name);
    free(member_offsets);
    for (size_t s = 0; s < sym_count; s++) free(syms[s].name);
    free(syms);
    return 0;

oom:
    if (err) snprintf(err, errsz, "out of memory");
    free(long_names_buf);
    free(name_offsets);
    free(is_long_name);
    if (syms) {
        for (size_t s = 0; s < sym_count; s++) free(syms[s].name);
        free(syms);
    }
    return -1;
}

int zc_ar_create_from_files(const char *out_path, const char *const *input_files,
                            size_t input_count, bool write_symtab,
                            char *err, size_t errsz) {
    ZcArWriteMember *members = (ZcArWriteMember *)calloc(input_count, sizeof(ZcArWriteMember));
    if (!members && input_count > 0) {
        if (err) snprintf(err, errsz, "out of memory");
        return -1;
    }

    for (size_t i = 0; i < input_count; i++) {
        const char *p = input_files[i];
        FILE *f = fopen(p, "rb");
        if (!f) {
            if (err) snprintf(err, errsz, "cannot open input file %s", p);
            for (size_t k = 0; k < i; k++) free((void *)members[k].data);
            free(members);
            return -1;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        uint8_t *data = NULL;
        if (sz > 0) {
            data = (uint8_t *)malloc((size_t)sz);
            if (!data) {
                if (err) snprintf(err, errsz, "out of memory");
                fclose(f);
                for (size_t k = 0; k < i; k++) free((void *)members[k].data);
                free(members);
                return -1;
            }
            if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
                if (err) snprintf(err, errsz, "short read on %s", p);
                fclose(f);
                free(data);
                for (size_t k = 0; k < i; k++) free((void *)members[k].data);
                free(members);
                return -1;
            }
        }
        fclose(f);
        members[i].name = p;
        members[i].data = data;
        members[i].size = (size_t)(sz > 0 ? sz : 0);
    }

    int rc = zc_ar_write(out_path, members, input_count, write_symtab, err, errsz);
    for (size_t i = 0; i < input_count; i++) {
        free((void *)members[i].data);
    }
    free(members);
    return rc;
}

/* CLI entry point for `zerocc --ar [options] archive-file [files...]` */
int zc_ar_main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s [options] <archive-file> [files...]\n", argv[0]);
        fprintf(stderr, "commands:\n");
        fprintf(stderr, "  rcs <lib.a> <files...>  create or replace members in archive with symbol index\n");
        fprintf(stderr, "  t   <lib.a>             list contents of archive\n");
        fprintf(stderr, "  x   <lib.a>             extract members from archive\n");
        return 1;
    }

    const char *opts_str = argv[1];
    if (opts_str[0] == '-') opts_str++;

    bool opt_r = false;
    bool opt_c = false;
    bool opt_s = false;
    bool opt_t = false;
    bool opt_x = false;

    for (size_t i = 0; opts_str[i]; i++) {
        char c = opts_str[i];
        if (c == 'r') opt_r = true;
        else if (c == 'c') opt_c = true;
        else if (c == 's') opt_s = true;
        else if (c == 't') opt_t = true;
        else if (c == 'x') opt_x = true;
        else if (c == 'D' || c == 'v' || c == 'q') { /* standard no-op flags */ }
        else {
            fprintf(stderr, "zerocc-ar: unrecognized option '%c'\n", c);
            return 1;
        }
    }

    const char *archive_path = argv[2];

    /* Mode: List table of contents */
    if (opt_t) {
        ZcAr ar;
        char err[256];
        if (zc_ar_open(archive_path, &ar, err, sizeof(err)) != 0) {
            fprintf(stderr, "zerocc-ar: %s\n", err);
            return 1;
        }
        for (size_t i = 0; i < ar.member_count; i++) {
            printf("%s\n", ar.members[i].name);
        }
        zc_ar_free(&ar);
        return 0;
    }

    /* Mode: Extract members */
    if (opt_x) {
        ZcAr ar;
        char err[256];
        if (zc_ar_open(archive_path, &ar, err, sizeof(err)) != 0) {
            fprintf(stderr, "zerocc-ar: %s\n", err);
            return 1;
        }
        for (size_t i = 0; i < ar.member_count; i++) {
            FILE *out = fopen(ar.members[i].name, "wb");
            if (!out) {
                fprintf(stderr, "zerocc-ar: cannot write %s\n", ar.members[i].name);
                zc_ar_free(&ar);
                return 1;
            }
            if (ar.members[i].size > 0 && ar.members[i].data) {
                fwrite(ar.members[i].data, 1, ar.members[i].size, out);
            }
            fclose(out);
        }
        zc_ar_free(&ar);
        return 0;
    }

    /* Mode: Create or replace members */
    if (opt_r || opt_c || opt_s || !opt_t) {
        const char *const *inputs = (const char *const *)(argv + 3);
        size_t input_count = (size_t)(argc >= 3 ? argc - 3 : 0);

        /* Default to creating symbol index unless suppressed */
        bool write_symtab = true;

        /* If archive already exists, perform member update / replacement */
        ZcAr existing_ar;
        char ar_err[256];
        bool existing = (zc_ar_open(archive_path, &existing_ar, ar_err, sizeof(ar_err)) == 0);

        if (!existing) {
            if (!opt_c) {
                /* standard ar creates with no warning if c flag is given; without c it prints to stderr */
            }
            char err[256];
            if (zc_ar_create_from_files(archive_path, inputs, input_count,
                                        write_symtab, err, sizeof(err)) != 0) {
                fprintf(stderr, "zerocc-ar: %s\n", err);
                return 1;
            }
            return 0;
        }

        /* Merge existing members with new inputs */
        size_t merged_cap = existing_ar.member_count + input_count;
        ZcArWriteMember *merged = (ZcArWriteMember *)calloc(merged_cap, sizeof(ZcArWriteMember));
        size_t merged_count = 0;

        /* First, copy existing members that are NOT replaced by new inputs */
        for (size_t e = 0; e < existing_ar.member_count; e++) {
            const char *ename = existing_ar.members[e].name;
            bool replaced = false;
            for (size_t n = 0; n < input_count; n++) {
                if (strcmp(ename, get_basename(inputs[n])) == 0) {
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                merged[merged_count].name = ename;
                merged[merged_count].data = existing_ar.members[e].data;
                merged[merged_count].size = existing_ar.members[e].size;
                merged_count++;
            }
        }

        /* Read new input files */
        uint8_t **new_buffers = (uint8_t **)calloc(input_count, sizeof(uint8_t *));
        for (size_t n = 0; n < input_count; n++) {
            FILE *f = fopen(inputs[n], "rb");
            if (!f) {
                fprintf(stderr, "zerocc-ar: cannot open %s\n", inputs[n]);
                for (size_t k = 0; k < n; k++) free(new_buffers[k]);
                free(new_buffers);
                free(merged);
                zc_ar_free(&existing_ar);
                return 1;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            rewind(f);
            uint8_t *data = NULL;
            if (sz > 0) {
                data = (uint8_t *)malloc((size_t)sz);
                if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
                    fprintf(stderr, "zerocc-ar: short read on %s\n", inputs[n]);
                    fclose(f);
                    free(data);
                    for (size_t k = 0; k < n; k++) free(new_buffers[k]);
                    free(new_buffers);
                    free(merged);
                    zc_ar_free(&existing_ar);
                    return 1;
                }
            }
            fclose(f);
            new_buffers[n] = data;
            merged[merged_count].name = inputs[n];
            merged[merged_count].data = data;
            merged[merged_count].size = (size_t)(sz > 0 ? sz : 0);
            merged_count++;
        }

        char write_err[256];
        int rc = zc_ar_write(archive_path, merged, merged_count, write_symtab,
                             write_err, sizeof(write_err));
        if (rc != 0) {
            fprintf(stderr, "zerocc-ar: %s\n", write_err);
        }

        for (size_t n = 0; n < input_count; n++) free(new_buffers[n]);
        free(new_buffers);
        free(merged);
        zc_ar_free(&existing_ar);
        return rc == 0 ? 0 : 1;
    }

    return 0;
}
