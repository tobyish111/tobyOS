/* evdev.h -- Linux input event device (/dev/input/event0) feed API.
 *
 * The PS/2 keyboard driver calls evdev_feed_key() on every make/break so a
 * Linux app that opened /dev/input/event0 receives the standard
 * struct input_event stream (EV_KEY + EV_SYN). The boot proof harness uses
 * evdev_reset() + evdev_feed_key() to inject a deterministic key sequence.
 */

#ifndef TOBYOS_EVDEV_H
#define TOBYOS_EVDEV_H

#include <tobyos/types.h>

/* Push one key make/break into the keyboard evdev queue (emits EV_KEY then a
 * SYN_REPORT, exactly like the kernel evdev layer). `code` is the Linux
 * keycode (KEY_*); `value` is 1=press, 0=release, 2=autorepeat. */
void evdev_feed_key(uint8_t code, int value);

/* Push one mouse report into the mouse evdev queue: EV_REL motion + BTN_*
 * edges + a SYN frame. `buttons`/`prev` are the PS/2 button bitmasks
 * (1=left,2=right,4=middle) now and before this report. */
void evdev_feed_mouse(int dx, int dy, int dz, uint8_t buttons, uint8_t prev);

/* Drop any queued events on all devices (used by proof harnesses before
 * injecting a deterministic sequence). */
void evdev_reset(void);

#endif /* TOBYOS_EVDEV_H */
