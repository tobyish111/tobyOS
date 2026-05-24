/* clipboard.c -- Phase 2 M2.7: kernel-side clipboard system.
 *
 * A single system-wide clipboard buffer (64KB max) that any process can
 * write to (copy) or read from (paste). Supports format tagging so
 * future rich-text and image clipboard operations can coexist with
 * plain text without collision. */

#include <tobyos/clipboard.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#define CLIP_BUF_SIZE  ABI_CLIP_MAX_SIZE  /* 64KB */

static struct {
    char     buf[CLIP_BUF_SIZE];
    uint32_t len;
    uint32_t format;   /* ABI_CLIP_FMT_* */
    bool     occupied;
} g_clip;

void clipboard_init(void) {
    g_clip.len      = 0;
    g_clip.format   = ABI_CLIP_FMT_TEXT;
    g_clip.occupied = false;
    kprintf("[clipboard] init: %u bytes available\n", CLIP_BUF_SIZE);
}

long clipboard_set(const char *data, uint32_t len, uint32_t format) {
    if (!data) return -ABI_EFAULT;
    if (format > ABI_CLIP_FMT_IMAGE) return -ABI_EINVAL;
    if (len > CLIP_BUF_SIZE) len = CLIP_BUF_SIZE;

    memcpy(g_clip.buf, data, len);
    g_clip.len      = len;
    g_clip.format   = format;
    g_clip.occupied = true;
    return (long)len;
}

long clipboard_get(char *buf, uint32_t buf_sz, uint32_t format) {
    if (!buf || buf_sz == 0) return -ABI_EINVAL;
    if (!g_clip.occupied) return 0;
    if (format != g_clip.format) return 0;

    uint32_t copy = g_clip.len;
    if (copy > buf_sz) copy = buf_sz;
    memcpy(buf, g_clip.buf, copy);
    return (long)copy;
}

long clipboard_clear(void) {
    g_clip.len      = 0;
    g_clip.format   = ABI_CLIP_FMT_TEXT;
    g_clip.occupied = false;
    return 0;
}
