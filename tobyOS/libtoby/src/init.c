/* libtoby/src/init.c -- one-time libc bring-up.
 *
 * Called by crt0.S exactly once, before main(). Today this is a very
 * thin function: the heap is lazy-initialised on first malloc(), the
 * stdio FILEs are static objects, and there's nothing that *needs* to
 * happen at process start. We use this slot for two things:
 *
 *   1. Static asserts that the libtoby header constants stay in lock-
 *      step with the kernel ABI header. If a future kernel renumbers
 *      O_RDONLY behind our back, the build (or the boot, depending on
 *      compiler version) catches it instead of users discovering it
 *      via mysterious EBADF returns.
 *
 *   2. A hook for future bring-up (locale, signal default disposition,
 *      tty mode setup, etc.) that we'll grow into during M25C/D/E.
 */

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include "libtoby_internal.h"

/* C11 _Static_assert has the cleanest diagnostic; fall back to the
 * (otherwise identical) GNU extension on older toolchains. */
#define LIBTOBY_SA(cond, msg) _Static_assert((cond), msg)

LIBTOBY_SA(O_RDONLY  == ABI_O_RDONLY,  "O_RDONLY drift");
LIBTOBY_SA(O_WRONLY  == ABI_O_WRONLY,  "O_WRONLY drift");
LIBTOBY_SA(O_RDWR    == ABI_O_RDWR,    "O_RDWR drift");
LIBTOBY_SA(O_CREAT   == ABI_O_CREAT,   "O_CREAT drift");
LIBTOBY_SA(O_EXCL    == ABI_O_EXCL,    "O_EXCL drift");
LIBTOBY_SA(O_TRUNC   == ABI_O_TRUNC,   "O_TRUNC drift");
LIBTOBY_SA(O_APPEND  == ABI_O_APPEND,  "O_APPEND drift");

LIBTOBY_SA(SEEK_SET  == ABI_SEEK_SET,  "SEEK_SET drift");
LIBTOBY_SA(SEEK_CUR  == ABI_SEEK_CUR,  "SEEK_CUR drift");
LIBTOBY_SA(SEEK_END  == ABI_SEEK_END,  "SEEK_END drift");

LIBTOBY_SA(WNOHANG   == ABI_WNOHANG,   "WNOHANG drift");

LIBTOBY_SA(EPERM     == ABI_EPERM,     "EPERM drift");
LIBTOBY_SA(ENOENT    == ABI_ENOENT,    "ENOENT drift");
LIBTOBY_SA(ENOMEM    == ABI_ENOMEM,    "ENOMEM drift");
LIBTOBY_SA(EBADF     == ABI_EBADF,     "EBADF drift");
LIBTOBY_SA(EINVAL    == ABI_EINVAL,    "EINVAL drift");
LIBTOBY_SA(ENOSYS    == ABI_ENOSYS,    "ENOSYS drift");
LIBTOBY_SA(EEXIST    == ABI_EEXIST,    "EEXIST drift");

/* ---- C++ global constructors / destructors (stage 13 C++ runtime) --
 * A program whose linker script defines the .init_array/.fini_array
 * bounds gets its constructors run before main and its destructors at
 * exit. The symbols are WEAK: every existing C program's script does
 * not define them, the references resolve to 0, and both walks are
 * no-ops -- no linker-script churn across the tree. */
typedef void (*toby_ctor_t)(void);
extern toby_ctor_t __init_array_start[] __attribute__((weak));
extern toby_ctor_t __init_array_end[]   __attribute__((weak));
extern toby_ctor_t __fini_array_start[] __attribute__((weak));
extern toby_ctor_t __fini_array_end[]   __attribute__((weak));

extern int atexit(void (*fn)(void));

static void __libtoby_run_fini(void) {
    /* Reverse order, per the C++ ABI. */
    if (!__fini_array_start || !__fini_array_end) return;
    for (toby_ctor_t *f = __fini_array_end; f-- > __fini_array_start; )
        if (*f) (*f)();
}

void __libtoby_init(int argc, char **argv, char **envp) {
    (void)argc; (void)argv;
    /* Milestone 25C: hand `environ` the kernel-packed env array so
     * getenv()/setenv()/exec*() all see the same view immediately.
     * The first call to setenv/unsetenv/putenv lazily migrates this
     * pointer to a heap-owned copy, leaving the stack array alone. */
    environ = envp;

    /* C++ static constructors (weak no-op for C programs). Runs after
     * environ is live so constructors may getenv(). */
    if (__init_array_start && __init_array_end) {
        if (__fini_array_start != __fini_array_end)
            atexit(__libtoby_run_fini);
        for (toby_ctor_t *f = __init_array_start;
             f < __init_array_end; f++)
            if (*f) (*f)();
    }
}
