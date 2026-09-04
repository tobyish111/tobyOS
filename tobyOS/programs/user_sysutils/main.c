/* user_sysutils -- kernel modules and filesystem makers/checkers, native
 * (2026-09-04, native-parity batch). One binary dispatched on argv[0]:
 *
 *   insmod lsmod modprobe rmmod
 *   blkid fsck fsck.ext2 fsck.ext3 fsck.ext4 fsck.vfat fsck.fat fsck.minix
 *   mkfs.ext2 mkfs.minix mkfs.vfat mkfs.fat tune2fs
 *
 * Raw sectors go through ABI_SYS_BLK_READ/BLK_WRITE (root only; the kernel
 * refuses to write a device that backs a live mount, so mkfs on /data's
 * disk fails with EBUSY instead of eating the filesystem). mkfs.ext2 writes
 * a revision-1 ext2 with FILETYPE dirents and a superblock copy in every
 * group, which the tree's own e2fsck/dumpe2fs (user_e2fs) read back.
 * mkfs.minix writes v1/v2/v3; mkfs.vfat writes FAT12/16/32 with an FSInfo
 * sector. fsck dispatches on the probed type like util-linux. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <tobyos/abi/abi.h>

static const char *g_prog = "sysutils";

static long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static long sc4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}

/* ------------------------------------------------------------ devices */
static const char *dev_name(const char *arg) {
    if (!strncmp(arg, "/dev/", 5)) return arg + 5;
    return arg;
}

static long blk_read(const char *dev, uint64_t lba, void *buf, uint32_t nsec) {
    uint8_t *p = buf;
    while (nsec) {
        uint32_t n = nsec > 256 ? 256 : nsec;
        long rc = sc4(ABI_SYS_BLK_READ, (long)(uintptr_t)dev, (long)lba, (long)(uintptr_t)p, (long)n);
        if (rc < 0) return rc;
        lba += n; p += (size_t)n * 512; nsec -= n;
    }
    return 0;
}

static long g_last_rc;   /* the errno of the last failed raw write, for messages */

static long blk_write(const char *dev, uint64_t lba, const void *buf, uint32_t nsec) {
    const uint8_t *p = buf;
    while (nsec) {
        uint32_t n = nsec > 256 ? 256 : nsec;
        long rc = sc4(ABI_SYS_BLK_WRITE, (long)(uintptr_t)dev, (long)lba, (long)(uintptr_t)p, (long)n);
        if (rc < 0) { g_last_rc = rc; return rc; }
        lba += n; p += (size_t)n * 512; nsec -= n;
    }
    return 0;
}

static int blk_info(const char *dev, struct abi_blk_info *out) {
    static struct abi_blk_info list[ABI_BLK_MAX_DEVICES];
    long n = sc2(ABI_SYS_BLK_LIST, (long)(uintptr_t)list, ABI_BLK_MAX_DEVICES);
    for (long i = 0; i < n; i++) if (!strcmp(list[i].name, dev)) { *out = list[i]; return 0; }
    return -1;
}

static const char *err_str(long rc) {
    switch (-rc) {
    case ABI_EPERM: return "Operation not permitted";
    case ABI_ENOENT: return "No such file or directory";
    case ABI_EBUSY: return "Device or resource busy";
    case ABI_EIO: return "Input/output error";
    case ABI_EINVAL: return "Invalid argument";
    default: return "Unknown error";
    }
}

/* Zero-fill lba..lba+nsec in chunks. */
static long blk_zero(const char *dev, uint64_t lba, uint64_t nsec) {
    static uint8_t z[256 * 512];
    memset(z, 0, sizeof z);
    while (nsec) {
        uint32_t n = nsec > 256 ? 256 : (uint32_t)nsec;
        long rc = blk_write(dev, lba, z, n);
        if (rc < 0) return rc;
        lba += n; nsec -= n;
    }
    return 0;
}

static uint64_t g_rng;
static uint32_t rnd32(void) {
    if (!g_rng) g_rng = ((uint64_t)time(NULL) << 20) ^ (uint64_t)getpid() ^ 0x9E3779B97F4A7C15ull;
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

/* ====================================================================
 *                        insmod / rmmod / lsmod / modprobe
 * ==================================================================== */
static int module_loaded(const char *name) {
    FILE *f = fopen("/proc/modules", "r");
    if (!f) return 0;
    char line[256]; int found = 0;
    while (fgets(line, sizeof line, f)) { char n[64]; if (sscanf(line, "%63s", n) == 1 && !strcmp(n, name)) { found = 1; break; } }
    fclose(f);
    return found;
}

static void module_basename(const char *path, char *out, size_t cap) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    snprintf(out, cap, "%s", b);
    size_t l = strlen(out);
    if (l > 3 && !strcmp(out + l - 3, ".ko")) out[l - 3] = '\0';
}

static int cmd_insmod(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage:\n\tinsmod [options] filename [args]\n"); return 1; }
    if (!strcmp(argv[1], "--version")) { printf("kmod version 31 (tobyOS)\n"); return 0; }
    const char *path = argv[1];
    struct stat st;
    if (stat(path, &st) != 0) { fprintf(stderr, "insmod: ERROR: could not load module %s: No such file or directory\n", path); return 1; }
    { char nm[64]; module_basename(path, nm, sizeof nm);
      if (module_loaded(nm)) { fprintf(stderr, "insmod: ERROR: could not insert module %s: File exists\n", path); return 1; } }
    long rc = sc3(ABI_SYS_MODULE, 0, (long)(uintptr_t)path, 0);
    if (rc < 0) {
        fprintf(stderr, "insmod: ERROR: could not insert module %s: %s\n", path,
                rc == -ABI_EPERM ? "Operation not permitted" : rc == -ABI_EEXIST ? "File exists" :
                rc == -ABI_ENOENT ? "No such file or directory" : "Invalid module format");
        return 1;
    }
    return 0;
}

static int cmd_rmmod(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage:\n\trmmod [options] modulename ...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;   /* -f -s -v -w accepted */
        char nm[64]; module_basename(argv[i], nm, sizeof nm);
        if (!module_loaded(nm)) { fprintf(stderr, "rmmod: ERROR: Module %s is not currently loaded\n", nm); rc = 1; continue; }
        long r = sc3(ABI_SYS_MODULE, 1, (long)(uintptr_t)nm, 0);
        if (r < 0) { fprintf(stderr, "rmmod: ERROR: could not remove module %s: %s\n", nm, r == -ABI_EPERM ? "Operation not permitted" : r == -ABI_ENOENT ? "No such file or directory" : "Resource temporarily unavailable"); rc = 1; }
    }
    return rc;
}

static int cmd_lsmod(int argc, char **argv) {
    (void)argc; (void)argv;
    FILE *f = fopen("/proc/modules", "r");
    if (!f) { fprintf(stderr, "lsmod: cannot open /proc/modules\n"); return 1; }
    printf("Module                  Size  Used by\n");
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char name[64]; unsigned long size; int used;
        if (sscanf(line, "%63s %lu %d", name, &size, &used) < 3) continue;
        printf("%-19s %8lu  %d\n", name, size, used);
    }
    fclose(f);
    return 0;
}

static int cmd_modprobe(int argc, char **argv) {
    int remove = 0, quiet = 0, dry = 0, verbose = 0, show = 0, all = 0, rc = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-r") || !strcmp(a, "--remove")) remove = 1;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) quiet = 1;
        else if (!strcmp(a, "-n") || !strcmp(a, "--dry-run") || !strcmp(a, "--show")) dry = 1;
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) verbose = 1;
        else if (!strcmp(a, "-D") || !strcmp(a, "--show-depends")) show = 1;
        else if (!strcmp(a, "-a") || !strcmp(a, "--all")) all = 1;
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("kmod version 31 (tobyOS)\n"); return 0; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { printf("Usage:\n\tmodprobe [options] [-i] [-b] modulename\n\tmodprobe [options] -a [-i] [-b] modulename [modulename...]\n\tmodprobe [options] -r [-i] modulename\n"); return 0; }
        else if (!strcmp(a, "-b") || !strcmp(a, "-s") || !strcmp(a, "-f") || !strcmp(a, "-i") || !strcmp(a, "--first-time")) {}
        else { fprintf(stderr, "modprobe: unrecognized option '%s'\n", a); return 1; }
    }
    if (i >= argc) { fprintf(stderr, "modprobe: ERROR: missing parameters. See -h.\n"); return 1; }
    (void)all;
    int nmods = 0;
    for (; i < argc; i++) {
        char nm[64]; module_basename(argv[i], nm, sizeof nm);
        for (char *p = nm; *p; p++) if (*p == '-') *p = '_';
        if (argv[i][0] != '-' && strchr(argv[i], '=') && nmods) continue;   /* module parameters: ignored */
        nmods++;
        char path[256]; snprintf(path, sizeof path, "/lib/modules/%s.ko", nm);
        struct stat st;
        if (remove) {
            if (!module_loaded(nm)) { if (!quiet) fprintf(stderr, "modprobe: FATAL: Module %s not found.\n", nm); rc = 1; continue; }
            if (verbose || dry) printf("rmmod %s\n", nm);
            if (dry) continue;
            if (sc3(ABI_SYS_MODULE, 1, (long)(uintptr_t)nm, 0) < 0) { fprintf(stderr, "modprobe: FATAL: Module %s is in use.\n", nm); rc = 1; }
            continue;
        }
        if (stat(path, &st) != 0) {
            /* also accept a dash spelling on disk */
            char alt[256]; snprintf(alt, sizeof alt, "/lib/modules/%s.ko", argv[i]);
            if (stat(alt, &st) == 0) snprintf(path, sizeof path, "%s", alt);
            else { if (!quiet) fprintf(stderr, "modprobe: FATAL: Module %s not found in directory /lib/modules\n", nm); rc = 1; continue; }
        }
        if (show) { printf("insmod %s\n", path); continue; }
        if (module_loaded(nm)) continue;
        if (verbose || dry) printf("insmod %s\n", path);
        if (dry) continue;
        long r = sc3(ABI_SYS_MODULE, 0, (long)(uintptr_t)path, 0);
        if (r < 0 && r != -ABI_EEXIST) { fprintf(stderr, "modprobe: ERROR: could not insert '%s': %s\n", nm, r == -ABI_EPERM ? "Operation not permitted" : "Invalid module format"); rc = 1; }
    }
    return rc;
}

/* ====================================================================
 *                               probing
 * ==================================================================== */
struct probe { char type[16]; char label[64]; char uuid[40]; char version[16]; };

static void uuid_fmt(const uint8_t *u, char *out) {
    snprintf(out, 40, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static void trim_label(char *s) {
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\0')) s[--l] = '\0';
}

/* Returns 1 if a filesystem was recognised. */
static int probe_dev(const char *dev, struct probe *pr) {
    memset(pr, 0, sizeof *pr);
    static uint8_t sec[4 * 512];
    if (blk_read(dev, 0, sec, 4) < 0) return 0;
    const uint8_t *sb = sec + 1024;
    /* ext2/3/4 */
    if (get16(sb + 0x38) == 0xEF53) {
        uint32_t compat = get32(sb + 0x5C), incompat = get32(sb + 0x60), rocompat = get32(sb + 0x64);
        if (incompat & (0x40 | 0x80 | 0x200 | 0x8000) || rocompat & (0x8 | 0x10 | 0x40 | 0x400)) snprintf(pr->type, sizeof pr->type, "ext4");
        else if (compat & 0x4) snprintf(pr->type, sizeof pr->type, "ext3");
        else snprintf(pr->type, sizeof pr->type, "ext2");
        memcpy(pr->label, sb + 0x78, 16); pr->label[16] = '\0'; trim_label(pr->label);
        int has_uuid = 0; for (int i = 0; i < 16; i++) if (sb[0x68 + i]) has_uuid = 1;
        if (has_uuid) uuid_fmt(sb + 0x68, pr->uuid);
        snprintf(pr->version, sizeof pr->version, "%u.%u", get32(sb + 0x4C), get16(sb + 0x3E));
        return 1;
    }
    /* minix: magic at 1024+16 (v1/v2) or 1024+24 (v3) */
    {
        uint32_t m = get16(sb + 16), m3 = get16(sb + 24);
        if (m == 0x137F || m == 0x138F || m == 0x2468 || m == 0x2478) { snprintf(pr->type, sizeof pr->type, "minix"); snprintf(pr->version, sizeof pr->version, "%d", m < 0x2000 ? 1 : 2); return 1; }
        if (m3 == 0x4D5A) { snprintf(pr->type, sizeof pr->type, "minix"); snprintf(pr->version, sizeof pr->version, "3"); return 1; }
    }
    /* FAT: 0x55AA signature, bytes/sector 512, and a plausible BPB */
    if (sec[510] == 0x55 && sec[511] == 0xAA && get16(sec + 11) == 512 && sec[13] && (sec[0] == 0xEB || sec[0] == 0xE9)) {
        int fat32 = get16(sec + 22) == 0 && get32(sec + 36) != 0;
        const uint8_t *ext = fat32 ? sec + 64 : sec + 36;
        if (ext[2] == 0x29 || ext[2] == 0x28) {
            snprintf(pr->type, sizeof pr->type, "vfat");
            uint32_t serial = get32(ext + 3);
            snprintf(pr->uuid, sizeof pr->uuid, "%04X-%04X", serial >> 16, serial & 0xFFFF);
            memcpy(pr->label, ext + 7, 11); pr->label[11] = '\0'; trim_label(pr->label);
            if (!strcmp(pr->label, "NO NAME")) pr->label[0] = '\0';
            snprintf(pr->version, sizeof pr->version, "%s", fat32 ? "FAT32" : (get16(sec + 22) && ((get16(sec + 19) ? get16(sec + 19) : get32(sec + 32)) / sec[13]) < 4085 ? "FAT12" : "FAT16"));
            return 1;
        }
    }
    /* anything the kernel already knows (tobyfs, iso9660...) */
    struct abi_blk_info bi;
    if (blk_info(dev, &bi) == 0 && bi.fs[0] && strcmp(bi.fs, "unknown") && strcmp(bi.fs, "-")) {
        snprintf(pr->type, sizeof pr->type, "%s", bi.fs);
        snprintf(pr->label, sizeof pr->label, "%s", bi.label);
        return 1;
    }
    return 0;
}

/* ====================================================================
 *                                 blkid
 * ==================================================================== */
static int cmd_blkid(int argc, char **argv) {
    int out_value = 0, out_export = 0, i = 1;
    const char *tag = NULL;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) { const char *v = argv[++i]; out_value = !strcmp(v, "value"); out_export = !strcmp(v, "export"); continue; }
        if (!strcmp(a, "-s") && i + 1 < argc) { tag = argv[++i]; continue; }
        if (!strcmp(a, "-c") || !strcmp(a, "-w")) { i++; continue; }
        if (!strcmp(a, "-g") || !strcmp(a, "-p") || !strcmp(a, "-i")) continue;
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("blkid from util-linux 2.39 (tobyOS)\n"); return 0; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { printf("Usage:\n blkid [options] [<device> ...]\n\nOptions:\n -o <format>  output format: value, export, full (default)\n -s <tag>     show specified tag(s) (default show all tags)\n"); return 0; }
        fprintf(stderr, "blkid: invalid option -- '%s'\n", a + 1); return 1;
    }
    static struct abi_blk_info list[ABI_BLK_MAX_DEVICES];
    const char *devs[ABI_BLK_MAX_DEVICES + 32]; int nd = 0;
    if (i < argc) for (; i < argc && nd < 48; i++) devs[nd++] = dev_name(argv[i]);
    else {
        long n = sc2(ABI_SYS_BLK_LIST, (long)(uintptr_t)list, ABI_BLK_MAX_DEVICES);
        for (long k = 0; k < n && nd < 48; k++) if (list[k].class != 3) devs[nd++] = list[k].name;
    }
    int printed = 0;
    for (int k = 0; k < nd; k++) {
        struct probe pr;
        if (!probe_dev(devs[k], &pr)) continue;
        const char *keys[4] = { "LABEL", "UUID", "TYPE", "VERSION" };
        const char *vals[4] = { pr.label, pr.uuid, pr.type, pr.version };
        if (out_value) {
            for (int t = 0; t < 4; t++) if (vals[t][0] && (!tag || !strcmp(tag, keys[t]))) { printf("%s\n", vals[t]); printed = 1; }
            continue;
        }
        if (out_export) {
            printf("DEVNAME=/dev/%s\n", devs[k]);
            for (int t = 0; t < 4; t++) if (vals[t][0] && (!tag || !strcmp(tag, keys[t]))) printf("%s=%s\n", keys[t], vals[t]);
            printf("\n"); printed = 1;
            continue;
        }
        printf("/dev/%s:", devs[k]);
        int any = 0;
        for (int t = 0; t < 4; t++) if (vals[t][0] && (!tag || !strcmp(tag, keys[t]))) { printf(" %s=\"%s\"", keys[t], vals[t]); any = 1; }
        printf("\n");
        if (any) printed = 1;
    }
    return printed ? 0 : 2;
}

/* ====================================================================
 *                               mkfs.ext2
 * ==================================================================== */
static int cmd_mkfs_ext2(int argc, char **argv) {
    const char *label = NULL, *dev = NULL;
    uint32_t bsize = 0, ninodes = 0, inode_size = 128, reserved_pct = 5;
    uint64_t blocks_arg = 0;
    int quiet = 0, i = 1, ext_type = 2;
    if (!strcmp(g_prog, "mkfs.ext3")) ext_type = 3;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-L") && i + 1 < argc) { label = argv[++i]; continue; }
        if (!strcmp(a, "-b") && i + 1 < argc) { bsize = (uint32_t)atoi(argv[++i]); continue; }
        if (!strcmp(a, "-N") && i + 1 < argc) { ninodes = (uint32_t)atoi(argv[++i]); continue; }
        if (!strcmp(a, "-I") && i + 1 < argc) { inode_size = (uint32_t)atoi(argv[++i]); continue; }
        if (!strcmp(a, "-m") && i + 1 < argc) { reserved_pct = (uint32_t)atoi(argv[++i]); continue; }
        if (!strcmp(a, "-i") && i + 1 < argc) { uint32_t ratio = (uint32_t)atoi(argv[++i]); if (ratio) ninodes = 0, reserved_pct |= 0; (void)ratio; continue; }
        if (!strcmp(a, "-t") && i + 1 < argc) { const char *t = argv[++i]; ext_type = !strcmp(t, "ext3") ? 3 : 2; continue; }
        if (!strcmp(a, "-O") || !strcmp(a, "-E") || !strcmp(a, "-o") || !strcmp(a, "-g") || !strcmp(a, "-M") || !strcmp(a, "-U") || !strcmp(a, "-e") || !strcmp(a, "-T") || !strcmp(a, "-d")) { i++; continue; }
        if (!strcmp(a, "-q")) { quiet = 1; continue; }
        if (!strcmp(a, "-F") || !strcmp(a, "-v") || !strcmp(a, "-c") || !strcmp(a, "-j") || !strcmp(a, "-n") || !strcmp(a, "-S") || !strcmp(a, "-D")) continue;
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("mke2fs 1.47.0 (tobyOS)\n"); return 0; }
        if (a[0] == '-') { fprintf(stderr, "%s: invalid option -- '%s'\nUsage: %s [-c|-l filename] [-b block-size] [-N number-of-inodes] [-L volume-label] [-m reserved-blocks-percentage] [-I inode-size] [-q] [-F] device [blocks-count]\n", g_prog, a + 1, g_prog); return 1; }
        if (!dev) dev = dev_name(a); else blocks_arg = strtoull(a, NULL, 10);
    }
    if (!dev) { fprintf(stderr, "Usage: %s [-c|-l filename] [-b block-size] [-N number-of-inodes] [-L volume-label] [-m reserved-blocks-percentage] [-I inode-size] [-q] [-F] device [blocks-count]\n", g_prog); return 1; }
    struct abi_blk_info bi;
    if (blk_info(dev, &bi)) { fprintf(stderr, "%s: No such file or directory while trying to determine filesystem size\n", g_prog); return 1; }
    uint64_t dev_bytes = bi.sector_count * 512ull;
    if (!bsize) bsize = dev_bytes >= 512ull * 1024 * 1024 ? 4096 : 1024;
    if (bsize != 1024 && bsize != 2048 && bsize != 4096) { fprintf(stderr, "%s: invalid block size - %u\n", g_prog, bsize); return 1; }
    if (inode_size != 128 && inode_size != 256) { fprintf(stderr, "%s: invalid inode size %u (min 128/max 256)\n", g_prog, inode_size); return 1; }
    uint64_t total = blocks_arg ? blocks_arg : dev_bytes / bsize;
    if (blocks_arg && blocks_arg * bsize > dev_bytes) { fprintf(stderr, "%s: Filesystem larger than apparent device size.\n", g_prog); return 1; }
    if (total > 0xFFFFFFFFull) total = 0xFFFFFFFFull;
    if (total < 64) { fprintf(stderr, "%s: Device size reported to be zero or too small.\n", g_prog); return 1; }
    uint32_t first_data = bsize == 1024 ? 1 : 0;
    uint32_t bpg = bsize * 8;
    uint32_t groups = (uint32_t)((total - first_data + bpg - 1) / bpg);
    /* mke2fs folds a tiny last group into the previous one */
    if (groups > 1 && (total - first_data) - (uint64_t)(groups - 1) * bpg < 50) { groups--; total = first_data + (uint64_t)groups * bpg; }
    uint32_t ipb = bsize / inode_size;                 /* inodes per block */
    if (!ninodes) ninodes = (uint32_t)((total * bsize) / 16384);   /* 1 inode per 16 KiB (mke2fs default) */
    if (ninodes < 16) ninodes = 16;
    uint32_t ipg = (ninodes + groups - 1) / groups;
    ipg = (ipg + ipb - 1) / ipb * ipb;
    if (ipg > bpg) ipg = bpg;
    ninodes = ipg * groups;
    uint32_t itb = ipg / ipb;                          /* inode table blocks per group */
    uint32_t gdt_blocks = (groups * 32 + bsize - 1) / bsize;
    uint32_t overhead = 1 + gdt_blocks + 2 + itb;      /* sb + gdt + bitmaps + itable */
    if (overhead + 2 >= bpg) { fprintf(stderr, "%s: too many inodes for this size\n", g_prog); return 1; }
    uint32_t reserved = (uint32_t)(total * reserved_pct / 100);
    uint8_t uuid[16]; for (int k = 0; k < 16; k += 4) put32(uuid + k, rnd32()); uuid[6] = (uint8_t)((uuid[6] & 0x0F) | 0x40); uuid[8] = (uint8_t)((uuid[8] & 0x3F) | 0x80);
    time_t now = time(NULL);

    if (!quiet) {
        printf("Creating filesystem with %llu %uk blocks and %u inodes\n", (unsigned long long)total, bsize / 1024, ninodes);
        char us[40]; uuid_fmt(uuid, us);
        printf("Filesystem UUID: %s\n", us);
        if (groups > 1) {
            printf("Superblock backups stored on blocks: \n\t");
            for (uint32_t g = 1; g < groups; g++) printf("%s%llu", g > 1 ? ", " : "", (unsigned long long)(first_data + (uint64_t)g * bpg));
            printf("\n");
        }
        printf("\nAllocating group tables: done                            \n");
    }
    /* free counts: root dir + lost+found take one data block each in group 0;
     * inodes 1..11 used */
    uint64_t free_blocks = 0;
    uint32_t root_blk = 0, lf_blk = 0;
    uint8_t *sb = calloc(1, bsize);
    uint8_t *gdt = calloc(1, (size_t)gdt_blocks * bsize);
    uint8_t *bbm = calloc(1, bsize), *ibm = calloc(1, bsize);
    if (!sb || !gdt || !bbm || !ibm) { fprintf(stderr, "%s: out of memory\n", g_prog); return 1; }
    /* pass 1: compute group descriptors and totals */
    for (uint32_t g = 0; g < groups; g++) {
        uint64_t gstart = first_data + (uint64_t)g * bpg;
        uint64_t gblocks = g == groups - 1 ? total - gstart : bpg;
        uint32_t used = overhead + (g == 0 ? 2 : 0);
        uint32_t gfree = (uint32_t)gblocks - used;
        uint8_t *d = gdt + g * 32;
        put32(d + 0, (uint32_t)(gstart + 1 + gdt_blocks));
        put32(d + 4, (uint32_t)(gstart + 2 + gdt_blocks));
        put32(d + 8, (uint32_t)(gstart + 3 + gdt_blocks));
        put16(d + 12, gfree);
        put16(d + 14, g == 0 ? ipg - 11 : ipg);
        put16(d + 16, g == 0 ? 2 : 0);
        free_blocks += gfree;
        if (g == 0) { root_blk = (uint32_t)(gstart + overhead); lf_blk = root_blk + 1; }
    }
    /* superblock */
    put32(sb + 0x00, ninodes);
    put32(sb + 0x04, (uint32_t)total);
    put32(sb + 0x08, reserved);
    put32(sb + 0x0C, (uint32_t)free_blocks);
    put32(sb + 0x10, ninodes - 11);
    put32(sb + 0x14, first_data);
    put32(sb + 0x18, bsize == 1024 ? 0 : bsize == 2048 ? 1 : 2);
    put32(sb + 0x1C, bsize == 1024 ? 0 : bsize == 2048 ? 1 : 2);
    put32(sb + 0x20, bpg);
    put32(sb + 0x24, bpg);
    put32(sb + 0x28, ipg);
    put32(sb + 0x2C, 0);
    put32(sb + 0x30, (uint32_t)now);
    put16(sb + 0x34, 0);
    put16(sb + 0x36, 0xFFFF);          /* max mount count: -1 */
    put16(sb + 0x38, 0xEF53);
    put16(sb + 0x3A, 1);               /* clean */
    put16(sb + 0x3C, 1);               /* errors: continue */
    put16(sb + 0x3E, 0);
    put32(sb + 0x40, (uint32_t)now);   /* lastcheck */
    put32(sb + 0x44, 0);
    put32(sb + 0x48, 0);               /* creator: Linux */
    put32(sb + 0x4C, 1);               /* rev 1 */
    put16(sb + 0x50, 0); put16(sb + 0x52, 0);
    put32(sb + 0x54, 11);              /* first ino */
    put16(sb + 0x58, (uint16_t)inode_size);
    put16(sb + 0x5A, 0);               /* block group nr (patched per copy) */
    put32(sb + 0x5C, 0);               /* compat (ext3 adds HAS_JOURNAL: not written here) */
    put32(sb + 0x60, 0x0002);          /* incompat: FILETYPE */
    put32(sb + 0x64, 0);               /* ro_compat */
    memcpy(sb + 0x68, uuid, 16);
    if (label) { size_t l = strlen(label); if (l > 16) l = 16; memcpy(sb + 0x78, label, l); }
    (void)ext_type;
    /* write: zero the metadata area of each group, then the structures */
    uint32_t spb = bsize / 512;
    for (uint32_t g = 0; g < groups; g++) {
        uint64_t gstart = first_data + (uint64_t)g * bpg;
        uint64_t gblocks = g == groups - 1 ? total - gstart : bpg;
        uint32_t used = overhead + (g == 0 ? 2 : 0);
        /* zero superblock..inode table (+2 data blocks in group 0) */
        long rc = blk_zero(dev, gstart * spb, (uint64_t)used * spb);
        if (rc < 0) goto wfail;
        put16(sb + 0x5A, (uint16_t)g);
        if (g == 0 && bsize == 1024) rc = blk_write(dev, 2, sb, 2);          /* sb at byte 1024 = sectors 2,3 */
        else if (g == 0) { uint8_t *b0 = calloc(1, bsize); memcpy(b0 + 1024, sb, 1024); rc = blk_write(dev, 0, b0, spb); free(b0); }
        else rc = blk_write(dev, gstart * spb, sb, 2);
        if (rc < 0) goto wfail;
        rc = blk_write(dev, (gstart + 1) * spb, gdt, gdt_blocks * spb);
        if (rc < 0) goto wfail;
        /* block bitmap */
        memset(bbm, 0, bsize);
        for (uint32_t b = 0; b < used; b++) bbm[b / 8] |= (uint8_t)(1u << (b % 8));
        for (uint64_t b = gblocks; b < bpg; b++) bbm[b / 8] |= (uint8_t)(1u << (b % 8));
        rc = blk_write(dev, (gstart + 1 + gdt_blocks) * spb, bbm, spb);
        if (rc < 0) goto wfail;
        /* inode bitmap */
        memset(ibm, 0, bsize);
        if (g == 0) for (uint32_t k = 0; k < 11; k++) ibm[k / 8] |= (uint8_t)(1u << (k % 8));
        for (uint32_t k = ipg; k < bpg; k++) ibm[k / 8] |= (uint8_t)(1u << (k % 8));
        rc = blk_write(dev, (gstart + 2 + gdt_blocks) * spb, ibm, spb);
        if (rc < 0) goto wfail;
        if (!quiet && g == 0) printf("Writing inode tables: done                            \n");
    }
    /* inodes 2 (root) and 11 (lost+found) in group 0's table, first block */
    {
        uint8_t *it = calloc(1, bsize);
        uint64_t itable = first_data + 3 + gdt_blocks;
        uint8_t *root = it + (2 - 1) * inode_size;
        put16(root + 0, 040755); put16(root + 2, 0); put32(root + 4, bsize);
        put32(root + 8, (uint32_t)now); put32(root + 12, (uint32_t)now); put32(root + 16, (uint32_t)now);
        put16(root + 24, 0); put16(root + 26, 3); put32(root + 28, bsize / 512);
        put32(root + 40, root_blk);
        int lf_in_block0 = (11 - 1) < ipb;
        uint8_t *lfi = lf_in_block0 ? it + (11 - 1) * inode_size : NULL;
        uint8_t lfbuf[256]; memset(lfbuf, 0, sizeof lfbuf);
        uint8_t *lf = lfi ? lfi : lfbuf;
        put16(lf + 0, 040700); put16(lf + 2, 0); put32(lf + 4, bsize);
        put32(lf + 8, (uint32_t)now); put32(lf + 12, (uint32_t)now); put32(lf + 16, (uint32_t)now);
        put16(lf + 24, 0); put16(lf + 26, 2); put32(lf + 28, bsize / 512);
        put32(lf + 40, lf_blk);
        long rc = blk_write(dev, itable * spb, it, spb);
        if (rc < 0) { free(it); goto wfail; }
        if (!lfi) {
            /* inode 11 lives in the second table block (inode_size 256 with 1k blocks) */
            uint8_t *it2 = calloc(1, bsize);
            uint32_t idx = (11 - 1) - ipb;
            memcpy(it2 + idx * inode_size, lfbuf, inode_size);
            rc = blk_write(dev, (itable + 1) * spb, it2, spb);
            free(it2);
            if (rc < 0) { free(it); goto wfail; }
        }
        free(it);
        /* directory blocks */
        uint8_t *d = calloc(1, bsize);
        put32(d + 0, 2); put16(d + 4, 12); d[6] = 1; d[7] = 2; d[8] = '.';
        put32(d + 12, 2); put16(d + 16, 12); d[18] = 2; d[19] = 2; d[20] = '.'; d[21] = '.';
        put32(d + 24, 11); put16(d + 28, (uint16_t)(bsize - 24)); d[30] = 10; d[31] = 2; memcpy(d + 32, "lost+found", 10);
        rc = blk_write(dev, (uint64_t)root_blk * spb, d, spb);
        if (rc < 0) { free(d); goto wfail; }
        memset(d, 0, bsize);
        put32(d + 0, 11); put16(d + 4, 12); d[6] = 1; d[7] = 2; d[8] = '.';
        put32(d + 12, 2); put16(d + 16, (uint16_t)(bsize - 12)); d[18] = 2; d[19] = 2; d[20] = '.'; d[21] = '.';
        rc = blk_write(dev, (uint64_t)lf_blk * spb, d, spb);
        free(d);
        if (rc < 0) goto wfail;
    }
    if (!quiet) printf("Writing superblocks and filesystem accounting information: done\n\n");
    free(sb); free(gdt); free(bbm); free(ibm);
    return 0;
wfail:
    fprintf(stderr, "%s: %s while writing to /dev/%s\n", g_prog, err_str(g_last_rc), dev);
    free(sb); free(gdt); free(bbm); free(ibm);
    return 1;
}

/* ====================================================================
 *                               mkfs.minix
 * ==================================================================== */
#define MINIX_BLOCK 1024

static int cmd_mkfs_minix(int argc, char **argv) {
    int version = 1, namelen = 30; uint32_t req_inodes = 0; const char *dev = NULL; uint64_t blocks_arg = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-1")) version = 1;
        else if (!strcmp(a, "-2") || !strcmp(a, "-v")) version = 2;
        else if (!strcmp(a, "-3")) version = 3;
        else if (!strcmp(a, "-n") && i + 1 < argc) namelen = atoi(argv[++i]);
        else if (!strncmp(a, "-n", 2) && a[2]) namelen = atoi(a + 2);
        else if (!strcmp(a, "-i") && i + 1 < argc) req_inodes = (uint32_t)atoi(argv[++i]);
        else if (!strncmp(a, "-i", 2) && a[2]) req_inodes = (uint32_t)atoi(a + 2);
        else if (!strcmp(a, "-c") || !strcmp(a, "--check")) {}
        else if (!strcmp(a, "-l") && i + 1 < argc) i++;
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("mkfs.minix from util-linux 2.39 (tobyOS)\n"); return 0; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { printf("Usage:\n mkfs.minix [options] /dev/name [blocks]\n\nOptions:\n -1                      use Minix version 1\n -2, -v                  use Minix version 2\n -3                      use Minix version 3\n -n, --namelength <num>  maximum length of filenames\n -i, --inodes <num>      number of inodes for the filesystem\n -c, --check             check the device for bad blocks\n"); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "mkfs.minix: invalid option -- '%s'\n", a + 1); return 1; }
        else if (!dev) dev = dev_name(a); else blocks_arg = strtoull(a, NULL, 10);
    }
    if (!dev) { fprintf(stderr, "Usage:\n mkfs.minix [options] /dev/name [blocks]\n"); return 1; }
    if (namelen != 14 && namelen != 30 && !(version == 3 && namelen == 60)) { fprintf(stderr, "mkfs.minix: unsupported name length: %d\n", namelen); return 1; }
    if (version == 3) namelen = 60;
    struct abi_blk_info bi;
    if (blk_info(dev, &bi)) { fprintf(stderr, "mkfs.minix: cannot stat /dev/%s\n", dev); return 1; }
    uint64_t total = blocks_arg ? blocks_arg : bi.sector_count * 512ull / MINIX_BLOCK;
    if (blocks_arg && blocks_arg * MINIX_BLOCK > bi.sector_count * 512ull) { fprintf(stderr, "mkfs.minix: will not try to make filesystem on '/dev/%s': device too small\n", dev); return 1; }
    uint64_t maxz = version == 1 ? 65535 : 0xFFFFFFFFull;
    if (total > maxz) total = maxz;
    if (total < 10) { fprintf(stderr, "mkfs.minix: %s: number of blocks too small\n", dev); return 1; }
    uint32_t inode_size = version == 1 ? 32 : 64;
    uint32_t ipb = MINIX_BLOCK / inode_size;
    uint32_t inodes = req_inodes ? req_inodes : (uint32_t)(total / 3);
    inodes = (inodes + ipb - 1) / ipb * ipb;
    if (version == 1 && inodes > 65535) inodes = 65535 / ipb * ipb;
    uint32_t imap_blocks = (inodes + 1 + 8 * MINIX_BLOCK - 1) / (8 * MINIX_BLOCK);
    uint32_t itable_blocks = inodes / ipb;
    uint32_t zmap_blocks = (uint32_t)((total - (2 + imap_blocks + itable_blocks) + 8 * MINIX_BLOCK - 1) / (8 * MINIX_BLOCK));
    /* zmap depends on firstdatazone which depends on zmap: iterate once */
    uint32_t first_data = 2 + imap_blocks + zmap_blocks + itable_blocks;
    zmap_blocks = (uint32_t)((total - first_data + 1 + 8 * MINIX_BLOCK - 1) / (8 * MINIX_BLOCK));
    first_data = 2 + imap_blocks + zmap_blocks + itable_blocks;
    if (first_data + 1 > total) { fprintf(stderr, "mkfs.minix: too many inodes for this size\n"); return 1; }
    uint32_t dirent = version == 3 ? 64 : (uint32_t)(2 + namelen);
    uint8_t *sb = calloc(1, MINIX_BLOCK);
    uint32_t max_size = version == 1 ? 0x10081C00u : 0x7FFFFFFFu;
    if (version < 3) {
        put16(sb + 0, version == 1 ? inodes : inodes);
        put16(sb + 2, version == 1 ? (uint32_t)total : 0);
        put16(sb + 4, imap_blocks);
        put16(sb + 6, zmap_blocks);
        put16(sb + 8, first_data);
        put16(sb + 10, 0);
        put32(sb + 12, max_size);
        put16(sb + 16, version == 1 ? (namelen == 30 ? 0x138F : 0x137F) : (namelen == 30 ? 0x2478 : 0x2468));
        put16(sb + 18, 1);              /* MINIX_VALID_FS */
        put32(sb + 20, version == 2 ? (uint32_t)total : 0);
    } else {
        put32(sb + 0, inodes);
        put16(sb + 4, 0);
        put16(sb + 6, imap_blocks);
        put16(sb + 8, zmap_blocks);
        put16(sb + 10, first_data);
        put16(sb + 12, 0);
        put16(sb + 14, 0);
        put32(sb + 16, max_size);
        put32(sb + 20, (uint32_t)total);
        put16(sb + 24, 0x4D5A);
        put16(sb + 26, 0);
        put16(sb + 28, MINIX_BLOCK);
        sb[30] = 0;
    }
    long rc;
    rc = blk_zero(dev, 0, (uint64_t)(first_data + 1) * 2);
    if (rc < 0) goto fail;
    rc = blk_write(dev, 2, sb, 2);
    if (rc < 0) goto fail;
    /* inode map: bit 0 and bit 1 (root inode 1) set; padding beyond inodes set */
    {
        uint8_t *map = calloc(1, (size_t)imap_blocks * MINIX_BLOCK);
        map[0] |= 0x03;
        for (uint32_t b = inodes + 1; b < imap_blocks * MINIX_BLOCK * 8; b++) map[b / 8] |= (uint8_t)(1u << (b % 8));
        rc = blk_write(dev, 2 * 2, map, imap_blocks * 2);
        free(map);
        if (rc < 0) goto fail;
    }
    /* zone map: bit 0 reserved set; zone first_data (root dir) used = bit 1;
     * padding beyond the last zone set */
    {
        uint8_t *map = calloc(1, (size_t)zmap_blocks * MINIX_BLOCK);
        map[0] |= 0x03;
        uint64_t nz = total - first_data;   /* zones 1..nz map to first_data..total-1 */
        for (uint64_t b = nz + 1; b < (uint64_t)zmap_blocks * MINIX_BLOCK * 8; b++) map[b / 8] |= (uint8_t)(1u << (b % 8));
        rc = blk_write(dev, (2 + imap_blocks) * 2, map, zmap_blocks * 2);
        free(map);
        if (rc < 0) goto fail;
    }
    /* inode table: inode 1 = root directory */
    {
        uint8_t *it = calloc(1, MINIX_BLOCK);
        time_t now = time(NULL);
        if (version == 1) {
            put16(it + 0, 040755); put16(it + 2, 0); put32(it + 4, 2 * dirent); put32(it + 8, (uint32_t)now);
            it[12] = 0; it[13] = 2; put16(it + 14, first_data);
        } else {
            put16(it + 0, 040755); put16(it + 2, 2); put16(it + 4, 0); put16(it + 6, 0);
            put32(it + 8, 2 * dirent); put32(it + 12, (uint32_t)now); put32(it + 16, (uint32_t)now); put32(it + 20, (uint32_t)now);
            put32(it + 24, first_data);
        }
        rc = blk_write(dev, (2 + imap_blocks + zmap_blocks) * 2, it, 2);
        free(it);
        if (rc < 0) goto fail;
    }
    /* root directory zone */
    {
        uint8_t *d = calloc(1, MINIX_BLOCK);
        if (version == 3) { put32(d, 1); d[4] = '.'; put32(d + dirent, 1); d[dirent + 4] = '.'; d[dirent + 5] = '.'; }
        else { put16(d, 1); d[2] = '.'; put16(d + dirent, 1); d[dirent + 2] = '.'; d[dirent + 3] = '.'; }
        rc = blk_write(dev, (uint64_t)first_data * 2, d, 2);
        free(d);
        if (rc < 0) goto fail;
    }
    free(sb);
    printf("%u inodes\n%llu blocks\nFirstdatazone=%u (%u)\nZonesize=%d\nMaxsize=%u\n\n", inodes, (unsigned long long)total, first_data, first_data, MINIX_BLOCK, max_size);
    return 0;
fail:
    free(sb);
    fprintf(stderr, "mkfs.minix: write failed on /dev/%s: %s\n", dev, err_str(rc));
    return 1;
}

/* ====================================================================
 *                               mkfs.vfat
 * ==================================================================== */
static int cmd_mkfs_vfat(int argc, char **argv) {
    int fat = 0, verbose = 0; const char *label = NULL, *dev = NULL; uint32_t spc = 0, reserved = 0, nfats = 2, root_entries = 0; uint64_t blocks_arg = 0;
    uint32_t serial = 0; int have_serial = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-F") && i + 1 < argc) fat = atoi(argv[++i]);
        else if (!strncmp(a, "-F", 2) && a[2]) fat = atoi(a + 2);
        else if (!strcmp(a, "-n") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(a, "-s") && i + 1 < argc) spc = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "-R") && i + 1 < argc) reserved = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "-f") && i + 1 < argc) nfats = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "-r") && i + 1 < argc) root_entries = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "-i") && i + 1 < argc) { serial = (uint32_t)strtoul(argv[++i], NULL, 16); have_serial = 1; }
        else if (!strcmp(a, "-S") || !strcmp(a, "-h") || !strcmp(a, "-M") || !strcmp(a, "-m") || !strcmp(a, "-l") || !strcmp(a, "-b") || !strcmp(a, "-C")) i++;
        else if (!strcmp(a, "-v")) verbose = 1;
        else if (!strcmp(a, "-I") || !strcmp(a, "-c") || !strcmp(a, "-a") || !strcmp(a, "-A") || !strcmp(a, "--invariant")) {}
        else if (!strcmp(a, "--help")) { printf("Usage: mkfs.fat [-a][-A][-c][-C][-v][-I][-l bad-block-file][-b backup-boot-sector]\n       [-m boot-msg-file][-n volume-name][-i volume-id]\n       [-s sectors-per-cluster][-S logical-sector-size][-f number-of-FATs]\n       [-h hidden-sectors][-F fat-size][-r root-dir-entries][-R reserved-sectors]\n       [--invariant]\n       /dev/name [blocks]\n"); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "%s: invalid option -- '%s'\n", g_prog, a + 1); return 1; }
        else if (!dev) dev = dev_name(a); else blocks_arg = strtoull(a, NULL, 10);
    }
    if (!dev) { fprintf(stderr, "No device specified.\nUsage: mkfs.fat [options] /dev/name [blocks]\n"); return 1; }
    printf("mkfs.fat 4.2 (2021-01-31)\n");
    struct abi_blk_info bi;
    if (blk_info(dev, &bi)) { fprintf(stderr, "%s: unable to open /dev/%s: No such file or directory\n", g_prog, dev); return 1; }
    uint64_t total = blocks_arg ? blocks_arg * 2 : bi.sector_count;
    if (total > bi.sector_count) { fprintf(stderr, "%s: blocks exceed device size\n", g_prog); return 1; }
    if (total > 0xFFFFFFFFull) total = 0xFFFFFFFFull;
    if (!fat) fat = total < 8192 ? 12 : total < 1048576 ? 16 : 32;
    if (fat != 12 && fat != 16 && fat != 32) { fprintf(stderr, "%s: FAT size must be 12, 16 or 32\n", g_prog); return 1; }
    if (!reserved) reserved = fat == 32 ? 32 : 1;
    if (!root_entries) root_entries = fat == 32 ? 0 : 512;
    if (fat == 32) root_entries = 0;
    uint32_t root_sectors = (root_entries * 32 + 511) / 512;
    uint32_t max_clusters = fat == 12 ? 4084 : fat == 16 ? 65524 : 0x0FFFFFF4;
    if (!spc) { spc = 1; while (spc < 128 && (total - reserved - root_sectors) / spc > max_clusters) spc *= 2; }
    if (spc > 128 || (spc & (spc - 1))) { fprintf(stderr, "%s: bad number of clusters per sector\n", g_prog); return 1; }
    /* sectors per FAT: iterate until stable */
    uint32_t spf = 1, clusters = 0;
    for (int it = 0; it < 8; it++) {
        uint64_t data = total - reserved - root_sectors - (uint64_t)nfats * spf;
        clusters = (uint32_t)(data / spc);
        uint64_t fat_bytes = fat == 12 ? ((uint64_t)(clusters + 2) * 3 + 1) / 2 : fat == 16 ? (uint64_t)(clusters + 2) * 2 : (uint64_t)(clusters + 2) * 4;
        uint32_t nspf = (uint32_t)((fat_bytes + 511) / 512);
        if (nspf == spf) break;
        spf = nspf;
    }
    if (clusters > max_clusters) { fprintf(stderr, "%s: Too many clusters for FAT%d; use -F %d or a larger cluster size\n", g_prog, fat, fat == 12 ? 16 : 32); return 1; }
    if ((fat == 16 && clusters < 4085) || (fat == 32 && clusters < 65525)) {
        fprintf(stderr, "WARNING: Not enough clusters for a %d bit FAT! The filesystem will be\nmisinterpreted as having a %d bit FAT without mount option \"fat=%d\".\n", fat, fat == 16 ? 12 : 16, fat);
    }
    if (!have_serial) serial = rnd32();
    uint8_t *bs = calloc(1, 512);
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90; memcpy(bs + 3, "mkfs.fat", 8);
    put16(bs + 11, 512); bs[13] = (uint8_t)spc; put16(bs + 14, reserved); bs[16] = (uint8_t)nfats;
    put16(bs + 17, root_entries);
    put16(bs + 19, total < 65536 ? (uint32_t)total : 0);
    bs[21] = 0xF8;
    put16(bs + 22, fat == 32 ? 0 : spf);
    put16(bs + 24, 63); put16(bs + 26, 255); put32(bs + 28, 0);
    put32(bs + 32, total < 65536 ? 0 : (uint32_t)total);
    char lab[12]; memset(lab, ' ', 11); lab[11] = '\0';
    if (label) { size_t l = strlen(label); if (l > 11) l = 11; for (size_t k = 0; k < l; k++) lab[k] = (char)toupper((unsigned char)label[k]); }
    else memcpy(lab, "NO NAME    ", 11);
    if (fat == 32) {
        put32(bs + 36, spf); put16(bs + 40, 0); put16(bs + 42, 0); put32(bs + 44, 2); put16(bs + 48, 1); put16(bs + 50, 6);
        bs[64] = 0x80; bs[65] = 0; bs[66] = 0x29; put32(bs + 67, serial); memcpy(bs + 71, lab, 11); memcpy(bs + 82, "FAT32   ", 8);
    } else {
        bs[36] = 0x80; bs[37] = 0; bs[38] = 0x29; put32(bs + 39, serial); memcpy(bs + 43, lab, 11); memcpy(bs + 54, fat == 12 ? "FAT12   " : "FAT16   ", 8);
    }
    bs[510] = 0x55; bs[511] = 0xAA;
    if (fat == 32) { bs[0x5A] = 0xCD; bs[0x5B] = 0x19; } /* stub: int 19h */
    long rc;
    uint64_t meta = reserved + (uint64_t)nfats * spf + root_sectors + (fat == 32 ? spc : 0);
    rc = blk_zero(dev, 0, meta);
    if (rc < 0) goto fail;
    rc = blk_write(dev, 0, bs, 1);
    if (rc < 0) goto fail;
    if (fat == 32) {
        uint8_t *fsi = calloc(1, 512);
        put32(fsi, 0x41615252); put32(fsi + 484, 0x61417272); put32(fsi + 488, clusters - 1); put32(fsi + 492, 3); fsi[510] = 0x55; fsi[511] = 0xAA;
        rc = blk_write(dev, 1, fsi, 1);
        if (rc >= 0) rc = blk_write(dev, 6, bs, 1);
        if (rc >= 0) rc = blk_write(dev, 7, fsi, 1);
        free(fsi);
        if (rc < 0) goto fail;
    }
    /* first FAT sector: entries 0, 1 (and 2 = root dir EOC for FAT32) */
    {
        uint8_t *f0 = calloc(1, 512);
        if (fat == 12) { f0[0] = 0xF8; f0[1] = 0xFF; f0[2] = 0xFF; }
        else if (fat == 16) { put16(f0, 0xFFF8); put16(f0 + 2, 0xFFFF); }
        else { put32(f0, 0x0FFFFFF8); put32(f0 + 4, 0x0FFFFFFF); put32(f0 + 8, 0x0FFFFFFF); }
        for (uint32_t n = 0; n < nfats; n++) { rc = blk_write(dev, reserved + (uint64_t)n * spf, f0, 1); if (rc < 0) break; }
        free(f0);
        if (rc < 0) goto fail;
    }
    /* root directory: volume label entry */
    if (label) {
        uint8_t *rd = calloc(1, 512);
        memcpy(rd, lab, 11); rd[11] = 0x08;
        uint64_t root_lba = reserved + (uint64_t)nfats * spf;
        rc = blk_write(dev, root_lba, rd, 1);
        free(rd);
        if (rc < 0) goto fail;
    }
    free(bs);
    if (verbose) printf("/dev/%s has %u heads and %u sectors per track,\nlogical sector size is 512,\nusing 0x%02x media descriptor, with %llu sectors;\ndrive number 0x80;\nfilesystem has %u %d-bit FATs and %u sector%s per cluster.\nFAT size is %u sector%s, and provides %u clusters.\n%u reserved sector%s.\nRoot directory contains %u slots and uses %u sectors.\nVolume ID is %08x, %s.\n",
                        dev, 255, 63, 0xF8, (unsigned long long)total, nfats, fat, spc, spc == 1 ? "" : "s", spf, spf == 1 ? "" : "s", clusters, reserved, reserved == 1 ? "" : "s", root_entries, root_sectors, serial, label ? "volume label set" : "no volume label");
    return 0;
fail:
    free(bs);
    fprintf(stderr, "%s: unable to write /dev/%s: %s\n", g_prog, dev, err_str(rc));
    return 1;
}

/* ====================================================================
 *                                 fsck
 * ==================================================================== */
static int cmd_fsck_vfat(int argc, char **argv) {
    const char *dev = NULL; int verbose = 0;
    for (int i = 1; i < argc; i++) { if (argv[i][0] == '-') { if (strchr(argv[i], 'v')) verbose = 1; continue; } dev = dev_name(argv[i]); }
    if (!dev) { fprintf(stderr, "usage: fsck.fat [-aAflnprtvVwy] [-d path -d ...] [-u path -u ...] device\n"); return 1; }
    printf("fsck.fat 4.2 (2021-01-31)\n");
    uint8_t bs[512];
    if (blk_read(dev, 0, bs, 1) < 0) { fprintf(stderr, "open: No such file or directory\n"); return 8; }
    int errs = 0;
    if (bs[510] != 0x55 || bs[511] != 0xAA) { printf("Boot sector signature missing\n"); errs++; }
    uint32_t bps = get16(bs + 11), spc = bs[13], reserved = get16(bs + 14), nfats = bs[16], root_entries = get16(bs + 17);
    uint32_t total = get16(bs + 19) ? get16(bs + 19) : get32(bs + 32);
    uint32_t spf = get16(bs + 22) ? get16(bs + 22) : get32(bs + 36);
    if (bps != 512) { printf("Logical sector size is %u bytes; only 512 is supported\n", bps); return 4; }
    if (!spc || (spc & (spc - 1)) || !nfats || !total || !spf) { printf("Invalid BPB (spc=%u fats=%u sectors=%u spf=%u)\n", spc, nfats, total, spf); return 4; }
    uint32_t root_sectors = (root_entries * 32 + 511) / 512;
    uint32_t data_start = reserved + nfats * spf + root_sectors;
    uint32_t clusters = (total - data_start) / spc;
    int fat = get16(bs + 22) == 0 ? 32 : clusters < 4085 ? 12 : 16;
    /* read FAT #1 and compare with the others */
    uint8_t *f1 = malloc((size_t)spf * 512), *fn = malloc((size_t)spf * 512);
    if (blk_read(dev, reserved, f1, spf) < 0) { printf("Read error in FAT\n"); return 4; }
    for (uint32_t n = 1; n < nfats; n++) {
        if (blk_read(dev, reserved + n * spf, fn, spf) < 0 || memcmp(f1, fn, (size_t)spf * 512)) { printf("FATs differ but appear to be intact. Using first FAT.\n"); errs++; break; }
    }
    uint32_t used = 0, files = 0;
    uint32_t (*fat_get)(const uint8_t *, uint32_t) = NULL; (void)fat_get;
#define FAT_GET(c) (fat == 12 ? (((c) & 1) ? get16(f1 + (c) * 3 / 2) >> 4 : get16(f1 + (c) * 3 / 2) & 0xFFF) : fat == 16 ? get16(f1 + (c) * 2) : get32(f1 + (c) * 4) & 0x0FFFFFFF)
    uint32_t eoc_min = fat == 12 ? 0xFF8 : fat == 16 ? 0xFFF8 : 0x0FFFFFF8;
    uint32_t bad = fat == 12 ? 0xFF7 : fat == 16 ? 0xFFF7 : 0x0FFFFFF7;
    uint32_t media = FAT_GET(0) & 0xFF;
    if (media != 0xF8 && media != 0xF0) { printf("FAT media byte 0x%02x is odd\n", media); errs++; }
    for (uint32_t c = 2; c < clusters + 2; c++) {
        uint32_t v = FAT_GET(c);
        if (v == 0) continue;
        used++;
        if (v == bad || v >= eoc_min) continue;
        if (v < 2 || v >= clusters + 2) { printf("Cluster %u out of range (%u > %u). Setting to EOF.\n", c, v, clusters + 1); errs++; }
    }
    /* root dir walk (FAT12/16 root region or FAT32 cluster chain) */
    {
        uint8_t *rd; uint32_t nsec;
        if (fat == 32) { uint32_t rc0 = get32(bs + 44); nsec = spc; rd = malloc((size_t)nsec * 512); if (blk_read(dev, data_start + (rc0 - 2) * spc, rd, nsec) < 0) nsec = 0; }
        else { nsec = root_sectors; rd = malloc((size_t)nsec * 512); if (nsec && blk_read(dev, reserved + nfats * spf, rd, nsec) < 0) nsec = 0; }
        for (uint32_t off = 0; off + 32 <= nsec * 512; off += 32) {
            const uint8_t *e = rd + off;
            if (e[0] == 0) break;
            if (e[0] == 0xE5 || (e[11] & 0x0F) == 0x0F || (e[11] & 0x08)) continue;
            files++;
            uint32_t start = get16(e + 26) | (fat == 32 ? get16(e + 20) << 16 : 0);
            if (start && (start < 2 || start >= clusters + 2)) { printf("Directory entry %u: start cluster %u out of range\n", off / 32, start); errs++; }
            else if (start) {
                uint32_t c = start, hops = 0;
                while (c >= 2 && c < clusters + 2 && hops++ <= clusters) { uint32_t v = FAT_GET(c); if (v >= eoc_min || v == bad) break; c = v; }
                if (hops > clusters) { printf("Cluster chain starting at %u loops\n", start); errs++; }
            }
        }
        free(rd);
    }
#undef FAT_GET
    free(f1); free(fn);
    if (verbose) printf("Checking for unused clusters.\n");
    printf("/dev/%s: %u files, %u/%u clusters\n", dev, files, used, clusters);
    return errs ? 4 : 0;
}

static int cmd_fsck_minix(int argc, char **argv) {
    const char *dev = NULL;
    for (int i = 1; i < argc; i++) { if (argv[i][0] == '-') continue; dev = dev_name(argv[i]); }
    if (!dev) { fprintf(stderr, "Usage:\n fsck.minix [options] <device>\n"); return 16; }
    printf("fsck.minix from util-linux 2.39 (tobyOS)\n");
    uint8_t sb[1024];
    if (blk_read(dev, 2, sb, 2) < 0) { fprintf(stderr, "fsck.minix: unable to read super block\n"); return 8; }
    uint32_t magic = get16(sb + 16), magic3 = get16(sb + 24), version;
    uint32_t inodes, zones, imap, zmap, firstzone, namelen;
    if (magic == 0x137F || magic == 0x138F) { version = 1; namelen = magic == 0x137F ? 14 : 30; inodes = get16(sb); zones = get16(sb + 2); imap = get16(sb + 4); zmap = get16(sb + 6); firstzone = get16(sb + 8); }
    else if (magic == 0x2468 || magic == 0x2478) { version = 2; namelen = magic == 0x2468 ? 14 : 30; inodes = get16(sb); zones = get32(sb + 20); imap = get16(sb + 4); zmap = get16(sb + 6); firstzone = get16(sb + 8); }
    else if (magic3 == 0x4D5A) { version = 3; namelen = 60; inodes = get32(sb); zones = get32(sb + 20); imap = get16(sb + 6); zmap = get16(sb + 8); firstzone = get16(sb + 10); }
    else { fprintf(stderr, "fsck.minix: bad magic number in super-block\n"); return 8; }
    int errs = 0;
    uint32_t inode_size = version == 1 ? 32 : 64, ipb = 1024 / inode_size;
    uint32_t itable_blocks = (inodes + ipb - 1) / ipb;
    if (2 + imap + zmap + itable_blocks != firstzone) { printf("bad s_firstdatazone (%u, expected %u)\n", firstzone, 2 + imap + zmap + itable_blocks); errs++; }
    if (version < 3 && get16(sb + 18) != 1) printf("Filesystem state is not clean (%u)\n", get16(sb + 18));
    uint8_t *im = malloc((size_t)imap * 1024), *zm = malloc((size_t)zmap * 1024), *it = malloc((size_t)itable_blocks * 1024);
    if (blk_read(dev, 4, im, imap * 2) < 0 || blk_read(dev, (2 + imap) * 2, zm, zmap * 2) < 0 || blk_read(dev, (2 + imap + zmap) * 2, it, itable_blocks * 2) < 0) { fprintf(stderr, "fsck.minix: read error\n"); return 8; }
    if (!(im[0] & 1)) { printf("Inode map: bit 0 not set\n"); errs++; }
    if (!(zm[0] & 1)) { printf("Zone map: bit 0 not set\n"); errs++; }
    /* root inode */
    const uint8_t *root = it;
    uint32_t rmode = get16(root);
    if ((rmode & 0170000) != 040000) { printf("Root inode isn't a directory.\n"); errs++; }
    if (!(im[0] & 2)) { printf("Root inode not marked in inode map\n"); errs++; }
    /* count used inodes/zones from the maps and verify each used inode's zones */
    uint32_t used_i = 0, used_z = 0;
    for (uint32_t i = 1; i <= inodes; i++) if (im[i / 8] & (1u << (i % 8))) used_i++;
    uint32_t nz = zones - firstzone;
    for (uint32_t z = 1; z <= nz; z++) if (zm[z / 8] & (1u << (z % 8))) used_z++;
    uint32_t seen_z = 0;
    for (uint32_t i = 1; i <= inodes; i++) {
        if (!(im[i / 8] & (1u << (i % 8)))) continue;
        const uint8_t *in = it + (i - 1) * inode_size;
        uint32_t mode = get16(in);
        uint32_t nlinks = version == 1 ? in[13] : get16(in + 2);
        if (!mode && !nlinks) { printf("Inode %u marked unused, but used for file...\n", i); }
        int ndirect = version == 1 ? 7 : 7;
        for (int k = 0; k < ndirect; k++) {
            uint32_t z = version == 1 ? get16(in + 14 + k * 2) : get32(in + 24 + k * 4);
            if (!z) continue;
            if (z < firstzone || z >= zones) { printf("Inode %u: zone %u out of range\n", i, z); errs++; continue; }
            uint32_t bit = z - firstzone + 1;
            if (!(zm[bit / 8] & (1u << (bit % 8)))) { printf("Zone %u: not marked in use in zone map\n", z); errs++; }
            seen_z++;
        }
    }
    (void)seen_z; (void)namelen;
    free(im); free(zm); free(it);
    printf("\n%u inodes used (%u%%)\n%u zones used (%u%%)\n", used_i, inodes ? used_i * 100 / inodes : 0, used_z, nz ? used_z * 100 / nz : 0);
    return errs ? 4 : 0;
}

static int cmd_fsck(int argc, char **argv) {
    const char *type = NULL, *dev = NULL;
    char *pass[64]; int np = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-t") && i + 1 < argc) { type = argv[++i]; continue; }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("fsck from util-linux 2.39 (tobyOS)\n"); return 0; }
        if (!strcmp(a, "-N") || !strcmp(a, "-A") || !strcmp(a, "-R") || !strcmp(a, "-T") || !strcmp(a, "-M") || !strcmp(a, "-P") || !strcmp(a, "-s") || !strcmp(a, "-l")) continue;
        if (a[0] == '-') { if (np < 60) pass[np++] = (char *)a; continue; }
        dev = a;
    }
    if (!dev) { fprintf(stderr, "Usage:\n fsck [options] -- [fs-options] [<filesystem> ...]\n"); return 16; }
    printf("fsck from util-linux 2.39 (tobyOS)\n");
    char tbuf[16];
    if (!type) {
        struct probe pr;
        if (!probe_dev(dev_name(dev), &pr)) { fprintf(stderr, "fsck: %s: no filesystem found\n", dev); return 8; }
        snprintf(tbuf, sizeof tbuf, "%s", pr.type); type = tbuf;
    }
    char prog[64]; snprintf(prog, sizeof prog, "fsck.%s", type);
    char *av[72]; int n = 0; av[n++] = prog;
    for (int k = 0; k < np; k++) av[n++] = pass[k];
    av[n++] = (char *)dev; av[n] = NULL;
    fflush(stdout);
    execvp(prog, av);
    fprintf(stderr, "fsck: error 2 (No such file or directory) while executing %s for %s\n", prog, dev);
    return 8;
}

static int cmd_fsck_ext(int argc, char **argv) {
    /* e2fsck (user_e2fs) is the checker; fsck.ext[234] are its util-linux
     * names. Pass everything through. */
    char *av[72]; int n = 0; av[n++] = (char *)"e2fsck";
    for (int i = 1; i < argc && n < 70; i++) av[n++] = argv[i];
    av[n] = NULL;
    execvp("e2fsck", av);
    fprintf(stderr, "%s: cannot execute e2fsck: %s\n", g_prog, strerror(errno));
    return 8;
}

/* ====================================================================
 *                                tune2fs
 * ==================================================================== */
static int cmd_tune2fs(int argc, char **argv) {
    const char *dev = NULL, *label = NULL, *errors = NULL, *uuid_opt = NULL;
    int list = 0, set_c = 0, set_C = 0, set_i = 0, set_m = 0, set_r = 0;
    long max_mounts = 0, mount_count = 0, interval = 0, reserved_pct = 0, reserved_blocks = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-l")) list = 1;
        else if (!strcmp(a, "-L") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(a, "-c") && i + 1 < argc) { set_c = 1; max_mounts = atol(argv[++i]); }
        else if (!strcmp(a, "-C") && i + 1 < argc) { set_C = 1; mount_count = atol(argv[++i]); }
        else if (!strcmp(a, "-i") && i + 1 < argc) { set_i = 1; const char *v = argv[++i]; char *e; interval = strtol(v, &e, 10); if (*e == 'd' || !*e) interval *= 86400; else if (*e == 'w') interval *= 7 * 86400; else if (*e == 'm') interval *= 30 * 86400; }
        else if (!strcmp(a, "-e") && i + 1 < argc) errors = argv[++i];
        else if (!strcmp(a, "-m") && i + 1 < argc) { set_m = 1; reserved_pct = atol(argv[++i]); }
        else if (!strcmp(a, "-r") && i + 1 < argc) { set_r = 1; reserved_blocks = atol(argv[++i]); }
        else if (!strcmp(a, "-U") && i + 1 < argc) uuid_opt = argv[++i];
        else if (!strcmp(a, "-o") || !strcmp(a, "-O") || !strcmp(a, "-E") || !strcmp(a, "-g") || !strcmp(a, "-u") || !strcmp(a, "-T") || !strcmp(a, "-M") || !strcmp(a, "-I") || !strcmp(a, "-Q") || !strcmp(a, "-z")) i++;
        else if (!strcmp(a, "-f") || !strcmp(a, "-j")) {}
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("tune2fs 1.47.0 (tobyOS)\n"); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "tune2fs: invalid option -- '%s'\nUsage: tune2fs [-c max_mounts_count] [-e errors_behavior] [-i interval[d|m|w]] [-l] [-L volume_label] [-m reserved_blocks_percent] [-r reserved_blocks_count] [-C mount_count] [-U UUID] device\n", a + 1); return 1; }
        else dev = dev_name(a);
    }
    if (!dev) { fprintf(stderr, "Usage: tune2fs [-c max_mounts_count] [-e errors_behavior] [-i interval[d|m|w]] [-l] [-L volume_label] [-m reserved_blocks_percent] [-r reserved_blocks_count] [-C mount_count] [-U UUID] device\n"); return 1; }
    printf("tune2fs 1.47.0 (tobyOS)\n");
    uint8_t sb[1024];
    if (blk_read(dev, 2, sb, 2) < 0) { fprintf(stderr, "tune2fs: No such file or directory while trying to open /dev/%s\nCouldn't find valid filesystem superblock.\n", dev); return 1; }
    if (get16(sb + 0x38) != 0xEF53) { fprintf(stderr, "tune2fs: Bad magic number in super-block while trying to open /dev/%s\nCouldn't find valid filesystem superblock.\n", dev); return 1; }
    int changed = 0;
    if (label) { memset(sb + 0x78, 0, 16); size_t l = strlen(label); if (l > 16) { fprintf(stderr, "Warning: label too long, truncating.\n"); l = 16; } memcpy(sb + 0x78, label, l); changed = 1; }
    if (set_c) { uint32_t v = max_mounts < 0 ? 0xFFFF : (uint32_t)max_mounts; put16(sb + 0x36, v); printf("Setting maximal mount count to %ld\n", max_mounts < 0 ? -1L : max_mounts); changed = 1; }
    if (set_C) { put16(sb + 0x34, (uint32_t)mount_count); printf("Setting current mount count to %ld\n", mount_count); changed = 1; }
    if (set_i) { put32(sb + 0x44, (uint32_t)interval); printf("Setting interval between checks to %ld seconds\n", interval); changed = 1; }
    if (errors) { uint32_t v = !strcmp(errors, "continue") ? 1 : !strcmp(errors, "remount-ro") ? 2 : !strcmp(errors, "panic") ? 3 : 0; if (!v) { fprintf(stderr, "tune2fs: bad error behavior - %s\n", errors); return 1; } put16(sb + 0x3C, v); printf("Setting error behavior to %u\n", v); changed = 1; }
    if (set_m) { uint32_t blocks = get32(sb + 0x04); uint32_t r = (uint32_t)((uint64_t)blocks * (uint64_t)reserved_pct / 100); put32(sb + 0x08, r); printf("Setting reserved blocks percentage to %ld%% (%u blocks)\n", reserved_pct, r); changed = 1; }
    if (set_r) { put32(sb + 0x08, (uint32_t)reserved_blocks); printf("Setting reserved blocks count to %ld\n", reserved_blocks); changed = 1; }
    if (uuid_opt) {
        uint8_t u[16];
        if (!strcmp(uuid_opt, "clear")) memset(u, 0, 16);
        else if (!strcmp(uuid_opt, "random") || !strcmp(uuid_opt, "time")) { for (int k = 0; k < 16; k += 4) put32(u + k, rnd32()); u[6] = (uint8_t)((u[6] & 0x0F) | 0x40); u[8] = (uint8_t)((u[8] & 0x3F) | 0x80); }
        else {
            const char *p = uuid_opt; int k = 0;
            while (*p && k < 16) { if (*p == '-') { p++; continue; } if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) break; char h[3] = { p[0], p[1], 0 }; u[k++] = (uint8_t)strtoul(h, NULL, 16); p += 2; }
            if (k != 16 || *p) { fprintf(stderr, "tune2fs: Invalid UUID format\n"); return 1; }
        }
        memcpy(sb + 0x68, u, 16); changed = 1;
    }
    if (changed) {
        put32(sb + 0x30, (uint32_t)time(NULL));
        long rc = blk_write(dev, 2, sb, 2);
        if (rc < 0) { fprintf(stderr, "tune2fs: %s while writing superblock to /dev/%s\n", err_str(rc), dev); return 1; }
        /* backups: every group when sparse_super is off; 0,1,3^n,5^n,7^n otherwise */
        uint32_t bsize = 1024u << get32(sb + 0x18), bpg = get32(sb + 0x20), first = get32(sb + 0x14), blocks = get32(sb + 0x04);
        uint32_t groups = (blocks - first + bpg - 1) / bpg, ro = get32(sb + 0x64);
        for (uint32_t g = 1; g < groups; g++) {
            int has = 1;
            if (ro & 0x1) { has = 0; if (g == 1) has = 1; for (uint32_t p = 3; p <= g; p *= 3) if (p == g) has = 1; for (uint32_t p = 5; p <= g; p *= 5) if (p == g) has = 1; for (uint32_t p = 7; p <= g; p *= 7) if (p == g) has = 1; }
            if (!has) continue;
            put16(sb + 0x5A, (uint16_t)g);
            uint64_t lba = ((uint64_t)first + (uint64_t)g * bpg) * (bsize / 512);
            blk_write(dev, lba, sb, 2);
        }
    }
    if (list) {
        char us[40]; uuid_fmt(sb + 0x68, us);
        char lab[17]; memcpy(lab, sb + 0x78, 16); lab[16] = '\0';
        uint32_t incompat = get32(sb + 0x60), compat = get32(sb + 0x5C), ro = get32(sb + 0x64);
        printf("Filesystem volume name:   %s\n", lab[0] ? lab : "<none>");
        printf("Last mounted on:          <not available>\n");
        printf("Filesystem UUID:          %s\n", us);
        printf("Filesystem magic number:  0x%04X\n", get16(sb + 0x38));
        printf("Filesystem revision #:    %u (%s)\n", get32(sb + 0x4C), get32(sb + 0x4C) ? "dynamic" : "original");
        printf("Filesystem features:     %s%s%s%s%s\n", compat & 0x4 ? " has_journal" : "", incompat & 0x2 ? " filetype" : "", ro & 0x1 ? " sparse_super" : "", incompat & 0x40 ? " extents" : "", ro & 0x2 ? " large_file" : "");
        printf("Filesystem flags:         signed_directory_hash \n");
        printf("Default mount options:    (none)\n");
        printf("Filesystem state:         %s\n", get16(sb + 0x3A) & 1 ? "clean" : "not clean");
        printf("Errors behavior:          %s\n", get16(sb + 0x3C) == 2 ? "Remount read-only" : get16(sb + 0x3C) == 3 ? "Panic" : "Continue");
        printf("Filesystem OS type:       Linux\n");
        printf("Inode count:              %u\n", get32(sb + 0x00));
        printf("Block count:              %u\n", get32(sb + 0x04));
        printf("Reserved block count:     %u\n", get32(sb + 0x08));
        printf("Free blocks:              %u\n", get32(sb + 0x0C));
        printf("Free inodes:              %u\n", get32(sb + 0x10));
        printf("First block:              %u\n", get32(sb + 0x14));
        printf("Block size:               %u\n", 1024u << get32(sb + 0x18));
        printf("Fragment size:            %u\n", 1024u << get32(sb + 0x1C));
        printf("Blocks per group:         %u\n", get32(sb + 0x20));
        printf("Fragments per group:      %u\n", get32(sb + 0x24));
        printf("Inodes per group:         %u\n", get32(sb + 0x28));
        printf("Inode blocks per group:   %u\n", get32(sb + 0x28) * get16(sb + 0x58) / (1024u << get32(sb + 0x18)));
        printf("Mount count:              %u\n", get16(sb + 0x34));
        printf("Maximum mount count:      %d\n", get16(sb + 0x36) == 0xFFFF ? -1 : (int)get16(sb + 0x36));
        {
            uint32_t iv = get32(sb + 0x44); char ivs[40];
            if (!iv) snprintf(ivs, sizeof ivs, "<none>");
            else if (iv % 604800 == 0) snprintf(ivs, sizeof ivs, "%u week%s", iv / 604800, iv / 604800 == 1 ? "" : "s");
            else if (iv % 86400 == 0) snprintf(ivs, sizeof ivs, "%u day%s", iv / 86400, iv / 86400 == 1 ? "" : "s");
            else snprintf(ivs, sizeof ivs, "%u seconds", iv);
            printf("Check interval:           %u (%s)\n", iv, ivs);
        }
        printf("Reserved blocks uid:      %u\n", get16(sb + 0x50));
        printf("Reserved blocks gid:      %u\n", get16(sb + 0x52));
        printf("First inode:              %u\n", get32(sb + 0x54));
        printf("Inode size:               %u\n", get16(sb + 0x58));
    }
    return 0;
}

/* ============================================================== dispatch */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet g_applets[] = {
    { "insmod",     cmd_insmod },
    { "rmmod",      cmd_rmmod },
    { "lsmod",      cmd_lsmod },
    { "modprobe",   cmd_modprobe },
    { "blkid",      cmd_blkid },
    { "fsck",       cmd_fsck },
    { "fsck.ext2",  cmd_fsck_ext },
    { "fsck.ext3",  cmd_fsck_ext },
    { "fsck.ext4",  cmd_fsck_ext },
    { "fsck.vfat",  cmd_fsck_vfat },
    { "fsck.fat",   cmd_fsck_vfat },
    { "fsck.msdos", cmd_fsck_vfat },
    { "dosfsck",    cmd_fsck_vfat },
    { "fsck.minix", cmd_fsck_minix },
    { "mkfs.ext2",  cmd_mkfs_ext2 },
    { "mke2fs",     cmd_mkfs_ext2 },
    { "mkfs.minix", cmd_mkfs_minix },
    { "mkfs.vfat",  cmd_mkfs_vfat },
    { "mkfs.fat",   cmd_mkfs_vfat },
    { "mkfs.msdos", cmd_mkfs_vfat },
    { "mkdosfs",    cmd_mkfs_vfat },
    { "tune2fs",    cmd_tune2fs },
    { 0, 0 }
};

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "sysutils";
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
    fprintf(stderr, "sysutils: applets:");
    for (int i = 0; g_applets[i].name; i++) fprintf(stderr, " %s", g_applets[i].name);
    fprintf(stderr, "\n");
    return 1;
}
