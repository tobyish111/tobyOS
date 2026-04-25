/* drvconf.c -- Milestone 35A: parse + apply driver overrides.
 *
 * Reads /etc/drvmatch.conf at boot via the VFS, parses it into a pair
 * of small static tables, and exposes lookups + an apply step that
 * walks the live PCI driver registry.
 *
 * Grammar (one rule per line; '#' starts a comment that runs to EOL;
 * blank lines are skipped; CR-LF is tolerated):
 *
 *   blacklist <driver-name>
 *   force     <vid>:<did> <driver-name>
 *
 * Numeric tokens accept hex with or without "0x" prefix and decimal
 * digits as a fallback. Names are <= DRVCONF_NAME_MAX-1 chars.
 *
 * Anything that doesn't parse is logged and skipped -- the goal is
 * "best effort": a bad line should never prevent the rest of the
 * file from being honoured.
 */

#include <tobyos/drvconf.h>
#include <tobyos/drvmatch.h>
#include <tobyos/pci.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>

/* ============================================================
 * private state
 * ============================================================ */

static char  g_blacklist[DRVCONF_MAX_BLACKLIST][DRVCONF_NAME_MAX];
static size_t g_bl_count;

static struct drvconf_force_rule g_force[DRVCONF_MAX_FORCE];
static size_t g_force_count;

static bool g_loaded;
static bool g_applied;

/* ============================================================
 * tiny lex helpers (no dependency on strtok / sscanf)
 * ============================================================ */

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static bool is_hex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Skip leading whitespace; returns updated pointer. */
static const char *skip_ws(const char *p) {
    while (*p && is_space(*p)) p++;
    return p;
}

/* Copy [start, end) into dst (capped, NUL-terminated). */
static void copy_token(char *dst, size_t cap,
                       const char *start, const char *end) {
    if (cap == 0) return;
    size_t n = (size_t)(end - start);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) dst[i] = start[i];
    dst[n] = '\0';
}

/* Parse a 16-bit hex number at *pp. PCI vendor:device IDs are
 * universally written in hex, so we never auto-switch to decimal --
 * a leading "0x"/"0X" is accepted and stripped, the rest of the
 * token is read as raw hex. Advances *pp past the number. Returns
 * 0 on success, -1 if no hex digits were available. */
static int parse_u16(const char **pp, uint16_t *out) {
    const char *p = *pp;
    if (!*p) return -1;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

    if (!is_hex(*p)) return -1;
    uint32_t acc = 0;
    while (*p && is_hex(*p)) {
        acc = (acc << 4) | (uint32_t)hex_val(*p);
        if (acc > 0xFFFF) return -1;
        p++;
    }
    *out = (uint16_t)acc;
    *pp = p;
    return 0;
}

/* Read one token (non-space, non-#) into dst. Returns updated pointer
 * past the token, or NULL if no token at *p (only ws/comment/eol). */
static const char *read_token(const char *p, char *dst, size_t cap) {
    p = skip_ws(p);
    if (!*p || *p == '#' || *p == '\n') return NULL;
    const char *start = p;
    while (*p && !is_space(*p) && *p != '#' && *p != '\n') p++;
    copy_token(dst, cap, start, p);
    return p;
}

/* ============================================================
 * rule registration
 * ============================================================ */

static bool add_blacklist(const char *name) {
    if (!name || !*name) return false;
    if (g_bl_count >= DRVCONF_MAX_BLACKLIST) {
        kprintf("[drvconf] WARN: blacklist table full, dropping '%s'\n",
                name);
        return false;
    }
    /* Reject duplicates so the table stays tidy. */
    for (size_t i = 0; i < g_bl_count; i++) {
        if (!strcmp(g_blacklist[i], name)) return true;
    }
    size_t n = strlen(name);
    if (n >= DRVCONF_NAME_MAX) n = DRVCONF_NAME_MAX - 1;
    for (size_t i = 0; i < n; i++) g_blacklist[g_bl_count][i] = name[i];
    g_blacklist[g_bl_count][n] = '\0';
    g_bl_count++;
    return true;
}

static bool add_force(uint16_t vendor, uint16_t device, const char *name) {
    if (!name || !*name) return false;
    if (g_force_count >= DRVCONF_MAX_FORCE) {
        kprintf("[drvconf] WARN: force table full, dropping %04x:%04x\n",
                (unsigned)vendor, (unsigned)device);
        return false;
    }
    /* Last-write-wins for duplicate VID:DID. */
    for (size_t i = 0; i < g_force_count; i++) {
        if (g_force[i].vendor == vendor && g_force[i].device == device) {
            size_t n = strlen(name);
            if (n >= DRVCONF_NAME_MAX) n = DRVCONF_NAME_MAX - 1;
            for (size_t j = 0; j < n; j++) g_force[i].driver[j] = name[j];
            g_force[i].driver[n] = '\0';
            return true;
        }
    }
    g_force[g_force_count].vendor = vendor;
    g_force[g_force_count].device = device;
    size_t n = strlen(name);
    if (n >= DRVCONF_NAME_MAX) n = DRVCONF_NAME_MAX - 1;
    for (size_t i = 0; i < n; i++) g_force[g_force_count].driver[i] = name[i];
    g_force[g_force_count].driver[n] = '\0';
    g_force_count++;
    return true;
}

/* ============================================================
 * line parsing
 * ============================================================ */

static int parse_one_line(const char *line, int line_no) {
    const char *p = skip_ws(line);
    if (!*p || *p == '#' || *p == '\n') return 0;

    char verb[16];
    p = read_token(p, verb, sizeof(verb));
    if (!p) return 0;

    if (!strcmp(verb, "blacklist")) {
        char name[DRVCONF_NAME_MAX];
        p = read_token(p, name, sizeof(name));
        if (!p) {
            kprintf("[drvconf] line %d: 'blacklist' missing driver name\n",
                    line_no);
            return -1;
        }
        return add_blacklist(name) ? 1 : -1;
    }

    if (!strcmp(verb, "force")) {
        p = skip_ws(p);
        uint16_t vid = 0, did = 0;
        if (parse_u16(&p, &vid) != 0) {
            kprintf("[drvconf] line %d: 'force' bad vendor id\n", line_no);
            return -1;
        }
        p = skip_ws(p);
        if (*p != ':') {
            kprintf("[drvconf] line %d: 'force' missing ':' separator\n",
                    line_no);
            return -1;
        }
        p++;
        p = skip_ws(p);
        if (parse_u16(&p, &did) != 0) {
            kprintf("[drvconf] line %d: 'force' bad device id\n", line_no);
            return -1;
        }
        char name[DRVCONF_NAME_MAX];
        const char *q = read_token(p, name, sizeof(name));
        if (!q) {
            kprintf("[drvconf] line %d: 'force' missing driver name\n",
                    line_no);
            return -1;
        }
        return add_force(vid, did, name) ? 1 : -1;
    }

    kprintf("[drvconf] line %d: unknown verb '%s', skipping\n",
            line_no, verb);
    return -1;
}

/* ============================================================
 * load
 * ============================================================ */

int drvconf_load_path(const char *path) {
    /* Reset on every call so the second call truly re-reads. */
    g_bl_count = 0;
    g_force_count = 0;
    g_loaded = false;
    g_applied = false;

    if (!path || !*path) path = DRVCONF_PATH;

    void *buf = NULL;
    size_t sz = 0;
    int rc = vfs_read_all(path, &buf, &sz);
    if (rc != 0 || !buf) {
        kprintf("[drvconf] %s not present (rc=%d) -- no overrides active\n",
                path, rc);
        g_loaded = true;        /* "loaded with zero rules" */
        return 0;
    }

    /* Walk line by line. We tolerate either '\n' or '\r\n' since
     * /etc/drvmatch.conf may legitimately come from a Windows host
     * via the host-side build. */
    int accepted = 0;
    int errors   = 0;
    int line_no  = 1;
    char *cstr   = (char *)buf;
    size_t i = 0;
    while (i <= sz) {
        size_t start = i;
        while (i < sz && cstr[i] != '\n') i++;
        size_t end = i;
        /* Strip a trailing '\r' for CR-LF files. */
        size_t len = end - start;
        char  tmp[256];
        if (len >= sizeof(tmp)) {
            kprintf("[drvconf] line %d too long (%lu bytes), skipping\n",
                    line_no, (unsigned long)len);
            errors++;
        } else {
            for (size_t k = 0; k < len; k++) tmp[k] = cstr[start + k];
            if (len > 0 && tmp[len - 1] == '\r') len--;
            tmp[len] = '\0';
            int r = parse_one_line(tmp, line_no);
            if (r > 0) accepted++;
            else if (r < 0) errors++;
        }
        line_no++;
        if (i >= sz) break;
        i++;            /* skip the '\n' itself */
    }
    kfree(buf);

    g_loaded = true;
    kprintf("[drvconf] loaded %s: blacklist=%lu force=%lu errors=%d\n",
            path,
            (unsigned long)g_bl_count, (unsigned long)g_force_count,
            errors);
    slog_emit(ABI_SLOG_LEVEL_INFO, SLOG_SUB_HW,
              "drvconf load: blacklist=%u force=%u errors=%d",
              (unsigned)g_bl_count, (unsigned)g_force_count, errors);

    return accepted;
}

int drvconf_load(void) {
    return drvconf_load_path(DRVCONF_PATH);
}

/* ============================================================
 * apply -- talks to drvmatch / pci
 * ============================================================ */

extern struct pci_driver *pci_driver_iter(struct pci_driver *prev);

static struct pci_driver *find_drv(const char *name) {
    for (struct pci_driver *it = pci_driver_iter(NULL); it;
         it = pci_driver_iter(it)) {
        if (it->name && !strcmp(it->name, name)) return it;
    }
    return NULL;
}

void drvconf_apply(void) {
    if (!g_loaded) {
        kprintf("[drvconf] apply: skipped (drvconf_load not called)\n");
        return;
    }
    if (g_applied) return;

    /* Blacklists first: drvmatch_disable_pci unbinds + rebinds, so by
     * the time we run the force loop the registry is in its post-
     * blacklist steady state. */
    for (size_t i = 0; i < g_bl_count; i++) {
        long n = drvmatch_disable_pci(g_blacklist[i]);
        if (n < 0) {
            kprintf("[drvconf] blacklist '%s' -> drvmatch err %ld\n",
                    g_blacklist[i], n);
            slog_emit(ABI_SLOG_LEVEL_WARN, SLOG_SUB_HW,
                      "drvconf blacklist '%s' err=%ld",
                      g_blacklist[i], n);
        } else {
            kprintf("[drvconf] blacklist '%s' applied (unbound %ld dev)\n",
                    g_blacklist[i], n);
            slog_emit(ABI_SLOG_LEVEL_INFO, SLOG_SUB_HW,
                      "drvconf blacklist '%s' unbound=%ld",
                      g_blacklist[i], n);
        }
    }

    /* Force rules: for each rule, walk the device table and try to
     * (re)bind the matching device to the requested driver. Skip if
     * it's already owned by that driver. If the device is owned by
     * something else, unbind it first via drv->remove (if any), then
     * probe the requested driver. If the requested driver isn't
     * registered, log + leave the device in its current state. */
    for (size_t fi = 0; fi < g_force_count; fi++) {
        const struct drvconf_force_rule *r = &g_force[fi];
        struct pci_driver *want = find_drv(r->driver);
        if (!want) {
            kprintf("[drvconf] force %04x:%04x -> '%s': driver not "
                    "registered, ignored\n",
                    (unsigned)r->vendor, (unsigned)r->device, r->driver);
            slog_emit(ABI_SLOG_LEVEL_WARN, SLOG_SUB_HW,
                      "drvconf force %04x:%04x: driver '%s' missing",
                      (unsigned)r->vendor, (unsigned)r->device, r->driver);
            continue;
        }
        bool any = false;
        size_t n = pci_device_count();
        for (size_t i = 0; i < n; i++) {
            struct pci_dev *p = pci_device_at(i);
            if (!p) continue;
            if (p->vendor != r->vendor || p->device != r->device) continue;
            any = true;

            if (p->driver == want) {
                kprintf("[drvconf] force %04x:%04x -> %s: already bound\n",
                        (unsigned)r->vendor, (unsigned)r->device, want->name);
                continue;
            }
            if (p->driver) {
                if (p->driver->remove) p->driver->remove(p);
                p->driver = NULL;
                p->driver_data = NULL;
            }
            int rc = want->probe(p);
            if (rc == 0) {
                p->driver = want;
                p->match_strategy = ABI_DRVMATCH_EXACT;
                kprintf("[drvconf] force %04x:%04x -> %s OK\n",
                        (unsigned)r->vendor, (unsigned)r->device, want->name);
                slog_emit(ABI_SLOG_LEVEL_INFO, SLOG_SUB_HW,
                          "drvconf force %04x:%04x -> %s OK",
                          (unsigned)r->vendor, (unsigned)r->device,
                          want->name);
            } else {
                kprintf("[drvconf] force %04x:%04x -> %s declined "
                        "(rc=%d), falling back\n",
                        (unsigned)r->vendor, (unsigned)r->device,
                        want->name, rc);
                slog_emit(ABI_SLOG_LEVEL_WARN, SLOG_SUB_HW,
                          "drvconf force %04x:%04x -> %s declined rc=%d",
                          (unsigned)r->vendor, (unsigned)r->device,
                          want->name, rc);
                /* Re-run the full bind pass so the original or some
                 * other matching driver gets a chance to claim it. */
                pci_bind_drivers();
            }
        }
        if (!any) {
            kprintf("[drvconf] force %04x:%04x -> %s: no such device "
                    "in PCI inventory, skipped\n",
                    (unsigned)r->vendor, (unsigned)r->device, want->name);
        }
    }

    g_applied = true;
}

/* ============================================================
 * read-only queries
 * ============================================================ */

bool drvconf_is_blacklisted(const char *name) {
    if (!name || !*name) return false;
    for (size_t i = 0; i < g_bl_count; i++) {
        if (!strcmp(g_blacklist[i], name)) return true;
    }
    return false;
}

const char *drvconf_force_driver(uint16_t vendor, uint16_t device) {
    for (size_t i = 0; i < g_force_count; i++) {
        if (g_force[i].vendor == vendor && g_force[i].device == device) {
            return g_force[i].driver;
        }
    }
    return NULL;
}

size_t drvconf_blacklist_count(void) { return g_bl_count; }
size_t drvconf_force_count(void)     { return g_force_count; }

const char *drvconf_blacklist_at(size_t idx) {
    return (idx < g_bl_count) ? g_blacklist[idx] : NULL;
}
const struct drvconf_force_rule *drvconf_force_at(size_t idx) {
    return (idx < g_force_count) ? &g_force[idx] : NULL;
}

void drvconf_dump_kprintf(void) {
    kprintf("[drvconf] === active overrides ===\n");
    if (g_bl_count == 0 && g_force_count == 0) {
        kprintf("[drvconf] (none)\n");
        return;
    }
    for (size_t i = 0; i < g_bl_count; i++) {
        kprintf("[drvconf] blacklist %s\n", g_blacklist[i]);
    }
    for (size_t i = 0; i < g_force_count; i++) {
        kprintf("[drvconf] force %04x:%04x -> %s\n",
                (unsigned)g_force[i].vendor,
                (unsigned)g_force[i].device,
                g_force[i].driver);
    }
}
