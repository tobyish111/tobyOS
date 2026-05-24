/* driver_autoload.c -- automatic driver selection and init for PnP.
 *
 * When a new PCI or USB device is detected, this module consults the
 * drvconf force rules and the PCI class-code-to-driver table to
 * automatically select and call the appropriate driver init function.
 * It tracks which driver serves which device and logs the result.
 */

#include <tobyos/driver_autoload.h>
#include <tobyos/drvconf.h>
#include <tobyos/drvmatch.h>
#include <tobyos/pnp.h>
#include <tobyos/pci.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>

#define TAG "autoload"

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static struct autoload_entry g_entries[AUTOLOAD_MAX_ENTRIES];
static int g_count;

struct class_driver_map {
    uint8_t  class_code;
    uint8_t  subclass;
    const char *driver;
};

static const struct class_driver_map g_class_map[] = {
    { 0x01, 0x01, "ide"      },
    { 0x01, 0x06, "ahci"     },
    { 0x01, 0x08, "nvme"     },
    { 0x02, 0x00, "e1000"    },
    { 0x03, 0x00, "vga"      },
    { 0x04, 0x03, "hda"      },
    { 0x0C, 0x03, "xhci"     },
};

#define CLASS_MAP_COUNT ((int)(sizeof(g_class_map)/sizeof(g_class_map[0])))

static const char *lookup_class_driver(uint8_t cc, uint8_t sc) {
    for (int i = 0; i < CLASS_MAP_COUNT; i++) {
        if (g_class_map[i].class_code == cc && g_class_map[i].subclass == sc)
            return g_class_map[i].driver;
    }
    return NULL;
}

void driver_autoload_init(void) {
    memset(g_entries, 0, sizeof(g_entries));
    g_count = 0;
    SLOG_INFO(TAG, "driver autoload subsystem initialised");
}

int driver_autoload_on_pci_detect(uint16_t vendor, uint16_t device,
                                  uint8_t class_code, uint8_t subclass) {
    if (g_count >= AUTOLOAD_MAX_ENTRIES) return -1;

    struct autoload_entry *e = &g_entries[g_count];
    e->vendor   = vendor;
    e->device   = device;
    e->bus_type = 0; /* PCI */

    const char *forced = drvconf_force_driver(vendor, device);
    const char *drv = forced ? forced : lookup_class_driver(class_code, subclass);

    if (!drv) {
        copy_str(e->driver_name, AUTOLOAD_NAME_MAX, "(none)");
        e->status = AUTOLOAD_UNBOUND;
        SLOG_WARN(TAG, "PCI %04x:%04x class %02x:%02x -- no driver match",
                  vendor, device, class_code, subclass);
        g_count++;
        return -1;
    }

    if (drvconf_is_blacklisted(drv)) {
        copy_str(e->driver_name, AUTOLOAD_NAME_MAX, drv);
        e->status = AUTOLOAD_DISABLED;
        SLOG_WARN(TAG, "PCI %04x:%04x driver '%s' blacklisted", vendor, device, drv);
        g_count++;
        return -1;
    }

    copy_str(e->driver_name, AUTOLOAD_NAME_MAX, drv);
    e->status = AUTOLOAD_BOUND;

    pnp_handle_hotplug(BUS_PCI, vendor, device, 1);

    SLOG_INFO(TAG, "PCI %04x:%04x -> driver '%s' (auto-loaded)",
              vendor, device, drv);
    g_count++;
    return 0;
}

int driver_autoload_on_usb_detect(uint16_t vendor, uint16_t product,
                                  uint8_t dev_class) {
    if (g_count >= AUTOLOAD_MAX_ENTRIES) return -1;

    struct autoload_entry *e = &g_entries[g_count];
    e->vendor   = vendor;
    e->device   = product;
    e->bus_type = 1; /* USB */

    const char *drv = NULL;
    switch (dev_class) {
    case 0x03: drv = "usb_hid"; break;
    case 0x08: drv = "usb_msc"; break;
    case 0x09: drv = "usb_hub"; break;
    case 0x01: drv = "usb_audio"; break;
    }

    if (!drv) {
        copy_str(e->driver_name, AUTOLOAD_NAME_MAX, "(none)");
        e->status = AUTOLOAD_UNBOUND;
        SLOG_WARN(TAG, "USB %04x:%04x class %02x -- no driver", vendor, product, dev_class);
        g_count++;
        return -1;
    }

    copy_str(e->driver_name, AUTOLOAD_NAME_MAX, drv);
    e->status = AUTOLOAD_BOUND;

    pnp_handle_hotplug(BUS_USB, vendor, product, 1);

    SLOG_INFO(TAG, "USB %04x:%04x -> driver '%s' (auto-loaded)",
              vendor, product, drv);
    g_count++;
    return 0;
}

int driver_autoload_count(void) { return g_count; }

const struct autoload_entry *driver_autoload_get(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_entries[idx];
}

void driver_autoload_dump(void) {
    kprintf("[autoload] %d entries:\n", g_count);
    for (int i = 0; i < g_count; i++) {
        const struct autoload_entry *e = &g_entries[i];
        const char *bus = e->bus_type == 0 ? "PCI" : "USB";
        const char *st;
        switch (e->status) {
        case AUTOLOAD_BOUND:    st = "bound";    break;
        case AUTOLOAD_FAILED:   st = "failed";   break;
        case AUTOLOAD_DISABLED: st = "disabled"; break;
        default:                st = "unbound";  break;
        }
        kprintf("  [%s] %04x:%04x -> %s (%s)\n", bus, e->vendor, e->device,
                e->driver_name, st);
    }
}
