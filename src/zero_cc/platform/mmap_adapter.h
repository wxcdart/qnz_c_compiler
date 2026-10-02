#ifndef ZERO_MMAP_ADAPTER_H
#define ZERO_MMAP_ADAPTER_H

#if defined(_WIN32)
#include <windows.h>

static inline size_t zero_get_page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize ? si.dwPageSize : 4096;
}

static inline void* zero_mmap_alloc_rw(size_t size) {
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static inline int zero_mmap_make_rx(void *addr, size_t size) {
    DWORD old_protect;
    return VirtualProtect(addr, size, PAGE_EXECUTE_READ, &old_protect) ? 0 : -1;
}

static inline int zero_mmap_make_ro(void *addr, size_t size) {
    DWORD old_protect;
    return VirtualProtect(addr, size, PAGE_READONLY, &old_protect) ? 0 : -1;
}

static inline void zero_mmap_free(void *addr, size_t size) {
    (void)size;
    if (addr) {
        VirtualFree(addr, 0, MEM_RELEASE);
    }
}

#else
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

static inline size_t zero_get_page_size(void) {
    long sz = sysconf(_SC_PAGESIZE);
    return (sz > 0) ? (size_t)sz : 4096;
}

static inline void* zero_mmap_alloc_rw(size_t size) {
    /* Allocate readable/writable private anonymous memory */
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

static inline int zero_mmap_make_rx(void *addr, size_t size) {
    /* Change protection to read/execute */
    return (mprotect(addr, size, PROT_READ | PROT_EXEC) == 0) ? 0 : -1;
}

static inline int zero_mmap_make_ro(void *addr, size_t size) {
    /* Change protection to read-only */
    return (mprotect(addr, size, PROT_READ) == 0) ? 0 : -1;
}

static inline void zero_mmap_free(void *addr, size_t size) {
    if (addr) {
        munmap(addr, size);
    }
}

#endif

#endif /* ZERO_MMAP_ADAPTER_H */
