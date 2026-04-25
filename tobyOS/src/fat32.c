/* fat32.c -- FAT32 filesystem driver (milestone 23B).
 *
 * Hooks straight into the same vfs_ops table that tobyfs and ramfs
 * use. The only thing this file knows about its underlying storage is
 * a `struct blk_dev *`; everything else (whether it's a whole disk, a
 * GPT partition, or eventually a USB LUN) is invisible.
 *
 * Design choices (kept deliberately conservative):
 *   - Sector size MUST be 512 (matches blk layer).
 *   - Cluster sizes 1..16 sectors (512..8192 bytes) are supported.
 *   - One in-memory cluster scratch buffer (allocated at mount).
 *   - One in-memory FAT-sector cache (single-entry write-back).
 *   - Both FAT copies are kept in sync on every mutation.
 *   - LFN reassembly on read; create() emits SHORT-NAME-ONLY entries
 *     using a deterministic 8.3 truncation. Pre-existing LFNs on the
 *     disk are preserved on read; we just don't generate new ones.
 *     Long names round-trip through readdir but the on-disk creation
 *     uses the truncated short name (e.g. "READme.TXT" -> "README~1.TXT").
 *   - mkdir is rejected (VFS_ERR_ROFS) per Milestone 23 scope.
 *
 * Path walking is iterative -- we never recurse on directories. Cluster
 * chains are walked entry-by-entry with a small in-memory cache of the
 * last FAT sector we touched.
 */

#include <tobyos/fat32.h>
#include <tobyos/blk.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

/* ---- in-memory state ---- */

struct fat32 {
    struct blk_dev *dev;

    uint32_t bytes_per_sec;       /* always 512 */
    uint32_t sec_per_clus;        /* 1..16 */
    uint32_t cluster_bytes;       /* sec_per_clus * 512 */
    uint32_t rsvd_sec_cnt;
    uint32_t num_fats;            /* 1 or 2 */
    uint32_t fat_sz_sec;          /* sectors per FAT */
    uint32_t tot_sec;             /* total sectors in the volume */
    uint32_t fat_lba;             /* first FAT sector (= rsvd_sec_cnt) */
    uint32_t data_lba;            /* first data sector (cluster 2 starts here) */
    uint32_t total_data_sec;      /* tot_sec - data_lba */
    uint32_t cluster_count;       /* total_data_sec / sec_per_clus */
    uint32_t root_clus;           /* usually 2 */
    uint32_t fsi_lba;             /* FSInfo sector LBA (0 if none) */

    /* Single-sector FAT scratch (read-modify-write cache, indexed by
     * which FAT sector currently lives in `fat_sec_buf`). */
    uint8_t  fat_sec_buf[512];
    uint32_t fat_sec_idx;         /* relative to start of FAT0; UINT32_MAX = empty */
    bool     fat_sec_dirty;

    /* Cached free hint -- updated lazily, used to speed up alloc. */
    uint32_t next_free_hint;

    /* Cluster-sized scratch buffers (allocated once at mount). */
    uint8_t *clus_buf;            /* cluster_bytes */
    uint8_t *clus_buf2;           /* second scratch (used during pack) */
};

#define FAT32_INVALID_SEC  0xFFFFFFFFu

/* Per-handle state for an open file. */
struct fat32_filepriv {
    uint32_t first_clus;          /* first data cluster (0 if file is empty) */
    uint32_t cur_clus;            /* cluster currently mapped by `pos` */
    uint32_t cur_clus_idx;        /* index of cur_clus within the chain */
    /* Location of the directory entry that owns this file -- needed so
     * we can flush size + first-cluster updates after a write. */
    uint32_t dir_clus;            /* parent directory cluster */
    uint32_t dir_off;             /* byte offset within dir cluster of the SHORT entry */
    uint32_t cluster_in_dir;      /* which cluster inside the chain dir_off lives in */
};

/* Per-handle state for an open dir (we materialise all entries up
 * front, just like tobyfs_opendir). */
struct fat32_diriter {
    struct vfs_dirent *ents;
    size_t             count;
};

/* ---- low-level helpers ---- */

static int read_sec(struct fat32 *fs, uint32_t lba, uint32_t cnt, void *buf) {
    return blk_read(fs->dev, lba, cnt, buf);
}
static int write_sec(struct fat32 *fs, uint32_t lba, uint32_t cnt, const void *buf) {
    return blk_write(fs->dev, lba, cnt, buf);
}

static uint32_t cluster_to_lba(const struct fat32 *fs, uint32_t clus) {
    return fs->data_lba + (clus - 2) * fs->sec_per_clus;
}

static int read_cluster(struct fat32 *fs, uint32_t clus, void *buf) {
    if (clus < 2 || clus >= fs->cluster_count + 2) return VFS_ERR_INVAL;
    return read_sec(fs, cluster_to_lba(fs, clus), fs->sec_per_clus, buf);
}
static int write_cluster(struct fat32 *fs, uint32_t clus, const void *buf) {
    if (clus < 2 || clus >= fs->cluster_count + 2) return VFS_ERR_INVAL;
    return write_sec(fs, cluster_to_lba(fs, clus), fs->sec_per_clus, buf);
}

/* Bring the FAT sector containing entry `clus` into fat_sec_buf.
 * Flushes the previous one to BOTH FATs if it was dirty. */
static int fat_load_sec(struct fat32 *fs, uint32_t clus) {
    uint32_t want = (clus * 4) / fs->bytes_per_sec;
    if (fs->fat_sec_idx == want) return VFS_OK;

    if (fs->fat_sec_dirty && fs->fat_sec_idx != FAT32_INVALID_SEC) {
        for (uint32_t i = 0; i < fs->num_fats; i++) {
            uint32_t lba = fs->fat_lba + i * fs->fat_sz_sec + fs->fat_sec_idx;
            int rc = write_sec(fs, lba, 1, fs->fat_sec_buf);
            if (rc != 0) return VFS_ERR_IO;
        }
        fs->fat_sec_dirty = false;
    }

    int rc = read_sec(fs, fs->fat_lba + want, 1, fs->fat_sec_buf);
    if (rc != 0) {
        fs->fat_sec_idx = FAT32_INVALID_SEC;
        return VFS_ERR_IO;
    }
    fs->fat_sec_idx = want;
    return VFS_OK;
}

/* Force a flush. Called before mount returns failure / before format
 * operations that touch the FATs in bulk. */
static int fat_flush(struct fat32 *fs) {
    if (!fs->fat_sec_dirty || fs->fat_sec_idx == FAT32_INVALID_SEC) return VFS_OK;
    for (uint32_t i = 0; i < fs->num_fats; i++) {
        uint32_t lba = fs->fat_lba + i * fs->fat_sz_sec + fs->fat_sec_idx;
        int rc = write_sec(fs, lba, 1, fs->fat_sec_buf);
        if (rc != 0) return VFS_ERR_IO;
    }
    fs->fat_sec_dirty = false;
    return VFS_OK;
}

static int fat_get(struct fat32 *fs, uint32_t clus, uint32_t *out) {
    int rc = fat_load_sec(fs, clus);
    if (rc != VFS_OK) return rc;
    uint32_t off = (clus * 4) % fs->bytes_per_sec;
    uint32_t v;
    memcpy(&v, fs->fat_sec_buf + off, 4);
    *out = v & FAT32_ENTRY_MASK;
    return VFS_OK;
}

static int fat_set(struct fat32 *fs, uint32_t clus, uint32_t val) {
    int rc = fat_load_sec(fs, clus);
    if (rc != VFS_OK) return rc;
    uint32_t off = (clus * 4) % fs->bytes_per_sec;
    uint32_t v;
    memcpy(&v, fs->fat_sec_buf + off, 4);
    v = (v & ~FAT32_ENTRY_MASK) | (val & FAT32_ENTRY_MASK);
    memcpy(fs->fat_sec_buf + off, &v, 4);
    fs->fat_sec_dirty = true;
    return VFS_OK;
}

/* Find a free cluster, mark it as EOC, return its index. */
static int alloc_cluster(struct fat32 *fs, uint32_t *out_clus) {
    uint32_t start = fs->next_free_hint < 2 ? 2 : fs->next_free_hint;
    uint32_t total = fs->cluster_count;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t c = start + i;
        if (c >= total + 2) c -= total;
        if (c < 2) c = 2;
        uint32_t v;
        int rc = fat_get(fs, c, &v);
        if (rc != VFS_OK) return rc;
        if (v == FAT32_FREE) {
            rc = fat_set(fs, c, FAT32_EOC);
            if (rc != VFS_OK) return rc;
            rc = fat_flush(fs);
            if (rc != VFS_OK) return rc;
            /* Zero the cluster so dir scans don't trip on stale data. */
            memset(fs->clus_buf, 0, fs->cluster_bytes);
            rc = write_cluster(fs, c, fs->clus_buf);
            if (rc != 0) return VFS_ERR_IO;
            fs->next_free_hint = c + 1;
            *out_clus = c;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOSPC;
}

/* Free an entire cluster chain starting at `head`. Safe to call with
 * head == 0 (no-op). */
static int free_chain(struct fat32 *fs, uint32_t head) {
    uint32_t c = head;
    while (c >= 2 && c < FAT32_EOC_MIN) {
        uint32_t next;
        int rc = fat_get(fs, c, &next);
        if (rc != VFS_OK) return rc;
        rc = fat_set(fs, c, FAT32_FREE);
        if (rc != VFS_OK) return rc;
        if (fs->next_free_hint > c) fs->next_free_hint = c;
        c = next;
    }
    return fat_flush(fs);
}

/* Get cluster N of a chain (0-based), allocating + linking new clusters
 * to extend the chain if needed. */
static int chain_get_or_grow(struct fat32 *fs, uint32_t head, uint32_t n,
                             uint32_t *out_clus, uint32_t *out_new_head) {
    *out_new_head = head;
    uint32_t c = head;

    if (c < 2 || c >= FAT32_EOC_MIN) {
        uint32_t nc;
        int rc = alloc_cluster(fs, &nc);
        if (rc != VFS_OK) return rc;
        *out_new_head = nc;
        c = nc;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next;
        int rc = fat_get(fs, c, &next);
        if (rc != VFS_OK) return rc;
        if (next < 2 || next >= FAT32_EOC_MIN) {
            uint32_t nc;
            rc = alloc_cluster(fs, &nc);
            if (rc != VFS_OK) return rc;
            rc = fat_set(fs, c, nc);
            if (rc != VFS_OK) return rc;
            rc = fat_flush(fs);
            if (rc != VFS_OK) return rc;
            next = nc;
        }
        c = next;
    }
    *out_clus = c;
    return VFS_OK;
}

/* ---- name handling ---- */

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

/* Convert "NAME    EXT" (11-byte 8.3 buffer) into the human-friendly
 * "name.ext" form (lowercase if the NTRes case bits say so). Returns
 * the length written into `out` (NUL-terminated). */
static size_t shortname_decode(const struct fat_dirent *de, char *out, size_t cap) {
    char name[8];
    char ext[3];
    memcpy(name, de->name,     8);
    memcpy(ext,  de->name + 8, 3);

    /* Special: 0x05 in slot 0 -> 0xE5 in real name. */
    if ((uint8_t)name[0] == FAT_DIR_KANJI_REPL) name[0] = (char)0xE5;

    int nlen = 8;
    while (nlen > 0 && name[nlen-1] == ' ') nlen--;
    int elen = 3;
    while (elen > 0 && ext[elen-1] == ' ') elen--;

    bool name_lower = (de->ntres & 0x08) != 0;
    bool ext_lower  = (de->ntres & 0x10) != 0;

    size_t pos = 0;
    for (int i = 0; i < nlen && pos + 1 < cap; i++) {
        char c = name[i];
        if (name_lower && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[pos++] = c;
    }
    if (elen > 0 && pos + 1 < cap) {
        out[pos++] = '.';
        for (int i = 0; i < elen && pos + 1 < cap; i++) {
            char c = ext[i];
            if (ext_lower && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            out[pos++] = c;
        }
    }
    out[pos] = 0;
    return pos;
}

/* Build an 11-byte 8.3 short name from a UTF-8/ASCII filename. The
 * algorithm is the simple "uppercase, strip illegal chars, truncate
 * to 8 + 3, append ~N if truncation occurred". `out` MUST be 11
 * bytes; we always pad with spaces. Returns true if the result is
 * unique-by-construction (name fits in 8.3 verbatim and is uppercase),
 * false if the caller might want to disambiguate with ~N. */
static bool shortname_encode(const char *name, uint8_t out[11], bool *needs_tilde) {
    memset(out, ' ', 11);
    *needs_tilde = false;
    bool fit = true;

    /* Find last '.' -- everything after is the extension. */
    int last_dot = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '.') last_dot = i;
    }
    if (last_dot == 0) last_dot = -1;  /* leading dot = no extension */

    int n = 0;
    for (int i = 0; name[i] && (last_dot < 0 || i < last_dot); i++) {
        char c = name[i];
        if (c == ' ' || c == '.') { fit = false; continue; }
        if (c < 0x20 || c == '"' || c == '*' || c == '/' ||
            c == ':' || c == '<' || c == '>' || c == '?' ||
            c == '\\' || c == '|') { fit = false; c = '_'; }
        if (c >= 'a' && c <= 'z') { c = (char)(c - 'a' + 'A'); fit = false; }
        if (n < 8) {
            out[n++] = (uint8_t)c;
        } else {
            *needs_tilde = true;
            fit = false;
            break;
        }
    }
    if (last_dot >= 0) {
        int e = 0;
        for (int i = last_dot + 1; name[i]; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') { c = (char)(c - 'a' + 'A'); fit = false; }
            if (c < 0x20 || c == '"' || c == '*' || c == '/' ||
                c == ':' || c == '<' || c == '>' || c == '?' ||
                c == '\\' || c == '|' || c == '.') { fit = false; c = '_'; }
            if (e < 3) out[8 + e++] = (uint8_t)c;
            else { *needs_tilde = true; fit = false; break; }
        }
    }
    if (out[0] == 0xE5) out[0] = FAT_DIR_KANJI_REPL;  /* preserve special */
    return fit;
}

/* FAT short-name 8-bit checksum used by LFN entries. */
static uint8_t shortname_checksum(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name[i]);
    }
    return sum;
}

/* Append one UTF-16LE code unit `c` to `out` if room (cap > 0).
 * Skip 0x0000 (LFN end-of-string marker) and 0xFFFF (LFN padding).
 * Replace anything outside printable ASCII with '?'. */
static void lfn_append(char *out, size_t *pos, size_t cap, uint16_t c) {
    if (c == 0xFFFF) return;        /* LFN pad */
    if (c == 0x0000) return;        /* LFN string terminator */
    char ch;
    if (c >= 0x20 && c < 0x7F) ch = (char)c;
    else                       ch = '?';
    if (*pos + 1 < cap) out[(*pos)++] = ch;
}

/* ---- directory iteration helpers ---- */

/* Compare an ASCII filename against the assembled name, case-insensitively.
 * Returns 0 if equal. */
static int name_iequal(const char *a, const char *b) {
    while (*a && *b) {
        if (to_upper(*a) != to_upper(*b)) return 1;
        a++; b++;
    }
    return (*a || *b) ? 1 : 0;
}

/* Per-entry callback during a directory walk. Return values:
 *   VFS_OK             -> continue scanning
 *   VFS_ERR_NOENT      -> stop scanning (used as "found-and-done")
 *   any other negative -> abort with that error
 *
 * The callback receives:
 *   `de`           pointer to the SHORT-name entry inside the cluster
 *   `name`         assembled name (LFN if present, else 8.3 decoded)
 *   `dir_clus`     cluster the entry lives in
 *   `dir_off`      byte offset within that cluster
 *   `lfn_count`    how many LFN entries precede the short entry
 */
typedef int (*dir_cb_t)(void *user,
                        const struct fat_dirent *de, const char *name,
                        uint32_t dir_clus, uint32_t dir_off, int lfn_count);

/* Scan an entire directory chain. Stops on FAT_DIR_FREE_END or when the
 * callback returns VFS_ERR_NOENT (which we translate to VFS_OK to mean
 * "found and done"). */
static int dir_walk(struct fat32 *fs, uint32_t dir_first_clus,
                    dir_cb_t cb, void *user) {
    uint32_t clus = dir_first_clus;
    char     name_buf[256];
    /* LFN reassembly state -- entries can come at most 20 deep
     * (255 chars / 13 chars-per-LFN = 19.6). */
    uint16_t lfn_chars[260];
    int      lfn_pending = 0;       /* how many entries collected */
    int      lfn_total   = 0;       /* total entries in the LFN run */
    uint8_t  lfn_chksum  = 0;
    uint32_t cur_idx     = 0;       /* cluster index in chain (for callbacks) */

    while (clus >= 2 && clus < FAT32_EOC_MIN) {
        int rc = read_cluster(fs, clus, fs->clus_buf);
        if (rc != VFS_OK) return rc;

        for (uint32_t off = 0; off + 32 <= fs->cluster_bytes; off += 32) {
            const struct fat_dirent *de =
                (const struct fat_dirent *)(fs->clus_buf + off);

            if (de->name[0] == FAT_DIR_FREE_END) {
                return VFS_OK;  /* end-of-directory marker */
            }
            if (de->name[0] == (uint8_t)FAT_DIR_FREE) {
                lfn_pending = lfn_total = 0;
                continue;
            }
            if ((de->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
                const struct fat_lfn_entry *lfn =
                    (const struct fat_lfn_entry *)de;
                int ord = lfn->ord & 0x1F;
                if (lfn->ord & FAT_LFN_LAST) {
                    lfn_total   = ord;
                    lfn_pending = 0;
                    lfn_chksum  = lfn->checksum;
                    memset(lfn_chars, 0, sizeof(lfn_chars));
                }
                if (ord >= 1 && ord <= 20 && lfn->checksum == lfn_chksum) {
                    int base = (ord - 1) * FAT_LFN_CHARS;
                    for (int i = 0; i < 5; i++)  lfn_chars[base + 0 + i] = lfn->name1[i];
                    for (int i = 0; i < 6; i++)  lfn_chars[base + 5 + i] = lfn->name2[i];
                    for (int i = 0; i < 2; i++)  lfn_chars[base + 11 + i] = lfn->name3[i];
                    lfn_pending++;
                }
                continue;
            }
            if (de->attr & FAT_ATTR_VOLUME_ID) {
                lfn_pending = lfn_total = 0;
                continue;  /* skip volume-label entries */
            }

            /* Real short-name entry. Decide which name to expose. */
            (void)cur_idx;
            size_t pos = 0;
            bool used_lfn = false;
            if (lfn_total > 0 && lfn_pending == lfn_total &&
                shortname_checksum(de->name) == lfn_chksum) {
                /* Stitch together LFN chars in order. Stop at the first
                 * 0x0000 (string terminator). */
                for (int i = 0; i < lfn_total * FAT_LFN_CHARS; i++) {
                    uint16_t cu = lfn_chars[i];
                    if (cu == 0x0000) break;
                    lfn_append(name_buf, &pos, sizeof(name_buf), cu);
                }
                name_buf[pos] = 0;
                used_lfn = true;
            }
            if (!used_lfn) {
                pos = shortname_decode(de, name_buf, sizeof(name_buf));
            }
            lfn_pending = lfn_total = 0;

            int crc = cb(user, de, name_buf, clus, off,
                         used_lfn ? lfn_total : 0);
            if (crc == VFS_ERR_NOENT) return VFS_OK;
            if (crc != VFS_OK)        return crc;
        }

        /* Advance to next cluster of the directory. */
        uint32_t next;
        int rrc = fat_get(fs, clus, &next);
        if (rrc != VFS_OK) return rrc;
        clus = next;
        cur_idx++;
    }
    return VFS_OK;
}

/* ---- path resolution ---- */

/* Find the next '/' separated component in `path` starting at `*pos`.
 * Writes a NUL-terminated copy into `out` (cap bytes) and advances
 * *pos past the slash. Returns 0 on success, 1 when no more
 * components remain. */
static int next_component(const char *path, size_t *pos, char *out, size_t cap) {
    while (path[*pos] == '/') (*pos)++;
    if (path[*pos] == 0) return 1;
    size_t i = 0;
    while (path[*pos] && path[*pos] != '/') {
        if (i + 1 < cap) out[i++] = path[*pos];
        (*pos)++;
    }
    out[i] = 0;
    return 0;
}

struct lookup_ctx {
    const char       *want;
    bool              found;
    struct fat_dirent de_copy;
    uint32_t          dir_clus;
    uint32_t          dir_off;
};

static int lookup_cb(void *user, const struct fat_dirent *de, const char *name,
                     uint32_t dir_clus, uint32_t dir_off, int lfn_count) {
    (void)lfn_count;
    struct lookup_ctx *ctx = (struct lookup_ctx *)user;
    if (name_iequal(name, ctx->want) == 0) {
        ctx->found    = true;
        ctx->de_copy  = *de;
        ctx->dir_clus = dir_clus;
        ctx->dir_off  = dir_off;
        return VFS_ERR_NOENT;  /* signals "stop scanning" */
    }
    return VFS_OK;
}

/* Walk an absolute path under the mount. Returns the SHORT-name entry
 * for the leaf in *out_de plus its directory location. Trailing slash
 * is tolerated. */
static int path_walk(struct fat32 *fs, const char *path,
                     struct fat_dirent *out_de,
                     uint32_t *out_dir_clus, uint32_t *out_dir_off) {
    /* Empty path / "/" -> root directory. We synthesise a fake dirent
     * pointing at root_clus so callers can stat the mount root. */
    if (!path || !*path || (path[0] == '/' && path[1] == 0)) {
        memset(out_de, 0, sizeof(*out_de));
        out_de->attr = FAT_ATTR_DIRECTORY;
        out_de->fst_clus_lo = (uint16_t)(fs->root_clus & 0xFFFF);
        out_de->fst_clus_hi = (uint16_t)(fs->root_clus >> 16);
        if (out_dir_clus) *out_dir_clus = 0;
        if (out_dir_off)  *out_dir_off  = 0;
        return VFS_OK;
    }

    uint32_t cur = fs->root_clus;
    char comp[256];
    size_t pos = 0;
    /* Skip leading '/'. */
    if (path[pos] == '/') pos++;
    int done = 0;

    while (!done) {
        int more = next_component(path, &pos, comp, sizeof(comp));
        if (more) break;

        struct lookup_ctx ctx = { .want = comp, .found = false };
        int rc = dir_walk(fs, cur, lookup_cb, &ctx);
        if (rc != VFS_OK) return rc;
        if (!ctx.found)   return VFS_ERR_NOENT;

        /* Peek for more components. If yes, descend; if no, return. */
        size_t save = pos;
        char  next_comp[2];
        int   has_more = (next_component(path, &save, next_comp, sizeof(next_comp))
                          == 0);
        if (has_more) {
            if (!(ctx.de_copy.attr & FAT_ATTR_DIRECTORY)) {
                return VFS_ERR_NOTDIR;
            }
            cur = ((uint32_t)ctx.de_copy.fst_clus_hi << 16) | ctx.de_copy.fst_clus_lo;
            if (cur < 2) return VFS_ERR_NOENT;  /* empty subdir, can't descend */
        } else {
            *out_de = ctx.de_copy;
            if (out_dir_clus) *out_dir_clus = ctx.dir_clus;
            if (out_dir_off)  *out_dir_off  = ctx.dir_off;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOENT;
}

/* Split "/foo/bar/baz" into ("/foo/bar", "baz"). out_parent receives
 * a kmalloc'd copy (caller frees); leaf points into `path`. */
static int split_parent_leaf(const char *path, char **out_parent, const char **out_leaf) {
    if (!path || !*path) return VFS_ERR_INVAL;
    /* Find last '/'. */
    int last = -1;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') last = i;
    }
    if (last < 0) {
        /* No slash -- caller passed a bare name. Treat parent as root. */
        char *p = kmalloc(2);
        if (!p) return VFS_ERR_NOMEM;
        p[0] = '/'; p[1] = 0;
        *out_parent = p;
        *out_leaf   = path;
        return VFS_OK;
    }
    if (path[last + 1] == 0) return VFS_ERR_INVAL;  /* trailing slash */
    char *p = kmalloc((size_t)last + 2);
    if (!p) return VFS_ERR_NOMEM;
    if (last == 0) {
        p[0] = '/'; p[1] = 0;
    } else {
        memcpy(p, path, (size_t)last);
        p[last] = 0;
    }
    *out_parent = p;
    *out_leaf   = path + last + 1;
    return VFS_OK;
}

/* ---- VFS hooks ---- */

static int fat32_stat(void *mnt, const char *path, struct vfs_stat *out) {
    struct fat32 *fs = (struct fat32 *)mnt;
    struct fat_dirent de;
    int rc = path_walk(fs, path, &de, 0, 0);
    if (rc != VFS_OK) return rc;
    if (de.attr & FAT_ATTR_DIRECTORY) {
        out->type = VFS_TYPE_DIR;
        out->size = 0;
    } else {
        out->type = VFS_TYPE_FILE;
        out->size = de.file_size;
    }
    out->uid = 0; out->gid = 0; out->mode = 0;  /* FAT has no perms */
    return VFS_OK;
}

static int fat32_open(void *mnt, const char *path, struct vfs_file *out) {
    struct fat32 *fs = (struct fat32 *)mnt;
    struct fat_dirent de;
    uint32_t dc = 0, doff = 0;
    int rc = path_walk(fs, path, &de, &dc, &doff);
    if (rc != VFS_OK) return rc;
    if (de.attr & FAT_ATTR_DIRECTORY) return VFS_ERR_ISDIR;

    struct fat32_filepriv *fp = kcalloc(1, sizeof(*fp));
    if (!fp) return VFS_ERR_NOMEM;
    fp->first_clus    = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    fp->cur_clus      = fp->first_clus;
    fp->cur_clus_idx  = 0;
    fp->dir_clus      = dc;
    fp->dir_off       = doff;
    fp->cluster_in_dir= 0;

    out->priv = fp;
    out->pos  = 0;
    out->size = de.file_size;
    out->uid  = 0;
    out->gid  = 0;
    out->mode = 0;
    return VFS_OK;
}

static int fat32_close(struct vfs_file *f) {
    if (f->priv) { kfree(f->priv); f->priv = 0; }
    return VFS_OK;
}

static long fat32_read(struct vfs_file *f, void *buf, size_t n) {
    struct fat32 *fs = (struct fat32 *)f->mnt;
    struct fat32_filepriv *fp = (struct fat32_filepriv *)f->priv;
    if (!fp) return VFS_ERR_INVAL;
    if (f->pos >= f->size || n == 0) return 0;

    size_t avail = f->size - f->pos;
    if (n > avail) n = avail;

    uint8_t *out  = (uint8_t *)buf;
    size_t   left = n;
    while (left > 0) {
        uint32_t want_idx = (uint32_t)(f->pos / fs->cluster_bytes);
        uint32_t off      = (uint32_t)(f->pos % fs->cluster_bytes);

        /* If our cached current cluster is past the desired one,
         * restart from the head. */
        if (fp->cur_clus < 2 || fp->cur_clus_idx > want_idx) {
            fp->cur_clus     = fp->first_clus;
            fp->cur_clus_idx = 0;
        }
        while (fp->cur_clus_idx < want_idx) {
            uint32_t next;
            int rc = fat_get(fs, fp->cur_clus, &next);
            if (rc != VFS_OK) return rc;
            if (next < 2 || next >= FAT32_EOC_MIN) return (long)(n - left);
            fp->cur_clus = next;
            fp->cur_clus_idx++;
        }

        int rc = read_cluster(fs, fp->cur_clus, fs->clus_buf);
        if (rc != VFS_OK) return rc;

        size_t chunk = fs->cluster_bytes - off;
        if (chunk > left) chunk = left;
        memcpy(out, fs->clus_buf + off, chunk);
        out   += chunk;
        f->pos += chunk;
        left  -= chunk;
    }
    return (long)n;
}

/* Update directory entry on disk: file_size + first_cluster. */
static int update_dirent(struct fat32 *fs, uint32_t dir_clus, uint32_t dir_off,
                         uint32_t new_size, uint32_t new_first_clus) {
    if (dir_clus < 2) return VFS_ERR_INVAL;
    int rc = read_cluster(fs, dir_clus, fs->clus_buf);
    if (rc != VFS_OK) return rc;
    struct fat_dirent *de = (struct fat_dirent *)(fs->clus_buf + dir_off);
    de->file_size  = new_size;
    de->fst_clus_lo = (uint16_t)(new_first_clus & 0xFFFF);
    de->fst_clus_hi = (uint16_t)(new_first_clus >> 16);
    return write_cluster(fs, dir_clus, fs->clus_buf);
}

static long fat32_write(struct vfs_file *f, const void *buf, size_t n) {
    struct fat32 *fs = (struct fat32 *)f->mnt;
    struct fat32_filepriv *fp = (struct fat32_filepriv *)f->priv;
    if (!fp || n == 0) return 0;

    const uint8_t *src  = (const uint8_t *)buf;
    size_t         left = n;

    while (left > 0) {
        uint32_t want_idx = (uint32_t)(f->pos / fs->cluster_bytes);
        uint32_t off      = (uint32_t)(f->pos % fs->cluster_bytes);

        uint32_t target;
        uint32_t new_head;
        int rc = chain_get_or_grow(fs, fp->first_clus, want_idx, &target, &new_head);
        if (rc != VFS_OK) return rc;
        if (new_head != fp->first_clus) {
            fp->first_clus = new_head;
            fp->cur_clus   = new_head;
            fp->cur_clus_idx = 0;
        }
        fp->cur_clus     = target;
        fp->cur_clus_idx = want_idx;

        size_t chunk = fs->cluster_bytes - off;
        if (chunk > left) chunk = left;

        if (chunk == fs->cluster_bytes) {
            /* Whole cluster overwrite -- skip the read step. */
            memcpy(fs->clus_buf, src, chunk);
        } else {
            rc = read_cluster(fs, target, fs->clus_buf);
            if (rc != VFS_OK) return rc;
            memcpy(fs->clus_buf + off, src, chunk);
        }
        rc = write_cluster(fs, target, fs->clus_buf);
        if (rc != 0) return VFS_ERR_IO;

        src   += chunk;
        f->pos += chunk;
        left  -= chunk;
        if (f->pos > f->size) f->size = f->pos;
    }

    /* Flush any pending FAT changes + persist the dirent. */
    int rc = fat_flush(fs);
    if (rc != VFS_OK) return rc;
    rc = update_dirent(fs, fp->dir_clus, fp->dir_off,
                       (uint32_t)f->size, fp->first_clus);
    if (rc != 0) return VFS_ERR_IO;
    return (long)n;
}

/* ---- create ---- */

/* Find an empty 32-byte slot in the directory chain, growing if
 * needed. Returns the cluster + offset in (*out_clus, *out_off). */
static int dir_find_free_slot(struct fat32 *fs, uint32_t dir_first_clus,
                              uint32_t *out_clus, uint32_t *out_off) {
    uint32_t clus = dir_first_clus;
    uint32_t prev = 0;
    while (clus >= 2 && clus < FAT32_EOC_MIN) {
        int rc = read_cluster(fs, clus, fs->clus_buf);
        if (rc != VFS_OK) return rc;
        for (uint32_t off = 0; off + 32 <= fs->cluster_bytes; off += 32) {
            uint8_t b = fs->clus_buf[off];
            if (b == FAT_DIR_FREE_END || b == (uint8_t)FAT_DIR_FREE) {
                *out_clus = clus;
                *out_off  = off;
                return VFS_OK;
            }
        }
        prev = clus;
        uint32_t next;
        rc = fat_get(fs, clus, &next);
        if (rc != VFS_OK) return rc;
        clus = next;
    }
    /* Out of slots: extend the directory by one cluster. */
    if (prev == 0) return VFS_ERR_NOSPC;
    uint32_t nc;
    int rc = alloc_cluster(fs, &nc);
    if (rc != VFS_OK) return rc;
    rc = fat_set(fs, prev, nc);
    if (rc != VFS_OK) return rc;
    rc = fat_flush(fs);
    if (rc != VFS_OK) return rc;
    *out_clus = nc;
    *out_off  = 0;
    return VFS_OK;
}

/* Disambiguate `base` (8.3 buffer) by appending ~N (1..9) so it
 * doesn't collide with anything in the directory. */
struct collide_ctx {
    const uint8_t *probe;   /* 11-byte short name */
    bool collided;
};

static int collide_cb(void *user, const struct fat_dirent *de, const char *name,
                      uint32_t dc, uint32_t off, int lfn) {
    (void)name; (void)dc; (void)off; (void)lfn;
    struct collide_ctx *ctx = (struct collide_ctx *)user;
    if (memcmp(de->name, ctx->probe, 11) == 0) {
        ctx->collided = true;
        return VFS_ERR_NOENT;
    }
    return VFS_OK;
}

static int disambiguate_short(struct fat32 *fs, uint32_t parent_clus,
                              uint8_t name11[11]) {
    /* Try base name first. */
    struct collide_ctx ctx = { .probe = name11, .collided = false };
    int rc = dir_walk(fs, parent_clus, collide_cb, &ctx);
    if (rc != VFS_OK) return rc;
    if (!ctx.collided) return VFS_OK;

    for (int n = 1; n <= 9; n++) {
        uint8_t cand[11];
        memcpy(cand, name11, 11);
        char tail[2] = { '~', (char)('0' + n) };
        /* Place ~N in slots 6..7 (index 5..6) to leave room for ext. */
        cand[5] = tail[0];
        cand[6] = tail[1];
        for (int i = 7; i < 8; i++) cand[i] = ' ';
        ctx.probe = cand; ctx.collided = false;
        rc = dir_walk(fs, parent_clus, collide_cb, &ctx);
        if (rc != VFS_OK) return rc;
        if (!ctx.collided) {
            memcpy(name11, cand, 11);
            return VFS_OK;
        }
    }
    return VFS_ERR_EXIST;
}

static int fat32_create(void *mnt, const char *path,
                        uint32_t uid, uint32_t gid, uint32_t mode) {
    (void)uid; (void)gid; (void)mode;  /* FAT has no permissions */
    struct fat32 *fs = (struct fat32 *)mnt;

    char *parent = 0;
    const char *leaf = 0;
    int rc = split_parent_leaf(path, &parent, &leaf);
    if (rc != VFS_OK) return rc;

    /* Resolve parent. */
    struct fat_dirent pde;
    rc = path_walk(fs, parent, &pde, 0, 0);
    kfree(parent); parent = 0;
    if (rc != VFS_OK) return rc;
    if (!(pde.attr & FAT_ATTR_DIRECTORY)) return VFS_ERR_NOTDIR;
    uint32_t pclus = ((uint32_t)pde.fst_clus_hi << 16) | pde.fst_clus_lo;
    if (pclus < 2) return VFS_ERR_NOENT;

    /* Reject if leaf already exists. */
    {
        struct lookup_ctx ctx = { .want = leaf, .found = false };
        rc = dir_walk(fs, pclus, lookup_cb, &ctx);
        if (rc != VFS_OK) return rc;
        if (ctx.found) return VFS_ERR_EXIST;
    }

    /* Encode + disambiguate short name. */
    uint8_t name11[11];
    bool needs_tilde = false;
    (void)shortname_encode(leaf, name11, &needs_tilde);
    rc = disambiguate_short(fs, pclus, name11);
    if (rc != VFS_OK) return rc;

    uint32_t slot_clus, slot_off;
    rc = dir_find_free_slot(fs, pclus, &slot_clus, &slot_off);
    if (rc != VFS_OK) return rc;

    rc = read_cluster(fs, slot_clus, fs->clus_buf);
    if (rc != VFS_OK) return rc;
    struct fat_dirent *de = (struct fat_dirent *)(fs->clus_buf + slot_off);
    memset(de, 0, sizeof(*de));
    memcpy(de->name, name11, 11);
    de->attr        = FAT_ATTR_ARCHIVE;
    de->ntres       = 0;
    de->fst_clus_lo = 0;
    de->fst_clus_hi = 0;
    de->file_size   = 0;
    rc = write_cluster(fs, slot_clus, fs->clus_buf);
    if (rc != 0) return VFS_ERR_IO;
    return VFS_OK;
}

/* ---- unlink ---- */

static int fat32_unlink(void *mnt, const char *path) {
    struct fat32 *fs = (struct fat32 *)mnt;
    struct fat_dirent de;
    uint32_t dc = 0, doff = 0;
    int rc = path_walk(fs, path, &de, &dc, &doff);
    if (rc != VFS_OK) return rc;
    if (de.attr & FAT_ATTR_DIRECTORY) return VFS_ERR_ISDIR;

    /* Free cluster chain. */
    uint32_t head = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    if (head >= 2) {
        rc = free_chain(fs, head);
        if (rc != VFS_OK) return rc;
    }

    /* Mark the short entry deleted. We also walk backward across any
     * preceding LFN entries within the same cluster and mark them too,
     * which is what fsck would do; missing LFN entries are tolerated.
     */
    rc = read_cluster(fs, dc, fs->clus_buf);
    if (rc != VFS_OK) return rc;

    fs->clus_buf[doff] = (uint8_t)FAT_DIR_FREE;

    /* Walk backward inside the cluster, tagging LFN entries. */
    int32_t pos = (int32_t)doff - 32;
    while (pos >= 0) {
        struct fat_dirent *prev = (struct fat_dirent *)(fs->clus_buf + pos);
        if ((prev->attr & FAT_ATTR_LFN) != FAT_ATTR_LFN) break;
        if (prev->name[0] == (uint8_t)FAT_DIR_FREE) break;
        prev->name[0] = (uint8_t)FAT_DIR_FREE;
        pos -= 32;
    }
    rc = write_cluster(fs, dc, fs->clus_buf);
    if (rc != 0) return VFS_ERR_IO;
    return fat_flush(fs);
}

/* ---- mkdir / chmod / chown -- not supported in 23B ---- */

static int fat32_mkdir(void *mnt, const char *path,
                       uint32_t uid, uint32_t gid, uint32_t mode) {
    (void)mnt; (void)path; (void)uid; (void)gid; (void)mode;
    return VFS_ERR_ROFS;
}

/* ---- opendir / readdir ---- */

struct collect_ctx {
    struct vfs_dirent *ents;
    size_t             cap;
    size_t             count;
};

static int collect_cb(void *user, const struct fat_dirent *de, const char *name,
                      uint32_t dc, uint32_t off, int lfn) {
    (void)dc; (void)off; (void)lfn;
    struct collect_ctx *ctx = (struct collect_ctx *)user;
    if (ctx->count >= ctx->cap) return VFS_OK;

    /* Skip "." / ".." -- VFS callers don't want them. */
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
        return VFS_OK;
    }

    struct vfs_dirent *e = &ctx->ents[ctx->count++];
    size_t nlen = strlen(name);
    if (nlen >= sizeof(e->name)) nlen = sizeof(e->name) - 1;
    memcpy(e->name, name, nlen);
    e->name[nlen] = 0;
    if (de->attr & FAT_ATTR_DIRECTORY) {
        e->type = VFS_TYPE_DIR;
        e->size = 0;
    } else {
        e->type = VFS_TYPE_FILE;
        e->size = de->file_size;
    }
    e->uid = 0; e->gid = 0; e->mode = 0;
    return VFS_OK;
}

static int fat32_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    struct fat32 *fs = (struct fat32 *)mnt;
    struct fat_dirent de;
    int rc = path_walk(fs, path, &de, 0, 0);
    if (rc != VFS_OK) return rc;
    if (!(de.attr & FAT_ATTR_DIRECTORY)) return VFS_ERR_NOTDIR;

    uint32_t clus = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    if (clus < 2) clus = fs->root_clus;  /* root has hi/lo == 0 in our fake */

    struct fat32_diriter *it = kmalloc(sizeof(*it));
    if (!it) return VFS_ERR_NOMEM;
    /* Cap: fit "every 32-byte slot in 64 clusters" -- bounded enough
     * to not blow the heap on huge dirs while comfortably handling the
     * typical case. */
    size_t cap = 64 * (fs->cluster_bytes / 32);
    if (cap < 64) cap = 64;
    it->ents = kcalloc(cap, sizeof(*it->ents));
    if (!it->ents) { kfree(it); return VFS_ERR_NOMEM; }
    it->count = 0;

    struct collect_ctx ctx = { .ents = it->ents, .cap = cap, .count = 0 };
    rc = dir_walk(fs, clus, collect_cb, &ctx);
    if (rc != VFS_OK) {
        kfree(it->ents); kfree(it);
        return rc;
    }
    it->count = ctx.count;
    out->priv  = it;
    out->index = 0;
    return VFS_OK;
}

static int fat32_closedir(struct vfs_dir *d) {
    struct fat32_diriter *it = (struct fat32_diriter *)d->priv;
    if (it) {
        if (it->ents) kfree(it->ents);
        kfree(it);
    }
    d->priv = 0;
    return VFS_OK;
}

static int fat32_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    struct fat32_diriter *it = (struct fat32_diriter *)d->priv;
    if (!it) return VFS_ERR_INVAL;
    if (d->index >= it->count) return VFS_ERR_NOENT;
    *out = it->ents[d->index++];
    return VFS_OK;
}

/* ---- vfs_ops table ---- */

/* M26E: drop our cached cluster buffers + the parent fs struct itself.
 * Called by vfs_unmount AFTER the slot has been removed from the
 * table, so no concurrent callers exist. We deliberately do NOT touch
 * fs->dev: the block device might still be alive (clean unmount) or
 * already flagged gone (yank-while-mounted), and either way the
 * usb_msc / partition layer owns its lifetime. */
static int fat32_umount(void *mnt) {
    struct fat32 *fs = (struct fat32 *)mnt;
    if (!fs) return VFS_OK;
    /* Best-effort flush of the cached FAT sector. fat_flush mirrors
     * the write to every FAT copy; if the device is gone, blk_write
     * returns -1 and we just log + continue -- unmount must not
     * "fail" because hardware vanished mid-operation. */
    if (fs->fat_sec_dirty && fs->dev && !fs->dev->gone) {
        if (fat_flush(fs) != VFS_OK) {
            kprintf("[fat32] umount: dirty FAT flush failed (device gone?)\n");
        }
    }
    fs->fat_sec_dirty = false;

    if (fs->clus_buf)  kfree(fs->clus_buf);
    if (fs->clus_buf2) kfree(fs->clus_buf2);
    fs->clus_buf  = 0;
    fs->clus_buf2 = 0;
    kfree(fs);
    return VFS_OK;
}

/* Exposed (non-static) so callers like usb_msc_unbind can identify a
 * FAT32 mount with `mount.ops == &fat32_ops` before reaching into the
 * mount-data via fat32_blkdev_of(). Still const -- nothing outside
 * fat32.c may mutate the table. */
const struct vfs_ops fat32_ops = {
    .open     = fat32_open,
    .close    = fat32_close,
    .read     = fat32_read,
    .write    = fat32_write,
    .create   = fat32_create,
    .unlink   = fat32_unlink,
    .mkdir    = fat32_mkdir,
    .opendir  = fat32_opendir,
    .closedir = fat32_closedir,
    .readdir  = fat32_readdir,
    .stat     = fat32_stat,
    .chmod    = 0,   /* FAT has no perms */
    .chown    = 0,
    .umount   = fat32_umount,
};

struct blk_dev *fat32_blkdev_of(void *mnt) {
    if (!mnt) return 0;
    return ((struct fat32 *)mnt)->dev;
}

/* ---- mount + probe ---- */

static int parse_bpb(struct fat32 *fs, const struct fat32_bpb *bpb) {
    if (bpb->bytes_per_sec != 512)        return VFS_ERR_INVAL;
    if (bpb->sec_per_clus  == 0 ||
        (bpb->sec_per_clus & (bpb->sec_per_clus - 1)) != 0) return VFS_ERR_INVAL;
    if (bpb->sec_per_clus  > 16)          return VFS_ERR_INVAL;  /* keep cluster <= 8K */
    if (bpb->num_fats      < 1 || bpb->num_fats > 2) return VFS_ERR_INVAL;
    if (bpb->root_ent_cnt  != 0)          return VFS_ERR_INVAL;  /* must be 0 for FAT32 */
    if (bpb->fat_sz16      != 0)          return VFS_ERR_INVAL;
    if (bpb->fat_sz32      == 0)          return VFS_ERR_INVAL;
    if (bpb->root_clus     < 2)           return VFS_ERR_INVAL;
    if (bpb->fs_ver        != 0)          return VFS_ERR_INVAL;

    fs->bytes_per_sec  = bpb->bytes_per_sec;
    fs->sec_per_clus   = bpb->sec_per_clus;
    fs->cluster_bytes  = (uint32_t)fs->sec_per_clus * fs->bytes_per_sec;
    fs->rsvd_sec_cnt   = bpb->rsvd_sec_cnt;
    fs->num_fats       = bpb->num_fats;
    fs->fat_sz_sec     = bpb->fat_sz32;
    fs->tot_sec        = bpb->tot_sec32 ? bpb->tot_sec32 : bpb->tot_sec16;
    fs->fat_lba        = fs->rsvd_sec_cnt;
    fs->data_lba       = fs->fat_lba + fs->num_fats * fs->fat_sz_sec;
    if (fs->tot_sec <= fs->data_lba)      return VFS_ERR_INVAL;
    fs->total_data_sec = fs->tot_sec - fs->data_lba;
    fs->cluster_count  = fs->total_data_sec / fs->sec_per_clus;
    fs->root_clus      = bpb->root_clus;
    fs->fsi_lba        = bpb->fs_info < fs->rsvd_sec_cnt ? bpb->fs_info : 0;
    return VFS_OK;
}

int fat32_probe(struct blk_dev *dev) {
    if (!dev) return 0;
    uint8_t buf[512];
    if (blk_read(dev, 0, 1, buf) != 0) return 0;
    /* 0x55AA boot signature. */
    if (buf[510] != 0x55 || buf[511] != 0xAA) return 0;
    const struct fat32_bpb *bpb = (const struct fat32_bpb *)buf;
    if (bpb->bytes_per_sec != 512)              return 0;
    if (bpb->sec_per_clus  == 0)                return 0;
    if (bpb->fat_sz16      != 0)                return 0;
    if (bpb->fat_sz32      == 0)                return 0;
    if (bpb->root_ent_cnt  != 0)                return 0;
    if (bpb->root_clus     < 2)                 return 0;
    /* "FAT32" tag in EBPB is informational but a strong hint. */
    if (memcmp(bpb->fs_type, "FAT32", 5) != 0)  return 0;
    return 1;
}

int fat32_mount(const char *mount_point, struct blk_dev *dev) {
    if (!mount_point || !dev) return VFS_ERR_INVAL;

    struct fat32 *fs = kcalloc(1, sizeof(*fs));
    if (!fs) return VFS_ERR_NOMEM;
    fs->dev = dev;
    fs->fat_sec_idx = FAT32_INVALID_SEC;
    fs->fat_sec_dirty = false;
    fs->next_free_hint = 2;

    uint8_t bpb_buf[512];
    if (blk_read(dev, 0, 1, bpb_buf) != 0) {
        kprintf("[fat32] boot-sector read failed\n");
        kfree(fs);
        return VFS_ERR_IO;
    }
    if (bpb_buf[510] != 0x55 || bpb_buf[511] != 0xAA) {
        kprintf("[fat32] no 0x55AA signature -- not a FAT volume\n");
        kfree(fs);
        return VFS_ERR_INVAL;
    }
    int rc = parse_bpb(fs, (const struct fat32_bpb *)bpb_buf);
    if (rc != VFS_OK) {
        kprintf("[fat32] BPB rejected (rc=%d)\n", rc);
        kfree(fs);
        return rc;
    }

    fs->clus_buf  = kmalloc(fs->cluster_bytes);
    fs->clus_buf2 = kmalloc(fs->cluster_bytes);
    if (!fs->clus_buf || !fs->clus_buf2) {
        if (fs->clus_buf)  kfree(fs->clus_buf);
        if (fs->clus_buf2) kfree(fs->clus_buf2);
        kfree(fs);
        return VFS_ERR_NOMEM;
    }

    /* Try to seed the free-cluster hint from FSInfo. */
    if (fs->fsi_lba) {
        uint8_t fsi_buf[512];
        if (blk_read(dev, fs->fsi_lba, 1, fsi_buf) == 0) {
            const struct fat32_fsinfo *fsi = (const struct fat32_fsinfo *)fsi_buf;
            if (fsi->lead_sig == FAT32_FSI_LEAD_SIG &&
                fsi->struct_sig == FAT32_FSI_STRUCT_SIG &&
                fsi->trail_sig == FAT32_FSI_TRAIL_SIG &&
                fsi->nxt_free  != 0xFFFFFFFFu &&
                fsi->nxt_free  >= 2) {
                fs->next_free_hint = fsi->nxt_free;
            }
        }
    }

    rc = vfs_mount(mount_point, &fat32_ops, fs);
    if (rc != VFS_OK) {
        kprintf("[fat32] vfs_mount('%s') failed: %d\n", mount_point, rc);
        kfree(fs->clus_buf);
        kfree(fs->clus_buf2);
        kfree(fs);
        return rc;
    }

    kprintf("[fat32] mounted '%s' on %s: %u clusters x %u B "
            "(%u KiB total, %u FATs x %u sec, root@cluster %u)\n",
            mount_point, dev->name ? dev->name : "(anon)",
            fs->cluster_count, fs->cluster_bytes,
            (fs->cluster_count * fs->cluster_bytes) / 1024u,
            fs->num_fats, fs->fat_sz_sec, fs->root_clus);
    return VFS_OK;
}
