// MPEG program streams (shared/video_player/mpeg_program_stream.h).
#include "mpeg_program_stream.h"

#include <string.h>

namespace oxos::video_player {

namespace {

constexpr uint8_t PACK = 0xBA;
constexpr uint8_t SYSTEM_HEADER = 0xBB;
constexpr uint8_t FIRST_AUDIO = 0xC0;
constexpr uint8_t FIRST_VIDEO = 0xE0;
constexpr uint8_t SEQUENCE_HEADER = 0xB3;

bool IsStartCode(const uint8_t *At) {
    return At[0] == 0 && At[1] == 0 && At[2] == 1;
}

// The payload of one PES packet whose bytes after the 6-byte start code and length are [At, End): an MPEG-2 header starts with the bits 10 and gives its own
// length; an MPEG-1 header is stuffing bytes, an optional buffer size and the time stamps.
const uint8_t *FindPayload(const uint8_t *At, const uint8_t *End) {
    if (At < End && (At[0] & 0xC0) == 0x80) {
        return End - At >= 3 ? At + 3 + At[2] : End;
    }
    while (At < End && *At == 0xFF) {
        At++;
    }
    if (At < End && (*At & 0xC0) == 0x40) {
        At += 2;
    }
    if (At >= End) {
        return End;
    }
    if ((*At & 0xF0) == 0x20) {
        return At + 5;
    }
    if ((*At & 0xF0) == 0x30) {
        return At + 10;
    }
    return At + 1;
}

} // namespace

void SplitProgramStream(std::vector<uint8_t> &File, std::vector<uint8_t> &Sound) {
    Sound.clear();
    uint8_t *data = File.data();
    size_t size = File.size();
    if (size >= 4 && IsStartCode(data) && data[3] == SEQUENCE_HEADER) {
        return;
    }
    size_t video = 0; // the video's bytes so far, at the start of File
    size_t at = 0;
    while (at + 6 <= size) {
        if (!IsStartCode(data + at)) {
            at++;
            continue;
        }
        uint8_t id = data[at + 3];
        if (id == PACK) {
            bool mpeg2 = (data[at + 4] & 0xC0) == 0x40;
            at += mpeg2 ? 14 + (at + 13 < size ? data[at + 13] & 7 : 0) : 12;
            continue;
        }
        if (id < SYSTEM_HEADER) {
            at += 4;
            continue;
        }
        size_t length = (size_t)data[at + 4] << 8 | data[at + 5];
        size_t end = at + 6 + length <= size ? at + 6 + length : size;
        if (id == FIRST_VIDEO || id == FIRST_AUDIO) {
            const uint8_t *payload = FindPayload(data + at + 6, data + end);
            size_t count = payload < data + end ? (size_t)(data + end - payload) : 0;
            if (id == FIRST_VIDEO) {
                memmove(data + video, payload, count);
                video += count;
            } else {
                Sound.insert(Sound.end(), payload, payload + count);
            }
        }
        at = end;
    }
    File.resize(video);
}

} // namespace oxos::video_player
