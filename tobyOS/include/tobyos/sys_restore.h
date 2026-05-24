/* sys_restore.h -- filesystem snapshots for rollback. */

#ifndef TOBYOS_SYS_RESTORE_H
#define TOBYOS_SYS_RESTORE_H

#include <tobyos/types.h>

#define RESTORE_MAX_POINTS 8

struct restore_point_info {
    uint32_t id;
    uint32_t timestamp;
    char     description[64];
    uint32_t used_blocks;
};

void sys_restore_init(void);
int  restore_create_point(const char *description);
int  restore_list_points(struct restore_point_info *out, int max);
int  restore_rollback(uint32_t id);
int  restore_delete_point(uint32_t id);

#endif /* TOBYOS_SYS_RESTORE_H */
