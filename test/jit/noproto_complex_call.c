/* Sprint 100: complex arguments passed to a call through a no-prototype
 * (old-style) declaration.  `void foo ();` tells the caller nothing about
 * the parameter types; the _Complex argument must still be marshalled as
 * a complex value - C99 6.5.2.2 default argument promotions keep
 * _Complex T complex (they never drop the imaginary part).  The
 * real-part-extraction path (complex -> scalar conversion, C99 6.3.1.7)
 * must only apply when the declaration actually names a NON-complex
 * parameter at that position.  Regression for c-torture
 * execute/20010605-2.  Compiled with zerocc (AOT + JIT) and host gcc;
 * expected exit code 0. */

void foo ();
void bar ();
void baz ();

int main(void)
{
    __complex__ double x;
    __complex__ float y;
    __complex__ long double z;

    __real__ x = 1.0;
    __imag__ x = 2.0;
    foo (x);

    __real__ y = 3.0f;
    __imag__ y = 4.0f;
    bar (y);

    __real__ z = 5.0L;
    __imag__ z = 6.0L;
    baz (z);

    return 0;
}

void foo (__complex__ double x)
{
    if (__real__ x != 1.0 || __imag__ x != 2.0)
        __builtin_abort ();
}

void bar (__complex__ float x)
{
    if (__real__ x != 3.0f || __imag__ x != 4.0f)
        __builtin_abort ();
}

void baz (__complex__ long double x)
{
    if (__real__ x != 5.0L || __imag__ x != 6.0L)
        __builtin_abort ();
}
