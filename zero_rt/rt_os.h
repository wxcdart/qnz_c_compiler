/* zero_rt internal OS-boundary layer.
 *
 * Every OS operation goes through the __zerocc_syscall builtin (the only
 * machine-code surface of the runtime; everything above this layer is
 * plain C compiled by ZeroCC itself).
 */
#ifndef ZERO_RT_OS_H
#define ZERO_RT_OS_H

#include <stdint.h>

/* Linux x86-64 syscall numbers (the per-OS "number table" of the
 * zero-rt plan: the only constants that change between targets). */
#define RT_SYS_READ        0
#define RT_SYS_WRITE       1
#define RT_SYS_BRK         12
#define RT_SYS_GETPID      39
#define RT_SYS_EXIT_GROUP  231

int      zeroc_rt_read(int fd, void *buf, uint64_t count);
int      zeroc_rt_write(int fd, const void *buf, uint64_t count);
uint64_t zeroc_rt_brk(uint64_t addr);      /* 0 = query current break */
int      zeroc_rt_getpid(void);
void     zeroc_rt_exit_group(int code);    /* does not return */

#endif /* ZERO_RT_OS_H */
