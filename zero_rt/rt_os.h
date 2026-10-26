/* zero_rt internal OS-boundary layer.
 *
 * Every OS operation goes through the __zerocc_syscall builtin (the only
 * machine-code surface of the runtime; everything above this layer is
 * plain C compiled by ZeroCC itself).
 *
 * Per-ISA syscall tables: the Makefile defines RT_TARGET_AARCH64 or
 * RT_TARGET_RISCV when cross-building zero_rt.a for those targets; the
 * default (no define) is the x86-64 table.
 */
#ifndef ZERO_RT_OS_H
#define ZERO_RT_OS_H

#include <stdint.h>

#ifndef RT_TARGET_AARCH64
#if defined(__aarch64__)
#define RT_TARGET_AARCH64 1
#else
#define RT_TARGET_AARCH64 0
#endif
#endif
#ifndef RT_TARGET_RISCV
#if defined(__riscv)
#define RT_TARGET_RISCV 1
#else
#define RT_TARGET_RISCV 0
#endif
#endif

#if RT_TARGET_AARCH64
/* Linux aarch64 syscall numbers. */
#define RT_SYS_READ        63
#define RT_SYS_WRITE       64
#define RT_SYS_BRK         214
#define RT_SYS_GETPID      172
#define RT_SYS_EXIT_GROUP  94
#define RT_SYS_MMAP        222
#define RT_HAS_BRK         1
#elif RT_TARGET_RISCV
/* Linux RISC-V (rv32 and rv64 share the syscall table). */
#define RT_SYS_READ        63
#define RT_SYS_WRITE       64
#define RT_SYS_BRK         214
#define RT_SYS_GETPID      172
#define RT_SYS_EXIT_GROUP  94
#define RT_HAS_BRK         1
#else
/* Linux x86-64 syscall numbers. */
#define RT_SYS_READ        0
#define RT_SYS_WRITE       1
#define RT_SYS_BRK         12
#define RT_SYS_GETPID      39
#define RT_SYS_EXIT_GROUP  231
#define RT_HAS_BRK         1
#endif

/* mmap(2) constants (the values are identical on all three targets). */
#define RT_PROT_READ      1
#define RT_PROT_WRITE     2
#define RT_MAP_PRIVATE    0x02
#define RT_MAP_ANONYMOUS  0x20

int      zeroc_rt_read(int fd, void *buf, uint64_t count);
int      zeroc_rt_write(int fd, const void *buf, uint64_t count);
uint64_t zeroc_rt_brk(uint64_t addr);      /* 0 = query current break */
int      zeroc_rt_getpid(void);
void     zeroc_rt_exit_group(int code);    /* does not return */

#endif /* ZERO_RT_OS_H */
