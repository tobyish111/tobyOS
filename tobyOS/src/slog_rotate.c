/* slog_rotate.c -- Log rotation and enhanced query support.
 *
 * Extends the existing slog.c with:
 *   - Log rotation when log exceeds 64KB
 *   - Per-subsystem log levels
 *   - Structured log format
 *   - Log query API
 *   - Ring buffer for last 1024 entries (in-memory for GUI viewer)
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/heap.h>
#include <tobyos/vfs.h>
#include <tobyos/pit.h>
#include <tobyos/slog.h>
#include <tobyos/spinlock.h>

#define SLOG_ROTATE_MAX_SIZE   (64 * 1024)
#define SLOG_ROTATE_MAX_ARCHIVES 4
#define SLOG_QUERY_RING_SIZE   1024
#define SLOG_MAX_SUBSYSTEMS    32
#define SLOG_SUB_NAME_MAX      16

struct slog_subsystem_config {
    char     name[SLOG_SUB_NAME_MAX];
    uint32_t min_level;
    bool     active;
};

struct slog_query_entry {
    uint64_t timestamp_ms;
    uint32_t level;
    int32_t  pid;
    char     subsystem[12];
    char     message[192];
};

static struct slog_subsystem_config g_subsystems[SLOG_MAX_SUBSYSTEMS];
static int g_subsystem_count = 0;

static struct slog_query_entry g_query_ring[SLOG_QUERY_RING_SIZE];
static uint32_t g_query_head = 0;
static uint32_t g_query_count = 0;
static spinlock_t g_query_lock = SPINLOCK_INIT;

static uint64_t g_current_log_size = 0;
static uint32_t g_rotate_count = 0;
static bool g_rotate_initialized = false;

static uint64_t rotate_now_ms(void) {
    uint32_t hz = pit_hz();
    if (!hz) return 0;
    return (pit_ticks() * 1000ULL) / hz;
}

void slog_rotate_init(void) {
    if (g_rotate_initialized) return;
    memset(g_subsystems, 0, sizeof(g_subsystems));
    memset(g_query_ring, 0, sizeof(g_query_ring));
    g_query_head = 0;
    g_query_count = 0;
    g_current_log_size = 0;
    g_rotate_count = 0;
    g_subsystem_count = 0;
    g_rotate_initialized = true;
    kprintf("[slog_rotate] log rotation initialized (max=%uKB, archives=%d)\n",
            SLOG_ROTATE_MAX_SIZE / 1024, SLOG_ROTATE_MAX_ARCHIVES);
}

int slog_rotate_set_subsystem_level(const char *subsystem, uint32_t level) {
    if (!subsystem) return -1;

    /* Find existing or allocate new slot */
    for (int i = 0; i < g_subsystem_count; i++) {
        if (strcmp(g_subsystems[i].name, subsystem) == 0) {
            g_subsystems[i].min_level = level;
            return 0;
        }
    }
    if (g_subsystem_count >= SLOG_MAX_SUBSYSTEMS) return -1;

    struct slog_subsystem_config *s = &g_subsystems[g_subsystem_count];
    size_t len = strlen(subsystem);
    if (len >= SLOG_SUB_NAME_MAX) len = SLOG_SUB_NAME_MAX - 1;
    memcpy(s->name, subsystem, len);
    s->name[len] = '\0';
    s->min_level = level;
    s->active = true;
    g_subsystem_count++;
    return 0;
}

uint32_t slog_rotate_get_subsystem_level(const char *subsystem) {
    if (!subsystem) return ABI_SLOG_LEVEL_DEBUG;
    for (int i = 0; i < g_subsystem_count; i++) {
        if (strcmp(g_subsystems[i].name, subsystem) == 0) {
            return g_subsystems[i].min_level;
        }
    }
    return ABI_SLOG_LEVEL_DEBUG;
}

bool slog_rotate_should_log(const char *subsystem, uint32_t level) {
    uint32_t min = slog_rotate_get_subsystem_level(subsystem);
    return level <= min;
}

void slog_rotate_record(uint32_t level, int32_t pid,
                        const char *subsystem, const char *message) {
    if (!g_rotate_initialized) return;

    uint64_t flags = spin_lock_irqsave(&g_query_lock);

    struct slog_query_entry *e = &g_query_ring[g_query_head];
    e->timestamp_ms = rotate_now_ms();
    e->level = level;
    e->pid = pid;

    if (subsystem) {
        size_t slen = strlen(subsystem);
        if (slen > 11) slen = 11;
        memcpy(e->subsystem, subsystem, slen);
        e->subsystem[slen] = '\0';
    } else {
        e->subsystem[0] = '\0';
    }

    if (message) {
        size_t mlen = strlen(message);
        if (mlen > 191) mlen = 191;
        memcpy(e->message, message, mlen);
        e->message[mlen] = '\0';
    } else {
        e->message[0] = '\0';
    }

    g_query_head = (g_query_head + 1) % SLOG_QUERY_RING_SIZE;
    if (g_query_count < SLOG_QUERY_RING_SIZE)
        g_query_count++;

    spin_unlock_irqrestore(&g_query_lock, flags);
}

static void copy_file(const char *src, const char *dst) {
    void *data = NULL;
    size_t size = 0;
    if (vfs_read_all(src, &data, &size) == 0 && data) {
        vfs_write_all(dst, data, size);
        kfree(data);
    }
}

static int do_rotate(void) {
    /* Rotate archives: .3 <- .2 <- .1 <- current */
    char old_path[64], new_path[64];
    for (int i = SLOG_ROTATE_MAX_ARCHIVES - 1; i > 0; i--) {
        ksnprintf(old_path, sizeof(old_path), "/data/system.log.%d", i);
        ksnprintf(new_path, sizeof(new_path), "/data/system.log.%d", i + 1);
        copy_file(old_path, new_path);
        vfs_unlink(old_path);
    }
    copy_file(SLOG_PERSIST_PATH, "/data/system.log.1");
    vfs_unlink(SLOG_PERSIST_PATH);

    g_current_log_size = 0;
    g_rotate_count++;
    kprintf("[slog_rotate] log rotated (count=%u)\n", (unsigned)g_rotate_count);
    SLOG_INFO("slog", "log rotated (archive #%u)", (unsigned)g_rotate_count);
    return 0;
}

int slog_rotate_check(void) {
    if (!g_rotate_initialized) return 0;
    if (g_current_log_size >= SLOG_ROTATE_MAX_SIZE) {
        return do_rotate();
    }
    return 0;
}

void slog_rotate_add_bytes(size_t bytes) {
    g_current_log_size += bytes;
}

int slog_query(const char *subsystem, uint32_t max_level,
               uint64_t since_ms,
               struct slog_query_entry *out, uint32_t cap) {
    if (!out || cap == 0) return 0;

    uint32_t written = 0;
    uint64_t flags = spin_lock_irqsave(&g_query_lock);

    uint32_t start;
    if (g_query_count < SLOG_QUERY_RING_SIZE) {
        start = 0;
    } else {
        start = g_query_head;
    }

    for (uint32_t i = 0; i < g_query_count && written < cap; i++) {
        uint32_t idx = (start + i) % SLOG_QUERY_RING_SIZE;
        struct slog_query_entry *e = &g_query_ring[idx];

        if (e->timestamp_ms < since_ms) continue;
        if (e->level > max_level) continue;
        if (subsystem && subsystem[0] &&
            strcmp(e->subsystem, subsystem) != 0) continue;

        out[written++] = *e;
    }

    spin_unlock_irqrestore(&g_query_lock, flags);
    return (int)written;
}

uint32_t slog_rotate_get_count(void) { return g_rotate_count; }
uint32_t slog_query_ring_count(void) { return g_query_count; }
