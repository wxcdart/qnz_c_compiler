#ifndef ZERO_RT_DLF_CN_DYNLINK_H
#define ZERO_RT_DLF_CN_DYNLINK_H

/* zero_rt/dlfcn/dynlink.h — Host-side in-process dynamic loader (Phase 2).
 *
 * Public API for zero_rt/dlfcn/dynlink.c, a minimal dynamic linker that
 * loads real .so files into the static-musl host process (build/bin/zerocc
 * and host test binaries).  Compiled by the host C compiler, linked via
 * ZC_DYNLINK_OBJ.  NOT part of per-target zero_rt.a archives.
 *
 * x86-64 ELF64 only in the initial MVP.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load a shared object (real .so on disk).  filename may be NULL (the main
 * executable namespace) or a path.  Returns an opaque handle or NULL with
 * errno set.  Auto-initializes the dynlink subsystem on first call. */
void *zc_dlopen(const char *filename, int flags);

/* Resolve a symbol within a handle's scope. */
void *zc_dlsym(void *handle, const char *name);

/* Resolve a symbol within a handle's scope matching a specific GNU version.
 * If version is NULL, behaves like zc_dlsym (resolves the default version). */
void *zc_dlvsym(void *handle, const char *name, const char *version);

/* Resolve next symbol occurrence starting after caller_pc's DSO (RTLD_NEXT). */
void *zc_dlsym_next(const void *caller_pc, const char *name);
void *zc_dlvsym_next(const void *caller_pc, const char *name, const char *version);

/* Reverse lookup: find DSO and closest symbol containing addr.
 * Returns 1 on match, 0 if addr not found in any loaded DSO.
 * info is cast to Dl_info*. */
int zc_dladdr(const void *addr, void *info);

/* Decrement a handle's refcount; unmaps and runs destructors on 0. */
int zc_dlclose(void *handle);

/* Bootstrap the dynlink subsystem: create the synthetic head DSO
 * representing the main executable.  Called lazily on first use, but may
 * be called explicitly.  Returns 0 on success. */
int zero_rt_dl_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_RT_DLF_CN_DYNLINK_H */
