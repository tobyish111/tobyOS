/* mkdisk_gpt.c -- host tool to build a GPT-formatted disk image
 * with one or more pre-formatted partitions (milestone 23A + 23B).
 *
 * Build (MSYS2/UCRT64 or any *nix):
 *     gcc -O2 -Wall -Wextra -o build/mkdisk_gpt tools/mkdisk_gpt.c
 *
 * Use:
 *     build/mkdisk_gpt build/disk_gpt.img
 *
 * What this produces
 * ------------------
 *
 *   A 48 MiB raw image with the following layout (LBA = 512 bytes):
 *
 *     LBA 0          Protective MBR  (one 0xEE entry, sig 0x55AA)
 *     LBA 1          GPT Primary Header (rev 1.0, sig "EFI PART")
 *     LBA 2..33      GPT Primary Entry Array (128 entries x 128 bytes)
 *     LBA 34..2047   "BIOS Boot" partition (slot 1) -- 1 MiB, all zeros
 *     LBA 2048..10239 tobyOS-data partition (slot 2) -- 4 MiB tobyfs
 *     LBA 10240..end-34 FAT32 partition (slot 3) -- ~43 MiB, formatted
 *                       with sample files (/HELLO.TXT, /BIN/README.MD)
 *     ...
 *     last 33 LBAs   GPT Backup (entry array + backup header)
 *
 *   Slot 2's tobyfs is fully formatted in-place using the same layout
 *   that tools/mkfs_tobyfs.c writes (block 0 = superblock, etc.). The
 *   kernel mounts it as /data via partition_find_by_type
 *   (GPT_TYPE_TOBYOS_DATA).
 *
 *   Slot 3's FAT32 (Microsoft Basic Data type GUID) is fully bootable
 *   by any FAT32 implementation: protective MBR, BPB+EBPB, FSInfo,
 *   backup boot sector, two synced FATs, and a root cluster with two
 *   sample files visible by readdir/cat. The kernel mounts it at
 *   /fat at boot; the live shell can also re-mount via
 *   `mountfs /mnt ide0:master.p3` or the auto-detect fast path.
 *
 *   Slot 1 exists purely so the operator can SEE multi-partition
 *   discovery in `blkdump`. It is not formatted with any filesystem.
 *
 * GPT spec compliance
 * -------------------
 *
 *   - protective MBR with the single 0xEE entry covering 1..disk_end
 *     (clamped to 0xFFFFFFFF if disk is bigger than 32 bits of LBAs)
 *   - primary header at LBA 1, backup at last LBA
 *   - both header CRC32s are computed correctly (header_crc32 zeroed
 *     before checksum, then patched in)
 *   - entry array CRC32 covers exactly num_entries * entry_size bytes
 *   - my_lba / alternate_lba pair correctly cross-link the two
 *     headers
 *   - first_usable_lba / last_usable_lba carve out the safe region
 *     between the two metadata blocks
 *   - per-entry name field is UTF-16LE, NUL-padded
 *
 * Verification
 * ------------
 *
 *   On Linux: `parted build/disk_gpt.img unit s print` should show
 *   three partitions.
 *
 *   In tobyOS: boot with `make run-gpt` and then run `blkdump -v`
 *   in the shell -- you should see ide0:master + .p1, .p2, .p3 with
 *   GUIDs and labels.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

/* ---- knobs ----- */

#define SECT_BYTES         512u
#define IMG_BYTES          (48u * 1024u * 1024u)        /* 48 MiB */
#define IMG_LBA            (IMG_BYTES / SECT_BYTES)     /* 98304 sectors */
#define ENTRY_COUNT        128u
#define ENTRY_BYTES        128u
#define ARRAY_BYTES        (ENTRY_COUNT * ENTRY_BYTES)  /* 16 KiB */
#define ARRAY_SECTORS      (ARRAY_BYTES / SECT_BYTES)   /* 32 sectors */

#define GPT_PRIMARY_HDR_LBA   1u
#define GPT_PRIMARY_ARR_LBA   2u
#define GPT_FIRST_USABLE_LBA  (GPT_PRIMARY_ARR_LBA + ARRAY_SECTORS)  /* 34 */

/* Partition layout (in sectors). */
#define BIOS_BOOT_LBA      GPT_FIRST_USABLE_LBA          /* 34 */
#define BIOS_BOOT_SECTS    (2048u - BIOS_BOOT_LBA)       /* ~1 MiB up to LBA 2047 */
#define BIOS_BOOT_END      (BIOS_BOOT_LBA + BIOS_BOOT_SECTS - 1)

#define TOBYFS_LBA         2048u
#define TOBYFS_SECTS       8192u                          /* 4 MiB */
#define TOBYFS_END         (TOBYFS_LBA + TOBYFS_SECTS - 1)

/* FAT32 partition: starts right after tobyfs. Sized to ~10 MiB so
 * there's room for a slot-4 ext4 partition behind it. */
#define FAT32_LBA          (TOBYFS_END + 1)              /* 10240 */
#define FAT32_SECTS        20480u                        /* 10 MiB */
#define FAT32_END          (FAT32_LBA + FAT32_SECTS - 1) /* 30719 */

/* M23D: ext4 partition. Fills the tail of the disk, stopping short
 * of the backup-GPT region (last 33 LBAs). */
#define EXT4_LBA           (FAT32_END + 1)               /* 30720 */
#define EXT4_END           (IMG_LBA - 33 - 1)            /* 98270 */
#define EXT4_SECTS         (EXT4_END - EXT4_LBA + 1)     /* 67551 = ~33 MiB */

/* tobyfs constants -- must match include/tobyos/tobyfs.h. */
#define TFS_MAGIC          0x546F627946535331ULL
#define TFS_BLOCK_SIZE     4096u
#define TFS_SECTORS_PER_BLOCK (TFS_BLOCK_SIZE / SECT_BYTES)
#define TFS_TOTAL_BLOCKS   1024u
#define TFS_INODE_COUNT    256u
#define TFS_INODE_SIZE     128u
#define TFS_INODES_PER_BLOCK (TFS_BLOCK_SIZE / TFS_INODE_SIZE)
#define TFS_INODE_BLOCKS   (TFS_INODE_COUNT / TFS_INODES_PER_BLOCK)
#define TFS_NDIRECT        16u
#define TFS_INODE_BITMAP_BLK 1u
#define TFS_DATA_BITMAP_BLK  2u
#define TFS_INODE_TABLE_BLK  3u
#define TFS_DATA_BLK_START   (TFS_INODE_TABLE_BLK + TFS_INODE_BLOCKS)
#define TFS_TYPE_DIR        2u
#define TFS_ROOT_INO        1u
#define TFS_MODE_VALID      0x10000u
#define TFS_DEFAULT_DIR_MODE (00755u | TFS_MODE_VALID)

/* GPT type GUIDs (mixed-endian byte arrays -- match partition.c). */
static const uint8_t GUID_BIOS_BOOT[16] = {
    0x48, 0x61, 0x68, 0x21,  0x49, 0x64,  0x6F, 0x6E,
    0x74, 0x4E,  0x65, 0x65, 0x64, 0x45, 0x46, 0x49
};
static const uint8_t GUID_TOBYOS_DATA[16] = {
    0x79, 0x62, 0x79, 0x54,  0x00, 0x00,  0x6F, 0x74,
    0x62, 0x79,  0x6F, 0x73, 0x64, 0x61, 0x74, 0x61
};
static const uint8_t GUID_MS_BASIC_DATA[16] = {
    0xA2, 0xA0, 0xD0, 0xEB,  0xE5, 0xB9,  0x33, 0x44,
    0x87, 0xC0,  0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

/* 0fc63daf-8483-4772-8e79-3d69d8477de4 -- Linux filesystem data
 * (matches GPT_TYPE_LINUX_FS in src/partition.c). M23D ext4 slot. */
static const uint8_t GUID_LINUX_FS[16] = {
    0xAF, 0x3D, 0xC6, 0x0F,  0x83, 0x84,  0x72, 0x47,
    0x8E, 0x79,  0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

/* Pre-baked unique partition GUIDs. Tools normally generate fresh
 * ones via UUID v4; we stamp deterministic values here so the test
 * output is byte-stable across runs. The values themselves are not
 * meaningful -- they only need to be globally unique within this
 * disk and well-formed. */
static const uint8_t UNIQUE_BIOS_BOOT[16] = {
    0x01, 0x00, 0x00, 0xB0, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x70, 0x4F, 0x42, 0x59, 0x4F, 0x53
};
static const uint8_t UNIQUE_TOBYFS[16] = {
    0x02, 0x00, 0x00, 0xB0, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x70, 0x4F, 0x42, 0x59, 0x4F, 0x53
};
static const uint8_t UNIQUE_MS[16] = {
    0x03, 0x00, 0x00, 0xB0, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x70, 0x4F, 0x42, 0x59, 0x4F, 0x53
};
static const uint8_t UNIQUE_EXT4[16] = {
    0x04, 0x00, 0x00, 0xB0, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x70, 0x4F, 0x42, 0x59, 0x4F, 0x53
};

/* Stable disk GUID. Same handwave as above. */
static const uint8_t DISK_GUID[16] = {
    0x44, 0x49, 0x53, 0x4B, 0x47, 0x55, 0x49, 0x44,
    0x74, 0x6F, 0x62, 0x79, 0x4F, 0x53, 0x00, 0x01
};

/* ---- on-disk structs ---- */

#pragma pack(push, 1)
struct gpt_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_entry_array_crc32;
};

struct gpt_entry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t name_utf16[36];
};

struct tfs_superblock {
    uint64_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t inode_bitmap_blk;
    uint32_t data_bitmap_blk;
    uint32_t inode_table_blk;
    uint32_t data_blk_start;
    uint32_t root_ino;
    uint32_t reserved[24];
};

struct tfs_inode_disk {
    uint16_t type;
    uint16_t nlink;
    uint32_t size;
    uint32_t mtime;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t direct[TFS_NDIRECT];
    uint8_t  pad[40];
};
#pragma pack(pop)

/* ---- CRC32/IEEE 802.3 ---- */

static uint32_t crc32_table[256];
static int      crc32_ready;

static void crc32_init(void) {
    if (crc32_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static uint32_t crc32(const void *p, size_t n) {
    crc32_init();
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc32_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- helpers ---- */

static int write_at(FILE *fp, uint64_t off, const void *buf, size_t n) {
    if (fseek(fp, (long)off, SEEK_SET) != 0) return -1;
    if (fwrite(buf, 1, n, fp) != n) return -1;
    return 0;
}

static int write_lba(FILE *fp, uint64_t lba, const void *buf, size_t sectors) {
    return write_at(fp, lba * SECT_BYTES, buf, sectors * SECT_BYTES);
}

static void encode_utf16le(const char *src, uint16_t *dst, size_t dst_chars) {
    size_t i = 0;
    for (; src[i] && i < dst_chars - 1; i++) dst[i] = (uint16_t)src[i];
    while (i < dst_chars) dst[i++] = 0;
}

static void bit_set(uint8_t *bm, uint32_t i) {
    bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/* ---- partition layout descriptor (build-time table) ---- */

struct part_def {
    const uint8_t *type_guid;
    const uint8_t *unique_guid;
    uint64_t       start_lba;
    uint64_t       end_lba;
    const char    *name;
};

static const struct part_def parts[] = {
    { GUID_BIOS_BOOT,     UNIQUE_BIOS_BOOT, BIOS_BOOT_LBA, BIOS_BOOT_END, "BIOS-boot"  },
    { GUID_TOBYOS_DATA,   UNIQUE_TOBYFS,    TOBYFS_LBA,    TOBYFS_END,    "tobyOS-data"},
    { GUID_MS_BASIC_DATA, UNIQUE_MS,        FAT32_LBA,     FAT32_END,     "FAT32-data" },
    { GUID_LINUX_FS,      UNIQUE_EXT4,      EXT4_LBA,      EXT4_END,      "ext4-data"  },
};

static const size_t num_parts = sizeof(parts) / sizeof(parts[0]);

/* ---- protective MBR ---- */

static void build_protective_mbr(uint8_t *sect) {
    memset(sect, 0, SECT_BYTES);
    /* MBR partition entry at offset 446 -- 16 bytes describing the
     * "GPT protective" entry of type 0xEE that spans LBA 1..end.
     *   +0  : 0x00      (status)
     *   +1.. : CHS first  (sentinel 0x00 0x02 0x00)
     *   +4  : 0xEE      (type)
     *   +5.. : CHS last   (sentinel 0xFE 0xFF 0xFF)
     *   +8  : starting LBA = 1
     *   +12 : sector count (whole disk minus 1) -- clamp to 0xFFFFFFFF
     *         if disk is bigger than 32-bit LBA range. */
    uint8_t *p = sect + 446;
    p[0] = 0x00;
    p[1] = 0x00; p[2] = 0x02; p[3] = 0x00;
    p[4] = 0xEE;
    p[5] = 0xFE; p[6] = 0xFF; p[7] = 0xFF;
    uint32_t start = 1;
    uint64_t span  = IMG_LBA - 1;
    uint32_t span32 = (span > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)span;
    memcpy(p + 8,  &start,  4);
    memcpy(p + 12, &span32, 4);
    sect[510] = 0x55;
    sect[511] = 0xAA;
}

/* ---- GPT entry array ---- */

static void build_entry_array(uint8_t *array) {
    memset(array, 0, ARRAY_BYTES);
    for (size_t i = 0; i < num_parts; i++) {
        struct gpt_entry e;
        memset(&e, 0, sizeof(e));
        memcpy(e.type_guid,   parts[i].type_guid,   16);
        memcpy(e.unique_guid, parts[i].unique_guid, 16);
        e.starting_lba = parts[i].start_lba;
        e.ending_lba   = parts[i].end_lba;
        e.attributes   = 0;
        encode_utf16le(parts[i].name, e.name_utf16, 36);
        memcpy(array + i * ENTRY_BYTES, &e, sizeof(e));
    }
}

/* ---- GPT header ---- */

static void build_header(struct gpt_header *h,
                         uint64_t my_lba,
                         uint64_t alt_lba,
                         uint64_t arr_lba,
                         uint32_t arr_crc) {
    memset(h, 0, sizeof(*h));
    h->signature              = 0x5452415020494645ULL;   /* "EFI PART" */
    h->revision               = 0x00010000u;
    h->header_size            = 92;
    h->header_crc32           = 0;        /* patched below */
    h->reserved               = 0;
    h->my_lba                 = my_lba;
    h->alternate_lba          = alt_lba;
    h->first_usable_lba       = GPT_FIRST_USABLE_LBA;
    /* Backup metadata occupies the LAST 33 LBAs (32 array + 1 header).
     * last_usable_lba is the highest LBA an entry may extend to. */
    h->last_usable_lba        = IMG_LBA - 33 - 1;
    memcpy(h->disk_guid, DISK_GUID, 16);
    h->partition_entry_lba    = arr_lba;
    h->num_partition_entries  = ENTRY_COUNT;
    h->partition_entry_size   = ENTRY_BYTES;
    h->partition_entry_array_crc32 = arr_crc;
    /* Compute header CRC over first header_size bytes with CRC field
     * zeroed (it already is). */
    h->header_crc32 = crc32(h, h->header_size);
}

/* ---- tobyfs format -- writes a complete tobyfs into [TOBYFS_LBA..] ---- */

static int format_tobyfs_at(FILE *fp, uint64_t base_lba) {
    uint8_t blk[TFS_BLOCK_SIZE];

    /* Block 0: superblock. */
    memset(blk, 0, sizeof(blk));
    struct tfs_superblock sb = {
        .magic            = TFS_MAGIC,
        .block_size       = TFS_BLOCK_SIZE,
        .total_blocks     = TFS_TOTAL_BLOCKS,
        .inode_count      = TFS_INODE_COUNT,
        .inode_bitmap_blk = TFS_INODE_BITMAP_BLK,
        .data_bitmap_blk  = TFS_DATA_BITMAP_BLK,
        .inode_table_blk  = TFS_INODE_TABLE_BLK,
        .data_blk_start   = TFS_DATA_BLK_START,
        .root_ino         = TFS_ROOT_INO,
    };
    memcpy(blk, &sb, sizeof(sb));
    if (write_lba(fp, base_lba + 0 * TFS_SECTORS_PER_BLOCK,
                  blk, TFS_SECTORS_PER_BLOCK) != 0) return -1;

    /* Block 1: inode bitmap (bit 0 reserved, bit 1 = root). */
    memset(blk, 0, sizeof(blk));
    bit_set(blk, 0);
    bit_set(blk, TFS_ROOT_INO);
    if (write_lba(fp, base_lba + TFS_INODE_BITMAP_BLK * TFS_SECTORS_PER_BLOCK,
                  blk, TFS_SECTORS_PER_BLOCK) != 0) return -1;

    /* Block 2: data bitmap (mark all metadata blocks used). */
    memset(blk, 0, sizeof(blk));
    for (uint32_t b = 0; b < TFS_DATA_BLK_START; b++) bit_set(blk, b);
    if (write_lba(fp, base_lba + TFS_DATA_BITMAP_BLK * TFS_SECTORS_PER_BLOCK,
                  blk, TFS_SECTORS_PER_BLOCK) != 0) return -1;

    /* Inode table: block 3 holds inodes 0..31 -- set inode 1 to root dir. */
    memset(blk, 0, sizeof(blk));
    struct tfs_inode_disk *table = (struct tfs_inode_disk *)blk;
    table[TFS_ROOT_INO].type  = TFS_TYPE_DIR;
    table[TFS_ROOT_INO].nlink = 1;
    table[TFS_ROOT_INO].size  = 0;
    table[TFS_ROOT_INO].mtime = 0;
    table[TFS_ROOT_INO].mode  = TFS_DEFAULT_DIR_MODE;
    table[TFS_ROOT_INO].uid   = 0;
    table[TFS_ROOT_INO].gid   = 0;
    if (write_lba(fp, base_lba + TFS_INODE_TABLE_BLK * TFS_SECTORS_PER_BLOCK,
                  blk, TFS_SECTORS_PER_BLOCK) != 0) return -1;

    /* Remaining inode-table blocks: zeroed. */
    memset(blk, 0, sizeof(blk));
    for (uint32_t i = 1; i < TFS_INODE_BLOCKS; i++) {
        if (write_lba(fp,
                      base_lba + (TFS_INODE_TABLE_BLK + i) * TFS_SECTORS_PER_BLOCK,
                      blk, TFS_SECTORS_PER_BLOCK) != 0) return -1;
    }
    return 0;
}

/* ===================================================================
 * FAT32 formatter (host side, for the slot-3 FAT32 partition).
 *
 * Geometry choices:
 *   bytes_per_sec   = 512
 *   sec_per_clus    = 1     (512-byte clusters -> max cluster count
 *                            per ~43 MiB partition, well above the
 *                            MS spec's FAT32 lower bound where it
 *                            still matters; our parser is permissive)
 *   rsvd_sec_cnt    = 32    (boot @0, FSInfo @1, backup boot @6)
 *   num_fats        = 2     (kept in sync)
 * ===================================================================
 */

#define FAT32_BPS           512u
#define FAT32_SPC           1u
#define FAT32_RSVD          32u
#define FAT32_NUM_FATS      2u
#define FAT32_ROOT_CLUS     2u
#define FAT32_FSI_LBA       1u
#define FAT32_BKP_BOOT_LBA  6u

#define FAT32_ENTRY_MASK    0x0FFFFFFFu
#define FAT32_FREE          0x00000000u
#define FAT32_EOC           0x0FFFFFFFu

#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20

#define FAT32_FSI_LEAD_SIG  0x41615252u
#define FAT32_FSI_STRUCT_SIG 0x61417272u
#define FAT32_FSI_TRAIL_SIG 0xAA550000u

#pragma pack(push, 1)
struct fat32_bpb_h {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t rsvd_sec_cnt;
    uint8_t  num_fats;
    uint16_t root_ent_cnt;
    uint16_t tot_sec16;
    uint8_t  media;
    uint16_t fat_sz16;
    uint16_t sec_per_trk;
    uint16_t num_heads;
    uint32_t hidd_sec;
    uint32_t tot_sec32;
    uint32_t fat_sz32;
    uint16_t ext_flags;
    uint16_t fs_ver;
    uint32_t root_clus;
    uint16_t fs_info;
    uint16_t bk_boot_sec;
    uint8_t  reserved[12];
    uint8_t  drv_num;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t vol_id;
    uint8_t  vol_lab[11];
    uint8_t  fs_type[8];
};
struct fat32_fsinfo_h {
    uint32_t lead_sig;
    uint8_t  reserved[480];
    uint32_t struct_sig;
    uint32_t free_count;
    uint32_t nxt_free;
    uint8_t  reserved2[12];
    uint32_t trail_sig;
};
struct fat_dirent_h {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  ntres;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
};
#pragma pack(pop)

static void name83_set(uint8_t out[11], const char *base, const char *ext) {
    memset(out, ' ', 11);
    for (int i = 0; i < 8 && base[i]; i++) {
        char c = base[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i] = (uint8_t)c;
    }
    if (ext) {
        for (int i = 0; i < 3 && ext[i]; i++) {
            char c = ext[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out[8 + i] = (uint8_t)c;
        }
    }
}

/* Compute the minimum FATsz that satisfies the FAT32 constraints:
 *   data_sec     = tot_sec - rsvd - num_fats * fat_sz
 *   cluster_cnt  = data_sec / spc
 *   fat_sz * bps >= cluster_cnt * 4    (each entry is 4 bytes)
 *
 * Substituting and solving for fat_sz:
 *   fat_sz * (bps/4 * spc + num_fats) >= (tot_sec - rsvd)
 *   fat_sz >= ceil( (tot_sec - rsvd) / (bps/4 * spc + num_fats) )
 *
 * For bps=512 the (bps/4) term is 128 (a clean integer), so this is
 * exact for the sector sizes the spec permits. */
static uint32_t fat_sz_for(uint32_t tot_sec, uint32_t rsvd, uint32_t spc,
                           uint32_t bps, uint32_t num_fats) {
    uint64_t numer = (uint64_t)tot_sec - rsvd;
    uint64_t denom = (uint64_t)(bps / 4u) * spc + num_fats;
    uint64_t fat_sz = (numer + denom - 1) / denom;   /* ceil */
    /* Pad by one sector so any fractional cluster slop never makes the
     * FAT undersized (handles spc that don't evenly divide data_sec). */
    return (uint32_t)fat_sz + 1;
}

static int format_fat32_at(FILE *fp, uint64_t base_lba, uint64_t sectors) {
    uint32_t tot_sec   = (uint32_t)sectors;
    uint32_t fat_sz    = fat_sz_for(tot_sec, FAT32_RSVD, FAT32_SPC,
                                    FAT32_BPS, FAT32_NUM_FATS);
    uint32_t fat0_lba  = FAT32_RSVD;
    uint32_t fat1_lba  = FAT32_RSVD + fat_sz;
    uint32_t data_lba  = FAT32_RSVD + FAT32_NUM_FATS * fat_sz;
    uint32_t data_sec  = tot_sec - data_lba;
    uint32_t clus_cnt  = data_sec / FAT32_SPC;

    /* ---- 1. Boot sector with BPB+EBPB ---- */
    uint8_t boot[FAT32_BPS];
    memset(boot, 0, sizeof(boot));
    struct fat32_bpb_h *bpb = (struct fat32_bpb_h *)boot;
    bpb->jmp[0] = 0xEB; bpb->jmp[1] = 0x58; bpb->jmp[2] = 0x90;
    memcpy(bpb->oem, "TOBYOS  ", 8);
    bpb->bytes_per_sec = FAT32_BPS;
    bpb->sec_per_clus  = FAT32_SPC;
    bpb->rsvd_sec_cnt  = FAT32_RSVD;
    bpb->num_fats      = FAT32_NUM_FATS;
    bpb->root_ent_cnt  = 0;
    bpb->tot_sec16     = 0;
    bpb->media         = 0xF8;
    bpb->fat_sz16      = 0;
    bpb->sec_per_trk   = 63;
    bpb->num_heads     = 16;
    bpb->hidd_sec      = (uint32_t)base_lba;
    bpb->tot_sec32     = tot_sec;
    bpb->fat_sz32      = fat_sz;
    bpb->ext_flags     = 0;
    bpb->fs_ver        = 0;
    bpb->root_clus     = FAT32_ROOT_CLUS;
    bpb->fs_info       = FAT32_FSI_LBA;
    bpb->bk_boot_sec   = FAT32_BKP_BOOT_LBA;
    bpb->drv_num       = 0x80;
    bpb->boot_sig      = 0x29;
    bpb->vol_id        = 0xCAFEF00D;
    memcpy(bpb->vol_lab, "TOBYOSFAT32", 11);
    memcpy(bpb->fs_type, "FAT32   ",     8);
    boot[510] = 0x55; boot[511] = 0xAA;
    if (write_lba(fp, base_lba + 0, boot, 1) != 0) return -1;
    if (write_lba(fp, base_lba + FAT32_BKP_BOOT_LBA, boot, 1) != 0) return -1;

    /* ---- 2. FSInfo sector @ rel LBA 1 (and backup @ FAT32_BKP_BOOT_LBA+1) ---- */
    uint8_t fsi_buf[FAT32_BPS];
    memset(fsi_buf, 0, sizeof(fsi_buf));
    struct fat32_fsinfo_h *fsi = (struct fat32_fsinfo_h *)fsi_buf;
    fsi->lead_sig    = FAT32_FSI_LEAD_SIG;
    fsi->struct_sig  = FAT32_FSI_STRUCT_SIG;
    fsi->free_count  = clus_cnt - 4;  /* 2 reserved + root + 2 sample-file clusters */
    fsi->nxt_free    = 5;
    fsi->trail_sig   = FAT32_FSI_TRAIL_SIG;
    if (write_lba(fp, base_lba + FAT32_FSI_LBA, fsi_buf, 1) != 0) return -1;
    if (write_lba(fp, base_lba + FAT32_BKP_BOOT_LBA + 1, fsi_buf, 1) != 0) return -1;

    /* ---- 3. Zero out both FATs first ---- */
    uint8_t zero[FAT32_BPS];
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < fat_sz; i++) {
        if (write_lba(fp, base_lba + fat0_lba + i, zero, 1) != 0) return -1;
        if (write_lba(fp, base_lba + fat1_lba + i, zero, 1) != 0) return -1;
    }

    /* Build the first FAT sector with the well-known initial entries:
     *   entry 0 = 0x0FFFFFF8 (media + EOC marker)
     *   entry 1 = 0x0FFFFFFF (clean shutdown + no error)
     *   entry 2 = EOC        (root cluster, single cluster)
     *   entry 3 = EOC        (sample file 1 -- /HELLO.TXT, single cluster)
     *   entry 4 = EOC        (sample file 2 -- /BIN/README.MD, single cluster)
     *   (sub-directory entries land in cluster 5 onward as we create them)
     */
    uint8_t fat_sec[FAT32_BPS];
    memset(fat_sec, 0, sizeof(fat_sec));
    uint32_t e0 = 0x0FFFFFF8u;
    uint32_t e1 = 0x0FFFFFFFu;
    uint32_t e2 = FAT32_EOC;     /* root cluster */
    uint32_t e3 = FAT32_EOC;     /* /HELLO.TXT */
    uint32_t e4 = FAT32_EOC;     /* /BIN sub-directory cluster */
    uint32_t e5 = FAT32_EOC;     /* /BIN/README.MD */
    memcpy(fat_sec + 0,  &e0, 4);
    memcpy(fat_sec + 4,  &e1, 4);
    memcpy(fat_sec + 8,  &e2, 4);
    memcpy(fat_sec + 12, &e3, 4);
    memcpy(fat_sec + 16, &e4, 4);
    memcpy(fat_sec + 20, &e5, 4);
    if (write_lba(fp, base_lba + fat0_lba, fat_sec, 1) != 0) return -1;
    if (write_lba(fp, base_lba + fat1_lba, fat_sec, 1) != 0) return -1;

    /* ---- 4. Root directory cluster (cluster 2) ----
     * Entries: volume label, HELLO.TXT, BIN (subdir).
     */
    uint8_t root[FAT32_BPS * FAT32_SPC];
    memset(root, 0, sizeof(root));
    struct fat_dirent_h *de = (struct fat_dirent_h *)root;

    /* volume label */
    name83_set(de[0].name, "TOBYOSFAT", "32 ");
    /* override -- volume label puts the label in the name field padded
     * with spaces, NOT split as 8.3. 11 bytes total. */
    memcpy(de[0].name, "TOBYOSFAT32", 11);
    de[0].attr = 0x08;  /* VOLUME_ID */

    /* HELLO.TXT */
    name83_set(de[1].name, "HELLO", "TXT");
    de[1].attr        = FAT_ATTR_ARCHIVE;
    de[1].fst_clus_lo = 3;
    de[1].fst_clus_hi = 0;
    /* size set after we know the body length */

    /* BIN <DIR> */
    name83_set(de[2].name, "BIN", "");
    de[2].attr        = FAT_ATTR_DIRECTORY;
    de[2].fst_clus_lo = 4;
    de[2].fst_clus_hi = 0;
    de[2].file_size   = 0;
    if (write_lba(fp, base_lba + data_lba + (FAT32_ROOT_CLUS - 2) * FAT32_SPC,
                  root, FAT32_SPC) != 0) return -1;

    /* ---- 5. Cluster 3: contents of /HELLO.TXT ---- */
    static const char hello_body[] =
        "Hello from FAT32 on tobyOS!\n"
        "This file lives in cluster 3 of the FAT32-data partition.\n"
        "Mount-time auto-detect found the BPB and exposed it via VFS.\n";
    uint32_t hello_len = (uint32_t)(sizeof(hello_body) - 1);
    de[1].file_size = hello_len;
    /* re-emit root with the correct file size now */
    if (write_lba(fp, base_lba + data_lba + (FAT32_ROOT_CLUS - 2) * FAT32_SPC,
                  root, FAT32_SPC) != 0) return -1;

    uint8_t cluster[FAT32_BPS * FAT32_SPC];
    memset(cluster, 0, sizeof(cluster));
    memcpy(cluster, hello_body, hello_len);
    if (write_lba(fp, base_lba + data_lba + (3 - 2) * FAT32_SPC,
                  cluster, FAT32_SPC) != 0) return -1;

    /* ---- 6. Cluster 4: contents of /BIN/ (one dir cluster) ----
     * Two real entries plus a "." and ".." pair. */
    memset(cluster, 0, sizeof(cluster));
    de = (struct fat_dirent_h *)cluster;
    name83_set(de[0].name, ".", "");
    de[0].attr        = FAT_ATTR_DIRECTORY;
    de[0].fst_clus_lo = 4;
    name83_set(de[1].name, "..", "");
    de[1].attr        = FAT_ATTR_DIRECTORY;
    de[1].fst_clus_lo = FAT32_ROOT_CLUS;   /* parent = root */
    name83_set(de[2].name, "README", "MD");
    de[2].attr        = FAT_ATTR_ARCHIVE;
    de[2].fst_clus_lo = 5;
    de[2].fst_clus_hi = 0;
    /* size set right after we know body length (below) */

    /* ---- 7. Cluster 5: contents of /BIN/README.MD ---- */
    static const char readme_body[] =
        "# README.MD on FAT32\n"
        "Subdirectory listing demo.\n"
        "Try `cat /fat/bin/README.MD` from the shell.\n";
    uint32_t readme_len = (uint32_t)(sizeof(readme_body) - 1);
    de[2].file_size = readme_len;
    if (write_lba(fp, base_lba + data_lba + (4 - 2) * FAT32_SPC,
                  cluster, FAT32_SPC) != 0) return -1;

    memset(cluster, 0, sizeof(cluster));
    memcpy(cluster, readme_body, readme_len);
    if (write_lba(fp, base_lba + data_lba + (5 - 2) * FAT32_SPC,
                  cluster, FAT32_SPC) != 0) return -1;

    printf("  FAT32: formatted (base_lba=%llu, %u sectors, %u clusters, "
           "FATsz=%u, root_clus=%u, sample files=2)\n",
           (unsigned long long)base_lba,
           tot_sec, clus_cnt, fat_sz, FAT32_ROOT_CLUS);
    return 0;
}

/* ===================================================================
 * ext4 formatter (host side, for the slot-4 ext4 partition).
 *
 * Geometry choices (kept minimal -- enough for an in-kernel RO mount):
 *   block_size      = 4096
 *   inode_size      = 128         (s_inode_size, dyn-rev)
 *   inodes_per_grp  = 64          (1 group is plenty)
 *   blocks_per_grp  = 32768       (the spec maximum for 4 KiB blocks)
 *   feature_incompat = FILETYPE   (so dir entries carry file_type)
 *   no extents (legacy 12 direct + 1 single-indirect)
 *   no journal, no htree, no flex_bg, no 64bit
 *
 * Block layout in group 0 (block numbers are filesystem-relative):
 *   block 0       boot block + superblock @ byte 1024
 *   block 1       group descriptor table (one 32-byte entry)
 *   block 2       block bitmap
 *   block 3       inode bitmap
 *   blocks 4..5   inode table (64 inodes x 128 B = 8192 B = 2 blocks)
 *   block 6       data: '/' directory
 *   block 7       data: /HELLO.TXT
 *   block 8       data: /BIN/ directory
 *   block 9       data: /BIN/README.MD
 *
 * Inode allocation:
 *   #2  -- root /         (mode S_IFDIR | 0755, two children)
 *   #11 -- /HELLO.TXT     (S_IFREG | 0644)
 *   #12 -- /BIN/          (S_IFDIR | 0755, one child)
 *   #13 -- /BIN/README.MD (S_IFREG | 0644)
 * ===================================================================
 */

#define E4_BLOCK_BYTES        4096u
#define E4_SECTORS_PER_BLOCK  (E4_BLOCK_BYTES / SECT_BYTES)        /* 8 */
#define E4_INODE_SIZE         128u
#define E4_INODES_PER_GROUP   64u
#define E4_BLOCKS_PER_GROUP   32768u
#define E4_FIRST_INO          11u

#define E4_MAGIC              0xEF53u
#define E4_INC_FILETYPE       0x0002u

#define E4_S_IFMT             0xF000u
#define E4_S_IFREG            0x8000u
#define E4_S_IFDIR            0x4000u

#define E4_FT_REG_FILE        1u
#define E4_FT_DIR             2u

#define E4_INO_BAD            1u
#define E4_INO_ROOT           2u
#define E4_INO_HELLO          11u
#define E4_INO_BIN_DIR        12u
#define E4_INO_README         13u

#define E4_BLK_GDT            1u
#define E4_BLK_BMAP           2u
#define E4_BLK_IMAP           3u
#define E4_BLK_INODE_TABLE   4u
#define E4_BLK_INODE_TABLE_LEN 2u   /* 64*128 / 4096 = 2 */
#define E4_BLK_ROOT_DIR       6u
#define E4_BLK_HELLO          7u
#define E4_BLK_BIN_DIR        8u
#define E4_BLK_README         9u
#define E4_USED_BLOCKS       10u

#pragma pack(push, 1)
struct ext4_super_block_h {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;

    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;

    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;

    /* Pad so the struct is exactly 1024 bytes (used fields end at 352). */
    uint8_t  s_padding[672];
};

struct ext4_group_desc_h {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
};

struct ext4_inode_h {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_hi;
    uint32_t i_obso_faddr;
    uint8_t  i_osd2[12];
};

struct ext4_dirent_h {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    /* name follows */
};
#pragma pack(pop)

/* Append a directory entry into `block` at `*off`. Returns new offset.
 * The caller is responsible for setting the LAST entry's rec_len so
 * that off + rec_len reaches the end of the block (ext4 spec). */
static uint32_t e4_append_dirent(uint8_t *block, uint32_t off,
                                  uint32_t inode, uint8_t file_type,
                                  const char *name) {
    struct ext4_dirent_h de = {0};
    uint8_t nlen = (uint8_t)strlen(name);
    de.inode = inode;
    de.name_len = nlen;
    de.file_type = file_type;
    /* rec_len rounded up to 4. */
    uint16_t base = (uint16_t)(sizeof(de) + nlen);
    uint16_t pad  = (uint16_t)((4u - (base & 3u)) & 3u);
    de.rec_len = (uint16_t)(base + pad);
    memcpy(block + off, &de, sizeof(de));
    memcpy(block + off + sizeof(de), name, nlen);
    /* The pad bytes are zero-init from memset(block, 0, ...) before. */
    return off + de.rec_len;
}

/* Inode N (1-based) lives in inode-table block (4 + (N-1)/inodes_per_block),
 * at byte offset ((N-1) % inodes_per_block) * inode_size. With our chosen
 * geometry (inode_size=128, block_size=4096) inodes_per_block = 32. */
static void e4_place_inode(uint8_t inode_table[E4_BLK_INODE_TABLE_LEN][E4_BLOCK_BYTES],
                           uint32_t ino, const struct ext4_inode_h *src) {
    uint32_t per = E4_BLOCK_BYTES / E4_INODE_SIZE;     /* 32 */
    uint32_t blk_idx = (ino - 1) / per;
    uint32_t off     = ((ino - 1) % per) * E4_INODE_SIZE;
    memcpy(inode_table[blk_idx] + off, src, sizeof(*src));
}

static int format_ext4_at(FILE *fp, uint64_t base_lba, uint64_t sectors) {
    /* sanity / sizing */
    if (sectors < 256) return -1;                  /* < 128 KiB makes no sense */
    uint64_t total_blocks = sectors / E4_SECTORS_PER_BLOCK;
    if (total_blocks > E4_BLOCKS_PER_GROUP) {
        /* For our test images (~33 MiB) we never approach this.
         * Reject anything that would need a second group so we don't
         * silently corrupt the FS. */
        fprintf(stderr,
                "ext4: partition too large for 1 group (%llu blocks, max %u)\n",
                (unsigned long long)total_blocks, E4_BLOCKS_PER_GROUP);
        return -1;
    }

    /* ---- 1. Block 0: boot/padding + superblock @ offset 1024 ---- */
    uint8_t blk0[E4_BLOCK_BYTES];
    memset(blk0, 0, sizeof(blk0));

    struct ext4_super_block_h sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_inodes_count        = E4_INODES_PER_GROUP;          /* 64 */
    sb.s_blocks_count_lo     = (uint32_t)total_blocks;
    sb.s_r_blocks_count_lo   = 0;
    sb.s_free_blocks_count_lo= (uint32_t)(total_blocks - E4_USED_BLOCKS);
    sb.s_free_inodes_count   = E4_INODES_PER_GROUP - 13u;    /* 1..13 used */
    sb.s_first_data_block    = 0;                            /* >=2 KiB blocks */
    sb.s_log_block_size      = 2;                            /* 1024 << 2 = 4096 */
    sb.s_log_cluster_size    = 2;
    sb.s_blocks_per_group    = E4_BLOCKS_PER_GROUP;
    sb.s_clusters_per_group  = E4_BLOCKS_PER_GROUP;
    sb.s_inodes_per_group    = E4_INODES_PER_GROUP;
    sb.s_mtime               = 0;
    sb.s_wtime               = (uint32_t)time(NULL);
    sb.s_mnt_count           = 0;
    sb.s_max_mnt_count       = 0;
    sb.s_magic               = E4_MAGIC;
    sb.s_state               = 1;        /* CLEAN */
    sb.s_errors              = 1;        /* CONTINUE */
    sb.s_minor_rev_level     = 0;
    sb.s_lastcheck           = (uint32_t)time(NULL);
    sb.s_checkinterval       = 0;
    sb.s_creator_os          = 0;        /* Linux */
    sb.s_rev_level           = 1;        /* DYNAMIC_REV (allows s_inode_size etc.) */
    sb.s_def_resuid          = 0;
    sb.s_def_resgid          = 0;

    sb.s_first_ino           = E4_FIRST_INO;
    sb.s_inode_size          = E4_INODE_SIZE;
    sb.s_block_group_nr      = 0;
    sb.s_feature_compat      = 0;
    sb.s_feature_incompat    = E4_INC_FILETYPE;
    sb.s_feature_ro_compat   = 0;
    /* Stable UUID + label so the disk identifies cleanly. */
    static const uint8_t E4_UUID[16] = {
        0xE4, 0x74, 0x6F, 0x62, 0x79, 0x4F, 0x53, 0x00,
        0x6D, 0x6B, 0x66, 0x73, 0x65, 0x78, 0x74, 0x34
    };
    memcpy(sb.s_uuid, E4_UUID, 16);
    memcpy(sb.s_volume_name, "TOBYEXT4", 8);
    sb.s_desc_size           = 32;
    sb.s_mkfs_time           = (uint32_t)time(NULL);
    sb.s_blocks_count_hi     = 0;

    /* SB starts at byte 1024 of block 0. */
    memcpy(blk0 + 1024, &sb, sizeof(sb));
    if (write_lba(fp, base_lba + 0, blk0, E4_SECTORS_PER_BLOCK) != 0) return -1;

    /* ---- 2. Block 1: group descriptor table ---- */
    uint8_t gdt[E4_BLOCK_BYTES];
    memset(gdt, 0, sizeof(gdt));
    struct ext4_group_desc_h gd = {0};
    gd.bg_block_bitmap_lo      = E4_BLK_BMAP;
    gd.bg_inode_bitmap_lo      = E4_BLK_IMAP;
    gd.bg_inode_table_lo       = E4_BLK_INODE_TABLE;
    gd.bg_free_blocks_count_lo = (uint16_t)(total_blocks - E4_USED_BLOCKS);
    gd.bg_free_inodes_count_lo = (uint16_t)(E4_INODES_PER_GROUP - 13u);
    gd.bg_used_dirs_count_lo   = 2;        /* root + /BIN */
    memcpy(gdt, &gd, sizeof(gd));
    if (write_lba(fp, base_lba + E4_BLK_GDT * E4_SECTORS_PER_BLOCK,
                  gdt, E4_SECTORS_PER_BLOCK) != 0) return -1;

    /* ---- 3. Block 2: block bitmap. Bits 0..(USED_BLOCKS-1) set. ---- */
    uint8_t bmap[E4_BLOCK_BYTES];
    memset(bmap, 0, sizeof(bmap));
    for (uint32_t b = 0; b < E4_USED_BLOCKS; b++) bit_set(bmap, b);
    /* Mark blocks beyond the partition end as "in use" so the FS doesn't
     * try to allocate past the partition. With 1 group and our small
     * partitions there's no overflow region to worry about. */
    if (write_lba(fp, base_lba + E4_BLK_BMAP * E4_SECTORS_PER_BLOCK,
                  bmap, E4_SECTORS_PER_BLOCK) != 0) return -1;

    /* ---- 4. Block 3: inode bitmap. Bits 0..12 set (inodes 1..13). ---- */
    uint8_t imap[E4_BLOCK_BYTES];
    memset(imap, 0, sizeof(imap));
    for (uint32_t i = 0; i < 13; i++) bit_set(imap, i);
    if (write_lba(fp, base_lba + E4_BLK_IMAP * E4_SECTORS_PER_BLOCK,
                  imap, E4_SECTORS_PER_BLOCK) != 0) return -1;

    /* ---- 5. Inode table (blocks 4..5) ---- */
    uint8_t inode_table[E4_BLK_INODE_TABLE_LEN][E4_BLOCK_BYTES];
    memset(inode_table, 0, sizeof(inode_table));

    /* Inode 1 (bad blocks placeholder; conventionally empty). */
    {
        struct ext4_inode_h in = {0};
        in.i_mode = 0;
        e4_place_inode(inode_table, E4_INO_BAD, &in);
    }

    /* Sample file bodies (so we know the sizes for the inodes below). */
    static const char hello_body[] =
        "Hello from ext4 on tobyOS!\n"
        "This file lives in inode 11 / block 7 of the ext4 partition.\n"
        "Mounted read-only by the in-kernel ext4 driver.\n";
    static const char readme_body[] =
        "# README.MD on ext4\n"
        "Subdirectory listing demo for the ext4 filesystem.\n"
        "Try `cat /ext/BIN/README.MD` from the shell.\n";
    uint32_t hello_len  = (uint32_t)(sizeof(hello_body)  - 1);
    uint32_t readme_len = (uint32_t)(sizeof(readme_body) - 1);

    /* Inode 2: root directory. Two children + "." + ".." references. */
    {
        struct ext4_inode_h in = {0};
        in.i_mode         = E4_S_IFDIR | 0755u;
        in.i_links_count  = 3;       /* "." in root + ".." in /BIN + parent */
        in.i_size_lo      = E4_BLOCK_BYTES;
        in.i_blocks_lo    = E4_SECTORS_PER_BLOCK;  /* in 512-byte units */
        in.i_flags        = 0;       /* legacy block pointers */
        in.i_block[0]     = E4_BLK_ROOT_DIR;
        e4_place_inode(inode_table, E4_INO_ROOT, &in);
    }

    /* Inode 11: /HELLO.TXT */
    {
        struct ext4_inode_h in = {0};
        in.i_mode         = E4_S_IFREG | 0644u;
        in.i_links_count  = 1;
        in.i_size_lo      = hello_len;
        in.i_blocks_lo    = E4_SECTORS_PER_BLOCK;
        in.i_flags        = 0;
        in.i_block[0]     = E4_BLK_HELLO;
        e4_place_inode(inode_table, E4_INO_HELLO, &in);
    }

    /* Inode 12: /BIN/ */
    {
        struct ext4_inode_h in = {0};
        in.i_mode         = E4_S_IFDIR | 0755u;
        in.i_links_count  = 2;       /* "." and parent's child entry */
        in.i_size_lo      = E4_BLOCK_BYTES;
        in.i_blocks_lo    = E4_SECTORS_PER_BLOCK;
        in.i_flags        = 0;
        in.i_block[0]     = E4_BLK_BIN_DIR;
        e4_place_inode(inode_table, E4_INO_BIN_DIR, &in);
    }

    /* Inode 13: /BIN/README.MD */
    {
        struct ext4_inode_h in = {0};
        in.i_mode         = E4_S_IFREG | 0644u;
        in.i_links_count  = 1;
        in.i_size_lo      = readme_len;
        in.i_blocks_lo    = E4_SECTORS_PER_BLOCK;
        in.i_flags        = 0;
        in.i_block[0]     = E4_BLK_README;
        e4_place_inode(inode_table, E4_INO_README, &in);
    }

    for (uint32_t i = 0; i < E4_BLK_INODE_TABLE_LEN; i++) {
        if (write_lba(fp,
                      base_lba + (E4_BLK_INODE_TABLE + i) * E4_SECTORS_PER_BLOCK,
                      inode_table[i], E4_SECTORS_PER_BLOCK) != 0) return -1;
    }

    /* ---- 6. Block 6: root directory data ---- */
    uint8_t dirblk[E4_BLOCK_BYTES];

    memset(dirblk, 0, sizeof(dirblk));
    {
        uint32_t off = 0;
        off = e4_append_dirent(dirblk, off, E4_INO_ROOT,    E4_FT_DIR,      ".");
        off = e4_append_dirent(dirblk, off, E4_INO_ROOT,    E4_FT_DIR,      "..");
        off = e4_append_dirent(dirblk, off, E4_INO_HELLO,   E4_FT_REG_FILE, "HELLO.TXT");
        /* Last entry's rec_len must reach the end of the block. */
        uint32_t bin_off = off;
        off = e4_append_dirent(dirblk, off, E4_INO_BIN_DIR, E4_FT_DIR,      "BIN");
        struct ext4_dirent_h *last = (struct ext4_dirent_h *)(dirblk + bin_off);
        last->rec_len = (uint16_t)(E4_BLOCK_BYTES - bin_off);
        (void)off;
    }
    if (write_lba(fp, base_lba + E4_BLK_ROOT_DIR * E4_SECTORS_PER_BLOCK,
                  dirblk, E4_SECTORS_PER_BLOCK) != 0) return -1;

    /* ---- 7. Block 7: /HELLO.TXT body ---- */
    uint8_t fblk[E4_BLOCK_BYTES];
    memset(fblk, 0, sizeof(fblk));
    memcpy(fblk, hello_body, hello_len);
    if (write_lba(fp, base_lba + E4_BLK_HELLO * E4_SECTORS_PER_BLOCK,
                  fblk, E4_SECTORS_PER_BLOCK) != 0) return -1;

    /* ---- 8. Block 8: /BIN/ directory data ---- */
    memset(dirblk, 0, sizeof(dirblk));
    {
        uint32_t off = 0;
        off = e4_append_dirent(dirblk, off, E4_INO_BIN_DIR, E4_FT_DIR,      ".");
        off = e4_append_dirent(dirblk, off, E4_INO_ROOT,    E4_FT_DIR,      "..");
        uint32_t readme_off = off;
        off = e4_append_dirent(dirblk, off, E4_INO_README,  E4_FT_REG_FILE, "README.MD");
        struct ext4_dirent_h *last = (struct ext4_dirent_h *)(dirblk + readme_off);
        last->rec_len = (uint16_t)(E4_BLOCK_BYTES - readme_off);
        (void)off;
    }
    if (write_lba(fp, base_lba + E4_BLK_BIN_DIR * E4_SECTORS_PER_BLOCK,
                  dirblk, E4_SECTORS_PER_BLOCK) != 0) return -1;

    /* ---- 9. Block 9: /BIN/README.MD body ---- */
    memset(fblk, 0, sizeof(fblk));
    memcpy(fblk, readme_body, readme_len);
    if (write_lba(fp, base_lba + E4_BLK_README * E4_SECTORS_PER_BLOCK,
                  fblk, E4_SECTORS_PER_BLOCK) != 0) return -1;

    printf("  ext4: formatted (base_lba=%llu, %llu blocks x %u B, "
           "1 group, %u inodes, root=2, files=2)\n",
           (unsigned long long)base_lba,
           (unsigned long long)total_blocks, E4_BLOCK_BYTES,
           E4_INODES_PER_GROUP);
    return 0;
}

/* ---- main ---- */

/* M23C: --fat32-only mode. Writes a raw FAT32 filesystem starting at
 * LBA 0 of the output file, sized to fill the requested byte count.
 * Used by `make usb-stick-img` to build a synthetic USB stick that the
 * kernel's xHCI + USB MSC stack can mount at /usb. The output is NOT
 * GPT-partitioned -- this exactly mirrors what 99% of consumer USB
 * sticks ship from the factory. */
static int mode_fat32_only(const char *path, uint64_t total_bytes) {
    if (total_bytes < 1ull * 1024 * 1024) {
        fprintf(stderr, "mkdisk_gpt: --fat32-only requires >= 1 MiB\n");
        return 1;
    }
    if (total_bytes % FAT32_BPS) {
        fprintf(stderr, "mkdisk_gpt: --fat32-only size must be a multiple of %u\n",
                (unsigned)FAT32_BPS);
        return 1;
    }
    uint64_t total_sectors = total_bytes / FAT32_BPS;
    FILE *fp = fopen(path, "wb+");
    if (!fp) {
        fprintf(stderr, "mkdisk_gpt: cannot open '%s' for writing\n", path);
        return 1;
    }
    if (fseek(fp, (long)total_bytes - 1, SEEK_SET) != 0 ||
        fputc(0, fp) == EOF) {
        fprintf(stderr, "mkdisk_gpt: failed to size '%s' to %llu bytes\n",
                path, (unsigned long long)total_bytes);
        fclose(fp); return 1;
    }
    rewind(fp);
    if (format_fat32_at(fp, 0, total_sectors) != 0) {
        fprintf(stderr, "mkdisk_gpt: failed to format FAT32 in '%s'\n", path);
        fclose(fp); return 1;
    }
    fflush(fp);
    fclose(fp);
    printf("mkdisk_gpt: '%s' formatted as raw FAT32 (%llu sectors, %llu MiB)\n",
           path, (unsigned long long)total_sectors,
           (unsigned long long)(total_bytes / (1024ull * 1024)));
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--fat32-only") == 0) {
        const char *path = argv[2];
        uint64_t total_bytes = strtoull(argv[3], 0, 0);
        return mode_fat32_only(path, total_bytes);
    }
    if (argc != 2) {
        fprintf(stderr,
                "usage: %s <out.img>                 (GPT disk image)\n"
                "       %s --fat32-only <out> <bytes>  (raw FAT32 stick)\n",
                argv[0], argv[0]);
        return 2;
    }
    const char *path = argv[1];
    FILE *fp = fopen(path, "wb+");
    if (!fp) {
        fprintf(stderr, "mkdisk_gpt: cannot open '%s' for writing\n", path);
        return 1;
    }

    /* Pre-size the file. */
    if (fseek(fp, (long)IMG_BYTES - 1, SEEK_SET) != 0 ||
        fputc(0, fp) == EOF) {
        fprintf(stderr, "mkdisk_gpt: failed to size image to %u bytes\n",
                IMG_BYTES);
        fclose(fp); return 1;
    }
    rewind(fp);

    /* 1. Protective MBR at LBA 0. */
    uint8_t mbr[SECT_BYTES];
    build_protective_mbr(mbr);
    if (write_lba(fp, 0, mbr, 1) != 0) {
        perror("write MBR"); fclose(fp); return 1;
    }

    /* 2. Build the entry array (same bytes for primary + backup). */
    uint8_t array[ARRAY_BYTES];
    build_entry_array(array);
    uint32_t arr_crc = crc32(array, ARRAY_BYTES);

    /* 3. Primary header @ LBA 1, primary array @ LBA 2..33. */
    struct gpt_header primary;
    build_header(&primary,
                 GPT_PRIMARY_HDR_LBA,
                 IMG_LBA - 1,                /* alternate = backup */
                 GPT_PRIMARY_ARR_LBA,
                 arr_crc);
    {
        uint8_t buf[SECT_BYTES] = {0};
        memcpy(buf, &primary, sizeof(primary));
        if (write_lba(fp, GPT_PRIMARY_HDR_LBA, buf, 1) != 0) {
            perror("write primary header"); fclose(fp); return 1;
        }
    }
    if (write_lba(fp, GPT_PRIMARY_ARR_LBA, array, ARRAY_SECTORS) != 0) {
        perror("write primary array"); fclose(fp); return 1;
    }

    /* 4. Backup array @ (IMG_LBA - 33), backup header @ (IMG_LBA - 1). */
    uint64_t backup_arr_lba = IMG_LBA - 33;
    uint64_t backup_hdr_lba = IMG_LBA - 1;
    if (write_lba(fp, backup_arr_lba, array, ARRAY_SECTORS) != 0) {
        perror("write backup array"); fclose(fp); return 1;
    }
    struct gpt_header backup;
    build_header(&backup,
                 backup_hdr_lba,
                 GPT_PRIMARY_HDR_LBA,
                 backup_arr_lba,
                 arr_crc);
    {
        uint8_t buf[SECT_BYTES] = {0};
        memcpy(buf, &backup, sizeof(backup));
        if (write_lba(fp, backup_hdr_lba, buf, 1) != 0) {
            perror("write backup header"); fclose(fp); return 1;
        }
    }

    /* 5. Format the tobyOS-data partition (slot 2). */
    if (format_tobyfs_at(fp, TOBYFS_LBA) != 0) {
        fprintf(stderr, "mkdisk_gpt: failed to write tobyfs region\n");
        fclose(fp); return 1;
    }

    /* 6. Format the FAT32-data partition (slot 3). */
    if (format_fat32_at(fp, FAT32_LBA, FAT32_SECTS) != 0) {
        fprintf(stderr, "mkdisk_gpt: failed to write FAT32 region\n");
        fclose(fp); return 1;
    }

    /* 7. Format the ext4 partition (slot 4). */
    if (format_ext4_at(fp, EXT4_LBA, EXT4_SECTS) != 0) {
        fprintf(stderr, "mkdisk_gpt: failed to write ext4 region\n");
        fclose(fp); return 1;
    }

    fflush(fp);
    fclose(fp);

    printf("mkdisk_gpt: '%s' formatted -- %u sectors (%u MiB)\n",
           path, IMG_LBA, IMG_BYTES / (1024 * 1024));
    printf("  GPT entries: %zu used of %u slots, %u-byte each, array CRC=0x%08x\n",
           num_parts, ENTRY_COUNT, ENTRY_BYTES, arr_crc);
    for (size_t i = 0; i < num_parts; i++) {
        printf("    slot %zu : LBA %lu..%lu (%lu sectors, %lu KiB)  '%s'\n",
               i + 1,
               (unsigned long)parts[i].start_lba,
               (unsigned long)parts[i].end_lba,
               (unsigned long)(parts[i].end_lba - parts[i].start_lba + 1),
               (unsigned long)(parts[i].end_lba - parts[i].start_lba + 1) / 2,
               parts[i].name);
    }
    printf("  tobyfs slot 2: formatted (1024 blocks, 256 inodes, root inode=1)\n");
    return 0;
}
