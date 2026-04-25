/* installer.c -- milestone 20 OS installer.
 *
 * Takes the bootable image that Limine loads as a module
 * ("install.img", a padded copy of base.iso) and flashes it onto the
 * front of the target disk, then stamps a fresh tobyfs region right
 * after it. The resulting disk is directly bootable as a BIOS hard
 * drive (Limine's hybrid ISO is a valid HDD boot image thanks to
 * --protective-msdos-label) and carries a brand-new persistent /data
 * for the installed OS.
 *
 * The install image lives in BOOTLOADER_RECLAIMABLE memory (mapped
 * into HHDM by vmm_init), so we can treat it as a plain RAM buffer
 * and just DMA/PIO it out to the target one sector at a time.
 *
 * Disk layout produced:
 *   sectors 0..INSTALLER_BOOT_SECTORS-1   : bootable Limine image
 *   sectors INSTALLER_BOOT_SECTORS..end   : tobyfs "/data" partition
 */

#include <tobyos/installer.h>
#include <tobyos/blk.h>
#include <tobyos/tobyfs.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* Stashed by installer_register_image(), called from kernel.c once the
 * Limine module list has been walked. NULL + size=0 means "no install
 * image this boot" (i.e. we booted from an installed disk whose
 * base.iso has no installer module). */
static const uint8_t *g_install_image;
static uint32_t       g_install_size;

void installer_register_image(const void *image, uint32_t size) {
    g_install_image = (const uint8_t *)image;
    g_install_size  = size;
    if (image && size) {
        kprintf("[installer] live install image registered: %u bytes @ %p\n",
                size, image);
    }
}

bool installer_image_available(void) {
    return g_install_image != 0 && g_install_size != 0;
}

uint32_t installer_image_size(void) {
    return g_install_size;
}

/* Copy the install image to the front of `target`, padding the tail
 * of the final sector with zeros. We chunk writes at 4 KiB (8 sectors)
 * so the ATA PIO loop doesn't hold the bus for absurd spans, and so a
 * read/write failure localises to a small span in diagnostics. */
static int flash_boot_image(struct blk_dev *target) {
    const uint32_t CHUNK_BYTES   = 4096u;
    const uint32_t CHUNK_SECTORS = CHUNK_BYTES / 512u;

    if (g_install_size == 0) {
        kprintf("[installer] no install image registered\n");
        return -1;
    }
    uint32_t sectors_needed = (g_install_size + 511u) / 512u;
    if (sectors_needed > INSTALLER_BOOT_SECTORS) {
        kprintf("[installer] install image is %u sectors but only %u reserved\n",
                sectors_needed, INSTALLER_BOOT_SECTORS);
        return -2;
    }
    if (target->sector_count < INSTALLER_BOOT_SECTORS) {
        kprintf("[installer] target disk too small (%lu sectors, need >=%u)\n",
                (unsigned long)target->sector_count, INSTALLER_BOOT_SECTORS);
        return -3;
    }

    uint8_t chunk[CHUNK_BYTES];
    uint32_t off = 0;
    uint64_t lba = 0;
    kprintf("[installer] flashing %u bytes of boot image to %s...\n",
            g_install_size, target->name);

    while (off < g_install_size) {
        uint32_t remain   = g_install_size - off;
        uint32_t to_copy  = remain > CHUNK_BYTES ? CHUNK_BYTES : remain;
        /* Zero-pad the tail of an unaligned last chunk. */
        if (to_copy < CHUNK_BYTES) memset(chunk, 0, CHUNK_BYTES);
        memcpy(chunk, g_install_image + off, to_copy);

        int rc = blk_write(target, lba, CHUNK_SECTORS, chunk);
        if (rc != 0) {
            kprintf("[installer] boot-image write failed at LBA %lu rc=%d\n",
                    (unsigned long)lba, rc);
            return -4;
        }
        off += to_copy;
        lba += CHUNK_SECTORS;

        /* Progress tick every 64 KiB. */
        if ((lba % 128) == 0) {
            kprintf("  ... %u / %u KiB\n", off / 1024u, g_install_size / 1024u);
        }
    }

    /* Zero any residual sectors up to INSTALLER_BOOT_SECTORS so a
     * previous install's leftover bytes can't pollute Limine's view
     * of the image. */
    memset(chunk, 0, CHUNK_BYTES);
    while (lba < INSTALLER_BOOT_SECTORS) {
        uint64_t left = (uint64_t)INSTALLER_BOOT_SECTORS - lba;
        uint32_t n    = (uint32_t)(left < CHUNK_SECTORS ? left : CHUNK_SECTORS);
        int rc = blk_write(target, lba, n, chunk);
        if (rc != 0) {
            kprintf("[installer] boot-region zero-fill failed at LBA %lu rc=%d\n",
                    (unsigned long)lba, rc);
            return -5;
        }
        lba += n;
    }
    kprintf("[installer] boot image written (%u bytes, %u sectors reserved)\n",
            g_install_size, INSTALLER_BOOT_SECTORS);
    return 0;
}

/* Create a fresh tobyfs at sector INSTALLER_BOOT_SECTORS of `target`
 * and mount it at /mnt so we can drop a couple of welcome files on
 * it. The mount stays live until reboot -- harmless, but simpler than
 * adding vfs_umount to the VFS. */
static int format_and_seed_data(struct blk_dev *target) {
    uint64_t avail = target->sector_count - INSTALLER_BOOT_SECTORS;
    struct blk_dev *data_part = blk_offset_wrap(
        target, INSTALLER_BOOT_SECTORS, avail, "data");
    if (!data_part) {
        kprintf("[installer] blk_offset_wrap for /data failed\n");
        return -10;
    }
    kprintf("[installer] formatting /data partition at LBA %u (%lu sectors)\n",
            INSTALLER_BOOT_SECTORS, (unsigned long)avail);

    int rc = tobyfs_format(data_part);
    if (rc != VFS_OK) {
        kprintf("[installer] tobyfs_format failed: %s\n", vfs_strerror(rc));
        return -11;
    }
    kprintf("[installer] tobyfs region formatted\n");

    /* Mount the fresh FS temporarily at /mnt so we can write a few
     * welcome files through the normal VFS API. */
    rc = tobyfs_mount("/mnt", data_part);
    if (rc != VFS_OK) {
        kprintf("[installer] post-format mount('/mnt') failed: %s\n",
                vfs_strerror(rc));
        return -12;
    }

    /* Seed /mnt/welcome.txt so `cat /data/welcome.txt` on the
     * installed system prints something that clearly came from the
     * installer (proving persistence survived install+reboot). */
    static const char welcome[] =
        "tobyOS -- installed via milestone-20 installer.\n"
        "This /data partition was created by `install` on the live ISO.\n"
        "Boot the disk directly (no -cdrom) and this file persists.\n";
    rc = vfs_write_all("/mnt/welcome.txt", welcome, sizeof(welcome) - 1);
    if (rc != VFS_OK) {
        kprintf("[installer] seed /mnt/welcome.txt failed: %s\n",
                vfs_strerror(rc));
        /* Non-fatal -- the disk is still bootable, just without the
         * welcome file. */
    }

    /* Mirror the live /etc/motd onto the installed /data/motd so the
     * operator can verify the cross-image file copy succeeded. */
    void *motd_buf = 0;
    size_t motd_size = 0;
    if (vfs_read_all("/etc/motd", &motd_buf, &motd_size) == VFS_OK) {
        (void)vfs_write_all("/mnt/motd", motd_buf, motd_size);
        kfree(motd_buf);
    }
    return 0;
}

/* Milestone-20 boot self-test. Mirrors the style of pkg_m17_selftest:
 * the normal kernel build compiles this as an empty stub; a kernel
 * built with -DINSTALL_M20_SELFTEST runs the full install workflow
 * against the primary IDE disk during boot and dumps status to
 * serial so a QMP-less test harness can verify without typing into
 * the shell. */
#ifdef INSTALL_M20_SELFTEST
void installer_m20_selftest(void) {
    kprintf("[m20-selftest] step 1: probe target disk\n");
    /* Milestone 21: the disk has already been probed and registered
     * during pci_bind_drivers() in kernel.c::_start. Just look it up. */
    struct blk_dev *target = blk_get_first();
    if (!target) {
        kprintf("[m20-selftest] FAIL: no target disk\n");
        return;
    }
    if (!installer_image_available()) {
        /* Booted without the install module (e.g. from an already-
         * installed disk). Nothing to do. */
        kprintf("[m20-selftest] SKIP: install image not loaded (post-install boot?)\n");
        return;
    }
    /* Idempotency guard: if /data was ALREADY mounted from the
     * installed-layout offset (which kernel.c attempts automatically
     * when the legacy offset-0 mount fails), the disk is already a
     * milestone-20 install -- skip to avoid looping re-installs on
     * every boot when building with the selftest flag. */
    struct vfs_stat st;
    if (vfs_stat("/data/welcome.txt", &st) == VFS_OK) {
        kprintf("[m20-selftest] SKIP: /data/welcome.txt already present -- "
                "target is already installed.\n");
        return;
    }
    kprintf("[m20-selftest] step 2: run installer_run\n");
    int rc = installer_run(target);
    if (rc != 0) {
        kprintf("[m20-selftest] FAIL: installer_run rc=%d\n", rc);
        return;
    }
    kprintf("[m20-selftest] step 3: verify /mnt/welcome.txt\n");
    void *buf = 0; size_t n = 0;
    int vrc = vfs_read_all("/mnt/welcome.txt", &buf, &n);
    if (vrc != VFS_OK) {
        kprintf("[m20-selftest] FAIL: read /mnt/welcome.txt rc=%d\n", vrc);
        return;
    }
    kprintf("[m20-selftest] /mnt/welcome.txt: %u bytes\n", (unsigned)n);
    kfree(buf);
    kprintf("[m20-selftest] SUCCESS -- installed image written, "
            "/data partition formatted + seeded.\n");
}
#else
void installer_m20_selftest(void) { /* no-op in default builds */ }
#endif

int installer_run(struct blk_dev *target) {
    if (!target) {
        kprintf("[installer] no target block device\n");
        return -1;
    }
    if (!installer_image_available()) {
        kprintf("[installer] no install image available -- are we booted "
                "from the live ISO?\n");
        return -2;
    }

    /* Check target capacity up front so we fail fast with a clear
     * message instead of mid-flash. */
    uint32_t min_sectors = INSTALLER_BOOT_SECTORS +
                           TFS_TOTAL_BLOCKS * TFS_SECTORS_PER_BLOCK;
    if (target->sector_count < min_sectors) {
        kprintf("[installer] target too small: %lu sectors < %u required\n",
                (unsigned long)target->sector_count, min_sectors);
        return -3;
    }

    kprintf("[installer] target=%s sectors=%lu  image=%u bytes\n",
            target->name,
            (unsigned long)target->sector_count,
            g_install_size);

    int rc = flash_boot_image(target);
    if (rc != 0) return rc;

    rc = format_and_seed_data(target);
    if (rc != 0) return rc;

    kprintf("[installer] install complete. Reboot to boot from the disk "
            "(QEMU: drop -cdrom, keep -drive file=disk.img).\n");
    return 0;
}
