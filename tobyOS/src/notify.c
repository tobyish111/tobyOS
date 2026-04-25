/* notify.c -- M31 desktop notification daemon (in-kernel ring).
 *
 * Implementation overview
 * -----------------------
 * One global ring of NOTIFY_MAX entries, indexed by a monotonic
 * publish counter. The "newest first" snapshot the compositor wants
 * is computed by walking backwards from `g.next_id - 1` and
 * translating each id into its slot (`id % NOTIFY_MAX`). Entries
 * older than NOTIFY_MAX worth of posts are silently overwritten;
 * we only count the eviction in stats and move on.
 *
 * Concurrency
 * -----------
 * notify_post is callable from any context (IRQ, syscall, kthread).
 * We protect the ring with a single spinlock, and never call into
 * kmalloc / sched / printk-with-allocation while it is held. The
 * other entry points (snapshot / dismiss / iterators) only run from
 * the compositor on pid 0, but they all take the lock too -- the
 * cost is a single atomic test under a contended access pattern
 * we never expect to actually contend.
 *
 * The ring is small enough (32 * 200 = 6.4 KiB) that "snapshot the
 * whole thing into the caller's buffer" is the natural read API. We
 * do not try to expose a stateful cursor.
 *
 * Strings are copied with bounded copies (notify_strncpy_safe), and
 * entries are zero-initialised on overwrite so leftover bytes from
 * an old entry never bleed into the new one's _reserved tail.
 */

#include <tobyos/notify.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>
#include <tobyos/pit.h>
#include <tobyos/service.h>

/* ---- module state ------------------------------------------------- */

struct notify_slot {
    struct abi_notification rec;
    bool                    in_use;
};

static struct {
    bool                ready;
    spinlock_t          lock;
    struct notify_slot  ring[NOTIFY_MAX];
    uint32_t            next_id;          /* monotonic, 1..              */
    uint32_t            head;             /* next write slot              */
    uint32_t            posted;           /* lifetime post count          */
    uint32_t            evicted;          /* overwritten without dismiss  */
    uint32_t            dismissed;        /* user-dismissed lifetime      */
} g;

/* ---- helpers ------------------------------------------------------ */

static void notify_strncpy_safe(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static uint64_t now_ms(void) {
    uint32_t hz = pit_hz();
    if (hz == 0) return 0;
    return (pit_ticks() * 1000ull) / (uint64_t)hz;
}

/* ---- lifecycle ---------------------------------------------------- */

void notify_init(void) {
    memset(&g, 0, sizeof(g));
    spin_init(&g.lock);
    g.next_id = 1;       /* 0 is reserved as "no id" */
    g.ready   = true;

    /* Best-effort: register as a built-in service so `services` shows
     * "notify" in the registry. service.c may not be initialised in
     * every diagnostic build, so we tolerate the failure quietly. */
    (void)service_register_builtin("notify");

    kprintf("[notify] daemon ready (ring depth %u)\n", (unsigned)NOTIFY_MAX);
}

/* ---- emit --------------------------------------------------------- */

uint32_t notify_post(uint32_t kind,
                     uint32_t urgency,
                     const char *app,
                     const char *title,
                     const char *body)
{
    if (!g.ready) return 0;

    /* Clamp urgency to the legal range so callers can't smuggle a
     * huge value into a future palette index. */
    if (urgency > NOTIFY_URG_ERR) urgency = NOTIFY_URG_ERR;

    uint64_t flags = spin_lock_irqsave(&g.lock);

    uint32_t slot = g.head % NOTIFY_MAX;
    struct notify_slot *s = &g.ring[slot];
    if (s->in_use) g.evicted++;

    memset(&s->rec, 0, sizeof(s->rec));
    s->rec.id      = g.next_id++;
    s->rec.kind    = kind;
    s->rec.urgency = urgency;
    s->rec.flags   = 0;
    s->rec.time_ms = now_ms();
    notify_strncpy_safe(s->rec.app,   sizeof(s->rec.app),   app);
    notify_strncpy_safe(s->rec.title, sizeof(s->rec.title), title);
    notify_strncpy_safe(s->rec.body,  sizeof(s->rec.body),  body);
    s->in_use = true;

    g.head++;
    g.posted++;

    uint32_t id = s->rec.id;
    spin_unlock_irqrestore(&g.lock, flags);

    /* Mirror to serial so headless test harnesses can grep for it. */
    kprintf("[notify #%u urg=%u kind=%u] %s: %s%s%s\n",
            (unsigned)id, (unsigned)urgency, (unsigned)kind,
            (app && app[0]) ? app : "?",
            (title && title[0]) ? title : "(no title)",
            (body && body[0]) ? " -- " : "",
            (body && body[0]) ? body : "");
    return id;
}

/* ---- read --------------------------------------------------------- */

uint32_t notify_get_records(struct abi_notification *out, uint32_t cap) {
    if (!out || cap == 0 || !g.ready) return 0;
    uint64_t flags = spin_lock_irqsave(&g.lock);
    /* Walk backwards from the most recent post for "newest first". */
    uint32_t written = 0;
    uint32_t scan = g.head;
    /* head is the NEXT slot to write -- previous post is at head-1.
     * Iterate up to NOTIFY_MAX times so we never wrap past valid
     * data, even if posted < NOTIFY_MAX. */
    for (uint32_t i = 0; i < NOTIFY_MAX && written < cap; i++) {
        if (scan == 0) break;
        scan--;
        struct notify_slot *s = &g.ring[scan % NOTIFY_MAX];
        if (!s->in_use) continue;
        if (s->rec.flags & ABI_NOTIFY_FLAG_DISMISSED) continue;
        out[written++] = s->rec;
    }
    spin_unlock_irqrestore(&g.lock, flags);
    return written;
}

bool notify_pop_pending_toast(struct abi_notification *out) {
    if (!out || !g.ready) return false;
    uint64_t flags = spin_lock_irqsave(&g.lock);
    bool found = false;
    /* Walk newest-first; pull the freshest entry that hasn't been
     * shown as a toast yet AND hasn't been dismissed. */
    uint32_t scan = g.head;
    for (uint32_t i = 0; i < NOTIFY_MAX; i++) {
        if (scan == 0) break;
        scan--;
        struct notify_slot *s = &g.ring[scan % NOTIFY_MAX];
        if (!s->in_use) continue;
        if (s->rec.flags & ABI_NOTIFY_FLAG_DISMISSED) continue;
        if (s->rec.flags & ABI_NOTIFY_FLAG_DISPLAYED) continue;
        s->rec.flags |= ABI_NOTIFY_FLAG_DISPLAYED;
        *out = s->rec;
        found = true;
        break;
    }
    spin_unlock_irqrestore(&g.lock, flags);
    return found;
}

/* ---- dismiss ------------------------------------------------------ */

void notify_dismiss(uint32_t id) {
    if (!g.ready || id == 0) return;
    uint64_t flags = spin_lock_irqsave(&g.lock);
    for (uint32_t i = 0; i < NOTIFY_MAX; i++) {
        struct notify_slot *s = &g.ring[i];
        if (!s->in_use) continue;
        if (s->rec.id != id) continue;
        if (!(s->rec.flags & ABI_NOTIFY_FLAG_DISMISSED)) {
            s->rec.flags |= ABI_NOTIFY_FLAG_DISMISSED;
            g.dismissed++;
        }
        break;
    }
    spin_unlock_irqrestore(&g.lock, flags);
}

void notify_dismiss_all(void) {
    if (!g.ready) return;
    uint64_t flags = spin_lock_irqsave(&g.lock);
    for (uint32_t i = 0; i < NOTIFY_MAX; i++) {
        struct notify_slot *s = &g.ring[i];
        if (!s->in_use) continue;
        if (!(s->rec.flags & ABI_NOTIFY_FLAG_DISMISSED)) {
            s->rec.flags |= ABI_NOTIFY_FLAG_DISMISSED;
            g.dismissed++;
        }
    }
    spin_unlock_irqrestore(&g.lock, flags);
}

/* ---- diagnostics -------------------------------------------------- */

/* Internal: count live (non-dismissed) entries with the lock already
 * held. Avoids the recursive lock that the public unread_count would
 * otherwise hit when called from notify_dump. */
static uint32_t notify_unread_count_locked(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < NOTIFY_MAX; i++) {
        struct notify_slot *s = &g.ring[i];
        if (!s->in_use) continue;
        if (!(s->rec.flags & ABI_NOTIFY_FLAG_DISMISSED)) n++;
    }
    return n;
}

uint32_t notify_unread_count(void) {
    if (!g.ready) return 0;
    uint64_t flags = spin_lock_irqsave(&g.lock);
    uint32_t n = notify_unread_count_locked();
    spin_unlock_irqrestore(&g.lock, flags);
    return n;
}

uint32_t notify_total_posted(void) { return g.posted; }

void notify_dump(void) {
    uint64_t flags = spin_lock_irqsave(&g.lock);
    kprintf("[notify] posted=%u evicted=%u dismissed=%u unread=%u "
            "next_id=%u head=%u\n",
            (unsigned)g.posted, (unsigned)g.evicted,
            (unsigned)g.dismissed, (unsigned)notify_unread_count_locked(),
            (unsigned)g.next_id, (unsigned)g.head);
    for (uint32_t i = 0; i < NOTIFY_MAX; i++) {
        struct notify_slot *s = &g.ring[i];
        if (!s->in_use) continue;
        kprintf("  [#%u urg=%u kind=%u flags=0x%x t=%llu] %s: %s\n",
                (unsigned)s->rec.id, (unsigned)s->rec.urgency,
                (unsigned)s->rec.kind, (unsigned)s->rec.flags,
                (unsigned long long)s->rec.time_ms,
                s->rec.app[0] ? s->rec.app : "?",
                s->rec.title);
    }
    spin_unlock_irqrestore(&g.lock, flags);
}
