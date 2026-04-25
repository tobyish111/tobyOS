/* linker.ld -- 64-bit higher-half kernel for Limine v11
 *
 * Limine v11 requires every PT_LOAD segment to be 4 KiB-aligned in both
 * its virtual address AND its file offset. We achieve that with explicit
 * PHDRS and ALIGN(CONSTANT(MAXPAGESIZE)) between every section.
 *
 * The __*_start / __*_end symbols are exposed so vmm.c can map each
 * section with its own permissions (RX for text, R for rodata, RW for
 * data/bss). They live at the section boundaries (already 4 KiB aligned
 * thanks to the ALIGN above each output section), so vmm can iterate
 * page-by-page without worrying about partial pages straddling sections.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    limine_reqs PT_LOAD;
    text        PT_LOAD;
    rodata      PT_LOAD;
    data        PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_start = .;

    .limine_reqs : {
        KEEP(*(.limine_reqs))
    } :limine_reqs

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    __text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_end = .;

    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
        /* AP startup trampoline blob -- copied at runtime to a fixed
         * sub-1 MiB physical page (0x8000). Lives in .rodata so the
         * kernel image side is non-executable; we read its bytes only. */
        . = ALIGN(16);
        *(.ap_trampoline)
    } :rodata
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_end = .;

    __data_start = .;
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_end = .;

    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
