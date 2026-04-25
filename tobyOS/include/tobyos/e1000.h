/* e1000.h -- driver for the Intel 82540EM gigabit NIC (QEMU's default).
 *
 * Polled, no IRQs. Milestone-21 update: this driver no longer exposes
 * its own tx/rx_drain symbols. It registers a struct pci_driver via
 * e1000_register(), and on successful probe it publishes itself as a
 * struct net_dev (see <tobyos/net.h>). The eth/arp/ip/udp stack uses
 * net_default()->tx and ->rx_drain so adding e1000e / RTL8169 /
 * virtio-net later is purely additive.
 *
 * Why we still ship e1000 specifically:
 *   - QEMU exposes it by default with `-device e1000`.
 *   - Datasheet (Intel 82540EM) is public and well-known.
 *   - It's plain MMIO + descriptor rings, no virtio plumbing required.
 */

#ifndef TOBYOS_E1000_H
#define TOBYOS_E1000_H

#include <tobyos/types.h>
#include <tobyos/net.h>

#define E1000_VENDOR     0x8086
#define E1000_DEVICE     0x100E   /* QEMU's default; 82540EM */

/* Insert this driver into the PCI registry. The actual NIC probe runs
 * later, during pci_bind_drivers(). */
void e1000_register(void);

#endif /* TOBYOS_E1000_H */
