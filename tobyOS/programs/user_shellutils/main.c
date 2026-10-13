/* rmdir yes tac seq whoami which uname free sync nohup timeout watch
 * stat du xargs dd printenv nice pgrep pkill tty stty cksum truncate
 * install mkfifo
 *
 * One multi-call binary, dispatching on argv[0]. Every one of these names
 * previously resolved to a busybox applet -- and busybox in this tree is a
 * gitignored, opt-in artifact that a clean checkout does not have, so all
 * sixteen would simply vanish. They are native now.
 *
 * One file per command, su_NAME.c, and what they share in su_common.c,
 * all included here: the build compiles one translation unit per program.
 * Built with SU_HOST, the same source compiles for Linux
 * (tools/agent/suhost.sh), so logs/utilgate.sh can grade it against GNU's
 * tools in WSL before a guest is booted.
 *
 * Where a tool reads kernel state it reads a real one: uname from
 * /proc/sys/kernel and /proc/version, free from /proc/meminfo, sync
 * through the block cache. Nothing here invents a number.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#ifdef SU_HOST
#include <sys/ioctl.h>      /* libtoby declares ioctl in unistd.h */
#endif
#include <termios.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <regex.h>
#include <limits.h>

/* The native open has no non-blocking flag: where GNU opens O_NONBLOCK
 * (sync, truncate) so a FIFO with no reader cannot hang it, this open
 * blocks as a plain one. */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif

#ifndef SU_HOST
#include <tobyos/abi/abi.h>
#endif

static const char *prog = "shellutils";

#ifndef SU_HOST
static long sc0(long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n) : "rcx", "r11", "memory");
    return r;
}
#endif

static int slurp(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* Trim to the first line, in place. */
static char *first_line(char *s) {
    char *nl = strchr(s, '\n');
    if (nl) *nl = 0;
    return s;
}

/* ------------------------------------------------------------- trivial */

#include "su_common.c"
#include "su_num.c"
#include "su_rmdir.c"
#include "su_yes.c"
#include "su_tac.c"
#include "su_seq.c"
#include "su_whoami.c"
#include "su_which.c"
#include "su_uname.c"
#include "su_free.c"
#include "su_sync.c"
#include "su_stat.c"
#include "su_du.c"
#include "su_xargs.c"
#include "su_nohup.c"
#include "su_timeout.c"
#include "su_watch.c"
#include "su_dd.c"
#include "su_printenv.c"
#include "su_nice.c"
#include "su_pgrep.c"
#include "su_tty.c"
#include "su_stty.c"
#include "su_cksum.c"
#include "su_truncate.c"
#include "su_install.c"
#include "su_mkfifo.c"
#include "su_time.c"
#include "su_date.c"
#include "su_touch.c"

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    su_set_name(argv[0]);

    if (!strcmp(base, "rmdir"))   return do_rmdir(argc, argv);
    if (!strcmp(base, "yes"))     return do_yes(argc, argv);
    if (!strcmp(base, "tac"))     return do_tac(argc, argv);
    if (!strcmp(base, "seq"))     return do_seq(argc, argv);
    if (!strcmp(base, "whoami"))  return do_whoami(argc, argv);
    if (!strcmp(base, "which"))   return do_which(argc, argv);
    if (!strcmp(base, "uname"))   return do_uname(argc, argv);
    if (!strcmp(base, "free"))    return do_free(argc, argv);
    if (!strcmp(base, "sync"))    return do_sync(argc, argv);
    if (!strcmp(base, "stat"))    return do_stat(argc, argv);
    if (!strcmp(base, "du"))      return do_du(argc, argv);
    if (!strcmp(base, "xargs"))   return do_xargs(argc, argv);
    if (!strcmp(base, "nohup"))   return do_nohup(argc, argv);
    if (!strcmp(base, "timeout")) return do_timeout(argc, argv);
    if (!strcmp(base, "watch"))   return do_watch(argc, argv);
    if (!strcmp(base, "dd"))      return do_dd(argc, argv);
    if (!strcmp(base, "printenv")) return do_printenv(argc, argv);
    if (!strcmp(base, "nice"))     return do_nice(argc, argv);
    if (!strcmp(base, "pgrep"))    return do_pgrep(argc, argv, 0);
    if (!strcmp(base, "pkill"))    return do_pgrep(argc, argv, 1);
    if (!strcmp(base, "tty"))      return do_tty(argc, argv);
    if (!strcmp(base, "stty"))     return do_stty(argc, argv);
    if (!strcmp(base, "cksum"))    return do_cksum(argc, argv);
    if (!strcmp(base, "truncate")) return do_truncate(argc, argv);
    if (!strcmp(base, "install"))  return do_install(argc, argv);
    if (!strcmp(base, "mkfifo"))   return do_mkfifo(argc, argv);
    if (!strcmp(base, "date"))     return do_date(argc, argv);
    if (!strcmp(base, "touch"))    return do_touch(argc, argv);

    fprintf(stderr, "%s: unknown applet\n", base);
    return 1;
}
