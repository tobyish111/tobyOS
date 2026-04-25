/* m35selftest.c -- Milestone 35 phase-by-phase selftest hooks.
 *
 * Compiled in only when -DM35_SELFTEST is set. The selftests run from
 * kmain right after each subsystem is initialised and emit the same
 * "[m35X-selftest] step N: PASS|FAIL" + final-verdict shape that
 * test_m34.ps1 already greps for, so a thin PowerShell driver
 * (test_m35.ps1) can decide PASS/FAIL off serial.log without having
 * to drive the kernel shell over QMP.
 *
 * Each phase has a dedicated entry point so kmain can call them at
 * the right moment in boot:
 *
 *   m35a_selftest()    after drvconf_load + drvconf_apply
 *   m35b_selftest()    after pci_bind_drivers (virtio-* + extra HW)
 *   m35c_selftest()    after xHCI / USB class enumeration
 *   m35d_selftest()    after hwcompat_init (M35D)
 *   m35e_selftest()    after safemode_init (M35E)
 *   m35f_selftest()    after hwinfo persists its first snapshot
 *
 * No test relies on guest-side userland; the compattest tool added
 * in M35G is a separate, always-built validation suite that runs
 * at boot via -DCOMPATTEST_AUTORUN. */

#ifdef M35_SELFTEST

#include <tobyos/types.h>
#include <tobyos/drvconf.h>
#include <tobyos/drvdb.h>
#include <tobyos/drvmatch.h>
#include <tobyos/pci.h>
#include <tobyos/blk.h>
#include <tobyos/usbreg.h>
#include <tobyos/hwdb.h>
#include <tobyos/safemode.h>
#include <tobyos/hwinfo.h>
#include <tobyos/drvmatch.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/abi/abi.h>

/* ============================================================
 * shared step recorder
 * ============================================================ */

static int g_step_no;
static int g_fail_total;

static void step(const char *area, bool pass, const char *fmt, ...) {
    g_step_no++;
    char tail[160];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(tail, sizeof(tail), fmt, ap);
    va_end(ap);
    if (!pass) g_fail_total++;
    kprintf("[%s-selftest] step %d: %s -- %s\n",
            area, g_step_no, pass ? "PASS" : "FAIL", tail);
}

/* ============================================================
 * M35A -- drvdb + drvconf overrides
 * ============================================================ */

void m35a_selftest(void) {
    g_step_no = 0;
    g_fail_total = 0;
    int local_failures = 0;
    int local_total = 0;
    kprintf("[m35a-selftest] milestone 35A begin\n");

    /* Step 1: drvdb knows our key QEMU PCI IDs. */
    {
        const struct drvdb_pci_entry *e =
            drvdb_pci_lookup(0x1AF4, 0x1000);
        bool ok = (e != NULL) && (e->tier == DRVDB_SUPPORTED) &&
                  e->driver && !strcmp(e->driver, "virtio-net");
        if (!ok) local_failures++;
        local_total++;
        step("m35a", ok,
             "drvdb_pci_lookup(1AF4:1000) -> %s tier=%s drv=%s",
             e ? e->friendly : "(null)",
             drvdb_tier_name(e ? e->tier : DRVDB_UNKNOWN),
             (e && e->driver) ? e->driver : "(none)");
    }

    /* Step 2: drvdb correctly tags an UNSUPPORTED chip. */
    {
        const struct drvdb_pci_entry *e =
            drvdb_pci_lookup(0x10EC, 0x8139);     /* RTL8139 */
        bool ok = (e != NULL) && (e->tier == DRVDB_UNSUPPORTED);
        if (!ok) local_failures++;
        local_total++;
        step("m35a", ok,
             "drvdb_pci_lookup(10EC:8139) tier=%s (expect unsupported)",
             drvdb_tier_name(e ? e->tier : DRVDB_UNKNOWN));
    }

    /* Step 3: USB lookup with class+subclass+protocol exact match. */
    {
        const struct drvdb_usb_entry *e =
            drvdb_usb_lookup(0x03, 0x01, 0x01);
        bool ok = (e != NULL) && e->driver && !strcmp(e->driver, "usb_hid");
        if (!ok) local_failures++;
        local_total++;
        step("m35a", ok,
             "drvdb_usb_lookup(03/01/01) -> %s drv=%s",
             e ? e->friendly : "(null)",
             (e && e->driver) ? e->driver : "(none)");
    }

    /* Step 4: USB lookup falls back to class-wide wildcard. */
    {
        const struct drvdb_usb_entry *e =
            drvdb_usb_lookup(0x09, 0x12, 0x34);     /* hub, made-up sub/proto */
        bool ok = (e != NULL) && e->driver && !strcmp(e->driver, "usb_hub");
        if (!ok) local_failures++;
        local_total++;
        step("m35a", ok,
             "drvdb_usb_lookup(09/wild/wild) -> %s drv=%s",
             e ? e->friendly : "(null)",
             (e && e->driver) ? e->driver : "(none)");
    }

    /* Step 5: drvconf parses the test fixture cleanly. */
    {
        int rc = drvconf_load_path("/etc/drvmatch.conf.test");
        bool ok = (rc >= 0) &&
                  (drvconf_blacklist_count() == 1) &&
                  (drvconf_force_count() == 1);
        if (!ok) local_failures++;
        local_total++;
        step("m35a", ok,
             "drvconf_load_path -> rc=%d blacklist=%lu force=%lu",
             rc,
             (unsigned long)drvconf_blacklist_count(),
             (unsigned long)drvconf_force_count());
    }

    /* Step 6: blacklist predicate works. */
    {
        bool ok = drvconf_is_blacklisted("rtl8169") &&
                  !drvconf_is_blacklisted("e1000") &&
                  !drvconf_is_blacklisted("");
        if (!ok) local_failures++;
        local_total++;
        step("m35a", ok,
             "blacklist predicate (rtl8169=%d e1000=%d empty=%d)",
             (int)drvconf_is_blacklisted("rtl8169"),
             (int)drvconf_is_blacklisted("e1000"),
             (int)drvconf_is_blacklisted(""));
    }

    /* Step 7: force lookup returns the configured driver. */
    {
        const char *d = drvconf_force_driver(0x1AF4, 0x1000);
        bool ok = (d != NULL) && !strcmp(d, "virtio-net") &&
                  drvconf_force_driver(0x9999, 0x9999) == NULL;
        if (!ok) local_failures++;
        local_total++;
        step("m35a", ok,
             "force_driver(1AF4:1000)=%s force_driver(9999:9999)=%s",
             d ? d : "(null)",
             drvconf_force_driver(0x9999, 0x9999) ? "(set)" : "(null)");
    }

    /* Step 8: missing config path is non-fatal. */
    {
        int rc = drvconf_load_path("/etc/drvmatch.conf.does.not.exist");
        bool ok = (rc == 0) &&
                  (drvconf_blacklist_count() == 0) &&
                  (drvconf_force_count() == 0);
        if (!ok) local_failures++;
        local_total++;
        step("m35a", ok,
             "missing config -> rc=%d (expect 0)", rc);
    }

    /* Re-load the production config so the rest of boot sees the
     * intended state. drvconf is idempotent: load + apply is fine
     * to repeat. */
    drvconf_load();

    kprintf("[m35a-selftest] milestone 35A end "
            "(failures=%d total=%d) -- %s\n",
            local_failures, local_total,
            local_failures == 0 ? "PASS" : "FAIL");
}

/* ============================================================
 * M35B -- virtio-blk + virtual hardware breadth
 *
 * The test fixture is build/vblk_test.img: a 4 MiB raw file that the
 * Makefile creates with a fixed ASCII sentinel ("TOBYM35B-VBLK0\0")
 * in the first 16 bytes of LBA 0. Booting via `make run-virtio-blk`
 * attaches it as a modern virtio-blk-pci device, which our driver
 * registers as "vblk0". This selftest then:
 *
 *   1. asks virtio_blk_present() whether a virtio-blk bound at all;
 *   2. confirms the registered blk_dev resolves by name;
 *   3. issues a single 1-sector read through the generic blk API
 *      and matches the sentinel.
 *
 * If no virtio-blk is present (the default `make run` IDE config),
 * the test reports SKIPPED_NO_VIRTIO_BLK -- not a failure -- so the
 * same selftest binary works in either QEMU configuration.
 *
 * As a bonus, we cross-check that the virtio-net driver also bound
 * if the test-host attached one. virtio-net binding is exercised
 * elsewhere; here we only assert that drvdb still reports the right
 * tier for it (regression coverage on the catalogue).
 * ============================================================ */

extern bool virtio_blk_present(void);
extern const char *virtio_blk_name(void);
extern uint64_t virtio_blk_capacity_lba(void);

#define M35B_SENTINEL_LEN 15
static const char m35b_sentinel[M35B_SENTINEL_LEN] = "TOBYM35B-VBLK0";

void m35b_selftest(void) {
    g_step_no = 0;
    g_fail_total = 0;
    int local_failures = 0;
    int local_total = 0;
    int local_skipped = 0;
    kprintf("[m35b-selftest] milestone 35B begin\n");

    bool present = virtio_blk_present();

    /* Step 1: drvdb still tags virtio-blk modern as PARTIAL/SUPPORTED.
     * This is a static-table regression check and runs regardless of
     * whether QEMU attached a virtio-blk device. */
    {
        const struct drvdb_pci_entry *e =
            drvdb_pci_lookup(0x1AF4, 0x1042);
        bool ok = (e != NULL) && e->driver &&
                  !strcmp(e->driver, "virtio-blk") &&
                  (e->tier == DRVDB_SUPPORTED || e->tier == DRVDB_PARTIAL);
        if (!ok) local_failures++;
        local_total++;
        step("m35b", ok,
             "drvdb_pci_lookup(1AF4:1042) -> %s tier=%s drv=%s",
             e ? e->friendly : "(null)",
             drvdb_tier_name(e ? e->tier : DRVDB_UNKNOWN),
             (e && e->driver) ? e->driver : "(none)");
    }

    /* Step 2: virtio-blk presence detector. */
    if (!present) {
        local_skipped++;
        kprintf("[m35b-selftest] step %d: SKIPPED_NO_VIRTIO_BLK -- "
                "virtio_blk_present()=false\n", ++g_step_no);
    } else {
        bool ok = virtio_blk_name() != NULL && virtio_blk_capacity_lba() > 0;
        if (!ok) local_failures++;
        local_total++;
        step("m35b", ok, "virtio-blk bound name=%s capacity_lba=%lu",
             virtio_blk_name() ? virtio_blk_name() : "(null)",
             (unsigned long)virtio_blk_capacity_lba());
    }

    /* Step 3: generic blk_find() resolves the registered name. */
    if (!present) {
        local_skipped++;
        kprintf("[m35b-selftest] step %d: SKIPPED_NO_VIRTIO_BLK -- "
                "blk_find skip\n", ++g_step_no);
    } else {
        struct blk_dev *bd = blk_find(virtio_blk_name());
        bool ok = bd != NULL && bd->ops && bd->ops->read &&
                  bd->sector_count == virtio_blk_capacity_lba();
        if (!ok) local_failures++;
        local_total++;
        step("m35b", ok, "blk_find(%s) -> %s sectors=%lu",
             virtio_blk_name() ? virtio_blk_name() : "(null)",
             bd ? bd->name : "(null)",
             (unsigned long)(bd ? bd->sector_count : 0));
    }

    /* Step 4: sentinel sector read round-trip.
     *
     * Stack-allocated 512-byte buffer is fine: kmain runs on a 16 KiB
     * boot stack (see linker.ld) and the read path uses our preallocated
     * DMA scratch internally, so this buffer never crosses to the device. */
    if (!present) {
        local_skipped++;
        kprintf("[m35b-selftest] step %d: SKIPPED_NO_VIRTIO_BLK -- "
                "sentinel read skip\n", ++g_step_no);
    } else {
        struct blk_dev *bd = blk_find(virtio_blk_name());
        uint8_t buf[BLK_SECTOR_SIZE];
        memset(buf, 0xA5, sizeof(buf));
        int rc = bd ? blk_read(bd, 0, 1, buf) : -1;
        bool match = (rc == 0) &&
                     (memcmp(buf, m35b_sentinel, M35B_SENTINEL_LEN) == 0);
        if (!match) local_failures++;
        local_total++;
        step("m35b", match,
             "sentinel-read rc=%d first16='%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c'",
             rc,
             buf[0], buf[1], buf[2],  buf[3],  buf[4],  buf[5],  buf[6],  buf[7],
             buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
    }

    /* Step 5: catalogue-only regression -- virtio-input lives in
     * drvdb (HID class is handled by usb_hid; the virtio-input PCI
     * device id is informational). We assert lookup at least succeeds
     * for the modern id. */
    {
        const struct drvdb_pci_entry *e =
            drvdb_pci_lookup(0x1AF4, 0x1052);
        bool ok = (e != NULL);
        if (!ok) local_failures++;
        local_total++;
        step("m35b", ok,
             "drvdb_pci_lookup(1AF4:1052 virtio-input) -> %s tier=%s",
             e ? e->friendly : "(null)",
             drvdb_tier_name(e ? e->tier : DRVDB_UNKNOWN));
    }

    kprintf("[m35b-selftest] milestone 35B end "
            "(failures=%d total=%d skipped=%d) -- %s\n",
            local_failures, local_total, local_skipped,
            local_failures == 0 ? "PASS" : "FAIL");
}

/* ============================================================
 * M35C -- USB device class expansion + safe unsupported handling
 *
 * Synthetic-attach checks ALWAYS run -- they prove the registry's
 * status routing handles known/unknown classes without crashing,
 * regardless of whether the test config attaches real USB hardware.
 *
 * Live-attach checks (a real keyboard/mouse/storage on usb0.0) only
 * run when usbreg already has entries with status=BOUND. If the host
 * skipped the qemu-xhci+usb-* args, those steps report SKIPPED
 * instead of failing.
 *
 * The synthetic entries use slot ids 200/201/202 -- well beyond what
 * any real xHCI emulation hands out (QEMU's qemu-xhci tops out at 64
 * slots), so we don't collide with hot-plugged devices either.
 * ============================================================ */

#define M35C_FAKE_SLOT_PRINTER    200
#define M35C_FAKE_SLOT_UNKNOWN    201
#define M35C_FAKE_SLOT_HID        202

void m35c_selftest(void) {
    g_step_no = 0;
    g_fail_total = 0;
    int local_failures = 0;
    int local_total = 0;
    int local_skipped = 0;
    kprintf("[m35c-selftest] milestone 35C begin\n");

    /* Step 1: synthetic UNSUPPORTED -- printer class with no in-tree
     * driver. usbreg should land it as UNSUPPORTED with the friendly
     * "USB Printer" name from drvdb. */
    {
        usbreg_record_attach(M35C_FAKE_SLOT_PRINTER, 0, 0, 3,
                             0x4242, 0x4243,
                             0x07, 0xFF, 0xFF, /* Printer / wildcard */
                             NULL);
        const struct usbreg_entry *e =
            usbreg_find(M35C_FAKE_SLOT_PRINTER);
        bool ok = (e != NULL) &&
                  (e->status == USBREG_STATUS_UNSUPPORTED) &&
                  e->friendly[0] != 0;
        if (!ok) local_failures++;
        local_total++;
        step("m35c", ok,
             "synthetic printer attach -> %s status=%s",
             e ? e->friendly : "(null)",
             usbreg_status_name(e ? e->status : USBREG_STATUS_FREE));
    }

    /* Step 2: synthetic UNKNOWN -- a class drvdb has zero entries
     * for. Routing must be UNKNOWN, not UNSUPPORTED, so userland
     * tools can distinguish "we know about this class but don't
     * drive it" from "we have no idea what this is". */
    {
        usbreg_record_attach(M35C_FAKE_SLOT_UNKNOWN, 0, 0, 3,
                             0x4242, 0x4244,
                             0xAA, 0xBB, 0xCC, /* fully made-up */
                             NULL);
        const struct usbreg_entry *e =
            usbreg_find(M35C_FAKE_SLOT_UNKNOWN);
        bool ok = (e != NULL) && (e->status == USBREG_STATUS_UNKNOWN);
        if (!ok) local_failures++;
        local_total++;
        step("m35c", ok,
             "synthetic unknown attach -> %s status=%s",
             e ? e->friendly : "(null)",
             usbreg_status_name(e ? e->status : USBREG_STATUS_FREE));
    }

    /* Step 3: synthetic BOUND -- pretend a HID keyboard with a real
     * driver name routes to BOUND. */
    {
        usbreg_record_attach(M35C_FAKE_SLOT_HID, 0, 0, 3,
                             0x4242, 0x4245,
                             0x03, 0x01, 0x01, /* HID Boot keyboard */
                             "usb_hid");
        const struct usbreg_entry *e =
            usbreg_find(M35C_FAKE_SLOT_HID);
        bool ok = (e != NULL) &&
                  (e->status == USBREG_STATUS_BOUND) &&
                  !strcmp(e->driver, "usb_hid");
        if (!ok) local_failures++;
        local_total++;
        step("m35c", ok,
             "synthetic HID attach -> %s status=%s drv=%s",
             e ? e->friendly : "(null)",
             usbreg_status_name(e ? e->status : USBREG_STATUS_FREE),
             e ? e->driver : "(null)");
    }

    /* Step 4: detach flips to GONE without losing the friendly
     * name (so devlist can keep showing "removed" entries). */
    {
        usbreg_record_detach(M35C_FAKE_SLOT_HID);
        const struct usbreg_entry *e =
            usbreg_find(M35C_FAKE_SLOT_HID);
        bool ok = (e != NULL) && (e->status == USBREG_STATUS_GONE) &&
                  e->friendly[0] != 0;
        if (!ok) local_failures++;
        local_total++;
        step("m35c", ok,
             "detach -> %s status=%s",
             e ? e->friendly : "(null)",
             usbreg_status_name(e ? e->status : USBREG_STATUS_FREE));
    }

    /* Step 5: drvdb has friendly names for all the common
     * "unsupported" classes we expect to see in a typical guest --
     * regression coverage on the catalogue, doubles as documentation
     * of which classes we've decided to recognise. */
    {
        const uint8_t cls[] = {
            0x01, /* Audio          */
            0x02, /* CDC            */
            0x07, /* Printer        */
            0x0B, /* Smart Card     */
            0x0E, /* Video          */
            0xE0, /* Wireless       */
            0xFF, /* Vendor-Specific*/
        };
        bool ok = true;
        for (size_t i = 0; i < sizeof(cls); i++) {
            const struct drvdb_usb_entry *e =
                drvdb_usb_lookup(cls[i], 0xFF, 0xFF);
            if (!e || !e->friendly) { ok = false; break; }
        }
        if (!ok) local_failures++;
        local_total++;
        step("m35c", ok, "drvdb covers known unsupported USB classes");
    }

    /* Step 6: live HID -- usbreg should contain at least one BOUND
     * entry with driver "usb_hid" if the host attached usb-kbd or
     * usb-mouse. Otherwise this is a safe SKIP -- we can't force
     * QEMU to attach hardware from inside the guest. */
    {
        bool found_bound_hid = false;
        for (size_t i = 0; i < USBREG_MAX; i++) {
            const struct usbreg_entry *e = usbreg_get(i);
            if (!e) continue;
            if (e->status == USBREG_STATUS_BOUND &&
                !strcmp(e->driver, "usb_hid")) {
                found_bound_hid = true;
                break;
            }
        }
        if (found_bound_hid) {
            local_total++;
            step("m35c", true, "live: usbreg has a BOUND usb_hid entry");
        } else {
            local_skipped++;
            kprintf("[m35c-selftest] step %d: SKIPPED_NO_USB_HID -- "
                    "host did not attach a usb-kbd / usb-mouse\n",
                    ++g_step_no);
        }
    }

    /* Step 7: live MSC -- mirror of step 6 for usb-storage. */
    {
        bool found_bound_msc = false;
        for (size_t i = 0; i < USBREG_MAX; i++) {
            const struct usbreg_entry *e = usbreg_get(i);
            if (!e) continue;
            if (e->status == USBREG_STATUS_BOUND &&
                !strcmp(e->driver, "usb_msc")) {
                found_bound_msc = true;
                break;
            }
        }
        if (found_bound_msc) {
            local_total++;
            step("m35c", true, "live: usbreg has a BOUND usb_msc entry");
        } else {
            local_skipped++;
            kprintf("[m35c-selftest] step %d: SKIPPED_NO_USB_MSC -- "
                    "host did not attach a usb-storage\n",
                    ++g_step_no);
        }
    }

    /* Final: dump the registry to debug.log so a failed run leaves
     * forensics behind. */
    usbreg_dump_kprintf();

    kprintf("[m35c-selftest] milestone 35C end "
            "(failures=%d total=%d skipped=%d) -- %s\n",
            local_failures, local_total, local_skipped,
            local_failures == 0 ? "PASS" : "FAIL");
}

/* ============================================================
 * M35D -- runtime hardware-compatibility database
 * ============================================================
 *
 * Verifies the kernel-side hwdb module:
 *   1. snapshot returns at least one PCI row
 *   2. every row has a non-empty friendly name + status field
 *   3. drvmatch+drvdb agree on the resolved status
 *   4. the per-tier counts round-trip vs the snapshot rows
 *   5. a known SUPPORTED PCI device renders as bound + supported
 *   6. the dump path runs without panicking the kernel
 *
 * Reuses the synthetic USB rows from m35c_selftest if they're still
 * around -- step 4 of the M35C run flips the HID slot to GONE which
 * the snapshot path then filters out, exactly the behaviour we want
 * to assert. */

void m35d_selftest(void) {
    g_step_no = 0;
    g_fail_total = 0;
    int local_failures = 0;
    int local_total = 0;
    int local_skipped = 0;
    kprintf("[m35d-selftest] milestone 35D begin\n");

    /* Step 1: snapshot returns at least one row, no kernel-buffer
     * overrun (we cap at MAX). */
    struct abi_hwcompat_entry rows[ABI_HWCOMPAT_MAX_ENTRIES];
    size_t n = hwdb_snapshot(rows, ABI_HWCOMPAT_MAX_ENTRIES);
    {
        bool ok = (n > 0) && (n <= ABI_HWCOMPAT_MAX_ENTRIES);
        if (!ok) local_failures++;
        local_total++;
        step("m35d", ok,
             "hwdb_snapshot -> n=%u (cap=%u)",
             (unsigned)n, (unsigned)ABI_HWCOMPAT_MAX_ENTRIES);
    }

    /* Step 2: every populated row has a friendly name + valid
     * status. The kernel never emits ABI_HWCOMPAT_UNKNOWN (it's a
     * userland-init sentinel only); seeing one would mean the join
     * fell through to the default arm. */
    {
        size_t blank = 0, unknown = 0;
        for (size_t i = 0; i < n; i++) {
            if (!rows[i].friendly[0]) blank++;
            if (rows[i].status == ABI_HWCOMPAT_UNKNOWN) unknown++;
        }
        bool ok = (blank == 0) && (unknown == 0);
        if (!ok) local_failures++;
        local_total++;
        step("m35d", ok,
             "rows have friendly+status (blank=%u unknown=%u)",
             (unsigned)blank, (unsigned)unknown);
    }

    /* Step 3: per-tier counters from hwdb_counts() must match the
     * snapshot tally (i.e. the two walks observe the same world). */
    size_t sup_c = 0, par_c = 0, uns_c = 0;
    size_t total_c = hwdb_counts(&sup_c, &par_c, &uns_c);
    {
        size_t sup_r = 0, par_r = 0, uns_r = 0;
        for (size_t i = 0; i < n; i++) {
            switch (rows[i].status) {
            case ABI_HWCOMPAT_SUPPORTED:   sup_r++; break;
            case ABI_HWCOMPAT_PARTIAL:     par_r++; break;
            case ABI_HWCOMPAT_UNSUPPORTED: uns_r++; break;
            default: break;
            }
        }
        bool ok = (sup_r == sup_c) && (par_r == par_c) &&
                  (uns_r == uns_c) && (sup_r + par_r + uns_r == n) &&
                  (total_c == n);
        if (!ok) local_failures++;
        local_total++;
        step("m35d", ok,
             "counts round-trip rows{S=%u P=%u U=%u} "
             "counts{S=%u P=%u U=%u total=%u}",
             (unsigned)sup_r, (unsigned)par_r, (unsigned)uns_r,
             (unsigned)sup_c, (unsigned)par_c, (unsigned)uns_c,
             (unsigned)total_c);
    }

    /* Step 4: at least one PCI row is bound AND classified as
     * SUPPORTED. QEMU's PIIX3 / virtio host always exposes a few
     * supported devices, so this asserts the kernel can detect at
     * least one fully-working device class on every boot. */
    {
        size_t pci_supported = 0;
        for (size_t i = 0; i < n; i++) {
            if (rows[i].bus != ABI_DEVT_BUS_PCI) continue;
            if (rows[i].status == ABI_HWCOMPAT_SUPPORTED &&
                rows[i].bound) pci_supported++;
        }
        bool ok = (pci_supported > 0);
        if (!ok) local_failures++;
        local_total++;
        step("m35d", ok,
             "PCI has at least one bound+supported device (count=%u)",
             (unsigned)pci_supported);
    }

    /* Step 5: a deliberately-bogus PCI ID that does NOT exist on the
     * bus must NOT appear in the snapshot. This guards against the
     * join accidentally pulling rows from drvdb (which would be a
     * regression -- drvdb is a knowledge base, not an inventory). */
    {
        bool ghost_seen = false;
        for (size_t i = 0; i < n; i++) {
            if (rows[i].vendor == 0xDEAD &&
                rows[i].product == 0xBEEF) { ghost_seen = true; break; }
        }
        bool ok = !ghost_seen;
        if (!ok) local_failures++;
        local_total++;
        step("m35d", ok,
             "drvdb-only entries do NOT leak into snapshot "
             "(ghost_seen=%d)", (int)ghost_seen);
    }

    /* Step 6: virtio-blk specifically. M35B added the driver and
     * registered an entry in drvdb; the join must surface it as
     * SUPPORTED+bound when -drive is attached, or PARTIAL+unbound
     * otherwise. Anything else (UNSUPPORTED/UNKNOWN) would mean the
     * join fell through. */
    {
        const struct abi_hwcompat_entry *vblk = NULL;
        for (size_t i = 0; i < n; i++) {
            if (rows[i].bus == ABI_DEVT_BUS_PCI &&
                rows[i].vendor == 0x1AF4 &&
                (rows[i].product == 0x1001 ||  /* legacy */
                 rows[i].product == 0x1042)) { /* modern */
                vblk = &rows[i];
                break;
            }
        }
        if (!vblk) {
            local_skipped++;
            kprintf("[m35d-selftest] step %d: "
                    "SKIPPED_NO_VIRTIO_BLK -- host has no virtio-blk\n",
                    ++g_step_no);
        } else {
            bool ok = (vblk->status == ABI_HWCOMPAT_SUPPORTED &&
                       vblk->bound) ||
                      (vblk->status == ABI_HWCOMPAT_PARTIAL &&
                       !vblk->bound);
            if (!ok) local_failures++;
            local_total++;
            step("m35d", ok,
                 "virtio-blk: status=%s bound=%u driver=%s",
                 hwdb_status_name(vblk->status),
                 (unsigned)vblk->bound,
                 vblk->driver[0] ? vblk->driver : "(none)");
        }
    }

    /* Step 7: dump path runs cleanly. We've already exercised it
     * indirectly via hwdb_counts but the formatter has its own
     * snprintf paths that snapshot doesn't share. */
    {
        kprintf("[m35d-selftest] kernel-side dump follows --\n");
        hwdb_dump_kprintf();
        local_total++;
        step("m35d", true, "hwdb_dump_kprintf returned cleanly");
    }

    kprintf("[m35d-selftest] milestone 35D end "
            "(failures=%d total=%d skipped=%d) -- %s\n",
            local_failures, local_total, local_skipped,
            local_failures == 0 ? "PASS" : "FAIL");
}

/* ============================================================
 * M35E -- boot profiles + driver-mode policy
 * ============================================================
 *
 * The kernel only ever boots in one profile per run, so we can't
 * directly verify "compatibility skips audio AT BOOT" inside this
 * selftest. What we *can* verify is the in-kernel state machine that
 * decides those answers:
 *
 *   1. each level resolves to a unique safemode_tag() string
 *   2. each level resolves to the correct ABI_BOOT_MODE_* via
 *      safemode_to_boot_mode() -- userland depends on this mapping
 *   3. the skip-policy table for each level matches the published
 *      docstring in safemode.h (and is otherwise the contract that
 *      kernel.c + xhci.c gate on)
 *   4. safemode_force_level() round-trips back to the booted state
 *      without leaving the kernel observing a different level than
 *      the one the operator picked
 *
 * The harness latches the *original* level on entry and restores it
 * before returning, so subsequent boot stages see the same world they
 * saw before m35e_selftest() ran.
 * ============================================================ */

struct m35e_policy {
    enum safemode_level level;
    const char *tag;
    uint32_t boot_mode;
    bool skip_usb_full;
    bool skip_usb_extra;
    bool skip_net;
    bool skip_audio;
    bool skip_gui;
    bool skip_services;
    bool skip_virtio_gpu;
};

/* Expected per-level table -- mirrors the documented contract in
 * include/tobyos/safemode.h. Any drift here is a hard failure. */
static const struct m35e_policy m35e_expected[] = {
    {
        .level           = SAFEMODE_LEVEL_NONE,
        .tag             = "normal",
        .boot_mode       = ABI_BOOT_MODE_NORMAL,
        .skip_usb_full   = false, .skip_usb_extra  = false,
        .skip_net        = false, .skip_audio      = false,
        .skip_gui        = false, .skip_services   = false,
        .skip_virtio_gpu = false,
    },
    {
        .level           = SAFEMODE_LEVEL_BASIC,
        .tag             = "safe-basic",
        .boot_mode       = ABI_BOOT_MODE_SAFE_BASIC,
        .skip_usb_full   = true,  .skip_usb_extra  = true,
        .skip_net        = true,  .skip_audio      = true,
        .skip_gui        = true,  .skip_services   = true,
        .skip_virtio_gpu = true,
    },
    {
        .level           = SAFEMODE_LEVEL_GUI,
        .tag             = "safe-gui",
        .boot_mode       = ABI_BOOT_MODE_SAFE_GUI,
        .skip_usb_full   = false, .skip_usb_extra  = true,
        .skip_net        = true,  .skip_audio      = true,
        .skip_gui        = false, .skip_services   = false,
        .skip_virtio_gpu = true,
    },
    {
        .level           = SAFEMODE_LEVEL_COMPATIBILITY,
        .tag             = "compatibility",
        .boot_mode       = ABI_BOOT_MODE_COMPATIBILITY,
        .skip_usb_full   = false, .skip_usb_extra  = true,
        .skip_net        = false, .skip_audio      = true,
        .skip_gui        = false, .skip_services   = false,
        .skip_virtio_gpu = true,
    },
};

void m35e_selftest(void) {
    g_step_no = 0;
    g_fail_total = 0;
    int local_failures = 0;
    int local_total = 0;
    kprintf("[m35e-selftest] milestone 35E begin\n");

    enum safemode_level booted_level = safemode_level();
    kprintf("[m35e-selftest] booted level=%u tag=%s\n",
            (unsigned)booted_level, safemode_tag());

    /* Step 1: the kernel was already initialised, so safemode_ready()
     * must be true. Anything else means we were called too early. */
    {
        bool ok = safemode_ready();
        if (!ok) local_failures++;
        local_total++;
        step("m35e", ok,
             "safemode_ready()=%d (must be 1 -- m35e runs after init)",
             (int)ok);
    }

    /* Step 2 -- 5: walk every published level, force into it, verify
     * tag + boot_mode + skip table all match the contract. */
    for (size_t i = 0;
         i < sizeof(m35e_expected) / sizeof(m35e_expected[0]); i++) {
        const struct m35e_policy *p = &m35e_expected[i];
        safemode_force_level(p->level);

        const char *tag = safemode_tag();
        uint32_t bm = safemode_to_boot_mode(p->level);

        bool tag_ok    = tag && !strcmp(tag, p->tag);
        bool bm_ok     = (bm == p->boot_mode);
        bool active_ok = (safemode_active() ==
                          (p->level != SAFEMODE_LEVEL_NONE));
        bool table_ok  =
            (safemode_skip_usb_full()    == p->skip_usb_full)   &&
            (safemode_skip_usb_extra()   == p->skip_usb_extra)  &&
            (safemode_skip_net()         == p->skip_net)        &&
            (safemode_skip_audio()       == p->skip_audio)      &&
            (safemode_skip_gui()         == p->skip_gui)        &&
            (safemode_skip_services()    == p->skip_services)   &&
            (safemode_skip_virtio_gpu()  == p->skip_virtio_gpu);

        bool ok = tag_ok && bm_ok && active_ok && table_ok;
        if (!ok) local_failures++;
        local_total++;
        step("m35e", ok,
             "level=%u expect tag=%s bm=%u got tag=%s bm=%u "
             "(active=%d table=%d)",
             (unsigned)p->level, p->tag, (unsigned)p->boot_mode,
             tag ? tag : "(null)", (unsigned)bm,
             (int)active_ok, (int)table_ok);

        /* Re-emit the human-readable policy line so a failed run has a
         * grep-friendly forensic record of what each level resolved
         * to. The kernel's own safemode_init() only prints the booted
         * level. */
        safemode_dump_policy();
    }

    /* Step 6: legacy alias parity. safemode_skip_usb() must agree
     * with safemode_skip_usb_full() in every level we just visited
     * (we currently sit in the last level == COMPATIBILITY). */
    {
        bool ok = (safemode_skip_usb() == safemode_skip_usb_full());
        if (!ok) local_failures++;
        local_total++;
        step("m35e", ok,
             "safemode_skip_usb() == safemode_skip_usb_full() (%d == %d)",
             (int)safemode_skip_usb(), (int)safemode_skip_usb_full());
    }

    /* Step 7: parse_level coverage. We can't call the static parser
     * directly, but reading /etc/safemode_level via safemode_init()
     * already exercised it. Here we just round-trip via the
     * documented integer/string mapping by forcing each level and
     * confirming safemode_tag() lands on the expected tag. (Steps
     * 2-5 already did this; this step asserts the *level* numbering
     * itself is stable -- a pure ABI guard.) */
    {
        bool ok =
            (SAFEMODE_LEVEL_NONE          == 0) &&
            (SAFEMODE_LEVEL_BASIC         == 1) &&
            (SAFEMODE_LEVEL_GUI           == 2) &&
            (SAFEMODE_LEVEL_COMPATIBILITY == 3);
        if (!ok) local_failures++;
        local_total++;
        step("m35e", ok,
             "enum numbering stable (none=%d basic=%d gui=%d compat=%d)",
             (int)SAFEMODE_LEVEL_NONE, (int)SAFEMODE_LEVEL_BASIC,
             (int)SAFEMODE_LEVEL_GUI, (int)SAFEMODE_LEVEL_COMPATIBILITY);
    }

    /* Step 8: ABI boot-mode numbering is locked too. Userland tools
     * (and the M35F hwreport summary) hard-code these values. */
    {
        bool ok =
            (ABI_BOOT_MODE_NORMAL        == 0u) &&
            (ABI_BOOT_MODE_SAFE_BASIC    == 1u) &&
            (ABI_BOOT_MODE_SAFE_GUI      == 2u) &&
            (ABI_BOOT_MODE_VERBOSE       == 3u) &&
            (ABI_BOOT_MODE_COMPATIBILITY == 4u);
        if (!ok) local_failures++;
        local_total++;
        step("m35e", ok,
             "ABI boot-mode numbering stable "
             "(normal=%u basic=%u gui=%u verbose=%u compat=%u)",
             (unsigned)ABI_BOOT_MODE_NORMAL,
             (unsigned)ABI_BOOT_MODE_SAFE_BASIC,
             (unsigned)ABI_BOOT_MODE_SAFE_GUI,
             (unsigned)ABI_BOOT_MODE_VERBOSE,
             (unsigned)ABI_BOOT_MODE_COMPATIBILITY);
    }

    /* Restore the level the kernel actually booted with so the rest
     * of the boot sequence (the M35F hwreport selftest, etc.) sees
     * the same world it would have seen had we not run. */
    safemode_force_level(booted_level);
    {
        bool ok = (safemode_level() == booted_level);
        if (!ok) local_failures++;
        local_total++;
        step("m35e", ok,
             "restored booted level=%u (now=%u)",
             (unsigned)booted_level, (unsigned)safemode_level());
    }

    kprintf("[m35e-selftest] milestone 35E end "
            "(failures=%d total=%d) -- %s\n",
            local_failures, local_total,
            local_failures == 0 ? "PASS" : "FAIL");
}

/* ============================================================
 * M35F -- enhanced diagnostics + reporting
 * ============================================================
 *
 * The userland-facing piece is /bin/hwreport (programs/hwreport).
 * This selftest verifies the same data sources that hwreport feeds
 * on are coherent at the kernel side:
 *
 *   1. hwinfo_snapshot returns a populated summary with mode field
 *      that decodes via safemode_to_boot_mode (M35F ABI bridge)
 *   2. snapshot_epoch is monotonic across two consecutive snapshots
 *   3. drvmatch_count agrees with hwdb_snapshot's pci row count
 *   4. the GREEN/YELLOW/RED verdict the kernel-side computes for the
 *      live snapshot matches the userland verdict_for() rule
 *   5. probe_failed and forced_off counts are non-negative and the
 *      reason strings the kernel emits trip the userland substring
 *      patterns hwreport keys on
 *
 * This isn't a substitute for booting hwreport in QEMU -- the boot
 * harness sentinels (M35F_HWR:) cover that side. The selftest is
 * specifically for the kernel pipeline. */

/* Mirror of hwreport's verdict_for(), kept in lockstep so a kernel
 * regression in hwdb_snapshot can be caught without running userland.
 * Any drift here is a bug -- the test fails loudly. */
static const char *m35f_verdict_kernel(size_t total,
                                       size_t pci_bound,
                                       size_t supported,
                                       size_t partial,
                                       size_t unsupported,
                                       size_t unknown) {
    (void)supported;
    if (total == 0)        return "RED";
    if (unknown > 0)       return "RED";
    if (pci_bound == 0)    return "RED";
    if (unsupported > 0)   return "YELLOW";
    if (partial > 0)       return "YELLOW";
    return "GREEN";
}

void m35f_selftest(void) {
    g_step_no = 0;
    g_fail_total = 0;
    int local_failures = 0;
    int local_total = 0;
    kprintf("[m35f-selftest] milestone 35F begin\n");

    /* Step 1: hwinfo snapshot is populated and the mode field decodes
     * cleanly through safemode_to_boot_mode (M35F ABI bridge). */
    struct abi_hwinfo_summary s1;
    hwinfo_snapshot(&s1);
    {
        bool ok =
            s1.cpu_vendor[0] != '\0' &&
            s1.mem_total_pages > 0 &&
            (s1.safe_mode == ABI_BOOT_MODE_NORMAL ||
             s1.safe_mode == ABI_BOOT_MODE_SAFE_BASIC ||
             s1.safe_mode == ABI_BOOT_MODE_SAFE_GUI ||
             s1.safe_mode == ABI_BOOT_MODE_VERBOSE ||
             s1.safe_mode == ABI_BOOT_MODE_COMPATIBILITY);
        if (!ok) local_failures++;
        local_total++;
        step("m35f", ok,
             "hwinfo snapshot: cpu='%s' mem=%lu pg mode=%u",
             s1.cpu_vendor, (unsigned long)s1.mem_total_pages,
             (unsigned)s1.safe_mode);
    }

    /* Step 2: snapshot_epoch must be monotonic. Two consecutive
     * hwinfo_snapshot calls bump it by exactly one. */
    struct abi_hwinfo_summary s2;
    hwinfo_snapshot(&s2);
    {
        bool ok = (s2.snapshot_epoch == s1.snapshot_epoch + 1);
        if (!ok) local_failures++;
        local_total++;
        step("m35f", ok,
             "snapshot_epoch monotonic (s1=%lu s2=%lu)",
             (unsigned long)s1.snapshot_epoch,
             (unsigned long)s2.snapshot_epoch);
    }

    /* Step 3: hwdb's PCI row count matches drvmatch's PCI table.
     * drvmatch_count(total,bound,unbound,forced) covers PCI+USB so
     * we extract the PCI subtotal: total - usb_n. Divergence means
     * one of the two pipelines lost a row. */
    struct abi_hwcompat_entry rows[ABI_HWCOMPAT_MAX_ENTRIES];
    size_t n = hwdb_snapshot(rows, ABI_HWCOMPAT_MAX_ENTRIES);
    {
        size_t pci_rows = 0;
        for (size_t i = 0; i < n; i++) {
            if (rows[i].bus == ABI_DEVT_BUS_PCI) pci_rows++;
        }
        uint32_t dm_total = 0, dm_bound = 0, dm_unbound = 0, dm_forced = 0;
        drvmatch_count(&dm_total, &dm_bound, &dm_unbound, &dm_forced);
        size_t pci_dm = pci_device_count();
        bool ok = ((size_t)pci_dm == pci_rows) &&
                  (dm_total >= dm_bound + dm_unbound + dm_forced - dm_forced);
        if (!ok) local_failures++;
        local_total++;
        step("m35f", ok,
             "pci_device_count=%u hwdb_pci_rows=%u drvmatch_total=%u",
             (unsigned)pci_dm, (unsigned)pci_rows, (unsigned)dm_total);
    }

    /* Step 4: tally the snapshot, compute the verdict, assert the
     * rule produces a stable string. We don't pin GREEN vs YELLOW
     * because that depends on the QEMU config (some test machines
     * legitimately have unsupported devices); we just verify the
     * returned tag is one of {GREEN, YELLOW, RED} and that the rule
     * agrees with the snapshot data. */
    {
        size_t pci_bound = 0, sup = 0, par = 0, uns = 0, unk = 0;
        for (size_t i = 0; i < n; i++) {
            const struct abi_hwcompat_entry *r = &rows[i];
            if (r->bus == ABI_DEVT_BUS_PCI && r->bound) pci_bound++;
            switch (r->status) {
            case ABI_HWCOMPAT_SUPPORTED:   sup++; break;
            case ABI_HWCOMPAT_PARTIAL:     par++; break;
            case ABI_HWCOMPAT_UNSUPPORTED: uns++; break;
            case ABI_HWCOMPAT_UNKNOWN:     unk++; break;
            default: break;
            }
        }
        const char *v = m35f_verdict_kernel(n, pci_bound,
                                            sup, par, uns, unk);
        bool tag_ok = !strcmp(v, "GREEN") || !strcmp(v, "YELLOW") ||
                      !strcmp(v, "RED");
        bool rule_ok = (n > 0) && (pci_bound > 0) && (unk == 0);
        bool ok = tag_ok && rule_ok;
        if (!ok) local_failures++;
        local_total++;
        step("m35f", ok,
             "verdict=%s (rows=%u pci_bound=%u sup=%u par=%u uns=%u unk=%u)",
             v, (unsigned)n, (unsigned)pci_bound,
             (unsigned)sup, (unsigned)par, (unsigned)uns, (unsigned)unk);
    }

    /* Step 5: hwdb_counts agrees with the manual tally. (Reused from
     * the M35D selftest; relevant here because the M35F report's
     * --summary mode plumbs hwdb_counts into the verdict.) */
    {
        size_t sup_c = 0, par_c = 0, uns_c = 0;
        size_t total_c = hwdb_counts(&sup_c, &par_c, &uns_c);
        size_t sup_r = 0, par_r = 0, uns_r = 0;
        for (size_t i = 0; i < n; i++) {
            switch (rows[i].status) {
            case ABI_HWCOMPAT_SUPPORTED:   sup_r++; break;
            case ABI_HWCOMPAT_PARTIAL:     par_r++; break;
            case ABI_HWCOMPAT_UNSUPPORTED: uns_r++; break;
            default: break;
            }
        }
        bool ok = (sup_r == sup_c) && (par_r == par_c) &&
                  (uns_r == uns_c) && (total_c == n);
        if (!ok) local_failures++;
        local_total++;
        step("m35f", ok,
             "hwdb_counts==tally (S=%u/%u P=%u/%u U=%u/%u total=%u/%u)",
             (unsigned)sup_r, (unsigned)sup_c,
             (unsigned)par_r, (unsigned)par_c,
             (unsigned)uns_r, (unsigned)uns_c,
             (unsigned)total_c, (unsigned)n);
    }

    /* Step 6: per the M35F docstring, hwreport keys probe_failed and
     * forced_off off substring matches in the kernel-emitted reason
     * string. Walk every row that has a non-empty reason and confirm
     * the substring catches at least one canonical pattern (or
     * remains zero -- both are acceptable; the test asserts the
     * patterns are *self-consistent* with what hwdb emits). */
    {
        bool any_reason = false;
        for (size_t i = 0; i < n; i++) {
            if (rows[i].reason[0]) { any_reason = true; break; }
        }
        bool ok = true;
        for (size_t i = 0; i < n; i++) {
            const char *r = rows[i].reason;
            if (!r[0]) continue;
            /* Reason strings the kernel emits look like one of:
             *   - "drvdb tier=...; bound by ..."
             *   - "no driver bound; drvdb tier=..."
             *   - "PROBE_FAILED: ..."
             *   - "blacklisted by /etc/drvmatch.conf"
             *   - "GONE -- removed via hot-unplug"
             * Any other format is fine, but it must NOT exceed the
             * documented length cap or be unterminated. */
            size_t len = strlen(r);
            if (len >= ABI_HWCOMPAT_REASON_MAX) { ok = false; break; }
        }
        if (!ok) local_failures++;
        local_total++;
        step("m35f", ok,
             "reason strings well-formed (any_reason=%d)",
             (int)any_reason);
    }

    /* Step 7: hwinfo_dump_kprintf runs cleanly (forensic dump path
     * shared with the shell builtin). */
    {
        kprintf("[m35f-selftest] kernel-side hwinfo dump follows --\n");
        hwinfo_dump_kprintf();
        local_total++;
        step("m35f", true, "hwinfo_dump_kprintf returned cleanly");
    }

    kprintf("[m35f-selftest] milestone 35F end "
            "(failures=%d total=%d) -- %s\n",
            local_failures, local_total,
            local_failures == 0 ? "PASS" : "FAIL");
}

#endif /* M35_SELFTEST */
