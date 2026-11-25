/* zero_rt string & memory builtins (the subset the Phase 0 battery needs,
 * plus the few the vendored code and stdio rely on).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dest;

    if (n < 8) {
        size_t i;
        for (i = 0; i < n; i++) d[i] = s[i];
        return dest;
    }

#if defined(__x86_64__) || defined(__aarch64__)
    /* Unaligned access is supported natively in hardware */
    while ((uintptr_t)d & 7) {
        *d++ = *s++;
        n--;
    }
    while (n >= 32) {
        uint64_t w0 = *(const uint64_t *)(s + 0);
        uint64_t w1 = *(const uint64_t *)(s + 8);
        uint64_t w2 = *(const uint64_t *)(s + 16);
        uint64_t w3 = *(const uint64_t *)(s + 24);
        *(uint64_t *)(d + 0) = w0;
        *(uint64_t *)(d + 8) = w1;
        *(uint64_t *)(d + 16) = w2;
        *(uint64_t *)(d + 24) = w3;
        d += 32;
        s += 32;
        n -= 32;
    }
    while (n >= 8) {
        *(uint64_t *)d = *(const uint64_t *)s;
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n > 0) {
        *d++ = *s++;
        n--;
    }
#else
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while ((uintptr_t)d & 7) {
            *d++ = *s++;
            n--;
        }
        while (n >= 32) {
            uint64_t w0 = *(const uint64_t *)(s + 0);
            uint64_t w1 = *(const uint64_t *)(s + 8);
            uint64_t w2 = *(const uint64_t *)(s + 16);
            uint64_t w3 = *(const uint64_t *)(s + 24);
            *(uint64_t *)(d + 0) = w0;
            *(uint64_t *)(d + 8) = w1;
            *(uint64_t *)(d + 16) = w2;
            *(uint64_t *)(d + 24) = w3;
            d += 32;
            s += 32;
            n -= 32;
        }
        while (n >= 8) {
            *(uint64_t *)d = *(const uint64_t *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
    }
    while (n > 0) {
        *d++ = *s++;
        n--;
    }
#endif
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dest;
    if (s < d && (size_t)(d - s) < n) {
        /* forward overlap: copy high to low */
#if defined(__x86_64__) || defined(__aarch64__)
        d += n;
        s += n;
        while (n >= 8) {
            d -= 8;
            s -= 8;
            *(uint64_t *)d = *(const uint64_t *)s;
            n -= 8;
        }
        while (n > 0) {
            d--;
            s--;
            *d = *s;
            n--;
        }
#else
        size_t i;
        for (i = n; i > 0; i--) d[i - 1] = s[i - 1];
#endif
        return dest;
    }
    return memcpy(dest, src, n);
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    if (n == 0) return s;

    uint8_t byte = (uint8_t)c;
    if (n < 8) {
        size_t i;
        for (i = 0; i < n; i++) p[i] = byte;
        return s;
    }

    uint64_t w = byte;
    w |= (w << 8);
    w |= (w << 16);
    w |= (w << 32);

    /* Align p to 8-byte boundary */
    while ((uintptr_t)p & 7) {
        *p++ = byte;
        n--;
    }

    while (n >= 32) {
        *(uint64_t *)(p + 0) = w;
        *(uint64_t *)(p + 8) = w;
        *(uint64_t *)(p + 16) = w;
        *(uint64_t *)(p + 24) = w;
        p += 32;
        n -= 32;
    }

    while (n >= 8) {
        *(uint64_t *)p = w;
        p += 8;
        n -= 8;
    }

    while (n > 0) {
        *p++ = byte;
        n--;
    }
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    if (a == b || n == 0) return 0;

#if defined(__x86_64__) || defined(__aarch64__)
    while (n >= 8) {
        uint64_t w1 = *(const uint64_t *)a;
        uint64_t w2 = *(const uint64_t *)b;
        if (w1 != w2) {
            for (size_t i = 0; i < 8; i++) {
                if (a[i] != b[i]) return (int)a[i] - (int)b[i];
            }
        }
        a += 8;
        b += 8;
        n -= 8;
    }
#else
    if ((((uintptr_t)a | (uintptr_t)b) & 7) == 0) {
        while (n >= 8) {
            uint64_t w1 = *(const uint64_t *)a;
            uint64_t w2 = *(const uint64_t *)b;
            if (w1 != w2) {
                for (size_t i = 0; i < 8; i++) {
                    if (a[i] != b[i]) return (int)a[i] - (int)b[i];
                }
            }
            a += 8;
            b += 8;
            n -= 8;
        }
    }
#endif

    while (n > 0) {
        if (*a != *b) return (int)*a - (int)*b;
        a++;
        b++;
        n--;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *s_orig = s;

    /* Align s to 8-byte boundary */
    while ((uintptr_t)s & 7) {
        if (!*s) return (size_t)(s - s_orig);
        s++;
    }

    const uint64_t *p64 = (const uint64_t *)s;
    const uint64_t ones = 0x0101010101010101ULL;
    const uint64_t mask = 0x8080808080808080ULL;
    for (;;) {
        uint64_t w = *p64;
        /* Check if any byte in 64-bit word w is zero */
        if (((w - ones) & ~w & mask) != 0) {
            const char *cp = (const char *)p64;
            if (!cp[0]) return (size_t)(cp - s_orig);
            if (!cp[1]) return (size_t)(cp - s_orig + 1);
            if (!cp[2]) return (size_t)(cp - s_orig + 2);
            if (!cp[3]) return (size_t)(cp - s_orig + 3);
            if (!cp[4]) return (size_t)(cp - s_orig + 4);
            if (!cp[5]) return (size_t)(cp - s_orig + 5);
            if (!cp[6]) return (size_t)(cp - s_orig + 6);
            return (size_t)(cp - s_orig + 7);
        }
        p64++;
    }
}

char *strcpy(char *dest, const char *src) {
    size_t len = strlen(src);
    memcpy(dest, src, len + 1);
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = 0;
    return dest;
}

char *strcat(char *dest, const char *src) {
    size_t dl = strlen(dest);
    size_t i = 0;
    while (src[i]) {
        dest[dl + i] = src[i];
        i++;
    }
    dest[dl + i] = 0;
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    size_t dl = strlen(dest);
    size_t i = 0;
    while (i < n && src[i]) {
        dest[dl + i] = src[i];
        i++;
    }
    dest[dl + i] = 0;
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    size_t i = 0;
    while (s1[i] && s1[i] == s2[i]) i++;
    return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
}

int strncmp(const char *s1, const char *s2, size_t n) {
    size_t i = 0;
    for (i = 0; i < n; i++) {
        if (s1[i] != s2[i])
            return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
        if (!s1[i]) return 0;
    }
    return 0;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

char *strrchr(const char *s, int c) {
    char *last = 0;
    for (;; s++) {
        if (*s == (char)c) last = (char *)s;
        if (!*s) return last;
    }
}

char *strstr(const char *haystack, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return (char *)haystack;
    for (;; haystack++) {
        if (!*haystack) return 0;
        if (strncmp(haystack, needle, nl) == 0) return (char *)haystack;
    }
}

int abs(int j) { return j < 0 ? -j : j; }

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!str) str = *saveptr;
    if (!str) return NULL;
    while (*str && strchr(delim, *str)) str++;
    if (!*str) {
        *saveptr = NULL;
        return NULL;
    }
    char *token = str;
    while (*str && !strchr(delim, *str)) str++;
    if (*str) {
        *str = '\0';
        *saveptr = str + 1;
    } else {
        *saveptr = NULL;
    }
    return token;
}

char *strtok(char *str, const char *delim) {
    static char *static_save = NULL;
    return strtok_r(str, delim, &static_save);
}

char *realpath(const char *path, char *resolved_path) {
    if (!path) return NULL;
    char *out = resolved_path;
    if (!out) {
        out = (char *)malloc(1024);
        if (!out) return NULL;
    }
    size_t len = strlen(path);
    if (len >= 1024) len = 1023;
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

static void zc_swap_bytes(char *a, char *b, size_t size) {
    for (size_t i = 0; i < size; i++) {
        char t = a[i]; a[i] = b[i]; b[i] = t;
    }
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (!base || nmemb < 2 || size == 0) return;
    char *arr = (char *)base;
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0 && compar(arr + (j - 1) * size, arr + j * size) > 0; j--) {
            zc_swap_bytes(arr + (j - 1) * size, arr + j * size, size);
        }
    }
}
