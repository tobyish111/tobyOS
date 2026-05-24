/* user.h -- kernel-side user management (milestone 4).
 *
 * Extends the simple users.h (name:uid:gid flat file) with password
 * hashing, authentication, and per-process uid integration. Stores
 * an enriched user database at /system/users.db in a simple binary
 * format.
 */

#ifndef TOBYOS_USER_H
#define TOBYOS_USER_H

#include <tobyos/types.h>

#define USER_FLAG_ADMIN   0x01u
#define USER_FLAG_LOCKED  0x02u

struct user_info {
    int      uid;
    int      gid;
    char     username[32];
    char     home_dir[64];
    uint8_t  password_hash[32]; /* SHA-256 of password+salt */
    uint8_t  salt[16];
    uint32_t flags;             /* USER_FLAG_ADMIN, USER_FLAG_LOCKED, etc */
};

void user_init(void);
int  user_authenticate(const char *username, const char *password);
int  user_create(const char *username, const char *password, int is_admin);
int  user_delete(int uid);
int  user_get_info(int uid, struct user_info *out);
int  user_set_password(int uid, const char *new_password);
int  user_count(void);
int  user_get_current_uid(void);
void user_set_current_uid(int uid);

#endif /* TOBYOS_USER_H */
