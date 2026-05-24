/* driver_autoload.h -- automatic driver selection and loading for PnP.
 *
 * Enhances the existing PnP and drvconf subsystems by automatically
 * selecting and starting drivers when new PCI/USB devices are detected.
 * Parses /etc/drvmatch.conf (already loaded by drvconf) and maintains
 * a mapping from device to active driver.
 */

#ifndef TOBYOS_DRIVER_AUTOLOAD_H
#define TOBYOS_DRIVER_AUTOLOAD_H

#include <tobyos/types.h>

#define AUTOLOAD_MAX_ENTRIES  32
#define AUTOLOAD_NAME_MAX     32

enum autoload_status {
    AUTOLOAD_UNBOUND  = 0,
    AUTOLOAD_BOUND    = 1,
    AUTOLOAD_FAILED   = 2,
    AUTOLOAD_DISABLED = 3,
};

struct autoload_entry {
    uint16_t           vendor;
    uint16_t           device;
    uint8_t            bus_type;
    char               driver_name[AUTOLOAD_NAME_MAX];
    enum autoload_status status;
    int                pnp_id;
};

void driver_autoload_init(void);

int  driver_autoload_on_pci_detect(uint16_t vendor, uint16_t device,
                                   uint8_t class_code, uint8_t subclass);

int  driver_autoload_on_usb_detect(uint16_t vendor, uint16_t product,
                                   uint8_t dev_class);

int  driver_autoload_count(void);
const struct autoload_entry *driver_autoload_get(int idx);

void driver_autoload_dump(void);

#endif /* TOBYOS_DRIVER_AUTOLOAD_H */
