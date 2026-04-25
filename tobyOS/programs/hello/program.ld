/* program.ld -- linker script for kernel-mode programs.
 *
 * Anchors the program at 0xffffffffa0000000 -- well above the kernel
 * image (which ends near 0xffffffff8000c000) and inside the
 * -mcmodel=kernel codegen window so absolute addresses still fit in
 * sign-extended 32-bit immediates.
 *
 * Sections are 4 KiB-aligned at every boundary so the loader can map
 * each PT_LOAD with its own permissions (.text RX, .rodata R, .data RW
 * NX) without one segment's pages spilling into another's.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(program_main)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R + X */
    rodata PT_LOAD FLAGS(4);   /* R     */
    data   PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = 0xffffffffa0000000;

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
