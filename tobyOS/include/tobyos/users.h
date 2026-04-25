/* users.h -- minimal on-disk user account database (milestone 15).
 *
 * Backs a tiny key/value-ish store at /data/users with one user per
 * line, in classic /etc/passwd-lite format:
 *
 *     # tobyOS users (milestone 15) -- name:uid:gid
 *     root:0:0
 *     toby:1000:1000
 *     guest:1001:1001
 *
 * If /data/users is missing on boot, users_init() installs the three
 * defaults above and persists them. There is no password column in
 * this milestone -- "authentication" is just "is this name in the
 * file?"; constraints document that an optional simple password check
 * is acceptable, but we keep the implementation honest by skipping it.
 *
 * Lifecycle:
 *   users_init()              load /data/users into the in-memory
 *                             cache; install + persist defaults if
 *                             absent. Safe to call when /data is
 *                             missing -- defaults stay in RAM, save()
 *                             becomes a no-op until next boot.
 *   users_lookup_by_name()    used by session_login at SYS_LOGIN time
 *                             to validate the typed username and
 *                             learn the user's uid/gid.
 *   users_lookup_by_uid()     used by `whoami`, `ls -l`-style listing,
 *                             SYS_USERNAME, and the GUI settings
 *                             header to render uid -> name.
 *   users_add()               used by the shell `users add` command
 *                             to register a new user at runtime.
 *   users_save()              flush the cache back to /data/users.
 *   users_dump()              kprintf the cache (debug).
 */

#ifndef TOBYOS_USERS_H
#define TOBYOS_USERS_H

#include <tobyos/types.h>

#define USER_NAME_MAX  32
#define USER_MAX       16

/* On-disk path. Lives on tobyfs so it survives reboots. */
#define USERS_PATH     "/data/users"

struct user {
    char name[USER_NAME_MAX];
    int  uid;
    int  gid;
};

/* Boot-time entry. Must be called AFTER /data is mounted (otherwise we
 * stay with defaults and save() can't reach disk). Idempotent. */
void users_init(void);

/* Look up a user by name. Returns a pointer into the in-memory cache
 * (do NOT free) or NULL if no such user. The pointer is stable until
 * the next users_add(). */
const struct user *users_lookup_by_name(const char *name);

/* Look up a user by uid. NULL if not found. */
const struct user *users_lookup_by_uid(int uid);

/* Append a new user. Fails (-1) if the name is empty / too long, the
 * cache is full, or the name / uid is already taken. The caller MUST
 * call users_save() to persist the change. */
int  users_add(const char *name, int uid, int gid);

/* Persist the cache to USERS_PATH. Returns 0 on success, -1 on
 * failure (e.g. /data not mounted, write rejected). */
int  users_save(void);

/* Diagnostics. */
void users_dump(void);

/* Iterate the cache. cb is called once per registered user; return
 * value of cb is ignored. Used by the shell `users` command. */
typedef void (*users_visit_fn)(const struct user *u, void *ctx);
void users_visit(users_visit_fn cb, void *ctx);

#endif /* TOBYOS_USERS_H */
