/* session.h -- minimal user-session manager (milestone 14).
 *
 * A "session" represents one user being logged in to the GUI desktop.
 * tobyOS supports a single active session at a time -- multi-user is
 * out of scope for this milestone.
 *
 * Lifecycle:
 *
 *   boot
 *     -> session_init()                 g.id = 0, g.active = false
 *     -> service "login" launches /bin/login (autorestart, only when
 *        no session is active).
 *
 *   user types name + clicks Login
 *     -> /bin/login calls SYS_LOGIN(username)
 *        -> session_login(username) bumps g.id to a fresh non-zero
 *           value, stores the username, marks active. Persists
 *           "user.last" via the settings layer.
 *        -> /bin/login exits cleanly. service_tick sees it died but
 *           the should_restart callback returns false (session is now
 *           active), so login does NOT come back.
 *
 *   user runs apps from the desktop launcher
 *     -> drain_launch_queue() in gui.c temporarily switches the
 *        current_proc()->session_id to g.id so the new child inherits
 *        the session tag in spawn_internal.
 *
 *   user clicks Logout
 *     -> SYS_LOGOUT() -> session_logout()
 *        -> walk the proc table, SIGTERM every PCB whose session_id
 *           matches g.id (skipping pid 0).
 *        -> mark session inactive.
 *        -> service_tick sees /bin/login isn't running and the session
 *           is inactive -> should_restart returns true -> login comes
 *           back up.
 *
 * The whole thing lives in <200 lines of C; permissions, multiple
 * concurrent sessions, password authentication etc. are all out of
 * scope.
 */

#ifndef TOBYOS_SESSION_H
#define TOBYOS_SESSION_H

#include <tobyos/types.h>

#define SESSION_USER_MAX  32

struct session_info {
    int  id;
    bool active;
    char username[SESSION_USER_MAX];
    int  uid;
    int  gid;
};

/* Boot-time entry. Zeroes session state, registers + starts the login
 * program service via the service manager. Caller must have already
 * called settings_init() and service_init(). */
void session_init(void);

/* Promote the system to "logged in as username". Bumps the session id,
 * persists the username via settings, and lets the login service stay
 * stopped until the next logout. Returns 0 on success, -1 on bad args.
 * Safe to call when already active -- treated as a re-login (id bumps,
 * existing session-tagged procs are NOT torn down). Used by SYS_LOGIN. */
int session_login(const char *username);

/* Tear the active session down: SIGTERMs every PCB whose session_id
 * matches the current id (excluding pid 0), marks the session
 * inactive, and lets the login service relaunch on the next service
 * tick. Returns 0 on success, -1 if no session was active. */
int session_logout(void);

/* Read-only accessors. session_current_id() returns 0 when no session
 * is active. Useful for: gui.c launch queue (tag children), session
 * relaunch policy, the SYS_SESSION_INFO syscall. */
int  session_current_id(void);
bool session_active(void);
void session_get_info(struct session_info *out);

/* Identity of the active user (milestone 15). Both return 0 ("root")
 * when no session is active -- which is also what kernel-internal code
 * (boot threads, idle pid 0) sees, by design. */
int  session_current_uid(void);
int  session_current_gid(void);

/* Restart-policy hook for service.c. Returns true (== "please restart
 * me") iff the session is currently NOT active. Used to gate the login
 * service so it only comes back up between sessions. */
struct service;
bool session_should_restart_login(struct service *s);

#endif /* TOBYOS_SESSION_H */
