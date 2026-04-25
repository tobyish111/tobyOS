/* initrd.h -- locate the boot-time tar module and hand it to ramfs.
 *
 * Limine ships our initramfs as a regular module (see limine.conf). At
 * boot, after the heap is up but before any code wants to read files,
 * we look for a module whose basename is "initrd.tar", point ramfs at
 * it, and the VFS now has a root mount.
 *
 * Returns true if the mount succeeded. On failure the kernel keeps
 * booting -- the shell just won't have a filesystem and ls/cat/run
 * will all report "no filesystem mounted".
 */

#ifndef TOBYOS_INITRD_H
#define TOBYOS_INITRD_H

#include <tobyos/types.h>

bool initrd_init(void);

#endif /* TOBYOS_INITRD_H */
