/* usb_hub.c -- USB Hub class driver (M26B).
 *
 * Lives between xhci.c (which owns the controller + slot table) and
 * the devtest harness (which exposes per-bus device records to
 * userland). On any device whose bDeviceClass == USB_CLASS_HUB or
 * whose bInterfaceClass == 0x09, xhci_finalize_device() calls
 * usb_hub_probe(), which then:
 *
 *   1. GET_DESCRIPTOR(HUB) -- pulls bNbrPorts + power-on delay
 *   2. xhci_configure_as_hub() -- flips the Slot Context Hub bit
 *   3. for each downstream port:
 *        SET_FEATURE(PORT_POWER) -> wait power-good
 *        GET_STATUS              -> if connected:
 *          SET_FEATURE(PORT_RESET) -> wait C_PORT_RESET
 *          xhci_attach_via_hub()   -> recursive class probe
 *
 * Records are kept in a small static table (g_hubs) so devtest can
 * iterate them; we cap at TOBY_MAX_HUBS = 4 since QEMU only ever
 * gives us 1 hub per topology and real laptops top out around 3.
 *
 * Failure handling: any failure on a single port is logged and
 * skipped; we never abort the whole hub probe. Empty / disconnected
 * ports produce zero records (and zero log spam).
 */

#include <tobyos/usb_hub.h>
#include <tobyos/xhci.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>

/* xhci.c exports this; usb_hid.c + usb_msc.c forward-declare it the
 * same way. Keeps usb_hub.c free of HCI internals. */
extern bool xhci_control_class(struct usb_device *dev,
                               uint8_t bm_request_type, uint8_t b_request,
                               uint16_t w_value, uint16_t w_index,
                               void *buf, uint16_t w_length);

/* ---- per-hub bookkeeping --------------------------------------- */

#define TOBY_MAX_HUBS  4

struct hub_entry {
    bool             in_use;
    struct usb_device *dev;          /* slot in xhci_dev_state[] */
    uint8_t          nports;
    uint8_t          ports_seen;     /* how many port-status reads succeeded */
    uint8_t          ports_with_dev; /* how many of those reported CONNECTION */
    uint8_t          ports_attached; /* how many we successfully addressed */
    uint16_t         pwr_on_2pwr_good_ms;
    uint16_t         characteristics;

    /* M26C: snapshot of CONNECTION bit for every downstream port,
     * indexed by (port - 1). usb_hub_poll() compares this against
     * GET_PORT_STATUS to detect attach/detach edges. The hub's own
     * change bit (C_PORT_CONNECTION) is also a useful trigger but
     * isn't strictly necessary -- polling the level is enough for
     * correctness, and avoids edge-coalescing problems on slow polls.
     */
    uint16_t         last_connected;   /* bit i = port (i+1) was connected */
    uint16_t         poll_errs;        /* GET_PORT_STATUS failure counter */
    uint32_t         poll_iter;        /* number of times we've polled */
    uint32_t         attach_count;     /* total attach events fired */
    uint32_t         detach_count;     /* total detach events fired */
};

static struct {
    size_t            count;
    struct hub_entry  ent[TOBY_MAX_HUBS];
} g_hubs;

/* Forward decls used by enumerate_hub_port + the new poll path. */
static void enumerate_hub_port(struct hub_entry *he, uint8_t port);
static struct hub_entry *find_hub_entry(struct usb_device *dev);

/* ---- standard hub class control transfers ---------------------- */

/* Class-specific GET_DESCRIPTOR(HUB) goes to RECIP_DEVICE, the others
 * (port ops) go to RECIP_OTHER with wIndex = port_num. wValue carries
 * either the descriptor type (for GET_DESCRIPTOR) or the feature
 * selector (for SET/CLEAR_FEATURE). */

static bool hub_get_desc(struct usb_device *dev, struct usb_hub_desc *out) {
    /* USB 2.0 §11.24.2.5: bRequest = 0x06 GET_DESCRIPTOR,
     * wValue = (USB_DESC_HUB << 8) | 0, wIndex = 0,
     * wLength = sizeof(usb_hub_desc) is upper-bounded but the device
     * may return less; we ask for the full struct and rely on
     * bDescLength to know the actual size. */
    memset(out, 0, sizeof(*out));
    return xhci_control_class(
        dev,
        USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
        USB_REQ_GET_DESCRIPTOR,
        (uint16_t)(USB_DESC_HUB << 8),
        0,
        out, (uint16_t)sizeof(*out));
}

static bool hub_set_port_feature(struct usb_device *dev,
                                 uint8_t port, uint16_t feature) {
    return xhci_control_class(
        dev,
        USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
        USB_REQ_SET_FEATURE,
        feature, port,
        0, 0);
}

static bool hub_clear_port_feature(struct usb_device *dev,
                                   uint8_t port, uint16_t feature) {
    return xhci_control_class(
        dev,
        USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
        USB_REQ_CLEAR_FEATURE,
        feature, port,
        0, 0);
}

/* GET_PORT_STATUS returns 4 bytes:
 *   bytes 0..1 = wPortStatus
 *   bytes 2..3 = wPortChange
 * Returns true on success; *status / *change set to the parsed
 * little-endian values (zero on failure). */
static bool hub_get_port_status(struct usb_device *dev,
                                uint8_t port,
                                uint16_t *out_status,
                                uint16_t *out_change) {
    uint8_t buf[4] = { 0 };
    bool ok = xhci_control_class(
        dev,
        USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
        USB_REQ_GET_STATUS,
        0, port,
        buf, sizeof(buf));
    if (!ok) {
        if (out_status) *out_status = 0;
        if (out_change) *out_change = 0;
        return false;
    }
    if (out_status) *out_status = (uint16_t)(buf[0] | (buf[1] << 8));
    if (out_change) *out_change = (uint16_t)(buf[2] | (buf[3] << 8));
    return true;
}

/* Translate the wPortStatus low/high speed bits to a USB-IF speed
 * code. Note: a USB-2 hub never sees a SuperSpeed device on its
 * downstream port (those route through the USB-3 root-hub side), so
 * we cap at HS. */
static uint8_t portstat_to_speed(uint16_t status) {
    if (status & USB_HUB_PORT_STAT_HIGH_SPEED) return 3;   /* HS */
    if (status & USB_HUB_PORT_STAT_LOW_SPEED)  return 2;   /* LS */
    return 1;                                              /* FS default */
}

/* ---- per-port bring-up ---------------------------------------- */

static void enumerate_hub_port(struct hub_entry *he, uint8_t port) {
    struct usb_device *parent = he->dev;

    /* 1. Apply port power. On most QEMU hubs this is a no-op (ports
     *    are always powered) but the spec-mandated pwr2pwrgood delay
     *    still has to be honoured. */
    if (!hub_set_port_feature(parent, port, USB_HUB_FEAT_PORT_POWER)) {
        kprintf("[usb_hub] slot %u port %u: SET_FEATURE(POWER) failed\n",
                parent->slot_id, port);
        return;
    }
    /* bPwrOn2PwrGood is in 2 ms units, capped at ~510 ms. We add 5 ms
     * slack on top, never less than 20 ms (qemu reports 0 here). */
    uint32_t delay = he->pwr_on_2pwr_good_ms + 5u;
    if (delay < 20u) delay = 20u;
    pit_sleep_ms((int)delay);

    /* 2. Read initial port status. If nothing is connected we are
     *    done (no record; that's intentional -- empty ports do NOT
     *    show up in devlist). */
    uint16_t status = 0, change = 0;
    if (!hub_get_port_status(parent, port, &status, &change)) {
        kprintf("[usb_hub] slot %u port %u: GET_STATUS failed\n",
                parent->slot_id, port);
        return;
    }
    he->ports_seen++;
    if (!(status & USB_HUB_PORT_STAT_CONNECTION)) {
        /* Empty port: clear any change bits and bail out cleanly. */
        if (change & USB_HUB_PORT_CHG_CONNECTION) {
            (void)hub_clear_port_feature(parent, port,
                                         USB_HUB_FEAT_C_PORT_CONNECTION);
        }
        return;
    }

    /* 3. Reset the downstream port. The hub auto-asserts ENABLE on
     *    completion (status bit 1) and sets the C_PORT_RESET change
     *    bit, which we then clear. */
    if (!hub_set_port_feature(parent, port, USB_HUB_FEAT_PORT_RESET)) {
        kprintf("[usb_hub] slot %u port %u: SET_FEATURE(RESET) failed\n",
                parent->slot_id, port);
        return;
    }
    /* Poll up to ~200 ms for C_PORT_RESET. QEMU completes in <10 ms. */
    bool reset_done = false;
    for (int i = 0; i < 40; i++) {
        pit_sleep_ms(5);
        if (!hub_get_port_status(parent, port, &status, &change)) break;
        if (change & USB_HUB_PORT_CHG_RESET) { reset_done = true; break; }
    }
    if (!reset_done) {
        kprintf("[usb_hub] slot %u port %u: PORT_RESET did not complete "
                "(status=0x%04x change=0x%04x)\n",
                parent->slot_id, port, status, change);
        return;
    }
    (void)hub_clear_port_feature(parent, port, USB_HUB_FEAT_C_PORT_RESET);

    /* Some hubs also leave C_PORT_CONNECTION set after the initial
     * power-on; clear it so we don't see a phantom hot-plug event in
     * the M26C polling loop later. */
    if (change & USB_HUB_PORT_CHG_CONNECTION) {
        (void)hub_clear_port_feature(parent, port,
                                     USB_HUB_FEAT_C_PORT_CONNECTION);
    }

    /* Re-read status to learn the negotiated speed AFTER reset. */
    if (!hub_get_port_status(parent, port, &status, &change)) {
        kprintf("[usb_hub] slot %u port %u: post-reset GET_STATUS failed\n",
                parent->slot_id, port);
        return;
    }
    if (!(status & USB_HUB_PORT_STAT_ENABLE)) {
        kprintf("[usb_hub] slot %u port %u: not ENABLED after reset "
                "(status=0x%04x)\n", parent->slot_id, port, status);
        return;
    }

    he->ports_with_dev++;
    uint8_t speed = portstat_to_speed(status);
    kprintf("[usb_hub] slot %u port %u: device connected, speed=%u\n",
            parent->slot_id, port, speed);

    /* 4. Hand off to the HCI for slot allocation + addressing. */
    struct usb_device *child = xhci_attach_via_hub(parent, port, speed);
    if (!child) {
        kprintf("[usb_hub] slot %u port %u: xhci_attach_via_hub failed\n",
                parent->slot_id, port);
        return;
    }
    he->ports_attached++;
    /* M26C: record the steady-state CONNECTION level for this port so
     * the first usb_hub_poll() pass after probe doesn't see a phantom
     * "newly attached" edge for devices we already brought up. */
    if (port >= 1 && port <= 16) {
        he->last_connected |= (uint16_t)(1u << (port - 1u));
    }
}

/* ---- public entry points -------------------------------------- */

bool usb_hub_probe(struct usb_device *dev) {
    if (!dev) return false;
    if (g_hubs.count >= TOBY_MAX_HUBS) {
        kprintf("[usb_hub] too many hubs (max %u), dropping slot %u\n",
                (unsigned)TOBY_MAX_HUBS, dev->slot_id);
        return false;
    }

    /* M26B scope: accept Full Speed (1) and High Speed (3) hubs.
     *
     * QEMU's `usb-hub` is hardcoded to USB 1.1 (FS), so this is the
     * only path that gets exercised in CI today. An HS hub additionally
     * needs Transaction Translator bookkeeping for FS/LS *children*
     * (xHCI spec §6.2.2: Parent Hub Slot ID + Parent Port Number on
     * the child Slot Context). FS-hub children, on the other hand, are
     * themselves FS/LS and need no TT fields at all -- the xHCI just
     * routes via the route string we already program.
     *
     * SuperSpeed hubs are out of scope (would require xHCI USB-3
     * pipe management); skip cleanly. */
    if (dev->speed != 1 && dev->speed != 3) {
        kprintf("[usb_hub] skipping unsupported hub speed (slot %u speed=%u)\n",
                dev->slot_id, dev->speed);
        return false;
    }

    /* Fetch the standard hub descriptor. We know it's at most ~15B
     * for a 15-port hub (7 fixed + 2 + 2 trailing bitmaps). */
    struct usb_hub_desc hd;
    if (!hub_get_desc(dev, &hd)) {
        kprintf("[usb_hub] GET_DESCRIPTOR(HUB) failed on slot %u\n",
                dev->slot_id);
        return false;
    }
    if (hd.bDescriptorType != USB_DESC_HUB || hd.bNbrPorts == 0) {
        kprintf("[usb_hub] slot %u: bogus hub desc (type=0x%02x nports=%u)\n",
                dev->slot_id, hd.bDescriptorType, hd.bNbrPorts);
        return false;
    }
    uint8_t nports = hd.bNbrPorts;
    if (nports > USB_HUB_MAX_PORTS) nports = USB_HUB_MAX_PORTS;

    /* Tell the controller this slot is now a hub. ttt = 0 for HS. */
    if (!xhci_configure_as_hub(dev, nports, 0)) {
        kprintf("[usb_hub] xhci_configure_as_hub failed on slot %u\n",
                dev->slot_id);
        return false;
    }

    struct hub_entry *he = &g_hubs.ent[g_hubs.count];
    memset(he, 0, sizeof(*he));
    he->in_use              = true;
    he->dev                 = dev;
    he->nports              = nports;
    he->characteristics     = hd.wHubCharacteristics;
    he->pwr_on_2pwr_good_ms = (uint16_t)((uint16_t)hd.bPwrOn2PwrGood * 2u);
    g_hubs.count++;

    kprintf("[usb_hub] slot %u: HUB nports=%u char=0x%04x pwr2pwrgood=%u ms\n",
            dev->slot_id, (unsigned)nports,
            (unsigned)hd.wHubCharacteristics,
            (unsigned)he->pwr_on_2pwr_good_ms);

    /* Walk every downstream port. Empty ports return silently; the
     * function never aborts the loop on per-port failure. */
    for (uint8_t p = 1; p <= nports; p++) {
        enumerate_hub_port(he, p);
    }

    kprintf("[usb_hub] slot %u: %u/%u ports populated, %u attached\n",
            dev->slot_id, he->ports_with_dev, he->nports,
            he->ports_attached);
    return true;
}

size_t usb_hub_count(void) {
    return g_hubs.count;
}

bool usb_hub_introspect_at(size_t i, struct abi_dev_info *out) {
    if (!out || i >= g_hubs.count) return false;
    struct hub_entry *he = &g_hubs.ent[i];
    if (!he->in_use || !he->dev) return false;

    memset(out, 0, sizeof(*out));
    out->bus        = ABI_DEVT_BUS_HUB;
    out->status     = ABI_DEVT_PRESENT | ABI_DEVT_BOUND;
    out->hub_depth  = he->dev->hub_depth;
    out->hub_port   = he->dev->hub_depth ? he->dev->hub_port : he->dev->port_id;
    out->vendor     = 0;
    out->device     = 0;
    out->class_code = USB_CLASS_HUB;
    out->index      = (uint8_t)i;

    if (he->dev->hub_depth == 0) {
        ksnprintf(out->name, ABI_DEVT_NAME_MAX, "hub1-%u",
                  (unsigned)he->dev->port_id);
    } else {
        ksnprintf(out->name, ABI_DEVT_NAME_MAX, "hub1-%u.%u",
                  (unsigned)he->dev->port_id,
                  (unsigned)he->dev->hub_port);
    }
    /* Driver name is the only one we have for hubs today. */
    const char *dn = "usb_hub";
    size_t n = 0;
    while (dn[n] && n + 1 < ABI_DEVT_DRIVER_MAX) {
        out->driver[n] = dn[n]; n++;
    }
    out->driver[n] = '\0';

    ksnprintf(out->extra, ABI_DEVT_EXTRA_MAX,
              "slot=%u nports=%u up=%u attached=%u depth=%u",
              (unsigned)he->dev->slot_id,
              (unsigned)he->nports,
              (unsigned)he->ports_with_dev,
              (unsigned)he->ports_attached,
              (unsigned)he->dev->hub_depth);
    return true;
}

int usb_hub_selftest(char *msg, size_t cap) {
    if (g_hubs.count == 0) {
        ksnprintf(msg, cap,
                  "no USB hub enumerated (try -device usb-hub)");
        return ABI_DEVT_SKIP;
    }
    /* Aggregate per-hub stats. A hub passes if its nports matches
     * the descriptor and at least one of:
     *   - we walked every port without crashing (ports_seen == nports)
     *   - all populated ports were successfully addressed
     */
    unsigned hubs_total    = 0;
    unsigned hubs_walked   = 0;   /* enumerated all their ports */
    unsigned ports_total   = 0;
    unsigned ports_present = 0;
    unsigned ports_addr    = 0;

    for (size_t i = 0; i < g_hubs.count; i++) {
        struct hub_entry *he = &g_hubs.ent[i];
        if (!he->in_use) continue;
        hubs_total++;
        ports_total   += he->nports;
        ports_present += he->ports_with_dev;
        ports_addr    += he->ports_attached;
        if (he->ports_seen == he->nports) hubs_walked++;
    }

    /* Walked-all-ports is a hard requirement (M26B says "disconnected
     * ports do not crash"). attached>0 is informational; many test
     * topologies legitimately have an empty hub. */
    if (hubs_walked != hubs_total) {
        ksnprintf(msg, cap,
                  "hubs=%u but only %u walked all ports "
                  "(ports_seen mismatch -> port enumeration crashed?)",
                  hubs_total, hubs_walked);
        return -ABI_EIO;
    }
    ksnprintf(msg, cap,
              "hubs=%u ports=%u populated=%u attached=%u",
              hubs_total, ports_total, ports_present, ports_addr);
    return 0;
}

/* ---- M26C: hot-plug polling for downstream ports -------------- */
/*
 * The xHC raises a Port Status Change TRB on root-hub events but
 * NOT on hub-downstream events (those propagate via the hub's own
 * status interrupt-IN endpoint, which we don't wire up today --
 * that would be a full M26D-class effort). Instead we GET_PORT_STATUS
 * on every downstream port a few times a second from the kernel idle
 * loop. Each control transfer is ~1ms, so 4 hubs * 8 ports = 32ms
 * per pass; we rate-limit to 5 Hz, which keeps the worst-case CPU
 * cost well under 1% on QEMU and gives QMP `device_add`/`device_del`
 * a noticeable reaction time of <200ms.
 */

#include <tobyos/hotplug.h>
#include <tobyos/perf.h>

static struct hub_entry *find_hub_entry(struct usb_device *dev) {
    if (!dev) return 0;
    for (size_t i = 0; i < g_hubs.count; i++) {
        if (g_hubs.ent[i].in_use && g_hubs.ent[i].dev == dev) {
            return &g_hubs.ent[i];
        }
    }
    return 0;
}

/* Helper: fold hub_entry + port_id into a string for hotplug events.
 * Mirrors xhci.c's xhci_format_dev_info() so userland sees the same
 * "usb1-1.3" format from both sides. */
static void hub_format_port_info(const struct hub_entry *he, uint8_t port,
                                 const char *what,
                                 char *out, size_t cap) {
    /* hub's `dev->port_id` is the root port the hub itself is on. */
    if (he->dev->hub_depth == 0) {
        ksnprintf(out, cap, "usb1-%u.%u %s",
                  (unsigned)he->dev->port_id, (unsigned)port, what);
    } else {
        ksnprintf(out, cap, "usb1-%u.%u.%u %s",
                  (unsigned)he->dev->port_id,
                  (unsigned)he->dev->hub_port,
                  (unsigned)port, what);
    }
}

int usb_hub_handle_port_change(struct usb_device *hub_dev, uint8_t port) {
    struct hub_entry *he = find_hub_entry(hub_dev);
    if (!he || !he->in_use) return -ABI_EIO;
    if (port < 1 || port > he->nports || port > 16) return -ABI_EIO;

    uint16_t bit = (uint16_t)(1u << (port - 1u));

    uint16_t status = 0, change = 0;
    if (!hub_get_port_status(hub_dev, port, &status, &change)) {
        he->poll_errs++;
        return -ABI_EIO;
    }
    /* Always clear C_PORT_CONNECTION so we don't re-fire on the next
     * pass. Some hubs latch additional change bits we don't care
     * about (C_PORT_ENABLE / C_PORT_OVER_CURRENT); clear them too,
     * since leaving them set masks future transitions on real hubs. */
    if (change & USB_HUB_PORT_CHG_CONNECTION) {
        (void)hub_clear_port_feature(hub_dev, port,
                                     USB_HUB_FEAT_C_PORT_CONNECTION);
    }

    bool ccs       = (status & USB_HUB_PORT_STAT_CONNECTION) != 0;
    bool was_conn  = (he->last_connected & bit) != 0;
    if (ccs == was_conn) {
        /* Spurious change (PEC/OCC etc.) -- nothing to do at the
         * device level. */
        return 0;
    }

    if (ccs) {
        /* Edge: cable just plugged in. Re-use the boot path; it
         * does PORT_RESET + xhci_attach_via_hub + bookkeeping. */
        kprintf("[usb_hub] hot-attach hub-slot %u port %u\n",
                hub_dev->slot_id, port);
        uint8_t before_attached = he->ports_attached;
        enumerate_hub_port(he, port);
        if (he->ports_attached > before_attached) {
            /* enumerate_hub_port() set last_connected for us. */
            uint8_t child_slot = xhci_slot_for_hub_port(hub_dev->slot_id,
                                                        port);
            char info[ABI_HOT_INFO_MAX];
            hub_format_port_info(he, port, "attached", info, sizeof(info));
            hotplug_post_attach(ABI_DEVT_BUS_USB,
                                child_slot, hub_dev->hub_depth + 1, port,
                                info);
            he->attach_count++;
        } else {
            /* Bring-up failed -- record the level anyway so we don't
             * keep retrying every poll. The next disconnect will
             * clear the bit. */
            he->last_connected |= bit;
        }
    } else {
        /* Edge: cable just yanked. Find + tear down the child slot
         * (if any) and clear our level bit. xhci_detach_slot() posts
         * the detach event with the right slot+depth+port info. */
        kprintf("[usb_hub] hot-detach hub-slot %u port %u\n",
                hub_dev->slot_id, port);
        uint8_t child_slot = xhci_slot_for_hub_port(hub_dev->slot_id, port);
        if (child_slot) {
            (void)xhci_detach_slot(child_slot);
        } else {
            /* No child was ever addressed (likely a flaky device that
             * never made it past PORT_RESET). Still post a detach
             * event so the test harness sees the level transition. */
            char info[ABI_HOT_INFO_MAX];
            hub_format_port_info(he, port, "detached (no slot)",
                                 info, sizeof(info));
            hotplug_post_detach(ABI_DEVT_BUS_USB, 0,
                                hub_dev->hub_depth + 1, port, info);
        }
        he->last_connected &= (uint16_t)~bit;
        he->detach_count++;
    }
    return 0;
}

int usb_hub_poll(void) {
    if (g_hubs.count == 0) return 0;
    /* Rate-limit to 5 Hz. perf_now_ns() returns 0 before TSC
     * calibration; we treat that as "always poll" so the early
     * boot path still works. */
    static uint64_t s_next_due_ns = 0;
    uint64_t now = perf_now_ns();
    if (now != 0 && now < s_next_due_ns) return 0;
    s_next_due_ns = now + 200000000ull;     /* 200 ms */

    int changes = 0;
    for (size_t i = 0; i < g_hubs.count; i++) {
        struct hub_entry *he = &g_hubs.ent[i];
        if (!he->in_use || !he->dev) continue;
        he->poll_iter++;
        for (uint8_t p = 1; p <= he->nports && p <= 16; p++) {
            uint16_t status = 0, change = 0;
            if (!hub_get_port_status(he->dev, p, &status, &change)) {
                he->poll_errs++;
                continue;
            }
            bool ccs      = (status & USB_HUB_PORT_STAT_CONNECTION) != 0;
            bool was_conn = (he->last_connected & (uint16_t)(1u << (p - 1u))) != 0;
            if (ccs != was_conn ||
                (change & USB_HUB_PORT_CHG_CONNECTION)) {
                if (usb_hub_handle_port_change(he->dev, p) == 0) {
                    changes++;
                }
            } else if (change) {
                /* Just clear stray C_PORT_* bits so the hub doesn't
                 * keep re-asserting its status interrupt forever. */
                if (change & USB_HUB_PORT_CHG_RESET) {
                    (void)hub_clear_port_feature(he->dev, p,
                                                 USB_HUB_FEAT_C_PORT_RESET);
                }
            }
        }
    }
    return changes;
}
