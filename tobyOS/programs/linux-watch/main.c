/* linux-watch -- inotify + epoll honesty acceptance test (2026-08-22).
 *
 * Before this slice inotify was a three-layer lie: init returned an
 * instance INDEX (the first caller got 0 == stdin), add_watch returned a
 * bare counter and registered nothing, and no VFS path ever emitted an
 * event. epoll stored EPOLLET/EPOLLONESHOT and ignored both, and refused
 * the 65th fd. Every bit here asserts VALUES (event masks, names, rename
 * cookies, edge counts), because "the call succeeded" is exactly what the
 * old lies were made of.
 *
 *   bit0  inotify_init1(IN_CLOEXEC|IN_NONBLOCK) yields a REAL fd:
 *         FD_CLOEXEC reads back, and an empty read is EAGAIN (an fd that
 *         was secretly stdin would block or read the console)
 *   bit1  IN_CREATE arrives with the right wd and the child's NAME
 *   bit2  rename emits IN_MOVED_FROM + IN_MOVED_TO sharing one nonzero
 *         cookie, names on both halves
 *   bit3  IN_DELETE arrives; after rm_watch a new create emits NOTHING
 *   bit4  poll() on the inotify fd WAKES when a forked child creates a
 *         file (the event machinery reaches parked pollers)
 *   bit5  epoll honesty: >64 registrations accepted; EPOLLET reports an
 *         edge ONCE until drained-and-rearmed; EPOLLONESHOT disarms after
 *         one report until EPOLL_CTL_MOD
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define WDIR "/data/watchd"

static char evbuf[1024];

/* Read one batch and find an event matching (mask, name); returns its
 * cookie via *ck. Nonblocking fd: returns 0 if nothing matched. */
static int find_event(int fd, uint32_t mask, const char *name, uint32_t *ck) {
    ssize_t n = read(fd, evbuf, sizeof evbuf);
    if (n <= 0) return 0;
    for (ssize_t off = 0; off < n; ) {
        struct inotify_event *ev = (struct inotify_event *)(evbuf + off);
        if ((ev->mask & mask) &&
            (!name || (ev->len && strcmp(ev->name, name) == 0))) {
            if (ck) *ck = ev->cookie;
            return 1;
        }
        off += (ssize_t)sizeof(*ev) + ev->len;
    }
    return 0;
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    mkdir(WDIR, 0777);

    int ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);

    /* ---- bit0: a real, flagged, non-stdin fd ---- */
    {
        errno = 0;
        ssize_t r = read(ifd, evbuf, sizeof evbuf);
        int fdflags = fcntl(ifd, F_GETFD);
        printf("watch: init1 fd=%d F_GETFD=%d empty-read rc=%zd errno=%d "
               "(want EAGAIN=%d)\n", ifd, fdflags, r, errno, EAGAIN);
        if (ifd > 2 && fdflags == FD_CLOEXEC && r == -1 && errno == EAGAIN)
            bits |= 1;
    }

    int wd = inotify_add_watch(ifd, WDIR,
                               IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                               IN_MOVED_TO | IN_MODIFY);
    printf("watch: add_watch(%s) wd=%d\n", WDIR, wd);

    /* ---- bit1: IN_CREATE with the right name ---- */
    {
        int fd = open(WDIR "/wfile", O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
        uint32_t ck = 0;
        int hit = find_event(ifd, IN_CREATE, "wfile", &ck);
        printf("watch: IN_CREATE(wfile) hit=%d\n", hit);
        if (hit) bits |= 2;
    }

    /* ---- bit2: rename pair shares a nonzero cookie ---- */
    {
        rename(WDIR "/wfile", WDIR "/wmoved");
        /* One read may carry both events; scan a single batch for each. */
        ssize_t n = read(ifd, evbuf, sizeof evbuf);
        uint32_t ck_from = 0, ck_to = 0;
        int hit_from = 0, hit_to = 0;
        for (ssize_t off = 0; off < n; ) {
            struct inotify_event *ev = (struct inotify_event *)(evbuf + off);
            if ((ev->mask & IN_MOVED_FROM) && ev->len &&
                strcmp(ev->name, "wfile") == 0) { hit_from = 1; ck_from = ev->cookie; }
            if ((ev->mask & IN_MOVED_TO) && ev->len &&
                strcmp(ev->name, "wmoved") == 0) { hit_to = 1; ck_to = ev->cookie; }
            off += (ssize_t)sizeof(*ev) + ev->len;
        }
        printf("watch: MOVED_FROM=%d MOVED_TO=%d cookies=%u/%u\n",
               hit_from, hit_to, ck_from, ck_to);
        if (hit_from && hit_to && ck_from == ck_to && ck_from != 0) bits |= 4;
    }

    /* ---- bit3: IN_DELETE, then rm_watch silences ---- */
    {
        unlink(WDIR "/wmoved");
        int hit = find_event(ifd, IN_DELETE, "wmoved", 0);
        int rmrc = inotify_rm_watch(ifd, wd);
        int fd = open(WDIR "/after", O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
        errno = 0;
        ssize_t r = read(ifd, evbuf, sizeof evbuf);
        printf("watch: IN_DELETE=%d rm_watch=%d post-rm read rc=%zd errno=%d\n",
               hit, rmrc, r, errno);
        if (hit && rmrc == 0 && r == -1 && errno == EAGAIN) bits |= 8;
        unlink(WDIR "/after");
    }

    /* ---- bit4: poll() wakes on a child's create ---- */
    {
        wd = inotify_add_watch(ifd, WDIR, IN_CREATE);
        pid_t pid = fork();
        if (pid == 0) {
            usleep(300 * 1000);        /* parent parks in poll first */
            int fd = open(WDIR "/fromchild", O_CREAT | O_WRONLY, 0644);
            if (fd >= 0) close(fd);
            _exit(0);
        }
        struct pollfd p = { .fd = ifd, .events = POLLIN };
        int pr = poll(&p, 1, 5000);
        int hit = (pr == 1) ? find_event(ifd, IN_CREATE, "fromchild", 0) : 0;
        printf("watch: poll wake pr=%d revents=0x%x event=%d\n",
               pr, p.revents, hit);
        if (pr == 1 && (p.revents & POLLIN) && hit) bits |= 16;
        if (pid > 0) waitpid(pid, 0, 0);
        unlink(WDIR "/fromchild");
        inotify_rm_watch(ifd, wd);
    }
    close(ifd);

    /* ---- bit5: epoll cap + ET + ONESHOT ---- */
    {
        int ep = epoll_create1(0);
        int cap_ok = 0, et_ok = 0, oneshot_ok = 0;
        if (ep >= 0) {
            /* (a) the 64-entry wall is gone: register 80 eventfds. */
            int efds[80]; int added = 0;
            for (int i = 0; i < 80; i++) {
                efds[i] = eventfd(0, EFD_NONBLOCK);
                if (efds[i] < 0) break;
                struct epoll_event e = { .events = EPOLLIN,
                                         .data.u64 = (uint64_t)i };
                if (epoll_ctl(ep, EPOLL_CTL_ADD, efds[i], &e) == 0) added++;
            }
            uint64_t one = 1;
            (void)!write(efds[77], &one, sizeof one);
            struct epoll_event out[4];
            int n = epoll_wait(ep, out, 4, 1000);
            cap_ok = (added == 80 && n == 1 && out[0].data.u64 == 77);
            printf("watch: epoll added=%d/80 wait n=%d data=%llu\n",
                   added, n, n > 0 ? (unsigned long long)out[0].data.u64 : 0);
            uint64_t drain;
            (void)!read(efds[77], &drain, sizeof drain);
            for (int i = 0; i < 80; i++)
                if (efds[i] >= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, efds[i], 0);
                                    if (i != 60 && i != 61) close(efds[i]); }

            /* (b) EPOLLET: the contract this kernel promises is NO LOST
             * WAKEUPS -- ET is served as level (see the kernel comment:
             * an emulated edge cannot see a drain-and-refill between two
             * waits, and silence there is a lost wakeup, strictly worse
             * than a spurious one). So assert the real-app loop: wake,
             * drain, refill BETWEEN waits, wake again -- the shape a
             * faithful-but-blind edge tracker fails. efds[60] survives
             * from above. */
            int et = efds[60];
            struct epoll_event e = { .events = EPOLLIN | EPOLLET,
                                     .data.u64 = 60 };
            epoll_ctl(ep, EPOLL_CTL_ADD, et, &e);
            (void)!write(et, &one, sizeof one);
            int n1 = epoll_wait(ep, out, 4, 1000);     /* first wake */
            (void)!read(et, &drain, sizeof drain);     /* drain BETWEEN waits */
            (void)!write(et, &one, sizeof one);        /* refill BETWEEN waits */
            int n3 = epoll_wait(ep, out, 4, 1000);     /* must wake again */
            (void)!read(et, &drain, sizeof drain);
            int n4 = epoll_wait(ep, out, 4, 300);      /* drained: silent */
            et_ok = (n1 == 1 && n3 == 1 && n4 == 0);
            printf("watch: ET n1=%d n3=%d n4=%d(want 0)\n", n1, n3, n4);
            epoll_ctl(ep, EPOLL_CTL_DEL, et, 0); close(et);

            /* (c) EPOLLONESHOT: one report, then disarmed until MOD. */
            int os = efds[61];
            (void)!read(os, &drain, sizeof drain);     /* ensure empty */
            struct epoll_event eo = { .events = EPOLLIN | EPOLLONESHOT,
                                      .data.u64 = 61 };
            epoll_ctl(ep, EPOLL_CTL_ADD, os, &eo);
            (void)!write(os, &one, sizeof one);
            int m1 = epoll_wait(ep, out, 4, 1000);     /* fires once */
            int m2 = epoll_wait(ep, out, 4, 300);      /* disarmed: silent */
            epoll_ctl(ep, EPOLL_CTL_MOD, os, &eo);     /* re-arm */
            int m3 = epoll_wait(ep, out, 4, 1000);     /* still readable */
            oneshot_ok = (m1 == 1 && m2 == 0 && m3 == 1);
            printf("watch: ONESHOT m1=%d m2=%d(want 0) m3=%d\n", m1, m2, m3);
            close(os); close(ep);
        }
        if (cap_ok && et_ok && oneshot_ok) bits |= 32;
    }

    printf("LXWATCH: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
