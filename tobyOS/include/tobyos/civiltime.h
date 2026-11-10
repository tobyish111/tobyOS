/* include/tobyos/civiltime.h -- ONE calendar for the whole tree.
 *
 * Proleptic Gregorian, 1970 epoch, UTC only (tobyOS has no time zones).
 * Header-only and freestanding, so the SAME arithmetic serves the kernel
 * (the shell's printf %(fmt)T in both of its builds) and libtoby (gmtime,
 * mktime, strftime, ctime).
 *
 * Before this existed, libtoby counted seconds from BOOT and pretended the
 * base was 2025-01-01, while the kernel counted from 1970 off the RTC. Two
 * calendars on one machine: native `date` printed 2025 on a box whose RTC
 * said 2026-09-01, and `busybox date` on the same box was right.
 *
 * The day <-> civil conversions are Howard Hinnant's: exact over the whole
 * int64 range and free of the year-by-year loops the old code used. */

#ifndef TOBYOS_CIVILTIME_H
#define TOBYOS_CIVILTIME_H

#include <stdint.h>
#include <stddef.h>

struct civil_time {
    int year;   /* the full year: 2026, not 126 */
    int mon;    /* 0..11 */
    int mday;   /* 1..31 */
    int hour;   /* 0..23 */
    int min;    /* 0..59 */
    int sec;    /* 0..60 */
    int wday;   /* 0 = Sunday */
    int yday;   /* 0..365 */
};

static inline int civil_is_leap(int64_t y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static inline int civil_days_in_month(int mon, int64_t year) {
    static const int dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mon < 0) mon = 0;
    if (mon > 11) mon = 11;
    if (mon == 1 && civil_is_leap(year)) return 29;
    return dim[mon];
}

/* Days since 1970-01-01 of y-m-d, m in 1..12. d may be out of range; it is
 * simply added, which is what a normalising mktime wants. */
static inline int64_t civil_days_from_ymd(int64_t y, int m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                    /* [0, 399]    */
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;    /* [0, 365]    */
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

static inline void civil_ymd_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;                                          /* [0, 146096] */
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;     /* [0, 399]    */
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                    /* [0, 365]    */
    const int64_t mp  = (5 * doy + 2) / 153;                                        /* [0, 11]     */
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);                                       /* [1, 31]     */
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);                                          /* [1, 12]     */
    *y = yoe + era * 400 + (*m <= 2);
}

/* Unix seconds -> broken-down UTC. Negative times (before 1970) work. */
static inline void civil_from_unix(int64_t t, struct civil_time *out) {
    int64_t days = t / 86400;
    int64_t rem  = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    int64_t y; int m, d;
    civil_ymd_from_days(days, &y, &m, &d);
    out->year = (int)y;
    out->mon  = m - 1;
    out->mday = d;
    out->hour = (int)(rem / 3600);
    out->min  = (int)(rem % 3600 / 60);
    out->sec  = (int)(rem % 60);
    int64_t wd = (days + 4) % 7;             /* 1970-01-01 was a Thursday */
    if (wd < 0) wd += 7;
    out->wday = (int)wd;
    out->yday = (int)(days - civil_days_from_ymd(y, 1, 1));
}

/* Broken-down -> Unix seconds, with mktime's contract: fields may be OUT OF
 * RANGE (month 12 is January of next year, mday 0 is the last day of the
 * previous month, hour 25 rolls over). wday and yday are ignored. */
static inline int64_t civil_to_unix(const struct civil_time *ct) {
    int64_t y   = ct->year;
    int64_t mon = ct->mon;
    int64_t yc  = mon / 12;
    mon %= 12;
    if (mon < 0) { mon += 12; yc--; }
    y += yc;
    int64_t days = civil_days_from_ymd(y, (int)mon + 1, (int64_t)ct->mday);
    return days * 86400 + (int64_t)ct->hour * 3600 + (int64_t)ct->min * 60 + ct->sec;
}

/* ISO 8601 week-numbering: 53 weeks when Jan 1 is a Thursday, or a
 * Wednesday in a leap year. */
static inline int civil_weeks_in_year(int64_t y) {
    int64_t d = civil_days_from_ymd(y, 1, 1);
    int wd = (int)(((d + 4) % 7 + 7) % 7);
    return (wd == 4 || (wd == 3 && civil_is_leap(y))) ? 53 : 52;
}

static inline void civil_iso_week(const struct civil_time *ct, int *week, int64_t *year) {
    int iso_wday = ct->wday == 0 ? 7 : ct->wday;          /* Mon = 1 .. Sun = 7 */
    int64_t y = ct->year;
    int w = (ct->yday + 1 - iso_wday + 10) / 7;
    if (w < 1) {
        y--;
        w = civil_weeks_in_year(y);
    } else if (w == 53 && civil_weeks_in_year(y) == 52) {
        w = 1;
        y++;
    }
    *week = w;
    *year = y;
}

/* ---- strftime -------------------------------------------------------- *
 *
 * The C99 set plus the GNU/busybox extras scripts actually use: the -, _, 0
 * and ^ flags, a field width, %k %l %P %s %N %q, and the %E/%O modifiers
 * (accepted, ignored -- the C locale is the only one). Returns the number of
 * bytes written excluding the NUL, or 0 if the result did not fit, exactly
 * as strftime(3) does. */

static inline int civil__emit(char *s, size_t cap, size_t *n, const char *str, int upper) {
    while (*str) {
        if (*n + 1 >= cap) return 0;
        char c = *str++;
        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        s[(*n)++] = c;
    }
    return 1;
}

/* A number with a default width and pad, both overridable by the flags. */
static inline int civil__emitnum(char *s, size_t cap, size_t *n, int64_t v,
                                 int defwidth, char defpad, char pad, int width) {
    char digits[24];
    int nd = 0;
    int neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    do { digits[nd++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    char use_pad = defpad;
    int  use_w   = defwidth;
    if (pad == '-')      { use_pad = 0; }
    else if (pad)        { use_pad = pad; }
    if (width >= 0)      { use_w = width; if (!use_pad) use_pad = defpad; }
    int body = nd + neg;
    if (use_pad) {
        int fill = use_w - body;
        /* a zero pad goes after the sign, a space pad before it */
        if (use_pad == '0' && neg) {
            if (*n + 1 >= cap) return 0;
            s[(*n)++] = '-';
            neg = 0;
        }
        while (fill-- > 0) {
            if (*n + 1 >= cap) return 0;
            s[(*n)++] = use_pad;
        }
    }
    if (neg) {
        if (*n + 1 >= cap) return 0;
        s[(*n)++] = '-';
    }
    while (nd > 0) {
        if (*n + 1 >= cap) return 0;
        s[(*n)++] = digits[--nd];
    }
    return 1;
}

static inline size_t civil_strftime(char *s, size_t cap, const char *fmt,
                                    const struct civil_time *ct) {
    static const char *const wday_name[7] = {
        "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };
    static const char *const mon_name[12] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December" };
    if (!s || cap == 0 || !fmt || !ct) return 0;
    size_t n = 0;
    while (*fmt) {
        if (*fmt != '%') {
            if (n + 1 >= cap) return 0;
            s[n++] = *fmt++;
            continue;
        }
        fmt++;
        char pad = 0; int upper = 0; int width = -1;
        for (;;) {
            if (*fmt == '-' || *fmt == '_' || *fmt == '0') { pad = *fmt == '_' ? ' ' : *fmt; fmt++; }
            else if (*fmt == '^' || *fmt == '#') { upper = 1; fmt++; }
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            if (width < 0) width = 0;
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == 'E' || *fmt == 'O') fmt++;
        char c = *fmt;
        if (!c) break;
        fmt++;

        char tmp[8];
        const char *sub = 0;              /* a composite: recurse on this */
        int ok = 1;
        int wi = ct->wday % 7; if (wi < 0) wi += 7;
        int mi = ct->mon  % 12; if (mi < 0) mi += 12;
        switch (c) {
        case 'a': tmp[0] = wday_name[wi][0]; tmp[1] = wday_name[wi][1]; tmp[2] = wday_name[wi][2]; tmp[3] = 0;
                  ok = civil__emit(s, cap, &n, tmp, upper); break;
        case 'A': ok = civil__emit(s, cap, &n, wday_name[wi], upper); break;
        case 'b': case 'h':
                  tmp[0] = mon_name[mi][0]; tmp[1] = mon_name[mi][1]; tmp[2] = mon_name[mi][2]; tmp[3] = 0;
                  ok = civil__emit(s, cap, &n, tmp, upper); break;
        case 'B': ok = civil__emit(s, cap, &n, mon_name[mi], upper); break;
        case 'C': ok = civil__emitnum(s, cap, &n, ct->year >= 0 ? ct->year / 100 : -((-ct->year + 99) / 100), 2, '0', pad, width); break;
        case 'd': ok = civil__emitnum(s, cap, &n, ct->mday, 2, '0', pad, width); break;
        case 'e': ok = civil__emitnum(s, cap, &n, ct->mday, 2, ' ', pad, width); break;
        case 'H': ok = civil__emitnum(s, cap, &n, ct->hour, 2, '0', pad, width); break;
        case 'k': ok = civil__emitnum(s, cap, &n, ct->hour, 2, ' ', pad, width); break;
        case 'I': ok = civil__emitnum(s, cap, &n, ct->hour % 12 == 0 ? 12 : ct->hour % 12, 2, '0', pad, width); break;
        case 'l': ok = civil__emitnum(s, cap, &n, ct->hour % 12 == 0 ? 12 : ct->hour % 12, 2, ' ', pad, width); break;
        case 'j': ok = civil__emitnum(s, cap, &n, ct->yday + 1, 3, '0', pad, width); break;
        case 'm': ok = civil__emitnum(s, cap, &n, ct->mon + 1, 2, '0', pad, width); break;
        case 'M': ok = civil__emitnum(s, cap, &n, ct->min, 2, '0', pad, width); break;
        case 'S': ok = civil__emitnum(s, cap, &n, ct->sec, 2, '0', pad, width); break;
        case 'N': ok = civil__emitnum(s, cap, &n, 0, 9, '0', pad, width); break;   /* no sub-second field */
        case 'p': ok = civil__emit(s, cap, &n, ct->hour < 12 ? "AM" : "PM", 0); break;
        case 'P': ok = civil__emit(s, cap, &n, ct->hour < 12 ? "am" : "pm", 0); break;
        case 'q': ok = civil__emitnum(s, cap, &n, ct->mon / 3 + 1, 1, '0', pad, width); break;
        case 's': ok = civil__emitnum(s, cap, &n, civil_to_unix(ct), 1, '0', pad, width); break;
        case 'u': ok = civil__emitnum(s, cap, &n, ct->wday == 0 ? 7 : ct->wday, 1, '0', pad, width); break;
        case 'w': ok = civil__emitnum(s, cap, &n, ct->wday, 1, '0', pad, width); break;
        case 'U': ok = civil__emitnum(s, cap, &n, (ct->yday + 7 - ct->wday) / 7, 2, '0', pad, width); break;
        case 'W': ok = civil__emitnum(s, cap, &n, (ct->yday + 7 - (ct->wday + 6) % 7) / 7, 2, '0', pad, width); break;
        case 'V': { int wk; int64_t yy; civil_iso_week(ct, &wk, &yy);
                    ok = civil__emitnum(s, cap, &n, wk, 2, '0', pad, width); break; }
        case 'G': { int wk; int64_t yy; civil_iso_week(ct, &wk, &yy);
                    ok = civil__emitnum(s, cap, &n, yy, 1, '0', pad, width); break; }
        case 'g': { int wk; int64_t yy; civil_iso_week(ct, &wk, &yy);
                    ok = civil__emitnum(s, cap, &n, ((yy % 100) + 100) % 100, 2, '0', pad, width); break; }
        case 'y': ok = civil__emitnum(s, cap, &n, ((ct->year % 100) + 100) % 100, 2, '0', pad, width); break;
        case 'Y': ok = civil__emitnum(s, cap, &n, ct->year, 1, '0', pad, width); break;
        case 'z': ok = civil__emit(s, cap, &n, "+0000", 0); break;
        case 'Z': ok = civil__emit(s, cap, &n, "UTC", 0); break;
        case 'n': ok = civil__emit(s, cap, &n, "\n", 0); break;
        case 't': ok = civil__emit(s, cap, &n, "\t", 0); break;
        case '%': ok = civil__emit(s, cap, &n, "%", 0); break;
        case 'c': sub = "%a %b %e %H:%M:%S %Y"; break;
        case 'D': case 'x': sub = "%m/%d/%y"; break;
        case 'F': sub = "%Y-%m-%d"; break;
        case 'r': sub = "%I:%M:%S %p"; break;
        case 'R': sub = "%H:%M"; break;
        case 'T': case 'X': sub = "%H:%M:%S"; break;
        default:
            /* an unknown conversion is copied through, as glibc does */
            if (n + 2 >= cap) return 0;
            s[n++] = '%';
            s[n++] = c;
            break;
        }
        if (sub) {
            char inner[64];
            size_t k = civil_strftime(inner, sizeof inner, sub, ct);
            if (k == 0 && sub[0]) return 0;
            ok = civil__emit(s, cap, &n, inner, upper);
        }
        if (!ok) return 0;
    }
    s[n] = 0;
    return n;
}

#endif /* TOBYOS_CIVILTIME_H */
