/* smbios.c -- find and validate the firmware's SMBIOS/DMI table.
 *
 * Three ways in, tried in this order:
 *   1. Limine's 64-bit "_SM3_" entry point   (source "limine-64")
 *   2. Limine's 32-bit "_SM_"  entry point   (source "limine-32")
 *   3. a scan of the BIOS F-segment          (source "f-scan")
 *
 * The scan exists because it is the only route that does not depend on
 * the bootloader request being right, and it is what Linux's
 * dmi_scan_machine() falls back to. Each path logs WHICH one produced the
 * table, so "no SMBIOS" and "we asked wrong" cannot be confused -- with
 * only one route and a bad request id, both look identical.
 *
 * Everything here validates before it trusts: anchor string, entry point
 * length, and the byte checksum (the sum of the entry point's bytes must
 * be zero). A machine with no SMBIOS leaves `present == false` and every
 * consumer publishes NOTHING rather than a plausible-looking default.
 */

#include <tobyos/smbios.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>   /* PAGE_SIZE */
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define SMBIOS_ID_STRLEN 64

static struct smbios_info g_info;
static char g_ids[SMBIOS_ID_MAX][SMBIOS_ID_STRLEN];

/* Map `len` bytes of physical memory into the HHDM and return the
 * kernel-virtual address, or NULL. */
static const uint8_t *map_phys(uint64_t phys, size_t len) {
    if (!phys || !len) return NULL;
    uint64_t page  = phys & ~((uint64_t)PAGE_SIZE - 1);
    size_t   span  = (size_t)(phys - page) + len;
    if (!vmm_hhdm_ensure_mapped(page, span, VMM_PRESENT | VMM_NX)) return NULL;
    return (const uint8_t *)(phys + vmm_hhdm_offset());
}

/* Limine hands back a pointer that may be physical or already inside the
 * HHDM mirror, exactly like the RSDP response. Normalise to something
 * dereferenceable, mapping on the way if needed. */
static const uint8_t *normalise(void *raw, size_t len) {
    if (!raw) return NULL;
    uint64_t addr = (uint64_t)raw;
    uint64_t hhdm = vmm_hhdm_offset();

    if (addr < hhdm) return map_phys(addr, len);
    if (vmm_translate(addr) == 0) {
        uint64_t phys = addr - hhdm;
        uint64_t page = phys & ~((uint64_t)PAGE_SIZE - 1);
        if (!vmm_hhdm_ensure_mapped(page, (size_t)(phys - page) + len,
                                    VMM_PRESENT | VMM_NX)) {
            return NULL;
        }
    }
    return (const uint8_t *)addr;
}

/* SMBIOS spec: the entry point is valid when its bytes sum to zero. */
static bool checksum_ok(const uint8_t *p, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* "_SM3_" -- SMBIOS 3.0. Carries a 64-bit table address and a MAXIMUM
 * size rather than an exact one; the table ends at its type-127
 * End-of-Table structure. */
static bool try_entry64(const uint8_t *e, const char *source) {
    if (!e) return false;
    if (memcmp(e, "_SM3_", 5) != 0) return false;
    uint8_t len = e[0x06];
    if (len < 0x18) return false;
    if (!checksum_ok(e, len)) {
        kprintf("[smbios] %s: _SM3_ checksum bad -- ignoring\n", source);
        return false;
    }
    uint32_t max_size = rd32(e + 0x0C);
    uint64_t addr     = rd64(e + 0x10);
    const uint8_t *tbl = map_phys(addr, max_size);
    if (!tbl) {
        kprintf("[smbios] %s: cannot map table at %p (%u bytes)\n",
                source, (void *)addr, max_size);
        return false;
    }
    g_info.present   = true;
    g_info.entry     = e;
    g_info.entry_len = len;
    g_info.table     = tbl;
    g_info.table_len = max_size;
    g_info.count     = 0;              /* 3.x does not report a count */
    g_info.major     = e[0x07];
    g_info.minor     = e[0x08];
    g_info.is_64     = true;
    g_info.source    = source;
    return true;
}

/* "_SM_" -- SMBIOS 2.1. The 32-bit table address and the structure count
 * live behind a second, nested "_DMI_" anchor with its own checksum. */
static bool try_entry32(const uint8_t *e, const char *source) {
    if (!e) return false;
    if (memcmp(e, "_SM_", 4) != 0) return false;
    uint8_t len = e[0x05];
    if (len < 0x1F) return false;
    if (!checksum_ok(e, len)) {
        kprintf("[smbios] %s: _SM_ checksum bad -- ignoring\n", source);
        return false;
    }
    if (memcmp(e + 0x10, "_DMI_", 5) != 0) return false;
    if (!checksum_ok(e + 0x10, 0x0F)) {
        kprintf("[smbios] %s: _DMI_ checksum bad -- ignoring\n", source);
        return false;
    }
    uint16_t tlen = rd16(e + 0x16);
    uint32_t addr = rd32(e + 0x18);
    const uint8_t *tbl = map_phys(addr, tlen);
    if (!tbl) {
        kprintf("[smbios] %s: cannot map table at %p (%u bytes)\n",
                source, (void *)(uint64_t)addr, tlen);
        return false;
    }
    g_info.present   = true;
    g_info.entry     = e;
    g_info.entry_len = len;
    g_info.table     = tbl;
    g_info.table_len = tlen;
    g_info.count     = rd16(e + 0x1C);
    g_info.major     = e[0x06];
    g_info.minor     = e[0x07];
    g_info.is_64     = false;
    g_info.source    = source;
    return true;
}

/* Legacy route: the entry point is 16-byte aligned somewhere in
 * 0xF0000..0xFFFFF. UEFI machines have nothing here, which is not an
 * error -- it is why the Limine request exists. */
static bool scan_f_segment(void) {
    const uint8_t *base = map_phys(0xF0000, 0x10000);
    if (!base) return false;
    for (size_t off = 0; off + 0x1F <= 0x10000; off += 16) {
        if (try_entry64(base + off, "f-scan")) return true;
        if (try_entry32(base + off, "f-scan")) return true;
    }
    return false;
}

/* ---- /sys/class/dmi/id string extraction ------------------------------
 *
 * A structure is a fixed "formatted area" (whose length is in the header)
 * followed by its string set: NUL-terminated strings back to back, ended
 * by an empty one. A byte in the formatted area that names a string holds
 * a 1-based INDEX into that set, and 0 means "not specified" -- which we
 * must report as absent, not as an empty string. */
static const char *struct_string(const uint8_t *s, size_t avail, uint8_t idx) {
    if (idx == 0) return NULL;
    uint8_t flen = s[1];
    if (flen < 4 || flen > avail) return NULL;
    const char *p   = (const char *)(s + flen);
    const char *end = (const char *)(s + avail);
    for (uint8_t i = 1; p < end; i++) {
        /* Bounded strlen -- klibc has no strnlen, and running off the end
         * of the table is exactly the failure this walk must not have. */
        size_t l = 0;
        while (p + l < end && p[l]) l++;
        if (l == 0) return NULL;               /* end of the string set */
        if (p + l >= end) return NULL;         /* unterminated: refuse it */
        if (i == idx) return p;
        p += l + 1;
    }
    return NULL;
}

static void save_id(int which, const char *s) {
    if (!s || which < 0 || which >= SMBIOS_ID_MAX) return;
    /* Firmware pads with blanks and ships placeholders; a value that is
     * only whitespace is not information. */
    while (*s == ' ') s++;
    if (!*s) return;
    size_t i = 0;
    for (; i + 1 < SMBIOS_ID_STRLEN && s[i]; i++) g_ids[which][i] = s[i];
    while (i > 0 && g_ids[which][i - 1] == ' ') i--;
    g_ids[which][i] = '\0';
}

/* Walk the table once, pulling the identity strings Linux publishes at
 * /sys/class/dmi/id. Types 0 (BIOS), 1 (system), 2 (baseboard) and
 * 3 (chassis) cover all of them. */
static void extract_ids(void) {
    const uint8_t *p   = g_info.table;
    const uint8_t *end = p + g_info.table_len;

    while (p + 4 <= end) {
        uint8_t type = p[0];
        uint8_t flen = p[1];
        if (flen < 4 || p + flen > end) break;

        /* Find this structure's end: past the formatted area, skip the
         * string set to its terminating empty string. */
        const uint8_t *q = p + flen;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) q++;
        size_t total = (size_t)(q + 2 - p);
        if (q + 1 >= end) total = (size_t)(end - p);

        switch (type) {
            case 0:  /* BIOS Information */
                save_id(SMBIOS_ID_BIOS_VENDOR,     struct_string(p, total, p[4]));
                save_id(SMBIOS_ID_BIOS_VERSION,    struct_string(p, total, p[5]));
                if (flen > 8)
                    save_id(SMBIOS_ID_BIOS_DATE,   struct_string(p, total, p[8]));
                break;
            case 1:  /* System Information */
                save_id(SMBIOS_ID_SYS_VENDOR,      struct_string(p, total, p[4]));
                save_id(SMBIOS_ID_PRODUCT_NAME,    struct_string(p, total, p[5]));
                save_id(SMBIOS_ID_PRODUCT_VERSION, struct_string(p, total, p[6]));
                save_id(SMBIOS_ID_PRODUCT_SERIAL,  struct_string(p, total, p[7]));
                break;
            case 2:  /* Baseboard */
                save_id(SMBIOS_ID_BOARD_VENDOR,    struct_string(p, total, p[4]));
                save_id(SMBIOS_ID_BOARD_NAME,      struct_string(p, total, p[5]));
                save_id(SMBIOS_ID_BOARD_VERSION,   struct_string(p, total, p[6]));
                save_id(SMBIOS_ID_BOARD_SERIAL,    struct_string(p, total, p[7]));
                break;
            case 3:  /* Chassis */
                save_id(SMBIOS_ID_CHASSIS_VENDOR,  struct_string(p, total, p[4]));
                break;
            case 127: /* End-of-Table */
                return;
            default: break;
        }
        p += total;
    }
}

const char *smbios_id_string(int which) {
    if (!g_info.present || which < 0 || which >= SMBIOS_ID_MAX) return NULL;
    return g_ids[which][0] ? g_ids[which] : NULL;
}

const struct smbios_info *smbios_get(void) { return &g_info; }

void smbios_init(void *entry32, void *entry64) {
    memset(&g_info, 0, sizeof(g_info));
    memset(g_ids, 0, sizeof(g_ids));

    const uint8_t *e64 = normalise(entry64, 0x18);
    const uint8_t *e32 = normalise(entry32, 0x1F);

    if (!try_entry64(e64, "limine-64") &&
        !try_entry32(e32, "limine-32") &&
        !scan_f_segment()) {
        kprintf("[smbios] no SMBIOS entry point (limine32=%p limine64=%p, "
                "F-segment scan found none) -- DMI unavailable\n",
                entry32, entry64);
        return;
    }

    /* SMBIOS 3.x reports only a maximum size, so trim to the real end of
     * table. Publishing the padding would hand dmidecode a tail of zero
     * bytes to misparse -- and would overstate the file's size. */
    if (g_info.is_64) {
        const uint8_t *p   = g_info.table;
        const uint8_t *end = p + g_info.table_len;
        while (p + 4 <= end) {
            uint8_t flen = p[1];
            if (flen < 4 || p + flen > end) break;
            const uint8_t *q = p + flen;
            while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) q++;
            if (q + 1 >= end) break;
            const uint8_t *next = q + 2;
            if (p[0] == 127) { g_info.table_len = (size_t)(next - g_info.table);
                               break; }
            p = next;
        }
    }

    extract_ids();

    /* ONE kprintf. The serial logger stamps "[N ms]" at format
     * conversions, so a line assembled from several calls comes out as
     * "table 446 bytes[10 ms] , 10 structures" -- which any whole-line
     * grep then misses. Build the variable tail first, emit once. */
    char tail[48];
    if (g_info.count) ksnprintf(tail, sizeof tail, ", %u structures",
                                g_info.count);
    else              tail[0] = '\0';
    kprintf("[smbios] %s: SMBIOS %u.%u, table %lu bytes%s\n",
            g_info.source, g_info.major, g_info.minor,
            (unsigned long)g_info.table_len, tail);
    const char *v = smbios_id_string(SMBIOS_ID_SYS_VENDOR);
    const char *n = smbios_id_string(SMBIOS_ID_PRODUCT_NAME);
    const char *b = smbios_id_string(SMBIOS_ID_BOARD_NAME);
    kprintf("[smbios] system: %s %s / board %s\n",
            v ? v : "(unreported)", n ? n : "(unreported)",
            b ? b : "(unreported)");
}
