/* addr2line -- turn an address into file:line, from DWARF.
 *
 *     addr2line [-e file] [-f] [-C] [-s] [-a] [-i] [ADDR ...]
 *
 * The line table is not a table. DWARF stores a PROGRAM whose execution
 * emits (address, file, line) rows -- that is how a whole function's line
 * information fits in a few dozen bytes -- so getting an answer means
 * running that program and watching the rows go by. There is no shortcut
 * and no lookup structure to read instead.
 *
 * DWARF 2 through 5 are all understood, and versions 4 and 5 differ in the
 * part that matters most: v5 replaced the two NUL-terminated string lists
 * (directories, then files) with format-described tables whose entries can
 * live in .debug_line_str. A parser that assumes v4 reads a v5 header as
 * garbage and then produces confident, wrong line numbers -- which is worse
 * than refusing, so the version is checked and the v5 shape is parsed
 * properly.
 *
 * -f also prints the function name. That comes from .symtab, not DWARF:
 * the symbol table already maps address ranges to names, and reading
 * .debug_info for DW_TAG_subprogram would be a second answer to a question
 * already answered.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          s64;

static int want_func, want_basename, want_addr, demangle;
static const char *g_exe = "a.out";

/* ------------------------------------------------------------ ELF access */

struct sec { const u8 *data; u64 size; };

struct elf {
    u8   *img;
    size_t len;
    struct sec line, line_str, str, info, abbrev, str_offsets;
    const u8 *symtab; u64 symsize;
    const char *strtab; u64 strsize;
};

static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }

static u8 *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    u8 *b = (u8 *)malloc(cap);
    if (!b) { fclose(f); return NULL; }
    for (;;) {
        if (n == cap) {
            u8 *nb = (u8 *)realloc(b, cap * 2);
            if (!nb) { free(b); fclose(f); return NULL; }
            b = nb; cap *= 2;
        }
        size_t got = fread(b + n, 1, cap - n, f);
        if (!got) break;
        n += got;
    }
    fclose(f);
    *len = n;
    return b;
}

static int elf_open(struct elf *e, const char *path) {
    memset(e, 0, sizeof *e);
    e->img = slurp(path, &e->len);
    if (!e->img) {
        fprintf(stderr, "addr2line: %s: %s\n", path, strerror(errno));
        return 0;
    }
    const u8 *o = e->img;
    if (e->len < 64 || memcmp(o, "\177ELF", 4) || o[4] != 2 || o[5] != 1) {
        fprintf(stderr, "addr2line: %s: not a little-endian ELF64 file\n", path);
        return 0;
    }
    u64 shoff = rd64(o + 0x28);
    int shentsize = rd16(o + 0x3A), shnum = rd16(o + 0x3C), shstrndx = rd16(o + 0x3E);
    if (shentsize != 64 || shnum == 0 || shoff + (u64)shnum * 64 > e->len) {
        fprintf(stderr, "addr2line: %s: no usable section headers\n", path);
        return 0;
    }
    const u8 *shs = o + shoff;
    u64 nameoff = rd64(shs + (u64)shstrndx * 64 + 24);
    const char *shstr = (const char *)(o + nameoff);

    for (int i = 0; i < shnum; i++) {
        const u8 *sh = shs + (u64)i * 64;
        const char *nm = shstr + rd32(sh);
        u64 off = rd64(sh + 24), size = rd64(sh + 32);
        if (off + size > e->len) continue;
        if (!strcmp(nm, ".debug_line"))      { e->line.data = o + off; e->line.size = size; }
        else if (!strcmp(nm, ".debug_line_str")) { e->line_str.data = o + off; e->line_str.size = size; }
        else if (!strcmp(nm, ".debug_str"))  { e->str.data = o + off; e->str.size = size; }
        else if (!strcmp(nm, ".debug_info"))   { e->info.data = o + off; e->info.size = size; }
        else if (!strcmp(nm, ".debug_abbrev")) { e->abbrev.data = o + off; e->abbrev.size = size; }
        else if (!strcmp(nm, ".debug_str_offsets")) { e->str_offsets.data = o + off; e->str_offsets.size = size; }
        else if (rd32(sh + 4) == 2 /* SHT_SYMTAB */) {
            e->symtab = o + off; e->symsize = size;
            u32 link = rd32(sh + 40);
            if (link < (u32)shnum) {
                const u8 *ls = shs + (u64)link * 64;
                u64 so = rd64(ls + 24), ss = rd64(ls + 32);
                if (so + ss <= e->len) { e->strtab = (const char *)(o + so); e->strsize = ss; }
            }
        }
    }
    return 1;
}

/* The function containing `addr`: the defined FUNC symbol with the greatest
 * value not above it, preferring one whose size actually covers it. */
static const char *func_for(struct elf *e, u64 addr) {
    if (!e->symtab || !e->strtab) return NULL;
    const char *best = NULL;
    u64 best_val = 0;
    for (u64 k = 0; k + 24 <= e->symsize; k += 24) {
        const u8 *sym = e->symtab + k;
        u32 nameoff = rd32(sym);
        u8 info = sym[4];
        u16 shndx = rd16(sym + 6);
        u64 value = rd64(sym + 8), size = rd64(sym + 16);
        if ((info & 0xF) != 2 /* STT_FUNC */) continue;
        if (shndx == 0) continue;
        if (nameoff == 0 || nameoff >= e->strsize) continue;
        if (addr < value) continue;
        if (size && addr >= value + size) continue;
        if (!best || value >= best_val) { best = e->strtab + nameoff; best_val = value; }
    }
    return best;
}

static int read_form_str(const u8 **p, const u8 *end, u64 form,
                         struct elf *e, int off_size, char *out, size_t cap);

/* -------------------------------------------------------------- LEB128 */

static u64 uleb(const u8 **p, const u8 *end) {
    u64 v = 0;
    int shift = 0;
    while (*p < end) {
        u8 b = *(*p)++;
        if (shift < 64) v |= (u64)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    return v;
}

static s64 sleb(const u8 **p, const u8 *end) {
    s64 v = 0;
    int shift = 0;
    u8 b = 0;
    while (*p < end) {
        b = *(*p)++;
        if (shift < 64) v |= (s64)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    if (shift < 64 && (b & 0x40)) v |= -((s64)1 << shift);
    return v;
}

/* --------------------------------------------- DW_AT_comp_dir, per unit
 *
 * In DWARF 4 a file entry with directory index 0 means "the compilation
 * directory", and that string is NOT in the line table -- it is
 * DW_AT_comp_dir on the compilation unit's DIE, in .debug_info. Without it
 * the answer is a bare "t.c:3" where every other tool says
 * "/full/path/t.c:3". DWARF 5 puts the compilation directory in the
 * directory table as entry 0, which is why v5 needs none of this.
 *
 * Only the FIRST DIE of each unit is read -- the compilation unit itself.
 * Its abbreviation says which attributes follow and in what form, so the
 * ones that are not wanted are skipped by form rather than guessed past.
 */
struct cu_info { u64 stmt_list; char comp_dir[512]; int have; };

#define MAXCU 256
static struct cu_info g_cus[MAXCU];
static int g_ncu;

/* Skips one attribute value of the given form inside .debug_info. */
static int skip_info_form(const u8 **p, const u8 *end, u64 form,
                          int off_size, int addr_size, u64 implicit) {
    switch (form) {
    case 0x01: *p += addr_size; return 1;                 /* addr        */
    case 0x03: *p += 2; return 1;                         /* block2      */
    case 0x04: *p += 4; return 1;                         /* block4      */
    case 0x05: *p += 2; return 1;                         /* data2       */
    case 0x06: *p += 4; return 1;                         /* data4       */
    case 0x07: *p += 8; return 1;                         /* data8       */
    case 0x08: while (*p < end && **p) (*p)++; if (*p < end) (*p)++; return 1;
    case 0x09: { u64 n = uleb(p, end); *p += n; return 1; }   /* block    */
    case 0x0a: { u64 n = **p; (*p)++; *p += n; return 1; }    /* block1   */
    case 0x0b: *p += 1; return 1;                         /* data1       */
    case 0x0c: *p += 1; return 1;                         /* flag        */
    case 0x0d: sleb(p, end); return 1;                    /* sdata       */
    case 0x0e: *p += off_size; return 1;                  /* strp        */
    case 0x0f: uleb(p, end); return 1;                    /* udata       */
    case 0x10: uleb(p, end); return 1;                    /* ref_addr    */
    case 0x11: *p += 1; return 1;
    case 0x12: *p += 2; return 1;
    case 0x13: *p += 4; return 1;
    case 0x14: *p += 8; return 1;
    case 0x15: uleb(p, end); return 1;
    case 0x16: { u64 f2 = uleb(p, end); return skip_info_form(p, end, f2, off_size, addr_size, implicit); }
    case 0x17: *p += off_size; return 1;                  /* sec_offset  */
    case 0x18: { u64 n = uleb(p, end); *p += n; return 1; }   /* exprloc  */
    case 0x19: return 1;                                  /* flag_present*/
    case 0x1a: uleb(p, end); return 1;                    /* strx        */
    case 0x1b: uleb(p, end); return 1;                    /* addrx       */
    case 0x1f: *p += off_size; return 1;                  /* line_strp   */
    case 0x20: *p += 8; return 1;                         /* ref_sig8    */
    case 0x21: return 1;                                  /* implicit_const */
    case 0x1e: *p += 16; return 1;                        /* data16      */
    case 0x25: *p += 1; return 1;
    case 0x26: *p += 2; return 1;
    case 0x27: *p += 3; return 1;
    case 0x28: *p += 4; return 1;
    case 0x29: *p += 1; return 1;
    case 0x2a: *p += 2; return 1;
    case 0x2b: *p += 3; return 1;
    case 0x2c: *p += 4; return 1;
    default: return 0;
    }
}

/* Finds the abbreviation with the given code, returning a pointer just past
 * its (tag, has_children) so the caller can walk its attribute specs. */
static const u8 *abbrev_find(struct elf *e, u64 table_off, u64 code,
                             const u8 **end_out) {
    if (!e->abbrev.data || table_off >= e->abbrev.size) return NULL;
    const u8 *p = e->abbrev.data + table_off;
    const u8 *end = e->abbrev.data + e->abbrev.size;
    while (p < end) {
        u64 c = uleb(&p, end);
        if (c == 0) return NULL;                 /* end of this table */
        uleb(&p, end);                           /* tag */
        if (p >= end) return NULL;
        p++;                                     /* has_children */
        if (c == code) { *end_out = end; return p; }
        /* skip this entry's attribute specs */
        for (;;) {
            u64 at = uleb(&p, end), form = uleb(&p, end);
            if (form == 0x21) sleb(&p, end);     /* implicit_const */
            if (at == 0 && form == 0) break;
        }
    }
    return NULL;
}

static void scan_comp_dirs(struct elf *e) {
    if (!e->info.data || !e->abbrev.data) return;
    const u8 *p = e->info.data, *secend = e->info.data + e->info.size;
    while (p + 11 <= secend && g_ncu < MAXCU) {
        u64 unit_len = rd32(p);
        int off_size = 4;
        p += 4;
        if (unit_len == 0xFFFFFFFFu) { unit_len = rd64(p); p += 8; off_size = 8; }
        if (unit_len == 0) break;
        const u8 *unit_end = p + unit_len;
        if (unit_end > secend || unit_end < p) break;

        int version = rd16(p);
        p += 2;
        u64 abbrev_off;
        int addr_size;
        if (version >= 5) {
            p++;                                  /* unit_type */
            addr_size = *p++;
            abbrev_off = off_size == 8 ? rd64(p) : rd32(p);
            p += off_size;
        } else {
            abbrev_off = off_size == 8 ? rd64(p) : rd32(p);
            p += off_size;
            addr_size = *p++;
        }

        u64 code = uleb(&p, unit_end);
        const u8 *aend = NULL;
        const u8 *spec = code ? abbrev_find(e, abbrev_off, code, &aend) : NULL;
        struct cu_info ci;
        memset(&ci, 0, sizeof ci);
        ci.stmt_list = (u64)-1;
        if (spec) {
            for (;;) {
                u64 at = uleb(&spec, aend), form = uleb(&spec, aend);
                s64 implicit = 0;
                if (form == 0x21) implicit = sleb(&spec, aend);
                if (at == 0 && form == 0) break;
                if (at == 0x1b /* DW_AT_comp_dir */) {
                    if (!read_form_str(&p, unit_end, form, e, off_size,
                                       ci.comp_dir, sizeof ci.comp_dir)) {
                        if (!skip_info_form(&p, unit_end, form, off_size, addr_size,
                                            (u64)implicit)) break;
                    }
                } else if (at == 0x10 /* DW_AT_stmt_list */) {
                    if (form == 0x17 || form == 0x06) {
                        ci.stmt_list = off_size == 8 && form == 0x17 ? rd64(p) : rd32(p);
                        p += (form == 0x17) ? off_size : 4;
                    } else if (form == 0x0b) { ci.stmt_list = *p; p += 1; }
                    else if (form == 0x05) { ci.stmt_list = rd16(p); p += 2; }
                    else if (form == 0x07) { ci.stmt_list = rd64(p); p += 8; }
                    else if (!skip_info_form(&p, unit_end, form, off_size, addr_size,
                                             (u64)implicit)) break;
                } else if (!skip_info_form(&p, unit_end, form, off_size, addr_size,
                                           (u64)implicit)) break;
            }
            ci.have = 1;
            g_cus[g_ncu++] = ci;
        }
        p = unit_end;
    }
}

static const char *comp_dir_for(u64 line_off) {
    for (int i = 0; i < g_ncu; i++)
        if (g_cus[i].have && g_cus[i].stmt_list == line_off && g_cus[i].comp_dir[0])
            return g_cus[i].comp_dir;
    /* A single-unit file is the common case; if only one unit exists its
     * directory is the answer whatever the stmt_list offset turned out to
     * be. */
    if (g_ncu == 1 && g_cus[0].comp_dir[0]) return g_cus[0].comp_dir;
    return NULL;
}

/* ------------------------------------------------------- the line program */

#define MAXFILES 512
#define MAXDIRS  128

struct lp_state {
    char dirs[MAXDIRS][256];
    int  ndirs;
    struct { char name[256]; int dir; } files[MAXFILES];
    int  nfiles;
    int  version;
    char comp_dir[512];
};

struct result { u64 addr; char file[512]; int line; int found; };

/* Reads one string in the given DWARF form into `out`. Returns 0 if the
 * form is one this parser does not read, which is a reason to give up on
 * the unit rather than to carry on with a wrong offset. */
static int read_form_str(const u8 **p, const u8 *end, u64 form,
                         struct elf *e, int off_size, char *out, size_t cap) {
    switch (form) {
    case 0x08: {                                   /* DW_FORM_string */
        size_t n = 0;
        while (*p < end && **p) { if (n + 1 < cap) out[n++] = (char)**p; (*p)++; }
        if (*p < end) (*p)++;
        out[n] = 0;
        return 1;
    }
    case 0x1f: {                                   /* DW_FORM_line_strp */
        u64 o = off_size == 8 ? rd64(*p) : rd32(*p);
        *p += off_size;
        if (e->line_str.data && o < e->line_str.size)
            snprintf(out, cap, "%s", (const char *)e->line_str.data + o);
        else out[0] = 0;
        return 1;
    }
    case 0x0e: {                                   /* DW_FORM_strp */
        u64 o = off_size == 8 ? rd64(*p) : rd32(*p);
        *p += off_size;
        if (e->str.data && o < e->str.size)
            snprintf(out, cap, "%s", (const char *)e->str.data + o);
        else out[0] = 0;
        return 1;
    }
    default:
        return 0;
    }
}

/* Skips a non-string form whose value we do not need. */
static int skip_form(const u8 **p, const u8 *end, u64 form, int off_size) {
    (void)off_size;   /* every form here is fixed-width or a LEB */
    switch (form) {
    case 0x0b: (*p) += 1; return 1;                /* data1 */
    case 0x05: (*p) += 2; return 1;                /* data2 */
    case 0x06: (*p) += 4; return 1;                /* data4 */
    case 0x07: (*p) += 8; return 1;                /* data8 */
    case 0x1e: (*p) += 16; return 1;               /* data16 (MD5) */
    case 0x0f: uleb(p, end); return 1;             /* udata */
    case 0x0d: sleb(p, end); return 1;             /* sdata */
    case 0x11: (*p) += 1; return 1;                /* ref1 */
    case 0x1a: uleb(p, end); return 1;             /* strx */
    case 0x25: (*p) += 1; return 1;                /* strx1 */
    case 0x26: (*p) += 2; return 1;                /* strx2 */
    case 0x27: (*p) += 3; return 1;                /* strx3 */
    case 0x28: (*p) += 4; return 1;                /* strx4 */
    default: return 0;
    }
}

static void build_path(struct lp_state *st, int fileidx, char *out, size_t cap) {
    if (fileidx < 0 || fileidx >= st->nfiles) { snprintf(out, cap, "??"); return; }
    const char *name = st->files[fileidx].name;
    int d = st->files[fileidx].dir;
    const char *dir = (d >= 0 && d < st->ndirs && st->dirs[d][0]) ? st->dirs[d] : NULL;

    if (name[0] == '/' || (name[0] && name[1] == ':')) {   /* already absolute */
        snprintf(out, cap, "%s", name);
        return;
    }
    if (!dir) {
        /* Directory index 0 in DWARF 2-4: the compilation directory, which
         * lives in .debug_info rather than here. */
        const char *cd = st->comp_dir;
        if (cd && *cd) snprintf(out, cap, "%s/%s", cd, name);
        else snprintf(out, cap, "%s", name);
        return;
    }
    if (dir[0] == '/' || (dir[0] && dir[1] == ':')) {
        snprintf(out, cap, "%s/%s", dir, name);
        return;
    }
    /* A relative directory is relative to the compilation directory. */
    if (st->comp_dir[0]) snprintf(out, cap, "%.200s/%.150s/%.150s", st->comp_dir, dir, name);
    else snprintf(out, cap, "%s/%s", dir, name);
}

/* Runs every line program in .debug_line, keeping the row that covers each
 * requested address. */
static void run_line_programs(struct elf *e, struct result *res, int nres) {
    if (!e->line.data) return;
    const u8 *p = e->line.data, *secend = e->line.data + e->line.size;

    while (p + 4 <= secend) {
        size_t unit_start_off = (size_t)(p - e->line.data);
        u64 unit_len = rd32(p);
        int off_size = 4;
        p += 4;
        if (unit_len == 0xFFFFFFFFu) {             /* DWARF64 */
            if (p + 8 > secend) break;
            unit_len = rd64(p);
            p += 8;
            off_size = 8;
        }
        if (unit_len == 0) break;
        const u8 *unit_end = p + unit_len;
        if (unit_end > secend || unit_end < p) break;

        if (p + 2 > unit_end) break;
        int version = rd16(p);
        p += 2;
        if (version < 2 || version > 5) { p = unit_end; continue; }

        if (version >= 5) {
            if (p + 2 > unit_end) break;
            p += 2;                                /* address_size, seg sel */
        }
        if (p + (size_t)off_size > unit_end) break;
        u64 header_len = off_size == 8 ? rd64(p) : rd32(p);
        p += off_size;
        const u8 *prog = p + header_len;
        if (prog > unit_end) break;

        if (p >= unit_end) break;
        u8 min_inst = *p++;
        u8 max_ops = 1;
        if (version >= 4) { if (p >= unit_end) break; max_ops = *p++; }
        if (max_ops == 0) max_ops = 1;
        if (p + 4 > unit_end) break;
        u8 default_is_stmt = *p++;
        s64 line_base = (s64)(signed char)*p++;
        u8 line_range = *p++;
        u8 opcode_base = *p++;
        if (line_range == 0) { p = unit_end; continue; }
        const u8 *std_lens = p;
        if (opcode_base == 0 || p + (opcode_base - 1) > unit_end) { p = unit_end; continue; }
        p += opcode_base - 1;

        struct lp_state st;
        memset(&st, 0, sizeof st);
        st.version = version;
        {   /* Which compilation unit does this line program belong to?
             * The unit's DW_AT_stmt_list is its offset in .debug_line. */
            const char *cd = comp_dir_for((u64)(unit_start_off));
            if (cd) snprintf(st.comp_dir, sizeof st.comp_dir, "%s", cd);
        }

        if (version >= 5) {
            /* Format-described tables. The formats say which content types
             * appear and in which form, so the entries can only be read by
             * following that description -- there is no fixed layout. */
            if (p >= unit_end) { p = unit_end; continue; }
            int dfc = *p++;
            u64 dform[8], dtype[8];
            if (dfc > 8) { p = unit_end; continue; }
            for (int i = 0; i < dfc; i++) { dtype[i] = uleb(&p, unit_end); dform[i] = uleb(&p, unit_end); }
            u64 dcount = uleb(&p, unit_end);
            for (u64 i = 0; i < dcount && p < unit_end; i++) {
                char buf[256];
                for (int k = 0; k < dfc; k++) {
                    if (dtype[k] == 1 /* DW_LNCT_path */) {
                        if (!read_form_str(&p, unit_end, dform[k], e, off_size, buf, sizeof buf))
                        { p = unit_end; break; }
                        if (st.ndirs < MAXDIRS) snprintf(st.dirs[st.ndirs], 256, "%s", buf);
                    } else if (!skip_form(&p, unit_end, dform[k], off_size)) { p = unit_end; break; }
                }
                if (st.ndirs < MAXDIRS) st.ndirs++;
            }
            if (p >= unit_end) { p = unit_end; continue; }
            int ffc = *p++;
            u64 fform[8], ftype[8];
            if (ffc > 8) { p = unit_end; continue; }
            for (int i = 0; i < ffc; i++) { ftype[i] = uleb(&p, unit_end); fform[i] = uleb(&p, unit_end); }
            u64 fcount = uleb(&p, unit_end);
            for (u64 i = 0; i < fcount && p < unit_end; i++) {
                char buf[256];
                int dir = 0;
                buf[0] = 0;
                for (int k = 0; k < ffc; k++) {
                    if (ftype[k] == 1) {
                        if (!read_form_str(&p, unit_end, fform[k], e, off_size, buf, sizeof buf))
                        { p = unit_end; break; }
                    } else if (ftype[k] == 2 /* DW_LNCT_directory_index */) {
                        u64 form = fform[k];
                        if (form == 0x0b) dir = *p++;
                        else if (form == 0x05) { dir = rd16(p); p += 2; }
                        else if (form == 0x0f) dir = (int)uleb(&p, unit_end);
                        else if (!skip_form(&p, unit_end, form, off_size)) { p = unit_end; break; }
                    } else if (!skip_form(&p, unit_end, fform[k], off_size)) { p = unit_end; break; }
                }
                if (st.nfiles < MAXFILES) {
                    snprintf(st.files[st.nfiles].name, 256, "%s", buf);
                    st.files[st.nfiles].dir = dir;
                    st.nfiles++;
                }
            }
        } else {
            /* v2-v4: directory 0 is the compilation directory and is not
             * listed, so the file entries' dir indexes are 1-based into a
             * list that starts empty. Keeping slot 0 empty preserves that. */
            st.dirs[st.ndirs++][0] = 0;
            while (p < unit_end && *p) {
                if (st.ndirs < MAXDIRS) {
                    size_t n = 0;
                    while (p < unit_end && *p && n < 255) st.dirs[st.ndirs][n++] = (char)*p++;
                    st.dirs[st.ndirs][n] = 0;
                    while (p < unit_end && *p) p++;
                    st.ndirs++;
                } else {
                    while (p < unit_end && *p) p++;
                }
                if (p < unit_end) p++;
            }
            if (p < unit_end) p++;
            /* File index 0 is unused in v2-v4; leave a placeholder so the
             * program's 1-based set_file lands on the right entry. */
            st.files[st.nfiles].name[0] = 0;
            st.files[st.nfiles].dir = 0;
            st.nfiles++;
            while (p < unit_end && *p) {
                char nm[256];
                size_t n = 0;
                while (p < unit_end && *p && n < 255) nm[n++] = (char)*p++;
                nm[n] = 0;
                while (p < unit_end && *p) p++;
                if (p < unit_end) p++;
                u64 dir = uleb(&p, unit_end);
                uleb(&p, unit_end);                /* mtime */
                uleb(&p, unit_end);                /* length */
                if (st.nfiles < MAXFILES) {
                    snprintf(st.files[st.nfiles].name, 256, "%s", nm);
                    st.files[st.nfiles].dir = (int)dir;
                    st.nfiles++;
                }
            }
            if (p < unit_end) p++;
        }

        /* ---- run the program ---- */
        const u8 *ip = prog;
        u64 address = 0;
        int file = 1, line = 1, is_stmt = default_is_stmt, end_seq = 0;
        u64 prev_addr = 0;
        int prev_file = 1, prev_line = 1, have_prev = 0;

        while (ip < unit_end) {
            u8 op = *ip++;
            int emit = 0;
            end_seq = 0;

            if (op >= opcode_base) {
                u64 adj = op - opcode_base;
                address += (adj / line_range) * min_inst;
                line += (int)(line_base + (s64)(adj % line_range));
                emit = 1;
            } else if (op == 0) {
                u64 elen = uleb(&ip, unit_end);
                const u8 *next = ip + elen;
                if (next > unit_end) break;
                u8 sub = elen ? *ip++ : 0;
                if (sub == 1) { emit = 1; end_seq = 1; }
                else if (sub == 2) {
                    if (elen >= 9) address = rd64(ip);
                    else if (elen >= 5) address = rd32(ip);
                }
                ip = next;
            } else {
                switch (op) {
                case 1: emit = 1; break;                                  /* copy */
                case 2: address += uleb(&ip, unit_end) * min_inst; break;  /* advance_pc */
                case 3: line += (int)sleb(&ip, unit_end); break;           /* advance_line */
                case 4: file = (int)uleb(&ip, unit_end); break;            /* set_file */
                case 5: uleb(&ip, unit_end); break;                        /* set_column */
                case 6: is_stmt = !is_stmt; break;
                case 7: break;                                             /* basic_block */
                case 8: address += ((255 - opcode_base) / line_range) * min_inst; break;
                case 9: if (ip + 2 <= unit_end) { address += rd16(ip); ip += 2; } break;
                case 10: case 11: break;
                case 12: uleb(&ip, unit_end); break;
                default: {
                    /* An opcode this parser does not know still declares how
                     * many LEB operands it has, so it can be skipped exactly
                     * rather than guessed past. */
                    int nargs = std_lens[op - 1];
                    for (int k = 0; k < nargs; k++) uleb(&ip, unit_end);
                    break;
                }
                }
            }
            (void)is_stmt;

            if (emit) {
                /* A row gives the line for every address from the PREVIOUS
                 * row up to this one -- the table is a sequence of range
                 * starts, so the answer for an address is the row before it. */
                if (have_prev) {
                    for (int r = 0; r < nres; r++) {
                        if (res[r].found) continue;
                        if (res[r].addr >= prev_addr && res[r].addr < address) {
                            build_path(&st, prev_file, res[r].file, sizeof res[r].file);
                            res[r].line = prev_line;
                            res[r].found = 1;
                        }
                    }
                }
                prev_addr = address;
                prev_file = file;
                prev_line = line;
                have_prev = !end_seq;
                if (end_seq) {
                    address = 0; file = 1; line = 1;
                    is_stmt = default_is_stmt;
                }
            }
        }
        p = unit_end;
    }
}

int main(int argc, char **argv) {
    const char *addrs[256];
    int naddr = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-e") && i + 1 < argc) { g_exe = argv[++i]; continue; }
        if (!strncmp(a, "--exe=", 6)) { g_exe = a + 6; continue; }
        if (!strcmp(a, "--version")) { printf("addr2line (tobyOS) 1.0\n"); return 0; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: addr2line [-e file] [-f] [-C] [-s] [-a] ADDR ...\n");
            return 0;
        }
        if (a[0] == '-' && a[1] && a[1] != '0') {
            for (const char *q = a + 1; *q; q++) {
                switch (*q) {
                case 'f': want_func = 1; break;
                case 's': want_basename = 1; break;
                case 'a': want_addr = 1; break;
                case 'C': demangle = 1; break;
                case 'i': break;   /* inlines: no .debug_info reader here */
                case 'p': break;
                case 'e': break;
                default: break;
                }
            }
            continue;
        }
        if (naddr < 256) addrs[naddr++] = a;
    }

    struct elf e;
    if (!elf_open(&e, g_exe)) return 1;
    scan_comp_dirs(&e);
    if (!e.line.data)
        fprintf(stderr, "addr2line: %s has no .debug_line -- build with -g\n", g_exe);

    /* Addresses may also come from stdin, which is how it is used in a
     * pipeline behind a backtrace. */
    char linebuf[256];
    if (naddr == 0) {
        while (fgets(linebuf, sizeof linebuf, stdin) && naddr < 256) {
            char *nl = strpbrk(linebuf, "\r\n");
            if (nl) *nl = 0;
            if (!linebuf[0]) continue;
            char *copy = (char *)malloc(strlen(linebuf) + 1);
            if (!copy) break;
            strcpy(copy, linebuf);
            addrs[naddr++] = copy;
        }
    }

    struct result res[256];
    memset(res, 0, sizeof res);
    for (int i = 0; i < naddr; i++)
        res[i].addr = strtoull(addrs[i], NULL, 16);

    run_line_programs(&e, res, naddr);

    for (int i = 0; i < naddr; i++) {
        /* GNU puts the address on its OWN line, not as a prefix. */
        if (want_addr) printf("0x%016llx\n", (unsigned long long)res[i].addr);
        if (want_func) {
            const char *fn = func_for(&e, res[i].addr);
            printf("%s\n", fn ? fn : "??");
        }
        if (!res[i].found) { printf("??:0\n"); continue; }
        const char *path = res[i].file;
        if (want_basename) {
            const char *b = path;
            for (const char *q = path; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
            path = b;
        }
        printf("%s:%d\n", path, res[i].line);
    }
    (void)demangle;
    free(e.img);
    return 0;
}
