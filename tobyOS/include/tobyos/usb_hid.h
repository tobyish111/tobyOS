/* usb_hid.h -- USB HID Boot Protocol class driver.
 *
 * Handles exactly two device kinds:
 *   - Boot keyboard  (bInterfaceClass=3 SubClass=1 Protocol=1)
 *   - Boot mouse     (bInterfaceClass=3 SubClass=1 Protocol=2)
 *
 * Boot Protocol gives us a guaranteed-fixed report layout (no
 * report-descriptor parser needed):
 *
 *   keyboard report (8 bytes):
 *     [0]  modifiers bitmap (Lctrl=0x01, Lshift=0x02, ... Rgui=0x80)
 *     [1]  reserved
 *     [2..7] up to 6 USB key usage codes; 0 = no key
 *
 *   mouse report (3+ bytes):
 *     [0]  buttons bitmap (left=0x01, right=0x02, middle=0x04)
 *     [1]  signed dx
 *     [2]  signed dy
 *     [3]  signed dz / wheel (optional)
 *
 * Translated reports are forwarded into the existing PS/2 dispatch
 * sinks (kbd_dispatch_char + mouse_inject_event) so the GUI sees one
 * unified input stream regardless of source. PS/2 keeps working in
 * parallel; both devices coexist.
 *
 * Maximum-supported-devices is intentionally tiny -- xHCI on a real
 * machine usually exposes 4-16 root-hub ports, but only one keyboard
 * + one mouse will ever be plugged in directly to the root hub on a
 * laptop. Hubs are not supported in this milestone, so 4 is plenty.
 */

#ifndef TOBYOS_USB_HID_H
#define TOBYOS_USB_HID_H

#include <tobyos/types.h>
#include <tobyos/usb.h>

#define USB_HID_MAX_DEVICES   4

/* Try to claim an interrupt-IN endpoint for HID Boot Protocol.
 * Returns true on success, in which case:
 *   - the device's HID state has been allocated and dev->hid_state set
 *   - SET_PROTOCOL(BOOT) and SET_IDLE(0) class requests have been issued
 *   - dev->int_complete points at this driver's report handler
 *   - the caller (xHCI) is responsible for submitting the FIRST
 *     interrupt-IN transfer; subsequent re-arming happens automatically
 *     from inside the report callback.
 *
 * Returns false (and does NOT touch the device) for any condition the
 * driver cannot handle: non-Boot subclass, non-keyboard/mouse protocol,
 * non-interrupt endpoint, etc. The caller may try other class drivers
 * (none today, but future xhci-class-driver registry could chain). */
bool usb_hid_probe(struct usb_device *dev,
                   const struct usb_iface_desc *iface,
                   const struct usb_endpoint_desc *ep);

/* M26C: drop any HID state owned by `dev`. Called by the HCI when a
 * slot is being torn down (cable yanked, port reset failed, etc.).
 * Idempotent -- safe to call when no state was ever attached. */
void usb_hid_unbind(struct usb_device *dev);

/* M26D introspection. count() returns the number of in-use HID
 * slots; introspect_at() fills out one ABI_DEVT_BUS_INPUT record per
 * slot in iteration order [0..count()). Returns 1 on success, 0 if
 * idx is out of range. */
struct abi_dev_info;
int      usb_hid_count(void);
int      usb_hid_kbd_count(void);
int      usb_hid_mouse_count(void);
uint64_t usb_hid_total_frames(void);
int      usb_hid_introspect_at(int idx, struct abi_dev_info *out);

/* devtest selftest: PASS if every in-use HID slot is well-formed,
 * SKIP if the HID pool is empty, FAIL on inconsistency. */
int      usb_hid_selftest(char *msg, size_t cap);

#endif /* TOBYOS_USB_HID_H */
