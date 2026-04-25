/* syscall.c -- SYSCALL/SYSRET MSR plumbing + the C dispatcher.
 *
 * Setup steps in syscall_init():
 *   1. EFER.SCE = 1                  enables the SYSCALL/SYSRET pair.
 *   2. IA32_STAR  = (KCS<<32) | (UDS<<48)
 *      The CPU forms:
 *        on syscall : CS = STAR[47:32]            = KERNEL_CS
 *                     SS = STAR[47:32] + 8        = KERNEL_DS
 *        on sysretq : CS = STAR[63:48] + 16 | 3   = USER_CS
 *                     SS = STAR[63:48] + 8  | 3   = USER_DS
 *      The +8/+16 offsets are why GDT slot order is fixed (see gdt.h).
 *   3. IA32_LSTAR = &syscall_entry   the actual jump target in 64-bit.
 *   4. IA32_FMASK = IF|DF|TF         RFLAGS bits to CLEAR on entry.
 *   5. g_kernel_syscall_rsp = top of TSS RSP0 stack.
 *
 * Milestone 7: syscalls now go through the per-process fd table for
 * read/write/close. SYS_WRITE took (buf, len) before; it now takes
 * (fd, buf, len) -- updated user programs (user_hello, user_bad,
 * /bin/echo, /bin/cat) all use the new signature.
 */

#include <tobyos/syscall.h>
#include <tobyos/proc.h>
#include <tobyos/file.h>
#include <tobyos/pipe.h>
#include <tobyos/sched.h>
#include <tobyos/signal.h>
#include <tobyos/socket.h>
#include <tobyos/gui.h>
#include <tobyos/gfx.h>
#include <tobyos/term.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/gdt.h>
#include <tobyos/tss.h>
#include <tobyos/printk.h>
#include <tobyos/cpu.h>
#include <tobyos/settings.h>
#include <tobyos/session.h>
#include <tobyos/users.h>
#include <tobyos/cap.h>
#include <tobyos/perf.h>
#include <tobyos/elf.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/devtest.h>
#include <tobyos/display.h>
#include <tobyos/hotplug.h>
#include <tobyos/slog.h>
#include <tobyos/watchdog.h>
#include <tobyos/safemode.h>
#include <tobyos/tobyfs.h>
#include <tobyos/service.h>
#include <tobyos/net.h>
#include <tobyos/hwinfo.h>
#include <tobyos/drvmatch.h>
#include <tobyos/hwdb.h>
#include <tobyos/notify.h>

extern uint64_t g_kernel_syscall_rsp;
extern void syscall_entry(void);

/* MSR numbers. */
#define IA32_EFER       0xC0000080u
#define IA32_STAR       0xC0000081u
#define IA32_LSTAR      0xC0000082u
#define IA32_FMASK      0xC0000084u

#define EFER_SCE        (1ULL << 0)

/* RFLAGS bits we want masked off on syscall entry. */
#define RFLAGS_TF       (1ULL << 8)
#define RFLAGS_IF       (1ULL << 9)
#define RFLAGS_DF       (1ULL << 10)

/* Tiny helper -- the user buffer is in user virtual memory, but our
 * page tables are shared so the kernel can read it directly. We DO
 * sanity-check the pointer is in the user half. */
#define USER_HALF_MAX 0x0000800000000000ULL

#define SYS_MAX_RW    65536u   /* per-call cap */

static bool user_buf_ok(uint64_t addr, size_t len) {
    if (len == 0) return true;
    if (addr >= USER_HALF_MAX)            return false;
    if (addr + len > USER_HALF_MAX)       return false;
    if (addr + len < addr)                return false;   /* wrap */
    return true;
}

static struct file *fd_lookup(int fd) {
    if (fd < 0 || fd >= PROC_NFDS) return 0;
    return current_proc()->fds[fd];
}

static int fd_alloc_into(struct proc *p, struct file *f) {
    for (int i = 0; i < PROC_NFDS; i++) {
        if (!p->fds[i]) {
            p->fds[i] = f;
            return i;
        }
    }
    return -1;
}

/* ---- syscall implementations ----------------------------------- */

static long sys_write(int fd, const void *buf, size_t len) {
    if (len == 0) return 0;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) {
        kprintf("[sys_write] reject: buf=%p len=%lu not in user half\n",
                buf, (unsigned long)len);
        return -1;
    }
    struct file *f = fd_lookup(fd);
    if (!f) return -1;
    return file_write(f, buf, len);
}

static long sys_read(int fd, void *buf, size_t len) {
    if (len == 0) return 0;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) {
        kprintf("[sys_read] reject: buf=%p len=%lu not in user half\n",
                buf, (unsigned long)len);
        return -1;
    }
    struct file *f = fd_lookup(fd);
    if (!f) return -1;
    return file_read(f, buf, len);
}

/* SYS_PIPE: returns two fds. user_fds_out points at int[2] in userspace. */
static long sys_pipe(int *user_fds_out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)user_fds_out, sizeof(int) * 2)) {
        return -1;
    }
    struct file *r = 0, *w = 0;
    if (pipe_create(&r, &w) != 0) return -1;

    struct proc *p = current_proc();
    int fd_r = fd_alloc_into(p, r);
    if (fd_r < 0) { file_close(r); file_close(w); return -1; }
    int fd_w = fd_alloc_into(p, w);
    if (fd_w < 0) { p->fds[fd_r] = 0; file_close(r); file_close(w); return -1; }

    user_fds_out[0] = fd_r;
    user_fds_out[1] = fd_w;
    return 0;
}

static long sys_close(int fd) {
    if (fd < 0 || fd >= PROC_NFDS) return -1;
    struct proc *p = current_proc();
    if (!p->fds[fd]) return -1;
    file_close(p->fds[fd]);
    p->fds[fd] = 0;
    return 0;
}

static __attribute__((noreturn)) void sys_exit(int code) {
    kprintf("[sys_exit] user requested exit, code=%d (0x%x)\n",
            code, (unsigned)code);
    proc_exit(code);
}

/* SYS_YIELD: voluntarily give up the CPU. Useful for CPU-bound demo
 * programs that want to remain killable by Ctrl+C even on hardware
 * without a working timer preempt path. Always returns 0. */
static long sys_yield(void) {
    sched_yield();
    return 0;
}

/* ---- networking syscalls (milestone 9) ------------------------- */

static long sys_socket(int domain, int type) {
    if (!cap_check(current_proc(), CAP_NET, "sys_socket")) return -1;
    if (domain != AF_INET || type != SOCK_DGRAM) return -1;
    struct sock *s = sock_alloc(SOCK_KIND_UDP);
    if (!s) return -1;

    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) { sock_close(s); return -1; }
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_SOCKET;
    f->sock = s;

    struct proc *p = current_proc();
    int fd = fd_alloc_into(p, f);
    if (fd < 0) { kfree(f); sock_close(s); return -1; }
    return fd;
}

static long sys_bind(int fd, uint16_t port_be) {
    if (!cap_check(current_proc(), CAP_NET, "sys_bind")) return -1;
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_SOCKET || !f->sock) return -1;
    return sock_bind(f->sock, port_be);
}

static long sys_sendto(int fd, const void *buf, size_t len,
                       uint32_t dst_ip_be, uint16_t dst_port_be) {
    if (!cap_check(current_proc(), CAP_NET, "sys_sendto")) return -1;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) return -1;
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_SOCKET || !f->sock) return -1;
    return sock_sendto(f->sock, buf, len, dst_ip_be, dst_port_be);
}

/* Validate a NUL-terminated user string lives entirely in the user
 * half. Cap is per-call so a malicious user can't make us scan
 * forever. Returns the (capped) string length on success, -1 on bad. */
static long user_str_ok(const char *s, size_t cap) {
    uint64_t addr = (uint64_t)(uintptr_t)s;
    if (addr == 0 || addr >= USER_HALF_MAX) return -1;
    for (size_t i = 0; i < cap; i++) {
        if (addr + i >= USER_HALF_MAX) return -1;
        if (s[i] == '\0') return (long)i;
    }
    /* Not NUL-terminated within cap -- treat as bad input. */
    return -1;
}

static long sys_recvfrom(int fd, void *buf, size_t len,
                         struct sockaddr_in_be *src_out) {
    if (!cap_check(current_proc(), CAP_NET, "sys_recvfrom")) return -1;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) return -1;
    if (src_out && !user_buf_ok((uint64_t)(uintptr_t)src_out,
                                sizeof(*src_out))) return -1;
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_SOCKET || !f->sock) return -1;
    uint32_t src_ip = 0; uint16_t src_port = 0;
    long rv = sock_recvfrom(f->sock, buf, len, &src_ip, &src_port);
    if (rv >= 0 && src_out) {
        src_out->ip   = src_ip;
        src_out->port = src_port;
        src_out->_pad = 0;
    }
    return rv;
}

/* ---- GUI syscalls (milestone 10) ------------------------------- */

static long sys_gui_create(uint32_t w, uint32_t h, const char *title) {
    if (!cap_check(current_proc(), CAP_GUI, "sys_gui_create")) return -1;
    char tbuf[32];
    tbuf[0] = '\0';
    if (title) {
        long n = user_str_ok(title, sizeof(tbuf));
        if (n < 0) return -1;
        memcpy(tbuf, title, (size_t)n);
        tbuf[n] = '\0';
    }
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return -1;

    if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("syscall: gui_create(%ux%u, '%s')", w, h, tbuf);
    }

    struct window *win = gui_window_create((int)w, (int)h, tbuf);
    if (!win) return -1;

    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) { gui_window_close(win); return -1; }
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_WINDOW;
    f->win  = win;

    struct proc *p = current_proc();
    int fd = fd_alloc_into(p, f);
    if (fd < 0) { kfree(f); gui_window_close(win); return -1; }
    return fd;
}

static long sys_gui_fill(int fd, int x, int y, uint32_t whlen, uint32_t color) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    int w = (int)(int16_t)(whlen & 0xFFFFu);
    int h = (int)(int16_t)((whlen >> 16) & 0xFFFFu);
    if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("syscall: gui_fill fd=%d xy=(%d,%d) wh=%dx%d color=0x%08x",
                       fd, x, y, w, h, (unsigned)color);
    }
    return gui_window_fill(f->win, x, y, w, h, color);
}

/* M27C: blend an ARGB colour over the window's existing pixels. The
 * window backbuf stays XRGB; alpha is consumed by the blend. Same
 * (w,h) packing as sys_gui_fill so userland can swap them freely. */
static long sys_gui_fill_argb(int fd, int x, int y,
                              uint32_t whlen, uint32_t argb) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    int w = (int)(int16_t)(whlen & 0xFFFFu);
    int h = (int)(int16_t)((whlen >> 16) & 0xFFFFu);
    if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("syscall: gui_fill_argb fd=%d xy=(%d,%d) wh=%dx%d argb=0x%08x",
                       fd, x, y, w, h, (unsigned)argb);
    }
    return gui_window_fill_argb(f->win, x, y, w, h, argb);
}

/* M27D: scaled/smoothed text. a4=fg, a5 packs (bg | scale<<24 |
 * smooth<<31). scale=0 is treated as 1 for backwards-compatibility
 * (legacy callers that always pass 0 in the upper bits get the
 * 8x8 path); scale is clamped to [1, 32]. */
static long sys_gui_text_scaled(int fd, uint32_t xy, const char *s,
                                uint32_t fg, uint32_t bg_scale_smooth) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    long n = user_str_ok(s, 256);
    if (n < 0) return -1;
    int x = (int)(int16_t)(xy & 0xFFFFu);
    int y = (int)(int16_t)((xy >> 16) & 0xFFFFu);
    uint32_t bg     = bg_scale_smooth & 0x00FFFFFFu;
    int      scale  = (int)((bg_scale_smooth >> 24) & 0x7Fu);
    int      smooth = (int)((bg_scale_smooth >> 31) & 0x1u);
    if (scale <= 0) scale = 1;
    if (scale > 32) scale = 32;
    /* Special-case the GFX_TRANSPARENT sentinel: the upper byte of
     * the user's bg field collides with our packed scale/smooth
     * bits, so an explicit "no background" value would never make
     * it through. Any caller that wants transparency must encode
     * 0x00FFFFFE in `bg` (a colour that nobody draws) -- the kernel
     * promotes that to the canonical sentinel. */
    if (bg == 0x00FFFFFEu) bg = GFX_TRANSPARENT;
    char buf[256];
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("syscall: gui_text_scaled fd=%d xy=(%d,%d) "
                       "len=%ld scale=%d smooth=%d",
                       fd, x, y, n, scale, smooth);
    }
    return gui_window_text_scaled(f->win, x, y, buf, fg, bg, scale, smooth);
}

static long sys_gui_text(int fd, uint32_t xy, const char *s,
                         uint32_t fg, uint32_t bg) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    long n = user_str_ok(s, 256);
    if (n < 0) return -1;
    int x = (int)(int16_t)(xy & 0xFFFFu);
    int y = (int)(int16_t)((xy >> 16) & 0xFFFFu);
    /* Copy into a small kernel buffer so the user can't change the
     * string out from under us mid-draw. */
    char buf[256];
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("syscall: gui_text fd=%d xy=(%d,%d) len=%ld",
                       fd, x, y, n);
    }
    return gui_window_text(f->win, x, y, buf, fg, bg);
}

static long sys_gui_flip(int fd) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("syscall: gui_flip fd=%d", fd);
    }
    return gui_window_flip(f->win);
}

static long sys_gui_poll_event(int fd, struct gui_event *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out))) return -1;
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    /* Poll into a kernel-side struct first so a partial write to user
     * memory can't leave the queue in a half-consumed state. */
    struct gui_event ev;
    int got = gui_window_poll_event(f->win, &ev);
    if (got > 0) memcpy(out, &ev, sizeof(*out));
    if (got > 0 && gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("syscall: gui_poll_event fd=%d -> type=%d "
                       "xy=(%d,%d) btn=0x%02x key=0x%02x",
                       fd, ev.type, ev.x, ev.y,
                       (unsigned)ev.button, (unsigned)ev.key);
    }
    return got;
}

/* ---- terminal session syscalls (milestone 13) ------------------ */

static long sys_term_open(void) {
    if (!cap_check(current_proc(), CAP_TERM, "sys_term_open")) return -1;
    struct term_session *s = term_session_create();
    if (!s) return -1;
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) { term_session_close(s); return -1; }
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_TERM;
    f->term = s;

    struct proc *p = current_proc();
    int fd = fd_alloc_into(p, f);
    if (fd < 0) { kfree(f); term_session_close(s); return -1; }
    return fd;
}

static long sys_term_write(int fd, const void *buf, size_t len) {
    if (len == 0) return 0;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) return -1;
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_TERM || !f->term) return -1;
    /* Bounce the input through a small kernel buffer so the user can't
     * mutate it while term_session_write_input reads byte-by-byte. */
    char tmp[256];
    size_t written = 0;
    while (written < len) {
        size_t chunk = len - written;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        memcpy(tmp, (const char *)buf + written, chunk);
        long rv = term_session_write_input(f->term, tmp, chunk);
        if (rv < 0) return rv;
        written += (size_t)rv;
        if ((size_t)rv < chunk) break;
    }
    return (long)written;
}

static long sys_term_read(int fd, void *buf, size_t cap) {
    if (cap == 0) return 0;
    if (cap > SYS_MAX_RW) cap = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, cap)) return -1;
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_TERM || !f->term) return -1;
    /* Drain to a kernel buffer first, then copy out -- keeps the ring
     * consumer logic simple and matches the pattern used by the other
     * pollable syscalls (gui_poll_event). */
    char tmp[256];
    size_t total = 0;
    while (total < cap) {
        size_t chunk = cap - total;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        long n = term_session_read_output(f->term, tmp, chunk);
        if (n <= 0) break;
        memcpy((char *)buf + total, tmp, (size_t)n);
        total += (size_t)n;
        if ((size_t)n < chunk) break;
    }
    return (long)total;
}

/* ---- VFS syscalls (milestone 13) --------------------------------- */

static long sys_fs_readdir(const char *path, struct vfs_dirent_user *out,
                           int cap, int offset) {
    if (cap <= 0) return 0;
    if (offset < 0) offset = 0;
    long plen = user_str_ok(path, VFS_PATH_MAX);
    if (plen < 0) return -1;
    if (!user_buf_ok((uint64_t)(uintptr_t)out,
                     (size_t)cap * sizeof(*out))) return -1;
    char kpath[VFS_PATH_MAX];
    memcpy(kpath, path, (size_t)plen);
    kpath[plen] = '\0';

    struct vfs_dir d;
    int rc = vfs_opendir(kpath, &d);
    if (rc != VFS_OK) return -1;

    struct vfs_dirent ent;
    int written = 0;
    int index   = 0;
    while (written < cap && vfs_readdir(&d, &ent) == VFS_OK) {
        if (index++ < offset) continue;
        /* Copy via a kernel staging buffer so a partial iteration
         * can't leave bad data in user memory. */
        struct vfs_dirent_user u;
        memset(&u, 0, sizeof(u));
        size_t i = 0;
        for (; i + 1 < SYS_FS_NAME_MAX && ent.name[i]; i++) {
            u.name[i] = ent.name[i];
        }
        u.name[i] = '\0';
        u.type = (ent.type == VFS_TYPE_DIR) ? SYS_FS_TYPE_DIR
                                            : SYS_FS_TYPE_FILE;
        u.size = (uint32_t)ent.size;
        u.uid  = ent.uid;
        u.gid  = ent.gid;
        u.mode = ent.mode;
        memcpy(&out[written], &u, sizeof(u));
        written++;
    }
    vfs_closedir(&d);
    return written;
}

static long sys_fs_readfile(const char *path, void *out, size_t cap) {
    if (cap == 0) return 0;
    if (cap > SYS_MAX_RW) cap = SYS_MAX_RW;
    long plen = user_str_ok(path, VFS_PATH_MAX);
    if (plen < 0) return -1;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, cap)) return -1;
    char kpath[VFS_PATH_MAX];
    memcpy(kpath, path, (size_t)plen);
    kpath[plen] = '\0';

    struct vfs_file f;
    int rc = vfs_open(kpath, &f);
    if (rc != VFS_OK) return -1;

    /* Stream in small chunks so we don't need a giant stack buffer for
     * large files. We copy into a kernel staging buffer and then into
     * user memory; user can't mutate the page under us mid-read. */
    char tmp[256];
    size_t total = 0;
    while (total < cap) {
        size_t want = cap - total;
        if (want > sizeof(tmp)) want = sizeof(tmp);
        long n = vfs_read(&f, tmp, want);
        if (n <= 0) break;
        memcpy((char *)out + total, tmp, (size_t)n);
        total += (size_t)n;
    }
    vfs_close(&f);
    return (long)total;
}

/* ---- exec (milestone 13) ---------------------------------------- *
 *
 * Thin wrapper around the desktop launch queue -- the actual
 * proc_spawn() runs on pid 0, so a buggy userspace caller can't wedge
 * the kernel's proc table from an arbitrary syscall context. */
static long sys_exec(const char *path, const char *arg) {
    if (!cap_check(current_proc(), CAP_EXEC, "sys_exec")) return -1;
    long plen = user_str_ok(path, VFS_PATH_MAX);
    if (plen < 0) return -1;
    char kpath[VFS_PATH_MAX];
    memcpy(kpath, path, (size_t)plen);
    kpath[plen] = '\0';

    char karg[128];
    const char *karg_ptr = 0;
    if (arg) {
        long alen = user_str_ok(arg, sizeof(karg));
        if (alen < 0) return -1;
        memcpy(karg, arg, (size_t)alen);
        karg[alen] = '\0';
        karg_ptr = karg;
    }
    return gui_launch_enqueue_arg(kpath, karg_ptr);
}

/* ---- settings + session syscalls (milestone 14) -------------------- */

static long sys_setting_get(const char *key, char *out, size_t cap) {
    if (cap == 0) return 0;
    if (cap > 1024) cap = 1024;
    long klen = user_str_ok(key, SETTING_KEY_MAX);
    if (klen < 0) return -1;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, cap)) return -1;
    char kkey[SETTING_KEY_MAX];
    memcpy(kkey, key, (size_t)klen);
    kkey[klen] = '\0';

    /* Stage in a kernel buffer so the user can't observe a half-
     * written value. Then copy out atomically. */
    char tmp[SETTING_VAL_MAX];
    size_t n = settings_get_str(kkey, tmp, sizeof(tmp), "");
    if (n + 1 > cap) n = (cap > 0) ? (cap - 1) : 0;
    memcpy(out, tmp, n);
    out[n] = '\0';
    return (long)n;
}

static long sys_setting_set(const char *key, const char *val) {
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE, "sys_setting_set"))
        return -1;
    long klen = user_str_ok(key, SETTING_KEY_MAX);
    if (klen < 0) return -1;
    long vlen = user_str_ok(val, SETTING_VAL_MAX);
    if (vlen < 0) return -1;
    char kkey[SETTING_KEY_MAX];
    char kval[SETTING_VAL_MAX];
    memcpy(kkey, key, (size_t)klen); kkey[klen] = '\0';
    memcpy(kval, val, (size_t)vlen); kval[vlen] = '\0';
    if (settings_set_str(kkey, kval) != 0) return -1;
    /* Persist immediately so the new value survives a reboot even if
     * the calling app crashes before it gets a chance to save. */
    (void)settings_save();
    return 0;
}

static long sys_login(const char *username) {
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE, "sys_login")) return -1;
    long n = user_str_ok(username, SESSION_USER_MAX);
    if (n < 0) return -1;
    char kname[SESSION_USER_MAX];
    memcpy(kname, username, (size_t)n);
    kname[n] = '\0';
    return session_login(kname) == 0 ? 0 : -1;
}

static long sys_logout(void) {
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE, "sys_logout")) return -1;
    return session_logout() == 0 ? 0 : -1;
}

static long sys_session_info(char *out, size_t cap) {
    if (cap == 0) return 0;
    if (cap > 1024) cap = 1024;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, cap)) return -1;

    struct session_info info;
    session_get_info(&info);

    /* Build a single line: "id=N active=B user=NAME\n". */
    char buf[128];
    /* Manual format because we want to keep the kernel-side dependency
     * footprint small (no kvprintf round-trip into a user buffer). */
    size_t i = 0;
    const char *p1 = "id=";
    while (*p1 && i < sizeof(buf) - 1) buf[i++] = *p1++;
    /* Decimal id (positive). */
    {
        char tmp[16]; int k = 0;
        unsigned v = (unsigned)info.id;
        if (v == 0) tmp[k++] = '0';
        while (v) { tmp[k++] = (char)('0' + v % 10u); v /= 10u; }
        while (k && i < sizeof(buf) - 1) buf[i++] = tmp[--k];
    }
    const char *p2 = " active=";
    while (*p2 && i < sizeof(buf) - 1) buf[i++] = *p2++;
    if (i < sizeof(buf) - 1) buf[i++] = info.active ? '1' : '0';
    const char *p3 = " user=";
    while (*p3 && i < sizeof(buf) - 1) buf[i++] = *p3++;
    for (int j = 0; info.username[j] && i < sizeof(buf) - 1; j++) {
        buf[i++] = info.username[j];
    }
    if (i < sizeof(buf) - 1) buf[i++] = '\n';
    buf[i] = '\0';

    if (i + 1 > cap) i = (cap > 0) ? (cap - 1) : 0;
    memcpy(out, buf, i);
    out[i] = '\0';
    return (long)i;
}

/* ---- user identity (milestone 15) ----------------------------- */

static long sys_getuid(void) {
    struct proc *p = current_proc();
    return p ? p->uid : 0;
}

static long sys_getgid(void) {
    struct proc *p = current_proc();
    return p ? p->gid : 0;
}

static long sys_username(int uid, char *out, size_t cap) {
    if (cap == 0) return 0;
    if (cap > 256) cap = 256;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, cap)) return -1;

    int target = uid;
    if (target < 0) {
        struct proc *p = current_proc();
        target = p ? p->uid : 0;
    }
    const struct user *u = users_lookup_by_uid(target);
    if (!u) {
        out[0] = '\0';
        return 0;
    }
    size_t n = 0;
    while (u->name[n] && n + 1 < cap) { out[n] = u->name[n]; n++; }
    out[n] = '\0';
    return (long)n;
}

static long sys_chmod(const char *path, uint32_t mode) {
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE, "sys_chmod")) return -1;
    long plen = user_str_ok(path, VFS_PATH_MAX);
    if (plen < 0) return -1;
    char kpath[VFS_PATH_MAX];
    memcpy(kpath, path, (size_t)plen);
    kpath[plen] = '\0';
    return vfs_chmod(kpath, mode);
}

static long sys_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE, "sys_chown")) return -1;
    long plen = user_str_ok(path, VFS_PATH_MAX);
    if (plen < 0) return -1;
    char kpath[VFS_PATH_MAX];
    memcpy(kpath, path, (size_t)plen);
    kpath[plen] = '\0';
    return vfs_chown(kpath, uid, gid);
}

/* ============================================================
 * Milestone 25A: libc-shape syscalls.
 *
 * Each helper below maps a single ABI_SYS_* number to a kernel
 * subsystem call. They follow these uniform rules:
 *
 *   - Validate every user pointer with user_buf_ok / user_str_ok.
 *   - Stage strings into a kernel buffer before consuming them so
 *     a concurrent thread (or future one) can't mutate them mid-call.
 *   - Return value:
 *       >= 0   : success
 *       -ABI_E*: explicit error code (preferred for new code)
 *       -1     : legacy "any error" (kept for compatibility with the
 *                older calls that already use it).
 *
 * No allocation across an error return: all helpers either succeed
 * fully or roll back and return without modifying caller-visible
 * state.
 * ============================================================ */

/* ---- process introspection ------------------------------------- */

static long sys_getpid(void) {
    struct proc *p = current_proc();
    return p ? p->pid : -ABI_EPERM;
}

static long sys_getppid(void) {
    struct proc *p = current_proc();
    return p ? p->ppid : 0;
}

/* Resolve a user-provided path against the calling proc's cwd. The
 * result lives in `out` (caller-owned buffer of size `cap`). Returns
 * 0 on success, -ABI_E* on failure. Absolute paths copy verbatim;
 * relative paths get prefixed with cwd + '/'. */
static int resolve_user_path(const char *user_path, size_t plen,
                             char *out, size_t cap) {
    if (plen == 0 || cap == 0) return -ABI_EINVAL;
    /* Absolute? */
    if (user_path[0] == '/') {
        if (plen + 1 > cap) return -ABI_ENAMETOOLONG;
        memcpy(out, user_path, plen);
        out[plen] = '\0';
        return 0;
    }
    struct proc *p = current_proc();
    const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
    size_t clen = strlen(cwd);
    /* Need cwd + '/' + path + NUL, but skip the slash if cwd already
     * ends with one (e.g. cwd == "/"). */
    bool need_slash = (clen == 0 || cwd[clen - 1] != '/');
    size_t need = clen + (need_slash ? 1 : 0) + plen + 1;
    if (need > cap) return -ABI_ENAMETOOLONG;
    memcpy(out, cwd, clen);
    if (need_slash) out[clen++] = '/';
    memcpy(out + clen, user_path, plen);
    out[clen + plen] = '\0';
    return 0;
}

/* ---- file open / close / dup ----------------------------------- */

static long sys_open(const char *path, int flags, int mode) {
    (void)mode;     /* M25A: permissions on creation not honoured yet */
    long plen = user_str_ok(path, ABI_PATH_MAX);
    if (plen < 0) return -ABI_EFAULT;
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, (size_t)plen, kpath, sizeof(kpath));
    if (rr) return rr;

    int access = flags & ABI_O_ACCMODE;
    bool want_create = (flags & ABI_O_CREAT) != 0;
    bool want_excl   = (flags & ABI_O_EXCL)  != 0;
    bool want_trunc  = (flags & ABI_O_TRUNC) != 0;
    bool want_append = (flags & ABI_O_APPEND)!= 0;
    (void)want_append; /* honoured at write-time once we plumb seek */

    /* Optionally create. Returns EEXIST if O_EXCL set and present. */
    if (want_create) {
        struct vfs_stat st;
        int sr = vfs_stat(kpath, &st);
        if (sr == VFS_OK && want_excl) return -ABI_EEXIST;
        if (sr == VFS_ERR_NOENT) {
            int cr = vfs_create(kpath);
            if (cr != VFS_OK) return -ABI_EACCES;
        }
    }

    if (want_trunc) {
        /* Trivial truncate: unlink + recreate. The VFS doesn't expose
         * a real truncate primitive yet (M25A scope). Honoured only
         * when the file already exists. */
        struct vfs_stat st;
        if (vfs_stat(kpath, &st) == VFS_OK) {
            vfs_unlink(kpath);
            (void)vfs_create(kpath);
        }
    }

    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return -ABI_ENOMEM;
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_VFS;
    /* Mint the open-file-description refcount up-front. file_clone()
     * (used by dup/dup2/fork-style fd inheritance) shares this counter
     * across all derived struct file copies, and file_close() only
     * triggers the underlying vfs ops->close when it hits zero. We
     * allocate before vfs_open so the failure path doesn't have to
     * unwind a successfully-opened handle on a refcount OOM. */
    f->vfs_refs = (int *)kmalloc(sizeof(int));
    if (!f->vfs_refs) { kfree(f); return -ABI_ENOMEM; }
    *f->vfs_refs = 1;

    int rc = vfs_open(kpath, &f->vfs);
    if (rc != VFS_OK) {
        kfree(f->vfs_refs);
        kfree(f);
        switch (rc) {
        case VFS_ERR_NOENT: return -ABI_ENOENT;
        case VFS_ERR_ISDIR: return -ABI_EISDIR;
        case VFS_ERR_PERM:  return -ABI_EACCES;
        case VFS_ERR_NOMOUNT: return -ABI_ENOENT;
        default:            return -ABI_EIO;
        }
    }
    /* Reject write attempts on a read-only access mode early -- we
     * still let the file_write path enforce mount-level RO. */
    (void)access;

    struct proc *p = current_proc();
    int fd = fd_alloc_into(p, f);
    if (fd < 0) {
        /* Tear down via file_close so it follows the refcount path
         * (refs goes 1->0 -> ops->close + free refs). */
        file_close(f);
        return -ABI_EMFILE;
    }
    return fd;
}

/* lseek currently supports VFS-backed files only. Returns the new
 * absolute position or -ABI_E*. */
static long sys_lseek(int fd, int64_t off, int whence) {
    struct file *f = fd_lookup(fd);
    if (!f) return -ABI_EBADF;
    if (f->kind != FILE_KIND_VFS) return -ABI_EINVAL;
    int64_t cur  = (int64_t)f->vfs.pos;
    int64_t size = (int64_t)f->vfs.size;
    int64_t newp;
    switch (whence) {
    case ABI_SEEK_SET: newp = off;        break;
    case ABI_SEEK_CUR: newp = cur + off;  break;
    case ABI_SEEK_END: newp = size + off; break;
    default: return -ABI_EINVAL;
    }
    if (newp < 0) return -ABI_EINVAL;
    f->vfs.pos = (size_t)newp;
    if ((size_t)newp > f->vfs.size) f->vfs.size = (size_t)newp;
    return newp;
}

/* Translate a vfs_stat into the public abi_stat. */
static void fill_abi_stat(const struct vfs_stat *src, struct abi_stat *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->size = (uint64_t)src->size;
    uint32_t typ = (src->type == VFS_TYPE_DIR) ? ABI_S_IFDIR : ABI_S_IFREG;
    dst->mode = typ | (src->mode & ABI_S_IFMT ? src->mode : (src->mode & 0777));
    /* If the underlying fs doesn't carry mode info, default to 0644 / 0755. */
    if ((src->mode & 0777) == 0) {
        dst->mode |= (src->type == VFS_TYPE_DIR) ? 0755 : 0644;
    }
    dst->uid = src->uid;
    dst->gid = src->gid;
}

static long sys_stat(const char *path, struct abi_stat *out) {
    long plen = user_str_ok(path, ABI_PATH_MAX);
    if (plen < 0) return -ABI_EFAULT;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out))) return -ABI_EFAULT;
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, (size_t)plen, kpath, sizeof(kpath));
    if (rr) return rr;
    struct vfs_stat vs;
    int sr = vfs_stat(kpath, &vs);
    if (sr == VFS_ERR_NOENT) return -ABI_ENOENT;
    if (sr != VFS_OK)        return -ABI_EACCES;

    struct abi_stat tmp;
    fill_abi_stat(&vs, &tmp);
    memcpy(out, &tmp, sizeof(tmp));
    return 0;
}

static long sys_fstat(int fd, struct abi_stat *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out))) return -ABI_EFAULT;
    struct file *f = fd_lookup(fd);
    if (!f) return -ABI_EBADF;
    struct abi_stat tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (f->kind == FILE_KIND_VFS) {
        struct vfs_stat vs = {
            .type = VFS_TYPE_FILE, .size = f->vfs.size,
            .uid  = f->vfs.uid,    .gid  = f->vfs.gid,
            .mode = f->vfs.mode,
        };
        fill_abi_stat(&vs, &tmp);
    } else {
        /* Pseudo-files (console, pipe, socket, window, term) report a
         * minimal "character device"-like stat. We use ABI_S_IFREG
         * because libc programs commonly only check ISDIR vs ISREG;
         * the size is simply zero. */
        tmp.mode = ABI_S_IFREG | 0666;
    }
    memcpy(out, &tmp, sizeof(tmp));
    return 0;
}

static long sys_dup(int oldfd) {
    struct file *f = fd_lookup(oldfd);
    if (!f) return -ABI_EBADF;
    struct file *cl = file_clone(f);
    if (!cl) return -ABI_ENOMEM;
    int nfd = fd_alloc_into(current_proc(), cl);
    if (nfd < 0) {
        file_close(cl);
        return -ABI_EMFILE;
    }
    return nfd;
}

static long sys_dup2(int oldfd, int newfd) {
    if (newfd < 0 || newfd >= PROC_NFDS) return -ABI_EBADF;
    if (oldfd == newfd) {
        return fd_lookup(oldfd) ? newfd : -ABI_EBADF;
    }
    struct file *f = fd_lookup(oldfd);
    if (!f) return -ABI_EBADF;
    struct file *cl = file_clone(f);
    if (!cl) return -ABI_ENOMEM;
    struct proc *p = current_proc();
    if (p->fds[newfd]) {
        file_close(p->fds[newfd]);
        p->fds[newfd] = 0;
    }
    p->fds[newfd] = cl;
    return newfd;
}

static long sys_unlink(const char *path) {
    long plen = user_str_ok(path, ABI_PATH_MAX);
    if (plen < 0) return -ABI_EFAULT;
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, (size_t)plen, kpath, sizeof(kpath));
    if (rr) return rr;
    int rc = vfs_unlink(kpath);
    switch (rc) {
    case VFS_OK:        return 0;
    case VFS_ERR_NOENT: return -ABI_ENOENT;
    case VFS_ERR_ROFS:  return -ABI_EROFS;
    case VFS_ERR_PERM:  return -ABI_EACCES;
    default:            return -ABI_EACCES;
    }
}

static long sys_mkdir(const char *path, int mode) {
    (void)mode;     /* M25A: not honoured yet -- new dirs use proc owner */
    long plen = user_str_ok(path, ABI_PATH_MAX);
    if (plen < 0) return -ABI_EFAULT;
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, (size_t)plen, kpath, sizeof(kpath));
    if (rr) return rr;
    int rc = vfs_mkdir(kpath);
    switch (rc) {
    case VFS_OK:         return 0;
    case VFS_ERR_EXIST:  return -ABI_EEXIST;
    case VFS_ERR_ROFS:   return -ABI_EROFS;
    case VFS_ERR_NOENT:  return -ABI_ENOENT;
    case VFS_ERR_PERM:   return -ABI_EACCES;
    default:             return -ABI_EACCES;
    }
}

/* ---- memory: brk ----------------------------------------------- */

static long sys_brk(uintptr_t new_brk) {
    struct proc *p = current_proc();
    if (!p) return -ABI_EPERM;
    uint64_t got = proc_brk(p, (uint64_t)new_brk);
    if (got == 0 && new_brk != 0) return -ABI_ENOMEM;
    return (long)got;
}

/* ---- environment + cwd ----------------------------------------- */

static long sys_getcwd(char *out, size_t cap) {
    if (cap == 0) return -ABI_EINVAL;
    if (cap > ABI_PATH_MAX) cap = ABI_PATH_MAX;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, cap)) return -ABI_EFAULT;
    struct proc *p = current_proc();
    const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
    size_t n = strlen(cwd);
    if (n + 1 > cap) return -ABI_ERANGE;
    memcpy(out, cwd, n);
    out[n] = '\0';
    return (long)n;
}

static long sys_chdir(const char *path) {
    long plen = user_str_ok(path, ABI_PATH_MAX);
    if (plen < 0) return -ABI_EFAULT;
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, (size_t)plen, kpath, sizeof(kpath));
    if (rr) return rr;
    struct vfs_stat st;
    int sr = vfs_stat(kpath, &st);
    if (sr == VFS_ERR_NOENT) return -ABI_ENOENT;
    if (sr != VFS_OK)        return -ABI_EACCES;
    if (st.type != VFS_TYPE_DIR) return -ABI_ENOTDIR;
    struct proc *p = current_proc();
    size_t n = strlen(kpath);
    if (n >= ABI_PATH_MAX) n = ABI_PATH_MAX - 1;
    memcpy(p->cwd, kpath, n);
    p->cwd[n] = '\0';
    return 0;
}

/* SYS_GETENV is a kernel-side stub: the kernel does NOT maintain a
 * global env table. Userland libc walks envp on its stack directly
 * (see __toby_envp in start.S). The syscall always returns 0
 * ("variable not set"); programs that need the value should use
 * libc-side getenv().
 *
 * We still validate inputs so a buggy caller gets a clean error
 * rather than silent success. */
static long sys_getenv(const char *name, char *out, size_t cap) {
    long nlen = user_str_ok(name, 256);
    if (nlen < 0) return -ABI_EFAULT;
    if (cap > 0 && !user_buf_ok((uint64_t)(uintptr_t)out, cap))
        return -ABI_EFAULT;
    if (cap > 0) out[0] = '\0';
    return 0;
}

/* ---- time ------------------------------------------------------ */

static long sys_nanosleep(uint64_t ns) {
    /* Implementation: spin-yield until enough wall time has passed.
     * Resolution is set by the perf tick (~10 ms on QEMU) -- good
     * enough for the uses libc has in M25A (sleep, usleep). */
    uint64_t start = perf_now_ns();
    uint64_t end   = start + ns;
    while (perf_now_ns() < end) {
        sched_yield();
    }
    return 0;
}

static long sys_clock_ms(void) {
    return (long)(perf_now_ns() / 1000000ull);
}

static long sys_abi_version(void) {
    return TOBY_ABI_VERSION;
}

/* ---- spawn / waitpid ------------------------------------------- */

/* Copy one user pointer-array (NULL-terminated) into a fresh
 * kernel-allocated array of kstrdup'd strings. *out_count is the
 * number of strings (excluding the NULL terminator). The caller
 * frees the strings + the array via free_kvec. */
static int copy_kvec_in(char *const *user_arr,
                        int max_entries, int max_strlen,
                        char ***out_arr, int *out_count) {
    *out_arr = 0;
    *out_count = 0;
    if (!user_arr) return 0;
    if (!user_buf_ok((uint64_t)(uintptr_t)user_arr,
                     sizeof(char *) * 1)) return -ABI_EFAULT;

    /* Count first. */
    int n = 0;
    for (;;) {
        if (!user_buf_ok((uint64_t)(uintptr_t)(user_arr + n),
                         sizeof(char *))) return -ABI_EFAULT;
        const char *s = user_arr[n];
        if (s == 0) break;
        if (n >= max_entries) return -ABI_E2BIG;
        n++;
    }
    if (n == 0) return 0;

    char **arr = (char **)kmalloc(sizeof(char *) * (size_t)(n + 1));
    if (!arr) return -ABI_ENOMEM;
    memset(arr, 0, sizeof(char *) * (size_t)(n + 1));

    for (int i = 0; i < n; i++) {
        const char *s = user_arr[i];
        long sl = user_str_ok(s, max_strlen);
        if (sl < 0) goto fail;
        char *kc = (char *)kmalloc((size_t)sl + 1);
        if (!kc) goto fail;
        memcpy(kc, s, (size_t)sl);
        kc[sl] = '\0';
        arr[i] = kc;
    }
    arr[n] = 0;
    *out_arr   = arr;
    *out_count = n;
    return 0;

fail:
    for (int i = 0; i < n; i++) if (arr[i]) kfree(arr[i]);
    kfree(arr);
    return -ABI_ENOMEM;
}

static void free_kvec(char **arr) {
    if (!arr) return;
    for (int i = 0; arr[i]; i++) kfree(arr[i]);
    kfree(arr);
}

static long sys_spawn(const struct abi_spawn_req *req) {
    if (!cap_check(current_proc(), CAP_EXEC, "sys_spawn")) return -ABI_EPERM;
    if (!user_buf_ok((uint64_t)(uintptr_t)req, sizeof(*req))) return -ABI_EFAULT;

    /* Snapshot the request into the kernel up front so a concurrent
     * user mutation can't change pointers we already validated. */
    struct abi_spawn_req kreq;
    memcpy(&kreq, req, sizeof(kreq));
    if (kreq.flags != 0) return -ABI_EINVAL;

    long plen = user_str_ok(kreq.path, ABI_PATH_MAX);
    if (plen < 0) return -ABI_EFAULT;
    char kpath[ABI_PATH_MAX];
    memcpy(kpath, kreq.path, (size_t)plen);
    kpath[plen] = '\0';

    char **kargv = 0; int kargc = 0;
    char **kenvp = 0; int kenvc = 0;
    int rc = copy_kvec_in((char *const *)kreq.argv,
                          ABI_ARGV_MAX, ABI_ARG_MAX,
                          &kargv, &kargc);
    if (rc < 0) return rc;
    rc = copy_kvec_in((char *const *)kreq.envp,
                      ABI_ENVP_MAX, ABI_ARG_MAX,
                      &kenvp, &kenvc);
    if (rc < 0) { free_kvec(kargv); return rc; }

    /* Map fd0/fd1/fd2 into struct file pointers, dup-cloning when the
     * caller passed an explicit fd (so the parent's copy is independent
     * of the child's). */
    struct proc *parent = current_proc();
    struct file *f0 = 0, *f1 = 0, *f2 = 0;
    int fds[3]      = { kreq.fd0, kreq.fd1, kreq.fd2 };
    struct file **out[3] = { &f0, &f1, &f2 };
    bool failed = false;
    for (int i = 0; i < 3 && !failed; i++) {
        int v = fds[i];
        if (v == ABI_SPAWN_FD_CONSOLE) {
            *out[i] = 0;            /* let spawn_internal install console */
            continue;
        }
        if (v == ABI_SPAWN_FD_INHERIT) {
            v = i;                   /* inherit parent's same-numbered fd */
        }
        if (v < 0 || v >= PROC_NFDS || !parent->fds[v]) {
            failed = true; break;
        }
        struct file *cl = file_clone(parent->fds[v]);
        if (!cl) { failed = true; break; }
        *out[i] = cl;
    }
    if (failed) {
        if (f0) file_close(f0);
        if (f1) file_close(f1);
        if (f2) file_close(f2);
        free_kvec(kargv); free_kvec(kenvp);
        return -ABI_EBADF;
    }

    struct proc_spec spec = {
        .path = kpath, .name = 0,
        .fd0 = f0, .fd1 = f1, .fd2 = f2,
        .argc = kargc, .argv = kargv,
        .envc = kenvc, .envp = kenvp,
        .sandbox_profile = 0,
        .cwd = 0,
    };
    int pid = proc_spawn(&spec);

    /* Whether spawn succeeded or not, our k-copies of argv/envp have
     * already been deep-copied onto the child's user stack and are no
     * longer needed. */
    free_kvec(kargv);
    free_kvec(kenvp);

    /* If proc_spawn fails, the file_clone'd fds are NOT yet owned by
     * the child; release them. On success they were transferred. */
    if (pid < 0) {
        if (f0) file_close(f0);
        if (f1) file_close(f1);
        if (f2) file_close(f2);
        return -ABI_ENOMEM;
    }
    return pid;
}

/* ---- Milestone 25D: dynamic loader helper -------------------- */

static long sys_dload(const char *path, uint64_t base,
                      struct abi_dlmap_info *out_user) {
    /* Defensive validation. dload installs new user mappings, so a
     * caller without CAP_EXEC has no business doing this. */
    if (!cap_check(current_proc(), CAP_EXEC, "sys_dload")) return -ABI_EPERM;

    long plen = user_str_ok(path, ABI_PATH_MAX);
    if (plen < 0) return -ABI_EFAULT;
    if (!user_buf_ok((uint64_t)(uintptr_t)out_user, sizeof(*out_user)))
        return -ABI_EFAULT;
    if ((base & (PAGE_SIZE - 1)) != 0) return -ABI_EINVAL;
    if (base == 0 || base >= 0x0000800000000000ULL) return -ABI_EINVAL;

    /* Resolve under the caller's sandbox (so dload can't be used to
     * pull arbitrary files outside the per-session FS root). */
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, (size_t)plen, kpath, sizeof(kpath));
    if (rr) return rr;

    void  *image     = 0;
    size_t image_sz  = 0;
    int    rc        = vfs_read_all(kpath, &image, &image_sz);
    if (rc != VFS_OK) {
        switch (rc) {
        case VFS_ERR_NOENT: return -ABI_ENOENT;
        case VFS_ERR_PERM:  return -ABI_EACCES;
        default:            return -ABI_EIO;
        }
    }

    /* Only ET_DYN is loadable via dload. ET_EXEC has fixed vaddrs,
     * isn't relocatable, and is meant to be the program -- not a
     * library mapped into someone else's address space. */
    if (image_sz < sizeof(Elf64_Ehdr)) {
        kfree(image);
        return -ABI_EINVAL;
    }
    {
        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
        if (eh->e_type != ET_DYN) {
            kfree(image);
            return -ABI_EINVAL;
        }
    }

    /* Find PT_DYNAMIC's vaddr (the only PHDR data the dynamic linker
     * really needs from us). Walked alongside the Ehdr from the
     * still-in-kernel image; we don't trust the now-loaded user
     * pages for this since the user could remap them under our
     * feet later. */
    uint64_t dyn_va = 0;
    {
        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
                               ((const uint8_t *)image + eh->e_phoff);
        for (Elf64_Half i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type == 2 /* PT_DYNAMIC */) {
                dyn_va = ph[i].p_vaddr + base;
                break;
            }
        }
    }

    /* Load into the caller's address space. CR3 is already this
     * process's PML4 -- we just point the editor at it for the
     * duration of vmm_map calls inside elf_load_user_at. */
    uint64_t old_editor = vmm_set_editor_root(read_cr3());

    struct elf_load_info info = {0};
    bool ok = elf_load_user_at(image, image_sz, base, &info);

    vmm_set_editor_root(old_editor);
    kfree(image);

    if (!ok) return -ABI_ENOMEM;

    /* PT_INTERP nested inside a library is nonsense; reject so we
     * never accidentally chain-load a second linker. */
    if (info.has_interp) return -ABI_EINVAL;

    struct abi_dlmap_info kout = {
        .base    = info.load_base,
        .entry   = info.entry,
        .dynamic = dyn_va,
        .phdr    = info.phdr_va,
        .phnum   = info.phnum,
        .phent   = info.phent,
        ._pad    = 0,
    };
    memcpy(out_user, &kout, sizeof(kout));
    return 0;
}

/* ---- Milestone 26A: peripheral test harness ------------------- */

static long sys_dev_list(struct abi_dev_info *out, uint32_t cap,
                         uint32_t mask) {
    if (cap == 0) return 0;
    if (cap > ABI_DEVT_MAX_DEVICES) cap = ABI_DEVT_MAX_DEVICES;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out) * cap))
        return -ABI_EFAULT;

    /* Build the list in a kernel staging buffer and only memcpy the
     * exact populated prefix into user memory. Two reasons:
     *   (1) the per-driver ksnprintf paths assume aligned, kernel-half
     *       memory; we don't want to feed them a user pointer that
     *       could disappear under us mid-write;
     *   (2) it bounds the user-visible record count to "what we
     *       actually filled" without needing to clear the tail. */
    static struct abi_dev_info staging[ABI_DEVT_MAX_DEVICES];
    int n = devtest_enumerate(staging, (int)cap, mask);
    if (n > 0) memcpy(out, staging, sizeof(*out) * (size_t)n);
    return n;
}

static long sys_dev_test(const char *name, char *msg, uint32_t cap) {
    /* name: short (<= 32B). Bound it explicitly so a malicious or
     * confused caller can't trick user_str_ok into walking past
     * a guard page. */
    long nlen = user_str_ok(name, ABI_DEVT_NAME_MAX);
    if (nlen < 0) return -ABI_EFAULT;
    if (cap > ABI_DEVT_MSG_MAX) cap = ABI_DEVT_MSG_MAX;
    if (cap > 0 && !user_buf_ok((uint64_t)(uintptr_t)msg, cap))
        return -ABI_EFAULT;

    /* Copy the name into kernel memory (devtest_run does strcmp). */
    char kname[ABI_DEVT_NAME_MAX];
    memcpy(kname, name, (size_t)nlen);
    kname[nlen] = '\0';

    char kmsg[ABI_DEVT_MSG_MAX];
    int rc = devtest_run(kname, kmsg, sizeof kmsg);
    if (cap > 0 && msg) {
        size_t n = 0;
        while (n + 1 < cap && kmsg[n]) { msg[n] = kmsg[n]; n++; }
        msg[n] = '\0';
    }
    return rc;
}

/* ---- Milestone 26C: hot-plug event drain ---------------------- */
static long sys_hot_drain(struct abi_hot_event *out, uint32_t cap) {
    if (cap == 0) return 0;
    if (cap > ABI_DEVT_HOT_RING) cap = ABI_DEVT_HOT_RING;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out) * cap))
        return -ABI_EFAULT;

    /* Stage in the kernel: hotplug_drain may run with IRQs off, and
     * we don't want to hand the spinlock-held path a user pointer.
     * We then memcpy the populated prefix to user memory. */
    static struct abi_hot_event staging[ABI_DEVT_HOT_RING];
    int n = hotplug_drain(staging, (int)cap);
    if (n > 0) memcpy(out, staging, sizeof(*out) * (size_t)n);
    return n;
}

/* M27E: present-stats. Snapshot the gfx-layer counters into the
 * caller's buffer in one shot. The struct is fixed-size (64 bytes)
 * so we don't need length negotiation. */
static long sys_display_present_stats(struct abi_display_present_stats *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out)))
        return -ABI_EFAULT;
    struct gfx_present_stats g_stats;
    gfx_present_stats(&g_stats);
    uint64_t cmp_full = 0, cmp_partial = 0;
    gui_invalidate_stats(&cmp_full, &cmp_partial);
    struct abi_display_present_stats staging = {
        .total_flips        = g_stats.total_flips,
        .full_flips         = g_stats.full_flips,
        .partial_flips      = g_stats.partial_flips,
        .empty_flips        = g_stats.empty_flips,
        .partial_pixels     = g_stats.partial_pixels,
        .full_pixels        = g_stats.full_pixels,
        .cmp_full_frames    = cmp_full,
        .cmp_partial_frames = cmp_partial,
    };
    memcpy(out, &staging, sizeof(staging));
    return 0;
}

/* ---- Milestone 27A: display introspection --------------------- */
static long sys_display_info(struct abi_display_info *out, uint32_t cap) {
    if (cap == 0) return 0;
    if (cap > ABI_DISPLAY_MAX_OUTPUTS) cap = ABI_DISPLAY_MAX_OUTPUTS;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out) * cap))
        return -ABI_EFAULT;

    /* Stage like every other devtest-shape syscall does: the
     * display_enumerate path uses ksnprintf-style copies which assume
     * kernel-resident memory. Kernel staging then memcpys the populated
     * prefix to user space in one shot. */
    static struct abi_display_info staging[ABI_DISPLAY_MAX_OUTPUTS];
    int n = display_enumerate(staging, (int)cap);
    if (n > 0) memcpy(out, staging, sizeof(*out) * (size_t)n);
    return n;
}

/* ---- Milestone 28A: structured logging ----------------------- */

static long sys_slog_read(struct abi_slog_record *out, uint32_t cap,
                          uint64_t since_seq) {
    if (cap == 0) return 0;
    if (cap > ABI_SLOG_RING_DEPTH) cap = ABI_SLOG_RING_DEPTH;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out) * cap))
        return -ABI_EFAULT;
    /* Stage in the kernel: the ring spinlock is taken inside slog_drain
     * and the slot snapshots are byte-copied; we then memcpy out in
     * one shot. */
    static struct abi_slog_record staging[ABI_SLOG_RING_DEPTH];
    uint32_t n = slog_drain(staging, cap, since_seq);
    if (n > 0) memcpy(out, staging, sizeof(*out) * (size_t)n);
    return (long)n;
}

static long sys_slog_write(uint32_t level, const char *sub_user,
                           const char *msg_user) {
    long sub_len = user_str_ok(sub_user, ABI_SLOG_SUB_MAX);
    long msg_len = user_str_ok(msg_user, ABI_SLOG_MSG_MAX);
    if (sub_len < 0 || msg_len < 0) return -ABI_EFAULT;
    if (level >= ABI_SLOG_LEVEL_MAX) return -ABI_EINVAL;

    /* Copy strings into kernel memory so the ring writer never
     * dereferences user pointers. */
    char ksub[ABI_SLOG_SUB_MAX];
    char kmsg[ABI_SLOG_MSG_MAX];
    memcpy(ksub, sub_user, (size_t)sub_len); ksub[sub_len] = '\0';
    memcpy(kmsg, msg_user, (size_t)msg_len); kmsg[msg_len] = '\0';

    int32_t pid = -1;
    struct proc *p = current_proc();
    if (p) pid = (int32_t)p->pid;
    slog_emit_pid(pid, level, ksub, kmsg);
    return 0;
}

static long sys_slog_stats(struct abi_slog_stats *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out)))
        return -ABI_EFAULT;
    struct abi_slog_stats staging;
    slog_stats(&staging);
    memcpy(out, &staging, sizeof(staging));
    return 0;
}

/* ---- Milestone 28C: watchdog status ------------------------- */

static long sys_wdog_status(struct abi_wdog_status *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out)))
        return -ABI_EFAULT;
    struct abi_wdog_status staging;
    wdog_status(&staging);
    memcpy(out, &staging, sizeof(staging));
    return 0;
}

/* ---- Milestone 28D: safe-mode probe ------------------------- */

static long sys_safe_mode(void) {
    /* No buffer, just a 0/1 verdict. Always succeeds. */
    return safemode_active() ? 1 : 0;
}

/* ---- Milestone 28E: filesystem check ------------------------ */

/* Cookie used by vfs_iter_mounts() to find the tobyfs mount whose
 * point matches the requested path. We can't grab the mount data
 * directly because vfs.c keeps that table file-private. */
struct fscheck_lookup {
    const char *want_point;
    void       *mount_data;     /* set on hit                  */
    bool        is_tobyfs;      /* set on hit                  */
};

extern const void *tobyfs_ops_addr(void);  /* forward decl below */

static bool fscheck_lookup_cb(const char *mount_point,
                              const struct vfs_ops *ops,
                              void *mount_data,
                              void *cookie) {
    struct fscheck_lookup *lk = (struct fscheck_lookup *)cookie;
    if (strcmp(mount_point, lk->want_point) == 0) {
        lk->mount_data = mount_data;
        lk->is_tobyfs  = (ops == tobyfs_ops_addr());
        return false; /* stop walking */
    }
    return true;
}

static long sys_fs_check(const char *path,
                         struct abi_fscheck_report *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out)))
        return -ABI_EFAULT;
    long plen = user_str_ok(path, ABI_FSCHECK_PATH_MAX);
    if (plen <= 0) return -ABI_EINVAL;

    /* Stage path into kernel space so the iteration callback sees a
     * stable, NUL-terminated buffer regardless of user paging. */
    char kpath[ABI_FSCHECK_PATH_MAX];
    for (long i = 0; i < plen; i++) kpath[i] = path[i];
    kpath[plen] = '\0';

    struct abi_fscheck_report staging;
    memset(&staging, 0, sizeof(staging));
    /* Always populate `path` so userland can see what the kernel
     * actually probed even on failure. */
    for (long i = 0; i < plen; i++) staging.path[i] = kpath[i];

    /* Find the mount. */
    struct fscheck_lookup lk = {
        .want_point  = kpath,
        .mount_data  = NULL,
        .is_tobyfs   = false,
    };
    vfs_iter_mounts(fscheck_lookup_cb, &lk);
    if (!lk.mount_data) {
        staging.status = ABI_FSCHECK_UNMOUNTED;
        const char *msg = "no filesystem mounted at this path";
        for (uint32_t i = 0; i < sizeof(staging.detail) - 1 && msg[i]; i++)
            staging.detail[i] = msg[i];
        memcpy(out, &staging, sizeof(staging));
        return -ABI_ENOENT;
    }

    if (lk.is_tobyfs) {
        const char *fs_type = "tobyfs";
        for (uint32_t i = 0; i < sizeof(staging.fs_type) - 1 && fs_type[i]; i++)
            staging.fs_type[i] = fs_type[i];

        struct tobyfs_check chk;
        int rc = tobyfs_check_mounted(lk.mount_data, &chk);
        if (rc != 0) {
            staging.status = ABI_FSCHECK_CORRUPT;
            staging.errors_found = chk.errors;
            const char *msg = chk.detail[0] ? chk.detail
                                            : "fscheck failed (I/O?)";
            for (uint32_t i = 0; i < sizeof(staging.detail) - 1 && msg[i]; i++)
                staging.detail[i] = msg[i];
            memcpy(out, &staging, sizeof(staging));
            return -ABI_EIO;
        }
        staging.errors_found    = chk.errors;
        staging.errors_repaired = chk.repaired;
        staging.total_bytes     = chk.bytes_total;
        staging.free_bytes      = chk.bytes_free;
        if (chk.severity == TFS_CHECK_OK) {
            staging.status = ABI_FSCHECK_OK;
        } else if (chk.severity == TFS_CHECK_WARN) {
            staging.status = ABI_FSCHECK_OK | ABI_FSCHECK_REPAIRED;
        } else {
            staging.status = ABI_FSCHECK_CORRUPT;
        }
        if (chk.detail[0]) {
            for (uint32_t i = 0;
                 i < sizeof(staging.detail) - 1 && chk.detail[i]; i++)
                staging.detail[i] = chk.detail[i];
        }
        memcpy(out, &staging, sizeof(staging));
        return 0;
    }

    /* Non-tobyfs mount (e.g. ramfs, fat32, /dev). M28E only verifies
     * tobyfs structurally; for everything else we just say "OK, type
     * unsupported by full check". */
    const char *msg = "no structural check available for this fs type";
    for (uint32_t i = 0; i < sizeof(staging.detail) - 1 && msg[i]; i++)
        staging.detail[i] = msg[i];
    staging.status = ABI_FSCHECK_OK;
    memcpy(out, &staging, sizeof(staging));
    return 0;
}

/* ---- Milestone 28G: stability self-test --------------------- */

/* Append `s` to `dst` at *off. Always NUL-terminates, never overruns. */
static void stab_cat(char *dst, size_t cap, size_t *off, const char *s) {
    if (!dst || !s || cap == 0) return;
    while (*s && *off + 1 < cap) dst[(*off)++] = *s++;
    dst[*off] = '\0';
}

/* Probe helpers. Each returns true on PASS, false on FAIL. They
 * append a short label/value pair to `r->detail` so the userland
 * tool can render the kernel's verdict verbatim. They MUST NOT
 * block, sleep, or hold any lock. */
static bool stab_boot(struct abi_stab_report *r, size_t *off) {
    r->boot_ms   = perf_now_ns() / 1000000ull;
    r->safe_mode = safemode_active() ? 1u : 0u;
    stab_cat(r->detail, sizeof(r->detail), off,
             safemode_active() ? "boot:safe " : "boot:normal ");
    return true;
}

static bool stab_log(struct abi_stab_report *r, size_t *off) {
    struct abi_slog_stats st;
    slog_stats(&st);
    char buf[64];
    ksnprintf(buf, sizeof(buf), "log:emit=%llu drop=%llu ",
              (unsigned long long)st.total_emitted,
              (unsigned long long)st.total_dropped);
    stab_cat(r->detail, sizeof(r->detail), off, buf);
    return st.total_emitted > 0;
}

static bool stab_panic(struct abi_stab_report *r, size_t *off) {
    /* PASS if /data/crash exists (panic infrastructure ready). On
     * a previously-crashed boot /data/crash/last.dump will also
     * be present; both states are healthy. */
    struct vfs_stat st;
    bool ok = (vfs_stat("/data/crash", &st) == VFS_OK);
    stab_cat(r->detail, sizeof(r->detail), off,
             ok ? "panic:ready " : "panic:noinit ");
    return ok;
}

static bool stab_watchdog(struct abi_stab_report *r, size_t *off) {
    struct abi_wdog_status st;
    wdog_status(&st);
    /* Use the kernel-side kick age, which is the watchdog's own
     * primary heartbeat signal. */
    uint64_t age = st.ms_since_kernel_kick;
    bool fresh = (st.timeout_ms == 0) || (age < st.timeout_ms);
    char buf[80];
    ksnprintf(buf, sizeof(buf), "wdog:age=%llu/to=%lu%s ",
              (unsigned long long)age,
              (unsigned long)st.timeout_ms,
              fresh ? "" : "(STALE)");
    stab_cat(r->detail, sizeof(r->detail), off, buf);
    return st.enabled && fresh;
}

static bool stab_filesystem(struct abi_stab_report *r, size_t *off) {
    /* Re-use the SYS_FS_CHECK probe path for /data. */
    struct fscheck_lookup lk = {
        .want_point = "/data", .mount_data = NULL, .is_tobyfs = false,
    };
    vfs_iter_mounts(fscheck_lookup_cb, &lk);
    if (!lk.mount_data) {
        stab_cat(r->detail, sizeof(r->detail), off, "fs:nomount ");
        /* No /data is acceptable on initrd-only boots; report but
         * still PASS the FS bit because there's nothing to validate. */
        return true;
    }
    if (!lk.is_tobyfs) {
        stab_cat(r->detail, sizeof(r->detail), off, "fs:nontobyfs ");
        return true;
    }
    struct tobyfs_check chk;
    int rc = tobyfs_check_mounted(lk.mount_data, &chk);
    if (rc != 0 || chk.severity != TFS_CHECK_OK) {
        char buf[80];
        ksnprintf(buf, sizeof(buf), "fs:BAD sev=%d errs=%u ",
                  chk.severity, chk.errors);
        stab_cat(r->detail, sizeof(r->detail), off, buf);
        return false;
    }
    char buf[64];
    ksnprintf(buf, sizeof(buf), "fs:ok i=%u/%u b=%u/%u ",
              chk.inodes_used, chk.inodes_total,
              chk.data_blocks_used, chk.data_blocks_total);
    stab_cat(r->detail, sizeof(r->detail), off, buf);
    return true;
}

static bool stab_services(struct abi_stab_report *r, size_t *off) {
    struct abi_service_info recs[8];
    uint32_t n = service_get_records(recs, 8);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (recs[i].kind == ABI_SVC_KIND_BUILTIN) {
            if (recs[i].state != ABI_SVC_STATE_RUNNING) bad++;
        } else {
            if (recs[i].state == ABI_SVC_STATE_FAILED) bad++;
        }
    }
    char buf[40];
    ksnprintf(buf, sizeof(buf), "svc:n=%u bad=%u ", n, bad);
    stab_cat(r->detail, sizeof(r->detail), off, buf);
    return n > 0 && bad == 0;
}

static bool stab_gui(struct abi_stab_report *r, size_t *off) {
    bool ok = gui_active();
    stab_cat(r->detail, sizeof(r->detail), off,
             ok ? "gui:active " : "gui:headless ");
    /* Headless safe-mode boots intentionally bring the compositor
     * down; treat that as a PASS. */
    if (!ok && safemode_active()) return true;
    return ok;
}

static bool stab_terminal(struct abi_stab_report *r, size_t *off) {
    /* Probe by creating + immediately closing a kernel-side session. We
     * deliberately bypass sys_term_open (and its CAP_TERM check) so the
     * probe doesn't accidentally fail in capability-restricted builds. */
    struct term_session *s = term_session_create();
    bool ok = (s != NULL);
    if (s) term_session_close(s);
    stab_cat(r->detail, sizeof(r->detail), off,
             ok ? "term:ok " : "term:closed ");
    if (!ok && safemode_active()) return true;
    return ok;
}

static bool stab_network(struct abi_stab_report *r, size_t *off) {
    bool up = net_is_up();
    stab_cat(r->detail, sizeof(r->detail), off, up ? "net:up " : "net:down ");
    /* Networking is best-effort; never fail the whole report on it. */
    return true;
}

static bool stab_input(struct abi_stab_report *r, size_t *off) {
    bool ok = (service_find("input") != 0);
    stab_cat(r->detail, sizeof(r->detail), off,
             ok ? "input:up " : "input:missing ");
    return ok;
}

static bool stab_safe_mode(struct abi_stab_report *r, size_t *off) {
    /* The safe-mode path is exercised by a separate /etc/safemode_now
     * boot. For the stability bit we just report it. */
    stab_cat(r->detail, sizeof(r->detail), off,
             safemode_active() ? "safe:on " : "safe:ready ");
    return true;
}

static bool stab_display(struct abi_stab_report *r, size_t *off) {
    int n = display_count();
    char buf[40];
    ksnprintf(buf, sizeof(buf), "disp:%d ", n);
    stab_cat(r->detail, sizeof(r->detail), off, buf);
    if (n <= 0 && safemode_active()) return true;
    return n > 0;
}

static long sys_stab_selftest(struct abi_stab_report *out, uint32_t mask) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out)))
        return -ABI_EFAULT;
    if (mask == 0) mask = ABI_STAB_OK_ALL;

    struct abi_stab_report r;
    memset(&r, 0, sizeof(r));
    r.expected_mask = mask;
    size_t off = 0;

    static const struct {
        uint32_t bit;
        bool (*fn)(struct abi_stab_report *, size_t *);
    } probes[] = {
        { ABI_STAB_OK_BOOT,        stab_boot       },
        { ABI_STAB_OK_LOG,         stab_log        },
        { ABI_STAB_OK_PANIC,       stab_panic      },
        { ABI_STAB_OK_WATCHDOG,    stab_watchdog   },
        { ABI_STAB_OK_FILESYSTEM,  stab_filesystem },
        { ABI_STAB_OK_SERVICES,    stab_services   },
        { ABI_STAB_OK_GUI,         stab_gui        },
        { ABI_STAB_OK_TERMINAL,    stab_terminal   },
        { ABI_STAB_OK_NETWORK,     stab_network    },
        { ABI_STAB_OK_INPUT,       stab_input      },
        { ABI_STAB_OK_SAFE_MODE,   stab_safe_mode  },
        { ABI_STAB_OK_DISPLAY,     stab_display    },
    };
    const int N = (int)(sizeof(probes) / sizeof(probes[0]));
    for (int i = 0; i < N; i++) {
        if ((mask & probes[i].bit) == 0) continue;
        if (probes[i].fn(&r, &off)) {
            r.result_mask |= probes[i].bit;
            r.pass_count++;
        } else {
            r.fail_count++;
        }
    }

    memcpy(out, &r, sizeof(r));
    return ((r.result_mask & mask) == mask) ? 0 : -ABI_EIO;
}

/* ---- Milestone 29A: hardware inventory snapshot -------------- */

/* SYS_HWINFO. Refresh the cached hardware summary (cheap; no
 * allocations) and copy it into the user struct. The kernel-side
 * implementation lives in hwinfo.c. */
static long sys_hwinfo(struct abi_hwinfo_summary *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out)))
        return -ABI_EFAULT;
    struct abi_hwinfo_summary snap;
    hwinfo_snapshot(&snap);
    memcpy(out, &snap, sizeof(snap));
    return 0;
}

/* ---- Milestone 29B: driver matching + fallback report -------- */

/* SYS_DRVMATCH. Look up (bus, vendor, device) in the kernel's
 * drvmatch table and copy the resulting record into the user
 * struct. The kernel-side implementation lives in drvmatch.c. */
static long sys_drvmatch(uint32_t bus, uint32_t vendor, uint32_t device,
                         struct abi_drvmatch_info *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out)))
        return -ABI_EFAULT;
    struct abi_drvmatch_info rec;
    long rc = drvmatch_query(bus, vendor, device, &rec);
    /* drvmatch_query() always populates rec, even when it returns
     * -ABI_ENOENT (it stamps the record with NONE/UNSUPPORTED so
     * userland can render it). Copy unconditionally. */
    memcpy(out, &rec, sizeof(rec));
    return rc;
}

/* ---- Milestone 35D: hardware compatibility database --------- */

/* SYS_HWCOMPAT_LIST. Snapshot the kernel's runtime PCI/USB
 * compatibility view into the caller's buffer. The implementation
 * lives in hwdb.c; this wrapper just validates the user pointer
 * (rejects unmapped pages with -EFAULT) and forwards through.
 *
 * `flags` is reserved for future use; we reject any non-zero value
 * so a future ABI bump can repurpose it without breaking older
 * userland. The cap is clamped to ABI_HWCOMPAT_MAX_ENTRIES so a
 * misbehaving caller can't trick the kernel into copying past the
 * end of its staging buffer. */
static long sys_hwcompat_list(struct abi_hwcompat_entry *out,
                              uint32_t cap, uint32_t flags) {
    if (flags != 0) return -ABI_EINVAL;
    if (cap == 0) return 0;
    if (cap > ABI_HWCOMPAT_MAX_ENTRIES) cap = ABI_HWCOMPAT_MAX_ENTRIES;
    if (!user_buf_ok((uint64_t)(uintptr_t)out,
                     (size_t)cap * sizeof(struct abi_hwcompat_entry))) {
        return -ABI_EFAULT;
    }
    /* Stage on the kernel stack (cap=64 -> 9 KiB max; the syscall
     * stack is 32 KiB), then bulk-copy into userland. Mirrors the
     * sys_notify_list / sys_dev_list pattern so the same auditing
     * lives in one place. */
    struct abi_hwcompat_entry staging[ABI_HWCOMPAT_MAX_ENTRIES];
    size_t n = hwdb_snapshot(staging, cap);
    if (n > 0) memcpy(out, staging, n * sizeof(staging[0]));
    return (long)n;
}

/* ---- Milestone 31: desktop notifications -------------------- */

/* Post a notification record into the kernel ring. The user record
 * is copied into kernel memory before notify_post() ever sees it,
 * so the ring writer never dereferences user pointers. Strings are
 * NUL-clamped against the ABI caps; the kernel decides id/flags/
 * time/kind itself, so the caller's spelling there is ignored.
 *
 * The caller's `app` field is overwritten with "user:<pid>" so the
 * compositor and notification center can always tell userland-
 * sourced toasts apart from kernel-emitted ones ("kernel",
 * "session", "service", ...). */
static long sys_notify_post(const struct abi_notification *user_rec) {
    if (!user_buf_ok((uint64_t)(uintptr_t)user_rec, sizeof(*user_rec)))
        return -ABI_EFAULT;

    struct abi_notification staging;
    memcpy(&staging, user_rec, sizeof(staging));

    /* Force NUL terminators on every string field; defends against a
     * caller that forgot. notify_post itself also clamps but doing
     * it here keeps the local copy obviously safe. */
    staging.app  [ABI_NOTIFY_APP_MAX   - 1] = '\0';
    staging.title[ABI_NOTIFY_TITLE_MAX - 1] = '\0';
    staging.body [ABI_NOTIFY_BODY_MAX  - 1] = '\0';

    /* Stamp source as user:<pid> so audit trails are useful even
     * when the caller forgot to fill `app`. */
    int32_t pid = -1;
    struct proc *p = current_proc();
    if (p) pid = (int32_t)p->pid;
    char src[ABI_NOTIFY_APP_MAX];
    if (pid >= 0) {
        ksnprintf(src, sizeof(src), "user:%d", (int)pid);
    } else {
        ksnprintf(src, sizeof(src), "user");
    }

    uint32_t id = notify_post(ABI_NOTIFY_KIND_USER,
                              staging.urgency,
                              src,
                              staging.title,
                              staging.body);
    if (id == 0) return -ABI_EBUSY;   /* notify subsystem not ready */
    return (long)id;
}

/* Snapshot up-to-`cap` non-dismissed notification records into the
 * caller's buffer (newest first). Returns the count written. */
static long sys_notify_list(struct abi_notification *out, uint32_t cap) {
    if (cap == 0) return 0;
    if (cap > 64) cap = 64;
    if (!user_buf_ok((uint64_t)(uintptr_t)out,
                     (size_t)cap * sizeof(struct abi_notification))) {
        return -ABI_EFAULT;
    }
    /* Stage on the kernel stack so notify_get_records() never
     * touches userland mid-walk. 64 * 200B = 12.8 KiB; the kernel
     * syscall stack is 32 KiB so this is comfortable. */
    struct abi_notification staging[64];
    uint32_t n = notify_get_records(staging, cap);
    if (n > 0) memcpy(out, staging, (size_t)n * sizeof(staging[0]));
    return (long)n;
}

/* Mark notification `id` as dismissed; id == 0 dismisses all. */
static long sys_notify_dismiss(uint32_t id) {
    if (id == 0) {
        notify_dismiss_all();
    } else {
        notify_dismiss(id);
    }
    return 0;
}

/* ---- Milestone 28F: service supervision query ---------------- */

/* Snapshot the kernel service registry into the caller's array.
 * Returns the number of records written (>=0). The kernel-side
 * implementation lives in service.c; this wrapper just enforces
 * the user-buffer contract. */
static long sys_svc_list(struct abi_service_info *out, uint32_t cap) {
    if (cap == 0 || cap > 64) return -ABI_EINVAL;
    if (!user_buf_ok((uint64_t)(uintptr_t)out,
                     (size_t)cap * sizeof(struct abi_service_info))) {
        return -ABI_EFAULT;
    }
    /* Stage into kernel memory so we never touch userland mid-tick. */
    struct abi_service_info staging[ABI_SVC_NAME_MAX > 0 ? 16 : 16];
    uint32_t want = cap;
    if (want > (uint32_t)(sizeof(staging) / sizeof(staging[0]))) {
        want = (uint32_t)(sizeof(staging) / sizeof(staging[0]));
    }
    uint32_t n = service_get_records(staging, want);
    if (n > 0) memcpy(out, staging, n * sizeof(staging[0]));
    return (long)n;
}

static long sys_waitpid(int pid, int *status_out, int flags) {
    if (status_out && !user_buf_ok((uint64_t)(uintptr_t)status_out,
                                   sizeof(*status_out))) {
        return -ABI_EFAULT;
    }

    if (flags & ABI_WNOHANG) {
        struct proc *child = proc_lookup(pid);
        if (!child) return -ABI_ENOENT;
        if (child->state != PROC_TERMINATED) return 0;
    }

    int code = proc_wait(pid);
    if (code < 0 && pid < 0) {
        /* proc_wait can't synthesize -ABI_E* directly; -1 means
         * "no such pid / waited on self". */
        return -ABI_ENOENT;
    }
    if (status_out) *status_out = code;
    return pid;
}

static long do_syscall(long num, long a1, long a2, long a3, long a4, long a5) {
    (void)a4; (void)a5;
    switch (num) {
    case SYS_EXIT:
        sys_exit((int)a1);
        /* unreachable */
    case SYS_WRITE:
        return sys_write((int)a1, (const void *)a2, (size_t)a3);
    case SYS_READ:
        return sys_read((int)a1, (void *)a2, (size_t)a3);
    case SYS_PIPE:
        return sys_pipe((int *)a1);
    case SYS_CLOSE:
        return sys_close((int)a1);
    case SYS_YIELD:
        return sys_yield();
    case SYS_SOCKET:
        return sys_socket((int)a1, (int)a2);
    case SYS_BIND:
        return sys_bind((int)a1, (uint16_t)a2);
    case SYS_SENDTO:
        return sys_sendto((int)a1, (const void *)a2, (size_t)a3,
                          (uint32_t)a4, (uint16_t)a5);
    case SYS_RECVFROM:
        return sys_recvfrom((int)a1, (void *)a2, (size_t)a3,
                            (struct sockaddr_in_be *)a4);
    case SYS_GUI_CREATE:
        return sys_gui_create((uint32_t)a1, (uint32_t)a2, (const char *)a3);
    case SYS_GUI_FILL:
        return sys_gui_fill((int)a1, (int)a2, (int)a3,
                            (uint32_t)a4, (uint32_t)a5);
    case ABI_SYS_GUI_FILL_ARGB:
        return sys_gui_fill_argb((int)a1, (int)a2, (int)a3,
                                 (uint32_t)a4, (uint32_t)a5);
    case ABI_SYS_GUI_TEXT_SCALED:
        return sys_gui_text_scaled((int)a1, (uint32_t)a2,
                                   (const char *)a3,
                                   (uint32_t)a4, (uint32_t)a5);
    case SYS_GUI_TEXT:
        return sys_gui_text((int)a1, (uint32_t)a2, (const char *)a3,
                            (uint32_t)a4, (uint32_t)a5);
    case SYS_GUI_FLIP:
        return sys_gui_flip((int)a1);
    case SYS_GUI_POLL_EVENT:
        return sys_gui_poll_event((int)a1, (struct gui_event *)a2);
    case SYS_TERM_OPEN:
        return sys_term_open();
    case SYS_TERM_WRITE:
        return sys_term_write((int)a1, (const void *)a2, (size_t)a3);
    case SYS_TERM_READ:
        return sys_term_read((int)a1, (void *)a2, (size_t)a3);
    case SYS_FS_READDIR:
        return sys_fs_readdir((const char *)a1,
                              (struct vfs_dirent_user *)a2,
                              (int)a3, (int)a4);
    case SYS_FS_READFILE:
        return sys_fs_readfile((const char *)a1, (void *)a2, (size_t)a3);
    case SYS_EXEC:
        return sys_exec((const char *)a1, (const char *)a2);
    case SYS_SETTING_GET:
        return sys_setting_get((const char *)a1, (char *)a2, (size_t)a3);
    case SYS_SETTING_SET:
        return sys_setting_set((const char *)a1, (const char *)a2);
    case SYS_LOGIN:
        return sys_login((const char *)a1);
    case SYS_LOGOUT:
        return sys_logout();
    case SYS_SESSION_INFO:
        return sys_session_info((char *)a1, (size_t)a2);
    case SYS_GETUID:
        return sys_getuid();
    case SYS_GETGID:
        return sys_getgid();
    case SYS_USERNAME:
        return sys_username((int)a1, (char *)a2, (size_t)a3);
    case SYS_CHMOD:
        return sys_chmod((const char *)a1, (uint32_t)a2);
    case SYS_CHOWN:
        return sys_chown((const char *)a1, (uint32_t)a2, (uint32_t)a3);

    /* ---- Milestone 25A: libc-shape calls ------------------------ */
    case SYS_GETPID:        return sys_getpid();
    case SYS_GETPPID:       return sys_getppid();
    case SYS_SPAWN:
        return sys_spawn((const struct abi_spawn_req *)a1);
    case SYS_WAITPID:
        return sys_waitpid((int)a1, (int *)a2, (int)a3);
    case SYS_OPEN:
        return sys_open((const char *)a1, (int)a2, (int)a3);
    case SYS_LSEEK:
        return sys_lseek((int)a1, (int64_t)a2, (int)a3);
    case SYS_STAT:
        return sys_stat((const char *)a1, (struct abi_stat *)a2);
    case SYS_FSTAT:
        return sys_fstat((int)a1, (struct abi_stat *)a2);
    case SYS_DUP:           return sys_dup((int)a1);
    case SYS_DUP2:          return sys_dup2((int)a1, (int)a2);
    case SYS_UNLINK:        return sys_unlink((const char *)a1);
    case SYS_MKDIR:         return sys_mkdir((const char *)a1, (int)a2);
    case SYS_BRK:           return sys_brk((uintptr_t)a1);
    case SYS_GETCWD:        return sys_getcwd((char *)a1, (size_t)a2);
    case SYS_CHDIR:         return sys_chdir((const char *)a1);
    case SYS_GETENV:
        return sys_getenv((const char *)a1, (char *)a2, (size_t)a3);
    case SYS_NANOSLEEP:     return sys_nanosleep((uint64_t)a1);
    case SYS_CLOCK_MS:      return sys_clock_ms();
    case SYS_ABI_VERSION:   return sys_abi_version();

    /* ---- Milestone 25D: dynamic loader helper -------------------- */
    case ABI_SYS_DLOAD:
        return sys_dload((const char *)a1, (uint64_t)a2,
                         (struct abi_dlmap_info *)a3);

    /* ---- Milestone 26A: peripheral test harness ------------------ */
    case ABI_SYS_DEV_LIST:
        return sys_dev_list((struct abi_dev_info *)a1,
                            (uint32_t)a2, (uint32_t)a3);
    case ABI_SYS_DEV_TEST:
        return sys_dev_test((const char *)a1, (char *)a2, (uint32_t)a3);

    /* ---- Milestone 26C: hot-plug event drain --------------------- */
    case ABI_SYS_HOT_DRAIN:
        return sys_hot_drain((struct abi_hot_event *)a1, (uint32_t)a2);

    /* ---- Milestone 27A: display introspection -------------------- */
    case ABI_SYS_DISPLAY_INFO:
        return sys_display_info((struct abi_display_info *)a1, (uint32_t)a2);
    case ABI_SYS_DISPLAY_PRESENT_STATS:
        return sys_display_present_stats(
            (struct abi_display_present_stats *)a1);

    /* ---- Milestone 28A: structured logging ---------------------- */
    case ABI_SYS_SLOG_READ:
        return sys_slog_read((struct abi_slog_record *)a1,
                             (uint32_t)a2, (uint64_t)a3);
    case ABI_SYS_SLOG_WRITE:
        return sys_slog_write((uint32_t)a1, (const char *)a2,
                              (const char *)a3);
    case ABI_SYS_SLOG_STATS:
        return sys_slog_stats((struct abi_slog_stats *)a1);

    /* ---- Milestone 28C: watchdog -------------------------------- */
    case ABI_SYS_WDOG_STATUS:
        return sys_wdog_status((struct abi_wdog_status *)a1);

    /* ---- Milestone 28D: safe-mode probe ------------------------- */
    case ABI_SYS_SAFE_MODE:
        return sys_safe_mode();

    /* ---- Milestone 28E: filesystem integrity check -------------- */
    case ABI_SYS_FS_CHECK:
        return sys_fs_check((const char *)a1,
                            (struct abi_fscheck_report *)a2);

    /* ---- Milestone 28F: service supervision query --------------- */
    case ABI_SYS_SVC_LIST:
        return sys_svc_list((struct abi_service_info *)a1, (uint32_t)a2);

    /* ---- Milestone 28G: stability self-test --------------------- */
    case ABI_SYS_STAB_SELFTEST:
        return sys_stab_selftest((struct abi_stab_report *)a1,
                                 (uint32_t)a2);

    /* ---- Milestone 29A: hardware inventory ---------------------- */
    case ABI_SYS_HWINFO:
        return sys_hwinfo((struct abi_hwinfo_summary *)a1);

    /* ---- Milestone 29B: driver matching + fallback report ------- */
    case ABI_SYS_DRVMATCH:
        return sys_drvmatch((uint32_t)a1, (uint32_t)a2, (uint32_t)a3,
                            (struct abi_drvmatch_info *)a4);

    /* ---- Milestone 35D: hardware compatibility database --------- */
    case ABI_SYS_HWCOMPAT_LIST:
        return sys_hwcompat_list((struct abi_hwcompat_entry *)a1,
                                 (uint32_t)a2, (uint32_t)a3);

    /* ---- Milestone 31: desktop notifications -------------------- */
    case ABI_SYS_NOTIFY_POST:
        return sys_notify_post((const struct abi_notification *)a1);
    case ABI_SYS_NOTIFY_LIST:
        return sys_notify_list((struct abi_notification *)a1, (uint32_t)a2);
    case ABI_SYS_NOTIFY_DISMISS:
        return sys_notify_dismiss((uint32_t)a1);

    default:
        kprintf("[syscall] unknown number %ld -- returning -ENOSYS\n", num);
        return -ABI_ENOSYS;
    }
}

long syscall_dispatch(long num, long a1, long a2, long a3, long a4, long a5) {
    /* ---- Milestone 26E: re-enable interrupts inside the syscall body.
     *
     * SYSCALL hardware clears RFLAGS.IF (IA32_FMASK has IF set), and the
     * .S trampoline jumps straight here without re-enabling. That used
     * to be fine because most syscalls only touched RAM-resident state
     * (initrd, in-memory file tables, sockets that completed in the IRQ
     * handler). M26E exposed the gap: `usbtest storage` -> sys_dev_test
     * -> usb_msc_selftest -> blk_read -> xhci_bulk_xfer_sync -> the
     * spin loop calls pit_sleep_ms(1), which in turn does `hlt`. With
     * IF=0 the CPU halts forever waiting for an interrupt that can't
     * fire, hanging the whole kernel.
     *
     * Re-enabling here is safe:
     *   - the SYSCALL trampoline has already swapped onto a per-process
     *     kernel stack and saved every user GP reg, so an IRQ here only
     *     clobbers volatile regs (which the C ABI lets it touch).
     *   - all kernel data structures touched by IRQ handlers (xhci event
     *     ring, pipe wait queues, ...) already use their own cli/sti
     *     critical sections, so nesting is fine.
     *   - we cli() again at the very end so the unwind + sysretq window
     *     stays atomic w.r.t. an interrupt arriving on the half-popped
     *     register stack -- sysretq will then reload IF=1 from the saved
     *     user RFLAGS in r11.
     *
     * Without this, ANY userland syscall that ends up waiting on a
     * bus-driven device (USB MSC, future SATA, future audio) would
     * deadlock the box. */
    sti();

    /* ---- Milestone 19: per-syscall perf zone + per-proc counter ---
     *
     * perf_syscall_enter/exit wrap the dispatch body. The overhead is
     * two rdtsc reads and one array increment when profiling is on;
     * when it's off (perf_set_enabled(false)) exit is a single early-
     * out branch.
     *
     * The outer PERF_Z_SYSCALL zone lets `perf` give an aggregate
     * "total time spent in syscalls" line, while the histogram
     * breaks that down per SYS_* number. */
    uint64_t t_sys;
    perf_syscall_enter((int)num, &t_sys);

    /* Per-proc bookkeeping -- cheap word increment, always on. */
    struct proc *caller = current_proc();
    if (caller) caller->syscall_count++;
    /* Global "syscalls serviced" counter used by the `perf` builtin. */
    perf_inc_total_syscalls();

    /* M28C: watchdog heartbeat from a syscall is the strongest sign
     * the userland process is making progress. Kick before dispatch
     * so even a syscall that blocks (proc_wait, recvfrom, ...) counts
     * as activity. */
    wdog_kick_proc(caller ? caller->pid : -1);

    if (log_enabled(LOG_CAT_SYSCALL)) {
        klog(LOG_CAT_SYSCALL, "pid=%d syscall num=%ld a1=0x%lx a2=0x%lx",
             caller ? caller->pid : -1, num,
             (unsigned long)a1, (unsigned long)a2);
    }

    long rv = do_syscall(num, a1, a2, a3, a4, a5);

    perf_syscall_exit((int)num, t_sys);
    perf_zone_end(PERF_Z_SYSCALL, t_sys);

    /* Drive the GUI compositor from the syscall return path.
     *
     * Milestone 19 optimization: skip gui_tick() entirely when the GUI
     * subsystem isn't active (serial-only shell path). The function is
     * already cheap-when-idle (early-out on !g.ready), but the call
     * itself still touches multiple globals; avoiding it on the hot
     * text-mode syscall path saves a measurable chunk on fast boxes
     * where the shell issues thousands of SYS_WRITE in a burst.
     *
     * Correctness: when the GUI isn't running, gui_tick has nothing
     * to do -- compositor state is idle, no launch queue to drain,
     * no service ticks to run (services tick is also gated behind
     * on_pid0 AND g.active internally). */
    if (gui_active()) {
        gui_tick();
    }

    /* Safe point #1: every syscall return runs through here. If the
     * kernel (or another proc, or the keyboard IRQ) sent us a signal
     * during the body, deliver it now -- proc_exit never returns. The
     * value already in `rv` is irrelevant in that case. */
    signal_deliver_if_pending();

    /* Re-mask interrupts before we return into the .S unwind. The
     * trampoline pops 14 registers and then does `mov rsp, [rsp]` to
     * jump back onto the user stack -- if an IRQ fired in that window
     * we'd execute the handler with a half-pop'd kernel stack and a
     * confused RSP. SYSRETQ will reload IF=1 from r11 (saved user
     * RFLAGS), so user mode runs with interrupts on as expected. */
    cli();
    return rv;
}

void syscall_init(void) {
    uint64_t efer = rdmsr(IA32_EFER);
    wrmsr(IA32_EFER, efer | EFER_SCE);

    uint64_t star = ((uint64_t)GDT_KERNEL_CS << 32) |
                    ((uint64_t)0x10          << 48);
    wrmsr(IA32_STAR, star);

    wrmsr(IA32_LSTAR, (uint64_t)&syscall_entry);
    wrmsr(IA32_FMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF);

    g_kernel_syscall_rsp = tss_kernel_rsp_top();

    kprintf("[sys] EFER.SCE on, STAR=0x%016lx, LSTAR=%p, FMASK=0x%lx\n",
            star, (void *)&syscall_entry,
            (unsigned long)(RFLAGS_IF | RFLAGS_DF | RFLAGS_TF));
    kprintf("[sys] kernel syscall rsp = %p\n", (void *)g_kernel_syscall_rsp);
    kprintf("[sys] available syscalls: %d=exit  %d=write  %d=read  "
            "%d=pipe  %d=close  %d=yield\n",
            SYS_EXIT, SYS_WRITE, SYS_READ, SYS_PIPE, SYS_CLOSE, SYS_YIELD);
    kprintf("[sys]                     %d=socket %d=bind %d=sendto %d=recvfrom\n",
            SYS_SOCKET, SYS_BIND, SYS_SENDTO, SYS_RECVFROM);
    kprintf("[sys]                     %d=gui_create %d=gui_fill %d=gui_text "
            "%d=gui_flip %d=gui_poll_event\n",
            SYS_GUI_CREATE, SYS_GUI_FILL, SYS_GUI_TEXT, SYS_GUI_FLIP,
            SYS_GUI_POLL_EVENT);
    kprintf("[sys]                     %d=term_open %d=term_write %d=term_read "
            "%d=fs_readdir %d=fs_readfile %d=exec\n",
            SYS_TERM_OPEN, SYS_TERM_WRITE, SYS_TERM_READ,
            SYS_FS_READDIR, SYS_FS_READFILE, SYS_EXEC);
    kprintf("[sys]                     %d=setting_get %d=setting_set "
            "%d=login %d=logout %d=session_info\n",
            SYS_SETTING_GET, SYS_SETTING_SET,
            SYS_LOGIN, SYS_LOGOUT, SYS_SESSION_INFO);
    kprintf("[sys]                     %d=getuid %d=getgid %d=username "
            "%d=chmod %d=chown\n",
            SYS_GETUID, SYS_GETGID, SYS_USERNAME, SYS_CHMOD, SYS_CHOWN);
    /* Milestone 25A: libc-shape calls. */
    kprintf("[sys]   M25A: %d=getpid %d=getppid %d=spawn %d=waitpid "
            "%d=open %d=lseek %d=stat %d=fstat\n",
            SYS_GETPID, SYS_GETPPID, SYS_SPAWN, SYS_WAITPID,
            SYS_OPEN, SYS_LSEEK, SYS_STAT, SYS_FSTAT);
    kprintf("[sys]         %d=dup %d=dup2 %d=unlink %d=mkdir %d=brk "
            "%d=getcwd %d=chdir %d=getenv\n",
            SYS_DUP, SYS_DUP2, SYS_UNLINK, SYS_MKDIR, SYS_BRK,
            SYS_GETCWD, SYS_CHDIR, SYS_GETENV);
    kprintf("[sys]         %d=nanosleep %d=clock_ms %d=abi_version "
            "(ABI v%u)\n",
            SYS_NANOSLEEP, SYS_CLOCK_MS, SYS_ABI_VERSION,
            (unsigned)TOBY_ABI_VERSION);
}
