/* Inlined function with parameter name that shadows a caller variable/param.
 * In Sprint 127/128, lowering inlined p->inner.val erroneously resolved `p`
 * to caller's local `p` because local_lookup_bound was not 0 during inlining. */
#include <stdio.h>

struct Inner {
    int val;
};

struct Outer {
    int tag;
    struct Inner inner;
};

static inline int get_inner_val(struct Outer *p) {
    return p->inner.val;
}

static inline int get_tag(struct Outer *p) {
    return p->tag;
}

int caller_fn(struct Outer *p, struct Outer *q) {
    /* Caller has parameter 'p', but passes 'q' to inlined callee whose param is also 'p'. */
    int t = get_tag(q);
    int v = get_inner_val(q);
    return p->tag * 1000 + t * 100 + v;
}

int main(void) {
    struct Outer o1 = { 1, { 2 } };
    struct Outer o2 = { 3, { 4 } };
    int res = caller_fn(&o1, &o2);
    /* Expect o1.tag (1)*1000 + o2.tag (3)*100 + o2.inner.val (4) = 1304.
     * Before the fix, get_tag(q) and get_inner_val(q) resolved 'p' to o1 instead of o2,
     * producing 1102. */
    if (res != 1304) {
        printf("FAILED: res=%d (expected 1304)\n", res);
        return 1;
    }
    printf("18_shadow_param passed\n");
    return 0;
}
