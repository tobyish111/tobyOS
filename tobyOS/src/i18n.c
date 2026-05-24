/* i18n.c -- Internationalization support.
 *
 * Provides locale system, number/date formatting, and IME skeleton.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>

#define I18N_MAX_LOCALES   8
#define I18N_LOCALE_NAME   8
#define I18N_DATE_FMT_MAX  32
#define I18N_TIME_FMT_MAX  16
#define I18N_BUF_MAX       128

struct locale {
    char name[I18N_LOCALE_NAME];
    char decimal_sep;
    char thousands_sep;
    char date_format[I18N_DATE_FMT_MAX];
    char time_format[I18N_TIME_FMT_MAX];
    bool rtl;
};

static struct locale g_locales[] = {
    { "en_US", '.', ',', "MM/DD/YYYY", "HH:MM:SS",  false },
    { "de_DE", ',', '.', "DD.MM.YYYY", "HH:MM:SS",  false },
    { "ja_JP", '.', ',', "YYYY/MM/DD", "HH:MM:SS",  false },
    { "ar_SA", '.', ',', "DD/MM/YYYY", "HH:MM:SS",  true  },
};
#define I18N_NUM_LOCALES  (sizeof(g_locales) / sizeof(g_locales[0]))

static int g_current_locale = 0;
static bool g_initialized = false;

/* IME state */
#define IME_COMPOSE_MAX  16
static struct {
    bool    active;
    char    compose_buf[IME_COMPOSE_MAX];
    int     compose_len;
} g_ime;

void i18n_init(void) {
    if (g_initialized) return;
    g_current_locale = 0;
    memset(&g_ime, 0, sizeof(g_ime));
    g_initialized = true;
    kprintf("[i18n] initialized (locale=%s)\n", g_locales[0].name);
    SLOG_INFO("i18n", "i18n ready (default=%s)", g_locales[0].name);
}

int i18n_set_locale(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < (int)I18N_NUM_LOCALES; i++) {
        if (strcmp(g_locales[i].name, name) == 0) {
            g_current_locale = i;
            kprintf("[i18n] locale set to %s\n", name);
            SLOG_INFO("i18n", "locale changed to %s", name);
            return 0;
        }
    }
    return -1;
}

const char *i18n_get_locale(void) {
    return g_locales[g_current_locale].name;
}

bool i18n_is_rtl(void) {
    return g_locales[g_current_locale].rtl;
}

int i18n_format_number(int64_t value, char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;

    const struct locale *loc = &g_locales[g_current_locale];
    char tmp[64];
    int pos = 0;
    bool negative = false;

    if (value < 0) {
        negative = true;
        value = -value;
    }

    if (value == 0) {
        tmp[pos++] = '0';
    } else {
        while (value > 0 && pos < 60) {
            tmp[pos++] = '0' + (char)(value % 10);
            value /= 10;
        }
    }

    /* Reverse with thousands separator */
    size_t out = 0;
    if (negative && out < cap - 1) buf[out++] = '-';

    int digit_count = 0;
    for (int i = pos - 1; i >= 0 && out < cap - 1; i--) {
        if (digit_count > 0 && digit_count % 3 == 0 && out < cap - 1) {
            buf[out++] = loc->thousands_sep;
        }
        buf[out++] = tmp[i];
        digit_count++;
    }
    buf[out] = '\0';
    return (int)out;
}

int i18n_format_decimal(int64_t integer_part, int frac_digits,
                        char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;

    const struct locale *loc = &g_locales[g_current_locale];
    int n = i18n_format_number(integer_part, buf, cap);
    if (n < 0) return -1;

    size_t out = (size_t)n;
    if (frac_digits > 0 && out < cap - 1) {
        buf[out++] = loc->decimal_sep;
        for (int i = 0; i < frac_digits && out < cap - 1; i++) {
            buf[out++] = '0';
        }
    }
    buf[out] = '\0';
    return (int)out;
}

int i18n_format_date(uint32_t year, uint32_t month, uint32_t day,
                     char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;

    const struct locale *loc = &g_locales[g_current_locale];
    const char *fmt = loc->date_format;

    /* Simple template replacement */
    size_t out = 0;
    const char *p = fmt;
    while (*p && out < cap - 1) {
        if (p[0] == 'Y' && p[1] == 'Y' && p[2] == 'Y' && p[3] == 'Y') {
            out += (size_t)ksnprintf(buf + out, cap - out, "%04u", year);
            p += 4;
        } else if (p[0] == 'M' && p[1] == 'M') {
            out += (size_t)ksnprintf(buf + out, cap - out, "%02u", month);
            p += 2;
        } else if (p[0] == 'D' && p[1] == 'D') {
            out += (size_t)ksnprintf(buf + out, cap - out, "%02u", day);
            p += 2;
        } else {
            buf[out++] = *p++;
        }
    }
    buf[out] = '\0';
    return (int)out;
}

int i18n_format_time(uint32_t hour, uint32_t min, uint32_t sec,
                     char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    return ksnprintf(buf, cap, "%02u:%02u:%02u", hour, min, sec);
}

/* IME skeleton */
void ime_init(void) {
    memset(&g_ime, 0, sizeof(g_ime));
    kprintf("[i18n] IME initialized\n");
}

const char *ime_input(uint32_t keycode) {
    /* Basic compose: store keycode as ASCII in compose buffer */
    if (keycode == 0) {
        /* Commit */
        g_ime.compose_buf[g_ime.compose_len] = '\0';
        g_ime.compose_len = 0;
        return g_ime.compose_buf;
    }

    if (keycode == 0x1B) {
        /* Escape: cancel */
        g_ime.compose_len = 0;
        g_ime.compose_buf[0] = '\0';
        return "";
    }

    if (g_ime.compose_len < IME_COMPOSE_MAX - 1 && keycode < 128) {
        g_ime.compose_buf[g_ime.compose_len++] = (char)keycode;
        g_ime.compose_buf[g_ime.compose_len] = '\0';
    }

    return NULL;
}

void ime_set_active(bool active) {
    g_ime.active = active;
    if (!active) {
        g_ime.compose_len = 0;
        g_ime.compose_buf[0] = '\0';
    }
}

bool ime_is_active(void) {
    return g_ime.active;
}
