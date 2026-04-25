/* ctype.h -- libtoby's C character classification.
 *
 * Pure ASCII only; no locale support. Each function follows C11
 * semantics for the (unsigned char) range [0..255] and EOF (-1).
 * Behaviour for other negative inputs is undefined, matching the
 * standard.
 *
 * Inline static so call sites compile down to a single comparison
 * pair -- no jump into libtoby for what is really a 2-instruction
 * predicate. */

#ifndef LIBTOBY_CTYPE_H
#define LIBTOBY_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

static inline int isascii(int c) { return (unsigned)c < 128u; }
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c) {
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isspace(int c) {
    return c == ' '  || c == '\t' || c == '\n'
        || c == '\v' || c == '\f' || c == '\r';
}
static inline int isblank(int c) { return c == ' ' || c == '\t'; }
static inline int iscntrl(int c) { return ((unsigned)c < 32u) || c == 127; }
static inline int isprint(int c) { return c >= 32 && c < 127; }
static inline int isgraph(int c) { return c > 32 && c < 127; }
static inline int ispunct(int c) {
    return isprint(c) && !isalnum(c) && c != ' ';
}

static inline int tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}
static inline int toupper(int c) {
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_CTYPE_H */
