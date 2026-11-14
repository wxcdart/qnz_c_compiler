/* zero_rt string & memory builtins (the subset the Phase 0 battery needs,
 * plus the few the vendored code and stdio rely on).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dest;
    if (s < d) {
        /* forward overlap: copy high to low */
        size_t i;
        for (i = n; i > 0; i--) d[i - 1] = s[i - 1];
    } else {
        size_t i;
        for (i = 0; i < n; i++) d[i] = s[i];
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    size_t i;
    for (i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char *strcpy(char *dest, const char *src) {
    size_t i = 0;
    while (src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = 0;
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
