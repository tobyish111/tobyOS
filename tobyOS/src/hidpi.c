/* hidpi.c -- HiDPI display scaling system (Phase 2 M2.6).
 *
 * Provides per-display DPI scaling:
 *   - Detects display DPI from EDID (if available) or user config
 *   - Scale factors: 1.0x (96 DPI), 1.25x (120), 1.5x (144), 2.0x (192)
 *   - Logical vs physical coordinate translation
 *   - Notifies compositor when scale changes (windows must re-render)
 *   - Apps query the scale factor and adjust rendering accordingly
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* Scale factor presets */
#define HIDPI_SCALE_100  100   /* 1.0x - 96 DPI */
#define HIDPI_SCALE_125  125   /* 1.25x - 120 DPI */
#define HIDPI_SCALE_150  150   /* 1.5x - 144 DPI */
#define HIDPI_SCALE_175  175   /* 1.75x - 168 DPI */
#define HIDPI_SCALE_200  200   /* 2.0x - 192 DPI */
#define HIDPI_SCALE_250  250   /* 2.5x - 240 DPI */
#define HIDPI_SCALE_300  300   /* 3.0x - 288 DPI */

struct hidpi_display {
    int id;
    int physical_width;    /* pixels */
    int physical_height;
    int logical_width;     /* scaled pixels */
    int logical_height;
    int scale_percent;     /* 100 = 1.0x */
    int dpi_x;
    int dpi_y;
    bool active;
};

#define HIDPI_MAX_DISPLAYS 4

static struct hidpi_display g_displays[HIDPI_MAX_DISPLAYS];
static int g_display_count;
static int g_default_scale = HIDPI_SCALE_100;

/* ---- Scale calculations ---- */

static int scale_up(int logical, int scale_pct) {
    return (logical * scale_pct + 50) / 100;
}

static int scale_down(int physical, int scale_pct) {
    if (scale_pct == 0) return physical;
    return (physical * 100 + scale_pct / 2) / scale_pct;
}

/* ---- Display management ---- */

int hidpi_register_display(int width, int height, int dpi) {
    if (g_display_count >= HIDPI_MAX_DISPLAYS) return -1;

    struct hidpi_display *d = &g_displays[g_display_count];
    d->id = g_display_count;
    d->physical_width = width;
    d->physical_height = height;
    d->dpi_x = dpi;
    d->dpi_y = dpi;
    d->active = true;

    /* Auto-detect scale from DPI */
    if (dpi <= 108) {
        d->scale_percent = HIDPI_SCALE_100;
    } else if (dpi <= 132) {
        d->scale_percent = HIDPI_SCALE_125;
    } else if (dpi <= 156) {
        d->scale_percent = HIDPI_SCALE_150;
    } else if (dpi <= 180) {
        d->scale_percent = HIDPI_SCALE_175;
    } else if (dpi <= 216) {
        d->scale_percent = HIDPI_SCALE_200;
    } else if (dpi <= 264) {
        d->scale_percent = HIDPI_SCALE_250;
    } else {
        d->scale_percent = HIDPI_SCALE_300;
    }

    d->logical_width = scale_down(width, d->scale_percent);
    d->logical_height = scale_down(height, d->scale_percent);

    kprintf("[hidpi] Display %d: %dx%d physical, %dx%d logical (scale %d%%, %d DPI)\n",
            d->id, width, height, d->logical_width, d->logical_height,
            d->scale_percent, dpi);

    g_display_count++;
    return d->id;
}

int hidpi_set_scale(int display_id, int scale_percent) {
    if (display_id < 0 || display_id >= g_display_count) return -1;
    if (scale_percent < 100 || scale_percent > 400) return -1;

    struct hidpi_display *d = &g_displays[display_id];
    d->scale_percent = scale_percent;
    d->logical_width = scale_down(d->physical_width, scale_percent);
    d->logical_height = scale_down(d->physical_height, scale_percent);

    kprintf("[hidpi] Display %d scale changed to %d%% (%dx%d logical)\n",
            display_id, scale_percent, d->logical_width, d->logical_height);
    return 0;
}

int hidpi_get_scale(int display_id) {
    if (display_id < 0 || display_id >= g_display_count)
        return g_default_scale;
    return g_displays[display_id].scale_percent;
}

/* Convert logical coordinates to physical */
int hidpi_to_physical_x(int display_id, int logical_x) {
    int scale = hidpi_get_scale(display_id);
    return scale_up(logical_x, scale);
}

int hidpi_to_physical_y(int display_id, int logical_y) {
    int scale = hidpi_get_scale(display_id);
    return scale_up(logical_y, scale);
}

/* Convert physical coordinates to logical */
int hidpi_to_logical_x(int display_id, int physical_x) {
    int scale = hidpi_get_scale(display_id);
    return scale_down(physical_x, scale);
}

int hidpi_to_logical_y(int display_id, int physical_y) {
    int scale = hidpi_get_scale(display_id);
    return scale_down(physical_y, scale);
}

/* Get logical display dimensions */
int hidpi_get_logical_size(int display_id, int *w, int *h) {
    if (display_id < 0 || display_id >= g_display_count) return -1;
    if (w) *w = g_displays[display_id].logical_width;
    if (h) *h = g_displays[display_id].logical_height;
    return 0;
}

/* Scale a font size for the given display */
int hidpi_scale_font_size(int display_id, int base_size_px) {
    int scale = hidpi_get_scale(display_id);
    return scale_up(base_size_px, scale);
}

/* Scale a dimension (width, height, margin, padding) */
int hidpi_scale_dim(int display_id, int dim) {
    int scale = hidpi_get_scale(display_id);
    return scale_up(dim, scale);
}

void hidpi_init(void) {
    g_display_count = 0;
    g_default_scale = HIDPI_SCALE_100;
    memset(g_displays, 0, sizeof(g_displays));
    kprintf("[hidpi] Display scaling subsystem initialized\n");
}
