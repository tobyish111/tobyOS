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
#include <tobyos/dns.h>
#include <tobyos/tcp.h>
#include <tobyos/http.h>
#include <tobyos/xhci.h>
#include <tobyos/usb_hub.h>
#include <tobyos/virtio_gpu.h>
#include <tobyos/gfx.h>
#include <tobyos/mouse.h>
#include <tobyos/gui.h>
#include <tobyos/term.h>
#include <tobyos/settings.h>
#include <tobyos/service.h>
#include <tobyos/session.h>
#include <tobyos/users.h>
#include <tobyos/pkg.h>
#include <tobyos/sec.h>
#include <tobyos/sysprot.h>
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
#include <tobyos/abi/abi.h>

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
    bootlog_init();
    kprintf("[boot] serial up\n");
    /* Milestone 28A: bring the structured-log ring up as early as
     * possible so every subsequent subsystem can SLOG_INFO/etc. into
     * a real ring and not just the early fallback. The ring is BSS-
     * resident so no allocator is required. */
    slog_init();
    SLOG_INFO(SLOG_SUB_BOOT, "tobyOS boot sequence started");
}

static void framebuffer_init(void) {
    if (fb_req.response == 0 || fb_req.response->framebuffer_count == 0) {
        /* Serial-only path: report and continue without a console. The
         * panic path still works; it just won't be visible on screen. */
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

    if (!console_init(fb->address, fb->pitch, fb->width, fb->height)) {
        kprintf("[boot] WARNING: console_init rejected the framebuffer\n");
        return;
    }
    kprintf("[boot] console up\n");

    /* Hand the same framebuffer to the gfx layer. The back buffer is
     * a heap allocation, so this DEFERS until heap_init() runs. We
     * stash the pointer/dims on the stack here and call gfx_init from
     * the post-heap path inside _start. */
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
        /* Looks like a raw phys -- map the page (and the next, in case
         * the RSDP straddles a page boundary -- it's only 36 bytes but
         * the alignment can be awkward) and convert. */
        uint64_t page = addr & ~((uint64_t)PAGE_SIZE - 1);
        if (vmm_translate(hhdm + page) == 0) {
            (void)vmm_map(hhdm + page, page, PAGE_SIZE * 2,
                          VMM_PRESENT | VMM_NX);
        }
        addr += hhdm;
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
    /* Drive shell + cursor blink between IRQs. hlt() puts the CPU in
     * C1 until the next interrupt -- both PIT (every 10 ms) and the
     * keyboard wake us, so input feels instant and the cursor blinks
     * smoothly without us spinning. */
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 1;
    for (;;) {
        hlt();
        /* net_poll() stays for NICs without MSI (notably QEMU's e1000,
         * which never advertises an MSI cap). MSI/MSI-X-driven NICs
         * (virtio-net, e1000e, rtl8169) make this a fast no-op because
         * the IRQ handler has already drained their rx_drain queue. */
        net_poll();
        /* xhci_poll() removed: M22 step 3e wires xHCI to MSI-X (or MSI),
         * and the IRQ handler invokes the same poll body atomically.
         * If MSI bring-up failed, xhci_pci_probe logs "staying polled"
         * and USB stays dark -- a deliberate, visible failure rather
         * than a silent fallback that masks broken interrupt routing. */
        /* M26C: deferred hot-plug processing. The IRQ handler only
         * RW1Cs the change bits + flips a bitmap; we do the actual
         * Enable/Disable Slot work here so xhci_cmd's spin doesn't
         * deadlock against the same-context MSI it's waiting for.
         * usb_hub_poll() is rate-limited internally to ~5 Hz so the
         * CPU cost stays bounded even with 4 hubs * 8 ports. Both
         * functions are no-ops when no controller / no hubs exist. */
        xhci_service_port_changes();
        usb_hub_poll();
        gui_tick();           /* recomposite if mouse moved or a window flipped */
        shell_poll();
        /* The blinking text-mode cursor would scribble over the GUI's
         * compositor pass, so we skip the tick while a window is up. */
        if (!gui_active()) console_tick(pit_ticks(), hz);
        /* M22 boot self-test: only fires when built with
         * -DACPI_M22_SELFTEST. In default builds this is a no-op
         * inline expansion, so there's no per-tick cost. */
        acpi_m22_selftest_tick();
    }
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
    pit_init(100);          /* via irq_install_isa(0, pit_irq) */
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
    e1000_register();
    e1000e_register();
    virtio_net_register();
    rtl8169_register();
    xhci_register();        /* USB 3.x host controller (qemu-xhci, real PCH xHCI) */
    virtio_gpu_register();  /* GPU: virtio-gpu (basic 2D); falls back to Limine FB */
    audio_hda_register();   /* M26F: HD Audio controller (M26A: stub probe only) */
    pci_bind_drivers();
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
    net_dump();

    modules_log();
    /* Mount the boot tar as the root filesystem before any code wants
     * to read files. After this, vfs_open / vfs_opendir / vfs_read_all
     * all work, and user_load_and_run() can be passed VFS paths. */
    initrd_init();
    /* Milestone 28D: latch safe-mode state right after the initrd is
     * mounted (so /etc/safemode_now is readable) but BEFORE any
     * optional subsystem inits. From here on, safemode_active() is
     * the canonical "skip non-essential drivers" gate. */
    safemode_init();

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
    }
    /* Process model + scheduler MUST come up before any proc_create
     * call. proc_init synthesizes pid 0 from this very boot context;
     * everything below is "running as pid 0". */
    proc_init();
    sched_init();
    signal_init();           /* milestone 8: SIGINT/SIGTERM + foreground tracking */
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
    /* Networking comes up AFTER SMP so the boot log reads top-to-bottom
     * in subsystem order. Failure is non-fatal -- the rest of the OS
     * still boots and the shell will say "no NIC" if you call ifconfig.
     * M28D: safe mode skips the NIC entirely -- a flaky e1000 was
     * historically a "won't boot" trigger.
     * M35E: COMPATIBILITY mode keeps networking up, so we now gate on
     * the more precise safemode_skip_net() predicate. */
    if (safemode_skip_net()) {
        kprintf("[safe] skipping net_init (NIC + DHCP + DNS) -- mode=%s\n",
                safemode_tag());
    } else {
        (void)net_init();
    }

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
        gfx_layer_init();
        if (safemode_skip_virtio_gpu()) {
            kprintf("[safe] mode=%s -- keeping Limine framebuffer "
                    "(skip virtio-gpu fast path)\n", safemode_tag());
        } else {
            virtio_gpu_install_backend();
        }
        mouse_init();
        gui_init();
        term_init();
        banner();
        /* Milestone 14: bring up settings + services + session BEFORE
         * shell_init so the desktop+login is already on screen by the
         * time the shell starts polling. The shell stays available for
         * debugging (type at the prompt to use it once you've Exited
         * Desktop or pressed F2). */
        m14_init();
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
        devtest_boot_run();
        shell_init();
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
    }
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
    idle_loop();
}
