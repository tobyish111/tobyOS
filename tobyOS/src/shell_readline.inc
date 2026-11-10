/* shell_readline.inc -- readline's key bindings, variables and functions as
 * bash's `bind` builtin sees them (and `compgen -A binding`).
 *
 * Included once by shell.c, after shell_htab.inc; everything is static.
 *
 * THE SPEC IS READLINE 8.2 AS BASH 5.2.15 USES IT (lib/readline/bind.c,
 * keymaps.c; bash's builtins/bind.def and the bind -x half of bashline.c),
 * read as a description of behaviour -- this is tsh's own code. tsh had a
 * `bind` that knew a handful of options and printed its own words (Oils
 * 0658 0660-0665). What readline keeps, and so what this keeps:
 *
 *   - KEYMAPS: 257 slots each (256 keys and ANYOTHERKEY), a slot being a
 *     function, a macro, or another keymap. emacs-standard holds emacs-meta
 *     at ESC and emacs-ctlx at C-x; vi-move and vi-insert are separate. The
 *     defaults come from shell_rltab.inc -- the guest bash's own `bind -p`.
 *   - Every LISTING walks a keymap key by key, 0 to 256, descending into a
 *     keymap where one is bound, which fixes the order bash prints in.
 *   - FUNCTIONS by name; names readline maps to one function share an id.
 *     Before line editing is set up only readline's own 142 exist; bash
 *     adds 31 when it sets readline up, which `bind` does on first use.
 *   - VARIABLES: 34 booleans and the string ones, with the guest's
 *     defaults; `set NAME VALUE` lines change them.
 *   - `bind -x` keeps the command as a macro in a parallel command keymap
 *     and binds the key to an unnamed function, so `bind -p` never lists
 *     it and `bind -X` dumps the command keymap.
 *
 * The line editor (programs/tsh/lineedit.c) does not dispatch through these
 * keymaps yet; what is here is the state `bind` reads and writes.
 */

#include "shell_rltab.inc"

#define RL_KEYMAP_SIZE  257
#define RL_ANYOTHERKEY  256
#define RL_ESC          27
#define RL_RUBOUT       127

enum { RL_ISFUNC = 0, RL_ISKMAP = 1, RL_ISMACR = 2 };
/* Functions without a name in the table. */
#define RL_FN_NONE  (-1)            /* unbound */
#define RL_FN_UNIX  (-2)            /* bash_execute_unix_command (bind -x) */
#define RL_FN_NULL  (-3)            /* _rl_null_function: an overridden keymap */

struct rl_kme {
    unsigned char type;
    short fn;                       /* RL_ISFUNC */
    struct rl_kme *map;             /* RL_ISKMAP: RL_KEYMAP_SIZE slots */
    char *macro;                    /* RL_ISMACR: translated bytes */
};

static struct rl_kme *g_rl_emacs, *g_rl_emacs_meta, *g_rl_emacs_ctlx;
static struct rl_kme *g_rl_vi_move, *g_rl_vi_ins;
static struct rl_kme *g_rl_keymap;          /* the current one */
static bool g_rl_vi_mode;                   /* editing-mode vi */
static bool g_rl_initialized;               /* bash's initialize_readline ran */
/* bind -x: the command keymaps, parallel to the editing ones. */
static struct rl_kme *g_rl_xemacs, *g_rl_xvi_ins, *g_rl_xvi_move;

static struct rl_kme *rl_new_keymap(void) {
    struct rl_kme *m = (struct rl_kme *)kmalloc(RL_KEYMAP_SIZE * sizeof *m);
    if (!m) return 0;
    for (int i = 0; i < RL_KEYMAP_SIZE; i++) {
        m[i].type = RL_ISFUNC;
        m[i].fn = RL_FN_NONE;
        m[i].map = 0;
        m[i].macro = 0;
    }
    return m;
}

static void rl_free_keymap(struct rl_kme *m) {
    if (!m) return;
    for (int i = 0; i < RL_KEYMAP_SIZE; i++) {
        if (m[i].type == RL_ISKMAP) rl_free_keymap(m[i].map);
        else if (m[i].type == RL_ISMACR) kfree(m[i].macro);
    }
    kfree(m);
}

/* ---- variables ---------------------------------------------------------- */

struct rl_boolvar { const char *name; bool *value; };
static bool g_rlv_bool[34] = {
    /* the guest's defaults, in boolean_varlist order */
    [0] = true,  /* bind-tty-special-chars */
    [7] = true,  /* convert-meta */
    [9] = true,  /* echo-control-characters */
    [13] = true, /* enable-meta-key */
    [18] = true, /* mark-directories */
    [21] = true, /* match-hidden-files */
    [25] = true, /* page-completions */
    [26] = true, /* prefer-visible-bell */
};
static const char *const g_rlv_bool_names[] = {
    "bind-tty-special-chars", "blink-matching-paren", "byte-oriented",
    "colored-completion-prefix", "colored-stats", "completion-ignore-case",
    "completion-map-case", "convert-meta", "disable-completion",
    "echo-control-characters", "enable-active-region",
    "enable-bracketed-paste", "enable-keypad", "enable-meta-key",
    "expand-tilde", "history-preserve-point", "horizontal-scroll-mode",
    "input-meta", "mark-directories", "mark-modified-lines",
    "mark-symlinked-directories", "match-hidden-files",
    "menu-complete-display-prefix", "meta-flag", "output-meta",
    "page-completions", "prefer-visible-bell",
    "print-completions-horizontally", "revert-all-at-newline",
    "show-all-if-ambiguous", "show-all-if-unmodified",
    "show-mode-in-prompt", "skip-completed-text", "visible-stats", 0
};
#define RLV_CONVERT_META   7
#define RLV_ACTIVE_REGION  10
#define RLV_BRACKETED      11
#define RLV_INPUT_META     17
#define RLV_META_FLAG      23
#define RLV_VISIBLE_BELL   26

/* meta-flag is input-meta under another name: one value */
static bool *rl_boolvar_slot(int i) {
    return &g_rlv_bool[i == RLV_META_FLAG ? RLV_INPUT_META : i];
}

enum { RL_BELL_NONE, RL_BELL_AUDIBLE, RL_BELL_VISIBLE };
static int   g_rlv_bell = RL_BELL_AUDIBLE;
static char *g_rlv_comment_begin;           /* 0: "#" */
static int   g_rlv_comp_width = -1;
static int   g_rlv_comp_prefix_len = 0;
static int   g_rlv_comp_query = 100;
static char *g_rlv_emacs_mode_str;          /* 0: "@" */
static int   g_rlv_hist_size = 0;           /* not stifled */
static char *g_rlv_isearch_term;            /* 0: unset, not listed */
static int   g_rlv_seq_timeout = 500;
static char *g_rlv_vi_cmd_str;              /* 0: "(cmd)" */
static char *g_rlv_vi_ins_str;              /* 0: "(ins)" */
static char *g_rlv_region_start, *g_rlv_region_end;

static int rl_stricmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : (unsigned char)*a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : (unsigned char)*b;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

static int rl_strnicmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : (unsigned char)a[i];
        int cb = (b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : (unsigned char)b[i];
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

/* ---- setting up ---------------------------------------------------------- */

static void rl_setup(void) {
    if (g_rl_emacs) return;
    g_rl_emacs = rl_new_keymap();
    g_rl_emacs_meta = rl_new_keymap();
    g_rl_emacs_ctlx = rl_new_keymap();
    g_rl_vi_move = rl_new_keymap();
    g_rl_vi_ins = rl_new_keymap();
    if (!g_rl_emacs || !g_rl_emacs_meta || !g_rl_emacs_ctlx || !g_rl_vi_move ||
        !g_rl_vi_ins)
        return;
    g_rl_emacs[RL_ESC].type = RL_ISKMAP;
    g_rl_emacs[RL_ESC].map = g_rl_emacs_meta;
    g_rl_emacs[0x18].type = RL_ISKMAP;
    g_rl_emacs[0x18].map = g_rl_emacs_ctlx;
    for (int i = 0; g_rl_defbinds[i].keys; i++) {
        const struct rl_defbind *d = &g_rl_defbinds[i];
        struct rl_kme *m = d->map == RL_MAP_EMACS ? g_rl_emacs :
                           d->map == RL_MAP_VI_MOVE ? g_rl_vi_move : g_rl_vi_ins;
        const unsigned char *k = (const unsigned char *)d->keys;
        int n = d->len;
        /* every key but the last leads into a keymap */
        int last = d->any ? n : n - 1;
        for (int j = 0; j < last; j++) {
            if (m[k[j]].type != RL_ISKMAP) {
                struct rl_kme *nm = rl_new_keymap();
                if (!nm) return;
                m[k[j]].type = RL_ISKMAP;
                m[k[j]].map = nm;
            }
            m = m[k[j]].map;
        }
        int slot = d->any ? RL_ANYOTHERKEY : k[n - 1];
        if (m[slot].type == RL_ISKMAP) continue;
        m[slot].type = RL_ISFUNC;
        m[slot].fn = d->fn;
    }
    g_rl_keymap = g_rl_emacs;
}

static bool rl_fn_exists(int i) {
    return g_rl_fnames[i].preinit || g_rl_initialized;
}

/* bash's initialize_readline, as far as `bind` can see it. */
static void rl_bash_init(void) {
    rl_setup();
    g_rl_initialized = true;
}

/* rl_named_function: case-insensitive, as readline's is. -1: none. */
static int rl_named_function(const char *name) {
    for (int i = 0; g_rl_fnames[i].name; i++)
        if (rl_fn_exists(i) && rl_stricmp(g_rl_fnames[i].name, name) == 0)
            return g_rl_fnames[i].fn;
    return RL_FN_NONE;
}

/* The keymap names, in readline's table order (the first name of a map is
 * the one it is reported under). */
static struct rl_kme *rl_keymap_by_index(int i) {
    switch (i) {
    case 0: case 1: return g_rl_emacs;
    case 2: return g_rl_emacs_meta;
    case 3: return g_rl_emacs_ctlx;
    case 4: case 5: case 6: return g_rl_vi_move;
    case 7: return g_rl_vi_ins;
    default: return 0;
    }
}
static const char *const g_rl_keymap_names[] = {
    "emacs", "emacs-standard", "emacs-meta", "emacs-ctlx", "vi", "vi-move",
    "vi-command", "vi-insert", 0
};

static struct rl_kme *rl_get_keymap_by_name(const char *name) {
    for (int i = 0; g_rl_keymap_names[i]; i++)
        if (rl_stricmp(name, g_rl_keymap_names[i]) == 0) return rl_keymap_by_index(i);
    return 0;
}

static const char *rl_get_keymap_name(struct rl_kme *m) {
    for (int i = 0; g_rl_keymap_names[i]; i++)
        if (rl_keymap_by_index(i) == m) return g_rl_keymap_names[i];
    return 0;
}

/* ---- key sequences ------------------------------------------------------ */

/* rl_translate_keyseq: the printable form ("\C-x\M-a") to the bytes. */
static int rl_translate_keyseq(const char *seq, struct shell_sb *out) {
    bool ctl = false, meta = false;
    for (size_t i = 0; ; i++) {
        unsigned c = (unsigned char)seq[i];
        if (!c && !ctl && !meta) break;
        if (c == '\\' && seq[i + 1]) {
            c = (unsigned char)seq[++i];
            if (c == 'C' && seq[i + 1] == '-') { i++; ctl = true; continue; }
            if (c == 'M' && seq[i + 1] == '-') { i++; meta = true; continue; }
            switch (c) {
            case 'a': c = 7; break;
            case 'b': c = 8; break;
            case 'd': c = RL_RUBOUT; break;
            case 'e': c = RL_ESC; break;
            case 'f': c = 12; break;
            case 'n': c = 10; break;
            case 'r': c = 13; break;
            case 't': c = 9; break;
            case 'v': c = 11; break;
            case '\\': c = '\\'; break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                i++;
                int t = 2;
                c -= '0';
                while (seq[i] >= '0' && seq[i] <= '7' && t--) c = c * 8 + (unsigned)(seq[i++] - '0');
                i--;
                c &= 0xff;
                break;
            }
            case 'x': {
                i++;
                int t = 2;
                unsigned v = 0;
                while (t > 0) {
                    char h = seq[i];
                    unsigned d;
                    if (h >= '0' && h <= '9') d = (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
                    else break;
                    v = v * 16 + d;
                    i++;
                    t--;
                }
                c = t == 2 ? 'x' : (v & 0xff);
                i--;
                break;
            }
            default:
                c &= 0xff;
                break;
            }
        }
        if (ctl) {
            c = (c == '?') ? RL_RUBOUT : ((c >= 'a' && c <= 'z' ? c - 32 : c) & 0x1f);
            ctl = false;
        }
        if (meta) { c |= 0x80; meta = false; }
        if ((c & 0x80) && g_rlv_bool[RLV_CONVERT_META]) {
            if (sb_putc(out, (char)RL_ESC) < 0 || sb_putc(out, (char)(c & 0x7f)) < 0) return -1;
        } else if (sb_putc(out, (char)c) < 0) {
            return -1;
        }
        if (!seq[i]) break;
    }
    return 0;
}

/* _rl_get_keyname: one key's printable name. */
static void rl_keyname(int key, struct shell_sb *b) {
    int c = key;
    if (c == RL_ESC) { (void)sb_puts(b, "\\e"); return; }
    if (c == RL_RUBOUT) { (void)sb_puts(b, "\\C-?"); return; }
    if (c < 32) {
        (void)sb_puts(b, "\\C-");
        c = c + 64;
        if (c >= 'A' && c <= 'Z') c += 32;
    }
    if (c >= 128 && c <= 159) {
        (void)sb_putc(b, '\\');
        (void)sb_putc(b, '2');
        c -= 128;
        (void)sb_putc(b, (char)('0' + c / 8));
        c = (c % 8) + '0';
    } else if (c >= 160) {
        unsigned char u = (unsigned char)c;
        (void)sb_putc(b, '\\');
        (void)sb_putc(b, (char)('0' + ((u >> 6) & 7)));
        (void)sb_putc(b, (char)('0' + ((u >> 3) & 7)));
        c = (c % 8) + '0';
    }
    if (c == '\\' || c == '"') (void)sb_putc(b, '\\');
    (void)sb_putc(b, (char)c);
}

/* rl_invoking_keyseqs_in_map: every sequence that reaches FN (or, for a
 * macro, the very macro) in MAP, in readline's order. */
static void rl_invoking(int type, short fn, const char *macro, struct rl_kme *map,
                        struct sh_strv *out) {
    for (int key = 0; key < RL_KEYMAP_SIZE; key++) {
        struct rl_kme *e = &map[key];
        if (e->type == RL_ISKMAP) {
            if (!e->map) continue;
            struct sh_strv seqs = { 0, 0, 0 };
            rl_invoking(type, fn, macro, e->map, &seqs);
            for (int i = 0; i < seqs.n; i++) {
                SH_SB(k);
                if (key == RL_ESC) {
                    (void)sb_puts(&k, (g_rlv_bool[RLV_CONVERT_META] && map[RL_ESC].type == RL_ISKMAP)
                                          ? "\\M-" : "\\e");
                } else {
                    int c = key;
                    if (c < 32 || c == RL_RUBOUT) {
                        (void)sb_puts(&k, "\\C-");
                        c = (c == RL_RUBOUT) ? '?' : c + 64;
                        if (c >= 'A' && c <= 'Z') c += 32;
                    }
                    if (c == '\\' || c == '"') (void)sb_putc(&k, '\\');
                    (void)sb_putc(&k, (char)c);
                }
                (void)sb_puts(&k, seqs.v[i]);
                (void)sv_add(out, k.p);
            }
            sv_free(&seqs);
            continue;
        }
        if (e->type != type) continue;
        if (type == RL_ISFUNC ? e->fn == fn : e->macro == macro) {
            SH_SB(k);
            rl_keyname(key, &k);
            (void)sv_add(out, k.p);
        }
    }
}

/* _rl_untranslate_macro_value */
static void rl_untranslate(const char *s, struct shell_sb *b) {
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c >= 128) {
            (void)sb_puts(b, "\\M-");
            c &= 0x7f;
        } else if (c == RL_ESC) {
            (void)sb_putc(b, '\\');
            c = 'e';
        } else if (c < 32) {
            (void)sb_puts(b, "\\C-");
            c = c + 64;
            if (c >= 'A' && c <= 'Z') c += 32;
        } else if (c == RL_RUBOUT) {
            (void)sb_puts(b, "\\C-");
            c = '?';
        }
        if (c == RL_ESC) {
            (void)sb_putc(b, '\\');
            c = 'e';
        } else if (c == '\\' || c == '"') {
            (void)sb_putc(b, '\\');
        }
        (void)sb_putc(b, (char)c);
    }
}

/* rl_generic_bind: TYPE (function FN or macro MACRO, owned) at SEQ. */
static int rl_generic_bind(int type, const char *seq, short fn, char *macro,
                           struct rl_kme *map) {
    if (!seq || !*seq) { kfree(macro); return -1; }
    SH_SB(keys);
    if (rl_translate_keyseq(seq, &keys) < 0) { kfree(macro); return -1; }
    int n = (int)keys.len;
    if (n == 0) { kfree(macro); return -1; }
    struct rl_kme *prevmap = map;
    int prevkey = (unsigned char)keys.p[0];
    int ic = 0;
    struct rl_kme *binding_map = map;
    for (int i = 0; i < n; i++) {
        if (i > 0) prevkey = ic;
        ic = (unsigned char)keys.p[i];
        if (i + 1 < n) {
            struct rl_kme k = { RL_ISFUNC, RL_FN_NONE, 0, 0 };
            if (map[ic].type != RL_ISKMAP) {
                k = map[ic];
                struct rl_kme *nm = rl_new_keymap();
                if (!nm) { kfree(macro); return -1; }
                map[ic].type = RL_ISKMAP;
                map[ic].map = nm;
                map[ic].macro = 0;
            }
            prevmap = map;
            map = map[ic].map;
            /* a shadowed function or macro moves to ANYOTHERKEY */
            if ((k.type == RL_ISFUNC && k.fn != RL_FN_NONE &&
                 k.fn != rl_named_function("do-lowercase-version")) ||
                k.type == RL_ISMACR) {
                map[RL_ANYOTHERKEY] = k;
            }
        } else {
            if (map[ic].type == RL_ISKMAP) {
                prevmap = map;
                map = map[ic].map;
                ic = RL_ANYOTHERKEY;
                if (type == RL_ISFUNC && fn == RL_FN_NONE) fn = RL_FN_NULL;
            }
            if (map[ic].type == RL_ISMACR) kfree(map[ic].macro);
            map[ic].type = (unsigned char)type;
            map[ic].fn = type == RL_ISFUNC ? fn : RL_FN_NONE;
            map[ic].macro = type == RL_ISMACR ? macro : 0;
            map[ic].map = 0;
        }
        binding_map = map;
    }
    /* Unbinding the last key of a keymap that only shadowed a function
     * gives the function back (one level, as readline does). */
    if (type == RL_ISFUNC && fn == RL_FN_NONE &&
        prevmap[prevkey].type == RL_ISKMAP && prevmap[prevkey].map == binding_map &&
        binding_map[RL_ANYOTHERKEY].type == RL_ISFUNC) {
        bool empty = true;
        for (int i = 0; i < RL_ANYOTHERKEY && empty; i++)
            if (binding_map[i].type != RL_ISFUNC || binding_map[i].fn != RL_FN_NONE)
                empty = false;
        if (empty) {
            short sfn = binding_map[RL_ANYOTHERKEY].fn;
            if (binding_map != g_rl_emacs_meta && binding_map != g_rl_emacs_ctlx)
                rl_free_keymap(binding_map);
            prevmap[prevkey].type = RL_ISFUNC;
            prevmap[prevkey].fn = sfn;
            prevmap[prevkey].map = 0;
        }
    }
    return 0;
}

/* rl_function_of_keyseq_len, for bind -r's "what was bound here". */
static const struct rl_kme *rl_entry_of_keys(const char *keys, int len,
                                             struct rl_kme *map) {
    for (int i = 0; i < len; i++) {
        int ic = (unsigned char)keys[i];
        if (ic >= 128 && g_rlv_bool[RLV_CONVERT_META]) {
            if (map[RL_ESC].type == RL_ISKMAP) {
                map = map[RL_ESC].map;
                ic &= 0x7f;
            } else {
                return &map[RL_ESC];
            }
        }
        if (map[ic].type == RL_ISKMAP) {
            if (i + 1 == len) return &map[ic];
            map = map[ic].map;
        } else if (i + 1 < len) {
            return 0;
        } else {
            return &map[ic];
        }
    }
    return 0;
}

/* ---- the dumpers ---------------------------------------------------------- */

/* bind -l, and compgen -A binding: the names, sorted. */
static void rl_funmap_names(struct sh_strv *out) {
    for (int i = 0; g_rl_fnames[i].name; i++)
        if (rl_fn_exists(i)) (void)sv_add(out, g_rl_fnames[i].name);
}

static void rl_function_dumper(bool readably) {
    shell_printf("\n");
    for (int i = 0; g_rl_fnames[i].name; i++) {
        if (!rl_fn_exists(i)) continue;
        const char *name = g_rl_fnames[i].name;
        struct sh_strv inv = { 0, 0, 0 };
        rl_invoking(RL_ISFUNC, g_rl_fnames[i].fn, 0, g_rl_keymap, &inv);
        if (readably) {
            if (!inv.n) shell_printf("# %s (not bound)\n", name);
            for (int j = 0; j < inv.n; j++) shell_printf("\"%s\": %s\n", inv.v[j], name);
        } else if (!inv.n) {
            shell_printf("%s is not bound to any keys\n", name);
        } else {
            SH_SB(l);
            (void)sb_puts(&l, name);
            (void)sb_puts(&l, " can be found on ");
            int j;
            for (j = 0; j < inv.n && j < 5; j++) {
                (void)sb_putc(&l, '"');
                (void)sb_puts(&l, inv.v[j]);
                (void)sb_puts(&l, j + 1 < inv.n ? "\", " : "\".\n");
            }
            if (j == 5 && j < inv.n) (void)sb_puts(&l, "...\n");
            shell_printf("%s", l.p);
        }
        sv_free(&inv);
    }
}

static void rl_macro_dumper_in(bool readably, struct rl_kme *map, const char *prefix) {
    for (int key = 0; key < RL_KEYMAP_SIZE; key++) {
        struct rl_kme *e = &map[key];
        if (e->type == RL_ISMACR) {
            SH_SB(kn);
            rl_keyname(key, &kn);
            SH_SB(v);
            rl_untranslate(e->macro ? e->macro : "", &v);
            if (readably) shell_printf("\"%s%s\": \"%s\"\n", prefix, kn.p, v.p);
            else shell_printf("%s%s outputs %s\n", prefix, kn.p, v.p);
        } else if (e->type == RL_ISKMAP && e->map) {
            SH_SB(kn);
            (void)sb_puts(&kn, prefix);
            if (key == RL_ESC) (void)sb_puts(&kn, "\\e");
            else rl_keyname(key, &kn);
            rl_macro_dumper_in(readably, e->map, kn.p);
        }
    }
}

static const char *rl_string_var(const char *name, char *num, size_t cap) {
    if (!rl_stricmp(name, "active-region-end-color") ||
        !rl_stricmp(name, "active-region-start-color")) {
        const char *v = name[14] == 'e' ? g_rlv_region_end : g_rlv_region_start;
        return v;
    }
    if (!rl_stricmp(name, "bell-style"))
        return g_rlv_bell == RL_BELL_NONE ? "none" :
               g_rlv_bell == RL_BELL_VISIBLE ? "visible" : "audible";
    if (!rl_stricmp(name, "comment-begin")) return g_rlv_comment_begin ? g_rlv_comment_begin : "#";
    if (!rl_stricmp(name, "completion-display-width")) { ksnprintf(num, cap, "%d", g_rlv_comp_width); return num; }
    if (!rl_stricmp(name, "completion-prefix-display-length")) { ksnprintf(num, cap, "%d", g_rlv_comp_prefix_len); return num; }
    if (!rl_stricmp(name, "completion-query-items")) { ksnprintf(num, cap, "%d", g_rlv_comp_query); return num; }
    if (!rl_stricmp(name, "editing-mode")) return g_rl_vi_mode ? "vi" : "emacs";
    if (!rl_stricmp(name, "emacs-mode-string")) return g_rlv_emacs_mode_str ? g_rlv_emacs_mode_str : "@";
    if (!rl_stricmp(name, "history-size")) { ksnprintf(num, cap, "%d", g_rlv_hist_size); return num; }
    if (!rl_stricmp(name, "isearch-terminators")) {
        if (!g_rlv_isearch_term) return 0;
        SH_SB(u);
        rl_untranslate(g_rlv_isearch_term, &u);
        ksnprintf(num, cap, "%s", u.p);
        return num;
    }
    if (!rl_stricmp(name, "keymap")) {
        const char *k = rl_get_keymap_name(g_rl_keymap);
        return k ? k : (g_rl_vi_mode ? "vi" : "emacs");
    }
    if (!rl_stricmp(name, "keyseq-timeout")) { ksnprintf(num, cap, "%d", g_rlv_seq_timeout); return num; }
    if (!rl_stricmp(name, "vi-cmd-mode-string")) return g_rlv_vi_cmd_str ? g_rlv_vi_cmd_str : "(cmd)";
    if (!rl_stricmp(name, "vi-ins-mode-string")) return g_rlv_vi_ins_str ? g_rlv_vi_ins_str : "(ins)";
    return 0;
}

static const char *const g_rlv_string_names[] = {
    "active-region-end-color", "active-region-start-color", "bell-style",
    "comment-begin", "completion-display-width",
    "completion-prefix-display-length", "completion-query-items",
    "editing-mode", "emacs-mode-string", "history-size",
    "isearch-terminators", "keymap", "keyseq-timeout", "vi-cmd-mode-string",
    "vi-ins-mode-string", 0
};

static void rl_variable_dumper(bool readably) {
    for (int i = 0; g_rlv_bool_names[i]; i++) {
        bool v = *rl_boolvar_slot(i);
        if (readably) shell_printf("set %s %s\n", g_rlv_bool_names[i], v ? "on" : "off");
        else shell_printf("%s is set to `%s'\n", g_rlv_bool_names[i], v ? "on" : "off");
    }
    for (int i = 0; g_rlv_string_names[i]; i++) {
        char num[64];
        const char *v = rl_string_var(g_rlv_string_names[i], num, sizeof num);
        if (!v) continue;
        if (readably) shell_printf("set %s %s\n", g_rlv_string_names[i], v);
        else shell_printf("%s is set to `%s'\n", g_rlv_string_names[i], v);
    }
}

/* ---- setting variables, and the init-file language -------------------- */

static const char *g_rl_init_file;          /* reading this file, at ... */
static int g_rl_init_line;

__attribute__((format(printf, 1, 2)))
static void rl_init_error(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_rl_init_file)
        kprintf_raw("readline: %s: line %d: %s\n", g_rl_init_file, g_rl_init_line, buf);
    else
        kprintf_raw("readline: %s\n", buf);
}

static int rl_find_bool(const char *name) {
    for (int i = 0; g_rlv_bool_names[i]; i++)
        if (!rl_stricmp(name, g_rlv_bool_names[i])) return i;
    return -1;
}

static int rl_find_string(const char *name) {
    for (int i = 0; g_rlv_string_names[i]; i++)
        if (!rl_stricmp(name, g_rlv_string_names[i])) return i;
    return -1;
}

static int rl_atoi(const char *s) {
    long v = 0;
    bool neg = false;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return (int)(neg ? -v : v);
}

/* A mode string or isearch-terminators: the value translated as a key
 * sequence. */
static char *rl_translated_copy(const char *v) {
    SH_SB(t);
    if (rl_translate_keyseq(v, &t) < 0) return 0;
    return sb_steal(&t);
}

/* rl_variable_bind. 0 done, nonzero "could not set". */
static int rl_variable_bind(const char *name, const char *value) {
    int b = rl_find_bool(name);
    if (b >= 0) {
        bool on = !value || !*value || !rl_stricmp(value, "on") ||
                  (value[0] == '1' && value[1] == '\0');
        *rl_boolvar_slot(b) = on;
        if (b == RLV_VISIBLE_BELL) g_rlv_bell = on ? RL_BELL_VISIBLE : RL_BELL_AUDIBLE;
        else if (b == RLV_BRACKETED) g_rlv_bool[RLV_ACTIVE_REGION] = on;
        return 0;
    }
    int s = rl_find_string(name);
    if (s < 0) {
        rl_init_error("%s: unknown variable name", name);
        return 0;
    }
    const char *n = g_rlv_string_names[s];
    int rc = 0;
    if (!rl_stricmp(n, "bell-style")) {
        if (!value || !*value) g_rlv_bell = RL_BELL_AUDIBLE;
        else if (!rl_stricmp(value, "none") || !rl_stricmp(value, "off")) g_rlv_bell = RL_BELL_NONE;
        else if (!rl_stricmp(value, "audible") || !rl_stricmp(value, "on")) g_rlv_bell = RL_BELL_AUDIBLE;
        else if (!rl_stricmp(value, "visible")) g_rlv_bell = RL_BELL_VISIBLE;
        else rc = 1;
    } else if (!rl_stricmp(n, "comment-begin")) {
        if (value && *value) { kfree(g_rlv_comment_begin); g_rlv_comment_begin = shell_strdup(value); }
        else rc = 1;
    } else if (!rl_stricmp(n, "completion-display-width")) {
        g_rlv_comp_width = value && *value ? rl_atoi(value) : -1;
    } else if (!rl_stricmp(n, "completion-prefix-display-length")) {
        int v = value && *value ? rl_atoi(value) : 0;
        g_rlv_comp_prefix_len = v < 0 ? 0 : v;
    } else if (!rl_stricmp(n, "completion-query-items")) {
        int v = value && *value ? rl_atoi(value) : 100;
        g_rlv_comp_query = v < 0 ? 0 : v;
    } else if (!rl_stricmp(n, "editing-mode")) {
        if (value && !rl_strnicmp(value, "vi", 2)) { g_rl_vi_mode = true; g_rl_keymap = g_rl_vi_ins; }
        else if (value && !rl_strnicmp(value, "emacs", 5)) { g_rl_vi_mode = false; g_rl_keymap = g_rl_emacs; }
        else rc = 1;
    } else if (!rl_stricmp(n, "emacs-mode-string") || !rl_stricmp(n, "vi-cmd-mode-string") ||
               !rl_stricmp(n, "vi-ins-mode-string")) {
        char **slot = n[0] == 'e' ? &g_rlv_emacs_mode_str :
                      n[3] == 'c' ? &g_rlv_vi_cmd_str : &g_rlv_vi_ins_str;
        char *v = value ? rl_translated_copy(value) : 0;
        kfree(*slot);
        *slot = v;
    } else if (!rl_stricmp(n, "history-size")) {
        int v = value && *value ? rl_atoi(value) : 500;
        g_rlv_hist_size = v < 0 ? 0 : v;
    } else if (!rl_stricmp(n, "isearch-terminators")) {
        if (!value) rc = 1;
        else {
            SH_SB(w);
            if (*value == '"' || *value == '\'') {
                const char *e = value + 1;
                while (*e && *e != *value) e++;
                (void)sb_putmem(&w, value + 1, (size_t)(e - value - 1));
            } else {
                const char *e = value;
                while (*e && *e != ' ' && *e != '\t') e++;
                (void)sb_putmem(&w, value, (size_t)(e - value));
            }
            kfree(g_rlv_isearch_term);
            g_rlv_isearch_term = rl_translated_copy(w.p);
        }
    } else if (!rl_stricmp(n, "keymap")) {
        struct rl_kme *k = value ? rl_get_keymap_by_name(value) : 0;
        if (k) g_rl_keymap = k;
        else rc = 1;
    } else if (!rl_stricmp(n, "keyseq-timeout")) {
        int v = value && *value ? rl_atoi(value) : 0;
        g_rlv_seq_timeout = v < 0 ? 0 : v;
    } else if (!rl_stricmp(n, "active-region-end-color") ||
               !rl_stricmp(n, "active-region-start-color")) {
        char **slot = n[14] == 'e' ? &g_rlv_region_end : &g_rlv_region_start;
        kfree(*slot);
        *slot = value ? rl_translated_copy(value) : 0;
    }
    if (rc) rl_init_error("%s: could not set value to `%s'", name, value ? value : "");
    return rc;
}

static bool rl_ws(char c) { return c == ' ' || c == '\t'; }

/* _rl_skip_to_delim: past a quoted part, a backslash quoting one byte. */
static size_t rl_skip_to_delim(const char *s, size_t i, char delim) {
    bool passc = false;
    for (; s[i]; i++) {
        if (passc) { passc = false; continue; }
        if (s[i] == '\\') { passc = true; continue; }
        if (s[i] == delim) break;
    }
    return i;
}

/* $if / $else / $endif / $include, for init files and bind's arguments. */
static int  g_rl_if_depth;                  /* nesting */
static unsigned long long g_rl_if_off;      /* bit per level: parsing is off */
static unsigned long long g_rl_if_prev;     /* bit per level: was off before */

static bool rl_parsing_off(void) {
    return g_rl_if_depth > 0 && (g_rl_if_off >> (g_rl_if_depth - 1)) & 1u;
}

static int rl_read_init_file(const char *path, bool include);

static void rl_parser_directive(char *s) {
    size_t i = 0;
    while (s[i] && !rl_ws(s[i])) i++;
    char *dir = s;
    char *args = s + i;
    if (*args) *args++ = '\0';
    while (rl_ws(*args)) args++;
    if (!rl_stricmp(dir, "if")) {
        bool outer_off = rl_parsing_off();
        bool on = false;
        if (!outer_off) {
            if (!rl_strnicmp(args, "term=", 5)) {
                const char *term = env_get("TERM");
                const char *want = args + 5;
                size_t wl = 0;
                while (want[wl] && !rl_ws(want[wl])) wl++;
                if (term) {
                    size_t tl = 0;
                    while (term[tl] && term[tl] != '-') tl++;
                    on = (strlen(term) == wl && !rl_strnicmp(term, want, wl)) ||
                         (tl == wl && !rl_strnicmp(term, want, wl));
                }
            } else if (!rl_strnicmp(args, "mode=", 5)) {
                on = (!rl_strnicmp(args + 5, "emacs", 5) && !g_rl_vi_mode) ||
                     (!rl_strnicmp(args + 5, "vi", 2) && g_rl_vi_mode);
            } else if (!rl_strnicmp(args, "version", 7)) {
                /* readline 8.2: compare against it */
                const char *p = args + 7;
                while (rl_ws(*p)) p++;
                char op[3] = { 0, 0, 0 };
                int oi = 0;
                while ((*p == '=' || *p == '!' || *p == '<' || *p == '>') && oi < 2) op[oi++] = *p++;
                while (rl_ws(*p)) p++;
                int maj = 0, mn = 0;
                while (*p >= '0' && *p <= '9') maj = maj * 10 + (*p++ - '0');
                if (*p == '.') { p++; while (*p >= '0' && *p <= '9') mn = mn * 10 + (*p++ - '0'); }
                int have = 8 * 100 + 2, want = maj * 100 + mn;
                if (!strcmp(op, "=") || !strcmp(op, "==")) on = have == want;
                else if (!strcmp(op, "!=")) on = have != want;
                else if (!strcmp(op, ">=")) on = have >= want;
                else if (!strcmp(op, "<=")) on = have <= want;
                else if (!strcmp(op, ">")) on = have > want;
                else if (!strcmp(op, "<")) on = have < want;
            } else {
                size_t al = 0;
                while (args[al] && !rl_ws(args[al])) al++;
                char word[128];
                if (al >= sizeof word) al = sizeof word - 1;
                memcpy(word, args, al);
                word[al] = '\0';
                const char *rest = args + al;
                while (rl_ws(*rest)) rest++;
                if (!strncmp(rest, "==", 2) || (*rest == '=' && rest[1] != '=') ||
                    !strncmp(rest, "!=", 2)) {
                    bool neq = rest[0] == '!';
                    rest += (rest[1] == '=') ? 2 : 1;
                    while (rl_ws(*rest)) rest++;
                    char num[64];
                    int bi = rl_find_bool(word);
                    const char *have = bi >= 0 ? (*rl_boolvar_slot(bi) ? "on" : "off")
                                               : rl_string_var(word, num, sizeof num);
                    size_t vl = 0;
                    while (rest[vl] && !rl_ws(rest[vl])) vl++;
                    bool eq = have && strlen(have) == vl && !rl_strnicmp(have, rest, vl);
                    on = neq ? !eq : eq;
                } else {
                    on = !rl_stricmp(word, "bash");     /* the application */
                }
            }
        }
        if (g_rl_if_depth < 64) {
            unsigned long long bit = 1ull << g_rl_if_depth;
            if (outer_off || !on) g_rl_if_off |= bit; else g_rl_if_off &= ~bit;
            if (outer_off) g_rl_if_prev |= bit; else g_rl_if_prev &= ~bit;
            g_rl_if_depth++;
        }
        return;
    }
    if (!rl_stricmp(dir, "else")) {
        if (g_rl_if_depth == 0) { rl_init_error("$else found without matching $if"); return; }
        unsigned long long bit = 1ull << (g_rl_if_depth - 1);
        if (!(g_rl_if_prev & bit)) g_rl_if_off ^= bit;
        return;
    }
    if (!rl_stricmp(dir, "endif")) {
        if (g_rl_if_depth == 0) { rl_init_error("$endif without matching $if"); return; }
        g_rl_if_depth--;
        return;
    }
    if (!rl_stricmp(dir, "include")) {
        if (rl_parsing_off()) return;
        size_t al = strlen(args);
        while (al && rl_ws(args[al - 1])) args[--al] = '\0';
        (void)rl_read_init_file(args, true);
        return;
    }
    rl_init_error("%s: unknown parser directive", dir);
}

/* Keys by name, for "Control-x: ..." lines. */
static int rl_glean_key(const char *name) {
    static const struct { const char *n; int v; } names[] = {
        { "DEL", 0x7f }, { "ESC", 27 }, { "Escape", 27 }, { "LFD", 10 },
        { "Newline", 10 }, { "RET", 13 }, { "Return", 13 }, { "Rubout", 0x7f },
        { "SPC", ' ' }, { "Space", ' ' }, { "Tab", 9 }, { 0, 0 }
    };
    for (int i = 0; names[i].n; i++)
        if (!rl_stricmp(name, names[i].n)) return names[i].v;
    return (unsigned char)*name;
}

static bool rl_has_prefix(const char *s, const char *const *prefixes) {
    for (int i = 0; prefixes[i]; i++) {
        size_t pl = strlen(prefixes[i]);
        for (const char *p = s; *p; p++)
            if (!rl_strnicmp(p, prefixes[i], pl)) return true;
    }
    return false;
}

static int rl_macro_bind_keys(const char *seq, const char *body, struct rl_kme *map) {
    SH_SB(m);
    if (rl_translate_keyseq(body, &m) < 0) return -1;
    return rl_generic_bind(RL_ISMACR, seq, RL_FN_NONE, sb_steal(&m), map);
}

/* rl_parse_and_bind: one line of an init file or one `bind` argument. */
static int rl_parse_and_bind(const char *in) {
    SH_SB(buf);
    if (sb_puts(&buf, in) < 0) return 1;
    char *string = buf.p;
    while (rl_ws(*string)) string++;
    if (!*string || *string == '#') return 0;
    if (*string == '$') { rl_parser_directive(string + 1); return 0; }
    if (rl_parsing_off()) return 0;
    size_t i = 0;
    if (*string == '"') {
        i = rl_skip_to_delim(string, 1, '"');
        if (!string[i]) {
            rl_init_error("%s: no closing `\"' in key binding", string);
            return 1;
        }
        i++;
    }
    char c;
    for (; (c = string[i]) && c != ':' && c != ' ' && c != '\t'; i++) ;
    if (i == 0) {
        rl_init_error("`%s': invalid key binding: missing key sequence", string);
        return 1;
    }
    bool equivalency = c == ':' && string[i + 1] == '=';
    bool foundsep = c != 0;
    if (string[i]) string[i++] = '\0';
    if (equivalency) string[i++] = '\0';
    if (!rl_stricmp(string, "set")) {
        char *var = string + i;
        while (*var && rl_ws(*var)) var++;
        char *value = var;
        while (*value && !rl_ws(*value)) value++;
        if (*value) *value++ = '\0';
        while (*value && rl_ws(*value)) value++;
        if (rl_find_bool(var) >= 0) {
            char *e = value;
            while (*e && !rl_ws(*e)) e++;
            if (e > value) *e = '\0';
        } else if (rl_find_string(var) >= 0) {
            if (*value == '"') {
                size_t q = rl_skip_to_delim(value, 1, '"');
                value[q] = '\0';
                value++;
            } else {
                char *e = value + strlen(value);
                while (e > value && rl_ws(e[-1])) e--;
                *e = '\0';
            }
        } else {
            rl_init_error("%s: unknown variable name", var);
            return 1;
        }
        (void)rl_variable_bind(var, value);
        return 0;
    }
    for (; string[i] && rl_ws(string[i]); i++) ;
    char *funname = string + i;
    if (*funname == '\'' || *funname == '"') {
        i = rl_skip_to_delim(string, i + 1, *funname);
        if (string[i]) i++;
        else {
            rl_init_error("`%s': missing closing quote for macro", funname);
            return 1;
        }
    }
    for (; string[i] && !rl_ws(string[i]); i++) ;
    string[i] = '\0';
    if (equivalency) return 0;
    if (!foundsep) {
        rl_init_error("%s: no key sequence terminator", string);
        return 1;
    }
    if (*string == '"') {
        SH_SB(seq);
        bool passc = false;
        for (size_t j = 1; string[j]; j++) {
            if (passc || string[j] == '\\') {
                (void)sb_putc(&seq, string[j]);
                passc = !passc;
                continue;
            }
            if (string[j] == '"') break;
            (void)sb_putc(&seq, string[j]);
        }
        if (*funname == '\'' || *funname == '"') {
            size_t fl = strlen(funname);
            if (fl && funname[fl - 1] == *funname) funname[fl - 1] = '\0';
            return rl_macro_bind_keys(seq.p, funname + 1, g_rl_keymap) < 0 ? 1 : 0;
        }
        (void)rl_generic_bind(RL_ISFUNC, seq.p, (short)rl_named_function(funname), 0,
                              g_rl_keymap);
        return 0;
    }
    /* old style: Control-x, Meta-x, a bare key */
    const char *kname = 0;
    for (const char *p = string; *p; p++) if (*p == '-') kname = p + 1;
    if (!kname) kname = string;
    int key = rl_glean_key(kname);
    bool foundmod = false;
    static const char *const ctl_prefixes[] = { "Control-", "C-", "CTRL-", 0 };
    static const char *const meta_prefixes[] = { "Meta-", "M-", 0 };
    if (rl_has_prefix(string, ctl_prefixes)) {
        key = (key >= 'a' && key <= 'z' ? key - 32 : key) & 0x1f;
        foundmod = true;
    }
    if (rl_has_prefix(string, meta_prefixes)) {
        key |= 0x80;
        foundmod = true;
    }
    if (!foundmod && kname != string) {
        rl_init_error("%s: unknown key modifier", string);
        return 1;
    }
    /* bind the one key, written as a sequence */
    SH_SB(seq);
    if (key & 0x80) { (void)sb_puts(&seq, "\\M-"); key &= 0x7f; }
    if (key < 32) { (void)sb_puts(&seq, "\\C-"); key += 64; }
    else if (key == RL_RUBOUT) { (void)sb_puts(&seq, "\\C-"); key = '?'; }
    if (key == '\\' || key == '"') (void)sb_putc(&seq, '\\');
    (void)sb_putc(&seq, (char)key);
    if (*funname == '\'' || *funname == '"') {
        size_t fl = strlen(funname);
        if (fl && funname[fl - 1] == *funname) funname[fl - 1] = '\0';
        return rl_macro_bind_keys(seq.p, funname + 1, g_rl_keymap) < 0 ? 1 : 0;
    }
    (void)rl_generic_bind(RL_ISFUNC, seq.p, (short)rl_named_function(funname), 0, g_rl_keymap);
    return 0;
}

/* rl_read_init_file. 0 read, -1 could not open. */
static int rl_read_init_file(const char *path, bool include) {
    void *data = 0;
    size_t sz = 0;
    SH_SB(full);
    if (include && path[0] != '/' && g_rl_init_file) {
        const char *slash = 0;
        for (const char *p = g_rl_init_file; *p; p++) if (*p == '/') slash = p;
        if (slash) (void)sb_putmem(&full, g_rl_init_file, (size_t)(slash - g_rl_init_file) + 1);
    }
    if (path[0] == '~' && path[1] == '/') {
        const char *home = env_get("HOME");
        (void)sb_puts(&full, home ? home : "");
        (void)sb_puts(&full, path + 1);
    } else {
        (void)sb_puts(&full, path);
    }
    char canon[VFS_PATH_MAX];
    if (shell_canonicalize_path(full.p, canon, sizeof canon) < 0 ||
        vfs_read_all(canon, &data, &sz) != VFS_OK)
        return -1;
    const char *saved_file = g_rl_init_file;
    int saved_line = g_rl_init_line;
    g_rl_init_file = full.p;
    g_rl_init_line = 0;
    const char *p = (const char *)data, *end = p + sz;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        g_rl_init_line++;
        SH_SB(line);
        (void)sb_putmem(&line, ls, (size_t)(p - ls));
        if (p < end) p++;
        /* a backslash-newline continues the line */
        while (line.len && line.p[line.len - 1] == '\\' && p < end) {
            sb_truncate(&line, line.len - 1);
            const char *ns = p;
            while (p < end && *p != '\n') p++;
            g_rl_init_line++;
            (void)sb_putmem(&line, ns, (size_t)(p - ns));
            if (p < end) p++;
        }
        const char *q = line.p;
        while (rl_ws(*q)) q++;
        if (!*q || *q == '#') continue;
        (void)rl_parse_and_bind(q);
    }
    g_rl_init_file = saved_file;
    g_rl_init_line = saved_line;
    kfree(data);
    return 0;
}

/* ---- bind -x ------------------------------------------------------------ */

static struct rl_kme *rl_cmd_xmap(struct rl_kme *kmap) {
    if (!g_rl_xemacs) {
        g_rl_xemacs = rl_new_keymap();
        g_rl_xvi_ins = rl_new_keymap();
        g_rl_xvi_move = rl_new_keymap();
        if (!g_rl_xemacs || !g_rl_xvi_ins || !g_rl_xvi_move) return 0;
        g_rl_xemacs[0x18].type = RL_ISKMAP;
        g_rl_xemacs[0x18].map = rl_new_keymap();
        g_rl_xemacs[RL_ESC].type = RL_ISKMAP;
        g_rl_xemacs[RL_ESC].map = rl_new_keymap();
    }
    if (kmap == g_rl_emacs) return g_rl_xemacs;
    if (kmap == g_rl_emacs_meta) return g_rl_xemacs[RL_ESC].map;
    if (kmap == g_rl_emacs_ctlx) return g_rl_xemacs[0x18].map;
    if (kmap == g_rl_vi_ins) return g_rl_xvi_ins;
    if (kmap == g_rl_vi_move) return g_rl_xvi_move;
    return 0;
}

/* isolate_sequence: -1 after the error is out. */
static int rl_isolate_sequence(const char *s, int ind, bool need_dquote, int *startp) {
    int i = ind;
    while (s[i] && rl_ws(s[i])) i++;
    if (need_dquote && s[i] != '"') {
        kprintf("bind: %s: first non-whitespace character is not `\"'\n", s);
        return -1;
    }
    char delim = (s[i] == '"' || s[i] == '\'') ? s[i] : 0;
    if (startp) *startp = delim ? ++i : i;
    bool passc = false;
    for (; s[i]; i++) {
        if (passc) { passc = false; continue; }
        if (s[i] == '\\') { passc = true; continue; }
        if (s[i] == delim) break;
    }
    if (delim && s[i] != delim) {
        kprintf("bind: no closing `%c' in %s\n", delim, s);
        return -1;
    }
    return i;
}

static int rl_bind_unix_command(const char *line) {
    int kstart;
    int i = rl_isolate_sequence(line, 0, true, &kstart);
    if (i < 0) return -1;
    SH_SB(kseq);
    (void)sb_putmem(&kseq, line + kstart, (size_t)(i - kstart));
    for (; line[i] && line[i] != ':'; i++) ;
    if (line[i] != ':') {
        kprintf("bind: %s: missing colon separator\n", line);
        return -1;
    }
    i = rl_isolate_sequence(line, i + 1, false, &kstart);
    if (i < 0) return -1;
    struct rl_kme *xmap = rl_cmd_xmap(g_rl_keymap);
    if (!xmap) return -1;
    char *value = (char *)kmalloc((size_t)(i - kstart) + 1);
    if (!value) return -1;
    memcpy(value, line + kstart, (size_t)(i - kstart));
    value[i - kstart] = '\0';
    (void)rl_generic_bind(RL_ISMACR, kseq.p, RL_FN_NONE, value, xmap);
    (void)rl_generic_bind(RL_ISFUNC, kseq.p, RL_FN_UNIX, 0, g_rl_keymap);
    return 0;
}

static void rl_unbind_unix_command(const char *kseq) {
    struct rl_kme *xmap = rl_cmd_xmap(g_rl_keymap);
    if (!xmap || rl_generic_bind(RL_ISFUNC, kseq, RL_FN_NONE, 0, xmap) != 0)
        kprintf("bind: `%s': cannot unbind in command keymap\n", kseq);
}

/* rl_unbind_function_in_map */
static bool rl_unbind_function(short fn, struct rl_kme *map) {
    bool any = false;
    for (int i = 0; i < RL_KEYMAP_SIZE; i++) {
        if (map[i].type == RL_ISFUNC && map[i].fn == fn) {
            map[i].fn = RL_FN_NONE;
            any = true;
        } else if (map[i].type == RL_ISKMAP && map[i].map) {
            if (rl_unbind_function(fn, map[i].map)) any = true;
        }
    }
    return any;
}

/* ---- the builtin ---------------------------------------------------------- */

/* bash's no_line_editing, inverted: an interactive shell with emacs or vi
 * editing on. A script never edits lines, so `bind` there warns. */
static bool shell_line_editing_on(void) {
    return g_interactive && (g_opt_emacs || g_opt_vi);
}

static void cmd_bind(int argc, char **argv) {
    /* bash warns -- and goes on -- when the shell is not editing lines */
    if (!shell_line_editing_on()) kprintf("bind: warning: line editing not enabled\n");
    rl_bash_init();
    struct sh_igo g;
    sh_igo_init(&g, "bind", argc, argv);
    bool lflag = false, vflag = false, pflag = false, Vflag = false, Pflag = false;
    bool sflag = false, Sflag = false, Xflag = false;
    const char *initfile = 0, *map_name = 0, *fun_name = 0, *unbind_name = 0;
    const char *remove_seq = 0, *cmd_seq = 0;
    bool fflag = false, qflag = false, uflag = false, rflag = false, xflag = false, mflag = false;
    int c;
    while ((c = sh_igo_next(&g, "lvpVPsSXf:q:u:m:r:x:")) != -1) {
        switch (c) {
        case 'l': lflag = true; break;
        case 'v': vflag = true; break;
        case 'p': pflag = true; break;
        case 'f': fflag = true; initfile = g.optarg; break;
        case 'm': mflag = true; map_name = g.optarg; break;
        case 'q': qflag = true; fun_name = g.optarg; break;
        case 'u': uflag = true; unbind_name = g.optarg; break;
        case 'r': rflag = true; remove_seq = g.optarg; break;
        case 'V': Vflag = true; break;
        case 'P': Pflag = true; break;
        case 's': sflag = true; break;
        case 'S': Sflag = true; break;
        case 'x': xflag = true; cmd_seq = g.optarg; break;
        case 'X': Xflag = true; break;
        default:
            sh_builtin_usage("bind");
            shell_set_status(2);
            return;
        }
    }
    int rc = 0;
    struct rl_kme *saved = 0;
    if (mflag && map_name) {
        struct rl_kme *k = rl_get_keymap_by_name(map_name);
        if (!k) {
            kprintf("bind: `%s': invalid keymap name\n", map_name);
            shell_set_status(1);
            return;
        }
        saved = g_rl_keymap;
        g_rl_keymap = k;
    }
    if (lflag) {
        struct sh_strv n = { 0, 0, 0 };
        rl_funmap_names(&n);
        for (int i = 0; i < n.n; i++) shell_printf("%s\n", n.v[i]);
        sv_free(&n);
    }
    if (pflag) rl_function_dumper(true);
    if (Pflag) rl_function_dumper(false);
    if (sflag) rl_macro_dumper_in(true, g_rl_keymap, "");
    if (Sflag) rl_macro_dumper_in(false, g_rl_keymap, "");
    if (vflag) rl_variable_dumper(true);
    if (Vflag) rl_variable_dumper(false);
    if (fflag && initfile) {
        if (rl_read_init_file(initfile, false) != 0) {
            kprintf("bind: %s: cannot read: %s\n", initfile, shell_errno_text(VFS_ERR_NOENT));
            rc = 1;
            goto done;
        }
    }
    if (qflag && fun_name) {
        int fn = rl_named_function(fun_name);
        if (fn == RL_FN_NONE) {
            kprintf("bind: `%s': unknown function name\n", fun_name);
            rc = 1;
        } else {
            struct sh_strv inv = { 0, 0, 0 };
            rl_invoking(RL_ISFUNC, (short)fn, 0, g_rl_keymap, &inv);
            if (!inv.n) {
                shell_printf("%s is not bound to any keys.\n", fun_name);
                rc = 1;
            } else {
                SH_SB(l);
                (void)sb_puts(&l, fun_name);
                (void)sb_puts(&l, " can be invoked via ");
                int j;
                for (j = 0; j < 5 && j < inv.n; j++) {
                    (void)sb_putc(&l, '"');
                    (void)sb_puts(&l, inv.v[j]);
                    (void)sb_puts(&l, j + 1 < inv.n ? "\", " : "\".\n");
                }
                if (j < inv.n) (void)sb_puts(&l, "...\n");
                shell_printf("%s", l.p);
            }
            sv_free(&inv);
        }
    }
    if (uflag && unbind_name) {
        int fn = rl_named_function(unbind_name);
        if (fn == RL_FN_NONE) {
            kprintf("bind: `%s': unknown function name\n", unbind_name);
            rc = 1;
        } else {
            (void)rl_unbind_function((short)fn, g_rl_keymap);
        }
    }
    if (rflag && remove_seq) {
        SH_SB(k);
        if (rl_translate_keyseq(remove_seq, &k) < 0) {
            kprintf("bind: `%s': cannot unbind\n", remove_seq);
            rc = 1;
            goto done;
        }
        const struct rl_kme *e = rl_entry_of_keys(k.p, (int)k.len, g_rl_keymap);
        bool was_unix = false;
        if (e) {
            if (e->type == RL_ISKMAP && e->map)
                was_unix = e->map[RL_ANYOTHERKEY].type == RL_ISFUNC &&
                           e->map[RL_ANYOTHERKEY].fn == RL_FN_UNIX;
            else
                was_unix = e->type == RL_ISFUNC && e->fn == RL_FN_UNIX;
            bool bound = (e->type == RL_ISKMAP) ||
                         (e->type == RL_ISFUNC && e->fn != RL_FN_NONE) ||
                         e->type == RL_ISMACR;
            if (bound) {
                if (rl_generic_bind(RL_ISFUNC, remove_seq, RL_FN_NONE, 0, g_rl_keymap) != 0) {
                    kprintf("bind: `%s': cannot unbind\n", remove_seq);
                    rc = 1;
                    goto done;
                }
                if (was_unix) rl_unbind_unix_command(remove_seq);
            }
        }
        goto done;
    }
    if (xflag && cmd_seq) rc = rl_bind_unix_command(cmd_seq) < 0 ? 1 : rc;
    if (Xflag) {
        struct rl_kme *x = rl_cmd_xmap(g_rl_keymap);
        if (x) rl_macro_dumper_in(true, x, "");
    }
    for (int i = g.ind; i < argc; i++) {
        /* a binding that replaces a bind -x one takes its command away */
        struct sh_strv before = { 0, 0, 0 }, after = { 0, 0, 0 };
        rl_invoking(RL_ISFUNC, RL_FN_UNIX, 0, g_rl_keymap, &before);
        (void)rl_parse_and_bind(argv[i]);
        rl_invoking(RL_ISFUNC, RL_FN_UNIX, 0, g_rl_keymap, &after);
        for (int b = 0; b < before.n; b++)
            if (!sv_has(&after, before.v[b])) rl_unbind_unix_command(before.v[b]);
        sv_free(&before);
        sv_free(&after);
    }
done:
    if (saved) g_rl_keymap = saved;
    shell_set_status(rc);
}
