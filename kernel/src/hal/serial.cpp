// Debug serial port (spec/oxos-internal/01-debug-serial.md): an SMSC LPC47M15x-compatible SuperIO on the LPC bus (XboxDev serial-usb-adapter,
// Modxo, xemu's lpc47m157), serial port 1 at 0x3F8, polled 16550. Only in SERIAL=1 builds.
#include <xk/arch.h>
#include <xk/serial.h>

#if XK_DEBUG_SERIAL
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
XBAPI int __cdecl RtlVsnprintf(char *Buffer, SIZE_T Size, const char *Format, va_list Args);

constexpr USHORT SIO_INDEX = 0x2E, SIO_DATA = 0x2F, COM1 = 0x3F8;
constexpr USHORT UART_THR = COM1, UART_DLL = COM1, UART_IER = COM1 + 1, UART_DLM = COM1 + 1, UART_FCR = COM1 + 2,
                 UART_LCR = COM1 + 3, UART_MCR = COM1 + 4, UART_LSR = COM1 + 5;
constexpr ULONG SERIAL_TIMEOUT = 200000; // LSR polls (about 1 us each on the LPC bus) before giving up on a byte

volatile BOOLEAN HalpSerialPresent;

static void HalpSioWrite(UCHAR Register, UCHAR Value) {
    outb(SIO_INDEX, Register);
    outb(SIO_DATA, Value);
}

static UCHAR HalpSioRead(UCHAR Register) {
    outb(SIO_INDEX, Register);
    return inb(SIO_DATA);
}

void HalpSerialInit() {
    HalpSerialPresent = FALSE;
    outb(SIO_INDEX, 0x55); // enter configuration mode
    if (HalpSioRead(0x26) != SIO_INDEX) {
        outb(SIO_INDEX, 0xAA);
        return;
    }
    HalpSioWrite(0x07, 4); // logical device: serial port 1
    HalpSioWrite(0x60, COM1 >> 8);
    HalpSioWrite(0x61, COM1 & 0xFF);
    HalpSioWrite(0x70, 4);
    HalpSioWrite(0x30, 1); // activate
    outb(SIO_INDEX, 0xAA); // leave configuration mode

    outb(UART_LCR, 0x80); // divisor latch: 115200 baud
    outb(UART_DLL, 1);
    outb(UART_DLM, 0);
    outb(UART_LCR, 0x03); // 8N1
    outb(UART_FCR, 0x07);
    outb(UART_MCR, 0x03);
    outb(UART_IER, 0);
    HalpSerialPresent = inb(UART_LSR) != 0xFF;
}

static bool HalpSerialPut(UCHAR c) {
    for (ULONG i = 0; i < SERIAL_TIMEOUT; i++)
        if (inb(UART_LSR) & 0x20) {
            outb(UART_THR, c);
            return true;
        }
    HalpSerialPresent = FALSE; // stuck or removed: stop rather than slow every line down
    return false;
}

void HalpSerialWrite(const char *Text, ULONG Length) {
    if (!HalpSerialPresent)
        return;
    ULONG flags = save_flags_cli();
    for (ULONG i = 0; i < Length; i++) {
        if (Text[i] == '\n' && !HalpSerialPut('\r'))
            break;
        if (!HalpSerialPut((UCHAR)Text[i]))
            break;
    }
    restore_flags(flags);
}

void XkSerialPrint(const char *Format, ...) {
    if (!HalpSerialPresent)
        return;
    char line[256];
    va_list ap;
    va_start(ap, Format);
    int n = RtlVsnprintf(line, sizeof(line), Format, ap);
    va_end(ap);
    HalpSerialWrite(line, n < 0 || n >= (int)sizeof(line) ? sizeof(line) - 1 : (ULONG)n);
}
#endif
