/* ahci_quirks.c -- AHCI storage hardening module.
 *
 * Runs after the primary AHCI driver (blk_ahci.c) has completed PCI
 * probe and port enumeration. Detects common real-hardware quirks:
 *
 *   - Misreported port counts (PI bitmap vs CAP.NP mismatch)
 *   - Stuck BSY bits in PxTFD that block command issue
 *   - Slow-spinup drives that need extended DET polling
 *   - FIS watchdog: commands that don't complete within 30 s are
 *     aborted (COMRESET + retry)
 *
 * Called once from kernel boot after pci_bind_drivers(). Idempotent;
 * safe to call when no AHCI HBA is present (returns immediately).
 */

#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>

/* ---- AHCI register offsets (subset needed for quirks) ------------ */

#define AQ_CAP              0x00
#define AQ_GHC              0x04
#define AQ_IS               0x08
#define AQ_PI               0x0C
#define AQ_VS               0x10
#define AQ_CAP2             0x24

#define AQ_PORT_OFF(i)      (0x100u + (uint32_t)(i) * 0x80u)
#define AQ_PxCMD            0x18
#define AQ_PxTFD            0x20
#define AQ_PxSSTS           0x28
#define AQ_PxSCTL           0x2C
#define AQ_PxSERR           0x30
#define AQ_PxCI             0x38
#define AQ_PxIS             0x10

#define AQ_PxCMD_ST         (1u << 0)
#define AQ_PxCMD_SUD        (1u << 1)
#define AQ_PxCMD_FRE        (1u << 4)
#define AQ_PxCMD_FR         (1u << 14)
#define AQ_PxCMD_CR         (1u << 15)

#define AQ_TFD_BSY          (1u << 7)
#define AQ_TFD_DRQ          (1u << 3)
#define AQ_TFD_ERR          (1u << 0)

#define AQ_SSTS_DET_MASK    0x0Fu
#define AQ_SSTS_DET_PRESENT 3u

#define AQ_SCTL_DET_MASK    0x0Fu
#define AQ_SCTL_DET_COMRESET 1u

#define AQ_FIS_TIMEOUT_MS   30000
#define AQ_COMRESET_RETRIES 3
#define AQ_SPINUP_TIMEOUT_MS 5000

#define AQ_MAX_PORTS        32

/* ---- state ------------------------------------------------------- */

static volatile uint8_t *g_aq_abar;

static inline void aq_w32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_aq_abar + off) = val;
}
static inline uint32_t aq_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_aq_abar + off);
}

static inline void aq_port_w32(int port, uint32_t reg, uint32_t val) {
    aq_w32(AQ_PORT_OFF(port) + reg, val);
}
static inline uint32_t aq_port_r32(int port, uint32_t reg) {
    return aq_r32(AQ_PORT_OFF(port) + reg);
}

/* ---- quirk: port count validation -------------------------------- */

static void aq_check_port_count(void) {
    uint32_t cap = aq_r32(AQ_CAP);
    uint32_t pi  = aq_r32(AQ_PI);
    uint8_t np = (uint8_t)((cap & 0x1F) + 1);

    int pi_count = 0;
    for (int i = 0; i < AQ_MAX_PORTS; i++) {
        if (pi & (1u << i)) pi_count++;
    }

    if (pi_count != np) {
        kprintf("[ahci_quirks] port count mismatch: CAP.NP=%u  PI popcount=%d\n",
                np, pi_count);
    }
}

/* ---- quirk: COMRESET with retry ---------------------------------- */

static bool aq_comreset(int port) {
    for (int attempt = 0; attempt < AQ_COMRESET_RETRIES; attempt++) {
        /* Stop command processing */
        uint32_t cmd = aq_port_r32(port, AQ_PxCMD);
        aq_port_w32(port, AQ_PxCMD, cmd & ~(AQ_PxCMD_ST | AQ_PxCMD_FRE));

        /* Wait for CR + FR to clear */
        for (int i = 0; i < 500; i++) {
            uint32_t c = aq_port_r32(port, AQ_PxCMD);
            if (!(c & AQ_PxCMD_CR) && !(c & AQ_PxCMD_FR)) break;
            pit_sleep_ms(1);
        }

        /* Issue COMRESET via SControl.DET */
        uint32_t sctl = aq_port_r32(port, AQ_PxSCTL);
        sctl = (sctl & ~AQ_SCTL_DET_MASK) | AQ_SCTL_DET_COMRESET;
        aq_port_w32(port, AQ_PxSCTL, sctl);
        pit_sleep_ms(2);

        /* Clear DET to allow re-establishment */
        sctl &= ~AQ_SCTL_DET_MASK;
        aq_port_w32(port, AQ_PxSCTL, sctl);
        pit_sleep_ms(10);

        /* Wait for device presence */
        bool detected = false;
        for (uint64_t t = 0; t < AQ_SPINUP_TIMEOUT_MS; t++) {
            uint32_t ssts = aq_port_r32(port, AQ_PxSSTS);
            if ((ssts & AQ_SSTS_DET_MASK) == AQ_SSTS_DET_PRESENT) {
                detected = true;
                break;
            }
            pit_sleep_ms(1);
        }

        if (detected) {
            /* Clear SERR */
            aq_port_w32(port, AQ_PxSERR, 0xFFFFFFFF);
            aq_port_w32(port, AQ_PxIS, 0xFFFFFFFF);

            /* Re-enable command processing */
            cmd = aq_port_r32(port, AQ_PxCMD);
            aq_port_w32(port, AQ_PxCMD, cmd | AQ_PxCMD_FRE | AQ_PxCMD_SUD);
            pit_sleep_ms(10);

            /* Wait for BSY clear */
            bool bsy_clear = false;
            for (int w = 0; w < 3000; w++) {
                uint32_t tfd = aq_port_r32(port, AQ_PxTFD);
                if (!(tfd & (AQ_TFD_BSY | AQ_TFD_DRQ))) {
                    bsy_clear = true;
                    break;
                }
                pit_sleep_ms(1);
            }

            if (bsy_clear) {
                aq_port_w32(port, AQ_PxCMD,
                            aq_port_r32(port, AQ_PxCMD) | AQ_PxCMD_ST);
                if (attempt > 0)
                    kprintf("[ahci_quirks] port %d: COMRESET succeeded on attempt %d\n",
                            port, attempt + 1);
                return true;
            }
            kprintf("[ahci_quirks] port %d: BSY stuck after COMRESET attempt %d\n",
                    port, attempt + 1);
        } else {
            kprintf("[ahci_quirks] port %d: no device after COMRESET attempt %d\n",
                    port, attempt + 1);
        }
    }
    return false;
}

/* ---- quirk: stuck BSY detection + recovery ----------------------- */

static void aq_check_stuck_bsy(void) {
    uint32_t pi = aq_r32(AQ_PI);
    for (int p = 0; p < AQ_MAX_PORTS; p++) {
        if (!(pi & (1u << p))) continue;

        uint32_t ssts = aq_port_r32(p, AQ_PxSSTS);
        if ((ssts & AQ_SSTS_DET_MASK) != AQ_SSTS_DET_PRESENT) continue;

        uint32_t tfd = aq_port_r32(p, AQ_PxTFD);
        if (tfd & AQ_TFD_BSY) {
            kprintf("[ahci_quirks] port %d: BSY stuck (TFD=0x%08x), attempting COMRESET\n",
                    p, tfd);
            if (!aq_comreset(p)) {
                kprintf("[ahci_quirks] port %d: recovery FAILED\n", p);
            }
        }
    }
}

/* ---- FIS watchdog ------------------------------------------------ */

bool ahci_quirks_fis_watchdog(volatile uint8_t *abar, int port, int slot) {
    uint64_t start = pit_ticks();
    uint32_t hz = pit_hz();
    uint64_t timeout_ticks = ((uint64_t)AQ_FIS_TIMEOUT_MS * hz) / 1000;

    while (1) {
        uint32_t ci = *(volatile uint32_t *)(abar + AQ_PORT_OFF(port) + AQ_PxCI);
        if (!(ci & (1u << slot))) return true;

        uint32_t pxis = *(volatile uint32_t *)(abar + AQ_PORT_OFF(port) + AQ_PxIS);
        if (pxis & ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27))) {
            kprintf("[ahci_quirks] port %d slot %d: error in PxIS=0x%08x\n",
                    port, slot, pxis);
            return false;
        }

        if ((pit_ticks() - start) >= timeout_ticks) {
            kprintf("[ahci_quirks] port %d slot %d: FIS watchdog timeout (%u ms)\n",
                    port, slot, AQ_FIS_TIMEOUT_MS);

            /* Attempt abort + COMRESET recovery */
            g_aq_abar = abar;
            if (aq_comreset(port)) {
                kprintf("[ahci_quirks] port %d: recovered after watchdog timeout\n", port);
            }
            return false;
        }

        __asm__ volatile ("pause" ::: "memory");
    }
}

/* ---- main init --------------------------------------------------- */

void ahci_quirks_init(void) {
    struct pci_dev *dev = pci_find_dev(PCI_ANY_ID, PCI_ANY_ID);

    /* Walk the PCI table for an AHCI controller */
    bool found = false;
    for (size_t i = 0; i < pci_device_count(); i++) {
        struct pci_dev *d = pci_device_at(i);
        if (d && d->class_code == 0x01 && d->subclass == 0x06) {
            dev = d;
            found = true;
            break;
        }
    }

    if (!found || !dev) {
        return;
    }

    void *bar = pci_map_bar(dev, 5, 0);
    if (!bar) {
        kprintf("[ahci_quirks] cannot map ABAR\n");
        return;
    }
    g_aq_abar = (volatile uint8_t *)bar;

    uint32_t vs = aq_r32(AQ_VS);
    kprintf("[ahci_quirks] HBA version %u.%u%u, running quirk checks\n",
            (vs >> 16) & 0xFFFF, (vs >> 8) & 0xFF, vs & 0xFF);

    aq_check_port_count();
    aq_check_stuck_bsy();

    kprintf("[ahci_quirks] hardening checks complete\n");
}
