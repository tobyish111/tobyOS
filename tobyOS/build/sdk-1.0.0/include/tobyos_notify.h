/* tobyos_notify.h -- libtoby userland wrappers for the M31 desktop
 * notification ring.
 *
 * Three thin syscall wrappers, plus one read-side convenience
 * (unread_count). All four follow the libtoby convention: return the
 * natural success value (>= 0) on success, or -1 with errno set on
 * failure.
 *
 *     notify_post     -- post one toast,    SYS_NOTIFY_POST
 *     notify_list     -- snapshot the ring, SYS_NOTIFY_LIST
 *     notify_dismiss  -- dismiss by id (0 = all), SYS_NOTIFY_DISMISS
 *     notify_unread   -- count records without DISMISSED set
 *
 * The on-the-wire record is `struct abi_notification` from the frozen
 * ABI header -- 200 bytes, byte-identical between any C and any future
 * C++ caller. App, title, body strings are clamped against
 * ABI_NOTIFY_APP_MAX / TITLE_MAX / BODY_MAX inside the kernel, so a
 * caller may pass arbitrarily long C strings without checking the
 * length first; everything past the cap is silently truncated.
 *
 * Posts emitted via this header are stamped by the kernel as
 * ABI_NOTIFY_KIND_USER and `app="user:<pid>"` so audit logs can
 * always tell userland-sourced toasts from kernel-emitted ones.
 */

#ifndef LIBTOBY_TOBYOS_NOTIFY_H
#define LIBTOBY_TOBYOS_NOTIFY_H

#include <stddef.h>
#include <stdint.h>
#include <tobyos/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Friendlier urgency aliases -- the underlying ABI_NOTIFY_URG_* names
 * also work and are interchangeable. */
#define TOBY_NOTIFY_INFO   ABI_NOTIFY_URG_INFO
#define TOBY_NOTIFY_WARN   ABI_NOTIFY_URG_WARN
#define TOBY_NOTIFY_ERR    ABI_NOTIFY_URG_ERR

/* Post a single notification. Returns the assigned monotonic id (>= 1)
 * on success, or -1 with errno set on failure (typically ENOSYS if the
 * notify subsystem isn't compiled in, or EBUSY if it isn't ready yet).
 *
 *   urgency : TOBY_NOTIFY_INFO / WARN / ERR
 *   title   : one-line headline (required, NULL is treated as "")
 *   body    : detail paragraph (NULL or "" => title-only) */
int toby_notify_post(unsigned urgency, const char *title, const char *body);

/* Snapshot up to `cap` records into `out`, newest-first. Returns the
 * number of records written (>= 0) on success, -1 with errno set on
 * failure. The destination buffer is written verbatim from the kernel
 * ring; fields are already NUL-clamped. */
long toby_notify_list(struct abi_notification *out, unsigned cap);

/* Dismiss one notification by id. Pass id == 0 to dismiss every entry
 * in the ring (the same effect as the in-shell "Clear all" button).
 * Returns 0 on success, -1 with errno set on failure. */
int toby_notify_dismiss(unsigned id);

/* Convenience: count records in the ring whose DISMISSED bit is clear.
 * Implemented by snapshotting the ring with notify_list and filtering
 * locally, so it costs one syscall plus a small stack buffer. Returns
 * the count on success, -1 with errno set on failure. */
long toby_notify_unread(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TOBYOS_NOTIFY_H */
