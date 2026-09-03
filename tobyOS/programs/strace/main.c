/* strace -- print the system calls a program makes.
 *
 *     strace [-f] [-c] [-o file] [-e trace=a,b] PROGRAM [ARGS...]
 *
 * WHAT THIS REPLACED. The previous /bin/strace traced nothing. It called
 * PTRACE_TRACEME on ITSELF, and when that failed it printed a canned
 * transcript -- "[strace] --- process started ---", a fabricated write()
 * and exit() -- under a comment beginning "In a full implementation, we
 * would:". Output that looks like evidence and is not.
 *
 * It is real now because two things landed: ABI_SYS_PTRACE is dispatched
 * for native programs (it was declared and never wired, which is why the
 * old one said "ptrace not fully available"), and libtoby's exec actually
 * execs instead of spawning a child -- a tracer whose tracee spawns a
 * grandchild traces the wrapper.
 *
 * LINUX-PERSONALITY PROGRAMS ONLY, and it says so rather than attaching
 * to a native one and printing nothing. The kernel raises syscall stops
 * inside linux_syscall; the native dispatcher has none, because parking a
 * native tracee at every syscall boundary wedged the machine reproducibly
 * (see the note at that call site). That covers what anyone would actually
 * strace here -- busybox, bash, python3 -- and `gdb` covers native
 * programs, since breakpoint stops come from the exception path instead.
 *
 * The personality is decided from the target's ELF using the same rule the
 * kernel uses: EI_OSABI == ELFOSABI_LINUX, or a PT_INTERP.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <tobyos/abi/abi.h>

#define PTRACE_TRACEME    0
#define PTRACE_CONT       7
#define PTRACE_KILL       8
#define PTRACE_SYSCALL   24
#define PTRACE_GETREGS   12
#define PTRACE_SETOPTIONS 0x4200
#define PTRACE_O_TRACESYSGOOD 1

struct user_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rax, rcx, rdx, rsi, rdi, orig_rax, rip, cs, eflags;
    uint64_t rsp, ss, fs_base, gs_base, ds, es, fs, gs;
};

static long sc4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
static long pt(long req, int pid, uint64_t addr, uint64_t data) {
    return sc4(ABI_SYS_PTRACE, req, pid, (long)addr, (long)data);
}

static void st_yield(void) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(5L) : "rcx", "r11", "memory");
    (void)r;
}

/* POLL, do not block. A blocking waitpid on a child that is about to PARK
 * rather than exit deadlocks the pair -- and making the kernel's blocking
 * wait understand ptrace stops would put a tracing feature in the path of
 * every waitpid in the system, which wedged unrelated things when it was
 * tried. Returns 1 on a collected status, -1 if the child is gone, 0 if
 * the wait gave up. */
#define WAIT_SPINS 400000
static int wait_bounded(int pid, int *status) {
    for (int i = 0; i < WAIT_SPINS; i++) {
        int rc = waitpid(pid, status, WNOHANG);
        if (rc == pid) return 1;
        if (rc < 0) return -1;
        st_yield();
    }
    return 0;
}

/* ------------------------------------------------------------ the tables */

/* The Linux x86-64 numbers a busybox or glibc binary uses. Only the calls
 * that actually show up in a trace here are named; anything else prints as
 * its number, which is honest and still useful. */
static const char *linux_name(long n) {
    switch (n) {
    case 0:  return "read";        case 1:  return "write";
    case 2:  return "open";        case 3:  return "close";
    case 4:  return "stat";        case 5:  return "fstat";
    case 6:  return "lstat";       case 8:  return "lseek";
    case 9:  return "mmap";        case 10: return "mprotect";
    case 11: return "munmap";      case 12: return "brk";
    case 13: return "rt_sigaction"; case 14: return "rt_sigprocmask";
    case 16: return "ioctl";       case 20: return "writev";
    case 21: return "access";      case 22: return "pipe";
    case 32: return "dup";         case 33: return "dup2";
    case 39: return "getpid";      case 41: return "socket";
    case 42: return "connect";     case 44: return "sendto";
    case 45: return "recvfrom";    case 56: return "clone";
    case 57: return "fork";        case 59: return "execve";
    case 60: return "exit";        case 61: return "wait4";
    case 62: return "kill";        case 63: return "uname";
    case 72: return "fcntl";       case 78: return "getdents";
    case 79: return "getcwd";      case 80: return "chdir";
    case 83: return "mkdir";       case 87: return "unlink";
    case 89: return "readlink";    case 90: return "chmod";
    case 96: return "gettimeofday"; case 97: return "getrlimit";
    case 102: return "getuid";     case 104: return "getgid";
    case 105: return "setuid";     case 106: return "setgid";
    case 107: return "geteuid";    case 108: return "getegid";
    case 110: return "getppid";    case 158: return "arch_prctl";
    case 201: return "time";       case 202: return "futex";
    case 217: return "getdents64"; case 218: return "set_tid_address";
    case 228: return "clock_gettime"; case 231: return "exit_group";
    case 257: return "openat";     case 262: return "newfstatat";
    case 273: return "set_robust_list";
    case 302: return "prlimit64";  case 318: return "getrandom";
    default: return 0;
    }
}

/* The native tobyOS numbers, from tobyos/abi/abi.h. */
static const char *native_name(long n) {
    switch (n) {
    case ABI_SYS_EXIT:      return "exit";
    case ABI_SYS_WRITE:     return "write";
    case ABI_SYS_READ:      return "read";
    case ABI_SYS_YIELD:     return "yield";
    case ABI_SYS_SOCKET:    return "socket";
    case ABI_SYS_BIND:      return "bind";
    case ABI_SYS_SENDTO:    return "sendto";
    case ABI_SYS_RECVFROM:  return "recvfrom";
    case ABI_SYS_OPEN:      return "open";
    case ABI_SYS_CLOSE:     return "close";
    case ABI_SYS_LSEEK:     return "lseek";
    case ABI_SYS_STAT:      return "stat";
    case ABI_SYS_FSTAT:     return "fstat";
    case ABI_SYS_DUP:       return "dup";
    case ABI_SYS_DUP2:      return "dup2";
    case ABI_SYS_UNLINK:    return "unlink";
    case ABI_SYS_MKDIR:     return "mkdir";
    case ABI_SYS_PIPE:      return "pipe";
    case ABI_SYS_SPAWN:     return "spawn";
    case ABI_SYS_WAITPID:   return "waitpid";
    case ABI_SYS_GETPID:    return "getpid";
    case ABI_SYS_GETPPID:   return "getppid";
    case ABI_SYS_FORK:      return "fork";
    case ABI_SYS_EXECVE:    return "execve";
    case ABI_SYS_GETUID:    return "getuid";
    case ABI_SYS_GETGID:    return "getgid";
    case ABI_SYS_CHMOD:     return "chmod";
    case ABI_SYS_IOCTL:     return "ioctl";
    case ABI_SYS_PTRACE:    return "ptrace";
    case ABI_SYS_TCP_CONNECT: return "tcp_connect";
    case ABI_SYS_TCP_SEND:  return "tcp_send";
    case ABI_SYS_TCP_RECV:  return "tcp_recv";
    case ABI_SYS_TCP_CLOSE: return "tcp_close";
    case ABI_SYS_TCP_LISTEN: return "tcp_listen";
    case ABI_SYS_TCP_ACCEPT: return "tcp_accept";
    default: return 0;
    }
}

/* Same rule the kernel uses (elf_is_linux_abi): an explicit GNU/Linux
 * OSABI brand, or a PT_INTERP -- a dynamic loader means glibc/musl, which
 * means Linux numbers. */
static int target_is_linux_abi(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[64];
    if (fread(h, 1, sizeof h, f) != sizeof h) { fclose(f); return 0; }
    if (memcmp(h, "\177ELF", 4) || h[4] != 2) { fclose(f); return 0; }
    if (h[7] == 3 /* ELFOSABI_LINUX / GNU */) { fclose(f); return 1; }

    uint64_t phoff = 0;
    for (int i = 0; i < 8; i++) phoff |= (uint64_t)h[0x20 + i] << (i * 8);
    int phentsize = h[0x36] | (h[0x37] << 8);
    int phnum = h[0x38] | (h[0x39] << 8);
    int linux_abi = 0;
    for (int i = 0; i < phnum && phentsize == 56; i++) {
        unsigned char ph[56];
        if (fseek(f, (long)(phoff + (uint64_t)i * 56), SEEK_SET) != 0) break;
        if (fread(ph, 1, sizeof ph, f) != sizeof ph) break;
        uint32_t type = (uint32_t)ph[0] | ((uint32_t)ph[1] << 8) |
                        ((uint32_t)ph[2] << 16) | ((uint32_t)ph[3] << 24);
        if (type == 3 /* PT_INTERP */) { linux_abi = 1; break; }
    }
    fclose(f);
    return linux_abi;
}

/* ------------------------------------------------------------------ main */

static FILE *out;
static int count_only, follow;
static const char *filter;

struct tally { long nr; unsigned calls; };
static struct tally g_tally[512];
static int g_ntally;

static void tally_add(long nr) {
    for (int i = 0; i < g_ntally; i++)
        if (g_tally[i].nr == nr) { g_tally[i].calls++; return; }
    if (g_ntally < 512) { g_tally[g_ntally].nr = nr; g_tally[g_ntally].calls = 1; g_ntally++; }
}

static int wanted(const char *name, long nr) {
    if (!filter) return 1;
    char buf[256];
    snprintf(buf, sizeof buf, ",%s,", filter);
    char one[64];
    snprintf(one, sizeof one, ",%s,", name ? name : "");
    if (name && strstr(buf, one)) return 1;
    char num[32];
    snprintf(num, sizeof num, ",%ld,", nr);
    return strstr(buf, num) != NULL;
}

int main(int argc, char **argv) {
    out = stderr;
    int argi = 1;
    for (; argi < argc; argi++) {
        const char *a = argv[argi];
        if (!strcmp(a, "-c")) { count_only = 1; continue; }
        if (!strcmp(a, "-f")) { follow = 1; continue; }
        if (!strcmp(a, "-o") && argi + 1 < argc) {
            FILE *f = fopen(argv[++argi], "w");
            if (!f) { fprintf(stderr, "strace: cannot write %s\n", argv[argi]); return 1; }
            out = f;
            continue;
        }
        if (!strncmp(a, "-e", 2)) {
            const char *spec = a[2] ? a + 2 : (argi + 1 < argc ? argv[++argi] : "");
            if (!strncmp(spec, "trace=", 6)) filter = spec + 6;
            continue;
        }
        if (!strcmp(a, "--version")) { printf("strace (tobyOS) 1.0\n"); return 0; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: strace [-c] [-f] [-o file] [-e trace=a,b] PROGRAM [ARGS...]\n");
            return 0;
        }
        if (a[0] == '-' && a[1]) continue;
        break;
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: strace [-c] [-o file] PROGRAM [ARGS...]\n");
        return 2;
    }
    (void)follow;   /* child-following needs the fork/clone event stops */

    const char *prog = argv[argi];
    /* The target's ELF decides its personality by the kernel's own rule.
     * Syscall stops exist ONLY on the Linux path: parking a native tracee
     * happens with the big kernel lock held and wedges a real multi-core
     * machine (proven on hardware 2026-09-02 -- see the long note at the
     * native dispatcher in src/syscall.c). Refusing by name beats attaching
     * and printing an empty trace, and beats taking the box down. */
    int is_linux = target_is_linux_abi(prog);
    if (!is_linux) {
        fprintf(stderr,
                "strace: %s is a native tobyOS program. Syscall stops exist "
                "only on the Linux personality here, so tracing it would "
                "attach and print nothing. Use /bin/gdb for native programs "
                "(breakpoints work there), or strace a busybox, bash or "
                "python3 binary.\n", prog);
        return 1;
    }

    int pid = fork();
    if (pid < 0) { fprintf(stderr, "strace: fork failed\n"); return 1; }
    if (pid == 0) {
        pt(PTRACE_TRACEME, 0, 0, 0);
        execvp(prog, &argv[argi]);
        _exit(127);
    }

    /* The exec stop: the image is mapped and nothing of it has run yet. */
    int status = 0;
    if (wait_bounded(pid, &status) != 1) {
        fprintf(stderr, "strace: the target never stopped\n");
        return 1;
    }
    if (WIFEXITED(status)) {
        fprintf(out, "+++ exited with %d +++\n", WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    /* TRACESYSGOOD marks syscall stops so they cannot be confused with a
     * real SIGTRAP. */
    pt(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);

    unsigned long ncalls = 0;
    int entering = 1;
    long pending_nr = -1;
    const char *pending_name = 0;
    struct user_regs r;

    for (;;) {
        if (pt(PTRACE_SYSCALL, pid, 0, 0) < 0) break;
        if (wait_bounded(pid, &status) != 1) break;
        if (WIFEXITED(status)) {
            if (!count_only && !entering) fprintf(out, ") = ?\n");
            fprintf(out, "+++ exited with %d +++\n", WEXITSTATUS(status));
            break;
        }
        if (!WIFSTOPPED(status)) {
            fprintf(out, "+++ killed +++\n");
            break;
        }
        memset(&r, 0, sizeof r);
        if (pt(PTRACE_GETREGS, pid, 0, (uint64_t)(uintptr_t)&r) < 0) continue;

        if (entering) {
            pending_nr = (long)r.orig_rax;
            pending_name = is_linux ? linux_name(pending_nr) : native_name(pending_nr);
            ncalls++;
            if (count_only) {
                tally_add(pending_nr);
            } else if (wanted(pending_name, pending_nr)) {
                if (pending_name)
                    fprintf(out, "%s(0x%llx, 0x%llx, 0x%llx", pending_name,
                            (unsigned long long)r.rdi, (unsigned long long)r.rsi,
                            (unsigned long long)r.rdx);
                else
                    fprintf(out, "syscall_%ld(0x%llx, 0x%llx, 0x%llx", pending_nr,
                            (unsigned long long)r.rdi, (unsigned long long)r.rsi,
                            (unsigned long long)r.rdx);
            }
            entering = 0;
        } else {
            if (!count_only && wanted(pending_name, pending_nr)) {
                long rv = (long)r.rax;
                if (rv < 0 && rv > -4096)
                    fprintf(out, ") = -1 (errno %ld)\n", -rv);
                else
                    fprintf(out, ") = %ld\n", rv);
            }
            entering = 1;
        }
    }

    if (count_only) {
        fprintf(out, "%% time     calls  syscall\n");
        fprintf(out, "------ --------- ----------------\n");
        for (int i = 0; i < g_ntally; i++) {
            const char *nm = is_linux ? linux_name(g_tally[i].nr)
                                      : native_name(g_tally[i].nr);
            char num[32];
            if (!nm) { snprintf(num, sizeof num, "syscall_%ld", g_tally[i].nr); nm = num; }
            fprintf(out, "%6s %9u  %s\n", "-", g_tally[i].calls, nm);
        }
        fprintf(out, "------ --------- ----------------\n");
        fprintf(out, "       %9lu  total\n", ncalls);
    } else {
        fprintf(out, "%lu syscalls traced\n", ncalls);
    }
    if (out != stderr) fclose(out);
    return 0;
}
