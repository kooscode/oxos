// The devices on the USB ports (spec/oxos_internal/32 §2-4): every device on the root hub's ports is given an address
// and its first configuration, a hub's own ports too (each Xbox controller is a hub with the pad and its memory-unit
// slots on it), and every device that is not a hub is handed to the caller. Boot-only.
#include <hardware/usb/usb.h>
#include <core/core.h>
#include <hardware/chipset/serial.h>

namespace xk::hardware::usb {

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

// USB standard requests and descriptor types (USB 2.0 chapter 9, chapter 11 for hubs).
constexpr UCHAR REQUEST_GET_STATUS = 0, REQUEST_CLEAR_FEATURE = 1, REQUEST_SET_FEATURE = 3, REQUEST_SET_ADDRESS = 5,
                REQUEST_GET_DESCRIPTOR = 6, REQUEST_SET_CONFIGURATION = 9;
constexpr UCHAR TO_DEVICE = 0x00, FROM_DEVICE = 0x80, FROM_HUB = 0xA0, TO_HUB_PORT = 0x23, FROM_HUB_PORT = 0xA3;
constexpr USHORT DESCRIPTOR_DEVICE = 0x0100, DESCRIPTOR_CONFIGURATION = 0x0200, DESCRIPTOR_HUB = 0x2900;
constexpr UCHAR CLASS_HUB = 0x09;
constexpr USHORT HUB_PORT_RESET = 4, HUB_PORT_POWER = 8, HUB_C_PORT_CONNECTION = 16, HUB_C_PORT_RESET = 20;

// Hubs followed from a root port: a controller is a hub (1); xemu puts its own hub in front of the controllers (2).
constexpr ULONG MAX_HUB_DEPTH = 3;

static UCHAR LastAddress;         // the last address given
static DEVICE_FOUND DeviceFound;  // SetUpDevices's caller

static void SleepMs(ULONG Ms) {
    LARGE_INTEGER wait;
    wait.QuadPart = -10000LL * Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &wait);
}

static void SetUpDevice(bool LowSpeed, ULONG Depth);

// A hub's ports: powered, then each connected one reset and its device set up.
static void SetUpHub(const USB_DEVICE &Hub, ULONG Depth) {
    UCHAR descriptor[9];
    if (ControlTransfer(Hub, FROM_HUB, REQUEST_GET_DESCRIPTOR, DESCRIPTOR_HUB, 0, sizeof(descriptor), descriptor) < 6)
        return;
    ULONG ports = descriptor[2] < 8 ? descriptor[2] : 7;
    for (USHORT p = 1; p <= ports; p++)
        ControlTransfer(Hub, TO_HUB_PORT, REQUEST_SET_FEATURE, HUB_PORT_POWER, p, 0, nullptr);
    SleepMs(descriptor[5] * 2 + 100); // the hub's power-good time (2 ms units), then 100 ms for its devices to settle
    for (USHORT p = 1; p <= ports; p++) {
        UCHAR status[4];
        if (ControlTransfer(Hub, FROM_HUB_PORT, REQUEST_GET_STATUS, 0, p, 4, status) != 4 || !(status[0] & 1))
            continue; // nothing connected
        ControlTransfer(Hub, TO_HUB_PORT, REQUEST_SET_FEATURE, HUB_PORT_RESET, p, 0, nullptr);
        for (ULONG ms = 0; ms < 100; ms += 10) {
            SleepMs(10);
            if (ControlTransfer(Hub, FROM_HUB_PORT, REQUEST_GET_STATUS, 0, p, 4, status) == 4 && (status[2] & 0x10))
                break; // the reset has ended
        }
        ControlTransfer(Hub, TO_HUB_PORT, REQUEST_CLEAR_FEATURE, HUB_C_PORT_RESET, p, 0, nullptr);
        ControlTransfer(Hub, TO_HUB_PORT, REQUEST_CLEAR_FEATURE, HUB_C_PORT_CONNECTION, p, 0, nullptr);
        if (!(status[0] & 2)) // not enabled
            continue;
        SleepMs(10); // reset recovery
        SetUpDevice(status[1] & 0x02, Depth + 1);
    }
}

// The device just reset at address 0 (behind Depth hubs): an address, its configuration, then a hub's ports, or (any
// other device, or a hub too deep to follow) the device handed to the caller.
static void SetUpDevice(bool LowSpeed, ULONG Depth) {
    USB_DEVICE device = {0, 8, LowSpeed};
    UCHAR head[8];
    if (ControlTransfer(device, FROM_DEVICE, REQUEST_GET_DESCRIPTOR, DESCRIPTOR_DEVICE, 0, 8, head) < 8)
        return;
    device.MaxPacket0 = head[7] ? head[7] : 8;
    UCHAR address = ++LastAddress;
    if (address > 127 || ControlTransfer(device, TO_DEVICE, REQUEST_SET_ADDRESS, address, 0, 0, nullptr) < 0)
        return;
    device.Address = address;
    SleepMs(2); // the device takes its address
    UCHAR config[256];
    if (ControlTransfer(device, FROM_DEVICE, REQUEST_GET_DESCRIPTOR, DESCRIPTOR_CONFIGURATION, 0, 9, config) < 9)
        return;
    ULONG total = config[2] | config[3] << 8;
    if (total > sizeof(config))
        total = sizeof(config);
    LONG length = ControlTransfer(device, FROM_DEVICE, REQUEST_GET_DESCRIPTOR, DESCRIPTOR_CONFIGURATION, 0,
                                  (USHORT)total, config);
    // A hub by its device class or by one of its interfaces.
    bool hub = head[4] == CLASS_HUB;
    for (LONG i = 0; i + 2 <= length && config[i] >= 2; i += config[i]) {
        if (config[i + 1] == 4 && i + 6 <= length) // an interface
            hub |= config[i + 5] == CLASS_HUB;
    }
    if (ControlTransfer(device, TO_DEVICE, REQUEST_SET_CONFIGURATION, config[5], 0, 0, nullptr) < 0)
        return;
    XK_SERIAL("usb: device %u: class %02x%s\n", address, head[4], hub ? ", a hub" : "");
    if (hub && Depth < MAX_HUB_DEPTH)
        return SetUpHub(device, Depth);
    DeviceFound(device, config, length);
}

void SetUpDevices(DEVICE_FOUND Found) {
    DeviceFound = Found;
    LastAddress = 0;
    for (ULONG p = 0; p < RootPorts(); p++) {
        bool lowSpeed;
        if (ResetPort(p, &lowSpeed))
            SetUpDevice(lowSpeed, 0);
    }
}

} // namespace xk::hardware::usb
