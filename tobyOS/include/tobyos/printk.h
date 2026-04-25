/* printk.h -- formatted kernel logging.
 *
 * Output is fanned out to:
 *   - QEMU debugcon (0xE9)  -- always
 *   - COM1 serial            -- after serial_init()
 *   - framebuffer console    -- after console_init() succeeds
 *
 * Supported conversions:
 *   %s   const char *
 *   %c   int (rendered as char)
 *   %d   int (signed decimal)
 *   %u   unsigned int
 *   %lu  unsigned long / uint64_t
 *   %ld  long / int64_t
 *   %x   unsigned int (lowercase hex, no prefix)
 *   %lx  unsigned long (lowercase hex)
 *   %p   void *        (rendered as 0x%016lx)
 *   %%   literal '%'
 *
 * Width + zero-padding is supported between '%' and the conversion, e.g.
 *   "%08x" -> hex padded to 8 cols with leading zeros.
 */

#ifndef TOBYOS_PRINTK_H
#define TOBYOS_PRINTK_H

#include <tobyos/types.h>
#include <stdarg.h>

void kputs(const char *s);
void kputc(char c);

__attribute__((format(printf, 1, 2)))
void kprintf(const char *fmt, ...);

void kvprintf(const char *fmt, va_list ap);

/* Optional "mirror" sink for kprintf output. When installed, every
 * byte emitted by kprintf/kputs/kputc is ALSO delivered to `cb(ctx, c)`
 * in addition to the normal serial+console fan-out. Used by the GUI
 * terminal to capture the output of kernel builtins (e.g. `pkg`) so
 * the user sees it in the terminal window instead of only in
 * serial.log. Pass cb=NULL to clear.
 *
 * Not thread-safe; intended to be set for the duration of a single
 * synchronous call and cleared immediately afterwards. Concurrent
 * kprintf from other CPUs during the window may leak into the sink
 * -- that is acceptable for the usage pattern (interactive commands). */
void printk_set_sink(void (*cb)(void *ctx, char c), void *ctx);

#endif /* TOBYOS_PRINTK_H */
