/* svc_mgr.c -- Kernel-side service registry for tobyOS.
 *
 * Tracks registered services, manages dependency ordering via
 * topological sort, handles process exits and auto-restart with
 * exponential backoff.
 */

#include <tobyos/svc_mgr.h>
#include <tobyos/klibc.h>
#include <tobyos/proc.h>

/* Forward declarations for kernel facilities */
extern uint32_t pit_ticks;
extern void signal_send_to_pid(int pid, int sig);
extern void kprintf(const char *fmt, ...);
#define printk kprintf

static int spawn_service_path(const char *path)
{
    struct proc_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.path = path;
    return proc_spawn(&spec);
}

/* ---- Global state ---- */

static struct service g_services[SVC_MAX];
static int g_svc_count;
static int g_initialized;

/* ---- Helpers ---- */

static struct service *find_by_name(const char *name)
{
    for (int i = 0; i < g_svc_count; i++) {
        if (strcmp(g_services[i].name, name) == 0)
            return &g_services[i];
    }
    return 0;
}

static struct service *find_by_pid(int pid)
{
    for (int i = 0; i < g_svc_count; i++) {
        if (g_services[i].pid == pid && g_services[i].state == SVC_RUNNING)
            return &g_services[i];
    }
    return 0;
}

static uint32_t now_ms(void)
{
    return pit_ticks * 10;
}

/* ---- Init ---- */

void svc_mgr_init(void)
{
    memset(g_services, 0, sizeof(g_services));
    g_svc_count = 0;
    g_initialized = 1;
    printk("[svc_mgr] initialized\n");
}

/* ---- Registration ---- */

int svc_register(const char *name, const char *path, int auto_restart)
{
    if (!g_initialized) return -1;
    if (g_svc_count >= SVC_MAX) return -1;
    if (!name || !path) return -1;
    if (find_by_name(name)) return -1;

    struct service *svc = &g_services[g_svc_count++];
    memset(svc, 0, sizeof(*svc));

    size_t nlen = strlen(name);
    if (nlen >= SVC_NAME_MAX) nlen = SVC_NAME_MAX - 1;
    memcpy(svc->name, name, nlen);

    size_t plen = strlen(path);
    if (plen >= SVC_PATH_MAX) plen = SVC_PATH_MAX - 1;
    memcpy(svc->path, path, plen);

    svc->auto_restart = auto_restart;
    svc->state = SVC_STOPPED;
    svc->backoff_ms = 1000;
    return 0;
}

int svc_add_dep(const char *service, const char *depends_on)
{
    struct service *svc = find_by_name(service);
    if (!svc) return -1;
    if (svc->dep_count >= SVC_DEP_MAX) return -1;

    size_t dlen = strlen(depends_on);
    if (dlen >= SVC_NAME_MAX) dlen = SVC_NAME_MAX - 1;
    memcpy(svc->deps[svc->dep_count], depends_on, dlen);
    svc->dep_count++;
    return 0;
}

/* ---- Topological sort for start order ---- */

static int g_topo_order[SVC_MAX];
static int g_topo_count;
static int g_topo_visited[SVC_MAX];

static int svc_index(const char *name)
{
    for (int i = 0; i < g_svc_count; i++) {
        if (strcmp(g_services[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void topo_visit(int idx)
{
    if (g_topo_visited[idx]) return;
    g_topo_visited[idx] = 1;

    struct service *svc = &g_services[idx];
    for (int d = 0; d < svc->dep_count; d++) {
        int dep_idx = svc_index(svc->deps[d]);
        if (dep_idx >= 0) topo_visit(dep_idx);
    }

    g_topo_order[g_topo_count++] = idx;
}

static void compute_start_order(void)
{
    g_topo_count = 0;
    memset(g_topo_visited, 0, sizeof(g_topo_visited));
    for (int i = 0; i < g_svc_count; i++) {
        topo_visit(i);
    }
}

/* ---- Start / Stop ---- */

static int do_start(struct service *svc)
{
    if (svc->state == SVC_RUNNING || svc->state == SVC_STARTING)
        return 0;

    /* Check that dependencies are running */
    for (int d = 0; d < svc->dep_count; d++) {
        struct service *dep = find_by_name(svc->deps[d]);
        if (dep && dep->state != SVC_RUNNING) {
            printk("[svc_mgr] %s: dependency '%s' not running\n",
                   svc->name, svc->deps[d]);
            return -1;
        }
    }

    svc->state = SVC_STARTING;
    int pid = spawn_service_path(svc->path);
    if (pid <= 0) {
        svc->state = SVC_CRASHED;
        printk("[svc_mgr] failed to start '%s'\n", svc->name);
        return -1;
    }

    svc->pid = pid;
    svc->state = SVC_RUNNING;
    svc->start_time = now_ms();
    printk("[svc_mgr] started '%s' pid=%d\n", svc->name, pid);
    return 0;
}

int svc_start(const char *name)
{
    struct service *svc = find_by_name(name);
    if (!svc) return -1;
    return do_start(svc);
}

int svc_stop(const char *name)
{
    struct service *svc = find_by_name(name);
    if (!svc) return -1;
    if (svc->state != SVC_RUNNING) return -1;

    svc->state = SVC_STOPPING;
    signal_send_to_pid(svc->pid, 15); /* SIGTERM */
    svc->state = SVC_STOPPED;
    svc->pid = 0;
    printk("[svc_mgr] stopped '%s'\n", svc->name);
    return 0;
}

int svc_restart(const char *name)
{
    svc_stop(name);
    return svc_start(name);
}

enum svc_state svc_get_state(const char *name)
{
    struct service *svc = find_by_name(name);
    if (!svc) return SVC_STOPPED;
    return svc->state;
}

/* ---- Start all respecting dependency order ---- */

int svc_start_all(void)
{
    compute_start_order();
    int started = 0;
    for (int i = 0; i < g_topo_count; i++) {
        struct service *svc = &g_services[g_topo_order[i]];
        if (svc->state == SVC_STOPPED) {
            if (do_start(svc) == 0)
                started++;
        }
    }
    printk("[svc_mgr] started %d/%d services\n", started, g_svc_count);
    return started;
}

/* ---- Handle process exit ---- */

void svc_handle_exit(int pid, int status)
{
    struct service *svc = find_by_pid(pid);
    if (!svc) return;

    svc->pid = 0;

    if (status == 0) {
        svc->state = SVC_STOPPED;
        svc->restart_count = 0;
        svc->backoff_ms = 1000;
        printk("[svc_mgr] '%s' exited cleanly\n", svc->name);
        return;
    }

    svc->state = SVC_CRASHED;
    svc->restart_count++;
    printk("[svc_mgr] '%s' crashed (status=%d, count=%d)\n",
           svc->name, status, svc->restart_count);

    if (!svc->auto_restart) return;
    if (svc->restart_count > 10) {
        printk("[svc_mgr] '%s' restart limit reached, giving up\n",
               svc->name);
        return;
    }

    /* Exponential backoff: 1s, 2s, 4s, 8s, 16s, max 30s */
    uint32_t delay = svc->backoff_ms;
    svc->backoff_ms *= 2;
    if (svc->backoff_ms > 30000) svc->backoff_ms = 30000;
    svc->last_restart = now_ms() + delay;

    printk("[svc_mgr] will restart '%s' in %u ms\n", svc->name, delay);
    do_start(svc);
}

/* ---- List services (for userland query) ---- */

int svc_list(struct service *out, int max)
{
    int n = g_svc_count < max ? g_svc_count : max;
    memcpy(out, g_services, (size_t)n * sizeof(struct service));
    return n;
}
