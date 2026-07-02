/* gpu_intel_modeset.c -- Intel HD Graphics Gen 6+ modesetting driver.
 *
 * Implements real hardware modesetting for Intel integrated GPUs:
 * - PCI enumeration and BAR0 MMIO mapping
 * - VGA disable
 * - DPLL programming for standard pixel clocks
 * - Pipe timing configuration (HTOTAL, HBLANK, HSYNC, VTOTAL, etc.)
 * - Display plane setup (XRGB8888, linear tiling)
 * - Page flipping and vblank synchronization
 * - Hardware cursor via dedicated cursor plane
 */

#include <tobyos/gpu_intel.h>
#include <tobyos/types.h>
#include <tobyos/pci.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>
#include <tobyos/gfx.h>
#include <tobyos/perf.h>

#define printk kprintf

/* ======================================================================
 * Static state
 * ====================================================================== */

static struct intel_gpu g_igpu;
static uint32_t g_cursor_buf[64 * 64] __attribute__((aligned(4096)));

/* Standard VESA/CEA timing table */
static const struct intel_mode g_modes[] = {
    /* 640x480@60 */
    { 640, 480, 60, 25175,
      640, 656, 752, 800,
      480, 490, 492, 525 },
    /* 800x600@60 */
    { 800, 600, 60, 40000,
      800, 840, 968, 1056,
      600, 601, 605, 628 },
    /* 1024x768@60 */
    { 1024, 768, 60, 65000,
      1024, 1048, 1184, 1344,
      768, 771, 777, 806 },
    /* 1280x720@60 (720p) */
    { 1280, 720, 60, 74250,
      1280, 1390, 1430, 1650,
      720, 725, 730, 750 },
    /* 1280x1024@60 */
    { 1280, 1024, 60, 108000,
      1280, 1328, 1440, 1688,
      1024, 1025, 1028, 1066 },
    /* 1366x768@60 */
    { 1366, 768, 60, 85500,
      1366, 1436, 1579, 1792,
      768, 771, 774, 798 },
    /* 1440x900@60 */
    { 1440, 900, 60, 106500,
      1440, 1520, 1672, 1904,
      900, 903, 909, 934 },
    /* 1600x900@60 */
    { 1600, 900, 60, 108000,
      1600, 1624, 1704, 1800,
      900, 901, 904, 1000 },
    /* 1920x1080@60 (1080p) */
    { 1920, 1080, 60, 148500,
      1920, 2008, 2052, 2200,
      1080, 1084, 1089, 1125 },
    /* 2560x1440@60 (1440p) */
    { 2560, 1440, 60, 241500,
      2560, 2608, 2640, 2720,
      1440, 1443, 1448, 1481 },
};

#define NUM_MODES ((int)(sizeof(g_modes) / sizeof(g_modes[0])))

/* ======================================================================
 * Register access helpers
 * ====================================================================== */

static uint32_t igpu_read(uint32_t offset) {
    if (!g_igpu.mmio) return 0;
    return g_igpu.mmio[offset / 4];
}

static void igpu_write(uint32_t offset, uint32_t value) {
    if (!g_igpu.mmio) return;
    g_igpu.mmio[offset / 4] = value;
}

static void igpu_set_bits(uint32_t offset, uint32_t mask) {
    igpu_write(offset, igpu_read(offset) | mask);
}

static void igpu_clear_bits(uint32_t offset, uint32_t mask) {
    igpu_write(offset, igpu_read(offset) & ~mask);
}

static void igpu_wait_bits(uint32_t offset, uint32_t mask, uint32_t expected) {
    for (int i = 0; i < 100000; i++) {
        if ((igpu_read(offset) & mask) == expected) return;
        for (volatile int j = 0; j < 100; j++) {}
    }
}

/* ======================================================================
 * PCI probe
 * ====================================================================== */

static int igpu_is_supported(uint16_t device_id) {
    switch (device_id) {
    case INTEL_HD2000:
    case INTEL_HD3000:
    case INTEL_HD4000:
    case INTEL_HD4600:
    case INTEL_HD530:
    case INTEL_UHD620:
    case INTEL_UHD630:
        return 1;
    default:
        if (device_id >= 0x0100 && device_id <= 0x0126) return 1;
        if (device_id >= 0x0150 && device_id <= 0x016A) return 1;
        if (device_id >= 0x0400 && device_id <= 0x042E) return 1;
        if (device_id >= 0x1900 && device_id <= 0x193D) return 1;
        return 0;
    }
}

static int igpu_get_gen(uint16_t device_id) {
    if (device_id >= 0x0100 && device_id <= 0x0126) return 6;
    if (device_id >= 0x0150 && device_id <= 0x016A) return 7;
    if (device_id >= 0x0400 && device_id <= 0x042E) return 7;
    if (device_id >= 0x1900 && device_id <= 0x193D) return 9;
    if (device_id >= 0x3E00 && device_id <= 0x3EFF) return 9;
    return 6;
}

static int igpu_probe_pci(void) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t vendor_dev = pci_cfg_read32(bus, dev, func, 0x00);
                if (vendor_dev == 0xFFFFFFFF) continue;

                uint16_t vendor = vendor_dev & 0xFFFF;
                uint16_t device = (vendor_dev >> 16) & 0xFFFF;

                if (vendor != 0x8086) continue;

                uint32_t class_rev = pci_cfg_read32(bus, dev, func, 0x08);
                uint8_t base_class = (class_rev >> 24) & 0xFF;
                uint8_t sub_class = (class_rev >> 16) & 0xFF;

                if (base_class != 0x03 || sub_class != 0x00) continue;

                if (!igpu_is_supported(device)) continue;

                uint32_t bar0 = pci_cfg_read32(bus, dev, func, 0x10);
                if ((bar0 & 0x1) != 0) continue;

                uint64_t mmio_base = bar0 & 0xFFFFFFF0u;
                if (bar0 & 0x4) {
                    uint32_t bar1 = pci_cfg_read32(bus, dev, func, 0x14);
                    mmio_base |= ((uint64_t)bar1) << 32;
                }

                /* Determine MMIO (GTTMMADR / BAR0) size from the BAR
                 * sizing dance. On gen8/9 this BAR is 16 MiB: registers in
                 * the low 8 MiB, the GGTT PTE array in the top 8 MiB. The
                 * old 4 MiB clamp below was fine for the register-only
                 * modeset path but truncated the GGTT half, so GT accel
                 * (intel_gt_init) couldn't reach its page tables. Keep the
                 * BAR's real size (capped at a sane 16 MiB so a bogus
                 * read can't make us map something enormous). */
                pci_cfg_write32(bus, dev, func, 0x10, 0xFFFFFFFF);
                uint32_t size_mask = pci_cfg_read32(bus, dev, func, 0x10);
                pci_cfg_write32(bus, dev, func, 0x10, bar0);
                size_t mmio_size = ~(size_mask & 0xFFFFFFF0u) + 1;
                if (mmio_size < 0x80000)   mmio_size = 0x80000;     /* 512 KiB floor */
                if (mmio_size > 0x1000000) mmio_size = 0x1000000;   /* 16 MiB ceiling */

                /* Enable bus mastering and memory space */
                uint32_t cmd = pci_cfg_read32(bus, dev, func, 0x04);
                cmd |= (1u << 1) | (1u << 2);
                pci_cfg_write32(bus, dev, func, 0x04, cmd);

                g_igpu.mmio_phys = mmio_base;
                g_igpu.mmio_size = mmio_size;
                g_igpu.device_id = device;
                g_igpu.gen = igpu_get_gen(device);
                g_igpu.has_pch = (g_igpu.gen >= 6);
                g_igpu.bus = (uint8_t)bus;
                g_igpu.dev = (uint8_t)dev;
                g_igpu.func = (uint8_t)func;
                g_igpu.bdf_valid = 1;

                printk("[intel_gpu] found device %04x at %02x:%02x.%d "
                       "gen=%d mmio=0x%llx size=0x%llx\n",
                       device, bus, dev, func,
                       g_igpu.gen, (unsigned long long)mmio_base,
                       (unsigned long long)mmio_size);
                return 1;
            }
        }
    }
    return 0;
}

/* ======================================================================
 * MMIO mapping
 * ====================================================================== */

static int igpu_map_mmio(void) {
    /* Map the MMIO BAR via HHDM (already identity-mapped by Limine for
     * RESERVED regions that cover PCI apertures). If it's outside HHDM,
     * fall back to vmm_map + pmm_phys_to_virt. */
    uint64_t virt = g_igpu.mmio_phys + pmm_hhdm_offset();
    vmm_map(virt, g_igpu.mmio_phys, g_igpu.mmio_size,
            VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_NOCACHE);
    g_igpu.mmio = (volatile uint32_t *)virt;
    if (!g_igpu.mmio) {
        printk("[intel_gpu] failed to map MMIO\n");
        return -1;
    }
    return 0;
}

/* ======================================================================
 * Framebuffer allocation
 * ====================================================================== */

static int igpu_alloc_framebuffer(int width, int height) {
    int stride = width * 4;
    size_t fb_size = (size_t)stride * height;
    fb_size = (fb_size + 0xFFF) & ~0xFFFULL;

    uint64_t fb_phys = pmm_alloc_pages(fb_size / 4096);
    if (!fb_phys) {
        printk("[intel_gpu] failed to allocate framebuffer (%zu bytes)\n", fb_size);
        return -1;
    }

    g_igpu.framebuffer = (uint32_t *)pmm_phys_to_virt(fb_phys);
    if (!g_igpu.framebuffer) {
        printk("[intel_gpu] failed to map framebuffer\n");
        return -1;
    }

    g_igpu.fb_phys = fb_phys;
    g_igpu.fb_size = fb_size;
    g_igpu.stride = stride;
    g_igpu.width = width;
    g_igpu.height = height;

    memset(g_igpu.framebuffer, 0, fb_size);

    /* Allocate back buffer for double buffering */
    uint64_t back_phys = pmm_alloc_pages(fb_size / 4096);
    if (back_phys) {
        g_igpu.back_buffer = (uint32_t *)pmm_phys_to_virt(back_phys);
        g_igpu.back_phys = back_phys;
        if (g_igpu.back_buffer)
            memset(g_igpu.back_buffer, 0, fb_size);
    }

    return 0;
}

/* ======================================================================
 * GTT (Graphics Translation Table) setup
 * ====================================================================== */

static void igpu_setup_gtt(uint64_t fb_phys, size_t fb_size) {
    size_t pages = fb_size / 4096;
    for (size_t i = 0; i < pages; i++) {
        uint64_t addr = fb_phys + i * 4096;
        uint32_t gtt_entry = (uint32_t)(addr & 0xFFFFF000u) | GTT_ENTRY_VALID;
        igpu_write(GTT_BASE + (uint32_t)(i * 4), gtt_entry);
    }
    /* Read back to ensure writes are flushed */
    (void)igpu_read(GTT_BASE);
}

/* ======================================================================
 * VGA disable
 * ====================================================================== */

static void igpu_disable_vga(void) {
    igpu_write(VGACNTRL, VGA_DISABLE);
    (void)igpu_read(VGACNTRL);
}

/* ======================================================================
 * DPLL programming
 * ====================================================================== */

/* N, M1, M2, P1, P2 divider calculation for a target pixel clock.
 * Reference clock is 120 MHz for PCH-based platforms.
 * VCO = refclk * (5 * (m1 + 2) + (m2 + 2)) / (n + 2)
 * dotclock = VCO / (p1 * p2)
 */

struct dpll_params {
    int n, m1, m2, p1, p2;
    uint32_t dpll_val;
    uint32_t fp_val;
};

static int igpu_calc_dpll(uint32_t target_khz, struct dpll_params *out) {
    int ref_khz = 120000;
    int best_error = 0x7FFFFFFF;
    int found = 0;

    int n_min = 1, n_max = 6;
    int m1_min = 12, m1_max = 22;
    int m2_min = 5, m2_max = 9;
    int p1_min = 1, p1_max = 8;
    int p2_options[] = {5, 10};

    for (int n = n_min; n <= n_max; n++) {
        for (int m1 = m1_min; m1 <= m1_max; m1++) {
            for (int m2 = m2_min; m2 <= m2_max; m2++) {
                if (m2 >= m1) continue;
                int m = 5 * (m1 + 2) + (m2 + 2);
                int vco = (int)((int64_t)ref_khz * m / (n + 2));

                if (vco < 1750000 || vco > 3500000) continue;

                for (int pi = 0; pi < 2; pi++) {
                    int p2 = p2_options[pi];
                    for (int p1 = p1_min; p1 <= p1_max; p1++) {
                        int p = p1 * p2;
                        int dotclock = vco / p;
                        int error = dotclock - (int)target_khz;
                        if (error < 0) error = -error;

                        if (error < best_error) {
                            best_error = error;
                            out->n = n;
                            out->m1 = m1;
                            out->m2 = m2;
                            out->p1 = p1;
                            out->p2 = p2;
                            found = 1;
                        }
                    }
                }
            }
        }
    }

    if (!found) return -1;

    /* Encode FP register: N in bits 16-23, M1 in bits 8-15, M2 in bits 0-7 */
    out->fp_val = ((uint32_t)(out->n - 1) << 16) |
                  ((uint32_t)(out->m1 - 2) << 8) |
                  ((uint32_t)(out->m2 - 2));

    /* DPLL register value */
    out->dpll_val = DPLL_VCO_ENABLE | DPLL_SDVO_HIGH_SPEED;
    out->dpll_val |= ((uint32_t)(out->p1 - 1) << 16);
    if (out->p2 == 5)
        out->dpll_val |= (1u << 24);

    return 0;
}

static void igpu_program_dpll(int pipe, struct dpll_params *params) {
    uint32_t dpll_reg = (pipe == 0) ? DPLL_A : DPLL_B;
    uint32_t fp0_reg = (pipe == 0) ? FPA0 : FPB0;
    uint32_t fp1_reg = (pipe == 0) ? FPA1 : FPB1;

    /* Disable DPLL first */
    igpu_clear_bits(dpll_reg, DPLL_VCO_ENABLE);
    igpu_wait_bits(dpll_reg, DPLL_VCO_ENABLE, 0);

    /* Program dividers */
    igpu_write(fp0_reg, params->fp_val);
    igpu_write(fp1_reg, params->fp_val);

    /* Enable DPLL */
    igpu_write(dpll_reg, params->dpll_val);
    (void)igpu_read(dpll_reg);

    /* Wait for DPLL lock */
    for (volatile int i = 0; i < 200000; i++) {}
}

/* ======================================================================
 * Pipe timing configuration
 * ====================================================================== */

static void igpu_set_pipe_timings(int pipe, const struct intel_mode *mode) {
    uint32_t base = (pipe == 0) ? 0 : 0x1000;

    uint32_t htotal_val = ((uint32_t)(mode->h_total - 1) << 16) |
                          (uint32_t)(mode->h_display - 1);
    uint32_t hblank_val = ((uint32_t)(mode->h_total - 1) << 16) |
                          (uint32_t)(mode->h_display - 1);
    uint32_t hsync_val = ((uint32_t)(mode->h_sync_end - 1) << 16) |
                         (uint32_t)(mode->h_sync_start - 1);

    uint32_t vtotal_val = ((uint32_t)(mode->v_total - 1) << 16) |
                          (uint32_t)(mode->v_display - 1);
    uint32_t vblank_val = ((uint32_t)(mode->v_total - 1) << 16) |
                          (uint32_t)(mode->v_display - 1);
    uint32_t vsync_val = ((uint32_t)(mode->v_sync_end - 1) << 16) |
                         (uint32_t)(mode->v_sync_start - 1);

    uint32_t pipesrc_val = ((uint32_t)(mode->width - 1) << 16) |
                           (uint32_t)(mode->height - 1);

    igpu_write(HTOTAL_A + base, htotal_val);
    igpu_write(HBLANK_A + base, hblank_val);
    igpu_write(HSYNC_A + base, hsync_val);
    igpu_write(VTOTAL_A + base, vtotal_val);
    igpu_write(VBLANK_A + base, vblank_val);
    igpu_write(VSYNC_A + base, vsync_val);
    igpu_write(PIPESRC_A + base, pipesrc_val);
}

/* ======================================================================
 * Pipe enable/disable
 * ====================================================================== */

static void igpu_enable_pipe(int pipe) {
    uint32_t pipe_reg = (pipe == 0) ? PIPE_A_CONF : PIPE_B_CONF;
    igpu_set_bits(pipe_reg, PIPE_CONF_ENABLE);
    igpu_wait_bits(pipe_reg, PIPE_CONF_STATE_ACTIVE, PIPE_CONF_STATE_ACTIVE);
}

static void igpu_disable_pipe(int pipe) {
    uint32_t pipe_reg = (pipe == 0) ? PIPE_A_CONF : PIPE_B_CONF;
    igpu_clear_bits(pipe_reg, PIPE_CONF_ENABLE);
    igpu_wait_bits(pipe_reg, PIPE_CONF_STATE_ACTIVE, 0);
}

/* ======================================================================
 * Display plane configuration
 * ====================================================================== */

static void igpu_configure_plane(int pipe, uint64_t fb_addr, int stride) {
    uint32_t ctrl_reg = (pipe == 0) ? PLANE_A_CTRL : PLANE_B_CTRL;
    uint32_t addr_reg = (pipe == 0) ? PLANE_A_ADDR : PLANE_B_ADDR;
    uint32_t stride_reg = (pipe == 0) ? PLANE_A_STRIDE : PLANE_B_STRIDE;

    /* Disable plane first */
    igpu_clear_bits(ctrl_reg, PLANE_ENABLE);
    igpu_write(addr_reg, 0);
    (void)igpu_read(addr_reg);

    /* Set stride */
    igpu_write(stride_reg, (uint32_t)stride);

    /* Enable plane: XRGB8888, no tiling, pipe select */
    uint32_t ctrl = PLANE_ENABLE | PLANE_FORMAT_XRGB8888 | PLANE_TILING_NONE;
    if (pipe == 1) ctrl |= (1u << 24);
    igpu_write(ctrl_reg, ctrl);

    /* Set framebuffer base address (triggers flip) */
    igpu_write(addr_reg, (uint32_t)(fb_addr & 0xFFFFFFFF));
    (void)igpu_read(addr_reg);
}

/* ======================================================================
 * Output (HDMI/DP/Analog) configuration
 * ====================================================================== */

static void igpu_enable_output(int pipe) {
    /* Try HDMI-B first */
    uint32_t hdmi_val = igpu_read(HDMI_B);
    if (hdmi_val != 0xFFFFFFFF) {
        hdmi_val |= (1u << 31);  /* Enable */
        hdmi_val &= ~(3u << 29); /* Clear pipe select */
        hdmi_val |= ((uint32_t)pipe << 30);
        hdmi_val |= (1u << 4);   /* HDMI mode (vs DVI) */
        igpu_write(HDMI_B, hdmi_val);
        return;
    }

    /* Fall back to analog (ADPA/VGA) */
    uint32_t adpa_val = igpu_read(ADPA);
    adpa_val |= (1u << 31);   /* Enable */
    adpa_val &= ~(3u << 30);  /* Clear pipe select */
    adpa_val |= ((uint32_t)pipe << 30);
    adpa_val |= (3u << 10);   /* Sync polarity: both active high */
    igpu_write(ADPA, adpa_val);
}

/* ======================================================================
 * Interrupt / Vblank
 * ====================================================================== */

static void igpu_enable_vblank_irq(int pipe) {
    uint32_t mask = (pipe == 0) ? DE_PIPE_A_VBLANK : DE_PIPE_B_VBLANK;
    igpu_clear_bits(DEIMR, mask);
    igpu_set_bits(DEIER, mask);
}

/* ======================================================================
 * Stage 1: read-only GT + display reconnaissance
 *
 * Everything here only READS registers/config space and logs. No writes,
 * no VGA disable, no modeset -- so it can never disturb the working Limine
 * display, even on the un-serial-debuggable EliteDesk. The numbers it
 * prints (GGTT size, stolen base/size, blitter ring head/tail/ctl, and
 * which pipe+plane Limine left scanning out) are exactly what Stages 2-4
 * (BLT ring + hardware page-flip) need and what QEMU can't show us.
 * ====================================================================== */

/* GEN8_BCS_RING_BASE / RING_* now live in gpu_intel.h (shared with the
 * Stage 2 GT bring-up below). */

/* Gen6+ graphics control regs live in the GPU's PCI CONFIG space. */
#define INTEL_PCI_MGGC0      0x50u             /* Mirror of GMCH gfx ctrl   */
#define INTEL_PCI_BDSM       0x5Cu             /* Base of Data Stolen Memory */

/* PIPE_A source size + the primary plane Limine programmed. We read these
 * to learn the active resolution + scanout surface WITHOUT changing them. */

static uint32_t recon_ring_reg(uint32_t ring_base, uint32_t off) {
    return igpu_read(ring_base + off);
}

void intel_gpu_gt_recon(void) {
    if (!igpu_probe_pci()) {
        kprintf("[intel_gpu] GT recon: no supported Intel GPU\n");
        return;
    }
    if (igpu_map_mmio() < 0) {
        kprintf("[intel_gpu] GT recon: MMIO map failed\n");
        return;
    }

    kprintf("[intel_gpu] GT recon: device=%04x gen=%d mmio=0x%llx size=0x%llx\n",
            g_igpu.device_id, g_igpu.gen,
            (unsigned long long)g_igpu.mmio_phys,
            (unsigned long long)g_igpu.mmio_size);

    /* --- Stolen memory + GGTT geometry, from PCI config (read-only). On
     * Gen8/9 MGGC0[15:8] encodes GGTT size, [7:6]+[15:8] encode stolen
     * size; BDSM[31:20] is the stolen-memory base. We log raw + decoded
     * so a wrong decode is still debuggable from the raw value. */
    if (g_igpu.bdf_valid) {
        uint32_t mggc0 = pci_cfg_read32(g_igpu.bus, g_igpu.dev, g_igpu.func,
                                        INTEL_PCI_MGGC0);
        uint32_t bdsm  = pci_cfg_read32(g_igpu.bus, g_igpu.dev, g_igpu.func,
                                        INTEL_PCI_BDSM);
        uint32_t ggms  = (mggc0 >> 6) & 0x3u;          /* GGTT size sel    */
        uint32_t gms   = (mggc0 >> 8) & 0xFFu;         /* stolen size sel  */
        uint64_t stolen_base = (uint64_t)(bdsm & 0xFFF00000u);
        kprintf("[intel_gpu] GT recon: MGGC0=0x%08x (GGMS=%u GMS=%u) "
                "BDSM=0x%08x stolen_base=0x%llx\n",
                mggc0, ggms, gms, bdsm,
                (unsigned long long)stolen_base);
    }

    /* --- Blitter ring state. On a Limine boot nothing has touched the
     * BCS, so we expect head==tail and RING_CTL length/enable telling us
     * whether firmware left a ring configured. This is the register set
     * Stage 2 will program. */
    uint32_t bcs_ctl   = recon_ring_reg(GEN8_BCS_RING_BASE, RING_CTL);
    uint32_t bcs_head  = recon_ring_reg(GEN8_BCS_RING_BASE, RING_HEAD);
    uint32_t bcs_tail  = recon_ring_reg(GEN8_BCS_RING_BASE, RING_TAIL);
    uint32_t bcs_start = recon_ring_reg(GEN8_BCS_RING_BASE, RING_START);
    kprintf("[intel_gpu] GT recon: BCS ring ctl=0x%08x head=0x%08x "
            "tail=0x%08x start=0x%08x\n",
            bcs_ctl, bcs_head, bcs_tail, bcs_start);

    /* --- Active display: which pipe/plane is Limine scanning out, and at
     * what surface address + stride. PIPESRC_A holds (w-1)<<16|(h-1);
     * PLANE_A_CTRL bit31 = enabled; PLANE_A_SURF = scanout phys; PLANE_A
     * _STRIDE = bytes/line. We only READ -- this is the surface a future
     * page-flip would retarget. */
    uint32_t pipesrc = igpu_read(PIPESRC_A);
    uint32_t pa_ctrl = igpu_read(PLANE_A_CTRL);
    uint32_t pa_surf = igpu_read(PLANE_A_SURF);
    uint32_t pa_strd = igpu_read(PLANE_A_STRIDE);
    uint32_t aw = ((pipesrc >> 16) & 0xFFFFu) + 1u;
    uint32_t ah = (pipesrc & 0xFFFFu) + 1u;
    kprintf("[intel_gpu] GT recon: PIPE_A src=%ux%u PLANE_A ctrl=0x%08x "
            "(en=%d) surf=0x%08x stride=%u\n",
            aw, ah, pa_ctrl, (pa_ctrl >> 31) & 1u, pa_surf, pa_strd);

    kprintf("[intel_gpu] GT recon: complete (read-only, display untouched)\n");

    /* Deliberately do NOT set g_igpu.ready -- recon must not make the rest
     * of the (modeset) driver think it owns the display. Stage 2+ will key
     * off its own state. Leave the MMIO mapping in place; it's harmless and
     * the later stages reuse it. */
}

/* ======================================================================
 * Stage 2: GT / render-engine bring-up  (forcewake + GGTT + BCS ring)
 *
 * SAFETY: touches ONLY forcewake, the GGTT (top half of BAR0), the BCS
 * ring registers (0x22000), and pages we allocate. It never writes a
 * pipe/plane/DPLL/VGA register, so the Limine scanout is untouched -- the
 * worst failure here is "the GT didn't respond", logged, with the self-test
 * flag left 0 so nothing downstream trusts the GPU. Unverifiable in QEMU
 * (no Intel GT); the self-test log line is the real-HW go/no-go signal.
 * ====================================================================== */

/* GT bring-up state, separate from the (unused) modeset state in g_igpu. */
static struct {
    int       fw_render;          /* forcewake acquired (render domain)  */
    int       fw_blitter;         /* forcewake acquired (blitter domain) */
    uint64_t  ggtt_mmio;          /* virt base of the GGTT PTE array      */
    uint32_t  ring_pages;         /* BCS ring size in 4 KiB pages         */
    uint64_t  ring_phys;          /* ring buffer phys                     */
    uint32_t *ring_virt;          /* ring buffer virt (HHDM)              */
    uint32_t  ring_ggtt_off;      /* ring's offset within GGTT (== GT VA) */
    uint64_t  scratch_phys;       /* self-test target page phys           */
    uint32_t *scratch_virt;       /* self-test target page virt           */
    uint32_t  scratch_ggtt_off;   /* scratch GT VA                        */
    int       selftest_ok;        /* GT executed our command stream       */
} g_gt;

int intel_gt_selftest_ok(void) { return g_gt.selftest_ok; }

/* Acquire a forcewake domain: write (mask<<16 | bit), then poll the ack.
 * Without this, gen9 render/blitter MMIO reads return 0 and writes drop. */
static int gt_forcewake_get(uint32_t req_reg, uint32_t ack_reg) {
    uint32_t set = (1u << (FORCEWAKE_KERNEL_BIT + 16)) |
                   (1u << FORCEWAKE_KERNEL_BIT);
    igpu_write(req_reg, set);
    (void)igpu_read(req_reg);            /* posting read */
    for (int i = 0; i < 100000; i++) {
        if (igpu_read(ack_reg) & (1u << FORCEWAKE_KERNEL_BIT))
            return 1;
        for (volatile int j = 0; j < 50; j++) {}
    }
    return 0;
}

static void gt_forcewake_put(uint32_t req_reg) {
    uint32_t clr = (1u << (FORCEWAKE_KERNEL_BIT + 16)) | 0u;  /* mask, val=0 */
    igpu_write(req_reg, clr);
    (void)igpu_read(req_reg);
}

/* Install a 4 KiB page into the GGTT at byte offset `ggtt_off` (which is
 * also the GPU virtual address the engines will use). Gen8/9 PTEs are 8
 * bytes in the top half of BAR0; gen7 (IVB/HSW) PTEs are 4 bytes at
 * GTT_BASE (2 MiB in), and pack the address' high bits [38:32] into PTE
 * bits [11:4] (GEN6_PTE_ADDR_ENCODE) so pages above 4 GiB still map. */
static void gt_ggtt_map(uint32_t ggtt_off, uint64_t phys) {
    uint32_t pte_index = ggtt_off >> 12;
    if (g_igpu.gen == 7) {
        volatile uint32_t *ggtt = (volatile uint32_t *)g_gt.ggtt_mmio;
        ggtt[pte_index] = (uint32_t)((phys & 0xFFFFF000ull) |
                                     ((phys >> 28) & 0xFF0ull) |
                                     GTT_ENTRY_VALID);
        (void)ggtt[pte_index];           /* posting read */
        return;
    }
    volatile uint64_t *ggtt = (volatile uint64_t *)g_gt.ggtt_mmio;
    ggtt[pte_index] = (phys & ~0xFFFull) | GEN8_GGTT_PTE_PRESENT;
    (void)ggtt[pte_index];               /* posting read */
}

/* Bring up the BCS ring: program START/CTL, leave HEAD=TAIL=0 (empty). */
static int gt_ring_init(void) {
    /* The ring buffer must be GGTT-mapped; the engine fetches commands
     * through its GT VA, not the raw phys. We place it at a fixed GGTT
     * offset well above anything the display uses. */
    g_gt.ring_pages    = 1;                       /* 4 KiB ring is plenty */
    g_gt.ring_ggtt_off = 0x40000u;                /* 256 KiB into GGTT    */
    g_gt.ring_phys = pmm_alloc_pages(g_gt.ring_pages);
    if (!g_gt.ring_phys) return 0;
    g_gt.ring_virt = (uint32_t *)pmm_phys_to_virt(g_gt.ring_phys);
    memset(g_gt.ring_virt, 0, g_gt.ring_pages * 4096u);
    gt_ggtt_map(g_gt.ring_ggtt_off, g_gt.ring_phys);

    /* Stop the ring first (CTL=0), then point it at our buffer. */
    igpu_write(GEN8_BCS_RING_BASE + RING_CTL, 0);
    (void)igpu_read(GEN8_BCS_RING_BASE + RING_CTL);
    igpu_write(GEN8_BCS_RING_BASE + RING_HEAD, 0);
    igpu_write(GEN8_BCS_RING_BASE + RING_TAIL, 0);
    /* RING_START takes the ring's GT virtual address (its GGTT offset). */
    igpu_write(GEN8_BCS_RING_BASE + RING_START, g_gt.ring_ggtt_off);
    /* CTL: (npages*4KiB - 4KiB) in bits 12.., length field is (size/4K - 1)
     * encoded in bits 20:12 ... gen8 uses (num_pages-1)<<12 | ENABLE. */
    uint32_t ctl = ((g_gt.ring_pages - 1u) << 12) | RING_CTL_ENABLE;
    igpu_write(GEN8_BCS_RING_BASE + RING_CTL, ctl);
    (void)igpu_read(GEN8_BCS_RING_BASE + RING_CTL);
    return 1;
}

/* Emit MI_STORE_DWORD_IMM(scratch <- magic) + MI_NOOP, advance TAIL, and
 * wait for HEAD to catch up == the GT executed our stream. Then verify the
 * scratch page actually holds the magic. This is the whole go/no-go test. */
#define GT_SELFTEST_MAGIC  0x7ED5C0DEu
static int gt_run_selftest(void) {
    g_gt.scratch_phys = pmm_alloc_pages(1);
    if (!g_gt.scratch_phys) return 0;
    g_gt.scratch_virt = (uint32_t *)pmm_phys_to_virt(g_gt.scratch_phys);
    g_gt.scratch_virt[0] = 0;                       /* clear target */
    g_gt.scratch_ggtt_off = 0x41000u;               /* next GGTT page */
    gt_ggtt_map(g_gt.scratch_ggtt_off, g_gt.scratch_phys);

    /* Diagnostic: read the scratch page's GGTT PTE back so a failed store
     * can be attributed to the store COMMAND (PTE correct) vs the MAPPING
     * (PTE wrong). gen7 PTEs are 4 bytes at GTT_BASE; gen8/9 are 8 bytes. */
    if (g_igpu.gen == 7) {
        volatile uint32_t *ggtt = (volatile uint32_t *)g_gt.ggtt_mmio;
        kprintf("[intel_gpu] GT selftest: scratch phys=0x%llx off=0x%x "
                "pte=0x%08x\n", (unsigned long long)g_gt.scratch_phys,
                g_gt.scratch_ggtt_off, ggtt[g_gt.scratch_ggtt_off >> 12]);
    }

    /* Build the command stream at ring head 0. MI_STORE_DWORD_IMM:
     *   gen8: dword0=cmd, dword1:2=64-bit GT addr, dword3=immediate.
     *   gen7: dword0=cmd, dword1=32-bit GT addr, dword2=immediate. */
    uint32_t *r = g_gt.ring_virt;
    int n = 0;
    if (g_igpu.gen == 7) {
        r[n++] = MI_STORE_DWORD_IMM_GEN7;           /* DW0 header (len 2) */
        r[n++] = 0;                                 /* DW1 reserved       */
        r[n++] = g_gt.scratch_ggtt_off;             /* DW2 addr (GT VA)   */
        r[n++] = GT_SELFTEST_MAGIC;                 /* DW3 immediate data */
        r[n++] = MI_NOOP;
    } else {
        r[n++] = MI_STORE_DWORD_IMM_GEN8;
        r[n++] = g_gt.scratch_ggtt_off;             /* addr lo (GT VA) */
        r[n++] = 0;                                 /* addr hi         */
        r[n++] = GT_SELFTEST_MAGIC;                 /* immediate data  */
        r[n++] = MI_NOOP;
    }
    /* TAIL is a byte offset into the ring; must be 8-byte (qword) aligned. */
    uint32_t tail = (uint32_t)(n * 4);
    tail = (tail + 7u) & ~7u;
    igpu_write(GEN8_BCS_RING_BASE + RING_TAIL, tail & RING_TAIL_MASK);
    (void)igpu_read(GEN8_BCS_RING_BASE + RING_TAIL);

    /* Wait for HEAD to reach TAIL (engine drained our commands). */
    int drained = 0;
    for (int i = 0; i < 200000; i++) {
        uint32_t head = igpu_read(GEN8_BCS_RING_BASE + RING_HEAD)
                        & RING_HEAD_MASK;
        if (head == (tail & RING_HEAD_MASK)) { drained = 1; break; }
        for (volatile int j = 0; j < 50; j++) {}
    }

    /* The store completes ASYNCHRONOUSLY: the ring HEAD advancing (drained)
     * means the engine PARSED the command, but MI_STORE_DWORD_IMM's write
     * travels the GPU memory pipeline and may not be visible to the CPU yet.
     * (This is why an earlier single-shot read was flaky -- one boot caught
     * the write, the next read 0 before it landed.) So POLL the scratch
     * value, flushing our write-back cache line each iteration, until the
     * magic appears or we time out. No new GPU command encoding needed --
     * just tolerance for write latency. */
    uint32_t got = 0;
    int settle = 0;
    for (int i = 0; i < 100000; i++) {
        __asm__ __volatile__("clflush (%0)" :: "r"(g_gt.scratch_virt)
                             : "memory");
        __asm__ __volatile__("mfence" ::: "memory");
        got = g_gt.scratch_virt[0];
        if (got == GT_SELFTEST_MAGIC) { settle = i; break; }
        for (volatile int j = 0; j < 50; j++) {}
    }
    kprintf("[intel_gpu] GT selftest: ring drained=%d scratch=0x%08x "
            "(want 0x%08x) settle_iters=%d\n",
            drained, got, GT_SELFTEST_MAGIC, settle);
    return drained && got == GT_SELFTEST_MAGIC;
}

void intel_gt_init(void) {
    memset(&g_gt, 0, sizeof(g_gt));

    /* Reuse the probe + MMIO mapping. (intel_gpu_gt_recon may already have
     * run and mapped it; re-probing is cheap and idempotent.) */
    if (!g_igpu.mmio) {
        if (!igpu_probe_pci()) {
            kprintf("[intel_gpu] GT init: no supported Intel GPU\n");
            return;
        }
        if (igpu_map_mmio() < 0) {
            kprintf("[intel_gpu] GT init: MMIO map failed\n");
            return;
        }
    }

    /* Gen7 (Ivy Bridge / Haswell, e.g. the EliteDesk HD 4600) path: 4-byte
     * GGTT PTEs at GTT_BASE (2 MiB into the BAR), a single multi-threaded
     * forcewake, and the 32-bit-address store command. Shares gt_ring_init /
     * gt_run_selftest with gen8/9 (both are gen-dispatched). SAFETY is
     * identical -- only forcewake/GGTT/ring/our pages, never pipe/plane/DPLL/
     * VGA -- so a broken blitter leaves the Limine display untouched. */
    if (g_igpu.gen == 7) {
        size_t need = GTT_BASE + (64u << 10);   /* base + 64 KiB of PTEs */
        if (g_igpu.mmio_size < need) {
            kprintf("[intel_gpu] GT init (gen7): BAR0 size 0x%zx too small for "
                    "GGTT (need >= 0x%zx) -- staying on Limine\n",
                    g_igpu.mmio_size, need);
            return;
        }
        g_gt.ggtt_mmio = (uint64_t)g_igpu.mmio + GTT_BASE;

        int fw = gt_forcewake_get(FORCEWAKE_MT_HSW, FORCEWAKE_ACK_HSW);
        g_gt.fw_render = g_gt.fw_blitter = fw;
        kprintf("[intel_gpu] GT init (gen7): forcewake(MT)=%d\n", fw);
        if (!fw) {
            kprintf("[intel_gpu] GT init (gen7): forcewake failed -- aborting "
                    "GT accel (display unaffected)\n");
            return;
        }

        if (!gt_ring_init()) {
            kprintf("[intel_gpu] GT init (gen7): BCS ring setup failed\n");
            gt_forcewake_put(FORCEWAKE_MT_HSW);
            return;
        }
        kprintf("[intel_gpu] GT init (gen7): BCS ring up (phys=0x%llx "
                "gtt_off=0x%x ctl=0x%08x)\n",
                (unsigned long long)g_gt.ring_phys, g_gt.ring_ggtt_off,
                igpu_read(GEN8_BCS_RING_BASE + RING_CTL));

        g_gt.selftest_ok = gt_run_selftest();
        kprintf("[intel_gpu] GT init (gen7): self-test %s\n",
                g_gt.selftest_ok ? "PASS -- blitter accel available"
                                 : "FAIL -- staying on Limine compositor");
        return;
    }

    /* Only gen7 (above) + gen8/gen9 GGTT/forcewake layouts are implemented. */
    if (g_igpu.gen < 8) {
        kprintf("[intel_gpu] GT init: gen %d < 8, GT accel unsupported "
                "(display stays on Limine)\n", g_igpu.gen);
        return;
    }

    /* GGTT lives in the top half of the (>=16 MiB) BAR. Guard the size so a
     * small/odd BAR can't make us write outside the mapping. We touch the
     * GGTT from GEN8_GGTT_OFFSET (8 MiB) up to the highest PTE we install
     * (scratch page at GGTT VA 0x41000 -> PTE byte offset 0x41*8); require
     * the mapping to cover at least the first 64 KiB of PTEs past the GGTT
     * base. A correct gen9 GTTMMADR is 16 MiB, so this passes once the BAR
     * sizing isn't clamped (the 4 MiB clamp was the Stage-2 EliteDesk
     * abort: "BAR0 size 0x400000 too small for GGTT"). */
    size_t ggtt_need = GEN8_GGTT_OFFSET + (64u << 10);
    if (g_igpu.mmio_size < ggtt_need) {
        kprintf("[intel_gpu] GT init: BAR0 size 0x%zx too small for GGTT "
                "(need >= 0x%zx) -- staying on Limine\n",
                g_igpu.mmio_size, ggtt_need);
        return;
    }
    g_gt.ggtt_mmio = (uint64_t)g_igpu.mmio + GEN8_GGTT_OFFSET;

    /* Forcewake render + blitter so the ring registers are live. */
    g_gt.fw_render  = gt_forcewake_get(FORCEWAKE_RENDER_GEN9,
                                       FORCEWAKE_RENDER_ACK);
    g_gt.fw_blitter = gt_forcewake_get(FORCEWAKE_BLITTER_GEN9,
                                       FORCEWAKE_BLITTER_ACK);
    kprintf("[intel_gpu] GT init: forcewake render=%d blitter=%d\n",
            g_gt.fw_render, g_gt.fw_blitter);
    if (!g_gt.fw_blitter) {
        kprintf("[intel_gpu] GT init: blitter forcewake failed -- "
                "aborting GT accel (display unaffected)\n");
        return;
    }

    if (!gt_ring_init()) {
        kprintf("[intel_gpu] GT init: BCS ring setup failed\n");
        gt_forcewake_put(FORCEWAKE_RENDER_GEN9);
        gt_forcewake_put(FORCEWAKE_BLITTER_GEN9);
        return;
    }
    kprintf("[intel_gpu] GT init: BCS ring up (phys=0x%llx gtt_off=0x%x "
            "ctl=0x%08x)\n",
            (unsigned long long)g_gt.ring_phys, g_gt.ring_ggtt_off,
            igpu_read(GEN8_BCS_RING_BASE + RING_CTL));

    g_gt.selftest_ok = gt_run_selftest();
    kprintf("[intel_gpu] GT init: self-test %s\n",
            g_gt.selftest_ok ? "PASS -- blitter accel available"
                             : "FAIL -- staying on Limine compositor");

    /* Keep forcewake held while the GT is in use (Stage 3 will rely on it).
     * If the self-test failed we could drop it, but holding it is harmless
     * and simpler; the GT is otherwise idle. */
}

/* ======================================================================
 * Stage 3: GPU-blitter present path (XY_SRC_COPY_BLT: back buffer -> scanout)
 *
 * With the Stage-2 GT self-test passed, offload the compositor's present
 * copy from the CPU (memcpy back-buffer -> Limine framebuffer) to the BCS
 * blitter. Each present is one XY_SRC_COPY_BLT of the dirty rect from the
 * CPU back buffer straight into the LIVE scanout surface -- addressed by the
 * display plane's OWN GGTT address (PLANE_A_SURF), so we write exactly the
 * pixels the pipe is scanning. We NEVER program pipe/plane/DPLL/VGA, so a
 * broken blit can at worst leave the screen stale (and the drain watchdog
 * then reverts to the CPU memcpy) -- it can't blackscreen the box.
 *
 * SAFETY / VALIDATION: before arming, intel_gt_present_setup() runs a
 * scratch-buffer blit self-test (pattern src -> scratch dst, read back and
 * compare) that lives near the proven ring/scratch GGTT region and touches
 * NO display register. Only a passing self-test arms the real path -- the
 * gen7 analog of the Stage-2 store self-test. Gen7 (Haswell) only for now;
 * QEMU no-op (intel_gt_selftest_ok()==0). See igpu-i915lite memory.
 * ====================================================================== */

/* Blit-self-test GGTT VAs, adjacent to the proven Stage-2 ring (0x40000) /
 * scratch (0x41000) -- known-safe (Stage 2 wrote PTEs there without
 * disturbing the display). */
#define GT_BLTST_SRC_OFF   0x50000u
#define GT_BLTST_DST_OFF   0x60000u
/* Bounded ring-drain budget for the real present path; only ever reached on
 * a genuine engine hang (a healthy blit drains in ~1 iteration). */
#define GT_PRESENT_DRAIN_MAX 500000

static struct {
    int      ready;             /* present path validated + armed        */
    int      disabled;          /* a ring-drain timeout retired it       */
    uint32_t dst_ggtt_off;      /* live scanout GT VA (PLANE_A_SURF)      */
    uint32_t dst_pitch;         /* scanout stride in BYTES (gen7)        */
    uint32_t ring_tail;         /* our tracked BCS ring write offset     */
    uint32_t next_src_ggtt_off; /* bump allocator for source GT VAs      */
    /* GGTT-map cache for the (up to 2, double-buffered) CPU back buffers.
     * Each is mapped once; va0 is the GT VA of the buffer's pixel (0,0)
     * (== mapped page base + the buffer's sub-page byte offset). */
    struct { const uint32_t *base; uint32_t va0; } src[2];
    int      src_count;
} g_present;

static inline uint32_t gt_align_up_u32(uint32_t x, uint32_t a) {
    return (x + (a - 1u)) & ~(a - 1u);
}

/* Flush a CPU (write-back) range to DRAM so the blitter's uncached GGTT
 * read sees it (gen7 GGTT PTEs are cache-level-0). Rounded to cache lines. */
static void gt_clflush_range(const void *p, size_t bytes) {
    if (!bytes) return;
    const uint8_t *a = (const uint8_t *)((uintptr_t)p & ~63ull);
    const uint8_t *e = (const uint8_t *)p + bytes;
    for (; a < e; a += 64)
        __asm__ __volatile__("clflush (%0)" :: "r"(a) : "memory");
    __asm__ __volatile__("mfence" ::: "memory");
}

/* Append `n` dwords to the BCS ring, handling wrap, then publish RING_TAIL.
 * The ring buffer is CPU write-back but the engine fetches it uncached via
 * the GGTT, so clflush the bytes we wrote before advancing the tail. */
static void gt_ring_emit(const uint32_t *dw, int n) {
    uint32_t size  = g_gt.ring_pages * 4096u;
    uint32_t bytes = (uint32_t)n * 4u;
    if (g_present.ring_tail + bytes + 8u > size) {
        /* Pad the tail of the ring with NOOPs and wrap; the engine executes
         * HEAD..end (NOOPs) then 0..new-tail. */
        while (g_present.ring_tail < size) {
            g_gt.ring_virt[g_present.ring_tail / 4] = MI_NOOP;
            g_present.ring_tail += 4;
        }
        gt_clflush_range(g_gt.ring_virt, size);
        g_present.ring_tail = 0;
    }
    uint32_t start = g_present.ring_tail;
    for (int i = 0; i < n; i++)
        g_gt.ring_virt[start / 4 + (uint32_t)i] = dw[i];
    g_present.ring_tail = start + bytes;
    if (g_present.ring_tail & 7u) {            /* qword-align the tail */
        g_gt.ring_virt[g_present.ring_tail / 4] = MI_NOOP;
        g_present.ring_tail += 4;
    }
    if (g_present.ring_tail >= size) g_present.ring_tail = 0;
    gt_clflush_range(&g_gt.ring_virt[start / 4],
                     (size_t)(g_present.ring_tail >= start
                              ? g_present.ring_tail - start
                              : size - start));
    igpu_write(GEN8_BCS_RING_BASE + RING_TAIL,
               g_present.ring_tail & RING_TAIL_MASK);
    (void)igpu_read(GEN8_BCS_RING_BASE + RING_TAIL);
}

/* Spin (bounded) until the engine's HEAD reaches our TAIL == it parsed our
 * command stream. Returns 1 on drain, 0 on timeout. */
static int gt_ring_drain(int max_iters) {
    uint32_t want = g_present.ring_tail & RING_HEAD_MASK;
    for (int i = 0; i < max_iters; i++) {
        uint32_t head = igpu_read(GEN8_BCS_RING_BASE + RING_HEAD)
                        & RING_HEAD_MASK;
        if (head == want) return 1;
        for (volatile int j = 0; j < 50; j++) {}
    }
    return 0;
}

/* Emit one XY_SRC_COPY_BLT (gen2-7 8-dword form) copying a wxh rect from
 * src(sx,sy) to dst(dx,dy). Addresses are GGTT byte offsets (GT VAs);
 * pitches are BYTES and must fit the signed-16-bit BR11/BR13 field.
 * Returns 0 on success, -1 if a parameter is out of range. */
static int gt_emit_blit(uint32_t dst_base, uint32_t dst_pitch, int dx, int dy,
                        uint32_t src_base, uint32_t src_pitch, int sx, int sy,
                        int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    if (dst_pitch > 0x7FFFu || src_pitch > 0x7FFFu) return -1;
    if ((dx + w) > 0xFFFF || (dy + h) > 0xFFFF) return -1;
    uint32_t cmd[8];
    cmd[0] = XY_SRC_COPY_BLT_CMD | BLT_WRITE_ALPHA | BLT_WRITE_RGB;
    cmd[1] = BLT_DEPTH_32 | BLT_ROP_SRCCOPY | (dst_pitch & 0xFFFFu); /* BR13 */
    cmd[2] = ((uint32_t)dy << 16) | (uint32_t)(dx & 0xFFFF);         /* BR22 dst TL */
    cmd[3] = ((uint32_t)(dy + h) << 16) | (uint32_t)((dx + w) & 0xFFFF); /* BR23 dst BR (excl) */
    cmd[4] = dst_base;                                               /* BR09 dst addr */
    cmd[5] = ((uint32_t)sy << 16) | (uint32_t)(sx & 0xFFFF);         /* BR26 src TL */
    cmd[6] = src_pitch & 0xFFFFu;                                    /* BR11 src pitch */
    cmd[7] = src_base;                                               /* BR12 src addr */
    gt_ring_emit(cmd, 8);
    return 0;
}

/* GGTT-map a CPU back buffer (once) and return the GT VA of its pixel (0,0).
 * The buffer is kmalloc'd (16-byte aligned, NOT page aligned, and not
 * guaranteed physically contiguous), so we translate each 4 KiB page and
 * carry the sub-page byte offset into the returned GT VA. Cached by base
 * pointer (there are only ever the 1-2 double-buffered surfaces). Returns 0
 * on failure (unmapped page / cache full). */
static uint32_t gt_src_map(const uint32_t *back, uint32_t w, uint32_t h) {
    for (int i = 0; i < g_present.src_count; i++)
        if (g_present.src[i].base == back) return g_present.src[i].va0;
    if (g_present.src_count >= (int)(sizeof(g_present.src) /
                                     sizeof(g_present.src[0])))
        return 0;

    uintptr_t b        = (uintptr_t)back;
    uint32_t  page_off = (uint32_t)(b & 0xFFFu);
    uint64_t  first    = b & ~0xFFFull;
    size_t    total    = (size_t)page_off + (size_t)w * h * 4u;
    uint32_t  pages    = (uint32_t)((total + 0xFFFu) / 4096u);
    uint32_t  ggtt     = g_present.next_src_ggtt_off;

    for (uint32_t i = 0; i < pages; i++) {
        uint64_t phys = vmm_translate(first + (uint64_t)i * 4096u);
        if (!phys) {
            kprintf("[intel_gpu] Stage3: back buffer %p page %u unmapped -- "
                    "GT present unavailable\n", (const void *)back, i);
            return 0;
        }
        gt_ggtt_map(ggtt + i * 4096u, phys & ~0xFFFull);
    }
    /* Reserve this buffer's GT VA range plus a 1 MiB gap before the next. */
    g_present.next_src_ggtt_off =
        ggtt + gt_align_up_u32(pages * 4096u + 0x100000u, 0x100000u);

    int idx = g_present.src_count++;
    g_present.src[idx].base = back;
    g_present.src[idx].va0  = ggtt + page_off;
    kprintf("[intel_gpu] Stage3: mapped back buffer %p -> GT VA 0x%x "
            "(%u pages, sub-page off 0x%x)\n",
            (const void *)back, ggtt + page_off, pages, page_off);
    return ggtt + page_off;
}

/* Scratch-buffer blit self-test: CPU-fills a source with a per-pixel pattern
 * (at the SAME sub-page offset the real back buffer will have, so we prove
 * that alignment too), blits it to a scratch dest with a DIFFERENT pitch,
 * then reads the dest back and compares. Touches only scratch pages + the
 * GT ring -- never the display. Returns 1 on a byte-exact copy. */
static int gt_blit_selftest(void) {
    enum { TW = 48, TH = 8 };
    uint32_t src_pitch = TW * 4u;               /* tight source rows        */
    uint32_t dst_pitch = (TW + 16u) * 4u;       /* padded dest (distinct)   */

    uint64_t sp = pmm_alloc_pages(2);
    uint64_t dp = pmm_alloc_pages(2);
    if (!sp || !dp) {
        kprintf("[intel_gpu] Stage3: blit self-test OOM\n");
        return 0;
    }
    uint32_t *sv = (uint32_t *)pmm_phys_to_virt(sp);
    uint32_t *dv = (uint32_t *)pmm_phys_to_virt(dp);

    /* Mimic the real back buffer's sub-page (dword) alignment. */
    uint32_t *bb = gfx_backbuf();
    uint32_t page_off = bb ? ((uint32_t)((uintptr_t)bb & 0xFFFu) & ~3u) : 0u;
    uint32_t *src_px = (uint32_t *)((uint8_t *)sv + page_off);

    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++)
            src_px[y * TW + x] = 0xC0DE0000u ^ (uint32_t)((y << 8) | x);
    memset(dv, 0, 2u * 4096u);

    gt_ggtt_map(GT_BLTST_SRC_OFF,          sp);
    gt_ggtt_map(GT_BLTST_SRC_OFF + 0x1000, sp + 4096);
    gt_ggtt_map(GT_BLTST_DST_OFF,          dp);
    gt_ggtt_map(GT_BLTST_DST_OFF + 0x1000, dp + 4096);

    gt_clflush_range(sv, 2u * 4096u);

    if (gt_emit_blit(GT_BLTST_DST_OFF, dst_pitch, 0, 0,
                     GT_BLTST_SRC_OFF + page_off, src_pitch, 0, 0,
                     TW, TH) < 0) {
        kprintf("[intel_gpu] Stage3: blit self-test emit rejected\n");
        return 0;
    }
    int drained = gt_ring_drain(GT_PRESENT_DRAIN_MAX);

    /* The blit's writes complete ASYNCHRONOUSLY: ring drain means the engine
     * parsed the command, but the pixels reach DRAM via the GPU write pipeline
     * a little later (the same async-store effect the Stage-2 store self-test
     * hit -- pass one boot, fail the next). So poll the LAST-written pixel
     * (writes retire in order, so once it lands the rest have too) before the
     * full compare, flushing its cache line each iteration. */
    volatile uint32_t *last = (volatile uint32_t *)
        ((uint8_t *)dv + (TH - 1) * dst_pitch + (TW - 1) * 4);
    uint32_t last_want = 0xC0DE0000u ^ (uint32_t)(((TH - 1) << 8) | (TW - 1));
    int settle = -1;
    for (int i = 0; i < 100000; i++) {
        gt_clflush_range((const void *)last, 4);
        if (*last == last_want) { settle = i; break; }
        for (volatile int j = 0; j < 50; j++) {}
    }

    gt_clflush_range(dv, 2u * 4096u);
    int ok = 1;
    uint32_t bad_x = 0, bad_y = 0, bad_got = 0, bad_want = 0;
    for (int y = 0; y < TH && ok; y++) {
        for (int x = 0; x < TW; x++) {
            uint32_t want = 0xC0DE0000u ^ (uint32_t)((y << 8) | x);
            uint32_t got  = *(uint32_t *)((uint8_t *)dv + y * dst_pitch + x * 4);
            if (got != want) {
                ok = 0; bad_x = x; bad_y = y; bad_got = got; bad_want = want;
                break;
            }
        }
    }
    kprintf("[intel_gpu] Stage3: blit self-test drained=%d settle_iters=%d "
            "copy=%s", drained, settle, (drained && ok) ? "OK" : "MISMATCH");
    if (!(drained && ok))
        kprintf(" (first bad @%u,%u got=0x%08x want=0x%08x)",
                bad_x, bad_y, bad_got, bad_want);
    kprintf("\n");
    /* Leave the scratch pages allocated + GGTT-mapped: the real present path
     * never references these GT VAs again, and not freeing avoids any chance
     * of a reused physical page sitting under a stale PTE. 16 KiB, one-time. */
    return drained && ok;
}

int intel_gt_present_setup(void) {
    if (!g_gt.selftest_ok) {
        kprintf("[intel_gpu] Stage3: GT self-test did not pass -- present "
                "path unavailable (staying on Limine)\n");
        return 0;
    }
    if (!g_igpu.mmio) return 0;
    if (g_igpu.gen != 7) {
        /* Only gen7 (Haswell EliteDesk) is validated: PLANE_A_STRIDE reads in
         * bytes here, whereas gen9 uses 64-byte units. Bail rather than blit
         * with a wrong pitch. */
        kprintf("[intel_gpu] Stage3: gen %d present path not implemented "
                "(gen7 only) -- staying on Limine\n", g_igpu.gen);
        return 0;
    }
    if (!gfx_ready()) {
        kprintf("[intel_gpu] Stage3: gfx layer not ready -- staying on Limine\n");
        return 0;
    }

    uint32_t ctrl = igpu_read(PLANE_A_CTRL);
    uint32_t surf = igpu_read(PLANE_A_SURF);
    uint32_t strd = igpu_read(PLANE_A_STRIDE);   /* gen7: bytes/line */
    kprintf("[intel_gpu] Stage3: scanout PLANE_A ctrl=0x%08x (en=%d) "
            "surf(GTVA)=0x%08x stride=%u bytes\n",
            ctrl, (ctrl >> 31) & 1u, surf, strd);
    if (!((ctrl >> 31) & 1u)) {
        kprintf("[intel_gpu] Stage3: primary plane disabled -- abort\n");
        return 0;
    }
    uint32_t need_pitch = gfx_width() * 4u;
    if (strd < need_pitch || strd > 0x7FFFu) {
        kprintf("[intel_gpu] Stage3: scanout stride %u out of range "
                "(need >=%u, <=0x7FFF) -- abort\n", strd, need_pitch);
        return 0;
    }

    g_present.dst_ggtt_off = surf & ~0xFFFu;
    g_present.dst_pitch    = strd;
    /* Continue the BCS ring where Stage 2's self-test left HEAD==TAIL. */
    g_present.ring_tail =
        igpu_read(GEN8_BCS_RING_BASE + RING_TAIL) & RING_TAIL_MASK;

    /* Source back buffers get GT VAs at 1 GiB into the (2 GiB) GGTT -- well
     * clear of the low-aperture scanout. Shift above the scanout on the
     * (unexpected) chance it maps into that window. */
    uint32_t src_span = 0x02000000u;                       /* reserve 32 MiB */
    uint32_t base     = 0x40000000u;                       /* 1 GiB          */
    uint32_t so = g_present.dst_ggtt_off;
    uint32_t se = so + gt_align_up_u32(strd * gfx_height(), 0x100000u);
    if (!(base >= se || base + src_span <= so))
        base = gt_align_up_u32(se, 0x100000u);
    g_present.next_src_ggtt_off = base;

    if (!gt_blit_selftest()) {
        kprintf("[intel_gpu] Stage3: blit self-test FAILED -- staying on "
                "Limine CPU present\n");
        return 0;
    }

    g_present.ready    = 1;
    g_present.disabled = 0;
    kprintf("[intel_gpu] Stage3: GT present ARMED (dst GTVA=0x%x pitch=%u, "
            "src base GTVA=0x%x)\n",
            g_present.dst_ggtt_off, g_present.dst_pitch,
            g_present.next_src_ggtt_off);
    return 1;
}

int intel_gt_present_rect(const uint32_t *back, uint32_t back_w, uint32_t back_h,
                          int x, int y, int w, int h) {
    if (!g_present.ready || g_present.disabled) return -1;
    if (!back || back_w == 0 || back_h == 0)    return -1;

    /* Clamp the rect to the buffer (the gfx layer already clips, but be
     * defensive -- a bad coord would blit garbage into scanout). */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)back_w) w = (int)back_w - x;
    if (y + h > (int)back_h) h = (int)back_h - y;
    if (w <= 0 || h <= 0) return 0;             /* nothing to do == success */

    uint32_t src_base = gt_src_map(back, back_w, back_h);
    if (!src_base) return -1;

    uint32_t src_pitch = back_w * 4u;
    /* Flush the dirty rows to DRAM so the blitter's uncached read is fresh. */
    for (int r = 0; r < h; r++)
        gt_clflush_range((const uint8_t *)back
                         + (size_t)(y + r) * src_pitch + (size_t)x * 4u,
                         (size_t)w * 4u);

    if (gt_emit_blit(g_present.dst_ggtt_off, g_present.dst_pitch, x, y,
                     src_base, src_pitch, x, y, w, h) < 0)
        return -1;

    if (!gt_ring_drain(GT_PRESENT_DRAIN_MAX)) {
        g_present.disabled = 1;
        kprintf("[intel_gpu] Stage3: ring-drain TIMEOUT -- retiring GT "
                "present, reverting to Limine CPU copy\n");
        return -1;
    }
    return 0;
}

/* ======================================================================
 * Stage 4: tear-free hardware page-flip (double-buffered PLANE_A_SURF)
 *
 * Stage 3 blits into the LIVE scanout (tears, like the CPU memcpy did).
 * Stage 4 gives the display its OWN pair of buffers and page-flips between
 * them: each frame we blit only the DIRTY region into the OFF-screen buffer
 * (never the one being scanned), then retarget PLANE_A_SURF at it -- the
 * hardware latches the new surface at vblank, so the swap is tear-free. We
 * touch ONLY the plane's surface-address register (double-buffered, latched
 * at vblank) -- never the pipe/DPLL/timings -- so it cannot re-modeset or
 * blackscreen.
 *
 * PERF: (1) dirty-rect blit -- because each buffer is two flips stale when we
 * return to it, we blit the union of the last two frames' damage, not the
 * whole frame (the first two flips prime both buffers full). (2) pipelined
 * flip -- we arm the flip and return WITHOUT busy-waiting for vblank; the
 * next present waits for the previous flip to latch (bounded) before it
 * touches the buffer that flip moved away from. So the compositor isn't
 * blocked ~16 ms per frame, and window drags blit only the damaged pixels.
 *
 * SAFETY: the flip is armed only after a self-test proves the mechanism --
 * flip the scanout to a scratch buffer, confirm PLANE_A_SURFLIVE tracks it,
 * then RESTORE the original Limine FB and confirm SURFLIVE tracks back. If
 * either doesn't latch (the gen9 attempt hung here on an UNBOUNDED vblank
 * wait -- ours is bounded by perf_now_ns), we stay on the Stage-3 blit path.
 * A runtime flip that fails to latch trips a watchdog that restores the
 * Limine FB and reverts to the CPU present. Gen7 only; QEMU no-op.
 * ====================================================================== */

/* Dedicated scanout buffers live high in the GGTT, clear of the firmware
 * scanout (offset 0, ~8 MiB), the ring/scratch (0x40000..0x60000), and the
 * Stage-3 source cache (1 GiB+). */
#define GT_FLIP_BUF0_OFF   0x10000000u   /* 256 MiB into the GGTT */
#define GT_FLIP_BUF1_OFF   0x18000000u   /* 384 MiB into the GGTT */
/* Bounded vblank/flip-latch wait (real time, not spins -- the gen9 hang was
 * an unbounded spin). A flip latches within one refresh (~16 ms @ 60 Hz). */
#define GT_FLIP_LATCH_NS   80000000ull   /* 80 ms */

static struct {
    int      ready;             /* page-flip present armed                */
    int      disabled;          /* a flip watchdog retired it            */
    int      selftest_ok;       /* SURFLIVE tracked a test flip + restore*/
    uint32_t orig_surf;         /* the Limine FB's GGTT offset (restore) */
    int      front;             /* index scanned now: -1=orig FB, 0/1=buf*/
    uint32_t width, height, pitch;   /* scanout geometry (pitch in bytes)*/
    uint32_t size_pages;        /* per-buffer size in 4 KiB pages        */
    struct { uint64_t phys; uint32_t *virt; uint32_t ggtt; } buf[2];
    uint64_t flip_count;
    /* Previous present's dirty rect. Each buffer is 2 flips stale when we
     * come back to it, so a dirty-rect present blits the union of the last
     * two frames' damage (dirty_{N} u dirty_{N+1}) to bring it current. */
    int      prev_dx, prev_dy, prev_dw, prev_dh;
} g_flip;

/* 12x19 arrow, drawn into the off-screen buffer each present (the scanout is
 * no longer the Limine FB the software overlay pokes). Mirrors gfx.c's. */
#define GT_CUR_W 12
#define GT_CUR_H 19
static const char g_flip_cursor[GT_CUR_H][GT_CUR_W + 1] = {
    "#           ", "##          ", "#.#         ", "#..#        ",
    "#...#       ", "#....#      ", "#.....#     ", "#......#    ",
    "#.......#   ", "#........#  ", "#.........# ", "#......#####",
    "#...##.#    ", "#..# #.#    ", "#.#  #.#    ", "##    #.#   ",
    "      #.#   ", "       #.#  ", "       ###  ",
};

/* Poll PLANE_A_SURFLIVE (the address the pipe is CURRENTLY scanning) until it
 * equals `want` (page-granular) or `timeout_ns` elapses. Bounded by real time
 * so a plane that never latches can't hang the box. Returns 1 on latch. */
static int gt_wait_surflive(uint32_t want, uint64_t timeout_ns,
                            uint64_t *elapsed_ns) {
    uint64_t t0 = perf_now_ns();
    want &= ~0xFFFu;
    for (;;) {
        uint32_t live = igpu_read(PLANE_A_SURFLIVE) & ~0xFFFu;
        uint64_t dt = perf_now_ns() - t0;
        if (live == want)      { if (elapsed_ns) *elapsed_ns = dt; return 1; }
        if (dt > timeout_ns)   { if (elapsed_ns) *elapsed_ns = dt; return 0; }
    }
}

/* Draw the arrow opaquely into a linear XRGB buffer (no shadow/blend -- a
 * blend would have to read the buffer the GT just wrote asynchronously), then
 * flush the touched rows so the (uncached) scanout sees the cursor. */
static void gt_flip_draw_cursor(uint32_t *buf, uint32_t w, uint32_t h,
                                int cx, int cy) {
    for (int dy = 0; dy < GT_CUR_H; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= (int)h) continue;
        const char *row = g_flip_cursor[dy];
        int minx = -1, maxx = -1;
        for (int dx = 0; dx < GT_CUR_W && row[dx]; dx++) {
            int px = cx + dx;
            if (px < 0 || px >= (int)w) continue;
            char c = row[dx];
            if      (c == '#') buf[(uint32_t)py * w + (uint32_t)px] = 0x00000000u;
            else if (c == '.') buf[(uint32_t)py * w + (uint32_t)px] = 0x00FFFFFFu;
            else continue;
            if (minx < 0) minx = px;
            maxx = px;
        }
        if (minx >= 0)
            gt_clflush_range(&buf[(uint32_t)py * w + (uint32_t)minx],
                             (size_t)(maxx - minx + 1) * 4u);
    }
}

/* ---- Hardware cursor plane -------------------------------------------
 * The display composites a 64x64 ARGB cursor over the primary plane at
 * scanout, independent of the page-flip -- so moving the pointer is a single
 * CUR_A_POS register write, NOT a compositor flip. This is what makes the
 * mouse smooth under page-flipping (the software overlay pokes the scanout
 * FB, which the page-flip no longer scans). Separate plane from the pipe/
 * DPLL, so a wrong cursor can't blackscreen -- worst case it doesn't show
 * (and we fall back to compositing the cursor into the flipped frame). */
#define GT_HWCUR_OFF   0x20000000u   /* cursor BO GGTT offset (512 MiB in) */
static struct {
    uint64_t phys; uint32_t *virt; uint32_t ggtt; int ready;
} g_hwcur;

static int intel_gt_cursor_setup(void) {
    g_hwcur.phys = pmm_alloc_pages(4);              /* 64x64x4 = 16 KiB */
    if (!g_hwcur.phys) return 0;
    g_hwcur.virt = (uint32_t *)pmm_phys_to_virt(g_hwcur.phys);
    g_hwcur.ggtt = GT_HWCUR_OFF;
    memset(g_hwcur.virt, 0, 4u * 4096u);            /* fully transparent */

    /* Rasterise the arrow into the top-left of the 64x64 ARGB image
     * (0xAARRGGBB): opaque black outline, opaque white fill, else clear. */
    for (int dy = 0; dy < GT_CUR_H; dy++) {
        const char *row = g_flip_cursor[dy];
        for (int dx = 0; dx < GT_CUR_W && row[dx]; dx++) {
            if      (row[dx] == '#') g_hwcur.virt[dy * 64 + dx] = 0xFF000000u;
            else if (row[dx] == '.') g_hwcur.virt[dy * 64 + dx] = 0xFFFFFFFFu;
        }
    }
    gt_clflush_range(g_hwcur.virt, 4u * 4096u);
    for (uint32_t p = 0; p < 4; p++)
        gt_ggtt_map(g_hwcur.ggtt + p * 4096u, g_hwcur.phys + (uint64_t)p * 4096u);

    /* Program pipe-A's cursor plane: 64x64 ARGB, base = GGTT offset. */
    igpu_write(CUR_A_CTL, CUR_MODE_64_ARGB);
    igpu_write(CUR_A_BASE, g_hwcur.ggtt);           /* base write latches it */
    (void)igpu_read(CUR_A_BASE);
    igpu_write(CUR_A_POS, 0);
    (void)igpu_read(CUR_A_POS);

    kprintf("[intel_gpu] Stage4: HW cursor plane enabled (ggtt=0x%x "
            "ctl=0x%08x base=0x%08x)\n", g_hwcur.ggtt,
            igpu_read(CUR_A_CTL), igpu_read(CUR_A_BASE));
    g_hwcur.ready = 1;
    return 1;
}

int intel_gt_cursor_active(void) { return g_hwcur.ready; }

void intel_gt_cursor_move(int x, int y) {
    if (!g_hwcur.ready) return;
    uint32_t pos = 0;
    if (x < 0) pos |= (1u << 15) | ((uint32_t)(-x) & 0x7FFFu);
    else       pos |=              ((uint32_t)x  & 0x7FFFu);
    if (y < 0) pos |= (1u << 31) | (((uint32_t)(-y) & 0x7FFFu) << 16);
    else       pos |=              (((uint32_t)y  & 0x7FFFu) << 16);
    igpu_write(CUR_A_POS, pos);
}

int intel_gt_flip_setup(void) {
    if (!g_gt.selftest_ok || !g_igpu.mmio) return 0;
    if (g_igpu.gen != 7) {
        kprintf("[intel_gpu] Stage4: gen %d page-flip not implemented "
                "(gen7 only)\n", g_igpu.gen);
        return 0;
    }
    if (!gfx_ready()) return 0;

    memset(&g_flip, 0, sizeof(g_flip));
    g_flip.front     = -1;                       /* still on the Limine FB */
    g_flip.orig_surf = igpu_read(PLANE_A_SURF) & ~0xFFFu;
    g_flip.pitch     = igpu_read(PLANE_A_STRIDE);
    g_flip.width     = gfx_width();
    g_flip.height    = gfx_height();
    if (g_flip.pitch < g_flip.width * 4u || g_flip.pitch > 0x7FFFu) {
        kprintf("[intel_gpu] Stage4: scanout stride %u out of range -- "
                "no page-flip\n", g_flip.pitch);
        return 0;
    }
    g_flip.size_pages = (g_flip.pitch * g_flip.height + 0xFFFu) / 4096u;

    uint32_t bases[2] = { GT_FLIP_BUF0_OFF, GT_FLIP_BUF1_OFF };
    for (int i = 0; i < 2; i++) {
        g_flip.buf[i].phys = pmm_alloc_pages(g_flip.size_pages);
        if (!g_flip.buf[i].phys) {
            kprintf("[intel_gpu] Stage4: OOM allocating scanout buffer %d "
                    "(%u pages) -- no page-flip\n", i, g_flip.size_pages);
            return 0;
        }
        g_flip.buf[i].virt = (uint32_t *)pmm_phys_to_virt(g_flip.buf[i].phys);
        g_flip.buf[i].ggtt = bases[i];
        for (uint32_t p = 0; p < g_flip.size_pages; p++)
            gt_ggtt_map(g_flip.buf[i].ggtt + p * 4096u,
                        g_flip.buf[i].phys + (uint64_t)p * 4096u);
        memset(g_flip.buf[i].virt, 0, (size_t)g_flip.size_pages * 4096u);
        gt_clflush_range(g_flip.buf[i].virt, (size_t)g_flip.size_pages * 4096u);
    }

    /* --- Flip self-test: flip to buf[0], confirm SURFLIVE tracks, restore.
     * buf[0] is black; content is irrelevant -- we only test that writing
     * PLANE_A_SURF actually moves the scanout and that SURFLIVE reports it.
     * Always restore to the original FB so the boot console stays visible. */
    uint32_t live0 = igpu_read(PLANE_A_SURFLIVE);
    uint64_t t_flip = 0, t_restore = 0;

    igpu_write(PLANE_A_SURF, g_flip.buf[0].ggtt);
    (void)igpu_read(PLANE_A_SURF);
    int flipped = gt_wait_surflive(g_flip.buf[0].ggtt, GT_FLIP_LATCH_NS, &t_flip);

    igpu_write(PLANE_A_SURF, g_flip.orig_surf);
    (void)igpu_read(PLANE_A_SURF);
    int restored = gt_wait_surflive(g_flip.orig_surf, GT_FLIP_LATCH_NS, &t_restore);

    kprintf("[intel_gpu] Stage4: flip self-test surflive0=0x%08x  "
            "flip->buf0 latched=%d (%llu us)  restore->orig latched=%d "
            "(%llu us)\n",
            live0, flipped, (unsigned long long)(t_flip / 1000),
            restored, (unsigned long long)(t_restore / 1000));

    if (!flipped || !restored) {
        /* Make certain we're back on the original FB, then decline. */
        igpu_write(PLANE_A_SURF, g_flip.orig_surf);
        (void)igpu_read(PLANE_A_SURF);
        kprintf("[intel_gpu] Stage4: flip self-test FAILED -- staying on "
                "Stage-3 blit present\n");
        return 0;
    }

    g_flip.selftest_ok = 1;
    g_flip.ready       = 1;
    kprintf("[intel_gpu] Stage4: page-flip ARMED (bufs GTVA 0x%x/0x%x, "
            "orig 0x%x, %ux%u pitch=%u)\n",
            g_flip.buf[0].ggtt, g_flip.buf[1].ggtt, g_flip.orig_surf,
            g_flip.width, g_flip.height, g_flip.pitch);

    /* Bring up the hardware cursor plane so pointer motion is a register
     * poke instead of a full flip. Best-effort: if it fails we composite the
     * cursor into the flipped frame instead (slower, but visible). */
    if (!intel_gt_cursor_setup())
        kprintf("[intel_gpu] Stage4: HW cursor setup failed -- compositing "
                "cursor into the frame (pointer motion will flip)\n");
    return 1;
}

/* Restore the scanout to the Limine FB and disable the page-flip path (called
 * when a flip fails to latch). Because a failed flip never left the current
 * front, the CPU limine present then targets the correct live surface. */
static void gt_flip_revert(void) {
    igpu_write(PLANE_A_SURF, g_flip.orig_surf);
    (void)igpu_read(PLANE_A_SURF);
    gt_wait_surflive(g_flip.orig_surf, GT_FLIP_LATCH_NS, NULL);   /* best effort */
    g_flip.disabled = 1;
    g_flip.front    = -1;
    kprintf("[intel_gpu] Stage4: flip watchdog -- scanout restored to Limine "
            "FB (0x%x), page-flip disabled\n", g_flip.orig_surf);
}

/* Bounding-box union of (*x,*y,*w,*h) with (ax,ay,aw,ah), result in place. */
static void gt_rect_union(int *x, int *y, int *w, int *h,
                          int ax, int ay, int aw, int ah) {
    if (aw <= 0 || ah <= 0) return;
    if (*w <= 0 || *h <= 0) { *x = ax; *y = ay; *w = aw; *h = ah; return; }
    int x0 = *x < ax ? *x : ax;
    int y0 = *y < ay ? *y : ay;
    int x1 = (*x + *w) > (ax + aw) ? (*x + *w) : (ax + aw);
    int y1 = (*y + *h) > (ay + ah) ? (*y + *h) : (ay + ah);
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

int intel_gt_flip_present(const uint32_t *back, uint32_t back_w, uint32_t back_h,
                          int cur_x, int cur_y, int cur_visible,
                          int dx, int dy, int dw, int dh) {
    if (!g_flip.ready || g_flip.disabled) return -1;
    if (!back || back_w != g_flip.width || back_h != g_flip.height) return -1;

    int off = (g_flip.front == 0) ? 1 : 0;   /* -1->0, 0->1, 1->0 (ping-pong) */

    /* Pipelined flip: before touching buf[off] (the surface the PREVIOUS flip
     * moved away from), wait -- bounded -- for that flip to have latched, so
     * we never blit into a buffer the pipe is still scanning. On the first
     * present the front is the Limine FB (orig_surf), already live. A stuck
     * flip is caught here (one present later) and reverts. */
    uint32_t front_ggtt = (g_flip.front < 0) ? g_flip.orig_surf
                                             : g_flip.buf[g_flip.front].ggtt;
    if (!gt_wait_surflive(front_ggtt, GT_FLIP_LATCH_NS, NULL)) {
        gt_flip_revert();
        return -1;
    }

    uint32_t src_base = gt_src_map(back, back_w, back_h);
    if (!src_base) return -1;

    uint32_t pitch = g_flip.pitch;
    const uint32_t MAGIC = 0xDEAD1234u;
    /* The hardware cursor plane composites the pointer independently; only
     * when it's unavailable do we draw the cursor into the frame (and then
     * need the sentinel to order it after the async blit, and a full-frame
     * blit so the cursor's old position elsewhere in this stale buffer is
     * overwritten). */
    int sw_cursor = (!g_hwcur.ready && cur_visible);

    /* Blit region: the union of this frame's dirty rect and the previous
     * present's (buf[off] is two flips stale). Prime both buffers with a full
     * blit for the first two flips; force full for the SW-cursor path. */
    int rx, ry, rw, rh;
    if (sw_cursor || g_flip.flip_count < 2) {
        rx = 0; ry = 0; rw = (int)g_flip.width; rh = (int)g_flip.height;
    } else {
        rx = dx; ry = dy; rw = dw; rh = dh;
        gt_rect_union(&rx, &ry, &rw, &rh,
                      g_flip.prev_dx, g_flip.prev_dy,
                      g_flip.prev_dw, g_flip.prev_dh);
    }
    /* Clamp to the surface; fall back to full on anything degenerate. */
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > (int)g_flip.width)  rw = (int)g_flip.width  - rx;
    if (ry + rh > (int)g_flip.height) rh = (int)g_flip.height - ry;
    if (rw <= 0 || rh <= 0) { rx = 0; ry = 0; rw = (int)g_flip.width; rh = (int)g_flip.height; }

    /* Flush the source region rows so the blitter's uncached read is fresh. */
    for (int r = 0; r < rh; r++)
        gt_clflush_range((const uint8_t *)back
                         + (size_t)(ry + r) * g_flip.width * 4u
                         + (size_t)rx * 4u,
                         (size_t)rw * 4u);

    /* Sentinel: poison the blit region's bottom-right pixel (written LAST in
     * raster order). Because we arm the flip WITHOUT then waiting for vblank,
     * we must confirm the blit's async writes have fully landed first -- else
     * a vblank that fires mid-blit would latch a partial frame (tear). The
     * old blocking code got this for free from its post-arm SURFLIVE wait. */
    uint32_t region_last = (uint32_t)(ry + rh - 1) * g_flip.width
                         + (uint32_t)(rx + rw - 1);
    g_flip.buf[off].virt[region_last] = MAGIC;
    gt_clflush_range(&g_flip.buf[off].virt[region_last], 4);

    if (gt_emit_blit(g_flip.buf[off].ggtt, pitch, rx, ry,
                     src_base, g_flip.width * 4u, rx, ry, rw, rh) < 0)
        return -1;
    if (!gt_ring_drain(GT_PRESENT_DRAIN_MAX)) { gt_flip_revert(); return -1; }

    /* Wait (bounded) for the blit to land -- the region's last pixel losing
     * the sentinel means the whole region is in DRAM. Cheap for a small drag
     * rect (settles in ~microseconds); this REPLACES the old ~16 ms vblank
     * busy-wait rather than adding to it. */
    for (int i = 0; i < 100000; i++) {
        gt_clflush_range(&g_flip.buf[off].virt[region_last], 4);
        if (g_flip.buf[off].virt[region_last] != MAGIC) break;
        for (volatile int j = 0; j < 50; j++) {}
    }

    /* SW-cursor fallback: composite the pointer on top of the (now-landed)
     * frame. The HW cursor plane handles this independently otherwise. */
    if (sw_cursor)
        gt_flip_draw_cursor(g_flip.buf[off].virt, g_flip.width, g_flip.height,
                            cur_x, cur_y);

    /* Arm the flip and return WITHOUT waiting for the vblank latch -- the pipe
     * latches on its own, and the NEXT present's pre-blit wait enforces the
     * buffer-reuse ordering. This is what keeps the compositor from busy-
     * waiting a full refresh every frame. */
    igpu_write(PLANE_A_SURF, g_flip.buf[off].ggtt);
    (void)igpu_read(PLANE_A_SURF);

    g_flip.front = off;
    g_flip.flip_count++;
    /* Record THIS frame's actual dirty rect (not the widened blit region) for
     * the next present's union. */
    g_flip.prev_dx = dx; g_flip.prev_dy = dy;
    g_flip.prev_dw = dw; g_flip.prev_dh = dh;
    return 0;
}

/* ======================================================================
 * Public API
 * ====================================================================== */

void intel_gpu_modeset_init(void) {
    memset(&g_igpu, 0, sizeof(g_igpu));

    if (!igpu_probe_pci()) {
        printk("[intel_gpu] no supported Intel GPU found\n");
        return;
    }

    if (igpu_map_mmio() < 0)
        return;

    igpu_disable_vga();
    printk("[intel_gpu] VGA disabled, modesetting ready\n");
    g_igpu.ready = 1;
}

int intel_gpu_set_mode(int width, int height, int hz) {
    if (!g_igpu.ready) return -1;

    /* Find matching mode */
    const struct intel_mode *mode = NULL;
    for (int i = 0; i < NUM_MODES; i++) {
        if (g_modes[i].width == width && g_modes[i].height == height) {
            if (hz == 0 || g_modes[i].hz == hz) {
                mode = &g_modes[i];
                break;
            }
        }
    }

    if (!mode) {
        printk("[intel_gpu] no mode found for %dx%d@%d\n", width, height, hz);
        return -1;
    }

    int pipe = 0;
    g_igpu.pipe = pipe;

    /* Disable current pipe if active */
    igpu_disable_pipe(pipe);

    /* Allocate framebuffer */
    if (igpu_alloc_framebuffer(width, height) < 0)
        return -1;

    /* Setup GTT */
    igpu_setup_gtt(g_igpu.fb_phys, g_igpu.fb_size);

    /* Program DPLL */
    struct dpll_params dpll;
    if (igpu_calc_dpll(mode->pixel_clock, &dpll) < 0) {
        printk("[intel_gpu] DPLL calculation failed for %u kHz\n", mode->pixel_clock);
        return -1;
    }
    igpu_program_dpll(pipe, &dpll);

    /* Set pipe timings */
    igpu_set_pipe_timings(pipe, mode);

    /* Configure and enable the display plane */
    igpu_configure_plane(pipe, g_igpu.fb_phys, g_igpu.stride);

    /* Enable pipe */
    igpu_enable_pipe(pipe);

    /* Enable output */
    igpu_enable_output(pipe);

    /* Enable vblank interrupt */
    igpu_enable_vblank_irq(pipe);

    printk("[intel_gpu] mode set: %dx%d@%d pipe=%d\n",
           width, height, mode->hz, pipe);
    return 0;
}

int intel_gpu_page_flip(uint64_t fb_phys_addr) {
    if (!g_igpu.ready) return -1;

    uint32_t addr_reg = (g_igpu.pipe == 0) ? PLANE_A_ADDR : PLANE_B_ADDR;
    igpu_write(addr_reg, (uint32_t)(fb_phys_addr & 0xFFFFFFFF));
    (void)igpu_read(addr_reg);
    return 0;
}

void intel_gpu_wait_vblank(void) {
    if (!g_igpu.ready) return;

    uint32_t mask = (g_igpu.pipe == 0) ? DE_PIPE_A_VBLANK : DE_PIPE_B_VBLANK;

    /* Clear pending vblank interrupt */
    igpu_write(DEIIR, mask);

    /* Spin until vblank fires (timeout ~20ms at 60Hz) */
    for (int i = 0; i < 2000000; i++) {
        if (igpu_read(DEIIR) & mask) {
            igpu_write(DEIIR, mask);
            return;
        }
    }
}

uint32_t *intel_gpu_get_framebuffer(void) {
    return g_igpu.framebuffer;
}

int intel_gpu_detected(void) {
    return g_igpu.ready;
}

int intel_gpu_get_width(void) {
    return g_igpu.width;
}

int intel_gpu_get_height(void) {
    return g_igpu.height;
}

/* ======================================================================
 * Hardware cursor
 * ====================================================================== */

void intel_gpu_set_cursor(const uint32_t *image, int w, int h) {
    if (!g_igpu.ready || !image) return;

    /* Hardware cursor is always 64x64 ARGB */
    memset(g_cursor_buf, 0, sizeof(g_cursor_buf));

    int cw = w > 64 ? 64 : w;
    int ch = h > 64 ? 64 : h;

    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            g_cursor_buf[y * 64 + x] = image[y * w + x];
        }
    }

    uint32_t ctl_reg = (g_igpu.pipe == 0) ? CUR_A_CTL : CUR_B_CTL;
    uint32_t base_reg = (g_igpu.pipe == 0) ? CUR_A_BASE : CUR_B_BASE;

    /* Get physical address of cursor buffer. Since it's in .bss,
     * we can compute its physical address via the kernel identity map. */
    uint64_t cur_phys = pmm_virt_to_phys((void *)g_cursor_buf);

    /* Enable cursor: 64x64 ARGB format */
    igpu_write(ctl_reg, CUR_MODE_64_ARGB);
    igpu_write(base_reg, (uint32_t)(cur_phys & 0xFFFFFFFF));
    (void)igpu_read(base_reg);
}

void intel_gpu_move_cursor(int x, int y) {
    if (!g_igpu.ready) return;

    uint32_t pos_reg = (g_igpu.pipe == 0) ? CUR_A_POS : CUR_B_POS;

    uint32_t pos_val = 0;
    if (x >= 0)
        pos_val |= (uint32_t)x;
    else
        pos_val |= (1u << 15) | (uint32_t)(-x);

    if (y >= 0)
        pos_val |= ((uint32_t)y << 16);
    else
        pos_val |= (1u << 31) | ((uint32_t)(-y) << 16);

    igpu_write(pos_reg, pos_val);
}

void intel_gpu_hide_cursor(void) {
    if (!g_igpu.ready) return;

    uint32_t ctl_reg = (g_igpu.pipe == 0) ? CUR_A_CTL : CUR_B_CTL;
    uint32_t base_reg = (g_igpu.pipe == 0) ? CUR_A_BASE : CUR_B_BASE;

    igpu_write(ctl_reg, 0);
    igpu_write(base_reg, 0);
    (void)igpu_read(base_reg);
}
