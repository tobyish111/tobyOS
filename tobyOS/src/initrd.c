/* initrd.c -- find the boot tar module and mount it as the root FS.
 *
 * The Limine module list lives at module_req.response (see kernel.c
 * which owns the request struct). We pick the module whose basename
 * matches "initrd.tar" -- this lets the build choose any prefix path
 * (e.g. "/initrd.tar" or "boot():/initrd.tar") without breaking us.
 *
 * Limine modules sit in BOOTLOADER_RECLAIMABLE memory which vmm_init
 * mirrors via HHDM, so the address Limine gives us is dereferenceable
 * for the lifetime of the kernel. We pass that pointer straight into
 * ramfs_mount(); ramfs holds the pointer (no copy) and serves all
 * subsequent reads out of it.
 */

#include <tobyos/initrd.h>
#include <tobyos/ramfs.h>
#include <tobyos/limine.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>

extern volatile struct limine_module_request module_req;

#define INITRD_NAME "initrd.tar"

static const char *basename_of(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

bool initrd_init(void) {
    if (!module_req.response || module_req.response->module_count == 0) {
        kprintf("[initrd] no Limine modules -- VFS will be empty\n");
        return false;
    }

    struct limine_file *found = 0;
    for (uint64_t i = 0; i < module_req.response->module_count; i++) {
        struct limine_file *m = module_req.response->modules[i];
        if (strcmp(basename_of(m->path), INITRD_NAME) == 0) {
            found = m;
            break;
        }
    }

    if (!found) {
        kprintf("[initrd] no module named '%s' -- VFS will be empty\n",
                INITRD_NAME);
        return false;
    }

    kprintf("[initrd] using '%s' (size=%lu, addr=%p)\n",
            found->path, (unsigned long)found->size, found->address);

    uint64_t hhdm = pmm_hhdm_offset();
    uint64_t virt = (uint64_t)(uintptr_t)found->address;
    uint64_t phys = (virt >= hhdm) ? (virt - hhdm) : virt;
    if (!vmm_hhdm_ensure_mapped(phys, (size_t)found->size,
                                VMM_PRESENT | VMM_WRITE | VMM_NX)) {
        kprintf("[initrd] failed to map module at phys %p\n", (void *)phys);
        return false;
    }
    if (virt != hhdm + phys)
        found->address = (void *)(uintptr_t)(hhdm + phys);

    int rc = ramfs_mount(found->address, found->size);
    if (rc != VFS_OK) {
        kprintf("[initrd] ramfs_mount failed: %d\n", rc);
        return false;
    }
    return true;
}
