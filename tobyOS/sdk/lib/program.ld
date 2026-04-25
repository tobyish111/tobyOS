/* sdk/lib/program.ld -- canonical linker script for tobyOS SDK apps.
 *
 * Every static SDK-built ELF anchors at 0x400000, with a three-segment
 * layout (text RX, rodata R, data RW). ENTRY resolves to `_start` in
 * libtoby/crt0.o, which calls __libtoby_init then main, then SYS_EXIT
 * with main's return value.
 *
 * Why these specific choices:
 *   - 0x400000 base   : matches every in-tree user program; the kernel's
 *                       ELF loader has no opinion on the address but
 *                       this leaves room below for low-mapped data and
 *                       above for stack/heap growth.
 *   - 4 KiB pages     : matches -z max-page-size=0x1000 in PROG_LDFLAGS
 *                       and the kernel's PAGE_SIZE; without this lld
 *                       defaults to 2 MiB and inflates every binary.
 *   - DISCARD .eh_frame : we build with -fno-unwind-tables so the
 *                       sections are spurious; dropping them shrinks
 *                       /repo/*.tpkg payloads measurably.
 *   - DISCARD .note.*  : freestanding builds don't need build-id /
 *                       gnu-property; the kernel never reads them. */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R + X */
    rodata PT_LOAD FLAGS(4);   /* R     */
    data   PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(0x1000);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
