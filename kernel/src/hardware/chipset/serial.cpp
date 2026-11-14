// Debug serial port (spec/oxos_internal/03_debug_serial.md): an SMSC LPC47M15x-compatible SuperIO on the LPC bus (XboxDev serial-usb-adapter, Modxo, xemu's
// lpc47m157), serial port 1 at 0x3F8, polled 16550. Only in SERIAL=1 builds.
#include <arch/arch.h>
#include <diagnostics/diagnostics.h>
#include <hardware/chipset/serial.h>
#include <runtime/runtime.h>

namespace xk::hardware::chipset {

#if XK_DEBUG_SERIAL
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)

constexpr USHORT SIO_INDEX = 0x2E, SIO_DATA = 0x2F, COM1 = 0x3F8;
constexpr USHORT UART_THR = COM1, UART_DLL = COM1, UART_IER = COM1 + 1, UART_DLM = COM1 + 1, UART_FCR = COM1 + 2, UART_LCR = COM1 + 3, UART_MCR = COM1 + 4,
                 UART_LSR = COM1 + 5;
constexpr ULONG SERIAL_TIMEOUT = 200000; // LSR polls (about 1 us each on the LPC bus) before giving up on a byte

volatile BOOLEAN SerialPresent; // a UART answered at set-up and has not timed out since

static void WriteSio(UCHAR Register, UCHAR Value) {
    outb(SIO_INDEX, Register);
    outb(SIO_DATA, Value);
}

static UCHAR ReadSio(UCHAR Register) {
    outb(SIO_INDEX, Register);
    return inb(SIO_DATA);
}

void InitSerial() {
    SerialPresent = FALSE;
    outb(SIO_INDEX, 0x55); // enter configuration mode
    if (ReadSio(0x26) != SIO_INDEX) {
        outb(SIO_INDEX, 0xAA);
        return;
    }
    WriteSio(0x07, 4); // logical device: serial port 1
    WriteSio(0x60, COM1 >> 8);
    WriteSio(0x61, COM1 & 0xFF);
    WriteSio(0x70, 4);
    WriteSio(0x30, 1);     // activate
    outb(SIO_INDEX, 0xAA); // leave configuration mode

    outb(UART_LCR, 0x80); // divisor latch: 115200 baud
    outb(UART_DLL, 1);
    outb(UART_DLM, 0);
    outb(UART_LCR, 0x03); // 8N1
    outb(UART_FCR, 0x07);
    outb(UART_MCR, 0x03);
    outb(UART_IER, 0);
    SerialPresent = inb(UART_LSR) != 0xFF;
}

static bool PutSerialByte(UCHAR c) {
    for (ULONG i = 0; i < SERIAL_TIMEOUT; i++) {
        if (inb(UART_LSR) & 0x20) {
            outb(UART_THR, c);
            return true;
        }
    }
    SerialPresent = FALSE; // stuck or removed: stop rather than slow every line down
    return false;
}

void WriteSerial(const char *Text, ULONG Length) {
    if (!SerialPresent)
        return;
    ULONG flags = save_flags_cli();
    for (ULONG i = 0; i < Length; i++) {
        if (Text[i] == '\n' && !PutSerialByte('\r'))
            break;
        if (!PutSerialByte((UCHAR)Text[i]))
            break;
    }
    restore_flags(flags);
}

void SerialPrint(const char *Format, ...) {
    diagnostics::BootMark(Format);
    if (!SerialPresent)
        return;
    char line[256];
    va_list ap;
    va_start(ap, Format);
    int n = RtlVsnprintf(line, sizeof(line), Format, ap);
    va_end(ap);
    XK_MARK("mark: log line formatted");
    WriteSerial(line, n < 0 || n >= (int)sizeof(line) ? sizeof(line) - 1 : (ULONG)n);
    XK_MARK("mark: log line written");
}
#endif

} // namespace xk::hardware::chipset
