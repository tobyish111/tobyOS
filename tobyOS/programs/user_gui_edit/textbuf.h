/* textbuf.h -- turn a file image into the editor's line array.
 *
 * Split out of main.c for ONE reason: this is where a text editor silently
 * loses data, and a GUI program cannot be driven headlessly. As a header it
 * is compiled into both the editor and a host test, so the truncation rules
 * below are exercised by something other than a human with a keyboard.
 *
 * The contract: parse as much as fits, and report HONESTLY whether anything
 * did not. Callers must refuse to save a buffer that came back partial --
 * writing it over the original with O_TRUNC deletes the remainder. */
#ifndef TOBY_GUI_EDIT_TEXTBUF_H
#define TOBY_GUI_EDIT_TEXTBUF_H

/* Why the buffer is partial, or 0 when it holds the whole file. */
struct textbuf_result { int truncated; const char *why; int line_count; };

/* `lines` is [max_lines][max_cols]. `total` bytes of `src` are parsed;
 * `more` says the reader knew there were further bytes it could not take. */
static void textbuf_parse(char *lines, int max_lines, int max_cols,
                          const char *src, long total, int more,
                          struct textbuf_result *out) {
    int line_count = 0, col = 0;
    out->truncated = 0; out->why = 0;
    if (more) { out->truncated = 1; out->why = "file is larger than the editor buffer"; }

    for (long i = 0; i < total; i++) {
        if (line_count >= max_lines) {
            out->truncated = 1;
            out->why = "file has more lines than the editor holds";
            break;
        }
        char *row = lines + (long)line_count * max_cols;
        if (src[i] == '\n')      { row[col] = '\0'; line_count++; col = 0; }
        else if (src[i] == '\r') { continue; }
        else if (col < max_cols - 1) { row[col++] = src[i]; }
        else {
            /* Keep parsing so the LINE COUNT stays right; only the overflow
             * characters are dropped, and the flag records that they were. */
            out->truncated = 1;
            out->why = "a line is longer than the editor holds";
        }
    }
    /* Trailing line with no newline, and the empty-file case. */
    if (line_count < max_lines && (col > 0 || line_count == 0)) {
        char *row = lines + (long)line_count * max_cols;
        row[col] = '\0'; line_count++;
    }
    if (line_count == 0) line_count = 1;
    out->line_count = line_count;
}
#endif
