/* pkgbuild.c -- manifest-driven .tpkg builder for the tobyOS SDK.
 *
 * Build (MSYS2/UCRT64, Linux, macOS):
 *     gcc -O2 -Wall -Wextra -std=c11 -o build/pkgbuild tools/pkgbuild.c
 *
 * Usage:
 *     pkgbuild [options] <tobyapp.toml>
 *
 * Options:
 *     -o <out.tpkg>    output package path (default: <name>.tpkg in CWD)
 *     -C <dir>         chdir before resolving relative file sources
 *                      (default = directory containing the manifest)
 *     -v               verbose: print every file copied + final stats
 *     -h, --help       this help
 *
 * Behaviour:
 *
 * pkgbuild is the SDK-facing front-end for the .tpkg format that the
 * in-kernel `pkg install` command (src/pkg.c) consumes. It reads a
 * single `tobyapp.toml` manifest, resolves every file path relative
 * to the manifest's directory (or -C), validates the manifest, and
 * writes a `.tpkg` whose byte layout is identical to what the older
 * tools/mkpkg produces by hand. The kernel installer is unaware of
 * pkgbuild's existence -- the contract is the package format.
 *
 * Manifest format (INI-flavoured TOML subset):
 *
 *     # comments start with '#'
 *
 *     [app]
 *     name        = "hello_cli"
 *     version     = "1.0.0"
 *     kind        = "cli"        # cli | gui | service
 *     description = "Optional one-line description"
 *     author      = "Your name"
 *     license     = "MIT"
 *
 *     [install]
 *     prefix      = "/data/apps/hello_cli"
 *
 *     [files]
 *     "bin/hello_cli.elf" = "build/hello_cli.elf"
 *     "share/data.txt"    = "data/data.txt"
 *
 *     [launcher]
 *     label = "Hello CLI"
 *     exec  = "bin/hello_cli.elf"
 *
 *     [service]                  # only for kind="service"
 *     restart   = "on-failure"   # always | on-failure | never
 *     autostart = false
 *
 * Strings may be quoted or unquoted; quotes are stripped if present.
 * Each [files] entry's left side is the path INSIDE the package
 * (appended to install.prefix); the right side is the host path on
 * disk to slurp.
 *
 * Validation:
 *   - app.name + app.version + app.kind required
 *   - install.prefix must be set and start with /data/
 *   - at least one [files] entry required
 *   - launcher.exec, if present, must reference a [files] dest
 *   - service.restart, if present, must be one of the three keywords
 *
 * Exits 0 on success, 2 on usage error, 3 on host I/O error,
 * 4 on validation error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>

#define MAX_FILES 32
#define MAX_LINE  512

/* All right-hand strings (and most lefthand keys) end up either as
 * fields of `m`, or as entries in the files[] table. We don't bother
 * with a dedicated string interner -- the manifest is small and the
 * wallclock difference is unmeasurable. */

struct file_ent {
    char  dest_in_pkg[256];     /* e.g. "bin/hello_cli.elf"           */
    char  src_on_host[512];     /* e.g. "build/hello_cli.elf"         */
    char  full_dest [512];      /* prefix + "/" + dest_in_pkg         */
    long  size;
    unsigned char *payload;
};

struct manifest {
    char name[64];
    char version[32];
    char kind[16];               /* cli | gui | service */
    char description[128];
    char author[64];
    char license[32];

    char install_prefix[256];

    struct file_ent files[MAX_FILES];
    int             nfiles;

    /* launcher (zero or one entry per package today) */
    int  has_launcher;
    char launcher_label[64];
    char launcher_exec [256];   /* dest-in-pkg path */

    /* service block (ignored unless kind == "service") */
    int  has_service;
    char service_restart[16];   /* always | on-failure | never */
    int  service_autostart;
};

/* --------- error helpers --------- */

static void die(int code, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "pkgbuild: error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(code);
}

static void warn(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "pkgbuild: warning: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* --------- string helpers --------- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s) {
        char c = e[-1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        e--;
    }
    *e = '\0';
    return s;
}

/* Strip surrounding "..." or '...' if present, in place. */
static char *unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n-1] == '"') ||
                   (s[0] == '\'' && s[n-1] == '\''))) {
        s[n-1] = '\0';
        return s + 1;
    }
    return s;
}

/* Strip an end-of-line `# comment` from `s`, in place. We have to be
 * quote-aware because manifests legitimately contain `#` in values
 * (e.g. `description = "version 1#beta"`). Quotes don't nest in our
 * subset, so a single in_dq/in_sq pair is sufficient. */
static void strip_eol_comment(char *s) {
    int in_dq = 0, in_sq = 0;
    for (char *p = s; *p; p++) {
        char c = *p;
        if      (c == '"'  && !in_sq) in_dq = !in_dq;
        else if (c == '\'' && !in_dq) in_sq = !in_sq;
        else if (c == '#'  && !in_dq && !in_sq) { *p = '\0'; return; }
    }
}

static void copy_capped(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* True if `s` starts with `prefix`. */
static int starts_with(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

/* Slurp an entire host file into a malloc'd buffer; sets *out_size. */
static unsigned char *slurp(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) die(3, "cannot open '%s': %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0) die(3, "fseek('%s'): %s", path, strerror(errno));
    long sz = ftell(f);
    if (sz < 0) die(3, "ftell('%s'): %s", path, strerror(errno));
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) die(3, "OOM reading '%s' (%ld bytes)", path, sz);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        die(3, "short read on '%s'", path);
    }
    fclose(f);
    *out_size = sz;
    return buf;
}

/* --------- TOML-subset parser ----------
 *
 * What we recognise:
 *   - blank lines and comments (`# ...`)
 *   - section headers `[name]` (case-sensitive)
 *   - key = value lines (key may be quoted or bare; value may be
 *     quoted, bare word, or the bareword "true" / "false" for bools)
 *
 * What we deliberately don't handle (and reject loudly):
 *   - inline tables, arrays, multi-line strings, dotted keys
 *
 * The parser is one pass, line-buffered, O(file size). */

static void parse_manifest(const char *path, struct manifest *m) {
    FILE *f = fopen(path, "r");
    if (!f) die(3, "cannot open manifest '%s': %s", path, strerror(errno));

    char line[MAX_LINE];
    int  lineno = 0;
    char section[32] = "";

    memset(m, 0, sizeof(*m));
    /* Defaults that are too tedious to require explicitly. */
    copy_capped(m->kind, "cli", sizeof(m->kind));
    copy_capped(m->service_restart, "on-failure", sizeof(m->service_restart));

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *t = trim(line);
        if (!*t || *t == '#') continue;

        /* Strip an end-of-line `# comment` so things like
         *   restart = "on-failure"   # always | on-failure | never
         * parse to value "on-failure", not the entire trailing remark. */
        strip_eol_comment(t);
        t = trim(t);
        if (!*t) continue;

        /* Section header */
        if (*t == '[') {
            char *end = strchr(t, ']');
            if (!end) die(2, "%s:%d: unterminated section", path, lineno);
            *end = '\0';
            copy_capped(section, t + 1, sizeof(section));
            continue;
        }

        /* key = value */
        char *eq = strchr(t, '=');
        if (!eq) die(2, "%s:%d: expected `key = value`, got '%s'",
                     path, lineno, t);
        *eq = '\0';
        char *k = trim(t);
        char *v = trim(eq + 1);
        k = unquote(k);
        v = unquote(v);

        if (!strcmp(section, "app")) {
            if      (!strcmp(k, "name"))        copy_capped(m->name, v, sizeof(m->name));
            else if (!strcmp(k, "version"))     copy_capped(m->version, v, sizeof(m->version));
            else if (!strcmp(k, "kind"))        copy_capped(m->kind, v, sizeof(m->kind));
            else if (!strcmp(k, "description")) copy_capped(m->description, v, sizeof(m->description));
            else if (!strcmp(k, "author"))      copy_capped(m->author, v, sizeof(m->author));
            else if (!strcmp(k, "license"))     copy_capped(m->license, v, sizeof(m->license));
            else warn("%s:%d: unknown key [app].%s -- ignored",
                      path, lineno, k);
        } else if (!strcmp(section, "install")) {
            if (!strcmp(k, "prefix")) copy_capped(m->install_prefix, v, sizeof(m->install_prefix));
            else warn("%s:%d: unknown key [install].%s -- ignored",
                      path, lineno, k);
        } else if (!strcmp(section, "files")) {
            if (m->nfiles >= MAX_FILES) die(4, "too many [files] entries (max %d)", MAX_FILES);
            struct file_ent *fe = &m->files[m->nfiles++];
            copy_capped(fe->dest_in_pkg, k, sizeof(fe->dest_in_pkg));
            copy_capped(fe->src_on_host, v, sizeof(fe->src_on_host));
        } else if (!strcmp(section, "launcher")) {
            m->has_launcher = 1;
            if      (!strcmp(k, "label")) copy_capped(m->launcher_label, v, sizeof(m->launcher_label));
            else if (!strcmp(k, "exec"))  copy_capped(m->launcher_exec,  v, sizeof(m->launcher_exec));
            else if (!strcmp(k, "icon"))  ; /* reserved for future use */
            else warn("%s:%d: unknown key [launcher].%s -- ignored",
                      path, lineno, k);
        } else if (!strcmp(section, "service")) {
            m->has_service = 1;
            if      (!strcmp(k, "restart"))   copy_capped(m->service_restart, v, sizeof(m->service_restart));
            else if (!strcmp(k, "autostart")) m->service_autostart = (!strcmp(v, "true") || !strcmp(v, "1"));
            else warn("%s:%d: unknown key [service].%s -- ignored",
                      path, lineno, k);
        } else if (!*section) {
            die(2, "%s:%d: key '%s' before any [section]", path, lineno, k);
        } else {
            warn("%s:%d: unknown section [%s] -- ignored", path, lineno, section);
        }
    }
    fclose(f);
}

static void validate(const struct manifest *m, const char *manifest_path) {
    if (!m->name[0])    die(4, "%s: [app].name is required", manifest_path);
    if (!m->version[0]) die(4, "%s: [app].version is required", manifest_path);
    if (!m->kind[0])    die(4, "%s: [app].kind is required (cli|gui|service)", manifest_path);
    if (strcmp(m->kind, "cli") && strcmp(m->kind, "gui") && strcmp(m->kind, "service")) {
        die(4, "%s: [app].kind must be one of cli|gui|service (got '%s')",
            manifest_path, m->kind);
    }
    if (!m->install_prefix[0]) {
        die(4, "%s: [install].prefix is required", manifest_path);
    }
    if (!starts_with(m->install_prefix, "/data/")) {
        die(4, "%s: [install].prefix must start with /data/ (got '%s')",
            manifest_path, m->install_prefix);
    }
    if (m->nfiles == 0) {
        die(4, "%s: at least one [files] entry is required", manifest_path);
    }
    if (m->has_launcher) {
        if (!m->launcher_label[0]) die(4, "%s: [launcher].label missing", manifest_path);
        if (!m->launcher_exec[0])  die(4, "%s: [launcher].exec missing",  manifest_path);
        int found = 0;
        for (int i = 0; i < m->nfiles; i++) {
            if (!strcmp(m->files[i].dest_in_pkg, m->launcher_exec)) { found = 1; break; }
        }
        if (!found) {
            die(4, "%s: [launcher].exec='%s' must be one of the [files] entries",
                manifest_path, m->launcher_exec);
        }
    }
    if (m->has_service) {
        if (strcmp(m->service_restart, "always") &&
            strcmp(m->service_restart, "on-failure") &&
            strcmp(m->service_restart, "never")) {
            die(4, "%s: [service].restart must be always|on-failure|never (got '%s')",
                manifest_path, m->service_restart);
        }
        if (strcmp(m->kind, "service") != 0) {
            warn("%s: [service] block ignored because [app].kind != \"service\"",
                 manifest_path);
        }
    }
}

/* Compose `<install_prefix>/<dest_in_pkg>` with one slash exactly. */
static void compose_full_dest(const struct manifest *m, struct file_ent *fe) {
    size_t plen = strlen(m->install_prefix);
    int    psl  = (plen > 0 && m->install_prefix[plen-1] == '/');
    int    dsl  = (fe->dest_in_pkg[0] == '/');
    snprintf(fe->full_dest, sizeof(fe->full_dest), "%s%s%s",
             m->install_prefix,
             (psl || dsl) ? "" : "/",
             dsl ? fe->dest_in_pkg + 1 : fe->dest_in_pkg);
}

/* Slice the manifest path's directory component into `out`. Used as
 * the default -C value so users can write `pkgbuild tobyapp.toml`
 * from inside their project and have file paths resolve naturally
 * relative to the manifest file. */
static void manifest_dir(const char *manifest_path, char *out, size_t cap) {
    const char *slash = NULL;
    for (const char *p = manifest_path; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (!slash) {
        copy_capped(out, ".", cap);
    } else {
        size_t n = (size_t)(slash - manifest_path);
        if (n >= cap) n = cap - 1;
        memcpy(out, manifest_path, n);
        out[n] = '\0';
    }
}

/* --------- main ----------------- */

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [options] <tobyapp.toml>\n"
            "  -o <out.tpkg>   output path (default: <name>.tpkg in CWD)\n"
            "  -C <dir>        chdir before resolving file sources\n"
            "                  (default: directory containing the manifest)\n"
            "  -v              verbose\n"
            "  -h, --help      this help\n",
            prog);
}

int main(int argc, char **argv) {
    const char *out_path     = NULL;
    const char *chdir_to     = NULL;
    const char *manifest_arg = NULL;
    int         verbose      = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(a, "-o")) {
            if (++i >= argc) die(2, "-o needs a value");
            out_path = argv[i];
        } else if (!strcmp(a, "-C")) {
            if (++i >= argc) die(2, "-C needs a value");
            chdir_to = argv[i];
        } else if (!strcmp(a, "-v")) {
            verbose = 1;
        } else if (a[0] == '-') {
            die(2, "unknown option '%s'", a);
        } else if (!manifest_arg) {
            manifest_arg = a;
        } else {
            die(2, "extra positional arg '%s' (manifest already set to '%s')",
                a, manifest_arg);
        }
    }
    if (!manifest_arg) {
        usage(argv[0]);
        return 2;
    }

    struct manifest m;
    parse_manifest(manifest_arg, &m);
    validate(&m, manifest_arg);

    /* If the user didn't pass -C, chdir into the manifest's directory
     * so relative file paths resolve as written. We capture the
     * absolute output path BEFORE chdir, so -o stays relative to the
     * caller's CWD as one would expect. */
    char default_dir[512];
    if (!chdir_to) {
        manifest_dir(manifest_arg, default_dir, sizeof(default_dir));
        chdir_to = default_dir;
    }

    /* Capture an absolute output path before chdir, if -o is relative
     * and not already absolute. We do this by leaving a NULL-or-set
     * out_path, then resolving it AFTER chdir below if needed. The
     * normal SDK pattern is `pkgbuild tobyapp.toml -o build/foo.tpkg`
     * from a project root, where chdir(.) is a no-op, so this is
     * mostly a defensive convenience. */
    char abs_out[1024] = "";
    if (out_path && out_path[0] != '/'
                 && !(out_path[0] && out_path[1] == ':')) {
        /* relative -- bake in caller's CWD now */
        if (!getcwd(abs_out, sizeof(abs_out))) {
            die(3, "getcwd: %s", strerror(errno));
        }
        size_t n = strlen(abs_out);
        snprintf(abs_out + n, sizeof(abs_out) - n, "/%s", out_path);
        out_path = abs_out;
    }

    if (chdir(chdir_to) != 0) {
        die(3, "chdir('%s'): %s", chdir_to, strerror(errno));
    }

    /* Compose full destinations + slurp every payload. */
    long total_body = 0;
    for (int i = 0; i < m.nfiles; i++) {
        compose_full_dest(&m, &m.files[i]);
        if (!starts_with(m.files[i].full_dest, "/data/")) {
            die(4, "computed dest '%s' (from prefix='%s' + '%s') must start with /data/",
                m.files[i].full_dest, m.install_prefix, m.files[i].dest_in_pkg);
        }
        m.files[i].payload = slurp(m.files[i].src_on_host, &m.files[i].size);
        total_body += m.files[i].size;
    }

    /* Default output path: `<name>.tpkg` in the original CWD if -o
     * wasn't given. Since we chdir'd above, default-naming has to
     * land in the chdir'd dir; that's a fine convention since most
     * projects produce build/ relative to themselves. */
    char default_out[256];
    if (!out_path) {
        snprintf(default_out, sizeof(default_out), "%s.tpkg", m.name);
        out_path = default_out;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) die(3, "open output '%s': %s", out_path, strerror(errno));

    /* Header. Same byte format as tools/mkpkg. The kernel reader is
     * line-oriented and tolerant of unknown header lines, but we
     * stick to the established whitelist (TPKG/NAME/VERSION/DESC/
     * APP/FILE/BODY) to keep things deterministic. The KIND, AUTHOR,
     * LICENSE, and SERVICE_* metadata are encoded as DESC suffixes
     * (e.g. "DESC kind=cli|author=...|...") so the kernel installer
     * can ignore them while pkg list / pkg info can surface them
     * later. */
    fprintf(out, "TPKG 1\n");
    fprintf(out, "NAME %s\n", m.name);
    fprintf(out, "VERSION %s\n", m.version);

    /* Pack the SDK metadata into a structured DESC so the kernel
     * installer (which only consumes NAME/VERSION/FILE/APP/BODY)
     * keeps working unchanged. Future M33 can add a richer
     * dedicated header line set if needed. */
    {
        char desc[400];
        size_t n = 0;
        n += snprintf(desc + n, sizeof(desc) - n, "kind=%s", m.kind);
        if (m.description[0]) n += snprintf(desc + n, sizeof(desc) - n, "|%s", m.description);
        if (m.author[0])      n += snprintf(desc + n, sizeof(desc) - n, "|author=%s", m.author);
        if (m.license[0])     n += snprintf(desc + n, sizeof(desc) - n, "|license=%s", m.license);
        if (m.has_service && !strcmp(m.kind, "service")) {
            n += snprintf(desc + n, sizeof(desc) - n,
                          "|restart=%s|autostart=%d",
                          m.service_restart, m.service_autostart);
        }
        fprintf(out, "DESC %s\n", desc);
    }

    if (m.has_launcher) {
        /* The kernel APP line wants "label|exec_full_path". Resolve
         * the launcher.exec (which is a dest-in-pkg) against the
         * install prefix so the registered launcher entry points at
         * the actual installed file. */
        for (int i = 0; i < m.nfiles; i++) {
            if (!strcmp(m.files[i].dest_in_pkg, m.launcher_exec)) {
                fprintf(out, "APP %s|%s\n",
                        m.launcher_label, m.files[i].full_dest);
                break;
            }
        }
    }

    for (int i = 0; i < m.nfiles; i++) {
        fprintf(out, "FILE %s %ld\n", m.files[i].full_dest, m.files[i].size);
    }
    fprintf(out, "BODY\n");

    /* Body: raw payloads, concatenated. */
    for (int i = 0; i < m.nfiles; i++) {
        if (m.files[i].size > 0 &&
            fwrite(m.files[i].payload, 1, (size_t)m.files[i].size, out) !=
            (size_t)m.files[i].size) {
            die(3, "write failed for '%s'", m.files[i].full_dest);
        }
        if (verbose) {
            fprintf(stderr, "[pkgbuild]   %s  <- %s  (%ld bytes)\n",
                    m.files[i].full_dest, m.files[i].src_on_host, m.files[i].size);
        }
    }

    fclose(out);
    for (int i = 0; i < m.nfiles; i++) free(m.files[i].payload);

    fprintf(stderr,
            "[pkgbuild] wrote %s (name=%s version=%s kind=%s files=%d body=%ld bytes)\n",
            out_path, m.name, m.version, m.kind, m.nfiles, total_body);
    return 0;
}
