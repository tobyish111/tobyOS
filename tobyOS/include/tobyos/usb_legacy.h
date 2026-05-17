/* usb_legacy.h -- legacy USB host-controller discovery.
 *
 * UHCI/OHCI/EHCI are common on older PCs. The full transfer schedulers
 * for those HCIs are separate projects, but binding them here gives the
 * kernel accurate diagnostics and, importantly, avoids disturbing BIOS
 * USB-legacy keyboard/mouse emulation until a real legacy-HCI scheduler
 * is present.
 */

#ifndef TOBYOS_USB_LEGACY_H
#define TOBYOS_USB_LEGACY_H

#include <tobyos/types.h>

void usb_legacy_register(void);
void usb_legacy_poll(void);

int usb_legacy_count(void);
int usb_legacy_uhci_count(void);
int usb_legacy_ohci_count(void);
int usb_legacy_ehci_count(void);

int usb_legacy_selftest(char *msg, size_t cap);

#endif /* TOBYOS_USB_LEGACY_H */
