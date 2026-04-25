/* safemode.h -- Milestone 28D + M29C: minimal recovery boot mode.
 *
 * M28D introduced a single boolean "safe mode active" flag. M29C
 * extends the same machinery with multiple levels:
 *
 *   SAFE_NONE   normal boot
 *   SAFE_BASIC  minimal drivers + kernel shell (skips USB, audio, NIC,
 *               GUI, services, login). Lands the operator in /bin/safesh.
 *   SAFE_GUI    BASIC + framebuffer + input + desktop (skips USB
 *               beyond HID-class via xHCI, audio, NIC, services).
 *
 * Selection is done at boot time by ANY of:
 *
 *   - the read-only initrd containing /etc/safemode_level whose first
 *     non-whitespace word is "basic" or "gui" (preferred path; built
 *     with `make SAFEMODE_LEVEL=basic` or `SAFEMODE_LEVEL=gui`)
 *   - the legacy /etc/safemode_now flag (M28D) -> treated as BASIC
 *   - safemode_force(true) at runtime -> treated as BASIC
 *
 * safemode_active() / safemode_level() are cheap (single global read)
 * and can be called from anywhere after early_init has run
 * safemode_init(). Callers MUST NOT cache the result before init --
 * query each time.
 */

#ifndef TOBYOS_SAFEMODE_H
#define TOBYOS_SAFEMODE_H

#include <tobyos/types.h>

/* M29C+M35E boot levels. Numeric values are stable: tests grep for them. */
enum safemode_level {
    SAFEMODE_LEVEL_NONE          = 0,   /* normal boot                   */
    SAFEMODE_LEVEL_BASIC         = 1,   /* M29C: minimal drivers + shell */
    SAFEMODE_LEVEL_GUI           = 2,   /* M29C: + framebuffer/input/GUI */
    SAFEMODE_LEVEL_COMPATIBILITY = 3,   /* M35E: GUI+net+services but
                                         * skips audio, non-HID USB,
                                         * and the virtio-gpu fast path.
                                         * Designed for "weird hardware
                                         * that boots with safe-gui but
                                         * we want net + services". */
};

/* One-shot: parse the cmdline + look for the initrd flags, latch the
 * result. Idempotent (subsequent calls are no-ops). Logs the level it
 * picked. */
void safemode_init(void);

/* Has safemode_init been called? */
bool safemode_ready(void);

/* True if we are running in any safe mode (BASIC or GUI). Cheap. */
bool safemode_active(void);

/* M29C: return the latched safe-mode level. */
enum safemode_level safemode_level(void);

/* Convenience predicates the boot path uses to gate optional
 * subsystems. All return true iff the subsystem must be SKIPPED for
 * the current boot mode.
 *
 *                               NONE BASIC GUI COMPAT
 *   safemode_skip_usb_full()    no   yes   yes  yes   (the whole stack)
 *   safemode_skip_usb_extra()   no   yes   yes  yes   (non-HID classes)
 *   safemode_skip_net()         no   yes   yes  no    (M35E re-enables)
 *   safemode_skip_audio()       no   yes   yes  yes
 *   safemode_skip_gui()         no   yes   no   no
 *   safemode_skip_services()    no   yes   no   no
 *   safemode_skip_virtio_gpu()  no   yes   yes  yes   (M35E: framebuffer-only)
 *
 * In NONE mode every helper returns false, so the boot path is
 * unchanged. The legacy single-knob safemode_skip_usb() is kept as
 * an alias of skip_usb_full() so callers from M28D/M29C compile
 * unchanged. */
bool safemode_skip_usb(void);          /* legacy alias */
bool safemode_skip_usb_full(void);     /* skip xHCI + class drivers */
bool safemode_skip_usb_extra(void);    /* skip non-HID USB only */
bool safemode_skip_net(void);
bool safemode_skip_audio(void);
bool safemode_skip_gui(void);
bool safemode_skip_services(void);
bool safemode_skip_virtio_gpu(void);   /* M35E: prefer framebuffer */

/* M35E: emit the resolved per-subsystem skip policy as kprintf lines.
 * Called automatically at the tail of safemode_init() and exposed for
 * the selftest harness so it can re-print the table on demand. */
void safemode_dump_policy(void);

/* Forced boot tag for the GUI/safesh banner. Returns "safe-basic",
 * "safe-gui", "compatibility", or "normal". */
const char *safemode_tag(void);

/* M35E: stable mapping safemode_level -> ABI_BOOT_MODE_* for the
 * bootdiag struct + userland queries. Returns ABI_BOOT_MODE_NORMAL
 * for SAFEMODE_LEVEL_NONE. */
uint32_t safemode_to_boot_mode(enum safemode_level lvl);

/* Force on/off (used by tests + the recovery menu). When `on` is
 * true we go to SAFE_BASIC; finer control should call safemode_force_level. */
void safemode_force(bool on);

/* Force a specific level. Used by the M29C harness + recovery menu. */
void safemode_force_level(enum safemode_level lvl);

#endif /* TOBYOS_SAFEMODE_H */
