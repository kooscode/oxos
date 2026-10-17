// Audio/video: AV pack, encoder detection, display mode programming (spec/12 §4).
// The per-mode encoder and CRTC/RAMDAC tables of the reference are not reproduced: modes are programmed from
// the documented VGA/NV2A register model with the standards' timings for each mode's picture size (spec/17 §7).
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>

XBAPI NTSTATUS NTAPI HalReadSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue, PULONG DataValue);
XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue);
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

// AV pack unplugged / plugged back (SMC events): the encoder's output control register 0xBA.
static UCHAR AvpOutputControl = 0x0F;
void AvpSmcSetOutputs(bool Enable) {
    if (AvpEncoderAddress && (!Enable || AvpCurrentMode))
        HalWriteSMBusValue(AvpEncoderAddress, 0xBA, FALSE, Enable ? (AvpOutputControl & 0x0F) | 0x20 : 0x3F);
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
    case 0x0D: // the current display mode word (titles' D3D derives its frame layout from it)
        *Result = AvpCurrentMode;
        return;
    case 0x05: { // encoder status bits (none for HDTV modes)
        ULONG v = 0;
        if (!(AvpCurrentMode & 0xC0000000) && AvpEncoderAddress == 0x8A &&
            NT_SUCCESS(HalReadSMBusValue(0x8A, 0x02, FALSE, &v)))
            v = (v & 0x0C) >> 2;
        else
            v = 0;
        *Result = v;
        return;
    }
    case 0x0F: { // Conexant: the field currently being sent (register 6, bit 0)
        ULONG v = 0;
        if (AvpEncoderAddress != 0x8A || !NT_SUCCESS(HalReadSMBusValue(0x8A, 0x06, FALSE, &v)))
            v = 0;
        *Result = v & 1;
        return;
    }
    case AV_QUERY_MODE_TABLE_VERSION:
        *Result = 0;
        return;
    case 0x13: // mode flag 0x10000000 (GPIO 0x80D6 bit 0 on the next mode set)
        AvpCurrentMode = (AvpCurrentMode & ~0x10000000u) | (Param ? 0x10000000u : 0);
        return;
    default:
        // Encoder writes (macrovision, flicker filter, widescreen, ...) need the encoder tables (spec/17): no effect yet.
        return;
    }
}

static void AvpCrtc(volatile UCHAR *regs, UCHAR index, UCHAR value) {
    regs[0x6013D4] = index;
    regs[0x6013D5] = value;
}

// Display timings per mode (spec/17 §7). The mode word's table (bits 30-31: 10b HDTV, 11b VGA, otherwise SDTV) and
// CRTC row (bits 8-15) select the picture size; the row meanings follow the D3D mode words (as nxdk's table lists
// them), and the timings are the public video standards' (CEA-861 480p, SMPTE 296M 720p, SMPTE 274M 1080i, VESA),
// not the reference's tables. Interlaced modes are programmed per field. xemu sizes its display from these CRTC
// registers (width = (CR01 + 1) * 8, height from CR12/CR07, doubled when CR39 says interlaced).
struct AVP_TIMING {
    UCHAR Table, Row;
    USHORT HDisplay, HSyncStart, HSyncEnd, HTotal;
    USHORT VDisplay, VSyncStart, VSyncEnd, VTotal; // per field when interlaced
    BOOLEAN Interlaced;
};
static const AVP_TIMING AvpTimings[] = {
    {2, 0x07, 640, 656, 752, 800, 480, 490, 492, 525, FALSE},      // 640x480p
    {2, 0x08, 720, 736, 798, 858, 480, 489, 495, 525, FALSE},      // 720x480p
    {2, 0x0A, 1280, 1390, 1430, 1650, 720, 725, 730, 750, FALSE},  // 1280x720p
    {2, 0x0C, 1920, 2008, 2052, 2200, 540, 542, 547, 562, TRUE},   // 1920x1080i
    {3, 0x06, 640, 656, 752, 800, 480, 490, 492, 525, FALSE},      // VGA 640x480
    {3, 0x03, 800, 840, 968, 1056, 600, 601, 605, 628, FALSE},     // VGA 800x600
    {3, 0x04, 1024, 1048, 1184, 1344, 768, 771, 777, 806, FALSE},  // VGA 1024x768
    {0, 0x02, 720, 736, 798, 858, 480, 489, 495, 525, FALSE},      // SDTV 720x480 (NTSC)
    {0, 0x04, 720, 736, 798, 864, 480, 489, 495, 525, FALSE},      // SDTV 720x480 (PAL)
};
static const AVP_TIMING AvpDefaultTiming = {0, 0, 640, 656, 752, 800, 480, 490, 492, 525, FALSE}; // SDTV 640x480

static const AVP_TIMING *AvpFindTiming(ULONG Mode) {
    UCHAR table = (UCHAR)(Mode >> 30), row = (UCHAR)(Mode >> 8);
    if (table == 1)
        table = 0;
    for (auto &t : AvpTimings)
        if (t.Table == table && t.Row == row)
            return &t;
    return &AvpDefaultTiming;
}

// VGA CRTC timing registers plus the NV extended bits (CR25 vertical bit 10s, CR2D horizontal bit 8s, CR39 the
// interlace half-line: 0xFF = progressive). Horizontal values are in 8-pixel characters.
XK_PERSIST static UCHAR AvpCr25; // CR25 timing bits (kept with AvpCurrentMode); pitch bit 5 is merged in at every mode set

static void AvpProgramTiming(volatile UCHAR *regs, const AVP_TIMING &t) {
    ULONG ht = t.HTotal / 8 - 5, hd = t.HDisplay / 8 - 1, hbs = t.HDisplay / 8 - 1, hbe = t.HTotal / 8 - 1;
    ULONG hrs = t.HSyncStart / 8, hre = t.HSyncEnd / 8;
    ULONG vt = t.VTotal - 2, vd = t.VDisplay - 1, vrs = t.VSyncStart, vre = t.VSyncEnd, vbs = t.VDisplay - 1,
          vbe = t.VTotal - 1, lc = 0x3FF;
    AvpCrtc(regs, 0x11, 0x00); // unlock CR0-7
    AvpCrtc(regs, 0x00, (UCHAR)ht);
    AvpCrtc(regs, 0x01, (UCHAR)hd);
    AvpCrtc(regs, 0x02, (UCHAR)hbs);
    AvpCrtc(regs, 0x03, (UCHAR)(0x80 | (hbe & 0x1F)));
    AvpCrtc(regs, 0x04, (UCHAR)hrs);
    AvpCrtc(regs, 0x05, (UCHAR)(((hbe & 0x20) << 2) | (hre & 0x1F)));
    AvpCrtc(regs, 0x06, (UCHAR)vt);
    AvpCrtc(regs, 0x07, (UCHAR)(((vt >> 8) & 1) | ((vd >> 7) & 2) | ((vrs >> 6) & 4) | ((vbs >> 5) & 8) |
                                ((lc >> 4) & 0x10) | ((vt >> 4) & 0x20) | ((vd >> 3) & 0x40) | ((vrs >> 2) & 0x80)));
    AvpCrtc(regs, 0x08, 0x00);
    AvpCrtc(regs, 0x09, (UCHAR)(((vbs >> 4) & 0x20) | ((lc >> 3) & 0x40)));
    AvpCrtc(regs, 0x10, (UCHAR)vrs);
    AvpCrtc(regs, 0x11, (UCHAR)(vre & 0x0F));
    AvpCrtc(regs, 0x12, (UCHAR)vd);
    AvpCrtc(regs, 0x14, 0x00);
    AvpCrtc(regs, 0x15, (UCHAR)vbs);
    AvpCrtc(regs, 0x16, (UCHAR)vbe);
    AvpCrtc(regs, 0x17, 0xE3);
    AvpCrtc(regs, 0x18, (UCHAR)lc); // line compare: never
    AvpCr25 = (UCHAR)(((vt >> 10) & 1) | ((vd >> 9) & 2) | ((vrs >> 8) & 4) | ((vbs >> 7) & 8) | ((hbe >> 2) & 0x10));
    AvpCrtc(regs, 0x2D, (UCHAR)(((ht >> 8) & 1) | ((hd >> 7) & 2) | ((hbs >> 6) & 4) | ((hrs >> 5) & 8)));
    AvpCrtc(regs, 0x39, t.Interlaced ? (UCHAR)(t.HTotal / 16 - 5) : 0xFF);
}

// Standard VGA graphics-mode state (spec/17 §5 step 9): sequencer, graphics controller, attribute controller with an
// identity palette, then attribute index bit 5 (palette address source = display enabled). Without that last bit a
// VGA (and xemu's display) shows a blank screen.
static void AvpVgaGraphicsMode(volatile UCHAR *regs) {
    static const UCHAR seq[] = {0x03, 0x01, 0x0F, 0x00, 0x0E};
    static const UCHAR gfx[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF};
    for (UCHAR i = 0; i < sizeof(seq); i++) {
        regs[0xC03C4] = i;
        regs[0xC03C5] = seq[i];
    }
    for (UCHAR i = 0; i < sizeof(gfx); i++) {
        regs[0xC03CE] = i;
        regs[0xC03CF] = gfx[i];
    }
    (void)regs[0x6013DA]; // reset the attribute controller's index/data flip-flop
    for (UCHAR i = 0; i < 0x15; i++) {
        regs[0x6013C0] = i;
        regs[0x6013C0] = i < 0x10 ? i : i == 0x10 ? 0x41 : i == 0x12 ? 0x0F : 0x00;
    }
    regs[0x6013C0] = 0x20;
}

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
        AvpProgramTiming(regs, *AvpFindTiming(Mode));
        // GPIOs: 0x80D6 bit 0 is the AV connector's widescreen line (mode bit 28; xemu's "Auto" aspect follows it).
        outb(0x80D6, (UCHAR)(((Mode >> 28) & 1) | 4));
        outb(0x80D8, 4);
        regs[0xC03C3] = 0x01;
        AvpVgaGraphicsMode(regs);
        regs[0xC03C2] = 0xE3; // misc output: 25 MHz clock, negative syncs
        AvpCurrentMode = Mode;
    }
    *(volatile ULONG *)(regs + 0x680600) = generalControl;
    AvpCrtc(regs, 0x13, (UCHAR)pitch8);
    AvpCrtc(regs, 0x19, (UCHAR)((pitch8 >> 3) & 0xE0));
    AvpCrtc(regs, 0x25, (UCHAR)(AvpCr25 | ((pitch8 >> 6) & 0x20)));
    AvpCrtc(regs, 0x28, (UCHAR)((depth >> 24) | 0x80));
    *(volatile ULONG *)(regs + 0x600800) = FrameBuffer;
    return 0;
}
