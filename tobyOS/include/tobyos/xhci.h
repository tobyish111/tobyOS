/* xhci.h -- USB 3.x xHCI host controller driver, polled, no IRQs.
 *
 * Bound through the milestone-21 PCI driver registry. Discovers
 * devices on every root-hub port at probe time, runs the Boot HID
 * class driver against any attached keyboard or mouse, and exposes
 * a single drain function that the kernel idle loop calls between
 * net_poll() and gui_tick() so HID reports flow with the same
 * latency PS/2 IRQs would.
 *
 * QEMU emulates this whole stack via:
 *
 *   -device qemu-xhci -device usb-kbd -device usb-mouse
 *
 * which the make run-xhci target wires up. PS/2 stays available
 * (the make run target keeps using QEMU's i440fx-default PS/2
 * keyboard + mouse), so xHCI is purely additive.
 */

#ifndef TOBYOS_XHCI_H
#define TOBYOS_XHCI_H

#include <tobyos/types.h>

/* Register the xHCI PCI driver so pci_bind_drivers() can find it. */
void xhci_register(void);

/* Drain the primary event ring. Called from the kernel idle loop;
 * cheap when nothing is pending. Dispatches Transfer Events to the
 * waiting HID device's int_complete callback and re-arms the
 * interrupt-IN endpoint for the next report. Safe at any time after
 * pci_bind_drivers(); a no-op if no xHCI controller was found. */
void xhci_poll(void);

/* ============================================================
 * M26A introspection -- read-only accessors for the devtest
 * harness.
 * ============================================================
 *
 * xhci.c keeps its private struct xhci_dev_state[] table indexed by
 * xHCI slot id; the harness needs a layout-stable, size-bounded
 * snapshot so it can be copied across the syscall boundary in an
 * abi_dev_info. These three helpers expose just that:
 *
 *   xhci_present()         -- true once probe() has bound a controller
 *   xhci_introspect_count() -- number of populated USB slots
 *   xhci_introspect_at()   -- fill a flat record for slot index `idx`
 *
 * "Slot index" here is a dense iteration index over in_use entries
 * (NOT the raw xHCI slot id), so callers can `for (i=0; i<count; i++)`
 * without seeing holes. */
#include <stdint.h>     /* for uint8_t / uint16_t in the record below */
struct abi_dev_info;     /* forward; lives in <tobyos/abi/abi.h>      */
bool   xhci_present(void);
int    xhci_introspect_count(void);
int    xhci_introspect_at(int idx, struct abi_dev_info *out);

/* Light "live" stats used by the xhci self-test. Returns 0 if no
 * controller is bound. irq_count is the cumulative number of MSI/MSI-X
 * interrupts the controller has fired since boot. */
int    xhci_irq_count(void);

/* Self-test for the controller. Used by devtest_run("xhci", ...).
 * Returns 0 when a controller is bound and looks healthy, ABI_DEVT_SKIP
 * if no controller is present. */
int    xhci_selftest(char *msg, size_t cap);

/* Self-test for the USB device list (HID + MSC bindings). Returns
 * 0 if at least one device is enumerated and bound, ABI_DEVT_SKIP if
 * the controller is present but no devices, and -ABI_E* on hard fails
 * (e.g. malformed slot table). */
int    xhci_devices_selftest(char *msg, size_t cap);

/* ============================================================
 * M26B hooks consumed by usb_hub.c
 * ============================================================
 *
 * Forward declarations only -- the implementations live in xhci.c
 * (so the hub class driver doesn't have to know about Slot Context
 * layout, route strings, or input contexts).
 */
struct usb_device;

/* Re-issue Configure Endpoint with the Slot Context Hub bit set and
 * NumberOfPorts populated. Called by usb_hub_probe() right after the
 * hub descriptor has been fetched; on success xHC will start routing
 * Setup TDs through this hub's downstream ports correctly. */
bool xhci_configure_as_hub(struct usb_device *dev,
                           uint8_t nports, uint8_t ttt);

/* Allocate + Address + finalize a downstream device on `parent`'s
 * `hub_port` (1-based) at `speed` (USB-IF speed code). Returns the
 * newly populated usb_device on success, NULL on any step failure
 * (with the slot fully torn down). */
struct usb_device *xhci_attach_via_hub(struct usb_device *parent,
                                       uint8_t hub_port,
                                       uint8_t speed);

/* ============================================================
 * M26C hooks consumed by usb_hub.c (and root-hub poll path)
 * ============================================================ */

/* Tear down the slot identified by `slot_id`. Issues Disable Slot
 * if the controller is still alive, frees the input/device contexts,
 * clears the DCBAA entry, and walks the class drivers to drop any
 * references they were holding. Posts an ABI_HOT_DETACH event on
 * success. Safe to call on an already-disabled slot (returns false
 * silently). */
bool xhci_detach_slot(uint8_t slot_id);

/* Look up the slot id of the device currently connected to root port
 * `port_id` (1-based). Returns 0 if the port is empty / no device.
 * Used by xhci_poll() to map a root-port disconnect TRB back to a
 * slot for teardown. */
uint8_t xhci_slot_for_root_port(uint8_t port_id);

/* Look up the slot id of the device hanging off (parent_slot, hub_port).
 * Returns 0 if no such child exists. Used by usb_hub_poll() to handle
 * downstream disconnect events. */
uint8_t xhci_slot_for_hub_port(uint8_t parent_slot_id, uint8_t hub_port);

/* Re-attach a device on a root port that has just gone CCS=1.
 * Same path as enumerate_port() but without iterating every port;
 * used by the root-port hot-plug handler. Returns slot_id on success,
 * 0 on failure or empty port. */
uint8_t xhci_attach_root_port(uint8_t port_id);

/* Iterate through every populated root-hub port number that has a
 * connected device and return the corresponding 1-based port id one
 * at a time. Returns 0 once exhausted. Cursor `*ix` is advanced
 * across calls; pass *ix = 0 to start. */
uint8_t xhci_iter_root_port_with_device(int *ix);

/* M26C: kernel-idle service entry point. xhci_poll() RW1C-clears
 * Port Status Change events and sets a per-port bit in
 * g_xhci.port_change_pending; this function drains that bitmap
 * and runs xhci_attach_root_port() / xhci_detach_slot() in normal
 * (non-IRQ) context. Cheap when nothing is pending; safe to call
 * even if xHCI was never bound. */
void xhci_service_port_changes(void);

#endif /* TOBYOS_XHCI_H */
