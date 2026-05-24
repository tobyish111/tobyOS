/* programs/debugger/main.c -- GDB-like basic debugger.
 *
 * Usage: debugger <program>
 *
 * Commands: break ADDR, continue, step, print ADDR, info regs, quit
 * Uses ABI_SYS_PTRACE for process control.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tobyos/abi/abi.h>

#define DBG_MAX_BREAKPOINTS  16
#define DBG_CMD_MAX          128

struct breakpoint {
    int      active;
    uint64_t addr;
    uint8_t  orig_byte;
    int      id;
};

static struct breakpoint g_bps[DBG_MAX_BREAKPOINTS];
static int g_bp_count = 0;
static int g_target_pid = -1;
static int g_running = 0;

static long sys_ptrace(int request, int pid, long addr, long data) {
    long ret;
    __asm__ volatile (
        "movq %1, %%rax\n\t"
        "movq %2, %%rdi\n\t"
        "movq %3, %%rsi\n\t"
        "movq %4, %%rdx\n\t"
        "movq %5, %%r10\n\t"
        "syscall"
        : "=a"(ret)
        : "r"((long)ABI_SYS_PTRACE),
          "r"((long)request), "r"((long)pid),
          "r"(addr), "r"(data)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#define PTRACE_TRACEME   0
#define PTRACE_PEEKTEXT  1
#define PTRACE_POKETEXT  4
#define PTRACE_CONT      7
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS   12

static uint64_t parse_hex(const char *s) {
    uint64_t val = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        val <<= 4;
        if (c >= '0' && c <= '9') val |= (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') val |= (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (uint64_t)(c - 'A' + 10);
        else break;
    }
    return val;
}

static void cmd_break(const char *arg) {
    if (!arg || !*arg) {
        printf("Usage: break <address>\n");
        return;
    }
    if (g_bp_count >= DBG_MAX_BREAKPOINTS) {
        printf("Too many breakpoints\n");
        return;
    }
    uint64_t addr = parse_hex(arg);
    int id = g_bp_count;
    g_bps[id].active = 1;
    g_bps[id].addr = addr;
    g_bps[id].id = id;
    g_bp_count++;
    printf("Breakpoint %d at 0x%lx\n", id, (unsigned long)addr);
}

static void cmd_continue(void) {
    if (g_target_pid < 0) {
        printf("No target process\n");
        return;
    }
    long rc = sys_ptrace(PTRACE_CONT, g_target_pid, 0, 0);
    if (rc < 0) {
        printf("Continue failed (rc=%ld)\n", rc);
    } else {
        printf("Continuing...\n");
        g_running = 1;
    }
}

static void cmd_step(void) {
    if (g_target_pid < 0) {
        printf("No target process\n");
        return;
    }
    long rc = sys_ptrace(PTRACE_SINGLESTEP, g_target_pid, 0, 0);
    if (rc < 0) {
        printf("Step failed (rc=%ld)\n", rc);
    } else {
        printf("Single step\n");
    }
}

static void cmd_print(const char *arg) {
    if (!arg || !*arg) {
        printf("Usage: print <address>\n");
        return;
    }
    uint64_t addr = parse_hex(arg);
    long val = sys_ptrace(PTRACE_PEEKTEXT, g_target_pid, (long)addr, 0);
    printf("0x%lx: 0x%lx\n", (unsigned long)addr, (unsigned long)val);
}

static void cmd_info_regs(void) {
    if (g_target_pid < 0) {
        printf("No target process\n");
        return;
    }
    uint64_t regs[16] = {0};
    long rc = sys_ptrace(PTRACE_GETREGS, g_target_pid, (long)regs, 0);
    if (rc < 0) {
        printf("Failed to read registers (rc=%ld)\n", rc);
        return;
    }
    printf("rax=0x%016lx  rbx=0x%016lx\n", (unsigned long)regs[0], (unsigned long)regs[1]);
    printf("rcx=0x%016lx  rdx=0x%016lx\n", (unsigned long)regs[2], (unsigned long)regs[3]);
    printf("rsi=0x%016lx  rdi=0x%016lx\n", (unsigned long)regs[4], (unsigned long)regs[5]);
    printf("rsp=0x%016lx  rbp=0x%016lx\n", (unsigned long)regs[6], (unsigned long)regs[7]);
    printf("rip=0x%016lx  rfl=0x%016lx\n", (unsigned long)regs[8], (unsigned long)regs[9]);
}

static void cmd_help(void) {
    printf("Commands:\n");
    printf("  break <addr>  - Set breakpoint at address\n");
    printf("  continue      - Continue execution\n");
    printf("  step          - Single step\n");
    printf("  print <addr>  - Print memory at address\n");
    printf("  info regs     - Show registers\n");
    printf("  quit          - Exit debugger\n");
}

int main(int argc, char **argv) {
    printf("tobyOS debugger v0.1\n");

    if (argc >= 2) {
        printf("Target: %s\n", argv[1]);
        /* Attempt to attach via ptrace */
        g_target_pid = (int)sys_ptrace(PTRACE_TRACEME, 0, 0, 0);
        if (g_target_pid < 0) {
            printf("Note: ptrace not fully available, running in demo mode\n");
            g_target_pid = -1;
        }
    } else {
        printf("Usage: debugger <program>\n");
        printf("Running in interactive mode (no target)\n");
    }

    printf("Type 'help' for commands.\n");

    char cmd[DBG_CMD_MAX];
    while (1) {
        printf("(dbg) ");
        if (!fgets(cmd, sizeof(cmd), stdin)) break;

        /* Strip newline */
        size_t len = strlen(cmd);
        if (len > 0 && cmd[len - 1] == '\n') cmd[--len] = '\0';
        if (len == 0) continue;

        /* Parse command */
        char *arg = cmd;
        while (*arg && *arg != ' ') arg++;
        if (*arg) { *arg = '\0'; arg++; }
        while (*arg == ' ') arg++;

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
            break;
        } else if (strcmp(cmd, "break") == 0 || strcmp(cmd, "b") == 0) {
            cmd_break(arg);
        } else if (strcmp(cmd, "continue") == 0 || strcmp(cmd, "c") == 0) {
            cmd_continue();
        } else if (strcmp(cmd, "step") == 0 || strcmp(cmd, "s") == 0) {
            cmd_step();
        } else if (strcmp(cmd, "print") == 0 || strcmp(cmd, "p") == 0) {
            cmd_print(arg);
        } else if (strcmp(cmd, "info") == 0) {
            if (strcmp(arg, "regs") == 0 || strcmp(arg, "registers") == 0) {
                cmd_info_regs();
            } else {
                printf("Unknown info subcommand: %s\n", arg);
            }
        } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
            cmd_help();
        } else {
            printf("Unknown command: %s (type 'help')\n", cmd);
        }
    }

    printf("Debugger exiting.\n");
    return 0;
}
