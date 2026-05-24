/* autoupdate.h -- background service checking for system/app updates. */

#ifndef TOBYOS_AUTOUPDATE_H
#define TOBYOS_AUTOUPDATE_H

#include <tobyos/types.h>

#define AUTOUPDATE_MAX  16

struct update_entry {
    int      id;
    char     name[32];
    char     cur_version[16];
    char     new_version[16];
    uint32_t size_kb;
    int      downloaded;
    int      applied;
};

void autoupdate_init(void);
int  autoupdate_check(void);
int  autoupdate_download(int id);
int  autoupdate_apply(int id);
void autoupdate_set_enabled(int enabled);
int  autoupdate_is_enabled(void);
int  autoupdate_list(struct update_entry *out, int max);
void autoupdate_tick(void);

#endif /* TOBYOS_AUTOUPDATE_H */
