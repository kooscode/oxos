// Controller test title (nxdk + SDL3, open source): shows every Xbox controller input live, with a checklist of the
// inputs seen so far, and keeps the same state in XkControllerTest for tests/controller_test.py (found in RAM by its
// signature). The controllers are read through SDL's gamepad API (nxdk-sdl3's Xbox driver on the kernel's USB host);
// each slot keeps the last state as an Xbox controller input report (xid_gamepad_in), the layout the test decodes.
#include <windows.h>

#include <array>
#include <cstring>

#include "sdl_screen.h"

namespace {

constexpr int SLOTS = 4;

struct PadSlot {
    unsigned int Present, Uid, Type, Reports; // Uid: SDL's joystick ID; Reports: frames read while connected
    unsigned char Last[20];       // the last state, as an input report (xid_gamepad_in)
    unsigned short DigitalSeen;   // every digital button bit seen set
    unsigned char AnalogMax[8];   // A B X Y Black White LT RT: highest value seen
    short AxisMin[4], AxisMax[4]; // LX LY RX RY
};

} // namespace

// The state, for tests/controller_test.py. The layout is fixed (the test reads it).
volatile struct {
    char Magic[8]; // "XKCTLTST"
    unsigned int Version, Self, Connects, Disconnects, Frames; // Self: this block's virtual address
    PadSlot Slot[SLOTS];
} XkControllerTest = {{'X', 'K', 'C', 'T', 'L', 'T', 'S', 'T'}, 1};

namespace {

// --- The input report

// The report's fixed header (bReportId 0, bLength 20) and its type for a game controller (XID_TYPE_GAMECONTROLLER in
// the high byte, as nxdk's usbh_xid_get_type gives it).
constexpr unsigned char REPORT_LENGTH = 20;
constexpr unsigned int GAME_CONTROLLER_TYPE = 0x0100;
constexpr unsigned char ANALOG_DOWN = 0xFF;

// The report's digital buttons (dButtons, bits 0-7) and analog buttons (bytes 4-9), as SDL names them. SDL maps the
// controller's Black button to the right shoulder and White to the left one.
constexpr std::array<SDL_GamepadButton, 8> DIGITAL_BUTTONS{
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK};
constexpr std::array<SDL_GamepadButton, 6> ANALOG_BUTTONS{
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};

// SDL's trigger axis (0..32767) as the report's 0..255.
unsigned char TriggerByte(Sint16 axis) {
    return static_cast<unsigned char>(SDL_max(axis, 0) * 255 / SDL_JOYSTICK_AXIS_MAX);
}

// SDL's Y axes grow downwards, the report's upwards (the driver flips them with ~, so ~ flips them back exactly).
short ReportY(Sint16 axis) {
    return static_cast<short>(~axis);
}

// The gamepad's state now, as an Xbox controller input report.
void FillReport(SDL_Gamepad *pad, unsigned char (&report)[REPORT_LENGTH]) {
    std::memset(report, 0, sizeof(report));
    report[1] = REPORT_LENGTH;
    unsigned short buttons = 0;
    for (std::size_t i = 0; i < DIGITAL_BUTTONS.size(); i++) {
        if (SDL_GetGamepadButton(pad, DIGITAL_BUTTONS[i]))
            buttons |= 1 << i;
    }
    std::memcpy(&report[2], &buttons, sizeof(buttons));
    for (std::size_t i = 0; i < ANALOG_BUTTONS.size(); i++)
        report[4 + i] = SDL_GetGamepadButton(pad, ANALOG_BUTTONS[i]) ? ANALOG_DOWN : 0;
    report[10] = TriggerByte(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    report[11] = TriggerByte(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    const std::array<short, 4> sticks{SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
                                      ReportY(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)),
                                      SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX),
                                      ReportY(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY))};
    std::memcpy(&report[12], sticks.data(), sizeof(sticks));
}

// --- The slots

int SlotOf(SDL_JoystickID id) {
    for (int i = 0; i < SLOTS; i++) {
        if (XkControllerTest.Slot[i].Present && XkControllerTest.Slot[i].Uid == id)
            return i;
    }
    return -1;
}

void Connected(SDL_JoystickID id) {
    XkControllerTest.Connects = XkControllerTest.Connects + 1;
    for (int i = 0; i < SLOTS; i++) {
        volatile PadSlot &p = XkControllerTest.Slot[i];
        if (p.Present)
            continue;
        std::memset(const_cast<PadSlot *>(&p), 0, sizeof(PadSlot));
        p.Uid = id;
        p.Type = GAME_CONTROLLER_TYPE;
        p.Present = 1;
        return;
    }
}

void Disconnected(SDL_JoystickID id) {
    XkControllerTest.Disconnects = XkControllerTest.Disconnects + 1;
    int s = SlotOf(id);
    if (s >= 0)
        XkControllerTest.Slot[s].Present = 0;
}

void OnEvent(const SDL_Event &event) {
    if (event.type == SDL_EVENT_GAMEPAD_ADDED)
        Connected(event.gdevice.which);
    else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
        Disconnected(event.gdevice.which);
}

// Reads every connected slot's gamepad into its report, and adds to what the slot has seen.
void ReadSlots() {
    for (int s = 0; s < SLOTS; s++) {
        volatile PadSlot &p = XkControllerTest.Slot[s];
        SDL_Gamepad *pad = p.Present ? SDL_GetGamepadFromID(p.Uid) : nullptr;
        if (!pad)
            continue;
        unsigned char report[REPORT_LENGTH];
        FillReport(pad, report);
        for (int i = 0; i < REPORT_LENGTH; i++)
            p.Last[i] = report[i];
        unsigned short buttons;
        std::memcpy(&buttons, &report[2], sizeof(buttons));
        p.DigitalSeen = p.DigitalSeen | buttons;
        for (int i = 0; i < 8; i++) {
            if (report[4 + i] > p.AnalogMax[i])
                p.AnalogMax[i] = report[4 + i];
        }
        std::array<short, 4> axis;
        std::memcpy(axis.data(), &report[12], sizeof(axis));
        for (int i = 0; i < 4; i++) {
            if (axis[i] < p.AxisMin[i])
                p.AxisMin[i] = axis[i];
            if (axis[i] > p.AxisMax[i])
                p.AxisMax[i] = axis[i];
        }
        p.Reports = p.Reports + 1;
    }
}

// --- The screen

// Short enough for the "seen" row to fit the screen's width; U D L R are the D-pad.
constexpr std::array<const char *, 8> DIGITAL_NAMES{"U", "D", "L", "R", "START", "BACK", "LS", "RS"};
constexpr std::array<const char *, 8> ANALOG_NAMES{"A", "B", "X", "Y", "BLK", "WHT", "LT", "RT"};
constexpr short STICK_REACHED = 16000; // a stick counts as pushed to a side past this

const char *Mark(bool reached, const char *mark) {
    return reached ? mark : ".";
}

void Show() {
    using sdl_screen::Print;
    sdl_screen::SetCursor(0);
    Print("OXOS controller test                     frame %u\n\n", XkControllerTest.Frames);
    for (int s = 0; s < SLOTS; s++) {
        const volatile PadSlot &p = XkControllerTest.Slot[s];
        if (!p.Present) {
            Print("Controller %d: -\n\n\n\n\n", s + 1);
            continue;
        }
        unsigned char report[REPORT_LENGTH];
        for (int i = 0; i < REPORT_LENGTH; i++)
            report[i] = p.Last[i];
        unsigned short buttons;
        std::memcpy(&buttons, &report[2], sizeof(buttons));
        std::array<short, 4> axis;
        std::memcpy(axis.data(), &report[12], sizeof(axis));
        Print("Controller %d: type %04x, %u reads\n", s + 1, p.Type, p.Reports);
        Print("  now:");
        for (int i = 0; i < 8; i++)
            Print(" %s:%-3u", ANALOG_NAMES[i], static_cast<unsigned int>(report[4 + i]));
        Print("\n  now: buttons %04x  L(%6d,%6d) R(%6d,%6d)\n", buttons, axis[0], axis[1], axis[2], axis[3]);
        Print("  seen:");
        for (int i = 0; i < 8; i++)
            Print(" %s%s", ANALOG_NAMES[i], p.AnalogMax[i] >= 0x80 ? "+" : "-");
        for (int i = 0; i < 8; i++)
            Print(" %s%s", DIGITAL_NAMES[i], p.DigitalSeen & (1 << i) ? "+" : "-");
        Print("\n  seen: sticks L%s%s%s%s R%s%s%s%s (+ = reached, - = not yet)\n\n",
              Mark(p.AxisMin[0] < -STICK_REACHED, "<"), Mark(p.AxisMax[0] > STICK_REACHED, ">"),
              Mark(p.AxisMax[1] > STICK_REACHED, "^"), Mark(p.AxisMin[1] < -STICK_REACHED, "v"),
              Mark(p.AxisMin[2] < -STICK_REACHED, "<"), Mark(p.AxisMax[2] > STICK_REACHED, ">"),
              Mark(p.AxisMax[3] > STICK_REACHED, "^"), Mark(p.AxisMin[3] < -STICK_REACHED, "v"));
    }
}

} // namespace

int main() {
    XkControllerTest.Self = reinterpret_cast<unsigned int>(&XkControllerTest);
    if (!sdl_screen::Open(true)) {
        DbgPrint("controller test: SDL: %s\n", SDL_GetError());
        return 1;
    }
    for (;;) {
        sdl_screen::Poll(OnEvent);
        ReadSlots();
        XkControllerTest.Frames = XkControllerTest.Frames + 1;
        Show();
        sdl_screen::Present();
    }
}
