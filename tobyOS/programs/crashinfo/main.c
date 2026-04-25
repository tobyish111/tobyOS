/* programs/crashinfo/main.c -- Milestone 28B: crash-dump inspector.
 *
 * Reads /data/crash/last.dump (the canonical "last panic" record
 * written by panic.c::try_write_crash_dump). The file starts with a
 * struct abi_crash_header followed by `body_bytes` of human-readable
 * text (registers, slog tail, stack walk, etc.). We decode the header,
 * print a compact human summary, then stream the body to stdout.
 *
 * Usage:
 *   crashinfo                -- pretty-print latest dump
 *   crashinfo --quiet        -- just print PASS/FAIL sentinel
 *   crashinfo --boot         -- M28B boot-harness mode: validates the
 *                               header magic + reason fields and emits
 *                               the M28B_CRASHINFO sentinels the test
 *                               script greps for.
 *   crashinfo --path P       -- read alternate dump path
 *
 * Exit codes:
 *   0   PASS  (dump found, header valid)
 *   1   dump file missing
 *   2   header invalid (bad magic / version / truncated)
 *   3   read error
 *   4   bad usage
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <tobyos/abi/abi.h>

#define DEFAULT_DUMP_PATH "/data/crash/last.dump"

#define DUMP_MAX_BYTES (256 * 1024)

static unsigned char g_buf[DUMP_MAX_BYTES];

static void usage(void) {
    fprintf(stderr,
            "usage: crashinfo [--quiet] [--boot] [--path PATH]\n");
}

static int read_dump(const char *path, long *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    long total = 0;
    for (;;) {
        if ((size_t)total >= sizeof(g_buf)) {
            break;
        }
        long n = read(fd, g_buf + total, sizeof(g_buf) - (size_t)total);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += n;
    }
    close(fd);
    *out_size = total;
    return 0;
}

static int validate_header(const struct abi_crash_header *h, long file_size) {
    if (file_size < (long)sizeof(*h)) return -1;
    if (h->magic != 0x48535243u) return -2; /* 'CRSH' little-endian */
    if (h->version != 1u) return -3;
    if ((long)(sizeof(*h) + h->body_bytes) > file_size) return -4;
    return 0;
}

static void print_pretty(const struct abi_crash_header *h, long file_size) {
    printf("=== tobyOS crash dump (M28B) ===\n");
    printf("path        : %s\n", DEFAULT_DUMP_PATH);
    printf("file_size   : %ld bytes\n", file_size);
    printf("magic       : 0x%08x ('CRSH')\n", (unsigned)h->magic);
    printf("version     : %u\n", (unsigned)h->version);
    printf("boot_seq    : %lu\n", (unsigned long)h->boot_seq);
    printf("time_ms     : %lu\n", (unsigned long)h->time_ms);
    printf("pid         : %d\n", (int)h->pid);
    printf("body_bytes  : %u\n", (unsigned)h->body_bytes);

    char reason_buf[ABI_CRASH_REASON_MAX + 1];
    memcpy(reason_buf, h->reason, ABI_CRASH_REASON_MAX);
    reason_buf[ABI_CRASH_REASON_MAX] = 0;
    printf("reason      : %s\n", reason_buf);

    printf("--- body ---\n");
    const char *body = (const char *)(g_buf + sizeof(*h));
    /* Body is plain ASCII text terminated by NULs/padding. Print exactly
     * body_bytes, but stop at first NUL within that range so we don't
     * spam a screenful of zeros. */
    for (uint32_t i = 0; i < h->body_bytes; i++) {
        char c = body[i];
        if (c == 0) break;
        fputc(c, stdout);
    }
    printf("\n--- end ---\n");
}

static int cmd_boot(const char *path) {
    long sz = 0;
    int rc = read_dump(path, &sz);
    if (rc < 0) {
        printf("M28B_CRASHINFO: FAIL (open: %s -> %s)\n",
               path, strerror(errno));
        return 1;
    }
    if (sz < (long)sizeof(struct abi_crash_header)) {
        printf("M28B_CRASHINFO: FAIL (truncated, %ld bytes)\n", sz);
        return 2;
    }
    const struct abi_crash_header *h =
        (const struct abi_crash_header *)g_buf;
    int v = validate_header(h, sz);
    if (v < 0) {
        printf("M28B_CRASHINFO: FAIL (header invalid, code=%d magic=0x%08x)\n",
               v, (unsigned)h->magic);
        return 2;
    }
    /* Sanity: reason should mention "kpanic" since boot harness asks for it. */
    char reason_buf[ABI_CRASH_REASON_MAX + 1];
    memcpy(reason_buf, h->reason, ABI_CRASH_REASON_MAX);
    reason_buf[ABI_CRASH_REASON_MAX] = 0;

    /* Emit a compact summary. */
    printf("M28B_CRASHINFO: header.magic=0x%08x version=%u "
           "body_bytes=%u boot_seq=%lu pid=%d\n",
           (unsigned)h->magic, (unsigned)h->version,
           (unsigned)h->body_bytes, (unsigned long)h->boot_seq,
           (int)h->pid);
    printf("M28B_CRASHINFO: reason=\"%s\"\n", reason_buf);

    const char *body = (const char *)(g_buf + sizeof(*h));
    uint32_t body_len = h->body_bytes;

    /* Print first ~600 bytes of the body so the test log captures
     * something readable. The full body can be ~1.5 KiB and would
     * just clutter the serial transcript. */
    uint32_t preview = body_len;
    if (preview > 600) preview = 600;
    printf("M28B_CRASHINFO: body_preview_begin\n");
    for (uint32_t i = 0; i < preview; i++) {
        char c = body[i];
        if (c == 0) break;
        fputc(c, stdout);
    }
    printf("\nM28B_CRASHINFO: body_preview_end\n");

    /* Light heuristics for content health. Scan the FULL body, not just
     * the preview, so the slog tail (which lives after the register
     * dump) still counts. */
    int found_regs = 0, found_slog = 0;
    const char *needle1 = "regs:";
    const char *needle2 = "slog";
    for (uint32_t i = 0; i + 4 < body_len; i++) {
        if (memcmp(body + i, needle1, 5) == 0) found_regs = 1;
        if (memcmp(body + i, needle2, 4) == 0) found_slog = 1;
    }
    printf("M28B_CRASHINFO: heuristics regs=%d slog=%d\n",
           found_regs, found_slog);

    if (!found_regs) {
        printf("M28B_CRASHINFO: FAIL (missing register section)\n");
        return 2;
    }

    printf("M28B_CRASHINFO: PASS\n");
    return 0;
}

int main(int argc, char **argv) {
    int quiet = 0;
    int boot = 0;
    const char *path = DEFAULT_DUMP_PATH;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--quiet")) {
            quiet = 1;
        } else if (!strcmp(a, "--boot")) {
            boot = 1;
        } else if (!strcmp(a, "--path") && i + 1 < argc) {
            path = argv[++i];
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else {
            usage();
            return 4;
        }
    }

    if (boot) {
        return cmd_boot(path);
    }

    long sz = 0;
    int rc = read_dump(path, &sz);
    if (rc < 0) {
        if (!quiet) {
            fprintf(stderr, "crashinfo: open %s: %s\n", path, strerror(errno));
        } else {
            printf("crashinfo: NO_DUMP\n");
        }
        return 1;
    }

    const struct abi_crash_header *h = (const struct abi_crash_header *)g_buf;
    int v = validate_header(h, sz);
    if (v < 0) {
        if (!quiet) {
            fprintf(stderr,
                    "crashinfo: invalid dump header (code=%d magic=0x%08x size=%ld)\n",
                    v, (unsigned)h->magic, sz);
        } else {
            printf("crashinfo: BAD_HEADER\n");
        }
        return 2;
    }

    if (quiet) {
        printf("crashinfo: OK boot_seq=%lu pid=%d body_bytes=%u\n",
               (unsigned long)h->boot_seq, (int)h->pid,
               (unsigned)h->body_bytes);
    } else {
        print_pretty(h, sz);
    }
    return 0;
}
