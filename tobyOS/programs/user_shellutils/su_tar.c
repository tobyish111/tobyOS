/* su_tar.c -- tar, as GNU tar 1.35's (logs/utilgate/staging/tar.sh).
 *
 *   tar {-c|-t|-x|-d|-r|-u|-A|--delete|--test-label} [OPTION...] [FILE]...
 *
 * Everything here was measured on GNU tar 1.35 in WSL (/usr/bin/tar),
 * case by case, never read from its source; the help text is this
 * project's own words. What the fixture holds it to:
 *
 *   - The archive's bytes, format by format (gnu, oldgnu, ustar, posix/pax,
 *     v7): every header field, long names ('L' and 'K' ././@LongLink in
 *     the gnu formats, the ustar prefix split, pax path/linkpath/uid/
 *     size/mtime/uname keywords and their order, := overrides and global
 *     headers), base-256 numbers, sparse members (gnu 'S' with extension
 *     blocks, pax 0.0/0.1/1.0), labels, multi-volume 'M' headers, the
 *     end's two zero blocks and the record padding.
 *   - Names: leading '/' and '..' prefixes stripped with "Removing leading
 *     `X' from member names" (and "hard link targets"), -P, -C in order
 *     (and per name when extracting), -T (lines, --null, options inside,
 *     unquoting), --add-file, excludes (with GNU's per-option matching
 *     defaults), tags, caches, ignore files, --transform.
 *   - Listings (-tv's columns and their widening, -R, --full-time, the
 *     quoting styles), extraction with every overwrite mode, backups,
 *     -O, --to-command, -d's reports, -r/-u/-A/--delete, -W, -g/-G, -M,
 *     compression through gzip/bzip2/xz run as GNU runs them, --totals,
 *     --checkpoint, the messages and exit statuses (2 fatal or failed,
 *     1 different or changed), argp's option errors (status 64).
 *
 * tobyOS: libtoby has no strtoumax, fnmatch, getpwnam, mknod or
 * nanosecond times -- the first three are done here (the user and group
 * databases read from /etc/passwd and /etc/group), a device node cannot
 * be made ("Cannot mknod: Operation not permitted", what GNU says to a
 * user), holes are found by reading (there is no SEEK_DATA), and local
 * time is UTC. Times are to the nanosecond on both since finding 239 --
 * libtoby's stat has them -- and a device node is made for root since
 * finding 214 (libtoby's mknod). */

#include <sys/time.h>
#include <sys/sysmacros.h>
#ifdef SU_HOST
#include <pwd.h>
#include <grp.h>
#endif

#define TAR_BLK 512
#define TAR_EXIT_DIFFER 1
#define TAR_EXIT_FAILURE 2

/* the header's fields (see "headers: writing") */
#define TAR_H_NAME 0
#define TAR_H_MODE 100
#define TAR_H_UID 108
#define TAR_H_GID 116
#define TAR_H_SIZE 124
#define TAR_H_MTIME 136
#define TAR_H_CHKSUM 148
#define TAR_H_TYPE 156
#define TAR_H_LINK 157
#define TAR_H_MAGIC 257
#define TAR_H_VERSION 263
#define TAR_H_UNAME 265
#define TAR_H_GNAME 297
#define TAR_H_DEVMAJ 329
#define TAR_H_DEVMIN 337
#define TAR_H_PREFIX 345
/* oldgnu/gnu extensions in the prefix's place */
#define TAR_H_ATIME 345
#define TAR_H_CTIME 357
#define TAR_H_OFFSET 369
#define TAR_H_LONGNAMES 381
#define TAR_H_SPARSE 386
#define TAR_H_ISEXTENDED 482
#define TAR_H_REALSIZE 483

/* ---- the run's state ---------------------------------------------------- */

enum { TAR_C_NONE, TAR_C_CREATE, TAR_C_LIST, TAR_C_EXTRACT, TAR_C_DIFF,
       TAR_C_APPEND, TAR_C_UPDATE, TAR_C_CAT, TAR_C_DELETE, TAR_C_TESTLABEL };
enum { TAR_F_DEFAULT, TAR_F_V7, TAR_F_OLDGNU, TAR_F_USTAR, TAR_F_POSIX,
       TAR_F_STAR, TAR_F_GNU };
enum { TAR_OLD_DEFAULT, TAR_OLD_NO_OVERWRITE_DIR, TAR_OLD_OVERWRITE,
       TAR_OLD_UNLINK_FIRST, TAR_OLD_KEEP_OLD, TAR_OLD_SKIP_OLD,
       TAR_OLD_KEEP_NEWER };
enum { TAR_Q_LITERAL, TAR_Q_SHELL, TAR_Q_SHELL_ALWAYS, TAR_Q_SHELL_ESCAPE,
       TAR_Q_SHELL_ESCAPE_ALWAYS, TAR_Q_C, TAR_Q_C_MAYBE, TAR_Q_ESCAPE,
       TAR_Q_LOCALE, TAR_Q_CLOCALE };
enum { TAR_SORT_NONE, TAR_SORT_NAME, TAR_SORT_INODE };

/* one -C, -T or file operand, in the order given */
struct tar_name {
    char *name;
    int dir;              /* index into tar_dirs: the -C in force for it */
    int is_dir_change;    /* a -C entry, not a name */
    int matched;          /* found in the archive (extract/list/diff) */
    uintmax_t found_count;/* --occurrence */
    int wildcards;        /* matching flags in force when it was given */
    int anchored, icase, wslash;
    int recursion;
};

/* --exclude patterns, each with the matching flags in force */
struct tar_excl {
    char *pat;
    int wildcards, anchored, icase, wslash;
};

/* one s/// of --transform */
struct tar_xf {
    regex_t re;
    char *repl;
    int global, nth, icase, ext;
    int flags;            /* TAR_XF_* : which kinds of name it applies to */
    int has_re;
};
#define TAR_XF_REG  1     /* r: regular names */
#define TAR_XF_SYM  2     /* s: symbolic link targets */
#define TAR_XF_HARD 4     /* h: hard link targets */

/* a decoded header -- what one member is */
struct tar_member {
    char *name;           /* as stored (long names and pax applied) */
    char *link;           /* link target */
    char type;
    mode_t mode;          /* permission bits */
    uintmax_t uid, gid;
    char *uname, *gname;
    intmax_t mtime; long mtime_ns;
    intmax_t atime; long atime_ns; int has_atime;
    intmax_t ctime; long ctime_ns; int has_ctime;
    uintmax_t size;       /* bytes of data that follow in the archive */
    uintmax_t realsize;   /* the file's size (sparse: larger than size) */
    unsigned long devmaj, devmin;
    int is_sparse;
    uintmax_t *sp_off, *sp_len;   /* the sparse map */
    size_t sp_n;
    int sp_major, sp_minor;       /* pax sparse version */
    char hdr[TAR_BLK];    /* the header block itself */
    uintmax_t hdr_block;  /* its block number (-R), counting from 0 */
    uintmax_t xhdr_block; /* the block number of its first long-name/pax block */
    int fmt;              /* the format the header says */
    char *dumpdir;        /* incremental 'D' contents */
    uintmax_t mv_offset;  /* 'M': where the member continues */
    size_t dumpdir_len;
};

static struct {
    int cmd;
    const char *cmd_opt;          /* for "You may not specify more than one" */
    char **archives; int narchives;
    int format;
    int verbose;
    int blocking;                 /* in blocks */
    size_t record_size;
    int record_size_set;
    int old_files;
    int ignore_zeros;
    int absolute;
    int dereference;
    int hard_deref;
    int to_stdout;
    const char *to_command;
    int ignore_cmd_error;
    int keep_dir_symlink;
    int recursive_unlink;
    int remove_files;
    int touch;                    /* -m */
    int same_perms; int same_perms_set;
    int same_owner; int same_owner_set;
    int numeric_owner;
    int one_fs;
    int sparse; int sparse_major, sparse_minor; int sparse_ver_set;
    int hole_seek;                /* --hole-detection */
    int recursion;
    int null_names;
    int verbatim;
    int unquote;
    int wildcards, anchored, icase, wslash;   /* current matching flags */
    int wildcards_set;            /* --wildcards / --no-wildcards seen */
    int anchored_set;             /* --anchored / --no-anchored seen */
    int sort;
    const char *owner_name; uintmax_t owner_uid; int owner_set;
    const char *group_name; uintmax_t group_gid; int group_set;
    struct su_mode mode; int mode_set;
    intmax_t mtime_s; long mtime_ns; int mtime_set; int clamp_mtime;
    intmax_t newer_s; long newer_ns; int newer_set; int newer_mtime_only;
    int strip;
    int occurrence;
    const char *starting_file;
    int totals;
    int checkpoint; int checkpoint_set;
    char **cp_actions; int ncp_actions;
    const char *index_file;
    FILE *vout;                   /* where -v goes: stdout, or stderr when the archive is stdout */
    int block_number;
    int quoting; char quote_extra[256];
    int utc, full_time;
    int show_transformed;
    int show_omitted;
    int interactive;
    int check_links;
    int delay_dir_restore;
    int no_overwrite_dir;
    const char *label;
    const char *compress;         /* the program, or 0 */
    int auto_compress;
    const char *one_top_level; int one_top_set;
    int backup;                   /* 0 none, 1 simple, 2 existing, 3 numbered */
    const char *suffix;
    int ignore_failed_read;
    int atime_preserve;
    int seek; int seek_set;
    int multi_volume;
    uintmax_t tape_length;
    const char *info_script;
    const char *volno_file;
    int incremental;              /* -G */
    const char *listed_incr;      /* -g FILE */
    int level; int level_set;
    int check_device;
    int restrict_;
    int warn_off, warn_on;        /* --warning bits */
    int pax_set;                  /* --pax-option given */
    int read_full;
    int force_local;
    int preserve_order;
    int xattrs, acls, selinux;
    char *exthdr_name, *globexthdr_name;
    intmax_t exthdr_mtime; int exthdr_mtime_set;
    intmax_t globexthdr_mtime; int globexthdr_mtime_set;
    char **pax_delete; int npax_delete;
    char **pax_kw; char **pax_val; int *pax_global; int npax;
    int pax_invalid_set;
    int verify;
    const char *rsh, *rmt;
    const char *owner_map, *group_map;
    int exclude_vcs, exclude_vcs_ignores, exclude_backups;
    int exclude_caches;           /* 0, 1 contents, 2 under, 3 all */
    char **excl_tag; int *excl_tag_kind; int nexcl_tag;
    char **excl_ignore; int *excl_ignore_rec; int nexcl_ignore;
} tar_o;

static struct tar_name *tar_names; static int tar_nnames, tar_names_cap;
static char **tar_dirs; static int tar_ndirs, tar_dirs_cap;
static struct tar_excl *tar_excls; static int tar_nexcls, tar_excls_cap;
static struct tar_xf *tar_xfs; static int tar_nxfs, tar_xfs_cap;
static int tar_status;            /* the exit status so far */
static int tar_to_dev_null;       /* the archive is /dev/null */
static int tar_verify_fd = -1;     /* -W: the archive, opened where tar started */
static int tar_multi_names;       /* more than one name (or a -T) was given */
static void tar_report_trailing(void);
static void tar_member_dir(int which);
static int tar_rm_tree(const char *name);
static char **tar_trailing; static int tar_ntrailing;   /* positional options after the names */

/* ---- memory --------------------------------------------------------------- */

/* strtoumax, base 10 (libtoby has none): blanks, an optional '+', digits;
 * ERANGE (and UINTMAX_MAX) past the top */
static uintmax_t tar_strtoumax(const char *s, char **end, int base) {
    (void)base;
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) { if (end) *end = (char *)s; return 0; }
    uintmax_t v = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    if (end) *end = (char *)p;
    if (over) { errno = ERANGE; return UINTMAX_MAX; }
    return v;
}

static void tar_nomem(void) __attribute__((noreturn));
static void tar_nomem(void) {
    fflush(stdout);
    fprintf(stderr, "%s: memory exhausted\n", su_name);
    exit(TAR_EXIT_FAILURE);
}
static void *tar_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) tar_nomem();
    return p;
}
static void *tar_zalloc(size_t n) {
    void *p = tar_alloc(n);
    memset(p, 0, n);
    return p;
}
static void *tar_realloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) tar_nomem();
    return p;
}
static char *tar_strdup(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)tar_alloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}
static char *tar_strndup(const char *s, size_t n) {
    char *p = (char *)tar_alloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
static void tar_grow(void **v, int *cap, int n, size_t elem) {
    if (n < *cap) return;
    *cap = *cap ? *cap * 2 : 16;
    *v = tar_realloc(*v, (size_t)*cap * elem);
}

/* a growable string */
struct tar_sb { char *s; size_t n, cap; };
static void tar_sb_need(struct tar_sb *b, size_t n) {
    if (b->n + n + 1 <= b->cap) return;
    size_t c = b->cap ? b->cap : 64;
    while (c < b->n + n + 1) c *= 2;
    b->s = (char *)tar_realloc(b->s, c);
    b->cap = c;
}
static void tar_sb_addn(struct tar_sb *b, const char *s, size_t n) {
    tar_sb_need(b, n);
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void tar_sb_add(struct tar_sb *b, const char *s) { tar_sb_addn(b, s, strlen(s)); }
static void tar_sb_addc(struct tar_sb *b, char c) { tar_sb_addn(b, &c, 1); }
static void tar_sb_printf(struct tar_sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tar_sb_printf(struct tar_sb *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { tar_sb_addn(b, tmp, (size_t)n); return; }
    tar_sb_need(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
}
static char *tar_sb_take(struct tar_sb *b) {
    if (!b->s) return tar_strdup("");
    char *s = b->s;
    b->s = 0; b->n = b->cap = 0;
    return s;
}

/* ---- quoting ----------------------------------------------------------------
 *
 * GNU tar quotes a name the way --quoting-style says (escape by default),
 * the same in listings and messages, except that a message also escapes
 * ':' ("a\:b: Cannot stat"). Measured in the C locale, style by style:
 *
 *   escape    bare; \\ \a \b \f \n \r \t \v, and \OOO for any other byte
 *             that is not printable (bytes from 0x80 too)
 *   c         the same inside "...", with \" ; clocale is c
 *   c-maybe   bare unless a byte needs an escape (then as c); a lone
 *             backslash alone does not make it need one
 *   locale    escape inside '...', with \'
 *   literal   as it is
 *   shell     bare unless it holds a blank or one of !"$&'()*;<=>?[\^`|
 *             (or starts with # or ~, or is empty); then '...', or "..."
 *             when it holds a ' and nothing that is special in "...";
 *             otherwise each ' as '\''. Control bytes go out as they are.
 *   shell-escape  as shell, but a control byte (or any from 0x80) closes
 *             the quote for a $'\OOO' -- 'nl'$'\n''x'
 *   *-always  quoted even when nothing needs it
 * --quote-chars=S adds S's bytes to those escaped with a backslash. */

static const char *tar_qstyle_names[] = {
    "literal", "shell", "shell-always", "shell-escape", "shell-escape-always",
    "c", "c-maybe", "escape", "locale", "clocale", 0
};

static int tar_q_printable(unsigned char c) { return c >= 0x20 && c < 0x7f; }

static const char *tar_c_escape(unsigned char c) {
    switch (c) {
    case '\a': return "\\a"; case '\b': return "\\b"; case '\f': return "\\f";
    case '\n': return "\\n"; case '\r': return "\\r"; case '\t': return "\\t";
    case '\v': return "\\v";
    }
    return 0;
}

static int tar_shell_special(const char *s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (i == 0 && (c == '#' || c == '~')) return 1;
    return c == ' ' || c == '\t' || c == '\n' || strchr("!\"$&'()*;<=>?[\\^`|", c) != 0;
}

/* S quoted in STYLE into B; COLON also escapes ':' (messages) */
static void tar_quote_into(struct tar_sb *b, const char *s, int style, int colon) {
    size_t n = strlen(s);
    int extra_any = 0;
    for (size_t i = 0; i < n; i++)
        if (tar_o.quote_extra[(unsigned char)s[i]] || (colon && s[i] == ':')) extra_any = 1;
    switch (style) {
    case TAR_Q_LITERAL:
        tar_sb_add(b, s);
        return;
    case TAR_Q_SHELL: case TAR_Q_SHELL_ALWAYS:
    case TAR_Q_SHELL_ESCAPE: case TAR_Q_SHELL_ESCAPE_ALWAYS: {
        int esc = style == TAR_Q_SHELL_ESCAPE || style == TAR_Q_SHELL_ESCAPE_ALWAYS;
        int always = style == TAR_Q_SHELL_ALWAYS || style == TAR_Q_SHELL_ESCAPE_ALWAYS;
        int need = always || n == 0, sq = 0, dqbad = 0, ctrl = 0;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (tar_shell_special(s, i)) need = 1;
            if (c == '\'') sq = 1;
            if (c == '$' || c == '`' || c == '"' || c == '\\') dqbad = 1;
            if (esc && !tar_q_printable(c)) { need = 1; ctrl = 1; }
        }
        if (!need) { tar_sb_add(b, s); return; }
        if (sq && !dqbad && !ctrl) {
            tar_sb_addc(b, '"'); tar_sb_add(b, s); tar_sb_addc(b, '"');
            return;
        }
        tar_sb_addc(b, '\'');
        int inq = 1;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (esc && !tar_q_printable(c)) {
                if (inq) { tar_sb_add(b, "'$'"); inq = 0; }
                const char *e = tar_c_escape(c);
                if (e) tar_sb_add(b, e);
                else tar_sb_printf(b, "\\%03o", c);
                continue;
            }
            if (!inq) { tar_sb_add(b, "''"); inq = 1; }
            if (c == '\'') tar_sb_add(b, "'\\''");
            else tar_sb_addc(b, (char)c);
        }
        tar_sb_addc(b, '\'');
        return;
    }
    default: break;
    }
    /* the backslash styles */
    int quotes = 0;            /* 1 "...", 2 '...' */
    if (style == TAR_Q_C || style == TAR_Q_CLOCALE) quotes = 1;
    else if (style == TAR_Q_LOCALE) quotes = 2;
    else if (style == TAR_Q_C_MAYBE) {
        int need = extra_any;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (!tar_q_printable(c) || c == '"') need = 1;
        }
        if (!need) { tar_sb_add(b, s); return; }
        quotes = 1;
    }
    if (quotes == 1) tar_sb_addc(b, '"');
    else if (quotes == 2) tar_sb_addc(b, '\'');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *e = tar_c_escape(c);
        if (c == '\\') tar_sb_add(b, "\\\\");
        else if (e) tar_sb_add(b, e);
        else if (!tar_q_printable(c)) tar_sb_printf(b, "\\%03o", c);
        else if (quotes == 1 && c == '"') tar_sb_add(b, "\\\"");
        else if (quotes == 2 && c == '\'') tar_sb_add(b, "\\'");
        else if (tar_o.quote_extra[c] || (colon && c == ':')) {
            tar_sb_addc(b, '\\'); tar_sb_addc(b, (char)c);
        } else tar_sb_addc(b, (char)c);
    }
    if (quotes == 1) tar_sb_addc(b, '"');
    else if (quotes == 2) tar_sb_addc(b, '\'');
}

/* rotating result buffers, so a message can hold several names */
static char *tar_qbufs[8]; static int tar_qslot;
static const char *tar_qkeep(char *s) {
    int i = tar_qslot++ & 7;
    free(tar_qbufs[i]);
    tar_qbufs[i] = s;
    return s;
}
/* a name in a listing */
static const char *tar_qname(const char *s) {
    struct tar_sb b = {0};
    tar_quote_into(&b, s, tar_o.quoting, 0);
    return tar_qkeep(tar_sb_take(&b));
}
/* a name in a message (quotearg_colon) */
static const char *tar_qc(const char *s) {
    struct tar_sb b = {0};
    tar_quote_into(&b, s, tar_o.quoting, 1);
    return tar_qkeep(tar_sb_take(&b));
}
/* 'NAME' -- quote(), as in "Missing links to 'x'." and "Cannot open 'x'" */
static const char *tar_q(const char *s) {
    struct tar_sb b = {0};
    tar_quote_into(&b, s, TAR_Q_LOCALE, 0);
    return tar_qkeep(tar_sb_take(&b));
}

/* ---- messages ---------------------------------------------------------------
 *
 * "tar: MESSAGE" on stderr, stdout flushed first, prefixed "block N: "
 * under -R. An error makes the final status 2 (and the closing "Exiting
 * with failure status due to previous errors"); a warning changes
 * nothing; a fatal error ends the run with "Error is not recoverable:
 * exiting now". */

static uintmax_t tar_cur_block;   /* -R: the block a message is about */
static int tar_msg_block;         /* -R in force while reading */

static void tar_vmsg(const char *fmt, va_list ap) {
    fflush(stdout);
    if (tar_o.vout && tar_o.vout != stdout) fflush(tar_o.vout);
    fprintf(stderr, "%s: ", su_name);
    if (tar_msg_block) fprintf(stderr, "block %ju: ", tar_cur_block);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
static void tar_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tar_warn(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); tar_vmsg(fmt, ap); va_end(ap);
}
static void tar_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tar_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); tar_vmsg(fmt, ap); va_end(ap);
    tar_status = TAR_EXIT_FAILURE;
}
static void tar_cleanup_children(void);
static int tar_confirm(const char *action, const char *name);
static void tar_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void tar_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); tar_vmsg(fmt, ap); va_end(ap);
    fprintf(stderr, "%s: Error is not recoverable: exiting now\n", su_name);
    tar_cleanup_children();
    exit(TAR_EXIT_FAILURE);
}
/* a usage error found after the options were read */
static void tar_usage_error(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void tar_usage_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fflush(stdout);
    fprintf(stderr, "%s: ", su_name);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fprintf(stderr, "Try '%s --help' or '%s --usage' for more information.\n", su_name, su_name);
    exit(TAR_EXIT_FAILURE);
}
/* a getopt-level error: argp's "Try" names the short name, status 64 */
static void tar_try_help64(void) __attribute__((noreturn));
static void tar_try_help64(void) {
    fflush(stdout);
    fprintf(stderr, "Try '%s --help' or '%s --usage' for more information.\n", su_short, su_short);
    exit(64);
}

/* "NAME: WHAT: strerror" -- the common shape */
static void tar_errno(const char *name, const char *what, int e) {
    tar_err("%s: %s: %s", tar_qc(name), what, strerror(e));
}

/* ---- options ----------------------------------------------------------------
 *
 * GNU tar reads its options with argp: getopt_long's rules and messages
 * (status 64 for those, "Try 'tar --help' or 'tar --usage'"), operands
 * taken in order among the options (-C between names applies to the
 * names after it), and a first argument with no dash read as a cluster
 * of old-style letters whose arguments are the words after it ("tar cfb
 * x.tar 20 a"). The long options are in GNU's order, letter by letter,
 * as its ambiguity messages list them. */

enum {
    TO_APPEND = 0x100, TO_ATIME_PRESERVE, TO_ACLS, TO_AUTO_COMPRESS, TO_ABSOLUTE,
    TO_AFTER_DATE, TO_ADD_FILE, TO_ANCHORED, TO_BLOCKING, TO_BZIP2, TO_BACKUP,
    TO_BLOCK_NUMBER, TO_CREATE, TO_COMPARE, TO_CATENATE, TO_CHECK_DEVICE,
    TO_CLAMP_MTIME, TO_COMPRESS, TO_CHECKPOINT, TO_CHECKPOINT_ACTION,
    TO_CHECK_LINKS, TO_CONFIRMATION, TO_DIFF, TO_DELETE, TO_DELAY_DIR,
    TO_DEREFERENCE, TO_DIRECTORY, TO_EXTRACT, TO_EXCLUDE, TO_EXCLUDE_FROM,
    TO_EXCLUDE_CACHES, TO_EXCLUDE_CACHES_UNDER, TO_EXCLUDE_CACHES_ALL,
    TO_EXCLUDE_TAG, TO_EXCLUDE_IGNORE, TO_EXCLUDE_IGNORE_REC,
    TO_EXCLUDE_TAG_UNDER, TO_EXCLUDE_TAG_ALL, TO_EXCLUDE_VCS,
    TO_EXCLUDE_VCS_IGNORES, TO_EXCLUDE_BACKUPS, TO_FILE, TO_FORCE_LOCAL,
    TO_FORMAT, TO_FULL_TIME, TO_FILES_FROM, TO_GROUP, TO_GROUP_MAP, TO_GZIP,
    TO_HOLE_DETECTION, TO_HARD_DEREF, TO_HELP, TO_INCREMENTAL,
    TO_IGNORE_FAILED_READ, TO_IGNORE_CMD_ERROR, TO_INFO_SCRIPT,
    TO_IGNORE_ZEROS, TO_INDEX_FILE, TO_INTERACTIVE, TO_IGNORE_CASE,
    TO_KEEP_OLD, TO_KEEP_NEWER, TO_KEEP_DIR_SYMLINK, TO_LIST, TO_LISTED_INCR,
    TO_LEVEL, TO_LABEL, TO_LZIP, TO_LZMA, TO_LZOP, TO_MTIME, TO_MODE,
    TO_MULTI_VOLUME, TO_NO_SEEK, TO_NO_CHECK_DEVICE, TO_NO_OVERWRITE_DIR,
    TO_NO_IGNORE_CMD_ERROR, TO_NO_SAME_OWNER, TO_NUMERIC_OWNER,
    TO_NO_SAME_PERMS, TO_NO_DELAY_DIR, TO_NO_XATTRS, TO_NO_SELINUX,
    TO_NO_ACLS, TO_NO_AUTO_COMPRESS, TO_NEWER, TO_NEWER_MTIME,
    TO_NO_QUOTE_CHARS, TO_NULL, TO_NO_NULL, TO_NO_UNQUOTE, TO_NO_VERBATIM,
    TO_NO_RECURSION, TO_NO_ANCHORED, TO_NO_IGNORE_CASE, TO_NO_WILDCARDS,
    TO_NO_WILDCARDS_SLASH, TO_OCCURRENCE, TO_OVERWRITE, TO_OVERWRITE_DIR,
    TO_ONE_TOP_LEVEL, TO_OWNER, TO_OWNER_MAP, TO_OLD_ARCHIVE, TO_ONE_FS,
    TO_PRESERVE_PERMS, TO_PRESERVE_ORDER, TO_POSIX, TO_PAX_OPTION,
    TO_PROGRAM_NAME, TO_QUOTING_STYLE, TO_QUOTE_CHARS, TO_REMOVE_FILES,
    TO_RECURSIVE_UNLINK, TO_RMT_COMMAND, TO_RSH_COMMAND, TO_RECORD_SIZE,
    TO_READ_FULL, TO_RESTRICT, TO_RECURSION, TO_SPARSE, TO_SPARSE_VERSION,
    TO_SEEK, TO_SKIP_OLD, TO_SAME_OWNER, TO_SORT, TO_SELINUX,
    TO_STARTING_FILE, TO_SUFFIX, TO_STRIP, TO_SHOW_DEFAULTS,
    TO_SHOW_SNAPSHOT, TO_SHOW_OMITTED, TO_SHOW_TRANSFORMED, TO_TEST_LABEL,
    TO_TO_STDOUT, TO_TO_COMMAND, TO_TOUCH, TO_TAPE_LENGTH, TO_TRANSFORM,
    TO_TOTALS, TO_UPDATE, TO_UNLINK_FIRST, TO_USE_COMPRESS, TO_UTC,
    TO_UNQUOTE, TO_USAGE, TO_VERIFY, TO_VOLNO_FILE, TO_VERBOSE,
    TO_VERBATIM, TO_VERSION, TO_WARNING, TO_WILDCARDS, TO_WILDCARDS_SLASH,
    TO_XATTRS, TO_XATTRS_INCLUDE, TO_XATTRS_EXCLUDE, TO_XZ, TO_ZSTD,
};

static const struct su_option tar_longopts[] = {
    { "append", SU_NO_ARG, 0, 'r' },
    { "atime-preserve", SU_OPT_ARG, 0, TO_ATIME_PRESERVE },
    { "acls", SU_NO_ARG, 0, TO_ACLS },
    { "auto-compress", SU_NO_ARG, 0, 'a' },
    { "absolute-names", SU_NO_ARG, 0, 'P' },
    { "after-date", SU_REQ_ARG, 0, TO_AFTER_DATE },
    { "add-file", SU_REQ_ARG, 0, TO_ADD_FILE },
    { "anchored", SU_NO_ARG, 0, TO_ANCHORED },
    { "blocking-factor", SU_REQ_ARG, 0, 'b' },
    { "bzip2", SU_NO_ARG, 0, 'j' },
    { "backup", SU_OPT_ARG, 0, TO_BACKUP },
    { "block-number", SU_NO_ARG, 0, 'R' },
    { "create", SU_NO_ARG, 0, 'c' },
    { "compare", SU_NO_ARG, 0, 'd' },
    { "catenate", SU_NO_ARG, 0, 'A' },
    { "concatenate", SU_NO_ARG, 0, 'A' },
    { "check-device", SU_NO_ARG, 0, TO_CHECK_DEVICE },
    { "clamp-mtime", SU_NO_ARG, 0, TO_CLAMP_MTIME },
    { "compress", SU_NO_ARG, 0, 'Z' },
    { "checkpoint", SU_OPT_ARG, 0, TO_CHECKPOINT },
    { "checkpoint-action", SU_REQ_ARG, 0, TO_CHECKPOINT_ACTION },
    { "check-links", SU_NO_ARG, 0, 'l' },
    { "confirmation", SU_NO_ARG, 0, 'w' },
    { "diff", SU_NO_ARG, 0, 'd' },
    { "delete", SU_NO_ARG, 0, TO_DELETE },
    { "delay-directory-restore", SU_NO_ARG, 0, TO_DELAY_DIR },
    { "dereference", SU_NO_ARG, 0, 'h' },
    { "directory", SU_REQ_ARG, 0, 'C' },
    { "extract", SU_NO_ARG, 0, 'x' },
    { "exclude", SU_REQ_ARG, 0, TO_EXCLUDE },
    { "exclude-from", SU_REQ_ARG, 0, 'X' },
    { "exclude-caches", SU_NO_ARG, 0, TO_EXCLUDE_CACHES },
    { "exclude-caches-under", SU_NO_ARG, 0, TO_EXCLUDE_CACHES_UNDER },
    { "exclude-caches-all", SU_NO_ARG, 0, TO_EXCLUDE_CACHES_ALL },
    { "exclude-tag", SU_REQ_ARG, 0, TO_EXCLUDE_TAG },
    { "exclude-ignore", SU_REQ_ARG, 0, TO_EXCLUDE_IGNORE },
    { "exclude-ignore-recursive", SU_REQ_ARG, 0, TO_EXCLUDE_IGNORE_REC },
    { "exclude-tag-under", SU_REQ_ARG, 0, TO_EXCLUDE_TAG_UNDER },
    { "exclude-tag-all", SU_REQ_ARG, 0, TO_EXCLUDE_TAG_ALL },
    { "exclude-vcs", SU_NO_ARG, 0, TO_EXCLUDE_VCS },
    { "exclude-vcs-ignores", SU_NO_ARG, 0, TO_EXCLUDE_VCS_IGNORES },
    { "exclude-backups", SU_NO_ARG, 0, TO_EXCLUDE_BACKUPS },
    { "file", SU_REQ_ARG, 0, 'f' },
    { "force-local", SU_NO_ARG, 0, TO_FORCE_LOCAL },
    { "format", SU_REQ_ARG, 0, 'H' },
    { "full-time", SU_NO_ARG, 0, TO_FULL_TIME },
    { "files-from", SU_REQ_ARG, 0, 'T' },
    { "get", SU_NO_ARG, 0, 'x' },
    { "group", SU_REQ_ARG, 0, TO_GROUP },
    { "group-map", SU_REQ_ARG, 0, TO_GROUP_MAP },
    { "gzip", SU_NO_ARG, 0, 'z' },
    { "gunzip", SU_NO_ARG, 0, 'z' },
    { "hole-detection", SU_REQ_ARG, 0, TO_HOLE_DETECTION },
    { "hard-dereference", SU_NO_ARG, 0, TO_HARD_DEREF },
    { "help", SU_NO_ARG, 0, '?' },
    { "incremental", SU_NO_ARG, 0, 'G' },
    { "ignore-failed-read", SU_NO_ARG, 0, TO_IGNORE_FAILED_READ },
    { "ignore-command-error", SU_NO_ARG, 0, TO_IGNORE_CMD_ERROR },
    { "info-script", SU_REQ_ARG, 0, 'F' },
    { "ignore-zeros", SU_NO_ARG, 0, 'i' },
    { "index-file", SU_REQ_ARG, 0, TO_INDEX_FILE },
    { "interactive", SU_NO_ARG, 0, 'w' },
    { "ignore-case", SU_NO_ARG, 0, TO_IGNORE_CASE },
    { "keep-old-files", SU_NO_ARG, 0, 'k' },
    { "keep-newer-files", SU_NO_ARG, 0, TO_KEEP_NEWER },
    { "keep-directory-symlink", SU_NO_ARG, 0, TO_KEEP_DIR_SYMLINK },
    { "list", SU_NO_ARG, 0, 't' },
    { "listed-incremental", SU_REQ_ARG, 0, 'g' },
    { "level", SU_REQ_ARG, 0, TO_LEVEL },
    { "label", SU_REQ_ARG, 0, 'V' },
    { "lzip", SU_NO_ARG, 0, TO_LZIP },
    { "lzma", SU_NO_ARG, 0, TO_LZMA },
    { "lzop", SU_NO_ARG, 0, TO_LZOP },
    { "mtime", SU_REQ_ARG, 0, TO_MTIME },
    { "mode", SU_REQ_ARG, 0, TO_MODE },
    { "multi-volume", SU_NO_ARG, 0, 'M' },
    { "no-seek", SU_NO_ARG, 0, TO_NO_SEEK },
    { "no-check-device", SU_NO_ARG, 0, TO_NO_CHECK_DEVICE },
    { "no-overwrite-dir", SU_NO_ARG, 0, TO_NO_OVERWRITE_DIR },
    { "no-ignore-command-error", SU_NO_ARG, 0, TO_NO_IGNORE_CMD_ERROR },
    { "no-same-owner", SU_NO_ARG, 0, TO_NO_SAME_OWNER },
    { "numeric-owner", SU_NO_ARG, 0, TO_NUMERIC_OWNER },
    { "no-same-permissions", SU_NO_ARG, 0, TO_NO_SAME_PERMS },
    { "no-delay-directory-restore", SU_NO_ARG, 0, TO_NO_DELAY_DIR },
    { "no-xattrs", SU_NO_ARG, 0, TO_NO_XATTRS },
    { "no-selinux", SU_NO_ARG, 0, TO_NO_SELINUX },
    { "no-acls", SU_NO_ARG, 0, TO_NO_ACLS },
    { "new-volume-script", SU_REQ_ARG, 0, 'F' },
    { "no-auto-compress", SU_NO_ARG, 0, TO_NO_AUTO_COMPRESS },
    { "newer", SU_REQ_ARG, 0, 'N' },
    { "newer-mtime", SU_REQ_ARG, 0, TO_NEWER_MTIME },
    { "no-quote-chars", SU_REQ_ARG, 0, TO_NO_QUOTE_CHARS },
    { "null", SU_NO_ARG, 0, TO_NULL },
    { "no-null", SU_NO_ARG, 0, TO_NO_NULL },
    { "no-unquote", SU_NO_ARG, 0, TO_NO_UNQUOTE },
    { "no-verbatim-files-from", SU_NO_ARG, 0, TO_NO_VERBATIM },
    { "no-recursion", SU_NO_ARG, 0, TO_NO_RECURSION },
    { "no-anchored", SU_NO_ARG, 0, TO_NO_ANCHORED },
    { "no-ignore-case", SU_NO_ARG, 0, TO_NO_IGNORE_CASE },
    { "no-wildcards", SU_NO_ARG, 0, TO_NO_WILDCARDS },
    { "no-wildcards-match-slash", SU_NO_ARG, 0, TO_NO_WILDCARDS_SLASH },
    { "occurrence", SU_OPT_ARG, 0, TO_OCCURRENCE },
    { "overwrite", SU_NO_ARG, 0, TO_OVERWRITE },
    { "overwrite-dir", SU_NO_ARG, 0, TO_OVERWRITE_DIR },
    { "one-top-level", SU_OPT_ARG, 0, TO_ONE_TOP_LEVEL },
    { "owner", SU_REQ_ARG, 0, TO_OWNER },
    { "owner-map", SU_REQ_ARG, 0, TO_OWNER_MAP },
    { "old-archive", SU_NO_ARG, 0, TO_OLD_ARCHIVE },
    { "one-file-system", SU_NO_ARG, 0, TO_ONE_FS },
    { "preserve-permissions", SU_NO_ARG, 0, 'p' },
    { "preserve-order", SU_NO_ARG, 0, 's' },
    { "portability", SU_NO_ARG, 0, TO_OLD_ARCHIVE },
    { "posix", SU_NO_ARG, 0, TO_POSIX },
    { "pax-option", SU_REQ_ARG, 0, TO_PAX_OPTION },
    { "program-name", SU_REQ_ARG, 0, TO_PROGRAM_NAME },
    { "quoting-style", SU_REQ_ARG, 0, TO_QUOTING_STYLE },
    { "quote-chars", SU_REQ_ARG, 0, TO_QUOTE_CHARS },
    { "remove-files", SU_NO_ARG, 0, TO_REMOVE_FILES },
    { "recursive-unlink", SU_NO_ARG, 0, TO_RECURSIVE_UNLINK },
    { "rmt-command", SU_REQ_ARG, 0, TO_RMT_COMMAND },
    { "rsh-command", SU_REQ_ARG, 0, TO_RSH_COMMAND },
    { "record-size", SU_REQ_ARG, 0, TO_RECORD_SIZE },
    { "read-full-records", SU_NO_ARG, 0, 'B' },
    { "restrict", SU_NO_ARG, 0, TO_RESTRICT },
    { "recursion", SU_NO_ARG, 0, TO_RECURSION },
    { "sparse", SU_NO_ARG, 0, 'S' },
    { "sparse-version", SU_REQ_ARG, 0, TO_SPARSE_VERSION },
    { "seek", SU_NO_ARG, 0, 'n' },
    { "skip-old-files", SU_NO_ARG, 0, TO_SKIP_OLD },
    { "same-owner", SU_NO_ARG, 0, TO_SAME_OWNER },
    { "same-permissions", SU_NO_ARG, 0, 'p' },
    { "same-order", SU_NO_ARG, 0, 's' },
    { "sort", SU_REQ_ARG, 0, TO_SORT },
    { "selinux", SU_NO_ARG, 0, TO_SELINUX },
    { "starting-file", SU_REQ_ARG, 0, 'K' },
    { "suffix", SU_REQ_ARG, 0, TO_SUFFIX },
    { "strip-components", SU_REQ_ARG, 0, TO_STRIP },
    { "show-defaults", SU_NO_ARG, 0, TO_SHOW_DEFAULTS },
    { "show-snapshot-field-ranges", SU_NO_ARG, 0, TO_SHOW_SNAPSHOT },
    { "show-omitted-dirs", SU_NO_ARG, 0, TO_SHOW_OMITTED },
    { "show-transformed-names", SU_NO_ARG, 0, TO_SHOW_TRANSFORMED },
    { "show-stored-names", SU_NO_ARG, 0, TO_SHOW_TRANSFORMED },
    { "test-label", SU_NO_ARG, 0, TO_TEST_LABEL },
    { "to-stdout", SU_NO_ARG, 0, 'O' },
    { "to-command", SU_REQ_ARG, 0, TO_TO_COMMAND },
    { "touch", SU_NO_ARG, 0, 'm' },
    { "tape-length", SU_REQ_ARG, 0, 'L' },
    { "transform", SU_REQ_ARG, 0, TO_TRANSFORM },
    { "totals", SU_OPT_ARG, 0, TO_TOTALS },
    { "update", SU_NO_ARG, 0, 'u' },
    { "unlink-first", SU_NO_ARG, 0, 'U' },
    { "use-compress-program", SU_REQ_ARG, 0, 'I' },
    { "ungzip", SU_NO_ARG, 0, 'z' },
    { "uncompress", SU_NO_ARG, 0, 'Z' },
    { "utc", SU_NO_ARG, 0, TO_UTC },
    { "unquote", SU_NO_ARG, 0, TO_UNQUOTE },
    { "usage", SU_NO_ARG, 0, TO_USAGE },
    { "verify", SU_NO_ARG, 0, 'W' },
    { "volno-file", SU_REQ_ARG, 0, TO_VOLNO_FILE },
    { "verbose", SU_NO_ARG, 0, 'v' },
    { "verbatim-files-from", SU_NO_ARG, 0, TO_VERBATIM },
    { "version", SU_NO_ARG, 0, TO_VERSION },
    { "warning", SU_REQ_ARG, 0, TO_WARNING },
    { "wildcards", SU_NO_ARG, 0, TO_WILDCARDS },
    { "wildcards-match-slash", SU_NO_ARG, 0, TO_WILDCARDS_SLASH },
    { "xattrs", SU_NO_ARG, 0, TO_XATTRS },
    { "xattrs-include", SU_REQ_ARG, 0, TO_XATTRS_INCLUDE },
    { "xattrs-exclude", SU_REQ_ARG, 0, TO_XATTRS_EXCLUDE },
    { "xz", SU_NO_ARG, 0, 'J' },
    { "xform", SU_REQ_ARG, 0, TO_TRANSFORM },
    { "zstd", SU_NO_ARG, 0, TO_ZSTD },
    { 0, 0, 0, 0 }
};

/* the short options; '-' first: operands come back in order, as 1 */
static const char tar_shortopts[] =
    "-AcdrtuxGnSkUWOmpsMBiajJzZhPlRvwo?0123456789"
    "g:C:T:X:f:F:L:b:H:V:I:K:N:";

/* the short letters whose old-style form takes the next word */
static const char tar_old_args[] = "gCTXfFLbHVIKN";

/* --warning keywords, in GNU's order, and which are on by default */
static const char *tar_warn_names[] = {
    "all", "alone-zero-block", "bad-dumpdir", "cachedir", "contiguous-cast",
    "file-changed", "file-ignored", "file-removed", "file-shrank",
    "file-unchanged", "filename-with-nuls", "ignore-archive",
    "ignore-newer", "new-directory", "rename-directory", "symlink-cast",
    "timestamp", "unknown-cast", "unknown-keyword", "xdev",
    "decompress-program", "existing-file", "xattr-write", "record-size",
    "failed-read", "missing-zero-blocks", "verbose", 0
};
enum { TW_ALL, TW_ALONE_ZERO, TW_BAD_DUMPDIR, TW_CACHEDIR, TW_CONTIG,
       TW_FILE_CHANGED, TW_FILE_IGNORED, TW_FILE_REMOVED, TW_FILE_SHRANK,
       TW_FILE_UNCHANGED, TW_FILENAME_NULS, TW_IGNORE_ARCHIVE,
       TW_IGNORE_NEWER, TW_NEW_DIR, TW_RENAME_DIR, TW_SYMLINK_CAST,
       TW_TIMESTAMP, TW_UNKNOWN_CAST, TW_UNKNOWN_KEYWORD, TW_XDEV,
       TW_DECOMPRESS, TW_EXISTING_FILE, TW_XATTR_WRITE, TW_RECORD_SIZE,
       TW_FAILED_READ, TW_MISSING_ZERO, TW_VERBOSE };
static unsigned long tar_warnings;   /* bit per keyword: on */
static int tar_warn_explicit[32];    /* set by --warning (on, or no-) */

static int tar_warning_on(int w) { return (tar_warnings >> w) & 1; }

/* argmatch's "invalid/ambiguous argument" with its list: VALUES grouped
 * by synonym (GROUPS gives each value's group: same group, same line) */
static void tar_argmatch_die(const char *what, const char *arg, int ambiguous,
                             const char *const *values, const int *groups) __attribute__((noreturn));
static void tar_argmatch_die(const char *what, const char *arg, int ambiguous,
                             const char *const *values, const int *groups) {
    fflush(stdout);
    fprintf(stderr, "%s: %s argument %s for %s\n", su_name,
            ambiguous ? "ambiguous" : "invalid", su_quote(arg), su_quote(what));
    fprintf(stderr, "Valid arguments are:");
    for (int i = 0; values[i]; i++) {
        if (i == 0 || !groups || groups[i] != groups[i - 1])
            fprintf(stderr, "\n  - %s", su_quote(values[i]));
        else
            fprintf(stderr, ", %s", su_quote(values[i]));
    }
    fputc('\n', stderr);
    exit(TAR_EXIT_FAILURE);
}

/* the index of ARG among VALUES (an exact match, or the one value it
 * abbreviates -- several values of one group count as one) */
static int tar_argmatch(const char *what, const char *arg, const char *const *values,
                        const int *groups) {
    size_t n = strlen(arg);
    int found = -1, ambig = 0;
    for (int i = 0; values[i]; i++) {
        if (strncmp(values[i], arg, n) != 0) continue;
        if (strlen(values[i]) == n) return i;
        if (found < 0) found = i;
        else if (!groups || groups[i] != groups[found]) ambig = 1;
    }
    if (found < 0 || ambig) tar_argmatch_die(what, arg, ambig, values, groups);
    return found;
}

/* ---- dates ------------------------------------------------------------------
 *
 * --mtime, --newer, --newer-mtime: a date, or (when it starts with / or .)
 * a file whose mtime is the date. The forms read here are the ones that
 * matter in practice: @SECONDS[.FRACTION], and YYYY-MM-DD with an
 * optional [ T]HH:MM[:SS[.FRACTION]] and a zone (UTC, Z, +HH:MM, +HHMM);
 * without a zone the time is local. Anything else: GNU's warning,
 * "Substituting -9223372036854775807 for unknown date format 'X'", and
 * that time. With -v, a date whose reading is not written as it was given
 * is told: "Option --mtime: Treating date 'X' as 2001-09-09 00:00:00". */

static int tar_digits(const char **pp, int n, long *out) {
    long v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)(*pp)[i])) return 0;
        v = v * 10 + ((*pp)[i] - '0');
    }
    *pp += n;
    *out = v;
    return 1;
}

static int tar_parse_frac(const char **pp, long *ns) {
    const char *p = *pp;
    long v = 0; int d = 0;
    if (*p != '.' && *p != ',') { *ns = 0; return 1; }
    p++;
    if (!isdigit((unsigned char)*p)) return 0;
    for (; isdigit((unsigned char)*p); p++) {
        if (d < 9) { v = v * 10 + (*p - '0'); d++; }
    }
    while (d < 9) { v *= 10; d++; }
    *ns = v;
    *pp = p;
    return 1;
}

/* days from 1970-01-01 to Y-M-D (proleptic Gregorian) */
static int64_t tar_days(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int tar_get_date(const char *s, intmax_t *sec, long *ns) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '@') {
        p++;
        int neg = 0;
        if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
        if (!isdigit((unsigned char)*p)) return 0;
        uintmax_t v = 0;
        for (; isdigit((unsigned char)*p); p++) {
            if (v > (uintmax_t)INTMAX_MAX / 10) return 0;
            v = v * 10 + (uintmax_t)(*p - '0');
            if (v > (uintmax_t)INTMAX_MAX) return 0;
        }
        long f = 0;
        if (!tar_parse_frac(&p, &f)) return 0;
        while (isspace((unsigned char)*p)) p++;
        if (*p) return 0;
        /* "-1.5" is -1 seconds and .5, as GNU reads it */
        *sec = neg ? -(intmax_t)v : (intmax_t)v;
        *ns = f;
        return 1;
    }
    long y, mo, d, h = 0, mi = 0, se = 0, f = 0;
    if (!tar_digits(&p, 4, &y) || *p++ != '-' || !tar_digits(&p, 2, &mo) ||
        *p++ != '-' || !tar_digits(&p, 2, &d))
        return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    int have_time = 0;
    if (*p == ' ' || *p == 'T') {
        const char *q = p + 1;
        while (*q == ' ') q++;
        if (isdigit((unsigned char)*q)) {
            p = q;
            if (!tar_digits(&p, 2, &h) || *p++ != ':' || !tar_digits(&p, 2, &mi)) return 0;
            if (*p == ':') {
                p++;
                if (!tar_digits(&p, 2, &se)) return 0;
                if (!tar_parse_frac(&p, &f)) return 0;
            }
            have_time = 1;
        }
    }
    (void)have_time;
    if (h > 24 || mi > 59 || se > 60) return 0;
    while (*p == ' ') p++;
    int zone = 0; long off = 0;
    if (!strcmp(p, "UTC") || !strcmp(p, "Z") || !strcmp(p, "GMT") || !strcmp(p, "UT")) {
        zone = 1; p += strlen(p);
    } else if (*p == '+' || *p == '-') {
        int sg = *p == '-' ? -1 : 1;
        p++;
        long oh, om = 0;
        if (!tar_digits(&p, 2, &oh)) return 0;
        if (*p == ':') p++;
        if (isdigit((unsigned char)*p) && !tar_digits(&p, 2, &om)) return 0;
        zone = 1;
        off = sg * (oh * 3600 + om * 60);
    }
    while (*p == ' ') p++;
    if (*p) return 0;
    if (zone) {
        *sec = (intmax_t)(tar_days(y, (int)mo, (int)d) * 86400 + h * 3600 + mi * 60 + se - off);
    } else {
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        tm.tm_year = (int)y - 1900; tm.tm_mon = (int)mo - 1; tm.tm_mday = (int)d;
        tm.tm_hour = (int)h; tm.tm_min = (int)mi; tm.tm_sec = (int)se;
        tm.tm_isdst = -1;
        time_t t = mktime(&tm);
        *sec = (intmax_t)t;
    }
    *ns = f;
    return 1;
}

static struct tm *tar_localtime(intmax_t t, struct tm *out) {
    time_t tt = (time_t)t;
    if (tar_o.utc) return gmtime_r(&tt, out);
    return localtime_r(&tt, out);
}

/* "2001-09-09 01:46:40[.5]" -- local, as GNU tells the date it read */
static void tar_fmt_date(char *buf, size_t cap, intmax_t sec, long ns) {
    struct tm tm;
    time_t tt = (time_t)sec;
    if (!localtime_r(&tt, &tm)) { snprintf(buf, cap, "%jd", sec); return; }
    size_t n = strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm);
    if (ns) {
        char f[16];
        snprintf(f, sizeof f, ".%09ld", ns);
        size_t k = strlen(f);
        while (k > 1 && f[k - 1] == '0') f[--k] = 0;
        snprintf(buf + n, cap - n, "%s", f);
    }
}

static char **tar_date_msgs; static int tar_ndate_msgs;
static void tar_parse_date_opt(const char *opt, const char *arg, intmax_t *sec, long *ns) {
    if (arg[0] == '/' || arg[0] == '.') {
        struct stat st;
        if (stat(arg, &st) != 0) {
            int e = errno;
            tar_warn("%s: Cannot stat: %s", tar_qc(arg), strerror(e));
            tar_usage_error("Date sample file not found");
        }
        *sec = (intmax_t)st.st_mtime;
        *ns = st.st_mtim.tv_nsec;
        return;
    }
    if (!tar_get_date(arg, sec, ns)) {
        tar_warn("Substituting %jd for unknown date format %s", (intmax_t)(-INTMAX_MAX), su_quote(arg));
        *sec = -INTMAX_MAX;
        *ns = 0;
        return;
    }
    /* told once the options are all read, if -v is among them */
    char b[64];
    tar_fmt_date(b, sizeof b, *sec, *ns);
    if (strcmp(b, arg) != 0) {
        struct tar_sb m = {0};
        tar_sb_printf(&m, "Option %s: Treating date %s as %s", opt, su_quote(arg), b);
        tar_date_msgs = (char **)tar_realloc(tar_date_msgs, (size_t)(tar_ndate_msgs + 1) * sizeof(char *));
        tar_date_msgs[tar_ndate_msgs++] = tar_sb_take(&m);
    }
}

/* ---- users and groups -------------------------------------------------------
 *
 * Names come from /etc/passwd and /etc/group (getpwuid and friends on
 * Linux; read by hand on tobyOS, where libtoby has none). A lookup that
 * fails is an empty name in the header, as GNU writes it. */

#ifndef SU_HOST
/* field 0 (name) and field 2 (id) of a colon-separated database line */
static int tar_db_find(const char *path, const char *name, uintmax_t id, int by_name,
                       char *name_out, size_t cap, uintmax_t *id_out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':');
        if (!c1) continue;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        *c1 = 0;
        char *e;
        uintmax_t v = tar_strtoumax(c2 + 1, &e, 10);
        if (e == c2 + 1) continue;
        if (by_name ? strcmp(line, name) == 0 : v == id) {
            if (name_out) snprintf(name_out, cap, "%s", line);
            if (id_out) *id_out = v;
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}
#endif

static int tar_uid_name(uintmax_t uid, char *out, size_t cap) {
#ifdef SU_HOST
    struct passwd *pw = getpwuid((uid_t)uid);
    if (!pw) return 0;
    snprintf(out, cap, "%s", pw->pw_name);
    return 1;
#else
    return tar_db_find("/etc/passwd", 0, uid, 0, out, cap, 0);
#endif
}
static int tar_gid_name(uintmax_t gid, char *out, size_t cap) {
#ifdef SU_HOST
    struct group *gr = getgrgid((gid_t)gid);
    if (!gr) return 0;
    snprintf(out, cap, "%s", gr->gr_name);
    return 1;
#else
    return tar_db_find("/etc/group", 0, gid, 0, out, cap, 0);
#endif
}
static int tar_user_uid(const char *name, uintmax_t *uid) {
#ifdef SU_HOST
    struct passwd *pw = getpwnam(name);
    if (!pw) return 0;
    *uid = pw->pw_uid;
    return 1;
#else
    return tar_db_find("/etc/passwd", name, 0, 1, 0, 0, uid);
#endif
}
static int tar_group_gid(const char *name, uintmax_t *gid) {
#ifdef SU_HOST
    struct group *gr = getgrnam(name);
    if (!gr) return 0;
    *gid = gr->gr_gid;
    return 1;
#else
    return tar_db_find("/etc/group", name, 0, 1, 0, 0, gid);
#endif
}

/* cached: a listing or an extraction asks the same few ids again and again */
struct tar_idcache { uintmax_t id; char name[64]; int ok; };
static struct tar_idcache tar_ucache[16], tar_gcache[16];
static int tar_ucn, tar_gcn;
static const char *tar_cached_name(uintmax_t id, int group) {
    struct tar_idcache *c = group ? tar_gcache : tar_ucache;
    int *n = group ? &tar_gcn : &tar_ucn;
    for (int i = 0; i < *n && i < 16; i++)
        if (c[i].id == id) return c[i].ok ? c[i].name : "";
    struct tar_idcache *e = &c[*n % 16];
    (*n)++;
    e->id = id;
    e->ok = group ? tar_gid_name(id, e->name, sizeof e->name)
                  : tar_uid_name(id, e->name, sizeof e->name);
    if (!e->ok) e->name[0] = 0;
    return e->name;
}

/* --owner=NAME[:UID] / --group: a bare number is the id (its name, if
 * any, is looked up when the header is written); a name is looked up for
 * its id and, when it has none, keeps the file's id; NAME:ID gives both. */
static void tar_parse_owner(const char *arg, int group, const char **name_out,
                            uintmax_t *id_out, int *idset) {
    const char *colon = strchr(arg, ':');
    *idset = 0;
    if (colon) {
        const char *ids = colon + 1;
        char *e;
        uintmax_t v;
        if (!*ids || !isdigit((unsigned char)*ids) ||
            (errno = 0, v = tar_strtoumax(ids, &e, 10), *e) || errno) {
            tar_fatal("%s: Invalid owner or group ID", tar_qc(ids));
        }
        *id_out = v;
        *idset = 1;
        *name_out = colon == arg ? 0 : tar_strndup(arg, (size_t)(colon - arg));
        if (colon == arg) *name_out = 0;
        return;
    }
    int alldig = *arg != 0;
    for (const char *p = arg; *p; p++) if (!isdigit((unsigned char)*p)) alldig = 0;
    if (alldig) {
        *id_out = tar_strtoumax(arg, 0, 10);
        *idset = 1;
        *name_out = 0;
        return;
    }
    uintmax_t v;
    if (group ? tar_group_gid(arg, &v) : tar_user_uid(arg, &v)) {
        *id_out = v;
        *idset = 1;
    }
    *name_out = arg;
}

/* ---- the name list ------------------------------------------------------------
 *
 * Operands, -C directories, --add-file names and -T files, in the order
 * given. A -T file is read where it stands in the list: its lines are
 * names (newline-ended, or NUL-ended after --null; empty ones skipped;
 * nothing trimmed), unquoted unless --no-unquote, and -- unless
 * --verbatim-files-from -- a line that starts with '-' is an option, of
 * the few that make sense there (-C, -X, --exclude*, --add-file, -T,
 * --null, --recursion, the matching options); any other is "FILE:LINE:
 * unrecognized option". A -T file is opened where tar started, whatever
 * -C came before it. */

enum { TNE_NAME, TNE_CHDIR, TNE_TFILE };
struct tar_nent {
    int kind;
    char *s;
    char *data; size_t len; /* a -T file's contents */
    size_t pos; int line;
    int null_sep, verbatim, unquote;
    int wildcards, anchored, icase, wslash, recursion;
    int nul_seen, after_nul;      /* a NUL in a newline-ended line */
};
static struct tar_nent *tar_nents; static int tar_nnents, tar_nents_cap;
static int tar_have_names;        /* any name source at all */
static int tar_tfile_stdin;       /* -T - was given */

static struct tar_nent *tar_nent_add(int kind, const char *s) {
    tar_grow((void **)&tar_nents, &tar_nents_cap, tar_nnents, sizeof *tar_nents);
    struct tar_nent *e = &tar_nents[tar_nnents++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->s = s ? tar_strdup(s) : 0;
    e->wildcards = tar_o.wildcards; e->anchored = tar_o.anchored;
    e->icase = tar_o.icase; e->wslash = tar_o.wslash;
    e->recursion = tar_o.recursion;
    e->null_sep = tar_o.null_names; e->verbatim = tar_o.verbatim;
    e->unquote = tar_o.unquote;
    return e;
}

/* the whole of FD */
static char *tar_slurp_fd(int fd, size_t *len) {
    size_t cap = 4096, n = 0;
    char *b = (char *)tar_alloc(cap);
    for (;;) {
        if (n + 4096 > cap) { cap *= 2; b = (char *)tar_realloc(b, cap); }
        ssize_t r = read(fd, b + n, cap - n - 1);
        if (r < 0) { if (errno == EINTR) continue; free(b); return 0; }
        if (r == 0) break;
        n += (size_t)r;
    }
    b[n] = 0;
    *len = n;
    return b;
}

/* backslash escapes in a -T name or a member name: \\ \n \t \a \b \f
 * \r \v \OOO; an unknown escape keeps its letter */
static void tar_unquote(char *s) {
    char *o = s;
    for (char *p = s; *p; ) {
        if (*p != '\\' || !p[1]) { *o++ = *p++; continue; }
        p++;
        switch (*p) {
        case '\\': *o++ = '\\'; p++; break;
        case 'n': *o++ = '\n'; p++; break;
        case 't': *o++ = '\t'; p++; break;
        case 'a': *o++ = '\a'; p++; break;
        case 'b': *o++ = '\b'; p++; break;
        case 'f': *o++ = '\f'; p++; break;
        case 'r': *o++ = '\r'; p++; break;
        case 'v': *o++ = '\v'; p++; break;
        case '?': *o++ = 0x7f; p++; break;
        default:
            if (*p >= '0' && *p <= '7') {
                int v = 0, k = 0;
                while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; k++; }
                *o++ = (char)v;
            } else {
                *o++ = '\\';
                *o++ = *p++;
            }
        }
    }
    *o = 0;
}

static void tar_add_excl(const char *pat);
static void tar_read_exclude_file(const char *file);
static int tar_tfile_option(const char *line, struct tar_nent *src);

/* --- the iterator: the next name, a -C, or the end ---
 * Returns TNE_NAME or TNE_CHDIR with *S set, or -1 at the end. *FLAGS
 * gets the matching flags of the entry. */
struct tar_nflags { int wildcards, anchored, icase, wslash, recursion; };
static int tar_ni;                /* the entry being read */
static int tar_next_name(char **s, struct tar_nflags *fl) {
    while (tar_ni < tar_nnents) {
        struct tar_nent *e = &tar_nents[tar_ni];
        if (e->kind != TNE_TFILE) {
            tar_ni++;
            *s = e->s;
            if (fl) {
                fl->wildcards = e->wildcards; fl->anchored = e->anchored;
                fl->icase = e->icase; fl->wslash = e->wslash;
                fl->recursion = e->recursion;
            }
            return e->kind;
        }
        /* the next line of a -T file; a NUL ends a name there too, and
         * the line that had one is told of once it is done */
        if (e->nul_seen && (e->pos >= e->len || !e->after_nul)) {
            if (tar_warning_on(TW_FILENAME_NULS))
                tar_warn("%s: file name read contains nul character", tar_qc(e->s));
            e->nul_seen = 0;
        }
        if (e->pos >= e->len) { tar_ni++; continue; }
        char sep = e->null_sep ? 0 : '\n';
        size_t start = e->pos, end = start;
        while (end < e->len && e->data[end] != sep && e->data[end] != 0) end++;
        e->after_nul = end < e->len && e->data[end] == 0 && sep;
        if (e->after_nul) e->nul_seen = 1;
        e->pos = end < e->len ? end + 1 : end;
        e->line++;
        if (end == start) continue;
        char *line = tar_strndup(e->data + start, end - start);
        if (!e->verbatim && line[0] == '-') {
            /* the option may change what follows: read it with the
             * entry's own settings, which it then updates */
            int saved_null = tar_o.null_names, saved_verb = tar_o.verbatim;
            tar_o.wildcards = e->wildcards; tar_o.anchored = e->anchored;
            tar_o.icase = e->icase; tar_o.wslash = e->wslash;
            tar_o.recursion = e->recursion;
            tar_o.null_names = e->null_sep; tar_o.verbatim = e->verbatim;
            int nn = tar_nnents;
            int r = tar_tfile_option(line, e);
            e = &tar_nents[tar_ni];
            if (r < 0)
                tar_err("%s:%d: unrecognized option", tar_qc(e->s), e->line);
            e->wildcards = tar_o.wildcards; e->anchored = tar_o.anchored;
            e->icase = tar_o.icase; e->wslash = tar_o.wslash;
            e->recursion = tar_o.recursion;
            e->null_sep = tar_o.null_names; e->verbatim = tar_o.verbatim;
            tar_o.null_names = saved_null; tar_o.verbatim = saved_verb;
            free(line);
            if (tar_nnents > nn) {
                /* it added entries (a -C, an --add-file, a -T) at the
                 * end: they belong here, before the rest of this file */
                int k = tar_nnents - nn;
                struct tar_nent *tmp = (struct tar_nent *)tar_alloc((size_t)k * sizeof *tmp);
                memcpy(tmp, &tar_nents[nn], (size_t)k * sizeof *tmp);
                memmove(&tar_nents[tar_ni + k], &tar_nents[tar_ni],
                        (size_t)(nn - tar_ni) * sizeof *tar_nents);
                memcpy(&tar_nents[tar_ni], tmp, (size_t)k * sizeof *tmp);
                free(tmp);
            }
            continue;
        }
        if (e->unquote) tar_unquote(line);
        *s = line;
        if (fl) {
            fl->wildcards = e->wildcards; fl->anchored = e->anchored;
            fl->icase = e->icase; fl->wslash = e->wslash;
            fl->recursion = e->recursion;
        }
        return TNE_NAME;
    }
    return -1;
}

/* every name at once, for the commands that match members against them */
static void tar_collect_names(void) {
    char *s;
    struct tar_nflags fl;
    int k;
    int dir = -1;
    while ((k = tar_next_name(&s, &fl)) >= 0) {
        if (k == TNE_CHDIR) {
            tar_grow((void **)&tar_dirs, &tar_dirs_cap, tar_ndirs, sizeof *tar_dirs);
            tar_dirs[tar_ndirs] = s;
            dir = tar_ndirs++;
            continue;
        }
        tar_grow((void **)&tar_names, &tar_names_cap, tar_nnames, sizeof *tar_names);
        struct tar_name *n = &tar_names[tar_nnames++];
        memset(n, 0, sizeof *n);
        n->name = s;
        n->dir = dir;
        n->wildcards = fl.wildcards; n->anchored = fl.anchored;
        n->icase = fl.icase; n->wslash = fl.wslash; n->recursion = fl.recursion;
    }
}

/* ---- parsing ------------------------------------------------------------------ */

static const char *tar_cmd_letters[] = {
    "", "-c", "-t", "-x", "-d", "-r", "-u", "-A", "--delete", "--test-label"
};

static void tar_set_cmd(int c) {
    if (tar_o.cmd && tar_o.cmd != c)
        tar_usage_error("You may not specify more than one '-Acdtrux', '--delete' or  '--test-label' option");
    tar_o.cmd = c;
}

/* the options for files that are there exclude each other: "'--B'
 * cannot be used with '--A'", the second named first */
static const char *tar_old_opt;
static void tar_set_old(int kind, const char *opt) {
    if (tar_old_opt && strcmp(tar_old_opt, opt) != 0)
        tar_usage_error("'%s' cannot be used with '%s'", opt, tar_old_opt);
    tar_old_opt = opt;
    tar_o.old_files = kind;
}

static void tar_set_compress(const char *prog) {
    tar_o.compress = prog;
}

static int tar_parse_pax_option(const char *arg);
static void tar_parse_transform(const char *arg);
static void tar_add_checkpoint_action(const char *arg);
static void tar_print_help(void);
static void tar_print_usage(void);

static int tar_backup_values_group[] = { 0, 0, 1, 1, 2, 2, 3, 3 };
static const char *tar_backup_values[] = { "none", "off", "simple", "never", "existing", "nil", "numbered", "t", 0 };

static int tar_backup_kind(const char *opt, const char *arg) {
    static const int kinds[] = { 0, 0, 1, 1, 2, 2, 3, 3 };
    int i = tar_argmatch(opt, arg, tar_backup_values, tar_backup_values_group);
    return kinds[i];
}

/* handle one option; FROM_T: read from a -T file (0 handled, -1 not allowed) */
static int tar_option(int c, char *arg, int from_t, const char *optname);

static int tar_tfile_option(const char *line, struct tar_nent *src) {
    (void)src;
    /* --name[=value], or -L[ ]value */
    if (line[1] == '-') {
        const char *name = line + 2;
        size_t len = strcspn(name, "=");
        int found = -1, ambig = 0, exact = 0;
        for (int i = 0; tar_longopts[i].name; i++) {
            if (strncmp(tar_longopts[i].name, name, len) != 0) continue;
            if (strlen(tar_longopts[i].name) == len) { found = i; exact = 1; break; }
            if (found < 0) found = i;
            else if (tar_longopts[i].val != tar_longopts[found].val) ambig = 1;
        }
        if (found < 0 || (ambig && !exact)) return -1;
        const struct su_option *o = &tar_longopts[found];
        char *arg = 0;
        if (name[len] == '=') {
            if (o->has_arg == SU_NO_ARG) return -1;
            arg = (char *)name + len + 1;
        } else if (o->has_arg == SU_REQ_ARG) return -1;
        return tar_option(o->val, arg, 1, o->name);
    }
    int c = (unsigned char)line[1];
    if (!c) return -1;
    const char *spec = strchr(tar_shortopts + 1, c);
    if (!spec || c == ':') return -1;
    char *arg = 0;
    if (spec[1] == ':') {
        const char *a = line + 2;
        while (*a == ' ') a++;
        if (!*a) return -1;
        arg = (char *)a;
    } else if (line[2]) return -1;
    return tar_option(c, arg, 1, 0);
}

static int tar_option(int c, char *arg, int from_t, const char *optname) {
    (void)optname;
    if (from_t) {
        /* only what shapes the name list */
        switch (c) {
        case 'C': case 'T': case 'X': case TO_ADD_FILE: case TO_EXCLUDE:
        case TO_EXCLUDE_CACHES: case TO_EXCLUDE_CACHES_UNDER: case TO_EXCLUDE_CACHES_ALL:
        case TO_EXCLUDE_TAG: case TO_EXCLUDE_TAG_UNDER: case TO_EXCLUDE_TAG_ALL:
        case TO_EXCLUDE_IGNORE: case TO_EXCLUDE_IGNORE_REC: case TO_EXCLUDE_VCS:
        case TO_EXCLUDE_VCS_IGNORES: case TO_EXCLUDE_BACKUPS:
        case TO_NULL: case TO_NO_NULL: case TO_RECURSION: case TO_NO_RECURSION:
        case TO_UNQUOTE: case TO_NO_UNQUOTE: case TO_VERBATIM: case TO_NO_VERBATIM:
        case TO_ANCHORED: case TO_NO_ANCHORED: case TO_IGNORE_CASE: case TO_NO_IGNORE_CASE:
        case TO_WILDCARDS: case TO_NO_WILDCARDS: case TO_WILDCARDS_SLASH:
        case TO_NO_WILDCARDS_SLASH:
            break;
        default:
            return -1;
        }
    }
    switch (c) {
    case 'A': tar_set_cmd(TAR_C_CAT); break;
    case 'c': tar_set_cmd(TAR_C_CREATE); break;
    case 'd': tar_set_cmd(TAR_C_DIFF); break;
    case 'r': tar_set_cmd(TAR_C_APPEND); break;
    case 't': tar_set_cmd(TAR_C_LIST); break;
    case 'u': tar_set_cmd(TAR_C_UPDATE); break;
    case 'x': tar_set_cmd(TAR_C_EXTRACT); break;
    case TO_DELETE: tar_set_cmd(TAR_C_DELETE); break;
    case TO_TEST_LABEL: tar_set_cmd(TAR_C_TESTLABEL); break;
    case 'G': tar_o.incremental = 1; break;
    case 'g': tar_o.listed_incr = arg; tar_o.incremental = 1; break;
    case 'n': tar_o.seek = 1; tar_o.seek_set = 1; break;
    case TO_NO_SEEK: tar_o.seek = 0; tar_o.seek_set = 1; break;
    case 'S': tar_o.sparse = 1; break;
    case TO_SPARSE_VERSION: {
        char *e;
        long ma = strtol(arg, &e, 10), mi = 0;
        if (e == arg) tar_usage_error("Invalid sparse version value");
        if (*e == '.') {
            char *f = e + 1;
            mi = strtol(f, &e, 10);
            if (e == f) tar_usage_error("Invalid sparse version value");
        }
        if (*e) tar_usage_error("Invalid sparse version value");
        tar_o.sparse_major = (int)ma; tar_o.sparse_minor = (int)mi;
        tar_o.sparse_ver_set = 1;
        tar_o.sparse = 1;
        break;
    }
    case TO_HOLE_DETECTION: {
        static const char *v[] = { "raw", "seek", 0 };
        tar_o.hole_seek = tar_argmatch("--hole-detection", arg, v, 0) == 1 ? 2 : 1;
        break;
    }
    case TO_IGNORE_FAILED_READ: tar_o.ignore_failed_read = 1; break;
    case TO_LEVEL: {
        char *e;
        long v = strtol(arg, &e, 10);
        if (e == arg || *e || v < 0) tar_usage_error("Invalid incremental level value");
        tar_o.level = (int)v; tar_o.level_set = 1;
        break;
    }
    case TO_CHECK_DEVICE: tar_o.check_device = 1; break;
    case TO_NO_CHECK_DEVICE: tar_o.check_device = 0; break;
    case TO_OCCURRENCE:
        if (arg) {
            char *e;
            errno = 0;
            uintmax_t v = tar_strtoumax(arg, &e, 10);
            if (e == arg || *e || errno || !isdigit((unsigned char)*arg) || v > INT_MAX)
                tar_fatal("%s: Invalid number", tar_qc(arg));
            tar_o.occurrence = (int)v;
        } else tar_o.occurrence = 1;
        break;
    case TO_ADD_FILE:
        tar_nent_add(TNE_NAME, arg);
        tar_have_names = 1;
        break;
    case 'C':
        tar_nent_add(TNE_CHDIR, arg);
        break;
    case TO_EXCLUDE: tar_add_excl(arg); break;
    case 'X': tar_read_exclude_file(arg); break;
    case TO_EXCLUDE_BACKUPS: tar_o.exclude_backups = 1; break;
    case TO_EXCLUDE_CACHES: tar_o.exclude_caches = 1; break;
    case TO_EXCLUDE_CACHES_UNDER: tar_o.exclude_caches = 2; break;
    case TO_EXCLUDE_CACHES_ALL: tar_o.exclude_caches = 3; break;
    case TO_EXCLUDE_TAG: case TO_EXCLUDE_TAG_UNDER: case TO_EXCLUDE_TAG_ALL:
        tar_o.excl_tag =(char **)tar_realloc(tar_o.excl_tag, (size_t)(tar_o.nexcl_tag + 1) * sizeof(char *));
        tar_o.excl_tag_kind = (int *)tar_realloc(tar_o.excl_tag_kind, (size_t)(tar_o.nexcl_tag + 1) * sizeof(int));
        tar_o.excl_tag[tar_o.nexcl_tag] = arg;
        tar_o.excl_tag_kind[tar_o.nexcl_tag++] = c == TO_EXCLUDE_TAG ? 1 : c == TO_EXCLUDE_TAG_UNDER ? 2 : 3;
        break;
    case TO_EXCLUDE_IGNORE: case TO_EXCLUDE_IGNORE_REC:
        tar_o.excl_ignore = (char **)tar_realloc(tar_o.excl_ignore, (size_t)(tar_o.nexcl_ignore + 1) * sizeof(char *));
        tar_o.excl_ignore_rec = (int *)tar_realloc(tar_o.excl_ignore_rec, (size_t)(tar_o.nexcl_ignore + 1) * sizeof(int));
        tar_o.excl_ignore[tar_o.nexcl_ignore] = arg;
        tar_o.excl_ignore_rec[tar_o.nexcl_ignore++] = c == TO_EXCLUDE_IGNORE_REC;
        break;
    case TO_EXCLUDE_VCS: tar_o.exclude_vcs = 1; break;
    case TO_EXCLUDE_VCS_IGNORES: tar_o.exclude_vcs_ignores = 1; break;
    case TO_NO_NULL: tar_o.null_names = 0; break;
    case TO_NULL: tar_o.null_names = 1; tar_o.verbatim = 1; break;
    case TO_NO_RECURSION: tar_o.recursion = 0; break;
    case TO_RECURSION: tar_o.recursion = 1; break;
    case TO_NO_UNQUOTE: tar_o.unquote = 0; break;
    case TO_UNQUOTE: tar_o.unquote = 1; break;
    case TO_VERBATIM: tar_o.verbatim = 1; break;
    case TO_NO_VERBATIM: tar_o.verbatim = 0; break;
    case 'T': {
        int fd;
        if (!strcmp(arg, "-")) { fd = 0; tar_tfile_stdin = 1; }
        else {
            struct stat st;
            if (stat(arg, &st) != 0) {
                int e = errno;
                tar_fatal("%s: Cannot stat: %s", tar_qc(arg), strerror(e));
            }
            fd = su_open_read(arg, 0);
            if (fd < 0) {
                int e = errno;
                tar_fatal("%s: Cannot open: %s", tar_qc(arg), strerror(e));
            }
        }
        struct tar_nent *e = tar_nent_add(TNE_TFILE, arg);
        e->data = tar_slurp_fd(fd, &e->len);
        if (!e->data) {
            int er = errno;
            tar_fatal("%s: Read error: %s", tar_qc(arg), strerror(er));
        }
        if (fd) close(fd);
        tar_have_names = 1;
        break;
    }
    case TO_ANCHORED: tar_o.anchored = 1; tar_o.anchored_set = 1; break;
    case TO_NO_ANCHORED: tar_o.anchored = 0; tar_o.anchored_set = 1; break;
    case TO_IGNORE_CASE: tar_o.icase = 1; break;
    case TO_NO_IGNORE_CASE: tar_o.icase = 0; break;
    case TO_WILDCARDS: tar_o.wildcards = 1; tar_o.wildcards_set = 1; break;
    case TO_NO_WILDCARDS: tar_o.wildcards = 0; tar_o.wildcards_set = 1; break;
    case TO_WILDCARDS_SLASH: tar_o.wslash = 1; break;
    case TO_NO_WILDCARDS_SLASH: tar_o.wslash = 0; break;
    case TO_KEEP_DIR_SYMLINK: tar_o.keep_dir_symlink = 1; break;
    case TO_KEEP_NEWER: tar_set_old(TAR_OLD_KEEP_NEWER, "--keep-newer-files"); break;
    case 'k': tar_set_old(TAR_OLD_KEEP_OLD, "--keep-old-files"); break;
    case TO_NO_OVERWRITE_DIR: tar_set_old(TAR_OLD_NO_OVERWRITE_DIR, "--no-overwrite-dir"); break;
    case TO_ONE_TOP_LEVEL:
        tar_o.one_top_set = 1;
        tar_o.one_top_level = arg;
        break;
    case TO_OVERWRITE: tar_set_old(TAR_OLD_OVERWRITE, "--overwrite"); break;
    case TO_OVERWRITE_DIR: tar_set_old(TAR_OLD_DEFAULT, "--overwrite-dir"); break;
    case TO_RECURSIVE_UNLINK: tar_o.recursive_unlink = 1; break;
    case TO_REMOVE_FILES: tar_o.remove_files = 1; break;
    case TO_SKIP_OLD: tar_set_old(TAR_OLD_SKIP_OLD, "--skip-old-files"); break;
    case 'U': tar_set_old(TAR_OLD_UNLINK_FIRST, "--unlink-first"); break;
    case 'W': tar_o.verify = 1; break;
    case TO_IGNORE_CMD_ERROR: tar_o.ignore_cmd_error = 1; break;
    case TO_NO_IGNORE_CMD_ERROR: tar_o.ignore_cmd_error = 0; break;
    case 'O': tar_o.to_stdout = 1; break;
    case TO_TO_COMMAND: tar_o.to_command = arg; break;
    case TO_ATIME_PRESERVE:
        if (arg) {
            static const char *v[] = { "replace", "system", 0 };
            tar_o.atime_preserve = 1 + tar_argmatch("--atime-preserve", arg, v, 0);
        } else tar_o.atime_preserve = 1;
        break;
    case TO_CLAMP_MTIME: tar_o.clamp_mtime = 1; break;
    case TO_DELAY_DIR: tar_o.delay_dir_restore = 1; break;
    case TO_NO_DELAY_DIR: tar_o.delay_dir_restore = 0; break;
    case TO_GROUP: {
        int idset;
        const char *nm;
        uintmax_t id = 0;
        tar_parse_owner(arg, 1, &nm, &id, &idset);
        tar_o.group_set = idset ? 2 : 1;
        tar_o.group_name = nm;
        tar_o.group_gid = id;
        break;
    }
    case TO_OWNER: {
        int idset;
        const char *nm;
        uintmax_t id = 0;
        tar_parse_owner(arg, 0, &nm, &id, &idset);
        tar_o.owner_set = idset ? 2 : 1;
        tar_o.owner_name = nm;
        tar_o.owner_uid = id;
        break;
    }
    case TO_GROUP_MAP: tar_o.group_map = arg; break;
    case TO_OWNER_MAP: tar_o.owner_map = arg; break;
    case TO_MODE:
        if (su_mode_compile(arg, &tar_o.mode) != 0)
            tar_fatal("Invalid mode given on option");
        tar_o.mode_set = 1;
        break;
    case TO_MTIME:
        tar_parse_date_opt("--mtime", arg, &tar_o.mtime_s, &tar_o.mtime_ns);
        tar_o.mtime_set = 1;
        break;
    case 'm': tar_o.touch = 1; break;
    case TO_NO_SAME_OWNER: tar_o.same_owner = 0; tar_o.same_owner_set = 1; break;
    case TO_NO_SAME_PERMS: tar_o.same_perms = 0; tar_o.same_perms_set = 1; break;
    case TO_NUMERIC_OWNER: tar_o.numeric_owner = 1; break;
    case 'p': tar_o.same_perms = 1; tar_o.same_perms_set = 1; break;
    case TO_SAME_OWNER: tar_o.same_owner = 1; tar_o.same_owner_set = 1; break;
    case TO_SORT: {
        static const char *v[] = { "none", "name", "inode", 0 };
        tar_o.sort = tar_argmatch("--sort", arg, v, 0);
        break;
    }
    case 's': tar_o.preserve_order = 1; break;
    case TO_ACLS: tar_o.acls = 1; break;
    case TO_NO_ACLS: tar_o.acls = 0; break;
    case TO_SELINUX: tar_o.selinux = 1; break;
    case TO_NO_SELINUX: tar_o.selinux = 0; break;
    case TO_XATTRS: tar_o.xattrs = 1; break;
    case TO_NO_XATTRS: tar_o.xattrs = 0; break;
    case TO_XATTRS_INCLUDE: case TO_XATTRS_EXCLUDE: tar_o.xattrs = 1; break;
    case TO_FORCE_LOCAL: tar_o.force_local = 1; break;
    case 'f':
        tar_o.archives = (char **)tar_realloc(tar_o.archives, (size_t)(tar_o.narchives + 1) * sizeof(char *));
        tar_o.archives[tar_o.narchives++] = arg;
        break;
    case 'F': tar_o.info_script = arg; tar_o.multi_volume = 1; break;
    case 'L': {
        uintmax_t v;
        char *e;
        if (su_strtoumax(arg, &e, &v, "bBcGgkKMmPTtwEZY0") != SU_NUM_OK)
            tar_usage_error("%s: Invalid tape length", tar_qc(arg));
        size_t al = strlen(arg);
        if (al && isdigit((unsigned char)arg[al - 1])) v *= 1024;
        tar_o.tape_length = v;
        tar_o.multi_volume = 1;
        break;
    }
    case 'M': tar_o.multi_volume = 1; break;
    case TO_RMT_COMMAND: tar_o.rmt = arg; break;
    case TO_RSH_COMMAND: tar_o.rsh = arg; break;
    case TO_VOLNO_FILE: tar_o.volno_file = arg; break;
    case 'b': {
        char *e;
        errno = 0;
        uintmax_t v = tar_strtoumax(arg, &e, 10);
        if (e == arg || *e || errno || v == 0 || v > (SIZE_MAX / TAR_BLK) || !isdigit((unsigned char)*arg))
            tar_usage_error("%s: Invalid blocking factor", tar_qc(arg));
        tar_o.blocking = (int)v;
        tar_o.record_size = (size_t)v * TAR_BLK;
        tar_o.record_size_set = 1;
        break;
    }
    case 'B': tar_o.read_full = 1; break;
    case 'i': tar_o.ignore_zeros = 1; break;
    case TO_RECORD_SIZE: {
        uintmax_t v;
        char *e;
        if (su_strtoumax(arg, &e, &v, "bBcGgkKMmPTtwEZY0") != SU_NUM_OK)
            tar_usage_error("%s: Invalid record size", tar_qc(arg));
        if (v % TAR_BLK || v == 0)
            tar_usage_error("Record size must be a multiple of %d.", TAR_BLK);
        tar_o.record_size = (size_t)v;
        tar_o.blocking = (int)(v / TAR_BLK);
        tar_o.record_size_set = 1;
        break;
    }
    case 'H': {
        static const struct { const char *n; int f; } fm[] = {
            { "v7", TAR_F_V7 }, { "oldgnu", TAR_F_OLDGNU }, { "ustar", TAR_F_USTAR },
            { "posix", TAR_F_POSIX }, { "pax", TAR_F_POSIX }, { "gnu", TAR_F_GNU },
            { 0, 0 }
        };
        int f = -1;
        for (int i = 0; fm[i].n; i++) if (!strcmp(arg, fm[i].n)) f = fm[i].f;
        if (f < 0) tar_usage_error("%s: Invalid archive format", tar_qc(arg));
        tar_o.format = f;
        break;
    }
    case TO_OLD_ARCHIVE: tar_o.format = TAR_F_V7; break;
    case TO_POSIX: tar_o.format = TAR_F_POSIX; break;
    case TO_PAX_OPTION:
        tar_parse_pax_option(arg);
        tar_o.pax_set = 1;
        break;
    case 'V': tar_o.label = arg; break;   /* checked once the command is known */
    case 'a': tar_o.auto_compress = 1; break;
    case TO_NO_AUTO_COMPRESS: tar_o.auto_compress = 0; break;
    case 'I': tar_set_compress(arg); break;
    case 'j': tar_set_compress("bzip2"); break;
    case 'J': tar_set_compress("xz"); break;
    case TO_LZIP: tar_set_compress("lzip"); break;
    case TO_LZMA: tar_set_compress("lzma"); break;
    case TO_LZOP: tar_set_compress("lzop"); break;
    case TO_ZSTD: tar_set_compress("zstd"); break;
    case 'z': tar_set_compress("gzip"); break;
    case 'Z': tar_set_compress("compress"); break;
    case TO_BACKUP:
        tar_o.backup = arg ? tar_backup_kind("--backup", arg) : 2;
        if (!arg) {
            const char *vc = getenv("VERSION_CONTROL");
            if (vc && *vc) tar_o.backup = tar_backup_kind("$VERSION_CONTROL", vc);
        }
        break;
    case TO_HARD_DEREF: tar_o.hard_deref = 1; break;
    case 'h': tar_o.dereference = 1; break;
    case 'K': tar_o.starting_file = arg; break;
    case TO_NEWER_MTIME: case 'N': case TO_AFTER_DATE:
        if (tar_o.newer_set) tar_usage_error("More than one threshold date");
        tar_parse_date_opt(c == TO_NEWER_MTIME ? "--newer-mtime" : "--after-date",
                           arg, &tar_o.newer_s, &tar_o.newer_ns);
        tar_o.newer_set = 1;
        tar_o.newer_mtime_only = c == TO_NEWER_MTIME;
        break;
    case TO_ONE_FS: tar_o.one_fs = 1; break;
    case 'P': tar_o.absolute = 1; break;
    case TO_SUFFIX: tar_o.suffix = arg; if (!tar_o.backup) tar_o.backup = 1; break;
    case TO_STRIP: {
        char *e;
        errno = 0;
        long v = strtol(arg, &e, 10);
        if (e == arg || *e || errno || v < 0 || !isdigit((unsigned char)*arg))
            tar_usage_error("%s: Invalid number of elements", tar_qc(arg));
        tar_o.strip = (int)v;
        break;
    }
    case TO_TRANSFORM: tar_parse_transform(arg); break;
    case TO_CHECKPOINT:
        if (arg) {
            char *e;
            errno = 0;
            long v = strtol(arg, &e, 10);
            if (e == arg || *e || errno) tar_fatal("--checkpoint value is not an integer");
            tar_o.checkpoint = (int)v;
        } else tar_o.checkpoint = 10;
        tar_o.checkpoint_set = 1;
        break;
    case TO_CHECKPOINT_ACTION: tar_add_checkpoint_action(arg); break;
    case TO_FULL_TIME: tar_o.full_time = 1; break;
    case TO_INDEX_FILE: tar_o.index_file = arg; break;
    case 'l': tar_o.check_links = 1; break;
    case TO_NO_QUOTE_CHARS:
        for (const char *p = arg; *p; p++) tar_o.quote_extra[(unsigned char)*p] = 0;
        break;
    case TO_QUOTE_CHARS:
        for (const char *p = arg; *p; p++) tar_o.quote_extra[(unsigned char)*p] = 1;
        break;
    case TO_QUOTING_STYLE: {
        int f = -1;
        for (int i = 0; tar_qstyle_names[i]; i++) if (!strcmp(arg, tar_qstyle_names[i])) f = i;
        if (f < 0)
            tar_fatal("Unknown quoting style %s. Try '%s --quoting-style=help' to get a list.",
                      su_quote(arg), su_name);
        tar_o.quoting = f;
        break;
    }
    case 'R': tar_o.block_number = 1; break;
    case TO_SHOW_DEFAULTS:
        printf("--format=gnu -f- -b20 --quoting-style=escape --rmt-command=/usr/sbin/rmt --rsh-command=/usr/bin/rsh\n");
        exit(0);
    case TO_SHOW_OMITTED: tar_o.show_omitted = 1; break;
    case TO_SHOW_SNAPSHOT:
        printf("This tar's snapshot file field ranges are\n"
               "   (field name      => [ min, max ]):\n\n"
               "    nfs             => [ 0, 1 ],\n"
               "    timestamp_sec   => [ %jd, %jd ],\n"
               "    timestamp_nsec  => [ 0, 999999999 ],\n"
               "    dev             => [ 0, %ju ],\n"
               "    ino             => [ 0, %ju ],\n\n",
               (intmax_t)INT64_MIN, (intmax_t)INT64_MAX, (uintmax_t)UINT64_MAX, (uintmax_t)UINT64_MAX);
        exit(0);
    case TO_SHOW_TRANSFORMED: tar_o.show_transformed = 1; break;
    case TO_TOTALS:
        if (arg) {
            static const char *sigs[] = { "SIGHUP", "SIGQUIT", "SIGINT", "SIGUSR1", "SIGUSR2", 0 };
            int ok = 0;
            for (int i = 0; sigs[i]; i++)
                if (!strcmp(arg, sigs[i]) || !strcmp(arg, sigs[i] + 3)) ok = 1;
            if (!ok) tar_fatal("Unknown signal name: %s", arg);
            /* a signal-driven total: nothing at the end */
        } else tar_o.totals = 1;
        break;
    case TO_UTC: tar_o.utc = 1; break;
    case 'v': tar_o.verbose++; break;
    case TO_WARNING: {
        const char *a = arg;
        int neg = 0;
        if (!strncmp(a, "no-", 3)) { neg = 1; a += 3; }
        int w = tar_argmatch("--warning", a, tar_warn_names, 0);
        unsigned long bits = w == TW_ALL ? ~0UL : 1UL << w;
        if (neg) tar_warnings &= ~bits;
        else tar_warnings |= bits;
        if (w != TW_ALL) tar_warn_explicit[w] = 1;
        else for (int i = 0; i < 32; i++) tar_warn_explicit[i] = 1;
        break;
    }
    case 'w': tar_o.interactive = 1; break;
    case 'o':
        /* -o: --old-archive when creating, --no-same-owner otherwise;
         * which, is known only once the command is */
        tar_o.format = tar_o.format == TAR_F_DEFAULT ? -1 : tar_o.format;
        break;
    case TO_RESTRICT: tar_o.restrict_ = 1; break;
    case TO_PROGRAM_NAME: su_name = arg; break;
    case '?':
        tar_print_help();
        exit(0);
    case TO_USAGE:
        tar_print_usage();
        exit(0);
    case TO_VERSION:
        printf("tar (tobyOS) 1.0\n");
        exit(0);
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        fflush(stdout);
        fprintf(stderr, "%s: Options '-[0-7][lmh]' not supported by *this* tar\n", su_name);
        tar_try_help64();
    case '8': case '9':
        fflush(stdout);
        fprintf(stderr, "%s: -%c: (PROGRAM ERROR) Option should have been recognized!?\n", su_name, c);
        tar_try_help64();
    default:
        return -1;
    }
    return 0;
}

/* ---- matching -----------------------------------------------------------------
 *
 * A pattern matches a name the way GNU's matching options say: with
 * wildcards (* ? [...], \ quoting the next character) or literally; anchored
 * at the start of the name, or at the start or after any '/'; with or
 * without case; '*' and '?' crossing '/' or not (--wildcards-match-slash);
 * and always as a leading directory too -- "d" matches "d/e/f". */

struct tar_mflags { int wildcards, anchored, icase, wslash; };

static int tar_lc(int c, int icase) { return icase ? tolower(c) : c; }

/* fnmatch, with LEADING_DIR: the pattern may match a leading part of S
 * ending where S has a '/' */
static int tar_fnm(const char *p, const char *s, int icase, int wslash, int leading) {
    for (;;) {
        char pc = *p;
        if (!pc) return !*s || (leading && *s == '/');
        if (pc == '*') {
            while (*p == '*') p++;
            if (!*p) {
                if (wslash) return 1;
                const char *sl = strchr(s, '/');
                return !sl || leading;
            }
            for (const char *t = s; ; t++) {
                if (tar_fnm(p, t, icase, wslash, leading)) return 1;
                if (!*t) return 0;
                if (!wslash && *t == '/') return 0;
            }
        }
        if (!*s) return 0;
        if (pc == '?') {
            if (!wslash && *s == '/') return 0;
            p++; s++;
            continue;
        }
        if (pc == '[') {
            const char *q = p + 1;
            int neg = 0;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            int matched = 0;
            int c = tar_lc((unsigned char)*s, icase);
            const char *start = q;
            int ok = 0;
            while (*q && (*q != ']' || q == start)) {
                if (*q == '[' && q[1] == ':') {
                    const char *e = strstr(q + 2, ":]");
                    if (e) {
                        char cls[16];
                        size_t l = (size_t)(e - (q + 2));
                        if (l < sizeof cls) {
                            memcpy(cls, q + 2, l); cls[l] = 0;
                            int ch = (unsigned char)*s, r = 0;
                            if (!strcmp(cls, "alpha")) r = isalpha(ch);
                            else if (!strcmp(cls, "digit")) r = isdigit(ch);
                            else if (!strcmp(cls, "alnum")) r = isalnum(ch);
                            else if (!strcmp(cls, "upper")) r = icase ? isalpha(ch) : isupper(ch);
                            else if (!strcmp(cls, "lower")) r = icase ? isalpha(ch) : islower(ch);
                            else if (!strcmp(cls, "space")) r = isspace(ch);
                            else if (!strcmp(cls, "blank")) r = ch == ' ' || ch == '\t';
                            else if (!strcmp(cls, "punct")) r = ispunct(ch);
                            else if (!strcmp(cls, "print")) r = isprint(ch);
                            else if (!strcmp(cls, "graph")) r = isgraph(ch);
                            else if (!strcmp(cls, "cntrl")) r = iscntrl(ch);
                            else if (!strcmp(cls, "xdigit")) r = isxdigit(ch);
                            if (r) matched = 1;
                            q = e + 2;
                            continue;
                        }
                    }
                }
                int lo = (unsigned char)*q;
                if (lo == '\\' && q[1]) { q++; lo = (unsigned char)*q; }
                q++;
                int hi = lo;
                if (*q == '-' && q[1] && q[1] != ']') {
                    q++;
                    hi = (unsigned char)*q;
                    if (hi == '\\' && q[1]) { q++; hi = (unsigned char)*q; }
                    q++;
                }
                if (icase) {
                    if ((c >= tar_lc(lo, 1) && c <= tar_lc(hi, 1)) ||
                        (toupper(c) >= lo && toupper(c) <= hi))
                        matched = 1;
                } else if (c >= lo && c <= hi) matched = 1;
            }
            if (*q == ']') ok = 1;
            if (!ok) {
                /* no closing bracket: a literal '[' */
                if (*s != '[') return 0;
                p++; s++;
                continue;
            }
            if (!wslash && *s == '/') return 0;
            if (matched == neg) return 0;
            p = q + 1; s++;
            continue;
        }
        if (pc == '\\' && p[1]) { p++; pc = *p; }
        if (tar_lc((unsigned char)pc, icase) != tar_lc((unsigned char)*s, icase)) return 0;
        p++; s++;
    }
}

/* the literal comparison, with the leading-directory rule */
static int tar_lit(const char *p, const char *s, int icase) {
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') n--;
    for (size_t i = 0; i < n; i++)
        if (tar_lc((unsigned char)p[i], icase) != tar_lc((unsigned char)s[i], icase)) return 0;
    return s[n] == 0 || s[n] == '/' || (n && p[n - 1] == '/');
}

static int tar_has_wild(const char *p) {
    /* what GNU calls pattern matching characters (not a backslash) */
    return strpbrk(p, "*?[") != 0;
}

static int tar_match(const char *pat, const char *name, const struct tar_mflags *f) {
    const char *s = name;
    for (;;) {
        int r = f->wildcards ? tar_fnm(pat, s, f->icase, f->wslash, 1)
                             : tar_lit(pat, s, f->icase);
        if (r) return 1;
        if (f->anchored) return 0;
        const char *sl = strchr(s, '/');
        if (!sl) return 0;
        s = sl + 1;
    }
}

/* ---- exclusion ------------------------------------------------------------- */

static void tar_add_excl(const char *pat) {
    tar_grow((void **)&tar_excls, &tar_excls_cap, tar_nexcls, sizeof *tar_excls);
    struct tar_excl *e = &tar_excls[tar_nexcls++];
    e->pat = tar_strdup(pat);
    /* exclusion's defaults: unanchored, wildcards, '*' crossing '/' --
     * each changed by the option given before the pattern */
    e->wildcards = tar_o.wildcards_set ? tar_o.wildcards : 1;
    e->anchored = tar_o.anchored_set ? tar_o.anchored : 0;
    e->icase = tar_o.icase;
    e->wslash = tar_o.wslash;
}

static void tar_read_exclude_file(const char *file) {
    int fd = !strcmp(file, "-") ? 0 : su_open_read(file, 0);
    if (fd < 0) {
        int e = errno;
        tar_fatal("%s: %s", tar_qc(file), strerror(e));
    }
    size_t len;
    char *d = tar_slurp_fd(fd, &len);
    if (fd) close(fd);
    if (!d) return;
    /* lines, whatever --null says; a NUL ends the pattern */
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && d[j] != '\n') j++;
        if (j > i) {
            char *p = tar_strndup(d + i, j - i);
            if (*p) tar_add_excl(p);
            free(p);
        }
        i = j + 1;
    }
    free(d);
}

/* the version-control names --exclude-vcs leaves out */
static const char *tar_vcs_names[] = {
    "CVS", ".cvsignore", "RCS", "SCCS", ".git", ".gitignore", ".gitattributes",
    ".gitmodules", ".arch-ids", "{arch}", "=RELEASE-ID", "=meta-update",
    "=update", ".bzr", ".bzrignore", ".bzrtags", ".hg", ".hgignore", ".hgtags",
    "_darcs", 0
};
static const char *tar_backup_pats[] = { ".#*", "*~", "#*#", 0 };

/* NAME (a path as it is stored) is excluded */
static int tar_excluded(const char *name) {
    for (int i = 0; i < tar_nexcls; i++) {
        struct tar_excl *e = &tar_excls[i];
        struct tar_mflags f = { e->wildcards, e->anchored, e->icase, e->wslash };
        if (tar_match(e->pat, name, &f)) return 1;
    }
    if (tar_o.exclude_vcs || tar_o.exclude_backups) {
        const char *b = strrchr(name, '/');
        b = b ? b + 1 : name;
        if (tar_o.exclude_vcs)
            for (int i = 0; tar_vcs_names[i]; i++) {
                /* the component anywhere in the path */
                const char *s = name;
                size_t n = strlen(tar_vcs_names[i]);
                for (;;) {
                    if (!strncmp(s, tar_vcs_names[i], n) && (s[n] == 0 || s[n] == '/')) return 1;
                    const char *sl = strchr(s, '/');
                    if (!sl) break;
                    s = sl + 1;
                }
            }
        if (tar_o.exclude_backups)
            for (int i = 0; tar_backup_pats[i]; i++) {
                struct tar_mflags f = { 1, 0, 0, 1 };
                if (tar_match(tar_backup_pats[i], name, &f)) return 1;
            }
    }
    return 0;
}

/* ---- --transform ---------------------------------------------------------------
 *
 * sed's s command, one or more separated by ';': s/REGEX/REPLACEMENT/FLAGS,
 * any delimiter, the regex basic (extended with the x flag), the flags g,
 * a number (that match only), i, x, and r s h (apply to member names,
 * symbolic link targets, hard link targets -- all three by default) or R S
 * H (not). The replacement's & and \1-\9, \L \U \l \u \E for case, and the
 * usual backslash escapes. */

static void tar_parse_transform(const char *arg) {
    const char *p = arg;
    while (*p) {
        if (*p != 's') tar_usage_error("Invalid transform expression");
        p++;
        char delim = *p;
        if (!delim) tar_usage_error("Invalid transform expression");
        p++;
        struct tar_sb re = {0}, rp = {0};
        while (*p && *p != delim) {
            if (*p == '\\' && p[1]) {
                if (p[1] == delim) { tar_sb_addc(&re, delim); p += 2; continue; }
                tar_sb_addc(&re, *p++);
            }
            tar_sb_addc(&re, *p++);
        }
        if (*p != delim) tar_usage_error("Invalid transform expression");
        p++;
        while (*p && *p != delim) {
            if (*p == '\\' && p[1]) {
                if (p[1] == delim) { tar_sb_addc(&rp, delim); p += 2; continue; }
                tar_sb_addc(&rp, *p++);
            }
            tar_sb_addc(&rp, *p++);
        }
        if (*p != delim) tar_usage_error("Invalid transform expression");
        p++;
        tar_grow((void **)&tar_xfs, &tar_xfs_cap, tar_nxfs, sizeof *tar_xfs);
        struct tar_xf *x = &tar_xfs[tar_nxfs++];
        memset(x, 0, sizeof *x);
        x->flags = TAR_XF_REG | TAR_XF_SYM | TAR_XF_HARD;
        x->repl = tar_sb_take(&rp);
        int cflags = 0;
        while (*p && *p != ';') {
            switch (*p) {
            case 'g': x->global = 1; break;
            case 'i': cflags |= REG_ICASE; break;
            case 'x': cflags |= REG_EXTENDED; break;
            case 'r': x->flags |= TAR_XF_REG; break;
            case 'R': x->flags &= ~TAR_XF_REG; break;
            case 's': x->flags |= TAR_XF_SYM; break;
            case 'S': x->flags &= ~TAR_XF_SYM; break;
            case 'h': x->flags |= TAR_XF_HARD; break;
            case 'H': x->flags &= ~TAR_XF_HARD; break;
            default:
                if (isdigit((unsigned char)*p)) {
                    int v = 0;
                    while (isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
                    x->nth = v;
                    continue;
                }
                tar_usage_error("Unknown flag in transform expression: %c", *p);
            }
            p++;
        }
        if (*p == ';') p++;
        char *res = tar_sb_take(&re);
        int rc = regcomp(&x->re, res, cflags);
        if (rc) {
            char eb[256];
            regerror(rc, &x->re, eb, sizeof eb);
            tar_usage_error("Invalid transform expression: %s", eb);
        }
        x->has_re = 1;
        free(res);
        /* a \N past the groups the regex has */
        for (const char *r = x->repl; *r; r++) {
            if (*r != '\\' || !r[1]) continue;
            r++;
            if (*r >= '1' && *r <= '9' && *r - '0' > (int)x->re.re_nsub)
                tar_usage_error("Invalid transform replacement: back reference out of range");
        }
    }
}

/* NAME transformed by every expression for its KIND; a new string */
static char *tar_transform1(const char *name, int kind);
/* a directory's trailing slash is not part of what is transformed */
static char *tar_transform(const char *name, int kind) {
    size_t n = strlen(name), k = n;
    while (k > 1 && name[k - 1] == '/') k--;
    if (k == n) return tar_transform1(name, kind);
    char *base = tar_strndup(name, k);
    char *t = tar_transform1(base, kind);
    free(base);
    size_t tl = strlen(t);
    t = (char *)tar_realloc(t, tl + (n - k) + 1);
    memcpy(t + tl, name + k, n - k);
    t[tl + n - k] = 0;
    return t;
}

static char *tar_transform1(const char *name, int kind) {
    char *cur = tar_strdup(name);
    for (int i = 0; i < tar_nxfs; i++) {
        struct tar_xf *x = &tar_xfs[i];
        if (!(x->flags & kind)) continue;
        struct tar_sb out = {0};
        const char *s = cur;
        size_t off = 0, len = strlen(cur);
        int count = 0, done = 0;
        regmatch_t m[10];
        while (off <= len) {
            int ef = off ? REG_NOTBOL : 0;
            if (regexec(&x->re, s + off, 10, m, ef) != 0) break;
            count++;
            size_t so = off + (size_t)m[0].rm_so, eo = off + (size_t)m[0].rm_eo;
            tar_sb_addn(&out, s + off, so - off);
            int apply = x->nth ? count == x->nth : 1;
            if (apply) {
                /* the replacement, with case conversion */
                int cmode = 0, one = 0;     /* cmode: 1 lower 2 upper; one: next char */
                const char *r = x->repl;
                for (; *r; r++) {
                    const char *ins = 0; size_t inl = 0;
                    char ch;
                    if (*r == '\\' && r[1]) {
                        r++;
                        if (*r >= '0' && *r <= '9') {
                            int k = *r - '0';
                            if (k <= (int)x->re.re_nsub && m[k].rm_so >= 0) {
                                ins = s + off + m[k].rm_so;
                                inl = (size_t)(m[k].rm_eo - m[k].rm_so);
                            } else continue;
                        } else if (*r == 'L') { cmode = 1; one = 0; continue; }
                        else if (*r == 'U') { cmode = 2; one = 0; continue; }
                        else if (*r == 'E') { cmode = 0; one = 0; continue; }
                        else if (*r == 'l') { one = 1; continue; }
                        else if (*r == 'u') { one = 2; continue; }
                        else {
                            switch (*r) {
                            case 'n': ch = '\n'; break;
                            case 't': ch = '\t'; break;
                            case 'a': ch = '\a'; break;
                            case 'f': ch = '\f'; break;
                            case 'v': ch = '\v'; break;
                            case 'r': ch = '\r'; break;
                            case '&': case '\\': ch = *r; break;
                            default:
                                /* an escape sed does not know stays as it is */
                                tar_sb_addc(&out, '\\');
                                ch = *r;
                            }
                            ins = &ch; inl = 1;
                        }
                    } else if (*r == '&') {
                        ins = s + so;
                        inl = eo - so;
                    } else {
                        ch = *r;
                        ins = &ch; inl = 1;
                    }
                    for (size_t k = 0; k < inl; k++) {
                        char c = ins[k];
                        if (one == 1) { c = (char)tolower((unsigned char)c); one = 0; }
                        else if (one == 2) { c = (char)toupper((unsigned char)c); one = 0; }
                        else if (cmode == 1) c = (char)tolower((unsigned char)c);
                        else if (cmode == 2) c = (char)toupper((unsigned char)c);
                        tar_sb_addc(&out, c);
                    }
                }
                done = 1;
            } else {
                tar_sb_addn(&out, s + so, eo - so);
            }
            if (eo == so) {
                /* an empty match: copy one character and go on */
                if (eo < len) tar_sb_addc(&out, s[eo]);
                off = eo + 1;
            } else off = eo;
            if (done && !x->global) break;
        }
        if (off < len) tar_sb_addn(&out, s + off, len - off);
        if (!out.s) out.s = tar_strdup("");
        free(cur);
        cur = tar_sb_take(&out);
    }
    return cur;
}

/* ---- --pax-option ------------------------------------------------------------- */

static int tar_parse_pax_option(const char *arg) {
    /* comma-separated keyword[[:]=value]; the keyword as it is, the
     * value without its leading blanks (as GNU stores them) */
    const char *p = arg;
    while (*p) {
        struct tar_sb kw = {0}, val = {0};
        int has_eq = 0, colon = 0;
        while (*p && *p != ',') {
            if (has_eq && !val.n && (*p == ' ' || *p == '\t')) { p++; continue; }
            if (!has_eq && *p == ':' && p[1] == '=') { colon = 1; has_eq = 1; p += 2; continue; }
            if (!has_eq && *p == '=') { has_eq = 1; p++; continue; }
            tar_sb_addc(has_eq ? &val : &kw, *p++);
        }
        if (*p == ',') p++;
        char *k = tar_sb_take(&kw), *v = tar_sb_take(&val);
        if (!has_eq)
            tar_usage_error("Keyword %s is unknown or not yet implemented", k);
        if (!strcmp(k, "delete")) {
            tar_o.pax_delete = (char **)tar_realloc(tar_o.pax_delete, (size_t)(tar_o.npax_delete + 1) * sizeof(char *));
            tar_o.pax_delete[tar_o.npax_delete++] = v;
        } else if (!strcmp(k, "exthdr.name")) {
            tar_o.exthdr_name = v;
        } else if (!strcmp(k, "globexthdr.name")) {
            tar_o.globexthdr_name = v;
        } else if (!strcmp(k, "exthdr.mtime") || !strcmp(k, "globexthdr.mtime")) {
            intmax_t s; long ns;
            /* a plain number is seconds; anything else a date */
            const char *q = v;
            if (*q == '-' || *q == '+') q++;
            int num = isdigit((unsigned char)*q) != 0;
            for (; *q; q++) if (!isdigit((unsigned char)*q) && *q != '.') num = 0;
            if (num) { s = (intmax_t)strtoll(v, 0, 10); ns = 0; }
            else tar_parse_date_opt(k, v, &s, &ns);
            if (k[0] == 'e') { tar_o.exthdr_mtime = s; tar_o.exthdr_mtime_set = 1; }
            else { tar_o.globexthdr_mtime = s; tar_o.globexthdr_mtime_set = 1; }
        } else {
            tar_o.pax_kw = (char **)tar_realloc(tar_o.pax_kw, (size_t)(tar_o.npax + 1) * sizeof(char *));
            tar_o.pax_val = (char **)tar_realloc(tar_o.pax_val, (size_t)(tar_o.npax + 1) * sizeof(char *));
            tar_o.pax_global = (int *)tar_realloc(tar_o.pax_global, (size_t)(tar_o.npax + 1) * sizeof(int));
            tar_o.pax_kw[tar_o.npax] = k;
            tar_o.pax_val[tar_o.npax] = v;
            tar_o.pax_global[tar_o.npax++] = !colon;
            continue;
        }
        free(k);
    }
    return 0;
}

/* ---- the archive ----------------------------------------------------------------
 *
 * Records of -b blocks (20 by default): written whole, the last one padded
 * with zeros after the two zero blocks that end an archive; read whole
 * from a file and as they come from a pipe. A compressed archive goes
 * through the compressor in a child -- "PROG" on the way out (what tar
 * runs is `sh -c PROG`), "PROG -d" on the way in, the archive on the
 * other end -- as GNU's does. A plain archive read from a file that turns
 * out to be compressed is read again through the right decompressor;
 * from a pipe, "Archive is compressed. Use -z option". */

static struct {
    int fd;
    const char *name;             /* as given: "-" for stdin/stdout */
    unsigned char *rec;
    size_t recsize;
    size_t pos;                   /* the next block in rec */
    size_t avail;                 /* read: valid bytes in rec */
    uintmax_t block;              /* the block number of the next block */
    uintmax_t bytes;              /* bytes read or written so far */
    uintmax_t records;            /* records so far (--checkpoint) */
    pid_t child;
    int eof;
    int writing;
    int is_stdio;
    int regular;
    dev_t dev; ino_t ino; int have_id;   /* to refuse archiving the archive */
    int compressed_by_us;
    struct timeval start;
    unsigned char *mem;           /* reading from memory (--delete) */
    size_t mem_len, mem_pos;
    uintmax_t passed;             /* records read or seeked over (--totals) */
} tar_ar;

static void tar_checkpoint_tick(int writing);

/* -w: "extract 'NAME'?" -- 1 for an answer starting with y or Y; the end
 * of the input (a newline said for it) is no */
static int tar_confirm(const char *action, const char *name) {
    if (!tar_o.interactive) return 1;
    FILE *out = tar_ar.is_stdio && tar_ar.writing ? stderr : stdout;
    fprintf(out, "%s %s?", action, tar_q(name));
    fflush(out);
    char line[256];
    ssize_t k = 0;
    int c = 0;
    char ch;
    while ((size_t)k < sizeof line - 1) {
        ssize_t r = read(0, &ch, 1);
        if (r <= 0) { c = -1; break; }
        if (ch == '\n') break;
        line[k++] = ch;
    }
    line[k] = 0;
    if (c < 0 && k == 0) { fputc('\n', out); fflush(out); return 0; }
    return line[0] == 'y' || line[0] == 'Y';
}

static void tar_cleanup_children(void) {
    if (tar_ar.child > 0) {
        if (tar_ar.fd >= 0) close(tar_ar.fd);
        tar_ar.fd = -1;
        int st;
        waitpid(tar_ar.child, &st, 0);
        tar_ar.child = 0;
    }
}

static int tar_full_write(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0) { if (errno == EINTR) continue; return (int)done ? (int)done : -1; }
        if (w == 0) break;
        done += (size_t)w;
    }
    return (int)done;
}

/* read until N bytes or the end; the count, or -1 */
static ssize_t tar_full_read(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return done ? (ssize_t)done : -1; }
        if (r == 0) break;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

/* the words of a compressor command, and "-d" for reading */
static char **tar_cmd_words(const char *prog, int decompress) {
    int cap = 8, n = 0;
    char **v = (char **)tar_alloc((size_t)cap * sizeof *v);
    const char *p = prog;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (n + 2 >= cap) { cap *= 2; v = (char **)tar_realloc(v, (size_t)cap * sizeof *v); }
        v[n++] = tar_strndup(s, (size_t)(p - s));
    }
    if (decompress) v[n++] = tar_strdup("-d");
    v[n] = 0;
    return v;
}

static void tar_child_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void tar_child_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "%s (child): ", su_name);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fprintf(stderr, "%s (child): Error is not recoverable: exiting now\n", su_name);
    _exit(TAR_EXIT_FAILURE);
}

/* the archive file's open flags for writing */
static int tar_open_archive_write(const char *name, int append) {
    if (!strcmp(name, "-")) return 1;
    int fd = open(name, append ? O_RDWR : (O_WRONLY | O_CREAT | O_TRUNC), 0666);
    return fd;
}

/* start the compressor for writing: tar writes into a pipe */
static int tar_spawn_compress_write(const char *prog, const char *name) {
    int pfd[2];
    signal(SIGPIPE, SIG_IGN);
    if (pipe(pfd) != 0) tar_fatal("Cannot create pipe: %s", strerror(errno));
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) tar_fatal("Cannot fork: %s", strerror(errno));
    if (pid == 0) {
        close(pfd[1]);
        int afd = 1;
        if (strcmp(name, "-") != 0) {
            afd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (afd < 0) {
                int e = errno;
                tar_child_fatal("%s: Cannot open: %s", tar_qc(name), strerror(e));
            }
        }
        if (pfd[0] != 0) { dup2(pfd[0], 0); close(pfd[0]); }
        if (afd != 1) { dup2(afd, 1); close(afd); }
        char *argv[4] = { (char *)"/bin/sh", (char *)"-c", (char *)prog, 0 };
        execv("/bin/sh", argv);
        int e = errno;
        tar_child_fatal("%s: Cannot exec: %s", tar_qc(prog), strerror(e));
    }
    close(pfd[0]);
    tar_ar.child = pid;
    return pfd[1];
}

/* start the decompressor for reading: tar reads from a pipe */
static int tar_spawn_decompress_read(const char *prog, const char *name, int infd) {
    int pfd[2];
    if (pipe(pfd) != 0) tar_fatal("Cannot create pipe: %s", strerror(errno));
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) tar_fatal("Cannot fork: %s", strerror(errno));
    if (pid == 0) {
        close(pfd[0]);
        if (pfd[1] != 1) { dup2(pfd[1], 1); close(pfd[1]); }
        if (infd >= 0) {
            if (infd != 0) { dup2(infd, 0); close(infd); }
        } else if (strcmp(name, "-") != 0) {
            int afd = su_open_read(name, 0);
            if (afd < 0) {
                int e = errno;
                tar_child_fatal("%s: Cannot open: %s", tar_qc(name), strerror(e));
            }
            struct stat ast;
            if (fstat(afd, &ast) == 0 && S_ISDIR(ast.st_mode)) {
                /* GNU's child reads it itself and fails; the
                 * decompressor then has nothing */
                fprintf(stderr, "%s (child): %s: Cannot read: %s\n", su_name, tar_qc(name), strerror(EISDIR));
                fprintf(stderr, "%s (child): At beginning of tape, quitting now\n", su_name);
                fprintf(stderr, "%s (child): Error is not recoverable: exiting now\n", su_name);
                int ep[2];
                if (pipe(ep) == 0) {
                    close(ep[1]);
                    pid_t g = fork();
                    if (g == 0) {
                        dup2(ep[0], 0);
                        close(ep[0]);
                        char **argv = tar_cmd_words(prog, 1);
                        execvp(argv[0], argv);
                        _exit(127);
                    }
                    close(ep[0]);
                    int st;
                    if (g > 0) waitpid(g, &st, 0);
                }
                _exit(TAR_EXIT_FAILURE);
            }
            if (afd != 0) { dup2(afd, 0); close(afd); }
        }
        char **argv = tar_cmd_words(prog, 1);
        execvp(argv[0], argv);
        int e = errno;
        tar_child_fatal("%s: Cannot exec: %s", tar_qc(argv[0]), strerror(e));
    }
    close(pfd[1]);
    tar_ar.child = pid;
    return pfd[0];
}

/* wait for the compressor; its failure is fatal */
static void tar_wait_child(void) {
    if (tar_ar.child <= 0) return;
    int st = 0;
    pid_t p;
    while ((p = waitpid(tar_ar.child, &st, 0)) < 0 && errno == EINTR) ;
    tar_ar.child = 0;
    if (p < 0) return;
    if (WIFSIGNALED(st)) {
        if (WTERMSIG(st) == SIGPIPE && !tar_ar.writing) return;
        if (!tar_o.ignore_cmd_error)
            tar_fatal("Child died with signal %d", WTERMSIG(st));
    } else if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
        tar_fatal("Child returned status %d", WEXITSTATUS(st));
    }
}

/* the compressor a magic number names */
static const char *tar_magic_prog(const unsigned char *b, size_t n) {
    if (n >= 2 && b[0] == 0x1f && b[1] == 0x8b) return "gzip";
    if (n >= 2 && b[0] == 0x1f && b[1] == 0x9d) return "compress";
    if (n >= 3 && b[0] == 'B' && b[1] == 'Z' && b[2] == 'h') return "bzip2";
    if (n >= 6 && !memcmp(b, "\xfd" "7zXZ\0", 6)) return "xz";
    if (n >= 4 && !memcmp(b, "LZIP", 4)) return "lzip";
    if (n >= 4 && !memcmp(b, "\x89LZO", 4)) return "lzop";
    if (n >= 4 && !memcmp(b, "\x28\xb5\x2f\xfd", 4)) return "zstd";
    if (n >= 3 && b[0] == 0x5d && b[1] == 0 && b[2] == 0) return "lzma";
    return 0;
}

/* the compressor -a picks from the archive's suffix */
static const char *tar_suffix_prog(const char *name) {
    static const struct { const char *suf; const char *prog; } t[] = {
        { ".gz", "gzip" }, { ".tgz", "gzip" }, { ".taz", "gzip" },
        { ".Z", "compress" }, { ".taZ", "compress" },
        { ".bz2", "bzip2" }, { ".tz2", "bzip2" }, { ".tbz2", "bzip2" }, { ".tbz", "bzip2" },
        { ".lz", "lzip" }, { ".lzma", "lzma" }, { ".tlz", "lzma" },
        { ".lzo", "lzop" }, { ".xz", "xz" }, { ".txz", "xz" },
        { ".zst", "zstd" }, { ".tzst", "zstd" }, { 0, 0 }
    };
    size_t n = strlen(name);
    for (int i = 0; t[i].suf; i++) {
        size_t k = strlen(t[i].suf);
        if (n > k && !strcmp(name + n - k, t[i].suf)) return t[i].prog;
    }
    return 0;
}

static void tar_ar_init(size_t recsize) {
    tar_ar.recsize = recsize;
    tar_ar.rec = (unsigned char *)tar_zalloc(recsize);
    tar_ar.pos = 0; tar_ar.avail = 0; tar_ar.block = 0; tar_ar.bytes = 0;
    tar_ar.records = 0; tar_ar.eof = 0; tar_ar.passed = 0;
    gettimeofday(&tar_ar.start, 0);
}

static const char *tar_archive_name0(void) {
    if (tar_o.narchives) return tar_o.archives[0];
    const char *t = getenv("TAPE");
    return t && *t ? t : "-";
}

/* HOST:FILE is a remote archive (rmt over rsh) unless --force-local;
 * there is no rsh here: GNU's answer for a host it cannot find */
static const char *tar_archive_name(void) {
    const char *n = tar_archive_name0();
    if (!tar_o.force_local) {
        const char *c = strchr(n, ':');
        if (c && c != n && memchr(n, '/', (size_t)(c - n)) == 0) {
            const char *at = memchr(n, '@', (size_t)(c - n));
            const char *h = at ? at + 1 : n;
            fflush(stdout);
            fprintf(stderr, "%s: Cannot connect to %.*s: resolve failed\n", su_name, (int)(c - h), h);
            exit(128);
        }
    }
    return n;
}

static void tar_note_archive_id(void) {
    struct stat st;
    if (tar_ar.fd >= 0 && fstat(tar_ar.fd, &st) == 0) {
        tar_ar.regular = S_ISREG(st.st_mode);
        if (S_ISREG(st.st_mode) && st.st_ino) {
            tar_ar.dev = st.st_dev; tar_ar.ino = st.st_ino; tar_ar.have_id = 1;
        }
    }
}

/* open the archive for writing a new one */
static void tar_open_write(void) {
    const char *name = tar_archive_name();
    tar_ar.name = name;
    tar_ar.writing = 1;
    tar_ar.is_stdio = !strcmp(name, "-");
    const char *prog = tar_o.compress;
    if (!prog && tar_o.auto_compress) prog = tar_suffix_prog(name);
    tar_ar_init(tar_o.record_size);
    if (prog) {
        tar_ar.fd = tar_spawn_compress_write(prog, name);
        tar_ar.compressed_by_us = 1;
        if (!tar_ar.is_stdio) {
            struct stat st;
            /* the child makes the file; note it once it is there */
            if (stat(name, &st) == 0 && S_ISREG(st.st_mode)) {
                tar_ar.dev = st.st_dev; tar_ar.ino = st.st_ino; tar_ar.have_id = 1;
            }
        }
        return;
    }
    tar_ar.fd = tar_open_archive_write(name, 0);
    if (tar_ar.fd < 0) {
        int e = errno;
        tar_fatal("%s: Cannot open: %s", tar_qc(name), strerror(e));
    }
    tar_note_archive_id();
    if (tar_o.verify && tar_ar.fd > 1) tar_verify_fd = open(name, O_RDONLY);
    struct stat a, n;
    if (strcmp(name, "-") != 0 && fstat(tar_ar.fd, &a) == 0 && stat("/dev/null", &n) == 0 && S_ISCHR(a.st_mode) &&
        a.st_rdev == n.st_rdev && a.st_ino == n.st_ino)
        tar_to_dev_null = 1;
}


static void tar_flush_record(void) {
    size_t n = tar_ar.recsize;
    /* a checkpoint comes as a record is about to go out */
    tar_ar.records++;
    tar_checkpoint_tick(1);
    int w = tar_full_write(tar_ar.fd, tar_ar.rec, n);
    if (w < 0) {
        int e = errno;
        if (e == EPIPE && tar_ar.child) tar_wait_child();
        tar_fatal("%s: Cannot write: %s", tar_qc(tar_ar.name), strerror(e));
    }
    if ((size_t)w < n) {
        tar_fatal("%s: Wrote only %d of %zu bytes", tar_qc(tar_ar.name), w, n);
    }
    tar_ar.bytes += n;
    memset(tar_ar.rec, 0, n);
    tar_ar.pos = 0;
}

/* ---- -M: volumes --------------------------------------------------------------
 *
 * Writing, a volume holds -L's length (N x 1024 bytes, or bytes with a
 * suffix) rounded up to whole records; the next volume is the next -f,
 * or -- when they run out -- "Prepare volume #N for 'NAME' and hit
 * return: " answered on standard input (its end: "EOF where user reply
 * was expected", fatal). A volume starts with its label ("LABEL Volume
 * N", under -V) and, when a member is cut by the volume's end, an 'M'
 * header: the member's name, the bytes still to come, and in the offset
 * field the bytes already written. Reading, the volumes follow each other
 * the same way; the 'M' header is checked against what was being read
 * ("This volume is out of sequence", "'NAME' is the wrong size"). */

static uintmax_t tar_mv_cap;          /* blocks per volume; 0 no limit */
static uintmax_t tar_mv_blocks;       /* blocks in this volume */
static int tar_mv_vol = 1;            /* this volume's number */
static int tar_mv_switching;
static char *tar_mv_name;             /* the member being written */
static uintmax_t tar_mv_total, tar_mv_left;
static char *tar_rd_name;             /* the member being read */
static uintmax_t tar_rd_total, tar_rd_left;

static void tar_write_label_block(const char *text);
static void tar_write_mv_header(const char *name, uintmax_t size, uintmax_t offset);

/* the archive name for volume N: the -f's in turn, then a prompt (the
 * names go round; a volume refused asks again under its number) */
static int tar_mv_idx = 1;
static char *tar_start_dir;
static const char *tar_mv_volume_name(int n, int writing) {
    int na = tar_o.narchives ? tar_o.narchives : 1;
    int idx = tar_mv_idx++;
    const char *nm = tar_o.narchives ? tar_o.archives[idx % na] : tar_archive_name0();
    if (idx < na) return nm;
    fflush(stdout);
    fprintf(stderr, "\aPrepare volume #%d for %s and hit return: ", n, tar_q(nm));
    fflush(stderr);
    char ch;
    ssize_t r = read(0, &ch, 1);
    if (r <= 0) {
        fprintf(stderr, "%s: EOF where user reply was expected\n", su_name);
        if (writing) fprintf(stderr, "%s: WARNING: Archive is incomplete\n", su_name);
        fprintf(stderr, "%s: Error is not recoverable: exiting now\n", su_name);
        exit(TAR_EXIT_FAILURE);
    }
    while (ch != '\n' && read(0, &ch, 1) == 1) ;
    return nm;
}

/* a volume's name as opened: relative to where tar started */
static char *tar_mv_path(const char *nm) {
    if (nm[0] == '/' || !tar_start_dir) return tar_strdup(nm);
    char *p = (char *)tar_alloc(strlen(tar_start_dir) + strlen(nm) + 2);
    sprintf(p, "%s/%s", tar_start_dir, nm);
    return p;
}

static void tar_mv_switch_write(void) {
    tar_mv_switching = 1;
    if (tar_ar.fd > 1) close(tar_ar.fd);
    tar_mv_vol++;
    const char *nm = tar_mv_volume_name(tar_mv_vol, 1);
    tar_ar.name = nm;
    char *path = tar_mv_path(nm);
    tar_ar.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    free(path);
    if (tar_ar.fd < 0) {
        int e = errno;
        tar_fatal("%s: Cannot open: %s", tar_qc(nm), strerror(e));
    }
    tar_mv_blocks = 0;
    if (tar_o.label) {
        char b[160];
        snprintf(b, sizeof b, "%s Volume %d", tar_o.label, tar_mv_vol);
        tar_write_label_block(b);
    }
    if (tar_mv_name && tar_mv_left > 0)
        tar_write_mv_header(tar_mv_name, tar_mv_left, tar_mv_total - tar_mv_left);
    tar_mv_switching = 0;
}

/* the next block to fill (zeroed); tar_w_done() hands it to the archive */
static unsigned char *tar_w_block(void) {
    if (tar_mv_cap && !tar_mv_switching && tar_mv_blocks >= tar_mv_cap && tar_ar.pos == 0)
        tar_mv_switch_write();
    return tar_ar.rec + tar_ar.pos;
}
static void tar_w_done(void) {
    tar_ar.pos += TAR_BLK;
    tar_ar.block++;
    tar_mv_blocks++;
    if (tar_ar.pos >= tar_ar.recsize) tar_flush_record();
}
/* a member's data block went out */
static void tar_mv_data_done(size_t n) {
    if (!tar_mv_name) return;
    tar_mv_left = tar_mv_left > n ? tar_mv_left - n : 0;
}
static void tar_mv_start(const char *name, uintmax_t size) {
    if (!tar_mv_cap) return;
    free(tar_mv_name);
    tar_mv_name = tar_strdup(name);
    tar_mv_total = tar_mv_left = size;
}
static void tar_w_data(const void *p, size_t n) {
    const unsigned char *s = (const unsigned char *)p;
    while (n) {
        unsigned char *b = tar_w_block();
        size_t k = n < TAR_BLK ? n : TAR_BLK;
        memcpy(b, s, k);
        tar_w_done();
        s += k; n -= k;
    }
}

/* the end: two zero blocks, then the record filled out */
static void tar_close_write(void) {
    tar_w_block(); tar_w_done();
    tar_w_block(); tar_w_done();
    if (tar_ar.pos) tar_flush_record();
    if (tar_ar.fd > 1) {
        if (close(tar_ar.fd) != 0 && !tar_ar.child) {
            int e = errno;
            tar_err("%s: Cannot close: %s", tar_qc(tar_ar.name), strerror(e));
        }
    } else if (tar_ar.fd == 1) {
        fflush(stdout);
    }
    tar_ar.fd = -1;
    tar_wait_child();
}

/* ---- reading -- */

static int tar_reading_started;

/* open the archive for reading */
static void tar_open_read(void) {
    const char *name = tar_archive_name();
    tar_ar.name = name;
    tar_ar.writing = 0;
    tar_ar.is_stdio = !strcmp(name, "-");
    tar_ar_init(tar_o.record_size);
    const char *prog = tar_o.compress;
    if (prog) {
        tar_ar.fd = tar_spawn_decompress_read(prog, name, -1);
        tar_ar.compressed_by_us = 1;
        return;
    }
    if (tar_ar.is_stdio) tar_ar.fd = 0;
    else if (tar_verify_fd >= 0) {
        tar_ar.fd = tar_verify_fd;
        tar_verify_fd = -1;
        lseek(tar_ar.fd, 0, SEEK_SET);
    } else {
        tar_ar.fd = su_open_read(name, 0);
        if (tar_ar.fd < 0) {
            int e = errno;
            tar_fatal("%s: Cannot open: %s", tar_qc(name), strerror(e));
        }
    }
    tar_note_archive_id();
}

/* GNU's words for a read that fails: "Cannot read", and at the archive's
 * start "At beginning of tape, quitting now" */
static void tar_read_failed(int e) __attribute__((noreturn));
static void tar_read_failed(int e) {
    tar_warn("%s: Cannot read: %s", tar_qc(tar_ar.name), strerror(e));
    if (tar_ar.passed <= 1) tar_fatal("At beginning of tape, quitting now");
    tar_fatal("%s: Read error: %s", tar_qc(tar_ar.name), strerror(e));
}

/* read the next record; 0 at the end */
static int tar_fill_record(void) {
    if (tar_ar.eof) return 0;
    tar_ar.passed++;
    ssize_t r;
    if (tar_ar.mem) {
        size_t k = tar_ar.mem_len - tar_ar.mem_pos;
        if (k > tar_ar.recsize) k = tar_ar.recsize;
        memcpy(tar_ar.rec, tar_ar.mem + tar_ar.mem_pos, k);
        tar_ar.mem_pos += k;
        r = (ssize_t)k;
    } else r = tar_full_read(tar_ar.fd, tar_ar.rec, tar_ar.recsize);
    if (r < 0) {
        int e = errno;
        tar_read_failed(e);
    }
    if (!tar_reading_started) {
        tar_reading_started = 1;
        /* a plain archive that is compressed after all */
        const char *prog = tar_ar.compressed_by_us ? 0 : tar_magic_prog(tar_ar.rec, (size_t)r);
        if (prog) {
            if (tar_ar.is_stdio || !tar_ar.regular)
                tar_fatal("Archive is compressed. Use %s option",
                          !strcmp(prog, "gzip") ? "-z" : !strcmp(prog, "bzip2") ? "-j" :
                          !strcmp(prog, "xz") ? "-J" : !strcmp(prog, "compress") ? "-Z" :
                          !strcmp(prog, "lzip") ? "--lzip" : !strcmp(prog, "lzma") ? "--lzma" :
                          !strcmp(prog, "lzop") ? "--lzop" : "--zstd");
            int afd = tar_ar.fd;
            lseek(afd, 0, SEEK_SET);
            tar_ar.fd = tar_spawn_decompress_read(prog, tar_ar.name, afd);
            close(afd);
            tar_ar.compressed_by_us = 1;
            r = tar_full_read(tar_ar.fd, tar_ar.rec, tar_ar.recsize);
            if (r < 0) {
                int e = errno;
                tar_fatal("%s: Read error: %s", tar_qc(tar_ar.name), strerror(e));
            }
        }
    }
    if (r == 0) { tar_ar.eof = 1; return 0; }
    tar_ar.bytes += (uintmax_t)r;
    tar_ar.records++;
    tar_checkpoint_tick(0);
    if ((size_t)r < tar_ar.recsize) tar_ar.eof = 1;
    /* a partial last block is as good as none */
    tar_ar.avail = (size_t)r - (size_t)r % TAR_BLK;
    tar_ar.pos = 0;
    return tar_ar.avail > 0;
}

static int tar_mv_switch_read(void);
static int tar_from_num(const unsigned char *f, size_t w, intmax_t *out, int *ok);
static int tar_header_sum_ok(const unsigned char *h);

/* the next block, or 0 at the end of the input */
static unsigned char *tar_r_block(void) {
    if (tar_ar.pos >= tar_ar.avail) {
        if (!tar_fill_record()) {
            if (!tar_o.multi_volume || tar_ar.child || tar_ar.mem || !tar_mv_switch_read()) return 0;
            if (tar_ar.pos >= tar_ar.avail && !tar_fill_record()) return 0;
        }
    }
    unsigned char *b = tar_ar.rec + tar_ar.pos;
    tar_ar.pos += TAR_BLK;
    tar_ar.block++;
    if (tar_rd_left) tar_rd_left = tar_rd_left > TAR_BLK ? tar_rd_left - TAR_BLK : 0;
    return b;
}

/* the next volume, while reading: 1 when it is open and placed after its
 * label and continuation header */
static int tar_mv_switch_read(void) {
    tar_mv_vol++;
    for (;;) {
        int na = tar_o.narchives ? tar_o.narchives : 1;
        if (tar_mv_vol > na && !tar_rd_left && !tar_rd_name) {
            /* nothing was cut: a last volume that just ended */
        }
        const char *nm = tar_mv_volume_name(tar_mv_vol, 0);
        if (tar_ar.fd > 0) close(tar_ar.fd);
        tar_ar.name = nm;
        char *path = tar_mv_path(nm);
        tar_ar.fd = su_open_read(path, 0);
        free(path);
        if (tar_ar.fd < 0) {
            int e = errno;
            tar_fatal("%s: Cannot open: %s", tar_qc(nm), strerror(e));
        }
        tar_ar.eof = 0; tar_ar.avail = 0; tar_ar.pos = 0;
        if (!tar_fill_record()) return 0;
        unsigned char *h = tar_ar.rec;
        if (h[TAR_H_TYPE] == 'V' && tar_header_sum_ok(h)) {
            tar_ar.pos += TAR_BLK;
            if (tar_ar.pos >= tar_ar.avail && !tar_fill_record()) return 0;
            h = tar_ar.rec + tar_ar.pos;
        }
        if (h[TAR_H_TYPE] != 'M' || !tar_header_sum_ok(h)) {
            if (tar_rd_left && tar_rd_name) {
                tar_warn("%s is not continued on this volume", tar_q(tar_rd_name));
                continue;
            }
            return 1;
        }
        char mn[101];
        size_t k = strnlen((const char *)h + TAR_H_NAME, 100);
        memcpy(mn, h + TAR_H_NAME, k); mn[k] = 0;
        intmax_t msize, moff; int ok;
        tar_from_num(h + TAR_H_SIZE, 12, &msize, &ok);
        tar_from_num(h + TAR_H_OFFSET, 12, &moff, &ok);
        if (tar_rd_left && tar_rd_name) {
            if (strcmp(mn, tar_rd_name) != 0) {
                tar_warn("%s is not continued on this volume", tar_q(tar_rd_name));
                continue;
            }
            if (tar_rd_total != (uintmax_t)msize + (uintmax_t)moff) {
                tar_warn("%s is the wrong size (%ju != %ju + %ju)", tar_q(tar_rd_name),
                         tar_rd_total, (uintmax_t)msize, (uintmax_t)moff);
                continue;
            }
            if (tar_rd_total - tar_rd_left != (uintmax_t)moff) {
                tar_warn("This volume is out of sequence (%ju - %ju != %ju)",
                         tar_rd_total, tar_rd_left, (uintmax_t)moff);
                continue;
            }
        }
        /* past the continuation header: its data is the member's rest */
        tar_ar.pos += TAR_BLK;
        return 1;
    }
}

/* up to MAX bytes of data from the record (at least a block's worth is
 * asked for): a pointer and the count, 0 at the end */
static unsigned char *tar_r_run(uintmax_t max, size_t *got) {
    if (tar_ar.pos >= tar_ar.avail) {
        unsigned char *b = tar_r_block();
        if (!b) { *got = 0; return 0; }
        *got = max < TAR_BLK ? (size_t)max : TAR_BLK;
        return b;
    }
    size_t inrec = tar_ar.avail - tar_ar.pos;
    uintmax_t blocks = (max + TAR_BLK - 1) / TAR_BLK;
    size_t take = blocks * TAR_BLK < inrec ? (size_t)(blocks * TAR_BLK) : inrec;
    unsigned char *b = tar_ar.rec + tar_ar.pos;
    tar_ar.pos += take;
    tar_ar.block += take / TAR_BLK;
    if (tar_rd_left) tar_rd_left = tar_rd_left > take ? tar_rd_left - take : 0;
    *got = take < max ? take : (size_t)max;
    return b;
}

/* put the last block back (to read it again) */
static void tar_r_unget(void) {
    tar_ar.pos -= TAR_BLK;
    tar_ar.block--;
}

static void tar_close_read(void) {
    if (tar_ar.child > 0) {
        /* drain, so the decompressor ends as it would */
        char buf[4096];
        while (read(tar_ar.fd, buf, sizeof buf) > 0) ;
        close(tar_ar.fd);
        tar_ar.fd = -1;
        tar_wait_child();
    } else if (tar_ar.fd > 0) {
        close(tar_ar.fd);
        tar_ar.fd = -1;
    }
}

/* ---- --totals, --checkpoint ------------------------------------------------- */

/* GNU's human-readable count of NUM/DEN: "10KiB", "1.5MiB", rounded up
 * (gnulib human_readable's default). In integers: this program is built
 * without SSE and libtoby's printf with it, so a double given to %f came
 * out 0 -- "(0KiB)" (finding 230). */
static void tar_human(char *buf, size_t cap, unsigned __int128 num, uintmax_t den,
                      const char *unit_suffix) {
    static const char *u[] = { "", "Ki", "Mi", "Gi", "Ti", "Pi", "Ei" };
    unsigned __int128 n = num, d = den ? den : 1;
    int i = 0;
    while (n >= d * 1024 && i < 6) { d *= 1024; i++; }
    if (i == 0) {
        snprintf(buf, cap, "%ju%s", (uintmax_t)((n + d - 1) / d), unit_suffix);
    } else if (n < d * 10) {
        uintmax_t t = (uintmax_t)((n * 10 + d - 1) / d);      /* tenths, up */
        if (t >= 100) snprintf(buf, cap, "%ju%s%s", t / 10, u[i], unit_suffix);
        else snprintf(buf, cap, "%ju.%ju%s%s", t / 10, t % 10, u[i], unit_suffix);
    } else {
        snprintf(buf, cap, "%ju%s%s", (uintmax_t)((n + d - 1) / d), u[i], unit_suffix);
    }
}

static void tar_print_totals(int writing, uintmax_t bytes) {
    struct timeval now;
    gettimeofday(&now, 0);
    int64_t us = (int64_t)(now.tv_sec - tar_ar.start.tv_sec) * 1000000 +
                 (int64_t)(now.tv_usec - tar_ar.start.tv_usec);
    char h[32], r[32];
    tar_human(h, sizeof h, bytes, 1, "B");
    if (us > 0) tar_human(r, sizeof r, (unsigned __int128)bytes * 1000000, (uintmax_t)us, "B/s");
    else snprintf(r, sizeof r, "? B/s");
    /* "10KiB": the B is part of the unit only with a prefix */
    size_t hl = strlen(h);
    if (hl > 1 && h[hl - 1] == 'B' && !isdigit((unsigned char)h[hl - 2]) && 0) h[hl - 1] = 0;
    fflush(stdout);
    fprintf(stderr, "Total bytes %s: %ju (%s, %s)\n", writing ? "written" : "read", bytes, h, r);
}

static void tar_add_checkpoint_action(const char *arg) {
    static const char *names[] = { "bell", "dot", ".", "echo", "totals", 0 };
    static const char *pre[] = { "echo=", "exec=", "sleep=", "ttyout=", "wait=", 0 };
    int ok = 0;
    for (int i = 0; names[i]; i++) if (!strcmp(arg, names[i])) ok = 1;
    for (int i = 0; pre[i]; i++) if (!strncmp(arg, pre[i], strlen(pre[i]))) ok = 1;
    if (!ok) tar_fatal("%s: unknown checkpoint action", tar_qc(arg));
    tar_o.cp_actions = (char **)tar_realloc(tar_o.cp_actions, (size_t)(tar_o.ncp_actions + 1) * sizeof(char *));
    tar_o.cp_actions[tar_o.ncp_actions++] = (char *)arg;
    if (!tar_o.checkpoint_set) { tar_o.checkpoint = 10; tar_o.checkpoint_set = 1; }
}

static void tar_checkpoint_tick(int writing) {
    if (!tar_o.checkpoint_set || tar_o.checkpoint <= 0) return;
    if (tar_ar.records % (uintmax_t)tar_o.checkpoint) return;
    uintmax_t n = tar_ar.records;
    if (!tar_o.ncp_actions) {
        fflush(stdout);
        fprintf(stderr, "%s: %s checkpoint %ju\n", su_name, writing ? "Write" : "Read", n);
        return;
    }
    for (int i = 0; i < tar_o.ncp_actions; i++) {
        const char *a = tar_o.cp_actions[i];
        if (!strcmp(a, "bell")) { fputc('\a', stderr); fflush(stderr); }
        else if (!strcmp(a, "dot") || !strcmp(a, ".")) { fputc('.', tar_o.vout); fflush(tar_o.vout); }
        else if (!strncmp(a, "echo", 4) && (a[4] == 0 || a[4] == '=')) {
            fflush(stdout);
            if (a[4] == 0) fprintf(stderr, "%s: %s checkpoint %ju\n", su_name, writing ? "Write" : "Read", n);
            else {
                /* %s: "read"/"write", %u the number, %T the totals; %{...}t a date */
                fprintf(stderr, "%s: ", su_name);
                for (const char *p = a + 5; *p; p++) {
                    if (*p == '%' && p[1] == 's') { fputs(writing ? "write" : "read", stderr); p++; }
                    else if (*p == '%' && p[1] == 'u') { fprintf(stderr, "%ju", n); p++; }
                    else if (*p == '%' && p[1] == '%') { fputc('%', stderr); p++; }
                    else fputc(*p, stderr);
                }
                fputc('\n', stderr);
            }
        } else if (!strncmp(a, "ttyout=", 7)) {
            FILE *t = fopen("/dev/tty", "w");
            if (t) {
                for (const char *p = a + 7; *p; p++) {
                    if (*p == '%' && p[1] == 's') { fputs(writing ? "write" : "read", t); p++; }
                    else if (*p == '%' && p[1] == 'u') { fprintf(t, "%ju", n); p++; }
                    else fputc(*p, t);
                }
                fclose(t);
            }
        } else if (!strncmp(a, "exec=", 5)) {
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
                char nb[32];
                snprintf(nb, sizeof nb, "%ju", n);
                setenv("TAR_CHECKPOINT", nb, 1);
                setenv("TAR_ARCHIVE", tar_ar.name, 1);
                setenv("TAR_SUBCOMMAND", tar_cmd_letters[tar_o.cmd], 1);
                {
                    int f = tar_o.format == TAR_F_DEFAULT ? TAR_F_GNU : tar_o.format;
                    setenv("TAR_FORMAT", f == TAR_F_GNU ? "gnu" : f == TAR_F_OLDGNU ? "oldgnu" :
                           f == TAR_F_POSIX ? "posix" : f == TAR_F_USTAR ? "ustar" : "v7", 1);
                    snprintf(nb, sizeof nb, "%d", tar_o.blocking);
                    setenv("TAR_BLOCKING_FACTOR", nb, 1);
                }
                char *argv[4] = { (char *)"/bin/sh", (char *)"-c", (char *)(a + 5), 0 };
                execv("/bin/sh", argv);
                _exit(127);
            }
            if (pid > 0) { int st; waitpid(pid, &st, 0); }
        } else if (!strncmp(a, "sleep=", 6)) {
            sleep((unsigned)atoi(a + 6));
        } else if (!strcmp(a, "totals")) {
            tar_print_totals(writing, tar_ar.bytes);
        }
    }
}

/* ---- headers: writing ------------------------------------------------------------
 *
 * The 512-byte header, field by field as GNU writes it in each format:
 *
 *   name[100] mode[8] uid[8] gid[8] size[12] mtime[12] chksum[8] typeflag
 *   linkname[100] magic[6] version[2] uname[32] gname[32] devmajor[8]
 *   devminor[8] prefix[155] (ustar, posix)
 *
 * Numbers are octal, zero-padded, ending in NUL ("0000644\0"); one too
 * big for that is base-256 in the gnu formats (0x80, then the value
 * big-endian; a negative one two's complement from 0xff), a pax keyword in
 * posix (the field then 0), and in ustar and v7 "value N out of uid_t
 * range 0..2097151" -- the member is not written, the status 1. The
 * checksum is six octal digits, NUL, space. Magic "ustar  \0" for gnu and
 * oldgnu, "ustar\0" "00" for ustar and posix, none for v7. devmajor and
 * devminor are written only for device nodes; uname and gname are empty
 * under --numeric-owner (and in v7). */


static void tar_octal(char *f, size_t w, uintmax_t v) {
    f[w - 1] = 0;
    for (size_t i = w - 1; i > 0; i--) { f[i - 1] = (char)('0' + (v & 7)); v >>= 3; }
}

/* base-256: 0x80 (or the sign) and the value big-endian */
static void tar_base256(char *f, size_t w, intmax_t v) {
    intmax_t u = v;
    for (size_t i = w; i > 0; i--) {
        f[i - 1] = (char)(u & 0xff);
        u = u < 0 ? ~(~u >> 8) : u >> 8;     /* the sign carried along */
    }
    if (v < 0) f[0] = (char)0xff;
    else f[0] = (char)0x80;
}

static uintmax_t tar_octal_max(size_t w) {
    /* w-1 digits */
    return ((uintmax_t)1 << (3 * (w - 1))) - 1;
}

/* one numeric field. 0, or -1 when it does not fit (and the format
 * cannot say it otherwise) -- the caller has the message */
static int tar_num(char *f, size_t w, intmax_t v, int fmt) {
    if (v >= 0 && (uintmax_t)v <= tar_octal_max(w)) { tar_octal(f, w, (uintmax_t)v); return 0; }
    if (fmt == TAR_F_GNU || fmt == TAR_F_OLDGNU) {
        /* base-256 holds w*8-1 bits */
        if (w < 8) {
            intmax_t lim = (intmax_t)1 << (8 * w - 1);
            if (v >= lim || v < -lim) return -1;
        }
        tar_base256(f, w, v);
        return 0;
    }
    return -1;
}

static void tar_checksum(unsigned char *h) {
    memset(h + TAR_H_CHKSUM, ' ', 8);
    unsigned long s = 0;
    for (int i = 0; i < TAR_BLK; i++) s += h[i];
    char b[8];
    snprintf(b, sizeof b, "%06lo", s & 0777777);
    memcpy(h + TAR_H_CHKSUM, b, 6);
    h[TAR_H_CHKSUM + 6] = 0;
    h[TAR_H_CHKSUM + 7] = ' ';
}

static void tar_put_str(unsigned char *h, int off, size_t w, const char *s) {
    size_t n = strlen(s);
    memcpy(h + off, s, n < w ? n : w);
}

/* a pax extended header under construction */
struct tar_xhdr { struct tar_sb sb; };

static int tar_pax_deleted(const char *kw) {
    for (int i = 0; i < tar_o.npax_delete; i++) {
        struct tar_mflags f = { 1, 1, 0, 1 };
        if (tar_fnm(tar_o.pax_delete[i], kw, 0, 1, 0)) return 1;
        (void)f;
    }
    return 0;
}

/* "LEN KEYWORD=VALUE\n", LEN counting itself */
static void tar_xhdr_add_raw(struct tar_xhdr *x, const char *kw, const char *val, size_t vlen) {
    size_t base = strlen(kw) + vlen + 3;
    size_t len = base, d;
    for (;;) {
        char t[32];
        d = (size_t)snprintf(t, sizeof t, "%zu", len);
        if (base + d == len) break;
        len = base + d;
    }
    tar_sb_printf(&x->sb, "%zu %s=", len, kw);
    tar_sb_addn(&x->sb, val, vlen);
    tar_sb_addc(&x->sb, '\n');
}
static void tar_xhdr_add(struct tar_xhdr *x, const char *kw, const char *val) {
    if (tar_pax_deleted(kw)) return;
    /* a keyword given with := replaces the file's own value */
    for (int i = 0; i < tar_o.npax; i++)
        if (!tar_o.pax_global[i] && !strcmp(tar_o.pax_kw[i], kw)) return;
    tar_xhdr_add_raw(x, kw, val, strlen(val));
}

/* SECONDS[.NANOS], the fraction without its trailing zeros */
static void tar_fmt_time(char *b, size_t cap, intmax_t s, long ns) {
    if (ns) snprintf(b, cap, "%jd.%09ld", s, ns);
    else { snprintf(b, cap, "%jd", s); return; }
    size_t n = strlen(b);
    while (n && b[n - 1] == '0') b[--n] = 0;
}

static void tar_xhdr_time(struct tar_xhdr *x, const char *kw, intmax_t s, long ns) {
    char b[64];
    tar_fmt_time(b, sizeof b, s, ns);
    tar_xhdr_add(x, kw, b);
}
static void tar_xhdr_num(struct tar_xhdr *x, const char *kw, uintmax_t v) {
    char b[32];
    snprintf(b, sizeof b, "%ju", v);
    tar_xhdr_add(x, kw, b);
}

/* the expansion of an exthdr.name/globexthdr.name template */
static unsigned long tar_global_seq;
static char *tar_xhdr_name(const char *tmpl, const char *path, int global) {
    struct tar_sb b = {0};
    char *dir, *base;
    const char *sl = path ? strrchr(path, '/') : 0;
    /* a trailing slash (a directory) is not where the base starts */
    char *p = path ? tar_strdup(path) : tar_strdup("");
    size_t pl = strlen(p);
    while (pl > 1 && p[pl - 1] == '/') p[--pl] = 0;
    sl = strrchr(p, '/');
    if (sl) { dir = tar_strndup(p, (size_t)(sl - p)); base = tar_strdup(sl + 1); if (!*dir) { free(dir); dir = tar_strdup("/"); } }
    else { dir = tar_strdup("."); base = tar_strdup(p); }
    for (const char *t = tmpl; *t; t++) {
        if (*t != '%' || !t[1]) { tar_sb_addc(&b, *t); continue; }
        t++;
        switch (*t) {
        case 'd': tar_sb_add(&b, dir); break;
        case 'f': tar_sb_add(&b, base); break;
        case 'p': tar_sb_printf(&b, "%ld", (long)getpid()); break;
        case 'n': tar_sb_printf(&b, "%lu", tar_global_seq); break;
        case '%': tar_sb_addc(&b, '%'); break;
        default: tar_sb_addc(&b, '%'); tar_sb_addc(&b, *t);
        }
    }
    (void)global;
    free(p); free(dir); free(base);
    return tar_sb_take(&b);
}

/* what one member's header says, for writing */
struct tar_out {
    const char *name;         /* stored name; a directory's ends in '/' */
    const char *link;         /* hard or symbolic link target */
    char type;
    mode_t mode;
    uintmax_t uid, gid;
    const char *uname, *gname;
    intmax_t mtime; long mtime_ns;
    intmax_t atime; long atime_ns;
    intmax_t ctime; long ctime_ns;
    uintmax_t size;           /* bytes of data in the archive */
    uintmax_t realsize;       /* sparse: the file's size */
    unsigned long devmaj, devmin;
    int sparse;               /* has a map */
    uintmax_t *sp_off, *sp_len; size_t sp_n;
};

/* the global pax header goes once, before the first member */
static int tar_global_written;

static void tar_write_xblock(char type, const char *hname, intmax_t hmtime,
                             const char *data, size_t len) {
    unsigned char *h = tar_w_block();
    memset(h, 0, TAR_BLK);
    tar_put_str(h, TAR_H_NAME, 100, hname);
    tar_octal((char *)h + TAR_H_MODE, 8, 0644);
    tar_octal((char *)h + TAR_H_UID, 8, 0);
    tar_octal((char *)h + TAR_H_GID, 8, 0);
    tar_octal((char *)h + TAR_H_SIZE, 12, len);
    if (hmtime < 0) hmtime = 0;
    if ((uintmax_t)hmtime > tar_octal_max(12)) hmtime = (intmax_t)tar_octal_max(12);
    tar_octal((char *)h + TAR_H_MTIME, 12, (uintmax_t)hmtime);
    h[TAR_H_TYPE] = (unsigned char)type;
    memcpy(h + TAR_H_MAGIC, "ustar\0" "00", 8);
    tar_checksum(h);
    tar_w_done();
    tar_w_data(data, len);
}

/* the sparse format version for posix archives, as MAJOR*10+MINOR (1.0 by
 * default) */
static int tar_sparse_version(void) {
    if (!tar_o.sparse_ver_set) return 10;
    return tar_o.sparse_major * 10 + tar_o.sparse_minor;
}

static void tar_write_global(void) {
    if (tar_global_written) return;
    tar_global_written = 1;
    struct tar_xhdr x = {0};
    int any = 0;
    if (tar_o.label && tar_o.cmd == TAR_C_CREATE) {
        tar_xhdr_add_raw(&x, "GNU.volume.label", tar_o.label, strlen(tar_o.label));
        any = 1;
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = tar_o.npax - 1; i >= 0; i--) {
            if (tar_o.pax_global[i] != (pass == 0)) continue;
            if (pass == 0) any = 1;
            tar_xhdr_add_raw(&x, tar_o.pax_kw[i], tar_o.pax_val[i], strlen(tar_o.pax_val[i]));
        }
    if (!any) { free(x.sb.s); return; }
    tar_global_seq++;
    char *name;
    if (tar_o.globexthdr_name) name = tar_xhdr_name(tar_o.globexthdr_name, 0, 1);
    else {
        const char *td = getenv("TMPDIR");
        if (!td || !*td) td = "/tmp";
        struct tar_sb b = {0};
        tar_sb_printf(&b, "%s/GlobalHead.%lu", td, tar_global_seq);
        name = tar_sb_take(&b);
    }
    intmax_t mt = tar_o.globexthdr_mtime_set ? tar_o.globexthdr_mtime : (intmax_t)time(0);
    tar_write_xblock('g', name, mt, x.sb.s, x.sb.n);
    free(name);
    free(x.sb.s);
}

/* a GNU long name or link: ././@LongLink, mode 0644, root/root, NUL-ended */
static void tar_write_longlink(char type, const char *s, int fmt) {
    size_t n = strlen(s) + 1;
    unsigned char *h = tar_w_block();
    memset(h, 0, TAR_BLK);
    tar_put_str(h, TAR_H_NAME, 100, "././@LongLink");
    tar_octal((char *)h + TAR_H_MODE, 8, 0644);
    tar_octal((char *)h + TAR_H_UID, 8, 0);
    tar_octal((char *)h + TAR_H_GID, 8, 0);
    tar_octal((char *)h + TAR_H_SIZE, 12, n);
    tar_octal((char *)h + TAR_H_MTIME, 12, 0);
    h[TAR_H_TYPE] = (unsigned char)type;
    memcpy(h + TAR_H_MAGIC, "ustar  \0", 8);
    tar_put_str(h, TAR_H_UNAME, 32, "root");
    tar_put_str(h, TAR_H_GNAME, 32, "root");
    (void)fmt;
    tar_checksum(h);
    tar_w_done();
    tar_w_data(s, n);
}

static int tar_out_fmt(void) {
    return tar_o.format == TAR_F_DEFAULT ? TAR_F_GNU : tar_o.format;
}

/* the ustar split: the last '/' that leaves a prefix of at most 155 and
 * a name of at most 100 (not empty); -1 when there is none */
static int tar_ustar_split(const char *name) {
    size_t n = strlen(name);
    if (n > 256) return -1;
    size_t lim = n - 1 < 155 ? n - 1 : 155;
    for (size_t i = lim + 1; i-- > 0; ) {
        if (name[i] != '/') continue;
        if (n - i - 1 == 0) continue;       /* a directory's own slash */
        if (n - i - 1 > 100) return -1;
        return (int)i;
    }
    return -1;
}

static const char *tar_type_word(int bits) {
    return bits == 8 ? "uid_t" : "gid_t";
}

/* write the header (and long names / pax header before it). 0 written,
 * -1 not (the message given) */
static int tar_write_header(struct tar_out *o, const char *msgname) {
    (void)msgname;
    tar_mv_start(o->name, o->size);
    int fmt = tar_out_fmt();
    unsigned char hb[TAR_BLK];
    memset(hb, 0, TAR_BLK);
    struct tar_xhdr x = {0};
    int needx = 0;
    size_t nl = strlen(o->name), ll = o->link ? strlen(o->link) : 0;
    char tchar = o->type;

    if (fmt == TAR_F_POSIX) tar_write_global();

    /* the names */
    int split = -1;
    switch (fmt) {
    case TAR_F_V7:
        if (nl > 99) {
            tar_err("%s: file name is too long (max 99); not dumped", tar_qc(o->name));
            return -1;
        }
        break;
    case TAR_F_USTAR:
        if (nl > 100) {
            split = tar_ustar_split(o->name);
            if (split < 0) {
                if (nl > 256) tar_err("%s: file name is too long (max 256); not dumped", tar_qc(o->name));
                else tar_err("%s: file name is too long (cannot be split); not dumped", tar_qc(o->name));
                return -1;
            }
        }
        break;
    default: break;
    }
    if ((fmt == TAR_F_V7 || fmt == TAR_F_USTAR) && o->link && ll > 100) {
        /* GNU says so, and writes the member anyway, its link cut short */
        tar_err("%s: link name is too long; not dumped", tar_qc(o->link));
    }

    /* the numbers: check before anything is written */
    intmax_t mt = o->mtime;
    uintmax_t sz = o->size;
    char uidf[8], gidf[8], sizef[12], mtimef[12], majf[8], minf[8];
    int over_uid = tar_num(uidf, 8, (intmax_t)o->uid, fmt) != 0;
    int over_gid = tar_num(gidf, 8, (intmax_t)o->gid, fmt) != 0;
    int over_size = tar_num(sizef, 12, (intmax_t)sz, fmt) != 0;
    int over_mtime = tar_num(mtimef, 12, mt, fmt) != 0;
    int isdev = tchar == '3' || tchar == '4';
    int over_maj = isdev && tar_num(majf, 8, (intmax_t)o->devmaj, fmt) != 0;
    int over_min = isdev && tar_num(minf, 8, (intmax_t)o->devmin, fmt) != 0;
    if (fmt != TAR_F_POSIX) {
        if (over_uid) { tar_err("value %ju out of uid_t range 0..%ju", o->uid, tar_octal_max(8)); return -1; }
        if (over_gid) { tar_err("value %ju out of gid_t range 0..%ju", o->gid, tar_octal_max(8)); return -1; }
        if (over_size) { tar_err("value %ju out of off_t range 0..%ju", sz, tar_octal_max(12)); return -1; }
        if (over_mtime) { tar_err("value %jd out of time_t range 0..%ju", mt, tar_octal_max(12)); return -1; }
        if (over_maj) { tar_err("value %lu out of major_t range 0..%ju", o->devmaj, tar_octal_max(8)); return -1; }
        if (over_min) { tar_err("value %lu out of minor_t range 0..%ju", o->devmin, tar_octal_max(8)); return -1; }
    }
    (void)tar_type_word;

    char *sparse_hname = 0;
    if (fmt == TAR_F_POSIX) {
        int spv = tar_sparse_version();
        if (o->sparse) {
            /* GNU's sparse keywords come first: 1.0 the real name and
             * size (the map is in the data), 0.1 the map in one keyword,
             * 0.0 an offset/numbytes pair per segment; 0.1 and 1.0 give
             * the member a GNUSparseFile.PID name of its own */
            char b[64];
            needx = 1;
            if (spv >= 10) {
                tar_xhdr_add(&x, "GNU.sparse.major", "1");
                tar_xhdr_add(&x, "GNU.sparse.minor", "0");
                tar_xhdr_add(&x, "GNU.sparse.name", o->name);
                snprintf(b, sizeof b, "%ju", o->realsize);
                tar_xhdr_add(&x, "GNU.sparse.realsize", b);
            } else {
                snprintf(b, sizeof b, "%ju", o->realsize);
                tar_xhdr_add(&x, "GNU.sparse.size", b);
                snprintf(b, sizeof b, "%zu", o->sp_n);
                tar_xhdr_add(&x, "GNU.sparse.numblocks", b);
                if (spv == 1) {
                    tar_xhdr_add(&x, "GNU.sparse.name", o->name);
                    struct tar_sb mp = {0};
                    for (size_t i = 0; i < o->sp_n; i++)
                        tar_sb_printf(&mp, "%s%ju,%ju", i ? "," : "", o->sp_off[i], o->sp_len[i]);
                    tar_xhdr_add(&x, "GNU.sparse.map", mp.s ? mp.s : "");
                    free(mp.s);
                } else {
                    for (size_t i = 0; i < o->sp_n; i++) {
                        snprintf(b, sizeof b, "%ju", o->sp_off[i]);
                        tar_xhdr_add_raw(&x, "GNU.sparse.offset", b, strlen(b));
                        snprintf(b, sizeof b, "%ju", o->sp_len[i]);
                        tar_xhdr_add_raw(&x, "GNU.sparse.numbytes", b, strlen(b));
                    }
                }
            }
            if (spv >= 1) {
                sparse_hname = tar_xhdr_name("%d/GNUSparseFile.%p/%f", o->name, 0);
                if (strlen(sparse_hname) > 100) sparse_hname[100] = 0;
            }
        }
        if (o->link && ll > 100) { tar_xhdr_add(&x, "linkpath", o->link); needx = 1; }
        if (nl > 100 && !sparse_hname) { tar_xhdr_add(&x, "path", o->name); needx = 1; }
        if (over_uid) { tar_xhdr_num(&x, "uid", o->uid); needx = 1; tar_octal(uidf, 8, 0); }
        if (over_gid) { tar_xhdr_num(&x, "gid", o->gid); needx = 1; tar_octal(gidf, 8, 0); }
        if (over_size) { tar_xhdr_num(&x, "size", sz); needx = 1; tar_octal(sizef, 12, 0); }
        if (over_mtime || o->mtime_ns) {
            if (!tar_pax_deleted("mtime")) { tar_xhdr_time(&x, "mtime", o->mtime, o->mtime_ns); needx = 1; }
            if (over_mtime) tar_octal(mtimef, 12, 0);
        }
        if (!tar_pax_deleted("atime")) { tar_xhdr_time(&x, "atime", o->atime, o->atime_ns); needx = 1; }
        if (!tar_pax_deleted("ctime")) { tar_xhdr_time(&x, "ctime", o->ctime, o->ctime_ns); needx = 1; }
        /* the header keeps 31 bytes of a name; a pax keyword only past 32 */
        if (o->uname && strlen(o->uname) > 32) { tar_xhdr_add(&x, "uname", o->uname); needx = 1; }
        if (o->gname && strlen(o->gname) > 32) { tar_xhdr_add(&x, "gname", o->gname); needx = 1; }
        if (over_maj) { tar_xhdr_num(&x, "SCHILY.devmajor", o->devmaj); needx = 1; tar_octal(majf, 8, 0); }
        if (over_min) { tar_xhdr_num(&x, "SCHILY.devminor", o->devmin); needx = 1; tar_octal(minf, 8, 0); }
        if (o->sparse) needx = 1;
        if (needx) {
            /* the := keywords, last given first */
            for (int i = tar_o.npax - 1; i >= 0; i--)
                if (!tar_o.pax_global[i])
                    tar_xhdr_add_raw(&x, tar_o.pax_kw[i], tar_o.pax_val[i], strlen(tar_o.pax_val[i]));
            char *hn = tar_xhdr_name(tar_o.exthdr_name ? tar_o.exthdr_name : "%d/PaxHeaders/%f", o->name, 0);
            intmax_t hm = tar_o.exthdr_mtime_set ? tar_o.exthdr_mtime : o->mtime;
            if (strlen(hn) > 100) hn[100] = 0;
            tar_write_xblock('x', hn, hm, x.sb.s ? x.sb.s : "", x.sb.n);
            free(hn);
        }
        free(x.sb.s);
    }

    if (fmt == TAR_F_GNU || fmt == TAR_F_OLDGNU) {
        size_t lim = fmt == TAR_F_GNU ? 100 : 99;
        if (o->link && ll > lim) tar_write_longlink('K', o->link, fmt);
        if (nl > lim) tar_write_longlink('L', o->name, fmt);
    }

    /* the header itself */
    unsigned char *h = hb;
    if (sparse_hname) {
        memcpy(h + TAR_H_NAME, sparse_hname, strlen(sparse_hname));
        free(sparse_hname);
    } else if (split >= 0) {
        memcpy(h + TAR_H_PREFIX, o->name, (size_t)split);
        memcpy(h + TAR_H_NAME, o->name + split + 1, nl - (size_t)split - 1);
    } else {
        size_t lim = fmt == TAR_F_OLDGNU && nl > 99 ? 99 : 100;
        memcpy(h + TAR_H_NAME, o->name, nl < lim ? nl : lim);
    }
    tar_octal((char *)h + TAR_H_MODE, 8, o->mode & 07777);
    memcpy(h + TAR_H_UID, uidf, 8);
    memcpy(h + TAR_H_GID, gidf, 8);
    memcpy(h + TAR_H_SIZE, sizef, 12);
    memcpy(h + TAR_H_MTIME, mtimef, 12);
    if (fmt == TAR_F_V7 && tchar == '0') tchar = 0;
    h[TAR_H_TYPE] = (unsigned char)tchar;
    if (o->link) {
        memcpy(h + TAR_H_LINK, o->link, ll < 100 ? ll : 100);
    }
    if (fmt != TAR_F_V7) {
        if (fmt == TAR_F_GNU || fmt == TAR_F_OLDGNU) memcpy(h + TAR_H_MAGIC, "ustar  \0", 8);
        else memcpy(h + TAR_H_MAGIC, "ustar\0" "00", 8);
        if (!tar_o.numeric_owner) {
            if (o->uname) tar_put_str(h, TAR_H_UNAME, 31, o->uname);
            if (o->gname) tar_put_str(h, TAR_H_GNAME, 31, o->gname);
        }
        if (isdev) {
            memcpy(h + TAR_H_DEVMAJ, majf, 8);
            memcpy(h + TAR_H_DEVMIN, minf, 8);
        }
    }
    if (tar_o.incremental && (fmt == TAR_F_GNU || fmt == TAR_F_OLDGNU)) {
        /* -g/-G: GNU keeps the access and change times in the header */
        tar_num((char *)h + TAR_H_ATIME, 12, o->atime, fmt);
        tar_num((char *)h + TAR_H_CTIME, 12, o->ctime, fmt);
    }
    if (o->sparse && (fmt == TAR_F_GNU || fmt == TAR_F_OLDGNU)) {
        /* the map's first four entries here, the rest in extension blocks */
        size_t i;
        for (i = 0; i < o->sp_n && i < 4; i++) {
            tar_num((char *)h + TAR_H_SPARSE + i * 24, 12, (intmax_t)o->sp_off[i], fmt);
            tar_num((char *)h + TAR_H_SPARSE + i * 24 + 12, 12, (intmax_t)o->sp_len[i], fmt);
        }
        if (o->sp_n > 4) h[TAR_H_ISEXTENDED] = 1;
        tar_num((char *)h + TAR_H_REALSIZE, 12, (intmax_t)o->realsize, fmt);
    }
    tar_checksum(h);
    memcpy(tar_w_block(), h, TAR_BLK);
    tar_w_done();
    if (o->sparse && (fmt == TAR_F_GNU || fmt == TAR_F_OLDGNU) && o->sp_n > 4) {
        size_t i = 4;
        while (i < o->sp_n) {
            unsigned char *e = tar_w_block();
            memset(e, 0, TAR_BLK);
            size_t k;
            for (k = 0; k < 21 && i < o->sp_n; k++, i++) {
                tar_num((char *)e + k * 24, 12, (intmax_t)o->sp_off[i], fmt);
                tar_num((char *)e + k * 24 + 12, 12, (intmax_t)o->sp_len[i], fmt);
            }
            if (i < o->sp_n) e[504] = 1;
            tar_w_done();
        }
    }
    return 0;
}

/* ---- member names: what is safe to store or extract ---------------------------
 *
 * Unless -P, a name loses its leading slashes and everything up to its
 * last ".." component ("d/../a" is stored as "a"; "d/e/.." as "."), with
 * "Removing leading `PREFIX' from member names" (or "hard link targets")
 * said once for each different prefix. */

static char **tar_rm_seen[2]; static int tar_rm_n[2];

static void tar_note_removed(const char *prefix, size_t n, int link) {
    for (int i = 0; i < tar_rm_n[link]; i++)
        if (strlen(tar_rm_seen[link][i]) == n && !strncmp(tar_rm_seen[link][i], prefix, n)) return;
    tar_rm_seen[link] = (char **)tar_realloc(tar_rm_seen[link], (size_t)(tar_rm_n[link] + 1) * sizeof(char *));
    tar_rm_seen[link][tar_rm_n[link]++] = tar_strndup(prefix, n);
    char *p = tar_strndup(prefix, n);
    tar_warn("Removing leading `%s' from %s", p, link ? "hard link targets" : "member names");
    free(p);
}

/* where the safe part of NAME starts */
static const char *tar_safe_start(const char *name, int link, int quiet) {
    const char *p = name;
    /* the last ".." component */
    const char *cut = 0;
    for (const char *s = name; *s; ) {
        if (s[0] == '.' && s[1] == '.' && (s[2] == '/' || s[2] == 0)) {
            cut = s[2] ? s + 3 : s + 2;
        }
        const char *sl = strchr(s, '/');
        if (!sl) break;
        s = sl + 1;
    }
    if (cut) p = cut;
    while (*p == '/') p++;
    if (p != name && !quiet) tar_note_removed(name, (size_t)(p - name), link);
    return p;
}

/* ---- creating ----------------------------------------------------------------- */

struct tar_link { dev_t dev; ino_t ino; char *name; char *path; char *target; nlink_t nlink, seen; };
static struct tar_link *tar_links; static int tar_nlinks, tar_links_cap;

static int tar_cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* the mtime --mtime and --clamp-mtime leave */
static void tar_apply_mtime(intmax_t *s, long *ns) {
    if (!tar_o.mtime_set) return;
    if (tar_o.clamp_mtime) {
        if (*s > tar_o.mtime_s || (*s == tar_o.mtime_s && *ns > tar_o.mtime_ns)) {
            *s = tar_o.mtime_s; *ns = tar_o.mtime_ns;
        }
        return;
    }
    *s = tar_o.mtime_s; *ns = tar_o.mtime_ns;
}

static void tar_stat_times(const struct stat *st, intmax_t *m, long *mn, intmax_t *a, long *an,
                           intmax_t *c, long *cn) {
    *m = (intmax_t)st->st_mtime; *a = (intmax_t)st->st_atime; *c = (intmax_t)st->st_ctime;
    *mn = st->st_mtim.tv_nsec; *an = st->st_atim.tv_nsec; *cn = st->st_ctim.tv_nsec;
}

/* the long listing of one member: "-rw-r--r-- u/g  SIZE DATE NAME" */
static int tar_ugswidth = 19;
static void tar_mode_string(char *b, mode_t mode, char type) {
    char t;
    switch (type) {
    case '5': t = 'd'; break;
    case '2': t = 'l'; break;
    case '1': t = 'h'; break;
    case '3': t = 'c'; break;
    case '4': t = 'b'; break;
    case '6': t = 'p'; break;
    case '7': t = 'C'; break;
    case 'D': t = 'd'; break;
    case 'M': t = 'M'; break;
    case 'N': t = 'N'; break;
    case 'V': t = 'V'; break;
    case 'S': t = '-'; break;
    case '0': case 0: case 'x': case 'g': case 'L': case 'K': t = '-'; break;
    default: t = '?';
    }
    b[0] = t;
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) b[1 + i] = (mode & (0400 >> i)) ? rwx[i] : '-';
    if (mode & 04000) b[3] = (mode & 0100) ? 's' : 'S';
    if (mode & 02000) b[6] = (mode & 0010) ? 's' : 'S';
    if (mode & 01000) b[9] = (mode & 0001) ? 't' : 'T';
    b[10] = 0;
}

static void tar_listing_time(char *b, size_t cap, intmax_t s, long ns) {
    struct tm tm;
    if (!tar_localtime(s, &tm)) { snprintf(b, cap, "%jd", s); return; }
    if (tar_o.full_time) {
        size_t n = strftime(b, cap, "%Y-%m-%d %H:%M:%S", &tm);
        if (ns) {
            snprintf(b + n, cap - n, ".%09ld", ns);
            size_t k = strlen(b);
            while (b[k - 1] == '0') b[--k] = 0;
        }
    } else strftime(b, cap, "%Y-%m-%d %H:%M", &tm);
}

/* one -vv style line: the member described by the fields given */
static void tar_print_long(FILE *out, char type, mode_t mode, const char *uname, const char *gname,
                           uintmax_t uid, uintmax_t gid, uintmax_t size, unsigned long maj,
                           unsigned long min, intmax_t mt, long mtns, const char *name,
                           const char *link, int numeric) {
    char ms[16], ug[160], sz[64], tb[64];
    tar_mode_string(ms, mode, type);
    char ub[32], gb[32];
    const char *u = uname, *g = gname;
    if (numeric || !u || !*u) { snprintf(ub, sizeof ub, "%ju", uid); u = ub; }
    if (numeric || !g || !*g) { snprintf(gb, sizeof gb, "%ju", gid); g = gb; }
    snprintf(ug, sizeof ug, "%s/%s", u, g);
    if (type == '3' || type == '4') snprintf(sz, sizeof sz, "%lu,%lu", maj, min);
    else snprintf(sz, sizeof sz, "%ju", size);
    int pad = tar_ugswidth - (int)strlen(ug) - (int)strlen(sz) - 1;
    if (pad < 0) { tar_ugswidth += -pad; pad = 0; }
    tar_listing_time(tb, sizeof tb, mt, mtns);
    fprintf(out, "%s %s %*s%s %s %s", ms, ug, pad, "", sz, tb, tar_qname(name));
    if (link) {
        if (type == '2') fprintf(out, " -> %s", tar_qname(link));
        else if (type == '1') fprintf(out, " link to %s", tar_qname(link));
    }
}

/* the file NAME is the archive being written */
static int tar_is_archive(const struct stat *st) {
    return tar_ar.have_id && st->st_ino == tar_ar.ino && st->st_dev == tar_ar.dev &&
           !tar_ar.compressed_by_us;
}

static void tar_remove_after(const char *path, int isdir) {
    if (!tar_o.remove_files) return;
    if (isdir) {
        if (rmdir(path) != 0 && errno != ENOENT) {
            int e = errno;
            tar_errno(path, "Cannot rmdir", e);
        }
    } else if (unlink(path) != 0) {
        int e = errno;
        tar_errno(path, "Cannot unlink", e);
    }
}

static dev_t tar_top_dev;
static int tar_update_mode;
static intmax_t tar_newest_copy(const char *name, int *found);

/* the data of a regular file: SIZE bytes, padded to a block; "file
 * shrank" and "file changed" as GNU says them */
static int tar_dump_data(const char *path, int fd, uintmax_t size, const struct stat *st0) {
    uintmax_t left = size;
    int status = 0;
    while (left >= 2 * TAR_BLK && fd >= 0) {
        /* whole blocks, as many as the record has room for, in one read */
        tar_w_block();
        size_t room = tar_ar.recsize - tar_ar.pos;
        size_t want = left - left % TAR_BLK;
        if (want > room) want = room;
        unsigned char *b = tar_ar.rec + tar_ar.pos;
        ssize_t r = tar_full_read(fd, b, want);
        if (r != (ssize_t)want) {
            /* short: the block by block path says what happened */
            if (r < 0) r = 0;
            size_t whole = (size_t)r / TAR_BLK;
            for (size_t i = 0; i < whole; i++) { tar_w_done(); tar_mv_data_done(TAR_BLK); }
            left -= whole * TAR_BLK;
            if ((size_t)r % TAR_BLK) {
                /* the part block read: it goes back for the slow path */
                lseek(fd, -(off_t)((size_t)r % TAR_BLK), SEEK_CUR);
            }
            break;
        }
        for (size_t i = 0; i < want / TAR_BLK; i++) {
            tar_ar.pos += TAR_BLK;
            tar_ar.block++;
            tar_mv_blocks++;
            tar_mv_data_done(TAR_BLK);
        }
        if (tar_ar.pos >= tar_ar.recsize) tar_flush_record();
        left -= want;
    }
    while (left) {
        unsigned char *b = tar_w_block();
        size_t want = left < TAR_BLK ? (size_t)left : TAR_BLK;
        ssize_t r = fd >= 0 ? tar_full_read(fd, b, want) : 0;
        if (r < 0) {
            int e = errno;
            tar_errno(path, "Read error", e);
            r = 0;
            status = 2;
        }
        if ((size_t)r < want) {
            /* the file is shorter now: zeros for the rest */
            memset(b + r, 0, TAR_BLK - (size_t)r);
            uintmax_t missing = left - (uintmax_t)r;
            if (!status) {
                tar_err("%s: File shrank by %ju bytes; padding with zeros", tar_qc(path), missing);
            }
            tar_w_done();
            left -= want;
            while (left) {
                unsigned char *z = tar_w_block();
                memset(z, 0, TAR_BLK);
                size_t k = left < TAR_BLK ? (size_t)left : TAR_BLK;
                tar_w_done();
                left -= k;
            }
            return 1;
        }
        if (want < TAR_BLK) memset(b + want, 0, TAR_BLK - want);
        tar_w_done();
        tar_mv_data_done(want);
        left -= want;
    }
    struct stat st1;
    if (fd >= 0 && fstat(fd, &st1) == 0) {
        if (st1.st_size != st0->st_size || st1.st_mtime != st0->st_mtime
            || st1.st_ctim.tv_sec != st0->st_ctim.tv_sec || st1.st_ctim.tv_nsec != st0->st_ctim.tv_nsec) {
            if (tar_warning_on(TW_FILE_CHANGED))
                tar_warn("%s: file changed as we read it", tar_qc(path));
            if (tar_status < TAR_EXIT_DIFFER) tar_status = TAR_EXIT_DIFFER;
        }
    }
    return 0;
}

static void tar_dump_path(const char *path, int top, int recursion, dev_t parent_dev);

/* the verbose line for a member being added */
static void tar_verbose_create(const char *disk_name, struct tar_out *o) {
    if (!tar_o.verbose) return;
    FILE *out = tar_o.vout;
    const char *shown = tar_o.show_transformed ? o->name : disk_name;
    char *dn = 0;
    if (!tar_o.show_transformed && o->type == '5') {
        size_t n = strlen(disk_name);
        if (!n || disk_name[n - 1] != '/') {
            dn = (char *)tar_alloc(n + 2);
            memcpy(dn, disk_name, n); dn[n] = '/'; dn[n + 1] = 0;
            shown = dn;
        }
    }
    if (tar_o.block_number) fprintf(out, "block %ju: ", tar_cur_block);
    if (tar_o.verbose > 1) {
        tar_print_long(out, o->type, o->mode, o->uname, o->gname, o->uid, o->gid,
                       o->type == '5' ? 0 : o->sparse ? o->realsize : o->size,
                       o->devmaj, o->devmin, o->mtime, o->mtime_ns, shown, o->link,
                       tar_o.numeric_owner);
    } else {
        fputs(tar_qname(shown), out);
    }
    fputc('\n', out);
    fflush(out);
    free(dn);
}

/* --owner-map, --group-map: lines "OLD NEW", OLD a name or +ID, NEW a
 * NAME, NAME:ID or +ID; '#' comments and empty lines pass. A line that
 * is not that is "FILE:LINE: malformed line"; a name with no id "can't
 * obtain UID of NAME"; any of them ends the run ("errors reading map
 * file"). */
struct tar_map { uintmax_t old; uintmax_t id; char *name; };
static struct tar_map *tar_umap, *tar_gmap; static int tar_numap, tar_ngmap;

static int tar_map_id(const char *s, int group, uintmax_t *id, const char *file, int line) {
    const char *p = s;
    if (*p == '+') p++;
    if (!*p) goto bad;
    for (const char *q = p; *q; q++) if (!isdigit((unsigned char)*q)) goto bad;
    *id = tar_strtoumax(p, 0, 10);
    return 0;
bad:
    tar_warn("%s:%d: invalid %s: %s", tar_qc(file), line, group ? "GID" : "UID", *s == '+' ? s + 1 : s);
    return -1;
}

static void tar_read_map(const char *file, int group, struct tar_map **out, int *n) {
    int fd = su_open_read(file, 0);
    if (fd < 0) {
        int e = errno;
        tar_fatal("%s: Cannot open: %s", tar_qc(file), strerror(e));
    }
    size_t len;
    char *d = tar_slurp_fd(fd, &len);
    close(fd);
    if (!d) return;
    int errs = 0, line = 0, cap = 0;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && d[j] != '\n') j++;
        char *l = tar_strndup(d + i, j - i);
        i = j + 1;
        line++;
        char *f[3] = { 0, 0, 0 };
        int nf = 0;
        char *save = l;
        while (*save && nf < 3) {
            while (*save == ' ' || *save == '\t') save++;
            if (!*save || *save == '#') break;
            f[nf++] = save;
            while (*save && *save != ' ' && *save != '\t') save++;
            if (*save) *save++ = 0;
        }
        if (nf == 0) { free(l); continue; }
        if (nf != 2) { tar_warn("%s:%d: malformed line", tar_qc(file), line); errs++; free(l); continue; }
        uintmax_t old, id = 0;
        if (f[0][0] == '+') {
            if (tar_map_id(f[0], group, &old, file, line)) { errs++; free(l); continue; }
        } else if (!(group ? tar_group_gid(f[0], &old) : tar_user_uid(f[0], &old))) {
            tar_warn("%s:%d: can't obtain %s of %s", tar_qc(file), line, group ? "GID" : "UID", f[0]);
            errs++; free(l); continue;
        }
        char *nm = 0;
        char *colon = strchr(f[1], ':');
        if (f[1][0] == '+') {
            if (tar_map_id(f[1], group, &id, file, line)) { errs++; free(l); continue; }
            nm = tar_strdup("");
        } else if (colon) {
            *colon = 0;
            if (tar_map_id(colon + 1, group, &id, file, line)) { errs++; free(l); continue; }
            nm = tar_strdup(f[1]);
        } else {
            if (!(group ? tar_group_gid(f[1], &id) : tar_user_uid(f[1], &id))) {
                tar_warn("%s:%d: can't obtain %s of %s", tar_qc(file), line, group ? "GID" : "UID", f[1]);
                errs++; free(l); continue;
            }
            nm = tar_strdup(f[1]);
        }
        tar_grow((void **)out, &cap, *n, sizeof **out);
        (*out)[*n].old = old; (*out)[*n].id = id; (*out)[*n].name = nm;
        (*n)++;
        free(l);
    }
    free(d);
    if (errs) tar_fatal("errors reading map file");
}

/* fill in the header fields from ST */
static void tar_out_from_stat(struct tar_out *o, const struct stat *st, char *ubuf, char *gbuf) {
    memset(o, 0, sizeof *o);
    o->mode = st->st_mode & 07777;
    if (tar_o.mode_set)
        o->mode = su_mode_adjust(o->mode, S_ISDIR(st->st_mode), 0, &tar_o.mode);
    o->uid = (uintmax_t)st->st_uid;
    o->gid = (uintmax_t)st->st_gid;
    if (tar_o.owner_set == 2) o->uid = tar_o.owner_uid;
    if (tar_o.group_set == 2) o->gid = tar_o.group_gid;
    if (tar_o.owner_set && tar_o.owner_name) o->uname = tar_o.owner_name;
    else {
        snprintf(ubuf, 64, "%s", tar_cached_name(o->uid, 0));
        o->uname = ubuf;
    }
    if (tar_o.group_set && tar_o.group_name) o->gname = tar_o.group_name;
    else {
        snprintf(gbuf, 64, "%s", tar_cached_name(o->gid, 1));
        o->gname = gbuf;
    }
    for (int i = 0; i < tar_numap; i++)
        if (tar_umap[i].old == (uintmax_t)st->st_uid) {
            /* the map has the last word, over --owner */
            o->uid = tar_umap[i].id;
            o->uname = tar_umap[i].name;
            break;
        }
    for (int i = 0; i < tar_ngmap; i++)
        if (tar_gmap[i].old == (uintmax_t)st->st_gid) {
            o->gid = tar_gmap[i].id;
            o->gname = tar_gmap[i].name;
            break;
        }
    tar_stat_times(st, &o->mtime, &o->mtime_ns, &o->atime, &o->atime_ns, &o->ctime, &o->ctime_ns);
    tar_apply_mtime(&o->mtime, &o->mtime_ns);
}

/* a link entry's target: the first name, made safe as a hard link
 * target (GNU says "Removing leading ... from hard link targets" when it
 * first works this out: when a file with more than one link is added,
 * or when it is met again) */
static const char *tar_link_target(struct tar_link *l) {
    if (!l->target) {
        const char *safe = tar_o.absolute ? l->path : tar_safe_start(l->path, 1, 0);
        l->target = tar_nxfs ? tar_transform(*safe ? safe : ".", TAR_XF_HARD) : tar_strdup(*safe ? safe : ".");
    }
    return l->target;
}

/* the hard-link entry of ST, or 0 */
static struct tar_link *tar_link_find(const struct stat *st) {
    if (!st->st_ino) return 0;
    for (int i = 0; i < tar_nlinks; i++)
        if (tar_links[i].ino == st->st_ino && tar_links[i].dev == st->st_dev) return &tar_links[i];
    return 0;
}
static struct tar_link *tar_link_add(const struct stat *st, const char *name, const char *path) {
    if (!st->st_ino) return 0;
    tar_grow((void **)&tar_links, &tar_links_cap, tar_nlinks, sizeof *tar_links);
    struct tar_link *l = &tar_links[tar_nlinks++];
    l->dev = st->st_dev; l->ino = st->st_ino;
    l->name = tar_strdup(name);
    l->path = tar_strdup(path);
    l->target = 0;
    l->nlink = st->st_nlink; l->seen = 1;
    return l;
}

/* the archive is /dev/null: GNU then reads no file's data */
static int tar_to_dev_null;

/* --exclude-caches*, --exclude-tag*: does the directory hold a tag? The
 * kind (1 contents, 2 under, 3 all) and the tag's name. CACHEDIR.TAG
 * counts only with its signature line. */
static int tar_dir_tagged(const char *path, char **ents, int ne, const char **tag) {
    for (int t = 0; t < tar_o.nexcl_tag; t++)
        for (int i = 0; i < ne; i++)
            if (!strcmp(ents[i], tar_o.excl_tag[t])) { *tag = tar_o.excl_tag[t]; return tar_o.excl_tag_kind[t]; }
    if (tar_o.exclude_caches) {
        for (int i = 0; i < ne; i++) {
            if (strcmp(ents[i], "CACHEDIR.TAG")) continue;
            char *f = (char *)tar_alloc(strlen(path) + 16);
            sprintf(f, "%s/CACHEDIR.TAG", path);
            int fd = open(f, O_RDONLY);
            free(f);
            if (fd < 0) continue;
            static const char sig[] = "Signature: 8a477f597d28d172789f06886806bc55";
            char b[sizeof sig];
            ssize_t r = tar_full_read(fd, b, sizeof sig - 1);
            close(fd);
            if (r == (ssize_t)(sizeof sig - 1) && !memcmp(b, sig, sizeof sig - 1)) {
                *tag = "CACHEDIR.TAG";
                return tar_o.exclude_caches;
            }
        }
    }
    return 0;
}

/* --exclude-ignore*, --exclude-vcs-ignores: patterns read from a file in
 * each directory, for its entries and those below it (matched against
 * an entry's own name) */
static char **tar_ign; static int tar_nign, tar_ign_cap;
static void tar_ignore_read(const char *path, const char *file) {
    char *f = (char *)tar_alloc(strlen(path) + strlen(file) + 2);
    sprintf(f, "%s/%s", path, file);
    int fd = open(f, O_RDONLY);
    free(f);
    if (fd < 0) return;
    size_t len;
    char *d = tar_slurp_fd(fd, &len);
    close(fd);
    if (!d) return;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && d[j] != '\n') j++;
        if (j > i && d[i] != '#') {
            tar_grow((void **)&tar_ign, &tar_ign_cap, tar_nign, sizeof *tar_ign);
            tar_ign[tar_nign++] = tar_strndup(d + i, j - i);
        }
        i = j + 1;
    }
    free(d);
}
static int tar_ignore_push(const char *path) {
    int before = tar_nign;
    for (int i = 0; i < tar_o.nexcl_ignore; i++) tar_ignore_read(path, tar_o.excl_ignore[i]);
    if (tar_o.exclude_vcs_ignores) {
        tar_ignore_read(path, ".cvsignore");
        tar_ignore_read(path, ".gitignore");
        tar_ignore_read(path, ".bzrignore");
        tar_ignore_read(path, ".hgignore");
    }
    return before;
}
static void tar_ignore_pop(int before) {
    while (tar_nign > before) free(tar_ign[--tar_nign]);
}
static int tar_ignored(const char *base) {
    for (int i = 0; i < tar_nign; i++)
        if (tar_fnm(tar_ign[i], base, 0, 1, 0)) return 1;
    return 0;
}

/* sparse maps: the data segments of FD */
static int tar_sparse_map(int fd, off_t size, uintmax_t **offs, uintmax_t **lens, size_t *n);

/* add PATH (a name as on disk) */
static void tar_dump_path(const char *path, int top, int recursion, dev_t parent_dev) {
    struct stat st;
    int r = tar_o.dereference ? stat(path, &st) : lstat(path, &st);
    if (r != 0) {
        int e = errno;
        if (!top && e == ENOENT) {
            if (tar_warning_on(TW_FILE_REMOVED))
                tar_warn("%s: File removed before we read it", tar_qc(path));
            if (tar_status < TAR_EXIT_DIFFER) tar_status = TAR_EXIT_DIFFER;
            return;
        }
        if (tar_o.ignore_failed_read) {
            if (tar_warning_on(TW_FAILED_READ))
                tar_warn("%s: Warning: Cannot stat: %s", tar_qc(path), strerror(e));
            return;
        }
        tar_errno(path, "Cannot stat", e);
        return;
    }
    if (tar_excluded(path)) return;
    if (tar_is_archive(&st)) {
        if (tar_warning_on(TW_IGNORE_ARCHIVE))
            tar_warn("%s: archive cannot contain itself; not dumped", tar_qc(path));
        return;
    }
    if (!top && tar_o.one_fs && S_ISDIR(st.st_mode) && st.st_dev != parent_dev) {
        if (tar_warning_on(TW_XDEV))
            tar_warn("%s: file is on a different filesystem; not dumped", tar_qc(path));
        return;
    }
    /* --newer: directories always; others when newer */
    if (tar_o.newer_set && !S_ISDIR(st.st_mode)) {
        intmax_t m, a, c; long mn, an, cn;
        tar_stat_times(&st, &m, &mn, &a, &an, &c, &cn);
        int newer = m > tar_o.newer_s || (m == tar_o.newer_s && mn >= tar_o.newer_ns);
        if (!tar_o.newer_mtime_only)
            newer = newer || c > tar_o.newer_s || (c == tar_o.newer_s && cn >= tar_o.newer_ns);
        if (!newer) {
            if (tar_o.verbose && tar_warning_on(TW_FILE_UNCHANGED))
                tar_warn("%s: file is unchanged; not dumped", tar_qc(path));
            return;
        }
    }

    /* -u: only what is newer than every copy in the archive (a
     * directory that is not is still gone into) */
    int update_skip = 0;
    if (tar_update_mode) {
        const char *sn = tar_o.absolute ? path : tar_safe_start(path, 0, 1);
        int found;
        intmax_t newest = tar_newest_copy(*sn ? sn : ".", &found);
        if (found && (intmax_t)st.st_mtime <= newest) {
            if (!S_ISDIR(st.st_mode)) return;
            update_skip = 1;
        }
    }

    if (!tar_confirm("add", path)) return;

    /* hard links: a file (or symbolic link) met before; its target is
     * worked out before this member's own name */
    int linkable = (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) && !tar_o.hard_deref;
    struct tar_link *lk = linkable ? tar_link_find(&st) : 0;
    const char *ltarget = 0;
    if (lk) {
        lk->seen++;
        ltarget = tar_link_target(lk);
    }

    /* the stored name */
    const char *safe = tar_o.absolute ? path : tar_safe_start(path, 0, 0);
    char *stored0 = tar_strdup(*safe ? safe : ".");
    char *stored = tar_nxfs ? tar_transform(stored0, TAR_XF_REG) : tar_strdup(stored0);
    free(stored0);
    char ubuf[64], gbuf[64];
    struct tar_out o;
    tar_out_from_stat(&o, &st, ubuf, gbuf);

    if (lk) {
        o.type = '1';
        o.name = stored;
        o.link = ltarget;
        o.size = 0;
        tar_cur_block = tar_ar.block;
        if (tar_write_header(&o, path) == 0) tar_verbose_create(path, &o);
        free(stored);
        tar_remove_after(path, 0);
        return;
    }
    if (linkable && (st.st_nlink > 1 || tar_multi_names)) {
        /* GNU remembers a file with links -- and, when it was given
         * more than one name, every file (one named twice is a link the
         * second time) -- working out its link name as it does */
        struct tar_link *nl = tar_link_add(&st, stored, path);
        if (nl) tar_link_target(nl);
    }

    if (S_ISREG(st.st_mode) && tar_to_dev_null) {
        /* nothing is read or written; -v still names it */
        o.type = '0';
        o.name = stored;
        o.size = (uintmax_t)st.st_size;
        tar_verbose_create(path, &o);
        free(stored);
        tar_remove_after(path, 0);
        return;
    }

    if (S_ISREG(st.st_mode)) {
        int fd = su_open_read(path, 0);
        if (fd < 0) {
            int e = errno;
            if (tar_o.ignore_failed_read) {
                if (tar_warning_on(TW_FAILED_READ))
                    tar_warn("%s: Warning: Cannot open: %s", tar_qc(path), strerror(e));
            } else tar_errno(path, "Cannot open", e);
            free(stored);
            return;
        }
        o.type = '0';
        o.name = stored;
        o.size = (uintmax_t)st.st_size;
        uintmax_t *so = 0, *sl = 0; size_t sn = 0;
        int fmt = tar_out_fmt();
        if (tar_o.sparse && st.st_size > 0 &&
            (fmt == TAR_F_GNU || fmt == TAR_F_OLDGNU || fmt == TAR_F_POSIX) &&
            tar_sparse_map(fd, st.st_size, &so, &sl, &sn) == 1) {
            uintmax_t data = 0;
            for (size_t i = 0; i < sn; i++) data += sl[i];
            if (fmt == TAR_F_POSIX) {
                /* written by the posix sparse code below */
            }
            o.sparse = 1;
            o.sp_off = so; o.sp_len = sl; o.sp_n = sn;
            o.realsize = (uintmax_t)st.st_size;
            o.size = data;
            if (fmt != TAR_F_POSIX) o.type = 'S';
        }
        tar_cur_block = tar_ar.block;
        if (o.sparse && fmt == TAR_F_POSIX && tar_sparse_version() >= 10) {
            /* GNU.sparse 1.0: the map at the start of the data, in decimal
             * lines, padded to a block; the header holds the real size */
            struct tar_sb m = {0};
            tar_sb_printf(&m, "%zu\n", sn);
            for (size_t i = 0; i < sn; i++) tar_sb_printf(&m, "%ju\n%ju\n", so[i], sl[i]);
            size_t mapsz = (m.n + TAR_BLK - 1) / TAR_BLK * TAR_BLK;
            o.size += mapsz;
            if (tar_write_header(&o, path) != 0) {
                close(fd); free(stored); free(m.s); free(so); free(sl);
                return;
            }
            tar_verbose_create(path, &o);
            tar_w_data(m.s, m.n);
            free(m.s);
        } else {
            if (tar_write_header(&o, path) != 0) {
                close(fd); free(stored); free(so); free(sl);
                return;
            }
            tar_verbose_create(path, &o);
        }
        if (o.sparse) {
            for (size_t i = 0; i < sn; i++) {
                if (lseek(fd, (off_t)so[i], SEEK_SET) < 0) break;
                tar_dump_data(path, fd, sl[i], &st);
            }
            free(so); free(sl);
        } else {
            tar_dump_data(path, fd, o.size, &st);
        }
        close(fd);
        free(stored);
        tar_remove_after(path, 0);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        /* the name ends in '/' */
        size_t n = strlen(stored);
        char *dn = (char *)tar_alloc(n + 2);
        memcpy(dn, stored, n);
        if (!n || dn[n - 1] != '/') dn[n++] = '/';
        dn[n] = 0;
        free(stored);
        o.type = '5';
        o.name = dn;
        o.size = 0;
        /* the directory is read first: one that cannot be is not
         * archived at all */
        char **ents = 0; int ne = 0, cap = 0;
        ino_t *inos = 0;
        if (recursion) {
            DIR *d = opendir(path);
            if (!d) {
                int e = errno;
                if (tar_o.ignore_failed_read) {
                    if (tar_warning_on(TW_FAILED_READ))
                        tar_warn("%s: Warning: Cannot open: %s", tar_qc(path), strerror(e));
                } else tar_errno(path, "Cannot open", e);
                free(dn);
                return;
            }
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                if (ne == cap) {
                    cap = cap ? cap * 2 : 16;
                    ents = (char **)tar_realloc(ents, (size_t)cap * sizeof *ents);
                    inos = (ino_t *)tar_realloc(inos, (size_t)cap * sizeof *inos);
                }
                inos[ne] = de->d_ino;
                ents[ne++] = tar_strdup(de->d_name);
            }
            closedir(d);
            if (tar_o.sort == TAR_SORT_NAME) qsort(ents, (size_t)ne, sizeof *ents, tar_cmp_str);
            else if (tar_o.sort == TAR_SORT_INODE) {
                /* insertion sort by inode, keeping names with them */
                for (int i = 1; i < ne; i++) {
                    ino_t ki = inos[i]; char *ks = ents[i];
                    int j = i - 1;
                    while (j >= 0 && inos[j] > ki) { inos[j + 1] = inos[j]; ents[j + 1] = ents[j]; j--; }
                    inos[j + 1] = ki; ents[j + 1] = ks;
                }
            }
        }
        /* a tag in it (--exclude-caches*, --exclude-tag*): 1 its
         * contents go, the tag stays; 2 its contents go; 3 it goes */
        int tagged = 0;
        const char *tagname = 0;
        if (recursion) tagged = tar_dir_tagged(path, ents, ne, &tagname);
        char *pslash = (char *)tar_alloc(strlen(path) + 2);
        sprintf(pslash, "%s%s", path, path[0] && path[strlen(path) - 1] == '/' ? "" : "/");
        if (tagged == 3) {
            if (tar_o.verbose && tar_warning_on(TW_CACHEDIR))
                tar_warn("%s: contains a cache directory tag %s; directory not dumped", tar_qc(pslash), tar_qc(tagname));
            for (int i = 0; i < ne; i++) free(ents[i]);
            free(ents); free(inos); free(pslash); free(dn);
            return;
        }
        tar_cur_block = tar_ar.block;
        int wrote = update_skip ? 1 : tar_write_header(&o, path) == 0;
        if (wrote && !update_skip) tar_verbose_create(path, &o);
        if (tagged && tar_o.verbose && tar_warning_on(TW_CACHEDIR))
            tar_warn("%s: contains a cache directory tag %s; contents not dumped", tar_qc(pslash), tar_qc(tagname));
        free(pslash);
        if (recursion && wrote) {
            int pushed = tar_ignore_push(path);
            size_t pl = strlen(path);
            for (int i = 0; i < ne; i++) {
                int skip = 0;
                if (tagged == 2) skip = 1;
                else if (tagged == 1) skip = strcmp(ents[i], tagname) != 0;
                if (!skip && tar_ignored(ents[i])) skip = 1;
                if (!skip) {
                    char *child = (char *)tar_alloc(pl + strlen(ents[i]) + 2);
                    if (pl && path[pl - 1] == '/') sprintf(child, "%s%s", path, ents[i]);
                    else sprintf(child, "%s/%s", path, ents[i]);
                    tar_dump_path(child, 0, recursion, st.st_dev);
                    free(child);
                }
            }
            tar_ignore_pop(pushed);
        }
        for (int i = 0; i < ne; i++) free(ents[i]);
        free(ents); free(inos);
        free(dn);
        tar_remove_after(path, 1);
        return;
    }

    if (S_ISLNK(st.st_mode)) {
        size_t cap = 256;
        char *t = 0;
        ssize_t k;
        for (;;) {
            t = (char *)tar_realloc(t, cap);
            k = readlink(path, t, cap);
            if (k < 0) {
                int e = errno;
                tar_errno(path, "Cannot readlink", e);
                free(t); free(stored);
                return;
            }
            if ((size_t)k < cap) break;
            cap *= 2;
        }
        t[k] = 0;
        char *lt = tar_nxfs ? tar_transform(t, TAR_XF_SYM) : tar_strdup(t);
        free(t);
        o.type = '2';
        o.name = stored;
        o.link = lt;
        o.size = 0;
        tar_cur_block = tar_ar.block;
        if (tar_write_header(&o, path) == 0) tar_verbose_create(path, &o);
        free(lt);
        free(stored);
        tar_remove_after(path, 0);
        return;
    }

    if (S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
        if (tar_out_fmt() == TAR_F_V7) {
            if (tar_warning_on(TW_FILE_IGNORED))
                tar_err("%s: Unknown file type; file ignored", tar_qc(path));
            else tar_status = TAR_EXIT_FAILURE;
            free(stored);
            return;
        }
        o.type = S_ISFIFO(st.st_mode) ? '6' : S_ISCHR(st.st_mode) ? '3' : '4';
        o.name = stored;
        o.size = 0;
        if (!S_ISFIFO(st.st_mode)) {
            o.devmaj = (unsigned long)major(st.st_rdev);
            o.devmin = (unsigned long)minor(st.st_rdev);
        }
        tar_cur_block = tar_ar.block;
        if (tar_write_header(&o, path) == 0) tar_verbose_create(path, &o);
        free(stored);
        tar_remove_after(path, 0);
        return;
    }

    if (S_ISSOCK(st.st_mode)) {
        if (tar_warning_on(TW_FILE_IGNORED))
            tar_warn("%s: socket ignored", tar_qc(path));
        free(stored);
        return;
    }
    if (tar_warning_on(TW_FILE_IGNORED))
        tar_warn("%s: Unknown file type; file ignored", tar_qc(path));
    free(stored);
}

/* a name from the command line or -T: trailing slashes go */
static void tar_dump_top(const char *name, int recursion) {
    char *n = tar_strdup(name);
    size_t l = strlen(n);
    while (l > 1 && n[l - 1] == '/') n[--l] = 0;
    struct stat st;
    if (stat(n, &st) == 0) tar_top_dev = st.st_dev;
    tar_dump_path(n, 1, recursion, tar_top_dev);
    free(n);
}

/* an 'M' header: a member continued from the last volume */
static void tar_write_mv_header(const char *name, uintmax_t size, uintmax_t offset) {
    unsigned char *h = tar_w_block();
    memset(h, 0, TAR_BLK);
    tar_put_str(h, TAR_H_NAME, 100, name);
    tar_octal((char *)h + TAR_H_SIZE, 12, size);
    h[TAR_H_TYPE] = 'M';
    tar_octal((char *)h + TAR_H_OFFSET, 12, offset);
    tar_checksum(h);
    tar_w_done();
}

static void tar_write_label_block(const char *text) {
    unsigned char *h = tar_w_block();
    memset(h, 0, TAR_BLK);
    tar_put_str(h, TAR_H_NAME, 100, text);
    intmax_t now = (intmax_t)time(0);
    tar_octal((char *)h + TAR_H_MTIME, 12, (uintmax_t)now);
    h[TAR_H_TYPE] = 'V';
    tar_checksum(h);
    tar_w_done();
}

/* the -V label: a 'V' header, GNU formats only */
static void tar_write_label(void) {
    if (!tar_o.label) return;
    char mvl[160];
    const char *text = tar_o.label;
    if (tar_o.multi_volume) {
        snprintf(mvl, sizeof mvl, "%s Volume 1", tar_o.label);
        text = mvl;
    }
    /* posix: GNU.volume.label in the global header; else a 'V' header */
    if (tar_out_fmt() == TAR_F_POSIX) tar_write_global();
    else tar_write_label_block(text);
    if (tar_o.verbose) {
        FILE *out = tar_o.vout;
        if (tar_o.verbose > 1) {
            /* GNU shows the label's time as 0 here */
            tar_print_long(out, 'V', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, text, 0, 1);
            fputs("--Volume Header--\n", out);
        } else fprintf(out, "%s\n", tar_qname(tar_o.label));
    }
}

static void tar_check_links_report(void) {
    if (!tar_o.check_links) return;
    for (int i = 0; i < tar_nlinks; i++) {
        if (tar_links[i].nlink > 1 && tar_links[i].seen < tar_links[i].nlink)
            tar_warn("Missing links to %s.", tar_q(tar_links[i].name));
    }
}


/* ---- -g, -G: incremental archives -------------------------------------------
 *
 * Measured on GNU's: a directory is a 'D' member whose data is its
 * "dumpdir" -- each entry's name after a letter, Y (in this archive), N
 * (unchanged since the last dump, not in it) or D (a directory), sorted,
 * each NUL-ended, and an empty one at the end. All the directories come
 * first (each before what is under it), then the files they hold, in
 * the same order. A directory not in the snapshot is "Directory is new";
 * the snapshot (-g FILE) is "GNU tar-1.35-2", the time of the dump, and
 * per directory its device, inode, time and name, then its dumpdir.
 * Extracting with -G/-g removes what a directory holds that its dumpdir
 * does not name ("Deleting 'NAME'" under -v); -tvv shows the dumpdir. */

struct tar_sdir { char *name; uintmax_t dev, ino; intmax_t mtime; long mtime_ns; int nfs;
                  char *dd; size_t ddlen; };
static struct tar_sdir *tar_snap, *tar_nsnapv; static int tar_nsnap, tar_snap_cap, tar_nnsnap, tar_nsnap_cap;
static intmax_t tar_snap_s; static long tar_snap_ns;
static int tar_snap_have;           /* an earlier dump to compare with */
static intmax_t tar_dump_s; static long tar_dump_ns;

static void tar_snap_load(const char *file) {
    int fd = su_open_read(file, 0);
    if (fd < 0) return;
    size_t len;
    char *d = tar_slurp_fd(fd, &len);
    close(fd);
    if (!d || !len) { free(d); return; }
    char *nl = memchr(d, '\n', len);
    if (strncmp(d, "GNU tar-", 8) != 0 || !nl) {
        /* the old formats begin with a time */
        char *e;
        errno = 0;
        strtol(d, &e, 10);
        if (e == d || (*e != '\n' && *e != ' '))
            tar_fatal("%s:1: Invalid time stamp: %s", tar_qc(file), strerror(EINVAL));
        free(d);
        return;
    }
    size_t pos = (size_t)(nl - d) + 1;
#define TAR_SNAP_FIELD(var) do { \
        if (pos >= len) tar_fatal("%s: Unexpected EOF in snapshot file", tar_qc(file)); \
        var = d + pos; pos += strnlen(d + pos, len - pos) + 1; \
        if (pos > len) tar_fatal("%s: Unexpected EOF in snapshot file", tar_qc(file)); \
    } while (0)
    char *f1, *f2;
    TAR_SNAP_FIELD(f1);
    TAR_SNAP_FIELD(f2);
    tar_snap_s = strtoll(f1, 0, 10);
    tar_snap_ns = strtol(f2, 0, 10);
    tar_snap_have = 1;
    while (pos < len) {
        char *nfs, *ms, *mns, *dv, *in, *nm;
        TAR_SNAP_FIELD(nfs);
        TAR_SNAP_FIELD(ms);
        TAR_SNAP_FIELD(mns);
        TAR_SNAP_FIELD(dv);
        TAR_SNAP_FIELD(in);
        TAR_SNAP_FIELD(nm);
        size_t dstart = pos;
        while (pos < len && d[pos]) pos += strnlen(d + pos, len - pos) + 1;
        size_t dend = pos;
        pos++;          /* the dumpdir's end */
        pos++;          /* the record's */
        tar_grow((void **)&tar_snap, &tar_snap_cap, tar_nsnap, sizeof *tar_snap);
        struct tar_sdir *sd = &tar_snap[tar_nsnap++];
        sd->nfs = atoi(nfs);
        sd->mtime = strtoll(ms, 0, 10); sd->mtime_ns = strtol(mns, 0, 10);
        sd->dev = tar_strtoumax(dv, 0, 10); sd->ino = tar_strtoumax(in, 0, 10);
        sd->name = tar_strdup(nm);
        sd->ddlen = dend - dstart;
        sd->dd = tar_strndup(d + dstart, sd->ddlen);
    }
#undef TAR_SNAP_FIELD
    free(d);
}

static void tar_snap_save(const char *file) {
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        int e = errno;
        tar_err("%s: Cannot open: %s", tar_qc(file), strerror(e));
        return;
    }
    struct tar_sb b = {0};
    tar_sb_add(&b, "GNU tar-1.35-2\n");
    tar_sb_printf(&b, "%jd", tar_dump_s); tar_sb_addc(&b, 0);
    tar_sb_printf(&b, "%ld", tar_dump_ns); tar_sb_addc(&b, 0);
    for (int i = 0; i < tar_nnsnap; i++) {
        struct tar_sdir *sd = &tar_nsnapv[i];
        tar_sb_printf(&b, "%d", sd->nfs); tar_sb_addc(&b, 0);
        tar_sb_printf(&b, "%jd", sd->mtime); tar_sb_addc(&b, 0);
        tar_sb_printf(&b, "%ld", sd->mtime_ns); tar_sb_addc(&b, 0);
        tar_sb_printf(&b, "%ju", sd->dev); tar_sb_addc(&b, 0);
        tar_sb_printf(&b, "%ju", sd->ino); tar_sb_addc(&b, 0);
        tar_sb_add(&b, sd->name); tar_sb_addc(&b, 0);
        tar_sb_addn(&b, sd->dd, sd->ddlen);
        tar_sb_addc(&b, 0);
        tar_sb_addc(&b, 0);
    }
    if (tar_full_write(fd, b.s, b.n) != (int)b.n) {
        int e = errno;
        tar_err("%s: Cannot write: %s", tar_qc(file), strerror(e));
    }
    close(fd);
    free(b.s);
}

/* one directory of the walk */
struct tar_idir { char *path; struct stat st; char **ent; char *flag; int n; };
static struct tar_idir *tar_idirs; static int tar_nidirs, tar_idirs_cap;

static int tar_incr_changed(const struct stat *st) {
    intmax_t m, a, c; long mn, an, cn;
    tar_stat_times(st, &m, &mn, &a, &an, &c, &cn);
    if (m > tar_snap_s || (m == tar_snap_s && mn >= tar_snap_ns)) return 1;
    if (c > tar_snap_s || (c == tar_snap_s && cn >= tar_snap_ns)) return 1;
    return 0;
}

/* gather PATH (a directory) and all under it, in walking order */
static void tar_incr_scan(const char *path, const struct stat *st, int parent_new) {
    int isnew = 1;
    struct tar_sdir *old = 0;
    for (int i = 0; tar_snap_have && i < tar_nsnap; i++)
        if (tar_snap[i].dev == (uintmax_t)st->st_dev && tar_snap[i].ino == (uintmax_t)st->st_ino) {
            isnew = 0;
            old = &tar_snap[i];
            break;
        }
    (void)parent_new;
    if (isnew && tar_o.verbose && tar_warning_on(TW_NEW_DIR)) tar_warn("%s: Directory is new", tar_qc(path));
    DIR *dp = opendir(path);
    if (!dp) {
        int e = errno;
        tar_errno(path, "Cannot open", e);
        return;
    }
    char **ents = 0; int ne = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (ne == cap) { cap = cap ? cap * 2 : 16; ents = (char **)tar_realloc(ents, (size_t)cap * sizeof *ents); }
        ents[ne++] = tar_strdup(de->d_name);
    }
    closedir(dp);
    qsort(ents, (size_t)ne, sizeof *ents, tar_cmp_str);
    tar_grow((void **)&tar_idirs, &tar_idirs_cap, tar_nidirs, sizeof *tar_idirs);
    int me = tar_nidirs++;
    tar_idirs[me].path = tar_strdup(path);
    tar_idirs[me].st = *st;
    tar_idirs[me].ent = ents;
    tar_idirs[me].n = ne;
    tar_idirs[me].flag = (char *)tar_zalloc((size_t)ne + 1);
    size_t pl = strlen(path);
    for (int i = 0; i < ne; i++) {
        char *child = (char *)tar_alloc(pl + strlen(ents[i]) + 2);
        if (pl && path[pl - 1] == '/') sprintf(child, "%s%s", path, ents[i]);
        else sprintf(child, "%s/%s", path, ents[i]);
        struct stat cs;
        char fl = 0;
        if (!tar_excluded(child) && lstat(child, &cs) == 0) {
            if (S_ISDIR(cs.st_mode)) {
                fl = 'D';
                tar_idirs[me].flag[i] = fl;
                tar_incr_scan(child, &cs, isnew);
            } else {
                /* an entry the last dump did not have is new */
                int known = 0;
                if (old) {
                    const char *p = old->dd, *e = old->dd + old->ddlen;
                    while (p < e && *p) {
                        size_t k = strnlen(p, (size_t)(e - p));
                        if (!strcmp(p + 1, ents[i])) { known = 1; break; }
                        p += k + 1;
                    }
                }
                fl = (isnew || !tar_snap_have || !known || tar_incr_changed(&cs)) ? 'Y' : 'N';
            }
        }
        tar_idirs[me].flag[i] = fl;
        free(child);
    }
}

/* -g/-G: a name from the command line */
static void tar_incr_top(const char *name, int recursion) {
    char *n = tar_strdup(name);
    size_t l = strlen(n);
    while (l > 1 && n[l - 1] == '/') n[--l] = 0;
    struct stat st;
    if (!recursion || lstat(n, &st) != 0 || !S_ISDIR(st.st_mode) || tar_excluded(n)) {
        tar_dump_top(n, recursion);
        free(n);
        return;
    }
    int first = tar_nidirs;
    tar_incr_scan(n, &st, 0);
    /* the directories, each with its dumpdir */
    for (int i = first; i < tar_nidirs; i++) {
        struct tar_idir *di = &tar_idirs[i];
        struct tar_sb dd = {0};
        for (int k = 0; k < di->n; k++) {
            if (!di->flag[k]) continue;
            tar_sb_addc(&dd, di->flag[k]);
            tar_sb_add(&dd, di->ent[k]);
            tar_sb_addc(&dd, 0);
        }
        tar_sb_addc(&dd, 0);
        const char *safe = tar_o.absolute ? di->path : tar_safe_start(di->path, 0, 0);
        char *stored0 = tar_strdup(*safe ? safe : ".");
        char *stored = tar_nxfs ? tar_transform(stored0, TAR_XF_REG) : tar_strdup(stored0);
        free(stored0);
        size_t sl = strlen(stored);
        char *dn = (char *)tar_alloc(sl + 2);
        memcpy(dn, stored, sl);
        if (!sl || dn[sl - 1] != '/') dn[sl++] = '/';
        dn[sl] = 0;
        free(stored);
        char ubuf[64], gbuf[64];
        struct tar_out o;
        tar_out_from_stat(&o, &di->st, ubuf, gbuf);
        o.type = 'D';
        o.name = dn;
        o.size = dd.n;
        tar_cur_block = tar_ar.block;
        if (tar_write_header(&o, di->path) == 0) {
            struct tar_out ov = o;
            ov.type = '5';
            tar_verbose_create(di->path, &ov);
            tar_w_data(dd.s, dd.n);
        }
        /* for the next snapshot */
        tar_grow((void **)&tar_nsnapv, &tar_nsnap_cap, tar_nnsnap, sizeof *tar_nsnapv);
        struct tar_sdir *sd = &tar_nsnapv[tar_nnsnap++];
        memset(sd, 0, sizeof *sd);
        sd->name = tar_strdup(di->path);
        sd->dev = (uintmax_t)di->st.st_dev; sd->ino = (uintmax_t)di->st.st_ino;
        intmax_t a, c; long an, cn;
        tar_stat_times(&di->st, &sd->mtime, &sd->mtime_ns, &a, &an, &c, &cn);
        sd->ddlen = dd.n ? dd.n - 1 : 0;
        sd->dd = tar_strndup(dd.s, sd->ddlen);
        free(dd.s);
        free(dn);
    }
    /* then the files they hold, as they were named */
    for (int i = first; i < tar_nidirs; i++) {
        struct tar_idir *di = &tar_idirs[i];
        size_t pl = strlen(di->path);
        for (int k = 0; k < di->n; k++) {
            if (di->flag[k] != 'Y') continue;
            char *child = (char *)tar_alloc(pl + strlen(di->ent[k]) + 2);
            if (pl && di->path[pl - 1] == '/') sprintf(child, "%s%s", di->path, di->ent[k]);
            else sprintf(child, "%s/%s", di->path, di->ent[k]);
            tar_dump_path(child, 0, 0, di->st.st_dev);
            free(child);
        }
    }
    free(n);
}

/* -tvv: a 'D' member's dumpdir, one entry a line, then an empty line */
static void tar_print_dumpdir(FILE *out, struct tar_member *m) {
    if (!m->dumpdir) { fputc('\n', out); return; }
    const char *p = m->dumpdir, *e = m->dumpdir + m->dumpdir_len;
    while (p < e && *p) {
        size_t k = strnlen(p, (size_t)(e - p));
        fprintf(out, "%c %s\n", p[0], tar_qname(p + 1));
        p += k + 1;
    }
    fputc('\n', out);
}

/* -x -G: what DIR holds that DUMPDIR does not name goes */
static void tar_incr_purge(const char *dir, struct tar_member *m) {
    if (!m->dumpdir) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    char **gone = 0; int ng = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        int found = 0;
        const char *p = m->dumpdir, *e = m->dumpdir + m->dumpdir_len;
        while (p < e && *p) {
            size_t k = strnlen(p, (size_t)(e - p));
            if (!strcmp(p + 1, de->d_name)) { found = 1; break; }
            p += k + 1;
        }
        if (found) continue;
        if (ng == cap) { cap = cap ? cap * 2 : 8; gone = (char **)tar_realloc(gone, (size_t)cap * sizeof *gone); }
        gone[ng++] = tar_strdup(de->d_name);
    }
    closedir(dp);
    qsort(gone, (size_t)ng, sizeof *gone, tar_cmp_str);
    for (int i = 0; i < ng; i++) {
        char *path = (char *)tar_alloc(strlen(dir) + strlen(gone[i]) + 2);
        sprintf(path, "%s/%s", dir, gone[i]);
        if (tar_o.verbose) {
            /* on the listing's stream, as GNU says it */
            fflush(stdout);
            fprintf(tar_o.vout, "%s: Deleting %s\n", su_name, tar_q(path));
            fflush(tar_o.vout);
        }
        if (tar_rm_tree(path) != 0) {
            int e = errno;
            tar_errno(path, "Cannot remove", e);
        }
        free(path);
        free(gone[i]);
    }
    free(gone);
}

/* the walk over the name list, for -c (and -r, -u after the old part) */
static void tar_dump_all(int update) {
    char *s;
    struct tar_nflags fl;
    int k;
    int start_dir = open(".", O_RDONLY | O_DIRECTORY);
    (void)update;
    while ((k = tar_next_name(&s, &fl)) >= 0) {
        if (k == TNE_CHDIR) {
            if (chdir(s) != 0) {
                int e = errno;
                tar_fatal("%s: Cannot open: %s", tar_qc(s), strerror(e));
            }
            continue;
        }
        if (tar_o.incremental) tar_incr_top(s, fl.recursion);
        else tar_dump_top(s, fl.recursion);
    }
    if (start_dir >= 0) close(start_dir);
}

/* ---- headers: reading -------------------------------------------------------------
 *
 * Any of the formats, told apart by the magic: "ustar  \0" (gnu, oldgnu:
 * the prefix area holds GNU's times and sparse map), "ustar\0" (ustar and
 * posix: the prefix is the name's leading part), none (v7: a regular file
 * may be type '\0', a directory a name ending in '/'). Long names come
 * before the member they name ('L', 'K'), pax keywords too ('x', and the
 * global 'g' for every member after it). The checksum is checked both
 * ways GNU accepts (bytes unsigned or signed). */

enum { TAR_HDR_OK, TAR_HDR_EOF, TAR_HDR_ZERO, TAR_HDR_BAD, TAR_HDR_EOF_IN };

/* a numeric field: octal (blanks around), or base-256 */
static int tar_from_num(const unsigned char *f, size_t w, intmax_t *out, int *ok) {
    *ok = 1;
    if (f[0] == 0x80 || f[0] == 0xff) {
        uintmax_t v = f[0] == 0xff ? (uintmax_t)-1 : 0;
        if (f[0] == 0x80) v = 0;
        for (size_t i = 1; i < w; i++) v = (v << 8) | f[i];
        if (f[0] == 0xff) {
            /* sign-extend from the field's width */
            intmax_t s = (intmax_t)v;
            *out = s;
        } else *out = (intmax_t)v;
        return 0;
    }
    size_t i = 0;
    while (i < w && (f[i] == ' ' || f[i] == 0)) {
        if (f[i] == 0) { /* an all-NUL field is 0 */
            size_t j = i;
            while (j < w && f[j] == 0) j++;
            if (j == w) { *out = 0; return 0; }
        }
        i++;
    }
    uintmax_t v = 0;
    int digits = 0;
    for (; i < w && f[i] >= '0' && f[i] <= '7'; i++) { v = v * 8 + (uintmax_t)(f[i] - '0'); digits++; }
    for (; i < w; i++) if (f[i] != ' ' && f[i] != 0) { *ok = 0; break; }
    if (!digits && *ok) { *out = 0; return 0; }
    *out = (intmax_t)v;
    return 0;
}

static int tar_header_sum_ok(const unsigned char *h) {
    unsigned long us = 0;
    long ss = 0;
    for (int i = 0; i < TAR_BLK; i++) {
        unsigned char c = (i >= TAR_H_CHKSUM && i < TAR_H_CHKSUM + 8) ? ' ' : h[i];
        us += c;
        ss += (signed char)c;
    }
    intmax_t stored;
    int ok;
    tar_from_num(h + TAR_H_CHKSUM, 8, &stored, &ok);
    if (!ok) return 0;
    /* an all-blank checksum field never matches */
    int any = 0;
    for (int i = 0; i < 8; i++) if (h[TAR_H_CHKSUM + i] >= '0' && h[TAR_H_CHKSUM + i] <= '7') any = 1;
    if (!any) return 0;
    return (uintmax_t)stored == us || stored == ss;
}

static int tar_block_zero(const unsigned char *b) {
    for (int i = 0; i < TAR_BLK; i++) if (b[i]) return 0;
    return 1;
}

/* the global pax keywords seen so far */
static char **tar_gkw, **tar_gval; static int tar_ngkw;

static void tar_member_free(struct tar_member *m) {
    free(m->name); free(m->link); free(m->uname); free(m->gname);
    free(m->sp_off); free(m->sp_len); free(m->dumpdir);
    memset(m, 0, sizeof *m);
}

/* read SIZE bytes of member data into a new buffer */
static char *tar_read_data_blob(uintmax_t size, int *eof) {
    char *b = (char *)tar_alloc((size_t)size + 1);
    uintmax_t got = 0;
    *eof = 0;
    while (got < size) {
        unsigned char *blk = tar_r_block();
        if (!blk) { *eof = 1; break; }
        size_t k = size - got < TAR_BLK ? (size_t)(size - got) : TAR_BLK;
        memcpy(b + got, blk, k);
        got += k;
    }
    b[got] = 0;
    return b;
}

static int tar_pax_global_apply;   /* applying a global header's keywords */
/* apply a pax record to M (or note it for -t's unknown-keyword warning) */
static void tar_pax_apply(struct tar_member *m, const char *kw, const char *val, size_t vlen,
                          int *path_set, int *link_set, int *size_set) {
    if (!strcmp(kw, "path")) { free(m->name); m->name = tar_strndup(val, vlen); *path_set = 1; }
    else if (!strcmp(kw, "linkpath")) { free(m->link); m->link = tar_strndup(val, vlen); *link_set = 1; }
    else if (!strcmp(kw, "size")) { m->size = tar_strtoumax(val, 0, 10); *size_set = 1; }
    else if (!strcmp(kw, "uid")) m->uid = tar_strtoumax(val, 0, 10);
    else if (!strcmp(kw, "gid")) m->gid = tar_strtoumax(val, 0, 10);
    else if (!strcmp(kw, "uname")) { free(m->uname); m->uname = tar_strndup(val, vlen); }
    else if (!strcmp(kw, "gname")) { free(m->gname); m->gname = tar_strndup(val, vlen); }
    else if (!strcmp(kw, "mtime") || !strcmp(kw, "atime") || !strcmp(kw, "ctime")) {
        intmax_t s; long ns;
        char *v = tar_strndup(val, vlen);
        const char *p = v;
        int neg = *p == '-';
        if (neg) p++;
        uintmax_t whole = tar_strtoumax(p, (char **)&p, 10);
        long f = 0;
        if (*p == '.') {
            p++;
            int d = 0;
            for (; isdigit((unsigned char)*p); p++) if (d < 9) { f = f * 10 + (*p - '0'); d++; }
            while (d++ < 9) f *= 10;
        }
        s = neg ? -(intmax_t)whole : (intmax_t)whole;
        ns = f;
        free(v);
        if (kw[0] == 'm') { m->mtime = s; m->mtime_ns = ns; }
        else if (kw[0] == 'a') { m->atime = s; m->atime_ns = ns; m->has_atime = 1; }
        else { m->ctime = s; m->ctime_ns = ns; m->has_ctime = 1; }
    }
    else if (!strcmp(kw, "GNU.sparse.size") || !strcmp(kw, "GNU.sparse.realsize")) {
        m->realsize = tar_strtoumax(val, 0, 10); m->is_sparse = 1;
    }
    else if (!strcmp(kw, "GNU.sparse.name")) { free(m->name); m->name = tar_strndup(val, vlen); *path_set = 1; m->is_sparse = 1; }
    else if (!strcmp(kw, "GNU.sparse.major")) { m->sp_major = atoi(val); m->is_sparse = 1; }
    else if (!strcmp(kw, "GNU.sparse.minor")) { m->sp_minor = atoi(val); m->is_sparse = 1; }
    else if (!strcmp(kw, "GNU.sparse.numblocks")) { m->is_sparse = 1; m->sp_major = 0; }
    else if (!strcmp(kw, "GNU.sparse.offset") || !strcmp(kw, "GNU.sparse.numbytes")) {
        /* 0.0: alternating records */
        m->is_sparse = 1;
        uintmax_t v = tar_strtoumax(val, 0, 10);
        if (kw[11] == 'o') {
            m->sp_off = (uintmax_t *)tar_realloc(m->sp_off, (m->sp_n + 1) * sizeof(uintmax_t));
            m->sp_len = (uintmax_t *)tar_realloc(m->sp_len, (m->sp_n + 1) * sizeof(uintmax_t));
            m->sp_off[m->sp_n] = v; m->sp_len[m->sp_n] = 0; m->sp_n++;
        } else if (m->sp_n) m->sp_len[m->sp_n - 1] = v;
    }
    else if (!strcmp(kw, "GNU.sparse.map")) {
        /* 0.1: "off,len,off,len" */
        m->is_sparse = 1;
        char *v = tar_strndup(val, vlen), *p = v;
        int odd = 0;
        while (*p) {
            uintmax_t x = tar_strtoumax(p, &p, 10);
            if (!odd) {
                m->sp_off = (uintmax_t *)tar_realloc(m->sp_off, (m->sp_n + 1) * sizeof(uintmax_t));
                m->sp_len = (uintmax_t *)tar_realloc(m->sp_len, (m->sp_n + 1) * sizeof(uintmax_t));
                m->sp_off[m->sp_n] = x; m->sp_len[m->sp_n] = 0; m->sp_n++;
            } else m->sp_len[m->sp_n - 1] = x;
            odd = !odd;
            if (*p == ',') p++; else break;
        }
        free(v);
    }
    else if (!strcmp(kw, "GNU.volume.label")) { }
    else if (!strcmp(kw, "SCHILY.devmajor")) m->devmaj = (unsigned long)tar_strtoumax(val, 0, 10);
    else if (!strcmp(kw, "SCHILY.devminor")) m->devmin = (unsigned long)tar_strtoumax(val, 0, 10);
    else if (!strncmp(kw, "SCHILY.xattr.", 13) || !strncmp(kw, "LIBARCHIVE.", 11) ||
             !strcmp(kw, "comment") || !strcmp(kw, "charset") || !strcmp(kw, "hdrcharset") ||
             !strncmp(kw, "SCHILY.", 7) || !strncmp(kw, "GNU.", 4) || !strncmp(kw, "RHT.", 4)) {
        /* known, and nothing to do here */
    }
    else if (tar_warning_on(TW_UNKNOWN_KEYWORD) && !tar_pax_global_apply) {
        tar_warn("Ignoring unknown extended header keyword '%s'", kw);
    }
}

/* parse pax records in DATA into callback-applied keywords */
static int tar_pax_parse(const char *data, size_t len, struct tar_member *m, int global,
                         int *path_set, int *link_set, int *size_set) {
    size_t p = 0;
    while (p < len) {
        size_t q = p, n = 0;
        while (q < len && isdigit((unsigned char)data[q])) { n = n * 10 + (size_t)(data[q] - '0'); q++; }
        if (q >= len || data[q] != ' ' || n == 0 || p + n > len || data[p + n - 1] != '\n') {
            tar_err("Malformed extended header: missing length");
            return -1;
        }
        q++;
        size_t end = p + n - 1;
        size_t eq = q;
        while (eq < end && data[eq] != '=') eq++;
        if (eq >= end) { tar_err("Malformed extended header: missing equal sign"); return -1; }
        char *kw = tar_strndup(data + q, eq - q);
        const char *val = data + eq + 1;
        size_t vlen = end - eq - 1;
        if (global) {
            tar_gkw = (char **)tar_realloc(tar_gkw, (size_t)(tar_ngkw + 1) * sizeof(char *));
            tar_gval = (char **)tar_realloc(tar_gval, (size_t)(tar_ngkw + 1) * sizeof(char *));
            tar_gkw[tar_ngkw] = kw;
            tar_gval[tar_ngkw++] = tar_strndup(val, vlen);
        } else {
            tar_pax_apply(m, kw, val, vlen, path_set, link_set, size_set);
            free(kw);
        }
        p += n;
    }
    return 0;
}

/* the type a v7 or ustar header means */
static char tar_norm_type(const unsigned char *h, const char *name) {
    char t = (char)h[TAR_H_TYPE];
    if (t == 0 || t == '0') {
        size_t n = strlen(name);
        return n && name[n - 1] == '/' ? '5' : '0';
    }
    return t;
}

static uintmax_t tar_member_start_block;

/* read the next member's header (with its long names and pax headers).
 * TAR_HDR_OK with M filled; or the end, or a bad block. */
static int tar_read_header(struct tar_member *m, int *eof_after_zero) {
    char *longname = 0, *longlink = 0;
    struct tar_member xm;
    memset(&xm, 0, sizeof xm);
    int have_x = 0, path_set = 0, link_set = 0, size_set = 0;
    memset(m, 0, sizeof *m);
    tar_member_start_block = tar_ar.block;
    m->xhdr_block = tar_ar.block;
    for (;;) {
        unsigned char *h = tar_r_block();
        if (!h) { free(longname); free(longlink); tar_member_free(&xm); return TAR_HDR_EOF; }
        if (tar_block_zero(h)) {
            free(longname); free(longlink); tar_member_free(&xm);
            (void)eof_after_zero;
            return TAR_HDR_ZERO;
        }
        if (!tar_header_sum_ok(h)) {
            free(longname); free(longlink); tar_member_free(&xm);
            return TAR_HDR_BAD;
        }
        m->hdr_block = tar_ar.block - 1;
        memcpy(m->hdr, h, TAR_BLK);
        char type = (char)h[TAR_H_TYPE];
        intmax_t v; int ok;
        tar_from_num(h + TAR_H_SIZE, 12, &v, &ok);
        uintmax_t size = v < 0 ? 0 : (uintmax_t)v;
        if (type == 'L' || type == 'K' || type == 'x' || type == 'g' || type == 'X') {
            int eof;
            intmax_t hmt; int hok;
            tar_from_num(h + TAR_H_MTIME, 12, &hmt, &hok);
            uintmax_t hblk = tar_ar.block - 1;
            char *data = tar_read_data_blob(size, &eof);
            if (eof) {
                free(data); free(longname); free(longlink); tar_member_free(&xm);
                return TAR_HDR_EOF_IN;
            }
            if (type == 'L') { free(longname); longname = data; }
            else if (type == 'K') { free(longlink); longlink = data; }
            else if (type == 'x' || type == 'X') {
                have_x = 1;
                tar_pax_parse(data, (size_t)size, &xm, 0, &path_set, &link_set, &size_set);
                free(data);
            } else {
                int before = tar_ngkw;
                tar_pax_parse(data, (size_t)size, &xm, 1, &path_set, &link_set, &size_set);
                free(data);
                for (int i = before; i < tar_ngkw; i++)
                    if (!strcmp(tar_gkw[i], "GNU.volume.label") && tar_ngkw == before + 1) {
                        /* (listed only when it is all the header says) */
                        /* a posix label: shown as GNU's 'V' would be */
                        memset(m, 0, sizeof *m);
                        m->type = 'V';
                        m->name = tar_strdup(tar_gval[i]);
                        m->link = tar_strdup("");
                        m->uname = tar_strdup("");
                        m->gname = tar_strdup("");
                        m->mtime = hmt;
                        (void)hok;
                        m->hdr_block = hblk;
                        m->xhdr_block = m->hdr_block;
                        free(longname); free(longlink); tar_member_free(&xm);
                        return TAR_HDR_OK;
                    }
            }
            continue;
        }
        break;
    }
    const unsigned char *h = (const unsigned char *)m->hdr;
    /* numbers that are not: "Archive contains 'X' where numeric off_t
     * value expected", and the header is a bad one */
    {
        static const struct { int off, w; const char *t; } nf[] = {
            { TAR_H_MODE, 8, "mode_t" }, { TAR_H_UID, 8, "uid_t" }, { TAR_H_GID, 8, "gid_t" },
            { TAR_H_SIZE, 12, "off_t" }, { TAR_H_MTIME, 12, "time_t" }, { 0, 0, 0 }
        };
        for (int i = 0; nf[i].t; i++) {
            intmax_t v; int ok;
            tar_from_num(h + nf[i].off, (size_t)nf[i].w, &v, &ok);
            if (!ok) {
                char b[16];
                size_t k = strnlen((const char *)h + nf[i].off, (size_t)nf[i].w);
                memcpy(b, h + nf[i].off, k); b[k] = 0;
                tar_err("Archive contains %s where numeric %s value expected", tar_q(b), nf[i].t);
                free(longname); free(longlink); tar_member_free(&xm);
                return TAR_HDR_BAD;
            }
        }
    }
    /* the format */
    if (!memcmp(h + TAR_H_MAGIC, "ustar  \0", 8)) m->fmt = TAR_F_GNU;
    else if (!memcmp(h + TAR_H_MAGIC, "ustar\0", 6)) m->fmt = have_x ? TAR_F_POSIX : TAR_F_USTAR;
    else m->fmt = TAR_F_V7;
    /* the name */
    char nb[257];
    if (m->fmt == TAR_F_USTAR || m->fmt == TAR_F_POSIX) {
        size_t pl = strnlen((const char *)h + TAR_H_PREFIX, 155);
        size_t nl = strnlen((const char *)h + TAR_H_NAME, 100);
        if (pl) {
            memcpy(nb, h + TAR_H_PREFIX, pl);
            nb[pl] = '/';
            memcpy(nb + pl + 1, h + TAR_H_NAME, nl);
            nb[pl + 1 + nl] = 0;
        } else {
            memcpy(nb, h + TAR_H_NAME, nl);
            nb[nl] = 0;
        }
    } else {
        size_t nl = strnlen((const char *)h + TAR_H_NAME, 100);
        memcpy(nb, h + TAR_H_NAME, nl);
        nb[nl] = 0;
    }
    m->name = longname ? longname : tar_strdup(nb);
    if (longname) longname = 0;
    char lb[101];
    size_t ll = strnlen((const char *)h + TAR_H_LINK, 100);
    memcpy(lb, h + TAR_H_LINK, ll); lb[ll] = 0;
    m->link = longlink ? longlink : tar_strdup(lb);
    longlink = 0;
    m->type = tar_norm_type(h, m->name);
    intmax_t v; int ok;
    tar_from_num(h + TAR_H_MODE, 8, &v, &ok); m->mode = (mode_t)(v & 07777);
    tar_from_num(h + TAR_H_UID, 8, &v, &ok); m->uid = (uintmax_t)v;
    tar_from_num(h + TAR_H_GID, 8, &v, &ok); m->gid = (uintmax_t)v;
    tar_from_num(h + TAR_H_SIZE, 12, &v, &ok); m->size = v < 0 ? 0 : (uintmax_t)v;
    tar_from_num(h + TAR_H_MTIME, 12, &v, &ok); m->mtime = v;
    if (m->fmt != TAR_F_V7) {
        char ub[33], gb[33];
        size_t k = strnlen((const char *)h + TAR_H_UNAME, 32);
        memcpy(ub, h + TAR_H_UNAME, k); ub[k] = 0;
        k = strnlen((const char *)h + TAR_H_GNAME, 32);
        memcpy(gb, h + TAR_H_GNAME, k); gb[k] = 0;
        m->uname = tar_strdup(ub);
        m->gname = tar_strdup(gb);
        tar_from_num(h + TAR_H_DEVMAJ, 8, &v, &ok); m->devmaj = (unsigned long)v;
        tar_from_num(h + TAR_H_DEVMIN, 8, &v, &ok); m->devmin = (unsigned long)v;
    } else {
        m->uname = tar_strdup("");
        m->gname = tar_strdup("");
    }
    m->realsize = m->size;
    if (m->fmt == TAR_F_GNU) {
        /* oldgnu/gnu: times and the sparse map */
        tar_from_num(h + TAR_H_ATIME, 12, &v, &ok);
        if (v) { m->atime = v; m->has_atime = 1; }
        tar_from_num(h + TAR_H_CTIME, 12, &v, &ok);
        if (v) { m->ctime = v; m->has_ctime = 1; }
        if (m->type == 'S') {
            m->is_sparse = 1;
            for (int i = 0; i < 4; i++) {
                intmax_t o, l;
                tar_from_num(h + TAR_H_SPARSE + i * 24, 12, &o, &ok);
                tar_from_num(h + TAR_H_SPARSE + i * 24 + 12, 12, &l, &ok);
                if (!o && !l && !h[TAR_H_SPARSE + i * 24]) break;
                m->sp_off = (uintmax_t *)tar_realloc(m->sp_off, (m->sp_n + 1) * sizeof(uintmax_t));
                m->sp_len = (uintmax_t *)tar_realloc(m->sp_len, (m->sp_n + 1) * sizeof(uintmax_t));
                m->sp_off[m->sp_n] = (uintmax_t)o; m->sp_len[m->sp_n] = (uintmax_t)l; m->sp_n++;
            }
            tar_from_num(h + TAR_H_REALSIZE, 12, &v, &ok);
            m->realsize = (uintmax_t)v;
            int ext = h[TAR_H_ISEXTENDED];
            while (ext) {
                unsigned char *e = tar_r_block();
                if (!e) return TAR_HDR_EOF_IN;
                for (int i = 0; i < 21; i++) {
                    intmax_t o, l;
                    if (!e[i * 24]) break;
                    tar_from_num(e + i * 24, 12, &o, &ok);
                    tar_from_num(e + i * 24 + 12, 12, &l, &ok);
                    m->sp_off = (uintmax_t *)tar_realloc(m->sp_off, (m->sp_n + 1) * sizeof(uintmax_t));
                    m->sp_len = (uintmax_t *)tar_realloc(m->sp_len, (m->sp_n + 1) * sizeof(uintmax_t));
                    m->sp_off[m->sp_n] = (uintmax_t)o; m->sp_len[m->sp_n] = (uintmax_t)l; m->sp_n++;
                }
                ext = e[504];
            }
        }
    }
    /* global keywords, then this member's */
    tar_pax_global_apply = 1;
    for (int i = 0; i < tar_ngkw; i++)
        tar_pax_apply(m, tar_gkw[i], tar_gval[i], strlen(tar_gval[i]), &path_set, &link_set, &size_set);
    tar_pax_global_apply = 0;
    if (have_x) {
        if (xm.name) { free(m->name); m->name = xm.name; xm.name = 0; }
        if (xm.link) { free(m->link); m->link = xm.link; xm.link = 0; }
        if (xm.uname) { free(m->uname); m->uname = xm.uname; xm.uname = 0; }
        if (xm.gname) { free(m->gname); m->gname = xm.gname; xm.gname = 0; }
        if (size_set) m->size = xm.size;
        /* numbers: what the extended header said, where it said it */
        const char *x = 0; (void)x;
        if (xm.uid) m->uid = xm.uid;
        if (xm.gid) m->gid = xm.gid;
        if (xm.mtime || xm.mtime_ns) { m->mtime = xm.mtime; m->mtime_ns = xm.mtime_ns; }
        if (xm.has_atime) { m->atime = xm.atime; m->atime_ns = xm.atime_ns; m->has_atime = 1; }
        if (xm.has_ctime) { m->ctime = xm.ctime; m->ctime_ns = xm.ctime_ns; m->has_ctime = 1; }
        if (xm.devmaj) m->devmaj = xm.devmaj;
        if (xm.devmin) m->devmin = xm.devmin;
        if (xm.is_sparse) {
            m->is_sparse = 1;
            m->realsize = xm.realsize;
            m->sp_major = xm.sp_major; m->sp_minor = xm.sp_minor;
            if (xm.sp_n) {
                free(m->sp_off); free(m->sp_len);
                m->sp_off = xm.sp_off; m->sp_len = xm.sp_len; m->sp_n = xm.sp_n;
                xm.sp_off = xm.sp_len = 0; xm.sp_n = 0;
            }
        }
        if (!size_set) m->realsize = m->is_sparse ? m->realsize : m->size;
    }
    tar_member_free(&xm);
    free(longlink);
    if (!*m->name) tar_warn("Substituting `.' for empty member name");
    if (tar_o.multi_volume) {
        free(tar_rd_name);
        tar_rd_name = tar_strdup(m->name);
        tar_rd_total = m->type == 'M' ? m->size : m->size;
        tar_rd_left = (m->type == '5' || m->type == '1' || m->type == '2' || m->type == 'V') ? 0 : m->size;
    }
    if (m->type == 'M') {
        intmax_t off; int ok;
        tar_from_num((const unsigned char *)m->hdr + TAR_H_OFFSET, 12, &off, &ok);
        m->mv_offset = (uintmax_t)off;
    }
    if (m->type == 'D' && m->size) {
        int eof;
        m->dumpdir = tar_read_data_blob(m->size, &eof);
        m->dumpdir_len = (size_t)m->size;
        if (eof) return TAR_HDR_EOF_IN;
        m->size = 0;
    }
    return TAR_HDR_OK;
}

/* a pax 1.0 sparse member: the map is the start of its data. Read it,
 * and leave M->size what follows it. */
static int tar_read_sparse10_map(struct tar_member *m) {
    if (!(m->is_sparse && m->sp_major == 1)) return 0;
    struct tar_sb b = {0};
    uintmax_t used = 0;
    size_t want_lines = 0, have_lines = 0;
    int first = 1;
    for (;;) {
        unsigned char *blk = tar_r_block();
        if (!blk) { free(b.s); return -1; }
        used += TAR_BLK;
        tar_sb_addn(&b, (const char *)blk, TAR_BLK);
        /* count complete lines */
        have_lines = 0;
        for (size_t i = 0; i < b.n; i++) if (b.s[i] == '\n') have_lines++;
        if (first && have_lines) {
            want_lines = 1 + 2 * (size_t)tar_strtoumax(b.s, 0, 10);
            first = 0;
        }
        if (!first && have_lines >= want_lines) break;
    }
    char *p = b.s;
    size_t n = (size_t)tar_strtoumax(p, &p, 10);
    free(m->sp_off); free(m->sp_len);
    m->sp_off = (uintmax_t *)tar_alloc((n + 1) * sizeof(uintmax_t));
    m->sp_len = (uintmax_t *)tar_alloc((n + 1) * sizeof(uintmax_t));
    m->sp_n = n;
    for (size_t i = 0; i < n; i++) {
        p++;
        m->sp_off[i] = tar_strtoumax(p, &p, 10); p++;
        m->sp_len[i] = tar_strtoumax(p, &p, 10);
    }
    free(b.s);
    m->size = m->size > used ? m->size - used : 0;
    return 0;
}

/* skip the data of a member */
static int tar_skip_data(uintmax_t size) {
    uintmax_t blocks = (size + TAR_BLK - 1) / TAR_BLK;
    uintmax_t inbuf = (tar_ar.avail - tar_ar.pos) / TAR_BLK;
    size_t per = tar_ar.recsize / TAR_BLK;
    /* a plain archive file is seeked over, whole records at a time, as
     * GNU does (fewer records read: checkpoints count what is read) */
    if (blocks > inbuf && tar_ar.regular && !tar_ar.child && !tar_ar.mem && !tar_ar.eof && !tar_o.multi_volume &&
        !tar_ar.writing && tar_ar.fd >= 0) {
        blocks -= inbuf;
        tar_ar.block += inbuf;
        tar_ar.pos = tar_ar.avail;
        uintmax_t recs = blocks / per;
        struct stat ast;
        off_t here = lseek(tar_ar.fd, 0, SEEK_CUR);
        /* not past the end: there the reading finds the archive short */
        if (recs && fstat(tar_ar.fd, &ast) == 0 && here >= 0 &&
            (uintmax_t)here + recs * tar_ar.recsize > (uintmax_t)ast.st_size)
            recs = 0;
        if (recs) {
            if (lseek(tar_ar.fd, (off_t)(recs * tar_ar.recsize), SEEK_CUR) >= 0) {
                tar_ar.passed += recs;
                tar_ar.block += recs * per;
                tar_ar.bytes += recs * tar_ar.recsize;
                blocks -= recs * per;
            }
        }
    }
    while (blocks--) if (!tar_r_block()) return -1;
    return 0;
}

/* ---- reading an archive --------------------------------------------------------- */

/* the name entry NAME matches, or -1; with no names everything matches (-2) */
static int tar_po_cur;             /* -s: the name being looked for */
static int tar_name_match1(const char *n, int i);
static int tar_name_match(const char *name) {
    if (!tar_nnames) return -2;
    if (tar_o.preserve_order) {
        /* only the current name, or (once it has matched) the next */
        char *n = tar_strdup(name);
        size_t l = strlen(n);
        while (l > 1 && n[l - 1] == '/') n[--l] = 0;
        int r = -1;
        if (tar_po_cur < tar_nnames && tar_name_match1(n, tar_po_cur)) r = tar_po_cur;
        else if (tar_po_cur < tar_nnames && tar_names[tar_po_cur].matched &&
                 tar_po_cur + 1 < tar_nnames && tar_name_match1(n, tar_po_cur + 1))
            r = ++tar_po_cur;
        free(n);
        return r;
    }
    /* the name as matched: without trailing slashes */
    char *n = tar_strdup(name);
    size_t l = strlen(n);
    while (l > 1 && n[l - 1] == '/') n[--l] = 0;
    for (int i = 0; i < tar_nnames; i++) {
        struct tar_name *nm = &tar_names[i];
        struct tar_mflags f = { nm->wildcards, nm->anchored, nm->icase, nm->wslash };
        int r;
        if (!nm->recursion) {
            /* --no-recursion: the name itself only */
            if (f.wildcards) r = tar_fnm(nm->name, n, f.icase, f.wslash, 0);
            else {
                char *p = tar_strdup(nm->name);
                size_t pl = strlen(p);
                while (pl > 1 && p[pl - 1] == '/') p[--pl] = 0;
                r = f.icase ? strcasecmp(p, n) == 0 : strcmp(p, n) == 0;
                free(p);
            }
        } else r = tar_match(nm->name, n, &f);
        if (r) {
            /* a matched literal name matches no more after --occurrence;
             * the first matching name takes the member */
            free(n);
            return i;
        }
    }
    free(n);
    return -1;
}

/* does entry I match N (a name without trailing slashes)? */
static int tar_name_match1(const char *n, int i) {
    struct tar_name *nm = &tar_names[i];
    struct tar_mflags f = { nm->wildcards, nm->anchored, nm->icase, nm->wslash };
    if (!nm->recursion) {
        if (f.wildcards) return tar_fnm(nm->name, n, f.icase, f.wslash, 0);
        char *p = tar_strdup(nm->name);
        size_t pl = strlen(p);
        while (pl > 1 && p[pl - 1] == '/') p[--pl] = 0;
        int r = f.icase ? strcasecmp(p, n) == 0 : strcmp(p, n) == 0;
        free(p);
        return r;
    }
    return tar_match(nm->name, n, &f);
}

/* NAME (the member) is selected: by the name list and the exclusions */
static int tar_started;            /* -K: the starting member has been met */
static int tar_selected(struct tar_member *m, int *which) {
    if (tar_o.starting_file && !tar_started) {
        struct tar_mflags f = { 0, 1, 0, 1 };
        char *n = tar_strdup(m->name);
        size_t l = strlen(n);
        while (l > 1 && n[l - 1] == '/') n[--l] = 0;
        if (tar_match(tar_o.starting_file, n, &f)) tar_started = 1;
        free(n);
        if (!tar_started) { *which = -1; return 0; }
    }
    int i = tar_name_match(m->name);
    *which = i;
    if (i == -1) return 0;
    if (tar_nexcls || tar_o.exclude_vcs || tar_o.exclude_backups) {
        char *n = tar_strdup(m->name);
        size_t l = strlen(n);
        while (l > 1 && n[l - 1] == '/') n[--l] = 0;
        int ex = tar_excluded(n);
        free(n);
        if (ex) return 0;
    }
    if (i >= 0) {
        struct tar_name *nm = &tar_names[i];
        nm->found_count++;
        if (tar_o.occurrence && nm->found_count != (uintmax_t)tar_o.occurrence) {
            if (nm->found_count > (uintmax_t)tar_o.occurrence) return 0;
            return 0;
        }
        nm->matched = 1;
    }
    return 1;
}

/* at the end: the names that matched nothing */
static void tar_report_unmatched(void) {
    int warned = 0;
    for (int i = 0; i < tar_nnames; i++) {
        struct tar_name *nm = &tar_names[i];
        if (nm->matched) continue;
        if (tar_o.occurrence && nm->found_count && nm->found_count < (uintmax_t)tar_o.occurrence) {
            tar_err("%s: Required occurrence not found in archive", tar_qc(nm->name));
            continue;
        }
        if (!warned && !nm->wildcards && tar_has_wild(nm->name) && !tar_o.wildcards_set) {
            tar_warn("Pattern matching characters used in file names");
            tar_warn("Use --wildcards to enable pattern matching, or --no-wildcards to suppress this warning");
            warned = 1;
        }
        tar_err("%s: Not found in archive", tar_qc(nm->name));
    }
}

/* the data size of M in the archive (what to skip) */
static uintmax_t tar_member_data(const struct tar_member *m) {
    switch (m->type) {
    case '1': case '2': case '3': case '4': case '5': case '6': case 'V':
        return m->type == '5' ? m->size : 0;
    default:
        return m->size;
    }
}

/* -t's line for M (and -x -v's) */
static int tar_bn_extract;         /* -x -R: GNU counts the header itself */
static void tar_print_member(FILE *out, struct tar_member *m, int verbose) {
    const char *name = m->name;
    char *xname = 0;
    if (tar_o.show_transformed && tar_nxfs) name = xname = tar_transform(m->name, TAR_XF_REG);
    if (tar_o.block_number) fprintf(out, "block %ju: ", tar_bn_extract ? m->hdr_block + 1 : m->xhdr_block);
    if (verbose) {
        const char *link = 0;
        char *xlink = 0;
        if (m->type == '1' || m->type == '2') {
            link = m->link;
            if (m->type == '1' && !tar_o.absolute) link = tar_safe_start(m->link, 1, 0);
            if (tar_o.show_transformed && tar_nxfs)
                link = xlink = tar_transform(link, m->type == '1' ? TAR_XF_HARD : TAR_XF_SYM);
        }
        uintmax_t size = m->is_sparse ? m->realsize : m->size;
        if (m->type == 'D') size = m->dumpdir_len;
        if (m->type == 'V') {
            char tb[64];
            tar_listing_time(tb, sizeof tb, m->mtime, m->mtime_ns);
            char ms[16];
            strcpy(ms, "V---------");
            char ug[160];
            const char *u = m->uname && *m->uname && !tar_o.numeric_owner ? m->uname : 0;
            const char *g = m->gname && *m->gname && !tar_o.numeric_owner ? m->gname : 0;
            char ub[32], gb[32];
            if (!u) { snprintf(ub, sizeof ub, "%ju", m->uid); u = ub; }
            if (!g) { snprintf(gb, sizeof gb, "%ju", m->gid); g = gb; }
            snprintf(ug, sizeof ug, "%s/%s", u, g);
            char sz[32];
            snprintf(sz, sizeof sz, "%ju", m->size);
            int pad = tar_ugswidth - (int)strlen(ug) - (int)strlen(sz) - 1;
            if (pad < 0) { tar_ugswidth -= pad; pad = 0; }
            fprintf(out, "%s %s %*s%s %s %s--Volume Header--\n", ms, ug, pad, "", sz, tb, tar_qname(name));
        } else {
            tar_print_long(out, m->type, m->mode, m->uname, m->gname, m->uid, m->gid, size,
                           m->devmaj, m->devmin, m->mtime, m->mtime_ns, name, link,
                           tar_o.numeric_owner);
            if (m->type == 'M') fprintf(out, "--Continued at byte %ju--", m->mv_offset);
            if (!strchr("01234567DKLMNSV", m->type))
                fprintf(out, " unknown file type %s", tar_q((char[]){ m->type, 0 }));
            fputc('\n', out);
        }
        free(xlink);
    } else {
        fprintf(out, "%s\n", tar_qname(name));
    }
    fflush(out);
    free(xname);
}

typedef int (*tar_member_fn)(struct tar_member *m);

/* the walk over the archive; FN gets each member and must read its data */
static int tar_read_count;
static void tar_read_archive(tar_member_fn fn, int open_it) {
    if (open_it) tar_open_read();
    int first = 1, skipping = 0;
    int label_checked = 0;
    for (;;) {
        struct tar_member m;
        int r = tar_read_header(&m, 0);
        if (r == TAR_HDR_EOF) {
            if (tar_o.block_number && !first) {
                fflush(stdout);
                fprintf(tar_o.vout, "block %ju: ** End of File **\n", tar_ar.block);
            }
            if (first && tar_ar.child > 0) {
                /* a decompressor that failed says so first; one that gave
                 * less than a block is an empty archive, as GNU reads it */
                tar_close_read();
                if (tar_ar.bytes == 0 || tar_ar.bytes < TAR_BLK) break;
            }
            if (first) tar_err("This does not look like a tar archive");
            break;
        }
        if (r == TAR_HDR_EOF_IN) {
            tar_member_free(&m);
            tar_fatal("Unexpected EOF in archive");
        }
        if (r == TAR_HDR_ZERO) {
            uintmax_t zb = tar_ar.block;
            if (tar_o.ignore_zeros) { first = 0; continue; }
            unsigned char *nb = tar_r_block();
            if (!nb || !tar_block_zero(nb)) {
                if (tar_warning_on(TW_ALONE_ZERO))
                    tar_warn("A lone zero block at %ju", zb);
                if (nb) tar_r_unget();
            }
            if (tar_o.block_number) {
                fflush(stdout);
                fprintf(tar_o.vout, "block %ju: ** Block of NULs **\n", zb - 1);
            }
            break;
        }
        if (r == TAR_HDR_BAD) {
            if (first) tar_err("This does not look like a tar archive");
            first = 0;
            if (!skipping) {
                tar_err("Skipping to next header");
                skipping = 1;
            }
            continue;
        }
        if (skipping) skipping = 0;
        first = 0;
        tar_read_count++;
        if (!label_checked && tar_o.label && tar_o.cmd != TAR_C_TESTLABEL) {
            /* -V when reading: the first member must be a label matching it */
            label_checked = 1;
            if (m.type != 'V')
                tar_fatal("Archive not labeled to match %s", tar_q(tar_o.label));
            if (!tar_fnm(tar_o.label, m.name, 0, 1, 0))
                tar_fatal("Volume %s does not match %s", tar_q(m.name), tar_q(tar_o.label));
        }
        int stop = fn(&m);
        tar_member_free(&m);
        if (stop) break;
    }
    if (open_it) tar_close_read();
}

/* ---- -t ------------------------------------------------------------------------ */

static void tar_omitted(struct tar_member *m) {
    if (!tar_o.show_omitted || !tar_nnames) return;
    if (m->type != '5' && m->type != 'D') return;
    char *n = tar_strdup(m->name);
    size_t l = strlen(n);
    while (l > 1 && n[l - 1] == '/') n[--l] = 0;
    tar_warn("%s: Omitting", tar_qc(n));
    free(n);
}

static int tar_list_one(struct tar_member *m) {
    int which;
    if (m->type == 'V') {
        /* the label is listed whatever the names say */
        tar_print_member(tar_o.vout, m, tar_o.verbose);
        return 0;
    }
    if (!tar_selected(m, &which)) {
        tar_omitted(m);
        tar_read_sparse10_map(m);
        tar_skip_data(tar_member_data(m));
        return 0;
    }
    if (!tar_o.absolute) tar_safe_start(m->name, 0, 0);
    if (which >= 0) tar_member_dir(which);
    tar_cur_block = m->hdr_block;
    tar_print_member(tar_o.vout, m, tar_o.verbose);
    if (m->type == 'D' && tar_o.incremental && tar_o.verbose > 1) tar_print_dumpdir(tar_o.vout, m);
    tar_read_sparse10_map(m);
    if (tar_skip_data(tar_member_data(m)) != 0) tar_fatal("Unexpected EOF in archive");
    return 0;
}

/* ---- -x ---------------------------------------------------------------------------
 *
 * Measured on GNU's -x, case by case in the fixture:
 *   - A file that is there is removed and made again (a link to it keeps
 *     the old data); an empty directory in the way is removed, a full one
 *     is "Cannot open: File exists". --overwrite writes into the file that
 *     is there (its other links see the new data); -U removes first
 *     ("Cannot unlink: Directory not empty"); --recursive-unlink empties a
 *     directory in the way; -k is "Cannot open: File exists" (for a link,
 *     "Cannot create symlink to 'T': File exists"), --skip-old-files
 *     silently passes ("skipping existing file" under -v),
 *     --keep-newer-files says "Current 'F' is newer or same age".
 *   - Modes: the archive's less the umask (all of them with -p or as
 *     root); missing parent directories are made 0777 less the umask; a
 *     directory's own mode and time are set once the archive is read (an
 *     existing one's too, unless --no-overwrite-dir); times are set on
 *     files, links and FIFOs (not with -m); owners only with --same-owner
 *     (or as root), "Cannot change ownership to uid U, gid G".
 *   - Unless -P: leading '/'s go ("Removing leading `/' from member
 *     names"), a member with a ".." in its name is refused ("Member name
 *     contains '..'"). --strip-components drops leading parts (a member
 *     with no more parts is passed over), --transform renames, -v shows
 *     the archive's names (the new ones with --show-transformed-names). */

struct tar_delayed { char *name; int dir; mode_t mode; intmax_t mtime; long mtime_ns;
                     intmax_t atime; long atime_ns; int set_mode; uintmax_t uid, gid; int chown; };
static struct tar_delayed *tar_delays; static int tar_ndelays, tar_delays_cap;
static mode_t tar_umask;

/* GNU's "implausibly old time stamp" (before 1970) and "time stamp ...
 * is N s in the future" for a member being extracted */
static void tar_check_time(const char *name, intmax_t s, long ns) {
    if (tar_o.touch || !tar_warning_on(TW_TIMESTAMP)) return;
    char b[64];
    if (s < 0) {
        tar_fmt_date(b, sizeof b, s, ns);
        tar_warn("%s: implausibly old time stamp %s", tar_qc(name), b);
        return;
    }
    struct timeval now;
    gettimeofday(&now, 0);
    long nns = (long)now.tv_usec * 1000;
    if (s > (intmax_t)now.tv_sec || (s == (intmax_t)now.tv_sec && ns > nns)) {
        intmax_t ds = s - (intmax_t)now.tv_sec;
        long dn = ns - nns;
        if (dn < 0) { dn += 1000000000L; ds--; }
        tar_fmt_date(b, sizeof b, s, ns);
        tar_warn("%s: time stamp %s is %jd.%09ld s in the future", tar_qc(name), b, ds, dn);
    }
}

static void tar_set_times(const char *name, struct tar_member *m, int nofollow) {
    if (tar_o.touch) return;
    tar_check_time(name, m->mtime, m->mtime_ns);
    struct timespec ts[2];
    if (m->has_atime) { ts[0].tv_sec = (time_t)m->atime; ts[0].tv_nsec = m->atime_ns; }
    else { ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_NOW; }
    ts[1].tv_sec = (time_t)m->mtime; ts[1].tv_nsec = m->mtime_ns;
    if (utimensat(AT_FDCWD, name, ts, nofollow ? AT_SYMLINK_NOFOLLOW : 0) != 0) {
        int e = errno;
        tar_errno(name, "Cannot utime", e);
    }
}

static void tar_set_owner(const char *name, struct tar_member *m, int nofollow) {
    if (!tar_o.same_owner) return;
    uintmax_t uid = m->uid, gid = m->gid;
    if (!tar_o.numeric_owner) {
        uintmax_t v;
        if (m->uname && *m->uname && tar_user_uid(m->uname, &v)) uid = v;
        if (m->gname && *m->gname && tar_group_gid(m->gname, &v)) gid = v;
    }
    int r = nofollow ? lchown(name, (uid_t)uid, (gid_t)gid) : chown(name, (uid_t)uid, (gid_t)gid);
    if (r != 0) {
        int e = errno;
        tar_err("%s: Cannot change ownership to uid %ju, gid %ju: %s", tar_qc(name), uid, gid, strerror(e));
    }
}

static mode_t tar_x_mode(mode_t m) {
    if (tar_o.same_perms) return m & 07777;
    return m & 0777 & ~tar_umask;
}

/* remove NAME and all under it (--recursive-unlink) */
static int tar_rm_tree(const char *name) {
    struct stat st;
    if (lstat(name, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(name);
    DIR *d = opendir(name);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char *c = (char *)tar_alloc(strlen(name) + strlen(de->d_name) + 2);
            sprintf(c, "%s/%s", name, de->d_name);
            tar_rm_tree(c);
            free(c);
        }
        closedir(d);
    }
    return rmdir(name);
}

/* make the missing parents of NAME (0777 less the umask) */
static void tar_make_parents(const char *name) {
    char *p = tar_strdup(name);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        struct stat st;
        if (lstat(p, &st) != 0) {
            if (mkdir(p, 0777) == 0) {
                /* noted, as GNU does, so its mode is right at the end */
            }
        }
        *s = '/';
    }
    free(p);
}

/* what to do about NAME being there: 1 go on, 0 skip the member (said),
 * -1 the member failed (said) */
enum { TAR_X_FILE, TAR_X_DIR, TAR_X_LINK, TAR_X_SYMLINK, TAR_X_NODE };
/* --backup: the name the file that is there moves to -- NAME~ (or the
 * suffix), or NAME.~N~ numbered (N past the highest there is), or for
 * "existing" numbered only where numbered ones are */
static char *tar_backup_name(const char *name) {
    const char *suf = tar_o.suffix;
    if (!suf) suf = getenv("SIMPLE_BACKUP_SUFFIX");
    if (!suf || !*suf || strchr(suf, '/')) suf = "~";
    int kind = tar_o.backup;
    unsigned long hi = 0;
    if (kind >= 2) {
        const char *sl = strrchr(name, '/');
        char *dir = sl ? tar_strndup(name, (size_t)(sl - name + 1)) : tar_strdup(".");
        const char *base = sl ? sl + 1 : name;
        size_t bl = strlen(base);
        DIR *d = opendir(*dir ? dir : ".");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                const char *n = de->d_name;
                if (strncmp(n, base, bl) || strncmp(n + bl, ".~", 2)) continue;
                const char *q = n + bl + 2;
                if (!isdigit((unsigned char)*q)) continue;
                char *e;
                unsigned long v = strtoul(q, &e, 10);
                if (e[0] == '~' && !e[1] && v > hi) hi = v;
            }
            closedir(d);
        }
        free(dir);
        if (kind == 2 && !hi) kind = 1;
        else kind = 3;
    }
    struct tar_sb b = {0};
    if (kind == 3) tar_sb_printf(&b, "%s.~%lu~", name, hi + 1);
    else tar_sb_printf(&b, "%s%s", name, suf);
    return tar_sb_take(&b);
}

static int tar_x_backup(const char *name, const struct stat *st) {
    if (!tar_o.backup || S_ISDIR(st->st_mode)) return 0;
    char *bk = tar_backup_name(name);
    if (rename(name, bk) != 0) {
        int e = errno;
        tar_err("%s: Cannot rename to %s: %s", tar_qc(name), tar_q(bk), strerror(e));
        free(bk);
        return -1;
    }
    if (tar_o.verbose) {
        fprintf(tar_o.vout, "Renaming %s to %s\n", tar_q(name), tar_q(bk));
        fflush(tar_o.vout);
    }
    free(bk);
    return 1;
}

static int tar_x_prepare(const char *name, struct tar_member *m, int kind) {
    struct stat st;
    if (lstat(name, &st) != 0) return 1;
    if (kind == TAR_X_DIR && S_ISDIR(st.st_mode)) return 1;
    if (kind == TAR_X_DIR && tar_o.keep_dir_symlink && S_ISLNK(st.st_mode)) {
        struct stat t;
        if (stat(name, &t) == 0 && S_ISDIR(t.st_mode)) return 3;   /* kept: its metadata too */
    }
    switch (tar_o.old_files) {
    case TAR_OLD_SKIP_OLD:
        if (kind == TAR_X_DIR) return 1;
        if (tar_o.verbose) tar_warn("%s: skipping existing file", tar_qc(name));
        return 0;
    case TAR_OLD_KEEP_OLD:
        if (kind == TAR_X_DIR) return 1;
        if (kind == TAR_X_SYMLINK)
            tar_err("%s: Cannot create symlink to %s: %s", tar_qc(name), tar_q(m->link), strerror(EEXIST));
        else if (kind == TAR_X_LINK)
            tar_err("%s: Cannot hard link to %s: %s", tar_qc(name), tar_q(m->link), strerror(EEXIST));
        else tar_errno(name, "Cannot open", EEXIST);
        return -1;
    case TAR_OLD_KEEP_NEWER:
        if (kind != TAR_X_DIR) {
            intmax_t mt = (intmax_t)st.st_mtime;
            long mns = st.st_mtim.tv_nsec;
            if (mt > m->mtime || (mt == m->mtime && mns >= m->mtime_ns)) {
                if (tar_warning_on(TW_IGNORE_NEWER))
                    tar_warn("Current %s is newer or same age", tar_q(name));
                return 0;
            }
        }
        break;
    case TAR_OLD_OVERWRITE:
        if (kind == TAR_X_FILE && S_ISREG(st.st_mode)) return 2;   /* write into it */
        break;
    default: break;
    }
    {
        int b = tar_x_backup(name, &st);
        if (b) return b > 0 ? 1 : -1;
    }
    if (tar_o.old_files == TAR_OLD_UNLINK_FIRST) {
        int r = S_ISDIR(st.st_mode) ? rmdir(name) : unlink(name);
        if (r != 0) {
            int e = errno;
            if (S_ISDIR(st.st_mode) && tar_o.recursive_unlink && tar_rm_tree(name) == 0) return 1;
            tar_errno(name, "Cannot unlink", e);
            return -1;
        }
        return 1;
    }
    /* the default: what is in the way goes, an empty directory too */
    if (S_ISDIR(st.st_mode)) {
        if (rmdir(name) == 0) return 1;
        int e = errno;
        if (tar_o.recursive_unlink && tar_rm_tree(name) == 0) return 1;
        if (tar_o.old_files == TAR_OLD_OVERWRITE && kind == TAR_X_FILE) {
            tar_errno(name, "Cannot open", EISDIR);
            return -1;
        }
        (void)e;
        if (kind == TAR_X_SYMLINK)
            tar_err("%s: Cannot create symlink to %s: %s", tar_qc(name), tar_q(m->link), strerror(EEXIST));
        else if (kind == TAR_X_LINK)
            tar_err("%s: Cannot hard link to %s: %s", tar_qc(name), tar_q(m->link), strerror(EEXIST));
        else if (kind == TAR_X_NODE)
            tar_err("%s: Cannot mknod: %s", tar_qc(name), strerror(EEXIST));
        else tar_errno(name, "Cannot open", EEXIST);
        return -1;
    }
    if (unlink(name) != 0) {
        int e = errno;
        tar_errno(name, "Cannot unlink", e);
        return -1;
    }
    return 1;
}

/* write the member's data to FD (-1: read and drop it) */
static int tar_copy_out(int fd, struct tar_member *m, const char *name) {
    int err = 0;
    if (m->is_sparse && m->sp_n) {
        /* the data segments, at their offsets */
        uintmax_t pos = 0;
        for (size_t i = 0; i < m->sp_n; i++) {
            uintmax_t left = m->sp_len[i];
            if (fd >= 0 && !err) {
                if (lseek(fd, (off_t)m->sp_off[i], SEEK_SET) < 0) {
                    /* a pipe: write the hole as zeros */
                    static const char z[TAR_BLK];
                    while (pos < m->sp_off[i]) {
                        size_t k = m->sp_off[i] - pos < TAR_BLK ? (size_t)(m->sp_off[i] - pos) : TAR_BLK;
                        if (tar_full_write(fd, z, k) != (int)k) { err = errno ? errno : EIO; break; }
                        pos += k;
                    }
                }
            }
            pos = m->sp_off[i];
            while (left) {
                unsigned char *b = tar_r_block();
                if (!b) return -2;
                size_t k = left < TAR_BLK ? (size_t)left : TAR_BLK;
                if (fd >= 0 && !err && tar_full_write(fd, b, k) != (int)k) err = errno ? errno : EIO;
                left -= k;
                pos += k;
            }
        }
        struct stat fst;
        if (fd >= 0 && !err && fstat(fd, &fst) == 0 && S_ISREG(fst.st_mode) &&
            (uintmax_t)fst.st_size < m->realsize) pos = (uintmax_t)fst.st_size;
        if (fd >= 0 && !err && pos < m->realsize) {
            /* the file ends in a hole */
            if (ftruncate(fd, (off_t)m->realsize) != 0) {
                static const char z[TAR_BLK];
                while (pos < m->realsize) {
                    size_t k = m->realsize - pos < TAR_BLK ? (size_t)(m->realsize - pos) : TAR_BLK;
                    if (tar_full_write(fd, z, k) != (int)k) break;
                    pos += k;
                }
            }
        }
        if (err) tar_errno(name, "Cannot write", err);
        return err ? -1 : 0;
    }
    uintmax_t left = m->size;
    while (left) {
        size_t k;
        unsigned char *b = tar_r_run(left, &k);
        if (!b) return -2;
        if (fd >= 0 && !err && tar_full_write(fd, b, k) != (int)k) err = errno ? errno : EIO;
        left -= k;
    }
    if (err) { tar_errno(name, "Cannot write", err); return -1; }
    return 0;
}

/* --to-command: the member's data on COMMAND's input */
static void tar_to_command(struct tar_member *m, const char *name) {
    int pfd[2];
    if (pipe(pfd) != 0) { tar_errno(name, "Cannot create pipe", errno); tar_skip_data(m->size); return; }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[1]);
        dup2(pfd[0], 0);
        close(pfd[0]);
        char b[64];
        setenv("TAR_FILENAME", name, 1);
        setenv("TAR_REALNAME", m->name, 1);
        snprintf(b, sizeof b, "%c", m->type == 0 ? '0' : m->type);
        setenv("TAR_FILETYPE", m->type == '5' ? "d" : m->type == '2' ? "l" : m->type == '1' ? "h" :
               m->type == '6' ? "p" : m->type == '3' ? "c" : m->type == '4' ? "b" : "f", 1);
        snprintf(b, sizeof b, "%04o", (unsigned)m->mode); setenv("TAR_MODE", b, 1);
        snprintf(b, sizeof b, "%ju", m->is_sparse ? m->realsize : m->size); setenv("TAR_SIZE", b, 1);
        tar_fmt_time(b, sizeof b, m->mtime, m->mtime_ns); setenv("TAR_MTIME", b, 1);
        if (m->has_atime) { tar_fmt_time(b, sizeof b, m->atime, m->atime_ns); setenv("TAR_ATIME", b, 1); }
        if (m->has_ctime) { tar_fmt_time(b, sizeof b, m->ctime, m->ctime_ns); setenv("TAR_CTIME", b, 1); }
        setenv("TAR_UNAME", m->uname ? m->uname : "", 1);
        setenv("TAR_GNAME", m->gname ? m->gname : "", 1);
        snprintf(b, sizeof b, "%ju", m->uid); setenv("TAR_UID", b, 1);
        snprintf(b, sizeof b, "%ju", m->gid); setenv("TAR_GID", b, 1);
        setenv("TAR_VERSION", "1.35", 1);
        setenv("TAR_ARCHIVE", tar_ar.name, 1);
        setenv("TAR_VOLUME", "1", 1);
        snprintf(b, sizeof b, "%ju", (uintmax_t)tar_o.blocking); setenv("TAR_BLOCKING_FACTOR", b, 1);
        setenv("TAR_FORMAT", m->fmt == TAR_F_GNU ? "gnu" : m->fmt == TAR_F_POSIX ? "posix" :
               m->fmt == TAR_F_USTAR ? "ustar" : "v7", 1);
        char *argv[4] = { (char *)"/bin/sh", (char *)"-c", (char *)tar_o.to_command, 0 };
        execv("/bin/sh", argv);
        _exit(127);
    }
    close(pfd[0]);
    signal(SIGPIPE, SIG_IGN);
    tar_copy_out(pfd[1], m, name);
    close(pfd[1]);
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    if (!tar_o.ignore_cmd_error) {
        if (WIFSIGNALED(st)) tar_err("%d: Child died with signal %d", (int)pid, WTERMSIG(st));
        else if (WIFEXITED(st) && WEXITSTATUS(st))
            tar_err("%d: Child returned status %d", (int)pid, WEXITSTATUS(st));
    }
}

/* the name a member is extracted as; 0 when it is passed over */
static char *tar_x_name(struct tar_member *m, int *refused) {
    *refused = 0;
    const char *n = m->name;
    if (!tar_o.absolute) {
        /* the leading part is said to go first; then a ".." anywhere in
         * the name refuses it, as GNU does */
        const char *safe = tar_safe_start(n, 0, 0);
        for (const char *s = n; *s; ) {
            if (s[0] == '.' && s[1] == '.' && (s[2] == '/' || s[2] == 0) && (s == n || s[-1] == '/')) {
                tar_err("%s: Member name contains '..'", tar_qc(n));
                *refused = 1;
                return 0;
            }
            const char *sl = strchr(s, '/');
            if (!sl) break;
            s = sl + 1;
        }
        n = safe;
    }
    /* --strip-components */
    for (int k = 0; k < tar_o.strip; k++) {
        const char *sl = strchr(n, '/');
        while (sl && sl[1] == '/') sl++;
        if (!sl || !sl[1]) return 0;
        n = sl + 1;
    }
    char *r = tar_nxfs ? tar_transform(n, TAR_XF_REG) : tar_strdup(n);
    /* the trailing slash of a directory goes; an empty name is "." */
    size_t l = strlen(r);
    while (l > 1 && r[l - 1] == '/') r[--l] = 0;
    if (!*r && !*m->name) { free(r); return tar_strdup("."); }
    if (!*r || !strcmp(r, ".") ) {
        if (m->type == '5') { free(r); return tar_strdup("."); }
        free(r);
        return 0;
    }
    if (tar_o.one_top_set) {
        const char *top = tar_o.one_top_level;
        char *def = 0;
        if (!top && 0) {
            /* the archive's name, its directory and suffixes gone */
            const char *an = tar_ar.name;
            const char *b = strrchr(an, '/');
            b = b ? b + 1 : an;
            def = tar_strdup(b);
            static const char *sfx[] = { ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.Z", ".tar.lz",
                                         ".tar.lzma", ".tar.lzo", ".tar.zst", ".tgz", ".taz",
                                         ".tbz2", ".tbz", ".tz2", ".txz", ".tlz", ".tzst", ".tar", 0 };
            for (int i = 0; sfx[i]; i++) {
                size_t dl = strlen(def), sl = strlen(sfx[i]);
                if (dl > sl && !strcmp(def + dl - sl, sfx[i])) { def[dl - sl] = 0; break; }
            }
            top = def;
        }
        size_t tl = strlen(top);
        if (!(strncmp(r, top, tl) == 0 && (r[tl] == '/' || r[tl] == 0))) {
            char *nr = (char *)tar_alloc(tl + strlen(r) + 2);
            sprintf(nr, "%s/%s", top, r);
            free(r);
            r = nr;
        }
        free(def);
    }
    return r;
}

/* -C between member names: a member is extracted in the directory of the
 * name it matched -- the -C's before that name, one after the other,
 * from where tar started. K is that name's directory index (-1 none). */
static char *tar_start_dir;
static int tar_cur_dir = -1;
static void tar_enter_dir(int k) {
    if (k == tar_cur_dir) return;
    if (!tar_start_dir) {
        char b[4096];
        if (!getcwd(b, sizeof b)) return;
        tar_start_dir = tar_strdup(b);
    }
    if (chdir(tar_start_dir) != 0) {
        int e = errno;
        tar_fatal("%s: Cannot open: %s", tar_qc(tar_start_dir), strerror(e));
    }
    tar_cur_dir = -1;
    for (int i = 0; i <= k; i++) {
        if (chdir(tar_dirs[i]) != 0) {
            int e = errno;
            tar_fatal("%s: Cannot open: %s", tar_qc(tar_dirs[i]), strerror(e));
        }
    }
    tar_cur_dir = k;
}

/* the directory a selected member goes in: its name's, or every -C */
static void tar_member_dir(int which) {
    if (!tar_ndirs) return;
    if (which >= 0) tar_enter_dir(tar_names[which].dir);
    else tar_enter_dir(tar_ndirs - 1);
}

static void tar_delay_dir(const char *name, struct tar_member *m, int set_mode) {
    if (set_mode) tar_check_time(name, m->mtime, m->mtime_ns);
    tar_grow((void **)&tar_delays, &tar_delays_cap, tar_ndelays, sizeof *tar_delays);
    struct tar_delayed *d = &tar_delays[tar_ndelays++];
    d->name = tar_strdup(name);
    d->dir = tar_cur_dir;
    d->mode = tar_x_mode(m->mode);
    d->mtime = m->mtime; d->mtime_ns = m->mtime_ns;
    d->atime = m->atime; d->atime_ns = m->has_atime ? m->atime_ns : UTIME_NOW;
    if (!m->has_atime) d->atime = 0;
    d->set_mode = set_mode;
    d->uid = m->uid; d->gid = m->gid;
    d->chown = tar_o.same_owner;
}

static void tar_apply_delays(void) {
    for (int i = tar_ndelays - 1; i >= 0; i--) {
        struct tar_delayed *d = &tar_delays[i];
        if (d->set_mode) {
            if (tar_ndirs) tar_enter_dir(d->dir);
            if (d->chown) {
                if (chown(d->name, (uid_t)d->uid, (gid_t)d->gid) != 0) {
                    int e = errno;
                    tar_err("%s: Cannot change ownership to uid %ju, gid %ju: %s", tar_qc(d->name), d->uid, d->gid, strerror(e));
                }
            }
            if (!tar_o.touch) {
                struct timespec ts[2];
                ts[0].tv_sec = (time_t)d->atime; ts[0].tv_nsec = d->atime_ns;
                ts[1].tv_sec = (time_t)d->mtime; ts[1].tv_nsec = d->mtime_ns;
                if (utimensat(AT_FDCWD, d->name, ts, 0) != 0) {
                    int e = errno;
                    tar_errno(d->name, "Cannot utime", e);
                }
            }
            if (chmod(d->name, d->mode) != 0) {
                int e = errno;
                tar_errno(d->name, "Cannot change mode", e);
            }
        }
        free(d->name);
    }
    tar_ndelays = 0;
}

static int tar_extract_one(struct tar_member *m) {
    int which;
    if (m->type == 'V' || m->type == 'M' || m->type == 'N') {
        if (m->type == 'V' && tar_o.verbose) tar_print_member(tar_o.vout, m, tar_o.verbose > 1);
        tar_skip_data(m->type == 'V' ? 0 : m->size);
        return 0;
    }
    if (!tar_selected(m, &which)) {
        tar_omitted(m);
        tar_read_sparse10_map(m);
        tar_skip_data(tar_member_data(m));
        return 0;
    }
    tar_read_sparse10_map(m);
    int refused;
    char *name = tar_x_name(m, &refused);
    if (!name) {
        tar_skip_data(tar_member_data(m));
        return 0;
    }
    tar_member_dir(which);
    if (!tar_confirm("extract", name)) {
        free(name);
        tar_skip_data(tar_member_data(m));
        return 0;
    }
    if (tar_o.verbose) tar_print_member(tar_o.vout, m, tar_o.verbose > 1);
    char type = m->type;
    if (!strchr("01234567DSV", type ? type : '0') && tar_warning_on(TW_UNKNOWN_CAST))
        tar_warn("%s: Unknown file type '%c', extracted as normal file", tar_qc(name), type);
    if (type == '7' && !tar_o.to_stdout && !tar_o.to_command) {
        /* said once, and only when files are made */
        static int said;
        if (!said && tar_warning_on(TW_CONTIG)) tar_warn("Extracting contiguous files as regular files");
        said = 1;
    }
    if (type == 'S' || type == '7') type = '0';
    if (type == 'D') type = '5';

    if (tar_o.to_stdout || tar_o.to_command) {
        if (type == '0' || (type != '1' && type != '2' && type != '3' && type != '4' &&
                            type != '5' && type != '6')) {
            if (tar_o.to_command) tar_to_command(m, name);
            else {
                fflush(stdout);
                if (tar_copy_out(1, m, name) == -2) { tar_err("Unexpected EOF in archive"); tar_fatal("Unexpected EOF in archive"); }
            }
        } else tar_skip_data(tar_member_data(m));
        free(name);
        return 0;
    }

    switch (type) {
    case '5': {
        int r = tar_x_prepare(name, m, TAR_X_DIR);
        if (r <= 0 || r == 3) break;
        tar_make_parents(name);
        struct stat st;
        int existed = stat(name, &st) == 0 && S_ISDIR(st.st_mode);
        if (!existed && mkdir(name, (tar_x_mode(m->mode) | 0700) & 07777) != 0) {
            int e = errno;
            tar_errno(name, "Cannot mkdir", e);
            break;
        }
        if (!existed || tar_o.old_files != TAR_OLD_NO_OVERWRITE_DIR)
            tar_delay_dir(name, m, 1);
        if (m->type == 'D' && tar_o.incremental) tar_incr_purge(name, m);
        break;
    }
    case '2': {
        int r = tar_x_prepare(name, m, TAR_X_SYMLINK);
        if (r <= 0) break;
        tar_make_parents(name);
        char *target = tar_nxfs ? tar_transform(m->link, TAR_XF_SYM) : tar_strdup(m->link);
        if (symlink(target, name) != 0) {
            int e = errno;
            tar_err("%s: Cannot create symlink to %s: %s", tar_qc(name), tar_q(target), strerror(e));
            free(target);
            break;
        }
        free(target);
        tar_set_owner(name, m, 1);
        tar_set_times(name, m, 1);
        break;
    }
    case '1': {
        const char *lt = m->link;
        if (!tar_o.absolute) lt = tar_safe_start(lt, 1, 0);
        for (int k = 0; k < tar_o.strip && lt; k++) {
            const char *sl = strchr(lt, '/');
            lt = sl ? sl + 1 : 0;
        }
        if (!lt) break;
        char *target = tar_nxfs ? tar_transform(lt, TAR_XF_HARD) : tar_strdup(lt);
        /* --one-top-level does not move a hard link's target: GNU's
         * "Cannot hard link to 'a'" for top/hl, measured */
        struct stat a, b;
        if (lstat(name, &a) == 0 && lstat(target, &b) == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev && a.st_ino) {
            free(target);
            break;   /* already the same file */
        }
        int r = tar_x_prepare(name, m, TAR_X_LINK);
        if (r <= 0) { free(target); break; }
        tar_make_parents(name);
        if (link(target, name) != 0) {
            int e = errno;
            tar_err("%s: Cannot hard link to %s: %s", tar_qc(name), tar_q(target), strerror(e));
        }
        free(target);
        break;
    }
    case '6': case '3': case '4': {
        int r = tar_x_prepare(name, m, TAR_X_NODE);
        if (r <= 0) break;
        tar_make_parents(name);
        int rc;
        if (type == '6') rc = mkfifo(name, tar_x_mode(m->mode));
        else {
            rc = mknod(name, tar_x_mode(m->mode) | (type == '3' ? S_IFCHR : S_IFBLK),
                       makedev(m->devmaj, m->devmin));
        }
        if (rc != 0) {
            int e = errno;
            tar_err("%s: Cannot %s: %s", tar_qc(name), type == '6' ? "mkfifo" : "mknod", strerror(e));
            break;
        }
        if (tar_o.same_perms) chmod(name, m->mode & 07777);
        tar_set_owner(name, m, 0);
        tar_set_times(name, m, 0);
        break;
    }
    default: {
        /* an unknown type was told of above */
        int r = tar_x_prepare(name, m, TAR_X_FILE);
        if (r <= 0) {
            tar_skip_data(m->size);
            break;
        }
        tar_make_parents(name);
        int fd;
        if (r == 2) fd = open(name, O_WRONLY | O_TRUNC, 0);
        else fd = open(name, O_WRONLY | O_CREAT | O_EXCL, tar_x_mode(m->mode));
        if (fd < 0) {
            int e = errno;
            tar_errno(name, "Cannot open", e);
            tar_skip_data(m->size);
            break;
        }
        int cr = tar_copy_out(fd, m, name);
        if (close(fd) != 0 && cr == 0) {
            int e = errno;
            tar_errno(name, "Cannot close", e);
        }
        if (cr == -2) {
            free(name);
            tar_err("Unexpected EOF in archive");
            tar_fatal("Unexpected EOF in archive");
        }
        tar_set_owner(name, m, 0);
        tar_set_times(name, m, 0);
        if (tar_o.same_perms || r == 2) {
            if (chmod(name, tar_x_mode(m->mode)) != 0) {
                int e = errno;
                tar_errno(name, "Cannot change mode", e);
            }
        }
        break;
    }
    }
    free(name);
    return 0;
}

/* ---- sparse files -------------------------------------------------------------------
 *
 * The data segments of a file with holes: by SEEK_DATA/SEEK_HOLE where
 * the system has them (Linux), else by reading it for blocks of zeros.
 * 1 when there is a hole, 0 when not (the file is stored whole). */

static int tar_sparse_map(int fd, off_t size, uintmax_t **offs, uintmax_t **lens, size_t *n) {
    size_t cap = 8;
    uintmax_t *o = (uintmax_t *)tar_alloc(cap * sizeof *o);
    uintmax_t *l = (uintmax_t *)tar_alloc(cap * sizeof *l);
    size_t k = 0;
    int used_seek = 0;
#if defined(SU_HOST) && defined(SEEK_DATA)
    if (tar_o.hole_seek != 1) {
        off_t pos = 0;
        int ok = 1;
        while (pos < size) {
            off_t d = lseek(fd, pos, SEEK_DATA);
            if (d < 0) { if (errno == ENXIO) break; ok = 0; break; }
            off_t h = lseek(fd, d, SEEK_HOLE);
            if (h < 0) { ok = 0; break; }
            if (k == cap) { cap *= 2; o = (uintmax_t *)tar_realloc(o, cap * sizeof *o); l = (uintmax_t *)tar_realloc(l, cap * sizeof *l); }
            o[k] = (uintmax_t)d; l[k] = (uintmax_t)(h - d); k++;
            pos = h;
        }
        if (ok) used_seek = 1;
        else k = 0;
    }
#endif
    if (!used_seek) {
        /* by reading: runs of non-zero 512-byte blocks */
        lseek(fd, 0, SEEK_SET);
        unsigned char b[TAR_BLK];
        off_t pos = 0;
        int in = 0;
        for (;;) {
            ssize_t r = tar_full_read(fd, b, TAR_BLK);
            if (r <= 0) break;
            int zero = 1;
            for (ssize_t i = 0; i < r; i++) if (b[i]) { zero = 0; break; }
            if (!zero) {
                if (!in) {
                    if (k == cap) { cap *= 2; o = (uintmax_t *)tar_realloc(o, cap * sizeof *o); l = (uintmax_t *)tar_realloc(l, cap * sizeof *l); }
                    o[k] = (uintmax_t)pos; l[k] = 0; k++;
                    in = 1;
                }
                l[k - 1] += (uintmax_t)r;
            } else in = 0;
            pos += r;
        }
    }
    lseek(fd, 0, SEEK_SET);
    uintmax_t data = 0;
    for (size_t i = 0; i < k; i++) data += l[i];
    /* the end of the file is always in the map (an empty segment there
     * when it ends in a hole) */
    if (!k || o[k - 1] + l[k - 1] < (uintmax_t)size) {
        if (k == cap) { cap *= 2; o = (uintmax_t *)tar_realloc(o, cap * sizeof *o); l = (uintmax_t *)tar_realloc(l, cap * sizeof *l); }
        o[k] = (uintmax_t)size; l[k] = 0; k++;
    }
    if (data == (uintmax_t)size) { free(o); free(l); return 0; }
    *offs = o; *lens = l; *n = k;
    return 1;
}

/* ---- -d ------------------------------------------------------------------------------
 *
 * GNU's -d, measured: each difference on stdout as "NAME: WHAT differs"
 * -- for a regular file Mode, Uid, Gid, Mod time, Size (and only when the
 * sizes agree, "Contents differ"); a directory's Mode; "Symlink differs";
 * "Not linked to TARGET"; "File type differs" -- the status 1. A file
 * that is not there is a warning, "NAME: Warning: Cannot stat: ...",
 * status 1 too; other failures are errors. */

static void tar_diff_report(const char *name, const char *what) {
    fflush(stderr);
    printf("%s: %s\n", tar_qc(name), what);
    fflush(stdout);
    if (tar_status < TAR_EXIT_DIFFER) tar_status = TAR_EXIT_DIFFER;
}

static int tar_diff_stat(const char *name, struct stat *st, int follow) {
    int r = follow ? stat(name, st) : lstat(name, st);
    if (r != 0) {
        int e = errno;
        if (e == ENOENT) {
            tar_warn("%s: Warning: Cannot stat: %s", tar_qc(name), strerror(e));
            if (tar_status < TAR_EXIT_DIFFER) tar_status = TAR_EXIT_DIFFER;
        } else tar_errno(name, "Cannot stat", e);
        return -1;
    }
    return 0;
}

static int tar_verifying;          /* -W: the -d pass after writing */
static int tar_diff_one(struct tar_member *m) {
    int which;
    if (m->type == 'V' || m->type == 'M' || m->type == 'N') { tar_skip_data(m->type == 'V' ? 0 : m->size); return 0; }
    if (!tar_selected(m, &which)) {
        tar_omitted(m);
        tar_read_sparse10_map(m);
        tar_skip_data(tar_member_data(m));
        return 0;
    }
    tar_read_sparse10_map(m);
    int refused;
    char *name = tar_x_name(m, &refused);
    if (!name) { tar_skip_data(tar_member_data(m)); return 0; }
    if (!tar_verifying) tar_member_dir(which);
    if (tar_o.verbose) {
        if (tar_verifying) fputs("Verify ", stdout);
        tar_print_member(stdout, m, tar_o.verbose > 1);
    }
    struct stat st;
    char type = m->type;
    if (type == 'S' || type == '7') type = '0';
    if (type == 'D') type = '5';
    switch (type) {
    case '5':
        if (tar_diff_stat(name, &st, 0) != 0) break;
        if (!S_ISDIR(st.st_mode)) { tar_diff_report(name, "File type differs"); break; }
        if ((st.st_mode & 07777) != (m->mode & 07777)) tar_diff_report(name, "Mode differs");
        break;
    case '2': {
        if (tar_diff_stat(name, &st, 0) != 0) break;
        char buf[4096];
        ssize_t k = readlink(name, buf, sizeof buf - 1);
        if (k < 0) { int e = errno; tar_errno(name, "Cannot readlink", e); break; }
        buf[k] = 0;
        if (strcmp(buf, m->link) != 0) tar_diff_report(name, "Symlink differs");
        break;
    }
    case '1': {
        const char *lt = m->link;
        if (!tar_o.absolute) lt = tar_safe_start(lt, 1, 0);
        struct stat a;
        if (tar_diff_stat(name, &st, 0) != 0) break;
        if (tar_diff_stat(lt, &a, 0) != 0) break;
        if (st.st_ino != a.st_ino || st.st_dev != a.st_dev || !st.st_ino) {
            fflush(stderr);
            printf("%s: Not linked to %s\n", tar_qc(name), tar_qc(lt));
            fflush(stdout);
            if (tar_status < TAR_EXIT_DIFFER) tar_status = TAR_EXIT_DIFFER;
        }
        break;
    }
    case '6': case '3': case '4':
        if (tar_diff_stat(name, &st, 0) != 0) break;
        if ((type == '6' && !S_ISFIFO(st.st_mode)) || (type == '3' && !S_ISCHR(st.st_mode)) ||
            (type == '4' && !S_ISBLK(st.st_mode))) { tar_diff_report(name, "File type differs"); break; }
        if (type != '6' && (major(st.st_rdev) != m->devmaj || minor(st.st_rdev) != m->devmin))
            tar_diff_report(name, "Device number differs");
        if (type != '6' && (st.st_mode & 07777) != (m->mode & 07777)) tar_diff_report(name, "Mode differs");
        break;
    default: {
        if (tar_diff_stat(name, &st, 0) != 0) { tar_skip_data(m->size); break; }
        if (!S_ISREG(st.st_mode)) { tar_diff_report(name, "File type differs"); tar_skip_data(m->size); break; }
        if ((st.st_mode & 07777) != (m->mode & 07777)) tar_diff_report(name, "Mode differs");
        if ((uintmax_t)st.st_uid != m->uid) tar_diff_report(name, "Uid differs");
        if ((uintmax_t)st.st_gid != m->gid) tar_diff_report(name, "Gid differs");
        if ((intmax_t)st.st_mtime != m->mtime) tar_diff_report(name, "Mod time differs");
        uintmax_t fsize = m->is_sparse ? m->realsize : m->size;
        if ((uintmax_t)st.st_size != fsize) {
            tar_diff_report(name, "Size differs");
            tar_skip_data(m->size);
            break;
        }
        int fd = su_open_read(name, 0);
        if (fd < 0) { int e = errno; tar_errno(name, "Cannot open", e); tar_skip_data(m->size); break; }
        int differ = 0;
        if (m->is_sparse && m->sp_n) {
            uintmax_t pos = 0;
            unsigned char fb[TAR_BLK];
            for (size_t i = 0; i < m->sp_n; i++) {
                /* the hole before this segment must be zeros */
                while (!differ && pos < m->sp_off[i]) {
                    size_t k = m->sp_off[i] - pos < TAR_BLK ? (size_t)(m->sp_off[i] - pos) : TAR_BLK;
                    ssize_t r = tar_full_read(fd, fb, k);
                    if (r != (ssize_t)k) { differ = 1; break; }
                    for (size_t j = 0; j < k; j++) if (fb[j]) differ = 1;
                    if (differ) {
                        /* GNU names the piece of the hole that has data */
                        fflush(stderr);
                        printf("%s: File fragment at %ju is not a hole\n", tar_qc(name), pos);
                        fflush(stdout);
                        if (tar_status < TAR_EXIT_DIFFER) tar_status = TAR_EXIT_DIFFER;
                        differ = 2;
                    }
                    pos += k;
                }
                lseek(fd, (off_t)m->sp_off[i], SEEK_SET);
                pos = m->sp_off[i];
                uintmax_t left = m->sp_len[i];
                while (left) {
                    unsigned char *b = tar_r_block();
                    if (!b) { close(fd); free(name); tar_fatal("Unexpected EOF in archive"); }
                    size_t k = left < TAR_BLK ? (size_t)left : TAR_BLK;
                    if (!differ) {
                        ssize_t r = tar_full_read(fd, fb, k);
                        if (r != (ssize_t)k || memcmp(fb, b, k)) differ = 1;
                    }
                    left -= k; pos += k;
                }
            }
        } else {
            uintmax_t left = m->size;
            unsigned char fb[TAR_BLK];
            while (left) {
                unsigned char *b = tar_r_block();
                if (!b) { close(fd); free(name); tar_fatal("Unexpected EOF in archive"); }
                size_t k = left < TAR_BLK ? (size_t)left : TAR_BLK;
                if (!differ) {
                    ssize_t r = tar_full_read(fd, fb, k);
                    if (r != (ssize_t)k || memcmp(fb, b, k)) differ = 1;
                }
                left -= k;
            }
        }
        close(fd);
        if (differ == 1) tar_diff_report(name, "Contents differ");
        break;
    }
    }
    free(name);
    return 0;
}

/* ---- -r, -u, -A, --delete --------------------------------------------------------
 *
 * The archive is read to its end (the first zero block), and what is
 * added is written from there: the record holding that block is
 * written again with the new members in it, and on. The file is not
 * made shorter. -u adds a file only when it is newer than every copy of
 * it the archive holds (or when it has none); -A copies the members of
 * each archive named; --delete moves the members it keeps over the ones
 * it drops, and cuts the file at its new end (a whole record). */

struct tar_seen { char *name; intmax_t mtime; };
static struct tar_seen *tar_seen_tab; static int tar_nseen, tar_seen_cap;
static uintmax_t tar_end_block;   /* where the archive's end marker is */

static int tar_scan_one(struct tar_member *m) {
    tar_grow((void **)&tar_seen_tab, &tar_seen_cap, tar_nseen, sizeof *tar_seen_tab);
    tar_seen_tab[tar_nseen].name = tar_strdup(m->name);
    tar_seen_tab[tar_nseen++].mtime = m->mtime;
    tar_read_sparse10_map(m);
    if (tar_skip_data(tar_member_data(m)) != 0) tar_fatal("Unexpected EOF in archive");
    tar_end_block = tar_ar.block;
    return 0;
}

/* open the archive read-write, read it to its end, and leave tar_ar set
 * up to write from there */
static void tar_open_for_append(void) {
    const char *name = tar_archive_name();
    if (!strcmp(name, "-"))
        tar_usage_error("Options '-Aru' are incompatible with '-f -'");
    tar_ar.name = name;
    tar_ar.is_stdio = 0;
    tar_ar_init(tar_o.record_size);
    tar_ar.fd = open(name, O_RDWR | O_CREAT, 0666);
    if (tar_ar.fd < 0) {
        /* GNU goes on, and its first read fails */
        tar_ar.passed = 1;
        tar_read_failed(EBADF);
    }
    tar_note_archive_id();
    if (tar_o.verify) tar_verify_fd = open(name, O_RDONLY);
    struct stat st;
    fstat(tar_ar.fd, &st);
    tar_end_block = 0;
    if (st.st_size > 0) {
        tar_reading_started = 1;   /* no compression sniffing here */
        int saved = tar_nnames;
        tar_nnames = 0;            /* every member counts */
        tar_read_archive(tar_scan_one, 0);
        tar_nnames = saved;
    }
    /* write from the end marker: the record that holds it, again */
    uintmax_t blk = tar_end_block;
    size_t per = tar_ar.recsize / TAR_BLK;
    uintmax_t rec = blk / per;
    off_t recoff = (off_t)(rec * tar_ar.recsize);
    memset(tar_ar.rec, 0, tar_ar.recsize);
    if (lseek(tar_ar.fd, recoff, SEEK_SET) >= 0) {
        ssize_t r = tar_full_read(tar_ar.fd, tar_ar.rec, (size_t)((blk - rec * per) * TAR_BLK));
        (void)r;
    }
    lseek(tar_ar.fd, recoff, SEEK_SET);
    tar_ar.pos = (size_t)(blk - rec * per) * TAR_BLK;
    memset(tar_ar.rec + tar_ar.pos, 0, tar_ar.recsize - tar_ar.pos);
    tar_ar.block = blk;
    tar_ar.writing = 1;
    tar_ar.eof = 0;
    tar_ar.bytes = 0;
}

static intmax_t tar_newest_copy(const char *name, int *found) {
    intmax_t best = 0;
    *found = 0;
    for (int i = 0; i < tar_nseen; i++) {
        const char *s = tar_seen_tab[i].name;
        size_t a = strlen(s), b = strlen(name);
        while (a > 1 && s[a - 1] == '/') a--;
        while (b > 1 && name[b - 1] == '/') b--;
        if (a == b && !strncmp(s, name, a)) {
            if (!*found || tar_seen_tab[i].mtime > best) best = tar_seen_tab[i].mtime;
            *found = 1;
        }
    }
    return best;
}


static void tar_catenate(void) {
    /* each archive's members are copied; one that is not the last keeps
     * the rest of the record its end marker is in (GNU's: the archives
     * after the first one are then behind zero blocks -- seen with -i) */
    tar_collect_names();
    size_t per = tar_ar.recsize / TAR_BLK;
    int names_left = 0;
    for (int i = 0; i < tar_nnames; i++) names_left++;
    for (int ni = 0; ni < tar_nnames; ni++) {
        const char *s = tar_names[ni].name;
        if (tar_names[ni].dir >= 0 && (ni == 0 || tar_names[ni - 1].dir != tar_names[ni].dir)) {
            if (chdir(tar_dirs[tar_names[ni].dir]) != 0) {
                int e = errno;
                tar_fatal("%s: Cannot open: %s", tar_qc(tar_dirs[tar_names[ni].dir]), strerror(e));
            }
        }
        int last = ni == tar_nnames - 1;
        int fd = su_open_read(s, 0);
        if (fd < 0) { int e = errno; tar_errno(s, "Cannot open", e); continue; }
        size_t len;
        char *d = tar_slurp_fd(fd, &len);
        close(fd);
        if (!d) { int e = errno; tar_fatal("%s: Cannot read: %s", tar_qc(s), strerror(e)); }
        size_t nb = len / TAR_BLK, i = 0;
        int bad = 0;
        while (i < nb) {
            const unsigned char *b = (const unsigned char *)d + i * TAR_BLK;
            if (tar_block_zero(b)) {
                if (!last) {
                    size_t end = (i / per + 1) * per;
                    if (end > nb) end = nb;
                    tar_w_data(b, (end - i) * TAR_BLK);
                }
                break;
            }
            if (!tar_header_sum_ok(b)) {
                if (!bad) tar_err("Skipping to next header");
                bad = 1;
                i++;
                continue;
            }
            bad = 0;
            intmax_t v; int ok;
            tar_from_num(b + TAR_H_SIZE, 12, &v, &ok);
            char t = (char)b[TAR_H_TYPE];
            uintmax_t size = (t == '1' || t == '2' || t == '3' || t == '4' || t == '5' || t == '6') ? 0 : (uintmax_t)(v < 0 ? 0 : v);
            size_t n = 1;
            if (t == 'S' && b[TAR_H_ISEXTENDED]) {
                while (i + n < nb) {
                    const unsigned char *e = (const unsigned char *)d + (i + n) * TAR_BLK;
                    n++;
                    if (!e[504]) break;
                }
            }
            n += (size_t)((size + TAR_BLK - 1) / TAR_BLK);
            if (i + n > nb) n = nb - i;
            tar_w_data(b, n * TAR_BLK);
            i += n;
        }
        free(d);
    }
    (void)names_left;
}

/* --delete: the kept members move over the dropped ones. The archive is
 * read whole (a file, or standard input), the spans of blocks of the
 * members kept noted, and they are written again from the start, then
 * the end marker, the record filled out; a file is cut there. */
static void tar_delete(void) {
    const char *name = tar_archive_name();
    int stdio = !strcmp(name, "-");
    int fd = stdio ? 0 : open(name, O_RDWR | O_CREAT, 0666);
    tar_ar.name = name;
    if (fd < 0) {
        tar_ar.passed = 1;
        tar_read_failed(EBADF);
    }
    size_t len = 0;
    char *all = tar_slurp_fd(fd, &len);
    if (!all) {
        int e = errno;
        tar_ar.passed = 1;
        tar_read_failed(e);
    }
    tar_collect_names();
    tar_ar.name = name;
    tar_ar_init(tar_o.record_size);
    tar_ar.mem = (unsigned char *)all;
    tar_ar.mem_len = len;
    tar_ar.mem_pos = 0;
    tar_ar.fd = -1;
    tar_reading_started = 1;
    uintmax_t *spans = 0; size_t nspan = 0, capspan = 0;
    int first = 1, dropped = 0;
    for (;;) {
        uintmax_t start = tar_ar.block;
        struct tar_member m;
        int r = tar_read_header(&m, 0);
        if (r == TAR_HDR_EOF) { if (first && len >= TAR_BLK) tar_err("This does not look like a tar archive"); break; }
        if (r == TAR_HDR_EOF_IN) tar_fatal("Unexpected EOF in archive");
        if (r == TAR_HDR_ZERO) break;
        if (r == TAR_HDR_BAD) {
            if (first) tar_err("This does not look like a tar archive");
            first = 0;
            continue;
        }
        first = 0;
        int which;
        int drop = m.type != 'V' && tar_selected(&m, &which) && which != -2;
        if (drop) dropped = 1;
        tar_read_sparse10_map(&m);
        if (tar_skip_data(tar_member_data(&m)) != 0) tar_fatal("Unexpected EOF in archive");
        if (!drop) {
            if (nspan + 2 > capspan) {
                capspan = capspan ? capspan * 2 : 32;
                spans = (uintmax_t *)tar_realloc(spans, capspan * sizeof *spans);
            }
            spans[nspan++] = start;
            spans[nspan++] = tar_ar.block - start;
        }
        tar_member_free(&m);
    }
    tar_report_unmatched();
    if (!dropped && !stdio) {
        /* nothing goes: the archive stays as it is */
        close(fd);
        free(spans);
        free(all);
        return;
    }
    /* write the kept blocks, from the start */
    tar_ar.mem = 0;
    tar_ar.fd = stdio ? 1 : fd;
    tar_ar.writing = 1;
    tar_ar.pos = 0; tar_ar.block = 0; tar_ar.bytes = 0; tar_ar.records = 0;
    memset(tar_ar.rec, 0, tar_ar.recsize);
    if (!stdio) lseek(fd, 0, SEEK_SET);
    for (size_t i = 0; i < nspan; i += 2) {
        uintmax_t off = spans[i] * TAR_BLK, n = spans[i + 1] * TAR_BLK;
        if (off + n > len) n = off < len ? len - off : 0;
        tar_w_data(all + off, (size_t)n);
    }
    tar_w_block(); tar_w_done();
    tar_w_block(); tar_w_done();
    if (tar_ar.pos) tar_flush_record();
    if (!stdio) {
        if (ftruncate(fd, (off_t)tar_ar.bytes) != 0) {
            int e = errno;
            tar_err("%s: Cannot truncate: %s", tar_qc(name), strerror(e));
        }
        close(fd);
    } else fflush(stdout);
    tar_ar.fd = -1;
    free(spans);
    free(all);
}

/* --test-label: the archive's label against the names given. With
 * none, the label is printed; a name that matches it makes the status 0,
 * none 1 ("Archive label mismatch" under -v, the label printed first). */
static void tar_test_label(void) {
    tar_collect_names();
    tar_open_read();
    struct tar_member m;
    int r = tar_read_header(&m, 0);
    int labeled = r == TAR_HDR_OK && m.type == 'V';
    if (r == TAR_HDR_EOF && !tar_nnames) tar_err("This does not look like a tar archive");
    if (!tar_nnames) {
        if (labeled) printf("%s\n", tar_qname(m.name));
    } else {
        int match = 0;
        for (int i = 0; labeled && i < tar_nnames; i++) {
            struct tar_name *nm = &tar_names[i];
            if (nm->wildcards ? tar_fnm(nm->name, m.name, nm->icase, 1, 0)
                              : (nm->icase ? strcasecmp(nm->name, m.name) : strcmp(nm->name, m.name)) == 0)
                match = 1;
        }
        if (!match) {
            for (int i = 0; i < tar_nnames; i++)
                if (!tar_names[i].wildcards && tar_has_wild(tar_names[i].name) && !tar_o.wildcards_set) {
                    tar_warn("Pattern matching characters used in file names");
                    tar_warn("Use --wildcards to enable pattern matching, or --no-wildcards to suppress this warning");
                    break;
                }
            if (tar_o.verbose) {
                if (labeled) { printf("%s\n", tar_qname(m.name)); fflush(stdout); }
                tar_warn("Archive label mismatch");
            }
            if (tar_status < TAR_EXIT_DIFFER) tar_status = TAR_EXIT_DIFFER;
        }
    }
    if (r == TAR_HDR_OK) tar_member_free(&m);
    tar_close_read();
}

/* -W: the archive just written, read back and compared, as -d would */
static void tar_verify(void) {
    tar_verifying = 1;
    int saved = tar_nnames;
    tar_nnames = 0;
    tar_reading_started = 0;
    tar_read_archive(tar_diff_one, 1);
    tar_nnames = saved;
}

static void tar_run_other(void) {
    switch (tar_o.cmd) {
    case TAR_C_DIFF:
        tar_collect_names();
        tar_open_read();
        if (!tar_nnames && tar_ndirs) tar_enter_dir(tar_ndirs - 1);
        tar_read_archive(tar_diff_one, 0);
        tar_close_read();
        tar_report_unmatched();
        break;
    case TAR_C_APPEND: case TAR_C_UPDATE:
        tar_update_mode = tar_o.cmd == TAR_C_UPDATE;
        tar_open_for_append();
        tar_dump_all(tar_update_mode);
        if (tar_update_mode) tar_report_trailing();
        tar_close_write();
        if (tar_o.verify) tar_verify();
        tar_check_links_report();
        break;
    case TAR_C_CAT:
        tar_open_for_append();
        tar_catenate();
        tar_close_write();
        break;
    case TAR_C_DELETE:
        tar_delete();
        break;
    case TAR_C_TESTLABEL:
        tar_test_label();
        break;
    }
}

/* @@DIFF@@ */

static int tar_run(void) {
    tar_umask = umask(0);
    umask(tar_umask);
    switch (tar_o.cmd) {
    case TAR_C_CREATE:
        if (tar_o.verify && !strcmp(tar_archive_name(), "-"))
            tar_fatal("Cannot verify stdin/stdout archive");
        if (tar_o.incremental) {
            struct timeval now;
            gettimeofday(&now, 0);
            tar_dump_s = (intmax_t)now.tv_sec; tar_dump_ns = (long)now.tv_usec * 1000;
            if (tar_o.listed_incr && !(tar_o.level_set && tar_o.level == 0)) tar_snap_load(tar_o.listed_incr);
        }
        tar_open_write();
        tar_write_label();
        tar_dump_all(0);
        if (tar_o.listed_incr) tar_snap_save(tar_o.listed_incr);
        tar_report_trailing();
        tar_close_write();
        if (tar_o.verify) tar_verify();
        tar_check_links_report();
        if (tar_o.totals) tar_print_totals(1, tar_ar.bytes);
        break;
    case TAR_C_LIST:
        tar_collect_names();
        tar_read_archive(tar_list_one, 1);
        tar_report_unmatched();
        if (tar_o.totals) tar_print_totals(0, tar_ar.passed * tar_ar.recsize);
        break;
    case TAR_C_EXTRACT: {
        tar_collect_names();
        tar_open_read();
        /* with no names, every -C before reading; with names, each
         * member goes where its name was given */
        if (!tar_nnames && tar_ndirs) tar_enter_dir(tar_ndirs - 1);
        tar_bn_extract = 1;
        tar_read_archive(tar_extract_one, 0);
        tar_close_read();
        tar_apply_delays();
        tar_report_unmatched();
        if (tar_o.totals) tar_print_totals(0, tar_ar.passed * tar_ar.recsize);
        break;
    }
    default:
        tar_run_other();
        break;
    }
    return tar_status;
}

/* @@END@@ */

/* ---- help ------------------------------------------------------------------------ */

static void tar_print_help(void) {
    printf("Usage: %s [OPTION...] [FILE]...\n", su_short);
    fputs(
"Create, list, update and unpack tape archives (tar files): many files kept\n"
"in one.\n"
"\n"
"For example:\n"
"  tar -cf all.tar a b     # put a and b into a new all.tar\n"
"  tar -tvf all.tar        # show what all.tar holds, in detail\n"
"  tar -xf all.tar         # unpack everything in all.tar\n"
"\n"
" What to do (exactly one):\n"
"  -A, --catenate, --concatenate   add other archives' members to the end of one\n"
"  -c, --create               make a new archive\n"
"      --delete               take members out of an archive (not on tapes)\n"
"  -d, --diff, --compare      tell how the files differ from the archive\n"
"  -r, --append               add files at the end of an archive\n"
"      --test-label           check the archive's volume label, then stop\n"
"  -t, --list                 show what the archive holds\n"
"  -u, --update               add only files newer than their archived copy\n"
"  -x, --extract, --get       unpack members from an archive\n"
"\n"
" How:\n"
"      --check-device         compare device numbers for incremental archives\n"
"                             (the default)\n"
"  -g, --listed-incremental=FILE   new-style GNU incremental backup, state in FILE\n"
"  -G, --incremental          old-style GNU incremental backup\n"
"      --hole-detection=TYPE  how to find holes in sparse files (raw, seek)\n"
"      --ignore-failed-read   an unreadable file is not a failure\n"
"      --level=NUMBER         the dump level of a new listed-incremental archive\n"
"      --no-check-device      ignore device numbers for incremental archives\n"
"      --no-seek              the archive cannot be seeked in\n"
"  -n, --seek                 the archive can be seeked in\n"
"      --occurrence[=NUMBER]  act on only the NUMBERth copy of each named member\n"
"                             (with --delete, --diff, --extract or --list and a\n"
"                             list of names; NUMBER is 1 if left out)\n"
"      --sparse-version=MAJOR[.MINOR]\n"
"                             the sparse format version to write (implies -S)\n"
"  -S, --sparse               store sparse files compactly\n"
"\n"
" Which local files:\n"
"      --add-file=FILE        add FILE (for names that start with a dash)\n"
"  -C, --directory=DIR        work in DIR from here on\n"
"      --exclude=PATTERN      leave out files matching PATTERN\n"
"      --exclude-backups      leave out backup and lock files\n"
"      --exclude-caches       leave out what directories holding CACHEDIR.TAG\n"
"                             contain, keeping the tag\n"
"      --exclude-caches-all   leave out directories holding CACHEDIR.TAG\n"
"      --exclude-caches-under leave out all under directories holding\n"
"                             CACHEDIR.TAG\n"
"      --exclude-ignore=FILE  read each directory's exclusion patterns from FILE\n"
"                             there, when it exists\n"
"      --exclude-ignore-recursive=FILE\n"
"                             the same, for the directory and all below it\n"
"      --exclude-tag=FILE     leave out what directories holding FILE contain,\n"
"                             keeping FILE\n"
"      --exclude-tag-all=FILE leave out directories holding FILE\n"
"      --exclude-tag-under=FILE   leave out all under directories holding FILE\n"
"      --exclude-vcs          leave out version-control directories and files\n"
"      --exclude-vcs-ignores  read exclusion patterns from the VCS ignore files\n"
"      --no-null              undo an earlier --null\n"
"      --no-recursion         do not go into directories by themselves\n"
"      --no-unquote           take names as they are, escapes and all\n"
"      --no-verbatim-files-from   lines of -T that start with '-' are options\n"
"                             (the default)\n"
"      --null                 -T's names end in NUL; implies\n"
"                             --verbatim-files-from\n"
"      --recursion            go into directories (the default)\n"
"  -T, --files-from=FILE      read the names to work on from FILE\n"
"      --unquote              decode escapes in names (the default)\n"
"      --verbatim-files-from  take -T's lines as names, whatever they hold\n"
"  -X, --exclude-from=FILE    leave out files matching the patterns in FILE\n"
"\n"
" Name matching (for both exclusion and member patterns):\n"
"\n"
"      --anchored             a pattern matches from the start of a name\n"
"      --ignore-case          upper and lower case are the same\n"
"      --no-anchored          a pattern may match after any '/' (exclusion's\n"
"                             default)\n"
"      --no-ignore-case       case matters (the default)\n"
"      --no-wildcards         a pattern is a plain string\n"
"      --no-wildcards-match-slash   wildcards stop at '/'\n"
"      --wildcards            patterns have wildcards (exclusion's default)\n"
"      --wildcards-match-slash   wildcards cross '/' (exclusion's default)\n"
"\n"
" Existing files:\n"
"\n"
"      --keep-directory-symlink   keep symbolic links to directories that are\n"
"                             there when unpacking\n"
"      --keep-newer-files     keep files that are newer than their archived copy\n"
"  -k, --keep-old-files       keep files that are there, as an error\n"
"      --no-overwrite-dir     keep the metadata of directories that are there\n"
"      --one-top-level[=DIR]  unpack into one directory (DIR, or one named after\n"
"                             the archive)\n"
"      --overwrite            write over files that are there\n"
"      --overwrite-dir        set the metadata of directories that are there\n"
"                             (the default)\n"
"      --recursive-unlink     empty a directory before unpacking one over it\n"
"      --remove-files         remove the files once they are archived\n"
"      --skip-old-files       keep files that are there, silently\n"
"  -U, --unlink-first         remove each file before unpacking it\n"
"  -W, --verify               check the archive once it is written\n"
"\n"
" Where unpacked data goes:\n"
"\n"
"      --ignore-command-error a command's failure is not tar's\n"
"      --no-ignore-command-error   a command's failure is tar's\n"
"  -O, --to-stdout            unpack files to standard output\n"
"      --to-command=COMMAND   unpack each file into COMMAND's input\n"
"\n"
" File attributes:\n"
"\n"
"      --atime-preserve[=METHOD]   keep the access times of archived files (by\n"
"                             setting them back, 'replace', the default, or\n"
"                             by never changing them, 'system')\n"
"      --clamp-mtime          use --mtime's date only for files newer than it\n"
"      --delay-directory-restore   set directories' times and modes only at the\n"
"                             end\n"
"      --group=NAME           record NAME as every file's group\n"
"      --group-map=FILE       map groups by FILE\n"
"      --mode=CHANGES         record every file's mode changed by CHANGES\n"
"      --mtime=DATE-OR-FILE   record DATE (or FILE's time) as every file's mtime\n"
"  -m, --touch                do not set unpacked files' modification times\n"
"      --no-delay-directory-restore\n"
"                             undo --delay-directory-restore\n"
"      --no-same-owner        unpacked files belong to you (the default unless\n"
"                             root)\n"
"      --no-same-permissions  unpacked modes lose the umask's bits (the default\n"
"                             unless root)\n"
"      --numeric-owner        use ids, not user and group names\n"
"      --owner=NAME           record NAME as every file's owner\n"
"      --owner-map=FILE       map owners by FILE\n"
"  -p, --preserve-permissions, --same-permissions\n"
"                             unpack modes exactly (the default for root)\n"
"      --same-owner           give unpacked files their archived owners (the\n"
"                             default for root)\n"
"      --sort=ORDER           the order of a directory's entries: none (the\n"
"                             default), name or inode\n"
"  -s, --preserve-order, --same-order\n"
"                             the names given are in the archive's order\n"
"\n"
" Extended attributes:\n"
"\n"
"      --acls                 keep POSIX ACLs\n"
"      --no-acls              do not keep POSIX ACLs\n"
"      --no-selinux           do not keep SELinux contexts\n"
"      --no-xattrs            do not keep extended attributes\n"
"      --selinux              keep SELinux contexts\n"
"      --xattrs               keep extended attributes\n"
"      --xattrs-exclude=MASK  leave out the extended attributes matching MASK\n"
"      --xattrs-include=MASK  keep only the extended attributes matching MASK\n"
"\n"
" The archive's device:\n"
"\n"
"      --force-local          the archive is local even with a colon in its name\n"
"  -f, --file=ARCHIVE         the archive file or device\n"
"  -F, --info-script=NAME, --new-volume-script=NAME\n"
"                             run NAME at the end of each volume (implies -M)\n"
"  -L, --tape-length=NUMBER   a new volume after NUMBER x 1024 bytes\n"
"  -M, --multi-volume         an archive of several volumes\n"
"      --rmt-command=COMMAND  the remote tape server to run\n"
"      --rsh-command=COMMAND  the remote shell to run\n"
"      --volno-file=FILE      keep the volume number in FILE\n"
"\n"
" Blocking:\n"
"\n"
"  -b, --blocking-factor=BLOCKS   records of BLOCKS x 512 bytes\n"
"  -B, --read-full-records    gather whole records when reading (for pipes)\n"
"  -i, --ignore-zeros         read past blocks of zeros (normally the end)\n"
"      --record-size=NUMBER   records of NUMBER bytes, a multiple of 512\n"
"\n"
" Format:\n"
"\n"
"  -H, --format=FORMAT        the format of a new archive\n"
"\n"
" FORMAT is one of:\n"
"    gnu                      GNU tar 1.13's\n"
"    oldgnu                   GNU tar's up to 1.12\n"
"    pax                      POSIX 1003.1-2001's (pax)\n"
"    posix                    the same as pax\n"
"    ustar                    POSIX 1003.1-1988's (ustar)\n"
"    v7                       the old V7 tar's\n"
"\n"
"      --old-archive, --portability\n"
"                             the same as --format=v7\n"
"      --pax-option=keyword[[:]=value][,keyword[[:]=value]]...\n"
"                             set pax keywords\n"
"      --posix                the same as --format=posix\n"
"  -V, --label=TEXT           give a new archive the volume name TEXT; when\n"
"                             reading, TEXT is a pattern the name must match\n"
"\n"
" Compression:\n"
"\n"
"  -a, --auto-compress        choose the compressor by the archive's suffix\n"
"  -I, --use-compress-program=PROG\n"
"                             compress with PROG (which must take -d)\n"
"  -j, --bzip2                compress with bzip2\n"
"  -J, --xz                   compress with xz\n"
"      --lzip                 compress with lzip\n"
"      --lzma                 compress with xz's lzma\n"
"      --lzop                 compress with lzop\n"
"      --no-auto-compress     do not choose by the suffix\n"
"      --zstd                 compress with zstd\n"
"  -z, --gzip, --gunzip, --ungzip   compress with gzip\n"
"  -Z, --compress, --uncompress   compress with compress\n"
"\n"
" Local files:\n"
"\n"
"      --backup[=CONTROL]     keep a backup of what is replaced (CONTROL: how)\n"
"      --hard-dereference     archive each hard link as the file it is\n"
"  -h, --dereference          archive what symbolic links point to\n"
"  -K, --starting-file=MEMBER-NAME\n"
"                             start at MEMBER-NAME when reading\n"
"      --newer-mtime=DATE     archive files whose data changed after DATE\n"
"  -N, --newer=DATE-OR-FILE, --after-date=DATE-OR-FILE\n"
"                             archive files newer than DATE-OR-FILE\n"
"      --one-file-system      stay on the starting file system\n"
"  -P, --absolute-names       keep leading '/'s and '..'s in names\n"
"      --suffix=STRING        the backup suffix ('~' unless\n"
"                             SIMPLE_BACKUP_SUFFIX says otherwise)\n"
"\n"
" Names:\n"
"\n"
"      --strip-components=NUMBER   drop NUMBER leading parts of names when\n"
"                             unpacking\n"
"      --transform=EXPRESSION, --xform=EXPRESSION\n"
"                             change names with sed's s command EXPRESSION\n"
"\n"
" What tar says:\n"
"\n"
"      --checkpoint[=NUMBER]  a progress line every NUMBER records (10)\n"
"      --checkpoint-action=ACTION   do ACTION at each checkpoint\n"
"      --full-time            times to the second (and below)\n"
"      --index-file=FILE      verbose output to FILE\n"
"  -l, --check-links          say so when not all of a file's links are archived\n"
"      --no-quote-chars=STRING   do not quote the characters in STRING\n"
"      --quote-chars=STRING   also quote the characters in STRING\n"
"      --quoting-style=STYLE  how names are quoted; the styles are below\n"
"  -R, --block-number         the block number with each message\n"
"                            \n"
"      --show-defaults        print this tar's defaults\n"
"      --show-omitted-dirs    name the directories passed over\n"
"      --show-snapshot-field-ranges\n"
"                             print the snapshot file's field ranges\n"
"      --show-transformed-names, --show-stored-names\n"
"                             show names as transformed or stored\n"
"      --totals[=SIGNAL]      the total bytes at the end; with SIGNAL, whenever\n"
"                             it arrives (SIGHUP, SIGQUIT, SIGINT, SIGUSR1 or\n"
"                             SIGUSR2, with or without the SIG)\n"
"      --utc                  times in UTC\n"
"  -v, --verbose              name each file as it goes\n"
"      --warning=KEYWORD      turn a warning on or off\n"
"  -w, --interactive, --confirmation\n"
"                             ask before each action\n"
"\n"
" Compatibility:\n"
"\n"
"  -o                         -c: --old-archive; otherwise --no-same-owner\n"
"\n"
" Other:\n"
"\n"
"  -?, --help                 this help\n"
"      --restrict             refuse some risky options\n"
"      --usage                a short usage line\n"
"      --version              the version\n"
"\n"
"An argument required by a long option is required by its short form too.\n"
"\n"
"Backups end in '~', or --suffix, or SIMPLE_BACKUP_SUFFIX. How they are made\n"
"comes from --backup or VERSION_CONTROL:\n"
"\n"
"  none, off       no backups\n"
"  t, numbered     numbered backups\n"
"  nil, existing   numbered where there are some already, else simple\n"
"  never, simple   simple backups\n"
"\n"
"The --quoting-style values are:\n"
"\n"
"  literal\n"
"  shell\n"
"  shell-always\n"
"  shell-escape\n"
"  shell-escape-always\n"
"  c\n"
"  c-maybe\n"
"  escape\n"
"  locale\n"
"  clocale\n"
"\n"
"*This* tar defaults to:\n"
"--format=gnu -f- -b20 --quoting-style=escape --rmt-command=/usr/sbin/rmt\n"
"--rsh-command=/usr/bin/rsh\n", stdout);
}

/* argp's --usage: every option, wrapped as argp wraps it */
static void tar_print_usage(void) {
    static const char *items[] = {
        "[-AcdrtuxGnSkUWOmpsMBiajJzZhPlRvwo?]", "[-g FILE]", "[-C DIR]", "[-T FILE]",
        "[-X FILE]", "[-f ARCHIVE]", "[-F NAME]", "[-L NUMBER]", "[-b BLOCKS]",
        "[-H FORMAT]", "[-V TEXT]", "[-I PROG]", "[-K MEMBER-NAME]", "[-N DATE-OR-FILE]",
        "[--catenate]", "[--concatenate]", "[--create]", "[--delete]", "[--diff]",
        "[--compare]", "[--append]", "[--test-label]", "[--list]", "[--update]",
        "[--extract]", "[--get]", "[--check-device]", "[--listed-incremental=FILE]",
        "[--incremental]", "[--hole-detection=TYPE]", "[--ignore-failed-read]",
        "[--level=NUMBER]", "[--no-check-device]", "[--no-seek]", "[--seek]",
        "[--occurrence[=NUMBER]]", "[--sparse-version=MAJOR[.MINOR]]", "[--sparse]",
        "[--add-file=FILE]", "[--directory=DIR]", "[--exclude=PATTERN]",
        "[--exclude-backups]", "[--exclude-caches]", "[--exclude-caches-all]",
        "[--exclude-caches-under]", "[--exclude-ignore=FILE]",
        "[--exclude-ignore-recursive=FILE]", "[--exclude-tag=FILE]",
        "[--exclude-tag-all=FILE]", "[--exclude-tag-under=FILE]", "[--exclude-vcs]",
        "[--exclude-vcs-ignores]", "[--no-null]", "[--no-recursion]", "[--no-unquote]",
        "[--no-verbatim-files-from]", "[--null]", "[--recursion]",
        "[--files-from=FILE]", "[--unquote]", "[--verbatim-files-from]",
        "[--exclude-from=FILE]", "[--anchored]", "[--ignore-case]", "[--no-anchored]",
        "[--no-ignore-case]", "[--no-wildcards]", "[--no-wildcards-match-slash]",
        "[--wildcards]", "[--wildcards-match-slash]", "[--keep-directory-symlink]",
        "[--keep-newer-files]", "[--keep-old-files]", "[--no-overwrite-dir]",
        "[--one-top-level[=DIR]]", "[--overwrite]", "[--overwrite-dir]",
        "[--recursive-unlink]", "[--remove-files]", "[--skip-old-files]",
        "[--unlink-first]", "[--verify]", "[--ignore-command-error]",
        "[--no-ignore-command-error]", "[--to-stdout]", "[--to-command=COMMAND]",
        "[--atime-preserve[=METHOD]]", "[--clamp-mtime]",
        "[--delay-directory-restore]", "[--group=NAME]", "[--group-map=FILE]",
        "[--mode=CHANGES]", "[--mtime=DATE-OR-FILE]", "[--touch]",
        "[--no-delay-directory-restore]", "[--no-same-owner]",
        "[--no-same-permissions]", "[--numeric-owner]", "[--owner=NAME]",
        "[--owner-map=FILE]", "[--preserve-permissions]", "[--same-permissions]",
        "[--same-owner]", "[--sort=ORDER]", "[--preserve-order]", "[--same-order]",
        "[--acls]", "[--no-acls]", "[--no-selinux]", "[--no-xattrs]", "[--selinux]",
        "[--xattrs]", "[--xattrs-exclude=MASK]", "[--xattrs-include=MASK]",
        "[--force-local]", "[--file=ARCHIVE]", "[--info-script=NAME]",
        "[--new-volume-script=NAME]", "[--tape-length=NUMBER]", "[--multi-volume]",
        "[--rmt-command=COMMAND]", "[--rsh-command=COMMAND]", "[--volno-file=FILE]",
        "[--blocking-factor=BLOCKS]", "[--read-full-records]", "[--ignore-zeros]",
        "[--record-size=NUMBER]", "[--format=FORMAT]", "[--  gnu]", "[--  oldgnu]",
        "[--  pax]", "[--  posix]", "[--  ustar]", "[--  v7]", "[--old-archive]",
        "[--portability]",
        "[--pax-option=keyword[[:]=value][,keyword[[:]=value]]...]", "[--posix]",
        "[--label=TEXT]", "[--auto-compress]", "[--use-compress-program=PROG]",
        "[--bzip2]", "[--xz]", "[--lzip]", "[--lzma]", "[--lzop]", "[--no-auto-compress]",
        "[--zstd]", "[--gzip]", "[--gunzip]", "[--ungzip]", "[--compress]", "[--uncompress]",
        "[--backup[=CONTROL]]", "[--hard-dereference]", "[--dereference]",
        "[--starting-file=MEMBER-NAME]", "[--newer-mtime=DATE]",
        "[--newer=DATE-OR-FILE]", "[--after-date=DATE-OR-FILE]",
        "[--one-file-system]", "[--absolute-names]", "[--suffix=STRING]",
        "[--strip-components=NUMBER]", "[--transform=EXPRESSION]",
        "[--xform=EXPRESSION]", "[--checkpoint[=NUMBER]]",
        "[--checkpoint-action=ACTION]", "[--full-time]", "[--index-file=FILE]",
        "[--check-links]", "[--no-quote-chars=STRING]", "[--quote-chars=STRING]",
        "[--quoting-style=STYLE]", "[--block-number]", "[--show-defaults]",
        "[--show-omitted-dirs]", "[--show-snapshot-field-ranges]",
        "[--show-transformed-names]", "[--show-stored-names]",
        "[--totals[=SIGNAL]]", "[--utc]", "[--verbose]", "[--warning=KEYWORD]",
        "[--interactive]", "[--confirmation]", "[--help]", "[--restrict]", "[--usage]",
        "[--version]", "[FILE]...", 0
    };
    /* argp wraps at 79 columns, continuation lines indented 12; an item
     * with an inner space ("[--  pax]") may break at that space */
    char line[256];
    int col;
    snprintf(line, sizeof line, "Usage: %s", su_short);
    fputs(line, stdout);
    col = (int)strlen(line);
    for (int i = 0; items[i]; i++) {
        const char *it = items[i];
        int len = (int)strlen(it);
        if (col + 1 + len > 79) {
            const char *sp = strstr(it, "  ") ? strchr(it, ' ') : 0;
            if (sp && col + 1 + (int)(sp - it) <= 79) {
                /* argp breaks at the item's own blank, keeping the rest */
                printf(" %.*s\n%12s%s", (int)(sp - it), it, "", sp + 1);
                col = 12 + (int)strlen(sp + 1);
                continue;
            }
            printf("\n%12s%s", "", it);
            col = 12 + len;
        } else {
            printf(" %s", it);
            col += 1 + len;
        }
    }
    putchar('\n');
}

/* ---- the command -------------------------------------------------------------------- */

static void tar_cleanup_children(void);

/* --one-top-level's directory from the archive's name: its directory
 * and a compression suffix, then ".tar", taken off; 0 when neither is
 * there */
static char *tar_deduce_top(const char *an) {
    if (!strcmp(an, "-")) return 0;
    const char *b = strrchr(an, '/');
    b = b ? b + 1 : an;
    char *d = tar_strdup(b);
    int cut = 0;
    static const char *sfx[] = { ".gz", ".tgz", ".taz", ".Z", ".taZ", ".bz2", ".tz2", ".tbz2", ".tbz",
                                 ".lz", ".lzma", ".tlz", ".lzo", ".xz", ".txz", ".zst", ".tzst", 0 };
    size_t dl = strlen(d);
    for (int i = 0; sfx[i]; i++) {
        size_t sl = strlen(sfx[i]);
        if (dl > sl && !strcmp(d + dl - sl, sfx[i])) { d[dl - sl] = 0; dl -= sl; cut = 1; break; }
    }
    if (dl > 4 && !strcmp(d + dl - 4, ".tar")) { d[dl - 4] = 0; cut = 1; }
    if (!cut) { free(d); return 0; }
    return d;
}

/* a positional option given after the last name: told at the end of -c
 * and -u ("-C 'D' has no effect", "--anchored has no effect") */
static void tar_note_trailing(int c, const char *arg) {
    const char *nm = 0;
    int witharg = 0;
    switch (c) {
    case 'C': nm = "-C"; witharg = 1; break;
    case 'X': nm = "--exclude-from"; witharg = 1; break;
    case TO_EXCLUDE: nm = "--exclude"; witharg = 1; break;
    case TO_EXCLUDE_TAG: nm = "--exclude-tag"; witharg = 1; break;
    case TO_EXCLUDE_TAG_ALL: nm = "--exclude-tag-all"; witharg = 1; break;
    case TO_EXCLUDE_TAG_UNDER: nm = "--exclude-tag-under"; witharg = 1; break;
    case TO_EXCLUDE_IGNORE: nm = "--exclude-ignore"; witharg = 1; break;
    case TO_EXCLUDE_IGNORE_REC: nm = "--exclude-ignore-recursive"; witharg = 1; break;
    case TO_WILDCARDS: nm = "--wildcards"; break;
    case TO_NO_WILDCARDS: nm = "--no-wildcards"; break;
    case TO_WILDCARDS_SLASH: nm = "--wildcards-match-slash"; break;
    case TO_NO_WILDCARDS_SLASH: nm = "--no-wildcards-match-slash"; break;
    case TO_IGNORE_CASE: nm = "--ignore-case"; break;
    case TO_NO_IGNORE_CASE: nm = "--no-ignore-case"; break;
    case TO_ANCHORED: nm = "--anchored"; break;
    case TO_NO_ANCHORED: nm = "--no-anchored"; break;
    case TO_EXCLUDE_VCS: nm = "--exclude-vcs"; break;
    case TO_EXCLUDE_VCS_IGNORES: nm = "--exclude-vcs-ignores"; break;
    case TO_EXCLUDE_BACKUPS: nm = "--exclude-backups"; break;
    case TO_EXCLUDE_CACHES: nm = "--exclude-caches"; break;
    case TO_EXCLUDE_CACHES_ALL: nm = "--exclude-caches-all"; break;
    case TO_EXCLUDE_CACHES_UNDER: nm = "--exclude-caches-under"; break;
    case TO_VERBATIM: nm = "--verbatim-files-from"; break;
    case TO_NO_VERBATIM: nm = "--no-verbatim-files-from"; break;
    case TO_UNQUOTE: nm = "--unquote"; break;
    case TO_NO_UNQUOTE: nm = "--no-unquote"; break;
    case TO_RECURSION: nm = "--recursion"; break;
    case TO_NO_RECURSION: nm = "--no-recursion"; break;
    case TO_NULL: nm = "--null"; break;
    case TO_NO_NULL: nm = "--no-null"; break;
    default: return;
    }
    struct tar_sb b = {0};
    if (witharg) tar_sb_printf(&b, "%s %s has no effect", nm, su_quote(arg));
    else tar_sb_printf(&b, "%s has no effect", nm);
    tar_trailing = (char **)tar_realloc(tar_trailing, (size_t)(tar_ntrailing + 1) * sizeof(char *));
    tar_trailing[tar_ntrailing++] = tar_sb_take(&b);
}

static void tar_report_trailing(void) {
    if (!tar_ntrailing) return;
    tar_err("The following options were used after non-option arguments.  These options are positional and affect only arguments that follow them.  Please, rearrange them properly.");
    for (int i = 0; i < tar_ntrailing; i++) tar_err("%s", tar_trailing[i]);
}

static int do_tar(int argc, char **argv) {
    /* defaults */
    tar_o.blocking = 20;
    tar_o.record_size = 20 * TAR_BLK;
    tar_o.recursion = 1;
    tar_o.unquote = 1;
    tar_o.wildcards = 0;
    tar_o.anchored = 1;
    tar_o.wslash = 1;
    tar_o.quoting = TAR_Q_ESCAPE;
    tar_o.check_device = 1;
    tar_o.vout = stdout;
    tar_warnings = ~0UL & ~((1UL << TW_DECOMPRESS) | (1UL << TW_EXISTING_FILE) |
                            (1UL << TW_XATTR_WRITE) | (1UL << TW_RECORD_SIZE) |
                            (1UL << TW_VERBOSE));
    tar_ar.fd = -1;
    {
        char b[4096];
        if (getcwd(b, sizeof b)) tar_start_dir = tar_strdup(b);
    }
    {
        /* names are quoted the escape way; '\\' always */
        memset(tar_o.quote_extra, 0, sizeof tar_o.quote_extra);
    }

    /* old-style options: "tar cvf a.tar x" is "tar -c -v -f a.tar x" */
    char **av = argv;
    int ac = argc;
    if (argc > 1 && argv[1][0] != '-' && argv[1][0]) {
        const char *letters = argv[1];
        int next = 2;
        char **nv = (char **)tar_alloc((size_t)(argc + (int)strlen(letters) + 2) * sizeof *nv);
        int n = 0;
        nv[n++] = argv[0];
        for (const char *p = letters; *p; p++) {
            char *opt = (char *)tar_alloc(3);
            opt[0] = '-'; opt[1] = *p; opt[2] = 0;
            nv[n++] = opt;
            if (strchr(tar_old_args, *p)) {
                if (next >= argc)
                    tar_usage_error("Old option '%c' requires an argument.", *p);
                nv[n++] = argv[next++];
            }
        }
        for (int i = next; i < argc; i++) nv[n++] = argv[i];
        nv[n] = 0;
        av = nv;
        ac = n;
    }

    int c, li = -1, names_seen = 0;
    for (;;) {
        su_optopt = -1;
        c = su_getopt_long(ac, av, tar_shortopts, tar_longopts, &li);
        if (c == -1) break;
        if (c == 1) {
            tar_nent_add(TNE_NAME, su_optarg);
            tar_have_names = 1;
            names_seen++;
            tar_ntrailing = 0;     /* what came before a name had its effect */
            continue;
        }
        if (names_seen) tar_note_trailing(c, su_optarg);
        if (c == '?' && su_optopt != -1) {
            /* getopt said why; argp adds its line */
            tar_try_help64();
        }
        if (c == ':') tar_try_help64();
        if (tar_option(c, su_optarg, 0, 0) < 0) tar_try_help64();
    }

    for (int i = su_optind; i < ac; i++) {
        tar_nent_add(TNE_NAME, av[i]);
        tar_have_names = 1;
        tar_ntrailing = 0;
    }

    if (tar_o.verbose)
        for (int i = 0; i < tar_ndate_msgs; i++) tar_warn("%s", tar_date_msgs[i]);
    if (tar_o.owner_map) tar_read_map(tar_o.owner_map, 0, &tar_umap, &tar_numap);
    if (tar_o.group_map) tar_read_map(tar_o.group_map, 1, &tar_gmap, &tar_ngmap);

    /* -K: the member to start at is also a name to look for */
    if (tar_o.starting_file && !(tar_o.cmd == TAR_C_CREATE || tar_o.cmd == TAR_C_APPEND || tar_o.cmd == TAR_C_UPDATE)) {
        tar_nent_add(TNE_NAME, tar_o.starting_file);
        tar_have_names = 1;
    }

    /* -o: what it means depends on the command */
    {
        int n = 0, tf = 0;
        for (int i = 0; i < tar_nnents; i++) {
            if (tar_nents[i].kind == TNE_NAME) n++;
            if (tar_nents[i].kind == TNE_TFILE) tf = 1;
        }
        tar_multi_names = n > 1 || tf;
    }
    if (tar_o.format == -1) {
        if (tar_o.cmd == TAR_C_CREATE) tar_o.format = TAR_F_V7;
        else { tar_o.format = TAR_F_DEFAULT; tar_o.same_owner = 0; tar_o.same_owner_set = 1; }
    }

    if (!tar_o.cmd)
        tar_usage_error("You must specify one of the '-Acdtrux', '--delete' or '--test-label' options");

    if (tar_o.narchives > 1 && !tar_o.multi_volume)
        tar_usage_error("Multiple archive files require '-M' option");
    if (tar_o.multi_volume && tar_o.label && tar_o.cmd == TAR_C_CREATE && tar_o.format == TAR_F_DEFAULT) { }

    /* the matching defaults for member names */
    int creating = tar_o.cmd == TAR_C_CREATE || tar_o.cmd == TAR_C_APPEND || tar_o.cmd == TAR_C_UPDATE;

    if (tar_o.compress && tar_o.multi_volume)
        tar_usage_error("Cannot use multi-volume compressed archives");
    if (tar_o.tape_length) {
        if (tar_o.tape_length < tar_o.record_size)
            tar_usage_error("Volume length cannot be less than record size");
        uintmax_t recs = (tar_o.tape_length + tar_o.record_size - 1) / tar_o.record_size;
        tar_mv_cap = recs * (tar_o.record_size / TAR_BLK);
    }
    if (tar_o.verify && tar_o.compress && tar_o.cmd == TAR_C_CREATE)
        tar_usage_error("Cannot verify compressed archives");
    if (tar_o.verify && !creating && tar_o.cmd != TAR_C_CAT && tar_o.cmd != TAR_C_DELETE)
        tar_usage_error("'--verify' cannot be used with '%s'", tar_cmd_letters[tar_o.cmd]);
    if (tar_o.compress && (tar_o.cmd == TAR_C_APPEND || tar_o.cmd == TAR_C_UPDATE || tar_o.cmd == TAR_C_DELETE))
        tar_usage_error("Cannot update compressed archives");
    if (tar_o.compress && tar_o.cmd == TAR_C_CAT)
        tar_usage_error("Cannot concatenate compressed archives");
    if (tar_o.occurrence) {
        if (tar_o.cmd != TAR_C_LIST && tar_o.cmd != TAR_C_EXTRACT && tar_o.cmd != TAR_C_DIFF &&
            tar_o.cmd != TAR_C_DELETE)
            tar_usage_error("'--occurrence' cannot be used with '%s'", tar_cmd_letters[tar_o.cmd]);
        if (!tar_have_names)
            tar_usage_error("--occurrence is meaningless without a file list");
    }
    if (tar_o.starting_file && creating)
        tar_usage_error("'--starting-file' cannot be used with '%s'", tar_cmd_letters[tar_o.cmd]);
    if (tar_o.one_top_set && !tar_o.one_top_level) {
        char *top = tar_deduce_top(tar_archive_name0());
        if (!top)
            tar_usage_error("Cannot deduce top-level directory name; please set it explicitly with --one-top-level=DIR");
        tar_o.one_top_level = top;
    }
    if (tar_o.one_top_set && tar_o.one_top_level && !*tar_o.one_top_level) tar_o.one_top_set = 0;
    if ((tar_o.sparse || tar_o.incremental) && (tar_o.cmd == TAR_C_CREATE || tar_o.cmd == TAR_C_APPEND || tar_o.cmd == TAR_C_UPDATE) &&
        (tar_o.format == TAR_F_V7 || tar_o.format == TAR_F_USTAR))
        tar_usage_error("GNU features wanted on incompatible archive format");
    if (tar_o.label && tar_o.cmd == TAR_C_CREATE) {
        if (strlen(tar_o.label) > 99)
            tar_usage_error("%s: Volume label is too long (limit is 99 bytes)", tar_qc(tar_o.label));
        if (tar_o.format == TAR_F_V7 || tar_o.format == TAR_F_USTAR)
            tar_usage_error("GNU features wanted on incompatible archive format");
    }
    if (tar_o.pax_set && tar_o.format == TAR_F_DEFAULT) tar_o.format = TAR_F_POSIX;
    if (tar_o.pax_set && creating) {
        int f = tar_o.format == TAR_F_DEFAULT ? TAR_F_GNU : tar_o.format;
        if (f != TAR_F_POSIX)
            tar_usage_error("--pax-option can be used only on POSIX archives");
    }
    if (tar_o.level_set && !tar_o.listed_incr && tar_o.cmd == TAR_C_CREATE)
        tar_warn("--level is meaningless without --listed-incremental");
    if (tar_o.cmd == TAR_C_CREATE && !tar_have_names && !tar_o.listed_incr)
        tar_usage_error("Cowardly refusing to create an empty archive");

    /* defaults the command decides */
    if (!tar_o.same_owner_set) tar_o.same_owner = geteuid() == 0;
    if (!tar_o.same_perms_set) tar_o.same_perms = geteuid() == 0;
    if (!strcmp(tar_archive_name0(), "-") && (tar_o.cmd == TAR_C_CREATE)) tar_o.vout = stderr;
    if (tar_o.to_stdout && tar_o.cmd == TAR_C_EXTRACT) tar_o.vout = stderr;
    if (tar_o.index_file) {
        FILE *f = fopen(tar_o.index_file, "w");
        if (!f) {
            int e = errno;
            tar_fatal("%s: Cannot open: %s", tar_qc(tar_o.index_file), strerror(e));
        }
        tar_o.vout = f;
    }
    if (!tar_o.verbose && tar_warn_explicit[TW_VERBOSE] && tar_warning_on(TW_VERBOSE)) { }
    if (tar_o.verbose && !tar_warn_explicit[TW_DECOMPRESS]) { }

    int st = tar_run();

    if (tar_o.index_file && tar_o.vout) fclose(tar_o.vout);
    if (st == TAR_EXIT_FAILURE) {
        fflush(stdout);
        fprintf(stderr, "%s: Exiting with failure status due to previous errors\n", su_name);
    }
    fflush(stdout);
    return st;
}

/* @@END@@ */
