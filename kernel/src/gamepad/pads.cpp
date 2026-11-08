// The controllers during the boot splash (spec/oxos_internal/32 §2-4): every device on the root hub's ports is set
// up, a hub's own ports too (each Xbox controller is a hub with the pad and its memory-unit slots on it), and each Xbox
// pad (interface class 0x58) gets an interrupt-IN pipe. The splash then reads the pads between its frames; X
// pressed at any time sets GamepadXPressed. Boot-only.
#include <xk/gamepad.h>
#include <xk/usb_host.h>
#include <xk/core.h>
#include <xk/serial.h>

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

// USB standard requests and descriptor types (USB 2.0 chapter 9, chapter 11 for hubs).
constexpr UCHAR REQUEST_GET_STATUS = 0, REQUEST_CLEAR_FEATURE = 1, REQUEST_SET_FEATURE = 3, REQUEST_SET_ADDRESS = 5,
                REQUEST_GET_DESCRIPTOR = 6, REQUEST_SET_CONFIGURATION = 9;
constexpr UCHAR TO_DEVICE = 0x00, FROM_DEVICE = 0x80, FROM_HUB = 0xA0, TO_HUB_PORT = 0x23,
                FROM_HUB_PORT = 0xA3, FROM_INTERFACE_CLASS = 0xA1;
constexpr USHORT DESCRIPTOR_DEVICE = 0x0100, DESCRIPTOR_CONFIGURATION = 0x0200, DESCRIPTOR_HUB = 0x2900;
constexpr UCHAR CLASS_HUB = 0x09, CLASS_XBOX_INPUT = 0x58;
constexpr USHORT HUB_PORT_RESET = 4, HUB_PORT_POWER = 8, HUB_C_PORT_CONNECTION = 16, HUB_C_PORT_RESET = 20;
// The Xbox pad's input report: 0, its length (20), the digital buttons (16 bits), then the eight analog buttons
// A, B, X, Y, BLACK, WHITE, LT, RT (0-255), then the sticks.
constexpr ULONG REPORT_X = 6;
constexpr UCHAR X_PRESSED = 0x80; // pressed at least halfway

// Hubs followed from a root port: a controller is a hub (1); xemu puts its own hub in front of the controllers (2).
constexpr ULONG GAMEPAD_HUB_DEPTH = 3;

volatile ULONG GamepadXPressed;
volatile ULONG GamepadPadsFound;
static bool GamepadpRunning;   // the controller was started (stopped again by GamepadWatchStop)
static UCHAR GamepadpAddress;  // the last address given

// A report from a pad: X pressed?
static void GamepadpCheck(const UCHAR *Report, LONG Length) {
    if (Length > (LONG)REPORT_X && Report[1] > REPORT_X && Report[REPORT_X] >= X_PRESSED &&
        !GamepadXPressed) {
        GamepadXPressed = 1;
        XK_SERIAL("gamepad: X pressed: the File Manager opens after the splash\n");
    }
}

// --- Setting up devices

static void GamepadpSetUp(bool LowSpeed, ULONG Depth);

// A hub's ports: powered, then each connected one reset and its device set up.
static void GamepadpSetUpHub(const USB_DEVICE &Hub, ULONG Depth) {
    UCHAR descriptor[9];
    if (UsbHostControl(Hub, FROM_HUB, REQUEST_GET_DESCRIPTOR, DESCRIPTOR_HUB, 0, sizeof(descriptor), descriptor) < 6)
        return;
    ULONG ports = descriptor[2] < 8 ? descriptor[2] : 7;
    for (USHORT p = 1; p <= ports; p++)
        UsbHostControl(Hub, TO_HUB_PORT, REQUEST_SET_FEATURE, HUB_PORT_POWER, p, 0, nullptr);
    // The hub's power-good time (2 ms units), then 100 ms for its devices to settle.
    LARGE_INTEGER wait;
    wait.QuadPart = -10000LL * (descriptor[5] * 2 + 100);
    KeDelayExecutionThread(KernelMode, FALSE, &wait);
    for (USHORT p = 1; p <= ports; p++) {
        UCHAR status[4];
        if (UsbHostControl(Hub, FROM_HUB_PORT, REQUEST_GET_STATUS, 0, p, 4, status) != 4 || !(status[0] & 1))
            continue; // nothing connected
        UsbHostControl(Hub, TO_HUB_PORT, REQUEST_SET_FEATURE, HUB_PORT_RESET, p, 0, nullptr);
        for (ULONG ms = 0; ms < 100; ms += 10) {
            wait.QuadPart = -10000LL * 10;
            KeDelayExecutionThread(KernelMode, FALSE, &wait);
            if (UsbHostControl(Hub, FROM_HUB_PORT, REQUEST_GET_STATUS, 0, p, 4, status) == 4 && (status[2] & 0x10))
                break; // the reset has ended
        }
        UsbHostControl(Hub, TO_HUB_PORT, REQUEST_CLEAR_FEATURE, HUB_C_PORT_RESET, p, 0, nullptr);
        UsbHostControl(Hub, TO_HUB_PORT, REQUEST_CLEAR_FEATURE, HUB_C_PORT_CONNECTION, p, 0, nullptr);
        if (!(status[0] & 2)) // not enabled
            continue;
        wait.QuadPart = -10000LL * 10; // reset recovery
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
        GamepadpSetUp(status[1] & 0x02, Depth + 1);
    }
}

// The device just reset at address 0 (behind Depth hubs): an address, its configuration, then a hub's
// ports or a pad's pipe.
static void GamepadpSetUp(bool LowSpeed, ULONG Depth) {
    USB_DEVICE device = {0, 8, LowSpeed};
    UCHAR head[8];
    if (UsbHostControl(device, FROM_DEVICE, REQUEST_GET_DESCRIPTOR, DESCRIPTOR_DEVICE, 0, 8, head) < 8)
        return;
    device.MaxPacket0 = head[7] ? head[7] : 8;
    UCHAR address = ++GamepadpAddress;
    if (address > 127 || UsbHostControl(device, TO_DEVICE, REQUEST_SET_ADDRESS, address, 0, 0, nullptr) < 0)
        return;
    device.Address = address;
    LARGE_INTEGER wait;
    wait.QuadPart = -10000LL * 2; // the device takes its address
    KeDelayExecutionThread(KernelMode, FALSE, &wait);
    UCHAR config[256];
    if (UsbHostControl(device, FROM_DEVICE, REQUEST_GET_DESCRIPTOR, DESCRIPTOR_CONFIGURATION, 0, 9, config) < 9)
        return;
    ULONG total = config[2] | config[3] << 8;
    if (total > sizeof(config))
        total = sizeof(config);
    LONG length = UsbHostControl(device, FROM_DEVICE, REQUEST_GET_DESCRIPTOR, DESCRIPTOR_CONFIGURATION, 0,
                                 (USHORT)total, config);
    // The descriptors that follow: the first Xbox input interface's interrupt-IN endpoint; a hub interface.
    bool hub = head[4] == CLASS_HUB, inPad = false;
    UCHAR interfaceNumber = 0, endpoint = 0;
    USHORT packet = 0;
    for (LONG i = 0; i + 2 <= length && config[i] >= 2; i += config[i]) {
        const UCHAR *d = config + i;
        if (d[1] == 4 && i + 6 <= length) { // an interface
            inPad = d[5] == CLASS_XBOX_INPUT && !endpoint;
            hub |= d[5] == CLASS_HUB;
            if (inPad)
                interfaceNumber = d[2];
        } else if (d[1] == 5 && inPad && i + 6 <= length && (d[2] & 0x80) && (d[3] & 3) == 3) { // an endpoint
            endpoint = d[2] & 0x0F;
            packet = (USHORT)((d[4] | d[5] << 8) & 0x7FF);
        }
    }
    if (UsbHostControl(device, TO_DEVICE, REQUEST_SET_CONFIGURATION, config[5], 0, 0, nullptr) < 0)
        return;
    XK_SERIAL("gamepad: device %u: class %02x%s%s\n", address, head[4], hub ? ", a hub" : "",
              endpoint ? ", an Xbox pad" : "");
    if (hub && Depth < GAMEPAD_HUB_DEPTH)
        return GamepadpSetUpHub(device, Depth);
    if (!endpoint || !UsbHostOpenPipe(GamepadPadsFound, device, endpoint, packet))
        return;
    GamepadPadsFound = GamepadPadsFound + 1;
    // Its state now, for X already held: XID's GET_REPORT; a pad without it is read through its pipe only.
    UCHAR report[20];
    GamepadpCheck(report, UsbHostControl(device, FROM_INTERFACE_CLASS, 1, 0x0100, interfaceNumber, sizeof(report),
                                         report));
}

// --- The watch

void GamepadWatchStart() {
    GamepadPadsFound = 0;
    GamepadpAddress = 0;
    if (!UsbHostStart())
        return;
    GamepadpRunning = true;
    for (ULONG p = 0; p < UsbHostPorts(); p++) {
        bool lowSpeed;
        if (UsbHostResetPort(p, &lowSpeed))
            GamepadpSetUp(lowSpeed, 0);
    }
    XK_SERIAL("gamepad: %lu pads\n", GamepadPadsFound);
}

void GamepadWatchPoll() {
    for (ULONG i = 0; i < GamepadPadsFound; i++) {
        UCHAR report[64];
        GamepadpCheck(report, UsbHostReadPipe(i, report, sizeof(report)));
    }
}

void GamepadWatchStop() {
    if (!GamepadpRunning)
        return;
    GamepadpRunning = false;
    UsbHostStop();
}

void GamepadWaitForX() {
    while (!GamepadXPressed) {
        GamepadWatchStart();
        for (ULONG ms = 0; !GamepadXPressed && (GamepadPadsFound || ms < 3000); ms += 10) {
            GamepadWatchPoll();
            LARGE_INTEGER t;
            t.QuadPart = -10000LL * 10;
            KeDelayExecutionThread(KernelMode, FALSE, &t);
        }
        GamepadWatchStop();
    }
}
