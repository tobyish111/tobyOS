/* keyboard.h -- PS/2 keyboard driver (IRQ1, scancode set 1). */

#ifndef TOBYOS_KEYBOARD_H
#define TOBYOS_KEYBOARD_H

#include <tobyos/types.h>

void kbd_init(void);

/* Returns the next ASCII char (0..255) or -1 if the buffer is empty. */
int  kbd_trygetc(void);

/* Non-destructive: true if the input ring has at least one byte. */
bool kbd_haschar(void);

/* Blocks until a char is available. Requires interrupts enabled. */
int  kbd_getc(void);

/* Shared input sink used by PS/2 and USB-HID. */
void kbd_dispatch_char(char c);
void kbd_flush_pending(void);

/* Push a byte directly into the console input ring, bypassing the GUI key
 * dispatch + ISIG handling. For programmatic/test input (the B21 TTY
 * harness). */
void kbd_inject_raw(char c);

/* PS/2 byte-level entry point.
 * Exported so the mouse IRQ can drain shared 8042 output bytes and
 * route keyboard bytes correctly instead of leaving them stuck. */
void kbd_ps2_handle_scancode(uint8_t sc);

/* M26D telemetry accessors. */
uint64_t kbd_chars_dispatched(void);
uint64_t kbd_irqs_total(void);
bool     kbd_caps_state(void);
bool     kbd_shift_state(void);
bool     kbd_ctrl_state(void);
bool     kbd_alt_state(void);
uint8_t  kbd_last_scancode(void);

#endif /* TOBYOS_KEYBOARD_H */
