// USB (src/hardware/usb/, spec/oxos_internal/32_file_manager_button.md §3): the first OHCI controller (the four controller ports) started by polling, with
// control transfers and interrupt-IN pipes (host_controller.cpp), and the devices on its ports and behind hubs given addresses and configurations
// (devices.cpp). Boot-only: the splash thread uses it for the gamepads, and it is stopped and reset before the first title starts, so the title's own USB
// driver finds it as after power-on.
#pragma once
#include <runtime/types.h>

// A device on the bus: its address (0 until SET_ADDRESS), endpoint 0's packet size and its speed.
struct USB_DEVICE {
    UCHAR Address;
    UCHAR MaxPacket0;
    bool LowSpeed;
};

constexpr ULONG USB_HOST_PIPES = 4; // interrupt-IN pipes: one per controller port

namespace xk::hardware::usb {

// --- The host controller (host_controller.cpp)

bool Start();       // the controller reset, running and its ports powered; false without it or its memory
void Stop();        // reset (USBRESET: no more DMA, the bus reset) and its memory freed
ULONG RootPorts();  // the root hub's ports
// Resets root port Port (0-based) when a device is connected; true when the port is then enabled.
bool ResetPort(ULONG Port, bool *LowSpeed);
// One control transfer on endpoint 0 (Data: Length bytes in or out, by RequestType's direction bit): the bytes moved, or -1 on an error or after 100 ms.
LONG ControlTransfer(const USB_DEVICE &Device, UCHAR RequestType, UCHAR Request, USHORT Value, USHORT Index, USHORT Length, PVOID Data);
// Interrupt-IN pipe Pipe (< USB_HOST_PIPES) on Endpoint (1-15) of Device, reading packets of MaxPacket bytes (at most 64); false when Pipe is out of range.
bool OpenPipe(ULONG Pipe, const USB_DEVICE &Device, UCHAR Endpoint, USHORT MaxPacket);
// Without waiting: the length of the packet that pipe Pipe received since the last call (copied to Data, at most Size bytes), 0 when none, -1 when the pipe
// stopped on an error.
LONG ReadPipe(ULONG Pipe, PVOID Data, ULONG Size);

// --- Devices (devices.cpp)

// A configured device, with its configuration descriptor and the descriptors after it (Length bytes) for the caller to look through.
using DEVICE_FOUND = void (*)(const USB_DEVICE &Device, const UCHAR *Configuration, LONG Length);

// After Start: every device on the root ports and behind hubs gets an address and its first configuration; Found is called for each device not followed as a
// hub.
void SetUpDevices(DEVICE_FOUND Found);

} // namespace xk::hardware::usb
