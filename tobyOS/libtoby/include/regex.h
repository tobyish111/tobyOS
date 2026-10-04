/* regex.h -- libtoby's POSIX regex (BRE and ERE). */

#ifndef LIBTOBY_REGEX_H
#define LIBTOBY_REGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int regoff_t;

typedef struct {
    void *priv;
    int   re_nsub;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

/* cflags for regcomp */
#define REG_EXTENDED  1
#define REG_ICASE     2
#define REG_NOSUB     4
#define REG_NEWLINE   8

/* LIBTOBY EXTENSION, and it exists because the three tools genuinely
 * disagree. Measured against the real GNU tools:
 *
 *   printf 'a\tb\n' | sed  's/[\t]/X/'      -> unchanged (backslash is
 *   printf 'atb\n'  | sed  's/[\t]/X/'      -> aXb        a LITERAL inside
 *   printf 'a\tb\n' | grep -c '[\t]'        -> 0          a bracket, so
 *   printf 'atb\n'  | grep -c '[\t]'        -> 1          [\t] is [\ t])
 *
 *   printf 'a\tb\n' | awk '/[\t]/{print}'   -> matches   (POSIX awk says
 *   printf 'atb\n'  | awk '/[\t]/{print}'   -> no match   EREs process
 *                                                         escapes)
 *
 * So the behaviour cannot be a property of the engine; it has to be a
 * property of the CALLER. awk passes this flag, sed and grep do not. */
#define REG_ESCBRACKET  0x100

/* LIBTOBY EXTENSION, and it exists for the same reason: a `{` that does
 * not open a valid interval. GNU is SPLIT on it, measured:
 *
 *   printf 'a{' | grep -cE 'a{'   -> 1        (a literal brace)
 *   printf 'a{' | awk  '/a{/'     -> matches  (a literal brace)
 *   printf 'a{' | sed -E 's/a{/X/'-> error: Unmatched \{
 *   bash: [[ { =~ { ]]            -> status 2 (regcomp refused it)
 *
 * glibc's regcomp REJECTS it; grep and gawk opt into the lenient
 * reading through RE_INVALID_INTERVAL_ORD. libtoby was lenient for
 * everyone, so `[[ { =~ { ]]` matched where bash reports a bad pattern.
 * The callers that want glibc's answer -- tsh's `=~` and sed -E -- pass
 * this flag; grep, awk and the editors keep the literal brace. */
#define REG_STRICTBRACE 0x200
/* tobyOS extension: the pattern and the strings are UTF-8, and a character
 * is a whole multibyte one -- `.` and a bracket expression take one, a
 * multibyte literal is one atom, [[:alpha:]] and the rest and REG_ICASE
 * are musl's for code points past 0x7f (tobyos/wctab.h). What musl's
 * regcomp does in a UTF-8 locale; /bin/tsh asks for it for `=~` when its
 * locale is UTF-8. Without the flag every byte is a character, as ever. */
#define REG_UTF8        0x400

/* eflags for regexec */
#define REG_NOTBOL    1
#define REG_NOTEOL    2

/* error codes */
#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13

int    regcomp (regex_t *preg, const char *pattern, int cflags);
int    regexec (const regex_t *preg, const char *string,
                size_t nmatch, regmatch_t pmatch[], int eflags);
void   regfree (regex_t *preg);
size_t regerror(int errcode, const regex_t *preg,
                char *errbuf, size_t errbuf_size);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_REGEX_H */
