/* SPDX-License-Identifier: GPL-2.0-or-later */

ENTRY(_start)

PHDRS
{
    firmware PT_LOAD FLAGS(5);
}

SECTIONS
{
    . = 0x8001000000000000;
    firmware_start = .;

    .text : ALIGN(4) {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } :firmware

    .rodata : ALIGN(8) {
        KEEP(*(.rodata.version))
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } :firmware

    .data : ALIGN(8) {
        *(.data .data.*)
    } :firmware

    .bss (NOLOAD) : ALIGN(8) {
        *(.bss .bss.*)
        *(COMMON)
    } :firmware

    firmware_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.GNU-stack)
        *(.eh_frame)
    }

    ASSERT(_start == 0x8001000000000000,
           "MMIX virt firmware reset entry moved")
    ASSERT(firmware_end - firmware_start <= 0x04000000,
           "MMIX virt firmware exceeds flash bank zero")
}
