/* panic.c -- Milestone 28B kernel panic + crash-dump handler.
 *
 * Layout:
 *
 *   1. Capture register snapshot via inline asm at panic entry. The
 *      values are sampled BEFORE we touch any C state so RSP/RBP are
 *      meaningful.
 *   2. Re-paint the framebuffer console as the "panic screen": a
 *      red-on-white banner the user sees on a real machine. We use
 *      console_clear + console_set_color to re-tint subsequent text;
 *      the existing 8x8 glyph renderer carries the message body.
 *   3. Print structured diagnostics: file:line + reason, current
 *      process info (pid/name/ppid/syscall_count), full register
 *      dump, crude stack-trace (frame-pointer walk, capped at 16
 *      frames so a corrupted frame chain can't loop forever), and
 *      a tail-of-slog dump for surrounding context.
 *   4. Best-effort write a binary+text crash dump to /data/crash/last.
 *      The dump file format is `struct abi_crash_header` followed by
 *      the formatted text body. We never touch VFS if /data was not
 *      mounted (panic + no disk = nothing we can usefully do).
 *   5. Halt forever.
 *
 * Re-entrancy: a second panic during the panic body just halts. We
 * also expose `kpanic_in_progress()` so IRQ handlers can short-circuit
 * (otherwise a timer firing during the panic banner could try to
 * re-enter slog or service_tick).
 */

#include <tobyos/panic.h>
#include <tobyos/printk.h>
#include <tobyos/console.h>
#include <tobyos/cpu.h>
#include <tobyos/proc.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>
#include <tobyos/abi/abi.h>
#include <tobyos/vfs.h>
#include <tobyos/pit.h>

static bool s_in_panic = false;
static const char *s_boot_trigger = NULL;

bool kpanic_in_progress(void) { return s_in_panic; }

bool kpanic_boot_request_pending(void)         { return s_boot_trigger != NULL; }
const char *kpanic_boot_request_trigger(void)  { return s_boot_trigger; }
void kpanic_boot_request(const char *trigger)  { s_boot_trigger = trigger; }

/* Captured register snapshot; struct lives outside the function so the
 * "outside the panic body" formatter can still reach it after the
 * panic banner has been printed. */
struct panic_regs {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cr0, cr2, cr3, cr4;
};

static void capture_regs(struct panic_regs *r) {
    /* GP regs first -- one volatile ASM block per slot keeps the
     * compiler from reordering and stomping the values we just
     * read. We don't really need RIP (this very function is the
     * RIP at the moment); we use the return-address trick for that. */
    __asm__ volatile ("mov %%rax, %0" : "=m"(r->rax));
    __asm__ volatile ("mov %%rbx, %0" : "=m"(r->rbx));
    __asm__ volatile ("mov %%rcx, %0" : "=m"(r->rcx));
    __asm__ volatile ("mov %%rdx, %0" : "=m"(r->rdx));
    __asm__ volatile ("mov %%rsi, %0" : "=m"(r->rsi));
    __asm__ volatile ("mov %%rdi, %0" : "=m"(r->rdi));
    __asm__ volatile ("mov %%rbp, %0" : "=m"(r->rbp));
    __asm__ volatile ("mov %%rsp, %0" : "=m"(r->rsp));
    __asm__ volatile ("mov %%r8 , %0" : "=m"(r->r8 ));
    __asm__ volatile ("mov %%r9 , %0" : "=m"(r->r9 ));
    __asm__ volatile ("mov %%r10, %0" : "=m"(r->r10));
    __asm__ volatile ("mov %%r11, %0" : "=m"(r->r11));
    __asm__ volatile ("mov %%r12, %0" : "=m"(r->r12));
    __asm__ volatile ("mov %%r13, %0" : "=m"(r->r13));
    __asm__ volatile ("mov %%r14, %0" : "=m"(r->r14));
    __asm__ volatile ("mov %%r15, %0" : "=m"(r->r15));
    /* Approximate RIP with the return address of capture_regs. */
    r->rip = (uint64_t)__builtin_return_address(0);
    r->rflags = read_rflags();
    r->cr0 = read_cr0();
    r->cr2 = read_cr2();
    r->cr3 = read_cr3();
    r->cr4 = read_cr4();
}

/* Walk the kernel stack via frame pointers. Most of our kernel code
 * is built with `-O2` which often elides frame pointers; the trace
 * is best-effort. We cap at MAX_FRAMES to bound an infinite loop on
 * a corrupted chain. */
#define MAX_FRAMES 16

static void print_stack_trace(uint64_t rbp_start) {
    kprintf("  Stack trace (frame-walk, cap=%d):\n", MAX_FRAMES);
    uint64_t *fp = (uint64_t *)rbp_start;
    int frames = 0;
    /* The kernel half lives at >= 0xFFFF_8000_0000_0000; reject any
     * obviously bogus pointer (NULL, low userland, unaligned). */
    while (fp && frames < MAX_FRAMES &&
           ((uintptr_t)fp & 7) == 0 &&
           (uintptr_t)fp >= 0xFFFF800000000000ULL) {
        uint64_t ret = fp[1];
        kprintf("    [%2d] rbp=%p ret=%p\n",
                frames, (void *)fp, (void *)ret);
        uint64_t next = fp[0];
        if (next <= (uint64_t)fp) break;     /* not advancing */
        fp = (uint64_t *)next;
        frames++;
    }
    if (frames == 0) {
        kprintf("    (no usable frame pointers -- "
                "kernel built without -fno-omit-frame-pointer)\n");
    }
}

static void print_proc_info(void) {
    struct proc *p = current_proc();
    if (p) {
        kprintf("  Current process: pid=%d name='%s' ppid=%d syscalls=%llu\n",
                (int)p->pid,
                p->name[0] ? p->name : "(unnamed)",
                (int)p->ppid,
                (unsigned long long)p->syscall_count);
    } else {
        kprintf("  Current process: (kernel context, no proc)\n");
    }
}

static void print_regs(const struct panic_regs *r) {
    kprintf("  Registers:\n");
    kprintf("    rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n",
            (unsigned long long)r->rax, (unsigned long long)r->rbx,
            (unsigned long long)r->rcx, (unsigned long long)r->rdx);
    kprintf("    rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            (unsigned long long)r->rsi, (unsigned long long)r->rdi,
            (unsigned long long)r->rbp, (unsigned long long)r->rsp);
    kprintf("    r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n",
            (unsigned long long)r->r8 , (unsigned long long)r->r9 ,
            (unsigned long long)r->r10, (unsigned long long)r->r11);
    kprintf("    r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
            (unsigned long long)r->r12, (unsigned long long)r->r13,
            (unsigned long long)r->r14, (unsigned long long)r->r15);
    kprintf("    rip=%016llx rflags=%016llx\n",
            (unsigned long long)r->rip, (unsigned long long)r->rflags);
    kprintf("    cr0=%016llx cr2=%016llx cr3=%016llx cr4=%016llx\n",
            (unsigned long long)r->cr0, (unsigned long long)r->cr2,
            (unsigned long long)r->cr3, (unsigned long long)r->cr4);
}

/* Paint the framebuffer console as the "panic screen". We don't have
 * a fill_rect on the console (yet), so we use the existing API: clear
 * the screen, set foreground to bright white, then print a banner.
 * The actual "red" comes through later via console_set_color. The
 * effect on a real machine is: cleared screen + red-tinted text + a
 * giant "KERNEL PANIC" banner. Good enough as a "the box is dead"
 * indicator a person standing nearby will recognise. */
static void paint_panic_screen(void) {
    if (!console_ready()) return;
    /* Clear so we get a visually-distinct full-screen banner. */
    console_clear();
    console_set_color(0x00FFFFFFu);  /* bright white */
    /* Big banner. Eight rows of '#' for emphasis. */
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 80; col++) console_putc('#');
        console_putc('\n');
    }
    console_putc('\n');
    console_set_color(0x00FF6060u);  /* hot red, used by the body */
    /* Centred-ish title. */
    const char *title = "                 KERNEL PANIC -- tobyOS halted";
    for (const char *p = title; *p; p++) console_putc(*p);
    console_putc('\n');
    console_putc('\n');
}

/* Build the formatted body of the crash dump. Returns the byte count
 * actually written (excluding NUL).
 *
 * NOTE: ksnprintf supports `%lx` (unsigned long) but not `%llx`. On
 * x86_64 unsigned long is already 64-bit so plain `%016lx` with an
 * `(unsigned long)` cast gives us the same hex output kprintf would. */
static size_t build_dump_text(char *buf, size_t cap,
                              const char *file, int line,
                              const char *reason,
                              const struct panic_regs *r) {
    size_t off = 0;
    int n;
    n = ksnprintf(buf + off, cap - off,
                  "tobyOS crash dump\n"
                  "  panic at: %s:%d\n"
                  "  reason  : %s\n",
                  file ? file : "?", line, reason ? reason : "(none)");
    if (n > 0) off += (size_t)n;
    {
        struct proc *p = current_proc();
        n = ksnprintf(buf + off, cap - off,
                      "  pid     : %d (%s)\n",
                      p ? (int)p->pid : -1,
                      p ? p->name : "(kernel)");
        if (n > 0) off += (size_t)n;
    }
    n = ksnprintf(buf + off, cap - off,
                  "  regs:\n"
                  "  rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n"
                  "  rsi=%016lx rdi=%016lx rbp=%016lx rsp=%016lx\n"
                  "  r8 =%016lx r9 =%016lx r10=%016lx r11=%016lx\n"
                  "  r12=%016lx r13=%016lx r14=%016lx r15=%016lx\n"
                  "  rip=%016lx rfl=%016lx\n"
                  "  cr0=%016lx cr2=%016lx cr3=%016lx cr4=%016lx\n",
                  (unsigned long)r->rax, (unsigned long)r->rbx,
                  (unsigned long)r->rcx, (unsigned long)r->rdx,
                  (unsigned long)r->rsi, (unsigned long)r->rdi,
                  (unsigned long)r->rbp, (unsigned long)r->rsp,
                  (unsigned long)r->r8 , (unsigned long)r->r9 ,
                  (unsigned long)r->r10, (unsigned long)r->r11,
                  (unsigned long)r->r12, (unsigned long)r->r13,
                  (unsigned long)r->r14, (unsigned long)r->r15,
                  (unsigned long)r->rip, (unsigned long)r->rflags,
                  (unsigned long)r->cr0, (unsigned long)r->cr2,
                  (unsigned long)r->cr3, (unsigned long)r->cr4);
    if (n > 0) off += (size_t)n;

    /* Tail of the slog ring -- copy the most-recent N records into the
     * dump body. We use a small temporary buffer of records on the
     * stack so we don't have to lock the ring for the whole format
     * loop. SLOG_DUMP_TAIL is small (16) because dump space is tight. */
#define SLOG_DUMP_TAIL 16
    if (slog_ready()) {
        n = ksnprintf(buf + off, cap - off, "  slog tail (last %d):\n",
                      SLOG_DUMP_TAIL);
        if (n > 0) off += (size_t)n;
        struct abi_slog_record tail[SLOG_DUMP_TAIL];
        uint32_t got = slog_drain(tail, SLOG_DUMP_TAIL, /*since_seq=*/0);
        /* Show only the most recent SLOG_DUMP_TAIL entries. */
        uint32_t start = 0;
        if (got > SLOG_DUMP_TAIL) start = got - SLOG_DUMP_TAIL;
        for (uint32_t i = start; i < got; i++) {
            const struct abi_slog_record *rec = &tail[i];
            const char *lvl = slog_level_name(rec->level);
            n = ksnprintf(buf + off, cap - off,
                          "    [%lu] %s %s pid=%d %s\n",
                          (unsigned long)rec->time_ms, lvl,
                          rec->sub, (int)rec->pid, rec->msg);
            if (n > 0) off += (size_t)n;
            if (off >= cap - 64) break;  /* leave room for trailer */
        }
    }
    return off;
}

#define CRASH_DUMP_DIR    "/data/crash"
#define CRASH_DUMP_LATEST CRASH_DUMP_DIR "/last.dump"
/* Crash dumps are tiny: header (~256B) + ~1.5 KB body. 4 KiB scratch
 * is plenty and lives on the static .bss so we don't depend on the
 * heap (which may itself be the reason we panicked). */
#define CRASH_DUMP_BUF_BYTES  4096
static uint8_t s_dump_buf[CRASH_DUMP_BUF_BYTES];

static int try_write_crash_dump(const char *file, int line,
                                const char *reason,
                                const struct panic_regs *r) {
    /* Build header. Body comes after. */
    struct abi_crash_header hdr = { 0 };
    hdr.magic       = 0x48535243u;   /* 'CRSH' */
    hdr.version     = 1;
    /* time_ms via PIT (cheap, no dependencies). */
    {
        uint32_t hz = pit_hz();
        hdr.time_ms = hz ? (pit_ticks() * 1000ull) / hz : 0;
    }
    hdr.boot_seq    = 1;             /* TODO: bump across reboots when /data is read-back */
    hdr.pid         = current_proc() ? (int32_t)current_proc()->pid : -1;
    {
        size_t i = 0;
        const char *src = reason ? reason : "(none)";
        while (i + 1 < sizeof(hdr.reason) && src[i]) {
            hdr.reason[i] = src[i]; i++;
        }
        hdr.reason[i] = '\0';
    }

    /* Write header into the scratch first. */
    if (sizeof(hdr) + 64 > CRASH_DUMP_BUF_BYTES) return -1;
    memcpy(s_dump_buf, &hdr, sizeof(hdr));
    char *body = (char *)s_dump_buf + sizeof(hdr);
    size_t cap = CRASH_DUMP_BUF_BYTES - sizeof(hdr);
    size_t blen = build_dump_text(body, cap, file, line, reason, r);
    /* Fix the body_bytes header field now that we know it. */
    struct abi_crash_header *hp = (struct abi_crash_header *)s_dump_buf;
    hp->body_bytes = (uint32_t)blen;

    size_t total = sizeof(hdr) + blen;

    /* Best-effort mkdir + write. Either failure is non-fatal -- we
     * just won't have an on-disk dump. */
    (void)vfs_mkdir(CRASH_DUMP_DIR);
    int rc = vfs_write_all(CRASH_DUMP_LATEST, s_dump_buf, total);
    return rc;
}

/* The user-facing entry point. */
void kpanic_at(const char *file, int line, const char *fmt, ...) {
    cli();

    if (s_in_panic) {
        /* Already panicking -- don't try to format again, just halt. */
        hlt_forever();
    }
    s_in_panic = true;

    /* Snapshot regs as early as possible. */
    struct panic_regs regs;
    capture_regs(&regs);

    /* Format the reason string into a stack buffer first so we can
     * (a) print it AND (b) ship it into the crash dump without
     * formatting twice. */
    char reason[ABI_CRASH_REASON_MAX];
    {
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(reason, sizeof(reason), fmt ? fmt : "(none)", ap);
        va_end(ap);
    }

    /* Repaint the framebuffer with the panic screen. */
    paint_panic_screen();

    /* Banner. */
    kprintf("\n");
    kprintf("==============================================================\n");
    kprintf("  KERNEL PANIC at %s:%d\n", file, line);
    kprintf("  reason: %s\n", reason);
    kprintf("==============================================================\n");

    /* Process info. */
    print_proc_info();

    /* Register dump. */
    print_regs(&regs);

    /* Stack trace. */
    print_stack_trace(regs.rbp);

    /* Tail of the slog ring -- valuable surrounding context. */
    if (slog_ready()) {
        kprintf("  Recent slog records:\n");
        slog_dump_kprintf();
    }

    /* Best-effort dump. */
    int dr = try_write_crash_dump(file, line, reason, &regs);
    if (dr == 0) {
        kprintf("  Crash dump saved to %s (%u bytes)\n",
                CRASH_DUMP_LATEST,
                (unsigned)(sizeof(struct abi_crash_header) +
                           ((struct abi_crash_header *)s_dump_buf)->body_bytes));
    } else {
        kprintf("  Crash dump skipped: vfs_write rc=%d "
                "(no /data, or fs read-only)\n", dr);
    }

    kprintf("==============================================================\n");
    kprintf("  System halted.\n");
    kprintf("==============================================================\n");

    hlt_forever();
}

/* Self-test entry. Triggered by the boot test or the userland
 * `crashtest` tool to validate the panic plumbing without needing a
 * real kernel bug. */
void kpanic_self_test(const char *trigger) {
    if (!trigger) trigger = "kpanic";
    if (!strcmp(trigger, "deref")) {
        /* NULL deref via a runtime indirection so the optimizer
         * doesn't fold the trap away. */
        volatile uint64_t *bad = (volatile uint64_t *)(uintptr_t)0;
        kprintf("[crashtest] deref triggering NULL read...\n");
        uint64_t v = *bad;
        (void)v;
        kpanic("crashtest deref: read returned -- IDT broken?");
    }
    if (!strcmp(trigger, "div0")) {
        volatile int a = 1, b = 0;
        kprintf("[crashtest] div0 triggering divide-by-zero...\n");
        volatile int c = a / b;
        (void)c;
        kpanic("crashtest div0: divide returned -- exception lost");
    }
    if (!strcmp(trigger, "assert")) {
        kprintf("[crashtest] assert triggering KASSERT(false)...\n");
        KASSERT(0 && "crashtest assert");
    }
    /* Default + "kpanic" */
    kprintf("[crashtest] kpanic triggering direct panic...\n");
    kpanic("crashtest: direct kpanic_self_test('%s')", trigger);
}
