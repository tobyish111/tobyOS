/* usbreg.h -- USB device attach registry (Milestone 35C).
 *
 * Hangs off the existing xHCI / hub probe path. Every time a USB
 * device finishes Address-Device + Get-Descriptor we record one
 * struct usbreg_entry here, regardless of whether one of the in-tree
 * class drivers (HID, MSC, Hub) was able to bind. Detach (cable yank
 * for USB 2.0, port-disconnect interrupt for USB 3.x) clears the
 * matching entry.
 *
 * The registry serves three goals:
 *
 *   1. devlist / hwinfo / hwcompat have a single place to ask "what
 *      USB devices are currently attached, and which one bound a
 *      driver", instead of each surface having to walk xhci slots
 *      independently.
 *
 *   2. Unsupported class codes are visible: the entry is created with
 *      driver=NULL and status=USBREG_STATUS_UNSUPPORTED, so devlist
 *      can render "USB Printer (07/01/02): unsupported" instead of
 *      silently dropping the device.
 *
 *   3. Defensive isolation: if a future class driver crashes during
 *      probe, the entry is still here with status=PROBE_FAILED; the
 *      OS keeps booting, the bad device is named, and a manual
 *      `usbtest` can target it without stack-walking through xhci.
 *
 * Storage is fixed-size (USBREG_MAX entries). Overflow drops the new
 * attach with a single SLOG_WARN -- existing entries are preserved.
 *
 * Concurrency: every public entry point disables interrupts internally
 * (IRQ-safe), so xhci's IRQ thread + the shell + the m35c selftest can
 * all touch the registry without coordination.
 */

#ifndef TOBYOS_USBREG_H
#define TOBYOS_USBREG_H

#include <tobyos/types.h>

#define USBREG_MAX            16    /* practical cap for VM + 1-2 hubs  */
#define USBREG_DRIVER_MAX     16
#define USBREG_FRIENDLY_MAX   48

/* status: lifecycle + binding outcome. ATTACH events flow through
 * STATUS_NEW -> (BOUND | UNSUPPORTED | PROBE_FAILED). Detach goes to
 * STATUS_GONE first (so debug log can still print the friendly name)
 * and is reaped on the next attach if the slot is reused. */
enum usbreg_status {
    USBREG_STATUS_FREE         = 0, /* slot unused                       */
    USBREG_STATUS_NEW          = 1, /* present but classify not done yet */
    USBREG_STATUS_BOUND        = 2, /* a class driver claimed it         */
    USBREG_STATUS_UNSUPPORTED  = 3, /* class is known but no driver      */
    USBREG_STATUS_UNKNOWN      = 4, /* class is not in drvdb either      */
    USBREG_STATUS_PROBE_FAILED = 5, /* driver matched but probe failed   */
    USBREG_STATUS_GONE         = 6, /* device was detached recently      */
};

struct usbreg_entry {
    enum usbreg_status status;
    uint8_t  slot_id;
    uint8_t  port_id;          /* 1-based root-hub port (0 if behind hub) */
    uint8_t  hub_depth;        /* 0 = root hub, 1 = behind one hub, ...   */
    uint8_t  speed;            /* USB-IF speed code 1..4                  */
    uint16_t vendor;
    uint16_t product;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    char     driver[USBREG_DRIVER_MAX];      /* "(none)" if unsupported */
    char     friendly[USBREG_FRIENDLY_MAX];  /* drvdb-derived name      */
};

void usbreg_init(void);

/* Called by the HCI driver from the classify path. `driver` is the
 * name of the class driver that successfully claimed the device, or
 * NULL if the device was unclaimed (which then routes the entry to
 * UNSUPPORTED or UNKNOWN based on whether drvdb has the class). */
void usbreg_record_attach(uint8_t slot_id,
                          uint8_t port_id,
                          uint8_t hub_depth,
                          uint8_t speed,
                          uint16_t vendor,
                          uint16_t product,
                          uint8_t dev_class,
                          uint8_t dev_subclass,
                          uint8_t dev_protocol,
                          const char *driver);

/* Called by xhci when a port goes Disconnected (or a hub reports a
 * downstream port-status change to disconnected). Marks the entry
 * STATUS_GONE; it stays visible in devlist until the slot is reused. */
void usbreg_record_detach(uint8_t slot_id);

/* Called by the HCI when probe is dispatched but the driver returns
 * an error. This converts STATUS_NEW/BOUND -> PROBE_FAILED so we can
 * surface "driver attempted but failed" distinct from "unsupported". */
void usbreg_record_probe_failed(uint8_t slot_id, const char *driver);

/* Iteration / introspection. */
size_t usbreg_count(void);                          /* incl. GONE rows */
size_t usbreg_count_active(void);                   /* excl. GONE rows */
const struct usbreg_entry *usbreg_get(size_t idx);  /* NULL on OOB     */

/* Lookup by slot id; NULL if not present. */
const struct usbreg_entry *usbreg_find(uint8_t slot_id);

/* Helper: human-readable status string ("bound", "unsupported", ...). */
const char *usbreg_status_name(enum usbreg_status st);

/* Diagnostic: dump all rows to kprintf. Used by hwinfo, devlist. */
void usbreg_dump_kprintf(void);

#endif /* TOBYOS_USBREG_H */
