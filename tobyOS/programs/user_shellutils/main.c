/* rmdir yes tac seq whoami which uname free sync nohup timeout watch
 * stat du xargs dd printenv nice pgrep pkill tty stty cksum truncate
 * install mkfifo hwclock arch nproc hostid logname link unlink pwd sum factor test [ printf chgrp
 * shred uniq who users uptime true false ls env
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
#include "su_sort.c"
#include "su_script.c"
#include "su_hwclock.c"
#include "su_arch.c"
#include "su_nproc.c"
#include "su_hostid.c"
#include "su_logname.c"
#include "su_link.c"
#include "su_pwd.c"
#include "su_sum.c"
#include "su_factor.c"
#include "su_test.c"
#include "su_printf.c"
#include "su_chgrp.c"
#include "su_shred.c"
#include "su_uniq.c"
#include "su_who.c"
#include "su_uptime.c"
#include "su_true.c"
#include "su_ls.c"
#include "su_env.c"
#include "su_readlink.c"
#include "su_mktemp.c"
#include "su_getent.c"
#include "su_hostname.c"
#include "su_chown.c"
#include "su_killall.c"
#include "su_mount.c"
#include "su_od.c"
#include "su_hexdump.c"
#include "su_comm.c"
#include "su_expand.c"
#include "su_fold.c"
#include "su_nl.c"
#include "su_paste.c"
#include "su_rev.c"
#include "su_shuf.c"
#include "su_split.c"
#include "su_rm.c"
#include "su_cp.c"
#include "su_mv.c"
#include "su_sm_common.c"
#include "su_md5sum.c"
#include "su_base64.c"
#include "su_dircolors.c"
#include "su_dir.c"
#include "su_fileops.c"
#include "su_echo.c"
#include "su_cat.c"
#include "su_tee.c"
#include "su_mkdir.c"
#include "su_mknod.c"
#include "su_chroot.c"
#include "su_chmod.c"
#include "su_ln.c"
#include "su_kill.c"
#include "su_id.c"
#include "su_df.c"
#include "su_pinky.c"
#include "su_stdbuf.c"
#include "su_chcon.c"
#include "su_runcon.c"
#include "su_txio.c"
#include "su_head.c"
#include "su_tail.c"
#include "su_wc.c"
#include "su_cut.c"
#include "su_tr.c"
#include "su_basename.c"
#include "su_sleep.c"
#include "su_expr.c"
#include "su_find.c"
#include "su_fmt.c"
#include "su_pr.c"
#include "su_join.c"
#include "su_tsort.c"
#include "su_numfmt.c"
#include "su_csplit.c"
#include "su_ptx.c"
#include "su_pathchk.c"
#include "su_diff.c"
#include "su_gzip.c"
#include "su_bzip2.c"
#include "su_xz.c"
#include "su_tar.c"

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
    if (!strcmp(base, "sort"))     return do_sort(argc, argv);
    if (!strcmp(base, "script"))   return do_script(argc, argv);
    if (!strcmp(base, "hwclock"))  return do_hwclock(argc, argv);
    if (!strcmp(base, "arch"))      return do_arch(argc, argv);
    if (!strcmp(base, "nproc"))     return do_nproc(argc, argv);
    if (!strcmp(base, "hostid"))    return do_hostid(argc, argv);
    if (!strcmp(base, "logname"))   return do_logname(argc, argv);
    if (!strcmp(base, "link"))      return do_link(argc, argv);
    if (!strcmp(base, "unlink"))    return do_unlink(argc, argv);
    if (!strcmp(base, "pwd"))       return do_pwd(argc, argv);
    if (!strcmp(base, "sum"))       return do_sum(argc, argv);
    if (!strcmp(base, "factor"))    return do_factor(argc, argv);
    if (!strcmp(base, "test") || !strcmp(base, "[")) return do_test(argc, argv);
    if (!strcmp(base, "printf"))    return do_printf(argc, argv);
    if (!strcmp(base, "chgrp"))     return do_chgrp(argc, argv);
    if (!strcmp(base, "shred"))     return do_shred(argc, argv);
    if (!strcmp(base, "uniq"))      return do_uniq(argc, argv);
    if (!strcmp(base, "who"))       return do_who(argc, argv);
    if (!strcmp(base, "users"))     return do_users(argc, argv);
    if (!strcmp(base, "uptime"))    return do_uptime(argc, argv);
    if (!strcmp(base, "true"))      return do_true(argc, argv, 0);
    if (!strcmp(base, "false"))     return do_true(argc, argv, 1);
    if (!strcmp(base, "ls"))        return do_ls(argc, argv);
    if (!strcmp(base, "env"))       return do_env(argc, argv);
    if (!strcmp(base, "readlink"))  return do_readlink(argc, argv);
    if (!strcmp(base, "realpath"))  return do_realpath(argc, argv);
    if (!strcmp(base, "mktemp"))    return do_mktemp(argc, argv);
    if (!strcmp(base, "getent"))    return do_getent(argc, argv);
    if (!strcmp(base, "groups"))    return do_groups(argc, argv);
    if (!strcmp(base, "hostname"))  return do_hostname(argc, argv);
    if (!strcmp(base, "domainname")) return do_domainname(argc, argv);
    if (!strcmp(base, "dnsdomainname")) return do_hostname(argc, argv);
    if (!strcmp(base, "nisdomainname")) return do_hostname(argc, argv);
    if (!strcmp(base, "ypdomainname")) return do_hostname(argc, argv);
    if (!strcmp(base, "chown"))     return do_chown(argc, argv);
    if (!strcmp(base, "killall"))   return do_killall(argc, argv);
    if (!strcmp(base, "mount"))     return do_mount(argc, argv);
    if (!strcmp(base, "umount"))    return do_umount(argc, argv);
    if (!strcmp(base, "od"))        return do_od(argc, argv);
    if (!strcmp(base, "hexdump"))   return do_hexdump(argc, argv);
    if (!strcmp(base, "hd"))        return do_hexdump(argc, argv);
    if (!strcmp(base, "comm"))      return do_comm(argc, argv);
    if (!strcmp(base, "expand"))    return do_expand(argc, argv);
    if (!strcmp(base, "unexpand"))  return do_unexpand(argc, argv);
    if (!strcmp(base, "fold"))      return do_fold(argc, argv);
    if (!strcmp(base, "nl"))        return do_nl(argc, argv);
    if (!strcmp(base, "paste"))     return do_paste(argc, argv);
    if (!strcmp(base, "rev"))       return do_rev(argc, argv);
    if (!strcmp(base, "shuf"))      return do_shuf(argc, argv);
    if (!strcmp(base, "split"))     return do_split(argc, argv);
    if (!strcmp(base, "rm"))        return do_rm(argc, argv);
    if (!strcmp(base, "cp"))        return do_cp(argc, argv);
    if (!strcmp(base, "mv"))        return do_mv(argc, argv);
    if (!strcmp(base, "md5sum"))    return do_md5sum(argc, argv);
    if (!strcmp(base, "sha1sum"))   return do_sha1sum(argc, argv);
    if (!strcmp(base, "sha224sum")) return do_sha224sum(argc, argv);
    if (!strcmp(base, "sha256sum")) return do_sha256sum(argc, argv);
    if (!strcmp(base, "sha384sum")) return do_sha384sum(argc, argv);
    if (!strcmp(base, "sha512sum")) return do_sha512sum(argc, argv);
    if (!strcmp(base, "b2sum"))     return do_b2sum(argc, argv);
    if (!strcmp(base, "base64"))    return do_base64(argc, argv);
    if (!strcmp(base, "base32"))    return do_base32(argc, argv);
    if (!strcmp(base, "basenc"))    return do_basenc(argc, argv);
    if (!strcmp(base, "dircolors")) return do_dircolors(argc, argv);
    if (!strcmp(base, "dir"))       return do_dir(argc, argv);
    if (!strcmp(base, "vdir"))      return do_vdir(argc, argv);
    if (!strcmp(base, "echo"))      return do_echo(argc, argv);
    if (!strcmp(base, "cat"))       return do_cat(argc, argv);
    if (!strcmp(base, "tee"))       return do_tee(argc, argv);
    if (!strcmp(base, "mkdir"))     return do_mkdir(argc, argv);
    if (!strcmp(base, "mknod"))     return do_mknod(argc, argv);
    if (!strcmp(base, "chroot"))    return do_chroot(argc, argv);
    if (!strcmp(base, "chmod"))     return do_chmod(argc, argv);
    if (!strcmp(base, "ln"))        return do_ln(argc, argv);
    if (!strcmp(base, "kill"))      return do_kill(argc, argv);
    if (!strcmp(base, "id"))        return do_id(argc, argv);
    if (!strcmp(base, "df"))        return do_df(argc, argv);
    if (!strcmp(base, "pinky"))     return do_pinky(argc, argv);
    if (!strcmp(base, "stdbuf"))    return do_stdbuf(argc, argv);
    if (!strcmp(base, "chcon"))     return do_chcon(argc, argv);
    if (!strcmp(base, "runcon"))    return do_runcon(argc, argv);
    if (!strcmp(base, "head"))      return do_head(argc, argv);
    if (!strcmp(base, "tail"))      return do_tail(argc, argv);
    if (!strcmp(base, "wc"))        return do_wc(argc, argv);
    if (!strcmp(base, "cut"))       return do_cut(argc, argv);
    if (!strcmp(base, "tr"))        return do_tr(argc, argv);
    if (!strcmp(base, "basename"))  return do_basename(argc, argv);
    if (!strcmp(base, "dirname"))   return do_dirname(argc, argv);
    if (!strcmp(base, "sleep"))     return do_sleep(argc, argv);
    if (!strcmp(base, "expr"))      return do_expr(argc, argv);
    if (!strcmp(base, "find"))      return do_find(argc, argv);
    if (!strcmp(base, "fmt"))       return do_fmt(argc, argv);
    if (!strcmp(base, "pr"))        return do_pr(argc, argv);
    if (!strcmp(base, "join"))      return do_join(argc, argv);
    if (!strcmp(base, "tsort"))     return do_tsort(argc, argv);
    if (!strcmp(base, "numfmt"))    return do_numfmt(argc, argv);
    if (!strcmp(base, "csplit"))    return do_csplit(argc, argv);
    if (!strcmp(base, "ptx"))       return do_ptx(argc, argv);
    if (!strcmp(base, "pathchk"))   return do_pathchk(argc, argv);
    if (!strcmp(base, "diff"))      return do_diff(argc, argv);
    if (!strcmp(base, "cmp"))       return do_cmp(argc, argv);
    if (!strcmp(base, "sdiff"))     return do_sdiff(argc, argv);
    if (!strcmp(base, "diff3"))     return do_diff3(argc, argv);
    if (!strcmp(base, "gzip"))      return do_gzip(argc, argv);
    if (!strcmp(base, "gunzip"))    return do_gzip(argc, argv);
    if (!strcmp(base, "zcat"))      return do_gzip(argc, argv);
    if (!strcmp(base, "bzip2"))     return do_bzip2(argc, argv);
    if (!strcmp(base, "bunzip2"))   return do_bzip2(argc, argv);
    if (!strcmp(base, "bzcat"))     return do_bzip2(argc, argv);
    if (!strcmp(base, "xz"))        return do_xz(argc, argv);
    if (!strcmp(base, "unxz"))      return do_xz(argc, argv);
    if (!strcmp(base, "xzcat"))     return do_xz(argc, argv);
    if (!strcmp(base, "tar"))       return do_tar(argc, argv);

    fprintf(stderr, "%s: unknown applet\n", base);
    return 1;
}
