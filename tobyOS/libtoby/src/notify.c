/* libtoby/src/notify.c -- userland wrappers for the M31 desktop
 * notification syscalls (SYS_NOTIFY_POST/LIST/DISMISS).
 *
 * The kernel ABI returns negative -ABI_E* on failure / non-negative
 * on success; these wrappers translate that to the POSIX-shape
 * "errno + -1" convention every other libtoby call uses.
 *
 * For toby_notify_post we own the staging buffer ourselves so the
 * caller doesn't have to declare or zero a struct abi_notification:
 * pass urgency + two strings, get back an id. The kernel will
 * overwrite kind / app / id / time anyway; passing zero in those
 * fields here makes that contract obvious. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <tobyos_notify.h>
#include "libtoby_internal.h"

/* Local strncpy-with-NUL: avoids dragging in <string.h>'s symbol just
 * to copy a bounded string. dst is always NUL-terminated provided
 * cap > 0. */
static void copy_capped(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

int toby_notify_post(unsigned urgency, const char *title, const char *body) {
    struct abi_notification rec;
    /* Zero the whole record so the kernel gets a clean staging copy
     * regardless of which fields we explicitly populate. The kernel
     * overrides id/kind/flags/time/app anyway -- the only fields the
     * caller actually controls are urgency + the three strings. */
    memset(&rec, 0, sizeof(rec));
    rec.urgency = urgency;
    rec.kind    = ABI_NOTIFY_KIND_USER;     /* informational; kernel overrides */
    copy_capped(rec.app,   "user", ABI_NOTIFY_APP_MAX);
    copy_capped(rec.title, title,  ABI_NOTIFY_TITLE_MAX);
    copy_capped(rec.body,  body,   ABI_NOTIFY_BODY_MAX);

    long rv = toby_sc1(ABI_SYS_NOTIFY_POST, (long)(uintptr_t)&rec);
    return (int)__toby_check(rv);
}

long toby_notify_list(struct abi_notification *out, unsigned cap) {
    if (!out && cap > 0) {
        errno = EFAULT;
        return -1;
    }
    long rv = toby_sc2(ABI_SYS_NOTIFY_LIST,
                       (long)(uintptr_t)out, (long)cap);
    return __toby_check(rv);
}

int toby_notify_dismiss(unsigned id) {
    long rv = toby_sc1(ABI_SYS_NOTIFY_DISMISS, (long)id);
    return (int)__toby_check(rv);
}

long toby_notify_unread(void) {
    /* The ring caps at 32 entries (notify.c::NOTIFY_MAX); we mirror
     * that here so a single snapshot covers everything. If the cap
     * ever grows we'll still never under-count -- the worst case is
     * we miss the OLDEST records, and unread is dominated by the
     * newest end of the ring anyway. */
    struct abi_notification buf[32];
    long n = toby_notify_list(buf, sizeof(buf) / sizeof(buf[0]));
    if (n < 0) return -1;
    long unread = 0;
    for (long i = 0; i < n; i++) {
        if (!(buf[i].flags & ABI_NOTIFY_FLAG_DISMISSED)) unread++;
    }
    return unread;
}
