/* console.h -- text console on top of the Limine framebuffer.
 *
 * Owns a (col, row) cursor and a scroll-up-on-overflow policy. Each glyph
 * cell is exactly 8x8 pixels so cols/rows derive directly from the
 * framebuffer dimensions.
 */

#ifndef TOBYOS_CONSOLE_H
#define TOBYOS_CONSOLE_H

#include <tobyos/types.h>

/* Initialise the console against the given framebuffer.
 * fb must point to a 32-bpp linear framebuffer; pitch is in bytes per row.
 * Returns false if the geometry is unusable. */
bool console_init(void *fb, uint64_t pitch, uint64_t width, uint64_t height);

/* True iff console_init has been called and succeeded. printk uses this
 * to decide whether to fan out to the framebuffer. */
bool console_ready(void);

void console_clear(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_puts(const char *s);

/* Override the foreground colour used by subsequent writes (0x00RRGGBB). */
void console_set_color(uint32_t fg);

/* Cursor / geometry introspection -- the shell uses these to drive
 * line editing safely when the framebuffer is up. */
void console_get_cursor(uint32_t *col, uint32_t *row);
void console_set_cursor(uint32_t  col, uint32_t  row);
void console_get_size  (uint32_t *cols, uint32_t *rows);

/* Show/hide the visible blinking cursor (default: shown). */
void console_show_cursor(bool visible);

/* Erase the cell to the left of the cursor and move there. Used by the
 * shell line editor; printk's '\b' stays non-destructive. */
void console_backspace(void);

/* Drive cursor blink. Pass the current tick count (any monotonic source
 * is fine -- we only diff and threshold it). Cheap when nothing changes. */
void console_tick(uint64_t ticks, uint32_t hz);

#endif /* TOBYOS_CONSOLE_H */
