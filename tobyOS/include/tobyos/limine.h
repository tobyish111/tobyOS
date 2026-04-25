/* limine.h -- shared Limine boot-protocol struct definitions.
 *
 * Just the bits used by more than one TU: memory-map types, HHDM
 * request, and memmap entry layout. The framebuffer request still
 * lives inline in kernel.c since only the boot path touches it.
 *
 * Reference: Limine Boot Protocol v3 (https://github.com/limine-bootloader/limine/blob/v11.x/PROTOCOL.md)
 */

#ifndef TOBYOS_LIMINE_H
#define TOBYOS_LIMINE_H

#include <tobyos/types.h>

#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b

/* ---- Memory-map ---- */

#define LIMINE_MEMMAP_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62 }

/* Memory-map region types -- match Limine v3. */
#define LIMINE_MEMMAP_USABLE                  0
#define LIMINE_MEMMAP_RESERVED                1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE        2
#define LIMINE_MEMMAP_ACPI_NVS                3
#define LIMINE_MEMMAP_BAD_MEMORY              4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE  5
#define LIMINE_MEMMAP_EXECUTABLE_AND_MODULES  6
#define LIMINE_MEMMAP_FRAMEBUFFER             7

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_memmap_entry **entries;
};

struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_memmap_response *response;
};

/* ---- HHDM (higher-half direct map) ---- */

#define LIMINE_HHDM_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852, 0x63984e959a98244b }

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_hhdm_response *response;
};

/* ---- Executable address (a.k.a. kernel address) ----
 *
 * The linker tells us where the kernel sits in *virtual* space (see
 * __kernel_start in linker.ld). The bootloader chooses where to load it
 * in *physical* space, and tells us via this response. vmm_init needs
 * both halves to build a virt -> phys mapping for the kernel image. */

#define LIMINE_EXECUTABLE_ADDRESS_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x71ba76863cc55f63, 0xb2644a48c516a487 }

struct limine_executable_address_response {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};

struct limine_executable_address_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_executable_address_response *response;
};

/* ---- Modules (extra files Limine loads alongside the kernel) ----
 *
 * Each module appears as a `limine_file` whose `address` points into
 * BOOTLOADER_RECLAIMABLE memory (and is therefore covered by our HHDM
 * mirror, so the pointer is dereferenceable after the CR3 switch in
 * vmm_init). We only use `address`, `size`, `path`, `cmdline`; the
 * remaining fields describe the source media and are left opaque. */

#define LIMINE_MODULE_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x3e7e279702be32af, 0xca1c4f3bd1280cee }

struct limine_uuid {
    uint32_t a;
    uint16_t b;
    uint16_t c;
    uint8_t  d[8];
};

struct limine_file {
    uint64_t revision;
    void    *address;
    uint64_t size;
    char    *path;
    char    *cmdline;
    uint32_t media_type;
    uint32_t unused;
    uint32_t tftp_ip;
    uint32_t tftp_port;
    uint32_t partition_index;
    uint32_t mbr_disk_id;
    struct limine_uuid gpt_disk_uuid;
    struct limine_uuid gpt_part_uuid;
    struct limine_uuid part_uuid;
};

struct limine_module_response {
    uint64_t revision;
    uint64_t module_count;
    struct limine_file **modules;
};

struct limine_module_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_module_response *response;
};

/* ---- RSDP (ACPI Root System Description Pointer) ----
 *
 * Limine hands us the firmware-provided RSDP as a *higher-half virtual*
 * pointer (already inside the HHDM mirror, so directly dereferenceable
 * after vmm_init). From the RSDP we walk to XSDT -> MADT in src/acpi.c
 * to find the local APICs. We never need to pull it out of low memory
 * ourselves -- this request hides whether the RSDP came from EBDA, BIOS
 * F-segment, or UEFI. */

#define LIMINE_RSDP_REQUEST \
    { LIMINE_COMMON_MAGIC, 0xc5e77b6b397e7b43, 0x27637845accdcf3c }

struct limine_rsdp_response {
    uint64_t revision;
    void    *address;   /* virt pointer to RSDP (HHDM-style) */
};

struct limine_rsdp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_rsdp_response *response;
};

#endif /* TOBYOS_LIMINE_H */
