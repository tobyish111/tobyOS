/* libtoby/src/hwinfo.c -- M29A SYS_HWINFO wrapper + formatting helpers.
 *
 * The kernel returns one snapshot struct per call. Userland tools
 * (hwinfo, hwreport, hwtest, bringuptest) all share the formatters
 * here so the printed text stays consistent across the entire M29
 * milestone. */

#include <tobyos_hwinfo.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "libtoby_internal.h"

int tobyhw_summary(struct abi_hwinfo_summary *out) {
    if (!out) { errno = EFAULT; return -1; }
    long rv = toby_sc1(ABI_SYS_HWINFO, (long)(uintptr_t)out);
    if (rv == 0) return 0;
    if ((unsigned long)rv > (unsigned long)-4096L) {
        errno = (int)(-rv);
    } else {
        errno = EIO;
    }
    return -1;
}

size_t tobyhw_format_features(uint32_t feat, char *dst, size_t cap) {
    static const struct { uint32_t bit; const char *name; } map[] = {
        { ABI_HW_CPU_FEAT_FPU,   "fpu"   },
        { ABI_HW_CPU_FEAT_TSC,   "tsc"   },
        { ABI_HW_CPU_FEAT_MSR,   "msr"   },
        { ABI_HW_CPU_FEAT_PAE,   "pae"   },
        { ABI_HW_CPU_FEAT_APIC,  "apic"  },
        { ABI_HW_CPU_FEAT_SSE,   "sse"   },
        { ABI_HW_CPU_FEAT_SSE2,  "sse2"  },
        { ABI_HW_CPU_FEAT_HT,    "ht"    },
        { ABI_HW_CPU_FEAT_LM,    "lm"    },
        { ABI_HW_CPU_FEAT_HYPER, "hyper" },
        { ABI_HW_CPU_FEAT_NX,    "nx"    },
    };
    if (cap == 0) return 0;
    dst[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (!(feat & map[i].bit)) continue;
        size_t nlen = strlen(map[i].name);
        size_t need = nlen + (off ? 1 : 0);
        if (off + need + 1 >= cap) break;
        if (off) dst[off++] = ' ';
        memcpy(dst + off, map[i].name, nlen);
        off += nlen;
        dst[off] = '\0';
    }
    return off;
}

size_t tobyhw_format_cpu_line(const struct abi_hwinfo_summary *s,
                              char *dst, size_t cap) {
    if (!s || !dst || cap == 0) return 0;
    char feat[80];
    tobyhw_format_features(s->cpu_features, feat, sizeof(feat));
    int n = snprintf(dst, cap,
                     "%s %s family=%u model=%u step=%u cpus=%u feat=%s",
                     s->cpu_vendor[0] ? s->cpu_vendor : "unknown",
                     s->cpu_brand [0] ? s->cpu_brand  : "unknown",
                     (unsigned)s->cpu_family,
                     (unsigned)s->cpu_model,
                     (unsigned)s->cpu_stepping,
                     (unsigned)s->cpu_count,
                     feat[0] ? feat : "(none)");
    if (n < 0) { dst[0] = '\0'; return 0; }
    return ((size_t)n < cap) ? (size_t)n : (cap - 1);
}

void tobyhw_print_summary(FILE *fp,
                          const struct abi_hwinfo_summary *s) {
    if (!fp || !s) return;

    char feat[80];
    tobyhw_format_features(s->cpu_features, feat, sizeof(feat));

    fprintf(fp,
            "tobyOS hardware inventory (epoch=%lu uptime_ms=%lu)\n",
            (unsigned long)s->snapshot_epoch,
            (unsigned long)s->boot_uptime_ms);
    fprintf(fp, "abi=%u  mode=%s  profile=%s\n",
            (unsigned)s->kernel_abi_ver,
            tobyhw_boot_mode_str(s->safe_mode),
            s->profile_hint);
    fprintf(fp,
            "cpu  : vendor=%s family=%u model=%u step=%u count=%u\n",
            s->cpu_vendor, (unsigned)s->cpu_family,
            (unsigned)s->cpu_model, (unsigned)s->cpu_stepping,
            (unsigned)s->cpu_count);
    fprintf(fp, "       brand=\"%s\"\n", s->cpu_brand);
    fprintf(fp, "       feat =%s\n", feat[0] ? feat : "(none)");
    fprintf(fp, "mem  : total=%lu pg  used=%lu pg  free=%lu pg\n",
            (unsigned long)s->mem_total_pages,
            (unsigned long)s->mem_used_pages,
            (unsigned long)s->mem_free_pages);
    fprintf(fp, "bus  : pci=%u usb=%u blk=%u input=%u audio=%u "
                "battery=%u hub=%u display=%u\n",
            (unsigned)s->pci_count, (unsigned)s->usb_count,
            (unsigned)s->blk_count, (unsigned)s->input_count,
            (unsigned)s->audio_count, (unsigned)s->battery_count,
            (unsigned)s->hub_count, (unsigned)s->display_count);
}

const char *tobyhw_profile_str(const struct abi_hwinfo_summary *s) {
    if (!s || s->profile_hint[0] == '\0') return "vm";
    return s->profile_hint;
}

int tobyhw_drvmatch(uint32_t bus, uint32_t vendor, uint32_t device,
                    struct abi_drvmatch_info *out) {
    if (!out) { errno = EFAULT; return -1; }
    long rv = toby_sc4(ABI_SYS_DRVMATCH,
                       (long)bus, (long)vendor, (long)device,
                       (long)(uintptr_t)out);
    if (rv == 0) return 0;
    if ((unsigned long)rv > (unsigned long)-4096L) {
        errno = (int)(-rv);
    } else {
        errno = EIO;
    }
    return -1;
}

const char *tobyhw_strategy_str(uint32_t strategy) {
    switch (strategy) {
    case ABI_DRVMATCH_NONE:        return "NONE";
    case ABI_DRVMATCH_EXACT:       return "EXACT";
    case ABI_DRVMATCH_CLASS:       return "CLASS";
    case ABI_DRVMATCH_GENERIC:     return "GENERIC";
    case ABI_DRVMATCH_UNSUPPORTED: return "UNSUPPORTED";
    case ABI_DRVMATCH_FORCED_OFF:  return "FORCED_OFF";
    default:                       return "?";
    }
}

int tobyhw_compat_list(struct abi_hwcompat_entry *out,
                       unsigned int cap,
                       unsigned int flags) {
    if (!out) { errno = EFAULT; return -1; }
    if (cap == 0) return 0;
    long rv = toby_sc3(ABI_SYS_HWCOMPAT_LIST,
                       (long)(uintptr_t)out, (long)cap, (long)flags);
    if (rv >= 0) return (int)rv;
    if ((unsigned long)rv > (unsigned long)-4096L) {
        errno = (int)(-rv);
    } else {
        errno = EIO;
    }
    return -1;
}

const char *tobyhw_compat_status_str(uint32_t status) {
    switch (status) {
    case ABI_HWCOMPAT_SUPPORTED:   return "supported";
    case ABI_HWCOMPAT_PARTIAL:     return "partial";
    case ABI_HWCOMPAT_UNSUPPORTED: return "unsupported";
    case ABI_HWCOMPAT_UNKNOWN:     return "unknown";
    default:                       return "?";
    }
}

const char *tobyhw_compat_bus_str(uint32_t bus) {
    switch (bus) {
    case ABI_DEVT_BUS_PCI:     return "pci";
    case ABI_DEVT_BUS_USB:     return "usb";
    case ABI_DEVT_BUS_BLK:     return "blk";
    case ABI_DEVT_BUS_INPUT:   return "input";
    case ABI_DEVT_BUS_AUDIO:   return "audio";
    case ABI_DEVT_BUS_BATTERY: return "bat";
    case ABI_DEVT_BUS_HUB:     return "hub";
    case ABI_DEVT_BUS_DISPLAY: return "fb";
    default:                   return "?";
    }
}

const char *tobyhw_boot_mode_str(uint32_t mode) {
    switch (mode) {
    case ABI_BOOT_MODE_NORMAL:        return "normal";
    case ABI_BOOT_MODE_SAFE_BASIC:    return "safe-basic";
    case ABI_BOOT_MODE_SAFE_GUI:      return "safe-gui";
    case ABI_BOOT_MODE_VERBOSE:       return "verbose";
    case ABI_BOOT_MODE_COMPATIBILITY: return "compatibility";
    default:                          return "?";
    }
}
