/* uac.c -- User Account Control (privilege escalation) for tobyOS.
 *
 * When a non-root process tries a privileged operation (install app,
 * modify system, change user, driver install), we suspend it, post a
 * desktop notification, and wait for user confirmation (password
 * entry).  Elevated PIDs are tracked in a kernel table with a
 * 5-minute timeout.
 */

#include <tobyos/uac.h>
#include <tobyos/proc.h>
#include <tobyos/user.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>
#include <tobyos/notify.h>
#include <tobyos/pit.h>

#define TAG "uac"

static struct uac_entry g_entries[UAC_MAX_ELEVATED];
static int g_count;
static bool g_initialised;

void uac_init(void) {
    memset(g_entries, 0, sizeof(g_entries));
    g_count = 0;
    g_initialised = true;
    SLOG_INFO(TAG, "UAC subsystem initialised (%d slots)", UAC_MAX_ELEVATED);
}

static struct uac_entry *find_entry(int pid) {
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].pid == pid) return &g_entries[i];
    }
    return NULL;
}

int uac_check_can_elevate(int uid) {
    struct user_info info;
    if (user_get_info(uid, &info) < 0) return 0;
    return (info.flags & USER_FLAG_ADMIN) ? 1 : 0;
}

const char *uac_reason_str(enum uac_reason reason) {
    switch (reason) {
    case UAC_INSTALL_APP:    return "install an application";
    case UAC_MODIFY_SYSTEM:  return "modify system settings";
    case UAC_CHANGE_USER:    return "change user account";
    case UAC_DRIVER_INSTALL: return "install a driver";
    }
    return "perform a privileged operation";
}

int uac_request_elevation(int pid, enum uac_reason reason) {
    if (!g_initialised) return -1;

    struct proc *p = proc_lookup(pid);
    if (!p) return -1;

    /* Already root -- no elevation needed */
    if (p->uid == 0) return 0;

    /* Already elevated */
    struct uac_entry *existing = find_entry(pid);
    if (existing && existing->state == UAC_STATE_ELEVATED) return 0;

    /* Check if user can elevate (must be admin) */
    if (!uac_check_can_elevate(p->uid)) {
        SLOG_WARN(TAG, "pid %d (uid %d) cannot elevate: not admin", pid, p->uid);
        return -1;
    }

    /* Allocate entry */
    if (g_count >= UAC_MAX_ELEVATED) {
        SLOG_ERROR(TAG, "UAC table full, cannot elevate pid %d", pid);
        return -1;
    }

    struct uac_entry *e;
    if (existing) {
        e = existing;
    } else {
        e = &g_entries[g_count++];
    }

    e->pid    = pid;
    e->uid    = p->uid;
    e->reason = reason;
    e->state  = UAC_STATE_PENDING;
    e->elevate_time_ms = 0;
    e->expire_time_ms  = 0;

    /* Post notification */
    char msg[128];
    ksnprintf(msg, sizeof(msg), "Process %d wants to %s",
              pid, uac_reason_str(reason));
    notify_post(ABI_NOTIFY_KIND_SYSTEM, NOTIFY_URG_WARN,
                "UAC", "Elevation Request", msg);

    SLOG_INFO(TAG, "elevation requested: pid %d reason '%s'",
              pid, uac_reason_str(reason));

    return 1;
}

void uac_grant(int pid) {
    struct uac_entry *e = find_entry(pid);
    if (!e || e->state != UAC_STATE_PENDING) return;

    e->state = UAC_STATE_ELEVATED;
    uint64_t hz = pit_hz();
    e->elevate_time_ms = (hz > 0) ? (pit_ticks() * 1000 / hz) : 0;
    e->expire_time_ms  = e->elevate_time_ms + UAC_TIMEOUT_MS;

    SLOG_INFO(TAG, "elevation GRANTED for pid %d (expires in 5 min)", pid);
}

void uac_deny(int pid) {
    struct uac_entry *e = find_entry(pid);
    if (!e || e->state != UAC_STATE_PENDING) return;

    e->state = UAC_STATE_DENIED;
    SLOG_INFO(TAG, "elevation DENIED for pid %d", pid);
}

int uac_is_elevated(int pid) {
    struct uac_entry *e = find_entry(pid);
    if (!e) return 0;
    return (e->state == UAC_STATE_ELEVATED) ? 1 : 0;
}

void uac_drop_elevation(int pid) {
    struct uac_entry *e = find_entry(pid);
    if (!e) return;

    e->state = UAC_STATE_NONE;
    SLOG_INFO(TAG, "elevation dropped for pid %d", pid);
}

void uac_tick(uint64_t now_ms) {
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].state == UAC_STATE_ELEVATED &&
            now_ms >= g_entries[i].expire_time_ms) {
            g_entries[i].state = UAC_STATE_EXPIRED;
            SLOG_WARN(TAG, "elevation expired for pid %d", g_entries[i].pid);
        }
    }
}

int uac_pending_count(void) {
    int count = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].state == UAC_STATE_PENDING) count++;
    }
    return count;
}

const struct uac_entry *uac_get_pending(int idx) {
    int count = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].state == UAC_STATE_PENDING) {
            if (count == idx) return &g_entries[i];
            count++;
        }
    }
    return NULL;
}
