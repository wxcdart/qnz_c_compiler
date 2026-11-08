/* #warning directive (C23 6.9.1, GCC extension pre-C23, Sprint 132).
 *
 * `#warning "text"` must emit a warning (the directive's own text as the
 * message) and NOT fail the build; the argument is optional; the directive
 * is consumed (nothing reaches the compiler); and `#pragma GCC diagnostic
 * ignored "-Wcpp"` / `warning "-Wcpp"` toggle it off/on. Previously the
 * directive was silently ignored (no diagnostic at all).
 *
 * The stderr text itself is differential-tested in the c23 gate; this
 * program guards the compile/run behavior under make test (JIT) and the
 * test-opt sweep (JIT + AOT, -O0..-O3).
 */
#pragma GCC diagnostic ignored "-Wcpp"
#warning "suppressed by pragma - must not print, must not fail"
#pragma GCC diagnostic warning "-Wcpp"
#warning "this one prints - must not fail the build"

/* a #warning inside an inactive conditional group must stay silent */
#if 0
#warning "hidden - inactive group"
#endif

int main(void)
{
    /* reaching here proves the directives compiled away cleanly */
    return 0;
}
