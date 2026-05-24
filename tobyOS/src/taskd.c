/* taskd.c -- Cron-like kernel task scheduler.
 *
 * Maintains a table of scheduled tasks that are checked on every
 * timer tick. When a task is due, its program is spawned.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>
#include <tobyos/proc.h>
#include <tobyos/slog.h>

#define TASKD_MAX_TASKS   32
#define TASKD_PATH_MAX    128

struct taskd_entry {
    bool     active;
    bool     enabled;
    char     path[TASKD_PATH_MAX];
    uint32_t interval_sec;
    uint64_t next_run_ms;
    uint64_t last_run_ms;
    uint32_t run_count;
};

static struct taskd_entry g_tasks[TASKD_MAX_TASKS];
static bool g_initialized = false;

/* Forward declaration */
int taskd_add(const char *path, uint32_t interval_sec);

static uint64_t taskd_now_ms(void) {
    uint32_t hz = pit_hz();
    if (!hz) return 0;
    return (pit_ticks() * 1000ULL) / hz;
}

void taskd_init(void) {
    if (g_initialized) return;
    memset(g_tasks, 0, sizeof(g_tasks));

    /* Default tasks */
    taskd_add("/bin/logrotate", 3600);   /* log rotation every hour */
    taskd_add("/bin/healthcheck", 300);  /* system health check every 5 min */

    g_initialized = true;
    kprintf("[taskd] scheduler initialized (%d slots)\n", TASKD_MAX_TASKS);
    SLOG_INFO("taskd", "task scheduler ready (max=%d)", TASKD_MAX_TASKS);
}

int taskd_add(const char *path, uint32_t interval_sec) {
    if (!path || interval_sec == 0) return -1;

    int slot = -1;
    for (int i = 0; i < TASKD_MAX_TASKS; i++) {
        if (!g_tasks[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    struct taskd_entry *t = &g_tasks[slot];
    t->active = true;
    t->enabled = true;
    size_t len = strlen(path);
    if (len >= TASKD_PATH_MAX) len = TASKD_PATH_MAX - 1;
    memcpy(t->path, path, len);
    t->path[len] = '\0';
    t->interval_sec = interval_sec;
    t->next_run_ms = taskd_now_ms() + (uint64_t)interval_sec * 1000;
    t->last_run_ms = 0;
    t->run_count = 0;

    kprintf("[taskd] added task %d: '%s' every %u sec\n",
            slot, path, (unsigned)interval_sec);
    return slot;
}

int taskd_remove(int id) {
    if (id < 0 || id >= TASKD_MAX_TASKS) return -1;
    if (!g_tasks[id].active) return -1;

    g_tasks[id].active = false;
    kprintf("[taskd] removed task %d\n", id);
    return 0;
}

void taskd_list(void) {
    kprintf("[taskd] scheduled tasks:\n");
    kprintf("  ID  ENABLED  INTERVAL  RUNS  PATH\n");
    for (int i = 0; i < TASKD_MAX_TASKS; i++) {
        if (!g_tasks[i].active) continue;
        kprintf("  %2d  %-7s  %5us    %4u  %s\n",
                i,
                g_tasks[i].enabled ? "yes" : "no",
                (unsigned)g_tasks[i].interval_sec,
                (unsigned)g_tasks[i].run_count,
                g_tasks[i].path);
    }
}

void taskd_tick(void) {
    if (!g_initialized) return;

    uint64_t now = taskd_now_ms();
    if (now == 0) return;

    for (int i = 0; i < TASKD_MAX_TASKS; i++) {
        struct taskd_entry *t = &g_tasks[i];
        if (!t->active || !t->enabled) continue;
        if (now < t->next_run_ms) continue;

        /* Task is due -- spawn it */
        char *argv[] = { t->path, NULL };
        char *envp[] = { (char *)"PATH=/bin", NULL };
        struct proc_spec spec = {
            .path = t->path,
            .name = t->path,
            .argc = 1,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid >= 0) {
            SLOG_DEBUG("taskd", "spawned task %d '%s' (pid=%d)",
                       i, t->path, pid);
        } else {
            SLOG_WARN("taskd", "failed to spawn task %d '%s' (rc=%d)",
                      i, t->path, pid);
        }

        t->last_run_ms = now;
        t->next_run_ms = now + (uint64_t)t->interval_sec * 1000;
        t->run_count++;
    }
}

void taskd_enable(int id, bool enable) {
    if (id < 0 || id >= TASKD_MAX_TASKS) return;
    if (!g_tasks[id].active) return;
    g_tasks[id].enabled = enable;
}
