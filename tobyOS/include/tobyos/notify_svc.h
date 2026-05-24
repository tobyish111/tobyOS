/* notify_svc.h -- Milestone 7 notification service (Windows-parity).
 *
 * Higher-level notification service with a ring buffer of 64
 * notifications. Layered on top of the existing M31 notify ring but
 * provides a simpler API surface for milestone 7's IPC story.
 */

#ifndef TOBYOS_NOTIFY_SVC_H
#define TOBYOS_NOTIFY_SVC_H

#include <tobyos/types.h>
#include <stdint.h>

#define NOTIFY_SVC_MAX       64
#define NOTIFY_TITLE_MAX     64
#define NOTIFY_BODY_MAX      256
#define NOTIFY_ICON_MAX      32

struct notification {
    int      id;
    int      source_pid;
    char     title[NOTIFY_TITLE_MAX];
    char     body[NOTIFY_BODY_MAX];
    char     icon[NOTIFY_ICON_MAX];
    uint32_t timestamp;
    int      dismissed;
};

void notify_svc_init(void);
int  notify_svc_send(int pid, const char *title, const char *body, const char *icon);
int  notify_svc_get_pending(struct notification *out, int max_count);
int  notify_svc_dismiss_one(int notif_id);
int  notify_svc_dismiss_all_entries(void);
int  notify_svc_get_count(void);

/* Syscall entry points (called from syscall.c) */
long sys_notify_svc_send(const char *title, const char *body, const char *icon);
long sys_notify_svc_get(struct notification *out, int max_count);
long sys_notify_svc_dismiss(int notif_id);

#endif /* TOBYOS_NOTIFY_SVC_H */
