// oxos_trainers.xbe: Universal Trainers' menu (nxdk + SDL3, spec/oxos_internal/37 §5). oxos_trainers.xos starts it in place of a game that has a trainer
// (oxos_title_redirect), with the launch data page the game was started with. It shows the game (its title image and name), its trainer, and a switch for the
// trainer and for each cheat, with the buttons of the cheats switched during the game (their "N keys" lines). Start Game is selected at the start, so A starts
// the game with the settings as they are. Changed switches are written to the game's section of C:\OXOS\extensions\oxos_trainers.ini first; then the page is
// put back and the game started through it, a launch oxos_trainers.xos lets through, and the trainer runs as the section says.
//
// The logo is oxos_trainers_logo.svg, drawn by the kernel's vector renderer (kernel/src/graphics/vector_renderer.cpp); everything else is drawn with SDL's
// renderer every frame, text in SDL's debug font.
#include <hal/xbox.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "../trainer_file.h"
#include "oxos_trainers_logo.h" // generated from oxos_trainers_logo.svg (Makefile)
#include "sdl_screen.h"
#include <graphics/vector_renderer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int WIDTH = sdl_screen::WIDTH;
constexpr int HEIGHT = sdl_screen::HEIGHT;

// --- Files: \Device\... paths

constexpr char INI_PATH[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\extensions\\oxos_trainers.ini";
constexpr char TRAINERS_FOLDER[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\trainers\\";
constexpr char SAVED_GAMES_FOLDER[] = "\\Device\\Harddisk0\\Partition1\\UDATA\\"; // E:\UDATA\<title ID>\TitleImage.xbx: the title image a game's saves keep
constexpr ULONG MAX_FILE_BYTES = 0x400000;                                        // the largest file read whole (the ini is about 200 KB)

NTSTATUS OpenFile(const char *path, ACCESS_MASK access, ULONG disposition, HANDLE &file) {
    ANSI_STRING name;
    RtlInitAnsiString(&name, path);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    IO_STATUS_BLOCK iosb;
    return NtCreateFile(&file, access | SYNCHRONIZE, &attributes, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, disposition,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
}

// size bytes at offset of an open file: true when all of them were read.
bool ReadAt(HANDLE file, ULONG offset, void *buffer, ULONG size) {
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER at;
    at.QuadPart = offset;
    return NT_SUCCESS(NtReadFile(file, nullptr, nullptr, nullptr, &iosb, buffer, size, &at)) && iosb.Information == size;
}

// The whole file (at most MAX_FILE_BYTES).
bool ReadWholeFile(const char *path, std::string &data) {
    HANDLE file;
    if (!NT_SUCCESS(OpenFile(path, GENERIC_READ, FILE_OPEN, file)))
        return false;
    FILE_STANDARD_INFORMATION info;
    IO_STATUS_BLOCK iosb;
    bool ok = NT_SUCCESS(NtQueryInformationFile(file, &iosb, &info, sizeof(info), FileStandardInformation)) && info.EndOfFile.QuadPart <= MAX_FILE_BYTES;
    if (ok) {
        data.resize(static_cast<std::size_t>(info.EndOfFile.QuadPart));
        ok = data.empty() || ReadAt(file, 0, data.data(), data.size());
    }
    NtClose(file);
    return ok;
}

// Replaces the file with data.
bool WriteWholeFile(const char *path, const std::string &data) {
    HANDLE file;
    if (!NT_SUCCESS(OpenFile(path, GENERIC_WRITE, FILE_OVERWRITE_IF, file)))
        return false;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER at;
    at.QuadPart = 0;
    bool ok = NT_SUCCESS(NtWriteFile(file, nullptr, nullptr, nullptr, &iosb, const_cast<char *>(data.data()), data.size(), &at)) &&
              iosb.Information == data.size();
    NtClose(file);
    return ok;
}

// The dword at offset of data, or 0 past its end.
ULONG ReadDword(const std::string &data, ULONG offset) {
    if (offset > data.size() || data.size() - offset < 4)
        return 0;
    ULONG value;
    std::memcpy(&value, data.data() + offset, 4);
    return value;
}

// Text the debug font can draw: printable ASCII, anything else '?'.
std::string MakePlainText(const char *text, std::size_t length) {
    std::string out;
    for (std::size_t i = 0; i < length && text[i]; i++)
        out += text[i] >= 0x20 && text[i] < 0x7F ? text[i] : '?';
    return out;
}

bool SameText(const std::string &a, const char *b) {
    return SDL_strcasecmp(a.c_str(), b) == 0;
}

// --- The game: the launch data page it was started with

constexpr char OWN_NAME[] = "\\oxos_trainers.xbe";

LAUNCH_DATA_PAGE SavedPage; // the page as the game's launcher wrote it, put back to start the game
char GamePath[sizeof(SavedPage.Header.szLaunchPath)];

// The game this menu was started for, from the launch data page's path ("\Device\...\folder;name.xbe"). Without a page, the disc: oxos_trainers.xos starts
// the menu in its place when the console starts it with no page (at power-on, a disc loaded in xemu), and the page made for it here has type 0xFFFFFFFF, which
// the kernel frees as it starts the disc, so the game still gets no launch data. False when the menu was started on its own (a page that names nothing,
// or the menu).
bool ReadLaunchPage() {
    if (!LaunchDataPage) {
        std::memset(&SavedPage, 0, sizeof(SavedPage));
        SavedPage.Header.dwLaunchDataType = 0xFFFFFFFF;
        std::snprintf(SavedPage.Header.szLaunchPath, sizeof(SavedPage.Header.szLaunchPath), "\\Device\\CdRom0;default.xbe");
    } else if (LaunchDataPage->Header.szLaunchPath[0]) {
        std::memcpy(&SavedPage, LaunchDataPage, sizeof(SavedPage));
    } else {
        return false;
    }
    std::snprintf(GamePath, sizeof(GamePath), "%s", SavedPage.Header.szLaunchPath);
    char *folderEnd = std::strchr(GamePath, ';');
    if (folderEnd)
        *folderEnd = '\\';
    std::size_t length = std::strlen(GamePath), own = std::strlen(OWN_NAME);
    return length < own || SDL_strcasecmp(GamePath + length - own, OWN_NAME) != 0; // a dashboard started the menu itself
}

// Starts the game as it was started before this menu: its launch data page as the launcher wrote it (the kernel's "already tried" flag cleared) and a quick
// reboot, as a dashboard starts a title. Without a game, the dashboard.
[[noreturn]] void StartGame(bool haveGame) {
    sdl_screen::Close();
    if (!haveGame) {
        XLaunchXBE(nullptr);
        HalReturnToFirmware(HalRebootRoutine);
    }
    if (!LaunchDataPage)
        LaunchDataPage = static_cast<PLAUNCH_DATA_PAGE>(MmAllocateContiguousMemory(sizeof(LAUNCH_DATA_PAGE)));
    std::memcpy(LaunchDataPage, &SavedPage, sizeof(SavedPage));
    LaunchDataPage->Header.dwFlags = 0;
    MmPersistContiguousMemory(LaunchDataPage, sizeof(LAUNCH_DATA_PAGE), TRUE);
    HalReturnToFirmware(HalQuickRebootRoutine);
    for (;;) {
        // the quick reboot does not return
    }
}

// --- The game's XBE: its title ID, name and title image

constexpr ULONG XBE_BASE_ADDRESS = 0x104, XBE_HEADERS_SIZE = 0x108, XBE_TIMESTAMP = 0x114, XBE_CERTIFICATE = 0x118;
constexpr ULONG XBE_SECTION_COUNT = 0x11C, XBE_SECTION_HEADERS = 0x120;
constexpr ULONG CERTIFICATE_TITLE_ID = 0x08, CERTIFICATE_TITLE_NAME = 0x0C, TITLE_NAME_CHARS = 40;
constexpr ULONG SECTION_HEADER_BYTES = 0x38, SECTION_RAW_ADDRESS = 0x0C, SECTION_RAW_SIZE = 0x10, SECTION_NAME = 0x14;
constexpr ULONG MAX_HEADER_BYTES = 0x10000; // the image header, read whole
constexpr ULONG MAX_IMAGE_BYTES = 0x40000;  // the largest title image read
constexpr char TITLE_IMAGE_SECTION[] = "$$XTIMAGE";

struct GAME {
    ULONG TitleId = 0;
    std::string Name;  // the certificate's title name
    std::string Image; // its title image (an XPR0 texture), or empty
    ULONG Timestamp = 0; // the image header's build timestamp: which build of the game this is (trainers are made for one)
};

bool ReadGame(const char *path, GAME &game) {
    HANDLE file;
    if (!NT_SUCCESS(OpenFile(path, GENERIC_READ, FILE_OPEN, file)))
        return false;
    std::string header(0x200, '\0');
    bool ok = ReadAt(file, 0, header.data(), header.size());
    ULONG size = std::min(ReadDword(header, XBE_HEADERS_SIZE), MAX_HEADER_BYTES);
    if (ok && size > header.size()) {
        header.resize(size);
        ok = ReadAt(file, 0, header.data(), size);
    }
    ULONG base = ReadDword(header, XBE_BASE_ADDRESS);
    ULONG certificate = ReadDword(header, XBE_CERTIFICATE) - base;
    if (ok) {
        game.TitleId = ReadDword(header, certificate + CERTIFICATE_TITLE_ID);
        game.Timestamp = ReadDword(header, XBE_TIMESTAMP);
        for (ULONG i = 0; i < TITLE_NAME_CHARS && certificate + CERTIFICATE_TITLE_NAME + 2 * i + 2 <= header.size(); i++) {
            auto c = static_cast<unsigned char>(header[certificate + CERTIFICATE_TITLE_NAME + 2 * i]);
            if (!c && !header[certificate + CERTIFICATE_TITLE_NAME + 2 * i + 1])
                break;
            game.Name += c >= 0x20 && c < 0x7F && !header[certificate + CERTIFICATE_TITLE_NAME + 2 * i + 1] ? static_cast<char>(c) : '?';
        }
        ULONG sections = ReadDword(header, XBE_SECTION_COUNT), headers = ReadDword(header, XBE_SECTION_HEADERS) - base;
        for (ULONG i = 0; i < sections && i < 256; i++) {
            ULONG at = headers + i * SECTION_HEADER_BYTES, name = ReadDword(header, at + SECTION_NAME) - base;
            if (name + sizeof(TITLE_IMAGE_SECTION) > header.size() || std::memcmp(header.data() + name, TITLE_IMAGE_SECTION, sizeof(TITLE_IMAGE_SECTION)))
                continue;
            ULONG raw = ReadDword(header, at + SECTION_RAW_ADDRESS), rawSize = ReadDword(header, at + SECTION_RAW_SIZE);
            if (rawSize && rawSize <= MAX_IMAGE_BYTES) {
                game.Image.resize(rawSize);
                if (!ReadAt(file, raw, game.Image.data(), rawSize))
                    game.Image.clear();
            }
        }
    }
    NtClose(file);
    return ok;
}

// The title image a game's saved games keep (E:\UDATA\<title ID>\TitleImage.xbx), for a game whose XBE has none.
void ReadSavedGamesImage(GAME &game) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s%08lx\\TitleImage.xbx", SAVED_GAMES_FOLDER, game.TitleId);
    if (!ReadWholeFile(path, game.Image) || game.Image.size() > MAX_IMAGE_BYTES)
        game.Image.clear();
}

// --- Title images: XPR0 textures (a header, then the texture), DXT-compressed or 32-bit, decoded to ARGB8888

constexpr ULONG XPR_MAGIC = 0x30525058; // "XPR0"
constexpr ULONG XPR_HEADER_SIZE = 0x08, XPR_TEXTURE_DATA = 0x10, XPR_TEXTURE_FORMAT = 0x18, XPR_TEXTURE_SIZE = 0x1C;
constexpr ULONG FORMAT_A8R8G8B8 = 0x06, FORMAT_X8R8G8B8 = 0x07, FORMAT_DXT1 = 0x0C, FORMAT_DXT3 = 0x0E, FORMAT_DXT5 = 0x0F; // swizzled or compressed
constexpr ULONG FORMAT_LINEAR_A8R8G8B8 = 0x12, FORMAT_LINEAR_X8R8G8B8 = 0x1E;

struct PICTURE {
    int Width = 0, Height = 0;
    std::vector<Uint32> Pixels; // ARGB8888, row by row
};

Uint32 Expand565(unsigned c) {
    unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

// a weighted mix of two ARGB colours, wa:wb
Uint32 MixColours(Uint32 a, Uint32 b, unsigned wa, unsigned wb) {
    Uint32 out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= ((((a >> shift) & 0xFF) * wa + ((b >> shift) & 0xFF) * wb) / (wa + wb)) << shift;
    return out;
}

// A pixel's place in a swizzled texture: the bits of x and y interleaved, x first, while both have bits left.
ULONG FindSwizzledIndex(ULONG x, ULONG y, ULONG width, ULONG height) {
    ULONG index = 0, bit = 1;
    for (ULONG mask = 1; mask < width || mask < height; mask <<= 1) {
        if (mask < width) {
            index |= x & mask ? bit : 0;
            bit <<= 1;
        }
        if (mask < height) {
            index |= y & mask ? bit : 0;
            bit <<= 1;
        }
    }
    return index;
}

// One 4x4 block of a DXT1/3/5 texture into the picture at (x, y).
void DecodeBlock(const unsigned char *block, ULONG format, PICTURE &picture, int x, int y) {
    const unsigned char *colour = format == FORMAT_DXT1 ? block : block + 8;
    unsigned c0 = colour[0] | colour[1] << 8, c1 = colour[2] | colour[3] << 8;
    Uint32 palette[4] = {Expand565(c0), Expand565(c1), 0, 0};
    if (format != FORMAT_DXT1 || c0 > c1) {
        palette[2] = MixColours(palette[0], palette[1], 2, 1);
        palette[3] = MixColours(palette[0], palette[1], 1, 2);
    } else {
        palette[2] = MixColours(palette[0], palette[1], 1, 1);
        palette[3] = 0; // transparent
    }
    ULONG indexes = colour[4] | colour[5] << 8 | colour[6] << 16 | static_cast<ULONG>(colour[7]) << 24;
    unsigned alphas[8] = {block[0], block[1]};
    if (format == FORMAT_DXT5) {
        // Eight levels between the two, or six and then 0 and 255.
        for (int i = 2; i < 8; i++) {
            if (alphas[0] > alphas[1])
                alphas[i] = ((8 - i) * alphas[0] + (i - 1) * alphas[1]) / 7;
            else
                alphas[i] = i < 6 ? ((6 - i) * alphas[0] + (i - 1) * alphas[1]) / 5 : i == 6 ? 0 : 255;
        }
    }
    unsigned long long alphaBits = 0;
    for (int i = 7; i >= 2; i--)
        alphaBits = alphaBits << 8 | block[i];
    for (int i = 0; i < 16; i++) {
        Uint32 pixel = palette[(indexes >> (2 * i)) & 3];
        if (format == FORMAT_DXT3) {
            unsigned a = (block[i / 2] >> (4 * (i & 1))) & 15;
            pixel = (pixel & 0xFFFFFF) | (a * 17) << 24;
        } else if (format == FORMAT_DXT5) {
            pixel = (pixel & 0xFFFFFF) | alphas[(alphaBits >> (3 * i)) & 7] << 24;
        }
        int px = x + (i & 3), py = y + i / 4;
        if (px < picture.Width && py < picture.Height)
            picture.Pixels[py * picture.Width + px] = pixel;
    }
}

// The title image's pixels: false for a texture this does not decode.
bool DecodeTitleImage(const std::string &xpr, PICTURE &picture) {
    if (ReadDword(xpr, 0) != XPR_MAGIC)
        return false;
    ULONG format = ReadDword(xpr, XPR_TEXTURE_FORMAT), kind = (format >> 8) & 0xFF;
    ULONG width = 1u << ((format >> 20) & 15), height = 1u << ((format >> 24) & 15), size = ReadDword(xpr, XPR_TEXTURE_SIZE);
    if (size) { // a linear texture of any size
        width = (size & 0xFFF) + 1;
        height = ((size >> 12) & 0xFFF) + 1;
    }
    ULONG data = ReadDword(xpr, XPR_HEADER_SIZE) + ReadDword(xpr, XPR_TEXTURE_DATA);
    bool dxt = kind == FORMAT_DXT1 || kind == FORMAT_DXT3 || kind == FORMAT_DXT5;
    bool argb = kind == FORMAT_A8R8G8B8 || kind == FORMAT_X8R8G8B8 || kind == FORMAT_LINEAR_A8R8G8B8 || kind == FORMAT_LINEAR_X8R8G8B8;
    ULONG bytes = dxt ? ((width + 3) / 4) * ((height + 3) / 4) * (kind == FORMAT_DXT1 ? 8 : 16) : width * height * 4;
    if ((!dxt && !argb) || width > 1024 || height > 1024 || data > xpr.size() || xpr.size() - data < bytes)
        return false;
    picture.Width = width;
    picture.Height = height;
    picture.Pixels.assign(width * height, 0);
    auto pixels = reinterpret_cast<const unsigned char *>(xpr.data() + data);
    if (dxt) {
        ULONG blockBytes = kind == FORMAT_DXT1 ? 8 : 16, across = (width + 3) / 4;
        for (ULONG by = 0; by < (height + 3) / 4; by++) {
            for (ULONG bx = 0; bx < across; bx++)
                DecodeBlock(pixels + (by * across + bx) * blockBytes, kind, picture, bx * 4, by * 4);
        }
        return true;
    }
    bool swizzled = kind == FORMAT_A8R8G8B8 || kind == FORMAT_X8R8G8B8, opaque = kind == FORMAT_X8R8G8B8 || kind == FORMAT_LINEAR_X8R8G8B8;
    for (ULONG y = 0; y < height; y++) {
        for (ULONG x = 0; x < width; x++) {
            ULONG at = swizzled ? FindSwizzledIndex(x, y, width, height) : y * width + x;
            Uint32 pixel;
            std::memcpy(&pixel, pixels + 4 * at, 4);
            picture.Pixels[y * width + x] = opaque ? pixel | 0xFF000000u : pixel;
        }
    }
    return true;
}

// --- The trainer file (.etm, trainer_file.h): its name, author and cheats

struct TRAINER {
    std::string Name, Author;
    std::vector<std::string> Cheats;
    std::vector<bool> Defaults; // each cheat's switch in the file
};

// A trainer from its file's bytes: false when they are not one.
bool ParseTrainer(const std::string &data, TRAINER &trainer) {
    if (data.size() < ETM_HEADER_BYTES || data.size() > ETM_MAX_BYTES)
        return false;
    std::vector<std::string> texts;
    for (ULONG at = ReadDword(data, ETM_TEXTS); texts.size() < ETM_MAX_CHEATS + 2 && ReadDword(data, at); at += 4) {
        ULONG text = ReadDword(data, at);
        if (text >= data.size())
            break;
        texts.push_back(MakePlainText(data.data() + text, data.size() - text));
    }
    if (texts.size() < 2)
        return false;
    trainer.Name = texts[0];
    trainer.Author = texts[1];
    ULONG switches = ReadDword(data, ETM_SWITCHES);
    for (std::size_t i = 2; i < texts.size(); i++) {
        trainer.Cheats.push_back(texts[i]);
        trainer.Defaults.push_back(switches + i - 2 < data.size() && data[switches + i - 2]);
    }
    return true;
}

bool ReadTrainer(const char *file, TRAINER &trainer) {
    std::string path = std::string(TRAINERS_FOLDER) + file, data;
    return ReadWholeFile(path.c_str(), data) && ParseTrainer(data, trainer);
}

// --- The game's section of oxos_trainers.ini: read, and written back with the switches changed

struct KEY_LINE {
    std::string Key;                    // the text before '=', trimmed
    std::string Value;                  // the text after it, trimmed
    std::size_t ValueStart, ValueBytes; // where the value is in the file
    std::size_t LineStart, LineEnd;     // the whole line, its line end included
};

std::string IniText;               // all of oxos_trainers.ini
std::vector<KEY_LINE> SectionKeys; // the game's section
std::size_t SectionEnd;            // after the section's last key line: where a missing key is added
std::string LineEnd = "\r\n";      // as the file has them

bool IsBlank(char c) {
    return c == ' ' || c == '\t';
}

// The game's section, [XXXXXXXX] (its title ID): false without one.
bool ReadSection(ULONG titleId) {
    if (!ReadWholeFile(INI_PATH, IniText))
        return false;
    if (IniText.find("\r\n") == std::string::npos)
        LineEnd = "\n";
    char name[12];
    std::snprintf(name, sizeof(name), "[%08lX]", titleId);
    bool inside = false, found = false;
    for (std::size_t line = 0; line < IniText.size();) {
        std::size_t end = IniText.find('\n', line);
        end = end == std::string::npos ? IniText.size() : end + 1;
        std::size_t start = line, stop = end;
        while (start < stop && IsBlank(IniText[start]))
            start++;
        while (stop > start && (IniText[stop - 1] == '\n' || IniText[stop - 1] == '\r' || IsBlank(IniText[stop - 1])))
            stop--;
        if (start < stop && IniText[start] == '[') {
            if (found)
                break;
            inside = SDL_strncasecmp(IniText.c_str() + start, name, std::strlen(name)) == 0;
            found = inside;
            if (inside)
                SectionEnd = end;
        } else if (inside && start < stop && IniText[start] != ';' && IniText[start] != '#') {
            std::size_t equals = IniText.find('=', start);
            if (equals != std::string::npos && equals < stop) {
                std::size_t keyEnd = equals, valueStart = equals + 1;
                while (keyEnd > start && IsBlank(IniText[keyEnd - 1]))
                    keyEnd--;
                while (valueStart < stop && IsBlank(IniText[valueStart]))
                    valueStart++;
                SectionKeys.push_back(
                    {IniText.substr(start, keyEnd - start), IniText.substr(valueStart, stop - valueStart), valueStart, stop - valueStart, line, end});
                SectionEnd = end;
            }
        }
        line = end;
    }
    return found;
}

// The number N of a cheat line "N. name", or 0 when the key is not one.
unsigned CheatNumber(const std::string &key) {
    if (key.empty() || key[0] < '0' || key[0] > '9')
        return 0;
    char *end;
    unsigned long number = std::strtoul(key.c_str(), &end, 10);
    return *end == '.' ? static_cast<unsigned>(number) : 0;
}

const KEY_LINE *FindKey(const char *key) {
    for (const KEY_LINE &line : SectionKeys) {
        if (SameText(line.Key, key))
            return &line;
    }
    return nullptr;
}

const KEY_LINE *FindCheat(unsigned number) {
    for (const KEY_LINE &line : SectionKeys) {
        if (CheatNumber(line.Key) == number)
            return &line;
    }
    return nullptr;
}

// The cheat number of a keys line "N keys", or 0 when the key is not one.
unsigned KeysCheatNumber(const std::string &key) {
    if (key.empty() || key[0] < '0' || key[0] > '9')
        return 0;
    char *end;
    unsigned long number = std::strtoul(key.c_str(), &end, 10);
    while (IsBlank(*end))
        end++;
    return SameText(end, "keys") ? static_cast<unsigned>(number) : 0;
}

// Text without the blanks at its ends.
std::string TrimText(const std::string &text) {
    std::size_t start = 0, stop = text.size();
    while (start < stop && IsBlank(text[start]))
        start++;
    while (stop > start && IsBlank(text[stop - 1]))
        stop--;
    return text.substr(start, stop - start);
}

// Text cut at each separator, each piece trimmed; empty pieces left out.
std::vector<std::string> SplitText(const std::string &text, char separator) {
    std::vector<std::string> pieces;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); i++) {
        if (i < text.size() && text[i] != separator)
            continue;
        std::string piece = TrimText(text.substr(start, i - start));
        if (!piece.empty())
            pieces.push_back(piece);
        start = i + 1;
    }
    return pieces;
}

// In-game keys as the ini writes them, "BUTTON + BUTTON, BUTTON + BUTTON": the buttons of each combination.
using KEYS = std::vector<std::vector<std::string>>;

KEYS ReadCombinations(const std::string &text) {
    KEYS combinations;
    for (const std::string &combination : SplitText(text, ','))
        combinations.push_back(SplitText(combination, '+'));
    return combinations;
}

// The in-game keys of each of cheats cheats, from the section's "N keys = ..." lines (none for a cheat without one).
std::vector<KEYS> ReadCheatKeys(std::size_t cheats) {
    std::vector<KEYS> keys(cheats);
    for (const KEY_LINE &line : SectionKeys) {
        unsigned number = KeysCheatNumber(line.Key);
        if (number >= 1 && number <= cheats)
            keys[number - 1] = ReadCombinations(line.Value);
    }
    return keys;
}

// Some cheat has in-game keys.
bool HaveAnyKeys(const std::vector<KEYS> &keys) {
    for (const KEYS &cheat : keys) {
        if (!cheat.empty())
            return true;
    }
    return false;
}

// --- The title's trainers: "trainer N = FILE | build TIMESTAMP ... | keys N: BUTTON + BUTTON; ..." lines (spec/oxos_internal/37 §2), the menu's choice

struct CHOICE {
    std::string File;                   // its file in the trainers folder
    std::vector<ULONG> Builds;          // the game builds it is made for (XBE timestamps); empty: any build, or not known
    std::vector<KEYS> Keys;             // its cheats' in-game keys, by cheat number - 1 (shorter than its cheats when the last ones have none)
    bool Loaded = false, Readable = false;
    TRAINER Trainer;                    // once loaded: its name, author and cheats
    std::vector<bool> On;               // its switches as set in the menu
};

// The number N of a line "trainer N", or 0.
unsigned TrainerLineNumber(const std::string &key) {
    if (key.size() < 9 || SDL_strncasecmp(key.c_str(), "trainer", 7) != 0 || !IsBlank(key[7]))
        return 0;
    char *end;
    unsigned long number = std::strtoul(key.c_str() + 8, &end, 10);
    return *end == '\0' ? static_cast<unsigned>(number) : 0;
}

// The section's trainers in the order of their numbers; current (trainer =) among them, first when the lines leave it out.
std::vector<CHOICE> ReadChoices(const std::string &current) {
    std::vector<std::pair<unsigned, CHOICE>> numbered;
    for (const KEY_LINE &line : SectionKeys) {
        unsigned number = TrainerLineNumber(line.Key);
        std::vector<std::string> fields = SplitText(line.Value, '|');
        if (!number || fields.empty())
            continue;
        CHOICE choice;
        choice.File = fields[0];
        for (std::size_t i = 1; i < fields.size(); i++) {
            if (SDL_strncasecmp(fields[i].c_str(), "build ", 6) == 0) {
                for (const std::string &build : SplitText(fields[i].substr(6), ' ')) {
                    char *end;
                    ULONG timestamp = std::strtoul(build.c_str(), &end, 16);
                    if (*end == '\0')
                        choice.Builds.push_back(timestamp);
                }
            } else if (SDL_strncasecmp(fields[i].c_str(), "keys ", 5) == 0) {
                for (const std::string &cheat : SplitText(fields[i].substr(5), ';')) {
                    std::size_t colon = cheat.find(':');
                    unsigned long number = std::strtoul(cheat.c_str(), nullptr, 10);
                    if (colon == std::string::npos || number < 1 || number > ETM_MAX_CHEATS)
                        continue;
                    if (choice.Keys.size() < number)
                        choice.Keys.resize(number);
                    choice.Keys[number - 1] = ReadCombinations(cheat.substr(colon + 1));
                }
            }
        }
        numbered.push_back({number, choice});
    }
    std::stable_sort(numbered.begin(), numbered.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });
    std::vector<CHOICE> choices;
    for (auto &entry : numbered)
        choices.push_back(entry.second);
    bool listed = std::any_of(choices.begin(), choices.end(), [&](const CHOICE &choice) {
        return SameText(choice.File, current.c_str());
    });
    if (!listed) {
        CHOICE own;
        own.File = current;
        choices.insert(choices.begin(), own);
    }
    return choices;
}

// --- Writing the section back

struct EDIT {
    std::size_t At, Bytes; // the bytes of IniText replaced
    std::string Text;      // by this
};

// Text with the edits made, in the order of their place in it (edits at the same place in the order given).
std::string MakeEdits(const std::string &text, std::vector<EDIT> &edits) {
    std::stable_sort(edits.begin(), edits.end(), [](const EDIT &a, const EDIT &b) {
        return a.At < b.At;
    });
    std::string edited;
    std::size_t copied = 0;
    for (const EDIT &edit : edits) {
        edited.append(text, copied, edit.At - copied);
        edited += edit.Text;
        copied = edit.At + edit.Bytes;
    }
    edited.append(text, copied, std::string::npos);
    return edited;
}

// IniText with the edits made, written to the ini.
bool WriteEdits(std::vector<EDIT> &edits) {
    return WriteWholeFile(INI_PATH, MakeEdits(IniText, edits));
}

// A cheat's name as a key: no '=', no leading '[', ';' or '#' (they would end the key or make the line a section or a comment).
std::string MakeKeyText(const std::string &name) {
    std::string key = name;
    std::replace(key.begin(), key.end(), '=', '-');
    std::size_t start = key.find_first_not_of("[;# ");
    key = start == std::string::npos ? "" : TrimText(key.substr(start));
    return key.empty() ? "(no name)" : key;
}

// The section with the trainer's and each cheat's switch, written back: only the values change; a missing line is added at the section's end.
bool WriteSection(bool enabled, const std::vector<bool> &on, const TRAINER &trainer) {
    std::vector<EDIT> edits;
    auto set = [&](const KEY_LINE *line, const std::string &newLine, const char *value) {
        if (line)
            edits.push_back({line->ValueStart, line->ValueBytes, value});
        else
            edits.push_back({SectionEnd, 0, newLine + value + LineEnd});
    };
    set(FindKey("trainer_enabled"), "trainer_enabled = ", enabled ? "yes" : "no");
    for (std::size_t i = 0; i < on.size(); i++) {
        std::string key = std::to_string(i + 1) + ". " + MakeKeyText(trainer.Cheats[i]) + " = ";
        set(FindCheat(i + 1), key, on[i] ? "yes" : "no");
    }
    return WriteEdits(edits);
}

// The section for another of its trainers: trainer =, trainer_enabled and the cheat and keys lines replaced by the new trainer's, where the first of them was;
// its other lines (title, the trainer lines) stay as they are.
bool WriteOtherTrainer(bool enabled, const CHOICE &choice, const std::vector<bool> &on, const std::vector<KEYS> &keys) {
    std::string block = "trainer = " + choice.File + LineEnd + "trainer_enabled = " + (enabled ? "yes" : "no") + LineEnd;
    for (std::size_t i = 0; i < on.size(); i++) {
        block += std::to_string(i + 1) + ". " + MakeKeyText(choice.Trainer.Cheats[i]) + " = " + (on[i] ? "yes" : "no") + LineEnd;
        if (i >= keys.size() || keys[i].empty())
            continue;
        block += std::to_string(i + 1) + " keys = ";
        for (std::size_t c = 0; c < keys[i].size(); c++) {
            for (std::size_t b = 0; b < keys[i][c].size(); b++)
                block += (b ? " + " : c ? ", " : "") + keys[i][c][b];
        }
        block += LineEnd;
    }
    std::vector<EDIT> edits;
    for (const KEY_LINE &line : SectionKeys) {
        if (SameText(line.Key, "trainer") || SameText(line.Key, "trainer_enabled") || CheatNumber(line.Key) || KeysCheatNumber(line.Key)) {
            edits.push_back({line.LineStart, line.LineEnd - line.LineStart, edits.empty() ? block : ""});
        }
    }
    if (edits.empty())
        edits.push_back({SectionEnd, 0, block});
    return WriteEdits(edits);
}

// --- New trainers: the trainers folder against the ini, first thing at every start of the menu (spec/oxos_internal/37 §7.3)
//
// A first pass lists the folder's file names and the names the ini knows (trainer =, trainer N =, the [unusable trainers] section); only the files it does not
// know are read. Each is added to the section of each title ID it lists, as that title's next "trainer N" (the section's own trainer becoming trainer 1), or
// gets a new section; a file OXOS cannot use as a trainer gets a line in [unusable trainers], so it is not read again. The ini is written once, when something
// was added, before the game's section is read: a trainer copied in for this game can be chosen at once. With more than NEW_TRAINERS_SHOWN new files the
// menu shows a refresh screen meanwhile (ShowRefresh). How long each part took is written to C:\OXOS\log\oxos_trainers_scan.log when the extension's
// section of oxos_extensions.ini says logging_enabled = yes.

constexpr char TRAINERS_FOLDER_NAME[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\trainers";
constexpr char SCAN_LOG_PATH[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\log\\oxos_trainers_scan.log";
constexpr char UNUSABLE_SECTION[] = "unusable trainers";
constexpr std::size_t NEW_TRAINERS_SHOWN = 50; // more new trainers than this: the refresh screen while they are read

constexpr char EXTENSIONS_INI_PATH[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\oxos_extensions.ini";

// The menu may write its log file: the extension's section of oxos_extensions.ini, [oxos_trainers], says logging_enabled = yes (spec 34 §5.1, the
// same switch the kernel and the SDK apply to the extension's oxos_log lines).
bool IsLoggingEnabled() {
    std::string ini;
    if (!ReadWholeFile(EXTENSIONS_INI_PATH, ini))
        return false;
    bool inside = false;
    for (std::size_t line = 0; line < ini.size();) {
        std::size_t end = ini.find('\n', line);
        end = end == std::string::npos ? ini.size() : end + 1;
        std::string text = ini.substr(line, end - line);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
            text.pop_back();
        text = TrimText(text);
        std::size_t equals = text.find('=');
        if (!text.empty() && text[0] == '[')
            inside = SameText(text, "[oxos_trainers]");
        else if (inside && equals != std::string::npos && SameText(TrimText(text.substr(0, equals)), "logging_enabled"))
            return SameText(TrimText(text.substr(equals + 1)), "yes");
        line = end;
    }
    return false;
}

// The names of the .etm files in C:\OXOS\trainers.
std::vector<std::string> ListTrainerFiles() {
    std::vector<std::string> names;
    ANSI_STRING name;
    RtlInitAnsiString(&name, TRAINERS_FOLDER_NAME);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    IO_STATUS_BLOCK iosb;
    HANDLE folder;
    if (!NT_SUCCESS(NtOpenFile(&folder, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &iosb, FILE_SHARE_READ,
                               FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)))
        return names;
    alignas(8) static unsigned char buffer[16384];
    for (bool first = true;; first = false) {
        if (!NT_SUCCESS(NtQueryDirectoryFile(folder, nullptr, nullptr, nullptr, &iosb, buffer, sizeof(buffer), FileDirectoryInformation, nullptr, first)))
            break; // STATUS_NO_MORE_FILES at the end
        for (ULONG offset = 0;;) {
            auto entry = reinterpret_cast<FILE_DIRECTORY_INFORMATION *>(buffer + offset);
            std::string file(entry->FileName, entry->FileNameLength);
            if (!(entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && file.size() > 4 && SameText(file.substr(file.size() - 4), ".etm"))
                names.push_back(file);
            if (!entry->NextEntryOffset)
                break;
            offset += entry->NextEntryOffset;
        }
    }
    NtClose(folder);
    return names;
}

std::string MakeLowerCase(std::string text) {
    for (char &c : text)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return text;
}

// The game builds a trainer checks for (the XBE timestamp at 0x10114, compared in its code): "4158CC50", several separated by spaces; "hidden" when it reads
// 0x10114 but its compare is not plain code (a checksum), "unknown" when its code does not read 0x10114 as such (no check, or an encoded body). The menu
// takes both for any build; the pack's index reads them properly.
std::string ReadTrainerBuilds(const std::string &data) {
    static const unsigned char TIMESTAMP[4] = {0x14, 0x01, 0x01, 0x00};
    auto byteAt = [&](std::size_t at) {
        return at < data.size() ? static_cast<unsigned char>(data[at]) : 0;
    };
    std::vector<ULONG> builds;
    bool reads = false;
    for (std::size_t at = 1; at + 4 <= data.size(); at++) {
        if (std::memcmp(data.data() + at, TIMESTAMP, 4) != 0)
            continue;
        reads = true;
        unsigned char op = byteAt(at - 1), before = at >= 2 ? byteAt(at - 2) : 0;
        std::size_t immediate = 0;
        if (op == 0x3D && before == 0x81) {
            immediate = at + 4; // cmp dword [0x10114], imm32
        } else if (op == 0xA1 || (before == 0x8B && (op & 0xC7) == 0x05)) {
            for (std::size_t next = at + 4; next < at + 16 && !immediate; next++) { // mov reg, [0x10114] ... cmp reg, imm32
                if (byteAt(next) == 0x3D || (byteAt(next) == 0x81 && byteAt(next + 1) >= 0xF8))
                    immediate = byteAt(next) == 0x3D ? next + 1 : next + 2;
            }
        } else if (op >= 0xB8 && op <= 0xBF && byteAt(at + 4) == 0x81 && byteAt(at + 5) >= 0x38 && byteAt(at + 5) <= 0x3F) {
            immediate = at + 6; // mov reg, 0x10114; cmp dword [reg], imm32
        }
        ULONG build = immediate ? ReadDword(data, immediate) : 0;
        if (build >= 0x30000000 && build <= 0x48000000 && std::find(builds.begin(), builds.end(), build) == builds.end())
            builds.push_back(build);
    }
    if (builds.empty())
        return reads ? "hidden" : "unknown";
    std::sort(builds.begin(), builds.end());
    std::string text;
    for (ULONG build : builds) {
        char hex[12];
        std::snprintf(hex, sizeof(hex), "%s%08lX", text.empty() ? "" : " ", build);
        text += hex;
    }
    return text;
}

// A title ID a trainer lists: two capital letters (the publisher) and a number. The list often has no terminating 0, so what follows the last ID is other data.
bool IsTitleId(ULONG id) {
    ULONG a = id >> 24, b = (id >> 16) & 0xFF;
    return a >= 'A' && a <= 'Z' && b >= 'A' && b <= 'Z';
}

// One section of the ini as the scan sees it.
struct SCANNED_SECTION {
    std::size_t End = 0;         // after its last key line: where lines are added
    std::string Trainer;         // its trainer =
    unsigned LastNumber = 0;     // its highest "trainer N"
};

struct SCAN {
    std::string Ini;                                   // the ini as read
    std::string LineEnd = "\r\n";                      // as the file has them
    std::vector<std::pair<std::string, SCANNED_SECTION>> Sections; // by name, lower case
    std::vector<std::string> Known;                    // the trainer file names the ini knows, lower case
    std::vector<EDIT> Edits;                           // the lines added
    std::size_t Unusable = 0;                          // of the new files, those [unusable trainers] got
};

// What the last scan found, for the menu started on its own (DrawRefreshDone).
struct SCAN_RESULT {
    std::size_t Files = 0;    // trainer files in the folder
    std::size_t Added = 0;    // new ones added to the ini as trainers
    std::size_t Unusable = 0; // new ones OXOS cannot use
    bool Written = true;      // the ini was written, or did not need to be
};

SCAN_RESULT ScanResult;

SCANNED_SECTION *FindScannedSection(SCAN &scan, const std::string &name) {
    for (auto &section : scan.Sections) {
        if (section.first == name)
            return &section.second;
    }
    return nullptr;
}

// The ini's sections and the trainer file names it knows. Only section lines, "trainer..." lines and the [unusable trainers] section's lines are taken
// apart; every other line only moves its section's end (the console reads the 245 KB ini at every start of the menu).
void ReadScannedIni(SCAN &scan) {
    if (scan.Ini.find("\r\n") == std::string::npos && !scan.Ini.empty())
        scan.LineEnd = "\n";
    const char *text = scan.Ini.data();
    std::size_t size = scan.Ini.size();
    SCANNED_SECTION *section = nullptr;
    bool unusable = false;
    for (std::size_t line = 0; line < size;) {
        const char *newline = static_cast<const char *>(std::memchr(text + line, '\n', size - line));
        std::size_t end = newline ? newline - text + 1 : size, start = line;
        while (start < end && IsBlank(text[start]))
            start++;
        char first = start < end ? text[start] : '\n';
        bool trainerLine = (first == 't' || first == 'T') && end - start > 7 && SDL_strncasecmp(text + start, "trainer", 7) == 0;
        if (first == '[') {
            const char *close = static_cast<const char *>(std::memchr(text + start, ']', end - start));
            if (close) {
                scan.Sections.push_back({MakeLowerCase(std::string(text + start + 1, close)), SCANNED_SECTION{end, "", 0}});
                section = &scan.Sections.back().second;
                unusable = scan.Sections.back().first == UNUSABLE_SECTION;
            }
        } else if (section && first != ';' && first != '#' && first != '\r' && first != '\n') {
            section->End = end;
            if (trainerLine || unusable) {
                std::string lineText(text + start, end - start);
                std::size_t equals = lineText.find('=');
                if (equals != std::string::npos) {
                    std::string key = TrimText(lineText.substr(0, equals)), value = lineText.substr(equals + 1);
                    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
                        value.pop_back();
                    value = TrimText(value);
                    unsigned number = TrainerLineNumber(key);
                    if (unusable)
                        scan.Known.push_back(MakeLowerCase(key));
                    if (SameText(key, "trainer"))
                        section->Trainer = value;
                    section->LastNumber = std::max(section->LastNumber, number);
                    if (!unusable && (number || SameText(key, "trainer")))
                        scan.Known.push_back(MakeLowerCase(TrimText(value.substr(0, value.find('|')))));
                }
            }
        }
        line = end;
    }
}

// A trainer line for file: "trainer N = FILE | build ...".
std::string MakeTrainerLine(SCAN &scan, unsigned number, const std::string &file, const std::string &builds) {
    return "trainer " + std::to_string(number) + " = " + file + " | build " + builds + scan.LineEnd;
}

// A new file, read and added to the ini (to its titles' sections, a new section, or [unusable trainers]).
void AddTrainer(SCAN &scan, const std::string &file) {
    std::string data;
    TRAINER trainer;
    std::vector<ULONG> ids;
    bool read = ReadWholeFile((std::string(TRAINERS_FOLDER) + file).c_str(), data) && ParseTrainer(data, trainer);
    for (int i = 0; read && i < 3; i++) {
        ULONG id = ReadDword(data, ReadDword(data, ETM_TITLE_IDS) + 4 * i);
        if (IsTitleId(id))
            ids.push_back(id);
    }
    const char *unusable = nullptr;
    if (!read)
        unusable = "not a trainer file";
    else if (ReadDword(data, ETM_HELPER_ENTRY))
        unusable = "needs a dashboard's helper routines";
    else if (ids.empty())
        unusable = "no title ID";
    if (unusable) {
        SCANNED_SECTION *section = FindScannedSection(scan, UNUSABLE_SECTION);
        if (!section) {
            scan.Edits.push_back({scan.Ini.size(), 0, scan.LineEnd + "[" + UNUSABLE_SECTION + "]" + scan.LineEnd});
            scan.Sections.push_back({UNUSABLE_SECTION, SCANNED_SECTION{scan.Ini.size(), "", 0}});
            section = &scan.Sections.back().second;
        }
        scan.Edits.push_back({section->End, 0, file + " = " + unusable + scan.LineEnd});
        scan.Unusable++;
        return;
    }
    std::string builds = ReadTrainerBuilds(data);
    for (ULONG id : ids) {
        char name[12];
        std::snprintf(name, sizeof(name), "%08lx", id);
        SCANNED_SECTION *section = FindScannedSection(scan, name);
        if (!section) {
            // A title new to the ini: its section, with this trainer, off, and its cheats as the file has them.
            char header[16];
            std::snprintf(header, sizeof(header), "[%08lX]", id);
            std::string text = scan.LineEnd + "; " + MakeKeyText(trainer.Name) + scan.LineEnd + header + scan.LineEnd;
            text += "title = " + MakeKeyText(trainer.Name) + scan.LineEnd + "trainer = " + file + scan.LineEnd + "trainer_enabled = no" + scan.LineEnd;
            for (std::size_t i = 0; i < trainer.Cheats.size(); i++)
                text += std::to_string(i + 1) + ". " + MakeKeyText(trainer.Cheats[i]) + " = " + (trainer.Defaults[i] ? "yes" : "no") + scan.LineEnd;
            scan.Edits.push_back({scan.Ini.size(), 0, text});
            scan.Sections.push_back({name, SCANNED_SECTION{scan.Ini.size(), file, 0}});
            continue;
        }
        if (section->Trainer.empty()) {
            // A section without a trainer (or written by hand without one): this one becomes its trainer.
            scan.Edits.push_back({section->End, 0, "trainer = " + file + scan.LineEnd});
            section->Trainer = file;
            continue;
        }
        if (!section->LastNumber) {
            // The section's own trainer becomes trainer 1.
            std::string own;
            ReadWholeFile((std::string(TRAINERS_FOLDER) + section->Trainer).c_str(), own);
            scan.Edits.push_back({section->End, 0, MakeTrainerLine(scan, 1, section->Trainer, own.empty() ? "unknown" : ReadTrainerBuilds(own))});
            section->LastNumber = 1;
        }
        section->LastNumber++;
        scan.Edits.push_back({section->End, 0, MakeTrainerLine(scan, section->LastNumber, file, builds)});
    }
}

// The scan: false only when the ini could not be written. With more than NEW_TRAINERS_SHOWN new trainers, showProgress (when given) is called before each
// is read and once at the end, with how many are done of how many.
bool ScanTrainers(void (*showProgress)(std::size_t done, std::size_t total)) {
    ULONGLONG frequency = KeQueryPerformanceFrequency();
    auto now = [&]() {
        return KeQueryPerformanceCounter();
    };
    // Milliseconds with one decimal ("12.3"), without floating point in the formatting.
    auto milliseconds = [&](ULONGLONG from, ULONGLONG to) {
        ULONGLONG tenths = (to - from) * 10000 / frequency;
        char text[24];
        std::snprintf(text, sizeof(text), "%lu.%lu", static_cast<unsigned long>(tenths / 10), static_cast<unsigned long>(tenths % 10));
        return std::string(text);
    };
    ULONGLONG start = now();
    std::vector<std::string> files = ListTrainerFiles();
    ULONGLONG listed = now();
    SCAN scan;
    ReadWholeFile(INI_PATH, scan.Ini);
    ReadScannedIni(scan);
    std::sort(scan.Known.begin(), scan.Known.end());
    std::vector<std::string> added;
    for (const std::string &file : files) {
        if (!std::binary_search(scan.Known.begin(), scan.Known.end(), MakeLowerCase(file)))
            added.push_back(file);
    }
    ULONGLONG compared = now();
    bool showing = showProgress && added.size() > NEW_TRAINERS_SHOWN;
    for (std::size_t i = 0; i < added.size(); i++) {
        if (showing)
            showProgress(i, added.size());
        AddTrainer(scan, added[i]);
    }
    if (showing)
        showProgress(added.size(), added.size());
    ULONGLONG read = now();
    // An ini that was empty or missing gets a first line saying what it is.
    if (scan.Ini.empty() && !scan.Edits.empty()) {
        std::string first = "; C:\\OXOS\\extensions\\oxos_trainers.ini: Universal Trainers, built by the trainer menu from C:\\OXOS\\trainers\\.";
        scan.Edits.insert(scan.Edits.begin(), {0, 0, first + scan.LineEnd});
    }
    bool written = scan.Edits.empty() || WriteWholeFile(INI_PATH, MakeEdits(scan.Ini, scan.Edits));
    ScanResult = {files.size(), added.size() - scan.Unusable, scan.Unusable, written};
    ULONGLONG done = now();
    std::string log = "Trainer scan: " + std::to_string(files.size()) + " files listed in " + milliseconds(start, listed) + " ms; the ini (" +
                      std::to_string(scan.Ini.size() / 1024) + " KB, " + std::to_string(scan.Known.size()) + " names) read and compared in " +
                      milliseconds(listed, compared) + " ms; " + std::to_string(added.size()) + " new read and added in " + milliseconds(compared, read) +
                      " ms; " + (scan.Edits.empty() ? "nothing to write" : written ? "ini written" : "ini NOT written") + " in " + milliseconds(read, done) +
                      " ms. Total " + milliseconds(start, done) + " ms.\r\n";
    if (IsLoggingEnabled())
        WriteWholeFile(SCAN_LOG_PATH, log);
    return written;
}

// --- Drawing: shapes, glows and text on SDL's renderer

SDL_Renderer *Renderer; // sdl_screen's, once it is open

SDL_FColor MakeColour(Uint32 rgb, float alpha = 1.0f) {
    return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
}

SDL_FColor FadeColour(SDL_FColor colour, float alpha) {
    colour.a *= alpha;
    return colour;
}

// The OXOS palette (kernel/splash/oxos_splash.svg).
const SDL_FColor GREEN = MakeColour(0x8BE31F), LIGHT_GREEN = MakeColour(0xC9F58A), PALE_GREEN = MakeColour(0xF4FFE6), DARK_GREEN = MakeColour(0x3C7A0C);
const SDL_FColor DEEP_GREEN = MakeColour(0x0B1F04), METAL = MakeColour(0x5E6B58), TEXT = MakeColour(0xE6F2DE), DIM_TEXT = MakeColour(0x8FA58A);
const SDL_FColor AMBER = MakeColour(0xFFC23D);

float SmoothStep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

void DrawTriangles(const std::vector<SDL_Vertex> &vertices, const std::vector<int> &indexes, SDL_BlendMode blend) {
    SDL_SetRenderDrawBlendMode(Renderer, blend);
    SDL_RenderGeometry(Renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()), indexes.data(), static_cast<int>(indexes.size()));
}

// A rectangle with a colour at each corner (top left, top right, bottom right, bottom left).
void FillCorners(float x, float y, float w, float h, SDL_FColor tl, SDL_FColor tr, SDL_FColor br, SDL_FColor bl, SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
    DrawTriangles({{{x, y}, tl, {}}, {{x + w, y}, tr, {}}, {{x + w, y + h}, br, {}}, {{x, y + h}, bl, {}}}, {0, 1, 2, 0, 2, 3}, blend);
}

void FillDown(float x, float y, float w, float h, SDL_FColor top, SDL_FColor bottom, SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
    FillCorners(x, y, w, h, top, top, bottom, bottom, blend);
}

void FillAcross(float x, float y, float w, float h, SDL_FColor left, SDL_FColor right, SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
    FillCorners(x, y, w, h, left, right, right, left, blend);
}

// The outline of a rectangle with corners of radius r, clockwise from the top left.
std::vector<SDL_FPoint> MakeRoundedOutline(float x, float y, float w, float h, float r) {
    constexpr int STEPS = 5;
    r = std::min({r, w / 2, h / 2});
    const SDL_FPoint centres[4] = {{x + w - r, y + r}, {x + w - r, y + h - r}, {x + r, y + h - r}, {x + r, y + r}};
    std::vector<SDL_FPoint> points;
    for (int corner = 0; corner < 4; corner++) {
        for (int i = 0; i <= STEPS; i++) {
            float angle = (corner - 1 + static_cast<float>(i) / STEPS) * SDL_PI_F / 2;
            points.push_back({centres[corner].x + r * SDL_cosf(angle), centres[corner].y + r * SDL_sinf(angle)});
        }
    }
    return points;
}

// A convex outline filled, a colour at its top and one at its bottom.
void FillOutline(const std::vector<SDL_FPoint> &points, SDL_FColor top, SDL_FColor bottom, SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
    float minY = points[0].y, maxY = points[0].y;
    for (const SDL_FPoint &p : points) {
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indexes;
    for (const SDL_FPoint &p : points) {
        float t = maxY > minY ? (p.y - minY) / (maxY - minY) : 0;
        SDL_FColor colour{top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t, top.b + (bottom.b - top.b) * t, top.a + (bottom.a - top.a) * t};
        vertices.push_back({p, colour, {}});
    }
    for (int i = 1; i + 1 < static_cast<int>(points.size()); i++) {
        indexes.push_back(0);
        indexes.push_back(i);
        indexes.push_back(i + 1);
    }
    DrawTriangles(vertices, indexes, blend);
}

// A band of the given width along a closed outline, inside it.
void StrokeOutline(const std::vector<SDL_FPoint> &outer, const std::vector<SDL_FPoint> &inner, SDL_FColor colour, SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indexes;
    int n = static_cast<int>(outer.size());
    for (int i = 0; i < n; i++) {
        vertices.push_back({outer[i], colour, {}});
        vertices.push_back({inner[i], colour, {}});
    }
    for (int i = 0; i < n; i++) {
        int a = 2 * i, b = 2 * ((i + 1) % n);
        indexes.insert(indexes.end(), {a, a + 1, b, b, a + 1, b + 1});
    }
    DrawTriangles(vertices, indexes, blend);
}

void FillRounded(float x, float y, float w, float h, float r, SDL_FColor top, SDL_FColor bottom, SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
    FillOutline(MakeRoundedOutline(x, y, w, h, r), top, bottom, blend);
}

void StrokeRounded(float x, float y, float w, float h, float r, float width, SDL_FColor colour, SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
    std::vector<SDL_FPoint> inner = MakeRoundedOutline(x + width, y + width, w - 2 * width, h - 2 * width, std::max(0.0f, r - width));
    StrokeOutline(MakeRoundedOutline(x, y, w, h, r), inner, colour, blend);
}

// A soft light around a rounded rectangle: rings fading outwards, added to what is there.
void DrawGlow(float x, float y, float w, float h, float r, SDL_FColor colour, float spread) {
    constexpr int RINGS = 6;
    for (int i = 0; i < RINGS; i++) {
        float d = spread * i / RINGS, step = spread / RINGS;
        float out = d + step;
        StrokeRounded(x - out, y - out, w + 2 * out, h + 2 * out, r + out, step, FadeColour(colour, 1.0f - static_cast<float>(i) / RINGS), SDL_BLENDMODE_ADD);
    }
}

// An arc of a ring from angle a0 to a1 (radians, clockwise from the right).
void StrokeArc(float cx, float cy, float r, float width, float a0, float a1, SDL_FColor colour, SDL_BlendMode blend = SDL_BLENDMODE_ADD) {
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indexes;
    int steps = std::max(2, static_cast<int>((a1 - a0) * r / 6));
    for (int i = 0; i <= steps; i++) {
        float a = a0 + (a1 - a0) * i / steps, c = SDL_cosf(a), s = SDL_sinf(a);
        vertices.push_back({{cx + r * c, cy + r * s}, colour, {}});
        vertices.push_back({{cx + (r - width) * c, cy + (r - width) * s}, colour, {}});
        if (i) {
            int k = 2 * i;
            indexes.insert(indexes.end(), {k - 2, k - 1, k, k, k - 1, k + 1});
        }
    }
    DrawTriangles(vertices, indexes, blend);
}

void FillCircle(float cx, float cy, float r, SDL_FColor colour, SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
    std::vector<SDL_FPoint> points;
    for (int i = 0; i < 24; i++)
        points.push_back({cx + r * SDL_cosf(i * SDL_PI_F / 12), cy + r * SDL_sinf(i * SDL_PI_F / 12)});
    FillOutline(points, colour, colour, blend);
}

// Text in SDL's debug font: 8x8 characters, drawn scale times as large.
float MeasureText(const std::string &text, float scale) {
    return text.size() * 8 * scale;
}

void DrawText(float x, float y, float scale, SDL_FColor colour, const std::string &text) {
    // The render scale scales the clip rectangle too: it is set again in the scaled coordinates, and put back.
    SDL_Rect clip;
    bool clipped = SDL_RenderClipEnabled(Renderer) && SDL_GetRenderClipRect(Renderer, &clip);
    SDL_SetRenderScale(Renderer, scale, scale);
    if (clipped) {
        SDL_Rect scaled{static_cast<int>(clip.x / scale), static_cast<int>(clip.y / scale), static_cast<int>(clip.w / scale), static_cast<int>(clip.h / scale)};
        SDL_SetRenderClipRect(Renderer, &scaled);
    }
    SDL_SetRenderDrawColorFloat(Renderer, colour.r, colour.g, colour.b, colour.a);
    SDL_RenderDebugText(Renderer, x / scale, y / scale, text.c_str());
    SDL_SetRenderScale(Renderer, 1, 1);
    if (clipped)
        SDL_SetRenderClipRect(Renderer, &clip);
}

// Text with a dark shadow under it, readable over anything.
void DrawShadowedText(float x, float y, float scale, SDL_FColor colour, const std::string &text) {
    DrawText(x + scale, y + scale, scale, MakeColour(0x000000, 0.8f * colour.a), text);
    DrawText(x, y, scale, colour, text);
}

// Text with a green halo: the text drawn faintly around itself, then over it.
void DrawGlowingText(float x, float y, float scale, SDL_FColor colour, SDL_FColor halo, const std::string &text) {
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++)
            DrawText(x + dx * scale, y + dy * scale, scale, halo, text);
    }
    DrawText(x, y, scale, colour, text);
}

void DrawCentredText(float y, float scale, SDL_FColor colour, const std::string &text) {
    DrawShadowedText((WIDTH - MeasureText(text, scale)) / 2, y, scale, colour, text);
}

// At most chars characters of text; longer text ends in "..".
std::string ShortenText(const std::string &text, std::size_t chars) {
    return text.size() <= chars ? text : text.substr(0, chars - 2) + "..";
}

// Text broken into lines of at most chars characters, at spaces where it can be.
std::vector<std::string> WrapText(const std::string &text, std::size_t chars) {
    std::vector<std::string> lines;
    std::string rest = text;
    while (rest.size() > chars) {
        std::size_t space = rest.rfind(' ', chars);
        std::size_t cut = space == std::string::npos || space == 0 ? chars : space;
        lines.push_back(rest.substr(0, cut));
        rest = rest.substr(cut + (cut == space ? 1 : 0));
    }
    if (!rest.empty())
        lines.push_back(rest);
    return lines;
}

SDL_Texture *MakeTexture(const PICTURE &picture, SDL_BlendMode blend) {
    SDL_Texture *texture = SDL_CreateTexture(Renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, picture.Width, picture.Height);
    if (texture) {
        SDL_UpdateTexture(texture, nullptr, picture.Pixels.data(), picture.Width * 4);
        SDL_SetTextureBlendMode(texture, blend);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    return texture;
}

// --- The logo: oxos_trainers_logo.svg's two pictures (the X in metal, then green), the top LOGO_HEIGHT rows of the 640x480 scene

constexpr int LOGO_HEIGHT = 104;

SDL_Texture *LogoStart, *LogoEnd; // the logo's two pictures, added to what is under them

void MakeLogo() {
    namespace vr = xk::graphics::vector_renderer;
    unsigned scratchBytes = 0;
    if (vr::Check(OXOS_TRAINERS_LOGO_SCENE, sizeof(OXOS_TRAINERS_LOGO_SCENE), &scratchBytes))
        return;
    std::vector<unsigned> scratch(scratchBytes / 4 + 1), frame(vr::W * vr::H);
    PICTURE picture;
    picture.Width = vr::W;
    picture.Height = LOGO_HEIGHT;
    picture.Pixels.resize(vr::W * LOGO_HEIGHT);
    for (vr::Phase phase : {vr::START, vr::END}) {
        vr::Render(OXOS_TRAINERS_LOGO_SCENE, sizeof(OXOS_TRAINERS_LOGO_SCENE), phase, frame.data(), scratch.data());
        for (std::size_t i = 0; i < picture.Pixels.size(); i++)
            picture.Pixels[i] = frame[i] | 0xFF000000u;
        (phase == vr::START ? LogoStart : LogoEnd) = MakeTexture(picture, SDL_BLENDMODE_ADD);
    }
}

// The logo at the top: the metal X fades in, turns green (as the boot splash does), then breathes.
void DrawLogo(float t) {
    SDL_FRect where{0, 2, static_cast<float>(WIDTH), static_cast<float>(LOGO_HEIGHT)};
    float fadeIn = SmoothStep(t / 0.5f), toGreen = SmoothStep((t - 0.5f) / 1.0f);
    if (LogoStart && toGreen < 1) {
        SDL_SetTextureAlphaModFloat(LogoStart, fadeIn * (1 - toGreen));
        SDL_RenderTexture(Renderer, LogoStart, nullptr, &where);
    }
    if (LogoEnd && toGreen > 0) {
        float breath = t > 1.5f ? 0.12f * (1 + SDL_sinf((t - 1.5f) * 2.2f)) : 0;
        SDL_SetTextureAlphaModFloat(LogoEnd, std::min(1.0f, toGreen + breath));
        SDL_RenderTexture(Renderer, LogoEnd, nullptr, &where);
        if (breath > 0) {
            SDL_SetTextureAlphaModFloat(LogoEnd, breath);
            SDL_RenderTexture(Renderer, LogoEnd, nullptr, &where);
        }
    }
}

// --- The background: a dark green gradient, a perspective grid sliding towards the viewer, rising sparks and scanlines

constexpr int SPARKS = 48;

struct SPARK {
    float X, Y, Speed, Size, Phase;
};

SPARK Sparks[SPARKS]; // where each spark is, moving up

void MakeSparks() {
    for (SPARK &spark : Sparks)
        spark = {SDL_randf() * WIDTH, SDL_randf() * HEIGHT, 8 + SDL_randf() * 22, 1 + SDL_randf() * 1.6f, SDL_randf() * 6.28f};
}

void DrawBackground(float t, float dt) {
    FillDown(0, 0, WIDTH, HEIGHT, MakeColour(0x010402), MakeColour(0x061A0B), SDL_BLENDMODE_NONE);
    // The grid: from the horizon (y 250) to the bottom; lines towards the vanishing point, cross lines closer together far away.
    constexpr float HORIZON = 250, VANISH_X = WIDTH / 2.0f;
    for (int i = -12; i <= 12; i++) {
        float bottomX = VANISH_X + i * 70.0f;
        SDL_SetRenderDrawBlendMode(Renderer, SDL_BLENDMODE_ADD);
        SDL_SetRenderDrawColorFloat(Renderer, GREEN.r, GREEN.g, GREEN.b, 0.07f);
        SDL_RenderLine(Renderer, VANISH_X + i * 6.0f, HORIZON, bottomX, HEIGHT);
    }
    float slide = SDL_fmodf(t * 0.35f, 1.0f);
    for (int i = 0; i < 14; i++) {
        float d = (i + slide) / 14.0f, y = HORIZON + (HEIGHT - HORIZON) * d * d;
        SDL_SetRenderDrawColorFloat(Renderer, GREEN.r, GREEN.g, GREEN.b, 0.10f * d);
        SDL_RenderLine(Renderer, 0, y, WIDTH, y);
    }
    FillDown(0, HORIZON - 30, WIDTH, 60, MakeColour(0x8BE31F, 0), MakeColour(0x8BE31F, 0.05f), SDL_BLENDMODE_ADD);
    for (SPARK &spark : Sparks) {
        spark.Y -= spark.Speed * dt;
        if (spark.Y < -4) {
            spark.Y = HEIGHT + 4;
            spark.X = SDL_randf() * WIDTH;
        }
        float twinkle = 0.35f + 0.35f * SDL_sinf(t * 2.5f + spark.Phase);
        FillCircle(spark.X, spark.Y, spark.Size, FadeColour(LIGHT_GREEN, twinkle * 0.5f), SDL_BLENDMODE_ADD);
    }
}

void DrawScanlines() {
    static std::vector<SDL_FRect> lines;
    if (lines.empty()) {
        for (int y = 0; y < HEIGHT; y += 3)
            lines.push_back({0, static_cast<float>(y), static_cast<float>(WIDTH), 1});
    }
    SDL_SetRenderDrawBlendMode(Renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(Renderer, 0, 0, 0, 0.18f);
    SDL_RenderFillRects(Renderer, lines.data(), static_cast<int>(lines.size()));
    // A vignette: the corners darker.
    FillAcross(0, 0, 60, HEIGHT, MakeColour(0, 0.55f), MakeColour(0, 0));
    FillAcross(WIDTH - 60, 0, 60, HEIGHT, MakeColour(0, 0), MakeColour(0, 0.55f));
}

// --- Controller button glyphs for the hints

void DrawButton(float x, float y, const char *label, SDL_FColor colour) {
    FillCircle(x + 9, y + 9, 10, FadeColour(colour, 0.25f), SDL_BLENDMODE_ADD);
    FillCircle(x + 9, y + 9, 9, colour);
    FillCircle(x + 9, y + 7, 6, MakeColour(0xFFFFFF, 0.25f));
    DrawText(x + 9 - 4 * std::strlen(label), y + 5, 1, MakeColour(0x000000), label);
}

// A small triangle pointing left (direction -1) or right (1), its point at x.
void DrawArrow(float x, float y, int direction, SDL_FColor colour) {
    std::vector<SDL_FPoint> arrow = {{x, y + 5}, {x - 7.0f * direction, y}, {x - 7.0f * direction, y + 10}};
    FillOutline(arrow, colour, colour);
}

// START or BACK: a small grey pill with the button's name. Returns its width.
float DrawPillButton(float x, float y, const char *label) {
    float w = MeasureText(label, 1) + 8;
    FillRounded(x, y + 2, w, 15, 7, MakeColour(0xD8D8D8), MakeColour(0x8A8A8A));
    DrawText(x + 4, y + 6, 1, MakeColour(0x101010), label);
    return w;
}

// --- The menu

constexpr float CARD_X = 22, CARD_Y = 136, CARD_W = 186, CARD_H = 306;
constexpr float LIST_X = 220, LIST_Y = 136, LIST_W = 398, LIST_H = 306;
constexpr float START_Y = LIST_Y + 12, START_H = 42;
constexpr float TRAINER_Y = START_Y + START_H + 12, ROW_H = 26;
constexpr float CHEATS_Y = TRAINER_Y + ROW_H + 26; // the first cheat row
constexpr int MOST_VISIBLE_CHEATS = 7; // one fewer when the trainer has in-game keys: the keys strip takes the last row
constexpr float IMAGE_SIZE = 128;
constexpr float SWITCH_X = 318; // the switches, from the list's inner edge

enum ITEM { START_ITEM = 0, TRAINER_ITEM = 1, FIRST_CHEAT = 2 };

struct MENU {
    bool HaveGame = false, HaveTrainer = false;
    const char *Problem = nullptr; // why there is no trainer to set (the screen says so; A starts the game)
    GAME Game;
    TRAINER Trainer;
    bool Enabled = false;
    std::vector<bool> On;
    std::vector<KEYS> Keys;      // each cheat's in-game keys: combinations of buttons
    bool HaveKeys = false;       // some cheat has them
    std::vector<CHOICE> Choices; // the title's trainers, LEFT and RIGHT switch between them
    int Choice = 0;              // the one shown
    int SavedChoice = 0;         // the one the section names (trainer =)
    bool Changed = false;
    int Selected = START_ITEM;
    float SelectedY = START_Y; // the selection bar, sliding to the selected row
    float Scroll = 0, TargetScroll = 0;
    float SelectedSince = 0; // when the selection last moved (the marquee starts again)
    PICTURE Picture;              // the title image, decoded
    SDL_Texture *Image = nullptr; // and as a texture, once SDL runs
    float StartingAt = -1; // when Start Game (or BACK) was chosen (the screen flashes and fades), or -1
    bool Leaving = false;  // BACK was pressed: the dashboard follows the fade instead of the game, and nothing is saved
    bool Saved = true;
};

MENU Menu; // everything the menu shows, and where it is

// How many cheat rows the list shows.
int CountVisibleCheats() {
    return Menu.HaveKeys ? MOST_VISIBLE_CHEATS - 1 : MOST_VISIBLE_CHEATS;
}

int CountItems() {
    return Menu.HaveTrainer ? FIRST_CHEAT + static_cast<int>(Menu.On.size()) : 1;
}

float FindItemY(int item) {
    if (item == START_ITEM)
        return START_Y;
    if (item == TRAINER_ITEM)
        return TRAINER_Y;
    return CHEATS_Y + (item - FIRST_CHEAT) * ROW_H - Menu.Scroll;
}

// The menu's contents: the game from the launch data page, its section, its trainer, its title image.
void LoadMenu() {
    Menu.HaveGame = ReadLaunchPage();
    if (!Menu.HaveGame) {
        Menu.Problem = "Start a game that has a trainer.";
        return;
    }
    if (!ReadGame(GamePath, Menu.Game)) {
        Menu.Problem = "The game's XBE cannot be read.";
        return;
    }
    if (Menu.Game.Image.empty())
        ReadSavedGamesImage(Menu.Game);
    if (!DecodeTitleImage(Menu.Game.Image, Menu.Picture))
        Menu.Picture = {};
    const KEY_LINE *file = ReadSection(Menu.Game.TitleId) ? FindKey("trainer") : nullptr;
    if (!file || !ReadTrainer(file->Value.c_str(), Menu.Trainer)) {
        Menu.Problem = file ? "Its trainer file cannot be read." : "No trainer for this game.";
        return;
    }
    Menu.Choices = ReadChoices(file->Value);
    for (std::size_t i = 0; i < Menu.Choices.size(); i++) {
        if (SameText(Menu.Choices[i].File, file->Value.c_str()))
            Menu.Choice = Menu.SavedChoice = static_cast<int>(i);
    }
    Menu.HaveTrainer = true;
    const KEY_LINE *enabled = FindKey("trainer_enabled");
    Menu.Enabled = enabled && SameText(enabled->Value, "yes");
    for (std::size_t i = 0; i < Menu.Trainer.Cheats.size(); i++) {
        const KEY_LINE *line = FindCheat(i + 1);
        Menu.On.push_back(line ? SameText(line->Value, "yes") : static_cast<bool>(Menu.Trainer.Defaults[i]));
    }
    // The keys: the section's "N keys" lines, else the trainer line's.
    Menu.Keys = ReadCheatKeys(Menu.Trainer.Cheats.size());
    const CHOICE &choice = Menu.Choices[Menu.Choice];
    if (!HaveAnyKeys(Menu.Keys)) {
        for (std::size_t i = 0; i < Menu.Keys.size() && i < choice.Keys.size(); i++)
            Menu.Keys[i] = choice.Keys[i];
    }
    Menu.HaveKeys = HaveAnyKeys(Menu.Keys);
}

void Select(int item, float now) {
    Menu.Selected = std::clamp(item, 0, CountItems() - 1);
    Menu.SelectedSince = now;
    int cheat = Menu.Selected - FIRST_CHEAT;
    if (cheat >= 0) {
        float top = cheat * ROW_H;
        if (top < Menu.TargetScroll)
            Menu.TargetScroll = top;
        if (top + ROW_H > Menu.TargetScroll + CountVisibleCheats() * ROW_H)
            Menu.TargetScroll = top + ROW_H - CountVisibleCheats() * ROW_H;
    } else {
        Menu.TargetScroll = 0;
    }
}

// The trainer is made for the running game's build (or for any build, or its build is not known).
bool FitsGame(const CHOICE &choice) {
    return choice.Builds.empty() || std::find(choice.Builds.begin(), choice.Builds.end(), Menu.Game.Timestamp) != choice.Builds.end();
}

// The shown trainer can be switched on: it is made for this build of the game. One made for another build stays off: the build is read from the
// trainer's own check, so on this build it would patch nothing, or patch the wrong places and crash the game.
bool CanSwitchOn() {
    return Menu.HaveTrainer && FitsGame(Menu.Choices[Menu.Choice]);
}

// The trainer runs in the game: switched on, and made for this build (Start Game saves it off otherwise).
bool IsTrainerOn() {
    return Menu.Enabled && CanSwitchOn();
}

// The next (step 1) or previous (-1) of the title's trainers, skipping files that cannot be read; each keeps its switches as set while the menu runs, and a
// trainer shown for the first time starts with its own defaults.
void SwitchTrainer(int step, float now) {
    int count = static_cast<int>(Menu.Choices.size());
    if (!Menu.HaveTrainer || count < 2)
        return;
    CHOICE &shown = Menu.Choices[Menu.Choice];
    shown.Loaded = true;
    shown.Readable = true;
    shown.Trainer = Menu.Trainer;
    shown.On = Menu.On;
    shown.Keys = Menu.Keys;
    int next = Menu.Choice;
    bool found = false;
    for (int tries = 1; tries < count && !found; tries++) {
        next = (next + step + count) % count;
        CHOICE &choice = Menu.Choices[next];
        if (!choice.Loaded) {
            choice.Loaded = true;
            choice.Readable = ReadTrainer(choice.File.c_str(), choice.Trainer);
            choice.On = choice.Trainer.Defaults;
            choice.Keys.resize(choice.Trainer.Cheats.size());
        }
        found = choice.Readable;
    }
    if (!found)
        return;
    Menu.Choice = next;
    Menu.Trainer = Menu.Choices[next].Trainer;
    Menu.On = Menu.Choices[next].On;
    Menu.Keys = Menu.Choices[next].Keys;
    Menu.HaveKeys = HaveAnyKeys(Menu.Keys);
    Menu.Changed = true;
    Menu.Scroll = Menu.TargetScroll = 0;
    Select(std::min(Menu.Selected, static_cast<int>(FIRST_CHEAT)), now);
}

void Activate(float now) {
    if (Menu.Selected == START_ITEM) {
        Menu.StartingAt = now;
        Menu.Leaving = false;
        bool on = IsTrainerOn(), changed = Menu.Changed || on != Menu.Enabled;
        if (changed && Menu.Choice != Menu.SavedChoice)
            Menu.Saved = WriteOtherTrainer(on, Menu.Choices[Menu.Choice], Menu.On, Menu.Keys);
        else if (changed)
            Menu.Saved = WriteSection(on, Menu.On, Menu.Trainer);
        Menu.Enabled = on;
        return;
    }
    if (!CanSwitchOn())
        return;
    if (Menu.Selected == TRAINER_ITEM)
        Menu.Enabled = !Menu.Enabled;
    else
        Menu.On[Menu.Selected - FIRST_CHEAT] = !Menu.On[Menu.Selected - FIRST_CHEAT];
    Menu.Changed = true;
}

// A switch: a pill with a knob, green and to the right when on. Slides as it changes (position 0 = off, 1 = on).
void DrawSwitch(float x, float y, float position, float dim) {
    constexpr float W = 44, H = 18;
    SDL_FColor track = FadeColour(position > 0.5f ? DARK_GREEN : MakeColour(0x1E261C), dim);
    FillRounded(x, y, W, H, H / 2, FadeColour(track, 1), FadeColour(position > 0.5f ? GREEN : MakeColour(0x2C3829), dim));
    StrokeRounded(x, y, W, H, H / 2, 1, FadeColour(position > 0.5f ? LIGHT_GREEN : METAL, 0.8f * dim));
    float knobX = x + H / 2 + position * (W - H);
    if (position > 0.5f)
        FillCircle(knobX, y + H / 2, 12, FadeColour(GREEN, 0.25f * dim), SDL_BLENDMODE_ADD);
    FillCircle(knobX, y + H / 2, H / 2 - 2, FadeColour(position > 0.5f ? PALE_GREEN : MakeColour(0x9AA894), dim));
    DrawText(position > 0.5f ? x + 6 : x + W - 14, y + 5, 1, FadeColour(position > 0.5f ? DEEP_GREEN : MakeColour(0x0A0F09), dim), position > 0.5f ? "ON" : "");
}

// The game card: the title image in a turning reticle, the trainer's badge (+N cheats) and its name, author and the title ID.
void DrawCard(float t, float slide) {
    float x = CARD_X - (1 - slide) * 240;
    FillRounded(x, CARD_Y, CARD_W, CARD_H, 10, MakeColour(0x0B1A0E, 0.82f), MakeColour(0x030805, 0.9f));
    StrokeRounded(x, CARD_Y, CARD_W, CARD_H, 10, 1, FadeColour(GREEN, 0.45f));
    float cx = x + CARD_W / 2, cy = CARD_Y + 18 + IMAGE_SIZE / 2;
    // The reticle: an outer ring in four arcs turning one way, an inner one the other way, tick marks.
    float spin = t * 0.6f;
    for (int i = 0; i < 4; i++)
        StrokeArc(cx, cy, 84, 2, spin + i * SDL_PI_F / 2 + 0.25f, spin + (i + 1) * SDL_PI_F / 2 - 0.25f, FadeColour(GREEN, 0.55f));
    for (int i = 0; i < 3; i++)
        StrokeArc(cx, cy, 78, 1, -spin * 1.6f + i * 2.094f, -spin * 1.6f + i * 2.094f + 1.2f, FadeColour(LIGHT_GREEN, 0.3f));
    for (int i = 0; i < 4; i++) {
        float a = i * SDL_PI_F / 2;
        SDL_SetRenderDrawBlendMode(Renderer, SDL_BLENDMODE_ADD);
        SDL_SetRenderDrawColorFloat(Renderer, LIGHT_GREEN.r, LIGHT_GREEN.g, LIGHT_GREEN.b, 0.8f);
        SDL_RenderLine(Renderer, cx + 86 * SDL_cosf(a), cy + 86 * SDL_sinf(a), cx + 92 * SDL_cosf(a), cy + 92 * SDL_sinf(a));
    }
    // The image, popping in, with a glow and corner brackets.
    float pop = SmoothStep((t - 0.3f) / 0.5f), size = IMAGE_SIZE * (0.6f + 0.4f * pop);
    float ix = cx - size / 2, iy = cy - size / 2;
    DrawGlow(ix, iy, size, size, 4, FadeColour(GREEN, 0.18f * pop * (0.8f + 0.2f * SDL_sinf(t * 3))), 14);
    FillRounded(ix - 2, iy - 2, size + 4, size + 4, 4, MakeColour(0x000000, pop), MakeColour(0x000000, pop));
    if (Menu.Image) {
        SDL_SetTextureAlphaModFloat(Menu.Image, pop);
        SDL_FRect where{ix, iy, size, size};
        SDL_RenderTexture(Renderer, Menu.Image, nullptr, &where);
    } else {
        FillDown(ix, iy, size, size, MakeColour(0x14301A, pop), MakeColour(0x06100A, pop));
        DrawText(cx - 12, cy - 12, 3, FadeColour(GREEN, pop), "?");
    }
    float b = 6 + 2 * SDL_sinf(t * 2.5f), arm = 16;
    SDL_SetRenderDrawBlendMode(Renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(Renderer, LIGHT_GREEN.r, LIGHT_GREEN.g, LIGHT_GREEN.b, pop);
    const float corners[4][4] = {{ix - b, iy - b, 1, 1}, {ix + size + b, iy - b, -1, 1}, {ix - b, iy + size + b, 1, -1},
                                 {ix + size + b, iy + size + b, -1, -1}}; // x, y, and which way the arms go
    for (const auto &c : corners) {
        SDL_FRect h{std::min(c[0], c[0] + c[2] * arm), c[1] - (c[3] < 0 ? 2 : 0), arm, 2};
        SDL_FRect v{c[0] - (c[2] < 0 ? 2 : 0), std::min(c[1], c[1] + c[3] * arm), 2, arm};
        SDL_RenderFillRect(Renderer, &h);
        SDL_RenderFillRect(Renderer, &v);
    }
    // The badge: a hexagon with +N, the classic name of a trainer with N cheats.
    float by = CARD_Y + IMAGE_SIZE + 52;
    if (Menu.HaveTrainer) {
        char count[8];
        std::snprintf(count, sizeof(count), "+%u", static_cast<unsigned>(Menu.On.size()));
        float w = std::max(64.0f, MeasureText(count, 3) + 30), hx = cx - w / 2;
        std::vector<SDL_FPoint> hexagon = {{hx + 12, by}, {hx + w - 12, by}, {hx + w, by + 17}, {hx + w - 12, by + 34}, {hx + 12, by + 34}, {hx, by + 17}};
        bool on = IsTrainerOn();
        FillOutline(hexagon, FadeColour(on ? GREEN : METAL, 0.9f), FadeColour(on ? DARK_GREEN : MakeColour(0x2A3326), 0.9f));
        if (on)
            DrawGlow(hx + 6, by + 2, w - 12, 30, 8, FadeColour(GREEN, 0.12f), 10);
        DrawShadowedText(cx - MeasureText(count, 3) / 2, by + 6, 3, on ? PALE_GREEN : MakeColour(0xC9D3C2), count);
        DrawText(cx - MeasureText("CHEATS", 1) / 2, by + 40, 1, DIM_TEXT, "CHEATS");
        float ty = by + 58;
        for (const std::string &line : WrapText(Menu.Trainer.Name, 21)) {
            if (ty > CARD_Y + CARD_H - 40)
                break;
            DrawText(cx - MeasureText(line, 1) / 2, ty, 1, TEXT, line);
            ty += 11;
        }
        ty += 2;
        for (const std::string &line : WrapText(Menu.Trainer.Author, 21)) {
            if (ty > CARD_Y + CARD_H - 30)
                break;
            DrawText(cx - MeasureText(line, 1) / 2, ty, 1, DIM_TEXT, line);
            ty += 11;
        }
    }
    if (Menu.Game.TitleId) {
        char id[24];
        std::snprintf(id, sizeof(id), "TITLE ID %08lX", Menu.Game.TitleId);
        DrawText(cx - MeasureText(id, 1) / 2, CARD_Y + CARD_H - 16, 1, FadeColour(GREEN, 0.7f), id);
    }
}

// How far text extra pixels too wide for its room is scrolled, since seconds after it was selected: it waits, scrolls to the end at 40 pixels a second,
// waits, and scrolls back.
float FindMarqueeOffset(float extra, float since) {
    float run = extra / 40.0f, time = SDL_fmodf(since, run * 2 + 3.0f);
    return time < 1 ? 0 : time < 1 + run ? (time - 1) * 40 : time < 2 + run ? extra : time < 2 + 2 * run ? extra - (time - 2 - run) * 40 : 0;
}

// One cheat's row: its number, its name (scrolling when the selected one is too long), a KEY tag for an in-game-key cheat, its switch.
void DrawCheat(int cheat, float x, float y, float t, float dim) {
    const std::string &full = Menu.Trainer.Cheats[cheat];
    std::string name = full;
    bool key = !Menu.Keys[cheat].empty();
    std::size_t tag = name.find("(IGKey)");
    if (tag != std::string::npos) {
        name.erase(tag, 7);
        key = true;
    }
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    bool selected = Menu.Selected == FIRST_CHEAT + cheat;
    char number[4];
    std::snprintf(number, sizeof(number), "%02d", cheat + 1);
    DrawText(x, y + 9, 1, FadeColour(selected ? LIGHT_GREEN : METAL, dim), number);
    constexpr std::size_t CHARS = 16;
    float nameX = x + 22, nameW = CHARS * 16;
    if (name.size() > CHARS && selected) {
        // Too long for its room: it scrolls.
        float offset = FindMarqueeOffset(MeasureText(name, 2) - nameW, t - Menu.SelectedSince);
        SDL_Rect clip{static_cast<int>(nameX), static_cast<int>(y), static_cast<int>(nameW), static_cast<int>(ROW_H)};
        SDL_SetRenderClipRect(Renderer, &clip);
        DrawShadowedText(nameX - offset, y + 5, 2, FadeColour(TEXT, dim), name);
        SDL_SetRenderClipRect(Renderer, nullptr);
    } else {
        DrawShadowedText(nameX, y + 5, 2, FadeColour(selected ? PALE_GREEN : TEXT, dim), ShortenText(name, CHARS));
    }
    if (key) {
        FillRounded(x + SWITCH_X - 44, y + 6, 28, 13, 3, FadeColour(AMBER, 0.85f * dim), FadeColour(MakeColour(0xB07A10), 0.85f * dim));
        DrawText(x + SWITCH_X - 42, y + 9, 1, FadeColour(MakeColour(0x201000), dim), "KEY");
    }
    DrawSwitch(x + SWITCH_X - 12, y + 4, Menu.On[cheat] ? 1.0f : 0.0f, dim);
}

// The keys strip under the cheats: the selected cheat's in-game keys as buttons ("RIGHT STICK + D-PAD DOWN", combinations side by side, scrolling when they
// do not fit), or what the KEY tag means.
void DrawKeys(float x, float w, float t) {
    float y = CHEATS_Y + CountVisibleCheats() * ROW_H + 6;
    FillAcross(x, y - 3, w / 2, 1, FadeColour(AMBER, 0), FadeColour(AMBER, 0.45f));
    FillAcross(x + w / 2, y - 3, w / 2, 1, FadeColour(AMBER, 0.45f), FadeColour(AMBER, 0));
    int cheat = Menu.Selected - FIRST_CHEAT;
    if (cheat < 0 || Menu.Keys[cheat].empty()) {
        constexpr char WHAT_KEY_MEANS[] = "KEY: SWITCHED IN THE GAME WITH BUTTONS";
        DrawText(x + (w - MeasureText(WHAT_KEY_MEANS, 1)) / 2, y + 7, 1, FadeColour(AMBER, 0.55f), WHAT_KEY_MEANS);
        return;
    }
    DrawText(x + 4, y + 7, 1, AMBER, "IN GAME");
    float left = x + 68, room = x + w - left;
    // The width of the buttons, to scroll them when they do not fit.
    const std::vector<std::vector<std::string>> &combinations = Menu.Keys[cheat];
    float width = 0;
    for (std::size_t c = 0; c < combinations.size(); c++) {
        width += c ? MeasureText(" OR ", 1) : 0;
        for (std::size_t b = 0; b < combinations[c].size(); b++)
            width += (b ? 16 : 0) + MeasureText(combinations[c][b], 1) + 10;
    }
    float offset = width > room ? FindMarqueeOffset(width - room, t - Menu.SelectedSince) : 0;
    SDL_Rect clip{static_cast<int>(left), static_cast<int>(y), static_cast<int>(room), 20};
    SDL_SetRenderClipRect(Renderer, &clip);
    float at = left - offset;
    for (std::size_t c = 0; c < combinations.size(); c++) {
        if (c) {
            DrawText(at, y + 7, 1, DIM_TEXT, " OR ");
            at += MeasureText(" OR ", 1);
        }
        for (std::size_t b = 0; b < combinations[c].size(); b++) {
            if (b) {
                DrawText(at + 4, y + 7, 1, AMBER, "+");
                at += 16;
            }
            float pill = MeasureText(combinations[c][b], 1) + 10;
            FillRounded(at, y + 2, pill, 17, 4, FadeColour(AMBER, 0.9f), MakeColour(0xB07A10, 0.9f));
            DrawText(at + 5, y + 7, 1, MakeColour(0x201000), combinations[c][b]);
            at += pill;
        }
    }
    SDL_SetRenderClipRect(Renderer, nullptr);
}

// The trainer is made for another build of the game: a red band across the cheats, rising to the right, "[ VERSION NOT COMPATIBLE ]" along it in white
// (each letter upright, stepped along the band: the debug font does not turn).
void DrawIncompatible(float x, float w) {
    constexpr char TEXT_ACROSS[] = "[ VERSION NOT COMPATIBLE ]";
    constexpr float SCALE = 1.5f, RISE = 60, HALF = 15;
    float middle = CHEATS_Y + CountVisibleCheats() * ROW_H / 2;
    auto bandY = [&](float at) {
        return middle + RISE / 2 - RISE * (at - x) / w;
    };
    std::vector<SDL_FPoint> shadow = {{x, bandY(x) - HALF + 3}, {x + w, bandY(x + w) - HALF + 3}, {x + w, bandY(x + w) + HALF + 3}, {x, bandY(x) + HALF + 3}};
    FillOutline(shadow, MakeColour(0x000000, 0.45f), MakeColour(0x000000, 0.45f));
    std::vector<SDL_FPoint> band = {{x, bandY(x) - HALF}, {x + w, bandY(x + w) - HALF}, {x + w, bandY(x + w) + HALF}, {x, bandY(x) + HALF}};
    FillOutline(band, MakeColour(0xE0342A, 0.92f), MakeColour(0x9A1410, 0.92f));
    float step = 8 * SCALE, start = x + (w - MeasureText(TEXT_ACROSS, SCALE)) / 2;
    for (std::size_t i = 0; TEXT_ACROSS[i]; i++) {
        float at = start + i * step;
        DrawShadowedText(at, bandY(at + step / 2) - 4 * SCALE, SCALE, MakeColour(0xFFFFFF), std::string(1, TEXT_ACROSS[i]));
    }
}

void DrawList(float t, float slide) {
    float x = LIST_X + (1 - slide) * 440;
    FillRounded(x, LIST_Y, LIST_W, LIST_H, 10, MakeColour(0x0B1A0E, 0.82f), MakeColour(0x030805, 0.9f));
    StrokeRounded(x, LIST_Y, LIST_W, LIST_H, 10, 1, FadeColour(GREEN, 0.45f));
    float inner = x + 12, innerW = LIST_W - 24;
    if (!Menu.HaveTrainer)
        DrawText(inner + 8, TRAINER_Y + 20, 1, AMBER, Menu.Problem);
    // The selection bar slides to the selected row.
    float targetY = FindItemY(Menu.Selected);
    Menu.SelectedY += (targetY - Menu.SelectedY) * 0.3f;
    if (Menu.Selected != START_ITEM) {
        float barY = Menu.SelectedY, pulse = 0.75f + 0.25f * SDL_sinf(t * 4);
        FillAcross(inner, barY, innerW, ROW_H, FadeColour(GREEN, 0.38f * pulse), FadeColour(GREEN, 0.04f), SDL_BLENDMODE_ADD);
        FillDown(inner, barY, 3, ROW_H, PALE_GREEN, GREEN);
        float bob = 2 * SDL_sinf(t * 6);
        std::vector<SDL_FPoint> chevron = {{inner - 9 + bob, barY + 7}, {inner - 3 + bob, barY + ROW_H / 2}, {inner - 9 + bob, barY + ROW_H - 7}};
        FillOutline(chevron, LIGHT_GREEN, LIGHT_GREEN);
    }
    // Start Game.
    bool startSelected = Menu.Selected == START_ITEM;
    float pulse = 0.7f + 0.3f * SDL_sinf(t * 3.2f);
    if (startSelected) {
        DrawGlow(inner, START_Y, innerW, START_H, 8, FadeColour(GREEN, 0.28f * pulse), 16);
        FillRounded(inner, START_Y, innerW, START_H, 8, LIGHT_GREEN, DARK_GREEN);
        FillRounded(inner + 2, START_Y + 2, innerW - 4, START_H / 2 - 2, 7, MakeColour(0xFFFFFF, 0.28f), MakeColour(0xFFFFFF, 0.06f));
        // A shine sweeping across the button.
        float sweep = SDL_fmodf(t * 0.6f, 1.6f) * innerW;
        SDL_Rect clip{static_cast<int>(inner), static_cast<int>(START_Y), static_cast<int>(innerW), static_cast<int>(START_H)};
        SDL_SetRenderClipRect(Renderer, &clip);
        FillAcross(inner + sweep - 60, START_Y, 60, START_H, MakeColour(0xFFFFFF, 0), MakeColour(0xFFFFFF, 0.35f), SDL_BLENDMODE_ADD);
        FillAcross(inner + sweep, START_Y, 60, START_H, MakeColour(0xFFFFFF, 0.35f), MakeColour(0xFFFFFF, 0), SDL_BLENDMODE_ADD);
        SDL_SetRenderClipRect(Renderer, nullptr);
    } else {
        FillRounded(inner, START_Y, innerW, START_H, 8, MakeColour(0x14301A), MakeColour(0x0A1A0E));
        StrokeRounded(inner, START_Y, innerW, START_H, 8, 2, FadeColour(GREEN, 0.7f));
    }
    const char *label = Menu.HaveGame ? "START GAME" : "DASHBOARD";
    float labelX = inner + (innerW - MeasureText(label, 3)) / 2 + 14, labelY = START_Y + (START_H - 24) / 2;
    std::vector<SDL_FPoint> play = {{labelX - 30, labelY + 2}, {labelX - 12, labelY + 12}, {labelX - 30, labelY + 22}};
    FillOutline(play, startSelected ? DEEP_GREEN : GREEN, startSelected ? DEEP_GREEN : GREEN);
    if (startSelected)
        DrawText(labelX, labelY + 1, 3, MakeColour(0x061204), label);
    else
        DrawGlowingText(labelX, labelY + 1, 3, LIGHT_GREEN, FadeColour(GREEN, 0.15f), label);
    if (!Menu.HaveTrainer)
        return;
    // The trainer's switch, then the cheats under a rule.
    bool trainerSelected = Menu.Selected == TRAINER_ITEM;
    DrawShadowedText(inner + 12, TRAINER_Y + 5, 2, trainerSelected ? PALE_GREEN : TEXT, "TRAINER");
    bool trainerOn = IsTrainerOn();
    DrawText(inner + 140, TRAINER_Y + 9, 1, trainerOn ? GREEN : DIM_TEXT, trainerOn ? "ON IN THE GAME" : "OFF");
    DrawSwitch(inner + SWITCH_X, TRAINER_Y + 4, trainerOn ? 1.0f : 0.0f, CanSwitchOn() ? 1.0f : 0.45f);
    float ruleY = TRAINER_Y + ROW_H + 10;
    FillAcross(inner, ruleY, innerW / 2, 1, FadeColour(GREEN, 0), FadeColour(GREEN, 0.6f));
    FillAcross(inner + innerW / 2, ruleY, innerW / 2, 1, FadeColour(GREEN, 0.6f), FadeColour(GREEN, 0));
    unsigned on = static_cast<unsigned>(std::count(Menu.On.begin(), Menu.On.end(), true));
    char heading[40];
    std::snprintf(heading, sizeof(heading), " CHEATS  %u OF %u ON ", on, static_cast<unsigned>(Menu.On.size()));
    FillRounded(inner + (innerW - MeasureText(heading, 1)) / 2 - 4, ruleY - 6, MeasureText(heading, 1) + 8, 12, 3, MakeColour(0x07110A), MakeColour(0x07110A));
    DrawText(inner + (innerW - MeasureText(heading, 1)) / 2, ruleY - 3, 1, LIGHT_GREEN, heading);
    Menu.Scroll += (Menu.TargetScroll - Menu.Scroll) * 0.25f;
    SDL_Rect clip{static_cast<int>(x), static_cast<int>(CHEATS_Y), static_cast<int>(LIST_W), static_cast<int>(CountVisibleCheats() * ROW_H)};
    SDL_SetRenderClipRect(Renderer, &clip);
    bool fits = CanSwitchOn();
    float dim = !fits ? 0.25f : Menu.Enabled ? 1.0f : 0.45f;
    for (int i = 0; i < static_cast<int>(Menu.On.size()); i++) {
        float y = FindItemY(FIRST_CHEAT + i);
        if (y > CHEATS_Y - ROW_H && y < CHEATS_Y + CountVisibleCheats() * ROW_H)
            DrawCheat(i, inner + 12, y, t, dim);
    }
    if (!fits)
        DrawIncompatible(inner, innerW);
    SDL_SetRenderClipRect(Renderer, nullptr);
    if (Menu.HaveKeys)
        DrawKeys(inner, innerW, t);
    // The scrollbar, when the cheats do not fit.
    if (static_cast<int>(Menu.On.size()) > CountVisibleCheats()) {
        float trackH = CountVisibleCheats() * ROW_H, total = Menu.On.size() * ROW_H;
        float thumbH = std::max(16.0f, trackH * trackH / total), thumbY = CHEATS_Y + (trackH - thumbH) * Menu.Scroll / (total - trackH);
        FillRounded(x + LIST_W - 8, CHEATS_Y, 4, trackH, 2, MakeColour(0x1E261C), MakeColour(0x1E261C));
        FillRounded(x + LIST_W - 8, thumbY, 4, thumbH, 2, LIGHT_GREEN, GREEN);
    }
}

// The buttons' hints at the bottom, side by side: A (what it does on the selected row; nothing on the rows of a trainer made for another build), START
// (Start Game, while a row other than Start Game is selected: there A does it), LEFT and RIGHT (the trainer, when the title has several), BACK (to the
// dashboard, without the game and without saving).
void DrawHints(float t) {
    float y = HEIGHT - 32, x = 30;
    bool onRow = Menu.Selected != START_ITEM && Menu.HaveTrainer;
    if (!onRow || CanSwitchOn()) {
        const char *label = onRow ? "ON/OFF" : Menu.HaveGame ? "START GAME" : "DASHBOARD";
        DrawButton(x, y, "A", MakeColour(0x47C83E));
        DrawText(x + 24, y + 5, 1, TEXT, label);
        x += 24 + MeasureText(label, 1) + 22;
    }
    if (onRow) {
        const char *label = Menu.HaveGame ? "START GAME" : "DASHBOARD";
        float w = DrawPillButton(x, y, "START");
        DrawText(x + w + 6, y + 5, 1, TEXT, label);
        x += w + 6 + MeasureText(label, 1) + 22;
    }
    if (Menu.HaveTrainer && Menu.Choices.size() > 1) {
        DrawArrow(x + 2, y + 4, -1, LIGHT_GREEN);
        DrawArrow(x + 21, y + 4, 1, LIGHT_GREEN);
        DrawText(x + 28, y + 5, 1, TEXT, "TRAINER");
        x += 28 + MeasureText("TRAINER", 1) + 22;
    }
    if (Menu.HaveGame) {
        float w = DrawPillButton(x, y, "BACK");
        DrawText(x + w + 6, y + 5, 1, TEXT, "EXIT");
    }
    if (Menu.Changed) {
        float blink = 0.6f + 0.4f * SDL_sinf(t * 5), textX = WIDTH - 30 - MeasureText("SAVED AT START", 1);
        FillCircle(textX - 10, y + 9, 4, FadeColour(AMBER, blink));
        DrawText(textX, y + 5, 1, AMBER, "SAVED AT START");
    }
}

// Start Game (or BACK) chosen: the screen flashes, says what starts, and fades to black.
void DrawStarting(float t) {
    float since = t - Menu.StartingAt;
    SDL_FColor flashTop = FadeColour(PALE_GREEN, std::max(0.0f, 0.6f - since * 3)), flashBottom = FadeColour(GREEN, std::max(0.0f, 0.4f - since * 2));
    FillDown(0, 0, WIDTH, HEIGHT, flashTop, flashBottom, SDL_BLENDMODE_ADD);
    FillDown(0, 0, WIDTH, HEIGHT, MakeColour(0, SmoothStep(since / 0.7f)), MakeColour(0, SmoothStep(since / 0.7f)));
    if (Menu.Leaving) {
        DrawCentredText(HEIGHT / 2 - 8, 2, FadeColour(LIGHT_GREEN, SmoothStep(since / 0.2f)), "DASHBOARD");
        return;
    }
    if (!Menu.HaveTrainer)
        return;
    char text[48];
    unsigned on = static_cast<unsigned>(std::count(Menu.On.begin(), Menu.On.end(), true));
    if (Menu.Enabled)
        std::snprintf(text, sizeof(text), "TRAINER ON: %u CHEAT%s", on, on == 1 ? "" : "S");
    else
        std::snprintf(text, sizeof(text), "TRAINER OFF");
    DrawCentredText(HEIGHT / 2 - 8, 2, FadeColour(LIGHT_GREEN, SmoothStep(since / 0.2f)), text);
    if (!Menu.Saved)
        DrawCentredText(HEIGHT / 2 + 20, 1, AMBER, "THE SETTINGS COULD NOT BE SAVED");
}

// The game's name under the logo; for a title with several trainers, "2 OF 8" beside it between arrows (LEFT and RIGHT switch), green when the trainer is
// made for this build of the game, amber when it is made for another (DrawIncompatible crosses its cheats out).
void DrawGameName(float t) {
    float appear = SmoothStep((t - 0.2f) / 0.5f), y = LOGO_HEIGHT + 4;
    bool choosing = Menu.HaveTrainer && Menu.Choices.size() > 1;
    std::string tag;
    bool fits = true;
    if (choosing) {
        fits = FitsGame(Menu.Choices[Menu.Choice]);
        char text[48];
        std::snprintf(text, sizeof(text), "%d OF %d", Menu.Choice + 1, static_cast<int>(Menu.Choices.size()));
        tag = text;
    }
    float tagW = choosing ? MeasureText(tag, 1) + 12 + 2 * 16 : 0; // the tag and an arrow on each side
    std::size_t chars = static_cast<std::size_t>((WIDTH - 40 - tagW - 12) / 16);
    std::string name = ShortenText(Menu.Game.Name, std::min<std::size_t>(36, chars));
    float w = MeasureText(name, 2), x = (WIDTH - w - (choosing ? 12 + tagW : 0)) / 2;
    DrawGlowingText(x, y, 2, FadeColour(PALE_GREEN, appear), FadeColour(GREEN, 0.12f * appear), name);
    if (!choosing)
        return;
    float left = x + w + 12, bob = 1.5f * SDL_sinf(t * 5), pillW = MeasureText(tag, 1) + 12;
    SDL_FColor colour = fits ? GREEN : AMBER;
    DrawArrow(left + 8 - bob, y + 3, -1, FadeColour(colour, appear));
    SDL_FColor top = FadeColour(fits ? LIGHT_GREEN : AMBER, 0.95f * appear), bottom = FadeColour(fits ? DARK_GREEN : MakeColour(0xB07A10), appear);
    FillRounded(left + 16, y, pillW, 16, 4, top, bottom);
    DrawText(left + 22, y + 4, 1, FadeColour(MakeColour(fits ? 0x061204 : 0x201000), appear), tag);
    DrawArrow(left + 16 + pillW + 8 + bob, y + 3, 1, FadeColour(colour, appear));
}

// The refresh screen, while many new trainers are read (more than NEW_TRAINERS_SHOWN): the logo, what is happening and how far it is; drawn about ten
// times a second (and at the end), so drawing costs the scan little.
void ShowRefresh(std::size_t done, std::size_t total) {
    static Uint64 started = 0, shown = 0;
    Uint64 ticks = SDL_GetTicks();
    if (!started)
        started = ticks;
    if (shown && ticks - shown < 100 && done < total)
        return;
    shown = ticks;
    float t = (ticks - started) / 1000.0f;
    sdl_screen::Poll();
    DrawBackground(t, 0.1f);
    DrawLogo(t);
    constexpr char TITLE[] = "TRAINER REFRESH IN PROGRESS";
    float pulse = 0.75f + 0.25f * SDL_sinf(t * 4);
    DrawGlowingText((WIDTH - MeasureText(TITLE, 2)) / 2, 200, 2, PALE_GREEN, FadeColour(GREEN, 0.15f * pulse), TITLE);
    char line[48];
    std::snprintf(line, sizeof(line), "NEW TRAINERS: %u OF %u", static_cast<unsigned>(done), static_cast<unsigned>(total));
    DrawCentredText(236, 1, TEXT, line);
    constexpr float BAR_X = 120, BAR_Y = 260, BAR_W = 400, BAR_H = 14;
    FillRounded(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, MakeColour(0x14301A), MakeColour(0x0A1A0E));
    StrokeRounded(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, 1, FadeColour(GREEN, 0.7f));
    float filled = total ? BAR_W * done / total : 0;
    if (filled >= BAR_H)
        FillRounded(BAR_X, BAR_Y, filled, BAR_H, BAR_H / 2, LIGHT_GREEN, DARK_GREEN);
    DrawCentredText(290, 1, DIM_TEXT, "ONCE: THE NEXT START IS QUICK");
    DrawScanlines();
    SDL_RenderPresent(Renderer);
}

// The menu started on its own (from a dashboard), with no game: what the scan did, and A to leave. This is how an empty ini is built from the trainers
// folder (spec/oxos_internal/37 §7.3).
void DrawRefreshDone(float t) {
    float appear = SmoothStep((t - 0.2f) / 0.5f);
    const char *title = ScanResult.Added || ScanResult.Unusable ? "TRAINER REFRESH COMPLETE" : "TRAINERS UP TO DATE";
    DrawGlowingText((WIDTH - MeasureText(title, 2)) / 2, 190, 2, FadeColour(PALE_GREEN, appear), FadeColour(GREEN, 0.15f * appear), title);
    std::vector<std::string> lines;
    char line[64];
    if (ScanResult.Added) {
        std::snprintf(line, sizeof(line), "%u NEW TRAINERS ADDED", static_cast<unsigned>(ScanResult.Added));
        lines.push_back(line);
    }
    if (ScanResult.Unusable) {
        std::snprintf(line, sizeof(line), "%u FILES ARE NOT USABLE TRAINERS", static_cast<unsigned>(ScanResult.Unusable));
        lines.push_back(line);
    }
    std::snprintf(line, sizeof(line), "%u TRAINERS IN C:\\OXOS\\TRAINERS", static_cast<unsigned>(ScanResult.Files));
    lines.push_back(line);
    if (!ScanResult.Written)
        lines.push_back("THE SETTINGS COULD NOT BE SAVED");
    for (std::size_t i = 0; i < lines.size(); i++)
        DrawCentredText(232 + 20 * i, 1, FadeColour(i + 1 == lines.size() && !ScanResult.Written ? AMBER : TEXT, appear), lines[i]);
    float y = HEIGHT - 32;
    DrawButton(30, y, "A", MakeColour(0x47C83E));
    DrawText(54, y + 5, 1, TEXT, "EXIT");
}

void DrawFrame(float t, float dt) {
    DrawBackground(t, dt);
    DrawLogo(t);
    if (!Menu.HaveGame) {
        DrawRefreshDone(t);
        DrawScanlines();
        if (Menu.StartingAt >= 0)
            DrawStarting(t);
        return;
    }
    if (Menu.HaveGame && !Menu.Game.Name.empty())
        DrawGameName(t);
    float slide = SmoothStep((t - 0.15f) / 0.45f);
    DrawCard(t, slide);
    DrawList(t, slide);
    DrawHints(t);
    DrawScanlines();
    if (Menu.StartingAt >= 0)
        DrawStarting(t);
}

// --- Controllers: A, B, START, the D-pad and the left stick (up and down held: repeats; left and right: the trainer)

float StickX, StickY; // the left stick, -1 (left, up) to 1

void ReadStick(const SDL_Event &event) {
    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX)
        StickX = event.gaxis.value / 32767.0f;
    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)
        StickY = event.gaxis.value / 32767.0f;
}

// -1, 1 or 0: LEFT or RIGHT pressed this frame, on the D-pad or the left stick (once per push).
int ReadSideways() {
    static int stickDirection = 0;
    int stick = StickX < -0.6f ? -1 : StickX > 0.6f ? 1 : 0, pushed = stick != stickDirection ? stick : 0;
    stickDirection = stick;
    if (sdl_screen::Pressed(SDL_GAMEPAD_BUTTON_DPAD_LEFT))
        return -1;
    if (sdl_screen::Pressed(SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
        return 1;
    return pushed;
}

// -1, 1 or 0: a move this frame, repeating while up or down is held (after 0.35 s, then every 0.09 s).
int ReadMove(float now) {
    static float heldSince = -1, lastRepeat = 0;
    static int heldDirection = 0;
    bool up = sdl_screen::Held(SDL_GAMEPAD_BUTTON_DPAD_UP) || StickY < -0.5f, down = sdl_screen::Held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || StickY > 0.5f;
    int direction = up ? -1 : down ? 1 : 0;
    if (direction != heldDirection) {
        heldDirection = direction;
        heldSince = now;
        lastRepeat = now;
        return direction;
    }
    if (direction && now - heldSince > 0.35f && now - lastRepeat > 0.09f) {
        lastRepeat = now;
        return direction;
    }
    return 0;
}

} // namespace

int main() {
    // The screen first, for the refresh screen; then the scan for new trainers, before the game's section is read (spec/oxos_internal/37 §7.3).
    bool screen = sdl_screen::Open(true);
    if (screen) {
        Renderer = sdl_screen::Renderer();
        MakeLogo();
        MakeSparks();
    }
    ScanTrainers(screen ? ShowRefresh : nullptr);
    LoadMenu();
    if (!screen)
        StartGame(Menu.HaveGame); // no screen: the game as it was asked for
    if (!Menu.Picture.Pixels.empty())
        Menu.Image = MakeTexture(Menu.Picture, SDL_BLENDMODE_BLEND);
    Uint64 start = SDL_GetTicks(), last = start;
    for (;;) {
        Uint64 ticks = SDL_GetTicks();
        float t = (ticks - start) / 1000.0f, dt = std::min(0.1f, (ticks - last) / 1000.0f);
        last = ticks;
        sdl_screen::Poll(ReadStick);
        if (Menu.StartingAt < 0) {
            int move = ReadMove(t);
            if (move)
                Select(Menu.Selected + move, t);
            int sideways = ReadSideways();
            if (sideways)
                SwitchTrainer(sideways, t);
            if (sdl_screen::Pressed(SDL_GAMEPAD_BUTTON_SOUTH))
                Activate(t);
            if (sdl_screen::Pressed(SDL_GAMEPAD_BUTTON_EAST))
                Select(START_ITEM, t);
            if (sdl_screen::Pressed(SDL_GAMEPAD_BUTTON_START)) {
                Select(START_ITEM, t);
                Activate(t);
            }
            if (sdl_screen::Pressed(SDL_GAMEPAD_BUTTON_BACK) && Menu.HaveGame) {
                Menu.StartingAt = t;
                Menu.Leaving = true;
            }
        } else if (t - Menu.StartingAt > 0.75f) {
            StartGame(Menu.HaveGame && !Menu.Leaving);
        }
        DrawFrame(t, dt);
        SDL_RenderPresent(Renderer);
    }
}
