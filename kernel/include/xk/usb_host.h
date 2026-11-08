// The USB host controller, as the boot reads the controllers (src/gamepad/usb_host.cpp,
// spec/oxos_internal/32_file_manager_button.md §3): the first OHCI controller (the four controller ports) started by
// polling, with control transfers and interrupt-IN pipes. Boot-only: the splash thread uses it, and it is stopped and
// reset before the first title starts, so the title's own USB driver finds it as after power-on.
#pragma once
#include <xk/types.h>

// A device on the bus: its address (0 until SET_ADDRESS), endpoint 0's packet size and its speed.
struct USB_DEVICE {
    UCHAR Address;
    UCHAR MaxPacket0;
    bool LowSpeed;
};

constexpr ULONG USB_HOST_PIPES = 4; // interrupt-IN pipes: one per controller port

bool UsbHostStart();  // the controller reset, running and its ports powered; false without it or its memory
void UsbHostStop();   // reset (USBRESET: no more DMA, the bus reset) and its memory freed
ULONG UsbHostPorts(); // the root hub's ports
// Resets root port Port (0-based) when a device is connected; true when the port is then enabled.
bool UsbHostResetPort(ULONG Port, bool *LowSpeed);
// One control transfer on endpoint 0 (Data: Length bytes in or out, by RequestType's direction bit): the bytes moved,
// or -1 on an error or after 100 ms.
LONG UsbHostControl(const USB_DEVICE &Device, UCHAR RequestType, UCHAR Request, USHORT Value, USHORT Index,
                    USHORT Length, PVOID Data);
// Interrupt-IN pipe Pipe (< USB_HOST_PIPES) on Endpoint (1-15) of Device, reading packets of MaxPacket bytes (at most
// 64); false when Pipe is out of range.
bool UsbHostOpenPipe(ULONG Pipe, const USB_DEVICE &Device, UCHAR Endpoint, USHORT MaxPacket);
// Without waiting: the length of the packet that pipe Pipe received since the last call (copied to Data, at most
// Size bytes), 0 when none, -1 when the pipe stopped on an error.
LONG UsbHostReadPipe(ULONG Pipe, PVOID Data, ULONG Size);
