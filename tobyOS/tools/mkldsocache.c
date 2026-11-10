/* mkldsocache -- generate a modern glibc ld.so.cache at initrd build time
 * (Phase G, 2026-08-22). Host tool, built with HOST_CC like mkfs_tobyfs
 * (a python version existed for one commit and silently never ran: the
 * gate's build shell has no python on PATH, and a generator that only
 * works in SOME shells is the sometimes-lie this tree hunts).
 *
 * Format: "glibc-ld.so.cache" + "1.1", new-format-only -- what every
 * distro ships since glibc 2.32 and what Bootlin's glibc 2.41 ld.so
 * reads. Why it matters: without a cache, every glibc-DYNAMIC spawn
 * needed an explicit LD_LIBRARY_PATH (eight call sites in kernel.c
 * carry the workaround), because the sysroot's baked-in default dirs
 * don't cover the initrd's layout for every library.
 *
 * Layout facts (glibc dl-cache.h / cache.c):
 *   header (48 B): magic[17] + version[3], u32 nlibs, u32 len_strings,
 *     u8 flags (3 = little-endian), u8 pad[3], u32 extension_offset (0),
 *     u32 unused[3]
 *   entry (24 B): s32 flags (0x0303 = FLAG_ELF_LIBC6|FLAG_X8664_LIB64),
 *     u32 key, u32 value (string offsets RELATIVE TO FILE START),
 *     u32 osversion (0), u64 hwcap (0)
 *   ld.so BINARY-SEARCHES entries with _dl_cache_libcmp and ldconfig
 *   sorts DESCENDING in that ordering -- an unsorted cache resolves only
 *   the names that happen to sit on probe points. libcmp is reproduced
 *   below, numeric runs and all.
 *
 * Usage: mkldsocache <initrd-stage-dir> <output-file>
 * Exits 0 on success (prints a summary), 1 when no libraries were found
 * (the Makefile then skips staging an empty cache). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAX_LIBS 4096
#define FLAG_ENTRY 0x0303

static const char *DIRS[] = {
    "lib", "lib64", "usr/lib", "usr/lib64",
    "opt/chrome", "opt/chrome/sysroot", 0
};

struct lib { char name[256]; char path[512]; };
static struct lib g_libs[MAX_LIBS];
static int g_nlibs;

/* glibc _dl_cache_libcmp: bytewise, except digit runs compare numerically. */
static int libcmp(const char *p1, const char *p2) {
    while (*p1 != '\0') {
        if (*p1 >= '0' && *p1 <= '9') {
            if (*p2 >= '0' && *p2 <= '9') {
                int v1 = *p1++ - '0';
                int v2 = *p2++ - '0';
                while (*p1 >= '0' && *p1 <= '9') v1 = v1 * 10 + *p1++ - '0';
                while (*p2 >= '0' && *p2 <= '9') v2 = v2 * 10 + *p2++ - '0';
                if (v1 != v2) return v1 - v2;
            } else
                return 1;
        } else if (*p2 >= '0' && *p2 <= '9')
            return -1;
        else if (*p1 != *p2)
            return (unsigned char)*p1 - (unsigned char)*p2;
        else {
            ++p1; ++p2;
        }
    }
    return -(unsigned char)*p2;
}

/* qsort comparator: DESCENDING libcmp order (what ldconfig writes and
 * what ld.so's binary search assumes). */
static int entcmp(const void *a, const void *b) {
    return libcmp(((const struct lib *)b)->name,
                  ((const struct lib *)a)->name);
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: mkldsocache <stage-dir> <out>\n");
        return 2;
    }
    const char *stage = argv[1], *out = argv[2];

    for (int d = 0; DIRS[d]; d++) {
        char full[600];
        snprintf(full, sizeof full, "%s/%s", stage, DIRS[d]);
        DIR *dp = opendir(full);
        if (!dp) continue;
        struct dirent *de;
        while ((de = readdir(dp)) != 0 && g_nlibs < MAX_LIBS) {
            if (!strstr(de->d_name, ".so")) continue;
            char p[900];
            snprintf(p, sizeof p, "%s/%s", full, de->d_name);
            struct stat st;
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            struct lib *l = &g_libs[g_nlibs++];
            snprintf(l->name, sizeof l->name, "%s", de->d_name);
            snprintf(l->path, sizeof l->path, "/%s/%s", DIRS[d], de->d_name);
        }
        closedir(dp);
    }
    if (g_nlibs == 0) {
        fprintf(stderr, "[mkldsocache] no libraries found under %s\n", stage);
        return 1;
    }
    qsort(g_libs, (size_t)g_nlibs, sizeof g_libs[0], entcmp);

    uint32_t header_sz = 48, entry_sz = 24;
    uint32_t strings_off = header_sz + entry_sz * (uint32_t)g_nlibs;
    uint32_t len_strings = 0;
    for (int i = 0; i < g_nlibs; i++)
        len_strings += (uint32_t)(strlen(g_libs[i].name) + 1 +
                                  strlen(g_libs[i].path) + 1);

    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 2; }
    fwrite("glibc-ld.so.cache", 1, 17, f);
    fwrite("1.1", 1, 3, f);
    put32(f, (uint32_t)g_nlibs);
    put32(f, len_strings);
    {
        uint8_t fl[4] = { 3, 0, 0, 0 };   /* endian=little + pad */
        fwrite(fl, 1, 4, f);
        put32(f, 0);                       /* extension_offset */
        put32(f, 0); put32(f, 0); put32(f, 0);
    }
    uint32_t off = strings_off;
    for (int i = 0; i < g_nlibs; i++) {
        put32(f, FLAG_ENTRY);
        put32(f, off);                                     /* key   */
        off += (uint32_t)strlen(g_libs[i].name) + 1;
        put32(f, off);                                     /* value */
        off += (uint32_t)strlen(g_libs[i].path) + 1;
        put32(f, 0);                                       /* osversion */
        put64(f, 0);                                       /* hwcap */
    }
    for (int i = 0; i < g_nlibs; i++) {
        fwrite(g_libs[i].name, 1, strlen(g_libs[i].name) + 1, f);
        fwrite(g_libs[i].path, 1, strlen(g_libs[i].path) + 1, f);
    }
    long total = ftell(f);
    fclose(f);
    printf("[mkldsocache] %d libraries -> %s (%ld bytes)\n",
           g_nlibs, out, total);
    return 0;
}
