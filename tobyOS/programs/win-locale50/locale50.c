/* win-locale50 -- Track C, W7s: named locales and code pages. Windows' ANSI
 * code page here is 1252 and its OEM one 437; MultiByteToWideChar and
 * WideCharToMultiByte convert through each code page's table (a character a
 * code page lacks written by best fit, else the default '?'); setlocale takes
 * a locale's name, its English name or a code page and answers as the UCRT
 * does; the C runtime's numbers, times, conversions, character classes and
 * collation then follow the locale; GetLocaleInfoEx, LCMapStringEx,
 * CompareStringEx and the number, date and time formats answer from each
 * locale's data. Each check is what Windows 11 does (measured first: this
 * program exits 52 there). Exit 52 iff all pass, else 100 + the first check
 * that failed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <locale.h>
#include <wchar.h>
#include <ctype.h>
#include <wctype.h>
#include <time.h>

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}
static char g_s[8192];
static int g_k;
static void s_reset(void) { g_k = 0; g_s[0] = 0; }
static void s_add(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_s + g_k, sizeof g_s - (size_t)g_k, fmt, ap);
    va_end(ap);
    if (n > 0 && g_k + n < (int)sizeof g_s) g_k += n;
}
static void s_hex(const unsigned char *b, int n) { s_add(" (%d):", n); for (int i = 0; i < n; i++) s_add(" %02X", b[i]); }
static void s_w(const wchar_t *w, int n) { s_add(" (%d):", n); for (int i = 0; i < n; i++) s_add(" %04X", (unsigned)w[i]); }
/* a wide string as text: ASCII as it is, the rest as \uXXXX */
static void s_wt(const wchar_t *w) { for (; *w; w++) { if (*w >= 0x20 && *w < 0x7F) s_add("%c", (char)*w); else s_add("\\u%04X", (unsigned)*w); } }
/* a narrow string: ASCII as it is, the rest as \xHH */
static void s_nt(const char *b) { for (; *b; b++) { unsigned char c = (unsigned char)*b; if (c >= 0x20 && c < 0x7F) s_add("%c", c); else s_add("\\x%02X", c); } }
static int seen(int n, const char *want) {
    printf("     %d saw:%s\n", n, g_s);
    if (strcmp(g_s, want) != 0) { printf("     %d want:%s\n", n, want); return 0; }
    return 1;
}

int main(void) {
    static const UINT cps[] = { CP_ACP, CP_OEMCP, 1252, 437, 850, 1251, 1250, 65001, 28591 };

    /* 1: the system's code pages and locale */
    s_reset();
    {
        wchar_t nm[LOCALE_NAME_MAX_LENGTH];
        s_add(" %u %u", GetACP(), GetOEMCP());
        GetUserDefaultLocaleName(nm, LOCALE_NAME_MAX_LENGTH); s_add(" "); s_wt(nm);
        GetSystemDefaultLocaleName(nm, LOCALE_NAME_MAX_LENGTH); s_add(" "); s_wt(nm);
        s_add(" %lx %lx %x %x", (unsigned long)GetUserDefaultLCID(), (unsigned long)GetSystemDefaultLCID(),
              (unsigned)GetUserDefaultUILanguage(), (unsigned)GetUserDefaultLangID());
        s_add(" | %d %d %d %d %d", IsValidCodePage(1252), IsValidCodePage(437), IsValidCodePage(65001), IsValidCodePage(1251), IsValidCodePage(12345));
        CPINFO ci; memset(&ci, 0, sizeof ci);
        BOOL r = GetCPInfo(1251, &ci); s_add(" | %d %u %02X %02X", r, ci.MaxCharSize, ci.DefaultChar[0], ci.LeadByte[0]);
        r = GetCPInfo(65001, &ci); s_add(" %d %u", r, ci.MaxCharSize);
    }
    check(1, seen(1,
        " 1252 437 en-US en-US 409 409 409 409 | 1 1 1 1 0 | 1 1 3F 00 1 4"), "GetACP 1252, GetOEMCP 437, the user's and system's locale en-US (0x409), the code pages valid, GetCPInfo");

    /* 2: MultiByteToWideChar */
    s_reset();
    {
        const char *s = "\x80\xE4\x84\xC0\xFF\xC3\xA4";
        for (int i = 0; i < 9; i++) {
            wchar_t w[16]; SetLastError(0);
            int n = MultiByteToWideChar(cps[i], 0, s, 7, w, 16);
            s_add(" | %u %d %lu", cps[i], n, GetLastError()); s_w(w, n > 0 ? n : 0);
        }
        wchar_t w[16];
        SetLastError(0); int n = MultiByteToWideChar(65001, MB_ERR_INVALID_CHARS, s, 7, w, 16); s_add(" | %d %lu", n, GetLastError());
        SetLastError(0); n = MultiByteToWideChar(12345, 0, s, 7, w, 16); s_add(" %d %lu", n, GetLastError());
        n = MultiByteToWideChar(1252, 0, s, -1, 0, 0); s_add(" %d", n);
        SetLastError(0); n = MultiByteToWideChar(1252, 0, s, 7, w, 2); s_add(" %d %lu", n, GetLastError());
        n = MultiByteToWideChar(65001, 0, "\xF0\x9F\x98\x80" "a", 5, w, 16); s_add(" |"); s_w(w, n > 0 ? n : 0);
    }
    check(2, seen(2,
        " | 0 7 0 (7): 20AC 00E4 201E 00C0 00FF 00C3 00A4 | 1 7 0 (7): 00C7 03A3 00E4 2514 00A0 251C 00F1 | 1252 7 0 (7): 20AC 00E4 201E 00C0 00FF 00C3 00A4 | 437 7 0 (7"
        "): 00C7 03A3 00E4 2514 00A0 251C 00F1 | 850 7 0 (7): 00C7 00F5 00E4 2514 00A0 251C 00F1 | 1251 7 0 (7): 0402 0434 201E 0410 044F 0413 00A4 | 1250 7 0 (7): 20AC "
        "00E4 201E 0154 02D9 0102 00A4 | 65001 5 0 (5): FFFD FFFD FFFD FFFD 00E4 | 28591 7 0 (7): 0080 00E4 0084 00C0 00FF 00C3 00A4 | 0 1113 0 87 8 0 122 | (3): D83D DE"
        "00 0061"), "MultiByteToWideChar through 1252, 437, 850, 1251, 1250, UTF-8 and 28591 (CP_ACP 1252, CP_OEMCP 437); its errors");

    /* 3: WideCharToMultiByte */
    s_reset();
    {
        const wchar_t *ws = L"\x20AC\x00E4\x0416\x4E2D\x00C0x";
        for (int i = 0; i < 9; i++) {
            char b[32]; BOOL used = 7; SetLastError(0);
            int n = WideCharToMultiByte(cps[i], 0, ws, 6, b, 32, NULL, cps[i] == 65001 ? NULL : &used);
            s_add(" | %u %d %d %lu", cps[i], n, used, GetLastError()); s_hex((unsigned char *)b, n > 0 ? n : 0);
        }
        char b[32]; BOOL used = 7;
        int n = WideCharToMultiByte(1252, 0, ws, 6, b, 32, "#", &used); s_add(" | %d %d", n, used); s_hex((unsigned char *)b, n);
        n = WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, L"\x0100\x2019", 2, b, 32, NULL, NULL); s_hex((unsigned char *)b, n);
        n = WideCharToMultiByte(1252, 0, L"\x0100\x0101\x0141\x2019", 4, b, 32, NULL, NULL); s_hex((unsigned char *)b, n);
        n = WideCharToMultiByte(1251, 0, L"\x0416\x0436\x00E4", 3, b, 32, NULL, NULL); s_hex((unsigned char *)b, n);
        SetLastError(0); n = WideCharToMultiByte(65001, 0, L"\x4E2D", 1, b, 2, NULL, NULL); s_add(" %d %lu", n, GetLastError());
        n = WideCharToMultiByte(65001, 0, L"a\xD83D\xDE00\xD800z", 5, b, 32, NULL, NULL); s_hex((unsigned char *)b, n);
        n = WideCharToMultiByte(1252, 0, L"\x00E9t\x00E9", -1, 0, 0, NULL, NULL); s_add(" %d", n);
    }
    check(3, seen(3,
        " | 0 6 1 0 (6): 80 E4 3F 3F C0 78 | 1 6 1 0 (6): 3F 84 3F 3F 41 78 | 1252 6 1 0 (6): 80 E4 3F 3F C0 78 | 437 6 1 0 (6): 3F 84 3F 3F 41 78 | 850 6 1 0 (6): 3F 84"
        " 3F 3F B7 78 | 1251 6 1 0 (6): 88 61 C6 3F 41 78 | 1250 6 1 0 (6): 80 E4 3F 3F 41 78 | 65001 13 7 0 (13): E2 82 AC C3 A4 D0 96 E4 B8 AD C3 80 78 | 28591 6 1 0 ("
        "6): 3F E4 3F 3F C0 78 | 6 1 (6): 80 E4 23 23 C0 78 (2): 3F 92 (4): 41 61 4C 92 (3): C6 E6 61 0 122 (9): 61 F0 9F 98 80 EF BF BD 7A 4"), "WideCharToMultiByte: each code page's characters, best fit (A-macron A), the default and its flag, no best fit, the errors");

    /* 4: setlocale's names */
    s_reset();
    {
        static const char *f[] = { "", "de-DE", "German_Germany.1252", "German_Germany", "German", "de", "fr-FR", "ru-RU",
            "Russian_Russia.1251", ".UTF-8", "en-US.UTF-8", "de-DE.UTF-8", ".utf8", ".1251", ".437", "English", "en-GB", "sv-SE",
            "tr-TR", "pl-PL", "es-ES", "C", "c", "POSIX", "bogus-XX", "de-DE.1251", "German_Germany.65001", "en", "French", "French_France" };
        s_add(" %s", setlocale(LC_ALL, NULL));
        for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
            const char *r = setlocale(LC_ALL, f[i]);
            s_add(" | %s", r ? r : "NULL");
            setlocale(LC_ALL, "C");
        }
        setlocale(LC_ALL, "de-DE");
        setlocale(LC_NUMERIC, "C");
        s_add(" | %s", setlocale(LC_ALL, NULL));
        s_add(" | %s %s", setlocale(LC_NUMERIC, NULL), setlocale(LC_TIME, NULL));
        setlocale(LC_ALL, "C");
        wchar_t *wr = _wsetlocale(LC_ALL, L"fr-FR"); s_add(" | "); s_wt(wr ? wr : L"NULL");
        setlocale(LC_ALL, "C");
    }
    check(4, seen(4,
        " C | English_United States.1252 | de-DE | German_Germany.1252 | German_Germany.1252 | German_Germany.1252 | de | fr-FR | ru-RU | Russian_Russia.1251 | English_U"
        "nited States.utf8 | en-US.UTF-8 | de-DE.UTF-8 | English_United States.utf8 | English_United States.1251 | English_United States.437 | English_United States.1252"
        " | en-GB | sv-SE | tr-TR | pl-PL | es-ES | C | NULL | NULL | NULL | NULL | German_Germany.utf8 | en | French_France.1252 | French_France.1252 | LC_COLLATE=de-DE"
        ";LC_CTYPE=de-DE;LC_MONETARY=de-DE;LC_NUMERIC=C;LC_TIME=de-DE | C de-DE | fr-FR"), "setlocale: a locale's name, its English name with or without a code page, a code page alone, C; the categories apart");

    /* 5: de-DE's numbers */
    s_reset();
    {
        setlocale(LC_ALL, "de-DE");
        struct lconv *lc = localeconv();
        s_add(" '%s' '%s' %d '%s'", lc->decimal_point, lc->thousands_sep, lc->grouping[0], lc->int_curr_symbol);
        s_hex((unsigned char *)lc->currency_symbol, (int)strlen(lc->currency_symbol));
        s_add(" '%s' '%s' %d %d %d %d", lc->mon_decimal_point, lc->mon_thousands_sep, lc->frac_digits, lc->p_cs_precedes, lc->p_sep_by_space, lc->n_sign_posn);
        char b[128]; snprintf(b, sizeof b, "%.2f|%g|%e|%.1f", 1234.5, 0.25, 12.5, -3.0); s_add(" | %s", b);
        wchar_t w[64]; swprintf(w, 64, L"%.2f", 2.5); s_add(" "); s_wt(w);
        s_add(" | %g %g %g", strtod("1,5", 0), strtod("1.5", 0), atof("2,25"));
        double d = 0; sscanf("3,75", "%lf", &d); s_add(" %g", d);
        setlocale(LC_ALL, "fr-FR");
        lc = localeconv(); s_add(" | fr '%s'", lc->decimal_point); s_hex((unsigned char *)lc->thousands_sep, (int)strlen(lc->thousands_sep));
        setlocale(LC_ALL, "C");
        lc = localeconv(); s_add(" | C '%s' '%s'", lc->decimal_point, lc->thousands_sep);
        snprintf(b, sizeof b, "%.2f", 1234.5); s_add(" %s", b);
    }
    check(5, seen(5,
        " ',' '.' 3 'EUR' (1): 80 ',' '.' 2 0 1 1 | 1234,50|0,25|1,250000e+01|-3,0 2,50 | 1,5 1 2,25 3,75 | fr ',' (1): A0 | C '.' '' 1234.50"), "de-DE: localeconv's separators and currency; printf, swprintf, strtod, atof and sscanf with the decimal comma; fr-FR's; C again");

    /* 6: strftime and wcsftime */
    s_reset();
    {
        time_t t = 1700000000; struct tm tm; gmtime_s(&tm, &t);
        char o[256]; wchar_t wo[128];
        const char *ls[] = { "de-DE", "fr-FR", "en-US", "sv-SE", "es-ES" };
        for (int i = 0; i < 5; i++) {
            setlocale(LC_ALL, ls[i]);
            strftime(o, sizeof o, "%A|%a|%B|%b|%c|%x|%X|%p|%#x|%#c", &tm);
            s_add(" | "); s_nt(o);
        }
        setlocale(LC_ALL, "ru-RU");
        int n = (int)strftime(o, sizeof o, "%A|%B|%b", &tm); s_add(" |"); s_hex((unsigned char *)o, n);
        wcsftime(wo, 128, L"%A|%B", &tm); s_add(" "); s_wt(wo);
        setlocale(LC_ALL, "C");
        strftime(o, sizeof o, "%A|%c|%x|%p", &tm); s_add(" | "); s_nt(o);
    }
    check(6, seen(6,
        " | Dienstag|Di|November|Nov|14.11.2023 22:13:20|14.11.2023|22:13:20||Dienstag, 14. November 2023|Dienstag, 14. November 2023 22:13:20 | mardi|mar.|novembre|nov."
        "|14/11/2023 22:13:20|14/11/2023|22:13:20||mardi 14 novembre 2023|mardi 14 novembre 2023 22:13:20 | Tuesday|Tue|November|Nov|11/14/2023 10:13:20 PM|11/14/2023|10"
        ":13:20 PM|PM|Tuesday, November 14, 2023|Tuesday, November 14, 2023 10:13:20 PM | tisdag|tis|november|nov|2023-11-14 22:13:20|2023-11-14|22:13:20||den 14 novembe"
        "r 2023|den 14 november 2023 22:13:20 | martes|ma.|noviembre|nov.|14/11/2023 22:13:20|14/11/2023|22:13:20||martes, 14 de noviembre de 2023|martes, 14 de noviembr"
        "e de 2023 22:13:20 | (18): E2 F2 EE F0 ED E8 EA 7C CD EE FF E1 F0 FC 7C ED EE FF \\u0432\\u0442\\u043E\\u0440\\u043D\\u0438\\u043A|\\u041D\\u043E\\u044F\\u0431\\u"
        "0440\\u044C | Tuesday|Tue Nov 14 22:13:20 2023|11/14/23|PM"), "strftime's names and formats in de-DE, fr-FR, en-US, sv-SE, es-ES, ru-RU (its code page) and wcsftime's, then the C locale's");

    /* 7: the multibyte conversions */
    s_reset();
    {
        wchar_t w[8]; char m[16]; int n;
        const char *ls[] = { "C", "de-DE", "ru-RU", ".UTF-8", "pl-PL" };
        for (int i = 0; i < 5; i++) {
            setlocale(LC_ALL, ls[i]);
            s_add(" | %s %d", ls[i], (int)MB_CUR_MAX);
            n = (int)mbstowcs(w, "\xE4\x80\xC6\xA3", 8); s_w(w, n > 0 ? n : 0);
            n = (int)wcstombs(m, L"\x00E4\x20AC\x0416\x0100", 16); s_add(" %d", n); if (n > 0) s_hex((unsigned char *)m, n);
            n = (int)wcstombs(m, L"\x00E4\x4E2D", 16); s_add(" %d", n);
            wchar_t wc = 0; n = mbtowc(&wc, "\xC3\xA4", 2); s_add(" %d %04X", n, (unsigned)wc);
            n = wctomb(m, 0x2019); s_add(" %d %02X", n, n > 0 ? (unsigned char)m[0] : 0);
            s_add(" %04X %d", (unsigned)btowc(0xE4), wctob(0x20AC));
        }
        setlocale(LC_ALL, "C");
    }
    check(7, seen(7,
        " | C 1 (4): 00E4 0080 00C6 00A3 -1 -1 1 00C3 -1 00 00E4 -1 | de-DE 1 (4): 00E4 20AC 00C6 00A3 -1 -1 1 00C3 1 92 00E4 128 | ru-RU 1 (4): 0434 0402 0416 0408 4 (4"
        "): 61 88 C6 41 -1 1 0413 1 92 0434 136 | .UTF-8 4 (0): 9 (9): C3 A4 E2 82 AC D0 96 C4 80 5 2 00E4 3 E2 FFFF -1 | pl-PL 1 (4): 00E4 20AC 0106 0141 -1 -1 1 0102 1"
        " 92 00E4 128"), "mbstowcs, wcstombs (best fit), mbtowc, wctomb, btowc, wctob and MB_CUR_MAX in C, de-DE, ru-RU, .UTF-8 and pl-PL");

    /* 8: character classes and case */
    s_reset();
    {
        const char *ls[] = { "C", "de-DE", "ru-RU", "tr-TR", "pl-PL" };
        for (int i = 0; i < 5; i++) {
            setlocale(LC_ALL, ls[i]);
            s_add(" | %s", ls[i]);
            int c[] = { 0xE4, 0xC4, 0xFF, 0xB5, 0xA3, 0xE6, 0x9F, 'i' };
            for (int k = 0; k < 8; k++)
                s_add(" %02X:%d%d%d%d,%02X,%02X", c[k], isalpha(c[k]) != 0, islower(c[k]) != 0, isupper(c[k]) != 0, ispunct(c[k]) != 0,
                      toupper(c[k]) & 0xFF, tolower(c[k]) & 0xFF);
            s_add(" w %04X %04X %04X %04X %d%d%d", (unsigned)towupper(0xE4), (unsigned)towupper(0xFF), (unsigned)towupper(0x436),
                  (unsigned)towlower(0x130), iswalpha(0x436) != 0, iswupper(0xC4) != 0, iswspace(0x3000) != 0);
        }
        setlocale(LC_ALL, "C");
    }
    check(8, seen(8,
        " | C E4:0000,E4,E4 C4:0000,C4,C4 FF:0000,FF,FF B5:0000,B5,B5 A3:0000,A3,A3 E6:0000,E6,E6 9F:0000,9F,9F 69:1100,49,69 w 00E4 00FF 0436 0130 111 | de-DE E4:1100,C"
        "4,E4 C4:1010,C4,E4 FF:1100,9F,FF B5:1101,B5,B5 A3:0001,A3,A3 E6:1100,C6,E6 9F:1010,9F,FF 69:1100,49,69 w 00C4 009F 0416 0130 111 | ru-RU E4:1100,C4,E4 C4:1010,C"
        "4,E4 FF:1100,DF,FF B5:1101,B5,B5 A3:1010,A3,BC E6:1100,C6,E6 9F:1100,8F,9F 69:1100,49,69 w 00C4 00DF 0416 0130 111 | tr-TR E4:1100,C4,E4 C4:1010,C4,E4 FF:1100,9"
        "F,FF B5:1101,B5,B5 A3:0001,A3,A3 E6:1100,C6,E6 9F:1010,9F,FF 69:1100,49,69 w 00C4 009F 0416 0130 111 | pl-PL E4:1100,C4,E4 C4:1010,C4,E4 FF:0000,FF,FF B5:1101,B"
        "5,B5 A3:1010,A3,B3 E6:1100,C6,E6 9F:1100,8F,9F 69:1100,49,69 w 00C4 00FF 0416 0130 111"), "isalpha, islower, isupper, ispunct, toupper and tolower past ASCII per code page; towupper, towlower, iswalpha in C and the locales");

    /* 9: collation */
    s_reset();
    {
        setlocale(LC_ALL, "de-DE");
        s_add(" %d %d %d %d %d %d", strcoll("\xE4", "b"), strcoll("\xE4", "z"), wcscoll(L"\x00E4", L"z"), _stricmp("\xE4", "\xC4"),
              _stricoll("\xE4", "\xC4"), _wcsicmp(L"\x00E4", L"\x00C4"));
        s_add(" %d %d", strcoll("a", "B"), strcoll("co-op", "coop"));
        setlocale(LC_ALL, "sv-SE");
        s_add(" | %d %d", strcoll("\xE4", "z"), wcscoll(L"\x00E4", L"z"));
        setlocale(LC_ALL, "C");
        s_add(" | %d %d %d", strcoll("\xE4", "b"), strcoll("a", "B"), _stricmp("\xE4", "\xC4"));
        s_add(" | %d %d %d %d", lstrcmpiA("\xE4", "\xC4"), lstrcmpA("a", "B"), lstrcmpW(L"a", L"B"), CompareStringOrdinal(L"a", -1, L"B", -1, TRUE));
    }
    check(9, seen(9,
        " -1 -1 -1 0 0 0 -1 -1 | 1 1 | 1 1 32 | 0 -1 -1 1"), "strcoll, wcscoll, _stricmp, _stricoll and _wcsicmp in de-DE, sv-SE (\\u00e4 after z) and C; lstrcmp(i) linguistic");

    /* 10: GetLocaleInfoEx */
    s_reset();
    {
        const wchar_t *locs[] = { L"de-DE", L"fr-FR", L"ru-RU", L"en-US", L"sv-SE", L"" };
        LCTYPE ts[] = { LOCALE_SNAME, LOCALE_SENGLISHLANGUAGENAME, LOCALE_SNATIVELANGUAGENAME, LOCALE_SENGLISHCOUNTRYNAME, LOCALE_SDECIMAL,
                        LOCALE_STHOUSAND, LOCALE_SMONTHNAME1, LOCALE_SDAYNAME1, LOCALE_SABBREVMONTHNAME1, LOCALE_SSHORTDATE, LOCALE_SLONGDATE,
                        LOCALE_STIMEFORMAT, LOCALE_IDEFAULTANSICODEPAGE, LOCALE_IDEFAULTCODEPAGE, LOCALE_SISO639LANGNAME, LOCALE_SISO3166CTRYNAME,
                        LOCALE_SCURRENCY, LOCALE_ILANGUAGE, LOCALE_SLIST, LOCALE_IMEASURE };
        for (int i = 0; i < 6; i++) {
            s_add(" |");
            for (int j = 0; j < 20; j++) { wchar_t o[128]; int n = GetLocaleInfoEx(locs[i], ts[j], o, 128); s_add(" [%d]", n); if (n > 0) s_wt(o); }
        }
        wchar_t o[64]; DWORD v = 0;
        int n = GetLocaleInfoEx(L"de-DE", LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER, (LPWSTR)&v, 2); s_add(" | %d %lu", n, (unsigned long)v);
        n = GetLocaleInfoEx(L"ru-RU", LOCALE_SMONTHNAME1 | LOCALE_RETURN_GENITIVE_NAMES, o, 64); s_add(" [%d]", n); s_wt(o);
        n = GetLocaleInfoW(0x407, LOCALE_SMONTHNAME3, o, 64); s_add(" [%d]", n); s_wt(o);
        char ao[64]; n = GetLocaleInfoA(0x419, LOCALE_SMONTHNAME1, ao, 64); s_add(" [%d]", n); s_nt(ao);
        n = GetLocaleInfoEx(L"de-DE", LOCALE_SMONTHNAME1, o, 3); s_add(" [%d %lu]", n, GetLastError());
        n = GetLocaleInfoEx(L"de-DE", LOCALE_SMONTHNAME1, 0, 0); s_add(" [%d]", n);
        s_add(" | %lx %lx", (unsigned long)LocaleNameToLCID(L"de-DE", 0), (unsigned long)LocaleNameToLCID(L"ru-RU", 0));
        n = LCIDToLocaleName(0x41D, o, 64, 0); s_add(" [%d]", n); s_wt(o);
        s_add(" %d", IsValidLocaleName(L"de-DE"));
    }
    check(10, seen(10,
        " | [6]de-DE [7]German [8]Deutsch [8]Germany [2], [2]. [7]Januar [7]Montag [4]Jan [11]dd.MM.yyyy [19]dddd, d. MMMM yyyy [9]HH:mm:ss [5]1252 [4]850 [3]de [3]DE [2"
        "]\\u20AC [5]0407 [2]; [2]0 | [6]fr-FR [7]French [9]fran\\u00E7ais [7]France [2], [2]\\u202F [8]janvier [6]lundi [6]janv. [11]dd/MM/yyyy [17]dddd d MMMM yyyy [9]"
        "HH:mm:ss [5]1252 [4]850 [3]fr [3]FR [2]\\u20AC [5]040c [2]; [2]0 | [6]ru-RU [8]Russian [8]\\u0440\\u0443\\u0441\\u0441\\u043A\\u0438\\u0439 [7]Russia [2], [2]\\u"
        "00A0 [7]\\u042F\\u043D\\u0432\\u0430\\u0440\\u044C [12]\\u043F\\u043E\\u043D\\u0435\\u0434\\u0435\\u043B\\u044C\\u043D\\u0438\\u043A [4]\\u044F\\u043D\\u0432 [1"
        "1]dd.MM.yyyy [17]d MMMM yyyy '\\u0433.' [8]H:mm:ss [5]1251 [4]866 [3]ru [3]RU [2]\\u20BD [5]0419 [2]; [2]0 | [6]en-US [8]English [8]English [14]United States [2"
        "]. [2], [8]January [7]Monday [4]Jan [9]M/d/yyyy [19]dddd, MMMM d, yyyy [11]h:mm:ss tt [5]1252 [4]437 [3]en [3]US [2]$ [5]0409 [2], [2]1 | [6]sv-SE [8]Swedish [8"
        "]svenska [7]Sweden [2], [2]\\u00A0 [8]januari [7]m\\u00E5ndag [4]jan [11]yyyy-MM-dd [18]'den 'd MMMM yyyy [9]HH:mm:ss [5]1252 [4]850 [3]sv [3]SE [3]kr [5]041d ["
        "2]; [2]0 | [1] [19]Invariant Language [19]Invariant Language [18]Invariant Country [2]. [2], [8]January [7]Monday [4]Jan [11]MM/dd/yyyy [19]dddd, dd MMMM yyyy ["
        "9]HH:mm:ss [5]1252 [4]437 [3]iv [3]IV [2]\\u00A4 [5]007f [2], [2]0 | 2 1252 [7]\\u044F\\u043D\\u0432\\u0430\\u0440\\u044F [5]M\\u00E4rz [7]\\xDF\\xED\\xE2\\xE0\\x"
        "F0\\xFC [0 122] [7] | 407 419 [6]sv-SE 1"), "GetLocaleInfoEx: de-DE, fr-FR, ru-RU, en-US, sv-SE and the invariant locale's data; as a number; genitive; by LCID (W, A); sizes");

    /* 11: LCMapStringEx and CompareStringEx */
    s_reset();
    {
        wchar_t o[32]; int n;
        n = LCMapStringEx(L"de-DE", LCMAP_UPPERCASE, L"stra\x00DF" L"e \x00E4 \x03B1\x03B2 \x0436 \x0131 \x00FF", -1, o, 32, 0, 0, 0); s_w(o, n);
        n = LCMapStringEx(L"tr-TR", LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, L"i\x0131", -1, o, 32, 0, 0, 0); s_w(o, n);
        n = LCMapStringEx(L"de-DE", LCMAP_LOWERCASE, L"\x00C4\x0391\x0416\x0130\x0178", -1, o, 32, 0, 0, 0); s_w(o, n);
        n = LCMapStringEx(L"tr-TR", LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING, L"I\x0130", -1, o, 32, 0, 0, 0); s_w(o, n);
        s_add(" |");
        s_add(" %d %d %d %d %d %d %d", CompareStringEx(L"de-DE", 0, L"\x00E4", -1, L"b", -1, 0, 0, 0), CompareStringEx(L"sv-SE", 0, L"\x00E4", -1, L"z", -1, 0, 0, 0),
              CompareStringEx(L"de-DE", 0, L"A", -1, L"a", -1, 0, 0, 0), CompareStringEx(L"de-DE", NORM_IGNORECASE, L"A", -1, L"a", -1, 0, 0, 0),
              CompareStringEx(L"de-DE", 0, L"co-op", -1, L"coop", -1, 0, 0, 0), CompareStringEx(L"de-DE", 0, L"10", -1, L"9", -1, 0, 0, 0),
              CompareStringEx(L"de-DE", SORT_DIGITSASNUMBERS, L"10", -1, L"9", -1, 0, 0, 0));
        s_add(" %d %d %d %d %d", CompareStringEx(L"de-DE", 0, L"a", -1, L"\x00E4", -1, 0, 0, 0), CompareStringEx(L"de-DE", 0, L"\x00E4", -1, L"az", -1, 0, 0, 0),
              CompareStringEx(L"de-DE", 0, L"ae", -1, L"\x00E4", -1, 0, 0, 0), CompareStringEx(L"de-DE", NORM_IGNORENONSPACE, L"\x00E4", -1, L"a", -1, 0, 0, 0),
              CompareStringEx(L"en-US", 0, L"apple", -1, L"Apple", -1, 0, 0, 0));
        s_add(" %d %d", CompareStringW(0x407, 0, L"Stra\x00DF" L"e", -1, L"Strasse", -1), CompareStringA(0x409, NORM_IGNORECASE, "ABC", -1, "abd", -1));
    }
    check(11, seen(11,
        " (18): 0053 0054 0052 0041 00DF 0045 0020 00C4 0020 0391 0392 0020 0416 0020 0131 0020 0178 0000 (3): 0130 0049 0000 (6): 00E4 03B1 0436 0130 00FF 0000 (3): 013"
        "1 0069 0000 | 1 3 3 2 3 1 3 1 1 3 2 1 2 1"), "LCMapStringEx's case mapping (\\u00df and dotless i kept; Turkish linguistic casing); CompareStringEx's order");

    /* 12: numbers, dates, times */
    s_reset();
    {
        wchar_t o[64]; int n;
        SYSTEMTIME st = { 2026, 11, 4, 12, 14, 5, 9, 0 };
        const wchar_t *ls[] = { L"de-DE", L"en-US", L"fr-FR", L"ru-RU", L"sv-SE" };
        for (int i = 0; i < 5; i++) {
            s_add(" |");
            n = GetNumberFormatEx(ls[i], 0, L"-1234567.891", NULL, o, 64); s_add(" [%d]", n); s_wt(o);
            n = GetDateFormatEx(ls[i], DATE_LONGDATE, &st, NULL, o, 64, NULL); s_add(" [%d]", n); s_wt(o);
            n = GetDateFormatEx(ls[i], DATE_SHORTDATE, &st, NULL, o, 64, NULL); s_add(" [%d]", n); s_wt(o);
            n = GetTimeFormatEx(ls[i], 0, &st, NULL, o, 64); s_add(" [%d]", n); s_wt(o);
            n = GetDateFormatEx(ls[i], 0, &st, L"ddd d MMM yy", o, 64, NULL); s_add(" [%d]", n); s_wt(o);
            n = GetTimeFormatEx(ls[i], TIME_NOSECONDS, &st, NULL, o, 64); s_add(" [%d]", n); s_wt(o);
        }
        n = GetDateFormatW(0x409, 0, &st, L"MMMM d, yyyy 'at' dddd", o, 64); s_add(" | [%d]", n); s_wt(o);
        n = GetTimeFormatW(0x409, 0, &st, L"hh 'o''clock' tt", o, 64); s_add(" [%d]", n); s_wt(o);
        n = GetNumberFormatEx(L"en-US", 0, L"0.5", NULL, o, 64); s_add(" [%d]", n); s_wt(o);
    }
    check(12, seen(12,
        " | [14]-1.234.567,89 [30]Donnerstag, 12. November 2026 [11]12.11.2026 [9]14:05:09 [13]Do 12 Nov 26 [6]14:05 | [14]-1,234,567.89 [28]Thursday, November 12, 2026 "
        "[11]11/12/2026 [11]2:05:09 PM [14]Thu 12 Nov 26 [8]2:05 PM | [14]-1\\u202F234\\u202F567,89 [23]jeudi 12 novembre 2026 [11]12/11/2026 [9]14:05:09 [16]jeu. 12 nov"
        ". 26 [6]14:05 | [14]-1\\u00A0234\\u00A0567,89 [18]12 \\u043D\\u043E\\u044F\\u0431\\u0440\\u044F 2026 \\u0433. [11]12.11.2026 [9]14:05:09 [13]\\u0427\\u0442 12 \\u"
        "043D\\u043E\\u044F 26 [6]14:05 | [14]-1\\u00A0234\\u00A0567,89 [21]den 12 november 2026 [11]2026-11-12 [9]14:05:09 [14]tor 12 nov 26 [6]14:05 | [30]November 12,"
        " 2026 at Thursday [14]02 o'clock PM [5]0.50"), "GetNumberFormatEx, GetDateFormatEx (genitive months in ru-RU) and GetTimeFormatEx in de-DE, en-US, fr-FR, ru-RU, sv-SE");

    if (first_fail) { printf("FAILED (first: %d)\n", first_fail); return 100 + first_fail; }
    printf("ALL PASS\n");
    return 52;
}
