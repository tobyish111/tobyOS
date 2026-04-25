/* service.c -- the system service manager (milestone 14, hardened in M28F).
 *
 * Three integration points:
 *
 *   - service_init() / service_register_*() are called once each from
 *     kernel.c::_start, after the relevant kernel subsystems are up.
 *
 *   - service_start_all() launches every registered PROGRAM service
 *     via the GUI launch queue. Using the launch queue means the
 *     actual proc_create runs on pid 0 from gui_tick(), which is
 *     where every other safe spawn happens too.
 *
 *   - service_tick() is pumped from gui_tick() while running on pid 0.
 *     It walks the program services, sees who died, and -- if the
 *     autorestart policy + should_restart callback agree -- re-queues
 *     them.
 *
 * We deliberately do NOT call proc_create_from_elf() directly here:
 * the launch queue path is already battle-tested by the desktop
 * launcher, gives us the same trace logs ("[gui] launched ..."), and
 * keeps us out of the IRQ-vs-proc-table concurrency corner.
 *
 * ------------------------------------------------------------------
 * Milestone 28F upgrade -- crash detection, exponential backoff, and
 * crash-loop containment:
 *
 *   * Every PROGRAM exit increments either `clean_exit_count` (rc==0)
 *     or `crash_count` (rc!=0). consecutive_crashes resets on each
 *     clean exit; this is what drives the backoff schedule and the
 *     SERVICE_DISABLED transition.
 *   * After a crash we move to SERVICE_BACKOFF and stamp
 *     backoff_until_ms = now + BASE * 2^min(consecutive-1, ceil).
 *     service_tick keeps the service in BACKOFF until the deadline
 *     elapses, then attempts to restart.
 *   * If consecutive_crashes hits SERVICE_DISABLE_THRESHOLD we move
 *     to SERVICE_DISABLED and stop auto-restarting. Operator can
 *     re-arm via service_clear() (also exposed to userland through
 *     `services clear <name>` once we bind that subcommand).
 *
 * The watchdog and crash-loop detector are deliberately implemented
 * with millisecond-resolution wall clock (perf_now_ns / 1e6) so a
 * single tick of the PIT is enough to make progress.
 */

#include <tobyos/service.h>
#include <tobyos/proc.h>
#include <tobyos/signal.h>
#include <tobyos/gui.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/perf.h>
#include <tobyos/slog.h>
#include <tobyos/notify.h>
#include <tobyos/abi/abi.h>

static struct service g_services[SERVICE_MAX];
static int            g_count;

static uint64_t now_ms(void) { return perf_now_ns() / 1000000ull; }

static void copy_capped(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static const char *state_str(enum service_state s) {
    switch (s) {
    case SERVICE_STOPPED:  return "stopped";
    case SERVICE_RUNNING:  return "running";
    case SERVICE_FAILED:   return "failed";
    case SERVICE_BACKOFF:  return "backoff";
    case SERVICE_DISABLED: return "disabled";
    }
    return "?";
}

static const char *kind_str(enum service_kind k) {
    return k == SERVICE_KIND_BUILTIN ? "builtin" : "program";
}

void service_init(void) {
    memset(g_services, 0, sizeof(g_services));
    g_count = 0;
    kprintf("[svc] service manager up (max %d services)\n", SERVICE_MAX);
    SLOG_INFO(SLOG_SUB_SVC,
              "manager up max=%d backoff_base=%ums cap=%ums disable_at=%u",
              SERVICE_MAX,
              (unsigned)SERVICE_BACKOFF_BASE_MS,
              (unsigned)SERVICE_BACKOFF_CAP_MS,
              (unsigned)SERVICE_DISABLE_THRESHOLD);
}

struct service *service_find(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_services[i].name, name) == 0) return &g_services[i];
    }
    return 0;
}

static struct service *alloc_slot(const char *name) {
    if (g_count >= SERVICE_MAX) {
        kprintf("[svc] registry full, can't add '%s'\n", name);
        return 0;
    }
    if (service_find(name)) {
        kprintf("[svc] duplicate service name '%s'\n", name);
        return 0;
    }
    struct service *s = &g_services[g_count++];
    memset(s, 0, sizeof(*s));
    copy_capped(s->name, name, SERVICE_NAME_MAX);
    return s;
}

int service_register_builtin(const char *name) {
    struct service *s = alloc_slot(name);
    if (!s) return -1;
    s->kind  = SERVICE_KIND_BUILTIN;
    s->state = SERVICE_RUNNING;          /* the kernel already brought it up */
    s->last_start_ms = now_ms();
    kprintf("[svc] registered builtin '%s'\n", s->name);
    SLOG_INFO(SLOG_SUB_SVC, "registered builtin '%s'", s->name);
    return 0;
}

int service_register_program(const char *name,
                             const char *path,
                             bool autorestart,
                             service_restart_fn should_restart) {
    if (!path || !path[0]) return -1;
    struct service *s = alloc_slot(name);
    if (!s) return -1;
    s->kind            = SERVICE_KIND_PROGRAM;
    s->state           = SERVICE_STOPPED;
    s->autorestart     = autorestart;
    s->should_restart  = should_restart;
    copy_capped(s->path, path, SERVICE_PATH_MAX);
    kprintf("[svc] registered program '%s' -> %s (autorestart=%d)\n",
            s->name, s->path, (int)autorestart);
    SLOG_INFO(SLOG_SUB_SVC,
              "registered program '%s' path='%s' autorestart=%d",
              s->name, s->path, (int)autorestart);
    return 0;
}

/* Spawn a PROGRAM service by enqueuing it on the GUI launch queue. The
 * actual proc_create runs from gui_tick() on pid 0; we won't know the
 * pid until the next tick, so we mark the service RUNNING optimistically
 * and let service_tick reconcile if the spawn fails. */
static int program_start(struct service *s) {
    if (s->state == SERVICE_RUNNING && s->pid > 0) return 0;
    if (s->state == SERVICE_DISABLED) {
        kprintf("[svc] '%s' refusing start: DISABLED (crashes=%u)\n",
                s->name, (unsigned)s->crash_count);
        SLOG_WARN(SLOG_SUB_SVC,
                  "'%s' start refused, DISABLED crashes=%u",
                  s->name, (unsigned)s->crash_count);
        return -1;
    }
    int rc = gui_launch_enqueue_arg(s->path, 0);
    if (rc != 0) {
        kprintf("[svc] '%s' enqueue FAILED (queue full?)\n", s->name);
        s->state = SERVICE_FAILED;
        SLOG_ERROR(SLOG_SUB_SVC, "'%s' enqueue failed", s->name);
        return -1;
    }
    /* The launch queue stamps a real pid in our slot below from the
     * gui_tick() drain hook -- but service_tick() won't see a pid
     * until after the next tick. Mark optimistically running. */
    s->state         = SERVICE_RUNNING;
    s->pid           = 0;
    s->restart_count++;
    s->last_start_ms = now_ms();
    kprintf("[svc] '%s' enqueued (restart #%d, crashes=%u)\n",
            s->name, s->restart_count, (unsigned)s->crash_count);
    SLOG_INFO(SLOG_SUB_SVC,
              "'%s' enqueued restart=%d crashes=%u",
              s->name, s->restart_count, (unsigned)s->crash_count);
    return 0;
}

int service_start(const char *name) {
    struct service *s = service_find(name);
    if (!s) {
        kprintf("[svc] start: unknown service '%s'\n", name);
        return -1;
    }
    if (s->kind == SERVICE_KIND_BUILTIN) {
        s->state = SERVICE_RUNNING;
        if (s->last_start_ms == 0) s->last_start_ms = now_ms();
        return 0;
    }
    return program_start(s);
}

int service_start_all(void) {
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if (service_start(g_services[i].name) == 0) n++;
    }
    kprintf("[svc] service_start_all: %d/%d started\n", n, g_count);
    SLOG_INFO(SLOG_SUB_SVC, "start_all started=%d/%d", n, g_count);
    return n;
}

int service_stop(const char *name) {
    struct service *s = service_find(name);
    if (!s) return -1;
    if (s->kind != SERVICE_KIND_PROGRAM) return 0;
    if (s->pid > 0) {
        signal_send_to_pid(s->pid, SIGTERM);
    }
    return 0;
}

int service_clear(const char *name) {
    struct service *s = service_find(name);
    if (!s) return -1;
    if (s->kind != SERVICE_KIND_PROGRAM) return 0;
    if (s->state != SERVICE_DISABLED && s->state != SERVICE_FAILED &&
        s->state != SERVICE_BACKOFF) {
        return 0;
    }
    kprintf("[svc] '%s' cleared (was %s, crashes=%u)\n",
            s->name, state_str(s->state), (unsigned)s->crash_count);
    SLOG_INFO(SLOG_SUB_SVC,
              "'%s' cleared from %s, crashes=%u",
              s->name, state_str(s->state), (unsigned)s->crash_count);
    s->consecutive_crashes = 0;
    s->backoff_until_ms    = 0;
    s->state               = SERVICE_STOPPED;
    /* M28F: service_clear() resets the supervisor's view of the
     * service. It deliberately does NOT auto-start; the operator
     * (or the regular STOPPED+autorestart path in service_tick)
     * decides when to actually try again. This prevents an instant
     * relapse into the same crash-loop that tripped DISABLED. */
    return 0;
}

/* Find the program service whose path we last enqueued, so we can
 * back-fill its `pid` field once gui's drain queue actually creates
 * the process. Linear search; SERVICE_MAX is tiny. Returns NULL if
 * the path doesn't match a registered service. */
static struct service *find_by_path(const char *path) {
    if (!path) return 0;
    for (int i = 0; i < g_count; i++) {
        if (g_services[i].kind == SERVICE_KIND_PROGRAM &&
            strcmp(g_services[i].path, path) == 0) return &g_services[i];
    }
    return 0;
}

/* Called by service_tick to scan the proc table for a recently-spawned
 * child whose name (basename of the program path) matches one of our
 * services with no pid yet. This is how a freshly-launched service
 * gets its pid populated. */
static void backfill_pids(void) {
    for (int i = 0; i < g_count; i++) {
        struct service *s = &g_services[i];
        if (s->kind != SERVICE_KIND_PROGRAM) continue;
        if (s->state != SERVICE_RUNNING) continue;
        if (s->pid > 0) continue;            /* already known */

        /* Derive basename from path. */
        const char *base = s->path;
        for (const char *c = s->path; *c; c++) if (*c == '/') base = c + 1;

        /* Walk the proc table for a live process whose name == base. */
        extern struct proc *proc_lookup(int pid);
        for (int pid = 1; pid < 64; pid++) {
            struct proc *p = proc_lookup(pid);
            if (!p) continue;
            if (p->state == PROC_TERMINATED) continue;
            if (strcmp(p->name, base) == 0) {
                s->pid = pid;
                kprintf("[svc] '%s' pid=%d (backfilled)\n", s->name, pid);
                break;
            }
        }
    }
    (void)find_by_path;   /* exposed for future direct use */
}

/* Compute the next backoff (in ms) for `consecutive` consecutive crashes.
 *  consecutive=1 -> 100   ms
 *  consecutive=2 -> 200   ms
 *  consecutive=3 -> 400   ms
 *  consecutive=4 -> 800   ms
 *  consecutive=5 -> 1600  ms (then SERVICE_DISABLED before we even wait)
 *  ...           cap at SERVICE_BACKOFF_CAP_MS (6400 ms)
 */
static uint32_t backoff_for(uint32_t consecutive) {
    if (consecutive == 0) return 0;
    uint32_t shift = consecutive - 1;
    if (shift > 6) shift = 6;
    uint32_t ms = SERVICE_BACKOFF_BASE_MS << shift;
    if (ms > SERVICE_BACKOFF_CAP_MS) ms = SERVICE_BACKOFF_CAP_MS;
    return ms;
}

/* Internal: handle the bookkeeping of an exited program service.
 * Increments counters, decides BACKOFF vs DISABLED, logs the event.
 * Does NOT itself enqueue the restart -- service_tick is responsible
 * for waking us back up after the cooldown elapses. */
static void apply_exit(struct service *s, int exit_code) {
    s->last_exit    = exit_code;
    s->last_exit_ms = now_ms();
    s->pid          = 0;

    bool crashed = (exit_code != 0);
    if (crashed) {
        s->crash_count++;
        s->consecutive_crashes++;
        s->last_crash_ms = s->last_exit_ms;
    } else {
        s->clean_exit_count++;
        s->consecutive_crashes = 0;
    }

    /* M28F: classify post-exit state. */
    if (!crashed) {
        s->state            = SERVICE_STOPPED;
        s->backoff_until_ms = 0;
        kprintf("[svc] '%s' exited cleanly rc=%d (clean_exits=%u)\n",
                s->name, exit_code, (unsigned)s->clean_exit_count);
        SLOG_INFO(SLOG_SUB_SVC,
                  "'%s' exited cleanly rc=%d clean=%u",
                  s->name, exit_code, (unsigned)s->clean_exit_count);
        return;
    }

    if (s->consecutive_crashes >= SERVICE_DISABLE_THRESHOLD) {
        s->state            = SERVICE_DISABLED;
        s->backoff_until_ms = 0;
        kprintf("[svc] '%s' DISABLED after %u consecutive crashes "
                "(rc=%d, total=%u)\n",
                s->name,
                (unsigned)s->consecutive_crashes,
                exit_code,
                (unsigned)s->crash_count);
        SLOG_ERROR(SLOG_SUB_SVC,
                   "'%s' DISABLED consecutive=%u rc=%d total=%u",
                   s->name,
                   (unsigned)s->consecutive_crashes,
                   exit_code, (unsigned)s->crash_count);
        /* M31: surface "service permanently disabled" as an error
         * toast. Crash-loop containment is the kind of event the
         * user genuinely needs to see -- the affected service is
         * gone until they explicitly clear it. */
        {
            char body[ABI_NOTIFY_BODY_MAX];
            ksnprintf(body, sizeof(body),
                      "rc=%d after %u consecutive crashes -- "
                      "auto-restart disabled.",
                      exit_code, (unsigned)s->consecutive_crashes);
            char title[ABI_NOTIFY_TITLE_MAX];
            ksnprintf(title, sizeof(title),
                      "Service '%s' disabled", s->name);
            notify_post(ABI_NOTIFY_KIND_SERVICE, ABI_NOTIFY_URG_ERR,
                        "service", title, body);
        }
        return;
    }

    uint32_t cooldown        = backoff_for(s->consecutive_crashes);
    s->backoff_until_ms      = s->last_exit_ms + cooldown;
    s->state                 = SERVICE_BACKOFF;
    kprintf("[svc] '%s' crashed rc=%d (consecutive=%u, total=%u) "
            "-> BACKOFF %u ms\n",
            s->name, exit_code,
            (unsigned)s->consecutive_crashes,
            (unsigned)s->crash_count,
            (unsigned)cooldown);
    SLOG_WARN(SLOG_SUB_SVC,
              "'%s' crashed rc=%d consecutive=%u total=%u backoff=%ums",
              s->name, exit_code,
              (unsigned)s->consecutive_crashes,
              (unsigned)s->crash_count,
              (unsigned)cooldown);
    /* M31: warn-level toast for transient crashes too -- the user
     * still gets an "auto-restarting" reassurance. We rate-limit
     * implicitly: each backoff edge is one record in the ring. */
    {
        char body[ABI_NOTIFY_BODY_MAX];
        ksnprintf(body, sizeof(body),
                  "rc=%d, restarting in %u ms (#%u)",
                  exit_code, (unsigned)cooldown,
                  (unsigned)s->consecutive_crashes);
        char title[ABI_NOTIFY_TITLE_MAX];
        ksnprintf(title, sizeof(title),
                  "Service '%s' crashed", s->name);
        notify_post(ABI_NOTIFY_KIND_SERVICE, ABI_NOTIFY_URG_WARN,
                    "service", title, body);
    }
}

void service_tick(void) {
    /* Phase 1: backfill pids for services whose program just spawned. */
    backfill_pids();

    uint64_t t = now_ms();

    /* Phase 2: detect exits + maybe restart, plus drain expired BACKOFFs. */
    for (int i = 0; i < g_count; i++) {
        struct service *s = &g_services[i];
        if (s->kind != SERVICE_KIND_PROGRAM) continue;

        /* (a) RUNNING but the proc disappeared -> handle the exit. */
        if (s->state == SERVICE_RUNNING && s->pid > 0) {
            struct proc *p = proc_lookup(s->pid);
            bool dead = !p || p->state == PROC_TERMINATED;
            if (dead) {
                int code = p ? p->exit_code : -1;
                apply_exit(s, code);
                /* Fall through to backoff/disable handling below if
                 * the policy decides to retry. */
            }
        }

        /* (b) BACKOFF -- waiting for the cooldown to elapse. */
        if (s->state == SERVICE_BACKOFF) {
            if (!s->autorestart) {
                /* No autorestart was requested; settle in STOPPED. */
                s->state = SERVICE_STOPPED;
                continue;
            }
            bool ok = s->should_restart ? s->should_restart(s) : true;
            if (!ok) {
                /* Policy says "not yet". Stay in BACKOFF; we'll be
                 * re-evaluated next tick. Don't consume the cooldown
                 * because the policy is the gating factor, not time. */
                continue;
            }
            if (t < s->backoff_until_ms) continue;
            kprintf("[svc] '%s' backoff elapsed (consecutive=%u) -> retry\n",
                    s->name, (unsigned)s->consecutive_crashes);
            SLOG_INFO(SLOG_SUB_SVC,
                      "'%s' backoff elapsed -> retry",
                      s->name);
            program_start(s);
        }

        /* (c) STOPPED + autorestart + clean exit -> regular policy
         *     path (e.g. login service comes back when the user logs
         *     out). */
        if (s->state == SERVICE_STOPPED && s->autorestart) {
            bool ok = s->should_restart ? s->should_restart(s) : true;
            if (ok) program_start(s);
        }
    }
}

void service_dump(void) {
    kprintf("[svc] %d services registered\n", g_count);
    kprintf("  %-12s %-7s %-8s %-4s %-3s %-5s %-5s %-5s %s\n",
            "name", "kind", "state", "auto", "pid",
            "rcnt", "ccnt", "ccon", "path");
    for (int i = 0; i < g_count; i++) {
        struct service *s = &g_services[i];
        kprintf("  %-12s %-7s %-8s %-4d %-3d %-5d %-5u %-5u %s\n",
                s->name, kind_str(s->kind), state_str(s->state),
                (int)s->autorestart, s->pid, s->restart_count,
                (unsigned)s->crash_count,
                (unsigned)s->consecutive_crashes,
                s->kind == SERVICE_KIND_PROGRAM ? s->path : "(builtin)");
    }
}

/* ============================================================
 *  M28F querying API: snapshot the registry into ABI-frozen
 *  records for SYS_SVC_LIST and the userland `services` tool.
 * ============================================================ */

uint32_t service_get_records(struct abi_service_info *out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    uint32_t n = (uint32_t)g_count;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++) {
        struct service          *s = &g_services[i];
        struct abi_service_info *r = &out[i];
        memset(r, 0, sizeof(*r));
        copy_capped(r->name, s->name, ABI_SVC_NAME_MAX);
        copy_capped(r->path, s->path, ABI_SVC_PATH_MAX);
        switch (s->state) {
        case SERVICE_STOPPED:  r->state = ABI_SVC_STATE_STOPPED;  break;
        case SERVICE_RUNNING:  r->state = ABI_SVC_STATE_RUNNING;  break;
        case SERVICE_FAILED:   r->state = ABI_SVC_STATE_FAILED;   break;
        case SERVICE_BACKOFF:  r->state = ABI_SVC_STATE_BACKOFF;  break;
        case SERVICE_DISABLED: r->state = ABI_SVC_STATE_DISABLED; break;
        default:               r->state = ABI_SVC_STATE_FAILED;   break;
        }
        r->kind             = (s->kind == SERVICE_KIND_BUILTIN)
                              ? ABI_SVC_KIND_BUILTIN
                              : ABI_SVC_KIND_PROGRAM;
        r->pid              = s->pid;
        r->last_exit        = s->last_exit;
        r->restart_count    = (uint32_t)s->restart_count;
        r->crash_count      = s->crash_count;
        r->last_start_ms    = s->last_start_ms;
        r->last_crash_ms    = s->last_crash_ms;
        r->backoff_until_ms = s->backoff_until_ms;
        r->autorestart      = s->autorestart ? 1u : 0u;
    }
    return n;
}

/* ============================================================
 *  M28F deterministic exit injection -- used by the in-kernel
 *  test harness to drive crash-loop scenarios without waiting
 *  for a real process to spawn and die.
 * ============================================================ */

int service_simulate_exit(struct service *s, int exit_code) {
    if (!s) return -1;
    if (s->kind != SERVICE_KIND_PROGRAM) return -1;
    /* Stamp a synthetic "we were running" pid so apply_exit's bookkeeping
     * mirrors the real path. The pid value itself is meaningless. */
    if (s->pid == 0) s->pid = -1;
    apply_exit(s, exit_code);
    return 0;
}
