// The GDB stub's serial transport (serial_transport.h, spec/oxos_internal/51 §4): COM1 polled, with the Modxo's rules (spec/oxos_internal/51 §4.1): RBR is
// read only after LSR says a byte is there (the Modxo returns junk otherwise), and LSR 0xFF means no host (no UART, or the Modxo with no PC holding DTR).
#include <gdb_stub/serial_transport.h>

namespace oxos::gdb_stub::serial_transport {

namespace {

constexpr uint16_t SIO_INDEX = 0x2E, SIO_DATA = 0x2F, COM1 = 0x3F8;
constexpr uint16_t UART_RBR = COM1, UART_THR = COM1, UART_DLL = COM1, UART_IER = COM1 + 1, UART_DLM = COM1 + 1, UART_FCR = COM1 + 2, UART_LCR = COM1 + 3,
                   UART_MCR = COM1 + 4, UART_LSR = COM1 + 5;
constexpr uint8_t LSR_DATA = 0x01, LSR_ROOM = 0x20, LSR_SENT = 0x40, LSR_NONE = 0xFF;
constexpr uint32_t WRITE_POLLS = 200000; // LSR reads (about 1 us each on the LPC bus) before a byte is given up: about 0.2 s

uint8_t ReadPort(uint16_t Port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(Port));
    return value;
}

void WritePort(uint16_t Port, uint8_t Value) {
    __asm__ volatile("outb %0, %1" ::"a"(Value), "Nd"(Port));
}

void WriteSuperIO(uint8_t Register, uint8_t Value) {
    WritePort(SIO_INDEX, Register);
    WritePort(SIO_DATA, Value);
}

// A byte from COM1, if one is there.
BOOLEAN ReadByte(UCHAR *Byte) {
    uint8_t lsr = ReadPort(UART_LSR);
    if (lsr == LSR_NONE || !(lsr & LSR_DATA))
        return FALSE;
    *Byte = ReadPort(UART_RBR);
    return TRUE;
}

BOOLEAN WriteByte(UCHAR Byte) {
    for (uint32_t i = 0; i < WRITE_POLLS; i++) {
        uint8_t lsr = ReadPort(UART_LSR);
        if (lsr == LSR_NONE)
            return FALSE;
        if (lsr & LSR_ROOM) {
            WritePort(UART_THR, Byte);
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN IsHostPresent() {
    return ReadPort(UART_LSR) != LSR_NONE;
}

void Flush() {
    for (uint32_t i = 0; i < WRITE_POLLS; i++) {
        uint8_t lsr = ReadPort(UART_LSR);
        if (lsr == LSR_NONE || (lsr & LSR_SENT))
            return;
    }
}

const OXOS_DEBUG_TRANSPORT SERIAL_TRANSPORT = {sizeof(OXOS_DEBUG_TRANSPORT), "serial", ReadByte, WriteByte, IsHostPresent, Flush};

} // namespace

void Start() {
    // Set up already (a kernel with its serial log), or the Modxo with a PC: its set-up writes are ignored, and leaving the configuration mode would empty
    // its buffers.
    if (ReadPort(UART_LSR) != LSR_NONE)
        return;
    WritePort(SIO_INDEX, 0x55); // the SuperIO's configuration mode
    WritePort(SIO_INDEX, 0x26);
    if (ReadPort(SIO_DATA) != SIO_INDEX) {
        WritePort(SIO_INDEX, 0xAA);
        return; // no SuperIO there (or the Modxo with no PC: it answers once one connects)
    }
    WriteSuperIO(0x07, 4); // logical device: serial port 1
    WriteSuperIO(0x60, COM1 >> 8);
    WriteSuperIO(0x61, COM1 & 0xFF);
    WriteSuperIO(0x70, 4);
    WriteSuperIO(0x30, 1);       // activate
    WritePort(SIO_INDEX, 0xAA); // leave the configuration mode
    WritePort(UART_LCR, 0x80);  // the divisor latch: 115200 baud
    WritePort(UART_DLL, 1);
    WritePort(UART_DLM, 0);
    WritePort(UART_LCR, 0x03); // 8N1
    WritePort(UART_FCR, 0x07);
    WritePort(UART_MCR, 0x03);
    WritePort(UART_IER, 0);
}

const OXOS_DEBUG_TRANSPORT &Transport() {
    return SERIAL_TRANSPORT;
}

} // namespace oxos::gdb_stub::serial_transport
