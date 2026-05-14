/* mouse.h -- PS/2 mouse driver. */

#ifndef TOBYOS_MOUSE_H
#define TOBYOS_MOUSE_H

#include <tobyos/types.h>

#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04

typedef void (*mouse_event_fn)(int dx, int dy, uint8_t buttons);

void mouse_init(void);
void mouse_set_callback(mouse_event_fn cb);
uint8_t mouse_buttons(void);

/* Shared input sink used by PS/2 and USB-HID. */
void mouse_inject_event(int dx, int dy, uint8_t buttons);

/* PS/2 byte-level entry point.
 * Exported so the keyboard IRQ can drain shared 8042 output bytes and
 * route AUX bytes correctly instead of leaving them stuck. */
void mouse_ps2_handle_byte(uint8_t b);

/* M26D telemetry accessors. */
uint64_t mouse_events_total(void);
uint64_t mouse_btn_press_total(void);
uint64_t mouse_dx_abs_total(void);
uint64_t mouse_dy_abs_total(void);
uint8_t  mouse_last_buttons(void);
int8_t   mouse_last_dx(void);
int8_t   mouse_last_dy(void);

#endif /* TOBYOS_MOUSE_H */