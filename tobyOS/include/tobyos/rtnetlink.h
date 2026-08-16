/* rtnetlink.h -- the seam between the socket layer and the control plane.
 *
 * socket.c owns the AF_NETLINK socket (its rx ring, its nl_pid, its namespace);
 * src/rtnetlink.c owns everything about the MESSAGES. Keeping the split here
 * means the control plane can be read, tested and changed without touching the
 * socket layer that chrome's network-change notifier and glibc's check_pf run
 * through.
 */

#ifndef TOBYOS_RTNETLINK_H
#define TOBYOS_RTNETLINK_H

#include <tobyos/types.h>

/* Parse every rtnetlink request in `kbuf` (a single sendto payload may hold
 * more than one) and build the whole reply into `out`.
 *
 *   ns      the SOCKET's network namespace -- NULL means the initial one, whose
 *           reply is byte-identical to what shipped before the control plane.
 *           The socket's, not the caller's, so a socket that predates an
 *           unshare keeps answering for the namespace it was made in.
 *   nl_pid  the socket's bound netlink pid. Readers DISCARD any reply whose
 *           nlmsg_pid is not their own (glibc's check_pf does, and a discarded
 *           dump makes getaddrinfo drop every IPv4 answer).
 *
 * Returns the number of bytes written, 0 if there was nothing to say. Never
 * writes a partial message: a reply that does not fit is dropped whole. */
/*   privileged  does the CALLER hold CAP_NET_ADMIN? Decided at the syscall
 *           boundary, where the caller's credentials exist -- rtnl_process is
 *           also reached from kernel context, which has none. Dumps ignore it;
 *           every command requires it. */
size_t rtnl_process(void *ns, uint32_t nl_pid, bool privileged,
                    const void *kbuf, size_t n, uint8_t *out, size_t cap);

/* The SIOC* half. netlink does NOT replace these: libc's if_nametoindex() --
 * which is how `ip addr add ... dev NAME` resolves an interface -- is an
 * ioctl, so a control plane without SIOCGIFINDEX cannot be driven by the
 * standard tools no matter how complete its netlink surface is. Answers from
 * the same state the dumps read, so the two cannot disagree.
 * Returns 0 or a negative ABI errno; -ABI_ENOTTY for a request it does not
 * implement, so callers that probe and fall back still work. */
long rtnl_sock_ioctl(void *ns, bool privileged, unsigned long rq,
                     unsigned long uarg);

#ifdef LXNETLINK_BOOT
/* The kernel-side half of the control-plane gate. It lives in rtnetlink.c
 * rather than kernel.c because it needs the message builders, and because the
 * two things it checks -- the initial namespace's exact reply bytes, and
 * whether a netlink-created pair really carries a frame -- are invisible from
 * userspace, which is where the rest of the gate (/bin/linux-netlink and a
 * busybox `ip` witness) lives. */
void rtnl_selftest(void);
#endif

#endif /* TOBYOS_RTNETLINK_H */
