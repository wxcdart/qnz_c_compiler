/* zero_rt zero_san report formatting (Sprint 167).
 *
 * Shared by the two __zerocsan_report implementations:
 *   - zero_rt/core/san_report.c      (zero_rt build, zerocc-compiled, all 4
 *     ISAs; emits via zeroc_rt_write(2, ...))
 *   - zero_rt/jit/san_report_host.c  (host-compiled copy linked into the
 *     zerocc driver for --run; emits via the host libc write(2, ...))
 *
 * Deliberately stdio-free: the report must work when zero_rt's own FILE
 * machinery is absent (host-linked AOT builds resolve fprintf/stderr from
 * the system libc, and zero_rt's stdio members must NOT be pulled into
 * those links - they define stdout/stderr/stdin and would collide with the
 * system libc's). The message is formatted into a local buffer and written
 * raw to fd 2.
 */
#ifndef ZERO_RT_SAN_FMT_H
#define ZERO_RT_SAN_FMT_H

#include <stdint.h>

/* Format `v` as a signed decimal into p, return the end pointer. */
static char *san_fmt_long(char *p, long v) {
    unsigned long u;
    if (v < 0) {
        *p++ = '-';
        u = (unsigned long)(-(v + 1)) + 1U;  /* safe for LONG_MIN */
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) { *p++ = '0'; return p; }
    char tmp[24];
    int n = 0;
    while (u != 0) { tmp[n++] = (char)('0' + (u % 10U)); u /= 10U; }
    while (n > 0) *p++ = tmp[--n];
    return p;
}

/* Format `u` as an unsigned decimal into p, return the end pointer. */
static char *san_fmt_uint(char *p, unsigned u) {
    if (u == 0) { *p++ = '0'; return p; }
    char tmp[12];
    int n = 0;
    while (u != 0) { tmp[n++] = (char)('0' + (u % 10U)); u /= 10U; }
    while (n > 0) *p++ = tmp[--n];
    return p;
}

/* Append `s` to p, return the end pointer. */
static char *san_fmt_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

#endif /* ZERO_RT_SAN_FMT_H */
