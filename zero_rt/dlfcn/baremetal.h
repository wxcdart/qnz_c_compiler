/* zero_rt/dlfcn/baremetal.h — Freestanding & Baremetal Symbol Engine (Phase 6).
 *
 * In freestanding and embedded environments without an OS, filesystems,
 * or MMU virtual memory, dynamic code loading requires:
 *   1. A flash/ROM-resident symbol dictionary (BaremetalSym[]).
 *   2. In-memory symbol lookup (zero_rt_baremetal_lookup).
 *   3. Architecture-specific instruction cache invalidation:
 *        - RISC-V: fence.i
 *        - AArch64 / ARM: isb (instruction synchronization barrier)
 *        - x86-64: mfence / memory barrier
 */

#ifndef ZERO_RT_DLFCN_BAREMETAL_H
#define ZERO_RT_DLFCN_BAREMETAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flash or RAM symbol entry */
typedef struct {
    const char *name;
    void       *addr;
} BaremetalSym;

/* Registered firmware / baremetal symbol table */
static const BaremetalSym *g_baremetal_symtab = NULL;
static size_t              g_baremetal_symcount = 0;

/* Register a firmware symbol table */
static inline void zero_rt_baremetal_register_symbols(const BaremetalSym *table, size_t count) {
    g_baremetal_symtab = table;
    g_baremetal_symcount = count;
}

/* Look up a symbol by name in the registered baremetal symbol table */
static inline void *zero_rt_baremetal_lookup(const char *name) {
    if (!name || !g_baremetal_symtab || g_baremetal_symcount == 0) {
        return NULL;
    }
    for (size_t i = 0; i < g_baremetal_symcount; i++) {
        if (strcmp(g_baremetal_symtab[i].name, name) == 0) {
            return g_baremetal_symtab[i].addr;
        }
    }
    return NULL;
}

/* Invalidate CPU instruction cache and serialize instruction pipeline */
static inline void zero_rt_cache_flush(void *addr, size_t len) {
    (void)addr;
    (void)len;
#if defined(__riscv)
    __asm__ volatile("fence.i" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile("isb" ::: "memory");
#elif defined(__arm__)
    __asm__ volatile("isb" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("" ::: "memory");
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* ZERO_RT_DLFCN_BAREMETAL_H */
