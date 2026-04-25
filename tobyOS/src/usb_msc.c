/* usb_msc.c -- USB Mass Storage Class driver (BBB transport, SCSI).
 *
 * Milestone 23C. Talks BBB ("Bulk-Only Transport") to a single LUN
 * SCSI device over the bulk-IN/bulk-OUT pipe pair set up by xHCI.
 * Exposes the resulting media as a struct blk_dev so the partition +
 * FAT32 layers can drive it the same way they drive IDE/AHCI/NVMe.
 *
 * BBB three-phase handshake (USB MSC BBB spec, rev 1.0):
 *
 *      Host                                       Device
 *      ----                                       ------
 *  1.  CBW (31 bytes, OUT)         ===========>
 *  2.  Data (IN or OUT, length =                Data
 *      CBW.dCBWDataTransferLength)               (or stalls if no data)
 *  3.  CSW (13 bytes, IN)          <===========  CSW
 *
 * On a STALL during the data phase the host issues
 * CLEAR_FEATURE(ENDPOINT_HALT) on the stalled endpoint and proceeds
 * to read the CSW. If reading the CSW also stalls, do BBB Reset
 * (class-specific control request 0xFF) + CLEAR_FEATURE on both
 * bulk endpoints and treat the command as failed.
 *
 * QEMU verification: `make run-xhci-usb` boots with a usb-storage
 * device backed by a FAT32-formatted disk image; the kernel should
 * register a "usb0" blk_dev on top of it, scan its GPT (none in this
 * build -- a raw FAT32 image), opportunistically auto-mount it at
 * /usb (kernel boot path) and exercise the FAT32 self-test on /usb.
 *
 * Limitations (intentional):
 *   - Single LUN per device (LUN 0). Multi-LUN sticks are rare; we
 *     just ignore LUN 1+ if the device reports them.
 *   - At most one MSC device at a time. Adding a second is a
 *     blk_register name collision away (we'd just bump the suffix).
 *   - Synchronous I/O only -- every read()/write() blocks the caller.
 *     With BBB this is exactly what the spec requires.
 *   - 16-bit dCBWDataTransferLength capped at 64 KiB per CBW (we
 *     never issue larger than 8 KiB / 16 sectors today).
 */

#include <tobyos/usb_msc.h>
#include <tobyos/usb.h>
#include <tobyos/blk.h>
#include <tobyos/partition.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pmm.h>
#include <tobyos/pit.h>
#include <tobyos/vfs.h>
#include <tobyos/fat32.h>
#include <tobyos/abi/abi.h>

/* ============================================================== */
/* xHCI helpers we share with src/xhci.c. Forward-declare to avoid */
/* leaking xhci.h internals into every consumer of usb_msc.h.      */
/* ============================================================== */

bool xhci_control_class(struct usb_device *dev,
                        uint8_t bm_request_type, uint8_t b_request,
                        uint16_t w_value, uint16_t w_index,
                        void *buf, uint16_t w_length);

bool xhci_configure_bulk_endpoints(struct usb_device *dev,
                                   uint8_t  in_dci,  uint16_t in_mps,
                                   uint8_t  out_dci, uint16_t out_mps);

bool xhci_bulk_xfer_sync(struct usb_device *dev, uint8_t dci,
                         void *buf, uint32_t len,
                         uint32_t *out_residue);

bool xhci_recover_stall(struct usb_device *dev, uint8_t dci);

/* ============================================================== */
/* BBB / SCSI on-the-wire structures                                */
/* ============================================================== */

#define CBW_SIGNATURE   0x43425355u   /* "USBC" little-endian */
#define CSW_SIGNATURE   0x53425355u   /* "USBS" little-endian */

#define CBW_FLAG_IN     0x80          /* dCBWFlags bit 7 = IN (read) */

#define CSW_STATUS_GOOD     0x00
#define CSW_STATUS_FAILED   0x01
#define CSW_STATUS_PHASE    0x02

#pragma pack(push, 1)
struct cbw {
    uint32_t dCBWSignature;          /* CBW_SIGNATURE */
    uint32_t dCBWTag;                /* echoed in matching CSW */
    uint32_t dCBWDataTransferLength; /* host data buffer size */
    uint8_t  bmCBWFlags;             /* bit 7: IN(1)/OUT(0) */
    uint8_t  bCBWLUN;                /* bits 3:0 */
    uint8_t  bCBWCBLength;           /* CB length, 1..16 */
    uint8_t  CBWCB[16];              /* the SCSI command itself */
};
_Static_assert(sizeof(struct cbw) == 31, "BBB CBW must be 31 B");

struct csw {
    uint32_t dCSWSignature;          /* CSW_SIGNATURE */
    uint32_t dCSWTag;                /* must equal CBW.dCBWTag */
    uint32_t dCSWDataResidue;        /* bytes the device did NOT process */
    uint8_t  bCSWStatus;             /* CSW_STATUS_* */
};
_Static_assert(sizeof(struct csw) == 13, "BBB CSW must be 13 B");
#pragma pack(pop)

/* SCSI opcodes we use. */
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A

/* ============================================================== */
/* Per-device state                                                 */
/* ============================================================== */

#define USB_MSC_MAX_DEVICES 2          /* one root-hub stick + one spare */

struct usb_msc {
    struct usb_device *udev;
    uint8_t            iface_num;
    uint8_t            max_lun;        /* SCSI LUNs - 1; 0 = single LUN */
    uint8_t            slot_id_cached; /* xHCI slot at probe time (kept after unbind) */

    uint32_t           block_size;     /* bytes per LBA (almost always 512) */
    uint64_t           block_count;    /* total LBAs */

    uint32_t           tag_seq;        /* monotonically increasing CBW tag */

    /* DMA-safe scratch buffers. We keep one cluster-sized read buffer
     * + one read/write CBW + CSW slot. The MSC driver serialises all
     * I/O against itself so there is no concurrent use. */
    struct cbw        *cbw;
    uint64_t           cbw_phys;
    struct csw        *csw;
    uint64_t           csw_phys;

    char               name[16];       /* e.g. "usb0" */
    struct blk_dev     blk;
    struct usb_msc    *self;           /* simplifies blk_dev->priv lookup */

    /* M26E: telemetry. Updated from msc_read/msc_write/usb_msc_unbind. */
    struct usb_msc_stats stats;
    bool                 in_use;       /* slot has been populated this boot */
};

/* Static pool. Heap allocations would also work but a tiny pool keeps
 * the boot path predictable and matches the rest of the milestone-21
 * device drivers (blk_ata / blk_ahci / blk_nvme). */
static struct usb_msc g_msc[USB_MSC_MAX_DEVICES];
static size_t         g_msc_count = 0;

/* ============================================================== */
/* DMA scratch allocator: one shared 4 KiB page, kept zero-init'd.  */
/* Pre-cluster (4 KiB) is the largest data phase any FS layer will   */
/* hand us today, so we pin a single page per device once + reuse.   */
/* ============================================================== */

#define USB_MSC_DATA_BUF_SZ 4096u
struct dma_buf {
    void    *virt;
    uint64_t phys;
};

static bool alloc_dma_page(struct dma_buf *out) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return false;
    out->virt = pmm_phys_to_virt(phys);
    out->phys = phys;
    memset(out->virt, 0, 4096);
    return true;
}

/* ============================================================== */
/* Per-device data buffer (one cluster).                            */
/* ============================================================== */

struct msc_dma {
    void    *virt;
    uint64_t phys;
};

static struct msc_dma g_data_buf[USB_MSC_MAX_DEVICES];

/* ============================================================== */
/* Low-level CBW / CSW helpers                                     */
/* ============================================================== */

static int do_cbw_data_csw(struct usb_msc *m,
                           bool dir_in, void *data_buf, uint32_t data_len,
                           const uint8_t *cdb, uint8_t cdb_len) {
    struct cbw *cbw = m->cbw;
    struct csw *csw = m->csw;

    /* (1) Build CBW. */
    memset(cbw, 0, sizeof(*cbw));
    cbw->dCBWSignature          = CBW_SIGNATURE;
    cbw->dCBWTag                = ++m->tag_seq;
    cbw->dCBWDataTransferLength = data_len;
    cbw->bmCBWFlags             = dir_in ? CBW_FLAG_IN : 0;
    cbw->bCBWLUN                = 0;       /* LUN 0 */
    cbw->bCBWCBLength           = cdb_len;
    if (cdb_len > 16) cdb_len = 16;
    memcpy(cbw->CBWCB, cdb, cdb_len);

    uint32_t residue = 0;
    if (!xhci_bulk_xfer_sync(m->udev, m->udev->bulk_out_dci,
                             cbw, sizeof(*cbw), &residue)) {
        kprintf("[usb-msc] CBW send failed (cdb=0x%02x)\n", cdb[0]);
        /* Try to clear OUT halt -- some devices stall a malformed CBW. */
        xhci_recover_stall(m->udev, m->udev->bulk_out_dci);
        return -1;
    }

    /* (2) Data phase (optional). On STALL we recover that endpoint and
     * proceed straight to the CSW per the BBB spec. */
    bool data_stalled = false;
    if (data_len > 0 && data_buf) {
        uint8_t dci = dir_in ? m->udev->bulk_in_dci : m->udev->bulk_out_dci;
        residue = data_len;
        if (!xhci_bulk_xfer_sync(m->udev, dci, data_buf, data_len, &residue)) {
            data_stalled = true;
            xhci_recover_stall(m->udev, dci);
        }
    }

    /* (3) CSW IN. If this STALLs once we recover and try again -- if
     * THAT stalls we trigger a BBB reset and bail. */
    uint32_t csw_residue = 0;
    if (!xhci_bulk_xfer_sync(m->udev, m->udev->bulk_in_dci,
                             csw, sizeof(*csw), &csw_residue)) {
        xhci_recover_stall(m->udev, m->udev->bulk_in_dci);
        if (!xhci_bulk_xfer_sync(m->udev, m->udev->bulk_in_dci,
                                 csw, sizeof(*csw), &csw_residue)) {
            kprintf("[usb-msc] CSW IN stalled twice -- giving up\n");
            return -2;
        }
    }

    if (csw->dCSWSignature != CSW_SIGNATURE) {
        kprintf("[usb-msc] bad CSW signature 0x%08x\n", csw->dCSWSignature);
        return -3;
    }
    if (csw->dCSWTag != m->tag_seq) {
        kprintf("[usb-msc] CSW tag mismatch (want %u got %u)\n",
                m->tag_seq, csw->dCSWTag);
        return -4;
    }
    if (csw->bCSWStatus != CSW_STATUS_GOOD) {
        /* CHECK CONDITION at the SCSI layer -- e.g. unit-not-ready
         * right after enumeration. Tell the caller; they'll usually
         * issue REQUEST SENSE and retry. */
        return data_stalled ? -5 : (int)csw->bCSWStatus;
    }
    return 0;
}

/* ============================================================== */
/* SCSI command builders                                            */
/* ============================================================== */

static int scsi_test_unit_ready(struct usb_msc *m) {
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return do_cbw_data_csw(m, true, 0, 0, cdb, 6);
}

static int scsi_inquiry(struct usb_msc *m, void *out, uint8_t len) {
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, len, 0 };
    return do_cbw_data_csw(m, true, out, len, cdb, 6);
}

static int scsi_request_sense(struct usb_msc *m, void *out, uint8_t len) {
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, len, 0 };
    return do_cbw_data_csw(m, true, out, len, cdb, 6);
}

static int scsi_read_capacity_10(struct usb_msc *m,
                                 uint32_t *last_lba, uint32_t *block_size) {
    uint8_t cdb[10] = { SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t resp[8] = { 0 };
    int rc = do_cbw_data_csw(m, true, resp, sizeof(resp), cdb, 10);
    if (rc) return rc;
    /* big-endian: 4-byte returned LBA, 4-byte block size */
    *last_lba   = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                  ((uint32_t)resp[2] <<  8) | ((uint32_t)resp[3]);
    *block_size = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                  ((uint32_t)resp[6] <<  8) | ((uint32_t)resp[7]);
    return 0;
}

/* SCSI READ (10) / WRITE (10): one CBW per call. count is 16-bit LBAs;
 * the FS layers above us cap at one cluster (8 sectors today), so we
 * never split into multiple CBWs. */
static int scsi_rw_10(struct usb_msc *m, bool is_write,
                      uint32_t lba, uint16_t count, void *buf) {
    uint8_t cdb[10] = {
        is_write ? SCSI_WRITE_10 : SCSI_READ_10,
        0,                               /* RDPROTECT/DPO/FUA = 0 */
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >>  8), (uint8_t)(lba),
        0,                               /* group number */
        (uint8_t)(count >> 8), (uint8_t)(count),
        0,                               /* control */
    };
    return do_cbw_data_csw(m, !is_write, buf,
                           (uint32_t)count * m->block_size, cdb, 10);
}

/* ============================================================== */
/* blk_dev hooks                                                    */
/* ============================================================== */

static int msc_read(struct blk_dev *dev, uint64_t lba, uint32_t count,
                    void *buf) {
    struct usb_msc *m = (struct usb_msc *)dev->priv;
    if (!m || !count) return 0;
    /* M26C: the underlying USB slot can disappear between when the FS
     * layer cached the blk_dev pointer and now. udev gets nulled by
     * usb_msc_unbind() so we return EIO instead of touching freed
     * controller state. */
    if (!m->udev) { m->stats.reads_eio++; return -1; }
    if (lba + count > m->block_count) { m->stats.reads_eio++; return -1; }

    uint8_t *out = (uint8_t *)buf;
    /* Largest CBW we use is one cluster = 4 KiB / m->block_size sectors. */
    uint32_t max_per_cbw = USB_MSC_DATA_BUF_SZ / m->block_size;
    if (max_per_cbw == 0) max_per_cbw = 1;

    while (count > 0) {
        uint16_t chunk = (count > max_per_cbw) ? (uint16_t)max_per_cbw
                                               : (uint16_t)count;

        int rc = scsi_rw_10(m, false, (uint32_t)lba, chunk,
                            g_data_buf[m - g_msc].virt);
        if (rc) {
            kprintf("[usb-msc] READ(10) lba=%llu cnt=%u rc=%d\n",
                    (unsigned long long)lba, chunk, rc);
            m->stats.reads_eio++;
            return -2;
        }
        memcpy(out, g_data_buf[m - g_msc].virt,
               (size_t)chunk * m->block_size);

        out   += (size_t)chunk * m->block_size;
        lba   += chunk;
        m->stats.bytes_read += (uint64_t)chunk * m->block_size;
        count -= chunk;
    }
    m->stats.reads_ok++;
    return 0;
}

static int msc_write(struct blk_dev *dev, uint64_t lba, uint32_t count,
                     const void *buf) {
    struct usb_msc *m = (struct usb_msc *)dev->priv;
    if (!m || !count) return 0;
    if (!m->udev) { m->stats.writes_eio++; return -1; }
    if (lba + count > m->block_count) { m->stats.writes_eio++; return -1; }

    const uint8_t *in = (const uint8_t *)buf;
    uint32_t max_per_cbw = USB_MSC_DATA_BUF_SZ / m->block_size;
    if (max_per_cbw == 0) max_per_cbw = 1;

    while (count > 0) {
        uint16_t chunk = (count > max_per_cbw) ? (uint16_t)max_per_cbw
                                               : (uint16_t)count;
        memcpy(g_data_buf[m - g_msc].virt, in,
               (size_t)chunk * m->block_size);

        int rc = scsi_rw_10(m, true, (uint32_t)lba, chunk,
                            g_data_buf[m - g_msc].virt);
        if (rc) {
            kprintf("[usb-msc] WRITE(10) lba=%llu cnt=%u rc=%d\n",
                    (unsigned long long)lba, chunk, rc);
            m->stats.writes_eio++;
            return -2;
        }
        in    += (size_t)chunk * m->block_size;
        lba   += chunk;
        m->stats.bytes_written += (uint64_t)chunk * m->block_size;
        count -= chunk;
    }
    m->stats.writes_ok++;
    return 0;
}

static const struct blk_ops g_msc_blk_ops = {
    .read  = msc_read,
    .write = msc_write,
};

/* ============================================================== */
/* Class request: BBB Reset, Get Max LUN                            */
/* ============================================================== */

static bool msc_get_max_lun(struct usb_msc *m, uint8_t *out) {
    uint8_t buf = 0;
    /* GET MAX LUN: class request, IN, recipient=interface, len=1.
     * If the device STALLs we fall back to LUN 0 (devices with a
     * single LUN are allowed to stall this request). */
    if (xhci_control_class(m->udev,
                           USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                           USB_MSC_REQ_GET_MAX_LUN,
                           0, m->iface_num,
                           &buf, 1)) {
        *out = buf;
        return true;
    }
    *out = 0;
    return false;   /* maybe stalled; caller treats as single-LUN */
}

static bool msc_bbb_reset(struct usb_msc *m) {
    /* BBB Reset: class request, OUT, recipient=interface, len=0. */
    return xhci_control_class(m->udev,
                              USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                              USB_MSC_REQ_RESET,
                              0, m->iface_num,
                              0, 0);
}

/* ============================================================== */
/* Probe entry point                                                */
/* ============================================================== */

bool usb_msc_probe(struct usb_device *dev,
                   const struct usb_iface_desc *iface,
                   const struct usb_endpoint_desc *ep_in,
                   const struct usb_endpoint_desc *ep_out) {
    if (g_msc_count >= USB_MSC_MAX_DEVICES) {
        kprintf("[usb-msc] too many devices, ignoring slot %u\n",
                dev->slot_id);
        return false;
    }

    struct usb_msc *m = &g_msc[g_msc_count];
    memset(m, 0, sizeof(*m));
    m->udev          = dev;
    m->iface_num     = iface->bInterfaceNumber;
    m->slot_id_cached = dev->slot_id;
    m->in_use         = true;

    uint16_t in_mps  = ep_in ->wMaxPacketSize & 0x07FFu;
    uint16_t out_mps = ep_out->wMaxPacketSize & 0x07FFu;
    uint8_t  in_dci  = (uint8_t)(2u * USB_EP_NUM(ep_in ->bEndpointAddress) + 1u);
    uint8_t  out_dci = (uint8_t)(2u * USB_EP_NUM(ep_out->bEndpointAddress));

    dev->bulk_in_addr  = ep_in ->bEndpointAddress;
    dev->bulk_out_addr = ep_out->bEndpointAddress;
    dev->msc_iface_num = iface->bInterfaceNumber;

    if (!xhci_configure_bulk_endpoints(dev, in_dci, in_mps, out_dci, out_mps)) {
        kprintf("[usb-msc] Configure-EP failed for slot %u\n", dev->slot_id);
        return false;
    }

    /* Allocate DMA scratch (one CBW + one CSW + one cluster data). All
     * three need to be HHDM-mapped so xhci_bulk_xfer_sync can
     * vmm_translate them straight to phys. A single 4K page is plenty
     * for CBW + CSW; the data buffer gets its own page. */
    struct dma_buf cbw_csw;
    if (!alloc_dma_page(&cbw_csw)) {
        kprintf("[usb-msc] OOM for CBW/CSW page\n");
        return false;
    }
    m->cbw      = (struct cbw *)cbw_csw.virt;
    m->cbw_phys = cbw_csw.phys;
    m->csw      = (struct csw *)((uint8_t *)cbw_csw.virt + 64);
    m->csw_phys = cbw_csw.phys + 64;

    if (!alloc_dma_page((struct dma_buf *)&g_data_buf[g_msc_count])) {
        kprintf("[usb-msc] OOM for data buffer\n");
        return false;
    }

    /* Get Max LUN. Single-LUN devices may STALL this; that's fine. */
    uint8_t mlun = 0;
    msc_get_max_lun(m, &mlun);
    m->max_lun = mlun;
    if (m->max_lun > 0) {
        kprintf("[usb-msc] device reports %u LUNs; only LUN 0 used\n",
                (unsigned)(m->max_lun + 1));
    }

    /* INQUIRY (36 bytes is the standard chunk every device returns). */
    uint8_t inq[36] = { 0 };
    int rc = scsi_inquiry(m, inq, sizeof(inq));
    if (rc) {
        kprintf("[usb-msc] INQUIRY rc=%d -- BBB reset + retry\n", rc);
        msc_bbb_reset(m);
        rc = scsi_inquiry(m, inq, sizeof(inq));
        if (rc) {
            kprintf("[usb-msc] INQUIRY retry rc=%d -- giving up\n", rc);
            return false;
        }
    }
    kprintf("[usb-msc] slot %u INQUIRY: ptype=%u vendor='%c%c%c%c%c%c%c%c' "
            "product='%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c'\n",
            dev->slot_id, inq[0] & 0x1F,
            inq[ 8], inq[ 9], inq[10], inq[11],
            inq[12], inq[13], inq[14], inq[15],
            inq[16], inq[17], inq[18], inq[19],
            inq[20], inq[21], inq[22], inq[23],
            inq[24], inq[25], inq[26], inq[27],
            inq[28], inq[29], inq[30], inq[31]);

    /* TEST UNIT READY -- some devices need a couple of polls right
     * after enumeration. Try up to 5 times with 100ms between. */
    for (int i = 0; i < 5; i++) {
        rc = scsi_test_unit_ready(m);
        if (rc == 0) break;
        /* Drain the sense data so the next TUR isn't masked. */
        uint8_t sense[18] = { 0 };
        scsi_request_sense(m, sense, sizeof(sense));
        pit_sleep_ms(100);
    }
    if (rc) {
        kprintf("[usb-msc] TEST_UNIT_READY never succeeded (rc=%d)\n", rc);
        return false;
    }

    /* READ CAPACITY (10): tells us LBA count + sector size. */
    uint32_t last_lba = 0, blksz = 0;
    rc = scsi_read_capacity_10(m, &last_lba, &blksz);
    if (rc) {
        kprintf("[usb-msc] READ_CAPACITY rc=%d\n", rc);
        return false;
    }
    if (blksz != BLK_SECTOR_SIZE) {
        /* All consumer USB sticks expose 512 B sectors, even when the
         * underlying flash is 4 KiB native. We don't bother with 4Kn. */
        kprintf("[usb-msc] sector size %u not supported (need %u) -- skip\n",
                blksz, BLK_SECTOR_SIZE);
        return false;
    }
    m->block_size  = blksz;
    m->block_count = (uint64_t)last_lba + 1ull;

    /* Build a "usbN" name. Lower-case + decimal index, stable for
     * blk_find lookups from the shell. */
    int idx = (int)g_msc_count;
    char *p = m->name;
    *p++ = 'u'; *p++ = 's'; *p++ = 'b';
    if (idx >= 10) { *p++ = (char)('0' + idx / 10); }
    *p++ = (char)('0' + idx % 10);
    *p   = '\0';

    m->blk.name         = m->name;
    m->blk.ops          = &g_msc_blk_ops;
    m->blk.sector_count = m->block_count;
    m->blk.priv         = m;
    m->blk.class        = BLK_CLASS_DISK;
    m->self             = m;
    blk_register(&m->blk);
    dev->msc_state = m;

    kprintf("[usb-msc] '%s' online: %llu sectors x %u B (%llu MiB)\n",
            m->name, (unsigned long long)m->block_count, m->block_size,
            (unsigned long long)((m->block_count * m->block_size) >> 20));

    /* Scan for GPT partitions on the new disk. partition_scan_disk
     * silently returns -1 for raw (non-GPT) media, which is fine for
     * single-FS USB sticks. The kernel boot path will look at /usb
     * directly for the FAT32 mount in that case. */
    int parts = partition_scan_disk(&m->blk);
    if (parts > 0) {
        kprintf("[usb-msc] '%s': %d GPT partition(s) registered\n",
                m->name, parts);
    } else {
        kprintf("[usb-msc] '%s': no GPT (raw filesystem assumed)\n",
                m->name);
    }

    g_msc_count++;
    return true;
}

/* ============================================================== */
/* M26E: helper -- enumerate VFS mounts that live on this disk.    */
/* ============================================================== */

/* `cookie` for vfs_iter_mounts during unbind. We collect up to
 * VFS_MAX_MOUNTS mount-points whose underlying block device is the
 * MSC disk OR one of its GPT partitions, then the caller drops them
 * outside the iteration (to avoid mutating the mount table mid-walk). */
#define MSC_UNBIND_MAX_VICTIMS 8

struct msc_unbind_walk {
    struct usb_msc *target;
    char            victims[MSC_UNBIND_MAX_VICTIMS][64];
    size_t          victim_count;
};

static bool msc_unbind_collect(const char *mount_point,
                               const struct vfs_ops *ops,
                               void *mount_data,
                               void *cookie) {
    struct msc_unbind_walk *w = (struct msc_unbind_walk *)cookie;
    if (!mount_point || !ops) return true;

    /* Only FAT32 mounts can sit on a USB stick today, but check the
     * vtable identity first so we never reinterpret e.g. tobyfs
     * mount-data as a struct fat32. */
    if (ops != &fat32_ops) return true;

    struct blk_dev *bd = fat32_blkdev_of(mount_data);
    if (!bd) return true;

    /* Match either the disk itself or any of its partition slices. */
    bool match = false;
    if (bd == &w->target->blk) match = true;
    else if (bd->parent == &w->target->blk) match = true;

    if (!match) return true;
    if (w->victim_count >= MSC_UNBIND_MAX_VICTIMS) return true;

    size_t cap = sizeof(w->victims[0]) - 1;
    size_t n   = 0;
    while (n < cap && mount_point[n]) n++;
    memcpy(w->victims[w->victim_count], mount_point, n);
    w->victims[w->victim_count][n] = '\0';
    w->victim_count++;
    return true;
}

/* M26C+M26E: tear down the MSC<->USB binding without ripping the
 * blk_dev out from under the partition / filesystem layer.
 *
 * Sequence:
 *   1. discover live FAT32 mounts that point at our disk or its
 *      partitions (M26E),
 *   2. log a *loud* warning per mount and force-unmount each (the
 *      umount op flushes best-effort; blk_write returns -EIO if the
 *      device is already gone, and that's fine),
 *   3. mark the blk_dev (and every partition slice that descends from
 *      it) gone so any straggler I/O returns -EIO without touching the
 *      controller,
 *   4. null udev / msc_state so the slot cannot accidentally be
 *      reused by xHCI's slot table.
 *
 * We deliberately do NOT free the blk_dev or remove it from the
 * registry: open file descriptors elsewhere may still hold the
 * pointer and they all check ->gone before touching ops. */
void usb_msc_unbind(struct usb_device *dev) {
    if (!dev) return;
    struct usb_msc *m = (struct usb_msc *)dev->msc_state;
    if (!m) return;

    kprintf("[usb-msc] unbind slot %u (blk='%s')\n",
            dev->slot_id, m->blk.name ? m->blk.name : "?");

    /* (1) collect victims */
    struct msc_unbind_walk walk;
    walk.target       = m;
    walk.victim_count = 0;
    memset(walk.victims, 0, sizeof(walk.victims));
    vfs_iter_mounts(msc_unbind_collect, &walk);

    /* (2) loud warning + forced unmount per live mount */
    if (walk.victim_count > 0) {
        m->stats.unsafe_removals++;
        kprintf("[usb-msc] WARN: '%s' yanked while %lu mount(s) active "
                "-- pending writes may be lost\n",
                m->blk.name ? m->blk.name : "?",
                (unsigned long)walk.victim_count);
        for (size_t i = 0; i < walk.victim_count; i++) {
            kprintf("[usb-msc]   unsafe-removal: forcing umount of '%s'\n",
                    walk.victims[i]);
            /* M26E: nullify udev BEFORE the umount so the FAT32 flush
             * inside fat32_umount sees ->dev->gone (set below) and
             * skips the dirty-FAT write instead of stalling on a dead
             * controller. We mark gone first via blk_mark_gone. */
        }
    } else {
        m->stats.safe_removals++;
    }

    /* (3) mark the disk + partition slices gone before tearing down */
    blk_mark_gone(&m->blk);

    /* (4) drop pointers to the (about-to-die) USB device */
    m->udev        = 0;
    dev->msc_state = 0;

    /* Now safely run the umount ops -- blk_read/write inside FAT32
     * flush will short-circuit to -EIO via the gone check. */
    for (size_t i = 0; i < walk.victim_count; i++) {
        int rc = vfs_unmount(walk.victims[i]);
        if (rc != 0) {
            kprintf("[usb-msc]   unsafe-removal: vfs_unmount('%s') rc=%d\n",
                    walk.victims[i], rc);
        }
    }
}

/* ============================================================== */
/* M26E: introspection + selftest                                   */
/* ============================================================== */

/* Walk the mount table looking for a FAT32 mount whose backing
 * blk_dev is this MSC disk or one of its partitions. Returns the
 * mount point in `out`/`cap`. Empty string if no live mount. */
struct msc_find_walk {
    struct usb_msc *target;
    char           *out;
    size_t          cap;
    bool            found;
};

static bool msc_find_mount_cb(const char *mount_point,
                              const struct vfs_ops *ops,
                              void *mount_data,
                              void *cookie) {
    struct msc_find_walk *w = (struct msc_find_walk *)cookie;
    if (w->found || !mount_point || !ops) return true;
    if (ops != &fat32_ops) return true;
    struct blk_dev *bd = fat32_blkdev_of(mount_data);
    if (!bd) return true;
    if (bd != &w->target->blk && bd->parent != &w->target->blk) return true;
    size_t cap = w->cap > 0 ? w->cap - 1 : 0;
    size_t n   = 0;
    while (n < cap && mount_point[n]) n++;
    memcpy(w->out, mount_point, n);
    w->out[n] = '\0';
    w->found  = true;
    return false; /* stop iteration */
}

size_t usb_msc_count(void) {
    return g_msc_count;
}

bool usb_msc_introspect_at(size_t idx, struct usb_msc_info *out) {
    if (!out || idx >= g_msc_count) return false;
    struct usb_msc *m = &g_msc[idx];

    memset(out, 0, sizeof(*out));
    out->blk_name    = m->blk.name;
    out->slot_id     = m->slot_id_cached;
    out->block_size  = m->block_size;
    out->block_count = m->block_count;
    out->bound       = (m->udev != 0);
    out->gone        = m->blk.gone;
    out->stats       = m->stats;

    struct msc_find_walk walk = {
        .target = m,
        .out    = out->mount_point,
        .cap    = sizeof(out->mount_point),
        .found  = false,
    };
    vfs_iter_mounts(msc_find_mount_cb, &walk);
    out->mounted = walk.found;
    return true;
}

int usb_msc_selftest(char *msg, size_t cap) {
    if (g_msc_count == 0) {
        ksnprintf(msg, cap,
                  "no USB MSC slots (run with -device usb-storage)");
        return ABI_DEVT_SKIP;
    }

    kprintf("[usb-msc-test] selftest start (%lu slot(s))\n",
            (unsigned long)g_msc_count);

    int failures   = 0;
    int bound      = 0;
    int gone       = 0;
    int probe_pass = 0;
    const char *fail_nm    = "?";
    const char *fail_what  = "ok";

    for (size_t i = 0; i < g_msc_count; i++) {
        struct usb_msc *m = &g_msc[i];
        const char *nm = m->blk.name ? m->blk.name : "?";

        if (!m->in_use) {
            kprintf("[usb-msc-test]   slot %lu: FAIL not initialised\n",
                    (unsigned long)i);
            failures++; fail_nm = nm; fail_what = "uninit";
            continue;
        }
        if (!m->blk.name || !m->blk.ops) {
            kprintf("[usb-msc-test]   slot %lu '%s': FAIL blk_dev incomplete\n",
                    (unsigned long)i, nm);
            failures++; fail_nm = nm; fail_what = "blk-incomplete";
            continue;
        }
        if (m->blk.class != BLK_CLASS_DISK) {
            kprintf("[usb-msc-test]   slot %lu '%s': FAIL class=%d (want DISK)\n",
                    (unsigned long)i, nm, (int)m->blk.class);
            failures++; fail_nm = nm; fail_what = "wrong-class";
            continue;
        }
        if (m->udev && m->blk.gone) {
            kprintf("[usb-msc-test]   slot %lu '%s': FAIL bound but gone\n",
                    (unsigned long)i, nm);
            failures++; fail_nm = nm; fail_what = "bound+gone";
            continue;
        }
        if (!m->udev && !m->blk.gone) {
            kprintf("[usb-msc-test]   slot %lu '%s': FAIL unbound but live\n",
                    (unsigned long)i, nm);
            failures++; fail_nm = nm; fail_what = "unbound+live";
            continue;
        }

        if (m->udev) {
            uint8_t scratch[BLK_SECTOR_SIZE];
            memset(scratch, 0, sizeof(scratch));
            int rc = blk_read(&m->blk, 0, 1, scratch);
            if (rc != 0) {
                kprintf("[usb-msc-test]   slot %lu '%s': FAIL probe read rc=%d\n",
                        (unsigned long)i, nm, rc);
                failures++; fail_nm = nm; fail_what = "probe-read";
                continue;
            }
            kprintf("[usb-msc-test]   slot %lu '%s': PASS bound, %llu sectors\n",
                    (unsigned long)i, nm,
                    (unsigned long long)m->block_count);
            bound++;
            probe_pass++;
        } else {
            kprintf("[usb-msc-test]   slot %lu '%s': PASS gone (unbound)\n",
                    (unsigned long)i, nm);
            gone++;
        }
    }

    kprintf("[usb-msc-test] selftest %s (%d failure(s))\n",
            failures == 0 ? "PASS" : "FAIL", failures);

    if (failures > 0) {
        ksnprintf(msg, cap,
                  "USB MSC: slot '%s' %s; %d/%lu OK",
                  fail_nm, fail_what,
                  (int)(g_msc_count - (size_t)failures),
                  (unsigned long)g_msc_count);
        return -ABI_EIO;
    }
    ksnprintf(msg, cap,
              "USB MSC: %lu slot(s), bound=%d gone=%d probe_ok=%d",
              (unsigned long)g_msc_count, bound, gone, probe_pass);
    return 0;
}
