/* gdb / debugger -- a real breakpoint debugger over ptrace.
 *
 *     gdb PROGRAM [ARGS...]
 *
 *     run                 start (or restart) the program under the debugger
 *     break SYM | *0xADDR set a breakpoint
 *     info breakpoints    list them
 *     delete N            remove one
 *     continue            resume until the next breakpoint
 *     stepi               execute exactly one instruction
 *     info registers      dump the register file
 *     x/N ADDR            examine N words of memory
 *     backtrace           walk the frame-pointer chain
 *     print SYM|ADDR      one word, and the symbol it lands in
 *     kill                terminate the target
 *     quit
 *
 * WHAT THIS REPLACED. The previous /bin/debugger never debugged anything:
 * it called PTRACE_TRACEME on ITSELF and treated the result as a target
 * pid, its `break` only wrote an address into a table, and it printed
 * "ptrace not fully available, running in demo mode" when that self-trace
 * failed. There was no fork, no exec, no int3, no stop -- a prompt with the
 * right words on it.
 *
 * HOW A BREAKPOINT ACTUALLY WORKS, since that is the whole tool:
 *   1. PEEKTEXT the 8 bytes at the address and remember them.
 *   2. POKETEXT the same 8 bytes with the low byte replaced by 0xCC (int3).
 *   3. Continue. The CPU raises #BP; the kernel parks the tracee and wakes
 *      us with a SIGTRAP stop.
 *   4. rip now points ONE PAST the 0xCC, so rewind it by 1 and put the
 *      original byte back.
 *   5. To continue past that breakpoint we must single-step ONCE with the
 *      original byte in place, then re-plant the 0xCC -- otherwise the
 *      first instruction executed is the int3 again and the program never
 *      moves.
 * Step 5 is the part that is easy to leave out and produces a debugger
 * that hits every breakpoint exactly once and then loops forever.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <tobyos/abi/abi.h>

#define PTRACE_TRACEME    0
#define PTRACE_PEEKTEXT   1
#define PTRACE_PEEKDATA   2
#define PTRACE_POKETEXT   4
#define PTRACE_POKEDATA   5
#define PTRACE_CONT       7
#define PTRACE_KILL       8
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS   12
#define PTRACE_SETREGS   13

#define MAX_BP 32

/* The Linux user_regs_struct layout the kernel's GETREGS fills. */
struct user_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rax, rcx, rdx, rsi, rdi, orig_rax, rip, cs, eflags;
    uint64_t rsp, ss, fs_base, gs_base, ds, es, fs, gs;
};

/* EVERY WAIT HERE IS BOUNDED. A debugger that blocks forever on a stop
 * that is never coming does not just fail -- it takes the machine with it:
 * an unbounded waitpid in `run` hung a whole boot, and the harness then
 * reported nothing at all rather than one failing case. WNOHANG plus a
 * deadline turns "no stop arrived" into a message. */
#define WAIT_SPINS 200000

/* libtoby has no yield wrapper; ABI_SYS_YIELD is 5. */
static void dbg_yield(void) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(5L) : "rcx", "r11", "memory");
    (void)r;
}

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

struct bp { int used, planted; uint64_t addr; uint64_t orig; char name[128]; };
static struct bp g_bp[MAX_BP];
static int g_target = -1, g_running;
static const char *g_prog;
static char **g_argv;

/* ------------------------------------------------------- symbols, for names */

struct sym { uint64_t value, size; char name[128]; };
static struct sym g_syms[2048];
static int g_nsym;

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const unsigned char *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void load_symbols(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static unsigned char img[4 << 20];
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
    if (n < 64 || memcmp(img, "\177ELF", 4) || img[4] != 2) return;
    uint64_t shoff = rd64(img + 0x28);
    int shnum = (int)(img[0x3C] | (img[0x3D] << 8));
    if (shoff + (uint64_t)shnum * 64 > n) return;
    for (int i = 0; i < shnum; i++) {
        const unsigned char *sh = img + shoff + (uint64_t)i * 64;
        if (rd32(sh + 4) != 2 /* SHT_SYMTAB */) continue;
        uint64_t off = rd64(sh + 24), size = rd64(sh + 32);
        uint32_t link = rd32(sh + 40);
        if (off + size > n || link >= (uint32_t)shnum) continue;
        const unsigned char *ls = img + shoff + (uint64_t)link * 64;
        uint64_t so = rd64(ls + 24), ss = rd64(ls + 32);
        if (so + ss > n) continue;
        const char *str = (const char *)(img + so);
        for (uint64_t k = 0; k + 24 <= size && g_nsym < 2048; k += 24) {
            const unsigned char *sym = img + off + k;
            uint32_t nameoff = rd32(sym);
            unsigned char info = sym[4];
            uint16_t shndx = (uint16_t)(sym[6] | (sym[7] << 8));
            if (shndx == 0 || nameoff == 0 || nameoff >= ss) continue;
            if ((info & 0xF) != 2 /* FUNC */ && (info & 0xF) != 1 /* OBJECT */)
                continue;
            g_syms[g_nsym].value = rd64(sym + 8);
            g_syms[g_nsym].size  = rd64(sym + 16);
            snprintf(g_syms[g_nsym].name, sizeof g_syms[0].name, "%s", str + nameoff);
            g_nsym++;
        }
    }
}

static int sym_addr(const char *name, uint64_t *out) {
    for (int i = 0; i < g_nsym; i++)
        if (!strcmp(g_syms[i].name, name)) { *out = g_syms[i].value; return 1; }
    return 0;
}

static const char *sym_for(uint64_t addr, uint64_t *off) {
    const char *best = NULL;
    uint64_t bv = 0;
    for (int i = 0; i < g_nsym; i++) {
        if (addr < g_syms[i].value) continue;
        if (g_syms[i].size && addr >= g_syms[i].value + g_syms[i].size) continue;
        if (!best || g_syms[i].value >= bv) { best = g_syms[i].name; bv = g_syms[i].value; }
    }
    if (best && off) *off = addr - bv;
    return best;
}

/* -------------------------------------------------------------- breakpoints */

static int plant(struct bp *b) {
    long word = pt(PTRACE_PEEKTEXT, g_target, b->addr, 0);
    if (word == -1) return -1;
    b->orig = (uint64_t)word;
    uint64_t patched = (b->orig & ~0xFFull) | 0xCC;
    if (pt(PTRACE_POKETEXT, g_target, b->addr, patched) < 0) return -1;
    b->planted = 1;
    return 0;
}

static int unplant(struct bp *b) {
    if (!b->planted) return 0;
    /* Re-read rather than writing back the remembered word wholesale: the
     * seven bytes beside the 0xCC may have been written since. */
    long cur = pt(PTRACE_PEEKTEXT, g_target, b->addr, 0);
    uint64_t restored = ((uint64_t)cur & ~0xFFull) | (b->orig & 0xFF);
    if (pt(PTRACE_POKETEXT, g_target, b->addr, restored) < 0) return -1;
    b->planted = 0;
    return 0;
}

static struct bp *bp_at(uint64_t addr) {
    for (int i = 0; i < MAX_BP; i++)
        if (g_bp[i].used && g_bp[i].addr == addr) return &g_bp[i];
    return NULL;
}

/* Reports where the target stopped, and rewinds rip if it landed on one of
 * our int3s. Returns 1 if the process is still alive. */
static int report_stop(int status) {
    if (WIFEXITED(status)) {
        printf("[Inferior 1 (process %d) exited %s%d]\n", g_target,
               WEXITSTATUS(status) ? "with code " : "normally", WEXITSTATUS(status));
        g_running = 0;
        g_target = -1;
        return 0;
    }
    if (!WIFSTOPPED(status)) {
        printf("Program terminated.\n");
        g_running = 0;
        g_target = -1;
        return 0;
    }
    struct user_regs r;
    memset(&r, 0, sizeof r);
    if (pt(PTRACE_GETREGS, g_target, 0, (uint64_t)(uintptr_t)&r) < 0) {
        printf("Stopped, but the registers could not be read.\n");
        return 1;
    }
    struct bp *b = bp_at(r.rip - 1);
    if (b) {
        /* The CPU pushed rip past the int3; put it back on the instruction
         * the user asked about, and restore the byte so it can execute. */
        r.rip -= 1;
        pt(PTRACE_SETREGS, g_target, 0, (uint64_t)(uintptr_t)&r);
        unplant(b);
        uint64_t off = 0;
        const char *nm = sym_for(r.rip, &off);
        printf("\nBreakpoint %d, 0x%016llx in %s%s+%llu ()\n",
               (int)(b - g_bp), (unsigned long long)r.rip,
               nm ? nm : "?", nm ? "" : "", (unsigned long long)off);
    } else {
        uint64_t off = 0;
        const char *nm = sym_for(r.rip, &off);
        printf("0x%016llx in %s+%llu ()\n", (unsigned long long)r.rip,
               nm ? nm : "?", (unsigned long long)off);
    }
    return 1;
}

/* Waits for the target to stop or exit, but never forever. Returns 1 if a
 * status was collected, 0 if the wait gave up. */
/* Returns 1 when a status was collected, -1 when the child is GONE (no
 * status to collect), and 0 when the wait simply gave up. The three are
 * different things and only the last is a failure -- reporting a finished
 * program as "no stop arrived" is a lie about a program that ran fine. */
static int wait_bounded(int *status) {
    for (int i = 0; i < WAIT_SPINS; i++) {
        int rc = waitpid(g_target, status, WNOHANG);
        if (rc == g_target) return 1;
        if (rc < 0) return -1;
        /* YIELD, do not sleep. usleep here spun without giving the
         * scheduler anything to switch to, so the freshly forked child
         * never got a timeslice to reach its execve -- the target's image
         * loaded only AFTER this loop gave up, which read as "the program
         * never reached its exec stop" about a program that had not been
         * allowed to start. */
        dbg_yield();
    }
    return 0;
}

static int wait_stop(void) {
    int status = 0;
    int rc = wait_bounded(&status);
    if (rc < 0) {
        /* The tracee finished and was reaped before this waitpid reached
         * it, so there is no status left to read -- the exit CODE is lost,
         * but the fact of the exit is not, and saying so beats claiming
         * nothing happened. */
        printf("[Inferior 1 (process %d) exited]\n", g_target);
        g_target = -1;
        g_running = 0;
        return 0;
    }
    if (rc == 0) {
        printf("The program did not stop (no ptrace stop arrived).\n");
        g_running = 0;
        return 0;
    }
    return report_stop(status);
}

/* Continue, stepping over a breakpoint we are currently sitting on. */
static void do_continue(void) {
    if (g_target < 0) { printf("The program is not being run.\n"); return; }
    struct user_regs r;
    memset(&r, 0, sizeof r);
    pt(PTRACE_GETREGS, g_target, 0, (uint64_t)(uintptr_t)&r);
    struct bp *here = bp_at(r.rip);
    if (here && !here->planted) {
        /* Step the ONE original instruction, then put the trap back. Skip
         * this and the very next thing executed is the int3 again. */
        pt(PTRACE_SINGLESTEP, g_target, 0, 0);
        int status = 0;
        if (wait_bounded(&status) != 1 || !WIFSTOPPED(status)) {
            report_stop(status);
            return;
        }
        plant(here);
    }
    /* Anything else that was temporarily lifted goes back too. */
    for (int i = 0; i < MAX_BP; i++)
        if (g_bp[i].used && !g_bp[i].planted) plant(&g_bp[i]);

    printf("Continuing.\n");
    if (pt(PTRACE_CONT, g_target, 0, 0) < 0) { printf("Continue failed.\n"); return; }
    wait_stop();
}

static void do_run(void) {
    if (g_target > 0) { printf("The program is already running.\n"); return; }
    int pid = fork();
    if (pid < 0) { printf("fork failed\n"); return; }
    if (pid == 0) {
        pt(PTRACE_TRACEME, 0, 0, 0);
        /* execvp REPLACES this process, which is what being the tracee
         * requires -- the program has to run as the pid the tracer is
         * watching. That was not always true: libtoby's exec used to spawn
         * a child and wait for it, so a traced process that exec'd stayed
         * a traced WRAPPER while an untraced grandchild ran the program,
         * and the exec stop could not happen. Fixed in libtoby; this uses
         * the ordinary call, and gets a PATH search with it. */
        execvp(g_prog, g_argv);
        _exit(127);
    }
    g_target = pid;
    g_running = 1;
    printf("Starting program: %s\n", g_prog);
    /* The child stops at its first traced syscall boundary, which is the
     * execvp itself -- that is the moment its image is loaded and the
     * breakpoints can be written. */
    int status = 0;
    if (wait_bounded(&status) != 1) {
        printf("The program never reached its exec stop.\n");
        return;
    }
    if (WIFEXITED(status)) { report_stop(status); return; }
    for (int i = 0; i < MAX_BP; i++)
        if (g_bp[i].used) plant(&g_bp[i]);
    if (pt(PTRACE_CONT, g_target, 0, 0) < 0) { printf("Could not start the program.\n"); return; }
    wait_stop();
}

static uint64_t parse_addr(const char *s, int *ok) {
    *ok = 1;
    while (*s == ' ') s++;
    if (*s == '*') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return strtoull(s + 2, NULL, 16);
    if ((*s >= '0' && *s <= '9')) return strtoull(s, NULL, 16);
    uint64_t a;
    if (sym_addr(s, &a)) return a;
    *ok = 0;
    return 0;
}

static void do_break(const char *arg) {
    if (!arg || !*arg) { printf("Usage: break SYMBOL | *0xADDR\n"); return; }
    int ok = 0;
    uint64_t addr = parse_addr(arg, &ok);
    if (!ok) { printf("Function \"%s\" not defined.\n", arg); return; }
    for (int i = 0; i < MAX_BP; i++) {
        if (g_bp[i].used) continue;
        g_bp[i].used = 1;
        g_bp[i].planted = 0;
        g_bp[i].addr = addr;
        snprintf(g_bp[i].name, sizeof g_bp[i].name, "%s", arg);
        uint64_t off = 0;
        const char *nm = sym_for(addr, &off);
        printf("Breakpoint %d at 0x%llx", i, (unsigned long long)addr);
        if (nm) printf(": %s+%llu", nm, (unsigned long long)off);
        printf("\n");
        if (g_target > 0) plant(&g_bp[i]);
        return;
    }
    printf("Too many breakpoints.\n");
}

static void do_info_break(void) {
    int any = 0;
    for (int i = 0; i < MAX_BP; i++) {
        if (!g_bp[i].used) continue;
        if (!any) printf("Num     Type           Disp Enb Address            What\n");
        any = 1;
        printf("%-7d breakpoint     keep y   0x%016llx %s\n", i,
               (unsigned long long)g_bp[i].addr, g_bp[i].name);
    }
    if (!any) printf("No breakpoints or watchpoints.\n");
}

static void do_delete(const char *arg) {
    if (!arg || !*arg) {
        for (int i = 0; i < MAX_BP; i++)
            if (g_bp[i].used) { if (g_target > 0) unplant(&g_bp[i]); g_bp[i].used = 0; }
        printf("All breakpoints deleted.\n");
        return;
    }
    int n = atoi(arg);
    if (n < 0 || n >= MAX_BP || !g_bp[n].used) { printf("No breakpoint number %d.\n", n); return; }
    if (g_target > 0) unplant(&g_bp[n]);
    g_bp[n].used = 0;
    printf("Deleted breakpoint %d.\n", n);
}

static void do_stepi(void) {
    if (g_target < 0) { printf("The program is not being run.\n"); return; }
    struct user_regs r;
    memset(&r, 0, sizeof r);
    pt(PTRACE_GETREGS, g_target, 0, (uint64_t)(uintptr_t)&r);
    struct bp *here = bp_at(r.rip);
    if (here && here->planted) unplant(here);
    if (pt(PTRACE_SINGLESTEP, g_target, 0, 0) < 0) {
        printf("Single step failed.\n");
        return;
    }
    wait_stop();
}

static void do_regs(void) {
    if (g_target < 0) { printf("The program has no registers now.\n"); return; }
    struct user_regs r;
    memset(&r, 0, sizeof r);
    if (pt(PTRACE_GETREGS, g_target, 0, (uint64_t)(uintptr_t)&r) < 0) {
        printf("Could not read registers.\n");
        return;
    }
    printf("rax 0x%-16llx rbx 0x%-16llx rcx 0x%llx\n",
           (unsigned long long)r.rax, (unsigned long long)r.rbx, (unsigned long long)r.rcx);
    printf("rdx 0x%-16llx rsi 0x%-16llx rdi 0x%llx\n",
           (unsigned long long)r.rdx, (unsigned long long)r.rsi, (unsigned long long)r.rdi);
    printf("rbp 0x%-16llx rsp 0x%-16llx rip 0x%llx\n",
           (unsigned long long)r.rbp, (unsigned long long)r.rsp, (unsigned long long)r.rip);
    printf("eflags 0x%llx\n", (unsigned long long)r.eflags);
}

static void do_examine(const char *cmd, const char *arg) {
    int count = 1;
    if (cmd[1] == '/') count = atoi(cmd + 2);
    if (count < 1) count = 1;
    if (count > 64) count = 64;
    int ok = 0;
    uint64_t addr = parse_addr(arg, &ok);
    if (!ok) { printf("No symbol \"%s\".\n", arg); return; }
    if (g_target < 0) { printf("The program is not being run.\n"); return; }
    for (int i = 0; i < count; i++) {
        long w = pt(PTRACE_PEEKDATA, g_target, addr + (uint64_t)i * 8, 0);
        printf("0x%016llx: 0x%016llx\n",
               (unsigned long long)(addr + (uint64_t)i * 8), (unsigned long long)w);
    }
}

static void do_backtrace(void) {
    if (g_target < 0) { printf("No stack.\n"); return; }
    struct user_regs r;
    memset(&r, 0, sizeof r);
    if (pt(PTRACE_GETREGS, g_target, 0, (uint64_t)(uintptr_t)&r) < 0) {
        printf("No stack.\n");
        return;
    }
    uint64_t off = 0;
    const char *nm = sym_for(r.rip, &off);
    printf("#0  0x%016llx in %s+%llu ()\n", (unsigned long long)r.rip,
           nm ? nm : "??", (unsigned long long)off);
    /* Walks the saved-rbp chain. That only works for frames a compiler
     * built with a frame pointer; -O2 code usually has none, and saying so
     * beats printing invented frames. */
    uint64_t rbp = r.rbp;
    for (int depth = 1; depth < 16 && rbp; depth++) {
        long saved_rbp = pt(PTRACE_PEEKDATA, g_target, rbp, 0);
        long ret = pt(PTRACE_PEEKDATA, g_target, rbp + 8, 0);
        if (ret == -1 || ret == 0) break;
        nm = sym_for((uint64_t)ret, &off);
        if (!nm) break;
        printf("#%-2d 0x%016llx in %s+%llu ()\n", depth,
               (unsigned long long)ret, nm, (unsigned long long)off);
        if ((uint64_t)saved_rbp <= rbp) break;    /* not a walkable chain */
        rbp = (uint64_t)saved_rbp;
    }
}

static void do_print(const char *arg) {
    if (!arg || !*arg) { printf("Usage: print SYMBOL | 0xADDR\n"); return; }
    int ok = 0;
    uint64_t addr = parse_addr(arg, &ok);
    if (!ok) { printf("No symbol \"%s\" in current context.\n", arg); return; }
    if (g_target < 0) { printf("The program is not being run.\n"); return; }
    long w = pt(PTRACE_PEEKDATA, g_target, addr, 0);
    printf("$1 = %ld\t(0x%llx at 0x%llx)\n", w, (unsigned long long)w,
           (unsigned long long)addr);
}

static void do_kill(void) {
    if (g_target < 0) { printf("The program is not being run.\n"); return; }
    pt(PTRACE_KILL, g_target, 0, 0);
    int status = 0;
    wait_bounded(&status);
    printf("[Inferior 1 (process %d) killed]\n", g_target);
    g_target = -1;
    g_running = 0;
}

static void do_help(void) {
    printf("run                  start the program\n"
           "break SYM | *0xADDR  set a breakpoint\n"
           "info breakpoints     list breakpoints\n"
           "delete [N]           remove one, or all\n"
           "continue             resume\n"
           "stepi                one instruction\n"
           "info registers       dump registers\n"
           "x/N ADDR             examine memory\n"
           "backtrace            frame-pointer chain\n"
           "print SYM|ADDR       one word\n"
           "kill                 terminate the target\n"
           "quit\n");
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "gdb";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;

    int argi = 1;
    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "--version")) { printf("gdb (tobyOS) 1.0\n"); return 0; }
        if (!strcmp(argv[argi], "-q") || !strcmp(argv[argi], "--quiet")) continue;
        if (!strcmp(argv[argi], "-h") || !strcmp(argv[argi], "--help")) { do_help(); return 0; }
        if (!strcmp(argv[argi], "--batch")) continue;
        break;
    }

    printf("tobyOS %s -- a breakpoint debugger over ptrace.\n", base);
    if (argi < argc) {
        g_prog = argv[argi];
        g_argv = &argv[argi];
        load_symbols(g_prog);
        printf("Reading symbols from %s...%s\n", g_prog,
               g_nsym ? "" : "(no symbols found)");
    } else {
        printf("No executable file specified.\n");
    }

    char line[256];
    while (1) {
        printf("(gdb) ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!line[0]) continue;

        char *arg = line;
        while (*arg && *arg != ' ') arg++;
        if (*arg) { *arg = 0; arg++; }
        while (*arg == ' ') arg++;

        if (!strcmp(line, "quit") || !strcmp(line, "q")) break;
        else if (!strcmp(line, "run") || !strcmp(line, "r")) do_run();
        else if (!strcmp(line, "break") || !strcmp(line, "b")) do_break(arg);
        else if (!strcmp(line, "delete") || !strcmp(line, "d")) do_delete(arg);
        else if (!strcmp(line, "continue") || !strcmp(line, "c")) do_continue();
        else if (!strcmp(line, "stepi") || !strcmp(line, "si") ||
                 !strcmp(line, "step")  || !strcmp(line, "s")) do_stepi();
        else if (!strcmp(line, "backtrace") || !strcmp(line, "bt")) do_backtrace();
        else if (!strcmp(line, "print") || !strcmp(line, "p")) do_print(arg);
        else if (!strcmp(line, "kill")) do_kill();
        else if (line[0] == 'x') do_examine(line, arg);
        else if (!strcmp(line, "info")) {
            if (!strncmp(arg, "reg", 3)) do_regs();
            else if (!strncmp(arg, "break", 5)) do_info_break();
            else printf("Undefined info command: \"%s\".\n", arg);
        }
        else if (!strcmp(line, "help") || !strcmp(line, "h")) do_help();
        else printf("Undefined command: \"%s\".  Try \"help\".\n", line);
    }

    if (g_target > 0) do_kill();
    return 0;
}
