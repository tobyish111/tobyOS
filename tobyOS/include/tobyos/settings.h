/* settings.h -- minimal persistent key=value configuration store
 * (milestone 14).
 *
 * The settings layer keeps a tiny in-memory cache of (key, value) pairs
 * and persists them to /data/settings.conf as plain text. Format:
 *
 *     # comments start with '#'
 *     key=value
 *     desktop.bg=0x00204060
 *     user.last=toby
 *
 * Whitespace around '=' is NOT trimmed (kept simple; just write
 * "key=value" with no spaces). Keys are case-sensitive ASCII; values are
 * arbitrary printable bytes up to SETTING_VAL_MAX-1.
 *
 * Lifecycle:
 *   settings_init()        called once at boot, after the /data tobyfs
 *                          mount succeeded. Loads /data/settings.conf
 *                          if present; if not (or unreadable), installs
 *                          the built-in defaults and writes them out.
 *   settings_get_str(...)  read a value; returns the default if missing.
 *   settings_get_u32(...)  parse the value as 0xHEX or decimal u32.
 *   settings_set_str(...)  update the in-memory cache.
 *   settings_save()        flush the cache back to /data/settings.conf.
 *
 * This is intentionally not thread-safe -- only pid 0 (and syscalls
 * running on user threads) ever touches it, and each access is short.
 */

#ifndef TOBYOS_SETTINGS_H
#define TOBYOS_SETTINGS_H

#include <tobyos/types.h>

#define SETTING_KEY_MAX     32
#define SETTING_VAL_MAX     64
#define SETTING_MAX_ENTRIES 32

/* Path of the persistent backing file. Lives on /data (the tobyfs
 * disk-backed mount) so it survives reboots. If /data isn't mounted,
 * the in-memory cache still works -- save() just becomes a no-op. */
#define SETTINGS_PATH "/data/settings.conf"

/* Boot-time entry point. Safe to call when /data is missing: in that
 * case only the defaults are loaded into the cache and save() will
 * fail gracefully. */
void settings_init(void);

/* Copy the value for `key` into `out` (NUL-terminated). If the key
 * isn't set, copy the optional `def` (or empty string when def==NULL)
 * instead. Returns the number of bytes written excluding the NUL. */
size_t settings_get_str(const char *key, char *out, size_t cap, const char *def);

/* Parse the value as either 0x-prefixed hex or plain decimal u32. If
 * the key is missing or the value is unparsable, returns `def`. */
uint32_t settings_get_u32(const char *key, uint32_t def);

/* Set / update an entry. Returns 0 on success, -1 on overflow (key
 * table full and key didn't already exist) or invalid args. Does NOT
 * persist on its own -- call settings_save() to flush. */
int settings_set_str(const char *key, const char *val);

/* Persist the cache to SETTINGS_PATH. Returns 0 on success, -1 if the
 * write failed (e.g. /data not mounted). */
int settings_save(void);

/* Diagnostics: print the cache to the kernel log. */
void settings_dump(void);

#endif /* TOBYOS_SETTINGS_H */
