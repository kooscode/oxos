// The open image's 2BL (spec/stock/01 §5): what its two C++ files share. bl.cpp is the 2BL itself; netboot.cpp, linked
// in NETBOOT=1 images only, asks the network for a boot image before the 2BL unpacks the kernel in flash
// (spec/oxos_internal/09_netboot.md). Both run with paging off (physical addresses), interrupts off.
#pragma once

typedef unsigned char UCHAR;
typedef unsigned long ULONG;

// The C library functions the compiler may call (bl.cpp: there is no C library here).
extern "C" void *memcpy(void *d, const void *s, unsigned n);
extern "C" void *memset(void *d, int c, unsigned n);

// A POST code on port 0x80 (the debug LEDs or a modchip's display show it).
static inline void BlPost(UCHAR code) {
    __asm__ volatile("outb %0, $0x80" ::"a"(code));
}

// The boot payload, written by tools/mkbios.py at bank offset PAYLOAD_OFFSET: where the kernel and the key block are
// (in flash, or in a bank netboot.cpp downloaded).
struct XK_BOOT_PAYLOAD {
    ULONG Magic;                      // 'OXKP'
    ULONG KernelOffset, KernelPacked; // raw deflate stream, bank-relative
    ULONG KernelSize;                 // the flat kernel image
    ULONG KeysOffset, KeysSize;       // the key block handed to the kernel
    ULONG SplashOffset, SplashSize;   // the boot splash copy in this bank (0, 0 if none; read by the kernel)
};
constexpr ULONG PAYLOAD_OFFSET = 0x6000, PAYLOAD_MAGIC = 0x504B584F; // 'OXKP'

namespace netboot {

// Brings up the network card and asks the PC for a boot image (UDP broadcasts to port 5141): the downloaded bank in
// RAM, or null (no card, no link, no answer, or a bad download: the flash image boots as usual). The card is reset
// again either way.
const UCHAR *Boot();

} // namespace netboot
