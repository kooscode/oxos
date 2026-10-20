// Debug serial port (spec/oxos-internal/01-debug-serial.md): SuperIO serial port 1 at 0x3F8, polled. Compiled in with SERIAL=1.
#pragma once
#include <xk/types.h>

#if XK_DEBUG_SERIAL
void HalpSerialInit();
void HalpSerialWrite(const char *Text, ULONG Length);
void XkSerialPrint(const char *Format, ...);
extern volatile BOOLEAN HalpSerialPresent;
#define XK_SERIAL(...) XkSerialPrint(__VA_ARGS__)
#else
#define XK_SERIAL(...) ((void)0)
#endif
