// oxos_boot_video.xbe: the boot video (spec/oxos_internal/38). The kernel starts it in place of the first title after a cold boot, and with
// boot_video_always = yes after every quick reboot that would start the dashboard or the disc (kernel/src/titles/title_loader.cpp, StartBootVideo). It plays
// the .mpg that boot_video_file in C:\OXOS\oxos_settings.ini names (shared/video_player) until it ends or a button is pressed on any controller, then
// quick-reboots without touching the launch data page: the kernel then starts the title it ran in place of. Started from a dashboard it does the same, and
// the dashboard comes back. A missing or unplayable file is skipped; what happened goes to the debug log (DbgPrint: the serial port of a SERIAL=1 kernel).
// From its start until the video's first frame the screen shows the boot splash's last picture (kernel/splash/oxos_splash.svg, drawn by the kernel's vector
// renderer), so the splash carries on into the video instead of a black gap.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <usbh_lib.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <graphics/vector_renderer.h>
#include <ini_file/settings_ini.h>
#include <video_player/video_player.h>

#include "oxos_splash.h"

namespace {

constexpr char SETTINGS_FILE[] = "C:\\OXOS\\oxos_settings.ini";
constexpr char VIDEOS_FOLDER[] = "C:\\OXOS\\boot_videos\\";
constexpr long MAX_SETTINGS_BYTES = 0x10000;
// The splash at least this long before the video: a TV or an HDMI adapter takes a moment to lock onto the player's new picture (user, 2026-11-17: "about
// 500 ms").
constexpr uint32_t SPLASH_LEAD_IN_MS = 500;

// The hard disk's drives a boot_video_file may name, and their partitions.
constexpr char DRIVES[] = "CEFGXYZ";
constexpr char PARTITIONS[] = "2167345";

// SDL's controllers are open (the USB host runs).
bool GamepadsOpen;

void Log(const char *Format, const char *Text) {
    char line[400];
    snprintf(line, sizeof(line), Format, Text);
    DbgPrint("boot video: %s\n", line);
}

// Shows the boot splash's last picture, as the kernel's splash ends (without its File Manager line). Nothing when the scene does not check.
void ShowSplash() {
    namespace vr = oxos::graphics::vector_renderer;
    static_assert(vr::W == oxos::video_player::SCREEN_WIDTH && vr::H == oxos::video_player::SCREEN_HEIGHT);
    unsigned scratchBytes = 0;
    if (vr::Check(OXOS_SPLASH_SCENE, sizeof(OXOS_SPLASH_SCENE), &scratchBytes)) {
        return;
    }
    std::vector<unsigned> scratch(scratchBytes / 4 + 1);
    std::vector<uint32_t> picture(vr::W * vr::H);
    vr::Render(OXOS_SPLASH_SCENE, sizeof(OXOS_SPLASH_SCENE), vr::END, picture.data(), scratch.data());
    oxos::video_player::ShowStillPicture(picture.data());
}

void MountDrives() {
    for (int i = 0; DRIVES[i]; i++) {
        char device[40];
        snprintf(device, sizeof(device), "\\Device\\Harddisk0\\Partition%c\\", PARTITIONS[i]);
        nxMountDrive(DRIVES[i], device);
    }
}

// The video file oxos_settings.ini names, as a path to open: a name in C:\OXOS\boot_videos\, or a full path with a drive letter; '/' becomes '\'. Empty when
// the file or the key is missing.
std::string FindVideoFile() {
    FILE *file = fopen(SETTINGS_FILE, "rb");
    if (!file) {
        return "";
    }
    static char text[MAX_SETTINGS_BYTES];
    size_t got = fread(text, 1, sizeof(text), file);
    fclose(file);
    BOOT_VIDEO_SETTINGS settings;
    uint32_t errors;
    oxos::ini_file::ReadBootVideoSettings(text, static_cast<uint32_t>(got), &settings, &errors);
    std::string path = settings.File;
    for (char &c : path) {
        if (c == '/') {
            c = '\\';
        }
    }
    if (path.empty() || (path.size() >= 2 && path[1] == ':')) {
        return path;
    }
    return VIDEOS_FOLDER + path;
}

// The player's stop check: true when a button went down on any controller since the last check (controllers are opened as they connect).
bool IsButtonPressed(void *) {
    bool pressed = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_GAMEPAD_ADDED && !SDL_GetGamepadFromID(event.gdevice.which)) {
            SDL_OpenGamepad(event.gdevice.which);
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            pressed = true;
        }
    }
    return pressed;
}

// Stops SDL and the USB host (its DMA must stop before the next title gets the memory), then quick-reboots; the launch data page is left as it is.
[[noreturn]] void StartNextTitle() {
    if (GamepadsOpen) {
        SDL_Quit();
        usbh_core_deinit();
    }
    HalReturnToFirmware(HalQuickRebootRoutine);
    for (;;) {
        Sleep(1000);
    }
}

} // namespace

int main() {
    ShowSplash();
    MountDrives();
    std::string video = FindVideoFile();
    if (video.empty()) {
        Log("%s names no boot_video_file", SETTINGS_FILE);
        StartNextTitle();
    }
    GamepadsOpen = SDL_Init(SDL_INIT_GAMEPAD);
    oxos::video_player::PLAY_RESULT result = oxos::video_player::PlayVideoFile(video.c_str(), SPLASH_LEAD_IN_MS, IsButtonPressed, nullptr);
    char line[400];
    snprintf(line, sizeof(line), "%s: %s", video.c_str(), oxos::video_player::DescribePlayResult(result));
    Log("%s", line);
    StartNextTitle();
}
