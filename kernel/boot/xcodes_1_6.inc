// X-code program for the 1.6 console (MCPXREVD5 builds), included by boot/xcodes.S at bank offset 0x80.
//
// Written from spec/stock/24_1_6_bringup.md (the 1.6 console's memory and chipset set-up) on the layout of
// XboxDev/cromwell's boot_rom/Xcodes.h (GPL-2.0), which the 1.0-1.5 images use. The 1.6 differs from Cromwell's
// table in (spec/stock/24 §7):
// - the SMBus is enabled first: the memory interface takes four of its settings from EEPROM bytes 0xC0-0xC3;
// - the memory interface, pad defaults and DRAM controller values of the 1.6;
// - the SMC version check; no writes to a video encoder.
// The memory test and size decision are not here: boot/start.S makes them (the same rule, spec/stock/24 §4). For
// that the program leaves the test set-up: PFB 0x100200 0x03070103, 0x100204 0x11448000, top of memory 128 MB.
//
// Register numbers are written as the program uses them: the NV2A at 0x0F000000 until the end, PCI configuration
// addresses 0x80000000 | bus << 16 | device << 11 | function << 8 | register.

// Jumps to a label: the offset counts from the next X-code (`.` is 4 bytes before it while operand 2 is assembled).
#define xcode_ifgoto_to(v, label) .byte 0x08; .long v; .long (label) - (. + 4);
#define xcode_goto_to(label) .byte 0x09; .long 0; .long (label) - (. + 4);

#define NV(offset) (0x0F000000 + (offset))
#define STRAP NV(0x101000)

// One byte read of the EEPROM (SMBus slave 0x54, address byte 0xA9) at offset `off`: the byte ends in the
// accumulator. Done (status exactly 0x10): clear it and read the data. An error bit (collision 0x02, error 0x04,
// time-out 0x20): clear it and start again. Neither: still busy, read the status again. Like the reference, it waits
// for ever: a 1.6 cannot set up its memory without these bytes.
#define EEPROM_READ(off)                 \
1:  xcode_outb(SMBUS + 4, 0x000000A9)    \
    xcode_outb(SMBUS + 8, off)           \
    xcode_outb(SMBUS + 2, 0x0000000A)    \
2:  xcode_inb(SMBUS)                     \
    xcode_ifgoto_to(0x00000010, 3f)      \
    xcode_outb(SMBUS, 0x00000010)        \
    xcode_inb(SMBUS + 6)                 \
    xcode_goto_to(4f)                    \
3:  xcode_bittoggle(0x00000026, 0)       \
    xcode_ifgoto_to(0x00000000, 5f)      \
    xcode_goto_to(2b)                    \
5:  xcode_outb_a(SMBUS)                  \
    xcode_goto_to(1b)                    \
4:

// Bit k of the byte in the low byte of `reg`: if set, OR `pattern` into `reg`. Eight of these copy the byte into the
// bytes `pattern` names (the X-codes have no shift).
#define COPY_BIT(reg, k, pattern)                 \
    xcode_peek(reg)                               \
    xcode_bittoggle(1 << (k), 0)                  \
    xcode_ifgoto_to(1 << (k), 6f)                 \
    xcode_peek(reg)                               \
    xcode_bittoggle(0xFFFFFFFF, (pattern) << (k)) \
    xcode_poke_a(reg)                             \
6:

// The EEPROM byte in the accumulator, copied to every byte of `reg` (stored first, then bits 0-7 copied up).
#define BYTE_TO_ALL(reg)              \
    xcode_poke_a(reg)                 \
    COPY_BIT(reg, 0, 0x01010100)      \
    COPY_BIT(reg, 1, 0x01010100)      \
    COPY_BIT(reg, 2, 0x01010100)      \
    COPY_BIT(reg, 3, 0x01010100)      \
    COPY_BIT(reg, 4, 0x01010100)      \
    COPY_BIT(reg, 5, 0x01010100)      \
    COPY_BIT(reg, 6, 0x01010100)      \
    COPY_BIT(reg, 7, 0x01010100)

    // Bridge, chipset and bus set-up (spec/stock/24 §2.2): power-management I/O at 0x8000, AGP bridge windows, the
    // NV2A's registers at 0x0F000000, then the SMBus (I/O 0xC000).
    xcode_pciout(0x80000884, 0x00008001)
    xcode_pciout(0x80000804, 0x00000003)
    xcode_outb(0x00008049, 0x00000008)
    xcode_outb(0x000080D9, 0x00000000)
    xcode_outb(0x00008026, 0x00000001)
    xcode_pciout(0x8000F04C, 0x00000001)
    xcode_pciout(0x8000F018, 0x00010100)
    xcode_pciout(0x80000084, 0x07FFFFFF)
    xcode_pciout(0x8000F020, 0x0FF00F00)
    xcode_pciout(0x8000F024, 0xF7F0F000)
    xcode_pciout(0x80010010, 0x0F000000)
    xcode_pciout(0x80010014, 0xF0000000)
    xcode_pciout(0x80010004, 0x00000007)
    xcode_pciout(0x8000F004, 0x00000007)
    xcode_pciout(0x80000904, 0x00000001)
    xcode_pciout(0x80000914, 0x0000C001)
    xcode_pciout(0x80000918, 0x0000C201)
    xcode_outb(0x0000C200, 0x00000070)

    // Memory interface, first two settings (§2.3).
    xcode_poke(NV(0x10B0), 0x01000010)
    xcode_poke(NV(0x10CC), 0x66660000)

    // EEPROM 0xC3 → strap bits 18/19 (§3.3): 0x42 both, 0x40 bit 19, 0x02 bit 18, anything else neither. The strap's
    // bits 18, 19 and 25-28 are cleared and bit 31 set.
    EEPROM_READ(0x000000C3)
    xcode_ifgoto_to(0x00000042, 1f)
    xcode_peek(STRAP)
    xcode_bittoggle(0xE1F3FFFF, 0x800C0000)
    xcode_goto_to(9f)
1:  xcode_ifgoto_to(0x00000040, 2f)
    xcode_peek(STRAP)
    xcode_bittoggle(0xE1F3FFFF, 0x80080000)
    xcode_goto_to(9f)
2:  xcode_ifgoto_to(0x00000002, 3f)
    xcode_peek(STRAP)
    xcode_bittoggle(0xE1F3FFFF, 0x80040000)
    xcode_goto_to(9f)
3:  xcode_peek(STRAP)
    xcode_bittoggle(0xE1F3FFFF, 0x80000000)
9:  xcode_poke_a(STRAP)

    // EEPROM 0xC2 → 0x10B8 bytes 2 and 3 (§3.3), assembled in pad register 0x1248 (set to its default below).
    EEPROM_READ(0x000000C2)
    BYTE_TO_ALL(NV(0x1248))
    xcode_peek(NV(0x1248))
    xcode_bittoggle(0xFFFF0000, 0)
    xcode_poke_a(NV(0x10B8))

    // The rest of the memory interface (§2.3), then the DRAM controller (§2.4).
    xcode_poke(NV(0x10D4), 0x0000000F)
    xcode_poke(NV(0x10B4), 0x00000000)
    xcode_poke(NV(0x10BC), 0x00005860)
    xcode_poke(NV(0x10C4), 0xAAAA0000)
    xcode_poke(NV(0x10C8), 0x00007D67)
    xcode_poke(NV(0x10D8), 0x00000000)
    xcode_poke(NV(0x10DC), 0x00000000)
    xcode_poke(NV(0x10E8), 0x04000000)
    xcode_poke(NV(0x100200), 0x03070103)
    xcode_poke(NV(0x100410), 0x11000016)
    xcode_poke(NV(0x100330), 0x84848888)
    xcode_poke(NV(0x10032C), 0xFFFFCFFF)
    xcode_poke(NV(0x100328), 0x00000001)
    xcode_poke(NV(0x100338), 0x000000DF)

    // SMC version check (§2.5): SMC register 1 ← 0 restarts its version string; if the first character is not 'P',
    // host function 3 register 0x6C ← 0x01000000. Either way the program goes on.
    xcode_outb(SMBUS + 4, 0x00000020)
    SMB_xcode_Write(0x01, 0x00)
    xcode_outb(SMBUS + 4, 0x00000021)
    xcode_outb(SMBUS + 8, 0x00000001)
    xcode_outb(SMBUS + 2, 0x0000000A)
    xcode_inb(SMBUS)
    xcode_ifgoto(0x00000010, -1)
    xcode_outb(SMBUS, 0x00000010)
    xcode_inb(SMBUS + 6)
    xcode_ifgoto_to(0x00000050, 1f)
    xcode_goto_to(2f)
1:  xcode_pciout(0x8000036C, 0x01000000)
2:

    // GPU clock and PBUS defaults (§2.6).
    xcode_poke(NV(0x680500), 0x00011C01)
    xcode_poke(NV(0x68050C), 0x000A0400)
    xcode_poke(NV(0x1220), 0x00000000)
    xcode_poke(NV(0x1228), 0x00000000)
    xcode_poke(NV(0x1264), 0x00000000)
    xcode_poke(NV(0x1210), 0x00000000)

    // EEPROM 0xC1 → pad register 0x122C, EEPROM 0xC0 → 0x1214, the byte in all four bytes (§3.3), each assembled in
    // 0x1248 first so that the two registers only ever hold their final value.
    EEPROM_READ(0x000000C1)
    BYTE_TO_ALL(NV(0x1248))
    xcode_peek(NV(0x1248))
    xcode_poke_a(NV(0x122C))
    EEPROM_READ(0x000000C0)
    BYTE_TO_ALL(NV(0x1248))
    xcode_peek(NV(0x1248))
    xcode_poke_a(NV(0x1214))

    // The other pad registers' defaults (§2.6); the 2BL calibrates nine of them (spec/stock/21 §3.7).
    xcode_poke(NV(0x1230), 0xFFFFFFFF)
    xcode_poke(NV(0x1234), 0xAAAAAAAA)
    xcode_poke(NV(0x1238), 0xAAAAAAAA)
    xcode_poke(NV(0x123C), 0x8B8B8B8B)
    xcode_poke(NV(0x1240), 0xFFFFFFFF)
    xcode_poke(NV(0x1244), 0x8B8B8B8B)
    xcode_poke(NV(0x1248), 0x8B8B8B8B)

    // DRAM initialisation and refresh (§2.7).
    xcode_poke(NV(0x1002D4), 0x00000001)
    xcode_poke(NV(0x1002C4), 0x00100000)
    xcode_poke(NV(0x1002CC), 0x00100000)
    xcode_poke(NV(0x1002C0), 0x00000011)
    xcode_poke(NV(0x1002C8), 0x00000011)
    xcode_poke(NV(0x1002C0), 0x00000032)
    xcode_poke(NV(0x1002C8), 0x00000032)
    xcode_poke(NV(0x1002C0), 0x00000132)
    xcode_poke(NV(0x1002C8), 0x00000132)
    xcode_poke(NV(0x1002D0), 0x00000001)
    xcode_poke(NV(0x1002D0), 0x00000001)
    xcode_poke(NV(0x100210), 0x80000000)
    xcode_poke(NV(0x124C), 0xAA8BAA8B)
    xcode_poke(NV(0x1250), 0x00005474)
    xcode_poke(NV(0x100228), 0x081205FF)
    xcode_poke(NV(0x1218), 0x00000000)

    // Chipset (§2.8).
    xcode_pciin_a(0x80000860)
    xcode_bittoggle(0xFFFFFFFF, 0x00000400)
    xcode_pciout_a(0x80000860)
    xcode_pciout(0x8000084C, 0x0000FDDE)
    xcode_pciout(0x8000089C, 0x871CC707)
    xcode_pciin_a(0x800008B4)
    xcode_bittoggle(0xFFFFF0FF, 0x00000F00)
    xcode_pciout_a(0x800008B4)
    xcode_pciout(0x80000340, 0xF0F0C0C0)
    xcode_pciout(0x80000344, 0x00000000)
    xcode_pciout(0x8000035C, 0x00000000)
    xcode_pciout(0x8000036C, 0x00230801)
    xcode_pciout(0x8000036C, 0x01230801)

    // The memory test's set-up, for boot/start.S (§4.1): the larger chips' geometry, top of memory still 128 MB.
    xcode_poke(NV(0x100200), 0x03070103)
    xcode_poke(NV(0x100204), 0x11448000)

    // The memory test's result to the SMC (§4.4): register 0x13 the lanes with the larger chips, 0x12 the faulty
    // lanes. Without these writes the 1.6 console resets about 0.4 s after power-on. boot/start.S tests the memory
    // only later, so the values are the result of a 64 MB console (lanes 0-3 larger, 4-7 absent), as Cromwell sends
    // them (boot_rom/Xcodes.h, "We emulate Good memory result to PIC").
    xcode_outb(SMBUS + 4, 0x00000020)
    SMB_xcode_Write(0x13, 0x0F)
    SMB_xcode_Write(0x12, 0xF0)

    // The NV2A's registers to 0xFD000000 (§2.10).
    xcode_pciout(0x8000F020, 0xFDF0FD00)
    xcode_pciout(0x80010010, 0xFD000000)

    // Cromwell's overflow trick (spec/stock/24 §1.4): `mov eax, 0xFFFC1000; jmp eax` at RAM address 0, where the
    // boot ROM's exit continues; boot/start.S runs at bank offset 0x1000.
    xcode_poke(0x00000000, 0xFC1000B8)
    xcode_poke(0x00000004, 0x90E0FFFF)
    xcode_END(0x806)
