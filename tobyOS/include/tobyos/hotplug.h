/* hotplug.h -- M26C hot-plug event ring for tobyOS.
 *
 * Kernel-side producers:
 *   - xhci.c (root-port connect/disconnect via Port Status Change TRB)
 *   - usb_hub.c (downstream-port C_PORT_CONNECTION via polling)
 *   - usb_hid.c / usb_msc.c (driver detach hooks)
 *
 * Single consumer:
 *   - syscall ABI_SYS_HOT_DRAIN, plumbed through libtoby/devtest.c.
 *
 * The ring is fixed-size (ABI_DEVT_HOT_RING). Producers update head_
 * monotonically; on overflow we increment a `dropped` counter and the
 * oldest unread entry is overwritten silently. The drain syscall
 * stamps that counter onto the first event it returns so userland can
 * reason about lost events without separate plumbing.
 *
 * Concurrency: producers may run in soft-irq context (xhci_poll) or
 * normal kernel thread (usb_hub_poll). Consumer is the syscall path.
 * We protect with an irq-off spinlock; events are tiny (64 bytes) so
 * the critical section is short. */

#ifndef TOBYOS_HOTPLUG_H
#define TOBYOS_HOTPLUG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <tobyos/abi/abi.h>   /* abi_hot_event, ABI_HOT_*, ABI_DEVT_HOT_RING */

/* Kernel-side post: copy `*ev` into the ring. seq + time_ms get
 * filled in for the caller -- callers only need to populate bus,
 * action, hub_depth, hub_port, slot, info. */
void  hotplug_post(const struct abi_hot_event *ev);

/* Convenience wrappers used by drivers; just shorthand for filling in
 * the struct + calling hotplug_post(). info[] may be NULL (means ""). */
void  hotplug_post_attach(uint8_t bus, uint16_t slot,
                          uint8_t hub_depth, uint8_t hub_port,
                          const char *info);
void  hotplug_post_detach(uint8_t bus, uint16_t slot,
                          uint8_t hub_depth, uint8_t hub_port,
                          const char *info);

/* Drain at most `cap` events into `out`. Returns count written.
 * Safe to call with cap == 0 (returns 0; doesn't touch the ring). */
int   hotplug_drain(struct abi_hot_event *out, int cap);

/* Stats for the kernel-side selftest. */
size_t hotplug_total_posted(void);
size_t hotplug_total_drained(void);
size_t hotplug_total_dropped(void);

/* devtest harness self-test ("hotplug"): synthesize one ATTACH +
 * one DETACH event, drain them, verify the round-trip. */
int   hotplug_selftest(char *msg, size_t cap);

#endif /* TOBYOS_HOTPLUG_H */
