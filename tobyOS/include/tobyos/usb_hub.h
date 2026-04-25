/* usb_hub.h -- USB Hub class driver (M26B).
 *
 * The HCI driver (xhci.c) calls usb_hub_probe() right after a device
 * has been Address-Device'd and its standard device descriptor has
 * been fetched, when bDeviceClass == USB_CLASS_HUB. usb_hub.c then
 * pulls the class-specific hub descriptor, flips the Slot Context
 * Hub bit via xhci_configure_as_hub(), powers each downstream port,
 * and re-uses the HCI's enumeration entrypoint to bring up devices
 * that are connected behind the hub.
 *
 * Scope (M26B):
 *   - HS hubs only (bcdUSB == 0x0200). FS / LS hubs and hub-attached
 *     LS/FS devices behind a HS hub need TT (Transaction Translator)
 *     handling; we report them in devlist but do not address them.
 *   - At most USB_HUB_MAX_DEPTH levels of nesting.
 *   - At most USB_HUB_MAX_PORTS downstream ports per hub.
 *   - No hot-plug yet (M26C).
 *
 * The hub's own "introspection" snapshot is exposed through the
 * existing devtest enumerate path: usb_hub_introspect_count() /
 * usb_hub_introspect_at() return ABI_DEVT_BUS_HUB records, while the
 * downstream devices keep showing up under ABI_DEVT_BUS_USB with their
 * hub_depth + hub_port fields filled in.
 */

#ifndef TOBYOS_USB_HUB_H
#define TOBYOS_USB_HUB_H

#include <tobyos/types.h>
#include <tobyos/usb.h>
#include <tobyos/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probe a freshly addressed hub-class device. dev->slot_id and
 * dev->bus_address must already be valid. Returns true if the device
 * is a hub we successfully configured (downstream ports may or may
 * not have anything connected). */
bool usb_hub_probe(struct usb_device *dev);

/* Number of hubs currently registered (for devtest / devlist). */
size_t usb_hub_count(void);

/* Fill `out` with the i-th hub record (0 <= i < count). Returns
 * false if i is out of range. The record uses bus = ABI_DEVT_BUS_HUB
 * and reports nports + depth in the extra string. */
bool usb_hub_introspect_at(size_t i, struct abi_dev_info *out);

/* devtest selftest entrypoint. Returns ABI_DEVT_PASS if at least one
 * hub is present and all its ports were enumerated without error,
 * ABI_DEVT_SKIP if no hub is present, ABI_DEVT_FAIL otherwise. */
int usb_hub_selftest(char *msg, size_t msg_cap);

/* M26C: poll every registered hub for downstream-port C_PORT_CONNECTION
 * change bits. On a rising edge (cable inserted) re-runs the same path
 * usb_hub_probe() uses for boot-time enumeration. On a falling edge
 * (cable yanked) calls into the HCI to free the affected slot. Posts
 * ABI_HOT_ATTACH / ABI_HOT_DETACH events to the hot-plug ring.
 *
 * Designed to be called from xhci_poll() once per timer tick (~100 Hz).
 * It's safe to call when no hubs are registered (returns 0). */
int usb_hub_poll(void);

/* Driver-side handler invoked by the HCI when a downstream port
 * connection-state change has already been observed (e.g. via PSCE
 * propagation from the hub's status endpoint). Same semantics as
 * usb_hub_poll, but scoped to a single (hub, port). Returns 0 on
 * success, -ABI_E* on failure. */
int usb_hub_handle_port_change(struct usb_device *hub_dev, uint8_t port);

#ifdef __cplusplus
}
#endif

#endif /* TOBYOS_USB_HUB_H */
