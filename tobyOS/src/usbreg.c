/* usbreg.c -- USB device attach registry (Milestone 35C).
 *
 * The xHCI / hub probe path calls usbreg_record_attach() for every
 * device that finishes Address Device, with the dev_class triple from
 * the device descriptor (or the active interface descriptor for
 * composite devices). We classify the attach against drvdb_usb_lookup
 * to derive a friendly name and the recommended driver, then store
 * the row.
 *
 * Status transitions are linear:
 *
 *     FREE --attach--> NEW
 *     NEW  --bound----> BOUND          (driver != NULL)
 *     NEW  --no-drv---> UNSUPPORTED    (drvdb has class, driver=NULL)
 *     NEW  --no-cls---> UNKNOWN        (drvdb has no entry at all)
 *     BOUND  --probe-fail--> PROBE_FAILED
 *     *      --detach--> GONE          (slot stays in registry until reused)
 *     GONE   --attach--> NEW           (slot reuse)
 *
 * Allocation: the table is a fixed-size array indexed by registry
 * position (NOT slot id). Slot ids are not contiguous and can range up
 * to xHCI MaxSlots (~64), but typical VM configs see <=8 USB devices,
 * so a 16-entry table is comfortable. Lookup by slot id is linear --
 * trivial at this device count.
 *
 * Concurrency: a single spinlock guards the entire table. All public
 * entry points take it; helpers callable while the lock is held are
 * marked _locked.
 */

#include <tobyos/usbreg.h>
#include <tobyos/drvdb.h>
#include <tobyos/spinlock.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>

static struct usbreg_entry  g_table[USBREG_MAX];
static spinlock_t           g_lock = SPINLOCK_INIT;
static bool                 g_inited;
static uint32_t             g_overflow_warns;

void usbreg_init(void) {
    uint64_t f = spin_lock_irqsave(&g_lock);
    memset(g_table, 0, sizeof(g_table));
    g_overflow_warns = 0;
    g_inited = true;
    spin_unlock_irqrestore(&g_lock, f);
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* Walk for a slot match. Caller holds g_lock. */
static struct usbreg_entry *find_slot_locked(uint8_t slot_id) {
    for (size_t i = 0; i < USBREG_MAX; i++) {
        struct usbreg_entry *e = &g_table[i];
        if (e->status != USBREG_STATUS_FREE && e->slot_id == slot_id) {
            return e;
        }
    }
    return NULL;
}

/* First FREE or GONE slot. Caller holds g_lock. */
static struct usbreg_entry *alloc_locked(void) {
    /* Pass 1: prefer an honest FREE slot so detached devices stay
     * visible until something needs the row. */
    for (size_t i = 0; i < USBREG_MAX; i++) {
        if (g_table[i].status == USBREG_STATUS_FREE) return &g_table[i];
    }
    /* Pass 2: reclaim a GONE slot. */
    for (size_t i = 0; i < USBREG_MAX; i++) {
        if (g_table[i].status == USBREG_STATUS_GONE) {
            memset(&g_table[i], 0, sizeof(g_table[i]));
            return &g_table[i];
        }
    }
    return NULL;
}

void usbreg_record_attach(uint8_t slot_id,
                          uint8_t port_id,
                          uint8_t hub_depth,
                          uint8_t speed,
                          uint16_t vendor,
                          uint16_t product,
                          uint8_t dev_class,
                          uint8_t dev_subclass,
                          uint8_t dev_protocol,
                          const char *driver) {
    if (!g_inited) usbreg_init();

    uint64_t f = spin_lock_irqsave(&g_lock);

    /* If this slot is already known, update in place (e.g. when the
     * iface-class scan re-classifies a hub that announced a generic
     * device class). */
    struct usbreg_entry *e = find_slot_locked(slot_id);
    bool reused = (e != NULL);
    if (!e) {
        e = alloc_locked();
        if (!e) {
            spin_unlock_irqrestore(&g_lock, f);
            if (g_overflow_warns < 4) {
                g_overflow_warns++;
                slog_emit(ABI_SLOG_LEVEL_WARN, SLOG_SUB_HW,
                          "usbreg overflow: dropping slot=%u "
                          "class=%02x/%02x/%02x",
                          slot_id, dev_class, dev_subclass, dev_protocol);
            }
            return;
        }
    }

    e->slot_id      = slot_id;
    e->port_id      = port_id;
    e->hub_depth    = hub_depth;
    e->speed        = speed;
    e->vendor       = vendor;
    e->product      = product;
    e->dev_class    = dev_class;
    e->dev_subclass = dev_subclass;
    e->dev_protocol = dev_protocol;

    /* Resolve a friendly name + recommended driver from drvdb. We look
     * at the (class, subclass, proto) triple; drvdb walks wildcards
     * itself. */
    const struct drvdb_usb_entry *db =
        drvdb_usb_lookup(dev_class, dev_subclass, dev_protocol);

    if (db && db->friendly) {
        copy_str(e->friendly, sizeof(e->friendly), db->friendly);
    } else {
        char buf[USBREG_FRIENDLY_MAX];
        ksnprintf(buf, sizeof(buf), "USB class %02x/%02x/%02x",
                  dev_class, dev_subclass, dev_protocol);
        copy_str(e->friendly, sizeof(e->friendly), buf);
    }

    if (driver && driver[0]) {
        copy_str(e->driver, sizeof(e->driver), driver);
        e->status = USBREG_STATUS_BOUND;
    } else if (db && db->driver) {
        /* drvdb knows what driver SHOULD claim this, but no class
         * driver actually called us with a name -- record the
         * recommended driver but flag as unsupported until/unless a
         * later record_attach upgrades us to BOUND. */
        copy_str(e->driver, sizeof(e->driver), db->driver);
        e->status = USBREG_STATUS_UNSUPPORTED;
    } else if (db) {
        copy_str(e->driver, sizeof(e->driver), "(none)");
        e->status = USBREG_STATUS_UNSUPPORTED;
    } else {
        copy_str(e->driver, sizeof(e->driver), "(none)");
        e->status = USBREG_STATUS_UNKNOWN;
    }

    spin_unlock_irqrestore(&g_lock, f);

    /* Outside the lock: emit a single structured event so debug.log
     * captures every attach + its outcome. We skip the SLOG channel
     * (which queues into journals) for UNKNOWN to avoid spamming on
     * weird emulated devices, but still kprintf so devlist sees it. */
    kprintf("[usbreg] slot=%u port=%u depth=%u speed=%u "
            "%04x:%04x class=%02x/%02x/%02x %s drv=%s%s\n",
            slot_id, port_id, hub_depth, speed,
            vendor, product,
            dev_class, dev_subclass, dev_protocol,
            e->friendly, e->driver,
            reused ? " (re-classify)" : "");

    if (e->status == USBREG_STATUS_BOUND) {
        slog_emit(ABI_SLOG_LEVEL_INFO, SLOG_SUB_HW,
                  "usb attach slot=%u %s drv=%s",
                  slot_id, e->friendly, e->driver);
    } else {
        slog_emit(ABI_SLOG_LEVEL_WARN, SLOG_SUB_HW,
                  "usb attach slot=%u %s status=%s class=%02x/%02x/%02x",
                  slot_id, e->friendly,
                  usbreg_status_name(e->status),
                  dev_class, dev_subclass, dev_protocol);
    }
}

void usbreg_record_detach(uint8_t slot_id) {
    if (!g_inited) return;
    char friendly[USBREG_FRIENDLY_MAX] = "(unknown)";
    bool found = false;

    uint64_t f = spin_lock_irqsave(&g_lock);
    struct usbreg_entry *e = find_slot_locked(slot_id);
    if (e) {
        copy_str(friendly, sizeof(friendly), e->friendly);
        e->status = USBREG_STATUS_GONE;
        found = true;
    }
    spin_unlock_irqrestore(&g_lock, f);

    if (found) {
        kprintf("[usbreg] slot=%u detach: %s\n", slot_id, friendly);
        slog_emit(ABI_SLOG_LEVEL_INFO, SLOG_SUB_HW,
                  "usb detach slot=%u %s", slot_id, friendly);
    }
}

void usbreg_record_probe_failed(uint8_t slot_id, const char *driver) {
    if (!g_inited) return;
    uint64_t f = spin_lock_irqsave(&g_lock);
    struct usbreg_entry *e = find_slot_locked(slot_id);
    if (e) {
        if (driver && driver[0]) {
            copy_str(e->driver, sizeof(e->driver), driver);
        }
        e->status = USBREG_STATUS_PROBE_FAILED;
    }
    spin_unlock_irqrestore(&g_lock, f);
    if (driver) {
        slog_emit(ABI_SLOG_LEVEL_ERROR, SLOG_SUB_HW,
                  "usb probe failed slot=%u driver=%s", slot_id, driver);
    }
}

size_t usbreg_count(void) {
    if (!g_inited) return 0;
    uint64_t f = spin_lock_irqsave(&g_lock);
    size_t n = 0;
    for (size_t i = 0; i < USBREG_MAX; i++) {
        if (g_table[i].status != USBREG_STATUS_FREE) n++;
    }
    spin_unlock_irqrestore(&g_lock, f);
    return n;
}

size_t usbreg_count_active(void) {
    if (!g_inited) return 0;
    uint64_t f = spin_lock_irqsave(&g_lock);
    size_t n = 0;
    for (size_t i = 0; i < USBREG_MAX; i++) {
        if (g_table[i].status != USBREG_STATUS_FREE &&
            g_table[i].status != USBREG_STATUS_GONE) {
            n++;
        }
    }
    spin_unlock_irqrestore(&g_lock, f);
    return n;
}

const struct usbreg_entry *usbreg_get(size_t idx) {
    if (!g_inited || idx >= USBREG_MAX) return NULL;
    if (g_table[idx].status == USBREG_STATUS_FREE) return NULL;
    return &g_table[idx];
}

const struct usbreg_entry *usbreg_find(uint8_t slot_id) {
    if (!g_inited) return NULL;
    uint64_t f = spin_lock_irqsave(&g_lock);
    const struct usbreg_entry *e = find_slot_locked(slot_id);
    spin_unlock_irqrestore(&g_lock, f);
    return e;
}

const char *usbreg_status_name(enum usbreg_status st) {
    switch (st) {
        case USBREG_STATUS_FREE:         return "free";
        case USBREG_STATUS_NEW:          return "new";
        case USBREG_STATUS_BOUND:        return "bound";
        case USBREG_STATUS_UNSUPPORTED:  return "unsupported";
        case USBREG_STATUS_UNKNOWN:      return "unknown";
        case USBREG_STATUS_PROBE_FAILED: return "probe-failed";
        case USBREG_STATUS_GONE:         return "gone";
    }
    return "?";
}

void usbreg_dump_kprintf(void) {
    if (!g_inited || usbreg_count() == 0) {
        kprintf("[usbreg] no entries\n");
        return;
    }
    kprintf("[usbreg] dump:\n");
    for (size_t i = 0; i < USBREG_MAX; i++) {
        const struct usbreg_entry *e = &g_table[i];
        if (e->status == USBREG_STATUS_FREE) continue;
        kprintf("  [%2u] slot=%u port=%u depth=%u  "
                "%04x:%04x  cls=%02x/%02x/%02x  "
                "%s  drv=%s  (%s)\n",
                (unsigned)i,
                e->slot_id, e->port_id, e->hub_depth,
                e->vendor, e->product,
                e->dev_class, e->dev_subclass, e->dev_protocol,
                e->friendly, e->driver,
                usbreg_status_name(e->status));
    }
}
