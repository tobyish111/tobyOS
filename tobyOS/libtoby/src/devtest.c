/* libtoby/src/devtest.c -- M26A peripheral test harness wrappers.
 *
 * The kernel exposes two syscalls (SYS_DEV_LIST, SYS_DEV_TEST). We
 * wrap them in the POSIX-shape "set errno + return -1" convention
 * everything else in libtoby uses. We also keep the table renderer
 * here so every userland tool (devlist, drvtest, usbtest, audiotest,
 * batterytest) writes the SAME columns -- no copy-pasted formatting. */

#include <tobyos_devtest.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "libtoby_internal.h"

int tobydev_list(struct abi_dev_info *out, size_t cap, uint32_t mask) {
    if (!out)   { errno = EFAULT; return -1; }
    if (cap == 0) { errno = EINVAL; return -1; }
    if (cap > ABI_DEVT_MAX_DEVICES) cap = ABI_DEVT_MAX_DEVICES;
    long rv = toby_sc3(ABI_SYS_DEV_LIST,
                       (long)(uintptr_t)out,
                       (long)cap,
                       (long)mask);
    return (int)__toby_check(rv);
}

int tobydev_test(const char *name, char *msg_out, size_t msg_cap) {
    if (!name)             { errno = EINVAL; return -1; }
    if (msg_out && msg_cap > 0) msg_out[0] = '\0';
    long rv = toby_sc3(ABI_SYS_DEV_TEST,
                       (long)(uintptr_t)name,
                       (long)(uintptr_t)msg_out,
                       (long)msg_cap);
    /* PASS == 0; SKIP == +1 (ABI_DEVT_SKIP). Pass them through. */
    if (rv >= 0) return (int)rv;
    /* Negative -- map to errno + -1 like every other libtoby wrapper. */
    if ((unsigned long)rv > (unsigned long)-4096L) {
        errno = (int)(-rv);
    } else {
        errno = EIO;
    }
    return -1;
}

const char *tobydev_bus_str(uint8_t bus) {
    switch (bus) {
    case ABI_DEVT_BUS_PCI:     return "pci";
    case ABI_DEVT_BUS_USB:     return "usb";
    case ABI_DEVT_BUS_BLK:     return "blk";
    case ABI_DEVT_BUS_INPUT:   return "input";
    case ABI_DEVT_BUS_AUDIO:   return "audio";
    case ABI_DEVT_BUS_BATTERY: return "battery";
    case ABI_DEVT_BUS_HUB:     return "hub";
    case ABI_DEVT_BUS_DISPLAY: return "display";
    default:                   return "?";
    }
}

/* The header + per-record formatter share column widths so the table
 * lines up regardless of which tool calls them. Widths are tuned
 * for an 80-col terminal. */

void tobydev_print_header(FILE *fp) {
    if (!fp) return;
    fprintf(fp, "%-7s %-16s %-12s %-6s %s\n",
            "BUS", "NAME", "DRIVER", "STAT", "INFO");
    fprintf(fp, "%-7s %-16s %-12s %-6s %s\n",
            "---", "----", "------", "----", "----");
}

void tobydev_print_record(FILE *fp, const struct abi_dev_info *r) {
    if (!fp || !r) return;
    char stat[8];
    /* P=present, B=bound, A=active, E=error, R=removed. */
    int i = 0;
    stat[i++] = (r->status & ABI_DEVT_PRESENT) ? 'P' : '-';
    stat[i++] = (r->status & ABI_DEVT_BOUND)   ? 'B' : '-';
    stat[i++] = (r->status & ABI_DEVT_ACTIVE)  ? 'A' : '-';
    stat[i++] = (r->status & ABI_DEVT_ERROR)   ? 'E' : '-';
    stat[i++] = (r->status & ABI_DEVT_REMOVED) ? 'R' : '-';
    stat[i]   = '\0';

    fprintf(fp, "%-7s %-16s %-12s %-6s %s\n",
            tobydev_bus_str(r->bus),
            r->name[0]   ? r->name   : "(unnamed)",
            r->driver[0] ? r->driver : "-",
            stat,
            r->extra[0]  ? r->extra  : "");
}

/* ============================================================
 * M26C hot-plug event drain
 * ============================================================ */

int tobydev_hot_drain(struct abi_hot_event *out, int cap) {
    if (cap < 0)              { errno = EINVAL; return -1; }
    if (cap > 0 && !out)      { errno = EFAULT; return -1; }
    long rv = toby_sc2(ABI_SYS_HOT_DRAIN,
                       (long)(uintptr_t)out,
                       (long)cap);
    return (int)__toby_check(rv);
}

void tobydev_print_hot_event(FILE *fp, const struct abi_hot_event *ev) {
    if (!fp || !ev) return;
    const char *act = "?";
    switch (ev->action) {
    case ABI_HOT_ATTACH: act = "+attach"; break;
    case ABI_HOT_DETACH: act = "-detach"; break;
    case ABI_HOT_ERROR:  act = "!error";  break;
    }
    fprintf(fp,
            "[%llu ms] seq=%llu %s bus=%s slot=%u depth=%u port=%u "
            "dropped=%u info=\"%s\"\n",
            (unsigned long long)ev->time_ms,
            (unsigned long long)ev->seq,
            act,
            tobydev_bus_str(ev->bus),
            (unsigned)ev->slot,
            (unsigned)ev->hub_depth,
            (unsigned)ev->hub_port,
            (unsigned)ev->dropped,
            ev->info[0] ? ev->info : "(none)");
}

/* ============================================================
 *  M27A: display introspection
 * ============================================================ */

int tobydisp_list(struct abi_display_info *out, size_t cap) {
    if (cap == 0) return 0;
    if (!out) { errno = EFAULT; return -1; }
    if (cap > ABI_DISPLAY_MAX_OUTPUTS) cap = ABI_DISPLAY_MAX_OUTPUTS;
    long rv = toby_sc2(ABI_SYS_DISPLAY_INFO,
                       (long)(uintptr_t)out,
                       (long)cap);
    return (int)__toby_check(rv);
}

const char *tobydisp_backend_str(uint8_t backend_id) {
    switch (backend_id) {
    case ABI_DISPLAY_BACKEND_LIMINE_FB:  return "limine-fb";
    case ABI_DISPLAY_BACKEND_VIRTIO_GPU: return "virtio-gpu";
    default:                             return "(none)";
    }
}

const char *tobydisp_format_str(uint8_t fmt) {
    switch (fmt) {
    case ABI_DISPLAY_FMT_XRGB8888: return "XRGB8888";
    case ABI_DISPLAY_FMT_ARGB8888: return "ARGB8888";
    default:                       return "?";
    }
}

void tobydisp_print_header(FILE *fp) {
    if (!fp) return;
    /* M27G: added ORIGIN column so an at-a-glance multi-monitor layout
     * is visible. The flip counter moves to the trailing free-form
     * "flips=" suffix to keep column widths sane. */
    fprintf(fp, "%-3s %-10s %-12s %-10s %-5s %-9s %-9s %-12s %s\n",
            "IDX", "NAME", "BACKEND", "RES", "BPP", "PITCH", "FORMAT",
            "ORIGIN", "FLAGS");
    fprintf(fp, "%-3s %-10s %-12s %-10s %-5s %-9s %-9s %-12s %s\n",
            "---", "----", "-------", "---", "---", "-----", "------",
            "------", "-----");
}

void tobydisp_print_record(FILE *fp, const struct abi_display_info *r) {
    if (!fp || !r) return;
    char res[16];
    snprintf(res, sizeof res, "%ux%u", (unsigned)r->width, (unsigned)r->height);

    /* M27G: print origin as "(x,y)". int32_t can be negative so always
     * use signed conversions. */
    char origin[16];
    snprintf(origin, sizeof origin, "(%d,%d)",
             (int)r->origin_x, (int)r->origin_y);

    char flags[8];
    int i = 0;
    flags[i++] = (r->status & ABI_DISPLAY_PRESENT) ? 'P' : '-';
    flags[i++] = (r->status & ABI_DISPLAY_PRIMARY) ? '*' : '-';
    flags[i++] = (r->status & ABI_DISPLAY_ACTIVE)  ? 'A' : '-';
    flags[i]   = '\0';

    /* libtoby printf supports %lu (long) but not %llu (long long), so we
     * cap the displayed flip counter at ULONG_MAX. On x86_64 that's 64
     * bits anyway so the cast is a no-op; on a hypothetical 32-bit
     * build the count would saturate -- still good enough for sanity. */
    fprintf(fp, "%-3u %-10s %-12s %-10s %-5u %-9u %-9s %-12s %s flips=%lu\n",
            (unsigned)r->index,
            r->name[0] ? r->name : "(unnamed)",
            r->backend[0] ? r->backend : tobydisp_backend_str(r->backend_id),
            res,
            (unsigned)r->bpp,
            (unsigned)r->pitch_bytes,
            tobydisp_format_str(r->pixel_format),
            origin,
            flags,
            (unsigned long)r->flips);
}
