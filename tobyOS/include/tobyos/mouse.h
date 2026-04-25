/* mouse.h -- PS/2 mouse driver.
 *
 * Talks to the same 8042 controller as the keyboard, but on the AUX
 * port (IRQ12). We accept the device's standard 3-byte streaming
 * packets, decode the deltas + button bits, and hand each packet to a
 * single registered callback (set by the GUI layer).
 *
 * Cursor position lives in the GUI layer -- this file only reports
 * deltas. Buttons are reported as a bitmask:
 *
 *   bit 0 -- left
 *   bit 1 -- right
 *   bit 2 -- middle
 */

#ifndef TOBYOS_MOUSE_H
#define TOBYOS_MOUSE_H

#include <tobyos/types.h>

#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04

/* Per-packet event from IRQ12. dx is positive = right, dy positive = up
 * in PS/2 land; mouse.c flips dy so positive = down (screen-relative). */
typedef void (*mouse_event_fn)(int dx, int dy, uint8_t buttons);

/* Initialise the AUX port + unmask IRQ12 (and IRQ2 cascade). Drains
 * any pending controller bytes before installing the handler. Safe to
 * call once at boot even if no GUI app ever runs -- the events just go
 * to a no-op default callback. */
void mouse_init(void);

/* Install a callback fired in IRQ context after each complete 3-byte
 * packet. Pass NULL to detach. The caller is responsible for keeping
 * the IRQ-side work small (we just bump some counters and queue a
 * compositor redraw). */
void mouse_set_callback(mouse_event_fn cb);

/* Snapshot of the most recent button state -- useful for the compositor
 * when deciding whether the user is mid-drag while a separate event
 * came in. */
uint8_t mouse_buttons(void);

/* Shared input sink (milestone 21: USB-HID re-uses this). Calls the
 * registered mouse_event_fn with the deltas + button mask, and
 * updates mouse_buttons() at the same time so any consumer that
 * polls button state sees a consistent value. Safe from IRQ context
 * AND from the kernel idle loop. */
void mouse_inject_event(int dx, int dy, uint8_t buttons);

/* M26D telemetry accessors. Saturating counters; reads are atomic
 * for consumer purposes (we never gate on exact equality). */
uint64_t mouse_events_total(void);
uint64_t mouse_btn_press_total(void);
uint64_t mouse_dx_abs_total(void);
uint64_t mouse_dy_abs_total(void);
uint8_t  mouse_last_buttons(void);
int8_t   mouse_last_dx(void);
int8_t   mouse_last_dy(void);

#endif /* TOBYOS_MOUSE_H */
