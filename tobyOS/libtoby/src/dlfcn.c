/* dlfcn.c -- Dynamic loading API for libtoby (Phase 3 M3.5).
 *
 * Implements the POSIX dlfcn.h interface:
 *   - dlopen(filename, flags)  -- load a shared library
 *   - dlsym(handle, symbol)    -- look up a symbol by name
 *   - dlclose(handle)          -- unload a library
 *   - dlerror()                -- get last error message
 *
 * Uses the kernel's sys_dload syscall to map libraries and the
 * dynamic linker's symbol table for resolution.
 */

#include "libtoby_internal.h"
#include <string.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

/* dlfcn flags */
#define RTLD_LAZY    0x0001
#define RTLD_NOW     0x0002
#define RTLD_GLOBAL  0x0100
#define RTLD_LOCAL   0x0000

/* ELF types needed for symbol lookup */
typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} Elf64_Dyn;

#define DT_NULL     0
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_STRSZ    10
#define DT_HASH     4

#define ELF64_ST_BIND(i)  ((i) >> 4)
#define STB_GLOBAL  1
#define STB_WEAK    2

/* Loaded library handle */
#define MAX_LOADED_LIBS 16

struct dl_handle {
    uint64_t base;           /* load base address */
    const Elf64_Dyn *dyn;   /* _DYNAMIC section */
    const char *strtab;
    const Elf64_Sym *symtab;
    const uint32_t *hash;    /* SysV hash table */
    int refcount;
    char path[128];
    int used;
};

static struct dl_handle g_libs[MAX_LOADED_LIBS];
static char g_dlerror[128];
static int g_has_error;

/* ---- SysV hash ---- */

static uint32_t elf_hash(const char *name) {
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        g = h & 0xF0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static const Elf64_Sym *hash_lookup(const struct dl_handle *lib,
                                     const char *name) {
    if (!lib->hash || !lib->symtab || !lib->strtab) return NULL;

    uint32_t nbucket = lib->hash[0];
    /* uint32_t nchain = lib->hash[1]; */
    const uint32_t *bucket = &lib->hash[2];
    const uint32_t *chain = &lib->hash[2 + nbucket];

    uint32_t h = elf_hash(name) % nbucket;
    for (uint32_t i = bucket[h]; i != 0; i = chain[i]) {
        const Elf64_Sym *sym = &lib->symtab[i];
        if (sym->st_shndx == 0) continue; /* undefined */
        uint8_t bind = ELF64_ST_BIND(sym->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        if (strcmp(lib->strtab + sym->st_name, name) == 0)
            return sym;
    }
    return NULL;
}

/* ---- Parse _DYNAMIC ---- */

static void parse_dynamic(struct dl_handle *h) {
    if (!h->dyn) return;
    for (const Elf64_Dyn *d = h->dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB:  h->strtab = (const char *)(h->base + d->d_val); break;
        case DT_SYMTAB:  h->symtab = (const Elf64_Sym *)(h->base + d->d_val); break;
        case DT_HASH:    h->hash = (const uint32_t *)(h->base + d->d_val); break;
        default: break;
        }
    }
}

/* ---- Public API ---- */

/* ---- LD_LIBRARY_PATH search for dlopen ---- */

static char g_dlopen_path[128];

static long try_dload(const char *path) {
    return toby_sc1(ABI_SYS_DLOAD, (long)path);
}

static long resolve_and_load(const char *filename) {
    size_t flen = strlen(filename);

    /* Absolute path: try directly */
    if (filename[0] == '/') {
        return try_dload(filename);
    }

    /* Check LD_LIBRARY_PATH via SYS_GETENV */
    char ld_path[256];
    long env_rc = toby_sc3(46 /* ABI_SYS_GETENV */,
                           (long)"LD_LIBRARY_PATH",
                           (long)ld_path, sizeof(ld_path));
    if (env_rc > 0) {
        /* Split on ':' and try each directory */
        char *s = ld_path;
        while (*s) {
            char *start = s;
            while (*s && *s != ':') s++;
            char saved = *s;
            *s = '\0';
            size_t dlen = (size_t)(s - start);
            if (dlen + 1 + flen + 1 <= sizeof(g_dlopen_path)) {
                memcpy(g_dlopen_path, start, dlen);
                g_dlopen_path[dlen] = '/';
                memcpy(g_dlopen_path + dlen + 1, filename, flen + 1);
                long rv = try_dload(g_dlopen_path);
                if (rv > 0) return rv;
            }
            if (saved == '\0') break;
            s++;
        }
    }

    /* Default: /lib/<filename> */
    memcpy(g_dlopen_path, "/lib/", 5);
    if (flen > 122) flen = 122;
    memcpy(g_dlopen_path + 5, filename, flen + 1);
    return try_dload(g_dlopen_path);
}

void *dlopen(const char *filename, int flags) {
    (void)flags;
    g_has_error = 0;

    if (!filename) {
        memcpy(g_dlerror, "dlopen(NULL) not supported", 27);
        g_has_error = 1;
        return NULL;
    }

    /* Check if already loaded */
    for (int i = 0; i < MAX_LOADED_LIBS; i++) {
        if (g_libs[i].used && strcmp(g_libs[i].path, filename) == 0) {
            g_libs[i].refcount++;
            return &g_libs[i];
        }
    }

    /* Find free slot */
    struct dl_handle *h = NULL;
    for (int i = 0; i < MAX_LOADED_LIBS; i++) {
        if (!g_libs[i].used) { h = &g_libs[i]; break; }
    }
    if (!h) {
        memcpy(g_dlerror, "too many loaded libraries", 26);
        g_has_error = 1;
        return NULL;
    }

    /* Resolve path with LD_LIBRARY_PATH support and load */
    long rv = resolve_and_load(filename);
    if (rv <= 0) {
        memcpy(g_dlerror, "dload failed: ", 14);
        size_t plen = strlen(filename);
        if (plen > 113) plen = 113;
        memcpy(g_dlerror + 14, filename, plen);
        g_dlerror[14 + plen] = '\0';
        g_has_error = 1;
        return NULL;
    }

    memset(h, 0, sizeof(*h));
    h->base = (uint64_t)rv;
    h->used = 1;
    h->refcount = 1;
    size_t plen = strlen(filename);
    if (plen > 127) plen = 127;
    memcpy(h->path, filename, plen);
    h->path[plen] = '\0';

    parse_dynamic(h);
    return h;
}

/* RTLD_DEFAULT / RTLD_NEXT sentinel */
#define RTLD_DEFAULT  ((void *)0)
#define RTLD_NEXT     ((void *)-1L)

void *dlsym(void *handle, const char *symbol) {
    g_has_error = 0;
    if (!symbol) {
        memcpy(g_dlerror, "invalid argument", 17);
        g_has_error = 1;
        return NULL;
    }

    /* If handle is NULL (RTLD_DEFAULT), search all loaded libs */
    if (!handle) {
        for (int i = 0; i < MAX_LOADED_LIBS; i++) {
            if (!g_libs[i].used) continue;
            const Elf64_Sym *sym = hash_lookup(&g_libs[i], symbol);
            if (sym && sym->st_value != 0)
                return (void *)(g_libs[i].base + sym->st_value);
        }
        size_t slen = strlen(symbol);
        memcpy(g_dlerror, "undefined symbol: ", 18);
        if (slen > 109) slen = 109;
        memcpy(g_dlerror + 18, symbol, slen);
        g_dlerror[18 + slen] = '\0';
        g_has_error = 1;
        return NULL;
    }

    struct dl_handle *h = (struct dl_handle *)handle;
    const Elf64_Sym *sym = hash_lookup(h, symbol);
    if (sym && sym->st_value != 0)
        return (void *)(h->base + sym->st_value);

    /* Fallback: search all loaded libraries for the symbol */
    for (int i = 0; i < MAX_LOADED_LIBS; i++) {
        if (!g_libs[i].used || &g_libs[i] == h) continue;
        sym = hash_lookup(&g_libs[i], symbol);
        if (sym && sym->st_value != 0)
            return (void *)(g_libs[i].base + sym->st_value);
    }

    size_t slen = strlen(symbol);
    memcpy(g_dlerror, "undefined symbol: ", 18);
    if (slen > 109) slen = 109;
    memcpy(g_dlerror + 18, symbol, slen);
    g_dlerror[18 + slen] = '\0';
    g_has_error = 1;
    return NULL;
}

int dlclose(void *handle) {
    g_has_error = 0;
    if (!handle) return -1;

    struct dl_handle *h = (struct dl_handle *)handle;
    if (!h->used) return -1;

    h->refcount--;
    if (h->refcount <= 0) {
        /* TODO: unmap the library memory */
        h->used = 0;
    }
    return 0;
}

char *dlerror(void) {
    if (!g_has_error) return NULL;
    g_has_error = 0;
    return g_dlerror;
}
