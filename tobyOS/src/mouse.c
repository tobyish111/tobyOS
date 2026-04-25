/* mouse.c -- PS/2 AUX-port mouse driver (IRQ12).
 *
 * Init flow (additive -- doesn't touch the keyboard side of the 8042):
 *   1. Send 0xA8 to the controller -- enables the AUX port.
 *   2. Read the controller config byte (cmd 0x20), set bit 1 (IRQ12
 *      enable), clear bit 5 (mouse-clock-disable). Write it back
 *      with cmd 0x60.
 *   3. AUX-write 0xF6 -- "set defaults" (3-byte packets, 100 Hz).
 *      Wait for the 0xFA ACK.
 *   4. AUX-write 0xF4 -- "enable data reporting". Wait for 0xFA.
 *   5. Drain anything stuck in the output buffer, install the IRQ12
 *      handler, unmask IRQ12 and the cascade IRQ2 on the master PIC.
 *
 * Packet format (3 bytes, no wheel):
 *   byte 0 -- flags:
 *     bit 0  left button
 *     bit 1  right button
 *     bit 2  middle button
 *     bit 3  ALWAYS 1 -- if cleared we're out of sync; flush + restart
 *     bit 4  X sign  (1 = dx is negative)
 *     bit 5  Y sign  (1 = dy is negative)
 *     bit 6  X overflow
 *     bit 7  Y overflow
 *   byte 1 -- dx (extend through bit 4 of byte 0 -> 9-bit signed)
 *   byte 2 -- dy (extend through bit 5 of byte 0 -> 9-bit signed)
 *
 * The PS/2 "y up = positive" convention is the opposite of every
 * framebuffer screen, so we negate dy before forwarding to the GUI.
 */

#include <tobyos/mouse.h>
#include <tobyos/cpu.h>
#include <tobyos/isr.h>
#include <tobyos/irq.h>
#include <tobyos/pic.h>
#include <tobyos/printk.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_CMD    0x64

/* Status register bits. */
#define KBD_STATUS_OUT_FULL  0x01
#define KBD_STATUS_IN_FULL   0x02
#define KBD_STATUS_AUX_DATA  0x20  /* set when output buffer holds AUX byte */

/* Controller commands (write to 0x64). */
#define CMD_READ_CONFIG    0x20
#define CMD_WRITE_CONFIG   0x60
#define CMD_DISABLE_AUX    0xA7
#define CMD_ENABLE_AUX     0xA8
#define CMD_DISABLE_KBD    0xAD
#define CMD_ENABLE_KBD     0xAE
#define CMD_AUX_WRITE_NEXT 0xD4

/* AUX device commands (write via CMD_AUX_WRITE_NEXT then to 0x60). */
#define AUX_SET_DEFAULTS   0xF6
#define AUX_ENABLE_REPORT  0xF4
#define AUX_ACK            0xFA

/* Config-byte bits we care about. */
#define CFG_IRQ12_ENABLE   (1u << 1)
#define CFG_MOUSE_DISABLE  (1u << 5)

/* ---- packet assembly ------------------------------------------ */

static volatile uint8_t g_pkt[3];
static volatile uint8_t g_pkt_pos = 0;
static volatile uint8_t g_buttons = 0;

/* M26D telemetry. Saturating counters + last-event snapshot. */
static volatile uint64_t g_events_total;
static volatile uint64_t g_btn_press_total;
static volatile uint64_t g_dx_abs_total;
static volatile uint64_t g_dy_abs_total;
static volatile int8_t   g_last_dx;
static volatile int8_t   g_last_dy;
static volatile uint8_t  g_last_buttons;

/* Default no-op callback so we never have to NULL-check in the IRQ. */
static void noop_cb(int dx, int dy, uint8_t b) { (void)dx; (void)dy; (void)b; }
static mouse_event_fn g_cb = noop_cb;

/* ---- 8042 controller plumbing --------------------------------- */

/* Wait for the controller's input buffer (host -> device) to drain so
 * we can write another byte. Bounded so a dead controller can't hang
 * the kernel forever. */
static void wait_in_clear(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(KBD_STATUS) & KBD_STATUS_IN_FULL) == 0) return;
    }
}

/* Wait for output (device -> host) to be available. */
static bool wait_out_full(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(KBD_STATUS) & KBD_STATUS_OUT_FULL) return true;
    }
    return false;
}

/* Wait for an AUX (mouse) byte to land in the output buffer. The shared
 * 8042 design means a keyboard scancode could arrive first; in that case
 * we drain it and keep waiting. Bounded so a silent mouse can't hang the
 * kernel. Returns the byte on success, -1 on timeout. */
static int wait_aux_data(void) {
    for (int i = 0; i < 200000; i++) {
        uint8_t st = inb(KBD_STATUS);
        if ((st & KBD_STATUS_OUT_FULL) == 0) continue;
        uint8_t b = inb(KBD_DATA);
        if (st & KBD_STATUS_AUX_DATA) return b;
        /* keyboard byte snuck in -- swallow + keep waiting */
    }
    return -1;
}

static void ctl_write(uint8_t cmd) {
    wait_in_clear();
    outb(KBD_CMD, cmd);
}
static void ctl_write_data(uint8_t b) {
    wait_in_clear();
    outb(KBD_DATA, b);
}
static int ctl_read_data(void) {
    if (!wait_out_full()) return -1;
    return inb(KBD_DATA);
}

/* Send a single byte to the AUX device and read its ACK. Returns true
 * if the ACK byte (0xFA) actually arrived. Uses the AUX-aware reader so
 * a stray keyboard scancode can't be mistaken for the ACK. */
static bool aux_send(uint8_t b) {
    ctl_write(CMD_AUX_WRITE_NEXT);
    ctl_write_data(b);
    int r = wait_aux_data();
    return r == AUX_ACK;
}

/* ---- IRQ handler ---------------------------------------------- */

static void mouse_irq(struct regs *r) {
    (void)r;
    /* The shared 8042 output buffer can also hold keyboard bytes; if
     * AUX_DATA isn't set it's not for us. EOI and bail. */
    uint8_t status = inb(KBD_STATUS);
    if ((status & KBD_STATUS_OUT_FULL) == 0 ||
        (status & KBD_STATUS_AUX_DATA) == 0) {
        goto eoi;
    }
    uint8_t b = inb(KBD_DATA);

    /* Re-sync: byte 0's bit 3 must be set. If it isn't, drop the byte
     * and restart the packet so a stray noise byte doesn't poison the
     * stream forever. */
    if (g_pkt_pos == 0 && (b & 0x08) == 0) goto eoi;

    g_pkt[g_pkt_pos++] = b;
    if (g_pkt_pos < 3) goto eoi;

    g_pkt_pos = 0;
    uint8_t flags = g_pkt[0];

    /* Drop overflowed packets entirely -- we can't trust the deltas. */
    if (flags & 0xC0) goto eoi;

    /* Sign-extend 9-bit deltas. */
    int dx = (int)g_pkt[1] - ((flags << 4) & 0x100);
    int dy = (int)g_pkt[2] - ((flags << 3) & 0x100);
    /* Flip dy so screen-down is positive. */
    dy = -dy;

    mouse_inject_event(dx, dy, flags & 0x07);

eoi:
    irq_eoi_isa(12);
}

/* ---- public API ----------------------------------------------- */

void mouse_set_callback(mouse_event_fn cb) {
    g_cb = cb ? cb : noop_cb;
}

uint8_t mouse_buttons(void) {
    return g_buttons;
}

/* Shared input sink. PS/2 IRQ ends here after assembling its 3-byte
 * packet; USB-HID lands here after decoding boot-protocol mouse
 * reports. Updates the cached button state and forwards to whoever
 * the GUI registered with mouse_set_callback(). */
void mouse_inject_event(int dx, int dy, uint8_t buttons) {
    /* Count low-to-high transitions across all three buttons -- this
     * is what "click" means downstream; bare button-held updates do
     * not advance the counter. */
    uint8_t newly = (uint8_t)(buttons & ~g_buttons);
    bool clicked = false;
    if (newly & MOUSE_BTN_LEFT)   { g_btn_press_total++; clicked = true; }
    if (newly & MOUSE_BTN_RIGHT)  { g_btn_press_total++; clicked = true; }
    if (newly & MOUSE_BTN_MIDDLE) { g_btn_press_total++; clicked = true; }

    g_buttons      = buttons;
    g_last_buttons = buttons;
    g_last_dx      = (int8_t)((dx < -128) ? -128 : (dx > 127 ? 127 : dx));
    g_last_dy      = (int8_t)((dy < -128) ? -128 : (dy > 127 ? 127 : dy));
    g_events_total++;
    g_dx_abs_total += (uint64_t)(dx < 0 ? -dx : dx);
    g_dy_abs_total += (uint64_t)(dy < 0 ? -dy : dy);

    /* M26D: snapshot every 16th event OR immediately on any click.
     * The click-triggered line is what makes "I clicked something"
     * a reliable test signal -- without it the snapshot may not
     * land for another 16 frames of pure motion. */
    if ((g_events_total & 15ull) == 0ull || clicked) {
        kprintf("[input] mouse_inject events=%lu clicks=%lu btn=0x%02x "
                "last_dx=%d last_dy=%d sumdx=%lu sumdy=%lu\n",
                (unsigned long)g_events_total,
                (unsigned long)g_btn_press_total,
                (unsigned)g_last_buttons,
                (int)g_last_dx, (int)g_last_dy,
                (unsigned long)g_dx_abs_total,
                (unsigned long)g_dy_abs_total);
    }

    g_cb(dx, dy, buttons);
}

/* M26D telemetry accessors -- read-only views into the IRQ-side
 * counters. All are O(1) and safe from any context. */
uint64_t mouse_events_total(void)     { return g_events_total; }
uint64_t mouse_btn_press_total(void)  { return g_btn_press_total; }
uint64_t mouse_dx_abs_total(void)     { return g_dx_abs_total; }
uint64_t mouse_dy_abs_total(void)     { return g_dy_abs_total; }
uint8_t  mouse_last_buttons(void)     { return g_last_buttons; }
int8_t   mouse_last_dx(void)          { return g_last_dx; }
int8_t   mouse_last_dy(void)          { return g_last_dy; }

void mouse_init(void) {
    /* This whole sequence is timing-sensitive: every byte we send to
     * the 8042 produces a response on the SAME shared port (0x60) that
     * the keyboard uses, so any stray IRQ1 from a keystroke (the user
     * literally just typed `gui<enter>` to get here) can race in and
     * read our config / ACK byte before we do. The defensive trio is:
     *   - cli() for the duration: no IRQs at all.
     *   - DISABLE_KBD (0xAD): the controller stops emitting scancodes,
     *     so even buffered keystrokes won't keep flowing in.
     *   - drain the output buffer once both ports are quiet, so any
     *     pre-init garbage byte doesn't masquerade as our first ACK. */
    cli();

    ctl_write(CMD_DISABLE_KBD);
    ctl_write(CMD_DISABLE_AUX);

    /* Drain anything left in the output buffer from before init or
     * from the brief window between disabling and the controller
     * actually settling. */
    while (inb(KBD_STATUS) & KBD_STATUS_OUT_FULL) (void)inb(KBD_DATA);

    /* Enable AUX port + tweak the controller config byte (set bit 1
     * = IRQ12 enable, clear bit 5 = mouse-clock-disable). */
    ctl_write(CMD_ENABLE_AUX);

    ctl_write(CMD_READ_CONFIG);
    int cfg = ctl_read_data();
    if (cfg < 0) {
        kprintf("[mouse] WARN: couldn't read controller config -- "
                "PS/2 mouse may not work\n");
        ctl_write(CMD_ENABLE_KBD);
        sti();
        return;
    }
    cfg = (cfg | CFG_IRQ12_ENABLE) & ~CFG_MOUSE_DISABLE;
    ctl_write(CMD_WRITE_CONFIG);
    ctl_write_data((uint8_t)cfg);

    /* Step: defaults + enable streaming. We don't fail hard on missing
     * ACKs because some firmware oddities still let the packets flow. */
    if (!aux_send(AUX_SET_DEFAULTS)) {
        kprintf("[mouse] WARN: AUX 0xF6 (set defaults) NACK'd\n");
    }
    if (!aux_send(AUX_ENABLE_REPORT)) {
        kprintf("[mouse] WARN: AUX 0xF4 (enable reporting) NACK'd\n");
    }

    /* Drain anything the AUX device queued while we were configuring. */
    while (inb(KBD_STATUS) & KBD_STATUS_OUT_FULL) (void)inb(KBD_DATA);

    /* Re-enable the keyboard port now that mouse setup is done. */
    ctl_write(CMD_ENABLE_KBD);

    /* Facade routes IRQ12 via PIC or IO APIC. PIC mode requires the
     * IRQ2 cascade to be open too (slave PIC's INT line goes through
     * the master's IRQ2 pin); IO APIC mode just routes GSI 12 to the
     * BSP and the cascade is irrelevant. */
    irq_install_isa(12, mouse_irq);
    if (!irq_using_ioapic()) {
        pic_unmask(2);
    }

    sti();

    kprintf("[mouse] PS/2 driver up (IRQ12 routed via %s)\n",
            irq_using_ioapic() ? "IO APIC" : "PIC");
}
