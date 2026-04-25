/* programs/logview/main.c -- Milestone 28A: structured-log viewer.
 *
 * Reads the kernel slog ring through the SYS_SLOG_READ syscall and
 * renders it as a table. Optionally tails a number of records, filters
 * by minimum level, or dumps the on-disk persisted log instead.
 *
 * Usage:
 *   logview                  -- render whole ring (table form)
 *   logview --json           -- machine-readable, one record per line
 *   logview --level WARN     -- only level <= WARN (i.e. WARN+ERROR)
 *   logview --tail N         -- last N records only
 *   logview --persist        -- print the persisted /data/system.log
 *   logview --stats          -- counters only (no records)
 *   logview --boot           -- the boot harness shape: prints a stable
 *                               PASS sentinel + a fixed table so the
 *                               M28A test_m28a.ps1 script can grep
 *
 * Exit codes (M28A convention):
 *   0  PASS  (records read OR --persist found a file OR --stats ok)
 *   1  syscall failed
 *   2  bad usage
 *   3  --persist requested but /data/system.log absent
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <tobyos_slog.h>

#ifndef SLOG_PERSIST_PATH
#define SLOG_PERSIST_PATH  "/data/system.log"
#endif

static void usage(void) {
    fprintf(stderr,
            "usage: logview [--json] [--level LEVEL] [--tail N]\n"
            "               [--persist] [--stats] [--boot]\n");
}

/* Drain everything currently in the ring into an arena. We allocate
 * the whole ABI_SLOG_RING_DEPTH worth on the BSS so we don't depend
 * on a working malloc for the boot harness. */
static struct abi_slog_record g_buf[ABI_SLOG_RING_DEPTH];

static int read_ring(void) {
    int n = tobylog_read(g_buf, ABI_SLOG_RING_DEPTH, 0);
    if (n < 0) {
        fprintf(stderr, "logview: slog_read failed: %s\n", strerror(errno));
        return -1;
    }
    return n;
}

static void print_table(int n, unsigned int max_level, int tail) {
    int start = 0;
    if (tail > 0 && n > tail) start = n - tail;
    tobylog_print_header(stdout);
    int shown = 0;
    for (int i = start; i < n; i++) {
        if (g_buf[i].level > max_level) continue;
        tobylog_print_record(stdout, &g_buf[i]);
        shown++;
    }
    /* Trailing sanity counter so the script can grep + a human can
     * see at a glance how many lines they're looking at. */
    printf("[logview] shown=%d total=%d max_level=%s tail=%d\n",
           shown, n, tobylog_level_str(max_level), tail);
}

static void print_json(int n, unsigned int max_level, int tail) {
    int start = 0;
    if (tail > 0 && n > tail) start = n - tail;
    int shown = 0;
    for (int i = start; i < n; i++) {
        const struct abi_slog_record *r = &g_buf[i];
        if (r->level > max_level) continue;
        printf("{\"seq\":%lu,\"time_ms\":%lu,\"level\":\"%s\","
               "\"sub\":\"%s\",\"pid\":%d,\"msg\":\"",
               (unsigned long)r->seq,
               (unsigned long)r->time_ms,
               tobylog_level_str(r->level),
               r->sub[0] ? r->sub : "kernel",
               (int)r->pid);
        /* Manual escape pass: ", \\, control bytes. */
        for (const char *p = r->msg; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { printf("\\%c", c); }
            else if (c < 0x20)         { printf("\\u%04x", c); }
            else                       { printf("%c", c); }
        }
        printf("\"}\n");
        shown++;
    }
    printf("[logview] shown=%d total=%d (json)\n", shown, n);
}

static int cmd_persist(void) {
    FILE *f = fopen(SLOG_PERSIST_PATH, "r");
    if (!f) {
        fprintf(stderr, "logview: cannot open %s: %s\n",
                SLOG_PERSIST_PATH, strerror(errno));
        return 3;
    }
    char line[256];
    int lines = 0;
    while (fgets(line, sizeof(line), f)) {
        printf("%s", line);
        lines++;
    }
    fclose(f);
    printf("[logview] persist=%s lines=%d\n", SLOG_PERSIST_PATH, lines);
    return 0;
}

static int cmd_stats(void) {
    struct abi_slog_stats st;
    if (tobylog_stats(&st) < 0) {
        fprintf(stderr, "logview: slog_stats failed: %s\n", strerror(errno));
        return 1;
    }
    printf("slog stats:\n");
    printf("  total_emitted    = %lu\n",  (unsigned long)st.total_emitted);
    printf("  total_dropped    = %lu\n",  (unsigned long)st.total_dropped);
    printf("  ring_in_use/depth= %u/%u\n",
           (unsigned)st.ring_in_use, (unsigned)st.ring_depth);
    printf("  per_level: ERROR=%lu WARN=%lu INFO=%lu DEBUG=%lu\n",
           (unsigned long)st.per_level[ABI_SLOG_LEVEL_ERROR],
           (unsigned long)st.per_level[ABI_SLOG_LEVEL_WARN],
           (unsigned long)st.per_level[ABI_SLOG_LEVEL_INFO],
           (unsigned long)st.per_level[ABI_SLOG_LEVEL_DEBUG]);
    printf("  persist: bytes=%lu flushes=%lu failures=%lu\n",
           (unsigned long)st.persist_bytes,
           (unsigned long)st.persist_flushes,
           (unsigned long)st.persist_failures);
    return 0;
}

/* The boot harness shape: a fixed sequence of grep-able lines so
 * test_m28a.ps1 can confirm the syscall path AND ring contents. */
static int cmd_boot(void) {
    int n = read_ring();
    if (n < 0) {
        printf("M28A_LOGVIEW: FAIL ring_read errno=%d\n", errno);
        return 1;
    }
    /* Look for the M28A_TAG markers the kernel harness emitted. */
    int tags = 0;
    int kernel_tag = 0, fs_tag = 0, net_tag = 0, gui_tag = 0;
    for (int i = 0; i < n; i++) {
        if (strstr(g_buf[i].msg, "M28A_TAG") != NULL) {
            tags++;
            if (!strcmp(g_buf[i].sub, "kernel")) kernel_tag = 1;
            if (!strcmp(g_buf[i].sub, "fs"))     fs_tag     = 1;
            if (!strcmp(g_buf[i].sub, "net"))    net_tag    = 1;
            if (!strcmp(g_buf[i].sub, "gui"))    gui_tag    = 1;
        }
    }
    /* Stats sanity. */
    struct abi_slog_stats st;
    int srv = tobylog_stats(&st);
    if (srv < 0) {
        printf("M28A_LOGVIEW: FAIL stats errno=%d\n", errno);
        return 1;
    }
    printf("M28A_LOGVIEW: ring=%d tags=%d kernel=%d fs=%d net=%d gui=%d "
           "emitted=%lu dropped=%lu in_use=%u depth=%u\n",
           n, tags, kernel_tag, fs_tag, net_tag, gui_tag,
           (unsigned long)st.total_emitted,
           (unsigned long)st.total_dropped,
           (unsigned)st.ring_in_use,
           (unsigned)st.ring_depth);
    /* Required: at least one tag from each of the five subsystems we
     * emit from in the kernel-side harness. tags >= 4 (drop debug if
     * console threshold gates it). Total ring must be non-empty. */
    int ok = (n > 0) && (tags >= 4) &&
             kernel_tag && fs_tag && net_tag && gui_tag;
    if (!ok) {
        printf("M28A_LOGVIEW: FAIL summary tag_check\n");
        return 1;
    }
    /* Render a small table so the human-readable sentinel exists. */
    tobylog_print_header(stdout);
    int show = n < 10 ? n : 10;
    for (int i = n - show; i < n; i++) tobylog_print_record(stdout, &g_buf[i]);
    /* Try the userland write path too -- post a record and confirm
     * the kernel accepts it. */
    int wrv = tobylog_write(ABI_SLOG_LEVEL_INFO, "user",
                            "M28A_TAG logview boot write");
    if (wrv != 0) {
        printf("M28A_LOGVIEW: FAIL slog_write errno=%d\n", errno);
        return 1;
    }
    printf("M28A_LOGVIEW: PASS\n");
    return 0;
}

int main(int argc, char **argv) {
    int           json     = 0;
    int           tail     = 0;
    int           do_stats = 0;
    int           do_persist = 0;
    int           do_boot  = 0;
    unsigned int  max_level = ABI_SLOG_LEVEL_DEBUG;  /* show everything */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--json"))         json = 1;
        else if (!strcmp(a, "--stats"))   do_stats = 1;
        else if (!strcmp(a, "--persist")) do_persist = 1;
        else if (!strcmp(a, "--boot"))    do_boot = 1;
        else if (!strcmp(a, "--level") && i + 1 < argc) {
            unsigned int v = tobylog_level_from_str(argv[++i]);
            if (v >= ABI_SLOG_LEVEL_MAX) {
                fprintf(stderr, "logview: bad level '%s'\n", argv[i]);
                return 2;
            }
            max_level = v;
        } else if (!strcmp(a, "--tail") && i + 1 < argc) {
            tail = atoi(argv[++i]);
            if (tail < 0) tail = 0;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "logview: unknown arg '%s'\n", a);
            usage();
            return 2;
        }
    }

    if (do_boot)    return cmd_boot();
    if (do_persist) return cmd_persist();
    if (do_stats)   return cmd_stats();

    int n = read_ring();
    if (n < 0) return 1;
    if (json) print_json(n, max_level, tail);
    else      print_table(n, max_level, tail);
    return 0;
}
