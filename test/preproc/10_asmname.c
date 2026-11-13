/* The glibc/__ASMNAME symbol-name idiom (c-torture execute/builtins
 * {strstr,memops}-asm): prefix is the (empty on x86-64) object macro
 * __USER_LABEL_PREFIX__; it is used in a NORMAL position, so it IS
 * pre-expanded to empty, and STRING() then stringifies the empty arg. */
#define STRING(x) #x
#define ASMNAME(cname) ASMNAME2(__USER_LABEL_PREFIX__, cname)
#define ASMNAME2(prefix, cname) STRING(prefix) cname
const char *n1 = ASMNAME(my_strstr);
const char *n2 = ASMNAME(my_memset);
#define NAME2(a, b) a##b
const char *n3 = NAME2(__USER_LABEL_PREFIX__, sym);
const char *n4 = NAME2(my_, sym);
