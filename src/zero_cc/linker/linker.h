#ifndef ZERO_CC_LINKER_LINKER_H
#define ZERO_CC_LINKER_LINKER_H
/*
 * linker.h - ZeroCC's own static linker (T2).
 *
 * Links ZeroCC-emit ET_REL x86-64 objects (optionally pulled from classic
 * `ar` archives) into a directly-executable static ET_EXEC:
 *
 *   zero_cc_link_exec(out, objects, archives, err)
 *
 * Pipeline:
 *   1. load objects; extract archive members that resolve undefined symbols
 *      (greedy to fixpoint, the classic archive algorithm)
 *   2. resolve global symbols (strong/weak, commons, section symbols;
 *      duplicate strong definitions are a multiple-definition error)
 *   3. lay out sections (.text/.rodata/.data/.bss) at a fixed base with
 *      per-section alignment; split into an RX and an RW PT_LOAD
 *   4. apply relocations: R_X86_64_{64,PC32,PLT32->PC32,32,32S,RELATIVE}
 *   5. emit ET_EXEC (2 program headers, rebuilt .symtab/.strtab/.shstrtab)
 *
 * Entry point: `_start` if the inputs define one (a CRT object), otherwise
 * `main` (ZeroCC programs use the no-argument main). This is what makes the
 * own linker the first customer of zero_rt: no system libc, no crt files.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* objects/archives: input file paths (.o / .a). Either list may be empty,
 * not both. Returns 0 on success, -1 with a message in err. */
int zero_cc_link_exec(const char *out_path,
                      const char *const *objects, int object_count,
                      const char *const *archives, int archive_count,
                      char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_CC_LINKER_LINKER_H */
