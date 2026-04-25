/* notify.h -- M31 notification daemon (kernel-side, single ring).
 *
 * The notification subsystem owns a fixed-size ring of recent
 * notifications. Sources call notify_post() to emit; the desktop
 * compositor pulls from the ring to paint toasts and the
 * notification-center panel.
 *
 * There is no userspace "daemon" process: the daemon is a built-in
 * service registered via service_register_builtin("notify"), which
 * makes it discoverable through `services` the same way networking
 * and gui are. When (later) we want a userspace daemon, this in-
 * kernel ring becomes its backing store and the service kind flips
 * to PROGRAM -- no API change.
 *
 * Concurrency: notify_post is safe to call from IRQ context (it only
 * mutates the ring under a spinlock and never allocates). The reader
 * APIs (notify_iter / notify_pop_pending_toast / notify_get_records)
 * run on pid 0 from the compositor pass.
 *
 * The on-the-wire record (struct abi_notification, defined in
 * <tobyos/abi/abi.h>) is shared verbatim with userspace via
 * SYS_NOTIFY_POST and SYS_NOTIFY_LIST -- byte-stable layout.
 */

#ifndef TOBYOS_NOTIFY_H
#define TOBYOS_NOTIFY_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Internal ring depth. Sized for "comfortably more than the user
 * could read without dismissing"; the oldest entries are evicted
 * silently when the ring overflows. */
#define NOTIFY_MAX           32

/* How long a toast stays on screen before auto-dismiss. */
#define NOTIFY_TOAST_LIFETIME_MS  4000u

/* Convenience aliases to the ABI urgency/kind constants so kernel
 * call sites read naturally without dragging in the abi/ prefix. */
#define NOTIFY_URG_INFO     ABI_NOTIFY_URG_INFO
#define NOTIFY_URG_WARN     ABI_NOTIFY_URG_WARN
#define NOTIFY_URG_ERR      ABI_NOTIFY_URG_ERR

/* ---- lifecycle ---------------------------------------------------- */

/* Boot-time entry. Zero state, register the "notify" builtin service.
 * Safe to call before the service registry exists -- the registration
 * is best-effort and a missing service.c is logged but not fatal. */
void notify_init(void);

/* ---- emit --------------------------------------------------------- */

/* Post a notification. `app` / `title` / `body` are NUL-terminated;
 * passing NULL for any is treated as "". Strings are copied into the
 * ring immediately so callers can free their buffers right after.
 *
 * Returns the assigned notification id (>= 1) on success, 0 if the
 * subsystem is not yet initialised (the call is a no-op).
 *
 * Safe to call from any context (IRQ, syscall, kernel thread). */
uint32_t notify_post(uint32_t kind,
                     uint32_t urgency,
                     const char *app,
                     const char *title,
                     const char *body);

/* ---- read --------------------------------------------------------- */

/* Snapshot the ring into the caller's buffer, newest first. Returns
 * the number of records written (<= cap). Used by the
 * notification-center panel and by SYS_NOTIFY_LIST. Skips entries
 * marked dismissed. */
uint32_t notify_get_records(struct abi_notification *out, uint32_t cap);

/* Pull the next entry that has NOT been displayed as a toast yet,
 * and mark it as displayed. Returns true with *out filled in, or
 * false if the ring has no pending toast. Called once per
 * compositor pass when the toast slot is free. */
bool notify_pop_pending_toast(struct abi_notification *out);

/* ---- dismiss ------------------------------------------------------ */

/* Mark a single notification dismissed. No-op if id is unknown.
 * Callers that hold an id from notify_post can use this to
 * dismiss a specific entry. */
void notify_dismiss(uint32_t id);

/* Dismiss every entry in the ring. Used by "Clear all" in the
 * notification center panel. */
void notify_dismiss_all(void);

/* ---- diagnostics -------------------------------------------------- */

/* Counts (live = posted - dismissed - evicted). Used by the bell
 * badge to show an unread count and by `services notify` for
 * introspection. */
uint32_t notify_unread_count(void);
uint32_t notify_total_posted(void);

/* Print the ring to the kernel log. Cheap. */
void notify_dump(void);

#endif /* TOBYOS_NOTIFY_H */
