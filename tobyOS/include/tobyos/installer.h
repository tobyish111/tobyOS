/* installer.h -- milestone 20 OS installer.
 *
 * Writes a bootable image (loaded by Limine as a module at boot) onto
 * a target block device, formats a fresh tobyfs in the trailing
 * region, and seeds a few welcome files on the new filesystem.
 *
 * Disk layout produced (milestone 20):
 *   sectors 0..INSTALLER_BOOT_SECTORS-1 : hybrid Limine-bootable image
 *   sectors INSTALLER_BOOT_SECTORS..+N  : tobyfs "/data" region
 *
 * All APIs return 0 on success, negative on error. Messages are
 * printed to the console so the operator can follow along.
 */

#ifndef TOBYOS_INSTALLER_H
#define TOBYOS_INSTALLER_H

#include <tobyos/types.h>

struct blk_dev;

/* Reserved boot region at the front of every installed disk. Large
 * enough to hold Limine + kernel + initrd comfortably (the current
 * base.iso clocks in under 1 MiB; we reserve 4 MiB to leave headroom
 * for growing the initrd + future kernels). Must stay in sync with
 * INSTALL_IMG_SIZE in the Makefile. */
#define INSTALLER_BOOT_BYTES    (4u * 1024u * 1024u)
#define INSTALLER_BOOT_SECTORS  (INSTALLER_BOOT_BYTES / 512u)

/* Called from kernel.c during boot. Stashes the Limine-loaded install
 * image (if any) so installer_run() can find it later. Passing NULL /
 * size=0 is legal and means "this boot has no installer image"
 * (e.g. after installing, the disk's base.iso has no install.img
 * module). */
void installer_register_image(const void *image, uint32_t size);

/* True iff an install image is available this boot (i.e. we're
 * running from the live ISO rather than a disk install). */
bool installer_image_available(void);

/* Raw size of the registered install image, in bytes. */
uint32_t installer_image_size(void);

/* End-to-end install: flash image, format /data partition, seed a
 * couple of welcome files. `target` must be a block device with at
 * least INSTALLER_BOOT_SECTORS + TFS_TOTAL_BLOCKS*TFS_SECTORS_PER_BLOCK
 * sectors. Blocking -- returns after the target is fully written.
 *
 * Returns 0 on success, negative on error. */
int installer_run(struct blk_dev *target);

/* Milestone-20 boot self-test. No-op in normal builds. When the
 * kernel is built with -DINSTALL_M20_SELFTEST, boot drives the full
 * install workflow against the primary IDE disk and dumps
 * [m20-selftest] status lines to serial so an external test harness
 * can verify without driving the shell. Called from kernel.c just
 * before shell_init(). */
void installer_m20_selftest(void);

#endif /* TOBYOS_INSTALLER_H */
