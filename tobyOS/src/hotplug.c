/* hotplug.c -- M26C hot-plug event ring.
 *
 * One global ring of `ABI_DEVT_HOT_RING` events, FIFO, lossy on
 * overflow. The lossy semantics are intentional: hot-plug producers
 * must never block in soft-irq context, so when userland is slow we
 * just bump a `dropped` counter and overwrite the oldest entry. The
 * counter rides along on the next drain so userland can detect lost
 * events without separate plumbing.
 *
 * Synchronisation:
 *   - producers: `hotplug_post()` is called from xhci_poll() (timer
 *     soft-irq), usb_hub_poll() (kernel thread), and driver detach
 *     hooks (any context that can run while a slot disappears).
 *   - consumer: ABI_SYS_HOT_DRAIN, called by libtoby (process ctx).
 * We protect everything with a single irq-off spinlock. Critical
 * sections are tiny -- one struct copy. */

#include <tobyos/hotplug.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/spinlock.h>
#include <tobyos/perf.h>
#include <tobyos/notify.h>
#include <tobyos/abi/abi.h>

/* Static ring -- avoids any heap dependency in early boot, which is
 * important because xhci can fire its first attach event before pmm
 * fully comes up on some hosts. */
static struct {
    spinlock_t          lock;
    uint32_t            head;       /* next slot to write */
    uint32_t            count;      /* live unread entries */
    uint64_t            seq;        /* monotonic id of *next* event */
    uint16_t            dropped;    /* unread overflow counter */
    size_t              total_posted;
    size_t              total_drained;
    size_t              total_dropped;
    struct abi_hot_event ring[ABI_DEVT_HOT_RING];
} g_hot = { .lock = SPINLOCK_INIT };

static inline uint64_t hot_now_ms(void) {
    /* perf_now_ns() returns 0 before perf_init has calibrated the
     * TSC; that's harmless, just yields time_ms=0 for the first few
     * boot events. */
    return perf_now_ns() / 1000000ull;
}

static void hot_copy_info(char *dst, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (src[i] && i + 1 < ABI_HOT_INFO_MAX) {
        dst[i] = src[i]; i++;
    }
    dst[i] = '\0';
}

void hotplug_post(const struct abi_hot_event *ev_in) {
    if (!ev_in) return;

    uint64_t flags = spin_lock_irqsave(&g_hot.lock);

    if (g_hot.count == ABI_DEVT_HOT_RING) {
        /* Full -- overwrite the oldest entry. The slot at
         * (head + count) % N == head is the oldest. */
        g_hot.dropped++;
        g_hot.total_dropped++;
        /* head stays where it is so the new entry overwrites the
         * oldest, then count stays at N. We rotate by advancing the
         * "logical oldest" pointer == (head + count + 1) % N == 
         * head + 1 (since count == N). Easiest: just advance head. */
        struct abi_hot_event *slot = &g_hot.ring[g_hot.head];
        *slot = *ev_in;
        slot->seq     = g_hot.seq++;
        slot->time_ms = hot_now_ms();
        g_hot.head    = (g_hot.head + 1u) % ABI_DEVT_HOT_RING;
        g_hot.total_posted++;
        spin_unlock_irqrestore(&g_hot.lock, flags);
        return;
    }

    /* Normal case: append at (head + count) mod N. */
    uint32_t idx = (g_hot.head + g_hot.count) % ABI_DEVT_HOT_RING;
    struct abi_hot_event *slot = &g_hot.ring[idx];
    *slot = *ev_in;
    slot->seq     = g_hot.seq++;
    slot->time_ms = hot_now_ms();
    g_hot.count++;
    g_hot.total_posted++;

    spin_unlock_irqrestore(&g_hot.lock, flags);
}

void hotplug_post_attach(uint8_t bus, uint16_t slot,
                         uint8_t hub_depth, uint8_t hub_port,
                         const char *info) {
    struct abi_hot_event ev = { 0 };
    ev.bus       = bus;
    ev.action    = ABI_HOT_ATTACH;
    ev.hub_depth = hub_depth;
    ev.hub_port  = hub_port;
    ev.slot      = slot;
    hot_copy_info(ev.info, info);
    hotplug_post(&ev);

    /* M31: surface the device-attach as a desktop notification.
     * notify_post is IRQ-safe (uses spin_lock_irqsave) so it's fine
     * to call from inside the same code path that posts the
     * hot_event ring entry above. */
    {
        char title[ABI_NOTIFY_TITLE_MAX];
        ksnprintf(title, sizeof(title), "Device attached: %s",
                  info && info[0] ? info : "unknown");
        char body[ABI_NOTIFY_BODY_MAX];
        ksnprintf(body, sizeof(body),
                  "bus=%u slot=%u hub.depth=%u hub.port=%u",
                  (unsigned)bus, (unsigned)slot,
                  (unsigned)hub_depth, (unsigned)hub_port);
        notify_post(ABI_NOTIFY_KIND_DEVICE, ABI_NOTIFY_URG_INFO,
                    "hotplug", title, body);
    }
}

void hotplug_post_detach(uint8_t bus, uint16_t slot,
                         uint8_t hub_depth, uint8_t hub_port,
                         const char *info) {
    struct abi_hot_event ev = { 0 };
    ev.bus       = bus;
    ev.action    = ABI_HOT_DETACH;
    ev.hub_depth = hub_depth;
    ev.hub_port  = hub_port;
    ev.slot      = slot;
    hot_copy_info(ev.info, info);
    hotplug_post(&ev);

    {
        char title[ABI_NOTIFY_TITLE_MAX];
        ksnprintf(title, sizeof(title), "Device removed: %s",
                  info && info[0] ? info : "unknown");
        char body[ABI_NOTIFY_BODY_MAX];
        ksnprintf(body, sizeof(body),
                  "bus=%u slot=%u hub.depth=%u hub.port=%u",
                  (unsigned)bus, (unsigned)slot,
                  (unsigned)hub_depth, (unsigned)hub_port);
        notify_post(ABI_NOTIFY_KIND_DEVICE, ABI_NOTIFY_URG_WARN,
                    "hotplug", title, body);
    }
}

int hotplug_drain(struct abi_hot_event *out, int cap) {
    if (!out || cap <= 0) return 0;

    uint64_t flags = spin_lock_irqsave(&g_hot.lock);

    int n = 0;
    while (n < cap && g_hot.count > 0) {
        out[n] = g_hot.ring[g_hot.head];
        g_hot.head  = (g_hot.head + 1u) % ABI_DEVT_HOT_RING;
        g_hot.count--;
        n++;
    }
    /* Stamp accumulated drop count onto the first record we return,
     * then clear it -- userland can detect "we lost X events" without
     * a side channel. If nothing to return, the dropped counter
     * persists for the next drain. */
    if (n > 0 && g_hot.dropped > 0) {
        out[0].dropped = g_hot.dropped;
        g_hot.dropped  = 0;
    }
    g_hot.total_drained += (size_t)n;

    spin_unlock_irqrestore(&g_hot.lock, flags);
    return n;
}

size_t hotplug_total_posted(void)  { return g_hot.total_posted;  }
size_t hotplug_total_drained(void) { return g_hot.total_drained; }
size_t hotplug_total_dropped(void) { return g_hot.total_dropped; }

/* devtest-registered self-test. Synthesizes one ATTACH + one DETACH
 * event, drains them, verifies both made it through with the right
 * sequence/action. PASSes on a clean round-trip; FAILs (with a
 * diagnostic message) otherwise. Exists as much as a "the ring works"
 * smoke test as a regression guard against future refactors. */
int hotplug_selftest(char *msg, size_t cap) {
    size_t posted0  = hotplug_total_posted();
    size_t drained0 = hotplug_total_drained();

    /* Synthetic events on a "synthetic" bus -- use BUS_ALL so they
     * don't pollute any real bus enumeration. */
    hotplug_post_attach(ABI_DEVT_BUS_ALL, 0xAA01, 0, 1, "selftest-attach");
    hotplug_post_detach(ABI_DEVT_BUS_ALL, 0xAA01, 0, 1, "selftest-detach");

    struct abi_hot_event drained[8];
    int got = hotplug_drain(drained, 8);
    if (got < 2) {
        ksnprintf(msg, cap,
                  "drain returned only %d of 2 synthetic events", got);
        return -ABI_EIO;
    }
    /* Locate our two events (there might be real driver events before
     * them if the controller raced us). */
    int saw_a = -1, saw_d = -1;
    for (int i = 0; i < got; i++) {
        if (drained[i].slot == 0xAA01 && drained[i].action == ABI_HOT_ATTACH)
            saw_a = i;
        if (drained[i].slot == 0xAA01 && drained[i].action == ABI_HOT_DETACH)
            saw_d = i;
    }
    if (saw_a < 0 || saw_d < 0) {
        ksnprintf(msg, cap,
                  "drained %d events but synthetic attach/detach missing "
                  "(a=%d d=%d)", got, saw_a, saw_d);
        return -ABI_EIO;
    }
    if (drained[saw_a].seq >= drained[saw_d].seq) {
        ksnprintf(msg, cap,
                  "FIFO order violated: attach seq=%llu >= detach seq=%llu",
                  (unsigned long long)drained[saw_a].seq,
                  (unsigned long long)drained[saw_d].seq);
        return -ABI_EIO;
    }
    ksnprintf(msg, cap,
              "ring=%u capacity=%u posted=%lu drained=%lu dropped=%lu",
              (unsigned)ABI_DEVT_HOT_RING, (unsigned)ABI_DEVT_HOT_RING,
              (unsigned long)(hotplug_total_posted()  - posted0),
              (unsigned long)(hotplug_total_drained() - drained0),
              (unsigned long)hotplug_total_dropped());
    return 0;
}
