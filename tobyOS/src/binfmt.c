/* binfmt.c -- #! interpreter lines.
 *
 * The rules are Linux's (fs/binfmt_script.c), because that is what every
 * script on the system was written against:
 *   - only the first BINPRM_BUF_SIZE bytes are examined;
 *   - blanks after "#!" are skipped; the interpreter runs to the next blank;
 *   - whatever follows, blanks trimmed at both ends, is ONE argument --
 *     "#!/bin/sh -e -u" hands sh the single argument "-e -u" -- and a script
 *     that wants two arguments does not get them here or on Linux;
 *   - argv[0] of the script is dropped in favour of the script's path, and
 *     the script's own argv[1..] follow.
 */
#include <tobyos/binfmt.h>
#include <tobyos/klibc.h>

static int is_blank(char c) { return c == ' ' || c == '\t'; }

int binfmt_shebang(const void *image, size_t len,
                   char *interp, size_t interp_cap,
                   char *optarg, size_t optarg_cap)
{
    const char *p = (const char *)image;
    if (!p || len < 3 || p[0] != '#' || p[1] != '!') return 0;

    size_t n = len < BINFMT_SHEBANG_MAX ? len : BINFMT_SHEBANG_MAX;
    size_t eol = 2;
    while (eol < n && p[eol] != '\n') eol++;
    /* a CRLF script: the \r is not part of anything */
    size_t end = eol;
    while (end > 2 && (p[end - 1] == '\r' || is_blank(p[end - 1]))) end--;

    size_t i = 2;
    while (i < end && is_blank(p[i])) i++;
    size_t s = i;
    while (i < end && !is_blank(p[i])) i++;
    if (i == s) return -1;                       /* "#!" and nothing */
    if ((size_t)(i - s) >= interp_cap) return -1;
    memcpy(interp, p + s, i - s);
    interp[i - s] = 0;

    while (i < end && is_blank(p[i])) i++;
    size_t alen = end - i;
    if (alen >= optarg_cap) alen = optarg_cap - 1; /* truncate, as Linux */
    if (alen) memcpy(optarg, p + i, alen);
    optarg[alen] = 0;
    return 1;
}

int binfmt_shebang_argv(const char *interp, const char *optarg,
                        const char *script, int argc, char **argv,
                        char **out, int max)
{
    int n = 0;
    int need = 2 + (optarg && optarg[0] ? 1 : 0) + (argc > 1 ? argc - 1 : 0);
    if (need > max) return -1;
    out[n++] = (char *)interp;
    if (optarg && optarg[0]) out[n++] = (char *)optarg;
    out[n++] = (char *)script;
    for (int i = 1; i < argc; i++) out[n++] = argv[i];
    return n;
}
