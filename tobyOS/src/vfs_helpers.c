/* vfs_helpers.c -- convenience wrappers used by several subsystems.
 *
 * Provides vfs_read_file / vfs_write_file (open-do-close shortcuts)
 * and clock_ms (uptime in milliseconds derived from PIT).
 */

#include <tobyos/types.h>
#include <tobyos/vfs.h>
#include <tobyos/pit.h>

uint64_t clock_ms(void)
{
    uint32_t hz = pit_hz();
    if (hz == 0) return 0;
    return pit_ticks() * 1000 / hz;
}

long vfs_read_file(const char *path, void *buf, size_t cap)
{
    struct vfs_file f;
    if (vfs_open(path, &f) != 0)
        return -1;
    long n = vfs_read(&f, buf, cap);
    vfs_close(&f);
    return n;
}

long vfs_write_file(const char *path, const void *buf, size_t len)
{
    struct vfs_file f;
    if (vfs_open(path, &f) != 0) {
        if (vfs_create(path) != 0)
            return -1;
        if (vfs_open(path, &f) != 0)
            return -1;
    }
    long n = vfs_write(&f, buf, len);
    vfs_close(&f);
    return n;
}
