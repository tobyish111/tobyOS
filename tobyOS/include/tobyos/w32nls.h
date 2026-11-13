/* include/tobyos/w32nls.h -- W7s: the Win32 layer's national language support
 * (src/win32_nls.c): code pages, Unicode case and character classes, named
 * locales' data, collation, and the number, date and time formats. The data
 * is what Windows 11 answers (src/win32_nls_data.h, generated on Windows by
 * tools/w32nlsdump.c); the rules are measured (programs/win-locale50). */
#pragma once
#include <tobyos/types.h>

/* -- code pages -- */
/* CP_ACP and CP_THREAD_ACP are 1252, CP_OEMCP 437: the code page, or 0 when
 * tobyOS has none of that number */
uint32_t w32nls_cp(uint32_t cp);
#define W32NLS_UTF8 65001u
/* a byte of a one-byte code page as UTF-16; strict: 0xFFFF for a byte
 * MB_ERR_INVALID_CHARS refuses */
uint16_t w32nls_b2u(uint32_t cp, uint8_t b, bool strict);
/* a UTF-16 unit as a byte of a one-byte code page: its own byte, else (unless
 * nobf) the best fit; -1 none */
int w32nls_u2b(uint32_t cp, uint16_t u, bool nobf);
/* one character of UTF-8 in b[0..n) as Windows reads it: the bytes taken
 * (>= 1) with the code point in *cp (U+FFFD, *bad, for what is not UTF-8:
 * an impossible byte one, a lead and an out-of-range second byte one, a
 * sequence a byte cuts short one); 0 for n == 0; -1 a sequence the end of b
 * cuts short */
int w32nls_utf8_dec(const uint8_t *b, int n, uint32_t *cp, bool *bad);

/* -- Unicode (Windows' tables) -- */
uint16_t w32nls_uupper(uint16_t u);
uint16_t w32nls_ulower(uint16_t u);
uint16_t w32nls_uclass(uint16_t u);                /* GetStringTypeW's CT_CTYPE1 */

/* -- the C runtime's tables for an ANSI code page (the C locale's for 0, or
 *    for one with none of its own) -- */
const uint16_t *w32nls_pctype(uint32_t cp);
const uint8_t *w32nls_toupper_tab(uint32_t cp);
const uint8_t *w32nls_tolower_tab(uint32_t cp);

/* -- locales: indexes 0..w32nls_nloc()-1 -- */
int w32nls_nloc(void);
int w32nls_find(const uint16_t *name);             /* a NUL-terminated name, any case; -1 none */
int w32nls_find_lcid(uint32_t lcid);               /* LOCALE_USER_DEFAULT etc. too; -1 none */
int w32nls_user_default(void);                     /* en-US */
int w32nls_invariant(void);
uint32_t w32nls_lcid(int loc);
/* a locale's value for an LCTYPE (its flags taken off), NUL-terminated
 * UTF-16; NULL for an LCTYPE tobyOS does not keep */
const uint16_t *w32nls_info(int loc, uint32_t lctype);
const uint16_t *w32nls_genitive(int loc, int month);   /* month 0..12 */
bool w32nls_info_is_number(uint32_t lctype);
uint32_t w32nls_info_number(int loc, uint32_t lctype);

/* -- collation and case -- */
/* CompareStringEx's answer: CSTR_LESS_THAN 1, CSTR_EQUAL 2, CSTR_GREATER_THAN 3 */
int w32nls_compare(int loc, uint32_t flags, const uint16_t *a, int na, const uint16_t *b, int nb);
/* LCMAP_UPPERCASE / LCMAP_LOWERCASE (with LCMAP_LINGUISTIC_CASING: Turkish
 * and Azeri dotted and dotless i) in place */
void w32nls_case_map(int loc, uint32_t flags, uint16_t *s, int n);

/* -- formats -- */
struct w32nls_time { int year, month, day, hour, min, sec; };
/* a date or time picture ("dddd, d. MMMM yyyy", "h:mm:ss tt") expanded: the
 * units written, -1 when out (cap units) is too small */
int w32nls_fmt_picture(int loc, const uint16_t *pic, const struct w32nls_time *t, uint16_t *out, int cap);
/* a time picture without its seconds / minutes and seconds / marker
 * (TIME_NOSECONDS 2, TIME_NOMINUTESORSECONDS 1, TIME_NOTIMEMARKER 4,
 * TIME_FORCE24HOURFORMAT 8), into pic2 */
void w32nls_time_picture(const uint16_t *pic, uint32_t flags, uint16_t *pic2, int cap);
struct w32nls_numfmt {
    int digits, lzero, negorder;
    int group[4], ngroup;                          /* "3;0" -> {3}, repeating */
    bool repeat;
    uint16_t dec[8], thou[8], neg[8];
};
void w32nls_numfmt_of(int loc, struct w32nls_numfmt *nf);
/* a number's string ("-1234.5": a sign, digits, a '.', digits) in the
 * format: the units written, -1 too small, -2 not a number */
int w32nls_fmt_number(const struct w32nls_numfmt *nf, const char *val, uint16_t *out, int cap);
