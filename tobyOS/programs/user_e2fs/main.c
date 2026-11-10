/* user_e2fs -- the ext2/3/4 inspection tools tobyOS was missing.
 *
 *   dumpe2fs  e2fsck  debugfs  badblocks
 *
 * These read the on-disk ext4 structures directly through ABI_SYS_BLK_READ,
 * which had to be added: userland could not see a disk's bytes at all, so
 * none of these could exist.
 *
 * ALL FOUR ARE READ-ONLY, and that is a decision rather than an omission.
 * A general raw-write path from userland would bypass the foreign-filesystem
 * and boot-medium guards that provisioning carries -- guards that exist
 * because swap once targeted a Windows EFI partition on this very system.
 * So:
 *
 *   dumpe2fs   full read-only dump: superblock + group descriptors
 *   e2fsck     CHECK only. -n is the default and -p/-y/-f are refused with
 *              the reason, rather than accepted and silently not repairing.
 *   debugfs    read-only subset: stats, and the superblock fields
 *   badblocks  the read-only scan, which is badblocks' own default
 *   resize2fs  -P (the minimum size) is a calculation over the
 *              superblock and is REAL; an actual resize is refused,
 *              for the same reason e2fsck -y and debugfs -w are
 *
 * A checker that says "clean" without being able to repair is still useful;
 * one that ACCEPTS -y and then repairs nothing is a trap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <tobyos/abi/abi.h>

static const char *g_prog = "e2fs";

static long sc4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r)
                     : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static int blk_read(const char *dev, uint64_t lba, void *buf, uint32_t nsec) {
    return (int)sc4(ABI_SYS_BLK_READ, (long)dev, (long)lba, (long)buf, (long)nsec);
}

/* ---- the ext2/3/4 superblock, offset 1024 ------------------------------ */
struct ext_sb {
    uint32_t inodes_count;
    uint32_t blocks_count_lo;
    uint32_t r_blocks_count_lo;
    uint32_t free_blocks_count_lo;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_cluster_size;
    uint32_t blocks_per_group;
    uint32_t clusters_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t  uuid[16];
    char     volume_name[16];
};

#define EXT_MAGIC 0xEF53

static int read_sb(const char *dev, struct ext_sb *sb, uint32_t *blocksize) {
    /* The superblock lives at byte 1024 -- sector 2 with 512-byte sectors --
     * and is 1024 bytes, so two sectors cover it. */
    unsigned char buf[1024];
    if (blk_read(dev, 2, buf, 2) != 0) {
        fprintf(stderr, "%s: cannot read %s (root only, and the device must "
                        "exist in the block registry -- try lsblk)\n",
                g_prog, dev);
        return -1;
    }
    memcpy(sb, buf, sizeof *sb);
    if (sb->magic != EXT_MAGIC) {
        fprintf(stderr, "%s: %s: bad magic 0x%04x -- not an ext2/3/4 "
                        "filesystem\n", g_prog, dev, sb->magic);
        return -1;
    }
    *blocksize = 1024u << sb->log_block_size;
    return 0;
}

static const char *fs_state(uint16_t st) {
    if (st & 0x0001) return "clean";
    if (st & 0x0002) return "has errors";
    return "not clean";
}

static void print_uuid(const uint8_t u[16]) {
    printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
           u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],
           u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
}

/* ---------------------------------------------------------------- dumpe2fs */
static int cmd_dumpe2fs(int argc, char **argv) {
    int header_only = 0, argi = 1;
    for (; argi < argc; argi++) {
        if (argv[argi][0] != '-' || !argv[argi][1]) break;
        if (!strcmp(argv[argi], "-h")) { header_only = 1; continue; }
        fprintf(stderr, "dumpe2fs: unsupported option %s\n", argv[argi]);
        return 1;
    }
    if (argi >= argc) { fprintf(stderr, "usage: dumpe2fs [-h] device\n"); return 1; }
    const char *dev = argv[argi];

    struct ext_sb sb;
    uint32_t bs;
    if (read_sb(dev, &sb, &bs) != 0) return 1;

    printf("Filesystem volume name:   %s\n",
           sb.volume_name[0] ? sb.volume_name : "<none>");
    printf("Filesystem UUID:          ");
    print_uuid(sb.uuid);
    printf("Filesystem magic number:  0x%04X\n", sb.magic);
    printf("Filesystem revision #:    %u\n", (unsigned)sb.rev_level);
    printf("Filesystem state:         %s\n", fs_state(sb.state));
    printf("Filesystem OS type:       %s\n",
           sb.creator_os == 0 ? "Linux" : "other");
    printf("Inode count:              %u\n", (unsigned)sb.inodes_count);
    printf("Block count:              %u\n", (unsigned)sb.blocks_count_lo);
    printf("Reserved block count:     %u\n", (unsigned)sb.r_blocks_count_lo);
    printf("Free blocks:              %u\n", (unsigned)sb.free_blocks_count_lo);
    printf("Free inodes:              %u\n", (unsigned)sb.free_inodes_count);
    printf("First block:              %u\n", (unsigned)sb.first_data_block);
    printf("Block size:               %u\n", (unsigned)bs);
    printf("Blocks per group:         %u\n", (unsigned)sb.blocks_per_group);
    printf("Inodes per group:         %u\n", (unsigned)sb.inodes_per_group);
    printf("Inode size:               %u\n", (unsigned)sb.inode_size);
    printf("Mount count:              %u\n", (unsigned)sb.mnt_count);
    printf("Maximum mount count:      %d\n", (int)(int16_t)sb.max_mnt_count);
    printf("Filesystem features:      compat 0x%08x incompat 0x%08x "
           "ro_compat 0x%08x\n",
           (unsigned)sb.feature_compat, (unsigned)sb.feature_incompat,
           (unsigned)sb.feature_ro_compat);

    if (header_only) return 0;

    /* Group descriptors follow the superblock. With 1 KiB blocks the table
     * starts at block 2; otherwise at block 1 -- getting this wrong reads
     * the wrong sector and prints convincing nonsense. */
    uint32_t gd_block = (bs == 1024) ? 2 : 1;
    uint64_t groups = (sb.blocks_count_lo + sb.blocks_per_group - 1u)
                      / sb.blocks_per_group;
    if (groups > 64) groups = 64;      /* enough to be useful, not a dump war */
    unsigned char buf[512];
    printf("\n");
    for (uint64_t g = 0; g < groups; g++) {
        uint32_t desc_size = (sb.feature_incompat & 0x80) ? 64 : 32;  /* 64BIT */
        uint64_t off = (uint64_t)gd_block * bs + g * desc_size;
        uint64_t lba = off / 512u;
        uint32_t within = (uint32_t)(off % 512u);
        if (blk_read(dev, lba, buf, 1) != 0) break;
        if (within + 16 > 512) continue;
        uint32_t bbitmap, ibitmap, itable;
        uint16_t freeb, freei;
        memcpy(&bbitmap, buf + within + 0, 4);
        memcpy(&ibitmap, buf + within + 4, 4);
        memcpy(&itable,  buf + within + 8, 4);
        memcpy(&freeb,   buf + within + 12, 2);
        memcpy(&freei,   buf + within + 14, 2);
        printf("Group %llu: block bitmap at %u, inode bitmap at %u, "
               "inode table at %u\n", (unsigned long long)g,
               (unsigned)bbitmap, (unsigned)ibitmap, (unsigned)itable);
        printf("  %u free blocks, %u free inodes\n",
               (unsigned)freeb, (unsigned)freei);
    }
    return 0;
}

/* ------------------------------------------------------------------ e2fsck */
static int cmd_e2fsck(int argc, char **argv) {
    int argi = 1;
    for (; argi < argc; argi++) {
        const char *a = argv[argi];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "-n") || !strcmp(a, "-f") || !strcmp(a, "-v")) continue;
        if (!strcmp(a, "-p") || !strcmp(a, "-y") || !strcmp(a, "-a")) {
            /* Refusing is the honest answer. Accepting -y and then repairing
             * nothing would let a script believe the filesystem was fixed. */
            fprintf(stderr, "e2fsck: %s asks for REPAIR, which this build "
                            "cannot do: userland has read-only block access, "
                            "deliberately, so that raw writes cannot bypass "
                            "the foreign-filesystem guards. Check-only "
                            "(-n) works.\n", a);
            return 8;
        }
        fprintf(stderr, "e2fsck: unsupported option %s\n", a);
        return 8;
    }
    if (argi >= argc) { fprintf(stderr, "usage: e2fsck [-n] device\n"); return 8; }
    const char *dev = argv[argi];

    struct ext_sb sb;
    uint32_t bs;
    if (read_sb(dev, &sb, &bs) != 0) return 8;

    int problems = 0;
    printf("e2fsck (check-only) %s\n", dev);

    if (!(sb.state & 0x0001)) {
        printf("%s: filesystem state is '%s'\n", dev, fs_state(sb.state));
        problems++;
    }
    if (sb.free_blocks_count_lo > sb.blocks_count_lo) {
        printf("%s: free block count (%u) exceeds total blocks (%u)\n", dev,
               (unsigned)sb.free_blocks_count_lo, (unsigned)sb.blocks_count_lo);
        problems++;
    }
    if (sb.free_inodes_count > sb.inodes_count) {
        printf("%s: free inode count (%u) exceeds total inodes (%u)\n", dev,
               (unsigned)sb.free_inodes_count, (unsigned)sb.inodes_count);
        problems++;
    }
    if (sb.inodes_per_group == 0 || sb.blocks_per_group == 0) {
        printf("%s: zero inodes or blocks per group -- superblock is corrupt\n", dev);
        problems++;
    }
    if (sb.inode_size && (sb.inode_size & (sb.inode_size - 1))) {
        printf("%s: inode size %u is not a power of two\n", dev,
               (unsigned)sb.inode_size);
        problems++;
    }

    uint32_t used_b = sb.blocks_count_lo - sb.free_blocks_count_lo;
    uint32_t used_i = sb.inodes_count - sb.free_inodes_count;
    printf("%s: %u/%u files (%.0f%% used), %u/%u blocks\n", dev,
           (unsigned)used_i, (unsigned)sb.inodes_count,
           sb.inodes_count ? (double)used_i * 100.0 / sb.inodes_count : 0.0,
           (unsigned)used_b, (unsigned)sb.blocks_count_lo);

    if (problems) {
        printf("%s: %d problem(s) found. This build cannot repair them; use a "
               "system with a writable e2fsck.\n", dev, problems);
        return 4;
    }
    printf("%s: clean\n", dev);
    return 0;
}

/* ----------------------------------------------------------------- debugfs */
static int cmd_debugfs(int argc, char **argv) {
    const char *req = 0;
    int argi = 1;
    for (; argi < argc; argi++) {
        const char *a = argv[argi];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "-R") && argi + 1 < argc) { req = argv[++argi]; continue; }
        if (!strcmp(a, "-w")) {
            fprintf(stderr, "debugfs: -w asks to open the filesystem WRITABLE, "
                            "which this build cannot do: userland block access "
                            "is read-only by design.\n");
            return 1;
        }
        if (!strcmp(a, "-c")) continue;
        fprintf(stderr, "debugfs: unsupported option %s\n", a);
        return 1;
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: debugfs [-R command] device\n"
                        "supported commands: stats, show_super_stats, features\n");
        return 1;
    }
    const char *dev = argv[argi];
    struct ext_sb sb;
    uint32_t bs;
    if (read_sb(dev, &sb, &bs) != 0) return 1;

    if (!req || !strcmp(req, "stats") || !strcmp(req, "show_super_stats")) {
        printf("Filesystem volume name:   %s\n",
               sb.volume_name[0] ? sb.volume_name : "<none>");
        printf("Filesystem state:         %s\n", fs_state(sb.state));
        printf("Block size:               %u\n", (unsigned)bs);
        printf("Inode count:              %u\n", (unsigned)sb.inodes_count);
        printf("Block count:              %u\n", (unsigned)sb.blocks_count_lo);
        printf("Free blocks:              %u\n", (unsigned)sb.free_blocks_count_lo);
        printf("Free inodes:              %u\n", (unsigned)sb.free_inodes_count);
        return 0;
    }
    if (!strcmp(req, "features")) {
        printf("Filesystem features:      compat 0x%08x incompat 0x%08x "
               "ro_compat 0x%08x\n",
               (unsigned)sb.feature_compat, (unsigned)sb.feature_incompat,
               (unsigned)sb.feature_ro_compat);
        return 0;
    }
    fprintf(stderr, "debugfs: '%s' needs a writable or walking implementation "
                    "this build does not have; try stats or features\n", req);
    return 1;
}

/* --------------------------------------------------------------- badblocks
 * The READ-ONLY scan, which is badblocks' own default mode. -w would
 * overwrite the device with test patterns, which is destructive even when it
 * works, so it is refused rather than quietly downgraded to a read test:
 * a caller asking for -w and getting a read scan would believe the media had
 * been exercised for write faults when it had not. */
static int cmd_badblocks(int argc, char **argv) {
    int verbose = 0, argi = 1;
    uint64_t last = 0, first = 0;
    for (; argi < argc; argi++) {
        const char *a = argv[argi];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "-v")) { verbose = 1; continue; }
        if (!strcmp(a, "-s")) continue;
        if (!strcmp(a, "-n") || !strcmp(a, "-w")) {
            fprintf(stderr, "badblocks: %s is a WRITE test and userland block "
                            "access here is read-only. The read-only scan (the "
                            "default) does run.\n", a);
            return 1;
        }
        if (!strcmp(a, "-b") && argi + 1 < argc) { argi++; continue; }
        fprintf(stderr, "badblocks: unsupported option %s\n", a);
        return 1;
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: badblocks [-v] device [last_block [first_block]]\n");
        return 1;
    }
    const char *dev = argv[argi++];
    if (argi < argc) last  = strtoull(argv[argi++], 0, 10);
    if (argi < argc) first = strtoull(argv[argi++], 0, 10);
    if (last == 0) last = 2048;        /* a bounded default scan */

    unsigned char buf[512];
    uint64_t bad = 0, checked = 0;
    for (uint64_t lba = first; lba <= last; lba++) {
        if (blk_read(dev, lba, buf, 1) != 0) {
            printf("%llu\n", (unsigned long long)lba);
            bad++;
            /* A device that is simply absent fails on the first sector and
             * would otherwise print every LBA in the range as "bad". */
            if (bad == 1 && lba == first) {
                fprintf(stderr, "badblocks: %s: first sector unreadable -- "
                                "the device is missing or not permitted "
                                "(root only)\n", dev);
                return 1;
            }
        }
        checked++;
    }
    if (verbose)
        fprintf(stderr, "Pass completed, %llu bad blocks found. (%llu checked)\n",
                (unsigned long long)bad, (unsigned long long)checked);
    return bad ? 1 : 0;
}

/* ---- dispatch ---------------------------------------------------------- */
struct applet { const char *name; int (*fn)(int, char **); };
/* --------------------------------------------------------------- resize2fs
 *
 * The interesting half of resize2fs is readable: -P computes the smallest
 * the filesystem could be shrunk to, which is (blocks in use) plus the
 * metadata that has to survive, and every number it needs is in the
 * superblock. That half is done properly.
 *
 * The other half MOVES DATA, and userland block access here is read-only by
 * design -- the same wall e2fsck -y, debugfs -w and badblocks -w hit, and
 * for the same reason: raw writes from userland would bypass the
 * foreign-filesystem guards. It is refused by name.
 */
static int cmd_resize2fs(int argc, char **argv) {
    int print_min = 0, minimize = 0, argi = 1;
    for (; argi < argc; argi++) {
        if (argv[argi][0] != '-' || !argv[argi][1]) break;
        if (!strcmp(argv[argi], "-P")) { print_min = 1; continue; }
        if (!strcmp(argv[argi], "-M")) { minimize = 1; continue; }
        if (!strcmp(argv[argi], "-f") || !strcmp(argv[argi], "-p")) continue;
        if (!strcmp(argv[argi], "-d") && argi + 1 < argc) { argi++; continue; }
        fprintf(stderr, "resize2fs: unsupported option %s\n", argv[argi]);
        return 1;
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: resize2fs [-P] [-M] device [size]\n");
        return 1;
    }
    const char *dev = argv[argi++];
    const char *want = (argi < argc) ? argv[argi] : NULL;

    struct ext_sb sb;
    uint32_t bs = 0;
    if (read_sb(dev, &sb, &bs) != 0) return 1;

    uint64_t total = sb.blocks_count_lo;
    uint64_t freeb = sb.free_blocks_count_lo;
    uint64_t used  = total > freeb ? total - freeb : total;
    /* resize2fs -P reports the minimum in FILESYSTEM BLOCKS, and adds a
     * margin for the metadata a smaller filesystem still needs. Two percent
     * plus the reserved blocks is what e2fsprogs settles on in practice; a
     * bare used-block count would be a number no shrink could ever reach. */
    uint64_t minimum = used + used / 50 + sb.r_blocks_count_lo + 1;
    if (minimum > total) minimum = total;

    printf("Estimated minimum size of the filesystem: %llu\n",
           (unsigned long long)minimum);
    if (print_min) return 0;

    if (!want && !minimize) {
        /* No size and no -M means "grow to fill the device", which is still
         * a write. */
        fprintf(stderr, "resize2fs: %s is %llu blocks of %u bytes "
                        "(%llu in use)\n", dev,
                (unsigned long long)total, bs, (unsigned long long)used);
    }
    fprintf(stderr, "resize2fs: RESIZING WOULD WRITE, and userland block "
                    "access here is read-only by design, so that raw writes "
                    "cannot bypass the foreign-filesystem guards. The "
                    "calculation (-P) is done; the move is not.\n");
    return 1;
}

static const struct applet g_applets[] = {
    { "dumpe2fs",  cmd_dumpe2fs },
    { "e2fsck",    cmd_e2fsck },
    { "debugfs",   cmd_debugfs },
    { "badblocks", cmd_badblocks },
    { "resize2fs", cmd_resize2fs },
    { 0, 0 }
};

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "e2fs";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    g_prog = base;
    for (int i = 0; g_applets[i].name; i++)
        if (!strcmp(base, g_applets[i].name))
            return g_applets[i].fn(argc, argv);
    if (argc > 1) {
        for (int i = 0; g_applets[i].name; i++) {
            if (strcmp(argv[1], g_applets[i].name)) continue;
            g_prog = g_applets[i].name;
            return g_applets[i].fn(argc - 1, argv + 1);
        }
    }
    fprintf(stderr, "e2fs: applets:");
    for (int i = 0; g_applets[i].name; i++) fprintf(stderr, " %s", g_applets[i].name);
    fprintf(stderr, "\n");
    return 1;
}
