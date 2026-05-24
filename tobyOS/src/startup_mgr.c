/* startup_mgr.c -- control which apps launch at login. */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/startup_mgr.h>
#include <tobyos/spinlock.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>

int  gui_launch_enqueue_arg(const char *path, const char *arg);

#define CONF_PATH "/system/config/startup.conf"

static void sm_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void sm_copyn(char *dst, size_t cap, const char *src, size_t n) {
    size_t i = 0;
    while (i + 1 < cap && i < n && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static char *sm_strchr(const char *s, int c) {
    while (*s) { if (*s == c) return (char *)s; s++; }
    return NULL;
}

static struct startup_entry g_entries[STARTUP_MAX];
static int g_count;
static spinlock_t g_startup_lock = SPINLOCK_INIT;

static void load_config(void) {
    void *raw = NULL;
    size_t sz = 0;
    if (vfs_read_all(CONF_PATH, &raw, &sz) != VFS_OK || !raw) return;

    char *buf = (char *)raw;
    g_count = 0;
    char *line = buf;
    while (*line && g_count < STARTUP_MAX) {
        char *eol = sm_strchr(line, '\n');
        if (eol) *eol = '\0';
        if (line[0] == '\0' || line[0] == '#') {
            line = eol ? eol + 1 : line + strlen(line);
            continue;
        }

        struct startup_entry *e = &g_entries[g_count];
        memset(e, 0, sizeof(*e));

        char *f1 = line;
        char *f2 = sm_strchr(f1, '|');
        if (!f2) goto next;
        sm_copyn(e->name, sizeof(e->name), f1, (size_t)(f2 - f1));
        f2++;
        char *f3 = sm_strchr(f2, '|');
        if (!f3) goto next;
        sm_copyn(e->path, sizeof(e->path), f2, (size_t)(f3 - f2));
        f3++;
        char *f4 = sm_strchr(f3, '|');
        if (!f4) goto next;
        e->enabled = (*f3 == '1');
        f4++;
        e->delay_ms = 0;
        for (char *p = f4; *p >= '0' && *p <= '9'; p++)
            e->delay_ms = e->delay_ms * 10 + (*p - '0');
        g_count++;

next:
        line = eol ? eol + 1 : line + strlen(line);
    }
    kfree(raw);
}

static void save_config(void) {
    char buf[2048];
    int off = 0;
    for (int i = 0; i < g_count && off < (int)sizeof(buf) - 128; i++) {
        struct startup_entry *e = &g_entries[i];
        off += ksnprintf(buf + off, sizeof(buf) - off, "%s|%s|%d|%d\n",
                         e->name, e->path, e->enabled, e->delay_ms);
    }
    vfs_write_all(CONF_PATH, buf, off);
}

int startup_add(const char *name, const char *path, int delay_ms) {
    if (!name || !path) return -1;
    uint64_t flags = spin_lock_irqsave(&g_startup_lock);
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].name, name) == 0) {
            spin_unlock_irqrestore(&g_startup_lock, flags);
            return -1;
        }
    }
    if (g_count >= STARTUP_MAX) {
        spin_unlock_irqrestore(&g_startup_lock, flags);
        return -1;
    }
    struct startup_entry *e = &g_entries[g_count++];
    memset(e, 0, sizeof(*e));
    sm_copy(e->name, sizeof(e->name), name);
    sm_copy(e->path, sizeof(e->path), path);
    e->enabled = 1;
    e->delay_ms = delay_ms > 0 ? delay_ms : 0;
    save_config();
    spin_unlock_irqrestore(&g_startup_lock, flags);
    kprintf("[startup] added: %s -> %s (delay=%dms)\n", name, path, delay_ms);
    return 0;
}

int startup_remove(const char *name) {
    if (!name) return -1;
    uint64_t flags = spin_lock_irqsave(&g_startup_lock);
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].name, name) == 0) {
            memmove(&g_entries[i], &g_entries[i + 1],
                    (g_count - i - 1) * sizeof(g_entries[0]));
            g_count--;
            save_config();
            spin_unlock_irqrestore(&g_startup_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_startup_lock, flags);
    return -1;
}

int startup_enable(const char *name, int enabled) {
    if (!name) return -1;
    uint64_t flags = spin_lock_irqsave(&g_startup_lock);
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].name, name) == 0) {
            g_entries[i].enabled = !!enabled;
            save_config();
            spin_unlock_irqrestore(&g_startup_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_startup_lock, flags);
    return -1;
}

int startup_list(struct startup_entry *out, int max) {
    uint64_t flags = spin_lock_irqsave(&g_startup_lock);
    int n = g_count < max ? g_count : max;
    memcpy(out, g_entries, n * sizeof(g_entries[0]));
    spin_unlock_irqrestore(&g_startup_lock, flags);
    return n;
}

void startup_run_all(void) {
    kprintf("[startup] launching startup apps (%d entries)\n", g_count);
    for (int i = 0; i < g_count; i++) {
        if (!g_entries[i].enabled) continue;
        kprintf("[startup] launching: %s (%s)\n", g_entries[i].name, g_entries[i].path);
        gui_launch_enqueue_arg(g_entries[i].path, NULL);
    }
}

void startup_mgr_init(void) {
    memset(g_entries, 0, sizeof(g_entries));
    g_count = 0;
    load_config();
    kprintf("[startup] manager initialized (%d entries loaded)\n", g_count);
}
