/* module.c -- loadable kernel module (LKM) system.
 *
 * Loads ELF relocatable objects (ET_REL / .ko files) into kernel
 * memory, resolves symbols against a static kernel export table,
 * processes x86_64 relocations, and calls the module's init function.
 */

#include <tobyos/module.h>
#include <tobyos/elf.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pci.h>
#include <tobyos/blk.h>
#include <tobyos/net.h>
#include <tobyos/spinlock.h>

/* ================================================================
 *  Kernel symbol export table
 * ================================================================ */

struct ksym {
    const char *name;
    void       *addr;
};

#define EXPORT_SYMBOL(sym) { #sym, (void *)(uintptr_t)&sym }

static const struct ksym g_ksyms[] = {
    /* PCI */
    EXPORT_SYMBOL(pci_register_driver),
    EXPORT_SYMBOL(pci_bind_drivers),
    EXPORT_SYMBOL(pci_map_bar),
    EXPORT_SYMBOL(pci_msi_enable),
    EXPORT_SYMBOL(pci_msix_enable),
    EXPORT_SYMBOL(pci_find_dev),
    EXPORT_SYMBOL(pci_find_capability),
    EXPORT_SYMBOL(pci_cfg_read32),
    EXPORT_SYMBOL(pci_cfg_read16),
    EXPORT_SYMBOL(pci_cfg_read8),
    EXPORT_SYMBOL(pci_cfg_write32),
    EXPORT_SYMBOL(pci_cfg_write16),
    EXPORT_SYMBOL(pci_dev_enable),
    EXPORT_SYMBOL(pci_device_count),
    EXPORT_SYMBOL(pci_device_at),

    /* Block */
    EXPORT_SYMBOL(blk_register),

    /* Network */
    EXPORT_SYMBOL(net_register),

    /* Memory */
    EXPORT_SYMBOL(kmalloc),
    EXPORT_SYMBOL(kfree),

    /* Printing */
    EXPORT_SYMBOL(kprintf),

    /* String / memory */
    EXPORT_SYMBOL(memset),
    EXPORT_SYMBOL(memcpy),
    EXPORT_SYMBOL(memmove),
    EXPORT_SYMBOL(memcmp),
    EXPORT_SYMBOL(strcmp),
    EXPORT_SYMBOL(strncmp),
    EXPORT_SYMBOL(strlen),
    EXPORT_SYMBOL(ksnprintf),

    { NULL, NULL }
};

static void *ksym_lookup(const char *name) {
    for (int i = 0; g_ksyms[i].name; i++) {
        if (strcmp(g_ksyms[i].name, name) == 0)
            return g_ksyms[i].addr;
    }
    return NULL;
}

/* ================================================================
 *  Module tracking
 * ================================================================ */

static struct loaded_module g_modules[MODULE_MAX];
static spinlock_t           g_mod_lock = SPINLOCK_INIT;

void module_init(void) {
    memset(g_modules, 0, sizeof(g_modules));
    kprintf("[module] kernel module subsystem initialized (%d slots)\n",
            MODULE_MAX);
}

struct loaded_module *module_find(const char *name) {
    for (int i = 0; i < MODULE_MAX; i++) {
        if (g_modules[i].in_use && strcmp(g_modules[i].name, name) == 0)
            return &g_modules[i];
    }
    return NULL;
}

static struct loaded_module *module_alloc_slot(void) {
    for (int i = 0; i < MODULE_MAX; i++) {
        if (!g_modules[i].in_use)
            return &g_modules[i];
    }
    return NULL;
}

/* ================================================================
 *  Section name helper
 * ================================================================ */

static const char *shdr_name(const Elf64_Ehdr *ehdr, const uint8_t *data,
                             const Elf64_Shdr *sh) {
    if (ehdr->e_shstrndx == 0) return "";
    const Elf64_Shdr *strtab_sh = (const Elf64_Shdr *)
        (data + ehdr->e_shoff + ehdr->e_shstrndx * ehdr->e_shentsize);
    if (sh->sh_name >= strtab_sh->sh_size) return "";
    return (const char *)(data + strtab_sh->sh_offset + sh->sh_name);
}

/* ================================================================
 *  ELF ET_REL loader
 * ================================================================ */

int module_load(const char *path) {
    void  *filedata = NULL;
    size_t filesz   = 0;

    int rc = vfs_read_all(path, &filedata, &filesz);
    if (rc != VFS_OK) {
        kprintf("[module] failed to read %s: %d\n", path, rc);
        return -1;
    }

    const uint8_t *data = (const uint8_t *)filedata;

    /* --- Validate ELF header --- */
    if (filesz < sizeof(Elf64_Ehdr)) {
        kprintf("[module] %s: too small for ELF header\n", path);
        kfree(filedata);
        return -1;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        kprintf("[module] %s: bad ELF magic\n", path);
        kfree(filedata);
        return -1;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA]  != ELFDATA2LSB) {
        kprintf("[module] %s: not ELF64 little-endian\n", path);
        kfree(filedata);
        return -1;
    }

    if (ehdr->e_type != ET_REL) {
        kprintf("[module] %s: not ET_REL (type=%u)\n", path, ehdr->e_type);
        kfree(filedata);
        return -1;
    }

    if (ehdr->e_machine != EM_X86_64) {
        kprintf("[module] %s: not x86_64 (machine=%u)\n", path, ehdr->e_machine);
        kfree(filedata);
        return -1;
    }

    uint16_t shnum = ehdr->e_shnum;
    if (shnum == 0 || ehdr->e_shoff == 0) {
        kprintf("[module] %s: no section headers\n", path);
        kfree(filedata);
        return -1;
    }

    if (ehdr->e_shoff + (uint64_t)shnum * ehdr->e_shentsize > filesz) {
        kprintf("[module] %s: section headers beyond file\n", path);
        kfree(filedata);
        return -1;
    }

    /* --- Calculate total size needed for ALLOC sections --- */
    const Elf64_Shdr *shdrs = (const Elf64_Shdr *)(data + ehdr->e_shoff);
    size_t total_alloc = 0;

    for (uint16_t i = 0; i < shnum; i++) {
        if (!(shdrs[i].sh_flags & SHF_ALLOC))
            continue;
        size_t align = shdrs[i].sh_addralign;
        if (align < 1) align = 1;
        total_alloc = (total_alloc + align - 1) & ~(align - 1);
        total_alloc += shdrs[i].sh_size;
    }

    if (total_alloc == 0) {
        kprintf("[module] %s: no ALLOC sections\n", path);
        kfree(filedata);
        return -1;
    }

    /* Allocate kernel memory for the module code/data. */
    uint8_t *mod_base = (uint8_t *)kmalloc(total_alloc);
    if (!mod_base) {
        kprintf("[module] %s: failed to allocate %zu bytes\n", path, total_alloc);
        kfree(filedata);
        return -1;
    }
    memset(mod_base, 0, total_alloc);

    /* --- Per-section runtime addresses (for relocation resolution) --- */
    uint64_t *sec_addrs = (uint64_t *)kmalloc(shnum * sizeof(uint64_t));
    if (!sec_addrs) {
        kfree(mod_base);
        kfree(filedata);
        return -1;
    }
    memset(sec_addrs, 0, shnum * sizeof(uint64_t));

    /* --- Copy ALLOC sections into module memory --- */
    size_t offset = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        if (!(shdrs[i].sh_flags & SHF_ALLOC))
            continue;
        size_t align = shdrs[i].sh_addralign;
        if (align < 1) align = 1;
        offset = (offset + align - 1) & ~(align - 1);
        sec_addrs[i] = (uint64_t)(uintptr_t)(mod_base + offset);
        if (shdrs[i].sh_type != SHT_NOBITS) {
            if (shdrs[i].sh_offset + shdrs[i].sh_size > filesz) {
                kprintf("[module] %s: section %u truncated\n", path, i);
                kfree(sec_addrs);
                kfree(mod_base);
                kfree(filedata);
                return -1;
            }
            memcpy(mod_base + offset, data + shdrs[i].sh_offset,
                   shdrs[i].sh_size);
        }
        offset += shdrs[i].sh_size;
    }

    /* --- Locate the symbol table --- */
    const Elf64_Shdr *symtab_sh = NULL;
    const char       *symstrtab = NULL;
    for (uint16_t i = 0; i < shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab_sh = &shdrs[i];
            if (shdrs[i].sh_link < shnum)
                symstrtab = (const char *)(data + shdrs[shdrs[i].sh_link].sh_offset);
            break;
        }
    }

    if (!symtab_sh || !symstrtab) {
        kprintf("[module] %s: no symbol table\n", path);
        kfree(sec_addrs);
        kfree(mod_base);
        kfree(filedata);
        return -1;
    }

    uint32_t sym_count = (uint32_t)(symtab_sh->sh_size / symtab_sh->sh_entsize);
    const Elf64_Sym *syms = (const Elf64_Sym *)(data + symtab_sh->sh_offset);

    /* --- Process relocations --- */
    for (uint16_t i = 0; i < shnum; i++) {
        if (shdrs[i].sh_type != SHT_RELA)
            continue;

        uint16_t target_idx = (uint16_t)shdrs[i].sh_info;
        if (target_idx >= shnum || sec_addrs[target_idx] == 0)
            continue;

        uint64_t target_base = sec_addrs[target_idx];
        uint32_t rela_count = (uint32_t)(shdrs[i].sh_size / shdrs[i].sh_entsize);
        const Elf64_Rela *relas = (const Elf64_Rela *)(data + shdrs[i].sh_offset);

        for (uint32_t r = 0; r < rela_count; r++) {
            uint32_t sym_idx  = (uint32_t)ELF64_R_SYM(relas[r].r_info);
            uint32_t rel_type = (uint32_t)ELF64_R_TYPE(relas[r].r_info);

            if (sym_idx >= sym_count) {
                kprintf("[module] %s: reloc sym index %u out of range\n",
                        path, sym_idx);
                kfree(sec_addrs);
                kfree(mod_base);
                kfree(filedata);
                return -1;
            }

            /* Resolve symbol value. */
            uint64_t sym_val = 0;
            const Elf64_Sym *sym = &syms[sym_idx];

            if (sym->st_shndx == SHN_UNDEF) {
                const char *sym_name = symstrtab + sym->st_name;
                void *kaddr = ksym_lookup(sym_name);
                if (!kaddr) {
                    kprintf("[module] %s: unresolved symbol '%s'\n",
                            path, sym_name);
                    kfree(sec_addrs);
                    kfree(mod_base);
                    kfree(filedata);
                    return -1;
                }
                sym_val = (uint64_t)(uintptr_t)kaddr;
            } else if (sym->st_shndx == SHN_ABS) {
                sym_val = sym->st_value;
            } else if (sym->st_shndx < shnum) {
                if (ELF64_ST_TYPE(sym->st_info) == STT_SECTION) {
                    sym_val = sec_addrs[sym->st_shndx];
                } else {
                    sym_val = sec_addrs[sym->st_shndx] + sym->st_value;
                }
            } else {
                kprintf("[module] %s: bad symbol section index %u\n",
                        path, sym->st_shndx);
                kfree(sec_addrs);
                kfree(mod_base);
                kfree(filedata);
                return -1;
            }

            /* Apply the relocation. */
            uint8_t *patch = (uint8_t *)(uintptr_t)(target_base + relas[r].r_offset);
            uint64_t S = sym_val;
            int64_t  A = relas[r].r_addend;
            uint64_t P = (uint64_t)(uintptr_t)patch;

            switch (rel_type) {
            case R_X86_64_64:
                *(uint64_t *)patch = S + A;
                break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                int64_t val = (int64_t)(S + A) - (int64_t)P;
                *(int32_t *)patch = (int32_t)val;
                break;
            }
            case R_X86_64_32S:
                *(int32_t *)patch = (int32_t)(S + A);
                break;
            case R_X86_64_NONE:
                break;
            default:
                kprintf("[module] %s: unsupported reloc type %u\n",
                        path, rel_type);
                kfree(sec_addrs);
                kfree(mod_base);
                kfree(filedata);
                return -1;
            }
        }
    }

    /* --- Extract module metadata from special sections --- */
    int  (*mod_init_fn)(void) = NULL;
    void (*mod_fini_fn)(void) = NULL;
    const char *mod_name = NULL;
    const char *mod_ver  = NULL;

    for (uint16_t i = 0; i < shnum; i++) {
        const char *name = shdr_name(ehdr, data, &shdrs[i]);
        if (strcmp(name, ".toby_mod_init") == 0 && sec_addrs[i]) {
            mod_init_fn = *(int (**)(void))(uintptr_t)sec_addrs[i];
        } else if (strcmp(name, ".toby_mod_fini") == 0 && sec_addrs[i]) {
            mod_fini_fn = *(void (**)(void))(uintptr_t)sec_addrs[i];
        } else if (strcmp(name, ".toby_mod_name") == 0 && sec_addrs[i]) {
            mod_name = (const char *)(uintptr_t)sec_addrs[i];
        } else if (strcmp(name, ".toby_mod_ver") == 0 && sec_addrs[i]) {
            mod_ver = (const char *)(uintptr_t)sec_addrs[i];
        }
    }

    if (!mod_name || mod_name[0] == '\0') {
        /* Fall back: derive a name from the filename. */
        const char *slash = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') slash = p + 1;
        mod_name = slash;
    }

    if (!mod_ver) mod_ver = "0.0";

    /* Check for duplicates. */
    uint64_t flags = spin_lock_irqsave(&g_mod_lock);
    if (module_find(mod_name)) {
        spin_unlock_irqrestore(&g_mod_lock, flags);
        kprintf("[module] %s: module '%s' already loaded\n", path, mod_name);
        kfree(sec_addrs);
        kfree(mod_base);
        kfree(filedata);
        return -1;
    }

    struct loaded_module *slot = module_alloc_slot();
    if (!slot) {
        spin_unlock_irqrestore(&g_mod_lock, flags);
        kprintf("[module] %s: no free module slots\n", path);
        kfree(sec_addrs);
        kfree(mod_base);
        kfree(filedata);
        return -1;
    }

    slot->in_use = true;
    size_t nlen = strlen(mod_name);
    if (nlen >= MODULE_NAME_MAX) nlen = MODULE_NAME_MAX - 1;
    memcpy(slot->name, mod_name, nlen);
    slot->name[nlen] = '\0';
    slot->base   = mod_base;
    slot->size   = total_alloc;
    slot->init   = mod_init_fn;
    slot->fini   = mod_fini_fn;
    slot->initialized = false;
    spin_unlock_irqrestore(&g_mod_lock, flags);

    kprintf("[module] loaded '%s' v%s (%zu bytes at %p)\n",
            slot->name, mod_ver, total_alloc, mod_base);

    /* Call init function. */
    if (mod_init_fn) {
        rc = mod_init_fn();
        if (rc != 0) {
            kprintf("[module] '%s' init() returned %d -- unloading\n",
                    slot->name, rc);
            kfree(sec_addrs);
            kfree(filedata);
            /* Clean up the slot. */
            flags = spin_lock_irqsave(&g_mod_lock);
            slot->in_use = false;
            spin_unlock_irqrestore(&g_mod_lock, flags);
            kfree(mod_base);
            return -1;
        }
        slot->initialized = true;
    }

    kfree(sec_addrs);
    kfree(filedata);
    return 0;
}

/* ================================================================
 *  Module unload
 * ================================================================ */

int module_unload(const char *name) {
    uint64_t flags = spin_lock_irqsave(&g_mod_lock);
    struct loaded_module *m = module_find(name);
    if (!m) {
        spin_unlock_irqrestore(&g_mod_lock, flags);
        kprintf("[module] unload: '%s' not found\n", name);
        return -1;
    }

    if (m->initialized && m->fini)
        m->fini();

    void *base = m->base;
    kprintf("[module] unloaded '%s'\n", m->name);
    m->in_use = false;
    m->initialized = false;
    m->base = NULL;
    spin_unlock_irqrestore(&g_mod_lock, flags);

    kfree(base);
    return 0;
}

/* ================================================================
 *  Load all .ko from /lib/modules/
 * ================================================================ */

int module_load_all(void) {
    struct vfs_dir dir;
    int rc = vfs_opendir("/lib/modules", &dir);
    if (rc != VFS_OK) {
        kprintf("[module] /lib/modules not found -- no modules to load\n");
        return 0;
    }

    int loaded = 0;
    struct vfs_dirent ent;
    while (vfs_readdir(&dir, &ent) == VFS_OK) {
        if (ent.type != VFS_TYPE_FILE)
            continue;
        size_t nlen = strlen(ent.name);
        if (nlen < 4)
            continue;
        if (strcmp(ent.name + nlen - 3, ".ko") != 0)
            continue;

        char fullpath[VFS_PATH_MAX];
        ksnprintf(fullpath, sizeof(fullpath), "/lib/modules/%s", ent.name);
        if (module_load(fullpath) == 0)
            loaded++;
    }

    vfs_closedir(&dir);

    if (loaded > 0)
        kprintf("[module] loaded %d module(s) from /lib/modules/\n", loaded);

    return loaded;
}

/* ================================================================
 *  Module listing
 * ================================================================ */

void module_list(void) {
    kprintf("[module] loaded modules:\n");
    int count = 0;
    uint64_t flags = spin_lock_irqsave(&g_mod_lock);
    for (int i = 0; i < MODULE_MAX; i++) {
        if (!g_modules[i].in_use) continue;
        kprintf("  [%d] %-20s %6zu bytes at %p%s\n",
                i, g_modules[i].name,
                g_modules[i].size, g_modules[i].base,
                g_modules[i].initialized ? " [active]" : " [loaded]");
        count++;
    }
    spin_unlock_irqrestore(&g_mod_lock, flags);
    if (count == 0)
        kprintf("  (none)\n");
}

/* ================================================================
 *  Syscall interface (SYS_MODULE)
 * ================================================================ */

long sys_module(uint64_t op, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    switch (op) {
    case 0: {
        const char *upath = (const char *)(uintptr_t)arg1;
        if (!upath) return -1;
        char kpath[VFS_PATH_MAX];
        size_t i;
        for (i = 0; i < VFS_PATH_MAX - 1; i++) {
            kpath[i] = upath[i];
            if (kpath[i] == '\0') break;
        }
        kpath[VFS_PATH_MAX - 1] = '\0';
        return module_load(kpath);
    }
    case 1: {
        const char *uname = (const char *)(uintptr_t)arg1;
        if (!uname) return -1;
        char kname[MODULE_NAME_MAX];
        size_t i;
        for (i = 0; i < MODULE_NAME_MAX - 1; i++) {
            kname[i] = uname[i];
            if (kname[i] == '\0') break;
        }
        kname[MODULE_NAME_MAX - 1] = '\0';
        return module_unload(kname);
    }
    case 2:
        module_list();
        return 0;
    default:
        return -1;
    }
}
