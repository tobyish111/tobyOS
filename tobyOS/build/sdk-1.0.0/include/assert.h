/* assert.h -- libtoby's assert.
 *
 * On failure the macro emits a "[libtoby] assert failed: expr at file:line"
 * line on stderr (fd 2), then aborts via abort() (which raises SYS_EXIT
 * with code 134, matching POSIX's "killed by SIGABRT" exit status). */

#ifndef LIBTOBY_ASSERT_H
#define LIBTOBY_ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

void __libtoby_assert_fail(const char *expr,
                           const char *file,
                           unsigned    line,
                           const char *func) __attribute__((noreturn));

#ifdef NDEBUG
#  define assert(e)  ((void)0)
#else
#  define assert(e) \
    ((e) ? (void)0 : __libtoby_assert_fail(#e, __FILE__, __LINE__, __func__))
#endif

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_ASSERT_H */
