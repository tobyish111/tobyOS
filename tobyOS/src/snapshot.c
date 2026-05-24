/* snapshot.c -- system restore points for tobyOS.
 *
 * Creates, lists, restores, and deletes system snapshots. Each
 * snapshot records a description and timestamp. In the demo
 * environment, snapshots are tracked in-memory; a real implementation
 * would persist tarballs of changed files under /system/snapshots/.
 */

#include <tobyos/snapshot.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pit.h>

#define SNAPSHOT_MAX 16

static struct restore_point g_snapshots[SNAPSHOT_MAX];
static int                  g_snapshot_count;
static int                  g_next_id = 1;

void snapshot_init(void)
{
    memset(g_snapshots, 0, sizeof(g_snapshots));
    g_snapshot_count = 0;
    g_next_id = 1;
    kprintf("[snapshot] initialised (max %d restore points)\n", SNAPSHOT_MAX);
}

int snapshot_create(const char *description)
{
    if (g_snapshot_count >= SNAPSHOT_MAX) {
        kprintf("[snapshot] max snapshots reached\n");
        return -1;
    }

    struct restore_point *rp = &g_snapshots[g_snapshot_count];
    memset(rp, 0, sizeof(*rp));
    rp->id = g_next_id++;

    if (description) {
        int len = (int)strlen(description);
        if (len > 63) len = 63;
        memcpy(rp->description, description, (size_t)len);
        rp->description[len] = '\0';
    } else {
        memcpy(rp->description, "System Snapshot", 16);
    }

    rp->timestamp = (uint32_t)(pit_ticks() / pit_hz());
    rp->size_kb = 256; /* estimated snapshot size */

    g_snapshot_count++;
    kprintf("[snapshot] created restore point %d: '%s'\n",
            rp->id, rp->description);
    return rp->id;
}

int snapshot_restore(int id)
{
    for (int i = 0; i < g_snapshot_count; i++) {
        if (g_snapshots[i].id == id) {
            kprintf("[snapshot] restoring to point %d: '%s'\n",
                    id, g_snapshots[i].description);
            /* In a real implementation, this would extract the tarball
             * and overwrite system files, then prompt for reboot. */
            return 0;
        }
    }
    kprintf("[snapshot] restore point %d not found\n", id);
    return -1;
}

int snapshot_list(struct restore_point *out, int max)
{
    if (!out || max <= 0) return 0;
    int count = g_snapshot_count;
    if (count > max) count = max;
    memcpy(out, g_snapshots, (size_t)count * sizeof(struct restore_point));
    return count;
}

int snapshot_delete(int id)
{
    for (int i = 0; i < g_snapshot_count; i++) {
        if (g_snapshots[i].id == id) {
            kprintf("[snapshot] deleted restore point %d\n", id);
            for (int j = i; j < g_snapshot_count - 1; j++)
                g_snapshots[j] = g_snapshots[j + 1];
            g_snapshot_count--;
            return 0;
        }
    }
    return -1;
}
