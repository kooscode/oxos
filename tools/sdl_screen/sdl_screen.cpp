// Text screen and controllers for OXOS's nxdk apps, on SDL3 (sdl_screen.h).
#include "sdl_screen.h"

#include <usbh_lib.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace sdl_screen {

namespace {

// --- State

SDL_Window *ScreenWindow;
SDL_Renderer *ScreenRenderer;
bool ScreenGamepads; // Open started the controllers (and with them the USB host)

// The text and the cursor, shared by every thread that prints; ScreenLock guards them.
SDL_Mutex *ScreenLock;
char ScreenText[ROWS][COLUMNS];
int ScreenRow;
int ScreenColumn;
int ScreenHighlight = -1;

SDL_Texture *ScreenPicture;
SDL_FRect ScreenPictureWhere;

// The buttons down on any controller, at the last Poll and at the one before (bit n: SDL_GamepadButton n).
Uint32 ButtonsNow;
Uint32 ButtonsBefore;

// --- Text

class TextLock {
public:
    TextLock() {
        if (ScreenLock)
            SDL_LockMutex(ScreenLock);
    }
    ~TextLock() {
        if (ScreenLock)
            SDL_UnlockMutex(ScreenLock);
    }
};

void ClearRow(int row, int from) {
    std::memset(&ScreenText[row][from], ' ', COLUMNS - from);
}

// Moves the grid up one row (the top row is lost) for text past the last row.
void Scroll() {
    std::memmove(ScreenText[0], ScreenText[1], sizeof(ScreenText) - sizeof(ScreenText[0]));
    ClearRow(ROWS - 1, 0);
    ScreenRow = ROWS - 1;
}

// A '\n' ends the row and moves the cursor below it; the grid scrolls only when something is written past the last
// row, so a '\n' at the end of the last row keeps the screen still.
void Put(char c) {
    if (ScreenRow == ROWS)
        Scroll();
    if (c == '\n') {
        ClearRow(ScreenRow, ScreenColumn);
        ScreenRow++;
        ScreenColumn = 0;
        return;
    }
    if (ScreenColumn == COLUMNS) {
        ScreenRow++;
        ScreenColumn = 0;
        if (ScreenRow == ROWS)
            Scroll();
    }
    ScreenText[ScreenRow][ScreenColumn++] = c;
}

// --- Drawing

void DrawText() {
    // SDL's debug font is 8x8; drawn at twice the height it gives 8x16 characters.
    constexpr float TEXT_SCALE_Y = static_cast<float>(CHAR_HEIGHT) / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    SDL_SetRenderScale(ScreenRenderer, 1.0f, TEXT_SCALE_Y);
    SDL_SetRenderDrawColor(ScreenRenderer, 0xFF, 0xFF, 0xFF, 0xFF);
    char row[COLUMNS + 1];
    TextLock lock;
    for (int r = 0; r < ROWS; r++) {
        int length = COLUMNS;
        while (length > 0 && ScreenText[r][length - 1] == ' ')
            length--;
        if (!length)
            continue;
        std::memcpy(row, ScreenText[r], length);
        row[length] = 0;
        SDL_RenderDebugText(ScreenRenderer, MARGIN, (MARGIN + r * CHAR_HEIGHT) / TEXT_SCALE_Y, row);
    }
    SDL_SetRenderScale(ScreenRenderer, 1.0f, 1.0f);
}

// --- Controllers

void OpenGamepad(SDL_JoystickID id) {
    if (!SDL_GetGamepadFromID(id))
        SDL_OpenGamepad(id);
}

Uint32 ButtonsDown() {
    Uint32 down = 0;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; i++) {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad)
            continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++) {
            if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b)))
                down |= 1u << b;
        }
    }
    SDL_free(ids);
    return down;
}

} // namespace

// --- Start and stop

bool Open(bool gamepads) {
    if (ScreenRenderer)
        return true;
    SDL_InitFlags flags = SDL_INIT_VIDEO | (gamepads ? SDL_INIT_GAMEPAD : 0);
    if (!SDL_Init(flags))
        return false;
    ScreenGamepads = gamepads;
    if (!SDL_CreateWindowAndRenderer("OXOS", WIDTH, HEIGHT, 0, &ScreenWindow, &ScreenRenderer)) {
        Close();
        return false;
    }
    SDL_SetRenderVSync(ScreenRenderer, 1);
    // The lock and the text outlive Close, so other threads can keep printing and an app that opens the screen again
    // (after a failed launch) still shows what it printed.
    if (!ScreenLock) {
        ScreenLock = SDL_CreateMutex();
        Clear();
    }
    return true;
}

bool IsOpen() {
    return ScreenRenderer != nullptr;
}

void Close() {
    ScreenPicture = nullptr;
    ScreenRenderer = nullptr;
    ScreenWindow = nullptr;
    // SDL_Quit destroys the renderer (pb_kill) and the window; SDL leaves the USB host running (other code might use
    // it), so it is stopped here.
    SDL_Quit();
    if (ScreenGamepads)
        usbh_core_deinit();
    ScreenGamepads = false;
}

// --- Text

void Clear() {
    TextLock lock;
    std::memset(ScreenText, ' ', sizeof(ScreenText));
    ScreenRow = 0;
    ScreenColumn = 0;
}

void Print(const char *format, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    TextLock lock;
    for (const char *c = text; *c; c++)
        Put(*c);
}

void SetCursor(int row, int column) {
    TextLock lock;
    ScreenRow = SDL_clamp(row, 0, ROWS - 1);
    ScreenColumn = SDL_clamp(column, 0, COLUMNS);
}

void SetHighlight(int row) {
    TextLock lock;
    ScreenHighlight = row;
}

// --- Drawing

SDL_Renderer *Renderer() {
    return ScreenRenderer;
}

void SetPicture(SDL_Texture *picture, const SDL_FRect &where) {
    ScreenPicture = picture;
    ScreenPictureWhere = where;
}

void Present() {
    if (!ScreenRenderer)
        return;
    SDL_SetRenderDrawColor(ScreenRenderer, 0, 0, 0, 0xFF);
    SDL_RenderClear(ScreenRenderer);
    if (ScreenPicture)
        SDL_RenderTexture(ScreenRenderer, ScreenPicture, nullptr, &ScreenPictureWhere);
    if (ScreenHighlight >= 0 && ScreenHighlight < ROWS) {
        const SDL_FRect bar{MARGIN - 4.0f, static_cast<float>(MARGIN + ScreenHighlight * CHAR_HEIGHT),
                            COLUMNS * CHAR_WIDTH + 8.0f, static_cast<float>(CHAR_HEIGHT)};
        SDL_SetRenderDrawColor(ScreenRenderer, 0x20, 0x50, 0x20, 0xFF);
        SDL_RenderFillRect(ScreenRenderer, &bar);
    }
    DrawText();
    SDL_RenderPresent(ScreenRenderer);
}

void ShowForever() {
    constexpr Uint32 IDLE_MS = 1000;
    for (;;) {
        if (ScreenRenderer)
            Present();
        else
            SDL_Delay(IDLE_MS);
    }
}

// --- Controllers

void Poll(void (*onEvent)(const SDL_Event &event)) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_GAMEPAD_ADDED)
            OpenGamepad(event.gdevice.which);
        if (onEvent)
            onEvent(event);
    }
    ButtonsBefore = ButtonsNow;
    ButtonsNow = ScreenGamepads ? ButtonsDown() : 0;
}

bool Held(SDL_GamepadButton button) {
    return ButtonsNow & (1u << button);
}

bool Pressed(SDL_GamepadButton button) {
    return (ButtonsNow & ~ButtonsBefore) & (1u << button);
}

} // namespace sdl_screen
