/* utmp.h -- the login records in /run/utmp (src/utmp.c). */
#ifndef TOBYOS_UTMP_H
#define TOBYOS_UTMP_H

#include <tobyos/types.h>

/* Boot: /run and /var as tmpfs (0755), /var/run -> /run, /var/tmp, and
 * /run/utmp with the boot and run-level records. */
void utmp_init(void);

/* A login on LINE ("pts/3", ":0") by USER from HOST ("" for none; IPV4,
 * network order, or 0), its process PID. Replaces a record for the line. */
void utmp_login(int pid, const char *line, const char *user,
                const char *host, uint32_t ipv4_be);

/* The login whose process is PID is over: its record becomes a dead
 * process's, as logout(3) leaves one. */
void utmp_logout(int pid);

/* Write /run/utmp again -- the boot record follows the clock when it is
 * set, as the boot time does. */
void utmp_refresh(void);

#endif
