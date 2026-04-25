/* elf.c -- minimal ELF64 loader (kernel + user variants).
 *
 * Strategy:
 *   1. Validate the Ehdr aggressively. Anything we don't fully support
 *      gets rejected up front; we never "best-effort" load a weird ELF.
 *   2. Walk PT_LOAD program headers. For each one:
 *        - Compute page-aligned [virt_lo, virt_hi).
 *        - For each page in that range that isn't already mapped from
 *          a previous segment of the same ELF, alloc a frame from the
 *          PMM and vmm_map it RW (we need to write the file bytes).
 *          For user mode, the mapping carries VMM_USER so CPL=3 can
 *          touch it.
 *        - memcpy the file contents in, memset the BSS tail.
 *   3. Second pass: vmm_protect each PT_LOAD range to its real
 *      permissions (PF_X -> exec, PF_W -> writable, otherwise R/NX).
 *      The user/kernel bit is preserved (it's part of the flags we
 *      pass).
 *   4. Return e_entry.
 *
 * Address space restrictions:
 *   kernel mode: vaddr must be in the kernel half (>= KERNEL_HALF_MIN)
 *   user mode  : vaddr must be > 0 and vaddr+memsz <= USER_HALF_MAX
 *                (NULL-page reserved; non-canonical addresses refused)
 *
 * Milestone 25D: the user-mode variant also accepts ET_DYN. Every
 * p_vaddr is biased by a caller-chosen load_base before mapping, so
 * the same image can be loaded at any non-overlapping user-half
 * address. We don't process PT_DYNAMIC ourselves -- relocations are
 * applied by /lib/ld-toby.so in user mode after the kernel has
 * faulted both program and interpreter into memory and packed an
 * auxv. The kernel only learns the layout it must report (entry,
 * load_base, PHDR table location, PT_INTERP presence); the actual
 * link work happens in user space.
 */

#include <tobyos/elf.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define KERNEL_HALF_MIN 0xFFFF800000000000ULL
#define USER_HALF_MAX   0x0000800000000000ULL

static inline uint64_t round_down(uint64_t x, uint64_t a) {
    return x & ~(a - 1);
}
static inline uint64_t round_up(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}

static bool validate_ehdr(const Elf64_Ehdr *e, size_t size, bool user_mode,
                          uint64_t load_base) {
    if (size < sizeof(Elf64_Ehdr)) {
        kprintf("[elf] reject: image (%lu B) smaller than Ehdr\n",
                (unsigned long)size);
        return false;
    }
    if (e->e_ident[EI_MAG0] != ELFMAG0 ||
        e->e_ident[EI_MAG1] != ELFMAG1 ||
        e->e_ident[EI_MAG2] != ELFMAG2 ||
        e->e_ident[EI_MAG3] != ELFMAG3) {
        kprintf("[elf] reject: bad ELF magic\n");
        return false;
    }
    if (e->e_ident[EI_CLASS] != ELFCLASS64) {
        kprintf("[elf] reject: not ELF64 (class=%u)\n", e->e_ident[EI_CLASS]);
        return false;
    }
    if (e->e_ident[EI_DATA] != ELFDATA2LSB) {
        kprintf("[elf] reject: not little-endian (data=%u)\n", e->e_ident[EI_DATA]);
        return false;
    }
    if (e->e_ident[EI_VERSION] != EV_CURRENT) {
        kprintf("[elf] reject: unknown EI_VERSION %u\n", e->e_ident[EI_VERSION]);
        return false;
    }
    if (e->e_machine != EM_X86_64) {
        kprintf("[elf] reject: not x86_64 (machine=%u)\n", e->e_machine);
        return false;
    }
    if (user_mode) {
        if (e->e_type != ET_EXEC && e->e_type != ET_DYN) {
            kprintf("[elf] reject (user): e_type=%u, only ET_EXEC/ET_DYN "
                    "supported\n", e->e_type);
            return false;
        }
        if (e->e_type == ET_EXEC && load_base != 0) {
            kprintf("[elf] reject (user): ET_EXEC must be loaded at "
                    "vaddr 0 base, got load_base=%p\n", (void *)load_base);
            return false;
        }
    } else {
        if (e->e_type != ET_EXEC) {
            kprintf("[elf] reject (kernel): e_type=%u, only ET_EXEC supported\n",
                    e->e_type);
            return false;
        }
        if (load_base != 0) {
            kprintf("[elf] reject (kernel): non-zero load_base unsupported\n");
            return false;
        }
    }
    if (e->e_phentsize != sizeof(Elf64_Phdr)) {
        kprintf("[elf] reject: weird e_phentsize %u (expected %lu)\n",
                e->e_phentsize, (unsigned long)sizeof(Elf64_Phdr));
        return false;
    }
    if (e->e_phnum == 0) {
        kprintf("[elf] reject: no program headers\n");
        return false;
    }
    /* phoff + phnum * phentsize must fit inside the image. */
    uint64_t ph_end = (uint64_t)e->e_phoff +
                      (uint64_t)e->e_phnum * (uint64_t)e->e_phentsize;
    if (ph_end > size) {
        kprintf("[elf] reject: program headers run off end of image "
                "(end=%lu, size=%lu)\n",
                (unsigned long)ph_end, (unsigned long)size);
        return false;
    }
    if (user_mode) {
        uint64_t entry = (e->e_type == ET_DYN)
                             ? (e->e_entry + load_base)
                             : e->e_entry;
        if (entry == 0 || entry >= USER_HALF_MAX) {
            kprintf("[elf] reject (user): entry %p outside (0, %p)\n",
                    (void *)entry, (void *)USER_HALF_MAX);
            return false;
        }
    } else {
        if (e->e_entry < KERNEL_HALF_MIN) {
            kprintf("[elf] reject (kernel): entry %p not in kernel half\n",
                    (void *)e->e_entry);
            return false;
        }
    }
    return true;
}

/* Convert PF_* to vmm flags. Read is implicit; everything is at least
 * present + NX-by-default. Caller adds VMM_USER if needed. */
static uint32_t phdr_to_vmm_flags(uint32_t p_flags, bool user_mode) {
    uint32_t f = VMM_PRESENT;
    if (p_flags & PF_W) f |= VMM_WRITE;
    if (!(p_flags & PF_X)) f |= VMM_NX;
    if (user_mode)        f |= VMM_USER;
    return f;
}

/* Make every page of [virt_lo, virt_hi) writable from the kernel side
 * so we can memcpy/memset into it. The real per-segment permissions
 * are applied in pass 2 via vmm_protect.
 *
 * Two cases per page:
 *   - unmapped: alloc a phys frame and vmm_map RW (with VMM_USER if
 *     user_mode).
 *   - already mapped (e.g. left over from a previous load of the same
 *     ELF, or from an overlapping earlier PT_LOAD): vmm_protect it
 *     back to RW. CR0.WP=1 means even CPL=0 honours R/O bits, so this
 *     step is necessary to re-load any program whose .text was made
 *     R-X by the previous pass-2. */
static bool ensure_writable_pages(uint64_t virt_lo, uint64_t virt_hi,
                                  bool user_mode) {
    uint32_t base = VMM_PRESENT | VMM_WRITE | VMM_NX;
    if (user_mode) base |= VMM_USER;
    for (uint64_t v = virt_lo; v < virt_hi; v += PAGE_SIZE) {
        if (vmm_translate(v) != 0) {
            if (!vmm_protect(v, PAGE_SIZE, base)) {
                kprintf("[elf] vmm_protect failed re-arming RW at virt %p\n",
                        (void *)v);
                return false;
            }
            continue;
        }
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            kprintf("[elf] OOM allocating page for virt %p\n", (void *)v);
            return false;
        }
        if (!vmm_map(v, phys, PAGE_SIZE, base)) {
            kprintf("[elf] vmm_map failed at virt %p\n", (void *)v);
            pmm_free_page(phys);
            return false;
        }
    }
    return true;
}

static bool do_elf_load(const void *image, size_t size, bool user_mode,
                        uint64_t load_base, struct elf_load_info *info) {
    if (!image || !info) return false;
    info->entry      = 0;
    info->load_base  = load_base;
    info->phdr_va    = 0;
    info->phnum      = 0;
    info->phent      = 0;
    info->has_interp = false;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    if (!validate_ehdr(eh, size, user_mode, load_base)) return false;

    const uint8_t *bytes = (const uint8_t *)image;
    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(bytes + eh->e_phoff);

    /* For ET_DYN we bias every vaddr by load_base. ET_EXEC never has a
     * non-zero load_base (validate_ehdr enforced it). */
    const uint64_t bias = (eh->e_type == ET_DYN) ? load_base : 0;

    /* Track the lowest PT_LOAD vaddr (post-bias) so we can pick a
     * fallback PHDR address when there's no PT_PHDR -- the convention
     * is that PHDRs land at first_load + e_phoff if e_phoff falls
     * inside the first PT_LOAD's [offset, offset+filesz) range. */
    uint64_t first_load_vaddr   = 0;
    uint64_t first_load_offset  = 0;
    uint64_t first_load_filesz  = 0;
    bool     have_first_load    = false;

    /* PT_PHDR explicit answer wins over the heuristic above. */
    uint64_t pt_phdr_vaddr = 0;
    bool     have_pt_phdr  = false;

    /* ---- pass 1: copy PT_LOAD segments into freshly mapped, RW pages ---- */
    int loaded = 0;
    for (Elf64_Half i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];

        if (ph->p_type == PT_INTERP) {
            info->has_interp = true;
            continue;
        }
        if (ph->p_type == PT_PHDR) {
            pt_phdr_vaddr = ph->p_vaddr + bias;
            have_pt_phdr  = true;
            continue;
        }
        if (ph->p_type != PT_LOAD) continue;

        if (ph->p_filesz > ph->p_memsz) {
            kprintf("[elf] reject: phdr[%u] filesz %lu > memsz %lu\n",
                    i, (unsigned long)ph->p_filesz, (unsigned long)ph->p_memsz);
            return false;
        }
        if ((uint64_t)ph->p_offset + ph->p_filesz > size) {
            kprintf("[elf] reject: phdr[%u] file range runs off image\n", i);
            return false;
        }

        uint64_t seg_vaddr = ph->p_vaddr + bias;
        if (seg_vaddr < ph->p_vaddr) {
            /* unsigned overflow from bias addition */
            kprintf("[elf] reject: phdr[%u] vaddr+base wraps\n", i);
            return false;
        }

        if (user_mode) {
            if (seg_vaddr == 0 || seg_vaddr >= USER_HALF_MAX ||
                seg_vaddr + ph->p_memsz > USER_HALF_MAX) {
                kprintf("[elf] reject (user): phdr[%u] vaddr %p memsz %lu "
                        "outside user half\n",
                        i, (void *)seg_vaddr, (unsigned long)ph->p_memsz);
                return false;
            }
        } else {
            if (seg_vaddr < KERNEL_HALF_MIN) {
                kprintf("[elf] reject (kernel): phdr[%u] vaddr %p not in "
                        "kernel half\n", i, (void *)seg_vaddr);
                return false;
            }
        }
        if (ph->p_memsz == 0) continue;

        if (!have_first_load || seg_vaddr < first_load_vaddr) {
            first_load_vaddr  = seg_vaddr;
            first_load_offset = ph->p_offset;
            first_load_filesz = ph->p_filesz;
            have_first_load   = true;
        }

        uint64_t virt_lo = round_down(seg_vaddr,                 PAGE_SIZE);
        uint64_t virt_hi = round_up  (seg_vaddr + ph->p_memsz,   PAGE_SIZE);

        kprintf("[elf] phdr[%u] LOAD vaddr=%p memsz=0x%lx filesz=0x%lx "
                "flags=%c%c%c%s -> pages [%p..%p)\n",
                i, (void *)seg_vaddr,
                (unsigned long)ph->p_memsz, (unsigned long)ph->p_filesz,
                (ph->p_flags & PF_R) ? 'R' : '-',
                (ph->p_flags & PF_W) ? 'W' : '-',
                (ph->p_flags & PF_X) ? 'X' : '-',
                user_mode ? " U" : "",
                (void *)virt_lo, (void *)virt_hi);

        if (!ensure_writable_pages(virt_lo, virt_hi, user_mode)) return false;

        if (ph->p_filesz) {
            memcpy((void *)seg_vaddr, bytes + ph->p_offset, ph->p_filesz);
        }
        if (ph->p_memsz > ph->p_filesz) {
            memset((void *)(seg_vaddr + ph->p_filesz), 0,
                   ph->p_memsz - ph->p_filesz);
        }
        loaded++;
    }

    if (loaded == 0) {
        kprintf("[elf] reject: no PT_LOAD segments\n");
        return false;
    }

    /* ---- pass 2: tighten permissions to what the segments asked for ---- */
    for (Elf64_Half i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t seg_vaddr = ph->p_vaddr + bias;
        uint64_t virt_lo = round_down(seg_vaddr,                 PAGE_SIZE);
        uint64_t virt_hi = round_up  (seg_vaddr + ph->p_memsz,   PAGE_SIZE);
        uint32_t flags   = phdr_to_vmm_flags(ph->p_flags, user_mode);

        if (!vmm_protect(virt_lo, virt_hi - virt_lo, flags)) {
            kprintf("[elf] WARN: vmm_protect [%p..%p) failed\n",
                    (void *)virt_lo, (void *)virt_hi);
        }
    }

    /* ---- compute output info ---- */
    info->entry = eh->e_entry + bias;
    info->phnum = eh->e_phnum;
    info->phent = eh->e_phentsize;
    if (have_pt_phdr) {
        info->phdr_va = pt_phdr_vaddr;
    } else if (have_first_load &&
               eh->e_phoff >= first_load_offset &&
               (uint64_t)eh->e_phoff +
                   (uint64_t)eh->e_phnum * eh->e_phentsize
                   <= first_load_offset + first_load_filesz) {
        /* PHDRs are contained inside the first PT_LOAD's file image. */
        info->phdr_va = first_load_vaddr +
                        (eh->e_phoff - first_load_offset);
    } else {
        info->phdr_va = 0;
    }

    kprintf("[elf] load OK%s: type=%s, %d segment(s), base=%p, entry=%p, "
            "phdr=%p (%u entries)%s\n",
            user_mode ? " (user)" : "",
            (eh->e_type == ET_DYN) ? "ET_DYN" : "ET_EXEC",
            loaded, (void *)load_base, (void *)info->entry,
            (void *)info->phdr_va, eh->e_phnum,
            info->has_interp ? ", PT_INTERP" : "");
    return true;
}

bool elf_load(const void *image, size_t size, elf_entry_t *out_entry) {
    struct elf_load_info info;
    if (!do_elf_load(image, size, false, 0, &info)) return false;
    *out_entry = (elf_entry_t)info.entry;
    return true;
}

bool elf_load_user(const void *image, size_t size, uint64_t *out_entry) {
    /* Backwards-compat: ET_EXEC only, no base. ET_DYN callers must
     * use elf_load_user_at directly with a chosen base. */
    if (size >= sizeof(Elf64_Ehdr)) {
        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
        if (eh->e_type == ET_DYN) {
            kprintf("[elf] elf_load_user(): refusing ET_DYN -- use "
                    "elf_load_user_at()\n");
            return false;
        }
    }
    struct elf_load_info info;
    if (!do_elf_load(image, size, true, 0, &info)) return false;
    *out_entry = info.entry;
    return true;
}

bool elf_load_user_at(const void *image, size_t size, uint64_t load_base,
                      struct elf_load_info *out) {
    return do_elf_load(image, size, true, load_base, out);
}

bool elf_peek_interp(const void *image, size_t size, char *out, size_t cap) {
    if (!image || !out || cap == 0) return false;
    out[0] = '\0';
    if (size < sizeof(Elf64_Ehdr)) return false;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3) return false;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return false;

    uint64_t ph_end = (uint64_t)eh->e_phoff +
                      (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
    if (ph_end > size) return false;

    const uint8_t *bytes = (const uint8_t *)image;
    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(bytes + eh->e_phoff);
    for (Elf64_Half i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_INTERP) continue;
        if (ph->p_filesz == 0) return false;
        if ((uint64_t)ph->p_offset + ph->p_filesz > size) return false;
        if (ph->p_filesz > cap) return false;

        const char *src = (const char *)(bytes + ph->p_offset);
        if (src[ph->p_filesz - 1] != '\0') return false;     /* must be NUL-terminated */
        memcpy(out, src, ph->p_filesz);
        return true;
    }
    return false;
}

bool elf_run(const void *image, size_t size, int *out_rc) {
    elf_entry_t entry;
    if (!elf_load(image, size, &entry)) return false;

    kprintf("[elf] calling entry at %p ...\n", (void *)entry);
    int rc = entry();
    kprintf("[elf] entry returned %d (0x%x)\n", rc, (unsigned)rc);
    if (out_rc) *out_rc = rc;
    return true;
}
