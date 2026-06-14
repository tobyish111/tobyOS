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
#include <tobyos/virtio_gpu.h>
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
#include <tobyos/uaccess.h>
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
#include <tobyos/notify_svc.h>
#include <tobyos/theme.h>
#include <tobyos/sysmon.h>
#include <tobyos/mouse.h>
#include <tobyos/keyboard.h>
#include <tobyos/usb_legacy.h>
#include <tobyos/xhci.h>
#include <tobyos/http.h>
#include <tobyos/inotify.h>
#include <tobyos/clipboard.h>
#include <tobyos/smp.h>

extern void syscall_entry(void);

/* Phase 3 M3.2: fork/exec forward declarations */
extern long sys_fork(void);
extern long sys_execve(const char *path, char *const argv[], char *const envp[]);

/* TCP/TLS userland networking */
extern long sys_tcp_user_connect(uint32_t ip_be, uint16_t port_be, uint32_t timeout_ms);

/* Audio engine */
extern int sys_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t format);
extern long sys_audio_write(int stream_id, const void *samples, size_t count);
extern int sys_audio_close(int stream_id);
extern int sys_audio_volume(int stream_id, uint8_t volume);
extern long sys_tcp_user_send(int conn_id, const void *buf, uint32_t len);
extern long sys_tcp_user_recv(int conn_id, void *buf, uint32_t len);
extern long sys_tcp_user_close(int conn_id);
extern long sys_tcp_user_listen(uint16_t port_be, int backlog);
extern long sys_tcp_user_accept(int listen_id);
extern long sys_tls_user_connect(uint32_t ip_be, uint16_t port_be, const char *hostname);
extern long sys_tls_user_send(int tls_id, const void *buf, uint32_t len);
extern long sys_tls_user_recv(int tls_id, void *buf, uint32_t len);
extern long sys_tls_user_close(int tls_id);

/* Kernel module management */
extern long sys_module(uint64_t op, uint64_t arg1, uint64_t arg2);

/* Phase 1 M1.4: IPC forward declarations */
extern long sys_shm_open(const char *name, int flags, size_t size);
extern long sys_shm_map(int shm_id, uint64_t hint_addr);
extern long sys_shm_unlink(const char *name);
extern long sys_unix_socket(void);
extern long sys_unix_bind(int sockfd, const char *path);
extern long sys_unix_listen(int sockfd, int backlog);
extern long sys_unix_connect(int sockfd, const char *path);
extern long sys_unix_accept(int sockfd);
extern long sys_unix_send(int sockfd, const void *buf, size_t len);
extern long sys_unix_recv(int sockfd, void *buf, size_t len);
extern long sys_unix_close(int sockfd);

static void syscall_service_input(void) {
    usb_legacy_poll();
    xhci_poll();
    kbd_flush_pending();
    mouse_flush_pending();
}

/* MSR numbers. */
#define IA32_EFER       0xC0000080u
#define IA32_STAR       0xC0000081u
#define IA32_LSTAR      0xC0000082u
#define IA32_FMASK      0xC0000084u
#define IA32_GS_BASE    0xC0000101u   /* active GS base (per-CPU data ptr) */

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

/* ---- per-copy uaccess helpers (the SMAP window is per-copy now) --------
 *
 * The syscall body no longer runs under a blanket stac window, so every
 * access to user memory below goes through <tobyos/uaccess.h> accessors.
 * Two local conveniences:
 *
 * user_str_in(): copy a NUL-terminated user string into a kernel buffer.
 * Rejects NULL, bad ranges, and strings that don't fit (no silent path
 * truncation -- same contract as the old validate-only user_str_ok).
 *
 * bounce_in(): kmalloc a kernel copy of a user buffer (caller kfrees).
 * sys_read/sys_write-class syscalls bounce through kernel buffers so the
 * whole VFS/pipe/console/socket stack below never sees a user pointer. */
static bool user_str_in(char *ks, size_t cap, const char *us) {
    if (!us) return false;
    long n = strncpy_from_user(ks, us, cap);
    if (n < 0) return false;
    if ((size_t)n >= cap - 1) return false;   /* didn't fit (or exact-fit) */
    return true;
}

static void *bounce_in(const void *ubuf, size_t len) {
    void *k = kmalloc(len ? len : 1);
    if (!k) return 0;
    if (copy_from_user(k, ubuf, len) != 0) { kfree(k); return 0; }
    return k;
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
    struct file *f = fd_lookup(fd);
    if (!f) return -1;
    void *k = bounce_in(buf, len);
    if (!k) return -ABI_EFAULT;
    long rv = file_write(f, k, len);
    kfree(k);
    return rv;
}

static long sys_read(int fd, void *buf, size_t len) {
    if (len == 0) return 0;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) return -ABI_EFAULT;
    struct file *f = fd_lookup(fd);
    if (!f) return -1;
    void *k = kmalloc(len);
    if (!k) return -ABI_ENOMEM;
    long rv = file_read(f, k, len);
    if (rv > 0 && copy_to_user(buf, k, (size_t)rv) != 0) rv = -ABI_EFAULT;
    kfree(k);
    return rv;
}

/* SYS_PIPE: returns two fds. user_fds_out points at int[2] in userspace. */
static long sys_pipe(int *user_fds_out) {
    struct file *r = 0, *w = 0;
    if (pipe_create(&r, &w) != 0) return -1;

    struct proc *p = current_proc();
    int fd_r = fd_alloc_into(p, r);
    if (fd_r < 0) { file_close(r); file_close(w); return -1; }
    int fd_w = fd_alloc_into(p, w);
    if (fd_w < 0) { p->fds[fd_r] = 0; file_close(r); file_close(w); return -1; }

    int fds[2] = { fd_r, fd_w };
    if (copy_to_user(user_fds_out, fds, sizeof(fds)) != 0) {
        p->fds[fd_r] = 0; p->fds[fd_w] = 0;
        file_close(r); file_close(w);
        return -ABI_EFAULT;
    }
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
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_SOCKET || !f->sock) return -1;
    void *k = bounce_in(buf, len);
    if (!k) return -ABI_EFAULT;
    long rv = sock_sendto(f->sock, k, len, dst_ip_be, dst_port_be);
    kfree(k);
    return rv;
}

static long sys_recvfrom(int fd, void *buf, size_t len,
                         struct sockaddr_in_be *src_out) {
    if (!cap_check(current_proc(), CAP_NET, "sys_recvfrom")) return -1;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) return -ABI_EFAULT;
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_SOCKET || !f->sock) return -1;
    void *k = kmalloc(len ? len : 1);
    if (!k) return -ABI_ENOMEM;
    uint32_t src_ip = 0; uint16_t src_port = 0;
    long rv = sock_recvfrom(f->sock, k, len, &src_ip, &src_port);
    if (rv > 0 && copy_to_user(buf, k, (size_t)rv) != 0) rv = -ABI_EFAULT;
    kfree(k);
    if (rv >= 0 && src_out) {
        struct sockaddr_in_be sa = { .ip = src_ip, .port = src_port, ._pad = 0 };
        if (copy_to_user(src_out, &sa, sizeof(sa)) != 0) return -ABI_EFAULT;
    }
    return rv;
}

/* ---- GUI syscalls (milestone 10) ------------------------------- */

static long sys_gui_create(uint32_t w, uint32_t h, const char *title) {
    if (!cap_check(current_proc(), CAP_GUI, "sys_gui_create")) return -1;
    char tbuf[32];
    tbuf[0] = '\0';
    if (title) {
        if (strncpy_from_user(tbuf, title, sizeof(tbuf)) < 0) return -1;
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
    char buf[256];
    long n = strncpy_from_user(buf, s, sizeof(buf));
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
    /* Copy into a small kernel buffer so the user can't change the
     * string out from under us mid-draw. */
    char buf[256];
    long n = strncpy_from_user(buf, s, sizeof(buf));
    if (n < 0) return -1;
    int x = (int)(int16_t)(xy & 0xFFFFu);
    int y = (int)(int16_t)((xy >> 16) & 0xFFFFu);
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
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    /* Drain local input before deciding the GUI event queue is empty.
     * USB keyboard reports are completed by xhci_poll(); doing that only
     * after this syscall returns can add an extra app yield per keystroke. */
    syscall_service_input();
    /* Poll into a kernel-side struct first so a partial write to user
     * memory can't leave the queue in a half-consumed state. */
    struct gui_event ev;
    int got = gui_window_poll_event(f->win, &ev);
    if (got > 0 && copy_to_user(out, &ev, sizeof(*out)) != 0)
        return -ABI_EFAULT;
    if (got > 0 && gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("syscall: gui_poll_event fd=%d -> type=%d "
                       "xy=(%d,%d) btn=0x%02x key=0x%02x",
                       fd, ev.type, ev.x, ev.y,
                       (unsigned)ev.button, (unsigned)ev.key);
    }
    return got;
}

/* ---- window state / title syscalls (milestone 38) --------------- */

static long sys_gui_set_state(int fd, int state) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -ABI_EBADF;
    if (state < GUI_WIN_NORMAL || state > GUI_WIN_MAXIMIZED) return -ABI_EINVAL;
    return gui_window_set_state(f->win, state) == 0 ? 0 : -ABI_ENOMEM;
}

static long sys_gui_set_title(int fd, const char *title) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -ABI_EBADF;
    char tbuf[GUI_TITLE_MAX];
    if (strncpy_from_user(tbuf, title, sizeof(tbuf)) < 0) return -ABI_EFAULT;
    return gui_window_set_title(f->win, tbuf) == 0 ? 0 : -ABI_EINVAL;
}

static long sys_clip_copy(const char *data, uint32_t len) {
    if (!data) return -ABI_EFAULT;
    if (len > 4095) len = 4095;
    void *k = bounce_in(data, len);
    if (!k) return -ABI_EFAULT;
    long rv = gui_clip_copy(k, len);
    kfree(k);
    return rv;
}

static long sys_clip_paste(char *buf, uint32_t max) {
    if (!buf || max == 0) return -ABI_EINVAL;
    if (max > 4096) max = 4096;
    char *k = (char *)kmalloc(max);
    if (!k) return -ABI_ENOMEM;
    long rv = gui_clip_paste(k, max);
    /* gui_clip_paste NUL-terminates: rv excludes the NUL, so rv+1 bytes
     * were written (rv+1 <= max by its own clamping). */
    if (rv >= 0 && copy_to_user(buf, k, (size_t)rv + 1) != 0) rv = -ABI_EFAULT;
    kfree(k);
    return rv;
}

/* ---- HTTP GET syscall ------------------------------------------ */
#define HTTP_SYSCALL_MAX_BODY  65536u  /* 64KB cap for userspace fetches */
#define HTTP_MAX_REDIRECTS     5

static long sys_http_get(const char *url, void *buf, uint32_t buf_sz) {
    if (!cap_check(current_proc(), CAP_NET, "sys_http_get")) return -ABI_EPERM;
    if (!url || !buf || buf_sz == 0) return -ABI_EINVAL;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, buf_sz)) return -ABI_EFAULT;

    uint32_t cap = buf_sz < HTTP_SYSCALL_MAX_BODY ? buf_sz : HTTP_SYSCALL_MAX_BODY;

    /* Copy URL to kernel buffer so we can follow redirects */
    char cur_url[512];
    if (strncpy_from_user(cur_url, url, sizeof(cur_url)) < 0)
        return -ABI_EFAULT;

    for (int redir = 0; redir <= HTTP_MAX_REDIRECTS; redir++) {
        struct http_response resp;
        int rc = http_get(cur_url, (size_t)cap, HTTP_DEFAULT_TIMEOUT_MS, &resp);
        if (rc < 0) return (long)rc;

        /* Follow 301/302/303/307/308 redirects */
        if ((resp.status == 301 || resp.status == 302 || resp.status == 303 ||
             resp.status == 307 || resp.status == 308) && resp.location[0]) {
            size_t loc_len = 0;
            while (resp.location[loc_len]) loc_len++;
            if (loc_len < sizeof(cur_url)) {
                memcpy(cur_url, resp.location, loc_len + 1);
                http_free(&resp);
                continue;
            }
        }

        size_t copy = resp.body_len < (size_t)cap ? resp.body_len : (size_t)cap;
        if (copy_to_user(buf, resp.body, copy) != 0) {
            http_free(&resp);
            return -ABI_EFAULT;
        }
        http_free(&resp);
        return (long)copy;
    }

    return HTTP_ERR_PROTOCOL;
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
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) return -ABI_EFAULT;
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_TERM || !f->term) return -1;
    /* Bounce the input through a small kernel buffer so the user can't
     * mutate it while term_session_write_input reads byte-by-byte. */
    char tmp[256];
    size_t written = 0;
    while (written < len) {
        size_t chunk = len - written;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        if (copy_from_user(tmp, (const char *)buf + written, chunk) != 0)
            return -ABI_EFAULT;
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
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, cap)) return -ABI_EFAULT;
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
        if (copy_to_user((char *)buf + total, tmp, (size_t)n) != 0)
            return -ABI_EFAULT;
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
    if (!user_buf_ok((uint64_t)(uintptr_t)out,
                     (size_t)cap * sizeof(*out))) return -ABI_EFAULT;
    char kpath[VFS_PATH_MAX];
    if (!user_str_in(kpath, sizeof(kpath), path)) return -ABI_EFAULT;

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
        if (copy_to_user(&out[written], &u, sizeof(u)) != 0) {
            vfs_closedir(&d);
            return -ABI_EFAULT;
        }
        written++;
    }
    vfs_closedir(&d);
    return written;
}

static long sys_fs_readfile(const char *path, void *out, size_t cap) {
    if (cap == 0) return 0;
    if (cap > SYS_MAX_RW) cap = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, cap)) return -ABI_EFAULT;
    char kpath[VFS_PATH_MAX];
    if (!user_str_in(kpath, sizeof(kpath), path)) return -ABI_EFAULT;

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
        if (copy_to_user((char *)out + total, tmp, (size_t)n) != 0) {
            vfs_close(&f);
            return -ABI_EFAULT;
        }
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
    char kpath[VFS_PATH_MAX];
    if (!user_str_in(kpath, sizeof(kpath), path)) return -ABI_EFAULT;

    char karg[128];
    const char *karg_ptr = 0;
    if (arg) {
        if (!user_str_in(karg, sizeof(karg), arg)) return -ABI_EFAULT;
        karg_ptr = karg;
    }
    return gui_launch_enqueue_arg(kpath, karg_ptr);
}

/* ---- settings + session syscalls (milestone 14) -------------------- */

static long sys_setting_get(const char *key, char *out, size_t cap) {
    if (cap == 0) return 0;
    if (cap > 1024) cap = 1024;
    char kkey[SETTING_KEY_MAX];
    if (!user_str_in(kkey, sizeof(kkey), key)) return -ABI_EFAULT;

    /* Stage in a kernel buffer so the user can't observe a half-
     * written value. Then copy out atomically. */
    char tmp[SETTING_VAL_MAX];
    size_t n = settings_get_str(kkey, tmp, sizeof(tmp), "");
    if (n + 1 > cap) n = (cap > 0) ? (cap - 1) : 0;
    tmp[n] = '\0';
    if (copy_to_user(out, tmp, n + 1) != 0) return -ABI_EFAULT;
    return (long)n;
}

static long sys_setting_set(const char *key, const char *val) {
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE, "sys_setting_set"))
        return -1;
    char kkey[SETTING_KEY_MAX];
    char kval[SETTING_VAL_MAX];
    if (!user_str_in(kkey, sizeof(kkey), key)) return -ABI_EFAULT;
    if (!user_str_in(kval, sizeof(kval), val)) return -ABI_EFAULT;
    if (settings_set_str(kkey, kval) != 0) return -1;
    if (strcmp(kkey, "ui.theme") == 0) {
        theme_set(strcmp(kval, "basic") == 0 ? THEME_BASIC : THEME_CYBER);
    }
    gui_settings_changed(kkey, kval);
    /* Persist immediately so the new value survives a reboot even if
     * the calling app crashes before it gets a chance to save. */
    (void)settings_save();
    return 0;
}

static long sys_login(const char *username, const char *password) {
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE, "sys_login")) return -1;
    char kname[SESSION_USER_MAX];
    if (!user_str_in(kname, sizeof(kname), username)) return -ABI_EFAULT;
    char kpass[65];
    kpass[0] = '\0';
    if (password) {
        if (strncpy_from_user(kpass, password, sizeof(kpass)) < 0)
            kpass[0] = '\0';     /* bad pointer == empty password (old shape) */
    }
    return session_login(kname, kpass) == 0 ? 0 : -1;
}

static long sys_logout(void) {
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE, "sys_logout")) return -1;
    return session_logout() == 0 ? 0 : -1;
}

static long sys_session_info(char *out, size_t cap) {
    if (cap == 0) return 0;
    if (cap > 1024) cap = 1024;

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
    buf[i] = '\0';
    if (copy_to_user(out, buf, i + 1) != 0) return -ABI_EFAULT;
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
    char tmp[64];
    size_t n = 0;
    if (u) {
        while (u->name[n] && n + 1 < cap && n + 1 < sizeof(tmp)) {
            tmp[n] = u->name[n];
            n++;
        }
    }
    tmp[n] = '\0';
    if (copy_to_user(out, tmp, n + 1) != 0) return -ABI_EFAULT;
    return (long)n;
}

static long sys_chmod(const char *path, uint32_t mode) {
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE, "sys_chmod")) return -1;
    char kpath[VFS_PATH_MAX];
    if (!user_str_in(kpath, sizeof(kpath), path)) return -ABI_EFAULT;
    return vfs_chmod(kpath, mode);
}

static long sys_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE, "sys_chown")) return -1;
    char kpath[VFS_PATH_MAX];
    if (!user_str_in(kpath, sizeof(kpath), path)) return -ABI_EFAULT;
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

/* ---- scheduling priority ---------------------------------------- *
 *
 * setpriority(pid, prio): pid<=0 targets the caller. Policy: a proc may only
 * renice itself or another proc owned by the same uid; only root (uid 0) may
 * RAISE a proc above NORMAL (so an unprivileged task can't grab RT/HIGH and
 * starve the desktop -- aging bounds the damage even then, but the gate keeps
 * the contract Windows-like). Returns the applied priority, ABI_PRIO_NONE if
 * the target doesn't exist, or -ABI_EPERM on a policy violation. */
static long sys_setpriority(int pid, int prio) {
    struct proc *me = current_proc();
    if (pid <= 0) pid = me ? me->pid : 0;

    struct proc *tgt = proc_lookup(pid);
    if (!tgt) return ABI_PRIO_NONE;

    bool is_root = me && me->uid == 0;
    if (!is_root) {
        if (!me || tgt->uid != me->uid) return -ABI_EPERM;   /* not your proc */
        if (prio > ABI_PRIO_NORMAL)     return -ABI_EPERM;   /* no self-elevation */
    }
    return sched_set_prio(pid, prio);
}

static long sys_getpriority(int pid) {
    struct proc *me = current_proc();
    if (pid <= 0) pid = me ? me->pid : 0;
    return sched_get_prio(pid);
}

/* Copy a user-provided path into the kernel and resolve it against the
 * calling proc's cwd. The result lives in `out` (caller-owned buffer of
 * size `cap`). Returns 0 on success, -ABI_E* on failure. Absolute paths
 * copy verbatim; relative paths get prefixed with cwd + '/'. The user
 * pointer is consumed HERE (per-copy uaccess) -- callers never touch it. */
static int resolve_user_path(const char *user_path, char *out, size_t cap) {
    char up[ABI_PATH_MAX];
    long plen = strncpy_from_user(up, user_path, sizeof(up));
    if (plen < 0) return -ABI_EFAULT;
    if ((size_t)plen >= sizeof(up) - 1) return -ABI_ENAMETOOLONG;
    if (plen == 0 || cap == 0) return -ABI_EINVAL;
    /* Absolute? */
    if (up[0] == '/') {
        if ((size_t)plen + 1 > cap) return -ABI_ENAMETOOLONG;
        memcpy(out, up, (size_t)plen);
        out[plen] = '\0';
        return 0;
    }
    struct proc *p = current_proc();
    const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
    size_t clen = strlen(cwd);
    /* Need cwd + '/' + path + NUL, but skip the slash if cwd already
     * ends with one (e.g. cwd == "/"). */
    bool need_slash = (clen == 0 || cwd[clen - 1] != '/');
    size_t need = clen + (need_slash ? 1 : 0) + (size_t)plen + 1;
    if (need > cap) return -ABI_ENAMETOOLONG;
    memcpy(out, cwd, clen);
    if (need_slash) out[clen++] = '/';
    memcpy(out + clen, up, (size_t)plen);
    out[clen + plen] = '\0';
    return 0;
}

/* ---- file open / close / dup ----------------------------------- */

static long sys_open(const char *path, int flags, int mode) {
    (void)mode;     /* M25A: permissions on creation not honoured yet */
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, kpath, sizeof(kpath));
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
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, kpath, sizeof(kpath));
    if (rr) return rr;
    struct vfs_stat vs;
    int sr = vfs_stat(kpath, &vs);
    if (sr == VFS_ERR_NOENT) return -ABI_ENOENT;
    if (sr != VFS_OK)        return -ABI_EACCES;

    struct abi_stat tmp;
    fill_abi_stat(&vs, &tmp);
    if (copy_to_user(out, &tmp, sizeof(tmp)) != 0) return -ABI_EFAULT;
    return 0;
}

static long sys_fstat(int fd, struct abi_stat *out) {
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
    if (copy_to_user(out, &tmp, sizeof(tmp)) != 0) return -ABI_EFAULT;
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
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, kpath, sizeof(kpath));
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
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, kpath, sizeof(kpath));
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
    struct proc *p = current_proc();
    const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
    size_t n = strlen(cwd);
    if (n + 1 > cap) return -ABI_ERANGE;
    if (copy_to_user(out, cwd, n + 1) != 0) return -ABI_EFAULT;
    return (long)n;
}

static long sys_chdir(const char *path) {
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, kpath, sizeof(kpath));
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
    char kname[256];
    if (strncpy_from_user(kname, name, sizeof(kname)) < 0) return -ABI_EFAULT;
    if (cap > 0) {
        char nul = '\0';
        if (copy_to_user(out, &nul, 1) != 0) return -ABI_EFAULT;
    }
    return 0;
}

/* ---- time ------------------------------------------------------ */

static long sys_nanosleep(uint64_t ns) {
    /* Resolution is set by the timer tick (~10 ms on QEMU) -- good enough
     * for the uses libc has in M25A (sleep, usleep).
     *
     * SMP: drop the BKL across the wait. The old spin-yield held it for the
     * WHOLE sleep whenever this CPU's queue was empty (sched_yield's fast
     * path returns without releasing), so a sleeping proc on an AP blocked
     * every other core's syscalls for its full sleep duration. A sleeping
     * proc touches no shared kernel state, so the lock isn't needed; we
     * re-take it before returning to the dispatch epilogue. Same pattern as
     * tcp_poll_until. sched_yield still runs other ready work on this CPU;
     * with nothing ready we genuinely idle in hlt until the next IRQ. */
    uint64_t end = perf_now_ns() + ns;
    bool had_bkl = bkl_held();
    if (had_bkl) bkl_exit();
    while (perf_now_ns() < end) {
        sched_yield();
        if (perf_now_ns() >= end) break;
        sti();
        hlt();
    }
    if (had_bkl) bkl_enter();
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
    if (max_strlen <= 1 || max_strlen > 4096) return -ABI_EINVAL;

    /* Count first: read each user pointer slot through the accessor. */
    int n = 0;
    for (;;) {
        uint64_t slot = 0;
        if (get_user_u64(&slot, user_arr + n) != 0) return -ABI_EFAULT;
        if (slot == 0) break;
        if (n >= max_entries) return -ABI_E2BIG;
        n++;
    }
    if (n == 0) return 0;

    char **arr = (char **)kmalloc(sizeof(char *) * (size_t)(n + 1));
    if (!arr) return -ABI_ENOMEM;
    memset(arr, 0, sizeof(char *) * (size_t)(n + 1));

    for (int i = 0; i < n; i++) {
        uint64_t slot = 0;
        if (get_user_u64(&slot, user_arr + i) != 0) goto fail;
        char *kc = (char *)kmalloc((size_t)max_strlen);
        if (!kc) goto fail;
        long sl = strncpy_from_user(kc, (const void *)(uintptr_t)slot,
                                    (size_t)max_strlen);
        if (sl < 0 || sl >= max_strlen - 1) { kfree(kc); goto fail; }
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

    /* Snapshot the request into the kernel up front so a concurrent
     * user mutation can't change pointers we already validated. */
    struct abi_spawn_req kreq;
    if (copy_from_user(&kreq, req, sizeof(kreq)) != 0) return -ABI_EFAULT;
    if (kreq.flags != 0) return -ABI_EINVAL;

    char kpath[ABI_PATH_MAX];
    if (!user_str_in(kpath, sizeof(kpath), kreq.path)) return -ABI_EFAULT;

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

    if ((base & (PAGE_SIZE - 1)) != 0) return -ABI_EINVAL;
    if (base == 0 || base >= 0x0000800000000000ULL) return -ABI_EINVAL;

    /* Resolve under the caller's sandbox (so dload can't be used to
     * pull arbitrary files outside the per-session FS root). */
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, kpath, sizeof(kpath));
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
    if (copy_to_user(out_user, &kout, sizeof(kout)) != 0) return -ABI_EFAULT;
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
    if (n > 0 && copy_to_user(out, staging, sizeof(*out) * (size_t)n) != 0)
        return -ABI_EFAULT;
    return n;
}

static long sys_dev_test(const char *name, char *msg, uint32_t cap) {
    if (cap > ABI_DEVT_MSG_MAX) cap = ABI_DEVT_MSG_MAX;

    /* Copy the name into kernel memory (devtest_run does strcmp). */
    char kname[ABI_DEVT_NAME_MAX];
    if (strncpy_from_user(kname, name, sizeof(kname)) < 0) return -ABI_EFAULT;

    char kmsg[ABI_DEVT_MSG_MAX];
    int rc = devtest_run(kname, kmsg, sizeof kmsg);
    if (cap > 0 && msg) {
        size_t n = strlen(kmsg);
        if (n + 1 > cap) n = cap - 1;
        kmsg[n] = '\0';
        if (copy_to_user(msg, kmsg, n + 1) != 0) return -ABI_EFAULT;
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
    if (n > 0 && copy_to_user(out, staging, sizeof(*out) * (size_t)n) != 0)
        return -ABI_EFAULT;
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
    if (copy_to_user(out, &staging, sizeof(staging)) != 0) return -ABI_EFAULT;
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
    if (n > 0 && copy_to_user(out, staging, sizeof(*out) * (size_t)n) != 0)
        return -ABI_EFAULT;
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
    if (n > 0 && copy_to_user(out, staging, sizeof(*out) * (size_t)n) != 0)
        return -ABI_EFAULT;
    return (long)n;
}

static long sys_slog_write(uint32_t level, const char *sub_user,
                           const char *msg_user) {
    if (level >= ABI_SLOG_LEVEL_MAX) return -ABI_EINVAL;

    /* Copy strings into kernel memory so the ring writer never
     * dereferences user pointers. */
    char ksub[ABI_SLOG_SUB_MAX];
    char kmsg[ABI_SLOG_MSG_MAX];
    if (strncpy_from_user(ksub, sub_user, sizeof(ksub)) < 0) return -ABI_EFAULT;
    if (strncpy_from_user(kmsg, msg_user, sizeof(kmsg)) < 0) return -ABI_EFAULT;

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
    if (copy_to_user(out, &staging, sizeof(staging)) != 0) return -ABI_EFAULT;
    return 0;
}

/* ---- Milestone 36B: live system monitor snapshot ---------------- */

static long sys_system_metrics(struct abi_system_metrics *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out)))
        return -ABI_EFAULT;
    struct abi_system_metrics staging;
    sysmon_sample(&staging);
    if (copy_to_user(out, &staging, sizeof(staging)) != 0) return -ABI_EFAULT;
    return 0;
}

/* ---- Milestone 28C: watchdog status ------------------------- */

static long sys_wdog_status(struct abi_wdog_status *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out)))
        return -ABI_EFAULT;
    struct abi_wdog_status staging;
    wdog_status(&staging);
    if (copy_to_user(out, &staging, sizeof(staging)) != 0) return -ABI_EFAULT;
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
    /* Stage path into kernel space so the iteration callback sees a
     * stable, NUL-terminated buffer regardless of user paging. */
    char kpath[ABI_FSCHECK_PATH_MAX];
    long plen = strncpy_from_user(kpath, path, sizeof(kpath));
    if (plen < 0) return -ABI_EFAULT;
    if (plen == 0 || (size_t)plen >= sizeof(kpath) - 1) return -ABI_EINVAL;

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
        (void)copy_to_user(out, &staging, sizeof(staging));
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
            (void)copy_to_user(out, &staging, sizeof(staging));
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
        if (copy_to_user(out, &staging, sizeof(staging)) != 0)
            return -ABI_EFAULT;
        return 0;
    }

    /* Non-tobyfs mount (e.g. ramfs, fat32, /dev). M28E only verifies
     * tobyfs structurally; for everything else we just say "OK, type
     * unsupported by full check". */
    const char *msg = "no structural check available for this fs type";
    for (uint32_t i = 0; i < sizeof(staging.detail) - 1 && msg[i]; i++)
        staging.detail[i] = msg[i];
    staging.status = ABI_FSCHECK_OK;
    if (copy_to_user(out, &staging, sizeof(staging)) != 0) return -ABI_EFAULT;
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

    if (copy_to_user(out, &r, sizeof(r)) != 0) return -ABI_EFAULT;
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
    if (copy_to_user(out, &snap, sizeof(snap)) != 0) return -ABI_EFAULT;
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
    if (copy_to_user(out, &rec, sizeof(rec)) != 0) return -ABI_EFAULT;
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
    if (n > 0 && copy_to_user(out, staging, n * sizeof(staging[0])) != 0)
        return -ABI_EFAULT;
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
    if (copy_from_user(&staging, user_rec, sizeof(staging)) != 0)
        return -ABI_EFAULT;

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
    if (n > 0 && copy_to_user(out, staging, (size_t)n * sizeof(staging[0])) != 0)
        return -ABI_EFAULT;
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
    if (n > 0 && copy_to_user(out, staging, n * sizeof(staging[0])) != 0)
        return -ABI_EFAULT;
    return (long)n;
}

static long sys_waitpid(int pid, int *status_out, int flags) {

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
    if (status_out && put_user_u32(status_out, (uint32_t)code) != 0)
        return -ABI_EFAULT;
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
        return sys_login((const char *)a1, (const char *)a2);
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
    case ABI_SYS_SETPRIORITY: return sys_setpriority((int)a1, (int)a2);
    case ABI_SYS_GETPRIORITY: return sys_getpriority((int)a1);
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

    /* ---- Milestone 36B: desktop/system monitor ------------------ */
    case ABI_SYS_SYSTEM_METRICS:
        return sys_system_metrics((struct abi_system_metrics *)a1);

    /* ---- Milestone 38: window management ------------------------ */
    case ABI_SYS_GUI_SET_STATE:
        return sys_gui_set_state((int)a1, (int)a2);
    case ABI_SYS_GUI_SET_TITLE:
        return sys_gui_set_title((int)a1, (const char *)a2);

    /* ---- Clipboard ------------------------------------------------- */
    case ABI_SYS_CLIP_COPY:  return sys_clip_copy((const char *)a1, (uint32_t)a2);
    case ABI_SYS_CLIP_PASTE: return sys_clip_paste((char *)a1, (uint32_t)a2);

    /* ---- Network: HTTP GET ------------------------------------------ */
    case ABI_SYS_HTTP_GET:
        return sys_http_get((const char *)a1, (void *)a2, (uint32_t)a3);

    /* ---- Advanced GUI drawing (Phase 1) ----------------------------- */
    case ABI_SYS_GUI_LINE: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int x0 = (int)(int16_t)(a2 & 0xFFFF), y0 = (int)(int16_t)(a2 >> 16);
        int x1 = (int)(int16_t)(a3 & 0xFFFF), y1 = (int)(int16_t)(a3 >> 16);
        return gui_window_line(f->win, x0, y0, x1, y1, (uint32_t)a4);
    }
    case ABI_SYS_GUI_RECT: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int w = (int)(int16_t)(a4 & 0xFFFF), h = (int)(int16_t)(a4 >> 16);
        return gui_window_rect(f->win, (int)a2, (int)a3, w, h, (uint32_t)a5);
    }
    case ABI_SYS_GUI_ROUNDED_RECT: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int w = (int)(int16_t)(a3 & 0xFFFF), h = (int)(int16_t)(a3 >> 16);
        int radius = (int)(a5 >> 24);
        uint32_t color = a5 & 0x00FFFFFFu;
        return gui_window_rounded_rect(f->win, (int)a2, (int)(a2 >> 32), w, h, radius, color);
    }
    case ABI_SYS_GUI_ROUNDED_RECT_BLEND: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int w = (int)(int16_t)(a3 & 0xFFFF), h = (int)(int16_t)(a3 >> 16);
        return gui_window_rounded_rect_blend(f->win, (int)a2, (int)(a3 >> 32), w, h, (int)a4, (uint32_t)a5);
    }
    case ABI_SYS_GUI_CIRCLE: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int cx = (int)(int16_t)(a2 & 0xFFFF), cy = (int)(int16_t)(a2 >> 16);
        return gui_window_circle(f->win, cx, cy, (int)a3, (uint32_t)a4);
    }
    case ABI_SYS_GUI_CIRCLE_OUTLINE: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int cx = (int)(int16_t)(a2 & 0xFFFF), cy = (int)(int16_t)(a2 >> 16);
        return gui_window_circle_outline(f->win, cx, cy, (int)a3, (uint32_t)a4);
    }
    case ABI_SYS_GUI_BLIT: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int dx = (int)(int16_t)(a2 & 0xFFFF), dy = (int)(int16_t)(a2 >> 16);
        int w = (int)(int16_t)(a4 & 0xFFFF), h = (int)(int16_t)(a4 >> 16);
        return gui_window_blit(f->win, dx, dy, w, h, (const uint32_t *)a3);
    }
    case ABI_SYS_GUI_BLIT_BLEND: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int dx = (int)(int16_t)(a2 & 0xFFFF), dy = (int)(int16_t)(a2 >> 16);
        int w = (int)(int16_t)(a4 & 0xFFFF), h = (int)(int16_t)(a4 >> 16);
        return gui_window_blit_blend(f->win, dx, dy, w, h, (const uint32_t *)a3);
    }
    case ABI_SYS_GUI_GETPIXELS: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int sx = (int)(int16_t)(a2 & 0xFFFF), sy = (int)(int16_t)(a2 >> 16);
        int w = (int)(int16_t)(a4 & 0xFFFF), h = (int)(int16_t)(a4 >> 16);
        return gui_window_getpixels(f->win, sx, sy, w, h, (uint32_t *)a3);
    }
    case ABI_SYS_GUI_GRADIENT: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int w = (int)(int16_t)(a3 & 0xFFFF), h = (int)(int16_t)(a3 >> 16);
        return gui_window_gradient(f->win, (int)a2, (int)(a2 >> 32), w, h, (uint32_t)a4, (uint32_t)a5);
    }
    case ABI_SYS_GUI_LINE_BLEND: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int x0 = (int)(int16_t)(a2 & 0xFFFF), y0 = (int)(int16_t)(a2 >> 16);
        int x1 = (int)(int16_t)(a3 & 0xFFFF), y1 = (int)(int16_t)(a3 >> 16);
        return gui_window_line_blend(f->win, x0, y0, x1, y1, (uint32_t)a4);
    }

    /* ---- Advanced compositor (Phase 2) ------------------------------ */
    case ABI_SYS_GUI_SET_OPACITY: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        return gui_window_set_opacity(f->win, (uint8_t)(a2 & 0xFF));
    }
    case ABI_SYS_GUI_WAIT_VSYNC: {
        uint64_t start_frame = gfx_frame_count();
        uint64_t deadline = perf_now_ns() + 20000000ull; /* 20ms timeout (~50Hz min) */
        while (gfx_frame_count() == start_frame && perf_now_ns() < deadline) {
            sched_yield();
        }
        return (long)(gfx_frame_count() - start_frame);
    }
    case ABI_SYS_GUI_BATCH_FILL: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int count = (int)a3;
        if (count <= 0 || count > 256) return -ABI_EINVAL;
        uint32_t *cmds = (uint32_t *)bounce_in((const void *)a2,
                                               (size_t)count * 16);
        if (!cmds) return -ABI_EFAULT;
        for (int i = 0; i < count; i++) {
            int16_t bx = (int16_t)(cmds[i * 4 + 0] & 0xFFFF);
            int16_t by = (int16_t)((cmds[i * 4 + 0] >> 16) & 0xFFFF);
            uint16_t bw = (uint16_t)(cmds[i * 4 + 1] & 0xFFFF);
            uint16_t bh = (uint16_t)((cmds[i * 4 + 1] >> 16) & 0xFFFF);
            uint32_t color = cmds[i * 4 + 2];
            gui_window_fill(f->win, bx, by, bw, bh, color);
        }
        kfree(cmds);
        return count;
    }

    /* ---- 3D Graphics / VirGL (Phase 3) ------------------------------ */
    case ABI_SYS_GL_CREATE_CTX: {
        static uint32_t next_ctx = 1;
        uint32_t id = next_ctx++;
        int rc = virtio_gpu_ctx_create(id, "userland");
        return rc == 0 ? (long)id : -1;
    }
    case ABI_SYS_GL_DESTROY_CTX:
        virtio_gpu_ctx_destroy((uint32_t)a1);
        return 0;
    case ABI_SYS_GL_CREATE_BUFFER:
        /* Buffer creation is a subset of context resource management.
         * For now, return -1 (not yet implemented beyond context). */
        (void)a1; (void)a2;
        return -1;
    case ABI_SYS_GL_SUBMIT: {
        uint32_t len = (uint32_t)a3;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *k = bounce_in((const void *)a2, len);
        if (!k) return -ABI_EFAULT;
        long rv = (long)virtio_gpu_submit_3d((uint32_t)a1, k, len);
        kfree(k);
        return rv;
    }
    case ABI_SYS_GL_SWAP_BUFFERS: {
        struct file *f = fd_lookup((int)a2);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        gui_window_flip(f->win);
        return 0;
    }

    /* ---- Phase 1 M1.1: Threading syscalls ---- */

    case ABI_SYS_THREAD_CREATE:
        return thread_create((uint64_t)a1, (uint64_t)a2,
                             (uint64_t)a3, (uint64_t)a4);

    case ABI_SYS_THREAD_EXIT:
        thread_exit((int)a1);
        return 0; /* unreachable */

    case ABI_SYS_THREAD_JOIN: {
        int code = 0;
        long rv = thread_join((int)a1, a2 ? &code : 0);
        if (rv == 0 && a2 && put_user_u32((void *)a2, (uint32_t)code) != 0)
            return -ABI_EFAULT;
        return rv;
    }

    case ABI_SYS_THREAD_DETACH:
        return thread_detach((int)a1);

    case ABI_SYS_FUTEX:
        return futex((uint32_t *)(uintptr_t)a1, (int)a2, (uint32_t)a3);

    case ABI_SYS_SET_TLS:
        thread_set_tls((uint64_t)a1);
        return 0;

    case ABI_SYS_GETTID:
        return current_proc() ? current_proc()->pid : -1;

    /* ---- Phase 1 M1.2: Advanced VMM syscalls ---- */

    case ABI_SYS_MMAP:
        return sys_mmap((uint64_t)a1, (uint64_t)a2, (uint32_t)a3,
                        (uint32_t)a4, (int)a5, 0);

    case ABI_SYS_MUNMAP:
        return sys_munmap((uint64_t)a1, (uint64_t)a2);

    case ABI_SYS_MPROTECT:
        return sys_mprotect((uint64_t)a1, (uint64_t)a2, (uint32_t)a3);

    /* ---- Phase 1 M1.3: Signal syscalls ---- */

    case ABI_SYS_SIGACTION:
        return sys_sigaction((int)a1, (const void *)(uintptr_t)a2,
                            (void *)(uintptr_t)a3);

    case ABI_SYS_SIGPROCMASK:
        return sys_sigprocmask((int)a1, (const void *)(uintptr_t)a2,
                              (void *)(uintptr_t)a3);

    case ABI_SYS_SIGRETURN:
        return sys_sigreturn();

    case ABI_SYS_SIGRESTORER:
        sys_sigrestorer((uint64_t)a1);
        return 0;

    case ABI_SYS_KILL:
        return sys_kill((int)a1, (int)a2);

    /* ---- Phase 1 M1.4: IPC syscalls ---- */

    case ABI_SYS_SHM_OPEN: {
        char kname[128];
        if (!user_str_in(kname, sizeof(kname),
                         (const char *)(uintptr_t)a1)) return -ABI_EFAULT;
        return sys_shm_open(kname, (int)a2, (size_t)a3);
    }

    case ABI_SYS_SHM_MAP:
        return sys_shm_map((int)a1, (uint64_t)a2);

    case ABI_SYS_SHM_UNLINK: {
        char kname[128];
        if (!user_str_in(kname, sizeof(kname),
                         (const char *)(uintptr_t)a1)) return -ABI_EFAULT;
        return sys_shm_unlink(kname);
    }

    case ABI_SYS_UNIX_SOCKET:
        return sys_unix_socket();

    case ABI_SYS_UNIX_BIND: {
        char kpath[128];
        if (!user_str_in(kpath, sizeof(kpath),
                         (const char *)(uintptr_t)a2)) return -ABI_EFAULT;
        return sys_unix_bind((int)a1, kpath);
    }

    case ABI_SYS_UNIX_LISTEN:
        return sys_unix_listen((int)a1, (int)a2);

    case ABI_SYS_UNIX_CONNECT: {
        char kpath[128];
        if (!user_str_in(kpath, sizeof(kpath),
                         (const char *)(uintptr_t)a2)) return -ABI_EFAULT;
        return sys_unix_connect((int)a1, kpath);
    }

    case ABI_SYS_UNIX_ACCEPT:
        return sys_unix_accept((int)a1);

    case ABI_SYS_UNIX_SEND: {
        size_t len = (size_t)a3;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *k = bounce_in((const void *)(uintptr_t)a2, len);
        if (!k) return -ABI_EFAULT;
        long rv = sys_unix_send((int)a1, k, len);
        kfree(k);
        return rv;
    }

    case ABI_SYS_UNIX_RECV: {
        size_t len = (size_t)a3;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        if (!user_buf_ok((uint64_t)a2, len)) return -ABI_EFAULT;
        void *k = kmalloc(len ? len : 1);
        if (!k) return -ABI_ENOMEM;
        long rv = sys_unix_recv((int)a1, k, len);
        if (rv > 0 && copy_to_user((void *)(uintptr_t)a2, k, (size_t)rv) != 0)
            rv = -ABI_EFAULT;
        kfree(k);
        return rv;
    }

    case ABI_SYS_UNIX_CLOSE:
        return sys_unix_close((int)a1);

    /* ---- Phase 1 M1.5: Enhanced VFS ---- */

    case ABI_SYS_SYMLINK: {
        char kpath[ABI_PATH_MAX], ktarget[ABI_PATH_MAX];
        if (!user_str_in(kpath, sizeof(kpath),
                         (const char *)(uintptr_t)a1)) return -ABI_EFAULT;
        if (!user_str_in(ktarget, sizeof(ktarget),
                         (const char *)(uintptr_t)a2)) return -ABI_EFAULT;
        int rc = vfs_symlink(kpath, ktarget);
        if (rc == VFS_OK) return 0;
        if (rc == VFS_ERR_EXIST) return -ABI_EEXIST;
        if (rc == VFS_ERR_NOSPC) return -ABI_ENOSPC;
        return -ABI_EINVAL;
    }

    case ABI_SYS_READLINK: {
        char *buf        = (char *)(uintptr_t)a2;
        size_t bufsz     = (size_t)a3;
        char kpath[ABI_PATH_MAX], ktmp[ABI_PATH_MAX];
        if (!user_str_in(kpath, sizeof(kpath),
                         (const char *)(uintptr_t)a1)) return -ABI_EFAULT;
        if (!buf || bufsz == 0) return -ABI_EFAULT;
        if (bufsz > sizeof(ktmp)) bufsz = sizeof(ktmp);
        int rc = vfs_readlink(kpath, ktmp, bufsz);
        if (rc == VFS_OK) {
            size_t n = strlen(ktmp);
            if (copy_to_user(buf, ktmp, n + 1) != 0) return -ABI_EFAULT;
            return (long)n;
        }
        if (rc == VFS_ERR_NOENT) return -ABI_ENOENT;
        return -ABI_EINVAL;
    }

    case ABI_SYS_INOTIFY_INIT:
        return sys_inotify_init();

    case ABI_SYS_INOTIFY_ADD_WATCH: {
        char kpath[ABI_PATH_MAX];
        if (!user_str_in(kpath, sizeof(kpath),
                         (const char *)(uintptr_t)a2)) return -ABI_EFAULT;
        return sys_inotify_add_watch((int)a1, kpath, (uint32_t)a3);
    }

    case ABI_SYS_INOTIFY_RM_WATCH:
        return sys_inotify_rm_watch((int)a1, (int)a2);

    /* ---- Phase 2 M2.4: Fluent Design Theme Engine ------------------- */
    case ABI_SYS_THEME_SET:
        return theme_fluent_set((uint32_t)a1) == 0 ? 0 : -ABI_EINVAL;

    /* ---- Phase 2 M2.7: Clipboard System ----------------------------- */
    case ABI_SYS_CLIP_SET: {
        uint32_t len = (uint32_t)a2;
        uint32_t fmt = (uint32_t)a3;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *k = bounce_in((const void *)(uintptr_t)a1, len);
        if (!k) return -ABI_EFAULT;
        long rv = clipboard_set((const char *)k, len, fmt);
        kfree(k);
        return rv;
    }
    case ABI_SYS_CLIP_GET: {
        char *buf = (char *)(uintptr_t)a1;
        uint32_t buf_sz = (uint32_t)a2;
        uint32_t fmt = (uint32_t)a3;
        if (!buf || buf_sz == 0) return -ABI_EFAULT;
        if (buf_sz > SYS_MAX_RW) buf_sz = SYS_MAX_RW;
        char *k = (char *)kmalloc(buf_sz);
        if (!k) return -ABI_ENOMEM;
        long rv = clipboard_get(k, buf_sz, fmt);
        if (rv > 0 && copy_to_user(buf, k, (size_t)rv <= (size_t)buf_sz
                                            ? (size_t)rv : (size_t)buf_sz) != 0)
            rv = -ABI_EFAULT;
        kfree(k);
        return rv;
    }
    case ABI_SYS_CLIP_CLEAR:
        return clipboard_clear();

    /* ---- Phase 3 M3.2: Fork/Exec ----------------------------------- */
    case ABI_SYS_FORK:
        return sys_fork();

    case ABI_SYS_EXECVE:
        return sys_execve((const char *)(uintptr_t)a1,
                          (char *const *)(uintptr_t)a2,
                          (char *const *)(uintptr_t)a3);

    /* ---- Audio Engine ---- */
    case ABI_SYS_AUDIO_OPEN:
        return sys_audio_open((uint32_t)a1, (uint8_t)a2, (uint8_t)a3);
    case ABI_SYS_AUDIO_WRITE: {
        /* count is in SAMPLES; bound the byte size conservatively (max
         * 4 bytes/sample) and bounce. */
        size_t count = (size_t)a3;
        if (count > SYS_MAX_RW / 4) count = SYS_MAX_RW / 4;
        void *k = bounce_in((const void *)a2, count * 4);
        if (!k) return -ABI_EFAULT;
        long rv = sys_audio_write((int)a1, k, count);
        kfree(k);
        return rv;
    }
    case ABI_SYS_AUDIO_CLOSE:
        return sys_audio_close((int)a1);
    case ABI_SYS_AUDIO_VOLUME:
        return sys_audio_volume((int)a1, (uint8_t)a2);

    /* ---- Userland TCP/TLS networking ---- */
    case ABI_SYS_TCP_CONNECT:
        return sys_tcp_user_connect((uint32_t)a1, (uint16_t)a2, (uint32_t)a3);
    case ABI_SYS_TCP_SEND: {
        uint32_t len = (uint32_t)a3;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *k = bounce_in((const void *)a2, len);
        if (!k) return -ABI_EFAULT;
        long rv = sys_tcp_user_send((int)a1, k, len);
        kfree(k);
        return rv;
    }
    case ABI_SYS_TCP_RECV: {
        uint32_t len = (uint32_t)a3;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        if (!user_buf_ok((uint64_t)a2, len)) return -ABI_EFAULT;
        void *k = kmalloc(len ? len : 1);
        if (!k) return -ABI_ENOMEM;
        long rv = sys_tcp_user_recv((int)a1, k, len);
        if (rv > 0 && copy_to_user((void *)a2, k, (size_t)rv) != 0)
            rv = -ABI_EFAULT;
        kfree(k);
        return rv;
    }
    case ABI_SYS_TCP_CLOSE:
        return sys_tcp_user_close((int)a1);
    case ABI_SYS_TCP_LISTEN:
        return sys_tcp_user_listen((uint16_t)a1, (int)a2);
    case ABI_SYS_TCP_ACCEPT:
        return sys_tcp_user_accept((int)a1);
    case ABI_SYS_TLS_CONNECT: {
        char khost[256];
        khost[0] = '\0';
        if (a3 && strncpy_from_user(khost, (const char *)a3,
                                    sizeof(khost)) < 0) return -ABI_EFAULT;
        return sys_tls_user_connect((uint32_t)a1, (uint16_t)a2,
                                    a3 ? khost : 0);
    }
    case ABI_SYS_TLS_SEND: {
        uint32_t len = (uint32_t)a3;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *k = bounce_in((const void *)a2, len);
        if (!k) return -ABI_EFAULT;
        long rv = sys_tls_user_send((int)a1, k, len);
        kfree(k);
        return rv;
    }
    case ABI_SYS_TLS_RECV: {
        uint32_t len = (uint32_t)a3;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        if (!user_buf_ok((uint64_t)a2, len)) return -ABI_EFAULT;
        void *k = kmalloc(len ? len : 1);
        if (!k) return -ABI_ENOMEM;
        long rv = sys_tls_user_recv((int)a1, k, len);
        if (rv > 0 && copy_to_user((void *)a2, k, (size_t)rv) != 0)
            rv = -ABI_EFAULT;
        kfree(k);
        return rv;
    }
    case ABI_SYS_TLS_CLOSE:
        return sys_tls_user_close((int)a1);

    /* ---- Kernel module management --------------------------------------- */
    case ABI_SYS_MODULE:
        return sys_module(a1, a2, a3);

    /* ---- Milestone 7: notification service / IPC -------------------- */
    case ABI_SYS_NOTIFY_SVC_SEND: {
        char kt[128], kb[256], ki[64];
        kt[0] = kb[0] = ki[0] = '\0';
        if (a1 && strncpy_from_user(kt, (const char *)a1, sizeof(kt)) < 0)
            return -ABI_EFAULT;
        if (a2 && strncpy_from_user(kb, (const char *)a2, sizeof(kb)) < 0)
            return -ABI_EFAULT;
        if (a3 && strncpy_from_user(ki, (const char *)a3, sizeof(ki)) < 0)
            return -ABI_EFAULT;
        return sys_notify_svc_send(a1 ? kt : 0, a2 ? kb : 0, a3 ? ki : 0);
    }
    case ABI_SYS_NOTIFY_SVC_GET: {
        int maxc = (int)a2;
        if (maxc <= 0) return -ABI_EINVAL;
        if (maxc > 16) maxc = 16;
        struct notification kbuf[16];
        long n = sys_notify_svc_get(kbuf, maxc);
        if (n > 0 && copy_to_user((void *)a1, kbuf,
                                  (size_t)n * sizeof(kbuf[0])) != 0)
            return -ABI_EFAULT;
        return n;
    }
    case ABI_SYS_NOTIFY_SVC_DISMISS:
        return sys_notify_svc_dismiss((int)a1);

    default:
        kprintf("[syscall] unknown number %ld -- returning -ENOSYS\n", num);
        return -ABI_ENOSYS;
    }
}

/* ============================================================================
 * Track B (foreign-binary compat) -- Linux x86-64 personality, milestone B1.
 *
 * A process tagged ABI_PERS_LINUX (its ELF was branded ELFOSABI_LINUX) makes
 * `syscall` instructions using the *Linux* x86-64 ABI: Linux syscall numbers
 * in RAX, args in RDI/RSI/RDX/R10/R8/R9. The .S trampoline already preserves
 * that exact register convention (it was modelled on Linux), and the initial
 * user stack we build (argc/argv/envp/auxv with AT_PHDR/AT_ENTRY/AT_BASE...)
 * is already Linux-shaped -- so all that's left is to translate the syscall
 * NUMBERS + a few ABI-specific semantics onto tobyOS's existing primitives.
 *
 * This function is that translation layer. 1:1 calls are forwarded to the
 * native dispatcher with the tobyOS number; Linux-specific ones (arch_prctl,
 * writev, set_tid_address, exit_group, clock_gettime, uname, ...) are handled
 * inline. Anything not yet covered returns -ENOSYS with a log line naming the
 * number, so a real musl/busybox binary that hits a gap tells us exactly what
 * to implement next (milestone B2+).
 *
 * Scope note: this is the FIRST milestone. The set below is enough to run a
 * static Linux binary through libc-style startup (TLS via arch_prctl, the
 * stdio write path via writev) to exit. Signals, full mmap-backed malloc
 * churn, fstat struct translation, openat dir semantics, and the dynamic
 * loader path are explicitly deferred. */

/* Linux x86-64 syscall numbers (arch/x86/entry/syscalls/syscall_64.tbl). */
enum {
    LX_read = 0, LX_write = 1, LX_open = 2, LX_close = 3,
    LX_stat = 4, LX_fstat = 5, LX_lstat = 6, LX_lseek = 8, LX_mmap = 9,
    LX_mprotect = 10, LX_munmap = 11, LX_brk = 12,
    LX_rt_sigaction = 13, LX_rt_sigprocmask = 14, LX_ioctl = 16,
    LX_readv = 19, LX_writev = 20, LX_access = 21, LX_pipe = 22,
    LX_dup = 32, LX_dup2 = 33, LX_nanosleep = 35, LX_sendfile = 40,
    LX_getpid = 39, LX_exit = 60, LX_uname = 63, LX_fcntl = 72,
    LX_getcwd = 79, LX_chdir = 80, LX_mkdir = 83, LX_unlink = 87,
    LX_getuid = 102, LX_getgid = 104, LX_geteuid = 107, LX_getegid = 108,
    LX_getppid = 110, LX_arch_prctl = 158, LX_gettid = 186,
    LX_futex = 202, LX_set_tid_address = 218, LX_clock_gettime = 228,
    LX_exit_group = 231, LX_newfstatat = 262, LX_set_robust_list = 273,
    LX_getrandom = 318,
};

/* arch_prctl codes. */
#define LX_ARCH_SET_FS   0x1002
#define LX_ARCH_GET_FS   0x1003

/* Linux mmap flag bits (differ from tobyOS VMA_FLAG_*). */
#define LXMAP_SHARED     0x01
#define LXMAP_PRIVATE    0x02
#define LXMAP_FIXED      0x10
#define LXMAP_ANONYMOUS  0x20

struct lx_iovec   { uint64_t iov_base; uint64_t iov_len; };
struct lx_timespec{ int64_t  tv_sec;   int64_t  tv_nsec; };

/* Linux x86-64 struct stat (arch/x86/include/uapi/asm/stat.h) -- 144 bytes,
 * field layout is ABI and must match byte-for-byte. tobyOS's S_IF* type bits
 * (abi.h) already equal Linux's (S_IFDIR=0x4000 / S_IFREG=0x8000), so the
 * mode just carries through. */
struct lx_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;  int64_t st_atime_nsec;
    int64_t  st_mtime;  int64_t st_mtime_nsec;
    int64_t  st_ctime;  int64_t st_ctime_nsec;
    int64_t  __unused3[3];
};

/* Build a Linux struct stat from a tobyOS vfs_stat and copy it out. */
static long linux_emit_stat(const struct vfs_stat *vs, void *ubuf) {
    struct lx_stat st;
    memset(&st, 0, sizeof st);
    uint32_t typ  = (vs->type == VFS_TYPE_DIR) ? 0x4000u : 0x8000u; /* S_IFDIR/REG */
    uint32_t perm = vs->mode & 0xFFFu;
    if (perm == 0) perm = (vs->type == VFS_TYPE_DIR) ? 0755u : 0644u;
    st.st_mode    = typ | perm;
    st.st_ino     = 1;                 /* synthetic; tobyOS VFS has no stable ino here */
    st.st_nlink   = 1;
    st.st_uid     = vs->uid;
    st.st_gid     = vs->gid;
    st.st_size    = (int64_t)vs->size;
    st.st_blksize = 512;
    st.st_blocks  = (int64_t)((vs->size + 511) / 512);
    if (copy_to_user(ubuf, &st, sizeof st) != 0) return -ABI_EFAULT;
    return 0;
}

/* Translate a Linux mmap flags word into the tobyOS VMA flag word that
 * sys_mmap (src/mmap.c) expects. Defaults to a private anonymous mapping
 * (the malloc case) when neither shared nor private is asked for. */
static uint32_t lx_mmap_flags(uint32_t lf) {
    uint32_t tf = 0;
    if (lf & LXMAP_ANONYMOUS) tf |= 0x01;   /* VMA_FLAG_ANON    */
    if (lf & LXMAP_SHARED)    tf |= 0x02;   /* VMA_FLAG_SHARED  */
    if (lf & LXMAP_PRIVATE)   tf |= 0x04;   /* VMA_FLAG_PRIVATE */
    if (lf & LXMAP_FIXED)     tf |= 0x08;   /* VMA_FLAG_FIXED   */
    return tf;
}

static long linux_syscall(long n, long a1, long a2, long a3, long a4, long a5) {
    switch (n) {
    /* ---- exits ---- */
    case LX_exit:
    case LX_exit_group:
        sys_exit((int)a1);          /* noreturn */
        return 0;

    /* ---- plain byte I/O (tobyOS handlers already take user buffers) ---- */
    case LX_read:   return sys_read((int)a1, (void *)a2, (size_t)a3);
    case LX_write:  return sys_write((int)a1, (const void *)a2, (size_t)a3);
    case LX_close:  return do_syscall(SYS_CLOSE, a1, 0, 0, 0, 0);
    case LX_lseek:  return do_syscall(SYS_LSEEK, a1, a2, a3, 0, 0);
    case LX_open:   return do_syscall(SYS_OPEN, a1, a2, a3, 0, 0);
    case LX_dup:    return do_syscall(SYS_DUP, a1, 0, 0, 0, 0);
    case LX_pipe:   return do_syscall(SYS_PIPE, a1, 0, 0, 0, 0);
    case LX_getcwd: return do_syscall(SYS_GETCWD, a1, a2, 0, 0, 0);
    case LX_chdir:  return do_syscall(SYS_CHDIR, a1, 0, 0, 0, 0);
    case LX_mkdir:  return do_syscall(SYS_MKDIR, a1, a2, 0, 0, 0);
    case LX_unlink: return do_syscall(SYS_UNLINK, a1, 0, 0, 0, 0);

    /* ---- stat family: translate tobyOS vfs_stat -> Linux struct stat ---- */
    case LX_stat:
    case LX_lstat: {                   /* (path, struct stat*) */
        char kpath[ABI_PATH_MAX];
        int rr = resolve_user_path((const char *)a1, kpath, sizeof kpath);
        if (rr) return rr;
        struct vfs_stat vs;
        int sr = vfs_stat(kpath, &vs);
        if (sr == VFS_ERR_NOENT) return -ABI_ENOENT;
        if (sr != VFS_OK)        return -ABI_EACCES;
        return linux_emit_stat(&vs, (void *)a2);
    }
    case LX_newfstatat: {              /* (dirfd, path, struct stat*, flags) */
        /* We honour absolute paths and AT_FDCWD; AT_EMPTY_PATH (stat the
         * dirfd itself) is treated like fstat(dirfd). */
        const char *upath = (const char *)a2;
        char probe[2] = {0,0};
        if (upath) (void)strncpy_from_user(probe, upath, sizeof probe);
        if (!upath || probe[0] == '\0') {
            /* AT_EMPTY_PATH-style: fall through to fstat on a1. */
            struct file *f = fd_lookup((int)a1);
            if (!f) return -ABI_EBADF;
            struct vfs_stat vs = { .type = VFS_TYPE_FILE, .size = f->vfs.size,
                                   .uid = f->vfs.uid, .gid = f->vfs.gid,
                                   .mode = f->vfs.mode };
            if (f->kind != FILE_KIND_VFS) { vs.mode = 0666; vs.size = 0; }
            return linux_emit_stat(&vs, (void *)a3);
        }
        char kpath[ABI_PATH_MAX];
        int rr = resolve_user_path(upath, kpath, sizeof kpath);
        if (rr) return rr;
        struct vfs_stat vs;
        int sr = vfs_stat(kpath, &vs);
        if (sr == VFS_ERR_NOENT) return -ABI_ENOENT;
        if (sr != VFS_OK)        return -ABI_EACCES;
        return linux_emit_stat(&vs, (void *)a3);
    }
    case LX_fstat: {                   /* (fd, struct stat*) */
        struct file *f = fd_lookup((int)a1);
        if (!f) return -ABI_EBADF;
        struct vfs_stat vs;
        if (f->kind == FILE_KIND_VFS) {
            vs = (struct vfs_stat){ .type = VFS_TYPE_FILE, .size = f->vfs.size,
                                    .uid = f->vfs.uid, .gid = f->vfs.gid,
                                    .mode = f->vfs.mode };
        } else {
            /* console/pipe/socket: report a minimal regular-file stat. */
            vs = (struct vfs_stat){ .type = VFS_TYPE_FILE, .size = 0,
                                    .mode = 0666 };
        }
        return linux_emit_stat(&vs, (void *)a2);
    }

    /* ---- identities ---- */
    case LX_getpid:  return do_syscall(SYS_GETPID, 0, 0, 0, 0, 0);
    case LX_getppid: return do_syscall(SYS_GETPPID, 0, 0, 0, 0, 0);
    case LX_gettid:  return do_syscall(ABI_SYS_GETTID, 0, 0, 0, 0, 0);
    case LX_getuid:
    case LX_geteuid: return do_syscall(SYS_GETUID, 0, 0, 0, 0, 0);
    case LX_getgid:
    case LX_getegid: return do_syscall(SYS_GETGID, 0, 0, 0, 0, 0);

    /* ---- scatter/gather: fan out onto the byte handlers ---- */
    case LX_writev:
    case LX_readv: {
        int             fd  = (int)a1;
        const void     *uio = (const void *)a2;
        int             cnt = (int)a3;
        if (cnt < 0)        return -ABI_EINVAL;
        if (cnt > 1024)     cnt = 1024;          /* IOV_MAX-ish clamp */
        long total = 0;
        for (int i = 0; i < cnt; i++) {
            struct lx_iovec iov;
            if (copy_from_user(&iov, (const uint8_t *)uio + (size_t)i * 16,
                               sizeof iov) != 0)
                return -ABI_EFAULT;
            if (iov.iov_len == 0) continue;
            long r = (n == LX_writev)
                       ? sys_write(fd, (const void *)iov.iov_base,
                                   (size_t)iov.iov_len)
                       : sys_read(fd, (void *)iov.iov_base,
                                  (size_t)iov.iov_len);
            if (r < 0) return total ? total : r;
            total += r;
            if ((uint64_t)r < iov.iov_len) break;  /* short transfer */
        }
        return total;
    }

    /* ---- TLS setup: Linux libc startup calls arch_prctl(ARCH_SET_FS) ---- */
    case LX_arch_prctl:
        if ((unsigned long)a1 == LX_ARCH_SET_FS) {
            thread_set_tls((uint64_t)a2);
            return 0;
        }
        if ((unsigned long)a1 == LX_ARCH_GET_FS) {
            struct proc *p = current_proc();
            if (put_user_u64((void *)a2, p ? p->tls_base : 0) != 0)
                return -ABI_EFAULT;
            return 0;
        }
        return -ABI_EINVAL;

    /* libc records a thread-exit futex address here; we have nothing to
     * store for it but must return the tid (libc uses the return value). */
    case LX_set_tid_address: {
        struct proc *p = current_proc();
        return p ? p->pid : 0;
    }
    case LX_set_robust_list:
        return 0;

    /* stdio probes the tty geometry on first write; any error just means
     * "not a tty" -> fully buffered, which still flushes at exit. */
    case LX_ioctl:
        return -ABI_ENOTTY;

    /* ---- memory ---- */
    case LX_brk: {
        struct proc *p = current_proc();
        if (!p) return -ABI_EPERM;
        if (a1 == 0) return (long)p->brk_cur;        /* query */
        long r = sys_brk((uintptr_t)a1);
        return (r < 0) ? (long)p->brk_cur : r;       /* Linux: unchanged on fail */
    }
    case LX_mmap:
        return sys_mmap((uint64_t)a1, (uint64_t)a2, (uint32_t)a3,
                        lx_mmap_flags((uint32_t)a4), (int)a5, 0);
    case LX_munmap:
        return do_syscall(ABI_SYS_MUNMAP, a1, a2, 0, 0, 0);
    case LX_mprotect:
        return do_syscall(ABI_SYS_MPROTECT, a1, a2, a3, 0, 0);

    /* ---- time ---- */
    case LX_nanosleep: {
        struct lx_timespec ts;
        if (!a1) return -ABI_EINVAL;
        if (copy_from_user(&ts, (const void *)a1, sizeof ts) != 0)
            return -ABI_EFAULT;
        uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        return do_syscall(SYS_NANOSLEEP, (long)ns, 0, 0, 0, 0);
    }
    case LX_clock_gettime: {
        long ms = do_syscall(ABI_SYS_CLOCK_MS, 0, 0, 0, 0, 0);
        if (ms < 0) ms = 0;
        struct lx_timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
        if (a2 && copy_to_user((void *)a2, &ts, sizeof ts) != 0)
            return -ABI_EFAULT;
        return 0;
    }

    /* ---- misc ---- */
    case LX_uname: {
        /* struct utsname: 6 x 65-byte NUL-padded fields. */
        char u[6 * 65];
        memset(u, 0, sizeof u);
        const char *vals[6] = { "Linux", "tobyos", "5.0.0-tobyos",
                                "#1 tobyOS Linux-ABI", "x86_64", "(none)" };
        for (int i = 0; i < 6; i++) {
            size_t l = strlen(vals[i]);
            memcpy(u + i * 65, vals[i], l);
        }
        if (a1 && copy_to_user((void *)a1, u, sizeof u) != 0)
            return -ABI_EFAULT;
        return 0;
    }
    case LX_getrandom: {
        /* Best-effort: zero-fill (deterministic). Real entropy is B2. */
        size_t len = (size_t)a2;
        if (len > 256) len = 256;
        char z[256]; memset(z, 0, len);
        if (a1 && len && copy_to_user((void *)a1, z, len) != 0)
            return -ABI_EFAULT;
        return (long)len;
    }

    /* Signals: accepted but not yet wired to the native signal layer
     * (the Linux struct sigaction / sigset layouts differ -- B2). Return
     * success so libc startup that installs handlers doesn't abort. */
    case LX_rt_sigaction:
    case LX_rt_sigprocmask:
        return 0;

    /* Futex: forward only the FUTEX_WAIT/WAKE low ops; private flag and
     * timeouts are ignored for now (single-threaded statics don't block
     * here). */
    case LX_futex:
        return futex((uint32_t *)(uintptr_t)a1, (int)(a2 & 0x7f), (uint32_t)a3);

    case LX_access:
        return -ABI_ENOENT;     /* conservative: report "not there" */

    /* Known-optional: libc/busybox probe these and fall back cleanly on
     * -ENOSYS. Handled explicitly (quietly) so they don't spam the log. */
    case LX_sendfile:           /* cat/cp fall back to a read/write loop */
    case LX_fcntl:              /* F_SETFD/CLOEXEC etc -- best-effort no-op */
        return (n == LX_fcntl) ? 0 : -ABI_ENOSYS;

    default:
        kprintf("[linux] unhandled syscall %ld (a1=0x%lx a2=0x%lx) -> -ENOSYS "
                "(implement in linux_syscall, milestone B2)\n",
                n, (unsigned long)a1, (unsigned long)a2);
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

    /* Big Kernel Lock: serialize the whole syscall body across CPUs so user
     * code parallelizes while the kernel's coarse-locked subsystems stay safe.
     * The scheduler drops/reacquires the BKL around any blocking switch inside
     * the body, and proc_exit's switch releases it for a dying proc. Held with
     * IRQs on (we just sti'd); IRQ handlers don't take the BKL. */
    bkl_enter();

#ifdef SMP_DIAG
    /* DIAG: record the syscall this CPU is currently inside (by cpu_idx) so a
     * freeze capture can name the syscall whose body/return path is holding the
     * BKL. Cleared to -1 at the normal return below. */
    extern volatile long g_cpu_syscall[32];
    g_cpu_syscall[smp_current_cpu_idx() & 31] = num;
#endif

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

    /* Track B: a process branded ABI_PERS_LINUX speaks the Linux x86-64
     * syscall ABI -- route its `num` through the translation layer. The
     * native path (the common case) is completely unchanged. */
    long rv = (caller && caller->personality == ABI_PERS_LINUX)
                  ? linux_syscall(num, a1, a2, a3, a4, a5)
                  : do_syscall(num, a1, a2, a3, a4, a5);

    perf_syscall_exit((int)num, t_sys);
    perf_zone_end(PERF_Z_SYSCALL, t_sys);

    /* Drive GUI/net/input housekeeping from the syscall-return path ONLY on a
     * uniprocessor. On a single CPU this is essential: while a user app runs,
     * pid 0's idle loop never gets the CPU, so its compositor/network/input
     * ticks would stall until the app yields -- driving them here keeps the
     * desktop and NIC RX alive across long syscall bursts.
     *
     * Under SMP this is REMOVED. pid 0 runs concurrently on the BSP and drives
     * all of this from idle_loop, so doing it again on every AP syscall return
     * is redundant -- and harmful: gui_tick() internally sched_yield()s, so the
     * syscall (holding the BKL) drops + reacquires the BKL and switches procs
     * mid-return. That deeply-nested BKL juggling on every syscall, combined
     * with the heavy work holding the lock, was the source of a rare full-
     * desktop freeze (the BSP couldn't make progress while APs churned the
     * lock through this path). Letting pid 0 be the sole driver keeps the BKL
     * critical sections short and the nesting flat. */
    if (gui_active() && smp_online_count() <= 1) {
        net_service_tick();
        syscall_service_input();
        gui_tick();
        net_service_tick();
        syscall_service_input();
    }

    /* Safe point #1: every syscall return runs through here. If the
     * kernel (or another proc, or the keyboard IRQ) sent us a signal
     * during the body, deliver it now -- for a caught signal this pushes a
     * handler frame and rewrites our trapframe so the SYSRETQ below lands in
     * the handler; for a fatal default it proc_exit()s and never returns.
     * `rv` is preserved as the to-be-restored RAX for the handler case
     * (or swapped for a syscall restart when the action has SA_RESTART). */
    signal_deliver_syscall(rv, num);

    /* Leaving the kernel: drop the BKL so another core can enter. (If the
     * body proc_exit'd or a signal killed us, the scheduler already released
     * it and we never got here; bkl_exit is idempotent regardless.) */
    bkl_exit();

#ifdef SMP_DIAG
    g_cpu_syscall[smp_current_cpu_idx() & 31] = -1;   /* DIAG: syscall done */
#endif

    /* Re-mask interrupts before we return into the .S unwind. The
     * trampoline pops 14 registers and then does `mov rsp, [rsp]` to
     * jump back onto the user stack -- if an IRQ fired in that window
     * we'd execute the handler with a half-pop'd kernel stack and a
     * confused RSP. SYSRETQ will reload IF=1 from r11 (saved user
     * RFLAGS), so user mode runs with interrupts on as expected. */
    cli();
    return rv;
}

#ifdef SMP_DIAG
/* DIAG: per-cpu_idx current syscall number (-1 == not in a syscall). Read at a
 * freeze to identify which syscall is holding the BKL on the stuck core. */
volatile long g_cpu_syscall[32] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
#endif

/* Program the SYSCALL-related MSRs on the CURRENT CPU. EFER.SCE/STAR/LSTAR/
 * FMASK are all per-CPU, so every core that runs ring-3 code must set them or
 * the `syscall` instruction #UDs. Shared by syscall_init (BSP) and
 * syscall_init_ap (each AP). */
static void syscall_program_msrs(void) {
    uint64_t efer = rdmsr(IA32_EFER);
    wrmsr(IA32_EFER, efer | EFER_SCE);

    uint64_t star = ((uint64_t)GDT_KERNEL_CS << 32) |
                    ((uint64_t)0x10          << 48);
    wrmsr(IA32_STAR, star);

    wrmsr(IA32_LSTAR, (uint64_t)&syscall_entry);
    wrmsr(IA32_FMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF);
}

/* Per-AP SYSCALL setup: just the MSRs. The AP's GS base + syscall_rsp are set
 * in ap_entry. Without this, the first `syscall` a user proc runs on an AP
 * raises #UD. */
void syscall_init_ap(void) {
    syscall_program_msrs();
}

void syscall_init(void) {
    syscall_program_msrs();

    uint64_t star = ((uint64_t)GDT_KERNEL_CS << 32) |
                    ((uint64_t)0x10          << 48);

    /* Per-CPU SYSCALL stack via GS base. GS base points at this CPU's
     * struct percpu; syscall_entry.S reads gs:[0] (syscall_rsp) / gs:[8]
     * (scratch). tobyOS user code never uses the GS base, so we leave it
     * pointed at per-CPU data across ring transitions (no swapgs needed).
     * APs do the equivalent in ap_entry(). */
    struct percpu *pc = smp_this_cpu();
    pc->syscall_rsp = tss_kernel_rsp_top();
    wrmsr(IA32_GS_BASE, (uint64_t)pc);

    kprintf("[sys] EFER.SCE on, STAR=0x%016lx, LSTAR=%p, FMASK=0x%lx\n",
            star, (void *)&syscall_entry,
            (unsigned long)(RFLAGS_IF | RFLAGS_DF | RFLAGS_TF));
    kprintf("[sys] kernel syscall rsp = %p (gs base=%p)\n",
            (void *)pc->syscall_rsp, (void *)pc);
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
