/* pnp.c -- Plug and Play / hot-plug device management for tobyOS.
 *
 * Maintains a registry of all enumerated devices, grouped by class and
 * bus type. Provides enable/disable control and driver association.
 * On boot, populates from PCI enumeration results; at runtime, entries
 * are added/removed via pnp_handle_hotplug().
 */

#include <tobyos/pnp.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#define PNP_MAX_DEVICES 64

static struct pnp_device g_devices[PNP_MAX_DEVICES];
static int               g_device_count;
static int               g_next_id = 1;

static const char *class_name(enum device_class c)
{
    switch (c) {
    case DEV_STORAGE: return "Storage";
    case DEV_INPUT:   return "Input";
    case DEV_AUDIO:   return "Audio";
    case DEV_NETWORK: return "Network";
    case DEV_DISPLAY: return "Display";
    case DEV_PRINTER: return "Printer";
    case DEV_OTHER:   return "Other";
    }
    return "Unknown";
}

static const char *bus_name(enum device_bus b)
{
    switch (b) {
    case BUS_PCI:      return "PCI";
    case BUS_USB:      return "USB";
    case BUS_PLATFORM: return "Platform";
    }
    return "Unknown";
}

static enum device_class classify_pci(uint16_t vid, uint16_t pid)
{
    (void)vid;
    /* Simple heuristic based on known demo device IDs */
    if (pid == 0x100E || pid == 0x10D3 || pid == 0x8168)
        return DEV_NETWORK;
    if (pid == 0x2922 || pid == 0x8086)
        return DEV_STORAGE;
    if (pid == 0x2668)
        return DEV_AUDIO;
    if (pid == 0x1234 || pid == 0x1050)
        return DEV_DISPLAY;
    return DEV_OTHER;
}

static const char *default_driver(enum device_class c)
{
    switch (c) {
    case DEV_NETWORK: return "e1000";
    case DEV_STORAGE: return "ahci";
    case DEV_AUDIO:   return "hda";
    case DEV_DISPLAY: return "vga";
    case DEV_INPUT:   return "hid";
    default:          return "generic";
    }
}

void pnp_init(void)
{
    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
    g_next_id = 1;

    /* Synthesize a few demo platform devices */
    struct {
        const char *name;
        enum device_class cls;
        enum device_bus bus;
        uint16_t vid, pid;
    } demos[] = {
        { "Intel Ethernet",        DEV_NETWORK, BUS_PCI, 0x8086, 0x100E },
        { "AHCI SATA Controller",  DEV_STORAGE, BUS_PCI, 0x8086, 0x2922 },
        { "Intel HDA Audio",       DEV_AUDIO,   BUS_PCI, 0x8086, 0x2668 },
        { "VGA Display",           DEV_DISPLAY, BUS_PCI, 0x1234, 0x1111 },
        { "PS/2 Keyboard",         DEV_INPUT,   BUS_PLATFORM, 0, 0 },
        { "PS/2 Mouse",            DEV_INPUT,   BUS_PLATFORM, 0, 0 },
    };

    for (int i = 0; i < (int)(sizeof(demos)/sizeof(demos[0])); i++) {
        struct pnp_device *d = &g_devices[g_device_count];
        memset(d, 0, sizeof(*d));
        d->id = g_next_id++;
        d->dev_class  = demos[i].cls;
        d->bus        = demos[i].bus;
        d->vendor_id  = demos[i].vid;
        d->product_id = demos[i].pid;
        d->enabled    = 1;
        d->connected  = 1;

        int nlen = (int)strlen(demos[i].name);
        if (nlen > 63) nlen = 63;
        memcpy(d->name, demos[i].name, (size_t)nlen);
        d->name[nlen] = '\0';

        const char *drv = default_driver(demos[i].cls);
        int dlen = (int)strlen(drv);
        if (dlen > 31) dlen = 31;
        memcpy(d->driver, drv, (size_t)dlen);
        d->driver[dlen] = '\0';

        g_device_count++;
    }

    kprintf("[pnp] initialised: %d devices enumerated\n", g_device_count);
}

int pnp_enumerate(struct pnp_device *out, int max)
{
    if (!out || max <= 0) return 0;
    int count = g_device_count;
    if (count > max) count = max;
    memcpy(out, g_devices, (size_t)count * sizeof(struct pnp_device));
    return count;
}

static struct pnp_device *find_device(int dev_id)
{
    for (int i = 0; i < g_device_count; i++)
        if (g_devices[i].id == dev_id) return &g_devices[i];
    return NULL;
}

int pnp_enable_device(int dev_id)
{
    struct pnp_device *d = find_device(dev_id);
    if (!d) return -1;
    d->enabled = 1;
    kprintf("[pnp] device %d '%s' enabled\n", dev_id, d->name);
    return 0;
}

int pnp_disable_device(int dev_id)
{
    struct pnp_device *d = find_device(dev_id);
    if (!d) return -1;
    d->enabled = 0;
    kprintf("[pnp] device %d '%s' disabled\n", dev_id, d->name);
    return 0;
}

int pnp_get_driver(int dev_id, char *driver_name, int max)
{
    struct pnp_device *d = find_device(dev_id);
    if (!d || !driver_name || max <= 0) return -1;
    int len = (int)strlen(d->driver);
    if (len >= max) len = max - 1;
    memcpy(driver_name, d->driver, (size_t)len);
    driver_name[len] = '\0';
    return 0;
}

void pnp_handle_hotplug(enum device_bus bus, uint16_t vid, uint16_t pid,
                        int connected)
{
    if (connected) {
        if (g_device_count >= PNP_MAX_DEVICES) {
            kprintf("[pnp] hotplug: device limit reached\n");
            return;
        }

        struct pnp_device *d = &g_devices[g_device_count];
        memset(d, 0, sizeof(*d));
        d->id         = g_next_id++;
        d->bus        = bus;
        d->vendor_id  = vid;
        d->product_id = pid;
        d->enabled    = 1;
        d->connected  = 1;
        d->dev_class  = (bus == BUS_PCI) ? classify_pci(vid, pid) : DEV_OTHER;

        ksnprintf(d->name, sizeof(d->name), "%s Device %04X:%04X",
                  bus_name(bus), vid, pid);

        const char *drv = default_driver(d->dev_class);
        int dlen = (int)strlen(drv);
        if (dlen > 31) dlen = 31;
        memcpy(d->driver, drv, (size_t)dlen);
        d->driver[dlen] = '\0';

        g_device_count++;
        kprintf("[pnp] hotplug attach: %s (%s)\n", d->name, class_name(d->dev_class));
    } else {
        /* Mark device as disconnected */
        for (int i = 0; i < g_device_count; i++) {
            struct pnp_device *d = &g_devices[i];
            if (d->bus == bus && d->vendor_id == vid &&
                d->product_id == pid && d->connected) {
                d->connected = 0;
                kprintf("[pnp] hotplug detach: %s\n", d->name);
                return;
            }
        }
    }
}
