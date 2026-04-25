/* devtest.c -- M26A peripheral test harness (kernel side).
 *
 * Two responsibilities:
 *
 *   (1) Enumerate every introspectable subsystem into a flat array of
 *       struct abi_dev_info records. The walk order is FROZEN as part
 *       of the ABI:
 *
 *         PCI -> USB -> BLK -> INPUT -> AUDIO -> BATTERY -> HUB
 *
 *       Inside each bus, secondary order is "registration order"
 *       (PCI: enumerated bus/slot/fn; BLK: blk_register order; etc.).
 *
 *   (2) Host a registry of named driver self-tests. Each entry is just
 *       (name, fn). Subsystems call devtest_register("xhci", ...) once
 *       at boot; userland exercises the same callbacks via SYS_DEV_TEST.
 *
 * There is intentionally no kmalloc here: the registry is a small
 * fixed array (DEVT_TESTS_MAX = 32) and the event ring is a power-of-
 * two ring of compile-time size (DEVT_EVENTS_RING = 32). Empty
 * subsystems just don't register anything. */

#include <tobyos/devtest.h>
#include <tobyos/abi/abi.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#include <tobyos/pci.h>
#include <tobyos/blk.h>
#include <tobyos/xhci.h>
#include <tobyos/usb.h>
#include <tobyos/usb_hub.h>
#include <tobyos/usb_hid.h>
#include <tobyos/usb_msc.h>
#include <tobyos/audio_hda.h>
#include <tobyos/acpi_bat.h>
#include <tobyos/hotplug.h>
#include <tobyos/keyboard.h>
#include <tobyos/mouse.h>
#include <tobyos/vfs.h>
#include <tobyos/fat32.h>
#include <tobyos/display.h>

/* ===== self-test registry ===== */

#define DEVT_TESTS_MAX  32
#define DEVT_NAME_BUF   16    /* short name + NUL fits comfortably here */

struct devt_entry {
    char       name[DEVT_NAME_BUF];
    devtest_fn fn;
    bool       used;
};

static struct devt_entry g_tests[DEVT_TESTS_MAX];
static int               g_test_count;

/* ===== event ring (M26A: stub; M26C will write into it) ===== */

#define DEVT_EVENTS_RING  32
static struct devtest_event g_events[DEVT_EVENTS_RING];
static volatile uint32_t    g_event_head;   /* producer */
static volatile uint32_t    g_event_tail;   /* consumer */
static volatile uint64_t    g_event_seq;    /* monotonic id */

/* ===== name helpers ===== */

static void devt_strlcpy(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t n = 0;
    while (src && src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

static int devt_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

/* ============================================================
 * registry API
 * ============================================================ */

void devtest_register(const char *name, devtest_fn fn) {
    if (!name || !fn) return;

    /* Replace if name already there. */
    for (int i = 0; i < DEVT_TESTS_MAX; i++) {
        if (g_tests[i].used && devt_streq(g_tests[i].name, name)) {
            g_tests[i].fn = fn;
            return;
        }
    }
    /* Otherwise insert into first free slot. */
    for (int i = 0; i < DEVT_TESTS_MAX; i++) {
        if (!g_tests[i].used) {
            devt_strlcpy(g_tests[i].name, name, DEVT_NAME_BUF);
            g_tests[i].fn   = fn;
            g_tests[i].used = true;
            g_test_count++;
            return;
        }
    }
    kprintf("[devtest] WARN: registry full, dropping test '%s'\n", name);
}

int devtest_run(const char *name, char *msg, size_t cap) {
    if (cap > 0 && msg) msg[0] = '\0';
    if (!name) {
        if (msg) ksnprintf(msg, cap, "no test name supplied");
        return -ABI_EINVAL;
    }
    for (int i = 0; i < DEVT_TESTS_MAX; i++) {
        if (g_tests[i].used && devt_streq(g_tests[i].name, name)) {
            return g_tests[i].fn(msg, cap);
        }
    }
    if (msg) ksnprintf(msg, cap, "no such test: %s", name);
    return -ABI_ENOENT;
}

int devtest_for_each(devtest_walk_cb cb, void *cookie) {
    if (!cb) return 0;
    int n = 0;
    char msg[ABI_DEVT_MSG_MAX];
    for (int i = 0; i < DEVT_TESTS_MAX; i++) {
        if (!g_tests[i].used) continue;
        msg[0] = '\0';
        int rc = g_tests[i].fn(msg, sizeof msg);
        cb(g_tests[i].name, rc, msg, cookie);
        n++;
    }
    return n;
}

/* ============================================================
 * device enumeration
 * ============================================================
 *
 * The walk order matters: tests + tools render exactly what the
 * kernel writes here. PCI first because every other bus is rooted
 * on top of it; USB next because USB devices are "logical children"
 * of the PCI xHCI; BLK after USB so USB-MSC disks appear right after
 * the USB device that produced them; INPUT before AUDIO/BATTERY for
 * symmetry with /dev/input on Linux.
 */

static int devt_emit_pci(devtest_dev_info_t *out, int cap, int idx) {
    int n = 0;
    size_t total = pci_device_count();
    for (size_t i = 0; i < total && idx + n < cap; i++) {
        struct pci_dev *p = pci_device_at(i);
        if (!p) continue;

        devtest_dev_info_t *r = &out[idx + n];
        memset(r, 0, sizeof *r);
        r->bus        = ABI_DEVT_BUS_PCI;
        r->status     = ABI_DEVT_PRESENT |
                        (p->driver ? ABI_DEVT_BOUND : 0);
        r->vendor     = p->vendor;
        r->device     = p->device;
        r->class_code = p->class_code;
        r->subclass   = p->subclass;
        r->prog_if    = p->prog_if;
        r->index      = (uint8_t)i;
        ksnprintf(r->name, ABI_DEVT_NAME_MAX, "%02x:%02x.%u",
                  (unsigned)p->bus, (unsigned)p->slot, (unsigned)p->fn);
        if (p->driver && p->driver->name) {
            devt_strlcpy(r->driver, p->driver->name, ABI_DEVT_DRIVER_MAX);
        }
        ksnprintf(r->extra, ABI_DEVT_EXTRA_MAX,
                  "%04x:%04x cls=%02x.%02x.%02x irq=%u",
                  (unsigned)p->vendor, (unsigned)p->device,
                  (unsigned)p->class_code, (unsigned)p->subclass,
                  (unsigned)p->prog_if, (unsigned)p->irq_line);
        n++;
    }
    return n;
}

static int devt_emit_usb(devtest_dev_info_t *out, int cap, int idx) {
    int n = 0;
    int total = xhci_introspect_count();
    for (int i = 0; i < total && idx + n < cap; i++) {
        if (xhci_introspect_at(i, &out[idx + n]) > 0) n++;
    }
    return n;
}

/* M26E: figure out whether `dev` (or any partition slice descended
 * from it, if `dev` is a disk) is currently the backing store of an
 * active VFS mount. Returns the mount-point (or "" if none). */
struct blk_mount_walk {
    struct blk_dev *target;
    char            mount[64];
    bool            found;
};

static bool blk_mount_walk_cb(const char *mount_point,
                              const struct vfs_ops *ops,
                              void *mount_data,
                              void *cookie) {
    struct blk_mount_walk *w = (struct blk_mount_walk *)cookie;
    if (w->found || !mount_point || !ops) return true;
    if (ops != &fat32_ops) return true;
    struct blk_dev *bd = fat32_blkdev_of(mount_data);
    if (!bd) return true;
    if (bd != w->target && bd->parent != w->target) return true;
    size_t cap = sizeof(w->mount) - 1;
    size_t n   = 0;
    while (n < cap && mount_point[n]) n++;
    memcpy(w->mount, mount_point, n);
    w->mount[n] = '\0';
    w->found    = true;
    return false;
}

static int devt_emit_blk(devtest_dev_info_t *out, int cap, int idx) {
    int n = 0;
    size_t total = blk_count();
    for (size_t i = 0; i < total && idx + n < cap; i++) {
        struct blk_dev *b = blk_get(i);
        if (!b) continue;
        devtest_dev_info_t *r = &out[idx + n];
        memset(r, 0, sizeof *r);
        r->bus    = ABI_DEVT_BUS_BLK;
        /* M26E: gone disks are still PRESENT (registered) but no
         * longer BOUND -- userland uses the absence of BOUND to mean
         * "I/O will return EIO". */
        r->status = ABI_DEVT_PRESENT | (b->gone ? 0u : ABI_DEVT_BOUND);
        r->index  = (uint8_t)i;
        devt_strlcpy(r->name, b->name ? b->name : "blk?", ABI_DEVT_NAME_MAX);

        const char *drv = "blk";
        switch (b->class) {
        case BLK_CLASS_DISK:      drv = "disk";       break;
        case BLK_CLASS_PARTITION: drv = "partition";  break;
        case BLK_CLASS_WRAPPER:   drv = "wrapper";    break;
        }
        devt_strlcpy(r->driver, drv, ABI_DEVT_DRIVER_MAX);

        struct blk_mount_walk w = { .target = b, .mount = {0}, .found = false };
        vfs_iter_mounts(blk_mount_walk_cb, &w);
        if (w.found) {
            r->status |= ABI_DEVT_ACTIVE;
        }

        ksnprintf(r->extra, ABI_DEVT_EXTRA_MAX,
                  "sectors=%lu (%lu KiB) class=%u%s%s%s",
                  (unsigned long)b->sector_count,
                  (unsigned long)((b->sector_count * BLK_SECTOR_SIZE) >> 10),
                  (unsigned)b->class,
                  b->gone ? " gone" : "",
                  w.found ? " mount=" : "",
                  w.found ? w.mount  : "");
        n++;
    }
    return n;
}

/* PS/2 input is always synthesised as two records (kbd + mouse) when
 * cap allows. Both are unconditionally PRESENT|BOUND on the
 * platforms we target: Limine boots us on legacy x86 where the i8042
 * is wired up and our IRQ1/IRQ12 handlers are installed at boot.
 *
 * After PS/2, M26D appends one record per active USB HID device so
 * downstream tooling sees PS/2 + USB HID through the same INPUT bus
 * iteration. Order is FROZEN: PS/2 kbd, PS/2 mouse, USB HID slots in
 * pool order. */
static int devt_emit_input(devtest_dev_info_t *out, int cap, int idx) {
    int n = 0;
    if (idx + n < cap) {
        devtest_dev_info_t *r = &out[idx + n];
        memset(r, 0, sizeof *r);
        r->bus        = ABI_DEVT_BUS_INPUT;
        r->status     = ABI_DEVT_PRESENT | ABI_DEVT_BOUND |
                        (kbd_chars_dispatched() ? ABI_DEVT_ACTIVE : 0u);
        r->index      = 0;
        r->class_code = 0x09;        /* HID class shape, for grouping  */
        r->subclass   = 0x01;        /* boot kbd */
        devt_strlcpy(r->name,   "kbd0",     ABI_DEVT_NAME_MAX);
        devt_strlcpy(r->driver, "ps2_kbd",  ABI_DEVT_DRIVER_MAX);
        ksnprintf(r->extra, ABI_DEVT_EXTRA_MAX,
                  "PS/2 IRQ1 chars=%lu irqs=%lu caps=%d",
                  (unsigned long)kbd_chars_dispatched(),
                  (unsigned long)kbd_irqs_total(),
                  kbd_caps_state() ? 1 : 0);
        n++;
    }
    if (idx + n < cap) {
        devtest_dev_info_t *r = &out[idx + n];
        memset(r, 0, sizeof *r);
        r->bus        = ABI_DEVT_BUS_INPUT;
        r->status     = ABI_DEVT_PRESENT | ABI_DEVT_BOUND |
                        (mouse_events_total() ? ABI_DEVT_ACTIVE : 0u);
        r->index      = 1;
        r->class_code = 0x09;
        r->subclass   = 0x02;        /* boot mouse */
        devt_strlcpy(r->name,   "mouse0",   ABI_DEVT_NAME_MAX);
        devt_strlcpy(r->driver, "ps2_mouse", ABI_DEVT_DRIVER_MAX);
        ksnprintf(r->extra, ABI_DEVT_EXTRA_MAX,
                  "PS/2 IRQ12 events=%lu clicks=%lu btn=0x%02x",
                  (unsigned long)mouse_events_total(),
                  (unsigned long)mouse_btn_press_total(),
                  (unsigned)mouse_last_buttons());
        n++;
    }
    /* M26D: append one record per active USB HID device. */
    int hid_total = usb_hid_count();
    for (int i = 0; i < hid_total && idx + n < cap; i++) {
        if (usb_hid_introspect_at(i, &out[idx + n])) n++;
    }
    return n;
}

static int devt_emit_audio(devtest_dev_info_t *out, int cap, int idx) {
    if (idx >= cap) return 0;
    return audio_hda_introspect(&out[idx], cap - idx);
}

static int devt_emit_battery(devtest_dev_info_t *out, int cap, int idx) {
    if (idx >= cap) return 0;
    return acpi_bat_introspect(&out[idx], cap - idx);
}

static int devt_emit_hub(devtest_dev_info_t *out, int cap, int idx) {
    int n = 0;
    size_t total = usb_hub_count();
    for (size_t i = 0; i < total && idx + n < cap; i++) {
        if (usb_hub_introspect_at(i, &out[idx + n])) n++;
    }
    return n;
}

/* M27A: project the display registry into dev-info shape so
 * `devlist display` works through the existing tool. We map
 * dimensions/pitch/format into the `extra` blob (ksnprintf'd in
 * the shape every other bus uses) and keep the per-output backend
 * name in the `driver` slot. status mirrors ABI_DISPLAY_* via a
 * small bit-shuffle: PRESENT->PRESENT, PRIMARY->BOUND, ACTIVE->ACTIVE
 * (no semantic loss; userland can still grep "P-A--" the way it
 * does for PCI). */
static int devt_emit_display(devtest_dev_info_t *out, int cap, int idx) {
    static struct abi_display_info recs[ABI_DISPLAY_MAX_OUTPUTS];
    int total = display_enumerate(recs, ABI_DISPLAY_MAX_OUTPUTS);
    int n = 0;
    for (int i = 0; i < total && idx + n < cap; i++) {
        const struct abi_display_info *d = &recs[i];
        devtest_dev_info_t *r = &out[idx + n];
        memset(r, 0, sizeof *r);
        r->bus    = ABI_DEVT_BUS_DISPLAY;
        r->status = ABI_DEVT_PRESENT |
                    ((d->status & ABI_DISPLAY_PRIMARY) ? ABI_DEVT_BOUND  : 0u) |
                    ((d->status & ABI_DISPLAY_ACTIVE)  ? ABI_DEVT_ACTIVE : 0u);
        r->index  = d->index;
        devt_strlcpy(r->name,   d->name[0]    ? d->name    : "fb?",      ABI_DEVT_NAME_MAX);
        devt_strlcpy(r->driver, d->backend[0] ? d->backend : "(none)",   ABI_DEVT_DRIVER_MAX);
        ksnprintf(r->extra, ABI_DEVT_EXTRA_MAX,
                  "%ux%u pitch=%u bpp=%u fmt=%u flips=%lu",
                  d->width, d->height, d->pitch_bytes, d->bpp,
                  (unsigned)d->pixel_format,
                  (unsigned long)d->flips);
        n++;
    }
    return n;
}

int devtest_enumerate(devtest_dev_info_t *out, int cap, uint32_t mask) {
    if (!out || cap <= 0) return 0;
    if (mask == 0) mask = ABI_DEVT_BUS_ALL;

    int n = 0;
    if (mask & ABI_DEVT_BUS_PCI)     n += devt_emit_pci    (out, cap, n);
    if (mask & ABI_DEVT_BUS_HUB)     n += devt_emit_hub    (out, cap, n);
    if (mask & ABI_DEVT_BUS_USB)     n += devt_emit_usb    (out, cap, n);
    if (mask & ABI_DEVT_BUS_BLK)     n += devt_emit_blk    (out, cap, n);
    if (mask & ABI_DEVT_BUS_INPUT)   n += devt_emit_input  (out, cap, n);
    if (mask & ABI_DEVT_BUS_AUDIO)   n += devt_emit_audio  (out, cap, n);
    if (mask & ABI_DEVT_BUS_BATTERY) n += devt_emit_battery(out, cap, n);
    if (mask & ABI_DEVT_BUS_DISPLAY) n += devt_emit_display(out, cap, n);
    return n;
}

/* ============================================================
 * event ring (M26A: empty; M26C populates)
 * ============================================================ */

void devtest_event_post(uint8_t kind, const devtest_dev_info_t *info) {
    if (!info || (kind != DEVT_EV_ATTACH && kind != DEVT_EV_DETACH)) return;
    uint32_t head = g_event_head;
    uint32_t next = (head + 1u) & (DEVT_EVENTS_RING - 1u);
    if (next == g_event_tail) {
        /* Drop oldest entry to make room -- userland that doesn't
         * drain shouldn't be able to wedge attach/detach delivery. */
        g_event_tail = (g_event_tail + 1u) & (DEVT_EVENTS_RING - 1u);
    }
    g_events[head].seq          = ++g_event_seq;
    g_events[head].timestamp_ms = 0;     /* filled by clock subsystem later */
    g_events[head].kind         = kind;
    g_events[head].info         = *info;
    g_event_head                = next;
}

int devtest_event_drain(struct devtest_event *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    while (n < cap && g_event_tail != g_event_head) {
        out[n] = g_events[g_event_tail];
        g_event_tail = (g_event_tail + 1u) & (DEVT_EVENTS_RING - 1u);
        n++;
    }
    return n;
}

/* ============================================================
 * boot-time inventory + self-test sweep
 * ============================================================ */

static const char *bus_label(uint8_t bus) {
    switch (bus) {
    case ABI_DEVT_BUS_PCI:     return "pci";
    case ABI_DEVT_BUS_USB:     return "usb";
    case ABI_DEVT_BUS_BLK:     return "blk";
    case ABI_DEVT_BUS_INPUT:   return "in";
    case ABI_DEVT_BUS_AUDIO:   return "snd";
    case ABI_DEVT_BUS_BATTERY: return "bat";
    case ABI_DEVT_BUS_HUB:     return "hub";
    case ABI_DEVT_BUS_DISPLAY: return "fb";
    default:                   return "?";
    }
}

void devtest_dump_kprintf(uint32_t mask) {
    static devtest_dev_info_t recs[ABI_DEVT_MAX_DEVICES];
    int n = devtest_enumerate(recs, ABI_DEVT_MAX_DEVICES, mask);
    kprintf("[devtest] === %d devices ===\n", n);
    for (int i = 0; i < n; i++) {
        const devtest_dev_info_t *r = &recs[i];
        kprintf("[INFO] %s: %s drv=%s status=0x%02x %s\n",
                bus_label(r->bus),
                r->name[0] ? r->name : "(unnamed)",
                r->driver[0] ? r->driver : "-",
                (unsigned)r->status,
                r->extra[0] ? r->extra : "");
    }
}

static void devt_log_test(const char *name, int rc, const char *msg,
                          void *cookie) {
    int *pf = (int *)cookie;
    const char *tag;
    if      (rc == 0)             { tag = "PASS"; pf[0]++; }
    else if (rc == ABI_DEVT_SKIP) { tag = "SKIP"; pf[2]++; }
    else                          { tag = "FAIL"; pf[1]++; }
    kprintf("[%s] %s: %s\n", tag, name, msg && msg[0] ? msg : "(no message)");
}

void devtest_boot_run(void) {
    kprintf("[boot] M26A: peripheral inventory + self-tests\n");
    devtest_dump_kprintf(ABI_DEVT_BUS_ALL);

    int counters[3] = {0, 0, 0};   /* pass, fail, skip */
    int total = devtest_for_each(devt_log_test, counters);
    kprintf("[boot] M26A: %d test(s) -- pass=%d fail=%d skip=%d\n",
            total, counters[0], counters[1], counters[2]);
}

/* ============================================================
 * one-time init -- registers all the built-in subsystem tests
 * ============================================================ */

/* Trivial PASS-always test: confirms the registry plumbing works
 * end-to-end (boot sweep, drvtest userland, syscall path). Useful
 * as a smoke check independent of any peripheral. */
static int test_devtest_self(char *msg, size_t cap) {
    ksnprintf(msg, cap, "devtest registry alive");
    return 0;
}

/* Block subsystem: PASS if at least one block device was registered. */
static int test_blk(char *msg, size_t cap) {
    size_t n = blk_count();
    if (n == 0) {
        ksnprintf(msg, cap, "no block devices registered");
        return ABI_DEVT_SKIP;
    }
    int disks = 0, parts = 0, wraps = 0;
    for (size_t i = 0; i < n; i++) {
        struct blk_dev *b = blk_get(i);
        if (!b) continue;
        switch (b->class) {
        case BLK_CLASS_DISK:      disks++; break;
        case BLK_CLASS_PARTITION: parts++; break;
        case BLK_CLASS_WRAPPER:   wraps++; break;
        }
    }
    ksnprintf(msg, cap, "%lu blk dev(s): disks=%d partitions=%d wrappers=%d",
              (unsigned long)n, disks, parts, wraps);
    return 0;
}

/* M26D input self-test: PS/2 driver always installs at boot on x86,
 * so this always PASSes. We use the test as a place to dump the
 * merged PS/2 + USB HID counters in one line so `drvtest input` is
 * a one-shot snapshot for the test harness. */
static int test_input(char *msg, size_t cap) {
    int hid_total = usb_hid_count();
    int hid_kbd   = usb_hid_kbd_count();
    int hid_mouse = usb_hid_mouse_count();
    ksnprintf(msg, cap,
              "ps2 kbd chars=%lu irqs=%lu (caps=%d) | "
              "ps2 mouse events=%lu clicks=%lu | "
              "usb_hid devs=%d (kbd=%d mouse=%d) frames=%lu",
              (unsigned long)kbd_chars_dispatched(),
              (unsigned long)kbd_irqs_total(),
              kbd_caps_state() ? 1 : 0,
              (unsigned long)mouse_events_total(),
              (unsigned long)mouse_btn_press_total(),
              hid_total, hid_kbd, hid_mouse,
              (unsigned long)usb_hid_total_frames());
    return 0;
}

/* PCI subsystem: every working tobyOS boot has at least one PCI
 * device (the host bridge), so anything else is a hard failure. */
static int test_pci(char *msg, size_t cap) {
    size_t n = pci_device_count();
    if (n == 0) {
        ksnprintf(msg, cap, "PCI enumeration returned zero devices");
        return -ABI_EIO;
    }
    int bound = 0;
    for (size_t i = 0; i < n; i++) {
        struct pci_dev *p = pci_device_at(i);
        if (p && p->driver) bound++;
    }
    ksnprintf(msg, cap, "%lu PCI device(s), %d driver-bound",
              (unsigned long)n, bound);
    return 0;
}

void devtest_init(void) {
    memset(g_tests,  0, sizeof g_tests);
    memset(g_events, 0, sizeof g_events);
    g_test_count = 0;
    g_event_head = g_event_tail = 0;
    g_event_seq  = 0;

    /* Built-in tests every kernel ships with. */
    devtest_register("devtest", test_devtest_self);
    devtest_register("pci",     test_pci);
    devtest_register("blk",     test_blk);

    /* Subsystem tests -- each subsystem owns its own callback so this
     * file does not need to know how the test is implemented. */
    devtest_register("xhci",     xhci_selftest);
    devtest_register("usb",      xhci_devices_selftest);
    devtest_register("usb_hub",  usb_hub_selftest);
    devtest_register("audio",    audio_hda_selftest);
    /* M26F: audio_tone exercises the full output path (stream descriptor
     * + BDL + DAC/PIN verbs). SKIPs cleanly when no codec is attached. */
    devtest_register("audio_tone", audio_hda_tone_selftest);
    devtest_register("battery",  acpi_bat_selftest);
    /* M26C: ring round-trip test (synthetic ATTACH + DETACH). Doesn't
     * exercise the full xHCI/hub stack -- those are validated by the
     * `usbtest hotplug` userland tool against QMP device_add/del.
     * Here we just guarantee the kernel-side ring works. */
    devtest_register("hotplug",  hotplug_selftest);
    /* M26D: input + HID. "input" always PASSes (PS/2 always present
     * on x86); "usb_hid" SKIPs cleanly when no HID device is plugged. */
    devtest_register("input",    test_input);
    devtest_register("usb_hid",  usb_hid_selftest);
    /* M26E: USB mass-storage hardening. SKIPs when no usb-storage
     * device is plugged; otherwise validates blk_dev<->udev coherence
     * and runs a single-sector probe read on each bound slot. */
    devtest_register("usb_msc",  usb_msc_selftest);
}
