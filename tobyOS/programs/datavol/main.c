/* datavol -- give tobyOS a PERSISTENT /data volume, from the shell.
 *
 * WHY THIS EXISTS. tobyOS has had the whole provisioning mechanism for a
 * while -- a GPT partition carrying the tobyOS-data type GUID, discovered
 * and mounted at every boot, behind a kernel guard that refuses to write
 * anyone else's disk. What it did not have was a way to REACH that from a
 * terminal: the only front end was the GUI disk manager, which is no help
 * over a serial console, and no help at all if you do not already know the
 * feature exists.
 *
 * So on real hardware /data fell through to the RAM fallback every boot and
 * everything the user saved disappeared on reboot -- not because the
 * machinery was missing, but because nothing exposed it.
 *
 *   datavol                 list every block device with its verdict
 *   datavol list            same
 *   datavol create DEV      provision DEV as the persistent /data volume
 *   datavol create DEV --erase
 *                           ...destroying a foreign filesystem on REMOVABLE
 *                           media. Requires typing the device name back.
 *
 * SAFETY. This program decides nothing. The kernel re-runs its guard on
 * every request and refuses, regardless of what is asked:
 *   - a fixed disk carrying a foreign filesystem or partition table (the
 *     drive with somebody's Windows on it is unreachable by any flag),
 *   - the live boot medium (iso9660),
 *   - anything backing a live mount.
 * The confirmation below exists to stop a TYPO, not to grant permission.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <tobyos/abi/abi.h>

static long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b)
                      : "rcx", "r11", "memory");
    return r;
}
static long sc1(long n, long a) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "0"(n), "D"(a)
                      : "rcx", "r11", "memory");
    return r;
}

#define MAX_DEVS 32

static const char *verdict_word(unsigned v) {
    switch (v) {
    case ABI_BLKV_UNKNOWN:  return "unknown";
    case ABI_BLKV_BLANK:    return "blank";
    case ABI_BLKV_TOBYOS:   return "tobyOS";
    case ABI_BLKV_FOREIGN:  return "foreign";
    case ABI_BLKV_MOUNTED:  return "mounted";
    case ABI_BLKV_NOT_DISK: return "-";
    default:                return "?";
    }
}

/* What the user can DO with this device, which is the only question a
 * listing is really being asked. */
static const char *advice(const struct abi_blk_info *d) {
    if (d->class != 1) return "partition -- pick its disk instead";
    if (d->flags & ABI_BLK_F_RAM)     return "RAM-backed -- never persistent";
    if (d->flags & ABI_BLK_F_GONE)    return "hardware removed";
    if (d->flags & ABI_BLK_F_DATA)    return "this is /data now";
    switch (d->verdict) {
    case ABI_BLKV_BLANK:   return "READY: datavol create <dev>";
    case ABI_BLKV_TOBYOS:  return "tobyOS data already -- add --force to redo";
    case ABI_BLKV_MOUNTED: return "in use -- unmount first";
    case ABI_BLKV_FOREIGN:
        if (d->fs[0] && strcmp(d->fs, "iso9660") == 0)
            return "the live boot medium -- never erasable";
        if (d->flags & ABI_BLK_F_REMOVABLE)
            return "REMOVABLE: datavol create <dev> --erase  (DESTROYS IT)";
        return "fixed disk with data on it -- refused, no override";
    default: return "not a provisioning target";
    }
}

static void human_size(uint64_t sectors, char *out, size_t cap) {
    uint64_t mib = sectors / 2048u;
    if (mib >= 1024) snprintf(out, cap, "%lu.%lu GiB",
                              (unsigned long)(mib / 1024),
                              (unsigned long)((mib % 1024) * 10 / 1024));
    else             snprintf(out, cap, "%lu MiB", (unsigned long)mib);
}

static int list_devices(struct abi_blk_info *v, int *n_out) {
    long n = sc2(ABI_SYS_BLK_LIST, (long)(uintptr_t)v, (long)MAX_DEVS);
    if (n < 0) {
        fprintf(stderr, "datavol: cannot list block devices (%ld)\n", n);
        return -1;
    }
    *n_out = (int)n;
    return 0;
}

static int cmd_list(void) {
    struct abi_blk_info v[MAX_DEVS];
    int n = 0;
    if (list_devices(v, &n) != 0) return 1;
    if (n == 0) { printf("no block devices\n"); return 0; }

    printf("%-14s %-9s %-10s %-8s %s\n",
           "DEVICE", "SIZE", "CONTENTS", "VERDICT", "WHAT YOU CAN DO");
    for (int i = 0; i < n; i++) {
        char sz[24];
        human_size(v[i].sector_count, sz, sizeof sz);
        printf("%-14s %-9s %-10s %-8s %s\n",
               v[i].name, sz,
               v[i].fs[0] ? v[i].fs : (v[i].class == 1 ? "(empty)" : "-"),
               v[i].class == 1 ? verdict_word(v[i].verdict) : "-",
               advice(&v[i]));
        if (v[i].model[0])
            printf("%-14s   %s%s\n", "", v[i].model,
                   (v[i].flags & ABI_BLK_F_REMOVABLE) ? "  [removable]" : "");
    }
    printf("\n/data is persistent only when it sits on one of these. A "
           "RAM-backed /data\nresets on every reboot.\n");
    return 0;
}

/* Shared by `create` and `format`: name what is about to be destroyed and
 * make the user type the device back. Guards against a TYPO -- the kernel
 * guard is what actually decides. Returns 0 to proceed. */
static int confirm_destroy(const struct abi_blk_info *t, const char *verb,
                           const char *sz) {
    printf("\n  ABOUT TO %s  %s  (%s%s%s)\n", verb, t->name, sz,
           t->model[0] ? ", " : "", t->model[0] ? t->model : "");
    printf("  It currently holds: %s\n",
           t->fs[0] ? t->fs : "an unrecognised partition table");
    printf("  EVERYTHING ON IT WILL BE LOST.\n\n");
    printf("  Type the device name to confirm: ");
    fflush(stdout);
    char answer[64];
    if (!fgets(answer, sizeof answer, stdin)) { printf("\naborted\n"); return 1; }
    size_t a = strlen(answer);
    while (a && (answer[a-1] == '\n' || answer[a-1] == '\r')) answer[--a] = 0;
    if (strcmp(answer, t->name) != 0) {
        printf("\n  not confirmed ('%s' != '%s') -- nothing was written\n",
               answer, t->name);
        return 1;
    }
    return 0;
}

/* Report the kernel's REASON. The whole point of the guard is that its
 * refusals are informative; "failed" would throw that away. */
static void explain_failure(long rc, const char *dev) {
    long e = -rc;
    if      (e == ABI_EPERM)  fprintf(stderr,
        "datavol: refused -- '%s' carries data tobyOS will not overwrite.\n"
        "  A fixed disk with a foreign filesystem, or the live boot medium,\n"
        "  cannot be written by any flag. Removable media with a foreign\n"
        "  filesystem need --erase.\n", dev);
    else if (e == ABI_EBUSY)  fprintf(stderr,
        "datavol: refused -- '%s' backs a live mount. Unmount it first.\n", dev);
    else if (e == ABI_EEXIST) fprintf(stderr,
        "datavol: '%s' already holds tobyOS data. Add --force to redo it.\n", dev);
    else if (e == ABI_EINVAL) fprintf(stderr,
        "datavol: '%s' is not a usable whole disk (too small, or RAM-backed).\n",
        dev);
    else if (e == ABI_EIO)    fprintf(stderr,
        "datavol: '%s' failed while writing/formatting -- the device may be "
        "faulty.\n", dev);
    else fprintf(stderr, "datavol: failed (%ld)\n", rc);
}

/* `datavol format DEV` -- a plain tobyfs across the whole stick, which is
 * what "format this USB" means. /data is untouched. */
static int cmd_format(const char *dev, unsigned flags) {
    struct abi_blk_info v[MAX_DEVS];
    int n = 0;
    if (list_devices(v, &n) != 0) return 1;

    const struct abi_blk_info *t = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(v[i].name, dev) == 0) { t = &v[i]; break; }
    if (!t) {
        fprintf(stderr, "datavol: no such device '%s' -- run `datavol list`\n",
                dev);
        return 1;
    }
    if (t->class != 1) {
        fprintf(stderr, "datavol: '%s' is a partition; format the whole "
                        "disk instead\n", dev);
        return 1;
    }

    char sz[24];
    human_size(t->sector_count, sz, sizeof sz);

    /* Formatting always destroys, so always confirm -- not only under
     * --erase the way `create` does for a blank disk. */
    if (confirm_destroy(t, "FORMAT", sz) != 0) return 1;

    struct abi_provision_req req;
    memset(&req, 0, sizeof req);
    snprintf(req.dev, sizeof req.dev, "%s", dev);
    req.flags = flags | ABI_PROV_F_FORMAT_ONLY;

    long rc = sc1(ABI_SYS_DATA_PROVISION, (long)(uintptr_t)&req);
    if (rc == ABI_PROV_OK_MOUNTED || rc == ABI_PROV_OK_NEXT_BOOT) {
        printf("\n'%s' (%s) is formatted. To use it:\n\n"
               "    mkdir -p /mnt/usb\n"
               "    mount -t tobyfs %s /mnt/usb\n\n"
               "/data was not touched. Note that a tobyfs volume can be\n"
               "adopted as /data on a later boot if nothing else claims it.\n",
               dev, sz, dev);
        return 0;
    }
    explain_failure(rc, dev);
    return 1;
}

static int cmd_create(const char *dev, unsigned flags) {
    struct abi_blk_info v[MAX_DEVS];
    int n = 0;
    if (list_devices(v, &n) != 0) return 1;

    const struct abi_blk_info *t = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(v[i].name, dev) == 0) { t = &v[i]; break; }
    if (!t) {
        fprintf(stderr, "datavol: no such device '%s' -- run `datavol list`\n",
                dev);
        return 1;
    }
    if (t->class != 1) {
        fprintf(stderr, "datavol: '%s' is a partition; provision its whole "
                        "disk instead\n", dev);
        return 1;
    }

    char sz[24];
    human_size(t->sector_count, sz, sizeof sz);

    /* Only --erase destroys somebody else's filesystem; a blank or
     * already-tobyOS target does not need the typed confirmation. */
    if (flags & ABI_PROV_F_ERASE) {
        if (confirm_destroy(t, "ERASE", sz) != 0) return 1;
    }

    struct abi_provision_req req;
    memset(&req, 0, sizeof req);
    snprintf(req.dev, sizeof req.dev, "%s", dev);
    req.flags = flags;

    long rc = sc1(ABI_SYS_DATA_PROVISION, (long)(uintptr_t)&req);
    switch (rc) {
    case ABI_PROV_OK_MOUNTED:
        printf("\n/data is now on '%s' (%s) and mounted. It will be found "
               "again on every boot.\n", dev, sz);
        return 0;
    case ABI_PROV_OK_NEXT_BOOT:
        printf("\n'%s' (%s) is now a tobyOS data volume. /data is already "
               "mounted elsewhere,\nso this one is picked up on the next "
               "boot.\n", dev, sz);
        return 0;
    default: break;
    }

    explain_failure(rc, dev);
    return 1;
}

static void usage(FILE *out) {
    fprintf(out,
        "usage: datavol [list]\n"
        "       datavol format DEV [--erase]        format a USB stick\n"
        "       datavol create DEV [--force] [--erase]\n"
        "\n"
        "  list          show every block device and what can be done with it\n"
        "\n"
        "  format DEV    lay a plain tobyfs across DEV and stop. This is\n"
        "                \"format my USB stick\": /data is NOT touched, and\n"
        "                the result is mounted with\n"
        "                    mount -t tobyfs DEV /mnt/usb\n"
        "                Always asks you to type the device name.\n"
        "\n"
        "  create DEV    make DEV the persistent /data volume instead\n"
        "    --force     DEV already holds tobyOS data; redo it\n"
        "\n"
        "  --erase       DEV holds a FOREIGN filesystem and is REMOVABLE;\n"
        "                destroy it. Required for either command.\n"
        "\n"
        "The kernel refuses fixed disks with foreign data, the live boot\n"
        "medium, and anything mounted -- no flag here overrides that.\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "list") == 0) return cmd_list();
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    int is_format = (strcmp(argv[1], "format") == 0);
    if (is_format || strcmp(argv[1], "create") == 0) {
        if (argc < 3) { usage(stderr); return 2; }
        unsigned flags = 0;
        for (int i = 3; i < argc; i++) {
            if      (strcmp(argv[i], "--force") == 0) flags |= ABI_PROV_F_FORCE;
            else if (strcmp(argv[i], "--erase") == 0) flags |= ABI_PROV_F_ERASE;
            else { fprintf(stderr, "datavol: unknown option '%s'\n", argv[i]);
                   usage(stderr); return 2; }
        }
        return is_format ? cmd_format(argv[2], flags)
                         : cmd_create(argv[2], flags);
    }
    fprintf(stderr, "datavol: unknown command '%s'\n", argv[1]);
    usage(stderr);
    return 2;
}
