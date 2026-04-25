/* libtoby/src/stab.c -- Milestone 28G userland wrapper for the
 * stability self-test syscall. Mirrors libtoby/src/svc.c. */

#include <tobyos_stab.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "libtoby_internal.h"

int tobystab_run(struct abi_stab_report *out, uint32_t mask) {
    if (!out) { errno = EFAULT; return -1; }
    long rv = toby_sc2(ABI_SYS_STAB_SELFTEST,
                       (long)(uintptr_t)out,
                       (long)mask);
    /* The kernel returns 0 on full PASS and -EIO when at least one
     * probe failed. In the latter case the report is still valid
     * and out->fail_count tells the caller how many probes failed.
     * We translate that into a positive return value so callers
     * don't have to inspect errno just to differentiate. */
    if (rv < 0) {
        if (rv == -ABI_EIO) return (int)out->fail_count;
        errno = (int)(-rv);
        return -1;
    }
    return 0;
}

const char *tobystab_bit_name(uint32_t bit) {
    switch (bit) {
    case ABI_STAB_OK_BOOT:        return "boot";
    case ABI_STAB_OK_LOG:         return "log";
    case ABI_STAB_OK_PANIC:       return "panic";
    case ABI_STAB_OK_WATCHDOG:    return "watchdog";
    case ABI_STAB_OK_FILESYSTEM:  return "filesystem";
    case ABI_STAB_OK_SERVICES:    return "services";
    case ABI_STAB_OK_GUI:         return "gui";
    case ABI_STAB_OK_TERMINAL:    return "terminal";
    case ABI_STAB_OK_NETWORK:     return "network";
    case ABI_STAB_OK_INPUT:       return "input";
    case ABI_STAB_OK_SAFE_MODE:   return "safe_mode";
    case ABI_STAB_OK_DISPLAY:     return "display";
    default:                      return "?";
    }
}

static void cat_str(char *dst, size_t cap, size_t *off, const char *s) {
    if (!dst || !s || cap == 0) return;
    while (*s && *off + 1 < cap) dst[(*off)++] = *s++;
    dst[*off] = '\0';
}

void tobystab_format_mask(char *dst, size_t cap, uint32_t mask) {
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    size_t off = 0;
    bool first = true;
    static const uint32_t bits[] = {
        ABI_STAB_OK_BOOT, ABI_STAB_OK_LOG, ABI_STAB_OK_PANIC,
        ABI_STAB_OK_WATCHDOG, ABI_STAB_OK_FILESYSTEM,
        ABI_STAB_OK_SERVICES, ABI_STAB_OK_GUI, ABI_STAB_OK_TERMINAL,
        ABI_STAB_OK_NETWORK, ABI_STAB_OK_INPUT,
        ABI_STAB_OK_SAFE_MODE, ABI_STAB_OK_DISPLAY,
    };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if ((mask & bits[i]) == 0) continue;
        if (!first) cat_str(dst, cap, &off, ",");
        cat_str(dst, cap, &off, tobystab_bit_name(bits[i]));
        first = false;
    }
}
