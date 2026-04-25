/* sectest.c -- Milestone 34G "securitytest" harness.
 *
 * One pass through every M34 hardening axis using only LIVE production
 * APIs. The boot self-test (pkg_m34_selftest, gated on
 * -DPKG_M34_SELFTEST) is the developer-facing equivalent: deeper, more
 * verbose, but tied to a special build. This harness is what you'd
 * actually ship: callable from the shell and cheap enough to autorun
 * on every boot of the security-focused build.
 *
 * Test areas (one or more steps each):
 *
 *   1. M34A  package integrity: valid installs, corrupt rejected
 *   2. M34B  update verification + rollback
 *   3. M34C  signing/trust-store: trusted accepted, unknown/tampered rejected
 *   4. M34D  sandbox defaults: CAPS in manifest survives the round trip
 *   5. M34E  protected paths: write to /data/packages/ denied, priv allowed
 *   6. M34F  audit log: emitted lines land in the slog ring
 *   7. M34X  regression: install -> upgrade -> rollback -> remove still green
 *
 * Step result conventions:
 *
 *   PASS     expected outcome observed
 *   FAIL     diverged outcome, surfaced in the OVERALL line
 *   SKIP     fixture missing, harness still moves on (counted but
 *            does NOT trip OVERALL=FAIL -- nicely degrades on a build
 *            that omits some optional .tpkg)
 *
 * The summary footer
 *
 *     [securitytest] OVERALL: PASS|FAIL pass=P fail=F skip=S total=T
 *
 * is the canonical line external tooling parses. */

#include <tobyos/sectest.h>
#include <tobyos/pkg.h>
#include <tobyos/cap.h>
#include <tobyos/proc.h>
#include <tobyos/vfs.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>
#include <tobyos/sysprot.h>

/* --- shared test plumbing ---------------------------------------- */

static const char *st_label(int rc) {
    return rc == 0 ? "PASS" : (rc < 0 ? "FAIL" : "SKIP");
}

static void st_record(struct sectest_summary *s, int rc) {
    s->total++;
    if (rc == 0)      s->pass++;
    else if (rc < 0)  s->fail++;
    else              s->skip++;
}

/* True iff `path` is reachable as a regular file. Used to gate optional
 * fixtures so a stripped initrd doesn't cause spurious FAILs. */
static bool st_fixture_present(const char *path) {
    struct vfs_stat st;
    return vfs_stat(path, &st) == VFS_OK && st.size > 0;
}

/* Thin strstr -- klibc.h doesn't export one. Same idea as the m34f
 * selftest helper; kept local so sectest.c is independent. */
static const char *st_strstr(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    if (!*needle) return hay;
    for (const char *p = hay; *p; p++) {
        const char *a = p;
        const char *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return p;
    }
    return NULL;
}

/* Drain the slog ring and check whether any record on tag `sub` since
 * `since_seq` contains `needle` in its body. Strict equality on the
 * sub field; substring match on the body. */
static bool st_audit_has(uint64_t since_seq,
                         const char *sub_want,
                         const char *needle)
{
    static struct abi_slog_record snap[ABI_SLOG_RING_DEPTH];
    uint32_t got = slog_drain(snap, ABI_SLOG_RING_DEPTH, since_seq);
    for (uint32_t i = 0; i < got; i++) {
        if (sub_want && strcmp(snap[i].sub, sub_want) != 0) continue;
        if (needle && !st_strstr(snap[i].msg, needle)) continue;
        return true;
    }
    return false;
}

static uint64_t st_now_seq(void) {
    struct abi_slog_stats s;
    slog_stats(&s);
    return s.total_emitted;
}

/* --- per-area tests ---------------------------------------------- */

/* M34A: install valid + reject one corruption flavour. */
static void area_m34a(struct sectest_summary *s) {
    kprintf("[securitytest] m34a -- package integrity\n");

    /* Step 1: clean install of the canonical fixture. */
    int rc;
    if (!st_fixture_present("/repo/helloapp.tpkg")) {
        kprintf("[securitytest] m34a step 1: SKIP (no /repo/helloapp.tpkg)\n");
        st_record(s, +1);
        return;
    }
    (void)pkg_remove("helloapp");          /* defensive */
    rc = pkg_install_path("/repo/helloapp.tpkg");
    int s1 = (rc == 0) ? 0 : -1;
    kprintf("[securitytest] m34a step 1: %s (install rc=%d)\n",
            st_label(s1), rc);
    st_record(s, s1);

    /* Step 2: clean remove (ensures pkg_remove path stays sane). */
    rc = pkg_remove("helloapp");
    int s2 = (rc == 0) ? 0 : -1;
    kprintf("[securitytest] m34a step 2: %s (remove rc=%d)\n",
            st_label(s2), rc);
    st_record(s, s2);

    /* Step 3: corrupt body must be rejected. */
    if (!st_fixture_present("/repo/helloapp_corrupt.tpkg")) {
        kprintf("[securitytest] m34a step 3: SKIP (no corrupt fixture)\n");
        st_record(s, +1);
    } else {
        rc = pkg_install_path("/repo/helloapp_corrupt.tpkg");
        int s3 = (rc != 0) ? 0 : -1;
        kprintf("[securitytest] m34a step 3: %s (corrupt rc=%d, expect non-zero)\n",
                st_label(s3), rc);
        st_record(s, s3);
        /* Even if a buggy install accidentally succeeded we want to
         * leave no trace. */
        (void)pkg_remove("helloapp");
    }
}

/* M34B: install v1, fail to upgrade with a corrupt v2 (no version
 * change), upgrade with valid v2, rollback to v1. */
static void area_m34b(struct sectest_summary *s) {
    kprintf("[securitytest] m34b -- update verification\n");

    if (!st_fixture_present("/repo/helloapp.tpkg") ||
        !st_fixture_present("/repo/helloapp_v2.tpkg")) {
        kprintf("[securitytest] m34b: SKIP (need helloapp.tpkg + helloapp_v2.tpkg)\n");
        st_record(s, +1);
        return;
    }

    (void)pkg_remove("helloapp");

    int rc = pkg_install_path("/repo/helloapp.tpkg");
    int s1 = (rc == 0) ? 0 : -1;
    kprintf("[securitytest] m34b step 1: %s (install v1 rc=%d)\n",
            st_label(s1), rc);
    st_record(s, s1);
    if (s1 != 0) { (void)pkg_remove("helloapp"); return; }

    /* Upgrade with corrupt v2: must fail and leave the live install
     * unchanged. */
    if (st_fixture_present("/repo/helloapp_v2_corrupt.tpkg")) {
        rc = pkg_upgrade_path("/repo/helloapp_v2_corrupt.tpkg");
        char ver[32]; ver[0] = '\0';
        (void)pkg_get_installed_version("helloapp", ver, sizeof(ver));
        int s2 = (rc != 0 && strcmp(ver, "1.0.0") == 0) ? 0 : -1;
        kprintf("[securitytest] m34b step 2: %s (corrupt upgrade rc=%d, ver='%s')\n",
                st_label(s2), rc, ver);
        st_record(s, s2);
    } else {
        kprintf("[securitytest] m34b step 2: SKIP (no v2_corrupt fixture)\n");
        st_record(s, +1);
    }

    /* Valid upgrade: must move version from 1.0.0 -> 1.1.0. */
    rc = pkg_upgrade_path("/repo/helloapp_v2.tpkg");
    char ver2[32]; ver2[0] = '\0';
    (void)pkg_get_installed_version("helloapp", ver2, sizeof(ver2));
    int s3 = (rc == 0 && strcmp(ver2, "1.1.0") == 0) ? 0 : -1;
    kprintf("[securitytest] m34b step 3: %s (good upgrade rc=%d, ver='%s')\n",
            st_label(s3), rc, ver2);
    st_record(s, s3);

    /* Rollback: must drop back to 1.0.0. */
    rc = pkg_rollback("helloapp");
    char ver3[32]; ver3[0] = '\0';
    (void)pkg_get_installed_version("helloapp", ver3, sizeof(ver3));
    int s4 = (rc == 0 && strcmp(ver3, "1.0.0") == 0) ? 0 : -1;
    kprintf("[securitytest] m34b step 4: %s (rollback rc=%d, ver='%s')\n",
            st_label(s4), rc, ver3);
    st_record(s, s4);

    (void)pkg_remove("helloapp");
}

/* M34C: signing/trust store. Trusted accepted; unknown rejected;
 * tampered rejected. Skip cleanly if signing fixtures absent. */
static void area_m34c(struct sectest_summary *s) {
    kprintf("[securitytest] m34c -- package signing\n");

    /* Save policy so we can restore it after the test. */
    int saved_policy = pkg_get_sig_policy();

    if (!st_fixture_present("/repo/helloapp_signed.tpkg")) {
        kprintf("[securitytest] m34c: SKIP (no signed fixtures)\n");
        st_record(s, +1);
        return;
    }

    (void)pkg_remove("helloapp");
    int rc = pkg_install_path("/repo/helloapp_signed.tpkg");
    int s1 = (rc == 0) ? 0 : -1;
    kprintf("[securitytest] m34c step 1: %s (trusted rc=%d)\n",
            st_label(s1), rc);
    st_record(s, s1);
    (void)pkg_remove("helloapp");

    if (st_fixture_present("/repo/helloapp_signed_unknown.tpkg")) {
        rc = pkg_install_path("/repo/helloapp_signed_unknown.tpkg");
        int s2 = (rc != 0) ? 0 : -1;
        kprintf("[securitytest] m34c step 2: %s (unknown key rc=%d)\n",
                st_label(s2), rc);
        st_record(s, s2);
        (void)pkg_remove("helloapp");
    } else {
        kprintf("[securitytest] m34c step 2: SKIP (no unknown-key fixture)\n");
        st_record(s, +1);
    }

    if (st_fixture_present("/repo/helloapp_signed_tampered.tpkg")) {
        rc = pkg_install_path("/repo/helloapp_signed_tampered.tpkg");
        int s3 = (rc != 0) ? 0 : -1;
        kprintf("[securitytest] m34c step 3: %s (tampered rc=%d)\n",
                st_label(s3), rc);
        st_record(s, s3);
        (void)pkg_remove("helloapp");
    } else {
        kprintf("[securitytest] m34c step 3: SKIP (no tampered fixture)\n");
        st_record(s, +1);
    }

    /* Step 4: REQUIRED policy rejects unsigned. */
    if (st_fixture_present("/repo/helloapp.tpkg")) {
        pkg_set_sig_policy(PKG_SIG_POLICY_REQUIRED);
        (void)pkg_remove("helloapp");
        rc = pkg_install_path("/repo/helloapp.tpkg");
        int s4 = (rc != 0) ? 0 : -1;
        kprintf("[securitytest] m34c step 4: %s (unsigned under REQUIRED rc=%d)\n",
                st_label(s4), rc);
        st_record(s, s4);
        (void)pkg_remove("helloapp");
    } else {
        kprintf("[securitytest] m34c step 4: SKIP (no unsigned fixture)\n");
        st_record(s, +1);
    }

    pkg_set_sig_policy(saved_policy);
}

/* M34D: declared CAPS in the manifest narrow process caps and CAP_ADMIN
 * is never grantable via the declared list. We exercise this against
 * an in-memory proc shape so we don't need to actually launch an app. */
static void area_m34d(struct sectest_summary *s) {
    kprintf("[securitytest] m34d -- sandbox defaults\n");

    /* Step 1: parser interprets a CAPS string. */
    uint32_t mask = 0xFFFFFFFFu;
    int unk = -1;
    int rc = cap_parse_list("FILE_READ,GUI", &mask, &unk);
    int s1 = (rc == 0 && unk == 0 &&
              (mask & CAP_FILE_READ) && (mask & CAP_GUI) &&
              !(mask & CAP_FILE_WRITE) && !(mask & CAP_NET))
             ? 0 : -1;
    kprintf("[securitytest] m34d step 1: %s (mask=0x%x unk=%d)\n",
            st_label(s1), (unsigned)mask, unk);
    st_record(s, s1);

    /* Step 2: ADMIN never sneaks through the declared list. */
    mask = 0xFFFFFFFFu;
    rc = cap_parse_list("ADMIN,FILE_READ", &mask, &unk);
    int s2 = (rc == 0 && (mask & CAP_FILE_READ) &&
              !(mask & CAP_ADMIN)) ? 0 : -1;
    kprintf("[securitytest] m34d step 2: %s (mask=0x%x; ADMIN must be off)\n",
            st_label(s2), (unsigned)mask);
    st_record(s, s2);

    /* Step 3: cap_apply_declared narrows but never widens a real proc.
     * Use a stack-shaped struct proc copy so we don't poke the live
     * shell. We only need the `caps` field here. */
    struct proc dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.caps = CAP_FILE_READ | CAP_GUI;     /* parent inherited */
    rc = cap_apply_declared(&dummy, "FILE_READ");   /* declared narrower */
    int s3 = (rc == 0 &&
              (dummy.caps & CAP_FILE_READ) &&
              !(dummy.caps & CAP_GUI)) ? 0 : -1;
    kprintf("[securitytest] m34d step 3: %s (post-apply caps=0x%x)\n",
            st_label(s3), (unsigned)dummy.caps);
    st_record(s, s3);

    /* Step 4: declaring a cap not in the parent's mask cannot grant
     * it. (Pure narrowing means the AND wins.) */
    dummy.caps = CAP_FILE_READ;
    rc = cap_apply_declared(&dummy, "FILE_READ,NET");
    int s4 = (rc == 0 &&
              (dummy.caps & CAP_FILE_READ) &&
              !(dummy.caps & CAP_NET)) ? 0 : -1;
    kprintf("[securitytest] m34d step 4: %s (post-apply caps=0x%x; NET must be off)\n",
            st_label(s4), (unsigned)dummy.caps);
    st_record(s, s4);
}

/* M34E: write to /data/packages/ from the kernel must be denied under
 * sysprot strict mode unless wrapped in a privileged scope. */
static void area_m34e(struct sectest_summary *s) {
    kprintf("[securitytest] m34e -- system file protection\n");

    sysprot_set_test_strict(true);

    const char *probe = "/data/packages/sectest_probe.txt";
    int rc = vfs_write_all(probe, "deny", 4);
    int s1 = (rc == VFS_ERR_PERM) ? 0 : -1;
    kprintf("[securitytest] m34e step 1: %s (deny write rc=%d, expect %d)\n",
            st_label(s1), rc, VFS_ERR_PERM);
    st_record(s, s1);

    int s2 = -1;
    {
        struct sysprot_priv_scope sc;
        sysprot_priv_begin(&sc);
        int rc2 = vfs_write_all(probe, "allow", 5);
        sysprot_priv_end(&sc);
        s2 = (rc2 == VFS_OK) ? 0 : -1;
        kprintf("[securitytest] m34e step 2: %s (priv write rc=%d)\n",
                st_label(s2), rc2);
        st_record(s, s2);
    }

    /* Cleanup -- need the priv scope to remove a protected-path file. */
    {
        struct sysprot_priv_scope sc;
        sysprot_priv_begin(&sc);
        (void)vfs_unlink(probe);
        sysprot_priv_end(&sc);
    }

    sysprot_set_test_strict(false);
}

/* M34F: audit log emission is observable through the slog ring. */
static void area_m34f(struct sectest_summary *s) {
    kprintf("[securitytest] m34f -- audit logging\n");

    uint64_t before = st_now_seq();
    SLOG_INFO(SLOG_SUB_AUDIT, "securitytest probe %llu",
              (unsigned long long)before);
    bool hit = st_audit_has(before, SLOG_SUB_AUDIT,
                            "securitytest probe");
    int s1 = hit ? 0 : -1;
    kprintf("[securitytest] m34f step 1: %s (probe observable in ring)\n",
            st_label(s1));
    st_record(s, s1);

    /* Cross-check: pkg lifecycle emits AUDIT lines without us asking. */
    if (!st_fixture_present("/repo/helloapp.tpkg")) {
        kprintf("[securitytest] m34f step 2: SKIP (no /repo/helloapp.tpkg)\n");
        st_record(s, +1);
        return;
    }
    (void)pkg_remove("helloapp");
    uint64_t before2 = st_now_seq();
    int rc = pkg_install_path("/repo/helloapp.tpkg");
    bool ahit = st_audit_has(before2, SLOG_SUB_AUDIT,
                             "pkg install OK name=helloapp");
    int s2 = (rc == 0 && ahit) ? 0 : -1;
    kprintf("[securitytest] m34f step 2: %s (install rc=%d, audit hit=%d)\n",
            st_label(s2), rc, (int)ahit);
    st_record(s, s2);
    (void)pkg_remove("helloapp");
}

/* Regression: the basic install / upgrade / rollback / remove cycle
 * still works end-to-end on the live, non-selftest kernel. */
static void area_regression(struct sectest_summary *s) {
    kprintf("[securitytest] regression -- install/upgrade/rollback/remove\n");

    if (!st_fixture_present("/repo/helloapp.tpkg") ||
        !st_fixture_present("/repo/helloapp_v2.tpkg")) {
        kprintf("[securitytest] regression: SKIP (need helloapp + helloapp_v2)\n");
        st_record(s, +1);
        return;
    }

    (void)pkg_remove("helloapp");

    int rc = pkg_install_path("/repo/helloapp.tpkg");
    int s1 = (rc == 0) ? 0 : -1;
    kprintf("[securitytest] regression step 1: %s (install rc=%d)\n",
            st_label(s1), rc);
    st_record(s, s1);
    if (s1 != 0) return;

    rc = pkg_upgrade_path("/repo/helloapp_v2.tpkg");
    int s2 = (rc == 0) ? 0 : -1;
    kprintf("[securitytest] regression step 2: %s (upgrade rc=%d)\n",
            st_label(s2), rc);
    st_record(s, s2);

    rc = pkg_rollback("helloapp");
    int s3 = (rc == 0) ? 0 : -1;
    kprintf("[securitytest] regression step 3: %s (rollback rc=%d)\n",
            st_label(s3), rc);
    st_record(s, s3);

    rc = pkg_remove("helloapp");
    int s4 = (rc == 0) ? 0 : -1;
    kprintf("[securitytest] regression step 4: %s (remove rc=%d)\n",
            st_label(s4), rc);
    st_record(s, s4);
}

/* --- public entry point ------------------------------------------ */

void sectest_run(struct sectest_summary *out) {
    struct sectest_summary tmp;
    memset(&tmp, 0, sizeof(tmp));

    kprintf("\n========================================\n");
    kprintf("[securitytest] M34 validation suite begin\n");
    kprintf("========================================\n");

    area_m34a(&tmp);
    area_m34b(&tmp);
    area_m34c(&tmp);
    area_m34d(&tmp);
    area_m34e(&tmp);
    area_m34f(&tmp);
    area_regression(&tmp);

    /* Belt-and-suspenders cleanup so a back-to-back run doesn't trip
     * over leftover backup files from area_m34b's upgrade. */
    (void)pkg_remove("helloapp");

    const char *verdict = (tmp.fail == 0) ? "PASS" : "FAIL";
    kprintf("[securitytest] OVERALL: %s pass=%d fail=%d skip=%d total=%d\n",
            verdict, tmp.pass, tmp.fail, tmp.skip, tmp.total);
    kprintf("========================================\n\n");

    if (out) *out = tmp;
}
