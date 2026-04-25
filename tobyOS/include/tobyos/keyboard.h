/* keyboard.h -- PS/2 keyboard driver (IRQ1, scancode set 1).
 *
 * After kbd_init():
 *   - the IRQ1 handler is registered and IRQ1 is unmasked
 *   - keystrokes accumulate in a ring buffer
 *   - kbd_trygetc() returns the next ASCII char or -1 if empty
 *   - kbd_getc()    blocks (sti+hlt) until a char is available
 *
 * Limitations of this initial cut:
 *   - US layout only
 *   - extended (E0-prefixed) keys are silently dropped (no arrows yet)
 *   - no key-up events are surfaced (other than for shift tracking)
 *   - no autorepeat reprogramming -- we just consume what the controller
 *     gives us
 */

#ifndef TOBYOS_KEYBOARD_H
#define TOBYOS_KEYBOARD_H

#include <tobyos/types.h>

void kbd_init(void);

/* Returns the next ASCII char (0..255) or -1 if the buffer is empty. */
int  kbd_trygetc(void);

/* Blocks until a char is available, then returns it. Requires
 * interrupts to be enabled. */
int  kbd_getc(void);

/* ---- shared input sink (milestone 21: USB-HID re-uses this) ----
 *
 * Push a single resolved ASCII char into wherever it needs to go --
 * that's the GUI focused window when the desktop is up, the text-mode
 * shell ring otherwise, or a SIGINT to the foreground process for
 * Ctrl+C. The PS/2 IRQ calls this after its scancode-to-ASCII map +
 * Caps/Ctrl resolution. USB-HID does the same after decoding boot-
 * protocol reports + applying its own modifier state. Anything that
 * lands here looks identical to downstream consumers.
 *
 * Safe to call from IRQ context AND from the kernel idle loop. */
void kbd_dispatch_char(char c);

/* ---- M26D telemetry accessors --------------------------------- *
 *
 * Read-only views over PS/2 driver state. Counters saturate (no
 * rollover guard); modifier queries return the live latch. Used by
 * the input self-test + by usbtest hid for diagnostics. */
uint64_t kbd_chars_dispatched(void);
uint64_t kbd_irqs_total(void);
bool     kbd_caps_state(void);
bool     kbd_shift_state(void);
bool     kbd_ctrl_state(void);
uint8_t  kbd_last_scancode(void);

#endif /* TOBYOS_KEYBOARD_H */
