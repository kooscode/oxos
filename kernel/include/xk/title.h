// The title-start call that game-trainer loaders hook (spec/oxos-internal/23-trainer-hook.md).
#pragma once

// `call [0x00010128]`: the XBE entry point, decoded by the loader into the image header. Dashboards (XBMC4Gamers)
// search the kernel image for exactly these bytes and redirect the call to their trainer stub.
constexpr unsigned char XE_TITLE_CALL[6] = {0xFF, 0x15, 0x28, 0x01, 0x01, 0x00};
// What the call looks like while a dashboard's trainer is installed (`call [0x8000000C]`).
constexpr unsigned char XE_TITLE_CALL_HOOKED[6] = {0xFF, 0x15, 0x0C, 0x00, 0x00, 0x80};

// Inline assembly, so the compiler cannot load the entry point into a register first. The title may clobber the
// caller-saved registers.
__attribute__((always_inline)) inline void XeCallTitleEntry() {
    asm volatile("call *0x00010128" ::: "eax", "ecx", "edx", "memory", "cc");
}
