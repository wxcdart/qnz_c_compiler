#ifndef ZERO_CC_LINKER_AR_WRITER_H
#define ZERO_CC_LINKER_AR_WRITER_H
/*
 * ar_writer.h - Classic Unix/GNU static archive writer for ZeroCC.
 *
 * Creates deterministic GNU-format static archives (.a) with:
 *   - Universal header `!<arch>\n`
 *   - GNU symbol index table (`/` member) with ELF symbol indexing
 *   - GNU long-name string table (`//` member) for filenames > 15 characters
 *   - 60-byte member headers with deterministic timestamp/uid/gid/mode
 *   - 2-byte alignment padding
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *name;       /* e.g. "os.o" or file path (basename will be extracted) */
    const uint8_t *data;    /* member file payload */
    size_t size;            /* member size in bytes */
} ZcArWriteMember;

/* Create an ar archive at out_path from in-memory member representations.
 * Returns 0 on success, -1 on failure with error message written to err. */
int zc_ar_write(const char *out_path, const ZcArWriteMember *members,
                size_t member_count, bool write_symtab,
                char *err, size_t errsz);

/* Create an ar archive at out_path by reading input object files from disk.
 * Returns 0 on success, -1 on failure with error message written to err. */
int zc_ar_create_from_files(const char *out_path, const char *const *input_files,
                            size_t input_count, bool write_symtab,
                            char *err, size_t errsz);

/* CLI entry point for `zerocc --ar <flags> <archive> <members...>`.
 * Returns 0 on success, non-zero on error. */
int zc_ar_main(int argc, char **argv);

#endif /* ZERO_CC_LINKER_AR_WRITER_H */
