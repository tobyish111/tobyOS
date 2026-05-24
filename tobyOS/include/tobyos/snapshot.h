#ifndef TOBYOS_SNAPSHOT_H
#define TOBYOS_SNAPSHOT_H

#include <tobyos/types.h>

struct restore_point {
    int      id;
    char     description[64];
    uint32_t timestamp;
    uint32_t size_kb;
};

void snapshot_init(void);
int  snapshot_create(const char *description);
int  snapshot_restore(int id);
int  snapshot_list(struct restore_point *out, int max);
int  snapshot_delete(int id);

#endif /* TOBYOS_SNAPSHOT_H */
