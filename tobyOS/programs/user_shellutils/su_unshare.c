/* su_unshare.c -- unshare, as util-linux 2.41.3's (logs/utilgate/
 * unshare.sh): run PROGRAM (or $SHELL, /bin/sh without SHELL) in new
 * namespaces.
 *
 * Measured on util-linux's (WSL, as an unprivileged user and inside its
 * own `unshare -r`; logs/b33/unsmeasure*-wsl.out):
 *   - -m -u -i -n -p -U -C -T each ask for a namespace; one unshare(2)
 *     takes them all, and its refusal is "unshare failed: ERR" (status 1).
 *     Asking for none makes no unshare(2) at all.
 *   - -r is --map-user=0 --map-group=0, -c the caller's own euid and egid;
 *     the last of them wins. A mapping implies -U, a mapped group
 *     setgroups "deny" as well -- and with --setgroups allow, "options
 *     --setgroups=allow and --map-group are mutually exclusive". A
 *     --map-user/--map-group id is a name, else a number; -S and -G take
 *     numbers: "failed to parse uid: 'X'" (or gid), with ": Numerical
 *     result out of range" after a '-' (blanks and a '+' may come first).
 *   - After unshare(2), as writes to /proc/self ("write failed FILE: ERR"):
 *     setgroups deny for a mapped group, uid_map "UID EUID 1", gid_map
 *     "GID EGID 1", and then --setgroups' own word (after a gid_map, a deny
 *     is refused: `-r --setgroups deny` fails).
 *   - With a new mount namespace, / is made private, recursively, first
 *     (--propagation slave|shared|private|unchanged; "unsupported
 *     propagation mode: X"; "cannot change root filesystem propagation").
 *   - -f forks: the parent waits and goes as the child went (a signal's
 *     death is raised again on itself). --mount-proc[=DIR] implies -m: the
 *     child makes DIR private and mounts a proc there ("mount DIR failed").
 *   - -R DIR is chroot(2) ("cannot change root directory to 'D': ERR"),
 *     then a chdir to -w's DIR or "/" ("cannot chdir to 'D': ERR"); -w
 *     alone is just the chdir. -G drops the supplementary groups and sets
 *     the gid ("setgroups failed", "setgid() failed"), then -S the uid
 *     ("setuid() failed").
 *   - PROGRAM is found on $PATH: "failed to execute P: ERR", status 127
 *     when it is not there, else 126. With none, $SHELL (/bin/sh when it
 *     is not set) with no arguments, argv[0] its last component.
 *
 * Not here, because there is nothing under them: persisting a namespace
 * on a file (--mount=FILE &c.; there is no nsfs to bind -- refused, not
 * ignored), --kill-child, --map-users/--map-groups/--map-auto/
 * --map-subids (no newuidmap, no /etc/subuid), -l, --keep-caps,
 * --monotonic, --boottime. -C and -T are asked of the kernel, which makes
 * no cgroup or time namespace: its EINVAL.
 *
 * tobyOS: unshare(2) is libtoby's (ABI_SYS_UNSHARE, the Linux
 * personality's ns_unshare; finding 241 -- there was no unshare(1) of
 * tobyOS's, and busybox's has no --map-user). */

#ifndef SU_HOST
#include <sched.h>
#endif

#define US_NEWTIME   0x00000080
#define US_NEWNS     0x00020000
#define US_NEWCGROUP 0x02000000
#define US_NEWUTS    0x04000000
#define US_NEWIPC    0x08000000
#define US_NEWUSER   0x10000000
#define US_NEWPID    0x20000000
#define US_NEWNET    0x40000000

static int us_unshare(int flags) {
#ifdef SU_HOST
    return (int)syscall(SYS_unshare, flags);
#else
    return unshare(flags);
#endif
}

/* a number, as util-linux's strtoul_or_err reads one: blanks and a '+'
 * may lead, nothing may follow; a '-' is out of range */
static uint64_t us_number(const char *s, const char *what) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') su_error(1, ERANGE, "failed to parse %s: '%s'", what, s);
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) su_error(1, 0, "failed to parse %s: '%s'", what, s);
    uint64_t v = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) su_error(1, ERANGE, "failed to parse %s: '%s'", what, s);
        v = v * 10 + d;
    }
    if (*p) su_error(1, 0, "failed to parse %s: '%s'", what, s);
    return v;
}

/* --map-user / --map-group: a name in DB, else a number */
static uint32_t us_id(const char *s, const char *db, const char *what) {
    unsigned long id;
    if (cr_db(db, s, 0, &id, 0, 0, 0) == 0) return (uint32_t)id;
    return (uint32_t)us_number(s, what);
}

static void us_write(const char *file, const char *text) {
    int fd = open(file, O_WRONLY);
    if (fd < 0) su_error(1, errno, "cannot open %s", file);
    size_t n = strlen(text);
    if (write(fd, text, n) != (ssize_t)n) su_error(1, errno, "write failed %s", file);
    if (close(fd) != 0) su_error(1, errno, "close failed %s", file);
}

static int do_unshare(int argc, char **argv) {
    enum { MAPUSER_OPT = 0x100, MAPGROUP_OPT, PROPAGATION_OPT, SETGROUPS_OPT,
           MOUNTPROC_OPT };
    static const struct su_option longs[] = {
        { "mount", SU_OPT_ARG, 0, 'm' },
        { "uts", SU_OPT_ARG, 0, 'u' },
        { "ipc", SU_OPT_ARG, 0, 'i' },
        { "net", SU_OPT_ARG, 0, 'n' },
        { "pid", SU_OPT_ARG, 0, 'p' },
        { "user", SU_OPT_ARG, 0, 'U' },
        { "cgroup", SU_OPT_ARG, 0, 'C' },
        { "time", SU_OPT_ARG, 0, 'T' },
        { "fork", SU_NO_ARG, 0, 'f' },
        { "mount-proc", SU_OPT_ARG, 0, MOUNTPROC_OPT },
        { "map-user", SU_REQ_ARG, 0, MAPUSER_OPT },
        { "map-group", SU_REQ_ARG, 0, MAPGROUP_OPT },
        { "map-root-user", SU_NO_ARG, 0, 'r' },
        { "map-current-user", SU_NO_ARG, 0, 'c' },
        { "propagation", SU_REQ_ARG, 0, PROPAGATION_OPT },
        { "setgroups", SU_REQ_ARG, 0, SETGROUPS_OPT },
        { "root", SU_REQ_ARG, 0, 'R' },
        { "wd", SU_REQ_ARG, 0, 'w' },
        { "setuid", SU_REQ_ARG, 0, 'S' },
        { "setgid", SU_REQ_ARG, 0, 'G' },
        { "help", SU_NO_ARG, 0, 'h' },
        { "version", SU_NO_ARG, 0, 'V' },
        { 0, 0, 0, 0 }
    };
    int flags = 0, forkit = 0, c;
    int map_user = -1, map_group = -1;          /* -1: none; else the id */
    uint32_t mu = 0, mg = 0;
    const char *procdir = 0, *setgroups_word = 0, *newroot = 0, *newdir = 0;
    unsigned long prop = MT_REC | MT_PRIVATE;   /* 0: unchanged */
    int force_uid = 0, force_gid = 0;
    uint32_t suid = 0, sgid = 0;
    uid_t euid = geteuid();
    gid_t egid = getegid();
    while ((c = su_getopt_long(argc, argv, "+fhVmuinpCTUrR:w:S:G:c", longs, 0)) != -1) {
        int bit = 0;
        switch (c) {
        case 'm': bit = US_NEWNS; break;
        case 'u': bit = US_NEWUTS; break;
        case 'i': bit = US_NEWIPC; break;
        case 'n': bit = US_NEWNET; break;
        case 'p': bit = US_NEWPID; break;
        case 'U': bit = US_NEWUSER; break;
        case 'C': bit = US_NEWCGROUP; break;
        case 'T': bit = US_NEWTIME; break;
        case 'f': forkit = 1; break;
        case MOUNTPROC_OPT:
            flags |= US_NEWNS;
            procdir = su_optarg ? su_optarg : "/proc";
            break;
        case MAPUSER_OPT:
            flags |= US_NEWUSER;
            mu = us_id(su_optarg, "/etc/passwd", "uid"); map_user = 1;
            break;
        case MAPGROUP_OPT:
            flags |= US_NEWUSER;
            mg = us_id(su_optarg, "/etc/group", "gid"); map_group = 1;
            break;
        case 'r':
            flags |= US_NEWUSER;
            mu = 0; mg = 0; map_user = map_group = 1;
            break;
        case 'c':
            flags |= US_NEWUSER;
            mu = (uint32_t)euid; mg = (uint32_t)egid; map_user = map_group = 1;
            break;
        case PROPAGATION_OPT:
            if (!strcmp(su_optarg, "slave")) prop = MT_REC | MT_SLAVE;
            else if (!strcmp(su_optarg, "shared")) prop = MT_REC | MT_SHARED;
            else if (!strcmp(su_optarg, "private")) prop = MT_REC | MT_PRIVATE;
            else if (!strcmp(su_optarg, "unchanged")) prop = 0;
            else su_error(1, 0, "unsupported propagation mode: %s", su_optarg);
            break;
        case SETGROUPS_OPT:
            if (strcmp(su_optarg, "allow") && strcmp(su_optarg, "deny"))
                su_error(1, 0, "unsupported --setgroups argument '%s'", su_optarg);
            setgroups_word = su_optarg;
            break;
        case 'R': newroot = su_optarg; break;
        case 'w': newdir = su_optarg; break;
        case 'S': suid = (uint32_t)us_number(su_optarg, "uid"); force_uid = 1; break;
        case 'G': sgid = (uint32_t)us_number(su_optarg, "gid"); force_gid = 1; break;
        case 'h':
            printf("\n"
                   "Usage:\n"
                   " %s [options] [<program> [<argument>...]]\n"
                   "\n"
                   "Run a program in namespaces of its own.\n"
                   "\n"
                   "Options:\n"
                   " -m, --mount               a mount namespace\n"
                   " -u, --uts                 a UTS (host and domain name) namespace\n"
                   " -i, --ipc                 an IPC namespace\n"
                   " -n, --net                 a network namespace\n"
                   " -p, --pid                 a pid namespace (its first child is 1)\n"
                   " -U, --user                a user namespace\n"
                   " -C, --cgroup              a cgroup namespace\n"
                   " -T, --time                a time namespace\n"
                   "\n"
                   " -f, --fork                run the program as a child, and wait\n"
                   "     --mount-proc[=<dir>]  mount a proc of the new namespaces there\n"
                   "                             (/proc; implies --mount)\n"
                   " -r, --map-root-user       be root inside: --map-user=0 --map-group=0\n"
                   " -c, --map-current-user    keep the same uid and gid inside\n"
                   "     --map-user=<uid>|<name>    the uid inside (implies --user)\n"
                   "     --map-group=<gid>|<name>   the gid inside (implies --user and\n"
                   "                             --setgroups=deny)\n"
                   "     --propagation slave|shared|private|unchanged\n"
                   "                           what / becomes in a new mount namespace\n"
                   "                             (private)\n"
                   "     --setgroups allow|deny\n"
                   "                           whether setgroups(2) may be called inside\n"
                   " -R, --root=<dir>          change the root directory to <dir>\n"
                   " -w, --wd=<dir>            change the working directory to <dir>\n"
                   " -S, --setuid <uid>        the uid to run the program as\n"
                   " -G, --setgid <gid>        the gid to run it as (and no other group)\n"
                   "\n"
                   " -h, --help                show this help\n"
                   " -V, --version             show the version\n",
                   su_short);
            return 0;
        case 'V': return su_version();
        default: su_try_help(1);
        }
        if (bit) {
            /* a namespace to keep on a file needs a filesystem of namespaces
             * to bind it from, and there is none here */
            if (su_optarg)
                su_error(1, 0, "cannot keep a namespace on a file (%s): not supported",
                         su_optarg);
            flags |= bit;
        }
    }
    if (map_group == 1 && setgroups_word && !strcmp(setgroups_word, "allow"))
        su_error(1, 0, "options --setgroups=allow and --map-group are mutually exclusive");

    if (flags && us_unshare(flags) != 0)
        su_error(1, errno, "unshare failed");

    if (map_group == 1) us_write("/proc/self/setgroups", "deny");
    char line[64];
    if (map_user == 1) {
        snprintf(line, sizeof line, "%u %u 1", (unsigned)mu, (unsigned)euid);
        us_write("/proc/self/uid_map", line);
    }
    if (map_group == 1) {
        snprintf(line, sizeof line, "%u %u 1", (unsigned)mg, (unsigned)egid);
        us_write("/proc/self/gid_map", line);
    }
    if (setgroups_word) us_write("/proc/self/setgroups", setgroups_word);

    if ((flags & US_NEWNS) && prop &&
        mt_sys_mount("none", "/", 0, prop, 0) != 0)
        su_error(1, errno, "cannot change root filesystem propagation");

    if (forkit) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0) su_error(1, errno, "fork failed");
        if (pid > 0) {
            int st;
            while (waitpid(pid, &st, 0) < 0)
                if (errno != EINTR) su_error(1, errno, "waitpid failed");
            /* libtoby's WIFEXITED is true of a signal's death too: that first */
            if (WIFSIGNALED(st)) {
                int sig = WTERMSIG(st);
                signal(sig, SIG_DFL);
                kill(getpid(), sig);
                exit(128 + sig);
            }
            exit(WEXITSTATUS(st));
        }
    }

    if (procdir &&
        (mt_sys_mount("none", procdir, 0, MT_PRIVATE | MT_REC, 0) != 0 ||
         mt_sys_mount("proc", procdir, "proc", MT_NOSUID | MT_NOEXEC | MT_NODEV, 0) != 0))
        su_error(1, errno, "mount %s failed", procdir);

    if (newroot) {
        if (chroot(newroot) != 0)
            su_error(1, errno, "cannot change root directory to '%s'", newroot);
        if (!newdir) newdir = "/";
    }
    if (newdir && chdir(newdir) != 0)
        su_error(1, errno, "cannot chdir to '%s'", newdir);

    if (force_gid) {
        if (setgroups(0, 0) != 0) su_error(1, errno, "setgroups failed");
        if (setgid((gid_t)sgid) != 0) su_error(1, errno, "setgid() failed");
    }
    if (force_uid && setuid((uid_t)suid) != 0) su_error(1, errno, "setuid() failed");

    fflush(stdout);
    if (su_optind < argc) {
        execvp(argv[su_optind], argv + su_optind);
        su_error(errno == ENOENT ? 127 : 126, errno, "failed to execute %s", argv[su_optind]);
    }
    const char *shell = getenv("SHELL");
    if (!shell) shell = "/bin/sh";
    const char *base = strrchr(shell, '/');
    char *sargv[2] = { (char *)(base ? base + 1 : shell), 0 };
    execv(shell, sargv);
    su_error(errno == ENOENT ? 127 : 126, errno, "failed to execute %s", shell);
    return 1;
}
