/* include/tobyos/unique_name.h -- one rule for "that name is taken".
 *
 * Shared by /bin/cp (-U) and File Explorer, for the same reason
 * <tobyos/file_types.h> is shared: the desktop and the shell must not invent
 * different names for the same operation. Explorer used to produce
 * "report (copy).txt" while nothing in the shell could do it at all, so the
 * two could not even be compared.
 *
 * The rule: report.txt -> report_1.txt -> report_2.txt ...
 *           "File x"   -> "File x_1"   -> "File x_2"   ...
 *
 * Pure string building, no filesystem: this header has no idea what exists.
 * The caller loops nth = 1, 2, 3... and stops at the first name its own
 * exists-check clears, which keeps the header usable from the kernel, from
 * freestanding userland, and from a host test.
 *
 * Deliberate choices:
 *
 *   - The suffix goes on the STEM, before the extension, so the copy is
 *     still the same kind of file. "report_1.txt" opens in the editor;
 *     "report.txt_1" opens in nothing.
 *   - Copying "report_1.txt" yields "report_1_1.txt", NOT "report_2.txt".
 *     Parsing a trailing _N and incrementing it would be prettier, but "_1"
 *     is a perfectly ordinary thing to have in a real filename (chapter_1,
 *     disk_1), and renaming someone's chapter_1 copy to chapter_2 would put
 *     it where a DIFFERENT real file probably belongs. Ugly beats wrong.
 *   - A leading dot is not an extension: ".gitignore" is a whole name, so it
 *     becomes ".gitignore_1" rather than "_1.gitignore".
 *   - Only the LAST dot splits, so "archive.tar.gz" -> "archive.tar_1.gz".
 *     Compound extensions would need a list of them, and a wrong guess there
 *     mangles the name; this is at least predictable.
 *   - When the name would not fit, the STEM is truncated, never the suffix
 *     or the extension. A name that lost its "_1" would collide with the
 *     file it was supposed to avoid, which is the one outcome that must not
 *     happen.
 */

#ifndef TOBYOS_UNIQUE_NAME_H
#define TOBYOS_UNIQUE_NAME_H

/* Build the nth candidate for `base` into `out` (capacity `cap`, including
 * the NUL). nth == 0 copies `base` through unchanged, so a caller can run
 * the loop from 0 and get "the original name first".
 *
 * Returns 1 on success, 0 if `out` is too small to hold even a truncated
 * stem plus the suffix and extension -- in which case `out` is left empty
 * rather than holding a name that does not mean what it says. */
static int unique_name_nth(char *out, unsigned cap,
                           const char *base, unsigned nth)
{
    unsigned i, blen = 0, dot, stem_len, ext_len, need, digits, n;
    char suf[16];

    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!base || !base[0]) return 0;
    while (base[blen]) blen++;

    if (nth == 0) {
        if (blen + 1 > cap) return 0;
        for (i = 0; i <= blen; i++) out[i] = base[i];
        return 1;
    }

    /* Split at the last dot. Index 0 does not count -- a dotfile's dot is
     * part of its name, not the start of an extension. */
    dot = blen;
    for (i = 1; i < blen; i++) if (base[i] == '.') dot = i;
    stem_len = dot;
    ext_len  = blen - dot;

    /* "_" + decimal nth, built without printf so this stays freestanding. */
    digits = 0; n = nth;
    while (n) { digits++; n /= 10; }
    if (digits == 0) digits = 1;
    if (digits + 2 > (unsigned)sizeof(suf)) return 0;
    suf[0] = '_';
    n = nth;
    for (i = 0; i < digits; i++) { suf[1 + digits - 1 - i] = (char)('0' + n % 10); n /= 10; }
    suf[1 + digits] = '\0';

    /* Suffix and extension are non-negotiable; the stem gives way. */
    need = 1 + digits + ext_len + 1;              /* + NUL */
    if (need > cap) return 0;
    if (stem_len + need > cap) stem_len = cap - need;
    if (stem_len == 0) return 0;                  /* nothing recognisable left */

    unsigned p = 0;
    for (i = 0; i < stem_len; i++) out[p++] = base[i];
    for (i = 0; i < 1u + digits; i++) out[p++] = suf[i];
    for (i = 0; i < ext_len; i++) out[p++] = base[dot + i];
    out[p] = '\0';
    return 1;
}

#endif /* TOBYOS_UNIQUE_NAME_H */
