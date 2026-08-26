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

/* Concurrent open files tracked per mount. Sized for "plenty";
 * exceeding it is reported, never silently ignored. */
#define FAT32_MAX_OPEN 64

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

    /* Open handles per FILE, keyed by its directory entry, so unlink()
     * can defer the release to the last close. See foref_get(). */
    struct fat32_openref {
        uint32_t id;                  /* unique; 0 = free slot */
        uint32_t dir_clus, dir_off;   /* the entry that names the file */
        int      refs;
        bool     orphan;              /* the name went away while open */
        uint32_t head;                /* chain to free at the last close */
        uint32_t size;                /* size, once the entry is gone */
    } openrefs[FAT32_MAX_OPEN];
    uint32_t oref_next_id;
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
    /* Which openref is OURS. Looking the record up by (dir_clus,dir_off)
     * instead would find whichever file now owns that slot once ours has
     * been unlinked. */
    uint32_t oref_id;
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

/* ---- open-handle references, so unlink can defer the release --------
 *
 * POSIX: unlink() removes the NAME; the bytes survive until the last
 * descriptor closes. fat32_unlink() freed the cluster chain before it
 * even tombstoned the entry, so an open handle was reading clusters the
 * allocator had already handed back -- and the very next file created
 * took them.
 *
 * FAT has no inode, so the identity of an open file is its directory
 * ENTRY (cluster + offset). That slot is stable while the file exists,
 * but unlink tombstones it and a later create can reuse it -- which is
 * why an orphaned handle must stop consulting the entry entirely and
 * work from what this table remembers instead.
 */
/* Find the record for a file that still HAS this name.
 *
 * The orphan check is the whole point. unlink() tombstones the entry and
 * dir_find_free_slot() hands that very slot to the next create, so
 * (dir_clus, dir_off) stops identifying one file the moment a name is
 * removed -- the new file would otherwise join the dead file's record,
 * inherit its refcount, and overwrite the chain the old handles are
 * still reading. Hence the id: a handle finds ITS OWN record, not
 * whatever now occupies the slot it was opened from. */
static struct fat32_openref *foref_find_live(struct fat32 *fs, uint32_t dc,
                                             uint32_t doff) {
    for (size_t i = 0; i < FAT32_MAX_OPEN; i++)
        if (fs->openrefs[i].refs > 0 && !fs->openrefs[i].orphan &&
            fs->openrefs[i].dir_clus == dc && fs->openrefs[i].dir_off == doff)
            return &fs->openrefs[i];
    return 0;
}

static struct fat32_openref *foref_by_id(struct fat32 *fs, uint32_t id) {
    if (!id) return 0;
    for (size_t i = 0; i < FAT32_MAX_OPEN; i++)
        if (fs->openrefs[i].refs > 0 && fs->openrefs[i].id == id)
            return &fs->openrefs[i];
    return 0;
}

static int foref_get(struct fat32 *fs, uint32_t dc, uint32_t doff,
                     uint32_t *out_id) {
    struct fat32_openref *r = foref_find_live(fs, dc, doff);
    if (r) { r->refs++; *out_id = r->id; return VFS_OK; }
    for (size_t i = 0; i < FAT32_MAX_OPEN; i++) {
        if (fs->openrefs[i].refs == 0) {
            fs->openrefs[i].id       = ++fs->oref_next_id;
            fs->openrefs[i].dir_clus = dc;
            fs->openrefs[i].dir_off  = doff;
            fs->openrefs[i].refs     = 1;
            fs->openrefs[i].orphan   = false;
            fs->openrefs[i].head     = 0;
            fs->openrefs[i].size     = 0;
            *out_id = fs->openrefs[i].id;
            return VFS_OK;
        }
    }
    kprintf("[fat32] open-file table full (%d) -- refusing to open "
            "untracked\n", FAT32_MAX_OPEN);
    return VFS_ERR_NOMEM;
}

static void foref_put(struct fat32 *fs, uint32_t id) {
    struct fat32_openref *r = foref_by_id(fs, id);
    if (!r) return;
    if (--r->refs > 0) return;
    bool     orphan = r->orphan;
    uint32_t head   = r->head;
    r->id = 0; r->dir_clus = 0; r->dir_off = 0; r->orphan = false;
    r->head = 0;
    /* Last close of a name-less file: NOW the clusters go back. */
    if (orphan && head >= 2) {
        (void)free_chain(fs, head);
        (void)fat_flush(fs);
    }
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

/* ================================================================
 * Timestamps, truncate and statfs.
 *
 * FAT stores a file's size and its dates in the DIRECTORY ENTRY, not in
 * an inode, so all of this works by rewriting the 32-byte entry that
 * path_walk() already located.
 *
 * Three gaps closed here:
 *   - truncate/ftruncate were NULL, so `>` onto an existing file and
 *     truncate(2) both failed with EROFS on a writable volume;
 *   - no code ever set a date, so every file tobyOS created on a FAT
 *     volume carried date 0 -- which is not "unknown", it decodes as
 *     day 0 of month 0 of 1980 and Windows shows it as invalid;
 *   - fat32_statfs() was written but never put in the ops table, so
 *     `df` on a FAT mount returned EROFS. It also reported cluster
 *     COUNTS against a hardcoded 4096-byte block size, which only told
 *     the truth when the cluster happened to be 4 KiB.
 * ================================================================ */

static uint64_t fat32_now_secs(void) {
    extern uint64_t lx_realtime_ns(uint64_t mono_ns);
    extern uint64_t perf_now_ns(void);
    return lx_realtime_ns(perf_now_ns()) / 1000000000ull;
}

/* Days <-> civil date, Howard Hinnant's algorithm: exact for the whole
 * range FAT can express, no lookup tables, no leap-year special cases
 * beyond the arithmetic itself. */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    int64_t  era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - (int)(era * 400));
    unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t  yr  = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp  = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp + (mp < 10u ? 3u : (unsigned)-9);
    *y = (int)(yr + (*m <= 2));
}

/* DOS date: yyyyyyym mmmddddd, year relative to 1980.
 * DOS time: hhhhhmmm mmmsssss, seconds in 2-second units. */
static void fat_dos_encode(uint64_t secs, uint16_t *date, uint16_t *time) {
    int64_t  days = (int64_t)(secs / 86400ull);
    uint32_t sod  = (uint32_t)(secs % 86400ull);
    int y; unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    /* FAT cannot represent anything outside 1980..2107. Clamping is the
     * honest option: the alternative is a wrapped year that reads as a
     * confidently wrong date. */
    if (y < 1980)  { y = 1980; m = 1;  d = 1;  sod = 0; }
    if (y > 2107)  { y = 2107; m = 12; d = 31; sod = 86399; }
    *date = (uint16_t)(((unsigned)(y - 1980) << 9) | (m << 5) | d);
    *time = (uint16_t)(((sod / 3600u) << 11) |
                       (((sod / 60u) % 60u) << 5) |
                       ((sod % 60u) / 2u));
}
static uint64_t fat_dos_decode(uint16_t date, uint16_t time) {
    if (date == 0) return 0;                 /* never stamped */
    int      y = 1980 + ((date >> 9) & 0x7F);
    unsigned m = (date >> 5) & 0x0F;
    unsigned d = date & 0x1F;
    if (m < 1 || m > 12 || d < 1 || d > 31) return 0;   /* refuse nonsense */
    uint32_t sod = ((time >> 11) & 0x1Fu) * 3600u +
                   ((time >> 5)  & 0x3Fu) * 60u +
                   ((time        & 0x1Fu) * 2u);
    return (uint64_t)(days_from_civil(y, m, d) * 86400 + (int64_t)sod);
}

/* The dirent is packed, so its uint16_t fields have no guaranteed
 * alignment and their addresses are not valid uint16_t pointers. Encode
 * into locals and assign. */
static void fat_stamp_write(struct fat_dirent *de, uint64_t secs) {
    uint16_t date, time;
    fat_dos_encode(secs, &date, &time);
    de->wrt_date = date;
    de->wrt_time = time;
}

static int read_dirent_at(struct fat32 *fs, uint32_t dir_clus,
                          uint32_t dir_off, struct fat_dirent *out) {
    if (dir_clus < 2) return VFS_ERR_INVAL;
    int rc = read_cluster(fs, dir_clus, fs->clus_buf);
    if (rc != VFS_OK) return rc;
    memcpy(out, fs->clus_buf + dir_off, sizeof(*out));
    return VFS_OK;
}
static int write_dirent_at(struct fat32 *fs, uint32_t dir_clus,
                           uint32_t dir_off, const struct fat_dirent *src) {
    if (dir_clus < 2) return VFS_ERR_INVAL;
    int rc = read_cluster(fs, dir_clus, fs->clus_buf);
    if (rc != VFS_OK) return rc;
    memcpy(fs->clus_buf + dir_off, src, sizeof(*src));
    return write_cluster(fs, dir_clus, fs->clus_buf);
}

/* Walk to cluster `n` of a chain WITHOUT extending it. */
static int chain_nth(struct fat32 *fs, uint32_t head, uint32_t n,
                     uint32_t *out) {
    uint32_t c = head;
    for (uint32_t i = 0; i < n; i++) {
        if (c < 2 || c >= FAT32_EOC_MIN) return VFS_ERR_INVAL;
        uint32_t next;
        int rc = fat_get(fs, c, &next);
        if (rc != VFS_OK) return rc;
        c = next;
    }
    if (c < 2 || c >= FAT32_EOC_MIN) return VFS_ERR_INVAL;
    *out = c;
    return VFS_OK;
}

/* Resize a file's cluster chain. `*first_clus` is updated in place (it
 * becomes 0 for an emptied file, or a fresh head for one that was
 * empty). */
static int fat_resize_chain(struct fat32 *fs, uint32_t *first_clus,
                            uint64_t old_size, uint64_t new_size) {
    uint32_t cb   = fs->cluster_bytes;
    uint32_t need = (uint32_t)((new_size + cb - 1) / cb);

    if (new_size < old_size) {
        if (need == 0) {
            int rc = free_chain(fs, *first_clus);
            if (rc != VFS_OK) return rc;
            *first_clus = 0;
            return VFS_OK;
        }
        uint32_t last;
        int rc = chain_nth(fs, *first_clus, need - 1, &last);
        if (rc != VFS_OK) return rc;

        /* Zero what is left of the last surviving cluster. Those bytes
         * are inside a cluster we keep, so growing the file again would
         * otherwise return its own old contents where POSIX promises
         * zeroes. */
        uint32_t off = (uint32_t)(new_size % cb);
        if (off) {
            rc = read_cluster(fs, last, fs->clus_buf);
            if (rc != VFS_OK) return rc;
            memset(fs->clus_buf + off, 0, cb - off);
            rc = write_cluster(fs, last, fs->clus_buf);
            if (rc != 0) return VFS_ERR_IO;
        }

        uint32_t next;
        rc = fat_get(fs, last, &next);
        if (rc != VFS_OK) return rc;
        if (next >= 2 && next < FAT32_EOC_MIN) {
            rc = fat_set(fs, last, FAT32_EOC);
            if (rc != VFS_OK) return rc;
            rc = fat_flush(fs);
            if (rc != VFS_OK) return rc;
            rc = free_chain(fs, next);
            if (rc != VFS_OK) return rc;
        }
        return VFS_OK;
    }

    if (new_size > old_size && need > 0) {
        /* FAT has no holes -- there is no way to say "this cluster is
         * absent but reads as zero", so growing must really allocate.
         * alloc_cluster() zeroes what it hands out, so the new range
         * still reads as zeroes. */
        uint32_t target, head;
        int rc = chain_get_or_grow(fs, *first_clus, need - 1, &target, &head);
        if (rc != VFS_OK) return rc;
        *first_clus = head;
        return fat_flush(fs);
    }
    return VFS_OK;
}

static int fat32_truncate(void *mnt, const char *path, uint64_t length) {
    struct fat32 *fs = (struct fat32 *)mnt;
    if (length > 0xFFFFFFFFull) return VFS_ERR_INVAL;  /* FAT32 caps at 4 GiB */
    struct fat_dirent de;
    uint32_t dc = 0, doff = 0;
    int rc = path_walk(fs, path, &de, &dc, &doff);
    if (rc != VFS_OK) return rc;
    if (de.attr & FAT_ATTR_DIRECTORY) return VFS_ERR_ISDIR;

    uint32_t first = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    rc = fat_resize_chain(fs, &first, de.file_size, length);
    if (rc != VFS_OK) return rc;

    de.file_size    = (uint32_t)length;
    de.fst_clus_hi  = (uint16_t)(first >> 16);
    de.fst_clus_lo  = (uint16_t)(first & 0xFFFF);
    fat_stamp_write(&de, fat32_now_secs());
    return write_dirent_at(fs, dc, doff, &de);
}

static int fat32_ftruncate(struct vfs_file *f, uint64_t length) {
    struct fat32 *fs = (struct fat32 *)f->mnt;
    struct fat32_filepriv *fp = (struct fat32_filepriv *)f->priv;
    if (!fp) return VFS_ERR_INVAL;
    if (length > 0xFFFFFFFFull) return VFS_ERR_INVAL;

    struct fat32_openref *r = foref_by_id(fs, fp->oref_id);
    if (r && r->orphan) {
        /* Unlinked but still open: work from the table, and leave the
         * tombstoned entry alone. */
        uint32_t ofirst = r->head;
        int orc = fat_resize_chain(fs, &ofirst, r->size, length);
        if (orc != VFS_OK) return orc;
        r->head = ofirst;
        r->size = (uint32_t)length;
        fp->first_clus   = ofirst;
        fp->cur_clus     = ofirst;
        fp->cur_clus_idx = 0;
        f->size          = (size_t)length;
        return fat_flush(fs);
    }

    struct fat_dirent de;
    int rc = read_dirent_at(fs, fp->dir_clus, fp->dir_off, &de);
    if (rc != VFS_OK) return rc;

    /* de.file_size, not f->size: the entry we just read is the shared
     * truth, and resizing against a stale length would free the wrong
     * clusters. */
    uint32_t first = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    rc = fat_resize_chain(fs, &first, de.file_size, length);
    if (rc != VFS_OK) return rc;

    fp->first_clus   = first;
    fp->cur_clus     = first;
    fp->cur_clus_idx = 0;

    de.file_size   = (uint32_t)length;
    de.fst_clus_hi = (uint16_t)(first >> 16);
    de.fst_clus_lo = (uint16_t)(first & 0xFFFF);
    fat_stamp_write(&de, fat32_now_secs());
    rc = write_dirent_at(fs, fp->dir_clus, fp->dir_off, &de);
    if (rc == VFS_OK) f->size = (size_t)length;
    return rc;
}

static int fat32_utimes(void *mnt, const char *path, uint64_t mtime,
                        uint64_t atime) {
    struct fat32 *fs = (struct fat32 *)mnt;
    struct fat_dirent de;
    uint32_t dc = 0, doff = 0;
    int rc = path_walk(fs, path, &de, &dc, &doff);
    if (rc != VFS_OK) return rc;

    fat_stamp_write(&de, mtime);
    /* FAT keeps only a DATE for last access -- no time of day. Storing
     * the date and letting stat report midnight is the closest the
     * format allows; inventing an access time it cannot hold would be
     * worse. */
    uint16_t adate, atime_tod;
    fat_dos_encode(atime, &adate, &atime_tod);
    de.lst_acc_date = adate;
    (void)atime_tod;                     /* FAT has nowhere to put it */
    return write_dirent_at(fs, dc, doff, &de);
}

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
    out->mtime = fat_dos_decode(de.wrt_date, de.wrt_time);
    /* Last-access is a bare date on FAT, so this is midnight of that day
     * -- the precision the format has, not a number we made up. */
    out->atime = fat_dos_decode(de.lst_acc_date, 0);
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

    /* Count this handle BEFORE anyone can unlink the name out from under
     * it -- that count is what makes the deferred release possible. */
    int orc = foref_get(fs, dc, doff, &fp->oref_id);
    if (orc != VFS_OK) { kfree(fp); return orc; }

    out->priv = fp;
    out->pos  = 0;
    out->size = de.file_size;
    out->uid  = 0;
    out->gid  = 0;
    out->mode = 0;
    return VFS_OK;
}

static int fat32_close(struct vfs_file *f) {
    if (f->priv) {
        struct fat32 *fs = (struct fat32 *)f->mnt;
        struct fat32_filepriv *fp = (struct fat32_filepriv *)f->priv;
        uint32_t id = fp->oref_id;
        kfree(fp);
        f->priv = 0;
        /* Last close of an already-unlinked file is where its clusters
         * actually go back. */
        if (fs) foref_put(fs, id);
    }
    return VFS_OK;
}

/* On FAT the file's size and its first cluster live in the DIRECTORY
 * ENTRY, and every mutation -- this handle's, another handle's, a truncate
 * by path -- rewrites that entry on disk. So the entry is the shared truth
 * and the handle's cached copy goes stale the moment anyone else touches
 * the file. Same reasoning as fp_refresh() in ext4/ext2; the fields differ
 * because FAT has no inode. */
static int fat_fp_refresh(struct fat32 *fs, struct vfs_file *f,
                          struct fat32_filepriv *fp) {
    struct fat32_openref *r = foref_by_id(fs, fp->oref_id);
    if (r && r->orphan) {
        /* The name is gone and the slot may already belong to a new
         * file, so the entry is no longer this file's truth -- the table
         * is. Re-reading it here would silently adopt somebody else's
         * chain. */
        if (fp->first_clus != r->head) {
            fp->first_clus   = r->head;
            fp->cur_clus     = r->head;
            fp->cur_clus_idx = 0;
        }
        f->size = r->size;
        return VFS_OK;
    }
    struct fat_dirent de;
    int rc = read_dirent_at(fs, fp->dir_clus, fp->dir_off, &de);
    if (rc != VFS_OK) return rc;
    uint32_t first = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    if (first != fp->first_clus) {
        /* The chain moved (or appeared, or was released) -- the cached
         * cursor into it means nothing now. */
        fp->first_clus   = first;
        fp->cur_clus     = first;
        fp->cur_clus_idx = 0;
    }
    f->size = de.file_size;
    return VFS_OK;
}

static long fat32_read(struct vfs_file *f, void *buf, size_t n) {
    struct fat32 *fs = (struct fat32 *)f->mnt;
    struct fat32_filepriv *fp = (struct fat32_filepriv *)f->priv;
    if (!fp) return VFS_ERR_INVAL;
    int frc = fat_fp_refresh(fs, f, fp);   /* another handle may have grown it */
    if (frc != VFS_OK) return frc;
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
    /* This is only ever called because the file changed, so it is also
     * where the modification time belongs. */
    fat_stamp_write(de, fat32_now_secs());
    return write_cluster(fs, dir_clus, fs->clus_buf);
}

static long fat32_write(struct vfs_file *f, const void *buf, size_t n) {
    struct fat32 *fs = (struct fat32 *)f->mnt;
    struct fat32_filepriv *fp = (struct fat32_filepriv *)f->priv;
    if (!fp || n == 0) return 0;

    int frc = fat_fp_refresh(fs, f, fp);
    if (frc != VFS_OK) return frc;

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
    struct fat32_openref *r = foref_by_id(fs, fp->oref_id);
    if (r && r->orphan) {
        /* No name, so no entry to update -- and writing the old slot
         * would corrupt whatever file has been created into it since.
         * The table carries the length now. */
        r->head = fp->first_clus;
        r->size = (uint32_t)f->size;
        return (long)n;
    }
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
    {
        /* Stamp creation, modification and access. A zero date is not
         * "no date" in FAT -- it decodes to day 0 of month 0, which
         * every FAT tool flags as corrupt. */
        uint64_t now = fat32_now_secs();
        uint16_t date, time;
        fat_dos_encode(now, &date, &time);
        de->crt_date     = date;
        de->crt_time     = time;
        de->wrt_date     = date;
        de->wrt_time     = time;
        de->lst_acc_date = date;
    }
    rc = write_cluster(fs, slot_clus, fs->clus_buf);
    if (rc != 0) return VFS_ERR_IO;
    return VFS_OK;
}

/* ---- unlink ---- */

/* Tombstone a short dirent plus any LFN entries directly before it in
 * the same cluster (what fsck would do; missing LFNs are tolerated).
 * Re-reads the cluster itself, so callers need no buffer discipline. */
static int dirent_mark_free(struct fat32 *fs, uint32_t dc, uint32_t doff) {
    int rc = read_cluster(fs, dc, fs->clus_buf);
    if (rc != VFS_OK) return rc;
    fs->clus_buf[doff] = (uint8_t)FAT_DIR_FREE;
    int32_t pos = (int32_t)doff - 32;
    while (pos >= 0) {
        struct fat_dirent *prev = (struct fat_dirent *)(fs->clus_buf + pos);
        if ((prev->attr & FAT_ATTR_LFN) != FAT_ATTR_LFN) break;
        if (prev->name[0] == (uint8_t)FAT_DIR_FREE) break;
        prev->name[0] = (uint8_t)FAT_DIR_FREE;
        pos -= 32;
    }
    if (write_cluster(fs, dc, fs->clus_buf) != 0) return VFS_ERR_IO;
    return VFS_OK;
}

static int fat32_unlink(void *mnt, const char *path) {
    struct fat32 *fs = (struct fat32 *)mnt;
    struct fat_dirent de;
    uint32_t dc = 0, doff = 0;
    int rc = path_walk(fs, path, &de, &dc, &doff);
    if (rc != VFS_OK) return rc;
    if (de.attr & FAT_ATTR_DIRECTORY) return VFS_ERR_ISDIR;

    uint32_t head = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    struct fat32_openref *r = foref_find_live(fs, dc, doff);
    if (r) {
        /* Somebody still has it open. Tombstone the name below, but the
         * clusters belong to those handles until the last one closes --
         * remember what to free, and what the file's length was, since
         * the entry that held both is about to become unreadable. */
        r->orphan = true;
        r->head   = head;
        r->size   = de.file_size;
    } else if (head >= 2) {
        rc = free_chain(fs, head);
        if (rc != VFS_OK) return rc;
    }

    /* Mark the short entry deleted (factored for rename, Phase H). */
    rc = dirent_mark_free(fs, dc, doff);
    if (rc != VFS_OK) return rc;
    return fat_flush(fs);
}

/* Phase H: rename within the volume. FAT has no inode indirection --
 * a rename is "write a fresh dirent carrying the same cluster chain +
 * size, then tombstone the old one". SCOPE, stated like ext2's: a
 * DIRECTORY only renames within its parent (its ".." entry names the
 * parent cluster and moving it would leave that stale); files move
 * freely. leveldb/SQLite's temp-over-live rename works on FAT now. */
static int fat32_rename(void *mnt, const char *oldpath, const char *newpath) {
    struct fat32 *fs = (struct fat32 *)mnt;
    struct fat_dirent sde;
    uint32_t sdc = 0, sdoff = 0;
    int rc = path_walk(fs, oldpath, &sde, &sdc, &sdoff);
    if (rc != VFS_OK) return rc;

    char *np = 0; const char *nleaf = 0;
    rc = split_parent_leaf(newpath, &np, &nleaf);
    if (rc != VFS_OK) return rc;
    struct fat_dirent pde;
    rc = path_walk(fs, np, &pde, 0, 0);
    kfree(np);
    if (rc != VFS_OK) return rc;
    if (!(pde.attr & FAT_ATTR_DIRECTORY)) return VFS_ERR_NOTDIR;
    uint32_t pclus = ((uint32_t)pde.fst_clus_hi << 16) | pde.fst_clus_lo;
    if (pclus < 2) return VFS_ERR_NOENT;

    char *op = 0; const char *oleaf = 0;
    rc = split_parent_leaf(oldpath, &op, &oleaf);
    if (rc != VFS_OK) return rc;
    struct fat_dirent opde;
    rc = path_walk(fs, op, &opde, 0, 0);
    kfree(op);
    if (rc != VFS_OK) return rc;
    uint32_t opclus = ((uint32_t)opde.fst_clus_hi << 16) | opde.fst_clus_lo;
    if ((sde.attr & FAT_ATTR_DIRECTORY) && opclus != pclus)
        return VFS_ERR_INVAL;

    /* Clobber an existing destination FILE, as rename(2) requires. */
    {
        struct fat_dirent dde;
        uint32_t ddc = 0, ddoff = 0;
        int drc = path_walk(fs, newpath, &dde, &ddc, &ddoff);
        if (drc == VFS_OK) {
            if (ddc == sdc && ddoff == sdoff) return VFS_OK;  /* same entry */
            if (dde.attr & FAT_ATTR_DIRECTORY) return VFS_ERR_INVAL;
            rc = fat32_unlink(mnt, newpath);
            if (rc != VFS_OK) return rc;
            /* The unlink rewrote directory clusters: re-walk the source
             * so (cluster, offset) are fresh, not stale-buffer guesses. */
            rc = path_walk(fs, oldpath, &sde, &sdc, &sdoff);
            if (rc != VFS_OK) return rc;
        } else if (drc != VFS_ERR_NOENT) {
            return drc;
        }
    }

    uint8_t name11[11];
    bool tilde = false;
    (void)shortname_encode(nleaf, name11, &tilde);
    rc = disambiguate_short(fs, pclus, name11);
    if (rc != VFS_OK) return rc;
    uint32_t slot_clus, slot_off;
    rc = dir_find_free_slot(fs, pclus, &slot_clus, &slot_off);
    if (rc != VFS_OK) return rc;
    rc = read_cluster(fs, slot_clus, fs->clus_buf);
    if (rc != VFS_OK) return rc;
    struct fat_dirent *nde = (struct fat_dirent *)(fs->clus_buf + slot_off);
    memcpy(nde, &sde, sizeof *nde);    /* same chain, size, attrs, times */
    memcpy(nde->name, name11, 11);
    if (write_cluster(fs, slot_clus, fs->clus_buf) != 0) return VFS_ERR_IO;

    rc = dirent_mark_free(fs, sdc, sdoff);   /* re-reads: order-safe */
    if (rc != VFS_OK) return rc;
    return fat_flush(fs);
}

/* ---- mkdir / chmod / chown -- not supported in 23B ---- */

/* Create a directory. This was a stub returning VFS_ERR_ROFS while being
 * listed in the ops table, so `mkdir` on a FAT volume had never worked --
 * invisible until fsmatrix could build a FAT fixture to ask. A stick you
 * format for another machine is not much use if it cannot hold a folder. */
static int fat32_mkdir(void *mnt, const char *path,
                       uint32_t uid, uint32_t gid, uint32_t mode) {
    (void)uid; (void)gid; (void)mode;      /* FAT has no permissions */
    struct fat32 *fs = (struct fat32 *)mnt;

    char *parent = 0;
    const char *leaf = 0;
    int rc = split_parent_leaf(path, &parent, &leaf);
    if (rc != VFS_OK) return rc;

    struct fat_dirent pde;
    rc = path_walk(fs, parent, &pde, 0, 0);
    kfree(parent); parent = 0;
    if (rc != VFS_OK) return rc;
    if (!(pde.attr & FAT_ATTR_DIRECTORY)) return VFS_ERR_NOTDIR;
    uint32_t pclus = ((uint32_t)pde.fst_clus_hi << 16) | pde.fst_clus_lo;
    if (pclus < 2) return VFS_ERR_NOENT;

    {
        struct lookup_ctx ctx = { .want = leaf, .found = false };
        rc = dir_walk(fs, pclus, lookup_cb, &ctx);
        if (rc != VFS_OK) return rc;
        if (ctx.found) return VFS_ERR_EXIST;
    }

    uint8_t name11[11];
    bool needs_tilde = false;
    (void)shortname_encode(leaf, name11, &needs_tilde);
    rc = disambiguate_short(fs, pclus, name11);
    if (rc != VFS_OK) return rc;

    /* One cluster for the new directory. alloc_cluster zeroes it, which
     * matters here: a zero first byte is what ends a directory scan, so a
     * zeroed cluster IS an empty directory. */
    uint32_t nclus = 0;
    rc = alloc_cluster(fs, &nclus);
    if (rc != VFS_OK) return rc;

    uint32_t slot_clus = 0, slot_off = 0;
    rc = dir_find_free_slot(fs, pclus, &slot_clus, &slot_off);
    if (rc != VFS_OK) { (void)free_chain(fs, nclus); return rc; }

    uint64_t now = fat32_now_secs();
    uint16_t date, time;
    fat_dos_encode(now, &date, &time);

    /* "." and ".." -- required by the spec, and what every other FAT
     * implementation walks to get back up the tree. */
    memset(fs->clus_buf, 0, fs->cluster_bytes);
    {
        struct fat_dirent *dot = (struct fat_dirent *)fs->clus_buf;
        struct fat_dirent *dd  = dot + 1;

        memset(dot->name, ' ', 11); dot->name[0] = '.';
        dot->attr        = FAT_ATTR_DIRECTORY;
        dot->fst_clus_hi = (uint16_t)(nclus >> 16);
        dot->fst_clus_lo = (uint16_t)(nclus & 0xFFFF);
        dot->crt_date = date; dot->crt_time = time;
        dot->wrt_date = date; dot->wrt_time = time;
        dot->lst_acc_date = date;

        memset(dd->name, ' ', 11); dd->name[0] = '.'; dd->name[1] = '.';
        dd->attr = FAT_ATTR_DIRECTORY;
        /* The spec is specific about this one: when the parent IS the
         * root, ".." stores cluster 0, not the root's real number. */
        uint32_t up = (pclus == fs->root_clus) ? 0u : pclus;
        dd->fst_clus_hi = (uint16_t)(up >> 16);
        dd->fst_clus_lo = (uint16_t)(up & 0xFFFF);
        dd->crt_date = date; dd->crt_time = time;
        dd->wrt_date = date; dd->wrt_time = time;
        dd->lst_acc_date = date;
    }
    if (write_cluster(fs, nclus, fs->clus_buf) != 0) {
        (void)free_chain(fs, nclus);
        return VFS_ERR_IO;
    }

    /* Then the entry that names it in the parent. Done last so a failure
     * anywhere above leaves no half-linked directory behind. */
    rc = read_cluster(fs, slot_clus, fs->clus_buf);
    if (rc != VFS_OK) { (void)free_chain(fs, nclus); return rc; }
    {
        struct fat_dirent *de = (struct fat_dirent *)(fs->clus_buf + slot_off);
        memset(de, 0, sizeof(*de));
        memcpy(de->name, name11, 11);
        de->attr        = FAT_ATTR_DIRECTORY;
        de->fst_clus_hi = (uint16_t)(nclus >> 16);
        de->fst_clus_lo = (uint16_t)(nclus & 0xFFFF);
        de->file_size   = 0;               /* always 0 for a directory */
        de->crt_date = date; de->crt_time = time;
        de->wrt_date = date; de->wrt_time = time;
        de->lst_acc_date = date;
    }
    if (write_cluster(fs, slot_clus, fs->clus_buf) != 0) {
        (void)free_chain(fs, nclus);
        return VFS_ERR_IO;
    }
    return fat_flush(fs);
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
/* Phase H: real numbers from the FAT itself -- entry 0 means free.
 * Sector-size clusters as bsize; a full FAT scan is bounded by the
 * volume size and only runs when someone actually asks. */
static int fat32_statfs(void *mnt, struct vfs_statfs *out) {
    struct fat32 *fs = (struct fat32 *)mnt;
    uint64_t freec = 0;
    for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
        uint32_t v;
        if (fat_get(fs, c, &v) == VFS_OK && v == 0) freec++;
    }
    /* Counts are in CLUSTERS, so the block size must be the cluster size.
     * Pairing cluster counts with a hardcoded 4096 was only right when
     * the cluster happened to be 4 KiB and misreported the volume by the
     * ratio otherwise. */
    out->bsize      = fs->cluster_bytes;
    out->blocks     = fs->cluster_count;
    out->bfree      = freec;
    out->files      = 0;                     /* FAT has no inode table */
    out->ffree      = 0;
    out->type_magic = 0x4d44;                /* MSDOS_SUPER_MAGIC */
    out->namelen    = 255;
    return VFS_OK;
}

/* fsync(2): the block cache is WRITE-BACK, so a successful write is not
 * yet on the medium. Without this, fsync() returned 0 while the bytes sat
 * in RAM -- which is the difference between "saved" and "saved until you
 * unplug it". */
static int fat32_sync(void *mnt) {
    struct fat32 *fs = (struct fat32 *)mnt;
    if (!fs || !fs->dev) return VFS_OK;
    (void)fat_flush(fs);          /* our own FAT sector cache first */
    extern void bcache_sync(struct blk_dev *dev);
    bcache_sync(fs->dev);
    return blk_flush(fs->dev) == 0 ? VFS_OK : VFS_ERR_IO;
}

const struct vfs_ops fat32_ops = {
    .open     = fat32_open,
    .close    = fat32_close,
    .read     = fat32_read,
    .write    = fat32_write,
    .create   = fat32_create,
    .unlink   = fat32_unlink,
    .rename   = fat32_rename,   /* Phase H */
    .mkdir    = fat32_mkdir,
    .opendir  = fat32_opendir,
    .closedir = fat32_closedir,
    .readdir  = fat32_readdir,
    .stat     = fat32_stat,
    .chmod     = 0,   /* FAT has no perms */
    .chown     = 0,
    .utimes    = fat32_utimes,
    .truncate  = fat32_truncate,
    .ftruncate = fat32_ftruncate,
    .umount    = fat32_umount,
    /* fat32_statfs() existed for a long time and was never listed here,
     * so `df` on a FAT mount answered EROFS from a function that was
     * sitting right above the table. */
    .statfs    = fat32_statfs,
    .sync      = fat32_sync,
};

struct blk_dev *fat32_blkdev_of(void *mnt) {
    if (!mnt) return 0;
    return ((struct fat32 *)mnt)->dev;
}


/* ================================================================
 * fat32_format -- make a FAT32 volume.
 *
 * WHY: two reasons, and the second is the load-bearing one.
 *
 *   1. A USB stick formatted as tobyfs is readable by exactly one
 *      operating system. FAT32 is the format every other machine can
 *      already read, which is what you actually want on a stick you
 *      carry between them.
 *   2. Nothing could TEST the FAT driver. There was no way to conjure a
 *      FAT volume in the kernel, so fsmatrix had to declare fat32 an
 *      uncovered hole -- and the truncate/utimes/statfs work of
 *      2026-08-25 would have shipped with nothing having executed a
 *      single line of it.
 *
 * The layout follows Microsoft's FAT specification: boot sector + FSInfo
 * in the reserved region, a backup of both at sector 6, two FATs, then
 * the data region with the root directory in cluster 2.
 * ================================================================ */

/* Cluster size by volume size, following the table mkfs.vfat uses. Also
 * bounded by what this driver's mount path accepts (<= 16 sectors, so a
 * cluster buffer stays <= 8 KiB). */
static uint32_t fat32_pick_spc(uint64_t total_sec) {
    if (total_sec <=   532480ull) return 1;    /* <= 260 MiB */
    if (total_sec <= 16777216ull) return 8;    /* <= 8 GiB   */
    return 16;                                 /* larger: 8 KiB clusters */
}

int fat32_format(struct blk_dev *dev) {
    if (!dev) return VFS_ERR_INVAL;
    uint64_t total_sec = dev->sector_count;
    if (total_sec < 8192) return VFS_ERR_INVAL;      /* far too small */
    if (total_sec > 0xFFFFFFFFull) total_sec = 0xFFFFFFFFull;

    const uint32_t bps       = 512;
    const uint32_t rsvd      = 32;
    const uint32_t num_fats  = 2;
    uint32_t spc = fat32_pick_spc(total_sec);

    /* Solve for the FAT size. Each FAT entry is 4 bytes and must cover
     * every data cluster plus the two reserved entries; the FAT itself
     * eats sectors that are then not available for data, so this is
     * iterative rather than a closed form. Converges in a couple of
     * rounds -- the loop bound is a backstop, not the exit condition. */
    uint32_t fat_sz = 1, clusters = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t data_sec = total_sec - rsvd - (uint64_t)fat_sz * num_fats;
        uint32_t want_clusters = (uint32_t)(data_sec / spc);
        uint32_t want_fat_sz =
            (uint32_t)(((uint64_t)(want_clusters + 2) * 4 + bps - 1) / bps);
        if (want_fat_sz == fat_sz) { clusters = want_clusters; break; }
        fat_sz = want_fat_sz;
        clusters = want_clusters;
    }
    if (fat_sz == 0) return VFS_ERR_INVAL;

    /* FAT32 is DEFINED as "more than 65524 clusters" -- below that the
     * spec says the volume is FAT16 and other systems will read it that
     * way. Shrinking the cluster is the fix when there is room for it;
     * refusing beats writing a superblock that lies about its own type. */
    while (clusters <= 65524 && spc > 1) {
        spc /= 2;
        fat_sz = 1;
        for (int i = 0; i < 16; i++) {
            uint64_t data_sec = total_sec - rsvd - (uint64_t)fat_sz * num_fats;
            uint32_t wc = (uint32_t)(data_sec / spc);
            uint32_t wf = (uint32_t)(((uint64_t)(wc + 2) * 4 + bps - 1) / bps);
            if (wf == fat_sz) { clusters = wc; break; }
            fat_sz = wf;
            clusters = wc;
        }
    }
    if (clusters <= 65524) return VFS_ERR_INVAL;     /* genuinely too small */

    uint8_t *sec = kcalloc(1, bps);
    if (!sec) return VFS_ERR_NOMEM;

    /* ---- boot sector ---- */
    struct fat32_bpb *b = (struct fat32_bpb *)sec;
    b->jmp[0] = 0xEB; b->jmp[1] = 0x58; b->jmp[2] = 0x90;
    memcpy(b->oem, "TOBYOS  ", 8);
    b->bytes_per_sec = (uint16_t)bps;
    b->sec_per_clus  = (uint8_t)spc;
    b->rsvd_sec_cnt  = (uint16_t)rsvd;
    b->num_fats      = (uint8_t)num_fats;
    b->root_ent_cnt  = 0;                 /* must be 0 on FAT32 */
    b->tot_sec16     = 0;
    b->media         = 0xF8;
    b->fat_sz16      = 0;                 /* must be 0 on FAT32 */
    b->sec_per_trk   = 63;
    b->num_heads     = 255;
    b->hidd_sec      = 0;
    b->tot_sec32     = (uint32_t)total_sec;
    b->fat_sz32      = fat_sz;
    b->ext_flags     = 0;                 /* mirror all FATs */
    b->fs_ver        = 0;
    b->root_clus     = 2;
    b->fs_info       = 1;
    b->bk_boot_sec   = 6;
    b->drv_num       = 0x80;
    b->boot_sig      = 0x29;
    /* A volume id derived from the clock. Not a serial number anyone can
     * rely on, which is exactly what it is on every other system too. */
    b->vol_id        = (uint32_t)(fat32_now_secs() ^ (total_sec * 2654435761u));
    memcpy(b->vol_lab, "TOBYOS     ", 11);
    memcpy(b->fs_type, "FAT32   ", 8);
    sec[510] = 0x55; sec[511] = 0xAA;

    int rc = VFS_OK;
    if (blk_write(dev, 0, 1, sec) != 0) { rc = VFS_ERR_IO; goto done; }
    if (blk_write(dev, 6, 1, sec) != 0) { rc = VFS_ERR_IO; goto done; }

    /* ---- FSInfo (and its backup at 6 + 1) ---- */
    memset(sec, 0, bps);
    {
        struct fat32_fsinfo *fi = (struct fat32_fsinfo *)sec;
        fi->lead_sig   = 0x41615252u;
        fi->struct_sig = 0x61417272u;
        fi->free_count = clusters - 1;     /* cluster 2 holds the root dir */
        fi->nxt_free   = 3;
        fi->trail_sig  = 0xAA550000u;
    }
    if (blk_write(dev, 1, 1, sec) != 0) { rc = VFS_ERR_IO; goto done; }
    if (blk_write(dev, 7, 1, sec) != 0) { rc = VFS_ERR_IO; goto done; }

    /* ---- the FATs ----
     * Zero every sector, then stamp the three live entries into sector 0
     * of each copy. Writing one sector at a time keeps the memory cost
     * flat regardless of how big the volume is. */
    memset(sec, 0, bps);
    for (uint32_t f = 0; f < num_fats; f++) {
        uint64_t base = rsvd + (uint64_t)f * fat_sz;
        for (uint32_t i = 1; i < fat_sz; i++) {
            if (blk_write(dev, base + i, 1, sec) != 0) {
                rc = VFS_ERR_IO; goto done;
            }
        }
    }
    {
        uint32_t *e = (uint32_t *)sec;
        e[0] = 0x0FFFFFF8u;                /* media byte + reserved bits */
        e[1] = 0xFFFFFFFFu;                /* end-of-chain marker slot */
        e[2] = 0x0FFFFFF8u;                /* the root directory: one cluster */
        for (uint32_t f = 0; f < num_fats; f++) {
            if (blk_write(dev, rsvd + (uint64_t)f * fat_sz, 1, sec) != 0) {
                rc = VFS_ERR_IO; goto done;
            }
        }
    }

    /* ---- the root directory cluster ----
     * Must be all zeroes: a zero first byte is what marks a directory
     * slot free-and-nothing-follows, so this is an EMPTY root rather
     * than whatever the medium happened to hold. */
    memset(sec, 0, bps);
    {
        uint64_t data_lba = rsvd + (uint64_t)fat_sz * num_fats;
        for (uint32_t i = 0; i < spc; i++) {
            if (blk_write(dev, data_lba + i, 1, sec) != 0) {
                rc = VFS_ERR_IO; goto done;
            }
        }
    }
    blk_flush(dev);

done:
    kfree(sec);
    return rc;
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
