/* Predefined macros: __STDC__, __STDC_VERSION__, __LINE__ (macro-body
 * __LINE__ expands at the call site), __FILE__ (path normalized to
 * <FILE> by the gate), __USER_LABEL_PREFIX__ (empty on x86-64). */
int v1 = __STDC__;
#if __STDC_VERSION__ == 201112L
int v2 = 1;
#endif
const char *f = __FILE__;
int l1 = __LINE__;
int l2 = __LINE__;
#define LN(x) x
int l3 = LN(__LINE__);
#define MLB(x) x + \
               1
int l4 = MLB(__LINE__);
int l5 = __LINE__;
const char *ulp = __USER_LABEL_PREFIX__;
int l6 = __LINE__;
