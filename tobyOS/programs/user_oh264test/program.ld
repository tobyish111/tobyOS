/* Linker script for C++ tobyOS programs (stage 13 C++ runtime).
 * Identical to the shared programs/program.ld PLUS the
 * .init_array/.fini_array output sections whose bounds __libtoby_init
 * walks to run static constructors/destructors. KEEP() protects the
 * entries from --gc-sections-style elimination. */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R + X */
    rodata PT_LOAD FLAGS(4);   /* R     */
    data   PT_LOAD FLAGS(6);   /* R + W */
    tls    PT_TLS FLAGS(4);    /* R (thread-local) */
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

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(.ctors))
        __init_array_end = .;
    } :data

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        KEEP(*(.dtors))
        __fini_array_end = .;
    } :data

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    .tdata : {
        *(.tdata .tdata.*)
    } :tls

    .tbss : {
        *(.tbss .tbss.*)
    } :tls

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
