/* env -- print the environment, or run a command in a modified one.
 *
 * POSIX XCU:  env [-i] [name=value]... [utility [argument...]]
 *
 * The "run a utility" half was missing, so `env echo hi` printed the whole
 * environment and never ran echo -- and `env CMD` is how a script asks for
 * the real utility rather than the shell's builtin version of it, which is
 * exactly what ./configure-shaped scripts do with `env time -f ...`. With
 * only the printing half, every such line produced an environment dump where
 * the script expected the command's output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static int is_assignment(const char *s) {
    if (!s || !*s) return 0;
    if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_'))
        return 0;
    for (const char *p = s + 1; *p; p++) {
        if (*p == '=') return 1;
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_'))
            return 0;
    }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int ignore = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (strcmp(argv[i], "-i") == 0 ||
            strcmp(argv[i], "--ignore-environment") == 0) {
            ignore = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        fprintf(stderr, "env: unknown option '%s'\n", argv[i]);
        return 125;
    }

    if (ignore) {
        /* Start from nothing; the assignments below are the whole
         * environment. environ is replaced rather than cleared in place
         * because the array itself may be static. */
        static char *empty[] = { 0 };
        environ = empty;
    }

    for (; i < argc && is_assignment(argv[i]); i++) {
        if (putenv(argv[i]) != 0) {
            fprintf(stderr, "env: cannot set '%s'\n", argv[i]);
            return 125;
        }
    }

    if (i >= argc) {
        if (!environ) return 0;
        for (char **e = environ; *e; e++) printf("%s\n", *e);
        return 0;
    }

    execvp(argv[i], &argv[i]);
    /* 127 is "not found", 126 is "found but could not run" -- POSIX XCU
     * requires env to make that distinction. */
    fprintf(stderr, "env: %s: cannot execute\n", argv[i]);
    return access(argv[i], 0) == 0 ? 126 : 127;
}
