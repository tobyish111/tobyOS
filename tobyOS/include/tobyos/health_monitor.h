/* health_monitor.h -- background system health checks. */

#ifndef TOBYOS_HEALTH_MONITOR_H
#define TOBYOS_HEALTH_MONITOR_H

#include <tobyos/types.h>

#define HEALTH_LOG_PATH "/var/log/health.log"
#define HEALTH_DISK_WARN_PERCENT  90
#define HEALTH_MEM_WARN_PERCENT   90
#define HEALTH_SVC_CRASH_LIMIT    3

struct health_status {
    int  disk_percent_used;
    int  mem_percent_used;
    int  services_healthy;
    int  services_failed;
    uint32_t uptime_seconds;
    int  warnings_active;
};

void health_monitor_init(void);
void health_check_tick(void);
int  health_get_status(struct health_status *out);

#endif /* TOBYOS_HEALTH_MONITOR_H */
