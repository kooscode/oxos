// Debug serial port (spec/oxos_internal/03_debug_serial.md): the UART driver, SuperIO serial port 1 at 0x3F8, polled. Compiled in with SERIAL=1. The log lines
// that go out through it (XK_SERIAL, DbgPrint, oxos_log) are formatted by diagnostics::LogLine (diagnostics/serial_log.cpp).
#pragma once
#include <runtime/types.h>

#if XK_DEBUG_SERIAL
namespace xk::hardware::chipset {
void InitSerial();                                // finds the SuperIO and sets the UART up; SerialPresent says whether one answered
void WriteSerial(const char *Text, ULONG Length); // Length bytes, a CR before each LF; gives up (SerialPresent false) when the UART stops taking them
extern volatile BOOLEAN SerialPresent;
} // namespace xk::hardware::chipset
#endif
