/* blk_ata.c -- ATA PIO driver for the IDE primary master.
 *
 * Milestone-21 update: this driver now lives under the PCI driver
 * registry. Its match table targets any storage.IDE controller
 * (class 0x01, subclass 0x01) -- which on QEMU's default i440fx is
 * the PIIX3 IDE function at 00:01.1 (8086:7010). The probe runs the
 * same identify + self-test as before, then publishes a struct
 * blk_dev via blk_register() so the rest of the kernel finds it
 * through blk_get_first() instead of a direct blk_ata_init() call.
 *
 * The transport itself is unchanged: 28-bit LBA, polled status, no
 * IRQs, no DMA. This is fine for QEMU's IDE emulation and keeps the
 * "make run" zero-config dev path bit-identical to milestone 20.
 *
 * Port layout (primary IDE bus):
 *   0x1F0   data            (16-bit, 256 words / sector)
 *   0x1F1   error / features
 *   0x1F2   sector count
 *   0x1F3   LBA[0:7]
 *   0x1F4   LBA[8:15]
 *   0x1F5   LBA[16:23]
 *   0x1F6   drive | LBA[24:27] | 0xE0 (LBA mode, master)
 *   0x1F7   status / command
 *   0x3F6   alt status / control  (write 0x02 to disable IRQs)
 *
 * Status register bits:
 *   BSY (0x80) = controller busy, ignore everything else
 *   DRQ (0x08) = data request -- ready to PIO-transfer a sector
 *   ERR (0x01) = command failed (read 0x1F1 for code)
 *
 * Commands used:
 *   0x20 READ SECTORS  (LBA28, PIO)
 *   0x30 WRITE SECTORS (LBA28, PIO)
 *   0xE7 CACHE FLUSH
 *   0xEC IDENTIFY DEVICE
 */

#include <tobyos/blk.h>
#include <tobyos/cpu.h>
#include <tobyos/pci.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6

#define ATA_REG_DATA     (ATA_PRIMARY_IO + 0)
#define ATA_REG_ERROR    (ATA_PRIMARY_IO + 1)
#define ATA_REG_FEAT     (ATA_PRIMARY_IO + 1)
#define ATA_REG_SECCNT   (ATA_PRIMARY_IO + 2)
#define ATA_REG_LBA0     (ATA_PRIMARY_IO + 3)
#define ATA_REG_LBA1     (ATA_PRIMARY_IO + 4)
#define ATA_REG_LBA2     (ATA_PRIMARY_IO + 5)
#define ATA_REG_DRIVE    (ATA_PRIMARY_IO + 6)
#define ATA_REG_STATUS   (ATA_PRIMARY_IO + 7)
#define ATA_REG_CMD      (ATA_PRIMARY_IO + 7)
#define ATA_REG_ALTSTAT  (ATA_PRIMARY_CTRL)
#define ATA_REG_DEVCTRL  (ATA_PRIMARY_CTRL)

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01
#define ATA_SR_DF   0x20    /* device fault */

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY   0xEC

/* The 400ns settling delay after writing the drive-select register is
 * canonically done by reading the alt-status register four times. */
static inline void ata_io_wait(void) {
    (void)inb(ATA_REG_ALTSTAT);
    (void)inb(ATA_REG_ALTSTAT);
    (void)inb(ATA_REG_ALTSTAT);
    (void)inb(ATA_REG_ALTSTAT);
}

/* Spin until BSY clears. Safety bound so a wedged drive doesn't hang
 * the kernel forever during probe. */
static int ata_wait_not_busy(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_REG_STATUS);
        if (!(s & ATA_SR_BSY)) return 0;
    }
    return -1;
}

/* Spin until DRQ asserts (ready to transfer) or ERR/DF/!BSY+!DRQ -- in
 * which case the command failed. */
static int ata_wait_drq(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_REG_STATUS);
        if (s & (ATA_SR_ERR | ATA_SR_DF)) return -2;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

/* Issue a single LBA28 command for `count` sectors at `lba`. Caller
 * does the per-sector PIO loop after this returns. */
static int ata_issue(uint64_t lba, uint8_t count, uint8_t cmd) {
    if (lba > 0x0FFFFFFFu) return -3;          /* outside LBA28 range */

    if (ata_wait_not_busy() != 0) return -1;

    /* 0xE0: master, LBA-mode bit; bits 0..3 carry LBA[27:24]. */
    outb(ATA_REG_DRIVE, 0xE0u | (uint8_t)((lba >> 24) & 0x0Fu));
    ata_io_wait();

    outb(ATA_REG_FEAT,   0x00);
    outb(ATA_REG_SECCNT, count);
    outb(ATA_REG_LBA0,   (uint8_t)(lba       & 0xFF));
    outb(ATA_REG_LBA1,   (uint8_t)((lba >>  8) & 0xFF));
    outb(ATA_REG_LBA2,   (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_REG_CMD,    cmd);
    return 0;
}

static int ata_pio_read(struct blk_dev *dev, uint64_t lba,
                        uint32_t count, void *buf) {
    (void)dev;
    if (count == 0) return 0;
    /* IDE LBA28 sector count: 0 means "256". We split larger requests
     * into chunks of <=255 to keep the interface boring. */
    uint8_t  *out = (uint8_t *)buf;
    while (count > 0) {
        uint32_t chunk = count > 255 ? 255 : count;
        if (ata_issue(lba, (uint8_t)chunk, ATA_CMD_READ_PIO) != 0) return -1;
        for (uint32_t s = 0; s < chunk; s++) {
            if (ata_wait_drq() != 0) return -2;
            uint16_t *p = (uint16_t *)(out + s * BLK_SECTOR_SIZE);
            for (int w = 0; w < 256; w++) p[w] = inw(ATA_REG_DATA);
            ata_io_wait();          /* required between sectors */
        }
        out   += chunk * BLK_SECTOR_SIZE;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

static int ata_pio_write(struct blk_dev *dev, uint64_t lba,
                         uint32_t count, const void *buf) {
    (void)dev;
    if (count == 0) return 0;
    const uint8_t *in = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t chunk = count > 255 ? 255 : count;
        if (ata_issue(lba, (uint8_t)chunk, ATA_CMD_WRITE_PIO) != 0) return -1;
        for (uint32_t s = 0; s < chunk; s++) {
            if (ata_wait_drq() != 0) return -2;
            const uint16_t *p = (const uint16_t *)(in + s * BLK_SECTOR_SIZE);
            for (int w = 0; w < 256; w++) outw(ATA_REG_DATA, p[w]);
            ata_io_wait();
        }
        /* Tell the drive to flush its internal write cache so the host
         * file actually contains our bytes if QEMU is killed shortly
         * after we return. */
        if (ata_wait_not_busy() != 0) return -3;
        outb(ATA_REG_CMD, ATA_CMD_CACHE_FLUSH);
        if (ata_wait_not_busy() != 0) return -4;

        in    += chunk * BLK_SECTOR_SIZE;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

static const struct blk_ops g_ata_ops = {
    .read  = ata_pio_read,
    .write = ata_pio_write,
};

static struct blk_dev g_ata_dev = {
    .name = "ide0:master",
    .ops  = &g_ata_ops,
    .sector_count = 0,
    .priv = 0,
};

static bool g_ata_registered;

/* IDENTIFY DEVICE: probe whether a drive is wired to the primary
 * master, and read its sector count out of words 60..61 of the
 * 256-word identification buffer. */
static int ata_identify(uint64_t *out_sectors) {
    /* Disable IRQs from the bus -- we poll. */
    outb(ATA_REG_DEVCTRL, 0x02);

    if (ata_wait_not_busy() != 0) return -1;
    outb(ATA_REG_DRIVE, 0xA0);   /* select master, non-LBA */
    ata_io_wait();

    outb(ATA_REG_SECCNT, 0);
    outb(ATA_REG_LBA0,   0);
    outb(ATA_REG_LBA1,   0);
    outb(ATA_REG_LBA2,   0);
    outb(ATA_REG_CMD,    ATA_CMD_IDENTIFY);

    uint8_t s = inb(ATA_REG_STATUS);
    if (s == 0) return -2;       /* no device on this bus */

    /* Wait for BSY to clear, then either ERR (not ATA -- could be
     * ATAPI) or DRQ (data is ready). */
    for (int i = 0; i < 1000000; i++) {
        s = inb(ATA_REG_STATUS);
        if (!(s & ATA_SR_BSY)) break;
    }
    if (s & ATA_SR_ERR) return -3;
    if (!(s & ATA_SR_DRQ)) return -4;

    uint16_t id[256];
    for (int w = 0; w < 256; w++) id[w] = inw(ATA_REG_DATA);

    /* Words 60..61 = 32-bit LBA28 max sector count. */
    uint32_t lba28 = ((uint32_t)id[61] << 16) | id[60];
    *out_sectors = lba28;
    return 0;
}

/* Bind to a PIIX3-class IDE controller. The actual transport is via
 * legacy I/O ports 0x1F0/0x3F6 (no use of the BAR0-3 the spec defines
 * for native-mode PCI IDE -- compatibility-mode IDE wires the legacy
 * ports up regardless of what BAR0..3 say, and that's what QEMU's
 * default i440fx + PIIX3 use). */
static int ata_pci_probe(struct pci_dev *dev) {
    /* Idempotency: the registry only allows one bind per device, but
     * because primary + secondary IDE share the same legacy ports we
     * still want to refuse a second bind if some weird board exposes
     * two function-0 IDE controllers. */
    if (g_ata_registered) {
        kprintf("[ata] already bound -- ignoring %02x:%02x.%x\n",
                dev->bus, dev->slot, dev->fn);
        return -1;
    }

    /* PIIX3 prog_if 0x80 = primary in compatibility mode + secondary
     * in compatibility mode + master IDE supported. We don't need
     * bus-master DMA for PIO, but enabling I/O-space access in the
     * command register is a no-op when it's already on (BIOS / Limine
     * does this for us in practice) and harmless to set explicitly. */
    pci_dev_enable(dev, PCI_CMD_IO_SPACE);

    uint64_t sectors = 0;
    int rc = ata_identify(&sectors);
    if (rc != 0) {
        kprintf("[ata] %02x:%02x.%x: no primary-master IDE drive "
                "(identify rc=%d)\n",
                dev->bus, dev->slot, dev->fn, rc);
        return -2;
    }
    if (sectors == 0) {
        kprintf("[ata] %02x:%02x.%x: primary master reports zero "
                "sectors -- ignoring\n",
                dev->bus, dev->slot, dev->fn);
        return -3;
    }
    g_ata_dev.sector_count = sectors;
    kprintf("[ata] primary master: %lu sectors (%lu KiB) -- "
            "PIO LBA28 (PCI %02x:%02x.%x)\n",
            (unsigned long)sectors, (unsigned long)(sectors / 2),
            dev->bus, dev->slot, dev->fn);

    /* Self-test: read sector 0. Catches "drive responded to identify
     * but read still hangs" failure modes early, with a clean message. */
    uint8_t scratch[BLK_SECTOR_SIZE];
    rc = ata_pio_read(&g_ata_dev, 0, 1, scratch);
    if (rc != 0) {
        kprintf("[ata] self-test read(LBA 0) failed rc=%d -- giving up\n", rc);
        return -4;
    }

    blk_register(&g_ata_dev);
    dev->driver_data = &g_ata_dev;
    g_ata_registered = true;
    return 0;
}

/* Class match: any PCI mass-storage / IDE controller. We don't restrict
 * to PIIX3's vendor:device because the same driver works on any IDE
 * controller running in compatibility mode (which is what every
 * relevant board exposes, including modern boards' "legacy IDE" mode). */
static const struct pci_match g_ata_matches[] = {
    { PCI_ANY_ID, PCI_ANY_ID,
      PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_IDE, PCI_ANY_CLASS },
    PCI_MATCH_END,
};

static struct pci_driver g_ata_driver = {
    .name    = "blk_ata",
    .matches = g_ata_matches,
    .probe   = ata_pci_probe,
    .remove  = 0,
    /* M29B: blk_ata is the generic-storage fallback path. It binds
     * any IDE controller exposed in legacy/compat mode -- no
     * vendor-specific code -- so drvmatch reports it as GENERIC. */
    .flags   = PCI_DRIVER_GENERIC,
};

void blk_ata_register(void) {
    pci_register_driver(&g_ata_driver);
}
