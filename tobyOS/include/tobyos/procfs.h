/* procfs.h -- /proc virtual filesystem (M1.5).
 *
 * Exposes process and kernel state as read-only virtual files:
 *   /proc/<pid>/status  - process name, state, pid, ppid, memory
 *   /proc/<pid>/cmdline - process command line (name)
 *   /proc/self          - symlink to /proc/<current_pid>
 *   /proc/uptime        - system uptime in seconds
 *   /proc/meminfo       - total/free/used memory
 *   /proc/version       - kernel version string
 */

#ifndef TOBYOS_PROCFS_H
#define TOBYOS_PROCFS_H

void procfs_init(void);

/* Mount procfs at an extra point (mount(2) -t proc). Returns VFS_*. */
int  procfs_mount_at(const char *path);

#endif /* TOBYOS_PROCFS_H */
