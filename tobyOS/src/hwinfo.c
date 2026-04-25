/* hwinfo.c -- Milestone 29A: hardware discovery & inventory.
 *
 * The implementation has three layers:
 *
 *   1. boot-time CPUID probe ............... hwinfo_init()
 *   2. cheap on-demand snapshot collector .. hwinfo_snapshot()
 *   3. textual rendering + persistence ..... hwinfo_format_text(),
 *                                            hwinfo_persist()
 *
 * Layer (1) runs once from kernel.c after slog/pmm but before
 * pci_init -- the CPU side is independent of the bus side, so we
 * grab it as early as possible. Layer (2) is what SYS_HWINFO calls;
 * it reads the cached CPU info and re-pulls the bus counts via
 * devtest_enumerate so they stay fresh across hot-plug. Layer (3)
 * is text-only on top of (2).
 *
 * The cached snapshot lives in BSS. No allocations anywhere on the
 * hot path -- callable from boot context BEFORE the heap is up
 * (used by the M29A boot harness).
 */

#include <tobyos/hwinfo.h>
#include <tobyos/devtest.h>
#include <tobyos/pmm.h>
#include <tobyos/pci.h>
#include <tobyos/safemode.h>
#include <tobyos/smp.h>
#include <tobyos/slog.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/vfs.h>
#include <tobyos/pit.h>
#include <tobyos/abi/abi.h>

/* ---------- internal state ---------- */

static struct abi_hwinfo_summary g_snap;
static bool                       g_init_done = false;
static uint64_t                   g_epoch = 0;
static char                       g_profile[ABI_HW_PROFILE_MAX] = "vm";

/* On-disk persistent snapshot. Written once at boot (after pci/usb/
 * blk are up) and refreshed by hwinfo_persist() when explicitly
 * called. The path is fixed so hwreport / bringuptest / external
 * automation can find it deterministically. */
#define HWINFO_SNAP_PATH   "/data/hwinfo.snap"

/* Maximum size of the rendered text dump. 4 KiB is plenty for the
 * worst case: ~64 devices * ~64 bytes per line = ~4096 bytes. */
#define HWINFO_TEXT_MAX    4096

/* ---------- CPUID helpers ---------- */

/* Plain CPUID. Returns the four standard registers via out-pointers.
 * Volatile because the result depends on EAX/ECX, which the compiler
 * cannot otherwise know. */
static inline void cpuid_raw(uint32_t leaf, uint32_t subleaf,
                             uint32_t *a, uint32_t *b,
                             uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

/* Highest standard / extended CPUID leaves the CPU supports. Cached
 * inside hwinfo_init so we never walk past them. */
static uint32_t s_max_std_leaf = 0;
static uint32_t s_max_ext_leaf = 0;

/* Read the 12-byte vendor string from CPUID.0H. Returns 0 if CPUID
 * is broken (shouldn't happen on x86_64 -- we already trampolined
 * through long mode -- but the check is cheap). */
static void cpuid_vendor(char out[ABI_HW_CPU_VENDOR_MAX]) {
    uint32_t a, b, c, d;
    cpuid_raw(0, 0, &a, &b, &c, &d);
    s_max_std_leaf = a;
    /* Vendor string is in EBX:EDX:ECX (note the order!). */
    memcpy(out + 0, &b, 4);
    memcpy(out + 4, &d, 4);
    memcpy(out + 8, &c, 4);
    out[12] = '\0';
    /* Strip any internal NUL noise some hypervisors put in. */
    for (size_t i = 0; i < 12; i++) {
        if (out[i] == '\0') { out[i] = ' '; }
    }
    out[12] = '\0';
    /* Trim trailing spaces. */
    for (int i = 11; i >= 0 && out[i] == ' '; i--) out[i] = '\0';
}

/* Read the brand string from CPUID.80000002..4 (16 bytes each =
 * 48 chars + NUL). If the CPU doesn't support extended leaves
 * 0x80000002..4 we fall back to the vendor string. */
static void cpuid_brand(char out[ABI_HW_CPU_BRAND_MAX],
                        const char *vendor) {
    uint32_t a, b, c, d;
    cpuid_raw(0x80000000u, 0, &a, &b, &c, &d);
    s_max_ext_leaf = a;
    if (s_max_ext_leaf < 0x80000004u) {
        /* Fall back to vendor + "<unknown>" so the field is never
         * empty -- keeps userland format strings simple. */
        size_t vlen = strlen(vendor);
        if (vlen >= ABI_HW_CPU_BRAND_MAX) vlen = ABI_HW_CPU_BRAND_MAX - 1;
        memcpy(out, vendor, vlen);
        out[vlen] = '\0';
        return;
    }
    uint32_t regs[12];
    cpuid_raw(0x80000002u, 0, &regs[0], &regs[1], &regs[2], &regs[3]);
    cpuid_raw(0x80000003u, 0, &regs[4], &regs[5], &regs[6], &regs[7]);
    cpuid_raw(0x80000004u, 0, &regs[8], &regs[9], &regs[10], &regs[11]);
    memcpy(out, regs, 48);
    out[ABI_HW_CPU_BRAND_MAX - 1] = '\0';

    /* Trim leading spaces (Intel pads the brand string with them). */
    size_t lead = 0;
    while (out[lead] == ' ') lead++;
    if (lead > 0) {
        size_t k = 0;
        while (out[lead + k] != '\0') {
            out[k] = out[lead + k];
            k++;
        }
        out[k] = '\0';
    }
}

/* CPUID.01H feature decode. Pulls EAX (family/model/stepping),
 * ECX/EDX (feature bits), folds them into our compact bitmap. */
static void cpuid_features(uint32_t *family, uint32_t *model,
                           uint32_t *stepping, uint32_t *features) {
    *family = *model = *stepping = *features = 0;
    if (s_max_std_leaf < 1) return;

    uint32_t a, b, c, d;
    cpuid_raw(1, 0, &a, &b, &c, &d);

    /* Family / model / stepping decode (per Intel SDM Vol.2A 3.3). */
    uint32_t base_family = (a >> 8)  & 0xF;
    uint32_t base_model  = (a >> 4)  & 0xF;
    uint32_t ext_family  = (a >> 20) & 0xFF;
    uint32_t ext_model   = (a >> 16) & 0xF;
    *stepping = a & 0xF;
    *family   = (base_family == 0xF) ? base_family + ext_family
                                     : base_family;
    *model    = ((base_family == 0x6) || (base_family == 0xF))
                ? (ext_model << 4) | base_model
                : base_model;

    uint32_t f = 0;
    if (d & (1u << 0))  f |= ABI_HW_CPU_FEAT_FPU;
    if (d & (1u << 4))  f |= ABI_HW_CPU_FEAT_TSC;
    if (d & (1u << 5))  f |= ABI_HW_CPU_FEAT_MSR;
    if (d & (1u << 6))  f |= ABI_HW_CPU_FEAT_PAE;
    if (d & (1u << 9))  f |= ABI_HW_CPU_FEAT_APIC;
    if (d & (1u << 25)) f |= ABI_HW_CPU_FEAT_SSE;
    if (d & (1u << 26)) f |= ABI_HW_CPU_FEAT_SSE2;
    if (d & (1u << 28)) f |= ABI_HW_CPU_FEAT_HT;
    if (c & (1u << 31)) f |= ABI_HW_CPU_FEAT_HYPER;

    /* Long mode + NX from extended leaf 0x80000001H:EDX. */
    if (s_max_ext_leaf >= 0x80000001u) {
        uint32_t ea, eb, ec, ed;
        cpuid_raw(0x80000001u, 0, &ea, &eb, &ec, &ed);
        if (ed & (1u << 20)) f |= ABI_HW_CPU_FEAT_NX;
        if (ed & (1u << 29)) f |= ABI_HW_CPU_FEAT_LM;
    }
    *features = f;
}

/* ---------- profile heuristic ---------- */

/* If the CPU brand contains "QEMU", "KVM", "Bochs", or "Virtual" we
 * call it a VM. Otherwise we look at the device list: a battery is
 * the laptop tell, an AHCI controller without battery is desktop.
 * This is intentionally approximate -- the user can override via
 * `hwtest <profile>` directly. */
static const char *infer_profile(const struct abi_hwinfo_summary *s) {
    const char *brand = s->cpu_brand;
    static const char *vm_markers[] = {
        "QEMU", "KVM", "Bochs", "Virtual", "VMware", "Hyper-V", NULL
    };
    for (size_t i = 0; vm_markers[i]; i++) {
        const char *m = vm_markers[i];
        size_t mlen = strlen(m);
        for (size_t j = 0; j + mlen <= ABI_HW_CPU_BRAND_MAX; j++) {
            if (memcmp(brand + j, m, mlen) == 0) return "vm";
        }
    }
    /* Hypervisor flag is the second-best fallback (set by all major
     * VM monitors via CPUID.01H:ECX[31]). */
    if (s->cpu_features & ABI_HW_CPU_FEAT_HYPER) return "vm";
    if (s->battery_count > 0)                    return "laptop";
    return "desktop";
}

/* ---------- snapshot / public API ---------- */

void hwinfo_init(void) {
    if (g_init_done) return;

    memset(&g_snap, 0, sizeof(g_snap));

    cpuid_vendor(g_snap.cpu_vendor);
    cpuid_brand (g_snap.cpu_brand, g_snap.cpu_vendor);
    cpuid_features(&g_snap.cpu_family, &g_snap.cpu_model,
                   &g_snap.cpu_stepping, &g_snap.cpu_features);

    g_snap.kernel_abi_ver = TOBY_ABI_VERSION;
    g_snap.cpu_count      = smp_cpu_count();
    if (g_snap.cpu_count == 0) g_snap.cpu_count = 1;
    /* M35F: surface the precise boot mode (ABI_BOOT_MODE_*) instead of
     * the legacy 0/1 flag. Old userland just checks `!= 0` for "any
     * safe mode active" and still sees the right truthiness; new
     * tools (hwreport, M35F) decode the full enum. */
    g_snap.safe_mode      = safemode_to_boot_mode(safemode_level());

    /* Initial profile is the static guess; refresh after devtest is
     * up so the battery_count tell takes effect. */
    const char *p = infer_profile(&g_snap);
    size_t plen = strlen(p);
    if (plen >= ABI_HW_PROFILE_MAX) plen = ABI_HW_PROFILE_MAX - 1;
    memcpy(g_profile, p, plen);
    g_profile[plen] = '\0';
    memcpy(g_snap.profile_hint, g_profile, sizeof(g_snap.profile_hint));

    g_init_done = true;

    SLOG_INFO(SLOG_SUB_HW,
              "hwinfo: cpu='%s' family=%u model=%u step=%u feat=0x%x",
              g_snap.cpu_brand, g_snap.cpu_family, g_snap.cpu_model,
              g_snap.cpu_stepping, g_snap.cpu_features);
}

/* Refresh the bus counters from devtest_enumerate. Allocates a small
 * stack array sized to ABI_DEVT_MAX_DEVICES (currently 64) so the
 * call is heap-free. */
static void refresh_bus_counts(struct abi_hwinfo_summary *s) {
    static struct abi_dev_info devs[ABI_DEVT_MAX_DEVICES];
    int n = devtest_enumerate(devs, ABI_DEVT_MAX_DEVICES,
                              ABI_DEVT_BUS_ALL);
    s->pci_count = s->usb_count = s->blk_count = 0;
    s->input_count = s->audio_count = 0;
    s->battery_count = s->hub_count = s->display_count = 0;
    for (int i = 0; i < n; i++) {
        switch (devs[i].bus) {
        case ABI_DEVT_BUS_PCI:     s->pci_count++;     break;
        case ABI_DEVT_BUS_USB:     s->usb_count++;     break;
        case ABI_DEVT_BUS_BLK:     s->blk_count++;     break;
        case ABI_DEVT_BUS_INPUT:   s->input_count++;   break;
        case ABI_DEVT_BUS_AUDIO:   s->audio_count++;   break;
        case ABI_DEVT_BUS_BATTERY: s->battery_count++; break;
        case ABI_DEVT_BUS_HUB:     s->hub_count++;     break;
        case ABI_DEVT_BUS_DISPLAY: s->display_count++; break;
        default: break;
        }
    }
}

/* Compute uptime in milliseconds from the PIT (same source slog
 * uses, so timestamps line up across the two ring buffers). */
static uint64_t boot_uptime_ms(void) {
    uint32_t hz = pit_hz();
    if (!hz) return 0;
    return (pit_ticks() * 1000ull) / hz;
}

void hwinfo_snapshot(struct abi_hwinfo_summary *out) {
    if (!g_init_done) hwinfo_init();

    refresh_bus_counts(&g_snap);

    g_snap.mem_total_pages = pmm_total_pages();
    g_snap.mem_used_pages  = pmm_used_pages();
    g_snap.mem_free_pages  = pmm_free_pages();
    g_snap.boot_uptime_ms  = boot_uptime_ms();
    g_snap.snapshot_epoch  = ++g_epoch;
    /* M35F: see init() above -- store the full boot-mode enum. */
    g_snap.safe_mode       = safemode_to_boot_mode(safemode_level());

    /* Re-evaluate profile now that we have device counts. The string
     * is stable across boots for a given machine. */
    const char *p = infer_profile(&g_snap);
    size_t plen = strlen(p);
    if (plen >= ABI_HW_PROFILE_MAX) plen = ABI_HW_PROFILE_MAX - 1;
    memset(g_snap.profile_hint, 0, sizeof(g_snap.profile_hint));
    memcpy(g_snap.profile_hint, p, plen);
    memset(g_profile, 0, sizeof(g_profile));
    memcpy(g_profile, p, plen);

    if (out) memcpy(out, &g_snap, sizeof(g_snap));
}

const struct abi_hwinfo_summary *hwinfo_current(void) {
    if (!g_init_done) hwinfo_init();
    return &g_snap;
}

const char *hwinfo_profile_hint(void) {
    if (!g_init_done) hwinfo_init();
    return g_profile;
}

/* ---------- text rendering ---------- */

/* Tiny buffered-printf helper. Used by both format_text and
 * dump_kprintf so the output of `cat /data/hwinfo.snap` matches
 * `hwinfo --snapshot` byte-for-byte. */
struct hwinfo_writer {
    char  *buf;     /* NULL => write straight to kprintf */
    size_t cap;
    size_t pos;
};

static void hw_emit(struct hwinfo_writer *w, const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (!w->buf) {
        kprintf("%s", line);
        return;
    }
    if (w->pos >= w->cap) return;
    size_t avail = w->cap - 1 - w->pos;
    size_t take  = ((size_t)n < avail) ? (size_t)n : avail;
    memcpy(w->buf + w->pos, line, take);
    w->pos += take;
    w->buf[w->pos] = '\0';
}

/* Decoded feature flag string, e.g. "fpu tsc msr sse sse2 lm nx".
 * Always fits in a fixed buffer because the bitmap is only 32 bits. */
static void format_features(uint32_t feat, char *buf, size_t cap) {
    static const struct { uint32_t bit; const char *name; } map[] = {
        { ABI_HW_CPU_FEAT_FPU,   "fpu"   },
        { ABI_HW_CPU_FEAT_TSC,   "tsc"   },
        { ABI_HW_CPU_FEAT_MSR,   "msr"   },
        { ABI_HW_CPU_FEAT_PAE,   "pae"   },
        { ABI_HW_CPU_FEAT_APIC,  "apic"  },
        { ABI_HW_CPU_FEAT_SSE,   "sse"   },
        { ABI_HW_CPU_FEAT_SSE2,  "sse2"  },
        { ABI_HW_CPU_FEAT_HT,    "ht"    },
        { ABI_HW_CPU_FEAT_LM,    "lm"    },
        { ABI_HW_CPU_FEAT_HYPER, "hyper" },
        { ABI_HW_CPU_FEAT_NX,    "nx"    },
    };
    if (cap == 0) return;
    buf[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (!(feat & map[i].bit)) continue;
        size_t need = strlen(map[i].name) + (off ? 1 : 0);
        if (off + need + 1 >= cap) break;
        if (off) buf[off++] = ' ';
        size_t nlen = strlen(map[i].name);
        memcpy(buf + off, map[i].name, nlen);
        off += nlen;
        buf[off] = '\0';
    }
}

/* Walk the live device list and emit one "BUS NAME [DRIVER] EXTRA"
 * line per device. Used inside both format_text and dump_kprintf so
 * the disk dump and the live console output are identical. */
static void emit_device_section(struct hwinfo_writer *w) {
    static struct abi_dev_info devs[ABI_DEVT_MAX_DEVICES];
    int n = devtest_enumerate(devs, ABI_DEVT_MAX_DEVICES,
                              ABI_DEVT_BUS_ALL);

    static const struct { uint8_t bus; const char *tag; } tags[] = {
        { ABI_DEVT_BUS_PCI,     "pci"     },
        { ABI_DEVT_BUS_HUB,     "hub"     },
        { ABI_DEVT_BUS_USB,     "usb"     },
        { ABI_DEVT_BUS_BLK,     "blk"     },
        { ABI_DEVT_BUS_INPUT,   "input"   },
        { ABI_DEVT_BUS_AUDIO,   "audio"   },
        { ABI_DEVT_BUS_BATTERY, "battery" },
        { ABI_DEVT_BUS_DISPLAY, "display" },
    };

    for (int i = 0; i < n; i++) {
        const struct abi_dev_info *d = &devs[i];
        const char *bus_tag = "?";
        for (size_t j = 0; j < sizeof(tags)/sizeof(tags[0]); j++) {
            if (tags[j].bus == d->bus) { bus_tag = tags[j].tag; break; }
        }
        const char *drv = (d->driver[0] != '\0') ? d->driver : "<none>";
        const char *xtra = (d->extra[0]  != '\0') ? d->extra  : "";
        hw_emit(w, "  %-7s %-24s [%-12s] %s\n",
                bus_tag, d->name, drv, xtra);
    }
    if (n == 0) {
        hw_emit(w, "  (no devices reported by devtest_enumerate)\n");
    }
}

size_t hwinfo_format_text(char *buf, size_t cap,
                          const struct abi_hwinfo_summary *snap) {
    if (!snap) snap = hwinfo_current();
    struct hwinfo_writer w = { buf, cap, 0 };
    if (buf && cap > 0) buf[0] = '\0';

    char feat[80];
    format_features(snap->cpu_features, feat, sizeof(feat));

    hw_emit(&w, "tobyOS hardware inventory (epoch=%lu uptime_ms=%lu)\n",
            (unsigned long)snap->snapshot_epoch,
            (unsigned long)snap->boot_uptime_ms);
    hw_emit(&w, "abi=%u  mode=%s  profile=%s\n",
            (unsigned)snap->kernel_abi_ver,
            snap->safe_mode ? "safe" : "normal",
            snap->profile_hint);
    hw_emit(&w, "cpu  : vendor=%s family=%u model=%u step=%u count=%u\n",
            snap->cpu_vendor, (unsigned)snap->cpu_family,
            (unsigned)snap->cpu_model, (unsigned)snap->cpu_stepping,
            (unsigned)snap->cpu_count);
    hw_emit(&w, "       brand=\"%s\"\n", snap->cpu_brand);
    hw_emit(&w, "       feat =%s\n", feat[0] ? feat : "(none)");
    hw_emit(&w, "mem  : total=%lu pg  used=%lu pg  free=%lu pg\n",
            (unsigned long)snap->mem_total_pages,
            (unsigned long)snap->mem_used_pages,
            (unsigned long)snap->mem_free_pages);
    hw_emit(&w, "bus  : pci=%u usb=%u blk=%u input=%u audio=%u "
                "battery=%u hub=%u display=%u\n",
            (unsigned)snap->pci_count, (unsigned)snap->usb_count,
            (unsigned)snap->blk_count, (unsigned)snap->input_count,
            (unsigned)snap->audio_count, (unsigned)snap->battery_count,
            (unsigned)snap->hub_count, (unsigned)snap->display_count);
    hw_emit(&w, "devices:\n");
    emit_device_section(&w);
    return w.pos;
}

void hwinfo_dump_kprintf(void) {
    if (!g_init_done) hwinfo_init();
    hwinfo_snapshot(NULL);

    char feat[80];
    format_features(g_snap.cpu_features, feat, sizeof(feat));
    kprintf("[hwinfo] === hardware inventory ===\n");
    kprintf("[hwinfo] cpu='%s' family=%u model=%u step=%u count=%u\n",
            g_snap.cpu_brand, (unsigned)g_snap.cpu_family,
            (unsigned)g_snap.cpu_model, (unsigned)g_snap.cpu_stepping,
            (unsigned)g_snap.cpu_count);
    kprintf("[hwinfo] feat=%s\n", feat[0] ? feat : "(none)");
    kprintf("[hwinfo] mem: total=%lu pg used=%lu pg free=%lu pg\n",
            (unsigned long)g_snap.mem_total_pages,
            (unsigned long)g_snap.mem_used_pages,
            (unsigned long)g_snap.mem_free_pages);
    kprintf("[hwinfo] bus: pci=%u usb=%u blk=%u input=%u audio=%u "
            "battery=%u hub=%u display=%u\n",
            (unsigned)g_snap.pci_count, (unsigned)g_snap.usb_count,
            (unsigned)g_snap.blk_count, (unsigned)g_snap.input_count,
            (unsigned)g_snap.audio_count, (unsigned)g_snap.battery_count,
            (unsigned)g_snap.hub_count, (unsigned)g_snap.display_count);
    kprintf("[hwinfo] mode=%s profile=%s abi=%u epoch=%lu\n",
            g_snap.safe_mode ? "safe" : "normal",
            g_snap.profile_hint, (unsigned)g_snap.kernel_abi_ver,
            (unsigned long)g_snap.snapshot_epoch);
}

/* Persist the rendered text to /data/hwinfo.snap. The kernel uses
 * a static buffer so we don't depend on the heap (the persistence
 * path can run very early). Returns the number of bytes written or
 * a negative VFS error code; 0 means /data wasn't reachable yet
 * and the caller should retry later. */
long hwinfo_persist(void) {
    if (!g_init_done) hwinfo_init();
    hwinfo_snapshot(NULL);

    static char text[HWINFO_TEXT_MAX];
    size_t n = hwinfo_format_text(text, sizeof(text), &g_snap);
    if (n == 0) return 0;

    int rc = vfs_write_all(HWINFO_SNAP_PATH, text, n);
    if (rc == VFS_OK) {
        SLOG_INFO(SLOG_SUB_HW, "hwinfo: persisted %lu bytes -> %s",
                  (unsigned long)n, HWINFO_SNAP_PATH);
        return (long)n;
    }
    /* /data may not be mounted yet (e.g. very early boot) -- treat
     * "no such directory" / "no mount" as a soft skip rather than a
     * hard failure. */
    if (rc == VFS_ERR_NOENT || rc == VFS_ERR_NOMOUNT ||
        rc == VFS_ERR_ROFS) {
        SLOG_DEBUG(SLOG_SUB_HW,
                   "hwinfo: /data unavailable (%s), snapshot deferred",
                   vfs_strerror(rc));
        return 0;
    }
    SLOG_WARN(SLOG_SUB_HW, "hwinfo: persist failed: %s",
              vfs_strerror(rc));
    return rc;
}
