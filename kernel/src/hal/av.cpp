// Audio/video: AV pack, encoder detection, display mode programming (spec/12 §4).
// The per-mode encoder and CRTC/RAMDAC tables of the reference are not reproduced: modes are programmed from
// the documented VGA/NV2A register model (640x480 timings for every mode for now; see the spec).
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>

XBAPI NTSTATUS NTAPI HalReadSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue, PULONG DataValue);
XBAPI NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG, PULONG, PVOID, ULONG, PULONG);

XBAPI UCHAR HalBootSMCVideoMode;
XK_PERSIST UCHAR HalBootSMCVideoMode = 7; // SMC AV pack code (7 = none) read at boot
XK_PERSIST static PVOID AvpSavedDataAddress;
XK_PERSIST static ULONG AvpCurrentMode;
static UCHAR AvpEncoderAddress; // 0x8A Conexant, 0xD4 Focus, 0xE0 Xcalibur (0 = none found)
volatile ULONG AvpSetDisplayModeCalls;

void AvInit() {
    ULONG v;
    if (NT_SUCCESS(HalReadSMBusValue(0x20, 0x04, FALSE, &v)))
        HalBootSMCVideoMode = (UCHAR)v;
    static const UCHAR encoders[] = {0x8A, 0xD4, 0xE0};
    for (UCHAR a : encoders)
        if (NT_SUCCESS(HalReadSMBusValue(a, 0x00, FALSE, &v))) {
            AvpEncoderAddress = a;
            break;
        }
}

XBAPI PVOID NTAPI AvGetSavedDataAddress() { return AvpSavedDataAddress; }
XBAPI void NTAPI AvSetSavedDataAddress(PVOID Address) { AvpSavedDataAddress = Address; }

static ULONG AvpPackFromSmc(UCHAR smc) {
    switch (smc) {
    case 0: return 3; // SCART
    case 1: return 4; // HDTV
    case 2: return 5; // VGA
    case 3: return 2; // RFU
    case 4: return 6; // S-Video
    case 6: return 1; // standard (composite)
    default: return 0;
    }
}

// AV pack | video standard (factory setting 0x103, & 0xC0FF00, default NTSC-M 60 Hz) | user video flags (setting 8).
static ULONG AvpCapabilities() {
    ULONG type, len, standard = 0, flags = 0;
    if (ExQueryNonVolatileSetting(0x103, &type, &standard, 4, &len) || len != 4)
        standard = 0x400100;
    if (ExQueryNonVolatileSetting(8, &type, &flags, 4, &len) || len != 4)
        flags = 0;
    return (flags & 0xFFFF0000) | AvpPackFromSmc(HalBootSMCVideoMode) | (standard & 0xC0FF00);
}

constexpr ULONG AV_QUERY_AV_CAPABILITIES = 6, AV_OPTION_BLANK_SCREEN = 9, AV_OPTION_NULL_MODE = 0xC,
                AV_QUERY_ENCODER_TYPE = 0x10, AV_QUERY_MODE_TABLE_VERSION = 0x11;

XBAPI void NTAPI AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result) {
    auto regs = (volatile UCHAR *)(RegisterBase ? RegisterBase : (PVOID)0xFD000000);
    switch (Option) {
    case AV_QUERY_AV_CAPABILITIES:
        *Result = AvpCapabilities();
        return;
    case AV_OPTION_BLANK_SCREEN: // VGA sequencer register 1: screen off/on
        regs[0xC03C4] = 1;
        regs[0xC03C5] = Param ? 0x21 : 0x01;
        if (!Param)
            outb(0x80D3, 4);
        return;
    case AV_OPTION_NULL_MODE:
        AvpCurrentMode = 0;
        return;
    case AV_QUERY_ENCODER_TYPE:
        *Result = AvpEncoderAddress == 0xD4;
        return;
    default:
        // Encoder options (macrovision, flicker filter, widescreen, ...) are accepted without effect for now.
        if (Result && (Option == AV_QUERY_MODE_TABLE_VERSION || Option == 5 || Option == 0xF))
            *Result = 0;
        return;
    }
}

static void AvpCrtc(volatile UCHAR *regs, UCHAR index, UCHAR value) {
    regs[0x6013D4] = index;
    regs[0x6013D5] = value;
}

// Standard 640x480 VGA timings (25.175 MHz class): 800 x 525 total.
static const UCHAR AvpVgaTimings[][2] = {
    {0x00, 0x5F}, {0x01, 0x4F}, {0x02, 0x50}, {0x03, 0x82}, {0x04, 0x54}, {0x05, 0x80}, {0x06, 0x0B},
    {0x07, 0x3E}, {0x08, 0x00}, {0x09, 0x40}, {0x10, 0xEA}, {0x11, 0x0C}, {0x12, 0xDF}, {0x14, 0x00},
    {0x15, 0xE7}, {0x16, 0x04}, {0x17, 0xE3},
};

XBAPI ULONG NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch,
                                   ULONG FrameBuffer) {
    auto regs = (volatile UCHAR *)RegisterBase;
    AvpSetDisplayModeCalls = AvpSetDisplayModeCalls + 1;
    if (!Mode)
        Mode = 0x0F010101;
    // Pixel format: RAMDAC general control and the CRTC pixel-depth code.
    ULONG generalControl = 0x100030, depth = 0x3000000;
    if (Format == 0x10 || Format == 0x1C)
        depth = 0x2000000;
    else if (Format == 0x11)
        generalControl = 0x101030, depth = 0x2000011;
    ULONG pitch8 = Pitch >> 3;
    if (Mode != AvpCurrentMode) {
        AvpCrtc(regs, 0x1F, 0x57); // unlock extended CRTC registers
        AvpCrtc(regs, 0x21, 0xFF);
        AvpCrtc(regs, 0x28, 0x00); // display off while reprogramming
        for (auto &t : AvpVgaTimings)
            AvpCrtc(regs, t[0], t[1]);
        regs[0xC03C3] = 0x01;
        regs[0xC03C2] = 0xE3; // misc output: 25 MHz clock, negative syncs
        AvpCurrentMode = Mode;
    }
    *(volatile ULONG *)(regs + 0x680600) = generalControl;
    AvpCrtc(regs, 0x13, (UCHAR)pitch8);
    AvpCrtc(regs, 0x19, (UCHAR)((pitch8 >> 3) & 0xE0));
    AvpCrtc(regs, 0x25, (UCHAR)((pitch8 >> 6) & 0x20));
    AvpCrtc(regs, 0x28, (UCHAR)((depth >> 24) | 0x80));
    *(volatile ULONG *)(regs + 0x600800) = FrameBuffer;
    return 0;
}
