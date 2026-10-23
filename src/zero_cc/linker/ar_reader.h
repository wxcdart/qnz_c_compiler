#ifndef ZERO_CC_LINKER_AR_READER_H
#define ZERO_CC_LINKER_AR_READER_H
/*
 * ar_reader.h - Classic `ar` static archive reader for the ZeroCC own-linker
 * (T2).
 *
 * Understands the universal Unix ar format (GNU ar, BSD ar, LLVM ar):
 *   `!<arch>\n` followed by 60-byte member headers + payloads (even-padded).
 * Long-name members (`#1/NNN`) are resolved. Index members (`//`,
 * `__.SYMDEF`, `Sym64 Table`) are skipped.
 *
 * Members are returned as views into the archive buffer (no copy); the
 * caller keeps the buffer alive (zc_ar_free releases both).
 */
#include <stddef.h>
#include <stdint.h>

#define ZC_AR_MAX_MEMBERS 4096

typedef struct {
    char *name;
    const uint8_t *data;
    size_t size;
} ZcArMember;

typedef struct {
    const uint8_t *base;   /* archive file image */
    size_t size;
    char *path;
    ZcArMember *members;
    size_t member_count;
    /* File is mmap/read here and owned by this struct */
    uint8_t *owned;
} ZcAr;

/* 0 on success (member_count may be 0 for an empty archive), -1 + err. */
int zc_ar_open(const char *path, ZcAr *out, char *err, size_t errsz);

void zc_ar_free(ZcAr *a);

#endif /* ZERO_CC_LINKER_AR_READER_H */
