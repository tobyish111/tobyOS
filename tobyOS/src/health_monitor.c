/* health_monitor.c -- background system health checks. */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/health_monitor.h>
#include <tobyos/spinlock.h>
#include <tobyos/vfs.h>
#include <tobyos/perf.h>

int  notify_svc_send(int pid, const char *title, const char *body, const char *icon);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);

static inline uint64_t hm_now_ms(void) { return perf_now_ns() / 1000000ULL; }

static void hm_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static struct health_status g_status;
static spinlock_t g_health_lock = SPINLOCK_INIT;
static uint64_t g_last_tick_ms;
static int g_disk_warned;
static int g_mem_warned;

#define HEALTH_TICK_INTERVAL_MS  60000

struct svc_crash_tracker {
    char name[32];
    int  crash_count;
    int  disabled;
};

#define SVC_TRACK_MAX 16
static struct svc_crash_tracker g_svc_crashes[SVC_TRACK_MAX];
static int g_svc_crash_count;

static void health_log(const char *msg) {
    char buf[288];
    uint32_t ts = (uint32_t)(hm_now_ms() / 1000);
    int len = ksnprintf(buf, sizeof(buf), "[%u] %s\n", ts, msg);
    if (len > 0)
        vfs_write_all(HEALTH_LOG_PATH, buf, len);
}

void health_check_tick(void) {
    uint64_t now = hm_now_ms();
    if (g_last_tick_ms != 0 && (now - g_last_tick_ms) < HEALTH_TICK_INTERVAL_MS)
        return;
    g_last_tick_ms = now;

    uint64_t flags = spin_lock_irqsave(&g_health_lock);

    uint64_t total = pmm_total_pages();
    uint64_t free_p = pmm_free_pages();
    if (total > 0)
        g_status.mem_percent_used = (int)((total - free_p) * 100 / total);
    g_status.uptime_seconds = (uint32_t)(now / 1000);
    g_status.disk_percent_used = 25;

    g_status.services_healthy = 0;
    g_status.services_failed = 0;
    for (int i = 0; i < g_svc_crash_count; i++) {
        if (g_svc_crashes[i].disabled) g_status.services_failed++;
        else g_status.services_healthy++;
    }
    g_status.warnings_active = 0;

    if (g_status.mem_percent_used >= HEALTH_MEM_WARN_PERCENT) {
        g_status.warnings_active++;
        if (!g_mem_warned) {
            g_mem_warned = 1;
            spin_unlock_irqrestore(&g_health_lock, flags);
            char msg[64];
            ksnprintf(msg, sizeof(msg), "Memory usage at %d%%", g_status.mem_percent_used);
            health_log(msg);
            notify_svc_send(-1, "High Memory Usage", msg, "warning");
            kprintf("[health] WARNING: %s\n", msg);
            return;
        }
    } else {
        g_mem_warned = 0;
    }

    if (g_status.disk_percent_used >= HEALTH_DISK_WARN_PERCENT) {
        g_status.warnings_active++;
        if (!g_disk_warned) {
            g_disk_warned = 1;
            spin_unlock_irqrestore(&g_health_lock, flags);
            char msg[64];
            ksnprintf(msg, sizeof(msg), "Disk usage at %d%%", g_status.disk_percent_used);
            health_log(msg);
            notify_svc_send(-1, "Low Disk Space", msg, "warning");
            kprintf("[health] WARNING: %s\n", msg);
            return;
        }
    } else {
        g_disk_warned = 0;
    }

    spin_unlock_irqrestore(&g_health_lock, flags);
}

int health_get_status(struct health_status *out) {
    if (!out) return -1;
    uint64_t flags = spin_lock_irqsave(&g_health_lock);
    memcpy(out, &g_status, sizeof(*out));
    spin_unlock_irqrestore(&g_health_lock, flags);
    return 0;
}

void health_report_svc_crash(const char *name) {
    if (!name) return;
    uint64_t flags = spin_lock_irqsave(&g_health_lock);
    int idx = -1;
    for (int i = 0; i < g_svc_crash_count; i++) {
        if (strcmp(g_svc_crashes[i].name, name) == 0) { idx = i; break; }
    }
    if (idx < 0 && g_svc_crash_count < SVC_TRACK_MAX) {
        idx = g_svc_crash_count++;
        memset(&g_svc_crashes[idx], 0, sizeof(g_svc_crashes[idx]));
        hm_copy(g_svc_crashes[idx].name, sizeof(g_svc_crashes[idx].name), name);
    }
    if (idx >= 0) {
        g_svc_crashes[idx].crash_count++;
        if (g_svc_crashes[idx].crash_count >= HEALTH_SVC_CRASH_LIMIT &&
            !g_svc_crashes[idx].disabled) {
            g_svc_crashes[idx].disabled = 1;
            spin_unlock_irqrestore(&g_health_lock, flags);
            char msg[128];
            ksnprintf(msg, sizeof(msg), "Service '%s' crashed %d times, disabled",
                      name, g_svc_crashes[idx].crash_count);
            health_log(msg);
            notify_svc_send(-1, "Service Disabled", msg, "error");
            kprintf("[health] %s\n", msg);
            return;
        }
    }
    spin_unlock_irqrestore(&g_health_lock, flags);
}

void health_monitor_init(void) {
    memset(&g_status, 0, sizeof(g_status));
    memset(g_svc_crashes, 0, sizeof(g_svc_crashes));
    g_svc_crash_count = 0;
    g_last_tick_ms = 0;
    g_disk_warned = 0;
    g_mem_warned = 0;
    kprintf("[health] monitor initialized\n");
}
