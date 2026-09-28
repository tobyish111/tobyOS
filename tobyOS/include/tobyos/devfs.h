/* devfs.h -- /dev as a VFS directory tree. See src/devfs.c. */
#ifndef TOBYOS_DEVFS_H
#define TOBYOS_DEVFS_H

#include <tobyos/types.h>

/* Mount devfs at /dev. Called once at boot, before /dev/shm's tmpfs. */
int devfs_init(void);

/* The mode (S_IFCHR | perms) of the device NODE at `kpath` -- /dev/null,
 * /dev/pts/3, /dev/input/event0 -- and its Linux major/minor, or 0 when no
 * node is there (a directory is not a node). The single table behind it is
 * also what `ls /dev` lists and what stat/chdir see through the VFS. */
uint32_t devfs_node_mode(const char *kpath, uint32_t *maj, uint32_t *min);

#endif
