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
    bool     nonblock;                 /* IN_NONBLOCK at init1 / O_NONBLOCK */
    int      refs;                     /* struct files sharing this instance
                                        * (dup/fork); last close releases */
    int      owner_pid;
    int      next_wd;
    struct inotify_watch watches[INOTIFY_MAX_WATCHES];
    struct abi_inotify_event events[INOTIFY_EVENT_QUEUE];
    int      event_head;
    int      event_tail;
    int      event_count;
    int      dropped;                  /* queue-full events lost (IN_Q_OVERFLOW owed) */
};

static struct inotify_instance g_instances[INOTIFY_MAX_INSTANCES];
static int g_live_instances;           /* cheap gate for the hot emit sites */

void inotify_init_subsystem(void) {
    memset(g_instances, 0, sizeof(g_instances));
    g_live_instances = 0;
    kprintf("[inotify] subsystem initialized\n");
}

bool inotify_active(void) { return g_live_instances != 0; }

long sys_inotify_init(void) {
    struct proc *p = current_proc();
    int pid = p ? p->pid : 0;

    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
        if (!g_instances[i].used) {
            memset(&g_instances[i], 0, sizeof(g_instances[i]));
            g_instances[i].used = true;
            g_instances[i].refs = 1;
            g_instances[i].owner_pid = pid;
            g_instances[i].next_wd = 1;
            g_live_instances++;
            return i;
        }
    }
    return -ABI_ENOMEM;
}

void inotify_ref(int id) {
    if (id >= 0 && id < INOTIFY_MAX_INSTANCES && g_instances[id].used)
        g_instances[id].refs++;
}

void inotify_release(int id) {
    if (id < 0 || id >= INOTIFY_MAX_INSTANCES) return;
    if (!g_instances[id].used) return;
    if (--g_instances[id].refs > 0) return;
    g_instances[id].used = false;
    g_live_instances--;
}

void inotify_set_nonblock(int id, bool nb) {
    if (id >= 0 && id < INOTIFY_MAX_INSTANCES && g_instances[id].used)
        g_instances[id].nonblock = nb;
}

bool inotify_nonblock(int id) {
    return id >= 0 && id < INOTIFY_MAX_INSTANCES &&
           g_instances[id].used && g_instances[id].nonblock;
}

bool inotify_readable(int id) {
    if (id < 0 || id >= INOTIFY_MAX_INSTANCES) return true; /* error: no block */
    struct inotify_instance *inst = &g_instances[id];
    if (!inst->used) return true;
    return inst->event_count > 0;
}

/* Drain queued events into `buf` in the LINUX wire format:
 *   struct inotify_event { s32 wd; u32 mask; u32 cookie; u32 len;
 *                          char name[len]; }  -- len includes NUL padding.
 * Returns bytes written; 0 with events pending means the buffer cannot
 * hold even one event (Linux answers EINVAL for that -- caller maps it);
 * -1 for a dead instance. Never blocks -- blocking lives in the caller's
 * wait loop where signals can interrupt it. */
long inotify_read(int id, void *buf, size_t n) {
    if (id < 0 || id >= INOTIFY_MAX_INSTANCES) return -1;
    struct inotify_instance *inst = &g_instances[id];
    if (!inst->used) return -1;

    uint8_t *out = (uint8_t *)buf;
    size_t   off = 0;
    while (inst->event_count > 0) {
        struct abi_inotify_event *ev = &inst->events[inst->event_head];
        uint32_t nlen = ev->len;               /* already includes the NUL */
        /* Pad the name to an 8-byte multiple; watchers iterate by len. */
        uint32_t plen = nlen ? ((nlen + 7u) & ~7u) : 0;
        size_t   need = 16 + plen;
        if (off + need > n) break;
        *(int32_t  *)(out + off + 0)  = ev->wd;
        *(uint32_t *)(out + off + 4)  = ev->mask;
        *(uint32_t *)(out + off + 8)  = ev->cookie;
        *(uint32_t *)(out + off + 12) = plen;
        if (plen) {
            memset(out + off + 16, 0, plen);
            memcpy(out + off + 16, ev->name, nlen);
        }
        off += need;
        inst->event_head = (inst->event_head + 1) % INOTIFY_EVENT_QUEUE;
        inst->event_count--;
    }
    return (long)off;
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

void inotify_emit_cookie(const char *path, uint32_t mask, const char *name,
                         uint32_t cookie) {
    if (!path) return;
    if (!g_live_instances) return;     /* the hot-path gate: one load */

    bool queued = false;
    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
        struct inotify_instance *inst = &g_instances[i];
        if (!inst->used) continue;

        for (int w = 0; w < INOTIFY_MAX_WATCHES; w++) {
            struct inotify_watch *watch = &inst->watches[w];
            if (!watch->used) continue;
            if (!(watch->mask & mask)) continue;

            /* Match, Linux semantics: the event path IS the watched path
             * (watch on the file itself), or its DIRECT child (watch on
             * the parent directory -- the name field carries the child).
             * inotify is NOT recursive: the old bare prefix match reported
             * /data/a/b/c against a watch on /data, which no Linux watcher
             * expects and which a recursive-watch library (watchman,
             * chokidar) would double-count. */
            size_t wlen = strlen(watch->path);
            if (strncmp(path, watch->path, wlen) != 0) continue;
            const char *rest = path + wlen;
            if (rest[0] == '/') {
                const char *c = rest + 1;
                while (*c && *c != '/') c++;
                if (*c == '/') continue;           /* deeper than one level */
            } else if (rest[0] != '\0') {
                continue;                          /* /datafoo vs /data */
            }

            /* Enqueue the event. */
            if (inst->event_count >= INOTIFY_EVENT_QUEUE) {
                inst->dropped++;                   /* IN_Q_OVERFLOW owed */
                continue;
            }

            struct abi_inotify_event *ev =
                &inst->events[inst->event_tail];
            ev->wd     = watch->wd;
            ev->mask   = mask;
            ev->cookie = cookie;
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
            queued = true;
            break;
        }
    }
    if (queued) {
        /* Parked pollers re-derive readiness (BKL held: emit sites are
         * VFS mutators inside syscalls). */
        extern void poll_event_notify(void);
        poll_event_notify();
    }
}

void inotify_emit(const char *path, uint32_t mask, const char *name) {
    inotify_emit_cookie(path, mask, name, 0);
}
