/* user_gui_helloapp/program.ld -- linker script for /data/apps/helloapp.elf.
 *
 * Identical layout to the other milestone-12+ GUI apps: single low-half
 * PT_LOAD segment at 0x400000, no PIE, no relocs. The package manager
 * drops the built ELF at /data/apps/helloapp.elf and registers it with
 * the desktop launcher via a .app descriptor. */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
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
