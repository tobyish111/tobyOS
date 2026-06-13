/* jbd2.h -- on-disk format of the ext3/ext4 journalling layer (JBD2).
 *
 * The journal is a circular log of physical block writes. Each atomic
 * filesystem operation becomes one transaction:
 *
 *     [ descriptor block ] [ data block ] ... [ data block ] [ commit ]
 *
 * The descriptor lists, via a tag per data block, the *real* (home)
 * block number each following journal data block belongs to. The commit
 * block is written last; a transaction is only "committed" once its
 * commit block is durable. On mount, recovery scans the log from the
 * superblock's s_start and replays every transaction that has a matching
 * commit, then writes each journalled block to its home location. Replay
 * is idempotent, so a crash *during* replay (or during the original
 * checkpoint) heals on the next mount.
 *
 * IMPORTANT: every multi-byte field below is stored BIG-ENDIAN on disk
 * (JBD2 inherited this from ext3). tobyOS runs little-endian x86-64, so
 * the driver byte-swaps on every access (be16()/be32() in ext4.c).
 *
 * What we implement: the classic format -- no metadata checksums
 * (CSUM_V2/V3), no 64-bit block numbers, no async commit. We DO parse
 * revoke blocks during recovery for fidelity, though our own writer
 * never emits them (it checkpoints each transaction immediately, so at
 * most one transaction is ever outstanding in the log).
 */

#ifndef TOBYOS_JBD2_H
#define TOBYOS_JBD2_H

#include <tobyos/types.h>

#define JBD2_MAGIC_NUMBER   0xc03b3998u

/* journal_header_s.h_blocktype */
#define JBD2_DESCRIPTOR_BLOCK   1u
#define JBD2_COMMIT_BLOCK       2u
#define JBD2_SUPERBLOCK_V1      3u
#define JBD2_SUPERBLOCK_V2      4u
#define JBD2_REVOKE_BLOCK       5u

/* block-tag t_flags (also big-endian on disk) */
#define JBD2_FLAG_ESCAPE        1u   /* data block's first word was the magic */
#define JBD2_FLAG_SAME_UUID     2u   /* no 16-byte UUID follows this tag */
#define JBD2_FLAG_DELETED       4u
#define JBD2_FLAG_LAST_TAG      8u   /* final tag in this descriptor block */

/* journal feature_incompat bits. We can drive a journal that has at most
 * REVOKE set; anything else (64BIT / ASYNC_COMMIT / CSUM_V2 / CSUM_V3)
 * means we can't safely recover it. */
#define JBD2_FEATURE_INCOMPAT_REVOKE        0x0001u
#define JBD2_FEATURE_INCOMPAT_64BIT         0x0002u
#define JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT  0x0004u
#define JBD2_FEATURE_INCOMPAT_CSUM_V2       0x0008u
#define JBD2_FEATURE_INCOMPAT_CSUM_V3       0x0010u

#define JBD2_INCOMPAT_SUPPORTED  (JBD2_FEATURE_INCOMPAT_REVOKE)

#pragma pack(push, 1)

/* Common 12-byte header at the front of every log block. */
typedef struct {
    uint32_t h_magic;       /* JBD2_MAGIC_NUMBER */
    uint32_t h_blocktype;   /* JBD2_*_BLOCK */
    uint32_t h_sequence;    /* transaction sequence number */
} jbd2_header_t;

/* The journal superblock (journal block 0), exactly one block on disk
 * but only its first 1024 bytes carry defined fields. */
typedef struct {
    jbd2_header_t s_header;

    /* Static information describing the journal. */
    uint32_t s_blocksize;       /* journal device blocksize */
    uint32_t s_maxlen;          /* total blocks in journal file */
    uint32_t s_first;           /* first block of log information */

    /* Dynamic information describing the current state of the log. */
    uint32_t s_sequence;        /* first commit ID expected in log */
    uint32_t s_start;           /* blocknr of start of log (0 == clean) */

    uint32_t s_errno;

    /* Remaining fields are only valid in a v2 superblock. */
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    uint32_t s_nr_users;
    uint32_t s_dynsuper;
    uint32_t s_max_transaction;
    uint32_t s_max_trans_data;
    uint8_t  s_checksum_type;
    uint8_t  s_padding2[3];
    uint32_t s_num_fc_blks;
    uint32_t s_padding[41];
    uint32_t s_checksum;
    uint8_t  s_users[16 * 48];
} jbd2_superblock_t;
_Static_assert(sizeof(jbd2_superblock_t) == 1024,
               "jbd2 superblock defined region must be 1024 bytes");

/* Classic block tag (no CSUM_V3, no 64BIT): 8 bytes. When the 64BIT
 * feature is set a trailing __be32 t_blocknr_high follows, which we
 * neither emit nor support. */
typedef struct {
    uint32_t t_blocknr;     /* home (real) block number, low 32 bits */
    uint16_t t_checksum;    /* unused without CSUM features */
    uint16_t t_flags;       /* JBD2_FLAG_* */
} jbd2_block_tag_t;
_Static_assert(sizeof(jbd2_block_tag_t) == 8, "jbd2 classic block tag 8 B");

/* Header of a revoke block (followed by r_count-12 bytes of be32 block
 * numbers). We parse these during recovery but never write them. */
typedef struct {
    jbd2_header_t r_header;
    uint32_t      r_count;  /* used bytes in this block, including header */
} jbd2_revoke_header_t;

/* Commit block: just the header for us (we emit zeroed checksum fields,
 * which the classic format permits). */
typedef struct {
    jbd2_header_t h;
    uint8_t  h_chksum_type;
    uint8_t  h_chksum_size;
    uint8_t  h_padding[2];
    uint32_t h_chksum[8];
    uint64_t h_commit_sec;
    uint32_t h_commit_nsec;
} jbd2_commit_header_t;

#pragma pack(pop)

#endif /* TOBYOS_JBD2_H */
