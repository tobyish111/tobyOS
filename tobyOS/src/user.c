/* user.c -- kernel-side user management (milestone 4).
 *
 * Enriched user database stored at /system/users.db. On first boot,
 * creates a default "admin" user with uid 0. Passwords are hashed
 * with BLAKE2b(salt || password) producing a 32-byte digest.
 *
 * Integrates with proc: each proc inherits its parent's uid, and
 * user_get_current_uid() / user_set_current_uid() read/write the
 * current process's uid field.
 */

#include <tobyos/user.h>
#include <tobyos/proc.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* monocypher for BLAKE2b hashing */
#include <monocypher.h>

#define USER_DB_PATH   "/system/users.db"
#define USER_DB_MAGIC  0x55534552u  /* 'USER' */
#define USER_DB_VER    1
#define USER_MAX_USERS 16

struct user_db_header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t _reserved;
};

static struct user_info g_users[USER_MAX_USERS];
static int              g_user_count;
static bool             g_user_inited;

/* ---- helpers ---- */

static void hash_password(const char *password, const uint8_t *salt,
                          uint8_t *out_hash)
{
    /* BLAKE2b(salt || password) -> 32 bytes */
    size_t plen = 0;
    if (password) while (password[plen]) plen++;

    uint8_t buf[16 + 256];
    memcpy(buf, salt, 16);
    size_t copy = plen;
    if (copy > 256) copy = 256;
    if (copy > 0) memcpy(buf + 16, password, copy);

    crypto_blake2b(out_hash, 32, buf, 16 + copy);
}

static void generate_salt(uint8_t *salt)
{
    /* Simple pseudo-random salt from TSC */
    uint64_t tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc));
    for (int i = 0; i < 16; i++) {
        salt[i] = (uint8_t)(tsc >> (i & 7));
        tsc = tsc * 6364136223846793005ULL + 1442695040888963407ULL;
    }
}

static int save_db(void)
{
    size_t total = sizeof(struct user_db_header) +
                   (size_t)g_user_count * sizeof(struct user_info);
    char *buf = (char *)kmalloc(total);
    if (!buf) return -1;

    struct user_db_header *hdr = (struct user_db_header *)buf;
    hdr->magic     = USER_DB_MAGIC;
    hdr->version   = USER_DB_VER;
    hdr->count     = (uint32_t)g_user_count;
    hdr->_reserved = 0;

    memcpy(buf + sizeof(struct user_db_header),
           g_users, (size_t)g_user_count * sizeof(struct user_info));

    int rc = vfs_write_all(USER_DB_PATH, buf, total);
    kfree(buf);
    if (rc != VFS_OK) {
        kprintf("[user] save_db failed: %s\n", vfs_strerror(rc));
        return -1;
    }
    return 0;
}

static int load_db(void)
{
    void *buf = 0;
    size_t sz = 0;
    int rc = vfs_read_all(USER_DB_PATH, &buf, &sz);
    if (rc != VFS_OK || !buf) return -1;

    if (sz < sizeof(struct user_db_header)) {
        kfree(buf);
        return -1;
    }

    struct user_db_header *hdr = (struct user_db_header *)buf;
    if (hdr->magic != USER_DB_MAGIC || hdr->version != USER_DB_VER) {
        kprintf("[user] bad magic/version in %s\n", USER_DB_PATH);
        kfree(buf);
        return -1;
    }

    int count = (int)hdr->count;
    if (count > USER_MAX_USERS) count = USER_MAX_USERS;

    size_t needed = sizeof(struct user_db_header) +
                    (size_t)count * sizeof(struct user_info);
    if (sz < needed) {
        kprintf("[user] truncated db (%lu < %lu)\n",
                (unsigned long)sz, (unsigned long)needed);
        kfree(buf);
        return -1;
    }

    memcpy(g_users, (char *)buf + sizeof(struct user_db_header),
           (size_t)count * sizeof(struct user_info));
    g_user_count = count;

    kfree(buf);
    return 0;
}

/* ---- public API ---- */

void user_init(void)
{
    if (g_user_inited) return;
    g_user_inited = true;

    memset(g_users, 0, sizeof(g_users));
    g_user_count = 0;

    if (load_db() == 0) {
        kprintf("[user] loaded %d user(s) from %s\n",
                g_user_count, USER_DB_PATH);
    } else {
        kprintf("[user] creating default admin user\n");
        user_create("admin", "admin", 1);
    }
}

int user_authenticate(const char *username, const char *password)
{
    if (!username || !password) return -1;

    for (int i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].username, username) != 0) continue;
        if (g_users[i].flags & USER_FLAG_LOCKED) return -1;

        uint8_t hash[32];
        hash_password(password, g_users[i].salt, hash);
        if (memcmp(hash, g_users[i].password_hash, 32) == 0)
            return g_users[i].uid;
        return -1;
    }
    return -1;
}

int user_create(const char *username, const char *password, int is_admin)
{
    if (!username || !password) return -1;
    if (g_user_count >= USER_MAX_USERS) return -1;

    /* Check for duplicate username */
    for (int i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].username, username) == 0) return -1;
    }

    struct user_info *u = &g_users[g_user_count];
    memset(u, 0, sizeof(*u));

    u->uid = g_user_count;
    u->gid = g_user_count;

    size_t nlen = 0;
    while (username[nlen] && nlen < 31) nlen++;
    memcpy(u->username, username, nlen);
    u->username[nlen] = '\0';

    /* Build home dir: /home/<username> */
    const char *prefix = "/home/";
    size_t plen = 6;
    if (plen + nlen < 63) {
        memcpy(u->home_dir, prefix, plen);
        memcpy(u->home_dir + plen, username, nlen);
        u->home_dir[plen + nlen] = '\0';
    }

    generate_salt(u->salt);
    hash_password(password, u->salt, u->password_hash);

    if (is_admin) u->flags |= USER_FLAG_ADMIN;

    g_user_count++;
    save_db();

    kprintf("[user] created user '%s' uid=%d%s\n",
            u->username, u->uid, is_admin ? " (admin)" : "");
    return u->uid;
}

int user_delete(int uid)
{
    if (uid == 0) return -1; /* cannot delete admin */
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].uid == uid) {
            /* Shift remaining entries down */
            for (int j = i; j < g_user_count - 1; j++)
                g_users[j] = g_users[j + 1];
            g_user_count--;
            memset(&g_users[g_user_count], 0, sizeof(struct user_info));
            save_db();
            return 0;
        }
    }
    return -1;
}

int user_get_info(int uid, struct user_info *out)
{
    if (!out) return -1;
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].uid == uid) {
            memcpy(out, &g_users[i], sizeof(struct user_info));
            return 0;
        }
    }
    return -1;
}

int user_set_password(int uid, const char *new_password)
{
    if (!new_password) return -1;
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].uid == uid) {
            generate_salt(g_users[i].salt);
            hash_password(new_password, g_users[i].salt,
                          g_users[i].password_hash);
            save_db();
            return 0;
        }
    }
    return -1;
}

int user_count(void)
{
    return g_user_count;
}

int user_get_current_uid(void)
{
    struct proc *p = current_proc();
    return p ? p->uid : 0;
}

void user_set_current_uid(int uid)
{
    struct proc *p = current_proc();
    if (p) p->uid = uid;
}
