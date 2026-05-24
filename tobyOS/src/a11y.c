/* a11y.c -- Accessibility support layer.
 *
 * Provides screen reader support, high contrast mode, font scaling,
 * keyboard-only navigation, and text-to-speech placeholder.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>

#define A11Y_TEXT_BUF_SIZE  4096
#define A11Y_ANNOUNCE_MAX   256
#define A11Y_WIDGET_DESC_MAX 128

void a11y_speak(const char *text);

enum a11y_mode {
    A11Y_MODE_NORMAL        = 0,
    A11Y_MODE_HIGH_CONTRAST = (1 << 0),
    A11Y_MODE_SCREEN_READER = (1 << 1),
    A11Y_MODE_KEYBOARD_ONLY = (1 << 2),
    A11Y_MODE_LARGE_TEXT    = (1 << 3),
};

struct a11y_state {
    bool     initialized;
    uint32_t modes;
    uint32_t font_scale_x10;  /* 10 = 1.0x, 30 = 3.0x */
    char     focused_widget[A11Y_WIDGET_DESC_MAX];
    char     screen_text[A11Y_TEXT_BUF_SIZE];
    size_t   screen_text_len;
    char     last_announcement[A11Y_ANNOUNCE_MAX];
};

static struct a11y_state g_a11y;

void a11y_init(void) {
    if (g_a11y.initialized) return;
    memset(&g_a11y, 0, sizeof(g_a11y));
    g_a11y.font_scale_x10 = 10;
    g_a11y.initialized = true;
    kprintf("[a11y] accessibility layer initialized\n");
    SLOG_INFO("a11y", "accessibility ready (font_scale=1.0)");
}

void a11y_set_mode(uint32_t mode_flags) {
    g_a11y.modes = mode_flags;
    kprintf("[a11y] modes set: 0x%x%s%s%s%s\n",
            (unsigned)mode_flags,
            (mode_flags & A11Y_MODE_HIGH_CONTRAST) ? " HIGH_CONTRAST" : "",
            (mode_flags & A11Y_MODE_SCREEN_READER) ? " SCREEN_READER" : "",
            (mode_flags & A11Y_MODE_KEYBOARD_ONLY) ? " KEYBOARD_ONLY" : "",
            (mode_flags & A11Y_MODE_LARGE_TEXT) ? " LARGE_TEXT" : "");
    SLOG_INFO("a11y", "mode changed to 0x%x", (unsigned)mode_flags);
}

uint32_t a11y_get_mode(void) {
    return g_a11y.modes;
}

bool a11y_get_contrast_mode(void) {
    return (g_a11y.modes & A11Y_MODE_HIGH_CONTRAST) != 0;
}

bool a11y_get_keyboard_only(void) {
    return (g_a11y.modes & A11Y_MODE_KEYBOARD_ONLY) != 0;
}

void a11y_set_font_scale(uint32_t scale_x10) {
    if (scale_x10 < 10) scale_x10 = 10;
    if (scale_x10 > 30) scale_x10 = 30;
    g_a11y.font_scale_x10 = scale_x10;
    kprintf("[a11y] font scale set to %u.%u\n",
            (unsigned)(scale_x10 / 10), (unsigned)(scale_x10 % 10));
    SLOG_INFO("a11y", "font scale = %u.%ux",
              (unsigned)(scale_x10 / 10), (unsigned)(scale_x10 % 10));
}

uint32_t a11y_get_font_scale(void) {
    return g_a11y.font_scale_x10;
}

void a11y_announce(const char *text) {
    if (!text) return;
    size_t len = strlen(text);
    if (len >= A11Y_ANNOUNCE_MAX) len = A11Y_ANNOUNCE_MAX - 1;
    memcpy(g_a11y.last_announcement, text, len);
    g_a11y.last_announcement[len] = '\0';

    if (g_a11y.modes & A11Y_MODE_SCREEN_READER) {
        a11y_speak(text);
    }
    SLOG_DEBUG("a11y", "announce: %s", text);
}

void a11y_speak(const char *text) {
    /* TTS placeholder: log the text that would be spoken */
    kprintf("[a11y] TTS: \"%s\"\n", text ? text : "");
    SLOG_DEBUG("a11y", "speak: %s", text ? text : "(null)");
}

void a11y_set_focused_widget(const char *description) {
    if (!description) {
        g_a11y.focused_widget[0] = '\0';
        return;
    }
    size_t len = strlen(description);
    if (len >= A11Y_WIDGET_DESC_MAX) len = A11Y_WIDGET_DESC_MAX - 1;
    memcpy(g_a11y.focused_widget, description, len);
    g_a11y.focused_widget[len] = '\0';

    if (g_a11y.modes & A11Y_MODE_SCREEN_READER) {
        a11y_speak(description);
    }
}

const char *a11y_get_focused_widget(void) {
    return g_a11y.focused_widget;
}

void a11y_update_screen_text(const char *text, size_t len) {
    if (!text || len == 0) return;
    if (len >= A11Y_TEXT_BUF_SIZE) len = A11Y_TEXT_BUF_SIZE - 1;
    memcpy(g_a11y.screen_text, text, len);
    g_a11y.screen_text[len] = '\0';
    g_a11y.screen_text_len = len;
}

const char *a11y_get_screen_text(size_t *out_len) {
    if (out_len) *out_len = g_a11y.screen_text_len;
    return g_a11y.screen_text;
}
