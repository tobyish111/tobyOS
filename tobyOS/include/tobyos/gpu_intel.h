/* gpu_intel.h -- Intel HD Graphics Gen 6+ modesetting for tobyOS.
 *
 * Supports Sandy Bridge (Gen 6), Ivy Bridge (Gen 7), Haswell (Gen 7.5)
 * and later Intel integrated GPUs for native display output.
 */

#ifndef TOBYOS_GPU_INTEL_H
#define TOBYOS_GPU_INTEL_H

#include <tobyos/types.h>

/* PCI device IDs we support */
#define INTEL_HD2000  0x0102
#define INTEL_HD3000  0x0116
#define INTEL_HD4000  0x0162
#define INTEL_HD4600  0x0412
#define INTEL_HD530   0x1912
#define INTEL_UHD620  0x3EA0
#define INTEL_UHD630  0x3E92

/* Register offsets (from MMIO base) */
#define PIPE_A_CONF    0x70008
#define PIPE_B_CONF    0x71008
#define DPLL_A         0x06014
#define DPLL_B         0x06018
#define DPLL_A_MD      0x0601C
#define DPLL_B_MD      0x06020
#define FPA0           0x06040
#define FPA1           0x06044
#define FPB0           0x06048
#define FPB1           0x0604C
#define PLANE_A_CTRL   0x70180
#define PLANE_A_ADDR   0x7019C
#define PLANE_A_STRIDE 0x70188
#define PLANE_A_SIZE   0x70190
#define PLANE_A_SURF   0x7019C
#define PLANE_A_OFFSET 0x701A4
#define PLANE_B_CTRL   0x71180
#define PLANE_B_ADDR   0x7119C
#define PLANE_B_STRIDE 0x71188
#define PLANE_B_SIZE   0x71190
#define HTOTAL_A       0x60000
#define HBLANK_A       0x60004
#define HSYNC_A        0x60008
#define VTOTAL_A       0x6000C
#define VBLANK_A       0x60010
#define VSYNC_A        0x60014
#define PIPESRC_A      0x6001C
#define HTOTAL_B       0x61000
#define HBLANK_B       0x61004
#define HSYNC_B        0x61008
#define VTOTAL_B       0x6100C
#define VBLANK_B       0x61010
#define VSYNC_B        0x61014
#define PIPESRC_B      0x6101C
#define VGACNTRL       0x71400
#define ADPA           0x61100
#define HDMI_B         0x61140
#define HDMI_C         0x61160
#define DP_B           0x64100
#define DP_C           0x64200
#define DP_D           0x64300

/* Pipe configuration bits */
#define PIPE_CONF_ENABLE        (1u << 31)
#define PIPE_CONF_STATE_ACTIVE  (1u << 30)
#define PIPE_CONF_8BPC          (0u << 5)
#define PIPE_CONF_10BPC         (1u << 5)
#define PIPE_CONF_PROGRESSIVE   0

/* DPLL bits */
#define DPLL_VCO_ENABLE         (1u << 31)
#define DPLL_REF_CLK_100MHZ     (0u << 13)
#define DPLL_SDVO_HIGH_SPEED    (1u << 30)
#define DPLL_MODE_LVDS          (2u << 26)
#define DPLL_MODE_DAC_SERIAL    (1u << 26)

/* Plane control bits */
#define PLANE_ENABLE            (1u << 31)
#define PLANE_FORMAT_XRGB8888   (6u << 26)
#define PLANE_FORMAT_XBGR8888   (14u << 26)
#define PLANE_TILING_NONE       0
#define PLANE_TILING_X          (1u << 10)

/* VGA control */
#define VGA_DISABLE             (1u << 31)

/* Cursor plane registers */
#define CUR_A_CTL      0x70080
#define CUR_A_BASE     0x70084
#define CUR_A_POS      0x70088
#define CUR_B_CTL      0x700C0
#define CUR_B_BASE     0x700C4
#define CUR_B_POS      0x700C8

#define CUR_ENABLE             (1u << 5)
#define CUR_FORMAT_ARGB        (1u << 5)  /* 64x64 ARGB cursor */
#define CUR_MODE_64_ARGB       0x27

/* GTT (Graphics Translation Table) */
#define GTT_BASE               0x200000
#define GTT_ENTRY_VALID        (1u << 0)
#define GTT_ENTRY_LOCAL_MEM    0

/* Interrupt registers */
#define DEIMR          0x44004
#define DEIER          0x4400C
#define DEIIR          0x44008
#define DEISR          0x44000
#define DE_PIPE_A_VBLANK       (1u << 0)
#define DE_PIPE_B_VBLANK       (1u << 5)

/* Standard timing modes */
struct intel_mode {
    int width, height, hz;
    uint32_t pixel_clock;   /* kHz */
    int h_display, h_sync_start, h_sync_end, h_total;
    int v_display, v_sync_start, v_sync_end, v_total;
};

struct intel_gpu {
    volatile uint32_t *mmio;
    uint64_t mmio_phys;
    size_t mmio_size;
    uint32_t *framebuffer;
    uint64_t fb_phys;
    size_t fb_size;
    int width, height, stride;
    int pipe;
    int ready;
    uint16_t device_id;
    int gen;
    int has_pch;
    uint32_t *back_buffer;
    uint64_t back_phys;
    /* PCI bus/dev/func of the GPU, recorded at probe so the GT recon
     * pass can read MGGC0 / BDSM (graphics control + stolen-memory
     * base) out of config space without re-scanning. */
    uint8_t  bus, dev, func;
    int      bdf_valid;
};

/* Stage 1 (i915-lite): read-only GT + display reconnaissance. Probes the
 * Intel GPU, maps its register MMIO, and logs the GT generation, GGTT /
 * stolen-memory config, blitter ring register state, and the display
 * plane/pipe Limine left active. Touches NO display registers and writes
 * nothing -- safe to call on any machine (silent no-op without an Intel
 * GPU). Its log output is the data we need to bring up the BLT ring and a
 * tear-free page-flip on the real EliteDesk, which QEMU cannot emulate. */
void     intel_gpu_gt_recon(void);

/* ======================================================================
 * Stage 2 (i915-lite): GT / render-engine bring-up (gen8/gen9, e.g. the
 * Skylake EliteDesk). Completely independent of the display pipe -- it
 * only touches forcewake, the GGTT, the blitter (BCS) ring, and buffers
 * we allocate ourselves. It NEVER programs the pipe/plane/DPLL/VGA, so a
 * broken blitter can at worst fail to update the screen, never blackscreen
 * it. QEMU has no Intel GT, so this is real-HW-only; everything is gated on
 * a hardware self-test (intel_gt_selftest_ok) before any user trusts it.
 * ====================================================================== */

/* Forcewake (gen9 render/blitter regs are dead without it). */
#define FORCEWAKE_RENDER_GEN9   0x0A278u   /* multi-threaded forcewake req  */
#define FORCEWAKE_RENDER_ACK    0x0D84u    /* GTSP1 ack (render)            */
#define FORCEWAKE_BLITTER_GEN9  0x0A188u
#define FORCEWAKE_BLITTER_ACK   0x130044u
#define FORCEWAKE_KERNEL_BIT    0          /* we drive bit 0 (mask<<16|val) */

/* Gen7.5 (Haswell) forcewake: a SINGLE multi-threaded domain wakes render +
 * blitter + media together (gen9 later split them). Same masked-write
 * (mask<<16|val) protocol as gt_forcewake_get. The address happens to match
 * FORCEWAKE_BLITTER_GEN9 -- gen9 reused it for its blitter domain -- but the
 * semantics differ, so name it for gen7. */
#define FORCEWAKE_MT_HSW        0x0A188u   /* Haswell FORCEWAKE_MT           */
#define FORCEWAKE_ACK_HSW       0x130044u  /* Haswell forcewake ack          */

/* Blitter Command Streamer (BCS) ring registers (gen8+ island base). */
#define GEN8_BCS_RING_BASE      0x22000u
#define RING_TAIL               0x30u
#define RING_HEAD               0x34u
#define RING_START              0x38u
#define RING_CTL                0x3Cu
#define RING_CTL_ENABLE         (1u << 0)
#define RING_HEAD_MASK          0x1FFFFCu
#define RING_TAIL_MASK          0x1FFFF8u

/* Gen8/9 GGTT lives in the TOP HALF of the (16 MiB) MMIO BAR; PTEs are
 * 8 bytes (NOT the gen7 4-byte PTEs at 0x200000 the old modeset code used).
 * A GGTT PTE is just the page phys addr | present. */
#define GEN8_GGTT_OFFSET        (8u << 20)     /* 8 MiB into a 16 MiB BAR   */
#define GEN8_GGTT_PTE_PRESENT   (1ull << 0)

/* Blitter command opcodes (gen8/9). */
#define MI_NOOP                 0x00000000u
/* MI_STORE_DWORD_IMM (gen8+): opcode 0x20, DWord Length 2 (4-dword cmd:
 * header + 64-bit addr + 1 data dword). Bit 22 = "Use Global GTT" -- REQUIRED
 * when the destination is a GGTT address (ours is); without it the engine
 * treats the address as a PPGTT VA we never set up, so the store is dropped
 * even though the command retires (ring head still advances). */
#define MI_USE_GLOBAL_GTT       (1u << 22)
#define MI_STORE_DWORD_IMM_GEN8 ((0x20u << 23) | MI_USE_GLOBAL_GTT | 2u)
/* Gen7 MI_STORE_DWORD_IMM: the address is a SINGLE 32-bit dword (gen8 widened
 * it to 64-bit / 2 dwords), so the command is 3 dwords total -> DWord Length 1
 * (len = total_dwords - 2). Same 0x20 opcode + "Use Global GTT" bit. */
#define MI_STORE_DWORD_IMM_GEN7 ((0x20u << 23) | MI_USE_GLOBAL_GTT | 1u)
#define MI_BATCH_BUFFER_END     (0x0Au << 23)
#define XY_COLOR_BLT_CMD        ((2u << 29) | (0x50u << 22) | 0x4u) /* + BPP */
#define XY_SRC_COPY_BLT_CMD     ((2u << 29) | (0x53u << 22) | 0x6u)
#define BLT_WRITE_ALPHA         (1u << 21)
#define BLT_WRITE_RGB           (1u << 20)
#define BLT_DEPTH_32            (3u << 24)     /* in BR13 (color depth)     */
#define BLT_ROP_SRCCOPY         (0xCCu << 16)
#define BLT_ROP_PATCOPY         (0xF0u << 16)

/* Stage 2 entry: bring up forcewake + GGTT + BCS ring and run the
 * store-dword self-test. Logs everything; sets the self-test flag on
 * success. Safe no-op without a supported Intel GPU. */
void     intel_gt_init(void);
int      intel_gt_selftest_ok(void);   /* 1 only if the GT proved itself */

/* ---- Stage 4 (deferred): hardware page-flip via PLANE_SURF ----
 * The display plane scans through the GGTT, so a tear-free flip would just
 * repoint PLANE_SURF at a GGTT offset (no timing/DPLL/format change). The
 * flip self-test was reverted 2026-06-06: it hung the EliteDesk, and no
 * off-box log channel works on that machine (no serial/net; USB-write also
 * hung), so it can't be debugged blind. These register defs are kept as
 * groundwork for when a working feedback channel exists. */
#define GEN8_DE_PIPE_A_IIR      0x44408u   /* per-pipe interrupt status (RW1C) */
#define GEN8_PIPE_VBLANK        (1u << 0)
#define PLANE_A_SURFLIVE        0x701ACu   /* address the plane is CURRENTLY scanning */

void     intel_gpu_modeset_init(void);
int      intel_gpu_set_mode(int width, int height, int hz);
int      intel_gpu_page_flip(uint64_t fb_phys_addr);
void     intel_gpu_wait_vblank(void);
uint32_t *intel_gpu_get_framebuffer(void);
int      intel_gpu_detected(void);
int      intel_gpu_get_width(void);
int      intel_gpu_get_height(void);

/* Hardware cursor support */
void     intel_gpu_set_cursor(const uint32_t *image, int w, int h);
void     intel_gpu_move_cursor(int x, int y);
void     intel_gpu_hide_cursor(void);

#endif /* TOBYOS_GPU_INTEL_H */
