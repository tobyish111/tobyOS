/* audio_hda.c -- Intel HD Audio (HDA) PCI driver.
 *
 * Milestone 26F: full controller bring-up + codec enumeration over
 * CORB/RIRB + best-effort tone playback through one output stream.
 *
 * Boot flow when an HDA controller is present:
 *
 *   pci_bind_drivers()
 *     -> hda_probe(dev)
 *          -> pci_map_bar(BAR0, 16 KiB)
 *          -> pci_dev_enable(MEM | BUS_MASTER)
 *          -> hda_controller_reset()              (CRST 1->0->1)
 *          -> hda_corb_rirb_init()                (DMA page split 50/50)
 *          -> hda_codec_scan()                    (STATESTS bitmap)
 *               for each cad in STATESTS:
 *                 hda_codec_enumerate(cad)        (Vendor/Rev/AFG/widgets)
 *
 * Subsequent self-tests:
 *   devtest_run("audio")        -> audio_hda_selftest()
 *   devtest_run("audio_tone")   -> audio_hda_tone_selftest()
 *
 * Boot flow when NO HDA controller is present:
 *   the PCI scan finds no match, hda_probe() never runs, every
 *   introspection / selftest cleanly reports SKIP. The driver does
 *   nothing and there is no kernel log noise beyond the generic
 *   "PCI: no driver claimed dev ..." line, which is also absent
 *   because the audio class isn't on the QEMU baseline machine.
 *
 * QEMU validation topology (mirrored by test_m26f.ps1):
 *   -audiodev none,id=hda \
 *   -device   intel-hda,id=hda0,audiodev=hda \
 *   -device   hda-output,bus=hda0.0,audiodev=hda
 *
 * Real hardware: the same code path runs on every Intel ICH/PCH HDA
 * controller (8086:1c20, 8086:8d20, 8086:a170, ...) and on AMD
 * 1022:1457 / 1022:15e3, plus VIA/Realtek clones, since we match by
 * class triple, not vendor/device. */

#include <tobyos/audio_hda.h>
#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>

/* ============================================================
 * PCI class match
 * ============================================================ */
#define PCI_CLASS_MULTIMEDIA      0x04
#define PCI_SUBCLASS_HDA          0x03
#define PCI_SUBCLASS_AUDIODEVICE  0x80   /* some chips report this */

/* ============================================================
 * Controller register map (Intel HDA spec rev 1.0a, table 4)
 * ============================================================ */

/* 16-bit Global Capabilities. Per HDA spec rev 1.0a, table 4:
 *   bits[15:12] = OSS  (Number of Output Streams Supported, 4 bits)
 *   bits[11: 8] = ISS  (Number of Input  Streams Supported, 4 bits)
 *   bits[ 7: 3] = BSS  (Number of Bidir  Streams Supported, 5 bits)
 *   bits[ 2: 1] = NSDO (Number of Serial Data Out signals)
 *   bit [ 0]    = 64OK (64-bit Address Supported)
 *
 * QEMU's intel-hda emulation reports gcap=0x4401 = OSS=4, ISS=4,
 * BSS=0, NSDO=0, 64OK=1. */
#define HDA_REG_GCAP        0x00
#define HDA_REG_VMIN        0x02
#define HDA_REG_VMAJ        0x03
#define HDA_REG_OUTPAY      0x04
#define HDA_REG_INPAY       0x06
#define HDA_REG_GCTL        0x08    /* 32-bit Global Control */
#define HDA_REG_WAKEEN      0x0C    /* 16-bit Wake Enable     */
#define HDA_REG_STATESTS    0x0E    /* 16-bit Codec Status    */
#define HDA_REG_GSTS        0x10
#define HDA_REG_INTCTL      0x20    /* 32-bit Interrupt Ctl   */
#define HDA_REG_INTSTS      0x24
#define HDA_REG_WALCLK      0x30
#define HDA_REG_SSYNC       0x38

#define HDA_REG_CORBLBASE   0x40
#define HDA_REG_CORBUBASE   0x44
#define HDA_REG_CORBWP      0x48
#define HDA_REG_CORBRP      0x4A
#define HDA_REG_CORBCTL     0x4C
#define HDA_REG_CORBSTS     0x4D
#define HDA_REG_CORBSIZE    0x4E

#define HDA_REG_RIRBLBASE   0x50
#define HDA_REG_RIRBUBASE   0x54
#define HDA_REG_RIRBWP      0x58
#define HDA_REG_RINTCNT     0x5A
#define HDA_REG_RIRBCTL     0x5C
#define HDA_REG_RIRBSTS     0x5D
#define HDA_REG_RIRBSIZE    0x5E

/* GCTL bits */
#define HDA_GCTL_CRST       (1u << 0)
#define HDA_GCTL_FCNTRL     (1u << 1)

/* INTCTL bits */
#define HDA_INTCTL_GIE      (1u << 31)
#define HDA_INTCTL_CIE      (1u << 30)

/* CORBCTL / CORBRP / RIRBCTL / RIRBWP bits */
#define HDA_CORBCTL_RUN     (1u << 1)
#define HDA_CORBRP_RST      (1u << 15)
#define HDA_RIRBCTL_INTCTL  (1u << 0)   /* RBCTL_IRQ_EN -- response IRQ */
#define HDA_RIRBCTL_DMAEN   (1u << 1)
#define HDA_RIRBWP_RST      (1u << 15)

/* RIRBSTS bits */
#define HDA_RIRBSTS_RINTFL  (1u << 0)
#define HDA_RIRBSTS_BCIS    (1u << 2)

/* Stream descriptor base + stride (each SD is 0x20 bytes wide).
 * Layout: ISS input descriptors first, then OSS output descriptors,
 * then BSS bidir descriptors. */
#define HDA_SD_BASE         0x80
#define HDA_SD_STRIDE       0x20

/* Per-stream-descriptor register offsets (relative to SD base). */
#define HDA_SD_CTL_LO       0x00    /* 8-bit control byte 0 (RUN/SRST/...)*/
#define HDA_SD_CTL_HI       0x02    /* 8-bit control byte 2 (stream tag)  */
#define HDA_SD_STS          0x03    /* 8-bit status                       */
#define HDA_SD_LPIB         0x04    /* 32-bit position                    */
#define HDA_SD_CBL          0x08    /* 32-bit cyclic buffer length        */
#define HDA_SD_LVI          0x0C    /* 16-bit last valid index            */
#define HDA_SD_FIFOS        0x10
#define HDA_SD_FMT          0x12    /* 16-bit stream format               */
#define HDA_SD_BDPL         0x18    /* 32-bit BDL phys low                */
#define HDA_SD_BDPU         0x1C    /* 32-bit BDL phys high               */

/* SDxCTL byte 0 bits */
#define HDA_SDCTL_SRST      (1u << 0)
#define HDA_SDCTL_RUN       (1u << 1)
#define HDA_SDCTL_IOCE      (1u << 2)

/* ============================================================
 * Codec verbs
 * ============================================================ */

/* 12-bit verbs (long form, 8-bit data). */
#define HDA_VERB_GET_PARAM         0xF00
#define HDA_VERB_GET_CONN_LIST     0xF02
#define HDA_VERB_SET_STREAM_CHAN   0x706
#define HDA_VERB_SET_PIN_CTL       0x707
#define HDA_VERB_SET_POWER_STATE   0x705
#define HDA_VERB_SET_EAPD_BTL      0x70C

/* 4-bit short verbs (16-bit data) -- bits[19:16] of the verb word. */
#define HDA_VERB_SHORT_SET_FORMAT   0x2
#define HDA_VERB_SHORT_SET_AMP_GAIN 0x3

/* GET_PARAM IDs */
#define HDA_PARAM_VENDOR_ID         0x00
#define HDA_PARAM_REVISION_ID       0x02
#define HDA_PARAM_SUBNODE_COUNT     0x04
#define HDA_PARAM_FUNCTION_TYPE     0x05
#define HDA_PARAM_AUDIO_WIDGET_CAP  0x09

/* AUDIO_WIDGET_CAP type field bits[23:20] */
#define HDA_WIDGET_TYPE_DAC        0x0
#define HDA_WIDGET_TYPE_ADC        0x1
#define HDA_WIDGET_TYPE_MIXER      0x2
#define HDA_WIDGET_TYPE_SELECTOR   0x3
#define HDA_WIDGET_TYPE_PIN        0x4

/* PIN_WIDGET_CONTROL bits */
#define HDA_PIN_CTL_OUT_EN         0x40
#define HDA_PIN_CTL_HP_EN          0x80

/* ============================================================
 * Driver state (single controller -- enough for QEMU + every laptop
 * I've seen; second controllers are silently ignored in probe).
 * ============================================================ */

#define HDA_CORB_ENTRIES   256   /* 4 B each = 1 KiB              */
#define HDA_RIRB_ENTRIES   256   /* 8 B each = 2 KiB              */

static struct {
    bool                bound;
    struct pci_dev     *dev;
    volatile uint8_t   *mmio;
    uint8_t             vmaj, vmin;
    uint16_t            gcap;
    uint8_t             iss;          /* input stream descriptor count */
    uint8_t             oss;          /* output stream descriptor count */
    uint8_t             bss;          /* bidir stream descriptor count */
    bool                addr64;       /* 64-bit DMA addressing supported */

    /* CORB/RIRB DMA. Both rings live in one 4 KiB page:
     *   [0      .. 1023]  CORB (256 * 4 B)
     *   [1024   .. 3071]  RIRB (256 * 8 B)
     *   [3072   .. 4095]  unused. */
    volatile uint32_t  *corb;
    uint64_t            corb_phys;
    uint16_t            corb_wp;      /* shadow of HDA_REG_CORBWP        */
    volatile uint32_t  *rirb;         /* read as pairs (response, ext)   */
    uint64_t            rirb_phys;
    uint16_t            rirb_rp;      /* our read cursor (HW writes WP)  */
    bool                rings_up;

    /* Codec table */
    struct audio_hda_codec_info codecs[AUDIO_HDA_MAX_CODECS];
    int                 codec_count;
    uint16_t            statests_post_reset; /* presence bitmap latched once  */

    /* Output stream 0: BDL + tone PCM buffer (lazy alloc on first
     * audio_hda_tone_selftest call). */
    volatile uint32_t  *bdl;          /* 4 entries * 16 B = 64 B used    */
    uint64_t            bdl_phys;
    int16_t            *pcm;          /* 1024 stereo frames = 4096 B     */
    uint64_t            pcm_phys;
    bool                tone_attempted;
    bool                tone_started;

    /* Telemetry exposed via introspect/selftest for the test harness. */
    uint64_t            verbs_sent;
    uint64_t            verbs_replied;
    uint64_t            verb_timeouts;
    uint64_t            streams_started;
} g_hda;

/* ============================================================
 * MMIO accessor primitives
 * ============================================================ */
static inline uint32_t hda_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_hda.mmio + off);
}
static inline void     hda_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_hda.mmio + off) = v;
}
static inline uint16_t hda_r16(uint32_t off) {
    return *(volatile uint16_t *)(g_hda.mmio + off);
}
static inline void     hda_w16(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(g_hda.mmio + off) = v;
}
static inline uint8_t  hda_r8 (uint32_t off) {
    return *(volatile uint8_t  *)(g_hda.mmio + off);
}
static inline void     hda_w8 (uint32_t off, uint8_t v) {
    *(volatile uint8_t  *)(g_hda.mmio + off) = v;
}

/* ============================================================
 * Verb construction + send/recv
 * ============================================================ */

/* Long-form verb (12-bit verb, 8-bit data). Used for almost everything:
 * Get Parameter, Set Pin Control, Set Power State, etc. */
static inline uint32_t hda_verb12(uint8_t cad, uint8_t nid,
                                  uint16_t verb, uint8_t data) {
    return ((uint32_t)cad  << 28) |
           ((uint32_t)nid  << 20) |
           ((uint32_t)(verb & 0xFFF) << 8) |
           (uint32_t)data;
}

/* Short-form verb (4-bit verb, 16-bit data). Used for Set Format,
 * Set Amplifier Gain/Mute, Get Connection Index. */
static inline uint32_t hda_verb4(uint8_t cad, uint8_t nid,
                                 uint8_t verb, uint16_t data) {
    return ((uint32_t)cad  << 28) |
           ((uint32_t)nid  << 20) |
           ((uint32_t)(verb & 0xF) << 16) |
           (uint32_t)data;
}

/* Send a verb over CORB and poll RIRB for the response.
 *
 * The CORB is a ring of 256 32-bit verbs. We advance our shadow write
 * pointer, deposit the verb at the new slot, then write CORBWP. The
 * controller DMA-reads the new slot, hands the verb to the codec on
 * SDI[cad], and the codec's response shows up in RIRB[next] together
 * with an extended response word (codec address, solicited flag, ...).
 *
 * We poll RIRBWP rather than firing an interrupt -- the bring-up code
 * runs entirely synchronously and the spin is microseconds long. The
 * boot path also runs before any reschedule point, so a 200-iteration
 * busy loop is fine (CORB DMA latency on QEMU is < 1 us, real chips
 * complete in 5-10 us).
 *
 * Returns the 32-bit response, or 0xFFFFFFFFu on timeout. */
static uint32_t hda_send_verb(uint32_t cmd) {
    if (!g_hda.rings_up) return 0xFFFFFFFFu;

    g_hda.verbs_sent++;

    /* RIRBSTS.RINTFL latches whenever RINTCNT responses have arrived;
     * we clear it before kicking the next verb so the controller
     * doesn't think the response queue is wedged. (Spec doesn't *say*
     * this gates DMA, but in practice QEMU's intel-hda model only
     * writes the next RIRB slot after RINTFL is acked when RINTCNT=1.
     * Clearing it before every send is cheap and matches what the
     * Linux HDA driver does in its polled-mode fallback.) */
    hda_w8(HDA_REG_RIRBSTS, HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_BCIS);

    uint16_t wp = (uint16_t)((g_hda.corb_wp + 1) & (HDA_CORB_ENTRIES - 1));
    g_hda.corb[wp] = cmd;
    /* Make the verb visible to the controller before we ring CORBWP. */
    __asm__ __volatile__("" ::: "memory");
    hda_w16(HDA_REG_CORBWP, wp);
    g_hda.corb_wp = wp;

    /* Wait for RIRBWP to advance past our last-known position. RIRB
     * slots are 8 bytes (response + extended response), so we index
     * the rirb array by entry * 2. We give it ~10 ms total: real
     * codecs reply in <10 us, but unsolicited-response controllers
     * sometimes hold off for a frame boundary. */
    for (int i = 0; i < 10000; i++) {
        uint16_t rwp = (uint16_t)(hda_r16(HDA_REG_RIRBWP) &
                                  (HDA_RIRB_ENTRIES - 1));
        if (rwp != g_hda.rirb_rp) {
            g_hda.rirb_rp = (uint16_t)((g_hda.rirb_rp + 1) &
                                       (HDA_RIRB_ENTRIES - 1));
            uint32_t resp = g_hda.rirb[g_hda.rirb_rp * 2 + 0];
            /* g_hda.rirb[rp*2 + 1] is the extended response (cad,
             * solicited, ...) -- not currently used. */
            g_hda.verbs_replied++;
            return resp;
        }
        /* Tight micro-delay so we don't spin the bus. ~1 us on QEMU. */
        for (volatile int j = 0; j < 200; j++) { }
    }

    g_hda.verb_timeouts++;
    kprintf("[hda] verb timeout: cmd=0x%08x "
            "(CORBWP=0x%x CORBRP=0x%x CORBCTL=0x%02x "
            "RIRBWP=0x%x rp=0x%x RIRBCTL=0x%02x RIRBSTS=0x%02x)\n",
            cmd,
            hda_r16(HDA_REG_CORBWP), hda_r16(HDA_REG_CORBRP),
            hda_r8 (HDA_REG_CORBCTL),
            hda_r16(HDA_REG_RIRBWP), g_hda.rirb_rp,
            hda_r8 (HDA_REG_RIRBCTL), hda_r8(HDA_REG_RIRBSTS));
    return 0xFFFFFFFFu;
}

/* ============================================================
 * Controller bring-up: CRST cycle + CORB/RIRB rings
 * ============================================================ */

/* Drop CRST low, wait for it to read 0, raise it, wait for it to read 1,
 * then sleep ~1 ms so codecs can assert their STATESTS bits. */
static bool hda_controller_reset(void) {
    /* Stop CORB/RIRB DMA -- a warm reset must start with both rings
     * idle, otherwise the controller can wedge with stale state. */
    hda_w8(HDA_REG_CORBCTL, hda_r8(HDA_REG_CORBCTL) & (uint8_t)~HDA_CORBCTL_RUN);
    hda_w8(HDA_REG_RIRBCTL, hda_r8(HDA_REG_RIRBCTL) & (uint8_t)~HDA_RIRBCTL_DMAEN);

    /* Stop every stream descriptor (defensive -- if BIOS / EFI started
     * one for the boot bell, the controller can't reset cleanly). */
    int total_sd = g_hda.iss + g_hda.oss + g_hda.bss;
    for (int s = 0; s < total_sd; s++) {
        uint32_t off = HDA_SD_BASE + (uint32_t)s * HDA_SD_STRIDE;
        hda_w8(off + HDA_SD_CTL_LO, 0);
    }

    /* Drop CRST low. */
    uint32_t gctl = hda_r32(HDA_REG_GCTL);
    hda_w32(HDA_REG_GCTL, gctl & ~HDA_GCTL_CRST);
    for (int i = 0; i < 1000; i++) {
        if ((hda_r32(HDA_REG_GCTL) & HDA_GCTL_CRST) == 0) break;
        for (volatile int j = 0; j < 200; j++) { }
    }
    if (hda_r32(HDA_REG_GCTL) & HDA_GCTL_CRST) {
        kprintf("[hda] CRST never went low (GCTL=0x%08x)\n",
                hda_r32(HDA_REG_GCTL));
        return false;
    }

    /* Raise CRST. */
    gctl = hda_r32(HDA_REG_GCTL);
    hda_w32(HDA_REG_GCTL, gctl | HDA_GCTL_CRST);
    for (int i = 0; i < 1000; i++) {
        if (hda_r32(HDA_REG_GCTL) & HDA_GCTL_CRST) break;
        for (volatile int j = 0; j < 200; j++) { }
    }
    if ((hda_r32(HDA_REG_GCTL) & HDA_GCTL_CRST) == 0) {
        kprintf("[hda] CRST never came back up (GCTL=0x%08x)\n",
                hda_r32(HDA_REG_GCTL));
        return false;
    }

    /* HDA spec section 5.5.1.2: wait at least 521 us for codecs to
     * assert STATESTS after CRST=1. We sleep 2 ms to be generous. */
    pit_sleep_ms(2);

    /* Latch the codec-presence bitmap NOW, while it is still valid.
     * STATESTS is RW1C: any subsequent read/modify/write that happens
     * to write 1 to a presence bit will silently clear it, and once
     * cleared the bit will not re-assert until the codec issues a
     * fresh wakeup event -- which post-boot codecs don't, because
     * they are already awake. The M26F bring-up bug was a stray clear
     * of STATESTS here, which made every probe report "0 codec(s)"
     * even with -device hda-duplex attached. Save it once so later
     * code (and selftest output) can rely on the snapshot. */
    g_hda.statests_post_reset = hda_r16(HDA_REG_STATESTS);

    /* Disable wake events for now -- we're polling. */
    hda_w16(HDA_REG_WAKEEN, 0);
    /* Disable global / controller interrupts; we'll re-enable when we
     * actually wire MSI in a later milestone. The rings work fine in
     * pure-polled mode for codec verbs, and tone playback is bounded
     * by pit_sleep_ms() in the test harness. */
    hda_w32(HDA_REG_INTCTL, 0);

    return true;
}

/* Allocate one 4 KiB DMA page; split it CORB(0..1023) / RIRB(1024..3071);
 * program the controller registers; reset both ring pointers; start DMA. */
static bool hda_corb_rirb_init(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        kprintf("[hda] CORB/RIRB DMA alloc failed\n");
        return false;
    }
    void *virt = pmm_phys_to_virt(phys);
    memset(virt, 0, 4096);

    g_hda.corb      = (volatile uint32_t *)virt;
    g_hda.corb_phys = phys;
    g_hda.rirb      = (volatile uint32_t *)((uint8_t *)virt + 1024);
    g_hda.rirb_phys = phys + 1024;

    /* CORB: program size to 256 entries (CORBSIZE bits[1:0]=2). */
    hda_w8(HDA_REG_CORBSIZE, 0x02);
    hda_w32(HDA_REG_CORBLBASE, (uint32_t)(g_hda.corb_phys & 0xFFFFFFFFu));
    hda_w32(HDA_REG_CORBUBASE, (uint32_t)(g_hda.corb_phys >> 32));

    /* Reset CORB read pointer. Spec dance: set CORBRPRST (bit 15), wait
     * for it to read back as 1 (= reset acknowledged), clear it, wait
     * for it to read back as 0 (= reset complete). On QEMU the bit
     * latches instantly; on some real chips this takes a few PCI
     * cycles, hence the bounded loops. */
    hda_w16(HDA_REG_CORBRP, HDA_CORBRP_RST);
    for (int i = 0; i < 1000; i++) {
        if (hda_r16(HDA_REG_CORBRP) & HDA_CORBRP_RST) break;
        for (volatile int j = 0; j < 100; j++) { }
    }
    hda_w16(HDA_REG_CORBRP, 0);
    for (int i = 0; i < 1000; i++) {
        if ((hda_r16(HDA_REG_CORBRP) & HDA_CORBRP_RST) == 0) break;
        for (volatile int j = 0; j < 100; j++) { }
    }
    /* CORBWP starts at 0; the first verb we send goes into slot 1. */
    hda_w16(HDA_REG_CORBWP, 0);
    g_hda.corb_wp = 0;

    /* RIRB: program size to 256 entries. */
    hda_w8(HDA_REG_RIRBSIZE, 0x02);
    hda_w32(HDA_REG_RIRBLBASE, (uint32_t)(g_hda.rirb_phys & 0xFFFFFFFFu));
    hda_w32(HDA_REG_RIRBUBASE, (uint32_t)(g_hda.rirb_phys >> 32));

    /* Reset RIRB write pointer. RIRBWP_RST is write-1-clears-WP and
     * auto-clears immediately on QEMU. */
    hda_w16(HDA_REG_RIRBWP, HDA_RIRBWP_RST);
    g_hda.rirb_rp = 0;

    /* Configure RIRB to interrupt every 1 response. We don't route
     * the actual IRQ to the CPU (INTCTL=0 keeps GIE off, so QEMU's
     * pci_set_irq() never fires), but we MUST set RIRBCTL.IRQ_EN.
     * QEMU's intel-hda emulation has this gotcha:
     *
     *   intel_hda_corb_run() returns early if rirb_count >= rirb_cnt.
     *   The only way to reset rirb_count is to clear RIRBSTS.IRQ.
     *   But QEMU only SETS RIRBSTS.IRQ if RIRBCTL.IRQ_EN is on.
     *
     * So with IRQ_EN off the controller silently wedges after
     * exactly RINTCNT responses (we'd see verb 1 succeed, every
     * subsequent verb time out). Enabling IRQ_EN at the controller
     * level + clearing RIRBSTS.IRQ in hda_send_verb() gives us the
     * write-1-clear handshake QEMU needs to reset rirb_count and
     * resume DMA. INTCTL stays 0 so no real IRQ is asserted. */
    hda_w16(HDA_REG_RINTCNT, 1);
    hda_w8(HDA_REG_RIRBSTS, HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_BCIS);

    /* Start CORB and RIRB DMA. */
    hda_w8(HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    hda_w8(HDA_REG_RIRBCTL, HDA_RIRBCTL_DMAEN | HDA_RIRBCTL_INTCTL);

    g_hda.rings_up = true;
    return true;
}

/* ============================================================
 * Codec enumeration
 * ============================================================ */

/* Walk one codec's tree: Vendor ID -> AFG -> widgets. Adds an entry to
 * g_hda.codecs on success. Logs the per-widget breakdown to serial so
 * the test harness can grep for it. */
static void hda_codec_enumerate(uint8_t cad) {
    if (g_hda.codec_count >= AUDIO_HDA_MAX_CODECS) {
        kprintf("[hda] codec %u: table full, ignoring\n", cad);
        return;
    }

    struct audio_hda_codec_info *c = &g_hda.codecs[g_hda.codec_count];
    memset(c, 0, sizeof *c);
    c->cad = cad;

    uint32_t r = hda_send_verb(hda_verb12(cad, 0,
                                          HDA_VERB_GET_PARAM,
                                          HDA_PARAM_VENDOR_ID));
    if (r == 0xFFFFFFFFu || r == 0) {
        kprintf("[hda] codec %u: no Vendor ID (resp=0x%08x) -- skipping\n",
                cad, r);
        return;
    }
    c->vendor = (uint16_t)(r >> 16);
    c->device = (uint16_t)r;

    r = hda_send_verb(hda_verb12(cad, 0,
                                 HDA_VERB_GET_PARAM,
                                 HDA_PARAM_REVISION_ID));
    c->revision = (uint16_t)(r & 0xFFFFu);

    /* Subordinate node count at root: bits[23:16]=starting NID,
     * bits[7:0]=count. Function groups live in this NID range. */
    r = hda_send_verb(hda_verb12(cad, 0,
                                 HDA_VERB_GET_PARAM,
                                 HDA_PARAM_SUBNODE_COUNT));
    if (r == 0xFFFFFFFFu) {
        kprintf("[hda] codec %u: SUBNODE_COUNT verb timed out -- skipping\n",
                cad);
        return;
    }
    uint8_t fg_start = (uint8_t)(r >> 16);
    uint8_t fg_count = (uint8_t)r;

    kprintf("[hda] codec %u: vendor=0x%04x device=0x%04x rev=0x%04x "
            "%u FG(s) @NID %u\n",
            cad, c->vendor, c->device, c->revision, fg_count, fg_start);

    /* Sanity bound: a real codec has at most a handful of FGs (typically
     * just an AFG, sometimes an MFG). Walking 256 NIDs takes thousands
     * of verbs and pollutes the serial log. Anything > 16 means we
     * misparsed SUBNODE_COUNT; bail rather than spam the rings.
     * Keep the iterator and the upper bound in `unsigned` so we don't
     * wrap when fg_start + fg_count crosses 255. */
    if (fg_count == 0 || fg_count > 16) {
        kprintf("[hda] codec %u: implausible FG count %u (start=%u) -- skipping\n",
                cad, fg_count, fg_start);
        return;
    }

    /* Find the first AFG (function type 0x01). MFG (0x02) carries
     * MIDI/modem and we don't drive it. */
    bool found_afg = false;
    unsigned fg_end = (unsigned)fg_start + fg_count;
    for (unsigned fgu = fg_start; fgu < fg_end; fgu++) {
        uint8_t fg = (uint8_t)fgu;
        r = hda_send_verb(hda_verb12(cad, fg,
                                     HDA_VERB_GET_PARAM,
                                     HDA_PARAM_FUNCTION_TYPE));
        uint8_t fgt = (uint8_t)r;
        if (fgt != 0x01) {
            kprintf("[hda]   FG NID %u: type=0x%02x (skip non-AFG)\n",
                    fg, fgt);
            continue;
        }
        c->afg_nid = fg;
        found_afg = true;

        /* Power up the AFG (D0). On real silicon many widgets reply
         * with all-zeros until the AFG is in D0; QEMU doesn't care
         * but does ack the verb cleanly. */
        hda_send_verb(hda_verb12(cad, fg,
                                 HDA_VERB_SET_POWER_STATE, 0x00));
        pit_sleep_ms(10);

        /* Subordinate node count at the AFG = widget range. */
        r = hda_send_verb(hda_verb12(cad, fg,
                                     HDA_VERB_GET_PARAM,
                                     HDA_PARAM_SUBNODE_COUNT));
        if (r == 0xFFFFFFFFu) {
            kprintf("[hda]   AFG @NID %u: SUBNODE_COUNT timeout -- skipping\n",
                    fg);
            break;
        }
        c->widget_start = (uint8_t)(r >> 16);
        c->widget_count = (uint8_t)r;

        if (c->widget_count == 0 || c->widget_count > 64) {
            kprintf("[hda]   AFG @NID %u: implausible widget count %u "
                    "(start=%u) -- skipping\n",
                    fg, c->widget_count, c->widget_start);
            break;
        }

        kprintf("[hda]   AFG @NID %u: widgets %u..%u (%u total)\n",
                fg, c->widget_start,
                c->widget_start + c->widget_count - 1, c->widget_count);

        unsigned w_end = (unsigned)c->widget_start + c->widget_count;
        for (unsigned wu = c->widget_start; wu < w_end; wu++) {
            uint8_t w = (uint8_t)wu;
            r = hda_send_verb(hda_verb12(cad, w,
                                         HDA_VERB_GET_PARAM,
                                         HDA_PARAM_AUDIO_WIDGET_CAP));
            if (r == 0xFFFFFFFFu) continue;
            uint8_t type = (uint8_t)((r >> 20) & 0xFu);
            switch (type) {
            case HDA_WIDGET_TYPE_DAC:
                c->dacs++;
                if (!c->first_dac_nid) c->first_dac_nid = w;
                break;
            case HDA_WIDGET_TYPE_ADC:
                c->adcs++;
                break;
            case HDA_WIDGET_TYPE_MIXER:
                c->mixers++;
                break;
            case HDA_WIDGET_TYPE_SELECTOR:
                c->selectors++;
                break;
            case HDA_WIDGET_TYPE_PIN:
                c->pins++;
                if (!c->first_pin_nid) c->first_pin_nid = w;
                break;
            default:
                c->others++;
                break;
            }
        }

        kprintf("[hda]   widgets: %u DAC, %u ADC, %u PIN, %u MIX, "
                "%u SEL, %u other (first DAC=NID %u, first PIN=NID %u)\n",
                c->dacs, c->adcs, c->pins, c->mixers,
                c->selectors, c->others,
                c->first_dac_nid, c->first_pin_nid);
        break;   /* one AFG per codec is the typical layout */
    }

    if (!found_afg) {
        kprintf("[hda] codec %u: no AFG found, ignoring\n", cad);
        return;
    }

    g_hda.codec_count++;
}

/* Walk every set bit in the post-reset STATESTS snapshot. We rely on
 * the latched value from hda_controller_reset() because STATESTS is
 * RW1C; re-reading it after rings come up can race with stray writes. */
static void hda_codec_scan(void) {
    uint16_t st = g_hda.statests_post_reset;
    uint16_t live = hda_r16(HDA_REG_STATESTS);
    kprintf("[hda] STATESTS=0x%04x (latched post-CRST presence bitmap, live=0x%04x)\n",
            st, live);
    if (!st) {
        kprintf("[hda] no codecs -- attach with QEMU -device hda-duplex,audiodev=<id>\n");
        return;
    }
    for (uint8_t cad = 0; cad < 15; cad++) {
        if (st & (1u << cad)) {
            hda_codec_enumerate(cad);
        }
    }
}

/* ============================================================
 * PCI probe
 * ============================================================ */

static int hda_probe(struct pci_dev *dev) {
    if (g_hda.bound) {
        /* M26F: only the first HDA controller is driven. */
        kprintf("[hda] additional controller at %02x:%02x.%x ignored\n",
                dev->bus, dev->slot, dev->fn);
        return -1;
    }

    /* HDA exposes a 16 KiB MMIO region (BAR0). The Stream Descriptor
     * pages (one per stream, 0x20 B each) start at offset 0x80. With
     * QEMU's intel-hda offering ISS=4 + OSS=4 we end up reading up to
     * offset 0x80 + 8*0x20 = 0x180 -- well inside one 4 KiB page. We
     * map a generous 16 KiB anyway so future stream/BDL work doesn't
     * hit a remap. */
    void *bar = pci_map_bar(dev, 0, 0x4000);
    if (!bar) {
        kprintf("[hda] probe: BAR0 map failed -- skipping\n");
        return -1;
    }
    /* Memory + bus master so DMA from CORB/RIRB and the stream BDLs
     * can flow upstream. */
    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    g_hda.dev   = dev;
    g_hda.mmio  = (volatile uint8_t *)bar;
    g_hda.gcap  = hda_r16(HDA_REG_GCAP);
    g_hda.vmaj  = hda_r8 (HDA_REG_VMAJ);
    g_hda.vmin  = hda_r8 (HDA_REG_VMIN);
    /* GCAP bit unpacking per HDA spec table 4 (see register comment
     * above). The earlier draft of this driver had OSS at bits[15:8]
     * and ISS at bits[7:4]; QEMU reports gcap=0x4401, which only
     * decodes to OSS=4/ISS=4 with the spec's 4-bit nibble layout
     * (the wider layout falsely produced ISS=0 + OSS=68 and made
     * the audio_tone selftest think there were no input streams). */
    g_hda.oss   = (uint8_t)((g_hda.gcap >> 12) & 0xFu);
    g_hda.iss   = (uint8_t)((g_hda.gcap >>  8) & 0xFu);
    g_hda.bss   = (uint8_t)((g_hda.gcap >>  3) & 0x1Fu);
    g_hda.addr64 = (g_hda.gcap & 0x1u) != 0;

    kprintf("[hda] %02x:%02x.%x ver=%u.%u gcap=0x%04x ISS=%u OSS=%u "
            "BSS=%u 64OK=%u\n",
            dev->bus, dev->slot, dev->fn,
            g_hda.vmaj, g_hda.vmin, g_hda.gcap,
            g_hda.iss, g_hda.oss, g_hda.bss, g_hda.addr64);

    if (!hda_controller_reset()) {
        kprintf("[hda] controller reset failed -- aborting bring-up\n");
        return -1;
    }
    kprintf("[hda] controller reset OK\n");

    if (!hda_corb_rirb_init()) {
        kprintf("[hda] CORB/RIRB init failed -- aborting bring-up\n");
        return -1;
    }
    kprintf("[hda] CORB @phys 0x%lx, RIRB @phys 0x%lx (rings live)\n",
            (unsigned long)g_hda.corb_phys,
            (unsigned long)g_hda.rirb_phys);

    hda_codec_scan();
    kprintf("[hda] enumeration complete: %d codec(s)\n", g_hda.codec_count);

    g_hda.bound = true;
    return 0;
}

/* ============================================================
 * Tone playback (best-effort)
 * ============================================================ */

/* Build an 880 Hz square wave, 48 kHz / 16-bit / stereo, into pcm[].
 * 880 Hz is one octave above A4; pleasantly loud and has an integer
 * sample-per-cycle ratio that doesn't beat against the buffer length. */
static void hda_fill_square_wave(int16_t *pcm, uint32_t frames,
                                 uint32_t freq_hz) {
    const uint32_t SR = 48000;
    /* samples_per_cycle = SR / freq_hz; 48000/880 ~= 54.5. Half-period
     * (in samples) = SR / (2 * freq_hz). */
    uint32_t half = SR / (2u * freq_hz);
    if (half == 0) half = 1;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t s = ((i / half) & 1u) ? (int16_t)+8000 : (int16_t)-8000;
        pcm[i * 2 + 0] = s;
        pcm[i * 2 + 1] = s;
    }
}

/* Lazy-allocate the BDL + PCM buffer pages. */
static bool hda_tone_alloc(void) {
    if (g_hda.bdl && g_hda.pcm) return true;

    if (!g_hda.bdl) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return false;
        g_hda.bdl      = (volatile uint32_t *)pmm_phys_to_virt(phys);
        g_hda.bdl_phys = phys;
        memset((void *)g_hda.bdl, 0, 4096);
    }
    if (!g_hda.pcm) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return false;
        g_hda.pcm      = (int16_t *)pmm_phys_to_virt(phys);
        g_hda.pcm_phys = phys;
        memset(g_hda.pcm, 0, 4096);
    }
    return true;
}

/* Fully reset stream descriptor `sdi` (index in the global SD array).
 * After this returns the descriptor is in a known-clean state with
 * RUN=0, all status bits cleared, and SRST released. */
static bool hda_sd_reset(uint32_t sdi) {
    uint32_t off = HDA_SD_BASE + sdi * HDA_SD_STRIDE;

    /* Drop RUN. */
    hda_w8(off + HDA_SD_CTL_LO,
           hda_r8(off + HDA_SD_CTL_LO) & (uint8_t)~HDA_SDCTL_RUN);
    /* Set SRST. */
    hda_w8(off + HDA_SD_CTL_LO,
           hda_r8(off + HDA_SD_CTL_LO) |  (uint8_t)HDA_SDCTL_SRST);
    for (int i = 0; i < 1000; i++) {
        if (hda_r8(off + HDA_SD_CTL_LO) & HDA_SDCTL_SRST) break;
        for (volatile int j = 0; j < 100; j++) { }
    }
    if ((hda_r8(off + HDA_SD_CTL_LO) & HDA_SDCTL_SRST) == 0) {
        kprintf("[hda] SD%u SRST never latched\n", sdi);
        return false;
    }
    /* Clear SRST. */
    hda_w8(off + HDA_SD_CTL_LO,
           hda_r8(off + HDA_SD_CTL_LO) & (uint8_t)~HDA_SDCTL_SRST);
    for (int i = 0; i < 1000; i++) {
        if ((hda_r8(off + HDA_SD_CTL_LO) & HDA_SDCTL_SRST) == 0) break;
        for (volatile int j = 0; j < 100; j++) { }
    }
    /* Clear sticky status bits. */
    hda_w8(off + HDA_SD_STS, 0xFF);
    return true;
}

int audio_hda_tone_selftest(char *msg, size_t cap) {
    if (!g_hda.bound) {
        ksnprintf(msg, cap,
                  "no HDA controller -- run QEMU with -device intel-hda");
        return ABI_DEVT_SKIP;
    }
    if (g_hda.codec_count == 0) {
        ksnprintf(msg, cap,
                  "controller present but no codec -- "
                  "run QEMU with -device hda-output");
        return ABI_DEVT_SKIP;
    }
    if (g_hda.oss == 0) {
        ksnprintf(msg, cap,
                  "controller has 0 output stream descriptors");
        return ABI_DEVT_SKIP;
    }

    /* Pick the first codec that has both a DAC and a PIN. */
    struct audio_hda_codec_info *c = NULL;
    for (int i = 0; i < g_hda.codec_count; i++) {
        if (g_hda.codecs[i].first_dac_nid && g_hda.codecs[i].first_pin_nid) {
            c = &g_hda.codecs[i];
            break;
        }
    }
    if (!c) {
        ksnprintf(msg, cap,
                  "no codec with both DAC and PIN widgets -- skipping");
        return ABI_DEVT_SKIP;
    }

    if (!hda_tone_alloc()) {
        ksnprintf(msg, cap, "tone DMA alloc failed");
        return -ABI_EIO;
    }

    g_hda.tone_attempted = true;

    /* Fill the buffer with one 880 Hz square wave period; 1024 stereo
     * frames @ 48 kHz = ~21.3 ms of audio, looped by LVI=0. */
    const uint32_t FRAMES = 1024;
    const uint32_t BYTES  = FRAMES * 4;       /* stereo * 16-bit */
    hda_fill_square_wave(g_hda.pcm, FRAMES, 880);

    /* Build a single-entry BDL pointing at the PCM buffer. The HDA
     * spec requires BDL entries to be 16 B each and the BDL itself
     * 128 B aligned -- pmm_alloc_page() gives us 4 KiB alignment
     * which is more than enough. */
    g_hda.bdl[0] = (uint32_t)(g_hda.pcm_phys & 0xFFFFFFFFu);
    g_hda.bdl[1] = (uint32_t)(g_hda.pcm_phys >> 32);
    g_hda.bdl[2] = BYTES;
    g_hda.bdl[3] = 0;                         /* IOC=0, no interrupt */

    /* Output stream descriptors live at indices ISS..ISS+OSS-1. We
     * use the first one. Stream tag 1 (must be != 0). */
    uint32_t sdi = g_hda.iss;
    uint32_t off = HDA_SD_BASE + sdi * HDA_SD_STRIDE;

    if (!hda_sd_reset(sdi)) {
        ksnprintf(msg, cap, "SD%u reset wedged", sdi);
        return -ABI_EIO;
    }

    /* Program format BEFORE BDL/CBL/LVI. Spec says CBL and FMT must
     * be set while RUN=0 -- which it is, post-SRST. */
    hda_w16(off + HDA_SD_FMT,  0x0011);       /* 48 kHz / 16 bit / 2 ch */
    hda_w32(off + HDA_SD_CBL,  BYTES);
    hda_w16(off + HDA_SD_LVI,  0);            /* one BDL entry          */
    hda_w32(off + HDA_SD_BDPL, (uint32_t)(g_hda.bdl_phys & 0xFFFFFFFFu));
    hda_w32(off + HDA_SD_BDPU, (uint32_t)(g_hda.bdl_phys >> 32));

    /* Read-back sanity. The chip mirrors writes when RUN=0; if the
     * read-back doesn't match we're talking to dead silicon. */
    if (hda_r32(off + HDA_SD_BDPL) != (uint32_t)(g_hda.bdl_phys & 0xFFFFFFFFu) ||
        hda_r32(off + HDA_SD_CBL)  != BYTES) {
        ksnprintf(msg, cap, "SD%u register read-back mismatch", sdi);
        return -ABI_EIO;
    }

    /* Program stream tag in SDCTL byte 2 (high nibble), then RUN. The
     * 2-step write keeps the controller from trying to fetch with an
     * uninitialised tag. */
    hda_w8(off + HDA_SD_CTL_HI, (uint8_t)(1u << 4));   /* stream tag = 1 */

    /* Codec verbs to point the DAC at our stream and unmute the PIN.
     *   1) Set Stream/Channel: stream tag 1 (high nibble), channel 0.
     *   2) Set Format: same as SDxFMT.
     *   3) Set Amp Gain/Mute: OUT path, both L+R, gain 0x7F (max).
     *      Format word: bit15=OUT, bit14=IN, bit13=L, bit12=R, bits[6:0]=gain.
     *   4) Set Pin Widget Control: OUT_EN | HP_EN.
     *   5) Set EAPD/BTL Enable: 0x02 (EAPD bit). Many real chips need
     *      this even for line-out; QEMU happily acks. */
    hda_send_verb(hda_verb12(c->cad, c->first_dac_nid,
                             HDA_VERB_SET_STREAM_CHAN, 0x10));
    hda_send_verb(hda_verb4 (c->cad, c->first_dac_nid,
                             HDA_VERB_SHORT_SET_FORMAT, 0x0011));
    hda_send_verb(hda_verb4 (c->cad, c->first_dac_nid,
                             HDA_VERB_SHORT_SET_AMP_GAIN, 0xB07Fu));
    hda_send_verb(hda_verb12(c->cad, c->first_pin_nid,
                             HDA_VERB_SET_PIN_CTL,
                             HDA_PIN_CTL_OUT_EN | HDA_PIN_CTL_HP_EN));
    hda_send_verb(hda_verb12(c->cad, c->first_pin_nid,
                             HDA_VERB_SET_EAPD_BTL, 0x02));

    /* Flip RUN. */
    hda_w8(off + HDA_SD_CTL_LO,
           hda_r8(off + HDA_SD_CTL_LO) | (uint8_t)HDA_SDCTL_RUN);
    g_hda.streams_started++;
    g_hda.tone_started = true;

    /* Let the buffer cycle ~12 times so the audio backend has a chance
     * to actually emit audible samples. 250 ms at 21 ms/loop = ~12. */
    pit_sleep_ms(250);

    /* Drop RUN. */
    hda_w8(off + HDA_SD_CTL_LO,
           hda_r8(off + HDA_SD_CTL_LO) & (uint8_t)~HDA_SDCTL_RUN);

    /* Mute the pin again so we don't leave it open after the test. */
    hda_send_verb(hda_verb12(c->cad, c->first_pin_nid,
                             HDA_VERB_SET_PIN_CTL, 0));

    uint32_t lpib = hda_r32(off + HDA_SD_LPIB);
    ksnprintf(msg, cap,
              "tone @880Hz, SD%u stream1 fmt=0x0011 cbl=%u LPIB=%u "
              "(verbs sent=%lu replied=%lu timeouts=%lu)",
              sdi, BYTES, lpib,
              (unsigned long)g_hda.verbs_sent,
              (unsigned long)g_hda.verbs_replied,
              (unsigned long)g_hda.verb_timeouts);
    return 0;
}

/* ============================================================
 * Public API
 * ============================================================ */

static const struct pci_match g_hda_matches[] = {
    { PCI_ANY_ID, PCI_ANY_ID,
      PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_HDA,         PCI_ANY_CLASS },
    { PCI_ANY_ID, PCI_ANY_ID,
      PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIODEVICE, PCI_ANY_CLASS },
    PCI_MATCH_END,
};

static struct pci_driver g_hda_driver = {
    .name    = "audio_hda",
    .matches = g_hda_matches,
    .probe   = hda_probe,
    .remove  = 0,
    ._next   = 0,
};

void audio_hda_register(void) {
    pci_register_driver(&g_hda_driver);
}

bool audio_hda_present(void) {
    return g_hda.bound;
}

int audio_hda_codec_count(void) {
    return g_hda.codec_count;
}

bool audio_hda_codec_at(int i, struct audio_hda_codec_info *out) {
    if (!out) return false;
    if (i < 0 || i >= g_hda.codec_count) return false;
    *out = g_hda.codecs[i];
    return true;
}

/* Devlist record(s):
 *   recs[0] = the controller itself
 *   recs[1..] = one per codec, in enumeration order
 *
 * We bound the codec records by `cap` so a small `cap` still reports
 * the controller. */
int audio_hda_introspect(struct abi_dev_info *out, int cap) {
    if (!g_hda.bound || cap <= 0 || !out) return 0;

    int n = 0;

    /* ---- controller record ---- */
    {
        struct abi_dev_info *r = &out[n];
        memset(r, 0, sizeof(*r));
        r->bus     = ABI_DEVT_BUS_AUDIO;
        r->status  = ABI_DEVT_PRESENT | ABI_DEVT_BOUND;
        if (g_hda.tone_started) r->status |= ABI_DEVT_ACTIVE;
        r->vendor     = g_hda.dev ? g_hda.dev->vendor : 0;
        r->device     = g_hda.dev ? g_hda.dev->device : 0;
        r->class_code = PCI_CLASS_MULTIMEDIA;
        r->subclass   = PCI_SUBCLASS_HDA;
        r->index      = 0;
        const char *nm = "hda0";
        size_t k = 0;
        while (nm[k] && k + 1 < ABI_DEVT_NAME_MAX) {
            r->name[k] = nm[k]; k++;
        }
        r->name[k] = '\0';
        const char *dn = "audio_hda";
        k = 0;
        while (dn[k] && k + 1 < ABI_DEVT_DRIVER_MAX) {
            r->driver[k] = dn[k]; k++;
        }
        r->driver[k] = '\0';
        ksnprintf(r->extra, ABI_DEVT_EXTRA_MAX,
                  "ver=%u.%u OSS=%u ISS=%u codecs=%d verbs=%lu/%lu",
                  g_hda.vmaj, g_hda.vmin, g_hda.oss, g_hda.iss,
                  g_hda.codec_count,
                  (unsigned long)g_hda.verbs_replied,
                  (unsigned long)g_hda.verbs_sent);
        n++;
    }

    /* ---- one record per codec ---- */
    for (int i = 0; i < g_hda.codec_count && n < cap; i++) {
        const struct audio_hda_codec_info *c = &g_hda.codecs[i];
        struct abi_dev_info *r = &out[n];
        memset(r, 0, sizeof(*r));
        r->bus     = ABI_DEVT_BUS_AUDIO;
        r->status  = ABI_DEVT_PRESENT | ABI_DEVT_BOUND;
        r->vendor     = c->vendor;
        r->device     = c->device;
        r->class_code = PCI_CLASS_MULTIMEDIA;
        r->subclass   = PCI_SUBCLASS_HDA;
        r->index      = (uint16_t)(i + 1);
        ksnprintf(r->name, ABI_DEVT_NAME_MAX, "hda0-codec%u", c->cad);
        const char *dn = "hda_codec";
        size_t k = 0;
        while (dn[k] && k + 1 < ABI_DEVT_DRIVER_MAX) {
            r->driver[k] = dn[k]; k++;
        }
        r->driver[k] = '\0';
        ksnprintf(r->extra, ABI_DEVT_EXTRA_MAX,
                  "afg=%u dac=%u adc=%u pin=%u mix=%u",
                  c->afg_nid, c->dacs, c->adcs, c->pins, c->mixers);
        n++;
    }

    return n;
}

int audio_hda_selftest(char *msg, size_t cap) {
    if (!g_hda.bound) {
        ksnprintf(msg, cap,
                  "no HDA controller present (run with -device intel-hda)");
        return ABI_DEVT_SKIP;
    }
    if (g_hda.gcap == 0xFFFF || g_hda.vmaj == 0xFF) {
        ksnprintf(msg, cap,
                  "controller bound but registers read as 0xFF (BAR map?)");
        return -ABI_EIO;
    }
    if (!g_hda.rings_up) {
        ksnprintf(msg, cap, "controller bound but CORB/RIRB never came up");
        return -ABI_EIO;
    }
    if (g_hda.codec_count == 0) {
        ksnprintf(msg, cap,
                  "controller OK (ver=%u.%u gcap=0x%04x ISS=%u OSS=%u) "
                  "but no codec -- run QEMU with -device hda-output",
                  g_hda.vmaj, g_hda.vmin, g_hda.gcap, g_hda.iss, g_hda.oss);
        return ABI_DEVT_SKIP;
    }
    if (g_hda.verb_timeouts > 0) {
        ksnprintf(msg, cap,
                  "controller bound but %lu verb timeout(s) seen -- "
                  "codec may be wedged",
                  (unsigned long)g_hda.verb_timeouts);
        return -ABI_EIO;
    }
    ksnprintf(msg, cap,
              "HDA ver=%u.%u gcap=0x%04x ISS=%u OSS=%u %d codec(s) "
              "verbs=%lu/%lu",
              g_hda.vmaj, g_hda.vmin, g_hda.gcap,
              g_hda.iss, g_hda.oss, g_hda.codec_count,
              (unsigned long)g_hda.verbs_replied,
              (unsigned long)g_hda.verbs_sent);
    return 0;
}
