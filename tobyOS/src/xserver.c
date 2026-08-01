/* xserver.c -- Fake X11 + MIT-SHM for Chromium Ozone (perf tier 2.5).
 *
 * Grows the handshake-only loopback into enough protocol for one mapped
 * window that chrome can paint via PutImage / ShmPutImage. Pixels land in
 * a well-known SysV segment (XFRAME_SYSV_KEY) that chromewin polls.
 */

#include <tobyos/xserver.h>
#include <tobyos/socket.h>
#include <tobyos/shm.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/heap.h>
#include <tobyos/perf.h>

#define XSRV_ROOT_ID    0x0000002bu
#define XSRV_CMAP_ID    0x00000020u
#define XSRV_VISUAL_ID  0x00000021u
#define XSRV_MIT_SHM_MAJOR  130
#define XSRV_RANDR_MAJOR    140
#define XSRV_XFIXES_MAJOR   141
#define XSRV_BIGREQ_MAJOR   142
#define XSRV_SYNC_MAJOR     143
#define X_MAX_WIN       16
#define X_MAX_GC        16
#define X_MAX_SHMSEG    8
#define X_REQ_CAP       (256 * 1024)
#define XSRV_RR_CRTC    0x00000060u
#define XSRV_RR_OUTPUT  0x00000061u
#define XSRV_RR_MODE    0x00000062u
/* Extension event bases must be > 1 (0=error, 1=reply) and outside core. */
#define XSRV_EV_MIT_SHM 64
#define XSRV_EV_RANDR   65
#define XSRV_EV_XFIXES  66
#define XSRV_EV_SYNC    67
#define XSRV_ERR_MIT_SHM 192
#define XSRV_ERR_RANDR   193
#define XSRV_ERR_XFIXES  194
#define XSRV_ERR_SYNC    195

struct x_win {
    uint32_t id;
    int16_t  x, y;
    uint16_t w, h;
    uint8_t  mapped;
    uint8_t  used;
};

struct x_gc {
    uint32_t id;
    uint8_t  used;
};

struct x_shmseg {
    uint32_t xid;
    int      shmid;
    uint8_t  used;
};

#define X_PEND_MAX  512   /* queued reply chunks while client RX ring is full */
#define X_ATOM_MAX  1024
#define XA_ATOM     4u
#define XA_CARDINAL 6u
#define XA_WINDOW   33u

struct x_pend {
    uint8_t *data;
    uint32_t len;
};

struct x_atom {
    uint32_t id;
    char     name[48];
    uint8_t  used;
};

struct x_conn {
    uint8_t  active;
    uint16_t seq;
    uint32_t rlen;
    uint8_t *rbuf;
    uint32_t rcap;
    struct x_win  wins[X_MAX_WIN];
    struct x_gc   gcs[X_MAX_GC];
    struct x_shmseg segs[X_MAX_SHMSEG];
    uint32_t focus;
    /* PutImage streaming state */
    uint8_t  put_active;
    uint32_t put_left;
    uint32_t put_dst_x, put_dst_y, put_w, put_h, put_row;
    uint32_t put_bpp;
    /* Never drop X replies: park here when sock RX ring is full. */
    struct x_pend pend[X_PEND_MAX];
    uint16_t pend_r, pend_w, pend_n;
    /* Set after first device-init PropertyNotify; gates poll-time idle wakes. */
    uint8_t  idle_wake_armed;
    /* Slice 89: ms stamp of the last request DECODED from this client. At a
     * stall, "chrome has sent nothing for 170s" (it is parked waiting on us)
     * and "chrome is still talking" are entirely different bugs, and nothing
     * in the tree could tell them apart -- the wait-graph is blind here
     * because blocking recvmsg never registers with waitt_enter. */
    uint64_t last_req_ms;
    uint8_t  display_wake_sent; /* one-shot wake after RANDR display probe */
};

/* Slice 89: extension surface. Default = MINIMAL (MIT-SHM only), which is
 * the configuration slice 88's control rig proved chrome MapWindows under.
 * -DXSRV_EXT_ALL restores the old RANDR/XFIXES/BIG-REQUESTS advertisement
 * for A/B testing. */
#ifdef XSRV_EXT_ALL
#undef XSRV_EXT_MINIMAL
#else
#define XSRV_EXT_MINIMAL 1
#endif

/* Global frame chrome paints into; chromewin polls this. */
static uint32_t g_xf_w = 800, g_xf_h = 600, g_xf_stride = 800 * 4;
static volatile uint32_t g_xf_gen;
static int g_xf_shmid = -1;
static uint8_t *g_xf_pixels;   /* HHDM */

static struct x_conn g_xconn[SOCK_MAX];
/* Atoms are server-global (shared across X clients). */
static struct x_atom g_atoms[X_ATOM_MAX];
static uint32_t g_next_atom = 100;

static inline void xput16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void xput32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t xget16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t xget32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int sock_idx(struct sock *s) {
    return sock_pool_index(s);
}

static void xframe_ensure(uint32_t w, uint32_t h) {
    /* Ignore tiny WmSync/dummy windows — keep the Ozone paint surface at the
     * composited size chrome actually uses (typically 800x600). */
    if (w < 64 || h < 64) return;
    if (w > 1920) w = 1920;
    if (h > 1200) h = 1200;
    size_t need = (size_t)w * (size_t)h * 4u;
    if (g_xf_shmid >= 0 && g_xf_w == w && g_xf_h == h && g_xf_pixels)
        return;
    long id = sysv_shm_ensure((int)XFRAME_SYSV_KEY, need);
    if (id < 0) {
        kprintf("[xsrv] xframe shm_ensure failed %ld\n", id);
        return;
    }
    g_xf_shmid = (int)id;
    g_xf_pixels = (uint8_t *)sysv_shm_kva(g_xf_shmid);
    g_xf_w = w;
    g_xf_h = h;
    g_xf_stride = w * 4;
    if (g_xf_pixels)
        memset(g_xf_pixels, 0x20, need);
    __atomic_fetch_add(&g_xf_gen, 1, __ATOMIC_RELEASE);
    kprintf("[xsrv] xframe %ux%u shmid=%d\n", w, h, g_xf_shmid);
}

static void xframe_bump(void) {
    __atomic_fetch_add(&g_xf_gen, 1, __ATOMIC_RELEASE);
}

/* Set when an X reply cannot be queued (RX ring full). The client is mid-
 * write and cannot read until that write returns — so xserver_handle must
 * return a SHORT count and let chrome drain. */
static int g_x_backpressure;

static void x_pend_push(struct x_conn *c, const uint8_t *rep, size_t n) {
    if (!c || !rep || !n) return;
    if (c->pend_n >= X_PEND_MAX) {
        static int once;
        if (!once) { once = 1; kprintf("[xsrv] pending reply queue FULL — drop\n"); }
        return;
    }
    uint8_t *copy = (uint8_t *)kmalloc(n);
    if (!copy) return;
    memcpy(copy, rep, n);
    c->pend[c->pend_w].data = copy;
    c->pend[c->pend_w].len = (uint32_t)n;
    c->pend_w = (uint16_t)((c->pend_w + 1) % X_PEND_MAX);
    c->pend_n++;
}

static void x_pend_flush(struct sock *s, struct x_conn *c) {
    if (!s || !c) return;
    while (c->pend_n > 0) {
        struct x_pend *p = &c->pend[c->pend_r];
        int rc = sock_unix_enqueue(s, p->data, p->len);
        if (rc == SOCK_ERR_AGAIN) {
            g_x_backpressure = 1;
            return;
        }
        if (p->data) kfree(p->data);
        p->data = 0; p->len = 0;
        c->pend_r = (uint16_t)((c->pend_r + 1) % X_PEND_MAX);
        c->pend_n--;
    }
}

/* Returns 0 ok. Never drops: parks on the conn if the RX ring is full. */
static int x_reply(struct sock *s, uint16_t seq, const uint8_t *rep, size_t n) {
    (void)seq;
    int idx = sock_idx(s);
    struct x_conn *c = (idx >= 0) ? &g_xconn[idx] : 0;
    if (c && c->pend_n > 0) {
        /* Preserve reply order behind anything already parked. */
        x_pend_push(c, rep, n);
        x_pend_flush(s, c);
        if (c->pend_n > 0) g_x_backpressure = 1;
        return 0;
    }
    int rc = sock_unix_enqueue(s, rep, n);
    if (rc == SOCK_ERR_AGAIN) {
        if (c) x_pend_push(c, rep, n);
        g_x_backpressure = 1;
        return 0;
    }
    return rc;
}

static int x_reply32(struct sock *s, uint16_t seq, uint8_t *rep) {
    rep[0] = 1;
    xput16(rep + 2, seq);
    return x_reply(s, seq, rep, 32);
}

/* Inject a root PropertyNotify so xcb_wait_for_event returns. Only call when
 * no typed reply is in flight for this request (void handlers / after reply). */
static void x_wake_event(struct sock *s, struct x_conn *c, uint16_t seq,
                         const char *why) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = 28; /* PropertyNotify */
    xput16(ev + 2, seq);
    xput32(ev + 4, XSRV_ROOT_ID);
    x_reply(s, seq, ev, 32);
    c->idle_wake_armed = 1;
    kprintf("[xsrv] wake (%s)\n", why);
}

/* Slice 90: ConfigureNotify(22). The control emits one after MapWindow and
 * again after ConfigureWindow; Ozone will not treat its geometry as real
 * (and so will not lay out or paint) until it sees one. */
static void x_send_configure_notify(struct sock *s, uint16_t seq, uint32_t wid,
                                    int16_t x, int16_t y, uint16_t w,
                                    uint16_t h) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = 22;                 /* ConfigureNotify */
    xput16(ev + 2, seq);
    xput32(ev + 4, wid);        /* event window */
    xput32(ev + 8, wid);        /* window */
    xput32(ev + 12, 0);         /* above-sibling = None */
    xput16(ev + 16, (uint16_t)x);
    xput16(ev + 18, (uint16_t)y);
    xput16(ev + 20, w);
    xput16(ev + 22, h);
    xput16(ev + 24, 0);         /* border-width */
    ev[26] = 0;                 /* override-redirect */
    x_reply(s, seq, ev, 32);
}

static int x_error(struct sock *s, uint16_t seq, uint8_t code, uint8_t minor,
                   uint32_t resid, uint8_t major) {
    uint8_t e[32];
    memset(e, 0, 32);
    e[0] = 0;
    e[1] = code;
    xput16(e + 2, seq);
    xput32(e + 4, resid);
    e[8] = minor;
    e[9] = major;
    return x_reply(s, seq, e, 32);
}

static struct x_win *win_find(struct x_conn *c, uint32_t id) {
    for (int i = 0; i < X_MAX_WIN; i++)
        if (c->wins[i].used && c->wins[i].id == id) return &c->wins[i];
    if (id == XSRV_ROOT_ID) {
        /* Synthetic root */
        static struct x_win root;
        root.id = XSRV_ROOT_ID;
        root.w = (uint16_t)g_xf_w;
        root.h = (uint16_t)g_xf_h;
        root.mapped = 1;
        root.used = 1;
        return &root;
    }
    return 0;
}

static struct x_win *win_alloc(struct x_conn *c, uint32_t id) {
    for (int i = 0; i < X_MAX_WIN; i++) {
        if (!c->wins[i].used) {
            memset(&c->wins[i], 0, sizeof(c->wins[i]));
            c->wins[i].used = 1;
            c->wins[i].id = id;
            return &c->wins[i];
        }
    }
    return 0;
}

static uint32_t atom_lookup(const char *name) {
    for (int i = 0; i < X_ATOM_MAX; i++)
        if (g_atoms[i].used && !strcmp(g_atoms[i].name, name))
            return g_atoms[i].id;
    return 0;
}

static const char *atom_name(uint32_t id) {
    /* Predefined core atoms (Xatom.h) chrome uses without InternAtom. */
    if (id == 23) return "RESOURCE_MANAGER";
    if (id == 31) return "STRING";
    if (id == 35) return "WM_HINTS";
    if (id == 37) return "WM_ICON_NAME";
    if (id == 39) return "WM_NAME";
    if (id == 40) return "WM_NORMAL_HINTS";
    if (id == 41) return "WM_SIZE_HINTS";
    if (id == 67) return "WM_CLASS";
    for (int i = 0; i < X_ATOM_MAX; i++)
        if (g_atoms[i].used && g_atoms[i].id == id)
            return g_atoms[i].name;
    return 0;
}

static void atom_store(uint32_t id, const char *name) {
    for (int i = 0; i < X_ATOM_MAX; i++) {
        if (!g_atoms[i].used) {
            g_atoms[i].used = 1;
            g_atoms[i].id = id;
            size_t n = 0;
            while (name[n] && n < sizeof(g_atoms[i].name) - 1) {
                g_atoms[i].name[n] = name[n];
                n++;
            }
            g_atoms[i].name[n] = 0;
            return;
        }
    }
    kprintf("[xsrv] atom table full (%d), dropping '%s'\n", X_ATOM_MAX, name);
}

static uint32_t atom_ensure(const char *name) {
    uint32_t id = atom_lookup(name);
    if (id) return id;
    id = g_next_atom++;
    atom_store(id, name);
    return id;
}

/* Tiny property store so ChangeProperty / GetProperty round-trip. */
#define X_PROP_MAX 64
#define X_PROP_VAL 256
struct x_prop {
    uint32_t wid, atom, type;
    uint8_t  format;
    uint16_t len;
    uint8_t  used;
    uint8_t  data[X_PROP_VAL];
};
static struct x_prop g_props[X_PROP_MAX];

static struct x_prop *prop_find(uint32_t wid, uint32_t atom) {
    for (int i = 0; i < X_PROP_MAX; i++)
        if (g_props[i].used && g_props[i].wid == wid && g_props[i].atom == atom)
            return &g_props[i];
    return 0;
}

static void prop_set(uint32_t wid, uint32_t atom, uint32_t type, uint8_t format,
                     const void *val, uint32_t nbytes) {
    struct x_prop *p = prop_find(wid, atom);
    if (!p) {
        for (int i = 0; i < X_PROP_MAX; i++) {
            if (!g_props[i].used) { p = &g_props[i]; break; }
        }
    }
    if (!p) return;
    if (nbytes > X_PROP_VAL) nbytes = X_PROP_VAL;
    p->used = 1;
    p->wid = wid;
    p->atom = atom;
    p->type = type;
    p->format = format ? format : 8;
    p->len = (uint16_t)nbytes;
    if (val && nbytes) memcpy(p->data, val, nbytes);
}

/* GetProperty reply: fixed 32 + value bytes (padded to 4). */
static void prop_reply(struct sock *s, uint16_t seq, uint32_t type,
                       uint8_t format, const void *val, uint32_t nbytes) {
    uint32_t pad = (4u - (nbytes & 3u)) & 3u;
    uint32_t words = (nbytes + pad) / 4u;
    uint32_t total = 32 + nbytes + pad;
    uint8_t *big = kmalloc(total ? total : 32);
    if (!big) return;
    memset(big, 0, total);
    big[0] = 1;
    big[1] = format;
    xput16(big + 2, seq);
    xput32(big + 4, words);
    xput32(big + 8, type);
    xput32(big + 12, 0); /* bytes-after */
    xput32(big + 16, format == 32 ? nbytes / 4u :
                     format == 16 ? nbytes / 2u : nbytes);
    if (nbytes && val) memcpy(big + 32, val, nbytes);
    x_reply(s, seq, big, total);
    kfree(big);
}

static void blit_z_rgb32(uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                         const uint8_t *src, uint32_t src_stride) {
    if (!g_xf_pixels) xframe_ensure(g_xf_w, g_xf_h);
    if (!g_xf_pixels) return;
    for (uint32_t y = 0; y < h; y++) {
        if (dy + y >= g_xf_h) break;
        uint32_t copy_w = w;
        if (dx + copy_w > g_xf_w) copy_w = g_xf_w - dx;
        if (dx >= g_xf_w || copy_w == 0) continue;
        uint8_t *dst = g_xf_pixels + (size_t)(dy + y) * g_xf_stride + dx * 4;
        const uint8_t *row = src + (size_t)y * src_stride;
        /* X11 ZPixmap 24/32 often arrives as BGRA or RGBX; store as ARGB
         * for TobyTK (byte order B,G,R,A on LE = 0xAARRGGBB word). */
        for (uint32_t x = 0; x < copy_w; x++) {
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            dst[x * 4 + 0] = b;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = r;
            dst[x * 4 + 3] = 0xff;
        }
    }
    xframe_bump();
}

static size_t xserver_build_setup(uint8_t *o, size_t cap) {
    const uint8_t nformats = 1, nscreens = 1, ndepths = 1, nvisuals = 1;
    size_t body  = 32 + 8u * nformats + 40u * nscreens + 8u * ndepths + 24u * nvisuals;
    size_t total = 8 + body;
    if (total > cap) return 0;
    memset(o, 0, total);
    size_t i = 0;
    o[i++] = 1; o[i++] = 0;
    xput16(o + i, 11); i += 2;
    xput16(o + i, 0);  i += 2;
    xput16(o + i, (uint16_t)(body / 4)); i += 2;
    xput32(o + i, 0); i += 4;
    xput32(o + i, XSRV_ROOT_ID + 1); i += 4;
    xput32(o + i, 0x001fffff); i += 4;
    xput32(o + i, 0); i += 4;
    xput16(o + i, 0); i += 2;
    xput16(o + i, 0xffff); i += 2;
    o[i++] = nscreens; o[i++] = nformats;
    o[i++] = 0; o[i++] = 0; o[i++] = 32; o[i++] = 32;
    /* Keep the keycode span small: a 248-keysym GetKeyboardMapping reply
     * has smashed chrome/xcb stack canaries on this path before. */
    o[i++] = 8; o[i++] = 39; /* 32 keycodes */
    xput32(o + i, 0); i += 4;
    o[i++] = 24; o[i++] = 32; o[i++] = 32; i += 5;
    xput32(o + i, XSRV_ROOT_ID); i += 4;
    xput32(o + i, XSRV_CMAP_ID); i += 4;
    xput32(o + i, 0x00ffffff); i += 4;
    xput32(o + i, 0x00000000); i += 4;
    xput32(o + i, 0); i += 4;
    xput16(o + i, (uint16_t)g_xf_w); i += 2;
    xput16(o + i, (uint16_t)g_xf_h); i += 2;
    xput16(o + i, 270); i += 2;
    xput16(o + i, 203); i += 2;
    xput16(o + i, 1); i += 2;
    xput16(o + i, 1); i += 2;
    xput32(o + i, XSRV_VISUAL_ID); i += 4;
    o[i++] = 0; o[i++] = 0; o[i++] = 24; o[i++] = ndepths;
    o[i++] = 24; o[i++] = 0;
    xput16(o + i, nvisuals); i += 2;
    i += 4;
    xput32(o + i, XSRV_VISUAL_ID); i += 4;
    o[i++] = 4; o[i++] = 8;
    xput16(o + i, 256); i += 2;
    xput32(o + i, 0x00ff0000); i += 4;
    xput32(o + i, 0x0000ff00); i += 4;
    xput32(o + i, 0x000000ff); i += 4;
    i += 4;
    return i;
}

static void handle_mit_shm(struct sock *s, struct x_conn *c,
                           const uint8_t *req, uint32_t req_len) {
    uint8_t minor = req[1];
    uint16_t seq = ++c->seq;
    uint8_t rep[32];
    memset(rep, 0, 32);

    if (minor == 0) { /* QueryVersion */
        rep[0] = 1;
        xput16(rep + 2, seq);
        xput16(rep + 8, 1);   /* major */
        xput16(rep + 10, 2);  /* minor */
        rep[12] = 1;          /* sharedPixmaps */
        x_reply(s, seq, rep, 32);
        return;
    }
    if (minor == 1) { /* Attach */
        uint32_t xid = xget32(req + 4);
        int shmid = (int)xget32(req + 8);
        for (int i = 0; i < X_MAX_SHMSEG; i++) {
            if (!c->segs[i].used) {
                c->segs[i].used = 1;
                c->segs[i].xid = xid;
                c->segs[i].shmid = shmid;
                return; /* no reply */
            }
        }
        x_error(s, seq, 11 /* Alloc */, minor, xid, XSRV_MIT_SHM_MAJOR);
        return;
    }
    if (minor == 2) { /* Detach */
        uint32_t xid = xget32(req + 4);
        for (int i = 0; i < X_MAX_SHMSEG; i++)
            if (c->segs[i].used && c->segs[i].xid == xid)
                c->segs[i].used = 0;
        return;
    }
    if (minor == 3) { /* PutImage */
        /* CARD8 minor, BOOL send_event, ... standard ShmPutImage */
        if (req_len < 40) return;
        uint32_t shmseg = xget32(req + 8);
        uint32_t src_x = xget16(req + 20);
        uint32_t src_y = xget16(req + 22);
        uint32_t src_w = xget16(req + 12);
        uint32_t src_h = xget16(req + 14);
        uint32_t dst_x = xget16(req + 24);
        uint32_t dst_y = xget16(req + 26);
        uint32_t total_w = xget16(req + 16);
        (void)src_x; (void)src_y;
        int shmid = -1;
        for (int i = 0; i < X_MAX_SHMSEG; i++)
            if (c->segs[i].used && c->segs[i].xid == shmseg)
                shmid = c->segs[i].shmid;
        uint8_t *kva = shmid >= 0 ? (uint8_t *)sysv_shm_kva(shmid) : 0;
        if (!kva) {
            x_error(s, seq, 2 /* Value */, minor, shmseg, XSRV_MIT_SHM_MAJOR);
            return;
        }
        uint32_t stride = total_w ? total_w * 4 : src_w * 4;
        const uint8_t *src = kva + (size_t)src_y * stride + src_x * 4;
        blit_z_rgb32(dst_x, dst_y, src_w, src_h, src, stride);
        if (req[1] /* send_event in some layouts - check byte 2 */) {
            /* Optional Completion event; Ozone usually sets send_event=1 */
        }
        /* Send Completion event when send_event (req[2]) is set */
        if (req[2]) {
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = XSRV_EV_MIT_SHM; /* Completion = base+0 */
            ev[1] = 0;
            xput16(ev + 2, seq);
            xput32(ev + 4, xget32(req + 4)); /* drawable */
            xput16(ev + 8, 0);
            x_reply(s, seq, ev, 32);
        }
        return;
    }
    /* Unknown minor: ignore */
    (void)req_len;
}

static void handle_request(struct sock *s, struct x_conn *c,
                           const uint8_t *req, uint32_t req_bytes) {
    uint8_t op = req[0];
    uint16_t seq = ++c->seq;
    uint8_t rep[64];
    memset(rep, 0, sizeof(rep));

    if (op == XSRV_MIT_SHM_MAJOR) {
        c->seq--; /* handle_mit_shm bumps */
        handle_mit_shm(s, c, req, req_bytes);
        return;
    }
    if (op == XSRV_BIGREQ_MAJOR) {
        /* BigRequests Enable — reply with max-request-length. */
        rep[0] = 1;
        xput16(rep + 2, seq);
        xput32(rep + 8, 0x3fffffu);
        x_reply32(s, seq, rep);
        return;
    }
    if (op == XSRV_XFIXES_MAJOR) {
        /* Most XFIXES requests are void. Spurious replies desync xcb and
         * chrome ImmediateCrash (exit 191) right after CreateRegionFromWindow. */
        uint8_t minor = req[1];
        kprintf("[xsrv] XFIXES minor=%u words=%u\n", minor, req_bytes / 4);
        if (minor == 0) { /* QueryVersion */
            rep[0] = 1;
            xput16(rep + 2, seq);
            xput32(rep + 8, 5);  /* major */
            xput32(rep + 12, 0); /* minor */
            x_reply32(s, seq, rep);
            return;
        }
        if (minor == 4) { /* GetCursorImage — empty cursor */
            uint8_t big[32];
            memset(big, 0, sizeof big);
            big[0] = 1;
            xput16(big + 2, seq);
            xput32(big + 4, 0);
            xput16(big + 8, 0); xput16(big + 10, 0);
            xput16(big + 12, 0); xput16(big + 14, 0);
            xput16(big + 16, 0); xput16(big + 18, 0);
            x_reply32(s, seq, big);
            return;
        }
        if (minor == 18) { /* FetchRegion / RegionExtents — empty extents */
            uint8_t big[40];
            memset(big, 0, sizeof big);
            big[0] = 1;
            xput16(big + 2, seq);
            xput32(big + 4, 2); /* 8 bytes of extents */
            x_reply(s, seq, big, 40);
            return;
        }
        if (minor == 19) { /* FetchRegion — empty rectangles */
            uint8_t big[40];
            memset(big, 0, sizeof big);
            big[0] = 1;
            xput16(big + 2, seq);
            xput32(big + 4, 2);
            x_reply(s, seq, big, 40);
            return;
        }
        /* All other XFIXES minors are void (CreateRegion*, SetRegion, …). */
        return;
    }
    if (op == XSRV_RANDR_MAJOR) {
        /* Minor opcodes from X11/extensions/randr.h (RANDR 1.6). */
        uint8_t minor = req[1];
        uint16_t nw = g_xf_w ? (uint16_t)g_xf_w : 800;
        uint16_t nh = g_xf_h ? (uint16_t)g_xf_h : 600;
        kprintf("[xsrv] RANDR minor=%u words=%u\n", minor, req_bytes / 4);
        if (minor == 0) { /* QueryVersion */
            rep[0] = 1;
            xput16(rep + 2, seq);
            xput32(rep + 8, 1);
            xput32(rep + 12, 5);
            x_reply32(s, seq, rep);
            return;
        }
        if (minor == 2) { /* SetScreenConfig */
            rep[0] = 1; rep[1] = 0;
            xput16(rep + 2, seq);
            xput32(rep + 8, 0);
            xput16(rep + 12, 0);
            x_reply32(s, seq, rep);
            return;
        }
        if (minor == 4) { /* SelectInput — void + current config event */
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = XSRV_EV_RANDR; /* RRScreenChangeNotify */
            ev[1] = 1;             /* Rotate_0 */
            xput16(ev + 2, seq);
            xput32(ev + 4, 1);
            xput32(ev + 8, 1);
            xput32(ev + 12, XSRV_ROOT_ID);
            xput32(ev + 16, xget32(req + 4));
            xput16(ev + 20, 0);
            xput16(ev + 22, 0);
            xput16(ev + 24, nw);
            xput16(ev + 26, nh);
            xput16(ev + 28, 270);
            xput16(ev + 30, 203);
            x_reply(s, seq, ev, 32);
            return;
        }
        if (minor == 8 || minor == 25) {
            /* length = c+o+8m+(b+p)/4 = 1+1+8+1 = 11 */
            uint8_t big[76];
            memset(big, 0, sizeof big);
            big[0] = 1;
            xput16(big + 2, seq);
            xput32(big + 4, 11);
            xput32(big + 8, 1);
            xput32(big + 12, 1);
            xput16(big + 16, 1);
            xput16(big + 18, 1);
            xput16(big + 20, 1);
            xput16(big + 22, 4);
            xput32(big + 32, XSRV_RR_CRTC);
            xput32(big + 36, XSRV_RR_OUTPUT);
            xput32(big + 40, XSRV_RR_MODE);
            xput16(big + 44, nw);
            xput16(big + 46, nh);
            xput32(big + 48, (uint32_t)nw * (uint32_t)nh * 60u);
            xput16(big + 52, (uint16_t)(nw + 8));
            xput16(big + 54, (uint16_t)(nw + 16));
            xput16(big + 56, (uint16_t)(nw + 40));
            xput16(big + 58, 0);
            xput16(big + 60, (uint16_t)(nh + 1));
            xput16(big + 62, (uint16_t)(nh + 4));
            xput16(big + 64, (uint16_t)(nh + 20));
            xput16(big + 66, 4);
            xput32(big + 68, 0);
            big[72] = 'd'; big[73] = 'u'; big[74] = 'm'; big[75] = 'y';
            x_reply(s, seq, big, 76);
            return;
        }
        if (minor == 9) { /* GetOutputInfo */
            /* length = 1+c+m+(n+p)/4 = 1+1+1+1 = 4 (c=1,m=1,n=4,clones=0) */
            uint8_t big[48];
            memset(big, 0, sizeof big);
            big[0] = 1; big[1] = 0;
            xput16(big + 2, seq);
            xput32(big + 4, 4);
            xput32(big + 8, 1);
            xput32(big + 12, XSRV_RR_CRTC);
            xput32(big + 16, 270);
            xput32(big + 20, 203);
            big[24] = 0; /* Connected */
            big[25] = 0;
            xput16(big + 26, 1); /* ncrtc */
            xput16(big + 28, 1); /* nmode */
            xput16(big + 30, 1); /* npreferred */
            xput16(big + 32, 0); /* nclone */
            xput16(big + 34, 4); /* name length */
            xput32(big + 36, XSRV_RR_CRTC);
            xput32(big + 40, XSRV_RR_MODE);
            big[44] = 'D'; big[45] = 'F'; big[46] = 'P'; big[47] = '1';
            x_reply(s, seq, big, 48);
            return;
        }
        if (minor == 20) { /* GetCrtcInfo — length = o+p = 2 */
            uint8_t big[40];
            memset(big, 0, sizeof big);
            big[0] = 1; big[1] = 0;
            xput16(big + 2, seq);
            xput32(big + 4, 2);
            xput32(big + 8, 1);
            xput16(big + 12, 0); xput16(big + 14, 0);
            xput16(big + 16, nw); xput16(big + 18, nh);
            xput32(big + 20, XSRV_RR_MODE);
            xput16(big + 24, 1); /* current rotation */
            xput16(big + 26, 1); /* possible rotations */
            xput16(big + 28, 1); /* noutput */
            xput16(big + 30, 1); /* npossible */
            xput32(big + 32, XSRV_RR_OUTPUT);
            xput32(big + 36, XSRV_RR_OUTPUT);
            x_reply(s, seq, big, 40);
            /* RRNotify (CrtcChange) = first_event+1 — wakes wait_for_event. */
            {
                uint8_t ev[32];
                memset(ev, 0, 32);
                ev[0] = XSRV_EV_RANDR + 1;
                ev[1] = 0; /* RRNotify_CrtcChange */
                xput16(ev + 2, seq);
                xput32(ev + 4, 1); /* timestamp */
                xput32(ev + 8, XSRV_ROOT_ID);
                xput32(ev + 12, XSRV_RR_CRTC);
                xput32(ev + 16, XSRV_RR_MODE);
                xput16(ev + 20, 1); /* rotation */
                xput16(ev + 22, 0); xput16(ev + 24, 0);
                xput16(ev + 26, nw); xput16(ev + 28, nh);
                x_reply(s, seq, ev, 32);
            }
            return;
        }
        if (minor == 15) { /* GetOutputProperty — empty like GetProperty */
            rep[0] = 1;
            xput16(rep + 2, seq);
            xput32(rep + 4, 0);
            xput32(rep + 8, 0);  /* type None */
            xput32(rep + 12, 0); /* bytes-after */
            xput32(rep + 16, 0); /* value length */
            x_reply32(s, seq, rep);
            return;
        }
        if (minor == 10) { /* ListOutputProperties — none */
            rep[0] = 1;
            xput16(rep + 2, seq);
            xput32(rep + 4, 0);
            xput16(rep + 8, 0);
            x_reply32(s, seq, rep);
            return;
        }
        if (minor == 31) { /* GetOutputPrimary */
            rep[0] = 1;
            xput16(rep + 2, seq);
            xput32(rep + 8, XSRV_RR_OUTPUT);
            x_reply32(s, seq, rep);
            return;
        }
        if (minor == 32) { /* GetProviders — none */
            rep[0] = 1;
            xput16(rep + 2, seq);
            xput16(rep + 8, 0);
            x_reply32(s, seq, rep);
            return;
        }
        if (minor == 42) { /* GetMonitors — empty; size from resources + WORKAREA */
            rep[0] = 1;
            xput16(rep + 2, seq);
            xput32(rep + 4, 0);
            xput32(rep + 8, 1);
            xput32(rep + 12, 0);
            xput32(rep + 16, 0);
            x_reply32(s, seq, rep);
            /* Display probe done — UI parks in wait_for_event without this. */
            if (!c->display_wake_sent) {
                c->display_wake_sent = 1;
                x_wake_event(s, c, seq, "post-GetMonitors");
            }
            return;
        }
        /* Optional/unused: void success rather than a corrupt typed reply. */
        kprintf("[xsrv] RANDR unhandled minor=%u (void)\n", minor);
        return;
    }
    switch (op) {
    case 1: { /* CreateWindow */
        if (req_bytes < 32) return;
        uint32_t wid = xget32(req + 4);
        uint16_t w = xget16(req + 16);
        uint16_t h = xget16(req + 18);
        struct x_win *win = win_alloc(c, wid);
        if (!win) { x_error(s, seq, 11, 0, wid, op); return; }
        win->x = (int16_t)xget16(req + 12);
        win->y = (int16_t)xget16(req + 14);
        win->w = w ? w : 800;
        win->h = h ? h : 600;
        xframe_ensure(win->w, win->h);
        kprintf("[xsrv] CreateWindow id=0x%x %ux%u\n", wid, win->w, win->h);
        /* StructureNotify clients (Ozone) expect CreateNotify. */
        {
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = 16; /* CreateNotify */
            xput16(ev + 2, seq);
            xput32(ev + 4, XSRV_ROOT_ID); /* parent */
            xput32(ev + 8, wid);
            xput16(ev + 12, (uint16_t)win->x);
            xput16(ev + 14, (uint16_t)win->y);
            xput16(ev + 16, win->w);
            xput16(ev + 18, win->h);
            xput16(ev + 20, 0); /* border */
            ev[22] = 0; /* override-redirect */
            x_reply(s, seq, ev, 32);
        }
        return;
    }
    case 4: { /* DestroyWindow */
        uint32_t wid = xget32(req + 4);
        struct x_win *win = win_find(c, wid);
        kprintf("[xsrv] DestroyWindow id=0x%x\n", wid);
        if (win && wid != XSRV_ROOT_ID) win->used = 0;
        /* DestroyNotify so clients waiting on StructureNotify wake. */
        if (wid != XSRV_ROOT_ID) {
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = 17; /* DestroyNotify */
            xput16(ev + 2, seq);
            xput32(ev + 4, wid);
            xput32(ev + 8, wid);
            x_reply(s, seq, ev, 32);
        }
        return;
    }
    case 36: /* GrabServer */
    case 37: /* UngrabServer */
        return;
    case 2: { /* ChangeWindowAttributes */
        uint32_t wid = xget32(req + 4);
        uint32_t mask = xget32(req + 8);
        kprintf("[xsrv] ChangeWindowAttributes wid=0x%x mask=0x%x\n",
                wid, mask);
        (void)wid; (void)mask;
        return;
    }
    case 3: { /* GetWindowAttributes */
        uint32_t wid = xget32(req + 4);
        struct x_win *win = win_find(c, wid);
        rep[0] = 1; rep[1] = 0; /* backing-store */
        xput16(rep + 2, seq);
        xput32(rep + 4, 3); /* length */
        xput32(rep + 8, XSRV_VISUAL_ID);
        xput16(rep + 12, 1); /* class InputOutput */
        rep[14] = 0; rep[15] = 0;
        xput32(rep + 16, 0);
        xput32(rep + 20, 0);
        xput32(rep + 24, win && win->mapped ? 1u : 0u);
        xput32(rep + 28, 0);
        x_reply(s, seq, rep, 32);
        /* extra 3*4 already claimed in length -- send pads */
        uint8_t extra[12]; memset(extra, 0, 12);
        x_reply(s, seq, extra, 12);
        return;
    }
    case 7: /* ReparentWindow */ return;
    case 8: case 9: { /* MapWindow / MapSubwindows */
        uint32_t wid = xget32(req + 4);
        struct x_win *win = win_find(c, wid);
        if (win) win->mapped = 1;
        c->focus = wid;
        /* Clients (Ozone) block on MapNotify after MapWindow. */
        {
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = 19; /* MapNotify */
            ev[1] = 0;  /* override-redirect */
            xput16(ev + 2, seq);
            /* StructureNotify is selected on the window itself → event=window */
            xput32(ev + 4, wid);
            xput32(ev + 8, wid);
            x_reply(s, seq, ev, 32);
        }
        /* Slice 90: VisibilityNotify BEFORE Expose, matching the control's
         * ground-truth order (logs/control/x11wire.txt after MapWindow:
         * MapNotify -> VisibilityNotify -> Expose -> ...). Chromium's Ozone
         * treats an unmapped-or-obscured window as "do not paint": without
         * this it mapped the window and then looped ChangeProperty /
         * GetInputFocus / SendEvent forever, never issuing a single
         * ShmPutImage. */
        {
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = 15; /* VisibilityNotify */
            xput16(ev + 2, seq);
            xput32(ev + 4, wid);
            ev[8] = 0;  /* VisibilityUnobscured */
            x_reply(s, seq, ev, 32);
        }
        /* Also expose so a client waiting on Expose starts painting. */
        {
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = 12; /* Expose */
            xput16(ev + 2, seq);
            xput32(ev + 4, wid);
            xput16(ev + 8, 0);
            xput16(ev + 10, 0);
            xput16(ev + 12, win ? win->w : (uint16_t)g_xf_w);
            xput16(ev + 14, win ? win->h : (uint16_t)g_xf_h);
            xput16(ev + 16, 0); /* count */
            x_reply(s, seq, ev, 32);
        }
        /* ConfigureNotify: the control emits one for the mapped frame (the
         * WM's final geometry). Ozone needs it to accept the window's size
         * as real before it will lay out and paint. */
        x_send_configure_notify(s, seq, wid,
                                win ? win->x : 0, win ? win->y : 0,
                                win ? win->w : (uint16_t)g_xf_w,
                                win ? win->h : (uint16_t)g_xf_h);
        return;
    }
    case 10: { /* UnmapWindow */
        uint32_t wid = xget32(req + 4);
        struct x_win *win = win_find(c, wid);
        if (win) win->mapped = 0;
        {
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = 18; /* UnmapNotify */
            xput16(ev + 2, seq);
            xput32(ev + 4, XSRV_ROOT_ID);
            xput32(ev + 8, wid);
            x_reply(s, seq, ev, 32);
        }
        return;
    }
    case 12: { /* ConfigureWindow */
        uint32_t wid = xget32(req + 4);
        struct x_win *win = win_find(c, wid);
        uint16_t mask = xget16(req + 8);
        const uint8_t *p = req + 12;
        if (win) {
            if (mask & 0x01) { win->x = (int16_t)xget32(p); p += 4; }
            if (mask & 0x02) { win->y = (int16_t)xget32(p); p += 4; }
            if (mask & 0x04) { win->w = (uint16_t)xget32(p); p += 4; }
            if (mask & 0x08) { win->h = (uint16_t)xget32(p); p += 4; }
            if (win->w && win->h) xframe_ensure(win->w, win->h);
        }
        {
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = 22; /* ConfigureNotify */
            xput16(ev + 2, seq);
            xput32(ev + 4, wid);
            xput32(ev + 8, wid);
            xput32(ev + 12, 0);
            xput16(ev + 16, win ? (uint16_t)win->x : 0);
            xput16(ev + 18, win ? (uint16_t)win->y : 0);
            xput16(ev + 20, win ? win->w : (uint16_t)g_xf_w);
            xput16(ev + 22, win ? win->h : (uint16_t)g_xf_h);
            xput16(ev + 24, 0);
            ev[26] = 0;
            x_reply(s, seq, ev, 32);
        }
        (void)mask;
        return;
    }
    case 15: { /* QueryTree */
        uint32_t wid = xget32(req + 4);
        rep[0] = 1;
        xput16(rep + 2, seq);
        xput32(rep + 4, 0); /* no children */
        xput32(rep + 8, XSRV_ROOT_ID);
        xput32(rep + 12, wid == XSRV_ROOT_ID ? 0 : XSRV_ROOT_ID);
        xput16(rep + 16, 0);
        x_reply32(s, seq, rep);
        return;
    }
    case 17: { /* GetAtomName */
        rep[0] = 1;
        xput16(rep + 2, seq);
        xput16(rep + 8, 4);
        xput32(rep + 4, 1); /* 4 bytes name */
        x_reply(s, seq, rep, 32);
        { uint8_t nm[4] = { 'A', 'T', 'O', 'M' }; x_reply(s, seq, nm, 4); }
        return;
    }
    case 23: { /* GetSelectionOwner */
        rep[0] = 1;
        xput16(rep + 2, seq);
        xput32(rep + 8, 0); /* None */
        x_reply32(s, seq, rep);
        return;
    }
    case 14: { /* GetGeometry */
        uint32_t wid = xget32(req + 4);
        struct x_win *win = win_find(c, wid);
        rep[0] = 1; rep[1] = 24; /* depth */
        xput16(rep + 2, seq);
        xput32(rep + 8, XSRV_ROOT_ID);
        xput16(rep + 12, win ? (uint16_t)win->x : 0);
        xput16(rep + 14, win ? (uint16_t)win->y : 0);
        xput16(rep + 16, win ? win->w : (uint16_t)g_xf_w);
        xput16(rep + 18, win ? win->h : (uint16_t)g_xf_h);
        xput16(rep + 20, 0);
        x_reply32(s, seq, rep);
        return;
    }
    case 16: { /* InternAtom — server-global table (shared across clients). */
        uint16_t nlen = xget16(req + 4);
        char abuf[48];
        uint16_t cpy = nlen < sizeof(abuf) - 1 ? nlen : (uint16_t)(sizeof(abuf) - 1);
        for (uint16_t i = 0; i < cpy; i++) abuf[i] = (char)req[8 + i];
        abuf[cpy] = 0;
        uint32_t existing = atom_lookup(abuf);
        uint32_t atom = existing ? existing : g_next_atom++;
        if (!existing) atom_store(atom, abuf);
        kprintf("[xsrv] InternAtom '%s' -> %u\n", abuf, atom);
        rep[0] = 1;
        xput16(rep + 2, seq);
        xput32(rep + 8, atom);
        x_reply32(s, seq, rep);
        /* After device-init wake, atom-cache end is another wait_for_event park. */
        if (c->idle_wake_armed && cpy == 14 &&
            !memcmp(abuf, "xwayland-touch", 14))
            x_wake_event(s, c, seq, "post-atom-cache");
        return;
    }
    case 22: { /* SetSelectionOwner — void */
        (void)req;
        return;
    }
    case 24: { /* ConvertSelection — synthesize SelectionNotify */
        uint8_t ev[32];
        memset(ev, 0, 32);
        ev[0] = 31; /* SelectionNotify */
        xput16(ev + 2, seq);
        xput32(ev + 4, xget32(req + 4));  /* requestor */
        xput32(ev + 8, xget32(req + 8));  /* selection */
        xput32(ev + 12, xget32(req + 12)); /* target */
        xput32(ev + 16, 0);               /* property None */
        xput32(ev + 20, 0);               /* time */
        x_reply(s, seq, ev, 32);
        return;
    }
    case 18: { /* ChangeProperty */
        uint32_t wid = xget32(req + 4);
        uint32_t prop = xget32(req + 8);
        uint32_t typ = xget32(req + 12);
        uint8_t format = req[16];
        uint32_t nunits = xget32(req + 20);
        uint32_t nbytes = nunits * (format == 16 ? 2u : format == 32 ? 4u : 1u);
        const uint8_t *val = req + 24;
        if (req_bytes < 24 + nbytes) nbytes = req_bytes > 24 ? req_bytes - 24 : 0;
        prop_set(wid, prop, typ, format, val, nbytes);
        {
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = 28; /* PropertyNotify */
            xput16(ev + 2, seq);
            xput32(ev + 4, wid);
            xput32(ev + 8, prop);
            ev[12] = 0; /* NewValue */
            x_reply(s, seq, ev, 32);
        }
        return;
    }
    case 20: { /* GetProperty — EWMH + stored props so Ozone leaves the WM wait */
        uint32_t wid = xget32(req + 4);
        uint32_t prop = xget32(req + 8);
        uint32_t type = xget32(req + 12);
        const char *pname = atom_name(prop);
        kprintf("[xsrv] GetProperty wid=0x%x prop=%u(%s) type=%u\n",
                wid, prop, pname ? pname : "?", type);
        /* Stored property wins (client ChangeProperty round-trip). */
        {
            struct x_prop *sp = prop_find(wid, prop);
            if (sp) {
                prop_reply(s, seq, sp->type, sp->format, sp->data, sp->len);
                return;
            }
        }
        if (pname && wid == XSRV_ROOT_ID) {
            if (!strcmp(pname, "_NET_WORKAREA") ||
                !strcmp(pname, "_NET_DESKTOP_GEOMETRY")) {
                uint32_t wa[4] = { 0, 0, g_xf_w, g_xf_h };
                prop_reply(s, seq, XA_CARDINAL, 32, wa,
                           !strcmp(pname, "_NET_DESKTOP_GEOMETRY") ? 8 : 16);
                return;
            }
            if (!strcmp(pname, "_NET_DESKTOP_VIEWPORT")) {
                uint32_t vp[2] = { 0, 0 };
                prop_reply(s, seq, XA_CARDINAL, 32, vp, sizeof vp);
                return;
            }
            if (!strcmp(pname, "_NET_CURRENT_DESKTOP") ||
                !strcmp(pname, "_NET_NUMBER_OF_DESKTOPS") ||
                !strcmp(pname, "_NET_ACTIVE_WINDOW")) {
                uint32_t v = !strcmp(pname, "_NET_NUMBER_OF_DESKTOPS") ? 1u :
                             !strcmp(pname, "_NET_ACTIVE_WINDOW") ? 0u : 0u;
                uint32_t typ = !strcmp(pname, "_NET_ACTIVE_WINDOW") ? XA_WINDOW
                                                                   : XA_CARDINAL;
                prop_reply(s, seq, typ, 32, &v, 4);
                return;
            }
            if (!strcmp(pname, "_NET_SUPPORTED")) {
                uint32_t list[12];
                int n = 0;
                list[n++] = atom_ensure("_NET_SUPPORTED");
                list[n++] = atom_ensure("_NET_WORKAREA");
                list[n++] = atom_ensure("_NET_WM_NAME");
                list[n++] = atom_ensure("_NET_WM_STATE");
                list[n++] = atom_ensure("_NET_WM_WINDOW_TYPE");
                list[n++] = atom_ensure("_NET_WM_PID");
                list[n++] = atom_ensure("_NET_ACTIVE_WINDOW");
                list[n++] = atom_ensure("_NET_CURRENT_DESKTOP");
                list[n++] = atom_ensure("_NET_NUMBER_OF_DESKTOPS");
                list[n++] = atom_ensure("_NET_DESKTOP_GEOMETRY");
                list[n++] = atom_ensure("_NET_DESKTOP_VIEWPORT");
                list[n++] = atom_ensure("_NET_FRAME_EXTENTS");
                prop_reply(s, seq, XA_ATOM, 32, list, (uint32_t)n * 4u);
                return;
            }
            if (!strcmp(pname, "RESOURCE_MANAGER")) {
                static const char rm[] = "Xft.dpi:\t96\nXft.rgba:\trgb\n";
                prop_reply(s, seq, 31 /* XA_STRING */, 8, rm, sizeof(rm) - 1);
                return;
            }
            if (!strcmp(pname, "_NET_SUPPORTING_WM_CHECK")) {
                /* SLICE 91: answer NONE -- we are NOT a window manager.
                 *
                 * This used to point at root so chrome would see "a WM
                 * exists". That was backwards. Declaring a WM makes Chromium
                 * take the DELEGATING path: it stops activating its own
                 * frame and instead asks the WM to, by SendEvent'ing
                 * _NET_ACTIVE_WINDOW at the root and then re-checking. We
                 * grant it (ConfigureNotify + FocusIn + _NET_ACTIVE_WINDOW),
                 * chrome does not believe it, and it retries forever --
                 * measured as an endless ChangeProperty / GetInputFocus /
                 * SendEvent loop after MapWindow, with ShmAttach never sent
                 * and therefore not one zero-copy frame in the entire arc.
                 *
                 * The control settles it: it is Xvfb with NO window manager
                 * running, so _NET_SUPPORTING_WM_CHECK is absent, and chrome
                 * takes the SELF-MANAGING path -- ConfigureWindow(Above),
                 * SetInputFocus, QueryPointer, then paint. That is the path
                 * we can actually satisfy: no WM to impersonate, just a
                 * server answering focus and geometry (which we now do).
                 * Falls through to the empty-property reply below. */
            }
            if (!strcmp(pname, "_NET_WM_NAME") && wid == XSRV_ROOT_ID) {
                static const char nm[] = "tobyWM";
                prop_reply(s, seq, atom_ensure("UTF8_STRING"), 8, nm,
                           sizeof(nm) - 1);
                return;
            }
        }
        /* Empty property (None) */
        rep[0] = 1;
        xput16(rep + 2, seq);
        x_reply32(s, seq, rep);
        return;
    }
    case 38: { /* QueryPointer */
        rep[0] = 1; rep[1] = 1; /* same-screen */
        xput16(rep + 2, seq);
        xput32(rep + 8, XSRV_ROOT_ID);
        xput32(rep + 12, c->focus ? c->focus : XSRV_ROOT_ID);
        x_reply32(s, seq, rep);
        return;
    }
    case 40: { /* TranslateCoordinates */
        rep[0] = 1; rep[1] = 1;
        xput16(rep + 2, seq);
        xput32(rep + 8, xget32(req + 8)); /* child = dst */
        xput16(rep + 12, xget16(req + 12));
        xput16(rep + 14, xget16(req + 14));
        x_reply32(s, seq, rep);
        return;
    }
    case 25: { /* SendEvent -- EWMH client->WM requests (slice 90) */
        /* There was NO handler for this at all: it fell through to
         * "unhandled opcode (ignored)". Chromium does not call SetInputFocus
         * to activate its frame -- it asks the WINDOW MANAGER to, by
         * SendEvent'ing a _NET_ACTIVE_WINDOW ClientMessage at the root. With
         * nobody playing WM, the request vanished and chrome sat in a
         * ChangeProperty / GetInputFocus / SendEvent retry loop forever,
         * never painting. We are the server AND the WM here, so honour it.
         *
         * Wire: [0]=25 [1]=propagate [2..3]=len [4..7]=destination
         *       [8..11]=event-mask [12..43]=the 32-byte event.
         * Embedded ClientMessage: [0]=33 [4..7]=window [8..11]=type atom. */
        if (req_bytes < 44) return;
        const uint8_t *iev = req + 12;
        if (iev[0] != 33 /* ClientMessage */) return;
        uint32_t target = xget32(iev + 4);
        uint32_t mtype  = xget32(iev + 8);
        const char *mname = atom_name(mtype);
        kprintf("[xsrv] SendEvent ClientMessage type=%u(%s) target=0x%x\n",
                mtype, mname ? mname : "?", target);
        if (mname && !strcmp(mname, "_NET_ACTIVE_WINDOW") && target) {
            struct x_win *tw = win_find(c, target);
            c->focus = target;
            /* Act as the WM would: raise, confirm geometry, hand it focus. */
            x_send_configure_notify(s, seq, target,
                                    tw ? tw->x : 0, tw ? tw->y : 0,
                                    tw ? tw->w : (uint16_t)g_xf_w,
                                    tw ? tw->h : (uint16_t)g_xf_h);
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = 9;              /* FocusIn */
            ev[1] = 3;              /* detail = Nonlinear */
            xput16(ev + 2, seq);
            xput32(ev + 4, target);
            ev[8] = 0;              /* mode = Normal */
            x_reply(s, seq, ev, 32);
            /* Publish the EWMH state chrome polls for confirmation. */
            prop_set(XSRV_ROOT_ID, atom_ensure("_NET_ACTIVE_WINDOW"),
                     XA_WINDOW, 32, &target, 4);
            memset(ev, 0, 32);
            ev[0] = 28;             /* PropertyNotify on root */
            xput16(ev + 2, seq);
            xput32(ev + 4, XSRV_ROOT_ID);
            xput32(ev + 8, atom_ensure("_NET_ACTIVE_WINDOW"));
            x_reply(s, seq, ev, 32);
        }
        return;
    }
    case 42: { /* SetInputFocus */
        c->focus = xget32(req + 4);
        /* Slice 90: the control answers SetInputFocus with FocusOut then
         * FocusIn. Ozone gates "this window is active" on FocusIn, and an
         * inactive window is one it will not paint -- which is why the
         * post-MapWindow loop kept re-asking (GetInputFocus / SendEvent)
         * instead of ever reaching ShmPutImage. */
        if (c->focus && c->focus != XSRV_ROOT_ID) {
            uint8_t ev[32];
            memset(ev, 0, 32);
            ev[0] = 10;             /* FocusOut */
            ev[1] = 5;              /* detail = Pointer */
            xput16(ev + 2, seq);
            xput32(ev + 4, c->focus);
            ev[8] = 0;              /* mode = Normal */
            x_reply(s, seq, ev, 32);

            memset(ev, 0, 32);
            ev[0] = 9;              /* FocusIn */
            ev[1] = 3;              /* detail = Nonlinear */
            xput16(ev + 2, seq);
            xput32(ev + 4, c->focus);
            ev[8] = 0;              /* mode = Normal */
            x_reply(s, seq, ev, 32);
        }
        return;
    }
    case 43: { /* GetInputFocus */
        rep[0] = 1; rep[1] = 1; /* revert-to Parent */
        xput16(rep + 2, seq);
        xput32(rep + 8, c->focus ? c->focus : XSRV_ROOT_ID);
        x_reply32(s, seq, rep);
        return;
    }
    case 45: /* OpenFont */ return;
    case 55: { /* CreateGC */
        uint32_t gid = xget32(req + 4);
        for (int i = 0; i < X_MAX_GC; i++) {
            if (!c->gcs[i].used) {
                c->gcs[i].used = 1;
                c->gcs[i].id = gid;
                return;
            }
        }
        return;
    }
    case 56: case 60: /* ChangeGC / FreeGC */ return;
    case 61: /* ClearArea */ return;
    case 62: /* CopyArea */ return;
    case 70: /* PolyFillRectangle */ return;
    case 72: { /* PutImage */
        if (req_bytes < 24) return;
        uint16_t w = xget16(req + 12);
        uint16_t h = xget16(req + 14);
        int16_t dx = (int16_t)xget16(req + 16);
        int16_t dy = (int16_t)xget16(req + 18);
        uint8_t depth = req[1];
        uint8_t format = req[20]; /* in standard layout byte 1 is format... */
        (void)depth; (void)format;
        /* Wire: opcode, format, length, drawable, gc, width, height, dstx, dsty, left_pad, depth */
        format = req[1];
        depth = req[21];
        const uint8_t *pixels = req + 24;
        uint32_t pix_bytes = req_bytes - 24;
        uint32_t stride = ((w * (depth == 24 ? 4u : 4u) + 3u) & ~3u);
        if (format == 2 /* ZPixmap */ && pix_bytes >= stride * h)
            blit_z_rgb32((uint32_t)dx, (uint32_t)dy, w, h, pixels, stride);
        return;
    }
    case 78: /* CreateColormap */ return;
    case 91: /* QueryBestSize */ {
        rep[0] = 1;
        xput16(rep + 2, seq);
        xput16(rep + 8, xget16(req + 8));
        xput16(rep + 10, xget16(req + 10));
        x_reply32(s, seq, rep);
        return;
    }
    case 98: { /* QueryExtension */
        uint16_t nlen = xget16(req + 4);
        const char *name = (const char *)(req + 8);
        char nbuf[64];
        uint16_t cpy = nlen < sizeof(nbuf) - 1 ? nlen : (uint16_t)(sizeof(nbuf) - 1);
        for (uint16_t i = 0; i < cpy; i++) nbuf[i] = name[i];
        nbuf[cpy] = 0;
        uint8_t major = 0, first_ev = 0;
        int present = 0;
        uint8_t first_err = 0;
        if (nlen == 7 && !memcmp(name, "MIT-SHM", 7)) {
            present = 1; major = XSRV_MIT_SHM_MAJOR;
            first_ev = XSRV_EV_MIT_SHM; first_err = XSRV_ERR_MIT_SHM;
        }
#ifndef XSRV_EXT_MINIMAL
        else if (nlen == 5 && !memcmp(name, "RANDR", 5)) {
            present = 1; major = XSRV_RANDR_MAJOR;
            first_ev = XSRV_EV_RANDR; first_err = XSRV_ERR_RANDR;
        } else if (nlen == 6 && !memcmp(name, "XFIXES", 6)) {
            present = 1; major = XSRV_XFIXES_MAJOR;
            first_ev = XSRV_EV_XFIXES; first_err = XSRV_ERR_XFIXES;
        } else if (nlen == 12 && !memcmp(name, "BIG-REQUESTS", 12)) {
            present = 1; major = XSRV_BIGREQ_MAJOR;
        }
#endif
        /* SYNC left absent: advertising it + QueryVersion desynced xcb and
         * ImmediateCrash'd (vec=3) right after GetModifierMapping.
         *
         * SLICE 89 -- XSRV_EXT_MINIMAL (default ON): advertise ONLY MIT-SHM.
         * Slice 88's control run (xtrace --denyextensions) proved this exact
         * chrome + flags MapWindows with ZERO extensions, while our runs --
         * which advertised RANDR + XFIXES + BIG-REQUESTS -- park before
         * MapWindow. The ledger's "do not ADD extensions on a hunch" said
         * nothing about the mixed state we were actually in: chrome takes the
         * extension-using path for what we advertise, then waits on companion
         * requests our hand-rolled replies never satisfy. Minimal is the
         * CONTROL-PROVEN configuration; MIT-SHM is kept because tier 2.5's
         * zero-copy frames require it. Build with -DXSRV_EXT_ALL to A/B. */
        kprintf("[xsrv] QueryExtension '%s' present=%d maj=%u ev=%u\n",
                nbuf, present, major, first_ev);
        rep[0] = 1;
        xput16(rep + 2, seq);
        rep[8] = present ? 1 : 0;
        rep[9] = major;
        rep[10] = first_ev;
        rep[11] = first_err;
        x_reply32(s, seq, rep);
        return;
    }
    case 99: { /* ListExtensions — coalesced */
        uint8_t big[40];
        memset(big, 0, sizeof big);
        big[0] = 1; big[1] = 1;
        xput16(big + 2, seq);
        xput32(big + 4, 2);
        big[32] = 7;
        big[33] = 'M'; big[34] = 'I'; big[35] = 'T';
        big[36] = '-'; big[37] = 'S'; big[38] = 'H'; big[39] = 'M';
        x_reply(s, seq, big, 40);
        return;
    }
    case 101: { /* GetKeyboardMapping — one coalesced reply (ring depth 64). */
        uint8_t first = req[4];
        uint8_t count = req[5];
        /* Cap: a hostile/huge count used to emit megabyte replies and smash
         * xcb stack buffers (canary mismatch right after this reply). */
        if (count == 0) count = 1;
        if (count > 32) count = 32;
        uint32_t nsym = (uint32_t)count; /* keysyms-per-keycode = 1 */
        uint32_t total = 32 + nsym * 4;
        uint8_t *big = kmalloc(total);
        if (!big) { x_error(s, seq, 11, 0, 0, op); return; }
        memset(big, 0, total);
        big[0] = 1; big[1] = 1; /* keysyms-per-keycode */
        xput16(big + 2, seq);
        xput32(big + 4, nsym);  /* length in 4-byte units */
        for (uint32_t i = 0; i < nsym; i++)
            xput32(big + 32 + i * 4, 0x0020u + first + i);
        x_reply(s, seq, big, total);
        kfree(big);
        return;
    }
    case 119: { /* GetModifierMapping — coalesced */
        uint8_t big[48];
        memset(big, 0, sizeof big);
        big[0] = 1; big[1] = 2;
        xput16(big + 2, seq);
        xput32(big + 4, 4); /* 16 bytes of modifier map */
        x_reply(s, seq, big, 48);
        /* Secondary conns stop here (no GetPointerMapping). Wake so they leave
         * xcb_wait_for_event. Safe with SYNC absent; with SYNC present this
         * raced and ImmediateCrash'd. */
        if (!c->idle_wake_armed)
            x_wake_event(s, c, seq, "GetModifierMapping");
        return;
    }
    case 116: /* SetPointerMapping — accept identity map */
        {
            uint8_t n = req[1];
            rep[0] = 1; rep[1] = 0; /* MappingSuccess */
            xput16(rep + 2, seq);
            (void)n;
            x_reply32(s, seq, rep);
            return;
        }
    case 117: { /* GetPointerMapping — 3-button identity */
        uint8_t big[36];
        memset(big, 0, sizeof big);
        big[0] = 1; big[1] = 3; /* map-length */
        xput16(big + 2, seq);
        xput32(big + 4, 1); /* 4 bytes of map data */
        big[32] = 1; big[33] = 2; big[34] = 3;
        {
            uint32_t hlen = 0; uint8_t h0 = 0, h1 = 0;
            if (s->count > 0) {
                struct sock_dgram *hd = &s->dgrams[s->tail];
                hlen = hd->len;
                if (hd->payload && hd->len >= 2) {
                    h0 = hd->payload[0]; h1 = hd->payload[1];
                }
            }
            kprintf("[xsrv] GetPointerMapping seq=%u ring=%u pend=%u "
                    "head_len=%u head=%u/%u\n",
                    seq, (unsigned)s->count, c->pend_n, hlen, h0, h1);
        }
        x_reply(s, seq, big, 36);
        kprintf("[xsrv] GetPointerMapping replied ring=%u pend=%u\n",
                (unsigned)s->count, c->pend_n);
        x_wake_event(s, c, seq, "GetPointerMapping");
        return;
    }
    case 104: /* Bell */ return;
    case 107: /* ChangePointerControl */ return;
    case 108: { /* GetPointerControl */
        rep[0] = 1;
        xput16(rep + 2, seq);
        xput16(rep + 8, 1);  /* accel-numerator */
        xput16(rep + 10, 1); /* accel-denominator */
        xput16(rep + 12, 4); /* threshold */
        x_reply32(s, seq, rep);
        return;
    }
    case 109: /* SetScreenSaver */ return;
    case 110: { /* GetScreenSaver */
        rep[0] = 1;
        xput16(rep + 2, seq);
        xput16(rep + 8, 600); /* timeout */
        xput16(rep + 10, 600);
        rep[12] = 1; rep[13] = 1;
        x_reply32(s, seq, rep);
        return;
    }
    case 113: /* GetKeyboardControl — stub zeros OK for Ozone probes */
        {
            uint8_t big[32 + 32];
            memset(big, 0, sizeof big);
            big[0] = 1; big[1] = 1; /* global-auto-repeat On */
            xput16(big + 2, seq);
            xput32(big + 4, 5); /* 20 bytes led + 32 key-click... length=5 -> 20 */
            /* Actually reply length is 5 (20 bytes of key-click-percent already in
             * fixed portion... keep minimal fixed 32). */
            xput32(big + 4, 0);
            x_reply(s, seq, big, 32);
            return;
        }
    default:
        /* Prefer silent ignore over BadImplementation: an error reply for a
         * void probe can put xcb into a sticky error state and stall Ozone.
         * Checked requests that need replies will show up as recv hangs —
         * log them and implement the next opcode. */
        {
            static int logged;
            if (logged < 80) {
                logged++;
                kprintf("[xsrv] unhandled opcode=%u seq=%u len=%u (ignored)\n",
                        op, seq, req_bytes);
            }
        }
        return;
    }
}

/* Decode complete requests already buffered. Stops when the client's RX
 * ring needs draining so replies are never dropped. */
static void pump_requests(struct sock *self, struct x_conn *c) {
    if (!c || !c->rbuf) return;
    for (;;) {
        if (c->rlen < 4) break;
        uint32_t words = xget16(c->rbuf + 2);
        if (words < 1) { c->rlen = 0; break; }
        uint32_t need = words * 4;
        if (need > c->rcap) {
            kprintf("[xsrv] request too large (%u), resetting\n", need);
            c->rlen = 0;
            break;
        }
        if (c->rlen < need) break;
        if (self->count + 4 >= SOCK_RX_DGRAMS) {
            g_x_backpressure = 1;
            break;
        }
        c->last_req_ms = perf_now_ns() / 1000000ull;
        if (c->rbuf[0] != 16) {
            static int nreq;
            if (nreq < 400) {
                nreq++;
                /* Slice 89: conn + seq on every request line. The stall
                 * diagnosis needs "which conn, which seq" to line up against
                 * chrome's ui/gfx/x narration of the reply it waits for. */
                kprintf("[xsrv] req c=%d op=%u words=%u seq=%u\n",
                        sock_idx(self), c->rbuf[0], need / 4,
                        (unsigned)(uint16_t)(c->seq + 1));
            }
        }
        handle_request(self, c, c->rbuf, need);
        memmove(c->rbuf, c->rbuf + need, c->rlen - need);
        c->rlen -= need;
        if (g_x_backpressure) break;
    }
}

void xserver_pump(struct sock *self) {
    if (!self || !self->x_server || !self->x_setup_done) return;
    int idx = sock_idx(self);
    if (idx < 0) return;
    g_x_backpressure = 0;
    x_pend_flush(self, &g_xconn[idx]);
    pump_requests(self, &g_xconn[idx]);
}

void xserver_on_poll(struct sock *self) {
    xserver_pump(self);
    /* When the client has drained all replies and is idle in wait_for_event,
     * poke so deferred UI work can run. Never inject while replies are
     * queued (that desynced xcb → INT3). */
    if (!self || !self->x_setup_done) return;
    int idx = sock_idx(self);
    if (idx < 0) return;
    struct x_conn *c = &g_xconn[idx];
    if (!c->idle_wake_armed || c->pend_n || c->rlen)
        return;
    /* If RX already has data the waiter should be runnable; still re-poke
     * after 500ms in case the event was for a different interest set. */
    static uint64_t last_poke_ms[SOCK_MAX];
    uint64_t now = perf_now_ns() / 1000000ull;
    uint64_t gap = self->count ? 500u : 100u;
    if (last_poke_ms[idx] && now - last_poke_ms[idx] < gap)
        return;
    if (self->count >= 8) {
        /* Tier 2.5: if the client really is blocked in recvmsg on THIS
         * conn, suppressing the poke here would wedge it forever -- a
         * blocked reader cannot drain the ring that gates its own wake.
         * Log once so a silent stall can be attributed. */
        static uint8_t muted[SOCK_MAX];
        int mi = sock_idx(self);
        if (mi >= 0 && !muted[mi]) {
            muted[mi] = 1;
            kprintf("[xsrv] poke MUTED sock=%d count=%u (unread ring)\n",
                    mi, (unsigned)self->count);
        }
        return; /* don't flood a stuck non-reader */
    }
    last_poke_ms[idx] = now;
    x_wake_event(self, c, c->seq, "poll-idle");
}

static void xconn_summary(struct sock *s) {
    int idx = sock_idx(s);
    if (idx < 0) return;
    struct x_conn *c = &g_xconn[idx];
    if (!c->active) return;
    uint64_t now = perf_now_ns() / 1000000ull;
    kprintf("[xsum] c=%d seq=%u lastreq=%lums rlen=%u pend=%u rx=%u armed=%u\n",
            idx, c->seq,
            (unsigned long)(c->last_req_ms ? now - c->last_req_ms : 0),
            c->rlen, c->pend_n, (unsigned)s->count, c->idle_wake_armed);
}

/* Slice 89: called from the sched_tick heartbeat, NOT from pid 0's idle
 * loop. The original placement inside xserver_tick never printed a single
 * line all run: under chrome load pid 0 is the idle task and never gets the
 * CPU, so ANY diagnostic hung off idle_loop is invisible in exactly the
 * situation it exists to explain. (The 100ms "wake (poll-idle)" lines that
 * kept appearing come from file_poll_ready's xserver_on_poll, a different
 * caller -- which is what made the gap easy to miss.) */
void xserver_debug_tick(void) {
    sock_foreach_xserver(xconn_summary);
}

void xserver_tick(void) {
    /* recvmsg waiters never enter poll — poke from the idle heartbeat. */
    sock_foreach_xserver(xserver_on_poll);
}

/* Tier 2.5 headed wall: called from the [uxstuck] probe (socket.c) when a
 * blocking recvmsg on a fake-X socket has waited >5s with an empty ring.
 * Prints exactly why the idle-wake machinery is not poking this conn --
 * every gate in xserver_on_poll is reproduced here as data. */
void xserver_debug_conn(struct sock *self) {
    int idx = sock_idx(self);
    if (idx < 0) { kprintf("[xdbg] no idx\n"); return; }
    struct x_conn *c = &g_xconn[idx];
    kprintf("[xdbg] sock=%d active=%u seq=%u rlen=%u pend_n=%u armed=%u "
            "count=%u setup=%u\n",
            idx, c->active, c->seq, c->rlen, c->pend_n, c->idle_wake_armed,
            (unsigned)self->count, (unsigned)self->x_setup_done);
    if (c->rlen >= 4)
        kprintf("[xdbg]   rbuf[0..3]=%u,%u,%u,%u (op,pad,len16lo,len16hi)\n",
                c->rbuf[0], c->rbuf[1], c->rbuf[2], c->rbuf[3]);
}

long xserver_handle(struct sock *self, const void *buf, size_t n) {
    int idx = sock_idx(self);
    if (idx < 0) return (long)n;
    struct x_conn *c = &g_xconn[idx];
    size_t consumed = 0;
    g_x_backpressure = 0;

    if (!self->x_setup_done) {
        if (n < 12) return (long)n;
        uint8_t reply[160];
        xframe_ensure(800, 600);
        size_t rlen = xserver_build_setup(reply, sizeof reply);
        if (rlen && sock_unix_enqueue(self, reply, rlen) == SOCK_ERR_AGAIN)
            return 0;
        self->x_setup_done = 1;
        c->active = 1;
        c->seq = 0;
        c->rlen = 0;
        if (!c->rbuf) {
            c->rcap = X_REQ_CAP;
            c->rbuf = kmalloc(c->rcap);
        }
        kprintf("[xsrv] setup: client sent %d bytes, replied %d bytes\n",
                (int)n, (int)rlen);
        const uint8_t *p = (const uint8_t *)buf;
        uint16_t an = xget16(p + 6), ad = xget16(p + 8);
        size_t skip = 12 + ((an + 3u) & ~3u) + ((ad + 3u) & ~3u);
        if (skip > n) return (long)n;
        buf = p + skip;
        n -= skip;
        consumed = skip;
        if (n == 0) return (long)consumed;
    }

    if (!c->rbuf) {
        c->rcap = X_REQ_CAP;
        c->rbuf = kmalloc(c->rcap);
        if (!c->rbuf) return (long)(consumed + n);
    }

    /* Drain parked replies, then finish parked requests, before more input. */
    x_pend_flush(self, c);
    pump_requests(self, c);
    if (g_x_backpressure) return (long)consumed;

    const uint8_t *in = (const uint8_t *)buf;
    size_t left = n;
    while (left > 0 && !g_x_backpressure) {
        size_t room = c->rcap - c->rlen;
        if (room == 0) {
            pump_requests(self, c);
            if (c->rlen == c->rcap) break;
            continue;
        }
        size_t take = left < room ? left : room;
        memcpy(c->rbuf + c->rlen, in, take);
        c->rlen += (uint32_t)take;
        in += take;
        left -= take;
        consumed += take;
        pump_requests(self, c);
    }
    /* Unread tail (left > 0) is a short write. Parked requests in rbuf are
     * finished by xserver_pump when the client drains RX. */
    return (long)consumed;
}

long xframe_poll(uint32_t *w, uint32_t *h, uint32_t *stride,
                 uint32_t *gen, void *pixels, size_t pixels_cap) {
    if (!g_xf_pixels) xframe_ensure(800, 600);
    if (w) *w = g_xf_w;
    if (h) *h = g_xf_h;
    if (stride) *stride = g_xf_stride;
    if (gen) *gen = __atomic_load_n(&g_xf_gen, __ATOMIC_ACQUIRE);
    if (pixels && pixels_cap) {
        size_t nbytes = (size_t)g_xf_stride * g_xf_h;
        if (nbytes > pixels_cap) nbytes = pixels_cap;
        if (g_xf_pixels) memcpy(pixels, g_xf_pixels, nbytes);
        else memset(pixels, 0, nbytes);
    }
    return 0;
}
