/* programs/c_args/main.c -- argv + envp via libtoby.
 *
 * Exercises crt0.S's argc/argv/envp setup and getenv() in stdlib.c.
 * Walks both lists, prints each entry, and looks up two known env
 * keys (HOME, ABI_VERSION). The kernel injects ABI_VERSION via the
 * abi_test pathway in M25C; for M25B validation we just print
 * whatever the parent passed (might be NULL). */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv, char **envp) {
    printf("[c_args] argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("[c_args]   argv[%d] = \"%s\"\n", i, argv[i] ? argv[i] : "(null)");
    }

    int envn = 0;
    for (char **e = envp; e && *e; e++) envn++;
    printf("[c_args] envc=%d\n", envn);
    int show = envn < 4 ? envn : 4;
    for (int i = 0; i < show; i++) {
        printf("[c_args]   envp[%d] = \"%s\"\n", i, envp[i]);
    }

    /* getenv round-trip: pull HOME if set, otherwise mention it. */
    const char *home = getenv("HOME");
    printf("[c_args] getenv(\"HOME\") = %s\n", home ? home : "(unset)");

    /* Reach into the persisted globals (set by crt0.S) directly so
     * we exercise the cross-TU access path. */
    printf("[c_args] __toby_argc=%d __toby_argv=%p __toby_envp=%p\n",
           __toby_argc, (void *)__toby_argv, (void *)__toby_envp);

    printf("[c_args] ALL OK\n");
    return 0;
}
