/* vnc.c -- RFB 3.8 remote desktop server on TCP/5900.
 *
 * Shape of this file: a cooperative, NON-BLOCKING state machine, driven from
 * net_service_tick() alongside ssh_poll(). That entry point runs on the
 * syscall-return path, so nothing here may block -- a full-screen update is
 * megabytes, and sending it with the blocking tcp_send() would stall the
 * whole machine mid-syscall. Instead pixels are encoded into a bounded TX
 * window a slice at a time and drained with tcp_send_nb().
 *
 * Input is injected as PS/2 scancodes rather than cooked characters. That
 * reuses the real driver path -- shift/ctrl state, the E0 arrow cluster, and
 * the evdev feed that Linux processes read from /dev/input/event0 -- so a
 * remote key is indistinguishable from a local one. See vnc_key().
 *
 * The pointer is placed ABSOLUTELY (gui_pointer_warp), because RFB carries
 * absolute coordinates and the relative path applies up to 5x acceleration.
 *
 * Not drawn remotely: the mouse cursor, when the display backend puts it on
 * a hardware plane instead of the back buffer. Viewers draw their own local
 * pointer over the window, and we place ours exactly where they point, so
 * the two coincide.
 */

#include <tobyos/vnc.h>
#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/des.h>
#include <tobyos/rng.h>
#include <tobyos/gfx.h>
#include <tobyos/gui.h>
#include <tobyos/keyboard.h>
#include <tobyos/mouse.h>
#include <tobyos/clipboard.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/pit.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* ---- tunables ----------------------------------------------------- */

#define VNC_RX_MAX      4096u
/* Sized so a single full-width pixel row always fits (16384px at 32bpp),
 * which lets the raw encoder step a whole row at a time. */
#define VNC_TX_MAX      65536u
#define VNC_TILE        16
#define VNC_MAX_FAILS   5      /* consecutive auth failures before a cool-off */
#define VNC_COOLOFF_SEC 30u
#define VNC_RETRY_SEC   5u     /* how often to re-check for a password file */

/* RFB encodings we understand on the wire. */
#define ENC_RAW          0
#define ENC_HEXTILE      5
#define ENC_DESKTOPSIZE  (-223)

/* Hextile subencoding bits. */
#define HEX_RAW          1
#define HEX_BG           2

/* ---- state -------------------------------------------------------- */

enum vnc_state {
    VS_IDLE = 0,
    VS_VERSION,    /* sent our version, awaiting the client's 12 bytes   */
    VS_SECURITY,   /* sent the type list, awaiting a 1-byte choice       */
    VS_AUTH,       /* sent the 16-byte challenge, awaiting the response  */
    VS_INIT,       /* auth passed, awaiting ClientInit                   */
    VS_RUN,
};

struct vnc_pixfmt {
    uint8_t  bpp, depth, big_endian, true_colour;
    uint16_t rmax, gmax, bmax;
    uint8_t  rsh, gsh, bsh;
};

static struct tcp_conn *g_lsn;
static struct tcp_conn *g_cli;
static enum vnc_state   g_state;

static uint8_t  g_rx[VNC_RX_MAX];
static size_t   g_rx_len;
static uint8_t  g_tx[VNC_TX_MAX];
static size_t   g_tx_head, g_tx_tail;

static uint8_t  g_passwd[VNC_PASSWD_LEN];
static bool     g_have_passwd;
static uint8_t  g_challenge[16];
static uint32_t g_fail_streak;
static uint64_t g_cooloff_until;   /* pit tick; 0 = not cooling off */

static struct vnc_pixfmt g_fmt;
static bool     g_enc_hextile;
static bool     g_enc_desktopsize;

/* Pending client request for an update. */
static bool     g_req_pending;
static bool     g_req_incremental;
static int      g_req_x, g_req_y, g_req_w, g_req_h;

/* Damage accumulated since we last started an update. */
static bool     g_dmg_valid;
static int      g_dmg_x, g_dmg_y, g_dmg_w, g_dmg_h;

/* In-flight update being streamed out. */
static bool     g_upd_active;
static bool     g_upd_hextile;
static int      g_upd_x, g_upd_y, g_upd_w, g_upd_h;
static int      g_upd_row;       /* raw: next row; hextile: next tile row */
static int      g_upd_col;       /* hextile: next tile column             */
static uint32_t g_hex_bg;
static bool     g_hex_bg_valid;

/* Oversized ClientCutText we are draining and discarding. */
static uint32_t g_discard;

/* Guest-clipboard change detection (see vnc_push_clipboard). */
static uint32_t g_clip_sig;
static uint64_t g_clip_next;

static uint32_t g_scr_w, g_scr_h;   /* geometry the client was told about */
static struct vnc_stats g_stats;

/* ---- byte order helpers (RFB is big-endian on the wire) ----------- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* ---- TX window ----------------------------------------------------- */

static size_t tx_pending(void) { return g_tx_tail - g_tx_head; }

static void tx_compact(void) {
    if (g_tx_head == 0) return;
    size_t n = tx_pending();
    if (n) memmove(g_tx, g_tx + g_tx_head, n);
    g_tx_head = 0;
    g_tx_tail = n;
}

static size_t tx_space(void) {
    tx_compact();
    return VNC_TX_MAX - g_tx_tail;
}

static bool tx_put(const void *buf, size_t n) {
    if (tx_space() < n) return false;
    memcpy(g_tx + g_tx_tail, buf, n);
    g_tx_tail += n;
    return true;
}

/* Drain as much as the send window will take right now. Never blocks. */
static void tx_flush(void) {
    while (g_cli && g_tx_head < g_tx_tail) {
        long s = tcp_send_nb(g_cli, g_tx + g_tx_head, tx_pending());
        if (s <= 0) break;
        g_tx_head += (size_t)s;
        g_stats.bytes_sent += (uint64_t)s;
    }
    if (g_tx_head == g_tx_tail) g_tx_head = g_tx_tail = 0;
}

/* ---- connection lifecycle ------------------------------------------ */

static void vnc_reset_session(void) {
    g_rx_len = 0;
    g_tx_head = g_tx_tail = 0;
    g_state = VS_IDLE;
    g_req_pending = false;
    g_dmg_valid = false;
    g_upd_active = false;
    g_discard = 0;
    g_enc_hextile = false;
    g_enc_desktopsize = false;
    g_hex_bg_valid = false;
    g_clip_sig = 0;
    g_clip_next = 0;

    /* Default to our own preferred format until the client says otherwise:
     * 32bpp little-endian XRGB, which is exactly the back buffer layout. */
    g_fmt.bpp = 32; g_fmt.depth = 24;
    g_fmt.big_endian = 0; g_fmt.true_colour = 1;
    g_fmt.rmax = g_fmt.gmax = g_fmt.bmax = 255;
    g_fmt.rsh = 16; g_fmt.gsh = 8; g_fmt.bsh = 0;
}

static void vnc_close_client(const char *why) {
    if (g_cli) {
        tcp_close(g_cli);
        g_cli = 0;
        kprintf("[vnc] client disconnected (%s)\n", why ? why : "closed");
    }
    vnc_reset_session();
}

/* ---- password ------------------------------------------------------ */

/* The obfuscation key the reference vncpasswd uses. This is NOT a secret
 * and provides NO security -- it exists so a stored password is not
 * plaintext on disk, and so our file interchanges with real vncpasswd
 * files. The wire authentication below is what actually matters. */
static const uint8_t VNC_FIXED_KEY[VNC_PASSWD_LEN] = {
    23, 82, 107, 6, 35, 78, 88, 7
};

static void vnc_obfuscate(const uint8_t in[8], uint8_t out[8], bool decrypt) {
    struct des_ctx ctx;
    des_set_key_vnc(&ctx, VNC_FIXED_KEY);
    if (decrypt) des_decrypt_block(&ctx, in, out);
    else         des_encrypt_block(&ctx, in, out);
}

int vnc_set_password(const char *plain) {
    uint8_t pw[VNC_PASSWD_LEN];
    memset(pw, 0, sizeof(pw));
    if (plain) {
        for (size_t i = 0; i < VNC_PASSWD_LEN && plain[i]; i++)
            pw[i] = (uint8_t)plain[i];
    }

    uint8_t enc[VNC_PASSWD_LEN];
    vnc_obfuscate(pw, enc, false);

    /* Persistent store first. With no /data (no provisioned disk) fall
     * back to the ramfs copy and say plainly that it is temporary --
     * silently losing the password on reboot would look like the server
     * breaking itself. */
    (void)vfs_mkdir(VNC_PASSWD_DIR);
    int rc = vfs_write_all(VNC_PASSWD_PATH, enc, sizeof(enc));
    if (rc == VFS_OK) {
        (void)vfs_chmod(VNC_PASSWD_PATH, 0600);
    } else {
        rc = vfs_write_all(VNC_PASSWD_ALT, enc, sizeof(enc));
        if (rc == VFS_OK) {
            (void)vfs_chmod(VNC_PASSWD_ALT, 0600);
            kprintf("[vnc] no persistent store; password saved to %s "
                    "and will be lost on reboot\n", VNC_PASSWD_ALT);
        }
    }

    memset(pw, 0, sizeof(pw));
    memset(enc, 0, sizeof(enc));
    return rc;
}

static bool vnc_load_from(const char *path) {
    void  *raw = 0;
    size_t sz  = 0;
    if (vfs_read_all(path, &raw, &sz) != VFS_OK) return false;
    if (!raw) return false;

    /* Real vncpasswd files are exactly 8 bytes; some tools append a second
     * (view-only) password. Take the first block either way. */
    if (sz < VNC_PASSWD_LEN) {
        kfree(raw);
        kprintf("[vnc] %s is %u bytes; expected at least %u\n",
                path, (unsigned)sz, (unsigned)VNC_PASSWD_LEN);
        return false;
    }

    vnc_obfuscate((const uint8_t *)raw, g_passwd, true);
    memset(raw, 0, sz);
    kfree(raw);
    g_have_passwd = true;
    kprintf("[vnc] password loaded from %s\n", path);
    return true;
}

static bool vnc_load_password(void) {
    return vnc_load_from(VNC_PASSWD_PATH) || vnc_load_from(VNC_PASSWD_ALT);
}

/* ---- pixel conversion ---------------------------------------------- */

static size_t vnc_bypp(void) { return (size_t)(g_fmt.bpp / 8u); }

static uint32_t vnc_pack(uint32_t xrgb) {
    uint32_t r = (xrgb >> 16) & 0xFFu;
    uint32_t gg = (xrgb >> 8) & 0xFFu;
    uint32_t b = xrgb & 0xFFu;
    uint32_t v = 0;
    v |= ((r  * (uint32_t)g_fmt.rmax) / 255u) << g_fmt.rsh;
    v |= ((gg * (uint32_t)g_fmt.gmax) / 255u) << g_fmt.gsh;
    v |= ((b  * (uint32_t)g_fmt.bmax) / 255u) << g_fmt.bsh;
    return v;
}

static void vnc_put_pixel(uint8_t *dst, uint32_t v) {
    size_t n = vnc_bypp();
    if (g_fmt.big_endian) {
        for (size_t i = 0; i < n; i++)
            dst[i] = (uint8_t)(v >> (8u * (n - 1u - i)));
    } else {
        for (size_t i = 0; i < n; i++)
            dst[i] = (uint8_t)(v >> (8u * i));
    }
}

/* ---- keyboard: X11 keysym -> PS/2 set-1 scancode -------------------- */

/* US layout. Index by letter for a-z; a table for everything else. */
static const uint8_t SC_LETTER[26] = {
    0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
    0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C
};

struct sym_entry {
    uint32_t sym;
    uint8_t  sc;
    uint8_t  shift;   /* the key must be typed with shift held */
    uint8_t  ext;     /* E0-prefixed */
};

static const struct sym_entry SYMTAB[] = {
    /* punctuation and digits, unshifted then shifted */
    {'1',0x02,0,0}, {'!',0x02,1,0},
    {'2',0x03,0,0}, {'@',0x03,1,0},
    {'3',0x04,0,0}, {'#',0x04,1,0},
    {'4',0x05,0,0}, {'$',0x05,1,0},
    {'5',0x06,0,0}, {'%',0x06,1,0},
    {'6',0x07,0,0}, {'^',0x07,1,0},
    {'7',0x08,0,0}, {'&',0x08,1,0},
    {'8',0x09,0,0}, {'*',0x09,1,0},
    {'9',0x0A,0,0}, {'(',0x0A,1,0},
    {'0',0x0B,0,0}, {')',0x0B,1,0},
    {'-',0x0C,0,0}, {'_',0x0C,1,0},
    {'=',0x0D,0,0}, {'+',0x0D,1,0},
    {'[',0x1A,0,0}, {'{',0x1A,1,0},
    {']',0x1B,0,0}, {'}',0x1B,1,0},
    {';',0x27,0,0}, {':',0x27,1,0},
    {'\'',0x28,0,0},{'"',0x28,1,0},
    {'`',0x29,0,0}, {'~',0x29,1,0},
    {'\\',0x2B,0,0},{'|',0x2B,1,0},
    {',',0x33,0,0}, {'<',0x33,1,0},
    {'.',0x34,0,0}, {'>',0x34,1,0},
    {'/',0x35,0,0}, {'?',0x35,1,0},
    {' ',0x39,0,0},

    /* editing / control */
    {0xFF08,0x0E,0,0},   /* BackSpace */
    {0xFF09,0x0F,0,0},   /* Tab       */
    {0xFF0D,0x1C,0,0},   /* Return    */
    {0xFF1B,0x01,0,0},   /* Escape    */
    {0xFF63,0x52,0,1},   /* Insert    */
    {0xFFFF,0x53,0,1},   /* Delete    */
    {0xFF50,0x47,0,1},   /* Home      */
    {0xFF57,0x4F,0,1},   /* End       */
    {0xFF55,0x49,0,1},   /* Page_Up   */
    {0xFF56,0x51,0,1},   /* Page_Down */
    {0xFF51,0x4B,0,1},   /* Left      */
    {0xFF52,0x48,0,1},   /* Up        */
    {0xFF53,0x4D,0,1},   /* Right     */
    {0xFF54,0x50,0,1},   /* Down      */

    /* function keys */
    {0xFFBE,0x3B,0,0}, {0xFFBF,0x3C,0,0}, {0xFFC0,0x3D,0,0},
    {0xFFC1,0x3E,0,0}, {0xFFC2,0x3F,0,0}, {0xFFC3,0x40,0,0},
    {0xFFC4,0x41,0,0}, {0xFFC5,0x42,0,0}, {0xFFC6,0x43,0,0},
    {0xFFC7,0x44,0,0}, {0xFFC8,0x57,0,0}, {0xFFC9,0x58,0,0},

    /* modifiers and locks */
    {0xFFE1,0x2A,0,0},   /* Shift_L   */
    {0xFFE2,0x36,0,0},   /* Shift_R   */
    {0xFFE3,0x1D,0,0},   /* Control_L */
    {0xFFE4,0x1D,0,1},   /* Control_R */
    {0xFFE9,0x38,0,0},   /* Alt_L     */
    {0xFFEA,0x38,0,1},   /* Alt_R     */
    {0xFFEB,0x5B,0,1},   /* Super_L   */
    {0xFFEC,0x5C,0,1},   /* Super_R   */
    {0xFFE5,0x3A,0,0},   /* Caps_Lock */
    {0xFF7F,0x45,0,0},   /* Num_Lock  */
    {0xFF14,0x46,0,0},   /* Scroll_Lock */

    /* keypad */
    {0xFF8D,0x1C,0,1},   /* KP_Enter    */
    {0xFFAB,0x4E,0,0},   /* KP_Add      */
    {0xFFAD,0x4A,0,0},   /* KP_Subtract */
    {0xFFAA,0x37,0,0},   /* KP_Multiply */
    {0xFFAF,0x35,0,1},   /* KP_Divide   */
    {0xFFAC,0x53,0,0},   /* KP_Decimal  */
    {0xFFB0,0x52,0,0}, {0xFFB1,0x4F,0,0}, {0xFFB2,0x50,0,0},
    {0xFFB3,0x51,0,0}, {0xFFB4,0x4B,0,0}, {0xFFB5,0x4C,0,0},
    {0xFFB6,0x4D,0,0}, {0xFFB7,0x47,0,0}, {0xFFB8,0x48,0,0},
    {0xFFB9,0x49,0,0},
};

static bool vnc_map_key(uint32_t sym, uint8_t *sc, bool *shift, bool *ext) {
    if (sym >= 'a' && sym <= 'z') {
        *sc = SC_LETTER[sym - 'a']; *shift = false; *ext = false;
        return true;
    }
    if (sym >= 'A' && sym <= 'Z') {
        *sc = SC_LETTER[sym - 'A']; *shift = true;  *ext = false;
        return true;
    }
    for (size_t i = 0; i < sizeof(SYMTAB) / sizeof(SYMTAB[0]); i++) {
        if (SYMTAB[i].sym == sym) {
            *sc    = SYMTAB[i].sc;
            *shift = SYMTAB[i].shift != 0;
            *ext   = SYMTAB[i].ext != 0;
            return true;
        }
    }
    return false;
}

static bool vnc_sym_is_modifier(uint32_t sym) {
    return sym == 0xFFE1 || sym == 0xFFE2 ||   /* Shift  */
           sym == 0xFFE3 || sym == 0xFFE4 ||   /* Ctrl   */
           sym == 0xFFE9 || sym == 0xFFEA ||   /* Alt    */
           sym == 0xFFEB || sym == 0xFFEC;     /* Super  */
}

static void vnc_inject_sc(uint8_t sc, bool ext, bool down) {
    if (ext) kbd_ps2_handle_scancode(0xE0);
    kbd_ps2_handle_scancode(down ? sc : (uint8_t)(sc | 0x80));
}

static void vnc_key(uint32_t sym, bool down) {
    uint8_t sc; bool need_shift, ext;
    if (!vnc_map_key(sym, &sc, &need_shift, &ext)) return;

    g_stats.key_events++;
#ifdef VNC_BOOT
    /* Gate evidence: prove a remote key reached the driver path, and with
     * which scancode. Bounded so a long session cannot flood the log. */
    if (g_stats.key_events <= 12)
        kprintf("[vnc] key sym=0x%x down=%d sc=0x%02x ext=%d shift=%d\n",
                (unsigned)sym, (int)down, (unsigned)sc, (int)ext,
                (int)need_shift);
#endif

    if (vnc_sym_is_modifier(sym)) {
        vnc_inject_sc(sc, ext, down);
        return;
    }

    /*
     * Shift synthesis. The character a key produces is decided at MAKE time,
     * so we only have to line shift up around the press. Clients differ:
     * some send Shift_L plus the shifted keysym, some send the shifted
     * keysym alone. Reconciling the driver's actual shift state against
     * what this keysym needs handles both, and leaves the state as we
     * found it.
     */
    bool fix = false, had = false;
    if (down) {
        had = kbd_shift_state();
        fix = (had != need_shift);
        if (fix) vnc_inject_sc(0x2A, false, need_shift);
    }

    vnc_inject_sc(sc, ext, down);

    if (fix) vnc_inject_sc(0x2A, false, had);
}

/* ---- pointer -------------------------------------------------------- */

static void vnc_pointer(uint8_t mask, int x, int y) {
    uint8_t buttons = 0;
    if (mask & 0x01) buttons |= MOUSE_BTN_LEFT;
    if (mask & 0x02) buttons |= MOUSE_BTN_MIDDLE;
    if (mask & 0x04) buttons |= MOUSE_BTN_RIGHT;

    /* Bits 3/4 are wheel up/down. They arrive as a momentary press, so a
     * detent is one event, not a held state. */
    int dz = 0;
    if (mask & 0x08) dz = 1;
    else if (mask & 0x10) dz = -1;

    g_stats.pointer_events++;
#ifdef VNC_BOOT
    if (g_stats.pointer_events <= 12)
        kprintf("[vnc] ptr (%d,%d) mask=0x%02x buttons=0x%02x dz=%d\n",
                x, y, (unsigned)mask, (unsigned)buttons, dz);
#endif
    gui_pointer_warp(x, y, dz, buttons);
}

/* ---- clipboard: guest -> viewer ------------------------------------- */

/*
 * RFB has no "clipboard changed" event to hook, so this samples the
 * kernel clipboard a couple of times a second and sends ServerCutText
 * when the contents change.
 *
 * The g_upd_active guard is load-bearing: a framebuffer update is
 * streamed into the TX window across many polls, and splicing another
 * message into the middle of its pixel payload would have the client
 * decode our clipboard text as pixels.
 */
static void vnc_push_clipboard(void) {
    if (g_state != VS_RUN || g_upd_active) return;

    uint64_t now = pit_ticks();
    if (g_clip_next && now < g_clip_next) return;
    g_clip_next = now + (uint64_t)pit_hz() / 2u;

    static char buf[2048];
    long n = clipboard_get(buf, sizeof(buf), ABI_CLIP_FMT_TEXT);
    if (n <= 0) return;

    uint32_t sig = (uint32_t)n;
    for (long i = 0; i < n; i++) sig = sig * 31u + (uint8_t)buf[i];
    if (sig == g_clip_sig) return;

    uint8_t hdr[8];
    hdr[0] = 3;                 /* ServerCutText */
    hdr[1] = hdr[2] = hdr[3] = 0;
    wr32(hdr + 4, (uint32_t)n);
    if (tx_space() < sizeof(hdr) + (size_t)n) return;   /* retry later */
    (void)tx_put(hdr, sizeof(hdr));
    (void)tx_put(buf, (size_t)n);
    g_clip_sig = sig;
}

/* ---- framebuffer updates -------------------------------------------- */

static void rect_clip(int *x, int *y, int *w, int *h) {
    int W = (int)gfx_width(), H = (int)gfx_height();
    /* Never exceed the geometry the client was TOLD about. A viewer that
     * did not negotiate DesktopSize still holds a framebuffer of the size
     * ServerInit announced, and a rect past its edge is a decode error on
     * its side; conversely, a screen that shrank would have us read past
     * the back buffer. The smaller of the two is safe in both directions. */
    if ((int)g_scr_w > 0 && (int)g_scr_w < W) W = (int)g_scr_w;
    if ((int)g_scr_h > 0 && (int)g_scr_h < H) H = (int)g_scr_h;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > W) *w = W - *x;
    if (*y + *h > H) *h = H - *y;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

static void dmg_absorb(void) {
    int x, y, w, h;
    if (!gfx_remote_damage_take(&x, &y, &w, &h)) return;
    if (!g_dmg_valid) {
        g_dmg_x = x; g_dmg_y = y; g_dmg_w = w; g_dmg_h = h;
        g_dmg_valid = true;
        return;
    }
    int x0 = g_dmg_x < x ? g_dmg_x : x;
    int y0 = g_dmg_y < y ? g_dmg_y : y;
    int x1 = (g_dmg_x + g_dmg_w) > (x + w) ? (g_dmg_x + g_dmg_w) : (x + w);
    int y1 = (g_dmg_y + g_dmg_h) > (y + h) ? (g_dmg_y + g_dmg_h) : (y + h);
    g_dmg_x = x0; g_dmg_y = y0; g_dmg_w = x1 - x0; g_dmg_h = y1 - y0;
}

/* Emit the FramebufferUpdate + rect header for the region being started. */
static bool vnc_begin_update(int x, int y, int w, int h, bool hextile) {
    uint8_t hdr[16];
    hdr[0] = 0;              /* FramebufferUpdate */
    hdr[1] = 0;              /* padding           */
    wr16(hdr + 2, 1);        /* one rectangle     */
    wr16(hdr + 4, (uint16_t)x);
    wr16(hdr + 6, (uint16_t)y);
    wr16(hdr + 8, (uint16_t)w);
    wr16(hdr + 10, (uint16_t)h);
    wr32(hdr + 12, hextile ? (uint32_t)ENC_HEXTILE : (uint32_t)ENC_RAW);
    if (!tx_put(hdr, sizeof(hdr))) return false;

    g_upd_active  = true;
    g_upd_hextile = hextile;
    g_upd_x = x; g_upd_y = y; g_upd_w = w; g_upd_h = h;
    g_upd_row = 0; g_upd_col = 0;
    g_hex_bg_valid = false;
    g_stats.rects_sent++;
    return true;
}

/* Tell the client the desktop resized, using the DesktopSize pseudo-encoding. */
static bool vnc_send_desktop_size(void) {
    uint8_t hdr[16];
    hdr[0] = 0;
    hdr[1] = 0;
    wr16(hdr + 2, 1);
    wr16(hdr + 4, 0);
    wr16(hdr + 6, 0);
    wr16(hdr + 8, (uint16_t)gfx_width());
    wr16(hdr + 10, (uint16_t)gfx_height());
    wr32(hdr + 12, (uint32_t)ENC_DESKTOPSIZE);
    if (!tx_put(hdr, sizeof(hdr))) return false;
    g_scr_w = gfx_width();
    g_scr_h = gfx_height();
    return true;
}

/* One raw row. Returns false when the TX window is full. */
static bool vnc_step_raw(const uint32_t *fb, uint32_t stride) {
    size_t bypp = vnc_bypp();
    size_t rowbytes = (size_t)g_upd_w * bypp;
    if (tx_space() < rowbytes) return false;

    uint8_t *dst = g_tx + g_tx_tail;
    const uint32_t *src = fb + (uint32_t)(g_upd_y + g_upd_row) * stride + g_upd_x;
    for (int i = 0; i < g_upd_w; i++) {
        vnc_put_pixel(dst, vnc_pack(src[i]));
        dst += bypp;
    }
    g_tx_tail += rowbytes;
    g_upd_row++;
    return true;
}

/*
 * One hextile tile.
 *
 * Two subencodings are used: a solid tile costs a few bytes (or ZERO, when
 * it repeats the previous tile's background), and anything else falls back
 * to Raw. Desktop content is mostly flat fills, so solid-tile detection is
 * where nearly all of the saving is; sub-rectangle decomposition would add
 * more, and can be layered on later without changing the wire contract.
 */
static bool vnc_step_hextile(const uint32_t *fb, uint32_t stride) {
    int tx0 = g_upd_x + g_upd_col * VNC_TILE;
    int ty0 = g_upd_y + g_upd_row * VNC_TILE;
    int tw = g_upd_x + g_upd_w - tx0;
    int th = g_upd_y + g_upd_h - ty0;
    if (tw > VNC_TILE) tw = VNC_TILE;
    if (th > VNC_TILE) th = VNC_TILE;

    size_t bypp = vnc_bypp();
    /* Worst case: subencoding byte + a full raw tile. */
    if (tx_space() < 1u + (size_t)tw * (size_t)th * bypp + bypp) return false;

    const uint32_t *base = fb + (uint32_t)ty0 * stride + tx0;
    uint32_t first = base[0];
    bool solid = true;
    for (int yy = 0; yy < th && solid; yy++) {
        const uint32_t *row = base + (uint32_t)yy * stride;
        for (int xx = 0; xx < tw; xx++) {
            if (row[xx] != first) { solid = false; break; }
        }
    }

    if (solid) {
        if (g_hex_bg_valid && g_hex_bg == first) {
            uint8_t sub = 0;          /* identical to the previous tile */
            (void)tx_put(&sub, 1);
        } else {
            uint8_t buf[1 + 4];
            buf[0] = HEX_BG;
            vnc_put_pixel(buf + 1, vnc_pack(first));
            (void)tx_put(buf, 1 + bypp);
            g_hex_bg = first;
            g_hex_bg_valid = true;
        }
        g_stats.solid_tiles++;
    } else {
        uint8_t sub = HEX_RAW;
        (void)tx_put(&sub, 1);
        uint8_t *dst = g_tx + g_tx_tail;
        for (int yy = 0; yy < th; yy++) {
            const uint32_t *row = base + (uint32_t)yy * stride;
            for (int xx = 0; xx < tw; xx++) {
                vnc_put_pixel(dst, vnc_pack(row[xx]));
                dst += bypp;
            }
        }
        g_tx_tail += (size_t)tw * (size_t)th * bypp;
        /* A raw tile leaves the running background undefined. */
        g_hex_bg_valid = false;
        g_stats.raw_tiles++;
    }

    /* Advance in tile-raster order. */
    int tiles_across = (g_upd_w + VNC_TILE - 1) / VNC_TILE;
    if (++g_upd_col >= tiles_across) {
        g_upd_col = 0;
        g_upd_row++;
    }
    return true;
}

static void vnc_update_step(void) {
    if (!g_upd_active) return;

    const uint32_t *fb = gfx_backbuf();
    if (!fb) { g_upd_active = false; return; }
    uint32_t stride = gfx_width();   /* the back buffer is tightly packed */

    if (g_upd_hextile) {
        int tiles_down = (g_upd_h + VNC_TILE - 1) / VNC_TILE;
        while (g_upd_row < tiles_down) {
            if (!vnc_step_hextile(fb, stride)) return;   /* window full */
        }
    } else {
        while (g_upd_row < g_upd_h) {
            if (!vnc_step_raw(fb, stride)) return;
        }
    }
    g_upd_active = false;
}

/* Start an update if the client wants one and there is something to send. */
static void vnc_maybe_start_update(void) {
    if (g_upd_active || !g_req_pending) return;
    if (!gfx_ready()) return;

    /* A resize has to reach the client before any pixels do, or it will
     * decode the new geometry into its old buffer. */
    if (g_enc_desktopsize &&
        (gfx_width() != g_scr_w || gfx_height() != g_scr_h)) {
        if (!vnc_send_desktop_size()) return;
        gfx_remote_damage_all();      /* everything must be redrawn */
    }

    dmg_absorb();

    int x = g_req_x, y = g_req_y, w = g_req_w, h = g_req_h;
    if (g_req_incremental) {
        if (!g_dmg_valid) return;     /* nothing changed; hold the request */
        /* Intersect the damage with what the client asked about. */
        int x0 = x > g_dmg_x ? x : g_dmg_x;
        int y0 = y > g_dmg_y ? y : g_dmg_y;
        int x1 = (x + w) < (g_dmg_x + g_dmg_w) ? (x + w) : (g_dmg_x + g_dmg_w);
        int y1 = (y + h) < (g_dmg_y + g_dmg_h) ? (y + h) : (g_dmg_y + g_dmg_h);
        x = x0; y = y0; w = x1 - x0; h = y1 - y0;
        if (w <= 0 || h <= 0) return;
    }

    rect_clip(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) { g_req_pending = false; return; }

    /* Raw steps a row at a time, so a row must fit the TX window. */
    if (!g_enc_hextile && (size_t)w * vnc_bypp() > VNC_TX_MAX) {
        kprintf("[vnc] row of %d px exceeds the %u-byte TX window\n",
                w, (unsigned)VNC_TX_MAX);
        vnc_close_client("row too wide");
        return;
    }

    if (!vnc_begin_update(x, y, w, h, g_enc_hextile)) return;
    g_req_pending = false;
    g_dmg_valid = false;
}

/* ---- handshake ------------------------------------------------------ */

static void vnc_send_challenge(void) {
    rng_fill(g_challenge, sizeof(g_challenge));
    (void)tx_put(g_challenge, sizeof(g_challenge));
    g_state = VS_AUTH;
}

static bool vnc_check_auth(const uint8_t response[16]) {
    struct des_ctx ctx;
    uint8_t expect[16];
    des_set_key_vnc(&ctx, g_passwd);
    des_encrypt_block(&ctx, g_challenge, expect);
    des_encrypt_block(&ctx, g_challenge + 8, expect + 8);

    /* Constant-time compare: no early exit on the first differing byte. */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(expect[i] ^ response[i]);
    memset(expect, 0, sizeof(expect));
    return diff == 0;
}

static void vnc_send_security_result(bool ok) {
    uint8_t buf[4];
    wr32(buf, ok ? 0u : 1u);
    (void)tx_put(buf, sizeof(buf));
    if (!ok) {
        /* RFB 3.8 carries a reason string on failure. */
        static const char reason[] = "Authentication failed";
        uint8_t len[4];
        wr32(len, (uint32_t)(sizeof(reason) - 1));
        (void)tx_put(len, sizeof(len));
        (void)tx_put(reason, sizeof(reason) - 1);
    }
}

static void vnc_send_server_init(void) {
    static const char name[] = "tobyOS";
    uint8_t buf[24];
    g_scr_w = gfx_width();
    g_scr_h = gfx_height();
    wr16(buf + 0, (uint16_t)g_scr_w);
    wr16(buf + 2, (uint16_t)g_scr_h);
    /* Our preferred pixel format -- the back buffer's own layout. */
    buf[4] = 32;      /* bits-per-pixel */
    buf[5] = 24;      /* depth          */
    buf[6] = 0;       /* big-endian     */
    buf[7] = 1;       /* true-colour    */
    wr16(buf + 8, 255); wr16(buf + 10, 255); wr16(buf + 12, 255);
    buf[14] = 16; buf[15] = 8; buf[16] = 0;
    buf[17] = buf[18] = buf[19] = 0;   /* padding */
    wr32(buf + 20, (uint32_t)(sizeof(name) - 1));
    (void)tx_put(buf, sizeof(buf));
    (void)tx_put(name, sizeof(name) - 1);
}

/* ---- client message dispatch ---------------------------------------- */

/* Returns bytes consumed, or 0 when the message is not fully buffered. */
static size_t vnc_handle_message(const uint8_t *p, size_t n) {
    switch (p[0]) {
    case 0: {   /* SetPixelFormat */
        if (n < 20) return 0;
        const uint8_t *f = p + 4;
        g_fmt.bpp         = f[0];
        g_fmt.depth       = f[1];
        g_fmt.big_endian  = f[2];
        g_fmt.true_colour = f[3];
        g_fmt.rmax = rd16(f + 4);
        g_fmt.gmax = rd16(f + 6);
        g_fmt.bmax = rd16(f + 8);
        g_fmt.rsh = f[10]; g_fmt.gsh = f[11]; g_fmt.bsh = f[12];
        if (g_fmt.bpp != 8 && g_fmt.bpp != 16 && g_fmt.bpp != 32) {
            kprintf("[vnc] client asked for %u bpp; only 8/16/32 are "
                    "supported\n", (unsigned)g_fmt.bpp);
            vnc_close_client("bad pixel format");
            return 0;
        }
        if (!g_fmt.true_colour) {
            /* We never send SetColourMapEntries, so a palette client would
             * decode garbage. Say so instead of pretending. */
            kprintf("[vnc] client asked for a colour map; not supported\n");
            vnc_close_client("colour map unsupported");
            return 0;
        }
        /* The format changed under any in-flight rect: restart cleanly. */
        g_upd_active = false;
        gfx_remote_damage_all();
        return 20;
    }
    case 1: {   /* FixColourMapEntries -- we never ask for one */
        if (n < 6) return 0;
        uint16_t ncolours = rd16(p + 4);
        size_t need = 6u + (size_t)ncolours * 6u;
        if (n < need) return 0;
        return need;
    }
    case 2: {   /* SetEncodings */
        if (n < 4) return 0;
        uint16_t count = rd16(p + 2);
        if (count > 512) { vnc_close_client("absurd encoding count"); return 0; }
        size_t need = 4u + (size_t)count * 4u;
        if (n < need) return 0;
        g_enc_hextile = false;
        g_enc_desktopsize = false;
        for (uint16_t i = 0; i < count; i++) {
            int32_t enc = (int32_t)rd32(p + 4 + (size_t)i * 4u);
            if (enc == ENC_HEXTILE)          g_enc_hextile = true;
            else if (enc == ENC_DESKTOPSIZE) g_enc_desktopsize = true;
        }
        kprintf("[vnc] encodings: hextile=%d desktopsize=%d\n",
                (int)g_enc_hextile, (int)g_enc_desktopsize);
        return need;
    }
    case 3: {   /* FramebufferUpdateRequest */
        if (n < 10) return 0;
        g_req_incremental = (p[1] != 0);
        g_req_x = (int)rd16(p + 2);
        g_req_y = (int)rd16(p + 4);
        g_req_w = (int)rd16(p + 6);
        g_req_h = (int)rd16(p + 8);
        g_req_pending = true;
        if (!g_req_incremental) gfx_remote_damage_all();
        return 10;
    }
    case 4: {   /* KeyEvent */
        if (n < 8) return 0;
        vnc_key(rd32(p + 4), p[1] != 0);
        return 8;
    }
    case 5: {   /* PointerEvent */
        if (n < 6) return 0;
        vnc_pointer(p[1], (int)rd16(p + 2), (int)rd16(p + 4));
        return 6;
    }
    case 6: {   /* ClientCutText -- remote clipboard into ours */
        if (n < 8) return 0;
        uint32_t len = rd32(p + 4);
        if (len > VNC_RX_MAX - 8u) {
            /* Too big to buffer; consume the header and drop the body as
             * it arrives rather than wedging the stream. */
            g_discard = len;
            return 8;
        }
        if (n < 8u + len) return 0;
        if (len) clipboard_set((const char *)(p + 8), len, ABI_CLIP_FMT_TEXT);
        return 8u + len;
    }
    default:
        kprintf("[vnc] unknown client message type %u\n", (unsigned)p[0]);
        vnc_close_client("bad message");
        return 0;
    }
}

static void vnc_run_messages(void) {
    for (;;) {
        if (g_discard) {
            size_t drop = g_rx_len < g_discard ? g_rx_len : g_discard;
            if (!drop) return;
            memmove(g_rx, g_rx + drop, g_rx_len - drop);
            g_rx_len -= drop;
            g_discard -= (uint32_t)drop;
            continue;
        }
        if (g_rx_len == 0) return;
        size_t used = vnc_handle_message(g_rx, g_rx_len);
        if (used == 0) return;                  /* need more, or closed */
        if (!g_cli) return;
        memmove(g_rx, g_rx + used, g_rx_len - used);
        g_rx_len -= used;
    }
}

/* ---- handshake progression ------------------------------------------ */

static void vnc_handshake(void) {
    switch (g_state) {
    case VS_VERSION: {
        if (g_rx_len < 12) return;
        /* We only speak 3.8, and said so; accept whatever the client
         * echoes rather than refusing a 3.3/3.7 client that will still
         * follow the 3.8 flow we drive. */
        memmove(g_rx, g_rx + 12, g_rx_len - 12);
        g_rx_len -= 12;

        uint8_t sec[2] = { 1, 2 };   /* one type: VNC Authentication */
        (void)tx_put(sec, sizeof(sec));
        g_state = VS_SECURITY;
        return;
    }
    case VS_SECURITY: {
        if (g_rx_len < 1) return;
        uint8_t choice = g_rx[0];
        memmove(g_rx, g_rx + 1, g_rx_len - 1);
        g_rx_len -= 1;
        if (choice != 2) {
            kprintf("[vnc] client chose security type %u; only 2 is "
                    "offered\n", (unsigned)choice);
            vnc_send_security_result(false);
            tx_flush();
            vnc_close_client("bad security type");
            return;
        }
        vnc_send_challenge();
        return;
    }
    case VS_AUTH: {
        if (g_rx_len < 16) return;
        bool ok = vnc_check_auth(g_rx);
        memmove(g_rx, g_rx + 16, g_rx_len - 16);
        g_rx_len -= 16;

        vnc_send_security_result(ok);
        if (!ok) {
            g_stats.auth_fail++;
            g_fail_streak++;
            kprintf("[vnc] authentication FAILED (%u in a row)\n",
                    (unsigned)g_fail_streak);
            if (g_fail_streak >= VNC_MAX_FAILS) {
                /* Rate-limit guessing. A single viewer slot already
                 * serialises attempts; this puts a hard floor under the
                 * rate at which a password can be brute-forced. */
                g_cooloff_until = pit_ticks() +
                                  (uint64_t)pit_hz() * VNC_COOLOFF_SEC;
                kprintf("[vnc] too many failures; refusing connections "
                        "for %us\n", (unsigned)VNC_COOLOFF_SEC);
            }
            tx_flush();
            vnc_close_client("auth failed");
            return;
        }
        g_stats.auth_ok++;
        g_fail_streak = 0;
        kprintf("[vnc] authenticated\n");
        g_state = VS_INIT;
        return;
    }
    case VS_INIT: {
        if (g_rx_len < 1) return;
        /* ClientInit's shared-flag: we only ever have one viewer, so
         * there is nothing to share or evict either way. */
        memmove(g_rx, g_rx + 1, g_rx_len - 1);
        g_rx_len -= 1;
        vnc_send_server_init();
        gfx_remote_damage_all();
        g_state = VS_RUN;
        kprintf("[vnc] session up: %ux%u\n",
                (unsigned)g_scr_w, (unsigned)g_scr_h);
        return;
    }
    default:
        return;
    }
}

/* ---- public entry points -------------------------------------------- */

void vnc_init(void) {
    if (g_lsn) return;
    if (!net_is_up()) return;

    /* Called from every net_service_tick, so that running `vncpasswd`
     * brings the server up without a reboot. Throttled: a machine with no
     * password configured must not hit the filesystem on every tick, and
     * the complaints below must not fill the log. */
    static uint64_t next_try;
    static bool     told_nopass, told_norng, told_des;
    uint64_t now = pit_ticks();
    if (next_try && now < next_try) return;
    next_try = now + (uint64_t)pit_hz() * VNC_RETRY_SEC;

    if (!des_selftest()) {
        if (!told_des) {
            kprintf("[vnc] DES self-test FAILED; refusing to start\n");
            told_des = true;
        }
        return;
    }
#ifdef VNC_BOOT
    /* Gate-only (logs/vnc.sh): seed a known password so the harness can
     * authenticate. Opt-in via -DVNC_BOOT; never in a normal build, which
     * is why the server is otherwise inert until someone runs vncpasswd. */
    {
        static bool seeded;
        if (!seeded && !g_have_passwd) {
            seeded = true;
            int rc = vnc_set_password("tobyvnc");
            if (rc == 0)
                kprintf("[vnc] VNC_BOOT: seeded gate password\n");
            else
                kprintf("[vnc] VNC_BOOT: could not write %s (rc=%d)\n",
                        VNC_PASSWD_PATH, rc);
        }
    }
#endif

    if (!g_have_passwd && !vnc_load_password()) {
        if (!told_nopass) {
            kprintf("[vnc] no %s -- not listening. Set one with "
                    "`vncpasswd`.\n", VNC_PASSWD_PATH);
            told_nopass = true;
        }
        return;
    }
    if (!rng_ready()) {
        /* A predictable challenge would make the DES response replayable. */
        if (!told_norng) {
            kprintf("[vnc] RNG not ready; refusing to start\n");
            told_norng = true;
        }
        return;
    }

    g_lsn = tcp_listen(htons((uint16_t)VNC_PORT), 2);
    if (!g_lsn) {
        kprintf("[vnc] listen TCP/%u failed (busy or no slot)\n",
                (unsigned)VNC_PORT);
        return;
    }
    vnc_reset_session();
    kprintf("[vnc] listening on TCP port %u (RFB 3.8, VNC auth)\n",
            (unsigned)VNC_PORT);
}

void vnc_poll(void) {
    if (!g_lsn || !net_is_up()) return;

    if (!g_cli) {
        if (g_cooloff_until) {
            if (pit_ticks() < g_cooloff_until) {
                /* Leave pending connections queued in the listener; they
                 * get served once the cool-off expires. */
                return;
            }
            g_cooloff_until = 0;
            g_fail_streak = 0;
        }
        g_cli = tcp_accept(g_lsn, 0);
        if (!g_cli) return;

        vnc_reset_session();
        g_stats.connects++;
        kprintf("[vnc] client connected\n");

        static const char ver[] = "RFB 003.008\n";
        (void)tx_put(ver, sizeof(ver) - 1);
        g_state = VS_VERSION;
        tx_flush();
        return;
    }

    /* Read whatever is waiting, without blocking. */
    if (g_rx_len < VNC_RX_MAX) {
        long n = tcp_recv(g_cli, g_rx + g_rx_len, VNC_RX_MAX - g_rx_len, 0);
        if (n < 0) { vnc_close_client("peer closed"); return; }
        if (n > 0) g_rx_len += (size_t)n;
    }

    if (g_state == VS_RUN) vnc_run_messages();
    else                   vnc_handshake();
    if (!g_cli) return;

    vnc_push_clipboard();
    vnc_maybe_start_update();
    vnc_update_step();
    tx_flush();
}

bool vnc_listening(void) { return g_lsn != 0; }
bool vnc_client_connected(void) { return g_cli != 0 && g_state == VS_RUN; }

void vnc_get_stats(struct vnc_stats *out) {
    if (out) *out = g_stats;
}
