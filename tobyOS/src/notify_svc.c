/* notify_svc.c -- Milestone 7 notification service implementation.
 *
 * Ring buffer of 64 notifications. Provides a higher-level service
 * interface layered on top of the existing M31 kernel notify ring.
 */

#include <tobyos/notify_svc.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pit.h>
#include <tobyos/proc.h>

static struct {
    struct notification ring[NOTIFY_SVC_MAX];
    int      head;
    int      count;
    int      next_id;
    bool     ready;
} g_nsvc;

static void nsvc_strncpy(char *dst, const char *src, size_t cap) {
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void notify_svc_init(void) {
    memset(&g_nsvc, 0, sizeof(g_nsvc));
    g_nsvc.next_id = 1;
    g_nsvc.ready = true;
    kprintf("[notify_svc] init: ring of %d entries\n", NOTIFY_SVC_MAX);
}

int notify_svc_send(int pid, const char *title, const char *body, const char *icon) {
    if (!g_nsvc.ready) return -1;

    int slot = g_nsvc.head;
    struct notification *n = &g_nsvc.ring[slot];

    memset(n, 0, sizeof(*n));
    n->id         = g_nsvc.next_id++;
    n->source_pid = pid;
    n->timestamp  = (uint32_t)pit_ticks();
    n->dismissed  = 0;

    nsvc_strncpy(n->title, title, NOTIFY_TITLE_MAX);
    nsvc_strncpy(n->body,  body,  NOTIFY_BODY_MAX);
    nsvc_strncpy(n->icon,  icon,  NOTIFY_ICON_MAX);

    g_nsvc.head = (g_nsvc.head + 1) % NOTIFY_SVC_MAX;
    if (g_nsvc.count < NOTIFY_SVC_MAX)
        g_nsvc.count++;

    return n->id;
}

int notify_svc_get_pending(struct notification *out, int max_count) {
    if (!g_nsvc.ready || !out || max_count <= 0) return 0;

    int written = 0;
    for (int i = 0; i < g_nsvc.count && written < max_count; i++) {
        int idx = (g_nsvc.head - g_nsvc.count + i + NOTIFY_SVC_MAX) % NOTIFY_SVC_MAX;
        if (!g_nsvc.ring[idx].dismissed) {
            out[written++] = g_nsvc.ring[idx];
        }
    }
    return written;
}

int notify_svc_dismiss_one(int notif_id) {
    if (!g_nsvc.ready) return -1;

    for (int i = 0; i < NOTIFY_SVC_MAX; i++) {
        if (g_nsvc.ring[i].id == notif_id) {
            g_nsvc.ring[i].dismissed = 1;
            return 0;
        }
    }
    return -1;
}

int notify_svc_dismiss_all_entries(void) {
    if (!g_nsvc.ready) return -1;
    for (int i = 0; i < NOTIFY_SVC_MAX; i++)
        g_nsvc.ring[i].dismissed = 1;
    return 0;
}

int notify_svc_get_count(void) {
    if (!g_nsvc.ready) return 0;
    int count = 0;
    for (int i = 0; i < g_nsvc.count; i++) {
        int idx = (g_nsvc.head - g_nsvc.count + i + NOTIFY_SVC_MAX) % NOTIFY_SVC_MAX;
        if (!g_nsvc.ring[idx].dismissed)
            count++;
    }
    return count;
}

/* Syscall entry points */
long sys_notify_svc_send(const char *title, const char *body, const char *icon) {
    struct proc *cur = current_proc();
    int pid = cur ? cur->pid : 0;
    int id = notify_svc_send(pid, title, body, icon);
    return id > 0 ? (long)id : -1;
}

long sys_notify_svc_get(struct notification *out, int max_count) {
    if (!out) return -14; /* EFAULT */
    return (long)notify_svc_get_pending(out, max_count);
}

long sys_notify_svc_dismiss(int notif_id) {
    if (notif_id == 0)
        return (long)notify_svc_dismiss_all_entries();
    return (long)notify_svc_dismiss_one(notif_id);
}
