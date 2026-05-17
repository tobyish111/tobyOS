/* ssh.h -- TCP/22 SSH service entry points.
 *
 * This is the protocol-facing service skeleton. It handles SSH version
 * exchange and sends a standards-shaped disconnect while the full
 * transport crypto/auth/channel stack is being brought up.
 */

#ifndef TOBYOS_SSH_H
#define TOBYOS_SSH_H

#ifndef SSH_PORT
#define SSH_PORT 22u
#endif

void ssh_init(void);
void ssh_poll(void);

#endif /* TOBYOS_SSH_H */
