/* sys_restore.c -- filesystem snapshots for rollback. */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/sys_restore.h>
#include <tobyos/spinlock.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/perf.h>

int  notify_svc_send(int pid, const char *title, const char *body, const char *icon);

#define RESTORE_INDEX_PATH "/data/restore/index.dat"
#define RESTORE_DATA_PATH  "/data/restore/"

static void sr_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void sr_copyn(char *dst, size_t cap, const char *src, size_t n) {
    size_t i = 0;
    while (i + 1 < cap && i < n && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static char *sr_strchr(const char *s, int c) {
    while (*s) { if (*s == c) return (char *)s; s++; }
    return NULL;
}

static struct restore_point_info g_points[RESTORE_MAX_POINTS];
static int g_point_count;
static uint32_t g_next_id = 1;
static spinlock_t g_restore_lock = SPINLOCK_INIT;

static const char *g_critical_paths[] = {
    "/system/config/startup.conf",
    "/system/config/network.conf",
    "/system/config/apps.conf",
    "/system/settings.conf",
    NULL
};

static void save_index(void) {
    char buf[2048];
    int off = 0;
    for (int i = 0; i < g_point_count && off < (int)sizeof(buf) - 128; i++) {
        struct restore_point_info *p = &g_points[i];
        off += ksnprintf(buf + off, sizeof(buf) - off, "%u|%u|%s|%u\n",
                         p->id, p->timestamp, p->description, p->used_blocks);
    }
    vfs_write_all(RESTORE_INDEX_PATH, buf, off);
}

static void load_index(void) {
    void *raw = NULL;
    size_t sz = 0;
    if (vfs_read_all(RESTORE_INDEX_PATH, &raw, &sz) != VFS_OK || !raw) return;

    char *buf = (char *)raw;
    g_point_count = 0;
    char *line = buf;
    while (*line && g_point_count < RESTORE_MAX_POINTS) {
        char *eol = sr_strchr(line, '\n');
        if (eol) *eol = '\0';
        if (*line == '\0') { line = eol ? eol + 1 : line + strlen(line); continue; }

        struct restore_point_info *p = &g_points[g_point_count];
        memset(p, 0, sizeof(*p));

        char *f = line;
        while (*f >= '0' && *f <= '9') { p->id = p->id * 10 + (uint32_t)(*f - '0'); f++; }
        if (*f == '|') f++;
        while (*f >= '0' && *f <= '9') { p->timestamp = p->timestamp * 10 + (uint32_t)(*f - '0'); f++; }
        if (*f == '|') f++;
        char *desc_end = sr_strchr(f, '|');
        if (desc_end) {
            sr_copyn(p->description, sizeof(p->description), f, (size_t)(desc_end - f));
            f = desc_end + 1;
        }
        while (*f >= '0' && *f <= '9') { p->used_blocks = p->used_blocks * 10 + (uint32_t)(*f - '0'); f++; }

        if (p->id >= g_next_id) g_next_id = p->id + 1;
        g_point_count++;
        line = eol ? eol + 1 : line + strlen(line);
    }
    kfree(raw);
}

int restore_create_point(const char *description) {
    uint64_t flags = spin_lock_irqsave(&g_restore_lock);

    if (g_point_count >= RESTORE_MAX_POINTS) {
        memmove(&g_points[0], &g_points[1],
                (RESTORE_MAX_POINTS - 1) * sizeof(g_points[0]));
        g_point_count = RESTORE_MAX_POINTS - 1;
    }

    uint32_t id = g_next_id++;
    struct restore_point_info *p = &g_points[g_point_count];
    memset(p, 0, sizeof(*p));
    p->id = id;
    p->timestamp = (uint32_t)(perf_now_ns() / 1000000000ULL);
    if (description)
        sr_copy(p->description, sizeof(p->description), description);

    uint32_t total_size = 0;
    for (int i = 0; g_critical_paths[i]; i++) {
        void *content = NULL;
        size_t csz = 0;
        if (vfs_read_all(g_critical_paths[i], &content, &csz) == VFS_OK && content) {
            char snap_path[256];
            ksnprintf(snap_path, sizeof(snap_path), "%s%u_%d.snap",
                      RESTORE_DATA_PATH, id, i);
            vfs_write_all(snap_path, content, csz);
            total_size += (uint32_t)csz;
            kfree(content);
        }
    }
    p->used_blocks = (total_size + 511) / 512;
    g_point_count++;

    save_index();
    spin_unlock_irqrestore(&g_restore_lock, flags);

    kprintf("[restore] created point %u: \"%s\" (%u blocks)\n",
            id, description ? description : "", p->used_blocks);
    return (int)id;
}

int restore_list_points(struct restore_point_info *out, int max) {
    uint64_t flags = spin_lock_irqsave(&g_restore_lock);
    int n = g_point_count < max ? g_point_count : max;
    memcpy(out, g_points, n * sizeof(g_points[0]));
    spin_unlock_irqrestore(&g_restore_lock, flags);
    return n;
}

int restore_rollback(uint32_t id) {
    uint64_t flags = spin_lock_irqsave(&g_restore_lock);
    int idx = -1;
    for (int i = 0; i < g_point_count; i++) {
        if (g_points[i].id == id) { idx = i; break; }
    }
    if (idx < 0) {
        spin_unlock_irqrestore(&g_restore_lock, flags);
        return -1;
    }

    for (int i = 0; g_critical_paths[i]; i++) {
        char snap_path[256];
        ksnprintf(snap_path, sizeof(snap_path), "%s%u_%d.snap",
                  RESTORE_DATA_PATH, id, i);
        void *content = NULL;
        size_t csz = 0;
        if (vfs_read_all(snap_path, &content, &csz) == VFS_OK && content) {
            vfs_write_all(g_critical_paths[i], content, csz);
            kfree(content);
        }
    }

    spin_unlock_irqrestore(&g_restore_lock, flags);
    kprintf("[restore] rolled back to point %u\n", id);
    notify_svc_send(-1, "System Restored", g_points[idx].description, "restore");
    return 0;
}

int restore_delete_point(uint32_t id) {
    uint64_t flags = spin_lock_irqsave(&g_restore_lock);
    for (int i = 0; i < g_point_count; i++) {
        if (g_points[i].id == id) {
            memmove(&g_points[i], &g_points[i + 1],
                    (g_point_count - i - 1) * sizeof(g_points[0]));
            g_point_count--;
            save_index();
            spin_unlock_irqrestore(&g_restore_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_restore_lock, flags);
    return -1;
}

void sys_restore_init(void) {
    memset(g_points, 0, sizeof(g_points));
    g_point_count = 0;
    g_next_id = 1;
    load_index();
    kprintf("[restore] system restore initialized (%d points loaded)\n", g_point_count);
}
