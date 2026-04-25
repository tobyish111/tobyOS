/* theme.h -- M31 Cyberpunk UI theme palette + geometry constants.
 *
 * Single source of truth for every colour, accent line, panel size,
 * and typography offset used by the desktop shell. The compositor
 * (src/gui.c) and any future C++ UI layer both consume the palette
 * through theme_active().
 *
 * Two themes ship today:
 *
 *   THEME_CYBER  - the M31 default. Dark translucent panels (faked
 *                  with darken-toward-black on the framebuffer; we
 *                  don't have real alpha yet), neon cyan/magenta/
 *                  amber accents, sharp 1-px borders, optional
 *                  half-alpha scanline band on the wallpaper.
 *
 *   THEME_BASIC  - the M12 desktop palette preserved verbatim, kept
 *                  as a debug fallback so a regressing M31 frame can
 *                  always be A/B'd against the known-good prior look.
 *                  Selected via /data/settings.conf "ui.theme=basic".
 *
 * Lifecycle:
 *
 *   theme_init()        called once from kmain after settings_init,
 *                       before gui_init. Picks the active palette
 *                       from settings_get_str("ui.theme", ...).
 *
 *   theme_active()      returns a pointer to the currently active
 *                       palette. Cheap (returns a static pointer);
 *                       safe from any context including IRQ.
 *
 *   theme_set(id)       runtime override; primarily for tests. The
 *                       next compositor pass picks up the change.
 *
 * The palette layout is intentionally flat (no nested structs, no
 * function pointers) so the same memory image works unchanged from
 * future C++ code via `extern "C"`.
 */

#ifndef TOBYOS_THEME_H
#define TOBYOS_THEME_H

#include <tobyos/types.h>

enum theme_id {
    THEME_CYBER = 0,
    THEME_BASIC = 1,
};

struct theme_palette {
    /* Identity */
    int      id;            /* enum theme_id */
    const char *name;       /* "cyber" / "basic" */

    /* Wallpaper / desktop background */
    uint32_t bg;            /* solid fill behind everything */
    uint32_t bg_band;       /* darker band along the very top */
    uint32_t bg_grid;       /* faint grid line colour, used in cyber */
    int      bg_grid_step;  /* 0 = no grid, else px between lines */
    int      scanline;      /* 0 = no scanline overlay, 1 = subtle band */

    /* Window chrome */
    uint32_t win_bg;        /* default client-area fill */
    uint32_t win_border;    /* outer 1-px frame */
    uint32_t win_glow;      /* M31: 1-px accent line under title bar */
    uint32_t title_focus;   /* focused window title bar */
    uint32_t title_unfocus; /* background window title bar */
    uint32_t title_text;    /* title-bar text */

    /* Close button (top-right "X") */
    uint32_t close_bg;
    uint32_t close_bg_hot;
    uint32_t close_fg;

    /* Taskbar */
    uint32_t taskbar;       /* taskbar fill */
    uint32_t taskbar_top;   /* 1-px accent line at the top edge */
    uint32_t taskbar_text;  /* dim brand text on the right */

    /* Start ("Apps") button */
    uint32_t start_bg;
    uint32_t start_bg_hot;
    uint32_t start_fg;

    /* Window tabs in the taskbar */
    uint32_t tab_bg;
    uint32_t tab_bg_focus;
    uint32_t tab_fg;
    uint32_t tab_border;

    /* Launcher menu */
    uint32_t menu_bg;
    uint32_t menu_border;
    uint32_t menu_hot;
    uint32_t menu_text;

    /* M31: system tray pills */
    uint32_t tray_bg;       /* pill fill */
    uint32_t tray_bg_hot;   /* pill fill on hover */
    uint32_t tray_text;     /* pill label */
    uint32_t tray_text_dim; /* "—" / disconnected */
    uint32_t tray_border;

    /* M31: status accent colours, reused by indicators + notifications */
    uint32_t accent_cyan;   /* primary neon */
    uint32_t accent_magenta;/* secondary */
    uint32_t accent_amber;  /* tertiary / warnings */
    uint32_t status_ok;     /* green dot */
    uint32_t status_warn;   /* amber dot */
    uint32_t status_err;    /* red dot */

    /* M31: notification toast + center panel */
    uint32_t toast_bg;
    uint32_t toast_border;
    uint32_t toast_title;
    uint32_t toast_body;
    uint32_t center_bg;
    uint32_t center_border;
    uint32_t center_header;
    uint32_t center_item_bg;
    uint32_t center_item_hot;
};

/* ---- lifecycle ----------------------------------------------------- */

/* Pick the active palette from settings. Safe to call even if /data
 * isn't mounted (settings_get_str returns the default in that case). */
void theme_init(void);

/* Returns a pointer to the active palette. NEVER NULL once
 * theme_init() has run; if called before init it returns the
 * built-in cyber palette so callers always get a valid pointer. */
const struct theme_palette *theme_active(void);

/* Force-select a theme by id. Logs the change. Used by tests and the
 * (future) settings GUI when the user flips themes live. */
void theme_set(enum theme_id id);

/* Look up a palette by id without touching the active selection.
 * Used by the compositor's diagnostic dump. */
const struct theme_palette *theme_get(enum theme_id id);

#endif /* TOBYOS_THEME_H */
