/* svc_mgr.h -- Kernel-side service manager for tobyOS.
 *
 * Registry of system services with dependency tracking, automatic
 * restart with exponential backoff, and topological ordering.
 */

#ifndef TOBYOS_SVC_MGR_H
#define TOBYOS_SVC_MGR_H

#include <stdint.h>

#define SVC_MAX        16
#define SVC_NAME_MAX   32
#define SVC_PATH_MAX   64
#define SVC_DEP_MAX     4

enum svc_state {
    SVC_STOPPED  = 0,
    SVC_STARTING = 1,
    SVC_RUNNING  = 2,
    SVC_STOPPING = 3,
    SVC_CRASHED  = 4,
};

struct service {
    char            name[SVC_NAME_MAX];
    char            path[SVC_PATH_MAX];
    char            deps[SVC_DEP_MAX][SVC_NAME_MAX];
    int             dep_count;
    enum svc_state  state;
    int             pid;
    int             restart_count;
    int             auto_restart;
    uint32_t        start_time;
    uint32_t        last_restart;
    uint32_t        backoff_ms;
};

void           svc_mgr_init(void);
int            svc_register(const char *name, const char *path,
                            int auto_restart);
int            svc_add_dep(const char *service, const char *depends_on);
int            svc_start(const char *name);
int            svc_stop(const char *name);
int            svc_restart(const char *name);
enum svc_state svc_get_state(const char *name);
int            svc_start_all(void);
void           svc_handle_exit(int pid, int status);
int            svc_list(struct service *out, int max);

#endif /* TOBYOS_SVC_MGR_H */
