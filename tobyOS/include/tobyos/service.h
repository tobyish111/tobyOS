/* service.h -- minimal system service manager (milestone 14).
 *
 * A "service" is a named, long-running component that the system wants
 * to keep alive. tobyOS supports two flavours:
 *
 *   SERVICE_KIND_BUILTIN   the implementation lives entirely inside
 *                          the kernel (e.g. networking, input, the
 *                          desktop compositor). Once the kernel finishes
 *                          its corresponding init function the service
 *                          is marked SERVICE_RUNNING; we never restart
 *                          it. These exist so the user can `services`
 *                          from the shell and see "yes, networking is
 *                          up" the same way they'd check userspace
 *                          daemons on a real OS.
 *
 *   SERVICE_KIND_PROGRAM   the implementation is an ELF on the VFS that
 *                          the service manager spawns via the GUI
 *                          launch queue (so the actual proc_create runs
 *                          on pid 0 -- safe). The service is
 *                          considered RUNNING while its pid is alive.
 *                          When it exits we OPTIONALLY restart it
 *                          (autorestart flag).
 *
 * Restart policy uses a callback (`should_restart`) so callers can
 * gate restarts on external state -- the login service uses this to
 * say "only restart me when no user session is active". This keeps
 * service_tick() oblivious to the session machinery.
 *
 * The whole module fits in <300 lines of C; it intentionally has no
 * dependency graph, no priorities, no pid 1 init dance. Boot order is
 * just "the order they were registered".
 */

#ifndef TOBYOS_SERVICE_H
#define TOBYOS_SERVICE_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>      /* abi_service_info for M28F querying */

#define SERVICE_NAME_MAX  16
#define SERVICE_PATH_MAX  64
#define SERVICE_MAX       8

enum service_kind {
    SERVICE_KIND_BUILTIN = 0,
    SERVICE_KIND_PROGRAM = 1,
};

/* Service lifecycle. STOPPED/RUNNING/FAILED are the original M14 set;
 * BACKOFF and DISABLED are M28F additions for crash-loop containment.
 *   BACKOFF  -- the service exited non-zero recently and we are
 *               waiting `backoff_until_ms` before re-enqueueing it.
 *   DISABLED -- the service has crashed too many times in too short a
 *               window; the supervisor refuses to restart it until an
 *               operator clears it (service_clear / service_start). */
enum service_state {
    SERVICE_STOPPED  = 0,
    SERVICE_RUNNING  = 1,
    SERVICE_FAILED   = 2,
    SERVICE_BACKOFF  = 3,
    SERVICE_DISABLED = 4,
};

struct service;

/* Restart-policy hook. Returns true if service_tick should restart
 * `s` after it exits. NULL means "always restart if autorestart is on,
 * never if not". */
typedef bool (*service_restart_fn)(struct service *s);

struct service {
    char               name[SERVICE_NAME_MAX];
    enum service_kind  kind;
    enum service_state state;
    char               path[SERVICE_PATH_MAX];   /* PROGRAM only */
    int                pid;                       /* PROGRAM only; 0 = none */
    int                last_exit;                  /* last observed exit code */
    bool               autorestart;
    service_restart_fn should_restart;
    int                restart_count;       /* total launches incl. retries */
    /* ---- Milestone 28F additions --------------------------------- */
    uint32_t           crash_count;         /* lifetime non-zero exits     */
    uint32_t           clean_exit_count;    /* lifetime zero-status exits  */
    uint64_t           last_start_ms;       /* perf clock at last enqueue  */
    uint64_t           last_crash_ms;       /* perf clock at last crash    */
    uint64_t           last_exit_ms;        /* perf clock at last exit     */
    uint64_t           backoff_until_ms;    /* in BACKOFF until this stamp */
    uint32_t           consecutive_crashes; /* reset on every clean exit   */
};

/* Boot-time entry: zero the registry. Safe to call multiple times. */
void service_init(void);

/* Register a built-in service. The kernel subsystem itself is already
 * up by the time this is called; we just record the name + RUNNING
 * state so it shows up in `services`. Returns 0 on success, -1 on
 * registry full / duplicate name. */
int service_register_builtin(const char *name);

/* Register a userspace program service. The exec is NOT performed
 * here -- call service_start_all() (or service_start) when ready.
 * `should_restart` may be NULL. Returns 0 on success, -1 on failure. */
int service_register_program(const char *name,
                             const char *path,
                             bool autorestart,
                             service_restart_fn should_restart);

/* Start a single service by name. For BUILTIN this is a no-op (it's
 * already running). For PROGRAM, enqueues the program on the GUI
 * launch queue. Returns 0 on success, -1 if the name is unknown. */
int service_start(const char *name);

/* Start every registered service in registration order. Convenience
 * for boot. Returns the number that started successfully. */
int service_start_all(void);

/* Stop a PROGRAM service (SIGTERMs the pid; the next service_tick
 * will reap it and -- if autorestart is on and should_restart
 * agrees -- relaunch it). No-op for BUILTIN. */
int service_stop(const char *name);

/* Called from gui_tick() on pid 0. Reaps terminated PROGRAM services
 * and restarts those whose autorestart says so. Cheap when nothing's
 * dying. */
void service_tick(void);

/* Diagnostics: print the registry to the kernel log. Safe to call from
 * a shell command. */
void service_dump(void);

/* Lookup helper used by the session module so it can see whether
 * /bin/login is currently up. */
struct service *service_find(const char *name);

/* ---- Milestone 28F: supervision + introspection ---------------- */

/* Snapshot the whole registry into the caller's buffer. Returns the
 * number of records written (<= cap). The records are ABI-frozen
 * (struct abi_service_info), so this is what the SVC_LIST syscall
 * forwards directly. */
uint32_t service_get_records(struct abi_service_info *out, uint32_t cap);

/* Test-only synchronous exit injection. Bypasses the proc table and
 * pretends `s` just exited with `exit_code`. Drives the same crash-
 * count / backoff / DISABLED transitions a real exit would, with no
 * dependency on the launch queue. Returns 0 always. Used by the
 * M28F harness to deterministically force crash-loop scenarios. */
int service_simulate_exit(struct service *s, int exit_code);

/* Re-enable a previously DISABLED service. Resets the crash counter
 * and backoff state and (optionally) starts it. Used by `services
 * clear <name>` and the safe-mode shell. Returns 0 on success. */
int service_clear(const char *name);

/* M28F backoff schedule (exposed for tests + diagnostics). */
#define SERVICE_BACKOFF_BASE_MS    100u   /* first cooldown               */
#define SERVICE_BACKOFF_CAP_MS    6400u   /* longest cooldown ever applied */
#define SERVICE_DISABLE_THRESHOLD    5u   /* crashes-in-a-row before STOP */

#endif /* TOBYOS_SERVICE_H */
