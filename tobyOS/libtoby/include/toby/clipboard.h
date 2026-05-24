/* clipboard.h -- Milestone 7 enhanced clipboard (libtoby).
 *
 * Multiple formats (text, image, RTF, HTML), clipboard history
 * (last 25 entries), and format-aware copy/paste.
 */

#ifndef TOBY_CLIPBOARD_H
#define TOBY_CLIPBOARD_H

#include <stddef.h>
#include <stdint.h>

#define CLIP_TEXT    0
#define CLIP_IMAGE   1
#define CLIP_RTF     2
#define CLIP_HTML    3

#define CLIP_HISTORY_MAX 25
#define CLIP_ENTRY_MAX   4096

int    toby_clip_copy(int format, const void *data, size_t len);
size_t toby_clip_paste(int format, void *buf, size_t max);
int    toby_clip_history_get(int index, int *format, void *buf, size_t max);
int    toby_clip_history_count(void);
void   toby_clip_clear(void);

#endif /* TOBY_CLIPBOARD_H */
