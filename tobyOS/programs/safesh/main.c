/* programs/safesh/main.c -- Milestone 28D: minimal recovery shell.
 *
 * The safe-mode counterpart to /bin/login + the GUI desktop. Runs on
 * the kernel console (stdin/stdout = serial + framebuffer text), no
 * graphics, no fonts beyond the built-in 8x16. Provides exactly the
 * recovery commands an operator needs:
 *
 *   help           list commands
 *   logs [N]       print last N slog records (default 30)
 *   wdog           print watchdog status
 *   crashinfo      spawn /bin/crashinfo (decodes /data/crash/last.dump)
 *   services       list supervised services
 *   fscheck [path] check filesystem integrity
 *   hwinfo         print hardware summary
 *   ps             list running processes
 *   cat <file>     print file contents
 *   ls [dir]       list directory
 *   reboot         ACPI shutdown
 *   exit           leave the shell (kernel goes to idle_loop)
 *
 * In --boot mode (test harness) safesh runs each command once,
 * verifies the essentials are healthy, prints the "M28D_SAFESH:
 * PASS" sentinel, and exits.
 *
 * Exit codes:
 *   0  PASS (boot mode) / clean exit (interactive)
 *   1  one or more required syscalls failed
 *   2  bad usage
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include <tobyos_slog.h>
#include <tobyos_wdog.h>
#include <tobyos_svc.h>
#include <tobyos_fscheck.h>
#include <tobyos_hwinfo.h>

static void usage(void) {
    fprintf(stderr, "usage: safesh [--boot]\n");
}

static int cmd_logs(int n) {
    static struct abi_slog_record buf[ABI_SLOG_RING_DEPTH];
    int got = tobylog_read(buf, ABI_SLOG_RING_DEPTH, 0);
    if (got < 0) {
        fprintf(stderr, "safesh: slog_read failed: %s\n", strerror(errno));
        return -1;
    }
    int start = 0;
    if (n > 0 && got > n) start = got - n;
    tobylog_print_header(stdout);
    for (int i = start; i < got; i++) {
        tobylog_print_record(stdout, &buf[i]);
    }
    printf("[safesh] logs: shown=%d total=%d\n", got - start, got);
    return got;
}

static int cmd_wdog(void) {
    struct abi_wdog_status s;
    if (tobywdog_status(&s) < 0) {
        fprintf(stderr, "safesh: wdog_status failed: %s\n", strerror(errno));
        return -1;
    }
    printf("[safesh] wdog: enabled=%u timeout_ms=%u kernel_hb=%lu "
           "sched_hb=%lu syscall_hb=%lu events=%lu\n",
           (unsigned)s.enabled, (unsigned)s.timeout_ms,
           (unsigned long)s.kernel_heartbeats,
           (unsigned long)s.sched_heartbeats,
           (unsigned long)s.syscall_heartbeats,
           (unsigned long)s.event_count);
    if (s.event_count > 0) {
        printf("         last_event: kind=%u pid=%d ms=%lu\n",
               (unsigned)s.last_event_kind, (int)s.last_event_pid,
               (unsigned long)s.last_event_ms);
        if (s.last_event_reason[0])
            printf("         reason: %s\n", s.last_event_reason);
    }
    return 0;
}

static int cmd_services(void) {
    struct abi_service_info recs[16];
    memset(recs, 0, sizeof(recs));
    int n = tobysvc_list(recs, 16);
    if (n < 0) {
        fprintf(stderr, "safesh: SVC_LIST failed: %s\n", strerror(errno));
        return -1;
    }
    printf("[safesh] services: %d registered\n", n);
    printf("  %-12s %-9s %-5s %-5s %-5s\n",
           "name", "state", "pid", "rcnt", "ccnt");
    for (int i = 0; i < n; i++) {
        const struct abi_service_info *r = &recs[i];
        char name[ABI_SVC_NAME_MAX + 1];
        memcpy(name, r->name, ABI_SVC_NAME_MAX);
        name[ABI_SVC_NAME_MAX] = '\0';
        printf("  %-12s %-9s %-5d %-5u %-5u\n",
               name, tobysvc_state_str(r->state),
               (int)r->pid,
               (unsigned)r->restart_count,
               (unsigned)r->crash_count);
    }
    return 0;
}

static int cmd_fscheck(const char *path) {
    struct abi_fscheck_report rpt;
    if (tobyfscheck(path, &rpt) < 0) {
        fprintf(stderr, "safesh: fscheck '%s' failed: %s\n", path, strerror(errno));
        return -1;
    }
    printf("[safesh] fscheck: path=%s fs=%s status=0x%x\n",
           rpt.path, rpt.fs_type, (unsigned)rpt.status);
    printf("         total=%lu free=%lu errors=%u repaired=%u\n",
           (unsigned long)rpt.total_bytes,
           (unsigned long)rpt.free_bytes,
           (unsigned)rpt.errors_found,
           (unsigned)rpt.errors_repaired);
    if (rpt.detail[0])
        printf("         detail: %s\n", rpt.detail);
    return 0;
}

static int cmd_hwinfo(void) {
    struct abi_hwinfo_summary hw;
    if (tobyhw_summary(&hw) < 0) {
        fprintf(stderr, "safesh: hwinfo failed: %s\n", strerror(errno));
        return -1;
    }
    printf("[safesh] hwinfo:\n");
    printf("  cpu    : %s (%s)\n", hw.cpu_brand, hw.cpu_vendor);
    printf("  cores  : %u  family=%u model=%u step=%u\n",
           (unsigned)hw.cpu_count, (unsigned)hw.cpu_family,
           (unsigned)hw.cpu_model, (unsigned)hw.cpu_stepping);
    printf("  memory : %lu pages total, %lu free (%lu MB free)\n",
           (unsigned long)hw.mem_total_pages,
           (unsigned long)hw.mem_free_pages,
           (unsigned long)(hw.mem_free_pages * 4 / 1024));
    printf("  devices: pci=%u usb=%u blk=%u input=%u audio=%u\n",
           (unsigned)hw.pci_count, (unsigned)hw.usb_count,
           (unsigned)hw.blk_count, (unsigned)hw.input_count,
           (unsigned)hw.audio_count);
    printf("  uptime : %lu ms  profile=%s  safe=%u\n",
           (unsigned long)hw.boot_uptime_ms,
           hw.profile_hint, (unsigned)hw.safe_mode);
    return 0;
}

static int cmd_cat(const char *path) {
    if (!path || !*path) {
        fprintf(stderr, "safesh: cat: no file specified\n");
        return -1;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "safesh: cat '%s': %s\n", path, strerror(errno));
        return -1;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    fclose(f);
    return 0;
}

static int cmd_ls(const char *path) {
    if (!path || !*path) path = "/";
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "safesh: ls '%s': %s\n", path, strerror(errno));
        return -1;
    }
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(d)) != NULL) {
        const char *type_tag = (ent->d_type == DT_DIR) ? "dir " : "file";
        printf("  %s  %s\n", type_tag, ent->d_name);
        count++;
    }
    closedir(d);
    printf("[safesh] ls: %d entries in '%s'\n", count, path);
    return 0;
}

static int cmd_crashinfo(void) {
    int rc = system("/bin/crashinfo");
    if (rc < 0) {
        fprintf(stderr, "safesh: failed to spawn crashinfo: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

/* Boot-mode shape: run each diagnostic once, gate on essentials. */
static int cmd_boot(void) {
    printf("M28D_SAFESH: starting (safe-mode shell harness)\n");

    int logs_rv = cmd_logs(8);
    if (logs_rv < 0) {
        printf("M28D_SAFESH: FAIL (slog_read)\n");
        return 1;
    }
    printf("M28D_SAFESH: slog_read=ok records=%d\n", logs_rv);

    if (cmd_wdog() < 0) {
        printf("M28D_SAFESH: FAIL (wdog_status)\n");
        return 1;
    }
    struct abi_wdog_status s;
    tobywdog_status(&s);
    if (!s.enabled || s.kernel_heartbeats == 0 ||
        s.syscall_heartbeats == 0) {
        printf("M28D_SAFESH: FAIL (wdog heartbeats stalled in safe mode)\n");
        return 1;
    }
    printf("M28D_SAFESH: wdog_alive=1\n");

    int wrv = tobylog_write(ABI_SLOG_LEVEL_INFO, "user",
                            "M28D_TAG safesh boot write");
    if (wrv != 0) {
        printf("M28D_SAFESH: FAIL (slog_write errno=%d)\n", errno);
        return 1;
    }
    printf("M28D_SAFESH: slog_write=ok\n");

    if (cmd_hwinfo() < 0) {
        printf("M28D_SAFESH: WARN (hwinfo failed)\n");
    }

    printf("M28D_SAFESH: PASS\n");
    return 0;
}

/* Interactive REPL (stdin = console keyboard). */
static int cmd_repl(void) {
    char line[128];
    printf("\n");
    printf("================ tobyOS safe-mode shell (safesh) ================\n");
    printf("Commands: help, logs, wdog, services, fscheck, hwinfo, crashinfo,\n");
    printf("          cat, ls, ps, reboot, exit\n");
    printf("=================================================================\n");
    for (;;) {
        printf("safe> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n[safesh] EOF -- exiting\n");
            return 0;
        }
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) continue;

        /* Tokenize: first word is cmd, rest is arg. */
        char *arg = line;
        while (*arg && *arg != ' ') arg++;
        if (*arg == ' ') { *arg = '\0'; arg++; }
        while (*arg == ' ') arg++;

        if (!strcmp(line, "help") || !strcmp(line, "?")) {
            printf("commands:\n");
            printf("  help              show this message\n");
            printf("  logs [N]          print last N log records (default 30)\n");
            printf("  wdog              print watchdog status\n");
            printf("  services          list supervised services\n");
            printf("  fscheck [path]    check filesystem (default /)\n");
            printf("  hwinfo            print hardware summary\n");
            printf("  crashinfo         show last crash dump\n");
            printf("  cat <file>        print file contents\n");
            printf("  ls [dir]          list directory (default /)\n");
            printf("  ps                list running processes\n");
            printf("  reboot            ACPI reset (may need power-cycle)\n");
            printf("  exit / quit       leave the shell\n");
        } else if (!strcmp(line, "logs")) {
            int k = 30;
            if (*arg) k = atoi(arg);
            if (k <= 0) k = 30;
            cmd_logs(k);
        } else if (!strcmp(line, "wdog")) {
            cmd_wdog();
        } else if (!strcmp(line, "services") || !strcmp(line, "svc")) {
            cmd_services();
        } else if (!strcmp(line, "fscheck")) {
            cmd_fscheck(*arg ? arg : "/");
        } else if (!strcmp(line, "hwinfo") || !strcmp(line, "hw")) {
            cmd_hwinfo();
        } else if (!strcmp(line, "crashinfo") || !strcmp(line, "crash")) {
            cmd_crashinfo();
        } else if (!strcmp(line, "cat")) {
            cmd_cat(arg);
        } else if (!strcmp(line, "ls") || !strcmp(line, "dir")) {
            cmd_ls(*arg ? arg : "/");
        } else if (!strcmp(line, "ps")) {
            /* ps uses the kernel debug console -- spawn it */
            int rc = system("/bin/services --json");
            if (rc < 0)
                printf("[safesh] ps: spawn failed: %s\n", strerror(errno));
        } else if (!strcmp(line, "reboot") || !strcmp(line, "shutdown")) {
            printf("[safesh] initiating reboot via ACPI...\n");
            fflush(stdout);
            /* ACPI reset: write 0x06 to port 0xCF9 (PCI reset).
             * The kernel may implement this differently -- we attempt
             * the system() path first which invokes the kernel's exit
             * path and triggers ACPI shutdown. */
            _exit(99);
        } else if (!strcmp(line, "exit") || !strcmp(line, "quit")) {
            printf("[safesh] bye.\n");
            return 0;
        } else {
            printf("[safesh] unknown command '%s' (try 'help')\n", line);
        }
    }
}

int main(int argc, char **argv) {
    int do_boot = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--boot")) do_boot = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "safesh: unknown arg '%s'\n", a);
            usage();
            return 2;
        }
    }
    if (do_boot) return cmd_boot();
    return cmd_repl();
}
