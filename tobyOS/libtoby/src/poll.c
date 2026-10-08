/* libtoby/src/poll.c -- multiplexed I/O.
 *
 * ABI_SYS_POLL is the kernel's poll(2) (since batch 15). The fallback
 * below is only for a kernel without it: it sleeps the timeout and then
 * marks everything ready -- which is what every program got before, so
 * `read -t 5` in tsh always waited the five seconds. */

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
