// Open BIOS image, bytes 0x000-0xFFF of each 256 KB bank: the MCPX header and the X-code table (spec/stock/01 §5).
//
// The X-codes are XboxDev/cromwell's boot_rom/Xcodes.h (GPL-2.0, https://github.com/XboxDev/cromwell), included
// from the copy the build fetches into third_party/cromwell. They set up the memory controller, chipset and SMBus
// devices, then end with Cromwell's "overflow trick": they write `mov eax, 0xFFFC1000; jmp eax` to RAM address 0
// and exit. On a retail console the MCPX boot ROM then fails its check of the (absent) Microsoft 2BL, hides itself,
// and the CPU runs on from address 0 into our 2BL at bank offset 0x1000. Without a boot ROM (MCPX X2, xemu) our
// reset code (reset.S) runs this table and jumps to the 2BL itself. The header values below are also Cromwell's
// (boot_rom/2bBootStartup.S); MCPXREVD5 selects the values for the 1.6 console's MCPX and OXOS's own 1.6 program,
// boot/xcodes_1_6.inc (spec/stock/24).
//
// X-code encoding (9 bytes each): opcode, operand 1, operand 2. Jump offsets are relative to the next X-code. The
// macros are Cromwell's (boot_rom/2bBootStartup.S). What each op does: spec/stock/24 §1.3 (MCPX ROM) and
// spec/oxos_internal/27 §5.3 (our reset code).

// 0x02: accumulator = 32-bit read of a.
#define xcode_peek(a) .byte 0x02; .long a; .long 0;
// 0x03: 32-bit write of v to a.
#define xcode_poke(a, v) .byte 0x03; .long a; .long v;
// 0x04: PCI configuration write: address a, value v.
#define xcode_pciout(a, v) .byte 0x04; .long a; .long v;
// 0x05: accumulator = PCI configuration read of address a.
#define xcode_pciin_a(a) .byte 0x05; .long a; .long 0;
// 0x06: accumulator = (accumulator AND andmask) OR ormask.
#define xcode_bittoggle(andmask, ormask) .byte 0x06; .long andmask; .long ormask;
// 0x08: if accumulator != v, go to the X-code n away from this one (n = -1: the one before).
#define xcode_ifgoto(v, n) .byte 0x08; .long v; .long (9 * ((n) - 1));
// 0x09: go to the X-code n away from this one.
#define xcode_goto(n) .byte 0x09; .long 0; .long (9 * ((n) - 1));
// 0x11: byte v to I/O port p.
#define xcode_outb(p, v) .byte 0x11; .long p; .long v;
// 0x12: accumulator = byte read from I/O port p.
#define xcode_inb(p) .byte 0x12; .long p; .long 0;
// 0x07 with 0x03: write the accumulator to a.
#define xcode_poke_a(a) .byte 0x07; .long 0x03; .long a;
// 0x07 with 0x04: PCI configuration write of the accumulator to address a.
#define xcode_pciout_a(a) .byte 0x07; .long 0x04; .long a;
// 0x07 with 0x11: the accumulator's low byte to I/O port p.
#define xcode_outb_a(p) .byte 0x07; .long 0x11; .long p;
// 0xEE: end of the program; v = the MTRR default type the ROM sets at exit (0x806, unused by reset.S).
#define xcode_END(v) .byte 0xEE; .long v; .long 0;

// The SMBus host controller's I/O base, and a byte write to an SMBus device: register (base + 8) and value (+ 6),
// start a byte-data cycle (+ 2 = 0x0A), wait until the status (+ 0) reads exactly 0x10 (done), then clear it. The
// device address (+ 4) is set by the program before.
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
    // 0x00-0x2F, "MCPX magic values: clock timings" (Cromwell). No known CPU code reads them; at power-on the Modxo
    // sees 0x00-0x2B read before the CPU's first fetch (2026-11-10). Their meaning is not determined (spec/stock/21
    // §4).
    // MCPXREVD5: the 1.6 console's values.
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
    // 0x6C-0x7F (spec/stock/21 §4). 0x70-0x77 are also read at power-on before the CPU's first fetch (2026-11-10);
    // 0x6C, 0x70, 0x74 and 0x78's upper bytes have no known CPU reader.
    .org 0x6c
    .long 0x00000107
    .org 0x70 // MCPX configuration area
    .long 0x0000000f
    .long 0x40004400
    // 0x78: its low byte is the flash byte the kernel reads for its encoder flags (0x90 = 1.6, spec/stock/24 §6.1).
    // 0x7C: the offset of the pad calibration table below, read by the 2BL.
#ifndef MCPXREVD5
    .long 0x12d10070
    .long 0x00000c90
#else
    .long 0x16ce0090
    .long 0x00000ecb
#endif
    // 0x80: the X-code program, run by the MCPX ROM or our reset code (reset.S): Cromwell's for 1.0-1.5, ours for
    // the 1.6 (spec/oxos_internal/28). tools/harden_xcodes.py adds write checks after assembly (spec/oxos_internal/27).
    .org 0x80
#ifndef MCPXREVD5
#include "Xcodes.h"
#else
#include "xcodes_1_6.inc"
#endif
    // The NV2A memory pad calibration table, where header 0x7C points (the 2BL reads it there, spec/stock/21 §3.7a).
    // On the 1.6 it follows a longer program (with its write checks), so it fills the end of the bank: 0xECB-0xFFF.
#ifndef MCPXREVD5
    .org 0xc90
#else
    .org 0xecb
#endif
#include "padcal_table.inc"
    .org 0x1000
