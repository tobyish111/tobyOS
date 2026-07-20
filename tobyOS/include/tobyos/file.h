/* file.h -- per-process file-handle abstraction (milestone 7).
 *
 * A `struct file` is the kernel-side handle that a process file
 * descriptor points at. Each fd in struct proc.fds[] owns exactly one
 * file struct (no sharing -- inheritance always allocates a fresh
 * struct via file_clone, which bumps the appropriate backing-object
 * counter). This simplifies refcount bookkeeping significantly: a
 * file_close() always destroys the struct.
 *
 * Backing kinds:
 *   FILE_KIND_NULL     - the slot is closed (treated as ENOENT)
 *   FILE_KIND_CONSOLE  - reads from the keyboard buffer (returns 0 if
 *                        empty -- we don't truly block on the console
 *                        because the kernel shell still needs to drive
 *                        keyboard input itself); writes go to kputc.
 *   FILE_KIND_PIPE_R   - read end of a kernel pipe (see pipe.h)
 *   FILE_KIND_PIPE_W   - write end of a kernel pipe
 *   FILE_KIND_VFS      - wraps a struct vfs_file (reserved for future
 *                        sys_open; not exposed to userspace yet)
 *   FILE_KIND_SOCKET   - kernel socket (milestone 9). file_read/write
 *                        return -EINVAL; sockets are operated on via
 *                        SYS_SENDTO / SYS_RECVFROM only. close() routes
 *                        through sock_close() to wake any blocked
 *                        recvfrom and free the pool slot.
 *   FILE_KIND_WINDOW   - GUI window (milestone 10). file_read/write
 *                        return -EINVAL; windows are operated on via
 *                        SYS_GUI_FILL / SYS_GUI_TEXT / SYS_GUI_FLIP /
 *                        SYS_GUI_POLL_EVENT only. close() routes
 *                        through gui_window_close(), which frees the
 *                        per-window backing store and splices the
 *                        window out of the compositor's z-order list.
 *                        On proc_exit this is what auto-closes any
 *                        windows the dying process was holding open.
 *
 * file_read/write/close dispatch on `kind`. Any read on a write end (or
 * vice versa) returns -EBADF (-1).
 */

#ifndef TOBYOS_FILE_H
#define TOBYOS_FILE_H

#include <tobyos/types.h>
#include <tobyos/vfs.h>

struct pipe;
struct sock;
struct window;
struct term_session;
struct pty;

enum file_kind {
    FILE_KIND_NULL    = 0,
    FILE_KIND_CONSOLE = 1,
    FILE_KIND_VFS     = 2,
    FILE_KIND_PIPE_R  = 3,
    FILE_KIND_PIPE_W  = 4,
    FILE_KIND_SOCKET  = 5,
    FILE_KIND_WINDOW  = 6,
    FILE_KIND_TERM    = 7,
    /* Track B/B3: a directory opened by a Linux process for getdents64.
     * tobyOS has no native directory-fd, so this kind just remembers the
     * path; getdents64 re-opens it via vfs_opendir and resumes after
     * dir_off entries (see linux_syscall in syscall.c). */
    FILE_KIND_DIR     = 8,
    /* Track B/B13: a Linux epoll instance (epoll_create1). The fd's backing
     * is a kmalloc'd struct epoll_inst (an interest list of {fd,events,data}).
     * See linux_syscall epoll_ctl/epoll_wait in syscall.c. */
    FILE_KIND_EPOLL   = 9,
    /* Track B/B23: the two ends of a pseudoterminal pair. Both reference the
     * same kmalloc'd struct pty; the slave carries the termios + line
     * discipline. See pty.c. */
    FILE_KIND_PTY_MASTER = 10,
    FILE_KIND_PTY_SLAVE  = 11,
    /* Track C (networking): a Linux eventfd (eventfd2). A counting object with
     * an 8-byte read/write ABI; libcurl's multi handle creates one as its
     * wakeup descriptor (EFD_CLOEXEC|EFD_NONBLOCK). Backed by a kmalloc'd
     * struct eventfd shared across dup/fork via a refcount. See file.c. */
    FILE_KIND_EVENTFD    = 12,
    /* Track B (graphics): the Linux framebuffer device /dev/fb0. A Linux app
     * opens it, queries geometry via FBIOGET_VSCREENINFO/FSCREENINFO, mmaps a
     * shadow scanout buffer, draws XRGB8888 pixels, and presents it with
     * FBIOPAN_DISPLAY (the kernel blits the shadow into the gfx backbuffer and
     * flips). file_read/write hit the default error; close just frees. The
     * per-open geometry + mmap base live in a global fbdev context (syscall.c). */
    FILE_KIND_FB         = 13,
    /* Track B (input): the Linux evdev device /dev/input/event0. A Linux app
     * opens it, queries identity/capabilities via the EVIOCG* ioctls, and
     * read()s fixed-size struct input_event records (EV_KEY make/break +
     * EV_SYN frames) fed from the PS/2 keyboard. read on an empty queue blocks
     * (or returns EAGAIN when O_NONBLOCK). The event ring lives in a global
     * evdev context (syscall.c); close just frees the struct file. */
    FILE_KIND_EVDEV      = 14,
    /* Chromium bring-up (slice 20): a Linux memfd_create() object -- anonymous,
     * page-backed, mmap-COHERENT shared memory. Backed by a kmalloc'd struct
     * memfd (a physical-page list + logical size), shared across dup/clone via a
     * refcount. read/write/lseek/ftruncate/fstat/mmap all route to it; every
     * mmap maps the SAME physical pages (real sharing). See mmap.c + file.c. */
    FILE_KIND_MEMFD      = 15,
    /* The classic bit-bucket devices. /dev/null reads EOF and swallows writes;
     * /dev/zero reads endless zeroes and swallows writes. Both are always
     * poll-ready. Chromium's base::LaunchProcess opens /dev/null to remap a
     * child's stdio before execve -- without it every renderer/GPU/network
     * child died with _exit(127) -- and virtually every Unix program uses it. */
    FILE_KIND_DEVNULL    = 16,
    FILE_KIND_DEVZERO    = 17,
};

struct eventfd;
struct memfd;

struct file {
    enum file_kind  kind;
    /* For PIPE_R / PIPE_W. */
    struct pipe    *pipe;
    /* For VFS-backed handles. */
    struct vfs_file vfs;
    /* Milestone 25A: side-allocated open-file-description refcount so
     * dup()/dup2() can hand multiple struct files to the same underlying
     * vfs_file. NULL for non-VFS kinds. The pointed-at counter starts at
     * 1 in sys_open, +1 per clone, -1 per close; the underlying handle's
     * ops->close is invoked exactly once, when the count reaches zero. */
    int            *vfs_refs;
    /* For SOCKET. */
    struct sock    *sock;
    /* For WINDOW. */
    struct window  *win;
    /* For TERM (milestone 13). */
    struct term_session *term;
    /* For FILE_KIND_DIR (Track B/B3 getdents64): kmalloc'd absolute path
     * of the directory + how many entries have already been returned, so
     * each getdents64 re-opens and resumes. */
    char           *dirpath;
    uint32_t        dir_off;
    /* For FILE_KIND_EPOLL (Track B/B13): the epoll interest list. */
    struct epoll_inst *epoll;
    /* For FILE_KIND_PTY_MASTER / FILE_KIND_PTY_SLAVE (Track B/B23). */
    struct pty *pty;
    /* For FILE_KIND_EVENTFD (Track C): the shared counter object. */
    struct eventfd *efd;
    /* For FILE_KIND_MEMFD (slice 20): the shared page-backed memory object.
     * The per-fd read/write cursor lives in vfs.pos (unused for memfd otherwise). */
    struct memfd *memfd;
    /* Open access mode (O_RDONLY=0 / O_WRONLY=1 / O_RDWR=2), captured at
     * open time and returned by fcntl(F_GETFL). Chromium's shared-memory
     * PlatformSharedMemoryRegion CHECKs that a writable region's fd reports
     * O_RDWR via F_GETFL -- a blanket-0 fcntl made it look read-only and
     * ImmediateCrash'd. Copied across dup() in file_clone. */
    int o_accmode;
};

/* eventfd flags (Linux ABI) */
#define EFD_SEMAPHORE 00000001u
#define EFD_CLOEXEC   02000000u
#define EFD_NONBLOCK  00004000u

/* Allocate a fresh eventfd-backed struct file with the given initial counter
 * value and flags (EFD_SEMAPHORE / EFD_NONBLOCK honoured). Returns NULL on
 * OOM. */
struct file *eventfd_file_make(unsigned int initval, unsigned int flags);

/* Poll readiness helper: nonzero if the eventfd counter is > 0 (POLLIN). */
int eventfd_pollin(struct file *f);

/* Allocate + initialise the well-known console-backed file. Each call
 * returns a fresh kmalloc'd struct -- never share, so close() can
 * always free unconditionally. Returns NULL on OOM. */
struct file *console_file_make(void);

/* Allocate a fresh struct file pointing at the same backing object as
 * `src`. For pipes this also bumps the pipe's reader/writer count, so
 * inheritance Just Works. Returns NULL on OOM. */
struct file *file_clone(struct file *src);

/* Drop one fd-level reference. For pipes, this decrements the pipe's
 * reader/writer count (potentially waking the other end with EOF/EPIPE)
 * and may free the underlying pipe when both ends hit zero. The struct
 * file itself is always freed. Safe to call with NULL. */
void file_close(struct file *f);

/* Read up to `n` bytes from `f` into `buf`. Returns:
 *   > 0 : bytes read
 *     0 : EOF (pipe with no writers, or console with no input)
 *   < 0 : error (-1 = EBADF, -2 = EINVAL, -3 = EPIPE)
 * Pipe reads block while writers exist but no data is available. */
long file_read (struct file *f, void *buf, size_t n);

/* Write up to `n` bytes from `buf` to `f`. Same return convention as
 * file_read. Pipe writes block while the buffer is full and at least
 * one reader is alive; if the last reader has gone, returns -3 (EPIPE). */
long file_write(struct file *f, const void *buf, size_t n);

#endif /* TOBYOS_FILE_H */
