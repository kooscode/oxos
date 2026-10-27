// Open BIOS image, bytes 0x000-0xFFF of each 256 KB bank: the MCPX header and the X-code table (spec/stock/01 §5).
//
// The X-codes are XboxDev/cromwell's boot_rom/Xcodes.h (GPL-2.0, https://github.com/XboxDev/cromwell), included
// from the copy the build fetches into third_party/cromwell. They set up the memory controller, chipset and SMBus
// devices, then end with Cromwell's "overflow trick": they write `mov eax, 0xFFFC1000; jmp eax` to RAM address 0
// and exit. The MCPX boot ROM then fails its check of the (absent) Microsoft 2BL, hides itself, and the CPU runs on
// from address 0 into our 2BL at bank offset 0x1000. The header values below are also Cromwell's
// (boot_rom/2bBootStartup.S); MCPXREVD5 selects the values for the 1.6 console's MCPX.
//
// X-code encoding (9 bytes each): opcode, operand 1, operand 2. Jump offsets are relative to the next X-code.

#define xcode_peek(a) .byte 0x02; .long a; .long 0;
#define xcode_poke(a, v) .byte 0x03; .long a; .long v;
#define xcode_pciout(a, v) .byte 0x04; .long a; .long v;
#define xcode_pciin_a(a) .byte 0x05; .long a; .long 0;
#define xcode_bittoggle(andmask, ormask) .byte 0x06; .long andmask; .long ormask;
#define xcode_ifgoto(v, n) .byte 0x08; .long v; .long (9 * ((n) - 1));
#define xcode_goto(n) .byte 0x09; .long 0; .long (9 * ((n) - 1));
#define xcode_outb(p, v) .byte 0x11; .long p; .long v;
#define xcode_inb(p) .byte 0x12; .long p; .long 0;
#define xcode_poke_a(a) .byte 0x07; .long 0x03; .long a;
#define xcode_pciout_a(a) .byte 0x07; .long 0x04; .long a;
#define xcode_outb_a(p) .byte 0x07; .long 0x11; .long p;
#define xcode_END(v) .byte 0xEE; .long v; .long 0;

#define SMBUS 0x0000c000
#define SMB_xcode_Write(reg, value) \
    xcode_outb(SMBUS + 8, reg);     \
    xcode_outb(SMBUS + 6, value);   \
    xcode_outb(SMBUS + 2, 0x0000000a); \
    xcode_inb(SMBUS);               \
    xcode_ifgoto(0x00000010, -1);   \
    xcode_outb(SMBUS, 0x00000010);

    .section .xcodes, "a"
    .org 0x00
    // MCPX magic values: clock timings
    .long 0xff000009
    .long 0xff000008
    .long 0x2b16d065
    .long 0x3346322d
    .long 0x01010101
    .long 0x08080808
    .long 0x00000801
#ifndef MCPXREVD5
    .long 0xc8fc7c8a
    .long 0x44290213
    .long 0x90004998
    .long 0x00000000
#else
    .long 0xc8b4588a
    .long 0x00100000
    .long 0x00050aa7
    .long 0xf0000000
#endif
    .long 0xffffffff
    .long 0xffffffff
    .org 0x6c
    .long 0x00000107
    .org 0x70 // MCPX configuration area
    .long 0x0000000f
    .long 0x40004400
#ifndef MCPXREVD5
    .long 0x12d10070
    .long 0x00000c90
#else
    .long 0x16ce0090
    .long 0x00000dc0
#endif
    .org 0x80
#include "Xcodes.h"
    // The NV2A memory pad calibration table, where header 0x7C points (the 2BL reads it there, spec/stock/21 §3.7a).
#ifndef MCPXREVD5
    .org 0xc90
#else
    .org 0xdc0
#endif
#include "padcal_table.inc"
    .org 0x1000
