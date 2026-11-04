/* freopen / file-I/O round-trip (Sprint: unblock SKIP-TARGET:unwrapped).
 *
 * The stdio.h virtual header must declare `freopen` with its real
 * `FILE *` return type.  When it was missing, `freopen` got an implicit
 * `int` declaration (gnu89), so the 64-bit `FILE *` return was truncated to
 * 32 bits at the call site - a garbage FILE pointer that segfaults on first
 * use.  This round-trips a file through freopen (write) + fopen/fscanf
 * (read) + remove, matching c-torture execute/printf-2.c and
 * execute/user-printf.c (the two SKIP-TARGET:unwrapped tests).
 *
 * Compiled with host gcc (make test) and zerocc -O0..-O3 (make test-opt);
 * expected exit code 0.  All output goes to the temp file, so stdout stays
 * clean for the test harness. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main (void)
{
    const char *path = "/tmp/zerocc_freopen_rt";
    char buf[64] = "";

    /* freopen redirects an existing stream (stdout) to a file.  The return
     * value must be a valid FILE * (the regression: implicit-int truncation). */
    FILE *f = freopen (path, "w", stdout);
    if (!f)
        return 1;
    printf ("hello ");
    printf ("%d", 42);
    printf ("%s", " world");
    printf ("\n");
    fclose (f);

    f = fopen (path, "r");
    if (!f)
        return 2;
    if (fscanf (f, "%s", buf) != 1)
        {
            fclose (f);
            remove (path);
            return 3;
        }
    fclose (f);
    remove (path);

    return strcmp (buf, "hello") == 0 ? 0 : 4;
}
