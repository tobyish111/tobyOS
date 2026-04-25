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

enum file_kind {
    FILE_KIND_NULL    = 0,
    FILE_KIND_CONSOLE = 1,
    FILE_KIND_VFS     = 2,
    FILE_KIND_PIPE_R  = 3,
    FILE_KIND_PIPE_W  = 4,
    FILE_KIND_SOCKET  = 5,
    FILE_KIND_WINDOW  = 6,
    FILE_KIND_TERM    = 7,
};

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
};

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
