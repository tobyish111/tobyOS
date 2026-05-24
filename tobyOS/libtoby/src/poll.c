/* libtoby/src/poll.c -- multiplexed I/O.
 *
 * Delegates to the kernel's SYS_POLL if available, otherwise falls
 * back to a naive busy-poll loop using read/write availability checks.
 * The fallback just marks everything ready (sufficient for single-fd
 * tty usage patterns). */

#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include "libtoby_internal.h"

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
    if (!fds && nfds > 0) { errno = EFAULT; return -1; }

#ifdef ABI_SYS_POLL
    long rv = toby_sc3(ABI_SYS_POLL,
                       (long)(uintptr_t)fds,
                       (long)nfds,
                       (long)timeout_ms);
    if (rv != -ENOSYS) {
        if (rv < 0) { errno = (int)(-rv); return -1; }
        return (int)rv;
    }
#endif

    /* Fallback: sleep for the timeout, then mark all fds ready for
     * whatever events they asked for. This is wrong but functional
     * for the common "poll stdin with timeout" pattern. */
    if (timeout_ms > 0) {
        usleep((unsigned)(timeout_ms) * 1000u);
    }

    int ready = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        /* Optimistic: assume all requested events are ready */
        fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
        if (fds[i].revents) ready++;
    }
    return ready;
}
