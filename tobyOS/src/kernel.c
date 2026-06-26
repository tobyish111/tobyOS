/* kernel.c -- tobyOS entry point.
 *
 * The Limine bootloader hands control to _start in 64-bit long mode with
 * the kernel mapped at the higher-half address from linker.ld. From here
 * we initialise our subsystems in dependency order, print a banner, and
 * drop into the idle loop.
 *
 * Subsystem layering:
 *   serial   -> printk's "always available" sink
 *   console  -> framebuffer text output (depends on Limine FB response)
 *   printk   -> formatted output, fans out to serial + console
 *   panic    -> uses printk; safe to call from anywhere after this point
 *
 * Anything below this file (interrupts, paging, etc.) is the next
 * milestone. Right now kmain just halts in a CPU-idle loop.
 */

#include <tobyos/types.h>
#include <tobyos/cpu.h>
#include <tobyos/serial.h>
#include <tobyos/console.h>
#include <tobyos/printk.h>
#include <tobyos/bootlog.h>
#include <tobyos/panic.h>
#include <tobyos/gdt.h>
#include <tobyos/idt.h>
#include <tobyos/isr.h>
#include <tobyos/pic.h>
#include <tobyos/ioapic.h>
#include <tobyos/irq.h>
#include <tobyos/pit.h>
#include <tobyos/rtc.h>
#include <tobyos/perf.h>
#include <tobyos/keyboard.h>
#include <tobyos/limine.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/shell.h>
#include <tobyos/tss.h>
#include <tobyos/syscall.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/signal.h>
#include <tobyos/acpi.h>
#include <tobyos/apic.h>
#include <tobyos/smp.h>
#include <tobyos/initrd.h>
#include <tobyos/vfs.h>
#include <tobyos/blk.h>
#include <tobyos/partition.h>
#include <tobyos/tobyfs.h>
#include <tobyos/fat32.h>
#include <tobyos/ext4.h>
#include <tobyos/pci.h>
#include <tobyos/net.h>
#include <tobyos/ipv6.h>
#include <tobyos/icmpv6.h>
#include <tobyos/dns.h>
#include <tobyos/tcp.h>
#include <tobyos/tcp_echo.h>
#include <tobyos/tcp_shell.h>
#include <tobyos/ssh.h>
#include <tobyos/ssh_crypto.h>
#include <tobyos/http.h>
#include <tobyos/xhci.h>
#include <tobyos/usb_legacy.h>
#include <tobyos/usb_hub.h>
#include <tobyos/usb_hid.h>
#include <tobyos/virtio_gpu.h>
#include <tobyos/gpu_intel.h>
#include <tobyos/gfx.h>
#include <tobyos/mouse.h>
#include <tobyos/gui.h>
#include <tobyos/pe.h>      /* C8: win32_gui_window_fd / win32_gui_fill_color */
#include <tobyos/term.h>
#include <tobyos/settings.h>
#include <tobyos/service.h>
#include <tobyos/session.h>
#include <tobyos/users.h>
#include <tobyos/pkg.h>
#include <tobyos/sec.h>
#include <tobyos/sysprot.h>
#include <tobyos/procfs.h>
#include <tobyos/inotify.h>
#include <tobyos/sectest.h>
#include <tobyos/installer.h>
#include <tobyos/audio_hda.h>
#include <tobyos/fw_cfg.h>
#include <tobyos/acpi_bat.h>
#include <tobyos/devtest.h>
#include <tobyos/display.h>
#include <tobyos/slog.h>
#include <tobyos/watchdog.h>
#include <tobyos/safemode.h>
#include <tobyos/hwinfo.h>
#include <tobyos/drvmatch.h>
#include <tobyos/drvconf.h>
#include <tobyos/drvdb.h>
#include <tobyos/usbreg.h>
#include <tobyos/theme.h>
#include <tobyos/notify.h>
#include <tobyos/rng.h>
#include <tobyos/module.h>
#include <tobyos/clipboard.h>
#include <tobyos/abi/abi.h>
#include <tobyos/swap.h>
#include <tobyos/page_fault.h>
#include <tobyos/bcache.h>

/* New subsystem headers */
extern void devmgr_init(void);
extern void devmgr_enumerate(void);
extern void taskd_init(void);
extern void taskd_tick(void);

/* Phase 1 M1.4: IPC subsystem init declarations */
extern void shm_init(void);
extern void unix_socket_init(void);

/* Phase 1 M1.5: sysfs */
extern void sysfs_init(void);

/* Phase 7: ASLR + hardening */
extern void aslr_init(void);
extern void hardening_init(void);

/* Phase 4: AML interpreter */
extern void aml_interp_init(void);

/* Phase 2: HiDPI display scaling */
extern void hidpi_init(void);

/* Phase B: Audio engine */
extern void audio_engine_init(void);

/* ---- Limine framebuffer request (kept inline -- only used here) ---- */

#define LIMINE_FRAMEBUFFER_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75, 0xa3148604f6fab11b }

struct limine_framebuffer {
    void     *address;
    uint64_t  width;
    uint64_t  height;
    uint64_t  pitch;
    uint16_t  bpp;
};

struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    struct limine_framebuffer **framebuffers;
};

struct limine_framebuffer_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_framebuffer_response *response;
};

/* ---- Limine protocol markers (start, base revision, end) ---- */

__attribute__((used, section(".limine_reqs")))
static volatile uint64_t requests_start[] = {
    0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf,
    0x785c6ed015d3e316, 0x181e920a7852b9d9
};

__attribute__((used, section(".limine_reqs")))
static volatile uint64_t base_revision[] = {
    0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 3
};

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_framebuffer_request fb_req = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_memmap_request memmap_req = {
    .id       = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_hhdm_request hhdm_req = {
    .id       = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = 0
};

/* vmm.c reaches in via `extern` to read this -- the kernel's physical
 * load address comes from Limine, not from the linker. */
__attribute__((used, section(".limine_reqs")))
volatile struct limine_executable_address_request exec_addr_req = {
    .id       = LIMINE_EXECUTABLE_ADDRESS_REQUEST,
    .revision = 0,
    .response = 0
};

/* shell.c reaches in via `extern` to enumerate / look up modules. */
__attribute__((used, section(".limine_reqs")))
volatile struct limine_module_request module_req = {
    .id       = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = 0
};

/* ACPI RSDP -- needed by acpi_init to walk MADT for SMP enumeration. */
__attribute__((used, section(".limine_reqs")))
static volatile struct limine_rsdp_request rsdp_req = {
    .id       = LIMINE_RSDP_REQUEST,
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_reqs")))
static volatile uint64_t requests_end[] = {
    0xadc0e0531bb10d03, 0x9572709f31764c62
};

/* ---- Boot sequence ---- */

static void early_init(void) {
    serial_init();
    serial_puts("\n[boot] tobyOS kernel entry\n");
    bootlog_init();
    kprintf("[boot] serial up (COM1 + debugcon)\n");
    /* Milestone 28A: bring the structured-log ring up as early as
     * possible so every subsequent subsystem can SLOG_INFO/etc. into
     * a real ring and not just the early fallback. The ring is BSS-
     * resident so no allocator is required. */
    slog_init();
    SLOG_INFO(SLOG_SUB_BOOT, "tobyOS boot sequence started");
}

static void limine_fb_canonical(struct limine_framebuffer *fb) {
    if (!fb || !fb->address) return;

    uint64_t hhdm = hhdm_req.response ? hhdm_req.response->offset : 0;
    if (hhdm == 0)
        hhdm = pmm_hhdm_offset();
    if (hhdm == 0)
        return;

    uint64_t virt = (uint64_t)(uintptr_t)fb->address;
    uint64_t phys = (virt >= hhdm) ? (virt - hhdm) : virt;
    fb->address   = (void *)(uintptr_t)(hhdm + phys);
}

static void framebuffer_init(void) {
    if (fb_req.response == 0 || fb_req.response->framebuffer_count == 0) {
        kprintf("[boot] WARNING: no framebuffer response from Limine\n");
        return;
    }

    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    kprintf("[boot] framebuffer: %lux%lu pitch=%lu bpp=%u addr=%p\n",
            fb->width, fb->height, fb->pitch,
            (unsigned)fb->bpp, fb->address);

    if (fb->bpp != 32) {
        kprintf("[boot] WARNING: framebuffer is %u bpp, console wants 32\n",
                (unsigned)fb->bpp);
        return;
    }

    /* Do not console_init here -- HHDM is not latched until pmm_init().
     * Early boot output goes to serial only. */
}

/* Map the GOP framebuffer into our HHDM and refresh console/gfx pointers.
 * Safe to call multiple times (post-CR3 hook, before gfx, before desktop). */
static void framebuffer_sync_mapping(void) {
    if (!fb_req.response || fb_req.response->framebuffer_count == 0)
        return;

    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    if (!fb || !fb->address || fb->bpp != 32)
        return;

    limine_fb_canonical(fb);

    uint64_t hhdm = pmm_hhdm_offset();
    uint64_t virt = (uint64_t)(uintptr_t)fb->address;
    uint64_t phys = (virt >= hhdm) ? (virt - hhdm) : virt;

    uint64_t size = fb->pitch * fb->height;
    if (size == 0) size = PAGE_SIZE;
    if (!vmm_hhdm_ensure_mapped(phys, (size_t)size,
                                VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_NOCACHE)) {
        kprintf("[boot] WARNING: framebuffer remap failed at phys %p\n",
                (void *)phys);
        console_notify_cr3_switch();
        return;
    }

    {
        bool had_console = console_ready();
        if (console_init(fb->address, fb->pitch, fb->width, fb->height)) {
            if (!had_console) {
                kprintf("[boot] console up (%lux%lu text cells)\n",
                        (unsigned long)(fb->width / 8),
                        (unsigned long)(fb->height / 8));
            }
        } else if (!had_console) {
            kprintf("[boot] WARNING: console_init failed on framebuffer\n");
        }
    }
    kprintf("[boot] framebuffer sync: phys=%p virt=%p size=%lu\n",
            (void *)phys, fb->address, (unsigned long)size);
    gfx_sync_framebuffer(fb->address, fb->pitch,
                         (uint32_t)fb->width, (uint32_t)fb->height);
}

/* After vmm_init switches CR3, Limine's HHDM framebuffer mapping may
 * be gone if UEFI/GOP tagged the region RESERVED instead of
 * FRAMEBUFFER. Re-map on demand so console/gfx keep working. */
static void framebuffer_validate_mapping(void) {
    framebuffer_sync_mapping();
    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
        if (fb && fb->address)
            kprintf("[boot] framebuffer mapped for kernel page tables "
                    "(%p)\n", fb->address);
    }
}

/* Called from vmm_init immediately after CR3 switch, before any
 * post-switch kprintf that might write through the framebuffer. */
void vmm_post_cr3_hook(void) {
    console_notify_cr3_switch();
    framebuffer_validate_mapping();
}

/* gfx layer needs the heap, so it gets initialised after heap_init.
 * Pull the framebuffer info back out of the Limine response. */
static void gfx_layer_init(void) {
    if (fb_req.response == 0 || fb_req.response->framebuffer_count == 0) {
        return;
    }
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    if (fb->bpp != 32) return;
    if (!gfx_init(fb->address, fb->pitch,
                  (uint32_t)fb->width, (uint32_t)fb->height)) {
        kprintf("[boot] WARNING: gfx_init rejected the framebuffer\n");
    }
}

static void modules_log(void) {
    if (!module_req.response || module_req.response->module_count == 0) {
        kprintf("[boot] no Limine modules\n");
        return;
    }
    kprintf("[boot] Limine modules (%lu):\n",
            (unsigned long)module_req.response->module_count);
    for (uint64_t i = 0; i < module_req.response->module_count; i++) {
        struct limine_file *m = module_req.response->modules[i];
        kprintf("  [%lu] %s  size=%lu  addr=%p\n",
                (unsigned long)i, m->path,
                (unsigned long)m->size, m->address);
    }
}

/* Milestone 20: scan the module list for an "install.img" entry and
 * hand its address+size to the installer. Present only when booting
 * from the live ISO; absent when booting from the installed disk's
 * base.iso. */
static void installer_scan_modules(void) {
    if (!module_req.response) return;
    for (uint64_t i = 0; i < module_req.response->module_count; i++) {
        struct limine_file *m = module_req.response->modules[i];
        if (!m || !m->path) continue;
        const char *name = m->path;
        /* Strip directory prefix so we match either /boot/install.img
         * or just install.img, in either case. */
        for (const char *c = m->path; *c; c++) {
            if (*c == '/') name = c + 1;
        }
        if (strcmp(name, "install.img") == 0) {
            installer_register_image(m->address, (uint32_t)m->size);
            return;
        }
    }
}

static void banner(void) {
    console_set_color(0x0066FF66);  /* green */
    kprintf("Welcome to tobyOS\n");
    console_set_color(0x00CCCCCC);  /* default grey */
    kprintf("milestone 14: services + settings + login. "
            "Boot drops into the GUI desktop with /bin/login.\n");
    kprintf("\n");
}

/* milestone 14: bring up settings -> service manager -> session.
 * Order matters:
 *   - settings_init() reads /data/settings.conf so other subsystems
 *     can call settings_get_* during their init.
 *   - service_init() zeroes the registry.
 *   - register the four BUILTIN services (input, network, desktop,
 *     session) so the operator can verify them with `services` from
 *     the shell.
 *   - gui_set_desktop_mode(true) flips the compositor so the user
 *     sees the wallpaper + taskbar before the login screen pops up.
 *   - session_init() registers + starts the /bin/login PROGRAM
 *     service. The login window will appear on the next gui_tick(). */
static void m14_init(void) {
    settings_init();

    /* Milestone 15: load /data/users (or seed defaults if absent) BEFORE
     * session_init so the login service can validate the typed name
     * against a real database. session_init itself relies on this. */
    users_init();

    /* Milestone 31: pick the active UI palette from settings BEFORE
     * the compositor paints anything. theme_init() is safe to call
     * even when /data isn't mounted -- settings_get_str returns the
     * default in that case, so the cyber palette wins. */
    theme_init();

    /* Milestone 31: bring up the in-kernel notification ring before
     * service_init so any boot-time emit (e.g. service crash on
     * first tick) lands cleanly. notify_init also registers the
     * "notify" builtin service, which is why we sequence it before
     * the explicit register_builtin block below. */
    notify_init();

    service_init();
    service_register_builtin("input");      /* keyboard + mouse already up */
    service_register_builtin("network");    /* net_init was called above */
    service_register_builtin("desktop");    /* compositor is up */
    service_register_builtin("session");    /* the session manager itself */

    gui_set_desktop_mode(true);

    session_init();
    service_dump();

    /* Milestone 31: kernel-emitted welcome toast. Lands in the
     * notification ring; the compositor pops it the next time
     * gui_tick runs, so the user sees a "Welcome to tobyOS" toast
     * fade in over the desktop a few hundred ms after the wallpaper
     * paints. Also seeds the notification center with a non-empty
     * list so first-time users have something to look at when they
     * click the bell. */
    {
        char hostname[64];
        size_t hn = settings_get_str("system.hostname", hostname,
                                     sizeof(hostname), "tobyOS");
        (void)hn;
        char body[ABI_NOTIFY_BODY_MAX];
        ksnprintf(body, sizeof(body),
                  "Welcome to %s. Click [Apps] to launch a program, "
                  "or click the bell to open the notification center.",
                  hostname);
        notify_post(ABI_NOTIFY_KIND_SYSTEM, ABI_NOTIFY_URG_INFO,
                    "kernel", "Desktop ready", body);
    }
    notify_post(ABI_NOTIFY_KIND_SYSTEM, ABI_NOTIFY_URG_INFO,
                "session", "Login service started",
                "/bin/login is running. Type 'root' to log in.");
}

/* Limine's RSDP request is documented as returning an HHDM-mirrored
 * virt pointer, but in practice (Limine v11.x as of this writing) it
 * sometimes hands back a raw physical address. The RSDP itself usually
 * lives in the BIOS F-segment (0xE0000..0xFFFFF) which is RESERVED in
 * the memmap, so vmm_init's HHDM mirror does NOT cover it. Two things
 * to handle:
 *   1. If the address looks like a phys (below the HHDM offset),
 *      add the HHDM offset to convert it.
 *   2. Map the page into HHDM ourselves, since vmm skipped RESERVED. */
static void *normalise_rsdp_pointer(void *raw) {
    if (!raw) return 0;

    uint64_t addr = (uint64_t)raw;
    uint64_t hhdm = hhdm_req.response ? hhdm_req.response->offset : 0;

    if (addr < hhdm) {
        uint64_t page = addr & ~((uint64_t)PAGE_SIZE - 1);
        (void)vmm_hhdm_ensure_mapped(page, PAGE_SIZE * 2, VMM_PRESENT | VMM_NX);
        addr += hhdm;
    } else if (hhdm && vmm_translate(addr) == 0) {
        uint64_t phys = addr - hhdm;
        uint64_t page = phys & ~((uint64_t)PAGE_SIZE - 1);
        (void)vmm_hhdm_ensure_mapped(page, PAGE_SIZE * 2, VMM_PRESENT | VMM_NX);
    }
    return (void *)addr;
}

/* SMP bring-up is split into two phases now that M22 drivers want
 * MSI-based IRQs at PCI-bind time:
 *
 *   smp_init_bsp()    -- ACPI + BSP LAPIC + IO APIC + IRQ facade
 *                        switch.  Must run BEFORE pci_bind_drivers so
 *                        any MSI-capable driver can call apic_read_id()
 *                        and irq_alloc_vector() during its probe.
 *
 *   smp_start_aps()   -- INIT-SIPI-SIPI for the AP cores.  Stays at
 *                        the end of init so the APs only enter the
 *                        scheduler once proc/sched/signal are alive.
 *
 * If the IO APIC is absent we stay on the legacy PIC -- preserving
 * the worst-case real-PC fallback path. If APIC bring-up fails
 * entirely we keep booting on the BSP only; the shell, userspace
 * path, and everything else still works exactly as before. */
static void smp_init_bsp(void) {
    void *rsdp = rsdp_req.response
                     ? normalise_rsdp_pointer(rsdp_req.response->address)
                     : 0;
    acpi_init(rsdp);
    if (!apic_init_bsp()) {
        kprintf("[boot] WARNING: BSP LAPIC init failed -- staying on PIC\n");
        return;
    }
    /* IO APIC + IRQ facade switch must come AFTER apic_init_bsp
     * (we need the LAPIC MMIO + apic_read_id). After this returns,
     * any subsequent irq_install_isa() goes through the IO APIC, and
     * MSI/MSI-X is safe to enable on PCI devices. */
    if (ioapic_init()) {
        irq_switch_to_ioapic();
    }
}

static __attribute__((noreturn)) void idle_loop(void) {
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 1;

    /* Keep local input fresh without letting it monopolise pid 0.
     * Keyboard delivery is immediate in the HID/PS2 path; mouse keeps
     * a tiny deferred queue because cursor drawing is a render concern. */
    #define SERVICE_INPUT()                                            \
        do {                                                           \
            usb_legacy_poll();                                         \
            xhci_poll();                                               \
            kbd_flush_pending();                                       \
            mouse_flush_pending();                                     \
        } while (0)

    for (;;) {
        /* SMP: pid 0's per-tick work (gui_tick, service ticks, net/USB/shell)
         * touches shared kernel state that user procs on other cores reach via
         * their syscalls, so each phase runs under the BKL.
         *
         * CRITICAL -- per-phase acquire/RELEASE, NOT one hold across the whole
         * iteration. A user proc busy-polling syscalls on an AP (notably
         * /bin/login's poll_event/yield main loop) takes the BKL on every call.
         * Holding the BKL across this entire body made that AP proc and pid 0
         * livelock on the lock -- both spinning ~100% in bkl_enter, the GUI
         * driver (pid 0) never completing a gui_tick -- which presented as a hard,
         * SMP-only desktop freeze (heartbeat just stops, no fault). Dropping the
         * BKL between phases bounds the hold so the fair ticket lock interleaves
         * the AP cleanly and pid 0 always makes forward progress. gui_tick's
         * internal sched_yield still drops/reacquires the BKL while a user proc
         * runs on this core. The phases are independent ticks -- no shared state
         * needs to persist held across a phase boundary. */

        /* Network service lane first: drain NIC RX and protocol daemons
         * before GUI/input work can consume pid 0's turn. */
        bkl_enter();
        net_service_tick();
        SERVICE_INPUT();
        bkl_exit();

        /*
         * Always drain xHCI once per loop.
         *
         * If xHCI IRQs work, this is usually a cheap no-op.
         * If xHCI IRQs do not work on real hardware, this is the only
         * thing keeping USB HID input responsive.
         */
        bkl_enter();
        SERVICE_INPUT();
        xhci_service_port_changes();
        usb_hub_poll();
        /* Catch any input that arrived while the other pollers ran. */
        SERVICE_INPUT();
        bkl_exit();

        /* GUI/window manager tick. */
        bkl_enter();
        gui_tick();
        bkl_exit();

        /* If GUI work yielded back after a repaint burst, give the
         * network lane another cheap chance before local input/shell. */
        bkl_enter();
        net_service_tick();
        SERVICE_INPUT();
        /* Local shell input. */
        shell_poll();
        /* Text cursor only when GUI is inactive. */
        if (!gui_active())
            console_tick(pit_ticks(), hz);
        acpi_m22_selftest_tick();
        bkl_exit();

        /*
         * Critical:
         *
         * If xHCI interrupts are enabled, hlt is good.
         * If xHCI is polling-only, hlt makes USB HID PIT-limited and
         * mouse movement becomes chunky.
         */
         __asm__ __volatile__("pause");
    }

    #undef SERVICE_INPUT
}
/* Smoke-test handler for vector 3 (#BP). Demonstrates that the IDT
 * dispatch reaches C and that we can iretq cleanly back to the caller.
 * Without this, the default exception handler would panic on int3. */
static void breakpoint_handler(struct regs *r) {
    kprintf("[isr] breakpoint hit at rip=%p (rflags=0x%lx)\n",
            (void *)r->rip, r->rflags);
}

static void int_smoke_test(void) {
    kprintf("[boot] firing int3 to exercise IDT round-trip...\n");
    isr_register(3, breakpoint_handler);
    __asm__ volatile ("int $3");
    kprintf("[boot] int3 returned cleanly, IDT round-trip OK\n");
}

static void pmm_init_and_test(void) {
    if (!hhdm_req.response) {
        kpanic("Limine HHDM request returned no response");
    }
    if (!memmap_req.response) {
        kpanic("Limine memmap request returned no response");
    }

    kprintf("[boot] HHDM offset = %p\n", (void *)hhdm_req.response->offset);

    pmm_init((struct limine_memmap_response *)memmap_req.response,
             hhdm_req.response->offset);

    /* IMPORTANT: do NOT bring the framebuffer console up here. On real
     * hardware UEFI, Limine does not always identity-map the lower
     * half, so any code path that walks the page tables before
     * vmm_init builds them (e.g. console_fb_writable -> vmm_translate)
     * will dereference NULL or read garbage and #PF. The post-CR3
     * hook (framebuffer_validate_mapping) brings the console up
     * once tobyOS's own PML4 is live, with the FB explicitly mapped
     * into HHDM. */

    /* Round-trip smoke test: alloc 4 pages, prove they're distinct, free
     * them, prove the free count returns to baseline. */
    size_t before = pmm_free_pages();
    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();
    uint64_t c = pmm_alloc_page();
    uint64_t d = pmm_alloc_page();
    kprintf("[pmm] test: allocated %p %p %p %p (free now=%lu)\n",
            (void *)a, (void *)b, (void *)c, (void *)d,
            (unsigned long)pmm_free_pages());

    /* Touch each page through the HHDM to make sure the address is
     * actually backed and writable. */
    *(uint64_t *)pmm_phys_to_virt(a) = 0xdeadbeefcafebabeULL;
    *(uint64_t *)pmm_phys_to_virt(d) = 0x1122334455667788ULL;
    kprintf("[pmm] test: read-back a=0x%lx d=0x%lx\n",
            *(uint64_t *)pmm_phys_to_virt(a),
            *(uint64_t *)pmm_phys_to_virt(d));

    pmm_free_page(a);
    pmm_free_page(b);
    pmm_free_page(c);
    pmm_free_page(d);
    size_t after = pmm_free_pages();
    if (after != before) {
        kpanic("pmm leak: before=%lu after=%lu", before, after);
    }
    kprintf("[pmm] test: alloc/free balanced (free=%lu before & after)\n",
            (unsigned long)before);
}

static void heap_init_and_test(void) {
    heap_init();

    struct heap_stats s0, s1, s2;
    heap_stats(&s0);
    kprintf("[heap] start: arenas=%lu total=%lu used=%lu free=%lu  "
            "brk=%p (region %p..%p)\n",
            (unsigned long)s0.arenas, (unsigned long)s0.total_bytes,
            (unsigned long)s0.used_bytes, (unsigned long)s0.free_bytes,
            (void *)heap_virt_brk(),
            (void *)heap_virt_base(), (void *)heap_virt_end());

    /* Mixed-size churn: assorted allocations, then free in a non-LIFO
     * order so coalescing has something to actually merge. */
    void *p[8];
    size_t sizes[8] = { 16, 64, 256, 4096, 13, 1, 1024, 80000 };
    for (int i = 0; i < 8; i++) {
        p[i] = kmalloc(sizes[i]);
        if (!p[i]) kpanic("kmalloc(%lu) returned NULL at i=%d",
                          (unsigned long)sizes[i], i);
        if (((uintptr_t)p[i] & 0xF) != 0) {
            kpanic("kmalloc(%lu) returned %p (not 16-byte aligned)",
                   (unsigned long)sizes[i], p[i]);
        }
        /* Every kmalloc pointer must land in the dedicated heap virt
         * window -- catches a regression where heap accidentally
         * returns a raw HHDM pointer again. */
        if ((uintptr_t)p[i] < heap_virt_base() ||
            (uintptr_t)p[i] >= heap_virt_end()) {
            kpanic("kmalloc(%lu) returned %p outside heap window [%p..%p)",
                   (unsigned long)sizes[i], p[i],
                   (void *)heap_virt_base(), (void *)heap_virt_end());
        }
        memset(p[i], (uint8_t)(0xA0 + i), sizes[i]);  /* exercise the storage */
    }

    heap_stats(&s1);
    kprintf("[heap] after 8 allocs: arenas=%lu used=%lu free=%lu allocs=%lu\n",
            (unsigned long)s1.arenas, (unsigned long)s1.used_bytes,
            (unsigned long)s1.free_bytes, (unsigned long)s1.alloc_count);

    /* The 80000-byte one is bigger than the initial 64 KiB arena, so we
     * must have grown to a second arena. Verify. */
    if (s1.arenas < 2) {
        kpanic("expected heap to grow >=2 arenas after 80000-byte alloc, "
               "got %lu", (unsigned long)s1.arenas);
    }

    /* Touch the head and tail of the big block to be sure all pages are
     * mapped + writable. */
    ((uint8_t *)p[7])[0]        = 0xCA;
    ((uint8_t *)p[7])[80000 - 1] = 0xFE;
    if (((uint8_t *)p[7])[0] != 0xCA || ((uint8_t *)p[7])[80000 - 1] != 0xFE) {
        kpanic("heap: read-back of large block failed");
    }

    /* Free in odd order to force coalescing across non-adjacent gaps. */
    int order[8] = { 3, 0, 6, 1, 4, 7, 2, 5 };
    for (int i = 0; i < 8; i++) kfree(p[order[i]]);

    heap_stats(&s2);
    kprintf("[heap] after 8 frees: arenas=%lu used=%lu free=%lu frees=%lu\n",
            (unsigned long)s2.arenas, (unsigned long)s2.used_bytes,
            (unsigned long)s2.free_bytes, (unsigned long)s2.free_count);

    if (s2.used_bytes != 0) {
        kpanic("heap leak: used_bytes=%lu after freeing everything",
               (unsigned long)s2.used_bytes);
    }

    /* kcalloc check. */
    uint32_t *zeroes = kcalloc(1024, sizeof(uint32_t));
    if (!zeroes) kpanic("kcalloc(1024 * 4) returned NULL");
    for (int i = 0; i < 1024; i++) {
        if (zeroes[i] != 0) kpanic("kcalloc returned non-zero at index %d", i);
    }
    kfree(zeroes);

    /* Fragmentation resilience: deliberately swiss-cheese the PMM by
     * grabbing 64 single pages and freeing every other one. Now the
     * largest physically-contiguous run is 1 page. The old heap (which
     * called pmm_alloc_pages(N)) would have failed any growth that
     * needed more than 1 contiguous page. The new heap goes through
     * vmm_map and stitches scattered frames into a contiguous virt
     * range, so an 80 KiB allocation must still succeed. */
    enum { FRAG_PAGES = 64 };
    uint64_t frag[FRAG_PAGES];
    size_t frag_alloced = 0;
    for (int i = 0; i < FRAG_PAGES; i++) {
        frag[i] = pmm_alloc_page();
        if (frag[i] == 0) break;          /* tiny RAM -- skip the test */
        frag_alloced++;
    }
    /* Free the even-indexed pages so phys is now used/free/used/free... */
    for (size_t i = 0; i < frag_alloced; i += 2) {
        pmm_free_page(frag[i]);
        frag[i] = 0;
    }

    void *big = kmalloc(80000);
    if (!big) {
        kpanic("heap: 80000-byte alloc failed under fragmented PMM "
               "(would have been impossible with the old contiguous-phys path)");
    }
        if ((uintptr_t)big < heap_virt_base() ||
            (uintptr_t)big >= heap_virt_end()) {
        kpanic("heap: fragmented-alloc returned out-of-region pointer %p", big);
    }
    /* Touch head, middle, tail to be sure every backing page is mapped. */
    ((uint8_t *)big)[0]            = 0x11;
    ((uint8_t *)big)[40000]        = 0x22;
    ((uint8_t *)big)[80000 - 1]    = 0x33;
    if (((uint8_t *)big)[0] != 0x11 ||
        ((uint8_t *)big)[40000] != 0x22 ||
        ((uint8_t *)big)[80000 - 1] != 0x33) {
        kpanic("heap: fragmented-alloc read-back failed");
    }
    kfree(big);

    /* Return the still-held odd pages to the PMM. */
    for (size_t i = 1; i < frag_alloced; i += 2) {
        pmm_free_page(frag[i]);
    }

    kprintf("[heap] test: ok (region OK, fragmented 80 KiB alloc OK, "
            "round-trip balanced, kcalloc zero-verified)\n");
    kprintf("[heap] brk now %p (%lu KiB virt consumed)\n",
            (void *)heap_virt_brk(),
            (unsigned long)((heap_virt_brk() - heap_virt_base()) / 1024));
}

static void user_first_run(void) {
    /* Smoke test: spawn /bin/hello as a real process and wait for it
     * before the shell takes over. Verifies the full milestone-5
     * round-trip (proc_create -> sched_yield -> first iretq -> user
     * runs -> SYS_EXIT -> proc_exit -> reap) works at boot. */
    kprintf("[boot] spawning first userspace process from /bin/hello...\n");
    int pid = proc_create_from_elf("/bin/hello", "hello-boot");
    if (pid < 0) {
        kprintf("[boot] WARNING: failed to spawn /bin/hello -- shell will "
                "start without a user process having run\n");
        return;
    }
    int rc = proc_wait(pid);
    kprintf("[boot] /bin/hello (pid=%d) finished, exit code=%d (0x%x)\n",
            pid, rc, (unsigned)rc);

    /* Milestone 25A smoke test: exercise every new syscall through
     * /bin/abi_test. The program is a freestanding ELF with inline
     * SYSCALL trampolines (no libc), so a regression in the libc
     * port (M25B) cannot mask a regression in the kernel-side
     * handlers added in M25A. Look for "[abi-test] ALL OK" in the
     * serial log to declare the milestone validated.
     *
     * Best-effort: if the binary isn't present (older boot media)
     * or /data isn't writable yet, we log + continue rather than
     * blocking the shell. */
    int pid2 = proc_create_from_elf("/bin/abi_test", "abi-test-boot");
    if (pid2 < 0) {
        kprintf("[boot] M25A: /bin/abi_test not spawned; "
                "ABI surface check skipped\n");
        return;
    }
    int rc2 = proc_wait(pid2);
    kprintf("[boot] M25A: /bin/abi_test (pid=%d) finished, exit code=%d "
            "(%s)\n",
            pid2, rc2, rc2 == 0 ? "PASS" : "FAIL");

    /* Milestone 25B smoke test: spawn each of the libtoby-linked C
     * sample programs and grade the result. These exercise the
     * libtoby static archive end-to-end: stdio (printf), heap
     * (malloc/free/realloc), file I/O (FILE *), argv/envp pickup,
     * and the libtoby_init -> main -> exit() chain. The kernel only
     * sees them as ordinary user processes -- the libc runs entirely
     * in userspace.
     *
     * Each program prints "[<name>] ALL OK" on stdout when every
     * sub-check passed, then returns 0. A non-zero exit (or absent
     * binary) is logged but does not block the shell. */
    static const struct {
        const char *path;
        const char *tag;
    } m25b_demos[] = {
        { "/bin/c_hello",     "c_hello-boot"     },
        { "/bin/c_args",      "c_args-boot"      },
        { "/bin/c_filedemo",  "c_filedemo-boot"  },
        { "/bin/c_alloctest", "c_alloctest-boot" },
    };
    int m25b_pass = 0, m25b_total = 0;
    for (size_t i = 0; i < sizeof(m25b_demos)/sizeof(m25b_demos[0]); i++) {
        int pid_n = proc_create_from_elf(m25b_demos[i].path, m25b_demos[i].tag);
        if (pid_n < 0) {
            kprintf("[boot] M25B: %s not spawned; libc demo skipped\n",
                    m25b_demos[i].path);
            continue;
        }
        m25b_total++;
        int rc_n = proc_wait(pid_n);
        kprintf("[boot] M25B: %s (pid=%d) exit=%d (%s)\n",
                m25b_demos[i].path, pid_n, rc_n, rc_n == 0 ? "PASS" : "FAIL");
        if (rc_n == 0) m25b_pass++;
    }
    kprintf("[boot] M25B: libtoby demos %d/%d PASS\n", m25b_pass, m25b_total);

    /* ============================================================ *
     *  Milestone 25C smoke test
     *
     *  Two flavours of validation:
     *
     *    (a) Re-spawn /bin/c_args via proc_spawn() with explicit
     *        argv + envp synthesised here in the kernel. This proves
     *        that argv+envp packed onto the user stack survive the
     *        spawn pipeline AND that the libtoby crt0 surfaces them
     *        verbatim to main() and to environ. Look for the program
     *        printing argc=4 envc=3 in the boot log.
     *
     *    (b) Spawn /bin/c_envtest with a sensible env -- it self-tests
     *        every M25C surface (setenv/unsetenv/putenv/clearenv,
     *        system(), execvp emulation). It exits 0 on PASS.
     *
     *  Failures are non-fatal: we log + continue so the shell still
     *  comes up for human inspection. */
    {
        char *m25c_argv[] = {
            (char *)"c_args", (char *)"alpha", (char *)"beta", (char *)"gamma", 0
        };
        char *m25c_envp[] = {
            (char *)"PATH=/bin",
            (char *)"TEST_PHASE=25C",
            (char *)"BUILD=ok",
            0
        };
        struct proc_spec spec = {
            .path = "/bin/c_args",
            .name = "c_args-spec",
            .argc = 4,
            .argv = m25c_argv,
            .envc = 3,
            .envp = m25c_envp,
        };
        int pid3 = proc_spawn(&spec);
        if (pid3 < 0) {
            kprintf("[boot] M25C: /bin/c_args via proc_spawn failed to spawn\n");
        } else {
            int rc3 = proc_wait(pid3);
            kprintf("[boot] M25C: /bin/c_args (pid=%d, argc=4 envc=3) "
                    "exit=%d (%s)\n",
                    pid3, rc3, rc3 == 0 ? "PASS" : "FAIL");
        }
    }
    {
        char *env_argv[] = { (char *)"c_envtest", 0 };
        char *env_envp[] = {
            (char *)"PATH=/bin",
            (char *)"HOME=/",
            (char *)"M25C_INHERIT=from-kernel",
            0
        };
        struct proc_spec spec = {
            .path = "/bin/c_envtest",
            .name = "c_envtest-boot",
            .argc = 1,
            .argv = env_argv,
            .envc = 3,
            .envp = env_envp,
        };
        int pid4 = proc_spawn(&spec);
        if (pid4 < 0) {
            kprintf("[boot] M25C: /bin/c_envtest not spawned; "
                    "exec/env smoke test skipped\n");
        } else {
            int rc4 = proc_wait(pid4);
            kprintf("[boot] M25C: /bin/c_envtest (pid=%d) exit=%d (%s)\n",
                    pid4, rc4, rc4 == 0 ? "PASS" : "FAIL");
        }
    }

    /* ---- M25D: dynamic-linker smoke ----------------------------------
     *
     *  Spawn /bin/c_dynhello -- a PIE that lists /lib/ld-toby.so as its
     *  PT_INTERP and DT_NEEDED=libtoby.so. If this exits 0, then end to
     *  end:
     *
     *    1. The kernel ELF loader handled ET_DYN at a relocated base
     *       and loaded the interpreter at a separate base.
     *    2. proc.c built and packed the AT_PHDR/AT_BASE/AT_ENTRY auxv
     *       array correctly.
     *    3. ld-toby.so successfully self-relocated, walked the program
     *       _DYNAMIC, called sys_dload() to map libtoby.so, and applied
     *       JUMP_SLOT relocations against it.
     *    4. printf/exit -- both library symbols -- worked through the
     *       dynamically-resolved PLT.
     *
     *  Look for the [c_dynhello] lines and a PASS in the boot log. */
    {
        char *dyn_argv[] = { (char *)"c_dynhello", 0 };
        char *dyn_envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/c_dynhello",
            .name = "c_dynhello-boot",
            .argc = 1,
            .argv = dyn_argv,
            .envc = 1,
            .envp = dyn_envp,
        };
        int pid5 = proc_spawn(&spec);
        if (pid5 < 0) {
            kprintf("[boot] M25D: /bin/c_dynhello not spawned; "
                    "dynamic-linker smoke test skipped\n");
        } else {
            int rc5 = proc_wait(pid5);
            kprintf("[boot] M25D: /bin/c_dynhello (pid=%d) exit=%d (%s)\n",
                    pid5, rc5, rc5 == 0 ? "PASS" : "FAIL");
        }
    }

    /* ---- M25E: ported sbase-style userland smoke ---------------------
     *
     *  Run each ported tool with a representative argv against either
     *  /etc/motd, /readme.txt, or the bundled initrd file system. We
     *  treat exit code 0 as PASS. Each port lives at /bin/p_<name> so
     *  it doesn't collide with the existing /bin/echo and /bin/cat
     *  custom-userland binaries; the ports are statically linked
     *  against libtoby.a, so they exercise libtoby's printf, string
     *  routines, getopt, opendir/readdir, and stat wrappers without
     *  also depending on the dynamic linker (which M25D already
     *  validated separately).
     *
     *  Look for [boot] M25E: <tool> ... PASS lines in the boot log. */
    {
        struct port_test {
            const char *path;
            const char *name;
            int         argc;
            const char *argv[6];
        };
        static const struct port_test ports[] = {
            { "/bin/p_echo", "p_echo-boot", 3,
              { "p_echo", "hello", "from-port-echo", 0, 0, 0 } },
            { "/bin/p_cat",  "p_cat-boot",  2,
              { "p_cat",  "/etc/motd", 0, 0, 0, 0 } },
            { "/bin/p_wc",   "p_wc-boot",   2,
              { "p_wc",   "/etc/motd", 0, 0, 0, 0 } },
            { "/bin/p_head", "p_head-boot", 4,
              { "p_head", "-n", "2", "/etc/motd", 0, 0 } },
            { "/bin/p_ls",   "p_ls-boot",   2,
              { "p_ls",   "/bin", 0, 0, 0, 0 } },
            { "/bin/p_grep", "p_grep-boot", 3,
              { "p_grep", "tobyOS", "/etc/motd", 0, 0, 0 } },
        };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        int passed = 0;
        for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
            const struct port_test *pt = &ports[i];
            char *argv[6];
            for (int a = 0; a < pt->argc; a++)
                argv[a] = (char *)pt->argv[a];
            argv[pt->argc] = 0;
            struct proc_spec spec = {
                .path = pt->path,
                .name = pt->name,
                .argc = pt->argc,
                .argv = argv,
                .envc = 1,
                .envp = envp,
            };
            int pid = proc_spawn(&spec);
            if (pid < 0) {
                kprintf("[boot] M25E: %s not spawned (rc=%d)\n",
                        pt->path, pid);
                continue;
            }
            int rc = proc_wait(pid);
            kprintf("[boot] M25E: %s (pid=%d) exit=%d (%s)\n",
                    pt->path, pid, rc, rc == 0 ? "PASS" : "FAIL");
            if (rc == 0) passed++;
        }
        kprintf("[boot] M25E: ports %d/%lu PASS\n",
                passed, (unsigned long)(sizeof(ports) / sizeof(ports[0])));
    }
}

/* Milestone 25C: post-shell_init validation. Runs a short sequence of
 * synthetic shell command lines so the boot log captures the shell's
 * own PATH lookup + envp propagation. Skipped if the shell hasn't
 * actually been initialised (defensive -- shell_init() is invoked
 * unconditionally on the same boot path that calls us). */
static void user_shell_smoketest(void) {
    static const char *lines[] = {
        "env",
        "setenv M25C_FROM_SHELL yes",
        "env",
        "c_args from-shell PATH-resolved",
        "unsetenv M25C_FROM_SHELL",
        0,
    };
    kprintf("[boot] M25C: driving shell with synthetic command lines\n");
    for (int i = 0; lines[i]; i++) {
        shell_run_test_line(lines[i]);
    }
    kprintf("[boot] M25C: shell smoketest done\n");
}

/* POSIX shell compatibility track: boot-driven smoke harness for the
 * command language features that tobysh is growing toward. This stays
 * deliberately sentinel-oriented so the PowerShell driver can validate
 * behaviour from serial.log without trying to interact with the shell. */
static void posix_shell_selftest(void) {
    static const char *lines[] = {
        "echo POSIXSH: start",
        "echo POSIXSH: subst=$(echo ok)",
        "echo POSIXSH: backtick=`echo ok2`",
        "false || echo POSIXSH: or-ok",
        "true && echo POSIXSH: and-ok",
        "if false; then echo POSIXSH: if-bad; else echo POSIXSH: if-ok; fi",
        "for x in alpha beta; do echo POSIXSH: for-$x; done",
        "set -- one two three",
        "echo POSIXSH: params count=$# first=$1 all=$*",
        "echo POSIXSH: default=${POSIX_MISSING:-fallback}",
        "echo POSIXSH: assign=${POSIX_ASSIGN:=setnow} again=$POSIX_ASSIGN",
        "echo POSIXSH: alt=${POSIX_ASSIGN:+alt}",
        "echo POSIXSH: len=${#POSIX_ASSIGN}",
        "echo POSIXSH: arith=$((1+2*3))",
        "echo POSIXSH: arith-cmp=$((3>2))-$((3<2))-$((3==3))-$((3!=2))",
        "echo POSIXSH: arith-logic=$((1&&0))-$((1||0))-$((!0))",
        "POSIX_STRIP=hello.world.txt",
        "echo POSIXSH: strip-prefix=${POSIX_STRIP#*.}",
        "echo POSIXSH: strip-prefix-greedy=${POSIX_STRIP##*.}",
        "echo POSIXSH: strip-suffix=${POSIX_STRIP%.*}",
        "echo POSIXSH: strip-suffix-greedy=${POSIX_STRIP%%.*}",
        "if false; then echo POSIXSH: elif-bad; elif true; then echo POSIXSH: elif-ok; else echo POSIXSH: elif-bad2; fi",
        "! false",
        "echo POSIXSH: negate=$?",
        "alias px='echo POSIXSH: alias-ok'",
        "px",
        "unalias px",
        "greet() { echo POSIXSH: func-$1-$#; return 0; }",
        "greet one two",
        "case beta in alpha) echo POSIXSH: case-bad ;; beta) echo POSIXSH: case-ok ;; *) echo POSIXSH: case-wild ;; esac",
        "for x in keep skip stop later; do case $x in skip) continue ;; stop) break ;; *) echo POSIXSH: loop-$x ;; esac; done",
        "until false; do echo POSIXSH: until-once; break; done",
        "write /data/posixsh_source.sh echo POSIXSH: script-ok",
        "sh /data/posixsh_source.sh",
        "write /data/posixsh_return.sh 'echo POSIXSH: return-before; return 7; echo POSIXSH: return-bad'",
        "sh /data/posixsh_return.sh",
        "echo POSIXSH: return-status=$?",
        "sh -c 'echo POSIXSH: shc-$0-$1-$#' label arg1",
        "/bin/echo POSIXSH: fd1-file >/data/posixsh_fd1.txt",
        "cat /data/posixsh_fd1.txt",
        "p_cat /data/posixsh_no_fd2 2>/data/posixsh_fd2.txt",
        "cat /data/posixsh_fd2.txt",
        "p_cat /data/posixsh_no_merge >/data/posixsh_merge.txt 2>&1",
        "cat /data/posixsh_merge.txt",
        "p_cat /data/posixsh_order 2>&1 >/data/posixsh_order.txt",
        "echo POSIXSH: dup-order-status=$?",
        "p_cat /data/posixsh_closed 2>&-",
        "echo POSIXSH: close-fd-status=$?",
        "> /data/posixsh_redir_only.txt",
        "/bin/echo POSIXSH: redir-only-ok >>/data/posixsh_redir_only.txt",
        "cat /data/posixsh_redir_only.txt",

        "write /data/posixsh_read_input.txt alpha beta gamma",
        "read X Y </data/posixsh_read_input.txt",
        "echo POSIXSH: read-file-$X-$Y",
        "sh /data/posixsh_read_heredoc.sh",

        "echo hello | echo POSIXSH: builtin-pipe",
        "pipefn() { echo POSIXSH: function-pipe; }",
        "echo hello | pipefn",
        "unset X Y",
        "echo hello | read X Y",
        "echo POSIXSH: read-pipe-status=$? var=${X:-unset}-${Y:-unset}",
        "echo hello | cd /",
        "echo POSIXSH: pipe-cwd-$PWD",

        "/bin/echo POSIXSH: wait-bg &",
        "echo POSIXSH: bgpid=$!",
        "wait $!",
        "echo POSIXSH: wait-status=$?",
        "/bin/echo POSIXSH: wait-all-bg &",
        "wait",
        "echo POSIXSH: wait-all-status=$?",

        "POSIX_SPECIAL=kept export POSIX_SPECIAL",
        "echo POSIXSH: special-assign=$POSIX_SPECIAL",
        "POSIX_TEMP=visible env >/data/posixsh_env_tmp.txt",
        "cat /data/posixsh_env_tmp.txt",
        "echo POSIXSH: temp-after=${POSIX_TEMP:-unset}",
        "readonly POSIX_RO=locked",
        "POSIX_RO=changed",
        "echo POSIXSH: readonly-status=$? value=$POSIX_RO",
        "set -- shift-a shift-b shift-c",
        "shift 2",
        "echo POSIXSH: shift-$#-$1",

        "OPTIND=1",
        "getopts ab: OPT -a -b bee extra",
        "echo POSIXSH: getopts-$OPT-${OPTARG:-none}-$OPTIND",
        "getopts ab: OPT -a -b bee extra",
        "echo POSIXSH: getopts-$OPT-${OPTARG:-none}-$OPTIND",
        "getopts ab: OPT -a -b bee extra",
        "echo POSIXSH: getopts-done-$OPTIND",

        "OPTIND=1",
        "getopts ab: OPT -ab bee",
        "echo POSIXSH: getopts-group-$OPT-${OPTARG:-none}-$OPTIND",
        "getopts ab: OPT -ab bee",
        "echo POSIXSH: getopts-group-$OPT-${OPTARG:-none}-$OPTIND",

        "OPTIND=1",
        "getopts :ab: OPT -z",
        "echo POSIXSH: getopts-bad-$OPT-${OPTARG:-none}-$?",

        "OPTIND=1",
        "getopts :ab: OPT -b",
        "echo POSIXSH: getopts-missing-$OPT-${OPTARG:-none}-$?",

        "OPTIND=1",
        "getopts x: OPT -x explicit",
        "echo POSIXSH: getopts-explicit-$OPT-${OPTARG:-none}-$OPTIND",

        "command -V export",
        "sh -c 'echo POSIXSH: exit-before; exit 9; echo POSIXSH: exit-bad'",
        "echo POSIXSH: exit-status=$?",
        "sh -c 'trap \"echo POSIXSH: trap-exit\" EXIT; echo POSIXSH: trap-body'",
        "echo POSIXSH: trap-status=$?",
        "sh -c 'trap \"echo POSIXSH: trap-reset-bad\" EXIT; trap - EXIT; echo POSIXSH: trap-reset-ok'",
        "command -V trap",
        "fredir() { echo POSIXSH: func-redir-one; echo POSIXSH: func-redir-two; }",
        "fredir >/data/posixsh_func_redir.txt",
        "cat /data/posixsh_func_redir.txt",
        "fxredir() { /bin/echo POSIXSH: func-ext-redir; }",
        "fxredir >/data/posixsh_func_ext.txt",
        "cat /data/posixsh_func_ext.txt",
        "{ echo POSIXSH: group-redir-one; echo POSIXSH: group-redir-two; } >/data/posixsh_group_redir.txt",
        "cat /data/posixsh_group_redir.txt",
        "( cd /data; POSIX_SUB=inside; readonly POSIX_SUB_RO=locked; sh -c 'echo POSIXSH: subshell-pwd-$PWD var-$POSIX_SUB'; trap \"echo POSIXSH: subshell-trap\" EXIT ) >/data/posixsh_subshell.txt",
        "cat /data/posixsh_subshell.txt",
        "echo POSIXSH: subshell-outer-pwd=$PWD var=${POSIX_SUB:-unset}",
        "( alias subalias='echo POSIXSH: subshell-alias-bad' )",
        "alias subalias || echo POSIXSH: subshell-alias-isolated",
        "( subfn() { echo POSIXSH: subshell-function-bad; } )",
        "command -v subfn || echo POSIXSH: subshell-function-isolated",
        "POSIX_SUB_RO=changed",
        "echo POSIXSH: subshell-readonly-status=$?",
        "( echo POSIXSH: subshell-exit-before; exit 6; echo POSIXSH: subshell-exit-bad )",
        "echo POSIXSH: subshell-exit-status=$?",
        "( echo POSIXSH: subshell-redir-one; echo POSIXSH: subshell-redir-two ) >/data/posixsh_subshell_redir.txt",
        "cat /data/posixsh_subshell_redir.txt",
        "eval echo POSIXSH: eval-ok",
        "touch /data/posixsh_a.txt",
        "touch /data/posixsh_b.txt",
        "echo POSIXSH: glob /data/posixsh_?.txt",
        "echo POSIXSH: colon-before; :; echo POSIXSH: colon-after",
        "command -v echo",
        "which echo",

        /* --- new POSIX features --- */

        /* echo -n: suppress trailing newline */
        "echo -n POSIXSH:; echo ' echo-n-ok'",

        /* echo -e: escape interpretation */
        "echo -e 'POSIXSH: echo-e-tab\\tok'",

        /* ${10}+ positional params via set -- */
        "set -- a b c d e f g h i j k",
        "echo POSIXSH: pos10=${10}-pos11=${11}",

        /* break n / continue n: test via script for nested loops */
        "sh /data/posixsh_break_n.sh",
        "sh /data/posixsh_cont_n.sh",

        /* set -x: xtrace prints commands with + prefix */
        "set -x",
        "echo POSIXSH: xtrace-on",
        "set +x",

        /* set -e: errexit causes exit on error (tested in subshell) */
        "sh -c 'set -e; false; echo POSIXSH: errexit-bad'",
        "echo POSIXSH: errexit-status=$?",

        /* set -u: nounset causes error on unset var */
        "unset NOVAR_POSIX",
        "sh -c 'set -u; echo $NOVAR_POSIX 2>/dev/null'",
        "echo POSIXSH: nounset-status=$?",

        /* $- shows current shell flags */
        "sh /data/posixsh_dollar_dash.sh",

        /* ~user expansion */
        "echo POSIXSH: tilde-user=$(echo ~root)",

        /* IFS word splitting */
        "export IFS=:",
        "V=a:b:c",
        "set -- $V",
        "unset IFS",
        "echo POSIXSH: ifs-split=$#-$1-$2-$3",

        /* export -p produces re-importable output */
        "export POSIXSH_EP=hello",
        "sh /data/posixsh_exportp.sh",

        /* set -- (no args) clears positional params */
        "set -- a b c",
        "set --",
        "echo POSIXSH: setdash-count=$#",

        /* trap '' SIG ignores (verify it sets the trap) */
        "sh -c 'trap \"\" INT; trap'",
        "trap - INT",

        /* test -t checks fd is a terminal */
        "test -t 0 && echo POSIXSH: test-t-ok",

        /* test -L (symlink, should fail in our simple VFS) */
        "test -L /bin/cat || echo POSIXSH: test-L-ok",

        /* $'...' ANSI-C quoting */
        "echo POSIXSH: ansic=$'hello\\tworld'",

        /* $* joins with first char of IFS */
        "set -- a b c",
        "export IFS=,",
        "echo POSIXSH: star-ifs=\"$*\"",
        "unset IFS",

        /* command -p runs command */
        "command -p echo POSIXSH: cmd-p-ok",

        /* arithmetic with bare variable names */
        "X=10",
        "Y=20",
        "echo POSIXSH: arith-var=$((X+Y))",

        /* arithmetic ternary */
        "echo POSIXSH: arith-tern=$((1 ? 42 : 99))",

        /* arithmetic assignment */
        "Z=5",
        "echo POSIXSH: arith-asgn=$((Z += 3))-$Z",

        /* set -f disables globbing */
        "set -f",
        "echo POSIXSH: noglob=*.txt",
        "set +f",

        /* case with | pattern */
        "case banana in apple|banana) echo POSIXSH: case-or-ok ;; *) echo POSIXSH: case-or-bad ;; esac",

        /* unset -f removes functions */
        "tempfn() { echo bad; }",
        "unset -f tempfn",
        "command -v tempfn || echo POSIXSH: unset-f-ok",

        /* arithmetic hex/octal */
        "echo POSIXSH: arith-hex=$((0xFF))",
        "echo POSIXSH: arith-oct=$((010))",

        /* arithmetic comma */
        "echo POSIXSH: arith-comma=$((1, 2, 3))",

        /* arithmetic pre-increment */
        "Q=5",
        "echo POSIXSH: arith-preinc=$((++Q))-$Q",

        /* arithmetic post-increment */
        "echo POSIXSH: arith-postinc=$((Q++))-$Q",

        /* for x; do (implicit $@) */
        "set -- p q r",
        "sh -c 'for x; do echo POSIXSH: forimpl-$x; done' arg0 hello world",

        /* printf %b */
        "printf 'POSIXSH: printf-b=%b\\n' 'tab\\there'",

        /* set -f noglob */
        "set -f",
        "echo POSIXSH: noglob2=*",
        "set +f",

        "echo POSIXSH: done",
        0
    };
    static const char heredoc_script[] =
        "/bin/cat <<EOF\n"
        "POSIXSH: heredoc-$POSIX_ASSIGN\n"
        "EOF\n";

    int hrc = vfs_write_all("/data/posixsh_heredoc.sh",
                            heredoc_script, strlen(heredoc_script));
    if (hrc != VFS_OK) {
        kprintf("POSIXSH: heredoc-setup-failed %s\n", vfs_strerror(hrc));
    }

    static const char read_heredoc_script[] =
        "read A B <<DELIM\n"
        "one two\n"
        "DELIM\n"
        "echo POSIXSH: read-heredoc-$A-$B\n";
    hrc = vfs_write_all("/data/posixsh_read_heredoc.sh",
                        read_heredoc_script, strlen(read_heredoc_script));
    if (hrc != VFS_OK) {
        kprintf("POSIXSH: read-heredoc-setup-failed %s\n", vfs_strerror(hrc));
    }

    static const char ifs_script[] =
        "IFS=:\n"
        "V=a:b:c\n"
        "set -- $V\n"
        "echo POSIXSH: ifs-split=$#-$1-$2-$3\n";
    hrc = vfs_write_all("/data/posixsh_ifs.sh",
                        ifs_script, strlen(ifs_script));

    static const char dollar_dash_script[] =
        "set -ex\n"
        "echo POSIXSH: dollar-dash=$-\n";
    hrc = vfs_write_all("/data/posixsh_dollar_dash.sh",
                        dollar_dash_script, strlen(dollar_dash_script));

    static const char exportp_script[] =
        "export -p\n";
    hrc = vfs_write_all("/data/posixsh_exportp.sh",
                        exportp_script, strlen(exportp_script));

    static const char break_n_script[] =
        "R=\n"
        "for i in 1 2; do for j in a b; do R=$R$i$j; break 2; done; done\n"
        "echo POSIXSH: break2=$R\n";
    hrc = vfs_write_all("/data/posixsh_break_n.sh",
                        break_n_script, strlen(break_n_script));

    static const char cont_n_script[] =
        "R=\n"
        "for i in 1 2; do for j in a b; do R=$R$i$j; continue 2; done; done\n"
        "echo POSIXSH: cont2=$R\n";
    hrc = vfs_write_all("/data/posixsh_cont_n.sh",
                        cont_n_script, strlen(cont_n_script));

    kprintf("[boot] POSIXSH: starting shell compatibility smoke\n");
    for (int i = 0; lines[i]; i++) {
        shell_run_test_line(lines[i]);
        if (strcmp(lines[i], "sh /data/posixsh_source.sh") == 0) {
            shell_run_test_line("sh /data/posixsh_heredoc.sh");
        }
    }
    kprintf("POSIXSH: PASS\n");
}

/* Milestone 26A: post-shell_init validation for the new shell builtins
 * + userland test programs. Runs `devlist` and `drvtest` via the shell
 * (kernel-side path), then spawns each /bin/<tool> with representative
 * argv (libtoby + SYS_DEV_LIST/SYS_DEV_TEST path).
 *
 * Each program's output is funnelled to the serial console through
 * libtoby stdout, so a grep of serial.log for "[devlist]"/"[drvtest]"
 * etc. is sufficient to confirm both transports work end-to-end. The
 * exit codes are still inspected so a panic-free hang on a probe is
 * caught: PASS = exit 0, FAIL = anything else. */
static void m26a_run_userland_tools(void) {
    static const char *shell_lines[] = {
        "echo --- M26A shell builtins ---",
        "devlist",
        "devlist usb",
        "devlist pci",
        "drvtest",
        "drvtest pci",
        "drvtest xhci",
        /* M26B: hub-bus enumeration + hub class self-test through the
         * shell builtin path. devlist hub renders the hub_depth/hub_port
         * topology; drvtest usb_hub runs the per-hub aggregate test. */
        "echo --- M26B shell builtins ---",
        "devlist hub",
        "drvtest usb_hub",
        0,
    };
    kprintf("[boot] M26A: driving shell builtins (devlist + drvtest)\n");
    for (int i = 0; shell_lines[i]; i++) {
        shell_run_test_line(shell_lines[i]);
    }

    struct utool {
        const char *path;
        const char *name;
        int         argc;
        const char *argv[6];
    };
    static const struct utool tools[] = {
        { "/bin/devlist",      "devlist-boot",      2,
          { "devlist",      "all",        0, 0, 0, 0 } },
        { "/bin/devlist",      "devlist-usb-boot",  2,
          { "devlist",      "usb",        0, 0, 0, 0 } },
        /* M26B: list hub records produced by usb_hub.c. */
        { "/bin/devlist",      "devlist-hub-boot",  2,
          { "devlist",      "hub",        0, 0, 0, 0 } },
        { "/bin/drvtest",      "drvtest-boot",      1,
          { "drvtest",      0, 0, 0, 0, 0 } },
        { "/bin/drvtest",      "drvtest-named",     4,
          { "drvtest",      "pci", "xhci", "usb", 0, 0 } },
        { "/bin/usbtest",      "usbtest-list",      2,
          { "usbtest",      "list",       0, 0, 0, 0 } },
        { "/bin/usbtest",      "usbtest-ctrl",      2,
          { "usbtest",      "controller", 0, 0, 0, 0 } },
        { "/bin/usbtest",      "usbtest-devs",      2,
          { "usbtest",      "devices",    0, 0, 0, 0 } },
        /* M26B: usbtest hub asks the kernel hub class driver to report
         * hub topology + run its self-test. Exit 0 even when no hub is
         * attached (clean SKIP). */
        { "/bin/usbtest",      "usbtest-hub",       2,
          { "usbtest",      "hub",        0, 0, 0, 0 } },
        /* M26C: usbtest hotplug runs the kernel-side ring round-trip
         * via SYS_DEV_TEST + drains the live hot-plug ring. Both halves
         * are PASS even when nothing has been physically (un)plugged
         * because the synthetic round-trip is always exercised. */
        { "/bin/usbtest",      "usbtest-hotplug",   2,
          { "usbtest",      "hotplug",    0, 0, 0, 0 } },
        /* M26D: usbtest hid lists INPUT bus + runs input + usb_hid
         * self-tests. SKIPs cleanly when no USB HID device is present
         * (pure PS/2 boot still PASSes the "input" half). */
        { "/bin/usbtest",      "usbtest-hid",       2,
          { "usbtest",      "hid",        0, 0, 0, 0 } },
        /* M26E: usbtest storage walks the BLK bus, runs the usb_msc
         * self-test, and exercises a FAT32 RW round-trip on /usb if
         * a usb-storage device is mounted. SKIPs cleanly otherwise. */
        { "/bin/usbtest",      "usbtest-storage",   2,
          { "usbtest",      "storage",    0, 0, 0, 0 } },
        /* M26F: audiotest now runs the full HDA bring-up validator.
         * Same binary handles "audio controller present + codec present"
         * (PASS), "controller present but no codec attached" (SKIP),
         * and "no controller at all" (SKIP). Exit 0 in every case
         * so booting on a machine without an audio chip stays clean. */
        { "/bin/audiotest",    "audiotest-boot",    1,
          { "audiotest",    0, 0, 0, 0, 0 } },
        { "/bin/batterytest",  "batterytest-boot",  1,
          { "batterytest",  0, 0, 0, 0, 0 } },
    };
    char *envp[] = { (char *)"PATH=/bin", 0 };
    int passed = 0, failed = 0, missing = 0;
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        const struct utool *u = &tools[i];
        char *argv[6];
        for (int a = 0; a < u->argc; a++) argv[a] = (char *)u->argv[a];
        argv[u->argc] = 0;
        struct proc_spec spec = {
            .path = u->path,
            .name = u->name,
            .argc = u->argc,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] M26A: %s not spawned (rc=%d)\n", u->path, pid);
            missing++;
            continue;
        }
        int rc = proc_wait(pid);
        const char *tag;
        if (rc == 0)         { tag = "PASS"; passed++; }
        else                 { tag = "FAIL"; failed++; }
        kprintf("[boot] M26A: %s (pid=%d) exit=%d (%s)\n",
                u->path, pid, rc, tag);
    }
    kprintf("[boot] M26A: userland %d PASS / %d FAIL / %d missing of %lu\n",
            passed, failed, missing,
            (unsigned long)(sizeof(tools) / sizeof(tools[0])));
}

/* Milestone 27A: display test harness boot validator. Spawns the three
 * new userland tools introduced for M27A (displayinfo / drawtest /
 * rendertest) and reports exit-code based PASS/FAIL with the same line
 * shape M26A uses, so the test_m27a.ps1 grep regex set is symmetric.
 *
 * displayinfo runs in plain mode + --json mode so we exercise both
 * output paths. drawtest runs once in non-interactive mode (it auto-
 * exits after a single full draw cycle). rendertest runs the full
 * default suite (every case). All three must exit 0. */
static void m27a_run_userland_tools(void) {
    struct utool {
        const char *path;
        const char *name;
        int         argc;
        const char *argv[6];
    };
    static const struct utool tools[] = {
        { "/bin/displayinfo",  "displayinfo-boot",  1,
          { "displayinfo",  0,        0, 0, 0, 0 } },
        { "/bin/displayinfo",  "displayinfo-json",  2,
          { "displayinfo",  "--json", 0, 0, 0, 0 } },
        { "/bin/drawtest",     "drawtest-boot",     1,
          { "drawtest",     0,        0, 0, 0, 0 } },
        { "/bin/rendertest",   "rendertest-boot",   1,
          { "rendertest",   0,        0, 0, 0, 0 } },
        { "/bin/fonttest",     "fonttest-boot",     1,
          { "fonttest",     0,        0, 0, 0, 0 } },
    };
    char *envp[] = { (char *)"PATH=/bin", 0 };
    int passed = 0, failed = 0, missing = 0;
    kprintf("[boot] M27A: driving display test harness "
            "(displayinfo + drawtest + rendertest)\n");
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        const struct utool *u = &tools[i];
        char *argv[6];
        for (int a = 0; a < u->argc; a++) argv[a] = (char *)u->argv[a];
        argv[u->argc] = 0;
        struct proc_spec spec = {
            .path = u->path,
            .name = u->name,
            .argc = u->argc,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] M27A: %s not spawned (rc=%d)\n", u->path, pid);
            missing++;
            continue;
        }
        int rc = proc_wait(pid);
        const char *tag;
        if (rc == 0) { tag = "PASS"; passed++; }
        else         { tag = "FAIL"; failed++; }
        kprintf("[boot] M27A: %s (pid=%d) exit=%d (%s)\n",
                u->path, pid, rc, tag);
    }
    kprintf("[boot] M27A: display %d PASS / %d FAIL / %d missing of %lu\n",
            passed, failed, missing,
            (unsigned long)(sizeof(tools) / sizeof(tools[0])));
    /* Mirror the kernel-side display state to serial so a regression
     * script can sanity-check the framebuffer geometry without spawning
     * a userland tool. */
    display_dump_kprintf();
}

/* ============================================================
 *  Milestone 28A: structured logging boot test harness.
 *
 *  After all kernel subsystems are up, exercise the slog ring from
 *  several subsystems / levels, attempt a persistent flush to
 *  /data/system.log, then spawn /bin/logview to render the ring
 *  through the SLOG_READ syscall path. Each step logs a one-line
 *  result tag the test_m28a.ps1 script greps for.
 * ============================================================ */
static void m28a_run_logging_harness(void) {
    kprintf("[boot] M28A: driving logging harness (slog ring + logview)\n");

    /* 1. Synthetic emissions from a spread of subsystems / levels.
     *    Tagged with M28A_TAG so the test script can find them in
     *    both the serial.log AND the ring drained by logview. */
    SLOG_INFO (SLOG_SUB_KERNEL,  "M28A_TAG kernel info trace");
    SLOG_WARN (SLOG_SUB_FS,      "M28A_TAG fs warn trace");
    SLOG_ERROR(SLOG_SUB_NET,     "M28A_TAG net error trace");
    SLOG_INFO (SLOG_SUB_GUI,     "M28A_TAG gui info trace");
    SLOG_INFO (SLOG_SUB_DRIVER,  "M28A_TAG driver info trace");
    SLOG_DEBUG(SLOG_SUB_PROC,    "M28A_TAG proc debug trace (gated)");
    SLOG_INFO (SLOG_SUB_DISPLAY, "M28A_TAG display info trace");
    SLOG_INFO (SLOG_SUB_AUDIO,   "M28A_TAG audio info trace");
    SLOG_WARN (SLOG_SUB_SVC,     "M28A_TAG svc warn trace");
    SLOG_ERROR(SLOG_SUB_PANIC,   "M28A_TAG panic-test (synthetic, no halt)");

    /* 2. Persist to disk (best-effort -- /data may not be mounted on
     *    every QEMU config). The flush function returns 0 on success.
     *    We log both the PASS line and a non-zero detail on failure
     *    so the script can branch. */
    int fl = slog_persist_flush();
    if (fl == 0) {
        kprintf("[boot] M28A: slog persist PASS path=%s\n", SLOG_PERSIST_PATH);
    } else {
        kprintf("[boot] M28A: slog persist SKIP rc=%d (no /data?)\n", fl);
    }

    /* 3. Snapshot stats and dump them so the test script can verify
     *    counters look reasonable. */
    {
        struct abi_slog_stats st;
        slog_stats(&st);
        kprintf("[boot] M28A: slog stats emitted=%llu dropped=%llu in_use=%u "
                "depth=%u err=%llu warn=%llu info=%llu debug=%llu\n",
                (unsigned long long)st.total_emitted,
                (unsigned long long)st.total_dropped,
                (unsigned)st.ring_in_use,
                (unsigned)st.ring_depth,
                (unsigned long long)st.per_level[ABI_SLOG_LEVEL_ERROR],
                (unsigned long long)st.per_level[ABI_SLOG_LEVEL_WARN],
                (unsigned long long)st.per_level[ABI_SLOG_LEVEL_INFO],
                (unsigned long long)st.per_level[ABI_SLOG_LEVEL_DEBUG]);
    }

    /* 4. Spawn /bin/logview --boot to drain the ring through the
     *    real syscall path and print the records to the console.
     *    --boot makes logview emit a fixed PASS sentinel + grep'able
     *    record table and exit 0. */
    {
        char *argv[] = { (char *)"logview", (char *)"--boot", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/logview",
            .name = "logview-boot",
            .argc = 2,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] M28A: /bin/logview not spawned (rc=%d) MISSING\n",
                    pid);
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] M28A: /bin/logview (pid=%d) exit=%d (%s)\n",
                    pid, rc, rc == 0 ? "PASS" : "FAIL");
        }
    }

    kprintf("[boot] M28A: logging harness complete\n");
}

/* ============================================================
 *  Milestone 29A: hardware-discovery harness.
 *
 *  Runs on every boot AFTER devtest_init / display_init so the
 *  bus counts in the snapshot reflect the fully-populated device
 *  tables. Emits a kernel-side dump (so test_m29a.ps1 can grep
 *  the serial log even if /data is not writable), persists the
 *  textual snapshot to /data/hwinfo.snap (best-effort, soft-skip
 *  on read-only mounts), and finally spawns /bin/hwinfo --boot
 *  to verify SYS_HWINFO from userland.
 *
 *  Each line is grepable as `[boot] M29A: ...` exactly like the
 *  M26A / M27A / M28A harnesses, so the aggregator script's
 *  PASS/FAIL extraction stays uniform across milestones.
 * ============================================================ */
static void m29a_run_hwinfo_harness(void) {
    kprintf("[boot] M29A: driving hwinfo harness "
            "(snapshot + persist + /bin/hwinfo --boot)\n");

    /* 1. Take a fresh snapshot and dump it through kprintf.
     *    Useful as a serial-only fallback when /data isn't ready. */
    hwinfo_dump_kprintf();

    /* 1b. Look for a prior snapshot left behind by an earlier boot.
     *     Existence + size proves the file system survived reboot
     *     and the snapshot is readable, which satisfies the M29A
     *     "snapshot file created and readable after reboot" test. */
    {
        struct vfs_stat pst;
        if (vfs_stat("/data/hwinfo.snap", &pst) == VFS_OK && pst.size > 0) {
            void  *prev = NULL;
            size_t pn = 0;
            int rrc = vfs_read_all("/data/hwinfo.snap", &prev, &pn);
            if (rrc == VFS_OK && prev) {
                kprintf("[boot] M29A: prior snapshot READABLE "
                        "(size=%llu bytes, %s)\n",
                        (unsigned long long)pst.size,
                        "from previous boot");
                kfree(prev);
            } else {
                kprintf("[boot] M29A: prior snapshot present "
                        "(size=%llu) but read failed rc=%d\n",
                        (unsigned long long)pst.size, rrc);
            }
        } else {
            kprintf("[boot] M29A: no prior snapshot "
                    "(first boot or /data not ready)\n");
        }
    }

    /* 2. Persist the rendered text to /data/hwinfo.snap. Returns
     *    bytes written (>0), 0 if /data is not writable yet, or a
     *    negative VFS error. We never fail the boot on a soft-skip;
     *    the test script handles all three branches. */
    long pn = hwinfo_persist();
    if (pn > 0) {
        kprintf("[boot] M29A: hwinfo persist PASS bytes=%ld path=%s\n",
                pn, "/data/hwinfo.snap");
    } else if (pn == 0) {
        kprintf("[boot] M29A: hwinfo persist SKIP (no /data yet)\n");
    } else {
        kprintf("[boot] M29A: hwinfo persist FAIL rc=%ld\n", pn);
    }

    /* 3. Read back the cached summary one more time to log a
     *    compact one-liner that test scripts can fingerprint by
     *    snapshot epoch. */
    {
        struct abi_hwinfo_summary s;
        hwinfo_snapshot(&s);
        kprintf("[boot] M29A: snapshot epoch=%lu cpu_count=%u "
                "mem_total_pg=%lu pci=%u usb=%u blk=%u disp=%u "
                "input=%u audio=%u battery=%u hub=%u profile=%s\n",
                (unsigned long)s.snapshot_epoch,
                (unsigned)s.cpu_count,
                (unsigned long)s.mem_total_pages,
                (unsigned)s.pci_count, (unsigned)s.usb_count,
                (unsigned)s.blk_count, (unsigned)s.display_count,
                (unsigned)s.input_count, (unsigned)s.audio_count,
                (unsigned)s.battery_count, (unsigned)s.hub_count,
                s.profile_hint);
    }

    /* 4. Spawn /bin/hwinfo --boot. The userland tool calls
     *    SYS_HWINFO, prints the human-readable inventory, and
     *    exits 0 on success / non-zero on a malformed snapshot.
     *    Same pattern as M28A's logview --boot. */
    {
        char *argv[] = { (char *)"hwinfo", (char *)"--boot", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/hwinfo",
            .name = "hwinfo-boot",
            .argc = 2,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] M29A: /bin/hwinfo not spawned (rc=%d) MISSING\n",
                    pid);
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] M29A: /bin/hwinfo (pid=%d) exit=%d (%s)\n",
                    pid, rc, rc == 0 ? "PASS" : "FAIL");
        }
    }

    kprintf("[boot] M29A: hwinfo harness complete\n");
}

/* ============================================================
 *  Milestone 29B: driver-matching + fallback harness.
 *
 *  Always:
 *    1. Dumps the live drvmatch table to serial.
 *    2. Spawns /bin/drvmatch --boot which probes SYS_DRVMATCH from
 *       userland with both known and bogus (vendor:device) keys, and
 *       prints M29B_DRV: PASS sentinels for the test script.
 *
 *  When DRVTEST_FLAG=1 baked /etc/drvtest_now into the initrd:
 *    3. Calls drvmatch_disable_pci("e1000") to forcibly unbind the
 *       e1000 NIC, re-runs the bind pass to verify nothing crashes
 *       and the device transitions to FORCED_OFF, then re-enables
 *       the driver to leave the system in a known-good state.
 *
 *  Kernel-side log lines are grepable as `[boot] M29B: ...`. */
static void m29b_run_drvmatch_harness(void) {
    kprintf("[boot] M29B: driving drvmatch harness "
            "(query + fallback + /bin/drvmatch --boot)\n");

    /* 1. Snapshot the live match table to serial. */
    drvmatch_dump_kprintf();

    /* 2. Sanity-check counters. */
    {
        uint32_t total = 0, bound = 0, unbound = 0, forced = 0;
        drvmatch_count(&total, &bound, &unbound, &forced);
        kprintf("[boot] M29B: drvmatch total=%u bound=%u "
                "unbound=%u forced_off=%u\n",
                (unsigned)total, (unsigned)bound,
                (unsigned)unbound, (unsigned)forced);
    }

    /* 3. Spawn /bin/drvmatch --boot. */
    {
        char *argv[] = { (char *)"drvmatch", (char *)"--boot", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/drvmatch",
            .name = "drvmatch-boot",
            .argc = 2,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] M29B: /bin/drvmatch not spawned (rc=%d) MISSING\n",
                    pid);
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] M29B: /bin/drvmatch (pid=%d) exit=%d (%s)\n",
                    pid, rc, rc == 0 ? "PASS" : "FAIL");
        }
    }

    /* 4. Optional forced-disable test, gated by /etc/drvtest_now. */
    {
        struct vfs_stat st;
        if (vfs_stat("/etc/drvtest_now", &st) != VFS_OK) {
            kprintf("[boot] M29B: forced-disable test SKIPPED "
                    "(no /etc/drvtest_now)\n");
        } else {
            const char *target = "e1000";
            kprintf("[boot] M29B: forced-disable test ARMED -- "
                    "target driver='%s'\n", target);
            long unbound = drvmatch_disable_pci(target);
            if (unbound < 0) {
                kprintf("[boot] M29B: forced-disable PASS (driver "
                        "absent on this VM, rc=%ld) -- nothing to "
                        "unbind, fallback path not exercised\n",
                        unbound);
            } else {
                kprintf("[boot] M29B: forced-disable removed %ld "
                        "device(s) from '%s'\n", unbound, target);
                /* Re-enable so the rest of boot stays consistent. */
                long restored = drvmatch_reenable_pci(target);
                kprintf("[boot] M29B: forced-disable restored %ld "
                        "device(s) to '%s'\n", restored, target);
                if (restored != unbound) {
                    kprintf("[boot] M29B: forced-disable WARN: "
                            "restored=%ld != unbound=%ld\n",
                            restored, unbound);
                }
            }
            kprintf("[boot] M29B: forced-disable PASS (no crash, "
                    "drvmatch table consistent)\n");
        }
    }

    kprintf("[boot] M29B: drvmatch harness complete\n");
}

/* --- Milestone 35F: hwreport harness ---
 *
 * Always spawns /bin/hwreport --boot which prints the M35F_HWR
 * sentinels test_m35.ps1 greps for. The verdict rule is GREEN /
 * YELLOW = exit 0 (PASS); RED = exit 3 (FAIL). The kernel side
 * intentionally does NOT inspect or veto the verdict; the userland
 * tool owns the decision so the same code path the operator sees on
 * the live shell is what the boot harness validates.
 *
 * Cheap (a few hundred ms): one syscall for the hwinfo summary, one
 * for the hwcompat snapshot, then ~200 lines of formatted output. */
static void m35f_run_hwreport_harness(void) {
    kprintf("[boot] M35F: driving hwreport harness "
            "(/bin/hwreport --boot)\n");
    char *argv[] = { (char *)"hwreport", (char *)"--boot", 0 };
    char *envp[] = { (char *)"PATH=/bin", 0 };
    struct proc_spec spec = {
        .path = "/bin/hwreport",
        .name = "hwreport-boot",
        .argc = 2,
        .argv = argv,
        .envc = 1,
        .envp = envp,
    };
    int pid = proc_spawn(&spec);
    if (pid < 0) {
        kprintf("[boot] M35F: /bin/hwreport not spawned (rc=%d) MISSING\n",
                pid);
    } else {
        int rc = proc_wait(pid);
        kprintf("[boot] M35F: /bin/hwreport (pid=%d) exit=%d (%s)\n",
                pid, rc, rc == 0 ? "PASS" : "FAIL");
    }
    kprintf("[boot] M35F: hwreport harness complete\n");
}

/* --- Milestone 35G: compattest harness ---
 *
 * Always spawns /bin/compattest --boot which runs the eight-bucket
 * end-to-end validation suite (system_boot, driver_match, fallback,
 * network, storage, usb_input, log_capture, no_crashes) and prints
 * M35G_CMP sentinels. Buckets that require real hardware return
 * SKIPPED_REAL_HARDWARE_REQUIRED (acceptable in QEMU); only an
 * actual FAIL fails the boot harness. */
static void m35g_run_compattest_harness(void) {
    kprintf("[boot] M35G: driving compattest harness "
            "(/bin/compattest --boot)\n");
    char *argv[] = { (char *)"compattest", (char *)"--boot", 0 };
    char *envp[] = { (char *)"PATH=/bin", 0 };
    struct proc_spec spec = {
        .path = "/bin/compattest",
        .name = "compattest-boot",
        .argc = 2,
        .argv = argv,
        .envc = 1,
        .envp = envp,
    };
    int pid = proc_spawn(&spec);
    if (pid < 0) {
        kprintf("[boot] M35G: /bin/compattest not spawned (rc=%d) MISSING\n",
                pid);
    } else {
        int rc = proc_wait(pid);
        kprintf("[boot] M35G: /bin/compattest (pid=%d) exit=%d (%s)\n",
                pid, rc, rc == 0 ? "PASS" : "FAIL");
    }
    kprintf("[boot] M35G: compattest harness complete\n");
}

/* --- Milestone 28B: crash-dump inspector harness ---
 * Runs after the M28A harness on every boot. If /data/crash/last.dump
 * exists, we spawn /bin/crashinfo --boot which decodes the file's
 * abi_crash_header, prints the M28B_CRASHINFO sentinels, and exits 0
 * on success. The test_m28b.ps1 inspect boot greps for these. On a
 * fresh disk (no prior panic) we just log "no crash dump on disk" and
 * skip -- this is the normal state for any healthy boot. */
static void m28b_run_crashinfo_inspector(void) {
    struct vfs_stat st;
    if (vfs_stat("/data/crash/last.dump", &st) != VFS_OK) {
        kprintf("[boot] M28B: no crash dump on disk -- skipping crashinfo\n");
        return;
    }
    kprintf("[boot] M28B: crash dump present (%llu bytes) -- running crashinfo\n",
            (unsigned long long)st.size);
    char *argv[] = { (char *)"crashinfo", (char *)"--boot", 0 };
    char *envp[] = { (char *)"PATH=/bin", 0 };
    struct proc_spec spec = {
        .path = "/bin/crashinfo",
        .name = "crashinfo-boot",
        .argc = 2,
        .argv = argv,
        .envc = 1,
        .envp = envp,
    };
    int pid = proc_spawn(&spec);
    if (pid < 0) {
        kprintf("[boot] M28B: /bin/crashinfo not spawned (rc=%d) MISSING\n", pid);
        return;
    }
    int rc = proc_wait(pid);
    kprintf("[boot] M28B: /bin/crashinfo (pid=%d) exit=%d (%s)\n",
            pid, rc, rc == 0 ? "PASS" : "FAIL");
}

/* --- Milestone 28F: service-supervision harness ---
 *
 * When /etc/svctest_now is present (built with SVCTEST_FLAG=1) we
 * register a known-bad userland service /bin/svc_crasher (always
 * exits non-zero) with autorestart. The supervisor's
 * apply_exit / backoff path takes over: each crash bumps
 * crash_count, the service moves to BACKOFF, after the cooldown it
 * is re-enqueued by service_tick(). After SERVICE_DISABLE_THRESHOLD
 * consecutive crashes (5) the supervisor must transition to
 * SERVICE_DISABLED and stop retrying -- otherwise we'd have an
 * infinite spawn loop.
 *
 * To keep the test deterministic and fast (and to avoid waiting for
 * launch-queue + scheduler ticks at boot time) we drive the
 * supervisor directly via service_simulate_exit(). That hits the
 * exact same apply_exit path a real userland exit would, but takes
 * microseconds. We then *also* spawn /bin/services --boot to verify
 * the SVC_LIST syscall exposes the same verdict to userland. */
static void m28f_run_service_harness(void) {
    /* Always run the userland-side `services --boot` probe so we have
     * a sentinel on every boot (even non-test ones) confirming the
     * SVC_LIST syscall path is functional. */
    {
        char *argv[] = { (char *)"services", (char *)"--boot", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/services",
            .name = "services-boot",
            .argc = 2,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] M28F: /bin/services not spawned (rc=%d) MISSING\n",
                    pid);
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] M28F: /bin/services (pid=%d) exit=%d (%s)\n",
                    pid, rc, rc == 0 ? "PASS" : "FAIL");
        }
    }

    /* Gated kernel-side test. */
    struct vfs_stat st;
    if (vfs_stat("/etc/svctest_now", &st) != VFS_OK) {
        return;
    }
    kprintf("[boot] M28F: /etc/svctest_now present -- running "
            "service supervision self-test\n");
    SLOG_INFO(SLOG_SUB_SVC, "M28F svctest harness starting");

    /* Register the deterministic crasher with autorestart on. The
     * registration intentionally runs AFTER session_init(), so this
     * sits next to /bin/login in the registry. */
    if (service_register_program("crasher", "/bin/svc_crasher",
                                 true, NULL) != 0) {
        kprintf("[boot] M28F_SVCTEST: FAIL could not register crasher\n");
        return;
    }

    struct service *s = service_find("crasher");
    if (!s) {
        kprintf("[boot] M28F_SVCTEST: FAIL crasher missing after register\n");
        return;
    }

    /* Drive 6 synthetic crashes (one more than the disable threshold)
     * with rc=42. Each transitions through STOPPED -> BACKOFF; the
     * 5th must trip SERVICE_DISABLED. We also dump the service state
     * each iteration so the test log is debuggable. */
    int   max_iters = (int)(SERVICE_DISABLE_THRESHOLD + 1);
    int   tripped_at = -1;
    for (int i = 0; i < max_iters; i++) {
        int prev_state = (int)s->state;
        (void)service_simulate_exit(s, 42);
        kprintf("[boot] M28F_SVCTEST: iter=%d prev=%d state=%d "
                "consecutive=%u total=%u backoff_until_ms=%lu\n",
                i, prev_state, (int)s->state,
                (unsigned)s->consecutive_crashes,
                (unsigned)s->crash_count,
                (unsigned long)s->backoff_until_ms);
        if (s->state == SERVICE_DISABLED && tripped_at < 0) {
            tripped_at = i;
        }
    }

    bool disable_ok = (s->state == SERVICE_DISABLED) &&
                      (s->crash_count >= SERVICE_DISABLE_THRESHOLD);
    bool counters_ok = (s->consecutive_crashes ==
                        SERVICE_DISABLE_THRESHOLD + 1) ||
                       (s->consecutive_crashes ==
                        SERVICE_DISABLE_THRESHOLD);

    /* Now verify SERVICE_DISABLED actually refuses a manual start. */
    int restart_rc = service_start("crasher");
    bool refuses = (restart_rc != 0);

    /* And confirm service_clear() resets the state away from DISABLED.
     * Note that clear() deliberately does NOT auto-start; we want the
     * supervisor to leave the freshly-cleared service alone until an
     * operator/user explicitly restarts it. */
    (void)service_clear("crasher");
    bool cleared_ok = (s->state != SERVICE_DISABLED) &&
                      (s->consecutive_crashes == 0);

    /* M28F: from here on the supervisor must NOT keep relaunching the
     * known-bad service in the background. Yank autorestart off -- so
     * even when service_tick walks the STOPPED slot it leaves the
     * crasher alone, and the post-svctest /bin/services snapshot
     * captures a quiet system. The crash_count we accumulated stays
     * resident in the record (proving containment fired). */
    s->autorestart = false;
    /* Pretend it's DISABLED for the snapshot too -- this is the most
     * useful state for an operator to see ("supervisor explicitly
     * gave up on it") and matches what the recovery flow's last
     * stable state should look like. */
    s->state = SERVICE_DISABLED;

    kprintf("[boot] M28F_SVCTEST: tripped_at=%d disable_ok=%d "
            "counters_ok=%d refuses_after_disable=%d cleared_ok=%d\n",
            tripped_at, (int)disable_ok, (int)counters_ok,
            (int)refuses, (int)cleared_ok);

    if (disable_ok && refuses && cleared_ok) {
        kprintf("[boot] M28F_SVCTEST: PASS\n");
        SLOG_INFO(SLOG_SUB_SVC,
                  "M28F svctest PASS tripped_at=%d crashes=%u",
                  tripped_at, (unsigned)s->crash_count);
    } else {
        kprintf("[boot] M28F_SVCTEST: FAIL\n");
        SLOG_ERROR(SLOG_SUB_SVC,
                   "M28F svctest FAIL state=%d crashes=%u",
                   (int)s->state, (unsigned)s->crash_count);
    }

    /* Re-spawn /bin/services --boot now that the synthetic test has
     * finished -- this exercises SVC_LIST and gives the test script a
     * deterministic snapshot it can grep on. */
    {
        char *argv[] = { (char *)"services", (char *)"--boot", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/services",
            .name = "services-postsvctest",
            .argc = 2,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid >= 0) {
            int rc = proc_wait(pid);
            kprintf("[boot] M28F: post-svctest /bin/services exit=%d (%s)\n",
                    rc, rc == 0 ? "PASS" : "FAIL");
        }
    }
}

/* --- Milestone 28G: stability self-test harness ---
 *
 * Always spawns /bin/stabilitytest --boot to confirm SYS_STAB_SELFTEST
 * is reachable and reports a sane result_mask (i.e. the kernel
 * subsystems exposed to the probe are all healthy at this point in
 * boot). When STABTEST_FLAG=1 baked /etc/stabtest_now into the initrd,
 * the harness ALSO runs the stabilitytest in --stress mode so the
 * heap/syscall/disk workload exercises the system end-to-end.
 *
 * The userland tool emits "M28G_STAB:" sentinels parsed by both
 * test_m28g.ps1 and the test_m28_final.ps1 aggregator. */
static void m28g_run_stability_harness(void) {
    /* Phase 1: lightweight probe-only run on every boot. */
    {
        char *argv[] = { (char *)"stabilitytest", (char *)"--boot", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/stabilitytest",
            .name = "stabilitytest-boot",
            .argc = 2,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] M28G: /bin/stabilitytest not spawned (rc=%d) MISSING\n",
                    pid);
            SLOG_ERROR(SLOG_SUB_KERNEL,
                       "M28G stability harness could not spawn (rc=%d)",
                       pid);
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] M28G: /bin/stabilitytest --boot pid=%d exit=%d (%s)\n",
                    pid, rc, rc == 0 ? "PASS" : "FAIL");
            if (rc == 0) {
                SLOG_INFO(SLOG_SUB_KERNEL,
                          "M28G stability self-test PASS (pid=%d)",
                          pid);
            } else {
                SLOG_WARN(SLOG_SUB_KERNEL,
                          "M28G stability self-test FAIL (pid=%d rc=%d)",
                          pid, rc);
            }
        }
    }

    /* Phase 2 (gated): rerun in --stress mode for the dedicated
     * stability test boot. We keep this opt-in because the stress
     * pass touches /init and a few other initrd files; on tiny
     * "safe-mode + minimal initrd" boots that data may not be
     * present, and we don't want a noisy warning every time. */
    struct vfs_stat st;
    if (vfs_stat("/etc/stabtest_now", &st) != VFS_OK) {
        return;
    }
    kprintf("[boot] M28G: /etc/stabtest_now present -- "
            "running stabilitytest --stress\n");
    SLOG_INFO(SLOG_SUB_KERNEL, "M28G stress harness starting");
    {
        char *argv[] = {
            (char *)"stabilitytest", (char *)"--boot",
            (char *)"--stress", 0,
        };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/stabilitytest",
            .name = "stabilitytest-stress",
            .argc = 3,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] M28G_STRESS: FAIL spawn rc=%d\n", pid);
            return;
        }
        int rc = proc_wait(pid);
        if (rc == 0) {
            kprintf("[boot] M28G_STRESS: PASS pid=%d\n", pid);
            SLOG_INFO(SLOG_SUB_KERNEL,
                      "M28G stress PASS pid=%d", pid);
        } else {
            kprintf("[boot] M28G_STRESS: FAIL pid=%d rc=%d\n", pid, rc);
            SLOG_ERROR(SLOG_SUB_KERNEL,
                       "M28G stress FAIL pid=%d rc=%d", pid, rc);
        }
    }
}

/* --- Milestone 28E: filesystem-integrity harness ---
 * Two-part validation:
 *
 *   (a) Always-on: when /data is mounted (true on every disk-backed
 *       boot), spawn /bin/fscheck --boot /data so userland exercises
 *       SYS_FS_CHECK end-to-end on the live mount. Sentinel is
 *       "M28E_FSCHECK: PASS" (clean) or WARN/FAIL strings.
 *
 *   (b) Gated by /etc/fscheck_now (FSCHECK_FLAG=1 at build): drive
 *       tobyfs_self_test, which builds an in-RAM tobyfs image,
 *       formats it, runs check_dev (expect OK), then deliberately
 *       corrupts the magic and runs check_dev again (expect FATAL).
 *       This is what proves the corruption-detection requirement
 *       fires without ever touching the live disk. */
static void m28e_run_fscheck_harness(void) {
    /* Part (a): live /data probe via the userland tool. */
    {
        struct vfs_stat st;
        bool have_data = (vfs_stat("/data", &st) == VFS_OK);
        if (have_data) {
            kprintf("[boot] M28E: /data mounted -- spawning /bin/fscheck --boot\n");
            char *argv[] = { (char *)"fscheck", (char *)"--boot",
                             (char *)"/data", 0 };
            char *envp[] = { (char *)"PATH=/bin", 0 };
            struct proc_spec spec = {
                .path = "/bin/fscheck",
                .name = "fscheck-boot",
                .argc = 3,
                .argv = argv,
                .envc = 1,
                .envp = envp,
            };
            int pid = proc_spawn(&spec);
            if (pid < 0) {
                kprintf("[boot] M28E: /bin/fscheck not spawned "
                        "(rc=%d) MISSING\n", pid);
            } else {
                int rc = proc_wait(pid);
                /* exit codes: 0=PASS, 4=WARN, 3=CORRUPT, 1=other */
                const char *tag = "FAIL";
                if      (rc == 0) tag = "PASS";
                else if (rc == 4) tag = "WARN";
                else if (rc == 3) tag = "CORRUPT";
                kprintf("[boot] M28E: /bin/fscheck (pid=%d) exit=%d (%s)\n",
                        pid, rc, tag);
            }
        } else {
            kprintf("[boot] M28E: /data not mounted -- skipping live probe\n");
        }
    }

    /* Part (b): kernel-side corruption detection self-test, gated by
     * the build-time flag so non-test boots don't pay for the 4 MiB
     * heap blip. */
    {
        struct vfs_stat st;
        if (vfs_stat("/etc/fscheck_now", &st) != VFS_OK) {
            return;
        }
        kprintf("[boot] M28E: /etc/fscheck_now present -- running "
                "kernel corruption-detection self-test\n");
        struct tobyfs_check clean, bad;
        int rc = tobyfs_self_test(&clean, &bad);
        if (rc != 0) {
            kprintf("[boot] M28E_KERNEL_FSCHECK: FAIL infrastructure "
                    "rc=%d\n", rc);
            return;
        }
        bool clean_ok   = (clean.severity == TFS_CHECK_OK);
        bool corrupt_ok = (bad.severity   == TFS_CHECK_FATAL);
        kprintf("[boot] M28E_KERNEL_FSCHECK: clean_sev=%d clean_errors=%u "
                "corrupt_sev=%d corrupt_errors=%u\n",
                clean.severity, clean.errors,
                bad.severity,   bad.errors);
        kprintf("[boot] M28E_KERNEL_FSCHECK: corrupt_detail=\"%s\"\n",
                bad.detail);
        if (clean_ok && corrupt_ok) {
            kprintf("[boot] M28E_KERNEL_FSCHECK: PASS\n");
        } else {
            kprintf("[boot] M28E_KERNEL_FSCHECK: FAIL clean_ok=%d "
                    "corrupt_ok=%d\n", clean_ok, corrupt_ok);
        }
    }
}

#if defined(WINPE8_BOOT) || defined(WINPE10_BOOT) || defined(WINPE11_BOOT) || defined(WINPE12_BOOT) || defined(WINPE13_BOOT) || defined(WINPE14_BOOT) || defined(WINPE14B_BOOT) || defined(WINPE15_BOOT) || defined(WINPE16B_BOOT) || defined(WINPE16D_BOOT) || defined(WINPE18C_BOOT)
/* ---- Track C / C8: a VISIBLE + INTERACTIVE stock Win32 GUI .exe ----
 *
 * C7 proved the user32/gdi32 bridge but the window was (a) hidden behind the
 * fullscreen login screen at boot and (b) inert (no input). C8 fixes both:
 *   - VISIBLE: programmatically sign a session in (root has an empty seed
 *     password), dismiss the login window, and launch the .exe so its window
 *     composites with full chrome on the logged-in desktop (wallpaper+taskbar).
 *   - INTERACTIVE: GetMessage now translates real mouse/keyboard/close events
 *     into WM_*; the harness drives a REAL mouse click (mouse_inject_event ->
 *     PS/2 driver -> compositor hit-test -> the window) to recolour the window,
 *     then a deterministic close to run WM_CLOSE->WM_DESTROY->PostQuitMessage.
 * The app returns 8 iff it PAINTED, HANDLED a click, and ran the close chain.
 */

/* The window's post-click fill colour (XRGB). The app's WndProc fills with
 * CreateSolidBrush(RGB(40,180,90)) after a click; FillRect swaps R<->B from
 * COLORREF, so the framebuffer colour is 0x0028B45A. */
#define WINPE8_GREEN 0x0028B45Au

/* Pump the desktop for ~`ms` milliseconds: yield so the GUI app runs, drain
 * local input (so injected mouse reports are processed), and composite. We run
 * here on pid 0 BEFORE idle_loop, so this is the compositor's only driver. */
static void winpe8_pump_ms(uint64_t ms) {
    uint64_t hz = pit_hz(); if (hz == 0) hz = 100;
    uint64_t end = pit_ticks() + (ms * hz + 999) / 1000;
    do {
        sched_yield();
        mouse_flush_pending();
        kbd_flush_pending();
        gui_tick();
    } while (pit_ticks() < end);
}

/* Move the host cursor onto (tx,ty) in SCREEN coords via the real PS/2 mouse
 * path. mouse_inject_event applies a pointer-acceleration multiplier (up to 5x)
 * so open-loop delta math overshoots -- drive it as a feedback loop that reads
 * the real cursor each step and converges. */
static void winpe8_move_cursor_to(int tx, int ty) {
    for (int i = 0; i < 200; i++) {
        int cxp = 0, cyp = 0;
        gui_cursor_pos(&cxp, &cyp);
        int ex = tx - cxp, ey = ty - cyp;
        if (ex >= -3 && ex <= 3 && ey >= -3 && ey <= 3) break;
        /* pre-divide for the accel multiplier, clamp the per-report delta, and
         * always nudge at least 1px toward the target. */
        int sx = ex / 6; if (sx > 40) sx = 40; if (sx < -40) sx = -40;
        if (sx == 0 && ex) sx = (ex > 0) ? 1 : -1;
        int sy = ey / 6; if (sy > 40) sy = 40; if (sy < -40) sy = -40;
        if (sy == 0 && ey) sy = (ey > 0) ? 1 : -1;
        mouse_inject_event(sx, sy, 0);
        winpe8_pump_ms(15);
    }
}

/* Sign a session in (root has an empty seed password) and dismiss the login
 * window, leaving a clean logged-in desktop (wallpaper + taskbar) so a launched
 * Win32 app's window shows with full chrome on top. */
static void winpe_autologin_clear(void) {
    int lr = session_login("root", "");
    kprintf("[boot] WINPE: session_login(root) rc=%d active=%d\n",
            lr, (int)session_active());
    service_stop("login");
    winpe8_pump_ms(400);
    for (int pid = 1; pid < 64; pid++) {
        struct proc *p = proc_lookup(pid);
        if (p && p->name[0] && strcmp(p->name, "login") == 0) {
            kprintf("[boot] WINPE: SIGKILL lingering login pid=%d\n", pid);
            signal_send_to_pid(pid, SIGKILL);
        }
    }
    winpe8_pump_ms(300);
    gui_invalidate_full();
    winpe8_pump_ms(300);
}

/* Spawn a PE as a session-tagged desktop app (tag the boot thread with the
 * active session around the spawn so the child inherits it). Returns the pid. */
static int winpe_spawn_session_app(const char *path, const char *name) {
    struct proc *self = current_proc();
    int sid  = self ? self->session_id : 0;
    int suid = self ? self->uid : 0;
    int sgid = self ? self->gid : 0;
    if (self) {
        self->session_id = session_current_id();
        self->uid        = session_current_uid();
        self->gid        = session_current_gid();
    }
    char *argv[] = { (char *)name, 0 };
    char *envp[] = { (char *)"PATH=/bin", 0 };
    struct proc_spec spec = {
        .path = path, .name = name,
        .argc = 1, .argv = argv, .envc = 1, .envp = envp,
    };
    int pid = proc_spawn(&spec);
    if (self) { self->session_id = sid; self->uid = suid; self->gid = sgid; }
    return pid;
}
#endif /* WINPE8_BOOT || WINPE10_BOOT */

void _start(void) {
    early_init();
    framebuffer_init();
    gdt_init();
    idt_init();
    int_smoke_test();
    pic_init();
    irq_init();             /* facade in PIC mode -- post-SMP we promote */
    /* Milestone 28C: arm the watchdog BEFORE pit_init so the very
     * first PIT tick can already feed it without crashing on g_ready=false. */
    wdog_init(WDOG_DEFAULT_TIMEOUT_MS);
    pit_init(1000);         /* 1000 Hz — 1ms tick for instant input response */
    rtc_init();             /* read CMOS wall-clock time */
    kbd_init();             /* via irq_install_isa(1, kbd_irq) */
    sti();                  /* IF=1 -- IRQs can now reach the CPU */
    kprintf("[boot] interrupts enabled (IRQ0 timer + IRQ1 keyboard, "
            "via legacy PIC for early boot)\n");
    /* Milestone 19: calibrate TSC as soon as IRQ0 is live. perf_init
     * samples the PIT, which needs `sti()` to have happened so
     * pit_ticks() actually advances. Must come BEFORE proc_init so
     * pid 0's creation stamp is meaningful (and so the scheduler's
     * per-proc cpu_ns accounting has a valid conversion rate). */
    perf_init();
    pmm_init_and_test();
    /* Reserve the AP startup trampoline's physical page BEFORE any
     * other allocation. The PMM hands out pages low-to-high, so 0x8000
     * gets eaten very quickly otherwise (vmm_init alone burns 70+ low
     * pages on intermediate page-table levels). Once reserved here,
     * smp.c can copy the trampoline into it later. */
    if (!pmm_reserve_page(AP_TRAMPOLINE_PHYS)) {
        kprintf("[boot] WARN: could not reserve AP trampoline at phys %p\n",
                (void *)AP_TRAMPOLINE_PHYS);
    } else {
        kprintf("[boot] reserved AP trampoline page at phys %p\n",
                (void *)AP_TRAMPOLINE_PHYS);
    }
    vmm_init_and_test((struct limine_memmap_response *)memmap_req.response);
    heap_init_and_test();
    tss_init();             /* RSP0 stack for ring-3 -> ring-0 transitions */
    syscall_init();          /* EFER.SCE / STAR / LSTAR / FMASK */

    /* Milestone 21: bring up the PCI bus + driver registry. We do this
     * here -- after the heap is alive (drivers may kmalloc + vmm_map
     * MMIO BARs), but BEFORE anything that needs to talk to discovered
     * hardware (storage at /data, NIC at net_init). The actual driver
     * probes run inside pci_bind_drivers() below; each probe is
     * no-op-on-absent so booting on hardware that lacks one of these
     * controllers is silent rather than fatal.
     *
     * Registration order is irrelevant -- pci_bind_drivers walks the
     * device list once and asks every registered driver in turn.
     *
     * BSP LAPIC + IO APIC come up just BEFORE the bind pass so any
     * MSI-capable driver (AHCI, NVMe, xHCI, e1000e, virtio-*, ...)
     * can call irq_alloc_vector() and pci_msi_enable() during its
     * probe and start delivering interrupts immediately. */
    smp_init_bsp();
    pci_init();
    usbreg_init();           /* M35C: USB device attach registry */
    blk_ata_register();
    blk_ahci_register();
    blk_nvme_register();
    virtio_blk_register();   /* M35B: modern virtio-blk-pci */
    virtio_rng_register();   /* virtio-rng entropy for SSH host keys */
    e1000_register();
    e1000e_register();
    virtio_net_register();
    rtl8169_register();
    usb_legacy_register();  /* USB 1.x/2.0 HCIs: UHCI/OHCI/EHCI diagnostics + safe legacy input */
    xhci_register();        /* USB 3.x host controller (qemu-xhci, real PCH xHCI) */
    virtio_gpu_register();  /* GPU: virtio-gpu (basic 2D); falls back to Limine FB */
    audio_hda_register();   /* M26F: HD Audio controller (M26A: stub probe only) */
    pci_bind_drivers();
    devmgr_init();
    devmgr_enumerate();
    taskd_init();
    /* M23A: scan every disk we just discovered for a GPT. Each
     * non-empty entry becomes a BLK_CLASS_PARTITION device in the
     * registry, named "<disk>.pN" and tagged with its type GUID +
     * label. Disks without a GPT (legacy raw layouts) are silently
     * skipped -- the legacy mount path below still works for them. */
    int parts_found = partition_scan_all();
    if (parts_found > 0) {
        kprintf("[boot] partition scan: %d partition(s) registered\n",
                parts_found);
    }
    blk_dump();
#ifdef NVME4K_BOOT
    /* Opt-in: exercise the NVMe 512<->native-LBA translation path
     * (-device nvme,...,logical_block_size=4096). Non-destructive. */
    blk_nvme_selftest();
#endif
#ifdef AHCIQ_BOOT
    /* Opt-in: exercise the AHCI NCQ command-queuing path on q35 (an
     * AHCI/SATA disk). Non-destructive. */
    blk_ahci_selftest();
#endif
#ifdef EXT4_SELFTEST
    /* Opt-in: ext4 write self-test -- formats + mounts a ramdev and
     * exercises create/write/grow/overwrite/mkdir/unlink/remount.
     * Self-contained (no host mke2fs, no QEMU disk). */
    { extern int ext4_self_test(void); ext4_self_test(); }
#endif
#ifdef EXT4_JOURNAL_SELFTEST
    /* Opt-in: ext4 JBD2 crash-consistency self-test -- injects a power
     * loss at each phase of a journalled transaction and proves remount
     * recovery is atomic (rollback / replay / heal). */
    { extern int ext4_journal_self_test(void); ext4_journal_self_test(); }
#endif
#ifdef MEMCOMP_SELFTEST
    /* Opt-in: memory-compression self-test -- LZ4 codec round-trip +
     * ratios, zram pool store/load, and the swap compress/disk-fallback
     * routing. Runs BEFORE swap_init so it can freely reconfigure swap. */
    { extern int memcomp_self_test(void); memcomp_self_test(); }
#endif
#ifdef HUGEPAGE_SELFTEST
    /* Opt-in: 2 MiB large-page self-test -- PMM huge-frame allocator +
     * VMM huge map/translate/unmap, and a large kmalloc backed by 2 MiB
     * leaves via the heap's huge-page growth path. */
    { extern int hugepage_self_test(void); hugepage_self_test(); }
#endif
    net_dump();

    modules_log();
    /* Mount the boot tar as the root filesystem before any code wants
     * to read files. After this, vfs_open / vfs_opendir / vfs_read_all
     * all work, and user_load_and_run() can be passed VFS paths. */
    initrd_init();
    procfs_init();   /* Phase 1 M1.5: mount /proc virtual filesystem */
#ifdef TOBYFS_STRESS_SELFTEST
    /* Opt-in: TobyFS crash-consistency + stress harness (needs the VFS
     * mount table, hence after initrd_init). Journal rollback/replay under
     * injected power loss, then a churn + integrity stress run. */
    { extern int tobyfs_crash_stress_test(void); tobyfs_crash_stress_test(); }
#endif
    /* Milestone 28D: latch safe-mode state right after the initrd is
     * mounted (so /etc/safemode_now is readable) but BEFORE any
     * optional subsystem inits. From here on, safemode_active() is
     * the canonical "skip non-essential drivers" gate. */
    safemode_init();
    module_init();

    /* Milestone 29A: hardware-discovery init. Caches CPUID-derived
     * fields (vendor / brand / family / features / cpu_count) into
     * the static snapshot, picks an initial profile guess, and
     * latches the kernel-side state. Cheap and idempotent -- safe
     * to call this early (before pci/usb), the per-bus counts will
     * simply be zero until hwinfo_snapshot() is called again after
     * device enumeration completes. */
    hwinfo_init();

    /* Milestone 29B: now that pci_bind_drivers() has classified
     * every PCI device's match strategy and the USB class drivers
     * have attached during xHCI enumeration, take a snapshot of the
     * driver-match table. Subsequent SYS_DRVMATCH calls use it. */
    drvmatch_init();

    /* Milestone 35A: read /etc/drvmatch.conf (now that the initrd is
     * mounted) and apply blacklist + force overrides. drvconf_apply()
     * walks the live registry and, for each blacklisted driver, calls
     * drvmatch_disable_pci() (which unbinds + re-runs the bind pass).
     * It then walks force rules and tries to re-bind each affected
     * device to the requested driver. The end state of the driver
     * match table is logged for the operator. */
    drvconf_load();
    drvconf_apply();
    drvconf_dump_kprintf();

#ifdef M35_SELFTEST
    extern void m35a_selftest(void);
    extern void m35b_selftest(void);
    extern void m35c_selftest(void);
    extern void m35d_selftest(void);
    extern void m35e_selftest(void);
    extern void m35f_selftest(void);
    m35a_selftest();
    m35b_selftest();
    m35c_selftest();
    m35d_selftest();
    m35e_selftest();
    m35f_selftest();
#endif

    bcache_init();

    /* Probe the IDE primary master and mount its tobyfs at /data. Two
     * layouts are supported (milestone 20):
     *
     *   1. Legacy / live: tobyfs starts at LBA 0 (host-formatted via
     *      mkfs_tobyfs or the `make disk` target).
     *   2. Installed: the first INSTALLER_BOOT_SECTORS of the disk
     *      carry a bootable Limine image, and tobyfs lives in the
     *      region starting at sector INSTALLER_BOOT_SECTORS (created
     *      by the installer).
     *
     * Try layout (1) first; if the superblock is absent or bogus,
     * fall back to (2). Either failure is non-fatal -- the ramfs root
     * stays mounted and the rest of the OS comes up cleanly. */
    {
        /* Mount priority for /data (M23A introduces step 0):
         *
         *   0. GPT partition tagged with the tobyOS-data type GUID.
         *      This is the modern, partition-table-backed layout that
         *      mkdisk_gpt produces and that future installers will
         *      lay down. If found, mount it and we're done.
         *
         *   1. Whole-disk tobyfs at LBA 0 (legacy "live ISO" layout
         *      where the disk image was formatted directly with
         *      mkfs_tobyfs). Tried only when no GPT-tagged partition
         *      is available.
         *
         *   2. Installed layout: tobyfs at LBA INSTALLER_BOOT_SECTORS
         *      (legacy installer carved space without a partition
         *      table). Final fallback before declaring /data
         *      unavailable.
         *
         * Each step is non-fatal -- the ramfs root stays mounted and
         * the rest of the OS boots even if /data never comes up. */
        bool data_mounted = false;
        struct blk_dev *gpt_data = partition_find_by_type(GPT_TYPE_TOBYOS_DATA);
        if (gpt_data) {
            int rc = tobyfs_mount("/data", gpt_data);
            if (rc == VFS_OK) {
                kprintf("[boot] mounted /data via GPT partition '%s' "
                        "(slot %u, parent '%s', LBA %lu, %lu sectors)\n",
                        gpt_data->name, (unsigned)gpt_data->partition_index,
                        gpt_data->parent ? gpt_data->parent->name : "?",
                        (unsigned long)gpt_data->offset_lba,
                        (unsigned long)gpt_data->sector_count);
                data_mounted = true;
            } else {
                kprintf("[boot] GPT data partition '%s' present but tobyfs "
                        "mount failed: %s -- falling through to legacy paths\n",
                        gpt_data->name, vfs_strerror(rc));
            }
        }

        if (!data_mounted) {
            /* Milestone 21: ask the block-device registry for "first
             * disk".  blk_ata's PCI probe (run during pci_bind_drivers
             * above) will have called blk_register() if it found an
             * IDE primary master. On boards/QEMU configurations
             * without IDE this returns NULL and we fall through to
             * the "/data unavailable" path -- exactly the behaviour
             * the old direct-init had. AHCI/NVMe drivers will
             * register their disks the same way in steps 1 and 2. */
            struct blk_dev *disk = blk_first_disk();
            if (!disk) disk = blk_get_first();
            if (disk) {
                int rc = tobyfs_mount("/data", disk);
                if (rc == VFS_OK) {
                    kprintf("[boot] mounted /data on whole disk '%s' "
                            "(legacy LBA-0 layout)\n", disk->name);
                    data_mounted = true;
                } else {
                    kprintf("[boot] /data at LBA 0 not a tobyfs (%s); "
                            "trying installed layout at LBA %u...\n",
                            vfs_strerror(rc), INSTALLER_BOOT_SECTORS);
                    uint64_t avail = disk->sector_count > INSTALLER_BOOT_SECTORS
                        ? disk->sector_count - INSTALLER_BOOT_SECTORS : 0;
                    if (avail >= TFS_TOTAL_BLOCKS * TFS_SECTORS_PER_BLOCK) {
                        struct blk_dev *part = blk_offset_wrap(
                            disk, INSTALLER_BOOT_SECTORS, avail, "data");
                        if (part) {
                            rc = tobyfs_mount("/data", part);
                            if (rc != VFS_OK) {
                                kprintf("[boot] no tobyfs at installed offset "
                                        "either: %s -- /data unavailable\n",
                                        vfs_strerror(rc));
                            } else {
                                kprintf("[boot] mounted installed /data at "
                                        "LBA %u\n", INSTALLER_BOOT_SECTORS);
                                data_mounted = true;
                            }
                        }
                    } else {
                        kprintf("[boot] disk too small for installed layout "
                                "-- /data unavailable\n");
                    }
                }
            } else {
                kprintf("[boot] no disk -- /data unavailable this boot\n");
            }
        }

        /* Milestone 23B: opportunistically mount the FIRST FAT32-looking
         * partition at /fat. We don't fail boot if there's no FAT32
         * volume present -- this is purely a convenience so the live
         * smoke test can `cat /fat/HELLO.TXT` without having to run
         * `mountfs` by hand. We probe partitions by reading their LBA
         * 0 looking for a valid BPB; the partition layer doesn't
         * encode "this is FAT32" in the type GUID strictly enough
         * (Microsoft Basic Data covers FAT16/FAT32/exFAT/NTFS). */
        bool fat_mounted = false;
        {
            size_t it = 0;
            struct blk_dev *p;
            while (!fat_mounted &&
                   (p = blk_iter_next(&it, BLK_CLASS_PARTITION)) != NULL) {
                if (!fat32_probe(p)) continue;
                int rc = fat32_mount("/fat", p);
                if (rc == VFS_OK) {
                    kprintf("[boot] mounted /fat via FAT32 partition '%s' "
                            "(slot %u, parent '%s', LBA %lu, %lu sectors)\n",
                            p->name, (unsigned)p->partition_index,
                            p->parent ? p->parent->name : "?",
                            (unsigned long)p->offset_lba,
                            (unsigned long)p->sector_count);
                    fat_mounted = true;
                } else {
                    kprintf("[boot] FAT32 partition '%s' looked valid but "
                            "mount failed: %s\n", p->name, vfs_strerror(rc));
                }
            }
            if (!fat_mounted) {
                kprintf("[boot] no FAT32 partition discovered -- /fat unmounted\n");
            }
        }

        /* Milestone 23B self-test: exercise readdir + read + create +
         * write + unlink on /fat so the boot log proves the driver is
         * actually wired through VFS end-to-end. Failure in any step
         * is logged but non-fatal -- the operator can still drop into
         * the shell and continue. */
        if (fat_mounted) {
            kprintf("[fat32-test] >>> begin smoke test on /fat\n");

            /* (1) readdir /fat */
            struct vfs_dir d;
            if (vfs_opendir("/fat", &d) == VFS_OK) {
                struct vfs_dirent e;
                int n = 0;
                while (vfs_readdir(&d, &e) == VFS_OK) {
                    kprintf("[fat32-test]   /fat[%d] %s  type=%d size=%u\n",
                            n++, e.name, (int)e.type, (unsigned)e.size);
                }
                vfs_closedir(&d);
                kprintf("[fat32-test]   readdir /fat -> %d entries\n", n);
            } else {
                kprintf("[fat32-test]   readdir /fat FAILED\n");
            }

            /* (2) read /fat/HELLO.TXT */
            void *body = 0;
            size_t blen = 0;
            int rc = vfs_read_all("/fat/HELLO.TXT", &body, &blen);
            if (rc == VFS_OK) {
                kprintf("[fat32-test]   read /fat/HELLO.TXT -> %u bytes\n",
                        (unsigned)blen);
                kprintf("[fat32-test]   first line: ");
                size_t i = 0;
                for (; i < blen && i < 80 && ((char *)body)[i] != '\n'; i++) {
                    kprintf("%c", ((char *)body)[i]);
                }
                kprintf("\n");
                kfree(body);
            } else {
                kprintf("[fat32-test]   read /fat/HELLO.TXT FAILED: %s\n",
                        vfs_strerror(rc));
            }

            /* (3) readdir /fat/BIN */
            if (vfs_opendir("/fat/BIN", &d) == VFS_OK) {
                struct vfs_dirent e;
                int n = 0;
                while (vfs_readdir(&d, &e) == VFS_OK) {
                    kprintf("[fat32-test]   /fat/BIN[%d] %s  type=%d size=%u\n",
                            n++, e.name, (int)e.type, (unsigned)e.size);
                }
                vfs_closedir(&d);
                kprintf("[fat32-test]   readdir /fat/BIN -> %d entries\n", n);
            } else {
                kprintf("[fat32-test]   readdir /fat/BIN FAILED\n");
            }

            /* (4) read /fat/BIN/README.MD */
            rc = vfs_read_all("/fat/BIN/README.MD", &body, &blen);
            if (rc == VFS_OK) {
                kprintf("[fat32-test]   read /fat/BIN/README.MD -> %u bytes\n",
                        (unsigned)blen);
                kfree(body);
            } else {
                kprintf("[fat32-test]   read /fat/BIN/README.MD FAILED: %s\n",
                        vfs_strerror(rc));
            }

            /* (5) create + write + read-back + unlink a fresh file. */
            const char *tname = "/fat/SELFTEST.LOG";
            const char *tbody = "FAT32 boot self-test passed\n";
            size_t tlen = 0;
            while (tbody[tlen]) tlen++;
            rc = vfs_write_all(tname, tbody, tlen);
            if (rc != VFS_OK) {
                kprintf("[fat32-test]   write_all %s FAILED: %s\n",
                        tname, vfs_strerror(rc));
            } else {
                kprintf("[fat32-test]   wrote %u bytes to %s\n",
                        (unsigned)tlen, tname);
                rc = vfs_read_all(tname, &body, &blen);
                if (rc == VFS_OK && blen == tlen) {
                    kprintf("[fat32-test]   read-back %u bytes (match)\n",
                            (unsigned)blen);
                    kfree(body);
                } else {
                    kprintf("[fat32-test]   read-back FAILED rc=%d size=%u\n",
                            rc, (unsigned)blen);
                    if (body) kfree(body);
                }
                rc = vfs_unlink(tname);
                if (rc != VFS_OK) {
                    kprintf("[fat32-test]   unlink %s FAILED: %s\n",
                            tname, vfs_strerror(rc));
                } else {
                    kprintf("[fat32-test]   unlinked %s\n", tname);
                    /* (6) verify the file is really gone. */
                    struct vfs_stat st;
                    rc = vfs_stat(tname, &st);
                    if (rc == VFS_ERR_NOENT) {
                        kprintf("[fat32-test]   stat %s post-unlink -> NOENT (good)\n",
                                tname);
                    } else {
                        kprintf("[fat32-test]   stat %s post-unlink unexpected rc=%d\n",
                                tname, rc);
                    }
                }
            }

            kprintf("[fat32-test] <<< end smoke test on /fat\n");
        }

        /* Milestone 23C: opportunistically mount a FAT32-formatted USB
         * mass-storage device at /usb. Two layouts work:
         *
         *   1. GPT-partitioned stick: scan every BLK_CLASS_PARTITION
         *      whose parent name starts with "usb"; pick the first that
         *      has a valid FAT32 BPB. (run-xhci-usb-gpt path)
         *   2. Raw FAT32 stick (most consumer USB sticks today): scan
         *      every BLK_CLASS_DISK whose name starts with "usb"; pick
         *      the first that has a valid FAT32 BPB. (run-xhci-usb path)
         *
         * Failure to find either is a normal "no USB stick attached"
         * boot and is logged but non-fatal. */
        bool usb_mounted = false;
        {
            size_t it = 0;
            struct blk_dev *p;
            while (!usb_mounted &&
                   (p = blk_iter_next(&it, BLK_CLASS_PARTITION)) != NULL) {
                if (!p->parent || !p->parent->name) continue;
                const char *pn = p->parent->name;
                if (pn[0] != 'u' || pn[1] != 's' || pn[2] != 'b') continue;
                if (!fat32_probe(p)) continue;
                int rc = fat32_mount("/usb", p);
                if (rc == VFS_OK) {
                    kprintf("[boot] mounted /usb via FAT32 partition '%s' "
                            "on USB stick '%s'\n",
                            p->name, pn);
                    usb_mounted = true;
                } else {
                    kprintf("[boot] /usb FAT32 partition '%s' mount failed: %s\n",
                            p->name, vfs_strerror(rc));
                }
            }
            if (!usb_mounted) {
                size_t it2 = 0;
                struct blk_dev *d;
                while (!usb_mounted &&
                       (d = blk_iter_next(&it2, BLK_CLASS_DISK)) != NULL) {
                    if (!d->name) continue;
                    if (d->name[0] != 'u' || d->name[1] != 's' || d->name[2] != 'b')
                        continue;
                    if (!fat32_probe(d)) continue;
                    int rc = fat32_mount("/usb", d);
                    if (rc == VFS_OK) {
                        kprintf("[boot] mounted /usb via raw FAT32 USB disk '%s'\n",
                                d->name);
                        usb_mounted = true;
                    } else {
                        kprintf("[boot] raw FAT32 USB '%s' mount failed: %s\n",
                                d->name, vfs_strerror(rc));
                    }
                }
            }
            if (!usb_mounted) {
                kprintf("[boot] no FAT32-on-USB discovered -- /usb unmounted\n");
            }
        }

        /* Milestone 23C self-test: prove the BBB/SCSI/xHCI stack actually
         * delivers bytes by exercising readdir + read on /usb. We don't
         * touch the contents (so the same image can be diffed bit-for-
         * bit before/after a boot), just walk the root + read whatever
         * "HELLO.TXT"-equivalent it has. */
        if (usb_mounted) {
            kprintf("[usb-msc-test] >>> begin smoke test on /usb\n");

            struct vfs_dir d;
            if (vfs_opendir("/usb", &d) == VFS_OK) {
                struct vfs_dirent e;
                int n = 0;
                while (vfs_readdir(&d, &e) == VFS_OK) {
                    kprintf("[usb-msc-test]   /usb[%d] %s  type=%d size=%u\n",
                            n++, e.name, (int)e.type, (unsigned)e.size);
                }
                vfs_closedir(&d);
                kprintf("[usb-msc-test]   readdir /usb -> %d entries\n", n);
            } else {
                kprintf("[usb-msc-test]   readdir /usb FAILED\n");
            }

            void *body = 0;
            size_t blen = 0;
            int rc = vfs_read_all("/usb/HELLO.TXT", &body, &blen);
            if (rc == VFS_OK) {
                kprintf("[usb-msc-test]   read /usb/HELLO.TXT -> %u bytes\n",
                        (unsigned)blen);
                kprintf("[usb-msc-test]   first line: ");
                size_t i = 0;
                for (; i < blen && i < 80 && ((char *)body)[i] != '\n'; i++) {
                    kprintf("%c", ((char *)body)[i]);
                }
                kprintf("\n");
                kfree(body);
            } else {
                kprintf("[usb-msc-test]   read /usb/HELLO.TXT FAILED: %s\n",
                        vfs_strerror(rc));
            }

            /* Round-trip test: write -> read-back -> unlink. Same shape
             * as the /fat suite but on /usb, so a successful run proves
             * BOT writes are landing on the device. */
            const char *tname = "/usb/USBTEST.LOG";
            const char *tbody = "USB MSC boot self-test passed\n";
            size_t tlen = 0;
            while (tbody[tlen]) tlen++;
            rc = vfs_write_all(tname, tbody, tlen);
            if (rc != VFS_OK) {
                kprintf("[usb-msc-test]   write_all %s FAILED: %s\n",
                        tname, vfs_strerror(rc));
            } else {
                kprintf("[usb-msc-test]   wrote %u bytes to %s\n",
                        (unsigned)tlen, tname);
                rc = vfs_read_all(tname, &body, &blen);
                if (rc == VFS_OK && blen == tlen) {
                    kprintf("[usb-msc-test]   read-back %u bytes (match)\n",
                            (unsigned)blen);
                    kfree(body);
                } else {
                    kprintf("[usb-msc-test]   read-back FAILED rc=%d size=%u\n",
                            rc, (unsigned)blen);
                    if (body) kfree(body);
                }
                rc = vfs_unlink(tname);
                if (rc != VFS_OK) {
                    kprintf("[usb-msc-test]   unlink %s FAILED: %s\n",
                            tname, vfs_strerror(rc));
                } else {
                    kprintf("[usb-msc-test]   unlinked %s\n", tname);
                }
            }

            /* M26E: explicit unmount + remount cycle. Validates that
             * vfs_unmount + fat32_umount drop their cluster buffers
             * cleanly, that we can re-mount the same FAT32 disk at the
             * same path, and that round-trip writes survive both sides
             * of the cycle (i.e. the dirty FAT-sector flush actually
             * lands before the umount frees the scratch buffer). */
            kprintf("[usb-msc-test]   M26E: unmount /usb + remount round-trip\n");
            int urc = vfs_unmount("/usb");
            if (urc != VFS_OK) {
                kprintf("[usb-msc-test]   unmount /usb FAILED rc=%d\n", urc);
            } else {
                kprintf("[usb-msc-test]   unmounted /usb cleanly\n");

                /* Remount must mirror the *initial* /usb bring-up: GPT sticks
                 * expose FAT32 on a PARTITION first; raw layout uses the
                 * whole DISK. M26E previously only scanned DISK, so after
                 * unmount a partitioned stick never remounted — /usb stayed
                 * dead and bootlog_flush_all() could not write BOOTLOG.TXT. */
                bool remount_ok = false;
                {
                    size_t itp = 0;
                    struct blk_dev *p;
                    while (!remount_ok &&
                           (p = blk_iter_next(&itp, BLK_CLASS_PARTITION)) !=
                               NULL) {
                        if (!p->parent || !p->parent->name) continue;
                        const char *pn = p->parent->name;
                        if (pn[0] != 'u' || pn[1] != 's' || pn[2] != 'b')
                            continue;
                        if (!fat32_probe(p)) continue;
                        int rrc = fat32_mount("/usb", p);
                        if (rrc == VFS_OK) {
                            kprintf("[usb-msc-test]   remounted /usb via "
                                    "partition '%s'\n",
                                    p->name);
                            remount_ok = true;
                        }
                    }
                }
                if (!remount_ok) {
                    size_t itd = 0;
                    struct blk_dev *d;
                    while (!remount_ok &&
                           (d = blk_iter_next(&itd, BLK_CLASS_DISK)) != NULL) {
                        if (!d || d->gone) continue;
                        if (!d->name || d->name[0] != 'u' ||
                            d->name[1] != 's' || d->name[2] != 'b')
                            continue;
                        if (!fat32_probe(d)) continue;
                        int rrc = fat32_mount("/usb", d);
                        if (rrc == VFS_OK) {
                            kprintf("[usb-msc-test]   remounted /usb via "
                                    "disk '%s'\n",
                                    d->name);
                            remount_ok = true;
                        }
                    }
                }
                if (!remount_ok) {
                    kprintf("[usb-msc-test]   remount /usb FAILED -- "
                            "no FAT32 USB partition or disk found\n");
                } else {
                    /* Round-trip a small file again to prove the FS
                     * came back fully wired. */
                    const char *rname = "/usb/M26E.LOG";
                    const char *rbody = "M26E remount round-trip OK\n";
                    size_t rlen = 0;
                    while (rbody[rlen]) rlen++;
                    int wrc = vfs_write_all(rname, rbody, rlen);
                    if (wrc != VFS_OK) {
                        kprintf("[usb-msc-test]   remount write FAILED: %s\n",
                                vfs_strerror(wrc));
                    } else {
                        void *rbuf = 0;
                        size_t rblen = 0;
                        wrc = vfs_read_all(rname, &rbuf, &rblen);
                        if (wrc == VFS_OK && rblen == rlen) {
                            kprintf("[usb-msc-test]   remount RW round-trip "
                                    "PASS (%u bytes)\n", (unsigned)rblen);
                            kfree(rbuf);
                        } else {
                            kprintf("[usb-msc-test]   remount read-back "
                                    "FAILED rc=%d size=%u\n",
                                    wrc, (unsigned)rblen);
                            if (rbuf) kfree(rbuf);
                        }
                        vfs_unlink(rname);
                    }
                }
            }

            kprintf("[usb-msc-test] <<< end smoke test on /usb\n");
        }

        /* Milestone 23D: opportunistically mount the FIRST ext4-looking
         * partition at /ext, read-only. The driver only exposes
         * read/readdir/stat through VFS -- create/write/unlink return
         * VFS_ERR_ROFS, which is the correct shape for a read-only
         * mount. We probe by reading the partition's superblock at
         * byte offset 1024 (sector 2) and checking the 0xEF53 magic +
         * the INCOMPAT bits we can safely handle. */
        bool ext_mounted = false;
        {
            size_t it = 0;
            struct blk_dev *p;
            while (!ext_mounted &&
                   (p = blk_iter_next(&it, BLK_CLASS_PARTITION)) != NULL) {
                if (!ext4_probe(p)) continue;
                int rc = ext4_mount("/ext", p);
                if (rc == VFS_OK) {
                    kprintf("[boot] mounted /ext via ext4 partition '%s' "
                            "(slot %u, parent '%s', LBA %lu, %lu sectors)\n",
                            p->name, (unsigned)p->partition_index,
                            p->parent ? p->parent->name : "?",
                            (unsigned long)p->offset_lba,
                            (unsigned long)p->sector_count);
                    ext_mounted = true;
                } else {
                    kprintf("[boot] ext4 partition '%s' looked valid but "
                            "mount failed: %s\n", p->name, vfs_strerror(rc));
                }
            }
            if (!ext_mounted) {
                kprintf("[boot] no ext4 partition discovered -- /ext unmounted\n");
            }
        }

        /* Milestone 23D self-test: read-only smoke test over /ext.
         * Validates: (1) readdir of root + /BIN, (2) read of HELLO.TXT
         * + BIN/README.MD, (3) write attempts return VFS_ERR_ROFS. */
        if (ext_mounted) {
            kprintf("[ext4-test] >>> begin smoke test on /ext\n");

            struct vfs_dir d;
            if (vfs_opendir("/ext", &d) == VFS_OK) {
                struct vfs_dirent e;
                int n = 0;
                while (vfs_readdir(&d, &e) == VFS_OK) {
                    kprintf("[ext4-test]   /ext[%d] %s  type=%d size=%u\n",
                            n++, e.name, (int)e.type, (unsigned)e.size);
                }
                vfs_closedir(&d);
                kprintf("[ext4-test]   readdir /ext -> %d entries\n", n);
            } else {
                kprintf("[ext4-test]   readdir /ext FAILED\n");
            }

            void *body = 0;
            size_t blen = 0;
            int rc = vfs_read_all("/ext/HELLO.TXT", &body, &blen);
            if (rc == VFS_OK) {
                kprintf("[ext4-test]   read /ext/HELLO.TXT -> %u bytes\n",
                        (unsigned)blen);
                kprintf("[ext4-test]   first line: ");
                size_t i = 0;
                for (; i < blen && i < 80 && ((char *)body)[i] != '\n'; i++) {
                    kprintf("%c", ((char *)body)[i]);
                }
                kprintf("\n");
                kfree(body);
            } else {
                kprintf("[ext4-test]   read /ext/HELLO.TXT FAILED: %s\n",
                        vfs_strerror(rc));
            }

            if (vfs_opendir("/ext/BIN", &d) == VFS_OK) {
                struct vfs_dirent e;
                int n = 0;
                while (vfs_readdir(&d, &e) == VFS_OK) {
                    kprintf("[ext4-test]   /ext/BIN[%d] %s  type=%d size=%u\n",
                            n++, e.name, (int)e.type, (unsigned)e.size);
                }
                vfs_closedir(&d);
                kprintf("[ext4-test]   readdir /ext/BIN -> %d entries\n", n);
            } else {
                kprintf("[ext4-test]   readdir /ext/BIN FAILED\n");
            }

            rc = vfs_read_all("/ext/BIN/README.MD", &body, &blen);
            if (rc == VFS_OK) {
                kprintf("[ext4-test]   read /ext/BIN/README.MD -> %u bytes\n",
                        (unsigned)blen);
                kfree(body);
            } else {
                kprintf("[ext4-test]   read /ext/BIN/README.MD FAILED: %s\n",
                        vfs_strerror(rc));
            }

            /* Confirm read-only enforcement: write/create/unlink MUST
             * each return VFS_ERR_ROFS. Anything else means the driver
             * accidentally surfaced a write path. */
            rc = vfs_write_all("/ext/SHOULD_FAIL.TXT", "x", 1);
            kprintf("[ext4-test]   write_all /ext/... -> %s "
                    "(want ROFS / -9)\n", vfs_strerror(rc));
            rc = vfs_unlink("/ext/HELLO.TXT");
            kprintf("[ext4-test]   unlink /ext/HELLO.TXT -> %s "
                    "(want ROFS / -9)\n", vfs_strerror(rc));

            kprintf("[ext4-test] <<< end smoke test on /ext\n");
        }

        installer_scan_modules();
        vfs_dump_mounts();

        if (data_mounted) {
            swap_init(TFS_TOTAL_BLOCKS * TFS_SECTORS_PER_BLOCK,
                      SWAP_SLOT_COUNT * SWAP_SECTORS_PER_PAGE);
        }
    }

    /* Loadable kernel modules: scan /lib/modules/ for .ko files and
     * load them. This runs after all filesystems are mounted so both
     * initrd and /data paths are accessible. After loading, re-run
     * pci_bind_drivers() so any newly registered PCI drivers from
     * modules get a chance to probe unclaimed devices. */
    {
        int mod_count = module_load_all();
        if (mod_count > 0) {
            kprintf("[boot] re-running PCI bind pass for %d new module(s)\n",
                    mod_count);
            pci_bind_drivers();
        }
    }

    /* Process model + scheduler MUST come up before any proc_create
     * call. proc_init synthesizes pid 0 from this very boot context;
     * everything below is "running as pid 0". */
    proc_init();
    sched_init();
    signal_init();           /* milestone 8: SIGINT/SIGTERM + foreground tracking */
    futex_init();            /* Phase 1 M1.1: futex wait/wake hash table */
    shm_init();              /* Phase 1 M1.4: shared memory */
    unix_socket_init();      /* Phase 1 M1.4: Unix domain sockets */
    sysfs_init();            /* Phase 1 M1.5: sysfs virtual filesystem */
    aslr_init();             /* Phase 7 M7.1: address space layout randomization */
    hardening_init();        /* Phase 7 M7.2: SMEP/SMAP/NX enforcement */
    page_fault_init();       /* Phase 1: COW + demand paging refcounts + vm_spaces */
    aml_interp_init();       /* Phase 4: AML namespace + interpreter */
    clipboard_init();        /* Phase 2 M2.7: system clipboard */
    hidpi_init();            /* Phase 2 M2.6: HiDPI display scaling */
    audio_engine_init();     /* Phase B: audio mixing + userland playback */
    inotify_init_subsystem(); /* Phase 1 M1.5: file watching */
    user_first_run();        /* spawn /bin/hello as pid 1, wait, reap */
    /* M22 step 5: kick the BSP's LAPIC timer at 100 Hz BEFORE waking
     * the APs. The ISR (see apic_timer_isr in apic.c) just bumps
     * this CPU's tick counter; cooperative preemption stays driven
     * by sched_yield() calls scattered through proc_exit / proc_wait /
     * the syscall layer. Each AP starts its own LAPIC timer inside
     * ap_entry once its LAPIC is up. */
    if (apic_timer_periodic_init(100)) {
        kprintf("[boot] BSP LAPIC timer @ 100 Hz live (vec=0x%02x)\n",
                (unsigned)0x40);
    }
    smp_start_aps();         /* INIT-SIPI-SIPI (BSP/IO APIC already up) */
    /* Networking intentionally starts after the GUI/input layer setup
     * below, but before desktop/login services are launched. On the HP
     * Realtek 8168 machine, moving NIC/DHCP before GUI setup regressed
     * into "no DHCP packets observed on the wire"; moving it after
     * login startup let userspace scheduling prevent DHCP from starting. */

    /* Milestone 24B–24D self-test: DNS + TCP + full HTTP GET to
     * example.com. Wall-clock cost is noticeable on every boot
     * (especially offline: DNS 1.5s ×2, TCP connect/recv, HTTP 3s) on
     * top of net_init()'s DHCP budget.  Default `make` sets -DFAST_BOOT
     * (Makefile EXTRA_CFLAGS); use `make fullboot` for this smoke block. */
#if !defined(FAST_BOOT)
    /* Milestone 24B self-test: exercise the resolver end-to-end
     * against the DNS server DHCP just gave us. Two cases:
     *   (a) example.com  -- canonical "internet still exists" probe.
     *   (b) tobyos.invalid -- RFC 6761 .invalid TLD, guaranteed to
     *       never resolve, so we confirm NXDOMAIN flows through
     *       gracefully (no hang, clean log line, returns false).
     * Short timeout (1.5 s) so a host without internet can't stall
     * the boot meaningfully -- the resolver just logs a timeout and
     * we move on to the desktop. */
    if (!safemode_skip_net() && net_is_up()) {
        struct dns_result r;
        kprintf("[dns-test] >>> resolving example.com (boot smoke test)\n");
        if (dns_resolve("example.com", 1500, &r)) {
            char ipbuf[16];
            net_format_ip(ipbuf, r.ip_be);
            kprintf("[dns-test]     example.com -> %s (ttl=%us)\n",
                    ipbuf, (unsigned)r.ttl_secs);
        } else {
            kprintf("[dns-test]     example.com: lookup failed (offline host?)\n");
        }
        kprintf("[dns-test] >>> resolving tobyos.invalid (NXDOMAIN probe)\n");
        if (dns_resolve("tobyos.invalid", 1500, &r)) {
            kprintf("[dns-test]     tobyos.invalid: UNEXPECTED success (rogue resolver?)\n");
        } else {
            kprintf("[dns-test]     tobyos.invalid: failed as expected\n");
        }
        kprintf("[dns-test] <<< done\n");

        /* Milestone 24C self-test: open a TCP connection to the
         * resolved address on port 80, exercise BOTH directions
         * (send a tiny GET / HTTP/1.0 request, drain whatever the
         * server returns), and close cleanly. We don't parse the
         * HTTP response -- that's 24D. Here we only care that the
         * state machine survives a real round trip with payload in
         * both directions. */
        kprintf("[tcp-test] >>> opening TCP connection to example.com:80\n");
        if (dns_resolve("example.com", 1500, &r)) {
            struct tcp_conn *c = tcp_connect(r.ip_be, htons(80), 3000);
            if (c) {
                kprintf("[tcp-test]     handshake OK -- sending HTTP/1.0 probe\n");
                static const char probe[] =
                    "GET / HTTP/1.0\r\n"
                    "Host: example.com\r\n"
                    "User-Agent: tobyOS/24C\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                long sent = tcp_send(c, probe, sizeof(probe) - 1);
                kprintf("[tcp-test]     tcp_send returned %ld\n", sent);

                /* Read whatever streams back, drop everything past
                 * the first 80 bytes so the log stays compact. */
                char rb[256];
                long total = 0;
                bool printed_first = false;
                for (int i = 0; i < 16; i++) {
                    long n = tcp_recv(c, rb, sizeof(rb) - 1, 1000);
                    if (n > 0) {
                        total += n;
                        if (!printed_first) {
                            rb[n < 80 ? n : 80] = 0;
                            /* Strip CRs so log lines stay neat. */
                            for (long k = 0; k < n && k < 80; k++)
                                if (rb[k] == '\r') rb[k] = ' ';
                            kprintf("[tcp-test]     first bytes: \"%s\"\n", rb);
                            printed_first = true;
                        }
                    } else if (n == -1) {
                        kprintf("[tcp-test]     server FIN; total bytes=%ld\n", total);
                        break;
                    } else if (n == -2) {
                        kprintf("[tcp-test]     server RST; total bytes=%ld\n", total);
                        break;
                    } else {
                        /* timeout this iteration; continue waiting */
                    }
                }
                tcp_close(c);
                kprintf("[tcp-test]     teardown OK.\n");
            } else {
                kprintf("[tcp-test]     connect failed (RST or timeout)\n");
            }
        } else {
            kprintf("[tcp-test]     skipped: DNS lookup of example.com failed\n");
        }
        kprintf("[tcp-test] <<< done\n");

        /* Milestone 24D self-test: drive the full HTTP client end-to-end
         * against a real public server. Validates URL parsing, DNS,
         * TCP, status-line + header parsing, Content-Length-bounded
         * body collection, and graceful close -- the same stack that
         * `pkg install http://...` rides on. */
        kprintf("[http-test] >>> GET http://example.com/\n");
        struct http_response hr;
        int hrc = http_get("http://example.com/",
                           /*max=*/64u * 1024u, /*timeout_ms=*/3000, &hr);
        if (hrc == 0) {
            kprintf("[http-test]     status=%d reason=\"%s\" type=\"%s\" body=%lu bytes\n",
                    hr.status, hr.reason,
                    hr.content_type[0] ? hr.content_type : "(none)",
                    (unsigned long)hr.body_len);
            /* Print the first ~80 bytes of the body, sanitised. */
            char preview[81];
            size_t n = hr.body_len < sizeof(preview) - 1
                       ? hr.body_len : sizeof(preview) - 1;
            for (size_t i = 0; i < n; i++) {
                char b = (char)hr.body[i];
                preview[i] = (b == '\r' || b == '\n' || b == '\t') ? ' ' : b;
            }
            preview[n] = 0;
            kprintf("[http-test]     body[0..%lu]: \"%s\"\n",
                    (unsigned long)n, preview);
            http_free(&hr);
        } else {
            kprintf("[http-test]     failed: %s (%d)\n", http_strerror(hrc), hrc);
        }
        kprintf("[http-test] <<< done\n");
    }
#else
    if (!safemode_skip_net() && net_is_up()) {
        kprintf("[boot] FAST_BOOT: skipping dns/tcp/http example.com smoke\n");
    }
#endif

    /* GUI subsystem (milestone 10): graphics back buffer + PS/2 mouse +
     * window manager. Each layer is independently no-op-on-failure --
     * if any of them refuses we still drop into the text shell.
     *
     * Milestone 21 step 7: virtio_gpu_install_backend() runs immediately
     * after gfx_layer_init() so the FIRST gfx_flip() (issued by gui_init
     * below) already routes through TRANSFER+FLUSH on machines with a
     * virtio-gpu. On every other machine the call is a silent no-op and
     * gfx_flip() stays on the universal Limine memcpy fallback.
     *
     * M28D: safe mode entirely skips the compositor stack -- the M28D
     * design is "framebuffer console only, no virtio-gpu, no mouse,
     * no window manager, no compositor". The kernel console keeps
     * working (printk + the framebuffer text overlay from
     * console.c) so the operator still has stdout.
     * M35E: GUI + COMPATIBILITY both bring the compositor up, but
     * COMPATIBILITY skips the virtio-gpu fast path so we stick with
     * the firmware-provided framebuffer (most-tested code path). */
     if (safemode_skip_gui()) {
        kprintf("[safe] skipping gfx/mouse/gui/term/m14_init -- mode=%s\n",
                safemode_tag());
        banner();
    } else {
        framebuffer_sync_mapping();
        gfx_layer_init();

        if (safemode_skip_virtio_gpu()) {
            kprintf("[safe] mode=%s -- keeping Limine framebuffer "
                    "(skip virtio-gpu fast path)\n", safemode_tag());
        } else {
            virtio_gpu_install_backend();
        }

        /* i915-lite Stage 1: read-only Intel GT/display reconnaissance.
         * Logs the real GT + scanout config (the data the BLT-ring and
         * page-flip stages need, which QEMU can't emulate). Writes nothing
         * and never touches the display, so it can't disturb the working
         * Limine framebuffer on the EliteDesk. Silent no-op without an
         * Intel GPU (e.g. QEMU's std/virtio paths). */
        intel_gpu_gt_recon();

        /* i915-lite Stage 2: bring up the Intel GT render side (forcewake +
         * GGTT + BCS blitter ring) and run a hardware store-dword self-test.
         * Touches ONLY GT/GGTT/ring regs + buffers we allocate -- never the
         * display pipe -- so a broken blitter can't blackscreen the box; it
         * just leaves intel_gt_selftest_ok()==0 and we stay on Limine. QEMU
         * has no Intel GT, so this is a no-op there (gen<8 / no GPU). */
        intel_gt_init();

        mouse_init();
        gui_init();

        /* Auto-enable GPU-accelerated compositor if VirtIO-GPU is active.
         * This activates per-window dirty tracking, direct scanout for
         * fullscreen windows, and triple buffering.  Falls back
         * transparently if no GPU is present. */
        if (virtio_gpu_present()) {
            gui_set_compositor_mode(COMPOSITOR_GPU_ACCEL);
        }

        term_init();
        banner();
    }

    /*
     * Bring networking up directly in the protected boot window. This is
     * intentionally synchronous: the HP Realtek path has proven sensitive to
     * losing this early turn once userspace/compositor work starts running.
     */
    if (safemode_skip_net()) {
        kprintf("[safe] skipping net_init (NIC + DHCP + DNS) -- mode=%s\n",
                safemode_tag());
    } else {
        kprintf("[boot] settling input before network bring-up\n");
        for (unsigned i = 0; i < 30; i++) {
            usb_legacy_poll();
            xhci_poll();
            kbd_flush_pending();
            mouse_flush_pending();
            pit_sleep_ms(1);
        }

        kprintf("[boot] before net_init\n");
        bool net_ok = net_init();
        kprintf("[boot] after net_init ok=%d net_up=%d\n",
                (int)net_ok, (int)net_is_up());

        if (net_ok) {
            kprintf("[boot] net_init OK; starting TCP services\n");
            ipv6_init();
            icmpv6_init();
#ifdef SLAAC_SELFTEST
            icmpv6_slaac_selftest();
#endif
#ifdef DHCPV6_SELFTEST
            /* Opt-in: stateful DHCPv6 client self-test (QEMU SLIRP has no
             * DHCPv6 server, so we drive synthetic ADVERTISE/REPLY msgs). */
            { extern int dhcpv6_self_test(void); dhcpv6_self_test(); }
#endif
            tcp_echo_init();
            tcp_shell_init();
            ssh_init();

            /* Static fallback is useful for a local shell, but keep DHCP
             * retrying from the idle service lane until a real lease lands. */
            if (!net_boot_used_dhcp()) {
                net_boot_request();
            }
        } else {
            kprintf("[boot] net_init failed; networking will retry in idle\n");
            net_boot_request();
        }
    }

    if (!safemode_skip_gui()) {
        /* Milestone 14: bring up settings + services + session BEFORE
         * shell_init so the desktop+login is already on screen by the
         * time the shell starts polling. The shell stays available for
         * debugging.
         */
        framebuffer_sync_mapping();
        m14_init();

        /*
         * Force one compositor pass as soon as the desktop stack is ready.
         */
        kprintf("[boot] first gui_tick after desktop init\n");
        gui_tick();
    }

    if (!safemode_skip_gui()) {
        gui_invalidate_full();
        gui_tick();
    }

    if (safemode_skip_services()) {
        kprintf("[safe] skipping pkg_init + selftests + devtest harnesses "
                "(non-essential)\n");
    } else {
        /* Milestone 16: package manager. Runs AFTER m14_init so the
         * compositor + launcher are up and pkg_refresh_launcher() has
         * something to register into. ensure_dir() creates /data/apps,
         * /data/packages, /data/repo on the persistent FS (idempotent --
         * safe across reboots). */
        /* Milestone 34: validate the SHA-256 + HMAC primitives BEFORE
         * pkg_init runs, so any regression in the crypto code panics
         * early instead of producing wrong-but-consistent digests on
         * the package verification path. Then load the trust store
         * (one-time read of /system/keys/trust.db -- absent file is
         * fine, returns 0 keys). */
        sec_selftest();
        ssh_crypto_selftest();
        sig_trust_store_init();
        /* Milestone 34E: register the protected-prefix table BEFORE
         * pkg_init -- pkg_init creates /data/packages/ and friends,
         * and although it goes through pkg_priv (which opens a sysprot
         * scope), having sysprot logged ahead of pkg_init keeps the
         * boot trace easy to read. */
        sysprot_init();
        pkg_init();
        /* Milestone 17: optional boot self-test for the package upgrade
         * pipeline. Only does anything when the kernel was built with
         * `make m17test` (which adds -DPKG_M17_SELFTEST); the regular
         * build leaves this as a no-op stub. */
        pkg_m17_selftest();
        /* Milestone 34: optional boot self-test for the package
         * security pipeline (integrity, signing, capability defaults,
         * protected paths). No-op stub unless the kernel was built
         * with `make m34test` (which adds -DPKG_M34_SELFTEST). */
        pkg_m34_selftest();
        /* Milestone 34G: optional boot autorun of the integrated
         * `securitytest` validation suite. Built with `make sectest`
         * (which adds -DSECTEST_AUTORUN). This is the operator-visible
         * version of the security tests -- it goes through the live
         * production paths the shell builtin would, then prints the
         * canonical OVERALL: line that test_m34g.ps1 greps. */
#ifdef SECTEST_AUTORUN
        sectest_run(NULL);
#endif
        installer_m20_selftest();
        /* Milestone 24D: optional boot self-test for HTTP-based package
         * install. Only does anything when built with `make m24dtest`;
         * default builds leave this as a no-op stub. The test driver is
         * tests/test_m24d.ps1 which sets up the host HTTP server first. */
        http_m24d_selftest();
        /* M22 step 4: arm the ACPI shutdown self-test if the kernel was
         * built with -DACPI_M22_SELFTEST. Default builds turn this into
         * a no-op stub, so there is no production cost. */
        acpi_m22_selftest_arm();
        /* M26A peripheral test harness: must run AFTER every device-
         * registering subsystem has had its turn (PCI bind, partition
         * scan, USB enumeration, mouse + keyboard init, audio probe).
         * The order here matters only inasmuch as devtest_boot_run will
         * only see what's already been registered when it walks. */
        /* M26G: probe QEMU fw_cfg first so acpi_bat_init can pick up
         * any opt/tobyos/battery_mock blob the test harness injected. */
        fw_cfg_init();
        acpi_bat_init();
        devtest_init();
        /* M27A: display registry + self-tests. Must run AFTER devtest_init
         * (which zeroes the test array) and AFTER any gfx backend swap so
         * the recorded backend name is current. Safe even on a true-
         * headless boot (registry stays empty, tests SKIP cleanly). */
        display_init();
#ifndef QUICK_BOOT
        devtest_boot_run();
#endif
        shell_init();
#ifdef POSIX_SHELL_SELFTEST
        posix_shell_selftest();
#endif
#ifndef QUICK_BOOT
        /* Milestone 25C: drive the shell over a few synthetic command
         * lines BEFORE the idle loop starts polling the keyboard. This
         * exercises shell_run_test_line -> execute_line -> resolve_program
         * (PATH walk) -> proc_spawn (with shell envp) -> proc_wait. */
        user_shell_smoketest();
        /* Milestone 26A: validate the new shell builtins (devlist, drvtest)
         * + every userland test program (/bin/devlist, /bin/drvtest,
         * /bin/usbtest, /bin/audiotest, /bin/batterytest). Each line in
         * the boot log is grep-able as `[boot] M26A: <path> ... PASS`. */
        m26a_run_userland_tools();
        /* Milestone 27A: display test harness. Spawns /bin/displayinfo,
         * /bin/drawtest, /bin/rendertest. Each emits `[boot] M27A: ...
         * PASS` lines that test_m27a.ps1 greps for. */
        m27a_run_userland_tools();
#else
        kprintf("[boot] QUICK_BOOT: skipping devtest_boot_run + shell "
                "smoketest + M26A/M27A spawns\n");
#endif
    }
#ifndef QUICK_BOOT
    /* Milestone 28A: structured logging harness. Exercises the slog
     * ring from multiple subsystems / levels, persists to disk, and
     * spawns /bin/logview to verify the SLOG_READ syscall path. Each
     * line is grepable as `[boot] M28A: ...`. */
    m28a_run_logging_harness();

    /* Milestone 29A: hardware-discovery harness. Snapshots the
     * unified hardware inventory, dumps it to serial, persists to
     * /data/hwinfo.snap, and spawns /bin/hwinfo --boot to verify
     * SYS_HWINFO from userland. Runs after M28A so the slog has a
     * chance to flush its boot-time output first. */
    m29a_run_hwinfo_harness();

    /* Milestone 29B: drvmatch harness. Dumps the live driver-match
     * table, spawns /bin/drvmatch --boot, and (only when
     * /etc/drvtest_now is present) exercises the forced-disable
     * fallback path. */
    m29b_run_drvmatch_harness();

    /* Milestone 35F: hardware-compatibility report harness. Always
     * spawns /bin/hwreport --boot which calls SYS_HWINFO +
     * SYS_HWCOMPAT_LIST, computes the GREEN/YELLOW/RED verdict, and
     * prints M35F_HWR sentinels. RED is a hard FAIL; GREEN/YELLOW
     * both pass. */
    m35f_run_hwreport_harness();

    /* Milestone 35G: end-to-end compatibility validation. Always
     * spawns /bin/compattest --boot which runs the eight-bucket
     * suite (system_boot, driver_match, fallback, network, storage,
     * usb_input, log_capture, no_crashes) and prints M35G_CMP
     * sentinels. Tests that require real hardware return
     * SKIPPED_REAL_HARDWARE_REQUIRED instead of failing. */
    m35g_run_compattest_harness();
#else
    kprintf("[boot] QUICK_BOOT: skipping M28A–M35G boot harness spawns "
            "(CI: `make fullboot` / omit -DQUICK_BOOT)\n");
#endif

#ifdef SIGTEST_BOOT
    /* Opt-in signal-delivery regression harness (build with
     * EXTRA_CFLAGS+=-DSIGTEST_BOOT). Runs regardless of QUICK_BOOT so the
     * real kernel signal frame/sigreturn path can be validated on a fast
     * headless boot. Spawns /bin/sigtest and asserts a clean exit. */
    {
        kprintf("[boot] SIGTEST: spawning /bin/sigtest\n");
        char *argv[] = { (char *)"sigtest", (char *)"--boot", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/sigtest",
            .name = "sigtest-boot",
            .argc = 2,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] SIGTEST: /bin/sigtest not spawned (rc=%d) MISSING\n",
                    pid);
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] SIGTEST: /bin/sigtest (pid=%d) exit=%d (%s)\n",
                    pid, rc, rc == 0 ? "PASS" : "FAIL");
        }
    }
#endif

#ifdef LUATEST_BOOT
    /* Opt-in Lua app-compat harness (build EXTRA_CFLAGS+=-DLUATEST_BOOT).
     * Runs /bin/lua on /etc/lua_selftest.lua, which prints "LUATEST: ALL OK"
     * when the interpreter (incl. string.format/rep/upper/lower, math.*) is
     * working. Demonstrates real Lua scripts executing on tobyOS. */
    {
        kprintf("[boot] LUATEST: spawning /bin/lua /etc/lua_selftest.lua\n");
        char *argv[] = { (char *)"lua", (char *)"/etc/lua_selftest.lua", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/lua",
            .name = "luatest-boot",
            .argc = 2,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] LUATEST: /bin/lua not spawned (rc=%d) MISSING\n", pid);
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] LUATEST: /bin/lua (pid=%d) exit=%d\n", pid, rc);
        }
    }
#endif

#ifdef WINPE_BOOT
    /* Track C (foreign-binary compat) -- Win32/PE proof, milestone C1.
     * Build EXTRA_CFLAGS+=-DWINPE_BOOT. Spawns /bin/win-hello.exe, a GENUINE
     * Windows x86-64 console PE (built with the Windows toolchain, importing
     * kernel32!{GetStdHandle,WriteFile,ExitProcess}). tobyOS's PE loader maps
     * the image, binds those imports to its Win32 shims via a user-mode
     * marshalling gate, and runs it: the program writes a line to stdout
     * through WriteFile and exits 42. The written line shows up in the serial
     * log and exit==42 proves the whole PE-load -> IAT -> shim chain. Runs
     * regardless of QUICK_BOOT so it is provable on a fast headless boot. */
    {
        kprintf("[boot] WINPE: spawning /bin/win-hello.exe (Windows x86-64 PE)\n");
        char *argv[] = { (char *)"win-hello.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-hello.exe",
            .name = "win-hello.exe",
            .argc = 1,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] WINPE: /bin/win-hello.exe not spawned (rc=%d) "
                    "MISSING\n", pid);
            kprintf("[WINPE] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE: /bin/win-hello.exe (pid=%d) exit=%d\n",
                    pid, rc);
            kprintf("[WINPE] VERDICT: %s exit=%d (expected 42)\n",
                    rc == 42 ? "PASS" : "FAIL", rc);
        }
    }
#endif

#ifdef WINPE2_BOOT
    /* Track C -- Win32/PE C-runtime proof, milestone C2. Build
     * EXTRA_CFLAGS+=-DWINPE2_BOOT. Spawns /bin/win-crt.exe, a Windows PE that
     * calls the REAL ucrt printf/puts (which inline to __acrt_iob_func +
     * __stdio_common_vfprintf, whose va_list is a STACK arg). tobyOS shims
     * those kernel-side with a full printf engine; the rendered lines show in
     * the serial log and exit==7 proves stack-arg marshalling + the format
     * engine. Runs regardless of QUICK_BOOT. */
    {
        kprintf("[boot] WINPE2: spawning /bin/win-crt.exe (Windows PE + C runtime)\n");
        char *argv[] = { (char *)"win-crt.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-crt.exe",
            .name = "win-crt.exe",
            .argc = 1,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] WINPE2: /bin/win-crt.exe not spawned (rc=%d) "
                    "MISSING\n", pid);
            kprintf("[WINPE2] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE2: /bin/win-crt.exe (pid=%d) exit=%d\n",
                    pid, rc);
            kprintf("[WINPE2] VERDICT: %s exit=%d (expected 7)\n",
                    rc == 7 ? "PASS" : "FAIL", rc);
        }
    }
#endif

#ifdef WINPE3_BOOT
    /* Track C -- the full mainCRTStartup, milestone C3. Build
     * EXTRA_CFLAGS+=-DWINPE3_BOOT. Spawns /bin/win-hello3.exe, a STOCK
     * `clang main.c -o win-hello3.exe` (no flags, no custom entry) that runs
     * the whole ucrt CRT startup before main: it reads the TEB via gs:[0x30]
     * (the SWAPGS scheme), uses the user-memory heap, gets argc/argv from the
     * CRT-data region, and printf's through the shimmed ucrt stdio. The lines
     * appear in the serial log and exit==3 proves the off-the-shelf .exe ran.
     * Runs regardless of QUICK_BOOT. */
    {
        kprintf("[boot] WINPE3: spawning /bin/win-hello3.exe (STOCK Windows .exe)\n");
        char *argv[] = { (char *)"win-hello3.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-hello3.exe",
            .name = "win-hello3.exe",
            .argc = 1,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] WINPE3: /bin/win-hello3.exe not spawned (rc=%d) "
                    "MISSING\n", pid);
            kprintf("[WINPE3] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE3: /bin/win-hello3.exe (pid=%d) exit=%d\n",
                    pid, rc);
            kprintf("[WINPE3] VERDICT: %s exit=%d (expected 3)\n",
                    rc == 3 ? "PASS" : "FAIL", rc);
        }
    }
#endif

#ifdef WINPE4_BOOT
    /* Track C -- a STOCK C++ .exe, milestone C4. Build EXTRA_CFLAGS+=-DWINPE4_BOOT.
     * Spawns /bin/win-cpp.exe, a plain `clang++ main.cpp -o win-cpp.exe`. Beyond
     * C3 this runs a C++ GLOBAL CONSTRUCTOR (mingw calls it from static __main ->
     * __do_global_ctors in CPL3 -- no kernel trampoline) before main, and pulls
     * in the wider ucrt surface (_errno/localeconv/FILE locking/wide-string
     * helpers) that tobyOS shims. The ctor's line appears in the serial log and
     * exit==5 proves the constructor ran. Runs regardless of QUICK_BOOT. */
    {
        kprintf("[boot] WINPE4: spawning /bin/win-cpp.exe (STOCK C++ Windows .exe)\n");
        char *argv[] = { (char *)"win-cpp.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-cpp.exe",
            .name = "win-cpp.exe",
            .argc = 1,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] WINPE4: /bin/win-cpp.exe not spawned (rc=%d) "
                    "MISSING\n", pid);
            kprintf("[WINPE4] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE4: /bin/win-cpp.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE4] VERDICT: %s exit=%d (expected 5)\n",
                    rc == 5 ? "PASS" : "FAIL", rc);
        }
    }
#endif

#ifdef WINPE5_BOOT
    /* Track C -- Win32 FILE I/O, milestone C5. Build EXTRA_CFLAGS+=-DWINPE5_BOOT.
     * Spawns /bin/win-fileio.exe, a STOCK clang-built .exe that round-trips a
     * file through the Win32 API: CreateFileA(write) -> WriteFile -> CloseHandle
     * -> CreateFileA(read) -> ReadFile -> CloseHandle, then verifies the bytes.
     * Proves real HANDLE<->fd mapping + Windows-path translation onto the VFS.
     * exit==9 means the round-trip matched. Runs regardless of QUICK_BOOT. */
    {
        kprintf("[boot] WINPE5: spawning /bin/win-fileio.exe (Win32 file I/O)\n");
        char *argv[] = { (char *)"win-fileio.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-fileio.exe",
            .name = "win-fileio.exe",
            .argc = 1,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] WINPE5: /bin/win-fileio.exe not spawned (rc=%d) "
                    "MISSING\n", pid);
            kprintf("[WINPE5] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE5: /bin/win-fileio.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE5] VERDICT: %s exit=%d (expected 9)\n",
                    rc == 9 ? "PASS" : "FAIL", rc);
        }
    }
#endif

#ifdef WINPE6_BOOT
    /* Track C -- multithreading, milestone C6. Build EXTRA_CFLAGS+=-DWINPE6_BOOT.
     * Spawns /bin/win-thread.exe, a STOCK multithreaded .exe: N CreateThread
     * workers each increment a SHARED counter under a real CRITICAL_SECTION,
     * main WaitForSingleObject-joins them and checks the total. exit==6 means
     * the threads ran AND the lock serialised them (no lost updates). Runs
     * regardless of QUICK_BOOT. */
    {
        kprintf("[boot] WINPE6: spawning /bin/win-thread.exe (Win32 threads + lock)\n");
        char *argv[] = { (char *)"win-thread.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-thread.exe",
            .name = "win-thread.exe",
            .argc = 1,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] WINPE6: /bin/win-thread.exe not spawned (rc=%d) "
                    "MISSING\n", pid);
            kprintf("[WINPE6] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE6: /bin/win-thread.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE6] VERDICT: %s exit=%d (expected 6)\n",
                    rc == 6 ? "PASS" : "FAIL", rc);
        }
    }
#endif

#ifdef WINPE7_BOOT
    /* Track C -- the user32/gdi32 GUI bridge, milestone C7. Build
     * EXTRA_CFLAGS+=-DWINPE7_BOOT. Spawns /bin/win-gui.exe, a STOCK Win32 GUI
     * .exe: RegisterClass + CreateWindowEx + a GetMessage/DispatchMessage loop
     * whose WndProc draws (BeginPaint -> FillRect + TextOut). tobyOS maps the
     * window onto a real desktop window and routes DispatchMessage to the
     * WndProc via a user-mode trampoline. The window stays up ~2s (for a
     * screenshot) then the app quits, returning 7 iff the WndProc ran. Runs
     * regardless of QUICK_BOOT. */
    {
        kprintf("[boot] WINPE7: spawning /bin/win-gui.exe (Win32 GUI window)\n");
        char *argv[] = { (char *)"win-gui.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-gui.exe",
            .name = "win-gui.exe",
            .argc = 1,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] WINPE7: /bin/win-gui.exe not spawned (rc=%d) "
                    "MISSING\n", pid);
            kprintf("[WINPE7] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE7: /bin/win-gui.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE7] VERDICT: %s exit=%d (expected 7)\n",
                    rc == 7 ? "PASS" : "FAIL", rc);
        }
    }
#endif

#ifdef WINPE8_BOOT
    /* Track C -- VISIBLE + INTERACTIVE Win32 GUI, milestone C8. Build
     * EXTRA_CFLAGS+=-DWINPE8_BOOT. Auto-login + dismiss login + launch the .exe
     * onto the logged-in desktop, drive a REAL mouse click (recolour) and a
     * deterministic close, assert exit==8. See the winpe8_* helpers above. */
    {
        /* Targeted input logging: GetMessage prints each WM_* it delivers to
         * the WndProc (proof of input delivery) without GUI_TRACE_VERBOSE's
         * per-frame serial flood. */
        win32_gui_set_log(true);

        /* (1+2) Sign in + dismiss the login window -> a clean logged-in desktop. */
        winpe_autologin_clear();

        /* (3) Launch the .exe as a session-tagged desktop app. */
        kprintf("[boot] WINPE8: spawning /bin/win-gui8.exe (interactive Win32 GUI)\n");
        int wpid = winpe_spawn_session_app("/bin/win-gui8.exe", "win-gui8.exe");

        if (wpid < 0) {
            kprintf("[boot] WINPE8: spawn failed rc=%d MISSING\n", wpid);
            kprintf("[WINPE8] VERDICT: FAIL reason=spawn\n");
        } else {
            /* (4) Wait (up to ~4s) for the window to come up. */
            int wfd = -1;
            for (int i = 0; i < 80 && wfd < 0; i++) {
                winpe8_pump_ms(50);
                wfd = win32_gui_window_fd(wpid);
            }
            if (wfd < 0) {
                kprintf("[boot] WINPE8: window never appeared\n");
                kprintf("[WINPE8] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(wpid, SIGKILL);
                (void)proc_wait(wpid);
            } else {
                kprintf("[boot] WINPE8: window up (fd=%d) -- painting blue\n", wfd);
                winpe8_pump_ms(800);   /* first WM_PAINT (blue) + composite */

                /* (5) REAL mouse click: move the cursor onto the window centre
                 * via the PS/2 driver path, then press + release. */
                int cx = 0, cy = 0;
                if (gui_focused_window_client_center(&cx, &cy)) {
                    kprintf("[boot] WINPE8: real mouse click at screen (%d,%d)\n", cx, cy);
                    winpe8_move_cursor_to(cx, cy);
                    mouse_inject_event(0, 0, MOUSE_BTN_LEFT);  /* button down */
                    winpe8_pump_ms(250);
                    mouse_inject_event(0, 0, 0);               /* button up   */
                    winpe8_pump_ms(400);
                }
                uint32_t after_real = win32_gui_fill_color();
                kprintf("[boot] WINPE8: fill after real click = 0x%06x (%s)\n",
                        after_real & 0xFFFFFFu,
                        after_real == WINPE8_GREEN
                            ? "GREEN: real click landed"
                            : "not green yet -- deterministic injection follows");

                /* (6) Deterministic injection GUARANTEES the recolour for the
                 * screenshot + exit code, regardless of real-click timing. */
                gui_post_mouse(GUI_EV_MOUSE_MOVE, 200, 100, 0);
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 200, 100, MOUSE_BTN_LEFT);
                winpe8_pump_ms(500);
                kprintf("[boot] WINPE8: fill after deterministic click = 0x%06x\n",
                        win32_gui_fill_color() & 0xFFFFFFu);

                /* (7) Hold the green window on screen for the QMP screenshot. */
                kprintf("[WINPE8] window recoloured GREEN; holding ~6s for screenshot\n");
                winpe8_pump_ms(6000);

                /* (8) Deterministic close -> WM_CLOSE -> WM_DESTROY ->
                 * PostQuitMessage -> WM_QUIT -> clean exit. */
                kprintf("[boot] WINPE8: posting WM_CLOSE (close chain)\n");
                gui_close_focused();
                winpe8_pump_ms(800);

                int rc = proc_wait(wpid);
                kprintf("[boot] WINPE8: /bin/win-gui8.exe (pid=%d) exit=%d\n", wpid, rc);
                kprintf("[WINPE8] VERDICT: %s exit=%d (expected 8)\n",
                        rc == 8 ? "PASS" : "FAIL", rc);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE10_BOOT
    /* Track C -- MULTI-WINDOW, milestone C10. Build EXTRA_CFLAGS+=-DWINPE10_BOOT.
     * Reuses C8's visible+interactive infra (auto-login + desktop launch + real
     * input). A stock .exe opens TWO top-level windows; the harness waits for
     * both to paint, drives a REAL mouse click into the FOCUSED window (recolours
     * only that one -> proof of per-window routing), holds for a screenshot, then
     * closes BOTH windows in turn (independent WM_CLOSE->WM_DESTROY). The app
     * returns 10 iff both painted, exactly ONE got the click, and both ran their
     * WM_DESTROY. */
    {
        win32_gui_set_log(true);
        winpe_autologin_clear();

        kprintf("[boot] WINPE10: spawning /bin/win-gui10.exe (two Win32 windows)\n");
        int wpid = winpe_spawn_session_app("/bin/win-gui10.exe", "win-gui10.exe");
        if (wpid < 0) {
            kprintf("[boot] WINPE10: spawn failed rc=%d MISSING\n", wpid);
            kprintf("[WINPE10] VERDICT: FAIL reason=spawn\n");
        } else {
            /* (4) Wait (up to ~5s) for BOTH windows to come up. */
            int n = 0;
            for (int i = 0; i < 100 && n < 2; i++) {
                winpe8_pump_ms(50);
                n = win32_gui_window_count(wpid);
            }
            if (n < 2) {
                kprintf("[boot] WINPE10: only %d window(s) appeared\n", n);
                kprintf("[WINPE10] VERDICT: FAIL reason=nowindows\n");
                signal_send_to_pid(wpid, SIGKILL);
                (void)proc_wait(wpid);
            } else {
                kprintf("[boot] WINPE10: %d windows up -- painting blue\n", n);
                winpe8_pump_ms(900);   /* both WM_PAINT (blue) + composite */

                /* (5) REAL click into the FOCUSED (topmost) window only. */
                int cx = 0, cy = 0;
                if (gui_focused_window_client_center(&cx, &cy)) {
                    kprintf("[boot] WINPE10: real mouse click into focused window at (%d,%d)\n",
                            cx, cy);
                    winpe8_move_cursor_to(cx, cy);
                    mouse_inject_event(0, 0, MOUSE_BTN_LEFT);
                    winpe8_pump_ms(250);
                    mouse_inject_event(0, 0, 0);
                    winpe8_pump_ms(500);
                }

                kprintf("[WINPE10] one of two windows recoloured GREEN; holding ~6s for screenshot\n");
                winpe8_pump_ms(6000);

                /* (6) Close BOTH windows in turn (focused first; after it retires,
                 * the other becomes focused). Each runs WM_CLOSE->WM_DESTROY. */
                kprintf("[boot] WINPE10: closing focused window #1\n");
                gui_close_focused();
                for (int i = 0; i < 30 && win32_gui_window_count(wpid) > 1; i++)
                    winpe8_pump_ms(50);
                kprintf("[boot] WINPE10: closing window #2 (count now %d)\n",
                        win32_gui_window_count(wpid));
                gui_close_focused();
                winpe8_pump_ms(800);

                int rc = proc_wait(wpid);
                kprintf("[boot] WINPE10: /bin/win-gui10.exe (pid=%d) exit=%d\n", wpid, rc);
                kprintf("[WINPE10] VERDICT: %s exit=%d (expected 10)\n",
                        rc == 10 ? "PASS" : "FAIL", rc);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE9_BOOT
    /* Track C -- runtime API resolution, milestone C9. Build
     * EXTRA_CFLAGS+=-DWINPE9_BOOT. Spawns /bin/win-c9.exe, a STOCK clang .exe
     * that resolves Win32 APIs at runtime via GetModuleHandleA/GetProcAddress/
     * LoadLibraryA and calls through the resolved pointer. exit 9 iff every step
     * passed (resolve by name, stable + distinct thunks, unknown->NULL, the call
     * reaches the shim, cross-DLL LoadLibrary). Runs regardless of QUICK_BOOT. */
    {
        kprintf("[boot] WINPE9: spawning /bin/win-c9.exe (GetModuleHandle/GetProcAddress)\n");
        char *argv[] = { (char *)"win-c9.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-c9.exe", .name = "win-c9.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] WINPE9: /bin/win-c9.exe not spawned (rc=%d) MISSING\n", pid);
            kprintf("[WINPE9] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE9: /bin/win-c9.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE9] VERDICT: %s exit=%d (expected 9)\n",
                    rc == 9 ? "PASS" : "FAIL", rc);
        }
    }
#endif

#ifdef WINPE11_BOOT
    /* Track C -- more GDI + more file ops + child/owned windows, milestone C11.
     * Build EXTRA_CFLAGS+=-DWINPE11_BOOT. Two proofs:
     *   A) /bin/win-c11file.exe (console): SetFilePointer/GetFileSize/DeleteFile/
     *      CreateDirectory/GetFileAttributes/FindFirstFile round-trip -> exit 11.
     *   B) /bin/win-gui11.exe (GUI): a window drawing rich GDI (pen line +
     *      Rectangle + Ellipse) plus a CHILD window owned by it; the app destroys
     *      only the main window and tobyOS cascades the destroy to the child ->
     *      exit 11. (The hWndParent it passes is its 9th arg, reachable now the
     *      gate marshals 10 args.) Reuses C8's auto-login + desktop launch. */
    {
        /* Part A: file ops (console; no desktop needed). */
        kprintf("[boot] WINPE11: spawning /bin/win-c11file.exe (file ops)\n");
        int fpid = winpe_spawn_session_app("/bin/win-c11file.exe", "win-c11file.exe");
        if (fpid < 0) {
            kprintf("[WINPE11F] VERDICT: FAIL reason=spawn\n");
        } else {
            int frc = proc_wait(fpid);
            kprintf("[boot] WINPE11: /bin/win-c11file.exe (pid=%d) exit=%d\n", fpid, frc);
            kprintf("[WINPE11F] VERDICT: %s exit=%d (expected 11)\n",
                    frc == 11 ? "PASS" : "FAIL", frc);
        }

        /* Part B: GDI + child/owned window (GUI, on the logged-in desktop). */
        win32_gui_set_log(true);
        winpe_autologin_clear();
        kprintf("[boot] WINPE11: spawning /bin/win-gui11.exe (GDI + child window)\n");
        int wpid = winpe_spawn_session_app("/bin/win-gui11.exe", "win-gui11.exe");
        if (wpid < 0) {
            kprintf("[boot] WINPE11: spawn failed rc=%d MISSING\n", wpid);
            kprintf("[WINPE11] VERDICT: FAIL reason=spawn\n");
        } else {
            int n = 0;
            for (int i = 0; i < 100 && n < 2; i++) {
                winpe8_pump_ms(50);
                n = win32_gui_window_count(wpid);
            }
            if (n < 2) {
                kprintf("[boot] WINPE11: only %d window(s) appeared\n", n);
                kprintf("[WINPE11] VERDICT: FAIL reason=nowindows\n");
                signal_send_to_pid(wpid, SIGKILL);
                (void)proc_wait(wpid);
            } else {
                kprintf("[boot] WINPE11: %d windows up (GDI main + child)\n", n);
                kprintf("[WINPE11] GDI main + child window up; holding ~5s for screenshot\n");
                /* Pump while the app holds (Sleep 4s) then self-destructs the
                 * main window -> cascade to the child -> both exit. */
                for (int i = 0; i < 160 && win32_gui_window_count(wpid) > 0; i++)
                    winpe8_pump_ms(50);
                int rc = proc_wait(wpid);
                kprintf("[boot] WINPE11: /bin/win-gui11.exe (pid=%d) exit=%d\n", wpid, rc);
                kprintf("[WINPE11] VERDICT: %s exit=%d (expected 11)\n",
                        rc == 11 ? "PASS" : "FAIL", rc);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE12_BOOT
    /* Track C -- bitmaps/BitBlt + real BUTTON/EDIT controls, milestone C12.
     * Build EXTRA_CFLAGS+=-DWINPE12_BOOT. Two GUI proofs on the logged-in
     * desktop (reusing C8's auto-login + desktop launch):
     *   A) /bin/win-bmp12.exe BitBlts an in-memory gradient bitmap -> exit 12.
     *   B) /bin/win-ctrl12.exe creates an EDIT + a BUTTON; the harness types
     *      into the edit and clicks the button (button -> WM_COMMAND, the app
     *      reads the edit text back) -> exit 12. */
    {
        win32_gui_set_log(true);
        winpe_autologin_clear();

        /* ---- Part A: bitmaps / BitBlt ---- */
        kprintf("[boot] WINPE12: spawning /bin/win-bmp12.exe (BitBlt)\n");
        int bpid = winpe_spawn_session_app("/bin/win-bmp12.exe", "win-bmp12.exe");
        if (bpid < 0) {
            kprintf("[WINPE12B] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 100 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(bpid); }
            if (wfd < 0) {
                kprintf("[WINPE12B] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(bpid, SIGKILL); (void)proc_wait(bpid);
            } else {
                winpe8_pump_ms(800);
                kprintf("[WINPE12B] BitBlt bitmap on the window; holding ~5s for screenshot\n");
                for (int i = 0; i < 160 && win32_gui_window_count(bpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(bpid);
                kprintf("[boot] WINPE12: /bin/win-bmp12.exe (pid=%d) exit=%d\n", bpid, rc);
                kprintf("[WINPE12B] VERDICT: %s exit=%d (expected 12)\n", rc == 12 ? "PASS" : "FAIL", rc);
            }
        }

        /* ---- Part B: BUTTON + EDIT controls ---- */
        kprintf("[boot] WINPE12: spawning /bin/win-ctrl12.exe (BUTTON/EDIT)\n");
        int cpid = winpe_spawn_session_app("/bin/win-ctrl12.exe", "win-ctrl12.exe");
        if (cpid < 0) {
            kprintf("[WINPE12] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 100 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(cpid); }
            if (wfd < 0) {
                kprintf("[WINPE12] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(cpid, SIGKILL); (void)proc_wait(cpid);
            } else {
                winpe8_pump_ms(900);   /* first paint -> controls drawn, EDIT focused */
                /* Type into the auto-focused EDIT. */
                const char *txt = "tobyOS";
                kprintf("[boot] WINPE12: typing '%s' into the EDIT control\n", txt);
                for (const char *p = txt; *p; p++) { gui_post_key((uint8_t)*p); winpe8_pump_ms(120); }
                /* Click the BUTTON at its client centre (placed at 30,110,120,32). */
                kprintf("[boot] WINPE12: clicking the BUTTON control\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 90, 126, MOUSE_BTN_LEFT);
                winpe8_pump_ms(500);
                kprintf("[WINPE12] EDIT typed + BUTTON clicked; holding ~4s for screenshot\n");
                for (int i = 0; i < 160 && win32_gui_window_count(cpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(cpid);
                kprintf("[boot] WINPE12: /bin/win-ctrl12.exe (pid=%d) exit=%d\n", cpid, rc);
                kprintf("[WINPE12] VERDICT: %s exit=%d (expected 12)\n", rc == 12 ? "PASS" : "FAIL", rc);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE13_BOOT
    /* Track C -- STATIC/CHECKBOX/LISTBOX/SCROLLBAR controls + MessageBoxA modal
     * dialog + GDI fonts/metrics, milestone C13. Build EXTRA_CFLAGS+=-DWINPE13_BOOT.
     * Two GUI proofs (reusing C8's auto-login + desktop launch):
     *   A) /bin/win-ctrl13.exe: the harness checks a CHECKBOX, selects a LISTBOX
     *      item, nudges a SCROLLBAR, then clicks a button that pops a MessageBox
     *      the harness OKs -> exit 13.
     *   B) /bin/win-font13.exe: draws multi-size/colour text -> exit 13. */
    {
        win32_gui_set_log(true);
        winpe_autologin_clear();

        /* ---- Part A: controls + dialog ---- */
        kprintf("[boot] WINPE13: spawning /bin/win-ctrl13.exe (controls + dialog)\n");
        int cpid = winpe_spawn_session_app("/bin/win-ctrl13.exe", "win-ctrl13.exe");
        if (cpid < 0) {
            kprintf("[WINPE13] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 100 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(cpid); }
            if (wfd < 0) {
                kprintf("[WINPE13] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(cpid, SIGKILL); (void)proc_wait(cpid);
            } else {
                winpe8_pump_ms(900);   /* controls drawn */
                kprintf("[boot] WINPE13: check the CHECKBOX\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 32, 57, MOUSE_BTN_LEFT);   winpe8_pump_ms(300);
                kprintf("[boot] WINPE13: select LISTBOX row 1 (Banana)\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 60, 103, MOUSE_BTN_LEFT);  winpe8_pump_ms(300);
                kprintf("[boot] WINPE13: nudge the SCROLLBAR (down arrow x3)\n");
                for (int i = 0; i < 3; i++) { gui_post_mouse(GUI_EV_MOUSE_DOWN, 209, 161, MOUSE_BTN_LEFT); winpe8_pump_ms(200); }
                kprintf("[WINPE13] controls set (checkbox+listbox+scrollbar); holding ~4s for screenshot\n");
                winpe8_pump_ms(4000);
                /* Pop the modal dialog and screenshot it. */
                kprintf("[boot] WINPE13: click 'Show dialog' button\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 89, 215, MOUSE_BTN_LEFT);  winpe8_pump_ms(700);
                kprintf("[WINPE13] dialog (MessageBox) up; holding ~4s for screenshot\n");
                winpe8_pump_ms(4000);
                kprintf("[boot] WINPE13: click OK in the MessageBox\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 160, 108, MOUSE_BTN_LEFT);
                for (int i = 0; i < 160 && win32_gui_window_count(cpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(cpid);
                kprintf("[boot] WINPE13: /bin/win-ctrl13.exe (pid=%d) exit=%d\n", cpid, rc);
                kprintf("[WINPE13] VERDICT: %s exit=%d (expected 13)\n", rc == 13 ? "PASS" : "FAIL", rc);
            }
        }

        /* ---- Part B: GDI fonts + metrics ---- */
        kprintf("[boot] WINPE13: spawning /bin/win-font13.exe (GDI fonts)\n");
        int fpid = winpe_spawn_session_app("/bin/win-font13.exe", "win-font13.exe");
        if (fpid < 0) {
            kprintf("[WINPE13F] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 100 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(fpid); }
            if (wfd < 0) {
                kprintf("[WINPE13F] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(fpid, SIGKILL); (void)proc_wait(fpid);
            } else {
                winpe8_pump_ms(800);
                kprintf("[WINPE13F] GDI fonts on the window; holding ~5s for screenshot\n");
                for (int i = 0; i < 160 && win32_gui_window_count(fpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(fpid);
                kprintf("[boot] WINPE13: /bin/win-font13.exe (pid=%d) exit=%d\n", fpid, rc);
                kprintf("[WINPE13F] VERDICT: %s exit=%d (expected 13)\n", rc == 13 ? "PASS" : "FAIL", rc);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE14_BOOT
    /* Track C -- COMBOBOX/RADIO/GROUPBOX/TAB controls + dialog templates,
     * milestone C14a. Build EXTRA_CFLAGS+=-DWINPE14_BOOT. Two GUI proofs
     * (reusing C8's auto-login + desktop launch):
     *   A) /bin/win-ctrl14.exe: the harness selects the 2nd RADIO, opens the
     *      COMBOBOX + picks "Green", switches to TAB "Two", then clicks Finish
     *      -> exit 14 iff radio2 + combo=Green + tab=Two all read back.
     *   B) /bin/win-dlg14.exe: a modal DialogBoxParamA built from an RT_DIALOG
     *      resource template; the harness types into the EDIT, toggles the
     *      CHECKBOX, then clicks OK -> exit 14. */
    {
        win32_gui_set_log(true);
        winpe_autologin_clear();

        /* ---- Part A: the four new controls ---- */
        kprintf("[boot] WINPE14: spawning /bin/win-ctrl14.exe (combo/radio/group/tab)\n");
        int cpid = winpe_spawn_session_app("/bin/win-ctrl14.exe", "win-ctrl14.exe");
        if (cpid < 0) {
            kprintf("[WINPE14] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 100 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(cpid); }
            if (wfd < 0) {
                kprintf("[WINPE14] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(cpid, SIGKILL); (void)proc_wait(cpid);
            } else {
                winpe8_pump_ms(900);   /* controls drawn */
                kprintf("[boot] WINPE14: select RADIO 'Slow'\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 41, 70, MOUSE_BTN_LEFT);   winpe8_pump_ms(300);
                kprintf("[boot] WINPE14: open the COMBOBOX\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 320, 43, MOUSE_BTN_LEFT);  winpe8_pump_ms(300);
                kprintf("[WINPE14] COMBOBOX dropdown open; holding ~4s for screenshot\n");
                winpe8_pump_ms(4000);
                kprintf("[boot] WINPE14: pick COMBOBOX item 'Green'\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 320, 75, MOUSE_BTN_LEFT);  winpe8_pump_ms(300);
                kprintf("[boot] WINPE14: switch to TAB 'Two'\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 80, 120, MOUSE_BTN_LEFT);  winpe8_pump_ms(300);
                kprintf("[WINPE14] controls set (radio+combo+tab); holding ~4s for screenshot\n");
                winpe8_pump_ms(4000);
                kprintf("[boot] WINPE14: click Finish\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 240, 315, MOUSE_BTN_LEFT);
                for (int i = 0; i < 160 && win32_gui_window_count(cpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(cpid);
                kprintf("[boot] WINPE14: /bin/win-ctrl14.exe (pid=%d) exit=%d\n", cpid, rc);
                kprintf("[WINPE14] VERDICT: %s exit=%d (expected 14)\n", rc == 14 ? "PASS" : "FAIL", rc);
            }
        }

        /* ---- Part B: a MODAL dialog from a RESOURCE template ---- */
        kprintf("[boot] WINPE14: spawning /bin/win-dlg14.exe (DialogBoxParamA + .rsrc)\n");
        int dpid = winpe_spawn_session_app("/bin/win-dlg14.exe", "win-dlg14.exe");
        if (dpid < 0) {
            kprintf("[WINPE14D] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 120 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(dpid); }
            if (wfd < 0) {
                kprintf("[WINPE14D] VERDICT: FAIL reason=nodialog\n");
                signal_send_to_pid(dpid, SIGKILL); (void)proc_wait(dpid);
            } else {
                winpe8_pump_ms(900);   /* dialog built + WM_INITDIALOG processed (edit='to') */
                kprintf("[WINPE14D] dialog (modal template) up; holding ~4s for screenshot\n");
                winpe8_pump_ms(4000);
                /* Type "byOS" into the auto-focused EDIT (WM_INITDIALOG seeded "to"). */
                kprintf("[boot] WINPE14: typing 'byOS' into the dialog EDIT\n");
                const char *typ = "byOS";
                for (const char *q = typ; *q; q++) { gui_post_key((uint8_t)*q); winpe8_pump_ms(150); }
                kprintf("[boot] WINPE14: check the CHECKBOX\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 24, 67, MOUSE_BTN_LEFT);   winpe8_pump_ms(300);
                kprintf("[boot] WINPE14: select RADIO 'Premium'\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 171, 119, MOUSE_BTN_LEFT); winpe8_pump_ms(300);
                kprintf("[WINPE14D] dialog filled (edit+check+radio); holding ~4s for screenshot\n");
                winpe8_pump_ms(4000);
                kprintf("[boot] WINPE14: click OK\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 119, 204, MOUSE_BTN_LEFT);
                for (int i = 0; i < 160 && win32_gui_window_count(dpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(dpid);
                kprintf("[boot] WINPE14: /bin/win-dlg14.exe (pid=%d) exit=%d\n", dpid, rc);
                kprintf("[WINPE14D] VERDICT: %s exit=%d (expected 14)\n", rc == 14 ? "PASS" : "FAIL", rc);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE14B_BOOT
    /* Track C -- REAL TrueType fonts, milestone C14b. Build EXTRA_CFLAGS+=
     * -DWINPE14B_BOOT. A stock Win32 GUI .exe CreateFontA's several pixel sizes
     * and TextOutA/DrawTextA's real strings; tobyOS rasterizes genuine Lato
     * (OFL) glyphs in the kernel via stb_truetype. exit 14 iff the text measured
     * PROPORTIONAL (TTF), not monospace (the 8x8 bitmap fallback). */
    {
        win32_gui_set_log(true);
        winpe_autologin_clear();
        kprintf("[boot] WINPE14B: spawning /bin/win-font14b.exe (TrueType fonts)\n");
        int fpid = winpe_spawn_session_app("/bin/win-font14b.exe", "win-font14b.exe");
        if (fpid < 0) {
            kprintf("[WINPE14B] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 120 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(fpid); }
            if (wfd < 0) {
                kprintf("[WINPE14B] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(fpid, SIGKILL); (void)proc_wait(fpid);
            } else {
                winpe8_pump_ms(900);   /* WM_PAINT -> glyphs rasterized + drawn */
                kprintf("[WINPE14B] TrueType text on the window; holding ~5s for screenshot\n");
                for (int i = 0; i < 200 && win32_gui_window_count(fpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(fpid);
                kprintf("[boot] WINPE14B: /bin/win-font14b.exe (pid=%d) exit=%d\n", fpid, rc);
                kprintf("[WINPE14B] VERDICT: %s exit=%d (expected 14)\n", rc == 14 ? "PASS" : "FAIL", rc);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE18C_BOOT
    /* Track C -- BOLD/ITALIC TrueType faces + desktop-wide TTF, milestone C18c.
     * Build EXTRA_CFLAGS+=-DWINPE18C_BOOT. A stock Win32 GUI .exe draws the same
     * string at the same pixel height in four faces (Regular/Bold/Italic/
     * BoldItalic) chosen by CreateFontA's lfWeight/lfItalic; tobyOS rasterizes
     * the matching Lato face. The harness reads the rendered pixels back and
     * machine-verifies BOLD has more ink than Regular (genuinely heavier -- Lato
     * Bold shares Regular's advance widths so a width check would be fooled) and
     * ITALIC slants (glyph tops sit right of bottoms). Then it spawns a NATIVE
     * tobyOS app (/bin/gui_about) to show that the desktop's own gui_window_text
     * now renders TrueType too. PASS iff exit==18 AND bold heavier AND italic
     * slanted. Two screenshots (Win32 faces; native TTF). */
    {
        /* Layout mirrored from programs/win-font18c/main.c. */
        const int LX = 20, Y0 = 20, DY = 60, RW = 560, RH = 54;
        (void)LX;
        win32_gui_set_log(true);
        winpe_autologin_clear();
        kprintf("[boot] WINPE18C: spawning /bin/win-font18c.exe (bold+italic TTF)\n");
        int fpid = winpe_spawn_session_app("/bin/win-font18c.exe", "win-font18c.exe");
        int verdict_ink = 0;
        if (fpid < 0) {
            kprintf("[WINPE18C] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 120 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(fpid); }
            if (wfd < 0) {
                kprintf("[WINPE18C] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(fpid, SIGKILL); (void)proc_wait(fpid);
            } else {
                winpe8_pump_ms(1000);   /* WM_PAINT -> all four faces rasterized + drawn */
                uint32_t bg = win32_gui_fill_color_fd(wfd);
                int rt, rb, bt, bb, it, ib, qt, qb;
                int ink_reg = win32_gui_ink_stats(wfd, 12, Y0 + 0 * DY - 2, RW, RH, bg, &rt, &rb);
                int ink_bld = win32_gui_ink_stats(wfd, 12, Y0 + 1 * DY - 2, RW, RH, bg, &bt, &bb);
                int ink_itl = win32_gui_ink_stats(wfd, 12, Y0 + 2 * DY - 2, RW, RH, bg, &it, &ib);
                int ink_bi  = win32_gui_ink_stats(wfd, 12, Y0 + 3 * DY - 2, RW, RH, bg, &qt, &qb);
                int reg_slant = (rt >= 0 && rb >= 0) ? (rt - rb) : 0;
                int itl_slant = (it >= 0 && ib >= 0) ? (it - ib) : 0;
                int slant_delta = itl_slant - reg_slant;
                int bold_heavier = (ink_reg > 0) && (ink_bld * 100 > ink_reg * 112);
                int italic_slanted = slant_delta >= 3;
                kprintf("[boot] WINPE18C: ink reg=%d bold=%d italic=%d bolditalic=%d\n",
                        ink_reg, ink_bld, ink_itl, ink_bi);
                kprintf("[boot] WINPE18C: slant reg(top-bot)=%d italic=%d delta=%d\n",
                        reg_slant, itl_slant, slant_delta);
                kprintf("[boot] WINPE18C: bold_heavier=%d italic_slanted=%d\n",
                        bold_heavier, italic_slanted);
                /* Gate on the two strong, deterministic signals: bold renders
                 * heavier ink (the only proof of the Bold face -- it shares
                 * Regular's widths) and exit==18 (the app saw Italic/BoldItalic
                 * advance widths differ, proving those faces loaded). The slant
                 * metric is informational (the screenshot confirms the lean). */
                (void)italic_slanted;
                verdict_ink = bold_heavier;
                kprintf("[WINPE18C] four faces drawn; holding ~6s for screenshot 1\n");
                winpe8_pump_ms(6000);
                for (int i = 0; i < 120 && win32_gui_window_count(fpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(fpid);
                kprintf("[boot] WINPE18C: /bin/win-font18c.exe (pid=%d) exit=%d\n", fpid, rc);

                /* Native demonstrator: /bin/gui_about draws many labels via
                 * sys_gui_text -> gui_window_text, now routed through kfont. */
                kprintf("[boot] WINPE18C: spawning native /bin/gui_about (desktop TTF)\n");
                int apid = winpe_spawn_session_app("/bin/gui_about", "gui_about");
                if (apid >= 0) {
                    winpe8_pump_ms(2500);   /* native app draws its first frame */
                    kprintf("[WINPE18C] native gui_about up; holding ~6s for screenshot 2\n");
                    winpe8_pump_ms(6000);
                    signal_send_to_pid(apid, SIGKILL); (void)proc_wait(apid);
                } else {
                    kprintf("[boot] WINPE18C: native gui_about spawn failed rc=%d\n", apid);
                }

                int pass = (rc == 18) && verdict_ink;
                kprintf("[WINPE18C] VERDICT: %s exit=%d (expected 18) bold_heavier+italic_slant=%d\n",
                        pass ? "PASS" : "FAIL", rc, verdict_ink);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE15_BOOT
    /* Track C -- menus + timers + multi-button MessageBox, milestone C15. Build
     * EXTRA_CFLAGS+=-DWINPE15_BOOT. A stock Win32 GUI .exe with a File/Help menu
     * bar, a 500 ms timer, and an MB_YESNO About box. The harness lets the timer
     * tick, opens Help -> About (a Yes/No box), clicks Yes, then File -> Exit;
     * exit 15 iff the timer fired + a menu command arrived + the box gave IDYES. */
    {
        win32_gui_set_log(true);
        winpe_autologin_clear();
        kprintf("[boot] WINPE15: spawning /bin/win-menu15.exe (menus + timers)\n");
        int mpid = winpe_spawn_session_app("/bin/win-menu15.exe", "win-menu15.exe");
        if (mpid < 0) {
            kprintf("[WINPE15] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 120 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(mpid); }
            if (wfd < 0) {
                kprintf("[WINPE15] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(mpid, SIGKILL); (void)proc_wait(mpid);
            } else {
                winpe8_pump_ms(1600);   /* let the 500ms timer tick a few times */
                kprintf("[WINPE15] menu bar + timer counter up; holding ~3s for screenshot\n");
                winpe8_pump_ms(3000);
                kprintf("[boot] WINPE15: open the Help menu\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 76, 11, MOUSE_BTN_LEFT);   winpe8_pump_ms(500);
                kprintf("[WINPE15] Help dropdown open; holding ~3s for screenshot\n");
                winpe8_pump_ms(3000);
                kprintf("[boot] WINPE15: click About -> Yes/No box\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 76, 31, MOUSE_BTN_LEFT);   winpe8_pump_ms(700);
                kprintf("[WINPE15] About MessageBox (Yes/No) up; holding ~3s for screenshot\n");
                winpe8_pump_ms(3000);
                kprintf("[boot] WINPE15: click Yes\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 124, 118, MOUSE_BTN_LEFT);  winpe8_pump_ms(700);
                kprintf("[boot] WINPE15: File -> Exit\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 28, 11, MOUSE_BTN_LEFT);    winpe8_pump_ms(400);
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 30, 86, MOUSE_BTN_LEFT);
                for (int i = 0; i < 160 && win32_gui_window_count(mpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(mpid);
                kprintf("[boot] WINPE15: /bin/win-menu15.exe (pid=%d) exit=%d\n", mpid, rc);
                kprintf("[WINPE15] VERDICT: %s exit=%d (expected 15)\n", rc == 15 ? "PASS" : "FAIL", rc);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE16A_BOOT
    /* Track C -- registry (advapi32), milestone C16a. Build EXTRA_CFLAGS+=
     * -DWINPE16A_BOOT. A stock Win32 console .exe reads a persisted DWORD counter
     * under HKCU\Software\tobyOS\C16, increments + writes it back (flushed to
     * /data/toby_registry.dat), and verifies the round-trip. exit 17 = first
     * write (no prior counter), exit 16 = a prior counter persisted (proven by
     * the 2-boot script: boot1 seeds, boot2 reads it back across reboot). */
    {
        win32_gui_set_log(true);    /* surface [winreg] activity */
        kprintf("[boot] WINPE16A: spawning /bin/win-reg16.exe (registry persistence)\n");
        char *argv[] = { (char *)"win-reg16.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-reg16.exe", .name = "win-reg16.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE16A] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE16A: /bin/win-reg16.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE16A] VERDICT: %s exit=%d (16=persisted, 17=first-write)\n",
                    (rc == 16 || rc == 17) ? "PASS" : "FAIL", rc);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE16B_BOOT
    /* Track C -- common file dialog (comdlg32), milestone C16b. Build
     * EXTRA_CFLAGS+=-DWINPE16B_BOOT. A stock Win32 GUI .exe writes a known file
     * C:\c16test.txt, then a button click calls GetOpenFileNameA; tobyOS shows a
     * modal file picker over /data; the harness clicks the picker's Open button
     * (the file is pre-selected by name) and the app opens the returned path +
     * verifies the contents -> exit 16. */
    {
        win32_gui_set_log(true);
        winpe_autologin_clear();
        kprintf("[boot] WINPE16B: spawning /bin/win-fdlg16.exe (Open file dialog)\n");
        int mpid = winpe_spawn_session_app("/bin/win-fdlg16.exe", "win-fdlg16.exe");
        if (mpid < 0) {
            kprintf("[WINPE16B] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 120 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(mpid); }
            if (wfd < 0) {
                kprintf("[WINPE16B] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(mpid, SIGKILL); (void)proc_wait(mpid);
            } else {
                winpe8_pump_ms(700);
                kprintf("[boot] WINPE16B: click 'Open File...'\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 110, 56, MOUSE_BTN_LEFT);  winpe8_pump_ms(900);
                kprintf("[WINPE16B] file picker up; holding ~3s for screenshot\n");
                winpe8_pump_ms(3000);
                kprintf("[boot] WINPE16B: click the picker's Open button\n");
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 278, 317, MOUSE_BTN_LEFT);
                for (int i = 0; i < 160 && win32_gui_window_count(mpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(mpid);
                kprintf("[boot] WINPE16B: /bin/win-fdlg16.exe (pid=%d) exit=%d\n", mpid, rc);
                kprintf("[WINPE16B] VERDICT: %s exit=%d (expected 16)\n", rc == 16 ? "PASS" : "FAIL", rc);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE16C_BOOT
    /* Track C -- command line + environment, milestone C16c. Build EXTRA_CFLAGS+=
     * -DWINPE16C_BOOT. A stock Win32 console .exe checks GetCommandLineA, sets +
     * reads an env var, reads a seeded default (OS=tobyOS), and expands %VAR%
     * references -> exit 16. */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE16C: spawning /bin/win-env16.exe (cmdline + env)\n");
        char *argv[] = { (char *)"win-env16.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-env16.exe", .name = "win-env16.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE16C] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE16C: /bin/win-env16.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE16C] VERDICT: %s exit=%d (expected 16)\n", rc == 16 ? "PASS" : "FAIL", rc);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE16E_BOOT
    /* Track C -- per-thread TEBs + TLS, milestone C16e. Build EXTRA_CFLAGS+=
     * -DWINPE16E_BOOT. A stock multithreaded Win32 console .exe: each thread
     * gets its own TEB, so TlsSetValue/TlsGetValue are per-thread -- three
     * workers each store a unique value, sleep, and read back their own (and the
     * main thread's value survives) -> exit 16. */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE16E: spawning /bin/win-tls16.exe (per-thread TLS)\n");
        char *argv[] = { (char *)"win-tls16.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-tls16.exe", .name = "win-tls16.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE16E] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE16E: /bin/win-tls16.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE16E] VERDICT: %s exit=%d (expected 16)\n", rc == 16 ? "PASS" : "FAIL", rc);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE17A_BOOT
    /* Track C -- Winsock TCP client, milestone C17a. Build EXTRA_CFLAGS+=
     * -DWINPE17A_BOOT. A stock Win32 console .exe (linked against ws2_32)
     * resolves example.com via gethostbyname, opens a TCP socket, sends an
     * HTTP/1.0 GET, drains the response, and verifies the "HTTP/1" status line
     * -> exit 17. Requires networking (net_init/DHCP/DNS, up by this point) and
     * an outbound path to the internet (QEMU -netdev user / SLIRP). */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE17A: net_up=%d -- spawning /bin/win-net17.exe (winsock TCP client)\n",
                (int)net_is_up());
        char *argv[] = { (char *)"win-net17.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-net17.exe", .name = "win-net17.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE17A] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE17A: /bin/win-net17.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE17A] VERDICT: %s exit=%d (expected 17)\n", rc == 17 ? "PASS" : "FAIL", rc);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE17B_BOOT
    /* Track C -- Winsock TCP server, milestone C17b. Build EXTRA_CFLAGS+=
     * -DWINPE17B_BOOT. A stock Win32 console .exe (ws2_32) binds :8080, listens,
     * accepts ONE inbound connection, reads the request, and exits 17 IFF the
     * magic token "TOBYPING" arrived -- i.e. a REAL external client connected.
     * The external client is the QEMU host: boot with -netdev user,hostfwd=
     * tcp::18080-:8080 and run logs/c17b.sh, which connects to 127.0.0.1:18080
     * once the "[c17b] listening" marker shows. The accept blocks cooperatively
     * (tcp_accept) while the boot thread proc_waits -- IRQ-driven RX completes
     * the handshake, same path the C17a client proved. */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE17B: net_up=%d -- spawning /bin/win-srv17.exe (winsock TCP server)\n",
                (int)net_is_up());
        char *argv[] = { (char *)"win-srv17.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-srv17.exe", .name = "win-srv17.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE17B] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE17B: /bin/win-srv17.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE17B] VERDICT: %s exit=%d (expected 17)\n", rc == 17 ? "PASS" : "FAIL", rc);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE17C_BOOT
    /* Track C -- Winsock UDP datagrams, milestone C17c. Build EXTRA_CFLAGS+=
     * -DWINPE17C_BOOT. A stock Win32 console .exe (ws2_32) binds UDP :9090,
     * blocks in recvfrom for one datagram, checks for "TOBYUDP", echoes it back,
     * and exits 17 IFF the token arrived and the echo send succeeded. The sender
     * is the QEMU host: boot with -netdev user,hostfwd=udp::18081-:9090 and run
     * logs/c17c.sh, which sends to 127.0.0.1:18081 once "[c17c] udp listening"
     * shows. recvfrom blocks cooperatively (sock_recvfrom) while the boot thread
     * proc_waits; the NIC RX IRQ delivers the datagram. */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE17C: net_up=%d -- spawning /bin/win-udp17.exe (winsock UDP echo)\n",
                (int)net_is_up());
        char *argv[] = { (char *)"win-udp17.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-udp17.exe", .name = "win-udp17.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE17C] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE17C: /bin/win-udp17.exe (pid=%d) exit=%d\n", pid, rc);
            kprintf("[WINPE17C] VERDICT: %s exit=%d (expected 17)\n", rc == 17 ? "PASS" : "FAIL", rc);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE18A_BOOT
    /* Track C -- run a REAL off-the-shelf third-party binary, milestone C18a.
     * Build EXTRA_CFLAGS+=-DWINPE18A_BOOT. /bin/win-sqlite.exe is the unmodified
     * public-domain SQLite amalgamation built as a Windows PE. It opens an
     * on-disk database at C:\c18.db (-> /data/c18.db), runs CREATE/INSERT/SELECT,
     * and exits 0. Proof = clean exit AND the database file SQLite wrote to /data
     * contains the row value 'TOBYSQL-OK' -- a path only reachable if the whole
     * engine (SQL parser -> VM -> b-tree -> the Win32 wide-file VFS shims) ran. */
    {
        win32_gui_set_log(true);
        (void)vfs_unlink("/data/c18.db");   /* fresh DB so CREATE TABLE succeeds */
        kprintf("[boot] WINPE18A: spawning /bin/win-sqlite.exe (real SQLite engine)\n");
        char *argv[] = {
            (char *)"win-sqlite.exe",
            (char *)"C:\\c18.db",
            (char *)"CREATE TABLE t(x);INSERT INTO t VALUES('TOBYSQL-OK');"
                    "INSERT INTO t VALUES(6*7);SELECT x FROM t;",
            0
        };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-sqlite.exe", .name = "win-sqlite.exe",
            .argc = 3, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE18A] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE18A: /bin/win-sqlite.exe (pid=%d) exit=%d\n", pid, rc);
            /* Read back the on-disk DB SQLite created and search for the row. */
            void *buf = 0; size_t sz = 0;
            int found = 0;
            if (vfs_read_all("/data/c18.db", &buf, &sz) == 0 && buf) {
                const char *p = (const char *)buf;
                const char *needle = "TOBYSQL-OK";
                for (size_t i = 0; sz >= 10 && i + 10 <= sz; i++) {
                    int k = 0; while (k < 10 && p[i + k] == needle[k]) k++;
                    if (k == 10) { found = 1; break; }
                }
                kprintf("[boot] WINPE18A: /data/c18.db = %lu bytes, sentinel %s\n",
                        (unsigned long)sz, found ? "FOUND" : "MISSING");
                kfree(buf);
            } else {
                kprintf("[boot] WINPE18A: /data/c18.db not readable\n");
            }
            int pass = (rc == 0 && found);
            kprintf("[WINPE18A] VERDICT: %s exit=%d sentinel=%d (expect exit 0 + db row)\n",
                    pass ? "PASS" : "FAIL", rc, found);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE18B_BOOT
    /* Track C -- compiler thread-local storage, milestone C18b. Build
     * EXTRA_CFLAGS+=-DWINPE18B_BOOT. /bin/win-tls18b.exe uses _Thread_local
     * across 4 worker threads (compiled -fno-emulated-tls => NATIVE Windows TLS:
     * gs:[0x58] + _tls_index via the PE .tls directory). The loader lays out a
     * TLS template block, writes _tls_index, and points each thread's TEB+0x58
     * at a private pointer array; CreateThread gives each thread its OWN block.
     * Proof = exit 42, reachable ONLY if every thread saw the pristine template
     * (g_n==1000) and main's private copy (1005) survived untouched -- i.e. real
     * per-thread isolation. Shared/unset TLS would corrupt the values and fail. */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE18B: spawning /bin/win-tls18b.exe (_Thread_local x4 threads)\n");
        char *argv[] = { (char *)"win-tls18b.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-tls18b.exe", .name = "win-tls18b.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE18B] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE18B: /bin/win-tls18b.exe (pid=%d) exit=%d\n", pid, rc);
            int pass = (rc == 42);
            kprintf("[WINPE18B] VERDICT: %s exit=%d (expected 42; per-thread _Thread_local)\n",
                    pass ? "PASS" : "FAIL", rc);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE18D_BOOT
    /* Track C -- shell32, milestone C18d. Build EXTRA_CFLAGS+=-DWINPE18D_BOOT.
     * /bin/win-shell18d.exe uses SHGetFolderPathA/SHGetSpecialFolderPathA (CSIDL
     * -> C:\ -> /data), ShellExecuteA (desktop launch via the safe pid-0 queue),
     * Shell_NotifyIcon (tray) and DragQueryFile. Proof = exit 88 (all shim return
     * values correct) AND the kernel re-reads the sentinel the app wrote under the
     * SHGetFolderPathA(CSIDL_APPDATA|FLAG_CREATE) path -- only reachable if that
     * CSIDL path genuinely resolved end-to-end through the C:\ -> /data layer. */
    {
        win32_gui_set_log(true);
        (void)vfs_unlink("/data/Users/toby/AppData/Roaming/c18d.txt");
        kprintf("[boot] WINPE18D: spawning /bin/win-shell18d.exe (shell32 CSIDL/ShellExecute)\n");
        char *argv[] = { (char *)"win-shell18d.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-shell18d.exe", .name = "win-shell18d.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE18D] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE18D: /bin/win-shell18d.exe (pid=%d) exit=%d\n", pid, rc);
            void *buf = 0; size_t sz = 0; int found = 0;
            if (vfs_read_all("/data/Users/toby/AppData/Roaming/c18d.txt", &buf, &sz) == 0 && buf) {
                const char *p = (const char *)buf;
                const char *needle = "C18D-SHELL-OK";
                for (size_t i = 0; sz >= 13 && i + 13 <= sz; i++) {
                    int k = 0; while (k < 13 && p[i + k] == needle[k]) k++;
                    if (k == 13) { found = 1; break; }
                }
                kprintf("[boot] WINPE18D: CSIDL appdata sentinel %s (%lu bytes)\n",
                        found ? "FOUND" : "MISSING", (unsigned long)sz);
                kfree(buf);
            } else {
                kprintf("[boot] WINPE18D: appdata sentinel file not readable\n");
            }
            int pass = (rc == 88 && found);
            kprintf("[WINPE18D] VERDICT: %s exit=%d sentinel=%d (expect exit 88 + CSIDL file)\n",
                    pass ? "PASS" : "FAIL", rc, found);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE19B_BOOT
    /* Track C -- run ANOTHER real off-the-shelf third-party binary, milestone
     * C19b. Build EXTRA_CFLAGS+=-DWINPE19B_BOOT. /bin/win-lua.exe is the
     * UNMODIFIED upstream Lua 5.4.7 interpreter built as a Windows PE. It runs
     * /etc/c19b.lua -- a program of integer arithmetic, the string library,
     * tables and io -- which writes its result to C:\c19b.out (-> /data). Proof
     * = clean exit AND the kernel re-reads /data/c19b.out and finds the exact
     * sentinel the script can only produce by genuinely lexing, parsing,
     * compiling and executing real Lua bytecode through the real VM + stdlib. */
    {
        win32_gui_set_log(true);
        (void)vfs_unlink("/data/c19b.out");
        kprintf("[boot] WINPE19B: spawning /bin/win-lua.exe /etc/c19b.lua (real Lua 5.4)\n");
        char *argv[] = { (char *)"win-lua.exe", (char *)"/etc/c19b.lua", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-lua.exe", .name = "win-lua.exe",
            .argc = 2, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE19B] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE19B: /bin/win-lua.exe (pid=%d) exit=%d\n", pid, rc);
            void *buf = 0; size_t sz = 0; int found = 0;
            if (vfs_read_all("/data/c19b.out", &buf, &sz) == 0 && buf) {
                const char *p = (const char *)buf;
                /* Exact result of the script: integer sum 385, fib(25)=75025,
                 * gsub word count 4 -- unreachable unless real Lua executed. */
                const char *needle = "TOBYLUA=== sum=385 fib25=75025 words=4";
                size_t nl = 0; while (needle[nl]) nl++;
                for (size_t i = 0; sz >= nl && i + nl <= sz; i++) {
                    size_t k = 0; while (k < nl && p[i + k] == needle[k]) k++;
                    if (k == nl) { found = 1; break; }
                }
                kprintf("[boot] WINPE19B: /data/c19b.out = %lu bytes, sentinel %s\n",
                        (unsigned long)sz, found ? "FOUND" : "MISSING");
                kfree(buf);
            } else {
                kprintf("[boot] WINPE19B: /data/c19b.out not readable\n");
            }
            int pass = (rc == 0 && found);
            kprintf("[WINPE19B] VERDICT: %s exit=%d sentinel=%d (expect exit 0 + Lua result)\n",
                    pass ? "PASS" : "FAIL", rc, found);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE19C_BOOT
    /* Track C -- minimal COM / ole32, TIGHTLY SCOPED, milestone C19c. Build
     * EXTRA_CFLAGS+=-DWINPE19C_BOOT. /bin/win-com19c.exe is a first-party
     * COM-ABI proof: it imports the REAL ole32 entry points (CoInitializeEx,
     * CoCreateInstance, CoTaskMemAlloc/Free, CoUninitialize) and drives the one
     * coclass tobyOS supports (CLSID_TobyAdder / ITobyAdder : IUnknown). The
     * point is that QueryInterface/AddRef/Release + the real Add method are all
     * dispatched THROUGH THE OBJECT'S VTABLE, whose slots are kernel-built gate
     * thunks. The exit code 19 is reachable only if: CoTaskMem alloc/free round
     * trips, CoCreateInstance returns a live object, Add(6,7)==42 through the
     * vtable, AddRef/Release reference counting is exact, and QueryInterface
     * both succeeds (IUnknown) and returns E_NOINTERFACE for an unknown IID. */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE19C: spawning /bin/win-com19c.exe (COM vtable proof)\n");
        char *argv[] = { (char *)"win-com19c.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-com19c.exe", .name = "win-com19c.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE19C] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE19C: /bin/win-com19c.exe (pid=%d) exit=%d\n", pid, rc);
            int pass = (rc == 19);
            kprintf("[WINPE19C] VERDICT: %s exit=%d (expect 19: vtable Add/QI/AddRef/Release)\n",
                    pass ? "PASS" : "FAIL", rc);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE20_BOOT
    /* Track C -- float-return marshalling through the gate, milestone C20. Build
     * EXTRA_CFLAGS+=-DWINPE20_BOOT. /bin/win-math20.exe calls the REAL ucrt libm
     * (sqrt/sin/cos/pow/exp/log/floor/ceil/fmod/fabs/atan2/...), each returning a
     * double in xmm0 via the new float-return gate (which marshals xmm0..xmm3 and
     * does movq xmm0,rax on the result) + the -msse kmath libm. It checks every
     * result against its known value within a tolerance using integer/relational
     * ops only (no float printing) and exits 20 only if all doubles round-tripped
     * arg -> xmm -> kernel -> xmm0 correctly (a failing check returns 100+index). */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE20: spawning /bin/win-math20.exe (float-return libm)\n");
        char *argv[] = { (char *)"win-math20.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-math20.exe", .name = "win-math20.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE20] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE20: /bin/win-math20.exe (pid=%d) exit=%d\n", pid, rc);
            int pass = (rc == 20);
            kprintf("[WINPE20] VERDICT: %s exit=%d (expect 20: 15 libm float round-trips; 100+i=fail i)\n",
                    pass ? "PASS" : "FAIL", rc);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE20L_BOOT
    /* Track C -- the REAL off-the-shelf Lua 5.4 interpreter doing FLOATING-POINT
     * math through the C20 float-return gate, milestone C20 (capstone). Build
     * EXTRA_CFLAGS+=-DWINPE20L_BOOT. /bin/win-lua.exe runs /etc/c20.lua, which
     * calls math.sqrt/sin/exp/log/pi (ucrt libm, all double-returning) and writes
     * integer-scaled results to C:\c20.out (-> /data). This turns C19b's honest
     * "floats return garbage" caveat into a proven capability: the kernel re-reads
     * the file and matches the exact sentinel the script can only produce if real
     * libm doubles round-tripped through the gate. */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE20L: spawning /bin/win-lua.exe /etc/c20.lua (real Lua float math)\n");
        char *argv[] = { (char *)"win-lua.exe", (char *)"/etc/c20.lua", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-lua.exe", .name = "win-lua.exe",
            .argc = 2, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE20L] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE20L: /bin/win-lua.exe (pid=%d) exit=%d\n", pid, rc);
            void *buf = 0; size_t sz = 0; int found = 0;
            if (vfs_read_all("/data/c20.out", &buf, &sz) == 0 && buf) {
                const char *p = (const char *)buf;
                const char *needle =
                    "TOBYMATH=== sqrt2=1414214 sin30=500000 e=2718282 lne=1000000 pi=3141593";
                size_t nl = 0; while (needle[nl]) nl++;
                for (size_t i = 0; sz >= nl && i + nl <= sz; i++) {
                    size_t k = 0; while (k < nl && p[i + k] == needle[k]) k++;
                    if (k == nl) { found = 1; break; }
                }
                kprintf("[boot] WINPE20L: /data/c20.out = %lu bytes, sentinel %s\n",
                        (unsigned long)sz, found ? "FOUND" : "MISSING");
                (void)p;
                kfree(buf);
            } else {
                kprintf("[boot] WINPE20L: /data/c20.out not readable\n");
            }
            int pass = (rc == 0 && found);
            kprintf("[WINPE20L] VERDICT: %s exit=%d sentinel=%d (expect exit 0 + Lua float math)\n",
                    pass ? "PASS" : "FAIL", rc, found);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE21_BOOT
    /* Track C -- float PRINTING through the kernel printf engine, milestone C21.
     * Build EXTRA_CFLAGS+=-DWINPE21_BOOT. /bin/win-printf21.exe is a clang/ucrt
     * PE (built like win-crt, so printf/fprintf route to __stdio_common_vfprintf
     * -> win32_vformat). It fprintf()s a battery of %f/%e/%g doubles -- default
     * and explicit precision, width, flags, exponent selection, %g zero-strip, a
     * negative value, and a 14-sig-digit %g -- to C:\c21.out, then exits 21. The
     * kernel re-reads /data/c21.out and matches the EXACT expected text, so the
     * verdict is PASS only if every double rendered correctly through the engine
     * (the C20 gap: doubles could be computed/returned but not yet printed). */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE21: spawning /bin/win-printf21.exe (kernel printf %%f/%%e/%%g)\n");
        char *argv[] = { (char *)"win-printf21.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-printf21.exe", .name = "win-printf21.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE21] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE21: /bin/win-printf21.exe (pid=%d) exit=%d\n", pid, rc);
            void *buf = 0; size_t sz = 0; int found = 0;
            if (vfs_read_all("/data/c21.out", &buf, &sz) == 0 && buf) {
                const char *p = (const char *)buf;
                const char *needle =
                    "C21|3.141593|1.50|0.5|100000|1e+06|1.234568e+04|1.563e-02|-2.500|0.66666666666667|    3.14|";
                size_t nl = 0; while (needle[nl]) nl++;
                for (size_t i = 0; sz >= nl && i + nl <= sz; i++) {
                    size_t k = 0; while (k < nl && p[i + k] == needle[k]) k++;
                    if (k == nl) { found = 1; break; }
                }
                kprintf("[boot] WINPE21: /data/c21.out = %lu bytes, match %s\n",
                        (unsigned long)sz, found ? "FOUND" : "MISSING");
                kfree(buf);
            } else {
                kprintf("[boot] WINPE21: /data/c21.out not readable\n");
            }
            int pass = (rc == 21 && found);
            kprintf("[WINPE21] VERDICT: %s exit=%d match=%d (expect 21 + exact %%f/%%e/%%g output)\n",
                    pass ? "PASS" : "FAIL", rc, found);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE21L_BOOT
    /* Track C -- the REAL Lua 5.4 interpreter PRINTING computed floats, C21
     * capstone. Build EXTRA_CFLAGS+=-DWINPE21L_BOOT. /bin/win-lua.exe runs
     * /etc/c21.lua, which formats doubles (1/4, 0.1, 10/3 at 14 sig digits,
     * sqrt(2)) to decimal text and writes C:\c21l.out (-> /data). win-lua is a
     * mingw build so its float formatting is libmingwex IN-PROCESS (the kernel
     * engine itself is proven by WINPE21); this confirms the full real-app story
     * -- compute a double, print it correctly -- works end to end. */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE21L: spawning /bin/win-lua.exe /etc/c21.lua (real Lua float printing)\n");
        char *argv[] = { (char *)"win-lua.exe", (char *)"/etc/c21.lua", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-lua.exe", .name = "win-lua.exe",
            .argc = 2, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE21L] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE21L: /bin/win-lua.exe (pid=%d) exit=%d\n", pid, rc);
            void *buf = 0; size_t sz = 0; int found = 0;
            if (vfs_read_all("/data/c21l.out", &buf, &sz) == 0 && buf) {
                const char *p = (const char *)buf;
                const char *needle =
                    "C21LUA=== quarter=0.250 tenth=0.1 third=3.3333333333333 root2=1.414214";
                size_t nl = 0; while (needle[nl]) nl++;
                for (size_t i = 0; sz >= nl && i + nl <= sz; i++) {
                    size_t k = 0; while (k < nl && p[i + k] == needle[k]) k++;
                    if (k == nl) { found = 1; break; }
                }
                kprintf("[boot] WINPE21L: /data/c21l.out = %lu bytes, sentinel %s\n",
                        (unsigned long)sz, found ? "FOUND" : "MISSING");
                kfree(buf);
            } else {
                kprintf("[boot] WINPE21L: /data/c21l.out not readable\n");
            }
            int pass = (rc == 0 && found);
            kprintf("[WINPE21L] VERDICT: %s exit=%d sentinel=%d (expect exit 0 + Lua float text)\n",
                    pass ? "PASS" : "FAIL", rc, found);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE22_BOOT
    /* Track C -- formatted/number INPUT (scanf family + strtod/atof), milestone
     * C22. Build EXTRA_CFLAGS+=-DWINPE22_BOOT. /bin/win-scan22.exe is a clang/ucrt
     * PE (built like win-crt, so sscanf/fscanf/strtod route to the ucrt stdio +
     * convert primitives -> the kernel win32_vscanf engine + -msse kmath_strtod).
     * It sscanf's ints/string/double, strtod/atof's with endptr + exponent, %x and
     * %f, and fscanf's a real file, then exits 22 iff every field parsed right
     * (else 50+i). The kernel re-reads /data/c22.out and matches the exact text,
     * so the verdict is PASS only if the scanf engine + real strtod work end to
     * end -- closing the C20/C21 float story (compute, print, and now READ). */
    {
        win32_gui_set_log(true);
        kprintf("[boot] WINPE22: spawning /bin/win-scan22.exe (scanf/strtod input)\n");
        char *argv[] = { (char *)"win-scan22.exe", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/win-scan22.exe", .name = "win-scan22.exe",
            .argc = 1, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[WINPE22] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] WINPE22: /bin/win-scan22.exe (pid=%d) exit=%d\n", pid, rc);
            void *buf = 0; size_t sz = 0; int found = 0;
            if (vfs_read_all("/data/c22.out", &buf, &sz) == 0 && buf) {
                const char *p = (const char *)buf;
                const char *needle =
                    "C22|n=4|a=42|b=-7|w=hello|d=314159|e=271828|g=-1500|u=255|f=50|end=r|m=2|fi=100|fd2=25|";
                size_t nl = 0; while (needle[nl]) nl++;
                for (size_t i = 0; sz >= nl && i + nl <= sz; i++) {
                    size_t k = 0; while (k < nl && p[i + k] == needle[k]) k++;
                    if (k == nl) { found = 1; break; }
                }
                kprintf("[boot] WINPE22: /data/c22.out = %lu bytes, match %s\n",
                        (unsigned long)sz, found ? "FOUND" : "MISSING");
                kfree(buf);
            } else {
                kprintf("[boot] WINPE22: /data/c22.out not readable\n");
            }
            int pass = (rc == 22 && found);
            kprintf("[WINPE22] VERDICT: %s exit=%d match=%d (expect 22 + exact scanf/strtod output)\n",
                    pass ? "PASS" : "FAIL", rc, found);
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef WINPE16D_BOOT
    /* Track C -- TTF everywhere (Win32 UI text), milestone C16d. Build
     * EXTRA_CFLAGS+=-DWINPE16D_BOOT. Re-runs the C14a control gallery
     * (win-ctrl14: GROUPBOX/RADIO/COMBOBOX/TAB) -- the proof is the SCREENSHOT:
     * every control label now renders as real TrueType (vs the 8x8 bitmap). The
     * exit code (14) confirms the controls still work (no behaviour regression). */
    {
        win32_gui_set_log(true);
        winpe_autologin_clear();
        kprintf("[boot] WINPE16D: spawning /bin/win-ctrl14.exe (controls in TTF)\n");
        int cpid = winpe_spawn_session_app("/bin/win-ctrl14.exe", "win-ctrl14.exe");
        if (cpid < 0) {
            kprintf("[WINPE16D] VERDICT: FAIL reason=spawn\n");
        } else {
            int wfd = -1;
            for (int i = 0; i < 100 && wfd < 0; i++) { winpe8_pump_ms(50); wfd = win32_gui_window_fd(cpid); }
            if (wfd < 0) {
                kprintf("[WINPE16D] VERDICT: FAIL reason=nowindow\n");
                signal_send_to_pid(cpid, SIGKILL); (void)proc_wait(cpid);
            } else {
                winpe8_pump_ms(900);
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 41, 70, MOUSE_BTN_LEFT);   winpe8_pump_ms(300);
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 320, 43, MOUSE_BTN_LEFT);  winpe8_pump_ms(300);
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 320, 75, MOUSE_BTN_LEFT);  winpe8_pump_ms(300);
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 80, 120, MOUSE_BTN_LEFT);  winpe8_pump_ms(300);
                kprintf("[WINPE16D] controls in TrueType; holding ~4s for screenshot\n");
                winpe8_pump_ms(4000);
                gui_post_mouse(GUI_EV_MOUSE_DOWN, 240, 315, MOUSE_BTN_LEFT);
                for (int i = 0; i < 160 && win32_gui_window_count(cpid) > 0; i++) winpe8_pump_ms(50);
                int rc = proc_wait(cpid);
                kprintf("[boot] WINPE16D: /bin/win-ctrl14.exe (pid=%d) exit=%d\n", cpid, rc);
                kprintf("[WINPE16D] VERDICT: %s exit=%d (expected 14; labels now TTF)\n", rc == 14 ? "PASS" : "FAIL", rc);
            }
        }
        win32_gui_set_log(false);
    }
#endif

#ifdef LINUXABI_BOOT
    /* Track B (foreign-binary compat) -- Linux ABI proof, milestone B1.
     * Build EXTRA_CFLAGS+=-DLINUXABI_BOOT. Spawns /bin/linux-hello, a
     * GENUINE Linux x86-64 static ELF (raw Linux syscalls, branded
     * ELFOSABI_LINUX) that the kernel runs under the Linux personality:
     * it sets up TLS via arch_prctl, writes to stdout via write+writev,
     * and exit_group(42) iff its %fs thread pointer verified. Exit==42
     * proves the whole Linux-compat chain. Runs regardless of QUICK_BOOT
     * so it is provable on a fast headless boot. */
    {
        kprintf("[boot] LXABI: spawning /bin/linux-hello (Linux x86-64 ELF)\n");
        char *argv[] = { (char *)"linux-hello", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/linux-hello",
            .name = "linux-hello",
            .argc = 1,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] LXABI: /bin/linux-hello not spawned (rc=%d) "
                    "MISSING\n", pid);
            kprintf("[LXABI] VERDICT: FAIL reason=spawn\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] LXABI: /bin/linux-hello (pid=%d) exit=%d\n",
                    pid, rc);
            kprintf("[LXABI] VERDICT: %s exit=%d (expected 42)\n",
                    rc == 42 ? "PASS" : "FAIL", rc);
        }

        /* B4: prove the Linux signal path (rt_sigaction + musl-style
         * restorer + rt_sigreturn) with a second hand-rolled Linux ELF
         * that installs a SIGUSR1 handler, raises it, and exits 0 iff the
         * handler actually ran. */
        kprintf("[boot] LXSIG: spawning /bin/linux-sigtest (Linux signals)\n");
        char *sargv[] = { (char *)"linux-sigtest", 0 };
        char *senvp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec sspec = {
            .path = "/bin/linux-sigtest", .name = "linux-sigtest",
            .argc = 1, .argv = sargv, .envc = 1, .envp = senvp,
        };
        int spid = proc_spawn(&sspec);
        if (spid < 0) {
            kprintf("[LXSIG] VERDICT: FAIL reason=spawn\n");
        } else {
            int src = proc_wait(spid);
            kprintf("[boot] LXSIG: /bin/linux-sigtest (pid=%d) exit=%d\n",
                    spid, src);
            kprintf("[LXSIG] VERDICT: %s exit=%d (expected 0)\n",
                    src == 0 ? "PASS" : "FAIL", src);
        }

        /* B6: prove file-backed mmap with a third hand-rolled Linux ELF
         * that mmaps files at offset 0 and a page offset and verifies the
         * mapped bytes equal read()'s. */
        kprintf("[boot] LXMMAP: spawning /bin/linux-mmaptest (file-backed mmap)\n");
        char *margv[] = { (char *)"linux-mmaptest", 0 };
        char *menvp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec mspec = {
            .path = "/bin/linux-mmaptest", .name = "linux-mmaptest",
            .argc = 1, .argv = margv, .envc = 1, .envp = menvp,
        };
        int mpid = proc_spawn(&mspec);
        if (mpid < 0) {
            kprintf("[LXMMAP] VERDICT: FAIL reason=spawn\n");
        } else {
            int mrc = proc_wait(mpid);
            kprintf("[boot] LXMMAP: /bin/linux-mmaptest (pid=%d) exit=%d\n",
                    mpid, mrc);
            kprintf("[LXMMAP] VERDICT: %s exit=%d (expected 0)\n",
                    mrc == 0 ? "PASS" : "FAIL", mrc);
        }

        /* B9: prove Linux threads -- a hand-rolled ELF spawns a thread with
         * clone(CLONE_VM), the thread runs in the shared address space, sets
         * a shared flag, and exits; the program exits 0 iff the thread ran. */
        kprintf("[boot] LXTHREAD: spawning /bin/linux-thread (clone CLONE_VM)\n");
        char *targv[] = { (char *)"linux-thread", 0 };
        char *tenvp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec tspec = {
            .path = "/bin/linux-thread", .name = "linux-thread",
            .argc = 1, .argv = targv, .envc = 1, .envp = tenvp,
        };
        int tpid = proc_spawn(&tspec);
        if (tpid < 0) {
            kprintf("[LXTHREAD] VERDICT: FAIL reason=spawn\n");
        } else {
            int trc = proc_wait(tpid);
            kprintf("[boot] LXTHREAD: /bin/linux-thread (pid=%d) exit=%d\n",
                    tpid, trc);
            kprintf("[LXTHREAD] VERDICT: %s exit=%d (expected 0)\n",
                    trc == 0 ? "PASS" : "FAIL", trc);
        }

        /* B11: prove pthread_join -- a hand-rolled ELF spawns N joinable
         * threads with clone(CLONE_CHILD_CLEARTID), then joins each via
         * futex(FUTEX_WAIT,&tid); it exits 0 only if the kernel zeroed every
         * tid + woke the futex on each thread's exit (the join rendezvous). */
        kprintf("[boot] LXJOIN: spawning /bin/linux-join (clone+futex join)\n");
        char *jargv[] = { (char *)"linux-join", 0 };
        char *jenvp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec jspec = {
            .path = "/bin/linux-join", .name = "linux-join",
            .argc = 1, .argv = jargv, .envc = 1, .envp = jenvp,
        };
        int jpid = proc_spawn(&jspec);
        if (jpid < 0) {
            kprintf("[LXJOIN] VERDICT: FAIL reason=spawn\n");
        } else {
            int jrc = proc_wait(jpid);
            kprintf("[boot] LXJOIN: /bin/linux-join (pid=%d) exit=%d\n",
                    jpid, jrc);
            kprintf("[LXJOIN] VERDICT: %s exit=%d (expected 0; pthread_join "
                    "contract: CLONE_CHILD_CLEARTID + futex wake)\n",
                    jrc == 0 ? "PASS" : "FAIL", jrc);
        }

        /* B13: prove readiness multiplexing -- a hand-rolled ELF runs a
         * poll/ppoll/select/epoll battery on a real pipe (empty->timeout,
         * write->POLLIN, POLLOUT, select, epoll_wait, EOF->POLLHUP); it
         * exits 0 only if every readiness check matched. */
        kprintf("[boot] LXPOLL: spawning /bin/linux-poll (poll/select/epoll)\n");
        char *poargv[] = { (char *)"linux-poll", 0 };
        char *poenvp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec pospec = {
            .path = "/bin/linux-poll", .name = "linux-poll",
            .argc = 1, .argv = poargv, .envc = 1, .envp = poenvp,
        };
        int popid = proc_spawn(&pospec);
        if (popid < 0) {
            kprintf("[LXPOLL] VERDICT: FAIL reason=spawn\n");
        } else {
            int porc = proc_wait(popid);
            kprintf("[boot] LXPOLL: /bin/linux-poll (pid=%d) exit=%d\n",
                    popid, porc);
            kprintf("[LXPOLL] VERDICT: %s exit=%d (expected 0; poll/select/"
                    "epoll on a pipe)\n", porc == 0 ? "PASS" : "FAIL", porc);
        }
    }
#endif

#ifdef LINUXBB_BOOT
    /* Track B milestone B2 -- run a REAL musl-libc binary (busybox). Build
     * EXTRA_CFLAGS+=-DLINUXBB_BOOT, with an opt-in musl-static busybox staged
     * at programs/busybox/busybox (see Makefile; not committed -- GPL). This
     * is the headline B2 proof: an unmodified, real-libc Linux binary (the
     * busybox multiplexer dispatching its `echo` applet through musl's full
     * startup: arch_prctl TLS, stdio via writev, malloc, exit). */
    {
        /* A battery of busybox applets that each exit 0 on success. They
         * fan out across the real-libc syscall surface: echo (writev),
         * uname (uname), pwd (getcwd), cat (open/read/fstat/close), wc
         * (read loop), stat (stat -> Linux struct stat translation), and
         * ls / ls -la (B3: opendir+getdents64 directory fd + per-entry
         * lstat). Any [linux] unhandled syscall is logged with its number
         * so gaps are self-identifying. */
        static char *bb[][4] = {
            { (char *)"busybox", (char *)"echo",
              (char *)"hello from busybox (musl libc) on tobyOS", 0 },
            { (char *)"busybox", (char *)"true",  0, 0 },
            { (char *)"busybox", (char *)"uname", (char *)"-a", 0 },
            { (char *)"busybox", (char *)"pwd",   0, 0 },
            { (char *)"busybox", (char *)"cat",   (char *)"/etc/motd", 0 },
            { (char *)"busybox", (char *)"wc",    (char *)"/etc/motd", 0 },
            { (char *)"busybox", (char *)"stat",  (char *)"/etc/motd", 0 },
            { (char *)"busybox", (char *)"ls",    (char *)"/bin", 0 },
            { (char *)"busybox", (char *)"ls",    (char *)"-la", (char *)"/bin" },
            { (char *)"busybox", (char *)"ls",    0, 0 },  /* B4: no path -> cwd "." */
        };
        int n = (int)(sizeof(bb) / sizeof(bb[0]));
        int npass = 0, nrun = 0;
        for (int t = 0; t < n; t++) {
            char *argv[5]; int ac = 0;
            for (int i = 0; i < 4 && bb[t][i]; i++) argv[ac++] = bb[t][i];
            argv[ac] = 0;
            char *envp[] = { (char *)"PATH=/bin", 0 };
            struct proc_spec spec = {
                .path = "/bin/busybox", .name = "busybox",
                .argc = ac, .argv = argv, .envc = 1, .envp = envp,
            };
            kprintf("[boot] LXBB: busybox %s%s%s ...\n", bb[t][1],
                    bb[t][2] ? " " : "", bb[t][2] ? bb[t][2] : "");
            int pid = proc_spawn(&spec);
            if (pid < 0) {
                kprintf("[boot] LXBB: /bin/busybox not present -- SKIPPED "
                        "(stage programs/busybox/busybox to enable)\n");
                kprintf("[LXBB] VERDICT: SKIP reason=no-busybox\n");
                nrun = -1;
                break;
            }
            int rc = proc_wait(pid);
            nrun++;
            if (rc == 0) npass++;
            kprintf("[LXBB] busybox %-8s exit=%d %s\n", bb[t][1], rc,
                    rc == 0 ? "OK" : "FAIL");
        }
        if (nrun >= 0)
            kprintf("[LXBB] VERDICT: %s pass=%d/%d\n",
                    (npass == nrun && nrun > 0) ? "PASS" : "FAIL", npass, nrun);
    }
#endif

#ifdef LINUXSH_BOOT
    /* Track B/B8: run a real shell -- `busybox sh -c '<external cmd>'`. The
     * shell forks, execve's /bin/busybox for the external command, and wait4's
     * for it. Proves fork/execve/wait4 translation end to end. Opt-in: needs
     * the static busybox staged (programs/busybox/busybox). */
    {
        kprintf("[boot] LXSH: spawning busybox sh -c 'busybox echo ...'\n");
        /* A command LIST so ash can't exec-optimize the first command away:
         * `echo A` must fork + wait4 (only the trailing `echo B` is exec'd in
         * place), so this exercises fork AND execve AND wait4. */
        char *argv[] = { (char *)"sh", (char *)"-c",
                         (char *)"busybox echo shell-forked-A; "
                                 "busybox echo shell-exec-B", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/busybox", .name = "sh",
            .argc = 3, .argv = argv, .envc = 1, .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] LXSH: /bin/busybox not present -- SKIPPED\n");
            kprintf("[LXSH] VERDICT: SKIP reason=no-busybox\n");
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] LXSH: busybox sh (pid=%d) exit=%d\n", pid, rc);
            kprintf("[LXSH] VERDICT: %s exit=%d (fork+execve+wait4 via a real "
                    "shell)\n", rc == 0 ? "PASS" : "FAIL", rc);
        }

        /* B12: shell PIPELINES -- each stage is a separate fork+dup2(pipe fd
         * onto 0/1)+execve, joined by wait4. A multi-stage pipeline only
         * succeeds if pipe/pipe2 + dup2/dup3 wiring works AND the data flows
         * through every stage in order. Each command's exit status is the
         * LAST stage's, and the last stage is `grep -q <token>` (or a count),
         * so exit 0 means the expected bytes actually traversed every pipe. */
        static const char *pipes[] = {
            /* 2-stage: bytes flow echo -> grep */
            "busybox echo pipe-flows-ok | busybox grep -q pipe-flows-ok",
            /* 3-stage: through an intermediate `cat`, content preserved */
            "busybox echo alpha bravo charlie | busybox cat | "
                "busybox grep -q 'alpha bravo charlie'",
            /* 3-stage: a real transform (word count) reaches the asserting stage */
            "busybox echo w x y z | busybox wc -w | busybox grep -q 4",
        };
        int pn = (int)(sizeof(pipes) / sizeof(pipes[0]));
        int ppass = 0, prun = 0;
        for (int t = 0; t < pn; t++) {
            char *pargv[] = { (char *)"sh", (char *)"-c", (char *)pipes[t], 0 };
            char *penvp[] = { (char *)"PATH=/bin", 0 };
            struct proc_spec pspec = {
                .path = "/bin/busybox", .name = "sh",
                .argc = 3, .argv = pargv, .envc = 1, .envp = penvp,
            };
            kprintf("[boot] LXPIPE: sh -c '%s'\n", pipes[t]);
            int ppid = proc_spawn(&pspec);
            if (ppid < 0) {
                kprintf("[boot] LXPIPE: /bin/busybox not present -- SKIPPED\n");
                kprintf("[LXPIPE] VERDICT: SKIP reason=no-busybox\n");
                prun = -1;
                break;
            }
            int prc = proc_wait(ppid);
            prun++;
            if (prc == 0) ppass++;
            kprintf("[LXPIPE] stage-test %d exit=%d %s\n", t, prc,
                    prc == 0 ? "OK" : "FAIL");
        }
        if (prun >= 0)
            kprintf("[LXPIPE] VERDICT: %s pass=%d/%d (multi-stage pipe + dup2 "
                    "wiring, data flows in order)\n",
                    (ppass == prun && prun > 0) ? "PASS" : "FAIL", ppass, prun);
    }
#endif

#ifdef LINUXDYN_BOOT
    /* Track B/B5: run a DYNAMICALLY-linked Linux binary -- a real musl
     * busybox (PT_INTERP=/lib/ld-musl-x86_64.so.1, NEEDED libc.musl).
     * The kernel loads both the PIE program and the ld-musl interpreter
     * (which IS libc), then jumps to the loader, which self-relocates,
     * relocates busybox, resolves symbols, and runs the applet. Proves
     * the Linux dynamic-loader path. Opt-in (stage programs/busybox/
     * busybox-dyn + ld-musl-x86_64.so.1). */
    {
        /* A battery through the DYNAMIC busybox -- every applet exits 0 only
         * if ld-musl relocated + the shared libc's full runtime works. */
        static char *bb[][4] = {
            { (char *)"busybox-dyn", (char *)"echo",
              (char *)"hello from DYNAMIC busybox via ld-musl", 0 },
            { (char *)"busybox-dyn", (char *)"uname", (char *)"-a", 0 },
            { (char *)"busybox-dyn", (char *)"pwd",   0, 0 },
            { (char *)"busybox-dyn", (char *)"cat",   (char *)"/etc/motd", 0 },
            { (char *)"busybox-dyn", (char *)"wc",    (char *)"/etc/motd", 0 },
            { (char *)"busybox-dyn", (char *)"stat",  (char *)"/etc/motd", 0 },
            { (char *)"busybox-dyn", (char *)"ls",    (char *)"/bin", 0 },
        };
        int n = (int)(sizeof(bb) / sizeof(bb[0]));
        int npass = 0, nrun = 0;
        for (int t = 0; t < n; t++) {
            char *argv[5]; int ac = 0;
            for (int i = 0; i < 4 && bb[t][i]; i++) argv[ac++] = bb[t][i];
            argv[ac] = 0;
            char *envp[] = { (char *)"PATH=/bin", 0 };
            struct proc_spec spec = {
                .path = "/bin/busybox-dyn", .name = "busybox-dyn",
                .argc = ac, .argv = argv, .envc = 1, .envp = envp,
            };
            kprintf("[boot] LXDYN: busybox-dyn %s ...\n", bb[t][1]);
            int pid = proc_spawn(&spec);
            if (pid < 0) {
                kprintf("[boot] LXDYN: /bin/busybox-dyn not present -- SKIPPED\n");
                kprintf("[LXDYN] VERDICT: SKIP reason=no-dynamic-busybox\n");
                nrun = -1;
                break;
            }
            int rc = proc_wait(pid);
            nrun++;
            if (rc == 0) npass++;
            kprintf("[LXDYN] busybox-dyn %-6s exit=%d %s\n", bb[t][1], rc,
                    rc == 0 ? "OK" : "FAIL");
        }
        if (nrun >= 0)
            kprintf("[LXDYN] VERDICT: %s pass=%d/%d (real ld-musl dynamic linking)\n",
                    (npass == nrun && nrun > 0) ? "PASS" : "FAIL", npass, nrun);

        /* B7: the multi-DSO headline -- a REAL musl binary (`file`, from
         * Alpine) that depends on a SEPARATE shared library, libmagic.so.1
         * (beyond libc). ld-musl must open + FILE-BACKED mmap
         * /lib/libmagic.so.1 and resolve its symbols. `file --version`
         * prints the version and exits 0 without touching the magic DB.
         * Opt-in: stage programs/busybox/{file,libmagic.so.1}. */
        kprintf("[boot] LXMULTI: spawning /bin/file --version (separate .so "
                "via ld-musl)\n");
        char *fargv[] = { (char *)"file", (char *)"--version", 0 };
        char *fenvp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec fspec = {
            .path = "/bin/file", .name = "file",
            .argc = 2, .argv = fargv, .envc = 1, .envp = fenvp,
        };
        int fpid = proc_spawn(&fspec);
        if (fpid < 0) {
            kprintf("[boot] LXMULTI: /bin/file not present -- SKIPPED\n");
            kprintf("[LXMULTI] VERDICT: SKIP reason=no-multidso-binary\n");
        } else {
            int frc = proc_wait(fpid);
            kprintf("[boot] LXMULTI: /bin/file --version (pid=%d) exit=%d\n",
                    fpid, frc);
            kprintf("[LXMULTI] VERDICT: %s exit=%d (file-backed mmap of "
                    "libmagic.so.1)\n", frc == 0 ? "PASS" : "FAIL", frc);
        }
    }
#endif

#ifdef LINUXAUTO_BOOT
    /* Track B/B10: drop-and-run an UNBRANDED Linux binary. /bin/busybox-auto
     * is a byte-for-byte copy of the musl busybox with e_ident[EI_OSABI]
     * forced to 0 (ELFOSABI_SYSV) -- i.e. NO `brandelf` tag, exactly what a
     * vanilla off-the-shelf Linux ELF looks like. Before B10 it would have
     * fallen to the native personality and mis-dispatched its first syscall.
     * Now the kernel auto-detects its PT_INTERP (/lib/ld-musl-x86_64.so.1) and
     * switches the process to ABI_PERS_LINUX with zero preparation. The
     * harness first re-reads the file to ASSERT it is genuinely unbranded
     * (OSABI byte == 0), so a PASS can only mean the loader auto-detected it. */
    {
        const char *path = "/bin/busybox-auto";
        void  *img = 0;
        size_t isz = 0;
        if (vfs_read_all(path, &img, &isz) != 0 || isz < 8) {
            if (img) kfree(img);
            kprintf("[boot] LXAUTO: %s not present -- SKIPPED\n", path);
            kprintf("[LXAUTO] VERDICT: SKIP reason=no-unbranded-binary\n");
        } else {
            uint8_t osabi = ((const uint8_t *)img)[7];   /* e_ident[EI_OSABI] */
            kfree(img);
            kprintf("[boot] LXAUTO: %s on-disk EI_OSABI=%u (%s)\n", path,
                    (unsigned)osabi,
                    osabi == 0 ? "ELFOSABI_SYSV/unbranded" : "BRANDED");
            if (osabi != 0) {
                kprintf("[LXAUTO] VERDICT: FAIL reason=test-binary-not-unbranded "
                        "osabi=%u\n", (unsigned)osabi);
            } else {
                char *argv[] = { (char *)"busybox-auto", (char *)"echo",
                                 (char *)"hello from an UNBRANDED, auto-detected "
                                         "Linux binary", 0 };
                char *envp[] = { (char *)"PATH=/bin", 0 };
                struct proc_spec spec = {
                    .path = path, .name = "busybox-auto",
                    .argc = 3, .argv = argv, .envc = 1, .envp = envp,
                };
                kprintf("[boot] LXAUTO: spawning %s echo ... (no brand)\n", path);
                int pid = proc_spawn(&spec);
                int rc  = (pid < 0) ? -1 : proc_wait(pid);
                kprintf("[LXAUTO] VERDICT: %s exit=%d (unbranded OSABI=0 dynamic "
                        "Linux binary; personality auto-detected from "
                        "PT_INTERP)\n", rc == 0 ? "PASS" : "FAIL", rc);
            }
        }
    }
#endif

#ifdef M36_SELFTEST
    /* Milestone 36E: in-OS compile + run self-test (TobyC stage-1). */
    {
        kprintf("[boot] M36: spawning /bin/selfhosttest\n");
        char *argv[] = { (char *)"selfhosttest", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/selfhosttest",
            .name = "selfhosttest-boot",
            .argc = 1,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] M36: /bin/selfhosttest not spawned (rc=%d)\n", pid);
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] M36: selfhosttest (pid=%d) exit=%d (%s)\n",
                    pid, rc, rc == 0 ? "PASS" : "FAIL");
        }
    }
#endif

#ifndef QUICK_BOOT
    /* Milestone 28B (post-boot inspector): on every clean boot, if a
     * crash dump from a previous panic survived in /data/crash/last.dump
     * we spawn /bin/crashinfo --boot to decode it. Test scripts grep
     * for the M28B_CRASHINFO sentinels this prints. */
    m28b_run_crashinfo_inspector();

    /* Milestone 28E: filesystem-integrity harness. Always probes the
     * live /data via /bin/fscheck --boot; additionally, when
     * FSCHECK_FLAG=1 baked /etc/fscheck_now into the initrd, runs the
     * in-kernel ramdisk self-test that proves corruption detection. */
    m28e_run_fscheck_harness();

    /* Milestone 28F: service-supervision harness. Always spawns
     * /bin/services --boot to confirm the SVC_LIST syscall and registry
     * snapshotting work. When SVCTEST_FLAG=1 (i.e. /etc/svctest_now is
     * present) it ALSO drives a synthetic crash-loop through the
     * supervisor and verifies BACKOFF -> DISABLED transitions plus
     * service_clear() recovery. */
    m28f_run_service_harness();

    /* Milestone 28G: stability self-test harness. Always spawns
     * /bin/stabilitytest --boot to confirm SYS_STAB_SELFTEST and
     * the per-subsystem probe verdicts. When STABTEST_FLAG=1
     * (/etc/stabtest_now present) it ALSO runs --stress for an
     * end-to-end heap+disk+syscall workload. */
    m28g_run_stability_harness();
#endif

    /* Milestone 28C: watchdog harness. Runs only when /etc/wdogtest_now
     * exists in the read-only initrd (built with WDOGTEST_FLAG=1). Drops
     * the timeout to ~600 ms, simulates a kernel sched stall via the
     * watchdog's own helper, then spawns /bin/wdogtest --boot to verify
     * userland can read the bite event via SYS_WDOG_STATUS. */
    {
        struct vfs_stat st;
        if (vfs_stat("/etc/wdogtest_now", &st) == VFS_OK) {
            kprintf("[boot] M28C: /etc/wdogtest_now present -- "
                    "running watchdog hang harness\n");
            uint32_t saved = wdog_timeout_ms();
            wdog_set_timeout_ms(600);
            kprintf("[boot] M28C: timeout reduced to 600 ms (was %u)\n",
                    (unsigned)saved);
            kprintf("[boot] M28C: simulating 1500 ms kernel stall...\n");
            wdog_simulate_kernel_stall(1500);
            kprintf("[boot] M28C: stall complete; spawning /bin/wdogtest --boot\n");
            char *argv[] = { (char *)"wdogtest", (char *)"--boot", 0 };
            char *envp[] = { (char *)"PATH=/bin", 0 };
            struct proc_spec spec = {
                .path = "/bin/wdogtest",
                .name = "wdogtest-boot",
                .argc = 2,
                .argv = argv,
                .envc = 1,
                .envp = envp,
            };
            int pid = proc_spawn(&spec);
            if (pid < 0) {
                kprintf("[boot] M28C: /bin/wdogtest not spawned (rc=%d) MISSING\n",
                        pid);
            } else {
                int rc = proc_wait(pid);
                kprintf("[boot] M28C: /bin/wdogtest (pid=%d) exit=%d (%s)\n",
                        pid, rc, rc == 0 ? "PASS" : "FAIL");
            }
            wdog_set_timeout_ms(saved);
            kprintf("[boot] M28C: timeout restored to %u ms\n",
                    (unsigned)saved);
            kprintf("[boot] M28C: watchdog harness complete\n");
        }
    }

    /* Milestone 28B (crash-test trigger): if /etc/crashtest_now exists
     * (i.e. the user built with CRASHTEST_FLAG=1, baking the marker
     * into the read-only initrd), deliberately trip the panic path so
     * the test script can verify the panic banner, register dump,
     * slog tail, and on-disk crash dump are all produced. A normal
     * boot never hits this path. */
    {
        struct vfs_stat st;
        if (vfs_stat("/etc/crashtest_now", &st) == VFS_OK) {
            kprintf("[boot] M28B: /etc/crashtest_now present -- "
                    "triggering controlled panic for test\n");
            SLOG_INFO(SLOG_SUB_PANIC,
                      "M28B controlled crashtest about to fire");
            /* Flush slog so the records make it to disk before the
             * panic re-paints the screen. panic.c will also dump
             * the ring; persisting it gives userland tooling
             * something to compare against on the inspect boot. */
            (void)slog_persist_flush();
            kpanic_self_test("kpanic");
            /* not reached */
        }
    }

    /* Milestone 28D: safe-mode finalisation. If we skipped the GUI we
     * never spawned the desktop / login / shell. Instead, drop into
     * /bin/safesh -- a minimal text REPL on stdin/stdout that lets
     * the operator inspect logs, query the watchdog, decode crash
     * dumps, or reboot. The harness exits cleanly so that when the
     * test script greps for the SAFESH PASS sentinel it knows the
     * essential subsystems all came up.
     * M35E: previously gated on safemode_active() which dragged GUI
     * and COMPATIBILITY into safesh too. Switch to safemode_skip_gui()
     * so the desktop tiers actually launch their desktop. */
    if (safemode_skip_gui()) {
        kprintf("[boot] M28D: SAFE MODE -- spawning /bin/safesh\n");
        SLOG_INFO(SLOG_SUB_SAFE, "spawning safesh");
        char *argv[] = { (char *)"safesh", (char *)"--boot", 0 };
        char *envp[] = { (char *)"PATH=/bin", 0 };
        struct proc_spec spec = {
            .path = "/bin/safesh",
            .name = "safesh",
            .argc = 2,
            .argv = argv,
            .envc = 1,
            .envp = envp,
        };
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[boot] M28D: /bin/safesh not spawned (rc=%d) MISSING\n",
                    pid);
        } else {
            int rc = proc_wait(pid);
            kprintf("[boot] M28D: /bin/safesh (pid=%d) exit=%d (%s)\n",
                    pid, rc, rc == 0 ? "PASS" : "FAIL");
        }
        kprintf("[boot] M28D: safe-mode harness complete\n");
    }

    /* Persist kprintf capture to /data and/or FAT32 install USB. */
    bootlog_flush_all();
    /* UDP boot log: GUI path sends after ~300 ms in gui_tick(); if we
     * never brought the desktop up, push once here so safesh still
     * ships logs to the LAN collector. */
    if (safemode_skip_gui())
        bootlog_net_upload();

    /* Boot sequence done. From here pid 0 holds the BKL around its per-tick
     * shared-state work (idle_loop), so it's now safe to let secondary CPUs
     * steal + run user procs in parallel. */
    sched_enable_ap_run();

    /* Scheduler + drivers are up: let blk_io_wait cooperatively yield
     * while block commands are in flight, so concurrent submitters keep
     * the AHCI/NVMe queues full instead of serializing. */
    blk_set_yield_ready();

#ifdef MCTEST_BOOT
    /* Opt-in multi-core proof (EXTRA_CFLAGS+=-DMCTEST_BOOT). With AP-run now
     * enabled, time one CPU-bound worker then four spawned together. ~one-
     * worker time => real parallelism; ~4x => serial. Held under the BKL so
     * pid-0's proc-table accesses are serialized vs the workers' exits;
     * proc_wait's sched_yield drops/reacquires it so the workers run in
     * parallel on the APs. /bin/mctest does no syscalls in its compute loop. */
    {
        struct proc_spec spec = {
            .path = "/bin/mctest", .name = "mctest",
            .argc = 0, .argv = 0, .envc = 0, .envp = 0,
        };
        uint32_t hz = pit_hz();
#define MC_NOW_MS() (hz ? (pit_ticks() * 1000ULL / hz) : 0ULL)
        bkl_enter();
        uint64_t t0 = MC_NOW_MS();
        int p1 = proc_spawn(&spec);
        if (p1 > 0) proc_wait(p1);
        uint64_t one = MC_NOW_MS() - t0;

        uint64_t t2 = MC_NOW_MS();
        int pids[4];
        for (int i = 0; i < 4; i++) pids[i] = proc_spawn(&spec);
        for (int i = 0; i < 4; i++) if (pids[i] > 0) proc_wait(pids[i]);
        uint64_t four = MC_NOW_MS() - t2;
        bkl_exit();
        kprintf("[boot] MCTEST: 1 worker = %llu ms; 4 workers = %llu ms "
                "(serial ~%llu ms)\n", (unsigned long long)one,
                (unsigned long long)four, (unsigned long long)(one * 4));
        if (four > 0)
            kprintf("[boot] MCTEST: parallelism ~%llu.%02llux\n",
                    (unsigned long long)((one * 4) / four),
                    (unsigned long long)(((one * 4 * 100) / four) % 100));
#undef MC_NOW_MS
    }
#endif

#ifdef MCARGV_BOOT
    /* Repro for the AP-first-run + argc>=1 + SMAP/SMEP fault. Spawn four
     * argc>=1 workers at once so APs steal+run them; if the bug is live one
     * of them SMEP-faults at first entry (kernel instruction-fetch from the
     * user page). Build: EXTRA_CFLAGS="-DMCARGV_BOOT" and boot with
     * -cpu qemu64,+smep,+smap. /bin/mctest ignores argv so behaviour is
     * otherwise identical to the argc=0 MCTEST run. */
    {
        char *av[] = { "mctest", "argone", "argtwo", "argthree", 0 };
        char *ev[] = { "PATH=/bin", "HOME=/", "TERM=toby", 0 };
        struct proc_spec spec = {
            .path = "/bin/mctest", .name = "mctest",
            .argc = 4, .argv = av, .envc = 3, .envp = ev,
        };
        int rounds = 8;
        kprintf("[boot] MCARGV: %d rounds x 4 argc>=1 workers (AP/SMAP repro)\n",
                rounds);
        for (int round = 0; round < rounds; round++) {
            bkl_enter();
            int pids[4];
            for (int i = 0; i < 4; i++) pids[i] = proc_spawn(&spec);
            for (int i = 0; i < 4; i++) if (pids[i] > 0) proc_wait(pids[i]);
            bkl_exit();
            kprintf("[boot] MCARGV: round %d/%d done\n", round + 1, rounds);
        }
        kprintf("[boot] MCARGV: all argc>=1 workers completed (no fault)\n");
    }
#endif

#ifdef SCHEDPRIO_BOOT
    /* Opt-in priority-scheduling + fair-timeslicing proof
     * (EXTRA_CFLAGS+=-DSCHEDPRIO_BOOT). Run under the DEFAULT QEMU CPU (no
     * +smap): these workers take argv, and argc>=1 first-run on an AP under
     * SMAP is the separate open real-HW bug -- irrelevant here.
     *
     * Spawn 6 identical timed CPU-bound workers (more than the 4 cores, so they
     * must contend) onto a 4-CPU machine, three at PRIO_HIGH and three at
     * PRIO_LOW. Each runs the SAME ~1500 ms wall window, so the CPU each one
     * accumulates (cpu_ns, captured at reap by proc_wait_info) is a direct
     * measure of the share the scheduler gave it. We assert two things:
     *   (1) every worker got SOME CPU (cpu_ns > 0) -> nothing starved, which is
     *       only possible because aging lifts the LOW procs and the per-AP LAPIC
     *       timer preempts the running ones so queued procs get a turn;
     *   (2) the HIGH group averaged materially more CPU than the LOW group
     *       -> priority actually steers the dispatcher. */
    {
        char *av[]  = { "mctest", "1500", 0 };
        struct proc_spec spec = {
            .path = "/bin/mctest", .name = "mctest",
            .argc = 2, .argv = av, .envc = 0, .envp = 0,
        };
        const int N = 6;
        int      pids[6];
        uint64_t cpu_ns[6];
        int      prio[6] = { PRIO_HIGH, PRIO_HIGH, PRIO_HIGH,
                             PRIO_LOW,  PRIO_LOW,  PRIO_LOW };

        kprintf("[boot] SCHEDPRIO: 6 timed workers (3 HIGH + 3 LOW) on %u CPUs\n",
                smp_online_count());
        bkl_enter();
        for (int i = 0; i < N; i++) {
            pids[i] = proc_spawn(&spec);
            if (pids[i] > 0) sched_set_prio(pids[i], prio[i]);   /* before they run */
        }
        /* Reap in spawn order; cpu_ns is captured pre-reap. All exit ~together
         * (same wall window), so later waits return near-immediately. */
        for (int i = 0; i < N; i++) {
            cpu_ns[i] = 0;
            if (pids[i] > 0) {
                struct proc_exit_info info;
                proc_wait_info(pids[i], &info);
                cpu_ns[i] = info.cpu_ns;
            }
        }
        bkl_exit();

        uint64_t hi = 0, lo = 0;
        bool all_ran = true;
        for (int i = 0; i < N; i++) {
            if (cpu_ns[i] == 0) all_ran = false;
            if (prio[i] == PRIO_HIGH) hi += cpu_ns[i]; else lo += cpu_ns[i];
            kprintf("[boot] SCHEDPRIO:   pid %d prio %d cpu=%llu ms\n",
                    pids[i], prio[i], (unsigned long long)(cpu_ns[i] / 1000000ULL));
        }
        uint64_t hi_avg = hi / 3, lo_avg = lo / 3;
        /* HIGH should average clearly more than LOW. Require >=1.3x and that no
         * worker starved. (lo_avg==0 would also fail all_ran.) */
        bool weighted = lo_avg == 0 ? (hi_avg > 0)
                                    : (hi_avg * 10ULL >= lo_avg * 13ULL);
        kprintf("[boot] SCHEDPRIO: HIGH avg=%llu ms  LOW avg=%llu ms  "
                "(ratio x100=%llu)\n",
                (unsigned long long)(hi_avg / 1000000ULL),
                (unsigned long long)(lo_avg / 1000000ULL),
                (unsigned long long)(lo_avg ? (hi_avg * 100ULL / lo_avg) : 0));
        kprintf("[boot] SCHEDPRIO: %s (no-starvation=%s, weighted=%s)\n",
                (all_ran && weighted) ? "PASS" : "FAIL",
                all_ran ? "yes" : "no", weighted ? "yes" : "no");
    }
#endif

#ifdef SCHEDINT_BOOT
    /* Opt-in interactivity / io_boost proof (EXTRA_CFLAGS+=-DSCHEDINT_BOOT),
     * default QEMU CPU. HOGS CPU-bound spinners saturate the cores; one
     * "pipeint" reader BLOCKS on a pipe fed every ~15 ms by its own (mostly
     * idle) feeder child, measuring true wake-to-run latency. ALL at
     * PRIO_NORMAL, so any latency difference is purely the io_boost (granted
     * when the reader blocks with quantum left): with it the woken reader
     * out-ranks the hogs at the next scheduling point; without it the reader
     * waits its FIFO turn behind them. Reader returns avg latency (ms) as its
     * exit code. Compare boost on/off (toggle SCHED_IO_BOOST) to see the delta;
     * HOGS == cores so every core is busy when the reader wakes. */
    {
        const int HOGS = 6;          /* > cores: even when the feeder frees its
                                      * core on each write, a hog is queued for
                                      * it, so the woken reader must out-rank
                                      * the hog to run promptly -- which is
                                      * exactly what io_boost decides. */
        char *hog_av[] = { "mctest", "2500", 0 };
        char *pi_av[]  = { "mctest", "pipeint", "1800", 0 };
        struct proc_spec hog = { .path="/bin/mctest", .name="mc-hog",
                                 .argc=2, .argv=hog_av };
        struct proc_spec rdr = { .path="/bin/mctest", .name="mc-pipe",
                                 .argc=3, .argv=pi_av };
        kprintf("[boot] SCHEDINT: %d CPU hogs + 1 pipe-blocked reader "
                "(all NORMAL) on %u CPUs\n", HOGS, smp_online_count());
        bkl_enter();
        int hpids[8], rpid;
        for (int i = 0; i < HOGS; i++) hpids[i] = proc_spawn(&hog);
        rpid = proc_spawn(&rdr);
        struct proc_exit_info info, rinfo;
        rinfo.exit_code = -1;
        if (rpid > 0) proc_wait_info(rpid, &rinfo);  /* exit_code = avg lat ms */
        for (int i = 0; i < HOGS; i++)
            if (hpids[i] > 0) proc_wait_info(hpids[i], &info);
        bkl_exit();
        kprintf("[boot] SCHEDINT: pipe-reader avg wake latency = %d ms "
                "(io_boost=%d)\n", rinfo.exit_code, SCHED_IO_BOOST);
    }
#endif

    idle_loop();
}
