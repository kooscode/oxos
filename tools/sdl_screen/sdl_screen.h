// Text screen and controllers for OXOS's nxdk apps, on SDL3 (Ryzee119's nxdk-sdl3, fetched into
// third_party/nxdk-sdl3 by `make -C kernel nxdk`): a grid of text drawn by the GPU with SDL's debug font (8x8 pixels,
// drawn twice as tall: 8x16 characters as in a VGA text mode) inside a margin a TV does not crop, an optional picture
// under it, the buttons of every connected controller, and the shutdown an app needs before it starts a title.
//
// An app's Makefile adds sdl_screen.cpp to SRCS and includes sdl_screen.mk after nxdk's Makefile.
#pragma once

#include <SDL3/SDL.h>

namespace sdl_screen {

// The screen: 640x480, text in ROWS x COLUMNS characters inside MARGIN pixels on every side.
constexpr int WIDTH = 640;
constexpr int HEIGHT = 480;
constexpr int MARGIN = 24;
constexpr int CHAR_WIDTH = 8;
constexpr int CHAR_HEIGHT = 16;
constexpr int COLUMNS = (WIDTH - 2 * MARGIN) / CHAR_WIDTH;
constexpr int ROWS = (HEIGHT - 2 * MARGIN) / CHAR_HEIGHT;

// --- Start and stop

// Starts SDL, sets the video mode and creates the GPU renderer; with gamepads also the controllers (the USB host).
// False when SDL cannot start; SDL_GetError says why. The first Open empties the text grid.
bool Open(bool gamepads);
bool IsOpen();

// Stops SDL: the renderer (pbkit) and, if it was started, the USB host. Call it before XLaunchXBE or a quick reboot:
// the USB host must stop its DMA before the title gets the memory it writes to. The text stays (Print still works),
// and Open shows it again.
void Close();

// --- Text (any thread)

// Empties the grid and puts the cursor at the top left.
void Clear();

// Writes at the cursor, like printf. A '\n' ends the row (the rest of it is cleared) and moves to the next; text
// past the last row scrolls the grid up one row.
void Print(const char *format, ...) SDL_PRINTF_VARARG_FUNC(1);

// Moves the cursor (row 0 is the top row).
void SetCursor(int row, int column = 0);

// Draws a bar behind one row (a selection); -1 for none.
void SetHighlight(int row);

// --- Drawing (the thread that called Open)

SDL_Renderer *Renderer();

// A picture drawn under the text by every Present, at where (screen pixels); nullptr for none. The app keeps the
// texture alive while it is set.
void SetPicture(SDL_Texture *picture, const SDL_FRect &where);

// Draws the picture, the highlight and the text, and shows them at the next vertical blank.
void Present();

// Keeps showing the screen, for an app that stops on a message (Present every frame; a sleep when SDL did not start).
[[noreturn]] void ShowForever();

// --- Controllers (the thread that called Open)

// Reads SDL's events: opens controllers as they connect, and updates Held and Pressed. Call it once a frame. Every
// event is also passed to onEvent when one is given.
void Poll(void (*onEvent)(const SDL_Event &event) = nullptr);

// True while the button is down on any controller.
bool Held(SDL_GamepadButton button);

// True when the button went down on any controller between the last two Polls (once per press).
bool Pressed(SDL_GamepadButton button);

} // namespace sdl_screen
