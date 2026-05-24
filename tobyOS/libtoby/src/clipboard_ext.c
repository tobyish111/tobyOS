/* clipboard_ext.c -- Milestone 7 enhanced clipboard (libtoby).
 *
 * Multi-format clipboard with history ring of 25 entries.
 * Uses the kernel's ABI_SYS_CLIP_SET / ABI_SYS_CLIP_GET syscalls
 * internally but adds format multiplexing and local history.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <toby/clipboard.h>
#include "libtoby_internal.h"

struct clip_entry {
    char     data[CLIP_ENTRY_MAX];
    size_t   len;
    int      format;
    int      valid;
};

static struct {
    struct clip_entry history[CLIP_HISTORY_MAX];
    int    head;
    int    count;
} g_clip_ext;

int toby_clip_copy(int format, const void *data, size_t len) {
    if (!data || len == 0) return -1;
    if (format < CLIP_TEXT || format > CLIP_HTML) return -1;
    if (len > CLIP_ENTRY_MAX) len = CLIP_ENTRY_MAX;

    /* Store in local history ring */
    int slot = g_clip_ext.head;
    memcpy(g_clip_ext.history[slot].data, data, len);
    g_clip_ext.history[slot].len = len;
    g_clip_ext.history[slot].format = format;
    g_clip_ext.history[slot].valid = 1;

    g_clip_ext.head = (g_clip_ext.head + 1) % CLIP_HISTORY_MAX;
    if (g_clip_ext.count < CLIP_HISTORY_MAX)
        g_clip_ext.count++;

    /* Also push to kernel clipboard for cross-process sharing */
    uint32_t kern_fmt = (uint32_t)format;
    toby_sc3(138 /* ABI_SYS_CLIP_SET */, (long)(uintptr_t)data,
             (long)len, (long)kern_fmt);

    return 0;
}

size_t toby_clip_paste(int format, void *buf, size_t max) {
    if (!buf || max == 0) return 0;

    /* Try kernel clipboard first */
    long rv = toby_sc3(139 /* ABI_SYS_CLIP_GET */, (long)(uintptr_t)buf,
                       (long)max, (long)(uint32_t)format);
    if (rv > 0) return (size_t)rv;

    /* Fall back to local history (most recent matching format) */
    for (int i = 0; i < g_clip_ext.count; i++) {
        int idx = (g_clip_ext.head - 1 - i + CLIP_HISTORY_MAX) % CLIP_HISTORY_MAX;
        if (g_clip_ext.history[idx].valid &&
            g_clip_ext.history[idx].format == format) {
            size_t copy = g_clip_ext.history[idx].len;
            if (copy > max) copy = max;
            memcpy(buf, g_clip_ext.history[idx].data, copy);
            return copy;
        }
    }
    return 0;
}

int toby_clip_history_get(int index, int *format, void *buf, size_t max) {
    if (index < 0 || index >= g_clip_ext.count) return -1;
    if (!buf || max == 0) return -1;

    int idx = (g_clip_ext.head - 1 - index + CLIP_HISTORY_MAX) % CLIP_HISTORY_MAX;
    if (!g_clip_ext.history[idx].valid) return -1;

    size_t copy = g_clip_ext.history[idx].len;
    if (copy > max) copy = max;
    memcpy(buf, g_clip_ext.history[idx].data, copy);
    if (format) *format = g_clip_ext.history[idx].format;
    return (int)copy;
}

int toby_clip_history_count(void) {
    return g_clip_ext.count;
}

void toby_clip_clear(void) {
    memset(&g_clip_ext, 0, sizeof(g_clip_ext));
    toby_sc0(140 /* ABI_SYS_CLIP_CLEAR */);
}
