/* syscalls.h -- syscall NAME -> NUMBER, for translating an OCI seccomp profile.
 *
 * A seccomp profile names syscalls; cBPF compares numbers. Something has to do
 * that mapping, and how it handles a name it does not know decides whether the
 * filter is a security boundary or a decoration:
 *
 *   - skipping an unknown name in an ALLOW-list profile silently DENIES a
 *     syscall the container was promised;
 *   - skipping an unknown name in a DENY-list profile silently PERMITS a
 *     syscall the profile was written to block.
 *
 * The second is a security hole that looks like a working sandbox. So the
 * lookup below returns -1 and the caller REFUSES TO START THE CONTAINER. A
 * runtime that will not start is a visible problem; a filter with a hole in it
 * is not.
 *
 * The table is deliberately partial -- it covers x86-64 Linux syscalls this
 * kernel actually implements plus the ones profiles commonly block. Adding a
 * name is one line, and the failure mode when one is missing is loud.
 */
#ifndef OCIRUN_SYSCALLS_H
#define OCIRUN_SYSCALLS_H

struct sysent { const char *name; int nr; };

static const struct sysent g_sysents[] = {
    { "read", 0 }, { "write", 1 }, { "open", 2 }, { "close", 3 },
    { "stat", 4 }, { "fstat", 5 }, { "lstat", 6 }, { "poll", 7 },
    { "lseek", 8 }, { "mmap", 9 }, { "mprotect", 10 }, { "munmap", 11 },
    { "brk", 12 }, { "rt_sigaction", 13 }, { "rt_sigprocmask", 14 },
    { "rt_sigreturn", 15 }, { "ioctl", 16 }, { "pread64", 17 },
    { "pwrite64", 18 }, { "readv", 19 }, { "writev", 20 }, { "access", 21 },
    { "pipe", 22 }, { "select", 23 }, { "sched_yield", 24 },
    { "mremap", 25 }, { "msync", 26 }, { "mincore", 27 }, { "madvise", 28 },
    { "shmget", 29 }, { "shmat", 30 }, { "shmctl", 31 },
    { "dup", 32 }, { "dup2", 33 }, { "pause", 34 }, { "nanosleep", 35 },
    { "getitimer", 36 }, { "alarm", 37 }, { "setitimer", 38 },
    { "getpid", 39 }, { "socket", 41 }, { "connect", 42 }, { "accept", 43 },
    { "sendto", 44 }, { "recvfrom", 45 }, { "sendmsg", 46 }, { "recvmsg", 47 },
    { "shutdown", 48 }, { "bind", 49 }, { "listen", 50 },
    { "getsockname", 51 }, { "getpeername", 52 }, { "socketpair", 53 },
    { "setsockopt", 54 }, { "getsockopt", 55 },
    { "clone", 56 }, { "fork", 57 }, { "vfork", 58 }, { "execve", 59 },
    { "exit", 60 }, { "wait4", 61 }, { "kill", 62 }, { "uname", 63 },
    { "semget", 64 }, { "semop", 65 }, { "semctl", 66 }, { "shmdt", 67 },
    { "msgget", 68 }, { "msgsnd", 69 }, { "msgrcv", 70 }, { "msgctl", 71 },
    { "fcntl", 72 }, { "flock", 73 }, { "fsync", 74 }, { "fdatasync", 75 },
    { "truncate", 76 }, { "ftruncate", 77 }, { "getdents", 78 },
    { "getcwd", 79 }, { "chdir", 80 }, { "fchdir", 81 }, { "rename", 82 },
    { "mkdir", 83 }, { "rmdir", 84 }, { "creat", 85 }, { "link", 86 },
    { "unlink", 87 }, { "symlink", 88 }, { "readlink", 89 },
    { "chmod", 90 }, { "fchmod", 91 }, { "chown", 92 }, { "fchown", 93 },
    { "lchown", 94 }, { "umask", 95 }, { "gettimeofday", 96 },
    { "getrlimit", 97 }, { "getrusage", 98 }, { "sysinfo", 99 },
    { "times", 100 }, { "ptrace", 101 }, { "getuid", 102 },
    { "syslog", 103 }, { "getgid", 104 }, { "setuid", 105 },
    { "setgid", 106 }, { "geteuid", 107 }, { "getegid", 108 },
    { "setpgid", 109 }, { "getppid", 110 }, { "getpgrp", 111 },
    { "setsid", 112 }, { "setreuid", 113 }, { "setregid", 114 },
    { "getgroups", 115 }, { "setgroups", 116 }, { "setresuid", 117 },
    { "getresuid", 118 }, { "setresgid", 119 }, { "getresgid", 120 },
    { "getpgid", 121 }, { "capget", 125 }, { "capset", 126 },
    { "rt_sigpending", 127 }, { "rt_sigtimedwait", 128 },
    { "rt_sigsuspend", 130 }, { "sigaltstack", 131 },
    { "utime", 132 }, { "mknod", 133 }, { "personality", 135 },
    { "statfs", 137 }, { "fstatfs", 138 },
    { "sched_setscheduler", 144 }, { "sched_getscheduler", 145 },
    { "mlock", 149 }, { "munlock", 151 },
    { "pivot_root", 155 }, { "prctl", 157 }, { "arch_prctl", 158 },
    { "chroot", 161 }, { "sync", 162 }, { "mount", 165 }, { "umount2", 166 },
    { "swapon", 167 }, { "swapoff", 168 }, { "reboot", 169 },
    { "sethostname", 170 }, { "setdomainname", 171 },
    { "init_module", 175 }, { "delete_module", 176 },
    { "gettid", 186 }, { "setxattr", 188 }, { "getxattr", 191 },
    { "time", 201 }, { "futex", 202 },
    { "sched_setaffinity", 203 }, { "sched_getaffinity", 204 },
    { "getdents64", 217 }, { "set_tid_address", 218 },
    { "clock_gettime", 228 }, { "clock_getres", 229 },
    { "clock_nanosleep", 230 }, { "exit_group", 231 },
    { "epoll_wait", 232 }, { "epoll_ctl", 233 }, { "tgkill", 234 },
    { "openat", 257 }, { "mkdirat", 258 }, { "mknodat", 259 },
    { "fchownat", 260 }, { "newfstatat", 262 }, { "unlinkat", 263 },
    { "renameat", 264 }, { "linkat", 265 }, { "symlinkat", 266 },
    { "readlinkat", 267 }, { "fchmodat", 268 }, { "faccessat", 269 },
    { "ppoll", 271 }, { "unshare", 272 }, { "splice", 275 },
    { "utimensat", 280 }, { "epoll_pwait", 281 }, { "signalfd", 282 },
    { "timerfd_create", 283 }, { "eventfd", 284 }, { "fallocate", 285 },
    { "timerfd_settime", 286 }, { "timerfd_gettime", 287 },
    { "accept4", 288 }, { "signalfd4", 289 }, { "eventfd2", 290 },
    { "epoll_create1", 291 }, { "dup3", 292 }, { "pipe2", 293 },
    { "inotify_init1", 294 }, { "preadv", 295 }, { "pwritev", 296 },
    { "prlimit64", 302 }, { "setns", 308 }, { "getcpu", 309 },
    { "process_vm_readv", 310 }, { "process_vm_writev", 311 },
    { "kcmp", 312 }, { "finit_module", 313 },
    { "sched_setattr", 314 }, { "sched_getattr", 315 },
    { "renameat2", 316 }, { "seccomp", 317 }, { "getrandom", 318 },
    { "memfd_create", 319 }, { "bpf", 321 }, { "execveat", 322 },
    { "userfaultfd", 323 }, { "membarrier", 324 }, { "mlock2", 325 },
    { "copy_file_range", 326 }, { "preadv2", 327 }, { "pwritev2", 328 },
    { "statx", 332 }, { "rseq", 334 },
    { "pidfd_send_signal", 424 }, { "pidfd_open", 434 },
    { "clone3", 435 }, { "close_range", 436 }, { "openat2", 437 },
    { "faccessat2", 439 }, { "landlock_create_ruleset", 444 },
};

/* -1 == unknown. The caller MUST treat that as fatal; see the header note. */
static int syscall_nr_by_name(const char *name)
{
    for (unsigned i = 0; i < sizeof g_sysents / sizeof g_sysents[0]; i++)
        if (strcmp(g_sysents[i].name, name) == 0) return g_sysents[i].nr;
    return -1;
}

#endif /* OCIRUN_SYSCALLS_H */
