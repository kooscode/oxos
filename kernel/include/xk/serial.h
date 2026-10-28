// Debug serial port (spec/oxos-internal/03-debug-serial.md): SuperIO serial port 1 at 0x3F8, polled. Compiled in with SERIAL=1.
#pragma once
#include <xk/types.h>

// Start-up marks (spec/oxos-internal/03 §8): until the title starts, every log point also leaves an address of its own
// in CMOS 0x70-0x76 ('P', address, count), in every build. A console that freezes during start-up with no log (a
// release build) names the last log point it passed at the next boot (a serial build reports it). With the
// serial log the address is the format string's; without it (release builds) the text is not needed, so each log
// point has a one-byte mark instead, named in the map after the function that holds it (the strings would take
// about 5 KB of resident memory, spec/oxos-internal/18-boot-memory.md).
extern "C" void XkBootMark(const char *Format);
#define XK_MARK_ID() ([] { static const char XkMark = 0; return &XkMark; }())
#if XK_DEBUG_SERIAL
// A start-up mark that is never printed (hot paths: each disk transfer, each SMBus transaction).
#define XK_MARK(Label) XkBootMark(Label)
#else
#define XK_MARK(Label) XkBootMark(XK_MARK_ID())
#endif

#if XK_DEBUG_SERIAL
void HalpSerialInit();
void HalpSerialWrite(const char *Text, ULONG Length);
void XkSerialPrint(const char *Format, ...);
extern volatile BOOLEAN HalpSerialPresent;
#define XK_SERIAL(...) XkSerialPrint(__VA_ARGS__)
#else
#define XK_SERIAL(Format, ...) XkBootMark(XK_MARK_ID())
#endif
