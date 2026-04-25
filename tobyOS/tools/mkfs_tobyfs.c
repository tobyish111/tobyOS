/* mkfs_tobyfs.c -- host tool to format a blank tobyfs disk image.
 *
 * Build (MSYS2/UCRT64 or any *nix):
 *     gcc -O2 -Wall -Wextra -o build/mkfs_tobyfs tools/mkfs_tobyfs.c
 *
 * Use:
 *     build/mkfs_tobyfs disk.img        # create+format 4 MiB image
 *
 * Layout (must match include/tobyos/tobyfs.h verbatim):
 *   block 0       superblock
 *   block 1       inode bitmap   (256 inodes; bits 0..10 = "metadata", bit 1 = root)
 *   block 2       data  bitmap   (1024 bits; bits 0..10 reserved for metadata)
 *   blocks 3..10  inode table    (256 inodes * 128 bytes = 32 KiB = 8 blocks)
 *   blocks 11..1023 data         (1013 blocks)
 *
 * After formatting:
 *   - inode 1 is the root directory (TYPE_DIR, size=0, no data blocks).
 *   - All other inodes are TYPE_FREE (zeroed).
 *   - Both bitmaps reflect the metadata reservations + the root inode.
 *
 * The kernel will refuse to mount any image whose magic doesn't match
 * TFS_MAGIC, so accidentally pointing it at /dev/sda1 won't trash data
 * silently.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#define TFS_MAGIC          0x546F627946535331ULL  /* "TobyFSS1" */
#define TFS_BLOCK_SIZE     4096u
#define TFS_TOTAL_BLOCKS   1024u
#define TFS_INODE_COUNT    256u
#define TFS_INODE_SIZE     128u
#define TFS_INODES_PER_BLOCK (TFS_BLOCK_SIZE / TFS_INODE_SIZE)
#define TFS_INODE_BLOCKS   (TFS_INODE_COUNT / TFS_INODES_PER_BLOCK)
#define TFS_NDIRECT        16u

#define TFS_INODE_BITMAP_BLK  1u
#define TFS_DATA_BITMAP_BLK   2u
#define TFS_INODE_TABLE_BLK   3u
#define TFS_DATA_BLK_START    (TFS_INODE_TABLE_BLK + TFS_INODE_BLOCKS)

#define TFS_TYPE_FREE 0u
#define TFS_TYPE_FILE 1u
#define TFS_TYPE_DIR  2u

#define TFS_ROOT_INO  1u

#pragma pack(push, 1)
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
/* Permission bits (milestone 15) -- mirror of TFS_MODE_* in
 * include/tobyos/tobyfs.h. The kernel treats mode==0 (i.e. no
 * MODE_VALID bit) as "legacy inode -- allow everything", so disks
 * formatted with an older mkfs still mount cleanly. */
#define TFS_MODE_VALID    0x10000u
#define TFS_DEFAULT_DIR_MODE  (00755u | TFS_MODE_VALID)

struct tfs_inode_disk {
    uint16_t type;
    uint16_t nlink;
    uint32_t size;
    uint32_t mtime;
    uint32_t mode;            /* milestone 15: 9-bit perms + MODE_VALID */
    uint32_t uid;             /* owner uid */
    uint32_t gid;             /* owner gid */
    uint32_t direct[TFS_NDIRECT];
    uint8_t  pad[40];
};
#pragma pack(pop)

_Static_assert(sizeof(struct tfs_inode_disk) == TFS_INODE_SIZE, "inode size");

static int write_block(FILE *fp, uint32_t blk, const void *buf) {
    if (fseek(fp, (long)blk * TFS_BLOCK_SIZE, SEEK_SET) != 0) return -1;
    if (fwrite(buf, 1, TFS_BLOCK_SIZE, fp) != TFS_BLOCK_SIZE)  return -1;
    return 0;
}

static void bit_set(uint8_t *bm, uint32_t i) {
    bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <disk.img>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    FILE *fp = fopen(path, "wb+");
    if (!fp) {
        fprintf(stderr, "mkfs: cannot open '%s' for writing\n", path);
        return 1;
    }

    /* Pre-size the file to TFS_TOTAL_BLOCKS * TFS_BLOCK_SIZE = 4 MiB
     * by writing a single zero byte at the last offset, then rewinding.
     * We then go fill in the metadata blocks. */
    if (fseek(fp, (long)TFS_TOTAL_BLOCKS * TFS_BLOCK_SIZE - 1, SEEK_SET) != 0 ||
        fputc(0, fp) == EOF) {
        fprintf(stderr, "mkfs: failed to size image\n");
        fclose(fp); return 1;
    }
    rewind(fp);

    /* Block 0: superblock. */
    uint8_t blk[TFS_BLOCK_SIZE];
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
    if (write_block(fp, 0, blk) != 0) { perror("write sb"); fclose(fp); return 1; }

    /* Block 1: inode bitmap. Bit 0 (reserved) and bit 1 (root) are set. */
    memset(blk, 0, sizeof(blk));
    bit_set(blk, 0);
    bit_set(blk, TFS_ROOT_INO);
    if (write_block(fp, TFS_INODE_BITMAP_BLK, blk) != 0) {
        perror("write ibitmap"); fclose(fp); return 1;
    }

    /* Block 2: data bitmap. Mark blocks 0..(TFS_DATA_BLK_START - 1) as
     * "used" -- they're metadata, not free for user data. */
    memset(blk, 0, sizeof(blk));
    for (uint32_t b = 0; b < TFS_DATA_BLK_START; b++) bit_set(blk, b);
    if (write_block(fp, TFS_DATA_BITMAP_BLK, blk) != 0) {
        perror("write dbitmap"); fclose(fp); return 1;
    }

    /* Blocks 3..10: inode table. Block 3 holds inodes 0..31; we set
     * inode 1 to TYPE_DIR (the empty root). */
    memset(blk, 0, sizeof(blk));
    struct tfs_inode_disk *table = (struct tfs_inode_disk *)blk;
    table[TFS_ROOT_INO].type  = TFS_TYPE_DIR;
    table[TFS_ROOT_INO].nlink = 1;
    table[TFS_ROOT_INO].size  = 0;
    table[TFS_ROOT_INO].mtime = 0;
    /* Root directory: owned by root (uid/gid 0), drwxr-xr-x. */
    table[TFS_ROOT_INO].mode  = TFS_DEFAULT_DIR_MODE;
    table[TFS_ROOT_INO].uid   = 0;
    table[TFS_ROOT_INO].gid   = 0;
    /* No direct blocks yet -- the kernel will lazily allocate one when
     * the first dirent is inserted. */
    if (write_block(fp, TFS_INODE_TABLE_BLK, blk) != 0) {
        perror("write inode table[0]"); fclose(fp); return 1;
    }
    /* Remaining 7 inode-table blocks: zeroed. */
    memset(blk, 0, sizeof(blk));
    for (uint32_t i = 1; i < TFS_INODE_BLOCKS; i++) {
        if (write_block(fp, TFS_INODE_TABLE_BLK + i, blk) != 0) {
            perror("write inode table[i]"); fclose(fp); return 1;
        }
    }

    /* Data area is already zeroed thanks to fputc(0) at the tail; we
     * deliberately leave it untouched so first-allocation reads see
     * predictable zeros. */

    fflush(fp);
    fclose(fp);

    printf("mkfs.tobyfs: '%s' formatted -- %u blocks of %u bytes (%u KiB), "
           "%u inodes, %u user data blocks\n",
           path,
           TFS_TOTAL_BLOCKS, TFS_BLOCK_SIZE,
           (TFS_TOTAL_BLOCKS * TFS_BLOCK_SIZE) / 1024,
           TFS_INODE_COUNT - 1,
           TFS_TOTAL_BLOCKS - TFS_DATA_BLK_START);
    return 0;
}
