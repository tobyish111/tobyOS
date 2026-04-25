/* programs/stabilitytest/main.c -- Milestone 28G: full-system
 * stability validator.
 *
 * The tool runs the kernel's SYS_STAB_SELFTEST (which probes every
 * subsystem touched by M28A-F: log, panic, watchdog, fs, services,
 * gui, terminal, network, input, safe-mode, display) and then layers
 * userland-side stress on top so we exercise the syscall path and
 * the heap allocator at the same time.
 *
 * Modes:
 *   stabilitytest                  -- run all probes, render a table
 *   stabilitytest --quick          -- skip stress, no extra workload
 *   stabilitytest --stress         -- add disk + heap + spawn stress
 *   stabilitytest --boot           -- emit `M28G_STAB:` sentinels for
 *                                     the test harness; exit 0 on PASS
 *   stabilitytest --json           -- one line of JSON, then exit
 *
 * Exit codes:
 *   0    every requested probe passed (and optional stress was clean)
 *   1    one or more probes failed
 *   2    bad usage
 *   3    syscall failure (errno preserved) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <tobyos_stab.h>

static void usage(void) {
    fprintf(stderr,
            "usage: stabilitytest [--quick|--stress|--boot|--json]\n"
            "  default          full self-test, human-readable\n"
            "  --quick          skip stress workload, faster output\n"
            "  --stress         add disk/heap/spawn stress on top\n"
            "  --boot           emit machine-grepable sentinels\n"
            "  --json           one line of JSON + exit\n");
}

/* Pretty-print the kernel's self-test report. */
static void render_report(const struct abi_stab_report *r) {
    char want[64], got[64];
    tobystab_format_mask(want, sizeof(want), r->expected_mask);
    tobystab_format_mask(got,  sizeof(got),  r->result_mask);
    printf("stabilitytest report:\n");
    printf("  boot_ms       : %lu\n", (unsigned long)r->boot_ms);
    printf("  safe_mode     : %s\n",  r->safe_mode ? "yes" : "no");
    printf("  expected mask : 0x%04x  (%s)\n",
           (unsigned)r->expected_mask, want);
    printf("  result   mask : 0x%04x  (%s)\n",
           (unsigned)r->result_mask, got);
    printf("  probes pass   : %u\n", (unsigned)r->pass_count);
    printf("  probes fail   : %u\n", (unsigned)r->fail_count);
    printf("  detail        : %s\n",
           r->detail[0] ? r->detail : "(empty)");

    /* Per-bit verdict. */
    static const uint32_t bits[] = {
        ABI_STAB_OK_BOOT, ABI_STAB_OK_LOG, ABI_STAB_OK_PANIC,
        ABI_STAB_OK_WATCHDOG, ABI_STAB_OK_FILESYSTEM,
        ABI_STAB_OK_SERVICES, ABI_STAB_OK_GUI, ABI_STAB_OK_TERMINAL,
        ABI_STAB_OK_NETWORK, ABI_STAB_OK_INPUT,
        ABI_STAB_OK_SAFE_MODE, ABI_STAB_OK_DISPLAY,
    };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        uint32_t b = bits[i];
        if ((r->expected_mask & b) == 0) continue;
        const char *verdict = (r->result_mask & b) ? "PASS" : "FAIL";
        printf("  %-12s : %s\n", tobystab_bit_name(b), verdict);
    }
}

static void render_json(const struct abi_stab_report *r) {
    printf("{\"expected_mask\":%u,\"result_mask\":%u,\"pass\":%u,"
           "\"fail\":%u,\"safe_mode\":%lu,\"boot_ms\":%lu,"
           "\"detail\":\"%s\"}\n",
           (unsigned)r->expected_mask,
           (unsigned)r->result_mask,
           (unsigned)r->pass_count,
           (unsigned)r->fail_count,
           (unsigned long)r->safe_mode,
           (unsigned long)r->boot_ms,
           r->detail);
}

/* Stress workload. Touches the heap, the syscall path (open/read/
 * close), and the standard streams. Designed to be cheap so it can
 * run on every boot. Returns 0 on PASS, 1 on FAIL. */
static int run_stress(void) {
    int fails = 0;

    /* Heap stress: 64x4KiB allocations + 64 frees, then a single
     * larger allocation to exercise free-list coalescing. */
    void *blocks[64];
    for (int i = 0; i < 64; i++) {
        blocks[i] = malloc(4096);
        if (!blocks[i]) {
            fprintf(stderr, "stress: malloc(4096) failed at i=%d\n", i);
            fails++;
            for (int j = 0; j < i; j++) free(blocks[j]);
            goto skip_heap;
        }
        memset(blocks[i], (unsigned char)(i & 0xff), 4096);
    }
    for (int i = 0; i < 64; i++) free(blocks[i]);
    void *big = malloc(64 * 1024);
    if (!big) {
        fprintf(stderr, "stress: malloc(64K) failed\n");
        fails++;
    } else {
        memset(big, 0xa5, 64 * 1024);
        free(big);
    }
skip_heap:;

    /* Disk I/O stress: read /etc/safemode_now or /etc/services_motd
     * if available, otherwise just /init. We don't fail on missing
     * files (initrd contents vary) -- only on a kernel error. */
    static const char *probes[] = {
        "/init", "/etc/motd", "/etc/banner", NULL,
    };
    int read_any = 0;
    for (int i = 0; probes[i]; i++) {
        int fd = open(probes[i], O_RDONLY);
        if (fd < 0) continue;
        char buf[128];
        ssize_t n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n < 0) {
            fprintf(stderr, "stress: read(%s) failed: %s\n",
                    probes[i], strerror(errno));
            fails++;
        } else {
            read_any = 1;
        }
    }
    if (!read_any) {
        fprintf(stderr, "stress: no readable file in initrd "
                "(non-fatal, environment-dependent)\n");
    }

    /* Syscall stress: 256 cheap syscalls (getpid is the cheapest
     * round-trip). We just want to confirm the SYSCALL/SYSRET path
     * survives a burst. */
    for (int i = 0; i < 256; i++) {
        if (getpid() <= 0) {
            fprintf(stderr, "stress: getpid() returned bad value\n");
            fails++;
            break;
        }
    }

    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    int quick = 0, stress = 0, boot = 0, json = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--quick"))  quick = 1;
        else if (!strcmp(a, "--stress")) stress = 1;
        else if (!strcmp(a, "--boot"))   boot = 1;
        else if (!strcmp(a, "--json"))   json = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "stabilitytest: unexpected argument '%s'\n", a);
            usage();
            return 2;
        }
    }

    struct abi_stab_report rpt;
    memset(&rpt, 0, sizeof(rpt));
    int fails = tobystab_run(&rpt, ABI_STAB_OK_ALL);
    if (fails < 0) {
        fprintf(stderr, "stabilitytest: SYS_STAB_SELFTEST failed: %s\n",
                strerror(errno));
        if (boot) printf("M28G_STAB: FAIL syscall errno=%d\n", errno);
        return 3;
    }

    /* Optional userland stress on top of the kernel verdict. */
    int stress_fails = 0;
    if (stress && !quick) {
        stress_fails = run_stress();
    }

    if (json) {
        render_json(&rpt);
        return (fails == 0 && stress_fails == 0) ? 0 : 1;
    }

    if (boot) {
        /* One sentinel per probe + a final summary line. The
         * test_m28_final.ps1 aggregator parses these. */
        static const uint32_t bits[] = {
            ABI_STAB_OK_BOOT, ABI_STAB_OK_LOG, ABI_STAB_OK_PANIC,
            ABI_STAB_OK_WATCHDOG, ABI_STAB_OK_FILESYSTEM,
            ABI_STAB_OK_SERVICES, ABI_STAB_OK_GUI,
            ABI_STAB_OK_TERMINAL, ABI_STAB_OK_NETWORK,
            ABI_STAB_OK_INPUT, ABI_STAB_OK_SAFE_MODE,
            ABI_STAB_OK_DISPLAY,
        };
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
            uint32_t b = bits[i];
            const char *v = (rpt.result_mask & b) ? "PASS" : "FAIL";
            printf("M28G_STAB: probe=%s verdict=%s\n",
                   tobystab_bit_name(b), v);
        }
        printf("M28G_STAB: detail=%s\n",
               rpt.detail[0] ? rpt.detail : "(empty)");
        printf("M28G_STAB: stress=%s safe_mode=%lu boot_ms=%lu\n",
               (stress ? (stress_fails ? "FAIL" : "PASS")
                       : "skipped"),
               (unsigned long)rpt.safe_mode,
               (unsigned long)rpt.boot_ms);
        if (fails == 0 && stress_fails == 0) {
            printf("M28G_STAB: PASS pass=%u fail=%u\n",
                   (unsigned)rpt.pass_count,
                   (unsigned)rpt.fail_count);
            return 0;
        }
        printf("M28G_STAB: FAIL pass=%u fail=%u stress_fails=%d\n",
               (unsigned)rpt.pass_count,
               (unsigned)rpt.fail_count, stress_fails);
        return 1;
    }

    render_report(&rpt);
    if (stress && !quick) {
        printf("  stress       : %s\n",
               stress_fails ? "FAIL" : "PASS");
    }
    if (fails == 0 && stress_fails == 0) {
        printf("stabilitytest: OVERALL PASS\n");
        return 0;
    }
    printf("stabilitytest: OVERALL FAIL (probe_fails=%d stress_fails=%d)\n",
           fails, stress_fails);
    return 1;
}
