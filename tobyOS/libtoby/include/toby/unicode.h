/* toby/unicode.h -- Unicode support library (Phase 6).
 *
 * UTF-8 decode/encode, character classification, case conversion,
 * bidirectional text classification, and basic NFC composition.
 */

#ifndef TOBY_UNICODE_H
#define TOBY_UNICODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- UTF-8 codec ---- */

/* Decode one codepoint from a UTF-8 string. Returns the codepoint
 * value and sets *bytes_consumed to the number of bytes read (1-4).
 * Returns -1 on invalid sequence. */
int utf8_decode(const char *s, int *bytes_consumed);

/* Encode a codepoint as UTF-8 into `out` (must have room for 4 bytes).
 * Returns the number of bytes written (1-4), or 0 on invalid codepoint. */
int utf8_encode(int codepoint, char *out);

/* Count the number of codepoints in a UTF-8 string. */
int utf8_strlen(const char *s);

/* Advance past `n` codepoints. Returns pointer into the string after
 * skipping n characters, or end-of-string if fewer than n remain. */
const char *utf8_advance(const char *s, int n);

/* Return the byte length of the next UTF-8 character (1-4) without
 * fully decoding it. Returns 0 on NUL, -1 on invalid lead byte. */
int utf8_char_len(const char *s);

/* ---- Character classification ---- */

int unicode_is_letter(int cp);
int unicode_is_digit(int cp);
int unicode_is_space(int cp);
int unicode_is_upper(int cp);
int unicode_is_lower(int cp);

/* ---- Case conversion ---- */

int unicode_to_upper(int cp);
int unicode_to_lower(int cp);

/* ---- Bidirectional text (simplified) ---- */

enum bidi_class {
    BIDI_L,     /* Left-to-Right */
    BIDI_R,     /* Right-to-Left */
    BIDI_AL,    /* Arabic Letter */
    BIDI_EN,    /* European Number */
    BIDI_AN,    /* Arabic Number */
    BIDI_WS,    /* Whitespace */
    BIDI_ON     /* Other Neutral */
};

enum bidi_class unicode_bidi_class(int cp);

/* ---- Normalization (NFC, simplified) ---- */

/* Attempt to compose a base character with a combining mark.
 * Returns the composed codepoint, or 0 if no composition exists. */
int unicode_compose(int base, int combining);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_UNICODE_H */
