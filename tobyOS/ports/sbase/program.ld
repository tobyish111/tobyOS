/* ports/sbase/program.ld -- shared linker script for the M25E
 * sbase-style ports (echo, cat, wc, head, ls, grep).
 *
 * Identical layout to programs/c_hello/program.ld -- anchored at
 * 0x400000 with R/X text, R-only rodata, R/W data+bss; we keep one
 * script for all ports because they have the same shape (single .c
 * + util.c + libtoby.a). */

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
