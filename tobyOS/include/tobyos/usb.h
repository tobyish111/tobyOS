/* usb.h -- minimal USB core types for the milestone-21 xHCI driver.
 *
 * Scope: just enough to enumerate a device, walk its configuration
 * descriptor for HID interfaces, and run a polled interrupt-IN
 * endpoint per HID device. We deliberately do not implement:
 *
 *   - USB hubs (one device per root-hub port only)
 *   - bulk / isochronous / control-OUT data stages with multiple TDs
 *   - composite devices beyond {HID-keyboard, HID-mouse} on separate
 *     interfaces
 *   - report-descriptor parsing (Boot Protocol gives us a fixed
 *     report layout, which is all we need for kbd + mouse)
 *   - string descriptors (we never call iManufacturer/iProduct)
 *   - low-speed devices (xHCI handles them but we don't bother)
 *
 * The HCI driver (currently only xhci.c) populates a struct
 * usb_device per attached device and calls usb_hid_probe() for any
 * interface whose bInterfaceClass == USB_CLASS_HID.
 */

#ifndef TOBYOS_USB_H
#define TOBYOS_USB_H

#include <tobyos/types.h>

/* ---- standard descriptor types -------------------------------- */

#define USB_DESC_DEVICE          0x01
#define USB_DESC_CONFIG          0x02
#define USB_DESC_STRING          0x03
#define USB_DESC_INTERFACE       0x04
#define USB_DESC_ENDPOINT        0x05
#define USB_DESC_HID             0x21    /* HID class descriptor */
#define USB_DESC_HID_REPORT      0x22

/* ---- standard request types (bRequest values) ------------------ */

#define USB_REQ_GET_STATUS         0x00
#define USB_REQ_CLEAR_FEATURE      0x01
#define USB_REQ_SET_FEATURE        0x03
#define USB_REQ_SET_ADDRESS        0x05
#define USB_REQ_GET_DESCRIPTOR     0x06
#define USB_REQ_SET_DESCRIPTOR     0x07
#define USB_REQ_GET_CONFIGURATION  0x08
#define USB_REQ_SET_CONFIGURATION  0x09

/* HID class requests (bmRequestType type=Class, recipient=Interface). */
#define USB_HID_REQ_GET_REPORT     0x01
#define USB_HID_REQ_GET_IDLE       0x02
#define USB_HID_REQ_GET_PROTOCOL   0x03
#define USB_HID_REQ_SET_REPORT     0x09
#define USB_HID_REQ_SET_IDLE       0x0A
#define USB_HID_REQ_SET_PROTOCOL   0x0B

/* HID protocol values for SET_PROTOCOL. */
#define USB_HID_PROTO_BOOT         0x00
#define USB_HID_PROTO_REPORT       0x01

/* bmRequestType bit fields. We always pre-compose the byte ourselves
 * rather than masking, so these are diagnostic constants. */
#define USB_DIR_OUT                0x00
#define USB_DIR_IN                 0x80
#define USB_TYPE_STANDARD          0x00
#define USB_TYPE_CLASS             0x20
#define USB_TYPE_VENDOR            0x40
#define USB_RECIP_DEVICE           0x00
#define USB_RECIP_INTERFACE        0x01
#define USB_RECIP_ENDPOINT         0x02
#define USB_RECIP_OTHER            0x03   /* per-port hub-class requests */

/* ---- standard descriptor structs (packed, on-wire layout) ------ */

struct __attribute__((packed)) usb_setup_packet {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};
_Static_assert(sizeof(struct usb_setup_packet) == 8, "setup packet must be 8 B");

struct __attribute__((packed)) usb_device_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};
_Static_assert(sizeof(struct usb_device_desc) == 18, "device desc must be 18 B");

struct __attribute__((packed)) usb_config_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;     /* config + ifaces + endpoints + class descs */
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;        /* 2 mA units */
};
_Static_assert(sizeof(struct usb_config_desc) == 9, "config desc must be 9 B");

struct __attribute__((packed)) usb_iface_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
};
_Static_assert(sizeof(struct usb_iface_desc) == 9, "iface desc must be 9 B");

struct __attribute__((packed)) usb_endpoint_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;     /* bit 7 = direction (1=IN), bits 3:0 = ep num */
    uint8_t  bmAttributes;         /* bits 1:0 = transfer type */
    uint16_t wMaxPacketSize;       /* bits 10:0 = mps; bits 12:11 = additional xacts */
    uint8_t  bInterval;            /* polling interval (in frames or 2^(N-1) usec) */
};
_Static_assert(sizeof(struct usb_endpoint_desc) == 7, "endpoint desc must be 7 B");

/* ---- USB class codes we recognise ------------------------------ */

#define USB_CLASS_HID            0x03
#define USB_CLASS_HUB            0x09     /* M26B: USB hub class */
#define USB_CLASS_MASS_STORAGE   0x08

/* HID interface protocols (when bInterfaceSubClass == 1, "boot"). */
#define USB_HID_PROTO_KEYBOARD   0x01
#define USB_HID_PROTO_MOUSE      0x02

/* ---- USB Hub class (USB 2.0 chapter 11) ------------------------ */
/*
 * Just enough on-wire constants for usb_hub.c to enumerate downstream
 * ports. Hub-class control requests use TYPE_CLASS, and use either
 * RECIP_DEVICE (for hub-wide ops) or RECIP_OTHER (for per-port ops).
 *
 *   bRequest 0x06  GET_DESCRIPTOR  (wValue = (DT<<8) | index)
 *   bRequest 0x00  GET_STATUS      (wIndex = port; 4-byte status)
 *   bRequest 0x01  CLEAR_FEATURE   (wValue = feature, wIndex = port)
 *   bRequest 0x03  SET_FEATURE     (wValue = feature, wIndex = port)
 */
#define USB_DESC_HUB             0x29     /* descriptor type for GET_DESCRIPTOR(HUB) */

/* Hub-class port features (USB 2.0 §11.24.2 PORT_* feature selectors). */
#define USB_HUB_FEAT_PORT_CONNECTION   0
#define USB_HUB_FEAT_PORT_ENABLE       1
#define USB_HUB_FEAT_PORT_RESET        4
#define USB_HUB_FEAT_PORT_POWER        8
#define USB_HUB_FEAT_C_PORT_CONNECTION 16
#define USB_HUB_FEAT_C_PORT_RESET      20

/* GET_PORT_STATUS returns 4 bytes: low half = wPortStatus,
 * high half = wPortChange. We treat both as bitmaps. */
#define USB_HUB_PORT_STAT_CONNECTION   (1u << 0)
#define USB_HUB_PORT_STAT_ENABLE       (1u << 1)
#define USB_HUB_PORT_STAT_RESET        (1u << 4)
#define USB_HUB_PORT_STAT_POWER        (1u << 8)
#define USB_HUB_PORT_STAT_LOW_SPEED    (1u << 9)
#define USB_HUB_PORT_STAT_HIGH_SPEED   (1u << 10)

#define USB_HUB_PORT_CHG_CONNECTION    (1u << 0)
#define USB_HUB_PORT_CHG_ENABLE        (1u << 1)
#define USB_HUB_PORT_CHG_RESET         (1u << 4)

/* Standard hub descriptor (USB 2.0 §11.23.2.1). bDescLength is at
 * least 7 + 2*ceil(N/8) bytes for a hub of N ports; we cap N at 15
 * (xHCI USB-2 hub spec limit) so the DeviceRemovable + PortPwrCtrlMask
 * tail fits comfortably in 4 bytes total. */
struct __attribute__((packed)) usb_hub_desc {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;          /* 0x29 */
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;           /* in 2 ms units */
    uint8_t  bHubContrCurrent;
    uint8_t  varlen[8];                /* DeviceRemovable + PortPwrCtrlMask */
};
_Static_assert(sizeof(struct usb_hub_desc) >= 7,
               "hub descriptor must be at least 7 B");

#define USB_HUB_MAX_PORTS  15           /* tobyOS internal cap */
#define USB_HUB_MAX_DEPTH   2           /* root hub -> external hub -> device */

/* Mass-storage interface (subclass, protocol) we accept.
 *
 *   subClass = 0x06  SCSI transparent command set (the only one any
 *                     real USB stick exposes today)
 *   protocol = 0x50  Bulk-Only Transport (BBB / "BOT") -- two bulk
 *                     endpoints, three-phase CBW/data/CSW handshake.
 *
 * Class-specific control requests live in the iface-class space:
 *
 *   bRequest 0xFF  Bulk-Only Mass Storage Reset
 *   bRequest 0xFE  Get Max LUN  (returns 1 byte; 0 = single LUN device) */
#define USB_MSC_SUBCLASS_SCSI    0x06
#define USB_MSC_PROTO_BBB        0x50
#define USB_MSC_REQ_RESET        0xFF
#define USB_MSC_REQ_GET_MAX_LUN  0xFE

/* ---- endpoint helpers ------------------------------------------ */

#define USB_EP_DIR_IN(addr)      (((addr) & 0x80) != 0)
#define USB_EP_NUM(addr)         ((addr) & 0x0F)

/* USB endpoint type, bmAttributes bits 1:0. */
#define USB_EP_CONTROL           0
#define USB_EP_ISOCH             1
#define USB_EP_BULK              2
#define USB_EP_INTERRUPT         3
#define USB_EP_TYPE(attr)        ((attr) & 0x03)

/* ---- usb_device: the HCI's view of an attached USB device ----- */
/*
 * The xHCI driver allocates one of these per device that successfully
 * passes Address Device. The HCI populates the fields up to and
 * including ep0_*; the rest (interrupt endpoint state, hid_state) is
 * filled by usb_hid_probe() if the device claims to be an HID class.
 *
 * Pointers are kernel-virtual (HHDM-translated). The matching phys
 * addresses live alongside because the HCI needs them to feed back
 * into context structures and TRB ring base pointers.
 */

struct usb_device {
    /* Stable identifiers. */
    uint8_t  slot_id;          /* xHCI slot, 1..MaxSlots */
    uint8_t  port_id;          /* 1-based root-hub port number */
    uint8_t  speed;            /* USB-IF speed code (1=FS, 2=LS, 3=HS, 4=SS) */
    uint8_t  bus_address;      /* 0 until Address Device completes */

    /* M26B hub topology. For root-hub-attached devices these are all
     * zero; for hub-downstream devices they record the path through
     * the topology that the xHC + intervening hubs use to route
     * setup tokens (xHCI 1.1 §4.5.1, "Route String").
     *
     *   parent_slot_id == 0 means "directly on root hub" (legacy path).
     *   route_string is the 20-bit xHCI route encoding (4 bits per
     *   tier: tier 1 = root, tier 2 = first downstream hub, ...).
     *   hub_depth = 0 for root-hub devices, 1 for "behind one hub", etc.
     *   hub_port  = 1-based downstream port number on the parent hub.
     */
    uint8_t  parent_slot_id;
    uint32_t route_string;
    uint8_t  hub_depth;
    uint8_t  hub_port;

    /* True once xhci_configure_as_hub has flipped the Slot Context Hub
     * bit and programmed NumberOfPorts. usb_hub.c sets this after a
     * successful Configure Endpoint round trip. */
    bool     is_hub;
    uint8_t  hub_nports;

    /* Endpoint 0 max packet size. Initial guess based on speed code;
     * updated to the real value after the first 8-byte GET_DESCRIPTOR
     * succeeds. */
    uint16_t mps0;

    /* Default control endpoint (EP0). The HCI keeps the single TRB
     * ring + cycle bit. Driver-specific state stays opaque to USB
     * consumers; xhci.c casts back to its own type. */
    void    *ep0_ring;         /* virt of TRB ring */
    uint64_t ep0_ring_phys;    /* phys of TRB ring */
    uint16_t ep0_idx;          /* next slot to enqueue */
    uint8_t  ep0_cycle;        /* producer cycle state */

    /* Optional interrupt-IN endpoint claimed by usb_hid_probe(). */
    void    *int_ring;
    uint64_t int_ring_phys;
    uint16_t int_idx;
    uint8_t  int_cycle;
    uint8_t  int_dci;          /* xHCI Doorbell Channel ID, 1..31 */
    uint8_t *int_buf;
    uint64_t int_buf_phys;
    uint16_t int_buf_size;
    bool     int_armed;        /* one transfer pending? */

    /* Set by usb_hid_probe; called by xhci event drain on completion. */
    void    *hid_state;
    void   (*int_complete)(struct usb_device *dev,
                           const uint8_t *buf, uint32_t len);

    /* ---- Optional bulk-IN/bulk-OUT pair (USB Mass Storage / BBB) ----
     *
     * Populated by usb_msc_probe(). The HCI fills in the rings (it
     * builds them inside Configure-Endpoint), the class driver only
     * cares about the DCIs + max-packet sizes + iface number for
     * class-specific control requests.
     *
     * For BBB the endpoint addresses (bEndpointAddress) are also kept
     * so we can issue CLEAR_FEATURE(ENDPOINT_HALT) on stall recovery
     * (the standard request takes wIndex = endpoint address, not DCI).
     */
    void    *bulk_in_ring;
    uint64_t bulk_in_ring_phys;
    uint16_t bulk_in_idx;
    uint8_t  bulk_in_cycle;
    uint8_t  bulk_in_dci;        /* DCI = 2*ep_num + 1 */
    uint8_t  bulk_in_addr;       /* bEndpointAddress, includes IN bit */
    uint16_t bulk_in_mps;

    void    *bulk_out_ring;
    uint64_t bulk_out_ring_phys;
    uint16_t bulk_out_idx;
    uint8_t  bulk_out_cycle;
    uint8_t  bulk_out_dci;       /* DCI = 2*ep_num + 0 */
    uint8_t  bulk_out_addr;      /* bEndpointAddress, no IN bit */
    uint16_t bulk_out_mps;

    uint8_t  msc_iface_num;      /* bInterfaceNumber for MSC class reqs */
    void    *msc_state;          /* opaque to HCI; usb_msc owns it */
};

#endif /* TOBYOS_USB_H */
