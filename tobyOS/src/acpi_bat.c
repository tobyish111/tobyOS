/* acpi_bat.c -- ACPI battery driver (M26G).
 *
 * Two cooperating discovery paths feed g_bat:
 *
 *   1. fw_cfg mock ("opt/tobyos/battery_mock"). This is checked
 *      first so the M26G test harness can force a deterministic
 *      battery state on QEMU x86, which has no native ACPI battery
 *      device. Format: key=value pairs separated by commas or
 *      whitespace. Recognised keys (all optional, with sane
 *      defaults):
 *
 *          present=0|1            -- 1 = inject; 0 = explicit absent
 *          state=charging|discharging|full|unknown
 *          percent=NN             -- 0..100
 *          design=NNNN            -- design capacity, mWh
 *          remaining=NNNN         -- remaining capacity, mWh
 *          rate=NNNN              -- present rate, mW
 *
 *      Example QEMU command-line:
 *
 *          -fw_cfg name=opt/tobyos/battery_mock,\
 *                  string=state=charging,percent=75,\
 *                          design=50000,remaining=37500,rate=1500
 *
 *      Any present=1 (or omission of present=) implies "battery
 *      present" so testers don't have to type that prefix.
 *
 *   2. Heuristic AML byte scan. We search every DSDT/SSDT for the
 *      literal ASCII "PNP0C0A" -- the spec-mandated _HID for a
 *      control-method battery. A hit means the firmware DECLARED a
 *      battery; it does NOT mean we can read its current charge
 *      (that needs an AML interpreter to evaluate _BIF / _BST).
 *      We surface this case as "present, charge unknown" with a
 *      clear note in the introspect record's "extra" column.
 *
 * Mock takes precedence: if both fire, the user explicitly asked
 * for the mock and probably wants its values.
 *
 * We track exactly one battery (most laptops have one, some have
 * two; ignoring the second is a documented limitation). */

#include <tobyos/acpi_bat.h>
#include <tobyos/acpi.h>
#include <tobyos/fw_cfg.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

static struct acpi_bat_status g_bat;

/* ---- mock-string parser ------------------------------------------ *
 *
 * Tokeniser walks the string left-to-right, recognising "key=value"
 * pairs separated by commas, semicolons, or whitespace. Values are
 * either small integers or fixed enum tags. Unknown keys are
 * silently ignored so we can extend the format without breaking
 * older mock blobs. */

static bool is_sep(char c) {
    return c == ',' || c == ';' || c == ' ' || c == '\t' ||
           c == '\n' || c == '\r';
}

static bool key_eq(const char *p, size_t plen, const char *k) {
    size_t klen = 0; while (k[klen]) klen++;
    if (plen != klen) return false;
    for (size_t i = 0; i < plen; i++) {
        char a = p[i], b = k[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static uint32_t parse_uint(const char *p, size_t len) {
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') break;
        v = v * 10u + (uint32_t)(p[i] - '0');
        if (v > 0x7FFFFFFFu) break;
    }
    return v;
}

static uint8_t parse_state_tag(const char *p, size_t len) {
    if (key_eq(p, len, "charging"))     return ACPI_BAT_STATE_CHARGING;
    if (key_eq(p, len, "discharging"))  return ACPI_BAT_STATE_DISCHARGING;
    if (key_eq(p, len, "full"))         return ACPI_BAT_STATE_FULL;
    if (key_eq(p, len, "unknown"))      return ACPI_BAT_STATE_UNKNOWN;
    return ACPI_BAT_STATE_UNKNOWN;
}

/* Parse `text`, populate `out`. Returns true if any key was
 * recognised AND the result represents a usable battery (so the
 * caller can decide whether to commit). present=0 returns true with
 * out->present == false so the caller can still note "explicit
 * absent" in the log. */
static bool parse_mock(const char *text, size_t len,
                       struct acpi_bat_status *out) {
    bool any = false;
    bool present = true;            /* default if no present= given */
    out->state         = ACPI_BAT_STATE_UNKNOWN;
    out->percent       = 0xFF;
    out->design_mwh    = 0;
    out->remaining_mwh = 0;
    out->rate_mw       = 0;

    size_t i = 0;
    while (i < len) {
        while (i < len && is_sep(text[i])) i++;
        if (i >= len) break;
        size_t k0 = i;
        while (i < len && text[i] != '=' && !is_sep(text[i])) i++;
        if (i >= len || text[i] != '=') {
            while (i < len && !is_sep(text[i])) i++;
            continue;
        }
        size_t klen = i - k0;
        i++;
        size_t v0 = i;
        while (i < len && !is_sep(text[i])) i++;
        size_t vlen = i - v0;

        const char *k = &text[k0];
        const char *v = &text[v0];

        if (key_eq(k, klen, "present")) {
            present = (vlen > 0 && v[0] != '0');
            any = true;
        } else if (key_eq(k, klen, "state")) {
            out->state = parse_state_tag(v, vlen);
            any = true;
        } else if (key_eq(k, klen, "percent")) {
            uint32_t p = parse_uint(v, vlen);
            if (p > 100u) p = 100u;
            out->percent = (uint8_t)p;
            any = true;
        } else if (key_eq(k, klen, "design")) {
            out->design_mwh = parse_uint(v, vlen);
            any = true;
        } else if (key_eq(k, klen, "remaining")) {
            out->remaining_mwh = parse_uint(v, vlen);
            any = true;
        } else if (key_eq(k, klen, "rate")) {
            out->rate_mw = parse_uint(v, vlen);
            any = true;
        }
    }
    out->present = present;
    return any;
}

/* ---- public init ------------------------------------------------- */

void acpi_bat_init(void) {
    memset(&g_bat, 0, sizeof(g_bat));
    g_bat.present = false;
    g_bat.source  = ACPI_BAT_SOURCE_NONE;
    g_bat.state   = ACPI_BAT_STATE_UNKNOWN;
    g_bat.percent = 0xFF;

    /* (1) fw_cfg mock first. */
    if (fw_cfg_present()) {
        char buf[256];
        int  n = fw_cfg_read_file("opt/tobyos/battery_mock",
                                  buf, sizeof(buf) - 1);
        if (n > 0) {
            struct acpi_bat_status mock;
            memset(&mock, 0, sizeof(mock));
            if (parse_mock(buf, (size_t)n, &mock)) {
                if (mock.present) {
                    mock.source = ACPI_BAT_SOURCE_FW_CFG_MOCK;
                    g_bat = mock;
                    kprintf("[battery] fw_cfg mock injected: "
                            "state=%u percent=%u design=%u "
                            "remaining=%u rate=%u\n",
                            (unsigned)g_bat.state, (unsigned)g_bat.percent,
                            (unsigned)g_bat.design_mwh,
                            (unsigned)g_bat.remaining_mwh,
                            (unsigned)g_bat.rate_mw);
                    return;
                } else {
                    kprintf("[battery] fw_cfg mock present but present=0 -- "
                            "treating as absent\n");
                    /* Don't return; allow heuristic scan to override
                     * if the host AML actually has a battery. */
                }
            } else {
                kprintf("[battery] fw_cfg mock blob unrecognised "
                        "(%d bytes) -- ignoring\n", n);
            }
        }
    }

    /* (2) Heuristic AML byte scan for "PNP0C0A". */
    static const char k_hid_battery[7] = { 'P','N','P','0','C','0','A' };
    int      where = -1;
    uint32_t off = 0;
    if (acpi_aml_find_bytes(k_hid_battery, sizeof k_hid_battery,
                            &where, &off)) {
        g_bat.present    = true;
        g_bat.source     = ACPI_BAT_SOURCE_DSDT_NAME;
        g_bat.state      = ACPI_BAT_STATE_UNKNOWN;
        g_bat.percent    = 0xFF;
        g_bat.hit_table  = where;
        g_bat.hit_offset = off;
        kprintf("[battery] PNP0C0A detected in %s @ offset 0x%x "
                "(charge unknown without AML interpreter)\n",
                where == 0 ? "DSDT" : "SSDT", (unsigned)off);
        return;
    }

    kprintf("[battery] no ACPI battery detected (no fw_cfg mock, "
            "no PNP0C0A in AML)\n");
}

bool acpi_bat_present(void) {
    return g_bat.present;
}

int acpi_bat_refresh(struct acpi_bat_status *out) {
    if (!g_bat.present) return -ABI_ENOENT;
    if (out) *out = g_bat;
    /* For the mock + DSDT-name paths there's no live re-read to do.
     * A real M27 _BST evaluation would update g_bat in place here. */
    return 0;
}

/* Translate state enum -> short tag for log strings. */
static const char *state_tag(uint8_t s) {
    switch (s) {
    case ACPI_BAT_STATE_DISCHARGING: return "discharging";
    case ACPI_BAT_STATE_CHARGING:    return "charging";
    case ACPI_BAT_STATE_FULL:        return "full";
    default:                         return "unknown";
    }
}

int acpi_bat_introspect(struct abi_dev_info *out, int cap) {
    if (cap <= 0 || !out) return 0;
    if (!g_bat.present) return 0;

    memset(out, 0, sizeof(*out));
    out->bus    = ABI_DEVT_BUS_BATTERY;
    out->status = ABI_DEVT_PRESENT;
    out->index  = 0;

    const char *nm = "battery0";
    size_t n = 0;
    while (nm[n] && n + 1 < ABI_DEVT_NAME_MAX) { out->name[n] = nm[n]; n++; }
    out->name[n] = '\0';

    const char *dn = (g_bat.source == ACPI_BAT_SOURCE_FW_CFG_MOCK)
                     ? "acpi_bat(mock)" : "acpi_bat";
    n = 0;
    while (dn[n] && n + 1 < ABI_DEVT_DRIVER_MAX) { out->driver[n] = dn[n]; n++; }
    out->driver[n] = '\0';

    if (g_bat.source == ACPI_BAT_SOURCE_DSDT_NAME) {
        ksnprintf(out->extra, ABI_DEVT_EXTRA_MAX,
                  "PNP0C0A in %s; charge unknown (no AML interp)",
                  g_bat.hit_table == 0 ? "DSDT" : "SSDT");
    } else if (g_bat.percent == 0xFF) {
        ksnprintf(out->extra, ABI_DEVT_EXTRA_MAX,
                  "%s (charge unknown)", state_tag(g_bat.state));
    } else if (g_bat.design_mwh && g_bat.remaining_mwh) {
        ksnprintf(out->extra, ABI_DEVT_EXTRA_MAX,
                  "%u%% %s (%u/%u mWh, %u mW)",
                  (unsigned)g_bat.percent, state_tag(g_bat.state),
                  (unsigned)g_bat.remaining_mwh,
                  (unsigned)g_bat.design_mwh,
                  (unsigned)g_bat.rate_mw);
    } else {
        ksnprintf(out->extra, ABI_DEVT_EXTRA_MAX, "%u%% %s",
                  (unsigned)g_bat.percent, state_tag(g_bat.state));
    }
    return 1;
}

int acpi_bat_selftest(char *msg, size_t cap) {
    if (!g_bat.present) {
        ksnprintf(msg, cap,
                  "no ACPI battery detected (desktop or QEMU default)");
        return ABI_DEVT_SKIP;
    }

    /* Sanity-check cached values. percent==0xFF is "unknown" and OK. */
    if (g_bat.percent != 0xFF && g_bat.percent > 100) {
        ksnprintf(msg, cap, "cached battery %% out of range (%u)",
                  (unsigned)g_bat.percent);
        return -ABI_EIO;
    }
    if (g_bat.state > ACPI_BAT_STATE_FULL) {
        ksnprintf(msg, cap, "cached battery state out of range (%u)",
                  (unsigned)g_bat.state);
        return -ABI_EIO;
    }
    /* Capacity sanity: remaining shouldn't exceed design when both
     * are non-zero. */
    if (g_bat.design_mwh && g_bat.remaining_mwh &&
        g_bat.remaining_mwh > g_bat.design_mwh) {
        ksnprintf(msg, cap,
                  "remaining (%u mWh) > design (%u mWh) -- mock data "
                  "looks wrong", (unsigned)g_bat.remaining_mwh,
                  (unsigned)g_bat.design_mwh);
        return -ABI_EIO;
    }

    if (g_bat.source == ACPI_BAT_SOURCE_DSDT_NAME) {
        ksnprintf(msg, cap,
                  "battery present (PNP0C0A found in %s); "
                  "charge readable on real HW only",
                  g_bat.hit_table == 0 ? "DSDT" : "SSDT");
    } else if (g_bat.percent == 0xFF) {
        ksnprintf(msg, cap, "battery0: state=%s (charge unknown)",
                  state_tag(g_bat.state));
    } else {
        ksnprintf(msg, cap, "battery0: %u%% %s",
                  (unsigned)g_bat.percent, state_tag(g_bat.state));
    }
    return 0;
}
