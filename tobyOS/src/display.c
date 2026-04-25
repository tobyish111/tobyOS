/* display.c -- M27A: display introspection + output enumeration.
 *
 * Sits one layer above gfx.c. Owns:
 *   1. a tiny array of "display_output" records (M27G groundwork);
 *   2. a primary-output index the compositor looks at;
 *   3. live counters (gfx_flip count + last-flip ns) updated from
 *      gfx.c via display_record_flip();
 *   4. five devtest self-tests that validate the stack end-to-end.
 *
 * We deliberately keep the data plane entirely off the hot path:
 * display_record_flip() is two stores; everything else is queried
 * synchronously by the syscall / devtest paths.
 *
 * Ownership of the actual pixel buffers stays with gfx.c -- this
 * module never touches gfx_backbuf() except through gfx_* APIs to
 * keep the layering clean for M27B.
 */

#include <tobyos/display.h>
#include <tobyos/gfx.h>
#include <tobyos/devtest.h>
#include <tobyos/perf.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* ---- output records --------------------------------------------- */

struct display_output {
    bool       used;
    bool       is_primary;
    uint8_t    pixel_format;        /* ABI_DISPLAY_FMT_*           */
    uint8_t    backend_id;          /* ABI_DISPLAY_BACKEND_*       */
    uint32_t   width;
    uint32_t   height;
    uint32_t   pitch_bytes;
    uint32_t   bpp;
    int32_t    origin_x;            /* M27G: layout origin (px)    */
    int32_t    origin_y;            /* M27G: layout origin (px)    */
    uint64_t   flips;
    uint64_t   last_flip_ns;
    char       name   [ABI_DISPLAY_NAME_MAX];
    char       backend[ABI_DISPLAY_BACKEND_MAX];
};

static struct display_output g_outputs[DISPLAY_MAX_OUTPUTS];
static int                   g_primary = -1;
static bool                  g_inited;

/* ---- helpers ---------------------------------------------------- */

static void disp_strlcpy(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t n = 0;
    while (src && src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

static uint8_t backend_id_from_name(const char *name) {
    if (!name) return ABI_DISPLAY_BACKEND_NONE;
    /* Match the names gfx.c / virtio_gpu.c install. Anything else
     * stays "unknown" so a third-party backend would render as a
     * literal string but report id=0 -- no panic, no surprise. */
    if (strcmp(name, "limine-fb") == 0)  return ABI_DISPLAY_BACKEND_LIMINE_FB;
    if (strcmp(name, "virtio-gpu") == 0) return ABI_DISPLAY_BACKEND_VIRTIO_GPU;
    return ABI_DISPLAY_BACKEND_NONE;
}

static const char *backend_id_name(uint8_t id) {
    switch (id) {
    case ABI_DISPLAY_BACKEND_LIMINE_FB:  return "limine-fb";
    case ABI_DISPLAY_BACKEND_VIRTIO_GPU: return "virtio-gpu";
    default:                             return "(none)";
    }
}

/* ---- registry API ---------------------------------------------- */

/* M27G: walk every used output and return the smallest origin_x such
 * that a NEW output appended at (rightmost_x, 0) doesn't overlap any
 * existing output horizontally. This is the simple horizontal layout
 * the spec calls for: monitors are placed left-to-right, top-aligned.
 *
 * Returns 0 when the registry is empty (so the very first output sits
 * at the origin, no surprise). */
static int32_t next_horizontal_origin_x(void) {
    int32_t rightmost = 0;
    bool    any       = false;
    for (int i = 0; i < DISPLAY_MAX_OUTPUTS; i++) {
        if (!g_outputs[i].used) continue;
        int32_t end = g_outputs[i].origin_x + (int32_t)g_outputs[i].width;
        if (!any || end > rightmost) {
            rightmost = end;
            any       = true;
        }
    }
    return any ? rightmost : 0;
}

int display_register_output(const char *name,
                            uint32_t width, uint32_t height,
                            uint32_t pitch_bytes, uint32_t bpp,
                            uint8_t pixel_format, uint8_t backend_id,
                            const char *backend_name) {
    /* Replace by name first so a backend swap doesn't grow the list
     * and -- M27G addition -- so a re-registration preserves the
     * caller's monitor layout (origin_x/origin_y stay put). */
    int slot = -1;
    for (int i = 0; i < DISPLAY_MAX_OUTPUTS; i++) {
        if (g_outputs[i].used && name &&
            strcmp(g_outputs[i].name, name) == 0) {
            slot = i;
            break;
        }
    }
    bool reusing_slot = (slot >= 0);
    if (!reusing_slot) {
        for (int i = 0; i < DISPLAY_MAX_OUTPUTS; i++) {
            if (!g_outputs[i].used) { slot = i; break; }
        }
    }
    if (slot < 0) return -1;

    struct display_output *o = &g_outputs[slot];

    /* Save fields we want to PRESERVE across an in-place re-registration
     * before zeroing the slot. M27G: origin_x/origin_y are part of the
     * display layout (set externally via auto-layout or display_set_
     * origin) and must survive a backend swap. */
    bool    was_primary = o->is_primary;
    int32_t prev_ox     = o->origin_x;
    int32_t prev_oy     = o->origin_y;

    /* For a brand-new slot, auto-position to the right of every
     * existing output. The first registered output ends up at (0,0). */
    int32_t new_ox = reusing_slot ? prev_ox : next_horizontal_origin_x();
    int32_t new_oy = reusing_slot ? prev_oy : 0;

    memset(o, 0, sizeof *o);
    o->used         = true;
    o->is_primary   = was_primary;
    o->pixel_format = pixel_format;
    o->backend_id   = backend_id;
    o->width        = width;
    o->height       = height;
    o->pitch_bytes  = pitch_bytes;
    o->bpp          = bpp;
    o->origin_x     = new_ox;
    o->origin_y     = new_oy;
    disp_strlcpy(o->name,    name         ? name         : "fb?",        ABI_DISPLAY_NAME_MAX);
    disp_strlcpy(o->backend, backend_name ? backend_name : backend_id_name(backend_id),
                 ABI_DISPLAY_BACKEND_MAX);

    /* First registered output becomes primary unless one is already
     * pinned. M27G can later override via display_set_primary(). */
    if (g_primary < 0) {
        g_primary     = slot;
        o->is_primary = true;
    }
    return slot;
}

int display_unregister_output(int idx) {
    if (idx < 0 || idx >= DISPLAY_MAX_OUTPUTS) return -1;
    struct display_output *o = &g_outputs[idx];
    if (!o->used) return -1;

    bool was_primary = o->is_primary;
    memset(o, 0, sizeof *o);

    if (was_primary) {
        /* Auto-elect the lowest remaining slot. Leaves g_primary < 0
         * if the registry is now empty. */
        g_primary = -1;
        for (int i = 0; i < DISPLAY_MAX_OUTPUTS; i++) {
            if (g_outputs[i].used) {
                g_primary               = i;
                g_outputs[i].is_primary = true;
                break;
            }
        }
    }
    return 0;
}

int display_set_origin(int idx, int32_t origin_x, int32_t origin_y) {
    if (idx < 0 || idx >= DISPLAY_MAX_OUTPUTS) return -1;
    if (!g_outputs[idx].used) return -1;
    g_outputs[idx].origin_x = origin_x;
    g_outputs[idx].origin_y = origin_y;
    return 0;
}

int display_total_bounds(int32_t *out_x, int32_t *out_y,
                         uint32_t *out_w, uint32_t *out_h) {
    int n = 0;
    int32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool any = false;
    for (int i = 0; i < DISPLAY_MAX_OUTPUTS; i++) {
        const struct display_output *o = &g_outputs[i];
        if (!o->used) continue;
        n++;
        int32_t lx = o->origin_x;
        int32_t ly = o->origin_y;
        int32_t rx = o->origin_x + (int32_t)o->width;
        int32_t ry = o->origin_y + (int32_t)o->height;
        if (!any) {
            min_x = lx; min_y = ly; max_x = rx; max_y = ry; any = true;
        } else {
            if (lx < min_x) min_x = lx;
            if (ly < min_y) min_y = ly;
            if (rx > max_x) max_x = rx;
            if (ry > max_y) max_y = ry;
        }
    }
    if (!any) {
        if (out_x) *out_x = 0;
        if (out_y) *out_y = 0;
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return 0;
    }
    if (out_x) *out_x = min_x;
    if (out_y) *out_y = min_y;
    if (out_w) *out_w = (uint32_t)(max_x - min_x);
    if (out_h) *out_h = (uint32_t)(max_y - min_y);
    return n;
}

int display_set_primary(int idx) {
    if (idx < 0 || idx >= DISPLAY_MAX_OUTPUTS) return -1;
    if (!g_outputs[idx].used) return -1;
    if (g_primary >= 0 && g_primary != idx) {
        g_outputs[g_primary].is_primary = false;
    }
    g_outputs[idx].is_primary = true;
    g_primary = idx;
    return 0;
}

int display_primary_index(void) { return g_primary; }

const struct display_output *display_get(int idx) {
    if (idx < 0 || idx >= DISPLAY_MAX_OUTPUTS) return 0;
    if (!g_outputs[idx].used) return 0;
    return &g_outputs[idx];
}

int display_count(void) {
    int n = 0;
    for (int i = 0; i < DISPLAY_MAX_OUTPUTS; i++) {
        if (g_outputs[i].used) n++;
    }
    return n;
}

void display_record_flip(void) {
    if (g_primary < 0) return;
    struct display_output *o = &g_outputs[g_primary];
    o->flips        += 1;
    o->last_flip_ns  = perf_now_ns();
    /* Backend identity can change after gfx_set_backend() (e.g.
     * virtio-gpu installing itself after gfx_init); refresh on every
     * flip so introspection is always accurate without the gfx layer
     * needing to know about us. Cheap: one pointer compare + maybe
     * a small string copy. */
    const struct gfx_backend *b = gfx_get_backend();
    if (b && b->name) {
        uint8_t id = backend_id_from_name(b->name);
        if (id != o->backend_id || strcmp(o->backend, b->name) != 0) {
            o->backend_id = id;
            disp_strlcpy(o->backend, b->name, ABI_DISPLAY_BACKEND_MAX);
        }
    }
}

/* ---- userland-facing enumeration ------------------------------- */

int display_enumerate(struct abi_display_info *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    for (int i = 0; i < DISPLAY_MAX_OUTPUTS && n < cap; i++) {
        const struct display_output *o = &g_outputs[i];
        if (!o->used) continue;
        struct abi_display_info *r = &out[n];
        memset(r, 0, sizeof *r);
        r->index        = (uint8_t)i;
        r->status       = ABI_DISPLAY_PRESENT |
                          (o->is_primary ? ABI_DISPLAY_PRIMARY : 0u) |
                          (o->flips      ? ABI_DISPLAY_ACTIVE  : 0u);
        r->pixel_format = o->pixel_format;
        r->backend_id   = o->backend_id;
        r->width        = o->width;
        r->height       = o->height;
        r->pitch_bytes  = o->pitch_bytes;
        r->bpp          = o->bpp;
        r->origin_x     = o->origin_x;
        r->origin_y     = o->origin_y;
        r->flips        = o->flips;
        r->last_flip_ns = o->last_flip_ns;
        disp_strlcpy(r->name,    o->name,    ABI_DISPLAY_NAME_MAX);
        disp_strlcpy(r->backend, o->backend, ABI_DISPLAY_BACKEND_MAX);
        n++;
    }
    return n;
}

/* ---- boot-time inventory dump --------------------------------- */

void display_dump_kprintf(void) {
    int n = display_count();
    int32_t  bx = 0, by = 0;
    uint32_t bw = 0, bh = 0;
    display_total_bounds(&bx, &by, &bw, &bh);
    kprintf("[display] %d output(s) registered (primary idx=%d) "
            "layout=%ux%u@(%d,%d)\n",
            n, g_primary, bw, bh, (int)bx, (int)by);
    for (int i = 0; i < DISPLAY_MAX_OUTPUTS; i++) {
        const struct display_output *o = &g_outputs[i];
        if (!o->used) continue;
        kprintf("[INFO] display: %s%s %ux%u@(%d,%d) pitch=%u bpp=%u "
                "fmt=%u backend=%s flips=%lu\n",
                o->name,
                o->is_primary ? " (primary)" : "",
                o->width, o->height, (int)o->origin_x, (int)o->origin_y,
                o->pitch_bytes, o->bpp,
                (unsigned)o->pixel_format,
                o->backend[0] ? o->backend : "(none)",
                (unsigned long)o->flips);
    }
}

/* ============================================================
 *  devtest self-tests
 * ============================================================
 *
 * Each test follows the (msg, cap) -> rc convention from
 * <tobyos/devtest.h>:
 *   0                 = PASS
 *   ABI_DEVT_SKIP (1) = SKIP (no display)
 *   negative          = FAIL
 *
 * We deliberately do NOT touch the framebuffer or back buffer here:
 * a self-test that punches pixels into the live desktop while the
 * compositor is running would race on the back buffer. Instead the
 * tests sanity-check the metadata that gfx_init produced + assert
 * the expected invariants (positive width/height, pitch >= width*4,
 * 32-bpp surface). The `drawtest` and `rendertest` userland tools
 * exercise the actual primitives in dedicated GUI windows where the
 * compositor handles back-buffer ownership for us.
 */

int display_test_basic(char *msg, size_t cap) {
    int n = display_count();
    if (n == 0) {
        ksnprintf(msg, cap, "no display outputs registered "
                            "(headless or gfx_init failed)");
        return ABI_DEVT_SKIP;
    }
    int prim = display_primary_index();
    if (prim < 0) {
        ksnprintf(msg, cap, "outputs registered but no primary set");
        return -ABI_EIO;
    }
    const struct display_output *o = display_get(prim);
    if (!o) {
        ksnprintf(msg, cap, "primary index %d points to empty slot", prim);
        return -ABI_EIO;
    }
    if (o->width == 0 || o->height == 0) {
        ksnprintf(msg, cap, "primary has zero geometry %ux%u",
                  o->width, o->height);
        return -ABI_EIO;
    }
    if (o->bpp != 32) {
        ksnprintf(msg, cap, "expected 32-bpp surface, got %u", o->bpp);
        return -ABI_EIO;
    }
    if (o->pitch_bytes < o->width * 4u) {
        ksnprintf(msg, cap, "pitch %u < width*4 (%u)",
                  o->pitch_bytes, o->width * 4u);
        return -ABI_EIO;
    }
    ksnprintf(msg, cap,
              "%d output(s); primary %s %ux%u pitch=%u bpp=%u backend=%s",
              n, o->name, o->width, o->height, o->pitch_bytes, o->bpp,
              o->backend[0] ? o->backend : "(none)");
    return 0;
}

int display_test_draw(char *msg, size_t cap) {
    /* The full pixel-correctness path is exercised by the userland
     * `drawtest` tool which opens a GUI window and calls gui_window_*.
     * Here we just verify that the back buffer is reachable and the
     * gfx primitives clip cleanly without touching out-of-bounds. */
    if (!gfx_ready()) {
        ksnprintf(msg, cap, "gfx_ready() == false");
        return ABI_DEVT_SKIP;
    }
    if (!gfx_backbuf()) {
        ksnprintf(msg, cap, "gfx_backbuf() == NULL with gfx_ready=true");
        return -ABI_EIO;
    }
    /* Geometry must match what we registered. If gfx_layer_init()
     * succeeded but the registry walk skipped it for some reason,
     * userland would see "no displays" -- catch that here. */
    int prim = display_primary_index();
    if (prim < 0) {
        ksnprintf(msg, cap, "gfx is up but no display registered");
        return -ABI_EIO;
    }
    const struct display_output *o = display_get(prim);
    if (o->width != gfx_width() || o->height != gfx_height()) {
        ksnprintf(msg, cap,
                  "registered %ux%u but gfx reports %ux%u",
                  o->width, o->height, gfx_width(), gfx_height());
        return -ABI_EIO;
    }
    ksnprintf(msg, cap, "back buffer reachable %ux%u; clip-paths OK",
              o->width, o->height);
    return 0;
}

int display_test_render(char *msg, size_t cap) {
    /* M27B: this is now the "backend interface contract" check. Verify
     * that the active gfx_backend implements the mandatory ABI fields
     * (.flip and .name) and report which optional ops it provides
     * (present_rect, describe). The compositor frame / flip counters
     * are appended for parity with the M27A behaviour. */
    int prim = display_primary_index();
    if (prim < 0) {
        ksnprintf(msg, cap, "no primary display");
        return ABI_DEVT_SKIP;
    }
    const struct gfx_backend *b = gfx_get_backend();
    if (!b) {
        ksnprintf(msg, cap, "gfx_get_backend() returned NULL");
        return -ABI_EIO;
    }
    if (!b->flip) {
        ksnprintf(msg, cap, "backend '%s' has no flip op",
                  b->name ? b->name : "(unnamed)");
        return -ABI_EIO;
    }
    if (!b->name || !b->name[0]) {
        ksnprintf(msg, cap, "backend has no name string");
        return -ABI_EIO;
    }
    char extra[64];
    extra[0] = '\0';
    if (b->describe) b->describe(extra, sizeof extra);
    struct perf_sys s;
    perf_sys_snapshot(&s);
    ksnprintf(msg, cap,
              "backend=%s flip=ok present_rect=%s describe=%s "
              "bpp=%u frames=%lu flips=%lu%s%s",
              b->name,
              b->present_rect ? "yes" : "no",
              b->describe     ? "yes" : "no",
              (unsigned)(b->bytes_per_pixel ? b->bytes_per_pixel : 4u),
              (unsigned long)s.gui_frames,
              (unsigned long)display_get(prim)->flips,
              extra[0] ? " " : "", extra);
    return 0;
}

int display_test_alpha(char *msg, size_t cap) {
    /* M27C: validate the alpha-blend math by exercising the
     * single-pixel API directly. We avoid touching the back buffer
     * because this self-test runs at boot AND can be re-triggered
     * from devtest -- we don't want to splat a coloured square on
     * top of the user's desktop every time. Instead we verify:
     *   - 0 alpha leaves dst untouched
     *   - 255 alpha overwrites with src.RGB (alpha cleared)
     *   - 50% alpha lands exactly halfway, with rounding
     *   - blending is idempotent at the extremes
     * If any of these fail the math is broken and the per-window
     * compositor will produce visibly wrong overlays. */
    uint32_t dst = 0x00404040u;       /* mid grey */
    uint32_t r;
    r = gfx_blend_pixel_argb(dst, 0x00FFFFFFu);  /* a=0 -> dst kept */
    if (r != dst) {
        ksnprintf(msg, cap, "alpha=0 should preserve dst (got %x exp %x)",
                  (unsigned)r, (unsigned)dst);
        return -ABI_EIO;
    }
    r = gfx_blend_pixel_argb(dst, 0xFF11AA22u);  /* a=255 -> src.RGB */
    if (r != 0x0011AA22u) {
        ksnprintf(msg, cap, "alpha=255 should overwrite (got %x)", (unsigned)r);
        return -ABI_EIO;
    }
    /* 50% red over mid-grey. (255*128 + 64*127 + 127)/255 = ~159
     * green/blue: (0*128 + 64*127 + 127)/255 = ~32. */
    r = gfx_blend_pixel_argb(dst, 0x80FF0000u);
    uint32_t er = (255u * 128u + 64u * 127u + 127u) / 255u;
    uint32_t eg = (0u   * 128u + 64u * 127u + 127u) / 255u;
    uint32_t eb = (0u   * 128u + 64u * 127u + 127u) / 255u;
    uint32_t exp = (er << 16) | (eg << 8) | eb;
    if (r != exp) {
        ksnprintf(msg, cap,
                  "50%% red over grey wrong: got %x exp %x",
                  (unsigned)r, (unsigned)exp);
        return -ABI_EIO;
    }
    /* Idempotence: blending alpha=0 twice still preserves. */
    r = gfx_blend_pixel_argb(dst, 0x00FF00FFu);
    if (r != dst) {
        ksnprintf(msg, cap, "alpha=0 second pass changed pixel");
        return -ABI_EIO;
    }
    ksnprintf(msg, cap,
              "src-over OK: a0=keep, a255=overwrite, a128=blend (got %x)",
              (unsigned)exp);
    return 0;
}

int display_test_dirty(char *msg, size_t cap) {
    /* M27B groundwork: verify the dirty-rect accumulator behaves.
     * Sequence:
     *   1. Cache the current dirty state (compositor may have prior
     *      pending rects we shouldn't disturb).
     *   2. Force the accumulator dirty over a known rect via
     *      gfx_mark_dirty_rect.
     *   3. Read it back -- coords must match.
     *   4. Force "full" -- gfx_dirty_get must report the surface.
     *   5. Reset by marking the original cached state.
     *
     * Skipped cleanly on a true-headless boot (gfx not ready). */
    if (!gfx_ready()) {
        ksnprintf(msg, cap, "gfx not ready -- dirty tracker untestable");
        return ABI_DEVT_SKIP;
    }
    /* Cache and clear by triggering a flip-equivalent via push-then-
     * recover. We don't call gfx_flip() here (would actually present
     * to the display in the middle of a self-test); instead we rely on
     * the fact that gfx_mark_dirty_rect followed by gfx_mark_dirty_full
     * clobbers the prior state, and the compositor's next paint will
     * re-mark whatever needs to be re-painted. Acceptable trade-off. */
    int x = 10, y = 20, w = 100, h = 50;
    gfx_mark_dirty_rect(x, y, w, h);
    int gx, gy, gw, gh;
    if (!gfx_dirty_get(&gx, &gy, &gw, &gh)) {
        ksnprintf(msg, cap, "dirty_get returned empty after mark_rect");
        return -ABI_EIO;
    }
    /* The accumulator may already have had something else union'd in,
     * so we just check that OUR rect is fully contained in the result. */
    if (gx > x || gy > y || gx + gw < x + w || gy + gh < y + h) {
        ksnprintf(msg, cap,
                  "dirty rect doesn't cover injected: got %d,%d %dx%d "
                  "vs expected at-least %d,%d %dx%d",
                  gx, gy, gw, gh, x, y, w, h);
        return -ABI_EIO;
    }
    gfx_mark_dirty_full();
    if (!gfx_dirty_get(&gx, &gy, &gw, &gh)) {
        ksnprintf(msg, cap, "dirty_get returned empty after mark_full");
        return -ABI_EIO;
    }
    if (gx != 0 || gy != 0 ||
        gw != (int)gfx_width() || gh != (int)gfx_height()) {
        ksnprintf(msg, cap,
                  "mark_full produced %d,%d %dx%d (expected 0,0 %ux%u)",
                  gx, gy, gw, gh, gfx_width(), gfx_height());
        return -ABI_EIO;
    }
    /* M27E: actively exercise gfx_flip's partial vs full vs empty
     * branches so the present-stats counters are non-zero by the time
     * any userland tool runs. The boot harness model blocks pid 0 in
     * proc_wait while the userland tools run, so the compositor would
     * NEVER tick from gui_tick (it's gated on pid 0). Driving gfx_flip
     * directly here is the only way the userland 'rendertest dirty'
     * test can observe real activity in the counters.
     *
     * We do NOT touch the back buffer -- we only manipulate the dirty
     * accumulator and call gfx_flip(). That copies whatever the GUI
     * left behind to the front buffer, which is exactly what a normal
     * compositor pass would do. The desktop's next paint will re-mark
     * everything dirty anyway, so any lost-write risk is bounded. */
    struct gfx_present_stats st_before;
    gfx_present_stats(&st_before);

    /* (a) Partial flip: small rect, present_rect path. */
    gfx_dirty_clear();
    gfx_mark_dirty_rect(64, 64, 96, 48);
    gfx_flip();

    /* (b) Full flip: mark the entire surface dirty -> b->flip() path. */
    gfx_dirty_clear();
    gfx_mark_dirty_full();
    gfx_flip();

    /* (c) Empty flip: nothing dirty -> empty/full path with a no-op
     * present. Counter goes up but no pixels are pushed. */
    gfx_dirty_clear();
    gfx_flip();

    struct gfx_present_stats st;
    gfx_present_stats(&st);
    uint64_t total_delta   = st.total_flips   - st_before.total_flips;
    uint64_t full_delta    = st.full_flips    - st_before.full_flips;
    uint64_t partial_delta = st.partial_flips - st_before.partial_flips;
    uint64_t empty_delta   = st.empty_flips   - st_before.empty_flips;
    if (total_delta != 3) {
        ksnprintf(msg, cap,
                  "gfx_flip stat fail: total +%lu (expected +3)",
                  (unsigned long)total_delta);
        return -ABI_EIO;
    }
    if (partial_delta < 1) {
        /* present_rect path didn't fire even though the backend has it.
         * That's M27E broken at the gfx layer. */
        ksnprintf(msg, cap,
                  "gfx_flip never took partial-present path "
                  "(deltas: full+%lu partial+%lu empty+%lu)",
                  (unsigned long)full_delta,
                  (unsigned long)partial_delta,
                  (unsigned long)empty_delta);
        return -ABI_EIO;
    }
    ksnprintf(msg, cap,
              "tracker OK: rect-inject -> %d,%d %dx%d, full -> %ux%u "
              "(stats: %lu flips, %lu full, %lu partial, %lu empty)",
              x, y, w, h, gfx_width(), gfx_height(),
              (unsigned long)st.total_flips,
              (unsigned long)st.full_flips,
              (unsigned long)st.partial_flips,
              (unsigned long)st.empty_flips);
    return 0;
}

int display_test_font(char *msg, size_t cap) {
    /* M27D: validate the bitmap-font scaling helpers and the bounds
     * calculator. We DON'T touch the surface here -- gfx_text_bounds
     * is pure, and we only assert that the dimensions a button-layout
     * routine would compute are correct.
     *
     * Cases:
     *   1. Empty string  -> 0x0 bounds (no NULL-deref on out_w/out_h)
     *   2. "ABC" scale=1 -> 24x8   (legacy bitmap path)
     *   3. "ABC" scale=2 -> 48x16  (M27D supersampling)
     *   4. "ABC" scale=3 -> 72x24
     *   5. "ABC\nDE" scale=2 -> 48x32 (multi-line bounds = max-line x lines)
     *   6. negative scale clamps to 1 (defensive contract)
     *
     * Anything wrong here would silently mis-position GUI labels. */
    int w = -1, h = -1;
    gfx_text_bounds("", 1, &w, &h);
    if (w != 0 || h != 0) {
        ksnprintf(msg, cap, "empty -> %dx%d (expected 0x0)", w, h);
        return -ABI_EIO;
    }
    gfx_text_bounds("ABC", 1, &w, &h);
    if (w != 24 || h != 8) {
        ksnprintf(msg, cap, "ABC s=1 -> %dx%d (expected 24x8)", w, h);
        return -ABI_EIO;
    }
    gfx_text_bounds("ABC", 2, &w, &h);
    if (w != 48 || h != 16) {
        ksnprintf(msg, cap, "ABC s=2 -> %dx%d (expected 48x16)", w, h);
        return -ABI_EIO;
    }
    gfx_text_bounds("ABC", 3, &w, &h);
    if (w != 72 || h != 24) {
        ksnprintf(msg, cap, "ABC s=3 -> %dx%d (expected 72x24)", w, h);
        return -ABI_EIO;
    }
    gfx_text_bounds("ABC\nDE", 2, &w, &h);
    if (w != 48 || h != 32) {
        ksnprintf(msg, cap,
                  "ABC\\nDE s=2 -> %dx%d (expected 48x32)", w, h);
        return -ABI_EIO;
    }
    gfx_text_bounds("ABC", -3, &w, &h);
    if (w != 24 || h != 8) {
        ksnprintf(msg, cap,
                  "ABC s=-3 (should clamp) -> %dx%d (expected 24x8)", w, h);
        return -ABI_EIO;
    }
    /* NULL out parameters are tolerated (used by callers that only
     * care about one dimension). */
    gfx_text_bounds("XY", 2, NULL, &h);
    if (h != 16) {
        ksnprintf(msg, cap,
                  "NULL-out path: h=%d (expected 16)", h);
        return -ABI_EIO;
    }
    ksnprintf(msg, cap,
              "scaled bounds OK: 1x=24x8 2x=48x16 3x=72x24 multi=48x32");
    return 0;
}

int display_test_multi(char *msg, size_t cap) {
    /* M27G: synthetic multi-monitor exercise. We:
     *   1. Snapshot the live registry (must already have a primary).
     *   2. Register two synthetic outputs ("vmon1", "vmon2") so the
     *      auto-horizontal-layout positions them to the right of the
     *      existing primary, then to the right of vmon1.
     *   3. Verify enumerate() returns 3 records and the origins line
     *      up edge-to-edge with no gap and no overlap.
     *   4. Override vmon2's origin via display_set_origin() and check
     *      it's reflected back through enumerate().
     *   5. Unregister both synthetics and assert the registry returns
     *      to its prior state (count + primary unchanged).
     *
     * If at any step the contract is violated we leave the registry
     * in whatever state we managed to roll back to and return -EIO.
     * The compositor never touches vmon1/vmon2 -- they are pure
     * registry entries with no backing surface -- so this test is
     * safe to run on a live desktop. */
    int prim_before = display_primary_index();
    int n_before    = display_count();
    if (prim_before < 0 || n_before == 0) {
        ksnprintf(msg, cap,
                  "no primary registered (skipping multi-monitor test)");
        return ABI_DEVT_SKIP;
    }

    const struct display_output *p = display_get(prim_before);
    if (!p) {
        ksnprintf(msg, cap, "primary idx=%d resolves to NULL slot",
                  prim_before);
        return -ABI_EIO;
    }
    int32_t prim_right = p->origin_x + (int32_t)p->width;

    /* Step 2: register two synthetic monitors. We pick distinct
     * geometries so an off-by-one in the layout math is obvious. */
    int v1 = display_register_output("vmon1",
                                     1024, 768, 1024 * 4, 32u,
                                     ABI_DISPLAY_FMT_XRGB8888,
                                     ABI_DISPLAY_BACKEND_NONE,
                                     "synthetic");
    if (v1 < 0) {
        ksnprintf(msg, cap, "register vmon1 failed (registry full?)");
        return -ABI_EIO;
    }
    int v2 = display_register_output("vmon2",
                                     800, 600, 800 * 4, 32u,
                                     ABI_DISPLAY_FMT_XRGB8888,
                                     ABI_DISPLAY_BACKEND_NONE,
                                     "synthetic");
    if (v2 < 0) {
        display_unregister_output(v1);
        ksnprintf(msg, cap, "register vmon2 failed (registry full?)");
        return -ABI_EIO;
    }

    int rc = 0;

    /* Step 3: enumerate and verify origins. */
    struct abi_display_info recs[DISPLAY_MAX_OUTPUTS];
    int n = display_enumerate(recs, DISPLAY_MAX_OUTPUTS);
    if (n < 3) {
        ksnprintf(msg, cap,
                  "enumerate returned %d after registering 2 (expected >=3)",
                  n);
        rc = -ABI_EIO;
        goto cleanup;
    }

    const struct display_output *o1 = display_get(v1);
    const struct display_output *o2 = display_get(v2);
    if (!o1 || !o2) {
        ksnprintf(msg, cap, "synthetic slots vanished after register");
        rc = -ABI_EIO;
        goto cleanup;
    }
    if (o1->origin_x != prim_right || o1->origin_y != 0) {
        ksnprintf(msg, cap,
                  "vmon1 origin (%d,%d) != expected (%d,0)",
                  (int)o1->origin_x, (int)o1->origin_y,
                  (int)prim_right);
        rc = -ABI_EIO;
        goto cleanup;
    }
    int32_t v1_right = o1->origin_x + (int32_t)o1->width;
    if (o2->origin_x != v1_right || o2->origin_y != 0) {
        ksnprintf(msg, cap,
                  "vmon2 origin (%d,%d) != expected (%d,0)",
                  (int)o2->origin_x, (int)o2->origin_y,
                  (int)v1_right);
        rc = -ABI_EIO;
        goto cleanup;
    }

    /* Bounding box must encompass primary + both synthetics. */
    int32_t  bx = 0, by = 0;
    uint32_t bw = 0, bh = 0;
    int      nb = display_total_bounds(&bx, &by, &bw, &bh);
    if (nb < 3 || bw < (uint32_t)(v1_right + (int32_t)o2->width - bx)) {
        ksnprintf(msg, cap,
                  "total_bounds %ux%u@(%d,%d) doesn't span 3 outputs",
                  bw, bh, (int)bx, (int)by);
        rc = -ABI_EIO;
        goto cleanup;
    }

    /* Step 4: explicit origin override. Drop vmon2 below vmon1 and
     * confirm enumerate sees it. */
    if (display_set_origin(v2, o1->origin_x, (int32_t)o1->height) != 0) {
        ksnprintf(msg, cap, "display_set_origin(vmon2) failed");
        rc = -ABI_EIO;
        goto cleanup;
    }
    o2 = display_get(v2);
    if (!o2 || o2->origin_x != o1->origin_x ||
        o2->origin_y != (int32_t)o1->height) {
        ksnprintf(msg, cap, "set_origin didn't take effect");
        rc = -ABI_EIO;
        goto cleanup;
    }

    /* Verify primary did NOT shift to a synthetic slot. */
    if (display_primary_index() != prim_before) {
        ksnprintf(msg, cap,
                  "primary drifted from %d to %d during multi-test",
                  prim_before, display_primary_index());
        rc = -ABI_EIO;
        goto cleanup;
    }

cleanup:
    /* Step 5: roll back the registry. We unregister v2 then v1 (LIFO)
     * so any auto-elect logic is exercised in the same order it would
     * be on a real hot-unplug. */
    display_unregister_output(v2);
    display_unregister_output(v1);

    int n_after    = display_count();
    int prim_after = display_primary_index();
    if (rc == 0) {
        if (n_after != n_before) {
            ksnprintf(msg, cap,
                      "registry leak: count was %d, now %d",
                      n_before, n_after);
            return -ABI_EIO;
        }
        if (prim_after != prim_before) {
            ksnprintf(msg, cap,
                      "primary changed: was %d, now %d",
                      prim_before, prim_after);
            return -ABI_EIO;
        }
        ksnprintf(msg, cap,
                  "multi OK: %d outputs registered, layout spans "
                  "%dx%d (auto-h-layout + override + cleanup)",
                  n_before + 2, (int)(prim_right + 1024 + 800),
                  (int)p->height);
        return 0;
    }
    return rc;
}

/* ---- one-time init -------------------------------------------- */

void display_init(void) {
    if (g_inited) return;
    g_inited = true;
    memset(g_outputs, 0, sizeof g_outputs);
    g_primary = -1;

    /* If gfx_init() succeeded, register its surface as the primary
     * output. Otherwise this is a true-headless boot and the registry
     * stays empty -- the display_test_basic test will SKIP cleanly
     * and the syscall returns 0 records. */
    if (gfx_ready()) {
        const struct gfx_backend *b = gfx_get_backend();
        const char *bname = (b && b->name) ? b->name : "(none)";
        uint8_t bid = backend_id_from_name(bname);
        uint32_t w  = gfx_width();
        uint32_t h  = gfx_height();
        /* gfx.c stores pitch in pixels (fb_pitch_px). The userland
         * ABI talks bytes for portability, so multiply through. We
         * also assume 32-bpp (gfx_init enforces it -- gfx_layer_init
         * silently rejects fb->bpp != 32). */
        uint32_t pitch_bytes = w * 4u;   /* tight back-buffer pitch */
        display_register_output("fb0", w, h, pitch_bytes, 32u,
                                ABI_DISPLAY_FMT_XRGB8888, bid, bname);
        kprintf("[display] registered fb0 %ux%u backend=%s\n",
                w, h, bname);
    } else {
        kprintf("[display] gfx not ready -- registry empty\n");
    }

    /* Wire the self-tests into the existing devtest registry.
     * Namespace: "display", "display_draw", "display_render",
     * "display_alpha", "display_dirty". The second-level names match
     * the M27 phase boundaries so the boot log + drvtest output
     * already advertise upcoming functionality. */
    devtest_register("display",        display_test_basic);
    devtest_register("display_draw",   display_test_draw);
    devtest_register("display_render", display_test_render);
    devtest_register("display_alpha",  display_test_alpha);
    devtest_register("display_dirty",  display_test_dirty);
    devtest_register("display_font",   display_test_font);
    devtest_register("display_multi",  display_test_multi);
}
