/* http_async.c -- async HTTP transfers on a ring-0 kernel worker
 * (stage 12D). See http_async.h for the model and locking rules.
 *
 * Life of a transfer:
 *   httpa_start()  -- syscall ctx: claim a slot (QUEUED), lazily spawn
 *                     the worker (proc_create_kernel).
 *   httpa_worker() -- worker ctx: scan for QUEUED work under the BKL,
 *                     mark RUNNING, run http_get_follow (yields its
 *                     CPU inside tcp waits via tcp_yield_wait), then
 *                     publish DONE/ERROR -- or discard everything if
 *                     the owner FINISHed (cancelled) meanwhile.
 *   httpa_poll()   -- syscall ctx: report state.
 *   httpa_read()   -- syscall ctx: copy body bytes out.
 *   httpa_finish() -- syscall ctx: free (or cancel) the slot.
 *
 * Slots owned by exited processes are reaped lazily when the table
 * fills, so a crashed app can't leak the table away permanently. */

#include <tobyos/http_async.h>
#include <tobyos/http.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/cpu.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define HTTPA_MAX         8
#define HTTPA_KERNEL_MAX  (1u << 20)   /* per-transfer kernel body cap */

struct ha_slot {
    bool     used;
    bool     cancel;            /* FINISH arrived while RUNNING */
    int      state;             /* ABI_HTTPA_* */
    int      owner;             /* pid of the requesting process */
    int      err;               /* HTTP_ERR_* when state == ERROR */
    uint32_t max_body;
    char     url[512];          /* rewritten to the final URL when DONE */
    struct http_response resp;  /* body kmalloc'd; freed on finish */
};

static struct ha_slot g_ha[HTTPA_MAX];
static int g_ha_worker_pid = -1;

/* ---- the worker ------------------------------------------------------ */

static void httpa_worker(void) {
    /* First entry arrives via the context switch with no BKL; take it
     * before touching shared state. The transfer engine releases it
     * itself inside tcp waits. */
    if (!bkl_held()) bkl_enter();
    struct proc *self = current_proc();
    if (self) self->tcp_yield_wait = true;
    kprintf("[httpa] worker up (pid=%d)\n", self ? self->pid : -1);

    for (;;) {
        struct ha_slot *s = NULL;
        for (int i = 0; i < HTTPA_MAX; i++)
            if (g_ha[i].used && g_ha[i].state == ABI_HTTPA_QUEUED) {
                s = &g_ha[i];
                break;
            }
        if (!s) {
            /* idle: never yield holding the BKL */
            bkl_exit();
            sched_yield();
            bkl_enter();
            continue;
        }

        s->state = ABI_HTTPA_RUNNING;
        size_t kmax = s->max_body;
        if (kmax == 0 || kmax > HTTPA_KERNEL_MAX) kmax = HTTPA_KERNEL_MAX;

        /* The engine runs mostly under the BKL but interleaves with
         * syscalls inside tcp waits -- s->url/max_body are stable
         * (only FINISH touches a RUNNING slot, and only sets cancel). */
        /* Patient per-recv timeout: async callers tolerate slow
         * servers by design (the UI is live the whole time; the app
         * can always FINISH-cancel). The sync 5 s default is for
         * blocking callers that freeze their caller. */
        struct http_response resp;
        int rc = http_get_follow(s->url, kmax,
                                 HTTP_F_TRUNCATE | HTTP_F_GZIP |
                                 HTTP_F_KEEPALIVE, 30000, &resp);

        if (s->cancel) {
            if (rc >= 0) http_free(&resp);
            s->used = false;
            s->cancel = false;
            continue;
        }
        if (rc < 0) {
            s->err = rc;
            s->state = ABI_HTTPA_ERROR;
            kprintf("[httpa] h%d ERROR %d (%s)\n",
                    (int)(s - g_ha), rc, http_strerror(rc));
        } else {
            s->resp = resp;
            s->state = ABI_HTTPA_DONE;
        }
    }
}

/* ---- slot helpers (syscall context, under the BKL) ------------------- */

static void slot_release(struct ha_slot *s) {
    if (s->state == ABI_HTTPA_RUNNING) {
        s->cancel = true;              /* worker frees when it returns */
        return;
    }
    if (s->state == ABI_HTTPA_DONE) http_free(&s->resp);
    s->used = false;
    s->cancel = false;
}

/* Reap slots whose owner exited (crashed app mid-transfer). */
static void reap_orphans(void) {
    for (int i = 0; i < HTTPA_MAX; i++) {
        struct ha_slot *s = &g_ha[i];
        if (!s->used) continue;
        struct proc *p = proc_lookup(s->owner);
        if (!p || p->pid != s->owner) slot_release(s);
    }
}

static struct ha_slot *slot_of(int h, int owner_pid) {
    if (h < 0 || h >= HTTPA_MAX) return NULL;
    struct ha_slot *s = &g_ha[h];
    if (!s->used || s->owner != owner_pid || s->cancel) return NULL;
    return s;
}

/* ---- the four entry points ------------------------------------------- */

int httpa_start(const char *url, uint32_t max_body, int owner_pid) {
    if (!url || !url[0]) return -1;

    if (g_ha_worker_pid < 0) {
        g_ha_worker_pid = proc_create_kernel(httpa_worker, "httpa");
        if (g_ha_worker_pid < 0) return -1;
    }

    struct ha_slot *s = NULL;
    for (int pass = 0; pass < 2 && !s; pass++) {
        for (int i = 0; i < HTTPA_MAX; i++)
            if (!g_ha[i].used) { s = &g_ha[i]; break; }
        if (!s && pass == 0) reap_orphans();
    }
    if (!s) return -1;

    memset(s, 0, sizeof(*s));
    s->used     = true;
    s->owner    = owner_pid;
    s->max_body = max_body;
    size_t l = strlen(url);
    if (l >= sizeof(s->url)) l = sizeof(s->url) - 1;
    memcpy(s->url, url, l);
    s->url[l] = 0;
    s->state = ABI_HTTPA_QUEUED;
    return (int)(s - g_ha);
}

int httpa_poll(int h, int owner_pid, struct abi_http_poll *out) {
    struct ha_slot *s = slot_of(h, owner_pid);
    if (!s || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->state = (int32_t)s->state;
    if (s->state == ABI_HTTPA_ERROR) out->err = (int32_t)s->err;
    if (s->state == ABI_HTTPA_DONE) {
        out->status     = (int32_t)s->resp.status;
        out->body_len   = (uint32_t)s->resp.body_len;
        out->body_total = (s->resp.content_len >= 0)
                              ? (uint32_t)s->resp.content_len
                              : (uint32_t)s->resp.body_len;
        out->content_encoding = s->resp.encoding;
        size_t ul = strlen(s->url);
        if (ul >= sizeof(out->final_url)) ul = sizeof(out->final_url) - 1;
        memcpy(out->final_url, s->url, ul);
        out->final_url[ul] = 0;
        size_t cl = strlen(s->resp.content_type);
        if (cl >= sizeof(out->content_type)) cl = sizeof(out->content_type) - 1;
        memcpy(out->content_type, s->resp.content_type, cl);
        out->content_type[cl] = 0;
    }
    return 0;
}

long httpa_read(int h, int owner_pid, void *dst, uint32_t off, uint32_t len) {
    struct ha_slot *s = slot_of(h, owner_pid);
    if (!s || s->state != ABI_HTTPA_DONE || !dst) return -1;
    if (off >= s->resp.body_len || !s->resp.body) return 0;
    size_t avail = s->resp.body_len - off;
    if (len < avail) avail = len;
    memcpy(dst, s->resp.body + off, avail);
    return (long)avail;
}

int httpa_finish(int h, int owner_pid) {
    if (h < 0 || h >= HTTPA_MAX) return -1;
    struct ha_slot *s = &g_ha[h];
    if (!s->used || s->owner != owner_pid) return -1;
    slot_release(s);
    return 0;
}
