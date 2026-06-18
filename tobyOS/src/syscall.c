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
#include <tobyos/pe.h>

extern void syscall_entry(void);

/* Phase 3 M3.2: fork/exec forward declarations */
extern long sys_fork(void);
extern long sys_execve(const char *path, char *const argv[], char *const envp[]);
/* Track B/B9: Linux clone(CLONE_VM) thread (shared address space). */
extern long sys_clone_thread(uint64_t flags, uint64_t stack, uint64_t ptid,
                             uint64_t ctid, uint64_t tls);

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
#define IA32_GS_BASE        0xC0000101u   /* active GS base (per-CPU data ptr) */
#define IA32_KERNEL_GS_BASE 0xC0000102u   /* SWAPGS shadow: the CPL3 GS base.
                                           * Holds the current proc's user GS
                                           * (TEB for a Win32 PE, else &percpu --
                                           * an identity swap). syscall_entry.S /
                                           * isr_stubs.S SWAPGS it in/out at every
                                           * CPL3 boundary; do_switch keeps it in
                                           * sync with the running proc. */

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

    /* Strip a leading "." / "./" so a cwd-relative path resolves against
     * the cwd rather than producing a bogus "/." -- e.g. `busybox ls .`
     * (no path arg -> opendir(".")). A bare "." becomes the cwd itself.
     * Only leading dot-components are collapsed (enough for the common
     * cases); interior "/./" and ".." are left alone. */
    char  *rel  = up;
    size_t rlen = (size_t)plen;
    while (rlen >= 1 && rel[0] == '.' && (rlen == 1 || rel[1] == '/')) {
        size_t skip = (rlen == 1) ? 1 : 2;
        rel += skip; rlen -= skip;
        while (rlen && rel[0] == '/') { rel++; rlen--; }
    }

    /* Need cwd + '/' + path + NUL, but skip the slash if cwd already ends
     * with one (e.g. cwd == "/") or there's no trailing component left. */
    bool need_slash = rlen > 0 && (clen == 0 || cwd[clen - 1] != '/');
    size_t need = clen + (need_slash ? 1 : 0) + rlen + 1;
    if (need > cap) return -ABI_ENAMETOOLONG;
    memcpy(out, cwd, clen);
    size_t o = clen;
    if (need_slash) out[o++] = '/';
    memcpy(out + o, rel, rlen);
    out[o + rlen] = '\0';
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
    LX_rt_sigaction = 13, LX_rt_sigprocmask = 14, LX_rt_sigreturn = 15,
    LX_ioctl = 16, LX_readv = 19, LX_writev = 20, LX_access = 21,
    LX_pipe = 22, LX_sched_yield = 24, LX_dup = 32, LX_dup2 = 33,
    LX_nanosleep = 35,
    LX_sendfile = 40, LX_getpid = 39, LX_clone = 56, LX_fork = 57,
    LX_vfork = 58, LX_execve = 59, LX_exit = 60, LX_wait4 = 61,
    LX_kill = 62, LX_setpgid = 109, LX_getpgrp = 111, LX_setsid = 112,
    LX_getpgid = 121,
    LX_uname = 63, LX_fcntl = 72, LX_getcwd = 79, LX_chdir = 80,
    LX_mkdir = 83, LX_unlink = 87, LX_getuid = 102, LX_getgid = 104,
    LX_geteuid = 107, LX_getegid = 108, LX_getppid = 110,
    LX_arch_prctl = 158, LX_gettid = 186, LX_tkill = 200, LX_futex = 202,
    LX_getdents64 = 217, LX_set_tid_address = 218, LX_clock_gettime = 228,
    LX_exit_group = 231, LX_tgkill = 234, LX_openat = 257,
    LX_newfstatat = 262, LX_set_robust_list = 273, LX_getrandom = 318,
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

/* Linux x86-64 `struct sigaction` as the rt_sigaction(2) syscall sees it:
 * sa_handler, then sa_flags, then the mandatory sa_restorer, then sa_mask.
 * This field ORDER differs from tobyOS's struct sigaction (handler, mask,
 * flags), so it must be translated by hand. The SA_* flag *values* are
 * already Linux-aligned in tobyOS (signal.h), so flags carry through. */
struct lx_sigaction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};
#define LX_SA_RESTORER 0x04000000u

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

/* The mmap offset is Linux mmap's 6th arg (user r9), which the dispatch
 * doesn't forward to C (only a1..a5). The .S stashed every user register
 * in the syscall_regs block at the top of this proc's kstack, so read it
 * back from there. */
static uint64_t lx_mmap_offset(void) {
    struct proc *p = current_proc();
    if (!p || !p->kstack_top) return 0;
    struct syscall_regs *r =
        (struct syscall_regs *)((uint8_t *)p->kstack_top - sizeof(*r));
    return r->r9;   /* user r9 == 6th syscall arg == mmap offset */
}

/* File-backed mmap (B6). The dynamic loader maps a shared library's
 * segments with mmap(fd, MAP_PRIVATE, offset). tobyOS's demand-paged
 * VMA_FILE fault path is a stub, and -- decisively -- musl closes the
 * library fd the instant mmap returns, so lazy by-fd paging is impossible.
 * We therefore map EAGERLY: reserve writable anonymous pages, read the
 * file content in at the requested offset, then tighten to `prot`. A short
 * read at EOF leaves the tail zeroed, which is exactly the file-hole / .bss
 * semantics the loader expects. */
static long linux_mmap_file(uint64_t addr, uint64_t len, uint32_t prot,
                            uint32_t lflags, int fd, uint64_t offset) {
    if (len == 0) return -ABI_EINVAL;
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_VFS) return -ABI_EBADF;

    /* Reserve + map writable anon pages so we can fill them. */
    uint32_t tflags = lx_mmap_flags(lflags) | 0x01u /* VMA_FLAG_ANON */;
    long base = sys_mmap(addr, len, 0x1u | 0x2u /* PROT_READ|WRITE */,
                         tflags, -1, 0);
    if (base < 0) return base;

    uint8_t *kbuf = (uint8_t *)kmalloc(4096);
    if (!kbuf) { sys_munmap((uint64_t)base, len); return -ABI_ENOMEM; }

    size_t save_pos = f->vfs.pos;
    f->vfs.pos = offset;
    uint64_t done = 0;
    while (done < len) {
        size_t want = (len - done) > 4096 ? 4096 : (size_t)(len - done);
        long got = file_read(f, kbuf, want);
        if (got <= 0) break;                  /* EOF -> leave tail zeroed */
        if (copy_to_user((void *)((uint64_t)base + done), kbuf,
                         (size_t)got) != 0) {
            kfree(kbuf); f->vfs.pos = save_pos;
            sys_munmap((uint64_t)base, len);
            return -ABI_EFAULT;
        }
        done += (uint64_t)got;
        if ((size_t)got < want) break;        /* short read == EOF */
    }
    kfree(kbuf);
    f->vfs.pos = save_pos;

    /* Tighten to the loader's requested protection (R-X for .text, etc.). */
    sys_mprotect((uint64_t)base, len, prot);
    return base;
}

/* Linux struct linux_dirent64 (getdents64). The byte layout is ABI; d_name
 * is oversized so any 8-aligned d_reclen we compute fits in this local copy. */
struct lx_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[VFS_NAME_MAX + 16];
} __attribute__((packed));
#define LX_DT_DIR  4
#define LX_DT_REG  8

/* Open a directory as a Linux getdents64-capable fd (FILE_KIND_DIR). tobyOS
 * has no native directory fd, so we just remember the path; getdents64
 * re-opens via vfs_opendir and resumes after dir_off entries. */
static long linux_open_dir(const char *kpath) {
    struct vfs_dir probe;
    if (vfs_opendir(kpath, &probe) != VFS_OK) return -ABI_ENOENT;
    vfs_closedir(&probe);
    struct file *f = (struct file *)kmalloc(sizeof *f);
    if (!f) return -ABI_ENOMEM;
    memset(f, 0, sizeof *f);
    f->kind = FILE_KIND_DIR;
    size_t n = strlen(kpath) + 1;
    f->dirpath = (char *)kmalloc(n);
    if (!f->dirpath) { kfree(f); return -ABI_ENOMEM; }
    memcpy(f->dirpath, kpath, n);
    f->dir_off = 0;
    int fd = fd_alloc_into(current_proc(), f);
    if (fd < 0) { kfree(f->dirpath); kfree(f); return -ABI_EMFILE; }
    return fd;
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
    case LX_open: {                    /* (path, flags, mode) */
        /* Opening a directory yields a getdents64-capable dir fd; files
         * fall through to the normal VFS open. */
        char kpath[ABI_PATH_MAX];
        if (resolve_user_path((const char *)a1, kpath, sizeof kpath) == 0) {
            struct vfs_stat vs;
            if (vfs_stat(kpath, &vs) == VFS_OK && vs.type == VFS_TYPE_DIR)
                return linux_open_dir(kpath);
        }
        return do_syscall(SYS_OPEN, a1, a2, a3, 0, 0);
    }
    case LX_openat: {                  /* (dirfd, path, flags, mode) */
        /* AT_FDCWD / absolute paths only (busybox uses these); a real
         * dirfd-relative open is out of scope for B3. */
        char kpath[ABI_PATH_MAX];
        if (resolve_user_path((const char *)a2, kpath, sizeof kpath) == 0) {
            struct vfs_stat vs;
            if (vfs_stat(kpath, &vs) == VFS_OK && vs.type == VFS_TYPE_DIR)
                return linux_open_dir(kpath);
        }
        return do_syscall(SYS_OPEN, a2, a3, a4, 0, 0);
    }
    case LX_dup:    return do_syscall(SYS_DUP, a1, 0, 0, 0, 0);
    case LX_pipe:   return do_syscall(SYS_PIPE, a1, 0, 0, 0, 0);
    case LX_getcwd: return do_syscall(SYS_GETCWD, a1, a2, 0, 0, 0);
    case LX_chdir:  return do_syscall(SYS_CHDIR, a1, 0, 0, 0, 0);
    case LX_mkdir:  return do_syscall(SYS_MKDIR, a1, a2, 0, 0, 0);
    case LX_unlink: return do_syscall(SYS_UNLINK, a1, 0, 0, 0, 0);

    /* ---- process control (B8): the shell forks, execs, and waits ---- */
    case LX_fork:
    case LX_vfork:
        return do_syscall(ABI_SYS_FORK, 0, 0, 0, 0, 0);
    case LX_clone:
        /* clone(flags, stack, ptid, ctid, tls). CLONE_VM (0x100) => a thread
         * that shares the address space (pthread_create); otherwise it's a
         * fork-equivalent (glibc/musl fork() may route through clone). */
        if ((uint64_t)a1 & 0x100u /* CLONE_VM */)
            return sys_clone_thread((uint64_t)a1, (uint64_t)a2, (uint64_t)a3,
                                    (uint64_t)a4, (uint64_t)a5);
        return do_syscall(ABI_SYS_FORK, 0, 0, 0, 0, 0);
    case LX_sched_yield:
        return do_syscall(SYS_YIELD, 0, 0, 0, 0, 0);
    case LX_execve:                    /* (path, argv, envp) -- same as Linux */
        return sys_execve((const char *)a1, (char *const *)a2,
                          (char *const *)a3);
    case LX_wait4: {                   /* (pid, *status, options, *rusage) */
        int   pid     = (int)a1;
        void *ustatus = (void *)a2;
        int   options = (int)a3;
        if (pid <= 0) {                /* "any child" -- sh -c forks exactly one */
            struct proc *self = current_proc();
            pid = self ? proc_any_child(self->pid) : -1;
            if (pid < 0) return -ABI_ECHILD;
        }
        if (options & 0x1 /* WNOHANG */) {
            struct proc *c = proc_lookup(pid);
            if (!c) return -ABI_ECHILD;
            if (c->state != PROC_TERMINATED) return 0;
        }
        int code = proc_wait(pid);
        if (code < 0) return -ABI_ECHILD;
        if (ustatus) {
            /* Linux wait status: normal exit -> (code & 0xff) << 8, with the
             * low 7 bits zero so WIFEXITED is true / WEXITSTATUS == code. */
            uint32_t wstatus = ((uint32_t)code & 0xff) << 8;
            if (put_user_u32(ustatus, wstatus) != 0) return -ABI_EFAULT;
        }
        return pid;
    }
    /* Job control: tobyOS has a single global foreground pid, not POSIX
     * process groups, so accept these as no-ops/identity so the shell's
     * setup doesn't abort. */
    case LX_setpgid: return 0;
    case LX_getpgid:
    case LX_getpgrp:
    case LX_setsid:  return do_syscall(SYS_GETPID, 0, 0, 0, 0, 0);

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
        if (f->kind == FILE_KIND_DIR && f->dirpath) {
            if (vfs_stat(f->dirpath, &vs) != VFS_OK)
                vs = (struct vfs_stat){ .type = VFS_TYPE_DIR, .mode = 0755 };
        } else if (f->kind == FILE_KIND_VFS) {
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
    case LX_getdents64: {              /* (fd, void *dirp, count) */
        struct file *f = fd_lookup((int)a1);
        if (!f) return -ABI_EBADF;
        if (f->kind != FILE_KIND_DIR || !f->dirpath) return -ABI_ENOTDIR;
        uint8_t *ubuf = (uint8_t *)a2;
        size_t   cap  = (size_t)a3;
        struct vfs_dir d;
        if (vfs_opendir(f->dirpath, &d) != VFS_OK) return -ABI_ENOENT;
        struct vfs_dirent ent;
        uint32_t idx = 0, emitted = 0;
        size_t   written = 0;
        bool     toosmall = false;
        /* Skip the entries already returned by earlier getdents64 calls. */
        while (idx < f->dir_off && vfs_readdir(&d, &ent) == VFS_OK) idx++;
        while (vfs_readdir(&d, &ent) == VFS_OK) {
            size_t namelen = 0;
            while (namelen < VFS_NAME_MAX && ent.name[namelen]) namelen++;
            size_t reclen = (19 + namelen + 1 + 7) & ~(size_t)7;  /* hdr=19, 8-align */
            if (written + reclen > cap) {
                if (written == 0) toosmall = true;   /* buffer can't hold one entry */
                break;
            }
            struct lx_dirent64 de;
            memset(&de, 0, reclen);
            de.d_ino    = (uint64_t)(f->dir_off + emitted + 1);
            de.d_off    = (int64_t)(f->dir_off + emitted + 1);
            de.d_reclen = (uint16_t)reclen;
            de.d_type   = (ent.type == VFS_TYPE_DIR) ? LX_DT_DIR : LX_DT_REG;
            memcpy(de.d_name, ent.name, namelen);
            de.d_name[namelen] = '\0';
            if (copy_to_user(ubuf + written, &de, reclen) != 0) {
                vfs_closedir(&d);
                return -ABI_EFAULT;
            }
            written += reclen;
            emitted++;
        }
        vfs_closedir(&d);
        f->dir_off += emitted;
        if (toosmall) return -ABI_EINVAL;
        return (long)written;          /* 0 => end of directory */
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
    case LX_mmap: {
        uint32_t lf = (uint32_t)a4;
        int      fd = (int)a5;
        long ret;
        if ((lf & LXMAP_ANONYMOUS) || fd < 0)        /* anonymous (malloc/TLS) */
            ret = sys_mmap((uint64_t)a1, (uint64_t)a2, (uint32_t)a3,
                           lx_mmap_flags(lf), -1, 0);
        else
            ret = linux_mmap_file((uint64_t)a1, (uint64_t)a2, (uint32_t)a3,
                                  lf, fd, lx_mmap_offset());  /* file-backed */
#ifdef LXMMAP_TRACE
        kprintf("[mmaptrace] addr=%p len=0x%lx prot=0x%x flags=0x%x fd=%d "
                "off=0x%lx -> %p\n", (void *)a1, (unsigned long)a2,
                (unsigned)a3, (unsigned)lf, fd,
                (unsigned long)lx_mmap_offset(), (void *)ret);
#endif
        return ret;
    }
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

    /* ---- signals (B4): translate the Linux signal ABI onto the native
     * signal layer. musl supplies its own sa_restorer trampoline (which
     * just issues rt_sigreturn); we record it as the proc's restorer so
     * delivery has a return path -- a Linux process never calls the
     * tobyOS SYS_SIGRESTORER. 1-arg handlers are fully supported; an
     * SA_SIGINFO 3-arg handler would receive tobyOS-layout siginfo/
     * ucontext (a known gap). ---- */
    case LX_rt_sigaction: {            /* (sig, act, oldact, sigsetsize) */
        int sig = (int)a1;
        if (sig <= 0 || sig >= SIG_MAX) return -ABI_EINVAL;
        if (sig == SIGKILL || sig == SIGSTOP) return -ABI_EINVAL;
        struct proc *p = current_proc();
        if (!p) return -ABI_EPERM;
        struct sigaction *cur = &p->sigstate.actions[sig];
        if (a3) {                       /* report old action in Linux layout */
            struct lx_sigaction old;
            memset(&old, 0, sizeof old);
            old.sa_handler  = (uint64_t)(uintptr_t)cur->sa_handler;
            old.sa_flags    = (uint64_t)(uint32_t)cur->sa_flags;
            old.sa_restorer = p->sigstate.restorer;
            old.sa_mask     = (uint64_t)cur->sa_mask;
            if (copy_to_user((void *)a3, &old, sizeof old) != 0)
                return -ABI_EFAULT;
        }
        if (a2) {                       /* install new action from Linux layout */
            struct lx_sigaction na;
            if (copy_from_user(&na, (const void *)a2, sizeof na) != 0)
                return -ABI_EFAULT;
            cur->sa_handler = (void (*)(int))(uintptr_t)na.sa_handler;
            cur->sa_mask    = (sigset_t)na.sa_mask;
            cur->sa_flags   = (int)na.sa_flags;
            if ((na.sa_flags & LX_SA_RESTORER) && na.sa_restorer)
                p->sigstate.restorer = na.sa_restorer;
        }
        return 0;
    }
    case LX_rt_sigprocmask:            /* (how, set, oldset, sigsetsize) */
        return sys_sigprocmask((int)a1, (const void *)a2, (void *)a3);
    case LX_rt_sigreturn:
        return sys_sigreturn();
    case LX_kill:                      /* (pid, sig) */
        return sys_kill((int)a1, (int)a2);
    case LX_tkill:                     /* (tid, sig) */
        return sys_kill((int)a1, (int)a2);
    case LX_tgkill:                    /* (tgid, tid, sig) -> signal the tid */
        return sys_kill((int)a2, (int)a3);

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

/* ============================================================
 * Track C (foreign-binary compat) -- Win32 / PE personality, milestone C1.
 *
 * A loaded Windows PE never makes raw syscalls. Its imports were bound by
 * the PE loader (src/pe_loader.c) to user-mode thunks that funnel into a
 * shared marshalling gate; that gate captures the Microsoft-x64 argument
 * registers into an 8-qword array and issues exactly one syscall:
 *   ABI_SYS_WIN32_DISPATCH(func_index, args_ptr)
 * We copy the args in and call the indexed shim below. Each shim reads the
 * MS-x64 arguments out of args[] (args[0]=rcx, [1]=rdx, [2]=r8, [3]=r9,
 * [4..7]=stack args, zeroed for C1) and returns the Win32 function's
 * result (which the gate hands back to the PE in RAX).
 *
 * The shims deliberately reuse tobyOS primitives (sys_write, proc_exit,
 * the per-proc fd table) so Win32 console I/O lands on the very same path
 * as native + Linux output.
 * ============================================================ */

/* Win32 standard-handle pseudo-values. nStdHandle is a DWORD, and the
 * compiler passes it via `mov ecx, imm32` which ZERO-extends, so the value
 * the gate captures is the 32-bit form (e.g. 0x00000000FFFFFFF5), NOT the
 * 64-bit sign-extended one. Compare at DWORD width. We map each handle
 * straight onto a tobyOS fd so the returned "HANDLE" feeds back to
 * WriteFile unchanged. */
#define WIN32_STD_INPUT_HANDLE   ((uint32_t)-10)   /* 0xFFFFFFF6 */
#define WIN32_STD_OUTPUT_HANDLE  ((uint32_t)-11)   /* 0xFFFFFFF5 */
#define WIN32_STD_ERROR_HANDLE   ((uint32_t)-12)   /* 0xFFFFFFF4 */
#define WIN32_INVALID_HANDLE     ((uint64_t)-1)

/* kernel32!GetStdHandle(DWORD nStdHandle) -> HANDLE. */
static long w32_GetStdHandle(uint64_t *args) {
    switch ((uint32_t)args[0]) {
        case WIN32_STD_INPUT_HANDLE:  return 0;   /* fd 0 */
        case WIN32_STD_OUTPUT_HANDLE: return 1;   /* fd 1 */
        case WIN32_STD_ERROR_HANDLE:  return 2;   /* fd 2 */
        default:                      return (long)WIN32_INVALID_HANDLE;
    }
}

/* kernel32!WriteFile(HANDLE hFile, LPCVOID buf, DWORD n,
 *                    LPDWORD written, LPOVERLAPPED ovl) -> BOOL.
 * We treat hFile as a tobyOS fd, ignore the (synchronous) overlapped arg,
 * and write through sys_write (fd lookup + copy_from_user + file_write).
 * On success the byte count is stored into *written (a user pointer). */
static long w32_WriteFile(uint64_t *args) {
    int       fd      = (int)args[0];
    uint64_t  ubuf    = args[1];
    uint32_t  n       = (uint32_t)args[2];
    uint64_t  pwrite  = args[3];   /* LPDWORD, may be NULL */

    long wrote = sys_write(fd, (const void *)(uintptr_t)ubuf, n);
    if (wrote < 0) {
        if (pwrite) { uint32_t z = 0; (void)copy_to_user((void *)(uintptr_t)pwrite, &z, 4); }
        return 0;   /* FALSE */
    }
    if (pwrite) {
        uint32_t w = (uint32_t)wrote;
        if (copy_to_user((void *)(uintptr_t)pwrite, &w, 4) != 0) return 0;
    }
    return 1;       /* TRUE */
}

/* kernel32!ExitProcess(UINT code) -> (noreturn). */
static long w32_ExitProcess(uint64_t *args) {
    kprintf("[win32] ExitProcess(%u)\n", (unsigned)args[0]);
    proc_exit((int)(uint32_t)args[0]);
    return 0;       /* unreached */
}

/* ============================================================
 * C2: the C-runtime printf path (api-ms-win-crt-stdio-l1-1-0.dll).
 *
 * A normal MinGW/ucrt program's printf inlines to a call through
 * __acrt_iob_func(stream) + __stdio_common_vfprintf(opts, FILE*, fmt, loc,
 * va_list) -- where va_list is the 5th (stack) argument, which is exactly
 * why the C2 gate marshals stack args. We shim those two plus puts, and
 * implement a real printf engine (flags/width/precision/length) below that
 * reads conversions from the user va_list and writes via file_write. Since
 * tobyOS has no real CRT DLL, these shims ARE the CRT for a PE.
 * ============================================================ */

/* Write a kernel buffer straight to a tobyOS fd (bypasses sys_write's
 * user-buffer bounce since our format buffer is kernel memory). */
static long win32_fd_write(int fd, const char *buf, size_t len) {
    if (len == 0) return 0;
    struct file *f = fd_lookup(fd);
    if (!f) return -1;
    return file_write(f, buf, len);
}

/* Chunked output sink for the formatter: accumulate then flush. */
struct win32_fmtbuf { int fd; size_t n; long total; char buf[256]; };
static void fb_flush(struct win32_fmtbuf *fb) {
    if (fb->n) { win32_fd_write(fb->fd, fb->buf, fb->n); fb->n = 0; }
}
static void fb_putc(struct win32_fmtbuf *fb, char c) {
    if (fb->n == sizeof(fb->buf)) fb_flush(fb);
    fb->buf[fb->n++] = c;
    fb->total++;
}
static void fb_pad(struct win32_fmtbuf *fb, char c, int count) {
    for (int i = 0; i < count; i++) fb_putc(fb, c);
}
static void fb_write(struct win32_fmtbuf *fb, const char *s, int len) {
    for (int i = 0; i < len; i++) fb_putc(fb, s[i]);
}

/* Forward digits of `v` in `base` into out[]; returns digit count (>=1). */
static int u64_digits(uint64_t v, char *out, int base, bool upper) {
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do { tmp[n++] = d[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

/* Read the next 8-byte va_list slot (a Microsoft-x64 vararg) from user. */
static uint64_t va_next(uint64_t uva, int *idx) {
    uint64_t v = 0;
    (void)copy_from_user(&v, (const void *)(uintptr_t)(uva + (size_t)(*idx) * 8), 8);
    (*idx)++;
    return v;
}

/* Emit prefix(sign/0x) + zero/space padding + body per flags/width. */
static void emit_field(struct win32_fmtbuf *fb, const char *pre, int pl,
                       const char *body, int bl, int width, bool left, bool zero) {
    int total = pl + bl;
    int pad = width > total ? width - total : 0;
    if (!left && !zero) fb_pad(fb, ' ', pad);
    fb_write(fb, pre, pl);
    if (!left && zero) fb_pad(fb, '0', pad);
    fb_write(fb, body, bl);
    if (left) fb_pad(fb, ' ', pad);
}

#define WIN32_FMT_MAX  4096

/* The printf engine. Reads the format string + variadic args from user
 * memory and writes the rendered output to `fd`. Returns chars written. */
static long win32_vformat(int fd, uint64_t ufmt, uint64_t uva) {
    char fmt[1024];
    long fl = strncpy_from_user(fmt, (const char *)(uintptr_t)ufmt, sizeof(fmt));
    if (fl < 0) return -1;
    fmt[sizeof(fmt) - 1] = '\0';

    struct win32_fmtbuf fb = { .fd = fd, .n = 0, .total = 0 };
    int ai = 0;
    const char *p = fmt;

    while (*p) {
        if (*p != '%') { fb_putc(&fb, *p++); continue; }
        p++;
        if (*p == '%') { fb_putc(&fb, '%'); p++; continue; }

        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; p++) {
            if (*p == '-') left = true;
            else if (*p == '0') zero = true;
            else if (*p == '+') plus = true;
            else if (*p == ' ') space = true;
            else if (*p == '#') alt = true;
            else break;
        }

        int width = 0;
        if (*p == '*') { p++; int w = (int)(int32_t)va_next(uva, &ai);
                         if (w < 0) { left = true; w = -w; } width = w; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (width > WIN32_FMT_MAX) width = WIN32_FMT_MAX;

        int prec = -1;
        if (*p == '.') {
            p++; prec = 0;
            if (*p == '*') { p++; prec = (int)(int32_t)va_next(uva, &ai); if (prec < 0) prec = -1; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        if (prec > WIN32_FMT_MAX) prec = WIN32_FMT_MAX;

        /* Length. NOTE: Windows `long` is 32-bit, so a single 'l' stays
         * 32-bit; only 'll'/'I64'/'z'/'j'/'t' are 64-bit. */
        bool wide = false;
        if (*p == 'l') { p++; if (*p == 'l') { wide = true; p++; } }
        else if (*p == 'h') { p++; if (*p == 'h') p++; }
        else if (*p == 'z' || *p == 'j' || *p == 't') { wide = true; p++; }
        else if (*p == 'I') {
            p++;
            if (p[0] == '6' && p[1] == '4') { wide = true; p += 2; }
            else if (p[0] == '3' && p[1] == '2') { p += 2; }
            else wide = true;                 /* bare 'I' == pointer width */
        }

        char conv = *p ? *p++ : '\0';
        char numbody[80];
        char pre[4]; int pl = 0;

        switch (conv) {
        case 'd': case 'i': {
            uint64_t raw = va_next(uva, &ai);
            int64_t sv = wide ? (int64_t)raw : (int64_t)(int32_t)raw;
            uint64_t mag;
            if (sv < 0) { mag = (uint64_t)(-sv); pre[pl++] = '-'; }
            else { mag = (uint64_t)sv; if (plus) pre[pl++] = '+'; else if (space) pre[pl++] = ' '; }
            char digs[24]; int dn = u64_digits(mag, digs, 10, false);
            if (prec == 0 && mag == 0) dn = 0;
            int zeros = (prec > dn) ? (prec - dn) : 0;
            int bl = 0;
            for (int i = 0; i < zeros && bl < (int)sizeof(numbody); i++) numbody[bl++] = '0';
            for (int i = 0; i < dn && bl < (int)sizeof(numbody); i++) numbody[bl++] = digs[i];
            emit_field(&fb, pre, pl, numbody, bl, width, left, zero && prec < 0);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'p': {
            uint64_t raw = va_next(uva, &ai);
            int base = (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : (conv == 'o' ? 8 : 10);
            bool upper = (conv == 'X');
            uint64_t v = (conv == 'p' || wide) ? raw : (uint32_t)raw;
            if (conv == 'p') { pre[pl++] = '0'; pre[pl++] = 'x'; }
            else if (alt && v != 0 && (conv == 'x' || conv == 'X')) { pre[pl++] = '0'; pre[pl++] = (char)conv; }
            char digs[24]; int dn = u64_digits(v, digs, base, upper);
            if (prec == 0 && v == 0) dn = 0;
            if (alt && conv == 'o' && (dn == 0 || digs[0] != '0')) { /* leading 0 */ }
            int zeros = (prec > dn) ? (prec - dn) : 0;
            int bl = 0;
            if (alt && conv == 'o') numbody[bl++] = '0';
            for (int i = 0; i < zeros && bl < (int)sizeof(numbody); i++) numbody[bl++] = '0';
            for (int i = 0; i < dn && bl < (int)sizeof(numbody); i++) numbody[bl++] = digs[i];
            emit_field(&fb, pre, pl, numbody, bl, width, left, zero && prec < 0);
            break;
        }
        case 'c': {
            char ch = (char)va_next(uva, &ai);
            emit_field(&fb, pre, 0, &ch, 1, width, left, false);
            break;
        }
        case 's': {
            uint64_t sp = va_next(uva, &ai);
            char sbuf[1024];
            int sl = 0;
            if (sp) {
                long n = strncpy_from_user(sbuf, (const char *)(uintptr_t)sp, sizeof(sbuf));
                if (n < 0) { const char *bad = "(badptr)"; fb_write(&fb, bad, 8); break; }
                sl = (n >= (long)sizeof(sbuf)) ? (int)sizeof(sbuf) - 1 : (int)n;
            } else {
                const char *nul = "(null)";
                emit_field(&fb, pre, 0, nul, 6, width, left, false);
                break;
            }
            if (prec >= 0 && prec < sl) sl = prec;
            emit_field(&fb, pre, 0, sbuf, sl, width, left, false);
            break;
        }
        case '\0':
            fb_putc(&fb, '%');
            break;
        default:
            /* Unknown conversion: emit it verbatim so nothing is silently lost. */
            fb_putc(&fb, '%');
            fb_putc(&fb, conv);
            break;
        }
    }

    fb_flush(&fb);
    return fb.total;
}

/* api-ms-win-crt-stdio-l1-1-0.dll!__acrt_iob_func(unsigned index) -> FILE*.
 * Returns an opaque token encoding the std fd; __stdio_common_vfprintf (also
 * a shim) decodes it. index 0/1/2 = stdin/stdout/stderr. */
#define WIN32_IOB_TAG 0xF11E0000ULL
static long w32_acrt_iob_func(uint64_t *args) {
    return (long)(WIN32_IOB_TAG | ((uint32_t)args[0] & 0xFF));
}

/* api-ms-win-crt-stdio-l1-1-0.dll!__stdio_common_vfprintf(
 *     UINT64 options, FILE* stream, const char* fmt, _locale_t loc, va_list).
 * The printf-family target. Decode the fd from the FILE* token, ignore
 * options + locale, run the formatter. Returns chars written. */
static long w32_stdio_common_vfprintf(uint64_t *args) {
    uint64_t stream = args[1];
    uint64_t ufmt   = args[2];
    uint64_t uva    = args[4];        /* 5th arg, marshalled off the stack */
    int fd = 1;
    if ((stream & 0xFFFFFF00ULL) == WIN32_IOB_TAG) fd = (int)(stream & 0xFF);
    if (!ufmt || !uva) return -1;
    return win32_vformat(fd, ufmt, uva);
}

/* api-ms-win-crt-stdio-l1-1-0.dll!puts(const char* s) -> >=0 / EOF.
 * Writes s + '\n' to stdout. */
static long w32_puts(uint64_t *args) {
    char s[1024];
    long n = strncpy_from_user(s, (const char *)(uintptr_t)args[0], sizeof(s));
    if (n < 0) return -1;             /* EOF */
    if (n >= (long)sizeof(s)) n = sizeof(s) - 1;
    win32_fd_write(1, s, (size_t)n);
    win32_fd_write(1, "\n", 1);
    return n + 1;
}

/* ============================================================
 * C3: enough kernel32 + ucrt to run a STOCK clang-built .exe through the
 * full mainCRTStartup. Most shims are trivial (no-op / return-constant /
 * return-pointer-into-the-CRT-data-page). The substantive ones: the heap
 * (malloc/calloc over sys_mmap'd user memory), the __p_* accessors, and
 * _initterm (which walks the C/C++ init tables -- empty for plain C).
 * ============================================================ */

/* The legion of "no-op / return 0" and "return TRUE" Win32 entry points. */
static long w32_zero(uint64_t *a) { (void)a; return 0; }
static long w32_one (uint64_t *a) { (void)a; return 1; }

/* CRT process-termination entry points -> tobyOS proc_exit. */
static long w32_exit (uint64_t *a) { proc_exit((int)(uint32_t)a[0]); return 0; }
static long w32_abort(uint64_t *a) { (void)a; proc_exit(3); return 0; }

/* The Win32 process heap: a per-proc bump allocator over anonymous user
 * memory (sys_mmap). Never reclaims (free() is a no-op), so every block is
 * fresh + zeroed -- which also makes calloc == malloc. */
#define WIN32_HEAP_ARENA (4u * 1024u * 1024u)
static uint64_t win32_heap_alloc(uint64_t n) {
    struct proc *p = current_proc();
    if (!p) return 0;
    n = (n + 15) & ~15ull;                 /* 16-byte align */
    if (n == 0) n = 16;
    if (p->win_heap_cur == 0 || p->win_heap_cur + n > p->win_heap_end) {
        uint64_t want = (n > WIN32_HEAP_ARENA) ? ((n + 0xFFFull) & ~0xFFFull)
                                               : WIN32_HEAP_ARENA;
        long base = sys_mmap(0, want, 0x03 /*RW*/, 0x05 /*ANON|PRIVATE*/, -1, 0);
        if (base < 0) return 0;
        p->win_heap_cur = (uint64_t)base;
        p->win_heap_end = (uint64_t)base + want;
    }
    uint64_t r = p->win_heap_cur;
    p->win_heap_cur += n;
    return r;
}
static long w32_malloc(uint64_t *a) { return (long)win32_heap_alloc(a[0]); }
static long w32_calloc(uint64_t *a) {
    uint64_t nmemb = a[0], sz = a[1], total = nmemb * sz;
    if (sz && total / sz != nmemb) return 0;       /* overflow */
    return (long)win32_heap_alloc(total);          /* mmap memory is zeroed */
}

/* __p_* accessors -> a USER pointer into the CRT data page (set up by the
 * PE loader). The ucrt startup reads argc/argv/environ/_commode/_fmode here. */
static long w32_p_argc(uint64_t *a)    { (void)a; return (long)(WIN32_CRT_DATA_BASE + WIN32_CRT_ARGC); }
static long w32_p_argv(uint64_t *a)    { (void)a; return (long)(WIN32_CRT_DATA_BASE + WIN32_CRT_ARGV); }
static long w32_p_environ(uint64_t *a) { (void)a; return (long)(WIN32_CRT_DATA_BASE + WIN32_CRT_ENVIRON); }
static long w32_p_commode(uint64_t *a) { (void)a; return (long)(WIN32_CRT_DATA_BASE + WIN32_CRT_COMMODE); }
static long w32_p_fmode(uint64_t *a)   { (void)a; return (long)(WIN32_CRT_DATA_BASE + WIN32_CRT_FMODE); }

/* _initterm(first,last) / _initterm_e: walk a table of init function
 * pointers and call each non-NULL one. A plain C program's tables are empty
 * (all NULL) so nothing is called -- which is necessary, because a kernel
 * shim cannot call back into CPL3. A non-NULL entry (a C++ global ctor) is
 * logged + skipped (a later user-mode-trampoline milestone). */
static long w32_initterm(uint64_t *a) {
    uint64_t p = a[0], end = a[1];
    for (; p + 8 <= end; p += 8) {
        uint64_t fn = 0;
        if (copy_from_user(&fn, (const void *)(uintptr_t)p, 8) != 0) break;
        if (fn) kprintf("[win32] _initterm: skipping ctor @%p (no user callback yet)\n",
                        (void *)(uintptr_t)fn);
    }
    return 0;
}

/* memcpy / strlen / strncmp over user memory (imported by the CRT). */
static long w32_memcpy(uint64_t *a) {
    uint64_t dst = a[0], src = a[1], n = a[2], done = 0;
    char buf[256];
    while (done < n) {
        uint64_t chunk = n - done; if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (copy_from_user(buf, (const void *)(uintptr_t)(src + done), chunk) != 0) break;
        if (copy_to_user((void *)(uintptr_t)(dst + done), buf, chunk) != 0) break;
        done += chunk;
    }
    return (long)dst;
}
static long w32_strlen(uint64_t *a) {
    char buf[1024];
    long n = strncpy_from_user(buf, (const char *)(uintptr_t)a[0], sizeof(buf));
    if (n < 0) return 0;
    return (n >= (long)sizeof(buf)) ? (long)sizeof(buf) - 1 : n;
}
static long w32_memcmp(uint64_t *a) {
    uint64_t p1 = a[0], p2 = a[1], n = a[2], done = 0;
    char b1[256], b2[256];
    while (done < n) {
        uint64_t chunk = n - done; if (chunk > sizeof(b1)) chunk = sizeof(b1);
        if (copy_from_user(b1, (const void *)(uintptr_t)(p1 + done), chunk) != 0) return 1;
        if (copy_from_user(b2, (const void *)(uintptr_t)(p2 + done), chunk) != 0) return 1;
        for (uint64_t i = 0; i < chunk; i++) {
            unsigned char c1 = (unsigned char)b1[i], c2 = (unsigned char)b2[i];
            if (c1 != c2) return (long)c1 - (long)c2;
        }
        done += chunk;
    }
    return 0;
}
static long w32_strncmp(uint64_t *a) {
    char s1[512], s2[512];
    uint64_t n = a[2]; if (n > 511) n = 511;
    if (strncpy_from_user(s1, (const char *)(uintptr_t)a[0], sizeof(s1)) < 0) return 0;
    if (strncpy_from_user(s2, (const char *)(uintptr_t)a[1], sizeof(s2)) < 0) return 0;
    for (uint64_t i = 0; i < n; i++) {
        unsigned char c1 = (unsigned char)s1[i], c2 = (unsigned char)s2[i];
        if (c1 != c2) return (long)c1 - (long)c2;
        if (c1 == 0) break;
    }
    return 0;
}

/* VirtualQuery(addr, buf, len): fill a minimal MEMORY_BASIC_INFORMATION
 * (committed, private, read-write). Enough for the CRT's startup probes. */
static long w32_VirtualQuery(uint64_t *a) {
    struct {
        uint64_t BaseAddress, AllocationBase;
        uint32_t AllocationProtect, _pad0;
        uint64_t RegionSize;
        uint32_t State, Protect, Type, _pad1;
    } mbi;
    memset(&mbi, 0, sizeof(mbi));
    mbi.BaseAddress    = a[0] & ~0xFFFull;
    mbi.AllocationBase = a[0] & ~0xFFFull;
    mbi.AllocationProtect = 0x04;     /* PAGE_READWRITE */
    mbi.RegionSize     = 0x100000;    /* 1 MiB */
    mbi.State          = 0x1000;      /* MEM_COMMIT  */
    mbi.Protect        = 0x04;        /* PAGE_READWRITE */
    mbi.Type           = 0x20000;     /* MEM_PRIVATE */
    if (copy_to_user((void *)(uintptr_t)a[1], &mbi, sizeof(mbi)) != 0) return 0;
    return (long)sizeof(mbi);
}

/* ---- C4: the extra ucrt surface a C++ program pulls in ---- */

/* _errno() / localeconv() / strerror(): return USER pointers into the CRT
 * data page (errno slot, minimal "C" lconv, a fixed message). */
static long w32_errno(uint64_t *a)     { (void)a; return (long)(WIN32_CRT_DATA_BASE + WIN32_CRT_ERRNO); }
static long w32_localeconv(uint64_t *a){ (void)a; return (long)(WIN32_CRT_DATA_BASE + WIN32_CRT_LCONV); }
static long w32_strerror(uint64_t *a)  { (void)a; return (long)(WIN32_CRT_DATA_BASE + WIN32_CRT_STRERR); }

/* fputc(c, FILE*) -> write one byte to the fd encoded in the FILE* token. */
static long w32_fputc(uint64_t *a) {
    int  c  = (int)(unsigned char)a[0];
    int  fd = 1;
    if ((a[1] & 0xFFFFFF00ULL) == WIN32_IOB_TAG) fd = (int)(a[1] & 0xFF);
    char ch = (char)c;
    if (win32_fd_write(fd, &ch, 1) < 0) return -1;   /* EOF */
    return c;
}

/* strnlen(s, max) over user memory. */
static long w32_strnlen(uint64_t *a) {
    uint64_t max = a[1];
    char buf[1024];
    long n = strncpy_from_user(buf, (const char *)(uintptr_t)a[0], sizeof(buf));
    if (n < 0) return 0;
    if (n >= (long)sizeof(buf)) n = sizeof(buf) - 1;
    return ((uint64_t)n > max) ? (long)max : n;
}

/* wcslen(ws): length of a UTF-16 string (count u16 units until 0). */
static long w32_wcslen(uint64_t *a) {
    uint64_t p = a[0];
    for (long n = 0; n < 65536; n++) {
        uint16_t w = 0;
        if (copy_from_user(&w, (const void *)(uintptr_t)(p + (uint64_t)n * 2), 2) != 0) return n;
        if (w == 0) return n;
    }
    return 65536;
}

/* ---- C5: file I/O via real HANDLE<->fd mapping ----
 * A Win32 HANDLE from CreateFile/GetStdHandle is just a tobyOS fd; the ucrt
 * FILE* tokens (0xF11E....) used by the stdio shims are a separate space and
 * never reach these. CreateFileA maps Windows access/disposition flags onto
 * O_* + sys_open; ReadFile/WriteFile/CloseHandle forward to sys_read/write/
 * close. */

/* kernel32!CreateFileA(name, access, share, sec, disposition, flags, template)
 * -> HANDLE (== fd) or INVALID_HANDLE_VALUE. */
static long w32_CreateFileA(uint64_t *a) {
    char wpath[ABI_PATH_MAX];
    long pn = strncpy_from_user(wpath, (const char *)(uintptr_t)a[0], sizeof(wpath));
    if (pn < 0) return (long)WIN32_INVALID_HANDLE;
    wpath[sizeof(wpath) - 1] = '\0';

    /* Windows path -> tobyOS path. A drive letter "X:" maps to tobyOS's
     * writable mount /data (the root ramfs is a read-only initrd), and '\'
     * becomes '/'. A path with no drive is left relative to the cwd. */
    char kp[ABI_PATH_MAX];
    int si = 0, di = 0;
    if (wpath[0] && wpath[1] == ':') {
        for (const char *r = "/data"; *r && di < (int)sizeof(kp) - 1; r++) kp[di++] = *r;
        si = 2;
        if (wpath[si] != '\\' && wpath[si] != '/' && di < (int)sizeof(kp) - 1) kp[di++] = '/';
    }
    for (; wpath[si] && di < (int)sizeof(kp) - 1; si++)
        kp[di++] = (wpath[si] == '\\') ? '/' : wpath[si];
    kp[di] = '\0';

    /* Hand sys_open a USER pointer: stage the translated path in the CRT page. */
    uint64_t ubuf = WIN32_CRT_DATA_BASE + WIN32_CRT_PATHBUF;
    if (copy_to_user((void *)(uintptr_t)ubuf, kp, (size_t)di + 1) != 0)
        return (long)WIN32_INVALID_HANDLE;

    uint32_t access = (uint32_t)a[1];
    uint32_t disp   = (uint32_t)a[4];
    bool rd = (access & 0x80000000UL) != 0;   /* GENERIC_READ  */
    bool wr = (access & 0x40000000UL) != 0;   /* GENERIC_WRITE */
    int flags = (rd && wr) ? ABI_O_RDWR : (wr ? ABI_O_WRONLY : ABI_O_RDONLY);
    switch (disp) {
        case 1: flags |= ABI_O_CREAT | ABI_O_EXCL;  break;  /* CREATE_NEW       */
        case 2: flags |= ABI_O_CREAT | ABI_O_TRUNC; break;  /* CREATE_ALWAYS    */
        case 3:                                      break;  /* OPEN_EXISTING    */
        case 4: flags |= ABI_O_CREAT;                break;  /* OPEN_ALWAYS      */
        case 5: flags |= ABI_O_TRUNC;                break;  /* TRUNCATE_EXISTING */
        default: break;
    }
    long fd = sys_open((const char *)(uintptr_t)ubuf, flags, 0644);
    return (fd < 0) ? (long)WIN32_INVALID_HANDLE : fd;
}

/* kernel32!ReadFile(hFile, buf, n, *read, overlapped) -> BOOL. */
static long w32_ReadFile(uint64_t *a) {
    int      fd     = (int)a[0];
    uint64_t ubuf   = a[1];
    uint32_t n      = (uint32_t)a[2];
    uint64_t pread  = a[3];
    long got = sys_read(fd, (void *)(uintptr_t)ubuf, n);
    if (got < 0) {
        if (pread) { uint32_t z = 0; (void)copy_to_user((void *)(uintptr_t)pread, &z, 4); }
        return 0;
    }
    if (pread) {
        uint32_t g = (uint32_t)got;
        if (copy_to_user((void *)(uintptr_t)pread, &g, 4) != 0) return 0;
    }
    return 1;
}

/* kernel32!CloseHandle(h) -> BOOL. Leaves std handles (0/1/2), the ucrt
 * FILE* tokens, and thread handles (reaped by the join) alone; closes real
 * file fds. */
static long w32_CloseHandle(uint64_t *a) {
    uint64_t h = a[0];
    if ((h & 0xFF00000000000000ULL) == WIN32_THREAD_TAG) return 1; /* thread handle */
    if ((h & 0xFFFFFF00ULL) == WIN32_IOB_TAG) return 1;           /* stdio FILE token */
    int fd = (int)h;
    if (fd < 3) return 1;                                         /* keep std handles open */
    return (sys_close(fd) == 0) ? 1 : 0;
}

/* ---- C6: multithreading + real critical sections ----
 * CreateThread starts a tobyOS thread at the CPL3 wrapper (shim page) which
 * calls the thread function and exits; WaitForSingleObject joins it. A thread
 * HANDLE is WIN32_THREAD_TAG|tid (distinct from file fds / FILE* tokens).
 * EnterCriticalSection/LeaveCriticalSection are REAL mutual exclusion: the
 * lock state lives in the user CRITICAL_SECTION struct (OwningThread at +0x10,
 * RecursionCount at +0x0C), and the BKL makes the read-modify-write atomic
 * across threads. */
#define WIN32_THREAD_STACK (256u * 1024u)

static long w32_CreateThread(uint64_t *a) {
    uint64_t start = a[2];   /* lpStartAddress */
    uint64_t param = a[3];   /* lpParameter    */
    uint64_t ptid  = a[5];   /* lpThreadId (optional) */

    /* {func, param} block + a thread stack, both from the per-proc heap. */
    uint64_t block = win32_heap_alloc(16);
    if (!block) return 0;
    uint64_t fp[2] = { start, param };
    if (copy_to_user((void *)(uintptr_t)block, fp, 16) != 0) return 0;

    uint64_t stack = win32_heap_alloc(WIN32_THREAD_STACK);
    if (!stack) return 0;
    uint64_t stack_top = (stack + WIN32_THREAD_STACK) & ~0xFULL;   /* 16-aligned */

    struct proc *leader = current_proc();
    int tid = thread_create(WIN32_THREAD_WRAPPER_VA, block, stack_top, 0);
    if (tid < 0) return 0;

    /* The new thread must carry the Win32 personality (so its gate calls route
     * to the dispatcher) and share the TEB. Safe here: we hold the BKL, so the
     * new thread can't run until this syscall yields/returns. */
    struct proc *t = proc_lookup(tid);
    if (t) { t->personality = ABI_PERS_WIN32; t->gs_base = leader ? leader->gs_base : 0; }

    if (ptid) { uint32_t id = (uint32_t)tid; (void)copy_to_user((void *)(uintptr_t)ptid, &id, 4); }
    return (long)(WIN32_THREAD_TAG | (uint64_t)(uint32_t)tid);
}

/* WaitForSingleObject(handle, ms) -> WAIT_OBJECT_0(0) / WAIT_FAILED. For a
 * thread handle this is a join (timeout ignored == INFINITE). */
static long w32_WaitForSingleObject(uint64_t *a) {
    uint64_t h = a[0];
    if ((h & 0xFF00000000000000ULL) == WIN32_THREAD_TAG) {
        int tid = (int)(h & 0xFFFFFFFFULL);
        return (thread_join(tid, 0) == 0) ? 0 : (long)0xFFFFFFFF;
    }
    return 0;
}

#define WIN32_CS_RECURSION 0x0C   /* CRITICAL_SECTION.RecursionCount (LONG) */
#define WIN32_CS_OWNER     0x10   /* CRITICAL_SECTION.OwningThread (HANDLE)  */

static long w32_InitializeCriticalSection(uint64_t *a) {
    uint64_t cs = a[0];
    uint32_t z4 = 0; uint64_t z8 = 0;
    (void)copy_to_user((void *)(uintptr_t)(cs + WIN32_CS_RECURSION), &z4, 4);
    (void)copy_to_user((void *)(uintptr_t)(cs + WIN32_CS_OWNER),     &z8, 8);
    return 0;
}

static long w32_EnterCriticalSection(uint64_t *a) {
    uint64_t cs = a[0];
    struct proc *p = current_proc();
    uint64_t me = p ? (uint64_t)p->pid : 0;
    for (;;) {
        uint64_t owner = 0;
        if (copy_from_user(&owner, (const void *)(uintptr_t)(cs + WIN32_CS_OWNER), 8) != 0)
            return 0;
        if (owner == 0 || owner == me) {
            uint32_t rec = 0;
            (void)copy_from_user(&rec, (const void *)(uintptr_t)(cs + WIN32_CS_RECURSION), 4);
            rec++;
            (void)copy_to_user((void *)(uintptr_t)(cs + WIN32_CS_OWNER),     &me,  8);
            (void)copy_to_user((void *)(uintptr_t)(cs + WIN32_CS_RECURSION), &rec, 4);
            return 0;
        }
        /* Held by another thread -- yield (drops/reacquires the BKL so the
         * owner can run + release) and retry. */
        sched_yield();
    }
}

static long w32_LeaveCriticalSection(uint64_t *a) {
    uint64_t cs = a[0];
    uint32_t rec = 0;
    if (copy_from_user(&rec, (const void *)(uintptr_t)(cs + WIN32_CS_RECURSION), 4) != 0)
        return 0;
    if (rec > 0) rec--;
    (void)copy_to_user((void *)(uintptr_t)(cs + WIN32_CS_RECURSION), &rec, 4);
    if (rec == 0) { uint64_t z = 0; (void)copy_to_user((void *)(uintptr_t)(cs + WIN32_CS_OWNER), &z, 8); }
    return 0;
}

/* ---- C7: the user32/gdi32 GUI bridge ----
 * A Win32 GUI app's window maps onto a tobyOS window (sys_gui_create -> an fd
 * that is BOTH the HWND and the HDC); drawing maps onto sys_gui_fill/text;
 * the message loop is synthesised here (CreateWindow/ShowWindow queue a
 * WM_PAINT; PostQuitMessage ends the loop). The one genuinely hard bit --
 * DispatchMessage calling the app's WndProc -- is handled by a user-mode
 * trampoline (the kernel can't call CPL3 code); see WIN32_DISPATCH_STUB_VA
 * + win32_user_stub_va in pe_loader.c. For now a single window per process. */
static struct {
    int      tgid;        /* owning process (thread group); 0 = none */
    int      fd;          /* sys_gui_create fd == HWND == HDC          */
    int      w, h;        /* client size                              */
    uint64_t wndproc;     /* registered WndProc (also in the CRT slot) */
    bool     needs_paint; /* a WM_PAINT is queued                      */
    bool     destroy;     /* DestroyWindow seen -> deliver WM_DESTROY  */
    bool     quit;        /* PostQuitMessage seen                      */
    int      quitcode;
    uint32_t fill_color;  /* last FillRect colour (TextOut bg tracks it) */
} g_win32_gui;

/* Win32 messages. WM_PAINT/WM_QUIT/WM_NULL are synthesised by the loop; the
 * WM_* in the 0x01xx/0x02xx ranges are translated from struct gui_event by
 * GetMessage so the WndProc receives real mouse/keyboard/close input (C8). */
#define WM_NULL         0x0000
#define WM_DESTROY      0x0002
#define WM_PAINT        0x000F
#define WM_CLOSE        0x0010
#define WM_QUIT         0x0012
#define WM_KEYDOWN      0x0100
#define WM_CHAR         0x0102
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_RBUTTONDOWN  0x0204
#define WM_RBUTTONUP    0x0205

/* MK_* wParam button-state bits for mouse messages. */
#define MK_LBUTTON      0x0001
#define MK_RBUTTON      0x0002
#define MK_MBUTTON      0x0010

/* A CreateSolidBrush handle is a tagged token carrying its 24-bit COLORREF so
 * FillRect can honour the colour. The high byte (0x7B) keeps it distinct from
 * the small-integer system-colour brushes (COLOR_WINDOW+1 etc.) and from the
 * thread/file/FILE* handle spaces. */
#define WIN32_BRUSH_TAG   0x7B00000000000000ull
#define WIN32_BRUSH_MASK  0xFF00000000000000ull

/* The default window fill (C7's blue) used when no solid brush is supplied. */
#define WIN32_FILL_DEFAULT 0x002E5C8Au

/* The Win32 MSG struct as the loop sees it (and as the DispatchMessage
 * trampoline reads it: hwnd@0, message@8, wParam@0x10, lParam@0x18). */
struct win32_msg {
    uint64_t hwnd;
    uint32_t message;
    uint32_t pad;
    uint64_t wParam;
    uint64_t lParam;
    uint32_t time;
    int32_t  ptx, pty;
};

/* When set (by the C8 harness via win32_gui_set_log), GetMessage logs each
 * non-trivial input message it delivers to the WndProc -- crisp proof the
 * mouse/keyboard/close events reach the app, without the per-frame volume of
 * GUI_TRACE_VERBOSE (which slowed the run ~13x writing serial). */
static bool g_win32_log_input;
void win32_gui_set_log(bool on) { g_win32_log_input = on; }

/* Poll one input event off a Win32 app's window, kernel-side (NO copy_to_user
 * -- unlike sys_gui_poll_event, the caller here is a shim with a kernel
 * destination). Drains pending USB/PS2 input first, like the syscall does. */
static int win32_poll_window_event(int fd, struct gui_event *out) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    syscall_service_input();
    return gui_window_poll_event(f->win, out);
}

/* RegisterClassA(&WNDCLASSA): stash the WndProc (offset +0x08) both in our
 * state and in the user CRT slot the DispatchMessage trampoline reads. */
static long w32_RegisterClassA(uint64_t *a) {
    uint64_t wc = a[0];
    uint64_t wndproc = 0;
    (void)copy_from_user(&wndproc, (const void *)(uintptr_t)(wc + 0x08), 8);
    g_win32_gui.wndproc = wndproc;
    (void)copy_to_user((void *)(uintptr_t)(WIN32_CRT_DATA_BASE + WIN32_CRT_WNDPROC),
                       &wndproc, 8);
    return 1;   /* a non-zero class atom */
}

/* CreateWindowExA(exStyle, class, name, style, x, y, W, H, parent, menu,
 *                 hInst, param) -> HWND (== the tobyOS window fd). W=a6, H=a7,
 * name=a2 -- all within the gate's 8 marshalled args. */
static long w32_CreateWindowExA(uint64_t *a) {
    uint64_t name = a[2];
    int w = (int)(uint32_t)a[6];
    int h = (int)(uint32_t)a[7];
    if (w <= 0 || w > 4096) w = 400;
    if (h <= 0 || h > 4096) h = 200;
    long fd = sys_gui_create((uint32_t)w, (uint32_t)h, (const char *)(uintptr_t)name);
    if (fd < 0) return 0;   /* NULL HWND */
    struct proc *p = current_proc();
    g_win32_gui.tgid        = p ? (p->is_thread ? p->tgid : p->pid) : 0;
    g_win32_gui.fd          = (int)fd;
    g_win32_gui.w           = w;
    g_win32_gui.h           = h;
    g_win32_gui.needs_paint = true;
    g_win32_gui.destroy     = false;
    g_win32_gui.quit        = false;
    g_win32_gui.fill_color  = WIN32_FILL_DEFAULT;
    return fd;   /* HWND == HDC == fd */
}

static long w32_ShowWindow(uint64_t *a)   { (void)a; g_win32_gui.needs_paint = true; return 1; }
static long w32_UpdateWindow(uint64_t *a) { (void)a; g_win32_gui.needs_paint = true; return 1; }
static long w32_PostQuitMessage(uint64_t *a) {
    g_win32_gui.quit = true; g_win32_gui.quitcode = (int)a[0]; return 0;
}
static long w32_TranslateMessage(uint64_t *a) { (void)a; return 0; }

/* DefWindowProcA(hwnd, message, wParam, lParam). The one default behaviour we
 * implement is the idiomatic WM_CLOSE -> DestroyWindow: a real app that doesn't
 * handle WM_CLOSE in its WndProc passes it here, and Windows responds by
 * destroying the window (which then sends WM_DESTROY). */
static long w32_DefWindowProcA(uint64_t *a) {
    if ((uint32_t)a[1] == WM_CLOSE) g_win32_gui.destroy = true;
    return 0;
}

/* DestroyWindow(hwnd) -> TRUE. We don't tear the tobyOS window down here (the
 * process exit / file_close does that); we just arm GetMessage to deliver one
 * WM_DESTROY so the app's WndProc can PostQuitMessage and unwind cleanly. */
static long w32_DestroyWindow(uint64_t *a) { (void)a; g_win32_gui.destroy = true; return 1; }

/* InvalidateRect(hwnd, &RECT|NULL, erase) -> TRUE. Marks a repaint pending so
 * the next GetMessage delivers WM_PAINT (how an app asks to redraw, e.g. after
 * a click changes its state). */
static long w32_InvalidateRect(uint64_t *a) { (void)a; g_win32_gui.needs_paint = true; return 1; }

/* Translate one struct gui_event into a Win32 message in *m. Returns true if a
 * deliverable message was produced (RESIZE/NONE are dropped). */
static bool win32_event_to_msg(const struct gui_event *ev,
                               struct win32_msg *m) {
    m->hwnd = (uint64_t)g_win32_gui.fd;
    switch (ev->type) {
    case GUI_EV_MOUSE_MOVE:
        m->message = WM_MOUSEMOVE;
        m->wParam  = (ev->button & 1) ? MK_LBUTTON : 0;
        m->lParam  = ((uint64_t)(uint16_t)ev->x) | ((uint64_t)(uint16_t)ev->y << 16);
        return true;
    case GUI_EV_MOUSE_DOWN:
        m->message = (ev->button & 2) ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
        m->wParam  = (ev->button & 2) ? MK_RBUTTON : MK_LBUTTON;
        m->lParam  = ((uint64_t)(uint16_t)ev->x) | ((uint64_t)(uint16_t)ev->y << 16);
        return true;
    case GUI_EV_MOUSE_UP:
        m->message = (ev->button & 2) ? WM_RBUTTONUP : WM_LBUTTONUP;
        m->wParam  = 0;
        m->lParam  = ((uint64_t)(uint16_t)ev->x) | ((uint64_t)(uint16_t)ev->y << 16);
        return true;
    case GUI_EV_KEY:
        /* tobyOS delivers an ASCII byte; surface it as both a virtual-key-ish
         * WM_KEYDOWN (wParam = the byte) -- enough for an app that switches on
         * keystrokes. (A separate WM_CHAR would need TranslateMessage state we
         * don't keep; the byte in wParam is sufficient here.) */
        m->message = WM_KEYDOWN;
        m->wParam  = (uint64_t)ev->key;
        m->lParam  = 1;
        return true;
    case GUI_EV_CLOSE:
        m->message = WM_CLOSE;
        return true;
    default:
        return false;   /* GUI_EV_NONE / GUI_EV_RESIZE -> no message */
    }
}

/* GetMessageA(&MSG, hwnd, min, max) -> >0 normal / 0 on WM_QUIT. Order of
 * delivery: a pending WM_DESTROY (after DestroyWindow), then WM_QUIT (after
 * PostQuitMessage), then a queued WM_PAINT, then any real input event drained
 * from the window (mouse/keyboard/close -> WM_*), else yield + WM_NULL so the
 * loop keeps turning without busy-spinning. */
static long w32_GetMessageA(uint64_t *a) {
    uint64_t msg = a[0];
    struct win32_msg m;
    memset(&m, 0, sizeof(m));

    if (g_win32_gui.quit) {
        m.message = WM_QUIT; m.wParam = (uint64_t)(uint32_t)g_win32_gui.quitcode;
        (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
        return 0;
    }
    if (g_win32_gui.destroy) {
        g_win32_gui.destroy = false;
        m.hwnd = (uint64_t)g_win32_gui.fd; m.message = WM_DESTROY;
        if (g_win32_log_input) kprintf("[winpe8] GetMessage -> WM_DESTROY\n");
        (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
        return 1;
    }
    if (g_win32_gui.needs_paint) {
        g_win32_gui.needs_paint = false;
        m.hwnd = (uint64_t)g_win32_gui.fd; m.message = WM_PAINT;
        (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
        return 1;
    }
    /* Drain one real input event (mouse/keyboard/close) and translate it. */
    if (g_win32_gui.fd >= 0) {
        struct gui_event ev;
        if (win32_poll_window_event(g_win32_gui.fd, &ev) > 0 &&
            win32_event_to_msg(&ev, &m)) {
            /* Log the meaningful ones (skip the WM_MOUSEMOVE flood). */
            if (g_win32_log_input && m.message != WM_MOUSEMOVE)
                kprintf("[winpe8] GetMessage -> WM_%04x wParam=0x%lx lParam=0x%lx\n",
                        (unsigned)m.message, (unsigned long)m.wParam,
                        (unsigned long)m.lParam);
            (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
            return 1;
        }
    }
    /* Nothing pending: yield and deliver WM_NULL so the app's loop keeps
     * turning (and other procs run) without busy-spinning. */
    sched_yield();
    m.hwnd = (uint64_t)g_win32_gui.fd; m.message = WM_NULL;
    (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
    return 1;
}

/* BeginPaint(hwnd, &PAINTSTRUCT) -> HDC. EndPaint -> present (flip). */
static long w32_BeginPaint(uint64_t *a) {
    uint64_t hwnd = a[0];
    uint64_t ps   = a[1];
    /* PAINTSTRUCT: hdc@0, fErase@8, rcPaint(l,t,r,b)@0x0C */
    uint64_t hdc = hwnd;          /* HDC == HWND == fd */
    uint32_t erase = 1;
    int32_t  rc[4] = { 0, 0, g_win32_gui.w, g_win32_gui.h };
    (void)copy_to_user((void *)(uintptr_t)(ps + 0x00), &hdc, 8);
    (void)copy_to_user((void *)(uintptr_t)(ps + 0x08), &erase, 4);
    (void)copy_to_user((void *)(uintptr_t)(ps + 0x0C), rc, sizeof(rc));
    return (long)hdc;
}
static long w32_EndPaint(uint64_t *a) {
    (void)a;
    if (g_win32_gui.fd >= 0) (void)sys_gui_flip(g_win32_gui.fd);
    return 1;
}

/* COLORREF is 0x00BBGGRR (red in the low byte); tobyOS framebuffer colours are
 * XRGB 0x00RRGGBB. Swap R and B. */
static uint32_t win32_colorref_to_xrgb(uint32_t cr) {
    uint32_t r = cr & 0xFF, g = (cr >> 8) & 0xFF, b = (cr >> 16) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

/* gdi32!CreateSolidBrush(COLORREF) -> HBRUSH. We return a tagged token that
 * carries the colour so FillRect can honour it (no real GDI object table). */
static long w32_CreateSolidBrush(uint64_t *a) {
    return (long)(WIN32_BRUSH_TAG | (uint64_t)((uint32_t)a[0] & 0xFFFFFFu));
}

/* gdi32!DeleteObject(hgdiobj) -> TRUE. Brush tokens carry no allocation. */
static long w32_DeleteObject(uint64_t *a) { (void)a; return 1; }

/* FillRect(hdc, &RECT, hbrush) -> non-zero. Honours a CreateSolidBrush colour
 * (so e.g. a click can repaint the window a new colour); a non-solid/system
 * brush handle falls back to the default fill. The chosen colour is remembered
 * so TextOutA draws its text on a matching background. */
static long w32_FillRect(uint64_t *a) {
    int      fd   = (int)a[0];
    uint64_t prc  = a[1];
    uint64_t hbr  = a[2];
    int32_t  rc[4] = { 0, 0, 0, 0 };
    if (copy_from_user(rc, (const void *)(uintptr_t)prc, sizeof(rc)) != 0) return 0;
    int x = rc[0], y = rc[1], w = rc[2] - rc[0], h = rc[3] - rc[1];
    if (w < 0) w = 0; if (h < 0) h = 0;
    uint32_t color = WIN32_FILL_DEFAULT;
    if ((hbr & WIN32_BRUSH_MASK) == WIN32_BRUSH_TAG)
        color = win32_colorref_to_xrgb((uint32_t)(hbr & 0xFFFFFFu));
    g_win32_gui.fill_color = color;
    uint32_t whlen = ((uint32_t)(w & 0xFFFF)) | ((uint32_t)(h & 0xFFFF) << 16);
    (void)sys_gui_fill(fd, x, y, whlen, color);
    return 1;
}

/* gdi32!TextOutA(hdc, x, y, str, len) -> non-zero. Drawn white on the window's
 * current fill colour (tracked by FillRect) so the label stays legible after a
 * recolour. */
static long w32_TextOutA(uint64_t *a) {
    int      fd  = (int)a[0];
    int      x   = (int)(int32_t)a[1];
    int      y   = (int)(int32_t)a[2];
    uint64_t str = a[3];
    uint32_t xy  = ((uint32_t)(x & 0xFFFF)) | ((uint32_t)(y & 0xFFFF) << 16);
    (void)sys_gui_text(fd, xy, (const char *)(uintptr_t)str,
                       0x00FFFFFF /* white */, g_win32_gui.fill_color);
    return 1;
}

/* kernel32!GetStartupInfoA(&STARTUPINFOA): a zeroed struct + cb. */
static long w32_GetStartupInfoA(uint64_t *a) {
    uint64_t si = a[0];
    uint8_t  zero[0x68];
    memset(zero, 0, sizeof(zero));
    uint32_t cb = sizeof(zero);
    memcpy(zero, &cb, 4);     /* STARTUPINFOA.cb @ +0 */
    (void)copy_to_user((void *)(uintptr_t)si, zero, sizeof(zero));
    return 0;
}

/* crt!__p__acmdln() -> char** (a pointer into the CRT-data page whose slot
 * holds the command-line string pointer). */
static long w32_p_acmdln(uint64_t *a) { (void)a; return (long)(WIN32_CRT_DATA_BASE + WIN32_CRT_ACMDLN); }

/* kernel32!Sleep(ms) -> real sleep (drops the BKL while waiting). */
static long w32_Sleep(uint64_t *a) {
    sys_nanosleep((uint64_t)(uint32_t)a[0] * 1000000ull);
    return 0;
}

/* memset over user memory. */
static long w32_memset(uint64_t *a) {
    uint64_t dst = a[0]; int v = (int)a[1]; uint64_t n = a[2], done = 0;
    char buf[256];
    memset(buf, (unsigned char)v, sizeof(buf) < n ? sizeof(buf) : n);
    while (done < n) {
        uint64_t chunk = n - done; if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (copy_to_user((void *)(uintptr_t)(dst + done), buf, chunk) != 0) break;
        done += chunk;
    }
    return (long)dst;
}

/* ---- C9: runtime API resolution (GetModuleHandle / GetProcAddress / LoadLibrary) ----
 * Real software resolves APIs at runtime by name, not just via the static IAT.
 * tobyOS has no real DLLs -- it IS the Win32 implementation -- so a module
 * HANDLE is a tagged token carrying a shim-table index whose .dll names the
 * module, and GetProcAddress returns the address of the loader-generated
 * marshalling thunk for the resolved shim (callable in CPL3 like any import). */
#define WIN32_HMODULE_TAG   0x7D00000000000000ull  /* distinct from brush 0x7B / thread 0x74 / FILE* 0xF11E */
#define WIN32_HMODULE_MASK  0xFF00000000000000ull
#define WIN32_PE_IMAGE_BASE 0x0000000140000000ull  /* all tobyOS PEs load here (delta 0) */

static int         win32_dll_first_index(const char *dll); /* defined after the table */
static const char *win32_shim_dll(int idx);                /* defined after the table */

/* kernel32!GetCurrentProcessId() -> the caller's pid. Observable, so the C9
 * test can prove a GetProcAddress'd pointer actually reaches the shim. */
static long w32_GetCurrentProcessId(uint64_t *a) {
    (void)a;
    struct proc *p = current_proc();
    return p ? (long)(p->is_thread ? p->tgid : p->pid) : 0;
}

/* kernel32!GetModuleHandleA(name) -> HMODULE. NULL name = this module (the
 * exe's ImageBase). A named DLL we provide -> a tagged handle carrying a shim
 * index that maps back to the DLL name; an unknown DLL -> NULL. */
static long w32_GetModuleHandleA(uint64_t *a) {
    uint64_t name = a[0];
    if (name == 0) return (long)WIN32_PE_IMAGE_BASE;
    char dll[64];
    if (strncpy_from_user(dll, (const char *)(uintptr_t)name, sizeof(dll)) < 0)
        return 0;
    int idx = win32_dll_first_index(dll);
    if (idx < 0) return 0;                       /* NULL: not a DLL we implement */
    return (long)(WIN32_HMODULE_TAG | (uint64_t)(uint32_t)idx);
}

/* kernel32!LoadLibraryA(name) -> HMODULE. Nothing to actually load (tobyOS *is*
 * the implementation); same resolution as GetModuleHandleA, NULL if unknown. */
static long w32_LoadLibraryA(uint64_t *a) { return w32_GetModuleHandleA(a); }

/* kernel32!FreeLibrary(hModule) -> TRUE (no real DLL refcount to drop). */
static long w32_FreeLibrary(uint64_t *a) { (void)a; return 1; }

/* kernel32!GetProcAddress(hModule, lpProcName) -> FARPROC. Resolves the function
 * by name within the module's DLL and returns the address of its pre-generated
 * marshalling thunk. Ordinal imports (MAKEINTRESOURCE) and unknown names -> NULL. */
static long w32_GetProcAddress(uint64_t *a) {
    uint64_t hmod  = a[0];
    uint64_t pname = a[1];
    if ((hmod & WIN32_HMODULE_MASK) != WIN32_HMODULE_TAG)
        return 0;                                /* exe base / unknown handle */
    if (pname == 0 || (pname >> 16) == 0)
        return 0;                                /* NULL or an ordinal */
    const char *dll = win32_shim_dll((int)(uint32_t)(hmod & 0xFFFFFFFFu));
    if (!dll) return 0;
    char fn[96];
    if (strncpy_from_user(fn, (const char *)(uintptr_t)pname, sizeof(fn)) < 0)
        return 0;
    int idx = win32_shim_index(dll, fn);
    if (idx < 0 || idx >= win32_shim_count()) return 0;
    return (long)(WIN32_PROCADDR_BASE_VA + (uint64_t)idx * WIN32_PROCADDR_STRIDE);
}

typedef long (*win32_shim_fn)(uint64_t *args);

struct win32_shim {
    const char    *dll;     /* lower-case DLL name, no path */
    const char    *func;    /* exact exported symbol */
    win32_shim_fn  fn;
};

/* The kernel32 subset for C1. Index into this table is what the PE loader
 * bakes into each IAT thunk; keep it append-only so existing thunks stay
 * valid. */
static const struct win32_shim g_win32_shims[] = {
    { "kernel32.dll", "GetStdHandle", w32_GetStdHandle },
    { "kernel32.dll", "WriteFile",    w32_WriteFile },
    { "kernel32.dll", "ExitProcess",  w32_ExitProcess },
    /* C2: the C-runtime printf path. The DLL name is one of the ucrt API
     * sets ("api-ms-win-crt-stdio-l1-1-0.dll"); match is case-insensitive. */
    { "api-ms-win-crt-stdio-l1-1-0.dll", "__acrt_iob_func",          w32_acrt_iob_func },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "__stdio_common_vfprintf",  w32_stdio_common_vfprintf },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "puts",                     w32_puts },

    /* ---- C3: the rest of the stock-clang mainCRTStartup surface ---- */
    /* kernel32. The critical-section funcs are REAL as of C6 (mutual
     * exclusion); DeleteCriticalSection stays a no-op. */
    { "kernel32.dll", "InitializeCriticalSection", w32_InitializeCriticalSection },
    { "kernel32.dll", "DeleteCriticalSection",     w32_zero },
    { "kernel32.dll", "EnterCriticalSection",      w32_EnterCriticalSection },
    { "kernel32.dll", "LeaveCriticalSection",      w32_LeaveCriticalSection },
    { "kernel32.dll", "GetLastError",              w32_zero },
    { "kernel32.dll", "SetUnhandledExceptionFilter", w32_zero },
    { "kernel32.dll", "Sleep",                     w32_Sleep },
    { "kernel32.dll", "TlsGetValue",               w32_zero },
    { "kernel32.dll", "VirtualProtect",            w32_one },
    { "kernel32.dll", "VirtualQuery",              w32_VirtualQuery },
    /* ucrt: environment */
    { "api-ms-win-crt-environment-l1-1-0.dll", "__p__environ",       w32_p_environ },
    /* ucrt: heap */
    { "api-ms-win-crt-heap-l1-1-0.dll", "malloc",                    w32_malloc },
    { "api-ms-win-crt-heap-l1-1-0.dll", "calloc",                    w32_calloc },
    { "api-ms-win-crt-heap-l1-1-0.dll", "free",                      w32_zero },
    { "api-ms-win-crt-heap-l1-1-0.dll", "_set_new_mode",             w32_zero },
    /* ucrt: locale / math / private */
    { "api-ms-win-crt-locale-l1-1-0.dll", "_configthreadlocale",     w32_zero },
    { "api-ms-win-crt-math-l1-1-0.dll", "__setusermatherr",          w32_zero },
    { "api-ms-win-crt-private-l1-1-0.dll", "__C_specific_handler",   w32_zero },
    { "api-ms-win-crt-private-l1-1-0.dll", "memcpy",                 w32_memcpy },
    { "api-ms-win-crt-private-l1-1-0.dll", "memcmp",                 w32_memcmp },
    /* ucrt: runtime (the startup core) */
    { "api-ms-win-crt-runtime-l1-1-0.dll", "__p___argc",             w32_p_argc },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "__p___argv",             w32_p_argv },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_configure_narrow_argv", w32_zero },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_initialize_narrow_environment", w32_zero },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_get_initial_narrow_environment", w32_p_environ },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_set_app_type",          w32_zero },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_set_invalid_parameter_handler", w32_zero },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_crt_atexit",            w32_zero },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_cexit",                 w32_zero },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_initterm",              w32_initterm },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_initterm_e",            w32_initterm },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "exit",                   w32_exit },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_exit",                  w32_exit },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "abort",                  w32_abort },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "signal",                 w32_zero },
    /* ucrt: stdio (extras beyond the C2 set) */
    { "api-ms-win-crt-stdio-l1-1-0.dll", "__p__commode",             w32_p_commode },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "__p__fmode",               w32_p_fmode },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fflush",                   w32_zero },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "setvbuf",                  w32_zero },
    /* ucrt: string */
    { "api-ms-win-crt-string-l1-1-0.dll", "strlen",                  w32_strlen },
    { "api-ms-win-crt-string-l1-1-0.dll", "strncmp",                 w32_strncmp },

    /* ---- C4: the extra ucrt surface a C++ .exe pulls in ---- */
    /* convert (multibyte/wide -- not on the integer-printf path; minimal) */
    { "api-ms-win-crt-convert-l1-1-0.dll", "mbrtowc",                w32_one },
    { "api-ms-win-crt-convert-l1-1-0.dll", "wcrtomb",                w32_one },
    /* filesystem: stdio FILE locking -- single-threaded, no-op */
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_lock_file",          w32_zero },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_unlock_file",        w32_zero },
    /* locale */
    { "api-ms-win-crt-locale-l1-1-0.dll", "localeconv",              w32_localeconv },
    /* runtime extras */
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_errno",                 w32_errno },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "strerror",              w32_strerror },
    /* stdio extras */
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fputc",                    w32_fputc },
    /* string extras */
    { "api-ms-win-crt-string-l1-1-0.dll", "strnlen",                 w32_strnlen },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcslen",                  w32_wcslen },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcsnlen",                 w32_wcslen },

    /* ---- C5: file I/O (kernel32) ---- */
    { "kernel32.dll", "CreateFileA",  w32_CreateFileA },
    { "kernel32.dll", "ReadFile",     w32_ReadFile },
    { "kernel32.dll", "CloseHandle",  w32_CloseHandle },

    /* ---- C6: multithreading (kernel32) ---- */
    { "kernel32.dll", "CreateThread",        w32_CreateThread },
    { "kernel32.dll", "WaitForSingleObject", w32_WaitForSingleObject },

    /* ---- C7: the GUI bridge ---- */
    /* kernel32 + crt extras the GUI-subsystem CRT startup pulls in */
    { "kernel32.dll", "GetStartupInfoA", w32_GetStartupInfoA },
    { "kernel32.dll", "IsDBCSLeadByte",  w32_zero },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "__p__acmdln", w32_p_acmdln },
    { "api-ms-win-crt-string-l1-1-0.dll",  "memset",      w32_memset },
    /* user32. NOTE: DispatchMessageA is bound by the loader to a user-mode
     * trampoline (it calls the app's WndProc), so it's deliberately NOT here. */
    { "user32.dll", "RegisterClassA",    w32_RegisterClassA },
    { "user32.dll", "CreateWindowExA",   w32_CreateWindowExA },
    { "user32.dll", "ShowWindow",        w32_ShowWindow },
    { "user32.dll", "UpdateWindow",      w32_UpdateWindow },
    { "user32.dll", "GetMessageA",       w32_GetMessageA },
    { "user32.dll", "TranslateMessage",  w32_TranslateMessage },
    { "user32.dll", "DefWindowProcA",    w32_DefWindowProcA },
    { "user32.dll", "PostQuitMessage",   w32_PostQuitMessage },
    { "user32.dll", "BeginPaint",        w32_BeginPaint },
    { "user32.dll", "EndPaint",          w32_EndPaint },
    { "user32.dll", "FillRect",          w32_FillRect },
    /* gdi32 */
    { "gdi32.dll",  "TextOutA",          w32_TextOutA },

    /* ---- C8: interactive single window (input events + close chain) ---- */
    /* user32: DestroyWindow/InvalidateRect drive the WM_CLOSE->WM_DESTROY and
     * repaint paths; GetMessageA (above) now translates real input -> WM_*. */
    { "user32.dll", "DestroyWindow",     w32_DestroyWindow },
    { "user32.dll", "InvalidateRect",    w32_InvalidateRect },
    /* gdi32: solid brushes so a click can repaint the window a new colour. */
    { "gdi32.dll",  "CreateSolidBrush",  w32_CreateSolidBrush },
    { "gdi32.dll",  "DeleteObject",      w32_DeleteObject },

    /* ---- C9: runtime API resolution (kernel32) ---- */
    { "kernel32.dll", "GetModuleHandleA",    w32_GetModuleHandleA },
    { "kernel32.dll", "GetProcAddress",      w32_GetProcAddress },
    { "kernel32.dll", "LoadLibraryA",        w32_LoadLibraryA },
    { "kernel32.dll", "FreeLibrary",         w32_FreeLibrary },
    { "kernel32.dll", "GetCurrentProcessId", w32_GetCurrentProcessId },
};
#define WIN32_SHIM_COUNT (int)(sizeof(g_win32_shims) / sizeof(g_win32_shims[0]))

/* Case-insensitive compare of a (possibly mixed-case) DLL name against our
 * lower-case table entries. */
static bool win32_dll_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
}

int win32_shim_index(const char *dll, const char *func) {
    if (!dll || !func) return -1;
    for (int i = 0; i < WIN32_SHIM_COUNT; i++) {
        if (win32_dll_eq(dll, g_win32_shims[i].dll) &&
            strcmp(func, g_win32_shims[i].func) == 0)
            return i;
    }
    return -1;
}

int win32_shim_count(void) { return WIN32_SHIM_COUNT; }

/* First shim-table index whose DLL matches `dll` (case-insensitive), or -1. The
 * index doubles as a module token in GetModuleHandleA/GetProcAddress (C9). */
static int win32_dll_first_index(const char *dll) {
    if (!dll) return -1;
    for (int i = 0; i < WIN32_SHIM_COUNT; i++)
        if (win32_dll_eq(dll, g_win32_shims[i].dll)) return i;
    return -1;
}

/* DLL name for a shim-table index (recovers a module's DLL from its handle
 * token), or NULL if out of range (C9). */
static const char *win32_shim_dll(int idx) {
    if (idx < 0 || idx >= WIN32_SHIM_COUNT) return 0;
    return g_win32_shims[idx].dll;
}

long win32_dispatch(uint64_t func_index, uint64_t args_ptr) {
    if ((int)func_index >= WIN32_SHIM_COUNT) {
        kprintf("[win32] bad shim index %lu\n", (unsigned long)func_index);
        return -1;
    }
    uint64_t args[8];
    if (copy_from_user(args, (const void *)(uintptr_t)args_ptr, sizeof(args)) != 0) {
        kprintf("[win32] dispatch: bad args_ptr %p\n", (void *)(uintptr_t)args_ptr);
        return -1;
    }
    return g_win32_shims[func_index].fn(args);
}

/* ---- C8 harness accessors ----
 * Let the boot harness observe the single Win32 GUI window without reaching
 * into g_win32_gui directly. win32_gui_window_fd returns the live window fd for
 * a given process (-1 if that process has no window yet); win32_gui_fill_color
 * reports the current FillRect colour so the harness can confirm a click
 * actually recoloured the window (real-mouse vs deterministic injection). */
int win32_gui_window_fd(int tgid) {
    if (g_win32_gui.tgid == tgid && g_win32_gui.fd >= 0) return g_win32_gui.fd;
    return -1;
}
uint32_t win32_gui_fill_color(void) { return g_win32_gui.fill_color; }

/* Win32 personality translator -- the mirror of linux_syscall(). A PE only
 * ever issues the marshalling gate's ABI_SYS_WIN32_DISPATCH; anything else
 * is unexpected and falls through to the native dispatcher (which will
 * report it). */
static long win32_syscall(long n, long a1, long a2, long a3, long a4, long a5) {
    if (n == ABI_SYS_WIN32_DISPATCH)
        return win32_dispatch((uint64_t)a1, (uint64_t)a2);
    /* The C6 thread wrapper issues a raw ABI_SYS_THREAD_EXIT when its thread
     * function returns -- that's expected; forward it (and anything else) to
     * the native dispatcher, noting only the genuinely-unexpected ones. */
    if (n != ABI_SYS_THREAD_EXIT)
        kprintf("[win32] raw syscall %ld from a PE process -> native dispatch\n", n);
    return do_syscall(n, a1, a2, a3, a4, a5);
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

    /* Track B/C: a process carrying a foreign-binary personality routes its
     * `num` through the matching translation layer. ABI_PERS_LINUX speaks the
     * Linux x86-64 syscall ABI; ABI_PERS_WIN32 is a loaded Windows PE whose
     * only syscall is the Win32 marshalling gate. The native path (the common
     * case, personality 0) is completely unchanged. */
    long rv;
    if (caller && caller->personality == ABI_PERS_LINUX)
        rv = linux_syscall(num, a1, a2, a3, a4, a5);
    else if (caller && caller->personality == ABI_PERS_WIN32)
        rv = win32_syscall(num, a1, a2, a3, a4, a5);
    else
        rv = do_syscall(num, a1, a2, a3, a4, a5);

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
    /* SWAPGS shadow default = &percpu so the first ring transitions are
     * identity swaps until a real user proc (with its own user_gs) runs;
     * do_switch overwrites this per proc. */
    wrmsr(IA32_KERNEL_GS_BASE, (uint64_t)pc);

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
