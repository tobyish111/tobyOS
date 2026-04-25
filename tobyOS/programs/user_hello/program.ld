/* user_hello/program.ld -- linker script for ring-3 programs.
 *
 * Anchors the program at 0x400000 -- the classic Linux x86_64 ELF base
 * address. Lives in the canonical lower half so VMM_USER pages can
 * back it without crossing into the kernel half.
 *
 * Sections are 4 KiB-aligned at every boundary so the loader can map
 * each PT_LOAD with its own permissions (.text RX, .rodata R, .data
 * RW NX) without one segment's pages spilling into another's.
 *
 * No -mcmodel=kernel here -- programs use the small code model
 * (default), which is fine because 0x400000 fits in 31 signed bits.
 */

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
