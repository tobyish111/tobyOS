/* vmm.h -- kernel virtual memory manager (x86_64 4-level paging).
 *
 * vmm_init() builds a fresh PML4 that:
 *   - mirrors the HHDM (phys 0..highest_phys mapped at hhdm_offset+phys,
 *     2 MiB pages, RW NX);
 *   - maps the kernel image at its higher-half virtual base
 *     (4 KiB pages, RW, currently exec-allowed -- per-section permissions
 *     come in milestone 3B);
 * then loads CR3.
 *
 * After vmm_init, callers can mint and revoke kernel mappings with
 * vmm_map / vmm_unmap. All addresses are kernel-only -- userspace lives
 * in milestone 4.
 */

#ifndef TOBYOS_VMM_H
#define TOBYOS_VMM_H

#include <tobyos/types.h>

/* ---- public flag bits (independent of the architectural PTE layout) ---- */

#define VMM_PRESENT  (1u << 0)
#define VMM_WRITE    (1u << 1)
#define VMM_USER     (1u << 2)   /* unused this milestone, but plumbed */
#define VMM_NOCACHE  (1u << 3)   /* PCD|PWT, for MMIO */
#define VMM_NX       (1u << 4)   /* no-execute (requires EFER.NXE, set in vmm_init) */
#define VMM_HUGE_2M  (1u << 5)   /* request a 2 MiB leaf (virt+phys must be 2M-aligned) */

struct limine_memmap_response;

/* Build the kernel PML4, mirror the HHDM + kernel image, and switch CR3.
 * Must run after pmm_init. Panics on any allocation failure.
 *
 * `memmap` is used to decide which physical ranges to mirror into HHDM.
 * We map every entry the kernel might dereference (usable RAM, ACPI,
 * bootloader-reclaimable, the kernel image, and the framebuffer) and
 * skip BAD_MEMORY / RESERVED -- the latter often includes sparse 64-bit
 * holes (e.g. 12 GiB at 0xfd00000000) that would waste PD pages. */
void vmm_init(struct limine_memmap_response *memmap);

/* Convenience wrapper that runs vmm_init and exercises the API. Mirrors
 * pmm_init_and_test / heap_init_and_test in style. */
void vmm_init_and_test(struct limine_memmap_response *memmap);

/* Physical address of the kernel PML4. Useful later when we want to load
 * the same root into a new process's CR3 (kernel half is shared). */
uint64_t vmm_kernel_pml4_phys(void);

/* Map [virt..virt+bytes) -> [phys..phys+bytes). All three must be page-
 * aligned (4 KiB normally, 2 MiB if VMM_HUGE_2M is set). Returns false
 * on misaligned input or PMM OOM while growing intermediate tables. */
bool vmm_map(uint64_t virt, uint64_t phys, size_t bytes, uint32_t flags);

/* Unmap [virt..virt+bytes). Tolerates already-unmapped pages (logs a
 * warning but keeps going). Issues invlpg per page. */
bool vmm_unmap(uint64_t virt, size_t bytes);

/* Change permissions on an already-mapped 4 KiB range. Walks every PT
 * leaf and rewrites only the flag bits (PRESENT, RW, US, NX, PCD/PWT),
 * preserving the physical frame field. Refuses to "shatter" a 2 MiB
 * leaf -- if the range covers one, we kpanic rather than silently
 * splitting it (that would be a bug in the caller, not a real use). */
bool vmm_protect(uint64_t virt, size_t bytes, uint32_t flags);

/* Return the physical address `virt` resolves to in the kernel PML4, or
 * 0 if unmapped. Includes the in-page offset (i.e. matches the value
 * the CPU would compute on a load). */
uint64_t vmm_translate(uint64_t virt);

/* Walk the kernel PML4 for `virt` and print every level: indices, raw
 * entry, present/writable/huge bits, and the resolved physical address.
 * Cheap diagnostic for the `page` shell command. */
void vmm_dump(uint64_t virt);

/* ---- per-process address spaces (milestone 5) ----
 *
 * Process model: every user process gets its own PML4. The kernel half
 * (entries 256..511) is shared by reference -- we copy the entries
 * themselves into the new PML4, but they keep pointing at the same
 * PDPT pages, so any later kernel-half mapping (heap growth, etc.)
 * is automatically visible in every process. The user half (0..255)
 * starts empty and is populated by elf_load_user + the user-stack
 * setup before the process first runs.
 *
 * vmm_create_user_pml4 returns the physical address of the new PML4
 * (suitable for CR3) or 0 on PMM OOM.
 *
 * vmm_destroy_user_pml4 walks entries 0..255 of the given PML4 and
 * frees every leaf data page + every intermediate PDPT/PD/PT page
 * underneath, then frees the PML4 frame itself. Entries 256..511 are
 * left strictly alone (those tables are shared with the kernel and
 * other processes). The CR3 must NOT be the active CR3 when this is
 * called -- callers should switch to the kernel PML4 first.
 *
 * vmm_set_active_root retargets the editor pointer AND writes CR3 so
 * the two stay in lock-step. This is what process bring-up wants:
 * elf_load_user copies bytes into newly-mapped user-half VAs via the
 * CPU's normal memory operands, so CR3 must point at the same PML4
 * the editor is mutating. Returns the previous editor root. Pass 0 to
 * mean "the kernel PML4".
 *
 * vmm_set_editor_root is the kernel-stays-on-its-CR3 variant: it
 * retargets ONLY g_pml4 and does NOT touch CR3. Use this from a
 * syscall handler that wants to install new user-half mappings into
 * the *current* user process's PML4 -- CR3 is already that PML4
 * (`syscall` doesn't switch it), so writing CR3 again would just
 * stomp it back to the kernel root once the caller restores. */
uint64_t vmm_create_user_pml4(void);
void     vmm_destroy_user_pml4(uint64_t pml4_phys);
uint64_t vmm_set_active_root(uint64_t pml4_phys);
uint64_t vmm_set_editor_root(uint64_t pml4_phys);

#endif /* TOBYOS_VMM_H */
