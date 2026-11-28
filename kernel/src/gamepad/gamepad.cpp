// The gamepads during the boot splash (spec/oxos_internal/32 §2-4): USB sets up the devices (hardware/usb/devices.cpp), and each Xbox pad (interface class
// 0x58) gets an interrupt-IN pipe. The splash then reads the pads between its frames; X pressed at any time sets XButtonPressed, B BButtonPressed (safe mode,
// spec/oxos_internal/34 §3.3). Boot-only.
#include <gamepad/gamepad.h>
#include <hardware/usb/usb.h>
#include <core/core.h>
#include <diagnostics/diagnostics.h>

namespace xk::gamepad {

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

// XID (the Xbox input device class): the interface class, and GET_REPORT, a class request to the interface.
constexpr UCHAR CLASS_XBOX_INPUT = 0x58;
constexpr UCHAR FROM_INTERFACE_CLASS = 0xA1, REQUEST_GET_REPORT = 1;
constexpr USHORT REPORT_INPUT = 0x0100;
// The Xbox pad's input report: 0, its length (20), the digital buttons (16 bits), then the eight analog buttons A, B, X, Y, BLACK, WHITE, LT, RT (0-255), then
// the sticks.
constexpr ULONG REPORT_B = 5, REPORT_X = 6;
constexpr UCHAR BUTTON_PRESSED = 0x80; // pressed at least halfway

volatile ULONG XButtonPressed;
volatile ULONG BButtonPressed;
volatile ULONG ControllersFound;
static bool UsbRunning; // the controller was started (stopped again by StopWatchingControllers)

// A report from a pad: X or B pressed?
static void CheckReport(const UCHAR *Report, LONG Length) {
    if (Length <= (LONG)REPORT_X || Report[1] <= REPORT_X)
        return;
    if (Report[REPORT_X] >= BUTTON_PRESSED && !XButtonPressed) {
        XButtonPressed = 1;
        XK_SERIAL("gamepad: X pressed: the File Manager opens after the splash\n");
    }
    if (Report[REPORT_B] >= BUTTON_PRESSED && !BButtonPressed) {
        BButtonPressed = 1;
        XK_SERIAL("gamepad: B pressed: safe mode, no kernel extensions\n");
    }
}

// A device USB set up: an Xbox pad when it has an Xbox input interface with an interrupt-IN endpoint (the first such interface); its pipe is opened and its
// state read now, for X already held.
static void FoundDevice(const USB_DEVICE &Device, const UCHAR *Config, LONG Length) {
    bool inPad = false;
    UCHAR interfaceNumber = 0, endpoint = 0;
    USHORT packet = 0;
    for (LONG i = 0; i + 2 <= Length && Config[i] >= 2; i += Config[i]) {
        const UCHAR *d = Config + i;
        if (d[1] == 4 && i + 6 <= Length) { // an interface
            inPad = d[5] == CLASS_XBOX_INPUT && !endpoint;
            if (inPad)
                interfaceNumber = d[2];
        } else if (d[1] == 5 && inPad && i + 6 <= Length && (d[2] & 0x80) && (d[3] & 3) == 3) { // an endpoint
            endpoint = d[2] & 0x0F;
            packet = (USHORT)((d[4] | d[5] << 8) & 0x7FF);
        }
    }
    if (!endpoint || !hardware::usb::OpenPipe(ControllersFound, Device, endpoint, packet))
        return;
    XK_SERIAL("gamepad: device %u: an Xbox pad\n", Device.Address);
    ControllersFound = ControllersFound + 1;
    // XID's GET_REPORT; a pad without it is read through its pipe only.
    UCHAR report[20];
    CheckReport(report, hardware::usb::ControlTransfer(Device, FROM_INTERFACE_CLASS, REQUEST_GET_REPORT, REPORT_INPUT, interfaceNumber, sizeof(report),
                                             report));
}

// --- The watch

void StartWatchingControllers() {
    ControllersFound = 0;
    if (!hardware::usb::Start())
        return;
    UsbRunning = true;
    hardware::usb::SetUpDevices(FoundDevice);
}

void PollControllers() {
    for (ULONG i = 0; i < ControllersFound; i++) {
        UCHAR report[64];
        CheckReport(report, hardware::usb::ReadPipe(i, report, sizeof(report)));
    }
}

void StopWatchingControllers() {
    if (!UsbRunning)
        return;
    UsbRunning = false;
    hardware::usb::Stop();
}

void WaitForXButton() {
    while (!XButtonPressed) {
        StartWatchingControllers();
        for (ULONG ms = 0; !XButtonPressed && (ControllersFound || ms < 3000); ms += 10) {
            PollControllers();
            LARGE_INTEGER t;
            t.QuadPart = -10000LL * 10;
            KeDelayExecutionThread(KernelMode, FALSE, &t);
        }
        StopWatchingControllers();
    }
}

} // namespace xk::gamepad
