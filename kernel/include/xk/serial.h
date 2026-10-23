// Debug serial port (spec/oxos-internal/03-debug-serial.md): SuperIO serial port 1 at 0x3F8, polled. Compiled in with SERIAL=1.
#pragma once
#include <xk/types.h>

// Start-up marks (spec/oxos-internal/03 §8): until the title starts, every log point also leaves the address of its
// format string in CMOS 0x70-0x76 ('P', address, count), in every build. A console that freezes during start-up with
// no log (a release build) names the last log point it passed at the next boot (a network-log build reports it).
extern "C" void XkBootMark(const char *Format);
// A start-up mark that is never printed (hot paths: each disk transfer, each SMBus transaction).
#define XK_MARK(Label) XkBootMark(Label)

#if XK_DEBUG_SERIAL
void HalpSerialInit();
void HalpSerialWrite(const char *Text, ULONG Length);
void XkSerialPrint(const char *Format, ...);
extern volatile BOOLEAN HalpSerialPresent;
#define XK_SERIAL(...) XkSerialPrint(__VA_ARGS__)
#else
#define XK_SERIAL(Format, ...) XkBootMark(Format)
#endif
