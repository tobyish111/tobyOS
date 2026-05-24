/* libtoby/src/unicode.c -- Unicode support (Phase 6).
 *
 * UTF-8 codec, character properties, bidi classification, and
 * basic NFC composition for Latin + common scripts.
 */

#include <toby/unicode.h>

/* ---- UTF-8 decode/encode ---- */

int utf8_decode(const char *s, int *bytes_consumed) {
    if (!s) { if (bytes_consumed) *bytes_consumed = 0; return -1; }

    unsigned char c = (unsigned char)s[0];
    int cp, len;

    if (c == 0) {
        if (bytes_consumed) *bytes_consumed = 0;
        return 0;
    }
    if (c < 0x80) {
        cp = c; len = 1;
    } else if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1F; len = 2;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0F; len = 3;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07; len = 4;
    } else {
        if (bytes_consumed) *bytes_consumed = 1;
        return -1;  /* Invalid lead byte */
    }

    for (int i = 1; i < len; i++) {
        unsigned char cont = (unsigned char)s[i];
        if ((cont & 0xC0) != 0x80) {
            if (bytes_consumed) *bytes_consumed = i;
            return -1;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }

    /* Reject overlong encodings and surrogates */
    if ((len == 2 && cp < 0x80) ||
        (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) ||
        cp > 0x10FFFF) {
        if (bytes_consumed) *bytes_consumed = len;
        return -1;
    }

    if (bytes_consumed) *bytes_consumed = len;
    return cp;
}

int utf8_encode(int codepoint, char *out) {
    if (!out) return 0;
    if (codepoint < 0 || codepoint > 0x10FFFF) return 0;
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return 0;

    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

int utf8_strlen(const char *s) {
    if (!s) return 0;
    int count = 0;
    while (*s) {
        int consumed;
        int cp = utf8_decode(s, &consumed);
        if (cp < 0 || consumed <= 0) { s++; continue; }
        s += consumed;
        count++;
    }
    return count;
}

const char *utf8_advance(const char *s, int n) {
    if (!s) return s;
    while (n > 0 && *s) {
        int consumed;
        utf8_decode(s, &consumed);
        if (consumed <= 0) { s++; n--; continue; }
        s += consumed;
        n--;
    }
    return s;
}

int utf8_char_len(const char *s) {
    if (!s || !*s) return 0;
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return -1;
}

/* ---- Character classification ---- */

int unicode_is_letter(int cp) {
    if (cp >= 'A' && cp <= 'Z') return 1;
    if (cp >= 'a' && cp <= 'z') return 1;
    /* Latin-1 Supplement letters */
    if (cp >= 0xC0 && cp <= 0xD6) return 1;
    if (cp >= 0xD8 && cp <= 0xF6) return 1;
    if (cp >= 0xF8 && cp <= 0xFF) return 1;
    /* Latin Extended-A/B */
    if (cp >= 0x100 && cp <= 0x24F) return 1;
    /* Greek */
    if (cp >= 0x370 && cp <= 0x3FF) return 1;
    /* Cyrillic */
    if (cp >= 0x400 && cp <= 0x4FF) return 1;
    /* Arabic */
    if (cp >= 0x600 && cp <= 0x6FF) return 1;
    /* CJK Unified Ideographs */
    if (cp >= 0x4E00 && cp <= 0x9FFF) return 1;
    /* Hiragana */
    if (cp >= 0x3040 && cp <= 0x309F) return 1;
    /* Katakana */
    if (cp >= 0x30A0 && cp <= 0x30FF) return 1;
    /* Hangul Syllables */
    if (cp >= 0xAC00 && cp <= 0xD7AF) return 1;
    return 0;
}

int unicode_is_digit(int cp) {
    if (cp >= '0' && cp <= '9') return 1;
    /* Arabic-Indic digits */
    if (cp >= 0x0660 && cp <= 0x0669) return 1;
    /* Extended Arabic-Indic digits */
    if (cp >= 0x06F0 && cp <= 0x06F9) return 1;
    /* Devanagari digits */
    if (cp >= 0x0966 && cp <= 0x096F) return 1;
    return 0;
}

int unicode_is_space(int cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
        cp == '\f' || cp == '\v') return 1;
    if (cp == 0xA0) return 1;   /* No-break space */
    if (cp == 0x2000 || cp == 0x2001 || cp == 0x2002 || cp == 0x2003 ||
        cp == 0x2004 || cp == 0x2005 || cp == 0x2006 || cp == 0x2007 ||
        cp == 0x2008 || cp == 0x2009 || cp == 0x200A) return 1;
    if (cp == 0x2028 || cp == 0x2029) return 1;  /* Line/paragraph separator */
    if (cp == 0x202F || cp == 0x205F || cp == 0x3000) return 1;
    return 0;
}

int unicode_is_upper(int cp) {
    if (cp >= 'A' && cp <= 'Z') return 1;
    if (cp >= 0xC0 && cp <= 0xD6) return 1;
    if (cp >= 0xD8 && cp <= 0xDE) return 1;
    /* Latin Extended-A uppercase (even codepoints 0x100..0x12E) */
    if (cp >= 0x100 && cp <= 0x12E && (cp & 1) == 0) return 1;
    /* Greek uppercase */
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return 1;
    /* Cyrillic uppercase */
    if (cp >= 0x410 && cp <= 0x42F) return 1;
    return 0;
}

int unicode_is_lower(int cp) {
    if (cp >= 'a' && cp <= 'z') return 1;
    if (cp >= 0xE0 && cp <= 0xF6) return 1;
    if (cp >= 0xF8 && cp <= 0xFE) return 1;
    /* Latin Extended-A lowercase (odd codepoints 0x101..0x12F) */
    if (cp >= 0x101 && cp <= 0x12F && (cp & 1) == 1) return 1;
    /* Greek lowercase */
    if (cp >= 0x3B1 && cp <= 0x3C9) return 1;
    /* Cyrillic lowercase */
    if (cp >= 0x430 && cp <= 0x44F) return 1;
    return 0;
}

/* ---- Case conversion ---- */

int unicode_to_upper(int cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp >= 0xE0 && cp <= 0xF6) return cp - 32;
    if (cp >= 0xF8 && cp <= 0xFE) return cp - 32;
    /* Latin Extended-A */
    if (cp >= 0x101 && cp <= 0x12F && (cp & 1) == 1) return cp - 1;
    /* Greek lowercase -> uppercase */
    if (cp >= 0x3B1 && cp <= 0x3C9) return cp - 32;
    /* Cyrillic */
    if (cp >= 0x430 && cp <= 0x44F) return cp - 32;
    return cp;
}

int unicode_to_lower(int cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0xC0 && cp <= 0xD6) return cp + 32;
    if (cp >= 0xD8 && cp <= 0xDE) return cp + 32;
    /* Latin Extended-A */
    if (cp >= 0x100 && cp <= 0x12E && (cp & 1) == 0) return cp + 1;
    /* Greek uppercase -> lowercase */
    if (cp >= 0x391 && cp <= 0x3A1) return cp + 32;
    if (cp >= 0x3A3 && cp <= 0x3A9) return cp + 32;
    /* Cyrillic */
    if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
    return cp;
}

/* ---- Bidirectional text classification ---- */

enum bidi_class unicode_bidi_class(int cp) {
    /* Arabic */
    if (cp >= 0x0600 && cp <= 0x06FF) return BIDI_AL;
    if (cp >= 0x0750 && cp <= 0x077F) return BIDI_AL;
    if (cp >= 0xFB50 && cp <= 0xFDFF) return BIDI_AL;
    if (cp >= 0xFE70 && cp <= 0xFEFF) return BIDI_AL;

    /* Hebrew */
    if (cp >= 0x0590 && cp <= 0x05FF) return BIDI_R;
    if (cp >= 0xFB1D && cp <= 0xFB4F) return BIDI_R;

    /* Arabic-Indic digits */
    if (cp >= 0x0660 && cp <= 0x0669) return BIDI_AN;
    if (cp >= 0x06F0 && cp <= 0x06F9) return BIDI_AN;

    /* European digits */
    if (cp >= '0' && cp <= '9') return BIDI_EN;

    /* Whitespace */
    if (unicode_is_space(cp)) return BIDI_WS;

    /* Default: left-to-right for Latin/CJK/most scripts */
    if (unicode_is_letter(cp)) return BIDI_L;

    return BIDI_ON;
}

/* ---- NFC composition (common Latin precomposed characters) ---- */

struct compose_pair {
    int base;
    int combining;
    int composed;
};

static const struct compose_pair g_compose_table[] = {
    /* Latin letters with diacritics */
    { 'A', 0x0300, 0x00C0 },  /* A + grave -> À */
    { 'A', 0x0301, 0x00C1 },  /* A + acute -> Á */
    { 'A', 0x0302, 0x00C2 },  /* A + circumflex -> Â */
    { 'A', 0x0303, 0x00C3 },  /* A + tilde -> Ã */
    { 'A', 0x0308, 0x00C4 },  /* A + diaeresis -> Ä */
    { 'A', 0x030A, 0x00C5 },  /* A + ring -> Å */
    { 'C', 0x0327, 0x00C7 },  /* C + cedilla -> Ç */
    { 'E', 0x0300, 0x00C8 },  /* E + grave -> È */
    { 'E', 0x0301, 0x00C9 },  /* E + acute -> É */
    { 'E', 0x0302, 0x00CA },  /* E + circumflex -> Ê */
    { 'E', 0x0308, 0x00CB },  /* E + diaeresis -> Ë */
    { 'I', 0x0300, 0x00CC },  /* I + grave -> Ì */
    { 'I', 0x0301, 0x00CD },  /* I + acute -> Í */
    { 'I', 0x0302, 0x00CE },  /* I + circumflex -> Î */
    { 'I', 0x0308, 0x00CF },  /* I + diaeresis -> Ï */
    { 'N', 0x0303, 0x00D1 },  /* N + tilde -> Ñ */
    { 'O', 0x0300, 0x00D2 },  /* O + grave -> Ò */
    { 'O', 0x0301, 0x00D3 },  /* O + acute -> Ó */
    { 'O', 0x0302, 0x00D4 },  /* O + circumflex -> Ô */
    { 'O', 0x0303, 0x00D5 },  /* O + tilde -> Õ */
    { 'O', 0x0308, 0x00D6 },  /* O + diaeresis -> Ö */
    { 'U', 0x0300, 0x00D9 },  /* U + grave -> Ù */
    { 'U', 0x0301, 0x00DA },  /* U + acute -> Ú */
    { 'U', 0x0302, 0x00DB },  /* U + circumflex -> Û */
    { 'U', 0x0308, 0x00DC },  /* U + diaeresis -> Ü */
    { 'Y', 0x0301, 0x00DD },  /* Y + acute -> Ý */
    { 'a', 0x0300, 0x00E0 },  /* a + grave -> à */
    { 'a', 0x0301, 0x00E1 },  /* a + acute -> á */
    { 'a', 0x0302, 0x00E2 },  /* a + circumflex -> â */
    { 'a', 0x0303, 0x00E3 },  /* a + tilde -> ã */
    { 'a', 0x0308, 0x00E4 },  /* a + diaeresis -> ä */
    { 'a', 0x030A, 0x00E5 },  /* a + ring -> å */
    { 'c', 0x0327, 0x00E7 },  /* c + cedilla -> ç */
    { 'e', 0x0300, 0x00E8 },  /* e + grave -> è */
    { 'e', 0x0301, 0x00E9 },  /* e + acute -> é */
    { 'e', 0x0302, 0x00EA },  /* e + circumflex -> ê */
    { 'e', 0x0308, 0x00EB },  /* e + diaeresis -> ë */
    { 'i', 0x0300, 0x00EC },  /* i + grave -> ì */
    { 'i', 0x0301, 0x00ED },  /* i + acute -> í */
    { 'i', 0x0302, 0x00EE },  /* i + circumflex -> î */
    { 'i', 0x0308, 0x00EF },  /* i + diaeresis -> ï */
    { 'n', 0x0303, 0x00F1 },  /* n + tilde -> ñ */
    { 'o', 0x0300, 0x00F2 },  /* o + grave -> ò */
    { 'o', 0x0301, 0x00F3 },  /* o + acute -> ó */
    { 'o', 0x0302, 0x00F4 },  /* o + circumflex -> ô */
    { 'o', 0x0303, 0x00F5 },  /* o + tilde -> õ */
    { 'o', 0x0308, 0x00F6 },  /* o + diaeresis -> ö */
    { 'u', 0x0300, 0x00F9 },  /* u + grave -> ù */
    { 'u', 0x0301, 0x00FA },  /* u + acute -> ú */
    { 'u', 0x0302, 0x00FB },  /* u + circumflex -> û */
    { 'u', 0x0308, 0x00FC },  /* u + diaeresis -> ü */
    { 'y', 0x0301, 0x00FD },  /* y + acute -> ý */
    { 'y', 0x0308, 0x00FF },  /* y + diaeresis -> ÿ */
    { 0, 0, 0 }
};

int unicode_compose(int base, int combining) {
    for (int i = 0; g_compose_table[i].base != 0; i++) {
        if (g_compose_table[i].base == base &&
            g_compose_table[i].combining == combining) {
            return g_compose_table[i].composed;
        }
    }
    return 0;
}
