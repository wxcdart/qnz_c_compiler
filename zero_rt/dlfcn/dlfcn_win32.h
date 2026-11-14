/* zero_rt/dlfcn/dlfcn_win32.h — Windows Win32 <dlfcn.h> Emulation Layer (Phase 6).
 *
 * Implements POSIX dlfcn primitives (dlopen, dlsym, dlvsym, dlclose, dlerror,
 * dladdr) on Windows platforms using native Win32 APIs:
 *   dlopen(path, flags) -> LoadLibraryA(path)
 *   dlsym(handle, name) -> GetProcAddress(handle, name)
 *   dlclose(handle)     -> FreeLibrary(handle)
 *   dlerror()           -> FormatMessageA(GetLastError())
 *   dladdr(addr, info)  -> GetModuleHandleExA + GetModuleFileNameA
 */

#ifndef ZERO_RT_DLFCN_WIN32_H
#define ZERO_RT_DLFCN_WIN32_H

#if defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__) || defined(__MINGW32__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RTLD_LAZY
#define RTLD_LAZY   1
#endif
#ifndef RTLD_NOW
#define RTLD_NOW    2
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0x100
#endif
#ifndef RTLD_LOCAL
#define RTLD_LOCAL  0
#endif
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)0)
#endif
#ifndef RTLD_NEXT
#define RTLD_NEXT    ((void *)-1l)
#endif

typedef struct {
    const char *dli_fname;
    void       *dli_fbase;
    const char *dli_sname;
    void       *dli_saddr;
} Dl_info;

static char g_win32_dlerror_buf[512];
static int  g_win32_has_error = 0;

static inline void zc_win32_set_dlerror(const char *msg) {
    if (msg) {
        snprintf(g_win32_dlerror_buf, sizeof(g_win32_dlerror_buf), "%s", msg);
        g_win32_has_error = 1;
    } else {
        DWORD err = GetLastError();
        if (err == 0) {
            g_win32_has_error = 0;
            return;
        }
        DWORD len = FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            g_win32_dlerror_buf, sizeof(g_win32_dlerror_buf) - 1, NULL);
        if (len > 0) {
            /* Trim trailing CRLF */
            while (len > 0 && (g_win32_dlerror_buf[len - 1] == '\r' ||
                               g_win32_dlerror_buf[len - 1] == '\n')) {
                g_win32_dlerror_buf[--len] = '\0';
            }
            g_win32_has_error = 1;
        } else {
            snprintf(g_win32_dlerror_buf, sizeof(g_win32_dlerror_buf),
                     "Win32 error code %lu", (unsigned long)err);
            g_win32_has_error = 1;
        }
    }
}

static inline void *zc_win32_dlopen(const char *filename, int flags) {
    (void)flags;
    if (!filename || !filename[0]) {
        HMODULE h = GetModuleHandleA(NULL);
        if (!h) {
            zc_win32_set_dlerror(NULL);
            return NULL;
        }
        g_win32_has_error = 0;
        return (void *)h;
    }

    HMODULE h = LoadLibraryA(filename);
    if (!h) {
        zc_win32_set_dlerror(NULL);
        return NULL;
    }
    g_win32_has_error = 0;
    return (void *)h;
}

static inline void *zc_win32_dlsym(void *handle, const char *name) {
    if (!name) return NULL;
    HMODULE h = (HMODULE)handle;
    if (!h || handle == RTLD_DEFAULT) {
        h = GetModuleHandleA(NULL);
    }
    FARPROC proc = GetProcAddress(h, name);
    if (!proc) {
        zc_win32_set_dlerror(NULL);
        return NULL;
    }
    g_win32_has_error = 0;
    return (void *)(uintptr_t)proc;
}

static inline void *zc_win32_dlvsym(void *handle, const char *name, const char *version) {
    (void)version; /* Win32 DLLs do not feature GNU ELF version sets */
    return zc_win32_dlsym(handle, name);
}

static inline int zc_win32_dlclose(void *handle) {
    if (!handle) return 0;
    HMODULE self = GetModuleHandleA(NULL);
    if ((HMODULE)handle == self) {
        return 0;
    }
    if (!FreeLibrary((HMODULE)handle)) {
        zc_win32_set_dlerror(NULL);
        return -1;
    }
    g_win32_has_error = 0;
    return 0;
}

static inline char *zc_win32_dlerror(void) {
    if (!g_win32_has_error) return NULL;
    g_win32_has_error = 0;
    return g_win32_dlerror_buf;
}

static inline int zc_win32_dladdr(const void *addr, Dl_info *info) {
    if (!addr || !info) return 0;
    HMODULE hModule = NULL;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &hModule)) {
        return 0;
    }
    static char path_buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(hModule, path_buf, sizeof(path_buf));
    if (len == 0) return 0;

    info->dli_fname = path_buf;
    info->dli_fbase = (void *)hModule;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */

#endif /* ZERO_RT_DLFCN_WIN32_H */
