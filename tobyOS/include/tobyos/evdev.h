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

/* Push one key make/break into the evdev queue (emits EV_KEY then a
 * SYN_REPORT, exactly like the kernel evdev layer). `code` is the Linux
 * keycode (KEY_*); `value` is 1=press, 0=release, 2=autorepeat. */
void evdev_feed_key(uint8_t code, int value);

/* Drop any queued events (used by the proof harness before injecting). */
void evdev_reset(void);

#endif /* TOBYOS_EVDEV_H */
