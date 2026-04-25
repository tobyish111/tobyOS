/* fw_cfg.c -- QEMU firmware-config PIO reader.
 *
 * x86 PIO layout (per QEMU spec):
 *   selector register: 0x510 (16-bit, write-only as far as we use it)
 *   data register:     0x511 (8-bit, read; writes are ignored on QEMU
 *                             v2.4+ unless using the DMA interface)
 *   DMA address:       0x514 (we don't use this -- 64-bit big-endian
 *                             physical address of an FWCfgDmaAccess
 *                             descriptor; the legacy byte-stream API
 *                             below is fast enough for sub-KiB blobs)
 *
 * Selector bits (we only ever issue read selects):
 *   0x0000 = signature ("QEMU" if device is present)
 *   0x0001 = revision  (bit 0 = legacy interface, bit 1 = DMA)
 *   0x0019 = file directory (count + array of FWCfgFile entries)
 *   0x0020 .. = per-file selectors assigned by QEMU at boot
 *
 * Endianness gotcha: the file-directory CONTENT is stored big-endian
 * in fw_cfg memory (count and per-file size/select), but everything
 * we read through PIO comes back as raw bytes. We do the bswap by
 * hand below.
 *
 * Boot dependency: must run after pit_init() since we use pit_sleep_ms
 * indirectly via... actually nothing here sleeps. fw_cfg_init() can be
 * called as soon as PIO is usable, which is essentially "always" on
 * x86_64 long mode. */

#include <tobyos/fw_cfg.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define FW_CFG_PORT_SEL    0x510
#define FW_CFG_PORT_DATA   0x511

#define FW_CFG_KEY_SIGNATURE   0x0000
#define FW_CFG_KEY_ID          0x0001
#define FW_CFG_KEY_FILE_DIR    0x0019

/* FWCfgFile (matches QEMU's struct, all big-endian on the wire). */
struct fw_cfg_file_entry {
    uint32_t size_be;       /* big-endian content size */
    uint16_t select_be;     /* big-endian per-file selector */
    uint16_t reserved;
    char     name[56];      /* NUL-terminated ASCII */
} __attribute__((packed));

static bool g_present = false;
static bool g_inited  = false;

static inline void fw_cfg_select(uint16_t sel) {
    outw(FW_CFG_PORT_SEL, sel);
}

static inline uint8_t fw_cfg_read_byte(void) {
    return inb(FW_CFG_PORT_DATA);
}

/* Read `n` bytes from the currently selected fw_cfg item into `out`.
 * The data offset advances automatically inside QEMU. */
static void fw_cfg_read_bytes(void *out, size_t n) {
    uint8_t *p = (uint8_t *)out;
    for (size_t i = 0; i < n; i++) p[i] = fw_cfg_read_byte();
}

/* Skip `n` bytes from the currently selected fw_cfg item. There's no
 * dedicated PIO skip opcode, so we just read and discard. The DMA
 * interface has a real skip op; not worth wiring up for the small
 * directories we care about. */
static void fw_cfg_skip_bytes(size_t n) {
    for (size_t i = 0; i < n; i++) (void)fw_cfg_read_byte();
}

/* Big-endian -> host helpers. fw_cfg directory fields are stored as
 * BE on the wire regardless of host endianness; we always run on
 * little-endian x86_64 so a bswap is the right thing. */
static inline uint32_t be32_to_cpu(uint32_t v) {
    return ((v & 0xFF000000u) >> 24)
         | ((v & 0x00FF0000u) >>  8)
         | ((v & 0x0000FF00u) <<  8)
         | ((v & 0x000000FFu) << 24);
}
static inline uint16_t be16_to_cpu(uint16_t v) {
    return (uint16_t)(((v & 0xFF00u) >> 8) | ((v & 0x00FFu) << 8));
}

bool fw_cfg_init(void) {
    if (g_inited) return g_present;
    g_inited = true;

    /* Sanity-check: read 4 bytes from the signature key and look for
     * "QEMU". On a real PC these ports are unmapped so we'll read
     * 0xFF (or whatever floats on the bus); the strncmp will fail
     * cleanly and we'll report absent. */
    fw_cfg_select(FW_CFG_KEY_SIGNATURE);
    char sig[4];
    fw_cfg_read_bytes(sig, sizeof sig);
    if (sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U') {
        g_present = true;
        kprintf("[fw_cfg] QEMU fw_cfg interface present at 0x%x/0x%x\n",
                (unsigned)FW_CFG_PORT_SEL, (unsigned)FW_CFG_PORT_DATA);
    } else {
        g_present = false;
        kprintf("[fw_cfg] no QEMU fw_cfg interface (sig=%02x%02x%02x%02x) "
                "-- mock data injection unavailable\n",
                (unsigned)(uint8_t)sig[0], (unsigned)(uint8_t)sig[1],
                (unsigned)(uint8_t)sig[2], (unsigned)(uint8_t)sig[3]);
    }
    return g_present;
}

bool fw_cfg_present(void) {
    return g_present;
}

int fw_cfg_read_file(const char *name, void *out, size_t cap) {
    if (!g_present || !name || !out) return -1;

    /* Read directory header: count of files (BE32). */
    fw_cfg_select(FW_CFG_KEY_FILE_DIR);
    uint32_t count_be;
    fw_cfg_read_bytes(&count_be, sizeof count_be);
    uint32_t count = be32_to_cpu(count_be);

    /* Walk entries; entries are 64 B each. Stop after a sane upper
     * bound -- we shouldn't ever see more than a few dozen. */
    if (count > 1024) {
        kprintf("[fw_cfg] file dir count=%u looks bogus -- aborting lookup\n",
                count);
        return -1;
    }

    uint16_t found_sel = 0;
    uint32_t found_size = 0;
    bool     found = false;
    size_t   name_len = 0;
    while (name[name_len]) name_len++;

    for (uint32_t i = 0; i < count; i++) {
        struct fw_cfg_file_entry e;
        fw_cfg_read_bytes(&e, sizeof e);
        if (found) continue;   /* drain remaining entries */
        /* Compare names: e.name is NUL-padded to 56 B. */
        bool match = true;
        for (size_t k = 0; k < name_len + 1; k++) {
            if (k >= 56) { match = false; break; }
            if (e.name[k] != name[k]) { match = false; break; }
        }
        if (match) {
            found      = true;
            found_sel  = be16_to_cpu(e.select_be);
            found_size = be32_to_cpu(e.size_be);
        }
    }

    if (!found) return -1;

    /* Re-select to the per-file slot, then read up to cap bytes. */
    fw_cfg_select(found_sel);
    size_t to_read = found_size < cap ? (size_t)found_size : cap;
    fw_cfg_read_bytes(out, to_read);
    /* Drain the rest so the next select starts at a clean offset.
     * Not strictly required because select-write resets the offset,
     * but cheap insurance. */
    if ((size_t)found_size > to_read) {
        fw_cfg_skip_bytes((size_t)found_size - to_read);
    }
    return (int)to_read;
}
