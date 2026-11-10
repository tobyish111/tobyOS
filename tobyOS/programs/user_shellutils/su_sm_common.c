/* su_sm_common.c -- what su_md5sum.c, su_base64.c and su_dircolors.c
 * share: GNU's two ways of quoting a name or an argument in a message,
 * as GNU coreutils 9.7 writes them in the C locale (measured with
 * md5sum's and base64's messages on names holding every printable
 * punctuation character, quotes, control bytes and UTF-8).
 *
 * Each of those files includes this one; the guard makes the second
 * and later includes nothing, so the unity build can take them in any
 * order (main.c may include this file too).
 *
 * These were measured against GNU 9.7 where su_common.c's su_quotef and
 * su_quote were not GNU's; those are now these (finding 209), and the
 * names here call them. */
#ifndef SU_SM_COMMON_C
#define SU_SM_COMMON_C

/* su_common.c's, which are GNU's now (finding 209) */
static const char *sm_quotef(const char *s) { return su_quotef(s); }
static const char *sm_quote(const char *s) { return su_quote(s); }

#endif /* SU_SM_COMMON_C */
