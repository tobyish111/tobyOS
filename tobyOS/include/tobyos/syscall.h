/* syscall.h -- minimal SYSCALL/SYSRET-based syscall layer.
 *
 * ABI (Linux x86_64-compatible -- well-known and entirely register-based):
 *
 *   rax    syscall number  (in)    return value (out)
 *   rdi    arg1
 *   rsi    arg2
 *   rdx    arg3
 *   r10    arg4   (note: rcx is clobbered by SYSCALL, so r10 replaces it)
 *   r8     arg5
 *   r9     arg6
 *   rcx    user RIP after syscall  (clobbered)
 *   r11    user RFLAGS             (clobbered)
 *   all other GP regs preserved across the call.
 *
 * On entry the CPU has loaded CS/SS from STAR but has NOT switched RSP.
 * The asm trampoline (syscall_entry.S) does the swap by stashing user
 * RSP via g_user_rsp_scratch (a momentary scratch slot used only for
 * the entry stack-swap window), then loading kernel RSP from
 * g_kernel_syscall_rsp and immediately pushing the user RSP onto the
 * per-process kernel stack. It also clears IF (via FMASK) so we run
 * with IRQs off until we sysretq back. Per-process stashing matters:
 * a parent that blocks in waitpid must come back with ITS user RSP,
 * not whatever the child's last syscall happened to leave behind.
 *
 * Currently implemented numbers (milestone 10):
 *
 *   0  SYS_EXIT     (int code)                              never returns
 *   1  SYS_WRITE    (int fd, const void *buf, size_t len)   bytes or -err
 *   2  SYS_READ     (int fd,       void *buf, size_t len)   bytes/0(EOF)/-err
 *   3  SYS_PIPE     (int fds[2])                            0 or -err
 *   4  SYS_CLOSE    (int fd)                                0 or -err
 *   5  SYS_YIELD    ()                                      0 (always)
 *
 *   --- networking (milestone 9) -----------------------------------
 *   6  SYS_SOCKET   (int domain, int type)                  fd or -err
 *   7  SYS_BIND     (int fd, uint16_t port_be)              0 or -err
 *   8  SYS_SENDTO   (int fd, const void *buf, size_t len,
 *                    uint32_t dst_ip_be, uint16_t dst_port_be)
 *                                                           bytes or -err
 *   9  SYS_RECVFROM (int fd, void *buf, size_t len,
 *                    struct sockaddr_in_be *src_out)
 *                                                           bytes or -err
 *
 *   --- GUI (milestone 10) ------------------------------------------
 *  10  SYS_GUI_CREATE (uint32_t w, uint32_t h, const char *title)
 *                                                           fd or -err
 *  11  SYS_GUI_FILL   (int fd, int x, int y, uint32_t whlen,
 *                       uint32_t color)                     0 or -err
 *                       (whlen packs w in low 16 bits, h in high 16)
 *  12  SYS_GUI_TEXT   (int fd, int xy, const char *s, uint32_t fg,
 *                       uint32_t bg)                        0 or -err
 *                       (xy packs x in low 16 bits, y in high 16)
 *  13  SYS_GUI_FLIP  (int fd)                               0 or -err
 *  14  SYS_GUI_POLL_EVENT (int fd, struct gui_event *out)
 *                                                           1 (got event),
 *                                                           0 (none),
 *                                                           -err
 *
 * The GUI calls pack two 16-bit ints into a single 32-bit register so
 * we don't blow past the 6-arg syscall ABI for fill/text. The cap of
 * 16 bits per coordinate is fine -- our framebuffer never exceeds
 * 65535 pixels in either dimension.
 *
 * Anything else returns -1 ("ENOSYS-like").
 *
 * Every syscall return runs through signal_deliver_if_pending() before
 * sysretq -- so a process targeted by SIGINT/SIGTERM dies on its very
 * next system call. CPU-bound loops are also killable thanks to the
 * PIT IRQ checking signals on the way back to ring 3 (see pit.c).
 */

#ifndef TOBYOS_SYSCALL_H
#define TOBYOS_SYSCALL_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Milestone 25A: the canonical syscall numbers + structs live in
 * <tobyos/abi/abi.h>. The SYS_* aliases below preserve the
 * pre-25A spelling so the kernel and any in-tree user program can
 * keep using them unchanged. New code should reach for the ABI_*
 * names directly. */

#define SYS_EXIT      ABI_SYS_EXIT
#define SYS_WRITE     ABI_SYS_WRITE
#define SYS_READ      ABI_SYS_READ
#define SYS_PIPE      ABI_SYS_PIPE
#define SYS_CLOSE     ABI_SYS_CLOSE
#define SYS_YIELD     ABI_SYS_YIELD
#define SYS_SOCKET    ABI_SYS_SOCKET
#define SYS_BIND      ABI_SYS_BIND
#define SYS_SENDTO    ABI_SYS_SENDTO
#define SYS_RECVFROM  ABI_SYS_RECVFROM
#define SYS_GUI_CREATE      ABI_SYS_GUI_CREATE
#define SYS_GUI_FILL        ABI_SYS_GUI_FILL
#define SYS_GUI_TEXT        ABI_SYS_GUI_TEXT
#define SYS_GUI_FLIP        ABI_SYS_GUI_FLIP
#define SYS_GUI_POLL_EVENT  ABI_SYS_GUI_POLL_EVENT

/* --- terminal + filesystem + exec (milestone 13) ---------------- *
 *
 *  15  SYS_TERM_OPEN   ()
 *                      -> fd of a FILE_KIND_TERM handle, or -1.
 *                      The session starts with a welcome banner and
 *                      a first prompt already queued in its output
 *                      ring, so the first SYS_TERM_READ returns
 *                      something drawable.
 *
 *  16  SYS_TERM_WRITE  (int fd, const void *buf, size_t len)
 *                      -> bytes consumed (== len for valid input).
 *                      Semantics are "the user typed these bytes":
 *                      printable bytes echo + extend the line
 *                      editor, 0x08/0x7F backspace, '\r'/'\n'
 *                      executes the line.
 *
 *  17  SYS_TERM_READ   (int fd, void *buf, size_t cap)
 *                      -> bytes drained (0 if the ring is empty).
 *                      Non-blocking -- callers should yield on 0.
 *
 *  18  SYS_FS_READDIR  (const char *path, struct vfs_dirent_user *out,
 *                        int cap, int offset)
 *                      -> number of entries written (0..cap) or -err.
 *                      `offset` lets the caller page through large
 *                      directories in chunks. Kernel allocates no
 *                      long-lived state -- each call reopens the dir.
 *
 *  19  SYS_FS_READFILE (const char *path, void *out, size_t cap)
 *                      -> bytes read (may be < file size if cap is
 *                      smaller, which we report via the size field of
 *                      SYS_FS_READDIR for the directory listing). -1
 *                      on any error.
 *
 *  20  SYS_EXEC        (const char *path, const char *arg)
 *                      -> 0 on success (queued), -1 on failure.
 *                      Spawn actually happens asynchronously on pid 0
 *                      via gui_launch_enqueue_arg(). `arg` may be
 *                      NULL; when non-NULL it becomes argv[1].
 */
#define SYS_TERM_OPEN       ABI_SYS_TERM_OPEN
#define SYS_TERM_WRITE      ABI_SYS_TERM_WRITE
#define SYS_TERM_READ       ABI_SYS_TERM_READ
#define SYS_FS_READDIR      ABI_SYS_FS_READDIR
#define SYS_FS_READFILE     ABI_SYS_FS_READFILE
#define SYS_EXEC            ABI_SYS_EXEC

/* --- settings + session (milestone 14) ---------------------------- *
 *
 *  21  SYS_SETTING_GET (const char *key, char *out, size_t cap)
 *                      -> bytes written to *out (excluding NUL), or
 *                      -1 on bad args. Returns 0 (and writes "") if
 *                      the key is unknown -- callers can supply
 *                      defaults themselves.
 *
 *  22  SYS_SETTING_SET (const char *key, const char *val)
 *                      -> 0 on success, -1 on bad args / cache full.
 *                      Persists immediately via settings_save() so the
 *                      change survives a reboot. Anyone may call this
 *                      (no permission model in milestone 14).
 *
 *  23  SYS_LOGIN       (const char *username)
 *                      -> 0 on success, -1 on bad args. Bumps the
 *                      session id, marks the session active, persists
 *                      "user.last". Intended to be called only by
 *                      /bin/login but not enforced.
 *
 *  24  SYS_LOGOUT      ()
 *                      -> 0 on success, -1 if no session is active.
 *                      SIGTERMs every PCB whose session_id matches the
 *                      current session and clears session state. The
 *                      caller itself is one of those PCBs (if it was
 *                      session-tagged), so it will be torn down on its
 *                      next syscall return.
 *
 *  25  SYS_SESSION_INFO(char *out, size_t cap)
 *                      -> bytes written to *out (excluding NUL), or
 *                      -1 on bad args. Format: "id=<n> active=<0|1>
 *                      user=<name>\n".
 */
#define SYS_SETTING_GET   ABI_SYS_SETTING_GET
#define SYS_SETTING_SET   ABI_SYS_SETTING_SET
#define SYS_LOGIN         ABI_SYS_LOGIN
#define SYS_LOGOUT        ABI_SYS_LOGOUT
#define SYS_SESSION_INFO  ABI_SYS_SESSION_INFO

/* --- user identity (milestone 15) -------------------------------- *
 *
 *  26  SYS_GETUID      ()                       -> caller's uid
 *  27  SYS_GETGID      ()                       -> caller's gid
 *  28  SYS_USERNAME    (int uid, char *out, size_t cap)
 *                      -> bytes written (excluding NUL), 0 if uid is
 *                      not registered, -1 on bad args. Pass uid=-1 to
 *                      look up the caller's own name.
 *  29  SYS_CHMOD       (const char *path, uint32_t mode)
 *                      -> 0 on success, -err on failure.
 *  30  SYS_CHOWN       (const char *path, uint32_t uid, uint32_t gid)
 *                      -> 0 on success, -err on failure (root only).
 */
#define SYS_GETUID        ABI_SYS_GETUID
#define SYS_GETGID        ABI_SYS_GETGID
#define SYS_USERNAME      ABI_SYS_USERNAME
#define SYS_CHMOD         ABI_SYS_CHMOD
#define SYS_CHOWN         ABI_SYS_CHOWN

/* --- Milestone 25A new syscalls (libc-shape) ----------------------- *
 *
 *  31  SYS_GETPID    ()                       -> int pid
 *  32  SYS_GETPPID   ()                       -> int ppid
 *  33  SYS_SPAWN     (struct abi_spawn_req *) -> pid or -err
 *  34  SYS_WAITPID   (int pid, int *status, int flags)
 *                                             -> reaped pid / 0 / -err
 *  35  SYS_OPEN      (const char *path, int flags, int mode) -> fd
 *  36  SYS_LSEEK     (int fd, int64_t off, int whence) -> new pos
 *  37  SYS_STAT      (const char *path, struct abi_stat *out) -> 0
 *  38  SYS_FSTAT     (int fd,           struct abi_stat *out) -> 0
 *  39  SYS_DUP       (int oldfd)              -> newfd
 *  40  SYS_DUP2      (int oldfd, int newfd)   -> newfd
 *  41  SYS_UNLINK    (const char *path)       -> 0
 *  42  SYS_MKDIR     (const char *path, int mode) -> 0
 *  43  SYS_BRK       (uintptr_t new_brk)      -> current brk
 *  44  SYS_GETCWD    (char *out, size_t cap)  -> bytes written
 *  45  SYS_CHDIR     (const char *path)       -> 0
 *  46  SYS_GETENV    (const char *name, char *out, size_t cap)
 *                                             -> bytes written or 0
 *  47  SYS_NANOSLEEP (uint64_t ns)            -> 0
 *  48  SYS_CLOCK_MS  ()                       -> uptime ms
 *  49  SYS_ABI_VERSION ()                     -> TOBY_ABI_VERSION
 */
#define SYS_GETPID         ABI_SYS_GETPID
#define SYS_GETPPID        ABI_SYS_GETPPID
#define SYS_SPAWN          ABI_SYS_SPAWN
#define SYS_WAITPID        ABI_SYS_WAITPID
#define SYS_OPEN           ABI_SYS_OPEN
#define SYS_LSEEK          ABI_SYS_LSEEK
#define SYS_STAT           ABI_SYS_STAT
#define SYS_FSTAT          ABI_SYS_FSTAT
#define SYS_DUP            ABI_SYS_DUP
#define SYS_DUP2           ABI_SYS_DUP2
#define SYS_UNLINK         ABI_SYS_UNLINK
#define SYS_MKDIR          ABI_SYS_MKDIR
#define SYS_BRK            ABI_SYS_BRK
#define SYS_GETCWD         ABI_SYS_GETCWD
#define SYS_CHDIR          ABI_SYS_CHDIR
#define SYS_GETENV         ABI_SYS_GETENV
#define SYS_NANOSLEEP      ABI_SYS_NANOSLEEP
#define SYS_CLOCK_MS       ABI_SYS_CLOCK_MS
#define SYS_ABI_VERSION    ABI_SYS_ABI_VERSION

/* --- Milestone 31: desktop notifications -------------------------- *
 *
 *  70  SYS_NOTIFY_POST    (const struct abi_notification *)
 *                                              -> id (>=1) or -err
 *  71  SYS_NOTIFY_LIST    (struct abi_notification *out, uint32_t cap)
 *                                              -> count or -err
 *  72  SYS_NOTIFY_DISMISS (uint32_t id)        -> 0 or -err
 *                                              (id == 0 dismisses all)
 *
 * The kernel owns the notification ring; userspace posts/list/dismiss
 * on top of it. The compositor pulls toasts and notification-center
 * contents out of the same ring with the in-kernel notify_* APIs. */
#define SYS_NOTIFY_POST     ABI_SYS_NOTIFY_POST
#define SYS_NOTIFY_LIST     ABI_SYS_NOTIFY_LIST
#define SYS_NOTIFY_DISMISS  ABI_SYS_NOTIFY_DISMISS

/* Used by SYS_RECVFROM to return the source address. Network byte
 * order, mirrored verbatim into userspace. */
struct sockaddr_in_be {
    uint32_t ip;            /* network byte order */
    uint16_t port;          /* network byte order */
    uint16_t _pad;
};

/* Used by SYS_FS_READDIR to return one directory entry to user space.
 * `type` is 1 (file) or 2 (dir) -- matches VFS_TYPE_*. Layout stays
 * byte-stable. */
#define SYS_FS_NAME_MAX  64
#define SYS_FS_TYPE_FILE 1
#define SYS_FS_TYPE_DIR  2
struct vfs_dirent_user {
    char     name[SYS_FS_NAME_MAX];
    uint32_t type;
    uint32_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
};

void syscall_init(void);
long syscall_dispatch(long num, long a1, long a2, long a3, long a4, long a5);

#endif /* TOBYOS_SYSCALL_H */
