/* programs/fscheck/main.c -- Milestone 28E: filesystem-integrity tool.
 *
 * Probes a kernel-side mount via SYS_FS_CHECK and renders a verdict.
 * The kernel does the actual structural validation; we just ask it
 * politely and pretty-print the result.
 *
 * Usage:
 *   fscheck                  -- probe /data (default), human-readable
 *   fscheck /data            -- probe explicit mount point
 *   fscheck --json /data     -- single-line JSON for CI scrapers
 *   fscheck --boot           -- harness mode used by test_m28e.ps1.
 *                               Probes /data and emits the
 *                               "M28E_FSCHECK: PASS|WARN|FAIL"
 *                               sentinel.
 *
 * Exit codes:
 *   0  PASS (status == OK)
 *   1  syscall failed
 *   2  bad usage
 *   3  CORRUPT (kernel says the FS is broken)
 *   4  WARN    (kernel applied a minor repair / saw warnings)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <tobyos_fscheck.h>

static void usage(void) {
    fprintf(stderr,
            "usage: fscheck [--json] [--boot] [<mountpoint>]\n"
            "       (default mountpoint: /data)\n");
}

static void print_pretty(const struct abi_fscheck_report *r) {
    char detail[ABI_FSCHECK_DETAIL_MAX + 1];
    memcpy(detail, r->detail, ABI_FSCHECK_DETAIL_MAX);
    detail[ABI_FSCHECK_DETAIL_MAX] = '\0';

    char fs_type[17];
    memcpy(fs_type, r->fs_type, 16);
    fs_type[16] = '\0';

    char path[ABI_FSCHECK_PATH_MAX + 1];
    memcpy(path, r->path, ABI_FSCHECK_PATH_MAX);
    path[ABI_FSCHECK_PATH_MAX] = '\0';

    printf("fscheck report:\n");
    printf("  path             = %s\n", path[0] ? path : "(unknown)");
    printf("  fs_type          = %s\n", fs_type[0] ? fs_type : "(unknown)");
    printf("  status           = 0x%02x (%s)\n",
           (unsigned)r->status, tobyfscheck_status_str(r->status));
    printf("    OK             = %u\n",
           (r->status & ABI_FSCHECK_OK)        ? 1u : 0u);
    printf("    REPAIRED       = %u\n",
           (r->status & ABI_FSCHECK_REPAIRED)  ? 1u : 0u);
    printf("    CORRUPT        = %u\n",
           (r->status & ABI_FSCHECK_CORRUPT)   ? 1u : 0u);
    printf("    UNMOUNTED      = %u\n",
           (r->status & ABI_FSCHECK_UNMOUNTED) ? 1u : 0u);
    printf("  errors_found     = %u\n", (unsigned)r->errors_found);
    printf("  errors_repaired  = %u\n", (unsigned)r->errors_repaired);
    printf("  total_bytes      = %lu\n", (unsigned long)r->total_bytes);
    printf("  free_bytes       = %lu\n", (unsigned long)r->free_bytes);
    printf("  detail           = \"%s\"\n", detail);
}

static void print_json(const struct abi_fscheck_report *r) {
    char detail[ABI_FSCHECK_DETAIL_MAX + 1];
    memcpy(detail, r->detail, ABI_FSCHECK_DETAIL_MAX);
    detail[ABI_FSCHECK_DETAIL_MAX] = '\0';
    for (char *p = detail; *p; p++) if (*p == '"') *p = '\'';

    char fs_type[17];
    memcpy(fs_type, r->fs_type, 16);
    fs_type[16] = '\0';

    char path[ABI_FSCHECK_PATH_MAX + 1];
    memcpy(path, r->path, ABI_FSCHECK_PATH_MAX);
    path[ABI_FSCHECK_PATH_MAX] = '\0';

    printf("{\"path\":\"%s\",\"fs_type\":\"%s\",\"status\":%u,"
           "\"status_str\":\"%s\","
           "\"errors_found\":%u,\"errors_repaired\":%u,"
           "\"total_bytes\":%lu,\"free_bytes\":%lu,"
           "\"detail\":\"%s\"}\n",
           path, fs_type, (unsigned)r->status,
           tobyfscheck_status_str(r->status),
           (unsigned)r->errors_found, (unsigned)r->errors_repaired,
           (unsigned long)r->total_bytes, (unsigned long)r->free_bytes,
           detail);
}

static int verdict_exit_code(const struct abi_fscheck_report *r) {
    if (r->status & ABI_FSCHECK_UNMOUNTED) return 1;
    if (r->status & ABI_FSCHECK_CORRUPT)   return 3;
    if (r->status & ABI_FSCHECK_REPAIRED)  return 4;
    if (r->status & ABI_FSCHECK_OK)        return 0;
    return 1;
}

static int cmd_boot(const char *path) {
    struct abi_fscheck_report r;
    memset(&r, 0, sizeof(r));
    int rv = tobyfscheck(path, &r);
    if (rv < 0 && (r.status == 0)) {
        printf("M28E_FSCHECK: FAIL syscall errno=%d path=%s\n",
               errno, path);
        return 1;
    }

    /* Always emit the same sentinel block so the harness can grep
     * unconditionally regardless of verdict. */
    char detail[ABI_FSCHECK_DETAIL_MAX + 1];
    memcpy(detail, r.detail, ABI_FSCHECK_DETAIL_MAX);
    detail[ABI_FSCHECK_DETAIL_MAX] = '\0';
    char fs_type[17];
    memcpy(fs_type, r.fs_type, 16);
    fs_type[16] = '\0';

    printf("M28E_FSCHECK: path=%s fs_type=%s status=%s "
           "errors_found=%u errors_repaired=%u total_bytes=%lu "
           "free_bytes=%lu\n",
           path, fs_type[0] ? fs_type : "?",
           tobyfscheck_status_str(r.status),
           (unsigned)r.errors_found, (unsigned)r.errors_repaired,
           (unsigned long)r.total_bytes, (unsigned long)r.free_bytes);
    printf("M28E_FSCHECK: detail=\"%s\"\n", detail);

    if (r.status & ABI_FSCHECK_OK) {
        if (r.status & ABI_FSCHECK_REPAIRED) {
            printf("M28E_FSCHECK: WARN\n");
            return 4;
        }
        printf("M28E_FSCHECK: PASS\n");
        return 0;
    }
    if (r.status & ABI_FSCHECK_CORRUPT) {
        printf("M28E_FSCHECK: FAIL (corrupt)\n");
        return 3;
    }
    if (r.status & ABI_FSCHECK_UNMOUNTED) {
        printf("M28E_FSCHECK: FAIL (unmounted)\n");
        return 1;
    }
    printf("M28E_FSCHECK: FAIL (status=0x%x)\n", (unsigned)r.status);
    return 1;
}

int main(int argc, char **argv) {
    int json = 0;
    int do_boot = 0;
    const char *path = "/data";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--json"))           json = 1;
        else if (!strcmp(a, "--boot"))      do_boot = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "fscheck: unknown flag '%s'\n", a);
            usage();
            return 2;
        } else {
            path = a;
        }
    }

    if (do_boot) return cmd_boot(path);

    struct abi_fscheck_report r;
    memset(&r, 0, sizeof(r));
    int rv = tobyfscheck(path, &r);
    if (rv < 0 && (r.status == 0)) {
        fprintf(stderr, "fscheck: syscall failed: %s\n", strerror(errno));
        return 1;
    }
    if (json) print_json(&r);
    else      print_pretty(&r);
    return verdict_exit_code(&r);
}
