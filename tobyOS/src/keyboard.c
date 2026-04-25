/* keyboard.c -- PS/2 keyboard driver, scancode set 1.
 *
 * The PS/2 controller exposes the keyboard at port 0x60 (data) /
 * 0x64 (status+command). On every keypress or release the controller
 * raises IRQ1 with the scancode waiting in port 0x60.
 *
 * Scancode set 1 (the BIOS default and what QEMU emits) layout:
 *   - 1-byte make code on press
 *   - same code | 0x80 on release
 *   - extended keys (arrows, numpad enter, etc.) are prefixed by 0xE0;
 *     we currently track + drop the prefix without producing a char
 *   - 0xE1 sequences (Pause/Break only) are also dropped
 *
 * The handler stays minimal: read the scancode, translate, push into
 * a lock-free SPSC ring buffer, EOI. The main loop drains via
 * kbd_trygetc() / kbd_getc().
 */

#include <tobyos/keyboard.h>
#include <tobyos/isr.h>
#include <tobyos/irq.h>
#include <tobyos/cpu.h>
#include <tobyos/signal.h>
#include <tobyos/gui.h>
#include <tobyos/printk.h>
#include <tobyos/abi/abi.h>
#include <tobyos/klibc.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

#define KBD_BUF_SIZE 256u  /* must be a power of two */

/* Ring buffer. head = producer (IRQ), tail = consumer (main).
 * Single-producer / single-consumer means no locking is required as
 * long as both indices are read/written atomically; uint8_t qualifies. */
static volatile uint8_t g_buf[KBD_BUF_SIZE];
static volatile uint8_t g_head = 0;
static volatile uint8_t g_tail = 0;

/* Modifier state. Updated from the IRQ; read from anywhere. */
static volatile bool g_shift  = false;
static volatile bool g_ctrl   = false;
static volatile bool g_caps   = false;
static volatile bool g_ext_e0 = false;  /* next byte is an E0 extended code */

/* M26D telemetry. Read-only counters incremented from the IRQ + the
 * shared dispatch sink. They saturate; no consumer cares about exact
 * overflow semantics. */
static volatile uint64_t g_chars_dispatched;
static volatile uint64_t g_irqs_total;
static volatile uint8_t  g_last_scancode;
static volatile char     g_last_char;

/* US-QWERTY scancode -> ASCII (base). 0 = unmapped/none. */
static const char g_map_base[128] = {
/* 0x00 */  0,    27,  '1', '2', '3', '4', '5', '6',
/* 0x08 */ '7',  '8', '9', '0', '-', '=', '\b','\t',
/* 0x10 */ 'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',
/* 0x18 */ 'o',  'p', '[', ']','\n',  0,  'a', 's',
/* 0x20 */ 'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',
/* 0x28 */ '\'', '`',  0, '\\', 'z', 'x', 'c', 'v',
/* 0x30 */ 'b',  'n', 'm', ',', '.', '/',  0,  '*',
/* 0x38 */  0,   ' ',  0,   0,   0,   0,   0,   0,
/* 0x40..0x7F: function/numpad/etc -- unmapped */
};

/* Same layout, with shift held. */
static const char g_map_shift[128] = {
/* 0x00 */  0,    27,  '!', '@', '#', '$', '%', '^',
/* 0x08 */ '&',  '*', '(', ')', '_', '+', '\b','\t',
/* 0x10 */ 'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
/* 0x18 */ 'O',  'P', '{', '}','\n',  0,  'A', 'S',
/* 0x20 */ 'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',
/* 0x28 */ '"',  '~',  0,  '|', 'Z', 'X', 'C', 'V',
/* 0x30 */ 'B',  'N', 'M', '<', '>', '?',  0,  '*',
/* 0x38 */  0,   ' ',  0,   0,   0,   0,   0,   0,
};

static void buf_push(char c) {
    uint8_t next = (uint8_t)((g_head + 1u) & (KBD_BUF_SIZE - 1u));
    if (next == g_tail) {
        return;  /* buffer full -- drop the oldest-most-recent char */
    }
    g_buf[g_head] = (uint8_t)c;
    g_head = next;
}

int kbd_trygetc(void) {
    if (g_head == g_tail) return -1;
    char c = (char)g_buf[g_tail];
    g_tail = (uint8_t)((g_tail + 1u) & (KBD_BUF_SIZE - 1u));
    return (uint8_t)c;
}

int kbd_getc(void) {
    int c;
    while ((c = kbd_trygetc()) < 0) {
        /* Idle while we wait for IRQ1 (or any other IRQ -- PIT will
         * wake us at least every 10 ms so we're never stuck > tick). */
        hlt();
    }
    return c;
}

/* Status register bits (mirror of mouse.c -- the 8042 status reg is
 * shared, and we need to know when a byte sitting in port 0x60 is
 * actually an AUX-port (mouse) byte instead of a keyboard scancode. */
#define KBD_STATUS_OUT_FULL  0x01
#define KBD_STATUS_AUX_DATA  0x20

static void kbd_irq(struct regs *r) {
    (void)r;

    /* Spurious / mis-routed: if there's nothing in the output buffer, or
     * the byte sitting there belongs to the AUX port (mouse), this IRQ
     * is not for us. Bailing out (without reading 0x60) keeps the byte
     * available for mouse_irq, which knows how to handle it. */
    uint8_t status = inb(KBD_STATUS);
    if ((status & KBD_STATUS_OUT_FULL) == 0 ||
        (status & KBD_STATUS_AUX_DATA) != 0) {
        goto eoi;
    }

    uint8_t sc = inb(KBD_DATA);
    g_irqs_total++;
    g_last_scancode = sc;

    /* VERBOSE-only scancode trace. Lets us see exactly what bytes the
     * keyboard controller delivers, which is invaluable for figuring out
     * why an emergency hotkey "didn't work" -- on Windows hosts QEMU's
     * GTK display intercepts F11 (full-screen toggle) and F12, so the
     * scancode literally never reaches the guest. With trace verbose
     * the user can confirm: if the IRQ never fires for a key, the host
     * is eating it. */
    if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("kbd_irq raw scancode 0x%02x (e0=%d)",
                       (unsigned)sc, (int)g_ext_e0);
    }

    /* Multi-byte sequences: track the prefix, then drop the next byte.
     * 0xE1 is only sent for Pause/Break (E1 1D 45 / E1 9D C5). We turn
     * the FIRST byte (0xE1) into an emergency exit -- it's effectively
     * unused for anything else, and Pause is virtually never grabbed by
     * the host. */
    if (sc == 0xE0) { g_ext_e0 = true;  goto eoi; }
    if (sc == 0xE1) {
        gui_emergency_exit("Pause/Break hotkey");
        goto eoi;
    }
    if (g_ext_e0)   { g_ext_e0 = false; goto eoi; }

    bool released = (sc & 0x80) != 0;
    uint8_t code  = (uint8_t)(sc & 0x7F);

    /* Modifier tracking (must observe both press AND release). */
    switch (code) {
    case 0x2A: case 0x36: g_shift = !released; goto eoi;  /* L/R Shift */
    case 0x1D:            g_ctrl  = !released; goto eoi;  /* L Ctrl    */
    case 0x3A:
        if (!released) g_caps = !g_caps;
        goto eoi;                                          /* Caps Lock */
    default: break;
    }

    if (released) goto eoi;
    if (code >= 128) goto eoi;

    /* Hot keys that bypass the normal char map. They fire on press only
     * and are intentionally hard-wired so a frozen GUI app cannot
     * swallow them. We register a generous palette because some hosts
     * (notably QEMU+GTK on Windows) silently swallow F11/F12 for
     * window-manager actions (full-screen toggle), so users get back to
     * the shell via whichever key actually makes it through:
     *
     *   F1  / F11        -> dump GUI status to serial
     *   F2  / F12        -> force-exit desktop + SIGINT all GUI apps
     *   ScrollLock       -> dump GUI status (alt for F1)
     *   Pause/Break (E1) -> force-exit desktop (handled above)
     */
    if (code == 0x58 /* F12 */ || code == 0x3C /* F2 */) {
        gui_emergency_exit(code == 0x58 ? "F12 hotkey" : "F2 hotkey");
        goto eoi;
    }
    if (code == 0x57 /* F11 */ || code == 0x3B /* F1 */) {
        gui_dump_status(code == 0x57 ? "F11 hotkey" : "F1 hotkey");
        goto eoi;
    }
    if (code == 0x46 /* ScrollLock */) {
        gui_dump_status("ScrollLock hotkey");
        goto eoi;
    }

    char c = g_shift ? g_map_shift[code] : g_map_base[code];
    if (c == 0) goto eoi;

    /* Caps Lock affects letters only, and inverts the shift state for
     * them. (Numbers/symbols stay the same.) */
    if (g_caps && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    else if (g_caps && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    /* Ctrl-A..Ctrl-Z -> 0x01..0x1A */
    if (g_ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        c = (char)((c & 0x1F));
    }

    kbd_dispatch_char(c);

eoi:
    irq_eoi_isa(1);
}

/* Shared post-resolution dispatch -- exported so USB-HID can plug
 * in identically once it's translated its boot-protocol reports
 * into ASCII. See keyboard.h for semantics. */
void kbd_dispatch_char(char c) {
    if (c == 0) return;
    g_chars_dispatched++;
    g_last_char = c;
    /* M26D: every 16th char OR the very first dispatched char emits a
     * one-line snapshot to the serial log. The "first" trigger is
     * what makes "PS/2 still alive" verifiable without typing a
     * paragraph; the every-16th cadence keeps interactive use to
     * roughly one line per typed word. */
    if (g_chars_dispatched == 1ull || (g_chars_dispatched & 15ull) == 0ull) {
        kprintf("[input] kbd_dispatch chars=%lu irqs=%lu shift=%d ctrl=%d "
                "caps=%d last_sc=0x%02x last_ch=0x%02x\n",
                (unsigned long)g_chars_dispatched,
                (unsigned long)g_irqs_total,
                g_shift ? 1 : 0, g_ctrl ? 1 : 0, g_caps ? 1 : 0,
                (unsigned)g_last_scancode, (unsigned)(uint8_t)g_last_char);
    }

    /* Ctrl+C: do NOT push to the input buffer. Instead, deliver SIGINT
     * to whatever process the shell currently has marked as
     * foreground. If there is no foreground proc (signal_get_foreground
     * == 0, meaning the shell prompt itself is the focus), the signal
     * is silently dropped -- we don't want Ctrl+C to kill the shell. */
    if (c == 0x03) {
        int fg = signal_get_foreground();
        if (fg > 0) signal_send_to_pid(fg, SIGINT);
        return;
    }

    /* Milestone 11: when the GUI is active, route all printable +
     * editing keystrokes to the focused window's event queue instead
     * of the text-mode shell ring. This way the shell never sees
     * "leftover" characters when the GUI app exits and pid 0 wakes
     * back up to drain the ring. Ctrl+C handling above still runs
     * first, so a user can always kill a misbehaving GUI program. */
    if (gui_active()) {
        gui_post_key((uint8_t)c);
        return;
    }

    buf_push(c);
}

/* Drain anything the BIOS / Limine left waiting in the controller's
 * output buffer before we registered the handler -- otherwise IRQ1
 * may never re-assert. */
static void kbd_drain_input(void) {
    /* Status bit 0 (OBF) = output buffer full. */
    while (inb(KBD_STATUS) & 0x01) {
        (void)inb(KBD_DATA);
    }
}

void kbd_init(void) {
    kbd_drain_input();
    /* Facade picks PIC vs IO APIC based on whether
     * irq_switch_to_ioapic() has run yet. Same IDT vector either way. */
    irq_install_isa(1, kbd_irq);
    kprintf("[kbd] PS/2 driver up (IRQ1 unmasked)\n");
}

/* ============================================================== */
/* M26D telemetry accessors                                        */
/* ============================================================== */

uint64_t kbd_chars_dispatched(void) { return g_chars_dispatched; }
uint64_t kbd_irqs_total(void)       { return g_irqs_total; }
bool     kbd_caps_state(void)       { return g_caps; }
bool     kbd_shift_state(void)      { return g_shift; }
bool     kbd_ctrl_state(void)       { return g_ctrl; }
uint8_t  kbd_last_scancode(void)    { return g_last_scancode; }
