/* src/anim.c -- Smooth window animation framework.
 *
 * Manages up to ANIM_MAX_CONCURRENT active animations. The compositor
 * calls anim_tick() each frame to advance progress, then queries
 * anim_get_transform() for each window to apply scale/translate/alpha.
 *
 * All math is fixed-point (scaled by 1024) -- no FPU/SSE needed.
 */

#include <tobyos/anim.h>
#include <tobyos/wm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define FP_SHIFT 10
#define FP_ONE   (1 << FP_SHIFT)  /* 1024 */

/* ---- Animation pool ---- */

static struct animation g_anims[ANIM_MAX_CONCURRENT];

/* ---- Easing: ease_out_cubic(t) = 1 - (1-t)^3 ----
 * Input and output are in [0..1024]. */

int anim_ease_out_cubic(int t) {
    if (t <= 0) return 0;
    if (t >= FP_ONE) return FP_ONE;
    int inv = FP_ONE - t;
    int inv2 = (inv * inv) >> FP_SHIFT;
    int inv3 = (inv2 * inv) >> FP_SHIFT;
    return FP_ONE - inv3;
}

/* ---- Init ---- */

void anim_init(void) {
    memset(g_anims, 0, sizeof(g_anims));
    kprintf("[anim] animation subsystem init (max %d concurrent)\n",
            ANIM_MAX_CONCURRENT);
}

/* ---- Start an animation ---- */

int anim_start(int wid, anim_type_t type, uint32_t duration_ms) {
    if (type == ANIM_NONE || duration_ms == 0)
        return -1;

    /* Cancel any existing animation on this window */
    for (int i = 0; i < ANIM_MAX_CONCURRENT; i++) {
        if (g_anims[i].active && g_anims[i].wid == wid)
            g_anims[i].active = 0;
    }

    /* Find free slot */
    struct animation *a = NULL;
    for (int i = 0; i < ANIM_MAX_CONCURRENT; i++) {
        if (!g_anims[i].active) { a = &g_anims[i]; break; }
    }
    if (!a) return -1;

    memset(a, 0, sizeof(*a));
    a->wid = wid;
    a->type = type;
    a->duration_ms = duration_ms;
    a->start_ms = 0;
    a->progress_fp = 0;
    a->active = 1;

    struct wm_window *win = wm_get_window(wid);
    if (win) {
        a->x1 = win->x;
        a->y1 = win->y;
        a->w1 = win->w;
        a->h1 = win->h;
    }

    switch (type) {
    case ANIM_WINDOW_OPEN:
        a->alpha0 = ANIM_ALPHA_HIDDEN;
        a->alpha1 = ANIM_ALPHA_OPAQUE;
        if (win) {
            int cx = win->x + win->w / 2;
            int cy = win->y + win->h / 2;
            a->w0 = (win->w * 85) / 100;
            a->h0 = (win->h * 85) / 100;
            a->x0 = cx - a->w0 / 2;
            a->y0 = cy - a->h0 / 2;
        }
        break;

    case ANIM_WINDOW_CLOSE:
        a->alpha0 = ANIM_ALPHA_OPAQUE;
        a->alpha1 = ANIM_ALPHA_HIDDEN;
        if (win) {
            int cx = win->x + win->w / 2;
            int cy = win->y + win->h / 2;
            a->x0 = win->x;
            a->y0 = win->y;
            a->w0 = win->w;
            a->h0 = win->h;
            a->w1 = (win->w * 90) / 100;
            a->h1 = (win->h * 90) / 100;
            a->x1 = cx - a->w1 / 2;
            a->y1 = cy - a->h1 / 2;
        }
        break;

    case ANIM_WINDOW_MINIMIZE:
        a->alpha0 = ANIM_ALPHA_OPAQUE;
        a->alpha1 = ANIM_ALPHA_HIDDEN;
        if (win) {
            a->x0 = win->x;
            a->y0 = win->y;
            a->w0 = win->w;
            a->h0 = win->h;
            int sw = wm_get_screen_w();
            int sh = wm_get_screen_h();
            a->x1 = sw / 2 - 20;
            a->y1 = sh - 40;
            a->w1 = 40;
            a->h1 = 30;
        }
        break;

    case ANIM_WINDOW_RESTORE:
        a->alpha0 = ANIM_ALPHA_HIDDEN;
        a->alpha1 = ANIM_ALPHA_OPAQUE;
        if (win) {
            int sw = wm_get_screen_w();
            int sh = wm_get_screen_h();
            a->x0 = sw / 2 - 20;
            a->y0 = sh - 40;
            a->w0 = 40;
            a->h0 = 30;
        }
        break;

    case ANIM_FADE_IN:
        a->alpha0 = ANIM_ALPHA_HIDDEN;
        a->alpha1 = ANIM_ALPHA_OPAQUE;
        if (win) {
            a->x0 = win->x; a->y0 = win->y;
            a->w0 = win->w; a->h0 = win->h;
        }
        break;

    case ANIM_FADE_OUT:
        a->alpha0 = ANIM_ALPHA_OPAQUE;
        a->alpha1 = ANIM_ALPHA_HIDDEN;
        if (win) {
            a->x0 = win->x; a->y0 = win->y;
            a->w0 = win->w; a->h0 = win->h;
        }
        break;

    case ANIM_SLIDE_IN:
        a->alpha0 = ANIM_ALPHA_HIDDEN;
        a->alpha1 = ANIM_ALPHA_OPAQUE;
        if (win) {
            a->x0 = win->x;
            a->y0 = win->y - 20;
            a->w0 = win->w;
            a->h0 = win->h;
        }
        break;

    case ANIM_SLIDE_OUT:
        a->alpha0 = ANIM_ALPHA_OPAQUE;
        a->alpha1 = ANIM_ALPHA_HIDDEN;
        if (win) {
            a->x0 = win->x; a->y0 = win->y;
            a->w0 = win->w; a->h0 = win->h;
            a->x1 = win->x; a->y1 = win->y - 20;
            a->w1 = win->w; a->h1 = win->h;
        }
        break;

    default:
        a->active = 0;
        return -1;
    }

    return 0;
}

/* ---- Tick all active animations ---- */

void anim_tick(uint32_t now_ms) {
    for (int i = 0; i < ANIM_MAX_CONCURRENT; i++) {
        struct animation *a = &g_anims[i];
        if (!a->active) continue;

        if (a->start_ms == 0)
            a->start_ms = now_ms;

        uint32_t elapsed = now_ms - a->start_ms;
        if (elapsed >= a->duration_ms) {
            a->progress_fp = FP_ONE;
            a->active = 0;
            continue;
        }

        a->progress_fp = (int)((elapsed * FP_ONE) / a->duration_ms);
    }
}

/* ---- Get transform for a window ---- */

int anim_get_transform(int wid, int *x, int *y, int *w, int *h, int *alpha) {
    for (int i = 0; i < ANIM_MAX_CONCURRENT; i++) {
        struct animation *a = &g_anims[i];
        if (!a->active || a->wid != wid) continue;

        int t = anim_ease_out_cubic(a->progress_fp);

        if (x) *x = a->x0 + ((a->x1 - a->x0) * t) / FP_ONE;
        if (y) *y = a->y0 + ((a->y1 - a->y0) * t) / FP_ONE;
        if (w) *w = a->w0 + ((a->w1 - a->w0) * t) / FP_ONE;
        if (h) *h = a->h0 + ((a->h1 - a->h0) * t) / FP_ONE;
        if (alpha) *alpha = a->alpha0 + ((a->alpha1 - a->alpha0) * t) / FP_ONE;

        return 1;
    }
    return 0;
}

/* ---- Check if window has active animation ---- */

int anim_is_active(int wid) {
    for (int i = 0; i < ANIM_MAX_CONCURRENT; i++) {
        if (g_anims[i].active && g_anims[i].wid == wid)
            return 1;
    }
    return 0;
}
