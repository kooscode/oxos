// The GDB stub's serial transport (spec/oxos_internal/51 §4): COM1 at 0x3F8, a 16550 subset polled (LSR bits 0, 5, 6; RBR; THR), as the SuperIO of the
// XboxDev serial adapter, a debug kit, xemu's lpc47m157 and the Modxo's firmware answer it. Built into GDB=1 kernels and oxos_gdb.xos with the stub.
#pragma once
#include <gdb_stub/gdb_stub.h>

namespace oxos::gdb_stub::serial_transport {

// Sets COM1 up when nothing answers there yet (a kernel without its serial log has not: the SuperIO's serial port 1 activated, 115200 8N1). The transport
// works from then on; a console without a serial port only never gets a host.
void Start();
// The transport's functions.
const OXOS_DEBUG_TRANSPORT &Transport();

} // namespace oxos::gdb_stub::serial_transport
