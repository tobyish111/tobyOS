/* partition.h -- GPT partition discovery (milestone 23A).
 *
 * Sits between the block layer (struct blk_dev) and the filesystem
 * layer (tobyfs / FAT32 / ext4):
 *
 *     PCI driver probe   ---> blk_register(disk, class=DISK)
 *                                            |
 *                              partition_scan_disk(disk)
 *                                            |
 *                                            v
 *                                  for each non-empty GPT entry:
 *                                    blk_partition_wrap(disk, lba, n, ...)
 *                                    blk_register(part, class=PARTITION)
 *                                            |
 *                                            v
 *                              filesystem mount can find a tobyOS-type
 *                              partition without knowing the disk layout
 *
 * GPT layout per UEFI 2.x spec
 * ----------------------------
 *
 *   LBA 0       : Protective MBR (one entry, type 0xEE, spans whole disk)
 *                 -- presence of a 0xEE partition is the canonical
 *                    "GPT, do not touch" signal for legacy MBR tools
 *   LBA 1       : Primary GPT Header  (96 bytes used, 512-byte block)
 *                 signature = "EFI PART", revision 1.0
 *   LBA 2..N    : Primary GPT Entry Array (default 128 entries x 128
 *                 bytes = 16 KiB = 32 sectors)
 *   ...
 *   LBA -33..-2 : Backup GPT Entry Array
 *   LBA -1      : Backup GPT Header
 *
 * Type GUIDs are stored on disk in the mixed-endian "Microsoft GUID"
 * form: bytes 0-3 little-endian uint32, 4-5 LE uint16, 6-7 LE uint16,
 * 8-15 byte-wise. We match against pre-baked constant byte arrays so
 * we don't have to do per-comparison swapping.
 */

#ifndef TOBYOS_PARTITION_H
#define TOBYOS_PARTITION_H

#include <tobyos/types.h>
#include <tobyos/blk.h>

#define GPT_HEADER_LBA            1
#define GPT_HEADER_SIGNATURE      0x5452415020494645ULL  /* "EFI PART" */
#define GPT_HEADER_REVISION_1_0   0x00010000u
#define GPT_HEADER_SIZE           92u
#define GPT_DEFAULT_ENTRY_COUNT   128u
#define GPT_DEFAULT_ENTRY_SIZE    128u
#define GPT_DEFAULT_ENTRY_LBA     2u

#define MBR_SIGNATURE_OFF         510
#define MBR_SIGNATURE_VAL         0xAA55u
#define MBR_PROTECTIVE_GPT_TYPE   0xEEu

/* Maximum partitions we'll surface from one disk. Real GPTs are
 * usually capped at 128 entries; we register fewer because the
 * registry is small (BLK_MAX_DEVICES=16) and we want to leave room
 * for multiple disks. */
#define PART_MAX_PER_DISK         8

#pragma pack(push, 1)

struct gpt_header {
    uint64_t signature;                /* must equal GPT_HEADER_SIGNATURE */
    uint32_t revision;                 /* 0x00010000 for 1.0 */
    uint32_t header_size;              /* 92 typically */
    uint32_t header_crc32;             /* CRC32 of the first header_size
                                          bytes with this field zeroed */
    uint32_t reserved;                 /* must be zero */
    uint64_t my_lba;                   /* this header's LBA (1 for primary) */
    uint64_t alternate_lba;            /* the OTHER header's LBA */
    uint64_t first_usable_lba;         /* start of partition area */
    uint64_t last_usable_lba;          /* end (inclusive) of partition area */
    uint8_t  disk_guid[16];            /* this disk's unique GUID */
    uint64_t partition_entry_lba;      /* where the entry array starts */
    uint32_t num_partition_entries;    /* slot count */
    uint32_t partition_entry_size;     /* per-entry byte length */
    uint32_t partition_entry_array_crc32;
    /* Trailing bytes up to the end of the LBA are reserved + zero. */
};

struct gpt_entry {
    uint8_t  type_guid[16];            /* 0..0 = unused slot */
    uint8_t  unique_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;               /* inclusive */
    uint64_t attributes;
    uint16_t name_utf16[36];           /* UTF-16LE, NUL-terminated */
};

#pragma pack(pop)

_Static_assert(sizeof(struct gpt_header) == 92, "gpt_header layout");
_Static_assert(sizeof(struct gpt_entry) == 128, "gpt_entry layout");

/* Type GUIDs we recognise. The byte arrays match what's stored on
 * disk -- mixed-endian per RFC 4122 section 4.1.2:
 *   bytes 0-3   : little-endian time_low
 *   bytes 4-5   : little-endian time_mid
 *   bytes 6-7   : little-endian time_hi_and_version
 *   bytes 8-9   : clock_seq (byte-wise)
 *   bytes 10-15 : node (byte-wise)
 *
 * To convert a written-form GUID like c12a7328-f81f-11d2-ba4b-00a0c93ec93b:
 *   28, 73, 2a, c1,   1f, f8,   d2, 11,   ba, 4b,   00, a0, c9, 3e, c9, 3b
 */
extern const uint8_t GPT_TYPE_UNUSED       [BLK_GUID_BYTES];  /* all zeros */
extern const uint8_t GPT_TYPE_EFI_SYSTEM   [BLK_GUID_BYTES];  /* c12a7328-... */
extern const uint8_t GPT_TYPE_BIOS_BOOT    [BLK_GUID_BYTES];  /* 21686148-... */
extern const uint8_t GPT_TYPE_MS_BASIC_DATA[BLK_GUID_BYTES];  /* ebd0a0a2-... */
extern const uint8_t GPT_TYPE_LINUX_FS     [BLK_GUID_BYTES];  /* 0fc63daf-... */
extern const uint8_t GPT_TYPE_LINUX_HOME   [BLK_GUID_BYTES];  /* 933ac7e1-... */
/* tobyOS-specific type GUID for our native data partitions. Recognising
 * this lets the boot path say "definitely mount THIS one as tobyfs"
 * without guessing. Generated once and frozen here:
 *   ToBy0000-7065-7263-6973-74656e74000     <-- "persistent" in ASCII
 *   bytes:  00 00 79 54 b0 65 65 72 63 69 73 74 65 6e 74 00
 *           ^^ NOTE: pre-baked into mixed-endian form below. */
extern const uint8_t GPT_TYPE_TOBYOS_DATA  [BLK_GUID_BYTES];

/* CRC32/IEEE 802.3 (polynomial 0xEDB88320, init 0xFFFFFFFF, reflected
 * input/output, final XOR 0xFFFFFFFF). Used for both header + entry
 * array validation. */
uint32_t partition_crc32(const void *data, size_t n);

/* Compare two GUID byte arrays. Returns 0 on equality. */
int partition_guid_cmp(const uint8_t a[BLK_GUID_BYTES],
                       const uint8_t b[BLK_GUID_BYTES]);

/* Scan `disk` for a GPT and register every non-empty entry as a
 * BLK_CLASS_PARTITION device.
 *
 * Returns:
 *    >= 0 : number of partitions discovered + registered
 *    -1   : disk is missing / I/O error / not a GPT (legacy disk
 *           presumably -- caller falls back to old behavior)
 *
 * Idempotent: skips re-registering partitions whose name is already
 * in the registry. Non-fatal failures emit a warning to serial and
 * continue scanning the next entry. */
int partition_scan_disk(struct blk_dev *disk);

/* Scan every BLK_CLASS_DISK currently in the registry. Returns the
 * total number of partitions registered across all disks. Safe to call
 * multiple times -- already-registered partitions are skipped. */
int partition_scan_all(void);

/* Convenience: return the FIRST registered partition whose type_guid
 * matches `type_guid` (16 bytes, mixed-endian on-disk form). NULL if
 * none. Useful for the boot path:
 *
 *     part = partition_find_by_type(GPT_TYPE_TOBYOS_DATA);
 *     if (part) tobyfs_mount("/data", part);
 */
struct blk_dev *partition_find_by_type(const uint8_t type_guid[BLK_GUID_BYTES]);

#endif /* TOBYOS_PARTITION_H */
