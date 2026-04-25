/* sectest.h -- Milestone 34G "securitytest" harness.
 *
 * One entry point that exercises every M34 hardening axis end-to-end
 * against the LIVE production paths (no SELFTEST gating). Used by:
 *
 *   - the `securitytest` shell builtin, so an operator can re-run the
 *     full suite at any time without rebooting;
 *   - kmain when the kernel is built with `make sectest` (which adds
 *     -DSECTEST_AUTORUN), giving an automated PASS/FAIL run on boot
 *     for the test_m34g.ps1 driver to grep.
 *
 * The harness is deliberately kept narrow: each step calls one or two
 * existing public APIs, checks the returned status, and prints a
 * single-line breadcrumb of the form
 *
 *     [securitytest] <area> step N: PASS|FAIL|SKIP (...details...)
 *
 * so a regex of the kernel log gives the same coverage as reading the
 * source. The summary footer
 *
 *     [securitytest] OVERALL: PASS|FAIL pass=P fail=F skip=S total=T
 *
 * is the canonical line external tooling looks for. */

#ifndef TOBYOS_SECTEST_H
#define TOBYOS_SECTEST_H

#include <tobyos/types.h>

struct sectest_summary {
    int total;        /* total steps executed                       */
    int pass;         /* steps that returned the expected outcome   */
    int fail;         /* steps whose outcome diverged from expected */
    int skip;         /* steps that were skipped (missing fixtures) */
};

/* Run the entire M34 validation suite. `out` (if non-NULL) is filled
 * with per-step counters. Always safe to call from boot or from the
 * shell. Idempotent -- cleans up after itself so consecutive runs do
 * not accumulate state. */
void sectest_run(struct sectest_summary *out);

#endif /* TOBYOS_SECTEST_H */
