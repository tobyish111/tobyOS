/* vfs_perms.c -- VFS permission policy enforcement layer.
 *
 * Provides higher-level permission policy on top of the existing
 * vfs_perm_check / vfs_chmod / vfs_chown primitives in vfs.c:
 *
 *   - Home directory creation (/home/USERNAME, mode 0700, owned by user)
 *   - System directory protection (/system/ owned by root, mode 0755)
 *   - Check/set/get permission wrappers that integrate with proc uid
 *   - Root (uid 0) bypasses all checks
 */

#include <tobyos/vfs_perms.h>
#include <tobyos/vfs.h>
#include <tobyos/proc.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>

#define TAG "perms"

/* System paths that should be root-owned with mode 0755. */
static const char *g_system_paths[] = {
    "/system",
    "/bin",
    "/etc",
    "/lib",
    NULL,
};

int vfs_perms_init(void) {
    SLOG_INFO(TAG, "VFS permission policy layer initialised");
    vfs_perms_enforce_system_dirs();
    return 0;
}

int vfs_check_perm(const char *path, int uid, int access_mode) {
    if (!path) return -1;
    if (uid == 0) return 0;

    struct vfs_stat st;
    int rc = vfs_stat(path, &st);
    if (rc != VFS_OK) return rc;

    if (!(st.mode & VFS_MODE_VALID)) return 0;

    uint16_t mode_bits = (uint16_t)(st.mode & VFS_MODE_PERMS);

    /* Owner check */
    if ((uint32_t)uid == st.uid) {
        uint16_t owner = (mode_bits >> 6) & 7;
        if ((owner & access_mode) == (uint16_t)access_mode) return 0;
        return VFS_ERR_PERM;
    }

    /* Group check -- simplified: just check group bits if gid matches */
    struct proc *p = current_proc();
    int gid = p ? p->gid : 0;
    if ((uint32_t)gid == st.gid) {
        uint16_t grp = (mode_bits >> 3) & 7;
        if ((grp & access_mode) == (uint16_t)access_mode) return 0;
        return VFS_ERR_PERM;
    }

    /* Other */
    uint16_t other = mode_bits & 7;
    if ((other & access_mode) == (uint16_t)access_mode) return 0;

    return VFS_ERR_PERM;
}

int vfs_set_perms(const char *path, uint16_t uid, uint16_t gid, uint16_t mode) {
    if (!path) return -1;

    int rc = vfs_chown(path, uid, gid);
    if (rc != VFS_OK && rc != VFS_ERR_ROFS) return rc;

    rc = vfs_chmod(path, mode);
    return rc;
}

int vfs_get_perms(const char *path, struct file_perms *out) {
    if (!path || !out) return -1;

    struct vfs_stat st;
    int rc = vfs_stat(path, &st);
    if (rc != VFS_OK) return rc;

    out->uid  = (uint16_t)st.uid;
    out->gid  = (uint16_t)st.gid;
    out->mode = (uint16_t)(st.mode & VFS_MODE_PERMS);
    return 0;
}

int vfs_perms_ensure_home(const char *username, int uid) {
    if (!username) return -1;

    char home[128];
    ksnprintf(home, sizeof(home), "/home/%s", username);

    struct vfs_stat st;
    int rc = vfs_stat(home, &st);
    if (rc == VFS_OK) {
        /* Already exists -- ensure ownership */
        vfs_chown(home, (uint32_t)uid, (uint32_t)uid);
        vfs_chmod(home, 00700u);
        SLOG_INFO(TAG, "home dir %s exists, ownership enforced for uid %d", home, uid);
        return 0;
    }

    /* Create /home if needed */
    vfs_mkdir("/home");

    rc = vfs_mkdir(home);
    if (rc != VFS_OK && rc != VFS_ERR_EXIST) {
        SLOG_WARN(TAG, "failed to create home dir %s: %d", home, rc);
        return rc;
    }

    vfs_chown(home, (uint32_t)uid, (uint32_t)uid);
    vfs_chmod(home, 00700u);
    SLOG_INFO(TAG, "created home dir %s for uid %d (mode 0700)", home, uid);
    return 0;
}

int vfs_perms_enforce_system_dirs(void) {
    int enforced = 0;

    for (int i = 0; g_system_paths[i]; i++) {
        const char *p = g_system_paths[i];
        struct vfs_stat st;
        if (vfs_stat(p, &st) != VFS_OK) continue;

        /* Ensure root ownership and mode 0755 */
        vfs_chown(p, 0, 0);
        vfs_chmod(p, 00755u);
        enforced++;
    }

    if (enforced > 0) {
        SLOG_INFO(TAG, "enforced root ownership on %d system directories", enforced);
    }
    return enforced;
}

bool vfs_perms_is_system_path(const char *path) {
    if (!path) return false;

    for (int i = 0; g_system_paths[i]; i++) {
        size_t plen = strlen(g_system_paths[i]);
        if (strncmp(path, g_system_paths[i], plen) == 0) {
            if (path[plen] == '\0' || path[plen] == '/')
                return true;
        }
    }
    return false;
}

int vfs_perms_default_file_mode(void) { return 00644; }
int vfs_perms_default_dir_mode(void)  { return 00755; }
