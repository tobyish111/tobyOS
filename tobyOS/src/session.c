/* session.c -- single-user session lifecycle (milestone 14).
 *
 * Tracks one global "currently logged in" record:
 *
 *     g.id        monotonically increasing counter; 0 means "no
 *                 session". Every SUCCESSFUL session_login bumps it
 *                 so old session-tagged children that survived a
 *                 logout (shouldn't happen, but defense in depth)
 *                 don't accidentally get reattached to the new
 *                 session.
 *     g.active    true between login() and logout().
 *     g.username  the name the user typed at the login screen.
 *
 * Process tagging is done by struct proc.session_id, populated in
 * proc.c::spawn_internal from current_proc()->session_id. The desktop
 * launcher in gui.c temporarily flips pid 0's session_id to g.id
 * around the spawn so apps launched from the desktop end up tagged
 * even though pid 0 itself is session-less.
 *
 * Logout walks the whole PCB table (PROC_MAX is 16) and SIGTERMs every
 * matching live process. We use SIGTERM rather than SIGINT so the
 * intent is clearly "session ending, please tear down" rather than
 * the more interactive Ctrl+C semantics.
 */

#include <tobyos/session.h>
#include <tobyos/service.h>
#include <tobyos/settings.h>
#include <tobyos/proc.h>
#include <tobyos/signal.h>
#include <tobyos/users.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/notify.h>
#include <tobyos/slog.h>
#include <tobyos/pit.h>
#include <tobyos/abi/abi.h>

#define LOGIN_SERVICE_NAME "login"
#define LOGIN_PROGRAM_PATH "/bin/login"

static struct {
    int  id;
    bool active;
    char username[SESSION_USER_MAX];
    int  uid;
    int  gid;
} g;

static void copy_capped(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ---- login rate-limiting / account lockout ----
 *
 * Per-username failed-attempt throttle. After LOGIN_MAX_FAILS consecutive
 * bad attempts the account is locked for LOGIN_LOCKOUT_MS; further attempts
 * are rejected without even running the (deliberately expensive) Argon2
 * verify, which both rate-limits online brute force and avoids burning CPU.
 *
 * Failures are keyed on the *typed* username, so unknown users are throttled
 * too -- this bounds username-enumeration probing and keeps the table from
 * being a free oracle. The counter resets if the previous failure was longer
 * than LOGIN_FAIL_WINDOW_MS ago (so an occasional typo doesn't accumulate
 * toward a lockout), and is cleared entirely on a successful login.
 *
 * The table is fixed-size; when full we evict the least-recently-touched
 * slot. Time comes from the monotonic PIT tick (unaffected by RTC/wall-clock
 * changes). */

#define LOGIN_MAX_FAILS       5
#define LOGIN_LOCKOUT_MS      30000ULL   /* lock for 30 s after MAX_FAILS */
#define LOGIN_FAIL_WINDOW_MS  30000ULL   /* forget stale failures after 30 s */
#define LOGIN_THROTTLE_SLOTS  (USER_MAX + 4)

static struct login_throttle {
    char     name[SESSION_USER_MAX];
    int      fails;
    uint64_t last_touch_ms;     /* last attempt (for LRU eviction)      */
    uint64_t lockout_until_ms;  /* 0 == not locked                      */
} g_throttle[LOGIN_THROTTLE_SLOTS];

static uint64_t login_now_ms(void) {
    uint32_t hz = pit_hz();
    if (!hz) return 0;
    return pit_ticks() * 1000ULL / hz;
}

static struct login_throttle *throttle_find(const char *name) {
    for (int i = 0; i < LOGIN_THROTTLE_SLOTS; i++) {
        if (g_throttle[i].name[0] && strcmp(g_throttle[i].name, name) == 0)
            return &g_throttle[i];
    }
    return 0;
}

/* Find the slot for `name`, allocating one (empty slot, else LRU eviction)
 * if it does not already exist. */
static struct login_throttle *throttle_get(const char *name, uint64_t now) {
    struct login_throttle *t = throttle_find(name);
    if (t) return t;

    struct login_throttle *victim = &g_throttle[0];
    for (int i = 0; i < LOGIN_THROTTLE_SLOTS; i++) {
        if (!g_throttle[i].name[0]) { victim = &g_throttle[i]; break; }
        if (g_throttle[i].last_touch_ms < victim->last_touch_ms)
            victim = &g_throttle[i];
    }
    memset(victim, 0, sizeof(*victim));
    copy_capped(victim->name, name, SESSION_USER_MAX);
    victim->last_touch_ms = now;
    return victim;
}

/* If `name` is currently locked out, return the remaining seconds (>=1);
 * otherwise return 0. Expired lockouts are cleared as a side effect. */
static unsigned throttle_locked_secs(const char *name, uint64_t now) {
    struct login_throttle *t = throttle_find(name);
    if (!t || t->lockout_until_ms == 0) return 0;
    if (now < t->lockout_until_ms) {
        uint64_t rem = t->lockout_until_ms - now;
        return (unsigned)((rem + 999) / 1000);
    }
    /* Lockout window has elapsed -- start the user fresh. */
    t->lockout_until_ms = 0;
    t->fails = 0;
    return 0;
}

static void throttle_record_fail(const char *name, uint64_t now) {
    struct login_throttle *t = throttle_get(name, now);
    if (!t) return;
    if (t->last_touch_ms && now - t->last_touch_ms > LOGIN_FAIL_WINDOW_MS)
        t->fails = 0;   /* stale: forget the old streak */
    t->fails++;
    t->last_touch_ms = now;
    if (t->fails >= LOGIN_MAX_FAILS && t->lockout_until_ms == 0) {
        t->lockout_until_ms = now + LOGIN_LOCKOUT_MS;
        kprintf("[session] '%s' locked out for %llus after %d failed attempts\n",
                name, (unsigned long long)(LOGIN_LOCKOUT_MS / 1000), t->fails);
        SLOG_WARN(SLOG_SUB_AUDIT,
                  "login LOCKOUT user='%s' fails=%d lock_ms=%llu",
                  name, t->fails, (unsigned long long)LOGIN_LOCKOUT_MS);
    }
}

static void throttle_clear(const char *name) {
    struct login_throttle *t = throttle_find(name);
    if (t) memset(t, 0, sizeof(*t));
}

int session_current_id(void) { return g.active ? g.id : 0; }
bool session_active(void)    { return g.active; }
int  session_current_uid(void) { return g.active ? g.uid : 0; }
int  session_current_gid(void) { return g.active ? g.gid : 0; }

void session_get_info(struct session_info *out) {
    if (!out) return;
    out->id     = g.id;
    out->active = g.active;
    out->uid    = g.active ? g.uid : 0;
    out->gid    = g.active ? g.gid : 0;
    copy_capped(out->username, g.username, SESSION_USER_MAX);
}

bool session_should_restart_login(struct service *s) {
    (void)s;
    return !g.active;
}

int session_login(const char *username, const char *password) {
    if (!username || !username[0]) {
        kprintf("[session] login refused: empty username\n");
        SLOG_WARN(SLOG_SUB_AUDIT,
                  "login REJECT reason=empty-username");
        return -1;
    }

    /* Rate-limit before doing any (expensive) credential work. */
    uint64_t now = login_now_ms();
    unsigned locked = throttle_locked_secs(username, now);
    if (locked) {
        kprintf("[session] login refused: '%s' locked out (%us remaining)\n",
                username, locked);
        SLOG_WARN(SLOG_SUB_AUDIT,
                  "login REJECT user='%s' reason=locked-out remaining_s=%u",
                  username, locked);
        return -1;
    }

    /* Validate against the on-disk users database. Reject unknown names
     * outright. Unknown-user rejections still count toward the lockout so
     * the throttle also blunts username-enumeration probing. */
    const struct user *u = users_lookup_by_name(username);
    if (!u) {
        kprintf("[session] login refused: unknown user '%s'\n", username);
        SLOG_WARN(SLOG_SUB_AUDIT,
                  "login REJECT user='%s' reason=unknown-user", username);
        throttle_record_fail(username, now);
        return -1;
    }

    if (!users_check_password(username, password ? password : "")) {
        kprintf("[session] login refused: bad password for '%s'\n", username);
        SLOG_WARN(SLOG_SUB_AUDIT,
                  "login REJECT user='%s' reason=bad-password", username);
        throttle_record_fail(username, now);
        return -1;
    }

    /* Success -- forget any accumulated failures for this user. */
    throttle_clear(username);

    /* Bump the id even on a re-login so any orphaned tagged procs are
     * automatically de-tagged from the new session. */
    g.id += 1;
    g.active = true;
    g.uid    = u->uid;
    g.gid    = u->gid;
    copy_capped(g.username, u->name, SESSION_USER_MAX);

    /* Persist the most-recent username so the login screen can pre-fill
     * the field next time. We tolerate save failures (e.g. /data not
     * mounted) -- the in-memory cache still has the new value. */
    settings_set_str("user.last", u->name);
    (void)settings_save();

    kprintf("[session] LOGIN id=%d user='%s' uid=%d gid=%d\n",
            g.id, g.username, g.uid, g.gid);
    /* M34F: surface the success on the audit log too so an
     * operator running `auditlog` sees the same event sequence
     * regardless of where they read it. */
    SLOG_INFO(SLOG_SUB_AUDIT,
              "login OK id=%d user='%s' uid=%d gid=%d",
              g.id, g.username, g.uid, g.gid);

    /* M31: surface the login on the desktop. The notification ring
     * is initialised in m14_init() before session_init(), so this
     * always lands cleanly. We deliberately use a friendly title
     * rather than dumping numeric ids -- the ids stay in the serial
     * log for debugging. */
    {
        char body[ABI_NOTIFY_BODY_MAX];
        ksnprintf(body, sizeof(body),
                  "Welcome, %s. Session #%d started.",
                  g.username, g.id);
        notify_post(ABI_NOTIFY_KIND_USER, ABI_NOTIFY_URG_INFO,
                    "session", "Logged in", body);
    }
    return 0;
}

int session_logout(void) {
    if (!g.active) {
        kprintf("[session] logout: no active session\n");
        return -1;
    }
    int sid = g.id;
    kprintf("[session] LOGOUT id=%d user='%s' -- terminating session procs\n",
            sid, g.username);
    SLOG_INFO(SLOG_SUB_AUDIT, "logout id=%d user='%s'", sid, g.username);

    /* Walk the proc table and SIGTERM every live process tagged with
     * the current session. We start at pid 1 because pid 0 (the boot
     * idle thread / shell) must never be killed. */
    int n_killed = 0;
    for (int pid = 1; pid < 64; pid++) {
        struct proc *p = proc_lookup(pid);
        if (!p) continue;
        if (p->state == PROC_TERMINATED) continue;
        if (p->session_id != sid) continue;
        kprintf("[session] SIGTERM pid=%d ('%s' session=%d)\n",
                pid, p->name, p->session_id);
        signal_send_to_pid(pid, SIGTERM);
        n_killed++;
    }
    /* M31: stash the user name BEFORE we wipe it so we can mention
     * who just logged out in the notification. */
    char who[SESSION_USER_MAX];
    copy_capped(who, g.username, sizeof(who));

    g.active = false;
    g.username[0] = '\0';
    g.uid = 0;
    g.gid = 0;
    kprintf("[session] session %d closed (%d procs signalled)\n", sid, n_killed);

    {
        char body[ABI_NOTIFY_BODY_MAX];
        ksnprintf(body, sizeof(body),
                  "Session #%d ended (%d procs signalled). "
                  "Re-displaying login.",
                  sid, n_killed);
        notify_post(ABI_NOTIFY_KIND_USER, ABI_NOTIFY_URG_INFO,
                    "session", who[0] ? who : "Logged out",
                    body);
    }

    /* The login service has autorestart; service_tick will see it
     * isn't running AND should_restart returns true (because we just
     * cleared g.active) and will relaunch /bin/login. */
    return 0;
}

void session_init(void) {
    memset(&g, 0, sizeof(g));
    g.id     = 0;
    g.active = false;

    /* Pre-fill the most-recent username (for log readability). */
    char last[SESSION_USER_MAX];
    settings_get_str("user.last", last, sizeof(last), "toby");
    kprintf("[session] init: last user='%s' (will be pre-filled at login)\n", last);

    /* Register + start the login service. Autorestart is on but gated
     * by session_should_restart_login so it only comes back when no
     * session is active. */
    service_register_program(LOGIN_SERVICE_NAME, LOGIN_PROGRAM_PATH,
                             true, session_should_restart_login);
    service_start(LOGIN_SERVICE_NAME);
}
