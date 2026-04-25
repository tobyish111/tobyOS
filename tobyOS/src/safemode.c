/* safemode.c -- M28D + M29C + M35E: minimal recovery boot mode.
 *
 * The M28D implementation latched a single boolean for "safe mode
 * active". M29C generalised that into multiple levels (BASIC, GUI),
 * and M35E adds a third tier -- COMPATIBILITY -- positioned between
 * GUI and NORMAL: full GUI + networking + services, but still drops
 * the optional drivers that are most likely to misbehave on quirky
 * hardware (audio, non-HID USB, virtio-gpu).
 *
 * Selection priority (first match wins):
 *
 *   1. /etc/safemode_level (M29C, preferred): first non-whitespace
 *      word is "basic"          / "1" -> SAFEMODE_LEVEL_BASIC,
 *                  "gui"        / "2" -> SAFEMODE_LEVEL_GUI,
 *                  "compatibility"|"compat" / "3" -> SAFEMODE_LEVEL_COMPATIBILITY.
 *   2. /etc/safemode_now    (M28D legacy)    -> SAFEMODE_LEVEL_BASIC.
 *   3. otherwise                              -> SAFEMODE_LEVEL_NONE.
 *
 * Latching is one-shot: once safemode_init() runs the answer is
 * frozen for the remainder of the boot. Tests + the recovery menu
 * can still call safemode_force_level() to override post-init.
 */

#include <tobyos/safemode.h>
#include <tobyos/printk.h>
#include <tobyos/vfs.h>
#include <tobyos/slog.h>
#include <tobyos/klibc.h>
#include <tobyos/abi/abi.h>

static bool                 g_ready  = false;
static enum safemode_level  g_level  = SAFEMODE_LEVEL_NONE;

bool safemode_ready(void)  { return g_ready; }
bool safemode_active(void) { return g_level != SAFEMODE_LEVEL_NONE; }
enum safemode_level safemode_level(void) { return g_level; }

const char *safemode_tag(void) {
    switch (g_level) {
    case SAFEMODE_LEVEL_BASIC:         return "safe-basic";
    case SAFEMODE_LEVEL_GUI:           return "safe-gui";
    case SAFEMODE_LEVEL_COMPATIBILITY: return "compatibility";
    default:                           return "normal";
    }
}

uint32_t safemode_to_boot_mode(enum safemode_level lvl) {
    switch (lvl) {
    case SAFEMODE_LEVEL_BASIC:         return ABI_BOOT_MODE_SAFE_BASIC;
    case SAFEMODE_LEVEL_GUI:           return ABI_BOOT_MODE_SAFE_GUI;
    case SAFEMODE_LEVEL_COMPATIBILITY: return ABI_BOOT_MODE_COMPATIBILITY;
    default:                           return ABI_BOOT_MODE_NORMAL;
    }
}

/* Per-level skip predicates. Kept tabular so the comment block in the
 * header stays in lockstep with the actual behaviour. */
bool safemode_skip_usb_full(void) {
    /* Disable the entire USB stack only when running stripped-down BASIC.
     * GUI keeps HID for a usable desktop; COMPATIBILITY also keeps HID
     * but drops everything else through skip_usb_extra. */
    return g_level == SAFEMODE_LEVEL_BASIC;
}
bool safemode_skip_usb_extra(void) {
    /* Skip non-HID USB classes (storage, audio, printer, ...). True
     * in BASIC, GUI, and COMPATIBILITY. NORMAL allows everything. */
    return g_level != SAFEMODE_LEVEL_NONE;
}
bool safemode_skip_usb(void) {
    /* Legacy M28D entrypoint -- treat as "skip the entire stack" for
     * back-compat with the original boot path. New callers should use
     * the more precise predicate. */
    return safemode_skip_usb_full();
}
bool safemode_skip_net(void) {
    /* M35E: COMPATIBILITY keeps networking up so the operator can
     * still pull updates / report state from a quirky machine. */
    return (g_level == SAFEMODE_LEVEL_BASIC) ||
           (g_level == SAFEMODE_LEVEL_GUI);
}
bool safemode_skip_audio(void) {
    /* Audio is the single most-likely-to-explode driver; suppress in
     * any safe mode. */
    return g_level != SAFEMODE_LEVEL_NONE;
}
bool safemode_skip_gui(void) {
    return g_level == SAFEMODE_LEVEL_BASIC;
}
bool safemode_skip_services(void) {
    /* Services run in NORMAL, GUI, and COMPATIBILITY -- only BASIC
     * lands the operator straight in safesh. */
    return g_level == SAFEMODE_LEVEL_BASIC;
}
bool safemode_skip_virtio_gpu(void) {
    /* M35E: prefer the Limine framebuffer over virtio-gpu in any
     * safe mode -- the firmware-provided FB is the most-tested code
     * path on every host we ship for. */
    return g_level != SAFEMODE_LEVEL_NONE;
}

void safemode_force_level(enum safemode_level lvl) {
    g_level = lvl;
    g_ready = true;
    kprintf("[safe] forced -> %s (level=%u)\n", safemode_tag(),
            (unsigned)lvl);
    if (slog_ready()) {
        SLOG_WARN(SLOG_SUB_SAFE, "safemode forced %s (level=%u)",
                  safemode_tag(), (unsigned)lvl);
    }
}

void safemode_force(bool on) {
    safemode_force_level(on ? SAFEMODE_LEVEL_BASIC : SAFEMODE_LEVEL_NONE);
}

/* Read a small file into a caller-provided buffer (no heap allocation,
 * since safemode runs before the heap is necessarily healthy in the
 * recovery path). Returns the byte count (always < cap, with a NUL
 * terminator past the last byte) or <= 0 on error. */
static long read_small_file(const char *path, char *buf, size_t cap) {
    if (!path || !buf || cap == 0) return -1;
    struct vfs_file f;
    if (vfs_open(path, &f) != VFS_OK) return -1;
    long n = vfs_read(&f, buf, cap - 1);
    vfs_close(&f);
    if (n < 0) return n;
    if ((size_t)n >= cap) n = (long)(cap - 1);
    buf[n] = '\0';
    return n;
}

static enum safemode_level parse_level(const char *txt) {
    if (!txt) return SAFEMODE_LEVEL_NONE;
    while (*txt == ' ' || *txt == '\t' || *txt == '\r' || *txt == '\n') txt++;
    if (!strncmp(txt, "basic",         5) || txt[0] == '1')
        return SAFEMODE_LEVEL_BASIC;
    if (!strncmp(txt, "gui",           3) || txt[0] == '2')
        return SAFEMODE_LEVEL_GUI;
    /* M35E: "compatibility" / "compat" / "3" -- both spellings,
     * because operators tend to type whichever is muscle-memory
     * (and the second form lets us stay short in scripts). */
    if (!strncmp(txt, "compatibility", 13) ||
        !strncmp(txt, "compat",         6) ||
        txt[0] == '3')
        return SAFEMODE_LEVEL_COMPATIBILITY;
    if (!strncmp(txt, "none",  4) || txt[0] == '0') return SAFEMODE_LEVEL_NONE;
    return SAFEMODE_LEVEL_BASIC;
}

void safemode_init(void) {
    if (g_ready) return;
    g_ready = true;
    g_level = SAFEMODE_LEVEL_NONE;

    char buf[32];
    long n = read_small_file("/etc/safemode_level", buf, sizeof(buf));
    if (n > 0) {
        g_level = parse_level(buf);
        kprintf("[safe] /etc/safemode_level=\"%.*s\" -> %s\n",
                (int)n, buf, safemode_tag());
    } else {
        struct vfs_stat st;
        if (vfs_stat("/etc/safemode_now", &st) == VFS_OK) {
            g_level = SAFEMODE_LEVEL_BASIC;
            kprintf("[safe] /etc/safemode_now present -- "
                    "SAFE MODE ACTIVE (basic, M28D legacy)\n");
        } else {
            kprintf("[safe] no safemode flag -- normal boot\n");
        }
    }

    if (slog_ready()) {
        SLOG_INFO(SLOG_SUB_SAFE, "boot mode = %s (level=%u)",
                  safemode_tag(), (unsigned)g_level);
    }

    /* M35E: dump the *resolved* per-subsystem policy so the operator
     * (and the m35e selftest) can see exactly what this boot profile
     * will skip. The order here matches the table in safemode.h so the
     * docs and the runtime stay in lockstep. */
    safemode_dump_policy();
}

void safemode_dump_policy(void) {
    kprintf("[safe] policy: mode=%s level=%u boot_mode=%u\n",
            safemode_tag(), (unsigned)g_level,
            (unsigned)safemode_to_boot_mode(g_level));
    kprintf("[safe] policy: usb_full=%s usb_extra=%s net=%s audio=%s "
            "gui=%s services=%s virtio_gpu=%s\n",
            safemode_skip_usb_full()    ? "skip" : "keep",
            safemode_skip_usb_extra()   ? "skip" : "keep",
            safemode_skip_net()         ? "skip" : "keep",
            safemode_skip_audio()       ? "skip" : "keep",
            safemode_skip_gui()         ? "skip" : "keep",
            safemode_skip_services()    ? "skip" : "keep",
            safemode_skip_virtio_gpu()  ? "skip" : "keep");
}
