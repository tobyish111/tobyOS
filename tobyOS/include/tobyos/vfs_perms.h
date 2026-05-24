/* vfs_perms.h -- VFS permission policy enforcement layer.
 *
 * Builds on top of the existing vfs_perm_check() infrastructure to
 * provide higher-level policy:
 *   - Home directory creation and ownership enforcement
 *   - System directory protection (root-owned, mode 0755)
 *   - Default permission assignment for new files/dirs
 *   - Permission checking wrappers that integrate with proc uid
 */

#ifndef TOBYOS_VFS_PERMS_H
#define TOBYOS_VFS_PERMS_H

#include <tobyos/types.h>

#define PERM_READ    4
#define PERM_WRITE   2
#define PERM_EXEC    1

struct file_perms {
    uint16_t uid;
    uint16_t gid;
    uint16_t mode;   /* rwxrwxrwx (9 bits) */
};

int  vfs_perms_init(void);

int  vfs_check_perm(const char *path, int uid, int access_mode);
int  vfs_set_perms(const char *path, uint16_t uid, uint16_t gid, uint16_t mode);
int  vfs_get_perms(const char *path, struct file_perms *out);

int  vfs_perms_ensure_home(const char *username, int uid);

int  vfs_perms_enforce_system_dirs(void);

bool vfs_perms_is_system_path(const char *path);

int  vfs_perms_default_file_mode(void);
int  vfs_perms_default_dir_mode(void);

#endif /* TOBYOS_VFS_PERMS_H */
