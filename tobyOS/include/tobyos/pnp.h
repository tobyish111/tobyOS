#ifndef TOBYOS_PNP_H
#define TOBYOS_PNP_H

#include <tobyos/types.h>

enum device_class { DEV_STORAGE, DEV_INPUT, DEV_AUDIO, DEV_NETWORK,
                    DEV_DISPLAY, DEV_PRINTER, DEV_OTHER };
enum device_bus   { BUS_PCI, BUS_USB, BUS_PLATFORM };

struct pnp_device {
    int               id;
    enum device_class dev_class;
    enum device_bus   bus;
    uint16_t          vendor_id;
    uint16_t          product_id;
    char              name[64];
    char              driver[32];
    int               enabled;
    int               connected;
};

void pnp_init(void);
int  pnp_enumerate(struct pnp_device *out, int max);
int  pnp_enable_device(int dev_id);
int  pnp_disable_device(int dev_id);
int  pnp_get_driver(int dev_id, char *driver_name, int max);
void pnp_handle_hotplug(enum device_bus bus, uint16_t vid, uint16_t pid,
                        int connected);

#endif /* TOBYOS_PNP_H */
