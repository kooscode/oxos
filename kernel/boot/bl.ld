/* Open BIOS image 2BL: the entry runs from flash at 0xFFFC1000, the C part is copied to RAM at 0x400000. */
ENTRY(_start)
SECTIONS {
    .entry 0xFFFC1000 : { *(.entry) }
    .ram 0x00400000 : AT(ALIGN(LOADADDR(.entry) + SIZEOF(.entry), 16)) {
        __ram_start = .;
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
        __ram_end = .;
    }
    __ram_load = LOADADDR(.ram);
    __ram_copy_dwords = (__ram_end - __ram_start) / 4;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    __bss_dwords = (__bss_end - __bss_start) / 4;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
