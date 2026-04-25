/* SPDX-License-Identifier: MIT
 *
 * tobyOS dynamic linker -- main logic.
 *
 * Pipeline (called from _start, after self-relocation):
 *
 *   1.  Parse the SysV stack: find auxv -> AT_PHDR/AT_PHNUM/
 *       AT_PHENT/AT_BASE/AT_ENTRY for the just-loaded program.
 *
 *   2.  Walk the program's PHDRs to locate PT_DYNAMIC. Build a
 *       module record for the program (base, _DYNAMIC, strtab,
 *       symtab, hash).
 *
 *   3.  For each DT_NEEDED in the program, build "/lib/<name>" and
 *       call sys_dload(). Build a module record for each library.
 *
 *   4.  Apply relocations:
 *
 *         a) For each library, apply its DT_RELA + DT_JMPREL using
 *            its own _DYNAMIC. Library-internal relocs are mostly
 *            R_X86_64_RELATIVE (because we link with -Bsymbolic).
 *
 *         b) For the program, apply DT_RELA + DT_JMPREL. Symbolic
 *            relocs are resolved by walking the module list in
 *            order (program first, then libraries -- matching glibc
 *            scope rules for the executable's lookup scope).
 *
 *   5.  Return the program entry point. _start tail-jumps there.
 *
 * Failure mode: ld_die("...") -- writes to fd 2 and exit(127).
 *
 * NOTE: We resolve relocations with BIND_NOW semantics. The kernel
 * also marks libtoby.so itself with DT_FLAGS_1 = NOW, but PLT
 * entries in the program may still appear in DT_JMPREL. We apply
 * them eagerly so that no lazy-binding trampoline is ever needed.
 */

#include "ld_internal.h"

/* ---- Tiny diagnostics --------------------------------------- */

static size_t ld_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static int ld_strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

static void ld_memcpy(void *dst, const void *src, size_t n) {
    uint8_t       *d = dst;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

void ld_puts(const char *s) {
    ld_write(2, s, ld_strlen(s));
}

void ld_die(const char *s) {
    ld_puts("[ld-toby] fatal: ");
    ld_puts(s);
    ld_puts("\n");
    ld_exit(127);
}

/* ---- Module table ------------------------------------------- */

#define LD_MAX_MODULES 8

struct module {
    const char  *name;          /* "<program>" or DT_NEEDED string */
    uint64_t     base;          /* runtime load base */
    Elf64_Dyn   *dynamic;       /* runtime addr of PT_DYNAMIC */
    const char  *strtab;        /* DT_STRTAB (runtime) */
    Elf64_Sym   *symtab;        /* DT_SYMTAB (runtime) */
    uint32_t    *hash;          /* DT_HASH (runtime) -- sysv */
    Elf64_Rela  *rela;          /* DT_RELA (runtime) */
    uint64_t     relasz;        /* DT_RELASZ */
    Elf64_Rela  *jmprel;        /* DT_JMPREL (runtime) */
    uint64_t     pltrelsz;      /* DT_PLTRELSZ */
};

static struct module g_modules[LD_MAX_MODULES];
static int           g_nmodules;

/* ---- DT_HASH (sysv) lookup ---------------------------------- */

static uint32_t sysv_hash(const char *name) {
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* Look up `name` in module `m`. Returns the symbol's defined
 * st_value (already biased by m->base) plus addend offset, or 0 if
 * not found / undefined. */
static uint64_t lookup_in_module(struct module *m, const char *name,
                                 int *out_found) {
    *out_found = 0;
    if (!m->hash || !m->symtab || !m->strtab) return 0;

    uint32_t  nbuckets = m->hash[0];
    uint32_t  nchain   = m->hash[1];
    uint32_t *bucket   = &m->hash[2];
    uint32_t *chain    = &m->hash[2 + nbuckets];

    if (nbuckets == 0) return 0;
    uint32_t h = sysv_hash(name) % nbuckets;

    for (uint32_t idx = bucket[h]; idx != 0 && idx < nchain; idx = chain[idx]) {
        Elf64_Sym  *s    = &m->symtab[idx];
        const char *sn   = m->strtab + s->st_name;
        if (s->st_shndx == SHN_UNDEF) continue;
        if (ld_strcmp(sn, name) != 0) continue;
        *out_found = 1;
        return m->base + s->st_value;
    }
    return 0;
}

/* Search every module in declaration order for a defined symbol.
 * Mirrors glibc's executable-first scope. */
static uint64_t resolve_global(const char *name, int *out_found) {
    for (int i = 0; i < g_nmodules; i++) {
        int found = 0;
        uint64_t v = lookup_in_module(&g_modules[i], name, &found);
        if (found) {
            *out_found = 1;
            return v;
        }
    }
    *out_found = 0;
    return 0;
}

/* ---- Dynamic section parsing -------------------------------- */

static void parse_dynamic(struct module *m) {
    if (!m->dynamic) ld_die("module has no PT_DYNAMIC");
    for (Elf64_Dyn *d = m->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB:   m->strtab   = (const char *)(m->base + d->d_un); break;
        case DT_SYMTAB:   m->symtab   = (Elf64_Sym *)(m->base + d->d_un); break;
        case DT_HASH:     m->hash     = (uint32_t  *)(m->base + d->d_un); break;
        case DT_RELA:     m->rela     = (Elf64_Rela*)(m->base + d->d_un); break;
        case DT_RELASZ:   m->relasz   = d->d_un; break;
        case DT_JMPREL:   m->jmprel   = (Elf64_Rela*)(m->base + d->d_un); break;
        case DT_PLTRELSZ: m->pltrelsz = d->d_un; break;
        default: break;
        }
    }
}

/* ---- Relocation application --------------------------------- */

static void apply_one(struct module *m, Elf64_Rela *r) {
    uint32_t type = ELF64_R_TYPE(r->r_info);
    uint32_t sym  = ELF64_R_SYM(r->r_info);
    uint64_t *slot = (uint64_t *)(m->base + r->r_offset);

    switch (type) {
    case R_X86_64_NONE:
        return;

    case R_X86_64_RELATIVE:
        *slot = m->base + (uint64_t)r->r_addend;
        return;

    case R_X86_64_64:
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT: {
        if (sym == 0) ld_die("reloc against STN_UNDEF");
        const char *name = m->strtab + m->symtab[sym].st_name;
        int found = 0;
        uint64_t v = resolve_global(name, &found);
        if (!found) {
            ld_puts("[ld-toby] unresolved symbol: ");
            ld_puts(name);
            ld_die("");
        }
        if (type == R_X86_64_64)
            *slot = v + (uint64_t)r->r_addend;
        else
            *slot = v;            /* GLOB_DAT and JUMP_SLOT ignore addend */
        return;
    }

    default:
        ld_die("unsupported relocation type");
    }
}

static void apply_module_relocs(struct module *m) {
    if (m->rela && m->relasz) {
        uint64_t n = m->relasz / sizeof(Elf64_Rela);
        for (uint64_t i = 0; i < n; i++) apply_one(m, &m->rela[i]);
    }
    if (m->jmprel && m->pltrelsz) {
        uint64_t n = m->pltrelsz / sizeof(Elf64_Rela);
        for (uint64_t i = 0; i < n; i++) apply_one(m, &m->jmprel[i]);
    }
}

/* ---- Library loading ---------------------------------------- */

static char g_path_buf[256];

static const char *build_lib_path(const char *name) {
    static const char prefix[] = "/lib/";
    size_t plen = sizeof(prefix) - 1;
    size_t nlen = ld_strlen(name);
    if (plen + nlen + 1 > sizeof(g_path_buf)) ld_die("library name too long");
    ld_memcpy(g_path_buf, prefix, plen);
    ld_memcpy(g_path_buf + plen, name, nlen + 1);
    return g_path_buf;
}

static void load_dependency(struct module *prog, const char *name,
                            uint64_t next_base) {
    if (g_nmodules >= LD_MAX_MODULES) ld_die("too many modules");

    const char *path = build_lib_path(name);
    struct abi_dlmap_info info = {0};
    long rc = ld_syscall3(ABI_SYS_DLOAD, (long)(uintptr_t)path,
                          (long)next_base, (long)(uintptr_t)&info);
    if (rc < 0) {
        ld_puts("[ld-toby] dload failed for ");
        ld_puts(path);
        ld_die("");
    }

    struct module *m = &g_modules[g_nmodules++];
    m->name    = name;
    m->base    = info.base;
    m->dynamic = (Elf64_Dyn *)(uintptr_t)info.dynamic;
    parse_dynamic(m);
    (void)prog;
}

/* ---- Program PHDR walk -------------------------------------- */

static Elf64_Dyn *find_program_dynamic(uint64_t phdr_va, uint16_t phnum,
                                       uint16_t phent, uint64_t prog_base) {
    for (uint16_t i = 0; i < phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(phdr_va + (uint64_t)i * phent);
        if (ph->p_type == PT_DYNAMIC) {
            return (Elf64_Dyn *)(prog_base + ph->p_vaddr);
        }
    }
    return 0;
}

/* AT_PHDR is the runtime address of the program header table.
 * AT_BASE is the runtime address of the dynamic linker (us), not
 * the program. We recover the program's load base from PT_PHDR:
 *
 *     program_base = AT_PHDR - PT_PHDR.p_vaddr
 *
 * Every PIE binary lld emits has a PT_PHDR (it's required for the
 * dynamic linker to find PHDRs without an external table). If
 * PT_PHDR is missing we fall back to "AT_PHDR - sizeof(Ehdr)"
 * which works whenever the PHDRs sit immediately after the Ehdr
 * (the only layout we currently produce). */
static uint64_t infer_program_base(uint64_t phdr_va, uint16_t phnum,
                                   uint16_t phent) {
    Elf64_Phdr *ph0 = (Elf64_Phdr *)phdr_va;
    for (uint16_t i = 0; i < phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uintptr_t)ph0 + (uint64_t)i * phent);
        if (ph->p_type == PT_PHDR) {
            return phdr_va - ph->p_vaddr;
        }
    }
    /* Fallback: assume the standard tobyOS layout where PHDRs
     * follow the Ehdr immediately and the first PT_LOAD covers
     * everything. The Ehdr sits at the program load base. */
    return phdr_va - sizeof(Elf64_Ehdr);
}

/* ---- Auxv walk ---------------------------------------------- */

struct auxv_unpacked {
    uint64_t phdr;
    uint64_t phnum;
    uint64_t phent;
    uint64_t base;
    uint64_t entry;
};

static void walk_auxv(uint64_t *sp, struct auxv_unpacked *out) {
    /* Stack layout (little-endian, qwords):
     *    [argc][argv0..argv{argc-1}][NULL]
     *    [envp0..envp{n-1}][NULL]
     *    [a_type][a_val]  ...  [AT_NULL][0]
     */
    uint64_t argc = sp[0];
    uint64_t *p   = &sp[1];                 /* argv */
    p += argc + 1;                          /* skip argv + terminator */
    while (*p) p++;                         /* skip envp */
    p++;                                    /* skip envp terminator */

    /* p now points at auxv */
    while (p[0] != ABI_AT_NULL) {
        switch (p[0]) {
        case ABI_AT_PHDR:  out->phdr  = p[1]; break;
        case ABI_AT_PHNUM: out->phnum = p[1]; break;
        case ABI_AT_PHENT: out->phent = p[1]; break;
        case ABI_AT_BASE:  out->base  = p[1]; break;
        case ABI_AT_ENTRY: out->entry = p[1]; break;
        default: break;
        }
        p += 2;
    }
}

/* ---- ld_main ------------------------------------------------ */

uint64_t ld_main(uint64_t *user_stack_top, uintptr_t self_base) {
    (void)self_base;

    struct auxv_unpacked av = {0};
    walk_auxv(user_stack_top, &av);

    if (!av.phdr || !av.phnum || !av.phent || !av.entry) {
        ld_die("missing auxv entries");
    }

    /* Module 0 = the program. */
    if (g_nmodules != 0) ld_die("ld_main called twice");
    struct module *prog = &g_modules[g_nmodules++];
    prog->name    = "<program>";
    prog->base    = infer_program_base(av.phdr, av.phnum, av.phent);
    prog->dynamic = find_program_dynamic(av.phdr, av.phnum, av.phent,
                                         prog->base);
    if (!prog->dynamic) ld_die("program has no PT_DYNAMIC");
    parse_dynamic(prog);

    /* Walk DT_NEEDED entries and dload each one.
     *
     * Each library is mapped at a fresh VA chosen here by us. We
     * pick a simple bump allocator starting at 0x60000000, with
     * 64MiB-aligned slots so stray relocations don't smear into
     * each other. The kernel's elf_load_user_at() validates that
     * the chosen base + segment vaddrs stay below the user split.
     */
    uint64_t next_base = 0x0000000060000000ULL;
    const uint64_t per_lib_slot = 0x0000000004000000ULL;   /* 64 MiB */

    for (Elf64_Dyn *d = prog->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED) continue;
        const char *name = prog->strtab + d->d_un;
        load_dependency(prog, name, next_base);
        next_base += per_lib_slot;
    }

    /* Apply each library's own relocations first (so program-side
     * lookups see fully-relocated GOTs). */
    for (int i = 1; i < g_nmodules; i++) apply_module_relocs(&g_modules[i]);

    /* Then apply the program's relocations. */
    apply_module_relocs(prog);

    return av.entry;
}
