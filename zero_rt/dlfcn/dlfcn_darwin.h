/* zero_rt/dlfcn/dlfcn_darwin.h — macOS (Darwin) Dynamic Loader & W^X Layer (Phase 6).
 *
 * Implements Darwin dynamic loader integration over libSystem.B.dylib,
 * including Apple Silicon W^X page protection (pthread_jit_write_protect_np)
 * and executable discovery via _NSGetExecutablePath().
 */

#ifndef ZERO_RT_DLFCN_DARWIN_H
#define ZERO_RT_DLFCN_DARWIN_H

#if defined(__APPLE__)

#include <dlfcn.h>
#include <pthread.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Apple Silicon W^X page protection control:
 * On macOS on Apple Silicon (arm64), memory pages mapped with MAP_JIT
 * cannot be simultaneously writable and executable.
 * Calling zero_rt_darwin_jit_write_protect(0) enables writing.
 * Calling zero_rt_darwin_jit_write_protect(1) restores execution mode. */
static inline void zero_rt_darwin_jit_write_protect(int enable) {
#if defined(__aarch64__)
    pthread_jit_write_protect_np(enable);
#else
    (void)enable;
#endif
}

/* Retrieve the path to the running executable image using Darwin dyld API. */
static inline int zero_rt_darwin_get_exe_path(char *buf, uint32_t *size) {
    if (!buf || !size) return -1;
    return _NSGetExecutablePath(buf, size);
}

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */

#endif /* ZERO_RT_DLFCN_DARWIN_H */
