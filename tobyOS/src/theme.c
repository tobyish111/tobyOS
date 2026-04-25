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
    .bg              = 0x000A0F18u,   /* near-black with a hint of blue */
    .bg_band         = 0x00050913u,
    .bg_grid         = 0x00121B2Cu,
    .bg_grid_step    = 32,
    .scanline        = 1,

    /* Window chrome */
    .win_bg          = 0x00121826u,
    .win_border      = 0x00263042u,
    .win_glow        = 0x0000E6FFu,   /* cyan accent under title bar */
    .title_focus     = 0x00161E30u,
    .title_unfocus   = 0x00101522u,
    .title_text      = 0x00DCEEFFu,

    .close_bg        = 0x00301823u,
    .close_bg_hot    = 0x00FF3060u,
    .close_fg        = 0x00FFC8D8u,

    /* Taskbar */
    .taskbar         = 0x000A1020u,
    .taskbar_top     = 0x0000E6FFu,   /* cyan accent line, top edge */
    .taskbar_text    = 0x006A87A8u,   /* dim brand text */

    /* Start button */
    .start_bg        = 0x00141E36u,
    .start_bg_hot    = 0x001A2D55u,
    .start_fg        = 0x00DCEEFFu,

    /* Tabs */
    .tab_bg          = 0x000F1828u,
    .tab_bg_focus    = 0x00182944u,
    .tab_fg          = 0x00C8DDF2u,
    .tab_border      = 0x00263042u,

    /* Launcher */
    .menu_bg         = 0x00101728u,
    .menu_border     = 0x0000E6FFu,
    .menu_hot        = 0x001E2C48u,
    .menu_text       = 0x00DCEEFFu,

    /* Tray pills */
    .tray_bg         = 0x00101728u,
    .tray_bg_hot     = 0x00182944u,
    .tray_text       = 0x00C8DDF2u,
    .tray_text_dim   = 0x005A6A82u,
    .tray_border     = 0x00263042u,

    /* Status accents */
    .accent_cyan     = 0x0000E6FFu,
    .accent_magenta  = 0x00FF36C8u,
    .accent_amber    = 0x00FFB347u,
    .status_ok       = 0x0040D060u,
    .status_warn     = 0x00FFB347u,
    .status_err      = 0x00FF4060u,

    /* Toast */
    .toast_bg        = 0x000B1322u,
    .toast_border    = 0x0000E6FFu,
    .toast_title     = 0x00FFFFFFu,
    .toast_body      = 0x00B0C2D8u,

    /* Notification center */
    .center_bg       = 0x000B1322u,
    .center_border   = 0x0000E6FFu,
    .center_header   = 0x00FF36C8u,
    .center_item_bg  = 0x00121826u,
    .center_item_hot = 0x001E2C48u,
};

/* ---- M12 basic palette (fallback / debug) ------------------------ */

static const struct theme_palette g_theme_basic = {
    .id              = THEME_BASIC,
    .name            = "basic",

    .bg              = 0x00204060u,
    .bg_band         = 0x00102540u,
    .bg_grid         = 0x00000000u,
    .bg_grid_step    = 0,
    .scanline        = 0,

    .win_bg          = 0x00181818u,
    .win_border      = 0x00101010u,
    .win_glow        = 0x00101010u,   /* no glow accent in basic */
    .title_focus     = 0x00224488u,
    .title_unfocus   = 0x00606060u,
    .title_text      = 0x00FFFFFFu,

    .close_bg        = 0x00C04040u,
    .close_bg_hot    = 0x00FF6060u,
    .close_fg        = 0x00FFFFFFu,

    .taskbar         = 0x00181C24u,
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
};

/* Active selection. Pre-init we still want a valid pointer so
 * theme_active() can be called from very early boot diagnostics. */
static const struct theme_palette *g_active = &g_theme_cyber;

const struct theme_palette *theme_get(enum theme_id id) {
    switch (id) {
    case THEME_BASIC: return &g_theme_basic;
    case THEME_CYBER:
    default:          return &g_theme_cyber;
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
    } else {
        g_active = &g_theme_cyber;
    }
    kprintf("[theme] active='%s' (settings ui.theme='%s')\n",
            g_active->name, buf);
}
