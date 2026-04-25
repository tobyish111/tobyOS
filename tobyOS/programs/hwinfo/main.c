/* programs/hwinfo/main.c -- Milestone 29A: hardware-inventory tool.
 *
 * Calls SYS_HWINFO via the libtoby wrapper, prints the canonical
 * snapshot text, and (optionally) reads back the persistent
 * /data/hwinfo.snap to confirm the in-memory and on-disk views
 * agree.
 *
 * Usage:
 *   hwinfo                  pretty inventory (default)
 *   hwinfo --json           one-line machine-readable summary
 *   hwinfo --snapshot       same text the kernel writes to disk
 *   hwinfo --persist        read back /data/hwinfo.snap
 *   hwinfo --boot           boot-harness sentinel mode (M29A_HW: ...)
 *
 * Exit codes:
 *   0    snapshot fetched successfully
 *   1    SYS_HWINFO returned an error
 *   2    bad usage
 *   3    file-system layer failed in --persist mode */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <tobyos_hwinfo.h>

#define HWINFO_SNAP_PATH "/data/hwinfo.snap"

static void usage(void) {
    fprintf(stderr,
            "usage: hwinfo [--json|--snapshot|--persist|--boot]\n"
            "  default     pretty hardware inventory\n"
            "  --json      one-line machine-readable summary\n"
            "  --snapshot  same text as /data/hwinfo.snap\n"
            "  --persist   read back the persisted snapshot\n"
            "  --boot      M29A boot-harness mode (sentinels)\n");
}

/* Compact one-line JSON. Callers (M29F regression guard, M29G
 * bringuptest) consume this for stable diffs. */
static void render_json(const struct abi_hwinfo_summary *s) {
    printf("{\"abi\":%u,\"epoch\":%lu,\"uptime_ms\":%lu,"
           "\"safe_mode\":%u,\"profile\":\"%s\","
           "\"cpu\":{\"vendor\":\"%s\",\"brand\":\"%s\","
           "\"family\":%u,\"model\":%u,\"step\":%u,\"count\":%u,"
           "\"feat\":%u},"
           "\"mem\":{\"total_pg\":%lu,\"used_pg\":%lu,\"free_pg\":%lu},"
           "\"bus\":{\"pci\":%u,\"usb\":%u,\"blk\":%u,\"input\":%u,"
           "\"audio\":%u,\"battery\":%u,\"hub\":%u,\"display\":%u}}\n",
           (unsigned)s->kernel_abi_ver,
           (unsigned long)s->snapshot_epoch,
           (unsigned long)s->boot_uptime_ms,
           (unsigned)s->safe_mode,
           s->profile_hint,
           s->cpu_vendor, s->cpu_brand,
           (unsigned)s->cpu_family, (unsigned)s->cpu_model,
           (unsigned)s->cpu_stepping, (unsigned)s->cpu_count,
           (unsigned)s->cpu_features,
           (unsigned long)s->mem_total_pages,
           (unsigned long)s->mem_used_pages,
           (unsigned long)s->mem_free_pages,
           (unsigned)s->pci_count, (unsigned)s->usb_count,
           (unsigned)s->blk_count, (unsigned)s->input_count,
           (unsigned)s->audio_count, (unsigned)s->battery_count,
           (unsigned)s->hub_count, (unsigned)s->display_count);
}

/* Cat the persistent snapshot file. Returns 0 on success, non-zero
 * on read failure. The file is small (<4 KiB) so we use a fixed
 * stack buffer to keep the heap pressure to zero. */
static int dump_persisted(void) {
    int fd = open(HWINFO_SNAP_PATH, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr,
                "FAIL: hwinfo: open(%s) failed: errno=%d\n",
                HWINFO_SNAP_PATH, errno);
        return 3;
    }
    char buf[4096];
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total);
        if (n <= 0) break;
        total += n;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    close(fd);
    buf[total] = '\0';
    if (total <= 0) {
        fprintf(stderr,
                "FAIL: hwinfo: %s is empty\n", HWINFO_SNAP_PATH);
        return 3;
    }
    fputs(buf, stdout);
    return 0;
}

int main(int argc, char **argv) {
    int do_json     = 0;
    int do_snap     = 0;
    int do_persist  = 0;
    int do_boot     = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--json"))     do_json    = 1;
        else if (!strcmp(argv[i], "--snapshot")) do_snap    = 1;
        else if (!strcmp(argv[i], "--persist"))  do_persist = 1;
        else if (!strcmp(argv[i], "--boot"))     do_boot    = 1;
        else if (!strcmp(argv[i], "--help") ||
                 !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "FAIL: hwinfo: unknown arg '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }

    if (do_persist) {
        return dump_persisted();
    }

    struct abi_hwinfo_summary s;
    if (tobyhw_summary(&s) != 0) {
        fprintf(stderr,
                "FAIL: hwinfo: tobyhw_summary: errno=%d\n", errno);
        return 1;
    }

    if (do_json) {
        render_json(&s);
        return 0;
    }
    if (do_snap) {
        tobyhw_print_summary(stdout, &s);
        return 0;
    }
    if (do_boot) {
        /* Sentinel format consumed by test_m29a.ps1. The script
         * greps for ^M29A_HW: lines and parses each key=value
         * field. Stable across runs apart from the volatile
         * uptime/epoch fields. */
        printf("M29A_HW: abi=%u profile=%s safe=%u\n",
               (unsigned)s.kernel_abi_ver, s.profile_hint,
               (unsigned)s.safe_mode);
        printf("M29A_HW: cpu_count=%u family=%u model=%u step=%u "
               "feat=0x%x vendor=%s\n",
               (unsigned)s.cpu_count, (unsigned)s.cpu_family,
               (unsigned)s.cpu_model, (unsigned)s.cpu_stepping,
               (unsigned)s.cpu_features, s.cpu_vendor);
        printf("M29A_HW: mem_total_pg=%lu mem_used_pg=%lu "
               "mem_free_pg=%lu\n",
               (unsigned long)s.mem_total_pages,
               (unsigned long)s.mem_used_pages,
               (unsigned long)s.mem_free_pages);
        printf("M29A_HW: bus pci=%u usb=%u blk=%u input=%u "
               "audio=%u battery=%u hub=%u display=%u\n",
               (unsigned)s.pci_count, (unsigned)s.usb_count,
               (unsigned)s.blk_count, (unsigned)s.input_count,
               (unsigned)s.audio_count, (unsigned)s.battery_count,
               (unsigned)s.hub_count, (unsigned)s.display_count);

        /* PASS verdict: at least one bus is non-zero and we have
         * a non-empty CPU vendor + non-zero memory. This proves
         * the snapshot was actually populated rather than zeroed. */
        int has_bus = (s.pci_count + s.usb_count + s.blk_count +
                       s.input_count + s.audio_count +
                       s.battery_count + s.hub_count +
                       s.display_count) > 0;
        if (s.cpu_vendor[0] != '\0' && s.mem_total_pages > 0 && has_bus) {
            printf("M29A_HW: PASS\n");
            return 0;
        }
        printf("M29A_HW: FAIL: snapshot looks empty\n");
        return 1;
    }

    tobyhw_print_summary(stdout, &s);
    return 0;
}
