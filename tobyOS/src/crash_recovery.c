/* crash_recovery.c -- handle kernel panics and unclean shutdowns. */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/crash_recovery.h>
#include <tobyos/spinlock.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/perf.h>

int  notify_svc_send(int pid, const char *title, const char *body, const char *icon);

#define CRASH_DUMP_PATH   "/data/crash/last.dump"
#define DIRTY_FLAG_PATH   "/data/crash/dirty"

static struct crash_dump g_last_dump;
static int g_has_dump;
static int g_dirty_flag;

static uint32_t compute_checksum(const struct crash_dump *d) {
    const uint8_t *p = (const uint8_t *)d;
    uint32_t sum = 0;
    size_t n = sizeof(*d) - sizeof(d->checksum);
    for (size_t i = 0; i < n; i++)
        sum = sum * 31 + p[i];
    return sum;
}

void crash_save_dump(const struct crash_dump *dump) {
    struct crash_dump d;
    memcpy(&d, dump, sizeof(d));
    d.magic = CRASH_MAGIC;
    d.timestamp = (uint32_t)(perf_now_ns() / 1000000000ULL);
    d.checksum = compute_checksum(&d);
    vfs_write_all(CRASH_DUMP_PATH, &d, sizeof(d));
    kprintf("[crash] dump saved (%u bytes)\n", (unsigned)sizeof(d));
}

int crash_load_dump(struct crash_dump *out) {
    if (!g_has_dump) return -1;
    memcpy(out, &g_last_dump, sizeof(*out));
    return 0;
}

void crash_clear(void) {
    g_has_dump = 0;
    memset(&g_last_dump, 0, sizeof(g_last_dump));
    char zeros[32];
    memset(zeros, 0, sizeof(zeros));
    vfs_write_all(CRASH_DUMP_PATH, zeros, sizeof(zeros));
}

int crash_check_unclean_shutdown(void) { return g_dirty_flag; }

void crash_set_clean_shutdown(void) {
    vfs_write_all(DIRTY_FLAG_PATH, "0\n", 2);
    g_dirty_flag = 0;
}

void crash_set_dirty_flag(void) {
    vfs_write_all(DIRTY_FLAG_PATH, "1\n", 2);
    g_dirty_flag = 1;
}

void crash_recovery_init(void) {
    g_has_dump = 0;
    g_dirty_flag = 0;

    /* Check dirty flag */
    void *dbuf = NULL;
    size_t dsz = 0;
    if (vfs_read_all(DIRTY_FLAG_PATH, &dbuf, &dsz) == VFS_OK && dbuf) {
        if (dsz > 0 && ((char *)dbuf)[0] == '1') {
            g_dirty_flag = 1;
            kprintf("[crash] WARNING: unclean shutdown detected\n");
        }
        kfree(dbuf);
    }

    /* Check crash dump */
    void *cbuf = NULL;
    size_t csz = 0;
    if (vfs_read_all(CRASH_DUMP_PATH, &cbuf, &csz) == VFS_OK && cbuf) {
        if (csz == sizeof(struct crash_dump)) {
            struct crash_dump *dump = (struct crash_dump *)cbuf;
            if (dump->magic == CRASH_MAGIC) {
                uint32_t expected = compute_checksum(dump);
                if (expected == dump->checksum) {
                    memcpy(&g_last_dump, dump, sizeof(g_last_dump));
                    g_has_dump = 1;
                    kprintf("[crash] found crash dump: \"%s\" at RIP=0x%lx\n",
                            dump->message, (unsigned long)dump->rip);
                    notify_svc_send(-1, "System Crash Detected",
                                   dump->message[0] ? dump->message : "Unknown crash reason",
                                   "warning");
                }
            }
        }
        kfree(cbuf);
    }

    if (g_dirty_flag && !g_has_dump)
        notify_svc_send(-1, "Unclean Shutdown",
                       "System was not shut down properly", "warning");

    crash_set_dirty_flag();
    kprintf("[crash] recovery initialized (has_dump=%d dirty=%d)\n",
            g_has_dump, g_dirty_flag);
}
