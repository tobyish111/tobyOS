/* dnd.h -- Drag-and-drop between windows.
 *
 * Window manager extension: tracks a single active drag operation,
 * dispatches DND_OFFER / DND_DROP events to target windows, and
 * provides a polling API for target apps to accept payloads.
 */

#ifndef TOBYOS_DND_H
#define TOBYOS_DND_H

#include <tobyos/types.h>

#define DND_TYPE_FILE    0
#define DND_TYPE_TEXT    1
#define DND_TYPE_IMAGE   2

#define DND_DATA_MAX     512

struct dnd_offer {
    int  type;
    int  source_wid;
    char data[DND_DATA_MAX];
    int  data_len;
    int  x, y;
    int  accepted;
};

void dnd_init(void);
int  dnd_begin(int source_wid, int type, const char *data, int len);
int  dnd_update(int x, int y);
int  dnd_drop(int x, int y);
void dnd_cancel(void);
int  dnd_get_offer(int target_wid, struct dnd_offer *out);
int  dnd_accept(int target_wid);
int  dnd_is_active(void);

#endif /* TOBYOS_DND_H */
