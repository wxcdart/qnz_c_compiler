/* ar_reader.c - classic ar archive reader for the ZeroCC own-linker (T2). */
#include "zero_cc/linker/ar_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define AR_MAGIC "!<arch>\n"

static int parse_dec(const char *s, size_t n, uint64_t *out) {
    uint64_t v = 0;
    int digits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\n' || c == '\r') continue;
        if (!isdigit((unsigned char)c)) return -1;
        if (v > (UINT64_MAX - 9) / 10) return -1;
        v = v * 10 + (uint64_t)(c - '0');
        digits++;
    }
    if (digits == 0) return -1;
    *out = v;
    return 0;
}

int zc_ar_open(const char *path, ZcAr *a, char *err, size_t errsz) {
    memset(a, 0, sizeof(*a));
    a->path = strdup(path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "cannot open archive %s", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, errsz, "cannot seek %s", path);
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 8) {
        snprintf(err, errsz, "%s: not an ar archive (too small)", path);
        fclose(f);
        return -1;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        snprintf(err, errsz, "out of memory");
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        snprintf(err, errsz, "short read on %s", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (memcmp(buf, AR_MAGIC, 8) != 0) {
        snprintf(err, errsz, "%s: not an ar archive (bad magic)", path);
        free(buf);
        return -1;
    }

    a->owned = buf;
    a->base = buf;
    a->size = (size_t)sz;
    a->members = (ZcArMember *)calloc(ZC_AR_MAX_MEMBERS, sizeof(ZcArMember));
    if (!a->members) {
        snprintf(err, errsz, "out of memory");
        free(buf);
        return -1;
    }

    size_t off = 8;
    while (off + 60 <= (size_t)sz) {
        const uint8_t *h = buf + off;
        char name[17];
        memcpy(name, h, 16);
        name[16] = '\0';
        /* Trim trailing spaces/newlines (classic short names are padded). */
        while (name[0] && (name[strlen(name) - 1] == ' ' ||
                           name[strlen(name) - 1] == '\n' ||
                           name[strlen(name) - 1] == '\r')) {
            name[strlen(name) - 1] = '\0';
        }
        uint64_t msize = 0;
        if (parse_dec((const char *)h + 48, 10, &msize) != 0) {
            snprintf(err, errsz, "%s: bad ar member header at offset %zu",
                     path, off);
            return -1;
        }
        if (memcmp(h + 58, "`\n", 2) != 0) {
            snprintf(err, errsz, "%s: bad ar member terminator at offset %zu",
                     path, off);
            return -1;
        }
        size_t payload = (size_t)msize;
        if (off + 60 + payload > (size_t)sz) {
            snprintf(err, errsz, "%s: member %s extends past end of file",
                     path, name);
            return -1;
        }
        const uint8_t *data = buf + off + 60;

        /* Long-name member: "#1/NNN" - the name is the first NNN bytes of
         * the payload, terminated by '/'. */
        char *mname = NULL;
        if (strncmp(name, "#1/", 3) == 0) {
            uint64_t nlen = 0;
            if (parse_dec((const char *)name + 3, (size_t)16 - 3, &nlen) != 0 ||
                nlen == 0 || nlen > payload) {
                snprintf(err, errsz, "%s: bad long-name member", path);
                return -1;
            }
            mname = (char *)malloc(nlen + 1);
            if (!mname) {
                snprintf(err, errsz, "out of memory");
                return -1;
            }
            memcpy(mname, data, nlen);
            mname[nlen] = '\0';
            data += nlen + 1;
            payload -= (size_t)nlen + 1;
        } else {
            /* Skip index / symbol-table members. */
            if (strcmp(name, "//") == 0 || strcmp(name, "/") == 0 ||
                strcmp(name, "__.SYMDEF") == 0 ||
                strcmp(name, "__.SYMDEF SORTED") == 0 ||
                strcmp(name, "Sym64 Table") == 0) {
                off += 60 + payload + (payload & 1);
                continue;
            }
            mname = strdup(name);
            if (!mname) {
                snprintf(err, errsz, "out of memory");
                return -1;
            }
        }

        if (a->member_count >= ZC_AR_MAX_MEMBERS) {
            snprintf(err, errsz, "%s: too many archive members", path);
            free(mname);
            return -1;
        }
        a->members[a->member_count].name = mname;
        a->members[a->member_count].data = data;
        a->members[a->member_count].size = payload;
        a->member_count++;

        off += 60 + payload + (payload & 1);
    }
    return 0;
}

void zc_ar_free(ZcAr *a) {
    if (!a) return;
    if (a->members) {
        for (size_t i = 0; i < a->member_count; i++) {
            free(a->members[i].name);
        }
        free(a->members);
    }
    free(a->owned);
    free(a->path);
    memset(a, 0, sizeof(*a));
}
