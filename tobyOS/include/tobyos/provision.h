/* provision.h -- user-driven persistent-storage provisioning.
 *
 * The one place in tobyOS that is allowed to WRITE a partition table.
 * Everything here is built around a hard safety rule (see the guard in
 * provision.c): a disk carrying a foreign filesystem or a foreign
 * partition table is never a valid target, no flag overrides that.
 * The only unprompted format in the system remains the boot-time
 * auto-format of a blank tobyOS-data GPT partition (kernel.c).
 */

#ifndef TOBYOS_PROVISION_H
#define TOBYOS_PROVISION_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

struct blk_dev;

/* Read-only classification of one whole disk. Fills `fs` (best-effort
 * filesystem signature name, "" if none), the ABI_BLK_F_* flag bits it
 * can determine from on-disk probes + the VFS mount table, and returns
 * the ABI_BLKV_* verdict. Probes never write. */
uint8_t provision_classify(struct blk_dev *d, uint32_t *flags,
                           char fs[ABI_BLK_FS_MAX]);

/* Is `dev` -- or, for a disk, any partition carved from it -- the
 * backing store of a live VFS mount? Fills *mount_point (may be NULL)
 * with the first match. Recognises tobyfs / fat32 / ext2 mounts. */
bool provision_dev_mounted(struct blk_dev *dev, const char **mount_point);

/* Create a tobyOS data volume on whole-disk `d`: guard, GPT with one
 * tobyOS-data partition, tobyfs format, mount as /data when /data is
 * RAM-backed or absent. Returns ABI_PROV_OK_* or -ABI_E* (see abi.h).
 * `force` = ABI_PROV_F_FORCE semantics. */
long provision_create_data_volume(struct blk_dev *d, uint32_t flags);

/* Fill a KERNEL staging buffer with up to `cap` abi_blk_info records
 * (one per registry entry, verdicts included). Returns the count.
 * The syscall wrapper in syscall.c owns the user-copy. */
long provision_fill_blk_list(struct abi_blk_info *out, uint32_t cap);

/* Opt-in boot self-test (-DPROVISION_SELFTEST): builds RAM-backed fake
 * disks (blank / NTFS / FAT-MBR / foreign GPT / mounted) and asserts
 * the guard verdicts, then provisions the blank one end-to-end.
 * Prints "[PROV] ... PASS/FAIL" lines to serial. */
/* Make /data writable by non-root sessions. A freshly formatted tobyfs
 * root is uid 0 / mode 0755, so without this every write from a desktop
 * session fails the permission check. Call it from EVERY site that mounts
 * /data -- it lived only in the boot path, and the provisioning path's
 * second mount silently skipped it. Idempotent. */
void data_volume_relax_perms(void);

void provision_selftest(void);

#endif /* TOBYOS_PROVISION_H */
