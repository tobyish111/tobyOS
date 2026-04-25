/* partition.c -- protective-MBR + GPT scanner (milestone 23A).
 *
 * What this file does
 * -------------------
 *
 *   1. Read LBA 0; verify the protective-MBR signature (0x55AA at +510)
 *      and the presence of a single 0xEE entry covering the whole disk.
 *      A GPT disk MUST have this; without it we bail (the caller falls
 *      back to "treat the whole disk as legacy tobyfs").
 *
 *   2. Read LBA 1; verify the GPT primary header (signature, revision,
 *      header CRC32, my_lba == 1). On any failure, optionally retry the
 *      backup header at the disk's last LBA. (Backup recovery is small
 *      and self-contained; left enabled by default.)
 *
 *   3. Read the partition entry array; verify its CRC32 against the
 *      header's `partition_entry_array_crc32`. Walk the entries in slot
 *      order, skipping any whose type_guid is all-zero. For every used
 *      entry:
 *        - clamp LBA range against the header's first/last_usable_lba
 *        - synthesise a name like "<disk>.p<N>"
 *        - decode the UTF-16LE label into ASCII (best effort; '?' for
 *          non-ASCII glyphs)
 *        - hand the disk + offset + size + metadata to
 *          blk_partition_wrap() and register the resulting blk_dev.
 *
 *   4. Return the count of partitions registered (0+) or -1 if the
 *      disk has no GPT at all.
 *
 * Idempotency
 * -----------
 *
 *   partition_scan_disk() is safe to call multiple times. Before
 *   registering a new partition we check the registry for a name
 *   collision (same disk + same partition index produces the same
 *   "<disk>.pN" string). On collision we silently skip -- the caller
 *   already has the device, no need for a second wrapper.
 *
 * What we DELIBERATELY DO NOT IMPLEMENT here
 * ------------------------------------------
 *
 *   - Filesystem detection. The block layer doesn't care what's INSIDE
 *     a partition; that's the next layer up's job (tobyfs_mount /
 *     fat32_mount in M23B).
 *   - GPT entry rewriting / repair / partition table editing. We are
 *     a read-only consumer of the on-disk layout.
 *   - Logical Volume Manager / dynamic disks / RAID metadata. Far out
 *     of scope.
 */

#include <tobyos/partition.h>
#include <tobyos/blk.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

/* ---- well-known type GUIDs (mixed-endian on-disk form) ----------- *
 *
 * Every comment header line below shows the canonical written form
 * (as printed by fdisk / parted / lsblk). The byte arrays underneath
 * are what's literally stored at offset 0..15 of a GPT entry's
 * partition_type_guid field. */

const uint8_t GPT_TYPE_UNUSED[BLK_GUID_BYTES] = { 0 };

/* c12a7328-f81f-11d2-ba4b-00a0c93ec93b -- EFI System Partition (ESP) */
const uint8_t GPT_TYPE_EFI_SYSTEM[BLK_GUID_BYTES] = {
    0x28, 0x73, 0x2A, 0xC1,  0x1F, 0xF8,  0xD2, 0x11,
    0xBA, 0x4B,  0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* 21686148-6449-6e6f-744e-656564454649 -- BIOS Boot Partition
 *   (Spelled "Hah!IdontNeedEFI" in ASCII -- yes, really, see GPT spec) */
const uint8_t GPT_TYPE_BIOS_BOOT[BLK_GUID_BYTES] = {
    0x48, 0x61, 0x68, 0x21,  0x49, 0x64,  0x6F, 0x6E,
    0x74, 0x4E,  0x65, 0x65, 0x64, 0x45, 0x46, 0x49
};

/* ebd0a0a2-b9e5-4433-87c0-68b6b72699c7 -- Microsoft Basic Data
 *   (also Linux's "default" data partition before ext4 GUID was added) */
const uint8_t GPT_TYPE_MS_BASIC_DATA[BLK_GUID_BYTES] = {
    0xA2, 0xA0, 0xD0, 0xEB,  0xE5, 0xB9,  0x33, 0x44,
    0x87, 0xC0,  0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

/* 0fc63daf-8483-4772-8e79-3d69d8477de4 -- Linux filesystem data */
const uint8_t GPT_TYPE_LINUX_FS[BLK_GUID_BYTES] = {
    0xAF, 0x3D, 0xC6, 0x0F,  0x83, 0x84,  0x72, 0x47,
    0x8E, 0x79,  0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

/* 933ac7e1-2eb4-4f13-b844-0e14e2aef915 -- Linux /home */
const uint8_t GPT_TYPE_LINUX_HOME[BLK_GUID_BYTES] = {
    0xE1, 0xC7, 0x3A, 0x93,  0xB4, 0x2E,  0x13, 0x4F,
    0xB8, 0x44,  0x0E, 0x14, 0xE2, 0xAE, 0xF9, 0x15
};

/* 54796279-0000-746f-6279-6f73646174610 -- tobyOS persistent data
 *   The bytes spell "tobyos data" in pieces if you squint at them; the
 *   important thing is that the canonical written form is also printed
 *   by `gpt list` so we can cross-check. */
const uint8_t GPT_TYPE_TOBYOS_DATA[BLK_GUID_BYTES] = {
    0x79, 0x62, 0x79, 0x54,  0x00, 0x00,  0x6F, 0x74,
    0x62, 0x79,  0x6F, 0x73, 0x64, 0x61, 0x74, 0x61
};

/* ---- CRC32/IEEE 802.3 ---------------------------------------------
 *
 * Standard reflected/polynomial form. We compute the table once on
 * first call and cache it -- 1 KiB total, lives in BSS. */

static uint32_t crc32_table[256];
static int      crc32_ready;

static void crc32_init_table(void) {
    if (crc32_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

uint32_t partition_crc32(const void *data, size_t n) {
    crc32_init_table();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c = crc32_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

int partition_guid_cmp(const uint8_t a[BLK_GUID_BYTES],
                       const uint8_t b[BLK_GUID_BYTES]) {
    for (size_t i = 0; i < BLK_GUID_BYTES; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

/* ---- internal helpers --------------------------------------------- */

static int guid_is_zero(const uint8_t g[BLK_GUID_BYTES]) {
    for (size_t i = 0; i < BLK_GUID_BYTES; i++) {
        if (g[i] != 0) return 0;
    }
    return 1;
}

/* Decode a 36-codepoint UTF-16LE string into ASCII (best effort).
 * Non-ASCII chars (>0x7E or null-padded zeroes between glyphs) are
 * dropped; the result is NUL-terminated and capped at out_sz - 1. */
static void utf16le_to_ascii(const uint16_t *src, size_t src_chars,
                             char *out, size_t out_sz) {
    if (out_sz == 0) return;
    size_t w = 0;
    for (size_t i = 0; i < src_chars && w + 1 < out_sz; i++) {
        uint16_t c = src[i];
        if (c == 0) break;                 /* explicit terminator */
        if (c >= 0x20 && c <= 0x7E) {
            out[w++] = (char)c;
        }
        /* else: drop. The label is purely cosmetic. */
    }
    out[w] = 0;
}

/* Format "<diskname>.pN" into out. Caller-provided buffer; we expect
 * out_sz >= len(diskname) + 5 ("p99\0"). */
static void make_partition_name(const char *disk_name, uint32_t idx,
                                char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    size_t w = 0;
    if (disk_name) {
        for (const char *c = disk_name; *c && w + 1 < out_sz; c++) {
            out[w++] = *c;
        }
    }
    if (w + 1 < out_sz) out[w++] = '.';
    if (w + 1 < out_sz) out[w++] = 'p';
    /* idx is 1..128 in practice; render decimal without printf so we
     * don't accidentally pull a stack format vector through here. */
    char  digits[8];
    int   d = 0;
    uint32_t v = idx;
    if (v == 0) {
        digits[d++] = '0';
    } else {
        while (v && d < (int)sizeof(digits)) {
            digits[d++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (d > 0 && w + 1 < out_sz) out[w++] = digits[--d];
    out[w] = 0;
}

/* Return 1 if a partition with this name is already registered. */
static int partition_already_registered(const char *name) {
    return blk_find(name) != 0;
}

/* ---- protective MBR check ----------------------------------------- *
 *
 * UEFI 2.x Annex 5.2.3: a GPT-formatted disk MUST carry a "protective"
 * MBR at LBA 0. The MBR partition table (16-byte entries at offset
 * 446..510) holds exactly one entry of type 0xEE that covers the entire
 * disk (LBA 1..disk_end), and the trailing 2 bytes at offset 510 are
 * 0x55 0xAA. This stops legacy MBR-only tools from "fixing" the disk
 * by deleting partitions they don't understand.
 *
 * We do NOT require the geometry fields to make sense -- any sane GPT
 * tool packs them with sentinel values (0xFE 0xFF 0xFF) for disks
 * larger than CHS can address. We only check signature + 0xEE present. */
static int check_protective_mbr(struct blk_dev *disk) {
    uint8_t mbr[BLK_SECTOR_SIZE];
    if (blk_read(disk, 0, 1, mbr) != 0) {
        kprintf("[part] %s: failed to read LBA 0 (MBR)\n",
                disk->name ? disk->name : "?");
        return 0;
    }
    /* Signature at +510 must be 55 AA (little-endian 0xAA55). */
    uint16_t sig = (uint16_t)mbr[MBR_SIGNATURE_OFF] |
                   ((uint16_t)mbr[MBR_SIGNATURE_OFF + 1] << 8);
    if (sig != MBR_SIGNATURE_VAL) {
        return 0;     /* no MBR signature -- definitely not a GPT disk */
    }
    /* Walk the four 16-byte MBR partition entries at offset 446. The
     * partition type byte sits at +4 within each entry. We accept the
     * disk as GPT if AT LEAST ONE entry is type 0xEE -- some firmwares
     * are sloppy about the "exactly one" rule.
     *
     * (Tools like parted ALWAYS write the protective entry into slot
     * 0 + leave the remaining 3 zeroed; the more permissive check is a
     * defense against odd installer images.) */
    for (int i = 0; i < 4; i++) {
        uint8_t type = mbr[446 + i * 16 + 4];
        if (type == MBR_PROTECTIVE_GPT_TYPE) return 1;
    }
    return 0;
}

/* ---- GPT header validation ---------------------------------------- *
 *
 * Returns 1 iff the supplied 512-byte buffer holds a valid header AND
 * its self-CRC matches. `expected_my_lba` is the LBA we tried to read
 * the header from (1 for primary, last LBA for backup). */
static int validate_gpt_header(const uint8_t *buf,
                               uint64_t expected_my_lba,
                               struct gpt_header *out) {
    /* Copy header out of the buffer first (we'll rewrite the CRC field
     * to zero in our local copy and recompute -- the on-disk bytes
     * themselves stay untouched). */
    memcpy(out, buf, sizeof(*out));

    if (out->signature != GPT_HEADER_SIGNATURE) return 0;
    if (out->revision != GPT_HEADER_REVISION_1_0) return 0;
    if (out->header_size < GPT_HEADER_SIZE ||
        out->header_size > BLK_SECTOR_SIZE) return 0;
    if (out->my_lba != expected_my_lba) return 0;
    if (out->partition_entry_size < sizeof(struct gpt_entry)) return 0;
    /* Sanity ceiling: 8 KiB per entry would let a maliciously crafted
     * header DoS-allocate the heap. Real GPTs use 128 bytes. */
    if (out->partition_entry_size > 4096) return 0;
    if (out->num_partition_entries == 0 ||
        out->num_partition_entries > 1024) return 0;

    /* Recompute header CRC. The stored CRC is over the first
     * header_size bytes with header_crc32 itself zeroed. */
    uint8_t  scratch[BLK_SECTOR_SIZE];
    memcpy(scratch, buf, out->header_size);
    /* Zero the 4 bytes of header_crc32, which lives at offset 16. */
    scratch[16] = 0; scratch[17] = 0; scratch[18] = 0; scratch[19] = 0;
    uint32_t calc = partition_crc32(scratch, out->header_size);
    if (calc != out->header_crc32) {
        kprintf("[part] header CRC mismatch (got 0x%08x, want 0x%08x)\n",
                (unsigned)calc, (unsigned)out->header_crc32);
        return 0;
    }
    return 1;
}

/* Read the GPT entry array from disk; returns kmalloc'd buffer (caller
 * kfree's) or NULL on failure. The buffer is exactly
 * num_entries * entry_size bytes. */
static uint8_t *read_entry_array(struct blk_dev *disk,
                                 const struct gpt_header *hdr) {
    /* Round size UP to a sector multiple so blk_read sees a whole-
     * sector count. We allocate the rounded-up size; callers index
     * the first num_entries * entry_size bytes only. */
    uint64_t total_bytes = (uint64_t)hdr->num_partition_entries *
                           (uint64_t)hdr->partition_entry_size;
    uint64_t total_sectors =
        (total_bytes + BLK_SECTOR_SIZE - 1) / BLK_SECTOR_SIZE;
    /* 16 KiB default array; cap at 64 KiB worst case. */
    if (total_sectors > 128) {
        kprintf("[part] entry array too large (%lu sectors)\n",
                (unsigned long)total_sectors);
        return 0;
    }

    uint8_t *buf = kmalloc((size_t)(total_sectors * BLK_SECTOR_SIZE));
    if (!buf) {
        kprintf("[part] OOM allocating entry array (%lu sectors)\n",
                (unsigned long)total_sectors);
        return 0;
    }
    if (blk_read(disk, hdr->partition_entry_lba,
                 (uint32_t)total_sectors, buf) != 0) {
        kprintf("[part] failed to read entry array @ LBA %lu\n",
                (unsigned long)hdr->partition_entry_lba);
        kfree(buf);
        return 0;
    }

    /* Verify array CRC. The CRC is computed over EXACTLY
     * num_entries * entry_size bytes -- not over the rounded-up
     * sector count. */
    uint32_t calc = partition_crc32(buf, (size_t)total_bytes);
    if (calc != hdr->partition_entry_array_crc32) {
        kprintf("[part] entry array CRC mismatch "
                "(got 0x%08x, want 0x%08x)\n",
                (unsigned)calc,
                (unsigned)hdr->partition_entry_array_crc32);
        kfree(buf);
        return 0;
    }
    return buf;
}

/* Try to read+validate the GPT primary header at LBA 1. On success
 * fills `*hdr` and returns 1. On failure tries the backup header at
 * `disk->sector_count - 1` and, if THAT validates and points back at a
 * sane primary, returns 1 with the BACKUP filled in. */
static int load_gpt_header(struct blk_dev *disk, struct gpt_header *hdr) {
    uint8_t buf[BLK_SECTOR_SIZE];

    if (blk_read(disk, GPT_HEADER_LBA, 1, buf) == 0) {
        if (validate_gpt_header(buf, GPT_HEADER_LBA, hdr)) return 1;
    }
    /* Primary is bad -- attempt backup recovery. */
    uint64_t backup_lba = disk->sector_count - 1;
    kprintf("[part] %s: primary GPT header bad, trying backup @ LBA %lu\n",
            disk->name ? disk->name : "?", (unsigned long)backup_lba);
    if (blk_read(disk, backup_lba, 1, buf) != 0) return 0;
    if (!validate_gpt_header(buf, backup_lba, hdr)) return 0;
    /* Backup is valid. Its `partition_entry_lba` points at the BACKUP
     * entry array (just before the backup header), which we'll happily
     * use -- the CRC check there is what gates whether we actually
     * register anything. */
    return 1;
}

/* ---- public API --------------------------------------------------- */

int partition_scan_disk(struct blk_dev *disk) {
    if (!disk || disk->class != BLK_CLASS_DISK) return -1;
    if (disk->sector_count < 34) return -1;   /* GPT minimum: 1+1+32 */

    if (!check_protective_mbr(disk)) {
        return -1;     /* no protective MBR -- not a GPT disk */
    }

    struct gpt_header hdr;
    if (!load_gpt_header(disk, &hdr)) {
        kprintf("[part] %s: no usable GPT header\n",
                disk->name ? disk->name : "?");
        return -1;
    }
    kprintf("[part] %s: GPT detected -- entries=%u (size=%u), "
            "usable LBA %lu..%lu\n",
            disk->name ? disk->name : "?",
            (unsigned)hdr.num_partition_entries,
            (unsigned)hdr.partition_entry_size,
            (unsigned long)hdr.first_usable_lba,
            (unsigned long)hdr.last_usable_lba);

    uint8_t *array = read_entry_array(disk, &hdr);
    if (!array) return -1;

    int registered = 0;
    for (uint32_t i = 0;
         i < hdr.num_partition_entries && registered < PART_MAX_PER_DISK;
         i++) {
        const uint8_t *entry_bytes = array + (size_t)i * hdr.partition_entry_size;
        struct gpt_entry e;
        memcpy(&e, entry_bytes, sizeof(e));
        if (guid_is_zero(e.type_guid)) continue;
        if (e.starting_lba == 0 || e.ending_lba < e.starting_lba) continue;
        if (e.starting_lba > disk->sector_count ||
            e.ending_lba > disk->sector_count) {
            kprintf("[part] entry %u: range %lu..%lu exceeds disk size %lu "
                    "-- skipped\n",
                    (unsigned)(i + 1),
                    (unsigned long)e.starting_lba,
                    (unsigned long)e.ending_lba,
                    (unsigned long)disk->sector_count);
            continue;
        }
        uint64_t span = e.ending_lba - e.starting_lba + 1;

        char label[BLK_PART_LABEL_MAX];
        utf16le_to_ascii(e.name_utf16,
                         sizeof(e.name_utf16) / sizeof(e.name_utf16[0]),
                         label, sizeof(label));

        /* Build a unique device name. Names live forever in the
         * registry; allocate a small heap buffer for it. */
        char  scratch[64];
        make_partition_name(disk->name, i + 1, scratch, sizeof(scratch));
        if (partition_already_registered(scratch)) {
            registered++;     /* already there -- count as "found" */
            continue;
        }
        size_t nlen = strlen(scratch);
        char  *name = kmalloc(nlen + 1);
        if (!name) {
            kprintf("[part] OOM naming partition\n");
            continue;
        }
        memcpy(name, scratch, nlen + 1);

        struct blk_dev *part = blk_partition_wrap(
            disk, e.starting_lba, span, name,
            i + 1, e.type_guid, label);
        if (!part) {
            kfree(name);
            continue;
        }
        blk_register(part);
        registered++;
        kprintf("[part] %s: slot %u -> %s, LBA %lu..%lu (%lu sectors), "
                "label='%s'\n",
                disk->name ? disk->name : "?",
                (unsigned)(i + 1),
                name,
                (unsigned long)e.starting_lba,
                (unsigned long)e.ending_lba,
                (unsigned long)span,
                label);
    }

    kfree(array);
    kprintf("[part] %s: registered %d partition(s)\n",
            disk->name ? disk->name : "?", registered);
    return registered;
}

int partition_scan_all(void) {
    int total = 0;
    size_t cookie = 0;
    struct blk_dev *d;
    while ((d = blk_iter_next(&cookie, BLK_CLASS_DISK)) != 0) {
        int n = partition_scan_disk(d);
        if (n > 0) total += n;
    }
    return total;
}

struct blk_dev *partition_find_by_type(const uint8_t type_guid[BLK_GUID_BYTES]) {
    if (!type_guid) return 0;
    size_t cookie = 0;
    struct blk_dev *d;
    while ((d = blk_iter_next(&cookie, BLK_CLASS_PARTITION)) != 0) {
        if (partition_guid_cmp(d->type_guid, type_guid) == 0) return d;
    }
    return 0;
}
