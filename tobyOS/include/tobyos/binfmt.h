/* binfmt.h -- recognising an executable that is not an ELF or PE image.
 *
 * Until now there was no #! handling anywhere: proc_spawn and execve read
 * the file, tested it for ELF and for MZ/PE, and a shell script failed both
 * tests and came back as "not a valid image" -- `./script.sh` returned 127
 * from a shell that could run every binary on the system but none of the
 * scripts. The kernel-side and hosted tsh both go through proc_spawn, and
 * execvp goes through execve, so both loaders consult this.
 */
#ifndef TOBYOS_BINFMT_H
#define TOBYOS_BINFMT_H

#include <tobyos/types.h>

/* Linux's BINPRM_BUF_SIZE: only this much of the first line is examined. */
#define BINFMT_SHEBANG_MAX 256

/* Nesting limit, as Linux: a script whose interpreter is a script whose
 * interpreter is a script... stops here rather than recursing until the
 * kernel stack runs out. */
#define BINFMT_MAX_RECURSION 4

/* Examines the head of an image.
 *   returns 1  -- "#!interp [arg]" parsed; interp and optarg filled (optarg
 *                 may be the empty string). As on Linux, everything after
 *                 the interpreter is passed as ONE argument, blanks and all.
 *   returns 0  -- not a shebang; try the other formats.
 *   returns -1 -- "#!" with no interpreter behind it: malformed, and the
 *                 caller should fail the exec rather than fall through and
 *                 hand the ELF loader a text file. */
int binfmt_shebang(const void *image, size_t len,
                   char *interp, size_t interp_cap,
                   char *optarg, size_t optarg_cap);

/* Builds the interpreter's argv from the script's:
 *     [interp, optarg?, script, argv[1], argv[2], ...]
 * into `out` (pointers only -- the strings must outlive the call). Returns
 * the new argc, or -1 if it would not fit in `max` slots. argv[0] of the
 * script is dropped: its place is taken by the script's path, which is
 * what the interpreter opens. */
int binfmt_shebang_argv(const char *interp, const char *optarg,
                        const char *script, int argc, char **argv,
                        char **out, int max);

#endif /* TOBYOS_BINFMT_H */
