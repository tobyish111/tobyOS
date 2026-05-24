/* uac.h -- User Account Control (privilege escalation) for tobyOS.
 *
 * UAC-equivalent for privileged operations.  When a non-root process
 * tries a privileged operation, it is suspended, a notification is
 * posted to the desktop, and the user must confirm (enter password)
 * to temporarily elevate the process.  Elevation expires after the
 * operation completes or after a timeout (5 minutes).
 */

#ifndef TOBYOS_UAC_H
#define TOBYOS_UAC_H

#include <tobyos/types.h>

#define UAC_MAX_ELEVATED  16
#define UAC_TIMEOUT_MS    (5u * 60u * 1000u)

enum uac_reason {
    UAC_INSTALL_APP    = 0,
    UAC_MODIFY_SYSTEM  = 1,
    UAC_CHANGE_USER    = 2,
    UAC_DRIVER_INSTALL = 3,
};

enum uac_state {
    UAC_STATE_NONE      = 0,
    UAC_STATE_PENDING   = 1,
    UAC_STATE_ELEVATED  = 2,
    UAC_STATE_DENIED    = 3,
    UAC_STATE_EXPIRED   = 4,
};

struct uac_entry {
    int             pid;
    int             uid;
    enum uac_reason reason;
    enum uac_state  state;
    uint64_t        elevate_time_ms;
    uint64_t        expire_time_ms;
};

void uac_init(void);

int  uac_request_elevation(int pid, enum uac_reason reason);
int  uac_is_elevated(int pid);
void uac_drop_elevation(int pid);
int  uac_check_can_elevate(int uid);

void uac_grant(int pid);
void uac_deny(int pid);

void uac_tick(uint64_t now_ms);

int  uac_pending_count(void);
const struct uac_entry *uac_get_pending(int idx);

const char *uac_reason_str(enum uac_reason reason);

#endif /* TOBYOS_UAC_H */
