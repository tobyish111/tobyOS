/* acpi_bat.h -- ACPI control-method battery presence + status (M26G).
 *
 * What this driver does (and doesn't):
 *
 *   DOES:
 *     - Heuristic detection of an ACPI battery in DSDT/SSDT bytecode
 *       by searching for the literal "PNP0C0A" _HID string. This
 *       tells us a battery EXISTS on the platform but says nothing
 *       about its current charge/state -- those live behind the
 *       _BIF (Battery Information) and _BST (Battery Status)
 *       methods, which require a real AML interpreter to evaluate.
 *
 *     - Optional mock injection via QEMU fw_cfg
 *       ("opt/tobyos/battery_mock"). When present, the mock
 *       overrides the heuristic and lets automated tests exercise
 *       the present/charging/percent path on QEMU x86, which has
 *       no native ACPI battery emulation.
 *
 *   DOES NOT:
 *     - Ship an AML interpreter. Real _BIF/_BST evaluation is a
 *       future-milestone task (think of this as the M26G groundwork
 *       for an ACPICA-style port). Until then, a heuristic-only hit
 *       is reported as "present, charge unknown" with a clear
 *       diagnostic in the introspect record's "extra" field.
 *
 * Public API matches the M26A stub so devtest/userland don't need
 * to know which mode is active. */

#ifndef TOBYOS_ACPI_BAT_H
#define TOBYOS_ACPI_BAT_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Charging state values reported by _BST or by the fw_cfg mock. */
#define ACPI_BAT_STATE_UNKNOWN     0
#define ACPI_BAT_STATE_DISCHARGING 1
#define ACPI_BAT_STATE_CHARGING    2
#define ACPI_BAT_STATE_FULL        3

/* How we learned about the battery. Influences the diagnostics
 * string and the selftest verdict (UNKNOWN-only-from-DSDT is a
 * "PASS with caveat", not a FAIL). */
#define ACPI_BAT_SOURCE_NONE        0   /* nothing detected      */
#define ACPI_BAT_SOURCE_DSDT_NAME   1   /* PNP0C0A found in AML  */
#define ACPI_BAT_SOURCE_FW_CFG_MOCK 2   /* fw_cfg mock injected  */

/* Snapshot of the system's first battery (we only support one). */
struct acpi_bat_status {
    bool     present;        /* false = no battery in this machine */
    uint8_t  source;         /* ACPI_BAT_SOURCE_*                   */
    uint8_t  state;          /* ACPI_BAT_STATE_*                    */
    uint8_t  percent;        /* 0..100; 0xFF if unknown             */
    uint32_t design_mwh;     /* design capacity in mWh, 0 if unk    */
    uint32_t remaining_mwh;  /* current remaining capacity, 0 if unk */
    uint32_t rate_mw;        /* present rate in mW, 0 if unknown    */
    /* Where in the AML inventory we found the _HID match. Only
     * meaningful when source == DSDT_NAME. table 0 = DSDT, 1+ =
     * SSDT[i-1]. Diagnostics only -- not exposed to userland. */
    int      hit_table;
    uint32_t hit_offset;
};

/* Run discovery. Order: fw_cfg mock first (so test harnesses can
 * deterministically force a battery present), then heuristic AML
 * scan. Idempotent. Safe to call before or after acpi_init() (the
 * AML scan just no-ops if g_info.dsdt.len == 0). */
void acpi_bat_init(void);

/* True if at least one battery was detected on this boot. */
bool acpi_bat_present(void);

/* Re-read the cached status. For mock entries this just copies the
 * cache. For DSDT-name-only entries this does nothing (we can't
 * re-evaluate _BST without an AML interpreter). Returns 0 on a hit
 * (out filled in), -ABI_ENOENT when no battery is present. */
int  acpi_bat_refresh(struct acpi_bat_status *out);

/* Fill an abi_dev_info record describing the battery. Returns the
 * number written (0 or 1). devtest_enumerate calls this. */
int  acpi_bat_introspect(struct abi_dev_info *out, int cap);

/* Self-test verdict:
 *   ABI_DEVT_SKIP -- no battery present (expected on most desktops)
 *   0             -- battery present and cached values look sane
 *  -ABI_EIO       -- present but cached values are obviously wrong
 *                    (percent > 100, etc.) */
int  acpi_bat_selftest(char *msg, size_t cap);

#endif /* TOBYOS_ACPI_BAT_H */
