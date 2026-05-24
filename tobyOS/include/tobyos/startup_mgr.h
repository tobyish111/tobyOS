/* startup_mgr.h -- control which apps launch at login. */

#ifndef TOBYOS_STARTUP_MGR_H
#define TOBYOS_STARTUP_MGR_H

#include <tobyos/types.h>

#define STARTUP_MAX  16

struct startup_entry {
    char name[32];
    char path[128];
    int  enabled;
    int  delay_ms;
};

void startup_mgr_init(void);
int  startup_add(const char *name, const char *path, int delay_ms);
int  startup_remove(const char *name);
int  startup_enable(const char *name, int enabled);
int  startup_list(struct startup_entry *out, int max);
void startup_run_all(void);

#endif /* TOBYOS_STARTUP_MGR_H */
