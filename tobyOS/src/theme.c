/* theme.c -- palette tables + selection for M31's UI theme.
 *
 * The palettes are plain `static const` global tables so they live in
 * .rodata and are addressable from any context (compositor IRQ paths
 * included). theme_init reads the active selection from settings;
 * everything else just chases a pointer.
 *
 * Design notes on the cyber palette:
 *   - Backgrounds stay deeply dark (#0A0F1A territory) so the neon
 *     accents pop. Pure black is avoided -- it makes the window
 *     borders invisible and reads as a dead pixel band on framebuffers
 *     that crush blacks (real LCDs over VGA-CT).
 *   - Accents are deliberately limited to three: cyan (info / primary
 *     focus), magenta (selected / brand), amber (warnings + clock).
 *     More than three accents always degenerates into "rainbow noise".
 *   - "Translucent" is faked. We don't have a per-pixel-alpha
 *     compositor, so panels just use a single dark fill with a 1-px
 *     accent line on the relevant edge -- the eye reads that as a
 *     sharp glass panel.
 *
 * The basic palette mirrors the M12 hard-coded constants verbatim, so
 * a regressed frame can always be diff'd against the prior look.
 * Anything new in M31 (tray pills, toasts, center) gets a basic
 * variant too -- usually just "the same dark grey as the menu" -- so
 * a basic-theme boot still draws everything coherently.
 */

#include <tobyos/theme.h>
#include <tobyos/settings.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* ---- M31 cyber palette ------------------------------------------- */

static const struct theme_palette g_theme_cyber = {
    .id              = THEME_CYBER,
    .name            = "cyber",

    /* Wallpaper */
    .bg              = 0x00070A14u,   /* slightly warmer deep graphite */
    .bg_band         = 0x000B1326u,
    .bg_vignette     = 0x00111A2Au,
    .bg_grid         = 0x00162138u,
    .bg_grid_step    = 32,
    .scanline        = 1,

    /* Window chrome */
    .win_bg          = 0x000F1724u,
    .win_border      = 0x00384E6Au,
    .win_glow        = 0x0000E6FFu,   /* cyan accent under title bar */
    .win_shadow      = 0x60000000u,
    .win_shadow_deep = 0x90000000u,
    .title_focus     = 0x00162038u,
    .title_focus_hi  = 0x00243860u,
    .title_unfocus   = 0x000F1522u,
    .title_unfocus_hi = 0x00182234u,
    .title_text      = 0x00F0FAFFu,
    .title_text_dim  = 0x008AA0B8u,

    .close_bg        = 0x001A2530u,
    .close_bg_hot    = 0x00FF2F66u,
    .close_fg        = 0x00FFC8D8u,

    /* Taskbar */
    .taskbar         = 0x00090E1Au,
    .taskbar_glass   = 0x92090E1Au,
    .taskbar_top     = 0x0000E6FFu,   /* cyan accent line, top edge */
    .taskbar_text    = 0x00708EA8u,   /* dim brand text */

    /* Start button */
    .start_bg        = 0x00131D36u,
    .start_bg_hot    = 0x00203862u,
    .start_fg        = 0x00E0F0FFu,

    /* Tabs */
    .tab_bg          = 0x000E1625u,
    .tab_bg_focus    = 0x001A3054u,
    .tab_fg          = 0x00D0E4F6u,
    .tab_border      = 0x002A3648u,

    /* Launcher */
    .menu_bg         = 0x000D1424u,
    .menu_border     = 0x0000E6FFu,
    .menu_hot        = 0x00202F4Cu,
    .menu_text       = 0x00E4F2FFu,

    /* Tray pills */
    .tray_bg         = 0x000D1424u,
    .tray_bg_hot     = 0x001A2D48u,
    .tray_text       = 0x00D4E8F6u,
    .tray_text_dim   = 0x00627688u,
    .tray_border     = 0x002C3A4Cu,

    /* Status accents */
    .accent_cyan     = 0x0000E6FFu,
    .accent_magenta  = 0x00FF36C8u,
    .accent_amber    = 0x00FFB347u,
    .status_ok       = 0x0045D868u,
    .status_warn     = 0x00FFB347u,
    .status_err      = 0x00FF4060u,

    /* Toast */
    .toast_bg        = 0x000C1424u,
    .toast_border    = 0x0000E6FFu,
    .toast_title     = 0x00FFFFFFu,
    .toast_body      = 0x00B8CCDEU,

    /* Notification center */
    .center_bg       = 0x000C1424u,
    .center_border   = 0x0000E6FFu,
    .center_header   = 0x00FF36C8u,
    .center_item_bg  = 0x00141A28u,
    .center_item_hot = 0x00202F4Cu,

    .panel           = 0x000B1220u,
    .panel_glass     = 0x8E0B1220u,
    .border_cyan     = 0x0000E6FFu,
    .border_orange   = 0x00FF9F2Eu,
    .glow_orange     = 0x00FFB347u,
    .glow_cyan       = 0x0000E6FFu,
    .text_primary    = 0x00F5FCFFu,
    .text_secondary  = 0x0090A8C0u,
    .danger          = 0x00FF4060u,
    .success         = 0x0045D868u,
    .corner_radius   = 5,
    .spacing         = 6,
};

/* ---- M12 basic palette (fallback / debug) ------------------------ */

static const struct theme_palette g_theme_basic = {
    .id              = THEME_BASIC,
    .name            = "basic",

    .bg              = 0x00204060u,
    .bg_band         = 0x00102540u,
    .bg_vignette     = 0x00204060u,
    .bg_grid         = 0x00000000u,
    .bg_grid_step    = 0,
    .scanline        = 0,

    .win_bg          = 0x00181818u,
    .win_border      = 0x00101010u,
    .win_glow        = 0x00101010u,   /* no glow accent in basic */
    .win_shadow      = 0x00000000u,
    .win_shadow_deep = 0x00000000u,
    .title_focus     = 0x00224488u,
    .title_focus_hi  = 0x00224488u,
    .title_unfocus   = 0x00606060u,
    .title_unfocus_hi = 0x00606060u,
    .title_text      = 0x00FFFFFFu,
    .title_text_dim  = 0x00C0C0C0u,

    .close_bg        = 0x00C04040u,
    .close_bg_hot    = 0x00FF6060u,
    .close_fg        = 0x00FFFFFFu,

    .taskbar         = 0x00181C24u,
    .taskbar_glass   = 0xFF181C24u,
    .taskbar_top     = 0x00404858u,
    .taskbar_text    = 0x00B0B8C8u,

    .start_bg        = 0x00305078u,
    .start_bg_hot    = 0x00407098u,
    .start_fg        = 0x00FFFFFFu,

    .tab_bg          = 0x00282E38u,
    .tab_bg_focus    = 0x00405678u,
    .tab_fg          = 0x00E0E0E0u,
    .tab_border      = 0x00101010u,

    .menu_bg         = 0x002A323Eu,
    .menu_border     = 0x00808890u,
    .menu_hot        = 0x00405678u,
    .menu_text       = 0x00FFFFFFu,

    .tray_bg         = 0x00282E38u,
    .tray_bg_hot     = 0x00405678u,
    .tray_text       = 0x00E0E0E0u,
    .tray_text_dim   = 0x00808890u,
    .tray_border     = 0x00101010u,

    .accent_cyan     = 0x0080C0FFu,
    .accent_magenta  = 0x00C080FFu,
    .accent_amber    = 0x00FFC080u,
    .status_ok       = 0x0080FF80u,
    .status_warn     = 0x00FFC080u,
    .status_err      = 0x00FF8080u,

    .toast_bg        = 0x002A323Eu,
    .toast_border    = 0x00808890u,
    .toast_title     = 0x00FFFFFFu,
    .toast_body      = 0x00C0C8D0u,

    .center_bg       = 0x002A323Eu,
    .center_border   = 0x00808890u,
    .center_header   = 0x00FFFFFFu,
    .center_item_bg  = 0x001E242Cu,
    .center_item_hot = 0x00405678u,

    .panel           = 0x002A323Eu,
    .panel_glass     = 0xFF2A323Eu,
    .border_cyan     = 0x0080C0FFu,
    .border_orange   = 0x00FFC080u,
    .glow_orange     = 0x00FFC080u,
    .glow_cyan       = 0x0080C0FFu,
    .text_primary    = 0x00FFFFFFu,
    .text_secondary  = 0x00C0C8D0u,
    .danger          = 0x00FF8080u,
    .success         = 0x0080FF80u,
    .corner_radius   = 2,
    .spacing         = 8,
};

/* ---- M37 KDE Plasma Breeze-dark inspired palette ----------------- */

static const struct theme_palette g_theme_plasma = {
    .id              = THEME_PLASMA,
    .name            = "plasma",

    /* Wallpaper: deep blue-grey gradient, matching Breeze wallpapers. */
    .bg              = 0x001B2838u,
    .bg_band         = 0x00142030u,
    .bg_vignette     = 0x00253545u,
    .bg_grid         = 0x002A3A4Au,
    .bg_grid_step    = 0,        /* no grid (cleaner Plasma look) */
    .scanline        = 0,

    /* Window chrome: Breeze-dark uses #31363b as title, #2a2e32 body. */
    .win_bg          = 0x002A2E32u,
    .win_border      = 0x003D4248u,
    .win_glow        = 0x003DAEE9u,   /* Breeze blue highlight */
    .win_shadow      = 0x60000000u,
    .win_shadow_deep = 0x90000000u,
    .title_focus     = 0x0031363Bu,
    .title_focus_hi  = 0x003B4045u,
    .title_unfocus   = 0x002A2E32u,
    .title_unfocus_hi = 0x0031363Bu,
    .title_text      = 0x00EFF0F1u,
    .title_text_dim  = 0x007F8C8Du,

    .close_bg        = 0x0031363Bu,
    .close_bg_hot    = 0x00ED1515u,   /* Breeze close-button red */
    .close_fg        = 0x00FFFFFFu,

    /* Taskbar: dark Plasma panel style. */
    .taskbar         = 0x001E2229u,
    .taskbar_glass   = 0xC81E2229u,
    .taskbar_top     = 0x003DAEE9u,   /* Breeze accent blue */
    .taskbar_text    = 0x007F8C8Du,

    /* Start button (Plasma kickoff trigger). */
    .start_bg        = 0x0031363Bu,
    .start_bg_hot    = 0x003B4045u,
    .start_fg        = 0x00EFF0F1u,

    /* Tabs */
    .tab_bg          = 0x00272C31u,
    .tab_bg_focus    = 0x003DAEE9u,
    .tab_fg          = 0x00EFF0F1u,
    .tab_border      = 0x003D4248u,

    /* Launcher (Kickoff style) */
    .menu_bg         = 0x001E2229u,
    .menu_border     = 0x003DAEE9u,
    .menu_hot        = 0x003B4045u,
    .menu_text       = 0x00EFF0F1u,

    /* Tray pills */
    .tray_bg         = 0x00272C31u,
    .tray_bg_hot     = 0x003B4045u,
    .tray_text       = 0x00EFF0F1u,
    .tray_text_dim   = 0x007F8C8Du,
    .tray_border     = 0x003D4248u,

    /* Status accents: Breeze palette accents. */
    .accent_cyan     = 0x003DAEE9u,   /* KDE blue */
    .accent_magenta  = 0x009B59B6u,   /* Breeze purple */
    .accent_amber    = 0x00F67400u,   /* Breeze orange */
    .status_ok       = 0x0027AE60u,   /* Breeze green */
    .status_warn     = 0x00F67400u,
    .status_err      = 0x00ED1515u,   /* Breeze red */

    /* Toast */
    .toast_bg        = 0x001E2229u,
    .toast_border    = 0x003DAEE9u,
    .toast_title     = 0x00EFF0F1u,
    .toast_body      = 0x00BDC3C7u,

    /* Notification center */
    .center_bg       = 0x001E2229u,
    .center_border   = 0x003DAEE9u,
    .center_header   = 0x003DAEE9u,
    .center_item_bg  = 0x00272C31u,
    .center_item_hot = 0x003B4045u,

    /* Panel tokens */
    .panel           = 0x001E2229u,
    .panel_glass     = 0xC81E2229u,
    .border_cyan     = 0x003DAEE9u,
    .border_orange   = 0x00F67400u,
    .glow_orange     = 0x00F67400u,
    .glow_cyan       = 0x003DAEE9u,
    .text_primary    = 0x00EFF0F1u,
    .text_secondary  = 0x007F8C8Du,
    .danger          = 0x00ED1515u,
    .success         = 0x0027AE60u,
    .corner_radius   = 8,
    .spacing         = 8,
};

/* Active selection. Pre-init we still want a valid pointer so
 * theme_active() can be called from very early boot diagnostics. */
static const struct theme_palette *g_active = &g_theme_cyber;

const struct theme_palette *theme_get(enum theme_id id) {
    switch (id) {
    case THEME_BASIC:  return &g_theme_basic;
    case THEME_PLASMA: return &g_theme_plasma;
    case THEME_CYBER:
    default:           return &g_theme_cyber;
    }
}

const struct theme_palette *theme_active(void) {
    return g_active ? g_active : &g_theme_cyber;
}

void theme_set(enum theme_id id) {
    const struct theme_palette *p = theme_get(id);
    if (p == g_active) return;
    kprintf("[theme] switch %s -> %s\n", g_active->name, p->name);
    g_active = p;
}

/* Case-insensitive equality on a small literal, no strcasecmp in our
 * libc. */
static int eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

void theme_init(void) {
    char buf[16];
    size_t n = settings_get_str("ui.theme", buf, sizeof(buf), "cyber");
    (void)n;
    if (eq_ci(buf, "basic")) {
        g_active = &g_theme_basic;
    } else if (eq_ci(buf, "plasma")) {
        g_active = &g_theme_plasma;
    } else {
        g_active = &g_theme_cyber;
    }
    kprintf("[theme] active='%s' (settings ui.theme='%s')\n",
            g_active->name, buf);
}

/* ---- Phase 2 M2.4: Fluent Design Theme State ---------------------- */

static const struct theme_state g_fluent_dark = {
    .background = 0x00202020u,
    .foreground = 0x00FFFFFFu,
    .accent     = 0x000078D4u,
    .border     = 0x00404040u,
    .button     = 0x002D2D2Du,
    .input      = 0x001E1E1Eu,
    .hover      = 0x00383838u,
    .active     = 0x00505050u,
};

static const struct theme_state g_fluent_light = {
    .background = 0x00F3F3F3u,
    .foreground = 0x00191919u,
    .accent     = 0x000067C0u,
    .border     = 0x00C8C8C8u,
    .button     = 0x00FBFBFBu,
    .input      = 0x00FFFFFFu,
    .hover      = 0x00E8E8E8u,
    .active     = 0x00D0D0D0u,
};

static const struct theme_state *g_fluent_active = &g_fluent_dark;

const struct theme_state *theme_fluent_active(void) {
    return g_fluent_active;
}

int theme_fluent_set(uint32_t mode) {
    switch (mode) {
    case THEME_MODE_DARK:
        g_fluent_active = &g_fluent_dark;
        kprintf("[theme] fluent mode -> dark\n");
        return 0;
    case THEME_MODE_LIGHT:
        g_fluent_active = &g_fluent_light;
        kprintf("[theme] fluent mode -> light\n");
        return 0;
    default:
        return -1;
    }
}

/* ---- Animation easing functions ----------------------------------- *
 *
 * All operate on a fixed-point range [0, 256] representing [0.0, 1.0].
 * Uses quadratic approximation (no floating point). */

int theme_ease_in(int t) {
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    return (t * t) >> 8;
}

int theme_ease_out(int t) {
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    int inv = 256 - t;
    return 256 - ((inv * inv) >> 8);
}

int theme_ease_in_out(int t) {
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    if (t < 128) {
        return (t * t) >> 7;
    } else {
        int inv = 256 - t;
        return 256 - ((inv * inv) >> 7);
    }
}
