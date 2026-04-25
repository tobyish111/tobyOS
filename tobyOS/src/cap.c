/* cap.c -- capability + sandbox profile enforcement (milestone 18).
 *
 * All of the policy lives here:
 *   - the static table of sandbox profiles,
 *   - the predicate helpers (cap_has / cap_check / cap_path_allowed),
 *   - the profile applier (narrowing-only: child caps &= profile caps).
 *
 * The hooks (vfs.c, syscall.c, socket.c, proc.c) only call into this
 * file; they never construct a cap mask themselves. Keeps every
 * security decision auditable from one place.
 */

#include <tobyos/cap.h>
#include <tobyos/proc.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/vfs.h>
#include <tobyos/slog.h>

/* ---- profile table ----------------------------------------------- *
 *
 * The profile entries are static + const except for the runtime
 * copies handed to cap_profile_apply (which are memcpy'd into the
 * PCB). Adding a new profile is a one-line change here.
 *
 * `restricted` is the only profile with a non-empty sandbox root by
 * default -- stricter profiles like "no caps at all" would deny
 * basically every syscall, which is more of a "dead process" than a
 * useful sandbox. If you need that, use restricted and chmod the
 * target dir to 000. */
static const struct cap_profile g_profiles[] = {
    {
        .name = "unrestricted",
        .caps = CAP_GROUP_ALL,
        .root = "",
    },
    {
        .name = "default",
        .caps = CAP_GROUP_ALL_USER,
        .root = "",
    },
    {
        .name = "file-read-only",
        .caps = CAP_FILE_READ | CAP_GUI | CAP_TERM,
        .root = "",
    },
    {
        .name = "network-only",
        .caps = CAP_NET | CAP_FILE_READ,
        .root = "",
    },
    {
        .name = "restricted",
        .caps = CAP_FILE_READ | CAP_GUI,
        .root = "/data/sandbox",
    },
};

#define PROFILE_COUNT ((int)(sizeof(g_profiles) / sizeof(g_profiles[0])))

/* ---- small helpers ---------------------------------------------- */

static bool streq_nocase_bound(const char *a, const char *b) {
    /* Profile names are ASCII-only; we compare case-sensitively but
     * tolerate a few common aliases below. Keep the comparison byte-
     * exact to avoid locale issues in a kernel. */
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

const struct cap_profile *cap_profile_lookup(const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < PROFILE_COUNT; i++) {
        if (streq_nocase_bound(g_profiles[i].name, name)) {
            return &g_profiles[i];
        }
    }
    return 0;
}

int cap_profile_foreach(cap_profile_iter_cb cb, void *user) {
    if (!cb) return 0;
    for (int i = 0; i < PROFILE_COUNT; i++) {
        int rc = cb(user, &g_profiles[i]);
        if (rc) return rc;
    }
    return 0;
}

/* ---- queries ---------------------------------------------------- */

bool cap_has(const struct proc *p, uint32_t need) {
    /* Kernel (NULL p): implicit ADMIN. */
    if (!p) return true;
    /* ADMIN is a blanket bypass -- still satisfied only if the proc
     * actually holds it. pid 0 is the only one that does. */
    if (p->caps & CAP_ADMIN) return true;
    return (p->caps & need) == need;
}

bool cap_check(const struct proc *p, uint32_t need, const char *what) {
    if (cap_has(p, need)) return true;
    /* Denial: name the missing bits so it's obvious from serial.log
     * which cap the process needed. */
    char missing[96];
    uint32_t lack = need & ~(p ? p->caps : 0u);
    (void)cap_mask_to_string(lack, missing, sizeof(missing));
    kprintf("[cap] deny pid=%d '%s' in %s: missing %s\n",
            p ? p->pid : -1,
            p ? p->name : "(kernel)",
            what ? what : "?",
            missing);
    /* M34F: every capability denial is a security event -- record it
     * structured so `auditlog` can show it. The kprintf above remains
     * for serial-log skimming. */
    SLOG_WARN(SLOG_SUB_AUDIT,
              "cap-deny pid=%d uid=%d '%s' op=%s missing=%s",
              p ? p->pid : -1,
              p ? p->uid : -1,
              p ? p->name : "(kernel)",
              what ? what : "?",
              missing);
    return false;
}

bool cap_path_allowed(const struct proc *p, const char *path) {
    if (!p) return true;
    if (!p->sandbox_root[0]) return true;            /* no jail */
    if (!path || path[0] != '/') return false;       /* must be absolute */

    size_t rlen = strlen(p->sandbox_root);
    if (strncmp(path, p->sandbox_root, rlen) != 0) return false;
    /* Character after the match must be end-of-string or '/' so
     * "/data/sandbox" doesn't match "/data/sandbox-evil/foo". */
    char next = path[rlen];
    return next == '\0' || next == '/';
}

bool cap_check_path(const struct proc *p, const char *path,
                    uint32_t want_bits, const char *what) {
    if (!cap_check(p, want_bits, what)) return false;
    if (!cap_path_allowed(p, path)) {
        kprintf("[cap] deny pid=%d '%s' in %s: path '%s' outside sandbox '%s'\n",
                p ? p->pid : -1,
                p ? p->name : "(kernel)",
                what ? what : "?",
                path ? path : "(null)",
                p ? p->sandbox_root : "");
        /* M34F: sandbox escape attempt -> audit. */
        SLOG_WARN(SLOG_SUB_AUDIT,
                  "sandbox-deny pid=%d uid=%d '%s' op=%s path=%s root=%s",
                  p ? p->pid : -1,
                  p ? p->uid : -1,
                  p ? p->name : "(kernel)",
                  what ? what : "?",
                  path ? path : "(null)",
                  p && p->sandbox_root[0] ? p->sandbox_root : "(none)");
        return false;
    }
    return true;
}

/* ---- profile apply ---------------------------------------------- */

int cap_profile_apply(struct proc *p, const char *profile_name) {
    if (!p || !profile_name) return -1;
    const struct cap_profile *prof = cap_profile_lookup(profile_name);
    if (!prof) {
        kprintf("[cap] unknown profile '%s' -- ignored\n", profile_name);
        return -1;
    }

    /* Narrow caps: a profile can never grant rights the parent didn't
     * already have. Exception: "unrestricted" requested from a proc
     * that already has ADMIN keeps ADMIN; for anyone else it just
     * equates to CAP_GROUP_ALL_USER, which their caps will cap. */
    uint32_t new_caps = p->caps & prof->caps;
    /* But if the profile explicitly asks for ADMIN and the parent
     * already had ADMIN, keep it -- otherwise the kernel's "spawn with
     * unrestricted" would accidentally drop ADMIN. */
    if ((prof->caps & CAP_ADMIN) && (p->caps & CAP_ADMIN)) {
        new_caps |= CAP_ADMIN;
    }
    p->caps = new_caps;

    /* Path root: a profile with a non-empty root replaces the current
     * one (stricter). An empty profile root leaves the inherited root
     * alone -- so a restricted parent can't be loosened by running a
     * child under "default". */
    if (prof->root[0]) {
        size_t n = strlen(prof->root);
        if (n >= CAP_PATH_MAX) n = CAP_PATH_MAX - 1;
        memcpy(p->sandbox_root, prof->root, n);
        p->sandbox_root[n] = '\0';
    }
    return 0;
}

void cap_grant_admin(struct proc *p) {
    if (!p) return;
    p->caps = CAP_GROUP_ALL;
    p->sandbox_root[0] = '\0';
}

/* ---- rendering --------------------------------------------------- */

static struct { uint32_t bit; const char *name; } g_bitnames[] = {
    { CAP_FILE_READ,      "FILE_READ" },
    { CAP_FILE_WRITE,     "FILE_WRITE" },
    { CAP_EXEC,           "EXEC" },
    { CAP_NET,            "NET" },
    { CAP_GUI,            "GUI" },
    { CAP_TERM,           "TERM" },
    { CAP_SETTINGS_WRITE, "SETTINGS_WRITE" },
    { CAP_ADMIN,          "ADMIN" },
};

size_t cap_mask_to_string(uint32_t caps, char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    if (caps == 0) {
        const char *none = "(none)";
        size_t i = 0;
        while (none[i] && i + 1 < cap) { buf[i] = none[i]; i++; }
        buf[i] = '\0';
        return i;
    }
    size_t n = 0;
    bool first = true;
    for (size_t i = 0; i < sizeof(g_bitnames)/sizeof(g_bitnames[0]); i++) {
        if (!(caps & g_bitnames[i].bit)) continue;
        if (!first) {
            if (n + 1 < cap) buf[n++] = ',';
        }
        first = false;
        const char *s = g_bitnames[i].name;
        while (*s && n + 1 < cap) buf[n++] = *s++;
    }
    buf[n] = '\0';
    return n;
}

void cap_dump_proc(const struct proc *p) {
    char caps_str[128];
    if (!p) {
        kprintf("  (kernel)          caps=%s  root=(none)\n",
                cap_mask_to_string(CAP_GROUP_ALL, caps_str, sizeof(caps_str))
                    ? caps_str : "?");
        return;
    }
    cap_mask_to_string(p->caps, caps_str, sizeof(caps_str));
    kprintf("  pid=%-3d name=%-14s  caps=%s  root=%s\n",
            p->pid, p->name,
            caps_str,
            p->sandbox_root[0] ? p->sandbox_root : "(none)");
}

/* ---- M34D: declared capability parser + applier ---------------- *
 *
 * A capability token is one of the names in g_bitnames (case-insensitive).
 * Separators are ',' OR ASCII whitespace; consecutive separators are
 * treated as one. The parser is byte-by-byte and never allocates -- it
 * walks the input, accumulates a token in a small local buffer, and
 * looks it up in g_bitnames. CAP_ADMIN is NEVER grantable through a
 * declared list: the lookup table includes ADMIN for symmetry with
 * cap_mask_to_string, but the parser strips it from the returned mask
 * defensively. */

static bool cap_is_sep(char c) {
    return c == ',' || c == ' ' || c == '\t';
}

/* Look up a single token (NUL-terminated, already upper-cased). Returns
 * the bit value or 0 if no match. */
static uint32_t cap_lookup_token(const char *tok) {
    for (size_t i = 0; i < sizeof(g_bitnames)/sizeof(g_bitnames[0]); i++) {
        const char *n = g_bitnames[i].name;
        const char *t = tok;
        while (*n && *t && *n == *t) { n++; t++; }
        if (*n == '\0' && *t == '\0') return g_bitnames[i].bit;
    }
    return 0;
}

int cap_parse_list(const char *csv, uint32_t *out_mask, int *out_unknown) {
    if (out_mask)    *out_mask    = 0;
    if (out_unknown) *out_unknown = 0;
    if (!csv) return -1;

    uint32_t mask    = 0;
    int      unknown = 0;
    char     tok[CAP_PROFILE_NAME_MAX];

    const char *p = csv;
    while (*p) {
        /* Skip separator runs. */
        while (*p && cap_is_sep(*p)) p++;
        if (!*p) break;

        /* Accumulate one token. Upper-case as we go for case-insensitive
         * matches; cap_lookup_token compares byte-exact. */
        size_t n = 0;
        while (*p && !cap_is_sep(*p)) {
            char c = *p++;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (n + 1 < sizeof(tok)) tok[n++] = c;
            /* Overlong token: keep advancing the cursor but the lookup
             * below will simply miss -- counted as unknown. */
        }
        tok[n] = '\0';
        if (n == 0) continue;

        uint32_t bit = cap_lookup_token(tok);
        if (bit == 0) {
            unknown++;
            kprintf("[cap] declared CAPS: ignoring unknown token '%s'\n", tok);
        } else {
            mask |= bit;
        }
    }

    /* Defensive: ADMIN can never be granted via a declared list. */
    mask &= ~CAP_ADMIN;

    if (out_mask)    *out_mask    = mask;
    if (out_unknown) *out_unknown = unknown;
    return unknown == 0 ? 0 : -1;
}

int cap_apply_declared(struct proc *p, const char *csv) {
    if (!p)        return -1;
    if (!csv || !*csv) return 0;          /* no-op */

    uint32_t mask = 0;
    int      unknown = 0;
    int      rc = cap_parse_list(csv, &mask, &unknown);

    /* Always narrow even if there were unknown tokens -- safer to drop
     * caps a typo couldn't enable than to ignore the whole declaration
     * and silently widen back to the profile defaults. */
    uint32_t before = p->caps;
    p->caps &= mask;
    /* Never strip ADMIN: a kernel-level proc that already had ADMIN
     * keeps it, declared lists can't drop the kernel back to userland. */
    if (before & CAP_ADMIN) p->caps |= CAP_ADMIN;

    char before_s[96], after_s[96], mask_s[96];
    cap_mask_to_string(before,  before_s, sizeof(before_s));
    cap_mask_to_string(p->caps, after_s,  sizeof(after_s));
    cap_mask_to_string(mask,    mask_s,   sizeof(mask_s));
    kprintf("[cap] pid=%d '%s' declared caps='%s' (parsed=%s) "
            "narrow %s -> %s\n",
            p->pid, p->name, csv, mask_s, before_s, after_s);
    return rc;
}
