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

                /* Determine MMIO size (typically 512KB to 2MB) */
                pci_cfg_write32(bus, dev, func, 0x10, 0xFFFFFFFF);
                uint32_t size_mask = pci_cfg_read32(bus, dev, func, 0x10);
                pci_cfg_write32(bus, dev, func, 0x10, bar0);
                size_t mmio_size = ~(size_mask & 0xFFFFFFF0u) + 1;
                if (mmio_size < 0x80000) mmio_size = 0x80000;
                if (mmio_size > 0x400000) mmio_size = 0x400000;

                /* Enable bus mastering and memory space */
                uint32_t cmd = pci_cfg_read32(bus, dev, func, 0x04);
                cmd |= (1u << 1) | (1u << 2);
                pci_cfg_write32(bus, dev, func, 0x04, cmd);

                g_igpu.mmio_phys = mmio_base;
                g_igpu.mmio_size = mmio_size;
                g_igpu.device_id = device;
                g_igpu.gen = igpu_get_gen(device);
                g_igpu.has_pch = (g_igpu.gen >= 6);

                printk("[intel_gpu] found device %04x at %02x:%02x.%d "
                       "gen=%d mmio=0x%llx size=0x%zx\n",
                       device, bus, dev, func,
                       g_igpu.gen, (unsigned long long)mmio_base, mmio_size);
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
