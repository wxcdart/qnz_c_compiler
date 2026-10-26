/* zero_rt minimal stdio (Phase 0 scope).
 *
 * Raw, unbuffered: every putchar/printf writes straight to the fd, so
 * fflush is a no-op and there is no stdout/stderr state to flush.
 *
 * Conversions: %d %i %o %u %x %X %c %s %p %f %e %g %%.
 * Flags: '-' (left-justify), '0' (zero pad), '#' (alternate form).
 * Width and precision supported; '*' arguments are NOT (documented).
 *
 * Float formatting uses vendored Ryü (third_party/vendor/ryu):
 *   d2fixed_buffered → correctly-rounded %f
 *   d2exp_buffered   → correctly-rounded %e, and the digit source for %g
 * glibc's %e/%f/%g are all correctly rounded to the requested precision,
 * which is exactly what these two functions produce (Ryü's *shortest*
 * d2s form is a different format family and is not used here).
 */
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include "rt_os.h"
#include "ryu/ryu.h"

/* The first definition of FILE wins; the stdio.h emu declares it opaque. */
struct FILE {
    int fd;
};

static struct FILE rt_stdout_obj = {1};
static struct FILE rt_stderr_obj = {2};
static struct FILE rt_stdin_obj = {0};
FILE *stdout = &rt_stdout_obj;
FILE *stderr = &rt_stderr_obj;
FILE *stdin = &rt_stdin_obj;

/* ------------------------------------------------------------------ */
/* Small string sink                                                   */
/* ------------------------------------------------------------------ */

#define RT_SINK_CAP 4096

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} RtSink;

static void sink_init(RtSink *s, char *buf, size_t cap) {
    s->buf = buf;
    s->len = 0;
    s->cap = cap;
    if (cap) buf[0] = 0;
}

static void sink_put(RtSink *s, char c) {
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len = s->len + 1;
}

static void sink_puts(RtSink *s, const char *str) {
    while (*str) sink_put(s, *str++);
}

/* Emit s centered in `width` columns (left=0 right-justified), with
 * pad_char ' ' or '0'. Returns the columns written. */
static void sink_emit(RtSink *s, const char *core, size_t core_len,
                      int width, int left, char pad) {
    if ((int)core_len >= width) {
        sink_puts(s, core);
        return;
    }
    int padn = width - (int)core_len;
    if (!left) {
        int i;
        for (i = 0; i < padn; i++) sink_put(s, pad);
    }
    {
        size_t i;
        for (i = 0; i < core_len; i++) sink_put(s, core[i]);
    }
    if (left) {
        int i;
        for (i = 0; i < padn; i++) sink_put(s, ' ');
    }
}

/* ------------------------------------------------------------------ */
/* Integer formatting                                                  */
/* ------------------------------------------------------------------ */

static const char RT_DIGITS_LC[] = "0123456789abcdef";
static const char RT_DIGITS_UC[] = "0123456789ABCDEF";

static void u64_chars(char *out, uint64_t v, int base, int upper) {
    const char *dig = upper ? RT_DIGITS_UC : RT_DIGITS_LC;
    char tmp[64];
    int i = 0, j;
    if (v == 0) tmp[i++] = '0';
    while (v) {
        tmp[i++] = dig[v % (uint64_t)base];
        v /= (uint64_t)base;
    }
    for (j = i - 1; j >= 0; j--) *out++ = tmp[j];
    *out = 0;
}

/* ------------------------------------------------------------------ */
/* Float formatting helpers                                            */
/* ------------------------------------------------------------------ */

/* Parse Ryü's "[-]d[.ddd]e[+-]NNN" into digits (no point), count and
 * decimal exponent E such that value = 0.digits x 10^(E+1) = digits x 10^E. */
static int ryu_parse(const char *src, char *digits, int *ndig, int *exp_out) {
    int i = 0, d = 0;
    if (src[0] == '-') return -1; /* sign handled by the caller */
    while (src[i] && src[i] != 'e' && src[i] != 'E') {
        if (src[i] != '.') {
            if (d < 60) digits[d] = src[i];
            d++;
            i++;
        } else {
            i++;
        }
    }
    /* exponent */
    {
        int e = 0;
        int neg = 0;
        if (src[i] == 'e' || src[i] == 'E') i++;
        if (src[i] == '-') { neg = 1; i++; }
        else if (src[i] == '+') i++;
        while (src[i] >= '0' && src[i] <= '9') {
            e = e * 10 + (src[i] - '0');
            i++;
        }
        if (neg) e = -e;
        /* value = D.dddd x 10^e */
        *exp_out = e;
    }
    *ndig = d;
    return 0;
}

/* %g: C rules over Ryü's correctly-rounded significant digits. */
static void fmt_g(RtSink *s, double x, int prec, int alt) {
    char ryu_buf[80];
    char digits[64];
    int ndig, E, i;
    int sig = prec > 0 ? prec : 6;

    /* sign */
    if (x < 0) sink_put(s, '-');

    d2exp_buffered(x < 0 ? -x : x, (uint32_t)(sig - 1), ryu_buf);
    if (ryu_parse(ryu_buf, digits, &ndig, &E) != 0) {
        sink_puts(s, "?");
        return;
    }

    if (E < -4 || E >= sig) {
        /* exponential form: d[.ddd]e±EE, trailing zeros dropped */
        int frac = ndig - 1;
        while (frac > 0 && digits[frac] == '0' && !alt) frac--;
        sink_put(s, digits[0]);
        if (frac > 0 || alt) {
            sink_put(s, '.');
            for (i = 1; i <= frac; i++) sink_put(s, digits[i]);
        }
        sink_put(s, 'e');
        if (E < 0) sink_put(s, '-'); else sink_put(s, '+');
        {
            int ae = E < 0 ? -E : E;
            if (ae >= 100) sink_put(s, '0' + ae / 100);
            ae %= 100;
            sink_put(s, '0' + ae / 10);
            sink_put(s, '0' + ae % 10);
        }
        return;
    }

    /* fixed form: place the point, pad, strip trailing zeros */
    if (E >= 0) {
        int intdig = E + 1; /* digits before the point (padded) */
        for (i = 0; i < intdig; i++)
            sink_put(s, i < ndig ? digits[i] : '0');
        {
            int fracmax = sig - 1 - E;
            int frac = fracmax < 0 ? 0 : fracmax;
            while (frac > 0 && (intdig + frac - 1 >= ndig || digits[intdig + frac - 1] == '0') && !alt)
                frac--;
            if (frac > 0 || alt) {
                sink_put(s, '.');
                for (i = intdig; i < intdig + frac; i++)
                    sink_put(s, i < ndig ? digits[i] : '0');
            }
        }
    } else {
        sink_puts(s, "0.");
        for (i = 0; i < -E - 1; i++) sink_put(s, '0');
        {
            int frac = ndig;
            while (frac > 0 && digits[frac - 1] == '0' && !alt) frac--;
            for (i = 0; i < frac; i++) sink_put(s, digits[i]);
            if (alt && frac == 0) sink_put(s, '0');
        }
    }
}

/* ------------------------------------------------------------------ */
/* Core formatter                                                      */
/* ------------------------------------------------------------------ */

static char rt_scratch[RT_SINK_CAP];

static int vsnprintf_core(char *out, size_t outcap, const char *fmt, va_list ap) {
    RtSink s;
    char core[80];

    sink_init(&s, out ? out : rt_scratch, out ? outcap : RT_SINK_CAP);
    /* When writing into `out`, reserve room for the NUL. */
    if (out && outcap > 0) s.cap = outcap - 1;

    for (; *fmt; fmt++) {
        char c = *fmt;
        if (c != '%') {
            sink_put(&s, c);
            continue;
        }
        fmt++;
        {
            int left = 0, zpad = 0, alt = 0;
            int width = 0;
            int prec = -1;
            while (*fmt == '-' || *fmt == '0' || *fmt == '#') {
                if (*fmt == '-') left = 1;
                else if (*fmt == '0') zpad = 1;
                else alt = 1;
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
            if (*fmt == '.') {
                fmt++;
                prec = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
            /* length modifiers: consume (all our ints are 64-bit) */
            while (*fmt == 'l' || *fmt == 'h') fmt++;

            switch (*fmt) {
            case 'd':
            case 'i': {
                long v = va_arg(ap, long);
                if (v < 0) {
                    sink_put(&s, '-');
                    u64_chars(core, 0 - (uint64_t)v, 10, 0);
                } else {
                    u64_chars(core, (uint64_t)v, 10, 0);
                }
                core[strlen(core)] = 0;
                sink_emit(&s, core, strlen(core), width, left,
                          zpad && !left ? '0' : ' ');
                break;
            }
            case 'u':
                u64_chars(core, (uint64_t)va_arg(ap, long), 10, 0);
                sink_emit(&s, core, strlen(core), width, left,
                          zpad && !left ? '0' : ' ');
                break;
            case 'x':
            case 'X': {
                uint64_t v = (uint64_t)va_arg(ap, long);
                int n;
                u64_chars(core, v, 16, *fmt == 'X');
                n = (int)strlen(core);
                if (alt && v) {
                    memmove(core + 2, core, (size_t)n + 1);
                    core[0] = '0';
                    core[1] = *fmt == 'X' ? 'X' : 'x';
                    n += 2;
                }
                sink_emit(&s, core, (size_t)n, width, left,
                          zpad && !left ? '0' : ' ');
                break;
            }
            case 'o': {
                uint64_t v = (uint64_t)va_arg(ap, long);
                u64_chars(core, v, 8, 0);
                if (alt && v && core[0] != '0') {
                    int n = (int)strlen(core);
                    int i;
                    for (i = n; i >= 0; i--) core[i + 1] = core[i];
                    core[0] = '0';
                }
                sink_emit(&s, core, strlen(core), width, left,
                          zpad && !left ? '0' : ' ');
                break;
            }
            case 'c': {
                int ch = va_arg(ap, int);
                sink_put(&s, (char)ch);
                break;
            }
            case 's': {
                const char *str = va_arg(ap, const char *);
                if (!str) {
                    sink_puts(&s, "(null)");
                } else {
                    size_t sl = strlen(str);
                    if (prec >= 0 && (int)sl > prec) sl = (size_t)prec;
                    sink_emit(&s, str, sl, width, left, ' ');
                }
                break;
            }
            case 'p': {
                void *p = va_arg(ap, void *);
                u64_chars(core + 2, (uint64_t)(uintptr_t)p, 16, 0);
                core[0] = '0';
                core[1] = 'x';
                sink_emit(&s, core, 2 + strlen(core + 2), width, left,
                          zpad && !left ? '0' : ' ');
                break;
            }
            case 'f': {
                double d = va_arg(ap, double);
                int p = prec >= 0 ? prec : 6;
                if (d < 0) sink_put(&s, '-');
                d2fixed_buffered(d < 0 ? -d : d, (uint32_t)p, core);
                sink_emit(&s, core, strlen(core), width, left,
                          zpad && !left ? '0' : ' ');
                break;
            }
            case 'e': {
                double d = va_arg(ap, double);
                int p = prec >= 0 ? prec : 6;
                if (d < 0) sink_put(&s, '-');
                d2exp_buffered(d < 0 ? -d : d, (uint32_t)p, core);
                sink_emit(&s, core, strlen(core), width, left,
                          zpad && !left ? '0' : ' ');
                break;
            }
            case 'g': {
                double d = va_arg(ap, double);
                int p = prec >= 0 ? prec : 6;
                char gcore[80];
                RtSink gs;
                if (d < 0) sink_put(&s, '-');
                sink_init(&gs, gcore, sizeof(gcore));
                fmt_g(&gs, d < 0 ? -d : d, p, alt);
                sink_emit(&s, gcore, gs.len < sizeof(gcore) ? gs.len : sizeof(gcore) - 1,
                          width, left, zpad && !left ? '0' : ' ');
                break;
            }
            case '%':
                sink_put(&s, '%');
                break;
            default:
                sink_put(&s, '%');
                sink_put(&s, *fmt);
                break;
            }
            /* fmt points at the specifier; the loop's fmt++ advances past
             * it. (A second fmt++ here used to drop the character after
             * every conversion: "int: %d str: %s\n" printed "int: 42str: hello".) */
        }
    }
    if (out) out[s.len < outcap ? s.len : outcap - 1] = 0;
    return (int)s.len;
}

static int rt_stream_printf(FILE *stream, const char *format, va_list ap) {
    char out[RT_SINK_CAP];
    int n = vsnprintf_core(out, sizeof out, format, ap);
    if (n > 0) {
        int r = zeroc_rt_write(stream->fd, out, n);
        (void)r;
    }
    return n;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    int n;
    va_start(ap, format);
    n = vsnprintf_core(str, size, format, ap);
    va_end(ap);
    return n;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    int n;
    va_start(ap, format);
    n = vsnprintf_core(str, RT_SINK_CAP + 1, format, ap);
    va_end(ap);
    return n;
}

int printf(const char *format, ...) {
    va_list ap;
    int n;
    va_start(ap, format);
    n = rt_stream_printf(stdout, format, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    int n;
    va_start(ap, format);
    n = rt_stream_printf(stream, format, ap);
    va_end(ap);
    return n;
}

int puts(const char *s) {
    int n = 0;
    while (s[n]) n++;
    zeroc_rt_write(1, s, (uint64_t)n);
    zeroc_rt_write(1, "\n", 1);
    return n + 1;
}

int putchar(int c) {
    char ch = (char)c;
    zeroc_rt_write(1, &ch, 1);
    return c;
}

int fputs(const char *s, FILE *stream) {
    int n = 0;
    while (s[n]) n++;
    if (n) zeroc_rt_write(stream->fd, s, (uint64_t)n);
    return n;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0; /* unbuffered */
}

int getchar(void) {
    char c;
    if (zeroc_rt_read(0, &c, 1) != 1) return -1;
    return (unsigned char)c;
}

int fgetc(FILE *stream) {
    char c;
    if (zeroc_rt_read(stream->fd, &c, 1) != 1) return -1;
    return (unsigned char)c;
}

/* File I/O: Phase 0 does not open files. */
FILE *fopen(const char *pathname, const char *mode) {
    (void)pathname;
    (void)mode;
    return 0;
}

int fclose(FILE *stream) {
    (void)stream;
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr; (void)size; (void)nmemb; (void)stream;
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr; (void)size; (void)nmemb; (void)stream;
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    (void)stream; (void)offset; (void)whence;
    return -1;
}

long ftell(FILE *stream) {
    (void)stream;
    return -1;
}

