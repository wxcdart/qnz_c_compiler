/**
 * @file foreign_loader.h
 * @brief In-Process Execution Loader for PE32+ and Mach-O 64-bit Binaries (Sprint 261)
 *
 * Provides Linux-host in-process execution for Windows PE32+ (x86-64) and
 * macOS Mach-O 64-bit (x86-64) executables without external emulators (Wine/Darwin):
 * - Memory mapping (mmap) respecting virtual addresses and section alignments
 * - Memory protection setup (PROT_READ, PROT_WRITE, PROT_EXEC)
 * - PE IAT import binding with Microsoft x64 (ms_abi) calling convention shims
 * - Mach-O load command traversal, segment mapping, and dyld symbol binding
 * - Entry point dispatch and exit status capture
 */

#ifndef ZERO_CC_LINKER_FOREIGN_LOADER_H
#define ZERO_CC_LINKER_FOREIGN_LOADER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FOREIGN_FMT_UNKNOWN = 0,
    FOREIGN_FMT_PE,
    FOREIGN_FMT_MACHO,
} ZeroCCForeignFormat;

/**
 * Detect the binary format of a raw file or memory buffer.
 */
ZeroCCForeignFormat zero_cc_detect_foreign_format(const uint8_t *data, size_t size);

/**
 * Load and execute a Windows PE32+ (x86-64) executable buffer in-process.
 * Returns the process exit code, or negative error code on loader failure.
 */
int zero_cc_run_pe_mem(const uint8_t *pe_data, size_t pe_size, int argc, char **argv);

/**
 * Load and execute a Windows PE32+ (x86-64) executable file in-process.
 */
int zero_cc_run_pe_file(const char *filepath, int argc, char **argv);

/**
 * Load and execute a macOS Mach-O 64-bit (x86-64) executable buffer in-process.
 * Returns the process exit code, or negative error code on loader failure.
 */
int zero_cc_run_macho_mem(const uint8_t *macho_data, size_t macho_size, int argc, char **argv);

/**
 * Load and execute a macOS Mach-O 64-bit (x86-64) executable file in-process.
 */
int zero_cc_run_macho_file(const char *filepath, int argc, char **argv);

/**
 * Check if the given file exists and has PE or Mach-O binary format.
 */
bool zero_cc_is_foreign_binary(const char *filepath);

/**
 * Auto-detect and execute either a PE32+ or Mach-O executable file in-process.
 */
int zero_cc_run_foreign_file(const char *filepath, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_CC_LINKER_FOREIGN_LOADER_H */
