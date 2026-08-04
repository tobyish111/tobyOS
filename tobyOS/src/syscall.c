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
#include <tobyos/kmath.h>      /* C20: kernel libm for the float-return shims */
#include <tobyos/proc.h>
#include <tobyos/file.h>
#include <tobyos/mmap.h>   /* memfd_* (memfd_create) */
#include <tobyos/pipe.h>
#include <tobyos/sched.h>
#include <tobyos/signal.h>
#include <tobyos/socket.h>
#include <tobyos/tcp.h>
#include <tobyos/dns.h>
#include <tobyos/rtc.h>
#include <tobyos/gui.h>
#include <tobyos/gfx.h>
#include <tobyos/virtio_gpu.h>
#include <tobyos/linux_drm.h>
#include <tobyos/term.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/rng.h>   /* getrandom + /dev/urandom entropy */
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
#include <tobyos/page_fault.h>   /* get_pte / PTE_* -- deadlocked-thread stack walk */
#include <tobyos/devtest.h>
#include <tobyos/blk.h>
#include <tobyos/provision.h>
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
#include <tobyos/evdev.h>
#include <tobyos/tty.h>
#include <tobyos/pty.h>
#include <tobyos/usb_legacy.h>
#include <tobyos/xhci.h>
#include <tobyos/http.h>
#include <tobyos/http_async.h>
#include <tobyos/ws.h>
#include <tobyos/inotify.h>
#include <tobyos/clipboard.h>
#include <tobyos/smp.h>
#include <tobyos/pe.h>
#include <tobyos/kfont.h>   /* C14b: kernel TrueType text for the GDI shims */

extern void syscall_entry(void);

/* Phase 3 M3.2: fork/exec forward declarations */
extern long sys_fork(void);
extern long sys_fork_share(void);
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
extern long sys_shmget(int key, size_t size, int shmflg);
extern long sys_shmat(int shmid, uint64_t shmaddr, int shmflg);
extern long sys_shmdt(uint64_t shmaddr);
extern long sys_shmctl(int shmid, int cmd, void *buf);
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

/* CLONE_FILES: a thread SHARES the thread-group leader's fd table, so an fd
 * created on one thread is visible to all threads. Non-threads use their own.
 * Without this, Chromium's IO thread creates the epoll/socket fds and worker
 * threads can't see them -> fd_lookup returns NULL -> NULL handle -> the process
 * calls through a NULL pointer and dies (a timing-dependent crash). RUNTIME fd
 * accessors route through here; lifecycle paths (spawn init, exit cleanup, fork)
 * stay per-proc, and a clone-thread's own fds[] is left empty (see
 * sys_clone_thread) so its exit never closes the shared open descriptions. */
struct file **proc_fds(struct proc *p) {
    if (p && p->is_thread) {
        struct proc *ld = proc_lookup(p->tgid);
        if (ld) return ld->fds;
    }
    return p ? p->fds : 0;
}

static struct file *fd_lookup(int fd) {
    if (fd < 0 || fd >= PROC_NFDS) return 0;
    struct file **t = proc_fds(current_proc());
    return t ? t[fd] : 0;
}

static int fd_alloc_into(struct proc *p, struct file *f) {
    struct file **t = proc_fds(p);
    if (!t) return -1;
    for (int i = 0; i < PROC_NFDS; i++) {
        if (!t[i]) {
            t[i] = f;
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
#ifdef CHROMIUM_BOOT
    /* slice 20: does chrome ever write the --dump-dom output to stdout (fd 1)?
     * If yes we should see "<html.../<h1>tobyOS"; if there are no [stdout] lines
     * at all, chrome never reaches the DOM dump (navigation never completes). */
    /* Slice 69: fd 2 as well. chrome reports its OWN fatal reasons on
     * STDERR ("error while loading shared libraries", sandbox/Ozone
     * complaints, CHECK failures); logging only fd 1 left an exit_group(21)
     * with no explanation anywhere. Wider window (120 chars) because those
     * messages carry the useful detail at the end. */
    if (fd == 1 || fd == 2) {
        static int w1 = 0;
        /* Slice 70: 160 -> 600 lines, 120 -> 200 chars. With --v=1 the
         * startup chatter is long and the interesting line is late. */
        if (w1 < 600) { w1++;
            char pre[201]; size_t n = len < 200 ? len : 200;
            memcpy(pre, k, n); pre[n] = '\0';
            for (size_t i = 0; i < n; i++)
                if (pre[i] == '\n' || pre[i] == '\r') pre[i] = ' ';
            kprintf("[fd%d] len=%lu: %s\n", fd, (unsigned long)len, pre);
        }
    }
#endif
    long rv = file_write(f, k, len);
    kfree(k);
    return rv;
}

static long evdev_read(unsigned dev, uint64_t ubuf, size_t len); /* evdev dev, below */

#ifdef CHROMIUM_BOOT
/* Slice 38 iter 12 [rdchk]: the netlog proved chrome SENT the fatal
 * protocol_version alert (SSL_ALERT_SENT 02 46) right after a 511-byte read,
 * while the ring-exit framing tracker saw the SAME stream leave rx_pop with
 * clean 17/0303 record headers. BoringSSL only sends alert 70 when a RECORD
 * HEADER parses wrong, so the bytes must differ between the kernel bounce
 * buffer and BoringSSL's parser. The one kernel step in between is
 * copy_to_user into chrome's (demand-paged, PartitionAlloc-fresh) read
 * buffer. Verify it: after every 443-socket read, copy the user buffer BACK
 * and memcmp against the bounce buffer. A mismatch names the kernel
 * (copy_to_user / demand-page / CoW frame swap); a clean compare exiles the
 * corruption into chrome's address space. Also log every read's (cap, rv):
 * the 511-vs-512 ledger discrepancy resolves here too. */
static void rdchk_verify(struct tcp_conn *tc, int fd, const void *kbuf,
                         uint64_t ubuf, long rv, size_t cap) {
    static int rd_logs;
    if (!tc) return;
    uint16_t pbe = tcp_remote_port_be(tc);
    uint16_t rp  = (uint16_t)((pbe >> 8) | (pbe << 8));
    if (rp != 443) return;
    if (rd_logs >= 96) return;
    rd_logs++;
    struct proc *me = current_proc();
    if (rv <= 0) {
        kprintf("[rdchk] pid=%d fd=%d cap=%lu rv=%ld\n",
                me ? me->pid : -1, fd, (unsigned long)cap, rv);
        return;
    }
    size_t vn = (size_t)rv;
    if (vn > 4096) vn = 4096;
    void *vb = kmalloc(vn);
    if (!vb) return;
    long bad = -1;
    if (copy_from_user_nofault(vb, (const void *)(uintptr_t)ubuf, vn) != 0) {
        bad = -2;                      /* user buffer unreadable?! */
    } else {
        for (size_t i = 0; i < vn; i++)
            if (((const uint8_t *)vb)[i] != ((const uint8_t *)kbuf)[i]) {
                bad = (long)i;
                break;
            }
    }
    if (bad == -1) {
        kprintf("[rdchk] pid=%d fd=%d cap=%lu rv=%ld verify=OK\n",
                me ? me->pid : -1, fd, (unsigned long)cap, rv);
    } else if (bad == -2) {
        kprintf("[rdchk] pid=%d fd=%d cap=%lu rv=%ld verify=UNREADABLE ubuf=%p\n",
                me ? me->pid : -1, fd, (unsigned long)cap, rv,
                (void *)(uintptr_t)ubuf);
    } else {
        kprintf("[rdchk] pid=%d fd=%d cap=%lu rv=%ld MISMATCH at +%ld ubuf=%p\n",
                me ? me->pid : -1, fd, (unsigned long)cap, rv, bad,
                (void *)(uintptr_t)ubuf);
        size_t w0 = (bad >= 8) ? (size_t)bad - 8 : 0;
        size_t wn = vn - w0 > 24 ? 24 : vn - w0;
        char hx[49];
        static const char hc[] = "0123456789abcdef";
        for (size_t i = 0; i < wn; i++) {
            hx[i*2]   = hc[((const uint8_t *)kbuf)[w0+i] >> 4];
            hx[i*2+1] = hc[((const uint8_t *)kbuf)[w0+i] & 0xf];
        }
        hx[wn*2] = 0;
        kprintf("[rdchk]   kernel+%lu: %s\n", (unsigned long)w0, hx);
        for (size_t i = 0; i < wn; i++) {
            hx[i*2]   = hc[((const uint8_t *)vb)[w0+i] >> 4];
            hx[i*2+1] = hc[((const uint8_t *)vb)[w0+i] & 0xf];
        }
        hx[wn*2] = 0;
        kprintf("[rdchk]   user  +%lu: %s\n", (unsigned long)w0, hx);
    }
    kfree(vb);
}
#endif

static long sys_read(int fd, void *buf, size_t len) {
    if (len == 0) return 0;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) return -ABI_EFAULT;
    struct file *f = fd_lookup(fd);
    if (!f) return -1;
    if (f->kind == FILE_KIND_EVDEV)      /* /dev/input/event<N>: input_event stream */
        return evdev_read((unsigned)f->dir_off, (uint64_t)(uintptr_t)buf, len);
    void *k = kmalloc(len);
    if (!k) return -ABI_ENOMEM;
    long rv = file_read(f, k, len);
    if (rv > 0 && copy_to_user(buf, k, (size_t)rv) != 0) rv = -ABI_EFAULT;
#ifdef CHROMIUM_BOOT
    if (f->kind == FILE_KIND_SOCKET && f->sock &&
        f->sock->kind == SOCK_KIND_TCP && f->sock->tcp && !f->sock->tcp_listening)
        rdchk_verify(f->sock->tcp, fd, k, (uint64_t)(uintptr_t)buf, rv, len);
#endif
    kfree(k);
    return rv;
}

/* Non-blocking read (slice 45, ABI_SYS_READ_NB): on a pipe, drains what is
 * buffered and returns -ABI_EAGAIN instead of parking when momentarily empty.
 * Native apps have no fcntl(O_NONBLOCK)/working poll on pipe fds; chromewin uses
 * this to pump the CDP screencast pipe from its TK loop. Non-pipe fds fall back
 * to the ordinary blocking read (chromewin only calls this on the pipe). */
static long sys_read_nb(int fd, void *buf, size_t len) {
    if (len == 0) return 0;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) return -ABI_EFAULT;
    struct file *f = fd_lookup(fd);
    if (!f) return -1;
    if (f->kind != FILE_KIND_PIPE_R)
        return sys_read(fd, buf, len);           /* only pipes get the NB path */
    void *k = kmalloc(len);
    if (!k) return -ABI_ENOMEM;
    long rv = pipe_tryread(f->pipe, k, len);     /* >0 / 0 EOF / -ABI_EAGAIN */
    if (rv > 0 && copy_to_user(buf, k, (size_t)rv) != 0) rv = -ABI_EFAULT;
    kfree(k);
    return rv;
}

/* pread64/pwrite64: positioned I/O that does NOT disturb the file's current
 * offset. glibc's dynamic loader (dl-load.c open_verify) pread64()s the ELF
 * header + program headers of every shared object it maps, and real tools like
 * GNU readelf pread64() the file they inspect, so this is on the critical path
 * for off-the-shelf dynamic Linux binaries. Only regular (seekable) VFS files
 * support it; pipes/sockets/ttys are unseekable -> ESPIPE, matching Linux. */
static long sys_pread64(int fd, void *buf, size_t len, int64_t offset) {
    if (len == 0) return 0;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (offset < 0) return -ABI_EINVAL;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, len)) return -ABI_EFAULT;
    struct file *f = fd_lookup(fd);
    if (!f) return -ABI_EBADF;
    if (f->kind != FILE_KIND_VFS) return -ABI_ESPIPE;
    void *k = kmalloc(len);
    if (!k) return -ABI_ENOMEM;
    size_t saved = f->vfs.pos;
    f->vfs.pos = (size_t)offset;
    long rv = file_read(f, k, len);
    f->vfs.pos = saved;          /* positioned read must not move the offset */
    if (rv > 0 && copy_to_user(buf, k, (size_t)rv) != 0) rv = -ABI_EFAULT;
    kfree(k);
    return rv;
}

static long sys_pwrite64(int fd, const void *buf, size_t len, int64_t offset) {
    if (len == 0) return 0;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    if (offset < 0) return -ABI_EINVAL;
    struct file *f = fd_lookup(fd);
    if (!f) return -ABI_EBADF;
    if (f->kind != FILE_KIND_VFS) return -ABI_ESPIPE;
    void *k = bounce_in(buf, len);
    if (!k) return -ABI_EFAULT;
    size_t saved = f->vfs.pos;
    f->vfs.pos = (size_t)offset;
    long rv = file_write(f, k, len);
    f->vfs.pos = saved;
    kfree(k);
    return rv;
}

/* SYS_PIPE: returns two fds. user_fds_out points at int[2] in userspace. */
static long sys_pipe(int *user_fds_out) {
    struct file *r = 0, *w = 0;
    if (pipe_create(&r, &w) != 0) return -1;

    struct proc *p = current_proc();
    struct file **t = proc_fds(p);
    int fd_r = fd_alloc_into(p, r);
    if (fd_r < 0) { file_close(r); file_close(w); return -1; }
    int fd_w = fd_alloc_into(p, w);
    if (fd_w < 0) { t[fd_r] = 0; file_close(r); file_close(w); return -1; }

    int fds[2] = { fd_r, fd_w };
    if (copy_to_user(user_fds_out, fds, sizeof(fds)) != 0) {
        t[fd_r] = 0; t[fd_w] = 0;
        file_close(r); file_close(w);
        return -ABI_EFAULT;
    }
    return 0;
}

static long sys_close(int fd) {
    if (fd < 0 || fd >= PROC_NFDS) return -1;
    struct file **t = proc_fds(current_proc());
    if (!t || !t[fd]) return -1;
    file_close(t[fd]);
    t[fd] = 0;
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
    f->sock_gen = sock_generation(s);        /* slice 89 */

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

/* Native TrueType text via the kernel kfont rasterizer (the same one the Win32
 * GDI path uses). a5 packs px (low 16) and face (high 16, KFONT_*). The glyphs
 * are alpha-blended onto the window backbuf -- the toolkit fills the widget
 * background first -- so there is no bg argument. Returns the advance width.
 * Falls back to the smoothed scaled-bitmap path if no font is loaded so text
 * never silently vanishes. */
static long sys_gui_text_ttf(int fd, uint32_t xy, const char *s,
                             uint32_t fg, uint32_t px_face) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    char buf[256];
    long n = strncpy_from_user(buf, s, sizeof(buf));
    if (n < 0) return -1;
    int x    = (int)(int16_t)(xy & 0xFFFFu);
    int y    = (int)(int16_t)((xy >> 16) & 0xFFFFu);
    int px   = (int)(px_face & 0xFFFFu);
    int face = (int)((px_face >> 16) & 0xFFu);
    if (px <= 0)  px = 16;
    if (px > 256) px = 256;
    if (!kfont_available()) {
        int scale = px >= 24 ? 3 : (px >= 14 ? 2 : 1);
        return gui_window_text_scaled(f->win, x, y, buf, fg, GFX_TRANSPARENT, scale, 1);
    }
    return kfont_draw_window_f(f->win, x, y, buf, (int)n, fg, px, face);
}

/* Companion metrics query so the toolkit can centre/right-align TTF text.
 * a1=string, a2=(px & 0xFFFF)|(face<<16). Returns the pixel advance width. */
static long sys_gui_text_ttf_width(const char *s, uint32_t px_face) {
    char buf[256];
    long n = strncpy_from_user(buf, s, sizeof(buf));
    if (n < 0) return -1;
    int px   = (int)(px_face & 0xFFFFu);
    int face = (int)((px_face >> 16) & 0xFFu);
    if (px <= 0)  px = 16;
    if (px > 256) px = 256;
    if (!kfont_available()) {
        int scale = px >= 24 ? 3 : (px >= 14 ? 2 : 1);
        return (long)n * 8 * scale;
    }
    return kfont_text_width_f(buf, (int)n, px, face);
}

/* Register a downloaded TTF/OTF font blob (stage 13E web fonts).
 * a1 = user font bytes, a2 = length. Returns a face id or -ABI_E*. */
static long sys_gui_font_register(const void *ubuf, uint32_t len) {
    if (!ubuf || len < 12 || len > (2u << 20)) return -ABI_EINVAL;
    if (!user_buf_ok((uint64_t)(uintptr_t)ubuf, len)) return -ABI_EFAULT;
    void *kb = kmalloc(len);
    if (!kb) return -ABI_ENOMEM;
    if (copy_from_user(kb, ubuf, len) != 0) { kfree(kb); return -ABI_EFAULT; }
    int face = kfont_register((const unsigned char *)kb, len);
    kfree(kb);                             /* kfont keeps its own copy */
    return face >= 0 ? (long)face : -ABI_EINVAL;
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
    return gui_window_text(f->win, x, y, buf, fg, bg);   /* native apps: 8x8 bitmap */
}

static long sys_gui_flip(int fd) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("syscall: gui_flip fd=%d", fd);
    }
    return gui_window_flip(f->win);
}

/* Per-widget dirty tracking: flip only a client-relative sub-rect.
 * a2 = (x&0xFFFF)|(y<<16), a3 = (w&0xFFFF)|(h<<16), int16 client coords. */
static long sys_gui_flip_rect(int fd, long a2, long a3) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
    int x = (int)(int16_t)(a2 & 0xFFFF);
    int y = (int)(int16_t)((a2 >> 16) & 0xFFFF);
    int w = (int)(int16_t)(a3 & 0xFFFF);
    int h = (int)(int16_t)((a3 >> 16) & 0xFFFF);
    return gui_window_flip_rect(f->win, x, y, w, h);
}

/* Rasterize a text run into a user coverage buffer via the kernel TTF
 * rasterizer (css-anim slice 3: browser compositing layers need
 * pixel-identical offscreen text). Renders into a kernel scratch, then
 * one copy_to_user. Buffer capped so a bad request can't kmalloc the
 * world. */
#define TTF_RASTER_MAX (512 * 1024)
static long sys_gui_text_ttf_raster(struct abi_ttf_raster *ureq) {
    if (!cap_check(current_proc(), CAP_GUI, "sys_gui_text_ttf_raster"))
        return -ABI_EPERM;
    if (!ureq) return -ABI_EINVAL;
    struct abi_ttf_raster req;
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) return -ABI_EFAULT;
    if (!req.cov || !req.s || req.w <= 0 || req.h <= 0) return -ABI_EINVAL;
    if ((long)req.w * req.h > TTF_RASTER_MAX) return -ABI_EINVAL;
    int px = req.px;
    if (px <= 0)  px = 16;
    if (px > 256) px = 256;
    char buf[256];
    long n = strncpy_from_user(buf, (const char *)(uintptr_t)req.s,
                               sizeof(buf));
    if (n < 0) return -ABI_EFAULT;
    if (req.len >= 0 && req.len < n) n = req.len;
    size_t sz = (size_t)req.w * (size_t)req.h;
    uint8_t *scratch = kmalloc(sz);
    if (!scratch) return -ABI_ENOMEM;
    memset(scratch, 0, sz);
    int adv = kfont_raster_cov(scratch, req.w, req.h, req.x, req.y,
                               buf, (int)n, px, req.face);
    long rc = adv;
    if (copy_to_user((void *)(uintptr_t)req.cov, scratch, sz) != 0)
        rc = -ABI_EFAULT;
    kfree(scratch);
    return rc;
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
/* SYS_HTTP_FETCH downloads the whole body kernel-side (transient
 * kmalloc), then truncates to the caller's buffer. 8 MiB: a single
 * modern stylesheet bundle is routinely megabytes (youtube.com ships
 * 3.4 MiB of CSS in one file) and this ceiling silently overrode the
 * caller's larger buf_sz, truncating scripts/sheets into garbage. */
#define HTTP_FETCH_KERNEL_MAX  (8u << 20)

static int http_is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

/* Append src to dst[*pos] within cap; returns 0, or -1 if it didn't fit. */
static int url_append(char *dst, size_t cap, size_t *pos, const char *src) {
    size_t l = strlen(src);
    if (*pos + l + 1 > cap) return -1;
    memcpy(dst + *pos, src, l);
    *pos += l;
    dst[*pos] = 0;
    return 0;
}

/* Case-insensitive prefix match (klibc has no strncasecmp). */
static int http_ci_prefix(const char *s, const char *prefix) {
    for (int i = 0; prefix[i]; i++) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Resolve a redirect Location against the URL that produced it. Real
 * sites routinely send relative forms ("/en/", "//host/p", "next.html"),
 * which the old verbatim copy turned into a Bad-URL failure. */
static int http_resolve_location(const char *cur, const char *loc,
                                 char *out, size_t cap) {
    /* Absolute: use as-is. */
    if (http_ci_prefix(loc, "http://") || http_ci_prefix(loc, "https://")) {
        size_t l = strlen(loc);
        if (l + 1 > cap) return -1;
        memcpy(out, loc, l + 1);
        return 0;
    }

    struct http_url u;
    if (http_parse_url(cur, &u) != 0) return -1;

    size_t pos = 0;
    if (url_append(out, cap, &pos, u.tls ? "https:" : "http:")) return -1;

    /* Protocol-relative: scheme + "//host/path". */
    if (loc[0] == '/' && loc[1] == '/')
        return url_append(out, cap, &pos, loc);

    if (url_append(out, cap, &pos, "//")) return -1;
    if (url_append(out, cap, &pos, u.host)) return -1;
    if (u.port != (u.tls ? 443 : 80)) {
        char pbuf[8];
        int  pi = 0;
        uint16_t v = u.port;
        char rev[6]; int ri = 0;
        do { rev[ri++] = (char)('0' + v % 10); v /= 10; } while (v);
        pbuf[pi++] = ':';
        while (ri > 0) pbuf[pi++] = rev[--ri];
        pbuf[pi] = 0;
        if (url_append(out, cap, &pos, pbuf)) return -1;
    }

    if (loc[0] == '/')                      /* root-relative */
        return url_append(out, cap, &pos, loc);

    /* Path-relative: replace everything after the last '/' of the path. */
    int last_slash = 0;
    for (int i = 0; u.path[i]; i++)
        if (u.path[i] == '/') last_slash = i;
    u.path[last_slash + 1] = 0;
    if (url_append(out, cap, &pos, u.path)) return -1;
    return url_append(out, cap, &pos, loc);
}

/* GET cur_url following up to HTTP_MAX_REDIRECTS redirects. cur_url is
 * rewritten in place to the URL that produced the final response (so
 * callers can report the post-redirect address). On 0, *resp is live
 * and the caller must http_free() it. timeout_ms is per-recv (0 =
 * HTTP_DEFAULT_TIMEOUT_MS); the async worker passes a patient one. */
int http_get_follow(char cur_url[512], size_t max_body, unsigned flags,
                    uint32_t timeout_ms, struct http_response *resp) {
    for (int redir = 0; redir <= HTTP_MAX_REDIRECTS; redir++) {
        int rc = http_get_opt(cur_url, max_body, timeout_ms, flags, resp);
        if (rc < 0) return rc;

        if (http_is_redirect(resp->status) && resp->location[0]) {
            char next[512];
            if (http_resolve_location(cur_url, resp->location,
                                      next, sizeof(next)) == 0) {
                kprintf("[http] %d redirect -> %s\n", resp->status, next);
                memcpy(cur_url, next, strlen(next) + 1);
                http_free(resp);
                continue;
            }
        }
        return 0;
    }
    return HTTP_ERR_PROTOCOL;                /* redirect loop */
}

/* Like http_get_follow but with fetch()/XHR request parameters. Only
 * GET/HEAD follow redirects transparently (safe, idempotent); other
 * methods return the 3xx so the caller (fetch) can decide. */
int http_request_follow(char cur_url[512], size_t max_body, unsigned flags,
                        uint32_t timeout_ms,
                        const struct http_request_ext *ext,
                        struct http_response *resp) {
    int is_get = !ext || !ext->method || !ext->method[0] ||
                 strcmp(ext->method, "GET") == 0 ||
                 strcmp(ext->method, "HEAD") == 0;
    for (int redir = 0; redir <= HTTP_MAX_REDIRECTS; redir++) {
        int rc = http_get_ext(cur_url, max_body, timeout_ms, flags, ext, resp);
        if (rc < 0) return rc;
        if (is_get && http_is_redirect(resp->status) && resp->location[0]) {
            char next[512];
            if (http_resolve_location(cur_url, resp->location,
                                      next, sizeof(next)) == 0) {
                kprintf("[http] %d redirect -> %s\n", resp->status, next);
                memcpy(cur_url, next, strlen(next) + 1);
                http_free(resp);
                continue;
            }
        }
        return 0;
    }
    return HTTP_ERR_PROTOCOL;
}

static long sys_http_get(const char *url, void *buf, uint32_t buf_sz) {
    if (!cap_check(current_proc(), CAP_NET, "sys_http_get")) return -ABI_EPERM;
    if (!url || !buf || buf_sz == 0) return -ABI_EINVAL;
    if (!user_buf_ok((uint64_t)(uintptr_t)buf, buf_sz)) return -ABI_EFAULT;

    uint32_t cap = buf_sz < HTTP_SYSCALL_MAX_BODY ? buf_sz : HTTP_SYSCALL_MAX_BODY;

    /* Copy URL to kernel buffer so we can follow redirects */
    char cur_url[512];
    if (strncpy_from_user(cur_url, url, sizeof(cur_url)) < 0)
        return -ABI_EFAULT;

    struct http_response resp;
    int rc = http_get_follow(cur_url, (size_t)cap, 0, 0, &resp);
    if (rc < 0) return (long)rc;

    size_t copy = resp.body_len < (size_t)cap ? resp.body_len : (size_t)cap;
    if (copy_to_user(buf, resp.body, copy) != 0) {
        http_free(&resp);
        return -ABI_EFAULT;
    }
    http_free(&resp);
    return (long)copy;
}

/* SYS_HTTP_FETCH: the browser-grade fetch (see abi.h). Downloads up to
 * HTTP_FETCH_KERNEL_MAX kernel-side, truncates into the user buffer,
 * and reports status / total length / final URL / content type. */
static long sys_http_fetch(struct abi_http_fetch *ureq) {
    if (!cap_check(current_proc(), CAP_NET, "sys_http_fetch")) return -ABI_EPERM;
    if (!ureq) return -ABI_EINVAL;

    struct abi_http_fetch req;
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) return -ABI_EFAULT;
    if (req.flags != 0) return -ABI_EINVAL;
    if (!req.url || !req.buf || req.buf_sz == 0) return -ABI_EINVAL;
    if (!user_buf_ok(req.buf, req.buf_sz)) return -ABI_EFAULT;

    char cur_url[512];
    if (strncpy_from_user(cur_url, (const char *)(uintptr_t)req.url,
                          sizeof(cur_url)) < 0)
        return -ABI_EFAULT;

    /* Truncate mode, capped at the caller's buffer: the browser renders
     * at most buf_sz bytes, so downloading past that (a 1.6 MiB
     * Wikipedia article, say) is pure waste -- and the old TOOBIG
     * refusal turned every big page into an error. */
    size_t kmax = (size_t)req.buf_sz;
    if (kmax > HTTP_FETCH_KERNEL_MAX) kmax = HTTP_FETCH_KERNEL_MAX;

    struct http_response resp;
    int rc = http_get_follow(cur_url, kmax,
                             HTTP_F_TRUNCATE | HTTP_F_GZIP | HTTP_F_KEEPALIVE,
                             0, &resp);
    if (rc < 0) return (long)rc;

    size_t copy = resp.body_len < (size_t)req.buf_sz ? resp.body_len
                                                     : (size_t)req.buf_sz;
    if (copy > 0 &&
        copy_to_user((void *)(uintptr_t)req.buf, resp.body, copy) != 0) {
        http_free(&resp);
        return -ABI_EFAULT;
    }

    req.status     = resp.status;
    req.body_total = (resp.content_len >= 0) ? (uint32_t)resp.content_len
                                             : (uint32_t)resp.body_len;
    _Static_assert(sizeof(req.final_url) >= sizeof(cur_url),
                   "abi_http_fetch.final_url must hold a kernel URL");
    memcpy(req.final_url, cur_url, strlen(cur_url) + 1);
    size_t ctl = strlen(resp.content_type);
    if (ctl >= sizeof(req.content_type)) ctl = sizeof(req.content_type) - 1;
    memcpy(req.content_type, resp.content_type, ctl);
    req.content_type[ctl] = 0;
    /* Kernel inflates gzip transparently, so this is normally IDENTITY;
     * report it for completeness / future browser-side handling. */
    req.content_encoding = resp.encoding;
    http_free(&resp);

    if (copy_to_user(ureq, &req, sizeof(req)) != 0) return -ABI_EFAULT;
    return (long)copy;
}

/* ---- Async HTTP (stage 12D; see http_async.c) -------------------- */

static long sys_http_start(struct abi_http_start *ureq) {
    if (!cap_check(current_proc(), CAP_NET, "sys_http_start")) return -ABI_EPERM;
    if (!ureq) return -ABI_EINVAL;
    struct abi_http_start req;
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) return -ABI_EFAULT;
    if (req.flags != 0 || !req.url) return -ABI_EINVAL;
    char kurl[512];
    if (strncpy_from_user(kurl, (const char *)(uintptr_t)req.url,
                          sizeof(kurl)) < 0)
        return -ABI_EFAULT;
    int h = httpa_start(kurl, req.max_body, current_proc()->pid);
    return h >= 0 ? (long)h : -ABI_EAGAIN;
}

static long sys_http_start2(struct abi_http_start2 *ureq) {
    if (!cap_check(current_proc(), CAP_NET, "sys_http_start2")) return -ABI_EPERM;
    if (!ureq) return -ABI_EINVAL;
    struct abi_http_start2 req;
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) return -ABI_EFAULT;
    if (!req.url) return -ABI_EINVAL;
    char kurl[512], kmethod[32], khdrs[1024];
    kmethod[0] = 0; khdrs[0] = 0;
    if (strncpy_from_user(kurl, (const char *)(uintptr_t)req.url,
                          sizeof(kurl)) < 0)
        return -ABI_EFAULT;
    if (req.method && strncpy_from_user(kmethod, (const char *)(uintptr_t)req.method,
                                        sizeof(kmethod)) < 0)
        return -ABI_EFAULT;
    if (req.headers && strncpy_from_user(khdrs, (const char *)(uintptr_t)req.headers,
                                         sizeof(khdrs)) < 0)
        return -ABI_EFAULT;
    uint8_t *kbody = NULL;
    uint32_t blen = req.body_len;
    if (req.body && blen) {
        if (blen > (256u * 1024)) return -ABI_EINVAL;
        if (!user_buf_ok(req.body, blen)) return -ABI_EFAULT;
        kbody = kmalloc(blen);
        if (!kbody) return -ABI_ENOMEM;
        if (copy_from_user(kbody, (const void *)(uintptr_t)req.body, blen) != 0) {
            kfree(kbody);
            return -ABI_EFAULT;
        }
    }
    /* only HTTP_F_NO_COOKIES honored from userland; other flags kernel-set */
    unsigned f = (req.flags & ABI_HTTP_F_NO_COOKIES) ? HTTP_F_NO_COOKIES : 0;
    int h = httpa_start2(kurl, kmethod, khdrs, kbody, blen,
                         req.max_body, f, current_proc()->pid);
    if (kbody) kfree(kbody);
    return h >= 0 ? (long)h : -ABI_EAGAIN;
}

static long sys_http_hdrs(long h, void *ubuf, uint64_t off_len) {
    uint32_t off = (uint32_t)(off_len >> 32);
    uint32_t len = (uint32_t)off_len;
    if (!ubuf || len == 0) return -ABI_EINVAL;
    if (len > 4096) len = 4096;
    if (!user_buf_ok((uint64_t)(uintptr_t)ubuf, len)) return -ABI_EFAULT;
    void *kb = kmalloc(len);
    if (!kb) return -ABI_ENOMEM;
    long n = httpa_hdrs((int)h, current_proc()->pid, kb, off, len);
    if (n > 0 && copy_to_user(ubuf, kb, (size_t)n) != 0) { kfree(kb); return -ABI_EFAULT; }
    kfree(kb);
    return n < 0 ? -ABI_EINVAL : n;
}

static long sys_http_poll(long h, struct abi_http_poll *uout) {
    if (!uout) return -ABI_EINVAL;
    struct abi_http_poll out;
    if (httpa_poll((int)h, current_proc()->pid, &out) != 0)
        return -ABI_EINVAL;
    if (copy_to_user(uout, &out, sizeof(out)) != 0) return -ABI_EFAULT;
    return 0;
}

/* a3 packs off<<32 | len (keeps the 3-arg syscall shape). The body is
 * bounced through a kernel buffer; transfers are capped at 1 MiB. */
static long sys_http_read(long h, void *ubuf, uint64_t off_len) {
    uint32_t off = (uint32_t)(off_len >> 32);
    uint32_t len = (uint32_t)off_len;
    if (!ubuf || len == 0) return -ABI_EINVAL;
    if (len > (1u << 20)) len = 1u << 20;
    if (!user_buf_ok((uint64_t)(uintptr_t)ubuf, len)) return -ABI_EFAULT;
    void *kb = kmalloc(len);
    if (!kb) return -ABI_ENOMEM;
    long n = httpa_read((int)h, current_proc()->pid, kb, off, len);
    if (n > 0 && copy_to_user(ubuf, kb, (size_t)n) != 0) {
        kfree(kb);
        return -ABI_EFAULT;
    }
    kfree(kb);
    return n < 0 ? -ABI_EINVAL : n;
}

static long sys_http_finish(long h) {
    if (httpa_finish((int)h, current_proc()->pid) != 0) return -ABI_EINVAL;
    return 0;
}

/* ---- WebSocket client (stage 13; see ws.c) ------------------------ */

static long sys_ws_open(struct abi_ws_open *ureq) {
    if (!cap_check(current_proc(), CAP_NET, "sys_ws_open")) return -ABI_EPERM;
    if (!ureq) return -ABI_EINVAL;
    struct abi_ws_open req;
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) return -ABI_EFAULT;
    if (req.flags != 0 || !req.url) return -ABI_EINVAL;
    char kurl[512];
    if (strncpy_from_user(kurl, (const char *)(uintptr_t)req.url,
                          sizeof(kurl)) < 0)
        return -ABI_EFAULT;
    int h = ws_open(kurl, current_proc()->pid);
    return h >= 0 ? (long)h : -ABI_EAGAIN;
}

static long sys_ws_poll(long h, struct abi_ws_poll *uout) {
    if (!uout) return -ABI_EINVAL;
    struct abi_ws_poll out;
    if (ws_poll((int)h, current_proc()->pid, &out) != 0)
        return -ABI_EINVAL;
    if (copy_to_user(uout, &out, sizeof(out)) != 0) return -ABI_EFAULT;
    return 0;
}

static long sys_ws_recv(long h, void *ubuf, uint64_t cap) {
    if (!ubuf || cap == 0) return -ABI_EINVAL;
    if (cap > (1u << 20)) cap = 1u << 20;
    if (!user_buf_ok((uint64_t)(uintptr_t)ubuf, cap)) return -ABI_EFAULT;
    void *kb = kmalloc((size_t)cap);
    if (!kb) return -ABI_ENOMEM;
    long n = ws_recv((int)h, current_proc()->pid, kb, (uint32_t)cap);
    if (n > 0 && copy_to_user(ubuf, kb, (size_t)n) != 0) {
        kfree(kb);
        return -ABI_EFAULT;
    }
    kfree(kb);
    return n < 0 ? -ABI_EAGAIN : n;
}

/* a3 packs op<<32 | len (the 3-arg syscall shape, like SYS_HTTP_READ). */
static long sys_ws_send(long h, const void *ubuf, uint64_t op_len) {
    int op = (int)(op_len >> 32);
    uint32_t len = (uint32_t)op_len;
    if (len > (256u << 10)) return -ABI_EINVAL;
    if (len && !ubuf) return -ABI_EINVAL;
    void *kb = NULL;
    if (len) {
        kb = kmalloc(len);
        if (!kb) return -ABI_ENOMEM;
        if (copy_from_user(kb, ubuf, len) != 0) {
            kfree(kb);
            return -ABI_EFAULT;
        }
    }
    int rc = ws_send((int)h, current_proc()->pid, kb, len, op);
    if (kb) kfree(kb);
    return rc == 0 ? 0 : -ABI_EINVAL;
}

static long sys_ws_close(long h, long code) {
    if (ws_close((int)h, current_proc()->pid, (uint16_t)code) != 0)
        return -ABI_EINVAL;
    return 0;
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
/* Lexically clean an ABSOLUTE path in-place into `out`: collapse runs of '/',
 * drop "." components, and resolve ".." by popping the previous component
 * (never above root). Purely lexical (no symlink following -- callers do that
 * separately via vfs_resolve_path). Returns 0, or -ABI_ENAMETOOLONG if it
 * doesn't fit. `in` must start with '/'.
 *
 * Without this, interior dot-components leak straight through to the fs layer,
 * whose lookups are a strcmp against normalized entry names (ramfs) or reject
 * "."/".." outright (tobyfs) -- so e.g. the Vulkan loader dlopen'ing an ICD at
 * "/opt/chrome/./libvk_swiftshader.so" (built from a relative library_path in
 * the ICD manifest) got ENOENT even though the file exists. */
static int path_lexical_clean(const char *in, char *out, size_t cap) {
    if (cap < 2) return -ABI_ENAMETOOLONG;
    size_t o = 0;
    out[o++] = '/';                       /* result is always rooted */
    const char *s = in;
    while (*s == '/') s++;                 /* skip leading slashes */
    while (*s) {
        const char *e = s;
        while (*e && *e != '/') e++;
        size_t clen = (size_t)(e - s);
        if (clen == 1 && s[0] == '.') {
            /* "." -- drop */
        } else if (clen == 2 && s[0] == '.' && s[1] == '.') {
            /* ".." -- pop the last component, but never above root */
            while (o > 1 && out[o - 1] != '/') o--;
            if (o > 1) o--;                /* also drop its leading '/' */
        } else if (clen > 0) {
            if (o > 1) {                   /* separator, unless right after root */
                if (o + 1 >= cap) return -ABI_ENAMETOOLONG;
                out[o++] = '/';
            }
            if (o + clen >= cap) return -ABI_ENAMETOOLONG;
            memcpy(out + o, s, clen);
            o += clen;
        }
        s = e;
        while (*s == '/') s++;
    }
    out[o] = '\0';
    return 0;
}

/* Tiny substring test (klibc has no strstr) for the CHROMIUM_BOOT path tracer. */
static int lx_path_has(const char *h, const char *n) {
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static int resolve_user_path(const char *user_path, char *out, size_t cap) {
    char up[ABI_PATH_MAX];
    long plen = strncpy_from_user(up, user_path, sizeof(up));
    if (plen < 0) return -ABI_EFAULT;
    if ((size_t)plen >= sizeof(up) - 1) return -ABI_ENAMETOOLONG;
    if (plen == 0 || cap == 0) return -ABI_EINVAL;
#ifdef CHROMIUM_BOOT
    /* slice 20: chrome's --dump-dom loads headless_command_resources.pak from
     * DIR_ASSETS (resolved via /proc/self/exe) to serve
     * chrome://headless/headless_command.html, whose executeCommands() JS
     * actually performs the dump. Trace those lookups. (klibc has no strstr.) */
    { static int c = 0;
      if (c < 40 && (lx_path_has(up, "headless") || lx_path_has(up, ".pak") ||
                     lx_path_has(up, "self/exe"))) {
          c++; kprintf("[path] %s\n", up); } }
#endif

    /* Build the absolute (still un-normalized) path in `full`, then lexically
     * clean it into `out`. Interior "/./", "/../", and "//" are all resolved by
     * path_lexical_clean, so a bare "." / "./foo" cwd-relative path Just Works
     * (e.g. `busybox ls .` -> opendir(".") -> the cwd) without a special case. */
    char full[ABI_PATH_MAX];
    if (up[0] == '/') {
        memcpy(full, up, (size_t)plen);
        full[plen] = '\0';
    } else {
        struct proc *p = current_proc();
        const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
        size_t clen = strlen(cwd);
        bool need_slash = (clen == 0 || cwd[clen - 1] != '/');
        size_t need = clen + (need_slash ? 1 : 0) + (size_t)plen + 1;
        if (need > sizeof(full)) return -ABI_ENAMETOOLONG;
        memcpy(full, cwd, clen);
        size_t o = clen;
        if (need_slash) full[o++] = '/';
        memcpy(full + o, up, (size_t)plen);
        full[o + plen] = '\0';
    }
    return path_lexical_clean(full, out, cap);
}

/* Resolve a (dirfd, user path) pair the way the *at() syscalls require: a
 * RELATIVE path resolves against DIRFD, not the cwd. Absolute paths, AT_FDCWD,
 * and a dirfd that is not an open directory all fall back to resolve_user_path
 * (cwd-relative), i.e. the long-standing behaviour.
 *
 * Chrome sandbox depends on this in two places: it opens /proc as a dirfd and
 * then reaches through it for "self/task/" (thread_helpers.cc:41) and
 * "self/fd/" (proc_util.cc:79). Both were fatal PCHECKs while we resolved
 * against the cwd. NOTE: resolve_user_path() prepends the cwd, so the
 * relativity test MUST be made on the RAW user string, before that happens. */
static int resolve_user_path_at(int dirfd, const char *upath,
                                char *out, size_t cap) {
    char rawp[ABI_PATH_MAX];
    long rl = strncpy_from_user(rawp, upath, sizeof rawp);
    if (rl < 0) return -ABI_EFAULT;
    if (rl > 0 && rawp[0] != '/' && dirfd >= 0) {
        struct file *df = fd_lookup(dirfd);
        if (df && df->kind == FILE_KIND_DIR && df->dirpath) {
            char joined[ABI_PATH_MAX];
            size_t dl = strlen(df->dirpath);
            if (dl + 1 + (size_t)rl < sizeof(joined)) {
                memcpy(joined, df->dirpath, dl);
                if (dl && joined[dl - 1] != '/') joined[dl++] = '/';
                memcpy(joined + dl, rawp, (size_t)rl + 1);
                return path_lexical_clean(joined, out, cap);
            }
        }
    }
    return resolve_user_path(upath, out, cap);
}

/* ---- Track B graphics: the Linux framebuffer device /dev/fb0 -----------
 *
 * A genuine Linux fbdev: open("/dev/fb0") -> FILE_KIND_FB; FBIOGET_VSCREENINFO
 * / FBIOGET_FSCREENINFO report a 32bpp XRGB8888 surface matching the gfx
 * scanout; mmap(fd) reserves a SHADOW scanout buffer (a normal anonymous user
 * region, so it tears down cleanly at exit -- never the real device pages);
 * the app draws XRGB pixels into it and presents with FBIOPAN_DISPLAY (or
 * FBIO_WAITFORVSYNC), which blits the shadow into the gfx back buffer and
 * flips. This is the standard double-buffered fbdev pan pattern. Single
 * global context: the proof runs one fbdev app at a time (before the
 * compositor, like REALTUI), so one open is enough. */
struct fbdev_ctx {
    bool     open;
    uint32_t xres, yres;       /* visible resolution */
    uint32_t bpp;              /* 32 */
    uint32_t line_length;      /* bytes per row in the shadow (xres*4) */
    uint32_t smem_len;         /* shadow size in bytes (line_length*yres) */
    uint64_t user_base;        /* mmap'd shadow base in the app's address space */
    uint64_t user_len;         /* mmap'd length */
    int      owner_pid;        /* tgid that opened it */
    uint64_t present_count;    /* FBIOPAN_DISPLAY / vsync presents served */
    /* M3: when the compositor is up, a fbdev app is presented INTO a compositor
     * window instead of the raw scanout, so a Linux GUI app appears as a normal,
     * movable, composited window alongside native + Win32 windows. `win` NULL =
     * the legacy direct-scanout fallback (pre-compositor boot harness). */
    struct window *win;        /* compositor window, or NULL for scanout mode */
    int      last_mx, last_my; /* last window-relative cursor pos (evdev deltas) */
};
static struct fbdev_ctx g_fbdev;

/* The Linux fbdev window's client size (a normal windowed resolution). */
#define FBDEV_WIN_W 640
#define FBDEV_WIN_H 480

/* Linux fbdev ioctl request numbers. */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIO_WAITFORVSYNC   0x4620   /* _IOW('F',0x20,u32); low 16 bits suffice */

/* Initialise the global fbdev geometry from the live gfx scanout. */
static void fbdev_init_geometry(void) {
    uint32_t w = gfx_width(), h = gfx_height();
    if (w == 0 || h == 0) { w = 1024; h = 768; }
    g_fbdev.xres = w; g_fbdev.yres = h; g_fbdev.bpp = 32;
    g_fbdev.line_length = w * 4;
    g_fbdev.smem_len = g_fbdev.line_length * h;
}

/* Present the shadow buffer: copy the app's XRGB pixels row-by-row into the
 * gfx back buffer, then flip to the hardware scanout. Returns 0 on success. */
static long fbdev_present(void) {
    if (!g_fbdev.open || !g_fbdev.user_base) return -ABI_EINVAL;
    uint32_t w = g_fbdev.xres, h = g_fbdev.yres;

    /* M3: present into the compositor window. We stage each row through a kernel
     * buffer (copy_from_user the app's shadow) and gui_window_blit it into the
     * window's backbuf -- struct window is opaque here, so we never touch its
     * internals directly. The compositor composites + flips it on the next tick;
     * the Linux app is now a normal movable window. */
    if (g_fbdev.win) {
        uint32_t *row = (uint32_t *)kmalloc(g_fbdev.line_length);
        if (!row) return -ABI_ENOMEM;
        for (uint32_t y = 0; y < h; y++) {
            if (copy_from_user(row,
                    (const void *)(uintptr_t)(g_fbdev.user_base + (uint64_t)y * g_fbdev.line_length),
                    w * 4) != 0)
                break;
            gui_window_blit(g_fbdev.win, 0, (int)y, (int)w, 1, row);
        }
        kfree(row);
        gui_window_flip(g_fbdev.win);
        g_fbdev.present_count++;
        return 0;
    }

    /* Legacy direct-scanout path (pre-compositor boot harness: REALTUI/fbsplash). */
    if (w > gfx_width())  w = gfx_width();
    if (h > gfx_height()) h = gfx_height();
    uint32_t *row = (uint32_t *)kmalloc(g_fbdev.line_length);
    if (!row) return -ABI_ENOMEM;
    for (uint32_t y = 0; y < h; y++) {
        if (copy_from_user(row,
                (const void *)(uintptr_t)(g_fbdev.user_base + (uint64_t)y * g_fbdev.line_length),
                w * 4) != 0)
            break;
        gfx_blit(0, (int)y, (int)w, 1, row, (int)w);
    }
    kfree(row);
    gfx_flip();
    g_fbdev.present_count++;
    return 0;
}

/* FBIOGET_VSCREENINFO: fill the app's struct fb_var_screeninfo (160 bytes on
 * x86-64). We set xres/yres/virtual/bpp + the XRGB8888 bitfields. */
static long fbdev_get_vscreeninfo(uint64_t up) {
    uint8_t v[160];
    memset(v, 0, sizeof(v));
    uint32_t *u32 = (uint32_t *)v;
    u32[0] = g_fbdev.xres;          /* xres            @0  */
    u32[1] = g_fbdev.yres;          /* yres            @4  */
    u32[2] = g_fbdev.xres;          /* xres_virtual    @8  */
    u32[3] = g_fbdev.yres;          /* yres_virtual    @12 */
    u32[6] = g_fbdev.bpp;           /* bits_per_pixel  @24 */
    /* fb_bitfield {offset,length,msb_right}: red@32, green@44, blue@56, transp@68 */
    u32[8]  = 16; u32[9]  = 8;      /* red.offset=16, len=8   */
    u32[11] = 8;  u32[12] = 8;      /* green.offset=8,  len=8 */
    u32[14] = 0;  u32[15] = 8;      /* blue.offset=0,   len=8 */
    u32[17] = 24; u32[18] = 8;      /* transp.offset=24,len=8 */
    return (copy_to_user((void *)(uintptr_t)up, v, sizeof(v)) == 0) ? 0 : -ABI_EFAULT;
}

/* FBIOGET_FSCREENINFO: fill the app's struct fb_fix_screeninfo (80 bytes). */
static long fbdev_get_fscreeninfo(uint64_t up) {
    uint8_t v[80];
    memset(v, 0, sizeof(v));
    const char id[] = "tobyos-fb";
    for (int i = 0; id[i] && i < 15; i++) v[i] = (uint8_t)id[i];   /* id[16] @0 */
    *(uint32_t *)(v + 24) = g_fbdev.smem_len;    /* smem_len    @24 */
    *(uint32_t *)(v + 28) = 0;                   /* type=PACKED @28 */
    *(uint32_t *)(v + 36) = 2;                   /* visual=TRUECOLOR @36 */
    *(uint32_t *)(v + 48) = g_fbdev.line_length; /* line_length @48 */
    return (copy_to_user((void *)(uintptr_t)up, v, sizeof(v)) == 0) ? 0 : -ABI_EFAULT;
}

/* Linux fbdev ioctl dispatch for a FILE_KIND_FB handle. */
static long fbdev_ioctl(unsigned long req, unsigned long arg) {
    switch (req & 0xFFFFu) {
    case FBIOGET_VSCREENINFO: return fbdev_get_vscreeninfo((uint64_t)arg);
    case FBIOGET_FSCREENINFO: return fbdev_get_fscreeninfo((uint64_t)arg);
    case FBIOPUT_VSCREENINFO: return 0;           /* accept (geometry is fixed) */
    case FBIOPAN_DISPLAY:                          /* present (double-buffer pan) */
    case FBIO_WAITFORVSYNC:   return fbdev_present();
    default:                  return -ABI_ENOTTY;
    }
}

/* mmap of /dev/fb0: reserve a fresh anonymous RW shadow scanout in the app's
 * address space and remember it as the present source. */
static long fbdev_mmap(uint64_t len) {
    if (g_fbdev.smem_len == 0) fbdev_init_geometry();
    uint64_t want = g_fbdev.smem_len;
    if (len > want) want = len;       /* honour an over-large request */
    long base = sys_mmap(0, want, 0x1u | 0x2u /*R|W*/,
                         0x01u | 0x02u /*ANON|SHARED*/, -1, 0);
    if (base < 0) return base;
    g_fbdev.user_base = (uint64_t)base;
    g_fbdev.user_len  = want;
    return base;
}

/* Present-on-exit: many real fbdev programs (e.g. busybox fbsplash) mmap the
 * device, draw straight into the mapping, and exit WITHOUT an explicit
 * FBIOPAN_DISPLAY -- on real hardware the mmap *is* the live scanout. Our
 * shadow model needs a present, so we do the final blit when the owning
 * process exits: at proc_exit the dying process's address space is still live
 * (so copy_from_user of the shadow works) and the hardware scanout keeps the
 * pixels after teardown. Called from proc_exit() for every exiting process. */
void fbdev_proc_exit(int pid) {
    if (!g_fbdev.open) return;
    if (g_fbdev.owner_pid != pid) return;
    if (g_fbdev.user_base) (void)fbdev_present();   /* final frame */
    if (g_fbdev.win) {                  /* M3: tear down the compositor window */
        gui_window_close(g_fbdev.win);
        g_fbdev.win = 0;
    }
    g_fbdev.open = false;               /* release for the next opener */
    g_fbdev.user_base = 0;
    g_fbdev.user_len  = 0;
}

/* fbdev_window_event() is defined further down, after evdev_push and the
 * EV_x / BTN_x constants it uses are in scope. */

/* ---- Track B input: the Linux evdev devices /dev/input/event{0,1} -----
 *
 * Genuine Linux input devices: open("/dev/input/event0") -> FILE_KIND_EVDEV
 * minor 0 (the KEYBOARD), event1 -> minor 1 (the MOUSE). read() returns a
 * stream of fixed 24-byte `struct input_event` records and the EVIOCG* ioctls
 * report per-device identity + capabilities. The PS/2 drivers feed raw input
 * here: the keyboard via evdev_feed_key() (for scancode set 1 the Linux
 * keycode equals the scancode across the main block, so the mapping is the
 * identity); the mouse via evdev_feed_mouse() as EV_REL deltas + BTN_* keys.
 * Each device has its own ring; read blocks (yield) when empty unless
 * O_NONBLOCK. */

#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define SYN_REPORT      0x00
#define REL_X           0x00
#define REL_Y           0x01
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define INPUT_EVENT_SZ  24u            /* sizeof(struct input_event) on x86-64 */

extern uint64_t pit_ticks(void);       /* full decls appear later in this file */
extern uint32_t pit_hz(void);

#define EVDEV_NDEV      2u
#define EVDEV_KBD       0u
#define EVDEV_MOUSE     1u
#define EVDEV_RING      512u           /* power of two */
struct evdev_evt { uint16_t type; uint16_t code; int32_t value; };
struct evdev_ctx {
    bool     open;
    bool     nonblock;
    int      owner_pid;
    int      kind;                     /* EVDEV_KBD / EVDEV_MOUSE */
    volatile uint32_t head, tail;      /* head=producer, tail=consumer */
    struct evdev_evt ring[EVDEV_RING];
    uint64_t produced, consumed;
};
static struct evdev_ctx g_evdev[EVDEV_NDEV];

/* evdev ioctl request numbers (Linux UAPI _IOC encoding -> low byte = nr,
 * second byte = type 'E'). We dispatch on nr after checking type=='E'. */
#define EVIOC_TYPE      'E'

static void evdev_push(unsigned dev, uint16_t type, uint16_t code, int32_t value) {
    struct evdev_ctx *d = &g_evdev[dev];
    uint32_t next = (d->head + 1u) & (EVDEV_RING - 1u);
    if (next == d->tail) {             /* full: drop oldest */
        d->tail = (d->tail + 1u) & (EVDEV_RING - 1u);
        d->consumed++;
    }
    d->ring[d->head].type  = type;
    d->ring[d->head].code  = code;
    d->ring[d->head].value = value;
    d->head = next;
    d->produced++;
}

void evdev_feed_key(uint8_t code, int value) {
    /* M3: while a Linux app is windowed, its input is delivered focus-routed via
     * fbdev_window_event(); suppress the global PS/2 feed so it isn't doubled. */
    if (g_fbdev.win) return;
    /* A real evdev key event is the EV_KEY followed by a SYN_REPORT frame. */
    evdev_push(EVDEV_KBD, EV_KEY, code, value);
    evdev_push(EVDEV_KBD, EV_SYN, SYN_REPORT, 0);
}

/* Feed one mouse report: relative motion + button edges, then a SYN frame --
 * exactly the packet shape a real PS/2 evdev mouse emits. `prev` is the button
 * bitmask before this report so we can emit press/release edges. The button
 * masks are the PS/2 driver's MOUSE_BTN_* bits (1=left,2=right,4=middle). */
void evdev_feed_mouse(int dx, int dy, uint8_t buttons, uint8_t prev) {
    if (g_fbdev.win) return;        /* M3: windowed -> focus-routed feed instead */
    if (dx) evdev_push(EVDEV_MOUSE, EV_REL, REL_X, dx);
    if (dy) evdev_push(EVDEV_MOUSE, EV_REL, REL_Y, dy);
    uint8_t changed = (uint8_t)(buttons ^ prev);
    if (changed & 0x01) evdev_push(EVDEV_MOUSE, EV_KEY, BTN_LEFT,   (buttons & 0x01) ? 1 : 0);
    if (changed & 0x02) evdev_push(EVDEV_MOUSE, EV_KEY, BTN_RIGHT,  (buttons & 0x02) ? 1 : 0);
    if (changed & 0x04) evdev_push(EVDEV_MOUSE, EV_KEY, BTN_MIDDLE, (buttons & 0x04) ? 1 : 0);
    if (dx || dy || changed)
        evdev_push(EVDEV_MOUSE, EV_SYN, SYN_REPORT, 0);
}

/* M3: the compositor (gui.c enqueue_event, IRQ context) calls this for EVERY
 * window event. When the event belongs to the Linux app's fbdev window, we
 * translate it into evdev records (event1 = mouse REL+BTN, event0 = key) and
 * push them onto the same rings the global PS/2 feed uses -- so the app's
 * blocking read() on /dev/input/event1 wakes on the IRQ exactly as before, but
 * the input is now focus-routed by the compositor (only the focused/hovered
 * window gets events). The global PS/2 feed is suppressed while a fbdev window
 * is up (evdev_feed_*), so there is no double input. */
void fbdev_window_event(struct window *w, const struct gui_event *e) {
    if (!w || w != g_fbdev.win || !e) return;
    switch (e->type) {
    case GUI_EV_MOUSE_MOVE:
        if (g_fbdev.last_mx >= 0) {
            int dx = e->x - g_fbdev.last_mx, dy = e->y - g_fbdev.last_my;
            if (dx) evdev_push(EVDEV_MOUSE, EV_REL, REL_X, dx);
            if (dy) evdev_push(EVDEV_MOUSE, EV_REL, REL_Y, dy);
            if (dx || dy) evdev_push(EVDEV_MOUSE, EV_SYN, SYN_REPORT, 0);
        }
        g_fbdev.last_mx = e->x; g_fbdev.last_my = e->y;
        break;
    case GUI_EV_MOUSE_DOWN:
    case GUI_EV_MOUSE_UP: {
        int val = (e->type == GUI_EV_MOUSE_DOWN) ? 1 : 0;
        uint16_t code = (e->button & 0x02) ? BTN_RIGHT
                      : (e->button & 0x04) ? BTN_MIDDLE : BTN_LEFT;
        g_fbdev.last_mx = e->x; g_fbdev.last_my = e->y;
        evdev_push(EVDEV_MOUSE, EV_KEY, code, val);
        evdev_push(EVDEV_MOUSE, EV_SYN, SYN_REPORT, 0);
        break;
    }
    case GUI_EV_KEY:
        /* deliver a press+release pair (the compositor has no key-up event) */
        evdev_push(EVDEV_KBD, EV_KEY, e->key, 1);
        evdev_push(EVDEV_KBD, EV_KEY, e->key, 0);
        evdev_push(EVDEV_KBD, EV_SYN, SYN_REPORT, 0);
        break;
    default: break;
    }
}

void evdev_reset(void) {
    for (unsigned i = 0; i < EVDEV_NDEV; i++) {
        g_evdev[i].head = g_evdev[i].tail = 0;
        g_evdev[i].produced = g_evdev[i].consumed = 0;
    }
}

/* Serialise one queued event into the 24-byte Linux struct input_event:
 *   struct timeval { long sec; long usec; } time;  __u16 type; __u16 code;
 *   __s32 value;  (offsets: time@0, type@16, code@18, value@20). */
static void evdev_serialise(const struct evdev_evt *e, uint8_t out[INPUT_EVENT_SZ]) {
    uint64_t hz = pit_hz(); if (!hz) hz = 100;
    uint64_t t  = pit_ticks();
    long sec  = (long)(t / hz);
    long usec = (long)((t % hz) * (1000000ull / hz));
    memset(out, 0, INPUT_EVENT_SZ);
    *(long *)(out + 0)      = sec;
    *(long *)(out + 8)      = usec;
    *(uint16_t *)(out + 16) = e->type;
    *(uint16_t *)(out + 18) = e->code;
    *(int32_t  *)(out + 20) = e->value;
}

/* read() on /dev/input/event<dev>: copy as many whole input_events as fit.
 * Blocks (yield) on an empty queue unless O_NONBLOCK, per evdev semantics. */
static long evdev_read(unsigned dev, uint64_t ubuf, size_t len) {
    if (dev >= EVDEV_NDEV) return -ABI_EBADF;
    struct evdev_ctx *d = &g_evdev[dev];
    size_t cap = len / INPUT_EVENT_SZ;
    if (cap == 0) return -ABI_EINVAL;     /* evdev requires >= one event */

    while (d->tail == d->head) {
        if (d->nonblock) return -ABI_EAGAIN;
        if (signal_pending_self()) return EINTR_RET;
        sti(); hlt();
    }

    size_t n = 0;
    uint8_t rec[INPUT_EVENT_SZ];
    while (n < cap && d->tail != d->head) {
        struct evdev_evt e = d->ring[d->tail];
        d->tail = (d->tail + 1u) & (EVDEV_RING - 1u);
        d->consumed++;
        evdev_serialise(&e, rec);
        if (copy_to_user((void *)(uintptr_t)(ubuf + n * INPUT_EVENT_SZ),
                         rec, INPUT_EVENT_SZ) != 0)
            return n ? (long)(n * INPUT_EVENT_SZ) : -ABI_EFAULT;
        n++;
    }
    return (long)(n * INPUT_EVENT_SZ);
}

static long evdev_copy_bits(uint64_t up, unsigned size, const uint8_t *bits,
                            unsigned nbits_bytes) {
    unsigned n = size < nbits_bytes ? size : nbits_bytes;
    return (copy_to_user((void *)(uintptr_t)up, bits, n) == 0) ? (long)n
                                                               : -ABI_EFAULT;
}

static long evdev_ioctl(unsigned dev, unsigned long req, unsigned long arg) {
    if (dev >= EVDEV_NDEV) return -ABI_EBADF;
    bool is_mouse = (g_evdev[dev].kind == (int)EVDEV_MOUSE);
    unsigned nr   = req & 0xFFu;
    unsigned type = (req >> 8) & 0xFFu;
    unsigned size = (req >> 16) & 0x3FFFu;
    if (type != EVIOC_TYPE) return -ABI_ENOTTY;

    switch (nr) {
    case 0x01: {                                   /* EVIOCGVERSION (int) */
        int ver = 0x010001;                        /* EV_VERSION */
        return (copy_to_user((void *)(uintptr_t)arg, &ver, sizeof(ver)) == 0)
               ? 0 : -ABI_EFAULT;
    }
    case 0x02: {                                   /* EVIOCGID (struct input_id) */
        uint16_t id[4] = { 0x0011 /*BUS_I8042*/, 0x0001,
                           (uint16_t)(is_mouse ? 0x0002 : 0x0001), 0xAB41 };
        return (copy_to_user((void *)(uintptr_t)arg, id, sizeof(id)) == 0)
               ? 0 : -ABI_EFAULT;
    }
    case 0x06: {                                   /* EVIOCGNAME(len) */
        const char *name = is_mouse ? "tobyOS PS/2 Mouse"
                                     : "tobyOS PS/2 Keyboard";
        unsigned nl = 0; while (name[nl]) nl++; nl++;   /* include NUL */
        unsigned n  = size < nl ? size : nl;
        return (copy_to_user((void *)(uintptr_t)arg, name, n) == 0)
               ? (long)n : -ABI_EFAULT;
    }
    case 0x20: {                                   /* EVIOCGBIT(0): event types */
        uint8_t evbits[4] = { 0 };
        evbits[0] = is_mouse ? ((1u << EV_SYN) | (1u << EV_KEY) | (1u << EV_REL))
                             : ((1u << EV_SYN) | (1u << EV_KEY));
        return evdev_copy_bits(arg, size, evbits, sizeof(evbits));
    }
    case 0x22: {                                   /* EVIOCGBIT(EV_REL): rel axes */
        uint8_t relbits[4] = { 0 };
        if (is_mouse) relbits[0] = (1u << REL_X) | (1u << REL_Y);  /* 0x03 */
        return evdev_copy_bits(arg, size, relbits, sizeof(relbits));
    }
    case 0x21: {                                   /* EVIOCGBIT(EV_KEY): key map */
        uint8_t keybits[(0x120 / 8) + 1];
        memset(keybits, 0, sizeof(keybits));
        if (is_mouse) {
            keybits[BTN_LEFT   >> 3] |= (uint8_t)(1u << (BTN_LEFT   & 7));
            keybits[BTN_RIGHT  >> 3] |= (uint8_t)(1u << (BTN_RIGHT  & 7));
            keybits[BTN_MIDDLE >> 3] |= (uint8_t)(1u << (BTN_MIDDLE & 7));
        } else {
            for (unsigned k = 1; k <= 0x53; k++)    /* main block we emit */
                keybits[k >> 3] |= (uint8_t)(1u << (k & 7));
        }
        return evdev_copy_bits(arg, size, keybits, sizeof(keybits));
    }
    default:
        return -ABI_ENOTTY;
    }
}

/* ---- file open / close / dup ----------------------------------- */

static long sys_open(const char *path, int flags, int mode) {
    (void)mode;     /* M25A: permissions on creation not honoured yet */
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, kpath, sizeof(kpath));
    if (rr) return rr;

    /* B23: pseudoterminal device nodes. /dev/ptmx allocates a fresh pair and
     * yields its master; /dev/pts/<N> yields the slave of pair N. These are
     * synthesized (no VFS backing). */
    if (kpath[0] == '/' && kpath[1] == 'd' && kpath[2] == 'e' &&
        kpath[3] == 'v' && kpath[4] == '/') {
        const char *dev = kpath + 5;
        /* /dev/null and /dev/zero. base::LaunchProcess opens /dev/null to
         * remap a child's stdio before execve; without it chrome's children
         * bailed with _exit(127) ("GPU process exited unexpectedly:
         * exit_code=32512"). Ubiquitous in Unix programs generally. */
        if (strcmp(dev, "null") == 0 || strcmp(dev, "zero") == 0) {
            struct file *f = (struct file *)kmalloc(sizeof(*f));
            if (!f) return -ABI_ENOMEM;
            memset(f, 0, sizeof(*f));
            f->kind = (dev[0] == 'n') ? FILE_KIND_DEVNULL : FILE_KIND_DEVZERO;
            f->o_accmode = 2;                       /* O_RDWR */
            int fd = fd_alloc_into(current_proc(), f);
            if (fd < 0) { kfree(f); return -ABI_EMFILE; }
            return fd;
        }
        /* /dev/urandom and /dev/random: the kernel CSPRNG as a readable file.
         * getrandom(2) is the modern path, but plenty of code still opens these
         * -- NSS's freebl falls back to /dev/urandom when the syscall is
         * unavailable, and openssl/glibc/python do the same. Both nodes are the
         * same non-blocking source (as on modern Linux, where /dev/random no
         * longer blocks once the pool is initialised). */
        if (strcmp(dev, "urandom") == 0 || strcmp(dev, "random") == 0) {
            struct file *f = (struct file *)kmalloc(sizeof(*f));
            if (!f) return -ABI_ENOMEM;
            memset(f, 0, sizeof(*f));
            f->kind = FILE_KIND_DEVRANDOM;
            f->o_accmode = 2;                       /* O_RDWR: writes are mixed in */
            int fd = fd_alloc_into(current_proc(), f);
            if (fd < 0) { kfree(f); return -ABI_EMFILE; }
            return fd;
        }
        if (strcmp(dev, "ptmx") == 0) {
            struct file *f = pty_open_master();
            if (!f) return -ABI_ENOMEM;
            int fd = fd_alloc_into(current_proc(), f);
            if (fd < 0) { file_close(f); return -ABI_EMFILE; }
            return fd;
        }
        if (dev[0] == 'p' && dev[1] == 't' && dev[2] == 's' && dev[3] == '/') {
            const char *ns = dev + 4;
            int idx = 0; bool any = false;
            for (const char *q = ns; *q; q++) {
                if (*q < '0' || *q > '9') { any = false; break; }
                idx = idx * 10 + (*q - '0'); any = true;
            }
            if (!any) return -ABI_ENOENT;
            struct file *f = pty_open_slave(idx);
            if (!f) return -ABI_ENOENT;
            int fd = fd_alloc_into(current_proc(), f);
            if (fd < 0) { file_close(f); return -ABI_EMFILE; }
            return fd;
        }
        /* Slice 98 (tier 3 Phase 1b): the DRM render node. Mesa opens
         * /dev/dri/renderD128 (and probes card0); both land here. When no
         * virgl-capable device is bound we return ENOENT, which is exactly
         * what a Linux box with no GPU does -- Mesa then falls back to
         * software, rather than failing in some deeper, stranger way. */
        /* Slice 102: /dev/dri as a LISTABLE DIRECTORY. libdrm's
         * drmGetDevice2(fd) -- which Mesa reaches through
         * loader_get_pci_id_for_fd, and which reported "failed to
         * retrieve device information" -- does not stop at the sysfs
         * tree: it opendir()s /dev/dri and iterates it to fill in the
         * device's node names. /dev/dri existed only as a special case
         * in this open path, so there was nothing to enumerate.
         * FILE_KIND_DIR + the dirpath marker; getdents64 synthesises the
         * entries (there is no VFS directory behind /dev). */
        if (strcmp(dev, "dri") == 0 || strcmp(dev, "dri/") == 0) {
            if (!lxdrm_available()) return -ABI_ENOENT;
            struct file *f = (struct file *)kmalloc(sizeof(*f));
            if (!f) return -ABI_ENOMEM;
            memset(f, 0, sizeof(*f));
            f->kind    = FILE_KIND_DIR;
            /* dirpath MUST be heap-owned: file_close/file_clone kfree it.
             * A string literal here panicked the kernel on close with
             * "bad block magic" once Mesa actually opened the directory. */
            f->dirpath = (char *)kmalloc(9);
            if (!f->dirpath) { kfree(f); return -ABI_ENOMEM; }
            memcpy(f->dirpath, "/dev/dri", 9);
            f->dir_off = 0;
            int fd = fd_alloc_into(current_proc(), f);
            if (fd < 0) { kfree(f); return -ABI_EMFILE; }
            return fd;
        }
        if (strncmp(dev, "dri/", 4) == 0) {
            struct file *f = lxdrm_open(dev);
            if (!f) return -ABI_ENOENT;
            int fd = fd_alloc_into(current_proc(), f);
            if (fd < 0) { kfree(f); return -ABI_EMFILE; }
            return fd;
        }
        /* Track B graphics: the Linux framebuffer device. */
        if (strcmp(dev, "fb0") == 0) {
            struct file *f = (struct file *)kmalloc(sizeof(*f));
            if (!f) return -ABI_ENOMEM;
            memset(f, 0, sizeof(*f));
            f->kind = FILE_KIND_FB;
            int fd = fd_alloc_into(current_proc(), f);
            if (fd < 0) { kfree(f); return -ABI_EMFILE; }
            struct proc *p = current_proc();
            memset(&g_fbdev, 0, sizeof(g_fbdev));
            g_fbdev.open = true;
            g_fbdev.owner_pid = p ? (p->is_thread ? p->tgid : p->pid) : 0;
            g_fbdev.last_mx = g_fbdev.last_my = -1;
            /* M3: if the desktop compositor is up, present this Linux app INTO a
             * compositor window (so it appears as a normal composited window),
             * sized to a windowed resolution we report as the fbdev geometry.
             * Otherwise fall back to the full-screen scanout (boot harness). */
            if (gui_in_desktop_mode()) {
                struct window *win = gui_window_create(FBDEV_WIN_W, FBDEV_WIN_H,
                                                       "Linux App");
                if (win) {
                    g_fbdev.win = win;
                    g_fbdev.xres = FBDEV_WIN_W;
                    g_fbdev.yres = FBDEV_WIN_H;
                    g_fbdev.bpp = 32;
                    g_fbdev.line_length = FBDEV_WIN_W * 4;
                    g_fbdev.smem_len = g_fbdev.line_length * FBDEV_WIN_H;
                    /* Phase 3 (foreign chrome): paint the client area with the
                     * active theme's window background so a Linux app that hasn't
                     * drawn yet (or clears its fb) blends with the desktop instead
                     * of flashing white. */
                    gui_window_fill(win, 0, 0, FBDEV_WIN_W, FBDEV_WIN_H,
                                    theme_active()->win_bg);
                }
            }
            if (!g_fbdev.win) fbdev_init_geometry();
            return fd;
        }
        /* Track B input: the Linux evdev devices. event0 = keyboard, event1 =
         * mouse. Attaches to the device's global event queue (does NOT flush
         * it, so events injected before the reader opened are still delivered).
         * The minor is stashed in f->dir_off (unused for this kind). */
        if (strcmp(dev, "input/event0") == 0 || strcmp(dev, "input/event1") == 0) {
            unsigned minor = (dev[11] == '1') ? EVDEV_MOUSE : EVDEV_KBD;
            struct file *f = (struct file *)kmalloc(sizeof(*f));
            if (!f) return -ABI_ENOMEM;
            memset(f, 0, sizeof(*f));
            f->kind = FILE_KIND_EVDEV;
            f->dir_off = minor;
            int fd = fd_alloc_into(current_proc(), f);
            if (fd < 0) { kfree(f); return -ABI_EMFILE; }
            struct proc *p = current_proc();
            g_evdev[minor].open = true;
            g_evdev[minor].kind = (int)minor;
            g_evdev[minor].nonblock = (flags & ABI_O_NONBLOCK) != 0;
            g_evdev[minor].owner_pid = p ? (p->is_thread ? p->tgid : p->pid) : 0;
            return fd;
        }
        /* Console / virtual-terminal device nodes: /dev/console, /dev/tty,
         * /dev/tty<N>. Real programs (e.g. busybox fbsplash) open the console
         * to switch the VT to graphics mode + hide the cursor. Back them all
         * with the kernel console; the KD/VT ioctls are accepted (see
         * LX_ioctl). */
        {
            bool is_con = (strcmp(dev, "console") == 0) || (strcmp(dev, "tty") == 0);
            if (!is_con && dev[0] == 't' && dev[1] == 't' && dev[2] == 'y') {
                is_con = true;                       /* /dev/tty<N> */
                for (const char *q = dev + 3; *q; q++)
                    if (*q < '0' || *q > '9') { is_con = false; break; }
            }
            if (is_con) {
                struct file *f = console_file_make();
                if (!f) return -ABI_ENOMEM;
                int fd = fd_alloc_into(current_proc(), f);
                if (fd < 0) { file_close(f); return -ABI_EMFILE; }
                return fd;
            }
        }
    }

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
            /* Slice 61c: map the REAL failure. The old blanket EACCES made
             * a FULL /data volume report "Permission denied" -- chrome's
             * shared-memory allocator logged 64k of those while the actual
             * condition was ENOSPC (4 MiB tobyfs exhausted), which sent the
             * frame-pipeline-freeze investigation toward permissions and
             * cost a day. Errno fidelity is diagnostic infrastructure. */
            if (cr == VFS_ERR_NOSPC)        return -ABI_ENOSPC;
            if (cr == VFS_ERR_NAMETOOLONG)  return -ABI_ENAMETOOLONG;
            if (cr != VFS_OK)               return -ABI_EACCES;
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
    f->o_accmode = flags & 3;    /* O_ACCMODE: O_RDONLY/O_WRONLY/O_RDWR for F_GETFL */

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
    /* POSIX: seeking past EOF does NOT change the file size -- the file only
     * grows when bytes are actually written. (Was: bumped vfs.size to the seek
     * offset, a latent bug. It surfaced via SQLite's positioned reads: seeking
     * to read a not-yet-written page grew the empty db, so SQLite saw a short
     * garbage "database" and returned SQLITE_NOTADB. Writes still extend via
     * file_write.) */
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
    struct file **t = proc_fds(current_proc());
    if (!t) { file_close(cl); return -ABI_EBADF; }
    if (t[newfd]) {
        file_close(t[newfd]);
        t[newfd] = 0;
    }
    t[newfd] = cl;
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

static long sys_rename(const char *oldp, const char *newp) {
    char ko[ABI_PATH_MAX], kn[ABI_PATH_MAX];
    int rr = resolve_user_path(oldp, ko, sizeof(ko));
    if (rr) return rr;
    rr = resolve_user_path(newp, kn, sizeof(kn));
    if (rr) return rr;
    int rc = vfs_rename(ko, kn);
    switch (rc) {
    case VFS_OK:        return 0;
    case VFS_ERR_NOENT: return -ABI_ENOENT;
    case VFS_ERR_NOTDIR:return -ABI_ENOTDIR;
    case VFS_ERR_EXIST: return -ABI_EEXIST;
    case VFS_ERR_NOSPC: return -ABI_ENOSPC;
    case VFS_ERR_ROFS:  return -ABI_EROFS;
    case VFS_ERR_PERM:  return -ABI_EACCES;
    case VFS_ERR_INVAL: return -ABI_EINVAL;
    default:            return -ABI_EIO;
    }
}

static long sys_mkdir(const char *path, int mode) {
    char kpath[ABI_PATH_MAX];
    int rr = resolve_user_path(path, kpath, sizeof(kpath));
    if (rr) return rr;
    /* Slice 86: honour the requested mode. A caller that asks for a private
     * directory and then verifies it (chrome's ProcessSingleton CHECKs for
     * exactly 0700) must not be told 0755. Mode 0 means "unspecified" from
     * in-kernel callers, which keep the old default. */
    uint32_t m = (uint32_t)mode & 07777u;
    int rc = m ? vfs_mkdir_mode(kpath, m) : vfs_mkdir(kpath);
#ifdef CHROMIUM_BOOT
    { static int mk = 0; if (mk < 24) { mk++;
        struct vfs_stat vs; int sr = vfs_stat(kpath, &vs);
        /* kprintf has no %o -- modes printed as hex (0700 == 0x1c0). */
        kprintf("[mkdir] '%s' mode=0x%x rc=%d -> readback 0x%x\n", kpath, m, rc,
                sr == VFS_OK ? (vs.mode & 07777u) : 0); } }
#endif
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
        /* PAUSE-spin, NOT `sti(); hlt()`.
         *
         * A parked HLT can wedge the whole machine: once every core is
         * halted, headless QEMU/TCG (and, defensively, any box where an
         * IOAPIC->LAPIC timer edge gets dropped) stops delivering the timer
         * interrupt that would wake us, so the HLT never returns. That is the
         * exact "silent stall a few seconds into boot" signature -- the single
         * runnable thing (pid 0's pause-spin idle loop) ends up parked behind
         * a sleeper's HLT and the box freezes with no fault.
         *
         * Spinning on PAUSE keeps this core executing, which (a) re-checks the
         * TSC deadline above every iteration so the sleep ALWAYS completes on
         * the monotonic clock (perf_now_ns keeps advancing regardless of IRQ
         * delivery), and (b) lets any genuinely-pending IRQ get taken at an
         * instruction boundary. We still sched_yield() each iteration so peers
         * (and pid 0) keep running, and we keep IRQs enabled. */
        sti();
        __asm__ volatile ("pause" ::: "memory");
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
    struct file **ptab  = proc_fds(parent);   /* thread -> leader's shared table */
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
        if (v < 0 || v >= PROC_NFDS || !ptab || !ptab[v]) {
            failed = true; break;
        }
        struct file *cl = file_clone(ptab[v]);
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

/* Per-core CPU utilisation. Returns the last-computed per-core busy % (the
 * caller samples SYS_SYSTEM_METRICS first, which advances the window). */
static long sys_cpu_stats(struct abi_cpu_stats *out) {
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out)))
        return -ABI_EFAULT;
    struct abi_cpu_stats staging;
    memset(&staging, 0, sizeof(staging));
    staging.cpu_count = sysmon_cpu_pcts(staging.busy_pct, ABI_CPU_STATS_MAX);
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

/* ---- persistent-storage provisioning ------------------------ */

static long sys_blk_list(struct abi_blk_info *out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    if (cap > ABI_BLK_MAX_DEVICES) cap = ABI_BLK_MAX_DEVICES;
    if (!user_buf_ok((uint64_t)(uintptr_t)out, sizeof(*out) * cap))
        return -ABI_EFAULT;
    /* Kernel staging, exact-prefix copy-out -- same pattern (and same
     * BKL-serialised safety argument) as sys_dev_list above. */
    static struct abi_blk_info staging[ABI_BLK_MAX_DEVICES];
    long n = provision_fill_blk_list(staging, cap);
    if (n <= 0) return n;
    if (copy_to_user(out, staging, sizeof(staging[0]) * (size_t)n) != 0)
        return -ABI_EFAULT;
    return n;
}

static long sys_data_provision(const struct abi_provision_req *ureq) {
    /* Formatting a disk is the most destructive syscall in the system:
     * gate it on the same capability as the other system-mutating
     * calls (settings/login/chmod). The kernel guard inside
     * provision_create_data_volume re-validates the target itself. */
    if (!cap_check(current_proc(), CAP_SETTINGS_WRITE,
                   "sys_data_provision"))
        return -ABI_EPERM;

    struct abi_provision_req req;
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) return -ABI_EFAULT;
    req.dev[sizeof(req.dev) - 1] = '\0';

    struct blk_dev *d = blk_find(req.dev);
    if (!d) return -ABI_ENOENT;
    return provision_create_data_volume(d,
        (req.flags & ABI_PROV_F_FORCE) != 0);
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
    case ABI_SYS_READ_NB:                        /* slice 45: non-blocking pipe read */
        return sys_read_nb((int)a1, (void *)a2, (size_t)a3);
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
    case ABI_SYS_GUI_TEXT_TTF:
        return sys_gui_text_ttf((int)a1, (uint32_t)a2, (const char *)a3,
                                (uint32_t)a4, (uint32_t)a5);
    case ABI_SYS_GUI_TEXT_TTF_WIDTH:
        return sys_gui_text_ttf_width((const char *)a1, (uint32_t)a2);
    case ABI_SYS_GUI_TEXT_TTF_RASTER:
        return sys_gui_text_ttf_raster((struct abi_ttf_raster *)a1);
    case ABI_SYS_GUI_FONT_REGISTER:
        return sys_gui_font_register((const void *)a1, (uint32_t)a2);
    case SYS_GUI_TEXT:
        return sys_gui_text((int)a1, (uint32_t)a2, (const char *)a3,
                            (uint32_t)a4, (uint32_t)a5);
    case SYS_GUI_FLIP:
        return sys_gui_flip((int)a1);
    case SYS_GUI_FLIP_RECT:
        return sys_gui_flip_rect((int)a1, a2, a3);
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

    /* ---- persistent-storage provisioning ------------------------ */
    case ABI_SYS_BLK_LIST:
        return sys_blk_list((struct abi_blk_info *)a1, (uint32_t)a2);
    case ABI_SYS_DATA_PROVISION:
        return sys_data_provision((const struct abi_provision_req *)a1);

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
    case ABI_SYS_CPU_STATS:
        return sys_cpu_stats((struct abi_cpu_stats *)a1);

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
    case ABI_SYS_HTTP_FETCH:
        return sys_http_fetch((struct abi_http_fetch *)a1);
    case ABI_SYS_HTTP_START:
        return sys_http_start((struct abi_http_start *)a1);
    case ABI_SYS_HTTP_POLL:
        return sys_http_poll((long)a1, (struct abi_http_poll *)a2);
    case ABI_SYS_HTTP_READ:
        return sys_http_read((long)a1, (void *)a2, (uint64_t)a3);
    case ABI_SYS_HTTP_FINISH:
        return sys_http_finish((long)a1);
    case ABI_SYS_HTTP_START2:
        return sys_http_start2((struct abi_http_start2 *)a1);
    case ABI_SYS_HTTP_HDRS:
        return sys_http_hdrs((long)a1, (void *)a2, (uint64_t)a3);

    /* ---- WebSocket client (stage 13) -------------------------------- */
    case ABI_SYS_WS_OPEN:
        return sys_ws_open((struct abi_ws_open *)a1);
    case ABI_SYS_WS_POLL:
        return sys_ws_poll((long)a1, (struct abi_ws_poll *)a2);
    case ABI_SYS_WS_RECV:
        return sys_ws_recv((long)a1, (void *)a2, (uint64_t)a3);
    case ABI_SYS_WS_SEND:
        return sys_ws_send((long)a1, (const void *)a2, (uint64_t)a3);
    case ABI_SYS_WS_CLOSE:
        return sys_ws_close((long)a1, (long)a2);

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
        /* Slice 39: the blit reads the USER pixel buffer with a raw memcpy
         * (gfx_surface_blit), which under SMAP (+smap runs) faults fatally
         * with AC clear -- first hit by chromewin's frame blit (panic at
         * gfx memcpy, err=0x1 P=1 kernel-read). Validate + fault-in the
         * range, then bracket the whole copy in one uaccess window (same
         * model as the ELF loader's segment writes). */
        if (w > 0 && h > 0) {
            size_t blen = (size_t)w * (size_t)h * 4;
            if (!user_range_ok(a3, blen) ||
                uaccess_prepare_read(a3, blen) != 0) return -1;
        }
        unsigned long uf = uaccess_begin();
        long br = gui_window_blit(f->win, dx, dy, w, h, (const uint32_t *)a3);
        uaccess_end(uf);
        return br;
    }
    case ABI_SYS_GUI_BLIT_BLEND: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int dx = (int)(int16_t)(a2 & 0xFFFF), dy = (int)(int16_t)(a2 >> 16);
        int w = (int)(int16_t)(a4 & 0xFFFF), h = (int)(int16_t)(a4 >> 16);
        if (w > 0 && h > 0) {
            size_t blen = (size_t)w * (size_t)h * 4;
            if (!user_range_ok(a3, blen) ||
                uaccess_prepare_read(a3, blen) != 0) return -1;
        }
        unsigned long uf = uaccess_begin();
        long br = gui_window_blit_blend(f->win, dx, dy, w, h,
                                        (const uint32_t *)a3);
        uaccess_end(uf);
        return br;
    }
    case ABI_SYS_GUI_GETPIXELS: {
        struct file *f = fd_lookup((int)a1);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win) return -1;
        int sx = (int)(int16_t)(a2 & 0xFFFF), sy = (int)(int16_t)(a2 >> 16);
        int w = (int)(int16_t)(a4 & 0xFFFF), h = (int)(int16_t)(a4 >> 16);
        if (w <= 0 || h <= 0) return 0;
        /* SMAP-safe: gui_window_getpixels writes its destination DIRECTLY, so
         * handing it the user pointer faults under CR4.SMAP. Stage each row
         * through a kernel buffer and copy_to_user it out instead. */
        static uint32_t krow[1024];
        if (w > 1024) w = 1024;
        uint32_t *udst = (uint32_t *)a3;
        long total = 0;
        for (int yy = 0; yy < h; yy++) {
            int got = gui_window_getpixels(f->win, sx, sy + yy, w, 1, krow);
            if (got <= 0) break;
            if (copy_to_user(udst + (long)yy * w, krow,
                             (size_t)got * sizeof(uint32_t)) != 0)
                return -ABI_EFAULT;
            total += got;
        }
        return total;
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
        return futex((uint32_t *)(uintptr_t)a1, (int)a2, (uint32_t)a3,
                     (const void *)(uintptr_t)a4,
                     (uint32_t *)(uintptr_t)a5);

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

    case ABI_SYS_XFRAME_POLL: {
        /* Tier 2.5: chromewin blits Ozone/MIT-SHM pixels without CDP JPEG.
         * a1=struct abi_xframe*, a2=pixels|NULL, a3=cap */
        extern long xframe_poll(uint32_t *, uint32_t *, uint32_t *,
                                uint32_t *, void *, size_t);
        if (!a1) return -ABI_EINVAL;
        uint32_t w = 0, h = 0, stride = 0, gen = 0;
        void *upix = (void *)(uintptr_t)a2;
        size_t cap = (size_t)a3;
        uint8_t *tmp = 0;
        if (upix && cap) {
            tmp = kmalloc(cap);
            if (!tmp) return -ABI_ENOMEM;
        }
        long rc = xframe_poll(&w, &h, &stride, &gen, tmp, cap);
        if (rc < 0) { if (tmp) kfree(tmp); return rc; }
        struct abi_xframe xf = { w, h, stride, gen };
        if (copy_to_user((void *)(uintptr_t)a1, &xf, sizeof xf) != 0)
            { if (tmp) kfree(tmp); return -ABI_EFAULT; }
        if (tmp) {
            size_t nbytes = (size_t)stride * h;
            if (nbytes > cap) nbytes = cap;
            if (copy_to_user(upix, tmp, nbytes) != 0)
                { kfree(tmp); return -ABI_EFAULT; }
            kfree(tmp);
        }
        return 0;
    }

    case ABI_SYS_VIZFRAME_POLL: {
        /* Slice 107: chrome's viz shared bitmap, straight to the caller --
         * no JPEG, no base64, no pipe. Same struct as XFRAME_POLL so
         * chromewin reuses its blit path; the difference is that this
         * producer exists (census-proven) and tier 2.5's did not.
         * The VIEWPORT comes IN through info->w/h: the kernel matches
         * regions by w*h*4 and has no way to know the window size. */
        extern long vizframe_poll(uint32_t *, uint32_t *, uint32_t *,
                                  uint32_t *, void *, size_t);
        if (!a1) return -ABI_EINVAL;
        struct abi_xframe req;
        if (copy_from_user(&req, (const void *)(uintptr_t)a1, sizeof req) != 0)
            return -ABI_EFAULT;
        uint32_t w = req.w, h = req.h, stride = 0, gen = 0;
        void  *upix = (void *)(uintptr_t)a2;
        size_t cap  = (size_t)a3;
        uint8_t *tmp = 0;
        if (upix && cap) {
            tmp = kmalloc(cap);
            if (!tmp) return -ABI_ENOMEM;
        }
        long rc = vizframe_poll(&w, &h, &stride, &gen, tmp, cap);
        if (rc < 0) { if (tmp) kfree(tmp); return rc; }
        struct abi_xframe xf = { w, h, stride, gen };
        if (copy_to_user((void *)(uintptr_t)a1, &xf, sizeof xf) != 0)
            { if (tmp) kfree(tmp); return -ABI_EFAULT; }
        if (tmp) {
            size_t nbytes = (size_t)stride * h;
            if (nbytes > cap) nbytes = cap;
            if (copy_to_user(upix, tmp, nbytes) != 0)
                { kfree(tmp); return -ABI_EFAULT; }
            kfree(tmp);
        }
        return 0;
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
        memset(ktmp, 0, sizeof ktmp);          /* slice 86: see LX_readlink */
        int rc = vfs_readlink(kpath, ktmp, bufsz);
        if (rc == VFS_OK) {
            size_t n = 0;
            while (n < bufsz && ktmp[n]) n++;
            size_t w = n + 1 > bufsz ? bufsz : n + 1;   /* never past the buffer */
            if (copy_to_user(buf, ktmp, w) != 0) return -ABI_EFAULT;
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
    LX_ioctl = 16, LX_pread64 = 17, LX_pwrite64 = 18,
    LX_readv = 19, LX_writev = 20, LX_access = 21,
    LX_pipe = 22, LX_sched_yield = 24, LX_mremap = 25,
    LX_dup = 32, LX_dup2 = 33,
    LX_nanosleep = 35,
    LX_sendfile = 40, LX_getpid = 39, LX_clone = 56, LX_fork = 57,
    LX_vfork = 58, LX_execve = 59, LX_exit = 60, LX_wait4 = 61,
    LX_kill = 62, LX_setpgid = 109, LX_getpgrp = 111, LX_setsid = 112,
    LX_getpgid = 121, LX_setfsuid = 122, LX_setfsgid = 123,
    LX_uname = 63, LX_fcntl = 72, LX_getcwd = 79, LX_chdir = 80,
    LX_flock = 73, LX_fsync = 74, LX_fdatasync = 75, LX_rename = 82,
    LX_unlinkat = 263, LX_renameat = 264, LX_renameat2 = 316,
    LX_mkdir = 83, LX_unlink = 87, LX_chmod = 90, LX_fchmod = 91,
    LX_fchmodat = 268, LX_getuid = 102, LX_getgid = 104,
    LX_geteuid = 107, LX_getegid = 108, LX_getppid = 110,
    LX_prctl = 157,
    LX_arch_prctl = 158, LX_gettid = 186, LX_tkill = 200, LX_futex = 202,
    LX_getdents64 = 217, LX_set_tid_address = 218, LX_clock_gettime = 228,
    LX_exit_group = 231, LX_tgkill = 234, LX_openat = 257,
    /* Slice 90: chrome's crash path calls both; both used to be -ENOSYS. */
    LX_rt_sigtimedwait = 128, LX_rt_tgsigqueueinfo = 297,
    LX_newfstatat = 262, LX_set_robust_list = 273, LX_getrandom = 318,
    LX_readlink = 89, LX_readlinkat = 267,  /* B20: /proc/self/exe etc. */
    /* Slice 69: symlink(2)/symlinkat(2). vfs_symlink() has existed since the
     * /proc work but was never reachable from Linux personality, so
     * symlink() returned ENOSYS -- which is precisely why the FULL chrome
     * binary died at startup: its ProcessSingleton ("one instance per
     * profile", a thing chrome-headless-shell does not have) creates a
     * SingletonLock SYMLINK in the profile dir and treats failure as fatal
     * (chrome_main_delegate.cc:520). */
    LX_symlink = 88, LX_symlinkat = 266,
    LX_dup3 = 292, LX_pipe2 = 293,   /* B12: shell pipelines (dup3/pipe2) */
    /* B13: readiness multiplexing. */
    LX_poll = 7, LX_select = 23, LX_epoll_create = 213, LX_epoll_wait = 232,
    LX_epoll_ctl = 233, LX_pselect6 = 270, LX_ppoll = 271, LX_epoll_pwait = 281,
    LX_epoll_create1 = 291,
    /* B14: BSD sockets (event-driven TCP servers). */
    LX_socket = 41, LX_connect = 42, LX_accept = 43, LX_sendto = 44,
    LX_recvfrom = 45, LX_sendmsg = 46, LX_recvmsg = 47,
    LX_shutdown = 48, LX_bind = 49, LX_listen = 50,
    LX_getsockname = 51, LX_getpeername = 52, LX_socketpair = 53,
    LX_setsockopt = 54,
    LX_getsockopt = 55, LX_accept4 = 288,
    LX_gettimeofday = 96, LX_sysinfo = 99, LX_getresuid = 118,
    LX_getresgid = 120, LX_statfs = 137, LX_fstatfs = 138,
    LX_getpriority = 140, LX_setpriority = 141,
    LX_sched_setaffinity = 203, LX_sched_getaffinity = 204,
    LX_clock_getres = 229,
    LX_time = 201, LX_inotify_add_watch = 253, LX_inotify_rm_watch = 254,
    LX_fallocate = 285, LX_statx = 332,
    /* B17: real busybox network clients arm an alarm-timeout (setitimer) around
     * each network op and ftruncate the wget output file. */
    LX_ftruncate = 77, LX_getitimer = 36, LX_setitimer = 38,
    /* B25: syscalls glibc 2.41 startup/pthread/malloc issue that previously
     * fell through to -ENOSYS. */
    LX_madvise = 28, LX_getrlimit = 97, LX_setrlimit = 160,
    LX_prlimit64 = 302, LX_rseq = 334, LX_clone3 = 435,
    /* realbash: PATH lookups / `test -e/-x` use the *at access variants. */
    LX_faccessat = 269, LX_faccessat2 = 439,
    /* Track C: libcurl's multi handle creates an eventfd as its wakeup fd. */
    LX_eventfd = 284, LX_eventfd2 = 290,
    /* Slice 38: chrome's headless screenshot writer (base::WriteFile ->
     * base::File) creates the PNG with creat() and chowns it -- creat was
     * the LAST blocker between a captured frame and /data/shot.png. */
    LX_creat = 85, LX_fchown = 93,
    LX_sched_getparam = 143, LX_sched_getscheduler = 145,
    /* Tier 2.5: SysV shm for MIT-SHM / Ozone */
    LX_shmget = 29, LX_shmat = 30, LX_shmctl = 31, LX_shmdt = 67,
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

/* FNV-1a 64-bit, used to synthesize a stable, per-file st_ino from a path.
 * tobyOS's VFS has no real inode numbers, but glibc's ld.so dedups already-
 * loaded shared libraries by (st_dev, st_ino): if every file reports the same
 * ino, ld.so thinks the 2nd DSO it opens is identical to the 1st and reuses the
 * wrong link map (so e.g. libc.so.6 silently aliases libdl.so.2 and every
 * libc symbol version lookup fails). A path/identity hash gives each file a
 * distinct, deterministic ino so dedup behaves. */
static uint64_t lx_ino_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;   /* FNV offset basis */
    if (s) for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    if (h <= 1) h += 2;                     /* never 0/1 (1 was the old stub) */
    return h;
}

/* Build a Linux struct stat from a tobyOS vfs_stat and copy it out. `ino` is a
 * synthetic-but-stable inode number (see lx_ino_hash) so distinct files get
 * distinct inodes -- required for glibc ld.so's shared-library dedup. */
/* Slice 99 (tier 3 Phase 1c): a DRM fd must stat as a CHARACTER DEVICE.
 *
 * Measured gap-list entry: Mesa's gbm_create_device() opened
 * /dev/dri/renderD128 and then returned NULL having issued ZERO ioctls
 * -- it fstats the fd first and bails unless S_ISCHR. Our generic
 * emitter only knows FILE/DIR, so every DRM fd looked like a regular
 * file and GBM refused it before asking the device anything.
 *
 * Linux's DRM major is 226 (render nodes start at minor 128). dev_t is
 * the split encoding: minor bits 0-7 and 20+, major bits 8-19. */
#define DRM_CHR_MAJOR 226u

/* Slice 103: GPU-path access trace. Four slices in a row guessed which
 * file libdrm wanted, built it, and changed nothing (see the ledger).
 * The library never says which access failed -- so log the accesses
 * ourselves, filtered to the paths this machinery touches, with their
 * results. Cheap, and it ends the guessing. */
static bool gpupath(const char *p) {
    if (!p) return false;
    return strncmp(p, "/sys/dev/char", 13) == 0 ||
           strncmp(p, "/sys/class/drm", 14) == 0 ||
           strncmp(p, "/sys/bus/pci", 12) == 0 ||
           strncmp(p, "/sys/devices/pci", 16) == 0 ||
           strncmp(p, "/dev/dri", 8) == 0;
}
/* Slice 104: is `path` one of our DRM nodes? Returns the char-device
 * minor (renderD128 -> 128, card0 -> 0) or -1. MEASURED need: the
 * [gpupath] trace showed `stat /dev/dri/card0 -> -1`, because the nodes
 * live only in the open() handler and vfs_stat has never heard of them.
 * Anything that stats a node BY NAME before opening it -- which is what
 * device enumeration does for every directory entry -- saw ENOENT. */
static int lx_dev_dri_minor(const char *path) {
    if (!path || strncmp(path, "/dev/dri/", 9) != 0) return -1;
    const char *leaf = path + 9;
    if (strcmp(leaf, "renderD128") == 0) return 128;
    if (strcmp(leaf, "card0") == 0)      return 0;
    return -1;
}

static void gputrace(const char *op, const char *path, long rc) {
    static int n;
    if (!gpupath(path) || n >= 120) return;
    n++;
    kprintf("[gpupath] %-10s %-52s -> %ld\n", op, path, rc);
}
static inline uint64_t lx_makedev(uint32_t maj, uint32_t min) {
    return ((uint64_t)(min & 0xffu))
         | ((uint64_t)(maj & 0xfffu) << 8)
         | ((uint64_t)(min & ~0xffu) << 12)
         | ((uint64_t)(maj & ~0xfffu) << 32);
}

static long linux_emit_chrdev_stat(uint32_t maj, uint32_t min, uint64_t ino,
                                   void *ubuf) {
    { static int n; if (n < 24) { n++;
        kprintf("[drmstat] chrdev emit %u:%u mode=S_IFCHR|0666\n", maj, min); } }
    struct lx_stat st;
    memset(&st, 0, sizeof st);
    st.st_mode    = 0x2000u | 0666u;          /* S_IFCHR | rw-rw-rw- */
    st.st_dev     = 6;                        /* devtmpfs-ish, non-zero */
    st.st_ino     = ino ? ino : 1;
    st.st_nlink   = 1;
    st.st_rdev    = lx_makedev(maj, min);
    st.st_blksize = 4096;
    if (copy_to_user(ubuf, &st, sizeof st) != 0) return -ABI_EFAULT;
    return 0;
}

static long linux_emit_stat(const struct vfs_stat *vs, uint64_t ino, void *ubuf) {
    struct lx_stat st;
    memset(&st, 0, sizeof st);
    uint32_t typ  = (vs->type == VFS_TYPE_DIR) ? 0x4000u : 0x8000u; /* S_IFDIR/REG */
    uint32_t perm = vs->mode & 0xFFFu;
    if (perm == 0) perm = (vs->type == VFS_TYPE_DIR) ? 0755u : 0644u;
    st.st_mode    = typ | perm;
    st.st_dev     = 1;
    st.st_ino     = ino ? ino : 1;
    /* 0 = unset -> the usual default (dirs 2, files 1). procfs sets a real
     * count for /proc/<pid>/task/, which chrome's sandbox tests. */
    st.st_nlink   = vs->nlink ? vs->nlink
                              : (vs->type == VFS_TYPE_DIR ? 2u : 1u);
    st.st_uid     = vs->uid;
    st.st_gid     = vs->gid;
    st.st_size    = (int64_t)vs->size;
    st.st_blksize = 512;
    st.st_blocks  = (int64_t)((vs->size + 511) / 512);
    if (copy_to_user(ubuf, &st, sizeof st) != 0) return -ABI_EFAULT;
    return 0;
}

/* statx(2): fill the modern struct statx (256-byte Linux x86-64 layout) from a
 * vfs_stat. We report STATX_BASIC_STATS (mode/size/ino/nlink/uid/gid/blocks);
 * timestamps are zero (no RTC epoch). glibc's stat() prefers statx now, so
 * filling it (vs -ENOSYS + a newfstatat fallback) cuts a syscall per stat. */
static long linux_emit_statx(const struct vfs_stat *vs, uint64_t ino, void *ubuf) {
    struct lx_statx_ts { int64_t sec; uint32_t nsec; int32_t pad; };
    struct lx_statx {
        uint32_t mask, blksize; uint64_t attributes;
        uint32_t nlink, uid, gid; uint16_t mode, spare0;
        uint64_t ino, size, blocks, attributes_mask;
        struct lx_statx_ts atime, btime, ctime, mtime;
        uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
        uint64_t mnt_id, spare2; uint64_t spare3[12];
    } sx;
    memset(&sx, 0, sizeof sx);
    uint32_t typ  = (vs->type == VFS_TYPE_DIR) ? 0x4000u : 0x8000u;
    uint32_t perm = vs->mode & 0xFFFu;
    if (perm == 0) perm = (vs->type == VFS_TYPE_DIR) ? 0755u : 0644u;
    sx.mask      = 0x000007ffu;              /* STATX_BASIC_STATS */
    sx.blksize   = 512;
    sx.nlink     = vs->nlink ? vs->nlink
                              : (vs->type == VFS_TYPE_DIR ? 2u : 1u);
    sx.uid       = vs->uid;
    sx.gid       = vs->gid;
    sx.mode      = (uint16_t)(typ | perm);
    sx.ino       = ino ? ino : 1;
    sx.size      = vs->size;
    sx.blocks    = (vs->size + 511) / 512;
    sx.dev_minor = 1;
    if (copy_to_user(ubuf, &sx, sizeof sx) != 0) return -ABI_EFAULT;
    return 0;
}

/* Synthesize a stable per-fd inode for fstat(). For VFS handles we hash the
 * backing node pointer (f->vfs.priv) -- distinct files => distinct nodes =>
 * distinct inodes, while two handles onto the same file share a node (so
 * glibc's ld.so sees them as the same object, which is correct). Directories
 * hash their path; other kinds fall back to an fd-derived value. */
static uint64_t lx_fd_ino(struct file *f, int fd) {
    if (f) {
        if (f->kind == FILE_KIND_DIR && f->dirpath)
            return lx_ino_hash(f->dirpath);
        if (f->kind == FILE_KIND_VFS) {
            /* Prefer the fs-assigned inode: two opens of the same file share it,
             * so the writable + read-only fds of chrome's shm temp file report
             * matching st_ino ("Writable and read-only inodes don't match"). */
            if (f->vfs.ino) {
                char buf[2 + 16 + 1];
                const char *hex = "0123456789abcdef";
                buf[0] = 'i'; buf[1] = ':';
                for (int i = 0; i < 16; i++)
                    buf[2 + i] = hex[(f->vfs.ino >> ((15 - i) * 4)) & 0xf];
                buf[18] = '\0';
                return lx_ino_hash(buf);
            }
            /* Fallback: hash the backing node pointer -- distinct files => distinct
             * nodes, and (in ramfs) two handles onto the same file share a node, so
             * ld.so's (st_dev,st_ino) dedup still works. */
            if (f->vfs.priv) {
                uintptr_t p = (uintptr_t)f->vfs.priv;
                char buf[2 + 16 + 1];
                const char *hex = "0123456789abcdef";
                buf[0] = 'n'; buf[1] = ':';
                for (int i = 0; i < 16; i++)
                    buf[2 + i] = hex[(p >> ((15 - i) * 4)) & 0xf];
                buf[18] = '\0';
                return lx_ino_hash(buf);
            }
        }
    }
    return lx_ino_hash("fd-fallback") + (uint64_t)(uint32_t)fd + 3;
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

    /* MAP_SHARED must actually SHARE. The copying path below gives every mapper
     * a private copy, which silently breaks cross-process shared memory --
     * chrome's Mojo bootstrap stalls on exactly this, because its
     * PlatformSharedMemoryRegion is a temp FILE, so the browser wrote into its
     * copy while the child waited on its own. Route MAP_SHARED through the
     * per-inode page cache so all mappers get the SAME physical pages.
     * Needs a stable inode (tobyfs supplies one; ino==0 means "no identity",
     * where sharing cannot be established and copying is the honest fallback).
     * Any failure falls through to the old path rather than failing the mmap. */
    if (lflags & LXMAP_SHARED) {
        bool created = false;
        /* Identity order matters. A region already mapped through THIS open file
         * (or an inherited/dup'd/SCM_RIGHTS copy of it) is found via the file
         * itself -- the only identity that survives the unlink() chrome does
         * immediately after creating a region. Otherwise fall back to
         * (inode, incarnation), which lets two SEPARATE opens of the same live
         * file share as POSIX requires -- chrome opens every shm region O_RDWR
         * and then O_RDONLY by path, and passes the read-only fd to children --
         * while a later file that merely inherits the recycled inode NUMBER
         * keys a different region. */
        struct shm_cache *sc = f->shm;
        if (!sc) sc = shm_cache_for_ino(f->vfs.ino, f->vfs.ino_gen, &created);
        /* No inode identity (ramfs et al.) -> fall through to the copying path,
         * as before this slice. Routing those through an anonymous region would
         * be more POSIX-correct in principle, but measured: every such mapping
         * here is a per-process mapping of chrome's .pak/ICU payload -- up to
         * 2656 pages EACH, CREATED by every process and attached by none. Cache
         * entries are permanent (no reclaim yet), so that traded a per-process
         * copy that is freed at teardown for one that is never freed. */
        if (sc) {
            f->shm = sc;                 /* pin for this fd and its clones */
            uint64_t alen = (len + 0xfffULL) & ~0xfffULL;
            uint64_t aoff = offset & ~0xfffULL;
            size_t page_off = (size_t)(aoff / PAGE_SIZE);
            size_t np       = (size_t)(alen / PAGE_SIZE);
            /* Sample the high-water mark BEFORE growing, so we can populate
             * exactly the pages this call newly allocated. Populating only on
             * `created` was wrong: a later, LARGER mapping of the same file
             * grows the cache, and those fresh pages would stay zero-filled
             * even though the file has content there -- which is what chrome's
             * persistent_memory_allocator reported as "Corruption detected in
             * shared-memory segment". Pages that already existed are NEVER
             * re-read: they are live shared memory by then and the on-disk
             * bytes are stale, so re-reading would clobber the peer's writes. */
            size_t before = shm_cache_npages(sc);
            if (shm_cache_ensure(sc, page_off + np) == 0) {
                size_t fill_from = before > page_off ? before : page_off;
                size_t fill_to   = page_off + np;
                if (fill_from < fill_to) {
                    size_t save_pos = f->vfs.pos;
                    f->vfs.pos = fill_from * PAGE_SIZE;
                    for (size_t i = fill_from; i < fill_to; i++) {
                        void *dst = shm_cache_page_ptr(sc, i);
                        if (!dst) break;
                        if (file_read(f, dst, PAGE_SIZE) <= 0) break; /* EOF: stays zero */
                    }
                    f->vfs.pos = save_pos;
                }
                long b = shm_cache_mmap(sc, addr, alen, prot,
                                        lx_mmap_flags(lflags), aoff);
#ifdef CHROMIUM_BOOT
                {   static int c = 0;
                    if (c < 200) { c++;
                        struct proc *me = current_proc();
                        kprintf("[shm] MAP_SHARED pid=%d ino=%lu/%lu off=%lu np=%lu "
                                "%s -> 0x%lx\n",
                                me ? me->pid : -1, (unsigned long)f->vfs.ino,
                                (unsigned long)f->vfs.ino_gen,
                                (unsigned long)page_off, (unsigned long)np,
                                created ? "CREATED" : "attached",
                                (unsigned long)b);
                    } }
#endif
                if (b >= 0) return b;
            }
        }
        /* fall through: table full / OOM / no base -- copy, as before */
    }

    /* Reserve + map writable anon pages so we can fill them. */
    uint32_t tflags = lx_mmap_flags(lflags) | 0x01u /* VMA_FLAG_ANON */;
    long base = sys_mmap(addr, len, 0x1u | 0x2u /* PROT_READ|WRITE */,
                         tflags, -1, 0);
    if (base < 0) return base;

#ifdef CHROMIUM_BOOT
    /* Lib load map: every file-backed segment ld.so maps for a .so. Correlate a
     * .so-region crash rip (isr flags LIB?) with a base+offset to symbolize it
     * (objdump the .so at rip-base+seg_off). off==0 lines are each .so's base. */
    {
        static int lm = 0;
        if (lm < 400) { lm++;
            struct proc *me = current_proc();
            kprintf("[libmap] pid=%d base=0x%lx len=0x%lx prot=0x%x off=0x%lx fd=%d "
                    "ino=%lu\n", me ? me->pid : -1,
                    (unsigned long)base, (unsigned long)len, prot,
                    (unsigned long)offset, fd, (unsigned long)f->vfs.ino);
        }
    }
#endif

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

/* mremap(old_addr, old_size, new_size, flags, new_addr) -- resize a mapping.
 * Used by glibc/musl realloc and CPython's obmalloc arena resize. tobyOS has
 * no in-place VMA-grow primitive, so: shrinking frees the tail in place; growing
 * (only with MREMAP_MAYMOVE, which malloc always passes) allocates a fresh anon
 * region, copies the old bytes across (both regions are mapped in the current
 * address space during the syscall), and unmaps the old one. */
#define LX_MREMAP_MAYMOVE 1u
static long linux_mremap(uint64_t old_addr, uint64_t old_sz,
                         uint64_t new_sz, uint32_t flags) {
    if (old_sz == 0 || new_sz == 0) return -ABI_EINVAL;
    if (old_addr & 0xfffULL) return -ABI_EINVAL;     /* must be page-aligned */
    uint64_t os = (old_sz + 0xfffULL) & ~0xfffULL;
    uint64_t ns = (new_sz + 0xfffULL) & ~0xfffULL;
    if (ns == os) return (long)old_addr;             /* no size change */
    if (ns < os) {                                   /* shrink: free the tail */
        sys_munmap(old_addr + ns, os - ns);
        return (long)old_addr;
    }
    if (!(flags & LX_MREMAP_MAYMOVE)) return -ABI_ENOMEM;  /* can't grow in place */
    long nb = sys_mmap(0, ns, 0x1u | 0x2u /*RW*/, 0x05 /*ANON|PRIVATE*/, -1, 0);
    if (nb < 0) return nb;
    uint8_t *buf = (uint8_t *)kmalloc(4096);
    if (!buf) { sys_munmap((uint64_t)nb, ns); return -ABI_ENOMEM; }
    for (uint64_t off = 0; off < os; off += 4096) {
        size_t chunk = (os - off) > 4096 ? 4096 : (size_t)(os - off);
        if (copy_from_user(buf, (const void *)(old_addr + off), chunk) != 0)
            break;                                   /* PROT_NONE tail: stop */
        (void)copy_to_user((void *)((uint64_t)nb + off), buf, chunk);
    }
    kfree(buf);
    sys_munmap(old_addr, os);
    return nb;
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
#define LX_DT_CHR  2      /* slice 102: /dev/dri nodes are char devices */

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

/* ===================================================================
 * Track B/B13: poll / select / epoll -- readiness multiplexing.
 *
 * One readiness predicate (file_poll_ready) computes the conditions currently
 * true for an fd; poll/ppoll/select/pselect6/epoll_wait all loop over their fd
 * lists calling it and block COOPERATIVELY (sched_yield) until at least one fd
 * is ready or the timeout elapses. Because the scheduler is cooperative,
 * yielding lets the other end of a pipe / a peer proc run and make the fd
 * ready, which we observe on the next pass.
 *
 * Scope/honesty: regular files are always ready (POSIX); pipes are exact (data/
 * space/EOF/EPIPE off the live ring + reader/writer counts); UDP sockets report
 * POLLIN off the queued-datagram count + always POLLOUT. TCP-socket POLLIN and
 * tty/console input-readiness are best-effort (reported writable; their input
 * side isn't peekable here) -- a documented limit, not exercised by the proof.
 * =================================================================== */

/* Defined with the socket syscalls below; file_poll_ready needs it to resolve
 * an in-flight non-blocking connect into a readiness edge. */
static int lx_conn_progress(struct sock *s);

#define LXP_POLLIN   0x001
#define LXP_POLLPRI  0x002
#define LXP_POLLOUT  0x004
#define LXP_POLLERR  0x008
#define LXP_POLLHUP  0x010
#define LXP_POLLNVAL 0x020
#define LX_EINTR     4

static short file_poll_ready(struct file *f) {
    if (!f) return LXP_POLLNVAL;
    short r = 0;
    switch (f->kind) {
    case FILE_KIND_VFS:
    case FILE_KIND_DIR:
        r |= LXP_POLLIN | LXP_POLLOUT;       /* regular files never block */
        break;
    case FILE_KIND_PIPE_R:
        if (f->pipe) {
            if (f->pipe->count > 0)    r |= LXP_POLLIN;
            if (f->pipe->writers == 0) r |= LXP_POLLIN | LXP_POLLHUP; /* EOF readable */
        }
        break;
    case FILE_KIND_PIPE_W:
        if (f->pipe) {
            if (f->pipe->readers == 0)             r |= LXP_POLLERR;
            else if (f->pipe->count < PIPE_BUF_SZ) r |= LXP_POLLOUT;
        }
        break;
    case FILE_KIND_SOCKET:
        if (f->sock && f->sock->kind == SOCK_KIND_NETLINK) {
            r |= LXP_POLLOUT;                          /* requests always accepted */
            if (f->sock->count > 0) r |= LXP_POLLIN;   /* a dump reply is queued */
        } else if (f->sock && f->sock->kind == SOCK_KIND_TCP) {
            /* B14: real TCP readiness. A listening socket is "readable" when a
             * handshake has completed (accept won't block); a connected socket
             * reports recv/send/hup/err from the live connection state. */
            struct tcp_conn *c = f->sock->tcp;
            if (f->sock->connecting) {
                /* A connect() still in flight. Linux signals completion by
                 * making the fd WRITABLE (success) or writable+error (failure)
                 * -- this is the edge chrome waits on, so it must be reported
                 * here or a non-blocking connect never resolves. */
                int pr = lx_conn_progress(f->sock);
                if (pr == 1)      r |= LXP_POLLOUT;
                else if (pr < 0)  r |= LXP_POLLOUT | LXP_POLLERR;
                break;                                 /* still connecting: no events */
            }
            if (f->sock->tcp_listening) {
                if (tcp_can_accept(c)) r |= LXP_POLLIN;
            } else if (c) {
                int tf = tcp_poll_flags(c);
                if (tf & TCP_RDY_RECV) r |= LXP_POLLIN;
                if (tf & TCP_RDY_SEND) r |= LXP_POLLOUT;
                if (tf & TCP_RDY_HUP)  r |= LXP_POLLHUP;
                if (tf & TCP_RDY_ERR)  r |= LXP_POLLERR;
            }
        } else {
            /* Writable only while the peer's RX ring has room. Reporting POLLOUT
             * unconditionally is what let chrome keep writing into a full ring
             * (which then dropped messages). Now a full peer ring reads as
             * not-writable, so chrome waits for POLLOUT and retries after EAGAIN
             * -- real backpressure, no loss. */
            if (!(f->sock && sock_unix_send_would_block(f->sock)))
                r |= LXP_POLLOUT;                    /* UDP (no peer) or room */
            /* Tier 2.5: keep fake-X clients from wedging in wait_for_event. */
            if (f->sock && f->sock->x_server) {
                extern void xserver_on_poll(struct sock *);
                xserver_on_poll(f->sock);
            }
            if (f->sock && f->sock->count > 0) r |= LXP_POLLIN;  /* rx queued */
            /* AF_UNIX socketpair: a closed peer makes us readable (EOF) + HUP,
             * so Mojo's epoll loop detects disconnect. The fake-X-server loopback
             * has no peer proc (peer_ip==0 by design) -- it is readable only when
             * it has actually queued a reply, never HUP. */
            /* Slice 86: a bound LISTENER also has no peer, but it is not
             * hung up -- it is readable exactly when a connect is queued.
             * Without this exclusion the EOF rule above would report every
             * listening socket as POLLHUP the moment it was created. */
            if (f->sock && f->sock->kind == SOCK_KIND_UNIX &&
                sock_unix_bound_name(f->sock)) {
                if (sock_unix_can_accept(f->sock)) r |= LXP_POLLIN;
            } else if (f->sock && f->sock->kind == SOCK_KIND_UNIX &&
                       f->sock->peer_ip == 0 && !f->sock->x_server)
                r |= LXP_POLLIN | LXP_POLLHUP;
        }
        break;
    case FILE_KIND_DEVNULL:                  /* bit-bucket + entropy devices: */
    case FILE_KIND_DEVZERO:                  /* never block in either direction */
    case FILE_KIND_DEVRANDOM:
        r |= LXP_POLLIN | LXP_POLLOUT;
        break;
    case FILE_KIND_CONSOLE:                  /* B22: real TTY input readiness */
        r |= LXP_POLLOUT;                    /* always writable */
        if (tty_console_readable()) r |= LXP_POLLIN;
        break;
    case FILE_KIND_PTY_MASTER:               /* B23: pseudoterminal readiness */
    case FILE_KIND_PTY_SLAVE:
        r |= LXP_POLLOUT;
        if (pty_readable(f)) r |= LXP_POLLIN;
        if (pty_hup(f))      r |= LXP_POLLHUP;
        break;
    case FILE_KIND_EVENTFD:                  /* Track C: eventfd readiness */
        r |= LXP_POLLOUT;                    /* writable unless at max (rare) */
        if (eventfd_pollin(f)) r |= LXP_POLLIN;
        break;
    default:                                 /* term/window/etc. */
        r |= LXP_POLLOUT;                    /* writable; input best-effort */
        break;
    }
    return r;
}

struct lx_pollfd { int fd; short events; short revents; };  /* 8 bytes, Linux ABI */
#define LX_POLL_MAXFDS 256

/* Core poll loop shared by poll/ppoll. timeout_ms < 0 == infinite. */
/* Wall-clock nanoseconds, given the current monotonic reading.
 *
 * The RTC only has 1-second granularity, so sampling it per call would make
 * CLOCK_REALTIME jump in whole seconds and stand still in between -- the same
 * "time isn't moving" hazard we are fixing. Instead anchor ONCE (epoch minus
 * the monotonic reading at that instant) and thereafter drive realtime from the
 * calibrated TSC, so it advances smoothly at nanosecond resolution and never
 * runs backwards. */
static uint64_t lx_realtime_ns(uint64_t mono_ns) {
    static uint64_t s_epoch_base_ns = 0;   /* unix_ns - mono_ns at anchor time */
    if (s_epoch_base_ns == 0) {
        uint64_t secs = rtc_unix_time();
        if (secs == 0) return mono_ns;     /* no RTC: monotonic is all we have */
        s_epoch_base_ns = secs * 1000000000ull - mono_ns;
    }
    return s_epoch_base_ns + mono_ns;
}

/* Slice 56: CLOCK_REALTIME minus CLOCK_MONOTONIC, for converting an absolute
 * realtime deadline (glibc FUTEX_CLOCK_REALTIME waits: default-clock
 * pthread_cond_timedwait / sem_timedwait) into the monotonic timebase every
 * kernel deadline comparison uses. 0 when there is no RTC (realtime ==
 * monotonic then, so the conversion is a no-op). */
uint64_t lx_realtime_offset_ns(void) {
    uint64_t mono = perf_now_ns();
    return lx_realtime_ns(mono) - mono;
}

/* Slice 39 freeze triage: prove (or refute) that a poll(2) spinner is alive.
 * Bumped once per lx_do_poll wait iteration; read by the sched.c heartbeat. */
uint64_t g_lx_poll_iters;
int      g_lx_poll_last_pid = -1;

#ifdef CHROMIUM_BOOT
/* [clkchk] (slice 41): cross-check that CLOCK_MONOTONIC is actually monotonic
 * and jump-free as chrome sees it. perf_now_ns() computes ns from ONE global
 * boot-TSC, but each CPU has its own raw TSC; under WHPX (unlike TCG, which
 * gives all vCPUs one virtual TSC) those can be unsynchronised, so a thread
 * migrating CPUs would see the clock leap or run backwards. Chrome's TimeTicks
 * MUST be monotonic; either pathology wrecks its deadline math and is a prime
 * suspect for the WHPX clock_gettime storm / "timers fire 100s late". One
 * shared last-value across all CPUs is intentional: interleaved reads from two
 * CPUs whose TSCs disagree produce exactly the >250ms / negative delta we log
 * (with a CPU-CHANGED tag). Threshold is far above any legit read interleave. */
static void clkchk_probe(uint64_t mono) {
    static uint64_t last_mono;
    static int      last_cpu = -1;
    static int      hits;
    struct percpu *pc = smp_this_cpu();
    int cpu = pc ? (int)pc->cpu_idx : -1;
    uint64_t lm = last_mono;
    if (lm) {
        int64_t d = (int64_t)(mono - lm);
        if ((d < 0 || d > 250000000ll) && hits < 40) {
            hits++;
            kprintf("[clkchk] cpu=%d(prev %d) mono=%lums last=%lums "
                    "delta=%ldms%s\n",
                    cpu, last_cpu,
                    (unsigned long)(mono / 1000000ull),
                    (unsigned long)(lm / 1000000ull),
                    (long)(d / 1000000ll),
                    (cpu != last_cpu) ? " [CPU-CHANGED]" : "");
        }
    }
    last_mono = mono;
    last_cpu  = cpu;
}
#endif

/* Lock-free fast path for pure time-read syscalls (slice 41). These read only
 * the monotonic TSC clock -- no mutable kernel state -- and write a tiny result
 * to user memory, so when that buffer is ALREADY resident+writable we serve
 * them WITHOUT the Big Kernel Lock. Under WHPX chrome's message pumps issue
 * clock_gettime/gettimeofday tens of thousands of times a second; taking the
 * BKL for each turns them into the system-wide throughput cap and starves the
 * threads that must run to answer captureScreenshot. Returns 1 iff fully
 * served (result in *out); 0 means "not handled here" -- the caller must fall
 * through to the normal BKL path (unknown clock id, or a non-resident buffer
 * that needs the fault/allocate path). No signals are pending (caller checked),
 * so there is nothing else the syscall-return path had to do. */
static int linux_fast_time_syscall(long num, long a1, long a2, long *out) {
    switch (num) {
    case LX_clock_gettime: {
        uint64_t mono = perf_now_ns();
#ifdef CHROMIUM_BOOT
        clkchk_probe(mono);
#endif
        uint64_t val;
        switch ((int)a1) {
        case 0: case 5: case 8:                 /* CLOCK_REALTIME family */
            val = lx_realtime_ns(mono); break;
        case 2: case 3:                          /* PROCESS/THREAD_CPUTIME */
        case 1: case 4: case 6: case 7: case 9:  /* MONOTONIC / BOOTTIME */
            val = mono; break;
        default:
            return 0;                            /* unknown id: normal path -> EINVAL */
        }
        if (a2 == 0) { *out = 0; return 1; }
        struct lx_timespec ts = { (int64_t)(val / 1000000000ull),
                                  (int64_t)(val % 1000000000ull) };
        if (copy_to_user_nofault((void *)a2, &ts, sizeof ts) != 0)
            return 0;                            /* not resident: BKL path handles it */
        *out = 0;
        return 1;
    }
    case LX_gettimeofday: {
        uint64_t mono = perf_now_ns();
#ifdef CHROMIUM_BOOT
        clkchk_probe(mono);
#endif
        if (a1 == 0) { *out = 0; return 1; }     /* tz (a2) ignored, matches slow path */
        uint64_t ns = lx_realtime_ns(mono);
        int64_t tv[2] = { (int64_t)(ns / 1000000000ull),
                          (int64_t)((ns / 1000ull) % 1000000ull) };
        if (copy_to_user_nofault((void *)a1, tv, sizeof tv) != 0)
            return 0;
        *out = 0;
        return 1;
    }
    case LX_clock_getres: {
        if (a2 == 0) { *out = 0; return 1; }
        struct lx_timespec ts = { 0, 1 };        /* 1 ns resolution */
        if (copy_to_user_nofault((void *)a2, &ts, sizeof ts) != 0)
            return 0;
        *out = 0;
        return 1;
    }
    default:
        return 0;
    }
}

static long lx_do_poll(uint64_t ufds, unsigned long nfds, long timeout_ms) {
    if (nfds > LX_POLL_MAXFDS) return -ABI_EINVAL;
    struct lx_pollfd fds[LX_POLL_MAXFDS];
    if (nfds && copy_from_user(fds, (const void *)(uintptr_t)ufds,
                               nfds * sizeof(fds[0])) != 0)
        return -ABI_EFAULT;
    struct proc *self = current_proc();
    bool infinite = (timeout_ms < 0);
    uint64_t po_start = perf_now_ns();
    uint64_t deadline = infinite ? 0
                                 : po_start + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        int nready = 0;
        bool saw_sock = false;
        for (unsigned long i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;           /* negative fd: ignored */
            struct file *f = fd_lookup(fds[i].fd);
            if (f && f->kind == FILE_KIND_SOCKET) saw_sock = true;
            short cond = file_poll_ready(f);
            short want = fds[i].events | LXP_POLLERR | LXP_POLLHUP | LXP_POLLNVAL;
            short rev  = cond & want;
            fds[i].revents = rev;
            if (rev) nready++;
        }
        if (nready > 0 || timeout_ms == 0 ||
            (!infinite && perf_now_ns() >= deadline)) {
#ifdef CHROMIUM_BOOT
            /* Slice 88: the headed UI thread poll(2)-times-out every 500ms
             * while its X socket sits at count=8 unread events ([xsrv] poke
             * MUTED). Decide between "the X fd is not in the caller's poll
             * set" and "it is, but the readiness predicate lies": on a
             * ZERO-ready timeout, if ANY fd in the set is an x_server sock
             * with data queued, that is the predicate lying -- log it. If
             * this never fires while the mute persists, the fd is absent
             * from the set and the bug is in chrome's watcher registration
             * (or an fd we handed out wrong). */
            if (nready == 0 && timeout_ms != 0) {
                static int xp = 0;
                for (unsigned long i = 0; xp < 24 && i < nfds; i++) {
                    if (fds[i].fd < 0) continue;
                    struct file *xf = fd_lookup(fds[i].fd);
                    if (xf && xf->kind == FILE_KIND_SOCKET && xf->sock &&
                        xf->sock->x_server && xf->sock->count > 0) {
                        xp++;
                        kprintf("[xpoll] pid=%d fd=%d XSRV count=%u events=0x%x "
                                "cond=0x%x -> NOT READY?!\n",
                                self ? self->pid : -1, fds[i].fd,
                                (unsigned)xf->sock->count,
                                (unsigned)fds[i].events,
                                (unsigned)file_poll_ready(xf));
                    }
                }
            }
            /* Slice 56: finite-timeout poll(2) overshoot (see [eplate]). */
            if (!infinite && timeout_ms > 0) {
                uint64_t act = perf_now_ns() - po_start;
                if (act > (uint64_t)timeout_ms * 1000000ull + 500000000ull) {
                    static int pl = 0;
                    if (pl < 48) { pl++;
                        kprintf("[polate] pid=%d to=%ldms act=%lums n=%d\n",
                                self ? self->pid : -1, timeout_ms,
                                (unsigned long)(act / 1000000ull), nready);
                    }
                }
            }
#endif
            if (nfds && copy_to_user((void *)(uintptr_t)ufds, fds,
                                     nfds * sizeof(fds[0])) != 0)
                return -ABI_EFAULT;
            return nready;
        }
        if (self && self->pending_signals) return -LX_EINTR;
        g_lx_poll_iters++;
        if (self) g_lx_poll_last_pid = self->pid;
        /* B14: drive RX so inbound TCP segments (handshakes completing on the
         * listener, data arriving on a conn, peer FIN) make a watched socket
         * ready while we cooperatively block -- the same pump tcp_accept uses. */
        if (saw_sock) net_poll();
        poll_wait_block();   /* slice 43: BLOCK (leave run queue), don't busy-spin */
    }
}

/* select(2): three fd_set bitmaps (byte arrays). Caps at PROC_NFDS. */
static long lx_do_select(int nfds, uint64_t urd, uint64_t uwr, uint64_t uex,
                         long timeout_ms) {
    if (nfds < 0) return -ABI_EINVAL;
    if (nfds > PROC_NFDS) nfds = PROC_NFDS;
    unsigned nbytes = (unsigned)((nfds + 7) / 8);
    uint8_t rd[ (PROC_NFDS + 7) / 8 ] = {0};
    uint8_t wr[ (PROC_NFDS + 7) / 8 ] = {0};
    uint8_t ex[ (PROC_NFDS + 7) / 8 ] = {0};
    if (urd && nbytes && copy_from_user(rd, (const void *)(uintptr_t)urd, nbytes)) return -ABI_EFAULT;
    if (uwr && nbytes && copy_from_user(wr, (const void *)(uintptr_t)uwr, nbytes)) return -ABI_EFAULT;
    if (uex && nbytes && copy_from_user(ex, (const void *)(uintptr_t)uex, nbytes)) return -ABI_EFAULT;

    struct proc *self = current_proc();
    bool infinite = (timeout_ms < 0);
    uint64_t deadline = infinite ? 0
                                 : perf_now_ns() + (uint64_t)timeout_ms * 1000000ull;
    uint8_t ord[(PROC_NFDS + 7) / 8], owr[(PROC_NFDS + 7) / 8], oex[(PROC_NFDS + 7) / 8];
    for (;;) {
        memset(ord, 0, sizeof(ord)); memset(owr, 0, sizeof(owr)); memset(oex, 0, sizeof(oex));
        int nready = 0;
        bool saw_sock = false;
        for (int fd = 0; fd < nfds; fd++) {
            int bit = 1 << (fd & 7), by = fd >> 3;
            bool wr_r = (urd && (rd[by] & bit));
            bool wr_w = (uwr && (wr[by] & bit));
            bool wr_e = (uex && (ex[by] & bit));
            if (!wr_r && !wr_w && !wr_e) continue;
            struct file *f = fd_lookup(fd);
            if (f && f->kind == FILE_KIND_SOCKET) saw_sock = true;
            short cond = file_poll_ready(f);
            if (wr_r && (cond & (LXP_POLLIN | LXP_POLLHUP | LXP_POLLERR))) { ord[by] |= bit; nready++; }
            if (wr_w && (cond & (LXP_POLLOUT | LXP_POLLERR)))             { owr[by] |= bit; nready++; }
            if (wr_e && (cond & LXP_POLLPRI))                            { oex[by] |= bit; nready++; }
        }
        if (nready > 0 || timeout_ms == 0 ||
            (!infinite && perf_now_ns() >= deadline)) {
            if (urd && nbytes && copy_to_user((void *)(uintptr_t)urd, ord, nbytes)) return -ABI_EFAULT;
            if (uwr && nbytes && copy_to_user((void *)(uintptr_t)uwr, owr, nbytes)) return -ABI_EFAULT;
            if (uex && nbytes && copy_to_user((void *)(uintptr_t)uex, oex, nbytes)) return -ABI_EFAULT;
            return nready;
        }
        if (self && self->pending_signals) return -LX_EINTR;
        if (saw_sock) net_poll();              /* B14: pump RX (see lx_do_poll) */
        poll_wait_block();   /* slice 43: BLOCK, don't busy-spin */
    }
}

/* ---- epoll ---- */
#define EPOLL_MAX 64
struct epoll_entry { int fd; uint32_t events; uint64_t data; bool used; };
struct epoll_inst  { struct epoll_entry e[EPOLL_MAX]; };
struct lx_epoll_event { uint32_t events; uint64_t data; } __attribute__((packed)); /* 12 B */
#define LX_EPOLL_CTL_ADD 1
#define LX_EPOLL_CTL_DEL 2
#define LX_EPOLL_CTL_MOD 3

static long lx_epoll_create(void) {
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return -ABI_ENOMEM;
    memset(f, 0, sizeof(*f));
    f->kind  = FILE_KIND_EPOLL;
    f->epoll = (struct epoll_inst *)kmalloc(sizeof(struct epoll_inst));
    if (!f->epoll) { kfree(f); return -ABI_ENOMEM; }
    memset(f->epoll, 0, sizeof(struct epoll_inst));
    int fd = fd_alloc_into(current_proc(), f);
    if (fd < 0) { kfree(f->epoll); kfree(f); return -ABI_EMFILE; }
    return fd;
}

static long lx_epoll_ctl(int epfd, int op, int fd, uint64_t uevent) {
    struct file *ef = fd_lookup(epfd);
    if (!ef || ef->kind != FILE_KIND_EPOLL || !ef->epoll) return -ABI_EBADF;
    if (!fd_lookup(fd)) return -ABI_EBADF;
    struct epoll_inst *ei = ef->epoll;
    struct lx_epoll_event ev = {0};
    if (op == LX_EPOLL_CTL_ADD || op == LX_EPOLL_CTL_MOD) {
        if (!uevent || copy_from_user(&ev, (const void *)(uintptr_t)uevent, sizeof(ev)))
            return -ABI_EFAULT;
    }
    if (op == LX_EPOLL_CTL_ADD) {
        int free_slot = -1;
        for (int i = 0; i < EPOLL_MAX; i++) {
            if (ei->e[i].used && ei->e[i].fd == fd) return -17 /* EEXIST */;
            if (!ei->e[i].used && free_slot < 0) free_slot = i;
        }
        if (free_slot < 0) return -ABI_ENOMEM;
        ei->e[free_slot] = (struct epoll_entry){ fd, ev.events, ev.data, true };
#ifdef CHROMIUM_BOOT
        /* [epreg] what chrome's IO threads actually poll on. The renderer's
         * IO threads block in epoll_wait forever after the bootstrap thread
         * exits; this shows whether they registered the channel SOCKET (kind 3)
         * or an EVENTFD (kind 12 -- the Mojo shared-memory notification), which
         * is the transport that must wake them and evidently does not. */
        {   static int er = 0;
            if (er < 200) { er++;
                struct file *rf = fd_lookup(fd);
                struct proc *me = current_proc();
                int eid = (rf && rf->kind == FILE_KIND_EVENTFD) ? eventfd_id(rf) : -1;
                kprintf("[epreg] pid=%d epfd=%d ADD fd=%d kind=%d efd_id=%d events=0x%x\n",
                        me ? me->pid : -1, epfd, fd,
                        rf ? (int)rf->kind : -1, eid, (unsigned)ev.events); } }
#endif
        return 0;
    }
    for (int i = 0; i < EPOLL_MAX; i++) {
        if (!ei->e[i].used || ei->e[i].fd != fd) continue;
        if (op == LX_EPOLL_CTL_DEL) { ei->e[i].used = false; return 0; }
        if (op == LX_EPOLL_CTL_MOD) { ei->e[i].events = ev.events; ei->e[i].data = ev.data; return 0; }
    }
    return -2 /* ENOENT */;
}

static long lx_epoll_wait(int epfd, uint64_t uevents, int maxevents, long timeout_ms) {
    struct file *ef = fd_lookup(epfd);
    if (!ef || ef->kind != FILE_KIND_EPOLL || !ef->epoll) return -ABI_EBADF;
    if (maxevents <= 0) return -ABI_EINVAL;
    struct epoll_inst *ei = ef->epoll;
    struct proc *self = current_proc();
    bool infinite = (timeout_ms < 0);
    uint64_t ep_start = perf_now_ns();
    uint64_t deadline = infinite ? 0
                                 : ep_start + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        int n = 0;
        bool saw_sock = false;
        for (int i = 0; i < EPOLL_MAX && n < maxevents; i++) {
            if (!ei->e[i].used) continue;
            struct file *f = fd_lookup(ei->e[i].fd);
            if (f && f->kind == FILE_KIND_SOCKET) saw_sock = true;
            short cond = file_poll_ready(f);
            uint32_t want = ei->e[i].events | LXP_POLLERR | LXP_POLLHUP;
            uint32_t rev  = (uint32_t)cond & want;
            if (!rev) continue;
            struct lx_epoll_event out = { rev, ei->e[i].data };
            if (copy_to_user((void *)(uintptr_t)(uevents + (uint64_t)n * sizeof(out)),
                             &out, sizeof(out)) != 0)
                return -ABI_EFAULT;
            n++;
        }
        if (n > 0 || timeout_ms == 0 ||
            (!infinite && perf_now_ns() >= deadline)) {
#ifdef CHROMIUM_BOOT
            /* Slice 56: a FINITE-timeout epoll_wait that returned far past its
             * bound = the poll_tick re-scan machinery let it oversleep. */
            if (!infinite && timeout_ms > 0) {
                uint64_t act = perf_now_ns() - ep_start;
                if (act > (uint64_t)timeout_ms * 1000000ull + 500000000ull) {
                    static int el = 0;
                    if (el < 48) { el++;
                        kprintf("[eplate] pid=%d epfd=%d to=%ldms act=%lums n=%d\n",
                                self ? self->pid : -1, epfd, timeout_ms,
                                (unsigned long)(act / 1000000ull), n);
                    }
                }
            }
#endif
            return n;
        }
        if (self && self->pending_signals) return -LX_EINTR;
        /* B14: epoll over sockets must pump RX while blocked (a full scan
         * before yielding would otherwise never see the handshake complete). */
        if (saw_sock) net_poll();
        poll_wait_block();   /* slice 43: BLOCK (leave run queue), don't busy-spin */
    }
}

/* ---- B14: Linux BSD sockets (event-driven TCP servers) ---------------
 *
 * POSIX sockets ARE file descriptors, so each socket is a FILE_KIND_SOCKET
 * struct file in the proc fd table -- this is what lets poll/select/epoll,
 * read/write and close all operate on a socket uniformly (file_poll_ready
 * already understands TCP readiness). The handlers wrap the kernel socket
 * pool (struct sock) and the TCP engine (tcp.c) directly rather than going
 * through ksock_* (which use a separate Win32-style handle space).
 *
 * The x86-64 syscall trap only delivers 5 register args here, so the 6-arg
 * sendto/recvfrom are supported only in their connected form (addr == NULL),
 * which is exactly what send()/recv() on a TCP socket use; the proof server
 * uses read()/write() and so is unaffected either way. */

/* errno values not in abi.h but expected by Linux-personality callers. */
#define LXE_EINTR          4
#define LXE_EAGAIN        11
#define LXE_EAFNOSUPPORT  97
#define LXE_ENOTSOCK      88
#define LXE_EADDRINUSE    98
#define LXE_EOPNOTSUPP    95
#define LXE_ECONNREFUSED 111
#define LXE_ENOTCONN     107
#define LXE_EINPROGRESS  115
#define LXE_ETIMEDOUT    110
#define LXE_EALREADY     114
#define LXE_EISCONN      106

#define LX_MSG_DONTWAIT  0x40         /* recv/send flag: never block this call */
#define LX_MSG_PEEK      0x02         /* recv flag: read without dequeuing */

#define LX_SOCK_DEF_ACCEPT_MS  3000   /* fallback accept() wait (cooperative) */
#define LX_SOCK_DEF_CONNECT_MS 5000   /* fallback connect() wait */

/* Wrap an allocated struct sock in a fresh FILE_KIND_SOCKET fd. */
static int lx_sock_install(struct sock *s) {
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return -1;
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_SOCKET;
    f->sock = s;
    f->sock_gen = sock_generation(s);        /* slice 89 */
    int fd = fd_alloc_into(current_proc(), f);
    if (fd < 0) { kfree(f); return -1; }
    return fd;
}

static struct sock *lx_sock_of(int fd) {
    struct file *f = fd_lookup(fd);
    if (!f || f->kind != FILE_KIND_SOCKET) return NULL;
    return f->sock;
}

/* eventfd2(initval, flags): create a counting eventfd. libcurl's multi handle
 * uses one (EFD_CLOEXEC|EFD_NONBLOCK) as its wakeup descriptor; without it
 * curl_multi_handle() fails and curl_easy_perform() returns CURLE_OUT_OF_MEMORY.
 * The fd is read/write/poll-capable (see file.c) and pollable in poll/epoll. */
/* Linux rejects unknown eventfd2 flag bits with -EINVAL, and chrome DEPENDS on
 * it: mojo/core/channel_linux.cc EventFDNotifier::KernelSupported() probes with
 * syscall(__NR_eventfd2, 0, ~0) and PCHECKs the call FAILS with EINVAL/ENOSYS/
 * EPERM (line 192). Accepting every flag handed it a valid fd -> FATAL. Same
 * probe idiom as memfd_create (channel_linux.cc:946). */
#define LX_EFD_KNOWN_FLAGS (EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK)

static long lx_eventfd(unsigned int initval, unsigned int flags) {
    if (flags & ~LX_EFD_KNOWN_FLAGS) return -ABI_EINVAL;
    struct file *f = eventfd_file_make(initval, flags);
    if (!f) return -ABI_ENOMEM;
    int fd = fd_alloc_into(current_proc(), f);
    if (fd < 0) { file_close(f); return -ABI_EMFILE; }
    return fd;
}

/* memfd_create(name, flags) (slice 20): an anonymous, page-backed,
 * mmap-COHERENT shared-memory fd. Chrome's compositor / base::*SharedMemoryRegion
 * memfd_create()s, ftruncate()s, then mmap()s it (often twice: a writable and a
 * read-only view) and relies on writes through one mapping being visible through
 * the other -- which tobyOS's copy-based file mmap can't do, but a memfd (mapping
 * the SAME physical pages) does. `name` is advisory (ignored); flags
 * (MFD_CLOEXEC/MFD_ALLOW_SEALING) are accepted but not tracked. */
#define LX_MFD_CLOEXEC       0x0001u
#define LX_MFD_ALLOW_SEALING 0x0002u
#define LX_MFD_HUGETLB       0x0004u
#define LX_MFD_NOEXEC_SEAL   0x0008u
#define LX_MFD_EXEC          0x0010u
#define LX_MFD_KNOWN_FLAGS  (LX_MFD_CLOEXEC | LX_MFD_ALLOW_SEALING | \
                             LX_MFD_HUGETLB | LX_MFD_NOEXEC_SEAL | LX_MFD_EXEC)

static long lx_memfd_create(const char *uname, unsigned int flags) {
    /* Linux REJECTS unknown flag bits with -EINVAL, and callers rely on it:
     * chrome's Mojo channel PROBES support by calling memfd_create(name, ~0u)
     * and expecting -EINVAL. Accepting everything (the old `(void)flags`) handed
     * it a valid fd, so it concluded the kernel was broken and hit the FATAL
     * Check at mojo/core/channel_linux.cc:947. Validate like Linux does. */
#ifdef CHROMIUM_BOOT
    /* Iter 20: log the NAME -- it identifies the memfd's owner subsystem
     * (V8 names its code memfds, Mojo its channels, etc). The JIT arena the
     * browser thread crashes executing from is memfd-backed; the name says
     * WHOSE JIT it is. */
    { static int c=0; if(c<48){c++;
        char nm[48]; int ni = 0;
        /* byte-at-a-time: a bulk copy of the full buffer can cross into an
         * unmapped page past a short name and spuriously fail */
        while (uname && ni < (int)sizeof nm - 1) {
            if (copy_from_user_nofault(&nm[ni], uname + ni, 1) != 0) break;
            if (!nm[ni]) break;
            ni++;
        }
        nm[ni] = 0;
        struct proc *mp = current_proc();
        kprintf("[memfd] pid=%d create name='%s' flags=0x%x%s\n",
            mp ? mp->pid : -1, nm, flags,
            (flags & ~LX_MFD_KNOWN_FLAGS) ? " -> EINVAL(unknown bits)" : " -> ok"); } }
#else
    (void)uname;
#endif
    if (flags & ~LX_MFD_KNOWN_FLAGS) return -ABI_EINVAL;
    struct memfd *mf = memfd_new();
    if (!mf) return -ABI_ENOMEM;
    struct file *f = (struct file *)kmalloc(sizeof *f);
    if (!f) { memfd_unref(mf); return -ABI_ENOMEM; }
    memset(f, 0, sizeof *f);
    f->kind      = FILE_KIND_MEMFD;
    f->memfd     = mf;
    f->o_accmode = 2;                 /* O_RDWR: a memfd is always read+write */
    int fd = fd_alloc_into(current_proc(), f);
    if (fd < 0) { file_close(f); return -ABI_EMFILE; }
#ifdef CHROMIUM_BOOT
    { static int c = 0; if (c < 24) { c++; kprintf("[memfd] create -> fd=%d\n", fd); } }
#endif
    return fd;
}

static long lx_socket(int domain, int type, int proto) {
    (void)proto;
    /* AF_UNIX stream/seqpacket/dgram: an in-memory IPC endpoint (SOCK_KIND_UNIX),
     * NOT networking -> no CAP_NET. Left unconnected until connect() (named X
     * socket) or handed to socketpair(). Chrome opens these for Mojo, D-Bus, and
     * ANGLE's xcb_connect(); returning EINVAL here was exactly D-Bus's "Failed to
     * open socket: Invalid argument" and blocked xcb from even trying. */
    if (domain == AF_UNIX) {
        int ut = type & 0xff;
        if (ut != SOCK_STREAM && ut != SOCK_DGRAM && ut != SOCK_SEQPACKET)
            return -ABI_EINVAL;
        struct sock *us = sock_alloc(SOCK_KIND_UNIX);
        if (!us) return -ABI_EMFILE;
        us->nonblock = (type & SOCK_NONBLOCK) != 0;
        us->sotype   = (uint8_t)ut;                 /* slice 80 */
        int ufd = lx_sock_install(us);
        if (ufd < 0) { sock_close(us); return -ABI_EMFILE; }
        return ufd;
    }
    /* AF_NETLINK/NETLINK_ROUTE: chrome's net::AddressTrackerLinux opens one to
     * enumerate interfaces and watch for link changes. Refusing it (the old
     * EINVAL) is the "Could not create NETLINK socket" error every run logged,
     * which leaves NetworkChangeNotifier unable to tell whether the machine is
     * online at all. See sock_netlink_send for the protocol we answer. */
    if (domain == AF_NETLINK) {
        if ((type & 0xff) != SOCK_RAW && (type & 0xff) != SOCK_DGRAM)
            return -ABI_EINVAL;
        if (proto != NETLINK_ROUTE) return -ABI_EINVAL;
        struct sock *ns = sock_alloc(SOCK_KIND_NETLINK);
        if (!ns) return -ABI_EMFILE;
        ns->nonblock = (type & SOCK_NONBLOCK) != 0;
        int nfd = lx_sock_install(ns);
        if (nfd < 0) { sock_close(ns); return -ABI_EMFILE; }
        return nfd;
    }
    /* We only implement IPv4. musl's getaddrinfo opens an AF_INET6 DNS socket
     * first and falls back to AF_INET *only* on EAFNOSUPPORT -- returning
     * EINVAL there made the fallback never happen (wget: "bad address"). */
    if (domain == AF_INET6) return -LXE_EAFNOSUPPORT;
    if (domain != AF_INET) return -ABI_EINVAL;
    int t = type & 0xff;                /* strip SOCK_NONBLOCK/SOCK_CLOEXEC */
    int kind;
    if (t == SOCK_STREAM)      kind = SOCK_KIND_TCP;
    else if (t == SOCK_DGRAM)  kind = SOCK_KIND_UDP;
    else return -ABI_EINVAL;
    if (!cap_check(current_proc(), CAP_NET, "lx_socket")) return -ABI_EACCES;
    struct sock *s = sock_alloc(kind);
    if (!s) return -ABI_EMFILE;
    /* Honour SOCK_NONBLOCK. This bit used to be stripped and discarded, so a
     * socket chrome created non-blocking still blocked its IO thread on the
     * first empty recv. */
    s->nonblock = (type & SOCK_NONBLOCK) != 0;
    int fd = lx_sock_install(s);
    if (fd < 0) { sock_close(s); return -ABI_EMFILE; }
    return fd;
}

/* socketpair(2): AF_UNIX only. Chromium's Mojo IPC builds its message pipes on
 * AF_UNIX socketpairs (SOCK_SEQPACKET on Linux). This is in-process IPC, not
 * networking -- NO CAP_NET. Creates a connected pair of in-memory message
 * endpoints (see sock_unix_* in socket.c) and writes the two fds to sv[2].
 * SOCK_STREAM/DGRAM/SEQPACKET are all treated as message channels; the
 * SOCK_CLOEXEC/NONBLOCK type bits are ignored. No SCM_RIGHTS (fd passing) yet. */
static long lx_socketpair(int domain, int type, int proto, uint64_t usv) {
    (void)proto;
    if (domain != AF_UNIX) return -LXE_EAFNOSUPPORT;
    int t = type & 0xff;
    if (t != SOCK_STREAM && t != SOCK_DGRAM && t != SOCK_SEQPACKET)
        return -ABI_EINVAL;

    struct sock *a = 0, *b = 0;
    if (sock_unix_pair(&a, &b) != 0) return -ABI_EMFILE;   /* pool exhausted */
    a->sotype = b->sotype = (uint8_t)t;         /* slice 80: SEQPACKET stays SEQPACKET */

    int fd0 = lx_sock_install(a);
    if (fd0 < 0) { sock_close(a); sock_close(b); return -ABI_EMFILE; }
    int fd1 = lx_sock_install(b);
    if (fd1 < 0) { sock_close(b); return -ABI_EMFILE; }    /* fd0/a leak on rare OOM */

    int fds[2] = { fd0, fd1 };
    if (copy_to_user((void *)usv, fds, sizeof fds) != 0) return -ABI_EFAULT;
    return 0;
}

/* Slice 86: pull the name out of a user sockaddr_un. Layout is
 * { u16 sun_family; char sun_path[108]; } == 110 bytes. An ABSTRACT socket
 * has sun_path[0] == '\0' and the name is the following bytes, which are
 * NOT NUL-terminated -- their length comes from addrlen, so the caller's
 * length is load-bearing rather than advisory. Shared by bind and connect
 * so the two cannot disagree about what a given address means. */
static long lx_sun_parse(uint64_t uaddr, uint32_t alen, char *name,
                         size_t namesz, bool *abstract) {
    uint8_t ua[112];
    uint32_t rd = alen > sizeof(ua) ? (uint32_t)sizeof(ua) : alen;
    if (!uaddr || rd < 2) return -ABI_EFAULT;
    memset(ua, 0, sizeof ua);
    if (copy_from_user(ua, (const void *)(uintptr_t)uaddr, rd) != 0)
        return -ABI_EFAULT;
    const char *sun = (const char *)ua + 2;
    uint32_t pathlen = rd - 2;
    bool abs = (pathlen > 0 && sun[0] == '\0');
    const char *src = abs ? sun + 1 : sun;
    uint32_t nlen = abs ? (pathlen - 1) : pathlen;
    /* A filesystem name is NUL-terminated inside the buffer; trust the NUL
     * over addrlen, since callers routinely pass sizeof(sockaddr_un). */
    if (!abs) { uint32_t k = 0; while (k < nlen && src[k]) k++; nlen = k; }
    if (nlen > namesz - 1) nlen = (uint32_t)namesz - 1;
    memcpy(name, src, nlen);
    name[nlen] = '\0';
    *abstract = abs;
    return 0;
}

/* Slice 86: write a sockaddr back to user space with Linux's truncation
 * rule -- copy at most *ualen bytes, then report the address's ACTUAL
 * length (which may exceed the buffer, telling the caller it was cut).
 *
 * Every one of these sites previously wrote sizeof(struct sockaddr_in)
 * unconditionally and ignored the caller's buffer size, so a caller that
 * passed anything smaller had its stack quietly overwritten past the end
 * of the object -- the classic way to trip -fstack-protector, which is
 * exactly the failure being chased (an INT3 from __stack_chk_fail).
 * getsockname alone runs ~126k times per interval under chrome. */
static void lx_addr_writeback(uint64_t uaddr, uint64_t ualen,
                              const void *src, uint32_t len) {
    if (!uaddr) return;
    uint32_t cap = 0;
    if (ualen) {
        if (copy_from_user(&cap, (const void *)(uintptr_t)ualen, 4) != 0) return;
    } else {
        cap = len;                         /* no length given: caller vouches */
    }
    uint32_t n = cap < len ? cap : len;
    if (n) (void)copy_to_user((void *)(uintptr_t)uaddr, src, n);
    if (ualen) (void)copy_to_user((void *)(uintptr_t)ualen, &len, 4);
}

static long lx_bind(int fd, uint64_t uaddr, uint32_t alen) {
    struct sock *s = lx_sock_of(fd);
    if (!s) return -LXE_ENOTSOCK;

    /* AF_UNIX: bind to a filesystem or abstract name. Before slice 86 this
     * fell through to the sockaddr_in path below and answered EINVAL for
     * every UNIX socket -- which is what full chrome's ProcessSingleton
     * hits when it binds SingletonSocket with sizeof(sockaddr_un) == 110. */
    if (s->kind == SOCK_KIND_UNIX) {
        char nm[109]; bool abs = false;
        long pr = lx_sun_parse(uaddr, alen, nm, sizeof nm, &abs);
        if (pr) return pr;
        if (!nm[0]) return -ABI_EINVAL;             /* autobind unsupported */
        int rc = sock_unix_bind_name(s, nm, abs);
        if (rc == -98) return -LXE_EADDRINUSE;
        return rc == 0 ? 0 : -ABI_EINVAL;
    }
    /* AF_NETLINK: bind(sockaddr_nl) selects the multicast groups to listen on.
     * We deliver no unsolicited notifications, so the groups are recorded and
     * otherwise unused; what matters is that bind SUCCEEDS, since chrome
     * PCHECKs it. nl_pid 0 means "kernel assigns one" -- use the pool index+1
     * so each socket gets a distinct, stable id, as Linux does per-socket. */
    if (s->kind == SOCK_KIND_NETLINK) {
        struct { uint16_t fam, pad; uint32_t pid, groups; } nl;
        memset(&nl, 0, sizeof nl);
        if (alen < sizeof nl || !uaddr ||
            copy_from_user(&nl, (const void *)(uintptr_t)uaddr, sizeof nl) != 0)
            return -ABI_EFAULT;
        if (nl.fam != AF_NETLINK) return -ABI_EINVAL;
        s->nl_pid = nl.pid ? nl.pid : (uint32_t)(current_proc()->pid);
        return 0;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    if (alen < 8 || !uaddr ||
        copy_from_user(&sa, (const void *)(uintptr_t)uaddr, sizeof sa) != 0)
        return -ABI_EFAULT;
    if (sa.sin_family != AF_INET) return -ABI_EINVAL;
    return sock_bind(s, sa.sin_port) == 0 ? 0 : -LXE_EADDRINUSE;
}

/* ---- Non-blocking connect bookkeeping --------------------------------
 *
 * A non-blocking connect() returns EINPROGRESS with the SYN on the wire; the
 * caller then waits for the socket to become writable and reads SO_ERROR to
 * learn the outcome. This resolves that state machine. Returns:
 *    0  still connecting
 *    1  connected (s->connecting cleared)
 *   <0  negative errno; also latched into s->so_error for getsockopt
 *
 * The deadline matters: a handshake that draws no answer at all leaves the
 * connection in SYN_SENT indefinitely, and without a timeout poll() would stay
 * silent forever and the caller would hang with no error to report. */
static int lx_conn_progress(struct sock *s) {
    if (!s || !s->connecting) return 1;
    if (!s->tcp) { s->connecting = 0; s->so_error = LXE_ECONNREFUSED;
                   return -LXE_ECONNREFUSED; }

    tcp_state_t st = tcp_state(s->tcp);
    if (st == TCP_ESTABLISHED) {
        s->connecting = 0;
        s->so_error   = 0;
        return 1;
    }
    int flags = tcp_poll_flags(s->tcp);
    if ((flags & TCP_RDY_ERR) || st == TCP_CLOSED) {
        s->connecting = 0;
        s->so_error   = LXE_ECONNREFUSED;
        return -LXE_ECONNREFUSED;
    }
    if (s->conn_deadline && pit_ticks() >= s->conn_deadline) {
        s->connecting = 0;
        s->so_error   = LXE_ETIMEDOUT;
        return -LXE_ETIMEDOUT;
    }
    return 0;                                   /* still in the handshake */
}

static long lx_listen(int fd, int backlog) {
    struct sock *s = lx_sock_of(fd);
    if (!s) return -LXE_ENOTSOCK;
    if (s->kind == SOCK_KIND_UNIX)            /* slice 86 */
        return sock_unix_listen(s) == 0 ? 0 : -ABI_EINVAL;
    if (s->kind != SOCK_KIND_TCP) return -LXE_EOPNOTSUPP;
    if (s->local_port == 0)       return -ABI_EINVAL;   /* must bind first */
    struct tcp_conn *lsn = tcp_listen(s->local_port, backlog);
    if (!lsn) return -LXE_EADDRINUSE;
    s->tcp = lsn;
    s->tcp_listening = true;
    return 0;
}

static long lx_accept(int fd, uint64_t uaddr, uint64_t ualen, int flags) {
    struct sock *s = lx_sock_of(fd);
    if (!s) return -LXE_ENOTSOCK;

    /* AF_UNIX listener (slice 86): connect() already built the connected
     * pair, so accept just hands over the server end. No handshake means
     * nothing to block on -- an empty backlog is EAGAIN. */
    if (s->kind == SOCK_KIND_UNIX) {
        struct sock *ns = 0;
        int rc = sock_unix_accept(s, &ns);
        if (rc == -11) return -LXE_EAGAIN;
        if (rc != 0 || !ns) return -ABI_EINVAL;
        ns->nonblock = (flags & SOCK_NONBLOCK) != 0;
        int nfd = lx_sock_install(ns);
        if (nfd < 0) { sock_close(ns); return -ABI_EMFILE; }
        struct { uint16_t fam; char path[108]; } sun;
        memset(&sun, 0, sizeof sun);
        sun.fam = AF_UNIX;                    /* an accepted end is unnamed */
        lx_addr_writeback(uaddr, ualen, &sun, 2);
        return nfd;
    }

    if (s->kind != SOCK_KIND_TCP || !s->tcp_listening || !s->tcp)
        return -ABI_EINVAL;
    /* Non-blocking listener: report EAGAIN rather than sitting in tcp_accept
     * for the whole timeout when no handshake is queued. */
    if (s->nonblock && !tcp_can_accept(s->tcp)) return -LXE_EAGAIN;
    uint32_t to = s->recv_timeout_ms ? s->recv_timeout_ms : LX_SOCK_DEF_ACCEPT_MS;
    struct tcp_conn *child = tcp_accept(s->tcp, to);
    if (!child) return -LXE_EAGAIN;
    struct sock *ns = sock_alloc(SOCK_KIND_TCP);
    if (!ns) { tcp_close(child); return -ABI_EMFILE; }
    ns->tcp = child;
    ns->local_port = s->local_port;
    ns->nonblock   = (flags & SOCK_NONBLOCK) != 0;
    int nfd = lx_sock_install(ns);
    if (nfd < 0) { tcp_close(child); sock_close(ns); return -ABI_EMFILE; }
    {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;        /* peer ip/port not exposed; family only */
        lx_addr_writeback(uaddr, ualen, &sa, (uint32_t)sizeof sa);
    }
    return nfd;
}

static long lx_connect(int fd, uint64_t uaddr, uint32_t alen) {
    struct sock *s = lx_sock_of(fd);
    if (!s) return -LXE_ENOTSOCK;
    if (!uaddr || alen < 2) return -ABI_EFAULT;

    uint16_t fam = 0;
    if (copy_from_user(&fam, (const void *)(uintptr_t)uaddr, sizeof fam) != 0)
        return -ABI_EFAULT;

    /* AF_UNIX: connect to a named/abstract path. sockaddr_un = { u16 family;
     * char sun_path[108]; }. Abstract sockets have sun_path[0]=='\0' and the
     * name is the following (non-NUL-terminated) bytes; xcb tries the abstract
     * "@/tmp/.X11-unix/X0" first, then the filesystem path. Only the X socket is
     * served (as an in-kernel fake X server, see sock_unix_connect_named). */
    if (fam == AF_UNIX) {
        if (s->kind != SOCK_KIND_UNIX) return -ABI_EINVAL;
        uint8_t ua[112];
        uint32_t rd = alen > sizeof(ua) ? (uint32_t)sizeof(ua) : alen;
        memset(ua, 0, sizeof ua);
        if (copy_from_user(ua, (const void *)(uintptr_t)uaddr, rd) != 0)
            return -ABI_EFAULT;
        const char *sun = (const char *)ua + 2;
        uint32_t pathlen = rd > 2 ? rd - 2 : 0;
        bool abstract = (pathlen > 0 && sun[0] == '\0');
        char name[109];
        uint32_t nlen = abstract ? (pathlen - 1) : pathlen;
        const char *src = abstract ? sun + 1 : sun;
        if (nlen > sizeof(name) - 1) nlen = sizeof(name) - 1;
        memcpy(name, src, nlen);
        name[nlen] = '\0';                        /* strcmp stops at any embedded NUL */
        int rc = sock_unix_connect_named(s, name, abstract);
        return rc == 0 ? 0 : -LXE_ECONNREFUSED;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    if (alen < 8 ||
        copy_from_user(&sa, (const void *)(uintptr_t)uaddr, sizeof sa) != 0)
        return -ABI_EFAULT;
    if (sa.sin_family != AF_INET) return -ABI_EINVAL;

    if (s->kind == SOCK_KIND_UDP) {
        /* "Connected" UDP: just remember the peer; send()/recv() (and the
         * connected read()/write() byte forms) then use it as the default
         * destination. No handshake. */
        s->peer_ip   = sa.sin_addr;
        s->peer_port = sa.sin_port;
        return 0;
    }

    uint32_t to = s->send_timeout_ms ? s->send_timeout_ms : LX_SOCK_DEF_CONNECT_MS;

    /* Non-blocking connect: put the SYN on the wire and return EINPROGRESS at
     * once. Chrome's TCPSocketPosix does exactly this and then waits for the
     * fd to become writable -- blocking here instead parked its IO thread for
     * a whole round trip on every connection. A repeat connect() while one is
     * in flight is EALREADY, as on Linux. */
    if (s->nonblock) {
        if (s->connecting) {
            int pr = lx_conn_progress(s);
            if (pr == 1) return 0;                    /* finished in the gap */
            return pr < 0 ? pr : -LXE_EALREADY;
        }
        if (s->tcp) return -LXE_EISCONN;
        struct tcp_conn *nc = tcp_connect_nb(sa.sin_addr, sa.sin_port);
        if (!nc) return -LXE_ECONNREFUSED;
        s->tcp        = nc;
        s->connecting = 1;
        s->so_error   = 0;
        uint32_t hz = pit_hz(); if (!hz) hz = 100;
        s->conn_deadline = pit_ticks() + ((uint64_t)hz * to) / 1000u;
        return -LXE_EINPROGRESS;
    }

    struct tcp_conn *c = tcp_connect(sa.sin_addr, sa.sin_port, to);
    if (!c) return -LXE_ECONNREFUSED;
    s->tcp = c;
    return 0;
}

/* Pull a destination (ip/port, network order) for a UDP send: prefer an
 * explicit sockaddr_in at `uaddr` (sendto's 5th arg; the 6th -- addrlen -- is
 * not delivered, so we assume sizeof(sockaddr_in)), else the connect()-ed peer.
 * Returns false if neither is available. */
static bool lx_udp_dest(struct sock *s, uint64_t uaddr,
                        uint32_t *ip_be, uint16_t *port_be) {
    if (uaddr) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        if (copy_from_user(&sa, (const void *)(uintptr_t)uaddr, sizeof sa) != 0)
            return false;
        if (sa.sin_family != AF_INET) return false;
        *ip_be = sa.sin_addr; *port_be = sa.sin_port;
        return true;
    }
    if (s->peer_port) { *ip_be = s->peer_ip; *port_be = s->peer_port; return true; }
    return false;
}

/* send/recv on a CONNECTED TCP socket, or a UDP socket (datagram to an explicit
 * sendto address or the connect()-ed peer). */
static long lx_send(int fd, uint64_t ubuf, size_t len, uint64_t uaddr) {
    struct sock *s = lx_sock_of(fd);
    if (!s) return -LXE_ENOTSOCK;

    if (s->kind == SOCK_KIND_UDP) {
        uint32_t dip; uint16_t dport;
        if (!lx_udp_dest(s, uaddr, &dip, &dport)) return -ABI_EINVAL;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *k = len ? kmalloc(len) : 0;
        if (len && !k) return -ABI_ENOMEM;
        long rv;
        if (len && copy_from_user(k, (const void *)(uintptr_t)ubuf, len) != 0)
            rv = -ABI_EFAULT;
        else {
            long n = sock_sendto(s, k, len, dip, dport);
            rv = (n < 0) ? -LXE_EAGAIN : n;
        }
        if (k) kfree(k);
        return rv;
    }

    if (s->kind == SOCK_KIND_UNIX) {
        /* AF_UNIX send()/sendto(). This arm was MISSING while recvfrom() (below)
         * and sendmsg() both had one, and that asymmetry alone was the Mojo
         * stall: the browser sends its invitation with sendmsg (it needs
         * SCM_RIGHTS), but the child replies to it with a plain 80-byte
         * sendto -- which fell through to the TCP check and came back
         * ENOTSOCK. So the child DID answer every invitation; we refused the
         * call, and both sides then waited on each other until the child hit
         * its 15s "no connection" deadline. sendto carries no ancillary data,
         * so this is sock_unix_send with no fds; a destination address is
         * meaningless on a connected pair and is ignored, as on Linux. */
        (void)uaddr;
        if (len == 0) return 0;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *uk = kmalloc(len);
        if (!uk) return -ABI_ENOMEM;
        long urv;
        if (copy_from_user(uk, (const void *)(uintptr_t)ubuf, len) != 0)
            urv = -ABI_EFAULT;
        else {
            long n = sock_unix_send(s, uk, len);
            /* SOCK_ERR_AGAIN = ring full -> tell the sender to retry (EAGAIN),
             * NOT EPIPE. Dropping the message here was corrupting Mojo's stream. */
            urv = (n == SOCK_ERR_AGAIN) ? -LXE_EAGAIN
                : (n < 0)               ? -ABI_EPIPE : n;
        }
        kfree(uk);
        return urv;
    }

    /* AF_NETLINK: the request is answered synchronously into our own rx ring
     * (see sock_netlink_send), so the reply is already readable when this
     * returns -- which is what makes chrome's blocking-then-MSG_DONTWAIT read
     * loop terminate correctly. */
    if (s->kind == SOCK_KIND_NETLINK) {
        (void)uaddr;
        if (len == 0) return 0;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *nk = kmalloc(len);
        if (!nk) return -ABI_ENOMEM;
        long nrv;
        if (copy_from_user(nk, (const void *)(uintptr_t)ubuf, len) != 0)
            nrv = -ABI_EFAULT;
        else
            nrv = sock_netlink_send(s, nk, len);
        kfree(nk);
        return nrv < 0 ? -ABI_EINVAL : nrv;
    }

    if (s->kind != SOCK_KIND_TCP || !s->tcp) return -LXE_ENOTSOCK;
    (void)uaddr;
    if (len == 0) return 0;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;
    /* A send on a socket whose non-blocking connect has not finished is
     * EAGAIN, not a broken pipe. */
    if (s->connecting) {
        int pr = lx_conn_progress(s);
        if (pr == 0) return -LXE_EAGAIN;
        if (pr < 0)  return pr;
    }
    void *k = kmalloc(len);
    if (!k) return -ABI_ENOMEM;
    long rv;
    if (copy_from_user(k, (const void *)(uintptr_t)ubuf, len) != 0) rv = -ABI_EFAULT;
    else if (s->nonblock) {
        /* Never wait for ACKs: queue what the window allows and report the
         * short count (0 -> EAGAIN, and the caller waits for POLLOUT). */
        long n = tcp_send_nb(s->tcp, k, len);
        rv = (n < 0) ? -ABI_EPIPE : (n == 0 ? -LXE_EAGAIN : n);
    } else {
        long n = tcp_send(s->tcp, k, len);
        rv = (n < 0) ? -ABI_EPIPE : n;
    }
    kfree(k);
    return rv;
}

static long lx_recv(int fd, uint64_t ubuf, size_t len, uint64_t uaddr,
                    uint64_t ualen, int flags) {
    struct sock *s = lx_sock_of(fd);
    if (!s) return -LXE_ENOTSOCK;

    /* This call must not block if the socket is in non-blocking mode OR the
     * caller passed MSG_DONTWAIT for this one call. Both matter here: chrome's
     * AddressTrackerLinux keeps its netlink socket BLOCKING and drives the
     * dump-drain loop entirely with MSG_DONTWAIT. */
    bool nowait = s->nonblock || (flags & LX_MSG_DONTWAIT) != 0;

    /* A recv on a socket still completing a non-blocking connect. */
    if (s->connecting) {
        int pr = lx_conn_progress(s);
        if (pr == 0) return nowait ? -LXE_EAGAIN : 0;
        if (pr < 0)  return pr;
    }

    if (s->kind == SOCK_KIND_NETLINK) {
        if (len == 0) return 0;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *nk = kmalloc(len);
        if (!nk) return -ABI_ENOMEM;
        long n = sock_netlink_recv_peek(s, nk, len, (flags & LX_MSG_PEEK) != 0);
        long nrv;
        if (n == SOCK_ERR_AGAIN)  nrv = -LXE_EAGAIN;   /* dump fully drained */
        else if (n < 0)           nrv = -LXE_ENOTSOCK;
        else if (copy_to_user((void *)(uintptr_t)ubuf, nk, (size_t)n) != 0)
            nrv = -ABI_EFAULT;
        else                      nrv = n;
        kfree(nk);
        return nrv;
    }

    /* Non-blocking and nothing to read: EAGAIN rather than parking. Pump the
     * NIC once first, so a reply that has physically arrived but not yet been
     * demultiplexed is seen now instead of costing the caller another epoll
     * round trip. */
    if (nowait && !sock_recv_ready(s)) {
        struct net_dev *nd = net_default();
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        net_poll();
        if (!sock_recv_ready(s)) return -LXE_EAGAIN;
    }

    if (s->kind == SOCK_KIND_UDP) {
        if (len == 0) return 0;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *k = kmalloc(len);
        if (!k) return -ABI_ENOMEM;
        uint32_t sip = 0; uint16_t sport = 0;
        uint32_t to = s->recv_timeout_ms ? s->recv_timeout_ms : 0;
        long n = sock_recvfrom_to(s, k, len, &sip, &sport, to);
        long rv;
        if (n == EINTR_RET) rv = -LXE_EINTR;
        else if (n < 0)     rv = -LXE_EAGAIN;
        else if (n == 0 && to) rv = -LXE_EAGAIN;       /* timeout, no datagram */
        else if (copy_to_user((void *)(uintptr_t)ubuf, k, (size_t)n) != 0) rv = -ABI_EFAULT;
        else rv = n;
        kfree(k);
        if (rv >= 0 && uaddr) {        /* report the sender's ip/port */
            struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET; sa.sin_addr = sip; sa.sin_port = sport;
            lx_addr_writeback(uaddr, ualen, &sa, (uint32_t)sizeof sa);
        }
        return rv;
    }

    if (s->kind == SOCK_KIND_UNIX) {
        /* AF_UNIX stream recv()/recvfrom(): libxcb reads the X setup this way on
         * some builds. Block like read() (xcb polls readable first). */
        if (len == 0) return 0;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        void *k = kmalloc(len);
        if (!k) return -ABI_ENOMEM;
        long n = sock_unix_recv(s, k, len, 0);
        long rv;
        if (n == EINTR_RET) rv = -LXE_EINTR;
        else if (n < 0)     rv = -LXE_EAGAIN;
        else if (copy_to_user((void *)(uintptr_t)ubuf, k, (size_t)n) != 0) rv = -ABI_EFAULT;
        else rv = n;
        kfree(k);
        return rv;
    }

    if (s->kind != SOCK_KIND_TCP || !s->tcp) return -LXE_ENOTSOCK;
    if (len == 0) return 0;
    if (len > SYS_MAX_RW) len = SYS_MAX_RW;

    /* MSG_PEEK must NOT consume. Chrome's IsConnectedAndIdle() peeks one
     * byte off sockets it hands to a new H2 session; consuming it shifted
     * the TLS record stream by one byte and killed every HTTPS load with
     * alert 70 / ERR_SSL_PROTOCOL_ERROR (see tcp_peek). */
    if (flags & LX_MSG_PEEK) {
        void *pk = kmalloc(len);
        if (!pk) return -ABI_ENOMEM;
        long pn = tcp_peek(s->tcp, pk, len);
        long prv;
        if (pn < 0)       prv = 0;                   /* peer closed -> EOF */
        else if (pn == 0) prv = -LXE_EAGAIN;         /* nothing buffered yet */
        else if (copy_to_user((void *)(uintptr_t)ubuf, pk, (size_t)pn) != 0)
            prv = -ABI_EFAULT;
        else prv = pn;
#ifdef CHROMIUM_BOOT
        {   static int pk_logs;
            if (pk_logs < 16) { pk_logs++;
                struct proc *me = current_proc();
                kprintf("[rdchk] pid=%d fd=%d PEEK cap=%lu -> %ld\n",
                        me ? me->pid : -1, fd, (unsigned long)len, prv);
            } }
#endif
        kfree(pk);
        return prv;
    }

    void *k = kmalloc(len);
    if (!k) return -ABI_ENOMEM;
    uint32_t to = s->recv_timeout_ms ? s->recv_timeout_ms : 0;
    long n = tcp_recv(s->tcp, k, len, to);
    long rv;
    if (n == -1) rv = 0;                 /* peer closed -> EOF */
    else if (n < 0) rv = -LXE_ECONNREFUSED;
    else if (copy_to_user((void *)(uintptr_t)ubuf, k, (size_t)n) != 0) rv = -ABI_EFAULT;
    else rv = n;
#ifdef CHROMIUM_BOOT
    rdchk_verify(s->tcp, fd, k, ubuf, rv, len);
#endif
    kfree(k);
    if (rv >= 0 && uaddr) {              /* connected: report family only */
        struct sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET;
        lx_addr_writeback(uaddr, ualen, &sa, (uint32_t)sizeof sa);
    }
    return rv;
}

static long lx_setsockopt(int fd, int level, int optname, uint64_t uval, uint32_t olen) {
    struct sock *s = lx_sock_of(fd);
    if (!s) return -LXE_ENOTSOCK;
    if (level != SOL_SOCKET) return 0;   /* accept-and-ignore other levels */
    /* Slice 77: SO_PASSCRED -- the receiver wants SCM_CREDENTIALS attached to
     * every message. Record it; lx_scm_recv_emit does the work. chrome's
     * crashpad client sets this on its handshake socket, and without it the
     * handler's reply carries no pid for prctl(PR_SET_PTRACER). */
    if (optname == SO_PASSCRED) {
        uint32_t on = 0;
        if (uval && olen >= 4) (void)copy_from_user(&on, (const void *)(uintptr_t)uval, 4);
        s->passcred = (on != 0);
        return 0;
    }
    if ((optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) && uval && olen >= 16) {
        int64_t tv[2];                   /* struct timeval { sec; usec; } */
        if (copy_from_user(tv, (const void *)(uintptr_t)uval, sizeof tv) != 0)
            return -ABI_EFAULT;
        uint32_t ms = (uint32_t)(tv[0] * 1000 + tv[1] / 1000);
        if (optname == SO_RCVTIMEO) s->recv_timeout_ms = ms;
        else                        s->send_timeout_ms = ms;
    }
    return 0;                            /* SO_REUSEADDR etc.: no-op success */
}

/* getsockname(2) / getpeername(2).
 *
 * The local ADDRESS used to be left at 0.0.0.0 -- only the family and port were
 * filled. That is not a cosmetic gap: glibc's getaddrinfo implements RFC 3484
 * destination sorting by connect()ing a UDP socket to each candidate and
 * calling getsockname() to discover which source address the route picks. A
 * reply of 0.0.0.0 reads as "no route to this destination", which is why every
 * resolution retried on a 3-second cadence. We have exactly one interface, so
 * the answer is always g_my_ip.
 *
 * getpeername used to be aliased to getsockname, which answered with the LOCAL
 * address -- wrong for every caller that asks who it is talking to. */
static long lx_sockaddr_out(struct sock *s, uint64_t uaddr, uint64_t ualen,
                            bool peer) {
    if (!s || !uaddr) return -LXE_ENOTSOCK;

    /* AF_NETLINK: answer with a sockaddr_nl carrying our assigned nl_pid. */
    if (s->kind == SOCK_KIND_NETLINK) {
        struct { uint16_t fam, pad; uint32_t pid, groups; } nl;
        memset(&nl, 0, sizeof nl);
        nl.fam = AF_NETLINK;
        nl.pid = peer ? 0 : s->nl_pid;        /* peer of a netlink socket = kernel (0) */
        lx_addr_writeback(uaddr, ualen, &nl, (uint32_t)sizeof nl);
        return 0;
    }

    /* AF_UNIX: answer with a sockaddr_un, not the sockaddr_in this used to
     * invent. Reporting family AF_INET for a UNIX socket is a lie any
     * caller that switches on sa_family acts on. Unnamed endpoints (both
     * ends of a socketpair, and an accepted server end) report just the
     * 2-byte family, which is what Linux does. */
    if (s->kind == SOCK_KIND_UNIX) {
        struct { uint16_t fam; char path[108]; } sun;
        memset(&sun, 0, sizeof sun);
        sun.fam = AF_UNIX;
        uint32_t len = 2;
        const char *nm = peer ? 0 : sock_unix_bound_name(s);
        if (nm && *nm) {
            size_t n = strlen(nm);
            if (n > sizeof(sun.path) - 1) n = sizeof(sun.path) - 1;
            memcpy(sun.path, nm, n);
            len = (uint32_t)(2 + n + 1);      /* family + path + NUL */
        }
        lx_addr_writeback(uaddr, ualen, &sun, len);
        return 0;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;

    if (peer) {
        if (s->kind == SOCK_KIND_TCP) {
            if (!s->tcp) return -LXE_ENOTCONN;
            sa.sin_addr = tcp_remote_ip_be(s->tcp);
            sa.sin_port = tcp_remote_port_be(s->tcp);
        } else {
            if (!s->peer_port) return -LXE_ENOTCONN;
            sa.sin_addr = s->peer_ip;
            sa.sin_port = s->peer_port;
        }
    } else {
        sa.sin_addr = g_my_ip;                /* our only interface */
        /* A TCP socket's real local port lives on the connection (the
         * ephemeral one picked at connect time), not in sock->local_port. */
        sa.sin_port = (s->kind == SOCK_KIND_TCP && s->tcp && !s->tcp_listening)
                        ? tcp_local_port_be(s->tcp)
                        : s->local_port;
    }

    lx_addr_writeback(uaddr, ualen, &sa, (uint32_t)sizeof sa);
    return 0;
}

static long lx_getsockname(int fd, uint64_t uaddr, uint64_t ualen) {
    return lx_sockaddr_out(lx_sock_of(fd), uaddr, ualen, false);
}

static long lx_getpeername(int fd, uint64_t uaddr, uint64_t ualen) {
    return lx_sockaddr_out(lx_sock_of(fd), uaddr, ualen, true);
}

/* B18: a REAL getsockopt(SOL_SOCKET, …). Before this it was an accept-and-ignore
 * stub that returned 0 and left *optval untouched -- so SO_ERROR/SO_TYPE readers
 * (the common case: check SO_ERROR after connect(), or SO_TYPE to learn the
 * socket kind) read uninitialised junk. Fills the int/timeval result and writes
 * back *optlen, honouring the caller's buffer size. */
static long lx_getsockopt(int fd, int level, int optname,
                          uint64_t uval, uint64_t ulen_ptr) {
    struct sock *s = lx_sock_of(fd);
    if (!s) return -LXE_ENOTSOCK;
    if (level != SOL_SOCKET || !uval || !ulen_ptr) return 0;

    uint32_t ulen = 0;
    if (copy_from_user(&ulen, (const void *)(uintptr_t)ulen_ptr, 4) != 0)
        return -ABI_EFAULT;

    /* SO_RCVTIMEO/SO_SNDTIMEO answer with a struct timeval {sec, usec}. */
    if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
        uint32_t ms = (optname == SO_RCVTIMEO) ? s->recv_timeout_ms
                                               : s->send_timeout_ms;
        int64_t tv[2] = { (int64_t)(ms / 1000), (int64_t)(ms % 1000) * 1000 };
        uint32_t n = ulen < sizeof tv ? ulen : (uint32_t)sizeof tv;
        if (n && copy_to_user((void *)(uintptr_t)uval, tv, n) != 0)
            return -ABI_EFAULT;
        (void)copy_to_user((void *)(uintptr_t)ulen_ptr, &n, 4);
        return 0;
    }

    /* Everything else is a 4-byte int. */
    int32_t v;
    switch (optname) {
    case SO_ERROR:
        /* This is how a poller learns whether a non-blocking connect actually
         * succeeded: it waits for writability, then reads SO_ERROR (0 = up).
         * Resolve the handshake first, then report and CLEAR the latched error,
         * as Linux does -- SO_ERROR is read-once. */
        (void)lx_conn_progress(s);
        v = s->so_error;
        s->so_error = 0;
        break;
    case SO_TYPE:       v = s->sotype ? s->sotype                     /* slice 80 */
                          : (s->kind == SOCK_KIND_TCP) ? SOCK_STREAM
                                                       : SOCK_DGRAM; break;
    case SO_ACCEPTCONN: v = s->tcp_listening ? 1 : 0;                break;
    case SO_SNDBUF:
    case SO_RCVBUF:     v = 65536;                                   break;
    case SO_PEERCRED: {
        /* Slice 81: return the PEER endpoint's latched creator credentials
         * as a 12-byte struct ucred. Everything unknown used to fall through
         * to the 4-byte zero readback below, so a caller asking for peer
         * credentials got 4 bytes of zeros AND a length of 4 -- structurally
         * wrong, not merely empty. For a socketpair Linux reports the
         * creating process for both ends, which is what the latched values
         * give us. */
        struct { int32_t pid; uint32_t uid, gid; } uc = { 0, 0, 0 };
        struct sock *peer = 0;
        if (s->kind == SOCK_KIND_UNIX && s->peer_ip)
            peer = sock_peer_of(s);
        const struct sock *src = peer ? peer : s;
        uc.pid = src->cr_pid; uc.uid = src->cr_uid; uc.gid = src->cr_gid;
        uint32_t want = ulen < sizeof uc ? ulen : (uint32_t)sizeof uc;
        if (want && copy_to_user((void *)(uintptr_t)uval, &uc, want) != 0)
            return -ABI_EFAULT;
        (void)copy_to_user((void *)(uintptr_t)ulen_ptr, &want, 4);
#ifdef CHROMIUM_BOOT
        { static int pc = 0; if (pc < 12) { pc++;
            kprintf("[peercred] fd=%d -> pid=%d uid=%u gid=%u len=%u\n",
                    fd, uc.pid, uc.uid, uc.gid, want); } }
#endif
        return 0;
    }
    default:            v = 0;  /* REUSEADDR/KEEPALIVE/BROADCAST/… readback */ break;
    }
    uint32_t n = ulen < 4 ? ulen : 4;
    if (n && copy_to_user((void *)(uintptr_t)uval, &v, n) != 0)
        return -ABI_EFAULT;
    (void)copy_to_user((void *)(uintptr_t)ulen_ptr, &n, 4);
    return 0;
}

/* B18: sendmsg/recvmsg scatter-gather. struct msghdr (x86-64 Linux):
 *   0:msg_name 8:msg_namelen(+pad) 16:msg_iov 24:msg_iovlen
 *   32:msg_control 40:msg_controllen 48:msg_flags(+pad)   (56 bytes)
 * struct iovec { void *iov_base; size_t iov_len; }        (16 bytes)
 * We implement the data path (gather/scatter + msg_name for UDP); ancillary
 * data (msg_control / SCM_RIGHTS) is not supported and is reported cleared. */
#define LX_MSG_IOV_MAX 16

struct lx_msghdr {
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t _pad0;
    uint64_t msg_iov;
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    int32_t  msg_flags;
    uint32_t _pad1;
};

/* Read a msghdr + its (bounded) iovec array from user. Returns the iovec count,
 * fills *total with the summed length (capped at SYS_MAX_RW), or <0 on fault.
 * (struct lx_iovec {iov_base, iov_len} is defined above for readv/writev.) */
static long lx_read_msghdr(uint64_t umsg, struct lx_msghdr *mh,
                           struct lx_iovec *iov, size_t *total) {
    if (!umsg || copy_from_user(mh, (const void *)(uintptr_t)umsg, sizeof *mh) != 0)
        return -ABI_EFAULT;
    uint64_t n = mh->msg_iovlen;
    if (n > LX_MSG_IOV_MAX) n = LX_MSG_IOV_MAX;
    if (n && (!mh->msg_iov ||
              copy_from_user(iov, (const void *)(uintptr_t)mh->msg_iov,
                             (size_t)n * sizeof *iov) != 0))
        return -ABI_EFAULT;
    size_t t = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (iov[i].iov_len > SYS_MAX_RW - t) { t = SYS_MAX_RW; break; }
        t += (size_t)iov[i].iov_len;
    }
    *total = t;
    return (long)n;
}

/* ---- SCM_RIGHTS fd passing over AF_UNIX (slice 20) --------------------------
 * struct cmsghdr (x86-64 Linux): 0:cmsg_len(size_t) 8:cmsg_level 12:cmsg_type,
 * payload follows, each cmsg padded to an 8-byte boundary.
 * Chrome's Mojo shared-memory channel (mojo/core/channel_linux.cc) passes its
 * memfd this way; without it that channel FATALs. In --single-process the peer
 * thread SHARES our fd table, but Mojo still expects a distinct received fd, so
 * we file_clone() the description on send (it rides with the message) and
 * fd_alloc_into() it on receive -- exactly Linux's "dup into the receiver". */
struct lx_cmsghdr {
    uint64_t cmsg_len;
    int32_t  cmsg_level;
    int32_t  cmsg_type;
};
#define LX_SOL_SOCKET    1
#define LX_SCM_RIGHTS    1
#define LX_MSG_CTRUNC    0x8
#define LX_CMSG_ALIGN(n) (((uint64_t)(n) + 7ull) & ~7ull)
#define LX_CMSG_CTLMAX   1024u      /* bound the control buffer we copy in */

/* Collect SCM_RIGHTS fds from a sendmsg control buffer, cloning each open
 * description. Returns how many were placed in files[]. */
static int lx_scm_send_collect(const struct lx_msghdr *mh,
                               struct file **files, int max) {
    int nf = 0;
    if (!mh->msg_control || mh->msg_controllen < sizeof(struct lx_cmsghdr))
        return 0;
    uint64_t clen = mh->msg_controllen;
    if (clen > LX_CMSG_CTLMAX) clen = LX_CMSG_CTLMAX;
    uint8_t *cbuf = (uint8_t *)kmalloc((size_t)clen);
    if (!cbuf) return 0;
    if (copy_from_user(cbuf, (const void *)(uintptr_t)mh->msg_control,
                       (size_t)clen) != 0) { kfree(cbuf); return 0; }
    uint64_t off = 0;
    while (off + sizeof(struct lx_cmsghdr) <= clen) {
        struct lx_cmsghdr *cm = (struct lx_cmsghdr *)(cbuf + off);
        if (cm->cmsg_len < sizeof(struct lx_cmsghdr)) break;
        if (off + cm->cmsg_len > clen) break;
        if (cm->cmsg_level == LX_SOL_SOCKET && cm->cmsg_type == LX_SCM_RIGHTS) {
            uint64_t dlen = cm->cmsg_len - sizeof(struct lx_cmsghdr);
            int cnt = (int)(dlen / sizeof(int32_t));
            int32_t *ufds = (int32_t *)(cbuf + off + sizeof(struct lx_cmsghdr));
            for (int i = 0; i < cnt && nf < max; i++) {
                struct file *src = fd_lookup(ufds[i]);
                if (!src) continue;                  /* bad fd: skip (Linux EBADF) */
                struct file *cl = file_clone(src);
                if (cl) files[nf++] = cl;
            }
        }
        off += LX_CMSG_ALIGN(cm->cmsg_len);
    }
    kfree(cbuf);
#ifdef CHROMIUM_BOOT
    if (nf) { static int c=0; if(c<24){c++; kprintf("[scm] sendmsg passing %d fd(s)\n", nf);} }
#endif
    return nf;
}

/* Install received descriptions into our fd table and emit one SCM_RIGHTS cmsg
 * into the caller's control buffer. Writes msg_controllen + msg_flags back. */
/* Slice 77: SCM_CREDENTIALS payload -- Linux `struct ucred` is {pid,uid,gid},
 * three 32-bit fields, so CMSG_LEN(12) == 28 (matching the cmsg_len=28 the
 * control's strace shows on crashpad's reply). */
struct lx_ucred { int32_t pid; uint32_t uid; uint32_t gid; };
#define LX_SCM_CREDENTIALS 2

static void lx_scm_recv_emit(uint64_t umsg, const struct lx_msghdr *mh,
                             struct file **files, int nf, bool pass_cred) {
    uint64_t ctl_len = 0;
    int32_t  flags   = 0;
#ifdef CHROMIUM_BOOT
    /* Log WHAT arrived, not just how many. Mojo's invitation hands the child a
     * channel; whether that is a socket -- and if so whether its peer still
     * exists -- decides what the child can do next. Logged HERE, before the
     * install/CTRUNC paths below may file_close() these pointers. */
    if (nf > 0) { static int c=0; if(c<24){c++;
        struct proc *me = current_proc();
        kprintf("[scm] recvmsg pid=%d nf=%d:", me ? me->pid : -1, nf);
        for (int i = 0; i < nf; i++) {
            if (!files[i]) { kprintf(" <null>"); continue; }
            kprintf(" kind=%d", (int)files[i]->kind);
            if (files[i]->kind == FILE_KIND_SOCKET && files[i]->sock)
                kprintf("(peer=%d)", files[i]->sock->peer_ip
                                     ? (int)files[i]->sock->peer_ip - 1 : -1);
            if (files[i]->kind == FILE_KIND_EVENTFD)
                kprintf("(efd_id=%d)", eventfd_id(files[i]));  /* trace Mojo notify efd across procs */
        }
        kprintf("\n"); } }
#endif
    if (nf > 0) {
        uint64_t need = sizeof(struct lx_cmsghdr) + LX_CMSG_ALIGN(nf * 4);
        if (!mh->msg_control || mh->msg_controllen < need) {
            for (int i = 0; i < nf; i++) file_close(files[i]);   /* no room */
            flags = LX_MSG_CTRUNC;
        } else {
            int32_t fdnums[SOCK_SCM_MAX_FDS];
            int got = 0;
            for (int i = 0; i < nf; i++) {
                int nfd = fd_alloc_into(current_proc(), files[i]);
                if (nfd < 0) { file_close(files[i]); continue; }
                fdnums[got++] = nfd;
            }
            if (got > 0) {
                uint64_t dlen = (uint64_t)got * 4;
                uint64_t clen = sizeof(struct lx_cmsghdr) + dlen;
                uint8_t buf[sizeof(struct lx_cmsghdr) + SOCK_SCM_MAX_FDS * 4];
                struct lx_cmsghdr *cm = (struct lx_cmsghdr *)buf;
                cm->cmsg_len = clen; cm->cmsg_level = LX_SOL_SOCKET;
                cm->cmsg_type = LX_SCM_RIGHTS;
                memcpy(buf + sizeof(*cm), fdnums, (size_t)dlen);
                if (copy_to_user((void *)(uintptr_t)mh->msg_control, buf,
                                 (size_t)clen) == 0)
                    ctl_len = clen;
            }
        }
    }
    /* Slice 77: SCM_CREDENTIALS. Linux delivers the SENDER's {pid,uid,gid}
     * to a receiver that enabled SO_PASSCRED. chrome's crashpad handshake
     * needs exactly this: the handler replies with 8 bytes + credentials and
     * the browser feeds the pid into prctl(PR_SET_PTRACER, pid) -- without
     * it the browser CHECKs and aborts (exit 191). Emitted only when no
     * SCM_RIGHTS was written (one cmsg is all any caller here asks for) and
     * only if the control buffer has room; the values come from the message
     * that sock_unix_recv_fds just dequeued, captured at ENQUEUE time so a
     * sender that has since exited still identifies itself correctly. */
    if (ctl_len == 0 && pass_cred && mh->msg_control) {
        uint64_t need = sizeof(struct lx_cmsghdr) + sizeof(struct lx_ucred);
        if (mh->msg_controllen >= need) {
            uint8_t buf[sizeof(struct lx_cmsghdr) + sizeof(struct lx_ucred)];
            struct lx_cmsghdr *cm = (struct lx_cmsghdr *)buf;
            struct lx_ucred   *uc = (struct lx_ucred *)(buf + sizeof(*cm));
            cm->cmsg_len   = need;              /* == 28, as Linux reports */
            cm->cmsg_level = LX_SOL_SOCKET;
            cm->cmsg_type  = LX_SCM_CREDENTIALS;
            extern int32_t  g_unix_last_cred_pid;
            extern uint32_t g_unix_last_cred_uid, g_unix_last_cred_gid;
            uc->pid = g_unix_last_cred_pid;
            uc->uid = g_unix_last_cred_uid;
            uc->gid = g_unix_last_cred_gid;
            if (copy_to_user((void *)(uintptr_t)mh->msg_control, buf,
                             sizeof buf) == 0)
                ctl_len = need;
#ifdef CHROMIUM_BOOT
            { static int cc = 0; if (cc < 16) { cc++;
                kprintf("[scmcred] emitted pid=%d uid=%u gid=%u ctl_len=%lu\n",
                        uc->pid, uc->uid, uc->gid, (unsigned long)ctl_len); } }
#endif
        } else {
            flags |= LX_MSG_CTRUNC;
        }
    }
#ifdef CHROMIUM_BOOT
    if (nf) { static int c=0; if(c<24){c++; kprintf("[scm] recvmsg emitted %d fd(s) ctl_len=%lu flags=0x%x\n", nf,(unsigned long)ctl_len,(unsigned)flags);} }
#endif
    (void)copy_to_user((void *)(uintptr_t)(umsg + 40), &ctl_len, 8);
    (void)copy_to_user((void *)(uintptr_t)(umsg + 48), &flags, 4);
}

static long lx_sendmsg(int fd, uint64_t umsg, int flags) {
    (void)flags;
    struct sock *s = lx_sock_of(fd);
    if (!s) return -LXE_ENOTSOCK;

    struct lx_msghdr mh;
    struct lx_iovec  iov[LX_MSG_IOV_MAX];
    size_t total = 0;
    long nio = lx_read_msghdr(umsg, &mh, iov, &total);
    if (nio < 0) return nio;

    void *k = total ? kmalloc(total) : 0;
    if (total && !k) return -ABI_ENOMEM;
    size_t off = 0;
    for (long i = 0; i < nio && off < total; i++) {
        size_t l = (size_t)iov[i].iov_len;
        if (l > total - off) l = total - off;
        if (l && copy_from_user((uint8_t *)k + off,
                                (const void *)(uintptr_t)iov[i].iov_base, l) != 0) {
            if (k) kfree(k);
            return -ABI_EFAULT;
        }
        off += l;
    }

    long rv;
    if (s->kind == SOCK_KIND_NETLINK) {
        /* Dump request via sendmsg (glibc's check_pf uses sendto, chrome's
         * AddressTrackerLinux uses sendto too, but netlink users legitimately
         * use either -- answer both). */
        long n = total ? sock_netlink_send(s, k, total) : 0;
        rv = (n < 0) ? -ABI_EINVAL : n;
    } else if (s->kind == SOCK_KIND_UDP) {
        uint32_t dip; uint16_t dport;
        uint64_t naddr = (mh.msg_namelen >= 8) ? mh.msg_name : 0;
        if (!lx_udp_dest(s, naddr, &dip, &dport)) { if (k) kfree(k); return -ABI_EINVAL; }
        long n = sock_sendto(s, k, total, dip, dport);
        rv = (n < 0) ? -LXE_EAGAIN : n;
    } else if (s->kind == SOCK_KIND_TCP && s->tcp) {
        long n = total ? (s->nonblock ? tcp_send_nb(s->tcp, k, total)
                                      : tcp_send(s->tcp, k, total)) : 0;
        rv = (n < 0) ? -ABI_EPIPE
           : (n == 0 && total && s->nonblock) ? -LXE_EAGAIN : n;
    } else if (s->kind == SOCK_KIND_UNIX) {
        /* AF_UNIX: libxcb + D-Bus write via sendmsg (not just writev), and Mojo
         * passes fds (its memfd) as SCM_RIGHTS ancillary data -- collect + clone
         * those so they ride with the message to the peer. */
        struct file *scm[SOCK_SCM_MAX_FDS];
        int nscm = lx_scm_send_collect(&mh, scm, SOCK_SCM_MAX_FDS);
        long n = sock_unix_send_fds(s, k, total, scm, nscm);
        /* Ring full -> EAGAIN (retry), never EPIPE. sock_unix_send_fds already
         * released the SCM_RIGHTS clones on backpressure, so the retry re-clones. */
        rv = (n == SOCK_ERR_AGAIN) ? -LXE_EAGAIN
           : (n < 0)               ? -ABI_EPIPE : n;
#ifdef CHROMIUM_BOOT
        /* Slice 75: the crashpad handshake probe. [chan] cannot see these --
         * it only records FILE_KIND_SOCKET fds, and socketpairs are not
         * that -- so AF_UNIX sendmsg/recvmsg were invisible exactly where
         * full chrome dies (slice 74: it sends, then never receives). Log
         * the fd, byte count, SCM_RIGHTS count and RESULT. */
        { static int um = 0; if (um < 48) { um++;
            struct proc *up = current_proc();
            kprintf("[uxmsg] pid=%d SEND fd=%d len=%lu nscm=%d -> %ld\n",
                    up ? up->pid : -1, fd, (unsigned long)total, nscm, rv);
        } }
#endif
    } else {
        rv = -LXE_ENOTSOCK;
    }
    if (k) kfree(k);
    return rv;
}

static long lx_recvmsg(int fd, uint64_t umsg, int flags) {
    (void)flags;
    struct sock *s = lx_sock_of(fd);
    if (!s) return -LXE_ENOTSOCK;

    struct file *scm_files[SOCK_SCM_MAX_FDS];   /* SCM_RIGHTS handed to us */
    int          scm_n = 0;
    struct lx_msghdr mh;
    struct lx_iovec  iov[LX_MSG_IOV_MAX];
    size_t total = 0;
    long nio = lx_read_msghdr(umsg, &mh, iov, &total);
    if (nio < 0) return nio;
    if (total == 0) return 0;

    void *k = kmalloc(total);
    if (!k) return -ABI_ENOMEM;

    long n;
    uint32_t sip = 0; uint16_t sport = 0;
    if (s->kind == SOCK_KIND_NETLINK) {
        /* glibc reads netlink with recvmsg, NOT recv: __check_pf/getifaddrs
         * (sysdeps/unix/sysv/linux/check_pf.c make_request) is the caller, and
         * getaddrinfo runs it for RFC 3484 sorting on every resolution. Missing
         * this arm returned ENOTSOCK and printed glibc's "Unexpected error 88 on
         * netlink descriptor N (address family 16)" -- and, worse, it BROKE
         * name resolution outright, because a netlink socket that opens and then
         * fails is worse than one that never opens: the EINVAL from socket() is
         * what used to send glibc down its fallback path. */
        n = sock_netlink_recv_peek(s, k, total, (flags & LX_MSG_PEEK) != 0);
        if (n == SOCK_ERR_AGAIN)   { kfree(k); return -LXE_EAGAIN; }
        if (n < 0)                 { kfree(k); return -LXE_ENOTSOCK; }
    } else if (s->kind == SOCK_KIND_UDP) {
        /* Slice 56 ROOT-CAUSE FIX for the ~50s system-wide silences: this arm
         * ignored O_NONBLOCK/MSG_DONTWAIT, so chrome's non-blocking QUIC/DNS
         * UDP reads (always recvmsg, read-until-EAGAIN) BLOCKED in
         * sock_recvfrom_to's infinite drain+hlt loop -- on the BSP, where the
         * blocked thread is never preempted in kernel mode and pid 0 (poll_tick
         * + net pump + the only futex-sweep driver) is is_idle and unstealable.
         * One empty speculative read therefore froze EVERY timer and poller in
         * the box until a stray datagram arrived 45-55s later ([cur] pid=39
         * RUNNING in recvmsg for 47s; [tick] fxsweep/polltick counters frozen;
         * [eplate] epoll to=4549ms act=49799ms). Mirror lx_recv's nowait path. */
        bool nowait = s->nonblock || (flags & LX_MSG_DONTWAIT) != 0;
        if (nowait && !sock_recv_ready(s)) {
            struct net_dev *nd = net_default();
            if (nd && nd->rx_drain) nd->rx_drain(nd);
            net_poll();
            if (!sock_recv_ready(s)) { kfree(k); return -LXE_EAGAIN; }
        }
        uint32_t to = s->recv_timeout_ms ? s->recv_timeout_ms : 0;
        n = sock_recvfrom_to(s, k, total, &sip, &sport, to);
        if (n == EINTR_RET)        { kfree(k); return -LXE_EINTR; }
        if (n < 0)                 { kfree(k); return -LXE_EAGAIN; }
        if (n == 0 && to)          { kfree(k); return -LXE_EAGAIN; }
    } else if (s->kind == SOCK_KIND_TCP && s->tcp) {
        if (flags & LX_MSG_PEEK) {               /* never consume (see lx_recv) */
            n = tcp_peek(s->tcp, k, total);
            if (n < 0)       n = 0;              /* peer closed -> EOF */
            else if (n == 0) { kfree(k); return -LXE_EAGAIN; }
        } else {
        uint32_t to = s->recv_timeout_ms ? s->recv_timeout_ms : 0;
        n = tcp_recv(s->tcp, k, total, to);
        if (n == -1)      n = 0;                 /* peer closed -> EOF */
        else if (n < 0)   { kfree(k); return -LXE_ECONNREFUSED; }
        }
    } else if (s->kind == SOCK_KIND_UNIX) {
        /* AF_UNIX stream: libxcb reads the X setup + events via recvmsg (this was
         * the wall -- ENOTSOCK here made xcb_connect() report XCB_CONN_ERROR even
         * though the writev'd request + our fake-X reply were fine). MSG_DONTWAIT
         * (0x40) -> non-blocking poll; otherwise block like read().
         *
         * Slice 88 -- THE HEADED OZONE WALL. This branch honoured ONLY
         * MSG_DONTWAIT and ignored s->nonblock (the fd's O_NONBLOCK), unlike
         * lx_recv and the UDP branch above. Chromium's X socket is O_NONBLOCK
         * and its UI-thread pump reads until EAGAIN with flags=0 -- so the
         * first read on an empty ring parked the UI thread in a FOREVER
         * blocking wait ([uxstuck] pid=3 sock=4 xsrv=1 count=0 to=0, "READY
         * in recvmsg" for 173s). Every downstream symptom -- no MapWindow, no
         * browser window, Target.createTarget never answering -- was this
         * one missing flag check. Mojo never tripped it because its channel
         * reads pass MSG_DONTWAIT explicitly. */
        bool nowait = s->nonblock || (flags & 0x40) != 0;
        uint32_t to = nowait ? 1u : 0u;
        n = sock_unix_recv_fds(s, k, total, to, scm_files, SOCK_SCM_MAX_FDS,
                               &scm_n);
        if (n == EINTR_RET)            { kfree(k); return -LXE_EINTR; }
        if (n < 0)                     { kfree(k); return -LXE_EAGAIN; }
        /* n == 0 means EITHER "nothing queued yet" OR "EOF: peer is gone". Only
         * the first may become EAGAIN. Reporting EAGAIN at EOF made recvmsg
         * disagree with file_poll_ready(), which flags a peer-less UNIX socket
         * POLLIN|POLLHUP -- so epoll_wait said "readable" forever while recvmsg
         * said "try again", and chrome's Mojo IO thread spun epoll_wait/recvmsg
         * endlessly instead of pumping messages (the renderer then never
         * completed a document load). Linux returns 0 at EOF even for
         * MSG_DONTWAIT; match the poll predicate exactly. */
        if (n == 0 && nowait) {   /* slice 88: O_NONBLOCK too, not just DONTWAIT */
            bool at_eof = (s->peer_ip == 0 && s->count == 0 && !s->x_server);
            if (!at_eof) { kfree(k); return -LXE_EAGAIN; }
        }
#ifdef CHROMIUM_BOOT
        /* Slice 75: the receive half of the crashpad-handshake probe. */
        { static int um = 0; if (um < 48) { um++;
            struct proc *up = current_proc();
            /* Slice 78: report WHY a 0 came back. A blocking recv must only
             * return 0 at real EOF (peer gone AND ring drained); returning
             * it while the peer is alive but simply has not sent yet reads
             * as EOF to the caller -- which is exactly what crashpad's
             * handler reports as "incorrect payload size 0". */
            kprintf("[uxmsg] pid=%d RECV fd=%d want=%lu flags=0x%x -> %ld "
                    "nscm=%d peer=%d count=%d\n", up ? up->pid : -1, fd,
                    (unsigned long)total, (unsigned)flags, n, scm_n,
                    s->peer_ip ? (int)s->peer_ip - 1 : -1, (int)s->count);
        } }
#endif
    } else {
        kfree(k);
        return -LXE_ENOTSOCK;
    }

    /* Scatter the received bytes across the iovecs. */
    size_t off = 0;
    for (long i = 0; i < nio && off < (size_t)n; i++) {
        size_t l = (size_t)iov[i].iov_len;
        if (l > (size_t)n - off) l = (size_t)n - off;
        if (l && copy_to_user((void *)(uintptr_t)iov[i].iov_base,
                              (const uint8_t *)k + off, l) != 0) {
            kfree(k);
            for (int j = 0; j < scm_n; j++) file_close(scm_files[j]);
            return -ABI_EFAULT;
        }
        off += l;
    }
#ifdef CHROMIUM_BOOT
    /* Verify the first iovec only: chrome's TCP reads are single-iovec. */
    if (s->kind == SOCK_KIND_TCP && s->tcp && n > 0 && nio > 0) {
        size_t l0 = (size_t)iov[0].iov_len;
        if (l0 > (size_t)n) l0 = (size_t)n;
        rdchk_verify(s->tcp, fd, k, (uint64_t)iov[0].iov_base, (long)l0, total);
    }
#endif
    kfree(k);

    /* Fill the sender address (UDP) / family (TCP) and clear ancillary. */
    size_t namelen_out = 0;
    if (s->kind == SOCK_KIND_NETLINK) {
        /* The source of a dump reply is the KERNEL, which netlink identifies as
         * nl_pid == 0. glibc's make_request explicitly skips any message whose
         * nl_pid is non-zero ("not from the kernel"), so getting this wrong
         * makes it silently discard every answer we send. */
        struct { uint16_t fam, pad; uint32_t pid, groups; } nla;
        memset(&nla, 0, sizeof nla);
        nla.fam = AF_NETLINK;
        namelen_out = sizeof nla;
        if (mh.msg_name && mh.msg_namelen >= sizeof nla)
            (void)copy_to_user((void *)(uintptr_t)mh.msg_name, &nla, sizeof nla);
    } else if (mh.msg_name && mh.msg_namelen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET; sa.sin_addr = sip; sa.sin_port = sport;
        (void)copy_to_user((void *)(uintptr_t)mh.msg_name, &sa, sizeof sa);
        namelen_out = sizeof sa;
    }
    {   /* msg_namelen written back; msg_controllen/msg_flags are set by the
         * SCM_RIGHTS emitter (which also installs any received fds). */
        uint32_t nl = (mh.msg_name) ? (uint32_t)namelen_out : 0;
        (void)copy_to_user((void *)(uintptr_t)(umsg + 8),  &nl, 4);
        lx_scm_recv_emit(umsg, &mh, scm_files, scm_n,
                     s->kind == SOCK_KIND_UNIX && s->passcred);
    }
    return n;
}

/* access(2)/faccessat(2) family: resolve the path and report existence. We do
 * not enforce per-file R/W/X bits (tobyOS files in /bin are all executable and
 * the VFS doesn't model Unix mode bits here), so any existing path is
 * accessible -- exactly what PATH search / `test -e` / shell rc-probing need.
 * AT_FDCWD and absolute paths are honoured (resolve_user_path also handles
 * cwd-relative); a non-AT_FDCWD dirfd-relative path is resolved against the cwd
 * (best-effort -- bash/coreutils use AT_FDCWD). */
static long lx_faccess(const char *upath) {
    char kpath[ABI_PATH_MAX];
    if (resolve_user_path(upath, kpath, sizeof kpath) != 0)
        return -ABI_EFAULT;
    struct vfs_stat vs;
    return (vfs_stat(kpath, &vs) == VFS_OK) ? 0 : -ABI_ENOENT;
}

/* ------------------------------------------------------------------ *
 * Linux syscall gap-list instrument (the permanent measure-first tool)
 *
 * Every unhandled Linux syscall self-identifies here. The Chromium
 * bring-up (docs/chromium-bringup-*.md) drives the whole effort off
 * this log: run a real Linux binary, grep `-a` the serial log for
 * "[linux] UNHANDLED", and the deduped set IS the ranked gap list.
 *
 * lx_scname() names the common x86-64 numbers so the log reads without
 * a syscall table to hand; unknown numbers just print numerically.
 *
 * The default case dedups on the number (a 512-entry seen[] bitmap +
 * hit counter) so a program that spins on an unimplemented syscall
 * (Chromium loops on epoll/futex variants) prints ONE detailed line,
 * not a flood down the slow serial line -- while still counting hits
 * and re-announcing every 100000th so hot loops stay visible.
 *
 * -DLINUX_SYSCALL_TRACE additionally logs EVERY Linux syscall entry
 * (number+name+args+comm) -- the "log everything" firehose for deep
 * ABI debugging; OFF by default (kept in-tree, like -DDL_TRACE). ------ */
static const char *lx_scname(long n) {
    switch (n) {
    case 0: return "read"; case 1: return "write"; case 2: return "open";
    case 3: return "close"; case 4: return "stat"; case 5: return "fstat";
    case 6: return "lstat"; case 7: return "poll"; case 8: return "lseek";
    case 9: return "mmap"; case 10: return "mprotect"; case 11: return "munmap";
    case 12: return "brk"; case 13: return "rt_sigaction"; case 14: return "rt_sigprocmask";
    case 16: return "ioctl"; case 17: return "pread64"; case 18: return "pwrite64";
    case 19: return "readv"; case 20: return "writev"; case 21: return "access";
    case 22: return "pipe"; case 23: return "select"; case 24: return "sched_yield";
    case 25: return "mremap"; case 28: return "madvise"; case 32: return "dup";
    case 33: return "dup2"; case 34: return "pause"; case 35: return "nanosleep";
    case 39: return "getpid"; case 40: return "sendfile"; case 41: return "socket";
    case 42: return "connect"; case 43: return "accept"; case 44: return "sendto";
    case 45: return "recvfrom"; case 46: return "sendmsg"; case 47: return "recvmsg";
    case 48: return "shutdown"; case 49: return "bind"; case 50: return "listen";
    case 51: return "getsockname"; case 52: return "getpeername"; case 54: return "setsockopt";
    case 55: return "getsockopt"; case 56: return "clone"; case 57: return "fork";
    case 59: return "execve"; case 60: return "exit"; case 61: return "wait4";
    case 62: return "kill"; case 63: return "uname"; case 72: return "fcntl";
    case 73: return "flock"; case 74: return "fsync"; case 75: return "fdatasync";
    case 79: return "getcwd"; case 82: return "rename"; case 83: return "mkdir";
    case 87: return "unlink"; case 89: return "readlink"; case 96: return "gettimeofday";
    case 97: return "getrlimit"; case 98: return "getrusage"; case 99: return "sysinfo";
    case 102: return "getuid"; case 104: return "getgid"; case 107: return "geteuid";
    case 110: return "getppid"; case 115: return "getgroups"; case 116: return "setgroups";
    case 131: return "sigaltstack"; case 137: return "statfs"; case 138: return "fstatfs";
    case 157: return "prctl"; case 158: return "arch_prctl"; case 160: return "setrlimit";
    case 186: return "gettid"; case 200: return "tkill"; case 201: return "time";
    case 202: return "futex"; case 203: return "sched_setaffinity";
    case 204: return "sched_getaffinity"; case 213: return "epoll_create";
    case 217: return "getdents64"; case 218: return "set_tid_address";
    case 221: return "fadvise64"; case 228: return "clock_gettime";
    case 230: return "clock_nanosleep"; case 231: return "exit_group";
    case 232: return "epoll_wait"; case 233: return "epoll_ctl"; case 234: return "tgkill";
    case 257: return "openat"; case 258: return "mkdirat"; case 262: return "newfstatat";
    case 263: return "unlinkat"; case 267: return "readlinkat"; case 269: return "faccessat";
    case 270: return "pselect6"; case 271: return "ppoll"; case 273: return "set_robust_list";
    case 280: return "utimensat"; case 281: return "epoll_pwait"; case 282: return "signalfd";
    case 283: return "timerfd_create"; case 284: return "eventfd";
    case 285: return "fallocate"; case 286: return "timerfd_settime";
    case 288: return "accept4"; case 289: return "signalfd4"; case 290: return "eventfd2";
    case 291: return "epoll_create1"; case 292: return "dup3"; case 293: return "pipe2";
    case 294: return "inotify_init1"; case 302: return "prlimit64";
    case 305: return "syncfs"; case 309: return "getcpu"; case 316: return "renameat2";
    case 318: return "getrandom"; case 319: return "memfd_create"; case 321: return "bpf";
    case 322: return "execveat"; case 324: return "membarrier"; case 325: return "mlock2";
    case 332: return "statx"; case 334: return "rseq"; case 424: return "pidfd_send_signal";
    case 425: return "io_uring_setup"; case 435: return "clone3";
    case 439: return "faccessat2"; case 441: return "epoll_pwait2";
    case 450: return "set_mempolicy_home_node"; case 451: return "cachestat";
    default: return "?";
    }
}

/* Small ring of recent Linux syscalls (number + caller tid) for crash
 * diagnostics: isr.c dumps it on a fatal user fault, which is far cheaper than
 * the full -DLINUX_SYSCALL_TRACE firehose when a program has already made tens
 * of thousands of calls (e.g. Chromium at 20k+). */
#define LX_RECENT 384   /* slice 20: 48 was flooded by busy threads, hiding the
                         * sequence that precedes chrome's FATAL CHECK */
static long     g_lx_recent_n[LX_RECENT];
static int      g_lx_recent_tid[LX_RECENT];
static long     g_lx_recent_r[LX_RECENT];    /* slice 86: the RETURN value */
static long     g_lx_recent_a1[LX_RECENT];   /* slice 89: first arg (the fd
                                              * for read/recv/write/poll) --
                                              * names WHICH socket the final
                                              * pre-abort reads were on */
static uint32_t g_lx_recent_i;
#define LX_MAXCPU 16
static uint32_t g_lx_slot_cpu[LX_MAXCPU];    /* slot this cpu is filling */

void lx_dump_recent_syscalls(void) {
    kprintf("[lx-recent] last %d Linux syscalls (oldest first, tid in []):\n",
            LX_RECENT);
    for (uint32_t k = 0; k < LX_RECENT; k++) {
        uint32_t idx = (g_lx_recent_i + k) % LX_RECENT;
        long nn = g_lx_recent_n[idx];
        if (nn == 0 && g_lx_recent_tid[idx] == 0) continue;   /* unused slot */
        /* Slice 86: print the RETURN too. The browser's abort follows a
         * syscall whose result register reads 0x00000000fffff001 -- a -4095
         * that is zero-extended rather than sign-extended -- and the number
         * alone could never say which call produced it. */
        kprintf("  [%d] %ld(%s) a1=%ld = %ld (0x%lx)\n", g_lx_recent_tid[idx], nn,
                lx_scname(nn), g_lx_recent_a1[idx], g_lx_recent_r[idx],
                (unsigned long)g_lx_recent_r[idx]);
    }
}

#ifdef CHROMIUM_BOOT
/* slice 20 wait-graph: which threads are blocked in a BLOCKING syscall, on what,
 * and for how long. chrome's renderer never completes a document load and is
 * almost always in-kernel (per the ring-3 profiler), so the question is who is
 * waiting on whom. Recorded at the syscall boundary, so an entry persists for
 * exactly as long as the thread is blocked inside the call. */
#define WAITT_MAX 64
static struct { int pid; long nr; uint64_t arg; long to_ms; uint64_t since_ns;
                uint8_t busy; }
        g_waitt[WAITT_MAX];

/* to_ms: the REQUESTED timeout in ms (-1 = infinite/none). Slice 56: this is
 * the number the whole stall investigation was missing -- a thread blocked 40s
 * on `futex to=50` is a kernel wake-latency bug; blocked 40s on `to=-1` is
 * chrome idling by choice. */
static void waitt_enter(long nr, uint64_t arg, long to_ms) {
    struct proc *p = current_proc(); if (!p) return;
    for (int i = 0; i < WAITT_MAX; i++) if (!g_waitt[i].busy) {
        g_waitt[i].pid = p->pid; g_waitt[i].nr = nr; g_waitt[i].arg = arg;
        g_waitt[i].to_ms = to_ms;
        g_waitt[i].since_ns = perf_now_ns(); g_waitt[i].busy = 1; return; }
}
static void waitt_exit(void) {
    struct proc *p = current_proc(); if (!p) return;
    for (int i = 0; i < WAITT_MAX; i++)
        if (g_waitt[i].busy && g_waitt[i].pid == p->pid) { g_waitt[i].busy = 0; return; }
}
void waitt_dump(void) {
    uint64_t now = perf_now_ns(); int n = 0;
    for (int i = 0; i < WAITT_MAX; i++) if (g_waitt[i].busy) n++;
    if (!n) return;
    kprintf("[wait] %d thread(s) blocked in a syscall:\n", n);
    for (int i = 0; i < WAITT_MAX; i++) if (g_waitt[i].busy)
        kprintf("  pid=%d %s(0x%lx to=%ld) for %lu ms\n", g_waitt[i].pid,
                lx_scname(g_waitt[i].nr), (unsigned long)g_waitt[i].arg,
                g_waitt[i].to_ms,
                (unsigned long)((now - g_waitt[i].since_ns) / 1000000ull));
    /* [cur] slice 56: what every NON-blocked Linux thread is doing right now --
     * in-kernel inside which syscall (and for how long), or in userspace.
     * Resolves the "heartbeat says RUNNING / profiler says not in ring 3"
     * contradiction: a RUNNING thread sitting in one syscall for seconds is
     * in an in-kernel wait/spin loop, and this names the loop. */
    {
        extern struct proc g_proc[];
        for (int i = 0; i < PROC_MAX; i++) {
            struct proc *p = &g_proc[i];
            if (p->state == PROC_UNUSED || p->personality != 1) continue;
            if (p->state != PROC_RUNNING && p->state != PROC_READY) continue;
            if (p->cursys >= 0 && p->cursys_ns)
                kprintf("  [cur] pid=%d %s in %s for %lu ms\n", p->pid,
                        proc_state_name(p->state), lx_scname(p->cursys),
                        (unsigned long)((now - p->cursys_ns) / 1000000ull));
            else
                kprintf("  [cur] pid=%d %s in USER\n", p->pid,
                        proc_state_name(p->state));
        }
    }
    /* [epset] slice 56 run-4 follow-up: for any epoll_wait blocked >10s, dump
     * WHAT it is waiting on and whether any of it is ready RIGHT NOW. Splits
     * "sender never sent" (all r0 -- the wedge is upstream of this thread)
     * from "data pending but the waiter never woke" (kernel readiness/wake
     * bug). kN = file kind, rN = file_poll_ready bits at dump time. */
    {
        for (int i = 0; i < WAITT_MAX; i++) {
            if (!g_waitt[i].busy) continue;
            if (g_waitt[i].nr != LX_epoll_wait &&
                g_waitt[i].nr != LX_epoll_pwait) continue;
            if (now - g_waitt[i].since_ns < 10000000000ull) continue;
            struct proc *p = proc_lookup(g_waitt[i].pid);
            if (!p) continue;
            struct file **t = proc_fds(p);
            if (!t) continue;
            int epfd = (int)g_waitt[i].arg;
            if (epfd < 0 || epfd >= PROC_NFDS) continue;
            struct file *ef = t[epfd];
            if (!ef || ef->kind != FILE_KIND_EPOLL || !ef->epoll) continue;
            kprintf("  [epset] pid=%d epfd=%d:", g_waitt[i].pid, epfd);
            for (int e = 0; e < EPOLL_MAX; e++) {
                if (!ef->epoll->e[e].used) continue;
                int wfd = ef->epoll->e[e].fd;
                struct file *wf = (wfd >= 0 && wfd < PROC_NFDS) ? t[wfd] : 0;
                kprintf(" %d(k%d r%x)", wfd, wf ? (int)wf->kind : -1,
                        wf ? (unsigned)file_poll_ready(wf) : 0u);
            }
            kprintf("\n");
        }
    }
    /* [tick] slice 56: is the wake machinery itself alive? fxsweep must climb
     * ~100/s (10ms rate limit) whenever the BSP yield slow path runs; polltick
     * must climb ~1000/s while any poller is parked. nextdue = the SOONEST
     * timed-futex deadline still parked (negative = OVERDUE: the sweep is
     * starving and every "50ms" wait in chrome is stretching). */
    {
        extern uint64_t g_fx_sweeps, g_fx_sweep_woken, g_poll_tick_wakes,
                        g_fx_rt_flag_count;
        extern struct proc g_proc[];
        int timed = 0, minpid = -1; int64_t mind = 0; int have = 0;
        for (int i = 0; i < PROC_MAX; i++) {
            struct proc *p = &g_proc[i];
            if (p->state != PROC_BLOCKED || !p->futex_deadline_ns) continue;
            timed++;
            int64_t d = (int64_t)p->futex_deadline_ns - (int64_t)now;
            if (!have || d < mind) { mind = d; minpid = p->pid; have = 1; }
        }
        extern uint64_t g_poll_event_wakes;     /* slice 67 */
        kprintf("  [tick] fxsweep=%lu woke=%lu polltick=%lu pollevent=%lu "
                "rtflag=%lu timedfx=%d nextdue=%ldms pid=%d\n",
                (unsigned long)g_fx_sweeps, (unsigned long)g_fx_sweep_woken,
                (unsigned long)g_poll_tick_wakes,
                (unsigned long)g_poll_event_wakes,
                (unsigned long)g_fx_rt_flag_count, timed,
                have ? (long)(mind / 1000000ll) : 0, minpid);
    }
}

/* Read one u64 from proc `p`'s user address space (via its cr3), for reading a
 * BLOCKED thread's stack from an unrelated context. Returns false if unmapped. */
static bool read_u64_of(struct proc *p, uint64_t va, uint64_t *out) {
    extern uint64_t *get_pte(uint64_t cr3, uint64_t virt_addr);
    uint64_t *pte = get_pte(p->cr3, va & ~0xfffULL);
    if (!pte || !(*pte & PTE_PRESENT)) return false;
    uint64_t phys = (*pte & PTE_ADDR_MASK) | (va & 0xfffULL);
    *out = *(volatile uint64_t *)pmm_phys_to_virt(phys);
    return true;
}

/* CHROME-SIDE VISIBILITY: dump a deadlocked renderer thread's USER call chain.
 * chrome is -fomit-frame-pointer, so scan the stack for return-address-looking
 * qwords (in the 0x1000_0000_0000+ code region where ld.so mmaps the main
 * binary + every .so). Symbolize offline: find the [libmap] base each addr
 * falls under, then `objdump -d --start-address=<addr-base> <that .so>`. The
 * saved user rsp/rip live in the syscall_regs frame at kstack_top. */
/* Walk one proc's saved user stack, printing code-region return addresses.
 * `label` names the blocking syscall (or the kill). See region notes below. */
static void bt_dump_one(struct proc *p, const char *label, uint64_t arg) {
    if (!p || !p->kstack_top) return;
    struct syscall_regs *sr =
        (struct syscall_regs *)((uint8_t *)p->kstack_top - sizeof(*sr));
    uint64_t urip = sr->rcx, ursp = sr->user_rsp;
    kprintf("[bt] pid=%d %s(0x%lx) urip=0x%lx ursp=0x%lx\n",
            p->pid, label, (unsigned long)arg, urip, ursp);
    int printed = 0;
    for (int s = 0; s < 1024 && printed < 32; s++) {
        uint64_t v;
        if (!read_u64_of(p, ursp + (uint64_t)s * 8, &v)) break;
        /* Code regions: MAIN chrome PIE at 0x500000 (shows WHAT chrome awaits),
         * ld.so at 0x40000000, .so's at 0x1000_0000_0000+. 0x1024../0x102d.. are
         * thread stacks / shm (data), skipped. */
        bool code = (v >= 0x500000ULL       && v < 0x0d000000ULL)
                 || (v >= 0x40000000ULL     && v < 0x41000000ULL)
                 || (v >= 0x100000000000ULL && v < 0x100002000000ULL);
        if (code) {
            const char *reg = (v < 0x0d000000ULL) ? "MAIN"
                            : (v < 0x41000000ULL) ? "ldso" : "lib";
            kprintf("[bt]   +0x%03x 0x%lx %s\n", s * 8, v, reg);
            printed++;
        }
    }
}

/* Dump the user call chains of an ENTIRE thread group (tgid). Called from
 * signal_send the instant a renderer is SIGKILLed, so its deadlocked stack is
 * captured at the exact moment before death -- the only reliable way to catch
 * it (the browser kills it in a narrow window the timer heartbeat steps over,
 * and the guest clocks diverge under SMP). */
void bt_dump_group(int tgid) {
    static int done = 0;
    if (done) return;
    done = 1;
    extern struct proc g_proc[];
    kprintf("[bt] ===== SIGKILL victim group tgid=%d call chains =====\n", tgid);
    for (int i = 1; i < PROC_MAX; i++) {
        struct proc *p = &g_proc[i];
        if (p->state == PROC_UNUSED) continue;
        if (p->pid != tgid && p->tgid != tgid) continue;
        /* Find its blocking-syscall label from the waitt table, if any. */
        const char *lbl = "(running)"; uint64_t arg = 0;
        for (int w = 0; w < WAITT_MAX; w++)
            if (g_waitt[w].busy && g_waitt[w].pid == p->pid) {
                lbl = lx_scname(g_waitt[w].nr); arg = g_waitt[w].arg; break;
            }
        bt_dump_one(p, lbl, arg);
    }
    kprintf("[bt] ===== end =====\n");
}

/* Dump the CALLER's own user call chain from inside one of its syscalls --
 * used by the clock_gettime spin detector (slice 34) to identify a user-space
 * Now() busy-loop. Safe: own context, own CR3, saved regs at kstack_top are
 * exactly this syscall's. */
void bt_dump_self(const char *why) {
    struct proc *me = current_proc();
    if (!me) return;
    bt_dump_one(me, why, 0);
}
#endif

static long linux_syscall_impl(long n, long a1, long a2, long a3, long a4, long a5);

/* [chan] -- every socket-fd I/O call with its ARGS *and its RESULT*.
 *
 * The recent-syscall ring records names only, which cannot answer the two
 * questions that decide the Mojo stall: does the child ever WRITE to its
 * channel fd (and with what error), and does a read come back PARTIAL --
 * tobyOS's AF_UNIX is message-queued with a tail_off path, while Mojo's
 * channel framing expects SOCK_STREAM byte semantics. Logged AFTER the call so
 * the return value is real; blocking calls therefore appear at wake time. */
#ifdef CHROMIUM_BOOT
/* Slice 56d (wall R1): always-on RING of socket-channel ops -- any fixed
 * kprintf budget exhausts before the ~25s renderer death (measured: 500 by
 * 17s, 1500 by 21.5s, 18s-gated 1200 by 24s at ~200 msgs/s). The ring is
 * silent and cheap; [rexit] dumps the dying thread-group's tail, which is
 * the discriminator R1 needs: a channel recv returning 0 right before
 * exit_group = the browser CLOSED the channel; a normal final message =
 * instructed shutdown (decode it); no channel activity = internal choice. */
#define CHRING_MAX 512
static struct chring_ent { int tgid, pid; short nr, fd; long ret; uint32_t ms; }
        g_chring[CHRING_MAX];
static uint32_t g_chring_i;
#endif

/* Slice 64 (tier 2): which Linux syscalls actually dominate, so BKL fast
 * paths get aimed by data. Counted under the BKL (plain increments safe);
 * the fast-path counters below are lock-free single-word adds. Dumped +
 * reset by lx_dump_syscall_top() from the 60s deep dump. */
volatile uint64_t g_lx_fast_futex;
volatile uint64_t g_lx_fast_time;             /* bumped in the time fast path */
volatile uint64_t g_lx_fast_yield;            /* slice 64: BKL-free yields */
static uint32_t g_lx_nrcount[512];
/* Slice 64b: BKL HOLD time per syscall (TSC cycles between bkl_enter and
 * the syscall's return), so "who holds the lock long" is a measurement
 * rather than a suspicion. Sampled on the slow (BKL) path only. */
/* Slice 64c: hold buckets -- 0..511 Linux syscall numbers, 512..767 native
 * tobyOS syscall numbers, plus two catch-alls. */
#define HOLD_SLOTS  770
#define HOLD_IDLE   768
#define HOLD_KERNEL 769
static uint64_t g_lx_holdtsc[HOLD_SLOTS];

static const char *hold_name(int i) {
    static char nb[16];
    if (i == HOLD_IDLE)   return "idle(pid0)";
    if (i == HOLD_KERNEL) return "kernel";
    if (i >= 512) {                      /* "nat:<n>" -- no snprintf in-kernel */
        int v = i - 512, o = 0;
        nb[o++] = 'n'; nb[o++] = 'a'; nb[o++] = 't'; nb[o++] = ':';
        if (v >= 100) nb[o++] = (char)('0' + (v / 100) % 10);
        if (v >= 10)  nb[o++] = (char)('0' + (v / 10) % 10);
        nb[o++] = (char)('0' + v % 10);
        nb[o] = 0;
        return nb;
    }
    return lx_scname(i);
}

static inline uint64_t lx_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Slice 64b: called from bkl_exit with the cycles this CPU actually HELD
 * the BKL, attributed to the syscall the caller is inside (proc->cursys,
 * stamped at linux_syscall entry). This is the number that matters: a
 * blocking syscall's wall time is irrelevant because the scheduler drops
 * the lock while blocked, so only these enter->exit segments block peers. */
/* Slice 64c: which bucket the BKL segment on each CPU belongs to, stamped
 * by the syscall entry right after bkl_enter(). Reading the proc's cursys
 * at bkl_exit (the first attempt) CANNOT work: linux_syscall clears cursys
 * to -1 before the lock is released, so every segment fell through to
 * cursys_nat -- which for any exec'd process is permanently 142, because
 * execve never returns to clear its own stamp. That misread reported
 * "execve = 93% of all BKL hold time" for a run with ~20 execve calls.
 * Caveat, stated: if a syscall BLOCKS, the scheduler drops the lock and
 * another proc's syscall may stamp this CPU, so segments after a blocking
 * switch can attribute to the wrong (still real) syscall. */
int g_bkl_sysno[MAX_CPUS];

void bkl_hold_account(uint64_t cyc) {
    struct proc *p = current_proc();
    uint32_t ci = smp_current_cpu_idx();
    int slot = (ci < MAX_CPUS) ? g_bkl_sysno[ci] : -1;
    if (slot >= 0 && slot < HOLD_SLOTS) { g_lx_holdtsc[slot] += cyc; return; }
    if (p && p->pid == 0) { g_lx_holdtsc[HOLD_IDLE] += cyc; return; }
    g_lx_holdtsc[HOLD_KERNEL] += cyc;
}

void lx_dump_syscall_top(void) {
    kprintf("[lx-top] fast: time=%lu futex=%lu yield=%lu | slow(BKL) top:",
            (unsigned long)g_lx_fast_time, (unsigned long)g_lx_fast_futex,
            (unsigned long)g_lx_fast_yield);
    for (int rank = 0; rank < 8; rank++) {
        uint32_t best = 0; int bi = -1;
        for (int i = 0; i < 512; i++)
            if (g_lx_nrcount[i] > best) { best = g_lx_nrcount[i]; bi = i; }
        if (bi < 0 || best == 0) break;
        kprintf(" %s=%u", lx_scname(bi), best);
        g_lx_nrcount[bi] = 0;                 /* consume so next rank differs */
    }
    kprintf("\n");
    /* Slice 64b: the same list ranked by BKL HOLD TIME (Mcycles) -- this is
     * what actually blocks other CPUs, and it is a different ranking than
     * call COUNT (a rare syscall that holds for ms hurts more than a
     * frequent one that holds for ns). */
    kprintf("[lx-hold] BKL hold Mcyc top:");
    for (int rank = 0; rank < 10; rank++) {
        uint64_t best = 0; int bi = -1;
        for (int i = 0; i < HOLD_SLOTS; i++)
            if (g_lx_holdtsc[i] > best) { best = g_lx_holdtsc[i]; bi = i; }
        if (bi < 0 || best == 0) break;
        kprintf(" %s=%lu", hold_name(bi), (unsigned long)(best >> 20));
        g_lx_holdtsc[bi] = 0;
    }
    kprintf("\n");
    for (int i = 0; i < 512; i++) g_lx_nrcount[i] = 0;
    for (int i = 0; i < HOLD_SLOTS; i++) g_lx_holdtsc[i] = 0;
}

static long linux_syscall(long n, long a1, long a2, long a3, long a4, long a5) {
    if (n >= 0 && n < 512) g_lx_nrcount[n]++;   /* slice 64: BKL-held count */
    /* Slice 64b: measure how long this syscall keeps the BKL. Note the
     * body may DROP the lock while blocking (the scheduler does that around
     * blocking switches), so this is an upper bound on hold time -- read it
     * as "wall time under the syscall", and pair it with [bkl] waits. */
    uint64_t t_hold0 = lx_rdtsc();
#ifdef CHROMIUM_BOOT
    int chan_fd = -1;
    switch (n) {
    case LX_read: case LX_write: case LX_readv: case LX_writev:
    case LX_sendmsg: case LX_recvmsg: case LX_sendto: case LX_recvfrom: {
        struct file *cf = fd_lookup((int)a1);
        if (cf && cf->kind == FILE_KIND_SOCKET) chan_fd = (int)a1;
        break;
    }
    default: break;
    }
    /* Slice 56: stamp which syscall this thread is inside (and since when) so
     * the [cur] dump can say what a "RUNNING" thread is doing in-kernel. */
    struct proc *curp = current_proc();
    if (curp) { curp->cursys = (int32_t)n; curp->cursys_ns = perf_now_ns(); }
#endif
    long r = linux_syscall_impl(n, a1, a2, a3, a4, a5);
    g_lx_recent_r[g_lx_slot_cpu[smp_current_cpu_idx() % LX_MAXCPU]] = r;
    (void)t_hold0;   /* slice 64b: see bkl_hold_account -- wall time under a
                      * syscall is NOT hold time (blocking drops the BKL);
                      * the real accounting happens in bkl_exit. */
#ifdef CHROMIUM_BOOT
    if (curp) curp->cursys = -1;
    if (chan_fd >= 0) {
        /* Ring-record EVERY socket op (silent), whatever the kprintf caps do. */
        {
            uint32_t ci = g_chring_i++ % CHRING_MAX;
            g_chring[ci].tgid = curp ? curp->tgid : -1;
            g_chring[ci].pid  = curp ? curp->pid  : -1;
            g_chring[ci].nr   = (short)n;
            g_chring[ci].fd   = (short)chan_fd;
            g_chring[ci].ret  = r;
            g_chring[ci].ms   = (uint32_t)(perf_now_ns() / 1000000ull);
        }
        static int chan_c = 0;
        /* Slice 56 R1/R2: any cap counted from boot exhausts during startup
         * churn (500 by ~17s, 1500 by ~21.5s) and misses the window that
         * matters -- the watch-page renderer's exit at ~25s and the ~35s
         * message-flow stop. Log only from 18s on. */
        /* Slice 74: the 18s gate was tuned for the renderer-death hunt (the
         * interesting window was late). FULL chrome dies at ~5s inside
         * crashpad's handler handshake -- a sendmsg/recvmsg pair with
         * SCM_RIGHTS over a SOCK_SEQPACKET socketpair, which on Linux is
         * immediately followed by recvmsg + PR_SET_PTRACER + sigaltstack,
         * and on tobyOS is followed by nothing but exit. Ungate for
         * CHROME_FULL so those calls and their RETURN VALUES are visible. */
#ifdef CHROME_FULL
        if (chan_c < 1200) {
#else
        if (perf_now_ns() >= 18000000000ull && chan_c < 1200) {
#endif
            chan_c++;
            struct proc *me = current_proc();
            /* For a RECEIVE that returned data, decode the FRONT of the received
             * bytes. MEASURED (slice 33) channel-frame layout on this build:
             *   off 0: u16 num_header_bytes (=16)  | u16 version (0/1)
             *   off 4: u32 num_bytes  == the TOTAL message size
             * so w1 (off 4) is the declared size and it MATCHES the delivered
             * byte count r for every fully-delivered message -- framing is
             * size-consistent, NOT truncated. (w1 > r only when a message is
             * bigger than the read buffer, e.g. 8504 into 4096: normal multi-read
             * reassembly, expected.) The VALIDATION_ERROR_ILLEGAL_MEMORY_RANGE is
             * therefore a NESTED pointer inside a correctly-framed message -- to
             * chase it, decode the mojom body past the 16-byte channel header.
             * Walk the iovec for recvmsg/readv; use the buffer directly
             * otherwise. copy_from_user_nofault so a bad ptr can't fault us. */
            uint32_t hd[16]; int have_hdr = 0;   /* first 64 bytes of the payload */
            for (int i = 0; i < 16; i++) hd[i] = 0;
            if (r >= 8 && (n == LX_recvmsg || n == LX_recvfrom ||
                           n == LX_read    || n == LX_readv)) {
                uint64_t bufp = 0;
                if (n == LX_recvmsg) {
                    uint64_t iov = 0;               /* msghdr.msg_iov @ +16 */
                    if (copy_from_user_nofault(&iov, (void *)(uintptr_t)(a2 + 16),
                                               sizeof(iov)) == 0 && iov)
                        (void)copy_from_user_nofault(&bufp,
                                (void *)(uintptr_t)iov, sizeof(bufp)); /* iov[0].base */
                } else if (n == LX_readv) {
                    (void)copy_from_user_nofault(&bufp,
                            (void *)(uintptr_t)a2, sizeof(bufp));      /* iov[0].base */
                } else {
                    bufp = (uint64_t)a2;                                /* raw buffer */
                }
                if (bufp) {
                    size_t want = (r < 64) ? (size_t)r : 64;
                    if (copy_from_user_nofault(hd, (void *)(uintptr_t)bufp,
                                               want) == 0)
                        have_hdr = 1;
                }
            }
            if (have_hdr) {
                /* Channel frame: off0 = u16 num_header_bytes | u16 version;
                 *                off4 = u32 total size (== r).
                 * At off `num_header_bytes` (=16) begins an IPCZ message (NOT a
                 * mojom message: chrome uses ipcz, so the mojom parcel is
                 * reassembled from ipcz parcels/shared-memory fragments and is
                 * NOT at a fixed socket offset). The words below are ipcz
                 * transport-header fields; `seq` increments per message. Kept as
                 * structural visibility only -- do NOT read a mojom num_bytes
                 * here (an earlier pass did and saw phantom 65560/1310744
                 * "overruns"; those are ipcz header bytes, not a size). */
                uint16_t chdr_hb = (uint16_t)(hd[0] & 0xffff);
                uint16_t chdr_v  = (uint16_t)(hd[0] >> 16);
                uint32_t csize   = hd[1];
                uint32_t iw0     = hd[4];   /* ipcz hdr word @ off16 */
                uint32_t iseq    = hd[6];   /* ipcz sequence no. @ off24 (increments) */
                kprintf("[chan] pid=%d %s fd=%d -> %ld  chan[hb=%u v=%u sz=%u] "
                        "ipcz[w0=0x%x seq=%u]\n",
                        me ? me->pid : -1, lx_scname(n), chan_fd, r,
                        chdr_hb, chdr_v, csize, iw0, iseq);
            } else {
                kprintf("[chan] pid=%d %s fd=%d a2=0x%lx a3=%lu -> %ld\n",
                        me ? me->pid : -1, lx_scname(n), chan_fd,
                        (unsigned long)a2, (unsigned long)a3, r);
            }
        }
    }
#endif
    return r;
}

static long linux_syscall_impl(long n, long a1, long a2, long a3, long a4, long a5) {
    {
        uint32_t i = g_lx_recent_i++ % LX_RECENT;
        g_lx_recent_n[i]   = n;
        g_lx_recent_tid[i] = (int)current_proc()->pid;
        g_lx_recent_a1[i]  = a1;
        g_lx_recent_r[i]   = 0x5eed;         /* "did not return" sentinel */
        /* Remember which slot this CPU is filling so the wrapper can stamp
         * the result once the body returns. Per-CPU rather than a single
         * global because syscalls run concurrently on every core. */
        g_lx_slot_cpu[smp_current_cpu_idx() % LX_MAXCPU] = i;
    }
#ifdef LINUX_SYSCALL_TRACE
    kprintf("[lxtrace] %s[%ld] n=%ld(%s) a=%lx,%lx,%lx,%lx,%lx\n",
            current_proc()->name, (long)current_proc()->pid, n, lx_scname(n),
            (unsigned long)a1, (unsigned long)a2, (unsigned long)a3,
            (unsigned long)a4, (unsigned long)a5);
#endif
    switch (n) {
    /* ---- exits ---- */
    case LX_exit:
        sys_exit((int)a1);          /* noreturn */
        return 0;
    case LX_exit_group:
#ifdef CHROMIUM_BOOT
        /* Slice 56 wall R1 / Slice 83: dump prelude on non-zero exit_group. */
        {
            struct proc *xp = current_proc();
            /* Slice 89: ALSO dump a SUCCESSFUL exit that did almost nothing.
             * chrome_crashpad_handler execs and exit_group(0)s 2ms later
             * after FOUR syscalls; every "sendmsg -> EPIPE -> abort 191"
             * downstream is a consequence of that. The old non-zero gate
             * made the one exit that matters the one exit we never saw --
             * a process that dies successfully without doing its job is
             * exactly as interesting as one that crashes. */
            bool quiet_death = xp && a1 == 0 && xp->syscall_count < 32 &&
                               xp->personality == 1;
            if (xp && (a1 != 0 || quiet_death)) {
                kprintf("[xexit] pid=%d '%s' exit_group(%ld) syscalls=%lu%s"
                        " -- syscall tail:\n",
                        xp->pid, xp->name, (long)a1,
                        (unsigned long)xp->syscall_count,
                        quiet_death ? " [QUIET DEATH: exited 0 having done"
                                      " essentially nothing]" : "");
                extern void lx_dump_recent_syscalls(void);
                lx_dump_recent_syscalls();
            }
            if (xp && xp->is_renderer) {
                kprintf("[rexit] RENDERER pid=%d tgid=%d exit_group(%ld) -- "
                        "last channel ops of this thread group:\n",
                        xp->pid, xp->tgid, (long)a1);
                {
                    int keep[48]; uint32_t ki = 0;
                    for (uint32_t k = 0; k < CHRING_MAX; k++) {
                        uint32_t idx = (g_chring_i + k) % CHRING_MAX;
                        if (g_chring[idx].ms == 0) continue;
                        if (g_chring[idx].tgid != xp->tgid) continue;
                        keep[ki % 48] = (int)idx; ki++;
                    }
                    uint32_t start = (ki < 48) ? 0 : ki - 48;
                    for (uint32_t k2 = start; k2 < ki; k2++) {
                        struct chring_ent *e = &g_chring[keep[k2 % 48]];
                        kprintf("  [chring] %ums [%d] %s(fd=%d) -> %ld\n",
                                (unsigned)e->ms, e->pid, lx_scname(e->nr),
                                (int)e->fd, (long)e->ret);
                    }
                    if (ki == 0)
                        kprintf("  [chring] (no channel ops recorded for tgid %d)\n",
                                xp->tgid);
                }
            }
        }
#endif
        /* Whole thread-group death (Linux exit_group), not per-thread exit. */
        proc_exit_group((int)a1);   /* noreturn */
        return 0;

    /* ---- plain byte I/O (tobyOS handlers already take user buffers) ---- */
    case LX_read:
#ifdef CHROMIUM_BOOT
        /* Slice 39: trace DevTools-pipe I/O (chrome reads commands on fd 3,
         * writes responses on fd 4). Answers "does chrome service the pipe at
         * all?" -- if this never fires, chrome never reached the pipe reader
         * and the stall is upstream of DevTools; if it does, the CDP framing
         * is the suspect. Only fd 3/4 + PIPE kind, so the log stays sparse. */
        if (((int)a1 == 3 || (int)a1 == 4)) {
            struct file *df = fd_lookup((int)a1);
            if (df && (df->kind == FILE_KIND_PIPE_R || df->kind == FILE_KIND_PIPE_W)) {
                struct proc *dp = current_proc();
                long dr = sys_read((int)a1, (void *)a2, (size_t)a3);
                /* Slice 63c: was UNCAPPED (5k+ lines/run; serial bytes are
                 * VM exits). First 64 keep bootstrap visibility; then 1 in
                 * 128 keeps a liveness sample. */
                static unsigned dprc;
                if (dprc < 64 || (dprc & 127u) == 0)
                    kprintf("[devpipe] pid=%d read(fd=%d, %lu) -> %ld #%u\n",
                            dp ? dp->pid : -1, (int)a1,
                            (unsigned long)a3, dr, dprc);
                dprc++;
                return dr;
            }
        }
#endif
        return sys_read((int)a1, (void *)a2, (size_t)a3);
    case LX_write:
#ifdef CHROMIUM_BOOT
        if (((int)a1 == 3 || (int)a1 == 4)) {
            struct file *df = fd_lookup((int)a1);
            if (df && (df->kind == FILE_KIND_PIPE_R || df->kind == FILE_KIND_PIPE_W)) {
                struct proc *dp = current_proc();
                long dw = sys_write((int)a1, (const void *)a2, (size_t)a3);
                static unsigned dpwc;            /* slice 63c: cap, as read */
                if (dpwc < 64 || (dpwc & 127u) == 0)
                    kprintf("[devpipe] pid=%d write(fd=%d, %lu) -> %ld #%u\n",
                            dp ? dp->pid : -1, (int)a1,
                            (unsigned long)a3, dw, dpwc);
                dpwc++;
                return dw;
            }
        }
#endif
        return sys_write((int)a1, (const void *)a2, (size_t)a3);
    case LX_pread64:
        return sys_pread64((int)a1, (void *)a2, (size_t)a3, (int64_t)a4);
    case LX_pwrite64:
        return sys_pwrite64((int)a1, (const void *)a2, (size_t)a3, (int64_t)a4);
    case LX_close:  return do_syscall(SYS_CLOSE, a1, 0, 0, 0, 0);
    case LX_lseek: {
        struct file *lf = fd_lookup((int)a1);
        if (lf && lf->kind == FILE_KIND_MEMFD) {   /* cursor lives in vfs.pos */
            int64_t off = (int64_t)a2; int whence = (int)a3;
            int64_t base = (whence == 0) ? 0
                         : (whence == 1) ? (int64_t)lf->vfs.pos
                         : (whence == 2) ? (int64_t)memfd_size(lf->memfd) : -1;
            if (base < 0) return -ABI_EINVAL;
            int64_t np = base + off;
            if (np < 0) return -ABI_EINVAL;
            lf->vfs.pos = (size_t)np;
            return np;
        }
        return do_syscall(SYS_LSEEK, a1, a2, a3, 0, 0);
    }
    case LX_open: {                    /* (path, flags, mode) */
        /* Opening a directory yields a getdents64-capable dir fd; files
         * fall through to the normal VFS open. */
        char kpath[ABI_PATH_MAX];
        bool got_kp = (resolve_user_path((const char *)a1, kpath, sizeof kpath) == 0);
        if (got_kp) {
            struct vfs_stat vs;
            if (vfs_stat(kpath, &vs) == VFS_OK && vs.type == VFS_TYPE_DIR)
                return linux_open_dir(kpath);
        }
        long ofd = do_syscall(SYS_OPEN, a1, a2, a3, 0, 0);
#ifdef CHROMIUM_BOOT
        /* [lopen] path->fd, so [libmap]'s per-mapping fd can be resolved to a
         * FILE PATH offline (match a [libmap] fd=N line to the most recent
         * [lopen] fd=N). Enables symbolizing a stack rip: base<-libmap, path<-
         * this, then objdump that .so at rip-base. */
        if (ofd >= 0 && got_kp) {
            static int lo = 0;
            if (lo < 300) { lo++;
                struct proc *me = current_proc();
                kprintf("[lopen] pid=%d fd=%ld %s\n", me ? me->pid : -1, ofd, kpath);
            }
        }
#endif
        return ofd;
    }
    case LX_openat: {                  /* (dirfd, path, flags, mode) */
        /* Resolves against DIRFD for relative paths (see
         * resolve_user_path_at). chrome's sandbox does
         * openat(proc_fd, "self/fd/", O_DIRECTORY|...) -- proc_util.cc:79 --
         * and PCHECKs the result, which was fatal while we resolved against
         * the cwd.
         * LIMITATION: only the DIRECTORY arm below opens by resolved kernel
         * path. A dirfd-relative open of a non-directory still falls through
         * to SYS_OPEN with the raw user pointer, i.e. cwd-relative. Nothing
         * exercised needs it; wire a by-kpath file open when something does. */
        char kpath[ABI_PATH_MAX];
        bool got_kp = (resolve_user_path_at((int)a1, (const char *)a2,
                                            kpath, sizeof kpath) == 0);
        if (got_kp) {
            struct vfs_stat vs;
            if (vfs_stat(kpath, &vs) == VFS_OK && vs.type == VFS_TYPE_DIR)
                return linux_open_dir(kpath);
#ifdef CHROMIUM_BOOT
            /* SQLite WAL -shm: memfd pre-size still busy-loops (mmap/lock).
             * ENOENT forces rollback-journal fallback (ServerCertificate path). */
            {
                size_t kp_n = 0;
                while (kpath[kp_n]) kp_n++;
                if (kp_n >= 4 && kpath[kp_n - 4] == '-' &&
                    kpath[kp_n - 3] == 's' && kpath[kp_n - 2] == 'h' &&
                    kpath[kp_n - 1] == 'm') {
                    static int shmd;
                    if (shmd < 4) { shmd++;
                        kprintf("[lopen] deny WAL -shm %s -> ENOENT\n", kpath); }
                    return -ABI_ENOENT;
                }
            }
#endif
        }
        long ofd = do_syscall(SYS_OPEN, a2, a3, a4, 0, 0);
#ifdef CHROMIUM_BOOT
        if (ofd >= 0 && got_kp) {
            static int lo = 0;
            if (lo < 300) { lo++;
                struct proc *me = current_proc();
                kprintf("[lopen] pid=%d fd=%ld %s\n", me ? me->pid : -1, ofd, kpath);
            }
        }
#endif
        return ofd;
    }
    case LX_creat:                     /* (path, mode) */
        /* creat(2) == open(path, O_WRONLY|O_CREAT|O_TRUNC, mode). Linux bits:
         * O_WRONLY=1 O_CREAT=0x40 O_TRUNC=0x200. chrome's screenshot writer
         * (headless_command_handler -> base::WriteFile) creates the PNG this
         * way; ENOSYS here was the only thing between a successfully captured
         * SwiftShader frame and /data/shot.png (slice 38 iter 3). */
        return do_syscall(SYS_OPEN, a1, 0x241, a2, 0, 0);
    case LX_fchown:                    /* (fd, owner, group) */
        /* Single-user OS: ownership is a no-op; report success so file
         * writers that chown their output don't error out. */
        return 0;
    case LX_sched_getparam: {          /* (pid, struct sched_param *) */
        int zero = 0;
        if (a2 && copy_to_user((void *)a2, &zero, sizeof zero) != 0)
            return -ABI_EFAULT;
        return 0;
    }
    case LX_sched_getscheduler:        /* (pid) -> SCHED_OTHER */
        return 0;
    case LX_dup:    return do_syscall(SYS_DUP, a1, 0, 0, 0, 0);
    /* B12: shell pipelines wire stage fds with dup2/dup3 and create the pipe
     * with pipe or pipe2. dup3's flags (O_CLOEXEC) and pipe2's flags
     * (O_CLOEXEC/O_NONBLOCK) are accepted but not tracked -- short-lived
     * pipeline stages exec immediately, so CLOEXEC is moot and the blocking
     * pipe semantics already give correct EOF/EPIPE. dup3 requires
     * oldfd != newfd (EINVAL otherwise), unlike dup2. */
    case LX_dup2:   return do_syscall(SYS_DUP2, a1, a2, 0, 0, 0);
    case LX_dup3:
        if (a1 == a2) return -ABI_EINVAL;
        return do_syscall(SYS_DUP2, a1, a2, 0, 0, 0);
    case LX_pipe:   return do_syscall(SYS_PIPE, a1, 0, 0, 0, 0);
    case LX_pipe2:  return do_syscall(SYS_PIPE, a1, 0, 0, 0, 0);

    /* ---- Track C: eventfd/eventfd2 (libcurl multi-handle wakeup fd) ---- */
    case LX_eventfd:   return lx_eventfd((unsigned int)a1, 0);
    case LX_eventfd2:  return lx_eventfd((unsigned int)a1, (unsigned int)a2);
    case 319:          /* memfd_create(name, flags) */
        return lx_memfd_create((const char *)a1, (unsigned int)a2);

    /* ---- B14: BSD sockets (FILE_KIND_SOCKET fds; poll/epoll-ready) ---- */
    case LX_socket:      return lx_socket((int)a1, (int)a2, (int)a3);
    case LX_bind:        return lx_bind((int)a1, (uint64_t)a2, (uint32_t)a3);
    case LX_listen:      return lx_listen((int)a1, (int)a2);
    case LX_accept:      return lx_accept((int)a1, (uint64_t)a2, (uint64_t)a3, 0);
    /* accept4's 4th arg carries SOCK_NONBLOCK/SOCK_CLOEXEC for the NEW fd
     * (they are not inherited from the listener). */
    case LX_accept4:     return lx_accept((int)a1, (uint64_t)a2, (uint64_t)a3, (int)a4);
    case LX_connect:     return lx_connect((int)a1, (uint64_t)a2, (uint32_t)a3);
    case LX_sendto:      return lx_send((int)a1, (uint64_t)a2, (size_t)a3, (uint64_t)a5);
    /* recvfrom(fd, buf, len, flags, src_addr, addrlen). a4 is FLAGS and was
     * being dropped -- so MSG_DONTWAIT was silently ignored and every such
     * "poll me" read blocked instead. */
    case LX_recvfrom:    return lx_recv((int)a1, (uint64_t)a2, (size_t)a3, (uint64_t)a5, 0, (int)a4);
    case LX_sendmsg:     return lx_sendmsg((int)a1, (uint64_t)a2, (int)a3);
    case LX_recvmsg:     return lx_recvmsg((int)a1, (uint64_t)a2, (int)a3);
    case LX_setsockopt:  return lx_setsockopt((int)a1, (int)a2, (int)a3, (uint64_t)a4, (uint32_t)a5);
    case LX_getsockopt:  return lx_getsockopt((int)a1, (int)a2, (int)a3, (uint64_t)a4, (uint64_t)a5);
    case LX_getsockname: return lx_getsockname((int)a1, (uint64_t)a2, (uint64_t)a3);
    case LX_getpeername: return lx_getpeername((int)a1, (uint64_t)a2, (uint64_t)a3);
    case LX_socketpair:  return lx_socketpair((int)a1, (int)a2, (int)a3, (uint64_t)a4);
    case LX_shutdown:    return 0;       /* half-close not modelled; close() tears down */

    /* gettimeofday(2): fill timeval from the monotonic clock (no RTC epoch; the
     * value increases, which is what chrome's timestamp deltas need). tz ign. */
    case LX_gettimeofday:
        if (a1) {
            /* Was time since BOOT presented as the Unix epoch -- so userspace
             * believed it was Jan 1 1970 plus a few seconds. Anchor to the RTC. */
            uint64_t ns = lx_realtime_ns(perf_now_ns());
            long tv[2] = { (long)(ns / 1000000000ull),
                           (long)((ns / 1000ull) % 1000000ull) };
            if (copy_to_user((void *)a1, tv, sizeof tv) != 0) return -ABI_EFAULT;
        }
        return 0;

    /* ---- B13: poll / select / epoll (readiness multiplexing) ---- */
    case LX_poll:                  /* poll(fds, nfds, timeout_ms) */
        return lx_do_poll((uint64_t)a1, (unsigned long)a2, (long)(int)a3);
    case LX_ppoll: {               /* ppoll(fds, nfds, *timespec, *sigmask, sz) */
        long ms = -1;              /* NULL timespec == infinite */
        if (a3) {
            int64_t ts[2];
            if (copy_from_user(ts, (const void *)a3, sizeof(ts)) != 0)
                return -ABI_EFAULT;
            ms = (long)(ts[0] * 1000 + ts[1] / 1000000);
        }
        return lx_do_poll((uint64_t)a1, (unsigned long)a2, ms);
    }
    case LX_select: {              /* select(nfds, *rd, *wr, *ex, *timeval) */
        long ms = -1;
        if (a5) {
            int64_t tv[2];
            if (copy_from_user(tv, (const void *)a5, sizeof(tv)) != 0)
                return -ABI_EFAULT;
            ms = (long)(tv[0] * 1000 + tv[1] / 1000);
        }
        return lx_do_select((int)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, ms);
    }
    case LX_pselect6: {            /* pselect6(nfds, *rd, *wr, *ex, *timespec, *sig) */
        long ms = -1;
        if (a5) {
            int64_t ts[2];
            if (copy_from_user(ts, (const void *)a5, sizeof(ts)) != 0)
                return -ABI_EFAULT;
            ms = (long)(ts[0] * 1000 + ts[1] / 1000000);
        }
        return lx_do_select((int)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, ms);
    }
    case LX_epoll_create:          /* epoll_create(size) -- size ignored */
    case LX_epoll_create1:         /* epoll_create1(flags) -- flags ignored */
        return lx_epoll_create();
    case LX_epoll_ctl:             /* epoll_ctl(epfd, op, fd, *event) */
        return lx_epoll_ctl((int)a1, (int)a2, (int)a3, (uint64_t)a4);
    case LX_epoll_wait:            /* epoll_wait(epfd, *events, maxevents, timeout) */
    case LX_epoll_pwait: {         /* epoll_pwait(... , *sigmask) -- sigmask ignored */
#ifdef CHROMIUM_BOOT
        waitt_enter(n, (uint64_t)(int)a1, (long)(int)a4);  /* arg = epoll fd */
#endif
        long er = lx_epoll_wait((int)a1, (uint64_t)a2, (int)a3, (long)(int)a4);
#ifdef CHROMIUM_BOOT
        waitt_exit();
#endif
        return er;
    }
    case LX_getcwd: return do_syscall(SYS_GETCWD, a1, a2, 0, 0, 0);
    case LX_chdir:  return do_syscall(SYS_CHDIR, a1, 0, 0, 0, 0);
    case LX_mkdir:  return do_syscall(SYS_MKDIR, a1, a2, 0, 0, 0);

    /* chmod/fchmodat. Slice 86: these used to validate the path and then
     * throw the mode away, on the belief that the VFS did not model
     * permission bits -- but vfs_chmod() has stored them all along. The
     * no-op was silently wrong for anything that tightens a mode and then
     * reads it back: NSS chmods its key database to 0600 and refuses to use
     * it if that does not stick, and chrome CHECKs its singleton directory
     * for exactly 0700. Store the mode for real; a filesystem that cannot
     * hold one (VFS_ERR_ROFS) still reports success rather than breaking
     * callers that only care that the call did not fail. */
    case LX_chmod: case LX_fchmodat: {
        const char *up = (const char *)(LX_chmod == n ? a1 : a2);
        uint32_t    md = (uint32_t)(LX_chmod == n ? a2 : a3) & 07777u;
        char kpath[ABI_PATH_MAX];
        if (resolve_user_path(up, kpath, sizeof kpath) != 0) return -ABI_EFAULT;
        struct vfs_stat vs;
        if (vfs_stat(kpath, &vs) != VFS_OK) return -ABI_ENOENT;
        int rc = vfs_chmod(kpath, md);
        if (rc == VFS_OK || rc == VFS_ERR_ROFS) return 0;
        return (rc == VFS_ERR_PERM) ? -ABI_EACCES : 0;
    }
    case LX_fchmod:
        return fd_lookup((int)a1) ? 0 : -ABI_EBADF;
    case LX_unlink: return do_syscall(SYS_UNLINK, a1, 0, 0, 0, 0);

    /* ---- fs mutations chrome's profile stores (leveldb/SQLite) require ---- */
    case LX_rename:                 /* (oldpath, newpath) */
        return sys_rename((const char *)a1, (const char *)a2);
    case LX_renameat:               /* (olddirfd, oldpath, newdirfd, newpath) */
    case LX_renameat2:              /* (..., flags) -- AT_FDCWD + flags ignored */
        return sys_rename((const char *)a2, (const char *)a4);
    case LX_unlinkat:               /* (dirfd, path, flags) -- AT_FDCWD; tobyfs
                                     * unlink removes files AND empty dirs, so it
                                     * serves AT_REMOVEDIR too. */
        return do_syscall(SYS_UNLINK, a2, 0, 0, 0, 0);
    case LX_fsync:                  /* tobyfs writes are journalled + write-through
                                     * (bcache flushes on commit), so a successful
                                     * write is already durable -- report success. */
    case LX_fdatasync:
        return 0;
    case LX_flock:                  /* single-process chrome: advisory locks are a
                                     * no-op that must SUCCEED (leveldb/SQLite hold
                                     * a LOCK file and bail if locking errors). */
        return 0;

    /* ---- process control (B8): the shell forks, execs, and waits ---- */
    case LX_fork:
    case LX_vfork: {
        /* LX_vfork / CLONE_VFORK → share-until-exec. Plain fork → CoW copy. */
        long fr = (n == LX_vfork) ? sys_fork_share()
                                  : do_syscall(ABI_SYS_FORK, 0, 0, 0, 0, 0);
#ifdef CHROMIUM_BOOT
        {   static int pf = 0;
            if (pf < 200) { pf++;
                struct proc *me = current_proc();
                kprintf("[fork] parent pid=%d returning %ld%s\n",
                        me ? me->pid : -1, fr,
                        (n == LX_vfork) ? " (share)" : ""); } }
#endif
        return fr;
    }
    case LX_clone:
        /* clone(flags, stack, ptid, ctid, tls).
         *
         * Slice 76: dispatch on CLONE_THREAD (0x10000), NOT CLONE_VM (0x100).
         * CLONE_VM alone only means "share the address space", which is ALSO
         * what vfork/posix_spawn use for a child that shares memory just
         * until it execs -- a separate PROCESS. Testing CLONE_VM turned
         * those into threads. That is exactly how full chrome died:
         * crashpad's handler child called clone3 with
         * CLONE_VM|CLONE_VFORK|CLONE_INTO_CGROUP (0x100004100, CLONE_THREAD
         * CLEAR) to spawn the grandchild that execs chrome_crashpad_handler;
         * we made it a thread, no grandchild ever existed, the child exited,
         * its socketpair endpoint was torn down, and the browser's handshake
         * sendmsg came back EPIPE -> CHECK -> INT3 -> exit 191. A real
         * pthread always sets CLONE_THREAD (measured alongside it:
         * 0x3d0f00 = VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|SETTLS|PARENT_SETTID). */
        if ((uint64_t)a1 & 0x10000u /* CLONE_THREAD */)
            return sys_clone_thread((uint64_t)a1, (uint64_t)a2, (uint64_t)a3,
                                    (uint64_t)a4, (uint64_t)a5);
        {
            uint64_t cfl = (uint64_t)a1;
            /* Share only for CLONE_VFORK/VM. Plain fork stays CoW (glibc
             * resumes on the parent's stack frame — a private RSP breaks it). */
            int share = (cfl & 0x4000u /* CLONE_VFORK */) ||
                        (cfl & 0x100u  /* CLONE_VM */);
#ifdef CHROMIUM_BOOT
            { static int cf = 0; if (cf < 200) { cf++;
                struct proc *me = current_proc();
                kprintf("[fork] clone flags=0x%lx pid=%d -> %s\n",
                        (unsigned long)cfl, me ? me->pid : -1,
                        share ? "share" : "cow"); } }
            /* Tier 2.5: STW tg_vm_quiesce + eager CoW make limited chrome
             * CoW forks viable. Cap still blocks fork storms (xdg-settings
             * loops); raised from 1 — denying #2 left UI stuck before any
             * browser CreateWindow/MapWindow (DevTools never answered). */
            if (!share) {
                struct proc *me = current_proc();
                if (me && me->tgid == 3) {
                    static int chrome_cow_forks;
                    /* Multi-process Ozone (no --single-process): allow a
                     * handful of utility/renderer CoW forks; still cap storms. */
                    if (chrome_cow_forks++ >= 16) {
                        kprintf("[fork] deny chrome CoW fork #%d pid=%d\n",
                                chrome_cow_forks, me->pid);
                        return -ABI_EAGAIN;
                    }
                    kprintf("[fork] allow chrome CoW fork #%d pid=%d\n",
                            chrome_cow_forks, me->pid);
                }
            }
#endif
            long cr = share ? sys_fork_share()
                            : do_syscall(ABI_SYS_FORK, 0, 0, 0, 0, 0);
#ifdef CHROMIUM_BOOT
            { static int crl = 0; if (crl < 200) { crl++;
                struct proc *me = current_proc();
                kprintf("[fork] parent pid=%d clone-returning %ld\n",
                        me ? me->pid : -1, cr); } }
#endif
            return cr;
        }
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
        if (code < 0) {
#ifdef CHROMIUM_BOOT
            /* Slice 83: crashpad's DoubleForkAndExec REQUIRES
             * waitpid(child) == child AND WIFEXITED(status) &&
             * WEXITSTATUS(status) == 0; anything else makes StartHandler
             * return false, which is a fatal CHECK for the browser. Its
             * wait4 is the last call before the abort, so log both outcomes
             * with the values crashpad actually tests. */
            { struct proc *wp = current_proc();
              kprintf("[wait4] pid=%d wait(%d) -> ECHILD (proc_wait=%d)\n",
                      wp ? wp->pid : -1, pid, code); }
#endif
            return -ABI_ECHILD;
        }
        if (ustatus) {
            /* Linux wait status: normal exit -> (code & 0xff) << 8, with the
             * low 7 bits zero so WIFEXITED is true / WEXITSTATUS == code. */
            uint32_t wstatus = ((uint32_t)code & 0xff) << 8;
            if (put_user_u32(ustatus, wstatus) != 0) return -ABI_EFAULT;
        }
#ifdef CHROMIUM_BOOT
        { static int w4 = 0; if (w4 < 16) { w4++;
            struct proc *wp = current_proc();
            kprintf("[wait4] pid=%d wait(%d) -> %d code=%d wstatus=0x%x "
                    "(WIFEXITED=%d WEXITSTATUS=%d)\n",
                    wp ? wp->pid : -1, pid, pid, code,
                    (unsigned)(((uint32_t)code & 0xff) << 8),
                    ((((uint32_t)code & 0xff) << 8) & 0x7f) == 0 ? 1 : 0,
                    (int)((((uint32_t)code & 0xff) << 8) >> 8) & 0xff); } }
#endif
        return pid;
    }
    /* Job control: tobyOS has a single global foreground pid, not POSIX
     * process groups, so accept these as no-ops/identity so the shell's
     * setup doesn't abort. */
    case LX_setpgid: return 0;
    case LX_getpgid:
    case LX_getpgrp:
    case LX_setsid:  return do_syscall(SYS_GETPID, 0, 0, 0, 0, 0);

    /* ---- readlink (B20): backs readlink(/proc/self/exe), realpath, etc. ---- */
    case LX_readlink:                  /* (path, buf, bufsz) */
    case LX_readlinkat: {              /* (dirfd, path, buf, bufsz) */
        int is_at = (n == LX_readlinkat);
        const char *upath = is_at ? (const char *)a2 : (const char *)a1;
        char *ubuf        = is_at ? (char *)a3       : (char *)a2;
        size_t bufsz      = is_at ? (size_t)a4       : (size_t)a3;
        char kpath[ABI_PATH_MAX], ktmp[ABI_PATH_MAX];
        int rr = resolve_user_path(upath, kpath, sizeof kpath);
        if (rr) return rr;
        if (!ubuf || bufsz == 0) return -ABI_EINVAL;
        size_t cap = bufsz < sizeof(ktmp) ? bufsz : sizeof(ktmp);
        /* Slice 86: ktmp used to be left uninitialised and its length taken
         * with a plain strlen(). vfs_readlink's static-table path always
         * terminates, but the per-mount readlink op (procfs /proc/self/exe,
         * and any filesystem-backed link) is free not to -- and then strlen
         * ran off into whatever was on the kernel stack. That is both a
         * kernel-memory disclosure and, worse for the caller, a readlink
         * that returns a length it never actually wrote. A caller doing the
         * near-universal
         *     ssize_t n = readlink(p, buf, sizeof buf); buf[n] = '\0';
         * then writes buf[sizeof buf] -- one byte past the array, which is
         * exactly where -fstack-protector puts the canary. Zero the buffer
         * and bound the length by what we actually have. */
        memset(ktmp, 0, sizeof ktmp);
        int rc = vfs_readlink(kpath, ktmp, cap);
        gputrace("readlink", kpath, rc);
        if (rc != VFS_OK) {
            /* Linux: EINVAL if the path exists but is not a symlink, ENOENT
             * if missing. glibc realpath() requires exactly this split --
             * returning ENOENT for a plain dir/file makes MakeAbsoluteFilePath
             * fail, and chrome then marks --user-data-dir invalid. */
            if (rc == VFS_ERR_NOENT) {
                struct vfs_stat vs;
                if (vfs_stat(kpath, &vs) == VFS_OK)
                    return -ABI_EINVAL;
                return -ABI_ENOENT;
            }
            return -ABI_EINVAL;
        }
        size_t n = 0;
        while (n < cap && ktmp[n]) n++;    /* bounded: never scans past cap */
        if (n == 0) return -ABI_EINVAL;    /* nothing resolved -> not a link */
        if (n > bufsz) n = bufsz;          /* readlink does NOT NUL-terminate */
#ifdef CHROMIUM_BOOT
        { static int rl = 0; if (rl < 40) { rl++;
            kprintf("[rdlink] pid=%d '%s' -> n=%u bufsz=%u\n",
                    current_proc() ? current_proc()->pid : -1, kpath,
                    (unsigned)n, (unsigned)bufsz); } }
#endif
        if (copy_to_user(ubuf, ktmp, n) != 0) return -ABI_EFAULT;
        return (long)n;
    }

    /* Slice 70: sigaltstack(2). Every crash handler on Linux -- crashpad in
     * chrome's case -- installs an alternate signal stack so it can still
     * run when the faulting thread's own stack is the problem, and logs an
     * ERROR when the call fails. We record and report the registration
     * faithfully (so getters see what was set, and SS_DISABLE works) but
     * still deliver signals on the normal stack, which is exactly what
     * -ENOSYS produced before -- minus the error and the risk that a caller
     * treats the failure as fatal. Handlers that genuinely need the
     * separate stack (deep-recursion / stack-overflow reporting) remain
     * unserved; that is a scheduler/signal-delivery change, not an ABI one. */
    case 131: {                        /* sigaltstack(const stack_t*, stack_t*) */
        struct lx_stack { uint64_t ss_sp; int32_t ss_flags; uint32_t _pad;
                          uint64_t ss_size; } cur, in;
        struct proc *sp = current_proc();
        if (!sp) return -ABI_EINVAL;
        cur.ss_sp    = sp->sas_sp;
        cur.ss_size  = sp->sas_size;
        cur.ss_flags = sp->sas_sp ? sp->sas_flags : 2 /* SS_DISABLE */;
        cur._pad     = 0;
        if (a2 && copy_to_user((void *)a2, &cur, sizeof cur) != 0)
            return -ABI_EFAULT;
#ifdef CHROMIUM_BOOT
        { static int ss = 0; if (ss < 16) { ss++;
            kprintf("[sigalt] pid=%d query=%s set=%s\n", sp->pid,
                    a2 ? "yes" : "no", a1 ? "yes" : "no"); } }
#endif
        if (a1) {
            if (copy_from_user(&in, (const void *)a1, sizeof in) != 0)
                return -ABI_EFAULT;
            if (in.ss_flags & 2) {            /* SS_DISABLE */
                sp->sas_sp = 0; sp->sas_size = 0; sp->sas_flags = 0;
            } else {
                if (in.ss_size < 2048) return -ABI_ENOMEM;   /* MINSIGSTKSZ */
                sp->sas_sp    = in.ss_sp;
                sp->sas_size  = in.ss_size;
                sp->sas_flags = in.ss_flags;
            }
        }
        return 0;
    }

    /* Slice 69: symlink(target, linkpath) / symlinkat(target, dirfd,
     * linkpath). Note the argument ORDER -- the TARGET comes first and is
     * an opaque string (it need not exist), while the LINK path is the one
     * that gets created; vfs_symlink takes them the other way round. */
    case LX_symlink:                   /* (target, linkpath) */
    case LX_symlinkat: {               /* (target, newdirfd, linkpath) */
        int is_at = (n == LX_symlinkat);
        const char *utarget = (const char *)a1;
        const char *ulink   = is_at ? (const char *)a3 : (const char *)a2;
        char ktarget[ABI_PATH_MAX], klink[ABI_PATH_MAX];
        if (strncpy_from_user(ktarget, utarget, sizeof ktarget) < 0)
            return -ABI_EFAULT;
        int rr = resolve_user_path(ulink, klink, sizeof klink);
        if (rr) return rr;
        int rc = vfs_symlink(klink, ktarget);
        switch (rc) {
        case VFS_OK:               return 0;
        case VFS_ERR_EXIST:        return -ABI_EEXIST;
        case VFS_ERR_NAMETOOLONG:  return -ABI_ENAMETOOLONG;
        case VFS_ERR_NOSPC:        return -ABI_ENOSPC;
        case VFS_ERR_INVAL:        return -ABI_EINVAL;
        default:                   return -ABI_EIO;
        }
    }

    /* ---- stat family: translate tobyOS vfs_stat -> Linux struct stat ---- */
    case LX_stat:
    case LX_lstat: {                   /* (path, struct stat*) */
        char kpath[ABI_PATH_MAX];
        int rr = resolve_user_path((const char *)a1, kpath, sizeof kpath);
        if (rr) return rr;
        {   /* slice 104: DRM nodes stat as char devices, by path too. */
            int dmin = lx_dev_dri_minor(kpath);
            if (dmin >= 0 && lxdrm_available()) {
                gputrace("stat", kpath, 0);
                return linux_emit_chrdev_stat(DRM_CHR_MAJOR, (uint32_t)dmin,
                                              lx_ino_hash(kpath), (void *)a2);
            }
        }
        struct vfs_stat vs;
        int sr = vfs_stat(kpath, &vs);
        gputrace("stat", kpath, sr);
        if (sr == VFS_ERR_NOENT) return -ABI_ENOENT;
        if (sr != VFS_OK)        return -ABI_EACCES;
        return linux_emit_stat(&vs, lx_ino_hash(kpath), (void *)a2);
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
            /* Slice 103: a DRM fd must report S_IFCHR here too. glibc's
             * fstat() goes through newfstatat(fd,"",AT_EMPTY_PATH), not
             * LX_fstat -- so the slice-99 char-device fix never applied
             * to libdrm, whose drmGetDevice2 checks S_ISCHR FIRST and
             * returns -EINVAL before touching sysfs. MEASURED: the
             * [gpupath] trace showed libdrm making ZERO sysfs accesses,
             * which is only possible if it bailed at this stat. */
            if (f->kind == FILE_KIND_DRM)
                return linux_emit_chrdev_stat(DRM_CHR_MAJOR, f->dir_off,
                                              lx_fd_ino(f, (int)a1),
                                              (void *)a3);
            if (f->kind != FILE_KIND_VFS) { vs.mode = 0666; vs.size = 0; }
            return linux_emit_stat(&vs, lx_fd_ino(f, (int)a1), (void *)a3);
        }
        /* Relative paths resolve against DIRFD -- chrome's sandbox stats
         * "self/task/" through a /proc dirfd (thread_helpers.cc:41). */
        char kpath[ABI_PATH_MAX];
        int rr = resolve_user_path_at((int)a1, upath, kpath, sizeof kpath);
        if (rr) return rr;
        {   /* slice 104: see the LX_stat note. */
            int dmin = lx_dev_dri_minor(kpath);
            if (dmin >= 0 && lxdrm_available()) {
                gputrace("newfstatat", kpath, 0);
                return linux_emit_chrdev_stat(DRM_CHR_MAJOR, (uint32_t)dmin,
                                              lx_ino_hash(kpath), (void *)a3);
            }
        }
        struct vfs_stat vs;
        int sr = vfs_stat(kpath, &vs);
        gputrace("newfstatat", kpath, sr);
        if (sr == VFS_ERR_NOENT) return -ABI_ENOENT;
        if (sr != VFS_OK)        return -ABI_EACCES;
        return linux_emit_stat(&vs, lx_ino_hash(kpath), (void *)a3);
    }
    case LX_fstat: {                   /* (fd, struct stat*) */
        struct file *f = fd_lookup((int)a1);
        if (!f) return -ABI_EBADF;
        { static int n; if (f->kind == FILE_KIND_DRM && n < 12) { n++;
            kprintf("[drmstat] LX_fstat on DRM fd=%d\n", (int)a1); } }
        struct vfs_stat vs;
        if (f->kind == FILE_KIND_DRM)         /* slice 99: S_IFCHR 226:128 */
            return linux_emit_chrdev_stat(DRM_CHR_MAJOR, f->dir_off,
                                          lx_fd_ino(f, (int)a1), (void *)a2);
        if (f->kind == FILE_KIND_DIR && f->dirpath) {
            if (vfs_stat(f->dirpath, &vs) != VFS_OK)
                vs = (struct vfs_stat){ .type = VFS_TYPE_DIR, .mode = 0755 };
        } else if (f->kind == FILE_KIND_VFS) {
            vs = (struct vfs_stat){ .type = VFS_TYPE_FILE, .size = f->vfs.size,
                                    .uid = f->vfs.uid, .gid = f->vfs.gid,
                                    .mode = f->vfs.mode };
        } else if (f->kind == FILE_KIND_MEMFD) {
            /* Chrome fstats the memfd to size its mapping + CHECK st_size. */
            vs = (struct vfs_stat){ .type = VFS_TYPE_FILE,
                                    .size = (size_t)memfd_size(f->memfd),
                                    .mode = 0777 };
        } else {
            /* console/pipe/socket: report a minimal regular-file stat. */
            vs = (struct vfs_stat){ .type = VFS_TYPE_FILE, .size = 0,
                                    .mode = 0666 };
        }
        return linux_emit_stat(&vs, lx_fd_ino(f, (int)a1), (void *)a2);
    }
    case LX_getdents64: {              /* (fd, void *dirp, count) */
        struct file *f = fd_lookup((int)a1);
        if (!f) return -ABI_EBADF;
        if (f->kind != FILE_KIND_DIR || !f->dirpath) return -ABI_ENOTDIR;
        uint8_t *ubuf = (uint8_t *)a2;
        size_t   cap  = (size_t)a3;
        /* Slice 102: /dev/dri is synthetic (no VFS directory behind it),
         * so emit its nodes here. libdrm iterates this to name the
         * device's card/render nodes -- see the open path. */
        if (strcmp(f->dirpath, "/dev/dri") == 0) {
            static const char *const dri_names[] = { "card0", "renderD128" };
            size_t written = 0;
            uint32_t emitted = 0;
            while (f->dir_off + emitted < 2) {
                const char *nm = dri_names[f->dir_off + emitted];
                size_t namelen = strlen(nm);
                size_t reclen = (19 + namelen + 1 + 7) & ~(size_t)7;
                if (written + reclen > cap) break;
                struct lx_dirent64 de;
                memset(&de, 0, reclen);
                de.d_ino    = 100 + f->dir_off + emitted;
                de.d_off    = (int64_t)(f->dir_off + emitted + 1);
                de.d_reclen = (uint16_t)reclen;
                de.d_type   = LX_DT_CHR;
                memcpy(de.d_name, nm, namelen);
                if (copy_to_user(ubuf + written, &de, reclen) != 0)
                    return -ABI_EFAULT;
                written += reclen;
                emitted++;
            }
            f->dir_off += emitted;
            return (long)written;
        }
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

    /* setfsuid/setfsgid: tobyOS has a single (root) identity, so there is no
     * separate fs-uid to change. Per the Linux contract these return the
     * PREVIOUS fs-uid/gid (always 0 here) and never fail; ncurses/nano probe
     * them when writing files. */
    case LX_setfsuid:
    case LX_setfsgid: return 0;

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

    /* prctl(2): per-thread/-process knobs. We don't model most of them, but the
     * common SET options are safe accept-and-ignore no-ops (return 0). Notably
     * PR_SET_VMA (0x53564d41, anon-VMA naming for /proc/maps) -- Chromium/V8 name
     * every cage/arena region; PR_SET_NAME (thread name), PR_SET_PDEATHSIG,
     * PR_SET_DUMPABLE, PR_SET_PTRACER, PR_SET_KEEPCAPS, PR_SET_TIMERSLACK,
     * PR_SET_NO_NEW_PRIVS, PR_SET_CHILD_SUBREAPER. Unknown options fall through
     * to the named -ENOSYS log so real gaps still surface. */
    case LX_prctl:
        switch ((unsigned long)a1) {
        case 0x53564d41:   /* PR_SET_VMA (name an anonymous VMA) */
        case 1:            /* PR_SET_PDEATHSIG */
        case 4:            /* PR_SET_DUMPABLE */
        case 8:            /* PR_SET_KEEPCAPS */
        case 15:           /* PR_SET_NAME */
        case 29:           /* PR_SET_TIMERSLACK */
        case 36:           /* PR_SET_CHILD_SUBREAPER */
        case 38:           /* PR_SET_NO_NEW_PRIVS */
        case 0x59616d61:   /* PR_SET_PTRACER (Yama) */
            return 0;
        case 3:            /* PR_GET_DUMPABLE -> dumpable */
        case 23:           /* PR_CAPBSET_READ -> cap present (we boot as root) */
            return 1;
        case 16: {         /* PR_GET_NAME: copy the 16-byte thread name to arg2 */
            char nm[16];
            memset(nm, 0, sizeof nm);
            const char *pn = current_proc()->name;
            for (int i = 0; i < 15 && pn[i]; i++) nm[i] = pn[i];
            if (a2) copy_to_user((void *)a2, nm, sizeof nm);
            return 0;
        }
        default:
            kprintf("[linux] UNHANDLED prctl option 0x%lx comm=%s -> -ENOSYS\n",
                    (unsigned long)a1, current_proc()->name);
            return -ABI_ENOSYS;
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

    /* B11: libc records its thread-exit futex address here. Store it as the
     * clear_child_tid so proc_exit zeroes it + FUTEX_WAKEs on exit (the
     * pthread_join contract). Returns the tid (libc uses the return value). */
    case LX_set_tid_address: {
        struct proc *p = current_proc();
        if (p) p->clear_child_tid = (uint64_t)a1;
        return p ? p->pid : 0;
    }
    case LX_set_robust_list:
        return 0;

    /* B25: glibc 2.41 startup/pthread/malloc gap-fills (previously -ENOSYS).
     *
     * rseq(): restartable sequences. We don't implement the rseq area, and
     * returning -ENOSYS is the *standard* contract glibc handles -- it simply
     * disables rseq and proceeds. We name the case so it isn't logged as an
     * "unhandled" syscall. */
    case LX_rseq:
        return -ABI_ENOSYS;

    /* madvise(): mostly advisory -- EXCEPT MADV_DONTNEED (4), which on Linux
     * has hard semantics for private anon memory: the pages are dropped and
     * the next touch reads ZEROS. Chrome's PartitionAlloc discards free slot
     * spans with it and its zero-fill (calloc) path skips memset for slots on
     * freshly-committed pages; no-opping DONTNEED handed those allocations
     * back full of dead data (measured: retired SwiftShader framebuffer
     * pixels 0xff000000... inside NSPR's calloc'd locks -> #GP in PR_Unlock,
     * slice 38). MADV_FREE (8) stays a legal no-op (lazy semantics). */
    case LX_madvise:
        if ((int)a3 == 4)                              /* MADV_DONTNEED */
            return sys_madvise_dontneed((uint64_t)a1, (uint64_t)a2);
        return 0;

    case LX_shmget:
        return sys_shmget((int)a1, (size_t)a2, (int)a3);
    case LX_shmat:
        return sys_shmat((int)a1, (uint64_t)a2, (int)a3);
    case LX_shmdt:
        return sys_shmdt((uint64_t)a1);
    case LX_shmctl:
        return sys_shmctl((int)a1, (int)a2, (void *)(uintptr_t)a3);

    /* prlimit64(pid, resource, *new, *old) and the legacy getrlimit/setrlimit.
     * glibc reads RLIMIT_STACK at startup (to size the main+default thread
     * stacks) and RLIMIT_NOFILE elsewhere. We don't enforce limits, so report
     * sane values and accept any set as a no-op. */
    case LX_prlimit64: {
        int      resource = (int)a2;
        uint64_t oldp     = (uint64_t)a4;
        if (oldp) {
            struct { uint64_t cur, max; } rl;
            rl.max = ~0ULL;                 /* RLIM64_INFINITY */
            switch (resource) {
            case 3:  rl.cur = 0x800000ULL; break;          /* RLIMIT_STACK 8 MiB */
            case 7:  rl.cur = PROC_NFDS; rl.max = PROC_NFDS; break; /* RLIMIT_NOFILE (== fd table) */
            default: rl.cur = ~0ULL; break;                /* AS/DATA/...: inf   */
            }
            if (copy_to_user((void *)(uintptr_t)oldp, &rl, sizeof rl) != 0)
                return -ABI_EFAULT;
        }
        return 0;                            /* a3 (new limit) accepted as no-op */
    }
    case LX_getrlimit: {                     /* (resource, *rlim) */
        int      resource = (int)a1;
        uint64_t out      = (uint64_t)a2;
        if (out) {
            struct { uint64_t cur, max; } rl;
            rl.max = ~0ULL;
            switch (resource) {
            case 3:  rl.cur = 0x800000ULL; break;
            case 7:  rl.cur = 1024; rl.max = 4096; break;
            default: rl.cur = ~0ULL; break;
            }
            if (copy_to_user((void *)(uintptr_t)out, &rl, sizeof rl) != 0)
                return -ABI_EFAULT;
        }
        return 0;
    }
    case LX_setrlimit:
        return 0;

    /* clone3(*cl_args, size): the modern thread-spawn entry glibc 2.41 tries
     * FIRST for pthread_create (falling back to clone on -ENOSYS). Translate
     * the cl_args struct to our existing clone-thread path. NOTE clone3 passes
     * the stack BASE + size (top = base+size), unlike legacy clone which passes
     * the stack TOP directly. */
    case LX_clone3: {
        struct lx_clone_args {
            uint64_t flags, pidfd, child_tid, parent_tid, exit_signal,
                     stack, stack_size, tls, set_tid, set_tid_size, cgroup;
        } ca;
        size_t want = (size_t)a2;
        if (want > sizeof ca) want = sizeof ca;
        for (size_t i = 0; i < sizeof ca; i++) ((uint8_t *)&ca)[i] = 0;
        if (copy_from_user(&ca, (const void *)(uintptr_t)a1, want) != 0) {
#ifdef CHROMIUM_BOOT
            /* Slice 76: crashpad's handler child calls clone3 and then exits
             * 0 with NO fork logged either way -- so clone3 bails before
             * reaching the fork/thread split. Name which way it went. */
            { struct proc *cp = current_proc();
              kprintf("[clone3] pid=%d EFAULT reading cl_args=0x%lx size=%lu\n",
                      cp ? cp->pid : -1, (unsigned long)a1,
                      (unsigned long)a2); }
#endif
            return -ABI_EFAULT;
        }
#ifdef CHROMIUM_BOOT
        { static int c3 = 0; if (c3 < 12) { c3++;
            struct proc *cp = current_proc();
            kprintf("[clone3] pid=%d flags=0x%lx size=%lu -> %s\n",
                    cp ? cp->pid : -1, (unsigned long)ca.flags,
                    (unsigned long)a2,
                    (ca.flags & 0x10000u) ? "THREAD" : "FORK"); } }
#endif
        /* Slice 76: CLONE_THREAD, not CLONE_VM -- see the LX_clone case. */
        if (ca.flags & 0x10000u /* CLONE_THREAD */) {
            uint64_t stack_top = ca.stack + ca.stack_size;
            return sys_clone_thread(ca.flags, stack_top, ca.parent_tid,
                                    ca.child_tid, ca.tls);
        }
        if ((ca.flags & 0x4000u /* CLONE_VFORK */) ||
            (ca.flags & 0x100u  /* CLONE_VM */))
            return sys_fork_share();
#ifdef CHROMIUM_BOOT
        {
            struct proc *me = current_proc();
            if (me && me->tgid == 3) {
                static int chrome_c3_cow;
                if (chrome_c3_cow++ >= 32) {
                    kprintf("[fork] deny chrome clone3 CoW #%d pid=%d\n",
                            chrome_c3_cow, me->pid);
                    return -ABI_EAGAIN;
                }
                kprintf("[fork] allow chrome clone3 CoW #%d pid=%d\n",
                        chrome_c3_cow, me->pid);
            }
        }
#endif
        return do_syscall(ABI_SYS_FORK, 0, 0, 0, 0, 0);
    }

    /* B17: busybox wraps each network op in an alarm-timeout via setitimer
     * (arm + disarm). We don't model interval timers, so accept-and-ignore as
     * success (the watched I/O has its own timeouts); getitimer zero-fills its
     * out struct. ftruncate sizes wget's output file -- the content is written
     * by write() regardless, so a no-op success is correct here. */
    case LX_setitimer:
        return 0;
    case LX_getitimer:
        if (a2) { struct { long it[4]; } z; for (int i=0;i<4;i++) z.it[i]=0;
                  (void)copy_to_user((void *)(uintptr_t)a2, &z, sizeof z); }
        return 0;
    case LX_ftruncate: {
        /* Chromium bring-up (slice 8): make ftruncate actually record the file
         * SIZE (was a plain no-op). Chrome's shared-memory Create ftruncates the
         * temp region to N and then relies on fstat reporting N; the no-op left
         * size 0 -> a base/PartitionAlloc CHECK failed -> ImmediateCrash. sys_fstat
         * reports f->vfs.size, which is in-memory and so survives the subsequent
         * unlink. The MAP_SHARED mmap of the region is sized by the mmap len and
         * zero-filled, so no on-disk blocks are needed here. Non-VFS fds keep the
         * old accept-as-success behaviour (wget's alarm-timeout ftruncate). */
        struct file *tf = fd_lookup((int)a1);
        if (tf && tf->kind == FILE_KIND_MEMFD && (long)a2 >= 0) {
#ifdef CHROMIUM_BOOT
            { static int c=0; if(c<48){c++; struct proc *mp = current_proc();
                kprintf("[memfd] pid=%d ftruncate fd=%d size=%lu\n",
                        mp ? mp->pid : -1, (int)a1,(unsigned long)a2);} }
#endif
            return memfd_ftruncate(tf->memfd, (uint64_t)a2);   /* allocates pages */
        }
        if (tf && tf->kind == FILE_KIND_VFS && (long)a2 >= 0)
            tf->vfs.size = (size_t)a2;
        return 0;
    }

    /* Track B/B21: ioctl on the console is a real TTY now. TCGETS/TCSETS
     * (termios), TIOCGWINSZ (size), TIOCGPGRP/TIOCSPGRP (foreground group),
     * TCFLSH/FIONREAD. A working TCGETS is what makes a Linux libc's
     * isatty() return true (-> line-buffered, interactive). ioctl(fd,cmd,arg)
     * = (a1,a2,a3). Non-console fds + unknown requests still get ENOTTY. */
    case LX_ioctl: {
        struct file *f = fd_lookup((int)a1);
        if (!f) return -ABI_EBADF;
        if (f->kind == FILE_KIND_CONSOLE) {
            unsigned long rq = (unsigned long)a2;
            /* KD* (0x4B00) and VT* (0x5600) console ioctls: accept as no-ops so
             * programs that put the VT into graphics mode (e.g. fbsplash) work.
             * Zero-fill the "get" queries (KDGETMODE/KDGKBMODE). */
            if ((rq & 0xFF00u) == 0x4B00u || (rq & 0xFF00u) == 0x5600u) {
                if (rq == 0x4B3Bu /*KDGETMODE*/ || rq == 0x4B44u /*KDGKBMODE*/) {
                    int z = 0;
                    (void)copy_to_user((void *)(uintptr_t)a3, &z, sizeof(z));
                }
                return 0;
            }
            return tty_console_ioctl(rq, (unsigned long)a3);
        }
        if (f->kind == FILE_KIND_PTY_MASTER || f->kind == FILE_KIND_PTY_SLAVE)
            return pty_ioctl(f, (unsigned long)a2, (unsigned long)a3);
        if (f->kind == FILE_KIND_FB)
            return fbdev_ioctl((unsigned long)a2, (unsigned long)a3);
        if (f->kind == FILE_KIND_EVDEV)
            return evdev_ioctl((unsigned)f->dir_off, (unsigned long)a2,
                               (unsigned long)a3);
        if (f->kind == FILE_KIND_DRM)                 /* slice 98: /dev/dri */
            return lxdrm_ioctl(f, (unsigned long)a2, (unsigned long)a3);
        return -ABI_ENOTTY;
    }

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
        struct file *mf = (fd >= 0) ? fd_lookup(fd) : NULL;
        if (mf && mf->kind == FILE_KIND_FB)          /* /dev/fb0 shadow scanout */
            ret = fbdev_mmap((uint64_t)a2);
        else if (mf && mf->kind == FILE_KIND_DRM)    /* slice 98: a GPU BO */
            ret = lxdrm_mmap(lx_mmap_offset(), (uint64_t)a2);
        else if (mf && mf->kind == FILE_KIND_MEMFD) { /* coherent shared memory */
            ret = memfd_map((uint64_t)a1, (uint64_t)a2, (uint32_t)a3,
                            lx_mmap_flags(lf), mf->memfd, lx_mmap_offset());
#ifdef CHROMIUM_BOOT
            { static int c=0; if(c<48){c++; struct proc *mp = current_proc();
                kprintf("[memfd] pid=%d mmap fd=%d len=0x%lx prot=0x%x %s off=0x%lx -> 0x%lx\n",
                        mp ? mp->pid : -1, fd,(unsigned long)a2,(unsigned)a3,
                        (lf & LXMAP_SHARED) ? "SHARED" : "priv",
                        (unsigned long)lx_mmap_offset(),(unsigned long)ret);} }
#endif
        }
        else if ((lf & LXMAP_ANONYMOUS) || fd < 0) { /* anonymous (malloc/TLS) */
            ret = sys_mmap((uint64_t)a1, (uint64_t)a2, (uint32_t)a3,
                           lx_mmap_flags(lf), -1, 0);
#ifdef CHROMIUM_BOOT
            /* [amap] anonymous mmap args+result. Chrome's renderer spins in a
             * tight mmap/munmap loop; this shows what it wants. A hint (a1!=0)
             * that we ignore, or a length whose natural alignment (e.g. 2 MiB
             * PartitionAlloc super-pages) our 4 KiB-granular allocator doesn't
             * satisfy, makes an allocator over-allocate/trim or retry forever.
             * Budget is PER PROCESS (slice 34): one global cap was exhausted
             * by the browser before the RENDERER -- the process that actually
             * spins -- ever ran. */
            {   static unsigned char am[PROC_MAX];
                struct proc *me = current_proc();
                int mp = me ? (me->is_thread ? me->tgid : me->pid) : 0;
                if (mp >= 0 && mp < PROC_MAX && am[mp] < 80) { am[mp]++;
                    kprintf("[amap] pid=%d mmap hint=0x%lx len=0x%lx prot=0x%x fl=0x%x -> 0x%lx\n",
                            me ? me->pid : -1, (unsigned long)a1,
                            (unsigned long)a2, (unsigned)a3,
                            (unsigned)lf, (unsigned long)ret); } }
#endif
        }
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
    case LX_munmap: {
        long mr = do_syscall(ABI_SYS_MUNMAP, a1, a2, 0, 0, 0);
#ifdef CHROMIUM_BOOT
        {   static unsigned char um[PROC_MAX];
            struct proc *me = current_proc();
            int mp = me ? (me->is_thread ? me->tgid : me->pid) : 0;
            if (mp >= 0 && mp < PROC_MAX && um[mp] < 80) { um[mp]++;
                kprintf("[amap] pid=%d munmap  addr=0x%lx len=0x%lx -> %ld\n",
                        me ? me->pid : -1, (unsigned long)a1,
                        (unsigned long)a2, mr); } }
#endif
        return mr;
    }
    case LX_mprotect:
        return do_syscall(ABI_SYS_MPROTECT, a1, a2, a3, 0, 0);
    case LX_mremap:                 /* (old, old_sz, new_sz, flags, new_addr) */
        return linux_mremap((uint64_t)a1, (uint64_t)a2, (uint64_t)a3,
                            (uint32_t)a4);

    /* ---- time ---- */
    case LX_nanosleep: {
        struct lx_timespec ts;
        if (!a1) return -ABI_EINVAL;
        if (copy_from_user(&ts, (const void *)a1, sizeof ts) != 0)
            return -ABI_EFAULT;
        uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        return do_syscall(SYS_NANOSLEEP, (long)ns, 0, 0, 0, 0);
    }
    case 230: {   /* clock_nanosleep(clockid, flags, *req, *rem) */
        /* Chromium bring-up (slice 8 gap-fill): route to nanosleep. Chrome
         * reaches this once past the VMA wall (was -ENOSYS). flags bit0 =
         * TIMER_ABSTIME -> sleep until the absolute deadline; tobyOS clocks are
         * perf_now_ns-based so chrome's absolute time compares directly. clockid
         * (a1) is ignored (MONOTONIC/REALTIME both map to perf_now_ns). */
        struct lx_timespec ts;
        if (!a3) return -ABI_EINVAL;
        if (copy_from_user(&ts, (const void *)a3, sizeof ts) != 0)
            return -ABI_EFAULT;
        uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        uint64_t ns = t;
        if ((int)a2 & 1) {                 /* TIMER_ABSTIME */
            uint64_t now = perf_now_ns();
            ns = (t > now) ? (t - now) : 0;
        }
        return do_syscall(SYS_NANOSLEEP, (long)ns, 0, 0, 0, 0);
    }
    case LX_clock_gettime: {
#ifdef CHROMIUM_BOOT
        /* Spin detector (slice 34): the browser main thread was seen calling
         * clock_gettime tens of thousands of times while every other thread
         * blocked -- a user-space Now() loop we could not identify from the
         * syscall ring alone. One-shot per THREAD: after an absurd number of
         * calls, dump the caller's user stack right here in its own context
         * (safe: own CR3, not IRQ). Threshold high enough that normal
         * time-heavy phases never trip it. */
        {   static uint32_t cgt_count[PROC_MAX];
            static uint8_t  cgt_dumped[PROC_MAX];
            struct proc *me = current_proc();
            if (me && me->pid > 0 && me->pid < PROC_MAX &&
                !cgt_dumped[me->pid] && ++cgt_count[me->pid] > 150000) {
                cgt_dumped[me->pid] = 1;
                extern void bt_dump_self(const char *why);
                bt_dump_self("clock_gettime-storm");
            }
        }
#endif
        /* This used to IGNORE the clock id and return whole MILLISECONDS since
         * boot for every clock. Two bugs in one:
         *   - CLOCK_REALTIME read as ~13 seconds past the epoch, which is why
         *     every chrome log line was stamped "0101/0000ss" (Jan 1 1970), and
         *     why TimeTicks and Time were indistinguishable to it.
         *   - 1 ms granularity while clock_getres advertises 1 ns, so code that
         *     measures sub-millisecond intervals saw ZERO elapsed time between
         *     successive calls. A monotonic clock that reports no progress is a
         *     textbook way to make a deadline loop spin forever -- and
         *     clock_gettime was ~50% of all syscalls in the ring, with chrome's
         *     renderer among the hottest callers.
         * Now: dispatch on the id and use the calibrated-TSC nanosecond source. */
        uint64_t mono = perf_now_ns();
        uint64_t val;
        switch ((int)a1) {
        case 0:  /* CLOCK_REALTIME       */
        case 5:  /* CLOCK_REALTIME_COARSE */
        case 8:  /* CLOCK_REALTIME_ALARM  */
            val = lx_realtime_ns(mono);
            break;
        case 2:  /* CLOCK_PROCESS_CPUTIME_ID */
        case 3:  /* CLOCK_THREAD_CPUTIME_ID  */
            /* Not per-task CPU accounting, but it must still be a clock that
             * only ever moves forward; monotonic is the honest approximation. */
            val = mono;
            break;
        case 1:  /* CLOCK_MONOTONIC       */
        case 4:  /* CLOCK_MONOTONIC_RAW   */
        case 6:  /* CLOCK_MONOTONIC_COARSE*/
        case 7:  /* CLOCK_BOOTTIME        */
        case 9:  /* CLOCK_BOOTTIME_ALARM  */
            val = mono;
            break;
        default:
            return -ABI_EINVAL;   /* unknown clock: fail, don't invent a time */
        }
        struct lx_timespec ts = { (long)(val / 1000000000ull),
                                  (long)(val % 1000000000ull) };
        if (a2 && copy_to_user((void *)a2, &ts, sizeof ts) != 0)
            return -ABI_EFAULT;
        return 0;
    }

    case LX_clock_getres: {
        struct lx_timespec ts = { 0, 1 };    /* 1 ns resolution */
        if (a2 && copy_to_user((void *)a2, &ts, sizeof ts) != 0)
            return -ABI_EFAULT;
        return 0;
    }

    /* ---- thread-pool / process-info fills (Chromium multi-threaded init) ---- */
    case LX_sched_setaffinity:
        return 0;                            /* accept; we don't pin threads */

    case LX_sched_getaffinity: {
        /* (pid, cpusetsize, mask): report all online CPUs so chrome's
         * ThreadPool sizes itself. Return #bytes written (the raw-syscall ABI). */
        uint32_t n = smp_cpu_count();
        if (n == 0) n = 1;
        uint64_t mask = (n >= 64) ? ~0ull : ((1ull << n) - 1);
        size_t len = (size_t)a2;
        if (len == 0) return -ABI_EINVAL;
        if (len > 8) len = 8;
        if (a3 && copy_to_user((void *)a3, &mask, len) != 0)
            return -ABI_EFAULT;
        return (long)len;
    }

    case LX_getpriority:  return 20;         /* nice 0 (kernel returns 20-nice) */
    case LX_setpriority:  return 0;          /* accept */

    case LX_getresuid:                       /* (ruid*, euid*, suid*) -> root */
    case LX_getresgid: {
        unsigned int z = 0;
        if ((a1 && copy_to_user((void *)a1, &z, 4) != 0) ||
            (a2 && copy_to_user((void *)a2, &z, 4) != 0) ||
            (a3 && copy_to_user((void *)a3, &z, 4) != 0))
            return -ABI_EFAULT;
        return 0;
    }

    case LX_sysinfo: {
        struct lx_sysinfo {
            long uptime; unsigned long loads[3];
            unsigned long totalram, freeram, sharedram, bufferram;
            unsigned long totalswap, freeswap;
            unsigned short procs, pad;
            unsigned long totalhigh, freehigh;
            unsigned int mem_unit;
        } si;
        memset(&si, 0, sizeof si);
        long ms = do_syscall(ABI_SYS_CLOCK_MS, 0, 0, 0, 0, 0);
        si.uptime   = (ms > 0) ? ms / 1000 : 0;
        si.totalram = pmm_total_pages();     /* counts are in mem_unit (page) */
        si.freeram  = pmm_free_pages();
        si.procs    = 1;
        si.mem_unit = 4096;
        if (a1 && copy_to_user((void *)a1, &si, sizeof si) != 0)
            return -ABI_EFAULT;
        return 0;
    }

    case LX_statfs:                          /* (path, buf) */
    case LX_fstatfs: {                        /* (fd, buf) -- buf is a2 in both */
        struct lx_statfs {
            long f_type, f_bsize, f_blocks, f_bfree, f_bavail;
            long f_files, f_ffree; unsigned long f_fsid;
            long f_namelen, f_frsize, f_flags, f_spare[4];
        } sf;
        memset(&sf, 0, sizeof sf);
        sf.f_type    = 0x01021994;           /* TMPFS_MAGIC */
        sf.f_bsize   = 4096;
        sf.f_frsize  = 4096;
        sf.f_blocks  = 1L << 20;             /* 4 GiB of 4 KiB blocks */
        sf.f_bfree   = sf.f_bavail = 3L << 18;
        sf.f_files   = 1L << 20;
        sf.f_ffree   = 1L << 19;
        sf.f_namelen = 255;
        if (a2 && copy_to_user((void *)a2, &sf, sizeof sf) != 0)
            return -ABI_EFAULT;
        return 0;
    }

    case LX_statx: {                   /* (dirfd, path, flags, mask, statxbuf) */
        { static int n; struct file *sf = ((int)a1 >= 0) ? fd_lookup((int)a1) : 0;
          if (sf && sf->kind == FILE_KIND_DRM && n < 12) { n++;
            kprintf("[drmstat] LX_statx on DRM fd=%d\n", (int)a1); } }
        const char *upath = (const char *)a2;
        char probe[2] = {0, 0};
        if (upath) (void)strncpy_from_user(probe, upath, sizeof probe);
        if (!upath || probe[0] == '\0') {      /* AT_EMPTY_PATH: statx the fd */
            struct file *f = fd_lookup((int)a1);
            if (!f) return -ABI_EBADF;
            struct vfs_stat vs = { .type = VFS_TYPE_FILE, .size = f->vfs.size,
                                   .uid = f->vfs.uid, .gid = f->vfs.gid,
                                   .mode = f->vfs.mode };
            /* Slice 103: same char-device answer for statx -- glibc may
             * use either. See the newfstatat note above. */
            if (f->kind == FILE_KIND_DRM) {
                struct vfs_stat cvs = { .type = VFS_TYPE_FILE, .size = 0,
                                        .mode = 0666 };
                long src = linux_emit_statx(&cvs, lx_fd_ino(f, (int)a1),
                                            (void *)a5);
                if (src == 0) {
                    /* Patch mode to S_IFCHR and fill rdev in place: the
                     * generic emitter only knows FILE/DIR. */
                    uint16_t m = (uint16_t)(0x2000u | 0666u);
                    uint32_t rmaj = DRM_CHR_MAJOR, rmin = f->dir_off;
                    (void)copy_to_user((uint8_t *)a5 + 0x1c, &m, sizeof m);
                    (void)copy_to_user((uint8_t *)a5 + 0x80, &rmaj, sizeof rmaj);
                    (void)copy_to_user((uint8_t *)a5 + 0x84, &rmin, sizeof rmin);
                }
                return src;
            }
            if (f->kind != FILE_KIND_VFS) { vs.mode = 0666; vs.size = 0; }
            return linux_emit_statx(&vs, lx_fd_ino(f, (int)a1), (void *)a5);
        }
        char kpath[ABI_PATH_MAX];
        int rr = resolve_user_path(upath, kpath, sizeof kpath);
        if (rr) return rr;
        {   /* Slice 104: DRM nodes stat as char devices here too -- and
             * TRACE it. glibc's stat() compiles to statx on modern
             * builds, so this branch was the blind spot that hid
             * libdrm's sysfs probes from [gpupath] entirely. */
            int dmin = lx_dev_dri_minor(kpath);
            if (dmin >= 0 && lxdrm_available()) {
                gputrace("statx", kpath, 0);
                return linux_emit_chrdev_stat(DRM_CHR_MAJOR, (uint32_t)dmin,
                                              lx_ino_hash(kpath), (void *)a5);
            }
        }
        struct vfs_stat vs;
        int sr = vfs_stat(kpath, &vs);
        gputrace("statx", kpath, sr);
        if (sr == VFS_ERR_NOENT) return -ABI_ENOENT;
        if (sr != VFS_OK)        return -ABI_EACCES;
        return linux_emit_statx(&vs, lx_ino_hash(kpath), (void *)a5);
    }

    case LX_time: {                    /* time_t time(time_t *tloc) */
        long secs = (long)(perf_now_ns() / 1000000000ull);
        if (a1 && copy_to_user((void *)a1, &secs, sizeof secs) != 0)
            return -ABI_EFAULT;
        return secs;
    }
    case LX_fallocate: {
        /* SQLite WAL -shm sizing: posix_fallocate(fd, 0, 32KiB) then fstat.
         * A silent success that left vfs.size=0 made History-shm reopen in a
         * tight loop forever (Tier 2.5 headed ozone stall after CreateWindow).
         * Mirror ftruncate: record the end offset as the file size. */
        struct file *af = fd_lookup((int)a1);
        if (!af) return -ABI_EBADF;
        uint64_t off = (uint64_t)a3;
        uint64_t len = (uint64_t)a4;
        uint64_t end = off + len;
        if (af->kind == FILE_KIND_MEMFD)
            return memfd_ftruncate(af->memfd, end);
        if (af->kind == FILE_KIND_VFS && end > (uint64_t)af->vfs.size)
            af->vfs.size = (size_t)end;
        return 0;
    }
    case LX_inotify_add_watch: {
        /* No real file-change events, but hand back a monotonic watch descriptor
         * so callers (fontconfig, dir watching) proceed rather than erroring. */
        static int g_lx_inotify_wd;
        return ++g_lx_inotify_wd;
    }
    case LX_inotify_rm_watch:  return 0;

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
        /* Real entropy from the kernel CSPRNG (RDRAND/device-seeded, see
         * rng.h). This used to ZERO-FILL, which is not merely weak -- it is
         * actively detectable: NSS's freebl seeds its RNG here, runs a
         * continuous health check, sees a constant stream and fails softoken
         * init with CKR_DEVICE_ERROR. That surfaced as chrome's
         * "[FATAL:crypto/nss_util.cc:146] nss_error=-8023" and made every
         * https:// navigation abort before the handshake. Anything doing real
         * crypto (TLS, key generation, ASLR, hash seeds) has the same
         * requirement, so this is a correctness fix well beyond chrome.
         *
         * The 256-byte cap is gone too: Linux getrandom fills the whole
         * request (up to 32 MiB for urandom), and silently returning a short
         * count leaves callers that do not loop with uninitialised tail bytes.
         * We chunk through a stack buffer to bound stack use. */
        uint64_t ubuf = a1;
        size_t   len  = (size_t)a2;
        if (len > SYS_MAX_RW) len = SYS_MAX_RW;
        if (!ubuf || len == 0) return 0;
        uint8_t chunk[256];
        size_t done = 0;
        while (done < len) {
            size_t take = len - done;
            if (take > sizeof chunk) take = sizeof chunk;
            rng_fill(chunk, take);
            if (copy_to_user((void *)(uintptr_t)(ubuf + done), chunk, take) != 0)
                return done ? (long)done : -ABI_EFAULT;
            done += take;
        }
        return (long)done;
    }

    /* ---- signals (B4 + B15): translate the Linux signal ABI onto the native
     * signal layer. musl supplies its own sa_restorer trampoline (which
     * just issues rt_sigreturn); we record it as the proc's restorer so
     * delivery has a return path -- a Linux process never calls the
     * tobyOS SYS_SIGRESTORER. Both 1-arg and SA_SIGINFO 3-arg handlers are
     * supported: delivery writes the siginfo_t/ucontext_t in x86-64 Linux
     * layout for ABI_PERS_LINUX procs (signal.c), and synchronous CPU faults
     * (SIGSEGV/SIGFPE/SIGILL) are delivered to handlers from the fault path. */
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
#ifdef CHROMIUM_BOOT
            /* Slice 84: the browser installs EIGHT handlers with
             * SA_ONSTACK|SA_SIGINFO on Linux (SIGABRT/BUS/FPE/ILL/QUIT/
             * SEGV/SYS/TRAP) after an sigaltstack; on tobyOS it installs ONE
             * and aborts. Log signal + flags so the two sequences can be
             * diffed line-for-line. 0x08000000=SA_ONSTACK, 4=SA_SIGINFO. */
            { static int sa = 0; if (sa < 24) { sa++;
                kprintf("[sigact] pid=%d sig=%d flags=0x%lx handler=%p%s%s\n",
                        p->pid, sig, (unsigned long)na.sa_flags,
                        (void *)(uintptr_t)na.sa_handler,
                        (na.sa_flags & 0x08000000u) ? " SA_ONSTACK" : "",
                        (na.sa_flags & 4u) ? " SA_SIGINFO" : ""); } }
#endif
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

    /* Slice 90: the two signal syscalls chrome's crash path needs. Both were
     * -ENOSYS, which is why a faulting thread could never finish reporting:
     * it sent its crashpad message fine, then died on the very next call and
     * aborted with exit_group(191) instead of raising properly. Filling them
     * does not stop a thread from crashing -- it lets the crash be REPORTED
     * the way Linux reports it, which is how we learn why. */
    case LX_rt_sigtimedwait: {  /* (set, info, timeout, sigsetsize) */
        uint64_t uset = 0;
        if (a1 && copy_from_user(&uset, (const void *)(uintptr_t)a1,
                                 sizeof uset) != 0)
            return -ABI_EFAULT;
        /* CONVENTION MISMATCH -- get this wrong and the wait either never
         * fires or fires on the wrong signal. Linux sigset_t sets bit
         * (signo-1); tobyOS's SIGMASK(s) is (1u << s), i.e. bit == signo
         * (see signal.h and oom.c's `1u << 9` for SIGKILL). Shift the
         * caller's set into OUR numbering once, here. */
        uint64_t kset = uset << 1;
        /* Deadline: NULL timeout means wait forever. */
        bool have_to = (a3 != 0);
        uint64_t deadline = 0;
        if (have_to) {
            int64_t ts[2];
            if (copy_from_user(ts, (const void *)(uintptr_t)a3, sizeof ts) != 0)
                return -ABI_EFAULT;
            deadline = perf_now_ns() + (uint64_t)ts[0] * 1000000000ull
                     + (uint64_t)ts[1];
        }
        struct proc *p = current_proc();
        if (!p) return -ABI_EINVAL;
        /* Crashpad parks a thread here with NO timeout, forever, waiting for
         * a crash signal -- so this wait MUST be cooperative. The first cut
         * spun on sched_yield() while holding the BKL and wedged the whole
         * guest at ~8.7s (log simply stopped): one thread hammering the
         * global lock in an infinite loop starves every other core. Same
         * shape, same fix as sock_unix_recv's blocking path: drop the BKL,
         * yield when this CPU has other runnable work, hlt when it does
         * not, and only re-take the lock on the way out. */
        bool had_bkl = bkl_held();
        if (had_bkl) bkl_exit();
#define RTSTW_RET(v) do { if (had_bkl) bkl_enter(); return (v); } while (0)
        for (;;) {
            /* Accept any pending signal that the caller asked to wait for.
             * Linux DEQUEUES it (it is consumed here, not delivered to a
             * handler), so clear the bit before returning the number. */
            uint64_t hit = p->pending_signals & kset;
            if (hit) {
                int signo = 0;      /* bit index IS the signal number here */
                for (int i = 0; i < 64; i++)
                    if (hit & (1ull << i)) { signo = i; break; }
                p->pending_signals &= ~(1ull << signo);
                if (a2) {   /* siginfo_t: kernel-origin, minimal but valid */
                    uint8_t si[128];
                    memset(si, 0, sizeof si);
                    *(int32_t *)&si[0] = signo;       /* si_signo */
                    *(int32_t *)&si[8] = 0;           /* si_code = SI_USER */
                    (void)copy_to_user((void *)(uintptr_t)a2, si, sizeof si);
                }
                RTSTW_RET(signo);
            }
            if (have_to && perf_now_ns() >= deadline)
                RTSTW_RET(-ABI_EAGAIN);     /* Linux: EAGAIN on timeout */
            if (p->pending_signals)         /* a signal we are NOT waiting for */
                RTSTW_RET(EINTR_RET);
            sti();
            {
                struct percpu *me_cpu = smp_this_cpu();
                if (me_cpu &&
                    __atomic_load_n(&me_cpu->ready_head, __ATOMIC_ACQUIRE))
                    sched_yield();
                else
                    hlt();
            }
        }
#undef RTSTW_RET
    }
    case LX_rt_tgsigqueueinfo:  /* (tgid, tid, sig, uinfo) -- re-raise to self */
        /* The siginfo payload is advisory for our delivery path; what matters
         * is that the signal actually reaches the thread, which is what the
         * crash path is trying to do when it re-raises after reporting. */
        return sys_kill((int)a2, (int)a3);

    /* Futex: forward only the FUTEX_WAIT/WAKE low ops; private flag and
     * timeouts are ignored for now (single-threaded statics don't block
     * here). */
    case LX_futex: { /* (uaddr, op, val, timeout, ...); op decoded inside */
#ifdef CHROMIUM_BOOT
        /* Slice 56: record the REQUESTED wait bound in ms for the [wait] dump.
         * FUTEX_WAIT (op 0) takes a RELATIVE timespec; FUTEX_WAIT_BITSET (op 9)
         * an ABSOLUTE monotonic one -- convert both to "ms from now". A
         * ludicrous value here (minutes+) = the deadline is on the wrong clock. */
        {
            long fto = -1;
            int fop = (int)a2 & 0x7f;
            if ((fop == 0 || fop == 9) && a4) {
                int64_t fts[2];
                if (copy_from_user(fts, (const void *)(uintptr_t)a4,
                                   sizeof fts) == 0) {
                    uint64_t t = (uint64_t)fts[0] * 1000000000ull
                               + (uint64_t)fts[1];
                    if (fop == 9) {
                        uint64_t fnow = perf_now_ns();
                        fto = (t > fnow) ? (long)((t - fnow) / 1000000ull) : 0;
                    } else {
                        fto = (long)(t / 1000000ull);
                    }
                }
            }
            waitt_enter(n, (uint64_t)a1, fto);
        }
#endif
        long fr = futex((uint32_t *)(uintptr_t)a1, (int)a2, (uint32_t)a3,
                        (const void *)(uintptr_t)a4,
                        (uint32_t *)(uintptr_t)a5);
#ifdef CHROMIUM_BOOT
        waitt_exit();
        /* [futex] WAIT/WAKE with ADDRESS and RESULT, so waits can be matched to
         * wakes rather than inferred from durations. Long futex waits alone
         * prove nothing -- chrome parks idle thread-pool workers for many
         * seconds on purpose. The question is whether a WAIT on some address
         * ever receives a WAKE on that SAME address (per-process: these are
         * private futexes, so addresses are only comparable within a pid).
         * op low bits: 0=WAIT 1=WAKE 9=WAIT_BITSET 10=WAKE_BITSET. */
        {   static int fx = 0;
            int op = (int)a2 & 0x7f;      /* strip PRIVATE(128)/CLOCK_REALTIME */
            if (fx < 400 && (op == 0 || op == 1 || op == 9 || op == 10)) {
                fx++;
                struct proc *me = current_proc();
                kprintf("[futex] pid=%d %s addr=0x%lx val=%u -> %ld\n",
                        me ? me->pid : -1,
                        (op == 0 || op == 9) ? "WAIT" : "WAKE",
                        (unsigned long)a1, (unsigned)a3, fr);
            } }
#endif
        return fr;
    }

    case LX_access:                 /* (path, mode) */
        return lx_faccess((const char *)a1);
    case LX_faccessat:              /* (dirfd, path, mode) */
    case LX_faccessat2:             /* (dirfd, path, mode, flags) */
        return lx_faccess((const char *)a2);

    /* Known-optional: libc/busybox probe these and fall back cleanly on
     * -ENOSYS. Handled explicitly (quietly) so they don't spam the log. */
    case LX_fcntl: {            /* (fd, cmd, arg) */
        if ((int)a2 == 3 /* F_GETFL */) {
            /* Return the fd's access mode. Chromium's shared-memory
             * PlatformSharedMemoryRegion::Create dups the region fd and CHECKs
             * fcntl(F_GETFL) reports the expected mode (O_RDWR for a writable
             * region); the old blanket-0 made every fd look O_RDONLY, so the
             * writable-region check failed -> base::ImmediateCrash. Status flags
             * (O_APPEND/O_NONBLOCK) aren't tracked; the access mode is what the
             * check reads. */
            struct file *ff = fd_lookup((int)a1);
            if (!ff) return -ABI_EBADF;
            /* O_NONBLOCK IS tracked for sockets now, and it must read back:
             * the standard way to go non-blocking is F_GETFL, OR the bit in,
             * F_SETFL -- if F_GETFL never reports it, code that checks its own
             * work (or re-derives flags later) sees a blocking socket. */
            if (ff->kind == FILE_KIND_SOCKET && ff->sock && ff->sock->nonblock)
                return ff->o_accmode | SOCK_NONBLOCK;
            return ff->o_accmode;
        }
        if ((int)a2 == 4 /* F_SETFL */) {
            struct file *ff = fd_lookup((int)a1);
            if (!ff) return -ABI_EBADF;
            if (ff->kind == FILE_KIND_SOCKET && ff->sock)
                ff->sock->nonblock = ((unsigned long)a3 & SOCK_NONBLOCK) != 0;
            return 0;
        }
        if ((int)a2 == 1033 /* F_ADD_SEALS */) {
            /* Chrome's Mojo shared-memory channel (channel_linux.cc) seals its
             * memfd then CHECKs F_GET_SEALS reports them -- returning 0 there is
             * a FATAL "Check failed". Store the seals on the memfd. */
            struct file *ff = fd_lookup((int)a1);
            if (!ff || ff->kind != FILE_KIND_MEMFD) return -ABI_EINVAL;
#ifdef CHROMIUM_BOOT
            { static int c=0; if(c<24){c++; kprintf("[memfd] ADD_SEALS fd=%d seals=0x%x\n",(int)a1,(unsigned)a3);} }
#endif
            return memfd_add_seals(ff->memfd, (unsigned int)a3);
        }
        if ((int)a2 == 1034 /* F_GET_SEALS */) {
            struct file *ff = fd_lookup((int)a1);
            if (!ff || ff->kind != FILE_KIND_MEMFD) return -ABI_EINVAL;
#ifdef CHROMIUM_BOOT
            { static int c=0; if(c<24){c++; kprintf("[memfd] GET_SEALS fd=%d -> 0x%lx\n",(int)a1,(unsigned long)memfd_get_seals(ff->memfd));} }
#endif
            return memfd_get_seals(ff->memfd);
        }
        /* Slice 105: F_DUPFD / F_DUPFD_CLOEXEC actually duplicate.
         *
         * These fell into the blanket `return 0` below, so every caller
         * was told "your new descriptor is fd 0". Mesa dups a DRM fd
         * through os_dupfd_cloexec() -- which asks for the lowest fd
         * >= 3 -- and then probed fd 0, i.e. stdin, for a GPU:
         *     "failed to get driver name for fd 0"
         *     "MESA-LOADER: failed to retrieve device information"
         * That is the whole reason the GL stack never reached our device,
         * and it is why libdrm appeared to make no DRM ioctls at all --
         * it was talking to the wrong descriptor entirely.
         *
         * Semantics: allocate the lowest free fd >= arg, sharing the same
         * open file (file_clone, exactly like dup2/dup3). */
        if ((int)a2 == 0 /* F_DUPFD */ || (int)a2 == 1030 /* F_DUPFD_CLOEXEC */) {
            struct file *ff = fd_lookup((int)a1);
            if (!ff) return -ABI_EBADF;
            int minfd = (int)a3;
            if (minfd < 0 || minfd >= PROC_NFDS) return -ABI_EINVAL;
            struct proc *p = current_proc();
            if (!p) return -ABI_EPERM;
            struct file **tab = proc_fds(p);
            if (!tab) return -ABI_EPERM;
            int nfd = -1;
            for (int i = minfd; i < PROC_NFDS; i++)
                if (!tab[i]) { nfd = i; break; }
            if (nfd < 0) return -ABI_EMFILE;
            struct file *cl = file_clone(ff);
            if (!cl) return -ABI_ENOMEM;
            tab[nfd] = cl;
#ifdef CHROMIUM_BOOT
            { static int c = 0; if (c < 24) { c++;
                kprintf("[fcntl] F_DUPFD%s fd=%d min=%d -> %d (kind=%d)\n",
                        ((int)a2 == 1030) ? "_CLOEXEC" : "", (int)a1, minfd,
                        nfd, (int)ff->kind); } }
#endif
            return nfd;
        }
        return 0;               /* F_GETFD/F_SETFD/F_SETFL/CLOEXEC: best-effort no-op */
    }
    case LX_sendfile:           /* cat/cp fall back to a read/write loop */
        return -ABI_ENOSYS;

    default: {
        /* First-hit-detailed, deduped gap-list logger (see lx_scname note).
         * seen[]/hits[] index by number for n<512 (covers all real Linux
         * x86-64 numbers); anything larger always prints. */
        static uint8_t  g_lx_seen[512];
        static uint32_t g_lx_hits[512];
        if (n >= 0 && n < 512) {
            uint32_t c = ++g_lx_hits[n];
            if (!g_lx_seen[n]) {
                g_lx_seen[n] = 1;
                kprintf("[linux] UNHANDLED syscall %ld (%s) "
                        "a=%lx,%lx,%lx,%lx,%lx comm=%s -> -ENOSYS\n",
                        n, lx_scname(n),
                        (unsigned long)a1, (unsigned long)a2, (unsigned long)a3,
                        (unsigned long)a4, (unsigned long)a5,
                        current_proc()->name);
            } else if ((c % 100000u) == 0) {
                kprintf("[linux] UNHANDLED syscall %ld (%s) still spinning "
                        "(%u hits)\n", n, lx_scname(n), c);
            }
        } else {
            kprintf("[linux] UNHANDLED syscall %ld (%s) "
                    "a=%lx,%lx,%lx,%lx,%lx comm=%s -> -ENOSYS\n",
                    n, lx_scname(n),
                    (unsigned long)a1, (unsigned long)a2, (unsigned long)a3,
                    (unsigned long)a4, (unsigned long)a5,
                    current_proc()->name);
        }
        return -ABI_ENOSYS;
    }
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
/* If lpOverlapped is non-NULL, seek to its 64-bit file offset before the I/O.
 * SQLite's Win32 VFS does positioned reads/writes by passing an OVERLAPPED with
 * the offset (Win64 layout: Offset @ +16, OffsetHigh @ +20) on a synchronous
 * handle -- without honouring it, every db write lands at the wrong place. */
static void win32_apply_overlapped(int fd, uint64_t lpov) {
    if (!lpov) return;
    uint32_t lo = 0, hi = 0;
    (void)copy_from_user(&lo, (const void *)(uintptr_t)(lpov + 16), 4);
    (void)copy_from_user(&hi, (const void *)(uintptr_t)(lpov + 20), 4);
    (void)sys_lseek(fd, (int64_t)(((uint64_t)hi << 32) | lo), 0 /*SEEK_SET*/);
}

static long w32_WriteFile(uint64_t *args) {
    int       fd      = (int)args[0];
    uint64_t  ubuf    = args[1];
    uint32_t  n       = (uint32_t)args[2];
    uint64_t  pwrite  = args[3];   /* LPDWORD, may be NULL */

    win32_apply_overlapped(fd, args[4]);   /* C18a: positioned I/O (SQLite VFS) */
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

/* C23: UTF-16 (user) -> narrow (kernel) string, defined later; the printf
 * core uses it for %ls/%S. */
static int win32_wstr_to_k(uint64_t uwide, char *out, int cap);

/* Chunked output sink for the formatter: accumulate then flush. Two modes:
 *   fd >= 0  -> flush to that file descriptor (printf/fprintf/wprintf).
 *   fd <  0  -> CAPTURE into cap[0..cap_max) (sprintf/swprintf); `total`
 *               still counts every char attempted, so the return value is the
 *               would-be length even when the buffer truncates. */
struct win32_fmtbuf {
    int fd; size_t n; long total; char buf[256];
    char  *cap; size_t cap_n, cap_max;
};
static void fb_flush(struct win32_fmtbuf *fb) {
    if (!fb->n) return;
    if (fb->fd >= 0) {
        win32_fd_write(fb->fd, fb->buf, fb->n);
    } else if (fb->cap) {
        for (size_t i = 0; i < fb->n && fb->cap_n < fb->cap_max; i++)
            fb->cap[fb->cap_n++] = fb->buf[i];
    }
    fb->n = 0;
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

/* The printf engine core. Renders a KERNEL format string `fmt` + the user
 * variadic block `uva` into either `fd` (>=0) or a capture buffer `cap`
 * (cap_max bytes, used when fd<0). Returns chars written (the would-be
 * length in capture mode, even if `cap` truncates). */
static long win32_format_core(int fd, char *cap, size_t cap_max,
                              const char *fmt, uint64_t uva) {
    struct win32_fmtbuf fb = { .fd = fd, .n = 0, .total = 0,
                               .cap = cap, .cap_n = 0, .cap_max = cap_max };
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
        bool wide = false;       /* 64-bit integer conversion */
        bool lmod = false;       /* 'l' length modifier seen (-> wide str/char) */
        bool hmod = false;       /* 'h' length modifier seen (-> narrow str/char) */
        if (*p == 'l') { lmod = true; p++; if (*p == 'l') { wide = true; p++; } }
        else if (*p == 'h') { hmod = true; p++; if (*p == 'h') p++; }
        else if (*p == 'L') { p++; }            /* long double == double on Windows */
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
        case 'C':                 /* C23: %C == wide char in a narrow printf */
        case 'c': {
            /* Wide when the conversion is %C, or %lc. The arg occupies one
             * va slot either way; for a wide char we down-convert U+0000..7F
             * to ASCII (others -> '?'), matching our UTF-16<->ASCII policy. */
            bool wch = (conv == 'C') || lmod;
            uint64_t raw = va_next(uva, &ai);
            char ch = wch ? ((raw < 0x80) ? (char)raw : '?') : (char)raw;
            emit_field(&fb, pre, 0, &ch, 1, width, left, false);
            break;
        }
        case 'S':                 /* C23: %S == wide string in a narrow printf */
        case 's': {
            /* Wide when the conversion is %S, or %ls (and not %hs). */
            bool wstr = (conv == 'S') || (lmod && !hmod);
            uint64_t sp = va_next(uva, &ai);
            char sbuf[1024];
            int sl = 0;
            if (sp) {
                if (wstr) {
                    sl = win32_wstr_to_k(sp, sbuf, sizeof(sbuf));
                } else {
                    long n = strncpy_from_user(sbuf, (const char *)(uintptr_t)sp, sizeof(sbuf));
                    if (n < 0) { const char *bad = "(badptr)"; fb_write(&fb, bad, 8); break; }
                    sl = (n >= (long)sizeof(sbuf)) ? (int)sizeof(sbuf) - 1 : (int)n;
                }
            } else {
                const char *nul = "(null)";
                emit_field(&fb, pre, 0, nul, 6, width, left, false);
                break;
            }
            if (prec >= 0 && prec < sl) sl = prec;
            emit_field(&fb, pre, 0, sbuf, sl, width, left, false);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            /* C21: float printing. The double arrives as its 8-byte bit pattern
             * in the next va_list slot (MS-x64 spills variadic doubles to the
             * stack, which is what the C2 gate marshals). The double->decimal
             * conversion needs FP, so it runs in the -msse kmath unit; we get
             * back the magnitude body + sign and reuse emit_field for padding. */
            uint64_t bits = va_next(uva, &ai);
            unsigned ff = (plus ? KMF_PLUS : 0u) | (space ? KMF_SPACE : 0u) | (alt ? KMF_HASH : 0u);
            char body[256]; int sgn = 0, special = 0;
            int bl = kmath_fmt_double(bits, conv, prec, ff, body, sizeof(body), &sgn, &special);
            if (sgn) pre[pl++] = (char)sgn;
            emit_field(&fb, pre, pl, body, bl, width, left, zero && !special);
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

/* The classic narrow printf engine: copy the user format string into the
 * kernel, then run the core writing to `fd`. Returns chars written. */
static long win32_vformat(int fd, uint64_t ufmt, uint64_t uva) {
    char fmt[1024];
    long fl = strncpy_from_user(fmt, (const char *)(uintptr_t)ufmt, sizeof(fmt));
    if (fl < 0) return -1;
    fmt[sizeof(fmt) - 1] = '\0';
    return win32_format_core(fd, 0, 0, fmt, uva);
}

/* C23: narrow sprintf core -> capture into a kernel buffer (then the caller
 * copies it out, narrow or wide). cap_max counts bytes incl. the NUL we add. */
static long win32_sformat(char *cap, size_t cap_max, uint64_t ufmt, uint64_t uva) {
    char fmt[1024];
    long fl = strncpy_from_user(fmt, (const char *)(uintptr_t)ufmt, sizeof(fmt));
    if (fl < 0) return -1;
    fmt[sizeof(fmt) - 1] = '\0';
    if (cap_max == 0) return win32_format_core(-1, 0, 0, fmt, uva); /* length probe */
    long t = win32_format_core(-1, cap, cap_max - 1, fmt, uva);
    size_t end = (size_t)t < cap_max - 1 ? (size_t)t : cap_max - 1;
    cap[end] = '\0';
    return t;
}

/* C23: wide printf engine. The wide CRT format string is UTF-16; transcode it
 * to a narrow format string, rewriting the string/char conversions to their
 * wide form so win32_format_core reads the args correctly:
 *   %s / %c  (default WIDE in the wide CRT) -> %ls / %lc
 *   %hs/ %hc (explicit narrow)              -> %s  / %c
 *   %S / %C  (explicit narrow in wide CRT)  -> %s  / %c
 * Everything else (flags/width/precision/numeric conversions) is copied
 * verbatim. The args themselves are unchanged -- only how the core reads %s. */
static int win32_wfmt_transcode(uint64_t uwfmt, char *out, int cap) {
    int oi = 0;
    for (uint64_t wi = 0; oi < cap - 1; wi++) {
        uint16_t wc = 0;
        if (copy_from_user(&wc, (const void *)(uintptr_t)(uwfmt + wi * 2), 2) != 0) break;
        if (wc == 0) break;
        char c = (wc < 0x80) ? (char)wc : '?';
        if (c != '%') { out[oi++] = c; continue; }
        out[oi++] = '%';
        /* copy the rest of the spec, tracking h/l, until the conversion char */
        bool hmod = false, lmod = false;
        for (;;) {
            uint16_t nw = 0;
            if (copy_from_user(&nw, (const void *)(uintptr_t)(uwfmt + (++wi) * 2), 2) != 0) { out[oi] = 0; return oi; }
            if (nw == 0) { out[oi] = 0; return oi; }
            char nc = (nw < 0x80) ? (char)nw : '?';
            if (nc == 'h') { hmod = true; if (oi < cap - 1) out[oi++] = nc; continue; }
            if (nc == 'l' || nc == 'L' || nc == 'w') { lmod = true; if (oi < cap - 1) out[oi++] = nc; continue; }
            if (nc == 'j' || nc == 'z' || nc == 't' || nc == 'I' ||
                nc == '0' || (nc >= '1' && nc <= '9') || nc == '.' || nc == '*' ||
                nc == '-' || nc == '+' || nc == ' ' || nc == '#') {
                if (oi < cap - 1) out[oi++] = nc; continue;
            }
            /* conversion char */
            if (nc == 's' || nc == 'c') {
                if (!hmod && !lmod && oi < cap - 1) out[oi++] = 'l';  /* default wide */
                if (oi < cap - 1) out[oi++] = nc;
            } else if (nc == 'S' || nc == 'C') {
                if (oi < cap - 1) out[oi++] = (nc == 'S') ? 's' : 'c';  /* narrow */
            } else {
                if (oi < cap - 1) out[oi++] = nc;
            }
            break;
        }
    }
    out[oi] = 0;
    return oi;
}

/* wprintf/fwprintf family -> fd. */
static long win32_wvformat(int fd, uint64_t uwfmt, uint64_t uva) {
    char fmt[1024];
    win32_wfmt_transcode(uwfmt, fmt, sizeof(fmt));
    return win32_format_core(fd, 0, 0, fmt, uva);
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
 * C23: wide (UTF-16) stdio. The wprintf family funnels through ucrt's
 * __stdio_common_vfwprintf (-> fd) and __stdio_common_vswprintf (-> wide
 * buffer); fputws/_putws/fputwc/putwchar emit a wide string/char. All
 * down-convert U+0000..7F to ASCII (others '?'), matching the kernel's
 * UTF-16<->ASCII policy used everywhere else in the Win32 layer.
 * ============================================================ */

/* __stdio_common_vfwprintf(opts, FILE* stream, const wchar_t* fmt, loc, va). */
static long w32_stdio_common_vfwprintf(uint64_t *args) {
    uint64_t stream = args[1];
    uint64_t uwfmt  = args[2];
    uint64_t uva    = args[4];
    int fd = 1;
    if ((stream & 0xFFFFFF00ULL) == WIN32_IOB_TAG) fd = (int)(stream & 0xFF);
    if (!uwfmt) return -1;
    return win32_wvformat(fd, uwfmt, uva);
}

/* __stdio_common_vswprintf(opts, wchar_t* buf, size_t count, const wchar_t* fmt,
 *     loc, va). Renders into a UTF-16 buffer of `count` code units (incl NUL).
 * Returns code units written excl. NUL, or -1 if it didn't fit (ucrt-ish). */
static long w32_stdio_common_vswprintf(uint64_t *args) {
    uint64_t ubuf  = args[1];
    uint64_t count = args[2];
    uint64_t uwfmt = args[3];
    uint64_t uva   = args[5];
    char fmt[1024];
    win32_wfmt_transcode(uwfmt, fmt, sizeof(fmt));
    char tmp[1024];
    long t = win32_format_core(-1, tmp, sizeof(tmp) - 1, fmt, uva);
    size_t n = ((size_t)t < sizeof(tmp) - 1) ? (size_t)t : sizeof(tmp) - 1;
    if (ubuf && count > 0) {
        size_t maxc = (size_t)count - 1;          /* leave room for NUL */
        size_t w = n < maxc ? n : maxc;
        for (size_t i = 0; i < w; i++) {
            uint16_t wc = (uint16_t)(uint8_t)tmp[i];
            (void)copy_to_user((void *)(uintptr_t)(ubuf + i * 2), &wc, 2);
        }
        uint16_t z = 0;
        (void)copy_to_user((void *)(uintptr_t)(ubuf + w * 2), &z, 2);
    }
    return t;
}

/* fputws(const wchar_t* ws, FILE* stream) -> >=0 / WEOF. */
static long w32_fputws(uint64_t *a) {
    uint64_t stream = a[1];
    int fd = 1;
    if ((stream & 0xFFFFFF00ULL) == WIN32_IOB_TAG) fd = (int)(stream & 0xFF);
    char s[1024];
    int n = win32_wstr_to_k(a[0], s, sizeof(s));
    win32_fd_write(fd, s, (size_t)n);
    return n;
}
/* _putws(const wchar_t* ws): fputws to stdout + newline. */
static long w32_putws(uint64_t *a) {
    char s[1024];
    int n = win32_wstr_to_k(a[0], s, sizeof(s));
    win32_fd_write(1, s, (size_t)n);
    win32_fd_write(1, "\n", 1);
    return n + 1;
}
/* fputwc(wchar_t c, FILE* stream) / putwchar(wchar_t c). */
static long w32_fputwc(uint64_t *a) {
    uint64_t wc = a[0]; uint64_t stream = a[1];
    int fd = 1;
    if ((stream & 0xFFFFFF00ULL) == WIN32_IOB_TAG) fd = (int)(stream & 0xFF);
    char c = (wc < 0x80) ? (char)wc : '?';
    win32_fd_write(fd, &c, 1);
    return (long)(uint16_t)wc;
}
static long w32_putwchar(uint64_t *a) {
    char c = (a[0] < 0x80) ? (char)a[0] : '?';
    win32_fd_write(1, &c, 1);
    return (long)(uint16_t)a[0];
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
/* Copy n bytes between two USER addresses in the current address space (bounces
 * through a small kernel buffer; copy_to/from_user can't go user->user). Used by
 * C18b per-thread TLS to clone the .tls template into each thread's block. */
static void win32_user_memcpy(uint64_t dst, uint64_t src, uint64_t n) {
    uint8_t buf[256];
    while (n) {
        uint64_t c = n < sizeof(buf) ? n : sizeof(buf);
        if (copy_from_user(buf, (const void *)(uintptr_t)src, c) != 0) break;
        if (copy_to_user((void *)(uintptr_t)dst, buf, c) != 0) break;
        src += c; dst += c; n -= c;
    }
}
/* Size-tracked allocation over the bump arena: store the byte count in a 16-byte
 * header (keeps the returned pointer 16-aligned) so realloc / HeapSize / _msize
 * can recover it. free() stays a no-op (the arena never reclaims). C18a: SQLite
 * + the ucrt heap need realloc, which the C1-C17 bump allocator could not do. */
static uint64_t win32_malloc_hdr(uint64_t n) {
    uint64_t base = win32_heap_alloc(n + 16);
    if (!base) return 0;
    uint64_t sz = n;
    (void)copy_to_user((void *)(uintptr_t)base, &sz, 8);
    return base + 16;
}
static uint64_t win32_msize(uint64_t uptr) {
    if (!uptr) return 0;
    uint64_t sz = 0;
    if (copy_from_user(&sz, (const void *)(uintptr_t)(uptr - 16), 8) != 0) return 0;
    return sz;
}
static long w32_malloc(uint64_t *a) { return (long)win32_malloc_hdr(a[0]); }
static long w32_calloc(uint64_t *a) {
    uint64_t nmemb = a[0], sz = a[1], total = nmemb * sz;
    if (sz && total / sz != nmemb) return 0;       /* overflow */
    return (long)win32_malloc_hdr(total);          /* mmap memory is zeroed */
}
/* realloc(ptr, n): NULL -> malloc; else fresh block + copy min(old, n). The bump
 * arena never frees, so the old block just leaks. */
static long w32_realloc(uint64_t *a) {
    uint64_t old = a[0], n = a[1];
    if (!old) return (long)win32_malloc_hdr(n);
    uint64_t np = win32_malloc_hdr(n);
    if (!np) return 0;
    uint64_t oldsz = win32_msize(old);
    uint64_t cp = oldsz < n ? oldsz : n;
    /* user->user copy in chunks via a kernel bounce */
    uint8_t buf[1024];
    for (uint64_t off = 0; off < cp; ) {
        uint64_t chunk = cp - off; if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (copy_from_user(buf, (const void *)(uintptr_t)(old + off), chunk) != 0) break;
        if (copy_to_user((void *)(uintptr_t)(np + off), buf, chunk) != 0) break;
        off += chunk;
    }
    return (long)np;
}
static long w32_msize(uint64_t *a) { return (long)win32_msize(a[0]); }

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

/* strstr(haystack, needle) -> a USER pointer into haystack at the first match,
 * or NULL. Searches within the copied prefix (cap 1024); the returned pointer
 * is haystack + offset so it stays valid for the caller. */
static long w32_strstr(uint64_t *a) {
    if (!a[0]) return 0;
    char hay[1024], nee[128];
    hay[0] = 0; nee[0] = 0;
    if (strncpy_from_user(hay, (const char *)(uintptr_t)a[0], sizeof(hay)) < 0) return 0;
    if (!a[1]) return (long)a[0];
    (void)strncpy_from_user(nee, (const char *)(uintptr_t)a[1], sizeof(nee));
    if (!nee[0]) return (long)a[0];                 /* empty needle -> haystack */
    int nlen = 0; while (nee[nlen]) nlen++;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (j < nlen && hay[i + j] && hay[i + j] == nee[j]) j++;
        if (j == nlen) return (long)(a[0] + (uint64_t)i);
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

/* ---- C23: wide (UTF-16) string CRT. All operate directly on user UTF-16
 * buffers via copy_{from,to}_user, comparing/copying 16-bit code units. ---- */
static inline uint16_t wld(uint64_t p, uint64_t i) {           /* load wchar[i] */
    uint16_t w = 0; (void)copy_from_user(&w, (const void *)(uintptr_t)(p + i * 2), 2); return w;
}
static inline void wst(uint64_t p, uint64_t i, uint16_t w) {   /* store wchar[i] */
    (void)copy_to_user((void *)(uintptr_t)(p + i * 2), &w, 2);
}
/* wcscmp(a,b) / wcsncmp(a,b,n): lexicographic compare of code units. */
static long w32_wcscmp(uint64_t *a) {
    for (uint64_t i = 0; i < 65536; i++) {
        uint16_t x = wld(a[0], i), y = wld(a[1], i);
        if (x != y) return (x < y) ? -1 : 1;
        if (x == 0) return 0;
    }
    return 0;
}
static long w32_wcsncmp(uint64_t *a) {
    uint64_t n = a[2];
    for (uint64_t i = 0; i < n; i++) {
        uint16_t x = wld(a[0], i), y = wld(a[1], i);
        if (x != y) return (x < y) ? -1 : 1;
        if (x == 0) return 0;
    }
    return 0;
}
/* wcscpy(dst,src) -> dst. */
static long w32_wcscpy(uint64_t *a) {
    uint64_t i = 0;
    for (; i < 65536; i++) { uint16_t w = wld(a[1], i); wst(a[0], i, w); if (w == 0) break; }
    return (long)a[0];
}
/* wcsncpy(dst,src,n) -> dst (pads with NULs, no NUL-term if src longer). */
static long w32_wcsncpy(uint64_t *a) {
    uint64_t n = a[2]; bool end = false;
    for (uint64_t i = 0; i < n; i++) {
        uint16_t w = end ? 0 : wld(a[1], i);
        if (w == 0) end = true;
        wst(a[0], i, w);
    }
    return (long)a[0];
}
/* wcscat(dst,src) -> dst. */
static long w32_wcscat(uint64_t *a) {
    uint64_t d = 0; while (d < 65536 && wld(a[0], d) != 0) d++;
    for (uint64_t i = 0; i < 65536; i++) { uint16_t w = wld(a[1], i); wst(a[0], d + i, w); if (w == 0) break; }
    return (long)a[0];
}
/* wcschr(s,c) / wcsrchr(s,c) -> pointer to match or NULL. */
static long w32_wcschr(uint64_t *a) {
    uint16_t c = (uint16_t)a[1];
    for (uint64_t i = 0; i < 65536; i++) {
        uint16_t w = wld(a[0], i);
        if (w == c) return (long)(a[0] + i * 2);
        if (w == 0) return 0;
    }
    return 0;
}
static long w32_wcsrchr(uint64_t *a) {
    uint16_t c = (uint16_t)a[1]; long hit = 0;
    for (uint64_t i = 0; i < 65536; i++) {
        uint16_t w = wld(a[0], i);
        if (w == c) hit = (long)(a[0] + i * 2);
        if (w == 0) break;
    }
    return hit;
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
    win32_apply_overlapped(fd, a[4]);   /* C18a: positioned I/O (SQLite VFS) */
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

/* ================= C16e: per-thread TEBs + TLS =================
 * Each CreateThread thread gets its OWN TEB (so gs:[0x30] / NtCurrentTeb is
 * per-thread) carrying its own TLS slot array; TlsAlloc/Free/SetValue/GetValue
 * operate on the calling thread's TEB (current_proc()->gs_base). */
#define WIN32_TEB_SELF_OFF       0x30
#define WIN32_TEB_STACKBASE_OFF  0x08
#define WIN32_TEB_STACKLIMIT_OFF 0x10
#define WIN32_TEB_TLS_OFF        0x80    /* TLS slot array within our 1-page TEB */
#define WIN32_TEB_TLSPTR_OFF     0x58    /* C18b: x64 ThreadLocalStoragePointer  */
#define WIN32_TLS_ARR_BYTES      0x80    /* C18b: per-thread TLS pointer array    */
#define WIN32_TLS_MAX            64
static uint8_t g_win32_tls_used[WIN32_TLS_MAX];

static void win32_reset_tls(void) { memset(g_win32_tls_used, 0, sizeof(g_win32_tls_used)); }

/* kernel32!TlsAlloc() -> a free index, or TLS_OUT_OF_INDEXES (0xFFFFFFFF). */
static long w32_TlsAlloc(uint64_t *a) {
    (void)a;
    for (int i = 0; i < WIN32_TLS_MAX; i++)
        if (!g_win32_tls_used[i]) { g_win32_tls_used[i] = 1; return i; }
    return (long)0xFFFFFFFF;
}
static long w32_TlsFree(uint64_t *a) {
    int i = (int)(uint32_t)a[0];
    if (i >= 0 && i < WIN32_TLS_MAX) g_win32_tls_used[i] = 0;
    return 1;
}
/* TlsSetValue(idx, val) -> TRUE: write to the calling thread's TEB slot. */
static long w32_TlsSetValue(uint64_t *a) {
    uint32_t i = (uint32_t)a[0];
    struct proc *p = current_proc();
    if (i >= WIN32_TLS_MAX || !p || !p->gs_base) return 0;
    uint64_t v = a[1];
    (void)copy_to_user((void *)(uintptr_t)(p->gs_base + WIN32_TEB_TLS_OFF + (uint64_t)i * 8), &v, 8);
    return 1;
}
/* TlsGetValue(idx) -> the calling thread's TEB slot value (0 if unset). */
static long w32_TlsGetValue(uint64_t *a) {
    uint32_t i = (uint32_t)a[0];
    struct proc *p = current_proc();
    if (i >= WIN32_TLS_MAX || !p || !p->gs_base) return 0;
    uint64_t v = 0;
    (void)copy_from_user(&v, (const void *)(uintptr_t)(p->gs_base + WIN32_TEB_TLS_OFF + (uint64_t)i * 8), 8);
    return (long)v;
}

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
    /* C18b: the .tls template lives on the thread-group leader (the main proc). */
    struct proc *lead = (leader && leader->is_thread) ? proc_lookup(leader->tgid) : leader;
    int tid = thread_create(WIN32_THREAD_WRAPPER_VA, block, stack_top, 0);
    if (tid < 0) return 0;

    /* The new thread must carry the Win32 personality (so its gate calls route
     * to the dispatcher) and share the TEB. Safe here: we hold the BKL, so the
     * new thread can't run until this syscall yields/returns. */
    struct proc *t = proc_lookup(tid);
    if (t) {
        t->personality = ABI_PERS_WIN32;
        /* C16e: give the thread its OWN TEB (per-thread NtCurrentTeb + TLS).
         * The TEB lives in the shared address space (win32_heap_alloc), so it's
         * reachable via the thread's gs_base once do_switch loads it. */
        uint64_t teb = win32_heap_alloc(0x400);
        if (teb) {
            uint64_t self = teb, sb = stack_top, sl = stack;
            (void)copy_to_user((void *)(uintptr_t)(teb + WIN32_TEB_SELF_OFF),       &self, 8);
            (void)copy_to_user((void *)(uintptr_t)(teb + WIN32_TEB_STACKBASE_OFF),  &sb,   8);
            (void)copy_to_user((void *)(uintptr_t)(teb + WIN32_TEB_STACKLIMIT_OFF), &sl,   8);
            t->gs_base = teb;
            /* C18b: give this thread its OWN static-TLS block + pointer array and
             * point its TEB's gs:[0x58] at the array, so every _Thread_local is
             * per-thread (the template's initial values, isolated per thread). */
            if (lead && lead->win_tls_total) {
                uint64_t arr = win32_heap_alloc(WIN32_TLS_ARR_BYTES);
                uint64_t blk = win32_heap_alloc(lead->win_tls_total);
                if (arr && blk) {
                    /* heap is fresh ANON (zeroed): only copy the template raw. */
                    win32_user_memcpy(blk, lead->win_tls_raw_va, lead->win_tls_raw_size);
                    (void)copy_to_user((void *)(uintptr_t)(arr + lead->win_tls_index * 8), &blk, 8);
                    (void)copy_to_user((void *)(uintptr_t)(teb + WIN32_TEB_TLSPTR_OFF), &arr, 8);
                }
            }
        } else {
            t->gs_base = leader ? leader->gs_base : 0;   /* fallback: share */
        }
    }

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
 * + win32_user_stub_va in pe_loader.c.
 *
 * C10: MULTIPLE top-level windows. A per-window table (keyed by HWND == the
 * tobyOS fd) holds each window's size/WndProc/paint+destroy flags/fill colour;
 * a class registry maps a class name -> its WndProc; CreateWindow associates a
 * window with its class's WndProc. GetMessage scans all windows and -- crucially
 * -- stashes the message's target WndProc into the CRT slot the C7 trampoline
 * reads, so DispatchMessage calls the RIGHT window's WndProc with no asm change
 * (GetMessage and DispatchMessage are called in lock-step per message). */
#define WIN32_MAX_WINDOWS 8
#define WIN32_MAX_CLASSES 8
#define WIN32_MAX_CTRLS   16   /* C12: BUTTON/EDIT child controls */
#define WIN32_CMDQ        8    /* C12: pending WM_COMMAND ring     */
#define WIN32_CTRL_TEXT   40

/* C12 handle tags (high byte) -- distinct from brush 0x7B / pen 0x7C / module
 * 0x7D / find 0x7E / thread 0x74 / FILE* 0xF11E. */
#define WIN32_BITMAP_TAG  0x6200000000000000ull   /* HBITMAP -> g_win32_bmp slot  */
#define WIN32_MEMDC_TAG   0x6300000000000000ull   /* memory HDC -> g_win32_memdc  */
#define WIN32_CTRL_TAG    0x6400000000000000ull   /* control HWND -> g_win32_ctrl */
#define WIN32_FONT_TAG    0x6500000000000000ull   /* HFONT: px in bits[0:15], face in bits[16:17] (C18c) */
#define WIN32_FONT_FACE_SHIFT 16
#define WIN32_FONT_FACE_MASK  0x3u
#define WIN32_TAG_MASK    0xFF00000000000000ull
/* Control kinds (C12 BUTTON/EDIT; C13 STATIC/CHECKBOX/LISTBOX/SCROLLBAR;
 * C14 COMBOBOX/RADIO/GROUPBOX/TAB). */
#define WIN32_CTRL_BUTTON    1
#define WIN32_CTRL_EDIT      2
#define WIN32_CTRL_STATIC    3
#define WIN32_CTRL_CHECKBOX  4
#define WIN32_CTRL_LISTBOX   5
#define WIN32_CTRL_SCROLLBAR 6
#define WIN32_CTRL_COMBOBOX  7   /* C14: dropdown edit+list             */
#define WIN32_CTRL_RADIO     8   /* C14: mutually-exclusive radio button */
#define WIN32_CTRL_GROUPBOX  9   /* C14: a labelled frame (visual only)  */
#define WIN32_CTRL_TAB       10  /* C14: SysTabControl32 tab strip       */
/* C25: comctl32 common controls (real comctl32 window classes). These use the
 * dedicated g_win32_cc[] store (rich enough for columns/rows/tree nodes/parts)
 * rather than the small g_win32_lb[] item store the C14 controls share. */
#define WIN32_CTRL_LISTVIEW  11  /* C25: SysListView32 (report view)        */
#define WIN32_CTRL_TREEVIEW  12  /* C25: SysTreeView32                      */
#define WIN32_CTRL_STATUSBAR 13  /* C25: msctls_statusbar32                 */
#define WIN32_CTRL_PROGRESS  14  /* C25: msctls_progress32                  */
#define WIN32_CTRL_TOOLBAR   15  /* C25: ToolbarWindow32                    */
#define WIN32_LB_MAX_ITEMS   8
#define WIN32_LB_ITEM_LEN    24

/* A control (BUTTON/EDIT/STATIC/CHECKBOX/LISTBOX/SCROLLBAR) is a VIRTUAL child
 * rendered INTO its parent window's client area (the compositor is top-level-
 * only -- no nested windows), with input routed from the parent. The kernel
 * draws it and handles its input; the app sees only WM_COMMAND/WM_VSCROLL. */
struct win32_ctrl {
    bool     in_use;
    int      parent_fd;          /* the owning top-level window (fd) */
    int      type;               /* WIN32_CTRL_* */
    int      x, y, w, h;         /* rect within the parent's client area */
    int      id;                 /* control id (CreateWindowEx hMenu) */
    bool     focus;              /* EDIT: has keyboard focus */
    bool     checked;            /* CHECKBOX / RADIO state (C13/C14) */
    int      sb_min, sb_max, sb_pos;  /* SCROLLBAR range/pos (C13) */
    bool     dropped;            /* C14 COMBOBOX: dropdown list is open */
    int      group;              /* C14 RADIO: group id (WS_GROUP runs)  */
    bool     hidden;             /* C14: ShowWindow(SW_HIDE) (TAB pages) */
    char     text[WIN32_CTRL_TEXT];
};
/* C13 LISTBOX item store, indexed by control slot. */
struct win32_lb { char items[WIN32_LB_MAX_ITEMS][WIN32_LB_ITEM_LEN]; int n, sel; };

struct win32_win {
    int      fd;          /* sys_gui_create fd == HWND == HDC; 0 = free slot */
    struct window *gwin;  /* C18c: the compositor window (for harness pixel readback) */
    int      w, h;        /* client size */
    uint64_t wndproc;     /* this window's WndProc (from its class) */
    bool     needs_paint; /* a WM_PAINT is queued */
    bool     destroy;     /* DestroyWindow seen -> deliver WM_DESTROY then retire */
    uint32_t fill_color;  /* last FillRect colour (TextOut bg tracks it) */
    /* C11 GDI device-context state (per window / HDC). */
    int      cur_x, cur_y;/* current position for MoveToEx/LineTo */
    uint32_t pen_color;   /* currently-selected pen colour (XRGB) */
    /* C11 child/owned windows: the parent/owner HWND (fd), or 0 if top-level.
     * Destroying the parent retires its children too. */
    int      parent_fd;
    /* C13 GDI text state (per window / HDC). */
    int      font_scale;  /* bitmap-font scale from SelectObject(CreateFont) */
    int      font_px;     /* C14b: TTF pixel height (0 = default 16)         */
    int      font_face;   /* C18c: KFONT_REGULAR/BOLD/ITALIC/BOLDITALIC      */
    uint32_t text_color;  /* SetTextColor (XRGB); default white */
    int      bk_mode;     /* OPAQUE(2, default) / TRANSPARENT(1) */
    /* C14: a dialog window (from a template). The kernel owns its painting
     * (fill bg + draw controls) since the app's DialogProc usually doesn't. */
    bool     is_dialog;
    /* C15: an attached menu bar (SetMenu); kernel-drawn into the client top. */
    uint64_t menu;        /* HMENU of the bar (0 = none)                    */
    int      menu_open;   /* index of the top-level item whose dropdown is open (-1) */
};

static struct {
    int  tgid;            /* owning process (thread group); 0 = none */
    bool quit;            /* PostQuitMessage seen (thread-wide) */
    int  quitcode;
    struct win32_win win[WIN32_MAX_WINDOWS];
    struct { char name[32]; uint64_t wndproc; } cls[WIN32_MAX_CLASSES];
    int  ncls;
    /* C12: virtual child controls + a ring of WM_COMMAND notifications pending
     * delivery to a parent window. Both live here so the per-fresh-app memset
     * clears them with the rest of the bridge state. */
    struct win32_ctrl ctrl[WIN32_MAX_CTRLS];
    struct { uint64_t hwnd; uint32_t message; uint64_t wParam, lParam; } cmd_q[WIN32_CMDQ];
    int  cmd_head, cmd_tail;
} g_win32_gui;

/* C13 LISTBOX item store, indexed by the control's slot. A separate static (not
 * in g_win32_gui); a listbox clears its slot on creation, so cross-app reuse is
 * safe. */
static struct win32_lb g_win32_lb[WIN32_MAX_CTRLS];

/* C25 comctl32 common-control store, indexed by the control's slot (parallel to
 * g_win32_lb). Rich enough for a ListView report grid, a TreeView, a multi-part
 * StatusBar and a Progress bar. Cleared when its control is created. */
#define CC_TEXT     28
#define CC_LV_COLS  6
#define CC_LV_ROWS  12
#define CC_LV_SUB   6
#define CC_TV_NODES 20
#define CC_SB_PARTS 4
struct win32_cc {
    /* SysListView32 (report view): columns + a row x subitem text grid. */
    int  lv_ncol;
    struct { int cx; char text[CC_TEXT]; } lv_col[CC_LV_COLS];
    int  lv_nrow;
    char lv_cell[CC_LV_ROWS][CC_LV_SUB][CC_TEXT];
    int  lv_sel;                 /* selected row (-1 = none) */
    /* SysTreeView32: a flat node array; hItem == (node index + 1). */
    int  tv_n;
    struct { char text[CC_TEXT]; int parent; int level;
             unsigned char expanded; unsigned char haskids; } tv[CC_TV_NODES];
    int  tv_sel;                 /* selected node index (-1 = none) */
    /* msctls_statusbar32: up to CC_SB_PARTS text panes. */
    int  sb_nparts;
    char sb_part[CC_SB_PARTS][CC_TEXT];
    /* msctls_progress32. */
    int  pb_min, pb_max, pb_pos, pb_step;
    /* ToolbarWindow32: just a button count (drawn as a strip). */
    int  tb_n;
};
static struct win32_cc g_win32_cc[WIN32_MAX_CTRLS];

/* True for the comctl32 control kinds that use g_win32_cc[] (C25). */
static bool win32_is_comctl(int type) {
    return type == WIN32_CTRL_LISTVIEW || type == WIN32_CTRL_TREEVIEW ||
           type == WIN32_CTRL_STATUSBAR || type == WIN32_CTRL_PROGRESS ||
           type == WIN32_CTRL_TOOLBAR;
}

/* Window-table entry for an HWND/HDC fd, or NULL. */
static struct win32_win *win32_win_find(int fd) {
    if (fd <= 0) return NULL;
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++)
        if (g_win32_gui.win[i].fd == fd) return &g_win32_gui.win[i];
    return NULL;
}
/* A free window slot, or NULL if the table is full. */
static struct win32_win *win32_win_alloc(void) {
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++)
        if (g_win32_gui.win[i].fd == 0) return &g_win32_gui.win[i];
    return NULL;
}
/* Count of live windows. */
static int win32_win_count(void) {
    int n = 0;
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++)
        if (g_win32_gui.win[i].fd > 0) n++;
    return n;
}

/* C12: case-insensitive string compare (for predefined control class names). */
static bool win32_streq_ci(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return false;
        if (!ca) return true;
    }
}
/* Map a window class name to a control kind (0 if not a control class). A
 * "BUTTON" with a checkbox style is resolved to WIN32_CTRL_CHECKBOX by the
 * caller (which has the dwStyle). */
static int win32_ctrl_class(const char *cn) {
    if (win32_streq_ci(cn, "BUTTON"))     return WIN32_CTRL_BUTTON;
    if (win32_streq_ci(cn, "EDIT"))       return WIN32_CTRL_EDIT;
    if (win32_streq_ci(cn, "STATIC"))     return WIN32_CTRL_STATIC;
    if (win32_streq_ci(cn, "LISTBOX"))    return WIN32_CTRL_LISTBOX;
    if (win32_streq_ci(cn, "SCROLLBAR"))  return WIN32_CTRL_SCROLLBAR;
    if (win32_streq_ci(cn, "COMBOBOX"))   return WIN32_CTRL_COMBOBOX;
    if (win32_streq_ci(cn, "SysTabControl32")) return WIN32_CTRL_TAB;
    /* C25: comctl32 common-control classes. */
    if (win32_streq_ci(cn, "SysListView32"))     return WIN32_CTRL_LISTVIEW;
    if (win32_streq_ci(cn, "SysTreeView32"))      return WIN32_CTRL_TREEVIEW;
    if (win32_streq_ci(cn, "msctls_statusbar32")) return WIN32_CTRL_STATUSBAR;
    if (win32_streq_ci(cn, "msctls_progress32"))  return WIN32_CTRL_PROGRESS;
    if (win32_streq_ci(cn, "ToolbarWindow32"))    return WIN32_CTRL_TOOLBAR;
    return 0;
}
/* BUTTON styles (low nibble of dwStyle) select the button subtype. */
#define BS_CHECKBOX        0x0002u
#define BS_AUTOCHECKBOX    0x0003u
#define BS_RADIOBUTTON     0x0004u
#define BS_GROUPBOX        0x0007u
#define BS_AUTORADIOBUTTON 0x0009u
/* C14: a BUTTON's low-style-bits subtype. BS_GROUPBOX(7)/BS_(AUTO)RADIOBUTTON
 * (4/9) reclassify a "BUTTON" the way BS_(AUTO)CHECKBOX(2/3) does. */
static int win32_button_subkind(int base, uint32_t style) {
    if (base != WIN32_CTRL_BUTTON) return base;
    uint32_t low = style & 0xFu;
    if (low == BS_CHECKBOX || low == BS_AUTOCHECKBOX)       return WIN32_CTRL_CHECKBOX;
    if (low == BS_RADIOBUTTON || low == BS_AUTORADIOBUTTON) return WIN32_CTRL_RADIO;
    if (low == BS_GROUPBOX)                                 return WIN32_CTRL_GROUPBOX;
    return WIN32_CTRL_BUTTON;
}
/* C12: resolve a control HWND token to its slot, or NULL. */
static struct win32_ctrl *win32_ctrl_find(uint64_t h) {
    if ((h & WIN32_TAG_MASK) != WIN32_CTRL_TAG) return NULL;
    int s = (int)(h & 0xFF);
    if (s < 0 || s >= WIN32_MAX_CTRLS || !g_win32_gui.ctrl[s].in_use) return NULL;
    return &g_win32_gui.ctrl[s];
}
/* C12 control helpers used by GetMessage/EndPaint (defined below, after the GDI
 * backend they call). */
static void win32_draw_controls(int parent_fd);
static void win32_refresh_controls(int parent_fd);
static void win32_dlg_paint(int fd);   /* C14: kernel-paint a dialog window */
static void win32_msg_push(int parent_fd, uint32_t message, uint64_t wParam, uint64_t lParam);
static bool win32_route_control_input(int parent_fd, const struct gui_event *ev);
static void win32_draw_menu(int fd);                                   /* C15 menus */
static bool win32_route_menu_input(int fd, const struct gui_event *ev);/* C15 menus */
static void win32_reset_menus_timers(void);                            /* C15 fresh-app reset */
static void win32_reset_env(void);                                     /* C16c env reset */

/* C16d: all Win32 UI text (control labels, menus, MessageBox, file dialog) goes
 * through here so it renders as real TrueType when a font is available, falling
 * back to the 8x8 bitmap otherwise. Native (non-PE) apps' sys_gui_text stays on
 * the bitmap font. The y arg is the 8x8-font top; nudge up so the taller glyph
 * keeps roughly the same vertical centre. */
#define WIN32_UI_FONT_PX 14
static void win32_ui_text(struct window *win, int x, int y, const char *s,
                          uint32_t fg, uint32_t bg) {
    if (win && s && kfont_available()) {
        int yy = y - (WIN32_UI_FONT_PX - 8) / 2;
        (void)kfont_draw_window(win, x, yy, s, -1, fg, WIN32_UI_FONT_PX);
        (void)bg;
    } else {
        (void)gui_window_text(win, x, y, s, fg, bg);
    }
}
struct win32_msg;
static bool win32_timer_poll(int only_fd, struct win32_msg *m);        /* C15 timers */
/* C13: MessageBoxA (font section) draws via these, defined further below. */
static struct window *win32_hdc_window(int fd);
extern uint64_t pit_ticks(void);
extern uint32_t pit_hz(void);

/* Retire window-table slots whose fd is no longer a live window in the CURRENT
 * process -- i.e. leftovers from a previous, now-exited GUI process. pids get
 * reused, so we validate against the live fd table (per-process) rather than
 * keying on tgid: a dead predecessor's fd won't resolve to a window here. */
static void win32_prune_dead_windows(void) {
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++) {
        int fd = g_win32_gui.win[i].fd;
        if (fd <= 0) continue;
        struct file *f = fd_lookup(fd);
        if (!f || f->kind != FILE_KIND_WINDOW || !f->win)
            g_win32_gui.win[i].fd = 0;
    }
}
/* Publish a window's WndProc into the CRT slot the DispatchMessage trampoline
 * reads, so the next DispatchMessage calls THIS window's WndProc. */
static void win32_stash_wndproc(const struct win32_win *w) {
    uint64_t wp = w ? w->wndproc : 0;
    (void)copy_to_user((void *)(uintptr_t)(WIN32_CRT_DATA_BASE + WIN32_CRT_WNDPROC),
                       &wp, 8);
}

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

/* C11: a CreatePen handle is a tagged token carrying its COLORREF (high byte
 * 0x7C, distinct from the brush 0x7B / thread 0x74 / module 0x7D / FILE* 0xF11E
 * spaces). SelectObject reads it; LineTo/Rectangle/Ellipse draw with it. */
#define WIN32_PEN_TAG     0x7C00000000000000ull

/* WS_CHILD window style bit (C11): a window created with it (or with a non-NULL
 * hWndParent) is a child/owned window. WS_GROUP starts a new radio group (C14). */
#define WS_CHILD          0x40000000u
#define WS_GROUP          0x00020000u

/* C12/C13 control + scroll messages and notifications. */
#define WM_SETTEXT        0x000C
#define WM_TIMER          0x0113   /* C15: SetTimer -> periodic WM_TIMER       */
#define WM_INITDIALOG     0x0110   /* C14: first message a DialogProc receives */
#define WM_VSCROLL        0x0115
#define WM_COMMAND        0x0111
#define WM_NOTIFY         0x004E   /* C14: TAB sends TCN_SELCHANGE via this    */
/* C15: menus. HMENU = WIN32_MENU_TAG | index; MF_* are AppendMenuA flags. */
#define WIN32_MENU_TAG    0x6600000000000000ull   /* distinct from font 0x65 / ctrl 0x64 */
#define MF_POPUP          0x0010u
#define MF_SEPARATOR      0x0800u
#define WIN32_MENUBAR_H   22
#define WIN32_MENU_ITEMH  18
#define BN_CLICKED        0
#define LBN_SELCHANGE     1
#define CBN_SELCHANGE     1        /* C14 COMBOBOX select notification (HIWORD) */
#define TCN_SELCHANGE     (0u-551u)/* C14 TAB select notification (NMHDR.code)  */
/* SendMessage control messages. */
#define BM_GETCHECK       0x00F0
#define BM_SETCHECK       0x00F1
#define LB_ADDSTRING      0x0180
#define LB_RESETCONTENT   0x0184
#define LB_SETCURSEL      0x0186
#define LB_GETCURSEL      0x0188
#define LB_GETTEXT        0x0189
#define LB_GETCOUNT       0x018B
/* C14 COMBOBOX messages (CB_*). */
#define CB_ADDSTRING      0x0143
#define CB_RESETCONTENT   0x014B
#define CB_GETCOUNT       0x0146
#define CB_SETCURSEL      0x014E
#define CB_GETCURSEL      0x0147
#define CB_GETLBTEXT      0x0148
/* C14 TAB messages (TCM_FIRST 0x1300). lParam of TCM_INSERTITEM = TCITEM whose
 * pszText (@+0x10) is the tab label. */
#define TCM_FIRST         0x1300
#define TCM_GETIMAGELIST  (TCM_FIRST + 2)
#define TCM_INSERTITEMA   (TCM_FIRST + 7)
#define TCM_GETITEMCOUNT  (TCM_FIRST + 4)
#define TCM_GETCURSEL     (TCM_FIRST + 11)
#define TCM_SETCURSEL     (TCM_FIRST + 12)
/* ---- C25: comctl32 common-control messages ---- */
#define WM_USER           0x0400
/* SysListView32 (LVM_FIRST 0x1000). */
#define LVM_FIRST         0x1000
#define LVM_GETITEMCOUNT  (LVM_FIRST + 4)
#define LVM_SETITEMA      (LVM_FIRST + 6)
#define LVM_INSERTITEMA   (LVM_FIRST + 7)
#define LVM_DELETEALLITEMS (LVM_FIRST + 9)
#define LVM_GETNEXTITEM   (LVM_FIRST + 12)
#define LVM_INSERTCOLUMNA (LVM_FIRST + 27)
#define LVM_SETITEMSTATE  (LVM_FIRST + 43)
#define LVM_GETITEMTEXTA  (LVM_FIRST + 45)
#define LVM_SETITEMTEXTA  (LVM_FIRST + 46)
#define LVIS_FOCUSED      0x0001
#define LVIS_SELECTED     0x0002
#define LVNI_SELECTED     0x0002
/* SysTreeView32 (TVM_FIRST 0x1100). */
#define TVM_FIRST         0x1100
#define TVM_INSERTITEMA   (TVM_FIRST + 0)
#define TVM_DELETEITEM    (TVM_FIRST + 1)
#define TVM_EXPAND        (TVM_FIRST + 2)
#define TVM_GETCOUNT      (TVM_FIRST + 5)
#define TVM_GETNEXTITEM   (TVM_FIRST + 10)
#define TVM_SELECTITEM    (TVM_FIRST + 11)
#define TVGN_ROOT         0x0000
#define TVGN_CARET        0x0009
#define TVE_COLLAPSE      0x0001
#define TVE_EXPAND        0x0002
/* msctls_statusbar32 (SB_* = WM_USER + n). */
#define SB_SETTEXTA       (WM_USER + 1)
#define SB_GETTEXTA       (WM_USER + 2)
#define SB_GETTEXTLENGTHA (WM_USER + 3)
#define SB_SETPARTS       (WM_USER + 4)
#define SB_GETPARTS       (WM_USER + 6)
/* msctls_progress32 (PBM_* = WM_USER + n). */
#define PBM_SETRANGE      (WM_USER + 1)
#define PBM_SETPOS        (WM_USER + 2)
#define PBM_DELTAPOS      (WM_USER + 3)
#define PBM_SETSTEP       (WM_USER + 4)
#define PBM_STEPIT        (WM_USER + 5)
#define PBM_SETRANGE32    (WM_USER + 6)
#define PBM_GETPOS        (WM_USER + 8)
/* ToolbarWindow32 (TB_* = WM_USER + n). */
#define TB_ADDBUTTONS     (WM_USER + 20)
#define TB_BUTTONCOUNT    (WM_USER + 24)
/* Scrollbar notification codes (wParam low word of WM_VSCROLL). */
#define SB_LINEUP         0
#define SB_LINEDOWN       1
#define SB_PAGEUP         2
#define SB_PAGEDOWN       3
/* MessageBox / dialog button-id results. */
#define IDOK              1
#define IDCANCEL          2
#define IDABORT           3
#define IDRETRY           4
#define IDIGNORE          5
#define IDYES             6
#define IDNO              7
/* MessageBox button-set (uType & 0xF). */
#define MB_OK               0u
#define MB_OKCANCEL         1u
#define MB_ABORTRETRYIGNORE 2u
#define MB_YESNOCANCEL      3u
#define MB_YESNO            4u
#define MB_RETRYCANCEL      5u

/* The default window fill (C7's blue) used when no solid brush is supplied. */
#define WIN32_FILL_DEFAULT 0x002E5C8Au
/* Default pen colour (white) until CreatePen/SelectObject sets one. */
#define WIN32_PEN_DEFAULT  0x00FFFFFFu

/* The default window fill (C7's blue) used when no solid brush is supplied. */
#define WIN32_FILL_DEFAULT 0x002E5C8Au
/* Default pen colour (white) until CreatePen/SelectObject sets one. */
#define WIN32_PEN_DEFAULT  0x00FFFFFFu

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

/* Resolve a class name (or NULL) to its registered WndProc. Falls back to the
 * most-recently registered class so a single-class app is robust even if the
 * name match is imperfect (and so the very first window before any explicit
 * lookup still gets a WndProc). 0 if no class has been registered. */
static uint64_t win32_class_wndproc(const char *name) {
    if (name && name[0]) {
        for (int i = 0; i < g_win32_gui.ncls; i++)
            if (strcmp(g_win32_gui.cls[i].name, name) == 0)
                return g_win32_gui.cls[i].wndproc;
    }
    return g_win32_gui.ncls ? g_win32_gui.cls[g_win32_gui.ncls - 1].wndproc : 0;
}

/* RegisterClassA(&WNDCLASSA): record the class name (+0x40) -> WndProc (+0x08)
 * mapping, and default the trampoline's CRT slot to this WndProc (GetMessage
 * refines it per message for multi-window apps). */
static long w32_RegisterClassA(uint64_t *a) {
    /* New-GUI-app boundary: after retiring any windows left by a previous,
     * now-exited GUI process, if none remain this is a fresh app -- reset the
     * whole bridge so a leftover quit flag / class table can't leak in. (Apps
     * register their class before creating windows; our apps use a single
     * class, so this fires once per app.) */
    win32_prune_dead_windows();
    if (win32_win_count() == 0) {
        memset(&g_win32_gui, 0, sizeof(g_win32_gui));
        win32_reset_menus_timers();   /* C15: HMENU/timer tokens don't survive a process exit */
    }

    uint64_t wc = a[0];
    uint64_t wndproc = 0, cnameptr = 0;
    (void)copy_from_user(&wndproc,  (const void *)(uintptr_t)(wc + 0x08), 8); /* lpfnWndProc */
    (void)copy_from_user(&cnameptr, (const void *)(uintptr_t)(wc + 0x40), 8); /* lpszClassName */
    char cname[32]; cname[0] = '\0';
    if (cnameptr)
        (void)strncpy_from_user(cname, (const char *)(uintptr_t)cnameptr, sizeof(cname));

    int slot = -1;
    for (int i = 0; i < g_win32_gui.ncls; i++)
        if (strcmp(g_win32_gui.cls[i].name, cname) == 0) { slot = i; break; }
    if (slot < 0 && g_win32_gui.ncls < WIN32_MAX_CLASSES) slot = g_win32_gui.ncls++;
    if (slot >= 0) {
        memcpy(g_win32_gui.cls[slot].name, cname, sizeof(g_win32_gui.cls[slot].name));
        g_win32_gui.cls[slot].name[sizeof(g_win32_gui.cls[slot].name) - 1] = '\0';
        g_win32_gui.cls[slot].wndproc = wndproc;
    }
    (void)copy_to_user((void *)(uintptr_t)(WIN32_CRT_DATA_BASE + WIN32_CRT_WNDPROC),
                       &wndproc, 8);
    return 1;   /* a non-zero class atom */
}

/* Allocate + initialise a virtual child control in the parent's client area.
 * `ktext` is a KERNEL string (or NULL); `style` drives radio grouping (WS_GROUP).
 * Returns the control slot index, or -1 if the table is full. Shared by
 * CreateWindowExA (C12/C13) and the C14 dialog-template builder. */
static int win32_ctrl_make(int parent_fd, int type, int x, int y, int w, int h,
                           int id, const char *ktext, uint32_t style) {
    for (int i = 0; i < WIN32_MAX_CTRLS; i++) {
        if (g_win32_gui.ctrl[i].in_use) continue;
        struct win32_ctrl *c = &g_win32_gui.ctrl[i];
        memset(c, 0, sizeof(*c));
        c->in_use = true;  c->parent_fd = parent_fd;  c->type = type;
        c->x = x;  c->y = y;  c->w = w;  c->h = h;  c->id = id;
        if (ktext) {
            int n = 0;
            while (n < (int)sizeof(c->text) - 1 && ktext[n]) { c->text[n] = ktext[n]; n++; }
            c->text[n] = 0;
        }
        /* Radio grouping: a control with WS_GROUP starts a new group; controls
         * after it share that group number until the next WS_GROUP. */
        int grp = 0;
        for (int j = 0; j < WIN32_MAX_CTRLS; j++)
            if (j != i && g_win32_gui.ctrl[j].in_use &&
                g_win32_gui.ctrl[j].parent_fd == parent_fd &&
                g_win32_gui.ctrl[j].group > grp)
                grp = g_win32_gui.ctrl[j].group;
        if (style & WS_GROUP) grp++;
        c->group = grp;

        switch (type) {
        case WIN32_CTRL_EDIT: {                    /* auto-focus the first EDIT */
            bool any = false;
            for (int j = 0; j < WIN32_MAX_CTRLS; j++)
                if (g_win32_gui.ctrl[j].in_use &&
                    g_win32_gui.ctrl[j].parent_fd == parent_fd &&
                    g_win32_gui.ctrl[j].type == WIN32_CTRL_EDIT &&
                    g_win32_gui.ctrl[j].focus) any = true;
            if (!any) c->focus = true;
            break;
        }
        case WIN32_CTRL_LISTBOX:
        case WIN32_CTRL_COMBOBOX:                  /* C14: list/combo/tab share */
        case WIN32_CTRL_TAB:                       /* the g_win32_lb item store  */
            memset(&g_win32_lb[i], 0, sizeof(g_win32_lb[i]));
            g_win32_lb[i].sel = -1;
            break;
        case WIN32_CTRL_SCROLLBAR:
            c->sb_min = 0; c->sb_max = 100; c->sb_pos = 0;
            break;
        case WIN32_CTRL_LISTVIEW:
        case WIN32_CTRL_TREEVIEW:
        case WIN32_CTRL_STATUSBAR:
        case WIN32_CTRL_PROGRESS:
        case WIN32_CTRL_TOOLBAR: {     /* C25: comctl32 store */
            struct win32_cc *cc = &g_win32_cc[i];
            memset(cc, 0, sizeof(*cc));
            cc->lv_sel = -1; cc->tv_sel = -1;
            cc->pb_min = 0; cc->pb_max = 100; cc->pb_pos = 0; cc->pb_step = 10;
            break;
        }
        }
        struct win32_win *pw = win32_win_find(parent_fd);
        if (pw) pw->needs_paint = true;            /* redraw parent -> draw control */
        return i;
    }
    return -1;
}

/* CreateWindowExA(exStyle, class, name, style, x, y, W, H, parent, menu,
 *                 hInst, param) -> HWND (== the tobyOS window fd). class=a1,
 * name=a2, style=a3, W=a6, H=a7, hWndParent=a8 (C11: reachable now the gate
 * marshals 10 args). Allocates a window-table slot bound to its class's WndProc;
 * a WS_CHILD style or a non-NULL parent records the parent/owner relationship. */
static long w32_CreateWindowExA(uint64_t *a) {
    uint64_t cnameptr = a[1];
    uint64_t name     = a[2];
    uint32_t style    = (uint32_t)a[3];
    int w = (int)(uint32_t)a[6];
    int h = (int)(uint32_t)a[7];
    int parent_fd = (int)a[8];           /* hWndParent (== the parent's fd) */
    if (w <= 0 || w > 4096) w = 400;
    if (h <= 0 || h > 4096) h = 200;
    /* Keep the parent link only if it references a live window of ours. */
    if (!win32_win_find(parent_fd)) parent_fd = 0;
    (void)style;   /* WS_CHILD is implied by a non-NULL parent here */

    win32_prune_dead_windows();
    g_win32_gui.quit = false;    /* creating a window -> not in a quit state */

    char cname[32]; cname[0] = '\0';
    if (cnameptr)
        (void)strncpy_from_user(cname, (const char *)(uintptr_t)cnameptr, sizeof(cname));

    /* A predefined control class (BUTTON/EDIT/STATIC/CHECKBOX/LISTBOX/SCROLLBAR;
     * C14: COMBOBOX/RADIO/GROUPBOX/SysTabControl32) with a parent creates a
     * VIRTUAL child rendered into the parent's client area, not a top-level
     * window. Its HWND is a tagged token; its id is hMenu (a9). A "BUTTON" with
     * a checkbox/radio/groupbox style is reclassified accordingly. */
    int ctype = win32_button_subkind(win32_ctrl_class(cname), style);
    if (ctype && parent_fd) {
        char ktext[WIN32_CTRL_TEXT]; ktext[0] = '\0';
        if (name)
            (void)strncpy_from_user(ktext, (const char *)(uintptr_t)name, sizeof(ktext));
        int slot = win32_ctrl_make(parent_fd, ctype, (int)(int32_t)a[4],
                                   (int)(int32_t)a[5], w, h, (int)a[9],
                                   name ? ktext : NULL, style);
        if (slot < 0) return 0;                       /* control table full */
        return (long)(WIN32_CTRL_TAG | (uint64_t)slot);
    }

    struct win32_win *win = win32_win_alloc();
    if (!win) return 0;                          /* table full -> NULL HWND */
    long fd = sys_gui_create((uint32_t)w, (uint32_t)h, (const char *)(uintptr_t)name);
    if (fd < 0) return 0;                        /* NULL HWND */

    struct proc *p = current_proc();
    g_win32_gui.tgid   = p ? (p->is_thread ? p->tgid : p->pid) : 0;
    win->fd          = (int)fd;
    win->gwin        = win32_hdc_window((int)fd);  /* C18c: stash for pixel readback */
    win->w           = w;
    win->h           = h;
    win->wndproc     = win32_class_wndproc(cname);
    win->needs_paint = true;
    win->destroy     = false;
    win->fill_color  = WIN32_FILL_DEFAULT;
    win->pen_color   = WIN32_PEN_DEFAULT;
    win->cur_x       = 0;
    win->cur_y       = 0;
    win->parent_fd   = parent_fd;
    win->font_scale  = 1;
    win->font_px     = 0;
    win->font_face   = 0;             /* C18c: KFONT_REGULAR */
    win->text_color  = 0x00FFFFFFu;   /* white */
    win->bk_mode     = 2;             /* OPAQUE */
    win->is_dialog   = false;
    /* C15: a top-level window's hMenu arg (a9) is its menu bar (if a menu token,
     * not a control id). menu_open = -1 = no dropdown showing. */
    win->menu        = ((a[9] & WIN32_TAG_MASK) == WIN32_MENU_TAG) ? a[9] : 0;
    win->menu_open   = -1;
    return fd;   /* HWND == HDC == fd */
}

/* ShowWindow/UpdateWindow: a window -> queue its repaint; a control (C12) ->
 * queue its PARENT's repaint so the control gets drawn into it. */
static long w32_ShowWindow(uint64_t *a) {
    struct win32_ctrl *c = win32_ctrl_find(a[0]);
    if (c) {
        c->hidden = ((int)a[1] == 0 /* SW_HIDE */);   /* C14: TAB page show/hide */
        struct win32_win *pw = win32_win_find(c->parent_fd);
        if (pw) { pw->needs_paint = true; win32_refresh_controls(c->parent_fd); }
        return 1;
    }
    struct win32_win *w = win32_win_find((int)a[0]); if (w) w->needs_paint = true; return 1;
}
static long w32_UpdateWindow(uint64_t *a) { return w32_ShowWindow(a); }
static long w32_PostQuitMessage(uint64_t *a) {
    g_win32_gui.quit = true; g_win32_gui.quitcode = (int)a[0]; return 0;
}
static long w32_TranslateMessage(uint64_t *a) { (void)a; return 0; }

/* DefWindowProcA(hwnd, message, wParam, lParam). The one default behaviour we
 * implement is the idiomatic WM_CLOSE -> DestroyWindow: a real app that doesn't
 * handle WM_CLOSE in its WndProc passes it here, and Windows responds by
 * destroying the window (which then sends WM_DESTROY). */
static long w32_DefWindowProcA(uint64_t *a) {
    if ((uint32_t)a[1] == WM_CLOSE) {
        struct win32_win *w = win32_win_find((int)a[0]);
        if (w) w->destroy = true;
    }
    return 0;
}

/* DestroyWindow(hwnd) -> TRUE. Arms GetMessage to deliver one WM_DESTROY for
 * THIS window (then retire it); the app's WndProc can PostQuitMessage. */
static long w32_DestroyWindow(uint64_t *a) {
    struct win32_win *w = win32_win_find((int)a[0]);
    if (w) w->destroy = true;
    return 1;
}

/* InvalidateRect(hwnd, &RECT|NULL, erase) -> TRUE. Marks THIS window for a
 * repaint so the next GetMessage delivers it a WM_PAINT (e.g. after a click). */
static long w32_InvalidateRect(uint64_t *a) {
    struct win32_win *w = win32_win_find((int)a[0]);
    if (w) w->needs_paint = true;
    return 1;
}

/* Translate one struct gui_event (from the window `fd`) into a Win32 message in
 * *m. Returns true if a deliverable message was produced (RESIZE/NONE dropped). */
static bool win32_event_to_msg(int fd, const struct gui_event *ev,
                               struct win32_msg *m) {
    m->hwnd = (uint64_t)fd;
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

/* GetMessageA(&MSG, hwnd, min, max) -> >0 normal / 0 on WM_QUIT. Scans ALL the
 * process's windows. Order: WM_QUIT (PostQuitMessage, thread-wide), then per
 * window a pending WM_DESTROY (after which the window is retired -- desktop
 * window closed + slot freed), then a queued WM_PAINT, then a drained input
 * event (mouse/keyboard/close -> WM_*); else yield + WM_NULL. Each delivered
 * message first stashes its window's WndProc so the following DispatchMessage
 * routes to the correct one (multi-window). */
static long w32_GetMessageA(uint64_t *a) {
    uint64_t msg = a[0];
    struct win32_msg m;
    memset(&m, 0, sizeof(m));

    if (g_win32_gui.quit) {
        m.message = WM_QUIT; m.wParam = (uint64_t)(uint32_t)g_win32_gui.quitcode;
        (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
        return 0;
    }

    /* WM_DESTROY: deliver, then retire the window (close its desktop window +
     * free the slot). C11: destroying a parent cascades to its children -- mark
     * each child for destroy so it gets its own WM_DESTROY next. */
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++) {
        struct win32_win *w = &g_win32_gui.win[i];
        if (w->fd > 0 && w->destroy) {
            int fd = w->fd;
            for (int j = 0; j < WIN32_MAX_WINDOWS; j++)
                if (g_win32_gui.win[j].fd > 0 && g_win32_gui.win[j].parent_fd == fd)
                    g_win32_gui.win[j].destroy = true;   /* cascade to children */
            win32_stash_wndproc(w);
            m.hwnd = (uint64_t)fd; m.message = WM_DESTROY;
            if (g_win32_log_input)
                kprintf("[winpe] GetMessage hwnd=%d -> WM_DESTROY\n", fd);
            w->fd = 0; w->destroy = false; w->needs_paint = false;  /* retire */
            (void)sys_close(fd);                                    /* remove from desktop */
            (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
            return 1;
        }
    }

    /* C12: deliver a pending WM_COMMAND (e.g. a button click) to its parent. */
    if (g_win32_gui.cmd_head != g_win32_gui.cmd_tail) {
        int t = g_win32_gui.cmd_tail;
        struct win32_win *pw = win32_win_find((int)g_win32_gui.cmd_q[t].hwnd);
        win32_stash_wndproc(pw);
        m.hwnd    = g_win32_gui.cmd_q[t].hwnd;
        m.message = g_win32_gui.cmd_q[t].message;
        m.wParam  = g_win32_gui.cmd_q[t].wParam;
        m.lParam  = g_win32_gui.cmd_q[t].lParam;
        g_win32_gui.cmd_tail = (t + 1) % WIN32_CMDQ;
        if (g_win32_log_input)
            kprintf("[winpe] GetMessage hwnd=%d -> WM_COMMAND id=%d\n",
                    (int)m.hwnd, (int)(m.wParam & 0xFFFF));
        (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
        return 1;
    }

    /* WM_PAINT for any window that needs one. (Controls are drawn into the
     * parent's backbuffer by EndPaint, after the app's own client paint.) */
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++) {
        struct win32_win *w = &g_win32_gui.win[i];
        if (w->fd > 0 && w->needs_paint) {
            w->needs_paint = false;
            /* C14: a modeless dialog window is painted by the kernel (the app's
             * DialogProc doesn't), so handle its WM_PAINT here + keep looping. */
            if (w->is_dialog) { win32_dlg_paint(w->fd); continue; }
            win32_stash_wndproc(w);
            m.hwnd = (uint64_t)w->fd; m.message = WM_PAINT;
            (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
            return 1;
        }
    }

    /* One real input event from any window -> WM_*. C12: if the window has child
     * controls and the event hits one (button click / edit keystroke), the
     * control consumes it (the app sees only the resulting WM_COMMAND); we
     * return WM_NULL so the loop keeps turning. */
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++) {
        struct win32_win *w = &g_win32_gui.win[i];
        if (w->fd <= 0) continue;
        struct gui_event ev;
        if (win32_poll_window_event(w->fd, &ev) <= 0) continue;
        if (win32_route_menu_input(w->fd, &ev)) {     /* C15: menu bar / dropdown */
            m.hwnd = (uint64_t)w->fd; m.message = WM_NULL;
            (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
            return 1;
        }
        if (win32_route_control_input(w->fd, &ev)) {
            m.hwnd = (uint64_t)w->fd; m.message = WM_NULL;
            (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
            return 1;
        }
        if (win32_event_to_msg(w->fd, &ev, &m)) {
            win32_stash_wndproc(w);
            if (g_win32_log_input && m.message != WM_MOUSEMOVE)
                kprintf("[winpe] GetMessage hwnd=%d -> WM_%04x wParam=0x%lx lParam=0x%lx\n",
                        w->fd, (unsigned)m.message, (unsigned long)m.wParam,
                        (unsigned long)m.lParam);
            (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
            return 1;
        }
    }

    /* C15: a due timer -> WM_TIMER (after input, before idle). */
    if (win32_timer_poll(-1, &m)) {
        if (g_win32_log_input)
            kprintf("[winpe] GetMessage hwnd=%d -> WM_TIMER id=%lu\n",
                    (int)m.hwnd, (unsigned long)m.wParam);
        (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
        return 1;
    }

    /* Nothing pending: yield and deliver WM_NULL so the loop keeps turning
     * (and other procs run) without busy-spinning. */
    sched_yield();
    m.hwnd = 0; m.message = WM_NULL;
    (void)copy_to_user((void *)(uintptr_t)msg, &m, sizeof(m));
    return 1;
}

/* BeginPaint(hwnd, &PAINTSTRUCT) -> HDC. EndPaint -> present (flip) the window. */
static long w32_BeginPaint(uint64_t *a) {
    uint64_t hwnd = a[0];
    uint64_t ps   = a[1];
    struct win32_win *w = win32_win_find((int)hwnd);
    /* PAINTSTRUCT: hdc@0, fErase@8, rcPaint(l,t,r,b)@0x0C */
    uint64_t hdc = hwnd;          /* HDC == HWND == fd */
    uint32_t erase = 1;
    int32_t  rc[4] = { 0, 0, w ? w->w : 0, w ? w->h : 0 };
    (void)copy_to_user((void *)(uintptr_t)(ps + 0x00), &hdc, 8);
    (void)copy_to_user((void *)(uintptr_t)(ps + 0x08), &erase, 4);
    (void)copy_to_user((void *)(uintptr_t)(ps + 0x0C), rc, sizeof(rc));
    return (long)hdc;
}
static long w32_EndPaint(uint64_t *a) {
    int fd = (int)a[0];
    if (win32_win_find(fd)) {
        win32_draw_controls(fd);   /* C12: overlay child controls on the app's paint */
        win32_draw_menu(fd);       /* C15: menu bar / dropdown on top */
        (void)sys_gui_flip(fd);
    }
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

/* C12: free a bitmap's backing store -- forward-declared so DeleteObject can
 * route to it. Defined with the bitmap shims below. */
static void win32_bitmap_free(uint64_t hbmp);

/* gdi32!DeleteObject(hgdiobj) -> TRUE. Brush/pen tokens carry no allocation; a
 * bitmap (C12) frees its pixel buffer. */
static long w32_DeleteObject(uint64_t *a) {
    if ((a[0] & WIN32_TAG_MASK) == WIN32_BITMAP_TAG) win32_bitmap_free(a[0]);
    return 1;
}

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
    struct win32_win *win = win32_win_find(fd);
    if (win) win->fill_color = color;
    uint32_t whlen = ((uint32_t)(w & 0xFFFF)) | ((uint32_t)(h & 0xFFFF) << 16);
    (void)sys_gui_fill(fd, x, y, whlen, color);
    return 1;
}

/* C14b: the DC's TTF pixel height (0 = the default 16 px). */
static int win32_win_px(const struct win32_win *w) {
    int px = w ? w->font_px : 0;
    return px > 0 ? px : 16;
}
/* C18c: the DC's selected font face (KFONT_REGULAR if none). */
static int win32_win_face(const struct win32_win *w) {
    return w ? w->font_face : 0;
}

/* gdi32!TextOutA(hdc, x, y, str, len) -> non-zero. C14b: when the TrueType font
 * is available, render real glyphs at the DC's pixel height (CreateFont) in the
 * DC's text colour (SetTextColor) via the kernel rasterizer; else fall back to
 * the scaled 8x8 bitmap font. */
static long w32_TextOutA(uint64_t *a) {
    int      fd  = (int)a[0];
    int      x   = (int)(int32_t)a[1];
    int      y   = (int)(int32_t)a[2];
    uint64_t str = a[3];
    int      len = (int)(int32_t)a[4];
    struct win32_win *win = win32_win_find(fd);
    uint32_t fg = win ? win->text_color : 0x00FFFFFFu;
    if (kfont_available()) {
        char s[256];
        long n = strncpy_from_user(s, (const char *)(uintptr_t)str, sizeof(s));
        if (n < 0) return 0;
        if (len >= 0 && len < (int)n) s[len] = 0;
        struct window *gw = win32_hdc_window(fd);
        if (gw) (void)kfont_draw_window_f(gw, x, y, s, -1, fg, win32_win_px(win),
                                          win32_win_face(win));
        return 1;
    }
    uint32_t xy    = ((uint32_t)(x & 0xFFFF)) | ((uint32_t)(y & 0xFFFF) << 16);
    uint32_t bg    = (win ? win->fill_color : WIN32_FILL_DEFAULT) & 0x00FFFFFFu;
    int      scale = win ? win->font_scale : 1;
    (void)sys_gui_text_scaled(fd, xy, (const char *)(uintptr_t)str, fg,
                              bg | ((uint32_t)(scale & 0x7F) << 24));
    return 1;
}

/* ---- C13/C14b: GDI fonts + text metrics ----
 * C14b: an HFONT token carries the requested PIXEL HEIGHT; the kernel TTF
 * rasterizer (kfont.c) renders at that size. If no font ships, the DC falls
 * back to the scaled 8x8 bitmap font (the token's height/8 = the bitmap scale). */

/* gdi32!CreateFontA(cHeight, cWidth, cEscapement, cOrientation, cWeight,
 * bItalic, ...) -> HFONT. C14b: low 16 bits carry |cHeight| in pixels. C18c:
 * bits 16-17 carry the face selected by cWeight (a[4], >=700 = bold) and
 * bItalic (a[5]) -- KFONT_REGULAR/BOLD/ITALIC/BOLDITALIC. */
static long w32_CreateFontA(uint64_t *a) {
    int hgt = (int)(int32_t)a[0]; if (hgt < 0) hgt = -hgt;
    if (hgt < 1) hgt = 16; if (hgt > 200) hgt = 200;
    int weight = (int)(int32_t)a[4];          /* FW_BOLD == 700 */
    bool italic = (a[5] & 0xFFu) != 0;
    int face = (weight >= 700 ? KFONT_BOLD : 0) | (italic ? KFONT_ITALIC : 0);
    return (long)(WIN32_FONT_TAG |
                  ((uint64_t)(face & WIN32_FONT_FACE_MASK) << WIN32_FONT_FACE_SHIFT) |
                  (uint64_t)(uint32_t)hgt);
}
/* gdi32!SetTextColor(hdc, COLORREF) -> previous colour (as COLORREF). */
static long w32_SetTextColor(uint64_t *a) {
    struct win32_win *w = win32_win_find((int)a[0]);
    if (!w) return -1;
    uint32_t old = w->text_color;
    w->text_color = win32_colorref_to_xrgb((uint32_t)a[1] & 0xFFFFFFu);
    return (long)win32_colorref_to_xrgb(old);
}
/* gdi32!SetBkColor(hdc, COLORREF) -> previous (the text background). */
static long w32_SetBkColor(uint64_t *a) {
    struct win32_win *w = win32_win_find((int)a[0]);
    if (!w) return -1;
    uint32_t old = w->fill_color;
    w->fill_color = win32_colorref_to_xrgb((uint32_t)a[1] & 0xFFFFFFu);
    return (long)win32_colorref_to_xrgb(old);
}
/* gdi32!SetBkMode(hdc, mode) -> previous mode. */
static long w32_SetBkMode(uint64_t *a) {
    struct win32_win *w = win32_win_find((int)a[0]);
    if (!w) return 0;
    int old = w->bk_mode; w->bk_mode = (int)a[1]; return old;
}
/* gdi32!GetTextExtentPoint32A(hdc, str, len, &SIZE{cx,cy}) -> TRUE. */
static long w32_GetTextExtentPoint32A(uint64_t *a) {
    struct win32_win *w = win32_win_find((int)a[0]);
    int len = (int)(int32_t)a[2];
    if (len < 0) len = 0;
    int px = win32_win_px(w);
    int32_t sz[2];
    if (kfont_available()) {
        char s[256];
        long n = strncpy_from_user(s, (const char *)(uintptr_t)a[1], sizeof(s));
        if (n < 0) n = 0;
        if (len > (int)n) len = (int)n;
        int asc = 0, desc = 0, lh = px;
        kfont_vmetrics_f(px, &asc, &desc, &lh, win32_win_face(w));
        sz[0] = kfont_text_width_f(s, len, px, win32_win_face(w));
        sz[1] = lh;
    } else {
        int scale = w ? w->font_scale : 1;
        sz[0] = len * 8 * scale; sz[1] = 8 * scale;
    }
    (void)copy_to_user((void *)(uintptr_t)a[3], sz, sizeof(sz));
    return 1;
}
/* gdi32!GetTextMetricsA(hdc, &TEXTMETRICA) -> TRUE (fills the common fields). */
static long w32_GetTextMetricsA(uint64_t *a) {
    struct win32_win *w = win32_win_find((int)a[0]);
    int px = win32_win_px(w);
    int32_t tm[8];
    memset(tm, 0, sizeof(tm));
    if (kfont_available()) {
        int asc = 0, desc = 0, lh = px;
        int face = win32_win_face(w);
        kfont_vmetrics_f(px, &asc, &desc, &lh, face);
        tm[0] = asc + desc;                       /* tmHeight       */
        tm[1] = asc;                              /* tmAscent       */
        tm[2] = desc;                             /* tmDescent      */
        tm[5] = kfont_text_width_f("n", -1, px, face);    /* tmAveCharWidth */
        tm[6] = kfont_text_width_f("W", -1, px, face);    /* tmMaxCharWidth */
    } else {
        int scale = w ? w->font_scale : 1;
        tm[0] = 8 * scale; tm[1] = 7 * scale; tm[2] = 1 * scale;
        tm[5] = 8 * scale; tm[6] = 8 * scale;
    }
    (void)copy_to_user((void *)(uintptr_t)a[1], tm, sizeof(tm));
    return 1;
}
/* user32!DrawTextA(hdc, str, len, &RECT, format) -> text height. Honours
 * DT_CENTER(0x1)/DT_VCENTER(0x4) within the rect; defaults to top-left. */
static long w32_DrawTextA(uint64_t *a) {
    int fd = (int)a[0];
    struct win32_win *w = win32_win_find(fd);
    if (!w) return 0;
    char s[256];
    long n = strncpy_from_user(s, (const char *)(uintptr_t)a[1], sizeof(s));
    if (n < 0) return 0;
    int len = (int)(int32_t)a[2];                 /* -1 = whole string */
    if (len >= 0 && len < (int)n) s[len] = 0;
    int32_t rc[4] = { 0, 0, 0, 0 };
    if (copy_from_user(rc, (const void *)(uintptr_t)a[3], sizeof(rc)) != 0) return 0;
    uint32_t fmt = (uint32_t)a[4];
    int px = win32_win_px(w);
    struct window *gw = win32_hdc_window(fd);
    if (kfont_available() && gw) {
        int face = win32_win_face(w);
        int tw = kfont_text_width_f(s, -1, px, face);
        int asc = 0, desc = 0, lh = px;
        kfont_vmetrics_f(px, &asc, &desc, &lh, face);
        int th = asc + desc;
        int x = rc[0], y = rc[1];
        if (fmt & 0x1u) x = rc[0] + ((rc[2] - rc[0]) - tw) / 2;   /* DT_CENTER  */
        if (fmt & 0x4u) y = rc[1] + ((rc[3] - rc[1]) - th) / 2;   /* DT_VCENTER */
        (void)kfont_draw_window_f(gw, x, y, s, -1, w->text_color, px, face);
        return th;
    }
    int scale = w->font_scale;
    int tw = (int)n * 8 * scale, th = 8 * scale;
    int x = rc[0], y = rc[1];
    if (fmt & 0x1u) x = rc[0] + ((rc[2] - rc[0]) - tw) / 2;   /* DT_CENTER  */
    if (fmt & 0x4u) y = rc[1] + ((rc[3] - rc[1]) - th) / 2;   /* DT_VCENTER */
    uint32_t xy = ((uint32_t)(x & 0xFFFF)) | ((uint32_t)(y & 0xFFFF) << 16);
    (void)sys_gui_text_scaled(fd, xy, (const char *)(uintptr_t)a[1], w->text_color,
                              (w->fill_color & 0xFFFFFFu) | ((uint32_t)(scale & 0x7F) << 24));
    return th;
}

/* user32!MessageBoxA(hWnd, lpText, lpCaption, uType) -> the clicked button's id.
 * A REAL MODAL dialog: it creates its own window with the text + the buttons for
 * the requested set (C15: MB_OK/OKCANCEL/YESNO/YESNOCANCEL/ABORTRETRYIGNORE/
 * RETRYCANCEL), draws it, and BLOCKS (a kernel poll loop, sched_yield-ing so the
 * rest of the system runs) until a button is clicked / Enter / the box is closed
 * -- then tears it down and returns the matching ID. */
static long w32_MessageBoxA(uint64_t *a) {
    char text[128], cap[40];
    text[0] = cap[0] = 0;
    if (a[1]) (void)strncpy_from_user(text, (const char *)(uintptr_t)a[1], sizeof(text));
    if (a[2]) (void)strncpy_from_user(cap,  (const char *)(uintptr_t)a[2], sizeof(cap));
    uint32_t kind = (uint32_t)a[3] & 0xFu;

    /* Button set for the requested type. */
    const char *blabel[3]; int bid[3]; int nb = 0;
    switch (kind) {
    case MB_OKCANCEL:         blabel[0]="OK";    bid[0]=IDOK;    blabel[1]="Cancel"; bid[1]=IDCANCEL; nb=2; break;
    case MB_YESNO:            blabel[0]="Yes";   bid[0]=IDYES;   blabel[1]="No";     bid[1]=IDNO;     nb=2; break;
    case MB_YESNOCANCEL:      blabel[0]="Yes";   bid[0]=IDYES;   blabel[1]="No";     bid[1]=IDNO;
                              blabel[2]="Cancel"; bid[2]=IDCANCEL; nb=3; break;
    case MB_RETRYCANCEL:      blabel[0]="Retry"; bid[0]=IDRETRY; blabel[1]="Cancel"; bid[1]=IDCANCEL; nb=2; break;
    case MB_ABORTRETRYIGNORE: blabel[0]="Abort"; bid[0]=IDABORT; blabel[1]="Retry";  bid[1]=IDRETRY;
                              blabel[2]="Ignore"; bid[2]=IDIGNORE; nb=3; break;
    default:                  blabel[0]="OK";    bid[0]=IDOK;    nb=1; break;
    }
    int dflt = bid[0];           /* Enter / close -> the default (first) button */

    const int W = 340, H = 150;
    /* NB: sys_gui_create marshals its title from USER space; ours is a kernel
     * string, so create the window directly with the kernel-side backend. */
    int fd = -1;
    if (cap_check(current_proc(), CAP_GUI, "MessageBox")) {
        struct window *bw = gui_window_create(W, H, cap[0] ? cap : "Message");
        if (bw) {
            struct file *bf = (struct file *)kmalloc(sizeof(*bf));
            if (bf) {
                memset(bf, 0, sizeof(*bf));
                bf->kind = FILE_KIND_WINDOW; bf->win = bw;
                fd = fd_alloc_into(current_proc(), bf);
                if (fd < 0) { kfree(bf); gui_window_close(bw); }
            } else gui_window_close(bw);
        }
    }
    if (fd < 0) return dflt;
    struct window *win = win32_hdc_window((int)fd);
    const int bw_ = 80, bh = 28, gap = 12, oky = H - 46;
    int total = nb * bw_ + (nb - 1) * gap;
    int bx0 = (W - total) / 2;
    if (win) {
        (void)gui_window_fill(win, 0, 0, W, H, 0x00DCDCDC);
        (void)win32_ui_text(win, 16, 26, text, 0x00101010, 0x00DCDCDC);
        for (int i = 0; i < nb; i++) {
            int bx = bx0 + i * (bw_ + gap);
            (void)gui_window_fill(win, bx, oky, bw_, bh, 0x00C0C0C0);
            (void)gui_window_rect(win, bx, oky, bw_, bh, 0x00303030);
            (void)gui_window_rect(win, bx + 1, oky + 1, bw_ - 2, bh - 2, 0x00F0F0F0);
            int tw = (int)strlen(blabel[i]) * 8;
            (void)win32_ui_text(win, bx + (bw_ - tw) / 2, oky + bh / 2 - 4,
                                  blabel[i], 0x00101010, 0x00C0C0C0);
        }
    }
    (void)sys_gui_flip((int)fd);
    if (g_win32_log_input) kprintf("[winpe] MessageBox: '%s' (modal, %d buttons) -- waiting\n", text, nb);

    /* Modal wait: bounded so a missing click can never hang the process. */
    uint64_t hz = pit_hz(); if (!hz) hz = 100;
    uint64_t deadline = pit_ticks() + 20 * hz;     /* ~20s safety cap */
    long result = dflt;
    bool done = false;
    while (!done && pit_ticks() < deadline) {
        struct gui_event ev;
        if (win32_poll_window_event((int)fd, &ev) > 0) {
            if (ev.type == GUI_EV_CLOSE) { result = (kind == MB_OK) ? IDOK : IDCANCEL; break; }
            if (ev.type == GUI_EV_MOUSE_DOWN && ev.y >= oky && ev.y < oky + bh) {
                for (int i = 0; i < nb; i++) {
                    int bx = bx0 + i * (bw_ + gap);
                    if (ev.x >= bx && ev.x < bx + bw_) { result = bid[i]; done = true; break; }
                }
            }
            if (ev.type == GUI_EV_KEY &&
                (ev.key == '\n' || ev.key == '\r' || ev.key == ' ')) { result = dflt; break; }
        }
        sched_yield();
    }
    (void)sys_close((int)fd);            /* tear the box down */
    if (g_win32_log_input) kprintf("[winpe] MessageBox: closed -> %ld\n", result);
    return result;
}

/* ================= C14: dialog templates (DialogBoxParamA) =================
 * The app passes a MAKEINTRESOURCE dialog id; the kernel walks the PE's .rsrc
 * section for the RT_DIALOG resource, parses the DLGTEMPLATE(EX) + each
 * DLGITEMTEMPLATE(EX), builds the dialog window + its controls (reusing the
 * virtual-control system), then -- because a MODAL dialog dispatches to the
 * app's DialogProc (CPL3) -- a user-mode trampoline (pe_loader.c) runs the
 * message loop: it asks the kernel (__toby_dlgsvc) for one message at a time
 * and DispatchMessages each to the DialogProc until EndDialog.
 */
#define DS_SETFONT  0x40u
#define RT_DIALOG   5

static long w32_SendMessageA(uint64_t *a);   /* defined below; used by SendDlgItemMessage */

/* The single active MODAL dialog (DialogBoxParamA). Modeless dialogs
 * (CreateDialogParamA) are ordinary windows in win[] flagged is_dialog. */
static struct {
    int      fd;            /* dialog window fd (0 = none active) */
    uint64_t dlgproc;       /* the app's DialogProc */
    bool     init_pending;  /* WM_INITDIALOG not yet delivered */
    uint64_t init_param;    /* dwInitParam (WM_INITDIALOG lParam) */
    bool     ended;         /* EndDialog called */
    long     result;        /* EndDialog result */
} g_win32_modal;

/* ---- little-endian readers over the copied template buffer (bounds-checked) */
static uint16_t dlg_rd16(const uint8_t *b, int p, int len) {
    if (p < 0 || p + 2 > len) return 0;
    return (uint16_t)(b[p] | ((uint16_t)b[p + 1] << 8));
}
static uint32_t dlg_rd32(const uint8_t *b, int p, int len) {
    if (p < 0 || p + 4 > len) return 0;
    return (uint32_t)b[p] | ((uint32_t)b[p + 1] << 8) |
           ((uint32_t)b[p + 2] << 16) | ((uint32_t)b[p + 3] << 24);
}
/* Read a UTF-16 string into an ASCII buffer (low byte per WCHAR); -> new pos. */
static int dlg_read_wsz(const uint8_t *b, int p, int len, char *out, int outsz) {
    int o = 0;
    while (p + 2 <= len) {
        uint16_t c = dlg_rd16(b, p, len); p += 2;
        if (c == 0) break;
        if (o < outsz - 1) out[o++] = (char)(c & 0xFF);
    }
    if (out && outsz) out[o] = 0;
    return p;
}
/* Skip a sz_Or_Ord (menu/class at the dialog level): 0xFFFF+ord, or a sz. */
static int dlg_skip_szord(const uint8_t *b, int p, int len) {
    uint16_t w = dlg_rd16(b, p, len);
    if (w == 0xFFFF) return p + 4;
    while (p + 2 <= len) { uint16_t c = dlg_rd16(b, p, len); p += 2; if (c == 0) break; }
    return p;
}
/* An item's windowClass: ordinal (0xFFFF+WORD), empty (0x0000), or a sz name. */
static int dlg_read_class(const uint8_t *b, int p, int len, int *ord, char *name, int namesz) {
    *ord = -1; if (name && namesz) name[0] = 0;
    uint16_t w = dlg_rd16(b, p, len);
    if (w == 0xFFFF) { *ord = dlg_rd16(b, p + 2, len); return p + 4; }
    if (w == 0x0000) return p + 2;
    return dlg_read_wsz(b, p, len, name, namesz);
}
/* An item's title: a sz, or 0xFFFF+ord (icon/resource -> empty text). */
static int dlg_read_title(const uint8_t *b, int p, int len, char *out, int outsz) {
    uint16_t w = dlg_rd16(b, p, len);
    if (w == 0xFFFF) { if (out && outsz) out[0] = 0; return p + 4; }
    return dlg_read_wsz(b, p, len, out, outsz);
}
/* Dialog units -> pixels (MS Shell Dlg 8pt baseline ~ 6x13 base units). */
static int dlg_dlu_x(int v) { return v * 3 / 2; }
static int dlg_dlu_y(int v) { return v * 13 / 8; }
/* Map a control class ordinal/name + style to a control kind. */
static int dlg_class_kind(int ord, const char *name, uint32_t style) {
    int base = 0;
    switch (ord) {
    case 0x80: base = WIN32_CTRL_BUTTON;    break;
    case 0x81: base = WIN32_CTRL_EDIT;      break;
    case 0x82: base = WIN32_CTRL_STATIC;    break;
    case 0x83: base = WIN32_CTRL_LISTBOX;   break;
    case 0x84: base = WIN32_CTRL_SCROLLBAR; break;
    case 0x85: base = WIN32_CTRL_COMBOBOX;  break;
    default:   if (name && name[0]) base = win32_ctrl_class(name); break;
    }
    return win32_button_subkind(base, style);
}

/* ---- .rsrc walk (user memory) ---- small copy_from_user readers ---- */
static uint32_t win32_uread32(uint64_t uva) {
    uint32_t v = 0; (void)copy_from_user(&v, (const void *)(uintptr_t)uva, 4); return v;
}
static uint16_t win32_uread16(uint64_t uva) {
    uint16_t v = 0; (void)copy_from_user(&v, (const void *)(uintptr_t)uva, 2); return v;
}
/* In a resource directory, find the entry whose id matches; return the target VA
 * (a subdir or a leaf, rsrc-relative offset added to rsrc_base). */
static uint64_t win32_rsrc_find_id(uint64_t rsrc_base, uint64_t dir, uint32_t id) {
    uint16_t nnamed = win32_uread16(dir + 0x0C);
    uint16_t nid    = win32_uread16(dir + 0x0E);
    uint64_t e = dir + 0x10 + (uint64_t)nnamed * 8;   /* skip named entries */
    for (int i = 0; i < nid; i++, e += 8)
        if (win32_uread32(e + 0) == id)
            return rsrc_base + (win32_uread32(e + 4) & 0x7FFFFFFFu);
    return 0;
}
/* First entry of a directory (used at the language level). */
static uint64_t win32_rsrc_first(uint64_t rsrc_base, uint64_t dir) {
    uint16_t nnamed = win32_uread16(dir + 0x0C);
    uint16_t nid    = win32_uread16(dir + 0x0E);
    if (nnamed + nid == 0) return 0;
    return rsrc_base + (win32_uread32(dir + 0x10 + 4) & 0x7FFFFFFFu);
}
/* Resolve RT_DIALOG id -> (user VA of the template, size). 0 if not found. */
static uint64_t win32_find_dialog_template(struct proc *lead, int dlg_id, uint32_t *out_size) {
    if (!lead || !lead->win_image_base || !lead->win_rsrc_rva) return 0;
    uint64_t base = lead->win_image_base;
    uint64_t rsrc = base + lead->win_rsrc_rva;
    uint64_t td = win32_rsrc_find_id(rsrc, rsrc, RT_DIALOG);
    if (!td) return 0;
    uint64_t idd = win32_rsrc_find_id(rsrc, td, (uint32_t)dlg_id);
    if (!idd) return 0;
    uint64_t leaf = win32_rsrc_first(rsrc, idd);   /* language level -> data entry */
    if (!leaf) return 0;
    uint32_t rva = win32_uread32(leaf + 0);
    uint32_t sz  = win32_uread32(leaf + 4);
    if (out_size) *out_size = sz;
    return rva ? base + rva : 0;
}

/* Fill a dialog window's background + draw its controls + present. */
static void win32_dlg_paint(int fd) {
    struct window *win = win32_hdc_window(fd);
    struct win32_win *w = win32_win_find(fd);
    if (!win || !w) return;
    (void)gui_window_fill(win, 0, 0, w->w, w->h, w->fill_color & 0x00FFFFFFu);
    win32_draw_controls(fd);
    (void)sys_gui_flip(fd);
}

/* Build the dialog window + its controls from the RT_DIALOG template. Returns
 * the dialog window fd, or -1. `modeless` flags a CreateDialogParam dialog. */
static int win32_dlg_build(int dlg_id, uint64_t dlgproc, bool modeless) {
    struct proc *p = current_proc();
    struct proc *lead = (p && p->is_thread) ? proc_lookup(p->tgid) : p;
    if (!lead) lead = p;

    uint32_t tsize = 0;
    uint64_t tmpl = win32_find_dialog_template(lead, dlg_id, &tsize);
    if (!tmpl || tsize == 0) {
        kprintf("[winpe] dialog: RT_DIALOG id=%d not found in .rsrc\n", dlg_id);
        return -1;
    }
    static uint8_t tbuf[2048];
    int tlen = (tsize > sizeof(tbuf)) ? (int)sizeof(tbuf) : (int)tsize;
    if (copy_from_user(tbuf, (const void *)(uintptr_t)tmpl, (size_t)tlen) != 0) return -1;

    int pos = 0;
    bool ex = (dlg_rd16(tbuf, 0, tlen) == 1 && dlg_rd16(tbuf, 2, tlen) == 0xFFFF);
    uint32_t style; uint16_t cdit;
    if (ex) {
        pos = 12;                                   /* dlgVer,sig,helpID,exStyle */
        style = dlg_rd32(tbuf, pos, tlen); pos += 4;
        cdit  = dlg_rd16(tbuf, pos, tlen); pos += 2;
    } else {
        style = dlg_rd32(tbuf, 0, tlen); pos = 8;   /* style, exStyle */
        cdit  = dlg_rd16(tbuf, pos, tlen); pos += 2;
    }
    int dcx = (int16_t)dlg_rd16(tbuf, pos + 4, tlen);
    int dcy = (int16_t)dlg_rd16(tbuf, pos + 6, tlen);
    pos += 8;                                       /* x,y,cx,cy */
    pos = dlg_skip_szord(tbuf, pos, tlen);          /* menu  */
    pos = dlg_skip_szord(tbuf, pos, tlen);          /* class */
    char title[40];
    pos = dlg_read_wsz(tbuf, pos, tlen, title, sizeof(title));   /* title */
    if (style & DS_SETFONT) {
        pos += ex ? 6 : 2;                          /* pointsize(+weight/italic/charset) */
        char face[40]; pos = dlg_read_wsz(tbuf, pos, tlen, face, sizeof(face));
    }

    int cw = dlg_dlu_x(dcx), ch = dlg_dlu_y(dcy);
    if (cw < 80)  cw = 80;  if (cw > 1000) cw = 1000;
    if (ch < 60)  ch = 60;  if (ch > 800)  ch = 800;

    /* Fresh-app boundary (a dialog-only app never calls RegisterClassA). */
    win32_prune_dead_windows();
    if (win32_win_count() == 0) { memset(&g_win32_gui, 0, sizeof(g_win32_gui)); win32_reset_menus_timers(); }
    g_win32_gui.quit = false;

    struct win32_win *wslot = win32_win_alloc();
    if (!wslot) return -1;
    int fd = -1;
    if (cap_check(current_proc(), CAP_GUI, "DialogBox")) {
        struct window *dw = gui_window_create((uint32_t)cw, (uint32_t)ch,
                                              title[0] ? title : "Dialog");
        if (dw) {
            struct file *df = (struct file *)kmalloc(sizeof(*df));
            if (df) {
                memset(df, 0, sizeof(*df));
                df->kind = FILE_KIND_WINDOW; df->win = dw;
                fd = fd_alloc_into(current_proc(), df);
                if (fd < 0) { kfree(df); gui_window_close(dw); }
            } else gui_window_close(dw);
        }
    }
    if (fd < 0) return -1;

    struct proc *cp = current_proc();
    g_win32_gui.tgid = cp ? (cp->is_thread ? cp->tgid : cp->pid) : 0;
    memset(wslot, 0, sizeof(*wslot));
    wslot->fd = fd; wslot->w = cw; wslot->h = ch;
    wslot->wndproc = dlgproc;
    wslot->fill_color = 0x00DCDCDC;     /* dialog face gray */
    wslot->pen_color  = WIN32_PEN_DEFAULT;
    wslot->font_scale = 1; wslot->text_color = 0x00101010; wslot->bk_mode = 2;
    wslot->is_dialog  = true;
    (void)modeless;

    for (int i = 0; i < cdit && pos < tlen; i++) {
        pos = (pos + 3) & ~3;                        /* each item is DWORD-aligned */
        uint32_t istyle, iid;
        if (ex) { pos += 8; istyle = dlg_rd32(tbuf, pos, tlen); pos += 4; }
        else    { istyle = dlg_rd32(tbuf, pos, tlen); pos += 8; }
        int ix  = (int16_t)dlg_rd16(tbuf, pos + 0, tlen);
        int iy  = (int16_t)dlg_rd16(tbuf, pos + 2, tlen);
        int icx = (int16_t)dlg_rd16(tbuf, pos + 4, tlen);
        int icy = (int16_t)dlg_rd16(tbuf, pos + 6, tlen);
        pos += 8;
        if (ex) { iid = dlg_rd32(tbuf, pos, tlen); pos += 4; }
        else    { iid = dlg_rd16(tbuf, pos, tlen); pos += 2; }
        int cord; char cname[24];
        pos = dlg_read_class(tbuf, pos, tlen, &cord, cname, sizeof(cname));
        char itext[40];
        pos = dlg_read_title(tbuf, pos, tlen, itext, sizeof(itext));
        uint16_t extra = dlg_rd16(tbuf, pos, tlen); pos += 2 + extra;   /* creation data */
        int kind = dlg_class_kind(cord, cname, istyle);
        if (!kind) continue;
        int slot = win32_ctrl_make(fd, kind, dlg_dlu_x(ix), dlg_dlu_y(iy),
                                   dlg_dlu_x(icx), dlg_dlu_y(icy),
                                   (int)iid, itext, istyle);
        if (g_win32_log_input && slot >= 0)
            kprintf("[winpe] dialog ctrl: kind=%d id=%d @(%d,%d %dx%d) '%s'\n",
                    kind, (int)iid, dlg_dlu_x(ix), dlg_dlu_y(iy),
                    dlg_dlu_x(icx), dlg_dlu_y(icy), itext);
    }
    return fd;
}

/* BEGIN: build the MODAL dialog, arm WM_INITDIALOG, return hDlg. */
static long win32_dlg_begin(uint64_t hInst, uint64_t lpTemplate, uint64_t parent,
                            uint64_t dlgproc, uint64_t initParam) {
    (void)hInst; (void)parent;
    int dlg_id = (int)(uint32_t)lpTemplate;       /* MAKEINTRESOURCE -> small int */
    int fd = win32_dlg_build(dlg_id, dlgproc, false);
    if (fd < 0) return 0;
    g_win32_modal.fd           = fd;
    g_win32_modal.dlgproc      = dlgproc;
    g_win32_modal.init_pending = true;
    g_win32_modal.init_param   = initParam;
    g_win32_modal.ended        = false;
    g_win32_modal.result       = 0;
    win32_dlg_paint(fd);
    if (g_win32_log_input)
        kprintf("[winpe] dialog: id=%d up (modal); waiting for EndDialog\n", dlg_id);
    return (long)fd;
}

/* PUMP: produce one message for the modal loop. Returns 1 (dispatch *msgptr to
 * the DialogProc) or 0 (the dialog ended -- stop the loop). */
static long win32_dlg_pump(uint64_t msgptr) {
    struct win32_msg m; memset(&m, 0, sizeof(m));
    int fd = g_win32_modal.fd;
    struct win32_win *w = win32_win_find(fd);
    if (fd <= 0 || !w || g_win32_modal.ended) {
        (void)copy_to_user((void *)(uintptr_t)msgptr, &m, sizeof(m));
        return 0;                                   /* ended */
    }
    /* WM_INITDIALOG first. */
    if (g_win32_modal.init_pending) {
        g_win32_modal.init_pending = false;
        win32_stash_wndproc(w);
        m.hwnd = (uint64_t)fd; m.message = WM_INITDIALOG;
        m.wParam = (uint64_t)fd; m.lParam = g_win32_modal.init_param;
        (void)copy_to_user((void *)(uintptr_t)msgptr, &m, sizeof(m));
        return 1;
    }
    /* A queued control notification (WM_COMMAND/WM_VSCROLL/WM_NOTIFY) for us. */
    if (g_win32_gui.cmd_head != g_win32_gui.cmd_tail) {
        int t = g_win32_gui.cmd_tail;
        if ((int)g_win32_gui.cmd_q[t].hwnd == fd) {
            win32_stash_wndproc(w);
            m.hwnd    = g_win32_gui.cmd_q[t].hwnd;
            m.message = g_win32_gui.cmd_q[t].message;
            m.wParam  = g_win32_gui.cmd_q[t].wParam;
            m.lParam  = g_win32_gui.cmd_q[t].lParam;
            g_win32_gui.cmd_tail = (t + 1) % WIN32_CMDQ;
            (void)copy_to_user((void *)(uintptr_t)msgptr, &m, sizeof(m));
            return 1;
        }
    }
    /* Repaint if needed (kernel owns the dialog's painting). */
    if (w->needs_paint) { w->needs_paint = false; win32_dlg_paint(fd); }
    /* One input event -> route to a control, or deliver to the DialogProc. */
    struct gui_event ev;
    if (win32_poll_window_event(fd, &ev) > 0) {
        if (ev.type == GUI_EV_CLOSE) {              /* [X] -> WM_COMMAND IDCANCEL */
            win32_msg_push(fd, WM_COMMAND, (uint64_t)IDCANCEL, 0);
        } else if (!win32_route_control_input(fd, &ev)) {
            if (win32_event_to_msg(fd, &ev, &m)) {
                win32_stash_wndproc(w);
                (void)copy_to_user((void *)(uintptr_t)msgptr, &m, sizeof(m));
                return 1;
            }
        }
    }
    /* Idle: yield + WM_NULL so the loop keeps turning without busy-spinning. */
    sched_yield();
    win32_stash_wndproc(w);
    m.hwnd = (uint64_t)fd; m.message = WM_NULL;
    (void)copy_to_user((void *)(uintptr_t)msgptr, &m, sizeof(m));
    return 1;
}

/* RESULT: tear the modal dialog down + return the EndDialog code. */
static long win32_dlg_result(void) {
    long r  = g_win32_modal.result;
    int  fd = g_win32_modal.fd;
    if (fd > 0) {
        for (int i = 0; i < WIN32_MAX_CTRLS; i++)
            if (g_win32_gui.ctrl[i].in_use && g_win32_gui.ctrl[i].parent_fd == fd)
                g_win32_gui.ctrl[i].in_use = false;
        struct win32_win *w = win32_win_find(fd);
        if (w) w->fd = 0;                            /* retire the window slot */
        (void)sys_close(fd);                         /* remove from the desktop */
    }
    memset(&g_win32_modal, 0, sizeof(g_win32_modal));
    g_win32_gui.quit = false;
    if (g_win32_log_input) kprintf("[winpe] dialog: ended -> result=%ld\n", r);
    return r;
}

/* The dialog service the modal trampoline calls (BEGIN/PUMP/RESULT). Hidden
 * from GetProcAddress consumers -- it's an internal helper, not a real API. */
static long w32_toby_dlgsvc(uint64_t *a) {
    switch (a[0]) {
    case 0: return win32_dlg_begin(a[1], a[2], a[3], a[4], a[5]);
    case 1: return win32_dlg_pump(a[1]);
    case 2: return win32_dlg_result();
    default: return 0;
    }
}

/* user32!EndDialog(hDlg, nResult). Modal: signal the pump to stop with this
 * result. Modeless: destroy the dialog window. */
static long w32_EndDialog(uint64_t *a) {
    int fd = (int)a[0];
    if (g_win32_modal.fd == fd) {
        g_win32_modal.ended  = true;
        g_win32_modal.result = (long)a[1];
        return 1;
    }
    struct win32_win *w = win32_win_find(fd);
    if (w) w->destroy = true;
    return 1;
}

/* user32!CreateDialogParamA -> a MODELESS dialog (the app pumps it with its own
 * GetMessage/DispatchMessage loop). WM_INITDIALOG is queued for that loop. */
static long w32_CreateDialogParamA(uint64_t *a) {
    int dlg_id = (int)(uint32_t)a[1];
    int fd = win32_dlg_build(dlg_id, a[3], true);
    if (fd < 0) return 0;
    win32_dlg_paint(fd);
    win32_msg_push(fd, WM_INITDIALOG, (uint64_t)fd, a[4]);   /* deliver via the app's loop */
    return (long)fd;
}

/* user32!DefDlgProcA(hDlg, msg, wParam, lParam): the default dialog behaviour.
 * WM_CLOSE -> end the dialog (IDCANCEL semantics). */
static long w32_DefDlgProcA(uint64_t *a) {
    uint32_t msg = (uint32_t)a[1];
    int fd = (int)a[0];
    if (msg == WM_CLOSE) {
        if (g_win32_modal.fd == fd) { g_win32_modal.ended = true; g_win32_modal.result = 0; }
        else { struct win32_win *w = win32_win_find(fd); if (w) w->destroy = true; }
    }
    return 0;
}

/* ---- dialog item (control-by-id) helpers ---- */
/* Find a control by (dialog hwnd, item id). */
static struct win32_ctrl *win32_dlg_item(int dlg_fd, int id) {
    for (int i = 0; i < WIN32_MAX_CTRLS; i++) {
        struct win32_ctrl *c = &g_win32_gui.ctrl[i];
        if (c->in_use && c->parent_fd == dlg_fd && c->id == id) return c;
    }
    return NULL;
}
/* user32!GetDlgItem(hDlg, id) -> the control's HWND token (or 0). */
static long w32_GetDlgItem(uint64_t *a) {
    struct win32_ctrl *c = win32_dlg_item((int)a[0], (int)a[1]);
    if (!c) return 0;
    return (long)(WIN32_CTRL_TAG | (uint64_t)(c - g_win32_gui.ctrl));
}
/* user32!GetDlgCtrlID(hCtrl) -> the control's id. */
static long w32_GetDlgCtrlID(uint64_t *a) {
    struct win32_ctrl *c = win32_ctrl_find(a[0]);
    return c ? c->id : 0;
}
/* user32!SetDlgItemTextA(hDlg, id, text) -> TRUE. */
static long w32_SetDlgItemTextA(uint64_t *a) {
    struct win32_ctrl *c = win32_dlg_item((int)a[0], (int)a[1]);
    if (!c) return 0;
    c->text[0] = 0;
    if (a[2]) (void)strncpy_from_user(c->text, (const char *)(uintptr_t)a[2], sizeof(c->text));
    win32_dlg_paint((int)a[0]);
    return 1;
}
/* user32!GetDlgItemTextA(hDlg, id, buf, max) -> length copied. */
static long w32_GetDlgItemTextA(uint64_t *a) {
    struct win32_ctrl *c = win32_dlg_item((int)a[0], (int)a[1]);
    int max = (int)(int32_t)a[3];
    if (max <= 0) return 0;
    int n = 0;
    if (c) while (n < (int)sizeof(c->text) - 1 && c->text[n]) n++;
    if (n > max - 1) n = max - 1;
    if (c && n) (void)copy_to_user((void *)(uintptr_t)a[2], c->text, n);
    char z = 0; (void)copy_to_user((void *)(uintptr_t)(a[2] + n), &z, 1);
    return n;
}
/* user32!CheckDlgButton(hDlg, id, check) -> TRUE. */
static long w32_CheckDlgButton(uint64_t *a) {
    struct win32_ctrl *c = win32_dlg_item((int)a[0], (int)a[1]);
    if (!c) return 0;
    c->checked = (a[2] != 0);
    win32_dlg_paint((int)a[0]);
    return 1;
}
/* user32!IsDlgButtonChecked(hDlg, id) -> BST_CHECKED(1)/BST_UNCHECKED(0). */
static long w32_IsDlgButtonChecked(uint64_t *a) {
    struct win32_ctrl *c = win32_dlg_item((int)a[0], (int)a[1]);
    return (c && c->checked) ? 1 : 0;
}
/* user32!CheckRadioButton(hDlg, first, last, check): set one radio in [first,
 * last], clear the others in that id range. */
static long w32_CheckRadioButton(uint64_t *a) {
    int dlg = (int)a[0], lo = (int)a[1], hi = (int)a[2], sel = (int)a[3];
    for (int i = 0; i < WIN32_MAX_CTRLS; i++) {
        struct win32_ctrl *c = &g_win32_gui.ctrl[i];
        if (c->in_use && c->parent_fd == dlg && c->id >= lo && c->id <= hi)
            c->checked = (c->id == sel);
    }
    win32_dlg_paint(dlg);
    return 1;
}
/* user32!SendDlgItemMessageA(hDlg, id, msg, wParam, lParam) -> result. */
static long w32_SendDlgItemMessageA(uint64_t *a) {
    struct win32_ctrl *c = win32_dlg_item((int)a[0], (int)a[1]);
    if (!c) return 0;
    uint64_t sub[4] = { WIN32_CTRL_TAG | (uint64_t)(c - g_win32_gui.ctrl), a[2], a[3], a[4] };
    return w32_SendMessageA(sub);
}
/* kernel32!lstrlenA(s) -> length (used by GUI apps to size TextOut strings). */
static long w32_lstrlenA(uint64_t *a) {
    if (!a[0]) return 0;
    char s[256];
    long n = strncpy_from_user(s, (const char *)(uintptr_t)a[0], sizeof(s));
    return n < 0 ? 0 : (long)strlen(s);
}
/* kernel32!lstrcmpA(a, b) -> <0/0/>0 (used by dialog apps to compare text). */
static long w32_lstrcmpA(uint64_t *a) {
    char s1[128], s2[128];
    s1[0] = s2[0] = 0;
    if (a[0]) (void)strncpy_from_user(s1, (const char *)(uintptr_t)a[0], sizeof(s1));
    if (a[1]) (void)strncpy_from_user(s2, (const char *)(uintptr_t)a[1], sizeof(s2));
    return (long)strcmp(s1, s2);
}

/* ---- C11: more GDI (pens, lines, shapes) ----
 * These map a GDI device context (HDC == HWND == fd) onto the window's drawing
 * backend (gui_window_line/_rect/_circle_outline). A per-window "current pen"
 * (set by SelectObject(CreatePen)) and "current position" (MoveToEx) give the
 * stateful MoveToEx/LineTo pair its expected behaviour. */

/* Resolve an HDC fd to the underlying compositor window (for the gui_window_*
 * drawing calls), or NULL. */
static struct window *win32_hdc_window(int fd) {
    struct file *f = fd_lookup(fd);
    return (f && f->kind == FILE_KIND_WINDOW) ? f->win : NULL;
}

/* gdi32!CreatePen(style, width, COLORREF) -> HPEN, a tagged token carrying the
 * colour (line width is not honoured by the 1px backend). */
static long w32_CreatePen(uint64_t *a) {
    return (long)(WIN32_PEN_TAG | (uint64_t)((uint32_t)a[2] & 0xFFFFFFu));
}

/* C12: select a bitmap into a memory DC -- forward-declared so SelectObject can
 * route to it. Defined with the bitmap shims below. */
static long win32_memdc_select_bitmap(uint64_t hdc, uint64_t hbmp);

/* gdi32!SelectObject(hdc, hgdiobj) -> the previously-selected object. A pen token
 * sets a window DC's current pen colour; a bitmap selected into a memory DC is
 * routed to the bitmap path (C12); other objects are passed back unchanged. */
static long w32_SelectObject(uint64_t *a) {
    int fd = (int)a[0];
    uint64_t obj = a[1];
    if ((a[0] & WIN32_TAG_MASK) == WIN32_MEMDC_TAG)        /* memory DC + bitmap */
        return win32_memdc_select_bitmap(a[0], obj);
    struct win32_win *w = win32_win_find(fd);
    if (w && (obj & WIN32_TAG_MASK) == WIN32_PEN_TAG) {
        uint64_t old = WIN32_PEN_TAG |
                       (uint64_t)win32_colorref_to_xrgb(w->pen_color);
        w->pen_color = win32_colorref_to_xrgb((uint32_t)(obj & 0xFFFFFFu));
        return (long)old;
    }
    if (w && (obj & WIN32_TAG_MASK) == WIN32_FONT_TAG) {   /* C13/C14b/C18c: select a font */
        uint64_t old = WIN32_FONT_TAG |
            ((uint64_t)(w->font_face & WIN32_FONT_FACE_MASK) << WIN32_FONT_FACE_SHIFT) |
            (uint64_t)(uint32_t)(w->font_px ? w->font_px : 16);
        int px = (int)(obj & 0xFFFF); if (px < 1) px = 16; if (px > 200) px = 200;
        w->font_px = px;                                  /* C14b: TTF pixel height */
        w->font_face = (int)((obj >> WIN32_FONT_FACE_SHIFT) & WIN32_FONT_FACE_MASK); /* C18c */
        w->font_scale = (px + 4) / 8;                     /* bitmap fallback scale  */
        if (w->font_scale < 1) w->font_scale = 1;
        if (w->font_scale > 8) w->font_scale = 8;
        return (long)old;
    }
    return (long)obj;
}

/* gdi32!MoveToEx(hdc, x, y, &POINT|NULL) -> TRUE. Sets the current position;
 * stores the previous point if a POINT is supplied. */
static long w32_MoveToEx(uint64_t *a) {
    int fd = (int)a[0];
    struct win32_win *w = win32_win_find(fd);
    if (!w) return 0;
    uint64_t pold = a[3];
    if (pold) {
        int32_t op[2] = { w->cur_x, w->cur_y };
        (void)copy_to_user((void *)(uintptr_t)pold, op, sizeof(op));
    }
    w->cur_x = (int)(int32_t)a[1];
    w->cur_y = (int)(int32_t)a[2];
    return 1;
}

/* gdi32!LineTo(hdc, x, y) -> TRUE. Draws from the current position to (x,y) with
 * the current pen, then moves the current position there. */
static long w32_LineTo(uint64_t *a) {
    int fd = (int)a[0];
    struct win32_win *w   = win32_win_find(fd);
    struct window    *win = win32_hdc_window(fd);
    if (!w || !win) return 0;
    int x = (int)(int32_t)a[1], y = (int)(int32_t)a[2];
    (void)gui_window_line(win, w->cur_x, w->cur_y, x, y, w->pen_color);
    w->cur_x = x; w->cur_y = y;
    return 1;
}

/* gdi32!Rectangle(hdc, l, t, r, b) -> TRUE. Outlines the rectangle in the
 * current pen colour. */
static long w32_Rectangle(uint64_t *a) {
    int fd = (int)a[0];
    struct win32_win *w   = win32_win_find(fd);
    struct window    *win = win32_hdc_window(fd);
    if (!w || !win) return 0;
    int l = (int)(int32_t)a[1], t = (int)(int32_t)a[2];
    int r = (int)(int32_t)a[3], b = (int)(int32_t)a[4];
    (void)gui_window_rect(win, l, t, r - l, b - t, w->pen_color);
    return 1;
}

/* gdi32!Ellipse(hdc, l, t, r, b) -> TRUE. Drawn as a circle inscribed in the
 * bounding box (the backend draws circles, not true ellipses -- honest
 * approximation when the box isn't square). */
static long w32_Ellipse(uint64_t *a) {
    int fd = (int)a[0];
    struct win32_win *w   = win32_win_find(fd);
    struct window    *win = win32_hdc_window(fd);
    if (!w || !win) return 0;
    int l = (int)(int32_t)a[1], t = (int)(int32_t)a[2];
    int r = (int)(int32_t)a[3], b = (int)(int32_t)a[4];
    int cx = (l + r) / 2, cy = (t + b) / 2;
    int rad = ((r - l) + (b - t)) / 4;          /* mean of the half-extents */
    if (rad < 1) rad = 1;
    (void)gui_window_circle_outline(win, cx, cy, rad, w->pen_color);
    return 1;
}

/* gdi32!SetPixel(hdc, x, y, COLORREF) -> the colour set (or -1 on bad hdc). */
static long w32_SetPixel(uint64_t *a) {
    int fd = (int)a[0];
    if (!win32_win_find(fd)) return -1;
    int x = (int)(int32_t)a[1], y = (int)(int32_t)a[2];
    uint32_t cr = (uint32_t)a[3];
    uint32_t whlen = 1u | (1u << 16);           /* 1x1 */
    (void)sys_gui_fill(fd, x, y, whlen, win32_colorref_to_xrgb(cr & 0xFFFFFFu));
    return (long)(cr & 0xFFFFFFu);
}

/* ---- C11: more file ops (seek, size, delete, mkdir, attrs, dir enum) ----
 * Built on the C5 base (HANDLE == fd, a "X:" drive -> the writable /data mount).
 * Path-taking ops translate the Windows path; some stage it into the CRT scratch
 * for the user-pointer syscalls, others call the kernel-side VFS directly. */

/* Translate a Windows path into an absolute tobyOS kernel path: a drive letter
 * "X:" -> the writable /data mount, '\' -> '/'. Returns the length, or -1. */
static int win32_translate_path_k(const char *wpath_user, char *kp, int cap) {
    char wpath[ABI_PATH_MAX];
    if (strncpy_from_user(wpath, wpath_user, sizeof(wpath)) < 0) return -1;
    wpath[sizeof(wpath) - 1] = '\0';
    int si = 0, di = 0;
    if (wpath[0] && wpath[1] == ':') {
        for (const char *r = "/data"; *r && di < cap - 1; r++) kp[di++] = *r;
        si = 2;
        if (wpath[si] != '\\' && wpath[si] != '/' && di < cap - 1) kp[di++] = '/';
    }
    for (; wpath[si] && di < cap - 1; si++)
        kp[di++] = (wpath[si] == '\\') ? '/' : wpath[si];
    kp[di] = '\0';
    return di;
}

/* Translate + stage a Windows path into the CRT-page scratch; returns a USER
 * pointer to it (for sys_open/unlink/mkdir, which expect a user path), or 0. */
static uint64_t win32_stage_path(const char *wpath_user) {
    char kp[ABI_PATH_MAX];
    int di = win32_translate_path_k(wpath_user, kp, sizeof(kp));
    if (di < 0) return 0;
    uint64_t ubuf = WIN32_CRT_DATA_BASE + WIN32_CRT_PATHBUF;
    if (copy_to_user((void *)(uintptr_t)ubuf, kp, (size_t)di + 1) != 0) return 0;
    return ubuf;
}

/* kernel32!SetFilePointer(hFile, lDist, &lDistHigh|NULL, dwMoveMethod) -> new
 * low-32 file position (FILE_BEGIN/CURRENT/END = 0/1/2 == SEEK_SET/CUR/END). */
static long w32_SetFilePointer(uint64_t *a) {
    int fd = (int)a[0];
    int64_t  dist   = (int64_t)(int32_t)a[1];     /* low 32, signed */
    uint32_t method = (uint32_t)a[3];
    int whence = (method <= 2) ? (int)method : ABI_SEEK_SET;
    long pos = sys_lseek(fd, dist, whence);
    if (pos < 0) return (long)0xFFFFFFFFu;         /* INVALID_SET_FILE_POINTER */
    return (long)(uint32_t)pos;
}

/* kernel32!GetFileSize(hFile, &lpFileSizeHigh|NULL) -> low-32 size. */
static long w32_GetFileSize(uint64_t *a) {
    struct file *f = fd_lookup((int)a[0]);
    if (!f || f->kind != FILE_KIND_VFS) return (long)0xFFFFFFFFu; /* INVALID_FILE_SIZE */
    uint64_t sz = f->vfs.size;
    if (a[1]) { uint32_t hi = (uint32_t)(sz >> 32);
                (void)copy_to_user((void *)(uintptr_t)a[1], &hi, 4); }
    return (long)(uint32_t)sz;
}

/* kernel32!DeleteFileA(path) -> TRUE on success. */
static long w32_DeleteFileA(uint64_t *a) {
    uint64_t up = win32_stage_path((const char *)(uintptr_t)a[0]);
    return (up && sys_unlink((const char *)(uintptr_t)up) == 0) ? 1 : 0;
}

/* kernel32!CreateDirectoryA(path, &SECURITY_ATTRIBUTES|NULL) -> TRUE. */
static long w32_CreateDirectoryA(uint64_t *a) {
    uint64_t up = win32_stage_path((const char *)(uintptr_t)a[0]);
    return (up && sys_mkdir((const char *)(uintptr_t)up, 0755) == 0) ? 1 : 0;
}

/* ================= C18d: shell32 ================= *
 * Path / launch / tray shims. CSIDL folder ids map to C:\ paths (which the file
 * shims already route to the writable /data mount), ShellExecuteA enqueues a
 * desktop launch via the SAFE pid-0 launch queue (a shim must never proc_spawn
 * directly -- see sys_exec), and Shell_NotifyIcon/DragQueryFile are minimal
 * (tobyOS has no system tray and delivers no OLE drag-drop). */

/* CSIDL (low byte; CSIDL_FLAG_CREATE 0x8000 etc. live in the high bits) -> a
 * Windows path under C:\ . Unknown ids fall back to the user profile. */
static const char *win32_csidl_path(int csidl) {
    switch (csidl & 0xFF) {
    case 0x00: case 0x10: return "C:\\Users\\toby\\Desktop";          /* DESKTOP(DIRECTORY) */
    case 0x02: case 0x0b: return "C:\\Users\\toby\\Start Menu";       /* PROGRAMS / STARTMENU */
    case 0x05: return "C:\\Users\\toby\\Documents";                   /* PERSONAL          */
    case 0x06: return "C:\\Users\\toby\\Favorites";                   /* FAVORITES         */
    case 0x0d: return "C:\\Users\\toby\\Music";                       /* MYMUSIC           */
    case 0x0e: return "C:\\Users\\toby\\Videos";                      /* MYVIDEO           */
    case 0x1a: return "C:\\Users\\toby\\AppData\\Roaming";            /* APPDATA           */
    case 0x1c: return "C:\\Users\\toby\\AppData\\Local";              /* LOCAL_APPDATA     */
    case 0x23: return "C:\\ProgramData";                              /* COMMON_APPDATA    */
    case 0x24: return "C:\\Windows";                                  /* WINDOWS           */
    case 0x25: return "C:\\Windows\\System32";                        /* SYSTEM            */
    case 0x26: return "C:\\Program Files";                            /* PROGRAM_FILES     */
    case 0x27: return "C:\\Users\\toby\\Pictures";                    /* MYPICTURES        */
    case 0x28: return "C:\\Users\\toby";                              /* PROFILE           */
    case 0x2b: return "C:\\Program Files (x86)";                      /* PROGRAM_FILESX86  */
    case 0x2e: return "C:\\Users\\Public\\Documents";                /* COMMON_DOCUMENTS  */
    default:   return "C:\\Users\\toby";
    }
}

/* mkdir -p over a translated /data path (creates each '/'-separated level). */
static void win32_mkdir_p_k(const char *kp) {
    char tmp[ABI_PATH_MAX];
    int n = 0;
    for (; kp[n] && n < (int)sizeof(tmp) - 1; n++) tmp[n] = kp[n];
    tmp[n] = '\0';
    for (int i = 1; i < n; i++) {
        if (tmp[i] == '/') { tmp[i] = '\0'; (void)vfs_mkdir(tmp); tmp[i] = '/'; }
    }
    (void)vfs_mkdir(tmp);                  /* ignore EEXIST at every level */
}

/* Copy a CSIDL's C:\ path to a user buffer, optionally creating the dir tree
 * (under /data). Returns 1 on success, 0 on a bad buffer. */
static long win32_shell_folder(uint64_t upath, int csidl, int create) {
    if (!upath) return 0;
    const char *p = win32_csidl_path(csidl);
    size_t n = 0; while (p[n]) n++;
    if (copy_to_user((void *)(uintptr_t)upath, p, n + 1) != 0) return 0;
    if (create || (csidl & 0x8000)) {      /* CSIDL_FLAG_CREATE */
        char kp[ABI_PATH_MAX];
        int di = 0;
        for (const char *r = "/data"; *r; r++) kp[di++] = *r;
        for (size_t i = 2; i < n && di < (int)sizeof(kp) - 1; i++)   /* skip "C:" */
            kp[di++] = (p[i] == '\\') ? '/' : p[i];
        kp[di] = '\0';
        win32_mkdir_p_k(kp);
    }
    return 1;
}

/* shell32!SHGetFolderPathA(hwnd, csidl, hToken, dwFlags, LPSTR pszPath) -> HRESULT. */
static long w32_SHGetFolderPathA(uint64_t *a) {
    return win32_shell_folder(a[4], (int)(uint32_t)a[1], 0)
               ? 0 /* S_OK */ : (long)0x80070057 /* E_INVALIDARG */;
}
/* shell32!SHGetSpecialFolderPathA(hwnd, LPSTR pszPath, csidl, BOOL fCreate) -> BOOL. */
static long w32_SHGetSpecialFolderPathA(uint64_t *a) {
    return win32_shell_folder(a[1], (int)(uint32_t)a[2], (int)(uint32_t)a[3]);
}

/* shell32!ShellExecuteA(hwnd, lpOp, lpFile, lpParams, lpDir, nShow) -> HINSTANCE.
 * Return >32 = success. A *.exe target is launched for real via the safe pid-0
 * desktop launch queue (/bin/<basename>); other verbs are accepted optimistically
 * (tobyOS has no file-type associations), matching ShellExecute's fire-and-forget
 * contract (it returns success before the target has necessarily started). */
static long w32_ShellExecuteA(uint64_t *a) {
    char file[ABI_PATH_MAX], params[256];
    if (strncpy_from_user(file, (const char *)(uintptr_t)a[2], sizeof(file)) < 0)
        return 2;   /* SE_ERR_FNF (<32 = error) */
    file[sizeof(file) - 1] = 0;
    params[0] = 0;
    if (a[3]) {
        (void)strncpy_from_user(params, (const char *)(uintptr_t)a[3], sizeof(params));
        params[sizeof(params) - 1] = 0;
    }
    int len = 0; while (file[len]) len++;
    int bs = 0; for (int i = len - 1; i >= 0; i--) if (file[i]=='\\'||file[i]=='/'){ bs=i+1; break; }
    const char *base = file + bs; int blen = len - bs;
    int is_exe = blen > 4 && base[blen-4]=='.' && (base[blen-3]|32)=='e' &&
                 (base[blen-2]|32)=='x' && (base[blen-1]|32)=='e';
    if (is_exe) {
        char path[ABI_PATH_MAX]; int di = 0;
        for (const char *r = "/bin/"; *r && di < (int)sizeof(path)-1; r++) path[di++] = *r;
        for (int i = 0; i < blen && di < (int)sizeof(path)-1; i++) path[di++] = base[i];
        path[di] = 0;
        int enq = gui_launch_enqueue_arg(path, params[0] ? params : 0);
        kprintf("[c18d] ShellExecuteA open '%s' -> launch %s (enqueue=%d)\n", file, path, enq);
    } else {
        kprintf("[c18d] ShellExecuteA open '%s' (no assoc)\n", file);
    }
    return 42;   /* >32 = success */
}

/* shell32!Shell_NotifyIconA(dwMessage, PNOTIFYICONDATA) -> BOOL. tobyOS has no
 * system tray; accept add/modify/delete so apps that show a tray icon proceed. */
static long w32_Shell_NotifyIconA(uint64_t *a) {
    kprintf("[c18d] Shell_NotifyIconA msg=%u (no tray; accepted)\n", (uint32_t)a[0]);
    return 1;   /* TRUE */
}

/* shell32!DragQueryFileA(hDrop, iFile, lpszFile, cch) -> file count / chars.
 * tobyOS delivers no OLE drag-drop, so any HDROP reports zero files. */
static long w32_DragQueryFileA(uint64_t *a) {
    if ((uint32_t)a[1] != 0xFFFFFFFFu && a[2] && (uint32_t)a[3] > 0) {
        char z = 0; (void)copy_to_user((void *)(uintptr_t)a[2], &z, 1);
    }
    return 0;   /* 0 files (query) / 0 chars (copy) */
}

/* kernel32!GetFileAttributesA(path) -> FILE_ATTRIBUTE_* or INVALID(0xFFFFFFFF).
 * Uses the kernel-side VFS stat directly (no user marshalling needed). */
static long w32_GetFileAttributesA(uint64_t *a) {
    char kp[ABI_PATH_MAX];
    if (win32_translate_path_k((const char *)(uintptr_t)a[0], kp, sizeof(kp)) < 0)
        return (long)0xFFFFFFFFu;
    struct vfs_stat vs;
    if (vfs_stat(kp, &vs) != VFS_OK) return (long)0xFFFFFFFFu; /* INVALID_FILE_ATTRIBUTES */
    return (vs.type == VFS_TYPE_DIR) ? 0x10 /* DIRECTORY */ : 0x80 /* NORMAL */;
}

/* ---- FindFirstFile/FindNextFile/FindClose: directory enumeration ----
 * A find HANDLE is a tagged token (high byte 0x7E) carrying a slot in a small
 * table of open VFS directory cursors. */
#define WIN32_FIND_TAG    0x7E00000000000000ull
#define WIN32_MAX_FINDS   4
static struct { bool in_use; struct vfs_dir dir; } g_win32_find[WIN32_MAX_FINDS];

/* Fill a WIN32_FIND_DATAA at user `out` from a VFS directory entry (we set only
 * the fields apps commonly read: attributes, low size, cFileName@0x2C). */
static void win32_fill_find_data(uint64_t out, const struct vfs_dirent *e) {
    uint8_t b[0x140];
    memset(b, 0, sizeof(b));
    uint32_t attr = (e->type == VFS_TYPE_DIR) ? 0x10u : 0x80u;
    memcpy(&b[0x00], &attr, 4);                  /* dwFileAttributes */
    uint32_t lo = (uint32_t)e->size;
    memcpy(&b[0x20], &lo, 4);                     /* nFileSizeLow */
    int n = 0;
    for (; e->name[n] && n < 259; n++) b[0x2C + n] = (uint8_t)e->name[n];
    b[0x2C + n] = 0;                              /* cFileName[MAX_PATH] @0x2C */
    (void)copy_to_user((void *)(uintptr_t)out, b, 0x2C + 260);
}

/* kernel32!FindFirstFileA(pattern, &WIN32_FIND_DATAA) -> find HANDLE or INVALID.
 * The trailing wildcard is stripped to yield the directory to enumerate. */
static long w32_FindFirstFileA(uint64_t *a) {
    char kp[ABI_PATH_MAX];
    int di = win32_translate_path_k((const char *)(uintptr_t)a[0], kp, sizeof(kp));
    if (di < 0) return (long)WIN32_INVALID_HANDLE;
    int cut = 0;                                 /* strip "/<wildcard>" -> dir */
    for (int i = di - 1; i >= 0; i--) if (kp[i] == '/') { cut = i; break; }
    kp[cut ? cut : 1] = '\0';
    int slot = -1;
    for (int i = 0; i < WIN32_MAX_FINDS; i++) if (!g_win32_find[i].in_use) { slot = i; break; }
    if (slot < 0) return (long)WIN32_INVALID_HANDLE;
    if (vfs_opendir(kp, &g_win32_find[slot].dir) != VFS_OK) return (long)WIN32_INVALID_HANDLE;
    struct vfs_dirent e;
    if (vfs_readdir(&g_win32_find[slot].dir, &e) != VFS_OK) {
        vfs_closedir(&g_win32_find[slot].dir);
        return (long)WIN32_INVALID_HANDLE;
    }
    g_win32_find[slot].in_use = true;
    win32_fill_find_data(a[1], &e);
    return (long)(WIN32_FIND_TAG | (uint64_t)slot);
}

/* kernel32!FindNextFileA(hFind, &WIN32_FIND_DATAA) -> TRUE, FALSE at end. */
static long w32_FindNextFileA(uint64_t *a) {
    if ((a[0] & WIN32_BRUSH_MASK) != WIN32_FIND_TAG) return 0;
    int slot = (int)(a[0] & 0xFF);
    if (slot < 0 || slot >= WIN32_MAX_FINDS || !g_win32_find[slot].in_use) return 0;
    struct vfs_dirent e;
    if (vfs_readdir(&g_win32_find[slot].dir, &e) != VFS_OK) return 0;
    win32_fill_find_data(a[1], &e);
    return 1;
}

/* kernel32!FindClose(hFind) -> TRUE. */
static long w32_FindClose(uint64_t *a) {
    if ((a[0] & WIN32_BRUSH_MASK) != WIN32_FIND_TAG) return 0;
    int slot = (int)(a[0] & 0xFF);
    if (slot >= 0 && slot < WIN32_MAX_FINDS && g_win32_find[slot].in_use) {
        vfs_closedir(&g_win32_find[slot].dir);
        g_win32_find[slot].in_use = false;
    }
    return 1;
}

/* ---- C12: bitmaps + BitBlt ----
 * A memory DC + bitmap let an app compose an off-screen image and BitBlt it onto
 * a window. A bitmap is a kmalloc'd 32bpp pixel buffer (DWORDs are 0x00RRGGBB,
 * which IS the tobyOS framebuffer's XRGB -- no conversion); a memory DC just
 * references a selected bitmap. BitBlt copies the source bitmap (or a sub-rect)
 * to the destination window via gui_window_blit. */
#define WIN32_MAX_BITMAPS 8
#define WIN32_MAX_MEMDC   4
static struct { bool in_use; int w, h; uint32_t *pixels; } g_win32_bmp[WIN32_MAX_BITMAPS];
static struct { bool in_use; int bmp; } g_win32_memdc[WIN32_MAX_MEMDC];

/* gdi32!CreateCompatibleDC(hdc) -> a memory HDC (tagged token), no bitmap yet. */
static long w32_CreateCompatibleDC(uint64_t *a) {
    (void)a;
    for (int i = 0; i < WIN32_MAX_MEMDC; i++)
        if (!g_win32_memdc[i].in_use) {
            g_win32_memdc[i].in_use = true; g_win32_memdc[i].bmp = -1;
            return (long)(WIN32_MEMDC_TAG | (uint64_t)i);
        }
    return 0;
}

/* Allocate a bitmap slot with a w*h pixel buffer; copies `bits` (user 32bpp) if
 * non-NULL, else zero-fills. Returns the HBITMAP token or 0. */
static long win32_bitmap_alloc(int w, int h, uint64_t bits_user) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    for (int i = 0; i < WIN32_MAX_BITMAPS; i++) {
        if (g_win32_bmp[i].in_use) continue;
        uint32_t *px = (uint32_t *)kmalloc((size_t)w * h * 4u);
        if (!px) return 0;
        if (bits_user) {
            if (copy_from_user(px, (const void *)(uintptr_t)bits_user,
                               (size_t)w * h * 4u) != 0) { kfree(px); return 0; }
        } else {
            memset(px, 0, (size_t)w * h * 4u);
        }
        g_win32_bmp[i].in_use = true; g_win32_bmp[i].w = w; g_win32_bmp[i].h = h;
        g_win32_bmp[i].pixels = px;
        return (long)(WIN32_BITMAP_TAG | (uint64_t)i);
    }
    return 0;
}

/* gdi32!CreateBitmap(w, h, planes, bpp, bits) -> HBITMAP (32bpp DWORD bits). */
static long w32_CreateBitmap(uint64_t *a) {
    return win32_bitmap_alloc((int)(int32_t)a[0], (int)(int32_t)a[1], a[4]);
}
/* gdi32!CreateCompatibleBitmap(hdc, w, h) -> HBITMAP (zero-filled). */
static long w32_CreateCompatibleBitmap(uint64_t *a) {
    return win32_bitmap_alloc((int)(int32_t)a[1], (int)(int32_t)a[2], 0);
}

/* SelectObject(memdc, bitmap) routing target. Returns the previously-selected
 * bitmap handle (or 0). */
static long win32_memdc_select_bitmap(uint64_t hdc, uint64_t hbmp) {
    int d = (int)(hdc & 0xFF);
    if (d < 0 || d >= WIN32_MAX_MEMDC || !g_win32_memdc[d].in_use) return 0;
    int old = g_win32_memdc[d].bmp;
    if ((hbmp & WIN32_TAG_MASK) == WIN32_BITMAP_TAG)
        g_win32_memdc[d].bmp = (int)(hbmp & 0xFF);
    return (old >= 0) ? (long)(WIN32_BITMAP_TAG | (uint64_t)old) : 0;
}

/* DeleteObject(bitmap) routing target. */
static void win32_bitmap_free(uint64_t hbmp) {
    int b = (int)(hbmp & 0xFF);
    if (b < 0 || b >= WIN32_MAX_BITMAPS || !g_win32_bmp[b].in_use) return;
    if (g_win32_bmp[b].pixels) kfree(g_win32_bmp[b].pixels);
    g_win32_bmp[b].pixels = 0; g_win32_bmp[b].in_use = false;
}

/* gdi32!DeleteDC(memdc) -> TRUE. */
static long w32_DeleteDC(uint64_t *a) {
    int d = (int)(a[0] & 0xFF);
    if ((a[0] & WIN32_TAG_MASK) == WIN32_MEMDC_TAG &&
        d >= 0 && d < WIN32_MAX_MEMDC)
        g_win32_memdc[d].in_use = false;
    return 1;
}

/* gdi32!BitBlt(hdcDest, x, y, w, h, hdcSrc, sx, sy, rop) -> TRUE. Copies a w*h
 * sub-rect (origin sx,sy) of the source memory DC's selected bitmap onto the
 * destination window at (x,y). dwRop is treated as SRCCOPY (a8=rop). */
static long w32_BitBlt(uint64_t *a) {
    int destfd = (int)a[0];
    int x = (int)(int32_t)a[1], y = (int)(int32_t)a[2];
    int w = (int)(int32_t)a[3], h = (int)(int32_t)a[4];
    uint64_t hsrc = a[5];
    int sx = (int)(int32_t)a[6], sy = (int)(int32_t)a[7];
    struct window *win = win32_hdc_window(destfd);
    if (!win || (hsrc & WIN32_TAG_MASK) != WIN32_MEMDC_TAG) return 0;
    int d = (int)(hsrc & 0xFF);
    if (d < 0 || d >= WIN32_MAX_MEMDC || !g_win32_memdc[d].in_use) return 0;
    int b = g_win32_memdc[d].bmp;
    if (b < 0 || b >= WIN32_MAX_BITMAPS || !g_win32_bmp[b].in_use) return 0;
    if (w <= 0 || h <= 0) return 0;
    int bw = g_win32_bmp[b].w, bh = g_win32_bmp[b].h;
    /* Extract the (sx,sy,w,h) sub-rect into a contiguous temp buffer (the
     * bitmap rows aren't contiguous for a sub-rect) then blit. */
    uint32_t *tmp = (uint32_t *)kmalloc((size_t)w * h * 4u);
    if (!tmp) return 0;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int srcx = sx + c, srcy = sy + r;
            tmp[r * w + c] = (srcx >= 0 && srcx < bw && srcy >= 0 && srcy < bh)
                             ? g_win32_bmp[b].pixels[srcy * bw + srcx] : 0;
        }
    }
    (void)gui_window_blit(win, x, y, w, h, tmp);
    kfree(tmp);
    return 1;
}

/* ---- C12: BUTTON / EDIT controls (drawing, input routing, text) ----
 * Controls are virtual children drawn into the parent's client backbuffer; the
 * kernel draws + handles them, the app only gets WM_COMMAND notifications. */

/* Scrollbar geometry: square arrow buttons of side = width, clamped. */
static int win32_sb_arrow(const struct win32_ctrl *c) {
    int a = c->w; if (a > 18) a = 18; if (a > c->h / 2) a = c->h / 2; return a;
}
/* Thumb y for a vertical scrollbar at the current pos. */
static int win32_sb_thumb_y(const struct win32_ctrl *c) {
    int ah = win32_sb_arrow(c);
    int track = c->h - 2 * ah - 16;           /* 16 = thumb height */
    if (track < 0) track = 0;
    int span = c->sb_max - c->sb_min; if (span <= 0) span = 1;
    int p = c->sb_pos - c->sb_min; if (p < 0) p = 0; if (p > span) p = span;
    return c->y + ah + (track * p) / span;
}

/* Pick a label text colour that contrasts with the parent background: dark text
 * on a light bg (e.g. a dialog's gray face), light text on a dark bg (the C13
 * app windows). Used for the controls whose label sits directly on the bg. */
static uint32_t win32_label_fg(uint32_t bg) {
    uint32_t r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    uint32_t lum = (r * 2 + g * 3 + b) / 6;
    return (lum > 140) ? 0x00101010u : 0x00E8E8E8u;
}

/* Draw all of a parent window's controls into its backbuffer (caller flips). */
static void win32_draw_controls(int parent_fd) {
    struct window *win = win32_hdc_window(parent_fd);
    if (!win) return;
    struct win32_win *pw = win32_win_find(parent_fd);
    uint32_t pbg = pw ? pw->fill_color : WIN32_FILL_DEFAULT;
    uint32_t lfg = win32_label_fg(pbg);
    for (int i = 0; i < WIN32_MAX_CTRLS; i++) {
        struct win32_ctrl *c = &g_win32_gui.ctrl[i];
        if (!c->in_use || c->parent_fd != parent_fd || c->hidden) continue;
        int ty = c->y + (c->h - 8) / 2;
        switch (c->type) {
        case WIN32_CTRL_BUTTON:
            (void)gui_window_fill(win, c->x, c->y, c->w, c->h, 0x00C8C8C8);
            (void)gui_window_rect(win, c->x, c->y, c->w, c->h, 0x00303030);
            (void)gui_window_rect(win, c->x + 1, c->y + 1, c->w - 2, c->h - 2, 0x00F4F4F4);
            (void)win32_ui_text(win, c->x + 10, ty, c->text, 0x00101010, 0x00C8C8C8);
            break;
        case WIN32_CTRL_EDIT:
            (void)gui_window_fill(win, c->x, c->y, c->w, c->h, 0x00FFFFFF);
            (void)gui_window_rect(win, c->x, c->y, c->w, c->h, c->focus ? 0x002E8AE0 : 0x00808080);
            (void)win32_ui_text(win, c->x + 5, ty, c->text[0] ? c->text : " ", 0x00000000, 0x00FFFFFF);
            break;
        case WIN32_CTRL_STATIC:
            (void)win32_ui_text(win, c->x, ty, c->text, lfg, pbg);
            break;
        case WIN32_CTRL_CHECKBOX: {
            int bs = 13, by = c->y + (c->h - bs) / 2;
            (void)gui_window_fill(win, c->x, by, bs, bs, 0x00FFFFFF);
            (void)gui_window_rect(win, c->x, by, bs, bs, 0x00404040);
            if (c->checked) {  /* a check mark (two strokes) */
                (void)gui_window_line(win, c->x + 2, by + 6, c->x + 5, by + 9, 0x00208020);
                (void)gui_window_line(win, c->x + 5, by + 9, c->x + 10, by + 2, 0x00208020);
            }
            (void)win32_ui_text(win, c->x + bs + 6, ty, c->text, lfg, pbg);
            break;
        }
        case WIN32_CTRL_LISTBOX: {
            (void)gui_window_fill(win, c->x, c->y, c->w, c->h, 0x00FFFFFF);
            (void)gui_window_rect(win, c->x, c->y, c->w, c->h, 0x00808080);
            struct win32_lb *lb = &g_win32_lb[i];
            for (int r = 0; r < lb->n && r * 14 + 14 <= c->h; r++) {
                int ry = c->y + 2 + r * 14;
                uint32_t bg = (r == lb->sel) ? 0x002E8AE0 : 0x00FFFFFF;
                uint32_t fg = (r == lb->sel) ? 0x00FFFFFF : 0x00101010;
                (void)gui_window_fill(win, c->x + 1, ry, c->w - 2, 14, bg);
                (void)win32_ui_text(win, c->x + 5, ry + 3, lb->items[r], fg, bg);
            }
            break;
        }
        case WIN32_CTRL_SCROLLBAR: {
            int ah = win32_sb_arrow(c);
            (void)gui_window_fill(win, c->x, c->y, c->w, c->h, 0x00C8C8C8);       /* track */
            (void)gui_window_rect(win, c->x, c->y, c->w, c->h, 0x00808080);
            (void)gui_window_fill(win, c->x, c->y, c->w, ah, 0x00B0B0B0);          /* up arrow */
            (void)win32_ui_text(win, c->x + c->w / 2 - 3, c->y + ah / 2 - 4, "^", 0x00202020, 0x00B0B0B0);
            (void)gui_window_fill(win, c->x, c->y + c->h - ah, c->w, ah, 0x00B0B0B0); /* down arrow */
            (void)win32_ui_text(win, c->x + c->w / 2 - 3, c->y + c->h - ah / 2 - 4, "v", 0x00202020, 0x00B0B0B0);
            int thy = win32_sb_thumb_y(c);                                          /* thumb */
            (void)gui_window_fill(win, c->x + 2, thy, c->w - 4, 16, 0x00707070);
            (void)gui_window_rect(win, c->x + 2, thy, c->w - 4, 16, 0x00404040);
            break;
        }
        case WIN32_CTRL_GROUPBOX: {     /* C14: a labelled frame (visual only) */
            (void)gui_window_rect(win, c->x, c->y + 4, c->w, c->h - 4, 0x00909090);
            int tw = 0; while (c->text[tw]) tw++;
            (void)gui_window_fill(win, c->x + 8, c->y, tw * 8 + 6, 9, pbg);
            (void)win32_ui_text(win, c->x + 11, c->y, c->text, lfg, pbg);
            break;
        }
        case WIN32_CTRL_RADIO: {        /* C14: a circular, group-exclusive check */
            int rr = 6, ccx = c->x + rr, ccy = c->y + c->h / 2;
            (void)gui_window_circle(win, ccx, ccy, rr, 0x00FFFFFF);
            (void)gui_window_circle_outline(win, ccx, ccy, rr, 0x00404040);
            if (c->checked) (void)gui_window_circle(win, ccx, ccy, 3, 0x00208020);
            (void)win32_ui_text(win, c->x + 2 * rr + 6, ty, c->text, lfg, pbg);
            break;
        }
        case WIN32_CTRL_COMBOBOX: {     /* C14: closed face (selected text + arrow) */
            struct win32_lb *lb = &g_win32_lb[i];
            int fh = 18;                 /* the always-visible field height */
            (void)gui_window_fill(win, c->x, c->y, c->w, fh, 0x00FFFFFF);
            (void)gui_window_rect(win, c->x, c->y, c->w, fh, c->dropped ? 0x002E8AE0 : 0x00808080);
            const char *seltxt = (lb->sel >= 0 && lb->sel < lb->n) ? lb->items[lb->sel] : "";
            (void)win32_ui_text(win, c->x + 5, c->y + (fh - 8) / 2, seltxt, 0x00101010, 0x00FFFFFF);
            int ax = c->x + c->w - 16;   /* drop-arrow button */
            (void)gui_window_fill(win, ax, c->y + 1, 15, fh - 2, 0x00C8C8C8);
            (void)win32_ui_text(win, ax + 4, c->y + (fh - 8) / 2, "v", 0x00202020, 0x00C8C8C8);
            break;                       /* the open dropdown is drawn in pass 2 */
        }
        case WIN32_CTRL_TAB: {          /* C14: a row of tab buttons + page frame */
            struct win32_lb *lb = &g_win32_lb[i];
            (void)gui_window_fill(win, c->x, c->y, c->w, c->h, pbg);
            int th = 20, tx = c->x;
            for (int t = 0; t < lb->n; t++) {
                int tw = 8 * (int)strlen(lb->items[t]) + 16;
                bool selt = (t == lb->sel);
                uint32_t tbg = selt ? 0x00D8D8D8 : 0x00B0B0B0;
                (void)gui_window_fill(win, tx, c->y, tw, th, tbg);
                (void)gui_window_rect(win, tx, c->y, tw, th, 0x00606060);
                (void)win32_ui_text(win, tx + 8, c->y + 6, lb->items[t], 0x00101010, tbg);
                tx += tw;
            }
            /* page area below the tab row */
            (void)gui_window_rect(win, c->x, c->y + th, c->w, c->h - th, 0x00606060);
            (void)gui_window_fill(win, c->x + 1, c->y + th + 1, c->w - 2, c->h - th - 2, 0x00D8D8D8);
            break;
        }
        case WIN32_CTRL_LISTVIEW: {     /* C25: report-view grid */
            struct win32_cc *cc = &g_win32_cc[i];
            (void)gui_window_fill(win, c->x, c->y, c->w, c->h, 0x00FFFFFF);
            (void)gui_window_rect(win, c->x, c->y, c->w, c->h, 0x00808080);
            int hh = 18;                /* column-header band */
            (void)gui_window_fill(win, c->x + 1, c->y + 1, c->w - 2, hh, 0x00DCDCDC);
            int cx = c->x;
            for (int col = 0; col < cc->lv_ncol; col++) {
                int cw = cc->lv_col[col].cx > 0 ? cc->lv_col[col].cx : 80;
                (void)gui_window_line(win, cx + cw, c->y + 1, cx + cw, c->y + hh, 0x00A0A0A0);
                (void)win32_ui_text(win, cx + 5, c->y + 5, cc->lv_col[col].text, 0x00202020, 0x00DCDCDC);
                cx += cw;
            }
            int rh = 16;
            for (int r = 0; r < cc->lv_nrow && c->y + hh + (r + 1) * rh < c->y + c->h; r++) {
                int ry = c->y + 1 + hh + r * rh;
                uint32_t bg = (r == cc->lv_sel) ? 0x002E8AE0 : 0x00FFFFFF;
                uint32_t fg = (r == cc->lv_sel) ? 0x00FFFFFF : 0x00101010;
                (void)gui_window_fill(win, c->x + 1, ry, c->w - 2, rh, bg);
                int gx = c->x;
                for (int col = 0; col < cc->lv_ncol && col < CC_LV_SUB; col++) {
                    int cw = cc->lv_col[col].cx > 0 ? cc->lv_col[col].cx : 80;
                    (void)win32_ui_text(win, gx + 5, ry + 4, cc->lv_cell[r][col], fg, bg);
                    gx += cw;
                }
            }
            break;
        }
        case WIN32_CTRL_TREEVIEW: {     /* C25: indented node tree */
            struct win32_cc *cc = &g_win32_cc[i];
            (void)gui_window_fill(win, c->x, c->y, c->w, c->h, 0x00FFFFFF);
            (void)gui_window_rect(win, c->x, c->y, c->w, c->h, 0x00808080);
            int rh = 16, row = 0;
            for (int n = 0; n < cc->tv_n; n++) {
                /* hide nodes whose any ancestor is collapsed */
                bool vis = true;
                int p = cc->tv[n].parent;
                while (p >= 0) { if (!cc->tv[p].expanded) { vis = false; break; } p = cc->tv[p].parent; }
                if (!vis) continue;
                int ry = c->y + 2 + row * rh;
                if (ry + rh > c->y + c->h) break;
                int ind = c->x + 6 + cc->tv[n].level * 16;
                bool seln = (n == cc->tv_sel);
                if (seln) (void)gui_window_fill(win, c->x + 1, ry, c->w - 2, rh, 0x002E8AE0);
                if (cc->tv[n].haskids)
                    (void)win32_ui_text(win, ind - 2, ry + 3,
                                        cc->tv[n].expanded ? "-" : "+",
                                        seln ? 0x00FFFFFF : 0x00404040,
                                        seln ? 0x002E8AE0 : 0x00FFFFFF);
                (void)win32_ui_text(win, ind + 12, ry + 3, cc->tv[n].text,
                                    seln ? 0x00FFFFFF : 0x00101010,
                                    seln ? 0x002E8AE0 : 0x00FFFFFF);
                row++;
            }
            break;
        }
        case WIN32_CTRL_STATUSBAR: {    /* C25: bottom-docked multi-part bar */
            struct win32_cc *cc = &g_win32_cc[i];
            int sx = 0, sw = pw ? pw->w : c->w, sh = 20;
            int sy = (pw ? pw->h : (c->y + c->h)) - sh;
            (void)gui_window_fill(win, sx, sy, sw, sh, 0x00C8C8C8);
            (void)gui_window_line(win, sx, sy, sx + sw, sy, 0x00808080);
            int nparts = cc->sb_nparts > 0 ? cc->sb_nparts : 1;
            int pw_each = sw / nparts;
            for (int p = 0; p < nparts; p++) {
                int px = sx + p * pw_each;
                if (p > 0) (void)gui_window_line(win, px, sy + 2, px, sy + sh - 2, 0x00909090);
                (void)win32_ui_text(win, px + 6, sy + 6, cc->sb_part[p], 0x00101010, 0x00C8C8C8);
            }
            break;
        }
        case WIN32_CTRL_PROGRESS: {     /* C25: filled proportion bar */
            struct win32_cc *cc = &g_win32_cc[i];
            (void)gui_window_fill(win, c->x, c->y, c->w, c->h, 0x00FFFFFF);
            (void)gui_window_rect(win, c->x, c->y, c->w, c->h, 0x00808080);
            int span = cc->pb_max - cc->pb_min;
            if (span < 1) span = 1;
            int filled = (int)(((long)(cc->pb_pos - cc->pb_min) * (c->w - 2)) / span);
            if (filled < 0) filled = 0;
            if (filled > c->w - 2) filled = c->w - 2;
            if (filled > 0)
                (void)gui_window_fill(win, c->x + 1, c->y + 1, filled, c->h - 2, 0x0028A828);
            break;
        }
        case WIN32_CTRL_TOOLBAR: {      /* C25: a strip of flat buttons */
            struct win32_cc *cc = &g_win32_cc[i];
            int tw = pw ? pw->w : c->w, th = 26;
            (void)gui_window_fill(win, 0, c->y, tw, th, 0x00E4E4E4);
            (void)gui_window_line(win, 0, c->y + th, tw, c->y + th, 0x00A0A0A0);
            for (int b = 0; b < cc->tb_n; b++) {
                int bx = 6 + b * 34;
                (void)gui_window_fill(win, bx, c->y + 3, 30, th - 6, 0x00C8C8C8);
                (void)gui_window_rect(win, bx, c->y + 3, 30, th - 6, 0x00808080);
            }
            break;
        }
        }
    }
    /* Pass 2: open COMBOBOX dropdowns, drawn on top of sibling controls. */
    for (int i = 0; i < WIN32_MAX_CTRLS; i++) {
        struct win32_ctrl *c = &g_win32_gui.ctrl[i];
        if (!c->in_use || c->parent_fd != parent_fd || c->hidden) continue;
        if (c->type != WIN32_CTRL_COMBOBOX || !c->dropped) continue;
        struct win32_lb *lb = &g_win32_lb[i];
        int fh = 18, dy = c->y + fh, dh = lb->n * 14 + 2;
        (void)gui_window_fill(win, c->x, dy, c->w, dh, 0x00FFFFFF);
        (void)gui_window_rect(win, c->x, dy, c->w, dh, 0x00404040);
        for (int r = 0; r < lb->n; r++) {
            int ry = dy + 1 + r * 14;
            uint32_t bg = (r == lb->sel) ? 0x002E8AE0 : 0x00FFFFFF;
            uint32_t fg = (r == lb->sel) ? 0x00FFFFFF : 0x00101010;
            (void)gui_window_fill(win, c->x + 1, ry, c->w - 2, 14, bg);
            (void)win32_ui_text(win, c->x + 5, ry + 3, lb->items[r], fg, bg);
        }
    }
}
/* Redraw a parent's controls + present (after a control changes). */
static void win32_refresh_controls(int parent_fd) {
    win32_draw_controls(parent_fd);
    win32_draw_menu(parent_fd);   /* C15: menu bar / open dropdown on top */
    (void)sys_gui_flip(parent_fd);
}
/* Queue a message (WM_COMMAND/WM_VSCROLL) for a parent window. */
static void win32_msg_push(int parent_fd, uint32_t message, uint64_t wParam, uint64_t lParam) {
    int next = (g_win32_gui.cmd_head + 1) % WIN32_CMDQ;
    if (next == g_win32_gui.cmd_tail) return;   /* full -> drop */
    g_win32_gui.cmd_q[g_win32_gui.cmd_head].hwnd    = (uint64_t)(uint32_t)parent_fd;
    g_win32_gui.cmd_q[g_win32_gui.cmd_head].message = message;
    g_win32_gui.cmd_q[g_win32_gui.cmd_head].wParam  = wParam;
    g_win32_gui.cmd_q[g_win32_gui.cmd_head].lParam  = lParam;
    g_win32_gui.cmd_head = next;
}

/* ================= C15: menus =================
 * An HMENU is a tagged token into g_win32_menus[]. A menu bar (CreateMenu) is
 * attached to a window via SetMenu and kernel-drawn into the top strip of the
 * window's client backbuffer; a popup (CreatePopupMenu) appended with MF_POPUP
 * is its dropdown. A menu-item click delivers WM_COMMAND(id) to the window. */
#define WIN32_MAX_MENUS   8
#define WIN32_MENU_ITEMS  8
struct win32_menuitem { int id; bool is_popup; uint64_t submenu; char text[24]; };
struct win32_menu { bool in_use; bool is_popup; int n; struct win32_menuitem items[WIN32_MENU_ITEMS]; };
static struct win32_menu g_win32_menus[WIN32_MAX_MENUS];

static struct win32_menu *win32_menu_find(uint64_t h) {
    if ((h & WIN32_TAG_MASK) != WIN32_MENU_TAG) return NULL;
    int s = (int)(h & 0xFF);
    if (s < 0 || s >= WIN32_MAX_MENUS || !g_win32_menus[s].in_use) return NULL;
    return &g_win32_menus[s];
}
static long win32_menu_alloc(bool popup) {
    for (int i = 0; i < WIN32_MAX_MENUS; i++)
        if (!g_win32_menus[i].in_use) {
            memset(&g_win32_menus[i], 0, sizeof(g_win32_menus[i]));
            g_win32_menus[i].in_use = true; g_win32_menus[i].is_popup = popup;
            return (long)(WIN32_MENU_TAG | (uint64_t)i);
        }
    return 0;
}
/* Width of a top-level bar item (label + padding). */
static int win32_menubar_item_w(const struct win32_menuitem *it) {
    return (int)strlen(it->text) * 8 + 16;
}
/* Left x of bar item `idx`. */
static int win32_menubar_item_x(const struct win32_menu *bar, int idx) {
    int x = 4;
    for (int i = 0; i < idx && i < bar->n; i++) x += win32_menubar_item_w(&bar->items[i]);
    return x;
}
/* Bar item hit by x (in the bar strip), or -1. */
static int win32_menubar_hit(const struct win32_menu *bar, int x) {
    int bx = 4;
    for (int i = 0; i < bar->n; i++) {
        int w = win32_menubar_item_w(&bar->items[i]);
        if (x >= bx && x < bx + w) return i;
        bx += w;
    }
    return -1;
}
/* Pixel width of a popup (widest item + padding). */
static int win32_menu_popup_w(const struct win32_menu *pop) {
    int mw = 60;
    for (int i = 0; i < pop->n; i++) {
        int w = (int)strlen(pop->items[i].text) * 8 + 28;
        if (w > mw) mw = w;
    }
    return mw;
}

/* Draw a window's menu bar (+ open dropdown) into the top of its backbuffer. */
static void win32_draw_menu(int fd) {
    struct win32_win *w = win32_win_find(fd);
    if (!w || !w->menu) return;
    struct win32_menu *bar = win32_menu_find(w->menu);
    struct window *win = win32_hdc_window(fd);
    if (!bar || !win) return;
    (void)gui_window_fill(win, 0, 0, w->w, WIN32_MENUBAR_H, 0x00ECECEC);
    (void)gui_window_line(win, 0, WIN32_MENUBAR_H - 1, w->w, WIN32_MENUBAR_H - 1, 0x00A0A0A0);
    int tx = 4;
    for (int i = 0; i < bar->n; i++) {
        int iw = win32_menubar_item_w(&bar->items[i]);
        bool open = (w->menu_open == i);
        if (open) (void)gui_window_fill(win, tx, 0, iw, WIN32_MENUBAR_H - 1, 0x002E8AE0);
        (void)win32_ui_text(win, tx + 8, (WIN32_MENUBAR_H - 8) / 2 - 1, bar->items[i].text,
                              open ? 0x00FFFFFF : 0x00202020, open ? 0x002E8AE0 : 0x00ECECEC);
        tx += iw;
    }
    if (w->menu_open >= 0 && w->menu_open < bar->n) {
        struct win32_menu *pop = win32_menu_find(bar->items[w->menu_open].submenu);
        if (pop) {
            int mx = win32_menubar_item_x(bar, w->menu_open), my = WIN32_MENUBAR_H;
            int mw = win32_menu_popup_w(pop), mh = pop->n * WIN32_MENU_ITEMH + 4;
            (void)gui_window_fill(win, mx, my, mw, mh, 0x00F4F4F4);
            (void)gui_window_rect(win, mx, my, mw, mh, 0x00606060);
            for (int j = 0; j < pop->n; j++) {
                int iy = my + 2 + j * WIN32_MENU_ITEMH;
                if (pop->items[j].text[0] == 0)   /* separator */
                    (void)gui_window_line(win, mx + 4, iy + WIN32_MENU_ITEMH / 2,
                                          mx + mw - 4, iy + WIN32_MENU_ITEMH / 2, 0x00B0B0B0);
                else
                    (void)win32_ui_text(win, mx + 10, iy + (WIN32_MENU_ITEMH - 8) / 2,
                                          pop->items[j].text, 0x00202020, 0x00F4F4F4);
            }
        }
    }
}
/* Repaint a window's menu immediately + arm a full WM_PAINT (so a closing
 * dropdown's background is erased by the app's paint). */
static void win32_menu_repaint(int fd) {
    struct win32_win *w = win32_win_find(fd);
    if (w) w->needs_paint = true;
    win32_refresh_controls(fd);
}
/* Route a click to the window's menu bar / open dropdown. Returns true if the
 * menu consumed it (the app should not also see it). */
static bool win32_route_menu_input(int fd, const struct gui_event *ev) {
    struct win32_win *w = win32_win_find(fd);
    if (!w || !w->menu || ev->type != GUI_EV_MOUSE_DOWN) return false;
    struct win32_menu *bar = win32_menu_find(w->menu);
    if (!bar) return false;
    /* An open dropdown captures clicks first. */
    if (w->menu_open >= 0 && w->menu_open < bar->n) {
        struct win32_menu *pop = win32_menu_find(bar->items[w->menu_open].submenu);
        if (pop) {
            int mx = win32_menubar_item_x(bar, w->menu_open), my = WIN32_MENUBAR_H;
            int mw = win32_menu_popup_w(pop), mh = pop->n * WIN32_MENU_ITEMH + 4;
            if (ev->x >= mx && ev->x < mx + mw && ev->y >= my && ev->y < my + mh) {
                int j = (ev->y - (my + 2)) / WIN32_MENU_ITEMH;
                if (j >= 0 && j < pop->n && pop->items[j].text[0] && pop->items[j].id) {
                    int id = pop->items[j].id;
                    w->menu_open = -1;
                    win32_menu_repaint(fd);
                    win32_msg_push(fd, WM_COMMAND, (uint64_t)(uint32_t)id, 0);  /* menu: HIWORD=0 */
                    if (g_win32_log_input)
                        kprintf("[winpe] menu: item id=%d -> WM_COMMAND\n", id);
                }
                return true;
            }
        }
    }
    /* A click in the bar toggles that item's dropdown. */
    if (ev->y >= 0 && ev->y < WIN32_MENUBAR_H) {
        int idx = win32_menubar_hit(bar, ev->x);
        w->menu_open = (idx >= 0 && w->menu_open != idx) ? idx : -1;
        win32_menu_repaint(fd);
        return true;
    }
    /* A click anywhere else closes an open dropdown (and is consumed). */
    if (w->menu_open >= 0) { w->menu_open = -1; win32_menu_repaint(fd); return true; }
    return false;
}

/* ================= C15: timers =================
 * SetTimer registers a periodic WM_TIMER for a window; GetMessage / the dialog
 * pump synthesise the message when the interval elapses (PIT-tick based). */
#define WIN32_MAX_TIMERS 8
struct win32_timer { bool in_use; int fd; uint64_t id, interval, next, proc; };
static struct win32_timer g_win32_timers[WIN32_MAX_TIMERS];

static long w32_SetTimer(uint64_t *a) {
    int fd = (int)a[0]; uint64_t id = a[1], ms = (uint32_t)a[2], proc = a[3];
    uint64_t hz = pit_hz(); if (!hz) hz = 100;
    uint64_t ticks = (ms * hz + 999) / 1000; if (ticks < 1) ticks = 1;
    for (int i = 0; i < WIN32_MAX_TIMERS; i++)        /* update existing */
        if (g_win32_timers[i].in_use && g_win32_timers[i].fd == fd && g_win32_timers[i].id == id) {
            g_win32_timers[i].interval = ticks; g_win32_timers[i].next = pit_ticks() + ticks;
            g_win32_timers[i].proc = proc; return (long)(id ? id : 1);
        }
    for (int i = 0; i < WIN32_MAX_TIMERS; i++)        /* allocate */
        if (!g_win32_timers[i].in_use) {
            g_win32_timers[i].in_use = true; g_win32_timers[i].fd = fd; g_win32_timers[i].id = id;
            g_win32_timers[i].interval = ticks; g_win32_timers[i].next = pit_ticks() + ticks;
            g_win32_timers[i].proc = proc;
            if (g_win32_log_input) kprintf("[winpe] SetTimer hwnd=%d id=%lu %lums\n", fd, (unsigned long)id, (unsigned long)ms);
            return (long)(id ? id : 1);
        }
    return 0;
}
static long w32_KillTimer(uint64_t *a) {
    int fd = (int)a[0]; uint64_t id = a[1];
    for (int i = 0; i < WIN32_MAX_TIMERS; i++)
        if (g_win32_timers[i].in_use && g_win32_timers[i].fd == fd && g_win32_timers[i].id == id)
            g_win32_timers[i].in_use = false;
    return 1;
}
/* If a timer for window `only_fd` (or any live window when only_fd<=0) is due,
 * fill *m with WM_TIMER + stash the window's WndProc; returns true. */
static bool win32_timer_poll(int only_fd, struct win32_msg *m) {
    uint64_t now = pit_ticks();
    for (int i = 0; i < WIN32_MAX_TIMERS; i++) {
        struct win32_timer *t = &g_win32_timers[i];
        if (!t->in_use) continue;
        if (only_fd > 0 && t->fd != only_fd) continue;
        struct win32_win *w = win32_win_find(t->fd);
        if (!w) { t->in_use = false; continue; }     /* window gone -> drop */
        if (now >= t->next) {
            t->next = now + t->interval;
            memset(m, 0, sizeof(*m));
            m->hwnd = (uint64_t)t->fd; m->message = WM_TIMER;
            m->wParam = t->id; m->lParam = t->proc;
            win32_stash_wndproc(w);
            return true;
        }
    }
    return false;
}
/* Clear process-global menu + timer state on a fresh GUI app (alongside the
 * g_win32_gui memset; HMENU/timer tokens don't survive a process exit). */
static void win32_reset_menus_timers(void) {
    memset(g_win32_menus, 0, sizeof(g_win32_menus));
    memset(g_win32_timers, 0, sizeof(g_win32_timers));
    win32_reset_env();   /* C16c: per-app environment reseeds on next use */
    win32_reset_tls();   /* C16e: per-app TLS index bitmap */
}

/* ---- menu shims (user32) ---- */
static long w32_CreateMenu(uint64_t *a)      { (void)a; return win32_menu_alloc(false); }
static long w32_CreatePopupMenu(uint64_t *a) { (void)a; return win32_menu_alloc(true); }
static long w32_DestroyMenu(uint64_t *a) {
    struct win32_menu *m = win32_menu_find(a[0]);
    if (m) m->in_use = false;
    return 1;
}
/* AppendMenuA(hMenu, uFlags, uIDNewItem, lpNewItem). MF_POPUP -> uIDNewItem is a
 * submenu HMENU; MF_SEPARATOR -> a divider; else a command item. */
static long w32_AppendMenuA(uint64_t *a) {
    struct win32_menu *m = win32_menu_find(a[0]);
    if (!m || m->n >= WIN32_MENU_ITEMS) return 0;
    uint32_t flags = (uint32_t)a[1];
    struct win32_menuitem *it = &m->items[m->n];
    memset(it, 0, sizeof(*it));
    if (flags & MF_SEPARATOR) {
        it->id = 0; it->text[0] = 0;
    } else if (flags & MF_POPUP) {
        it->is_popup = true; it->submenu = a[2];
        if (a[3]) (void)strncpy_from_user(it->text, (const char *)(uintptr_t)a[3], sizeof(it->text));
    } else {
        it->id = (int)a[2];
        if (a[3]) (void)strncpy_from_user(it->text, (const char *)(uintptr_t)a[3], sizeof(it->text));
    }
    m->n++;
    return 1;
}
/* SetMenu(hwnd, hMenu) -> TRUE; attach a bar menu to a window. */
static long w32_SetMenu(uint64_t *a) {
    struct win32_win *w = win32_win_find((int)a[0]);
    if (!w) return 0;
    w->menu = a[1]; w->menu_open = -1;
    w->needs_paint = true;
    win32_refresh_controls((int)a[0]);
    return 1;
}
static long w32_GetMenu(uint64_t *a) {
    struct win32_win *w = win32_win_find((int)a[0]);
    return w ? (long)w->menu : 0;
}

/* C14: a TAB selection change is reported as WM_NOTIFY whose lParam points to an
 * NMHDR { hwndFrom, idFrom, code=TCN_SELCHANGE } staged in the CRT data page.
 * The app handles WM_NOTIFY, checks ->code, then calls TabCtrl_GetCurSel. */
static void win32_tab_notify(int parent_fd, int ctrl_id, int slot) {
    uint64_t base = WIN32_CRT_DATA_BASE + WIN32_CRT_NMHDR;
    uint64_t hwndFrom = WIN32_CTRL_TAG | (uint64_t)slot;
    uint64_t idFrom   = (uint64_t)(uint32_t)ctrl_id;
    uint32_t code     = (uint32_t)TCN_SELCHANGE;
    (void)copy_to_user((void *)(uintptr_t)(base + 0x00), &hwndFrom, 8);
    (void)copy_to_user((void *)(uintptr_t)(base + 0x08), &idFrom, 8);
    (void)copy_to_user((void *)(uintptr_t)(base + 0x10), &code, 4);
    win32_msg_push(parent_fd, WM_NOTIFY, (uint64_t)(uint32_t)ctrl_id, base);
}

/* Route a parent window's input event to a control if it targets one. Returns
 * true if a control consumed it (the app should NOT see it). */
static bool win32_route_control_input(int parent_fd, const struct gui_event *ev) {
    bool has = false;
    for (int i = 0; i < WIN32_MAX_CTRLS; i++)
        if (g_win32_gui.ctrl[i].in_use && g_win32_gui.ctrl[i].parent_fd == parent_fd) has = true;
    if (!has) return false;

    if (ev->type == GUI_EV_MOUSE_DOWN) {
        /* C14 pre-pass A: an open COMBOBOX dropdown is drawn on top of its
         * siblings, so it captures clicks first; a closed combo opens on a
         * field click. */
        for (int i = 0; i < WIN32_MAX_CTRLS; i++) {
            struct win32_ctrl *c = &g_win32_gui.ctrl[i];
            if (!c->in_use || c->parent_fd != parent_fd || c->hidden) continue;
            if (c->type != WIN32_CTRL_COMBOBOX) continue;
            struct win32_lb *lb = &g_win32_lb[i];
            int fh = 18, dy = c->y + fh, dh = lb->n * 14 + 2;
            bool inField = ev->x >= c->x && ev->x < c->x + c->w &&
                           ev->y >= c->y && ev->y < c->y + fh;
            if (c->dropped) {
                bool inDrop = ev->x >= c->x && ev->x < c->x + c->w &&
                              ev->y >= dy && ev->y < dy + dh;
                if (inDrop) {
                    int row = (ev->y - (dy + 1)) / 14;
                    if (row >= 0 && row < lb->n) {
                        lb->sel = row;
                        win32_msg_push(parent_fd, WM_COMMAND,
                                       ((uint64_t)CBN_SELCHANGE << 16) | (uint32_t)c->id,
                                       WIN32_CTRL_TAG | (uint64_t)i);
                        if (g_win32_log_input)
                            kprintf("[winpe] control: COMBOBOX id=%d -> sel=%d ('%s')\n",
                                    c->id, row, lb->items[row]);
                    }
                    c->dropped = false;
                    /* closing shrinks the combo's footprint -> repaint the bg to
                     * erase the dropdown (the app's WM_PAINT / the kernel dialog
                     * painter refills the background, then redraws controls). */
                    { struct win32_win *pw = win32_win_find(parent_fd); if (pw) pw->needs_paint = true; }
                    win32_refresh_controls(parent_fd);
                    return true;
                }
                c->dropped = false;                 /* click off the list -> close */
                { struct win32_win *pw = win32_win_find(parent_fd); if (pw) pw->needs_paint = true; }
                win32_refresh_controls(parent_fd);
                if (inField) return true;           /* on the field -> just close */
                /* else fall through so the click can hit another control */
            } else if (inField) {
                c->dropped = true;                  /* open the dropdown */
                win32_refresh_controls(parent_fd);
                if (g_win32_log_input)
                    kprintf("[winpe] control: COMBOBOX id=%d opened\n", c->id);
                return true;
            }
        }
        /* C14 pre-pass B: a TAB strip captures clicks only in its tab ROW; the
         * page area below stays click-transparent so the page's controls work. */
        for (int i = 0; i < WIN32_MAX_CTRLS; i++) {
            struct win32_ctrl *c = &g_win32_gui.ctrl[i];
            if (!c->in_use || c->parent_fd != parent_fd || c->hidden) continue;
            if (c->type != WIN32_CTRL_TAB) continue;
            if (ev->y < c->y || ev->y >= c->y + 20) continue;   /* tab row height = 20 */
            struct win32_lb *lb = &g_win32_lb[i];
            int tx = c->x;
            for (int t = 0; t < lb->n; t++) {
                int tw = 8 * (int)strlen(lb->items[t]) + 16;
                if (ev->x >= tx && ev->x < tx + tw) {
                    if (lb->sel != t) {
                        lb->sel = t;
                        win32_tab_notify(parent_fd, c->id, i);   /* WM_NOTIFY TCN_SELCHANGE */
                        win32_refresh_controls(parent_fd);
                        if (g_win32_log_input)
                            kprintf("[winpe] control: TAB id=%d -> sel=%d ('%s')\n",
                                    c->id, t, lb->items[t]);
                    }
                    return true;
                }
                tx += tw;
            }
            return true;   /* in the tab row but between tabs -> still consume */
        }
        for (int i = 0; i < WIN32_MAX_CTRLS; i++) {
            struct win32_ctrl *c = &g_win32_gui.ctrl[i];
            if (!c->in_use || c->parent_fd != parent_fd || c->hidden) continue;
            /* COMBOBOX is handled above; GROUPBOX/TAB page areas are click-
             * transparent so controls drawn over them still receive clicks.
             * C25: the comctl32 controls are driven by messages in this proof,
             * so they are click-transparent too (no spurious hit regions). */
            if (c->type == WIN32_CTRL_COMBOBOX || c->type == WIN32_CTRL_GROUPBOX ||
                c->type == WIN32_CTRL_TAB || win32_is_comctl(c->type)) continue;
            if (ev->x < c->x || ev->x >= c->x + c->w ||
                ev->y < c->y || ev->y >= c->y + c->h) continue;
            uint64_t hctrl = WIN32_CTRL_TAG | (uint64_t)i;
            switch (c->type) {
            case WIN32_CTRL_BUTTON:
                win32_msg_push(parent_fd, WM_COMMAND,
                               ((uint64_t)BN_CLICKED << 16) | (uint32_t)c->id, hctrl);
                if (g_win32_log_input)
                    kprintf("[winpe] control: BUTTON id=%d clicked -> WM_COMMAND\n", c->id);
                break;
            case WIN32_CTRL_CHECKBOX:
                c->checked = !c->checked;
                win32_msg_push(parent_fd, WM_COMMAND,
                               ((uint64_t)BN_CLICKED << 16) | (uint32_t)c->id, hctrl);
                win32_refresh_controls(parent_fd);
                if (g_win32_log_input)
                    kprintf("[winpe] control: CHECKBOX id=%d -> checked=%d\n", c->id, (int)c->checked);
                break;
            case WIN32_CTRL_RADIO:
                /* mutual exclusion within the same parent + WS_GROUP group */
                for (int j = 0; j < WIN32_MAX_CTRLS; j++) {
                    struct win32_ctrl *o = &g_win32_gui.ctrl[j];
                    if (o->in_use && o->parent_fd == parent_fd &&
                        o->type == WIN32_CTRL_RADIO && o->group == c->group)
                        o->checked = false;
                }
                c->checked = true;
                win32_msg_push(parent_fd, WM_COMMAND,
                               ((uint64_t)BN_CLICKED << 16) | (uint32_t)c->id, hctrl);
                win32_refresh_controls(parent_fd);
                if (g_win32_log_input)
                    kprintf("[winpe] control: RADIO id=%d (group=%d) selected\n", c->id, c->group);
                break;
            case WIN32_CTRL_EDIT:
                for (int j = 0; j < WIN32_MAX_CTRLS; j++)
                    if (g_win32_gui.ctrl[j].in_use && g_win32_gui.ctrl[j].parent_fd == parent_fd &&
                        g_win32_gui.ctrl[j].type == WIN32_CTRL_EDIT)
                        g_win32_gui.ctrl[j].focus = false;
                c->focus = true;
                win32_refresh_controls(parent_fd);
                break;
            case WIN32_CTRL_LISTBOX: {
                struct win32_lb *lb = &g_win32_lb[i];
                int row = (ev->y - (c->y + 2)) / 14;
                if (row >= 0 && row < lb->n) {
                    lb->sel = row;
                    win32_msg_push(parent_fd, WM_COMMAND,
                                   ((uint64_t)LBN_SELCHANGE << 16) | (uint32_t)c->id, hctrl);
                    win32_refresh_controls(parent_fd);
                    if (g_win32_log_input)
                        kprintf("[winpe] control: LISTBOX id=%d -> sel=%d ('%s')\n",
                                c->id, row, lb->items[row]);
                }
                break;
            }
            case WIN32_CTRL_SCROLLBAR: {
                int ah = win32_sb_arrow(c);
                int code, delta;
                if (ev->y < c->y + ah)              { code = SB_LINEUP;   delta = -1; }
                else if (ev->y >= c->y + c->h - ah) { code = SB_LINEDOWN; delta = +1; }
                else if (ev->y < win32_sb_thumb_y(c)) { code = SB_PAGEUP;   delta = -5; }
                else                                { code = SB_PAGEDOWN; delta = +5; }
                c->sb_pos += delta;
                if (c->sb_pos < c->sb_min) c->sb_pos = c->sb_min;
                if (c->sb_pos > c->sb_max) c->sb_pos = c->sb_max;
                win32_msg_push(parent_fd, WM_VSCROLL,
                               ((uint64_t)(uint32_t)c->sb_pos << 16) | (uint32_t)code, hctrl);
                win32_refresh_controls(parent_fd);
                if (g_win32_log_input)
                    kprintf("[winpe] control: SCROLLBAR id=%d -> pos=%d -> WM_VSCROLL\n", c->id, c->sb_pos);
                break;
            }
            default: break;
            }
            return true;
        }
        return false;   /* click missed all controls -> deliver to app */
    }

    if (ev->type == GUI_EV_KEY) {
        for (int i = 0; i < WIN32_MAX_CTRLS; i++) {
            struct win32_ctrl *c = &g_win32_gui.ctrl[i];
            if (!c->in_use || c->parent_fd != parent_fd ||
                c->type != WIN32_CTRL_EDIT || !c->focus) continue;
            int n = 0;
            while (n < (int)sizeof(c->text) - 1 && c->text[n]) n++;
            uint8_t k = ev->key;
            if (k == '\b' || k == 0x7F) { if (n > 0) c->text[n - 1] = 0; }
            else if (k >= 0x20 && k < 0x7F && n < (int)sizeof(c->text) - 1) {
                c->text[n] = (char)k; c->text[n + 1] = 0;
            } else return false;   /* non-text key -> deliver to app */
            win32_refresh_controls(parent_fd);
            if (g_win32_log_input)
                kprintf("[winpe] control: EDIT id=%d text='%s'\n", c->id, c->text);
            return true;
        }
        return false;   /* no focused edit -> deliver to app */
    }
    return false;
}

/* user32!GetWindowTextA(hwnd, buf, max) -> length copied (a control's text). */
static long w32_GetWindowTextA(uint64_t *a) {
    struct win32_ctrl *c = win32_ctrl_find(a[0]);
    int max = (int)(int32_t)a[2];
    if (max <= 0) return 0;
    int n = 0;
    if (c) { while (n < (int)sizeof(c->text) - 1 && c->text[n]) n++; }
    if (n > max - 1) n = max - 1;
    if (c && n) (void)copy_to_user((void *)(uintptr_t)a[1], c->text, n);
    char z = 0; (void)copy_to_user((void *)(uintptr_t)(a[1] + n), &z, 1);
    return n;
}
/* user32!SetWindowTextA(hwnd, text) -> TRUE. Sets a control's text + redraws. */
static long w32_SetWindowTextA(uint64_t *a) {
    struct win32_ctrl *c = win32_ctrl_find(a[0]);
    if (!c) return 0;
    c->text[0] = 0;
    if (a[1])
        (void)strncpy_from_user(c->text, (const char *)(uintptr_t)a[1], sizeof(c->text));
    win32_refresh_controls(c->parent_fd);
    return 1;
}

/* C13: a control's slot index (for the parallel listbox store). */
static int win32_ctrl_slot(const struct win32_ctrl *c) {
    return (int)(c - g_win32_gui.ctrl);
}

/* C25: read an int32 / a char*-string field out of a user-space control struct
 * (LVITEM/LVCOLUMN/TVINSERTSTRUCT) by byte offset. */
static int cc_rd_i32(uint64_t base, uint64_t off) {
    int32_t v = 0;
    (void)copy_from_user(&v, (const void *)(uintptr_t)(base + off), 4);
    return v;
}
static void cc_rd_str(uint64_t base, uint64_t off, char *dst, int dstsz) {
    dst[0] = 0;
    uint64_t p = 0;
    if (copy_from_user(&p, (const void *)(uintptr_t)(base + off), 8) == 0 && p)
        (void)strncpy_from_user(dst, (const char *)(uintptr_t)p, dstsz);
}
static int cc_copy_out(uint64_t base, uint64_t ptroff, const char *src) {
    uint64_t buf = 0;
    if (copy_from_user(&buf, (const void *)(uintptr_t)(base + ptroff), 8) != 0 || !buf)
        return 0;
    int n = 0; while (src[n]) n++;
    (void)copy_to_user((void *)(uintptr_t)buf, src, n);
    char z = 0; (void)copy_to_user((void *)(uintptr_t)(buf + n), &z, 1);
    return n;
}

/* C25: comctl32 common-control message handler (SysListView32/SysTreeView32/
 * msctls_statusbar32/msctls_progress32/ToolbarWindow32). Dispatched by control
 * TYPE first because the StatusBar/Progress/Toolbar messages share WM_USER+n
 * numeric values. The kernel IS the control's WndProc. */
static long win32_cc_sendmessage(struct win32_ctrl *c, int slot,
                                 uint32_t msg, uint64_t wp, uint64_t lp) {
    struct win32_cc *cc = &g_win32_cc[slot];
    switch (c->type) {
    case WIN32_CTRL_LISTVIEW:
        switch (msg) {
        case LVM_INSERTCOLUMNA: {
            if (cc->lv_ncol >= CC_LV_COLS) return -1;
            int idx = cc->lv_ncol++;
            cc->lv_col[idx].cx = cc_rd_i32(lp, 0x08);          /* LVCOLUMN.cx   */
            cc_rd_str(lp, 0x10, cc->lv_col[idx].text, CC_TEXT);/* LVCOLUMN.pszText */
            win32_refresh_controls(c->parent_fd);
            return idx;
        }
        case LVM_INSERTITEMA: {
            int row = cc_rd_i32(lp, 0x04);                     /* LVITEM.iItem  */
            if (row < 0) row = 0;
            if (row >= CC_LV_ROWS) return -1;
            cc_rd_str(lp, 0x18, cc->lv_cell[row][0], CC_TEXT); /* LVITEM.pszText */
            for (int s = 1; s < CC_LV_SUB; s++) cc->lv_cell[row][s][0] = 0;
            if (row + 1 > cc->lv_nrow) cc->lv_nrow = row + 1;
            win32_refresh_controls(c->parent_fd);
            return row;
        }
        case LVM_SETITEMA:
        case LVM_SETITEMTEXTA: {
            int row = (msg == LVM_SETITEMTEXTA) ? (int)(int32_t)wp : cc_rd_i32(lp, 0x04);
            int sub = cc_rd_i32(lp, 0x08);                     /* iSubItem */
            if (row < 0 || row >= CC_LV_ROWS || sub < 0 || sub >= CC_LV_SUB) return 0;
            cc_rd_str(lp, 0x18, cc->lv_cell[row][sub], CC_TEXT);
            if (row + 1 > cc->lv_nrow) cc->lv_nrow = row + 1;
            win32_refresh_controls(c->parent_fd);
            return 1;
        }
        case LVM_GETITEMTEXTA: {
            int row = (int)(int32_t)wp;
            int sub = cc_rd_i32(lp, 0x08);
            if (row < 0 || row >= cc->lv_nrow || sub < 0 || sub >= CC_LV_SUB) return 0;
            return cc_copy_out(lp, 0x18, cc->lv_cell[row][sub]);  /* LVITEM.pszText */
        }
        case LVM_GETITEMCOUNT:   return cc->lv_nrow;
        case LVM_DELETEALLITEMS: cc->lv_nrow = 0; cc->lv_sel = -1;
                                 win32_refresh_controls(c->parent_fd); return 1;
        case LVM_SETITEMSTATE: {
            int row = (int)(int32_t)wp;
            uint32_t state = (uint32_t)cc_rd_i32(lp, 0x0C);    /* LVITEM.state */
            if (state & LVIS_SELECTED) { cc->lv_sel = row; win32_refresh_controls(c->parent_fd); }
            return 1;
        }
        case LVM_GETNEXTITEM:
            return ((uint32_t)lp & LVNI_SELECTED) ? cc->lv_sel : -1;
        }
        return 0;
    case WIN32_CTRL_TREEVIEW:
        switch (msg) {
        case TVM_INSERTITEMA: {
            if (cc->tv_n >= CC_TV_NODES) return 0;
            int idx = cc->tv_n++;
            uint64_t hParent = 0;
            (void)copy_from_user(&hParent, (const void *)(uintptr_t)(lp + 0x00), 8);
            int parent = (hParent == 0) ? -1 : (int)hParent - 1;
            if (parent < -1 || parent >= idx) parent = -1;
            cc->tv[idx].parent   = parent;
            cc->tv[idx].level    = (parent < 0) ? 0 : cc->tv[parent].level + 1;
            cc->tv[idx].expanded = 1;
            cc->tv[idx].haskids  = 0;
            if (parent >= 0) cc->tv[parent].haskids = 1;
            cc_rd_str(lp, 0x28, cc->tv[idx].text, CC_TEXT);    /* TVINSERTSTRUCT.item.pszText */
            win32_refresh_controls(c->parent_fd);
            return (long)(idx + 1);                            /* HTREEITEM */
        }
        case TVM_GETCOUNT: return cc->tv_n;
        case TVM_EXPAND: {
            int node = (int)lp - 1;
            if (node < 0 || node >= cc->tv_n) return 0;
            if (wp & TVE_EXPAND)        cc->tv[node].expanded = 1;
            else if (wp & TVE_COLLAPSE) cc->tv[node].expanded = 0;
            win32_refresh_controls(c->parent_fd);
            return 1;
        }
        case TVM_SELECTITEM: {
            int node = (int)lp - 1;
            cc->tv_sel = (node >= 0 && node < cc->tv_n) ? node : -1;
            win32_refresh_controls(c->parent_fd);
            return 1;
        }
        case TVM_GETNEXTITEM:
            if (wp == TVGN_CARET) return cc->tv_sel >= 0 ? (long)(cc->tv_sel + 1) : 0;
            if (wp == TVGN_ROOT) {
                for (int n = 0; n < cc->tv_n; n++)
                    if (cc->tv[n].parent < 0) return (long)(n + 1);
                return 0;
            }
            return 0;
        }
        return 0;
    case WIN32_CTRL_STATUSBAR:
        switch (msg) {
        case SB_SETPARTS: {
            int n = (int)(int32_t)wp;
            if (n < 1) n = 1; if (n > CC_SB_PARTS) n = CC_SB_PARTS;
            cc->sb_nparts = n; win32_refresh_controls(c->parent_fd); return 1;
        }
        case SB_SETTEXTA: {
            int part = (int)(wp & 0xFF);
            if (part < 0 || part >= CC_SB_PARTS) return 0;
            cc->sb_part[part][0] = 0;
            if (lp) (void)strncpy_from_user(cc->sb_part[part], (const char *)(uintptr_t)lp, CC_TEXT);
            if (part + 1 > cc->sb_nparts) cc->sb_nparts = part + 1;
            win32_refresh_controls(c->parent_fd);
            return 1;
        }
        case SB_GETTEXTA: {
            int part = (int)(wp & 0xFF);
            if (part < 0 || part >= CC_SB_PARTS) return 0;
            int n = 0; while (cc->sb_part[part][n]) n++;
            if (lp) {
                (void)copy_to_user((void *)(uintptr_t)lp, cc->sb_part[part], n);
                char z = 0; (void)copy_to_user((void *)(uintptr_t)(lp + n), &z, 1);
            }
            return n;
        }
        case SB_GETTEXTLENGTHA: {
            int part = (int)(wp & 0xFF);
            if (part < 0 || part >= CC_SB_PARTS) return 0;
            int n = 0; while (cc->sb_part[part][n]) n++;
            return n;
        }
        case SB_GETPARTS: return cc->sb_nparts;
        }
        return 0;
    case WIN32_CTRL_PROGRESS:
        switch (msg) {
        case PBM_SETRANGE:
            cc->pb_min = (int)(lp & 0xFFFF); cc->pb_max = (int)((lp >> 16) & 0xFFFF);
            win32_refresh_controls(c->parent_fd); return 1;
        case PBM_SETRANGE32:
            cc->pb_min = (int)(int32_t)wp; cc->pb_max = (int)(int32_t)lp;
            win32_refresh_controls(c->parent_fd); return 1;
        case PBM_SETPOS: {
            int old = cc->pb_pos; cc->pb_pos = (int)(int32_t)wp;
            if (cc->pb_pos < cc->pb_min) cc->pb_pos = cc->pb_min;
            if (cc->pb_pos > cc->pb_max) cc->pb_pos = cc->pb_max;
            win32_refresh_controls(c->parent_fd); return old;
        }
        case PBM_DELTAPOS: {
            int old = cc->pb_pos; cc->pb_pos += (int)(int32_t)wp;
            if (cc->pb_pos > cc->pb_max) cc->pb_pos = cc->pb_max;
            win32_refresh_controls(c->parent_fd); return old;
        }
        case PBM_SETSTEP: { int old = cc->pb_step; cc->pb_step = (int)(int32_t)wp; return old; }
        case PBM_STEPIT: {
            int old = cc->pb_pos; cc->pb_pos += cc->pb_step;
            if (cc->pb_pos > cc->pb_max) cc->pb_pos = cc->pb_max;
            win32_refresh_controls(c->parent_fd); return old;
        }
        case PBM_GETPOS: return cc->pb_pos;
        }
        return 0;
    case WIN32_CTRL_TOOLBAR:
        switch (msg) {
        case TB_ADDBUTTONS:  cc->tb_n += (int)(int32_t)wp;
                             win32_refresh_controls(c->parent_fd); return 1;
        case TB_BUTTONCOUNT: return cc->tb_n;
        }
        return 0;
    }
    return 0;
}

/* user32!SendMessageA(hwnd, msg, wParam, lParam) -> result. Handles the control
 * messages used to drive BUTTON(checkbox)/EDIT/LISTBOX controls (the kernel IS
 * the control's WndProc); non-control targets / unknown messages return 0. */
static long w32_SendMessageA(uint64_t *a) {
    struct win32_ctrl *c = win32_ctrl_find(a[0]);
    if (!c) return 0;
    if (win32_is_comctl(c->type))
        return win32_cc_sendmessage(c, win32_ctrl_slot(c),
                                    (uint32_t)a[1], a[2], a[3]);
    uint32_t msg = (uint32_t)a[1];
    uint64_t wp = a[2], lp = a[3];
    int slot = win32_ctrl_slot(c);
    struct win32_lb *lb = &g_win32_lb[slot];
    switch (msg) {
    case BM_GETCHECK:   return c->checked ? 1 : 0;
    case BM_SETCHECK:   c->checked = (wp != 0); win32_refresh_controls(c->parent_fd); return 0;
    case WM_SETTEXT:
        c->text[0] = 0;
        if (lp) (void)strncpy_from_user(c->text, (const char *)(uintptr_t)lp, sizeof(c->text));
        win32_refresh_controls(c->parent_fd);
        return 1;
    case LB_ADDSTRING:
        if (lb->n >= WIN32_LB_MAX_ITEMS) return -2 /* LB_ERR */;
        lb->items[lb->n][0] = 0;
        if (lp) (void)strncpy_from_user(lb->items[lb->n], (const char *)(uintptr_t)lp, WIN32_LB_ITEM_LEN);
        lb->n++;
        win32_refresh_controls(c->parent_fd);
        return lb->n - 1;
    case LB_RESETCONTENT: lb->n = 0; lb->sel = -1; win32_refresh_controls(c->parent_fd); return 0;
    case LB_SETCURSEL:    lb->sel = (int)(int32_t)wp; win32_refresh_controls(c->parent_fd); return lb->sel;
    case LB_GETCURSEL:    return lb->sel;
    case LB_GETCOUNT:     return lb->n;
    case LB_GETTEXT: {
        int idx = (int)(int32_t)wp;
        if (idx < 0 || idx >= lb->n) return -2;
        int n = 0; while (n < WIN32_LB_ITEM_LEN - 1 && lb->items[idx][n]) n++;
        if (lp) {
            (void)copy_to_user((void *)(uintptr_t)lp, lb->items[idx], n);
            char z = 0; (void)copy_to_user((void *)(uintptr_t)(lp + n), &z, 1);
        }
        return n;
    }
    /* ---- C14 COMBOBOX (CB_*) ---- reuse the listbox item store ---- */
    case CB_ADDSTRING:
        if (lb->n >= WIN32_LB_MAX_ITEMS) return -1 /* CB_ERR */;
        lb->items[lb->n][0] = 0;
        if (lp) (void)strncpy_from_user(lb->items[lb->n], (const char *)(uintptr_t)lp, WIN32_LB_ITEM_LEN);
        lb->n++;
        win32_refresh_controls(c->parent_fd);
        return lb->n - 1;
    case CB_RESETCONTENT: lb->n = 0; lb->sel = -1; win32_refresh_controls(c->parent_fd); return 0;
    case CB_GETCOUNT:     return lb->n;
    case CB_SETCURSEL:
        lb->sel = (int)(int32_t)wp;
        if (lb->sel < -1) lb->sel = -1;
        if (lb->sel >= lb->n) lb->sel = lb->n - 1;
        win32_refresh_controls(c->parent_fd);
        return lb->sel;
    case CB_GETCURSEL:    return lb->sel;
    case CB_GETLBTEXT: {
        int idx = (int)(int32_t)wp;
        if (idx < 0 || idx >= lb->n) return -1 /* CB_ERR */;
        int n = 0; while (n < WIN32_LB_ITEM_LEN - 1 && lb->items[idx][n]) n++;
        if (lp) {
            (void)copy_to_user((void *)(uintptr_t)lp, lb->items[idx], n);
            char z = 0; (void)copy_to_user((void *)(uintptr_t)(lp + n), &z, 1);
        }
        return n;
    }
    /* ---- C14 TAB (SysTabControl32, TCM_*) ---- tabs in the item store ---- */
    case TCM_INSERTITEMA: {
        /* lParam = TCITEM; pszText (char*) at offset 0x10 on x64. */
        if (lb->n >= WIN32_LB_MAX_ITEMS) return -1;
        lb->items[lb->n][0] = 0;
        if (lp) {
            uint64_t pszText = 0;
            if (copy_from_user(&pszText, (const void *)(uintptr_t)(lp + 0x10), 8) == 0 && pszText)
                (void)strncpy_from_user(lb->items[lb->n], (const char *)(uintptr_t)pszText,
                                        WIN32_LB_ITEM_LEN);
        }
        if (lb->sel < 0) lb->sel = 0;       /* first inserted tab is selected */
        int idx = lb->n++;
        win32_refresh_controls(c->parent_fd);
        return idx;
    }
    case TCM_GETITEMCOUNT: return lb->n;
    case TCM_GETCURSEL:    return lb->sel;
    case TCM_SETCURSEL: {
        int old = lb->sel;
        lb->sel = (int)(int32_t)wp;
        if (lb->sel < 0) lb->sel = 0;
        if (lb->sel >= lb->n) lb->sel = lb->n - 1;
        win32_refresh_controls(c->parent_fd);
        return old;
    }
    default: return 0;
    }
}

/* user32 scrollbar API on a SCROLLBAR control (and tolerated on windows). */
static long w32_SetScrollRange(uint64_t *a) {       /* (hwnd, bar, min, max, redraw) */
    struct win32_ctrl *c = win32_ctrl_find(a[0]);
    if (!c || c->type != WIN32_CTRL_SCROLLBAR) return 0;
    c->sb_min = (int)(int32_t)a[2]; c->sb_max = (int)(int32_t)a[3];
    win32_refresh_controls(c->parent_fd); return 1;
}
static long w32_SetScrollPos(uint64_t *a) {         /* (hwnd, bar, pos, redraw) -> old */
    struct win32_ctrl *c = win32_ctrl_find(a[0]);
    if (!c || c->type != WIN32_CTRL_SCROLLBAR) return 0;
    int old = c->sb_pos; c->sb_pos = (int)(int32_t)a[2];
    if (c->sb_pos < c->sb_min) c->sb_pos = c->sb_min;
    if (c->sb_pos > c->sb_max) c->sb_pos = c->sb_max;
    win32_refresh_controls(c->parent_fd); return old;
}
static long w32_GetScrollPos(uint64_t *a) {         /* (hwnd, bar) -> pos */
    struct win32_ctrl *c = win32_ctrl_find(a[0]);
    return (c && c->type == WIN32_CTRL_SCROLLBAR) ? c->sb_pos : 0;
}
/* SetScrollInfo(hwnd, bar, &SCROLLINFO{cbSize,fMask,nMin@8,nMax@0xC,nPage@0x10,
 * nPos@0x14,...}, redraw). */
static long w32_SetScrollInfo(uint64_t *a) {
    struct win32_ctrl *c = win32_ctrl_find(a[0]);
    if (!c || c->type != WIN32_CTRL_SCROLLBAR) return 0;
    int32_t v[4];   /* nMin, nMax, nPage, nPos at +8..+0x14 */
    if (copy_from_user(v, (const void *)(uintptr_t)(a[2] + 8), sizeof(v)) == 0) {
        c->sb_min = v[0]; c->sb_max = v[1]; c->sb_pos = v[3];
        win32_refresh_controls(c->parent_fd);
    }
    return c->sb_max;
}
static long w32_GetScrollInfo(uint64_t *a) {
    struct win32_ctrl *c = win32_ctrl_find(a[0]);
    if (!c || c->type != WIN32_CTRL_SCROLLBAR) return 0;
    int32_t v[4] = { c->sb_min, c->sb_max, 1, c->sb_pos };
    (void)copy_to_user((void *)(uintptr_t)(a[2] + 8), v, sizeof(v));
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

/* kernel32!LoadLibraryExA(name, hFile, flags) -> HMODULE. The extra args don't
 * change which built-in DLL we resolve; same as LoadLibraryA on a[0]. (C19b:
 * real Lua's package loader imports this; it only matters if a C module is
 * require()d, which resolves to NULL here -- pure-Lua require still works.) */
static long w32_LoadLibraryExA(uint64_t *a) { return w32_GetModuleHandleA(a); }

/* kernel32!GetModuleFileNameA(hModule, lpFilename, nSize) -> length. Returns the
 * running PE's path; Lua's package lib calls this to derive the default
 * package.path/cpath prefix. (C19b) */
static long w32_GetModuleFileNameA(uint64_t *a) {
    uint64_t buf = a[1]; uint32_t cap = (uint32_t)a[2];
    if (!buf || !cap) return 0;
    const char *path = "C:\\win-lua.exe";
    uint32_t n = 0; while (path[n]) n++;
    if (n > cap - 1) n = cap - 1;
    if (copy_to_user((void *)(uintptr_t)buf, path, n) != 0) return 0;
    char z = 0; (void)copy_to_user((void *)(uintptr_t)(buf + n), &z, 1);
    return (long)n;
}

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

/* ================= C16a: registry (advapi32) =================
 * A small in-kernel registry tree backing the Reg* API. Predefined roots
 * (HKLM/HKCU/HKCR/HKU) are fixed keys; an opened HKEY is a tagged token into the
 * key table. Writes are flushed to /data/toby_registry.dat (vfs_write_all) so an
 * app's settings survive a reboot; the hive is lazy-loaded on first use. */
#define WIN32_HKEY_TAG    0x6700000000000000ull   /* distinct: menu 0x66 / font 0x65 */
#define REG_MAX_KEYS      24
#define REG_MAX_VALS      4
#define REG_NAME_LEN      40
#define REG_DATA_LEN      48
#define REG_PATH          "/data/toby_registry.dat"
#define REG_MAGIC         0x52475431u   /* 'RGT1' -- hive format v1 */

#define ERROR_SUCCESS         0
#define ERROR_FILE_NOT_FOUND  2
#define ERROR_NO_MORE_ITEMS   259
#define REG_SZ                1
#define REG_BINARY            3
#define REG_DWORD             4

/* C16a: flush dirty bcache to the backing disk so registry writes reach /data's
 * disk image even on an unclean shutdown (a forced kill skips shutdown sync). */
struct blk_dev;
extern struct blk_dev *blk_get(size_t idx);
extern void bcache_sync(struct blk_dev *dev);
static void reg_sync_disks(void) {
    int n = 0;
    for (size_t i = 0; ; i++) {
        struct blk_dev *d = blk_get(i);
        if (!d) break;
        bcache_sync(d); n++;
    }
    if (g_win32_log_input) kprintf("[winreg] synced %d block device(s)\n", n);
}

struct reg_value { char name[REG_NAME_LEN]; uint32_t type, len; uint8_t data[REG_DATA_LEN]; };
struct reg_key {
    bool in_use; int parent; char name[REG_NAME_LEN];
    int nvals; struct reg_value vals[REG_MAX_VALS];
};
static struct { uint32_t magic, version, nkeys, reserved; struct reg_key keys[REG_MAX_KEYS]; } g_reg;
static bool g_reg_loaded;

static void reg_seed_roots(void) {
    /* roots 0..3 = HKEY_CLASSES_ROOT / HKCU / HKLM / HKEY_USERS */
    static const char *rn[4] = { "CLASSES_ROOT", "CURRENT_USER", "LOCAL_MACHINE", "USERS" };
    for (int i = 0; i < 4; i++) {
        memset(&g_reg.keys[i], 0, sizeof(g_reg.keys[i]));
        g_reg.keys[i].in_use = true; g_reg.keys[i].parent = -1;
        int n = 0; while (rn[i][n] && n < REG_NAME_LEN - 1) { g_reg.keys[i].name[n] = rn[i][n]; n++; }
        g_reg.keys[i].name[n] = 0;
    }
}
static void reg_ensure_loaded(void) {
    if (g_reg_loaded) return;
    g_reg_loaded = true;
    void *buf = 0; size_t sz = 0;
    if (vfs_read_all(REG_PATH, &buf, &sz) == 0 && buf && sz >= sizeof(g_reg)) {
        memcpy(&g_reg, buf, sizeof(g_reg));   /* the hive is at offset 0 */
        kfree(buf);
        if (g_reg.magic == REG_MAGIC) {
            kprintf("[winreg] loaded hive %s (%d keys)\n", REG_PATH, (int)g_reg.nkeys);
            return;
        }
    } else if (buf) kfree(buf);
    memset(&g_reg, 0, sizeof(g_reg));
    g_reg.magic = REG_MAGIC; g_reg.version = 1;
    reg_seed_roots();
    kprintf("[winreg] fresh hive (no %s)\n", REG_PATH);
}
static void reg_flush(void) {
    g_reg.nkeys = 0;
    for (int i = 0; i < REG_MAX_KEYS; i++) if (g_reg.keys[i].in_use) g_reg.nkeys++;
    (void)vfs_unlink(REG_PATH);   /* clean slate -- vfs_create doesn't truncate */
    int cr = vfs_create(REG_PATH);
    if (cr != VFS_OK && cr != VFS_ERR_EXIST) {
        if (g_win32_log_input) kprintf("[winreg] flush create failed rc=%d\n", cr);
        return;
    }
    struct vfs_file f;
    if (vfs_open(REG_PATH, &f) != VFS_OK) return;
    /* Write in chunks (a single large vfs_write can fail on tobyfs). */
    const uint8_t *p = (const uint8_t *)&g_reg;
    size_t total = sizeof(g_reg), off = 0;
    bool ok = true;
    while (off < total) {
        size_t chunk = total - off; if (chunk > 4096) chunk = 4096;
        long w = vfs_write(&f, p + off, chunk);
        if (w <= 0) { ok = false; break; }
        off += (size_t)w;
    }
    vfs_close(&f);
    if (ok) reg_sync_disks();    /* make the write durable on the disk image */
    if (g_win32_log_input)
        kprintf("[winreg] flush %s -> %s (%lu bytes, %d keys)\n",
                REG_PATH, ok ? "ok" : "IO-FAIL", (unsigned long)off, (int)g_reg.nkeys);
}
/* Predefined root or tagged HKEY -> key index, or -1. The predefined HKEYs
 * (HKEY_CURRENT_USER etc.) are (HKEY)(LONG_PTR)0x8000000n, which compilers
 * SIGN-extend to 0xFFFFFFFF8000000n -- so match them on the low 32 bits, and
 * check our tagged handle (top byte 0x67) first to disambiguate. */
static int reg_resolve(uint64_t hkey) {
    reg_ensure_loaded();
    if ((hkey & WIN32_TAG_MASK) == WIN32_HKEY_TAG) {
        int idx = (int)(hkey & 0xFF);
        if (idx >= 0 && idx < REG_MAX_KEYS && g_reg.keys[idx].in_use) return idx;
        return -1;
    }
    uint32_t lo = (uint32_t)hkey;
    if (lo >= 0x80000000u && lo <= 0x800000FFu) {
        int r = (int)(lo - 0x80000000u);
        return (r >= 0 && r < 4) ? r : 2;   /* unknown predefined -> HKLM */
    }
    return -1;
}
static bool reg_name_eq(const char *a, const char *b) {   /* case-insensitive */
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return false;
        if (!ca) return true;
    }
}
static int reg_find_child(int parent, const char *name) {
    for (int i = 0; i < REG_MAX_KEYS; i++)
        if (g_reg.keys[i].in_use && g_reg.keys[i].parent == parent &&
            reg_name_eq(g_reg.keys[i].name, name)) return i;
    return -1;
}
static int reg_alloc_child(int parent, const char *name) {
    for (int i = 4; i < REG_MAX_KEYS; i++)
        if (!g_reg.keys[i].in_use) {
            memset(&g_reg.keys[i], 0, sizeof(g_reg.keys[i]));
            g_reg.keys[i].in_use = true; g_reg.keys[i].parent = parent;
            int n = 0; while (name[n] && n < REG_NAME_LEN - 1) { g_reg.keys[i].name[n] = name[n]; n++; }
            g_reg.keys[i].name[n] = 0;
            return i;
        }
    return -1;
}
/* Walk a "Sub\\Path" relative to root key `idx`; create=true makes missing keys. */
static int reg_walk(int idx, const char *path, bool create) {
    char comp[REG_NAME_LEN]; int ci = 0;
    for (const char *p = path; ; p++) {
        if (*p == '\\' || *p == '/' || *p == 0) {
            if (ci > 0) {
                comp[ci] = 0;
                int child = reg_find_child(idx, comp);
                if (child < 0) {
                    if (!create) return -1;
                    child = reg_alloc_child(idx, comp);
                    if (child < 0) return -1;
                }
                idx = child; ci = 0;
            }
            if (*p == 0) break;
        } else if (ci < REG_NAME_LEN - 1) comp[ci++] = *p;
    }
    return idx;
}
static struct reg_value *reg_find_value(int idx, const char *name) {
    struct reg_key *k = &g_reg.keys[idx];
    for (int i = 0; i < k->nvals; i++)
        if (reg_name_eq(k->vals[i].name, name)) return &k->vals[i];
    return NULL;
}

/* advapi32!RegCreateKeyExA(hKey, lpSubKey, Res, lpClass, dwOpts, sam, lpSA,
 * phkResult=a7, lpdwDisposition=a8) -> ERROR_SUCCESS. */
static long w32_RegCreateKeyExA(uint64_t *a) {
    int root = reg_resolve(a[0]);
    if (root < 0) return ERROR_FILE_NOT_FOUND;
    char sub[160]; sub[0] = 0;
    if (a[1]) (void)strncpy_from_user(sub, (const char *)(uintptr_t)a[1], sizeof(sub));
    int idx = sub[0] ? reg_walk(root, sub, true) : root;
    if (idx < 0) return ERROR_FILE_NOT_FOUND;
    uint64_t h = WIN32_HKEY_TAG | (uint64_t)idx;
    if (a[7]) (void)copy_to_user((void *)(uintptr_t)a[7], &h, 8);
    if (a[8]) { uint32_t disp = 1; (void)copy_to_user((void *)(uintptr_t)a[8], &disp, 4); }
    reg_flush();
    if (g_win32_log_input) kprintf("[winreg] CreateKey '%s' -> key#%d\n", sub, idx);
    return ERROR_SUCCESS;
}
/* RegOpenKeyExA(hKey, lpSubKey, ulOptions, sam, phkResult=a4) -> ERROR_SUCCESS / NOT_FOUND. */
static long w32_RegOpenKeyExA(uint64_t *a) {
    int root = reg_resolve(a[0]);
    if (root < 0) return ERROR_FILE_NOT_FOUND;
    char sub[160]; sub[0] = 0;
    if (a[1]) (void)strncpy_from_user(sub, (const char *)(uintptr_t)a[1], sizeof(sub));
    int idx = sub[0] ? reg_walk(root, sub, false) : root;
    if (idx < 0) return ERROR_FILE_NOT_FOUND;
    uint64_t h = WIN32_HKEY_TAG | (uint64_t)idx;
    if (a[4]) (void)copy_to_user((void *)(uintptr_t)a[4], &h, 8);
    return ERROR_SUCCESS;
}
static long w32_RegCloseKey(uint64_t *a) { (void)a; return ERROR_SUCCESS; }
/* RegSetValueExA(hKey, lpValueName, Res, dwType, lpData, cbData) -> ERROR_SUCCESS. */
static long w32_RegSetValueExA(uint64_t *a) {
    int idx = reg_resolve(a[0]);
    if (idx < 0) return ERROR_FILE_NOT_FOUND;
    char vn[REG_NAME_LEN]; vn[0] = 0;
    if (a[1]) (void)strncpy_from_user(vn, (const char *)(uintptr_t)a[1], sizeof(vn));
    uint32_t type = (uint32_t)a[3], len = (uint32_t)a[5];
    if (len > REG_DATA_LEN) len = REG_DATA_LEN;
    struct reg_value *v = reg_find_value(idx, vn);
    if (!v) {
        struct reg_key *k = &g_reg.keys[idx];
        if (k->nvals >= REG_MAX_VALS) return ERROR_FILE_NOT_FOUND;
        v = &k->vals[k->nvals++];
        memset(v, 0, sizeof(*v));
        int n = 0; while (vn[n] && n < REG_NAME_LEN - 1) { v->name[n] = vn[n]; n++; } v->name[n] = 0;
    }
    v->type = type; v->len = len;
    if (len && a[4]) (void)copy_from_user(v->data, (const void *)(uintptr_t)a[4], len);
    reg_flush();
    if (g_win32_log_input) kprintf("[winreg] SetValue key#%d '%s' type=%u len=%u\n", idx, vn, type, len);
    return ERROR_SUCCESS;
}
/* RegQueryValueExA(hKey, lpValueName, lpRes, lpType=a3, lpData=a4, lpcbData=a5)
 * -> ERROR_SUCCESS / NOT_FOUND. */
static long w32_RegQueryValueExA(uint64_t *a) {
    int idx = reg_resolve(a[0]);
    if (idx < 0) return ERROR_FILE_NOT_FOUND;
    char vn[REG_NAME_LEN]; vn[0] = 0;
    if (a[1]) (void)strncpy_from_user(vn, (const char *)(uintptr_t)a[1], sizeof(vn));
    struct reg_value *v = reg_find_value(idx, vn);
    if (!v) return ERROR_FILE_NOT_FOUND;
    if (a[3]) (void)copy_to_user((void *)(uintptr_t)a[3], &v->type, 4);
    uint32_t cap = 0;
    if (a[5]) (void)copy_from_user(&cap, (const void *)(uintptr_t)a[5], 4);
    if (a[4] && v->len) {
        uint32_t n = (a[5] && cap < v->len) ? cap : v->len;
        (void)copy_to_user((void *)(uintptr_t)a[4], v->data, n);
    }
    if (a[5]) (void)copy_to_user((void *)(uintptr_t)a[5], &v->len, 4);
    return ERROR_SUCCESS;
}
static long w32_RegDeleteValueA(uint64_t *a) {
    int idx = reg_resolve(a[0]);
    if (idx < 0) return ERROR_FILE_NOT_FOUND;
    char vn[REG_NAME_LEN]; vn[0] = 0;
    if (a[1]) (void)strncpy_from_user(vn, (const char *)(uintptr_t)a[1], sizeof(vn));
    struct reg_key *k = &g_reg.keys[idx];
    for (int i = 0; i < k->nvals; i++)
        if (reg_name_eq(k->vals[i].name, vn)) {
            k->vals[i] = k->vals[--k->nvals];
            reg_flush();
            return ERROR_SUCCESS;
        }
    return ERROR_FILE_NOT_FOUND;
}
static long w32_RegDeleteKeyA(uint64_t *a) {
    int root = reg_resolve(a[0]);
    if (root < 0) return ERROR_FILE_NOT_FOUND;
    char sub[160]; sub[0] = 0;
    if (a[1]) (void)strncpy_from_user(sub, (const char *)(uintptr_t)a[1], sizeof(sub));
    int idx = sub[0] ? reg_walk(root, sub, false) : -1;
    if (idx < 4) return ERROR_FILE_NOT_FOUND;       /* never delete a root */
    g_reg.keys[idx].in_use = false;                 /* (children left orphaned; ok) */
    reg_flush();
    return ERROR_SUCCESS;
}
/* RegEnumKeyExA(hKey, dwIndex, lpName=a2, lpcchName=a3, ...) -> ERROR_SUCCESS/NO_MORE_ITEMS. */
static long w32_RegEnumKeyExA(uint64_t *a) {
    int parent = reg_resolve(a[0]);
    if (parent < 0) return ERROR_FILE_NOT_FOUND;
    int want = (int)(uint32_t)a[1], seen = 0;
    for (int i = 0; i < REG_MAX_KEYS; i++) {
        if (!g_reg.keys[i].in_use || g_reg.keys[i].parent != parent) continue;
        if (seen++ != want) continue;
        int n = 0; while (g_reg.keys[i].name[n]) n++;
        uint32_t cap = 0; if (a[3]) (void)copy_from_user(&cap, (const void *)(uintptr_t)a[3], 4);
        if (cap && (uint32_t)n + 1 > cap) n = (int)cap - 1;
        if (a[2] && n >= 0) {
            (void)copy_to_user((void *)(uintptr_t)a[2], g_reg.keys[i].name, n);
            char z = 0; (void)copy_to_user((void *)(uintptr_t)(a[2] + n), &z, 1);
        }
        uint32_t outn = (uint32_t)n; if (a[3]) (void)copy_to_user((void *)(uintptr_t)a[3], &outn, 4);
        return ERROR_SUCCESS;
    }
    return ERROR_NO_MORE_ITEMS;
}
/* RegEnumValueA(hKey, dwIndex, lpName=a2, lpcchName=a3, lpRes, lpType=a5, lpData=a6, lpcbData=a7). */
static long w32_RegEnumValueA(uint64_t *a) {
    int idx = reg_resolve(a[0]);
    if (idx < 0) return ERROR_FILE_NOT_FOUND;
    int want = (int)(uint32_t)a[1];
    struct reg_key *k = &g_reg.keys[idx];
    if (want < 0 || want >= k->nvals) return ERROR_NO_MORE_ITEMS;
    struct reg_value *v = &k->vals[want];
    int n = 0; while (v->name[n]) n++;
    if (a[2]) { (void)copy_to_user((void *)(uintptr_t)a[2], v->name, n);
                char z = 0; (void)copy_to_user((void *)(uintptr_t)(a[2] + n), &z, 1); }
    uint32_t outn = (uint32_t)n; if (a[3]) (void)copy_to_user((void *)(uintptr_t)a[3], &outn, 4);
    if (a[5]) (void)copy_to_user((void *)(uintptr_t)a[5], &v->type, 4);
    if (a[6] && v->len) (void)copy_to_user((void *)(uintptr_t)a[6], v->data, v->len);
    if (a[7]) (void)copy_to_user((void *)(uintptr_t)a[7], &v->len, 4);
    return ERROR_SUCCESS;
}

/* ================= C16b: common file dialogs (comdlg32) =================
 * GetOpenFileNameA/GetSaveFileNameA as a REAL modal file picker over /data
 * (== "C:"): it lists the current directory, lets the user pick/navigate, and
 * writes the chosen "C:\..." path into the OPENFILENAMEA.lpstrFile buffer.
 * OPENFILENAMEA x64 offsets: lpstrFile@0x30, nMaxFile@0x38, lpstrInitialDir@0x50,
 * lpstrTitle@0x58, Flags@0x60. */
#define OFD_MAX_ENTRIES 48
#define OFD_ROW_H       16
struct ofd_entry { char name[40]; bool is_dir; };

/* leaf name of a path (after the last '\\' or '/'). */
static const char *ofd_leaf(const char *p) {
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') s = q + 1;
    return s;
}
/* List "/data/<curdir>" into ents[]; returns count. Adds ".." if not at root. */
static int ofd_list(const char *curdir, struct ofd_entry *ents, int cap) {
    char dpath[192];
    int n = 0; const char *base = "/data";
    while (base[n] && n < (int)sizeof(dpath) - 1) { dpath[n] = base[n]; n++; }
    if (curdir[0]) { if (n < (int)sizeof(dpath) - 1) dpath[n++] = '/';
        for (const char *p = curdir; *p && n < (int)sizeof(dpath) - 1; p++) dpath[n++] = *p; }
    dpath[n] = 0;
    int cnt = 0;
    if (curdir[0]) {   /* parent entry */
        ents[cnt].is_dir = true; ents[cnt].name[0] = '.'; ents[cnt].name[1] = '.'; ents[cnt].name[2] = 0; cnt++;
    }
    struct vfs_dir d;
    if (vfs_opendir(dpath, &d) == VFS_OK) {
        struct vfs_dirent de;
        while (cnt < cap && vfs_readdir(&d, &de) == VFS_OK) {
            if (de.name[0] == '.' && de.name[1] == 0) continue;      /* skip "." */
            if (de.name[0] == '.' && de.name[1] == '.' && de.name[2] == 0) continue;
            int k = 0; while (de.name[k] && k < 39) { ents[cnt].name[k] = de.name[k]; k++; }
            ents[cnt].name[k] = 0;
            ents[cnt].is_dir = (de.type == VFS_TYPE_DIR);
            cnt++;
        }
        vfs_closedir(&d);
    }
    return cnt;
}

static long win32_file_dialog(uint64_t ofn, bool save) {
    if (!ofn) return 0;
    uint64_t lpFile = 0, lpTitle = 0; uint32_t nMax = 0;
    (void)copy_from_user(&lpFile,  (const void *)(uintptr_t)(ofn + 0x30), 8);
    (void)copy_from_user(&nMax,    (const void *)(uintptr_t)(ofn + 0x38), 4);
    (void)copy_from_user(&lpTitle, (const void *)(uintptr_t)(ofn + 0x58), 8);
    char field[48]; field[0] = 0;
    if (lpFile) { char tmp[160]; tmp[0] = 0;
        (void)strncpy_from_user(tmp, (const char *)(uintptr_t)lpFile, sizeof(tmp));
        const char *lf = ofd_leaf(tmp);
        int i = 0; while (lf[i] && i < (int)sizeof(field) - 1) { field[i] = lf[i]; i++; } field[i] = 0; }
    char title[40]; title[0] = 0;
    if (lpTitle) (void)strncpy_from_user(title, (const char *)(uintptr_t)lpTitle, sizeof(title));
    const char *deftitle = save ? "Save As" : "Open";

    const int W = 420, H = 344;
    int fd = -1;
    if (cap_check(current_proc(), CAP_GUI, "FileDialog")) {
        struct window *dw = gui_window_create(W, H, title[0] ? title : deftitle);
        if (dw) {
            struct file *df = (struct file *)kmalloc(sizeof(*df));
            if (df) { memset(df, 0, sizeof(*df)); df->kind = FILE_KIND_WINDOW; df->win = dw;
                      fd = fd_alloc_into(current_proc(), df);
                      if (fd < 0) { kfree(df); gui_window_close(dw); } }
            else gui_window_close(dw);
        }
    }
    if (fd < 0) return 0;
    struct window *win = win32_hdc_window(fd);

    char curdir[128]; curdir[0] = 0;             /* relative to /data */
    static struct ofd_entry ents[OFD_MAX_ENTRIES];
    int nents = 0, sel = -1, top = 0;
    const int listX = 12, listY = 56, listW = W - 24, listH = OFD_ROW_H * 14;
    const int okx = W - 180, oky = H - 40, okw = 76, okh = 26;
    const int cax = W - 92,  cay = oky,    caw = 76, cah = okh;
    bool relist = true, redraw = true;
    long result = 0;

    uint64_t hz = pit_hz(); if (!hz) hz = 100;
    uint64_t deadline = pit_ticks() + 40 * hz;
    while (pit_ticks() < deadline) {
        if (relist) {
            nents = ofd_list(curdir, ents, OFD_MAX_ENTRIES);
            sel = -1;
            for (int i = 0; i < nents; i++)
                if (!ents[i].is_dir && win32_streq_ci(ents[i].name, field)) { sel = i; break; }
            relist = false; redraw = true;
        }
        if (redraw && win) {
            (void)gui_window_fill(win, 0, 0, W, H, 0x00DCDCDC);
            char look[160]; int li = 0;
            const char *pfx = "Look in:  C:\\";
            for (const char *p = pfx; *p && li < 150; p++) look[li++] = *p;
            for (const char *p = curdir; *p && li < 150; p++) look[li++] = (*p == '/') ? '\\' : *p;
            look[li] = 0;
            (void)win32_ui_text(win, 12, 14, look, 0x00101010, 0x00DCDCDC);
            (void)gui_window_fill(win, listX, listY, listW, listH, 0x00FFFFFF);
            (void)gui_window_rect(win, listX, listY, listW, listH, 0x00808080);
            for (int r = 0; r < nents && r < listH / OFD_ROW_H; r++) {
                int i = top + r; if (i >= nents) break;
                int ry = listY + 2 + r * OFD_ROW_H;
                uint32_t bg = (i == sel) ? 0x002E8AE0 : 0x00FFFFFF;
                uint32_t fg = (i == sel) ? 0x00FFFFFF : 0x00101010;
                if (i == sel) (void)gui_window_fill(win, listX + 1, ry - 1, listW - 2, OFD_ROW_H, bg);
                char line[48]; int c = 0;
                if (ents[i].is_dir) { const char *d = "[ ] "; for (int k = 0; d[k]; k++) line[c++] = d[k]; line[1] = '+'; }
                for (int k = 0; ents[i].name[k] && c < 46; k++) line[c++] = ents[i].name[k];
                line[c] = 0;
                (void)win32_ui_text(win, listX + 6, ry + 1, line, fg, bg);
            }
            /* filename field */
            int fy = listY + listH + 10;
            (void)win32_ui_text(win, 12, fy + 4, "Name:", 0x00101010, 0x00DCDCDC);
            (void)gui_window_fill(win, 60, fy, W - 72, 20, 0x00FFFFFF);
            (void)gui_window_rect(win, 60, fy, W - 72, 20, 0x00808080);
            (void)win32_ui_text(win, 65, fy + 6, field[0] ? field : " ", 0x00000000, 0x00FFFFFF);
            /* buttons */
            (void)gui_window_fill(win, okx, oky, okw, okh, 0x00C0C0C0);
            (void)gui_window_rect(win, okx, oky, okw, okh, 0x00303030);
            (void)win32_ui_text(win, okx + okw / 2 - (save ? 16 : 16), oky + okh / 2 - 4,
                                  save ? "Save" : "Open", 0x00101010, 0x00C0C0C0);
            (void)gui_window_fill(win, cax, cay, caw, cah, 0x00C0C0C0);
            (void)gui_window_rect(win, cax, cay, caw, cah, 0x00303030);
            (void)win32_ui_text(win, cax + caw / 2 - 24, cay + cah / 2 - 4, "Cancel", 0x00101010, 0x00C0C0C0);
            (void)sys_gui_flip(fd);
            redraw = false;
        }
        struct gui_event ev;
        if (win32_poll_window_event(fd, &ev) > 0) {
            if (ev.type == GUI_EV_CLOSE) { result = 0; break; }
            if (ev.type == GUI_EV_MOUSE_DOWN) {
                if (ev.x >= listX && ev.x < listX + listW && ev.y >= listY && ev.y < listY + listH) {
                    int r = (ev.y - (listY + 2)) / OFD_ROW_H, i = top + r;
                    if (i >= 0 && i < nents) {
                        sel = i;
                        if (!ents[i].is_dir) {
                            int k = 0; while (ents[i].name[k] && k < (int)sizeof(field) - 1) { field[k] = ents[i].name[k]; k++; } field[k] = 0;
                        }
                        redraw = true;
                    }
                } else if (ev.x >= okx && ev.x < okx + okw && ev.y >= oky && ev.y < oky + okh) {
                    if (sel >= 0 && ents[sel].is_dir) {     /* navigate into / up */
                        if (ents[sel].name[0] == '.' && ents[sel].name[1] == '.') {
                            int e = 0; while (curdir[e]) e++;
                            while (e > 0 && curdir[e - 1] != '/') e--;
                            if (e > 0) e--; curdir[e] = 0;
                        } else {
                            int e = 0; while (curdir[e]) e++;
                            if (e && e < (int)sizeof(curdir) - 1) curdir[e++] = '/';
                            for (int k = 0; ents[sel].name[k] && e < (int)sizeof(curdir) - 1; k++) curdir[e++] = ents[sel].name[k];
                            curdir[e] = 0;
                        }
                        relist = true;
                    } else if (field[0]) {                  /* pick the file */
                        char res[200]; int e = 0;
                        const char *cp = "C:\\"; for (int k = 0; cp[k]; k++) res[e++] = cp[k];
                        for (const char *p = curdir; *p && e < 190; p++) res[e++] = (*p == '/') ? '\\' : *p;
                        if (curdir[0] && e < 190) res[e++] = '\\';
                        for (const char *p = field; *p && e < 198; p++) res[e++] = *p;
                        res[e] = 0;
                        if (lpFile) {
                            int cap = (int)nMax; if (cap <= 0 || cap > (int)sizeof(res)) cap = (int)sizeof(res);
                            int wl = e; if (wl > cap - 1) wl = cap - 1;
                            (void)copy_to_user((void *)(uintptr_t)lpFile, res, wl);
                            char z = 0; (void)copy_to_user((void *)(uintptr_t)(lpFile + wl), &z, 1);
                        }
                        if (g_win32_log_input) kprintf("[winpe] %s -> '%s'\n", save ? "GetSaveFileName" : "GetOpenFileName", res);
                        result = 1; break;
                    }
                } else if (ev.x >= cax && ev.x < cax + caw && ev.y >= cay && ev.y < cay + cah) {
                    result = 0; break;
                }
            } else if (ev.type == GUI_EV_KEY) {            /* edit the name field */
                int n = 0; while (field[n]) n++;
                uint8_t k = ev.key;
                if (k == '\b' || k == 0x7F) { if (n > 0) field[n - 1] = 0; redraw = true; }
                else if (k == '\n' || k == '\r') {          /* Enter == Open */
                    struct gui_event ok; memset(&ok, 0, sizeof(ok));
                    ok.type = GUI_EV_MOUSE_DOWN; ok.x = okx + 2; ok.y = oky + 2;
                    /* fallthrough by faking an Open click next loop is messy; do inline */
                    if (field[0]) {
                        char res[200]; int e = 0; const char *cp = "C:\\";
                        for (int kk = 0; cp[kk]; kk++) res[e++] = cp[kk];
                        for (const char *p = curdir; *p && e < 190; p++) res[e++] = (*p == '/') ? '\\' : *p;
                        if (curdir[0] && e < 190) res[e++] = '\\';
                        for (const char *p = field; *p && e < 198; p++) res[e++] = *p;
                        res[e] = 0;
                        if (lpFile) { int cap = (int)nMax; if (cap <= 0 || cap > (int)sizeof(res)) cap = (int)sizeof(res);
                            int wl = e; if (wl > cap - 1) wl = cap - 1;
                            (void)copy_to_user((void *)(uintptr_t)lpFile, res, wl);
                            char z = 0; (void)copy_to_user((void *)(uintptr_t)(lpFile + wl), &z, 1); }
                        result = 1; break;
                    }
                    (void)ok;
                } else if (k >= 0x20 && k < 0x7F && n < (int)sizeof(field) - 1) { field[n] = (char)k; field[n + 1] = 0; redraw = true; }
            }
        }
        sched_yield();
    }
    (void)sys_close(fd);
    return result;
}
static long w32_GetOpenFileNameA(uint64_t *a) { return win32_file_dialog(a[0], false); }
static long w32_GetSaveFileNameA(uint64_t *a) { return win32_file_dialog(a[0], true); }

/* ================= C16c: command line + environment =================
 * GetCommandLineA returns the CRT-page command line (filled by the loader);
 * GetCommandLineW a UTF-16 copy. A small per-app environment block (seeded with
 * the usual Windows vars) backs GetEnvironmentVariableA/SetEnvironmentVariableA/
 * ExpandEnvironmentStringsA. */
#define WIN32_ENV_MAX  16
#define ENV_NAME_LEN   32
#define ENV_VAL_LEN    80
static struct { bool in_use; char name[ENV_NAME_LEN], val[ENV_VAL_LEN]; } g_win32_env[WIN32_ENV_MAX];
static bool g_win32_env_seeded;

static int env_find(const char *name) {
    for (int i = 0; i < WIN32_ENV_MAX; i++)
        if (g_win32_env[i].in_use && win32_streq_ci(g_win32_env[i].name, name)) return i;
    return -1;
}
static void env_set_k(const char *name, const char *val) {  /* kernel strings */
    int i = env_find(name);
    if (i < 0) { for (i = 0; i < WIN32_ENV_MAX; i++) if (!g_win32_env[i].in_use) break; if (i >= WIN32_ENV_MAX) return; }
    g_win32_env[i].in_use = true;
    int n = 0; while (name[n] && n < ENV_NAME_LEN - 1) { g_win32_env[i].name[n] = name[n]; n++; } g_win32_env[i].name[n] = 0;
    n = 0; while (val[n] && n < ENV_VAL_LEN - 1) { g_win32_env[i].val[n] = val[n]; n++; } g_win32_env[i].val[n] = 0;
}
static void env_seed(void) {
    if (g_win32_env_seeded) return;
    g_win32_env_seeded = true;
    memset(g_win32_env, 0, sizeof(g_win32_env));
    env_set_k("OS", "tobyOS");
    env_set_k("PATH", "C:\\bin");
    env_set_k("TEMP", "C:\\");
    env_set_k("TMP", "C:\\");
    env_set_k("COMPUTERNAME", "TOBYOS");
    env_set_k("USERNAME", "root");
    env_set_k("USERPROFILE", "C:\\");
    env_set_k("SystemRoot", "C:\\Windows");
    env_set_k("windir", "C:\\Windows");
    env_set_k("NUMBER_OF_PROCESSORS", "4");
}
/* C16: reset the per-app environment (next env call reseeds defaults). */
static void win32_reset_env(void) { g_win32_env_seeded = false; memset(g_win32_env, 0, sizeof(g_win32_env)); }

/* kernel32!GetCommandLineA() -> the CRT-page command-line string. */
static long w32_GetCommandLineA(uint64_t *a) {
    (void)a;
    return (long)(WIN32_CRT_DATA_BASE + WIN32_CRT_CMDLINE);
}
/* kernel32!GetCommandLineW() -> a UTF-16 copy in the CRT page. */
static long w32_GetCommandLineW(uint64_t *a) {
    (void)a;
    uint64_t dst = WIN32_CRT_DATA_BASE + WIN32_CRT_CMDLINEW;
    char s[200]; s[0] = 0;
    (void)strncpy_from_user(s, (const char *)(uintptr_t)(WIN32_CRT_DATA_BASE + WIN32_CRT_CMDLINE), sizeof(s));
    uint16_t w[200]; int i = 0; for (; s[i] && i < 199; i++) w[i] = (uint16_t)(uint8_t)s[i]; w[i] = 0;
    (void)copy_to_user((void *)(uintptr_t)dst, w, (size_t)(i + 1) * 2);
    return (long)dst;
}
/* kernel32!GetEnvironmentVariableA(name, buf, size) -> chars copied (excl NUL),
 * or 0 if not found. */
static long w32_GetEnvironmentVariableA(uint64_t *a) {
    env_seed();
    char name[ENV_NAME_LEN]; name[0] = 0;
    if (a[0]) (void)strncpy_from_user(name, (const char *)(uintptr_t)a[0], sizeof(name));
    int i = env_find(name);
    if (i < 0) return 0;
    const char *v = g_win32_env[i].val;
    int len = 0; while (v[len]) len++;
    int cap = (int)(uint32_t)a[2];
    if (a[1] && cap > 0) {
        int n = (len < cap - 1) ? len : cap - 1;
        (void)copy_to_user((void *)(uintptr_t)a[1], v, n);
        char z = 0; (void)copy_to_user((void *)(uintptr_t)(a[1] + n), &z, 1);
    }
    return (long)len;
}
/* kernel32!SetEnvironmentVariableA(name, val) -> TRUE. val==NULL deletes. */
static long w32_SetEnvironmentVariableA(uint64_t *a) {
    env_seed();
    char name[ENV_NAME_LEN]; name[0] = 0;
    if (a[0]) (void)strncpy_from_user(name, (const char *)(uintptr_t)a[0], sizeof(name));
    if (!name[0]) return 0;
    if (!a[1]) { int i = env_find(name); if (i >= 0) g_win32_env[i].in_use = false; return 1; }
    char val[ENV_VAL_LEN]; val[0] = 0;
    (void)strncpy_from_user(val, (const char *)(uintptr_t)a[1], sizeof(val));
    env_set_k(name, val);
    return 1;
}
/* kernel32!ExpandEnvironmentStringsA(src, dst, size) -> bytes written (incl NUL).
 * Expands %VAR% references against the environment block. */
static long w32_ExpandEnvironmentStringsA(uint64_t *a) {
    env_seed();
    char src[200]; src[0] = 0;
    if (a[0]) (void)strncpy_from_user(src, (const char *)(uintptr_t)a[0], sizeof(src));
    char out[256]; int o = 0;
    for (int i = 0; src[i] && o < (int)sizeof(out) - 1; ) {
        if (src[i] == '%') {
            int j = i + 1; char vn[ENV_NAME_LEN]; int k = 0;
            while (src[j] && src[j] != '%' && k < ENV_NAME_LEN - 1) vn[k++] = src[j++];
            vn[k] = 0;
            if (src[j] == '%') {                 /* %VAR% -> value */
                int e = env_find(vn);
                if (e >= 0) { const char *v = g_win32_env[e].val; for (int m = 0; v[m] && o < (int)sizeof(out) - 1; m++) out[o++] = v[m]; }
                i = j + 1;
            } else { out[o++] = src[i++]; }      /* lone % */
        } else out[o++] = src[i++];
    }
    out[o] = 0;
    int cap = (int)(uint32_t)a[2];
    if (a[1] && cap > 0) {
        int n = (o < cap - 1) ? o : cap - 1;
        (void)copy_to_user((void *)(uintptr_t)a[1], out, n);
        char z = 0; (void)copy_to_user((void *)(uintptr_t)(a[1] + n), &z, 1);
    }
    return (long)(o + 1);
}

/* ================= C17a: Winsock TCP client (ws2_32) =================
 * A Win32 SOCKET maps onto the kernel BSD-like socket pool (ksock_* in
 * socket.c, the same stack the native HTTP/curl path rides). SOCKET handles
 * carry a distinct tag byte so closesocket/send/recv tell them apart from
 * file HANDLEs (small fds) and FILE* tokens (0xF11E....). All user buffers go
 * through copy_from/to_user so the shims stay SMAP-safe. gethostbyname routes
 * through the kernel DNS resolver and builds a real `struct hostent` in the
 * app's heap. */
#define WIN32_SOCKET_TAG  0x6800000000000000ULL  /* distinct: hkey 0x67 / menu 0x66 */
#define WIN32_TAGBYTE_MASK 0xFF00000000000000ULL
#define WIN32_INVALID_SOCK ((long)(int64_t)-1)   /* INVALID_SOCKET == SOCKET_ERROR */

/* Winsock error codes (WinError.h subset) used by WSAGetLastError. */
#define WSAEINTR_         10004
#define WSAEFAULT_        10014
#define WSAENOTSOCK_      10038
#define WSAEAFNOSUPPORT_  10047
#define WSAECONNRESET_    10054
#define WSAECONNREFUSED_  10061
#define WSAETIMEDOUT_     10060
#define WSAHOST_NOT_FOUND_ 11001

static int g_wsa_lasterror = 0;
/* C18a: the Win32 thread-last-error (GetLastError/SetLastError). Set by file
 * shims so SQLite's winAccess can tell ERROR_FILE_NOT_FOUND apart from a real
 * I/O error. Defined here (early) so the wide-file shims below can set it. */
static uint32_t g_win32_lasterr = 0;
#define WIN32_ERR_FILE_NOT_FOUND 2u
#define WIN32_ERR_ALREADY_EXISTS 183u

/* Decode a tagged SOCKET handle into a kernel ksock fd, or -1 if it isn't one. */
static int win32_sock_fd(uint64_t h) {
    if ((h & WIN32_TAGBYTE_MASK) != WIN32_SOCKET_TAG) return -1;
    return (int)(h & 0xFFFF);
}

/* ws2_32!WSAStartup(wVersionRequested, lpWSAData) -> 0. Fills WSADATA (zeroed,
 * version words set); the CRT only checks the return value + version. */
static long w32_WSAStartup(uint64_t *a) {
    uint16_t ver = (uint16_t)a[0];
    if (a[1]) {
        /* sizeof(WSADATA) on Win64 == 408 bytes. */
        uint8_t wd[408];
        memset(wd, 0, sizeof(wd));
        uint16_t v = ver ? ver : 0x0202;
        wd[0] = (uint8_t)(v & 0xFF); wd[1] = (uint8_t)(v >> 8);     /* wVersion */
        wd[2] = 0x02; wd[3] = 0x02;                                  /* wHighVersion 2.2 */
        (void)copy_to_user((void *)(uintptr_t)a[1], wd, sizeof(wd));
    }
    g_wsa_lasterror = 0;
    return 0;
}
static long w32_WSACleanup(uint64_t *a) { (void)a; return 0; }
static long w32_WSAGetLastError(uint64_t *a) { (void)a; return (long)g_wsa_lasterror; }

/* ws2_32!socket(af, type, protocol) -> a tagged SOCKET, or INVALID_SOCKET. */
static long w32_socket(uint64_t *a) {
    int af = (int)(uint32_t)a[0], type = (int)(uint32_t)a[1];
    if (af != AF_INET) { g_wsa_lasterror = WSAEAFNOSUPPORT_; return WIN32_INVALID_SOCK; }
    int fd = ksock_socket(AF_INET, type, 0);  /* SOCK_STREAM/DGRAM match Win32 1:1 */
    if (fd < 0) { g_wsa_lasterror = WSAEINTR_; return WIN32_INVALID_SOCK; }
    return (long)(WIN32_SOCKET_TAG | (uint64_t)(uint32_t)fd);
}

/* ws2_32!connect(s, name, namelen) -> 0 / SOCKET_ERROR. */
static long w32_connect(uint64_t *a) {
    int fd = win32_sock_fd(a[0]);
    if (fd < 0) { g_wsa_lasterror = WSAENOTSOCK_; return WIN32_INVALID_SOCK; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    if (!a[1] || copy_from_user(&sa, (const void *)(uintptr_t)a[1], sizeof(sa)) != 0) {
        g_wsa_lasterror = WSAEFAULT_; return WIN32_INVALID_SOCK;
    }
    if (ksock_connect(fd, &sa) != 0) { g_wsa_lasterror = WSAECONNREFUSED_; return WIN32_INVALID_SOCK; }
    return 0;
}

/* ws2_32!send(s, buf, len, flags) -> bytes sent / SOCKET_ERROR. */
static long w32_send(uint64_t *a) {
    int fd = win32_sock_fd(a[0]);
    if (fd < 0) { g_wsa_lasterror = WSAENOTSOCK_; return WIN32_INVALID_SOCK; }
    int len = (int)(uint32_t)a[2];
    if (len <= 0) return 0;
    int cap = len > 16384 ? 16384 : len;
    uint8_t *kb = (uint8_t *)kmalloc(cap);
    if (!kb) { g_wsa_lasterror = WSAEINTR_; return WIN32_INVALID_SOCK; }
    if (copy_from_user(kb, (const void *)(uintptr_t)a[1], cap) != 0) {
        kfree(kb); g_wsa_lasterror = WSAEFAULT_; return WIN32_INVALID_SOCK;
    }
    long n = ksock_send(fd, kb, cap, 0);
    kfree(kb);
    if (n <= 0) { g_wsa_lasterror = WSAECONNRESET_; return WIN32_INVALID_SOCK; }
    return n;
}

/* ws2_32!recv(s, buf, len, flags) -> bytes / 0 (closed) / SOCKET_ERROR. */
static long w32_recv(uint64_t *a) {
    int fd = win32_sock_fd(a[0]);
    if (fd < 0) { g_wsa_lasterror = WSAENOTSOCK_; return WIN32_INVALID_SOCK; }
    int len = (int)(uint32_t)a[2];
    if (len <= 0) return 0;
    int cap = len > 16384 ? 16384 : len;
    uint8_t *kb = (uint8_t *)kmalloc(cap);
    if (!kb) { g_wsa_lasterror = WSAEINTR_; return WIN32_INVALID_SOCK; }
    long n = ksock_recv(fd, kb, cap, 0);   /* tcp_recv: >0 data, -1 FIN, -2 RST, 0 timeout */
    if (n > 0) {
        if (copy_to_user((void *)(uintptr_t)a[1], kb, (size_t)n) != 0) {
            kfree(kb); g_wsa_lasterror = WSAEFAULT_; return WIN32_INVALID_SOCK;
        }
        kfree(kb);
        return n;
    }
    kfree(kb);
    if (n == -1 || n == 0) return 0;       /* FIN / timeout -> graceful close */
    g_wsa_lasterror = WSAECONNRESET_;      /* RST */
    return WIN32_INVALID_SOCK;
}

/* ws2_32!closesocket(s) -> 0 / SOCKET_ERROR. */
static long w32_closesocket(uint64_t *a) {
    int fd = win32_sock_fd(a[0]);
    if (fd < 0) { g_wsa_lasterror = WSAENOTSOCK_; return WIN32_INVALID_SOCK; }
    ksock_close(fd);
    return 0;
}

/* ws2_32!gethostbyname(name) -> a `struct hostent *` in the app heap, or NULL.
 * Resolves via the kernel DNS resolver (the same one DHCP configured). */
static long w32_gethostbyname(uint64_t *a) {
    char name[128]; name[0] = 0;
    if (a[0]) (void)strncpy_from_user(name, (const char *)(uintptr_t)a[0], sizeof(name));
    if (!name[0]) { g_wsa_lasterror = WSAHOST_NOT_FOUND_; return 0; }
    if (!net_is_up()) { g_wsa_lasterror = WSAHOST_NOT_FOUND_; return 0; }
    struct dns_result r;
    if (!dns_resolve(name, 4000, &r)) { g_wsa_lasterror = WSAHOST_NOT_FOUND_; return 0; }

    int nlen = 0; while (name[nlen]) nlen++;
    /* Layout (Win64 struct hostent):
     *  +0x00 h_name      -> base+0x40
     *  +0x08 h_aliases   -> base+0x20 (one NULL entry)
     *  +0x10 h_addrtype  = AF_INET (short)
     *  +0x12 h_length    = 4         (short)
     *  +0x18 h_addr_list -> base+0x28 { base+0x38, NULL }
     *  +0x38 the 4-byte IPv4 address (network order)
     *  +0x40 the name string bytes  */
    uint64_t total = 0x41 + (uint64_t)nlen;
    uint64_t base = win32_heap_alloc(total);
    if (!base) { g_wsa_lasterror = WSAEINTR_; return 0; }

    uint8_t buf[0x41 + 128] __attribute__((aligned(8)));
    int blen = (int)total; if (blen > (int)sizeof(buf)) blen = (int)sizeof(buf);
    memset(buf, 0, blen);
    *(uint64_t *)(buf + 0x00) = base + 0x40;          /* h_name      */
    *(uint64_t *)(buf + 0x08) = base + 0x20;          /* h_aliases   */
    *(uint16_t *)(buf + 0x10) = (uint16_t)AF_INET;    /* h_addrtype  */
    *(uint16_t *)(buf + 0x12) = 4;                    /* h_length    */
    *(uint64_t *)(buf + 0x18) = base + 0x28;          /* h_addr_list */
    /* aliases[0] = NULL already zero (buf+0x20) */
    *(uint64_t *)(buf + 0x28) = base + 0x38;          /* h_addr_list[0] -> ip */
    /* h_addr_list[1] = NULL already zero (buf+0x30) */
    *(uint32_t *)(buf + 0x38) = r.ip_be;              /* the IPv4 address */
    for (int i = 0; i < nlen && (0x40 + i) < blen; i++) buf[0x40 + i] = (uint8_t)name[i];
    (void)copy_to_user((void *)(uintptr_t)base, buf, blen);
    g_wsa_lasterror = 0;
    return (long)base;
}

/* Byte-order + address helpers (pure computation). */
static long w32_htons(uint64_t *a) { return (long)htons((uint16_t)a[0]); }
static long w32_ntohs(uint64_t *a) { return (long)ntohs((uint16_t)a[0]); }
static long w32_htonl(uint64_t *a) { return (long)(uint32_t)htonl((uint32_t)a[0]); }
static long w32_ntohl(uint64_t *a) { return (long)(uint32_t)ntohl((uint32_t)a[0]); }
/* ws2_32!inet_addr("a.b.c.d") -> the address in network byte order, INADDR_NONE on error. */
static long w32_inet_addr(uint64_t *a) {
    char s[32]; s[0] = 0;
    if (a[0]) (void)strncpy_from_user(s, (const char *)(uintptr_t)a[0], sizeof(s));
    uint32_t parts[4]; int pi = 0, have = 0; uint32_t cur = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') { cur = cur * 10 + (uint32_t)(*p - '0'); have = 1; if (cur > 255) return (long)(uint32_t)0xFFFFFFFF; }
        else if (*p == '.' || *p == 0) {
            if (!have || pi >= 4) return (long)(uint32_t)0xFFFFFFFF;
            parts[pi++] = cur; cur = 0; have = 0;
            if (*p == 0) break;
        } else return (long)(uint32_t)0xFFFFFFFF;
    }
    if (pi != 4) return (long)(uint32_t)0xFFFFFFFF;
    uint32_t ip_be = (parts[0]) | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return (long)(uint32_t)ip_be;
}

/* ---- C17b: Winsock TCP server (bind / listen / accept) ----
 * The kernel already implements the full server-side TCP handshake (tcp_listen
 * + a per-listener accept queue + tcp_accept; see tcp.c). These wrap ksock_*. */
#define WSAEADDRINUSE_  10048

/* ws2_32!bind(s, name, namelen) -> 0 / SOCKET_ERROR. */
static long w32_bind(uint64_t *a) {
    int fd = win32_sock_fd(a[0]);
    if (fd < 0) { g_wsa_lasterror = WSAENOTSOCK_; return WIN32_INVALID_SOCK; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    if (!a[1] || copy_from_user(&sa, (const void *)(uintptr_t)a[1], sizeof(sa)) != 0) {
        g_wsa_lasterror = WSAEFAULT_; return WIN32_INVALID_SOCK;
    }
    if (ksock_bind(fd, &sa) != 0) { g_wsa_lasterror = WSAEADDRINUSE_; return WIN32_INVALID_SOCK; }
    return 0;
}

/* ws2_32!listen(s, backlog) -> 0 / SOCKET_ERROR. */
static long w32_listen(uint64_t *a) {
    int fd = win32_sock_fd(a[0]);
    if (fd < 0) { g_wsa_lasterror = WSAENOTSOCK_; return WIN32_INVALID_SOCK; }
    if (ksock_listen(fd, (int)(int32_t)(uint32_t)a[1]) != 0) {
        g_wsa_lasterror = WSAEINTR_; return WIN32_INVALID_SOCK;
    }
    return 0;
}

/* ws2_32!accept(s, addr, addrlen) -> a tagged SOCKET for the new connection,
 * or INVALID_SOCKET. addr/addrlen are optional (filled with the peer family). */
static long w32_accept(uint64_t *a) {
    int fd = win32_sock_fd(a[0]);
    if (fd < 0) { g_wsa_lasterror = WSAENOTSOCK_; return WIN32_INVALID_SOCK; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    int nfd = ksock_accept(fd, &sa);          /* blocks (tcp_accept) up to recv timeout */
    if (nfd < 0) { g_wsa_lasterror = WSAETIMEDOUT_; return WIN32_INVALID_SOCK; }
    if (a[1]) {
        (void)copy_to_user((void *)(uintptr_t)a[1], &sa, sizeof(sa));
        if (a[2]) { int alen = (int)sizeof(sa); (void)copy_to_user((void *)(uintptr_t)a[2], &alen, sizeof(alen)); }
    }
    return (long)(WIN32_SOCKET_TAG | (uint64_t)(uint32_t)nfd);
}

/* ---- C17c: Winsock UDP datagrams (sendto / recvfrom) ----
 * Map onto sock_sendto/sock_recvfrom (UDP) via ksock_sendto/ksock_recvfrom. */

/* ws2_32!sendto(s, buf, len, flags, to, tolen) -> bytes sent / SOCKET_ERROR. */
static long w32_sendto(uint64_t *a) {
    int fd = win32_sock_fd(a[0]);
    if (fd < 0) { g_wsa_lasterror = WSAENOTSOCK_; return WIN32_INVALID_SOCK; }
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    if (!a[4] || copy_from_user(&to, (const void *)(uintptr_t)a[4], sizeof(to)) != 0) {
        g_wsa_lasterror = WSAEFAULT_; return WIN32_INVALID_SOCK;
    }
    int len = (int)(uint32_t)a[2];
    if (len <= 0) return 0;
    int cap = len > 16384 ? 16384 : len;
    uint8_t *kb = (uint8_t *)kmalloc(cap);
    if (!kb) { g_wsa_lasterror = WSAEINTR_; return WIN32_INVALID_SOCK; }
    if (copy_from_user(kb, (const void *)(uintptr_t)a[1], cap) != 0) {
        kfree(kb); g_wsa_lasterror = WSAEFAULT_; return WIN32_INVALID_SOCK;
    }
    long n = ksock_sendto(fd, kb, cap, to.sin_addr, to.sin_port);
    kfree(kb);
    if (n < 0) { g_wsa_lasterror = WSAECONNRESET_; return WIN32_INVALID_SOCK; }
    return n;
}

/* ws2_32!recvfrom(s, buf, len, flags, from, fromlen) -> bytes / SOCKET_ERROR.
 * Blocks (sock_recvfrom) until a datagram arrives. Fills the optional `from`. */
static long w32_recvfrom(uint64_t *a) {
    int fd = win32_sock_fd(a[0]);
    if (fd < 0) { g_wsa_lasterror = WSAENOTSOCK_; return WIN32_INVALID_SOCK; }
    int len = (int)(uint32_t)a[2];
    if (len <= 0) return 0;
    int cap = len > 16384 ? 16384 : len;
    uint8_t *kb = (uint8_t *)kmalloc(cap);
    if (!kb) { g_wsa_lasterror = WSAEINTR_; return WIN32_INVALID_SOCK; }
    uint32_t sip = 0; uint16_t sport = 0;
    long n = ksock_recvfrom(fd, kb, cap, &sip, &sport);
    if (n < 0) { kfree(kb); g_wsa_lasterror = WSAECONNRESET_; return WIN32_INVALID_SOCK; }
    if (n > 0 && copy_to_user((void *)(uintptr_t)a[1], kb, (size_t)n) != 0) {
        kfree(kb); g_wsa_lasterror = WSAEFAULT_; return WIN32_INVALID_SOCK;
    }
    kfree(kb);
    if (a[4]) {                                   /* from sockaddr (optional) */
        struct sockaddr_in from;
        memset(&from, 0, sizeof(from));
        from.sin_family = (uint16_t)AF_INET;
        from.sin_addr = sip;
        from.sin_port = sport;
        (void)copy_to_user((void *)(uintptr_t)a[4], &from, sizeof(from));
        if (a[5]) { int fl = (int)sizeof(from); (void)copy_to_user((void *)(uintptr_t)a[5], &fl, sizeof(fl)); }
    }
    return n;
}

/* ================= C18a: run real off-the-shelf SQLite =================
 * The SQLite shell + amalgamation pull a much wider kernel32 surface than any
 * first-party test: the Win32 heap, the WIDE (W) file APIs (SQLite's WinNT VFS),
 * time, console, file locking, and memory-mapped files. Most map onto existing
 * VFS/heap primitives or are safe stubs (mmap -> fail so SQLite uses normal I/O;
 * locking -> success since we are single-process). */

/* ---- Win32 process heap (over the size-tracked malloc) ---- */
#define WIN32_PROCHEAP_TOKEN  0x48454150ull     /* "HEAP"; an opaque non-NULL HANDLE */
static long w32_GetProcessHeap(uint64_t *a) { (void)a; return (long)WIN32_PROCHEAP_TOKEN; }
static long w32_HeapCreate (uint64_t *a) { (void)a; return (long)WIN32_PROCHEAP_TOKEN; }
static long w32_HeapDestroy(uint64_t *a) { (void)a; return 1; }
static long w32_HeapValidate(uint64_t *a){ (void)a; return 1; }
static long w32_HeapCompact(uint64_t *a) { (void)a; return 0; }
/* HeapAlloc(hHeap, dwFlags, n) -> ptr (HEAP_ZERO_MEMORY 0x8 already holds: arena
 * is fresh-zeroed). */
static long w32_HeapAlloc(uint64_t *a)   { return (long)win32_malloc_hdr(a[2]); }
static long w32_HeapFree (uint64_t *a)   { (void)a; return 1; }
static long w32_HeapSize (uint64_t *a)   { return (long)win32_msize(a[2]); }
/* HeapReAlloc(hHeap, dwFlags, ptr, n). */
static long w32_HeapReAlloc(uint64_t *a) { uint64_t b[2] = { a[2], a[3] }; return w32_realloc(b); }

/* ---- wide-char helpers + conversion APIs ---- */
/* Read a user UTF-16 string into a kernel ASCII buffer (ASCII subset; non-ASCII
 * code units become '?'). Returns the ASCII length. */
static int win32_wstr_to_k(uint64_t uwide, char *out, int cap) {
    int i = 0;
    for (; i < cap - 1; i++) {
        uint16_t wc = 0;
        if (copy_from_user(&wc, (const void *)(uintptr_t)(uwide + (uint64_t)i * 2), 2) != 0) break;
        if (wc == 0) break;
        out[i] = (wc < 0x80) ? (char)wc : '?';
    }
    out[i] = 0;
    return i;
}
/* Write a kernel ASCII string out to a user UTF-16 buffer (incl. NUL). cap is in
 * WORDs. Returns code units written incl. NUL. */
static int win32_k_to_wstr(uint64_t uwide, const char *s, int cap) {
    int i = 0;
    for (; s[i] && i < cap - 1; i++) {
        uint16_t wc = (uint16_t)(uint8_t)s[i];
        (void)copy_to_user((void *)(uintptr_t)(uwide + (uint64_t)i * 2), &wc, 2);
    }
    uint16_t z = 0;
    (void)copy_to_user((void *)(uintptr_t)(uwide + (uint64_t)i * 2), &z, 2);
    return i + 1;
}
/* MultiByteToWideChar(cp, flags, mbStr, cbMb, wStr, cchWide). cbMb<0 => NUL-term
 * (count includes the NUL). cchWide==0 => return required count. */
static long w32_MultiByteToWideChar(uint64_t *a) {
    int cb = (int)(int32_t)a[3];
    char src[1024];
    int n;
    if (cb < 0) {
        n = (int)strncpy_from_user(src, (const char *)(uintptr_t)a[2], sizeof(src));
        if (n < 0) return 0;
        n = (int)strlen(src) + 1;                /* include NUL */
    } else {
        n = cb < (int)sizeof(src) ? cb : (int)sizeof(src);
        if (n && copy_from_user(src, (const void *)(uintptr_t)a[2], (size_t)n) != 0) return 0;
    }
    int cch = (int)(int32_t)a[5];
    if (cch == 0) return n;                       /* size query */
    int w = 0;
    for (; w < n && w < cch; w++) {
        uint16_t wc = (uint16_t)(uint8_t)src[w];
        (void)copy_to_user((void *)(uintptr_t)(a[4] + (uint64_t)w * 2), &wc, 2);
    }
    return w;
}
/* WideCharToMultiByte(cp, flags, wStr, cchWide, mbStr, cbMb, defChar, usedDef). */
static long w32_WideCharToMultiByte(uint64_t *a) {
    int cch = (int)(int32_t)a[3];
    char dst[1024];
    int n;
    if (cch < 0) { n = win32_wstr_to_k(a[2], dst, sizeof(dst)); n += 1; }   /* + NUL */
    else {
        n = 0;
        for (; n < cch && n < (int)sizeof(dst) - 1; n++) {
            uint16_t wc = 0;
            if (copy_from_user(&wc, (const void *)(uintptr_t)(a[2] + (uint64_t)n * 2), 2) != 0) break;
            dst[n] = (wc < 0x80) ? (char)wc : '?';
        }
        dst[n] = 0;
    }
    int cb = (int)(int32_t)a[5];
    if (cb == 0) return n;                         /* size query */
    int w = n < cb ? n : cb;
    if (w && a[4]) (void)copy_to_user((void *)(uintptr_t)a[4], dst, (size_t)w);
    return w;
}

/* ---- wide file APIs (SQLite's WinNT VFS) ---- */
/* Translate a KERNEL Windows path string ("X:\..") into a tobyOS path (drive ->
 * /data, '\\' -> '/'). Mirrors w32_CreateFileA's inline logic for the W side. */
static int win32_xlate_winpath(const char *wpath, char *kp, int cap) {
    int si = 0, di = 0;
    if (wpath[0] && wpath[1] == ':') {
        for (const char *r = "/data"; *r && di < cap - 1; r++) kp[di++] = *r;
        si = 2;
        if (wpath[si] != '\\' && wpath[si] != '/' && di < cap - 1) kp[di++] = '/';
    }
    for (; wpath[si] && di < cap - 1; si++) kp[di++] = (wpath[si] == '\\') ? '/' : wpath[si];
    kp[di] = '\0';
    return di;
}
/* Open a KERNEL Windows path with the CreateFile access/disposition encoding. */
static long win32_open_winpath(const char *wpath, uint32_t access, uint32_t disp) {
    char kp[ABI_PATH_MAX];
    int di = win32_xlate_winpath(wpath, kp, sizeof(kp));
    if (di < 0) return (long)WIN32_INVALID_HANDLE;
    uint64_t ubuf = WIN32_CRT_DATA_BASE + WIN32_CRT_PATHBUF;
    if (copy_to_user((void *)(uintptr_t)ubuf, kp, (size_t)di + 1) != 0) return (long)WIN32_INVALID_HANDLE;
    bool rd = (access & 0x80000000UL) != 0, wr = (access & 0x40000000UL) != 0;
    int flags = (rd && wr) ? ABI_O_RDWR : (wr ? ABI_O_WRONLY : ABI_O_RDONLY);
    switch (disp) {
        case 1: flags |= ABI_O_CREAT | ABI_O_EXCL;  break;
        case 2: flags |= ABI_O_CREAT | ABI_O_TRUNC; break;
        case 3:                                      break;
        case 4: flags |= ABI_O_CREAT;                break;
        case 5: flags |= ABI_O_TRUNC;                break;
        default: break;
    }
    long fd = sys_open((const char *)(uintptr_t)ubuf, flags, 0644);
    return (fd < 0) ? (long)WIN32_INVALID_HANDLE : fd;
}
/* CreateFileW(name, access, share, sa, disp, flags, template). */
static long w32_CreateFileW(uint64_t *a) {
    char wp[ABI_PATH_MAX];
    win32_wstr_to_k(a[0], wp, sizeof(wp));
    return win32_open_winpath(wp, (uint32_t)a[1], (uint32_t)a[4]);
}
static long w32_GetFileAttributesW(uint64_t *a) {
    char wp[ABI_PATH_MAX]; win32_wstr_to_k(a[0], wp, sizeof(wp));
    char kp[ABI_PATH_MAX]; win32_xlate_winpath(wp, kp, sizeof(kp));
    struct vfs_stat vs;
    if (vfs_stat(kp, &vs) != VFS_OK) { g_win32_lasterr = WIN32_ERR_FILE_NOT_FOUND; return (long)0xFFFFFFFFu; }
    g_win32_lasterr = 0;
    return (vs.type == VFS_TYPE_DIR) ? 0x10 : 0x80;
}
/* GetFileAttributesExW(name, infoLevel, lpFileInformation) ->
 * WIN32_FILE_ATTRIBUTE_DATA { DWORD attr; FILETIME c/a/w; DWORD szHi/szLo }. */
static long w32_GetFileAttributesExW(uint64_t *a) {
    char wp[ABI_PATH_MAX]; win32_wstr_to_k(a[0], wp, sizeof(wp));
    char kp[ABI_PATH_MAX]; win32_xlate_winpath(wp, kp, sizeof(kp));
    struct vfs_stat vs;
    if (vfs_stat(kp, &vs) != VFS_OK) { g_win32_lasterr = WIN32_ERR_FILE_NOT_FOUND; return 0; }
    uint8_t b[36]; memset(b, 0, sizeof(b));
    uint32_t attr = (vs.type == VFS_TYPE_DIR) ? 0x10u : 0x80u;
    memcpy(&b[0], &attr, 4);
    uint32_t szlo = (uint32_t)vs.size, szhi = (uint32_t)(vs.size >> 32);
    memcpy(&b[28], &szhi, 4); memcpy(&b[32], &szlo, 4);
    (void)copy_to_user((void *)(uintptr_t)a[2], b, sizeof(b));
    g_win32_lasterr = 0;
    return 1;
}
static long w32_DeleteFileW(uint64_t *a) {
    char wp[ABI_PATH_MAX]; win32_wstr_to_k(a[0], wp, sizeof(wp));
    char kp[ABI_PATH_MAX]; int di = win32_xlate_winpath(wp, kp, sizeof(kp));
    if (di < 0) return 0;
    uint64_t ubuf = WIN32_CRT_DATA_BASE + WIN32_CRT_PATHBUF;
    if (copy_to_user((void *)(uintptr_t)ubuf, kp, (size_t)di + 1) != 0) return 0;
    return (sys_unlink((const char *)(uintptr_t)ubuf) == 0) ? 1 : 0;
}
/* GetFullPathNameW(name, nBuf, buf, &filePart): we treat the input as already
 * absolute and copy it through, pointing filePart at the basename. */
static long w32_GetFullPathNameW(uint64_t *a) {
    char wp[ABI_PATH_MAX]; int n = win32_wstr_to_k(a[0], wp, sizeof(wp));
    int cap = (int)(uint32_t)a[1];
    if (a[2] && cap > 0) {
        int w = win32_k_to_wstr(a[2], wp, cap) - 1;   /* code units excl NUL */
        if (a[3]) {                                    /* lpFilePart */
            int base = 0; for (int i = 0; i < n; i++) if (wp[i] == '\\' || wp[i] == '/') base = i + 1;
            uint64_t fp = a[2] + (uint64_t)base * 2;
            (void)copy_to_user((void *)(uintptr_t)a[3], &fp, 8);
        }
        (void)w;
    }
    return n;                                          /* required/!written length in chars */
}
static long w32_GetFullPathNameA(uint64_t *a) {
    char wp[ABI_PATH_MAX]; (void)strncpy_from_user(wp, (const char *)(uintptr_t)a[0], sizeof(wp));
    int n = (int)strlen(wp);
    int cap = (int)(uint32_t)a[1];
    if (a[2] && cap > 0) {
        int w = n < cap - 1 ? n : cap - 1;
        (void)copy_to_user((void *)(uintptr_t)a[2], wp, (size_t)w);
        char z = 0; (void)copy_to_user((void *)(uintptr_t)(a[2] + w), &z, 1);
        if (a[3]) { int base = 0; for (int i = 0; i < n; i++) if (wp[i] == '\\' || wp[i] == '/') base = i + 1;
                    uint64_t fp = a[2] + (uint64_t)base; (void)copy_to_user((void *)(uintptr_t)a[3], &fp, 8); }
    }
    return n;
}
/* GetTempPathW/A -> "C:\\" (maps to /data). Returns length in chars. */
static long w32_GetTempPathW(uint64_t *a) {
    int cap = (int)(uint32_t)a[0];
    if (a[1] && cap >= 4) (void)win32_k_to_wstr(a[1], "C:\\", cap);
    return 3;
}
static long w32_GetTempPathA(uint64_t *a) {
    int cap = (int)(uint32_t)a[0];
    if (a[1] && cap >= 4) { (void)copy_to_user((void *)(uintptr_t)a[1], "C:\\", 4); }
    return 3;
}
static long w32_SetCurrentDirectoryW(uint64_t *a) { (void)a; return 1; }
static long w32_FindFirstFileW(uint64_t *a) { (void)a; g_wsa_lasterror = 2; return (long)WIN32_INVALID_HANDLE; }
/* GetFileType(h): real fds -> DISK(1); std handles -> CHAR(2). */
static long w32_GetFileType(uint64_t *a) { return ((int)a[0] < 3) ? 2 : 1; }
static long w32_FlushFileBuffers(uint64_t *a) { (void)a; return 1; }
static long w32_SetEndOfFile(uint64_t *a) { (void)a; return 1; }   /* no real truncate; grows via writes */
static long w32_GetDiskFreeSpaceA(uint64_t *a) {
    uint32_t spc = 8, bps = 512, freec = 0x100000, totc = 0x200000;
    if (a[1]) (void)copy_to_user((void *)(uintptr_t)a[1], &spc, 4);
    if (a[2]) (void)copy_to_user((void *)(uintptr_t)a[2], &bps, 4);
    if (a[3]) (void)copy_to_user((void *)(uintptr_t)a[3], &freec, 4);
    if (a[4]) (void)copy_to_user((void *)(uintptr_t)a[4], &totc, 4);
    return 1;
}

/* ---- time ---- */
#define WIN32_FT_EPOCH_DIFF 11644473600ull        /* seconds 1601-01-01 .. 1970 */
static void win32_put_filetime(uint64_t uft, uint64_t unix_secs) {
    uint64_t ft = (unix_secs + WIN32_FT_EPOCH_DIFF) * 10000000ull;
    uint32_t lo = (uint32_t)ft, hi = (uint32_t)(ft >> 32);
    (void)copy_to_user((void *)(uintptr_t)uft, &lo, 4);
    (void)copy_to_user((void *)(uintptr_t)(uft + 4), &hi, 4);
}
static long w32_GetSystemTimeAsFileTime(uint64_t *a) {
    if (a[0]) win32_put_filetime(a[0], rtc_unix_time());
    return 0;
}
static long w32_GetSystemTime(uint64_t *a) {     /* SYSTEMTIME: 8 WORDs */
    struct rtc_time t; rtc_read_time(&t);
    uint16_t st[8] = { t.year, t.month, t.weekday, t.day, t.hour, t.minute, t.second, 0 };
    if (a[0]) (void)copy_to_user((void *)(uintptr_t)a[0], st, sizeof(st));
    return 0;
}
static int64_t win32_days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;          /* days since 1970-01-01 */
}
static long w32_SystemTimeToFileTime(uint64_t *a) {
    uint16_t st[8]; memset(st, 0, sizeof(st));
    if (!a[0] || copy_from_user(st, (const void *)(uintptr_t)a[0], sizeof(st)) != 0) return 0;
    int64_t days = win32_days_from_civil(st[0], st[1], st[3]);
    uint64_t secs = (uint64_t)(days * 86400 + st[4] * 3600 + st[5] * 60 + st[6]);
    if (a[1]) win32_put_filetime(a[1], secs);
    return 1;
}
static long w32_GetTickCount(uint64_t *a) {
    (void)a; uint64_t hz = pit_hz(); if (!hz) hz = 100;
    return (long)(uint32_t)(pit_ticks() * 1000ull / hz);
}
static long w32_QueryPerformanceCounter(uint64_t *a) {
    uint64_t v = pit_ticks();
    if (a[0]) (void)copy_to_user((void *)(uintptr_t)a[0], &v, 8);
    return 1;
}
static long w32_QueryPerformanceFrequency(uint64_t *a) {
    uint64_t hz = pit_hz(); if (!hz) hz = 100;
    if (a[0]) (void)copy_to_user((void *)(uintptr_t)a[0], &hz, 8);
    return 1;
}
static long w32_GetSystemInfo(uint64_t *a) {      /* SYSTEM_INFO ~48 bytes */
    uint8_t b[48]; memset(b, 0, sizeof(b));
    uint32_t page = 4096, gran = 65536, nproc = 1;
    memcpy(&b[4], &page, 4);                       /* dwPageSize */
    memcpy(&b[32], &nproc, 4);                     /* dwNumberOfProcessors */
    memcpy(&b[40], &gran, 4);                      /* dwAllocationGranularity */
    if (a[0]) (void)copy_to_user((void *)(uintptr_t)a[0], b, sizeof(b));
    return 0;
}

/* ---- file locking (single-process: always succeed) + mmap (fail -> SQLite
 *      falls back to ordinary read/write I/O) ---- */
static long w32_LockFileStub(uint64_t *a)   { (void)a; return 1; }
static long w32_CreateFileMappingW(uint64_t *a) { (void)a; g_wsa_lasterror = 8; return 0; } /* NULL -> no mmap */
static long w32_MapViewOfFile(uint64_t *a)  { (void)a; return 0; }

/* ---- console (report "not a console" so the shell uses plain file I/O) ---- */
static long w32_GetConsoleMode(uint64_t *a) {
    if (a[1]) { uint32_t z = 0; (void)copy_to_user((void *)(uintptr_t)a[1], &z, 4); }
    return 0;                                      /* FALSE: handle is not a console */
}
static long w32_WriteConsoleW(uint64_t *a) {       /* (h, wbuf, nchars, *written, resv) */
    int nch = (int)(uint32_t)a[2];
    char buf[512];
    int w = 0;
    for (; w < nch && w < (int)sizeof(buf); w++) {
        uint16_t wc = 0;
        if (copy_from_user(&wc, (const void *)(uintptr_t)(a[1] + (uint64_t)w * 2), 2) != 0) break;
        buf[w] = (wc < 0x80) ? (char)wc : '?';
    }
    int fd = ((int)a[0] >= 0 && (int)a[0] < 3) ? (int)a[0] : 1;
    if (w > 0) (void)sys_write(fd, buf, (size_t)w);
    if (a[3]) { uint32_t cw = (uint32_t)w; (void)copy_to_user((void *)(uintptr_t)a[3], &cw, 4); }
    return 1;
}

/* ---- misc ---- */
static long w32_GetCurrentProcess(uint64_t *a)  { (void)a; return (long)(int64_t)-1; } /* pseudo handle */
static long w32_GetCurrentThreadId(uint64_t *a) { (void)a; struct proc *p = current_proc(); return p ? (long)(uint32_t)p->pid : 1; }
static long w32_LocalFree(uint64_t *a)          { (void)a; return 0; }
static long w32_AreFileApisANSI(uint64_t *a)    { (void)a; return 1; }
static long w32_CreateMutexW(uint64_t *a)       { (void)a; return (long)0x4D000001ull; } /* opaque non-NULL */
/* FormatMessageA: write a generic message; honour FORMAT_MESSAGE_ALLOCATE_BUFFER
 * (0x100) where lpBuffer is a char** to receive a heap copy. */
static long w32_FormatMessageA(uint64_t *a) {
    static const char msg[] = "tobyOS Win32 error";
    int len = (int)sizeof(msg) - 1;
    uint32_t flags = (uint32_t)a[0];
    if (flags & 0x100u) {
        uint64_t p = win32_malloc_hdr((uint64_t)len + 1);
        if (p) { (void)copy_to_user((void *)(uintptr_t)p, msg, (size_t)len + 1);
                 (void)copy_to_user((void *)(uintptr_t)a[4], &p, 8); }   /* *(char**)lpBuffer = p */
        return len;
    }
    int cap = (int)(uint32_t)a[5];
    if (a[4] && cap > 0) { int w = len < cap - 1 ? len : cap - 1;
        (void)copy_to_user((void *)(uintptr_t)a[4], msg, (size_t)w);
        char z = 0; (void)copy_to_user((void *)(uintptr_t)(a[4] + w), &z, 1); return w; }
    return len;
}

/* ================= C18a: the C-runtime breadth SQLite pulls in =================
 * Standard libc that the ucrt would normally provide: string/ctype/conversion
 * (pure compute on user memory), stdio FILE* (a FILE* token == WIN32_IOB_TAG|fd,
 * extended from the C2 printf path), and the _wstat/_access/_mkdir file layer
 * (onto the VFS). NOTE: float-returning CRT (strtod) cannot pass a double back
 * through the integer gate, so it is stubbed -- SQLite only needs it for REAL
 * literals, which the C18a proof query does not use. */
#define W32_STRCAP 4096
static int win32_file_fd(uint64_t tok) {
    if ((tok & 0xFFFFFF00ULL) == WIN32_IOB_TAG) return (int)(tok & 0xFF);
    return -1;
}

/* ---- ctype (operate on the int arg) ---- */
static long w32_isalnum(uint64_t *a){int c=(int)a[0];return (c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z');}
static long w32_isalpha(uint64_t *a){int c=(int)a[0];return (c>='A'&&c<='Z')||(c>='a'&&c<='z');}
static long w32_isdigit(uint64_t *a){int c=(int)a[0];return c>='0'&&c<='9';}
static long w32_isspace(uint64_t *a){int c=(int)a[0];return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f';}
static long w32_isprint(uint64_t *a){int c=(int)a[0];return c>=0x20&&c<0x7F;}
static long w32_tolower(uint64_t *a){int c=(int)a[0];return (c>='A'&&c<='Z')?c+32:c;}
static long w32_toupper(uint64_t *a){int c=(int)a[0];return (c>='a'&&c<='z')?c-32:c;}
/* C19b: remaining C-locale ctype predicates (real Lua's pattern matcher + libs). */
static long w32_iscntrl(uint64_t *a){int c=(int)a[0];return (c>=0&&c<0x20)||c==0x7F;}
static long w32_isgraph(uint64_t *a){int c=(int)a[0];return c>0x20&&c<0x7F;}
static long w32_islower(uint64_t *a){int c=(int)a[0];return c>='a'&&c<='z';}
static long w32_isupper(uint64_t *a){int c=(int)a[0];return c>='A'&&c<='Z';}
static long w32_ispunct(uint64_t *a){int c=(int)a[0];return c>0x20&&c<0x7F&&
    !((c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z'));}
static long w32_isxdigit(uint64_t *a){int c=(int)a[0];return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');}

/* ---- string (user memory) ---- */
static long w32_strcmp(uint64_t *a) {
    char s1[W32_STRCAP], s2[W32_STRCAP];
    if (strncpy_from_user(s1,(const char*)(uintptr_t)a[0],sizeof(s1))<0) return 0;
    if (strncpy_from_user(s2,(const char*)(uintptr_t)a[1],sizeof(s2))<0) return 0;
    for (int i=0;;i++){unsigned char c1=(unsigned char)s1[i],c2=(unsigned char)s2[i];
        if(c1!=c2) return (long)c1-(long)c2; if(!c1) return 0;}
}
static long w32_strcpy(uint64_t *a) {
    char s[W32_STRCAP];
    int n=(int)strncpy_from_user(s,(const char*)(uintptr_t)a[1],sizeof(s)); if(n<0)return (long)a[0];
    (void)copy_to_user((void*)(uintptr_t)a[0],s,(size_t)n+1); return (long)a[0];
}
static long w32_strcat(uint64_t *a) {
    char d[W32_STRCAP],s[W32_STRCAP];
    int dn=(int)strncpy_from_user(d,(const char*)(uintptr_t)a[0],sizeof(d)); if(dn<0)dn=0;
    int sn=(int)strncpy_from_user(s,(const char*)(uintptr_t)a[1],sizeof(s)); if(sn<0)sn=0;
    int room=W32_STRCAP-1-dn; if(sn>room)sn=room;
    (void)copy_to_user((void*)(uintptr_t)(a[0]+dn),s,(size_t)sn);
    char z=0; (void)copy_to_user((void*)(uintptr_t)(a[0]+dn+sn),&z,1);
    return (long)a[0];
}
static long w32_strchr(uint64_t *a) {
    char s[W32_STRCAP]; int n=(int)strncpy_from_user(s,(const char*)(uintptr_t)a[0],sizeof(s)); if(n<0)return 0;
    char c=(char)(int)a[1];
    for(int i=0;i<=n;i++) if(s[i]==c) return (long)(a[0]+(uint64_t)i);
    return 0;
}
static long w32_strrchr(uint64_t *a) {
    char s[W32_STRCAP]; int n=(int)strncpy_from_user(s,(const char*)(uintptr_t)a[0],sizeof(s)); if(n<0)return 0;
    char c=(char)(int)a[1]; long r=0;
    for(int i=0;i<=n;i++) if(s[i]==c) r=(long)(a[0]+(uint64_t)i);
    return r;
}
static long w32_strpbrk(uint64_t *a) {
    char s[W32_STRCAP],set[256]; int n=(int)strncpy_from_user(s,(const char*)(uintptr_t)a[0],sizeof(s));
    if(n<0)return 0; if(strncpy_from_user(set,(const char*)(uintptr_t)a[1],sizeof(set))<0)return 0;
    for(int i=0;s[i];i++)for(int j=0;set[j];j++)if(s[i]==set[j])return (long)(a[0]+(uint64_t)i);
    return 0;
}
static long w32_strspn(uint64_t *a) {
    char s[W32_STRCAP],set[256]; int n=(int)strncpy_from_user(s,(const char*)(uintptr_t)a[0],sizeof(s));
    if(n<0)return 0; (void)strncpy_from_user(set,(const char*)(uintptr_t)a[1],sizeof(set));
    int i=0; for(;s[i];i++){int f=0;for(int j=0;set[j];j++)if(s[i]==set[j]){f=1;break;} if(!f)break;} return i;
}
static long w32_strcspn(uint64_t *a) {
    char s[W32_STRCAP],set[256]; int n=(int)strncpy_from_user(s,(const char*)(uintptr_t)a[0],sizeof(s));
    if(n<0)return 0; (void)strncpy_from_user(set,(const char*)(uintptr_t)a[1],sizeof(set));
    int i=0; for(;s[i];i++){int f=0;for(int j=0;set[j];j++)if(s[i]==set[j]){f=1;break;} if(f)break;} return i;
}
static long w32_strdup(uint64_t *a) {
    char s[W32_STRCAP]; int n=(int)strncpy_from_user(s,(const char*)(uintptr_t)a[0],sizeof(s)); if(n<0)return 0;
    uint64_t p=win32_malloc_hdr((uint64_t)n+1); if(!p)return 0;
    (void)copy_to_user((void*)(uintptr_t)p,s,(size_t)n+1); return (long)p;
}
static long w32_memchr(uint64_t *a) {
    uint64_t base=a[0],n=a[2]; int c=(int)(unsigned char)a[1];
    for(uint64_t i=0;i<n;i++){unsigned char b; if(copy_from_user(&b,(const void*)(uintptr_t)(base+i),1)!=0)break; if(b==(unsigned char)c)return (long)(base+i);}
    return 0;
}
static long w32_memmove(uint64_t *a) {
    uint64_t d=a[0],s=a[1],n=a[2]; uint8_t buf[2048];
    /* copy via a kernel bounce -> safe for overlap; go backwards if d>s */
    if (d>s) { for(uint64_t off=n; off>0; ){uint64_t ch=off> sizeof(buf)?sizeof(buf):off; off-=ch;
        if(copy_from_user(buf,(const void*)(uintptr_t)(s+off),ch)!=0)break; (void)copy_to_user((void*)(uintptr_t)(d+off),buf,ch);} }
    else { for(uint64_t off=0; off<n; ){uint64_t ch=n-off> sizeof(buf)?sizeof(buf):n-off;
        if(copy_from_user(buf,(const void*)(uintptr_t)(s+off),ch)!=0)break; (void)copy_to_user((void*)(uintptr_t)(d+off),buf,ch); off+=ch;} }
    return (long)d;
}

/* ---- conversion ---- */
static long w32_atoi(uint64_t *a) {
    char s[64]; if(strncpy_from_user(s,(const char*)(uintptr_t)a[0],sizeof(s))<0)return 0;
    int i=0,sign=1; while(s[i]==' '||s[i]=='\t')i++; if(s[i]=='-'){sign=-1;i++;}else if(s[i]=='+')i++;
    long v=0; for(;s[i]>='0'&&s[i]<='9';i++)v=v*10+(s[i]-'0'); return sign*v;
}
static long w32_strtol(uint64_t *a) {
    char s[64]; if(strncpy_from_user(s,(const char*)(uintptr_t)a[0],sizeof(s))<0)return 0;
    int base=(int)a[2]; int i=0,sign=1; while(s[i]==' '||s[i]=='\t')i++;
    if(s[i]=='-'){sign=-1;i++;}else if(s[i]=='+')i++;
    if((base==0||base==16)&&s[i]=='0'&&(s[i+1]=='x'||s[i+1]=='X')){i+=2;base=16;}
    if(base==0)base=10;
    long v=0; for(;s[i];i++){int d; char c=s[i];
        if(c>='0'&&c<='9')d=c-'0'; else if(c>='a'&&c<='z')d=c-'a'+10; else if(c>='A'&&c<='Z')d=c-'A'+10; else break;
        if(d>=base)break; v=v*base+d;}
    if(a[1]){uint64_t ep=a[0]+(uint64_t)i; (void)copy_to_user((void*)(uintptr_t)a[1],&ep,8);}
    return sign*v;
}
/* C22: strtod is now REAL. The actual decimal->double parse runs in the -msse
 * kmath unit (this file is -mno-sse); the result's bit pattern is returned in
 * rax and, because strtod is in win32_shim_is_double_ret(), the C20 float gate
 * does `movq xmm0, rax` so the caller gets the double in xmm0. endptr (a[1]) is
 * advanced past the consumed bytes (== str on no conversion, per C). */
static long w32_strtod(uint64_t *a) {
    char s[128];
    if (strncpy_from_user(s, (const char *)(uintptr_t)a[0], sizeof(s)) < 0) {
        if (a[1]) (void)copy_to_user((void *)(uintptr_t)a[1], &a[0], 8);
        return 0;
    }
    s[sizeof(s) - 1] = '\0';
    int consumed = 0;
    uint64_t bits = kmath_strtod(s, &consumed, 0);
    if (a[1]) { uint64_t ep = a[0] + (uint64_t)consumed; (void)copy_to_user((void *)(uintptr_t)a[1], &ep, 8); }
    return (long)bits;
}
/* atof(str) == strtod(str, NULL); also a real double through the float gate. */
static long w32_atof(uint64_t *a) {
    char s[128];
    if (strncpy_from_user(s, (const char *)(uintptr_t)a[0], sizeof(s)) < 0) return 0;
    s[sizeof(s) - 1] = '\0';
    int consumed = 0;
    return (long)kmath_strtod(s, &consumed, 0);
}
/* strtoul(str, endptr, base) -> unsigned long; like w32_strtol without the sign
 * negation (a leading '-' still wraps per C, but we keep it simple/unsigned). */
static long w32_strtoul(uint64_t *a) {
    char s[64]; if (strncpy_from_user(s, (const char *)(uintptr_t)a[0], sizeof(s)) < 0) return 0;
    int base = (int)a[2]; int i = 0; int neg = 0; while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == '-') { neg = 1; i++; } else if (s[i] == '+') i++;
    if ((base == 0 || base == 16) && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) { i += 2; base = 16; }
    if (base == 0) base = 10;
    unsigned long v = 0; for (; s[i]; i++) { int d; char c = s[i];
        if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'z') d = c - 'a' + 10; else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10; else break;
        if (d >= base) break; v = v * (unsigned)base + (unsigned)d; }
    if (a[1]) { uint64_t ep = a[0] + (uint64_t)i; (void)copy_to_user((void *)(uintptr_t)a[1], &ep, 8); }
    return (long)(neg ? (unsigned long)(-(long)v) : v);
}

/* ---- stdio FILE* (token == WIN32_IOB_TAG | fd) ---- */
static long w32_fopen(uint64_t *a) {
    char path[ABI_PATH_MAX], mode[8];
    if(strncpy_from_user(path,(const char*)(uintptr_t)a[0],sizeof(path))<0)return 0;
    mode[0]=0; if(a[1])(void)strncpy_from_user(mode,(const char*)(uintptr_t)a[1],sizeof(mode));
    char kp[ABI_PATH_MAX]; win32_xlate_winpath(path,kp,sizeof(kp));
    uint64_t ubuf=WIN32_CRT_DATA_BASE+WIN32_CRT_PATHBUF; int di=0; while(kp[di])di++;
    if(copy_to_user((void*)(uintptr_t)ubuf,kp,(size_t)di+1)!=0)return 0;
    int wr=0,rd=0,plus=0; for(int i=0;mode[i];i++){if(mode[i]=='w'||mode[i]=='a')wr=1;if(mode[i]=='r')rd=1;if(mode[i]=='+')plus=1;}
    int flags;
    if(plus)flags=ABI_O_RDWR|(wr?(ABI_O_CREAT|ABI_O_TRUNC):0);
    else if(wr)flags=ABI_O_WRONLY|ABI_O_CREAT|ABI_O_TRUNC;
    else flags=ABI_O_RDONLY; (void)rd;
    long fd=sys_open((const char*)(uintptr_t)ubuf,flags,0644);
    if(fd<0)return 0;
    return (long)(WIN32_IOB_TAG|(uint64_t)((uint32_t)fd&0xFF));
}
static long w32_fclose(uint64_t *a){int fd=win32_file_fd(a[0]); if(fd>=3)(void)sys_close(fd); return 0;}
/* fread(ptr=a0, size=a1, nmemb=a2, FILE*=a3). (C19b: the FILE* is a3 like
 * fwrite -- a latent arg-swap here read the fd from the buffer ptr and wrote
 * into the FILE* token; SQLite never hit it as its VFS uses ReadFile, but real
 * Lua's script loader reads via fread.) */
static long w32_fread(uint64_t *a){int fd=win32_file_fd(a[3]); if(fd<0)return 0; uint64_t sz=a[1],nm=a[2]; if(!sz)return 0;
    long got=sys_read(fd,(void*)(uintptr_t)a[0],sz*nm); return got>0?(long)((uint64_t)got/sz):0;}
static long w32_fwrite(uint64_t *a){int fd=win32_file_fd(a[3]); if(fd<0)return 0; uint64_t sz=a[1],nm=a[2]; if(!sz)return 0;
    long w=sys_write(fd,(const void*)(uintptr_t)a[0],sz*nm); return w>0?(long)((uint64_t)w/sz):0;}
static long w32_fputs(uint64_t *a){int fd=win32_file_fd(a[1]); if(fd<0)return -1;
    char s[W32_STRCAP]; int n=(int)strncpy_from_user(s,(const char*)(uintptr_t)a[0],sizeof(s)); if(n<0)return -1;
    return sys_write(fd,(const void*)(uintptr_t)a[0],(size_t)n)>=0?0:-1;}
static long w32_fgetc(uint64_t *a){int fd=win32_file_fd(a[0]); if(fd<0)return -1;
    uint64_t ub=WIN32_CRT_DATA_BASE+WIN32_CRT_PATHBUF; long g=sys_read(fd,(void*)(uintptr_t)ub,1);
    if(g<=0)return -1; unsigned char c; if(copy_from_user(&c,(const void*)(uintptr_t)ub,1)!=0)return -1; return (long)c;}
static long w32_fgets(uint64_t *a){int fd=win32_file_fd(a[2]); if(fd<0)return 0; int n=(int)(int32_t)a[1]; if(n<=1)return 0;
    int i=0; for(;i<n-1;i++){long g=sys_read(fd,(void*)(uintptr_t)(a[0]+(uint64_t)i),1); if(g<=0)break;
        unsigned char c; if(copy_from_user(&c,(const void*)(uintptr_t)(a[0]+(uint64_t)i),1)!=0)break; if(c=='\n'){i++;break;}}
    if(i==0)return 0; char z=0; (void)copy_to_user((void*)(uintptr_t)(a[0]+(uint64_t)i),&z,1); return (long)a[0];}
static long w32_fseek(uint64_t *a){int fd=win32_file_fd(a[0]); if(fd<0)return -1;
    return sys_lseek(fd,(int64_t)(int32_t)a[1],(int)a[2])<0?-1:0;}
static long w32_ftell(uint64_t *a){int fd=win32_file_fd(a[0]); if(fd<0)return -1; return sys_lseek(fd,0,1);}
static long w32_rewind(uint64_t *a){int fd=win32_file_fd(a[0]); if(fd>=0)(void)sys_lseek(fd,0,0); return 0;}
static long w32_fileno(uint64_t *a){return win32_file_fd(a[0]);}
static long w32_get_osfhandle(uint64_t *a){return (long)(int)a[0];}     /* HANDLE == fd */
static long w32_isatty(uint64_t *a){(void)a;return 0;}                  /* batch, not a tty */
static long w32_setmode(uint64_t *a){(void)a;return 0;}
/* ============================================================
 * C22: the scanf family. ucrt's sscanf/fscanf/scanf inline to the stdio
 * primitives __stdio_common_vsscanf (input = a string buffer) and
 * __stdio_common_vfscanf (input = a FILE*), whose va_list is the last (stack)
 * arg the C2 gate marshals. The engine below parses a NUL-terminated KERNEL
 * input buffer per a user format string, reading one user pointer per
 * conversion from the va_list and copy_to_user-ing the parsed value. Floats use
 * the -msse kmath_strtod (this file is -mno-sse). Mirrors the native libc
 * sscanf but adds %f/%e/%g, '*' (assignment suppression), and field width.
 * ============================================================ */
static int w32_sc_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
/* Returns the number of assigned fields; *consumed gets input bytes consumed. */
static long win32_vscanf(const char *s, size_t slen, uint64_t ufmt, uint64_t uva, int *consumed) {
    char fmt[512];
    long fl = strncpy_from_user(fmt, (const char *)(uintptr_t)ufmt, sizeof(fmt));
    if (fl < 0) { if (consumed) *consumed = 0; return -1; }
    fmt[sizeof(fmt) - 1] = '\0';

    int count = 0, ai = 0;
    size_t si = 0;
    const char *f = fmt;

    while (*f) {
        if (w32_sc_isspace(*f)) {                 /* whitespace in fmt: skip input ws */
            while (si < slen && w32_sc_isspace(s[si])) si++;
            f++;
            continue;
        }
        if (*f != '%') {                          /* literal: must match */
            if (si >= slen || s[si] != *f) goto done;
            si++; f++;
            continue;
        }
        f++;                                      /* skip '%' */
        if (*f == '%') { if (si >= slen || s[si] != '%') goto done; si++; f++; continue; }

        int suppress = 0;
        if (*f == '*') { suppress = 1; f++; }
        int width = 0;
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        int lng = 0;                              /* 0=int/float, 1=long/double, 2=long long */
        if (*f == 'l') { lng = 1; f++; if (*f == 'l') { lng = 2; f++; } }
        else if (*f == 'h') { f++; if (*f == 'h') f++; }
        else if (*f == 'L' || *f == 'j' || *f == 'z' || *f == 't') { lng = (*f == 'L') ? 1 : 2; f++; }
        char conv = *f ? *f++ : '\0';
        if (!conv) break;

        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            while (si < slen && w32_sc_isspace(s[si])) si++;     /* skip leading ws */
            int base = (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : (conv == 'o' ? 8 : 10);
            int is_signed = (conv == 'd' || conv == 'i');
            int neg = 0, ndig = 0;
            size_t start = si;
            int maxw = width > 0 ? width : 0x7fffffff;
            int taken = 0;
            if (si < slen && (s[si] == '-' || s[si] == '+')) { neg = (s[si] == '-'); si++; taken++; }
            if ((conv == 'i' || conv == 'x' || conv == 'X' || conv == 'p') && taken < maxw &&
                si + 1 < slen && s[si] == '0' && (s[si+1] == 'x' || s[si+1] == 'X')) {
                base = 16; si += 2; taken += 2;
            }
            unsigned long long v = 0;
            while (si < slen && taken < maxw) {
                char c = s[si]; int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                if (d >= base) break;
                v = v * (unsigned)base + (unsigned)d; si++; taken++; ndig++;
            }
            if (ndig == 0) { si = start; goto done; }
            long long sv = neg ? -(long long)v : (long long)v;
            if (!suppress) {
                uint64_t p = va_next(uva, &ai);
                if (p) {
                    uint64_t out = is_signed ? (uint64_t)sv : (uint64_t)v;
                    (void)copy_to_user((void *)(uintptr_t)p, &out, (lng >= 2) ? 8 : 4);
                }
                count++;
            }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            while (si < slen && w32_sc_isspace(s[si])) si++;
            int cons = 0; uint32_t fb = 0;
            uint64_t db = kmath_strtod(s + si, &cons, &fb);
            if (width > 0 && cons > width) cons = width;     /* approx width honouring */
            if (cons == 0) goto done;
            si += (size_t)cons;
            if (!suppress) {
                uint64_t p = va_next(uva, &ai);
                if (p) {
                    if (lng >= 1) (void)copy_to_user((void *)(uintptr_t)p, &db, 8);
                    else          (void)copy_to_user((void *)(uintptr_t)p, &fb, 4);
                }
                count++;
            }
            break;
        }
        case 's': {
            while (si < slen && w32_sc_isspace(s[si])) si++;
            if (si >= slen) goto done;
            char tmp[1024]; int tn = 0;
            int maxw = width > 0 ? width : (int)sizeof(tmp) - 1;
            while (si < slen && !w32_sc_isspace(s[si]) && tn < maxw && tn < (int)sizeof(tmp) - 1)
                tmp[tn++] = s[si++];
            tmp[tn] = '\0';
            if (!suppress) {
                uint64_t p = va_next(uva, &ai);
                if (p) (void)copy_to_user((void *)(uintptr_t)p, tmp, (size_t)tn + 1);
                count++;
            }
            break;
        }
        case 'c': {
            int w = width > 0 ? width : 1;
            if (si + (size_t)w > slen) goto done;
            char tmp[256]; if (w > (int)sizeof(tmp)) w = (int)sizeof(tmp);
            for (int i = 0; i < w; i++) tmp[i] = s[si++];
            if (!suppress) {
                uint64_t p = va_next(uva, &ai);
                if (p) (void)copy_to_user((void *)(uintptr_t)p, tmp, (size_t)w);
                count++;
            }
            break;
        }
        case 'n': {                               /* chars consumed so far; no count++ */
            uint64_t p = va_next(uva, &ai);
            int n = (int)si;
            if (p) (void)copy_to_user((void *)(uintptr_t)p, &n, 4);
            break;
        }
        default:
            goto done;
        }
    }
done:
    if (consumed) *consumed = (int)si;
    return count;
}

/* __stdio_common_vsscanf(options, const char* buffer, size_t count, const char*
 * fmt, _locale_t, va_list) -> assigned-field count. */
static long w32_vsscanf(uint64_t *a) {
    if (!a[1] || !a[3] || !a[5]) return -1;
    char in[1024];
    long n = strncpy_from_user(in, (const char *)(uintptr_t)a[1], sizeof(in));
    if (n < 0) return -1;
    size_t slen = (n >= (long)sizeof(in)) ? sizeof(in) - 1 : (size_t)n;
    in[slen] = '\0';
    int consumed = 0;
    return win32_vscanf(in, slen, a[3], a[5], &consumed);
}

/* __stdio_common_vfscanf(options, FILE* stream, const char* fmt, _locale_t,
 * va_list) -> assigned-field count. Reads a bounded chunk at the file's current
 * offset, parses it, then re-seeks the fd to just past the consumed bytes so the
 * next read continues correctly (a regular file; stdin is read once). */
static long w32_vfscanf(uint64_t *a) {
    int fd = win32_file_fd(a[1]);
    if (fd < 0 || !a[2] || !a[4]) return -1;
    struct file *f = fd_lookup(fd);
    if (!f) return -1;
    int64_t startpos = sys_lseek(fd, 0, 1);       /* SEEK_CUR; <0 if not seekable */
    char in[1024];
    long r = file_read(f, in, sizeof(in) - 1);    /* straight into a kernel buffer */
    if (r <= 0) return -1;
    size_t slen = (size_t)r; in[slen] = '\0';
    int consumed = 0;
    long cnt = win32_vscanf(in, slen, a[2], a[4], &consumed);
    if (startpos >= 0) (void)sys_lseek(fd, startpos + (int64_t)consumed, 0);  /* SEEK_SET */
    return cnt;
}
/* C19b: stdio status/pushback. Our fd-backed FILE* shims signal EOF/error via
 * return values, so feof/ferror/clearerr are no-ops; ungetc rewinds one byte. */
static long w32_feof(uint64_t *a){(void)a;return 0;}
static long w32_ferror(uint64_t *a){(void)a;return 0;}
static long w32_clearerr(uint64_t *a){(void)a;return 0;}
static long w32_ungetc(uint64_t *a){int fd=win32_file_fd(a[1]); int c=(int)a[0];
    if(fd<0||c<0)return -1; long pos=sys_lseek(fd,0,1); if(pos>0)(void)sys_lseek(fd,pos-1,0); return c;}
/* freopen(path,mode,stream): tobyOS has no real reopen; accept by returning the
 * same stream token unchanged (covers the common freopen(NULL,"rb",std*) case). */
static long w32_freopen(uint64_t *a){return (long)a[2];}
/* setlocale(cat, name) -> a stable "C" string; tobyOS is C-locale only. */
static long w32_setlocale(uint64_t *a){(void)a;
    uint64_t ub=WIN32_CRT_DATA_BASE+WIN32_CRT_PATHBUF; char c[2]={'C',0};
    return (copy_to_user((void*)(uintptr_t)ub,c,2)==0)?(long)ub:0;}
static long w32__configthreadlocale(uint64_t *a){(void)a;return 0;}     /* _DISABLE_PER_THREAD */

/* ---- environment / process ---- */
static long w32_getenv(uint64_t *a) {
    env_seed();
    char name[ENV_NAME_LEN]; if(!a[0]||strncpy_from_user(name,(const char*)(uintptr_t)a[0],sizeof(name))<0)return 0;
    int i=env_find(name); if(i<0)return 0;
    const char *v=g_win32_env[i].val; int n=0; while(v[n])n++;
    uint64_t p=win32_malloc_hdr((uint64_t)n+1); if(!p)return 0;
    (void)copy_to_user((void*)(uintptr_t)p,v,(size_t)n+1); return (long)p;
}
static long w32_getpid(uint64_t *a){(void)a;struct proc*p=current_proc();return p?(long)p->pid:1;}
static long w32_system(uint64_t *a){(void)a;return -1;}
static long w32_assert(uint64_t *a){(void)a;kprintf("[win32] _assert failed\n");proc_exit(134);return 0;}

/* ---- file / path CRT (onto the VFS) ---- */
static uint64_t win32_stage_winpath(const char *winpath_user) {
    char wp[ABI_PATH_MAX]; if(strncpy_from_user(wp,winpath_user,sizeof(wp))<0)return 0;
    char kp[ABI_PATH_MAX]; int di=win32_xlate_winpath(wp,kp,sizeof(kp)); if(di<0)return 0;
    uint64_t ubuf=WIN32_CRT_DATA_BASE+WIN32_CRT_PATHBUF;
    if(copy_to_user((void*)(uintptr_t)ubuf,kp,(size_t)di+1)!=0)return 0; return ubuf;
}
static long w32_access(uint64_t *a) {
    char wp[ABI_PATH_MAX]; if(strncpy_from_user(wp,(const char*)(uintptr_t)a[0],sizeof(wp))<0)return -1;
    char kp[ABI_PATH_MAX]; win32_xlate_winpath(wp,kp,sizeof(kp));
    struct vfs_stat vs; return (vfs_stat(kp,&vs)==VFS_OK)?0:-1;
}
static long w32_chmod(uint64_t *a){(void)a;return 0;}
static long w32_crt_mkdir(uint64_t *a){uint64_t up=win32_stage_winpath((const char*)(uintptr_t)a[0]); return (up&&sys_mkdir((const char*)(uintptr_t)up,0755)==0)?0:-1;}
static long w32_crt_unlink(uint64_t *a){uint64_t up=win32_stage_winpath((const char*)(uintptr_t)a[0]); return (up&&sys_unlink((const char*)(uintptr_t)up)==0)?0:-1;}
/* C19b: remove(path) == unlink. rename has no kernel primitive yet -> reports
 * failure (Lua's os.rename surfaces it as nil,errmsg; uncalled by the test). */
static long w32_crt_remove(uint64_t *a){uint64_t up=win32_stage_winpath((const char*)(uintptr_t)a[0]); return (up&&sys_unlink((const char*)(uintptr_t)up)==0)?0:-1;}
static long w32_crt_rename(uint64_t *a){(void)a;return -1;}
static long w32_crt_wunlink(uint64_t *a){char wp[ABI_PATH_MAX]; win32_wstr_to_k(a[0],wp,sizeof(wp));
    char kp[ABI_PATH_MAX]; int di=win32_xlate_winpath(wp,kp,sizeof(kp)); if(di<0)return -1;
    uint64_t ubuf=WIN32_CRT_DATA_BASE+WIN32_CRT_PATHBUF; if(copy_to_user((void*)(uintptr_t)ubuf,kp,(size_t)di+1)!=0)return -1;
    return sys_unlink((const char*)(uintptr_t)ubuf)==0?0:-1;}
static long w32_fullpath(uint64_t *a){    /* _fullpath(absPath, relPath, maxLen) */
    char rp[ABI_PATH_MAX]; int n=(int)strncpy_from_user(rp,(const char*)(uintptr_t)a[1],sizeof(rp)); if(n<0)return 0;
    if(!a[0])return (long)a[1]; int cap=(int)(uint32_t)a[2]; int w=n<cap-1?n:cap-1;
    (void)copy_to_user((void*)(uintptr_t)a[0],rp,(size_t)w); char z=0;(void)copy_to_user((void*)(uintptr_t)(a[0]+w),&z,1);
    return (long)a[0];
}
/* _stat64i32(path, &st): fill st_mode@6 + st_size@20 (48-byte struct). */
static void win32_fill_stat(uint64_t out, const struct vfs_stat *vs) {
    uint8_t b[48]; memset(b,0,sizeof(b));
    uint16_t mode = (vs->type==VFS_TYPE_DIR) ? 0x4000|0x1FF : 0x8000|0x1B6;  /* S_IFDIR / S_IFREG */
    memcpy(&b[6],&mode,2);
    int32_t sz=(int32_t)vs->size; memcpy(&b[20],&sz,4);
    int64_t mt=(int64_t)rtc_unix_time(); memcpy(&b[24],&mt,8); memcpy(&b[32],&mt,8); memcpy(&b[40],&mt,8);
    (void)copy_to_user((void*)(uintptr_t)out,b,sizeof(b));
}
static long w32_stat64i32(uint64_t *a){
    char wp[ABI_PATH_MAX]; if(strncpy_from_user(wp,(const char*)(uintptr_t)a[0],sizeof(wp))<0)return -1;
    char kp[ABI_PATH_MAX]; win32_xlate_winpath(wp,kp,sizeof(kp));
    struct vfs_stat vs; if(vfs_stat(kp,&vs)!=VFS_OK)return -1; win32_fill_stat(a[1],&vs); return 0;
}
static long w32_fstat64(uint64_t *a){
    struct file *f=fd_lookup((int)a[0]); if(!f)return -1;
    struct vfs_stat vs; memset(&vs,0,sizeof(vs)); vs.type=VFS_TYPE_FILE;
    if(f->kind==FILE_KIND_VFS)vs.size=f->vfs.size; win32_fill_stat(a[1],&vs); return 0;
}
static long w32_findclose_crt(uint64_t *a){(void)a;return 0;}
static long w32_findfirst_crt(uint64_t *a){(void)a;return -1;}          /* no match */

/* _localtime64(const __time64_t*) -> struct tm* (filled in app heap). */
static long w32_localtime64(uint64_t *a) {
    int64_t t=0; if(a[0])(void)copy_from_user(&t,(const void*)(uintptr_t)a[0],8);
    int64_t days=t/86400, rem=t%86400; if(rem<0){rem+=86400;days--;}
    int sec=(int)(rem%60), mn=(int)((rem/60)%60), hr=(int)(rem/3600);
    /* civil from days since 1970 (Howard Hinnant) */
    int64_t z=days+719468; int64_t era=(z>=0?z:z-146096)/146097; int doe=(int)(z-era*146097);
    int yoe=(doe-doe/1460+doe/36524-doe/146096)/365; int y=(int)(yoe+era*400);
    int doy=doe-(365*yoe+yoe/4-yoe/100); int mp=(5*doy+2)/153; int d=doy-(153*mp+2)/5+1;
    int m=mp+(mp<10?3:-9); y+=(m<=2);
    int wday=(int)((days%7+4)%7); if(wday<0)wday+=7;
    int32_t tm[9]={sec,mn,hr,d,m-1,y-1900,wday,0,0};
    uint64_t p=win32_malloc_hdr(sizeof(tm)); if(!p)return 0;
    (void)copy_to_user((void*)(uintptr_t)p,tm,sizeof(tm)); return (long)p;
}

/* C19b: time()/clock() return integer time_t/clock_t (marshal fine through the
 * gate; difftime/gmtime/strftime stay out -- difftime returns a double). Real
 * Lua seeds its RNG from time(NULL) in luaopen_math, so this is on the startup
 * path. Epoch base is uptime-relative -- adequate for a seed; tobyOS has no RTC
 * wall-clock plumbed into the Win32 layer. */
static long w32_time64(uint64_t *a){
    uint64_t hz=pit_hz(); if(!hz)hz=100; int64_t secs=(int64_t)(pit_ticks()/hz);
    if(a[0])(void)copy_to_user((void*)(uintptr_t)a[0],&secs,8); return (long)secs;
}
static long w32_clock(uint64_t *a){(void)a;
    uint64_t hz=pit_hz(); if(!hz)hz=100; return (long)(pit_ticks()*1000ull/hz);  /* CLOCKS_PER_SEC=1000 */
}

/* ---- a few more kernel32 ---- */
static long w32_GetDiskFreeSpaceW(uint64_t *a){return w32_GetDiskFreeSpaceA(a);}
static long w32_SetFileTime(uint64_t *a){(void)a;return 1;}
/* Real GetLastError/SetLastError (was a 0 stub). SQLite's winOpen/winAccess read
 * it to distinguish created-vs-existing files and ERROR_FILE_NOT_FOUND from a
 * real I/O error. (g_win32_lasterr is defined up by g_wsa_lasterror.) */
static long w32_GetLastError(uint64_t *a){(void)a;return (long)g_win32_lasterr;}
static long w32_SetLastError(uint64_t *a){g_win32_lasterr=(uint32_t)a[0];return 0;}

/* ================= C19c: minimal COM / ole32 (TIGHTLY SCOPED) =================
 * A DELIBERATELY NARROW slice -- NOT a real COM runtime. tobyOS supports exactly
 * ONE coclass (CLSID_TobyAdder) exposing ONE interface (ITobyAdder : IUnknown)
 * with ONE real method (Add); every other CLSID/IID returns REGDB_E_CLASSNOTREG
 * / E_NOINTERFACE. It proves the genuine COM ABI: CoInitializeEx/CoUninitialize,
 * CoTaskMemAlloc/Free (-> the Win32 heap), and CoCreateInstance returning a real
 * vtable-backed object on which QueryInterface/AddRef/Release + Add are called
 * THROUGH THE VTABLE. The hard part -- dispatching a kernel-provided object's
 * methods back into the kernel -- reuses the C9 procaddr gate-thunks: each vtable
 * slot holds the fixed VA of a `mov eax,idx; jmp gate` thunk, so
 * obj->lpVtbl->Method(this,...) traps into the matching shim with `this` as
 * arg0. The object lives in user memory (win32_heap_alloc): [+0 lpVtbl, +8
 * refcount(i32), +12 magic(u32)]. */
#define COM_S_OK                 0L
#define COM_E_NOINTERFACE        ((long)0x80004002L)
#define COM_E_POINTER            ((long)0x80004003L)
#define COM_E_FAIL               ((long)0x80004005L)
#define COM_E_OUTOFMEMORY        ((long)0x8007000EL)
#define COM_CLASS_E_NOAGGREGATION ((long)0x80040110L)
#define COM_REGDB_E_CLASSNOTREG  ((long)0x80040154L)
#define COM_OBJ_MAGIC            0x434F424Au   /* 'COBJ' */

/* GUIDs as raw 16-byte blobs (the app passes pointers to identical bytes). */
static const uint8_t COM_IID_IUnknown[16]   = {0,0,0,0, 0,0, 0,0, 0xC0,0,0,0,0,0,0,0x46};
static const uint8_t COM_CLSID_TobyAdder[16]= {0x79,0x62,0x6F,0x74, 0x44,0x41, 0x52,0x44,
                                               'T','O','B','Y','C','L','S','D'};
static const uint8_t COM_IID_ITobyAdder[16] = {0x79,0x62,0x6F,0x74, 0x49,0x44, 0x46,0x49,
                                               'T','O','B','Y','I','F','A','D'};

static int win32_guid_eq_user(uint64_t uptr, const uint8_t *kguid){
    uint8_t g[16]; if(!uptr||copy_from_user(g,(const void*)(uintptr_t)uptr,16)!=0) return 0;
    for(int i=0;i<16;i++) if(g[i]!=kguid[i]) return 0; return 1;
}
/* The CPL3-callable VA of the procaddr gate-thunk for a named shim (C9). */
static uint64_t win32_com_thunk(const char *fn){
    int idx = win32_shim_index("ole32.dll", fn);
    if(idx<0||idx>=win32_shim_count()) return 0;
    return WIN32_PROCADDR_BASE_VA + (uint64_t)idx*WIN32_PROCADDR_STRIDE;
}
/* Validate that `self` points at one of our live COM objects. */
static int win32_com_obj_ok(uint64_t self){
    if(!self) return 0; uint32_t m=0;
    if(copy_from_user(&m,(const void*)(uintptr_t)(self+12),4)!=0) return 0;
    return m==COM_OBJ_MAGIC;
}
static int win32_com_ref_get(uint64_t self){ int32_t r=0;
    (void)copy_from_user(&r,(const void*)(uintptr_t)(self+8),4); return r; }
static void win32_com_ref_set(uint64_t self,int32_t r){
    (void)copy_to_user((void*)(uintptr_t)(self+8),&r,4); }

static long w32_CoInitializeEx(uint64_t *a){(void)a;return COM_S_OK;}
static long w32_CoInitialize(uint64_t *a){(void)a;return COM_S_OK;}
static long w32_CoUninitialize(uint64_t *a){(void)a;return 0;}
static long w32_CoTaskMemAlloc(uint64_t *a){return (long)win32_malloc_hdr(a[0]);}
static long w32_CoTaskMemFree(uint64_t *a){(void)a;return 0;}   /* arena heap: no-op */

/* CoCreateInstance(rclsid=a0, pUnkOuter=a1, dwClsCtx=a2, riid=a3, ppv=a4). */
static long w32_CoCreateInstance(uint64_t *a){
    uint64_t rclsid=a[0], pUnkOuter=a[1], riid=a[3], ppv=a[4]; (void)a[2];
    if(!ppv) return COM_E_POINTER;
    uint64_t zero=0; (void)copy_to_user((void*)(uintptr_t)ppv,&zero,8);
    if(pUnkOuter) return COM_CLASS_E_NOAGGREGATION;
    if(!win32_guid_eq_user(rclsid,COM_CLSID_TobyAdder)) return COM_REGDB_E_CLASSNOTREG;
    if(!win32_guid_eq_user(riid,COM_IID_ITobyAdder) &&
       !win32_guid_eq_user(riid,COM_IID_IUnknown))     return COM_E_NOINTERFACE;
    uint64_t vt  = win32_heap_alloc(4*8);
    uint64_t obj = win32_heap_alloc(16);
    if(!vt||!obj) return COM_E_OUTOFMEMORY;
    uint64_t e[4] = { win32_com_thunk("__toby_com_QueryInterface"),
                      win32_com_thunk("__toby_com_AddRef"),
                      win32_com_thunk("__toby_com_Release"),
                      win32_com_thunk("__toby_com_Add") };
    if(!e[0]||!e[1]||!e[2]||!e[3]) return COM_E_FAIL;
    if(copy_to_user((void*)(uintptr_t)vt,e,sizeof(e))!=0) return COM_E_FAIL;
    int32_t one=1; uint32_t magic=COM_OBJ_MAGIC;
    if(copy_to_user((void*)(uintptr_t)(obj+0),&vt,8)!=0) return COM_E_FAIL;
    (void)copy_to_user((void*)(uintptr_t)(obj+8),&one,4);
    (void)copy_to_user((void*)(uintptr_t)(obj+12),&magic,4);
    (void)copy_to_user((void*)(uintptr_t)ppv,&obj,8);
    return COM_S_OK;
}
/* Vtable methods -- reached only via the object's gate-thunks; `this`==a0. */
static long w32_com_QueryInterface(uint64_t *a){
    uint64_t self=a[0], riid=a[1], ppv=a[2];
    if(!ppv) return COM_E_POINTER;
    uint64_t zero=0; (void)copy_to_user((void*)(uintptr_t)ppv,&zero,8);
    if(!win32_com_obj_ok(self)) return COM_E_POINTER;
    if(win32_guid_eq_user(riid,COM_IID_ITobyAdder)||win32_guid_eq_user(riid,COM_IID_IUnknown)){
        win32_com_ref_set(self, win32_com_ref_get(self)+1);
        (void)copy_to_user((void*)(uintptr_t)ppv,&self,8);
        return COM_S_OK;
    }
    return COM_E_NOINTERFACE;
}
static long w32_com_AddRef(uint64_t *a){
    uint64_t self=a[0]; if(!win32_com_obj_ok(self)) return 0;
    int32_t r=win32_com_ref_get(self)+1; win32_com_ref_set(self,r); return r;
}
static long w32_com_Release(uint64_t *a){
    uint64_t self=a[0]; if(!win32_com_obj_ok(self)) return 0;
    int32_t r=win32_com_ref_get(self)-1; if(r<0)r=0; win32_com_ref_set(self,r);
    if(r==0){ uint32_t dead=0; (void)copy_to_user((void*)(uintptr_t)(self+12),&dead,4); }
    return r;
}
/* The one real interface method: Add(this, x, y, *out) -> S_OK, *out = x+y. */
static long w32_com_Add(uint64_t *a){
    uint64_t self=a[0]; int32_t x=(int32_t)(uint32_t)a[1], y=(int32_t)(uint32_t)a[2];
    uint64_t pout=a[3];
    if(!win32_com_obj_ok(self)||!pout) return COM_E_POINTER;
    int32_t s=x+y; return (copy_to_user((void*)(uintptr_t)pout,&s,4)==0)?COM_S_OK:COM_E_FAIL;
}

/* ================= C20: float-return CRT math (libm) =================
 * These shims return a `double` in xmm0, so their IAT/procaddr thunks jmp to the
 * float-return gate (WIN32_FGATE_VA) instead of the integer gate -- the loader
 * picks the gate via win32_shim_is_double_ret() below. The float gate marshals
 * the four MS-x64 FP-arg registers as bit patterns into a[4..7] (and the integer
 * regs into a[0..3]); the actual FP runs in the -msse kmath.c (FXSAVE-guarded),
 * exchanged as bit patterns since this file is -mno-sse. The gate does
 * `movq xmm0, rax` on the returned bits. One-arg funcs read xmm0 = a[4]; two-arg
 * read xmm0,xmm1 = a[4],a[5]; ldexp's int exponent is the 2nd integer arg a[1]. */
static long w32_sqrt (uint64_t *a){ return (long)kmath_op1(KM_SQRT , a[4]); }
static long w32_sin  (uint64_t *a){ return (long)kmath_op1(KM_SIN  , a[4]); }
static long w32_cos  (uint64_t *a){ return (long)kmath_op1(KM_COS  , a[4]); }
static long w32_tan  (uint64_t *a){ return (long)kmath_op1(KM_TAN  , a[4]); }
static long w32_exp  (uint64_t *a){ return (long)kmath_op1(KM_EXP  , a[4]); }
static long w32_exp2 (uint64_t *a){ return (long)kmath_op1(KM_EXP2 , a[4]); }
static long w32_log  (uint64_t *a){ return (long)kmath_op1(KM_LOG  , a[4]); }
static long w32_log10(uint64_t *a){ return (long)kmath_op1(KM_LOG10, a[4]); }
static long w32_log2 (uint64_t *a){ return (long)kmath_op1(KM_LOG2 , a[4]); }
static long w32_floor(uint64_t *a){ return (long)kmath_op1(KM_FLOOR, a[4]); }
static long w32_ceil (uint64_t *a){ return (long)kmath_op1(KM_CEIL , a[4]); }
static long w32_trunc(uint64_t *a){ return (long)kmath_op1(KM_TRUNC, a[4]); }
static long w32_round(uint64_t *a){ return (long)kmath_op1(KM_ROUND, a[4]); }
static long w32_fabs (uint64_t *a){ return (long)kmath_op1(KM_FABS , a[4]); }
static long w32_atan (uint64_t *a){ return (long)kmath_op1(KM_ATAN , a[4]); }
static long w32_asin (uint64_t *a){ return (long)kmath_op1(KM_ASIN , a[4]); }
static long w32_acos (uint64_t *a){ return (long)kmath_op1(KM_ACOS , a[4]); }
static long w32_sinh (uint64_t *a){ return (long)kmath_op1(KM_SINH , a[4]); }
static long w32_cosh (uint64_t *a){ return (long)kmath_op1(KM_COSH , a[4]); }
static long w32_tanh (uint64_t *a){ return (long)kmath_op1(KM_TANH , a[4]); }
static long w32_cbrt (uint64_t *a){ return (long)kmath_op1(KM_CBRT , a[4]); }
static long w32_pow  (uint64_t *a){ return (long)kmath_op2(KM2_POW  , a[4], a[5]); }
static long w32_fmod (uint64_t *a){ return (long)kmath_op2(KM2_FMOD , a[4], a[5]); }
static long w32_atan2(uint64_t *a){ return (long)kmath_op2(KM2_ATAN2, a[4], a[5]); }
static long w32_hypot(uint64_t *a){ return (long)kmath_op2(KM2_HYPOT, a[4], a[5]); }
static long w32_ldexp(uint64_t *a){ return (long)kmath_ldexp(a[4], (long)(int)(uint32_t)a[1]); }
/* frexp(double x, int *e): x = xmm0 = a[4], e = 2nd integer arg = a[1] (rdx). */
static long w32_frexp(uint64_t *a){
    int e = 0; uint64_t mb = kmath_frexp(a[4], &e);
    if (a[1]) copy_to_user((void *)(uintptr_t)a[1], &e, sizeof(e));
    return (long)mb;
}

/* C25: comctl32. InitCommonControlsEx registers the common-control window
 * classes; in our model the classes are recognised unconditionally by
 * win32_ctrl_class(), so this just acknowledges. ImageList_* are tolerated
 * stubs (report-view ListViews here carry no images). */
static long w32_InitCommonControlsEx(uint64_t *a) { (void)a; return 1; }
static long w32_InitCommonControls(uint64_t *a)   { (void)a; return 0; }
static long w32_ImageList_Create(uint64_t *a)     { (void)a; return 0x69000001; }
static long w32_ImageList_AddMasked(uint64_t *a)  { (void)a; return 0; }
static long w32_ImageList_ReplaceIcon(uint64_t *a){ (void)a; return 0; }
static long w32_ImageList_Destroy(uint64_t *a)    { (void)a; return 1; }

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
    /* C23: wide (UTF-16) stdio. */
    { "api-ms-win-crt-stdio-l1-1-0.dll", "__stdio_common_vfwprintf", w32_stdio_common_vfwprintf },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "__stdio_common_vswprintf", w32_stdio_common_vswprintf },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "__stdio_common_vswprintf_s", w32_stdio_common_vswprintf },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fputws",                   w32_fputws },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "_putws",                   w32_putws },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fputwc",                   w32_fputwc },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "putwchar",                 w32_putwchar },

    /* ---- C3: the rest of the stock-clang mainCRTStartup surface ---- */
    /* kernel32. The critical-section funcs are REAL as of C6 (mutual
     * exclusion); DeleteCriticalSection stays a no-op. */
    { "kernel32.dll", "InitializeCriticalSection", w32_InitializeCriticalSection },
    { "kernel32.dll", "DeleteCriticalSection",     w32_zero },
    { "kernel32.dll", "EnterCriticalSection",      w32_EnterCriticalSection },
    { "kernel32.dll", "LeaveCriticalSection",      w32_LeaveCriticalSection },
    { "kernel32.dll", "GetLastError",              w32_GetLastError },
    { "kernel32.dll", "SetLastError",              w32_SetLastError },
    { "kernel32.dll", "SetUnhandledExceptionFilter", w32_zero },
    { "kernel32.dll", "Sleep",                     w32_Sleep },
    { "kernel32.dll", "TlsAlloc",                  w32_TlsAlloc },
    { "kernel32.dll", "TlsFree",                   w32_TlsFree },
    { "kernel32.dll", "TlsGetValue",               w32_TlsGetValue },
    { "kernel32.dll", "TlsSetValue",               w32_TlsSetValue },
    { "kernel32.dll", "VirtualProtect",            w32_one },
    { "kernel32.dll", "VirtualQuery",              w32_VirtualQuery },
    /* ucrt: environment */
    { "api-ms-win-crt-environment-l1-1-0.dll", "__p__environ",       w32_p_environ },
    /* ucrt: heap */
    { "api-ms-win-crt-heap-l1-1-0.dll", "malloc",                    w32_malloc },
    { "api-ms-win-crt-heap-l1-1-0.dll", "calloc",                    w32_calloc },
    { "api-ms-win-crt-heap-l1-1-0.dll", "realloc",                   w32_realloc },   /* C18a */
    { "api-ms-win-crt-heap-l1-1-0.dll", "_msize",                    w32_msize },     /* C18a */
    { "api-ms-win-crt-heap-l1-1-0.dll", "free",                      w32_zero },
    { "api-ms-win-crt-heap-l1-1-0.dll", "_set_new_mode",             w32_zero },
    /* ucrt: locale / math / private */
    { "api-ms-win-crt-locale-l1-1-0.dll", "_configthreadlocale",     w32_zero },
    { "api-ms-win-crt-math-l1-1-0.dll", "__setusermatherr",          w32_zero },
    /* C20: float-return libm (double in xmm0 via the float-return gate). */
    { "api-ms-win-crt-math-l1-1-0.dll", "sqrt",   w32_sqrt },
    { "api-ms-win-crt-math-l1-1-0.dll", "sin",    w32_sin },
    { "api-ms-win-crt-math-l1-1-0.dll", "cos",    w32_cos },
    { "api-ms-win-crt-math-l1-1-0.dll", "tan",    w32_tan },
    { "api-ms-win-crt-math-l1-1-0.dll", "exp",    w32_exp },
    { "api-ms-win-crt-math-l1-1-0.dll", "exp2",   w32_exp2 },
    { "api-ms-win-crt-math-l1-1-0.dll", "log",    w32_log },
    { "api-ms-win-crt-math-l1-1-0.dll", "log10",  w32_log10 },
    { "api-ms-win-crt-math-l1-1-0.dll", "log2",   w32_log2 },
    { "api-ms-win-crt-math-l1-1-0.dll", "floor",  w32_floor },
    { "api-ms-win-crt-math-l1-1-0.dll", "ceil",   w32_ceil },
    { "api-ms-win-crt-math-l1-1-0.dll", "trunc",  w32_trunc },
    { "api-ms-win-crt-math-l1-1-0.dll", "round",  w32_round },
    { "api-ms-win-crt-math-l1-1-0.dll", "fabs",   w32_fabs },
    { "api-ms-win-crt-math-l1-1-0.dll", "atan",   w32_atan },
    { "api-ms-win-crt-math-l1-1-0.dll", "asin",   w32_asin },
    { "api-ms-win-crt-math-l1-1-0.dll", "acos",   w32_acos },
    { "api-ms-win-crt-math-l1-1-0.dll", "sinh",   w32_sinh },
    { "api-ms-win-crt-math-l1-1-0.dll", "cosh",   w32_cosh },
    { "api-ms-win-crt-math-l1-1-0.dll", "tanh",   w32_tanh },
    { "api-ms-win-crt-math-l1-1-0.dll", "cbrt",   w32_cbrt },
    { "api-ms-win-crt-math-l1-1-0.dll", "pow",    w32_pow },
    { "api-ms-win-crt-math-l1-1-0.dll", "fmod",   w32_fmod },
    { "api-ms-win-crt-math-l1-1-0.dll", "atan2",  w32_atan2 },
    { "api-ms-win-crt-math-l1-1-0.dll", "hypot",  w32_hypot },
    { "api-ms-win-crt-math-l1-1-0.dll", "ldexp",  w32_ldexp },
    { "api-ms-win-crt-math-l1-1-0.dll", "frexp",  w32_frexp },
    { "api-ms-win-crt-private-l1-1-0.dll", "__C_specific_handler",   w32_zero },
    { "api-ms-win-crt-private-l1-1-0.dll", "memcpy",                 w32_memcpy },
    { "api-ms-win-crt-private-l1-1-0.dll", "memcmp",                 w32_memcmp },
    { "api-ms-win-crt-private-l1-1-0.dll", "strstr",                 w32_strstr },
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
    { "api-ms-win-crt-locale-l1-1-0.dll", "setlocale",              w32_setlocale },
    { "api-ms-win-crt-locale-l1-1-0.dll", "_configthreadlocale",    w32__configthreadlocale },
    /* runtime extras */
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_errno",                 w32_errno },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "strerror",              w32_strerror },
    /* stdio extras */
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fputc",                    w32_fputc },
    /* string extras */
    { "api-ms-win-crt-string-l1-1-0.dll", "strnlen",                 w32_strnlen },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcslen",                  w32_wcslen },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcsnlen",                 w32_wcslen },
    /* C23: wide (UTF-16) string CRT. */
    { "api-ms-win-crt-string-l1-1-0.dll", "wcscmp",                  w32_wcscmp },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcsncmp",                 w32_wcsncmp },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcscpy",                  w32_wcscpy },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcscpy_s",                w32_wcscpy },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcsncpy",                 w32_wcsncpy },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcscat",                  w32_wcscat },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcschr",                  w32_wcschr },
    { "api-ms-win-crt-string-l1-1-0.dll", "wcsrchr",                 w32_wcsrchr },

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
    { "kernel32.dll", "LoadLibraryExA",      w32_LoadLibraryExA },
    { "kernel32.dll", "GetModuleFileNameA",  w32_GetModuleFileNameA },
    { "kernel32.dll", "FreeLibrary",         w32_FreeLibrary },
    { "kernel32.dll", "GetCurrentProcessId", w32_GetCurrentProcessId },

    /* ---- C11: more GDI (gdi32) ---- */
    { "gdi32.dll",  "CreatePen",      w32_CreatePen },
    { "gdi32.dll",  "SelectObject",   w32_SelectObject },
    { "gdi32.dll",  "MoveToEx",       w32_MoveToEx },
    { "gdi32.dll",  "LineTo",         w32_LineTo },
    { "gdi32.dll",  "Rectangle",      w32_Rectangle },
    { "gdi32.dll",  "Ellipse",        w32_Ellipse },
    { "gdi32.dll",  "SetPixel",       w32_SetPixel },

    /* ---- C11: more file ops (kernel32) ---- */
    { "kernel32.dll", "SetFilePointer",      w32_SetFilePointer },
    { "kernel32.dll", "GetFileSize",         w32_GetFileSize },
    { "kernel32.dll", "DeleteFileA",         w32_DeleteFileA },
    { "kernel32.dll", "CreateDirectoryA",    w32_CreateDirectoryA },
    { "kernel32.dll", "GetFileAttributesA",  w32_GetFileAttributesA },
    /* C18d: shell32 -- CSIDL paths, ShellExecute launch, tray/drag stubs. */
    { "shell32.dll",  "SHGetFolderPathA",        w32_SHGetFolderPathA },
    { "shell32.dll",  "SHGetSpecialFolderPathA", w32_SHGetSpecialFolderPathA },
    { "shell32.dll",  "ShellExecuteA",           w32_ShellExecuteA },
    { "shell32.dll",  "Shell_NotifyIconA",       w32_Shell_NotifyIconA },
    { "shell32.dll",  "DragQueryFileA",          w32_DragQueryFileA },
    /* C25: comctl32 -- common controls (ListView/TreeView/StatusBar/Progress/
     * Toolbar) are driven through SendMessageA; these are the DLL's own exports. */
    { "comctl32.dll", "InitCommonControlsEx",    w32_InitCommonControlsEx },
    { "comctl32.dll", "InitCommonControls",      w32_InitCommonControls },
    { "comctl32.dll", "ImageList_Create",        w32_ImageList_Create },
    { "comctl32.dll", "ImageList_AddMasked",     w32_ImageList_AddMasked },
    { "comctl32.dll", "ImageList_ReplaceIcon",   w32_ImageList_ReplaceIcon },
    { "comctl32.dll", "ImageList_Destroy",       w32_ImageList_Destroy },
    { "kernel32.dll", "FindFirstFileA",      w32_FindFirstFileA },
    { "kernel32.dll", "FindNextFileA",       w32_FindNextFileA },
    { "kernel32.dll", "FindClose",           w32_FindClose },

    /* ---- C12: bitmaps + BitBlt (gdi32) ---- */
    { "gdi32.dll",  "CreateCompatibleDC",     w32_CreateCompatibleDC },
    { "gdi32.dll",  "CreateBitmap",           w32_CreateBitmap },
    { "gdi32.dll",  "CreateCompatibleBitmap", w32_CreateCompatibleBitmap },
    { "gdi32.dll",  "BitBlt",                 w32_BitBlt },
    { "gdi32.dll",  "DeleteDC",               w32_DeleteDC },

    /* ---- C12: BUTTON/EDIT controls (user32) ---- */
    { "user32.dll", "GetWindowTextA",         w32_GetWindowTextA },
    { "user32.dll", "SetWindowTextA",         w32_SetWindowTextA },

    /* ---- C13: more controls + dialogs (user32) ---- */
    { "user32.dll", "SendMessageA",           w32_SendMessageA },
    { "user32.dll", "DrawTextA",              w32_DrawTextA },
    { "user32.dll", "MessageBoxA",            w32_MessageBoxA },
    { "user32.dll", "SetScrollRange",         w32_SetScrollRange },
    { "user32.dll", "SetScrollPos",           w32_SetScrollPos },
    { "user32.dll", "GetScrollPos",           w32_GetScrollPos },
    { "user32.dll", "SetScrollInfo",          w32_SetScrollInfo },
    { "user32.dll", "GetScrollInfo",          w32_GetScrollInfo },
    /* ---- C13: GDI fonts + text metrics (gdi32) ---- */
    { "gdi32.dll",  "CreateFontA",            w32_CreateFontA },
    { "gdi32.dll",  "SetTextColor",           w32_SetTextColor },
    { "gdi32.dll",  "SetBkColor",             w32_SetBkColor },
    { "gdi32.dll",  "SetBkMode",              w32_SetBkMode },
    { "gdi32.dll",  "GetTextExtentPoint32A",  w32_GetTextExtentPoint32A },
    { "gdi32.dll",  "GetTextMetricsA",        w32_GetTextMetricsA },

    /* ---- C14: dialog templates + dialog-item helpers (user32) ----
     * NOTE: DialogBoxParamA is bound by the loader to a CPL3 trampoline (it runs
     * the modal loop + dispatches to the app's DialogProc), so it is NOT in this
     * table; __toby_dlgsvc is the internal service that trampoline calls. */
    { "user32.dll", "__toby_dlgsvc",          w32_toby_dlgsvc },
    { "user32.dll", "EndDialog",              w32_EndDialog },
    { "user32.dll", "CreateDialogParamA",     w32_CreateDialogParamA },
    { "user32.dll", "DefDlgProcA",            w32_DefDlgProcA },
    { "user32.dll", "GetDlgItem",             w32_GetDlgItem },
    { "user32.dll", "GetDlgCtrlID",           w32_GetDlgCtrlID },
    { "user32.dll", "SetDlgItemTextA",        w32_SetDlgItemTextA },
    { "user32.dll", "GetDlgItemTextA",        w32_GetDlgItemTextA },
    { "user32.dll", "CheckDlgButton",         w32_CheckDlgButton },
    { "user32.dll", "IsDlgButtonChecked",     w32_IsDlgButtonChecked },
    { "user32.dll", "CheckRadioButton",       w32_CheckRadioButton },
    { "user32.dll", "SendDlgItemMessageA",    w32_SendDlgItemMessageA },
    { "kernel32.dll", "lstrcmpA",             w32_lstrcmpA },
    { "kernel32.dll", "lstrlenA",             w32_lstrlenA },

    /* ---- C15: menus + timers + multi-button MessageBox (user32) ---- */
    { "user32.dll", "CreateMenu",             w32_CreateMenu },
    { "user32.dll", "CreatePopupMenu",        w32_CreatePopupMenu },
    { "user32.dll", "AppendMenuA",            w32_AppendMenuA },
    { "user32.dll", "DestroyMenu",            w32_DestroyMenu },
    { "user32.dll", "SetMenu",                w32_SetMenu },
    { "user32.dll", "GetMenu",                w32_GetMenu },
    { "user32.dll", "SetTimer",               w32_SetTimer },
    { "user32.dll", "KillTimer",              w32_KillTimer },

    /* ---- C16a: registry (advapi32), persisted to /data ---- */
    { "advapi32.dll", "RegCreateKeyExA",      w32_RegCreateKeyExA },
    { "advapi32.dll", "RegOpenKeyExA",        w32_RegOpenKeyExA },
    { "advapi32.dll", "RegCloseKey",          w32_RegCloseKey },
    { "advapi32.dll", "RegSetValueExA",       w32_RegSetValueExA },
    { "advapi32.dll", "RegQueryValueExA",     w32_RegQueryValueExA },
    { "advapi32.dll", "RegDeleteValueA",      w32_RegDeleteValueA },
    { "advapi32.dll", "RegDeleteKeyA",        w32_RegDeleteKeyA },
    { "advapi32.dll", "RegEnumKeyExA",        w32_RegEnumKeyExA },
    { "advapi32.dll", "RegEnumValueA",        w32_RegEnumValueA },

    /* ---- C16b: common file dialogs (comdlg32) ---- */
    { "comdlg32.dll", "GetOpenFileNameA",     w32_GetOpenFileNameA },
    { "comdlg32.dll", "GetSaveFileNameA",     w32_GetSaveFileNameA },

    /* ---- C16c: command line + environment (kernel32) ---- */
    { "kernel32.dll", "GetCommandLineA",         w32_GetCommandLineA },
    { "kernel32.dll", "GetCommandLineW",         w32_GetCommandLineW },
    { "kernel32.dll", "GetEnvironmentVariableA", w32_GetEnvironmentVariableA },
    { "kernel32.dll", "SetEnvironmentVariableA", w32_SetEnvironmentVariableA },
    { "kernel32.dll", "ExpandEnvironmentStringsA", w32_ExpandEnvironmentStringsA },

    /* ---- C17a: Winsock TCP client (ws2_32) ---- */
    { "ws2_32.dll", "WSAStartup",      w32_WSAStartup },
    { "ws2_32.dll", "WSACleanup",      w32_WSACleanup },
    { "ws2_32.dll", "WSAGetLastError", w32_WSAGetLastError },
    { "ws2_32.dll", "socket",          w32_socket },
    { "ws2_32.dll", "connect",         w32_connect },
    { "ws2_32.dll", "send",            w32_send },
    { "ws2_32.dll", "recv",            w32_recv },
    { "ws2_32.dll", "closesocket",     w32_closesocket },
    { "ws2_32.dll", "gethostbyname",   w32_gethostbyname },
    { "ws2_32.dll", "htons",           w32_htons },
    { "ws2_32.dll", "ntohs",           w32_ntohs },
    { "ws2_32.dll", "htonl",           w32_htonl },
    { "ws2_32.dll", "ntohl",           w32_ntohl },
    { "ws2_32.dll", "inet_addr",       w32_inet_addr },

    /* ---- C17b: Winsock TCP server (ws2_32) ---- */
    { "ws2_32.dll", "bind",            w32_bind },
    { "ws2_32.dll", "listen",          w32_listen },
    { "ws2_32.dll", "accept",          w32_accept },

    /* ---- C17c: Winsock UDP datagrams (ws2_32) ---- */
    { "ws2_32.dll", "sendto",          w32_sendto },
    { "ws2_32.dll", "recvfrom",        w32_recvfrom },

    /* ---- C18a: run real off-the-shelf SQLite (kernel32 breadth) ---- */
    /* Win32 process heap */
    { "kernel32.dll", "GetProcessHeap",      w32_GetProcessHeap },
    { "kernel32.dll", "HeapCreate",          w32_HeapCreate },
    { "kernel32.dll", "HeapDestroy",         w32_HeapDestroy },
    { "kernel32.dll", "HeapAlloc",           w32_HeapAlloc },
    { "kernel32.dll", "HeapReAlloc",         w32_HeapReAlloc },
    { "kernel32.dll", "HeapFree",            w32_HeapFree },
    { "kernel32.dll", "HeapSize",            w32_HeapSize },
    { "kernel32.dll", "HeapValidate",        w32_HeapValidate },
    { "kernel32.dll", "HeapCompact",         w32_HeapCompact },
    /* wide-char conversion */
    { "kernel32.dll", "MultiByteToWideChar", w32_MultiByteToWideChar },
    { "kernel32.dll", "WideCharToMultiByte", w32_WideCharToMultiByte },
    /* wide file APIs (SQLite WinNT VFS) */
    { "kernel32.dll", "CreateFileW",         w32_CreateFileW },
    { "kernel32.dll", "GetFileAttributesW",  w32_GetFileAttributesW },
    { "kernel32.dll", "GetFileAttributesExW",w32_GetFileAttributesExW },
    { "kernel32.dll", "DeleteFileW",         w32_DeleteFileW },
    { "kernel32.dll", "GetFullPathNameW",    w32_GetFullPathNameW },
    { "kernel32.dll", "GetFullPathNameA",    w32_GetFullPathNameA },
    { "kernel32.dll", "GetTempPathW",        w32_GetTempPathW },
    { "kernel32.dll", "GetTempPathA",        w32_GetTempPathA },
    { "kernel32.dll", "SetCurrentDirectoryW",w32_SetCurrentDirectoryW },
    { "kernel32.dll", "FindFirstFileW",      w32_FindFirstFileW },
    { "kernel32.dll", "GetFileType",         w32_GetFileType },
    { "kernel32.dll", "FlushFileBuffers",    w32_FlushFileBuffers },
    { "kernel32.dll", "SetEndOfFile",        w32_SetEndOfFile },
    { "kernel32.dll", "GetDiskFreeSpaceA",   w32_GetDiskFreeSpaceA },
    { "kernel32.dll", "AreFileApisANSI",     w32_AreFileApisANSI },
    /* time */
    { "kernel32.dll", "GetSystemTimeAsFileTime", w32_GetSystemTimeAsFileTime },
    { "kernel32.dll", "GetSystemTime",       w32_GetSystemTime },
    { "kernel32.dll", "SystemTimeToFileTime",w32_SystemTimeToFileTime },
    { "kernel32.dll", "GetTickCount",        w32_GetTickCount },
    { "kernel32.dll", "GetTickCount64",      w32_GetTickCount },
    { "kernel32.dll", "QueryPerformanceCounter",   w32_QueryPerformanceCounter },
    { "kernel32.dll", "QueryPerformanceFrequency", w32_QueryPerformanceFrequency },
    { "kernel32.dll", "GetSystemInfo",       w32_GetSystemInfo },
    /* file locking (single-process -> succeed) + mmap (fail -> normal I/O) */
    { "kernel32.dll", "LockFile",            w32_LockFileStub },
    { "kernel32.dll", "LockFileEx",          w32_LockFileStub },
    { "kernel32.dll", "UnlockFile",          w32_LockFileStub },
    { "kernel32.dll", "UnlockFileEx",        w32_LockFileStub },
    { "kernel32.dll", "CreateFileMappingW",  w32_CreateFileMappingW },
    { "kernel32.dll", "MapViewOfFile",       w32_MapViewOfFile },
    { "kernel32.dll", "FlushViewOfFile",     w32_LockFileStub },
    { "kernel32.dll", "UnmapViewOfFile",     w32_LockFileStub },
    /* console (report not-a-console -> plain file I/O) */
    { "kernel32.dll", "GetConsoleMode",            w32_GetConsoleMode },
    { "kernel32.dll", "GetConsoleScreenBufferInfo",w32_zero },
    { "kernel32.dll", "SetConsoleMode",            w32_one },
    { "kernel32.dll", "SetConsoleCtrlHandler",     w32_one },
    { "kernel32.dll", "SetConsoleTextAttribute",   w32_one },
    { "kernel32.dll", "WriteConsoleW",             w32_WriteConsoleW },
    { "kernel32.dll", "ReadConsoleW",              w32_zero },
    /* misc */
    { "kernel32.dll", "GetCurrentProcess",   w32_GetCurrentProcess },
    { "kernel32.dll", "GetCurrentThreadId",  w32_GetCurrentThreadId },
    { "kernel32.dll", "FormatMessageA",      w32_FormatMessageA },
    { "kernel32.dll", "FormatMessageW",      w32_zero },
    { "kernel32.dll", "LocalFree",           w32_LocalFree },
    { "kernel32.dll", "CreateMutexW",        w32_CreateMutexW },
    { "kernel32.dll", "DebugBreak",          w32_zero },
    { "kernel32.dll", "OutputDebugStringA",  w32_zero },
    { "kernel32.dll", "OutputDebugStringW",  w32_zero },
    { "kernel32.dll", "GetDiskFreeSpaceW",   w32_GetDiskFreeSpaceW },
    { "kernel32.dll", "SetFileTime",         w32_SetFileTime },
    { "kernel32.dll", "WaitForSingleObjectEx", w32_WaitForSingleObject },

    /* ---- C18a: the C-runtime breadth SQLite pulls in ---- */
    /* ctype */
    { "api-ms-win-crt-string-l1-1-0.dll", "isalnum",  w32_isalnum },
    { "api-ms-win-crt-string-l1-1-0.dll", "isalpha",  w32_isalpha },
    { "api-ms-win-crt-string-l1-1-0.dll", "isdigit",  w32_isdigit },
    { "api-ms-win-crt-string-l1-1-0.dll", "isspace",  w32_isspace },
    { "api-ms-win-crt-string-l1-1-0.dll", "isprint",  w32_isprint },
    { "api-ms-win-crt-string-l1-1-0.dll", "tolower",  w32_tolower },
    { "api-ms-win-crt-string-l1-1-0.dll", "toupper",  w32_toupper },
    /* C19b: ctype gaps the real Lua stdlib + pattern matcher reach */
    { "api-ms-win-crt-string-l1-1-0.dll", "iscntrl",  w32_iscntrl },
    { "api-ms-win-crt-string-l1-1-0.dll", "isgraph",  w32_isgraph },
    { "api-ms-win-crt-string-l1-1-0.dll", "islower",  w32_islower },
    { "api-ms-win-crt-string-l1-1-0.dll", "isupper",  w32_isupper },
    { "api-ms-win-crt-string-l1-1-0.dll", "ispunct",  w32_ispunct },
    { "api-ms-win-crt-string-l1-1-0.dll", "isxdigit", w32_isxdigit },
    /* string */
    { "api-ms-win-crt-string-l1-1-0.dll", "strcmp",   w32_strcmp },
    { "api-ms-win-crt-string-l1-1-0.dll", "strcpy",   w32_strcpy },
    { "api-ms-win-crt-string-l1-1-0.dll", "strcat",   w32_strcat },
    { "api-ms-win-crt-string-l1-1-0.dll", "strchr",   w32_strchr },
    { "api-ms-win-crt-string-l1-1-0.dll", "strrchr",  w32_strrchr },
    { "api-ms-win-crt-string-l1-1-0.dll", "strspn",   w32_strspn },
    { "api-ms-win-crt-string-l1-1-0.dll", "strcspn",  w32_strcspn },
    { "api-ms-win-crt-string-l1-1-0.dll", "strpbrk",  w32_strpbrk },
    { "api-ms-win-crt-string-l1-1-0.dll", "strcoll",  w32_strcmp },  /* C-locale == strcmp */
    { "api-ms-win-crt-string-l1-1-0.dll", "_strdup",  w32_strdup },
    { "api-ms-win-crt-private-l1-1-0.dll", "memchr",  w32_memchr },
    { "api-ms-win-crt-private-l1-1-0.dll", "memmove", w32_memmove },
    { "api-ms-win-crt-private-l1-1-0.dll", "strchr",  w32_strchr },
    { "api-ms-win-crt-private-l1-1-0.dll", "strrchr", w32_strrchr },
    /* conversion */
    { "api-ms-win-crt-convert-l1-1-0.dll", "atoi",    w32_atoi },
    { "api-ms-win-crt-convert-l1-1-0.dll", "strtol",  w32_strtol },
    { "api-ms-win-crt-convert-l1-1-0.dll", "strtoul", w32_strtoul },
    { "api-ms-win-crt-convert-l1-1-0.dll", "strtod",  w32_strtod },
    { "api-ms-win-crt-convert-l1-1-0.dll", "atof",    w32_atof },
    /* stdio FILE* */
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fopen",     w32_fopen },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fclose",    w32_fclose },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fread",     w32_fread },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fwrite",    w32_fwrite },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fputs",     w32_fputs },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fgetc",     w32_fgetc },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fgets",     w32_fgets },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "fseek",     w32_fseek },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "ftell",     w32_ftell },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "rewind",    w32_rewind },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "_fileno",   w32_fileno },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "_get_osfhandle", w32_get_osfhandle },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "_isatty",   w32_isatty },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "_setmode",  w32_setmode },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "__stdio_common_vsscanf", w32_vsscanf },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "__stdio_common_vfscanf", w32_vfscanf },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "_popen",    w32_zero },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "_pclose",   w32_zero },
    /* C19b: real-Lua stdio gaps */
    { "api-ms-win-crt-stdio-l1-1-0.dll", "getc",      w32_fgetc },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "ungetc",    w32_ungetc },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "feof",      w32_feof },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "ferror",    w32_ferror },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "clearerr",  w32_clearerr },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "freopen",   w32_freopen },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "tmpfile",   w32_zero },
    { "api-ms-win-crt-stdio-l1-1-0.dll", "tmpnam",    w32_zero },
    /* environment / process */
    { "api-ms-win-crt-environment-l1-1-0.dll", "getenv", w32_getenv },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_getpid",  w32_getpid },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "system",   w32_system },
    { "api-ms-win-crt-runtime-l1-1-0.dll", "_assert",  w32_assert },
    /* file / path */
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_access",        w32_access },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_chmod",         w32_chmod },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_mkdir",         w32_crt_mkdir },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_unlink",        w32_crt_unlink },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "remove",         w32_crt_remove },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "rename",         w32_crt_rename },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_wunlink",       w32_crt_wunlink },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_fullpath",      w32_fullpath },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_stat64i32",     w32_stat64i32 },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_fstat64",       w32_fstat64 },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_findfirst64i32",w32_findfirst_crt },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_findnext64i32", w32_findfirst_crt },
    { "api-ms-win-crt-filesystem-l1-1-0.dll", "_findclose",     w32_findclose_crt },
    /* time */
    { "api-ms-win-crt-time-l1-1-0.dll", "_localtime64", w32_localtime64 },
    { "api-ms-win-crt-time-l1-1-0.dll", "_time64",      w32_time64 },
    { "api-ms-win-crt-time-l1-1-0.dll", "clock",        w32_clock },
    /* C19c: minimal COM / ole32 (tightly scoped -- one coclass/interface). */
    { "ole32.dll", "CoInitializeEx",   w32_CoInitializeEx },
    { "ole32.dll", "CoInitialize",     w32_CoInitialize },
    { "ole32.dll", "CoUninitialize",   w32_CoUninitialize },
    { "ole32.dll", "CoCreateInstance", w32_CoCreateInstance },
    { "ole32.dll", "CoTaskMemAlloc",   w32_CoTaskMemAlloc },
    { "ole32.dll", "CoTaskMemFree",    w32_CoTaskMemFree },
    /* Internal vtable-method shims: never imported by name -- the kernel hands
     * back their procaddr-thunk VAs as the COM object's vtable slots, so they're
     * reached only through obj->lpVtbl->Method(this,...). */
    { "ole32.dll", "__toby_com_QueryInterface", w32_com_QueryInterface },
    { "ole32.dll", "__toby_com_AddRef",         w32_com_AddRef },
    { "ole32.dll", "__toby_com_Release",        w32_com_Release },
    { "ole32.dll", "__toby_com_Add",            w32_com_Add },
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

/* True if `dll` is a ucrt api-set stub ("api-ms-win-crt-..."), case-insensitive. */
static bool win32_is_apiset_crt(const char *dll) {
    const char *pfx = "api-ms-win-crt-";
    for (int i = 0; pfx[i]; i++) {
        char c = dll[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != pfx[i]) return false;
    }
    return true;
}

int win32_shim_index(const char *dll, const char *func) {
    if (!dll || !func) return -1;
    for (int i = 0; i < WIN32_SHIM_COUNT; i++) {
        if (win32_dll_eq(dll, g_win32_shims[i].dll) &&
            strcmp(func, g_win32_shims[i].func) == 0)
            return i;
    }
    /* C23: ucrt forwards the same CRT function through several api-ms-win-crt-*
     * api-set stub DLLs (e.g. wcschr imports from -private-l1 in one build,
     * -string-l1 in another). If an api-ms-win-crt-* import didn't match by
     * exact DLL, fall back to a name-only match against any -crt-* shim. */
    if (win32_is_apiset_crt(dll)) {
        for (int i = 0; i < WIN32_SHIM_COUNT; i++)
            if (win32_is_apiset_crt(g_win32_shims[i].dll) &&
                strcmp(func, g_win32_shims[i].func) == 0)
                return i;
    }
    return -1;
}

int win32_shim_count(void) { return WIN32_SHIM_COUNT; }

/* C20: 1 if shim `idx` returns a `double` (so the loader binds its thunk to the
 * float-return gate). Matched by fn pointer so only the genuine libm shims route
 * through the FP gate. */
int win32_shim_is_double_ret(int idx) {
    if (idx < 0 || idx >= WIN32_SHIM_COUNT) return 0;
    win32_shim_fn f = g_win32_shims[idx].fn;
    return f == w32_sqrt || f == w32_sin   || f == w32_cos   || f == w32_tan  ||
           f == w32_exp  || f == w32_exp2  || f == w32_log   || f == w32_log10 ||
           f == w32_log2 || f == w32_floor || f == w32_ceil  || f == w32_trunc ||
           f == w32_round|| f == w32_fabs  || f == w32_atan  || f == w32_asin  ||
           f == w32_acos || f == w32_sinh  || f == w32_cosh  || f == w32_tanh  ||
           f == w32_cbrt || f == w32_pow   || f == w32_fmod  || f == w32_atan2 ||
           f == w32_hypot|| f == w32_ldexp || f == w32_frexp ||
           f == w32_strtod || f == w32_atof;   /* C22: string->double also returns in xmm0 */
}

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
    uint64_t args[10];   /* C11: gate marshals a0..a9 (hWndParent = a8) */
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
    if (g_win32_gui.tgid != tgid) return -1;
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++)
        if (g_win32_gui.win[i].fd > 0) return g_win32_gui.win[i].fd;   /* first live window */
    return -1;
}
uint32_t win32_gui_fill_color(void) {
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++)
        if (g_win32_gui.win[i].fd > 0) return g_win32_gui.win[i].fill_color;
    return WIN32_FILL_DEFAULT;
}
/* C10: number of live windows owned by `tgid`, and a specific window's fill. */
int win32_gui_window_count(int tgid) {
    if (g_win32_gui.tgid != tgid) return 0;
    return win32_win_count();
}
uint32_t win32_gui_fill_color_fd(int fd) {
    struct win32_win *w = win32_win_find(fd);
    return w ? w->fill_color : 0;
}

/* C18c proof: analyze rendered text in a window region so the harness can
 * machine-verify the new font faces (bold genuinely renders heavier ink; italic
 * genuinely slants). Reads the window's client backbuffer directly (works from
 * the boot harness's context, where the app's fd isn't in our fd table) and
 * returns the count of "ink" pixels (those differing from `bg` past a luma
 * threshold). *top_cx/*bot_cx receive the x-centroid of ink in the region's top
 * vs bottom third (-1 if none) -- an italic glyph leans forward, so its top
 * centroid sits to the RIGHT of its bottom centroid. */
int win32_gui_ink_stats(int fd, int rx, int ry, int rw, int rh, uint32_t bg,
                        int *top_cx, int *bot_cx) {
    if (top_cx) *top_cx = -1;
    if (bot_cx) *bot_cx = -1;
    struct win32_win *w = win32_win_find(fd);
    if (!w || !w->gwin || rw <= 0 || rh <= 0) return 0;
    static uint32_t row[1024];
    if (rw > 1024) rw = 1024;
    uint32_t b = bg & 0xFFFFFFu;
    int br = (int)((b >> 16) & 0xFF), bg8 = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int third = rh / 3; if (third < 1) third = 1;
    long count = 0, top_n = 0, bot_n = 0; long long top_xs = 0, bot_xs = 0;
    for (int yy = 0; yy < rh; yy++) {
        int got = gui_window_getpixels(w->gwin, rx, ry + yy, rw, 1, row);
        if (got < rw) continue;                 /* clipped/short row -> skip */
        for (int xx = 0; xx < rw; xx++) {
            uint32_t px = row[xx] & 0xFFFFFFu;
            int dr = (int)((px >> 16) & 0xFF) - br;
            int dg = (int)((px >> 8) & 0xFF) - bg8;
            int db = (int)(px & 0xFF) - bb;
            int dist = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
            if (dist > 60) {                     /* ink */
                count++;
                if (yy < third)            { top_xs += xx; top_n++; }
                else if (yy >= rh - third) { bot_xs += xx; bot_n++; }
            }
        }
    }
    if (top_cx && top_n) *top_cx = (int)(top_xs / top_n);
    if (bot_cx && bot_n) *bot_cx = (int)(bot_xs / bot_n);
    return (int)count;
}

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

    /* ---- Lock-free time-read fast path (slice 41; WHPX clock_gettime storm).
     * Serve a handful of pure time-read syscalls WITHOUT the BKL when their
     * result buffer is already resident+writable. Under WHPX chrome's message
     * pumps issue clock_gettime/gettimeofday tens of thousands of times a
     * second; taking the BKL for each makes it the system-wide throughput cap
     * (see [hb-x]) and starves the compositor/network threads that must run to
     * produce a frame. linux_fast_time_syscall() returns 0 (falls through to
     * the normal BKL path) for anything it can't fully serve here -- an unknown
     * clock id, or a non-resident buffer that needs the fault/allocate path. We
     * only take it when no signal is pending, so the syscall-return signal-
     * delivery step below has nothing to do. */
    {
        struct proc *ftp = current_proc();
        if (ftp && ftp->personality == ABI_PERS_LINUX && !ftp->pending_signals) {
            long frv;
            if (linux_fast_time_syscall(num, a1, a2, &frv)) {
                { extern volatile uint64_t g_lx_fast_time; g_lx_fast_time++; }
                ftp->syscall_count++;
                perf_inc_total_syscalls();
                wdog_kick_proc(ftp->pid);
                cli();      /* match the normal return: mask before the .S unwind */
                return frv;
            }
            /* Slice 64 (tier 2): BKL-free futex fast paths -- WAIT with a
             * stale expected value (-EAGAIN) and WAKE with no waiters (0)
             * are chrome's dominant futex cases and touch only
             * g_futex_lock-protected state; see futex_fast() in thread.c.
             * Falls through to the BKL path for anything that parks/wakes. */
            if (num == LX_futex) {
                extern int futex_fast(uint32_t *, int, uint32_t, long *);
                extern volatile uint64_t g_lx_fast_futex;
                if (futex_fast((uint32_t *)(uintptr_t)a1, (int)a2,
                               (uint32_t)a3, &frv)) {
                    g_lx_fast_futex++;
                    ftp->syscall_count++;
                    perf_inc_total_syscalls();
                    wdog_kick_proc(ftp->pid);
                    cli();
                    return frv;
                }
            }
            /* Slice 64 (tier 2): sched_yield is chrome's #1 syscall by 100x
             * (401k/60s measured) and every one took the BKL only to run a
             * bkl_exit/bkl_enter round trip. A yield with an empty local
             * queue is a no-op; serve it lock-free. See sched_yield_fast(). */
            if (num == LX_sched_yield) {
                extern int sched_yield_fast(void);
                extern volatile uint64_t g_lx_fast_yield;
                if (sched_yield_fast()) {
                    g_lx_fast_yield++;
                    ftp->syscall_count++;
                    perf_inc_total_syscalls();
                    wdog_kick_proc(ftp->pid);
                    cli();
                    return 0;
                }
            }
        }
    }

    /* Big Kernel Lock: serialize the whole syscall body across CPUs so user
     * code parallelizes while the kernel's coarse-locked subsystems stay safe.
     * The scheduler drops/reacquires the BKL around any blocking switch inside
     * the body, and proc_exit's switch releases it for a dying proc. Held with
     * IRQs on (we just sti'd); IRQ handlers don't take the BKL. */
    bkl_enter();
    /* Slice 64c: stamp the hold bucket for this CPU (see bkl_hold_account).
     * Linux syscalls 0..511, native 512+n; cleared to -1 by the return path
     * so post-syscall kernel work doesn't land on the last syscall. */
    {
        struct proc *hp = current_proc();
        uint32_t hci = smp_current_cpu_idx();
        extern int g_bkl_sysno[];
        if (hci < MAX_CPUS)
            g_bkl_sysno[hci] =
                (hp && hp->personality == ABI_PERS_LINUX)
                    ? ((num >= 0 && num < 512) ? (int)num : -1)
                    : ((num >= 0 && num < 256) ? 512 + (int)num : -1);
    }

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
    else {
        /* Slice 64c: stamp the NATIVE syscall so BKL hold time attributes
         * to it (chromewin's GUI/blit calls live here). */
        if (caller) caller->cursys_nat = (int32_t)num;
        rv = do_syscall(num, a1, a2, a3, a4, a5);
        if (caller) caller->cursys_nat = -1;
    }

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

    /* Slice 88: CoW-fork STW -- a sibling must not return to user while
     * vmm_cow_fork is stripping PTE_WRITABLE. Park until tg_vm_resume. */
    {
        struct proc *qp = current_proc();
        while (qp && __atomic_load_n(&qp->vm_quiesce, __ATOMIC_ACQUIRE)) {
            qp->state = PROC_BLOCKED;
            qp->vm_quiesced = 1;
            bkl_exit();
            sched_yield();
            bkl_enter();
            qp = current_proc();
        }
    }

    /* Leaving the kernel: drop the BKL so another core can enter. (If the
     * body proc_exit'd or a signal killed us, the scheduler already released
     * it and we never got here; bkl_exit is idempotent regardless.) */
    bkl_exit();
    {   /* slice 64c: this CPU is no longer inside a syscall */
        uint32_t hci = smp_current_cpu_idx();
        extern int g_bkl_sysno[];
        if (hci < MAX_CPUS) g_bkl_sysno[hci] = -1;
    }

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

    /* STAR[63:48] is the SYSRET selector BASE: the CPU forms
     *   CS = base + 16   and   SS = base + 8.
     * It forces CS.RPL = 3, but it does NOT force SS.RPL -- SS keeps whatever
     * RPL the base carries. The base must therefore already have RPL 3 in it:
     *   0x13 + 8  = 0x1B = GDT_USER_DS  (RPL 3)
     *   0x13 + 16 = 0x23 = GDT_USER_CS  (RPL 3)
     * This was 0x10, which produced a correct CS (0x23, RPL forced) but
     * SS = 0x18 -- the user data segment with RPL 0. A privilege-changing
     * iretq requires SS.RPL == CS.RPL, so after the first syscall EVERY return
     * to ring 3 was riding an illegal frame. TCG does not check this, which is
     * why plain QEMU never complained; real VT-x (WHPX) and real hardware
     * enforce it and #GP(0x18) on the iretq in common_isr. */
    uint64_t star = ((uint64_t)GDT_KERNEL_CS << 32) |
                    ((uint64_t)SYSRET_SEL_BASE << 48);
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

    /* Recomputed only for the boot log below -- keep it in step with the value
     * syscall_program_msrs() actually wrote, or the log misreports it. */
    uint64_t star = ((uint64_t)GDT_KERNEL_CS << 32) |
                    ((uint64_t)SYSRET_SEL_BASE << 48);

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
