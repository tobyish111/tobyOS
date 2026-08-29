/* pty.h -- pseudoterminal (PTY) pairs (Track B / B23).
 *
 * B21 gave the kernel text console a real controlling-TTY line discipline;
 * B23 generalises that into pseudoterminals so userspace can create its own
 * terminals. A PTY is a (master, slave) pair of fds connected back-to-back:
 *
 *   - the MASTER end is what a terminal emulator / `script` / `expect` /
 *     sshd holds: bytes it writes become the slave's input ("keystrokes"),
 *     bytes the slave writes come back out of the master to be displayed;
 *   - the SLAVE end (/dev/pts/N) behaves exactly like a TTY: it carries a
 *     `struct ktermios`, runs the cooked line discipline (canonical line
 *     editing + ECHO, or raw/VMIN), and answers TCGETS/TCSETS/TIOCGWINSZ so
 *     a program with its stdio on the slave thinks it's at a real terminal.
 *
 * Linux glibc/musl `openpty()` is: open("/dev/ptmx") -> master fd;
 * ioctl(TIOCSPTLCK, 0) (unlock); ioctl(TIOCGPTN) -> N; open("/dev/pts/N") ->
 * slave fd. We support exactly that handshake.
 */

#ifndef TOBYOS_PTY_H
#define TOBYOS_PTY_H

#include <tobyos/types.h>
#include <tobyos/tty.h>

struct file;

/* ioctl request numbers specific to PTYs (Linux x86-64). The TTY_* ioctls in
 * tty.h (TCGETS/TCSETS/TIOCGWINSZ/...) also apply to a PTY slave/master. */
#define PTY_TIOCSCTTY   0x540Eu      /* set controlling terminal (accepted) */
#define PTY_TIOCGPTN    0x80045430u  /* get pty number (master)             */
#define PTY_TIOCSPTLCK  0x40045431u  /* lock/unlock slave (master; no-op)   */
#define PTY_TIOCGPTPEER 0x5441u      /* open peer (slave) of a master       */

/* Open /dev/ptmx: allocate a fresh PTY pair and return its MASTER struct file
 * (caller installs it in an fd). Returns NULL if no slot / OOM. */
struct file *pty_open_master(void);

/* Open /dev/pts/<index>: return a SLAVE struct file for an existing pair.
 * Returns NULL if the index is invalid / not allocated. */
struct file *pty_open_slave(int index);

/* file.c backend dispatch (read/write/close/clone reference-count). */
long pty_master_read (struct file *f, void *buf, size_t n);
long pty_master_write(struct file *f, const void *buf, size_t n);
long pty_slave_read  (struct file *f, void *buf, size_t n);
long pty_slave_write (struct file *f, const void *buf, size_t n);
long pty_ioctl       (struct file *f, unsigned long cmd, unsigned long arg);
void pty_close        (struct file *f);   /* drop one end's reference */
void pty_clone_ref     (struct file *f);   /* dup/fork: bump end refcount */

/* ---- kernel-side helpers (no userspace pointers involved) ------------
 *
 * The three calls above this point are the userspace path: pty_ioctl moves
 * termios/winsize through copy_to_user/copy_from_user, and pty_master_read
 * BLOCKS the calling process on a wait queue. Neither is usable from an
 * in-kernel service such as the SSH server, which pumps bytes from
 * net_service_tick() -- a context that runs on the syscall-return path and
 * must never block. These are the same operations without those two
 * assumptions. */

/* Which pair does this master belong to? Needed to open the matching slave,
 * since `struct pty` is private to pty.c. Returns -1 if `f` is not a master. */
int  pty_index_of(struct file *f);

/* Like pty_master_read, but never parks the caller. Returns:
 *     > 0  bytes read
 *       0  EOF -- the slave end is fully closed (the program on it exited)
 *      -2  nothing buffered right now; try again next tick
 * The 0-vs--2 split is the whole point: a poller must be able to tell "the
 * shell has nothing to say yet" from "the shell is gone", and the blocking
 * pty_master_read collapses both onto a wait. */
long pty_master_read_nb(struct file *f, void *buf, size_t n);

/* Set the slave's window size from kernel memory (SSH pty-req / window-change
 * carry it in the protocol, not in a user buffer). */
void pty_set_winsize(struct file *f, uint16_t cols, uint16_t rows);

/* poll/select readiness for a PTY fd (master or slave). */
bool pty_readable(struct file *f);
bool pty_hup(struct file *f);             /* other end fully closed */

#endif /* TOBYOS_PTY_H */
