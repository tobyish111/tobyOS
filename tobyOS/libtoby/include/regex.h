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
