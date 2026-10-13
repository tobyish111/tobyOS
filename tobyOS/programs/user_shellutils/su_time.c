/* su_time.c -- time for date and touch, as GNU's (logs/utilgate/date.sh,
 * touch.sh): civil-date arithmetic for any year, POSIX TZ strings, GNU's
 * date-string parser (parse_datetime) and its strftime (fprintftime).
 *
 * TZ: tobyOS has no zoneinfo files, so TZ is a POSIX string -- "UTC0",
 * "EST5EDT", "CET-1CEST,M3.5.0,M10.5.0/3", "<+0530>-5:30" -- with glibc's
 * defaults: no rules means the US rules (M3.2.0,M11.1.0), a name with no
 * offset is UTC under that name ("America/X" is "America"), TZ= empty is
 * "Universal", and no TZ at all is UTC (no /etc/localtime here).
 *
 * The parser takes what GNU's grammar takes, measured case by case: ISO
 * dates and times with a T, US m/d/y, "3 Feb 2021", "Feb 3, 2021",
 * hh:mm[:ss[.frac]] with am/pm, a zone (an abbreviation, a military
 * letter, +hhmm, UTC+3, a sign right after a time), a day of the week
 * (with an ordinal: "next fri", "2 sat"), relative items ("1 day ago",
 * "last year", "tomorrow", "+3 hours", "fortnight"), pure numbers (hhmm,
 * yyyymmdd), "@SECONDS", and a leading TZ="..." for the parse alone. One
 * date, one time, one zone and one day at most; a weekday next to a date is
 * ignored; with a date or a day and no time it is midnight, with only
 * relative items the current time. */

/* ---- civil dates (any year) ------------------------------------------- */

static int64_t tx_floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/* days from 1970-01-01 to Y-M-D (proleptic Gregorian) */
static int64_t tx_days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = tx_floordiv(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void tx_civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    int64_t era = tx_floordiv(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}

static int tx_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int tx_mdays(int64_t y, int m) {
    static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && tx_leap(y) ? 29 : md[m - 1];
}

struct tx_tm {
    int64_t year;            /* the year itself, 2021 */
    int mon, mday;           /* 1..12, 1..31 */
    int hour, min, sec;
    int wday, yday;          /* 0 Sunday; 0-based */
    long nsec;
    long gmtoff;             /* seconds east */
    int isdst;
    char zone[40];
};

static void tx_breakdown(int64_t t, long off, struct tx_tm *tm) {
    int64_t s = t + off;
    int64_t days = tx_floordiv(s, 86400);
    int64_t rem = s - days * 86400;
    tx_civil_from_days(days, &tm->year, &tm->mon, &tm->mday);
    tm->hour = (int)(rem / 3600);
    tm->min = (int)(rem % 3600 / 60);
    tm->sec = (int)(rem % 60);
    tm->wday = (int)(((days % 7) + 11) % 7);          /* 1970-01-01 was a Thursday */
    tm->yday = (int)(days - tx_days_from_civil(tm->year, 1, 1));
    tm->gmtoff = off;
}

/* ---- POSIX TZ ------------------------------------------------------------ */

struct tx_rule { char kind; int m, w, d, n; long time; };

struct tx_tz {
    char std[40], dst[40];
    long std_off, dst_off;     /* seconds EAST of UTC */
    int has_dst;
    struct tx_rule start, end;
};

static int tx_tz_name(const char **pp, char *out, size_t cap) {
    const char *p = *pp;
    size_t n = 0;
    if (*p == '<') {
        p++;
        while (*p && *p != '>') { if (n + 1 < cap) out[n++] = *p; p++; }
        if (*p != '>') return 0;
        p++;
    } else {
        while (isalpha((unsigned char)*p)) { if (n + 1 < cap) out[n++] = *p; p++; }
        if (n < 3) return 0;
    }
    out[n] = 0;
    *pp = p;
    return 1;
}

/* [+-]hh[:mm[:ss]] in seconds; *ok cleared on a malformed one */
static long tx_tz_hms(const char **pp, int *ok) {
    const char *p = *pp;
    int sign = 1;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    if (!isdigit((unsigned char)*p)) { *ok = 0; return 0; }
    long v = 0, part = 0;
    while (isdigit((unsigned char)*p)) part = part * 10 + (*p++ - '0');
    v = part * 3600;
    if (*p == ':') {
        p++; part = 0;
        while (isdigit((unsigned char)*p)) part = part * 10 + (*p++ - '0');
        v += part * 60;
        if (*p == ':') {
            p++; part = 0;
            while (isdigit((unsigned char)*p)) part = part * 10 + (*p++ - '0');
            v += part;
        }
    }
    *pp = p;
    return sign * v;
}

static int tx_tz_rule(const char **pp, struct tx_rule *r) {
    const char *p = *pp;
    memset(r, 0, sizeof *r);
    r->time = 7200;
    if (*p == 'M') {
        r->kind = 'M';
        p++;
        r->m = (int)strtol(p, (char **)&p, 10);
        if (*p++ != '.') return 0;
        r->w = (int)strtol(p, (char **)&p, 10);
        if (*p++ != '.') return 0;
        r->d = (int)strtol(p, (char **)&p, 10);
        if (r->m < 1 || r->m > 12 || r->w < 1 || r->w > 5 || r->d < 0 || r->d > 6) return 0;
    } else if (*p == 'J') {
        r->kind = 'J';
        p++;
        r->n = (int)strtol(p, (char **)&p, 10);
    } else if (isdigit((unsigned char)*p)) {
        r->kind = 'D';
        r->n = (int)strtol(p, (char **)&p, 10);
    } else
        return 0;
    if (*p == '/') {
        p++;
        int ok = 1;
        r->time = tx_tz_hms(&p, &ok);
        if (!ok) return 0;
    }
    *pp = p;
    return 1;
}

static void tx_tz_set(struct tx_tz *z, const char *tz) {
    memset(z, 0, sizeof *z);
    if (!tz) { strcpy(z->std, "UTC"); return; }
    if (!*tz) { strcpy(z->std, "Universal"); return; }
    if (*tz == ':') tz++;
    const char *p = tz;
    int ok = 1;
    if (tx_tz_name(&p, z->std, sizeof z->std) && (*p == '+' || *p == '-' || isdigit((unsigned char)*p))) {
        z->std_off = -tx_tz_hms(&p, &ok);
        if (ok && *p) {
            if (tx_tz_name(&p, z->dst, sizeof z->dst)) {
                z->has_dst = 1;
                z->dst_off = z->std_off + 3600;
                if (*p == '+' || *p == '-' || isdigit((unsigned char)*p)) z->dst_off = -tx_tz_hms(&p, &ok);
                if (ok && *p == ',') {
                    p++;
                    ok = tx_tz_rule(&p, &z->start) && *p++ == ',' && tx_tz_rule(&p, &z->end);
                } else if (ok && !*p) {
                    z->start = (struct tx_rule){ 'M', 3, 2, 0, 0, 7200 };
                    z->end = (struct tx_rule){ 'M', 11, 1, 0, 0, 7200 };
                }
                if (!ok || *p) z->has_dst = 0;
            }
        }
        if (ok) return;
    }
    /* a name with no zone data: UTC, called by the name's letters */
    memset(z, 0, sizeof *z);
    size_t n = 0;
    for (const char *q = tz; isalpha((unsigned char)*q) && n + 1 < sizeof z->std; q++) z->std[n++] = *q;
    z->std[n] = 0;
    if (!n) strcpy(z->std, "UTC");
}

/* the instant (UTC) rule R takes effect in YEAR, OFF the offset before it */
static int64_t tx_rule_utc(const struct tx_rule *r, int64_t year, long off) {
    int64_t day;
    if (r->kind == 'J') day = tx_days_from_civil(year, 1, 1) + r->n - 1 + (tx_leap(year) && r->n >= 60);
    else if (r->kind == 'D') day = tx_days_from_civil(year, 1, 1) + r->n;
    else {
        int64_t first = tx_days_from_civil(year, r->m, 1);
        int wd = (int)(((first % 7) + 11) % 7);
        int64_t dd = first + ((r->d - wd + 7) % 7) + 7 * (r->w - 1);
        while (dd >= first + tx_mdays(year, r->m)) dd -= 7;
        day = dd;
    }
    return day * 86400 + r->time - off;
}

static int tx_tz_isdst(const struct tx_tz *z, int64_t t) {
    if (!z->has_dst) return 0;
    int64_t y;
    int m, d;
    tx_civil_from_days(tx_floordiv(t + z->std_off, 86400), &y, &m, &d);
    int64_t s = tx_rule_utc(&z->start, y, z->std_off), e = tx_rule_utc(&z->end, y, z->dst_off);
    if (s < e) return t >= s && t < e;
    return !(t >= e && t < s);
}

static void tx_tz_local(const struct tx_tz *z, int64_t t, long ns, struct tx_tm *tm) {
    int dst = tx_tz_isdst(z, t);
    tx_breakdown(t, dst ? z->dst_off : z->std_off, tm);
    tm->isdst = dst;
    tm->nsec = ns;
    snprintf(tm->zone, sizeof tm->zone, "%s", dst ? z->dst : z->std);
}

/* local wall time -> UTC: ISDST < 0 lets the zone decide */
static int64_t tx_tz_mktime(const struct tx_tz *z, int64_t local, int isdst) {
    if (!z->has_dst) return local - z->std_off;
    if (isdst > 0) return local - z->dst_off;
    if (isdst == 0) return local - z->std_off;
    int64_t t1 = local - z->std_off;
    if (!tx_tz_isdst(z, t1)) return t1;
    int64_t t2 = local - z->dst_off;
    return tx_tz_isdst(z, t2) ? t2 : t1;
}

static struct tx_tz tx_tz_env;
static int tx_tz_loaded;

static const struct tx_tz *tx_tz_cur(void) {
    if (!tx_tz_loaded) { tx_tz_set(&tx_tz_env, getenv("TZ")); tx_tz_loaded = 1; }
    return &tx_tz_env;
}

/* ---- the clock ------------------------------------------------------------ */

static void tx_now(int64_t *s, long *ns) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *s = (int64_t)ts.tv_sec;
    *ns = ts.tv_nsec;
}

/* ---- GNU's date strings --------------------------------------------------- */

enum { TK_END, TK_NUM, TK_SNUM, TK_DEC, TK_SDEC, TK_WORD, TK_CHAR };

struct tx_tok {
    int kind;
    int64_t v;         /* NUM/SNUM: the value (signed); DEC: whole part */
    long frac;         /* DEC: nanoseconds */
    int digits;
    int ch;            /* CHAR */
    char w[32];        /* WORD, lowercased */
    int neg;           /* SDEC */
};

enum { W_NONE, W_MONTH, W_DAY, W_MERID, W_UNIT, W_AGO, W_SHIFT, W_ORD, W_ZONE, W_DAYZONE,
       W_DST, W_MIL, W_LOCAL };

struct tx_word { const char *s; int kind; long v; };

static const struct tx_word tx_words[] = {
    { "january", W_MONTH, 1 }, { "february", W_MONTH, 2 }, { "march", W_MONTH, 3 },
    { "april", W_MONTH, 4 }, { "may", W_MONTH, 5 }, { "june", W_MONTH, 6 },
    { "july", W_MONTH, 7 }, { "august", W_MONTH, 8 }, { "september", W_MONTH, 9 },
    { "sept", W_MONTH, 9 }, { "october", W_MONTH, 10 }, { "november", W_MONTH, 11 },
    { "december", W_MONTH, 12 },
    { "sunday", W_DAY, 0 }, { "monday", W_DAY, 1 }, { "tuesday", W_DAY, 2 }, { "tues", W_DAY, 2 },
    { "wednesday", W_DAY, 3 }, { "wednes", W_DAY, 3 }, { "thursday", W_DAY, 4 },
    { "thur", W_DAY, 4 }, { "thurs", W_DAY, 4 }, { "friday", W_DAY, 5 }, { "saturday", W_DAY, 6 },
    { "am", W_MERID, 1 }, { "a.m.", W_MERID, 1 }, { "pm", W_MERID, 2 }, { "p.m.", W_MERID, 2 },
    { "year", W_UNIT, 'Y' }, { "month", W_UNIT, 'M' }, { "fortnight", W_UNIT, 'F' },
    { "week", W_UNIT, 'W' }, { "day", W_UNIT, 'D' }, { "hour", W_UNIT, 'h' },
    { "minute", W_UNIT, 'm' }, { "min", W_UNIT, 'm' }, { "second", W_UNIT, 's' },
    { "sec", W_UNIT, 's' },
    { "ago", W_AGO, 0 }, { "tomorrow", W_SHIFT, 1 }, { "yesterday", W_SHIFT, -1 },
    { "today", W_SHIFT, 0 }, { "now", W_SHIFT, 0 },
    { "last", W_ORD, -1 }, { "this", W_ORD, 0 }, { "next", W_ORD, 1 }, { "first", W_ORD, 1 },
    { "third", W_ORD, 3 }, { "fourth", W_ORD, 4 }, { "fifth", W_ORD, 5 }, { "sixth", W_ORD, 6 },
    { "seventh", W_ORD, 7 }, { "eighth", W_ORD, 8 }, { "ninth", W_ORD, 9 }, { "tenth", W_ORD, 10 },
    { "eleventh", W_ORD, 11 }, { "twelfth", W_ORD, 12 },
    { "dst", W_DST, 0 },
    { "gmt", W_ZONE, 0 }, { "ut", W_ZONE, 0 }, { "utc", W_ZONE, 0 },
    { "wet", W_ZONE, 0 }, { "west", W_DAYZONE, 0 }, { "bst", W_DAYZONE, 0 },
    { "art", W_ZONE, -3 * 3600 }, { "brt", W_ZONE, -3 * 3600 }, { "brst", W_DAYZONE, -3 * 3600 },
    { "nst", W_ZONE, -(3 * 3600 + 1800) }, { "ndt", W_DAYZONE, -(3 * 3600 + 1800) },
    { "ast", W_ZONE, -4 * 3600 }, { "adt", W_DAYZONE, -4 * 3600 },
    { "clt", W_ZONE, -4 * 3600 }, { "clst", W_DAYZONE, -4 * 3600 },
    { "est", W_ZONE, -5 * 3600 }, { "edt", W_DAYZONE, -5 * 3600 },
    { "cst", W_ZONE, -6 * 3600 }, { "cdt", W_DAYZONE, -6 * 3600 },
    { "mst", W_ZONE, -7 * 3600 }, { "mdt", W_DAYZONE, -7 * 3600 },
    { "pst", W_ZONE, -8 * 3600 }, { "pdt", W_DAYZONE, -8 * 3600 },
    { "akst", W_ZONE, -9 * 3600 }, { "akdt", W_DAYZONE, -9 * 3600 },
    { "hst", W_ZONE, -10 * 3600 }, { "hast", W_ZONE, -10 * 3600 }, { "hadt", W_DAYZONE, -10 * 3600 },
    { "sst", W_ZONE, -12 * 3600 },
    { "wat", W_ZONE, 3600 }, { "cet", W_ZONE, 3600 }, { "cest", W_DAYZONE, 3600 },
    { "met", W_ZONE, 3600 }, { "mez", W_ZONE, 3600 }, { "mest", W_DAYZONE, 3600 },
    { "mesz", W_DAYZONE, 3600 }, { "eet", W_ZONE, 2 * 3600 }, { "eest", W_DAYZONE, 2 * 3600 },
    { "cat", W_ZONE, 2 * 3600 }, { "sast", W_ZONE, 2 * 3600 }, { "eat", W_ZONE, 3 * 3600 },
    { "msk", W_ZONE, 3 * 3600 }, { "msd", W_DAYZONE, 3 * 3600 }, { "ist", W_ZONE, 5 * 3600 + 1800 },
    { "sgt", W_ZONE, 8 * 3600 }, { "kst", W_ZONE, 9 * 3600 }, { "jst", W_ZONE, 9 * 3600 },
    { "gst", W_ZONE, 10 * 3600 }, { "nzst", W_ZONE, 12 * 3600 }, { "nzdt", W_DAYZONE, 12 * 3600 },
    { 0, 0, 0 }
};

/* a word's meaning: names, their three-letter forms, plural units, an
 * abbreviation's trailing '.', military letters; LZ the local zone names */
static int tx_lookup(const char *w, long *v, const struct tx_tz *z) {
    char b[32];
    size_t n = strlen(w);
    if (n >= sizeof b) return W_NONE;
    memcpy(b, w, n + 1);
    /* the universal names first, as GNU looks them up: under TZ=UTC0,
     * "UTC+3" is still UTC with an offset */
    if (!strcmp(b, "gmt") || !strcmp(b, "ut") || !strcmp(b, "utc")) { *v = 0; return W_ZONE; }
    if (z->std[0] && !strcasecmp(b, z->std)) { *v = 0; return W_LOCAL; }
    if (z->has_dst && !strcasecmp(b, z->dst)) { *v = 1; return W_LOCAL; }
    for (int i = 0; tx_words[i].s; i++)
        if (!strcmp(b, tx_words[i].s)) { *v = tx_words[i].v; return tx_words[i].kind; }
    if (n > 1 && b[n - 1] == '.') b[--n] = 0;                  /* "feb." */
    if (n == 3)
        for (int i = 0; tx_words[i].s; i++)
            if ((tx_words[i].kind == W_MONTH || tx_words[i].kind == W_DAY) &&
                !strncmp(b, tx_words[i].s, 3)) { *v = tx_words[i].v; return tx_words[i].kind; }
    if (n > 1 && b[n - 1] == 's') {                              /* "days" */
        b[n - 1] = 0;
        for (int i = 0; tx_words[i].s; i++)
            if (tx_words[i].kind == W_UNIT && !strcmp(b, tx_words[i].s)) { *v = tx_words[i].v; return W_UNIT; }
    }
    if (n == 1 && b[0] != 'j' && b[0] >= 'a' && b[0] <= 'z') {   /* military */
        int c = b[0];
        if (c == 'z') *v = 0;
        else if (c <= 'i') *v = (c - 'a' + 1) * 3600;
        else if (c <= 'm') *v = (c - 'k' + 10) * 3600;
        else *v = -(c - 'n' + 1) * 3600;
        return W_MIL;
    }
    if (n == 1 && b[0] == 'j') return W_MIL + 100;              /* J: local time */
    return W_NONE;
}

static int tx_lex(const char *s, struct tx_tok *out, int cap) {
    int n = 0;
    const char *p = s;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '(') {                                     /* (comments) */
            int depth = 0;
            do {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                else if (!*p) return -1;
                p++;
            } while (depth > 0);
            continue;
        }
        if (n >= cap - 1) return -1;
        struct tx_tok *t = &out[n];
        memset(t, 0, sizeof *t);
        if (!*p) { t->kind = TK_END; return n + 1; }
        if (*p == '+' || *p == '-') {
            /* a sign: blanks may follow it; one not before a digit is
             * skipped, as GNU's lexer skips it ("3-feb-2021") */
            const char *q = p + 1;
            while (isspace((unsigned char)*q)) q++;
            if (!isdigit((unsigned char)*q)) { p++; continue; }
        }
        if (isdigit((unsigned char)*p) || *p == '+' || *p == '-') {
            int neg = *p == '-', sign = *p == '+' || *p == '-';
            if (sign) { p++; while (isspace((unsigned char)*p)) p++; }
            int64_t v = 0;
            int dg = 0;
            while (isdigit((unsigned char)*p)) {
                if (v > INT64_MAX / 10 - 10) return -1;
                v = v * 10 + (*p++ - '0');
                dg++;
            }
            if ((*p == '.' || *p == ',') && isdigit((unsigned char)p[1])) {
                p++;
                long frac = 0;
                int fd = 0;
                while (isdigit((unsigned char)*p)) {
                    if (fd < 9) { frac = frac * 10 + (*p - '0'); fd++; }
                    else if (fd++ >= 9 && 0) {}
                    p++;
                }
                if (fd > 9) return -1;                       /* more than ns */
                while (fd < 9) { frac *= 10; fd++; }
                t->kind = sign ? TK_SDEC : TK_DEC;
                t->v = v;
                t->frac = frac;
                t->neg = neg;
            } else {
                t->kind = sign ? TK_SNUM : TK_NUM;
                t->v = neg ? -v : v;
                t->digits = dg;
            }
            n++;
            continue;
        }
        if (isalpha((unsigned char)*p)) {
            size_t k = 0;
            while ((isalpha((unsigned char)*p) || *p == '.') && k + 1 < sizeof t->w) {
                t->w[k++] = (char)tolower((unsigned char)*p);
                p++;
            }
            t->w[k] = 0;
            /* 'T' between an ISO date and a time */
            if (k == 1 && t->w[0] == 't' && isdigit((unsigned char)*p)) { t->kind = TK_CHAR; t->ch = 'T'; }
            else t->kind = TK_WORD;
            n++;
            continue;
        }
        t->kind = TK_CHAR;
        t->ch = *p++;
        n++;
    }
}

struct tx_pd {
    int64_t year; int year_digits; int month, day;
    int hour, minutes, seconds; long nsec; int merid;     /* 0 24h, 1 am, 2 pm */
    long zone; int zones, local_isdst, locals;
    int dates, times, days, rels;
    int dow, dow_ord;
    int64_t ry, rmo, rd, rh, rmi, rs; long rns;
};

/* +hh, +hhmm, +hh:mm as seconds east; 0 for none taken; -1 bad */
static int tx_zone_offset(struct tx_tok *t, int *i, long *off) {
    struct tx_tok *a = &t[*i];
    if (a->kind != TK_SNUM) return 0;
    int64_t v = a->v < 0 ? -a->v : a->v;
    int neg = a->v < 0;
    long hh, mm;
    if (t[*i + 1].kind == TK_CHAR && t[*i + 1].ch == ':' && t[*i + 2].kind == TK_NUM) {
        if (a->digits > 2 || t[*i + 2].digits != 2) return -1;
        hh = (long)v;
        mm = (long)t[*i + 2].v;
        *i += 3;
    } else {
        if (a->digits <= 2) { hh = (long)v; mm = 0; }
        else if (a->digits <= 4) { hh = (long)(v / 100); mm = (long)(v % 100); }
        else return -1;
        *i += 1;
    }
    if (hh > 24 || mm > 59) return -1;
    *off = (neg ? -1 : 1) * (hh * 3600 + mm * 60);
    return 1;
}

static int tx_rel_unit(struct tx_pd *pd, long unit, int64_t n, long ns) {
    switch (unit) {
    case 'Y': pd->ry += n; break;
    case 'M': pd->rmo += n; break;
    case 'F': pd->rd += 14 * n; break;
    case 'W': pd->rd += 7 * n; break;
    case 'D': pd->rd += n; break;
    case 'h': pd->rh += n; break;
    case 'm': pd->rmi += n; break;
    case 's': pd->rs += n; pd->rns += ns; break;
    default: return 0;
    }
    pd->rels++;
    return 1;
}

/* hh[:mm[:ss[.frac]]] [am|pm] [zone offset]; 0 not a time, 1 one, -1 bad */
static int tx_time(struct tx_tok *t, int *i, struct tx_pd *pd, const struct tx_tz *z, int iso) {
    int k = *i;
    if (t[k].kind != TK_NUM) return 0;
    int hour = (int)t[k].v, min = 0, sec = 0;
    long ns = 0;
    int have_colon = t[k + 1].kind == TK_CHAR && t[k + 1].ch == ':';
    long v;
    int merid = 0;
    if (have_colon) {
        if (t[k + 2].kind != TK_NUM) return -1;
        min = (int)t[k + 2].v;
        k += 3;
        if (t[k].kind == TK_CHAR && t[k].ch == ':') {
            if (t[k + 1].kind == TK_NUM) { sec = (int)t[k + 1].v; k += 2; }
            else if (t[k + 1].kind == TK_DEC) { sec = (int)t[k + 1].v; ns = t[k + 1].frac; k += 2; }
            else return -1;
        }
    } else {
        /* a bare number is a time only with am/pm */
        if (t[k + 1].kind != TK_WORD || tx_lookup(t[k + 1].w, &v, z) != W_MERID) return 0;
        k += 1;
    }
    if (t[k].kind == TK_WORD && tx_lookup(t[k].w, &v, z) == W_MERID) {
        merid = (int)v;
        k++;
    }
    if (merid) {
        if (hour < 1 || hour > 12) return -1;
        hour = hour % 12 + (merid == 2 ? 12 : 0);
    }
    if (hour > 23 || min > 59 || sec > 59) return -1;
    if (iso && !have_colon) return -1;
    pd->hour = hour;
    pd->minutes = min;
    pd->seconds = sec;
    pd->nsec = ns;
    pd->times++;
    /* a sign straight after a time is its zone ("04:05 +0200", "+2") */
    if (!merid) {
        long off;
        int r = tx_zone_offset(t, &k, &off);
        if (r < 0) return -1;
        if (r) { pd->zone = off; pd->zones++; }
    }
    *i = k;
    return 1;
}

static int tx_parse_items(struct tx_tok *t, struct tx_pd *pd, const struct tx_tz *z) {
    int i = 0;
    long v, v2;
    while (t[i].kind != TK_END) {
        struct tx_tok *a = &t[i], *b = &t[i + 1], *c = &t[i + 2];
        int ka = a->kind == TK_WORD ? tx_lookup(a->w, &v, z) : W_NONE;
        /* ISO 8601 date: 2021-02-03 [T time] */
        if (a->kind == TK_NUM && b->kind == TK_SNUM && b->v <= 0 && c->kind == TK_SNUM && c->v <= 0 &&
            b->digits <= 2 && c->digits <= 2 && b->v != 0 && c->v != 0 && b->digits && c->digits) {
            pd->year = a->v; pd->year_digits = a->digits;
            pd->month = (int)-b->v; pd->day = (int)-c->v;
            pd->dates++;
            i += 3;
            if (t[i].kind == TK_CHAR && t[i].ch == 'T') {
                i++;
                int r = tx_time(t, &i, pd, z, 1);
                if (r != 1) return 0;
                if (t[i].kind == TK_WORD) {           /* "Z" */
                    int kz = tx_lookup(t[i].w, &v, z);
                    if (kz == W_MIL || kz == W_ZONE) { pd->zone = v; pd->zones++; i++; }
                }
            }
            continue;
        }
        /* times: hh:mm..., Npm */
        if (a->kind == TK_NUM) {
            int r = tx_time(t, &i, pd, z, 0);
            if (r < 0) return 0;
            if (r > 0) continue;
        }
        /* m/d[/y] */
        if (a->kind == TK_NUM && b->kind == TK_CHAR && b->ch == '/' && c->kind == TK_NUM) {
            if (t[i + 3].kind == TK_CHAR && t[i + 3].ch == '/' && t[i + 4].kind == TK_NUM) {
                if (a->digits >= 3) {
                    pd->year = a->v; pd->year_digits = a->digits;
                    pd->month = (int)c->v; pd->day = (int)t[i + 4].v;
                } else {
                    pd->month = (int)a->v; pd->day = (int)c->v;
                    pd->year = t[i + 4].v; pd->year_digits = t[i + 4].digits;
                }
                i += 5;
            } else {
                pd->month = (int)a->v; pd->day = (int)c->v;
                i += 3;
            }
            pd->dates++;
            continue;
        }
        /* d month [y], d-mon-y */
        if (a->kind == TK_NUM && b->kind == TK_WORD && tx_lookup(b->w, &v2, z) == W_MONTH) {
            pd->day = (int)a->v; pd->month = (int)v2;
            i += 2;
            if (t[i].kind == TK_SNUM && t[i].v < 0) { pd->year = -t[i].v; pd->year_digits = t[i].digits; i++; }
            else if (t[i].kind == TK_NUM && !(t[i + 1].kind == TK_CHAR && t[i + 1].ch == ':')) {
                pd->year = t[i].v; pd->year_digits = t[i].digits; i++;
            }
            pd->dates++;
            continue;
        }
        if (ka == W_MONTH) {
            pd->month = (int)v;
            i++;
            if (t[i].kind == TK_SNUM && t[i].v < 0 && t[i + 1].kind == TK_SNUM && t[i + 1].v < 0) {
                pd->day = (int)-t[i].v;
                pd->year = -t[i + 1].v; pd->year_digits = t[i + 1].digits;
                i += 2;
            } else if (t[i].kind == TK_NUM && !(t[i + 1].kind == TK_CHAR && t[i + 1].ch == ':')) {
                pd->day = (int)t[i].v;
                i++;
                if (t[i].kind == TK_CHAR && t[i].ch == ',' && t[i + 1].kind == TK_NUM) {
                    pd->year = t[i + 1].v; pd->year_digits = t[i + 1].digits;
                    i += 2;
                } else if (t[i].kind == TK_NUM && t[i].digits >= 3 &&
                           !(t[i + 1].kind == TK_CHAR && t[i + 1].ch == ':')) {
                    pd->year = t[i].v; pd->year_digits = t[i].digits;
                    i++;
                }
            } else
                return 0;
            pd->dates++;
            continue;
        }
        /* days of the week: [ordinal|N] day [,] */
        if (ka == W_DAY || ((ka == W_ORD || a->kind == TK_NUM) && b->kind == TK_WORD &&
                            tx_lookup(b->w, &v2, z) == W_DAY)) {
            if (ka == W_DAY) { pd->dow = (int)v; pd->dow_ord = 0; i++; }
            else { pd->dow = (int)v2; pd->dow_ord = (int)(ka == W_ORD ? v : a->v); i += 2; }
            if (t[i].kind == TK_CHAR && t[i].ch == ',') i++;
            pd->days++;
            continue;
        }
        /* relative: [N|ordinal|signed] unit [ago] */
        {
            int64_t n = 1;
            long ns = 0;
            int k = i, have = 0;
            if (a->kind == TK_NUM || a->kind == TK_SNUM || ka == W_ORD) {
                n = ka == W_ORD ? v : a->v;
                k++;
                have = 1;
            } else if (a->kind == TK_DEC || a->kind == TK_SDEC) {
                n = a->v * (a->neg ? -1 : 1);
                ns = a->frac * (a->neg ? -1 : 1);
                k++;
                have = 2;
            }
            long unit;
            if (t[k].kind == TK_WORD && tx_lookup(t[k].w, &unit, z) == W_UNIT) {
                if (have == 2 && unit != 's') return 0;
                k++;
                if (t[k].kind == TK_WORD && tx_lookup(t[k].w, &v2, z) == W_AGO) { n = -n; ns = -ns; k++; }
                tx_rel_unit(pd, unit, n, ns);
                i = k;
                continue;
            }
            if (have == 2) return 0;
            if (have && ka == W_ORD) return 0;
        }
        if (ka == W_SHIFT) { tx_rel_unit(pd, 'D', v, 0); i++; continue; }
        /* zones */
        if (ka == W_LOCAL) {
            pd->local_isdst = (int)v;
            pd->locals++;
            i++;
            if (t[i].kind == TK_WORD && tx_lookup(t[i].w, &v2, z) == W_DST) { pd->local_isdst = 1; i++; }
            continue;
        }
        if (ka == W_ZONE || ka == W_MIL || ka == W_DAYZONE) {
            long off = v + (ka == W_DAYZONE ? 3600 : 0);
            i++;
            if (ka == W_ZONE && t[i].kind == TK_WORD && tx_lookup(t[i].w, &v2, z) == W_DST) { off += 3600; i++; }
            else if (t[i].kind == TK_SNUM) {                       /* UTC+3 */
                long o2;
                int r = tx_zone_offset(t, &i, &o2);
                if (r < 0) return 0;
                off += o2;
            }
            pd->zone = off;
            pd->zones++;
            continue;
        }
        if (ka == W_MIL + 100) { i++; continue; }                  /* J */
        /* a pure number: a year, a date or a time by its length */
        if (a->kind == TK_NUM) {
            if (pd->dates && !pd->year_digits && !pd->rels && (pd->times || a->digits > 2)) {
                pd->year = a->v; pd->year_digits = a->digits;
            } else if (a->digits > 4) {
                pd->day = (int)(a->v % 100);
                pd->month = (int)(a->v / 100 % 100);
                pd->year = a->v / 10000; pd->year_digits = a->digits - 4;
                pd->dates++;
            } else {
                if (a->digits <= 2) { pd->hour = (int)a->v; pd->minutes = 0; }
                else { pd->hour = (int)(a->v / 100); pd->minutes = (int)(a->v % 100); }
                pd->seconds = 0; pd->nsec = 0;
                if (pd->hour > 23 || pd->minutes > 59) return 0;
                pd->times++;
            }
            i++;
            continue;
        }
        return 0;
    }
    return 1;
}

/* GNU's parse_datetime: S into *SEC and *NS; NOW the reference. 0 if invalid. */
static int tx_parse_date(const char *s, int64_t now_s, long now_ns, int64_t *sec, long *ns) {
    const struct tx_tz *z = tx_tz_cur();
    struct tx_tz tzp;
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    /* TZ="..." for this string alone */
    if (!strncmp(p, "TZ=\"", 4)) {
        char buf[128];
        size_t n = 0;
        const char *q = p + 4;
        for (; *q && *q != '"'; q++) {
            if (*q == '\\' && q[1]) q++;
            if (n + 1 < sizeof buf) buf[n++] = *q;
        }
        if (*q != '"') return 0;
        buf[n] = 0;
        tx_tz_set(&tzp, buf);
        z = &tzp;
        p = q + 1;
    }
    struct tx_tok toks[128];
    if (*p == '@') {                                     /* @SECONDS */
        int nt = tx_lex(p + 1, toks, 128);
        if (nt != 2) return 0;
        struct tx_tok *a = &toks[0];
        if (a->kind == TK_NUM || a->kind == TK_SNUM) { *sec = a->v; *ns = 0; return 1; }
        if (a->kind == TK_DEC || a->kind == TK_SDEC) {
            if (a->neg) {
                *sec = -a->v - (a->frac ? 1 : 0);
                *ns = a->frac ? 1000000000L - a->frac : 0;
            } else { *sec = a->v; *ns = a->frac; }
            return 1;
        }
        return 0;
    }
    int nt = tx_lex(p, toks, 128);
    if (nt < 1) return 0;
    struct tx_pd pd;
    memset(&pd, 0, sizeof pd);
    pd.local_isdst = -1;
    if (!tx_parse_items(toks, &pd, z)) return 0;
    if (pd.dates > 1 || pd.times > 1 || pd.zones > 1 || pd.days > 1 || pd.locals > 1) return 0;
    if (pd.zones && pd.locals) return 0;

    struct tx_tm now;
    tx_tz_local(z, now_s, now_ns, &now);
    int64_t y = now.year;
    int mo = now.mon, d = now.mday;
    if (pd.dates) {
        if (pd.year_digits) {
            y = pd.year;
            if (pd.year_digits == 2) y += y < 69 ? 2000 : 1900;
        }
        mo = pd.month;
        d = pd.day;
        if (mo < 1 || mo > 12 || d < 1 || d > tx_mdays(y, mo)) return 0;
    }
    int h, mi, se;
    long nsec;
    if (pd.times) { h = pd.hour; mi = pd.minutes; se = pd.seconds; nsec = pd.nsec; }
    else if (pd.rels && !pd.dates && !pd.days) { h = now.hour; mi = now.min; se = now.sec; nsec = now_ns; }
    else { h = mi = se = 0; nsec = 0; }
    /* the day of the week, when no date was given */
    int64_t days = tx_days_from_civil(y, mo, d);
    if (pd.days && !pd.dates) {
        int wd = (int)(((days % 7) + 11) % 7);
        int ord = pd.dow_ord;
        days += ((pd.dow - wd + 7) % 7) + 7 * (ord - (ord > 0 && wd != pd.dow));
        int64_t yy; int mm, dd;
        tx_civil_from_days(days, &yy, &mm, &dd);
        y = yy; mo = mm; d = dd;
    }
    /* relative years, months and days on the calendar, then normalized */
    if (pd.ry || pd.rmo || pd.rd) {
        int64_t m0 = (y + pd.ry) * 12 + (mo - 1) + pd.rmo;
        y = tx_floordiv(m0, 12);
        mo = (int)(m0 - y * 12) + 1;
        days = tx_days_from_civil(y, mo, 1) + (d - 1) + pd.rd;
    } else
        days = tx_days_from_civil(y, mo, d);
    int64_t local = days * 86400 + h * 3600 + mi * 60 + se;
    int64_t t;
    if (pd.zones) t = local - pd.zone;
    else t = tx_tz_mktime(z, local, pd.local_isdst);
    t += pd.rh * 3600 + pd.rmi * 60 + pd.rs;
    long n = nsec + pd.rns;
    while (n < 0) { n += 1000000000L; t--; }
    while (n >= 1000000000L) { n -= 1000000000L; t++; }
    *sec = t;
    *ns = n;
    return 1;
}

/* ---- GNU's strftime ----------------------------------------------------- */

static int tx_iso_week(const struct tx_tm *tm, int64_t *iso_year) {
    int64_t y = tm->year;
    int wday = (tm->wday + 6) % 7;                 /* Monday 0 */
    int64_t wk = (tm->yday - wday + 10) / 7;
    if (wk < 1) {
        y--;
        int days = tx_leap(y) ? 366 : 365;
        wk = (tm->yday + days - wday + 10) / 7;
    } else {
        int days = tx_leap(y) ? 366 : 365;
        if (wk == 53 && tm->yday - wday + 3 >= days) { y++; wk = 1; }
    }
    *iso_year = y;
    return (int)wk;
}

struct tx_out { char *s; size_t n, cap; };

static void tx_put(struct tx_out *o, const char *p, size_t n) {
    if (o->n + n + 1 > o->cap) {
        o->cap = (o->n + n + 1) * 2 + 64;
        o->s = (char *)realloc(o->s, o->cap);
        if (!o->s) su_error(1, ENOMEM, "memory exhausted");
    }
    memcpy(o->s + o->n, p, n);
    o->n += n;
    o->s[o->n] = 0;
}

/* a field, as GNU pads one: a NUMBER is padded with its conversion's own
 * character (DEFPAD: '0', or ' ' for %e %k %l) to its own width (DEFW), or
 * to WIDTH; '-' pads nothing, '_' pads with spaces, '0' with zeros. TEXT
 * is padded with spaces on the left (zeros under '0'). '^' upper-cases;
 * '#' swaps the case: up when there is a lower-case letter, else down. */
static void tx_field(struct tx_out *o, const char *text, int numeric, int flag, int upcase,
                     int swapcase, int width, char defpad, int defw) {
    char buf[96];
    const char *t = text;
    int neg = 0;
    if (numeric) {
        while (*t == ' ') t++;
        if (*t == '-') { neg = 1; t++; }
        while (*t == '0' && t[1]) t++;
    }
    snprintf(buf, sizeof buf, "%s", t);
    if (swapcase) {
        int lower = 0;
        for (char *q = buf; *q; q++) if (islower((unsigned char)*q)) lower = 1;
        for (char *q = buf; *q; q++)
            *q = lower ? (char)toupper((unsigned char)*q) : (char)tolower((unsigned char)*q);
    }
    if (upcase) for (char *q = buf; *q; q++) *q = (char)toupper((unsigned char)*q);
    int w = width >= 0 ? width : (numeric ? defw : 0);
    char pad = numeric ? defpad : ' ';
    if (flag == '_') pad = ' ';
    else if (flag == '0') pad = '0';
    if (flag == '-') w = 0;
    int len = (int)strlen(buf) + neg;
    if (pad == '0') {
        if (neg) tx_put(o, "-", 1);
        for (int k = len; k < w; k++) tx_put(o, "0", 1);
    } else {
        for (int k = len; k < w; k++) tx_put(o, " ", 1);
        if (neg) tx_put(o, "-", 1);
    }
    tx_put(o, buf, strlen(buf));
}

static const char *const tx_wday_name[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                             "Thursday", "Friday", "Saturday" };
static const char *const tx_mon_name[12] = { "January", "February", "March", "April", "May",
                                             "June", "July", "August", "September", "October",
                                             "November", "December" };

static void tx_format(struct tx_out *o, const char *fmt, const struct tx_tm *tm, int64_t secs);

static void tx_conv(struct tx_out *o, int c, int flag, int upcase, int swapcase, int width,
                    int colons, const struct tx_tm *tm, int64_t secs) {
    char b[96];
    int64_t isoy;
    switch (c) {
    case 'a': snprintf(b, sizeof b, "%.3s", tx_wday_name[tm->wday]); break;
    case 'A': snprintf(b, sizeof b, "%s", tx_wday_name[tm->wday]); break;
    case 'b': case 'h': snprintf(b, sizeof b, "%.3s", tx_mon_name[tm->mon - 1]); break;
    case 'B': snprintf(b, sizeof b, "%s", tx_mon_name[tm->mon - 1]); break;
    case 'p': snprintf(b, sizeof b, "%s", tm->hour < 12 ? "AM" : "PM");
              if (swapcase) { swapcase = 0; for (char *q = b; *q; q++) *q = (char)tolower((unsigned char)*q); }
              break;
    case 'P': snprintf(b, sizeof b, "%s", tm->hour < 12 ? "am" : "pm"); break;
    case 'Z': snprintf(b, sizeof b, "%s", tm->zone);
              if (swapcase) { swapcase = 0; for (char *q = b; *q; q++) *q = (char)tolower((unsigned char)*q); }
              break;
    case 'c': case 'D': case 'F': case 'r': case 'R': case 'T': case 'x': case 'X': {
        const char *sub = c == 'c' ? "%a %b %e %H:%M:%S %Y" : c == 'D' || c == 'x' ? "%m/%d/%y" :
                          c == 'F' ? "%Y-%m-%d" : c == 'r' ? "%I:%M:%S %p" : c == 'R' ? "%H:%M" : "%H:%M:%S";
        struct tx_out tmp = { 0, 0, 0 };
        tx_format(&tmp, sub, tm, secs);
        tx_field(o, tmp.s ? tmp.s : "", 0, flag, upcase, swapcase, width, ' ', 0);
        free(tmp.s);
        return;
    }
    case 'C': snprintf(b, sizeof b, "%02lld", (long long)tx_floordiv(tm->year, 100));
              tx_field(o, b, 1, flag, upcase, swapcase, width, '0', 2); return;
    case 'd': snprintf(b, sizeof b, "%02d", tm->mday); tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'e': snprintf(b, sizeof b, "%2d", tm->mday); tx_field(o, b, 1, flag, 0, 0, width, ' ', 2); return;
    case 'G': tx_iso_week(tm, &isoy); snprintf(b, sizeof b, "%lld", (long long)isoy);
              tx_field(o, b, 1, flag, 0, 0, width, '0', 4); return;
    case 'g': tx_iso_week(tm, &isoy); snprintf(b, sizeof b, "%02lld", (long long)(((isoy % 100) + 100) % 100));
              tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'H': snprintf(b, sizeof b, "%02d", tm->hour); tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'I': snprintf(b, sizeof b, "%02d", tm->hour % 12 ? tm->hour % 12 : 12);
              tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'j': snprintf(b, sizeof b, "%03d", tm->yday + 1); tx_field(o, b, 1, flag, 0, 0, width, '0', 3); return;
    case 'k': snprintf(b, sizeof b, "%2d", tm->hour); tx_field(o, b, 1, flag, 0, 0, width, ' ', 2); return;
    case 'l': snprintf(b, sizeof b, "%2d", tm->hour % 12 ? tm->hour % 12 : 12);
              tx_field(o, b, 1, flag, 0, 0, width, ' ', 2); return;
    case 'm': snprintf(b, sizeof b, "%02d", tm->mon); tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'M': snprintf(b, sizeof b, "%02d", tm->min); tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'n': tx_put(o, "\n", 1); return;
    case 't': tx_put(o, "\t", 1); return;
    case 'N': {
        int digits = width > 0 && width <= 9 ? width : 9;
        snprintf(b, sizeof b, "%09ld", tm->nsec);
        b[digits] = 0;
        tx_put(o, b, strlen(b));
        return;
    }
    case 'q': snprintf(b, sizeof b, "%d", (tm->mon - 1) / 3 + 1); tx_field(o, b, 1, flag, 0, 0, width, '0', 1); return;
    case 's': snprintf(b, sizeof b, "%lld", (long long)secs); tx_field(o, b, 1, flag, 0, 0, width, '0', 1); return;
    case 'S': snprintf(b, sizeof b, "%02d", tm->sec); tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'u': snprintf(b, sizeof b, "%d", tm->wday ? tm->wday : 7); tx_field(o, b, 1, flag, 0, 0, width, '0', 1); return;
    case 'U': snprintf(b, sizeof b, "%02d", (tm->yday + 7 - tm->wday) / 7);
              tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'V': snprintf(b, sizeof b, "%02d", tx_iso_week(tm, &isoy)); tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'w': snprintf(b, sizeof b, "%d", tm->wday); tx_field(o, b, 1, flag, 0, 0, width, '0', 1); return;
    case 'W': snprintf(b, sizeof b, "%02d", (tm->yday + 7 - (tm->wday + 6) % 7) / 7);
              tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'y': snprintf(b, sizeof b, "%02lld", (long long)(((tm->year % 100) + 100) % 100));
              tx_field(o, b, 1, flag, 0, 0, width, '0', 2); return;
    case 'Y': snprintf(b, sizeof b, "%lld", (long long)tm->year); tx_field(o, b, 1, flag, 0, 0, width, '0', 4); return;
    case 'z': {
        long off = tm->gmtoff, a = off < 0 ? -off : off;
        char sg = off < 0 ? '-' : '+';
        long hh = a / 3600, mm = a / 60 % 60, ss = a % 60;
        if (colons == 0) snprintf(b, sizeof b, "%c%02ld%02ld", sg, hh, mm);
        else if (colons == 1) snprintf(b, sizeof b, "%c%02ld:%02ld", sg, hh, mm);
        else if (colons == 2) snprintf(b, sizeof b, "%c%02ld:%02ld:%02ld", sg, hh, mm, ss);
        else if (ss) snprintf(b, sizeof b, "%c%02ld:%02ld:%02ld", sg, hh, mm, ss);
        else if (mm) snprintf(b, sizeof b, "%c%02ld:%02ld", sg, hh, mm);
        else snprintf(b, sizeof b, "%c%02ld", sg, hh);
        tx_field(o, b, 0, flag, 0, 0, width, ' ', 0);
        return;
    }
    default: return;
    }
    tx_field(o, b, 0, flag, upcase, swapcase, width, ' ', 0);
}

static void tx_format(struct tx_out *o, const char *fmt, const struct tx_tm *tm, int64_t secs) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { tx_put(o, p, 1); continue; }
        const char *start = p++;
        int flag = 0, upcase = 0, swapcase = 0, width = -1, colons = 0;
        for (;; p++) {
            if (*p == '-' || *p == '_' || *p == '0') flag = *p;
            else if (*p == '^') upcase = 1;
            else if (*p == '#') swapcase = 1;
            else break;
        }
        if (isdigit((unsigned char)*p)) {
            width = 0;
            while (isdigit((unsigned char)*p)) width = width * 10 + (*p++ - '0');
        }
        while (*p == ':') { colons++; p++; }
        if (*p == 'E' || *p == 'O') {
            int mod = *p++;
            const char *ok = mod == 'E' ? "cCxXyY" : "deHImMSuUVwWy";
            if (!*p || !strchr(ok, *p)) {               /* not one of glibc's: as written */
                tx_put(o, start, (size_t)(p - start) + (*p ? 1 : 0));
                if (!*p) return;
                continue;
            }
        }
        if (!*p) { tx_put(o, start, (size_t)(p - start)); return; }
        if (colons && *p != 'z') { tx_put(o, start, (size_t)(p - start) + 1); continue; }
        if (*p == '%') { tx_put(o, "%", 1); continue; }
        if (!strchr("aAbBcCdDeFgGhHIjklmMnNpPqrRsStTuUVwWxXyYzZ", *p)) {
            tx_put(o, start, (size_t)(p - start) + 1);
            continue;
        }
        tx_conv(o, *p, flag, upcase, swapcase, width, colons, tm, secs);
    }
}

/* SECS/NS formatted with FMT in the current zone, or UTC when UTC */
static char *tx_strftime(const char *fmt, int64_t secs, long ns, int utc) {
    struct tx_tm tm;
    if (utc) {
        tx_breakdown(secs, 0, &tm);
        tm.isdst = 0;
        tm.nsec = ns;
        strcpy(tm.zone, "UTC");
    } else
        tx_tz_local(tx_tz_cur(), secs, ns, &tm);
    struct tx_out o = { 0, 0, 0 };
    tx_put(&o, "", 0);
    tx_format(&o, fmt, &tm, secs);
    return o.s;
}
