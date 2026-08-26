/* sysfs.c -- /sys virtual filesystem exposing kernel/hardware state.
 *
 * Provides a read-only virtual tree at /sys with subtrees:
 *   /sys/cpu/count       - number of online CPUs
 *   /sys/cpu/model       - CPU model string
 *   /sys/mem/total       - total physical RAM in bytes
 *   /sys/mem/free        - free physical RAM in bytes
 *   /sys/kernel/version  - kernel version string
 *   /sys/kernel/uptime   - uptime in milliseconds
 *   /sys/devices/...     - enumerated devices
 *
 * Mounts via the VFS as a standard filesystem driver at /sys.
 */

#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pmm.h>
#include <tobyos/pci.h>    /* /sys/bus/pci/devices population */
#include <tobyos/usbreg.h> /* /sys/bus/usb/devices population */
#include <tobyos/smbios.h> /* /sys/firmware/dmi + /sys/class/dmi/id  */
#include <tobyos/cputelem.h> /* /sys/class/hwmon + cpuN/cpufreq        */
#include <tobyos/types.h>

extern uint64_t pit_ticks(void);
extern uint32_t pit_hz(void);
extern uint32_t smp_cpu_count(void);

/* 96 -> 320 (2026-08-23): the PCI tree costs 1 dir + 7 files per
 * function, and a desktop board enumerates a dozen or more. The table is
 * static, so this is ~320 * (VFS_PATH_MAX + a few words) of BSS.
 *
 * 320 -> 640 (2026-08-24): the USB tree costs 1 dir + up to 16 files per
 * device (USBREG_MAX = 16 of them), and 320 was ALREADY within reach on a
 * real board -- an EliteDesk enumerates ~25 PCI functions, which is 225
 * nodes of PCI alone against a base of 33. The headroom checks below
 * degrade gracefully, but a machine that silently exposes only some of
 * its devices is the kind of half-truth this arc exists to avoid. */
#define SYSFS_MAX_NODES 640
#define SYSFS_BUF_SIZE  256

struct sysfs_node {
    char path[VFS_PATH_MAX];
    enum vfs_type type;
    /* For files: function to generate content dynamically */
    int (*generate)(char *buf, size_t bufsz);
    /* Slice 100: for VFS_TYPE_SYMLINK, the literal target. Sysfs served
     * only files and dirs, but libdrm READLINKS
     * /sys/dev/char/<maj>:<min>/device/subsystem and keys the bus type on
     * the target's basename -- a regular file cannot answer that. */
    const char *link;
    /* 2026-08-23: PRE-RENDERED file content, owned by this node. The
     * generator signature carries no context, so one function cannot
     * serve N per-device files (every PCI device needs its own vendor,
     * device, class, ...). PCI topology is fixed once pci_init has run,
     * so those files are rendered once at mount and stored here. NULL
     * for every generator-backed or directory node. */
    char *fixed;
    /* 2026-08-24: BINARY content, served straight from kernel memory.
     * `fixed` cannot carry it for two reasons -- it is NUL-terminated
     * (the DMI table is full of NULs) and the read path copies it into a
     * 256-byte scratch buffer, while an SMBIOS table is kilobytes. A blob
     * node points at memory the kernel already owns for the life of the
     * system, so nothing is copied at all. */
    const uint8_t *blob;
    size_t         blob_len;
    /* Linux keeps the DMI serial numbers root-only; every other sysfs
     * node here is 0444. 0 means "use the default". */
    uint32_t mode;
};

static struct sysfs_node g_sysfs_nodes[SYSFS_MAX_NODES];
static int g_sysfs_count;

/* ---- content generators ---- */

static int gen_cpu_count(char *buf, size_t sz) {
    uint32_t n = smp_cpu_count();
    int len = 0;
    char tmp[32];
    uint32_t v = n;
    if (v == 0) { tmp[0] = '0'; len = 1; }
    else {
        while (v > 0) { tmp[len++] = '0' + (v % 10); v /= 10; }
        for (int i = 0; i < len / 2; i++) {
            char c = tmp[i]; tmp[i] = tmp[len-1-i]; tmp[len-1-i] = c;
        }
    }
    tmp[len++] = '\n';
    if ((size_t)len >= sz) len = (int)sz - 1;
    memcpy(buf, tmp, (size_t)len);
    buf[len] = '\0';
    return len;
}

static int gen_mem_total(char *buf, size_t sz) {
    extern size_t pmm_total_pages(void);
    size_t total = pmm_total_pages() * 4096;
    char tmp[32]; int len = 0;
    size_t v = total;
    if (v == 0) { tmp[0] = '0'; len = 1; }
    else {
        while (v > 0) { tmp[len++] = '0' + (char)(v % 10); v /= 10; }
        for (int i = 0; i < len / 2; i++) {
            char c = tmp[i]; tmp[i] = tmp[len-1-i]; tmp[len-1-i] = c;
        }
    }
    tmp[len++] = '\n';
    if ((size_t)len >= sz) len = (int)sz - 1;
    memcpy(buf, tmp, (size_t)len);
    buf[len] = '\0';
    return len;
}

static int gen_mem_free(char *buf, size_t sz) {
    extern size_t pmm_free_pages(void);
    size_t fr = pmm_free_pages() * 4096;
    char tmp[32]; int len = 0;
    size_t v = fr;
    if (v == 0) { tmp[0] = '0'; len = 1; }
    else {
        while (v > 0) { tmp[len++] = '0' + (char)(v % 10); v /= 10; }
        for (int i = 0; i < len / 2; i++) {
            char c = tmp[i]; tmp[i] = tmp[len-1-i]; tmp[len-1-i] = c;
        }
    }
    tmp[len++] = '\n';
    if ((size_t)len >= sz) len = (int)sz - 1;
    memcpy(buf, tmp, (size_t)len);
    buf[len] = '\0';
    return len;
}

static int gen_kernel_version(char *buf, size_t sz) {
    const char *ver = "tobyOS 1.0.0\n";
    size_t len = strlen(ver);
    if (len >= sz) len = sz - 1;
    memcpy(buf, ver, len);
    buf[len] = '\0';
    return (int)len;
}

static int gen_kernel_uptime(char *buf, size_t sz) {
    uint32_t hz = pit_hz();
    uint64_t ms = (hz > 0) ? (pit_ticks() * 1000ULL) / (uint64_t)hz : 0;
    char tmp[32]; int len = 0;
    uint64_t v = ms;
    if (v == 0) { tmp[0] = '0'; len = 1; }
    else {
        while (v > 0) { tmp[len++] = '0' + (char)(v % 10); v /= 10; }
        for (int i = 0; i < len / 2; i++) {
            char c = tmp[i]; tmp[i] = tmp[len-1-i]; tmp[len-1-i] = c;
        }
    }
    tmp[len++] = '\n';
    if ((size_t)len >= sz) len = (int)sz - 1;
    memcpy(buf, tmp, (size_t)len);
    buf[len] = '\0';
    return len;
}

/* B24: Linux-compatible nodes. get_nprocs()/sysconf(_SC_NPROCESSORS_ONLN)
 * read /sys/devices/system/cpu/online (the cpulist: "0" for one CPU,
 * "0-<n-1>" for several); uname-ish probes read /sys/kernel/{ostype,osrelease}. */
static int gen_cpu_online(char *buf, size_t sz) {
    uint32_t n = smp_cpu_count();
    if (n == 0) n = 1;
    int len = 0;
    buf[len++] = '0';
    if (n > 1) {
        buf[len++] = '-';
        char tmp[16]; int tl = 0; uint32_t v = n - 1;
        if (v == 0) tmp[tl++] = '0';
        else { while (v) { tmp[tl++] = (char)('0' + v % 10); v /= 10; } }
        while (tl > 0 && (size_t)len < sz - 2) buf[len++] = tmp[--tl];
    }
    buf[len++] = '\n';
    buf[len] = '\0';
    return len;
}

static int gen_ostype(char *buf, size_t sz) {
    const char *s = "Linux\n";
    size_t len = strlen(s);
    if (len >= sz) len = sz - 1;
    memcpy(buf, s, len); buf[len] = '\0';
    return (int)len;
}

static int gen_osrelease(char *buf, size_t sz) {
    const char *s = "6.1.0-tobyos\n";
    size_t len = strlen(s);
    if (len >= sz) len = sz - 1;
    memcpy(buf, s, len); buf[len] = '\0';
    return (int)len;
}

/* Slice 100: PCI identity of the virtio-gpu device, in the exact shapes
 * libdrm parses. sysfs reports these as "0x%04x\n" text. */
static int gen_fixed(char *buf, size_t sz, const char *s) {
    size_t n = strlen(s);
    if (n >= sz) n = sz - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return (int)n;
}
static int gen_pci_vendor(char *b, size_t s)   { return gen_fixed(b, s, "0x1af4\n"); }
static int gen_pci_device(char *b, size_t s)   { return gen_fixed(b, s, "0x1050\n"); }
static int gen_pci_revision(char *b, size_t s) { return gen_fixed(b, s, "0x01\n"); }
static int gen_pci_subdev(char *b, size_t s)   { return gen_fixed(b, s, "0x1100\n"); }
/* Linux libdrm does NOT read the text vendor/device files -- on Linux
 * drmParsePciDeviceInfo() opens .../device/config and reads the first 64
 * BINARY bytes of PCI config space. Synthesize that header for the
 * virtio-gpu function (1af4:1050, class 0x030000 display, subsystem
 * 1af4:1100). Returns an explicit 64, so the NUL bytes inside are fine. */
static int gen_pci_config(char *b, size_t s) {
    if (s < 64) return 0;
    memset(b, 0, 64);
    b[0x00] = 0xf4; b[0x01] = 0x1a;          /* vendor 1af4 */
    b[0x02] = 0x50; b[0x03] = 0x10;          /* device 1050 */
    b[0x04] = 0x07; b[0x05] = 0x00;          /* command: io+mem+busmaster */
    b[0x06] = 0x10; b[0x07] = 0x00;          /* status: cap list */
    b[0x08] = 0x01;                          /* revision 01 */
    b[0x09] = 0x00; b[0x0a] = 0x00; b[0x0b] = 0x03;  /* class 030000 */
    b[0x0e] = 0x00;                          /* header type 0 */
    b[0x2c] = 0xf4; b[0x2d] = 0x1a;          /* subsystem vendor 1af4 */
    b[0x2e] = 0x00; b[0x2f] = 0x11;          /* subsystem device 1100 */
    b[0x34] = 0x40;                          /* capabilities pointer */
    return 64;
}

static int gen_drm_uevent(char *b, size_t s) {
    return gen_fixed(b, s,
        "DRIVER=virtio_gpu\n"
        "PCI_CLASS=30000\n"
        "PCI_ID=1AF4:1050\n"
        "PCI_SUBSYS_ID=1AF4:1100\n"
        "PCI_SLOT_NAME=0000:00:04.0\n"
        "MODALIAS=pci:v00001AF4d00001050sv00001AF4sd00001100bc03sc00i00\n");
}

/* ---- registration ---- */

static void sysfs_populate_pci(void);

static void sysfs_add_dir(const char *path) {
    if (g_sysfs_count >= SYSFS_MAX_NODES) return;
    /* Idempotent (2026-08-24). Several populators now need the same
     * ancestor -- /class is wanted by both the DMI id tree and hwmon --
     * and adding it twice put two nodes with one path in the table, which
     * readdir would then list twice. Lookup would not have noticed, so
     * this would have shown up only as a duplicated directory entry. */
    for (int i = 0; i < g_sysfs_count; i++) {
        if (g_sysfs_nodes[i].type == VFS_TYPE_DIR &&
            strcmp(g_sysfs_nodes[i].path, path) == 0) return;
    }
    struct sysfs_node *n = &g_sysfs_nodes[g_sysfs_count++];
    size_t len = strlen(path);
    if (len >= VFS_PATH_MAX) len = VFS_PATH_MAX - 1;
    memcpy(n->path, path, len);
    n->path[len] = '\0';
    n->type = VFS_TYPE_DIR;
    n->generate = 0;
}

static void sysfs_add_file(const char *path, int (*gen)(char *, size_t)) {
    if (g_sysfs_count >= SYSFS_MAX_NODES) return;
    struct sysfs_node *n = &g_sysfs_nodes[g_sysfs_count++];
    size_t len = strlen(path);
    if (len >= VFS_PATH_MAX) len = VFS_PATH_MAX - 1;
    memcpy(n->path, path, len);
    n->path[len] = '\0';
    n->type = VFS_TYPE_FILE;
    n->generate = gen;
}

/* Add a file whose bytes are computed NOW and remembered. `text` is
 * copied; the caller keeps ownership of its buffer. */
static void sysfs_add_fixed(const char *path, const char *text) {
    if (g_sysfs_count >= SYSFS_MAX_NODES) return;
    size_t tlen = strlen(text);
    char *copy = (char *)kmalloc(tlen + 1);
    if (!copy) return;
    memcpy(copy, text, tlen);
    copy[tlen] = '\0';
    struct sysfs_node *n = &g_sysfs_nodes[g_sysfs_count++];
    size_t len = strlen(path);
    if (len >= VFS_PATH_MAX) len = VFS_PATH_MAX - 1;
    memcpy(n->path, path, len);
    n->path[len] = '\0';
    n->type = VFS_TYPE_FILE;
    n->generate = 0;
    n->fixed = copy;
}

/* ---- /sys/bus/pci/devices (2026-08-23) ----------------------------
 *
 * The layout pciutils' lspci and busybox's lspci both walk: one
 * directory per function named <domain:bus:dev.fn>, each holding the
 * identity as lowercase 0x-prefixed text. `class` is the full 24-bit
 * class/subclass/prog-if word, which is what tools decode into "VGA
 * compatible controller" and friends.
 *
 * Until now /sys/bus/pci existed but was EMPTY -- it was created only so
 * the DRM subsystem symlink had somewhere to point -- so every
 * sysfs-walking tool saw a machine with no PCI devices at all. */
static void sysfs_populate_pci(void) {
    sysfs_add_dir("/bus/pci/devices");

    size_t n = pci_device_count();
    size_t made = 0;
    for (size_t i = 0; i < n; i++) {
        struct pci_dev *d = pci_device_at(i);
        if (!d) continue;
        /* Leave headroom: each device costs 1 dir + 8 files. */
        if (g_sysfs_count + 10 >= SYSFS_MAX_NODES) {
            kprintf("[sysfs] WARN: node table full -- %lu of %lu PCI "
                    "device(s) exposed\n",
                    (unsigned long)made, (unsigned long)n);
            break;
        }

        char dir[VFS_PATH_MAX], path[VFS_PATH_MAX], val[64];
        ksnprintf(dir, sizeof dir, "/bus/pci/devices/0000:%02x:%02x.%u",
                  (unsigned)d->bus, (unsigned)d->slot, (unsigned)d->fn);
        sysfs_add_dir(dir);

        ksnprintf(path, sizeof path, "%s/vendor", dir);
        ksnprintf(val, sizeof val, "0x%04x\n", (unsigned)d->vendor);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/device", dir);
        ksnprintf(val, sizeof val, "0x%04x\n", (unsigned)d->device);
        sysfs_add_fixed(path, val);

        /* class/subclass/prog-if packed exactly as PCI config space has
         * it -- lspci reads this one word, not three files. */
        ksnprintf(path, sizeof path, "%s/class", dir);
        ksnprintf(val, sizeof val, "0x%02x%02x%02x\n",
                  (unsigned)d->class_code, (unsigned)d->subclass,
                  (unsigned)d->prog_if);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/revision", dir);
        ksnprintf(val, sizeof val, "0x%02x\n", (unsigned)d->revision);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/irq", dir);
        ksnprintf(val, sizeof val, "%u\n",
                  d->irq_line == 0xFF ? 0u : (unsigned)d->irq_line);
        sysfs_add_fixed(path, val);

        /* We do not track subsystem ids per device; report the device's
         * own ids rather than inventing a different vendor, which is what
         * a device with no subsystem header reads as anyway. */
        ksnprintf(path, sizeof path, "%s/subsystem_vendor", dir);
        ksnprintf(val, sizeof val, "0x%04x\n", (unsigned)d->vendor);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/subsystem_device", dir);
        ksnprintf(val, sizeof val, "0x%04x\n", (unsigned)d->device);
        sysfs_add_fixed(path, val);

        /* uevent -- THE file busybox's lspci actually parses. It ignores
         * vendor/device/class entirely and pulls PCI_SLOT_NAME, PCI_CLASS
         * and PCI_ID out of here; with the file absent it printed one
         * "(null) Class 0000: 0000:0000" per device. Keys are Linux's
         * spelling: class is the bare 24-bit hex with no 0x, ids are
         * uppercase VVVV:DDDD. */
        {
            char ue[SYSFS_BUF_SIZE];
            ksnprintf(ue, sizeof ue,
                      "DRIVER=%s\n"
                      "PCI_CLASS=%X\n"
                      "PCI_ID=%04X:%04X\n"
                      "PCI_SUBSYS_ID=%04X:%04X\n"
                      "PCI_SLOT_NAME=0000:%02x:%02x.%u\n",
                      (d->driver && d->driver->name) ? d->driver->name
                                                     : "(none)",
                      ((unsigned)d->class_code << 16) |
                      ((unsigned)d->subclass << 8) | (unsigned)d->prog_if,
                      (unsigned)d->vendor, (unsigned)d->device,
                      (unsigned)d->vendor, (unsigned)d->device,
                      (unsigned)d->bus, (unsigned)d->slot, (unsigned)d->fn);
            ksnprintf(path, sizeof path, "%s/uevent", dir);
            sysfs_add_fixed(path, ue);
        }

        made++;
    }
    kprintf("[sysfs] /sys/bus/pci/devices: %lu device(s) exposed\n",
            (unsigned long)made);
}

/* ---- /sys/bus/usb/devices (2026-08-24) ----------------------------
 *
 * The layout usbutils' lsusb walks: one directory per device, named
 * <busnum>-<rootport>[.<hubport>...], holding the device descriptor as
 * text attributes. Linux additionally synthesises a root-hub device per
 * host controller (with its OWN 1d6b:0002/0003 ids, which no silicon
 * ever reported); tobyOS does not, so nothing here is a device that
 * did not answer GET_DESCRIPTOR.
 *
 * Bus number: tobyOS binds exactly ONE xHC (xhci_probe refuses a second
 * with "already bound"), and it is the only host controller that records
 * into usbreg -- usb_legacy.c drives UHCI/OHCI/EHCI for diagnostics and
 * legacy input without registering devices. So bus 1 is a fact here, not
 * a placeholder. If a second controller ever registers, this must carry
 * a real controller index instead.
 *
 * Port numbers are the xHC's OWN root-port indices, which is NOT what
 * Linux would print for the same machine: Linux's xhci driver registers
 * two HCDs per controller (USB2 primary + USB3 shared) and renumbers
 * ports within each, so a hub on qemu-xhci's USB2 port 1 -- xHCI root
 * port 5, because ports 1-4 are the SuperSpeed set -- is "1-1" there and
 * "1-5" here. tobyOS drives one flat bus, so it reports the port number
 * it actually used. Faking Linux's split would mean inventing a second
 * bus that does not exist.
 *
 * SNAPSHOT, not a live view: nodes are built once at sysfs_init, which
 * runs long after pci_bind_drivers has enumerated the boot-time USB
 * topology. A device hot-plugged later attaches to usbreg but does NOT
 * appear here -- the node table is append-only. Same limitation the PCI
 * tree has; called out because for USB it is much easier to hit.
 */
static const char *usb_speed_mbps(uint8_t code) {
    /* Linux's `speed` attribute is the signalling rate in Mbit/s as a
     * decimal string. Anything we do not recognise gets NO attribute
     * rather than a guessed rate. */
    switch (code) {
        case 1: return "12";      /* full  */
        case 2: return "1.5";     /* low   */
        case 3: return "480";     /* high  */
        case 4: return "5000";    /* super */
        default: return NULL;
    }
}

static void sysfs_populate_usb(void) {
    sysfs_add_dir("/bus/usb");
    sysfs_add_dir("/bus/usb/devices");

    size_t made = 0, skipped_full = 0;
    for (size_t i = 0; i < USBREG_MAX; i++) {
        const struct usbreg_entry *e = usbreg_get(i);
        if (!e) continue;                        /* FREE row */
        if (e->status == USBREG_STATUS_GONE) continue;  /* detached */

        /* Each device costs 1 dir + up to 16 files. */
        if (g_sysfs_count + 18 >= SYSFS_MAX_NODES) { skipped_full++; continue; }

        /* Linux port-path name. hub_depth nibbles of the xHCI route
         * string spell the port taken at each tier below the root. */
        char dir[VFS_PATH_MAX], path[VFS_PATH_MAX], val[64];
        int dl = ksnprintf(dir, sizeof dir, "/bus/usb/devices/1-%u",
                           (unsigned)e->port_id);
        for (unsigned t = 0; t < e->hub_depth && dl > 0 &&
                             (size_t)dl < sizeof dir; t++) {
            dl += ksnprintf(dir + dl, sizeof dir - (size_t)dl, ".%u",
                            (unsigned)((e->route_string >> (t * 4)) & 0xFu));
        }
        sysfs_add_dir(dir);

        ksnprintf(path, sizeof path, "%s/idVendor", dir);
        ksnprintf(val, sizeof val, "%04x\n", (unsigned)e->vendor);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/idProduct", dir);
        ksnprintf(val, sizeof val, "%04x\n", (unsigned)e->product);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/bDeviceClass", dir);
        ksnprintf(val, sizeof val, "%02x\n", (unsigned)e->dev_class);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/bDeviceSubClass", dir);
        ksnprintf(val, sizeof val, "%02x\n", (unsigned)e->dev_subclass);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/bDeviceProtocol", dir);
        ksnprintf(val, sizeof val, "%02x\n", (unsigned)e->dev_protocol);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/busnum", dir);
        sysfs_add_fixed(path, "1\n");

        const char *spd = usb_speed_mbps(e->speed);
        if (spd) {
            ksnprintf(path, sizeof path, "%s/speed", dir);
            ksnprintf(val, sizeof val, "%s\n", spd);
            sysfs_add_fixed(path, val);
        }

        /* Everything below comes from the full device descriptor, which
         * only exists once usbreg_record_details has run. Without it the
         * fields would all read zero, and a bcdUSB of 0.00 is a lie
         * where an absent file is the truth. */
        if (e->have_details) {
            ksnprintf(path, sizeof path, "%s/devnum", dir);
            ksnprintf(val, sizeof val, "%u\n", (unsigned)e->bus_address);
            sysfs_add_fixed(path, val);

            /* Linux renders bcdUSB space-padded to width 2: " 2.00". */
            ksnprintf(path, sizeof path, "%s/version", dir);
            ksnprintf(val, sizeof val, "%s%x.%02x\n",
                      (e->bcd_usb >> 8) < 0x10 ? " " : "",
                      (unsigned)(e->bcd_usb >> 8),
                      (unsigned)(e->bcd_usb & 0xFFu));
            sysfs_add_fixed(path, val);

            ksnprintf(path, sizeof path, "%s/bcdDevice", dir);
            ksnprintf(val, sizeof val, "%04x\n", (unsigned)e->bcd_device);
            sysfs_add_fixed(path, val);

            ksnprintf(path, sizeof path, "%s/bMaxPacketSize0", dir);
            ksnprintf(val, sizeof val, "%u\n", (unsigned)e->max_packet0);
            sysfs_add_fixed(path, val);

            ksnprintf(path, sizeof path, "%s/bNumConfigurations", dir);
            ksnprintf(val, sizeof val, "%u\n", (unsigned)e->num_configs);
            sysfs_add_fixed(path, val);

            /* String descriptors. An empty string means the device has
             * no such index (or the fetch failed) -- Linux omits the
             * attribute in exactly that case, and so do we. */
            if (e->manufacturer[0]) {
                ksnprintf(path, sizeof path, "%s/manufacturer", dir);
                ksnprintf(val, sizeof val, "%s\n", e->manufacturer);
                sysfs_add_fixed(path, val);
            }
            if (e->prod_name[0]) {
                ksnprintf(path, sizeof path, "%s/product", dir);
                ksnprintf(val, sizeof val, "%s\n", e->prod_name);
                sysfs_add_fixed(path, val);
            }
            if (e->serial[0]) {
                ksnprintf(path, sizeof path, "%s/serial", dir);
                ksnprintf(val, sizeof val, "%s\n", e->serial);
                sysfs_add_fixed(path, val);
            }
        }

        /* uevent, in Linux's spelling for a usb_device. MAJOR/MINOR/
         * DEVNAME are deliberately absent: there is no /dev/bus/usb node
         * to name. PRODUCT is vid/pid/bcdDevice, TYPE is the class
         * triple in DECIMAL. */
        {
            char ue[SYSFS_BUF_SIZE];
            ksnprintf(ue, sizeof ue,
                      "DEVTYPE=usb_device\n"
                      "DRIVER=%s\n"
                      "PRODUCT=%x/%x/%x\n"
                      "TYPE=%u/%u/%u\n"
                      "BUSNUM=001\n"
                      "DEVNUM=%03u\n",
                      e->driver[0] ? e->driver : "(none)",
                      (unsigned)e->vendor, (unsigned)e->product,
                      (unsigned)e->bcd_device,
                      (unsigned)e->dev_class, (unsigned)e->dev_subclass,
                      (unsigned)e->dev_protocol,
                      (unsigned)e->bus_address);
            ksnprintf(path, sizeof path, "%s/uevent", dir);
            sysfs_add_fixed(path, ue);
        }

        made++;
    }
    if (skipped_full) {
        kprintf("[sysfs] WARN: node table full -- %lu USB device(s) NOT "
                "exposed\n", (unsigned long)skipped_full);
    }
    kprintf("[sysfs] /sys/bus/usb/devices: %lu device(s) exposed\n",
            (unsigned long)made);
}

/* Add a file whose bytes are BINARY kernel memory, served in place. The
 * caller must guarantee the pointer outlives the mount (SMBIOS's does --
 * it is firmware memory mapped into the HHDM for the life of the
 * system). */
static void sysfs_add_blob(const char *path, const uint8_t *data, size_t len,
                           uint32_t mode) {
    if (g_sysfs_count >= SYSFS_MAX_NODES) return;
    if (!data || !len) return;
    struct sysfs_node *n = &g_sysfs_nodes[g_sysfs_count++];
    size_t plen = strlen(path);
    if (plen >= VFS_PATH_MAX) plen = VFS_PATH_MAX - 1;
    memcpy(n->path, path, plen);
    n->path[plen] = '\0';
    n->type     = VFS_TYPE_FILE;
    n->generate = 0;
    n->blob     = data;
    n->blob_len = len;
    n->mode     = mode;
}

static void sysfs_add_fixed_mode(const char *path, const char *text,
                                 uint32_t mode) {
    int before = g_sysfs_count;
    sysfs_add_fixed(path, text);
    if (g_sysfs_count > before) g_sysfs_nodes[before].mode = mode;
}

/* ---- /sys/firmware/dmi + /sys/class/dmi/id (2026-08-24) -------------
 *
 * Two surfaces over one table, because Linux software reads two:
 *   - dmidecode(8) opens /sys/firmware/dmi/tables/{smbios_entry_point,DMI}
 *     and decodes the raw structures itself.
 *   - almost everything else (systemd, hwinfo, lots of shell scripts)
 *     reads the pre-parsed one-line files under /sys/class/dmi/id.
 *
 * If the firmware reported no SMBIOS at all, NOTHING is published -- not
 * empty files, not "unknown". An absent attribute is how Linux says "this
 * machine did not tell me", and a tool can act on that; a file containing
 * a guess it cannot distinguish from a fact is worse than no file.
 */
static void sysfs_populate_dmi(void) {
    const struct smbios_info *si = smbios_get();
    if (!si->present) {
        kprintf("[sysfs] /sys/firmware/dmi: no SMBIOS -- nothing published\n");
        return;
    }

    sysfs_add_dir("/firmware");
    sysfs_add_dir("/firmware/dmi");
    sysfs_add_dir("/firmware/dmi/tables");
    sysfs_add_blob("/firmware/dmi/tables/smbios_entry_point",
                   si->entry, si->entry_len, 00400u);
    sysfs_add_blob("/firmware/dmi/tables/DMI",
                   si->table, si->table_len, 00400u);

    /* The identity files. Each is one line, and each is present only if
     * the firmware actually reported it. */
    static const struct { const char *name; int id; bool secret; } ids[] = {
        { "bios_vendor",      SMBIOS_ID_BIOS_VENDOR,      false },
        { "bios_version",     SMBIOS_ID_BIOS_VERSION,     false },
        { "bios_date",        SMBIOS_ID_BIOS_DATE,        false },
        { "sys_vendor",       SMBIOS_ID_SYS_VENDOR,       false },
        { "product_name",     SMBIOS_ID_PRODUCT_NAME,     false },
        { "product_version",  SMBIOS_ID_PRODUCT_VERSION,  false },
        { "product_serial",   SMBIOS_ID_PRODUCT_SERIAL,   true  },
        { "board_vendor",     SMBIOS_ID_BOARD_VENDOR,     false },
        { "board_name",       SMBIOS_ID_BOARD_NAME,       false },
        { "board_version",    SMBIOS_ID_BOARD_VERSION,    false },
        { "board_serial",     SMBIOS_ID_BOARD_SERIAL,     true  },
        { "chassis_vendor",   SMBIOS_ID_CHASSIS_VENDOR,   false },
    };

    bool made_dir = false;
    size_t published = 0;
    char path[VFS_PATH_MAX], val[96];
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const char *s = smbios_id_string(ids[i].id);
        if (!s) continue;
        if (!made_dir) {
            sysfs_add_dir("/class");
            sysfs_add_dir("/class/dmi");
            sysfs_add_dir("/class/dmi/id");
            made_dir = true;
        }
        ksnprintf(path, sizeof path, "/class/dmi/id/%s", ids[i].name);
        ksnprintf(val, sizeof val, "%s\n", s);
        /* Serial numbers are 0400 on Linux; match that rather than
         * publishing a machine's serial world-readable. */
        sysfs_add_fixed_mode(path, val, ids[i].secret ? 00400u : 00444u);
        published++;
    }

    kprintf("[sysfs] /sys/firmware/dmi: SMBIOS %u.%u via %s, "
            "%lu-byte table, %lu id file(s)\n",
            si->major, si->minor, si->source,
            (unsigned long)si->table_len, (unsigned long)published);
}

/* ---- /sys/class/hwmon + /sys/devices/system/cpu/cpuN/cpufreq (2026-08-24)
 *
 * temp1_input is a GENERATOR, not a pre-rendered value: a temperature
 * that was true at boot is a lie by the time anything reads it. The
 * frequency files are the opposite -- MSR_PLATFORM_INFO describes fixed
 * operating points, so those are rendered once.
 *
 * Both publish NOTHING when their MSR gate said no. See cputelem.h: a
 * per-core register cannot be filed under a named CPU without a
 * cross-CPU read, and this kernel has none.
 */
static int gen_temp1_input(char *buf, size_t sz) {
    int32_t mc = 0;
    if (!cputherm_read_mc(&mc)) {
        /* The sensor exists but says its reading is not valid right now.
         * An empty file is how that is said; a stale number is not. */
        if (sz) buf[0] = '\0';
        return 0;
    }
    return ksnprintf(buf, sz, "%d\n", mc);
}

static void sysfs_populate_hwmon(void) {
    const struct cputherm_info *ti = cputherm_get();
    if (!ti->present) {
        kprintf("[sysfs] /sys/class/hwmon: no sensor -- nothing published\n");
        return;
    }
    char val[64];
    sysfs_add_dir("/class");                 /* idempotent-ish: see note */
    sysfs_add_dir("/class/hwmon");
    sysfs_add_dir("/class/hwmon/hwmon0");
    /* `name` identifies the sensor interface to userspace; "coretemp" is
     * the standard identifier for Intel's package/core thermal MSRs,
     * which is exactly what this is. */
    sysfs_add_fixed("/class/hwmon/hwmon0/name", "coretemp\n");
    sysfs_add_file ("/class/hwmon/hwmon0/temp1_input", gen_temp1_input);
    ksnprintf(val, sizeof val, "%s\n", ti->label);
    sysfs_add_fixed("/class/hwmon/hwmon0/temp1_label", val);
    ksnprintf(val, sizeof val, "%u\n", ti->tjmax_c * 1000u);
    sysfs_add_fixed("/class/hwmon/hwmon0/temp1_crit", val);
    kprintf("[sysfs] /sys/class/hwmon/hwmon0: coretemp, TjMax %u C\n",
            ti->tjmax_c);
}

/* Called AFTER smp_start_aps(), NOT from sysfs_init().
 *
 * REAL-HARDWARE BUG, 2026-08-24: this used to run inside sysfs_init(),
 * which kmain calls at line ~4117 -- SIXTY LINES BEFORE smp_start_aps().
 * At that moment only the BSP is online, so smp_cpu_count() answers 1 and
 * an EliteDesk with four cores published exactly one cpufreq directory:
 *
 *     [sysfs] /sys/devices/system/cpu/cpuN/cpufreq: 1 cpu(s), base 3300000 kHz
 *
 * QEMU could never catch it -- there the vendor gate declines and this
 * loop never runs at all -- so it took a machine where the feature
 * actually works to expose it. Publishing 1 of 4 CPUs is exactly the
 * quiet half-truth this arc exists to avoid.
 *
 * Note /sys/devices/system/cpu/online was UNAFFECTED: it is a generator,
 * evaluated at read time, so it reported 0-3 correctly all along. That
 * contrast is the lesson -- a value captured at init is only as true as
 * the moment it was captured. */
static int g_cpufreq_published;

void sysfs_publish_cpufreq(void) {
    if (g_cpufreq_published) return;      /* idempotent: add_fixed is not */
    g_cpufreq_published = 1;

    const struct cpufreq_msr_info *fi = cpufreq_msr_get();
    if (!fi->present) {
        kprintf("[sysfs] /sys/devices/system/cpu/*/cpufreq: no P-state info "
                "-- nothing published\n");
        return;
    }
    uint32_t ncpu = smp_cpu_count();
    if (ncpu == 0) ncpu = 1;
    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX], val[64];
    uint32_t made = 0;
    for (uint32_t i = 0; i < ncpu; i++) {
        if (g_sysfs_count + 10 >= SYSFS_MAX_NODES) break;
        ksnprintf(dir, sizeof dir, "/devices/system/cpu/cpu%u", i);
        sysfs_add_dir(dir);
        ksnprintf(dir, sizeof dir, "/devices/system/cpu/cpu%u/cpufreq", i);
        sysfs_add_dir(dir);

        /* Every value below is PACKAGE-WIDE, which is why publishing it
         * under each cpu<N> is honest even though we never read a
         * per-core register. */
        ksnprintf(path, sizeof path, "%s/cpuinfo_max_freq", dir);
        ksnprintf(val, sizeof val, "%u\n", fi->max_khz);
        sysfs_add_fixed(path, val);
        ksnprintf(path, sizeof path, "%s/scaling_max_freq", dir);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/cpuinfo_min_freq", dir);
        ksnprintf(val, sizeof val, "%u\n", fi->min_khz);
        sysfs_add_fixed(path, val);
        ksnprintf(path, sizeof path, "%s/scaling_min_freq", dir);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/base_frequency", dir);
        ksnprintf(val, sizeof val, "%u\n", fi->base_khz);
        sysfs_add_fixed(path, val);

        ksnprintf(path, sizeof path, "%s/scaling_driver", dir);
        ksnprintf(val, sizeof val, "%s\n", fi->driver);
        sysfs_add_fixed(path, val);
        made++;
    }
    kprintf("[sysfs] /sys/devices/system/cpu/*/cpufreq: %u cpu(s), "
            "base %u kHz (no scaling_cur_freq/governor -- see cpufreq.c)\n",
            made, fi->base_khz);
}

static void sysfs_add_link(const char *path, const char *target) {
    if (g_sysfs_count >= SYSFS_MAX_NODES) return;
    struct sysfs_node *n = &g_sysfs_nodes[g_sysfs_count++];
    size_t len = strlen(path);
    if (len >= VFS_PATH_MAX) len = VFS_PATH_MAX - 1;
    memcpy(n->path, path, len);
    n->path[len] = '\0';
    n->type = VFS_TYPE_SYMLINK;
    n->generate = 0;
    n->link = target;
}

/* ---- VFS ops ---- */

struct sysfs_file_priv {
    char buf[SYSFS_BUF_SIZE];   /* backing store for generated content   */
    const uint8_t *src;         /* what read() serves: buf, fixed, blob  */
    size_t len;
    size_t pos;
};

static struct sysfs_node *sysfs_find(const char *path) {
    for (int i = 0; i < g_sysfs_count; i++) {
        if (strcmp(g_sysfs_nodes[i].path, path) == 0)
            return &g_sysfs_nodes[i];
    }
    return 0;
}

static int sysfs_open(void *mnt, const char *path, struct vfs_file *out) {
    (void)mnt;
    struct sysfs_node *n = sysfs_find(path);
    if (!n) return VFS_ERR_NOENT;
    if (n->type == VFS_TYPE_DIR) return VFS_ERR_ISDIR;

    struct sysfs_file_priv *priv = kmalloc(sizeof(*priv));
    if (!priv) return VFS_ERR_NOMEM;
    memset(priv, 0, sizeof(*priv));

    if (n->generate) {
        int len = n->generate(priv->buf, SYSFS_BUF_SIZE);
        priv->len = (size_t)(len > 0 ? len : 0);
        priv->src = (const uint8_t *)priv->buf;
    } else if (n->blob) {
        /* No copy: the blob is kernel memory that outlives every handle. */
        priv->src = n->blob;
        priv->len = n->blob_len;
    } else if (n->fixed) {
        /* Served in place rather than copied into the 256-byte scratch
         * buffer, which used to silently truncate anything longer. */
        priv->src = (const uint8_t *)n->fixed;
        priv->len = strlen(n->fixed);
    } else {
        priv->src = (const uint8_t *)priv->buf;
        priv->len = 0;
    }
    priv->pos = 0;
    out->priv = priv;
    out->size = priv->len;
    return VFS_OK;
}

static int sysfs_close(struct vfs_file *f) {
    if (f->priv) kfree(f->priv);
    return VFS_OK;
}

static long sysfs_read(struct vfs_file *f, void *buf, size_t n) {
    struct sysfs_file_priv *priv = f->priv;
    if (!priv || !priv->src) return 0;
    if (priv->pos >= priv->len) return 0;
    size_t avail = priv->len - priv->pos;
    if (n > avail) n = avail;
    memcpy(buf, priv->src + priv->pos, n);
    priv->pos += n;
    f->pos = priv->pos;
    return (long)n;
}

/* Default mode for a node that did not ask for one.
 *
 * DIRECTORIES NEED THE EXECUTE BIT. This returned 0444 for every node,
 * directory or not, which meant a directory nobody could TRAVERSE:
 * vfs_opendir asks vfs_perm_check for READ|EXEC, and vfs_perm_check
 * short-circuits for uid 0 -- so root walked /sys happily and every gate
 * in the tree was green, while the EliteDesk, logged in as `toby`, got
 * VFS_ERR_PERM from the first component. lspci printed "no PCI bus
 * exposed" and exited 1 on a machine whose /sys/bus/pci/devices held all
 * eleven functions. sensors, dmidecode, lsusb and cpupower were all
 * failing the same way for the same reason. */
static uint32_t sysfs_default_mode(const struct sysfs_node *n) {
    return (n->type == VFS_TYPE_DIR) ? 00555u : 00444u;
}

static int sysfs_stat(void *mnt, const char *path, struct vfs_stat *out) {
    (void)mnt;
    struct sysfs_node *n = sysfs_find(path);
    if (!n) return VFS_ERR_NOENT;
    out->type = n->type;
    out->size = 0;
    out->uid = 0;
    out->gid = 0;
    out->mode = (n->mode ? n->mode : sysfs_default_mode(n)) | VFS_MODE_VALID;
    if (n->type == VFS_TYPE_FILE && n->generate) {
        char tmp[SYSFS_BUF_SIZE];
        int len = n->generate(tmp, SYSFS_BUF_SIZE);
        out->size = (size_t)(len > 0 ? len : 0);
    } else if (n->type == VFS_TYPE_FILE && n->blob) {
        out->size = n->blob_len;
    } else if (n->type == VFS_TYPE_FILE && n->fixed) {
        out->size = strlen(n->fixed);
    }
    return VFS_OK;
}

static int sysfs_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    (void)mnt;
    struct sysfs_node *n = sysfs_find(path);
    if (!n && strcmp(path, "/") != 0) return VFS_ERR_NOENT;
    out->index = 0;
    out->priv = (void *)(uintptr_t)(path[0] ? (uintptr_t)path : (uintptr_t)"/");
    return VFS_OK;
}

static int sysfs_closedir(struct vfs_dir *d) {
    (void)d;
    return VFS_OK;
}

static int sysfs_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    const char *parent = "/";
    if (d->priv) parent = (const char *)(uintptr_t)d->priv;

    size_t plen = strlen(parent);
    size_t idx = 0;

    for (int i = 0; i < g_sysfs_count; i++) {
        const char *npath = g_sysfs_nodes[i].path;
        if (strncmp(npath, parent, plen) != 0) continue;
        if (plen > 1 && npath[plen] != '/') continue;

        const char *rest = npath + plen;
        if (plen == 1 && npath[0] == '/') rest = npath + 1;
        else if (rest[0] == '/') rest++;

        if (rest[0] == '\0') continue;

        /* Only direct children (no nested '/' in the rest) */
        const char *slash = rest;
        while (*slash && *slash != '/') slash++;
        if (*slash == '/') continue;

        if (idx == d->index) {
            size_t nlen = (size_t)(slash - rest);
            if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
            memcpy(out->name, rest, nlen);
            out->name[nlen] = '\0';
            out->type = g_sysfs_nodes[i].type;
            out->size = 0;
            out->uid = 0;
            out->gid = 0;
            out->mode = (g_sysfs_nodes[i].mode
                             ? g_sysfs_nodes[i].mode
                             : sysfs_default_mode(&g_sysfs_nodes[i]))
                        | VFS_MODE_VALID;
            d->index++;
            return VFS_OK;
        }
        idx++;
    }
    return VFS_ERR_NOENT;
}

/* Slice 100: readlink for the symlink nodes (libdrm's bus-type probe). */
static int sysfs_readlink(void *mnt, const char *path, char *buf,
                          size_t bufsz) {
    (void)mnt;
    struct sysfs_node *n = sysfs_find(path);
    if (!n) return VFS_ERR_NOENT;
    if (n->type != VFS_TYPE_SYMLINK || !n->link) return VFS_ERR_INVAL;
    size_t len = strlen(n->link);
    if (len >= bufsz) len = bufsz ? bufsz - 1 : 0;
    memcpy(buf, n->link, len);
    if (bufsz) buf[len] = '\0';
    /* VFS_OK, NOT the length: vfs_readlink's callers test `rc != VFS_OK`
     * and measure the buffer themselves (see procfs). Returning the
     * length here made every successful readlink look like an error --
     * which is exactly what the first run reported. */
    return VFS_OK;
}

static const struct vfs_ops sysfs_ops = {
    .open    = sysfs_open,
    .close   = sysfs_close,
    .read    = sysfs_read,
    .write   = 0,
    .create  = 0,
    .unlink  = 0,
    .mkdir   = 0,
    .opendir = sysfs_opendir,
    .closedir= sysfs_closedir,
    .readdir = sysfs_readdir,
    .stat    = sysfs_stat,
    .readlink= sysfs_readlink,          /* slice 100 */
    .chmod   = 0,
    .chown   = 0,
    .umount  = 0,
};

/* Mount sysfs at an extra point (mount(2) -t sysfs). Same singleton
 * argument as procfs_mount_at -- and it also closes a gap that predates
 * this slice: /proc/filesystems has always advertised "nodev sysfs",
 * while mount(2) answered ENODEV for it. A filesystem list that names
 * something the kernel then refuses is the same class of lie as a knob
 * that accepts a limit and enforces nothing. */
int sysfs_mount_at(const char *path) {
    return vfs_mount(path, &sysfs_ops, 0);
}

void sysfs_init(void) {
    sysfs_add_dir("/");
    sysfs_add_dir("/cpu");
    sysfs_add_dir("/mem");
    sysfs_add_dir("/kernel");
    sysfs_add_file("/cpu/count", gen_cpu_count);
    sysfs_add_file("/mem/total", gen_mem_total);
    sysfs_add_file("/mem/free", gen_mem_free);
    sysfs_add_file("/kernel/version", gen_kernel_version);
    sysfs_add_file("/kernel/uptime", gen_kernel_uptime);

    /* B24: Linux-layout nodes real software reads at startup. */
    sysfs_add_dir("/devices");
    sysfs_add_dir("/devices/system");
    sysfs_add_dir("/devices/system/cpu");
    sysfs_add_file("/devices/system/cpu/online",   gen_cpu_online);
    sysfs_add_file("/devices/system/cpu/possible", gen_cpu_online);
    sysfs_add_file("/devices/system/cpu/present",  gen_cpu_online);
    sysfs_add_file("/kernel/ostype",    gen_ostype);
    sysfs_add_file("/kernel/osrelease", gen_osrelease);

    /* Slice 100 (tier 3 Phase 1c): the DRM device tree libdrm validates a
     * render-node fd against. Measured need -- Mesa loaded virtio_gpu_dri.so
     * off our node, then fell back to swrast WITHOUT another ioctl, because
     * gallium's pipe_loader calls drmGetDevice2(fd), which never touches the
     * device: it fstats the fd for the major:minor and then reads SYSFS.
     *
     * drmNodeIsDRM()        stat  .../device/drm
     * drmParseSubsystemType readlink .../device/subsystem -> basename "pci"
     * drmParsePciDeviceInfo .../device/{vendor,device,revision,subsystem_*}
     * drmParsePciBusInfo    .../device/uevent -> PCI_SLOT_NAME=
     *
     * 226 is the Linux DRM major; 128 is our render node's minor (see
     * linux_drm.c). The ids are the virtio-gpu ones the driver logs at
     * probe (1af4:1050). This is a static description of one device --
     * when a second DRM device ever exists, generate it from the PCI scan
     * rather than extending this by hand. */
    sysfs_add_dir("/dev");
    sysfs_add_dir("/dev/char");
    sysfs_add_dir("/dev/char/226:128");
    sysfs_add_dir("/dev/char/226:128/device");
    sysfs_add_dir("/dev/char/226:128/device/drm");
    sysfs_add_link("/dev/char/226:128/device/subsystem", "../../../bus/pci");
    sysfs_add_file("/dev/char/226:128/device/vendor",            gen_pci_vendor);
    sysfs_add_file("/dev/char/226:128/device/device",            gen_pci_device);
    sysfs_add_file("/dev/char/226:128/device/revision",          gen_pci_revision);
    sysfs_add_file("/dev/char/226:128/device/subsystem_vendor",  gen_pci_vendor);
    sysfs_add_file("/dev/char/226:128/device/subsystem_device",  gen_pci_subdev);
    sysfs_add_file("/dev/char/226:128/device/uevent",            gen_drm_uevent);
    sysfs_add_file("/dev/char/226:128/device/config",            gen_pci_config);
    /* The bus directory the subsystem link points at, so a resolver that
     * follows it finds something rather than dangling. */
    sysfs_add_dir("/bus");
    sysfs_add_dir("/bus/pci");
    sysfs_populate_pci();
    sysfs_populate_usb();
    sysfs_populate_dmi();
    sysfs_populate_hwmon();
    /* cpufreq is NOT populated here -- it needs the AP count, and the
     * APs are not up yet. kmain calls sysfs_publish_cpufreq() after
     * smp_start_aps(). */

    int rc = vfs_mount("/sys", &sysfs_ops, 0);
    if (rc == VFS_OK) {
        kprintf("[sysfs] mounted at /sys (%d nodes)\n", g_sysfs_count);
    } else {
        kprintf("[sysfs] mount failed: %s\n", vfs_strerror(rc));
    }
}
