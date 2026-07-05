/* libtoby/src/int128.c -- compiler-rt 128-bit integer division helpers.
 *
 * clang emits calls to __udivti3/__umodti3/__divti3/__modti3 for
 * (unsigned) __int128 division; freestanding builds have no
 * compiler-rt, so provide them here (M-JS: QuickJS uses int128 in its
 * number paths). Simple shift-subtract long division -- these are
 * cold paths, clarity over speed. */

typedef unsigned __int128 u128;
typedef __int128 s128;

u128 __udivti3(u128 a, u128 b);
u128 __umodti3(u128 a, u128 b);
s128 __divti3(s128 a, s128 b);
s128 __modti3(s128 a, s128 b);

static u128 udivmod128(u128 a, u128 b, u128 *rem)
{
    if (b == 0) {                       /* div-by-zero: match x86 trap-free */
        if (rem) *rem = a;
        return (u128)-1;
    }
    if (a < b) {
        if (rem) *rem = a;
        return 0;
    }
    u128 q = 0, r = 0;
    for (int i = 127; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= (u128)1 << i;
        }
    }
    if (rem) *rem = r;
    return q;
}

u128 __udivti3(u128 a, u128 b) { return udivmod128(a, b, 0); }

u128 __umodti3(u128 a, u128 b)
{
    u128 r;
    udivmod128(a, b, &r);
    return r;
}

s128 __divti3(s128 a, s128 b)
{
    int neg = (a < 0) != (b < 0);
    u128 ua = a < 0 ? (u128)-a : (u128)a;
    u128 ub = b < 0 ? (u128)-b : (u128)b;
    u128 q = udivmod128(ua, ub, 0);
    return neg ? -(s128)q : (s128)q;
}

s128 __modti3(s128 a, s128 b)
{
    u128 ua = a < 0 ? (u128)-a : (u128)a;
    u128 ub = b < 0 ? (u128)-b : (u128)b;
    u128 r;
    udivmod128(ua, ub, &r);
    return a < 0 ? -(s128)r : (s128)r;
}
