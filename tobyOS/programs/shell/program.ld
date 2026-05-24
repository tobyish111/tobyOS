/* shell/program.ld -- linker script for the desktop shell.
 *
 * Based on c_hello/program.ld with an added PT_TLS segment for
 * thread-local storage support.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R + X */
    rodata PT_LOAD FLAGS(4);   /* R     */
    data   PT_LOAD FLAGS(6);   /* R + W */
    tls    PT_TLS  FLAGS(4);   /* R     */
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

    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls

    .tbss : {
        *(.tbss .tbss.*)
    } :data :tls

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
