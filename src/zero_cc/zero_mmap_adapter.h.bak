#ifndef ZERO_MMAP_ADAPTER_H
#define ZERO_MMAP_ADAPTER_H

#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

static inline void* zero_mmap_alloc_rw(size_t size) {
    /* Allocate readable/writable private anonymous memory */
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

static inline int zero_mmap_make_rx(void *addr, size_t size) {
    /* Change protection to read/execute */
    if (mprotect(addr, size, PROT_READ | PROT_EXEC) == 0) {
        return 0;
    }
    return -1;
}

static inline void zero_mmap_free(void *addr, size_t size) {
    if (addr) {
        munmap(addr, size);
    }
}

#endif /* ZERO_MMAP_ADAPTER_H */
