/* module.c -- loadable kernel module (LKM) system.
 *
 * Loads ELF relocatable objects (ET_REL / .ko files) into kernel
 * memory, resolves symbols against a static kernel export table,
 * processes x86_64 relocations, and calls the module's init function.
 */

#include <tobyos/module.h>
#include <tobyos/proc.h>
#include <tobyos/abi/abi.h>
#include <tobyos/elf.h>
#include <tobyos/vfs.h>
#include <tobyos/uaccess.h>
#include <tobyos/heap.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/apic.h>   /* tlb_shootdown_remote on module free */
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

const struct loaded_module *module_slot(int i) {
    if (i < 0 || i >= MODULE_MAX) return NULL;
    return &g_modules[i];
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

/* ================================================================
 *  Executable module memory (2026-09-04)
 *
 *  The heap is mapped NX, and a module is CODE: the first real module ever
 *  loaded (hello.ko, from /lib/modules at boot) faulted on its first
 *  instruction fetch. Modules are built -mcmodel=kernel like the kernel, so
 *  their PC32/32S relocations only reach the top 2 GiB: the window sits
 *  1 GiB above the kernel image, one 2 MiB slot per module, backed by PMM
 *  pages mapped writable+executable, supervisor only.
 * ================================================================ */
#define MODULE_VA_BASE   0xffffffffc0000000ull
#define MODULE_VA_STRIDE (2ull << 20)
static bool g_va_used[MODULE_MAX];

static void module_free_exec(void *base, uint64_t *pages, size_t npages, int va_slot) {
    if (base && npages) {
        vmm_unmap((uint64_t)(uintptr_t)base, npages * PAGE_SIZE);
        /* vmm_unmap does a LOCAL invlpg only (see vmm.h): the other CPUs are
         * still holding writable translations for this window. The pages are
         * about to go back to the PMM and be handed to somebody else, and the
         * VA is about to be reused by the next insmod at the same address --
         * both are frame changes, so every peer must drop its entries FIRST.
         *
         * Found on the 4-core EliteDesk: `rmmod hello` then `insmod` hung,
         * where QEMU's software TLB had never reproduced it. Ordering is
         * load-bearing -- shoot down BEFORE pmm_free_page, or a peer can
         * still write through a stale entry into reallocated memory. */
        tlb_shootdown_remote();
        for (size_t i = 0; i < npages; i++) if (pages && pages[i]) pmm_free_page(pages[i]);
    }
    if (pages) kfree(pages);
    if (va_slot >= 0 && va_slot < MODULE_MAX) g_va_used[va_slot] = false;
}

static void *module_alloc_exec(size_t bytes, uint64_t **pages_out, size_t *npages_out, int *va_slot_out) {
    *pages_out = NULL; *npages_out = 0; *va_slot_out = -1;
    if (bytes == 0 || bytes > MODULE_VA_STRIDE) return NULL;
    int slot = -1;
    for (int i = 0; i < MODULE_MAX; i++) if (!g_va_used[i]) { slot = i; break; }
    if (slot < 0) return NULL;
    size_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *pages = (uint64_t *)kmalloc(npages * sizeof(uint64_t));
    if (!pages) return NULL;
    memset(pages, 0, npages * sizeof(uint64_t));
    uint64_t va = MODULE_VA_BASE + (uint64_t)slot * MODULE_VA_STRIDE;
    for (size_t i = 0; i < npages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys || !vmm_map(va + i * PAGE_SIZE, phys, PAGE_SIZE, VMM_WRITE)) {
            if (phys) pmm_free_page(phys);
            if (i) { vmm_unmap(va, i * PAGE_SIZE); for (size_t k = 0; k < i; k++) pmm_free_page(pages[k]); }
            kfree(pages);
            return NULL;
        }
        pages[i] = phys;
    }
    g_va_used[slot] = true;
    memset((void *)(uintptr_t)va, 0, npages * PAGE_SIZE);
    *pages_out = pages; *npages_out = npages; *va_slot_out = slot;
    return (void *)(uintptr_t)va;
}

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
    /* (the executable window allocator lives above; see module_alloc_exec) */
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

    /* Executable supervisor memory for the module code/data: NOT the heap. */
    uint64_t *mod_pages = NULL; size_t mod_npages = 0; int va_slot = -1;
    uint8_t *mod_base = (uint8_t *)module_alloc_exec(total_alloc, &mod_pages, &mod_npages, &va_slot);
    if (!mod_base) {
        kprintf("[module] %s: failed to allocate %lu bytes of executable memory\n", path, (unsigned long)total_alloc);
        kfree(filedata);
        return -1;
    }

    /* --- Per-section runtime addresses (for relocation resolution) --- */
    uint64_t *sec_addrs = (uint64_t *)kmalloc(shnum * sizeof(uint64_t));
    if (!sec_addrs) {
        module_free_exec(mod_base, mod_pages, mod_npages, va_slot);
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
                module_free_exec(mod_base, mod_pages, mod_npages, va_slot);
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
        module_free_exec(mod_base, mod_pages, mod_npages, va_slot);
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
                module_free_exec(mod_base, mod_pages, mod_npages, va_slot);
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
                    module_free_exec(mod_base, mod_pages, mod_npages, va_slot);
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
                module_free_exec(mod_base, mod_pages, mod_npages, va_slot);
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
                module_free_exec(mod_base, mod_pages, mod_npages, va_slot);
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
        /* "hello.ko" is the FILE; the module is "hello" (lsmod, rmmod) */
        static char nbuf[MODULE_NAME_MAX];
        size_t nl = strlen(slash);
        if (nl > 3 && strcmp(slash + nl - 3, ".ko") == 0) nl -= 3;
        if (nl >= MODULE_NAME_MAX) nl = MODULE_NAME_MAX - 1;
        memcpy(nbuf, slash, nl); nbuf[nl] = '\0';
        mod_name = nbuf;
    }

    if (!mod_ver) mod_ver = "0.0";

    /* Check for duplicates. */
    uint64_t flags = spin_lock_irqsave(&g_mod_lock);
    if (module_find(mod_name)) {
        spin_unlock_irqrestore(&g_mod_lock, flags);
        kprintf("[module] %s: module '%s' already loaded\n", path, mod_name);
        kfree(sec_addrs);
        module_free_exec(mod_base, mod_pages, mod_npages, va_slot);
        kfree(filedata);
        return -1;
    }

    struct loaded_module *slot = module_alloc_slot();
    if (!slot) {
        spin_unlock_irqrestore(&g_mod_lock, flags);
        kprintf("[module] %s: no free module slots\n", path);
        kfree(sec_addrs);
        module_free_exec(mod_base, mod_pages, mod_npages, va_slot);
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
    slot->pages  = mod_pages;
    slot->npages = mod_npages;
    slot->va_slot = va_slot;
    slot->init   = mod_init_fn;
    slot->fini   = mod_fini_fn;
    slot->initialized = false;
    spin_unlock_irqrestore(&g_mod_lock, flags);

    kprintf("[module] loaded '%s' v%s (%lu bytes at %p)\n",
            slot->name, mod_ver, (unsigned long)total_alloc, mod_base);

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
            module_free_exec(mod_base, mod_pages, mod_npages, va_slot);
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
    uint64_t *pages = m->pages; size_t npages = m->npages; int va_slot = m->va_slot;
    kprintf("[module] unloaded '%s'\n", m->name);
    m->in_use = false;
    m->initialized = false;
    m->base = NULL; m->pages = NULL; m->npages = 0; m->va_slot = -1;
    spin_unlock_irqrestore(&g_mod_lock, flags);

    module_free_exec(base, pages, npages, va_slot);
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
    /* A module is ring-0 code relocated against the kernel symbol table.
     * Loading one is exactly as privileged as the kernel itself; before
     * 2026-09-04 any uid could insmod. */
    { struct proc *me = current_proc();
      if (!me || me->uid != 0) return -ABI_EPERM; }
    (void)arg2;
    switch (op) {
    case 0: {
        char kpath[VFS_PATH_MAX];
        if (strncpy_from_user(kpath, (const char *)(uintptr_t)arg1,
                              sizeof(kpath)) < 0) return -1;
        {   /* the errno insmod/modprobe report: EEXIST, ENOENT, EINVAL */
            const char *slash = kpath;
            for (const char *p = kpath; *p; p++) if (*p == '/') slash = p + 1;
            char nm[MODULE_NAME_MAX]; size_t nl = strlen(slash);
            if (nl > 3 && strcmp(slash + nl - 3, ".ko") == 0) nl -= 3;
            if (nl >= MODULE_NAME_MAX) nl = MODULE_NAME_MAX - 1;
            memcpy(nm, slash, nl); nm[nl] = '\0';
            if (module_find(nm)) return -ABI_EEXIST;
            struct vfs_stat vs;
            if (vfs_stat(kpath, &vs) != VFS_OK) return -ABI_ENOENT;
            return module_load(kpath) == 0 ? 0 : -ABI_EINVAL;
        }
    }
    case 1: {
        char kname[MODULE_NAME_MAX];
        if (strncpy_from_user(kname, (const char *)(uintptr_t)arg1,
                              sizeof(kname)) < 0) return -1;
        if (!module_find(kname)) return -ABI_ENOENT;
        return module_unload(kname) == 0 ? 0 : -ABI_EBUSY;
    }
    case 2:
        module_list();
        return 0;
    default:
        return -1;
    }
}
