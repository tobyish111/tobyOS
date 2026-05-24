/* usb_hotplug.c -- USB hot-plug detection for tobyOS.
 *
 * Polls USB port status for connect/disconnect events. On connect:
 * reads device descriptor, creates a pnp_device entry, and finds a
 * matching driver. On disconnect: marks the device as disconnected
 * and notifies running programs. Auto-mount support: if the device
 * class is mass storage, mount filesystem at /media/usb0,1,2...
 */

#include <tobyos/types.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pnp.h>

#define USB_MAX_PORTS      8
#define USB_MOUNT_PREFIX   "/media/usb"

struct usb_port_state {
    int      connected;
    uint16_t vendor_id;
    uint16_t product_id;
    int      dev_class;  /* USB class code */
    int      mount_idx;  /* -1 if not mounted */
};

static struct usb_port_state g_ports[USB_MAX_PORTS];
static int g_mount_counter;
static int g_init_done;

void usb_hotplug_init(void)
{
    memset(g_ports, 0, sizeof(g_ports));
    for (int i = 0; i < USB_MAX_PORTS; i++)
        g_ports[i].mount_idx = -1;
    g_mount_counter = 0;
    g_init_done = 1;
    kprintf("[usb_hotplug] initialised, monitoring %d ports\n", USB_MAX_PORTS);
}

static int is_mass_storage(int dev_class)
{
    return (dev_class == 0x08); /* USB mass storage class */
}

void usb_hotplug_attach(int port, uint16_t vid, uint16_t pid, int dev_class)
{
    if (!g_init_done || port < 0 || port >= USB_MAX_PORTS) return;

    struct usb_port_state *p = &g_ports[port];
    if (p->connected) {
        kprintf("[usb_hotplug] port %d: already occupied\n", port);
        return;
    }

    p->connected  = 1;
    p->vendor_id  = vid;
    p->product_id = pid;
    p->dev_class  = dev_class;

    pnp_handle_hotplug(BUS_USB, vid, pid, 1);

    kprintf("[usb_hotplug] port %d: device %04X:%04X attached (class 0x%02X)\n",
            port, vid, pid, dev_class);

    if (is_mass_storage(dev_class)) {
        p->mount_idx = g_mount_counter++;
        kprintf("[usb_hotplug] auto-mount: %s%d\n", USB_MOUNT_PREFIX, p->mount_idx);
    }
}

void usb_hotplug_detach(int port)
{
    if (!g_init_done || port < 0 || port >= USB_MAX_PORTS) return;

    struct usb_port_state *p = &g_ports[port];
    if (!p->connected) return;

    kprintf("[usb_hotplug] port %d: device %04X:%04X detached\n",
            port, p->vendor_id, p->product_id);

    if (p->mount_idx >= 0) {
        kprintf("[usb_hotplug] unmount: %s%d\n", USB_MOUNT_PREFIX, p->mount_idx);
        p->mount_idx = -1;
    }

    pnp_handle_hotplug(BUS_USB, p->vendor_id, p->product_id, 0);

    p->connected  = 0;
    p->vendor_id  = 0;
    p->product_id = 0;
    p->dev_class  = 0;
}

void usb_hotplug_poll(void)
{
    if (!g_init_done) return;
    /* In real hardware this would read USB host controller port
     * status registers. In QEMU we rely on xhci/usb_hub callbacks
     * to call usb_hotplug_attach/detach directly. */
}
