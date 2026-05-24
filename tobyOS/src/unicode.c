/* unicode.c -- UTF-8 encoding/decoding and Unicode utilities.
 *
 * Provides UTF-8 codec, string operations, and basic Unicode
 * character classification.
 */

#include <tobyos/types.h>
#include <tobyos/klibc.h>

/* Decode one UTF-8 codepoint from `s`. Stores codepoint in *cp.
 * Returns number of bytes consumed (1-4), or 0 on invalid sequence. */
int utf8_decode(const char *s, uint32_t *cp) {
    if (!s || !cp) return 0;
    const uint8_t *p = (const uint8_t *)s;

    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        if ((p[1] & 0xC0) != 0x80) return 0;
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) |
              ((uint32_t)(p[1] & 0x3F));
        if (*cp < 0x80) return 0;
        return 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) |
              ((uint32_t)(p[1] & 0x3F) << 6) |
              ((uint32_t)(p[2] & 0x3F));
        if (*cp < 0x800) return 0;
        return 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
            (p[3] & 0xC0) != 0x80) return 0;
        *cp = ((uint32_t)(p[0] & 0x07) << 18) |
              ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) |
              ((uint32_t)(p[3] & 0x3F));
        if (*cp < 0x10000 || *cp > 0x10FFFF) return 0;
        return 4;
    }
    return 0;
}

/* Encode one codepoint as UTF-8 into buf (must have room for 4 bytes).
 * Returns bytes written (1-4), or 0 if codepoint is invalid. */
int utf8_encode(uint32_t cp, char *buf) {
    if (!buf) return 0;
    uint8_t *p = (uint8_t *)buf;

    if (cp < 0x80) {
        p[0] = (uint8_t)cp;
        return 1;
    } else if (cp < 0x800) {
        p[0] = (uint8_t)(0xC0 | (cp >> 6));
        p[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        p[0] = (uint8_t)(0xE0 | (cp >> 12));
        p[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        p[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        p[0] = (uint8_t)(0xF0 | (cp >> 18));
        p[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        p[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        p[3] = (uint8_t)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Count the number of codepoints in a UTF-8 string. */
size_t utf8_strlen(const char *s) {
    if (!s) return 0;
    size_t count = 0;
    while (*s) {
        uint32_t cp;
        int n = utf8_decode(s, &cp);
        if (n == 0) { s++; continue; }
        s += n;
        count++;
    }
    return count;
}

/* Advance past one codepoint. Returns pointer to next, or NULL at end. */
const char *utf8_advance(const char *s) {
    if (!s || !*s) return NULL;
    uint32_t cp;
    int n = utf8_decode(s, &cp);
    if (n == 0) return s + 1;
    return s + n;
}

/* Move backward one codepoint. Returns pointer to previous cp start. */
const char *utf8_prev(const char *s, const char *start) {
    if (!s || !start || s <= start) return NULL;
    const char *p = s - 1;
    while (p > start && (*p & 0xC0) == 0x80) p--;
    return p;
}

/* Unicode character properties */
bool unicode_is_alpha(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 0xC0 && cp <= 0x024F) return true;  /* Latin Extended */
    if (cp >= 0x0400 && cp <= 0x04FF) return true; /* Cyrillic */
    if (cp >= 0x3040 && cp <= 0x309F) return true; /* Hiragana */
    if (cp >= 0x30A0 && cp <= 0x30FF) return true; /* Katakana */
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true; /* CJK Unified */
    if (cp >= 0x0600 && cp <= 0x06FF) return true; /* Arabic */
    return false;
}

bool unicode_is_digit(uint32_t cp) {
    if (cp >= '0' && cp <= '9') return true;
    if (cp >= 0x0660 && cp <= 0x0669) return true; /* Arabic-Indic digits */
    return false;
}

bool unicode_is_space(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return true;
    if (cp == 0x00A0) return true;  /* NBSP */
    if (cp == 0x2000 || cp == 0x2001 || cp == 0x2002 || cp == 0x2003)
        return true; /* various typographic spaces */
    if (cp == 0x3000) return true;  /* ideographic space */
    return false;
}

uint32_t unicode_to_upper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)
        return cp - 32;
    return cp;
}

uint32_t unicode_to_lower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)
        return cp + 32;
    return cp;
}

/* Bidirectional text detection */
bool unicode_is_rtl(uint32_t cp) {
    /* Arabic block */
    if (cp >= 0x0600 && cp <= 0x06FF) return true;
    /* Hebrew block */
    if (cp >= 0x0590 && cp <= 0x05FF) return true;
    /* Arabic Supplement + Extended */
    if (cp >= 0x0750 && cp <= 0x077F) return true;
    if (cp >= 0x08A0 && cp <= 0x08FF) return true;
    /* Arabic Presentation Forms */
    if (cp >= 0xFB50 && cp <= 0xFDFF) return true;
    if (cp >= 0xFE70 && cp <= 0xFEFF) return true;
    return false;
}
