/* programs/hwreport/main.c -- M35F enhanced diagnostics + reporting.
 *
 * `hwreport` is the operator-facing one-shot diagnostics view that
 * stitches together everything M35 added:
 *
 *   - hwinfo summary (CPU + memory + bus counts + boot mode + profile)
 *   - hwcompat snapshot (per-bus device list with resolved status,
 *     bound driver, and reason)
 *   - per-tier compatibility tally (supported / partial / unsupported)
 *   - errors + warnings derived from the live snapshot:
 *       * unsupported devices
 *       * partial devices (bound but with caveats)
 *       * any "probe failed" rows surfaced via the compat reason text
 *   - "system compatibility status" verdict (GREEN / YELLOW / RED)
 *
 * Output modes:
 *   hwreport            pretty (default, multi-section)
 *   hwreport --json     one-line machine-readable summary (CI-friendly)
 *   hwreport --summary  one-line verdict + counts (shell prompt)
 *   hwreport --boot     M35F boot-harness sentinels (M35F_HWR: ...)
 *
 * Exit codes:
 *   0    everything fetched and rendered (verdict GREEN or YELLOW)
 *   1    syscall layer failed (errno written to stderr)
 *   2    bad usage
 *   3    --boot mode could not validate the snapshot (verdict RED)
 *
 * The compatibility verdict mirrors the same rule the M35F selftest
 * asserts so userland and kernel agree on what a healthy boot looks
 * like:
 *
 *   GREEN  : at least one PCI device bound; no UNSUPPORTED rows;
 *            no UNKNOWN rows; PARTIAL rows allowed.
 *   YELLOW : GREEN preconditions but with PARTIAL or UNSUPPORTED rows
 *            present (still bootable, operator should check).
 *   RED    : UNKNOWN rows present, snapshot empty, or zero PCI bound
 *            (the join fell through -- likely a regression).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <tobyos_hwinfo.h>

#define HWR_BUF_CAP  ABI_HWCOMPAT_MAX_ENTRIES

/* Verdict codes (kept lowercase strings for log greps, plus the
 * single-letter shorthand used in --summary output). */
#define VERDICT_GREEN  "GREEN"
#define VERDICT_YELLOW "YELLOW"
#define VERDICT_RED    "RED"

struct compat_tally {
    int total;
    int pci_total;
    int usb_total;
    int pci_bound;
    int usb_bound;
    int supported;
    int partial;
    int unsupported;
    int unknown;
    int probe_failed; /* rows whose `reason` mentions probe-failure */
    int forced_off;   /* rows whose `reason` mentions blacklist/force-off */
};

static void usage(void) {
    fprintf(stderr,
            "usage: hwreport [--json|--summary|--boot]\n"
            "  default     pretty multi-section diagnostics report\n"
            "  --json      one-line machine-readable verdict + counts\n"
            "  --summary   one-line verdict + per-tier counts\n"
            "  --boot      M35F boot-harness mode (sentinels)\n");
}

/* Walk a hwcompat snapshot and populate a compat_tally. The probe-
 * failed / forced-off counts are derived from the reason string
 * because the kernel does not (yet) expose a numeric error code
 * per row -- the reason text is the canonical record. Substring
 * matching is fine: every reason that surfaces a probe failure
 * starts with "probe failed" and every override starts with
 * "blacklisted" or "forced". */
static void tally(const struct abi_hwcompat_entry *rows, int n,
                  struct compat_tally *t) {
    memset(t, 0, sizeof(*t));
    t->total = n;
    for (int i = 0; i < n; i++) {
        const struct abi_hwcompat_entry *r = &rows[i];
        if (r->bus == ABI_DEVT_BUS_PCI) {
            t->pci_total++;
            if (r->bound) t->pci_bound++;
        } else if (r->bus == ABI_DEVT_BUS_USB) {
            t->usb_total++;
            if (r->bound) t->usb_bound++;
        }
        switch (r->status) {
        case ABI_HWCOMPAT_SUPPORTED:   t->supported++;   break;
        case ABI_HWCOMPAT_PARTIAL:     t->partial++;     break;
        case ABI_HWCOMPAT_UNSUPPORTED: t->unsupported++; break;
        case ABI_HWCOMPAT_UNKNOWN:     t->unknown++;     break;
        default: break;
        }
        if (r->reason[0]) {
            if (strstr(r->reason, "probe failed") ||
                strstr(r->reason, "PROBE_FAILED")) {
                t->probe_failed++;
            }
            if (strstr(r->reason, "blacklist") ||
                strstr(r->reason, "forced off") ||
                strstr(r->reason, "FORCED_OFF")) {
                t->forced_off++;
            }
        }
    }
}

/* The verdict rule lives in one place so render + boot harness +
 * future selftests stay in lockstep. */
static const char *verdict_for(const struct compat_tally *t) {
    if (t->total == 0)        return VERDICT_RED;
    if (t->unknown > 0)       return VERDICT_RED;
    if (t->pci_bound == 0)    return VERDICT_RED;
    if (t->unsupported > 0)   return VERDICT_YELLOW;
    if (t->partial > 0)       return VERDICT_YELLOW;
    if (t->probe_failed > 0)  return VERDICT_YELLOW;
    return VERDICT_GREEN;
}

/* Pretty renderer -- multi-section, designed to fit in an 80-col
 * terminal so it stays readable during a live recovery session. */
static void render_pretty(const struct abi_hwinfo_summary *s,
                          const struct abi_hwcompat_entry *rows,
                          int n,
                          const struct compat_tally *t) {
    const char *verdict = verdict_for(t);

    /* --- section 1: boot context ------------------------------- */
    printf("============================================================\n");
    printf("  tobyOS hardware report (M35F)\n");
    printf("============================================================\n");
    printf("boot mode  : %s (raw=%u)\n",
           tobyhw_boot_mode_str(s->safe_mode), (unsigned)s->safe_mode);
    printf("profile    : %s\n", s->profile_hint);
    printf("uptime     : %lu ms (epoch=%lu)\n",
           (unsigned long)s->boot_uptime_ms,
           (unsigned long)s->snapshot_epoch);
    printf("abi        : %u\n", (unsigned)s->kernel_abi_ver);

    /* --- section 2: hardware inventory ------------------------- */
    printf("\n-- hardware inventory --\n");
    printf("cpu        : %s family=%u model=%u step=%u count=%u\n",
           s->cpu_vendor, (unsigned)s->cpu_family,
           (unsigned)s->cpu_model, (unsigned)s->cpu_stepping,
           (unsigned)s->cpu_count);
    printf("brand      : \"%s\"\n", s->cpu_brand);
    printf("memory     : total=%lu pg used=%lu pg free=%lu pg\n",
           (unsigned long)s->mem_total_pages,
           (unsigned long)s->mem_used_pages,
           (unsigned long)s->mem_free_pages);
    printf("bus counts : pci=%u usb=%u blk=%u input=%u audio=%u "
           "battery=%u hub=%u display=%u\n",
           (unsigned)s->pci_count, (unsigned)s->usb_count,
           (unsigned)s->blk_count, (unsigned)s->input_count,
           (unsigned)s->audio_count, (unsigned)s->battery_count,
           (unsigned)s->hub_count, (unsigned)s->display_count);

    /* --- section 3: compatibility tally + per-row table -------- */
    printf("\n-- compatibility tally --\n");
    printf("total      : %d (pci=%d usb=%d)\n",
           t->total, t->pci_total, t->usb_total);
    printf("supported  : %d\n", t->supported);
    printf("partial    : %d\n", t->partial);
    printf("unsupported: %d\n", t->unsupported);
    printf("unknown    : %d  (must be 0)\n", t->unknown);
    printf("bound      : pci=%d usb=%d\n", t->pci_bound, t->usb_bound);

    printf("\n-- compatibility detail --\n");
    printf("BUS  VID:DID    CLS.SUB.PI  STATUS       BOUND DRIVER         "
           "FRIENDLY\n");
    for (int i = 0; i < n; i++) {
        const struct abi_hwcompat_entry *r = &rows[i];
        printf("%-3s  %04x:%04x  %02x.%02x.%02x   %-12s %-5s %-14s %s\n",
               tobyhw_compat_bus_str(r->bus),
               (unsigned)r->vendor, (unsigned)r->product,
               (unsigned)r->class_code, (unsigned)r->subclass,
               (unsigned)r->prog_if,
               tobyhw_compat_status_str(r->status),
               r->bound ? "yes" : "no",
               r->driver[0] ? r->driver : "(none)",
               r->friendly);
        if (r->reason[0]) {
            printf("                                   reason: %s\n",
                   r->reason);
        }
    }

    /* --- section 4: errors + warnings -------------------------- */
    printf("\n-- errors + warnings --\n");
    if (t->unsupported == 0 && t->partial == 0 &&
        t->probe_failed == 0 && t->forced_off == 0 &&
        t->unknown == 0) {
        printf("(no compatibility issues detected)\n");
    } else {
        for (int i = 0; i < n; i++) {
            const struct abi_hwcompat_entry *r = &rows[i];
            const char *kind = NULL;
            if (r->status == ABI_HWCOMPAT_UNKNOWN)         kind = "UNKNOWN";
            else if (r->status == ABI_HWCOMPAT_UNSUPPORTED)kind = "UNSUPPORTED";
            else if (r->status == ABI_HWCOMPAT_PARTIAL)    kind = "PARTIAL";
            else if (r->reason[0] && (strstr(r->reason, "probe failed") ||
                                      strstr(r->reason, "PROBE_FAILED"))) {
                kind = "PROBE_FAILED";
            } else if (r->reason[0] && strstr(r->reason, "blacklist")) {
                kind = "FORCED_OFF";
            }
            if (!kind) continue;
            printf("[%-12s] %s %04x:%04x %s%s%s\n",
                   kind,
                   tobyhw_compat_bus_str(r->bus),
                   (unsigned)r->vendor, (unsigned)r->product,
                   r->friendly,
                   r->reason[0] ? " -- " : "",
                   r->reason[0] ? r->reason : "");
        }
    }

    /* --- section 5: verdict ----------------------------------- */
    printf("\n-- system compatibility status --\n");
    printf("verdict: %s\n", verdict);
    if (!strcmp(verdict, VERDICT_GREEN)) {
        printf("(all detected hardware bound and supported)\n");
    } else if (!strcmp(verdict, VERDICT_YELLOW)) {
        printf("(boot is fine but some hardware is partial/unsupported)\n");
    } else {
        printf("(boot has serious compatibility issues -- check log)\n");
    }
    printf("============================================================\n");
}

static void render_json(const struct abi_hwinfo_summary *s,
                        const struct compat_tally *t) {
    const char *v = verdict_for(t);
    printf("{\"verdict\":\"%s\",\"boot_mode\":\"%s\","
           "\"profile\":\"%s\","
           "\"counts\":{\"total\":%d,\"supported\":%d,"
           "\"partial\":%d,\"unsupported\":%d,\"unknown\":%d,"
           "\"probe_failed\":%d,\"forced_off\":%d},"
           "\"bound\":{\"pci\":%d,\"usb\":%d},"
           "\"bus\":{\"pci\":%d,\"usb\":%d}}\n",
           v, tobyhw_boot_mode_str(s->safe_mode), s->profile_hint,
           t->total, t->supported, t->partial, t->unsupported,
           t->unknown, t->probe_failed, t->forced_off,
           t->pci_bound, t->usb_bound, t->pci_total, t->usb_total);
}

static void render_summary(const struct abi_hwinfo_summary *s,
                           const struct compat_tally *t) {
    const char *v = verdict_for(t);
    printf("hwreport: verdict=%s mode=%s "
           "total=%d supported=%d partial=%d unsupported=%d "
           "unknown=%d probe_failed=%d forced_off=%d\n",
           v, tobyhw_boot_mode_str(s->safe_mode),
           t->total, t->supported, t->partial, t->unsupported,
           t->unknown, t->probe_failed, t->forced_off);
}

/* Boot harness. Spits stable sentinels test_m35.ps1 (m35f phase) can
 * pattern-match on, and emits PASS only when the verdict is GREEN or
 * YELLOW. RED is a hard FAIL. */
static int do_boot(void) {
    struct abi_hwinfo_summary s;
    if (tobyhw_summary(&s) != 0) {
        printf("M35F_HWR: FAIL: tobyhw_summary errno=%d\n", errno);
        return 1;
    }

    struct abi_hwcompat_entry rows[HWR_BUF_CAP];
    int n = tobyhw_compat_list(rows, HWR_BUF_CAP, 0);
    if (n < 0) {
        printf("M35F_HWR: FAIL: tobyhw_compat_list errno=%d\n", errno);
        return 1;
    }

    struct compat_tally t;
    tally(rows, n, &t);
    const char *v = verdict_for(&t);

    printf("M35F_HWR: boot_mode=%s profile=%s\n",
           tobyhw_boot_mode_str(s.safe_mode), s.profile_hint);
    printf("M35F_HWR: total=%d pci=%d usb=%d "
           "supported=%d partial=%d unsupported=%d unknown=%d\n",
           t.total, t.pci_total, t.usb_total,
           t.supported, t.partial, t.unsupported, t.unknown);
    printf("M35F_HWR: bound pci=%d usb=%d "
           "probe_failed=%d forced_off=%d\n",
           t.pci_bound, t.usb_bound, t.probe_failed, t.forced_off);
    printf("M35F_HWR: verdict=%s\n", v);

    /* Boot harness PASS rule. RED is the only failure: GREEN means
     * everything bound supported, YELLOW means partial/unsupported
     * present (acceptable in QEMU because we ship known-unsupported
     * device entries for diagnostics coverage). */
    int pass = strcmp(v, VERDICT_RED) != 0 &&
               t.unknown == 0 &&
               t.pci_bound > 0;
    if (pass) {
        printf("M35F_HWR: PASS\n");
        return 0;
    }
    printf("M35F_HWR: FAIL\n");
    return 3;
}

int main(int argc, char **argv) {
    int do_json = 0, do_sum = 0, do_boot_mode = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--json"))    do_json      = 1;
        else if (!strcmp(argv[i], "--summary")) do_sum       = 1;
        else if (!strcmp(argv[i], "--boot"))    do_boot_mode = 1;
        else if (!strcmp(argv[i], "--help") ||
                 !strcmp(argv[i], "-h")) { usage(); return 0; }
        else {
            fprintf(stderr, "FAIL: hwreport: unknown arg '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }

    if (do_boot_mode) return do_boot();

    struct abi_hwinfo_summary s;
    if (tobyhw_summary(&s) != 0) {
        fprintf(stderr,
                "FAIL: hwreport: tobyhw_summary errno=%d\n", errno);
        return 1;
    }

    struct abi_hwcompat_entry rows[HWR_BUF_CAP];
    int n = tobyhw_compat_list(rows, HWR_BUF_CAP, 0);
    if (n < 0) {
        fprintf(stderr,
                "FAIL: hwreport: tobyhw_compat_list errno=%d\n", errno);
        return 1;
    }

    struct compat_tally t;
    tally(rows, n, &t);

    if (do_json)      render_json(&s, &t);
    else if (do_sum)  render_summary(&s, &t);
    else              render_pretty(&s, rows, n, &t);
    return 0;
}
