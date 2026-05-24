/* inotify.c -- basic file watching mechanism (M1.5).
 *
 * Provides a kernel-side inotify subsystem. Each inotify instance has
 * a set of watches (path + event mask) and a ring buffer of pending
 * events. Events are posted by inotify_emit() from VFS mutation paths.
 *
 * The fd returned by SYS_INOTIFY_INIT is an opaque instance id
 * (not a real file descriptor in the proc's fd table). Watching and
 * event retrieval happen through dedicated syscalls, keeping the
 * implementation simple and self-contained.
 */

#include <tobyos/inotify.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/proc.h>
#include <tobyos/vfs.h>

struct inotify_watch {
    bool     used;
    int      wd;
    char     path[VFS_PATH_MAX];
    uint32_t mask;
};

struct inotify_instance {
    bool     used;
    int      owner_pid;
    int      next_wd;
    struct inotify_watch watches[INOTIFY_MAX_WATCHES];
    struct abi_inotify_event events[INOTIFY_EVENT_QUEUE];
    int      event_head;
    int      event_tail;
    int      event_count;
};

static struct inotify_instance g_instances[INOTIFY_MAX_INSTANCES];

void inotify_init_subsystem(void) {
    memset(g_instances, 0, sizeof(g_instances));
    kprintf("[inotify] subsystem initialized\n");
}

long sys_inotify_init(void) {
    struct proc *p = current_proc();
    int pid = p ? p->pid : 0;

    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
        if (!g_instances[i].used) {
            memset(&g_instances[i], 0, sizeof(g_instances[i]));
            g_instances[i].used = true;
            g_instances[i].owner_pid = pid;
            g_instances[i].next_wd = 1;
            return i;
        }
    }
    return -ABI_ENOMEM;
}

long sys_inotify_add_watch(int fd, const char *path, uint32_t mask) {
    if (fd < 0 || fd >= INOTIFY_MAX_INSTANCES) return -ABI_EBADF;
    struct inotify_instance *inst = &g_instances[fd];
    if (!inst->used) return -ABI_EBADF;
    if (!path) return -ABI_EFAULT;

    size_t plen = strlen(path);
    if (plen == 0 || plen >= VFS_PATH_MAX) return -ABI_EINVAL;

    /* Check if a watch already exists on this path -- update mask. */
    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (inst->watches[i].used &&
            strcmp(inst->watches[i].path, path) == 0) {
            inst->watches[i].mask = mask;
            return inst->watches[i].wd;
        }
    }

    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (!inst->watches[i].used) {
            inst->watches[i].used = true;
            inst->watches[i].wd   = inst->next_wd++;
            inst->watches[i].mask = mask;
            memcpy(inst->watches[i].path, path, plen + 1);
            return inst->watches[i].wd;
        }
    }
    return -ABI_ENOSPC;
}

long sys_inotify_rm_watch(int fd, int wd) {
    if (fd < 0 || fd >= INOTIFY_MAX_INSTANCES) return -ABI_EBADF;
    struct inotify_instance *inst = &g_instances[fd];
    if (!inst->used) return -ABI_EBADF;

    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (inst->watches[i].used && inst->watches[i].wd == wd) {
            inst->watches[i].used = false;
            return 0;
        }
    }
    return -ABI_EINVAL;
}

void inotify_emit(const char *path, uint32_t mask, const char *name) {
    if (!path) return;

    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
        struct inotify_instance *inst = &g_instances[i];
        if (!inst->used) continue;

        for (int w = 0; w < INOTIFY_MAX_WATCHES; w++) {
            struct inotify_watch *watch = &inst->watches[w];
            if (!watch->used) continue;
            if (!(watch->mask & mask)) continue;

            /* Match: the watched path is a prefix of the event path,
             * or an exact match. */
            size_t wlen = strlen(watch->path);
            if (strncmp(path, watch->path, wlen) != 0) continue;
            char next = path[wlen];
            if (next != '\0' && next != '/') continue;

            /* Enqueue the event. */
            if (inst->event_count >= INOTIFY_EVENT_QUEUE) continue;

            struct abi_inotify_event *ev =
                &inst->events[inst->event_tail];
            ev->wd     = watch->wd;
            ev->mask   = mask;
            ev->cookie = 0;
            ev->len    = 0;
            memset(ev->name, 0, sizeof(ev->name));
            if (name) {
                size_t nlen = strlen(name);
                if (nlen >= sizeof(ev->name)) nlen = sizeof(ev->name) - 1;
                memcpy(ev->name, name, nlen);
                ev->len = (uint32_t)(nlen + 1);
            }
            inst->event_tail = (inst->event_tail + 1) % INOTIFY_EVENT_QUEUE;
            inst->event_count++;
            break;
        }
    }
}
